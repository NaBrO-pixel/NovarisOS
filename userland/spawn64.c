/* spawn64.c - starting another program and getting it back.
 *
 * Milestone 73. `posix_spawn` is how Wine starts the wineserver, and on
 * x86-64 glibc implements it as
 *
 *     clone(CLONE_VM | CLONE_VFORK | SIGCHLD, child_stack)
 *
 * which is neither of the two things this kernel's `clone` knew about.
 * It has CLONE_VM, so it was taken for a thread and answered with a tid;
 * the caller then asked `wait4` for that tid and was told -ECHILD,
 * because a thread is not a child. wineboot got as far as building its
 * prefix and stopped there.
 *
 * What this asserts is the ordinary contract: a spawn produces a real
 * process, that process runs a different program, and its parent can
 * wait for it and be told how it ended.
 *
 * What it deliberately does NOT assert is the failure path. On Linux the
 * child reports a failed exec back to the parent through the memory the
 * two of them share, so `posix_spawn` itself returns ENOENT. This kernel
 * gives the child a copy-on-write copy rather than the parent's own
 * pages, so that report never arrives and the failure surfaces as the
 * child's exit status instead. That is a real difference, it is written
 * down in ROADMAP.md, and a test asserting the Linux behaviour here
 * would simply fail rather than tell anyone anything new.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>

extern char **environ;

/* The program the spawned child becomes. It prints one line and exits
 * 24. The host run of this test stages it at the same path the guest's
 * initrd puts it at, so both sides exec the same bytes. */
#define CHILD "/tmp/forkchild64"

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

/* One spawn, waited for. Returns the wait status, or -1. */
static int spawn_and_wait(const char *label, int *out_rc)
{
    char *const argv[] = { (char *)CHILD, NULL };
    pid_t pid = -1;
    int status = 0;
    int rc;

    rc = posix_spawn(&pid, CHILD, NULL, NULL, argv, environ);
    *out_rc = rc;
    if (rc != 0)
        return -1;

    ok(label, pid > 0);

    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return status;
}

int main(void)
{
    int status, rc = 0;

    /* Unbuffered: the child of a spawn inherits whatever is sitting in
     * this buffer, and on the guest stdout is a serial log rather than a
     * file, so a duplicated buffer is a duplicated line in the middle of
     * the transcript the differential is matching against. */
    setvbuf(stdout, NULL, _IONBF, 0);

    status = spawn_and_wait("posix_spawn produced a process", &rc);
    ok("posix_spawn reported success", rc == 0);
    ok("the spawned child was waited for", status >= 0);
    ok("it exited rather than being killed", WIFEXITED(status));

    /* The value forkchild64 exits with. Getting this back is the whole
     * chain working: the clone made a process, execve replaced it with a
     * different program, that program ran, and wait4 found it. */
    ok("and it exited 24, which is the program it became",
       WEXITSTATUS(status) == 24);

    /* A second one, because a spawn that works once and leaves the
     * process table or the parent's blocked state wrong works exactly
     * once. Wine spawns more than one. */
    status = spawn_and_wait("a second posix_spawn produced a process", &rc);
    ok("the second spawn reported success", rc == 0);
    ok("and the second child exited 24 too",
       WIFEXITED(status) && WEXITSTATUS(status) == 24);

    /* And there is nothing left to wait for. A vfork that left the
     * parent thinking it still had children would hang here rather than
     * failing, so this is bounded by the harness's timeout. */
    ok("no third child appears from nowhere",
       waitpid(-1, &status, WNOHANG) <= 0);

    /* The mode a directory was made with, read back.
     *
     * This belongs with the spawn rather than in a filesystem test
     * because of what needs it: the wineserver creates its socket
     * directory 0700 and then refuses to start - "must not be
     * accessible by other users" - if stat reports any of the low six
     * bits set. A filesystem that invented 0755 for every directory
     * always reported them set, so the server died immediately after
     * being spawned, which looked like a spawn problem and was not. */
    {
        struct stat st;
        const char *dir = "/tmp/spawn64-0700";

        rmdir(dir);
        ok("a directory can be made 0700", mkdir(dir, 0700) == 0);
        ok("and stat says 0700, not something invented",
           stat(dir, &st) == 0 && (st.st_mode & 07777) == 0700);
        ok("which is what the wineserver checks for",
           (st.st_mode & 077) == 0);
        rmdir(dir);
    }

    printf("spawn64: %d failures\n", failures);
    return failures ? 1 : 103;
}
