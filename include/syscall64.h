#ifndef SYSCALL64_H
#define SYSCALL64_H

#include <stdint.h>

/* Ring 3, and the syscall instruction that gets back out of it.
 *
 * `int 0x80` is not how a 64-bit kernel is entered. SYSCALL/SYSRET are a
 * pair of instructions with no descriptor lookup and no stack switch at
 * all - which is the part that has to be handled by hand: SYSCALL leaves
 * the user's rsp loaded, so the entry stub is running on a ring-3 stack
 * until it moves off it.
 *
 * The selectors come out of three MSRs rather than the IDT, and they are
 * computed from one base by fixed offsets, which is why gdt64.h orders
 * the user pair data-before-code:
 *
 *   SYSCALL:  CS = STAR[47:32],      SS = STAR[47:32] + 8
 *   SYSRET :  CS = STAR[63:48] + 16, SS = STAR[63:48] + 8   (both RPL 3)
 *
 * With STAR[47:32] = 0x08 that gives kernel 0x08/0x10, and with
 * STAR[63:48] = 0x10 it gives user 0x23/0x1B. */

/* Linux's x86-64 numbers, not Novaris's own and not the i386 ones.
 *
 * This is the point where Milestone 44's item 4 starts being paid for.
 * The 32-bit kernel implements Linux's *i386* ABI - `write` is 4 there
 * and 1 here, `exit` is 1 there and 60 here, the arguments arrive in
 * different registers, and the structures they point at are laid out
 * differently. Every one of those has to be re-earned, and using the
 * real numbers from the start is how the rest of it gets earned against
 * something real rather than against a private convention. */
#define SYS64_READ            0
#define SYS64_WRITE           1
#define SYS64_OPEN            2
#define SYS64_CLOSE           3
#define SYS64_LSEEK           8
#define SYS64_MKDIR           83
#define SYS64_RMDIR           84
#define SYS64_GETDENTS64      217
#define SYS64_UNLINK          87
#define SYS64_STAT            4
#define SYS64_PREAD64         17
#define SYS64_PWRITE64        18
#define SYS64_ACCESS          21
#define SYS64_NEWFSTATAT      262
#define SYS64_OPENAT          257

/* The working directory, and the links a Wine prefix is held together
 * with (Milestone 68). */
#define SYS64_LSTAT           6
#define SYS64_GETCWD          79
#define SYS64_CHDIR           80
#define SYS64_FCHDIR          81
#define SYS64_SYMLINK         88
#define SYS64_MKDIRAT         258
#define SYS64_UNLINKAT        263
#define SYS64_SYMLINKAT       266
#define SYS64_READLINKAT      267

/* The "relative to the working directory" dirfd, and the flag that asks
 * a stat not to follow a final symlink. */
#define AT_FDCWD_             (-100)
#define AT_SYMLINK_NOFOLLOW_  0x100
#define AT_REMOVEDIR_         0x200

/* open(2) flags, Linux's values for x86-64. */
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_APPEND   0x0400

/* mmap(2) protection and flags, Linux's values. */
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
/* "Exactly here, but only if it is free." Linux fails these with EEXIST
 * rather than replacing what is already mapped, and the flag implies
 * fixed placement on its own - it does not come with MAP_FIXED. Wine
 * reserves its address space with it and relies on the refusal: see
 * uspace64_mmap. */
#define MAP_FIXED_NOREPLACE 0x100000

/* lseek(2) whence */
#define SEEK_SET_  0
#define SEEK_CUR_  1
#define SEEK_END_  2
#define SYS64_FSTAT           5
#define SYS64_MMAP            9
#define SYS64_MPROTECT        10
#define SYS64_MUNMAP          11
#define SYS64_BRK             12
#define SYS64_RT_SIGACTION    13
#define SYS64_RT_SIGPROCMASK  14
#define SYS64_RT_SIGRETURN    15
#define SYS64_IOCTL           16
#define SYS64_WRITEV          20
#define SYS64_UNAME           63
#define SYS64_READLINK        89
#define SYS64_FORK            57
#define SYS64_EXECVE          59
#define SYS64_WAIT4           61
#define SYS64_GETPID          39
#define SYS64_GETPPID         110
#define SYS64_GETUID          102
#define SYS64_GETGID          104
#define SYS64_GETEUID         107
#define SYS64_GETEGID         108
#define SYS64_CLONE           56
#define SYS64_GETTID          186
#define SYS64_ARCH_PRCTL      158
#define SYS64_FUTEX           202
#define SYS64_SET_TID_ADDRESS 218
#define SYS64_EXIT_GROUP      231
#define SYS64_SET_ROBUST_LIST 273
#define SYS64_PRLIMIT64       302
#define SYS64_GETRANDOM       318
#define SYS64_RSEQ            334
#define SYS64_EXIT            60

/* futex(2) operations, Linux's values. FUTEX_PRIVATE_FLAG is masked off
 * rather than acted on: it lets Linux skip a global hash lookup, which
 * is an optimisation rather than a semantic, and with no shared memory
 * here private and shared behave identically. */
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_PRIVATE_FLAG  128

/* clone() flag bits, Linux's values. Only these are acted on. */
#define CLONE_VM      0x00000100
#define CLONE_VFORK   0x00004000
#define CLONE_THREAD  0x00010000
#define CLONE_SETTLS  0x00080000

/* One Novaris-private number, well above anything Linux uses, kept for
 * the bring-up test that has no way to print. */
#define SYS64_ECHO  0x1000   /* returns its argument + 0x1111 */

/* One syscall's arguments, in the order the entry stub pushes them.
 *
 * Passed by pointer rather than as seven parameters because Linux's sixth
 * argument lives in r9 and SysV's sixth parameter does too - but the
 * *number* has to go somewhere as well, so a register-for-register
 * mapping runs out. A struct on the syscall stack costs one `mov` and
 * makes the whole set addressable. */
/* Exactly the frame syscall64_entry builds, 128 bytes of it, in address
 * order. The callee-saved half and the two saved-return fields are there
 * for `clone`, which has to hand a new thread a complete register set
 * and cannot invent one from registers it cannot see. */
typedef struct syscall64_args {
    uint64_t nr;                          /* rax                       */
    uint64_t a1, a2, a3, a4, a5, a6;      /* rdi, rsi, rdx, r10, r8, r9 */
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t _pad;                        /* keeps the call 16-aligned */
    uint64_t ret_rflags;                  /* r11, as SYSCALL left it   */
    uint64_t ret_rip;                     /* rcx, likewise             */
} syscall64_args_t;

/* Sets EFER.SCE, STAR, LSTAR and FMASK. Call after gdt64_install(). */
void syscall64_init(void);

/* Enters ring 3 at user_rip with user_rsp and `arg` in rdi, and returns
 * here when the program makes a SYS64_EXIT call - whichever program that
 * turns out to be, once a scheduler is rotating between several.
 * Implemented in syscall64.s. */
extern void enter_user_mode64(uint64_t user_rip, uint64_t user_rsp,
                              uint64_t arg);

/* The ring-3 test program, as bytes to be copied into a user page. It is
 * position independent - immediates and `syscall`, no absolute
 * addressing - so it runs wherever it is mapped. */
extern uint8_t user_test_code[];
extern uint8_t user_test_code_end[];

/* The scheduler's test program: increments the counter whose address it
 * is given, forever. It is stopped by resuming it at task_count_exit
 * rather than by anything it does itself. */
extern uint8_t task_count_code[];
extern uint8_t task_count_exit[];
extern uint8_t task_count_code_end[];

/* What the dispatcher saw. */
uint64_t syscall64_count(void);
uint64_t syscall64_last_arg(void);
uint64_t syscall64_exit_code(void);
uint64_t syscall64_bytes_written(void);

/* The last syscall number that fell through to -ENOSYS, and how many
 * did. With no strace available, running a real program and reading
 * these is how its requirements get discovered. */
uint64_t syscall64_unimplemented(void);
uint64_t syscall64_unimplemented_count(void);

/* Prints every call and its result. Off by default, because it would
 * otherwise bury the program's own output in the serial transcript the
 * tests match against. */
void syscall64_set_trace(int on);

/* Closes every open descriptor. A real kernel does this as part of
 * process teardown; here the layers share one table. */
void syscall64_reset_files(void);

/* What readlink("/proc/self/exe") answers. There is no /proc here, so
 * the loader records it. */
void syscall64_set_exe_path(const char* path);

/* Set by the kernel when it ends a program itself, so the recorded
 * status is the kernel's rather than a leftover. */
void syscall64_set_exit_code(uint64_t code);

/* Threads that ended through exit(2) while siblings were still running -
 * the case that does NOT end the process. */
uint64_t syscall64_thread_exits(void);

/* Threads that actually blocked in futex(2), and wakeups that actually
 * woke one. A run where nothing ever contended would show zero here and
 * still pass everything else. */
uint64_t syscall64_futex_waits(void);
uint64_t syscall64_futex_wakes(void);

/* File-backed mmap(2) calls served. Zero would mean every mapping in a
 * run took the anonymous path. */
uint64_t syscall64_file_maps(void);

/* Processes created and programs replaced. A run where a fork silently
 * became a thread would look identical without these. */
uint64_t syscall64_forks(void);
/* Frames consumed by the most recent fork - the measurement that tells
 * a shared clone from a copied one (Milestone 69). */
uint64_t syscall64_last_fork_frames(void);
uint64_t syscall64_execs(void);

/* How many of those forks were vforks - a parent suspended until its
 * child execve'd or exited (Milestone 73). */
uint64_t syscall64_vforks(void);

#endif
