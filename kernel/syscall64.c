/* syscall64.c - the MSRs that make SYSCALL work, and what it dispatches to.
 *
 * This is the bring-up half of Milestone 44's item 4, and it is worth
 * being clear about what it is not. Novaris's 32-bit kernel implements
 * *Linux's i386 syscall ABI* - the numbers, the register convention and
 * the structure layouts - and that is precisely what lets real glibc and
 * real Wine run on it unmodified. None of that transfers to x86-64, which
 * has different numbers, a different register convention and different
 * structures. What is here is the mechanism: ring 3 is reachable, a
 * syscall arrives in C, and a value comes back. The ABI on top of it is
 * still to be written. */

#include "syscall64.h"
#include "gdt64.h"
#include "serial64.h"
#include "uspace64.h"
#include "win32_64.h"
#include "sched64.h"
#include "signal64.h"
#include "ramfs64.h"
#include "paging64.h"
#include "kstring.h"
#include "proc64.h"
#include "elf64.h"
#include "initrd64.h"
#include "pmm64.h"
#include "fb64.h"

#define IA32_EFER   0xC0000080u
#define IA32_STAR   0xC0000081u
#define IA32_LSTAR  0xC0000082u
#define IA32_FMASK  0xC0000084u

#define EFER_SCE    (1ULL << 0)

extern void syscall64_entry(void);

static uint64_t call_count;
static uint64_t last_arg;
static uint64_t exit_code;

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ __volatile__("wrmsr"
                         :: "c"(msr),
                            "a"((uint32_t)value),
                            "d"((uint32_t)(value >> 32)));
}

void syscall64_init(void) {
    uint64_t star;

    call_count = last_arg = exit_code = 0;

    /* Without SCE the instruction raises #UD rather than doing anything,
     * and the fault looks like a bad opcode rather than a missing bit. */
    write_msr(IA32_EFER, read_msr(IA32_EFER) | EFER_SCE);

    /* STAR[47:32] is the SYSCALL base, STAR[63:48] the SYSRET base. See
     * syscall64.h for how each expands into a CS/SS pair; the values are
     * 0x08 and 0x10 because of how gdt64.c orders its descriptors, and
     * changing that order silently changes which selectors land here. */
    star = ((uint64_t)GDT64_KCODE_SEL << 32)
         | ((uint64_t)GDT64_KDATA_SEL << 48);
    write_msr(IA32_STAR, star);

    write_msr(IA32_LSTAR, (uint64_t)&syscall64_entry);

    /* Bits set here are *cleared* in RFLAGS on entry. IF, so the handler
     * does not run with interrupts on while it is still sorting out its
     * stack; DF, so the string instructions in any C it calls count
     * upwards, which the SysV ABI requires and a ring-3 program is under
     * no obligation to leave true; TF, so a single-stepping debugger in
     * ring 3 does not trap inside the kernel. */
    write_msr(IA32_FMASK, (1ULL << 9) | (1ULL << 10) | (1ULL << 8));
}

static uint64_t bytes_written;

uint64_t syscall64_bytes_written(void) { return bytes_written; }

/* The number of the last call that had no implementation, and how many
 * there were. Discovering what a real program needs is done by running
 * it and reading these, since this kernel has no strace. */
static uint64_t unimpl_count, last_unimpl;

/* How many threads ended through exit(2) without ending the process. */
static uint64_t thread_exits;

/* syscall64.s records the caller's ring-3 stack here on every entry, and
 * it is the only place that value exists - the frame the stub builds
 * describes the registers but not the stack they came off. */
extern uint64_t saved_user_rsp;

/* Open files belong to the process, not to the kernel. Until Milestone
 * 64 this was a static table, which is indistinguishable from correct
 * while there is one program and wrong the moment a fork produces two.
 * 0-2 are never allocated, so stdin/stdout/stderr keep meaning what
 * they mean. */
#define FD_MAX PROC64_FD_MAX
#define fds    (proc64_current()->fds)

void syscall64_reset_files(void) {
    /* Through the macro rather than a local pointer: `fds` expands to a
     * member access, so `p->fds` would expand inside itself. */
    if (!proc64_current()) return;
    for (int i = 0; i < FD_MAX; i++) fds[i].used = 0;
}

/* What /proc/self/exe resolves to. Per process, because execve replaces
 * it and a child must not answer with its parent's path. */
void syscall64_set_exe_path(const char* path) {
    proc64_t* p = proc64_current();
    if (!p) return;
    kstrlcpy(p->exe_path, path, PROC64_PATH_MAX);
}

/* Every path a program hands this kernel goes through here first.
 *
 * Until Milestone 68 a relative path was answered -ENOENT, and the
 * comment on openat said why: there was no working directory to measure
 * one against. That is not an obscure gap - `configure`, `ld.so`, make,
 * and Wine all spend most of their path handling relative to where they
 * are, and Wine's very first act on a prefix is to chdir into it.
 *
 * The result is absolute but not textually normalised: "." , ".." and
 * doubled slashes are left for ramfs64's walker, which has to handle
 * them anyway because an absolute path can contain them too. Resolving
 * them here as text would also be wrong - ".." after a symlink is not
 * the parent directory of the link.
 *
 * Returns 0, or a negative errno.
 */
static int64_t abs_path(const char* in, char* out) {
    proc64_t* p = proc64_current();
    uint64_t n = 0, i;

    if (!in) return -14;                               /* -EFAULT */
    if (!in[0]) return -2;                             /* -ENOENT */

    if (in[0] == '/') {
        for (i = 0; in[i]; i++) {
            if (n + 1 >= PROC64_PATH_MAX) return -36;  /* -ENAMETOOLONG */
            out[n++] = in[i];
        }
        out[n] = 0;
        return 0;
    }

    if (!p) return -2;

    for (i = 0; p->cwd[i]; i++) {
        if (n + 1 >= PROC64_PATH_MAX) return -36;
        out[n++] = p->cwd[i];
    }
    /* "/" already ends in one; anything else needs the separator. */
    if (n == 0 || out[n - 1] != '/') {
        if (n + 1 >= PROC64_PATH_MAX) return -36;
        out[n++] = '/';
    }
    for (i = 0; in[i]; i++) {
        if (n + 1 >= PROC64_PATH_MAX) return -36;
        out[n++] = in[i];
    }
    out[n] = 0;
    return 0;
}

static uint64_t do_open(const char* path, uint64_t flags) {
    int node = ramfs64_lookup(path);
    int fd;

    if (node < 0) {
        if (!(flags & O_CREAT)) return (uint64_t)-2;   /* -ENOENT */
        node = ramfs64_create(path, 0);
        if (node < 0) return (uint64_t)-28;            /* -ENOSPC */
    } else if (flags & O_TRUNC) {
        ramfs64_truncate(node);
    }

    for (fd = 3; fd < FD_MAX; fd++) if (!fds[fd].used) break;
    if (fd == FD_MAX) return (uint64_t)-24;            /* -EMFILE */

    fds[fd].node = node;
    fds[fd].pos  = (flags & O_APPEND) ? ramfs64_size(node) : 0;
    fds[fd].used = 1;
    return (uint64_t)fd;
}

/* st_dev and st_ino, and they are not decoration.
 *
 * ld.so identifies an already-loaded object by the (device, inode) pair
 * it gets back from fstat, so that opening the same library twice by two
 * different paths does not map it twice. Reporting 0/0 for every file
 * makes every file look like the same file: ld.so opens libc, compares
 * it against the main executable it already has, decides they are the
 * same object, and closes it without mapping. What that looks like from
 * outside is "undefined symbol: __libc_start_main" - a symbol error, a
 * long way from the stat that caused it.
 *
 * The node index is a perfectly good inode number: it is unique per file
 * and stable for as long as the file exists. */
static void fill_ids(uint8_t* st, int node) {
    *(uint64_t*)(st + 0) = 1;                          /* st_dev */
    *(uint64_t*)(st + 8) = (uint64_t)node + 1;         /* st_ino */
    *(uint64_t*)(st + 16) = 1;                         /* st_nlink */
}

/* struct stat, x86-64. Only the fields anything here reads are filled;
 * the offsets are the kernel's and must not be tidied.
 *   0 st_dev  8 st_ino  16 st_nlink  24 st_mode  48 st_size  56 st_blksize */
static uint64_t do_stat(const char* path, void* out, int follow) {
    int node = follow ? ramfs64_lookup(path)
                      : ramfs64_lookup_nofollow(path);
    uint8_t* st = (uint8_t*)out;

    if (node < 0) return (uint64_t)-2;                 /* -ENOENT */
    for (int i = 0; i < 144; i++) st[i] = 0;
    fill_ids(st, node);

    /* S_IFDIR, S_IFLNK or S_IFREG. ld.so checks this to decide whether a
     * path it is about to search is a directory at all, and anything
     * walking a tree checks it to decide whether to recurse - an lstat
     * that reported a symlink as an ordinary file would send `cp -r`
     * and Wine's prefix updater into the target instead of copying the
     * link. */
    *(uint32_t*)(st + 24) = ramfs64_is_dir(node)  ? 0040755u
                          : ramfs64_is_link(node) ? 0120777u
                                                  : 0100644u;
    *(uint64_t*)(st + 48) = ramfs64_size(node);
    *(uint64_t*)(st + 56) = 4096;
    return 0;
}

/* readlink(path, buf, bufsiz).
 *
 * Two kinds of answer. /proc/self/exe is synthesised - there is no /proc
 * here, so the path is remembered when the program is loaded - and it
 * earns its place because it is how a program finds out where it was
 * installed: Wine uses it to locate its own lib directory, and without
 * it Wine computes the path to ntdll.so as (null) and stops.
 *
 * The other kind is a real symlink, which this filesystem has had since
 * Milestone 68. A prefix is held together with them: dosdevices/c: is a
 * link to ../drive_c, and Wine reaches drive C by reading it.
 *
 * readlink does not NUL-terminate, and a caller that assumed it did
 * would read past what it was given.
 */
static uint64_t do_readlink(const char* given, char* buf, uint64_t size) {
    char path[PROC64_PATH_MAX];
    proc64_t* me = proc64_current();
    const char* target;
    uint64_t n;
    int node;
    int64_t e;

    if (!buf) return (uint64_t)-14;                    /* -EFAULT */

    if (kstrcmp(given, "/proc/self/exe") == 0) {
        if (!me || !me->exe_path[0]) return (uint64_t)-2;
        target = me->exe_path;
    } else {
        e = abs_path(given, path);
        if (e) return (uint64_t)e;

        node = ramfs64_lookup_nofollow(path);
        if (node < 0) return (uint64_t)-2;             /* -ENOENT */

        target = ramfs64_readlink(node);
        /* Not a symlink. -EINVAL, which is how a caller tells "there is
         * nothing here" from "the thing here is not a link". */
        if (!target) return (uint64_t)-22;
    }

    n = kstrlen(target);
    if (n > size) n = size;
    kmemcpy(buf, target, n);
    return n;
}

static uint64_t forks, execs;
static uint64_t last_fork_frames;

static void frame_from_args(const syscall64_args_t* args, uint64_t rax,
                            registers64_t* out);

/* fork(2). The child is the parent with a different address space, a
 * different pid, and 0 where the parent gets the child's pid.
 *
 * Reached from two syscalls: SYS_fork, and SYS_clone without CLONE_VM,
 * which is what glibc's fork() actually issues.
 *
 * Ordering matters: the address space is the part that can fail, so it
 * happens before anything is committed. A half-built process is worse
 * than a failed fork.
 */
static uint64_t do_fork(const syscall64_args_t* args) {
    registers64_t child;
    int child_pid;
    proc64_t* cp;
    /* What the fork itself cost, in frames. This is the only place the
     * difference between sharing and copying is visible: both produce a
     * correct child, and only one of them charges the resident size of
     * the process for it. */
    uint64_t free_before = pmm64_free_frames();

    child_pid = proc64_fork_from(proc64_current_pid());
    if (child_pid < 0) return (uint64_t)-11;           /* -EAGAIN */
    cp = proc64_get(child_pid);

    if (!vmspace64_create(&cp->space)) {
        proc64_exit(child_pid, 0);
        proc64_reap_child(proc64_current_pid(), 0);
        return (uint64_t)-12;                          /* -ENOMEM */
    }
    /* Shared, not copied (Milestone 69). The eager clone is still there
     * and still correct; it cost the whole resident size of the process
     * and stopped at 16MB, which is not a fork anything the size of
     * Wine can use. */
    if (!vmspace64_clone_cow(vmspace64_current_phys(), &cp->space)) {
        vmspace64_destroy(&cp->space);
        proc64_exit(child_pid, 0);
        proc64_reap_child(proc64_current_pid(), 0);
        return (uint64_t)-12;
    }

    /* The child resumes exactly where the parent is about to, with a 0
     * in rax - which is the whole of how the two tell each other
     * apart. */
    frame_from_args(args, 0, &child);

    /* The child inherits the parent's thread pointer.
     *
     * Passing 0 here - "a task that has never run has no TLS yet" - is
     * right for a brand new program, which will call arch_prctl before
     * it touches TLS, and wrong for a fork, which is a copy of a
     * process that already did. The child comes back from clone inside
     * glibc, whose very next act is to read the thread descriptor at
     * fs:0x10; with FS_BASE at 0 that is a read of linear address 0x10
     * and the child dies on a null dereference that has nothing
     * visibly to do with fork. */
    if (sched64_add_frame_for(&child, &cp->space,
                              read_msr(0xC0000100u), child_pid) < 0) {
        vmspace64_destroy(&cp->space);
        return (uint64_t)-11;
    }
    forks++;
    {
        uint64_t now = pmm64_free_frames();
        last_fork_frames = free_before > now ? free_before - now : 0;
    }
    return (uint64_t)child_pid;
}

uint64_t syscall64_forks(void) { return forks; }
uint64_t syscall64_execs(void) { return execs; }
uint64_t syscall64_last_fork_frames(void) { return last_fork_frames; }

/* The calling thread's complete user state, as a frame it could be
 * resumed from, with `rax` set to what the syscall will return.
 *
 * This is what makes a blocking syscall possible without a per-task
 * kernel stack: a thread that blocks does not leave a half-finished
 * kernel call behind, it leaves *this*, and waking it is resuming it. */
static void frame_from_args(const syscall64_args_t* args, uint64_t rax,
                            registers64_t* out) {
    out->rax = rax;
    out->rbx = args->rbx;
    out->rcx = 0;              /* SYSCALL destroyed it on the way in */
    out->rdx = args->a3;
    out->rsi = args->a2;
    out->rdi = args->a1;
    out->rbp = args->rbp;
    out->r8  = args->a5;
    out->r9  = args->a6;
    out->r10 = args->a4;
    out->r11 = 0;
    out->r12 = args->r12;
    out->r13 = args->r13;
    out->r14 = args->r14;
    out->r15 = args->r15;
    out->int_no   = 0;
    out->err_code = 0;
    out->rip    = args->ret_rip;
    out->cs     = 0x23;
    out->rflags = args->ret_rflags | 0x200;
    out->rsp    = saved_user_rsp;
    out->ss     = 0x1B;
}

uint64_t syscall64_thread_exits(void) { return thread_exits; }

/* Used when the kernel ends a program itself - a fault with no handler -
 * so that the status a test reads is the one the kernel decided on
 * rather than whatever the last program to exit left behind. */
void syscall64_set_exit_code(uint64_t code) { exit_code = code; }

/* Threads that actually slept, and wakeups that actually woke one.
 * Counted because "the futex worked" and "the futex was never contended"
 * look identical from outside, and only the first is worth claiming. */
static uint64_t futex_waits, futex_wakes;

/* File-backed mappings made. Counted for the same reason the futex
 * counters are: a program can be handed a correct-looking pointer by
 * the anonymous path and never notice the file was not involved. */
static uint64_t file_maps;

uint64_t syscall64_file_maps(void) { return file_maps; }

uint64_t syscall64_futex_waits(void) { return futex_waits; }
uint64_t syscall64_futex_wakes(void) { return futex_wakes; }

uint64_t syscall64_unimplemented(void) { return last_unimpl; }
uint64_t syscall64_unimplemented_count(void) { return unimpl_count; }

/* Off by default: it prints on every call, so leaving it on would bury
 * the program's own output in the transcript the tests match against. */
static int trace;

void syscall64_set_trace(int on) { trace = on; }

static uint64_t dispatch(syscall64_args_t* args);

uint64_t syscall64_dispatch(syscall64_args_t* args) {
    uint64_t r;

    if (!trace) return dispatch(args);

    serial64_puts("NOVARIS64: [call] ");
    serial64_putdec(args->nr);

    /* Path-taking calls print their path. Without this a trace of a
     * dynamic loader is a wall of pointers, and the whole question -
     * which file could it not find - is the one thing it does not
     * answer. The argument holding the path differs per call. */
    if (args->nr == SYS64_OPEN || args->nr == SYS64_ACCESS ||
        args->nr == SYS64_STAT || args->nr == SYS64_UNLINK ||
        args->nr == SYS64_LSTAT || args->nr == SYS64_CHDIR ||
        args->nr == SYS64_MKDIR || args->nr == SYS64_RMDIR ||
        args->nr == SYS64_READLINK) {
        serial64_puts(" \"");
        serial64_puts((const char*)args->a1);
        serial64_puts("\"");
    } else if (args->nr == SYS64_OPENAT || args->nr == SYS64_NEWFSTATAT ||
               args->nr == SYS64_READLINKAT || args->nr == SYS64_UNLINKAT ||
               args->nr == SYS64_MKDIRAT) {
        serial64_puts(" \"");
        serial64_puts((const char*)args->a2);
        serial64_puts("\"");
    }

    serial64_puts("(");
    serial64_puthex(args->a1);
    serial64_puts(", ");
    serial64_puthex(args->a2);
    serial64_puts(", ");
    serial64_puthex(args->a3);
    /* mmap's interesting arguments are the ones past the third: whether
     * it is anonymous, which file, and at what offset. A trace of a
     * loader without them cannot be compared to what the ELF says. */
    if (args->nr == SYS64_MMAP) {
        serial64_puts(" flags=");
        serial64_puthex(args->a4);
        serial64_puts(" fd=");
        serial64_putdec(args->a5);
        serial64_puts(" off=");
        serial64_puthex(args->a6);
    }

    serial64_puts(")");
    r = dispatch(args);
    serial64_puts(" = ");
    serial64_puthex(r);
    serial64_putc('\n');
    return r;
}

/* Called from syscall64_entry with a pointer to the pushed arguments. */
/* execve(2).
 *
 * The hard part is that there is no going back. Once the old address
 * space is gone the caller's stack, code and arguments are gone with
 * it - including the strings execve was passed - so everything needed
 * from the old process is copied out first, and the new space is built
 * before the old one is discarded. A failure after that point cannot
 * return an error to a caller that no longer exists.
 */
#define EXECVE_MAX_ARGS 8

static uint64_t do_execve(const char* path, const char* const* argv,
                          const char* const* envp,
                          const syscall64_args_t* args) {
    /* Copied into the kernel while the old space still exists. */
    static char  kpath[PROC64_PATH_MAX];
    static char  kargv_store[EXECVE_MAX_ARGS][128];
    static char  kenvp_store[EXECVE_MAX_ARGS][128];
    static const char* kargv[EXECVE_MAX_ARGS + 1];
    static const char* kenvp[EXECVE_MAX_ARGS + 1];
    const uint64_t STACK_TOP   = 0x00007FFFFFFF0000ULL;
    const uint64_t STACK_PAGES = 64;
    const void* image;
    uint64_t len, rsp;
    vmspace64_t fresh;
    elf64_info_t info;
    registers64_t entry;
    proc64_t* p = proc64_current();
    int nargv = 0, nenvp = 0;

    if (!p) return (uint64_t)-1;
    (void)args;

    /* Made absolute against the working directory here, so that
     * execve("./configure") works and so that /proc/self/exe answers
     * with a path that still means something after a chdir. */
    if (abs_path(path, kpath) != 0) return (uint64_t)-2;
    for (; argv && argv[nargv] && nargv < EXECVE_MAX_ARGS; nargv++) {
        kstrlcpy(kargv_store[nargv], argv[nargv], sizeof(kargv_store[0]));
        kargv[nargv] = kargv_store[nargv];
    }
    kargv[nargv] = 0;
    for (; envp && envp[nenvp] && nenvp < EXECVE_MAX_ARGS; nenvp++) {
        kstrlcpy(kenvp_store[nenvp], envp[nenvp], sizeof(kenvp_store[0]));
        kenvp[nenvp] = kenvp_store[nenvp];
    }
    kenvp[nenvp] = 0;

    /* The path is looked up before anything is torn down, so a missing
     * program is an ordinary -ENOENT rather than a dead process. From
     * the filesystem rather than the initrd, so a program that was
     * written at run time is as executable as one that shipped. */
    {
        int node = ramfs64_lookup(kpath);
        if (node < 0) return (uint64_t)-2;             /* -ENOENT */
        image = ramfs64_data(node);
        len   = ramfs64_size(node);
        if (!image || !len) return (uint64_t)-8;       /* -ENOEXEC */
    }

    if (!vmspace64_create(&fresh)) return (uint64_t)-12;
    if (elf64_load(image, len, &fresh, &info) != ELF64_OK) {
        vmspace64_destroy(&fresh);
        return (uint64_t)-8;                           /* -ENOEXEC */
    }

    for (uint64_t i = 0; i < STACK_PAGES; i++) {
        uint64_t f = pmm64_alloc_frame();
        if (!f || vmspace64_map(&fresh, STACK_TOP - (i + 1) * PAGE64_SIZE,
                                f, PAGE64_PRESENT | PAGE64_WRITE |
                                PAGE64_USER) != PAGING64_OK) {
            vmspace64_destroy(&fresh);
            return (uint64_t)-12;
        }
    }

    /* Past this line the old process is being replaced, and there is
     * nothing left to return an error to. */
    p->space = fresh;
    proc64_set_current(p->pid);
    p->brk_base = p->brk_current = info.brk_start;
    p->mmap_next = USPACE64_MMAP_BASE;
    kstrlcpy(p->exe_path, kpath, PROC64_PATH_MAX);

    rsp = uspace64_build_stack(&fresh, STACK_TOP, STACK_PAGES,
                               kargv, &info, 0, kenvp);

    for (uint64_t i = 0; i < sizeof(entry) / 8; i++)
        ((uint64_t*)&entry)[i] = 0;
    entry.rip    = info.entry;
    entry.rsp    = rsp;
    entry.cs     = 0x23;
    entry.ss     = 0x1B;
    entry.rflags = 0x202;

    sched64_set_current_space(&fresh);
    execs++;
    vmspace64_switch(&fresh);
    sched64_resume(&entry);                            /* never returns */
    return 0;
}

static uint64_t dispatch(syscall64_args_t* args) {
    uint64_t nr = args->nr;
    uint64_t a1 = args->a1, a2 = args->a2, a3 = args->a3;

    call_count++;

    /* A Win32 import, arriving through one of pe64.c's thunks. Checked
     * before the switch because the range is contiguous and has nothing
     * to do with Linux's numbering. */
    if (nr >= WIN32_64_BASE && nr <= WIN32_64_EXIT) {
        if (nr == WIN32_64_EXIT) {
            exit_code = a1;
            return a1;
        }
        return win32_64_call(nr, a1, a2, a3, args->a4);
    }

    switch (nr) {
    case SYS64_WRITE: {
        /* write(fd, buf, count). `buf` is a ring-3 pointer, and it is
         * dereferenced directly because the kernel is running in the
         * caller's address space - the high half is shared, so a syscall
         * never had to leave the space it was called from.
         *
         * It is also completely unchecked, which is the honest state of
         * this ABI: a real implementation validates that the range is
         * mapped and belongs to the caller before touching it, and does
         * so without racing the caller. Nothing here does that yet. */
        const char* buf = (const char*)a2;
        uint64_t n = a3;

        /* A real file descriptor goes to the filesystem; 1 and 2 are the
         * serial port. Nothing here has a descriptor table entry for
         * stdout, so the two cases are told apart by number. */
        if (a1 >= 3) {
            int64_t w;
            if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
            w = ramfs64_write(fds[a1].node, fds[a1].pos, buf, n);
            if (w > 0) fds[a1].pos += (uint64_t)w;
            return (uint64_t)w;
        }

        if (a1 != 1 && a1 != 2) return (uint64_t)-9;   /* -EBADF */
        if (a1 != 1 && a1 != 2) return (uint64_t)-1;   /* stdout/stderr */
        for (uint64_t i = 0; i < n; i++) serial64_putc(buf[i]);
        bytes_written += n;
        return n;
    }
    /* writev(fd, iov, iovcnt) - glibc's stdio uses this rather than
     * write() as soon as it has a buffer to flush. */
    case SYS64_WRITEV: {
        const struct { const char* base; uint64_t len; }* iov =
            (const void*)a2;
        uint64_t total = 0;
        if (a1 != 1 && a1 != 2) return (uint64_t)-9;   /* -EBADF */
        for (uint64_t i = 0; i < a3; i++) {
            for (uint64_t j = 0; j < iov[i].len; j++)
                serial64_putc(iov[i].base[j]);
            total += iov[i].len;
        }
        bytes_written += total;
        return total;
    }

    case SYS64_BRK:
        return uspace64_brk(a1);

    /* mmap(addr, length, prot, flags, fd, offset) */
    case SYS64_MMAP: {
        uint64_t flags = args->a4;
        uint64_t off   = args->a6;
        int64_t  fd    = (int64_t)args->a5;
        uint64_t mapped;
        int node;

        if (flags & MAP_ANONYMOUS)
            return uspace64_mmap(a1, a2, a3, flags);

        if (fd < 3 || fd >= FD_MAX || !fds[fd].used) return (uint64_t)-9;
        if (off & (PAGE64_SIZE - 1)) return (uint64_t)-22;  /* -EINVAL */
        node = fds[fd].node;

        /* /dev/fb0, decided before the rule below rather than after.
         *
         * This is the one mapping in this kernel that is genuinely
         * shared rather than copied: the whole point of mapping a
         * framebuffer is that the process's stores land on the screen,
         * so MAP_SHARED here is real and MAP_PRIVATE would be useless.
         * Ordering matters and is the bug this comment replaces - with
         * the file rule first, every writable MAP_SHARED was refused
         * before anyone asked what was being mapped, so /dev/fb0 could
         * be opened and described but never mapped.
         *
         * This is what a display driver above the kernel needs: Wine's
         * would open /dev/fb0, mmap it, and composite into it. */
        if (ramfs64_device(node) == RAMFS64_DEV_FB) {
            uint64_t span = fb64_bytes();
            if (!fb64_ready()) return (uint64_t)-19;        /* -ENODEV */
            if (off >= span) return (uint64_t)-22;
            if (a2 > span - off) return (uint64_t)-22;
            return uspace64_map_phys(a2, fb64_phys() + off, a3);
        }

        /* --- an ordinary file mapping ---
         *
         * MAP_PRIVATE is a copy: the pages are the process's own, and
         * writing them does not change the file. That is exactly what a
         * loader wants, and it is what makes this implementable by
         * copying rather than by sharing frames - the filesystem keeps
         * a file's bytes in a heap allocation, which is not page
         * aligned and cannot be mapped directly.
         *
         * MAP_SHARED would have to write back, so it is allowed only
         * where there is nothing to write back: a read-only mapping. */
        if ((flags & MAP_SHARED) && (a3 & PROT_WRITE))
            return (uint64_t)-19;                      /* -ENODEV */

        /* Mapped writable whatever the caller asked for, because the
         * kernel is about to write the file's contents into it. */
        mapped = uspace64_mmap(a1, a2, PROT_READ | PROT_WRITE,
                               flags | MAP_ANONYMOUS);
        if ((int64_t)mapped < 0) return mapped;

        /* The pages arrive zeroed, so a mapping that runs past the end
         * of the file reads as zeros there - which is what Linux does
         * for the tail of the last page. Beyond that last page Linux
         * raises SIGBUS; this does not, and simply keeps reading zeros. */
        {
            uint64_t avail = ramfs64_size(node);
            uint64_t want  = a2;
            if (off < avail) {
                if (off + want > avail) want = avail - off;
                ramfs64_read(node, off, (void*)mapped, want);
            }
        }

        if (!(a3 & PROT_WRITE))
            uspace64_protect(mapped, a2, a3);

        file_maps++;
        return mapped;
    }

    case SYS64_MUNMAP:
        return uspace64_munmap(a1, a2);

    case SYS64_MPROTECT:
        /* Accepted and ignored. Every mapping this kernel makes is
         * already readable and writable by the process that owns it, so
         * the only thing an honest implementation would add here is
         * *removing* permissions - and nothing yet depends on that. */
        return 0;

    /* arch_prctl(ARCH_SET_FS, addr) is how a thread pointer is set on
     * x86-64, and glibc does it before it can touch a single piece of
     * thread-local storage - errno included. Nothing works before this. */
    case SYS64_ARCH_PRCTL:
        switch (a1) {
        case 0x1002:                                   /* ARCH_SET_FS */
            write_msr(0xC0000100u, a2);
            return 0;
        case 0x1001:                                   /* ARCH_SET_GS */
            write_msr(0xC0000101u, a2);
            return 0;
        default:
            return (uint64_t)-22;                      /* -EINVAL */
        }

    /* clone(flags, child_stack, parent_tid, child_tid, tls)
     *
     * Threads only - the CLONE_VM|CLONE_THREAD case. A fork, which is
     * the same syscall without CLONE_VM, would have to copy the address
     * space; nothing here does that, so it is refused rather than
     * silently producing a second thread where a child process was
     * asked for.
     *
     * The child is its parent with three differences, which is exactly
     * what Linux specifies: a different stack, a thread pointer of its
     * own, and rax = 0 so the two can tell each other apart on return.
     * Everything else is inherited, which is why the entry stub saves
     * the callee-saved registers it is otherwise entitled to ignore. */
    case SYS64_CLONE: {
        registers64_t child;
        const vmspace64_t* space;
        int tid;

        /* clone without CLONE_VM is a fork, and this is not a corner
         * case: it is how glibc implements fork(2). There is no
         * fork(2) in glibc's source - it calls
         * clone(CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD) - so a
         * kernel that answers -ENOSYS here has no fork as far as any
         * ordinary C program is concerned, however complete its
         * SYS_fork happens to be. That is exactly what this kernel
         * was: the raw-assembly fork test passed and every glibc
         * program's fork() failed. */
        if (!(a1 & CLONE_VM)) return do_fork(args);
        if (!a2)                 return (uint64_t)-22;  /* -EINVAL       */

        space = sched64_current_space();
        if (!space) return (uint64_t)-38;

        child.rax = 0;            /* how the child knows it is the child */
        child.rbx = args->rbx;
        child.rcx = 0;            /* SYSCALL destroyed it in the parent too */
        child.rdx = a3;
        child.rsi = a2;
        child.rdi = a1;
        child.rbp = args->rbp;
        child.r8  = args->a5;
        child.r9  = args->a6;
        child.r10 = args->a4;
        child.r11 = 0;
        child.r12 = args->r12;
        child.r13 = args->r13;
        child.r14 = args->r14;
        child.r15 = args->r15;

        child.int_no   = 0;
        child.err_code = 0;
        child.rip      = args->ret_rip;      /* just after its `syscall` */
        child.cs       = 0x23;
        child.rflags   = args->ret_rflags | 0x200;   /* IF, or it never
                                                      * yields the CPU  */
        child.rsp      = a2;
        child.ss       = 0x1B;

        tid = sched64_add_frame(&child, space,
                                (a1 & CLONE_SETTLS) ? args->a5 : 0);
        if (tid < 0) return (uint64_t)-11;              /* -EAGAIN */

        /* The parent gets the child's tid; the child gets 0 above. */
        return (uint64_t)tid + 1;
    }

    /* fork(2). The child is the parent with a different address space,
     * a different pid, and 0 where the parent gets the child's pid.
     *
     * Ordering matters: the address space copy is the expensive part
     * and the part that can fail, so it happens before anything is
     * committed. A half-built process is worse than a failed fork. */
    case SYS64_FORK:
        return do_fork(args);

    /* execve(path, argv, envp). Replaces the calling process rather
     * than returning to it, so on success there is nothing to return
     * to: the syscall ends by entering the new program. */
    case SYS64_EXECVE:
        return do_execve((const char*)a1, (const char* const*)a2,
                         (const char* const*)a3, args);

    case SYS64_WAIT4: {
        int status = 0;
        int reaped = proc64_reap_child(proc64_current_pid(), &status);
        registers64_t self, next;
        vmspace64_t next_space;
        uint64_t next_fs;

        if (reaped >= 0) {
            /* wait4 reports a *wait status*, not an exit code: the low
             * byte says how it died and the next says with what. A
             * caller using WEXITSTATUS shifts it back down. */
            if (a2) *(int*)a2 = (status & 0xFF) << 8;
            return (uint64_t)reaped;
        }

        /* Nothing to reap. If there are no children at all that is
         * -ECHILD; if there are, the caller has to *wait* - which is
         * the entire point of the call, and returning -ECHILD to a
         * parent whose child simply has not been scheduled yet is the
         * difference between a working fork and a racing one. */
        if (!proc64_has_children(proc64_current_pid()))
            return (uint64_t)-10;                      /* -ECHILD */

        /* Blocked, and restarted rather than resumed: rewind rip by the
         * two bytes of the `syscall` instruction and put the number
         * back in rax, so waking re-executes the call and re-checks.
         * Linux does the same thing for a restartable syscall, for the
         * same reason - there is no other way to return a value that
         * was not known when the caller blocked. */
        frame_from_args(args, SYS64_WAIT4, &self);
        self.rip = args->ret_rip - 2;

        if (!sched64_block_current(&self, PROC64_WAIT_KEY(proc64_current_pid()), SYS64_WAIT4,
                                   &next, &next_space, &next_fs))
            return (uint64_t)-10;

        vmspace64_switch(&next_space);
        write_msr(0xC0000100u, next_fs);
        sched64_resume(&next);                         /* never returns */
    }

    case SYS64_GETPID:
        return (uint64_t)proc64_current_pid();

    case SYS64_GETPPID: {
        proc64_t* p = proc64_current();
        return p ? (uint64_t)p->parent : 0;
    }

    case SYS64_GETTID:
        return (uint64_t)sched64_current() + 1;

    case SYS64_SET_TID_ADDRESS:
        return (uint64_t)sched64_current() + 1;

    case SYS64_SET_ROBUST_LIST:
        return 0;

    case SYS64_IOCTL: {
        /* The two fbdev queries, with Linux's own structure layouts -
         * offsets taken from <linux/fb.h> for x86-64. A program asks the
         * driver what the screen is rather than being told out of band,
         * because that is the interface everything from SDL to Wine's
         * fbdev path already speaks.
         *
         * Getting these offsets wrong is the classic way to produce a
         * driver that "works" and paints diagonal stripes: xres and
         * line_length land in the wrong fields and every scanline is
         * placed from a wrong stride. The differential test in
         * userland/fbdraw64.c checks the values, not just the call. */
        #define FBIOGET_VSCREENINFO 0x4600
        #define FBIOGET_FSCREENINFO 0x4602

        if ((a2 == FBIOGET_VSCREENINFO || a2 == FBIOGET_FSCREENINFO) &&
            a1 >= 3 && a1 < FD_MAX && fds[a1].used &&
            ramfs64_device(fds[a1].node) == RAMFS64_DEV_FB) {
            uint8_t* out = (uint8_t*)a3;
            uint32_t i;

            if (!fb64_ready()) return (uint64_t)-19;       /* -ENODEV */
            if (!out) return (uint64_t)-14;               /* -EFAULT */

            if (a2 == FBIOGET_VSCREENINFO) {
                for (i = 0; i < 160; i++) out[i] = 0;
                *(uint32_t*)(out +  0) = fb64_width();     /* xres */
                *(uint32_t*)(out +  4) = fb64_height();    /* yres */
                *(uint32_t*)(out +  8) = fb64_width();     /* xres_virtual */
                *(uint32_t*)(out + 12) = fb64_height();    /* yres_virtual */
                *(uint32_t*)(out + 24) = fb64_bpp();       /* bits_per_pixel */
                /* The colour layout, as bit offsets into a pixel. This
                 * is what says 0x00RRGGBB rather than leaving a caller
                 * to guess from the depth. */
                *(uint32_t*)(out + 32) = 16; *(uint32_t*)(out + 36) = 8; /* red */
                *(uint32_t*)(out + 44) =  8; *(uint32_t*)(out + 48) = 8; /* green */
                *(uint32_t*)(out + 56) =  0; *(uint32_t*)(out + 60) = 8; /* blue */
                /* transp length stays 0: the fourth byte is padding this
                 * driver writes as zero, not an alpha channel. */
                return 0;
            }

            for (i = 0; i < 80; i++) out[i] = 0;
            {
                static const char id[] = "novarisfb";
                for (i = 0; id[i]; i++) out[i] = (uint8_t)id[i];
            }
            *(uint64_t*)(out + 16) = fb64_phys();          /* smem_start */
            *(uint32_t*)(out + 24) = (uint32_t)fb64_bytes(); /* smem_len */
            *(uint32_t*)(out + 28) = 0;                    /* PACKED_PIXELS */
            *(uint32_t*)(out + 36) = 2;                    /* TRUECOLOR */
            *(uint32_t*)(out + 48) = fb64_pitch();         /* line_length */
            return 0;
        }

        /* glibc asks whether fd 1 is a terminal to choose line
         * buffering. -ENOTTY makes it a fully buffered stream, which is
         * correct here: this is a serial port, not a tty. */
        return (uint64_t)-25;
    }

    case SYS64_FSTAT: {
        uint8_t* st = (uint8_t*)a2;
        for (int i = 0; i < 144; i++) st[i] = 0;

        /* A real file has to report itself as one. Until Milestone 61
         * this answered "character device, size 0" for every descriptor,
         * which is fine for stdout and fatal for a library: ld.so fstats
         * the file it has just opened, sees something unmappable with no
         * length, and gives up without ever mapping libc - which
         * presents much later as an undefined symbol. */
        if (a1 >= 3) {
            if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
            fill_ids(st, fds[a1].node);
            *(uint32_t*)(st + 24) = ramfs64_is_dir(fds[a1].node)
                                    ? 0040755u : 0100644u;
            *(uint64_t*)(st + 48) = ramfs64_size(fds[a1].node);
            *(uint64_t*)(st + 56) = 4096;
            return 0;
        }

        /* stdin/stdout/stderr: a character device, which is what makes
         * glibc's stdio choose the buffering it does. */
        *(uint32_t*)(st + 24) = 0020620u;  /* S_IFCHR | 0620 */
        *(uint64_t*)(st + 56) = 1024;
        return 0;
    }

    case SYS64_GETRANDOM: {
        /* Not random. It is deterministic and it is documented as such,
         * because a kernel with no entropy source that pretends
         * otherwise is worse than one that says so. glibc uses this for
         * the stack guard, which this makes predictable. */
        uint8_t* buf = (uint8_t*)a1;
        static uint64_t seed = 0x2545F4914F6CDD1DULL;
        for (uint64_t i = 0; i < a2; i++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            buf[i] = (uint8_t)seed;
        }
        return a2;
    }

    /* futex(uaddr, op, val, timeout, uaddr2, val3)
     *
     * Two operations, which is what a thread library actually needs:
     * WAIT blocks until somebody WAKEs the same address.
     *
     * FUTEX_PRIVATE_FLAG is masked off rather than acted on. It tells
     * Linux the futex is not shared between processes, which lets it
     * skip looking the page up in a global hash - an optimisation, not a
     * semantic. With one address space per process and no shared memory
     * here, private and shared behave identically. */
    case SYS64_FUTEX: {
        uint32_t op = (uint32_t)a2 & ~(uint32_t)FUTEX_PRIVATE_FLAG;

        if (op == FUTEX_WAIT) {
            registers64_t self, next;
            vmspace64_t next_space;
            uint64_t next_fs;

            /* The comparison is the whole point of the interface, and
             * it is why futex has no race: between the caller deciding
             * to sleep and this check, the waker may already have run
             * and changed the value. If it has, do not sleep. */
            if (*(volatile uint32_t*)a1 != (uint32_t)a3)
                return (uint64_t)-11;              /* -EAGAIN */

            futex_waits++;
            frame_from_args(args, 0, &self);
            if (!sched64_block_current(&self, a1, 0, &next, &next_space,
                                       &next_fs))
                return (uint64_t)-35;              /* -EDEADLK */

            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);
            sched64_resume(&next);                 /* never returns */
        }

        if (op == FUTEX_WAKE) {
            int n = sched64_wake(a1, (int)(uint32_t)a3);
            futex_wakes += (uint64_t)n;
            return (uint64_t)n;
        }

        /* FUTEX_REQUEUE, FUTEX_WAIT_BITSET, priority inheritance and
         * the rest are not implemented; -ENOSYS is what lets a library
         * fall back rather than assume they worked. */
        return (uint64_t)-38;
    }

    case SYS64_RT_SIGACTION:
        /* Real since Milestone 58: a handler installed here is a handler
         * a fault will actually reach. */
        return (uint64_t)(int64_t)signal64_sigaction(
                   (int)a1, (const ksigaction64_t*)a2,
                   (ksigaction64_t*)a3);

    case SYS64_RT_SIGRETURN: {
        registers64_t resumed;
        /* The frame sits on the thread's own stack, which is where the
         * handler is returning from. Resuming it is the same trick as
         * futex: this syscall does not return, the thread does. */
        signal64_sigreturn(saved_user_rsp, &resumed);
        /* The scheduler's saved copy for this task is stale now, and
         * harmlessly so: the next timer tick overwrites it from the
         * live frame before anything switches away. */
        sched64_resume(&resumed);                 /* never returns */
    }

    case SYS64_RT_SIGPROCMASK:
        /* Still accepted and ignored. Masking matters for asynchronous
         * signals; a fault is delivered to the thread that caused it
         * whether or not it wanted to hear about it. */
        return 0;

    /* --- files ---------------------------------------------------- */

    case SYS64_OPEN: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return do_open(path, a2);
    }

    case SYS64_OPENAT: {
        /* openat(dirfd, path, flags, mode). glibc uses this in
         * preference to open(2).
         *
         * AT_FDCWD means "relative to the working directory", which is
         * now a thing this kernel has. A real dirfd - relative to some
         * other open directory - is answered for the case that actually
         * occurs, an absolute path, where the dirfd is ignored by
         * definition; anything else is -EBADF rather than silently
         * resolved against the wrong directory. */
        char path[PROC64_PATH_MAX];
        const char* given = (const char*)a2;
        int64_t e;

        if ((int64_t)(int32_t)a1 != AT_FDCWD_ && given && given[0] != '/')
            return (uint64_t)-9;                       /* -EBADF */

        e = abs_path(given, path);
        if (e) return (uint64_t)e;
        return do_open(path, a3);
    }

    /* chdir(path). Wine's first act on a prefix is to chdir into it, and
     * the 32-bit tree's log is full of "chdir to /disk/.wine : No such
     * file or directory" from before it had one. */
    case SYS64_CHDIR: {
        char path[PROC64_PATH_MAX];
        proc64_t* me = proc64_current();
        int node;
        int64_t e;

        if (!me) return (uint64_t)-2;
        e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;

        node = ramfs64_lookup(path);
        if (node < 0) return (uint64_t)-2;             /* -ENOENT */
        if (!ramfs64_is_dir(node)) return (uint64_t)-20; /* -ENOTDIR */

        /* Stored as the canonical path built back up from the node, not
         * as the text the program supplied - so that getcwd after
         * chdir("a/../b") answers "/b", and after a chdir through a
         * symlink answers where it landed rather than how it got
         * there. */
        if (ramfs64_path(node, me->cwd, PROC64_PATH_MAX) < 0)
            return (uint64_t)-36;                      /* -ENAMETOOLONG */
        return 0;
    }

    case SYS64_FCHDIR: {
        proc64_t* me = proc64_current();
        if (!me) return (uint64_t)-2;
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        if (!ramfs64_is_dir(fds[a1].node)) return (uint64_t)-20;
        if (ramfs64_path(fds[a1].node, me->cwd, PROC64_PATH_MAX) < 0)
            return (uint64_t)-36;
        return 0;
    }

    /* getcwd(buf, size). Returns the length including the NUL, which is
     * the kernel's contract and not glibc's - glibc returns the pointer
     * and gets the length from here. */
    case SYS64_GETCWD: {
        proc64_t* me = proc64_current();
        char* buf = (char*)a1;
        uint64_t n;

        if (!me || !buf) return (uint64_t)-14;         /* -EFAULT */
        n = kstrlen(me->cwd) + 1;
        if (a2 < n) return (uint64_t)-34;              /* -ERANGE */
        kmemcpy(buf, me->cwd, n);
        return n;
    }

    case SYS64_SYMLINK: {
        /* symlink(target, linkpath) - the target is not a path this
         * kernel resolves, so only the second argument is made
         * absolute. */
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a2, path);
        if (e) return (uint64_t)e;
        e = ramfs64_symlink(path, (const char*)a1);
        return e < 0 ? (uint64_t)e : 0;
    }

    case SYS64_SYMLINKAT: {
        char path[PROC64_PATH_MAX];
        int64_t e;
        if ((int64_t)(int32_t)a2 != AT_FDCWD_
            && ((const char*)a3)[0] != '/') return (uint64_t)-9;
        e = abs_path((const char*)a3, path);
        if (e) return (uint64_t)e;
        e = ramfs64_symlink(path, (const char*)a1);
        return e < 0 ? (uint64_t)e : 0;
    }

    case SYS64_CLOSE:
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        fds[a1].used = 0;
        return 0;

    case SYS64_LSEEK: {
        uint64_t base;
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        switch (a3) {
        case SEEK_SET_: base = 0; break;
        case SEEK_CUR_: base = fds[a1].pos; break;
        case SEEK_END_: base = ramfs64_size(fds[a1].node); break;
        default: return (uint64_t)-22;
        }
        fds[a1].pos = base + a2;
        return fds[a1].pos;
    }

    /* access(path, mode). ld.so calls this before anything else, on
     * /etc/ld.so.preload, and treats -ENOSYS as fatal enough to stop
     * looking - so answering it is the difference between a loader that
     * searches for a library and one that gives up. */
    /* readlink(path, buf, bufsiz).
     *
     * Only /proc/self/exe, and that one earns its place: it is how a
     * program finds out where it was installed. Wine uses it to locate
     * its own lib directory, and without it Wine computes the path to
     * ntdll.so as (null) and stops - which is exactly how this came to
     * be implemented. There is no /proc here, so the answer is
     * remembered when the program is loaded.
     *
     * readlink does not NUL-terminate, and a caller that assumed it did
     * would read past what it was given. */
    case SYS64_READLINK:
        return do_readlink((const char*)a1, (char*)a2, a3);

    case SYS64_READLINKAT:
        if ((int64_t)(int32_t)a1 != AT_FDCWD_
            && ((const char*)a2)[0] != '/') return (uint64_t)-9;
        return do_readlink((const char*)a2, (char*)a3, args->a4);

    case SYS64_ACCESS: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return ramfs64_lookup(path) >= 0 ? 0 : (uint64_t)-2;
    }

    case SYS64_STAT: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return do_stat(path, (void*)a2, 1);
    }

    case SYS64_LSTAT: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return do_stat(path, (void*)a2, 0);
    }

    case SYS64_NEWFSTATAT: {
        char path[PROC64_PATH_MAX];
        int64_t e;
        if ((int64_t)(int32_t)a1 != AT_FDCWD_
            && ((const char*)a2)[0] != '/') return (uint64_t)-9;
        e = abs_path((const char*)a2, path);
        if (e) return (uint64_t)e;
        return do_stat(path, (void*)a3,
                       !(args->a4 & AT_SYMLINK_NOFOLLOW_));
    }

    /* pread64/pwrite64(fd, buf, count, offset). A positioned read that
     * does not move the file offset - which is how ld.so reads a
     * library's program headers while keeping its place, and the first
     * thing it wanted once it could open the library at all. */
    case SYS64_PREAD64: {
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        return (uint64_t)ramfs64_read(fds[a1].node, args->a4,
                                      (void*)a2, a3);
    }

    case SYS64_PWRITE64: {
        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        return (uint64_t)ramfs64_write(fds[a1].node, args->a4,
                                       (const void*)a2, a3);
    }

    /* getdents64(fd, buf, count). The one call a flat path table could
     * not have answered at all: it enumerates a directory, which needs
     * children to enumerate.
     *
     * The file offset counts entries rather than bytes, which is legal -
     * d_off is opaque to the caller and only ever fed back to lseek. */
    case SYS64_GETDENTS64: {
        uint8_t* out = (uint8_t*)a2;
        uint64_t written = 0;
        int dir;

        if (a1 < 3 || a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        dir = fds[a1].node;
        if (!ramfs64_is_dir(dir)) return (uint64_t)-20;   /* -ENOTDIR */

        for (;;) {
            uint64_t index = fds[a1].pos;
            const char* name;
            uint64_t ino, reclen, namelen;
            uint8_t type;
            int child;

            /* "." and ".." are not stored as nodes; they are
             * synthesised here, the way a real filesystem does. */
            if (index == 0) {
                name = "."; ino = (uint64_t)dir + 1; type = 4;
            } else if (index == 1) {
                int up = ramfs64_parent(dir);
                name = ".."; ino = (uint64_t)(up < 0 ? dir : up) + 1;
                type = 4;
            } else if (ramfs64_child(dir, index - 2, &child)) {
                name = ramfs64_name(child);
                ino  = (uint64_t)child + 1;
                type = ramfs64_is_dir(child) ? 4 : 8;   /* DT_DIR/DT_REG */
            } else {
                break;                                   /* end of it */
            }

            namelen = kstrlen(name);
            /* d_ino(8) d_off(8) d_reclen(2) d_type(1) name+NUL, rounded
             * to 8 so the next record starts aligned. */
            reclen = (8 + 8 + 2 + 1 + namelen + 1 + 7) & ~7ULL;
            if (written + reclen > a3) break;

            *(uint64_t*)(out + written)      = ino;
            *(uint64_t*)(out + written + 8)  = (int64_t)index + 1;
            *(uint16_t*)(out + written + 16) = (uint16_t)reclen;
            *(uint8_t*)(out + written + 18)  = type;
            kmemcpy(out + written + 19, name, namelen);
            out[written + 19 + namelen] = 0;

            written += reclen;
            fds[a1].pos++;
        }
        return written;
    }

    case SYS64_RMDIR: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return (uint64_t)(int64_t)ramfs64_rmdir(path);
    }

    case SYS64_MKDIR: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return ramfs64_create(path, 1) >= 0 ? 0 : (uint64_t)-28;
    }

    case SYS64_MKDIRAT: {
        char path[PROC64_PATH_MAX];
        int64_t e;
        if ((int64_t)(int32_t)a1 != AT_FDCWD_
            && ((const char*)a2)[0] != '/') return (uint64_t)-9;
        e = abs_path((const char*)a2, path);
        if (e) return (uint64_t)e;
        return ramfs64_create(path, 1) >= 0 ? 0 : (uint64_t)-28;
    }

    case SYS64_UNLINK: {
        char path[PROC64_PATH_MAX];
        int64_t e = abs_path((const char*)a1, path);
        if (e) return (uint64_t)e;
        return (uint64_t)(int64_t)ramfs64_unlink(path);
    }

    /* unlinkat(dirfd, path, flags). One call for two operations: with
     * AT_REMOVEDIR it is rmdir and without it is unlink. glibc's
     * remove() and rmdir() both arrive here. */
    case SYS64_UNLINKAT: {
        char path[PROC64_PATH_MAX];
        int64_t e;
        if ((int64_t)(int32_t)a1 != AT_FDCWD_
            && ((const char*)a2)[0] != '/') return (uint64_t)-9;
        e = abs_path((const char*)a2, path);
        if (e) return (uint64_t)e;
        return (uint64_t)(int64_t)((a3 & AT_REMOVEDIR_)
                                   ? ramfs64_rmdir(path)
                                   : ramfs64_unlink(path));
    }

    case SYS64_READ: {
        int64_t n;
        if (a1 < 3) return 0;                          /* stdin: end of file */
        if (a1 >= FD_MAX || !fds[a1].used) return (uint64_t)-9;
        n = ramfs64_read(fds[a1].node, fds[a1].pos, (void*)a2, a3);
        if (n > 0) fds[a1].pos += (uint64_t)n;
        return (uint64_t)n;
    }
    case SYS64_ECHO:
        last_arg = a1;
        return a1 + 0x1111;
    /* exit(2) ends the calling THREAD. exit_group(2) ends the process.
     *
     * Treating them as the same thing is a real divergence and it was
     * observable: userland/thread64.s, run on Linux with exit(60), hung
     * - the parent thread ended and the parked child kept the process
     * alive. Novaris passed that same program, because it had only one
     * behaviour for both. */
    case SYS64_EXIT: {
        registers64_t next;
        vmspace64_t next_space;
        uint64_t next_fs;

        thread_exits++;
        if (sched64_exit_current(&next, &next_space, &next_fs)) {
            /* A sibling is still runnable, so this thread simply stops
             * existing and that one continues. There is no returning to
             * the caller: the thread it would return to is gone. */
            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);        /* its own TLS */
            sched64_resume(&next);                  /* never returns */
        }
        /* The last thread. Falling through leaves ring 3 the way
         * exit_group does, which is correct - the process is over. */
        exit_code = a1;
        proc64_exit(proc64_current_pid(), (int)a1);
        return a1;
    }

    case SYS64_EXIT_GROUP: {
        registers64_t next;
        vmspace64_t next_space;
        uint64_t next_fs;

        exit_code = a1;
        {
            proc64_t* me = proc64_current();
            int parent = me ? me->parent : 0;
            proc64_exit(proc64_current_pid(), (int)a1);
            /* A parent blocked in wait4 is waiting on exactly this. */
            if (parent) sched64_wake(PROC64_WAIT_KEY(parent),
                                     SCHED64_MAX_TASKS);
        }

        /* If another process is runnable, this one ending is not the
         * end of the run - a parent waiting on it has to get its turn.
         * Same machinery as thread exit; the difference is only which
         * table records the status. */
        if (sched64_exit_process(proc64_current_pid(), &next, &next_space,
                                 &next_fs)) {
            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);
            sched64_resume(&next);                     /* never returns */
        }
        return a1;
    }
    default:
        /* Said out loud, because there is no strace here and the only
         * way to find out what a real program wants is to let it ask. */
        unimpl_count++;
        last_unimpl = nr;
        serial64_puts("NOVARIS64: [enosys] syscall ");
        serial64_putdec(nr);
        serial64_putc('\n');
        /* Linux answers an unimplemented call with -ENOSYS, and programs
         * do check for it, so this is -38 rather than -1. */
        return (uint64_t)-38;
    }
}

uint64_t syscall64_count(void)     { return call_count; }
uint64_t syscall64_last_arg(void)  { return last_arg; }
uint64_t syscall64_exit_code(void) { return exit_code; }
