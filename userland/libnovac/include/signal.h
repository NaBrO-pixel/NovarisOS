/* <signal.h>: POSIX signals on NovarisOS, which delivers them with Linux's
 * x86-64 frame layout and numbering (kernel/signal64.c). */
#ifndef NOVAC_SIGNAL_H
#define NOVAC_SIGNAL_H
#include <sys/types.h>
#include <novac/defs.h>

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define NSIG      65

typedef int sig_atomic_t;
typedef unsigned long sigset_t;        /* the kernel's 64-signal set */

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 0x00000001
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int __pad0;
    union {
        struct { pid_t si_pid; uid_t si_uid; } __kill;
        void *si_addr;
        char __size[112];
    } __u;
} siginfo_t;
#define si_pid  __u.__kill.si_pid
#define si_uid  __u.__kill.si_uid
#define si_addr __u.si_addr

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __sa;
    sigset_t sa_mask;
    int      sa_flags;
};
#define sa_handler   __sa.sa_handler
#define sa_sigaction __sa.sa_sigaction

NOVAC_BEGIN_DECLS
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct sigaction *restrict act,
              struct sigaction *restrict old);
int raise(int sig);
int kill(pid_t pid, int sig);
int sigprocmask(int how, const sigset_t *restrict set, sigset_t *restrict old);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
NOVAC_END_DECLS
#endif
