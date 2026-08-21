#ifndef SOCK64_H
#define SOCK64_H

#include <stdint.h>

/* Unix-domain sockets, which is to say: a rendezvous.
 *
 * Milestone 74 built the connection - two pipes crossed - and that is
 * all a socketpair is, because a socketpair's two ends are handed to
 * somebody who already has both. What the wineserver needs is the other
 * arrangement: a name in the filesystem that processes which have never
 * met can find, a queue of callers waiting at it, and an accept that
 * manufactures a fresh crossed pair per caller.
 *
 * So there is nothing here about moving bytes. Once a connection exists
 * it *is* a pair of pipes and the descriptor layer already carries it;
 * this is only the part that gets two strangers holding opposite ends of
 * the same pair.
 */

#define SOCK64_MAX      32
#define SOCK64_BACKLOG  8

/* What a socket is at the moment. Linux does not expose these as a state
 * machine but every error it returns is about one: bind on a bound
 * socket is -EINVAL, listen on a connected one is -EINVAL, accept on one
 * that is not listening is -EINVAL, and read on one that is not
 * connected is -ENOTCONN. */
#define SOCK64_UNBOUND    0
#define SOCK64_BOUND      1
#define SOCK64_LISTENING  2
#define SOCK64_CONNECTED  3

void sock64_init(void);

/* A new socket, or -1 if the table is full. AF_UNIX/SOCK_STREAM only -
 * see the note in sock64.c about why a datagram socket is refused
 * rather than approximated. */
int  sock64_create(void);
void sock64_destroy(int s);
int  sock64_valid(int s);

int  sock64_state(int s);

/* bind(2): the socket takes a name, which is a node in the filesystem.
 * The node is the identity - two paths that resolve to it are the same
 * socket - so connect looks up a path and then asks who is bound to the
 * node, rather than comparing strings. */
int  sock64_bind(int s, int node);
int  sock64_listen(int s, int backlog);

/* Who is listening on this node, or -1. */
int  sock64_listener_for_node(int node);

/* connect(2): makes the crossed pair and leaves the far side on the
 * listener's queue. Fills in the caller's own two pipe ends. Returns 0,
 * -ECONNREFUSED if nobody is listening, or -EAGAIN if the backlog is
 * full. Both ends are referenced before this returns, so a listener
 * that is closed before accepting still gives them back. */
int  sock64_connect(int s, int listener, int* out_rx, int* out_tx);

/* accept(2): takes the oldest pending connection off the queue and
 * hands over its two pipe ends, already referenced. -EAGAIN when the
 * queue is empty, which the caller turns into a wait. */
int  sock64_accept(int s, int* out_rx, int* out_tx);

/* The address a task blocks on waiting for a connection to arrive. */
uint64_t sock64_wait_key(int s);

/* How many callers are waiting to be accepted. poll(2) asks: a
 * listening socket is "readable" exactly when this is not zero. */
int  sock64_pending(int s);

uint64_t sock64_live(void);
uint64_t sock64_accepted(void);

#endif
