/* inet64.c - TCP/IP from a process's side (the inet layer).
 *
 * Static glibc, like the other socket tests, and an ordinary Linux
 * program: the loopback half runs on the build host too, and make test
 * requires every line the host prints to appear in the guest's
 * transcript.
 *
 * With "--external" (the kernel passes it when it has brought a card
 * up) it also talks to the build machine through QEMU's user-mode
 * network, where the host is 10.0.2.2: an HTTP fetch and a DNS question
 * to servers tools/net64_helper.py runs there, and an accept of a
 * connection the helper makes to us through a host forward. Those lines
 * are guest-only, and say so.
 *
 * Exit status 0 when everything passed; the layer checks for it. */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HOST_HTTP_PORT 18780
#define HOST_DNS_PORT  18781
#define GUEST_LISTEN   8080

static int failures;

static void ok(int cond, const char* what) {
    printf("%s %s\n", cond ? "[ok]" : "[FAIL]", what);
    if (!cond) failures++;
    fflush(stdout);
}

static int read_all(int fd, char* buf, int max) {
    int got = 0;
    while (got < max) {
        ssize_t n = read(fd, buf + got, (size_t)(max - got));
        if (n <= 0) break;
        got += (int)n;
    }
    return got;
}

static int write_all(int fd, const char* buf, int len) {
    int put = 0;
    while (put < len) {
        ssize_t n = write(fd, buf + put, (size_t)(len - put));
        if (n <= 0) return put;
        put += (int)n;
    }
    return put;
}

static struct sockaddr_in addr_of(const char* ip, int port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

/* --- the loopback ------------------------------------------------------- */

#define BULK (192 * 1024)

static unsigned char pattern(int i) { return (unsigned char)((i * 7 + i / 251) & 0xFF); }

static void tcp_loopback(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = addr_of("127.0.0.1", 0), got;
    socklen_t len = sizeof(got);
    int port;
    pid_t child;

    ok(ls >= 0, "socket(AF_INET, SOCK_STREAM)");
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ok(bind(ls, (struct sockaddr*)&a, sizeof(a)) == 0, "bind to 127.0.0.1, any port");
    ok(getsockname(ls, (struct sockaddr*)&got, &len) == 0 &&
       got.sin_family == AF_INET && ntohs(got.sin_port) != 0,
       "getsockname gives the port it chose");
    port = ntohs(got.sin_port);
    ok(listen(ls, 4) == 0, "listen");

    {
        int again = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in same = addr_of("127.0.0.1", port);
        ok(bind(again, (struct sockaddr*)&same, sizeof(same)) < 0 &&
           errno == EADDRINUSE, "a second bind to that port is EADDRINUSE");
        close(again);
    }

    child = fork();
    if (child == 0) {
        /* The client: a line, a reply, then the bulk the other way, and
         * shutdown so the server sees the end of it. */
        int c = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in to = addr_of("127.0.0.1", port);
        char reply[64];
        unsigned char* bulk = malloc(BULK);
        int n, i, flag = 1;

        if (connect(c, (struct sockaddr*)&to, sizeof(to)) != 0) _exit(10);
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        if (write_all(c, "hello over loopback\n", 20) != 20) _exit(11);
        n = read_all(c, reply, 21);
        if (n != 21 || memcmp(reply, "HELLO OVER LOOPBACK\n!", 21) != 0) _exit(12);
        for (i = 0; i < BULK; i++) bulk[i] = pattern(i);
        if (write_all(c, (char*)bulk, BULK) != BULK) _exit(13);
        if (shutdown(c, SHUT_WR) != 0) _exit(14);
        n = read_all(c, reply, sizeof(reply));
        if (n != 3 || memcmp(reply, "end", 3) != 0) _exit(15);
        close(c);
        _exit(0);
    }

    {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int s = accept(ls, (struct sockaddr*)&peer, &plen);
        char line[32];
        unsigned char* bulk = malloc(BULK + 1);
        int n, i, good = 1, status = -1;
        struct sockaddr_in me, them;
        socklen_t l1 = sizeof(me), l2 = sizeof(them);

        ok(s >= 0, "accept a connection over the loopback");
        ok(plen == sizeof(peer) && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
           "accept reports the peer as 127.0.0.1");
        ok(getsockname(s, (struct sockaddr*)&me, &l1) == 0 &&
           ntohs(me.sin_port) == port, "the accepted socket's name is the listener's port");
        ok(getpeername(s, (struct sockaddr*)&them, &l2) == 0 &&
           them.sin_port == peer.sin_port, "getpeername agrees with accept");

        n = read_all(s, line, 20);
        ok(n == 20 && memcmp(line, "hello over loopback\n", 20) == 0,
           "the server reads the client's line");
        for (i = 0; i < 20; i++)
            if (line[i] >= 'a' && line[i] <= 'z') line[i] = (char)(line[i] - 32);
        line[20] = '!';
        ok(write_all(s, line, 21) == 21, "and answers it");

        n = read_all(s, (char*)bulk, BULK + 1);
        for (i = 0; i < BULK && i < n; i++)
            if (bulk[i] != pattern(i)) { good = 0; break; }
        ok(n == BULK && good, "192KB arrive intact, then the end of file");
        ok(write_all(s, "end", 3) == 3, "a write after the peer's shutdown");
        close(s);

        waitpid(child, &status, 0);
        ok(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "the client saw its reply and the last word");
        if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            printf("  client status %d\n", status);
    }

    {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in to = addr_of("127.0.0.1", port);
        close(ls);
        ok(connect(c, (struct sockaddr*)&to, sizeof(to)) < 0 && errno == ECONNREFUSED,
           "a connect after the listener closed is ECONNREFUSED");
        close(c);
    }
}

static void tcp_nonblocking(void) {
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in a = addr_of("127.0.0.1", 0), got;
    socklen_t len = sizeof(got);
    int c, s, r, err = -1;
    socklen_t elen = sizeof(err);
    struct pollfd p;

    bind(ls, (struct sockaddr*)&a, sizeof(a));
    getsockname(ls, (struct sockaddr*)&got, &len);
    listen(ls, 1);
    ok(accept(ls, 0, 0) < 0 && errno == EAGAIN,
       "accept on an empty non-blocking listener is EAGAIN");

    c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    r = connect(c, (struct sockaddr*)&got, sizeof(got));
    ok(r == 0 || errno == EINPROGRESS, "a non-blocking connect starts");

    p.fd = c; p.events = POLLOUT; p.revents = 0;
    ok(poll(&p, 1, 5000) == 1 && (p.revents & POLLOUT), "poll says it is writable once connected");
    ok(getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0,
       "SO_ERROR is 0");

    p.fd = ls; p.events = POLLIN; p.revents = 0;
    ok(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN), "the listener polls readable");
    /* accept4, because an accepted socket does not inherit O_NONBLOCK
     * from its listener - on Linux, and so here. */
    s = accept4(ls, 0, 0, SOCK_NONBLOCK);
    ok(s >= 0, "and accept4 takes it, non-blocking");

    ok(read(s, &r, 1) < 0 && errno == EAGAIN, "a non-blocking read with nothing there is EAGAIN");
    ok(write(c, "x", 1) == 1, "one byte the other way");
    p.fd = s; p.events = POLLIN; p.revents = 0;
    ok(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN), "poll wakes for the byte");
    {
        char pk = 0;
        ok(recv(s, &pk, 1, MSG_PEEK) == 1 && pk == 'x',
           "MSG_PEEK sees the byte without taking it");
    }
    close(c);
    {
        char b[4];
        int n1 = (int)read(s, b, sizeof(b));
        int n2 = -1;
        for (int i = 0; i < 500; i++) {
            n2 = (int)read(s, b, sizeof(b));
            if (n2 >= 0 || errno != EAGAIN) break;
            usleep(10000);
        }
        ok(n1 == 1 && n2 == 0, "the byte, then end of file after the peer closes");
    }
    close(s);
    close(ls);

    {
        int d = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        struct sockaddr_in none = addr_of("127.0.0.1", 9);
        r = connect(d, (struct sockaddr*)&none, sizeof(none));
        if (r < 0 && errno == EINPROGRESS) {
            p.fd = d; p.events = POLLOUT; p.revents = 0;
            poll(&p, 1, 5000);
            err = 0; elen = sizeof(err);
            getsockopt(d, SOL_SOCKET, SO_ERROR, &err, &elen);
        } else {
            err = errno;
        }
        ok(err == ECONNREFUSED, "a connect to a closed port is refused");
        close(d);
    }
}

static void udp_loopback(void) {
    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in at = addr_of("127.0.0.1", 0), bname, from;
    socklen_t len = sizeof(bname), flen = sizeof(from);
    char buf[64];
    ssize_t n;

    ok(a >= 0 && b >= 0, "socket(AF_INET, SOCK_DGRAM) twice");
    ok(bind(b, (struct sockaddr*)&at, sizeof(at)) == 0, "bind one of them");
    getsockname(b, (struct sockaddr*)&bname, &len);
    ok(sendto(a, "datagram one", 12, 0, (struct sockaddr*)&bname, sizeof(bname)) == 12,
       "sendto over the loopback");
    n = recvfrom(b, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
    ok(n == 12 && memcmp(buf, "datagram one", 12) == 0, "recvfrom gets it");
    ok(from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && from.sin_port != 0,
       "from 127.0.0.1 and the sender's own port");
    ok(sendto(b, "and back", 8, 0, (struct sockaddr*)&from, flen) == 8 &&
       recv(a, buf, sizeof(buf), 0) == 8 && memcmp(buf, "and back", 8) == 0,
       "the answer reaches the unbound sender");
    ok(connect(a, (struct sockaddr*)&bname, sizeof(bname)) == 0 &&
       send(a, "connected", 9, 0) == 9 && read(b, buf, sizeof(buf)) == 9,
       "a connected UDP socket sends with send(2)");
    {
        struct pollfd p = { b, POLLIN, 0 };
        ok(poll(&p, 1, 0) == 0, "and nothing is left queued");
    }
    close(a);
    close(b);
}

static void interfaces(int external) {
    struct ifaddrs* list = 0;
    int lo = 0, eth = 0;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!strcmp(i->ifa_name, "lo")) lo = 1;
            if (!strcmp(i->ifa_name, "eth0")) eth = 1;
        }
        freeifaddrs(list);
    }
    ok(lo, "getifaddrs lists lo with an IPv4 address");
    if (external) ok(eth, "getifaddrs lists eth0 too (guest only)");

    /* The ioctls Wine's nsiproxy asks a UDP socket about each name. */
    {
        int u = socket(AF_INET, SOCK_DGRAM, 0);
        struct ifreq r;
        memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo");
        ok(ioctl(u, SIOCGIFFLAGS, &r) == 0 && (r.ifr_flags & IFF_UP) &&
           (r.ifr_flags & IFF_LOOPBACK), "SIOCGIFFLAGS: lo is up and a loopback");
        memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo");
        ok(ioctl(u, SIOCGIFHWADDR, &r) == 0 &&
           r.ifr_hwaddr.sa_family == ARPHRD_LOOPBACK, "SIOCGIFHWADDR: lo is ARPHRD_LOOPBACK");
        memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo");
        ok(ioctl(u, SIOCGIFINDEX, &r) == 0 && r.ifr_ifindex == (int)if_nametoindex("lo"),
           "SIOCGIFINDEX agrees with if_nametoindex");
        memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "nosuch0");
        ok(ioctl(u, SIOCGIFFLAGS, &r) < 0 && errno == ENODEV,
           "an interface that does not exist is ENODEV");
        if (external) {
            FILE* f = fopen("/proc/net/route", "r");
            char line[256];
            int deflt = 0;
            memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "eth0");
            ok(ioctl(u, SIOCGIFHWADDR, &r) == 0 && r.ifr_hwaddr.sa_family == ARPHRD_ETHER,
               "SIOCGIFHWADDR: eth0 is Ethernet (guest only)");
            while (f && fgets(line, sizeof(line), f))
                if (!strncmp(line, "eth0\t00000000\t0202000A", 22)) deflt = 1;
            if (f) fclose(f);
            ok(deflt, "/proc/net/route has the default route via 10.0.2.2 (guest only)");
        }
        close(u);
    }
}

/* --- through QEMU, to the build machine --------------------------------- */

static void http_fetch(void) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = addr_of("10.0.2.2", HOST_HTTP_PORT);
    static char page[65536];
    const char* req = "GET /novaris.txt HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
    int n;

    ok(connect(c, (struct sockaddr*)&to, sizeof(to)) == 0,
       "connect to the host's HTTP server (guest only)");
    write_all(c, req, (int)strlen(req));
    n = read_all(c, page, sizeof(page) - 1);
    page[n > 0 ? n : 0] = 0;
    ok(strstr(page, " 200 ") != 0, "HTTP/1.0 200 from the host (guest only)");
    ok(strstr(page, "served to NovarisOS over TCP") != 0,
       "and the file's body (guest only)");
    close(c);
}

static void dns_query(void) {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = addr_of("10.0.2.2", HOST_DNS_PORT);
    unsigned char q[512], r[512];
    int n = 0, got;
    const char* labels[] = { "example", "com", 0 };
    struct pollfd p;

    q[n++] = 0x4E; q[n++] = 0x56;           /* id */
    q[n++] = 0x01; q[n++] = 0x00;           /* recursion desired */
    q[n++] = 0; q[n++] = 1;                 /* one question */
    q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    for (int i = 0; labels[i]; i++) {
        int l = (int)strlen(labels[i]);
        q[n++] = (unsigned char)l;
        memcpy(q + n, labels[i], (size_t)l);
        n += l;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;                 /* A */
    q[n++] = 0; q[n++] = 1;                 /* IN */

    ok(sendto(u, q, (size_t)n, 0, (struct sockaddr*)&to, sizeof(to)) == n,
       "a DNS question to the host (guest only)");
    p.fd = u; p.events = POLLIN; p.revents = 0;
    got = poll(&p, 1, 5000) == 1 ? (int)recv(u, r, sizeof(r), 0) : -1;
    ok(got > 12 && r[0] == 0x4E && r[1] == 0x56 && ((r[6] << 8) | r[7]) == 1,
       "an answer with 1 record (guest only)");
    if (got >= 16)
        ok(r[got - 4] == 192 && r[got - 3] == 0 && r[got - 2] == 2 && r[got - 1] == 1,
           "example.com is 192.0.2.1 (guest only)");
    close(u);
}

static void accept_from_host(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1, s;
    struct sockaddr_in a = addr_of("0.0.0.0", GUEST_LISTEN), peer;
    socklen_t plen = sizeof(peer);
    char line[128];
    int n;

    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ok(bind(ls, (struct sockaddr*)&a, sizeof(a)) == 0 && listen(ls, 1) == 0,
       "listen on port 8080 (guest only)");
    printf("waiting for the host to connect\n");
    fflush(stdout);
    s = accept(ls, (struct sockaddr*)&peer, &plen);
    ok(s >= 0, "accept a connection from the host (guest only)");
    n = (int)read(s, line, sizeof(line) - 1);
    line[n > 0 ? n : 0] = 0;
    ok(strstr(line, "hello from the host") != 0, "read the host's line (guest only)");
    write_all(s, "hello from NovarisOS\n", 21);
    close(s);
    close(ls);
}

int main(int argc, char** argv) {
    int external = argc > 1 && !strcmp(argv[1], "--external");

    setvbuf(stdout, 0, _IOLBF, 0);
    printf("inet64: TCP/IP from a process\n");
    tcp_loopback();
    tcp_nonblocking();
    udp_loopback();
    interfaces(external);
    if (external) {
        http_fetch();
        dns_query();
        accept_from_host();
    }
    printf("inet64: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
