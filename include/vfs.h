#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_FILE      0x01u
#define VFS_DIRECTORY 0x02u
/* A bound Unix socket. Milestone 28 kept bound names in a table of their
 * own and said so; Milestone 29 found out why that is not enough. Wine's
 * client does not try to connect blindly - it lstat()s the socket path
 * first and waits for it to appear, which is how it knows the server it
 * just started is ready. A name that exists only inside the socket layer
 * never appears, so the client waited for ever and then reported that a
 * server seemed to be running but could not be reached.
 *
 * The node holds no data. It exists so that stat() can say S_IFSOCK,
 * `ls` can show it, and unlink() can take it away - which is exactly what
 * a Unix socket file is on a real system. */
#define VFS_SOCKET    0x04u
/* A symbolic link. Milestone 31 built these and then took them back out,
 * because what they unlock - Wine's DOS drive table, and therefore its
 * whole real startup path - needed a prefix, and a prefix needed a disk.
 * Milestone 32 built the disk, so they are back and they are on.
 *
 * The target path is the node's contents: `data` holds it and `length` is
 * its length, exactly as a real symlink's target is the file's data. That
 * is not a shortcut - it is what makes a symlink on the FAT volume
 * storable at all, since FAT has no link type of its own. */
#define VFS_SYMLINK   0x08u
/* Milestone 32 raised this from 64. A Wine prefix has names longer than
 * that - the winsxs assembly directories run to 91 characters - and a
 * name too long for a node is a file that can only be reached by its 8.3
 * short name, which is not a filesystem anyone can use. */
#define VFS_NAME_MAX  128

/* How much storage a *growable* mapped file gets reserved up front.
 *
 * Milestone 32. The design note on vfs_make_mappable() says growing a
 * mapped file moves its bytes and therefore breaks any mapping already
 * handed out, and records it as a known cost. Wine turns out to depend on
 * the opposite. wineserver's session mapping - the shared memory window
 * classes and other user32 objects live in - is grown with ftruncate and
 * then *extended* rather than remapped: the server maps only the new
 * tail and keeps every earlier block where it is. Moving the storage
 * under it is what "Failed to get shared session object for window class"
 * was.
 *
 * A reserve does not make that impossible, it makes it rare: growth
 * within the reserve costs nothing and moves nothing. It applies only to
 * files whose bytes this kernel owns outright (ramfs), because those are
 * the ones that grow - a DLL mapped off the disk is written once, at its
 * final size, and reserving four megabytes for each of those would be
 * four megabytes each of pure waste.
 *
 * The real answer is a page cache, where a file is a vector of frames and
 * growth appends one. That is a milestone, not a constant. */
#define VFS_MAPPING_RESERVE (4u * 1024u * 1024u)

/* How long a symlink's target may be. Wine's are short ("../drive_c",
 * "/tmp/.wine-0/server-..."), and a bound keeps a corrupt on-disk node
 * from turning into an unbounded read. */
#define VFS_SYMLINK_MAX 256

struct vfs_node;

typedef uint32_t (*vfs_read_t)(struct vfs_node* node, uint32_t offset,
                                uint32_t size, uint8_t* buffer);
typedef struct vfs_node* (*vfs_readdir_t)(struct vfs_node* node, uint32_t index);
typedef struct vfs_node* (*vfs_finddir_t)(struct vfs_node* node, const char* name);

/* The writable half of a filesystem driver.
 *
 * Milestone 32 needed this. Until now there was exactly one filesystem,
 * so vfs_write() *was* the ramfs write and no dispatch was necessary. A
 * second filesystem makes the difference between "the VFS" and "the
 * in-memory filesystem that happens to be all there is" real, and this
 * struct is that line. A node with `ops` of NULL is a ramfs node, which
 * keeps every existing node and every existing caller exactly as it was.
 *
 * Read, readdir and finddir stay as they were - per-node function
 * pointers rather than entries here - because they already worked that
 * way and changing them would have touched the shell, the desktop, the
 * ELF loader and the PE loader for no gain. */
typedef struct vfs_ops {
    int32_t (*write)(struct vfs_node* node, uint32_t offset, uint32_t size,
                     const uint8_t* buffer);
    int32_t (*truncate)(struct vfs_node* node, uint32_t length);
    struct vfs_node* (*create)(struct vfs_node* dir, const char* name,
                               uint32_t flags);
    int32_t (*unlink)(struct vfs_node* dir, const char* name, int want_dir);
    int32_t (*rename)(struct vfs_node* olddir, const char* oldname,
                      struct vfs_node* newdir, const char* newname);

    /* Brings a file's bytes into `data` so they can be mapped or executed
     * in place. A ramfs file's bytes are already there; a disk file's are
     * not, and something has to read them. NULL means "already there". */
    int (*materialize)(struct vfs_node* node);

    /* Called when the node is about to be freed, so the driver can drop
     * whatever it hung off it. */
    void (*forget)(struct vfs_node* node);
} vfs_ops_t;

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
    uint32_t flags;   /* VFS_FILE, VFS_DIRECTORY, VFS_SOCKET or VFS_SYMLINK */
    uint32_t length;  /* byte size, meaningful for files */
    uint32_t impl;
    vfs_read_t read;
    vfs_readdir_t readdir;
    vfs_finddir_t finddir;

    /* Which driver owns this node. NULL is ramfs - see vfs_ops_t. */
    const vfs_ops_t* ops;

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
     * keeps an unmodified boot's memory cost exactly what it was.
     *
     * For a disk-backed file `data` is normally NULL and the bytes are on
     * the disk; it is filled in only when something has to look at the
     * whole file at once - see materialize() above. */
    uint8_t* data;
    uint32_t capacity;
    int      from_initrd;
    int      in_use;

    /* The unaligned allocation `data` was carved out of, when the file
     * has been made mappable and `data` is a page-aligned address inside
     * it. 0 when `data` is the allocation. See vfs_make_mappable(). */
    uint8_t* data_base;
    int      mappable;

    /* When the file was last written, as a Unix time. Milestone 35: the
     * VFS had no timestamps at all and `stat` reported zeroes, which was
     * fine until Wine started *installing* things. Every file setupapi
     * copies into a prefix gets its times set afterwards, and a kernel
     * that answers utimensat with -ENOSYS turns each of those into a
     * "Converting errno 38 to STATUS_UNSUCCESSFUL" and a file copy that
     * reports failure. One number per node, set when the file changes and
     * settable from user space, is the whole of what that needs. */
    uint32_t mtime;

    /* Open descriptors and shared mappings referring to this node, and
     * whether its name has been removed while they existed. Milestone 30
     * needed both: a program that creates a temporary file, opens it,
     * unlinks it and keeps using the descriptor is doing the ordinary
     * Unix thing - it is how wineserver makes anonymous shared memory -
     * and freeing the node under it would leave an open descriptor and a
     * live mapping pointing at reused heap. The node survives its name. */
    int      refs;
    int      unlinked;

    /* The permission bits this node was created with. Nothing here
     * *enforces* them - there are no users to enforce them against - but
     * recording what a program asked for and reporting it back is not the
     * same as pretending. Milestone 29 needed it for a concrete reason:
     * wineserver creates its directory with mkdir(0700) and then refuses
     * to run if stat() says the mode is anything else, because on a real
     * machine a server directory other users can reach is a security
     * hole. Reporting a fixed 0755 for everything made that check fail. */
    uint32_t mode;

    /* --- the disk half (Milestone 32) ---------------------------------
     * Where this node's directory entry lives, so that a write can update
     * the size and first cluster the on-disk entry records. `fs_cluster`
     * is the first cluster of the file (0 for an empty one). Meaningless
     * for a ramfs node, and left zero there.
     *
     * `populated` is a directory's "I have read my entries off the disk"
     * flag. A FAT directory's children are instantiated the first time
     * anything looks inside it and then cached, which is what makes the
     * cost of a big filesystem proportional to what is *used* rather than
     * to what exists. */
    uint32_t fs_cluster;
    uint32_t fs_dirent_lba;
    uint16_t fs_dirent_off;
    uint16_t fs_slots;
    uint8_t  populated;
    uint8_t  fs_dirty;
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

/* The same, without following a symlink in the final position - which is
 * the whole of the difference between stat() and lstat(). */
vfs_node_t* vfs_lookup_nofollow(const char* path);

/* The absolute path of a node, rebuilt by walking parent links. There is
 * no path stored anywhere - the tree is the only record - so this is how
 * fchdir() answers "which directory is this descriptor?", which is how
 * wineserver moves into the config directory it opened. Returns the
 * length written, or 0 if the buffer was too small. */
uint32_t vfs_path_of(const vfs_node_t* node, char* buf, uint32_t size);

/* --- lifetime (Milestone 30) --------------------------------------------
 *
 * A name and a file are different things, and until now this filesystem
 * conflated them: unlink freed the node, so an open descriptor to an
 * unlinked file pointed at nothing. Every descriptor and every shared
 * mapping now holds a reference, and the node goes when the last one
 * does - which is what makes `open(); unlink(); keep using it` work. */
void vfs_node_ref(vfs_node_t* node);
void vfs_node_unref(vfs_node_t* node);

/* --- shared mappings (Milestone 30) -------------------------------------
 *
 * Makes a file's bytes mappable: its storage is reallocated page-aligned
 * and large enough for `bytes`, so that page N of the file is exactly one
 * physical frame and mapping it into a process hands over *that* frame.
 *
 * That is the whole of MAP_SHARED here, and it is why there is no page
 * cache: the file's bytes and the mapping are the same memory, so there
 * is nothing to keep coherent. The cost is that growing a mapped file
 * moves it - see the note in ramfs.c. Returns 0 if memory ran out. */
int vfs_make_mappable(vfs_node_t* node, uint32_t bytes);

/* Stamps a node with the current time. Writes and truncates do it
 * themselves; this is for utimensat, and for a caller that has changed a
 * file some other way. */
void vfs_touch(vfs_node_t* node);
vfs_node_t* vfs_resolve_parent(const char* path, const char** leaf_out);

/* Brings a disk-backed file's bytes into memory so they can be executed
 * or mapped in place; a no-op for a file whose bytes are already there.
 * Returns 1 on success. */
int vfs_materialize(vfs_node_t* node);

/* Grows the file if needed and copies bytes in. Returns bytes written, or
 * a negative errno. Writing an initrd-backed file copies it into the heap
 * first - see `from_initrd`. */
int32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size,
                  const uint8_t* buffer);
int32_t vfs_truncate(vfs_node_t* node, uint32_t length);

vfs_node_t* vfs_create(vfs_node_t* dir, const char* name, uint32_t flags);
int32_t vfs_unlink(vfs_node_t* dir, const char* name, int want_dir);
int32_t vfs_rename(const char* oldpath, const char* newpath);

/* Creates a symbolic link named `name` in `dir` pointing at `target`.
 * Returns 0, or a negative errno. */
int32_t vfs_symlink(vfs_node_t* dir, const char* name, const char* target);

/* Reads a symlink's target into `buf`. Returns the number of bytes, or a
 * negative errno; the result is not NUL-terminated, exactly as readlink()
 * leaves it. */
int32_t vfs_readlink(vfs_node_t* node, char* buf, uint32_t size);

/* --- node lifetime, shared between filesystem drivers -------------------
 *
 * The node cache is one pool, not one per filesystem, so that a path can
 * cross a mount point without anything having to know it did. Drivers
 * take nodes from here and give them back here.
 *
 * Milestone 32 moved this off a fixed 384-entry array and onto the heap,
 * in blocks. A FAT volume with a Wine prefix on it has thousands of
 * files, and a ceiling sized for an initrd would have turned "the disk is
 * big enough" into "the node table is not". */
vfs_node_t* vfs_node_alloc(void);
void vfs_node_release(vfs_node_t* node);
void vfs_dir_append(vfs_node_t* dir, vfs_node_t* child);
int  vfs_dir_remove(vfs_node_t* dir, vfs_node_t* child);

/* Builds the root and populates it from the initrd archive. Replaces
 * initrd_init() as the thing that sets vfs_root. */
void ramfs_init(uint32_t initrd_addr, uint32_t initrd_size);

/* How many nodes are in use, for the shell's `fsinfo`. */
uint32_t vfs_node_count(void);
uint32_t vfs_heap_bytes(void);

/* A driver that allocates a node's storage itself says how much, so the
 * figure above stays the whole of what the filesystem is holding. */
void vfs_heap_account(int32_t delta);

#endif
