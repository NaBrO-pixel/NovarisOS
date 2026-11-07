/* prctl64.c - what a process calls itself.
 *
 * Milestone 95. chrome.exe's run asked for prctl three times and got
 * -ENOSYS every time. Wine calls it in three places and two of them are
 * PR_SET_NAME - dlls/ntdll/unix/env.c, when a process learns what it is
 * called, and loader/preloader.c, which is the only one of the three
 * that looks at the answer:
 *
 *     if (wld_prctl( 15 / * PR_SET_NAME * /, (long)name ) == -1) return;
 *
 * The third is PR_SET_PTRACER, which Wine's own comment calls a work
 * around for "Ubuntu's ptrace breakage". That is a Yama option, and a
 * kernel built without Yama answers EINVAL - which is what the host
 * this was measured on does, so it is not a gap to fill.
 *
 * Every expected answer below is Linux's, measured rather than
 * remembered. The truncation rule in particular is not what a reading
 * of the manual suggests: a name longer than the limit is quietly cut
 * to fifteen characters and a NUL, and the call still returns 0.
 *
 * Reached through syscall(2) rather than glibc's wrapper so that what is
 * compared is the kernel ABI.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

static long pr(long option, long arg2)
{
    return syscall(SYS_prctl, option, arg2, 0L, 0L, 0L);
}

#define PR_SET_NAME_ 15
#define PR_GET_NAME_ 16

/* An option no prctl will ever have. */
#define PR_NONSENSE  9999

int main(void)
{
    char buf[64];
    long rc;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* --- setting and reading back ------------------------------------ */

    ok("PR_SET_NAME returns 0", pr(PR_SET_NAME_, (long)"novaris") == 0);

    memset(buf, 0, sizeof(buf));
    ok("PR_GET_NAME returns 0", pr(PR_GET_NAME_, (long)buf) == 0);
    ok("and reads back what was set", strcmp(buf, "novaris") == 0);

    /* Twice, with a different name, because a kernel that sets the name
     * once and then ignores later calls passes the two above. */
    ok("a second name is accepted",
       pr(PR_SET_NAME_, (long)"second") == 0);
    memset(buf, 0, sizeof(buf));
    pr(PR_GET_NAME_, (long)buf);
    ok("and replaces the first", strcmp(buf, "second") == 0);

    /* --- the limit --------------------------------------------------- *
     *
     * Fifteen characters and a NUL. Sixteen is one too many and the
     * sixteenth is dropped - not refused, dropped, with the call still
     * reporting success. A caller that checks only the return value has
     * no way to know its name was shortened. */

    ok("a name of exactly fifteen characters is kept whole",
       pr(PR_SET_NAME_, (long)"exactly15chars!") == 0);
    memset(buf, 0, sizeof(buf));
    pr(PR_GET_NAME_, (long)buf);
    ok("and reads back all fifteen", strcmp(buf, "exactly15chars!") == 0);

    ok("a longer name is accepted, not refused",
       pr(PR_SET_NAME_, (long)"sixteenchars1234") == 0);
    memset(buf, 0, sizeof(buf));
    pr(PR_GET_NAME_, (long)buf);
    ok("and comes back cut to fifteen", strlen(buf) == 15);
    ok("keeping the front of it", strcmp(buf, "sixteenchars123") == 0);

    /* --- what it refuses ---------------------------------------------- */

    errno = 0;
    rc = pr(PR_SET_NAME_, 0);
    ok("PR_SET_NAME with a null pointer is EFAULT",
       rc == -1 && errno == EFAULT);

    errno = 0;
    rc = pr(PR_GET_NAME_, 0);
    ok("PR_GET_NAME with a null pointer is EFAULT",
       rc == -1 && errno == EFAULT);

    errno = 0;
    rc = pr(PR_NONSENSE, 0);
    ok("an option that does not exist is EINVAL",
       rc == -1 && errno == EINVAL);

    /* The name survived every refusal above. */
    memset(buf, 0, sizeof(buf));
    pr(PR_GET_NAME_, (long)buf);
    ok("and none of those changed the name",
       strcmp(buf, "sixteenchars123") == 0);

    /* --- across a fork ------------------------------------------------ *
     *
     * The child starts with its parent's name and can change its own
     * without the parent noticing. Both halves matter: a kernel keeping
     * one name for the machine passes the first and fails the second. */

    pr(PR_SET_NAME_, (long)"theparent");
    {
        pid_t child = fork();
        int status = 0;

        if (child == 0) {
            char cbuf[64];
            int bad = 0;

            memset(cbuf, 0, sizeof(cbuf));
            pr(PR_GET_NAME_, (long)cbuf);
            if (strcmp(cbuf, "theparent") != 0) bad |= 1;

            pr(PR_SET_NAME_, (long)"thechild");
            memset(cbuf, 0, sizeof(cbuf));
            pr(PR_GET_NAME_, (long)cbuf);
            if (strcmp(cbuf, "thechild") != 0) bad |= 2;

            _exit(bad);
        }

        ok("fork produced a child", child > 0);
        waitpid(child, &status, 0);
        ok("the child inherited the parent's name",
           WIFEXITED(status) && !(WEXITSTATUS(status) & 1));
        ok("and could set its own",
           WIFEXITED(status) && !(WEXITSTATUS(status) & 2));

        memset(buf, 0, sizeof(buf));
        pr(PR_GET_NAME_, (long)buf);
        ok("while the parent kept its own", strcmp(buf, "theparent") == 0);
    }

    printf("prctl64: %d failures\n", failures);
    return failures ? 1 : 127;
}
