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
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

static void nap(long ms)
{
    struct timespec t;
    t.tv_sec  = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    syscall(SYS_nanosleep, &t, NULL);
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

/* For the signal-mask tests: what the mask looked like from inside a
 * handler. */
static volatile sig_atomic_t inside_usr1, inside_usr2, mask_handler_ran;

static void on_usr1_look(int s)
{
    sigset_t cur;
    (void)s;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    inside_usr1 = sigismember(&cur, SIGUSR1);
    inside_usr2 = sigismember(&cur, SIGUSR2);
    mask_handler_ran++;
}

/* A SIGSEGV handler that faults itself. On Linux the second fault finds
 * SIGSEGV blocked - the handler is running - and the process dies of
 * it; it is not delivered again. */
static void on_segv_fault_again(int s)
{
    (void)s;
    *(volatile int *)0 = 2;
}

static int is_blocked(int sig)
{
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    return sigismember(&cur, sig);
}

static int is_pending(int sig)
{
    sigset_t p;
    sigpending(&p);
    return sigismember(&p, sig);
}

static long ms_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

/* For the alternate-stack test: each thread's own alternate stack, and
 * where its handler's locals actually were. */
#define ALT_SIZE (64 * 1024)
static char *alt_stack[2];
static volatile uintptr_t handler_sp[2];
static __thread int thread_idx;
static pthread_barrier_t alt_barrier;

static void on_usr1_where(int s)
{
    volatile int local;
    (void)s;
    handler_sp[thread_idx] = (uintptr_t)&local;
}

static int tgkill_self(int sig);

static void *alt_thread(void *arg)
{
    stack_t ss;

    thread_idx = (int)(intptr_t)arg;
    ss.ss_sp = alt_stack[thread_idx];
    ss.ss_size = ALT_SIZE;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    /* Both threads have registered before either takes a signal: one
     * alternate stack per process would put both on the second's. */
    pthread_barrier_wait(&alt_barrier);
    if (thread_idx == 1)
        pthread_barrier_wait(&alt_barrier);     /* let thread 0 go first */
    tgkill_self(SIGUSR1);
    if (thread_idx == 0)
        pthread_barrier_wait(&alt_barrier);
    return NULL;
}

static int on_alt(int i)
{
    uintptr_t lo = (uintptr_t)alt_stack[i];
    return handler_sp[i] >= lo && handler_sp[i] < lo + ALT_SIZE;
}

static int tgkill_self(int sig)
{
    return (int)syscall(SYS_tgkill, getpid(), (int)syscall(SYS_gettid), sig);
}

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

    /* --- a signal to a process that is asleep ------------------------ *
     *
     * The child blocks in read(2) on a pipe nobody will ever write to.
     * That is not a spin and not a timeout: it is parked in the kernel,
     * and it will never reach the boundary where a pending signal is
     * taken unless something wakes it. Linux interrupts a blocking
     * syscall when a signal arrives for exactly this reason.
     *
     * Without that, kill(2) to a sleeping process is a signal nothing
     * ever delivers, and the only way to see it is a test that blocks
     * for real rather than sleeping in a loop. */
    {
        int pipefd[2];
        pid_t sleeper;
        int status = 0;

        if (pipe(pipefd) == 0) {
            sleeper = fork();
            if (sleeper == 0) {
                char c;
                ssize_t n;
                close(pipefd[1]);         /* this copy; the parent holds one */
                n = read(pipefd[0], &c, 1);   /* parks here for good */
                _exit(n == 0 ? 41 : 42);
            }

            ok("a child that blocks in read exists", sleeper > 0);
            /* The parent keeps the write end open on purpose. Closing
             * it here would give the child end-of-file instead of a
             * wait, and a child that returns from read(2) is not
             * testing what this is for - the host caught exactly that
             * and reported the child exiting 41 rather than dying. */
            nap(100);                     /* it is certainly parked */

            ok("kill(sleeping child, SIGKILL) returns 0",
               sys_kill(sleeper, SIGKILL) == 0);

            waitpid(sleeper, &status, 0);
            ok("and the signal reached it where it slept",
               WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            close(pipefd[0]);
        }
    }

    /* --- signal masks ---------------------------------------------- *
     *
     * rt_sigprocmask used to be accepted and ignored. Every answer here
     * is Linux's; the last one is the case that made it matter - a fault
     * inside a SIGSEGV handler, which this kernel delivered again
     * forever instead of ending the process. */
    {
        sigset_t set, none;
        int before;

        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);

        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        ok("sigprocmask(SIG_BLOCK, SIGUSR1) returns 0",
           sigprocmask(SIG_BLOCK, &set, NULL) == 0);
        ok("and reads back as blocked", is_blocked(SIGUSR1));

        before = got_usr1;
        ok("tgkill(self, SIGUSR1) while blocked returns 0",
           tgkill_self(SIGUSR1) == 0);
        ok("and the handler has not run", got_usr1 == before);
        ok("and sigpending shows it", is_pending(SIGUSR1));

        sigprocmask(SIG_UNBLOCK, &set, NULL);
        ok("unblocking runs it before sigprocmask returns",
           got_usr1 == before + 1);
        ok("and it is no longer pending", !is_pending(SIGUSR1));

        /* The process-directed form takes the same path. */
        sigprocmask(SIG_BLOCK, &set, NULL);
        before = got_usr1;
        ok("kill(self, SIGUSR1) while blocked returns 0",
           sys_kill(getpid(), SIGUSR1) == 0);
        ok("and the handler has not run either", got_usr1 == before);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
        ok("until it is unblocked", got_usr1 == before + 1);

        /* Inside a handler: the signal itself and sa_mask are blocked,
         * and the mask from before is back afterwards. */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1_look;
        sigemptyset(&sa.sa_mask);
        sigaddset(&sa.sa_mask, SIGUSR2);
        sigaction(SIGUSR1, &sa, NULL);
        mask_handler_ran = 0;
        tgkill_self(SIGUSR1);
        ok("a handler ran", mask_handler_ran == 1);
        ok("with its own signal blocked", inside_usr1);
        ok("and what sa_mask named", inside_usr2);
        ok("and after it returned neither is blocked",
           !is_blocked(SIGUSR1) && !is_blocked(SIGUSR2));

        sa.sa_flags = SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        tgkill_self(SIGUSR1);
        ok("with SA_NODEFER its own signal is not blocked", !inside_usr1);

        sigfillset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);
        ok("SIGKILL cannot be blocked", !is_blocked(SIGKILL));
        ok("nor SIGSTOP", !is_blocked(SIGSTOP));
        ok("though everything else can", is_blocked(SIGTERM));
        sigprocmask(SIG_SETMASK, &none, NULL);

        /* fork(2) hands the child its parent's mask. */
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        sigprocmask(SIG_BLOCK, &set, NULL);
        child = fork();
        if (child == 0)
            _exit(is_blocked(SIGUSR2) ? 0 : 1);
        waitpid(child, &status, 0);
        ok("a forked child inherits the mask",
           WIFEXITED(status) && WEXITSTATUS(status) == 0);
        sigprocmask(SIG_SETMASK, &none, NULL);

        /* A fault in a SIGSEGV handler ends the process. */
        child = fork();
        if (child == 0) {
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = on_segv_fault_again;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGSEGV, &sa, NULL);
            *(volatile int *)0 = 1;
            _exit(3);                       /* not reached on Linux */
        }
        waitpid(child, &status, 0);
        ok("a fault inside a SIGSEGV handler kills the process",
           WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    }

    /* --- rt_sigsuspend and ppoll ------------------------------------ *
     *
     * Both replace the mask for exactly the length of a wait. The
     * handler that ends the wait runs under the temporary mask, and the
     * old one is back when it returns. */
    {
        sigset_t usr1, empty, only_usr2;
        struct timespec t0, tmo;
        struct pollfd pf;
        int pipefd[2], r, e;
        pid_t parent = getpid();

        sigemptyset(&empty);
        sigemptyset(&usr1);
        sigaddset(&usr1, SIGUSR1);
        sigemptyset(&only_usr2);
        sigaddset(&only_usr2, SIGUSR2);

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1_look;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);

        /* Woken by another process. */
        sigprocmask(SIG_SETMASK, &usr1, NULL);
        mask_handler_ran = 0;
        child = fork();
        if (child == 0) {
            nap(50);
            sys_kill(parent, SIGUSR1);
            _exit(0);
        }
        errno = 0;
        r = sigsuspend(&only_usr2);
        e = errno;
        ok("sigsuspend returns -1", r == -1);
        ok("with EINTR", e == EINTR);
        ok("once the handler has run", mask_handler_ran == 1);
        ok("which ran under the temporary mask", inside_usr2);
        ok("and the old mask is back", is_blocked(SIGUSR1) && !is_blocked(SIGUSR2));
        waitpid(child, &status, 0);

        /* Already pending when it is called. */
        mask_handler_ran = 0;
        tgkill_self(SIGUSR1);
        ok("a blocked signal waits", mask_handler_ran == 0);
        r = sigsuspend(&empty);
        ok("sigsuspend takes one already pending at once",
           r == -1 && mask_handler_ran == 1);
        ok("and the mask is back", is_blocked(SIGUSR1));

        /* ppoll's timeout. */
        tmo.tv_sec = 0;
        tmo.tv_nsec = 50 * 1000000L;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        r = ppoll(NULL, 0, &tmo, NULL);
        ok("ppoll with nothing to wait for times out with 0", r == 0);
        ok("and not before its timeout", ms_since(&t0) >= 45);

        ok("a pipe for ppoll", pipe(pipefd) == 0);
        pf.fd = pipefd[0];
        pf.events = POLLIN;

        /* Ready, and the mask is left as it was. */
        ok("a byte in it", write(pipefd[1], "x", 1) == 1);
        r = ppoll(&pf, 1, &tmo, &empty);
        ok("ppoll on a ready pipe returns 1", r == 1 && (pf.revents & POLLIN));
        ok("and leaves the mask as it found it", is_blocked(SIGUSR1));
        {
            char c;
            ok("and the byte read back out", read(pipefd[0], &c, 1) == 1);
        }

        /* A signal the temporary mask lets through ends the wait. */
        mask_handler_ran = 0;
        tgkill_self(SIGUSR1);
        tmo.tv_sec = 1;
        tmo.tv_nsec = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        errno = 0;
        r = ppoll(&pf, 1, &tmo, &empty);
        e = errno;
        ok("ppoll is interrupted by a signal its mask unblocks",
           r == -1 && e == EINTR);
        ok("at once, not after its timeout", ms_since(&t0) < 500);
        ok("and the handler ran", mask_handler_ran == 1);
        ok("and the old mask is back after it", is_blocked(SIGUSR1));

        /* One the temporary mask still blocks does not. */
        mask_handler_ran = 0;
        tgkill_self(SIGUSR1);
        tmo.tv_sec = 0;
        tmo.tv_nsec = 50 * 1000000L;
        r = ppoll(&pf, 1, &tmo, &usr1);
        ok("ppoll whose mask blocks the signal times out", r == 0);
        ok("without running the handler", mask_handler_ran == 0);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        ok("which runs when it is unblocked", mask_handler_ran == 1);

        close(pipefd[0]);
        close(pipefd[1]);
    }

    /* --- sigaltstack is per thread --------------------------------- *
     *
     * Wine gives every thread its own signal stack. One per process put
     * every thread's signals on whichever registered last, and two
     * threads taking signals at once overwrote each other's frames:
     * winedevice.exe died of it in every first wineboot. */
    {
        pthread_t th[2];

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1_where;
        sa.sa_flags = SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        sigprocmask(SIG_SETMASK, &(sigset_t){0}, NULL);

        alt_stack[0] = malloc(ALT_SIZE);
        alt_stack[1] = malloc(ALT_SIZE);
        pthread_barrier_init(&alt_barrier, NULL, 2);
        ok("two threads with alternate stacks of their own",
           pthread_create(&th[0], NULL, alt_thread, (void *)0) == 0 &&
           pthread_create(&th[1], NULL, alt_thread, (void *)1) == 0);
        pthread_join(th[0], NULL);
        pthread_join(th[1], NULL);
        ok("the first thread's handler ran on its own alternate stack",
           on_alt(0));
        ok("and the second's on its own", on_alt(1));
        {
            stack_t cur;
            sigaltstack(NULL, &cur);
            ok("and the main thread still has none",
               (cur.ss_flags & SS_DISABLE) != 0);
        }
    }

    /* --- a forked child knows its own tid --------------------------- *
     *
     * glibc's fork asks the kernel to write the child's tid into the
     * child's thread descriptor (CLONE_CHILD_SETTID). A kernel that
     * ignores the flag leaves the parent's tid there. raise() cannot see
     * the difference - glibc asks gettid() when a thread signals itself -
     * but anything that records an owner does: a robust mutex's lock word
     * is the owner's tid as glibc has it, so in the child it must be the
     * child's gettid() and not the parent's. */
    {
        pthread_mutexattr_t ma;
        pthread_mutex_t m;
        int parent_tid = (int)syscall(SYS_gettid);

        pthread_mutexattr_init(&ma);
        pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
        pthread_mutex_init(&m, &ma);

        pthread_mutex_lock(&m);
        ok("a robust mutex records its owner's tid",
           (m.__data.__lock & 0x3fffffff) == parent_tid);
        pthread_mutex_unlock(&m);

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        sigprocmask(SIG_SETMASK, &(sigset_t){0}, NULL);

        child = fork();
        if (child == 0) {
            int before = got_usr1, bad = 0;
            int me = (int)syscall(SYS_gettid);
            pthread_mutex_lock(&m);
            if ((m.__data.__lock & 0x3fffffff) != me) bad |= 1;
            if (me == parent_tid) bad |= 1;
            pthread_mutex_unlock(&m);
            if (raise(SIGUSR1) != 0) bad |= 2;
            if (got_usr1 != before + 1) bad |= 4;
            _exit(bad);
        }
        waitpid(child, &status, 0);
        ok("in a forked child it records the child's tid, not the parent's",
           WIFEXITED(status) && !(WEXITSTATUS(status) & 1));
        ok("and the child's raise() returns 0",
           WIFEXITED(status) && !(WEXITSTATUS(status) & 2));
        ok("and runs its handler",
           WIFEXITED(status) && !(WEXITSTATUS(status) & 4));
    }

    /* --- ppoll writes back the time left ---------------------------- *
     *
     * Through the raw syscall: glibc's ppoll() copies the timeout to a
     * local, so the write-back is only visible here. */
    {
        struct pollfd pf;
        struct timespec t;
        int pipefd[2], e;
        long r;
        sigset_t empty;
        pid_t parent = getpid();

        sigemptyset(&empty);
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        sigprocmask(SIG_SETMASK, &empty, NULL);

        ok("a pipe for the ppoll write-back", pipe(pipefd) == 0);
        pf.fd = pipefd[0];
        pf.events = POLLIN;

        ok("a byte to make it ready", write(pipefd[1], "x", 1) == 1);
        t.tv_sec = 1;
        t.tv_nsec = 0;
        r = syscall(SYS_ppoll, &pf, 1, &t, NULL, 8);
        ok("ppoll on a ready pipe leaves most of a second",
           r == 1 && t.tv_sec == 0 ? t.tv_nsec > 900000000L
                                   : (r == 1 && t.tv_sec == 1 && t.tv_nsec == 0));
        {
            char c;
            ok("and the byte comes back out", read(pipefd[0], &c, 1) == 1);
        }

        t.tv_sec = 0;
        t.tv_nsec = 50 * 1000000L;
        r = syscall(SYS_ppoll, &pf, 1, &t, NULL, 8);
        ok("ppoll that times out leaves nothing",
           r == 0 && t.tv_sec == 0 && t.tv_nsec == 0);

        child = fork();
        if (child == 0) {
            nap(100);
            sys_kill(parent, SIGUSR1);
            _exit(0);
        }
        t.tv_sec = 2;
        t.tv_nsec = 0;
        errno = 0;
        r = syscall(SYS_ppoll, &pf, 1, &t, &empty, 8);
        e = errno;
        ok("ppoll interrupted by a signal returns EINTR", r == -1 && e == EINTR);
        ok("and leaves less than it was given",
           t.tv_sec < 2 && t.tv_sec >= 0 && t.tv_nsec >= 0 &&
           t.tv_nsec < 1000000000L);
        ok("but more than nothing", t.tv_sec >= 1 || t.tv_nsec > 0);
        waitpid(child, &status, 0);

        close(pipefd[0]);
        close(pipefd[1]);
    }

    /* --- rt_sigtimedwait ------------------------------------------- *
     *
     * Taking a signal as a return value instead of through a handler:
     * sigwait, sigwaitinfo and sigtimedwait are all this call. */
    {
        sigset_t usr1, usr2, empty;
        siginfo_t si;
        struct timespec t0, zero = {0, 0}, fifty = {0, 50 * 1000000L},
                        second = {1, 0};
        int r, e, before, got;
        pid_t parent = getpid();

        sigemptyset(&empty);
        sigemptyset(&usr1);
        sigaddset(&usr1, SIGUSR1);
        sigemptyset(&usr2);
        sigaddset(&usr2, SIGUSR2);

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);

        /* Pending already, sent to the thread. */
        sigprocmask(SIG_SETMASK, &usr1, NULL);
        before = got_usr1;
        tgkill_self(SIGUSR1);
        memset(&si, 0, sizeof(si));
        r = sigtimedwait(&usr1, &si, &zero);
        ok("sigtimedwait takes a pending blocked signal", r == SIGUSR1);
        ok("and says which", si.si_signo == SIGUSR1);
        /* SI_USER through glibc, which rewrites SI_TKILL from the
         * caller's own process to SI_USER; the raw call below shows what
         * the kernel said. */
        ok("and glibc reports it as SI_USER", si.si_code == SI_USER);
        ok("and by whom", si.si_pid == getpid());
        ok("without running the handler", got_usr1 == before);
        ok("and it is no longer pending", !is_pending(SIGUSR1));

        tgkill_self(SIGUSR1);
        memset(&si, 0, sizeof(si));
        r = (int)syscall(SYS_rt_sigtimedwait, &usr1, &si, &zero, 8);
        ok("the raw call says the kernel sent it to the thread (SI_TKILL)",
           r == SIGUSR1 && si.si_code == SI_TKILL);

        /* Sent to the process. */
        sys_kill(getpid(), SIGUSR1);
        memset(&si, 0, sizeof(si));
        r = sigtimedwait(&usr1, &si, &zero);
        ok("one sent with kill is taken too, as SI_USER",
           r == SIGUSR1 && si.si_code == SI_USER && si.si_pid == getpid());

        /* Nothing there. */
        errno = 0;
        r = sigtimedwait(&usr1, NULL, &zero);
        e = errno;
        ok("a zero timeout with nothing pending is EAGAIN",
           r == -1 && e == EAGAIN);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        errno = 0;
        r = sigtimedwait(&usr1, NULL, &fifty);
        e = errno;
        ok("a timeout that runs out is EAGAIN", r == -1 && e == EAGAIN);
        ok("and not before it", ms_since(&t0) >= 45);

        /* Woken by another process. */
        sigprocmask(SIG_SETMASK, &usr2, NULL);
        child = fork();
        if (child == 0) {
            nap(100);
            sys_kill(parent, SIGUSR2);
            _exit(0);
        }
        memset(&si, 0, sizeof(si));
        r = sigwaitinfo(&usr2, &si);
        ok("sigwaitinfo is woken by a signal from another process",
           r == SIGUSR2);
        ok("and names the process that sent it", si.si_pid == child);
        waitpid(child, &status, 0);

        /* Ended by a signal it was not waiting for. */
        before = got_usr1;
        child = fork();
        if (child == 0) {
            nap(100);
            sys_kill(parent, SIGUSR1);
            _exit(0);
        }
        errno = 0;
        r = sigtimedwait(&usr2, NULL, &second);
        e = errno;
        ok("a signal outside the set ends the wait with EINTR",
           r == -1 && e == EINTR);
        ok("and its handler runs", got_usr1 == before + 1);
        waitpid(child, &status, 0);

        /* sigwait, the POSIX spelling. */
        tgkill_self(SIGUSR2);
        got = 0;
        ok("sigwait returns 0", sigwait(&usr2, &got) == 0);
        ok("with the signal it took", got == SIGUSR2);

        sigprocmask(SIG_SETMASK, &empty, NULL);
    }

    printf("kill64: %d failures\n", failures);
    return failures ? 1 : 127;
}
