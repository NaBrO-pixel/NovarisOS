#ifndef RAMFS64_H
#define RAMFS64_H

#include <stdint.h>
#include <stddef.h>

/* A writable filesystem, in RAM, and since Milestone 65 a real tree.
 *
 * It was a flat table keyed by whole path: "/a/b/c" was an entry whose
 * name happened to contain slashes, and a lookup was a string compare.
 * That is enough to open a file you can already name and not enough for
 * anything that *explores* - `readdir` has nothing to enumerate, ".."
 * has nowhere to go, and creating a file in a directory that does not
 * exist succeeds. A Wine prefix is a deep tree that Wine walks, so the
 * flat version stops being a simplification and starts being a lie.
 *
 * Now each node holds one path component and its parent, the root is
 * node 0, and a path is resolved by walking it. Still no disk, no
 * permissions, no links, and no working directory - every path is
 * absolute, because there is no process attribute to make it relative
 * to yet.
 */

#define RAMFS64_MAX_NODES 192
#define RAMFS64_NAME_MAX  64
#define RAMFS64_PATH_MAX  128

void ramfs64_init(void);
void ramfs64_seed_from_initrd(void);

/* Resolves an absolute path, honouring "." and "..". Returns a node
 * index, or -1. */
int  ramfs64_lookup(const char* path);

/* Creates one node. Its parent directory must already exist - which is
 * the difference a tree makes, and the reason the seed below builds
 * intermediate directories on the way down. */
int  ramfs64_create(const char* path, int is_dir);

/* Creates a directory and every missing directory above it. */
int  ramfs64_mkdirp(const char* path);

int64_t ramfs64_read(int node, uint64_t offset, void* buf, uint64_t len);
int64_t ramfs64_write(int node, uint64_t offset, const void* buf,
                      uint64_t len);
int     ramfs64_truncate(int node);

/* unlink refuses a directory and rmdir refuses a non-empty one, which
 * is what lets a program tell "wrong kind of thing" from "still in
 * use". */
int     ramfs64_unlink(const char* path);
int     ramfs64_rmdir(const char* path);

/* Enumeration. `index` counts children from 0; returns 0 when there are
 * no more. "." and ".." are not stored as nodes - the caller synthesises
 * them, the same way a real filesystem does. */
int     ramfs64_child(int dir, uint64_t index, int* out_node);

const char* ramfs64_name(int node);
const void* ramfs64_data(int node);
uint64_t    ramfs64_size(int node);
int         ramfs64_is_dir(int node);
int         ramfs64_parent(int node);

/* Device nodes. A file marked with one of these has no bytes of its own;
 * what read/mmap/ioctl mean for it is decided by syscall64.c. */
#define RAMFS64_DEV_NONE 0
#define RAMFS64_DEV_FB   1        /* /dev/fb0 - the linear framebuffer */

int         ramfs64_set_device(int node, int device);
int         ramfs64_device(int node);
uint64_t    ramfs64_count(void);

#endif
