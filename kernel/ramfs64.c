/* ramfs64.c - files and directories, kept in the kernel heap. */

#include "ramfs64.h"
#include "kheap64.h"
#include "kstring.h"
#include "initrd64.h"

typedef struct {
    char     name[RAMFS64_NAME_MAX];   /* one component, not a path */
    int      parent;                   /* node index; -1 for the root */
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
    int      is_dir;
    int      used;
} node64_t;

static node64_t nodes[RAMFS64_MAX_NODES];
static uint64_t node_count;

#define ROOT 0

static int alloc_node(const char* name, int parent, int is_dir) {
    int i;

    for (i = 0; i < RAMFS64_MAX_NODES; i++) if (!nodes[i].used) break;
    if (i == RAMFS64_MAX_NODES) return -1;

    kstrlcpy(nodes[i].name, name, RAMFS64_NAME_MAX);
    nodes[i].parent   = parent;
    nodes[i].data     = 0;
    nodes[i].size     = 0;
    nodes[i].capacity = 0;
    nodes[i].is_dir   = is_dir;
    nodes[i].used     = 1;
    node_count++;
    return i;
}

/* One component of a path, by name, in one directory. */
static int find_child(int dir, const char* name, uint64_t len) {
    if (dir < 0 || !nodes[dir].used || !nodes[dir].is_dir) return -1;

    for (int i = 0; i < RAMFS64_MAX_NODES; i++) {
        if (!nodes[i].used || nodes[i].parent != dir) continue;
        if (kstrnlen(nodes[i].name, RAMFS64_NAME_MAX) != len) continue;
        if (kstrncmp(nodes[i].name, name, len) == 0) return i;
    }
    return -1;
}

/* Walks `path` from the root. If `stop_at_parent`, resolves all but the
 * last component and hands back the final name - which is what create
 * needs, since the thing it is about to make does not exist yet. */
static int resolve(const char* path, int stop_at_parent,
                   const char** last, uint64_t* last_len) {
    int node = ROOT;
    const char* p = path;

    if (!path || path[0] != '/') return -1;    /* absolute paths only */

    while (*p) {
        const char* start;
        uint64_t len = 0;

        while (*p == '/') p++;
        if (!*p) break;

        start = p;
        while (*p && *p != '/') { p++; len++; }

        /* The last component, when the caller wants its parent. */
        {
            const char* probe = p;
            while (*probe == '/') probe++;
            if (!*probe && stop_at_parent) {
                if (last)     *last     = start;
                if (last_len) *last_len = len;
                return node;
            }
        }

        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* The root's parent is the root, as it is everywhere. */
            if (nodes[node].parent >= 0) node = nodes[node].parent;
            continue;
        }

        node = find_child(node, start, len);
        if (node < 0) return -1;
    }

    if (stop_at_parent) return -1;    /* the path was just "/" */
    return node;
}

void ramfs64_init(void) {
    for (int i = 0; i < RAMFS64_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].data) kfree64(nodes[i].data);
        nodes[i].used = 0;
        nodes[i].data = 0;
    }
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
    return resolve(path, 0, 0, 0);
}

int ramfs64_create(const char* path, int is_dir) {
    const char* name = 0;
    uint64_t len = 0;
    int parent, existing;
    char component[RAMFS64_NAME_MAX];

    parent = resolve(path, 1, &name, &len);
    if (parent < 0 || !len || len >= RAMFS64_NAME_MAX) return -1;

    existing = find_child(parent, name, len);
    if (existing >= 0) return existing;

    kmemcpy(component, name, len);
    component[len] = 0;
    return alloc_node(component, parent, is_dir);
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

    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return -9;
    nd = &nodes[node];
    if (nd->is_dir) return -21;                     /* -EISDIR */

    if (offset >= nd->size) return 0;
    if (offset + len > nd->size) len = nd->size - offset;

    kmemcpy(buf, nd->data + offset, len);
    return (int64_t)len;
}

int64_t ramfs64_write(int node, uint64_t offset, const void* buf,
                      uint64_t len) {
    node64_t* nd;

    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return -9;
    nd = &nodes[node];
    if (nd->is_dir) return -21;

    if (!ensure_capacity(nd, offset + len)) return -12;

    kmemcpy(nd->data + offset, buf, len);
    if (offset + len > nd->size) nd->size = offset + len;
    return (int64_t)len;
}

int ramfs64_truncate(int node) {
    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return -9;
    nodes[node].size = 0;
    return 0;
}

static int has_children(int dir) {
    for (int i = 0; i < RAMFS64_MAX_NODES; i++)
        if (nodes[i].used && nodes[i].parent == dir) return 1;
    return 0;
}

int ramfs64_unlink(const char* path) {
    int i = ramfs64_lookup(path);

    if (i < 0) return -2;                           /* -ENOENT */
    if (nodes[i].is_dir) return -21;                /* -EISDIR */

    /* Freed immediately: nothing here reference counts. On Linux a file
     * unlinked while open survives until the last descriptor closes, and
     * a program relying on that reads freed memory here. */
    if (nodes[i].data) kfree64(nodes[i].data);
    nodes[i].data = 0;
    nodes[i].used = 0;
    if (node_count) node_count--;
    return 0;
}

int ramfs64_rmdir(const char* path) {
    int i = ramfs64_lookup(path);

    if (i < 0) return -2;
    if (!nodes[i].is_dir) return -20;               /* -ENOTDIR */
    if (i == ROOT) return -16;                      /* -EBUSY */
    if (has_children(i)) return -39;                /* -ENOTEMPTY */

    nodes[i].used = 0;
    if (node_count) node_count--;
    return 0;
}

int ramfs64_child(int dir, uint64_t index, int* out_node) {
    uint64_t seen = 0;

    if (dir < 0 || dir >= RAMFS64_MAX_NODES || !nodes[dir].used) return 0;
    if (!nodes[dir].is_dir) return 0;

    for (int i = 0; i < RAMFS64_MAX_NODES; i++) {
        if (!nodes[i].used || nodes[i].parent != dir) continue;
        if (i == ROOT) continue;
        if (seen == index) { if (out_node) *out_node = i; return 1; }
        seen++;
    }
    return 0;
}

const char* ramfs64_name(int node) {
    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return 0;
    return nodes[node].name;
}

const void* ramfs64_data(int node) {
    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return 0;
    return nodes[node].data;
}

uint64_t ramfs64_size(int node) {
    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return 0;
    return nodes[node].size;
}

int ramfs64_is_dir(int node) {
    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return 0;
    return nodes[node].is_dir;
}

int ramfs64_parent(int node) {
    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return -1;
    return nodes[node].parent;
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
