/* epoll64.c - epoll(7), for the wineserver.
 *
 * The wineserver's main loop waits for its clients with epoll when it
 * can, and with poll when epoll_create fails - which it always did here.
 * poll hands the kernel every client descriptor on every pass, a few
 * hundred of them, and each is looked at whether anything happened to it
 * or not: 63,000 polls at 178,000 cycles each in one Chromium run, three
 * quarters of everything the wineserver did under the kernel lock, on the
 * CPU the browser shares with it (Milestone 104).
 *
 * epoll is the same question asked once. The descriptors are registered
 * with epoll_ctl, and a wait looks only at the ones something may have
 * happened to. For a pipe, or a Unix socket made of pipes - which is what
 * every client connection is - pipe64.c knows exactly when: each pipe
 * keeps the entries watching it and marks them dirty whenever its bytes,
 * readers or writers change. A wait then re-examines the dirty entries,
 * the ones that were ready last time (it is level-triggered: ready stays
 * reported until it stops being true), and the few kinds of descriptor
 * whose readiness this cannot follow, which it asks about every time.
 *
 * Entries are indexed by descriptor number, so finding one is free. The
 * instance belongs to the process that made it; its descriptor numbers are
 * that process's. Everything here but epoll64_mark runs under the kernel
 * lock; the dirty queue is the pipes' lock's (spin64.h), since pipes
 * change without the kernel lock now. */

#include "epoll64.h"
#include "pipe64.h"
#include "kstring.h"

typedef struct {
    uint32_t events;        /* asked for, poll(2) bits plus ONESHOT */
    uint64_t data;
    int      rx, tx;        /* the pipes it watches, -1 for none */
    uint8_t  used;
    uint8_t  always;        /* not watchable: looked at on every wait */
    uint8_t  dirty;         /* in the dirty queue (pipes' lock) */
    uint8_t  inready;       /* in the ready list */
} entry_t;

typedef struct {
    int     used, refs, owner;
    entry_t e[EPOLL64_FDS];
    int     dirty[EPOLL64_FDS], ndirty;      /* the pipes' lock's */
    int     ready[EPOLL64_FDS], nready;
    int     always[EPOLL64_FDS], nalways;
} instance_t;

static instance_t eps[EPOLL64_MAX];

/* What waits looked at, for the report: how many waits, and how many
 * entries in each of the three lists over all of them. */
static uint64_t st_waits, st_dirty, st_always, st_ready, st_out;

void epoll64_stats(uint64_t* w, uint64_t* d, uint64_t* a, uint64_t* r, uint64_t* o) {
    *w = st_waits; *d = st_dirty; *a = st_always; *r = st_ready; *o = st_out;
    st_waits = st_dirty = st_always = st_ready = st_out = 0;
}

static int valid(int ep) { return ep >= 0 && ep < EPOLL64_MAX && eps[ep].used; }

void epoll64_reset(void) {
    for (int i = 0; i < EPOLL64_MAX; i++) eps[i].used = 0;
}

int epoll64_create(int owner_pid) {
    for (int i = 0; i < EPOLL64_MAX; i++) {
        if (eps[i].used) continue;
        kmemset(&eps[i], 0, sizeof(eps[i]));
        eps[i].used  = 1;
        eps[i].refs  = 1;
        eps[i].owner = owner_pid;
        return i;
    }
    return -1;
}

int epoll64_owner(int ep) { return valid(ep) ? eps[ep].owner : -1; }

static void unwatch(int ep, int fd) {
    entry_t* e = &eps[ep].e[fd];
    if (e->rx >= 0) pipe64_unwatch(e->rx, ep, fd);
    if (e->tx >= 0 && e->tx != e->rx) pipe64_unwatch(e->tx, ep, fd);
    e->rx = e->tx = -1;
}

static void list_drop(int* list, int* n, int fd) {
    for (int i = 0; i < *n; i++)
        if (list[i] == fd) { list[i] = list[--*n]; return; }
}

static void remove_entry(int ep, int fd) {
    instance_t* in = &eps[ep];
    entry_t* e = &in->e[fd];
    if (!e->used) return;
    unwatch(ep, fd);
    if (e->inready) list_drop(in->ready, &in->nready, fd);
    if (e->always)  list_drop(in->always, &in->nalways, fd);
    /* A dirty mark left in the queue is skipped when drained: unused. */
    e->used = 0;
    e->inready = 0;
    e->always = 0;
}

void epoll64_ref(int ep) { if (valid(ep)) eps[ep].refs++; }

void epoll64_unref(int ep) {
    if (!valid(ep)) return;
    if (--eps[ep].refs > 0) return;
    for (int fd = 0; fd < EPOLL64_FDS; fd++)
        if (eps[ep].e[fd].used) remove_entry(ep, fd);
    eps[ep].used = 0;
}

/* Watched by its pipes, or else on the always list. Marked dirty either
 * way, so the next wait looks at it once. */
static void attach(int ep, int fd, int rx, int tx, int watchable) {
    instance_t* in = &eps[ep];
    entry_t* e = &in->e[fd];
    int ok = watchable;
    e->rx = e->tx = -1;
    if (ok && rx >= 0) { if (pipe64_watch(rx, ep, fd)) e->rx = rx; else ok = 0; }
    if (ok && tx >= 0 && tx != rx) {
        if (pipe64_watch(tx, ep, fd)) e->tx = tx; else ok = 0;
    }
    if (!ok) {
        unwatch(ep, fd);
        e->always = 1;
        in->always[in->nalways++] = fd;
    }
    {
        uint64_t fl = pipe64_lock();
        epoll64_mark(ep, fd);
        pipe64_unlock(fl);
    }
}

int epoll64_ctl(int ep, int op, int fd, uint32_t events, uint64_t data,
                int rx, int tx, int watchable) {
    entry_t* e;
    if (!valid(ep)) return -9;                          /* -EBADF */
    if (fd < 0 || fd >= EPOLL64_FDS) return -9;
    e = &eps[ep].e[fd];

    switch (op) {
    case EPOLL64_CTL_ADD:
        if (e->used) return -17;                        /* -EEXIST */
        e->used = 1;
        e->inready = 0;
        e->always = 0;
        e->events = events;
        e->data = data;
        attach(ep, fd, rx, tx, watchable);
        return 0;
    case EPOLL64_CTL_MOD:
        if (!e->used) return -2;                        /* -ENOENT */
        e->events = events;
        e->data = data;
        /* What it watches may have changed under the same number - a
         * socket that has since connected - so it is attached afresh. */
        unwatch(ep, fd);
        if (e->always) { list_drop(eps[ep].always, &eps[ep].nalways, fd); e->always = 0; }
        attach(ep, fd, rx, tx, watchable);
        return 0;
    case EPOLL64_CTL_DEL:
        if (!e->used) return -2;
        remove_entry(ep, fd);
        return 0;
    }
    return -22;                                         /* -EINVAL */
}

void epoll64_forget_fd(int pid, int fd) {
    if (fd < 0 || fd >= EPOLL64_FDS) return;
    for (int ep = 0; ep < EPOLL64_MAX; ep++)
        if (eps[ep].used && eps[ep].owner == pid && eps[ep].e[fd].used)
            remove_entry(ep, fd);
}

void epoll64_mark(int ep, int fd) {
    instance_t* in;
    if (!valid(ep) || fd < 0 || fd >= EPOLL64_FDS) return;
    in = &eps[ep];
    if (in->e[fd].dirty) return;
    in->e[fd].dirty = 1;
    in->dirty[in->ndirty++] = fd;
}

/* What the entry reports now: the asked-for bits that hold, plus error
 * and hang-up, which are reported whether asked for or not. */
static short evaluate(entry_t* e, int fd, epoll64_ready_fn ready, void* ctx) {
    short want = (short)(e->events & 0xFFFF);
    return (short)(ready(fd, want, ctx) & (want | 0x008 | 0x010));
}

static void note(instance_t* in, int fd, short got) {
    entry_t* e = &in->e[fd];
    if (got && !e->inready) { e->inready = 1; in->ready[in->nready++] = fd; }
}

int epoll64_collect(int ep, epoll64_ready_fn ready, void* ctx,
                    epoll64_event_t* out, int max) {
    static int drained[EPOLL64_FDS];
    instance_t* in;
    int nd, n = 0;

    if (!valid(ep) || max <= 0) return 0;
    in = &eps[ep];

    /* The dirty queue, taken whole: pipes go on changing meanwhile, and
     * what changes after this goes in the next one. */
    {
        uint64_t fl = pipe64_lock();
        nd = in->ndirty;
        for (int i = 0; i < nd; i++) {
            drained[i] = in->dirty[i];
            in->e[drained[i]].dirty = 0;
        }
        in->ndirty = 0;
        pipe64_unlock(fl);
    }
    st_waits++;
    st_dirty += (uint64_t)nd;
    st_always += (uint64_t)in->nalways;
    for (int i = 0; i < nd; i++) {
        int fd = drained[i];
        if (in->e[fd].used) note(in, fd, evaluate(&in->e[fd], fd, ready, ctx));
    }
    for (int i = 0; i < in->nalways; i++) {
        int fd = in->always[i];
        note(in, fd, evaluate(&in->e[fd], fd, ready, ctx));
    }

    /* Everything that was ready, asked again - ready stays reported only
     * while it is true - and reported, up to `max`. Those beyond `max`
     * stay in the list for the next wait. */
    st_ready += (uint64_t)in->nready;
    for (int i = 0; i < in->nready; ) {
        int fd = in->ready[i];
        entry_t* e = &in->e[fd];
        short got = e->used ? evaluate(e, fd, ready, ctx) : 0;
        if (!got) {
            e->inready = 0;
            in->ready[i] = in->ready[--in->nready];
            continue;
        }
        if (n < max) {
            out[n].events = (uint32_t)(uint16_t)got;
            out[n].data   = e->data;
            n++;
            /* One report, then nothing until it is re-armed with MOD. */
            if (e->events & EPOLL64_ONESHOT) {
                e->events &= ~0xFFFFu;
                e->inready = 0;
                in->ready[i] = in->ready[--in->nready];
                continue;
            }
        }
        i++;
    }
    st_out += (uint64_t)n;
    return n;
}
