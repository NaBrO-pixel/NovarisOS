#ifndef RAMFS64_H
#define RAMFS64_H

#include <stdint.h>
#include <stddef.h>

/* A writable filesystem, in RAM.
 *
 * Milestone 54's initrd is read-only, and that is the thing standing in
 * Wine's way: a Wine prefix is thousands of files that Wine *creates*.
 * This is the smallest filesystem that can hold one - no disk, no
 * journal, nothing that survives a reboot. The 32-bit tree took
 * Milestone 26 to get a writable filesystem and Milestone 32 to put it
 * on a disk, in that order, for the same reason.
 *
 * Honest about its shape: this is a flat table keyed by whole path, not
 * a directory tree. "/a/b/c" is an entry whose name happens to contain
 * slashes, and a lookup is an exact string match. That is enough for
 * open/read/write on known paths and is not enough for readdir or for
 * renaming a directory - both of which Wine will eventually want, and
 * both of which need a real tree.
 */

#define RAMFS64_MAX_NODES 128
#define RAMFS64_PATH_MAX  128

void ramfs64_init(void);

/* Seeds the filesystem from the initrd, so that what was readable
 * before this milestone is still readable after it. */
void ramfs64_seed_from_initrd(void);

/* Returns a node index, or -1. `create` makes a regular file if it is
 * missing. */
int  ramfs64_lookup(const char* path);
int  ramfs64_create(const char* path, int is_dir);

int64_t ramfs64_read(int node, uint64_t offset, void* buf, uint64_t len);
int64_t ramfs64_write(int node, uint64_t offset, const void* buf,
                      uint64_t len);
int     ramfs64_truncate(int node);
int     ramfs64_unlink(const char* path);

uint64_t ramfs64_size(int node);
int      ramfs64_is_dir(int node);
uint64_t ramfs64_count(void);

#endif
