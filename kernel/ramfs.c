/* ramfs.c - a writable, hierarchical in-memory filesystem, and the path
 * resolution every filesystem shares.
 *
 * Milestone 26. Until now the only filesystem was the initrd: a flat
 * archive of read-only files, where a path resolved on its last component
 * alone so that "/lib/i386-linux-gnu/libc.so.6" and "libc.so.6" named the
 * same thing. `open` for writing returned -EROFS and there was no
 * `unlink`, `mkdir` or `rename` at all.
 *
 * Two problems, and they are one problem. Wine does not merely want to
 * write files - it wants a *tree*: a prefix directory it creates, a
 * registry it rewrites, directories it makes and removes. A flat
 * read-only archive is not a filesystem with writing missing, it is a
 * different thing.
 *
 * So the initrd stops being the filesystem and becomes what it actually
 * is: a source of initial contents. The root is now a ramfs, populated
 * from the archive at boot, and every file in it can be written, renamed
 * or removed.
 *
 * The one thing carefully preserved: an initrd-backed file's bytes are
 * *not* copied at boot. `data` points into the initrd image and
 * `from_initrd` says so, so mounting costs nothing beyond the nodes
 * themselves. The copy happens on the first write to that file, and only
 * to that file. `meminfo` before and after a boot is unchanged, which is
 * a property this project has spent several milestones defending.
 *
 * --- Milestone 32 -------------------------------------------------------
 *
 * Two changes, both forced by there now being a *second* filesystem
 * (kernel/fat32.c) mounted inside this one.
 *
 * The first is dispatch. vfs_write(), vfs_create(), vfs_unlink() and
 * friends used to *be* the ramfs implementations; now they look at the
 * node's `ops` and hand over when it has any. A node with no ops is a
 * ramfs node, so every existing caller and every existing node behaves
 * exactly as before.
 *
 * The second is that path resolution stopped walking `first_child`
 * directly and started going through the finddir hook. That is not
 * tidiness: a FAT directory's children do not exist as nodes until
 * something asks for them, and the hook is where "read this directory off
 * the disk" happens. Walking the child list directly would have found an
 * empty list and reported that the file was not there.
 *
 * Symlinks arrived with the same milestone, and resolution is where they
 * live - see walk_from().
 */

#include "vfs.h"
#include "blockdev.h"
#include "kheap.h"
#include "kstring.h"
#include "posix.h"
#include "console.h"
#include "vga_text.h"
#include "rtc.h"

/* Nodes come from the kernel heap in blocks, threaded onto a free list.
 *
 * Milestone 26 used a fixed 384-entry array, for the reason the process
 * table is one: a visible ceiling beats an allocator failure in the
 * middle of a path walk. Milestone 32 had to give that up. A FAT volume
 * carrying a Wine prefix has thousands of files, and a node table sized
 * for an initrd would have replaced "the initrd is too small" with "the
 * node table is too small" - the same wall, one milestone later.
 *
 * The ceiling is still there, it is just much further away, and it is a
 * count rather than an array so an unused filesystem costs nothing. A
 * node is about 250 bytes, so the whole pool at its maximum is about 2MB
 * of a 192MB heap. */
#define VFS_NODES_PER_BLOCK 128
#define VFS_MAX_BLOCKS      64      /* 8192 nodes */

static vfs_node_t* blocks[VFS_MAX_BLOCKS];
static uint32_t nblocks = 0;
static vfs_node_t* free_list = 0;
static vfs_node_t* root = 0;
static uint32_t nodes_used = 0;
static uint32_t heap_bytes = 0;

/* --- the initrd archive, which is now only an input --------------------- */

/* 'STL2' since Milestone 35, when the format grew directories - a name is
 * a path now, and the old 'STLR' images have a 60-byte name field where
 * this one has 124. Different magic so a stale image is refused rather
 * than read as gibberish. */
#define INITRD_MAGIC      0x324C5453u /* 'STL2' */
#define INITRD_MAX_FILES  2048
#define INITRD_NAME_MAX   124

typedef struct {
    uint32_t magic;
    uint32_t nfiles;
} __attribute__((packed)) initrd_header_t;

typedef struct {
    uint32_t magic;
    char name[INITRD_NAME_MAX];
    uint32_t offset;
    uint32_t length;
} __attribute__((packed)) initrd_file_header_t;

/* --- node bookkeeping ---------------------------------------------------- */

static int grow_pool(void) {
    if (nblocks >= VFS_MAX_BLOCKS) return 0;
    vfs_node_t* blk =
        (vfs_node_t*)kmalloc(sizeof(vfs_node_t) * VFS_NODES_PER_BLOCK);
    if (!blk) return 0;
    kmemset(blk, 0, sizeof(vfs_node_t) * VFS_NODES_PER_BLOCK);
    for (uint32_t i = 0; i < VFS_NODES_PER_BLOCK; i++) {
        blk[i].next_sibling = free_list;
        free_list = &blk[i];
    }
    blocks[nblocks++] = blk;
    return 1;
}

vfs_node_t* vfs_node_alloc(void) {
    if (!free_list && !grow_pool()) return 0;
    vfs_node_t* n = free_list;
    free_list = n->next_sibling;
    kmemset(n, 0, sizeof(*n));
    n->in_use = 1;
    nodes_used++;
    return n;
}

/* Gives a node back. The driver that owned it gets told first, so it can
 * drop whatever it hung off the node, and the node's own storage goes
 * whether or not it had one. */
void vfs_node_release(vfs_node_t* n) {
    if (!n || !n->in_use) return;
    if (n->ops && n->ops->forget) n->ops->forget(n);
    if (n->data && !n->from_initrd) {
        heap_bytes -= n->capacity;
        kfree(n->data_base ? n->data_base : n->data);
    }
    kmemset(n, 0, sizeof(*n));
    n->next_sibling = free_list;
    free_list = n;
    nodes_used--;
}

void vfs_node_ref(vfs_node_t* node) {
    if (node) node->refs++;
}

void vfs_node_unref(vfs_node_t* node) {
    if (!node || node->refs <= 0) return;
    if (--node->refs == 0 && node->unlinked) vfs_node_release(node);
}

void vfs_dir_append(vfs_node_t* dir, vfs_node_t* child) {
    if (!dir || !child) return;
    child->parent = dir;
    child->next_sibling = 0;
    /* Appended, not prepended: readdir order is creation order, which is
     * what makes `ls` stable across boots and a directory listing
     * comparable between two runs. */
    vfs_node_t** tail = &dir->first_child;
    while (*tail) tail = &(*tail)->next_sibling;
    *tail = child;
}

int vfs_dir_remove(vfs_node_t* dir, vfs_node_t* child) {
    if (!dir || !child) return 0;
    vfs_node_t** link = &dir->first_child;
    while (*link && *link != child) link = &(*link)->next_sibling;
    if (!*link) return 0;
    *link = child->next_sibling;
    child->next_sibling = 0;
    child->parent = 0;
    return 1;
}

static int name_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* --- the vfs_node_t hooks ------------------------------------------------ */

static uint32_t ramfs_read(vfs_node_t* node, uint32_t offset, uint32_t size,
                           uint8_t* buffer) {
    if (!node->data || offset >= node->length) return 0;
    if (offset + size > node->length) size = node->length - offset;
    kmemcpy(buffer, node->data + offset, size);
    return size;
}

static vfs_node_t* ramfs_readdir(vfs_node_t* node, uint32_t index) {
    vfs_node_t* c = node->first_child;
    while (c && index) { c = c->next_sibling; index--; }
    return c;
}

static vfs_node_t* ramfs_finddir(vfs_node_t* node, const char* name) {
    for (vfs_node_t* c = node->first_child; c; c = c->next_sibling) {
        if (name_eq(c->name, name)) return c;
    }
    return 0;
}

/* --- creating and removing ----------------------------------------------- */

static vfs_node_t* ramfs_create(vfs_node_t* dir, const char* name,
                                uint32_t flags) {
    vfs_node_t* n = vfs_node_alloc();
    if (!n) return 0;

    kstrlcpy(n->name, name, VFS_NAME_MAX);
    n->flags = flags;
    n->length = 0;
    /* What the shell's own `mkdir` and an open() that did not name a mode
     * get; posix.c overwrites it with the caller's when there is one. */
    n->mode = (flags & VFS_DIRECTORY) ? 0755u : 0644u;
    n->read = ramfs_read;
    n->readdir = (flags & VFS_DIRECTORY) ? ramfs_readdir : 0;
    n->finddir = (flags & VFS_DIRECTORY) ? ramfs_finddir : 0;
    vfs_dir_append(dir, n);
    return n;
}

vfs_node_t* vfs_create(vfs_node_t* dir, const char* name, uint32_t flags) {
    if (!dir || !(dir->flags & VFS_DIRECTORY)) return 0;
    if (!name || !*name) return 0;
    if (vfs_finddir(dir, name)) return 0;   /* already exists */
    vfs_node_t* n = (dir->ops && dir->ops->create)
                        ? dir->ops->create(dir, name, flags)
                        : ramfs_create(dir, name, flags);
    if (n) n->mtime = rtc_unix_time();
    return n;
}

/* A file has just changed. One place rather than three, because the two
 * callers that matter (a write and a truncate) are exactly the two things
 * that make a modification time wrong if they are missed. */
void vfs_touch(vfs_node_t* node) {
    if (node) node->mtime = rtc_unix_time();
}

/* Walks up to the root collecting names, then writes them out forwards.
 * The depth bound is the tree's, not a guess: a node cannot be its own
 * ancestor, so a chain longer than the node table is a corrupted tree
 * rather than a deep directory. */
#define VFS_MAX_DEPTH 32

uint32_t vfs_path_of(const vfs_node_t* node, char* buf, uint32_t size) {
    if (!node || !buf || size < 2) return 0;

    const vfs_node_t* chain[VFS_MAX_DEPTH];
    uint32_t depth = 0;
    for (const vfs_node_t* n = node; n && n->parent; n = n->parent) {
        if (depth >= VFS_MAX_DEPTH) return 0;
        chain[depth++] = n;
    }

    if (depth == 0) { buf[0] = '/'; buf[1] = '\0'; return 1; }

    uint32_t w = 0;
    while (depth--) {
        uint32_t len = kstrlen(chain[depth]->name);
        if (w + 1 + len + 1 > size) return 0;
        buf[w++] = '/';
        for (uint32_t i = 0; i < len; i++) buf[w++] = chain[depth]->name[i];
    }
    buf[w] = '\0';
    return w;
}

static int32_t ramfs_unlink(vfs_node_t* dir, const char* name, int want_dir) {
    vfs_node_t* victim = ramfs_finddir(dir, name);
    if (!victim) return -ENOENT;

    int is_dir = (victim->flags & VFS_DIRECTORY) != 0;

    /* rmdir on a file and unlink on a directory are both errors, and
     * Linux distinguishes them - EISDIR one way, ENOTDIR the other. */
    if (want_dir && !is_dir) return -ENOTDIR;
    if (!want_dir && is_dir) return -EISDIR;
    if (is_dir && victim->first_child) return -ENOTEMPTY;

    vfs_dir_remove(dir, victim);
    /* The name goes now; the file goes when the last descriptor and the
     * last mapping do. */
    victim->unlinked = 1;
    if (victim->refs == 0) vfs_node_release(victim);
    return 0;
}

int32_t vfs_unlink(vfs_node_t* dir, const char* name, int want_dir) {
    if (!dir || !(dir->flags & VFS_DIRECTORY)) return -ENOTDIR;
    if (!name || name_eq(name, ".") || name_eq(name, "..")) return -EINVAL;
    if (dir->ops && dir->ops->unlink) return dir->ops->unlink(dir, name, want_dir);
    return ramfs_unlink(dir, name, want_dir);
}

/* --- path resolution -----------------------------------------------------
 *
 * Real resolution: every component has to exist and be a directory. "."
 * and ".." are handled because a dynamic linker and a shell both produce
 * them, and because ".." on the root is the root, not a fault.
 *
 * Since Milestone 32 it also follows symlinks, which is why walk_from()
 * takes a starting directory and a depth: a link's target is resolved by
 * calling back into the same function, from the root for an absolute
 * target and from the link's own directory for a relative one. The depth
 * bound is what ELOOP is - a link that names itself is otherwise an
 * infinite descent, and Wine's prefix is full of links pointing at each
 * other's directories.
 *
 * `leaf_out` always points into the *caller's* string, never into a
 * rewritten copy, because a link is resolved by recursion rather than by
 * splicing its target into the path. That is what lets vfs_resolve_parent
 * hand the leaf straight to a driver. */

#define VFS_MAX_LINKS 8

static vfs_node_t* walk_from(vfs_node_t* start, const char* path,
                             const char** leaf_out, int follow_final,
                             int depth);

/* Resolves what a symlink names, from the directory it lives in. */
static vfs_node_t* follow_link(vfs_node_t* link, int depth) {
    if (depth >= VFS_MAX_LINKS) return 0;

    char target[VFS_SYMLINK_MAX];
    int32_t n = vfs_readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return 0;
    target[n] = '\0';

    vfs_node_t* base = (target[0] == '/') ? root : link->parent;
    if (!base) return 0;
    return walk_from(base, target, 0, 1, depth + 1);
}

static vfs_node_t* walk_from(vfs_node_t* start, const char* path,
                             const char** leaf_out, int follow_final,
                             int depth) {
    if (!start || !path) return 0;
    vfs_node_t* cur = start;

    /* A leading slash restarts at the root; otherwise the walk is
     * relative to whatever it was given. Absolute paths are the norm -
     * posix.c joins the working directory on before it gets here. */
    if (*path == '/') { cur = root; while (*path == '/') path++; }

    for (;;) {
        while (*path == '/') path++;
        if (!*path) {
            /* The path ended on a separator, so the thing named is the
             * directory itself. A caller wanting a leaf gets none. */
            if (leaf_out) *leaf_out = 0;
            return cur;
        }

        const char* seg = path;
        uint32_t seglen = 0;
        while (path[seglen] && path[seglen] != '/') seglen++;
        path += seglen;
        while (*path == '/') path++;

        int last = (*path == '\0');
        if (last && leaf_out) {
            /* Last component, and the caller wants the parent directory
             * plus this name rather than the node itself. */
            *leaf_out = seg;
            return cur;
        }

        if (seglen >= VFS_NAME_MAX) return 0;
        char name[VFS_NAME_MAX];
        for (uint32_t i = 0; i < seglen; i++) name[i] = seg[i];
        name[seglen] = '\0';

        vfs_node_t* next = 0;
        if (name_eq(name, ".")) {
            next = cur;
        } else if (name_eq(name, "..")) {
            next = cur->parent ? cur->parent : cur;
        } else {
            /* Through the hook, not the child list: a directory on the
             * disk reads its entries in here. */
            next = vfs_finddir(cur, name);
        }
        if (!next) return 0;

        if (next->flags & VFS_SYMLINK) {
            /* A link in the middle of a path is always followed; one at
             * the end only when the caller asked - that difference is
             * stat() versus lstat(). */
            if (!last || follow_final) {
                next = follow_link(next, depth);
                if (!next) return 0;
            }
        }

        if (last) return next;                         /* the node itself */
        if (!(next->flags & VFS_DIRECTORY)) return 0;  /* "/file/more" */
        cur = next;
    }
}

vfs_node_t* vfs_lookup(const char* path) {
    if (!path || !root) return 0;
    return walk_from(root, path, 0, 1, 0);
}

vfs_node_t* vfs_lookup_nofollow(const char* path) {
    if (!path || !root) return 0;
    return walk_from(root, path, 0, 0, 0);
}

vfs_node_t* vfs_resolve_parent(const char* path, const char** leaf_out) {
    if (!path || !leaf_out || !root) return 0;
    *leaf_out = 0;
    vfs_node_t* dir = walk_from(root, path, leaf_out, 1, 0);
    if (!dir || !*leaf_out) return 0;
    if (!(dir->flags & VFS_DIRECTORY)) return 0;
    return dir;
}

/* --- writing -------------------------------------------------------------- */

/* Grows a file's buffer to at least `want` bytes, and materialises an
 * initrd-backed file into the heap on the way - the copy-on-write that
 * keeps an unmodified boot free. Growth is doubling rather than exact, so
 * a program appending a byte at a time does not reallocate every time. */
static int ensure_capacity(vfs_node_t* n, uint32_t want) {
    if (!n->from_initrd && n->data && n->capacity >= want) return 1;

    uint32_t cap = n->capacity ? n->capacity : 64;
    while (cap < want) {
        if (cap > 0x40000000u) return 0;
        cap *= 2;
    }

    uint8_t* buf = (uint8_t*)kmalloc(cap);
    if (!buf) return 0;

    uint32_t keep = n->length < want ? n->length : want;
    if (n->data && keep) kmemcpy(buf, n->data, keep);
    if (cap > keep) kmemset(buf + keep, 0, cap - keep);

    if (n->data && !n->from_initrd) {
        heap_bytes -= n->capacity;
        kfree(n->data_base ? n->data_base : n->data);
    }
    n->data = buf;
    n->data_base = 0;
    n->mappable = 0;
    n->capacity = cap;
    n->from_initrd = 0;
    heap_bytes += cap;
    return 1;
}

/* Page-aligned storage, so that a shared mapping can hand user space the
 * frames the file's bytes are actually in.
 *
 * Alignment is got the blunt way - over-allocate by a page and round up -
 * because the kernel heap has no aligned allocator and one caller does
 * not justify inventing it. `data_base` remembers the real allocation so
 * it can still be freed.
 *
 * Growing a file after this moves its bytes, and therefore breaks any
 * mapping already handed out. Nothing does that here: a file is sized by
 * ftruncate before it is mapped, which is how anonymous shared memory is
 * made on Unix and exactly what wineserver does. Recorded rather than
 * defended against, because defending would mean a page cache and a page
 * cache is a different milestone. */
/* Set to 1 to have every mapping and its running total printed to the
 * console as it happens. Off by default: it is a few lines per program
 * launch in ordinary use and seventy during a Wine startup, which is
 * noise on a desktop and a hazard to any test matching on the
 * transcript. The counters below cost an add and stay on, so `df` can
 * still answer the question at any time. */
#define TRACE_MAPPINGS 0

/* What mapping files actually costs, so the question can be answered
 * with a number rather than an assumption. README.md has claimed since
 * Milestone 35 that Wine's startup is dominated by "no page cache - a
 * mapped DLL is read in full and copied into the heap, per file". The
 * copy is real; whether it dominates anything is a different claim, and
 * these are here to settle it against the block layer's own counters. */
static uint32_t stat_mappable_calls, stat_mappable_copied;
static uint32_t stat_materialize_calls, stat_materialize_bytes;

void vfs_map_stats(uint32_t* calls, uint32_t* copied,
                   uint32_t* mat_calls, uint32_t* mat_bytes) {
    if (calls) *calls = stat_mappable_calls;
    if (copied) *copied = stat_mappable_copied;
    if (mat_calls) *mat_calls = stat_materialize_calls;
    if (mat_bytes) *mat_bytes = stat_materialize_bytes;
}

int vfs_make_mappable(vfs_node_t* n, uint32_t bytes) {
    if (!n || (n->flags & VFS_DIRECTORY)) return 0;
    /* A disk file has to be read in before there is anything to align.
     * This is the one place where "no page cache" has a price: mapping a
     * three-megabyte DLL off the disk costs three megabytes of heap. It
     * is charged per file actually mapped, though, where the initrd
     * charged for every file that existed. */
    if (!vfs_materialize(n)) return 0;

    if (bytes < n->length) bytes = n->length;
    if (bytes == 0) bytes = 1;

    uint32_t cap = (bytes + 4095u) & ~4095u;
    /* The growth reserve, for files this kernel's own memory backs, and
     * a decision made *once* - when the storage is first laid out and
     * nothing can be mapped through it yet. See VFS_MAPPING_RESERVE.
     *
     * Both halves of that are Milestone 34's, and both were bugs first.
     * The reserve is for a file a program creates and then grows while it
     * is mapped - wineserver's session file, which is what it was
     * measured against. A file out of the initrd is a program or a DLL:
     * nothing grows one, and since private file mappings became
     * copy-on-write *every* library a dynamic linker maps comes through
     * here, so four megabytes each of headroom for growth that cannot
     * happen is a hundred megabytes of heap given to Wine's few dozen.
     *
     * And re-deciding it later is worse than either answer. `from_initrd`
     * is cleared by the copy below, so a second mapping of the same
     * library would find a file that no longer looked like an initrd one,
     * take the reserve, and *reallocate* - moving every byte to a new
     * address while the mapping made a moment earlier still pointed at
     * the old frames. ld.so maps a library four times over (the whole
     * file, then each segment), and what it read out of the second
     * mapping was the third one's memory: "wine: could not load ntdll.so:
     * eh\xdd\xdd\xdd: cannot open shared object file", a DT_NEEDED name
     * read out of a freed heap block. */
    if (!n->mappable && !n->ops && !n->from_initrd && cap < VFS_MAPPING_RESERVE) {
        cap = VFS_MAPPING_RESERVE;
    }
    if (n->mappable && n->capacity >= cap) return 1;

    /* Storage that is already mappable and is being replaced anyway: the
     * file has outgrown what it was given, and anything mapping it is
     * about to be left pointing at the old frames. Nothing here can
     * prevent that without a page cache - so say it, because silently it
     * arrives as a corrupt file three subsystems away. */
    if (n->mappable) {
        terminal_writestring_color("[vfs] ", VGA_COLOR_LIGHT_RED);
        terminal_writestring("mapped file grew past its storage - "
                             "existing mappings of it are now stale\n");
    }

    uint8_t* raw = (uint8_t*)kmalloc(cap + 4096u);
    if (!raw) return 0;
    uint8_t* buf = (uint8_t*)(((uint32_t)raw + 4095u) & ~4095u);

    uint32_t keep = n->length < cap ? n->length : cap;
    stat_mappable_calls++;
    stat_mappable_copied += keep;

    /* Reported here rather than left for a shell command to ask about.
     * A `wine` run never returns to the prompt - the wineserver it starts
     * outlives the program - so nothing typed afterwards is ever read,
     * and four attempts to collect these numbers with a --post-cmd
     * collected nothing at all. A counter that cannot be read is not a
     * measurement. This goes into the serial log, where the run itself
     * already is. */
    if (TRACE_MAPPINGS) {
        char num[12];
        terminal_writestring_color("[map] ", VGA_COLOR_LIGHT_CYAN);
        ku32_to_dec(keep >> 10, num);
        terminal_writestring(num);
        terminal_writestring("K  ");
        terminal_writestring(n->name);
        terminal_writestring("  (total ");
        ku32_to_dec(stat_mappable_copied >> 10, num);
        terminal_writestring(num);
        terminal_writestring("K in ");
        ku32_to_dec(stat_mappable_calls, num);
        terminal_writestring(num);
        terminal_writestring(" files, disk ");
        uint32_t rd = 0, wr = 0;
        blockdev_stats(&rd, &wr);
        ku32_to_dec(rd, num);
        terminal_writestring(num);
        terminal_writestring(" sectors)\n");
    }

    if (n->data && keep) kmemcpy(buf, n->data, keep);
    kmemset(buf + keep, 0, cap - keep);

    if (n->data && !n->from_initrd) {
        heap_bytes -= n->capacity;
        kfree(n->data_base ? n->data_base : n->data);
    }
    n->data = buf;
    n->data_base = raw;
    n->capacity = cap;
    n->mappable = 1;
    n->from_initrd = 0;
    heap_bytes += cap + 4096u;
    return 1;
}

int vfs_materialize(vfs_node_t* n) {
    if (!n) return 0;
    if (n->data) return 1;
    stat_materialize_calls++;
    stat_materialize_bytes += n->length;
    if (n->ops && n->ops->materialize) return n->ops->materialize(n);
    /* A ramfs file with no storage is an empty one, and an empty file's
     * bytes are trivially all present. */
    return ensure_capacity(n, n->length ? n->length : 1);
}

static int32_t ramfs_write(vfs_node_t* node, uint32_t offset, uint32_t size,
                           const uint8_t* buffer) {
    uint32_t end = offset + size;
    if (!ensure_capacity(node, end)) return -ENOSPC;

    /* Writing past the end leaves a hole, and a hole reads as zero -
     * ensure_capacity zeroes everything it hands back, so this is already
     * true and worth saying rather than relying on. */
    kmemcpy(node->data + offset, buffer, size);
    if (end > node->length) node->length = end;
    node->mtime = rtc_unix_time();
    return (int32_t)size;
}

int32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size,
                  const uint8_t* buffer) {
    if (!node) return -EBADF;
    if (node->flags & VFS_DIRECTORY) return -EISDIR;
    if (!size) return 0;

    uint32_t end = offset + size;
    if (end < offset) return -EINVAL;          /* wrapped */

    if (node->ops && node->ops->write) {
        return node->ops->write(node, offset, size, buffer);
    }
    return ramfs_write(node, offset, size, buffer);
}

static int32_t ramfs_truncate(vfs_node_t* node, uint32_t length) {
    if (length > node->length) {
        if (!ensure_capacity(node, length)) return -ENOSPC;
    } else if (node->from_initrd) {
        /* Shrinking an initrd file still has to detach it, or the next
         * write would scribble on the archive. */
        if (!ensure_capacity(node, length ? length : 1)) return -ENOSPC;
    } else if (length < node->length && node->data) {
        kmemset(node->data + length, 0, node->length - length);
    }
    node->length = length;
    node->mtime = rtc_unix_time();
    return 0;
}

int32_t vfs_truncate(vfs_node_t* node, uint32_t length) {
    if (!node) return -EBADF;
    if (node->flags & VFS_DIRECTORY) return -EISDIR;
    if (node->ops && node->ops->truncate) return node->ops->truncate(node, length);
    return ramfs_truncate(node, length);
}

/* --- symlinks ------------------------------------------------------------- */

int32_t vfs_symlink(vfs_node_t* dir, const char* name, const char* target) {
    if (!dir || !(dir->flags & VFS_DIRECTORY)) return -ENOTDIR;
    if (!name || !*name || !target || !*target) return -EINVAL;
    uint32_t tlen = kstrlen(target);
    if (tlen >= VFS_SYMLINK_MAX) return -ENAMETOOLONG;
    if (vfs_finddir(dir, name)) return -EEXIST;

    vfs_node_t* n = vfs_create(dir, name, VFS_SYMLINK);
    if (!n) return -ENOSPC;
    n->mode = 0777u;   /* what every Unix reports for a link */

    /* The target is the node's contents, which is what it is on disk too.
     * vfs_write() goes through the owning driver, so this one line stores
     * a ramfs link in the heap and a FAT link in a file on the volume. */
    int32_t w = vfs_write(n, 0, tlen, (const uint8_t*)target);
    if (w < 0) {
        vfs_unlink(dir, name, 0);
        return w;
    }
    n->length = tlen;
    return 0;
}

int32_t vfs_readlink(vfs_node_t* node, char* buf, uint32_t size) {
    if (!node || !buf) return -EFAULT;
    if (!(node->flags & VFS_SYMLINK)) return -EINVAL;
    uint32_t n = node->length;
    if (n > size) n = size;
    if (!n) return 0;
    return (int32_t)vfs_read(node, 0, n, (uint8_t*)buf);
}

/* --- rename --------------------------------------------------------------- */

/* Moving a node between two ramfs directories is relinking it, and
 * nothing else - the bytes never move. */
static int32_t ramfs_rename(vfs_node_t* olddir, const char* oldname,
                            vfs_node_t* newdir, const char* newname) {
    vfs_node_t* moving = ramfs_finddir(olddir, oldname);
    if (!moving) return -ENOENT;

    /* A rename onto an existing name replaces it, which is the property
     * that makes rename() the atomic-update primitive every program uses
     * it as. A directory in the way is refused rather than clobbered. */
    char leafbuf[VFS_NAME_MAX];
    kstrlcpy(leafbuf, newname, VFS_NAME_MAX);
    vfs_node_t* victim = ramfs_finddir(newdir, leafbuf);
    if (victim) {
        if (victim == moving) return 0;       /* rename to itself */
        if (victim->flags & VFS_DIRECTORY) return -EISDIR;
    }

    vfs_dir_remove(olddir, moving);
    if (victim) ramfs_unlink(newdir, leafbuf, 0);

    kstrlcpy(moving->name, leafbuf, VFS_NAME_MAX);
    vfs_dir_append(newdir, moving);
    return 0;
}

int32_t vfs_rename(const char* oldpath, const char* newpath) {
    const char* oldleaf = 0;
    const char* newleaf = 0;
    vfs_node_t* olddir = vfs_resolve_parent(oldpath, &oldleaf);
    if (!olddir) return -ENOENT;
    /* The old leaf points into `oldpath`, which stays alive, but resolving
     * the new path can walk symlinks and there is no reason to depend on
     * that ordering - so it is copied out first. */
    char oldbuf[VFS_NAME_MAX];
    kstrlcpy(oldbuf, oldleaf, VFS_NAME_MAX);

    vfs_node_t* newdir = vfs_resolve_parent(newpath, &newleaf);
    if (!newdir) return -ENOENT;

    /* Renaming across a mount point is not a rename, it is a copy and a
     * delete, and Unix has always made the caller do that themselves. */
    if (olddir->ops != newdir->ops) return -EXDEV;

    if (olddir->ops && olddir->ops->rename) {
        return olddir->ops->rename(olddir, oldbuf, newdir, newleaf);
    }
    return ramfs_rename(olddir, oldbuf, newdir, newleaf);
}

/* --- mounting ------------------------------------------------------------- */

uint32_t vfs_node_count(void) { return nodes_used; }
uint32_t vfs_heap_bytes(void) { return heap_bytes; }

void vfs_heap_account(int32_t delta) {
    if (delta < 0 && (uint32_t)(-delta) > heap_bytes) heap_bytes = 0;
    else heap_bytes = (uint32_t)((int32_t)heap_bytes + delta);
}

/* Makes every directory named on the way to `path`'s last component, and
 * hands back that directory plus a pointer to the leaf name inside
 * `path`. Milestone 35: the initrd carries a tree, and the tree is
 * described only by the paths of the files in it, so the directories have
 * to be inferred rather than read.
 *
 * Nothing here needs the archive sorted, though mkinitrd.py sorts it -
 * an existing directory is found and reused, and a component that exists
 * as a *file* is a broken archive and stops that entry rather than
 * replacing anything. */
static vfs_node_t* initrd_make_dirs(const char* path, const char** leaf_out) {
    vfs_node_t* dir = root;
    const char* p = path;

    *leaf_out = 0;
    while (*p == '/') p++;

    for (;;) {
        const char* slash = p;
        while (*slash && *slash != '/') slash++;
        if (!*slash) {                      /* no more separators: the leaf */
            *leaf_out = p;
            return dir;
        }

        char comp[VFS_NAME_MAX];
        uint32_t n = 0;
        while (p < slash && n < VFS_NAME_MAX - 1) comp[n++] = *p++;
        comp[n] = '\0';
        p = slash;
        while (*p == '/') p++;
        if (!n) continue;                   /* a doubled separator */

        vfs_node_t* next = vfs_finddir(dir, comp);
        if (!next) next = vfs_create(dir, comp, VFS_DIRECTORY);
        if (!next || !(next->flags & VFS_DIRECTORY)) return 0;
        dir = next;
    }
}

void ramfs_init(uint32_t initrd_addr, uint32_t initrd_size) {
    (void)initrd_size;
    free_list = 0;
    nblocks = 0;
    nodes_used = 0;
    heap_bytes = 0;

    root = vfs_node_alloc();
    if (!root) return;
    root->name[0] = '/';
    root->name[1] = '\0';
    root->flags = VFS_DIRECTORY;
    root->mode = 0755u;
    root->readdir = ramfs_readdir;
    root->finddir = ramfs_finddir;
    root->parent = 0;
    vfs_root = root;

    initrd_header_t* hdr = (initrd_header_t*)initrd_addr;
    if (hdr->magic == INITRD_MAGIC) {
        uint32_t n = hdr->nfiles;
        if (n > INITRD_MAX_FILES) n = INITRD_MAX_FILES;
        initrd_file_header_t* entries =
            (initrd_file_header_t*)(initrd_addr + sizeof(initrd_header_t));

        for (uint32_t i = 0; i < n; i++) {
            char name[VFS_NAME_MAX];
            uint32_t len = 0;
            while (len < INITRD_NAME_MAX && len < VFS_NAME_MAX - 1 &&
                   entries[i].name[len]) {
                name[len] = entries[i].name[len];
                len++;
            }
            name[len] = '\0';
            if (!len) continue;

            /* A name is a path now. Everything before the last '/' is
             * directories, made on the way if they are not there yet -
             * which is how /usr/lib/wine/i386-unix comes to exist without
             * the archive having to describe directories separately. */
            const char* leaf = 0;
            vfs_node_t* dir = initrd_make_dirs(name, &leaf);
            if (!dir || !leaf || !*leaf) continue;

            vfs_node_t* f = vfs_create(dir, leaf, VFS_FILE);
            if (!f) break;
            /* Referenced, not copied - see the file header. */
            f->data = (uint8_t*)(initrd_addr + entries[i].offset);
            f->length = entries[i].length;
            f->capacity = entries[i].length;
            f->from_initrd = 1;
        }
    }
    /* A missing or corrupt initrd still mounts an empty root rather than
     * leaving vfs_root null, so the shell reports "no files" instead of
     * "no filesystem" - that is a build problem, not something every
     * caller should have to special-case. */

    /* Somewhere for a program to write. Every Unix has one, glibc's
     * tmpfile() wants one, and the filesystem test needs a directory it
     * can own outright on both the host and here. */
    vfs_create(root, "tmp", VFS_DIRECTORY);

    /* And the mount point the disk goes on, made here rather than by
     * whoever mounts, so that "/disk" is a path that exists on a machine
     * with no disk at all - `ls /disk` should say "empty", not "no such
     * directory". */
    vfs_create(root, "disk", VFS_DIRECTORY);

    /* Home. $HOME is /root on a machine with no disk (see process.c), and
     * Wine's prefix is $HOME/.wine - which it creates itself, but only the
     * last component of it. Made here rather than shipped in the initrd
     * because the archive carries files and an empty directory is not a
     * file: "wine: chdir to /root/.wine : No such file or directory" is
     * what shipping it as nothing at all looks like. */
    vfs_create(root, "root", VFS_DIRECTORY);
}
