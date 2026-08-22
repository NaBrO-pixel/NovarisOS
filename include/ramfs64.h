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
 * node 0, and a path is resolved by walking it. Still no disk and no
 * permissions.
 *
 * Milestone 68 sized it against the thing it exists for. A prefix built
 * by this very Wine tree, counted rather than estimated, is 1049 nodes
 * (119 directories, 917 files, 13 symlinks), its longest component is
 * 93 characters and its longest path 170. Every one of those numbers
 * was over a limit here: 192 nodes, 64-character names, 128-character
 * paths, and no symlinks at all. A prefix could not have been unpacked
 * into this filesystem, let alone used.
 *
 * Two structural changes came with the sizing:
 *
 * - **The node table is in the heap**, not in BSS, because 4096 nodes at
 *   this name length is 1.2MB and the kernel image is not the place for
 *   it.
 * - **Directories keep a child list.** Every lookup used to scan the
 *   whole table, so resolving one path cost depth x MAX_NODES. At 192
 *   that is invisible and at 4096 it is the quadratic kmalloc of
 *   Milestone 46 all over again - Wine opens a prefix's worth of files,
 *   and the cost is paid on every component of every one of them.
 */

#define RAMFS64_MAX_NODES 4096
#define RAMFS64_NAME_MAX  256
#define RAMFS64_PATH_MAX  1024

/* How many symlinks one resolution may follow before it is called a
 * loop. Linux uses 40 and so does this. */
#define RAMFS64_LINK_MAX  40

void ramfs64_init(void);
void ramfs64_seed_from_initrd(void);

/* Resolves an absolute path, honouring ".", ".." and symlinks. Returns
 * a node index, or -1. */
int  ramfs64_lookup(const char* path);

/* The same walk, except that a symlink named as the final component is
 * returned as itself rather than followed. This is the difference
 * between stat and lstat, and it is what readlink and unlink need - a
 * readlink that followed the link would describe the target and never
 * the link. */
int  ramfs64_lookup_nofollow(const char* path);

/* Creates a symlink. `target` is stored verbatim and interpreted only
 * when something walks through it, exactly as a real filesystem does:
 * a link to a path that does not exist is legal and dangles. */
int  ramfs64_symlink(const char* path, const char* target);

/* The stored target text, or 0 if this node is not a symlink. */
const char* ramfs64_readlink(int node);
int         ramfs64_is_link(int node);

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

/* ftruncate(2): set the length either way, zeroing anything growing
 * exposes. */
int     ramfs64_resize(int node, uint64_t len);

/* rename(2). The node moves rather than being copied, so a program
 * holding the file open keeps holding the same file - which is what
 * makes "write a temporary and move it into place" atomic. */
int     ramfs64_rename(const char* oldpath, const char* newpath);

/* The physical frames backing a file, created on the first shared
 * mapping (Milestone 78). A heap allocation cannot be shared between
 * two address spaces; frames can, and the wineserver's session data has
 * to be - it hands every client a descriptor and they all map it
 * read-write. Returns 0 if the frames could not be had. */
const uint64_t* ramfs64_frames(int node, uint64_t bytes, uint64_t* out_n);

/* Open descriptors on a node. A file unlinked while open survives until
 * the last one closes - which is how a program gets anonymous shared
 * memory out of a filesystem, and what Wine does for every shared
 * mapping: create, size, unlink, map. */
void ramfs64_ref_node(int node);
void ramfs64_unref_node(int node);

/* unlink refuses a directory and rmdir refuses a non-empty one, which
 * is what lets a program tell "wrong kind of thing" from "still in
 * use". */
int     ramfs64_unlink(const char* path);
int     ramfs64_rmdir(const char* path);

/* Enumeration. `index` counts children from 0; returns 0 when there are
 * no more. "." and ".." are not stored as nodes - the caller synthesises
 * them, the same way a real filesystem does. */
int     ramfs64_child(int dir, uint64_t index, int* out_node);

/* Writes the absolute path of `node` into `out`. This is the walk the
 * other way - up the parent chain to the root - and it is what getcwd
 * and fchdir are: a directory the kernel holds as a node index has to
 * be turned back into the text a program asked for. Returns the length,
 * or -1 if it would not fit. */
int ramfs64_path(int node, char* out, uint64_t size);

const char* ramfs64_name(int node);
const void* ramfs64_data(int node);
uint64_t    ramfs64_size(int node);
/* The permission bits a node was created with. Stored rather than
 * invented, because the wineserver creates its socket directory 0700 and
 * refuses to run if stat reports anything looser. */
uint32_t    ramfs64_mode(int node);
void        ramfs64_set_mode(int node, uint32_t mode);
int         ramfs64_is_dir(int node);
int         ramfs64_parent(int node);

/* Device nodes. A file marked with one of these has no bytes of its own;
 * what read/mmap/ioctl mean for it is decided by syscall64.c. */
#define RAMFS64_DEV_NONE 0
#define RAMFS64_DEV_FB   1        /* /dev/fb0 - the linear framebuffer */
#define RAMFS64_DEV_KBD  2        /* /dev/input/event0 - the keyboard    */
#define RAMFS64_DEV_MOUSE 3       /* /dev/input/event1 - the mouse       */
#define RAMFS64_DEV_SOCK  4       /* a bound Unix-domain socket          */
#define RAMFS64_DEV_NULL  5       /* /dev/null                           */

int         ramfs64_set_device(int node, int device);
int         ramfs64_device(int node);
uint64_t    ramfs64_count(void);

#endif
