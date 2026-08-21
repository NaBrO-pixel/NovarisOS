/* sock64.c - a socket two processes that have never met can find.
 *
 * Milestone 75. Milestone 74 built the connection and stopped one call
 * short: the wineserver makes a *named* socket in its own directory and
 * every Wine process connects to it by that name.
 *
 * The difference between that and a socketpair is the whole subject. A
 * socketpair's two ends are handed to somebody who already holds both,
 * so there is no question of who is talking to whom. A listening socket
 * has to answer that question with a name in the filesystem, a queue of
 * callers waiting at it, and an accept that manufactures a fresh pair
 * per caller - and the assertions below are mostly about the queue and
 * the pairing, because moving the bytes afterwards is Milestone 74's
 * job and was tested there.
 *
 * The one that matters most is that two connections do not get each
 * other's traffic. A rendezvous that handed every caller the same pair
 * would pass a single-client test perfectly.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#define SOCKNAME "/tmp/sock64-test"

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

static void fill(struct sockaddr_un *a)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strcpy(a->sun_path, SOCKNAME);
}

int main(void)
{
    struct sockaddr_un addr;
    struct stat st;
    int srv, c1, c2, s1, s2;
    char buf[64];

    setvbuf(stdout, NULL, _IONBF, 0);
    unlink(SOCKNAME);

    /* --- a name in the filesystem ------------------------------------ */

    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    ok("a socket can be made", srv >= 0);

    /* Before bind there is no name, so there is nothing to connect to
     * and nothing to stat. */
    ok("and it has no name until it is bound", stat(SOCKNAME, &st) == -1);

    fill(&addr);
    ok("bind gives it one", bind(srv, (struct sockaddr *)&addr,
                                 sizeof(addr)) == 0);
    ok("which is now in the filesystem", stat(SOCKNAME, &st) == 0);

    /* Wine's client lstats this and refuses to connect unless the type
     * bits say socket - "'%s/%s' is not a socket" - so the type is not
     * decoration. */
    ok("and stat says it is a socket", S_ISSOCK(st.st_mode));

    /* Binding a second socket to a name that is taken has to fail, and
     * fail with the error Wine tests for: it treats EADDRINUSE as
     * "somebody else is already the server" rather than as a problem. */
    {
        int dup_srv = socket(AF_UNIX, SOCK_STREAM, 0);
        errno = 0;
        ok("a second bind to the same name is refused",
           bind(dup_srv, (struct sockaddr *)&addr, sizeof(addr)) == -1);
        ok("and the refusal is EADDRINUSE",
           errno == EADDRINUSE || errno == EEXIST);
        close(dup_srv);
    }

    /* --- nobody is listening yet -------------------------------------- */

    c1 = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    ok("connecting before listen is refused",
       connect(c1, (struct sockaddr *)&addr, sizeof(addr)) == -1);
    ok("and the refusal is ECONNREFUSED", errno == ECONNREFUSED);
    close(c1);

    ok("listen succeeds", listen(srv, 5) == 0);

    /* --- one client -------------------------------------------------- */

    c1 = socket(AF_UNIX, SOCK_STREAM, 0);
    ok("a client socket", c1 >= 0);
    ok("connects once somebody is listening",
       connect(c1, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    s1 = accept(srv, NULL, NULL);
    ok("and the server accepts it", s1 >= 0);
    ok("which is a different descriptor from the listening one", s1 != srv);

    ok("the client writes", write(c1, "req1", 4) == 4);
    memset(buf, 0, sizeof(buf));
    ok("and the server reads it", read(s1, buf, sizeof(buf)) == 4);
    ok("unchanged", memcmp(buf, "req1", 4) == 0);

    ok("the server replies", write(s1, "rep1", 4) == 4);
    memset(buf, 0, sizeof(buf));
    ok("and the client reads that", read(c1, buf, sizeof(buf)) == 4);
    ok("unchanged", memcmp(buf, "rep1", 4) == 0);

    /* --- two clients, which must not be the same connection ---------- */
    /* The assertion a rendezvous that reused one pair would fail. */

    c2 = socket(AF_UNIX, SOCK_STREAM, 0);
    ok("a second client connects",
       connect(c2, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    s2 = accept(srv, NULL, NULL);
    ok("and is accepted separately", s2 >= 0 && s2 != s1);

    ok("the second client writes", write(c2, "two", 3) == 3);
    memset(buf, 0, sizeof(buf));
    ok("and it arrives on the second connection",
       read(s2, buf, sizeof(buf)) == 3 && memcmp(buf, "two", 3) == 0);

    /* And not on the first. The first connection has to still be idle -
     * if the two shared a pair, "two" would be sitting in it. */
    {
        int fl = fcntl(s1, F_GETFL);
        fcntl(s1, F_SETFL, fl | O_NONBLOCK);
        errno = 0;
        ok("and not on the first one",
           read(s1, buf, sizeof(buf)) == -1 && errno == EAGAIN);
        fcntl(s1, F_SETFL, fl);
    }

    /* Closing one connection must not disturb the other. */
    close(c1);
    ok("closing one client is end of file on its connection",
       read(s1, buf, sizeof(buf)) == 0);
    close(s1);

    ok("and the other connection still works",
       write(c2, "still", 5) == 5);
    memset(buf, 0, sizeof(buf));
    ok("in both directions",
       read(s2, buf, sizeof(buf)) == 5 && memcmp(buf, "still", 5) == 0);
    close(c2);
    close(s2);

    /* --- the queue: connect before accept ---------------------------- */
    /* A rendezvous is not a handshake. A caller that arrives while the
     * server is busy waits in the backlog, and accept finds it there
     * afterwards - which is what lets a server that is mid-request not
     * lose the next client. */
    {
        int q1, q2, a1, a2;

        q1 = socket(AF_UNIX, SOCK_STREAM, 0);
        q2 = socket(AF_UNIX, SOCK_STREAM, 0);
        ok("two clients connect before either is accepted",
           connect(q1, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
           connect(q2, (struct sockaddr *)&addr, sizeof(addr)) == 0);

        write(q1, "first", 5);
        write(q2, "second", 6);

        a1 = accept(srv, NULL, NULL);
        a2 = accept(srv, NULL, NULL);
        ok("both are accepted", a1 >= 0 && a2 >= 0 && a1 != a2);

        /* First in, first accepted - and, more to the point, each
         * accepted descriptor is joined to the client that queued it
         * rather than to whichever arrived last. */
        memset(buf, 0, sizeof(buf));
        ok("the first accepted is joined to the first caller",
           read(a1, buf, sizeof(buf)) == 5 && memcmp(buf, "first", 5) == 0);
        memset(buf, 0, sizeof(buf));
        ok("and the second to the second",
           read(a2, buf, sizeof(buf)) == 6 && memcmp(buf, "second", 6) == 0);

        close(q1); close(q2); close(a1); close(a2);
    }

    /* --- across a fork, which is how a real server is used ----------- */
    {
        int status = 0;
        pid_t kid = fork();

        if (kid == 0) {
            int cs = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un ca;
            fill(&ca);
            if (connect(cs, (struct sockaddr *)&ca, sizeof(ca)) != 0)
                _exit(31);
            if (write(cs, "child", 5) != 5)
                _exit(32);
            if (read(cs, buf, sizeof(buf)) != 6)
                _exit(33);
            close(cs);
            _exit(13);
        }
        ok("fork returned", kid > 0);

        {
            int as = accept(srv, NULL, NULL);
            ok("the server accepts the forked client", as >= 0);
            memset(buf, 0, sizeof(buf));
            ok("and reads what it sent",
               read(as, buf, sizeof(buf)) == 5 &&
               memcmp(buf, "child", 5) == 0);
            ok("and can answer it", write(as, "parent", 6) == 6);
            close(as);
        }

        waitpid(kid, &status, 0);
        ok("the child exited 13",
           WIFEXITED(status) && WEXITSTATUS(status) == 13);
    }

    close(srv);
    unlink(SOCKNAME);
    ok("and the name goes away with it", stat(SOCKNAME, &st) == -1);

    printf("sock64: %d failures\n", failures);
    return failures ? 1 : 109;
}
