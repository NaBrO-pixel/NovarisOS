/* pipe64.c - the descriptor layer, as the wineserver uses it.
 *
 * Milestone 74. The wineserver's first act is a pipe, and Milestone 73
 * left it there: "wineserver: pipe: Function not implemented". The thing
 * after that is the one that matters, though - every Wine process talks
 * to the server over a socketpair, and a socketpair is the case a pipe
 * implementation is most likely to get subtly wrong, because it is two
 * pipes crossed and a plain pipe answered in its place passes every
 * obvious test while silently losing one direction.
 *
 * So the assertions below are mostly about direction and about ends:
 * which descriptor may write, which may read, what happens when the last
 * writer goes away, and what happens when the last reader does. Those
 * last two are how the two sides of a connection find out the other has
 * died; a kernel that gets them wrong does not fail, it hangs.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <stdlib.h>

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

int main(void)
{
    int  p[2], sv[2];
    char buf[64];
    int  flags;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* --- pipe(2): one direction, and it is enforced ------------------ */

    ok("a pipe can be made", pipe(p) == 0);
    ok("and its two ends are different descriptors", p[0] != p[1]);

    ok("what goes in the write end", write(p[1], "hello", 5) == 5);
    memset(buf, 0, sizeof(buf));
    ok("comes out of the read end", read(p[0], buf, sizeof(buf)) == 5);
    ok("unchanged", memcmp(buf, "hello", 5) == 0);

    /* The direction is not a convention, it is the descriptor. A pipe
     * implementation that stored one object per pipe and let either end
     * do either thing would pass everything above and fail here. */
    errno = 0;
    ok("the read end cannot be written to", write(p[0], "x", 1) == -1);
    errno = 0;
    ok("the write end cannot be read from", read(p[1], buf, 1) == -1);

    /* Two writes then one read: a pipe is a byte stream, not a queue of
     * messages, so the boundary between them is not preserved. */
    write(p[1], "ab", 2);
    write(p[1], "cd", 2);
    memset(buf, 0, sizeof(buf));
    ok("a pipe is a stream, not a record queue",
       read(p[0], buf, sizeof(buf)) == 4 && memcmp(buf, "abcd", 4) == 0);

    /* End of file is the last writer closing, not the pipe emptying.
     * This is how a client learns the server has gone. */
    write(p[1], "z", 1);
    close(p[1]);
    ok("what was already written survives the writer",
       read(p[0], buf, sizeof(buf)) == 1);
    ok("and then the reader sees end of file",
       read(p[0], buf, sizeof(buf)) == 0);
    close(p[0]);

    /* --- O_NONBLOCK, which is per descriptor ------------------------- */

    ok("another pipe", pipe(p) == 0);
    flags = fcntl(p[0], F_GETFL);
    ok("F_GETFL answers", flags != -1);
    ok("and O_NONBLOCK is not set to begin with", !(flags & O_NONBLOCK));
    ok("F_SETFL sets it", fcntl(p[0], F_SETFL, flags | O_NONBLOCK) == 0);
    ok("and F_GETFL agrees afterwards",
       (fcntl(p[0], F_GETFL) & O_NONBLOCK) != 0);

    errno = 0;
    ok("an empty non-blocking read is EAGAIN, not end of file",
       read(p[0], buf, sizeof(buf)) == -1 && errno == EAGAIN);

    /* --- FD_CLOEXEC ---------------------------------------------------- */

    ok("F_GETFD says the descriptor is not close-on-exec",
       fcntl(p[0], F_GETFD) == 0);
    ok("F_SETFD sets it", fcntl(p[0], F_SETFD, FD_CLOEXEC) == 0);
    ok("and F_GETFD agrees", fcntl(p[0], F_GETFD) == FD_CLOEXEC);

    /* --- dup, and what it shares ------------------------------------- */
    {
        int d = dup(p[1]);
        ok("a descriptor can be duplicated", d >= 0 && d != p[1]);
        /* dup never copies close-on-exec - the point of the flag is that
         * it belongs to the descriptor, not to the thing it refers to. */
        ok("and the copy is not close-on-exec", fcntl(d, F_GETFD) == 0);

        write(d, "dup", 3);
        memset(buf, 0, sizeof(buf));
        ok("the copy writes to the same pipe",
           read(p[0], buf, sizeof(buf)) == 3 && memcmp(buf, "dup", 3) == 0);

        /* And it holds a reference of its own: closing one of two
         * writers must not look like end of file to the reader. */
        close(p[1]);
        errno = 0;
        ok("one writer closing is not end of file while another is open",
           read(p[0], buf, sizeof(buf)) == -1 && errno == EAGAIN);
        close(d);
        ok("and closing the last one is",
           read(p[0], buf, sizeof(buf)) == 0);
        close(p[0]);
    }

    /* --- socketpair: two pipes, crossed ------------------------------ */

    /* Non-blocking, because two of the assertions below are about a read
     * finding nothing - and a blocking read that finds nothing does not
     * fail, it waits, which in a test is a hang rather than a result.
     * SOCK_NONBLOCK is O_NONBLOCK, in the same bit. */
    ok("a socketpair can be made",
       socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    ok("and its two ends are different descriptors", sv[0] != sv[1]);

    ok("sv[0] writes", write(sv[0], "ping", 4) == 4);
    memset(buf, 0, sizeof(buf));
    ok("and sv[1] reads it", read(sv[1], buf, sizeof(buf)) == 4);
    ok("unchanged", memcmp(buf, "ping", 4) == 0);

    /* The assertion a plain pipe would fail. Both directions have to
     * work, and they have to be *separate* - a reply must not come back
     * to the sender as if it were its own request. */
    ok("sv[1] writes back", write(sv[1], "pong", 4) == 4);
    memset(buf, 0, sizeof(buf));
    ok("and sv[0] reads that", read(sv[0], buf, sizeof(buf)) == 4);
    ok("unchanged", memcmp(buf, "pong", 4) == 0);

    write(sv[0], "self", 4);
    errno = 0;
    ok("what sv[0] wrote does not come back to sv[0]",
       read(sv[0], buf, sizeof(buf)) == -1 && errno == EAGAIN);
    /* Drained, so the closing assertions below start from empty - and
     * asserted while draining, because "it went somewhere" and "it went
     * to the peer" are different claims. */
    ok("it went to sv[1] instead", read(sv[1], buf, sizeof(buf)) == 4);

    /* Closing one endpoint ends both of its directions: the peer sees
     * end of file on what it was reading. This is how the wineserver
     * notices a client has exited. */
    close(sv[0]);
    ok("closing one endpoint is end of file for the other",
       read(sv[1], buf, sizeof(buf)) == 0);
    close(sv[1]);

    /* --- a pipe across a fork ---------------------------------------- */
    /* The case that made this worth a test of its own: a fork
     * duplicates every descriptor, and a pipe that did not count the
     * copy would report end of file as soon as either process closed. */
    {
        int status = 0;
        pid_t kid;

        ok("a pipe for the child", pipe(p) == 0);
        kid = fork();
        if (kid == 0) {
            close(p[0]);
            write(p[1], "from the child", 14);
            close(p[1]);
            _exit(11);
        }
        ok("fork returned", kid > 0);
        close(p[1]);                    /* the parent is not a writer */

        memset(buf, 0, sizeof(buf));
        ok("the child's bytes arrive",
           read(p[0], buf, sizeof(buf)) == 14);
        ok("unchanged", memcmp(buf, "from the child", 14) == 0);
        ok("and the child closing its end is end of file",
           read(p[0], buf, sizeof(buf)) == 0);
        close(p[0]);

        waitpid(kid, &status, 0);
        ok("the child exited 11", WIFEXITED(status) && WEXITSTATUS(status) == 11);
    }

    /* --- a write larger than the pipe ---------------------------------- *
     *
     * A blocking write to a pipe returns only when every byte has gone,
     * sleeping while the pipe is full. This kernel used to return what
     * fitted and then -EAGAIN, and Wine sends each request to the
     * wineserver with one writev that must go whole: winedevice.exe lost
     * a thread answering an adapter query with more than 64KB. */
    {
        enum { BIG = 200000, V = 70000 };
        static unsigned char out[BIG], back[BIG + V];
        int p[2], status = 0, good = 1;
        long got = 0, n;
        pid_t kid;

        for (int i = 0; i < BIG; i++) out[i] = (unsigned char)(i * 7 + 3);
        ok("a pipe for the big write", pipe(p) == 0);
        kid = fork();
        if (kid == 0) {
            struct iovec v[3] = { { out, 30000 }, { out + 30000, 30000 },
                                  { out + 60000, V - 60000 } };
            close(p[0]);
            if (write(p[1], out, BIG) != BIG) _exit(1);
            if (writev(p[1], v, 3) != V) _exit(2);
            _exit(0);
        }
        close(p[1]);
        while ((n = read(p[0], back + got, sizeof(back) - got)) > 0) got += n;
        close(p[0]);
        for (long i = 0; i < BIG && good; i++) good = back[i] == out[i];
        for (long i = 0; i < V && good; i++) good = back[BIG + i] == out[i];
        ok("a 200000-byte write and a 70000-byte writev go whole",
           waitpid(kid, &status, 0) == kid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0);
        ok("and every byte arrives, in order", got == BIG + V && good);
    }

    printf("pipe64: %d failures\n", failures);
    return failures ? 1 : 107;
}
