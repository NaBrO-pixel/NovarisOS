/* kill64.c - one process signalling another.
 *
 * Milestone 93. Until now this kernel's only signal calls were tkill and
 * tgkill, and both of them begin
 *
 *     if (tid != proc64_current_pid()) return -ESRCH;
 *
 * so nothing here could signal anything but itself. kill(2) was not
 * merely missing a case statement: a signal sent to another process has
 * no frame to rewrite, because the target may be blocked, or runnable,
 * or on no CPU at all. It has to be recorded and acted on later, and
 * "later" has to be a place where ending a process is expressible.
 *
 * chrome.exe asked for kill(2) twice in a run and was told -ENOSYS both
 * times. A process tree is the thing Chromium is.
 *
 * Every expected answer below is Linux's. The calls go through
 * syscall(2) rather than glibc's wrappers so that what is compared is
 * the kernel ABI and not the library's opinion of it - glibc's kill()
 * is a thin wrapper today and might not be tomorrow, and raise() is not
 * a wrapper at all.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

static int sys_kill(int pid, int sig)
{
    return (int)syscall(SYS_kill, pid, sig);
}

/* What the handlers below record. volatile because the only writes are
 * from a signal handler and the only reads are not. */
static volatile sig_atomic_t got_usr1, got_usr2, got_term;

static void on_usr1(int s) { (void)s; got_usr1++; }
static void on_usr2(int s) { (void)s; got_usr2++; }
static void on_term(int s) { (void)s; got_term++; }

/* A pid that exists on neither machine. Linux's pid_max is 4194304 at
 * the very most, and this is above it. */
#define NO_SUCH_PID 4194305

int main(void)
{
    struct sigaction sa;
    int status;
    pid_t child;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* --- signal 0: the existence check ------------------------------ *
     *
     * kill(pid, 0) sends nothing and reports whether the process is
     * there. It is how every supervisor in the world polls a child, and
     * it is the one form of kill that a kernel with no signals at all
     * could still answer. */

    ok("kill(self, 0) succeeds", sys_kill(getpid(), 0) == 0);

    errno = 0;
    ok("kill(nonexistent, 0) is ESRCH",
       sys_kill(NO_SUCH_PID, 0) == -1 && errno == ESRCH);

    errno = 0;
    ok("kill(self, 99) is EINVAL - there is no signal 99",
       sys_kill(getpid(), 99) == -1 && errno == EINVAL);

    errno = 0;
    ok("kill(self, -1) is EINVAL",
       sys_kill(getpid(), -1) == -1 && errno == EINVAL);

    /* --- to itself, with a handler ----------------------------------- *
     *
     * The handler must have run by the time kill returns. That is not a
     * detail: raise() and abort() are built on it, and a kernel that
     * defers a self-signal to some later syscall breaks both. */

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);

    got_usr1 = 0;
    ok("kill(self, SIGUSR1) returns 0", sys_kill(getpid(), SIGUSR1) == 0);
    ok("and the handler had already run when it returned", got_usr1 == 1);

    /* Twice, because a pending-set implementation that forgets to clear
     * the bit delivers the second one for ever. */
    ok("a second one is delivered too", sys_kill(getpid(), SIGUSR1) == 0);
    ok("and exactly once more", got_usr1 == 2);

    /* --- to itself, ignored ------------------------------------------ *
     *
     * SIG_IGN is not "no handler". A kernel that treats them the same
     * applies the default action - which for SIGUSR2 is to die - to a
     * process that explicitly asked for the signal to be discarded. */

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGUSR2, &sa, NULL);

    got_usr2 = 0;
    ok("kill(self, SIGUSR2) with SIG_IGN returns 0",
       sys_kill(getpid(), SIGUSR2) == 0);
    ok("and no handler ran", got_usr2 == 0);
    ok("and the process is still here to say so", 1);

    /* --- a signal whose default action is to ignore ------------------ *
     *
     * SIGCHLD with nothing installed. Getting this wrong is not subtle:
     * every process whose child exits receives one, so a kernel that
     * treats an uncaught SIGCHLD as fatal kills every parent it has. */

    ok("kill(self, SIGCHLD) with no handler returns 0",
       sys_kill(getpid(), SIGCHLD) == 0);
    ok("and did not end the process", 1);

    /* --- to another process ------------------------------------------ *
     *
     * The real thing. The child installs a handler, says so, and waits;
     * the parent signals it and the child exits 7 to prove the handler
     * ran in the child rather than in the parent. */

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);

    child = fork();
    if (child == 0) {
        struct timespec t = { 0, 20000000L };          /* 20ms */
        int spins = 0;

        /* The parent's handlers were inherited, which is what makes the
         * next line a test of the child's own disposition rather than of
         * fork. Waiting in nanosleep rather than spinning, because a
         * signal is acted on when the target returns from a syscall. */
        while (!got_term && spins++ < 500)
            syscall(SYS_nanosleep, &t, NULL);

        _exit(got_term ? 7 : 8);
    }
    ok("fork produced a child", child > 0);

    if (child > 0) {
        struct timespec t = { 0, 20000000L };

        /* Give it time to reach its loop. Sending too early is not an
         * error - the signal would be pending and taken at the child's
         * next syscall - but it makes a failure ambiguous. */
        syscall(SYS_nanosleep, &t, NULL);

        ok("kill(child, SIGTERM) returns 0", sys_kill(child, SIGTERM) == 0);

        /* A blocking wait rather than a WNOHANG poll, deliberately:
         * this kernel's wait4 ignores its options argument, so WNOHANG
         * does not return 0 there the way it does on Linux, and a test
         * of kill(2) should not also be a test of that. Blocking is the
         * behaviour both agree on. */
        status = 0;
        waitpid(child, &status, 0);

        ok("the child exited", WIFEXITED(status));
        ok("and its handler is the one that ran (exit 7)",
           WIFEXITED(status) && WEXITSTATUS(status) == 7);
        ok("and the parent's own handler did not run", got_term == 0);
    }

    /* --- a child killed rather than handled --------------------------- *
     *
     * SIGKILL cannot be caught, so the child dies whatever it installed,
     * and wait(2) must report that as a death rather than as an exit.
     * The distinction is the whole reason WIFSIGNALED exists: a parent
     * reading the raw status - which is what a process supervisor does -
     * sees 0x0009 for a killed child and 0x0900 for one that exited 9. */

    child = fork();
    if (child == 0) {
        struct timespec t = { 0, 20000000L };
        int spins = 0;
        /* Tries to catch SIGKILL, which is not allowed to work. */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sigaction(SIGKILL, &sa, NULL);
        while (spins++ < 500)
            syscall(SYS_nanosleep, &t, NULL);
        _exit(3);
    }

    if (child > 0) {
        struct timespec t = { 0, 20000000L };

        syscall(SYS_nanosleep, &t, NULL);
        ok("kill(child, SIGKILL) returns 0", sys_kill(child, SIGKILL) == 0);

        status = 0;
        waitpid(child, &status, 0);

        ok("the child was killed, not exited", WIFSIGNALED(status));
        ok("and the signal was SIGKILL",
           WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        ok("and it is not reported as a normal exit", !WIFEXITED(status));
    }

    /* --- a pid that is gone ------------------------------------------- *
     *
     * Reaped, so the pid is no longer a process. ESRCH, not success. */
    errno = 0;
    ok("kill(reaped child, 0) is ESRCH",
       sys_kill(child, 0) == -1 && errno == ESRCH);

    printf("kill64: %d failures\n", failures);
    return failures ? 1 : 127;
}
