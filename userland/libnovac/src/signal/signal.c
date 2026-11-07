/* Signals: sigaction and friends over rt_sigaction(13), rt_sigprocmask(14),
 * kill(62) and tgkill(234), which NovarisOS implements with Linux's ABI. */
#include <signal.h>
#include <errno.h>
#include <stddef.h>
#include "../internal/syscall.h"

#define SA_RESTORER 0x04000000

/* Where a handler returns to. The kernel puts this address in the signal
 * frame's return slot; it must make the rt_sigreturn call with the frame
 * still where the kernel left it, so it is a bare instruction sequence,
 * never a C function with a prologue. */
void novac__sigreturn(void);
__asm__(
    ".text\n"
    ".globl novac__sigreturn\n"
    ".type novac__sigreturn,@function\n"
    "novac__sigreturn:\n"
    "    movq $15, %rax\n"            /* NOVAC_SYS_rt_sigreturn */
    "    syscall\n"
    "    hlt\n"
);

/* The kernel's struct sigaction - not the C library's. */
struct ksigaction {
    void         *handler;
    unsigned long flags;
    void        (*restorer)(void);
    sigset_t      mask;
};

static int valid(int sig) { return sig >= 1 && sig < NSIG; }

int sigaction(int sig, const struct sigaction *restrict act,
              struct sigaction *restrict old)
{
    struct ksigaction k, ko;
    long r;

    if (!valid(sig)) { errno = EINVAL; return -1; }
    if (act) {
        k.handler  = (void *)act->sa_handler;
        k.flags    = (unsigned long)(unsigned int)act->sa_flags | SA_RESTORER;
        k.restorer = novac__sigreturn;
        k.mask     = act->sa_mask;
    }
    r = novac_ret(novac_syscall6(NOVAC_SYS_rt_sigaction, sig,
                                 act ? (long)&k : 0, old ? (long)&ko : 0,
                                 sizeof(sigset_t), 0, 0));
    if (r < 0) return -1;
    if (old) {
        old->sa_handler = (void (*)(int))ko.handler;
        old->sa_mask    = ko.mask;
        old->sa_flags   = (int)(ko.flags & ~(unsigned long)SA_RESTORER);
    }
    return 0;
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction sa, old;
    sa.sa_handler = handler;
    sa.sa_mask    = 0;
    sa.sa_flags   = SA_RESTART;
    if (sigaction(sig, &sa, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

int kill(pid_t pid, int sig)
{
    return (int)novac_sys2(NOVAC_SYS_kill, pid, sig);
}

/* To this thread, as POSIX asks of raise(): delivered before raise
 * returns, unless it is blocked. */
int raise(int sig)
{
    long pid = novac_syscall6(NOVAC_SYS_getpid, 0, 0, 0, 0, 0, 0);
    long tid = novac_syscall6(NOVAC_SYS_gettid, 0, 0, 0, 0, 0, 0);
    return (int)novac_sys3(NOVAC_SYS_tgkill, pid, tid, sig);
}

int sigprocmask(int how, const sigset_t *restrict set, sigset_t *restrict old)
{
    if (set && how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
        errno = EINVAL;
        return -1;
    }
    return (int)novac_ret(novac_syscall6(NOVAC_SYS_rt_sigprocmask, how,
                                         (long)set, (long)old,
                                         sizeof(sigset_t), 0, 0));
}

int sigemptyset(sigset_t *set) { if (!set) { errno = EINVAL; return -1; } *set = 0; return 0; }
int sigfillset(sigset_t *set)  { if (!set) { errno = EINVAL; return -1; } *set = ~0UL; return 0; }

int sigaddset(sigset_t *set, int sig)
{
    if (!set || !valid(sig)) { errno = EINVAL; return -1; }
    *set |= 1UL << (sig - 1);
    return 0;
}

int sigdelset(sigset_t *set, int sig)
{
    if (!set || !valid(sig)) { errno = EINVAL; return -1; }
    *set &= ~(1UL << (sig - 1));
    return 0;
}

int sigismember(const sigset_t *set, int sig)
{
    if (!set || !valid(sig)) { errno = EINVAL; return -1; }
    return (*set >> (sig - 1)) & 1;
}

/* abort()'s half that needs signals (src/stdlib/exit.c calls it through a
 * weak reference). SIGABRT unblocked and raised; if a handler caught it
 * and returned, the default action is restored and it is raised again, so
 * abort() never returns - POSIX's rule. */
void novac__raise_abort(void)
{
    sigset_t s = 1UL << (SIGABRT - 1);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    raise(SIGABRT);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}
