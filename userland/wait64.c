/* wait64.c - what wait4's first and third arguments mean.
 *
 * Milestone 94. This kernel's wait4 read its caller's pid and options
 * and used neither, which made it "reap whichever child has exited, and
 * block until one does" wearing the name of a call that means neither
 * of those things.
 *
 * WNOHANG is the one that gets noticed. A poll loop written against it
 * does not poll: it blocks on the first turn, so a supervisor asking
 * "has anything finished?" instead says "wait until something does".
 * Chromium's child management is that loop.
 *
 * The pid is the quieter failure and the worse one. waitpid(child_a)
 * would reap child_b, report child_b's pid, and destroy the status that
 * child_a's parent was waiting for - and nothing anywhere would say so.
 * Two children, exiting with different codes, is the only way to catch
 * it, so that is what the second half of this does.
 *
 * Every expected answer is Linux's, reached through syscall(2) rather
 * than glibc's wrappers so that what is compared is the ABI.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
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

static int sys_wait4(int pid, int *status, int options)
{
    return (int)syscall(SYS_wait4, pid, status, options, NULL);
}

static void nap(long ms)
{
    struct timespec t;
    t.tv_sec  = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    syscall(SYS_nanosleep, &t, NULL);
}

int main(void)
{
    int status, rc;
    pid_t a, b, seen;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* --- no children at all ----------------------------------------- *
     *
     * ECHILD either way: WNOHANG changes whether a caller waits, not
     * what it is told about a family it does not have. */

    errno = 0;
    ok("wait4 with no children is ECHILD",
       sys_wait4(-1, &status, 0) == -1 && errno == ECHILD);

    errno = 0;
    ok("and WNOHANG does not change that",
       sys_wait4(-1, &status, WNOHANG) == -1 && errno == ECHILD);

    /* --- a child that has not finished -------------------------------- *
     *
     * The whole point. There is a child, it is alive, and the caller
     * asked not to wait: 0 is the answer, and it is neither an error nor
     * a pid. A kernel that ignores WNOHANG blocks here instead, and the
     * difference between those two is invisible until you time it. */

    a = fork();
    if (a == 0) {
        nap(400);
        _exit(11);
    }
    ok("fork produced a child", a > 0);

    nap(50);                       /* let it get going */

    status = 0;
    rc = sys_wait4(-1, &status, WNOHANG);
    ok("WNOHANG on a live child returns 0", rc == 0);

    rc = sys_wait4(a, &status, WNOHANG);
    ok("and returns 0 when asked about it by pid", rc == 0);

    /* Blocking now, which must still work - the flag is the difference,
     * not the mechanism. */
    status = 0;
    ok("a blocking wait then reaps it", sys_wait4(a, &status, 0) == a);
    ok("with the status it exited with",
       WIFEXITED(status) && WEXITSTATUS(status) == 11);

    errno = 0;
    ok("and it is gone afterwards",
       sys_wait4(a, &status, WNOHANG) == -1 && errno == ECHILD);

    /* --- WNOHANG on a child that has already finished ------------------ *
     *
     * Reaps it and reports it, exactly as a blocking wait would. The
     * flag says "do not wait", not "do not reap". */

    a = fork();
    if (a == 0)
        _exit(12);

    nap(200);                      /* it is certainly gone by now */

    status = 0;
    ok("WNOHANG on a finished child reaps it",
       sys_wait4(-1, &status, WNOHANG) == a);
    ok("and reports its status",
       WIFEXITED(status) && WEXITSTATUS(status) == 12);

    /* --- the pid argument --------------------------------------------- *
     *
     * Two children with different exit codes. Waiting for the second one
     * by pid must not reap the first, and must not report the first's
     * code - which is exactly what a wait4 that ignores its pid does,
     * because the first is the one sitting at the front of the table. */

    a = fork();
    if (a == 0) {
        nap(100);
        _exit(21);
    }
    b = fork();
    if (b == 0) {
        nap(300);
        _exit(22);
    }
    ok("two children", a > 0 && b > 0 && a != b);

    nap(600);                      /* both have finished */

    status = 0;
    seen = sys_wait4(b, &status, 0);
    ok("waiting for the second by pid returns that pid", seen == b);
    ok("and its exit code, not the other one's",
       WIFEXITED(status) && WEXITSTATUS(status) == 22);

    /* The first is untouched: still reapable, still carrying its own
     * code. If the pid had been ignored above, this one is already gone
     * and this call reports ECHILD instead. */
    status = 0;
    seen = sys_wait4(a, &status, 0);
    ok("the first child was left alone", seen == a);
    ok("and still has its own exit code",
       WIFEXITED(status) && WEXITSTATUS(status) == 21);

    /* --- a pid that is not this process's child ------------------------ *
     *
     * ECHILD, not ESRCH: wait(2) is asked about children, and a process
     * that exists but is not one is the same to it as one that does not
     * exist at all. */
    errno = 0;
    ok("waiting for pid 1, which is not our child, is ECHILD",
       sys_wait4(1, &status, WNOHANG) == -1 && errno == ECHILD);

    printf("wait64: %d failures\n", failures);
    return failures ? 1 : 127;
}
