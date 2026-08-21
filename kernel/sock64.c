/* sock64.c - Unix-domain sockets: the part that introduces two
 * processes that have never met. */

#include "sock64.h"
#include "pipe64.h"

typedef struct {
    int rx, tx;                 /* the server side of a pending pair */
} pending64_t;

typedef struct {
    int         state;
    int         node;           /* the filesystem name, or -1 */
    int         backlog;
    pending64_t queue[SOCK64_BACKLOG];
    int         head, count;    /* a ring, so accept is first-come */
    int         used;
} sock64_t;

static sock64_t socks[SOCK64_MAX];
static uint64_t accepted;

void sock64_init(void) {
    for (int i = 0; i < SOCK64_MAX; i++) {
        socks[i].state   = SOCK64_UNBOUND;
        socks[i].node    = -1;
        socks[i].backlog = 0;
        socks[i].head    = 0;
        socks[i].count   = 0;
        socks[i].used    = 0;
    }
    accepted = 0;
}

int sock64_valid(int s) { return s >= 0 && s < SOCK64_MAX && socks[s].used; }
int sock64_state(int s) { return sock64_valid(s) ? socks[s].state : -1; }

int sock64_create(void) {
    int i;
    for (i = 0; i < SOCK64_MAX; i++) if (!socks[i].used) break;
    if (i == SOCK64_MAX) return -1;

    socks[i].state   = SOCK64_UNBOUND;
    socks[i].node    = -1;
    socks[i].backlog = 0;
    socks[i].head    = 0;
    socks[i].count   = 0;
    socks[i].used    = 1;
    return i;
}

void sock64_destroy(int s) {
    if (!sock64_valid(s)) return;

    /* Connections that arrived and were never accepted. Their pipes were
     * referenced by connect on behalf of a server that is now never
     * going to exist, so they are given back here - and giving them back
     * is what makes the caller's own end report end of file instead of
     * waiting for a reply from nobody. */
    while (socks[s].count > 0) {
        pending64_t* p = &socks[s].queue[socks[s].head];
        pipe64_unref(p->rx, 1, 0);
        pipe64_unref(p->tx, 0, 1);
        socks[s].head = (socks[s].head + 1) % SOCK64_BACKLOG;
        socks[s].count--;
    }
    socks[s].used  = 0;
    socks[s].state = SOCK64_UNBOUND;
    socks[s].node  = -1;
}

int sock64_bind(int s, int node) {
    if (!sock64_valid(s)) return -9;                   /* -EBADF */
    if (socks[s].state != SOCK64_UNBOUND) return -22;  /* -EINVAL */
    socks[s].node  = node;
    socks[s].state = SOCK64_BOUND;
    return 0;
}

int sock64_listen(int s, int backlog) {
    if (!sock64_valid(s)) return -9;
    /* Linux allows listen on an unbound socket by autobinding it to an
     * abstract name; nothing here does that, and a listener with no name
     * is one nobody could ever find, so it is refused rather than
     * silently accepted. */
    if (socks[s].state != SOCK64_BOUND &&
        socks[s].state != SOCK64_LISTENING) return -22;
    if (backlog < 1) backlog = 1;
    if (backlog > SOCK64_BACKLOG) backlog = SOCK64_BACKLOG;
    socks[s].backlog = backlog;
    socks[s].state   = SOCK64_LISTENING;
    return 0;
}

int sock64_listener_for_node(int node) {
    if (node < 0) return -1;
    for (int i = 0; i < SOCK64_MAX; i++)
        if (socks[i].used && socks[i].state == SOCK64_LISTENING &&
            socks[i].node == node) return i;
    return -1;
}

int sock64_connect(int s, int listener, int* out_rx, int* out_tx) {
    int a, b;
    pending64_t* slot;

    if (!sock64_valid(s)) return -9;
    if (socks[s].state == SOCK64_CONNECTED) return -106;   /* -EISCONN */
    if (!sock64_valid(listener) ||
        socks[listener].state != SOCK64_LISTENING) return -111; /* -ECONNREFUSED */
    if (socks[listener].count >= socks[listener].backlog) return -11; /* -EAGAIN */

    /* Two pipes, crossed - the same shape socketpair makes, because a
     * connection is the same thing however the two ends found each
     * other. `a` carries the caller's writes to the server, `b` the
     * server's back. */
    a = pipe64_create();
    if (a < 0) return -23;                             /* -ENFILE */
    b = pipe64_create();
    if (b < 0) { pipe64_unref(a, 0, 0); return -23; }

    /* Referenced now, for both sides, rather than when accept runs. The
     * server's half is owned by the queue until somebody takes it, and
     * a listener closed before then has something concrete to release -
     * see sock64_destroy. Doing it at accept instead would leave the
     * pipes with a zero count while they sat on the queue, and a zero
     * count is how this layer says "finished". */
    pipe64_ref(b, 1, 0);        /* caller reads b  */
    pipe64_ref(a, 0, 1);        /* caller writes a */

    slot = &socks[listener].queue[(socks[listener].head +
                                   socks[listener].count) % SOCK64_BACKLOG];
    slot->rx = a;               /* server reads a  */
    slot->tx = b;               /* server writes b */
    pipe64_ref(a, 1, 0);
    pipe64_ref(b, 0, 1);
    socks[listener].count++;

    socks[s].state = SOCK64_CONNECTED;
    *out_rx = b;
    *out_tx = a;
    return 0;
}

int sock64_accept(int s, int* out_rx, int* out_tx) {
    pending64_t* p;

    if (!sock64_valid(s)) return -9;
    if (socks[s].state != SOCK64_LISTENING) return -22;
    if (socks[s].count == 0) return -11;               /* -EAGAIN */

    p = &socks[s].queue[socks[s].head];
    *out_rx = p->rx;
    *out_tx = p->tx;
    /* The references connect took on the server's behalf move into the
     * new descriptor rather than being taken again. */
    socks[s].head = (socks[s].head + 1) % SOCK64_BACKLOG;
    socks[s].count--;
    accepted++;
    return 0;
}

/* A namespace of its own, beside the pipe, process and vfork keys. A
 * task waiting for a connection and a task waiting for bytes must not
 * wake each other: the first would find the queue still empty and the
 * second would find the pipe still dry, and both would go back to
 * sleep having lost the wake-up the other needed. */
uint64_t sock64_wait_key(int s) {
    return 0x4000000000000000ULL + (uint64_t)s;
}

int sock64_pending(int s) {
    return sock64_valid(s) ? socks[s].count : 0;
}

uint64_t sock64_live(void) {
    uint64_t n = 0;
    for (int i = 0; i < SOCK64_MAX; i++) if (socks[i].used) n++;
    return n;
}

uint64_t sock64_accepted(void) { return accepted; }
