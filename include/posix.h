#ifndef POSIX_H
#define POSIX_H

#include <stdint.h>
#include "idt.h"

/* posix.h - the Linux/i386 system call ABI.
 *
 * Milestone 18, and the first instalment of item 3 on the Path A list
 * (ROADMAP.md): the POSIX-shaped syscall surface Wine's own build links
 * against. Wine is not a Windows program - it is a *Unix* program that
 * happens to implement Windows. Cross-compiling it means giving it a
 * kernel it recognises underneath, and that means these numbers, these
 * argument registers and these errno values, not a bespoke set.
 *
 * So this replaces the three ad-hoc calls Milestone 5 defined
 * (SYS_EXIT=0, SYS_WRITE=1, SYS_MMAP=2) with the real thing. The two
 * numberings cannot coexist - Linux's exit is 1, which was Novaris's
 * write - so the handful of demo programs that used the old numbers were
 * migrated rather than bridged.
 *
 * The ABI, which is Linux's and not ours to choose:
 *
 *   eax  = syscall number
 *   ebx, ecx, edx, esi, edi, ebp = arguments 1..6
 *   eax  = return value; errors come back as -errno, in range [-4095, -1]
 *
 * What this buys, concretely: a program compiled with an ordinary
 * `gcc -m32 -static -nostdlib` and hand-written `int $0x80` stubs - a
 * program written for Linux, with nothing in it that knows Novaris
 * exists - runs unmodified here. userland/posix_test.c is exactly that,
 * and the same binary is run on the Linux build host and on Novaris and
 * the two transcripts compared. */

/* --- syscall numbers (Linux i386) --------------------------------------
 * Only the ones implemented or deliberately stubbed are named here; the
 * dispatcher reports anything else by number. */
#define SYS_exit             1
#define SYS_read             3
#define SYS_write            4
#define SYS_open             5
#define SYS_close            6
#define SYS_waitpid          7
#define SYS_unlink          10
#define SYS_chdir           12
#define SYS_rename          38
#define SYS_mkdir           39
#define SYS_rmdir           40
#define SYS_fsync          118
#define SYS_truncate        92
#define SYS_ftruncate       93
#define SYS_truncate64     193
#define SYS_ftruncate64    194
#define SYS_getdents64     220
#define SYS_mkdirat        296
#define SYS_unlinkat       301
#define SYS_renameat       302
#define SYS_renameat2      353
#define SYS_time            13
#define SYS_lseek           19
#define SYS__llseek        140
#define SYS_getpid          20
#define SYS_access          33
#define SYS_kill            37
#define SYS_brk             45
#define SYS_ioctl           54
#define SYS_munmap          91
#define SYS_uname          122
#define SYS_mprotect       125
#define SYS_writev         146
#define SYS_nanosleep      162
#define SYS_rt_sigaction   174
#define SYS_rt_sigprocmask 175
#define SYS_getcwd         183
#define SYS_mmap2          192
#define SYS_stat64         195
#define SYS_fstat64        197
#define SYS_getuid32       199
#define SYS_getgid32       200
#define SYS_geteuid32      201
#define SYS_getegid32      202
#define SYS_gettid         224
#define SYS_exit_group     252
#define SYS_set_thread_area 243
#define SYS_rt_sigreturn   173
#define SYS_tgkill         270
#define SYS_clone          120
#define SYS_futex          240
#define SYS_set_tid_address 258
#define SYS_ugetrlimit     191
#define SYS_openat         295
#define SYS_faccessat      307
#define SYS_faccessat2     439
#define SYS_fstatat64      300
#define SYS_pread64        180
#define SYS_readlink        85
#define SYS_prctl          172
#define SYS_socketcall     102
#define SYS_fork             2
#define SYS_vfork          190
#define SYS_execve          11
#define SYS_waitpid_         7
#define SYS_wait4          114
#define SYS_dup             41
#define SYS_dup2            63
#define SYS_pipe            42
#define SYS_pipe2          331
#define SYS_readlinkat     305
#define SYS_set_robust_list 311
#define SYS_getrandom      355
#define SYS_statx          383
#define SYS_rseq           386
#define SYS_clock_gettime64 403
#define SYS_clock_gettime  265
/* Milestone 29 - two processes at once. Wine's start_server() is fork(),
 * execve(wineserver), waitpid(); its server loop multiplexes descriptors
 * rather than blocking on one; and its loader hands a child a pipe to
 * report back through. */
#define SYS__newselect     142
#define SYS_poll           168
#define SYS_ppoll          309
#define SYS_pselect6       308
#define SYS_dup3           330
#define SYS_getppid         64
#define SYS_fcntl           55
#define SYS_fcntl64        221
#define SYS_prlimit64      340
#define SYS_setrlimit       75
#define SYS_getrlimit       76
#define SYS_getrusage       77
#define SYS_gettimeofday    78
#define SYS_symlink         83
#define SYS_symlinkat      304
#define SYS_sched_yield    158
#define SYS_madvise        219
#define SYS_lstat64        196
#define SYS_fchdir         133
#define SYS_epoll_create   254
#define SYS_epoll_create1  329
#define SYS_clock_getres   266
#define SYS_sysinfo        116
#define SYS_setsid          66
#define SYS_getpgrp         65
#define SYS_setpgid         57
#define SYS_umask           60
#define SYS_chmod           15
#define SYS_fchmod          94
#define SYS_fchmodat       306
#define SYS_clock_nanosleep 267
#define SYS_clock_settime64 404
#define SYS_clock_getres_time64 406
#define SYS_clock_nanosleep_time64 407
#define SYS_sigreturn      119
#define SYS_sched_setaffinity 241
#define SYS_sched_getaffinity 242
#define SYS_pwrite64       181
#define SYS_statfs64       268
#define SYS_fstatfs64      269
#define SYS_getpriority     96
#define SYS_setpriority     97
#define SYS_sigaltstack    186
#define SYS_userfaultfd    374
#define SYS_clone3         435
#define SYS_membarrier     375

/* --- errno values, negated on return ------------------------------------ */
#define EPERM    1
#define ENOENT   2
#define EBADF    9
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define ENOSYS  38
#define EAGAIN  11
#define ERANGE  34
#define EOPNOTSUPP 95
#define ENODEV  19
#define EINTR    4
#define ETIMEDOUT 110   /* what a futex wait that ran out of time returns */
#define EISDIR  21
#define ENOTDIR 20
#define ENOTEMPTY 39
#define EXDEV   18
#define ENOTCONN 107
#define EPIPE    32
#define EAFNOSUPPORT 97
#define EPROTONOSUPPORT 93
#define EADDRINUSE 98
#define ECONNREFUSED 111
#define EISCONN  106
#define ENOTSOCK 88
#define ENFILE   23
#define EMSGSIZE 90
#define ECHILD   10
#define ESRCH     3
#define ENOEXEC   8
#define ELOOP    40
#define ENAMETOOLONG 36
#define E2BIG     7

/* --- poll / select (Milestone 29) ---------------------------------------
 *
 * Wine's server loop does not block on one descriptor, it waits on all of
 * them at once - so a socket layer without this is a socket layer
 * wineserver cannot use. The bit values are Linux's. */
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

typedef struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
} k_pollfd_t;

/* select's fd_set is a bitmap of `nfds` bits, and the syscall's first
 * argument says how many of them to look at. 32 descriptors is the whole
 * table, so one word is genuinely enough - but the caller's set may be
 * wider (glibc's fd_set is 1024 bits), so only the words that matter are
 * read and written. */
#define K_FDSET_WORDS 32

/* --- mmap / mprotect flags, as Linux defines them ----------------------- */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_NORESERVE 0x4000
/* "This address or nothing" - MAP_FIXED without the part of MAP_FIXED
 * that silently unmaps whatever was there. Wine asks for a Windows
 * image's ImageBase this way, because taking it by force would destroy
 * something else and taking a different address would be wrong.
 *
 * Novaris ignored the flag, which meant it read the request as "anywhere
 * will do" and answered with an arena address. Wine compared that against
 * the address it had asked for, saw a difference, and reported
 * "out of memory for 0x400000" - a message about the one thing that was
 * not the problem. */
#define MAP_FIXED_NOREPLACE 0x100000

/* --- signals (Milestone 19) ---------------------------------------------
 *
 * Wine needs these more than it needs almost anything else on this list:
 * it installs a SIGSEGV handler and uses page faults as a control-flow
 * mechanism, so a kernel that cannot deliver a signal to a handler and
 * resume the faulting instruction afterwards cannot run it at all. */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define NSIG    32

#define SIG_DFL 0u
#define SIG_IGN 1u

/* sigprocmask's `how`. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* sa_flags. SA_RESTORER matters here: raw rt_sigaction on i386 requires
 * the *caller* to supply the trampoline that issues rt_sigreturn (glibc
 * supplies one; a program using raw syscalls has to too). Novaris uses
 * the caller's, exactly as Linux does, which is what lets one binary run
 * on both. */
#define SA_NOCLDSTOP 0x00000001u
#define SA_SIGINFO   0x00000004u
#define SA_RESTORER  0x04000000u
#define SA_ONSTACK   0x08000000u
#define SA_NODEFER   0x40000000u
#define SA_RESETHAND 0x80000000u

/* The i386 rt_sigaction argument, as the kernel sees it. Not libc's
 * struct sigaction - the field order differs. */
typedef struct {
    uint32_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint32_t mask[2];   /* 64-bit sigset; only the low word is used */
} k_sigaction_t;

/* si_code values. The generic ones are negative or >= 0x80 and say who
 * raised the signal; the small positive ones are per-signal and say what
 * kind of fault it was. Only the ones Novaris can actually produce are
 * listed. */
#define SI_USER      0        /* kill() */
#define SI_KERNEL    0x80     /* a fault with no more specific cause */
#define SI_TKILL     (-6)     /* tgkill() */
#define SEGV_MAPERR  1        /* address not mapped */
#define SEGV_ACCERR  2        /* mapped, but the access was not permitted */
#define FPE_INTDIV   1
#define ILL_ILLOPN   2
#define TRAP_BRKPT   1

/* --- the SA_SIGINFO handler's second and third arguments (Milestone 23)
 *
 * A three-argument handler is called as
 *
 *     void handler(int signo, siginfo_t* info, void* ucontext);
 *
 * and Wine's whole exception machinery is built on reading - and writing
 * back - the register state in that third argument. These are the
 * Linux/i386 layouts of both, field for field, because a handler compiled
 * against glibc's headers indexes them by offset and does not care what
 * the kernel calls them.
 *
 * struct sigcontext is written so every member is four bytes: on Linux
 * the selectors are declared as a 16-bit value plus a 16-bit pad, which
 * is what lets glibc's gregset_t treat the whole thing as an array of 19
 * longs indexed by REG_GS(0) ... REG_SS(18). Keeping them uint32_t here
 * is the same bytes with less ceremony. */
typedef struct {
    uint32_t gs, fs, es, ds;                    /* REG_GS  .. REG_DS   0-3 */
    uint32_t edi, esi, ebp, esp;                /* REG_EDI .. REG_ESP  4-7 */
    uint32_t ebx, edx, ecx, eax;                /* REG_EBX .. REG_EAX  8-11 */
    uint32_t trapno, err;                       /* REG_TRAPNO, REG_ERR 12-13 */
    uint32_t eip, cs, eflags;                   /* REG_EIP .. REG_EFL 14-16 */
    uint32_t esp_at_signal, ss;                 /* REG_UESP, REG_SS   17-18 */
    uint32_t fpstate;      /* struct _fpstate*; NULL - no FP state saved */
    uint32_t oldmask;
    uint32_t cr2;          /* the faulting address, for a page fault */
} k_sigcontext_t;          /* 88 bytes; mcontext_t is exactly this */

/* struct _fpstate - the x87/SSE state a signal handler is shown, pointed
 * at by sigcontext.fpstate (Milestone 24).
 *
 * The layout has a history in it. The first 112 bytes are the *legacy*
 * i387 environment: a 386's worth of control/status/tag words and eight
 * 10-byte x87 registers, which is also exactly Windows' FLOATING_SAVE_AREA
 * - Wine casts this pointer straight to one. Everything from offset 112
 * on is simply the 512-byte FXSAVE image, unaltered, which is where the
 * XMM registers and MXCSR live. Linux writes both halves and so does
 * Novaris; a handler reading either finds what it expects.
 *
 * `magic` is how a program knows whether the FXSAVE half is there at all:
 * 0xffff means legacy x87 only, 0 (X86_FXSR_MAGIC) means the image
 * follows. Wine tests it as `fpstate->status >> 16`, reading status and
 * magic as one 32-bit word, which is why they are adjacent 16-bit fields
 * and not two ints. */
#define X86_FXSR_MAGIC 0x0000

typedef struct {
    uint32_t cw, sw, tag, ipoff, cssel, dataoff, datasel;  /* 0   .. 27 */
    uint8_t  st[8][10];                                    /* 28  .. 107 */
    uint16_t status, magic;                                /* 108 .. 111 */
    uint8_t  fxsave[512];                                  /* 112 .. 623 */
} __attribute__((aligned(16))) k_fpstate_t;                /* 624 bytes */

typedef struct {
    uint32_t ss_sp;
    int32_t  ss_flags;
    uint32_t ss_size;
} k_stack_t;

#define SS_DISABLE 2

/* The kernel's struct ucontext is uc_flags/uc_link/uc_stack/uc_mcontext
 * and an 8-byte sigmask, and stops there. glibc's ucontext_t declares a
 * 128-byte sigmask and a trailing __fpregs_mem (and, on a current glibc,
 * a shadow-stack field after that - the host measures sizeof at 364), so
 * a handler that reads or copies the whole struct would run off the end
 * of the kernel's. The tail below makes the frame comfortably larger than
 * glibc thinks the struct is - cheap insurance, and it is the program's
 * own stack either way. */
typedef struct {
    uint32_t       uc_flags;
    uint32_t       uc_link;
    k_stack_t      uc_stack;
    k_sigcontext_t uc_mcontext;   /* at offset 20 */
    uint32_t       uc_sigmask[2]; /* at offset 108; the kernel's 8 bytes */
    uint8_t        uc_tail[276];  /* glibc's wider sigmask + __fpregs_mem */
} k_ucontext_t;                   /* 392 bytes, >= glibc's 364 */

/* siginfo_t. The union after the first three ints is 128 bytes wide in
 * total, and which member is live depends on si_code. */
typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    union {
        uint32_t _pad[29];
        struct { int32_t si_pid; uint32_t si_uid; } _kill;
        struct { uint32_t si_addr; } _sigfault;
    } _sifields;
} k_siginfo_t;                    /* 128 bytes */

/* --- open flags --------------------------------------------------------- */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_DIRECTORY 0x10000

/* --- lseek whence ------------------------------------------------------- */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Handles one `int 0x80`. Called from kernel/syscall.c, which owns the
 * IDT registration; split out so the ABI and its table live in one file
 * rather than growing inside the interrupt plumbing. */
void posix_syscall(registers_t* regs);

/* Per-process state: the file descriptor table and the mmap/brk
 * bookkeeping. Called when a program starts and when it exits, because
 * descriptors and mappings belong to the process, not to the kernel.
 *
 * Since Milestone 29 these bracket a whole *run* - one program and every
 * process it forks - rather than a single process. The per-process half
 * lives in kernel/posix_proc.c. */
void posix_process_begin(void);

/* Records the path the running program was started from, so
 * readlink("/proc/self/exe") can answer. */
void posix_set_exe_path(const char* path);

/* --- the descriptor table, as the socket layer sees it (Milestone 28) ---
 *
 * kernel/socket.c owns sockets and kernel/posix.c owns descriptors, so
 * these four are the seam between them. `export`/`import` are what
 * SCM_RIGHTS is made of: a descriptor sent over a socket travels as an
 * opaque token, because the *number* is the sender's and means nothing at
 * the other end. */
/* Turns a possibly-relative path into an absolute one, against the
 * calling process's working directory. Exported for kernel/socket.c,
 * which has to match bound names as absolute paths - two processes can
 * bind and connect from different directories and mean the same socket. */
const char* posix_resolve_path(const char* path, char* buf, uint32_t size);

struct socket;
int      posix_fd_install_socket(struct socket* s);
struct socket* posix_fd_socket(int fd);
uint32_t posix_fd_export(int fd);
int      posix_fd_import(uint32_t token);

/* "Do not return from this syscall yet - park the task on `addr` and run
 * the whole call again when something wakes it." The retry is what makes
 * this usable by an implementation that cannot resume mid-call, and the
 * block is what stops it spinning. Milestone 25 built the second half;
 * this is the first half asking for it. */
void posix_request_block(uint32_t addr, registers_t* regs);

/* The same, with a deadline: "wake me when `addr` is signalled *or* when
 * tick `deadline` arrives, whichever comes first". A pure sleep passes an
 * `addr` of 0 - there is nothing to be woken *by*, only a time to be
 * woken *at*. */
void posix_request_block_until(uint32_t addr, uint32_t deadline,
                               registers_t* regs);

/* And the set form: "wake me when any of these `n` is signalled, or at
 * `deadline`". This is poll's shape, and having it is what lets a poll
 * be woken by the descriptor that actually moved rather than noticing on
 * the next tick - see the comment above sys_poll(). */
void posix_request_block_multi(const uint32_t* addrs, int n, uint32_t deadline,
                               registers_t* regs);
void posix_process_end(void);

/* Ends the running process with `wstatus` - already in Linux's wait
 * encoding, because both callers know which kind they mean: the exit
 * syscall shifts its code up eight, and the signal layer passes a bare
 * signal number for a program killed by one. Releases the process's
 * memory and wakes a parent waiting in wait4. Does not return. */
void posix_exit_status(int wstatus);

/* The same, for every thread of the process at once. What an unhandled
 * fault does: Linux terminates the thread group rather than the thread,
 * and a kernel that terminates only the thread leaves the others running
 * in an address space it has just taken apart. */
void posix_exit_process_group(int wstatus);

/* --- signal delivery ----------------------------------------------------
 *
 * Delivery happens on the way back to ring 3, which is the only moment a
 * signal can be injected: the trap frame about to be iret'ed from *is*
 * the thread's user-mode state, so redirecting eip to a handler and
 * stashing the original frame on the user stack is all it takes. See
 * posix_signal.c. */

/* Called on the way out of every interrupt that is returning to ring 3.
 * Delivers one pending, unblocked signal if there is one. */
void posix_deliver_pending(registers_t* regs);

/* A CPU exception that belongs to a signal - a page fault is SIGSEGV, a
 * divide error SIGFPE, and so on. Returns 1 if the process had a handler
 * and it has been set up to run (so the fault is handled and the
 * instruction will be retried or the handler will decide otherwise), 0 if
 * the process should die instead. */
int posix_handle_fault_signal(registers_t* regs, uint32_t vector);

/* Raises a signal against the running process. si_code is SI_USER and
 * si_pid zero; posix_raise_from() names both, which is what separates a
 * tgkill (SI_TKILL) from a kill in the siginfo_t the handler sees. */
int posix_raise(int signo);
int posix_raise_from(int signo, int32_t code, int32_t pid);

/* Raises a signal against *another* process - which is what kill() means
 * now that there is more than one. Returns -ESRCH for a pid nothing
 * answers to; signal 0 is the existence check and raises nothing. */
int posix_raise_pid(int pid, int signo, int32_t code, int32_t from);

/* Clears every handler and mask. Called when a program starts, since
 * signal dispositions belong to the process. */
void posix_signal_reset(void);

/* Terminates the running process. Implemented in posix.c, used by the
 * signal layer's default action and by its "this cannot be recovered
 * from" paths. Does not return. */
void posix_exit_process(void);

/* The signal syscalls, implemented in posix_signal.c and dispatched from
 * posix.c's table. */
int32_t posix_sys_rt_sigaction(int signo, const k_sigaction_t* act,
                               k_sigaction_t* old, uint32_t sigsetsize);
int32_t posix_sys_rt_sigprocmask(int how, const uint32_t* set, uint32_t* old,
                                 uint32_t sigsetsize);
int32_t posix_sys_rt_sigreturn(registers_t* regs);
int32_t posix_sys_sigreturn(registers_t* regs);
int32_t posix_sys_sigaltstack(const k_stack_t* ss, k_stack_t* old);

/* --- threads (kernel/posix_thread.c, Milestone 20) ---------------------- */
int32_t posix_sys_clone(uint32_t flags, uint32_t child_stack, uint32_t* ptid,
                        uint32_t udesc, uint32_t* ctid, registers_t* regs);
int32_t posix_sys_futex(uint32_t uaddr, int op, uint32_t val,
                        uint32_t timeout, registers_t* regs);

/* Milestone 25's futex counters, for the shell's `futexinfo`. Waiting
 * being cheaper is not something a program can see in its own output, so
 * the kernel has to be asked. */
void posix_futex_stats(uint32_t* waits, uint32_t* blocks, uint32_t* retries,
                       uint32_t* idles, uint32_t* wakes);
void posix_futex_stats_reset(void);
int32_t posix_sys_set_thread_area(uint32_t udesc);

/* Zeroes the CLONE_CHILD_CLEARTID word of the thread that is exiting, so
 * a joiner spinning on it sees the thread finish. */
void posix_thread_exiting(void);

/* True (once) if the syscall just serviced asked to be re-executed rather
 * than to return - how futex(FUTEX_WAIT) waits without the scheduler
 * being able to suspend a task mid-syscall. Consumed by posix_syscall(),
 * which restores eax, rewinds eip past the `int $0x80`, and yields. */
int posix_retry_pending(void);
void posix_request_retry(void);

/* Syscall tracing, for comparing what a program does here against what
 * it does on Linux under strace. Milestone 22 needed it: a dynamic
 * linker makes a hundred-odd calls before main(), and reasoning about
 * which one behaves differently is far harder than reading the two
 * traces side by side. */
void posix_set_trace(int on);
int  posix_trace_enabled(void);

/* How many syscalls this program made that Novaris does not implement,
 * for the same reason the Win32 layer counts missing APIs: a program that
 * quietly got -ENOSYS somewhere is worth telling the user about. */
uint32_t posix_unimplemented_count(void);

#endif
