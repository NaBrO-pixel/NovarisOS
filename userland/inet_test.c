/* inet_test.c - a Linux TCP client, run on Novaris.
 *
 * Same rules as the rest of userland/: raw `int $0x80`, Linux syscall
 * numbers, `gcc -m32 -static -nostdlib -ffreestanding`, linked against
 * nothing that has heard of Novaris.
 *
 * This one cannot be compared against the host the way posix_test.c is,
 * because it needs a server to talk to and the two machines do not have
 * the same one. What it proves instead is the thing Milestone 41 is
 * about: that the network stack is reachable from a *process*. Until
 * now it was reachable only from the shell's own `fetch` and `update`,
 * which run in the kernel - so from inside a program, and therefore
 * from inside anything running under Wine, there was no network at all.
 *
 * It speaks HTTP/1.0 by hand, because a request is one line and the
 * point is the socket underneath it rather than the protocol on top.
 *
 * Usage: inet_test.elf <dotted-quad> <port>
 *
 * See ROADMAP.md Milestone 41.
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
#define SYS_write         4
#define SYS_close         6
#define SYS_socketcall  102

#define SC_SOCKET       1
#define SC_CONNECT      3
#define SC_GETPEERNAME  7
#define SC_SEND         9
#define SC_RECV        10

#define AF_INET       2
#define SOCK_STREAM   1

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;    /* network order */
    unsigned int   sin_addr;    /* network order */
    unsigned char  sin_zero[8];
};

/* --- the small amount of libc this needs -------------------------------- */

static unsigned slen(const char* s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }

static void out_uint(unsigned v) {
    char buf[12];
    int i = 11;
    buf[i--] = '\0';
    if (!v) buf[i--] = '0';
    while (v) { buf[i--] = (char)('0' + v % 10); v /= 10; }
    out(&buf[i + 1]);
}

static void out_int(long v) {
    if (v < 0) { out("-"); out_uint((unsigned)(-v)); }
    else out_uint((unsigned)v);
}

/* Network byte order is big-endian, and this machine is not, so both of
 * these are byte swaps. Written out rather than assumed: a port that is
 * only right because the host happens to be little-endian is a bug that
 * travels. */
static unsigned short hton16(unsigned short v) {
    return (unsigned short)((v << 8) | (v >> 8));
}
static unsigned hton32(unsigned v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v & 0xFF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

/* "10.0.2.2" -> 0x0A000202 in host order. Returns 0 on anything that is
 * not four numbers with three dots between them. */
static unsigned parse_ip(const char* s) {
    unsigned v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned byte = 0;
        while (*s >= '0' && *s <= '9') byte = byte * 10 + (unsigned)(*s++ - '0');
        if (byte > 255) return 0;
        v = (v << 8) | byte;
        if (part < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    return *s ? 0 : v;
}

static unsigned parse_uint(const char* s) {
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

/* --- the test ------------------------------------------------------------ */

static char buf[4096];

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "10.0.2.2";
    unsigned port = argc > 2 ? parse_uint(argv[2]) : 80;

    unsigned ip = parse_ip(host);
    if (!ip || !port) {
        out("usage: inet_test.elf <dotted-quad> <port>\n");
        return 2;
    }

    out("connecting to ");
    out(host);
    out(":");
    out_uint(port);
    out("\n");

    long a[6];

    a[0] = AF_INET; a[1] = SOCK_STREAM; a[2] = 0;
    long fd = sc2(SYS_socketcall, SC_SOCKET, (long)a);
    if (fd < 0) {
        out("[--] socket failed: ");
        out_int(fd);
        out("\n");
        return 1;
    }
    out("[ok] socket\n");

    struct sockaddr_in sin;
    for (unsigned i = 0; i < sizeof(sin.sin_zero); i++) sin.sin_zero[i] = 0;
    sin.sin_family = AF_INET;
    sin.sin_port = hton16((unsigned short)port);
    sin.sin_addr = hton32(ip);

    a[0] = fd; a[1] = (long)&sin; a[2] = sizeof(sin);
    long r = sc2(SYS_socketcall, SC_CONNECT, (long)a);
    if (r < 0) {
        out("[--] connect failed: ");
        out_int(r);
        out("\n");
        sc1(SYS_close, fd);
        return 1;
    }
    out("[ok] connect\n");

    /* getpeername, because a socket that connected should be able to say
     * to what - and because it is the one call that proves the address
     * survived the trip through the kernel in the right byte order. */
    struct sockaddr_in peer;
    unsigned peerlen = sizeof(peer);
    a[0] = fd; a[1] = (long)&peer; a[2] = (long)&peerlen;
    if (sc2(SYS_socketcall, SC_GETPEERNAME, (long)a) == 0) {
        out("[ok] peer port ");
        out_uint(hton16(peer.sin_port));
        out("\n");
    }

    /* HTTP/1.0 rather than 1.1: the server closes when the body ends and
     * that close is the framing, so there is no chunked encoding to
     * decode and no keep-alive to shut down. */
    static const char req[] =
        "GET / HTTP/1.0\r\nHost: novaris\r\nConnection: close\r\n\r\n";

    a[0] = fd; a[1] = (long)req; a[2] = sizeof(req) - 1; a[3] = 0;
    r = sc2(SYS_socketcall, SC_SEND, (long)a);
    if (r != (long)(sizeof(req) - 1)) {
        out("[--] send returned ");
        out_int(r);
        out("\n");
        sc1(SYS_close, fd);
        return 1;
    }
    out("[ok] sent ");
    out_int(r);
    out(" bytes\n");

    /* Read to the end of the stream. A stream read is allowed to return
     * short and this loops for exactly that reason; a test that assumed
     * one recv would bring the whole reply would pass against a fast
     * server and fail against a real one. */
    unsigned total = 0;
    int saw_status = 0;
    for (;;) {
        a[0] = fd; a[1] = (long)buf; a[2] = sizeof(buf) - 1; a[3] = 0;
        r = sc2(SYS_socketcall, SC_RECV, (long)a);
        if (r <= 0) break;

        if (!saw_status) {
            /* Print the status line only, so the transcript is an
             * assertion rather than a web page. */
            buf[r] = '\0';
            unsigned i = 0;
            while (i < (unsigned)r && buf[i] != '\r' && buf[i] != '\n') i++;
            buf[i] = '\0';
            out("[ok] ");
            out(buf);
            out("\n");
            saw_status = 1;
        }
        total += (unsigned)r;
    }

    if (r < 0) {
        out("[--] recv failed: ");
        out_int(r);
        out("\n");
        sc1(SYS_close, fd);
        return 1;
    }

    out("[ok] read ");
    out_uint(total);
    out(" bytes to end of stream\n");

    sc1(SYS_close, fd);
    out("[ok] close\n");
    out("inet_test: a process opened a TCP connection\n");
    return 0;
}

/* No libc, so no crt0: this is the ELF entry point. The stack at entry
 * holds argc, then argv[], exactly as Linux leaves it. */
__attribute__((naked, used)) void _start(void) {
    __asm__ __volatile__(
        "movl (%esp), %eax\n"      /* argc */
        "leal 4(%esp), %edx\n"     /* argv */
        "pushl %edx\n"
        "pushl %eax\n"
        "call main\n"
        "addl $8, %esp\n"
        "movl %eax, %ebx\n"
        "movl $1, %eax\n"          /* SYS_exit */
        "int $0x80\n");
}
