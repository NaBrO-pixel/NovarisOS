#ifndef PIPE64_H
#define PIPE64_H

#include <stdint.h>

/* Pipes: a byte stream between two descriptors.
 *
 * This exists because the wineserver could not start without it - it
 * makes a pipe for its own shutdown signalling before it does anything
 * else - but the thing it is really for is the one after that. Every
 * Wine process talks to the server over a socketpair, and a socketpair
 * is two of these crossed: what makes it bidirectional is not a
 * different object but a second pipe pointing the other way.
 *
 * So a descriptor does not hold "a pipe". It holds a direction: which
 * pipe it may read from and which it may write to, either of which may
 * be absent. A read end of a pipe(2) is read-only, a write end is
 * write-only, and a socketpair endpoint is both - and the same read()
 * and write() serve all three.
 *
 * The buffer is a ring in the kernel heap. 64KB is Linux's default pipe
 * capacity and the number matters: the wineserver writes a request and
 * waits for a reply, so a capacity smaller than a request would deadlock
 * a protocol that works everywhere else.
 */

#define PIPE64_MAX 64
#define PIPE64_BUF 65536

void pipe64_init(void);

/* A new pipe with no ends attached yet, or -1 if the table or the heap
 * is full. The caller attaches ends with pipe64_ref. */
int  pipe64_create(void);

/* Ends are counted, not owned: fork duplicates a descriptor and both
 * copies are real, so what decides whether a pipe is finished is how
 * many readers and writers are left rather than who created it. */
void pipe64_ref(int p, int reader, int writer);
void pipe64_unref(int p, int reader, int writer);

/* read: 0 at end of file, which for a pipe means "empty and no writer
 * left". -EAGAIN if there is a writer but nothing to read yet and the
 * caller asked not to block; the blocking case is the caller's, because
 * only the syscall layer can park a task.
 *
 * write: -EPIPE when every reader has gone, which is what makes a
 * server notice that its client died. A short write is a real answer -
 * the ring is finite - and callers must handle it. */
int64_t pipe64_read(int p, void* buf, uint64_t n);
int64_t pipe64_write(int p, const void* buf, uint64_t n);

uint64_t pipe64_available(int p);   /* bytes ready to read           */
uint64_t pipe64_space(int p);       /* bytes that would fit          */
int      pipe64_readers(int p);
int      pipe64_writers(int p);
int      pipe64_valid(int p);

/* Live pipes, for the assertions: a layer that leaks one descriptor per
 * connection is not visible any other way. */
uint64_t pipe64_live(void);

#endif
