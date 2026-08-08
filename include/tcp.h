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
    TCP_SYN_SENT,
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

/* Queues data. Returns how much was taken - which is 0 when a segment is
 * already in flight, so the caller polls and tries again. */
int tcp_send(int handle, const uint8_t* data, uint32_t len);
/* Sends all of it, polling until it fits or `ticks` elapse. */
int tcp_send_all(int handle, const uint8_t* data, uint32_t len, uint32_t ticks);

/* Copies out what has arrived. Returns bytes copied, 0 if none yet. */
int tcp_recv(int handle, uint8_t* out, uint32_t max);

/* Whether the peer has finished sending and everything it sent has been
 * read. */
int tcp_eof(int handle);

void tcp_close(int handle);

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
