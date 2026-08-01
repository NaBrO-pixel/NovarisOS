/* sock_test.c - a Linux Unix-socket program, run on Novaris.
 *
 * Same rules as the rest: raw `int $0x80`, Linux syscall numbers,
 * `gcc -m32 -static -nostdlib -ffreestanding`, linked against nothing.
 * The same binary runs on the Linux build host and on Novaris and the
 * transcripts are compared.
 *
 * On i386 every socket operation goes through one syscall - socketcall,
 * 102 - with the operation number in the first argument and a pointer to
 * that operation's arguments in the second. A 1990s space saving that
 * every i386 libc still speaks, and the reason there is no `socket`
 * syscall to call directly.
 *
 * The last section is the one this exists for. SCM_RIGHTS - sending an
 * open file descriptor to the other end of a socket - is how wineserver
 * hands a client the descriptor behind a Windows HANDLE. A socket layer
 * without it would look complete and be useless for the thing it was
 * built for.
 *
 * See ROADMAP.md Milestone 28.
 */

static long sc1(long n, long a) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}
static long sc2(long n, long a, long b) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b)
                         : "memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return r;
}

#define SYS_exit          1
#define SYS_read          3
#define SYS_write         4
#define SYS_open          5
#define SYS_close         6
#define SYS_unlink       10
#define SYS_socketcall  102

/* socketcall sub-calls. */
#define SC_SOCKET      1
#define SC_BIND        2
#define SC_CONNECT     3
#define SC_LISTEN      4
#define SC_ACCEPT      5
#define SC_GETSOCKNAME 6
#define SC_SOCKETPAIR  8
#define SC_SEND        9
#define SC_RECV       10
#define SC_SHUTDOWN   13
#define SC_SENDMSG    16
#define SC_RECVMSG    17

#define AF_UNIX      1
#define SOCK_STREAM  1
#define SOL_SOCKET   1
#define SCM_RIGHTS   1
#define SHUT_WR      1

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

#define ENOENT_          -2
#define EPROTONOSUPPORT -93
#define EINVAL_         -22

struct sockaddr_un { unsigned short sun_family; char sun_path[108]; };
struct iovec  { void* iov_base; unsigned long iov_len; };
struct msghdr {
    void* msg_name; unsigned long msg_namelen;
    struct iovec* msg_iov; unsigned long msg_iovlen;
    void* msg_control; unsigned long msg_controllen;
    unsigned long msg_flags;
};
struct cmsghdr { unsigned long cmsg_len; int cmsg_level; int cmsg_type; };

static long sock(long call, long* args) {
    return sc2(SYS_socketcall, call, (long)args);
}

/* --- a few bytes of libc ------------------------------------------------- */

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }
static void check(const char* label, int ok) {
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(label);
    out("\n");
}
static int memeq(const char* a, const char* b, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static void set_path(struct sockaddr_un* a, const char* p) {
    a->sun_family = AF_UNIX;
    unsigned i = 0;
    while (p[i] && i < sizeof(a->sun_path) - 1) { a->sun_path[i] = p[i]; i++; }
    a->sun_path[i] = '\0';
}

#define SOCKPATH "/tmp/novaris_socktest"
#define FILEPATH "/tmp/novaris_sockfile"

int main_(void) {
    out("unix socket test - written for Linux, running unmodified\n\n");
    sc1(SYS_unlink, (long)SOCKPATH);
    sc1(SYS_unlink, (long)FILEPATH);

    /* 1. socketpair: two connected ends, no name, no listener. This is
     *    what Wine's ntdll creates first. */
    out("1. socketpair\n");
    int sv[2] = { -1, -1 };
    {
        long a[4] = { AF_UNIX, SOCK_STREAM, 0, (long)sv };
        check("socketpair(AF_UNIX, SOCK_STREAM) succeeds",
              sock(SC_SOCKETPAIR, a) == 0);
        check("it returned two descriptors", sv[0] >= 0 && sv[1] >= 0 &&
                                             sv[0] != sv[1]);
    }
    {
        const char* msg = "hello over a socket";
        long n = (long)slen(msg);
        long a[6] = { sv[0], (long)msg, n, 0, 0, 0 };
        check("send returns the byte count", sock(SC_SEND, a) == n);

        char buf[64];
        long b[6] = { sv[1], (long)buf, 64, 0, 0, 0 };
        long got = sock(SC_RECV, b);
        check("the other end receives the same count", got == n);
        check("and the same bytes", memeq(buf, msg, (unsigned)n));
    }
    {
        /* Both directions: a socket pair is not a pipe. */
        const char* back = "and back again";
        long n = (long)slen(back);
        long a[6] = { sv[1], (long)back, n, 0, 0, 0 };
        sock(SC_SEND, a);
        char buf[64];
        long b[6] = { sv[0], (long)buf, 64, 0, 0, 0 };
        check("it carries traffic the other way too",
              sock(SC_RECV, b) == n && memeq(buf, back, (unsigned)n));
    }
    {
        /* Closing one end is end-of-stream at the other, which is 0 and
         * not an error - the distinction every protocol loop is built
         * on. */
        long a[3] = { sv[0], SHUT_WR, 0 };
        sock(SC_SHUTDOWN, a);
        char buf[8];
        long b[6] = { sv[1], (long)buf, 8, 0, 0, 0 };
        check("after shutdown the peer reads end-of-stream, not an error",
              sock(SC_RECV, b) == 0);
    }
    sc1(SYS_close, sv[0]);
    sc1(SYS_close, sv[1]);

    /* 2. A named socket, which is how a client finds a server. */
    out("\n2. bind, listen, connect, accept\n");
    int srv = -1, cli = -1, acc = -1;
    {
        long a[3] = { AF_UNIX, SOCK_STREAM, 0 };
        srv = (int)sock(SC_SOCKET, a);
        check("socket(AF_UNIX, SOCK_STREAM) succeeds", srv >= 0);

        struct sockaddr_un addr;
        set_path(&addr, SOCKPATH);
        long b[3] = { srv, (long)&addr, sizeof(addr) };
        check("bind to a path succeeds", sock(SC_BIND, b) == 0);
        long c[2] = { srv, 4 };
        check("listen succeeds", sock(SC_LISTEN, c) == 0);
    }
    {
        long a[3] = { AF_UNIX, SOCK_STREAM, 0 };
        cli = (int)sock(SC_SOCKET, a);
        struct sockaddr_un addr;
        set_path(&addr, SOCKPATH);
        long b[3] = { cli, (long)&addr, sizeof(addr) };
        check("connect to it succeeds", sock(SC_CONNECT, b) == 0);

        /* Accepting after the connect, which is the point of a backlog:
         * the client does not have to wait for the server to be ready. */
        long c[3] = { srv, 0, 0 };
        acc = (int)sock(SC_ACCEPT, c);
        check("accept returns a new descriptor",
              acc >= 0 && acc != srv && acc != cli);
    }
    {
        const char* msg = "client to server";
        long n = (long)slen(msg);
        long a[6] = { cli, (long)msg, n, 0, 0, 0 };
        sock(SC_SEND, a);
        char buf[64];
        long b[6] = { acc, (long)buf, 64, 0, 0, 0 };
        check("the accepted socket carries the client's message",
              sock(SC_RECV, b) == n && memeq(buf, msg, (unsigned)n));
    }
    {
        /* Connecting to a name with nothing bound to it is ENOENT - the
         * name is a filesystem path, and there is no file. Wine tests
         * exactly this to decide whether it needs to start a wineserver
         * of its own, so the *number* matters and not just the failure. */
        long a[3] = { AF_UNIX, SOCK_STREAM, 0 };
        int s2 = (int)sock(SC_SOCKET, a);
        struct sockaddr_un addr;
        set_path(&addr, "/tmp/novaris_nothing_here");
        long b[3] = { s2, (long)&addr, sizeof(addr) };
        check("connecting to a name nobody bound is ENOENT",
              sock(SC_CONNECT, b) == ENOENT_);
        sc1(SYS_close, s2);
    }

    /* 3. SCM_RIGHTS. The reason this milestone exists. */
    out("\n3. passing a file descriptor over a socket\n");
    {
        const char* contents = "a file behind a descriptor\n";
        long clen = (long)slen(contents);
        long fd = sc3(SYS_open, (long)FILEPATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        sc3(SYS_write, fd, (long)contents, clen);
        sc1(SYS_close, fd);

        fd = sc3(SYS_open, (long)FILEPATH, O_RDONLY, 0);
        check("opened a file to send", fd >= 0);

        /* One byte of payload alongside it: Linux will not deliver
         * control data on a stream socket with no data at all. */
        char payload = 'x';
        struct iovec iov = { &payload, 1 };
        char cbuf[64];
        struct cmsghdr* cm = (struct cmsghdr*)cbuf;
        cm->cmsg_len = sizeof(struct cmsghdr) + sizeof(int);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        *(int*)(cbuf + sizeof(struct cmsghdr)) = (int)fd;

        struct msghdr mh;
        mh.msg_name = 0; mh.msg_namelen = 0;
        mh.msg_iov = &iov; mh.msg_iovlen = 1;
        mh.msg_control = cbuf; mh.msg_controllen = cm->cmsg_len;
        mh.msg_flags = 0;

        long a[3] = { cli, (long)&mh, 0 };
        check("sendmsg with SCM_RIGHTS succeeds", sock(SC_SENDMSG, a) == 1);

        char rpay = 0;
        struct iovec riov = { &rpay, 1 };
        char rcbuf[64];
        for (unsigned i = 0; i < sizeof(rcbuf); i++) rcbuf[i] = 0;
        struct msghdr rmh;
        rmh.msg_name = 0; rmh.msg_namelen = 0;
        rmh.msg_iov = &riov; rmh.msg_iovlen = 1;
        rmh.msg_control = rcbuf; rmh.msg_controllen = sizeof(rcbuf);
        rmh.msg_flags = 0;

        long b[3] = { acc, (long)&rmh, 0 };
        check("recvmsg returns the payload byte", sock(SC_RECVMSG, b) == 1);
        check("and it is the byte that was sent", rpay == 'x');

        struct cmsghdr* rcm = (struct cmsghdr*)rcbuf;
        check("with one SCM_RIGHTS control message",
              rmh.msg_controllen >= sizeof(struct cmsghdr) + sizeof(int) &&
              rcm->cmsg_level == SOL_SOCKET && rcm->cmsg_type == SCM_RIGHTS);

        int newfd = *(int*)(rcbuf + sizeof(struct cmsghdr));
        check("the descriptor arrived as a new number",
              newfd >= 0 && newfd != (int)fd);

        char rbuf[64];
        long got = sc3(SYS_read, newfd, (long)rbuf, 64);
        check("and reading it gives the file's contents",
              got == clen && memeq(rbuf, contents, (unsigned)clen));
        sc1(SYS_close, newfd);
        sc1(SYS_close, fd);
    }

    /* 4. The refusals, which matter as much as what works - and which
     *    are checked against the host rather than assumed, because the
     *    first draft of this section guessed two of them wrong. */
    out("\n4. what is refused\n");
    {
        long a[3] = { AF_UNIX, SOCK_STREAM, 99 };
        check("an unknown protocol is EPROTONOSUPPORT",
              sock(SC_SOCKET, a) == EPROTONOSUPPORT);
        long b[3] = { AF_UNIX, 99, 0 };
        check("an unknown socket type is EINVAL", sock(SC_SOCKET, b) == EINVAL_);
    }

    sc1(SYS_close, acc);
    sc1(SYS_close, cli);
    sc1(SYS_close, srv);
    sc1(SYS_unlink, (long)SOCKPATH);
    sc1(SYS_unlink, (long)FILEPATH);

    out("\nunix socket test done.\n");
    return 0;
}

void _start(void) {
    sc1(SYS_exit, main_());
    __builtin_unreachable();
}
