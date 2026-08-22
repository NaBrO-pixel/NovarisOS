/* shm64.c - a file with no name, shared between two processes.
 *
 * Milestone 78 owed this test and did not have it. The five fixes it
 * made were covered only by the existing suite continuing to pass and by
 * Wine getting further, which is evidence and not an assertion.
 *
 * The two mechanisms worth asserting are the two that were wrong:
 *
 *   - A file unlinked while open goes on existing until the last
 *     descriptor closes. ramfs64_unlink used to free it immediately and
 *     its own comment predicted the consequence.
 *
 *   - A MAP_SHARED mapping is shared. A private copy satisfies every
 *     read and write a single process can make, so the only thing that
 *     can tell the difference is a second process - which is why the
 *     interesting half of this program runs after a fork.
 *
 * Together they are one idiom rather than two features: create, size,
 * unlink, map is how a program gets anonymous shared memory out of a
 * filesystem, and it is what Wine does for every shared mapping it
 * makes. Neither half is any use without the other.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define NAME "/tmp/shm64-file"
#define SPAN 4096

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

int main(void)
{
    struct stat st;
    char buf[64];
    int fd;

    setvbuf(stdout, NULL, _IONBF, 0);
    unlink(NAME);

    /* --- a file that outlives its name ------------------------------- */

    fd = open(NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    ok("a file to share", fd >= 0);
    ok("with something in it", write(fd, "before", 6) == 6);

    ok("unlink removes the name", unlink(NAME) == 0);
    ok("and the name is gone", stat(NAME, &st) == -1 && errno == ENOENT);

    /* The descriptor is now the only thing that refers to it. Everything
     * below would read freed memory on a filesystem that let the node go
     * when the name did. */
    ok("but the descriptor still finds it", fstat(fd, &st) == 0);
    ok("at the size it had", st.st_size == 6);

    memset(buf, 0, sizeof(buf));
    ok("and still reads what was written",
       pread(fd, buf, sizeof(buf), 0) == 6 && memcmp(buf, "before", 6) == 0);

    ok("it can still be written", pwrite(fd, "after ", 6, 0) == 6);
    memset(buf, 0, sizeof(buf));
    ok("and read back changed",
       pread(fd, buf, sizeof(buf), 0) == 6 && memcmp(buf, "after ", 6) == 0);

    /* Sized after being unlinked, which is the order the idiom uses. */
    ok("and resized", ftruncate(fd, SPAN) == 0);
    ok("to the size asked for", fstat(fd, &st) == 0 && st.st_size == SPAN);

    /* --- and is shared, which only a second process can show --------- */
    {
        volatile unsigned char *p;
        int status = 0;
        pid_t kid;

        p = mmap(NULL, SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ok("it maps shared", p != MAP_FAILED);

        /* What write(2) put there is what the mapping shows: one file,
         * one copy of its bytes, whichever way it is reached. */
        ok("the mapping shows what write(2) wrote",
           memcmp((void *)p, "after ", 6) == 0);

        /* And the other way round. */
        memcpy((void *)p + 16, "through-the-map", 15);
        memset(buf, 0, sizeof(buf));
        ok("and read(2) shows what the mapping wrote",
           pread(fd, buf, 15, 16) == 15 &&
           memcmp(buf, "through-the-map", 15) == 0);

        p[100] = 0xA5;

        kid = fork();
        if (kid == 0) {
            /* The child's own mapping of the same descriptor. A private
             * copy would give it the parent's bytes at this point and
             * diverge from here on - which is exactly what makes the
             * assertions below the ones that matter. */
            volatile unsigned char *q =
                mmap(NULL, SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (q == MAP_FAILED) _exit(61);

            if (q[100] != 0xA5) _exit(62);      /* the parent's store */
            q[200] = 0x5A;                      /* one of its own     */

            /* Through its inherited descriptor rather than its mapping,
             * so the child proves both routes reach the same bytes. */
            if (pwrite(fd, "child", 5, 300) != 5) _exit(63);
            _exit(23);
        }
        ok("fork returned", kid > 0);
        waitpid(kid, &status, 0);
        ok("the child saw the parent's store through its own mapping",
           WIFEXITED(status) && WEXITSTATUS(status) == 23);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 23)
            printf("     (child exited %d)\n", WEXITSTATUS(status));

        /* The assertion a private copy fails. The child is gone; what it
         * wrote has to still be here, in this process's mapping. */
        ok("and the parent sees what the child wrote", p[200] == 0x5A);
        ok("including what it wrote through the descriptor",
           memcmp((void *)p + 300, "child", 5) == 0);

        ok("it unmaps", munmap((void *)p, SPAN) == 0);
    }

    /* --- a fixed shared mapping goes exactly where it was asked ------ */
    /* Wine maps its shared user data at an address compiled into every
     * Windows program, inside a range it reserved earlier. A mapping
     * that succeeded somewhere else would be worse than one that failed:
     * the call reports an address nobody is going to look at. */
    {
        void *reserved, *fixed;

        reserved = mmap(NULL, SPAN * 4, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ok("a range to reserve", reserved != MAP_FAILED);

        /* Over the middle of it, the way Wine does. */
        fixed = mmap((char *)reserved + SPAN, SPAN, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_FIXED, fd, 0);
        ok("a fixed shared mapping succeeds", fixed != MAP_FAILED);
        ok("at exactly the address asked for",
           fixed == (void *)((char *)reserved + SPAN));
        ok("and it is the same file",
           fixed != MAP_FAILED && ((unsigned char *)fixed)[200] == 0x5A);

        munmap(reserved, SPAN * 4);
    }

    close(fd);

    /* The name never came back. */
    ok("the name is still gone", stat(NAME, &st) == -1);

    printf("shm64: %d failures\n", failures);
    return failures ? 1 : 119;
}
