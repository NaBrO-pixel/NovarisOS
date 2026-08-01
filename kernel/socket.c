/* socket.c - Unix domain sockets. See include/socket.h for why.
 *
 * The shape of a SOCK_STREAM Unix socket is two byte queues and a peer
 * pointer: what you write goes into the peer's queue, what you read comes
 * out of your own. Everything else - socketpair, bind/listen/connect/
 * accept, shutdown, EOF - is bookkeeping around that.
 *
 * Two things make it more than a pipe, and both are there because Wine
 * needs them:
 *
 *   - a *name*. wineserver binds a socket at a path and clients connect
 *     to it, so the filesystem has to be able to hold one.
 *   - SCM_RIGHTS. A descriptor sent over a socket arrives at the other
 *     end as a descriptor. That is how wineserver hands a client the fd
 *     behind a Windows HANDLE, and a socket layer without it would look
 *     complete and be useless for the thing it was built for.
 *
 * Waiting is real, not a spin: a receive with nothing to read parks the
 * task on Milestone 25's blocking path and the sender wakes it.
 */

#include "socket.h"
#include "posix.h"
#include "kheap.h"
#include "kstring.h"
#include "scheduler.h"
#include "console.h"

#define MAX_SOCKETS   32
#define SOCK_BUF_SIZE 16384      /* per direction */
#define MAX_BACKLOG   4
#define MAX_PASSED_FDS 8
#define MAX_BOUND     8

typedef enum {
    SS_FREE = 0,
    SS_UNBOUND,
    SS_BOUND,
    SS_LISTENING,
    SS_CONNECTED,
} sock_state_t;

/* A descriptor waiting to be picked up by recvmsg. Stored as the payload
 * posix.c hands over rather than as an fd number, because the number is
 * the *sender's* and means nothing to the receiver. */
typedef struct {
    uint32_t token;
} passed_fd_t;

struct socket {
    sock_state_t state;
    int          type;           /* SOCK_STREAM only */

    socket_t*    peer;           /* the other end, or 0 */
    int          peer_closed;    /* peer is gone: reads drain then EOF */
    int          shut_rd, shut_wr;
    int          nonblock;

    /* This socket's *receive* queue - what the peer wrote. A plain ring
     * buffer; a stream socket has no message boundaries to preserve. */
    uint8_t*     buf;
    uint32_t     head, tail, count;

    /* Descriptors the peer sent with SCM_RIGHTS, in arrival order. */
    passed_fd_t  passed[MAX_PASSED_FDS];
    uint32_t     passed_count;

    /* Listening sockets only: connections waiting for accept(). */
    socket_t*    backlog[MAX_BACKLOG];
    uint32_t     backlog_count;

    char         path[108];      /* the name it is bound to, if any */
};

static socket_t sockets[MAX_SOCKETS];

/* Bound names. A small table rather than a filesystem node type: a socket
 * is not a file, nothing can open() it, and giving the ramfs a node kind
 * that only one subsystem understands would be worse than a lookup. */
static struct {
    char      path[108];
    socket_t* sock;
} bound[MAX_BOUND];

static uint32_t stat_bytes = 0, stat_fds = 0;

/* --- housekeeping -------------------------------------------------------- */

static socket_t* sock_alloc(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].state == SS_FREE) {
            socket_t* s = &sockets[i];
            kmemset(s, 0, sizeof(*s));
            s->buf = (uint8_t*)kmalloc(SOCK_BUF_SIZE);
            if (!s->buf) return 0;
            s->state = SS_UNBOUND;
            s->type = SOCK_STREAM;
            return s;
        }
    }
    return 0;
}

static void sock_free(socket_t* s) {
    if (!s || s->state == SS_FREE) return;
    for (int i = 0; i < MAX_BOUND; i++) {
        if (bound[i].sock == s) { bound[i].sock = 0; bound[i].path[0] = '\0'; }
    }
    if (s->buf) kfree(s->buf);
    kmemset(s, 0, sizeof(*s));
    s->state = SS_FREE;
}

static int path_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static socket_t* find_bound(const char* path) {
    for (int i = 0; i < MAX_BOUND; i++) {
        if (bound[i].sock && path_eq(bound[i].path, path)) return bound[i].sock;
    }
    return 0;
}

/* --- the byte queue ------------------------------------------------------ */

static uint32_t q_space(const socket_t* s) { return SOCK_BUF_SIZE - s->count; }

static uint32_t q_put(socket_t* s, const uint8_t* src, uint32_t n) {
    uint32_t space = q_space(s);
    if (n > space) n = space;
    for (uint32_t i = 0; i < n; i++) {
        s->buf[s->tail] = src[i];
        s->tail = (s->tail + 1) % SOCK_BUF_SIZE;
    }
    s->count += n;
    return n;
}

static uint32_t q_get(socket_t* s, uint8_t* dst, uint32_t n, int peek) {
    if (n > s->count) n = s->count;
    uint32_t head = s->head;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = s->buf[head];
        head = (head + 1) % SOCK_BUF_SIZE;
    }
    if (!peek) { s->head = head; s->count -= n; }
    return n;
}

/* --- transfer ------------------------------------------------------------ */

int32_t socket_write(socket_t* s, const void* buf, uint32_t len) {
    if (!s || s->state != SS_CONNECTED) return -ENOTCONN;
    if (s->shut_wr) return -EPIPE;
    if (s->peer_closed || !s->peer) return -EPIPE;
    if (len == 0) return 0;

    uint32_t n = q_put(s->peer, (const uint8_t*)buf, len);
    if (n == 0) return -EAGAIN;   /* the peer's queue is full */
    stat_bytes += n;

    /* Anyone parked on the peer's receive queue can go. */
    scheduler_wake_on((uint32_t)s->peer, MAX_SOCKETS, 0);
    return (int32_t)n;
}

int32_t socket_read(socket_t* s, void* buf, uint32_t len, registers_t* regs) {
    if (!s) return -EBADF;
    if (s->shut_rd) return 0;
    if (len == 0) return 0;

    if (s->count == 0) {
        /* Nothing buffered. If the peer has gone this is end of stream,
         * which is 0 and not an error - the distinction every protocol
         * loop is built on. */
        if (s->peer_closed || !s->peer) return 0;
        if (s->nonblock) return -EAGAIN;
        /* Park on this socket's own address and have the whole syscall
         * re-execute when somebody writes to it. */
        posix_request_block((uint32_t)s, regs);
        return 0;   /* not used: the call is going to happen again */
    }

    uint32_t n = q_get(s, (uint8_t*)buf, len, 0);
    return (int32_t)n;
}

void socket_close(socket_t* s) {
    if (!s) return;
    if (s->peer) {
        s->peer->peer_closed = 1;
        s->peer->peer = 0;
        /* A reader waiting on the peer has to find out it will never get
         * anything, or it waits for ever. */
        scheduler_wake_on((uint32_t)s->peer, MAX_SOCKETS, 0);
    }
    for (uint32_t i = 0; i < s->backlog_count; i++) sock_free(s->backlog[i]);
    sock_free(s);
}

/* --- connecting a pair --------------------------------------------------- */

static void link_pair(socket_t* a, socket_t* b) {
    a->peer = b;  b->peer = a;
    a->state = SS_CONNECTED;
    b->state = SS_CONNECTED;
}

/* --- the sub-calls -------------------------------------------------------- */

static int32_t do_socket(int domain, int type, int protocol) {
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
    type &= ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
    /* A type this kernel does not have is EINVAL; a protocol it does not
     * have is EPROTONOSUPPORT. Linux distinguishes the two and so does
     * this - both were checked against the host rather than guessed. */
    if (type != SOCK_STREAM) return -EINVAL;
    if (protocol != 0) return -EPROTONOSUPPORT;

    socket_t* s = sock_alloc();
    if (!s) return -ENFILE;
    s->nonblock = (flags & SOCK_NONBLOCK) ? 1 : 0;

    int fd = posix_fd_install_socket(s);
    if (fd < 0) { sock_free(s); return fd; }
    return fd;
}

static int32_t do_socketpair(int domain, int type, int protocol, int* sv) {
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    if (!sv) return -EFAULT;
    int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
    type &= ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (type != SOCK_STREAM) return -EINVAL;
    if (protocol != 0) return -EPROTONOSUPPORT;

    socket_t* a = sock_alloc();
    socket_t* b = a ? sock_alloc() : 0;
    if (!a || !b) { sock_free(a); sock_free(b); return -ENFILE; }
    a->nonblock = b->nonblock = (flags & SOCK_NONBLOCK) ? 1 : 0;
    link_pair(a, b);

    int fa = posix_fd_install_socket(a);
    int fb = fa >= 0 ? posix_fd_install_socket(b) : -EMFILE;
    if (fa < 0 || fb < 0) { socket_close(a); socket_close(b); return -EMFILE; }
    sv[0] = fa;
    sv[1] = fb;
    return 0;
}

static int32_t do_bind(socket_t* s, const sockaddr_un_t* addr, uint32_t len) {
    if (!s || !addr) return -EFAULT;
    if (len < 3 || addr->sun_family != AF_UNIX) return -EINVAL;
    if (s->state != SS_UNBOUND) return -EINVAL;
    if (find_bound(addr->sun_path)) return -EADDRINUSE;

    for (int i = 0; i < MAX_BOUND; i++) {
        if (!bound[i].sock) {
            kstrlcpy(bound[i].path, addr->sun_path, sizeof(bound[i].path));
            bound[i].sock = s;
            kstrlcpy(s->path, addr->sun_path, sizeof(s->path));
            s->state = SS_BOUND;
            return 0;
        }
    }
    return -ENOSPC;
}

static int32_t do_listen(socket_t* s, int backlog) {
    (void)backlog;
    if (!s) return -EBADF;
    if (s->state != SS_BOUND) return -EINVAL;
    s->state = SS_LISTENING;
    return 0;
}

static int32_t do_connect(socket_t* s, const sockaddr_un_t* addr, uint32_t len) {
    if (!s || !addr) return -EFAULT;
    if (len < 3 || addr->sun_family != AF_UNIX) return -EINVAL;
    if (s->state == SS_CONNECTED) return -EISCONN;

    socket_t* listener = find_bound(addr->sun_path);
    /* The two failures are different and both matter. Nothing bound to
     * that name at all is ENOENT, because the name is a filesystem path
     * and there is no file - checked against the host, which is how the
     * first version of this being ECONNREFUSED was caught. Bound but not
     * listening *is* ECONNREFUSED. Wine reads the number to decide
     * whether to start a wineserver of its own. */
    if (!listener) return -ENOENT;
    if (listener->state != SS_LISTENING) return -ECONNREFUSED;
    if (listener->backlog_count >= MAX_BACKLOG) return -EAGAIN;

    /* The server's end of the new connection. It is created here rather
     * than in accept() so that a client can connect before the server
     * gets round to accepting, which is the whole point of a backlog. */
    socket_t* server_end = sock_alloc();
    if (!server_end) return -ENFILE;
    link_pair(s, server_end);

    listener->backlog[listener->backlog_count++] = server_end;
    scheduler_wake_on((uint32_t)listener, MAX_SOCKETS, 0);
    return 0;
}

static int32_t do_accept(socket_t* s, sockaddr_un_t* addr, uint32_t* addrlen,
                         int flags, registers_t* regs) {
    if (!s) return -EBADF;
    if (s->state != SS_LISTENING) return -EINVAL;

    if (s->backlog_count == 0) {
        if (s->nonblock || (flags & SOCK_NONBLOCK)) return -EAGAIN;
        posix_request_block((uint32_t)s, regs);
        return 0;   /* the call will happen again */
    }

    socket_t* c = s->backlog[0];
    for (uint32_t i = 1; i < s->backlog_count; i++) s->backlog[i - 1] = s->backlog[i];
    s->backlog_count--;

    if (addr && addrlen) {
        /* An accepted Unix socket has no name of its own, which Linux
         * reports as a family and nothing else. */
        addr->sun_family = AF_UNIX;
        addr->sun_path[0] = '\0';
        *addrlen = 2;
    }
    int fd = posix_fd_install_socket(c);
    if (fd < 0) { socket_close(c); return fd; }
    return fd;
}

/* --- SCM_RIGHTS ----------------------------------------------------------
 *
 * The control data on a Unix socket is a list of cmsghdrs; the only one
 * that matters here carries file descriptors. Sending one hands the
 * *object* to the socket, not the number - the number is the sender's and
 * means nothing at the other end - and receiving installs it as a fresh
 * descriptor in the receiver. */

static int32_t send_control(socket_t* s, const uint8_t* ctl, uint32_t len) {
    if (!s->peer) return -EPIPE;
    uint32_t off = 0;
    while (off + sizeof(k_cmsghdr_t) <= len) {
        const k_cmsghdr_t* c = (const k_cmsghdr_t*)(ctl + off);
        if (c->cmsg_len < sizeof(k_cmsghdr_t) || off + c->cmsg_len > len) break;

        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            uint32_t nfds = (c->cmsg_len - sizeof(k_cmsghdr_t)) / 4;
            const int32_t* fdv = (const int32_t*)(ctl + off + sizeof(k_cmsghdr_t));
            for (uint32_t i = 0; i < nfds; i++) {
                if (s->peer->passed_count >= MAX_PASSED_FDS) return -EMSGSIZE;
                uint32_t token = posix_fd_export(fdv[i]);
                if (!token) return -EBADF;
                s->peer->passed[s->peer->passed_count++].token = token;
                stat_fds++;
            }
        }
        off += (c->cmsg_len + 3u) & ~3u;
    }
    return 0;
}

static uint32_t recv_control(socket_t* s, uint8_t* ctl, uint32_t len) {
    if (!s->passed_count || len < sizeof(k_cmsghdr_t) + 4) return 0;

    uint32_t room = (len - sizeof(k_cmsghdr_t)) / 4;
    uint32_t n = s->passed_count < room ? s->passed_count : room;

    k_cmsghdr_t* c = (k_cmsghdr_t*)ctl;
    c->cmsg_len = sizeof(k_cmsghdr_t) + n * 4;
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;

    int32_t* fdv = (int32_t*)(ctl + sizeof(k_cmsghdr_t));
    for (uint32_t i = 0; i < n; i++) {
        fdv[i] = posix_fd_import(s->passed[i].token);
    }
    for (uint32_t i = n; i < s->passed_count; i++) s->passed[i - n] = s->passed[i];
    s->passed_count -= n;
    return c->cmsg_len;
}

/* --- sendmsg / recvmsg ---------------------------------------------------- */

typedef struct { uint32_t iov_base; uint32_t iov_len; } k_iovec_t;

static int32_t do_sendmsg(socket_t* s, const k_msghdr_t* m, int flags) {
    (void)flags;
    if (!s || !m) return -EFAULT;

    if (m->msg_control && m->msg_controllen) {
        int32_t r = send_control(s, (const uint8_t*)m->msg_control,
                                 m->msg_controllen);
        if (r < 0) return r;
    }

    int32_t total = 0;
    const k_iovec_t* iov = (const k_iovec_t*)m->msg_iov;
    for (uint32_t i = 0; i < m->msg_iovlen; i++) {
        if (!iov[i].iov_len) continue;
        int32_t n = socket_write(s, (const void*)iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint32_t)n < iov[i].iov_len) break;   /* the queue filled up */
    }
    return total;
}

static int32_t do_recvmsg(socket_t* s, k_msghdr_t* m, int flags,
                          registers_t* regs) {
    if (!s || !m) return -EFAULT;

    /* Control data first, and *without* requiring payload bytes: a
     * descriptor can legitimately arrive with a zero-length message, and
     * a receiver that waited for bytes first would hang. */
    uint32_t ctl_len = 0;
    if (m->msg_control && m->msg_controllen) {
        ctl_len = recv_control(s, (uint8_t*)m->msg_control, m->msg_controllen);
    }
    m->msg_controllen = ctl_len;
    m->msg_flags = 0;

    int32_t total = 0;
    k_iovec_t* iov = (k_iovec_t*)m->msg_iov;
    for (uint32_t i = 0; i < m->msg_iovlen; i++) {
        if (!iov[i].iov_len) continue;
        if (s->count == 0) {
            if (total || ctl_len) break;     /* got something; do not wait */
            if (s->peer_closed || !s->peer) return 0;
            if (s->nonblock || (flags & MSG_DONTWAIT)) return -EAGAIN;
            posix_request_block((uint32_t)s, regs);
            return 0;
        }
        uint32_t n = q_get(s, (uint8_t*)iov[i].iov_base, iov[i].iov_len,
                           (flags & MSG_PEEK) ? 1 : 0);
        total += (int32_t)n;
        if (n < iov[i].iov_len) break;
    }
    return total;
}

/* --- the demultiplexer ---------------------------------------------------- */

int32_t socket_syscall(uint32_t call, uint32_t* a, registers_t* regs) {
    if (!a) return -EFAULT;

    switch (call) {
        case SYS_SOCKET:
            return do_socket((int)a[0], (int)a[1], (int)a[2]);

        case SYS_SOCKETPAIR:
            return do_socketpair((int)a[0], (int)a[1], (int)a[2], (int*)a[3]);

        case SYS_BIND:
            return do_bind(posix_fd_socket((int)a[0]),
                           (const sockaddr_un_t*)a[1], a[2]);

        case SYS_LISTEN:
            return do_listen(posix_fd_socket((int)a[0]), (int)a[1]);

        case SYS_CONNECT:
            return do_connect(posix_fd_socket((int)a[0]),
                              (const sockaddr_un_t*)a[1], a[2]);

        case SYS_ACCEPT:
            return do_accept(posix_fd_socket((int)a[0]), (sockaddr_un_t*)a[1],
                             (uint32_t*)a[2], 0, regs);
        case SYS_ACCEPT4:
            return do_accept(posix_fd_socket((int)a[0]), (sockaddr_un_t*)a[1],
                             (uint32_t*)a[2], (int)a[3], regs);

        case SYS_SEND:
            return socket_write(posix_fd_socket((int)a[0]), (const void*)a[1], a[2]);
        case SYS_SENDTO:
            /* A connected stream socket ignores the destination. */
            return socket_write(posix_fd_socket((int)a[0]), (const void*)a[1], a[2]);

        case SYS_RECV:
            return socket_read(posix_fd_socket((int)a[0]), (void*)a[1], a[2], regs);
        case SYS_RECVFROM:
            if (a[4]) ((sockaddr_un_t*)a[4])->sun_family = AF_UNIX;
            if (a[5]) *(uint32_t*)a[5] = 2;
            return socket_read(posix_fd_socket((int)a[0]), (void*)a[1], a[2], regs);

        case SYS_SENDMSG:
            return do_sendmsg(posix_fd_socket((int)a[0]), (const k_msghdr_t*)a[1],
                              (int)a[2]);
        case SYS_RECVMSG:
            return do_recvmsg(posix_fd_socket((int)a[0]), (k_msghdr_t*)a[1],
                              (int)a[2], regs);

        case SYS_SHUTDOWN: {
            socket_t* s = posix_fd_socket((int)a[0]);
            if (!s) return -ENOTSOCK;
            int how = (int)a[1];
            if (how == SHUT_RD || how == SHUT_RDWR) s->shut_rd = 1;
            if (how == SHUT_WR || how == SHUT_RDWR) {
                s->shut_wr = 1;
                /* The peer has to see end-of-stream, which is the whole
                 * purpose of a half close. */
                if (s->peer) {
                    s->peer->peer_closed = 1;
                    scheduler_wake_on((uint32_t)s->peer, MAX_SOCKETS, 0);
                }
            }
            return 0;
        }

        case SYS_GETSOCKNAME: {
            socket_t* s = posix_fd_socket((int)a[0]);
            if (!s) return -ENOTSOCK;
            sockaddr_un_t* addr = (sockaddr_un_t*)a[1];
            if (!addr || !a[2]) return -EFAULT;
            addr->sun_family = AF_UNIX;
            kstrlcpy(addr->sun_path, s->path, sizeof(addr->sun_path));
            *(uint32_t*)a[2] = 2 + kstrlen(s->path) + 1;
            return 0;
        }
        case SYS_GETPEERNAME: {
            socket_t* s = posix_fd_socket((int)a[0]);
            if (!s) return -ENOTSOCK;
            if (s->state != SS_CONNECTED) return -ENOTCONN;
            sockaddr_un_t* addr = (sockaddr_un_t*)a[1];
            if (!addr || !a[2]) return -EFAULT;
            addr->sun_family = AF_UNIX;
            addr->sun_path[0] = '\0';
            *(uint32_t*)a[2] = 2;
            return 0;
        }

        case SYS_SETSOCKOPT:
            /* Nothing here has an option worth setting, and refusing
             * would fail programs that only set buffer sizes and
             * timeouts they do not depend on. */
            return posix_fd_socket((int)a[0]) ? 0 : -ENOTSOCK;
        case SYS_GETSOCKOPT: {
            if (!posix_fd_socket((int)a[0])) return -ENOTSOCK;
            /* SO_ERROR (4) is the one a client reads after connect, and
             * the honest answer here is always "no error". */
            if (a[3] && a[4] && *(uint32_t*)a[4] >= 4) {
                *(int32_t*)a[3] = 0;
                *(uint32_t*)a[4] = 4;
            }
            return 0;
        }

        default:
            return -ENOSYS;
    }
}

/* --- lifecycle ------------------------------------------------------------ */

void socket_process_begin(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) sockets[i].state = SS_FREE;
    for (int i = 0; i < MAX_BOUND; i++) { bound[i].sock = 0; bound[i].path[0] = '\0'; }
    stat_bytes = 0;
    stat_fds = 0;
}

void socket_process_end(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].state != SS_FREE) sock_free(&sockets[i]);
    }
    for (int i = 0; i < MAX_BOUND; i++) { bound[i].sock = 0; bound[i].path[0] = '\0'; }
}

uint32_t socket_open_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_SOCKETS; i++) if (sockets[i].state != SS_FREE) n++;
    return n;
}
uint32_t socket_bytes_moved(void) { return stat_bytes; }
uint32_t socket_fds_passed(void) { return stat_fds; }
