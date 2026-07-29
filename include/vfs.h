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

/* One node in the filesystem tree. Deliberately minimal - no write
 * support, no permissions, no timestamps - just enough to list and read
 * files. `impl` is opaque storage for whichever filesystem driver owns
 * this node (the initrd driver uses it as a byte offset into the
 * archive); nothing outside that driver should interpret it. */
typedef struct vfs_node {
    char name[VFS_NAME_MAX];
    uint32_t flags;   /* VFS_FILE or VFS_DIRECTORY */
    uint32_t length;  /* byte size, meaningful for files */
    uint32_t impl;
    vfs_read_t read;
    vfs_readdir_t readdir;
    vfs_finddir_t finddir;
} vfs_node_t;

/* The root of whichever filesystem is currently mounted - just the
 * initrd for now (kernel/initrd.c sets this). NULL until something
 * mounts, so callers must check before using it. Only one filesystem can
 * be mounted at a time right now; a real mount table is future work. */
extern vfs_node_t* vfs_root;

uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
vfs_node_t* vfs_readdir(vfs_node_t* node, uint32_t index);
vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name);

#endif
