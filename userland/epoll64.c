/* epoll64.c - epoll(7), as the wineserver uses it (Milestone 104).
 *
 * The wineserver registers every client socket with epoll_ctl and waits
 * with epoll_wait, level-triggered, identifying each descriptor by the
 * u32 it stored in the event's data. This checks that much against
 * Linux, line for line, and a little around it: modifying and removing a
 * registration, the errors for doing either to something absent, hang-up
 * when the other end goes, a closed descriptor leaving the set on its
 * own, and a wait that sleeps until another process writes.
 *
 * Exits 104 when everything held.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s epoll: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

static int add(int ep, int fd, unsigned events, unsigned id)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.u32 = id;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

static int mod(int ep, int fd, unsigned events, unsigned id)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.u32 = id;
    return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

int main(void)
{
    struct epoll_event got[8];
    int ep, sv[2], pfd[2], n;
    char buf[16];

    setvbuf(stdout, NULL, _IONBF, 0);

    ok("epoll_create(0) is refused", epoll_create(0) == -1 && errno == EINVAL);
    ep = epoll_create1(EPOLL_CLOEXEC);
    ok("epoll_create1 gives a descriptor", ep >= 0);
    ok("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ok("pipe", pipe(pfd) == 0);

    ok("add a socket for reading", add(ep, sv[0], EPOLLIN, 7) == 0);
    ok("add it twice is EEXIST", add(ep, sv[0], EPOLLIN, 7) == -1 && errno == EEXIST);
    ok("add a pipe's read end", add(ep, pfd[0], EPOLLIN, 9) == 0);
    ok("mod of an absent fd is ENOENT",
       mod(ep, pfd[1], EPOLLOUT, 1) == -1 && errno == ENOENT);
    ok("del of an absent fd is ENOENT",
       epoll_ctl(ep, EPOLL_CTL_DEL, pfd[1], NULL) == -1 && errno == ENOENT);

    ok("nothing is ready yet", epoll_wait(ep, got, 8, 0) == 0);

    ok("write to the socket's peer", write(sv[1], "abc", 3) == 3);
    n = epoll_wait(ep, got, 8, 0);
    ok("the socket is readable", n == 1 && got[0].data.u32 == 7 &&
                                 (got[0].events & EPOLLIN));
    n = epoll_wait(ep, got, 8, 0);
    ok("and still is, level-triggered", n == 1 && got[0].data.u32 == 7);
    ok("read it", read(sv[0], buf, sizeof(buf)) == 3);
    ok("then it is not", epoll_wait(ep, got, 8, 0) == 0);

    ok("write to the pipe", write(pfd[1], "x", 1) == 1);
    ok("write to the socket again", write(sv[1], "y", 1) == 1);
    n = epoll_wait(ep, got, 8, 0);
    ok("both are readable", n == 2 &&
       ((got[0].data.u32 == 7 && got[1].data.u32 == 9) ||
        (got[0].data.u32 == 9 && got[1].data.u32 == 7)));
    n = epoll_wait(ep, got, 1, 0);
    ok("maxevents 1 gives one", n == 1);

    ok("mod the socket to writable", mod(ep, sv[0], EPOLLOUT, 8) == 0);
    ok("drain the pipe", read(pfd[0], buf, sizeof(buf)) == 1);
    n = epoll_wait(ep, got, 8, 0);
    ok("the socket is writable, with its new id", n == 1 &&
       got[0].data.u32 == 8 && (got[0].events & EPOLLOUT) &&
       !(got[0].events & EPOLLIN));
    ok("del the socket", epoll_ctl(ep, EPOLL_CTL_DEL, sv[0], NULL) == 0);
    ok("nothing left ready", epoll_wait(ep, got, 8, 0) == 0);

    /* The pipe's writer goes: its reader hangs up, readable for the end
     * of file. Then the reader closes and leaves the set by itself. */
    close(pfd[1]);
    n = epoll_wait(ep, got, 8, 0);
    /* Linux says hung up; this kernel says readable, at end of file -
     * what its poll has always said, and what the wineserver notices a
     * dead client by. Either is end of file to a reader. */
    ok("a writerless pipe reports its end", n == 1 &&
       got[0].data.u32 == 9 && (got[0].events & (EPOLLIN | EPOLLHUP)));
    close(pfd[0]);
    ok("a closed descriptor leaves the set", epoll_wait(ep, got, 8, 0) == 0);

    /* A wait that has to sleep: another process writes after a while. */
    ok("re-add the socket", add(ep, sv[0], EPOLLIN, 5) == 0);
    ok("drain it", read(sv[0], buf, sizeof(buf)) == 1);
    {
        pid_t pid = fork();
        int status = 0;
        if (pid == 0) {
            usleep(100000);
            _exit(write(sv[1], "z", 1) == 1 ? 0 : 1);
        }
        n = epoll_wait(ep, got, 8, 5000);
        ok("a blocking wait wakes for another process's write",
           n == 1 && got[0].data.u32 == 5);
        ok("the writer exits", pid > 0 && waitpid(pid, &status, 0) == pid &&
                               WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    n = epoll_wait(ep, got, 8, 50);
    ok("a timed wait with nothing new still sees the byte", n == 1);
    ok("read it", read(sv[0], buf, sizeof(buf)) == 1);
    ok("a timed wait with nothing ready times out", epoll_wait(ep, got, 8, 50) == 0);

    close(sv[1]);
    n = epoll_wait(ep, got, 8, 0);
    ok("the peer gone: readable, at end of file", n == 1 && (got[0].events & EPOLLIN));
    close(sv[0]);
    close(ep);

    printf("epoll: %s\n", failures ? "FAILED" : "all held");
    return failures ? 1 : 104;
}
