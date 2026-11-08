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

/* The far side of the long-command-line check below: run as
 * "spawn64 --count a1 ... a40" with V0..V39 added to the environment, it
 * says whether all of it arrived. */
static int count_mode(int argc, char **argv)
{
    int env = 0, last_env = 0;
    for (char **e = environ; *e; e++) {
        env++;
        if (!strcmp(*e, "V39=39")) last_env = 1;
    }
    return (argc == 42 && !strcmp(argv[41], "a40") && env >= 40 && last_env) ? 0 : 1;
}

/* A program exec'd with more than sixteen arguments or environment
 * variables (Milestone 101). The kernel kept sixteen of each, silently;
 * Chromium lost its last two arguments to it, and every process the end
 * of an ordinary environment. Run on Linux too, so both must agree. */
static void long_command_line(const char *self)
{
    char *args[43], *envs[200], bufs[40][8], ebufs[40][12];
    int n = 0, status = -1;
    pid_t pid;

    args[0] = (char *)self;
    args[1] = "--count";
    for (int i = 0; i < 40; i++) {
        snprintf(bufs[i], sizeof(bufs[i]), "a%d", i + 1);
        args[2 + i] = bufs[i];
    }
    args[42] = NULL;
    for (char **e = environ; *e && n < 150; e++) envs[n++] = *e;
    for (int i = 0; i < 40; i++) {
        snprintf(ebufs[i], sizeof(ebufs[i]), "V%d=%d", i, i);
        envs[n++] = ebufs[i];
    }
    envs[n] = NULL;

    pid = fork();
    if (pid == 0) {
        execve(self, args, envs);
        _exit(2);
    }
    waitpid(pid, &status, 0);
    ok("execve with 41 arguments and 40 more variables delivers all of them",
       WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--count")) return count_mode(argc, argv);
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

    {
        /* NovarisOS runs this test from a copy in the kernel, which has no
         * file behind it, so the initrd carries one at /tmp/spawn64; on
         * Linux it is the program itself. */
        char self[512];
        ssize_t l;
        if (access("/tmp/spawn64", X_OK) == 0) {
            long_command_line("/tmp/spawn64");
        } else if ((l = readlink("/proc/self/exe", self, sizeof(self) - 1)) > 0) {
            self[l] = 0;
            long_command_line(self);
        } else {
            ok("/proc/self/exe names this program", 0);
        }
    }

    printf("spawn64: %d failures\n", failures);
    return failures ? 1 : 103;
}
