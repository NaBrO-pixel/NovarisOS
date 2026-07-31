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
        if (paging_get_entry(va) & PAGE_PRESENT) continue; /* already there */
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
    if (!(flags & MAP_ANONYMOUS) || fd >= 0) {
        /* A file-backed request is refused rather than quietly returning
         * zeroed memory, which would corrupt whatever read it. */
        return -ENOSYS;
    }

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

static int32_t sys_open(const char* path, int flags) {
    if (!path) return -EFAULT;
    if ((flags & O_WRONLY) || (flags & O_RDWR) || (flags & O_CREAT)) {
        return -EROFS; /* the initrd is read-only */
    }

    /* Leading "./" and "/" are both accepted, since the initrd is flat and
     * a program written for Linux will happily pass either. */
    const char* name = path;
    if (name[0] == '/') name++;
    if (name[0] == '.' && name[1] == '/') name += 2;

    vfs_node_t* node = vfs_root ? vfs_finddir(vfs_root, name) : 0;
    if (!node) return -ENOENT;

    int fd = fd_alloc();
    if (fd < 0) return -EMFILE;

    fds[fd].kind = FD_FILE;
    fds[fd].node = node;
    fds[fd].offset = 0;
    fds[fd].writable = 0;
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
    if (e->kind != FD_CONSOLE) return -EROFS;

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
#define ST_SIZE_OFFSET  44   /* st_size, 64-bit, in i386's struct stat64 */
#define ST_MODE_OFFSET  16
#define S_IFREG 0100000
#define S_IFCHR 0020000

static int32_t stat_common(uint8_t* st, uint32_t size, uint32_t mode) {
    if (!st) return -EFAULT;
    kmemset(st, 0, 96);
    *(uint32_t*)(st + ST_MODE_OFFSET) = mode | 0444;
    *(uint64_t*)(st + ST_SIZE_OFFSET) = size;
    return 0;
}

static int32_t sys_fstat64(int fd, uint8_t* st) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (e->kind == FD_CONSOLE) return stat_common(st, 0, S_IFCHR);
    return stat_common(st, e->node->length, S_IFREG);
}

static int32_t sys_stat64(const char* path, uint8_t* st) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return fd;
    int32_t r = sys_fstat64(fd, st);
    sys_close(fd);
    return r;
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

void posix_syscall(registers_t* regs) {
    uint32_t n = regs->eax;
    uint32_t a = regs->ebx, b = regs->ecx, c = regs->edx;
    uint32_t d = regs->esi, e = regs->edi, f = regs->ebp;
    int32_t r;

    switch (n) {
        case SYS_write:    r = sys_write((int)a, (const char*)b, c); break;
        case SYS_read:     r = sys_read((int)a, (char*)b, c); break;
        case SYS_writev:   r = sys_writev((int)a, (const iovec_t*)b, (int)c); break;
        case SYS_open:     r = sys_open((const char*)a, (int)b); break;
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
             * signal number is the last argument in both. */
            r = posix_raise((int)(n == SYS_kill ? b : c));
            break;

        /* Still accepted and ignored: TLS setup, which needs per-thread
         * GDT entries the POSIX side does not have yet. */
        case SYS_set_thread_area:
            r = 0;
            break;

        /* isatty() is ioctl(TCGETS) under the covers; saying "yes, a
         * terminal" for the console streams and "no" otherwise is enough
         * for a libc to pick line buffering. */
        case SYS_ioctl:
            r = (a < 3) ? 0 : -EINVAL;
            break;

        case SYS_exit:
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

    regs->eax = (uint32_t)r;
}
