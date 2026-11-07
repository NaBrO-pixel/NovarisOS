/* tgkill64.c - a signal addressed to one thread.
 *
 * Milestone 96. tkill and tgkill used to begin
 *
 *     if (tid != proc64_current_pid()) return -ESRCH;
 *
 * which is wrong twice over. A tid is not a pid - gettid(2) returns a
 * task slot and getpid(2) a process - so the comparison only ever
 * agreed by accident, for a process with one thread. And a thread that
 * is not the caller could not be signalled at all.
 *
 * That second half is where chrome.exe stopped. Chromium's
 * StackSamplingProfiler suspends the main thread; Wine implements
 * NtSuspendThread by sending the target SIGUSR1 and then asking it for
 * its register context; -ESRCH to that tgkill left the wineserver
 * believing the thread had died - it clears unix_pid and unix_tid when
 * a signal comes back ESRCH - while the thread itself ran on, marked
 * suspended forever. A suspended thread is not allowed to acquire
 * anything, so every wait it made afterwards was answered PENDING. The
 * visible symptom was chrome.exe waiting for eternity on a mutex it had
 * just created unowned, which was signalled the whole time.
 *
 * So the two things this checks are the two things that were broken:
 * the signal reaches the named thread and no other, and it reaches it
 * where it sleeps.
 *
 * Every expected answer below is Linux's. The calls go through
 * syscall(2) rather than a library wrapper, because glibc has no
 * tgkill() to call.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>

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

static int sys_tgkill(int tgid, int tid, int sig)
{
    return (int)syscall(SYS_tgkill, tgid, tid, sig);
}

static int sys_tkill(int tid, int sig)
{
    return (int)syscall(SYS_tkill, tid, sig);
}

static int sys_gettid(void)
{
    return (int)syscall(SYS_gettid);
}

/* What the handlers record. volatile because the only writes are from a
 * signal handler and the only reads are not. */
static volatile sig_atomic_t usr1_count, usr2_count;
static volatile sig_atomic_t usr1_tid,   usr2_tid;

static void on_usr1(int s) { (void)s; usr1_tid = sys_gettid(); usr1_count++; }
static void on_usr2(int s) { (void)s; usr2_tid = sys_gettid(); usr2_count++; }

/* The second thread, and what it reports back. */
static volatile sig_atomic_t helper_tid, helper_ready, helper_done;
static volatile sig_atomic_t helper_read_ret, helper_read_errno;
static int helper_pipe[2];

static void *helper(void *arg)
{
    char c;
    ssize_t n;

    (void)arg;
    helper_tid   = sys_gettid();
    helper_ready = 1;

    /* Parks here. Nothing ever writes to this pipe and both ends stay
     * open, so the only way out is a signal - which is the whole point:
     * the thread the wineserver stops is usually asleep in a read on
     * its wait descriptor, and a signal that waits for the sleeper to
     * wake up by itself never arrives. */
    errno = 0;
    n = read(helper_pipe[0], &c, 1);
    helper_read_ret   = (sig_atomic_t)n;
    helper_read_errno = (sig_atomic_t)errno;

    helper_done = 1;
    return NULL;
}

/* A tid and a pid that exist on neither machine. Linux's pid_max is
 * 4194304 at the very most, and this is above it. */
#define NO_SUCH_ID 4194305

int main(void)
{
    struct sigaction sa;
    pthread_t th;
    int mypid = (int)getpid();
    int mytid = sys_gettid();
    int r;

    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                 /* no SA_RESTART: read(2) must end */
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_usr2;
    sigaction(SIGUSR2, &sa, NULL);

    ok("gettid is positive", mytid > 0);

    /* The existence check. Signal 0 sends nothing and reports whether
     * the target could have been signalled. */
    ok("tgkill(self, self, 0) returns 0", sys_tgkill(mypid, mytid, 0) == 0);
    ok("tkill(self, 0) returns 0",        sys_tkill(mytid, 0) == 0);

    /* A thread naming itself. raise() and abort() are this, and both
     * expect the handler to have run by the time the call returns -
     * not at some later syscall. */
    ok("tgkill(self, self, SIGUSR1) returns 0",
       sys_tgkill(mypid, mytid, SIGUSR1) == 0);
    ok("and the handler has already run", usr1_count == 1);
    ok("on the calling thread", usr1_tid == mytid);

    ok("tkill(self, SIGUSR1) returns 0", sys_tkill(mytid, SIGUSR1) == 0);
    ok("and it ran again", usr1_count == 2);

    /* The arguments that are refused. */
    errno = 0;
    r = sys_tgkill(mypid, mytid, -1);
    ok("tgkill with a negative signal is EINVAL", r == -1 && errno == EINVAL);
    errno = 0;
    r = sys_tgkill(mypid, mytid, 100);
    ok("tgkill with a signal above the range is EINVAL",
       r == -1 && errno == EINVAL);
    errno = 0;
    r = sys_tgkill(mypid, 0, SIGUSR1);
    ok("tgkill with tid 0 is EINVAL", r == -1 && errno == EINVAL);
    errno = 0;
    r = sys_tgkill(0, mytid, SIGUSR1);
    ok("tgkill with tgid 0 is EINVAL", r == -1 && errno == EINVAL);
    errno = 0;
    r = sys_tgkill(-1, mytid, SIGUSR1);
    ok("tgkill with a negative tgid is EINVAL", r == -1 && errno == EINVAL);
    errno = 0;
    r = sys_tkill(0, SIGUSR1);
    ok("tkill with tid 0 is EINVAL", r == -1 && errno == EINVAL);

    /* The ones that are looked up and not found. */
    errno = 0;
    r = sys_tgkill(mypid, NO_SUCH_ID, SIGUSR1);
    ok("tgkill at a tid that does not exist is ESRCH",
       r == -1 && errno == ESRCH);
    errno = 0;
    r = sys_tkill(NO_SUCH_ID, SIGUSR1);
    ok("tkill at a tid that does not exist is ESRCH",
       r == -1 && errno == ESRCH);

    /* The check that is the whole reason tgkill exists alongside tkill:
     * the tid must be in the named thread group. A tid recycled into
     * another process must not be signalled by a caller that meant the
     * one before it. */
    errno = 0;
    r = sys_tgkill(NO_SUCH_ID, mytid, SIGUSR1);
    ok("tgkill with a tgid the tid is not in is ESRCH",
       r == -1 && errno == ESRCH);

    ok("none of the refused calls delivered anything", usr1_count == 2);

    /* And now the thread. */
    ok("a pipe for it", pipe(helper_pipe) == 0);
    ok("a second thread started", pthread_create(&th, NULL, helper, NULL) == 0);

    {
        int spins = 0;
        while (!helper_ready && spins++ < 500) nap(10);
    }
    ok("it reported a tid of its own", helper_ready && helper_tid > 0);
    ok("and it is not the main thread's", helper_tid != mytid);

    /* It is certainly parked in read(2) by now. */
    nap(200);
    ok("it has not finished", helper_done == 0);

    ok("tgkill(self, other thread, SIGUSR2) returns 0",
       sys_tgkill(mypid, (int)helper_tid, SIGUSR2) == 0);

    {
        int spins = 0;
        while (!helper_done && spins++ < 500) nap(10);
    }
    ok("the other thread came out of read(2)", helper_done == 1);

    pthread_join(th, NULL);

    ok("the handler ran once", usr2_count == 1);
    ok("on the thread that was named", usr2_tid == helper_tid);
    ok("and not on the one that sent it", usr2_tid != mytid);
    ok("the main thread's own count is untouched", usr1_count == 2);

    /* What an interrupted read(2) reports. The handler was installed
     * without SA_RESTART, so the call ends rather than being restarted
     * underneath the program. */
    ok("read(2) returned -1", helper_read_ret == (sig_atomic_t)-1);
    ok("with EINTR", helper_read_errno == EINTR);

    close(helper_pipe[0]);
    close(helper_pipe[1]);

    printf("tgkill64: %d failures\n", failures);
    return failures ? 1 : 127;
}
