/* cowfork64.c - fork that shares, and a write that separates.
 *
 * Milestone 69. The point of this program is that it cannot tell whether
 * the kernel copied eagerly or lazily: both give exactly this output.
 * That is deliberate - correctness first, and the kernel measures the
 * cost separately by counting free frames across the fork, which is the
 * only place the difference is visible.
 *
 * What it can tell is whether the two processes are actually
 * independent, which is the thing copy-on-write is easy to get wrong:
 * share the page, forget to make the *parent* read-only too, and the
 * child's writes land in the parent's memory. Every assertion below
 * would still pass if the pages were merely shared read-write, except
 * the ones that read the buffer back after the other side has changed
 * it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* Big enough that an eager fork has to copy something substantial, and
 * still under the 16MB the old eager clone could manage - so that
 * switching back to it is a fair comparison rather than an out-of-space
 * failure. */
#define SIZE (8 * 1024 * 1024)

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

/* Every byte, so a partially-copied page is caught rather than only a
 * partially-copied buffer. */
static int all_bytes_are(const unsigned char *p, unsigned char v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

int main(void)
{
    unsigned char *buf = malloc(SIZE);
    pid_t child;
    int status = 0;

    /* Unbuffered, which matters more here than in the other tests. A
     * buffered stdout is flushed at exit, so a fork duplicates whatever
     * is still sitting in the buffer and both processes print it - and
     * on the guest, where the output is a serial log rather than a
     * file, a crash after the fork loses it entirely. Unbuffered means
     * every line is a write(2) at the moment it happens, which is also
     * what makes the log readable when something goes wrong. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!buf) {
        printf("FAIL could not allocate\n");
        return 1;
    }

    memset(buf, 0xA5, SIZE);
    ok("the parent filled its buffer", all_bytes_are(buf, 0xA5, SIZE));

    child = fork();

    /* The child branches before anything is printed. Both processes run
     * every line after fork() returns, so an ok() here would be printed
     * twice - once by each - and the differential would compare a
     * transcript against itself. */
    if (child == 0) {
        /* The child sees what the parent had at the moment of the fork.
         * If the pages had not been shared correctly this would be
         * zeroes or rubbish. */
        if (!all_bytes_are(buf, 0xA5, SIZE))
            _exit(11);

        /* Now write to every page. Each one of these is a fault the
         * kernel turns into a private copy. */
        memset(buf, 0x5A, SIZE);
        if (!all_bytes_are(buf, 0x5A, SIZE))
            _exit(12);

        /* A second pass over pages the child already owns - these must
         * not fault again, and must certainly not be copied again. */
        memset(buf, 0x3C, SIZE);
        if (!all_bytes_are(buf, 0x3C, SIZE))
            _exit(13);

        _exit(21);
    }

    ok("fork returned", child > 0);

    if (waitpid(child, &status, 0) != child)
        ok("waited for the child", 0);

    ok("the child saw the parent's memory and rewrote it",
       WIFEXITED(status) && WEXITSTATUS(status) == 21);

    /* The one that matters. The child overwrote all 8MB twice; if the
     * parent's mapping had stayed writable and shared, this buffer is
     * now 0x3C and the fork was never a fork. */
    ok("and the parent's own copy is untouched",
       all_bytes_are(buf, 0xA5, SIZE));

    /* The parent writes too, after the child has exited. By then it is
     * the only owner left, so the kernel should reclaim the pages
     * rather than copy them - invisible from here, and asserted by the
     * kernel. */
    memset(buf, 0x77, SIZE);
    ok("the parent can still write to its own pages",
       all_bytes_are(buf, 0x77, SIZE));

    free(buf);

    printf("cowfork64: %d failures\n", failures);
    return failures ? 1 : 97;
}
