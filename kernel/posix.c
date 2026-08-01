/* posix.c - the Linux/i386 system call ABI. See include/posix.h for why
 * this exists and what it is for. */

#include "posix.h"
#include "console.h"
#include "process.h"
#include "paging.h"
#include "pmm.h"
#include "scheduler.h"
#include "vfs.h"
#include "kstring.h"
#include "rtc.h"
#include "pit.h"

#define PAGE_SIZE 4096u
/* Must match ELF_STACK_SIZE in kernel/process.c - reported through
 * ugetrlimit(RLIMIT_STACK), which glibc uses to size its guard pages. */
#define ELF_STACK_BYTES (16u * PAGE_SIZE)

/* --- file descriptors ---------------------------------------------------
 *
 * A real table, per process. Descriptors 0/1/2 are the console; the rest
 * are open initrd files. The initrd is read-only (ROADMAP.md Milestone 6),
 * so open() for writing fails with -EROFS rather than pretending. */

#define MAX_FDS 32

typedef enum {
    FD_FREE = 0,
    FD_CONSOLE,
    FD_FILE,
} fd_kind_t;

typedef struct {
    fd_kind_t   kind;
    vfs_node_t* node;
    uint32_t    offset;
    int         writable;
    int         append;      /* O_APPEND: every write goes to the end */
    uint32_t    dir_index;   /* getdents64's position in a directory */
} fd_entry_t;

static fd_entry_t fds[MAX_FDS];

static int fd_alloc(void) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (fds[i].kind == FD_FREE) return i;
    }
    return -1;
}

static fd_entry_t* fd_get(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return 0;
    if (fds[fd].kind == FD_FREE) return 0;
    return &fds[fd];
}

/* --- the process's address space ----------------------------------------
 *
 * mmap and brk both hand out address space, so they need somewhere to
 * hand it out from. Both arenas sit clear of everything the loaders place
 * (paging_reserve_region() knows about them), and both are per-process:
 * the mappings live in the process's own page directory since Milestone
 * 15, so they vanish when it is destroyed. */

#define MMAP_ARENA_START 0x42000000u
#define MMAP_ARENA_END   0x48000000u
#define BRK_ARENA_START  0x48000000u
#define BRK_ARENA_END    0x49000000u

static uint32_t mmap_next = MMAP_ARENA_START;
static uint32_t brk_current = BRK_ARENA_START;
static uint32_t brk_mapped = BRK_ARENA_START;

static uint32_t unimplemented = 0;

/* Linux's PROT_* to the x86 page-table bits. There is no way to say
 * "readable but not writable *and* not executable" on 32-bit x86 without
 * PAE's NX bit, so PROT_EXEC is accepted and ignored - noted here because
 * a silently-wrong protection is worth being explicit about. */
static uint32_t prot_to_pte(int prot) {
    uint32_t flags = PAGE_USER;
    if (prot == PROT_NONE) return 0; /* caller unmaps instead */
    flags |= PAGE_PRESENT;
    if (prot & PROT_WRITE) flags |= PAGE_RW;
    return flags;
}

void posix_exit_process(void) {
    if (scheduler_is_active()) {
        scheduler_exit_current();
        /* Returns normally; isr.s's epilogue performs the switch as this
         * call chain unwinds - see w32_thread_exit() for the same shape
         * and why halting here instead would hang. */
        return;
    }
    process_exit_to_kernel(); /* does not return */
}

void posix_process_begin(void) {
    posix_signal_reset();
    for (int i = 0; i < MAX_FDS; i++) fds[i].kind = FD_FREE;
    for (int i = 0; i < 3; i++) {
        fds[i].kind = FD_CONSOLE;
        fds[i].node = 0;
        fds[i].offset = 0;
        fds[i].writable = (i != 0);
    }
    mmap_next = MMAP_ARENA_START;
    brk_current = BRK_ARENA_START;
    brk_mapped = BRK_ARENA_START;
    unimplemented = 0;
}

void posix_process_end(void) {
    /* The mappings themselves need no teardown: they were made in the
     * process's own page directory, which is about to be destroyed. Only
     * the bookkeeping is reset, so the next program starts from a clean
     * arena rather than inheriting a high-water mark. */
    for (int i = 0; i < MAX_FDS; i++) fds[i].kind = FD_FREE;
    mmap_next = MMAP_ARENA_START;
    brk_current = BRK_ARENA_START;
    brk_mapped = BRK_ARENA_START;
}

uint32_t posix_unimplemented_count(void) {
    return unimplemented;
}

/* --- memory ------------------------------------------------------------- */

static int map_range(uint32_t base, uint32_t bytes, uint32_t flags) {
    for (uint32_t va = base; va < base + bytes; va += PAGE_SIZE) {
        uint32_t pte = paging_get_entry(va);
        if (pte & PAGE_PRESENT) {
            /* Already mapped - which is the normal case for MAP_FIXED, and
             * exactly what a dynamic linker does: reserve the whole span
             * of a library with one PROT_NONE anonymous mapping, then map
             * each segment over it with MAP_FIXED and the protection that
             * segment actually wants. Keeping the frame but *not* applying
             * the new flags would leave every one of those segments with
             * the reservation's permissions, so a write to the GOT would
             * fault. Keep the page, take the new protection. */
            paging_map_page(va, pte & ~0xFFFu, flags);
            continue;
        }
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return 0;
        paging_map_page(va, phys, flags);
        kmemset((void*)va, 0, PAGE_SIZE); /* anonymous memory reads as zero */
    }
    return 1;
}

static void unmap_range(uint32_t base, uint32_t bytes) {
    for (uint32_t va = base; va < base + bytes; va += PAGE_SIZE) {
        uint32_t pte = paging_get_entry(va);
        if (!(pte & PAGE_PRESENT)) continue;
        paging_unmap_page(va);
        pmm_free_frame(pte & ~0xFFFu);
    }
}

/* mmap2(addr, length, prot, flags, fd, pgoffset). Only anonymous private
 * mappings are supported - file-backed mmap needs a page cache, which
 * this kernel does not have. */
static int32_t sys_mmap2(uint32_t addr, uint32_t length, int prot,
                         int flags, int fd, uint32_t pgoff) {
    (void)pgoff;
    if (length == 0) return -EINVAL;
    /* File-backed MAP_PRIVATE is real since Milestone 22, because it is
     * how a dynamic linker maps a shared library - ld-linux.so.2 does
     * essentially nothing else. With no page cache, a private file
     * mapping degenerates to "allocate pages and read the bytes in",
     * which is behaviourally correct for MAP_PRIVATE: the mapping is a
     * private copy, and nothing here ever writes it back. MAP_SHARED
     * would be a lie and is refused. */
    int file_backed = !(flags & MAP_ANONYMOUS) && fd >= 0;
    if (file_backed && (flags & MAP_SHARED)) return -ENODEV;
    if (!file_backed && fd >= 0 && !(flags & MAP_ANONYMOUS)) return -ENOSYS;

    uint32_t bytes = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t base;

    if (flags & MAP_FIXED) {
        if (addr == 0 || (addr & (PAGE_SIZE - 1))) return -EINVAL;
        base = addr;
    } else {
        if (mmap_next + bytes > MMAP_ARENA_END) return -ENOMEM;
        base = mmap_next;
        mmap_next += bytes;
    }

    uint32_t pte_flags = prot_to_pte(prot);
    if (!pte_flags) pte_flags = PAGE_PRESENT | PAGE_USER; /* PROT_NONE: mapped, unwritable */

    if (!map_range(base, bytes, pte_flags)) {
        unmap_range(base, bytes);
        return -ENOMEM;
    }

    if (!file_backed) {
        /* Anonymous memory reads as zero - *always*, including when it
         * lands on pages that were already mapped. map_range() only zeroes
         * pages it newly allocates, which is not enough here: a dynamic
         * linker maps a library's .bss with MAP_FIXED|MAP_ANONYMOUS
         * directly over pages the file mapping already populated, and
         * without this those pages keep the file's bytes.
         *
         * The symptom was a program that ran perfectly and then faulted on
         * exit reading a garbage pointer - libc's exit-handler list lives
         * in .bss, and it held whatever happened to be at that file
         * offset instead of NULL. */
        kmemset((void*)base, 0, bytes);
    }

    if (file_backed) {
        fd_entry_t* e = fd_get(fd);
        if (!e || e->kind != FD_FILE) {
            unmap_range(base, bytes);
            return -EBADF;
        }
        /* pgoff is in pages, as mmap2's name promises - the whole reason
         * mmap2 exists is to express a file offset beyond 4GB/4096 in a
         * 32-bit argument. */
        uint32_t off = pgoff * PAGE_SIZE;
        uint32_t avail = (off < e->node->length) ? e->node->length - off : 0;
        uint32_t want = length < avail ? length : avail;
        if (want) vfs_read(e->node, off, want, (uint8_t*)base);
        /* Anything past the end of the file stays zero, which is what the
         * bss tail of a shared library's data segment needs. */
    }
    return (int32_t)base;
}

static int32_t sys_munmap(uint32_t addr, uint32_t length) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (length == 0) return -EINVAL;
    unmap_range(addr, (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    return 0;
}

static int32_t sys_mprotect(uint32_t addr, uint32_t length, int prot) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    uint32_t bytes = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t flags = prot_to_pte(prot);
    if (!flags) flags = PAGE_PRESENT | PAGE_USER;

    for (uint32_t va = addr; va < addr + bytes; va += PAGE_SIZE) {
        uint32_t pte = paging_get_entry(va);
        if (!(pte & PAGE_PRESENT)) return -ENOMEM;
        /* Keep the frame, change the permission bits. */
        paging_map_page(va, pte & ~0xFFFu, flags);
    }
    return 0;
}

/* brk(0) reports the break; brk(addr) moves it. Growing maps pages,
 * shrinking unmaps them. */
static int32_t sys_brk(uint32_t addr) {
    if (addr == 0) return (int32_t)brk_current;
    if (addr < BRK_ARENA_START || addr > BRK_ARENA_END) {
        return (int32_t)brk_current; /* Linux returns the unchanged break */
    }

    uint32_t want = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (want > brk_mapped) {
        if (!map_range(brk_mapped, want - brk_mapped,
                       PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            return (int32_t)brk_current;
        }
        brk_mapped = want;
    } else if (want < brk_mapped) {
        unmap_range(want, brk_mapped - want);
        brk_mapped = want;
    }
    brk_current = addr;
    return (int32_t)brk_current;
}

/* --- files -------------------------------------------------------------- */

/* Path resolution is real since Milestone 26 - every component of
 * "/tmp/a/b" has to exist and be a directory.
 *
 * The one deliberate exception is the basename fallback, and it is what
 * keeps dynamic linking working. ld-linux.so.2 searches a list of
 * directories, so it probes "/lib/i386-linux-gnu/libc.so.6",
 * "/usr/lib/libc.so.6" and several more; the initrd's contents are all at
 * the root, and none of those directories exist. So a path whose
 * *directory part does not resolve* falls back to matching the last
 * component at the root.
 *
 * The condition matters. If the directory does resolve, a miss inside it
 * is a genuine ENOENT - otherwise unlinking /tmp/x/hello.txt and then
 * opening it would find the root's hello.txt and report success, which
 * is exactly the kind of quiet wrongness this fallback could have
 * introduced. */
static vfs_node_t* resolve_for_read(const char* path) {
    vfs_node_t* n = vfs_lookup(path);
    if (n) return n;

    const char* leaf = 0;
    if (vfs_resolve_parent(path, &leaf)) return 0;  /* the directory exists */

    const char* base = path;
    for (const char* p = path; *p; p++) if (*p == '/') base = p + 1;
    if (!*base) return 0;
    return vfs_root ? vfs_finddir(vfs_root, base) : 0;
}

static int32_t sys_open(const char* path, int flags, uint32_t mode) {
    (void)mode;
    if (!path) return -EFAULT;

    int want_write = (flags & (O_WRONLY | O_RDWR)) != 0;
    vfs_node_t* node = resolve_for_read(path);

    if (!node) {
        if (!(flags & O_CREAT)) return -ENOENT;

        const char* leaf = 0;
        vfs_node_t* dir = vfs_resolve_parent(path, &leaf);
        if (!dir) return -ENOENT;
        node = vfs_create(dir, leaf, VFS_FILE);
        if (!node) return -ENOSPC;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }

    if (node->flags & VFS_DIRECTORY) {
        /* Opening a directory read-only is legal and is how getdents64
         * gets a descriptor; opening one for writing is not. */
        if (want_write || (flags & O_CREAT)) return -EISDIR;
    } else if (flags & O_TRUNC) {
        if (want_write) vfs_truncate(node, 0);
    }

    int fd = fd_alloc();
    if (fd < 0) return -EMFILE;

    fds[fd].kind = FD_FILE;
    fds[fd].node = node;
    fds[fd].offset = (want_write && (flags & O_APPEND)) ? node->length : 0;
    fds[fd].writable = want_write;
    fds[fd].append = (flags & O_APPEND) != 0;
    fds[fd].dir_index = 0;
    return fd;
}

static int32_t sys_close(int fd) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (fd < 3) return 0; /* closing a standard stream is a no-op here */
    e->kind = FD_FREE;
    e->node = 0;
    return 0;
}

/* access(path, mode). Everything present is now readable, writable and
 * executable - the filesystem has no permissions, and claiming otherwise
 * would make W_OK lie in the direction that breaks programs. The dynamic
 * linker probes with this before it opens, so answering it is what lets
 * it find a library at all. */
static int32_t sys_access(const char* path, int mode) {
    (void)mode;
    if (!path) return -EFAULT;
    return resolve_for_read(path) ? 0 : -ENOENT;
}

static int32_t sys_read(int fd, char* buf, uint32_t count) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (!buf) return -EFAULT;
    if (count == 0) return 0;

    if (e->kind == FD_CONSOLE) {
        /* Only fd 0 reads, and only a line at a time - the console has no
         * character-at-a-time path. */
        if (fd != 0) return -EBADF;
        return -ENOSYS;
    }

    if (e->offset >= e->node->length) return 0; /* EOF */
    uint32_t left = e->node->length - e->offset;
    if (count > left) count = left;
    uint32_t got = vfs_read(e->node, e->offset, count, (uint8_t*)buf);
    e->offset += got;
    return (int32_t)got;
}

static int32_t sys_write(int fd, const char* buf, uint32_t count) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (!buf) return -EFAULT;
    if (e->kind != FD_CONSOLE) {
        if (!e->writable) return -EBADF;
        /* O_APPEND is not "seek to the end when you open it" - it is
         * "every write goes to the end", re-evaluated each time, which is
         * what makes two descriptors appending to one file not overwrite
         * each other. */
        if (e->append) e->offset = e->node->length;
        int32_t n = vfs_write(e->node, e->offset, count, (const uint8_t*)buf);
        if (n > 0) e->offset += (uint32_t)n;
        return n;
    }

    /* stderr in red, so a program's diagnostics are distinguishable from
     * its output the way they are on a real terminal. terminal_write()
     * takes a length, which matters here: a Linux write() is counted
     * bytes, not a NUL-terminated string, and may legitimately contain
     * embedded NULs or no terminator at all. */
    if (fd == 2) {
        uint8_t saved = terminal_getcolor();
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        terminal_write(buf, count);
        terminal_setcolor(saved);
    } else {
        terminal_write(buf, count);
    }
    return (int32_t)count;
}

/* writev(fd, iov, iovcnt) - glibc's printf reaches for this before
 * write(), so a program built against a real libc needs it. */
typedef struct {
    uint32_t iov_base;
    uint32_t iov_len;
} iovec_t;

static int32_t sys_writev(int fd, const iovec_t* iov, int iovcnt) {
    if (!iov || iovcnt < 0) return -EFAULT;
    int32_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int32_t n = sys_write(fd, (const char*)iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return total ? total : n;
        total += n;
    }
    return total;
}

static int32_t sys_lseek(int fd, int32_t offset, int whence) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (e->kind != FD_FILE) return -ESPIPE;

    int32_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int32_t)e->offset; break;
        case SEEK_END: base = (int32_t)e->node->length; break;
        default: return -EINVAL;
    }
    int32_t target = base + offset;
    if (target < 0) return -EINVAL;
    e->offset = (uint32_t)target;
    return target;
}

/* struct stat64's layout is fixed by the ABI. Only the fields a program
 * actually reads are filled in; the rest stay zero. */
/* i386's struct stat64, by byte offset. Only the fields something
 * actually reads are filled in, but which ones those are is not
 * obvious - see st_ino below. */
#define ST_DEV_OFFSET    0   /* st_dev, 64-bit */
#define ST_INO32_OFFSET 12   /* __st_ino, the 32-bit legacy field */
#define ST_MODE_OFFSET  16
#define ST_NLINK_OFFSET 20
#define ST_SIZE_OFFSET  44   /* st_size, 64-bit */
#define ST_BLKSIZE_OFFSET 52
#define ST_BLOCKS_OFFSET  56 /* 64-bit, in 512-byte units */
#define ST_INO_OFFSET   88   /* st_ino, the real 64-bit field */
#define S_IFREG 0100000
#define S_IFCHR 0020000
#define S_IFDIR 0040000

/* The device every initrd file claims to be on. Any non-zero value does;
 * it only has to be consistent and not collide with 0. */
#define NOVARIS_INITRD_DEV 0x0701u

/* `ino` is not decoration, and getting it wrong cost most of Milestone
 * 22. glibc's _dl_map_object_from_fd, having opened a shared library,
 * stats it and walks the list of already-loaded objects looking for one
 * with the same st_ino and st_dev - that is how a library named two
 * different ways gets loaded once rather than twice. With every file
 * reporting st_ino = 0, libc.so.6 "matched" an object already in the
 * list, so the linker closed the file and returned that map instead of
 * loading libc at all.
 *
 * The symptom was nothing like the cause: five copies of "no version
 * information available", then "undefined symbol: __libc_start_main".
 * The syscall trace is what found it - the host maps libc with five
 * mmap2 calls after the stat, and Novaris went straight from stat to
 * close. */
static int32_t stat_common(uint8_t* st, uint32_t size, uint32_t mode,
                           uint32_t ino) {
    if (!st) return -EFAULT;
    kmemset(st, 0, 96);
    *(uint32_t*)(st + ST_DEV_OFFSET) = NOVARIS_INITRD_DEV;
    *(uint32_t*)(st + ST_INO32_OFFSET) = ino;
    /* 0755 rather than 0555: the filesystem is writable now, and a
     * program that checks the mode before trying should be told the
     * truth. There are no permissions to enforce, so every file is
     * everybody's. */
    *(uint32_t*)(st + ST_MODE_OFFSET) = mode | 0755;
    *(uint32_t*)(st + ST_NLINK_OFFSET) = 1;
    *(uint64_t*)(st + ST_SIZE_OFFSET) = size;
    *(uint32_t*)(st + ST_BLKSIZE_OFFSET) = PAGE_SIZE;
    *(uint64_t*)(st + ST_BLOCKS_OFFSET) = (size + 511u) / 512u;
    *(uint64_t*)(st + ST_INO_OFFSET) = ino;
    return 0;
}

/* A stable, unique, non-zero inode per file. The VFS has no inode of its
 * own, so the node's address stands in: distinct per file, constant for
 * the life of the mount, and never 0. */
static uint32_t node_ino(const vfs_node_t* node) {
    uint32_t v = (uint32_t)node;
    return v ? v : 1u;
}

static int32_t sys_fstat64(int fd, uint8_t* st) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (e->kind == FD_CONSOLE) {
        /* The three console descriptors are one character device, and
         * distinct from every file. */
        return stat_common(st, 0, S_IFCHR, 1u + (uint32_t)fd);
    }
    uint32_t mode = (e->node->flags & VFS_DIRECTORY) ? S_IFDIR : S_IFREG;
    return stat_common(st, e->node->length, mode, node_ino(e->node));
}

static int32_t sys_stat64(const char* path, uint8_t* st) {
    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    int32_t r = sys_fstat64(fd, st);
    sys_close(fd);
    return r;
}

/* --- the writable half (Milestone 26) -----------------------------------
 *
 * All of these are thin: kernel/ramfs.c owns the tree and the errno
 * values, and these only split a path and hand it over. The *at variants
 * ignore their dirfd, because there is no per-process working directory
 * yet and every path here is resolved from the root - which is honest for
 * AT_FDCWD, the only value glibc passes. */

static int32_t sys_unlink(const char* path, int want_dir) {
    if (!path) return -EFAULT;
    const char* leaf = 0;
    vfs_node_t* dir = vfs_resolve_parent(path, &leaf);
    if (!dir) return -ENOENT;
    return vfs_unlink(dir, leaf, want_dir);
}

static int32_t sys_mkdir(const char* path) {
    if (!path) return -EFAULT;
    const char* leaf = 0;
    vfs_node_t* dir = vfs_resolve_parent(path, &leaf);
    if (!dir) return -ENOENT;
    if (vfs_finddir(dir, leaf)) return -EEXIST;
    return vfs_create(dir, leaf, VFS_DIRECTORY) ? 0 : -ENOSPC;
}

static int32_t sys_ftruncate(int fd, uint32_t length) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (e->kind == FD_CONSOLE) return -EINVAL;
    if (!e->writable) return -EBADF;
    return vfs_truncate(e->node, length);
}

/* getdents64. The struct is Linux's:
 *
 *   u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[];
 *
 * so d_name starts at offset 19 and each record is padded to a multiple
 * of 8. "." and ".." are synthesised - they are not nodes in the tree,
 * but every Linux directory has them and a program that sorts its
 * readdir output would otherwise disagree between the two systems. */
#define DT_DIR 4
#define DT_REG 8

static int32_t sys_getdents64(int fd, uint8_t* buf, uint32_t count) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (!buf) return -EFAULT;
    if (e->kind == FD_CONSOLE || !(e->node->flags & VFS_DIRECTORY)) {
        return -ENOTDIR;
    }

    uint32_t written = 0;
    for (;;) {
        const char* name;
        vfs_node_t* target;

        if (e->dir_index == 0) {
            name = ".";
            target = e->node;
        } else if (e->dir_index == 1) {
            name = "..";
            target = e->node->parent ? e->node->parent : e->node;
        } else {
            target = vfs_readdir(e->node, e->dir_index - 2);
            if (!target) break;
            name = target->name;
        }

        uint32_t namelen = kstrlen(name);
        uint32_t reclen = (19 + namelen + 1 + 7u) & ~7u;
        if (written + reclen > count) {
            /* Not even one record fits: Linux says EINVAL rather than
             * silently returning nothing, so a caller with a too-small
             * buffer finds out. */
            if (written == 0) return -EINVAL;
            break;
        }

        uint8_t* rec = buf + written;
        kmemset(rec, 0, reclen);
        *(uint64_t*)(rec + 0)  = node_ino(target);
        *(uint64_t*)(rec + 8)  = (int64_t)(e->dir_index + 1);
        *(uint16_t*)(rec + 16) = (uint16_t)reclen;
        rec[18] = (target->flags & VFS_DIRECTORY) ? DT_DIR : DT_REG;
        kmemcpy(rec + 19, name, namelen);

        written += reclen;
        e->dir_index++;
    }
    return (int32_t)written;
}

/* --- everything else ---------------------------------------------------- */

static int32_t sys_uname(uint8_t* buf) {
    if (!buf) return -EFAULT;
    /* struct utsname: six 65-byte fields. */
    kmemset(buf, 0, 6 * 65);
    kstrlcpy((char*)(buf + 0 * 65), "Novaris", 65);
    kstrlcpy((char*)(buf + 1 * 65), "novaris", 65);
    kstrlcpy((char*)(buf + 2 * 65), "0.18", 65);
    kstrlcpy((char*)(buf + 3 * 65), "Novaris Milestone 18", 65);
    kstrlcpy((char*)(buf + 4 * 65), "i686", 65);
    kstrlcpy((char*)(buf + 5 * 65), "(none)", 65);
    return 0;
}

static int32_t sys_nanosleep(const uint32_t* req) {
    if (!req) return -EFAULT;
    uint32_t ticks = req[0] * 100u + req[1] / 10000000u; /* 100Hz PIT */
    uint32_t target = pit_get_ticks() + ticks;
    __asm__ __volatile__("sti");
    while (pit_get_ticks() < target) __asm__ __volatile__("hlt");
    __asm__ __volatile__("cli");
    return 0;
}

static void report_unimplemented(uint32_t number) {
    unimplemented++;
    if (unimplemented > 8) return; /* one screenful is plenty */
    char buf[12];
    terminal_writestring_color("[posix] ", VGA_COLOR_LIGHT_BROWN);
    terminal_writestring("unimplemented syscall ");
    ku32_to_dec(number, buf);
    terminal_writestring(buf);
    terminal_writestring(" -> -ENOSYS\n");
}

/* --- tracing ------------------------------------------------------------ */

static int trace_on = 0;

void posix_set_trace(int on) { trace_on = on; }
int posix_trace_enabled(void) { return trace_on; }

static const char* syscall_name(uint32_t n) {
    switch (n) {
        case SYS_exit: return "exit";
        case SYS_read: return "read";
        case SYS_write: return "write";
        case SYS_open: return "open";
        case SYS_unlink: return "unlink";
        case SYS_unlinkat: return "unlinkat";
        case SYS_mkdir: return "mkdir";
        case SYS_mkdirat: return "mkdirat";
        case SYS_rmdir: return "rmdir";
        case SYS_rename: return "rename";
        case SYS_renameat: return "renameat";
        case SYS_renameat2: return "renameat2";
        case SYS_getdents64: return "getdents64";
        case SYS_ftruncate64: return "ftruncate64";
        case SYS_truncate64: return "truncate64";
        case SYS_fsync: return "fsync";
        case SYS_chdir: return "chdir";
        case SYS_close: return "close";
        case SYS_lseek: return "lseek";
        case SYS_getpid: return "getpid";
        case SYS_access: return "access";
        case SYS_kill: return "kill";
        case SYS_brk: return "brk";
        case SYS_ioctl: return "ioctl";
        case SYS_munmap: return "munmap";
        case SYS_uname: return "uname";
        case SYS_mprotect: return "mprotect";
        case SYS_writev: return "writev";
        case SYS_pread64: return "pread64";
        case SYS_rt_sigaction: return "rt_sigaction";
        case SYS_rt_sigprocmask: return "rt_sigprocmask";
        case SYS_rt_sigreturn: return "rt_sigreturn";
        case SYS_mmap2: return "mmap2";
        case SYS_stat64: return "stat64";
        case SYS_fstat64: return "fstat64";
        case SYS_fstatat64: return "fstatat64";
        case SYS_openat: return "openat";
        case SYS_faccessat: return "faccessat";
        case SYS_faccessat2: return "faccessat2";
        case SYS_readlinkat: return "readlinkat";
        case SYS_set_thread_area: return "set_thread_area";
        case SYS_clone: return "clone";
        case SYS_futex: return "futex";
        case SYS_gettid: return "gettid";
        case SYS_exit_group: return "exit_group";
        case SYS_set_tid_address: return "set_tid_address";
        case SYS_set_robust_list: return "set_robust_list";
        case SYS_getrandom: return "getrandom";
        case SYS_ugetrlimit: return "ugetrlimit";
        case SYS_statx: return "statx";
        case SYS_rseq: return "rseq";
        case SYS_clock_gettime: return "clock_gettime";
        case SYS_clock_gettime64: return "clock_gettime64";
        default: return 0;
    }
}

static void trace_hex(uint32_t v) {
    char b[12];
    ku32_to_hex(v, b, 0, 8);
    terminal_writestring("0x");
    terminal_writestring(b);
}

static void trace_enter(uint32_t n, uint32_t a, uint32_t b, uint32_t c,
                        uint32_t d, uint32_t e, uint32_t f) {
    char buf[12];
    terminal_writestring_color("[trace] ", VGA_COLOR_LIGHT_CYAN);
    const char* name = syscall_name(n);
    if (name) {
        terminal_writestring(name);
    } else {
        terminal_writestring("syscall#");
        ku32_to_dec(n, buf);
        terminal_writestring(buf);
    }
    terminal_writestring("(");
    trace_hex(a); terminal_writestring(", ");
    trace_hex(b); terminal_writestring(", ");
    trace_hex(c); terminal_writestring(", ");
    trace_hex(d); terminal_writestring(", ");
    trace_hex(e); terminal_writestring(", ");
    trace_hex(f);
    terminal_writestring(")");
    /* The first argument of the path-taking calls is worth seeing
     * literally - which library the linker is reaching for is usually the
     * whole question. */
    const char* path = 0;
    if (n == SYS_open || n == SYS_access || n == SYS_stat64) path = (const char*)a;
    if (n == SYS_openat || n == SYS_faccessat || n == SYS_faccessat2) {
        path = (const char*)b;
    }
    if (path) {
        terminal_writestring(" \"");
        terminal_writestring(path);
        terminal_writestring("\"");
    }
}

static void trace_exit(int32_t r) {
    char buf[12];
    terminal_writestring(" = ");
    if (r < 0 && r > -4096) {
        terminal_writestring("-");
        ku32_to_dec((uint32_t)(-r), buf);
        terminal_writestring(buf);
    } else {
        trace_hex((uint32_t)r);
    }
    terminal_writestring("\n");
}

void posix_syscall(registers_t* regs) {
    uint32_t n = regs->eax;
    uint32_t a = regs->ebx, b = regs->ecx, c = regs->edx;
    uint32_t d = regs->esi, e = regs->edi, f = regs->ebp;
    int32_t r;

    if (trace_on) trace_enter(n, a, b, c, d, e, f);

    switch (n) {
        case SYS_write:    r = sys_write((int)a, (const char*)b, c); break;
        case SYS_read:     r = sys_read((int)a, (char*)b, c); break;
        case SYS_writev:   r = sys_writev((int)a, (const iovec_t*)b, (int)c); break;
        case SYS_open:     r = sys_open((const char*)a, (int)b, c); break;
        case SYS_access:
            /* The dynamic linker probes with access() before it opens, so
             * answering this is what lets it find a library at all. */
            r = sys_access((const char*)a, (int)b);
            break;
        case SYS_faccessat:
            r = sys_access((const char*)b, (int)c);
            break;
        case SYS_faccessat2:
            r = sys_access((const char*)b, (int)c);
            break;

        case SYS_openat:
            /* (dirfd, path, flags, mode). There is no per-process working
             * directory, so a relative path resolves from the root and
             * dirfd carries no information - which is exactly right for
             * AT_FDCWD, the only value glibc passes. */
            r = sys_open((const char*)b, (int)c, d);
            break;

        /* --- the writable half (Milestone 26) ------------------------- */
        case SYS_unlink:   r = sys_unlink((const char*)a, 0); break;
        case SYS_rmdir:    r = sys_unlink((const char*)a, 1); break;
        case SYS_mkdir:    r = sys_mkdir((const char*)a); break;
        case SYS_unlinkat:
            /* (dirfd, path, flags). AT_REMOVEDIR is 0x200 and is how
             * glibc's rmdir() reaches this. */
            r = sys_unlink((const char*)b, (c & 0x200) ? 1 : 0);
            break;
        case SYS_mkdirat:  r = sys_mkdir((const char*)b); break;
        case SYS_rename:   r = vfs_rename((const char*)a, (const char*)b); break;
        case SYS_renameat: r = vfs_rename((const char*)b, (const char*)d); break;
        case SYS_renameat2:
            /* (olddirfd, old, newdirfd, new, flags). glibc's rename()
             * uses this with flags 0; the RENAME_NOREPLACE/EXCHANGE forms
             * are refused rather than silently treated as a plain rename,
             * and glibc falls back cleanly. */
            if (e) { r = -EINVAL; break; }
            r = vfs_rename((const char*)b, (const char*)d);
            break;
        case SYS_ftruncate64:
            /* (fd, length_lo, length_hi) - the 64-bit length arrives as a
             * register pair, and anything needing the high half is far
             * beyond what fits in RAM here. */
            if (c) { r = -EINVAL; break; }
            r = sys_ftruncate((int)a, b);
            break;
        case SYS_truncate64: {
            if (c) { r = -EINVAL; break; }
            vfs_node_t* t = vfs_lookup((const char*)a);
            r = t ? vfs_truncate(t, b) : -ENOENT;
            break;
        }
        case SYS_getdents64:
            r = sys_getdents64((int)a, (uint8_t*)b, c);
            break;
        case SYS_fsync:
            /* There is nothing behind the filesystem to flush to. */
            r = fd_get((int)a) ? 0 : -EBADF;
            break;
        case SYS_chdir:
            /* Accepted for a directory that exists, and then ignored:
             * paths resolve from the root, so honouring it would take a
             * per-process cwd this does not have. Reporting success for a
             * directory that is not there would be worse. */
            r = vfs_lookup((const char*)a) ? 0 : -ENOENT;
            break;
        case SYS_pread64: {
            /* (fd, buf, count, offset_lo, offset_hi) - a positioned read
             * that does not disturb the descriptor's own offset. The
             * dynamic linker reads ELF headers this way. */
            fd_entry_t* e = fd_get((int)a);
            if (!e || e->kind != FD_FILE) { r = -EBADF; break; }
            uint32_t off = d;
            if (off >= e->node->length) { r = 0; break; }
            uint32_t left = e->node->length - off;
            uint32_t n = c < left ? c : left;
            r = (int32_t)vfs_read(e->node, off, n, (uint8_t*)b);
            break;
        }
        case SYS_close:    r = sys_close((int)a); break;
        case SYS_lseek:    r = sys_lseek((int)a, (int32_t)b, (int)c); break;
        case SYS_fstat64:  r = sys_fstat64((int)a, (uint8_t*)b); break;
        case SYS_stat64:   r = sys_stat64((const char*)a, (uint8_t*)b); break;

        case SYS_mmap2:    r = sys_mmap2(a, b, (int)c, (int)d, (int)e, f); break;
        case SYS_munmap:   r = sys_munmap(a, b); break;
        case SYS_mprotect: r = sys_mprotect(a, b, (int)c); break;
        case SYS_brk:      r = sys_brk(a); break;

        case SYS_getpid:   r = 0x100; break;
        case SYS_gettid:   r = scheduler_current_pid() ? scheduler_current_pid()
                                                       : 0x100; break;
        case SYS_getuid32: case SYS_geteuid32:
        case SYS_getgid32: case SYS_getegid32: r = 0; break;

        case SYS_uname:    r = sys_uname((uint8_t*)a); break;

        /* --- what a real glibc asks for on the way to main() ------------
         * Every one of these was found by running an actual
         * glibc-linked binary and reading the -ENOSYS reports it
         * provoked, rather than guessed at from a list. */

        case SYS_ugetrlimit: {
            /* struct rlimit { rlim_cur, rlim_max }. glibc reads RLIMIT_STACK
             * (resource 3) to size its own guard pages. */
            uint32_t* lim = (uint32_t*)b;
            if (!lim) { r = -EFAULT; break; }
            lim[0] = (a == 3) ? ELF_STACK_BYTES : 0xFFFFFFFFu;
            lim[1] = lim[0];
            r = 0;
            break;
        }

        case SYS_getrandom: {
            /* Not cryptographic - the PIT tick is the only entropy here -
             * and saying so matters, because glibc seeds pointer mangling
             * from it. Enough to be *different* per call, not enough to be
             * unguessable. */
            uint8_t* buf = (uint8_t*)a;
            if (!buf) { r = -EFAULT; break; }
            static uint32_t seed = 0;
            if (!seed) seed = pit_get_ticks() | 1u;
            for (uint32_t i = 0; i < b; i++) {
                seed = seed * 1103515245u + 12345u;
                buf[i] = (uint8_t)(seed >> 16);
            }
            r = (int32_t)b;
            break;
        }

        case SYS_clock_gettime: {
            /* struct timespec { tv_sec, tv_nsec }, 100Hz resolution. */
            uint32_t* ts = (uint32_t*)b;
            if (!ts) { r = -EFAULT; break; }
            uint32_t ticks = pit_get_ticks();
            ts[0] = rtc_unix_time();
            ts[1] = (ticks % 100u) * 10000000u;
            r = 0;
            break;
        }

        case SYS_clock_gettime64: {
            /* Same, with a 64-bit tv_sec - what a 32-bit glibc built past
             * the 2038 transition calls first. */
            uint32_t* ts = (uint32_t*)b;
            if (!ts) { r = -EFAULT; break; }
            uint32_t ticks = pit_get_ticks();
            ts[0] = rtc_unix_time();
            ts[1] = 0;
            ts[2] = (ticks % 100u) * 10000000u;
            ts[3] = 0;
            r = 0;
            break;
        }

        /* Accepted and ignored: robust-futex bookkeeping for a process
         * that never dies holding one, and restartable sequences, which
         * glibc treats as optional and falls back from cleanly. */
        case SYS_set_robust_list: r = 0; break;
        case SYS_rseq:            r = -ENOSYS; break;

        /* Genuinely absent, and answered rather than reported: there are
         * no symlinks and no statx, and glibc falls back to fstat64. */
        case SYS_fstatat64:
            /* (dirfd, path, statbuf, flags). AT_EMPTY_PATH (0x1000) means
             * "stat the descriptor itself", which is how glibc's fstat is
             * routed on a kernel new enough to have this; otherwise the
             * path is relative to a directory, and the initrd is flat so
             * there is only one. */
            if (d & 0x1000u) {
                r = sys_fstat64((int)a, (uint8_t*)c);
            } else {
                r = sys_stat64((const char*)b, (uint8_t*)c);
            }
            break;

        case SYS_readlinkat: r = -EINVAL; break;
        case SYS_statx:      r = -ENOSYS; break;
        case SYS_time:     r = (int32_t)rtc_unix_time(); break;
        case SYS_nanosleep: r = sys_nanosleep((const uint32_t*)a); break;

        /* Real since Milestone 19 - see kernel/posix_signal.c. */
        case SYS_rt_sigaction:
            r = posix_sys_rt_sigaction((int)a, (const k_sigaction_t*)b,
                                       (k_sigaction_t*)c, d);
            break;
        case SYS_rt_sigprocmask:
            r = posix_sys_rt_sigprocmask((int)a, (const uint32_t*)b,
                                         (uint32_t*)c, d);
            break;
        case SYS_rt_sigreturn:
            /* Restores the interrupted context wholesale, so eax is set
             * by the restore rather than by the usual `regs->eax = r`
             * below - returning here would overwrite it. */
            posix_sys_rt_sigreturn(regs);
            return;

        case SYS_kill:
        case SYS_tgkill:
            /* One process, so the pid is only checked for sanity; the
             * signal number is the last argument in both. The two differ
             * in the si_code an SA_SIGINFO handler sees, which is how a
             * program tells a targeted thread signal from a process one. */
            if (n == SYS_kill) {
                r = posix_raise_from((int)b, SI_USER, (int32_t)a);
            } else {
                r = posix_raise_from((int)c, SI_TKILL, (int32_t)a);
            }
            break;

        /* Real since Milestone 20 - see kernel/posix_thread.c. */
        case SYS_set_thread_area:
            r = posix_sys_set_thread_area(a);
            break;
        case SYS_clone:
            r = posix_sys_clone(a, b, (uint32_t*)c, d, (uint32_t*)e, regs);
            break;
        case SYS_futex:
            r = posix_sys_futex(a, (int)b, c, d, regs);
            break;
        case SYS_set_tid_address:
            /* glibc calls this at startup to register where to clear the
             * tid. One process, so the value is simply reported back. */
            r = scheduler_current_pid() ? scheduler_current_pid() : 0x100;
            break;

        /* isatty() is ioctl(TCGETS) under the covers; saying "yes, a
         * terminal" for the console streams and "no" otherwise is enough
         * for a libc to pick line buffering. */
        case SYS_ioctl:
            r = (a < 3) ? 0 : -EINVAL;
            break;

        case SYS_exit:
            if (trace_on) trace_exit(0);
            /* A thread exiting has to clear the word its creator asked
             * for, or a joiner waits for ever. exit_group takes the whole
             * program with it, so it does not. */
            posix_thread_exiting();
            /* fall through */
        case SYS_exit_group:
            /* Same two paths sys_exit always had: into the scheduler's
             * ready queue when one is running, or back to the kernel. */
            if (scheduler_is_active()) {
                scheduler_exit_current();
            } else {
                process_exit_to_kernel(); /* does not return */
            }
            return; /* eax is not written: this task is finished */

        default:
            report_unimplemented(n);
            r = -ENOSYS;
            break;
    }

    /* An implementation can ask for the whole call to happen again rather
     * than return - futex(FUTEX_WAIT) does. Restoring eax matters: the
     * syscall number lives there and would otherwise be replaced by this
     * call's result, so the re-executed `int $0x80` would dispatch to
     * whatever that value happened to name. `int $0x80` is two bytes. */
    if (posix_retry_pending()) {
        if (trace_on) { terminal_writestring(" = <retry>\n"); }
        regs->eax = n;
        regs->eip -= 2;
        scheduler_yield_from_trap(regs);
        return;
    }

    if (trace_on) trace_exit(r);
    regs->eax = (uint32_t)r;
}
