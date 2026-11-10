/* pipe64.c - a byte stream between two descriptors. */

#include "pipe64.h"
#include "serial64.h"
#include "kheap64.h"
#include "kstring.h"
#include "ramfs64.h"
#include "proc64.h"
#include "inet64.h"
#include "spin64.h"
#include "epoll64.h"

typedef struct {
    fdpass64_t fds[8];
    int        n;
} batch64_t;

typedef struct {
    uint8_t* buf;
    uint64_t head;      /* next byte to read  */
    uint64_t tail;      /* next byte to write */
    uint64_t len;       /* bytes in the ring  */
    int      readers;
    int      writers;

    /* Descriptors in flight, oldest first. */
    batch64_t inflight[PIPE64_INFLIGHT];
    int       in_head, in_count;

    int      used;

    /* The epoll entries watching this pipe (epoll64.c), told of every
     * change to it. A few at most: one per epoll descriptor watching a
     * descriptor of this pipe. */
    struct { int ep, fd; } watch[4];
    int      nwatch;
} pipe64_t;

static pipe64_t pipes[PIPE64_MAX];

/* Called with the pipes' lock held, by everything that changes what a
 * poll of this pipe would answer. */
static void notify(int p) {
    for (int k = 0; k < pipes[p].nwatch; k++)
        epoll64_mark(pipes[p].watch[k].ep, pipes[p].watch[k].fd);
}

static void drop_inflight(const fdpass64_t* f);

void pipe64_init(void) {
    /* Sockets hold pipes; they go with them. */
    inet64_reset();
    for (int i = 0; i < PIPE64_MAX; i++) {
        /* The buffer is deliberately not freed here. pipe64_init runs at
         * the top of every layer in kmain64, which resets the world; the
         * heap is reset with it, so freeing would be a double free of
         * memory that no longer exists. */
        pipes[i].buf     = 0;
        pipes[i].head    = 0;
        pipes[i].tail    = 0;
        pipes[i].len     = 0;
        pipes[i].readers = 0;
        pipes[i].writers = 0;
        pipes[i].in_head = 0;
        pipes[i].in_count = 0;
        pipes[i].nwatch  = 0;
        pipes[i].used    = 0;
    }
    epoll64_reset();
}

int pipe64_valid(int p) {
    return p >= 0 && p < PIPE64_MAX && pipes[p].used;
}

static int create_nl(void) {
    int i;

    for (i = 0; i < PIPE64_MAX; i++) if (!pipes[i].used) break;
    if (i == PIPE64_MAX) {
        /* Said once, because a ceiling that is reached is reached
         * thousands of times and the first one is the news.
         *
         * A ceiling this kernel runs into has, every time so far,
         * presented as something several layers away that names
         * anything but the ceiling. Saying so here costs one line and
         * turns the next one into an hour rather than a day. */
        static int said;
        if (!said) {
            said = 1;
            serial64_puts("NOVARIS64: [pipe] all ");
            serial64_putdec(PIPE64_MAX);
            serial64_puts(" pipes are in use - pipe2 and socketpair now"
                          " return -ENFILE\n");
        }
        return -1;
    }

    pipes[i].buf = (uint8_t*)kmalloc64(PIPE64_BUF);
    if (!pipes[i].buf) return -1;

    pipes[i].head    = 0;
    pipes[i].tail    = 0;
    pipes[i].len     = 0;
    pipes[i].readers = 0;
    pipes[i].writers = 0;
    pipes[i].in_head = 0;
    pipes[i].in_count = 0;
    pipes[i].nwatch  = 0;
    pipes[i].used    = 1;
    return i;
}

static void ref_nl(int p, int reader, int writer) {
    if (!pipe64_valid(p)) return;
    if (reader) pipes[p].readers++;
    if (writer) pipes[p].writers++;
    notify(p);
}

static void unref_nl(int p, int reader, int writer) {
    if (!pipe64_valid(p)) return;
    if (reader && pipes[p].readers > 0) pipes[p].readers--;
    if (writer && pipes[p].writers > 0) pipes[p].writers--;
    notify(p);

    /* Nobody left on either side. The buffer goes back to the heap and
     * the slot is reusable - a server that accepts connections forever
     * would otherwise run out of pipes rather than out of memory, and
     * the two failures look nothing alike from outside. */
    if (pipes[p].readers == 0 && pipes[p].writers == 0) {
        /* Descriptors that were sent and never received. Their pipes were
         * referenced when they went in flight, on behalf of a receiver
         * that is now not coming, so they are given back here. Miss this
         * and a message dropped on a closing connection leaks a pipe per
         * message - invisible until a long-running server stops being
         * able to open anything. */
        while (pipes[p].in_count > 0) {
            batch64_t* b = &pipes[p].inflight[pipes[p].in_head];
            for (int k = 0; k < b->n; k++) drop_inflight(&b->fds[k]);
            pipes[p].in_head = (pipes[p].in_head + 1) % PIPE64_INFLIGHT;
            pipes[p].in_count--;
        }
        kfree64(pipes[p].buf);
        pipes[p].buf  = 0;
        pipes[p].used = 0;
        pipes[p].len  = 0;
    }
}

/* Give back a descriptor that was sent and never received.
 *
 * It knows which kind because fdpass64_t says so, and it has to: a file
 * in flight is holding the node open, and a node whose last opener
 * disappeared without saying so is a file that never goes away. */
static void drop_inflight(const fdpass64_t* f) {
    if (f->kind == FD64_FILE) ramfs64_unref_node(f->node);
    else {
        pipe64_unref(f->rx, 1, 0);
        pipe64_unref(f->tx, 0, 1);
    }
    if (f->kind == FD64_INET) inet64_unref(f->sock);
}

static int send_fds_nl(int p, const fdpass64_t* in, int n) {
    batch64_t* b;

    if (!pipe64_valid(p) || n <= 0) return 0;
    if (n > (int)(sizeof(b->fds) / sizeof(b->fds[0]))) return 0;
    if (pipes[p].in_count >= PIPE64_INFLIGHT) return 0;

    b = &pipes[p].inflight[(pipes[p].in_head + pipes[p].in_count)
                           % PIPE64_INFLIGHT];
    for (int k = 0; k < n; k++) b->fds[k] = in[k];
    b->n = n;
    pipes[p].in_count++;
    return 1;
}

static int recv_fds_nl(int p, fdpass64_t* out, int max) {
    batch64_t* b;
    int n;

    if (!pipe64_valid(p) || pipes[p].in_count == 0) return 0;

    b = &pipes[p].inflight[pipes[p].in_head];
    n = b->n < max ? b->n : max;
    for (int k = 0; k < n; k++) out[k] = b->fds[k];

    /* Anything the receiver had no room for is dropped, and dropped
     * properly: Linux closes the surplus rather than leaving it in
     * flight, because the sender has already given it away. */
    for (int k = n; k < b->n; k++) drop_inflight(&b->fds[k]);

    pipes[p].in_head = (pipes[p].in_head + 1) % PIPE64_INFLIGHT;
    pipes[p].in_count--;
    return n;
}

int pipe64_inflight(int p) {
    return pipe64_valid(p) ? pipes[p].in_count : 0;
}

uint64_t pipe64_available(int p) {
    return pipe64_valid(p) ? pipes[p].len : 0;
}

uint64_t pipe64_space(int p) {
    return pipe64_valid(p) ? PIPE64_BUF - pipes[p].len : 0;
}

int pipe64_readers(int p) { return pipe64_valid(p) ? pipes[p].readers : 0; }
int pipe64_writers(int p) { return pipe64_valid(p) ? pipes[p].writers : 0; }

static int64_t read_nl(int p, void* buf, uint64_t n) {
    pipe64_t* q;
    uint8_t*  out = (uint8_t*)buf;
    uint64_t  got = 0;

    if (!pipe64_valid(p)) return -9;                   /* -EBADF */
    q = &pipes[p];

    if (q->len == 0) {
        /* Empty. With a writer still attached this is "not yet" and the
         * caller decides whether to wait; with none it is end of file,
         * which read(2) reports as 0 and every reader already handles. */
        if (q->writers == 0) return 0;
        return -11;                                    /* -EAGAIN */
    }

    if (n > q->len) n = q->len;
    while (got < n) {
        uint64_t run = PIPE64_BUF - q->head;           /* to the wrap */
        if (run > n - got) run = n - got;
        kmemcpy(out + got, q->buf + q->head, run);
        q->head = (q->head + run) % PIPE64_BUF;
        got += run;
    }
    q->len -= got;
    notify(p);
    return (int64_t)got;
}

/* pipe64_read without taking anything: MSG_PEEK on a connected socket.
 * Chromium asks whether a connection is still alive by peeking a byte,
 * and a peek that consumed would hand it the first byte of the reply. */
static int64_t peek_nl(int p, void* buf, uint64_t n) {
    pipe64_t* q;
    uint8_t*  out = (uint8_t*)buf;
    uint64_t  got = 0, at;

    if (!pipe64_valid(p)) return -9;
    q = &pipes[p];
    if (q->len == 0) return q->writers == 0 ? 0 : -11;

    if (n > q->len) n = q->len;
    at = q->head;
    while (got < n) {
        uint64_t run = PIPE64_BUF - at;
        if (run > n - got) run = n - got;
        kmemcpy(out + got, q->buf + at, run);
        at = (at + run) % PIPE64_BUF;
        got += run;
    }
    return (int64_t)got;
}

static int64_t write_nl(int p, const void* buf, uint64_t n) {
    pipe64_t*      q;
    const uint8_t* in = (const uint8_t*)buf;
    uint64_t       put = 0, room;

    if (!pipe64_valid(p)) return -9;                   /* -EBADF */
    q = &pipes[p];

    /* Nobody is going to read this. Linux also raises SIGPIPE, which
     * kills the writer by default; returning the error without the
     * signal is the safer half to implement first, and it is the half
     * every server checks for - Wine sets SIGPIPE to SIG_IGN precisely
     * so that it sees EPIPE instead of dying. */
    if (q->readers == 0) return -32;                   /* -EPIPE */

    room = PIPE64_BUF - q->len;
    if (room == 0) return -11;                         /* -EAGAIN */
    if (n > room) n = room;                            /* a short write */

    while (put < n) {
        uint64_t run = PIPE64_BUF - q->tail;
        if (run > n - put) run = n - put;
        kmemcpy(q->buf + q->tail, in + put, run);
        q->tail = (q->tail + run) % PIPE64_BUF;
        put += run;
    }
    q->len += put;
    notify(p);
    return (int64_t)put;
}

uint64_t pipe64_live(void) {
    uint64_t n = 0;
    for (int i = 0; i < PIPE64_MAX; i++) if (pipes[i].used) n++;
    return n;
}

/* --- the pipes' lock -------------------------------------------------------
 *
 * One for every pipe (spin64.h): reads and writes of pipes and sockets
 * run without the kernel lock now (syscall64.c), and the two ends of a
 * pipe are usually two processes on two CPUs. The sections are short - a
 * copy of at most a ring's worth - so one lock for all of them costs less
 * than the bookkeeping of one each would. */
static spin64_t pipes_lock = SPIN64_INIT;

int pipe64_create(void) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    int r = create_nl();
    spin64_release(&pipes_lock, fl);
    return r;
}

void pipe64_ref(int p, int reader, int writer) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    ref_nl(p, reader, writer);
    spin64_release(&pipes_lock, fl);
}

void pipe64_unref(int p, int reader, int writer) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    unref_nl(p, reader, writer);
    spin64_release(&pipes_lock, fl);
}

int pipe64_send_fds(int p, const fdpass64_t* in, int n) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    int r = send_fds_nl(p, in, n);
    spin64_release(&pipes_lock, fl);
    return r;
}

int pipe64_recv_fds(int p, fdpass64_t* out, int max) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    int r = recv_fds_nl(p, out, max);
    spin64_release(&pipes_lock, fl);
    return r;
}

int64_t pipe64_read(int p, void* buf, uint64_t n) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    int64_t r = read_nl(p, buf, n);
    spin64_release(&pipes_lock, fl);
    return r;
}

int64_t pipe64_peek(int p, void* buf, uint64_t n) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    int64_t r = peek_nl(p, buf, n);
    spin64_release(&pipes_lock, fl);
    return r;
}

int64_t pipe64_write(int p, const void* buf, uint64_t n) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    int64_t r = write_nl(p, buf, n);
    spin64_release(&pipes_lock, fl);
    return r;
}


/* All of `n` bytes, or none: for a writer that runs without the kernel
 * lock and cannot sleep, so must not leave half a request behind. The
 * pieces are the caller's iovec, already checked to be mapped. -EAGAIN if
 * the ring has less room than that; -EPIPE with no reader. */
int64_t pipe64_write_all(int p, const void* const* bufs, const uint64_t* lens,
                         int cnt, uint64_t n) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    int64_t r;
    if (!pipe64_valid(p)) r = -9;
    else if (pipes[p].readers == 0) r = -32;
    else if (PIPE64_BUF - pipes[p].len < n) r = -11;
    else {
        for (int i = 0; i < cnt; i++)
            if (lens[i]) (void)write_nl(p, bufs[i], lens[i]);
        r = (int64_t)n;
    }
    spin64_release(&pipes_lock, fl);
    return r;
}

/* --- epoll's watchers, and its use of the lock --------------------------- */

int pipe64_watch(int p, int ep, int fd) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    int ok = 0;
    if (pipe64_valid(p) && pipes[p].nwatch < 4) {
        pipes[p].watch[pipes[p].nwatch].ep = ep;
        pipes[p].watch[pipes[p].nwatch].fd = fd;
        pipes[p].nwatch++;
        ok = 1;
    }
    spin64_release(&pipes_lock, fl);
    return ok;
}

void pipe64_unwatch(int p, int ep, int fd) {
    uint64_t fl = spin64_acquire(&pipes_lock);
    if (p >= 0 && p < PIPE64_MAX) {
        for (int k = 0; k < pipes[p].nwatch; k++) {
            if (pipes[p].watch[k].ep != ep || pipes[p].watch[k].fd != fd) continue;
            pipes[p].watch[k] = pipes[p].watch[--pipes[p].nwatch];
            break;
        }
    }
    spin64_release(&pipes_lock, fl);
}

uint64_t pipe64_lock(void)          { return spin64_acquire(&pipes_lock); }
void     pipe64_unlock(uint64_t fl) { spin64_release(&pipes_lock, fl); }
