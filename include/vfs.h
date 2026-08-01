#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_FILE      0x01u
#define VFS_DIRECTORY 0x02u
#define VFS_NAME_MAX  64

struct vfs_node;

typedef uint32_t (*vfs_read_t)(struct vfs_node* node, uint32_t offset,
                                uint32_t size, uint8_t* buffer);
typedef struct vfs_node* (*vfs_readdir_t)(struct vfs_node* node, uint32_t index);
typedef struct vfs_node* (*vfs_finddir_t)(struct vfs_node* node, const char* name);

/* One node in the filesystem tree. `impl` is opaque storage for whichever
 * filesystem driver owns this node (the initrd driver used it as a byte
 * offset into the archive); nothing outside that driver should interpret
 * it.
 *
 * Milestone 26 gave the tree real *shape* - parent and sibling links, so
 * directories nest - and made it writable. Before that the root was a
 * flat list of read-only initrd entries and a path resolved on its last
 * component alone. The read/readdir/finddir hooks are unchanged, which is
 * why the shell, the desktop's file browser, the ELF loader and the PE
 * loader all needed no changes at all. */
typedef struct vfs_node {
    char name[VFS_NAME_MAX];
    uint32_t flags;   /* VFS_FILE or VFS_DIRECTORY */
    uint32_t length;  /* byte size, meaningful for files */
    uint32_t impl;
    vfs_read_t read;
    vfs_readdir_t readdir;
    vfs_finddir_t finddir;

    /* --- the tree (Milestone 26) --------------------------------------
     * Children are a singly linked list in creation order, which is the
     * order readdir reports and the order `ls` shows. */
    struct vfs_node* parent;
    struct vfs_node* first_child;
    struct vfs_node* next_sibling;

    /* --- file contents ------------------------------------------------
     * `data` is the file's bytes and `capacity` how much room they have.
     * When `from_initrd` is set, `data` points *into the initrd image*
     * and is not owned - the file costs nothing until something writes
     * it, at which point the bytes are copied into the heap. That is what
     * keeps an unmodified boot's memory cost exactly what it was. */
    uint8_t* data;
    uint32_t capacity;
    int      from_initrd;
    int      in_use;
} vfs_node_t;

/* The root of whichever filesystem is currently mounted - just the
 * initrd for now (kernel/initrd.c sets this). NULL until something
 * mounts, so callers must check before using it. Only one filesystem can
 * be mounted at a time right now; a real mount table is future work. */
extern vfs_node_t* vfs_root;

uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
vfs_node_t* vfs_readdir(vfs_node_t* node, uint32_t index);
vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name);

/* --- the writable half (Milestone 26) -----------------------------------
 *
 * All of it lives in kernel/ramfs.c. Errors come back as negative errno
 * values so kernel/posix.c can hand them straight to the program.
 *
 * Path resolution is real: every component of "/tmp/a/b" has to exist and
 * be a directory. `vfs_resolve_parent()` splits a path into "the
 * directory it lives in" and "the final name", which is what create,
 * unlink, mkdir and rename all need. */
vfs_node_t* vfs_lookup(const char* path);
vfs_node_t* vfs_resolve_parent(const char* path, const char** leaf_out);

/* Grows the file if needed and copies bytes in. Returns bytes written, or
 * a negative errno. Writing an initrd-backed file copies it into the heap
 * first - see `from_initrd`. */
int32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size,
                  const uint8_t* buffer);
int32_t vfs_truncate(vfs_node_t* node, uint32_t length);

vfs_node_t* vfs_create(vfs_node_t* dir, const char* name, uint32_t flags);
int32_t vfs_unlink(vfs_node_t* dir, const char* name, int want_dir);
int32_t vfs_rename(const char* oldpath, const char* newpath);

/* Builds the root and populates it from the initrd archive. Replaces
 * initrd_init() as the thing that sets vfs_root. */
void ramfs_init(uint32_t initrd_addr, uint32_t initrd_size);

/* How many nodes are in use, for the shell's `fsinfo`. */
uint32_t vfs_node_count(void);
uint32_t vfs_heap_bytes(void);

#endif
