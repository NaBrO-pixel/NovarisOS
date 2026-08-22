/* ramfs64.c - files and directories, kept in the kernel heap. */

#include "ramfs64.h"
#include "kheap64.h"
#include "kstring.h"
#include "initrd64.h"
#include "paging64.h"
#include "pmm64.h"

typedef struct {
    char     name[RAMFS64_NAME_MAX];   /* one component, not a path */
    int      parent;                   /* node index; -1 for the root */
    uint8_t* data;                     /* a symlink keeps its target here */
    uint64_t size;
    uint64_t capacity;
    int      is_dir;
    int      is_link;

    /* The permission bits, as mkdir(2) and open(2) were given them.
     *
     * These used to be invented by do_stat - 0755 for a directory and
     * 0644 for a file, whatever the caller had asked for - and that is
     * fine until something reads them back and cares. The wineserver
     * does: it creates its socket directory 0700 and then refuses to
     * start if stat says anyone else can reach it, which a fixed 0755
     * always says. */
    uint32_t mode;

    /* Physical frames backing this file, once anything has mapped it
     * MAP_SHARED, and 0 before that (Milestone 78).
     *
     * A file is a heap allocation, which is fine for reading and writing
     * and useless for sharing: two processes cannot map the same
     * unaligned pointer and see each other's stores. The wineserver's
     * session data has to be exactly that - it hands each client a
     * descriptor and every one of them maps it read-write - so the first
     * shared mapping moves the contents into frames and the frames
     * become the file. `data` is released at that point, because two
     * copies of a file's bytes is not a cache, it is a bug waiting for
     * somebody to write to the wrong one. */
    uint64_t* frames;
    uint64_t  nframes;

    /* How many descriptors are open on this node, and whether its name
     * has been taken away (Milestone 78).
     *
     * On Linux a file unlinked while open goes on existing until the
     * last descriptor closes, and that is not a corner: it is how a
     * program gets anonymous shared memory out of a filesystem - create,
     * size, unlink, map, and the only thing left referring to it is the
     * descriptor. Wine does exactly that for every shared mapping it
     * makes. This used to free the node on unlink and say so in a
     * comment; the comment was right about what would happen. */
    int      openers;
    int      unlinked;

    int      device;                   /* RAMFS64_DEV_*; 0 = an ordinary file */
    int      used;

    /* The child list. A directory points at its first child and each
     * child at the next; both are node indices, -1 for "no more". This
     * is what stops a lookup having to scan the whole table - see the
     * note in ramfs64.h about why that stopped being affordable. */
    int      first_child;
    int      next_sibling;
} node64_t;

/* In the heap rather than in BSS: 4096 x sizeof(node64_t) is about
 * 1.2MB, and a kernel image does not need to carry it. */
static node64_t* nodes;
static uint64_t  node_count;

/* Free nodes, threaded through next_sibling. Without this, allocating
 * the 4000th node means scanning 4000 entries to find the one hole. */
static int free_head;

#define ROOT 0

static int ensure_capacity(node64_t* nd, uint64_t want);

static int valid(int n) {
    return nodes && n >= 0 && n < RAMFS64_MAX_NODES && nodes[n].used;
}

static int alloc_node(const char* name, int parent, int is_dir) {
    int i;

    if (!nodes || free_head < 0) return -1;
    i         = free_head;
    free_head = nodes[i].next_sibling;

    kstrlcpy(nodes[i].name, name, RAMFS64_NAME_MAX);
    nodes[i].parent       = parent;
    nodes[i].data         = 0;
    nodes[i].size         = 0;
    nodes[i].capacity     = 0;
    nodes[i].is_dir       = is_dir;
    nodes[i].is_link      = 0;
    nodes[i].frames       = 0;
    nodes[i].nframes      = 0;
    nodes[i].openers      = 0;
    nodes[i].unlinked     = 0;
    /* What do_stat used to invent, so a caller that passes no mode is
     * left exactly where it was. */
    nodes[i].mode         = is_dir ? 0755u : 0644u;
    nodes[i].device       = RAMFS64_DEV_NONE;
    nodes[i].used         = 1;
    nodes[i].first_child  = -1;
    nodes[i].next_sibling = -1;

    /* Into the parent's child list. At the front, because appending
     * would mean walking the list and the order of a directory is not
     * promised to anyone - readdir on Linux does not sort either. */
    if (parent >= 0 && parent < RAMFS64_MAX_NODES) {
        nodes[i].next_sibling      = nodes[parent].first_child;
        nodes[parent].first_child  = i;
    }

    node_count++;
    return i;
}

/* Takes a node out of its parent's child list. After this nothing can
 * find it by name; whether it still exists is a separate question, and
 * the answer is "while somebody has it open". */
static void detach_from_parent(int n) {
    int parent = nodes[n].parent;

    if (parent >= 0 && parent < RAMFS64_MAX_NODES) {
        int c = nodes[parent].first_child;
        if (c == n) {
            nodes[parent].first_child = nodes[n].next_sibling;
        } else {
            while (c >= 0 && nodes[c].next_sibling != n)
                c = nodes[c].next_sibling;
            if (c >= 0) nodes[c].next_sibling = nodes[n].next_sibling;
        }
    }
    nodes[n].parent       = -1;
    nodes[n].next_sibling = -1;
}

/* Lets the storage go and returns the slot. Only ever called when the
 * node has no name and no openers left. */
static void release_node(int n) {
    if (nodes[n].data) kfree64(nodes[n].data);
    nodes[n].data = 0;

    /* The filesystem holds one owner of each frame; a process that has
     * the file mapped holds another, so this is a release rather than a
     * free and the mapping goes on working until it is unmapped. */
    if (nodes[n].frames) {
        for (uint64_t i = 0; i < nodes[n].nframes; i++)
            pmm64_free_frame(nodes[n].frames[i]);
        kfree64(nodes[n].frames);
        nodes[n].frames  = 0;
        nodes[n].nframes = 0;
    }

    nodes[n].used         = 0;
    nodes[n].first_child  = -1;
    nodes[n].next_sibling = free_head;
    free_head             = n;
    if (node_count) node_count--;
}

/* Unlinks a node and returns it to the free list. The caller has already
 * decided this is allowed. */
static void free_node(int n) {
    detach_from_parent(n);
    release_node(n);
}

void ramfs64_ref_node(int node) {
    if (valid(node)) nodes[node].openers++;
}

void ramfs64_unref_node(int node) {
    if (!valid(node)) return;
    if (nodes[node].openers > 0) nodes[node].openers--;
    /* The last descriptor on a file that no longer has a name. Nothing
     * can reach it again, so now it can go. */
    if (nodes[node].openers == 0 && nodes[node].unlinked)
        release_node(node);
}

/* One component of a path, by name, in one directory. */
static int find_child(int dir, const char* name, uint64_t len) {
    if (!valid(dir) || !nodes[dir].is_dir) return -1;

    for (int i = nodes[dir].first_child; i >= 0; i = nodes[i].next_sibling) {
        if (kstrnlen(nodes[i].name, RAMFS64_NAME_MAX) != len) continue;
        if (kstrncmp(nodes[i].name, name, len) == 0) return i;
    }
    return -1;
}

/* Walks `path` from the root.
 *
 * `want_parent` resolves all but the last component and copies the
 * final name into `name_out` - which is what create needs, since the
 * thing it is about to make does not exist yet. It is a copy and not a
 * pointer into `path` because following a symlink rewrites the path
 * into a buffer local to this function, and a pointer into that buffer
 * would dangle the moment it returned.
 *
 * `follow_final` decides what happens when the last component is itself
 * a symlink: stat follows it, lstat and readlink do not. Components in
 * the middle are always followed - "through a link" is what a link is
 * for.
 */
static int walk(const char* path, int want_parent, int follow_final,
                char* name_out) {
    /* Two, alternating: expanding a link builds a new path out of the
     * old one, and the old one may itself be one of these buffers. */
    char bufs[2][RAMFS64_PATH_MAX];
    int  which = 0, links = 0;
    const char* p;
    int node;

    if (!nodes || !path || path[0] != '/') return -1;   /* absolute only */

    p    = path;
    node = ROOT;

    while (*p) {
        const char* start;
        uint64_t len = 0;
        int child, is_last;

        while (*p == '/') p++;
        if (!*p) break;

        start = p;
        while (*p && *p != '/') { p++; len++; }

        {
            const char* probe = p;
            while (*probe == '/') probe++;
            is_last = !*probe;
        }

        if (is_last && want_parent) {
            if (!len || len >= RAMFS64_NAME_MAX) return -1;
            if (name_out) {
                kmemcpy(name_out, start, len);
                name_out[len] = 0;
            }
            return node;
        }

        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* The root's parent is the root, as it is everywhere. */
            if (nodes[node].parent >= 0) node = nodes[node].parent;
            continue;
        }

        child = find_child(node, start, len);
        if (child < 0) return -1;

        if (nodes[child].is_link && (!is_last || follow_final)) {
            char*    out = bufs[which];
            uint64_t n   = 0;
            const char* target = (const char*)nodes[child].data;
            uint64_t tlen = nodes[child].size;

            if (++links > RAMFS64_LINK_MAX) return -1;   /* -ELOOP */
            if (!target || !tlen) return -1;
            if (tlen >= RAMFS64_PATH_MAX) return -1;

            /* The new path is the link's target with whatever was left
             * of the old one appended. A target starting with '/' is
             * resolved from the root; otherwise it is relative to the
             * directory the link sits in, which is where `node` already
             * is - we have not stepped into the link. */
            kmemcpy(out, target, tlen);
            n = tlen;

            for (const char* rest = p; *rest; rest++) {
                if (n + 2 >= RAMFS64_PATH_MAX) return -1;
                if (rest == p && *rest != '/') out[n++] = '/';
                out[n++] = *rest;
            }
            out[n] = 0;

            /* An absolute target restarts at the root; a relative one
             * carries on from `node`, the directory the link lives in.
             * The component loop itself does not care which, so this is
             * the only line that has to. */
            if (out[0] == '/') node = ROOT;

            p     = out;
            which = !which;
            continue;
        }

        node = child;
    }

    if (want_parent) return -1;       /* the path was just "/" */
    return node;
}

void ramfs64_init(void) {
    if (!nodes) {
        nodes = (node64_t*)kmalloc64(sizeof(node64_t) * RAMFS64_MAX_NODES);
        if (!nodes) return;              /* nothing else here can work */
    } else {
        for (int i = 0; i < RAMFS64_MAX_NODES; i++)
            if (nodes[i].used && nodes[i].data) kfree64(nodes[i].data);
    }

    /* Every node free, threaded into one list, so allocating the last
     * one costs the same as allocating the first. */
    for (int i = 0; i < RAMFS64_MAX_NODES; i++) {
        nodes[i].used         = 0;
        nodes[i].data         = 0;
        nodes[i].capacity     = 0;
        nodes[i].size         = 0;
        nodes[i].first_child  = -1;
        nodes[i].next_sibling = i + 1 < RAMFS64_MAX_NODES ? i + 1 : -1;
    }
    free_head  = 0;
    node_count = 0;

    /* The root, and it must be node 0 - resolve() starts there. */
    alloc_node("/", -1, 1);

    /* The directories a Linux program expects. Without /tmp a program
     * that writes a scratch file fails here and works everywhere else,
     * which is the kind of difference that makes a differential test
     * useless. */
    ramfs64_mkdirp("/tmp");
    ramfs64_mkdirp("/root");
    ramfs64_mkdirp("/etc");

    /* /dev/null, for the same reason and with a sharper edge. Wine opens
     * it before it does anything else, to guarantee that descriptors 0,
     * 1 and 2 are all open - it dups the result until it is above 2 -
     * and a program whose stdin is closed rather than empty behaves
     * differently in ways that surface a long way from here. */
    {
        int n;
        ramfs64_mkdirp("/dev");
        n = ramfs64_create("/dev/null", 0);
        if (n >= 0) {
            ramfs64_set_device(n, RAMFS64_DEV_NULL);
            ramfs64_set_mode(n, 0666);
        }
    }

    {
        static const char passwd[] = "root:x:0:0:root:/root:/bin/sh\n";
        static const char nsswitch[] =
            "passwd: files\ngroup: files\nshadow: files\nhosts: files\n";
        int n;

        /* glibc looks the user up to answer getpwuid, and Wine asks it
         * where HOME is. A lookup returning NULL is checked by nobody. */
        n = ramfs64_create("/etc/passwd", 0);
        if (n >= 0) ramfs64_write(n, 0, passwd, sizeof(passwd) - 1);

        n = ramfs64_create("/etc/nsswitch.conf", 0);
        if (n >= 0) ramfs64_write(n, 0, nsswitch, sizeof(nsswitch) - 1);
    }
}

int ramfs64_lookup(const char* path) {
    return walk(path, 0, 1, 0);
}

int ramfs64_lookup_nofollow(const char* path) {
    return walk(path, 0, 0, 0);
}

int ramfs64_create(const char* path, int is_dir) {
    char component[RAMFS64_NAME_MAX];
    int parent, existing;

    parent = walk(path, 1, 1, component);
    if (parent < 0) return -1;

    existing = find_child(parent, component,
                          kstrnlen(component, RAMFS64_NAME_MAX));
    if (existing >= 0) return existing;

    return alloc_node(component, parent, is_dir);
}

int ramfs64_symlink(const char* path, const char* target) {
    char component[RAMFS64_NAME_MAX];
    int parent, node;
    uint64_t tlen;

    if (!target || !*target) return -22;                /* -EINVAL */
    tlen = kstrlen(target);
    if (tlen >= RAMFS64_PATH_MAX) return -36;           /* -ENAMETOOLONG */

    parent = walk(path, 1, 1, component);
    if (parent < 0) return -2;                          /* -ENOENT */
    if (find_child(parent, component,
                   kstrnlen(component, RAMFS64_NAME_MAX)) >= 0)
        return -17;                                     /* -EEXIST */

    node = alloc_node(component, parent, 0);
    if (node < 0) return -28;                           /* -ENOSPC */

    /* The target is the node's contents. It is never interpreted here -
     * a link to a path that does not exist is legal and dangles, which
     * is why this does not check it. */
    nodes[node].is_link = 1;
    if (!ensure_capacity(&nodes[node], tlen + 1)) { free_node(node); return -12; }
    kmemcpy(nodes[node].data, target, tlen);
    nodes[node].data[tlen] = 0;
    nodes[node].size = tlen;
    return node;
}

const char* ramfs64_readlink(int node) {
    if (!valid(node) || !nodes[node].is_link) return 0;
    return (const char*)nodes[node].data;
}

int ramfs64_is_link(int node) {
    return valid(node) && nodes[node].is_link;
}

/* Up the parent chain, then reversed - the components arrive in the
 * wrong order and the only way to emit them in the right one is to
 * collect them first. */
int ramfs64_path(int node, char* out, uint64_t size) {
    int chain[64];
    int depth = 0;
    uint64_t n = 0;

    if (!valid(node) || !out || size < 2) return -1;

    while (node > 0 && depth < 64) {
        chain[depth++] = node;
        node = nodes[node].parent;
    }
    if (node > 0) return -1;             /* deeper than this can describe */

    if (depth == 0) { out[0] = '/'; out[1] = 0; return 1; }

    while (depth-- > 0) {
        const char* nm = nodes[chain[depth]].name;
        uint64_t len = kstrnlen(nm, RAMFS64_NAME_MAX);

        if (n + len + 2 > size) return -1;
        out[n++] = '/';
        kmemcpy(out + n, nm, len);
        n += len;
    }
    out[n] = 0;
    return (int)n;
}

int ramfs64_mkdirp(const char* path) {
    char partial[RAMFS64_PATH_MAX];
    uint64_t n = 0;
    const char* p = path;
    int node = ROOT;

    if (!path || path[0] != '/') return -1;

    partial[n++] = '/';
    while (*p) {
        uint64_t len = 0;
        const char* start;

        while (*p == '/') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != '/') { p++; len++; }

        if (n + len + 1 >= RAMFS64_PATH_MAX) return -1;
        if (n > 1) partial[n++] = '/';
        kmemcpy(partial + n, start, len);
        n += len;
        partial[n] = 0;

        node = ramfs64_lookup(partial);
        if (node < 0) node = ramfs64_create(partial, 1);
        if (node < 0) return -1;
    }
    return node;
}

/* Copy between a frame-backed file and a plain buffer. `to_file` says
 * which way. Page at a time, because the frames are not contiguous -
 * they come from the allocator one at a time and only the direct map
 * makes them all reachable at once. */
static void frame_copy(node64_t* nd, uint64_t off, void* buf,
                       uint64_t len, int to_file) {
    uint8_t* b = (uint8_t*)buf;
    uint64_t done = 0;

    while (done < len) {
        uint64_t page = (off + done) / PAGE64_SIZE;
        uint64_t in   = (off + done) % PAGE64_SIZE;
        uint64_t run  = PAGE64_SIZE - in;
        uint8_t* p;

        if (page >= nd->nframes) break;
        if (run > len - done) run = len - done;
        p = (uint8_t*)phys64_to_virt(nd->frames[page]) + in;

        if (to_file) kmemcpy(p, b + done, run);
        else         kmemcpy(b + done, p, run);
        done += run;
    }
}

/* Give this file frames, and make them the file.
 *
 * Called the first time anything maps it MAP_SHARED. Everything already
 * in the heap allocation is copied across and the allocation is
 * released: one file, one copy of its bytes, whichever way it is
 * reached afterwards. Returns 0 if the frames could not be had, leaving
 * the node exactly as it was. */
static int make_framed(node64_t* nd, uint64_t bytes) {
    uint64_t need = (bytes + PAGE64_SIZE - 1) / PAGE64_SIZE;
    uint64_t* fresh;
    uint64_t got = 0;

    if (!need) need = 1;
    if (nd->frames && nd->nframes >= need) return 1;

    fresh = (uint64_t*)kmalloc64(need * sizeof(uint64_t));
    if (!fresh) return 0;

    /* The frames it already had keep their contents and their identity -
     * somebody may be mapping them right now, so growing a shared file
     * must not move it. */
    for (; got < nd->nframes && got < need; got++) fresh[got] = nd->frames[got];

    for (; got < need; got++) {
        uint64_t f = pmm64_alloc_frame();
        if (!f) {
            /* Undo, so a failure leaves a file rather than half of one. */
            for (uint64_t k = nd->nframes; k < got; k++) pmm64_free_frame(fresh[k]);
            kfree64(fresh);
            return 0;
        }
        kmemset(phys64_to_virt(f), 0, PAGE64_SIZE);
        fresh[got] = f;
    }

    if (nd->frames) kfree64(nd->frames);
    nd->frames  = fresh;
    nd->nframes = need;

    /* First time: move the contents in and let the heap copy go. */
    if (nd->data) {
        frame_copy(nd, 0, nd->data, nd->size, 1);
        kfree64(nd->data);
        nd->data     = 0;
        nd->capacity = 0;
    }
    return 1;
}

static int ensure_capacity(node64_t* nd, uint64_t want) {
    uint64_t cap;
    uint8_t* fresh;

    if (want <= nd->capacity) return 1;

    cap = nd->capacity ? nd->capacity : 64;
    while (cap < want) cap *= 2;

    fresh = (uint8_t*)kmalloc64(cap);
    if (!fresh) return 0;

    for (uint64_t i = 0; i < nd->size; i++) fresh[i] = nd->data[i];
    for (uint64_t i = nd->size; i < cap; i++) fresh[i] = 0;

    if (nd->data) kfree64(nd->data);
    nd->data     = fresh;
    nd->capacity = cap;
    return 1;
}

int64_t ramfs64_read(int node, uint64_t offset, void* buf, uint64_t len) {
    node64_t* nd;

    if (!valid(node)) return -9;
    nd = &nodes[node];
    if (nd->is_dir) return -21;                     /* -EISDIR */

    if (offset >= nd->size) return 0;
    if (offset + len > nd->size) len = nd->size - offset;

    if (nd->frames) frame_copy(nd, offset, buf, len, 0);
    else            kmemcpy(buf, nd->data + offset, len);
    return (int64_t)len;
}

int64_t ramfs64_write(int node, uint64_t offset, const void* buf,
                      uint64_t len) {
    node64_t* nd;

    if (!valid(node)) return -9;
    nd = &nodes[node];
    if (nd->is_dir) return -21;

    if (nd->frames) {
        if (!make_framed(nd, offset + len)) return -12;
        frame_copy(nd, offset, (void*)buf, len, 1);
    } else {
        if (!ensure_capacity(nd, offset + len)) return -12;
        kmemcpy(nd->data + offset, buf, len);
    }
    if (offset + len > nd->size) nd->size = offset + len;
    return (int64_t)len;
}

int ramfs64_truncate(int node) {
    if (!valid(node)) return -9;
    nodes[node].size = 0;
    return 0;
}

/* ftruncate(2): set a file's length, either way.
 *
 * Growing has to zero what it exposes. The bytes between the old end and
 * the new one were never written by anybody, so they must read as zeros
 * rather than as whatever the allocation happened to contain - and this
 * heap hands back memory that was somebody else's file a moment ago. */
int ramfs64_resize(int node, uint64_t len) {
    if (!valid(node)) return -9;                    /* -EBADF */
    if (nodes[node].is_dir) return -21;             /* -EISDIR */

    if (len > nodes[node].size) {
        if (nodes[node].frames) {
            /* Fresh frames arrive zeroed, so growing into them exposes
             * zeros without doing anything more. */
            if (!make_framed(&nodes[node], len)) return -28;
        } else {
            if (!ensure_capacity(&nodes[node], len)) return -28;  /* -ENOSPC */
            kmemset(nodes[node].data + nodes[node].size, 0,
                    len - nodes[node].size);
        }
    }
    nodes[node].size = len;
    return 0;
}

/* The frames backing this file, creating them if this is the first
 * shared mapping. `bytes` is how much of it the caller wants to map.
 *
 * The caller takes a reference on each frame it maps: the filesystem
 * holds one of its own, so a process unmapping the file drops its
 * reference and the file keeps its contents. */
const uint64_t* ramfs64_frames(int node, uint64_t bytes, uint64_t* out_n) {
    if (!valid(node) || nodes[node].is_dir) return 0;
    if (bytes < nodes[node].size) bytes = nodes[node].size;
    if (!make_framed(&nodes[node], bytes)) return 0;
    if (out_n) *out_n = nodes[node].nframes;
    return nodes[node].frames;
}

/* rename(2), which is how a program replaces a file without anybody ever
 * seeing it half written: write a temporary, then move it into place in
 * one step. The wineserver saves its registry exactly that way, and
 * without this it writes reg30000.tmp, fails to install it, and starts
 * again - so `wineboot -u` never finishes.
 *
 * The node itself moves. Its contents are not copied and its identity
 * does not change, which is what makes the replacement atomic from
 * outside and what a program holding the file open expects. */
int ramfs64_rename(const char* oldpath, const char* newpath) {
    char component[RAMFS64_NAME_MAX];
    int src, dst_parent, existing;

    src = ramfs64_lookup_nofollow(oldpath);
    if (src < 0) return -2;                         /* -ENOENT */
    if (src == ROOT) return -16;                    /* -EBUSY */

    dst_parent = walk(newpath, 1, 1, component);
    if (dst_parent < 0) return -2;

    existing = find_child(dst_parent, component,
                          kstrnlen(component, RAMFS64_NAME_MAX));
    if (existing == src) return 0;                  /* the same name */

    if (existing >= 0) {
        /* Linux refuses to replace a directory with a file or the other
         * way round, and refuses a non-empty directory either way.
         * Answering those wrongly turns a failed rename into a lost
         * tree. */
        if (nodes[existing].is_dir != nodes[src].is_dir)
            return nodes[existing].is_dir ? -21 : -20;   /* EISDIR/ENOTDIR */
        if (nodes[existing].is_dir && nodes[existing].first_child >= 0)
            return -39;                             /* -ENOTEMPTY */
        /* Replacing a file somebody has open is an unlink of the old
         * one, with the same rule: its name goes, it does not. */
        detach_from_parent(existing);
        nodes[existing].unlinked = 1;
        if (nodes[existing].openers == 0) release_node(existing);
    }

    /* A directory cannot be moved into itself or into its own subtree:
     * the result is a loop with no path back to the root, which every
     * walk in this file would follow forever. */
    if (nodes[src].is_dir) {
        for (int a = dst_parent; a >= 0; a = nodes[a].parent) {
            if (a == src) return -22;               /* -EINVAL */
            if (a == ROOT) break;
        }
    }

    /* Out of the old parent's child list. free_node does this too, but
     * it also frees the node - here the node is the thing being kept. */
    {
        int op = nodes[src].parent;
        if (op >= 0 && op < RAMFS64_MAX_NODES) {
            int c = nodes[op].first_child;
            if (c == src) {
                nodes[op].first_child = nodes[src].next_sibling;
            } else {
                while (c >= 0 && nodes[c].next_sibling != src)
                    c = nodes[c].next_sibling;
                if (c >= 0) nodes[c].next_sibling = nodes[src].next_sibling;
            }
        }
    }

    kstrlcpy(nodes[src].name, component, RAMFS64_NAME_MAX);
    nodes[src].parent       = dst_parent;
    nodes[src].next_sibling = nodes[dst_parent].first_child;
    nodes[dst_parent].first_child = src;
    return 0;
}

static int has_children(int dir) {
    return valid(dir) && nodes[dir].first_child >= 0;
}

int ramfs64_unlink(const char* path) {
    /* Deliberately does not follow a final symlink: unlinking a link
     * removes the link, not the file it names. */
    int i = ramfs64_lookup_nofollow(path);

    if (i < 0) return -2;                           /* -ENOENT */
    if (nodes[i].is_dir) return -21;                /* -EISDIR */

    /* The name goes now; the file goes when the last descriptor on it
     * closes. Anything still holding it open keeps reading and writing
     * the same bytes, which is what makes create-size-unlink-map work -
     * and that is how a program asks a filesystem for anonymous shared
     * memory. */
    detach_from_parent(i);
    nodes[i].unlinked = 1;
    if (nodes[i].openers == 0) release_node(i);
    return 0;
}

int ramfs64_rmdir(const char* path) {
    int i = ramfs64_lookup_nofollow(path);

    if (i < 0) return -2;
    if (!nodes[i].is_dir) return -20;               /* -ENOTDIR */
    if (i == ROOT) return -16;                      /* -EBUSY */
    if (has_children(i)) return -39;                /* -ENOTEMPTY */

    free_node(i);
    return 0;
}

int ramfs64_child(int dir, uint64_t index, int* out_node) {
    uint64_t seen = 0;

    if (!valid(dir)) return 0;
    if (!nodes[dir].is_dir) return 0;

    for (int i = nodes[dir].first_child; i >= 0; i = nodes[i].next_sibling) {
        if (seen == index) { if (out_node) *out_node = i; return 1; }
        seen++;
    }
    return 0;
}

const char* ramfs64_name(int node) {
    if (!valid(node)) return 0;
    return nodes[node].name;
}

const void* ramfs64_data(int node) {
    if (!valid(node)) return 0;
    return nodes[node].data;
}

uint64_t ramfs64_size(int node) {
    if (!valid(node)) return 0;
    return nodes[node].size;
}

uint32_t ramfs64_mode(int node) {
    return valid(node) ? nodes[node].mode : 0;
}

void ramfs64_set_mode(int node, uint32_t mode) {
    if (valid(node)) nodes[node].mode = mode & 07777u;
}

int ramfs64_is_dir(int node) {
    if (!valid(node)) return 0;
    return nodes[node].is_dir;
}

int ramfs64_parent(int node) {
    if (!valid(node)) return -1;
    return nodes[node].parent;
}

/* A device node is a file whose bytes are not in the heap - what open,
 * mmap and ioctl do with it depends on which device it is. The
 * filesystem itself deliberately knows nothing beyond the number: it is
 * syscall64.c that decides that RAMFS64_DEV_FB means "mmap this to the
 * framebuffer", exactly as Linux keeps its device model out of tmpfs. */
int ramfs64_set_device(int node, int device) {
    if (!valid(node)) return 0;
    if (nodes[node].is_dir) return 0;
    nodes[node].device = device;
    return 1;
}

int ramfs64_device(int node) {
    if (!valid(node))
        return RAMFS64_DEV_NONE;
    return nodes[node].device;
}

uint64_t ramfs64_count(void) { return node_count; }

void ramfs64_seed_from_initrd(void) {
    uint64_t n = initrd64_file_count();

    for (uint64_t i = 0; i < n; i++) {
        const char* name = initrd64_name(i);
        const void* data;
        uint64_t len;
        char path[RAMFS64_PATH_MAX];
        char dir[RAMFS64_PATH_MAX];
        int node, cut = -1;

        if (!name) continue;
        if (initrd64_open(name, &data, &len) != INITRD64_OK) continue;

        /* The archive stores relative names; the filesystem is rooted. */
        path[0] = '/';
        kstrlcpy(path + 1, name, RAMFS64_PATH_MAX - 1);

        /* Every directory on the way has to exist first now, which is
         * the whole difference between a tree and a table of strings. */
        for (int c = 0; path[c]; c++) if (path[c] == '/') cut = c;
        if (cut > 0) {
            kstrlcpy(dir, path, (uint64_t)cut + 1);
            ramfs64_mkdirp(dir);
        }

        node = ramfs64_create(path, 0);
        if (node < 0) continue;
        ramfs64_write(node, 0, data, len);
    }
}
