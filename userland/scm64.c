/* scm64.c - passing a descriptor to another process.
 *
 * Milestone 76. This is Wine's handle mechanism. Every Windows object a
 * Wine process holds is, underneath, a file descriptor the wineserver
 * sent it over their socket with SCM_RIGHTS - so this is not one more
 * socket option, it is the thing the whole Windows side stands on.
 *
 * What makes it different from everything else in this tree is that it
 * is the first operation touching *two* processes' descriptor tables.
 * The sender names one of its own numbers; the receiver gets the thing
 * that number referred to, at a number the receiver chooses. The two
 * numbers have nothing to do with each other, and a test that only
 * checked "a descriptor arrived" would pass on an implementation that
 * sent the integer.
 *
 * So the assertions are about identity: the descriptor that arrives has
 * to be the same open file as the one that was sent - same contents,
 * same position, and writes through one visible through the other -
 * while being a different number, in a different process, that goes on
 * working after the sender has closed its own.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

/* Send `fd` down `sock` with one byte of payload, the way Wine does:
 * the descriptor never travels alone. */
static int send_fd(int sock, int fd, char tag)
{
    struct msghdr msg;
    struct iovec  vec;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *c;

    memset(&msg, 0, sizeof(msg));
    memset(cbuf, 0, sizeof(cbuf));

    vec.iov_base = &tag;
    vec.iov_len  = 1;
    msg.msg_iov    = &vec;
    msg.msg_iovlen = 1;
    msg.msg_control    = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type  = SCM_RIGHTS;
    c->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    msg.msg_controllen = c->cmsg_len;

    return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}

/* Receive one, returning the new descriptor or -1. `*tag` gets the
 * payload byte. */
static int recv_fd(int sock, char *tag)
{
    struct msghdr msg;
    struct iovec  vec;
    char cbuf[CMSG_SPACE(sizeof(int)) + 32];
    struct cmsghdr *c;
    int fd = -1;

    memset(&msg, 0, sizeof(msg));
    vec.iov_base = tag;
    vec.iov_len  = 1;
    msg.msg_iov    = &vec;
    msg.msg_iovlen = 1;
    msg.msg_control    = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    if (recvmsg(sock, &msg, 0) != 1)
        return -1;

    for (c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
    }
    return fd;
}

int main(void)
{
    int sv[2];
    char buf[64], tag = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* --- a plain message with no descriptor -------------------------- */
    /* First, because sendmsg has to carry ordinary bytes correctly
     * before anything else it does matters. */
    ok("a socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    {
        struct msghdr m;
        struct iovec v;
        memset(&m, 0, sizeof(m));
        v.iov_base = (void *)"plain";
        v.iov_len  = 5;
        m.msg_iov = &v;
        m.msg_iovlen = 1;
        ok("sendmsg with no control data sends bytes",
           sendmsg(sv[0], &m, 0) == 5);
        memset(buf, 0, sizeof(buf));
        ok("and they arrive",
           read(sv[1], buf, sizeof(buf)) == 5 && memcmp(buf, "plain", 5) == 0);
    }

    /* --- a file descriptor, to another process ----------------------- */
    {
        int status = 0;
        pid_t kid;
        int f;

        /* A file with known contents, opened and then read halfway, so
         * that the position travels with it and can be checked. */
        f = open("/tmp/scm64-file", O_RDWR | O_CREAT | O_TRUNC, 0644);
        ok("a file to send", f >= 0);
        ok("with something in it", write(f, "abcdefgh", 8) == 8);
        ok("read halfway", lseek(f, 4, SEEK_SET) == 4);

        kid = fork();
        if (kid == 0) {
            char t = 0;
            int got = recv_fd(sv[1], &t);
            char c[8];

            if (got < 0) _exit(41);
            if (t != 'F') _exit(42);
            /* A different number in a different process. */
            if (got == f) _exit(43);
            /* The same open file: reading continues from where the
             * sender had got to, which is the part that proves this is
             * the descriptor rather than a fresh open of the name. */
            if (read(got, c, 4) != 4) _exit(44);
            if (memcmp(c, "efgh", 4) != 0) _exit(45);
            /* And writes through it are visible to the sender. */
            if (lseek(got, 0, SEEK_SET) != 0) _exit(46);
            if (write(got, "ZZZZ", 4) != 4) _exit(47);
            close(got);
            _exit(17);
        }
        ok("fork returned", kid > 0);

        /* Let the child reach its recvmsg and block there before
         * anything is sent.
         *
         * This is not politeness, it is the assertion. A recvmsg that
         * finds its data already waiting never exercises the path where
         * the call blocks and is restarted - and that path is where this
         * went wrong: the restart put read(2)'s number back in rax, so
         * the call came back as `read(fd, &msghdr, flags)`, the same
         * arguments under a different name, and the payload landed in
         * the message header instead of the buffer it pointed at. The
         * test passed for as long as the send happened to win the race. */
        usleep(50000);

        ok("the descriptor is sent", send_fd(sv[0], f, 'F') == 0);
        /* Closed immediately, the way Wine does - the sender is done
         * with it the moment sendmsg returns. If the reference had been
         * taken on arrival rather than on send, this close would be the
         * one that destroyed it. */
        close(f);

        waitpid(kid, &status, 0);
        ok("the child got a working descriptor",
           WIFEXITED(status) && WEXITSTATUS(status) == 17);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 17)
            printf("     (child exited %d)\n", WEXITSTATUS(status));

        /* What the child wrote through its copy, read back through a
         * fresh open. The sender closed its own descriptor before the
         * child ever used it. */
        f = open("/tmp/scm64-file", O_RDONLY);
        memset(buf, 0, sizeof(buf));
        ok("and its writes went to the real file",
           read(f, buf, sizeof(buf)) == 8 && memcmp(buf, "ZZZZefgh", 8) == 0);
        close(f);
        unlink("/tmp/scm64-file");
    }

    /* --- a pipe end, which is what Wine actually passes -------------- */
    /* The wineserver hands a process one end of a pipe it made. The
     * assertion is that the end still works as an end: what the parent
     * writes to its half comes out of the half the child received. */
    {
        int status = 0, p[2];
        pid_t kid;

        ok("a pipe to send an end of", pipe(p) == 0);

        kid = fork();
        if (kid == 0) {
            char t = 0, c[16];
            int got;
            /* The fork copied both ends before either was sent, so this
             * process is holding a writer of its own. Left open, the
             * end-of-file assertion below would wait for this process to
             * close a descriptor it is not going to touch. */
            close(p[0]);
            close(p[1]);
            got = recv_fd(sv[1], &t);
            if (got < 0) _exit(51);
            if (t != 'P') _exit(52);
            if (read(got, c, 6) != 6) _exit(53);
            if (memcmp(c, "stream", 6) != 0) _exit(54);
            /* And end of file still arrives through it. */
            if (read(got, c, sizeof(c)) != 0) _exit(55);
            close(got);
            _exit(19);
        }
        ok("fork returned", kid > 0);

        usleep(50000);                  /* block the child first, as above */
        ok("the read end is sent", send_fd(sv[0], p[0], 'P') == 0);
        close(p[0]);                    /* the parent is not a reader */
        ok("the parent writes to its own end", write(p[1], "stream", 6) == 6);
        close(p[1]);                    /* which the child sees as EOF */

        waitpid(kid, &status, 0);
        ok("the child read through the end it was given",
           WIFEXITED(status) && WEXITSTATUS(status) == 19);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 19)
            printf("     (child exited %d)\n", WEXITSTATUS(status));
    }

    close(sv[0]);
    close(sv[1]);

    printf("scm64: %d failures\n", failures);
    return failures ? 1 : 113;
}
