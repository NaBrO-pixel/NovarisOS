#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/* tcp - enough of TCP to fetch a file.
 *
 * Milestone 39. Client side only: this kernel connects out, it does not
 * listen. That halves the state machine (no LISTEN, no SYN_RECEIVED, no
 * simultaneous open) and removes the half that is only useful to a
 * machine other machines can reach, which this is not.
 *
 * Three simplifications, all deliberate and all visible in throughput
 * rather than correctness:
 *
 *   - **One segment in flight.** Send, wait for the ACK, send the next.
 *     A real stack keeps a congestion window full. For the traffic here -
 *     a few hundred bytes of HTTP request, then megabytes *inbound* - the
 *     slow direction is the one nobody uses.
 *   - **No window scaling, no SACK, no timestamps.** The window is the
 *     receive ring - 64KB, which is the largest a header without window
 *     scaling can advertise - and it is advertised honestly.
 *   - **Retransmit is a timeout and a resend**, with no round-trip
 *     estimation. One second, doubling, five tries.
 *
 * What is *not* simplified: the checksum (TCP's is mandatory, unlike
 * UDP's), sequence arithmetic (wrapping compares, done with signed
 * differences), and out-of-order data (dropped rather than mis-assembled,
 * and the sender retransmits).
 */

#define TCP_MAX_CONNECTIONS 4
#define TCP_RECV_BUFFER     65536
#define TCP_MSS             1460

typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
} tcp_state_t;

/* Opens a connection. Returns a handle >= 0, or negative. Does not wait -
 * the handshake happens across net_poll() calls, and tcp_state() says
 * where it has got to. */
int tcp_connect(uint32_t dest_ip, uint16_t dest_port);

/* Connects and waits for the handshake, up to `ticks`. Returns a handle
 * or negative. */
int tcp_connect_wait(uint32_t dest_ip, uint16_t dest_port, uint32_t ticks);

tcp_state_t tcp_state(int handle);

/* The ephemeral port this end was given. getsockname() needs it, and it
 * is chosen down here rather than by the caller. */
uint16_t tcp_local_port(int handle);

/* Queues data. Returns how much was taken - which is 0 when a segment is
 * already in flight, so the caller polls and tries again. */
int tcp_send(int handle, const uint8_t* data, uint32_t len);
/* Sends all of it, polling until it fits or `ticks` elapse. */
int tcp_send_all(int handle, const uint8_t* data, uint32_t len, uint32_t ticks);

/* Copies out what has arrived. Returns bytes copied, 0 if none yet. */
int tcp_recv(int handle, uint8_t* out, uint32_t max);

/* How many bytes are waiting, without taking any. What poll() needs:
 * asking by reading would consume the thing being asked about. */
uint32_t tcp_recv_ready(int handle);

/* Whether the peer has finished sending and everything it sent has been
 * read. */
int tcp_eof(int handle);

void tcp_close(int handle);

/* --- accepting ------------------------------------------------------------
 *
 * A passive open. tcp_listen() claims a local port; a SYN for it creates
 * a connection in TCP_SYN_RECEIVED and answers SYN+ACK, and the third
 * segment of the handshake moves it to ESTABLISHED. tcp_accept() then
 * hands out the handle.
 *
 * There is no separate backlog: a half-open connection occupies an
 * ordinary slot in the connection table, so the backlog is however many
 * of the table's slots are free. That is the honest shape for a table of
 * four, and a listen() whose backlog argument promises more than the
 * machine has would be a lie told in a signature. */
int tcp_listen(uint16_t port);
void tcp_unlisten(uint16_t port);

/* A connection that finished its handshake on `port`, or -1. */
int tcp_accept(uint16_t port);

/* Whether tcp_accept would return one, without taking it. */
int tcp_accept_ready(uint16_t port);

/* Who is at the other end. For a connection that arrived rather than one
 * that was dialled, the caller did not choose these and has no other way
 * to learn them. */
uint32_t tcp_peer_ip(int handle);
uint16_t tcp_peer_port(int handle);

/* Called by net_poll(): retransmits, and finishes closing. */
void tcp_tick(void);

/* Wires tcp_input into kernel/net.c. Called once by net_init(). */
void tcp_init(void);

uint32_t tcp_bytes_received(void);
uint32_t tcp_retransmits(void);

/* Diagnostics: how many times the tick ran, how many acknowledgements it
 * managed to send, and how many it could not. */
uint32_t tcp_tick_calls(void);
uint32_t tcp_acks_sent(void);
uint32_t tcp_acks_failed(void);

#endif /* TCP_H */
