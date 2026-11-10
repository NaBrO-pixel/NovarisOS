#ifndef EPOLL64_H
#define EPOLL64_H

#include <stdint.h>

/* epoll64 - epoll(7), for the wineserver (Milestone 104). See epoll64.c. */

#define EPOLL64_MAX      8       /* instances, machine-wide */
#define EPOLL64_FDS      2048    /* = PROC64_FD_MAX: entries are by fd */

#define EPOLL64_CTL_ADD  1
#define EPOLL64_CTL_DEL  2
#define EPOLL64_CTL_MOD  3

#define EPOLL64_ONESHOT  (1u << 30)

/* What epoll_wait hands back: Linux's struct epoll_event, which is packed
 * on x86-64 - 12 bytes, the data straight after the mask. */
typedef struct {
    uint32_t events;
    uint64_t data;
} __attribute__((packed)) epoll64_event_t;

/* How ready `fd` of the instance's owner is, for `want` (poll(2) bits).
 * syscall64.c's, shared with poll. */
typedef short (*epoll64_ready_fn)(int fd, short want, void* ctx);

void epoll64_reset(void);
int  epoll64_create(int owner_pid);              /* the instance, or -1 */
void epoll64_ref(int ep);
void epoll64_unref(int ep);
int  epoll64_owner(int ep);

/* `rx` and `tx` are the fd's pipes, -1 for none, and `watchable` whether
 * every change to its readiness is a change to those pipes - true for a
 * pipe or a connected Unix socket. Anything else is looked at on every
 * wait. Returns 0 or -errno. */
int  epoll64_ctl(int ep, int op, int fd, uint32_t events, uint64_t data,
                 int rx, int tx, int watchable);

/* Ready entries, at most `max`, into `out`. */
int  epoll64_collect(int ep, epoll64_ready_fn ready, void* ctx,
                     epoll64_event_t* out, int max);

/* `fd` of process `pid` has been closed: out of every instance it owns. */
void epoll64_forget_fd(int pid, int fd);

/* From pipe64.c, under the pipes' lock: something about a pipe that
 * entry `fd` of instance `ep` watches has changed. */
void epoll64_mark(int ep, int fd);
void epoll64_stats(uint64_t* w, uint64_t* d, uint64_t* a, uint64_t* r, uint64_t* o);

#endif
