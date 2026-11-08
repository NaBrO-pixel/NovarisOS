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

/* Out-of-order segments held per connection, waiting for a gap. */
#ifndef TCP_OOO_SLOTS
#define TCP_OOO_SLOTS 4
#endif

typedef struct {
    int         in_use;
    tcp_state_t state;

    uint32_t    remote_ip;
    uint16_t    remote_port, local_port;

    uint32_t    snd_next;      /* next sequence number to send */
    /* The highest sequence number ever sent. A timeout moves snd_next
     * back to resend; an ACK may still cover far more than that - the
     * receiver kept what arrived after the gap - and it is valid up to
     * here, not just up to snd_next. */
    uint32_t    snd_max;
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

    /* The send side (Milestone 97). Everything the caller handed over
     * and the peer has not yet acknowledged, from snd_unacked on: the
     * byte at sequence number s is sndbuf[(snd_head + s - snd_unacked)
     * % TCP_SEND_BUFFER]. What is in flight is snd_next - snd_unacked;
     * the rest has not been sent yet. tcp_output sends while the peer's
     * window - snd_wnd, which this stack used to ignore - has room, so a
     * window's worth is in flight at once rather than one segment.
     *
     * SYN and FIN keep the one-segment slot above (pending_*): they are
     * sent only while no data is outstanding, so the two never mix. */
    uint8_t     sndbuf[TCP_SEND_BUFFER];
    uint32_t    snd_head;
    uint32_t    snd_len;
    uint32_t    snd_wnd;
    uint32_t    data_rto_deadline;  /* 0: no data timer running */
    uint32_t    data_rto_ticks;
    uint32_t    data_tries;
    int         fin_queued;         /* tcp_close with data still queued */
    int         output_pending;     /* the receive path saw room open */

    /* Congestion control (Milestone 98), Reno's shape. cwnd is how much
     * this side lets itself have in flight whatever the peer's window
     * says: it starts at a few segments and doubles each round trip
     * (slow start) until ssthresh, then grows a segment per round trip.
     * A timeout means the network lost things - cwnd back to one segment,
     * ssthresh to half what was in flight. Three duplicate ACKs mean one
     * segment was lost and the rest got through: resend just that one
     * (fast retransmit) and halve rather than collapse. */
    uint32_t    cwnd, ssthresh;
    uint32_t    dupacks;
    uint32_t    recover;            /* snd_next when fast recovery began */
    int         in_recovery;

    /* Reassembly (Milestone 98): segments that arrived ahead of a gap,
     * kept until the gap is filled instead of dropped - so one lost
     * segment costs one retransmission rather than everything after it. */
    struct {
        int      used;
        uint32_t seq, len;
        uint8_t  data[TCP_MSS];
    } ooo[TCP_OOO_SLOTS];
    uint32_t    ooo_fin_seq;        /* a FIN seen ahead of the data before it */
    uint32_t    dupacks_owed;       /* one per segment that arrived past a gap */
    int         ooo_fin;

    int         peer_finished;
    int         ack_pending;
    /* Passive opens only: set once tcp_accept() has handed this out, so
     * the same connection is not accepted twice. */
    int         accepted;
    uint32_t    last_advertised;   /* the window in the last ACK we sent */
    uint32_t    time_wait_until;
} tcp_conn_t;

static tcp_conn_t conns[TCP_MAX_CONNECTIONS];
static uint16_t   next_port = 49152;

/* Ports with something listening on them. A flat array rather than a
 * structure with a queue, because the queue is the connection table -
 * see the note in tcp.h. */
#ifndef TCP_MAX_LISTEN
#define TCP_MAX_LISTEN 4
#endif
static uint16_t   listening[TCP_MAX_LISTEN];
static uint32_t   bytes_received;
static uint32_t   retransmits;

/* Diagnostics. A transfer that stalls has exactly two possible reasons -
 * the loop that would acknowledge is not running, or it is running and
 * the acknowledgement is not leaving - and these tell them apart. */
static uint32_t   tick_calls, ack_ok, ack_failed;

/* --- sequence arithmetic ------------------------------------------------ */

static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static uint32_t ooo_held, fast_retransmits;
static uint32_t closed_by_reset, closed_by_retries, closed_by_data_retries;

static void tcp_output(tcp_conn_t* c);
static void retransmit_first(tcp_conn_t* c);

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
    th->checksum = htons(segment_checksum(net_source_ip(c->remote_ip), c->remote_ip,
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
    if (seq_lt(c->snd_max, c->snd_next)) c->snd_max = c->snd_next;

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
    th->checksum = htons(segment_checksum(net_source_ip(dest_ip), dest_ip,
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

static int is_listening(uint16_t port) {
    for (int i = 0; i < TCP_MAX_LISTEN; i++) {
        if (listening[i] == port) return 1;
    }
    return 0;
}

/* A SYN arrived for a listening port: take a slot, answer SYN+ACK, and
 * leave it half-open until the client's ACK completes it. */
static tcp_conn_t* accept_syn(uint32_t src_ip, uint16_t src_port,
                              uint16_t dest_port, uint32_t their_seq) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &conns[i];
        if (c->in_use) continue;

        kmemset(c, 0, sizeof(*c));
        c->in_use = 1;
        c->state = TCP_SYN_RECEIVED;
        c->remote_ip = src_ip;
        c->remote_port = src_port;
        c->local_port = dest_port;
        c->rcv_next = their_seq + 1;          /* the SYN takes one */
        c->snd_wnd = TCP_MSS;     /* until the handshake's ACK says */
        c->snd_next = 0x4E560000u + pit_get_ticks() * 64;
        c->snd_unacked = c->snd_next;
        c->snd_max = c->snd_next;

        send_tracked(c, TCP_SYN | TCP_ACK, 0, 0);
        return c;
    }
    return 0;
}

/* --- reassembly --------------------------------------------------------- */

static void ooo_store(tcp_conn_t* c, uint32_t seq, const uint8_t* data,
                      uint32_t len) {
    int free_slot = -1;
    for (int i = 0; i < TCP_OOO_SLOTS; i++) {
        if (!c->ooo[i].used) { if (free_slot < 0) free_slot = i; continue; }
        if (c->ooo[i].seq == seq) return;              /* have it already */
    }
    if (free_slot < 0) return;     /* full: dropped, and resent later */
    c->ooo[free_slot].used = 1;
    c->ooo[free_slot].seq = seq;
    c->ooo[free_slot].len = len;
    kmemcpy(c->ooo[free_slot].data, data, len);
    ooo_held++;
}

/* Moves into the receive ring whatever the last in-order segment made
 * contiguous - all of a held segment that starts at rcv_next, or the
 * tail of one that straddles it - and forgets what is now behind. */
static void ooo_drain(tcp_conn_t* c) {
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < TCP_OOO_SLOTS; i++) {
            uint32_t end, skip, take;
            if (!c->ooo[i].used) continue;
            end = c->ooo[i].seq + c->ooo[i].len;
            if (seq_le(end, c->rcv_next)) { c->ooo[i].used = 0; continue; }
            if (seq_lt(c->rcv_next, c->ooo[i].seq)) continue;   /* still a gap */
            skip = c->rcv_next - c->ooo[i].seq;
            take = c->ooo[i].len - skip;
            if (take > recv_free(c)) continue;          /* no room yet */
            recv_push(c, c->ooo[i].data + skip, take);
            c->rcv_next += take;
            bytes_received += take;
            c->ooo[i].used = 0;
            progress = 1;
        }
    }
}

/* --- congestion control ------------------------------------------------- */

static uint32_t flight(const tcp_conn_t* c) { return c->snd_next - c->snd_unacked; }

static void cc_init(tcp_conn_t* c) {
    c->cwnd = 4 * TCP_MSS;              /* RFC 3390's initial window */
    c->ssthresh = 0xFFFFFFFFu;
    c->dupacks = 0;
    c->in_recovery = 0;
}

/* Every ACK: `data` bytes newly acknowledged, `seg_len` the bytes the
 * segment itself carried, `wnd` the window it advertised. */
static void cc_on_ack(tcp_conn_t* c, uint32_t ack, uint32_t data,
                      uint32_t seg_len, uint32_t wnd) {
    if (!c->cwnd) cc_init(c);

    if (!data) {
        /* A duplicate: nothing new acknowledged, no data of its own, the
         * window unchanged, and something still in flight. */
        if (ack == c->snd_unacked && !seg_len && wnd == c->snd_wnd &&
            flight(c) && c->snd_len) {
            if (++c->dupacks == 3 && !c->in_recovery) {
                /* Fast retransmit: the segment at snd_unacked is the one
                 * missing. Resend it alone, now. */
                uint32_t half = flight(c) / 2;
                c->ssthresh = half > 2 * TCP_MSS ? half : 2 * TCP_MSS;
                c->cwnd = c->ssthresh + 3 * TCP_MSS;
                c->recover = c->snd_next;
                c->in_recovery = 1;
                retransmit_first(c);
                fast_retransmits++;
            } else if (c->in_recovery) {
                c->cwnd += TCP_MSS;     /* each one is a segment that left */
            }
        }
        return;
    }

    c->dupacks = 0;
    if (c->in_recovery) {
        if (seq_le(c->recover, ack)) {          /* all of it recovered */
            c->in_recovery = 0;
            c->cwnd = c->ssthresh;
        } else {
            retransmit_first(c);                /* the next hole */
        }
        return;
    }
    if (c->cwnd < c->ssthresh) {
        c->cwnd += data < TCP_MSS ? data : TCP_MSS;     /* slow start */
    } else {
        uint32_t inc = TCP_MSS * TCP_MSS / c->cwnd;      /* avoidance */
        c->cwnd += inc ? inc : 1;
    }
    if (c->cwnd > TCP_SEND_BUFFER) c->cwnd = TCP_SEND_BUFFER;
}

static void cc_on_timeout(tcp_conn_t* c) {
    uint32_t half = flight(c) / 2;
    if (!c->cwnd) cc_init(c);
    c->ssthresh = half > 2 * TCP_MSS ? half : 2 * TCP_MSS;
    c->cwnd = TCP_MSS;
    c->dupacks = 0;
    c->in_recovery = 0;
}

/* The oldest unacknowledged segment, sent again without moving snd_next. */
static void retransmit_first(tcp_conn_t* c) {
    static uint8_t seg[TCP_MSS];
    uint32_t n = flight(c) < c->snd_len ? flight(c) : c->snd_len;
    if (n > TCP_MSS) n = TCP_MSS;
    if (!n) return;
    for (uint32_t i = 0; i < n; i++)
        seg[i] = c->sndbuf[(c->snd_head + i) % TCP_SEND_BUFFER];
    send_segment(c, TCP_ACK | TCP_PSH, c->snd_unacked, seg, n);
    retransmits++;
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
        /* A SYN for a port something is listening on is a connection
         * being opened *to* this machine, which is the one case where
         * an unrecognised segment is not an error. */
        if ((th->flags & (TCP_SYN | TCP_ACK)) == TCP_SYN &&
            is_listening(dest_port)) {
            c = accept_syn(src_ip, src_port, dest_port, ntohl(th->seq));
            if (c) return;      /* SYN+ACK sent; the rest is the tick's */
            /* No slot free. Reset rather than drop: the client learns
             * now instead of retrying into a table that is still full. */
        }

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
        closed_by_reset++;
        c->state = TCP_CLOSED;
        return;
    }

    /* An acknowledgement of what is in flight retires it. */
    if ((th->flags & TCP_ACK) && seq_le(c->snd_unacked, ack) &&
        seq_le(ack, c->snd_max)) {
        /* Acknowledged past where a go-back left snd_next: what it covers
         * does not need sending again. */
        if (seq_lt(c->snd_next, ack)) c->snd_next = ack;
        /* Data first: what of the send ring this covers. SYN and FIN are
         * only ever outstanding with the ring empty, so a delta beyond
         * the ring's length is theirs. */
        uint32_t delta = ack - c->snd_unacked;
        uint32_t data = delta < c->snd_len ? delta : c->snd_len;
        cc_on_ack(c, ack, data, data_len, ntohs(th->window));
        if (data) {
            c->snd_head = (c->snd_head + data) % TCP_SEND_BUFFER;
            c->snd_len -= data;
            c->data_tries = 0;
            c->data_rto_ticks = RTO_INITIAL_TICKS;
            /* Progress restarts the timer; nothing left in flight stops it. */
            c->data_rto_deadline = (ack != c->snd_next && c->snd_len)
                ? pit_get_ticks() + c->data_rto_ticks : 0;
        }
        c->snd_wnd = ntohs(th->window);
        c->output_pending = 1;
        c->snd_unacked = ack;
        if (seq_le(c->pending_seq + c->pending_len +
                   ((c->pending_flags & (TCP_SYN | TCP_FIN)) ? 1 : 0), ack)) {
            c->pending_len = 0;
            c->pending_flags = 0;
        }
    }

    switch (c->state) {
        case TCP_SYN_RECEIVED:
            /* The third segment of the handshake. Anything else here -
             * a retransmitted SYN, say - is left for the tick to answer
             * by resending the SYN+ACK it still has pending. */
            if (th->flags & TCP_ACK) {
                c->state = TCP_ESTABLISHED;
                c->pending_len = 0;
                c->pending_flags = 0;
            }
            return;

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
            ooo_drain(c);
        } else if (seq_lt(c->rcv_next, seq) && data_len <= TCP_MSS &&
                   seq_lt(seq - c->rcv_next, recv_free(c))) {
            /* Ahead of a gap and inside the window: keep it. */
            ooo_store(c, seq, data, data_len);
            /* One ACK for each of these, not one per tick: three
             * duplicates are what a sender takes as "one segment lost,
             * resend it now" rather than waiting out its timer. */
            if (c->dupacks_owed < 8) c->dupacks_owed++;
            if (th->flags & TCP_FIN) {
                c->ooo_fin = 1;
                c->ooo_fin_seq = seq + data_len;
            }
        }
        /* Acknowledged at once, in order or not: a duplicate ACK is what
         * tells the sender which segment is missing. */
        c->ack_pending = 1;
    }

    /* The FIN counts only once everything before it has arrived - its
     * own segment's data included, or the data a gap held back. It used
     * to count whenever its segment did not start ahead, so a FIN whose
     * data was refused for want of room ended the stream short, and a
     * retransmitted FIN was counted twice. A FIN already counted is
     * acknowledged again: that is what its sender is asking for. */
    int fin_now = 0;
    if ((th->flags & TCP_FIN) && seq + data_len == c->rcv_next) fin_now = 1;
    if (c->ooo_fin && c->ooo_fin_seq == c->rcv_next) fin_now = 1;
    if ((th->flags & TCP_FIN) && seq + data_len + 1 == c->rcv_next &&
        c->peer_finished)
        c->ack_pending = 1;
    if (fin_now && !c->peer_finished) {
        c->ooo_fin = 0;
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

/* --- sending what the ring holds ---------------------------------------- */

/* Sends from snd_next while the peer's window has room. Stops, without
 * moving snd_next, at the first segment that could not leave - the
 * transmitter busy, ARP still asking - and the next tick tries again. */
static void tcp_output(tcp_conn_t* c) {
    static uint8_t seg[TCP_MSS];
    int sent = 0;

    c->output_pending = 0;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) return;

    for (;;) {
        uint32_t in_flight = c->snd_next - c->snd_unacked;
        uint32_t wnd = c->snd_wnd < TCP_SEND_BUFFER ? c->snd_wnd : TCP_SEND_BUFFER;
        if (c->cwnd && wnd > c->cwnd) wnd = c->cwnd;
        uint32_t avail, room, n;

        if (in_flight >= c->snd_len) break;            /* all of it is out */
        avail = c->snd_len - in_flight;
        room = wnd > in_flight ? wnd - in_flight : 0;
        if (!room) break;                              /* the window is full */
        n = avail < room ? avail : room;
        if (n > TCP_MSS) n = TCP_MSS;
        for (uint32_t i = 0; i < n; i++)
            seg[i] = c->sndbuf[(c->snd_head + in_flight + i) % TCP_SEND_BUFFER];
        if (send_segment(c, TCP_ACK | TCP_PSH, c->snd_next, seg, n) != 0) break;
        c->snd_next += n;
        if (seq_lt(c->snd_max, c->snd_next)) c->snd_max = c->snd_next;
        c->ack_pending = 0;            /* every segment carries the ACK */
        sent = 1;
    }
    if (sent && !c->data_rto_deadline) {
        if (!c->data_rto_ticks) c->data_rto_ticks = RTO_INITIAL_TICKS;
        c->data_rto_deadline = pit_get_ticks() + c->data_rto_ticks;
    }
}

/* The FIN, once everything before it has been acknowledged. */
static void send_fin(tcp_conn_t* c) {
    c->fin_queued = 0;
    if (c->state == TCP_ESTABLISHED) {
        send_tracked(c, TCP_ACK | TCP_FIN, 0, 0);
        c->state = TCP_FIN_WAIT_1;
    } else if (c->state == TCP_CLOSE_WAIT) {
        send_tracked(c, TCP_ACK | TCP_FIN, 0, 0);
        c->state = TCP_LAST_ACK;
    }
}

/* --- the public interface ----------------------------------------------- */

void tcp_init(void) {
    void net_register_tcp(void (*fn)(uint32_t, uint32_t, const uint8_t*, uint32_t));
    net_register_tcp(tcp_receive);
}

int tcp_connect(uint32_t dest_ip, uint16_t dest_port) {
    if (!net_config()->configured && !net_is_local(dest_ip)) return -1;

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
        c->snd_max = c->snd_next;
        c->snd_wnd = TCP_MSS;     /* until the SYN+ACK says */

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
    /* CLOSE_WAIT too: the peer has finished sending, not receiving. */
    if (!c || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT))
        return -1;
    if (c->pending_flags) return 0;     /* a SYN or FIN still in flight */

    if (c->fin_queued) return -1;                       /* closed for sending */

    /* Into the send ring, as much as fits; tcp_output decides what of it
     * the window lets go now. Taken means taken: the ring keeps it until
     * the peer acknowledges it, and the timers resend it if that takes
     * too long - so a caller never offers the same bytes twice. */
    uint32_t space = TCP_SEND_BUFFER - c->snd_len;
    uint32_t take = len < space ? len : space;
    for (uint32_t i = 0; i < take; i++)
        c->sndbuf[(c->snd_head + c->snd_len + i) % TCP_SEND_BUFFER] = data[i];
    c->snd_len += take;
    tcp_output(c);
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

int tcp_tx_idle(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    return c && !c->pending_len && !c->pending_flags && !c->snd_len &&
           !c->fin_queued;
}

void tcp_close(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    if (!c) return;

    /* Data still to go: the FIN follows it, from the tick, once the peer
     * has acknowledged the last byte. Sent now, it would take the
     * retransmission slot and the data would never be resent. */
    if ((c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) &&
        (c->snd_len || c->pending_flags)) {
        c->fin_queued = 1;
        return;
    }

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
        /* The duplicates owed for segments past a gap. They are
         * identical, and that is the point. */
        while (c->dupacks_owed) {
            if (!send_ack(c)) break;
            c->dupacks_owed--;
            c->ack_pending = 0;
        }

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

        /* The data timer. Expired with data in flight: go back to the
         * oldest unacknowledged byte and send from there (go-back-N -
         * the receiver keeps only in-order data, so anything after a
         * lost segment was dropped anyway). Expired with nothing in
         * flight and the window shut: a one-byte probe, which is how a
         * sender learns the window has reopened if the update was lost. */
        if (c->snd_len && c->data_rto_deadline && now >= c->data_rto_deadline) {
            if (++c->data_tries > RTO_MAX_TRIES) {
                closed_by_data_retries++;
                c->state = TCP_CLOSED;
                continue;
            }
            retransmits++;
            cc_on_timeout(c);
            c->snd_next = c->snd_unacked;
            c->data_rto_ticks *= 2;
            c->data_rto_deadline = 0;
            if (!c->snd_wnd) {
                c->snd_wnd = 1;
                tcp_output(c);
                c->snd_wnd = 0;
            }
            c->output_pending = 1;
        }
        if (c->snd_len && !c->snd_wnd && !c->data_rto_deadline &&
            c->snd_next == c->snd_unacked) {
            if (!c->data_rto_ticks) c->data_rto_ticks = RTO_INITIAL_TICKS;
            c->data_rto_deadline = now + c->data_rto_ticks;    /* persist */
        }
        if (c->output_pending || (c->snd_len && c->snd_next - c->snd_unacked < c->snd_len))
            tcp_output(c);
        if (c->fin_queued && !c->snd_len && !c->pending_flags) send_fin(c);

        if (!c->pending_len && !c->pending_flags) continue;
        if (now < c->rto_deadline) continue;

        if (++c->tries > RTO_MAX_TRIES) {
            closed_by_retries++;
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

int tcp_listen(uint16_t port) {
    if (!port) return -1;
    for (int i = 0; i < TCP_MAX_LISTEN; i++) {
        if (listening[i] == port) return 0;        /* already */
    }
    for (int i = 0; i < TCP_MAX_LISTEN; i++) {
        if (!listening[i]) { listening[i] = port; return 0; }
    }
    return -1;
}

void tcp_unlisten(uint16_t port) {
    for (int i = 0; i < TCP_MAX_LISTEN; i++) {
        if (listening[i] == port) listening[i] = 0;
    }
}

int tcp_accept(uint16_t port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &conns[i];
        if (!c->in_use || c->accepted) continue;
        if (c->local_port != port) continue;
        /* ESTABLISHED only: a half-open connection is not a connection
         * yet, and handing one to accept() would give the caller a
         * socket whose first read could never succeed. */
        if (c->state != TCP_ESTABLISHED) continue;
        c->accepted = 1;
        return i;
    }
    return -1;
}

int tcp_accept_ready(uint16_t port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &conns[i];
        if (c->in_use && !c->accepted && c->local_port == port &&
            c->state == TCP_ESTABLISHED) {
            return 1;
        }
    }
    return 0;
}

uint32_t tcp_recv_ready(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    if (!c) return 0;
    return (c->recv_head - c->recv_tail) % TCP_RECV_BUFFER;
}

uint32_t tcp_peer_ip(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    return c ? c->remote_ip : 0;
}

uint16_t tcp_peer_port(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    return c ? c->remote_port : 0;
}

uint16_t tcp_local_port(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    return c ? c->local_port : 0;
}

uint32_t tcp_bytes_received(void) { return bytes_received; }
uint32_t tcp_retransmits(void) { return retransmits; }
uint32_t tcp_tick_calls(void) { return tick_calls; }
uint32_t tcp_acks_sent(void) { return ack_ok; }
uint32_t tcp_acks_failed(void) { return ack_failed; }

uint32_t tcp_send_space(int handle) {
    tcp_conn_t* c = handle_conn(handle);
    if (!c || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) ||
        c->fin_queued || c->pending_flags)
        return 0;
    return TCP_SEND_BUFFER - c->snd_len;
}

uint32_t tcp_ooo_held(void) { return ooo_held; }
uint32_t tcp_fast_retransmits(void) { return fast_retransmits; }

/* Why connections ended badly: a reset from the peer, or the SYN/FIN or
 * the data retransmitted RTO_MAX_TRIES times without an answer. */
void tcp_bad_closes(uint32_t* reset, uint32_t* control, uint32_t* data) {
    *reset = closed_by_reset; *control = closed_by_retries;
    *data = closed_by_data_retries;
}
