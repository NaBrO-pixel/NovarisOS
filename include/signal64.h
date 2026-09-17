#ifndef SIGNAL64_H
#define SIGNAL64_H

#include <stdint.h>
#include "idt64.h"

/* Signal delivery, x86-64 Linux layout.
 *
 * The layout is copied exactly rather than invented, and that is the
 * point: Wine's exception dispatch takes a SIGSEGV, reads the saved
 * register set out of the ucontext, turns it into a Windows EXCEPTION_
 * RECORD, and very often *writes RIP back* to resume somewhere else.
 * A handler that cannot do that is not useful to Wine, and a frame that
 * is merely Novaris-shaped could not be tested against Linux at all.
 *
 * Only what a fault needs is here: no queued signals, no kill(2), no
 * masking, no alternate stack. Those matter for asynchronous signals;
 * a fault is delivered to the thread that caused it, at the moment it
 * causes it.
 */

#define SIG64_TRAP    5
#define SIG64_KILLSIG 9
#define SIG64_SEGV   11
#define SIG64_TERM   15
#define SIG64_CHLD   17
#define SIG64_CONT   18
#define SIG64_STOP   19
#define SIG64_TSTP   20
#define SIG64_TTIN   21
#define SIG64_TTOU   22
#define SIG64_URG    23
#define SIG64_WINCH  28

#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTORER  0x04000000

/* sigaltstack(2)'s stack_t, in Linux's layout. */
typedef struct {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  __pad;
    uint64_t ss_size;
} altstack64_t;

#define SS_ONSTACK   1
#define SS_DISABLE   2

/* struct sigcontext_64, which is what uc_mcontext is. Field order is
 * the kernel's and must not be tidied. */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;
    uint64_t reserved[8];
} sigcontext64_t;

typedef struct {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp;          /* stack_t uc_stack */
    uint32_t ss_flags;
    uint32_t __pad0;
    uint64_t ss_size;
    sigcontext64_t uc_mcontext;
    uint64_t uc_sigmask[16];
} ucontext64_t;

/* What the kernel pushes on the user stack. The handler returns into
 * pretcode, which is the restorer, which calls rt_sigreturn. */
typedef struct {
    uint64_t     pretcode;
    ucontext64_t uc;
    uint64_t     info[16];   /* siginfo_t, only partly filled */
} rt_sigframe64_t;

/* The kernel's struct sigaction, which is NOT glibc's. */
typedef struct {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
} ksigaction64_t;

/* SIG_IGN as the kernel sees it: Linux's sigaction passes 1 for it, and
 * it is not an address to jump to. */
#define SIG64_IGN 1ULL

void signal64_reset(void);

/* The handler table is per process. fork copies it; execve puts every
 * caught signal back to its default and leaves ignored ones ignored. */
void signal64_fork(int parent_pid, int child_pid);
void signal64_exec(void);

/* sigaltstack(2). Either pointer may be null. Returns 0 or a negative
 * errno.
 *
 * Not decoration, and not only for programs that overflow their stacks
 * on purpose: a thread that faults *because* its stack pointer is bad
 * cannot be handed a frame below that stack pointer, and the kernel
 * writing one there faults in ring 0. Wine asks for an alternate stack
 * at every thread start, in init_thread_pipe, and until now the answer
 * was -ENOSYS. */
int  signal64_sigaltstack(const altstack64_t* ss, altstack64_t* oss,
                          uint64_t cur_rsp);

int  signal64_sigaction(int sig, const ksigaction64_t* act,
                        ksigaction64_t* oact);

/* Rewrites `r` to enter the handler for `sig`, pushing a frame on the
 * faulting thread's own stack. Returns 0 if there is no handler, in
 * which case the caller must treat the fault as fatal. */
int  signal64_deliver(int sig, registers64_t* r, uint64_t fault_addr);

/* rt_sigreturn: rebuild a register set from the frame the handler is
 * returning off. `user_rsp` is the thread's rsp at the syscall, which
 * points at the ucontext. */
int  signal64_sigreturn(uint64_t user_rsp, registers64_t* out);

/* Report every signal this kernel hands to a handler: which signal, in
 * which process, and - the part that matters - where the *original*
 * fault was, rather than where the handler later died. Off by default,
 * because a program servicing its own pages through SIGSEGV would bury
 * the transcript. */
void signal64_set_trace(int on);

/* --- kill(2): a signal from one process to another ------------------
 *
 * Everything above this point is synchronous: a fault, delivered to the
 * thread that caused it, at the moment it causes it. kill(2) is the
 * other kind. The sender is not the target, so there is no frame to
 * rewrite when the call is made - the target may not even be on a CPU.
 * So the signal is recorded against the target and acted on when that
 * process next returns from a syscall, which is where Linux acts on one
 * too.
 */

/* Record `sig` against `pid`. Returns 0, or a negative errno. */
int  signal64_raise(int pid, int sig);

/* Whether anything is recorded against this process. */
int  signal64_has_pending(int pid);

/* The lowest-numbered signal recorded against it, removed from the set.
 * 0 when there is none. Lowest-first because that is the order Linux
 * delivers in, and it puts SIGKILL ahead of most things. */
int  signal64_take_pending(int pid);

/* What this process would do with `sig` as things stand. The caller
 * cannot work this out from signal64_deliver's return value, because
 * that answers 0 both for "nothing is installed" and for "SIG_IGN" -
 * which is right for a fault, where either way the process dies, and
 * badly wrong for kill(2), where one of them means discard it. */
#define SIG64_DISP_TERM     0   /* default action: end the process     */
#define SIG64_DISP_IGNORE   1   /* discard it                          */
#define SIG64_DISP_HANDLER  2   /* enter the installed handler         */
int  signal64_disposition(int pid, int sig);

uint64_t signal64_delivered(void);
uint64_t signal64_returns(void);
uint64_t signal64_raised(void);

#endif
