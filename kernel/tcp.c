/* tcp.c - enough of TCP to fetch a file.
 *
 * See include/tcp.h for what is left out and why. What follows is the
 * usual shape: a table of connections, a segment builder, a receive path
 * that is one switch on the state, and a tick that retransmits.
 *
 * The part that is easy to get wrong, and the reason it is written the
 * way it is: **sequence numbers wrap**. Comparing them with `<` is a bug
 * that appears after four gigabytes and never in a test. Every comparison
 * here goes through seq_lt/seq_le, which subtract and look at the sign of
 * the *signed* difference - which is correct across the wrap because the
 * difference is what matters, not the absolute values.
 */

#include "tcp.h"
#include "net.h"
#include "netdev.h"
#include "kstring.h"
#include "pit.h"

typedef struct __attribute__((packed)) {
    uint16_t src_port, dest_port;
    uint32_t seq, ack;
    uint8_t  data_offset;      /* high nibble: header length in dwords */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_header_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* Retransmission: one second, doubling, five attempts. No round-trip
 * estimation - a stack that sends one segment at a time has no samples
 * worth smoothing. */
#define RTO_INITIAL_TICKS 100
#define RTO_MAX_TRIES     5

typedef struct {
    int         in_use;
    tcp_state_t state;

    uint32_t    remote_ip;
    uint16_t    remote_port, local_port;

    uint32_t    snd_next;      /* next sequence number to send */
    uint32_t    snd_unacked;   /* oldest byte not yet acknowledged */
    uint32_t    rcv_next;      /* next sequence number expected */

    /* The one segment in flight, kept so it can be retransmitted. */
    uint8_t     pending[TCP_MSS];
    uint32_t    pending_len;
    uint8_t     pending_flags;
    uint32_t    pending_seq;
    uint32_t    rto_deadline;
    uint32_t    rto_ticks;
    uint32_t    tries;

    uint8_t     recv[TCP_RECV_BUFFER];
    uint32_t    recv_head, recv_tail;

    int         peer_finished;
    int         ack_pending;
    uint32_t    last_advertised;   /* the window in the last ACK we sent */
    uint32_t    time_wait_until;
} tcp_conn_t;

static tcp_conn_t conns[TCP_MAX_CONNECTIONS];
static uint16_t   next_port = 49152;
static uint32_t   bytes_received;
static uint32_t   retransmits;

/* Diagnostics. A transfer that stalls has exactly two possible reasons -
 * the loop that would acknowledge is not running, or it is running and
 * the acknowledgement is not leaving - and these tell them apart. */
static uint32_t   tick_calls, ack_ok, ack_failed;

/* --- sequence arithmetic ------------------------------------------------ */

static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

/* --- the receive ring --------------------------------------------------- */

static uint32_t recv_free(const tcp_conn_t* c) {
    return TCP_RECV_BUFFER - 1 - ((c->recv_head - c->recv_tail) % TCP_RECV_BUFFER);
}

static void recv_push(tcp_conn_t* c, const uint8_t* data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        uint32_t next = (c->recv_head + 1) % TCP_RECV_BUFFER;
        if (next == c->recv_tail) return;         /* full: the window lied */
        c->recv[c->recv_head] = data[i];
        c->recv_head = next;
    }
}

/* --- sending ------------------------------------------------------------ */

/* TCP's checksum covers a "pseudo header" of the IP addresses, the
 * protocol number and the segment length as well as the segment itself.
 * It is the one place this stack has to know about the layer below it,
 * and it is why UDP could get away with a zero checksum and TCP cannot. */
static uint16_t segment_checksum(uint32_t src_ip, uint32_t dest_ip,
                                 const uint8_t* segment, uint32_t len) {
    uint32_t sum = 0;

    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dest_ip >> 16) & 0xFFFF;
    sum += dest_ip & 0xFFFF;
    sum += IP_PROTO_TCP;
    sum += len;

    return net_checksum(segment, len, sum);
}

static int send_segment(tcp_conn_t* c, uint8_t flags, uint32_t seq,
                        const uint8_t* data, uint32_t len) {
    static uint8_t segment[sizeof(tcp_header_t) + TCP_MSS];

    if (len > TCP_MSS) return -1;

    tcp_header_t* th = (tcp_header_t*)segment;
    th->src_port = htons(c->local_port);
    th->dest_port = htons(c->remote_port);
    th->seq = htonl(seq);
    th->ack = htonl(c->rcv_next);
    th->data_offset = (sizeof(tcp_header_t) / 4) << 4;
    th->flags = flags;
    uint32_t window = recv_free(c);
    if (window > 65535) window = 65535;
    th->window = htons((uint16_t)window);
    c->last_advertised = window;
    th->checksum = 0;
    th->urgent = 0;

    if (len) kmemcpy(segment + sizeof(tcp_header_t), data, len);

    uint32_t total = sizeof(tcp_header_t) + len;
    th->checksum = htons(segment_checksum(net_config()->ip, c->remote_ip,
                                          segment, total));

    return net_send_ip(c->remote_ip, IP_PROTO_TCP, segment, total);
}

/* Sends and remembers, so the tick can send it again. */
static int send_tracked(tcp_conn_t* c, uint8_t flags,
                        const uint8_t* data, uint32_t len) {
    c->pending_seq = c->snd_next;
    c->pending_flags = flags;
    c->pending_len = len;
    if (len) kmemcpy(c->pending, data, len);

    int r = send_segment(c, flags, c->pending_seq, data, len);

    /* SYN and FIN each occupy one sequence number, which is what makes
     * them acknowledgeable. */
    c->snd_next += len;
    if (flags & (TCP_SYN | TCP_FIN)) c->snd_next += 1;

    c->rto_ticks = RTO_INITIAL_TICKS;
    c->rto_deadline = pit_get_ticks() + c->rto_ticks;
    c->tries = 0;
    return r;
}

/* Returns whether the acknowledgement actually left the machine. The
 * caller has to know: rtl_send() drops when all four transmit buffers are
 * busy, and net_send_ip() returns short when the next hop's MAC is still
 * being resolved. */
static int send_ack(tcp_conn_t* c) {
    return send_segment(c, TCP_ACK, c->snd_next, 0, 0) == 0;
}

static void send_reset(uint32_t dest_ip, uint16_t dest_port,
                       uint16_t src_port, uint32_t seq) {
    static uint8_t segment[sizeof(tcp_header_t)];
    tcp_header_t* th = (tcp_header_t*)segment;

    kmemset(segment, 0, sizeof(segment));
    th->src_port = htons(src_port);
    th->dest_port = htons(dest_port);
    th->seq = htonl(seq);
    th->data_offset = (sizeof(tcp_header_t) / 4) << 4;
    th->flags = TCP_RST;
    th->checksum = htons(segment_checksum(net_config()->ip, dest_ip,
                                          segment, sizeof(segment)));
    net_send_ip(dest_ip, IP_PROTO_TCP, segment, sizeof(segment));
}

/* --- receiving ---------------------------------------------------------- */

static tcp_conn_t* find_conn(uint32_t remote_ip, uint16_t remote_port,
                             uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &conns[i];
        if (!c->in_use) continue;
        if (c->remote_ip == remote_ip && c->remote_port == remote_port &&
            c->local_port == local_port) {
            return c;
        }
    }
    return 0;
}

static void tcp_receive(uint32_t src_ip, uint32_t dest_ip,
                        const uint8_t* seg, uint32_t len) {
    (void)dest_ip;
    if (len < sizeof(tcp_header_t)) return;

    const tcp_header_t* th = (const tcp_header_t*)seg;
    uint32_t hdr_len = (uint32_t)(th->data_offset >> 4) * 4;
    if (hdr_len < sizeof(tcp_header_t) || hdr_len > len) return;

    /* The checksum is mandatory and is the only thing standing between a
     * corrupted segment and the receive buffer. */
    if (segment_checksum(src_ip, dest_ip, seg, len) != 0) return;

    uint16_t src_port = ntohs(th->src_port);
    uint16_t dest_port = ntohs(th->dest_port);

    tcp_conn_t* c = find_conn(src_ip, src_port, dest_port);
    if (!c) {
        /* Nothing here is listening, so anything unrecognised gets a
         * reset - which is what tells the far end to stop retrying
         * rather than to wait out its own timeout. */
        if (!(th->flags & TCP_RST)) {
            send_reset(src_ip, src_port, dest_port, ntohl(th->ack));
        }
        return;
    }

    uint32_t seq = ntohl(th->seq);
    uint32_t ack = ntohl(th->ack);
    const uint8_t* data = seg + hdr_len;
    uint32_t data_len = len - hdr_len;

    if (th->flags & TCP_RST) {
        c->state = TCP_CLOSED;
        return;
    }

    /* An acknowledgement of what is in flight retires it. */
    if ((th->flags & TCP_ACK) && seq_le(c->snd_unacked, ack) &&
        seq_le(ack, c->snd_next)) {
        c->snd_unacked = ack;
        if (seq_le(c->pending_seq + c->pending_len +
                   ((c->pending_flags & (TCP_SYN | TCP_FIN)) ? 1 : 0), ack)) {
            c->pending_len = 0;
            c->pending_flags = 0;
        }
    }

    switch (c->state) {
        case TCP_SYN_SENT:
            if ((th->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                c->rcv_next = seq + 1;
                c->state = TCP_ESTABLISHED;
                c->ack_pending = 1;
            }
            return;

        case TCP_ESTABLISHED:
        case TCP_FIN_WAIT_1:
        case TCP_FIN_WAIT_2:
            break;

        case TCP_LAST_ACK:
            if (th->flags & TCP_ACK) c->state = TCP_CLOSED;
            return;

        case TCP_TIME_WAIT:
            return;

        default:
            return;
    }

    /* In-order data only. Out of order is dropped rather than held: the
     * sender will retransmit, and a reassembly queue is a lot of code for
     * a stack whose peer is one hop away. */
    if (data_len) {
        /* All of the segment or none of it. Taking a *part* of one looks
         * like progress and is the opposite: rcv_next then lands in the
         * middle of what the sender has already moved past, so every
         * following segment is out of order and dropped until the sender's
         * retransmission timer fires. A download that did that stopped
         * dead at 53KB with the bytes it had all correct - which is what
         * a partial accept looks like from the outside. */
        if (seq == c->rcv_next && data_len <= recv_free(c)) {
            recv_push(c, data, data_len);
            c->rcv_next += data_len;
            bytes_received += data_len;
        }
        c->ack_pending = 1;
    }

    if ((th->flags & TCP_FIN) && seq_le(seq, c->rcv_next)) {
        c->rcv_next += 1;
        c->peer_finished = 1;
        c->ack_pending = 1;

        if (c->state == TCP_ESTABLISHED) {
            c->state = TCP_CLOSE_WAIT;
        } else if (c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2) {
            c->state = TCP_TIME_WAIT;
            /* Two seconds rather than the specified two minutes. This
             * machine is not going to reuse the port pair inside two
             * minutes, and a connection table with slots held that long
             * is four connections that are not available. */
            c->time_wait_until = pit_get_ticks() + 200;
        }
    } else if ((th->flags & TCP_ACK) && c->state == TCP_FIN_WAIT_1 &&
               seq_le(c->snd_next, ack)) {
        c->state = TCP_FIN_WAIT_2;
    }
}

/* --- the public interface ----------------------------------------------- */

void tcp_init(void) {
    void net_register_tcp(void (*fn)(uint32_t, uint32_t, const uint8_t*, uint32_t));
    net_register_tcp(tcp_receive);
}

int tcp_connect(uint32_t dest_ip, uint16_t dest_port) {
    if (!net_config()->configured) return -1;

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &conns[i];
        if (c->in_use) continue;

        kmemset(c, 0, sizeof(*c));
        c->in_use = 1;
        c->state = TCP_SYN_SENT;
        c->remote_ip = dest_ip;
        c->remote_port = dest_port;
        c->local_port = next_port++;
        if (next_port < 49152) next_port = 49152;

        /* An initial sequence number that is not zero. The tick count is
         * not the clock-driven ISN the RFC asks for, and the reason that
         * matters - an old duplicate segment being accepted into a new
         * connection - needs the same port pair inside a minute, which
         * next_port makes unlikely on its own. */
        c->snd_next = 0x4E560000u + pit_get_ticks() * 64;
        c->snd_unacked = c->snd_next;

        if (send_tracked(c, TCP_SYN, 0, 0) < 0) {
            /* No route yet: ARP is resolving. The tick retransmits. */
        }
        return i;
    }
    return -2;
}

static tcp_conn_t* handle_conn(int handle) {
    if (handle < 0 || handle >= TCP_MAX_CONNECTIONS) return 0;
    return conns[handle].in_use ? &conns[handle] : 0;
}

tcp_state_t tcp_state(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    return c ? c->state : TCP_CLOSED;
}

int tcp_connect_wait(uint32_t dest_ip, uint16_t dest_port, uint32_t ticks) {
    int h = tcp_connect(dest_ip, dest_port);
    if (h < 0) return h;

    uint32_t deadline = pit_get_ticks() + ticks;
    while (pit_get_ticks() < deadline) {
        net_poll();
        tcp_state_t s = tcp_state(h);
        if (s == TCP_ESTABLISHED) return h;
        if (s == TCP_CLOSED) { tcp_close(h); return -3; }
        __asm__ __volatile__("hlt");
    }
    tcp_close(h);
    return -4;
}

int tcp_send(int handle, const uint8_t* data, uint32_t len) {
    tcp_conn_t* c = handle_conn(handle);
    if (!c || c->state != TCP_ESTABLISHED) return -1;
    if (c->pending_len || c->pending_flags) return 0;   /* one in flight */

    uint32_t take = len < TCP_MSS ? len : TCP_MSS;
    if (send_tracked(c, TCP_ACK | TCP_PSH, data, take) < 0) return 0;
    return (int)take;
}

int tcp_send_all(int handle, const uint8_t* data, uint32_t len, uint32_t ticks) {
    uint32_t sent = 0;
    uint32_t deadline = pit_get_ticks() + ticks;

    while (sent < len && pit_get_ticks() < deadline) {
        int n = tcp_send(handle, data + sent, len - sent);
        if (n < 0) return n;
        if (n == 0) {
            net_poll();
            __asm__ __volatile__("hlt");
            continue;
        }
        sent += (uint32_t)n;
    }
    return (int)sent;
}

int tcp_recv(int handle, uint8_t* out, uint32_t max) {
    tcp_conn_t* c = handle_conn(handle);
    if (!c) return -1;

    uint32_t n = 0;
    while (n < max && c->recv_tail != c->recv_head) {
        out[n++] = c->recv[c->recv_tail];
        c->recv_tail = (c->recv_tail + 1) % TCP_RECV_BUFFER;
    }

    /* Draining the ring opens the window, and the sender has no way to
     * find that out unless it is told. This is the window update, and
     * leaving it out is what made a 2MB download take six minutes.
     *
     * The failure is worth spelling out because it looks like slowness
     * rather than a bug. The sender fills the window and stops. The
     * reader empties the ring - but an ACK was only ever sent in response
     * to *data*, and no data is arriving, because the sender is waiting
     * for the window this ACK would have opened. Neither side is broken
     * and neither side speaks. What breaks the tie is the sender's
     * persist timer, about a second and a half later, and the whole
     * transfer proceeds one window per persist timeout: 8KB per 1.5s,
     * which is the 5KB/s that was measured.
     *
     * A window update per MSS freed, rather than per byte, because an ACK
     * for every byte read would be its own kind of silly. */
    if (n && recv_free(c) >= c->last_advertised + TCP_MSS) {
        c->ack_pending = 1;
    }
    return (int)n;
}

int tcp_eof(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    if (!c) return 1;
    return c->peer_finished && c->recv_tail == c->recv_head;
}

void tcp_close(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    if (!c) return;

    if (c->state == TCP_ESTABLISHED) {
        send_tracked(c, TCP_ACK | TCP_FIN, 0, 0);
        c->state = TCP_FIN_WAIT_1;
    } else if (c->state == TCP_CLOSE_WAIT) {
        send_tracked(c, TCP_ACK | TCP_FIN, 0, 0);
        c->state = TCP_LAST_ACK;
    } else {
        c->in_use = 0;
        c->state = TCP_CLOSED;
    }
}

void tcp_tick(void) {
    uint32_t now = pit_get_ticks();
    tick_calls++;

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &conns[i];
        if (!c->in_use) continue;

        if (c->state == TCP_CLOSED ||
            (c->state == TCP_TIME_WAIT && now >= c->time_wait_until)) {
            c->in_use = 0;
            continue;
        }

        /* Acknowledgements are sent from here rather than from the
         * interrupt handler that noticed the data.
         *
         * This is the fix for a 200KB download that stopped at 53KB. An
         * ACK is a frame, the card has four transmit buffers, and
         * rtl_send() drops rather than waits (see the note there) - so
         * acknowledging from inside the receive interrupt meant the
         * acknowledgement for a burst of segments was the one thing
         * competing with itself for a buffer, and the ones that lost were
         * silently dropped. The sender then waited on a window that had
         * been opened by an ACK nobody sent.
         *
         * Out here the transmitter has had time to drain, and one ACK per
         * poll covers however many segments arrived since the last one -
         * which is what a delayed ACK is for anyway. */
        /* Cleared only if the acknowledgement was really sent.
         *
         * Clearing it unconditionally is what held a 2MB download to
         * 8KB/s. A dropped ACK is not an error - the transmitter was
         * busy, which is normal and which TCP is built for - but it is
         * only harmless if it is *sent again*. Forgetting it instead left
         * the sender waiting on a window nobody had opened, once per
         * dropped ACK, each one costing a persist timeout. The transfer
         * then runs at one window per stall rather than at the speed of
         * the link. */
        /* Cleared *before* the send, not after.
         *
         * This is the bug that held every download to one round-trip per
         * retransmission timeout, and it is a race with the interrupt
         * handler rather than anything wrong with TCP. The flag is set by
         * the receive path, which runs in the interrupt; it is cleared
         * here, which does not. Clearing it after send_ack() returns
         * therefore erases a request that arrived *during* the send - the
         * segments that landed while the acknowledgement for the ones
         * before them was being built. Nobody asks again, because asking
         * is what the flag was for. The sender waits for an
         * acknowledgement that will never come, times out, and
         * retransmits; that retransmission sets the flag again, and the
         * whole window is acknowledged at once - which is why the traces
         * showed an acknowledgement arriving in the same millisecond as
         * every retransmission, and nothing at all in between.
         *
         * Clearing first cannot lose one. A segment that arrives after
         * the clear sets the flag again and is acknowledged next tick; a
         * segment that arrives after that, during send_ack() itself, is
         * covered by the acknowledgement being sent *and* leaves the flag
         * set, which costs one redundant acknowledgement and no data. */
        if (c->ack_pending) {
            c->ack_pending = 0;
            if (send_ack(c)) {
                ack_ok++;
            } else {
                /* Not sent - the transmitter was busy or the next hop is
                 * still being resolved. Ask again. */
                c->ack_pending = 1;
                ack_failed++;
            }
        }

        if (!c->pending_len && !c->pending_flags) continue;
        if (now < c->rto_deadline) continue;

        if (++c->tries > RTO_MAX_TRIES) {
            c->state = TCP_CLOSED;
            continue;
        }

        retransmits++;
        send_segment(c, c->pending_flags, c->pending_seq,
                     c->pending_len ? c->pending : 0, c->pending_len);
        c->rto_ticks *= 2;
        c->rto_deadline = now + c->rto_ticks;
    }
}

uint32_t tcp_bytes_received(void) { return bytes_received; }
uint32_t tcp_retransmits(void) { return retransmits; }
uint32_t tcp_tick_calls(void) { return tick_calls; }
uint32_t tcp_acks_sent(void) { return ack_ok; }
uint32_t tcp_acks_failed(void) { return ack_failed; }
