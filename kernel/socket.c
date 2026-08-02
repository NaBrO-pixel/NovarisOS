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
#include "vfs.h"

/* Raised in Milestone 30 for the same reason the descriptor table was:
 * wineserver plus two clients is already a dozen sockets and pipes, and
 * every Windows handle backed by a file is another descriptor in flight. */
#define MAX_SOCKETS   64
#define SOCK_BUF_SIZE 16384      /* per direction */
#define MAX_BACKLOG   8
#define MAX_PASSED_FDS 16
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
    /* The byte offset in the receiver's stream at which the message
     * carrying this descriptor begins. Milestone 29 found out why that
     * has to be recorded: on a real Unix socket ancillary data belongs to
     * a *message*, and a receive never merges the control data of two
     * messages or reads past the point where the next message's begins.
     * Novaris attached it to the socket instead, so wineserver - which
     * receives with a 256-byte control buffer - collected both of the
     * client's passed descriptors in one call, used the first and
     * discarded the second. It then killed the client for failing to
     * send a descriptor it had in fact sent. */
    uint32_t at;
} passed_fd_t;

struct socket {
    sock_state_t state;
    int          type;           /* SOCK_STREAM only */

    /* How many descriptors name this socket. One at creation; dup() and
     * fork() add more, and close() only tears the socket down when the
     * last one goes. Milestone 28 had no need for this because a
     * descriptor and a socket were the same thing. */
    int          refs;

    socket_t*    peer;           /* the other end, or 0 */
    int          peer_closed;    /* peer is gone: reads drain then EOF */
    int          shut_rd, shut_wr;
    int          nonblock;

    /* This socket's *receive* queue - what the peer wrote. A plain ring
     * buffer; a stream socket has no message boundaries to preserve. */
    uint8_t*     buf;
    uint32_t     head, tail, count;
    /* Monotonic byte counters, so a descriptor can name a position in the
     * stream rather than a place in the ring. */
    uint32_t     rx_written;   /* bytes ever queued into this socket */
    uint32_t     rx_read;      /* bytes ever taken out of it */

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
            s->refs = 1;
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
    s->rx_written += n;
    return n;
}

/* How many bytes a read may take before it would cross into a message
 * that carries its own descriptors. Linux stops a receive there rather
 * than merging the two, and so does this. All of `count` when no
 * descriptor is waiting further along the stream. */
static uint32_t q_limit(const socket_t* s) {
    uint32_t limit = s->count;
    for (uint32_t i = 0; i < s->passed_count; i++) {
        if (s->passed[i].at <= s->rx_read) continue;   /* already reachable */
        uint32_t until = s->passed[i].at - s->rx_read;
        if (until < limit) limit = until;
    }
    return limit;
}

static uint32_t q_get(socket_t* s, uint8_t* dst, uint32_t n, int peek) {
    uint32_t limit = q_limit(s);
    if (n > limit) n = limit;
    uint32_t head = s->head;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = s->buf[head];
        head = (head + 1) % SOCK_BUF_SIZE;
    }
    if (!peek) { s->head = head; s->count -= n; s->rx_read += n; }
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

    /* read(2) has nowhere to put ancillary data, so Linux discards it
     * rather than letting it block the stream. Without this a plain read
     * on a socket whose next message carries a descriptor would be
     * stopped at that boundary for ever, since only recvmsg can clear
     * it. */
    if (s->count && s->passed_count) {
        uint32_t drop = 0;
        while (drop < s->passed_count && s->passed[drop].at <= s->rx_read) drop++;
        if (drop) {
            for (uint32_t i = drop; i < s->passed_count; i++) {
                s->passed[i - drop] = s->passed[i];
            }
            s->passed_count -= drop;
        }
    }

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
    /* Draining this socket's queue is what makes the *peer* writable
     * again, so anyone parked on the peer has to be told. Milestone 31:
     * this was missing, and invisible for as long as every poll also
     * carried a one-tick deadline and re-tested everything anyway. A poll
     * that is really woken by its descriptors notices the difference
     * immediately - as a hang. */
    if (n && s->peer) scheduler_wake_on((uint32_t)s->peer, MAX_SOCKETS, 0);
    return (int32_t)n;
}

void socket_ref(socket_t* s) {
    if (s && s->state != SS_FREE) s->refs++;
}

void socket_close(socket_t* s) {
    if (!s) return;
    /* Another descriptor still names it - a dup, or the same descriptor
     * in a forked child. Nothing happens to the socket, and in
     * particular the peer is *not* told about an end of stream that has
     * not happened. */
    if (s->refs > 1) { s->refs--; return; }
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

/* A bound name is matched as an absolute path, because two processes can
 * bind and connect from different working directories and mean the same
 * socket - which is exactly what Wine's client and server do, both having
 * chdir'd into the config directory first. */
static const char* sock_abs(const char* path, char* buf, uint32_t size) {
    return posix_resolve_path(path, buf, size);
}

static int32_t do_bind(socket_t* s, const sockaddr_un_t* addr, uint32_t len) {
    if (!s || !addr) return -EFAULT;
    if (len < 3 || addr->sun_family != AF_UNIX) return -EINVAL;
    if (s->state != SS_UNBOUND) return -EINVAL;

    char abs[128];
    const char* name = sock_abs(addr->sun_path, abs, sizeof(abs));
    if (find_bound(name)) return -EADDRINUSE;

    for (int i = 0; i < MAX_BOUND; i++) {
        if (bound[i].sock) continue;

        /* The name goes into the filesystem as well as into this table.
         * A client that stats the path before connecting - Wine does,
         * to find out whether the server it started is ready yet - has
         * to be able to see it. */
        const char* leaf = 0;
        vfs_node_t* dir = vfs_resolve_parent(name, &leaf);
        if (!dir) return -ENOENT;
        if (vfs_finddir(dir, leaf)) return -EADDRINUSE;
        vfs_node_t* node = vfs_create(dir, leaf, VFS_SOCKET);
        if (!node) return -ENOSPC;
        node->mode = 0777;

        kstrlcpy(bound[i].path, name, sizeof(bound[i].path));
        bound[i].sock = s;
        kstrlcpy(s->path, name, sizeof(s->path));
        s->state = SS_BOUND;
        return 0;
    }
    return -ENOSPC;
}

void socket_unbind_path(const char* path) {
    for (int i = 0; i < MAX_BOUND; i++) {
        if (bound[i].sock && path_eq(bound[i].path, path)) {
            bound[i].sock->state = SS_UNBOUND;
            bound[i].sock = 0;
            bound[i].path[0] = '\0';
        }
    }
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

    char abs[128];
    socket_t* listener = find_bound(sock_abs(addr->sun_path, abs, sizeof(abs)));
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
                /* Attached at the offset where this message's bytes are
                 * about to be written - send_control() runs before the
                 * payload, which is what makes rx_written the right
                 * number here. */
                s->peer->passed[s->peer->passed_count].token = token;
                s->peer->passed[s->peer->passed_count].at = s->peer->rx_written;
                s->peer->passed_count++;
                stat_fds++;
            }
        }
        off += (c->cmsg_len + 3u) & ~3u;
    }
    return 0;
}

static uint32_t recv_control(socket_t* s, uint8_t* ctl, uint32_t len) {
    if (!s->passed_count || len < sizeof(k_cmsghdr_t) + 4) return 0;

    /* Only the descriptors belonging to the message the reader is at.
     * Anything attached further along the stream belongs to a later
     * recvmsg, however much room the caller offered - which is the whole
     * point, since wineserver offers 256 bytes and would otherwise
     * collect every descriptor the client had sent so far. */
    uint32_t here = 0;
    while (here < s->passed_count && s->passed[here].at <= s->rx_read) here++;
    if (!here) return 0;

    uint32_t room = (len - sizeof(k_cmsghdr_t)) / 4;
    uint32_t n = here < room ? here : room;

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
     * a receiver that waited for bytes first would hang.
     *
     * `room` is remembered because msg_controllen is both an input (how
     * much space the caller has) and an output (how much was used), and a
     * receive that has to wait runs this function *again* from the top -
     * the block below rewinds the whole syscall. Writing the output value
     * before the wait therefore told the second attempt that the caller
     * had no room for control data at all, and the descriptor was
     * silently dropped.
     *
     * That is not hypothetical: it is why Wine's client connected to
     * wineserver, got its four bytes of greeting, and then wrote to
     * descriptor -1 - the request descriptor the server passed it arrived
     * on the attempt that blocked and was thrown away on the one that
     * succeeded. Milestone 28's test never saw it because nothing there
     * ever had to wait for control data. */
    uint32_t room = m->msg_controllen;
    uint32_t ctl_len = 0;
    if (m->msg_control && room) {
        ctl_len = recv_control(s, (uint8_t*)m->msg_control, room);
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
            if (s->nonblock || (flags & MSG_DONTWAIT)) {
                m->msg_controllen = room;
                return -EAGAIN;
            }
            /* Put the caller's header back the way it was: this call is
             * about to happen again from the beginning. */
            m->msg_controllen = room;
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

/* --- pipes and readiness (Milestone 29) ----------------------------------
 *
 * A pipe is a connected pair with each end shut in one direction, which
 * is what a pipe *is* - the byte queue, the blocking read and the
 * end-of-stream when the writer closes are all the socket machinery
 * unchanged. Building it here rather than beside it is the whole reason
 * Milestone 28's queue was written as a queue and not as a socket detail.
 *
 * Wine needs pipes for the same reason every Unix program does: its
 * loader hands a child a descriptor to report back through, and
 * wineserver's own startup handshake is a pipe the parent reads until the
 * server says it is ready. */
int socket_pipe(socket_t** read_end, socket_t** write_end) {
    socket_t* r = sock_alloc();
    socket_t* w = r ? sock_alloc() : 0;
    if (!r || !w) { sock_free(r); sock_free(w); return -ENFILE; }

    link_pair(r, w);
    r->shut_wr = 1;   /* writing to the read end is EPIPE */
    w->shut_rd = 1;   /* reading the write end is end-of-stream */

    *read_end = r;
    *write_end = w;
    return 0;
}

void socket_set_nonblock(socket_t* s, int on) {
    if (s) s->nonblock = on ? 1 : 0;
}

int socket_get_nonblock(socket_t* s) {
    return s ? s->nonblock : 0;
}

int socket_readable(socket_t* s) {
    if (!s) return 0;
    if (s->state == SS_LISTENING) return s->backlog_count > 0;
    if (s->shut_rd) return 1;                  /* read returns 0 at once */
    if (s->count) return 1;
    return s->peer_closed || !s->peer;         /* end of stream is readable */
}

uint32_t socket_poll_mask(socket_t* s) {
    uint32_t mask = 0;
    if (!s) return POLLNVAL;

    if (s->state == SS_LISTENING) {
        /* An incoming connection is what "readable" means on a listening
         * socket, which is what makes poll() and accept() compose. */
        if (s->backlog_count) mask |= POLLIN;
        return mask;
    }

    if (s->count || s->shut_rd) mask |= POLLIN;

    if (s->peer_closed || !s->peer) {
        /* The peer is gone. Both bits, and both matter: POLLHUP is how a
         * server loop learns a client went away, and POLLIN is how the
         * read that returns 0 gets a chance to happen. */
        mask |= POLLHUP | POLLIN;
    } else if (s->state == SS_CONNECTED && !s->shut_wr && q_space(s->peer)) {
        mask |= POLLOUT;
    }
    return mask;
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
