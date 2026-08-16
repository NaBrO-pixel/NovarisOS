/* ramfs64.c - files that can be written to, kept in the kernel heap. */

#include "ramfs64.h"
#include "kheap64.h"
#include "kstring.h"
#include "initrd64.h"

typedef struct {
    char     path[RAMFS64_PATH_MAX];
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
    int      is_dir;
    int      used;
} node64_t;

static node64_t nodes[RAMFS64_MAX_NODES];
static uint64_t node_count;

void ramfs64_init(void) {
    for (int i = 0; i < RAMFS64_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].data) kfree64(nodes[i].data);
        nodes[i].used     = 0;
        nodes[i].data     = 0;
        nodes[i].size     = 0;
        nodes[i].capacity = 0;
        nodes[i].is_dir   = 0;
        nodes[i].path[0]  = 0;
    }
    node_count = 0;

    /* The directories a Linux program expects to exist. Without /tmp a
     * program that writes a scratch file fails on this system and works
     * on every other one, which is the kind of difference that makes a
     * differential test useless. */
    ramfs64_create("/tmp", 1);
    ramfs64_create("/root", 1);
    ramfs64_create("/etc", 1);

    /* glibc looks the user up to answer getpwuid, and Wine asks it where
     * HOME is. With no passwd database the lookup returns NULL, which
     * nothing checks, and the failure surfaces as a NULL dereference
     * inside a library rather than as a missing file. */
    {
        static const char passwd[] =
            "root:x:0:0:root:/root:/bin/sh\n";
        static const char nsswitch[] =
            "passwd: files\ngroup: files\nshadow: files\nhosts: files\n";
        int n;

        n = ramfs64_create("/etc/passwd", 0);
        if (n >= 0) ramfs64_write(n, 0, passwd, sizeof(passwd) - 1);

        n = ramfs64_create("/etc/nsswitch.conf", 0);
        if (n >= 0) ramfs64_write(n, 0, nsswitch, sizeof(nsswitch) - 1);
    }
}

int ramfs64_lookup(const char* path) {
    for (int i = 0; i < RAMFS64_MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        if (kstrncmp(nodes[i].path, path, RAMFS64_PATH_MAX) == 0) return i;
    }
    return -1;
}

int ramfs64_create(const char* path, int is_dir) {
    int existing = ramfs64_lookup(path);
    int i;

    if (existing >= 0) return existing;
    if (kstrnlen(path, RAMFS64_PATH_MAX) >= RAMFS64_PATH_MAX) return -1;

    for (i = 0; i < RAMFS64_MAX_NODES; i++) if (!nodes[i].used) break;
    if (i == RAMFS64_MAX_NODES) return -1;

    kstrlcpy(nodes[i].path, path, RAMFS64_PATH_MAX);
    nodes[i].data     = 0;
    nodes[i].size     = 0;
    nodes[i].capacity = 0;
    nodes[i].is_dir   = is_dir;
    nodes[i].used     = 1;
    node_count++;
    return i;
}

/* Grows a file's backing store to at least `want`. There is no realloc
 * in this kernel, so this is allocate-copy-free, and the capacity
 * doubles rather than fitting exactly - a program that appends a byte at
 * a time would otherwise copy the whole file on every write. */
static int ensure_capacity(node64_t* n, uint64_t want) {
    uint64_t cap;
    uint8_t* fresh;

    if (want <= n->capacity) return 1;

    cap = n->capacity ? n->capacity : 64;
    while (cap < want) cap *= 2;

    fresh = (uint8_t*)kmalloc64(cap);
    if (!fresh) return 0;

    for (uint64_t i = 0; i < n->size; i++) fresh[i] = n->data[i];
    for (uint64_t i = n->size; i < cap; i++) fresh[i] = 0;

    if (n->data) kfree64(n->data);
    n->data     = fresh;
    n->capacity = cap;
    return 1;
}

int64_t ramfs64_read(int node, uint64_t offset, void* buf, uint64_t len) {
    node64_t* n;

    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return -9;
    n = &nodes[node];
    if (n->is_dir) return -21;                      /* -EISDIR */

    if (offset >= n->size) return 0;                /* end of file */
    if (offset + len > n->size) len = n->size - offset;

    kmemcpy(buf, n->data + offset, len);
    return (int64_t)len;
}

int64_t ramfs64_write(int node, uint64_t offset, const void* buf,
                      uint64_t len) {
    node64_t* n;

    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return -9;
    n = &nodes[node];
    if (n->is_dir) return -21;

    if (!ensure_capacity(n, offset + len)) return -12;   /* -ENOMEM */

    /* Writing past the end leaves a hole, and a hole reads as zeros -
     * ensure_capacity zeroes what it allocates, so this is already true
     * rather than needing a second pass. */
    kmemcpy(n->data + offset, buf, len);
    if (offset + len > n->size) n->size = offset + len;
    return (int64_t)len;
}

int ramfs64_truncate(int node) {
    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return -9;
    nodes[node].size = 0;
    return 0;
}

int ramfs64_unlink(const char* path) {
    int i = ramfs64_lookup(path);
    if (i < 0) return -2;                            /* -ENOENT */
    if (nodes[i].is_dir) return -21;

    /* Freed immediately, because nothing here keeps a reference count.
     * On Linux a file that is unlinked while open stays alive until the
     * last descriptor closes; a program relying on that would read freed
     * memory here. */
    if (nodes[i].data) kfree64(nodes[i].data);
    nodes[i].data = 0;
    nodes[i].used = 0;
    if (node_count) node_count--;
    return 0;
}

uint64_t ramfs64_size(int node) {
    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return 0;
    return nodes[node].size;
}

int ramfs64_is_dir(int node) {
    if (node < 0 || node >= RAMFS64_MAX_NODES || !nodes[node].used) return 0;
    return nodes[node].is_dir;
}

uint64_t ramfs64_count(void) { return node_count; }

void ramfs64_seed_from_initrd(void) {
    uint64_t n = initrd64_file_count();

    for (uint64_t i = 0; i < n; i++) {
        const char* name = initrd64_name(i);
        const void* data;
        uint64_t len;
        char path[RAMFS64_PATH_MAX];
        int node;

        if (!name) continue;
        if (initrd64_open(name, &data, &len) != INITRD64_OK) continue;

        /* The archive stores relative names; the filesystem is rooted. */
        path[0] = '/';
        kstrlcpy(path + 1, name, RAMFS64_PATH_MAX - 1);

        node = ramfs64_create(path, 0);
        if (node < 0) continue;
        ramfs64_write(node, 0, data, len);
    }
}
