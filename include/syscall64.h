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
#define SYS64_FSTAT           5
#define SYS64_MMAP            9
#define SYS64_MPROTECT        10
#define SYS64_MUNMAP          11
#define SYS64_BRK             12
#define SYS64_RT_SIGACTION    13
#define SYS64_RT_SIGPROCMASK  14
#define SYS64_IOCTL           16
#define SYS64_WRITEV          20
#define SYS64_UNAME           63
#define SYS64_READLINK        89
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

/* Threads that ended through exit(2) while siblings were still running -
 * the case that does NOT end the process. */
uint64_t syscall64_thread_exits(void);

/* Threads that actually blocked in futex(2), and wakeups that actually
 * woke one. A run where nothing ever contended would show zero here and
 * still pass everything else. */
uint64_t syscall64_futex_waits(void);
uint64_t syscall64_futex_wakes(void);

#endif
