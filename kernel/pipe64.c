/* pipe64.c - a byte stream between two descriptors. */

#include "pipe64.h"
#include "kheap64.h"
#include "kstring.h"
#include "ramfs64.h"
#include "proc64.h"

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
} pipe64_t;

static pipe64_t pipes[PIPE64_MAX];

static void drop_inflight(const fdpass64_t* f);

void pipe64_init(void) {
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
        pipes[i].used    = 0;
    }
}

int pipe64_valid(int p) {
    return p >= 0 && p < PIPE64_MAX && pipes[p].used;
}

int pipe64_create(void) {
    int i;

    for (i = 0; i < PIPE64_MAX; i++) if (!pipes[i].used) break;
    if (i == PIPE64_MAX) return -1;

    pipes[i].buf = (uint8_t*)kmalloc64(PIPE64_BUF);
    if (!pipes[i].buf) return -1;

    pipes[i].head    = 0;
    pipes[i].tail    = 0;
    pipes[i].len     = 0;
    pipes[i].readers = 0;
    pipes[i].writers = 0;
    pipes[i].in_head = 0;
    pipes[i].in_count = 0;
    pipes[i].used    = 1;
    return i;
}

void pipe64_ref(int p, int reader, int writer) {
    if (!pipe64_valid(p)) return;
    if (reader) pipes[p].readers++;
    if (writer) pipes[p].writers++;
}

void pipe64_unref(int p, int reader, int writer) {
    if (!pipe64_valid(p)) return;
    if (reader && pipes[p].readers > 0) pipes[p].readers--;
    if (writer && pipes[p].writers > 0) pipes[p].writers--;

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
}

int pipe64_send_fds(int p, const fdpass64_t* in, int n) {
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

int pipe64_recv_fds(int p, fdpass64_t* out, int max) {
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

int64_t pipe64_read(int p, void* buf, uint64_t n) {
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
    return (int64_t)got;
}

int64_t pipe64_write(int p, const void* buf, uint64_t n) {
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
    return (int64_t)put;
}

uint64_t pipe64_live(void) {
    uint64_t n = 0;
    for (int i = 0; i < PIPE64_MAX; i++) if (pipes[i].used) n++;
    return n;
}
