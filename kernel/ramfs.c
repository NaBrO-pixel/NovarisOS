/* ramfs.c - a writable, hierarchical in-memory filesystem.
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
 */

#include "vfs.h"
#include "kheap.h"
#include "kstring.h"
#include "posix.h"

/* A static pool rather than kmalloc'd nodes, for the same reason the
 * process table is one: a fixed, visible ceiling beats an allocator
 * failure in the middle of a path walk. 256 is about ten times what the
 * initrd needs. */
/* 384 since Milestone 30: the Wine initrd carries the loader, wineserver,
 * the NLS tables and a few dozen PE builtins, and a directory tree the
 * programs then create on top of that. */
#define RAMFS_MAX_NODES 384

static vfs_node_t nodes[RAMFS_MAX_NODES];
static vfs_node_t* root = 0;
static uint32_t nodes_used = 0;
static uint32_t heap_bytes = 0;

/* --- the initrd archive, which is now only an input --------------------- */

#define INITRD_MAGIC      0x53544C52u /* 'STLR' */
#define INITRD_MAX_FILES  256
#define INITRD_NAME_MAX   60

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

static vfs_node_t* node_alloc(void) {
    for (uint32_t i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].in_use) {
            vfs_node_t* n = &nodes[i];
            kmemset(n, 0, sizeof(*n));
            n->in_use = 1;
            nodes_used++;
            return n;
        }
    }
    return 0;
}

static void node_free(vfs_node_t* n) {
    if (n->data && !n->from_initrd) {
        heap_bytes -= n->capacity;
        kfree(n->data_base ? n->data_base : n->data);
    }
    kmemset(n, 0, sizeof(*n));
    nodes_used--;
}

void vfs_node_ref(vfs_node_t* node) {
    if (node) node->refs++;
}

void vfs_node_unref(vfs_node_t* node) {
    if (!node || node->refs <= 0) return;
    if (--node->refs == 0 && node->unlinked) node_free(node);
}

static int name_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Compares `a` against the first `n` bytes of `b`, which is how a path
 * component is matched without copying it out first. */
static int name_eq_n(const char* name, const char* seg, uint32_t seglen) {
    for (uint32_t i = 0; i < seglen; i++) {
        if (!name[i] || name[i] != seg[i]) return 0;
    }
    return name[seglen] == '\0';
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

vfs_node_t* vfs_create(vfs_node_t* dir, const char* name, uint32_t flags) {
    if (!dir || !(dir->flags & VFS_DIRECTORY)) return 0;
    if (!name || !*name) return 0;
    if (ramfs_finddir(dir, name)) return 0;   /* already exists */

    vfs_node_t* n = node_alloc();
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
    n->parent = dir;

    /* Appended, not prepended: readdir order is creation order, which is
     * what makes `ls` stable across boots and a directory listing
     * comparable between two runs. */
    vfs_node_t** tail = &dir->first_child;
    while (*tail) tail = &(*tail)->next_sibling;
    *tail = n;
    return n;
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

int32_t vfs_unlink(vfs_node_t* dir, const char* name, int want_dir) {
    if (!dir || !(dir->flags & VFS_DIRECTORY)) return -ENOTDIR;
    if (name_eq(name, ".") || name_eq(name, "..")) return -EINVAL;

    vfs_node_t** link = &dir->first_child;
    while (*link && !name_eq((*link)->name, name)) link = &(*link)->next_sibling;
    if (!*link) return -ENOENT;

    vfs_node_t* victim = *link;
    int is_dir = (victim->flags & VFS_DIRECTORY) != 0;

    /* rmdir on a file and unlink on a directory are both errors, and
     * Linux distinguishes them - EISDIR one way, ENOTDIR the other. */
    if (want_dir && !is_dir) return -ENOTDIR;
    if (!want_dir && is_dir) return -EISDIR;
    if (is_dir && victim->first_child) return -ENOTEMPTY;

    *link = victim->next_sibling;
    /* The name goes now; the file goes when the last descriptor and the
     * last mapping do. */
    victim->unlinked = 1;
    victim->parent = 0;
    victim->next_sibling = 0;
    if (victim->refs == 0) node_free(victim);
    return 0;
}

/* --- path resolution -----------------------------------------------------
 *
 * Real resolution: every component has to exist and be a directory. "."
 * and ".." are handled because a dynamic linker and a shell both produce
 * them, and because ".." on the root is the root, not a fault. */

static vfs_node_t* walk(const char* path, const char** leaf_out) {
    if (!root) return 0;
    vfs_node_t* cur = root;

    /* A relative path is relative to the root: there is one process at a
     * time and no per-process working directory yet, so "/" is it. */
    while (*path == '/') path++;

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

        if (!*path && leaf_out) {
            /* Last component, and the caller wants the parent directory
             * plus this name rather than the node itself. */
            *leaf_out = seg;
            return cur;
        }

        /* Not the last component (or the caller wants the node): descend. */
        vfs_node_t* next = 0;
        if (name_eq_n(".", seg, seglen)) {
            next = cur;
        } else if (name_eq_n("..", seg, seglen)) {
            next = cur->parent ? cur->parent : cur;
        } else {
            for (vfs_node_t* c = cur->first_child; c; c = c->next_sibling) {
                if (name_eq_n(c->name, seg, seglen)) { next = c; break; }
            }
        }
        if (!next) return 0;

        if (!*path) return next;             /* the node itself */
        if (!(next->flags & VFS_DIRECTORY)) return 0;  /* "/file/more" */
        cur = next;
    }
}

vfs_node_t* vfs_lookup(const char* path) {
    if (!path) return 0;
    return walk(path, 0);
}

vfs_node_t* vfs_resolve_parent(const char* path, const char** leaf_out) {
    if (!path || !leaf_out) return 0;
    *leaf_out = 0;
    vfs_node_t* dir = walk(path, leaf_out);
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
    if (!n->from_initrd && n->capacity >= want) return 1;

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
int vfs_make_mappable(vfs_node_t* n, uint32_t bytes) {
    if (!n || (n->flags & VFS_DIRECTORY)) return 0;
    if (bytes < n->length) bytes = n->length;
    if (bytes == 0) bytes = 1;

    uint32_t cap = (bytes + 4095u) & ~4095u;
    if (n->mappable && n->capacity >= cap) return 1;

    uint8_t* raw = (uint8_t*)kmalloc(cap + 4096u);
    if (!raw) return 0;
    uint8_t* buf = (uint8_t*)(((uint32_t)raw + 4095u) & ~4095u);

    uint32_t keep = n->length < cap ? n->length : cap;
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

int32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size,
                  const uint8_t* buffer) {
    if (!node) return -EBADF;
    if (node->flags & VFS_DIRECTORY) return -EISDIR;
    if (!size) return 0;

    uint32_t end = offset + size;
    if (end < offset) return -EINVAL;          /* wrapped */
    if (!ensure_capacity(node, end)) return -ENOSPC;

    /* Writing past the end leaves a hole, and a hole reads as zero -
     * ensure_capacity zeroes everything it hands back, so this is already
     * true and worth saying rather than relying on. */
    kmemcpy(node->data + offset, buffer, size);
    if (end > node->length) node->length = end;
    return (int32_t)size;
}

int32_t vfs_truncate(vfs_node_t* node, uint32_t length) {
    if (!node) return -EBADF;
    if (node->flags & VFS_DIRECTORY) return -EISDIR;

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
    return 0;
}

/* --- rename --------------------------------------------------------------- */

int32_t vfs_rename(const char* oldpath, const char* newpath) {
    const char* oldleaf = 0;
    const char* newleaf = 0;
    vfs_node_t* olddir = vfs_resolve_parent(oldpath, &oldleaf);
    vfs_node_t* newdir = vfs_resolve_parent(newpath, &newleaf);
    if (!olddir || !newdir) return -ENOENT;

    /* Unlink from the old directory without freeing anything. */
    vfs_node_t** link = &olddir->first_child;
    while (*link && !name_eq_n((*link)->name, oldleaf, kstrlen(oldleaf))) {
        link = &(*link)->next_sibling;
    }
    if (!*link) return -ENOENT;
    vfs_node_t* moving = *link;

    /* A rename onto an existing name replaces it, which is the property
     * that makes rename() the atomic-update primitive every program uses
     * it as. A directory in the way is refused rather than clobbered. */
    char leafbuf[VFS_NAME_MAX];
    kstrlcpy(leafbuf, newleaf, VFS_NAME_MAX);
    vfs_node_t* victim = ramfs_finddir(newdir, leafbuf);
    if (victim) {
        if (victim == moving) return 0;       /* rename to itself */
        if (victim->flags & VFS_DIRECTORY) return -EISDIR;
    }

    *link = moving->next_sibling;
    moving->next_sibling = 0;

    if (victim) vfs_unlink(newdir, leafbuf, 0);

    kstrlcpy(moving->name, leafbuf, VFS_NAME_MAX);
    moving->parent = newdir;
    vfs_node_t** tail = &newdir->first_child;
    while (*tail) tail = &(*tail)->next_sibling;
    *tail = moving;
    return 0;
}

/* --- mounting ------------------------------------------------------------- */

uint32_t vfs_node_count(void) { return nodes_used; }
uint32_t vfs_heap_bytes(void) { return heap_bytes; }

void ramfs_init(uint32_t initrd_addr, uint32_t initrd_size) {
    (void)initrd_size;
    for (uint32_t i = 0; i < RAMFS_MAX_NODES; i++) nodes[i].in_use = 0;
    nodes_used = 0;
    heap_bytes = 0;

    root = node_alloc();
    root->name[0] = '/';
    root->name[1] = '\0';
    root->flags = VFS_DIRECTORY;
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

            vfs_node_t* f = vfs_create(root, name, VFS_FILE);
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
}
