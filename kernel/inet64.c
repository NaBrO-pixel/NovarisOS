/* inet64.c - AF_INET sockets for the 64-bit kernel. See include/inet64.h.
 *
 * The shape of a connected TCP socket:
 *
 *     process  --write-->  [tx pipe]  --pump-->  tcp_send   --> wire
 *     process  <--read---  [rx pipe]  <--pump--  tcp_recv   <-- wire
 *
 * The socket record holds the far end of each pipe: it is the one writer
 * of rx and the one reader of tx. That is what makes the end of a
 * connection fall out of the pipe rules the descriptor layer already
 * has. When the peer's FIN has been read out of TCP the pump drops its
 * writer reference on rx, and the process's next read returns 0. When
 * the last descriptor is closed the process's writer references on tx
 * are gone, and once the pump has sent what was left in it, it sends the
 * FIN. A peer that resets the connection loses its reader on tx, so a
 * write gets -EPIPE.
 *
 * The pump runs from the timer, once a millisecond, and from every
 * system call that touches one of these sockets. It is bounded per call,
 * so a fast peer cannot hold the timer interrupt. */

#include <stdint.h>
#include "inet64.h"
#include "net.h"
#include "netdev.h"
#include "tcp.h"
#include "dhcp.h"
#include "rtl8139.h"
#include "pipe64.h"
#include "sched64.h"
#include "clock64.h"
#include "serial64.h"
#include "kstring.h"

#define PIPE64_WAIT_KEY_(p) (0x3000000000000000ULL + (uint64_t)(p))

/* What net.c, tcp.c and dhcp.c call the clock. They were written for the
 * 32-bit kernel's 100Hz PIT, and every timeout in them is in its ticks. */
uint32_t pit_get_ticks(void) { return (uint32_t)(clock64_ticks() / 10); }

enum {
    S_FREE = 0,
    S_NEW,          /* made, maybe bound                            */
    S_LISTEN,
    S_CONNECTING,   /* SYN sent, waiting for the answer             */
    S_CONNECTED,    /* has a TCP handle and its pipes               */
    S_DONE,         /* the TCP handle is gone; the pipes may linger */
};

typedef struct {
    int      state;
    int      type;
    int      refs;          /* descriptors that name it */
    int      err;           /* SO_ERROR, as a positive errno */

    uint32_t lip;           /* bound address, 0 for any */
    uint16_t lport;         /* bound port, 0 for not yet */
    uint32_t rip;           /* the peer: TCP's, or UDP's default */
    uint16_t rport;

    int      tcp;           /* tcp.c's handle, -1 for none */
    int      rx, tx;        /* the pipes, -1 for none */
    int      rx_open;       /* still the writer of rx */
    int      tx_open;       /* still the reader of tx */
    int      shut_wr;       /* shutdown(SHUT_WR): FIN once tx is empty */
    int      fin_sent;
    uint64_t linger_until;  /* closed and waiting for the peer: give up then */
    int      udp_bound;     /* holds net.c's port for lport */
} isock_t;

static isock_t socks[INET64_MAX];
static int      card;           /* a card was found */
static int      ready;          /* net_init has run */
static uint16_t next_ephemeral = 32768;
static uint64_t last_arp_refresh;
static uint64_t events;

#ifndef UDP_PORTS
#define UDP_PORTS 8                 /* net.c's default, if not overridden */
#endif

/* A table that fills is said once, with how many of each kind are open,
 * because what the program sees - ENFILE, ENOBUFS - surfaces far away:
 * Wine could not map ENFILE and Chromium reported a page that failed to
 * load with error 10014. */
static void full_once(const char* what, int size) {
    static int said;
    int by_state[6] = { 0 };
    if (said) return;
    said = 1;
    for (int i = 0; i < INET64_MAX; i++) by_state[socks[i].state]++;
    serial64_puts("NOVARIS64: [inet] all ");
    serial64_putdec((uint64_t)size);
    serial64_puts(" ");
    serial64_puts(what);
    serial64_puts(" in use - sockets new ");
    serial64_putdec((uint64_t)by_state[S_NEW]);
    serial64_puts(" listening ");
    serial64_putdec((uint64_t)by_state[S_LISTEN]);
    serial64_puts(" connecting ");
    serial64_putdec((uint64_t)by_state[S_CONNECTING]);
    serial64_puts(" connected ");
    serial64_putdec((uint64_t)by_state[S_CONNECTED]);
    serial64_puts(" done ");
    serial64_putdec((uint64_t)by_state[S_DONE]);
    serial64_puts("\n");
}

static void wake_all(void) {
    events++;
    sched64_wake(INET64_WAIT_KEY, SCHED64_MAX_TASKS);
}

int inet64_valid(int s) {
    return s >= 0 && s < INET64_MAX && socks[s].state != S_FREE;
}

/* --- bring-up ----------------------------------------------------------- */

static void say_ip(uint32_t ip) {
    char text[16];
    net_format_ip(ip, text);
    serial64_puts(text);
}

int inet64_init(void) {
    const net_config_t* c;

    if (ready) return card;
    ready = 1;
    card = net_init();
    if (!card) {
        serial64_puts("NOVARIS64: [net] no RTL8139 - loopback only\n");
        return 0;
    }

    /* DHCP waits with hlt for the timer, which the caller has running. */
    if (!dhcp_configure(300)) {                    /* three seconds */
        serial64_puts("NOVARIS64: [net] no DHCP answer - using QEMU's"
                      " user-mode addresses\n");
        net_set_config(0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    }
    c = net_config();

    /* Resolve the gateway now, while waiting is allowed, so the first
     * datagram a program sends has somewhere to go. A UDP packet sent
     * while ARP is still asking is dropped, and a resolver's first
     * query then costs it its whole timeout. */
    if (c->gateway) {
        uint8_t mac[ETH_ALEN];
        uint64_t until = clock64_ticks() + 1000;
        while (!net_arp_lookup(c->gateway, mac) && clock64_ticks() < until) {
            for (int i = 0; i < 20 && clock64_ticks() < until; i++) {
                net_poll();
                __asm__ __volatile__("hlt");
            }
        }
    }
    last_arp_refresh = clock64_ticks();

    {
        const uint8_t* m = netdev_get()->mac;
        static const char hex[] = "0123456789abcdef";
        char mac[18];
        for (int i = 0; i < 6; i++) {
            mac[i * 3]     = hex[m[i] >> 4];
            mac[i * 3 + 1] = hex[m[i] & 15];
            mac[i * 3 + 2] = i < 5 ? ':' : 0;
        }
        serial64_puts("NOVARIS64: [net] eth0 ");
        serial64_puts(mac);
        serial64_puts(" address ");
        say_ip(c->ip);
        serial64_puts(" gateway ");
        say_ip(c->gateway);
        serial64_puts(" dns ");
        say_ip(c->dns);
        serial64_puts("\n");
    }
    return 1;
}

int inet64_card_up(void) { return card && net_config()->configured; }
uint32_t inet64_ip(void) { return card ? net_config()->ip : 0; }
uint32_t inet64_netmask(void) { return card ? net_config()->netmask : 0; }
const uint8_t* inet64_mac(void) { return card ? netdev_get()->mac : 0; }

/* --- the pump ----------------------------------------------------------- */

static void drop_tcp(isock_t* k) {
    k->tcp = -1;
    k->state = S_DONE;
    if (k->rx_open) {
        k->rx_open = 0;
        pipe64_unref(k->rx, 0, 1);              /* the reader sees EOF */
        sched64_wake(PIPE64_WAIT_KEY_(k->rx), SCHED64_MAX_TASKS);
    }
}

static void release_pipes(isock_t* k) {
    if (k->rx_open) { pipe64_unref(k->rx, 0, 1); k->rx_open = 0; }
    if (k->tx_open) { pipe64_unref(k->tx, 1, 0); k->tx_open = 0; }
    /* The anchors - see make_pipes. */
    if (k->rx >= 0) pipe64_unref(k->rx, 1, 0);
    if (k->tx >= 0) pipe64_unref(k->tx, 0, 1);
    if (k->rx >= 0) sched64_wake(PIPE64_WAIT_KEY_(k->rx), SCHED64_MAX_TASKS);
    if (k->tx >= 0) sched64_wake(PIPE64_WAIT_KEY_(k->tx), SCHED64_MAX_TASKS);
    k->rx = k->tx = -1;
}

static void free_sock(isock_t* k) {
    if (k->udp_bound) net_udp_unbind(k->lport);
    release_pipes(k);
    kmemset(k, 0, sizeof(*k));
    k->tcp = -1;
    k->rx = k->tx = -1;
}

/* The two pipes of a connection that has just been established. */
static int make_pipes(isock_t* k) {
    int rx = pipe64_create(), tx;
    if (rx < 0) return -1;
    tx = pipe64_create();
    if (tx < 0) { pipe64_unref(rx, 0, 0); return -1; }
    /* The stack's halves - the writer of rx, the reader of tx - and an
     * anchor on the other side of each, held until the record is freed.
     *
     * The anchors are what let a descriptor take its ends late (see
     * inet_fd_sync in syscall64.c): a pipe with no references is freed,
     * so without them a connection whose peer sent its data and its FIN
     * before the process first read would have had its rx pipe freed
     * under it by the end of file. With them the pipe outlives that, the
     * bytes wait in it, and the reader finds them and then the EOF. The
     * anchor on rx is a reader and on tx a writer, so neither changes
     * what the process sees: EOF is no writers on rx, -EPIPE no readers
     * on tx. */
    pipe64_ref(rx, 1, 1);
    pipe64_ref(tx, 1, 1);
    k->rx = rx; k->tx = tx;
    k->rx_open = 1; k->tx_open = 1;
    return 0;
}

/* Returns whether anything moved, so a poll can go round again while a
 * transfer is running and stop as soon as it is not. */
static int pump_one(isock_t* k) {
    static uint8_t buf[TCP_MSS];
    int moved = 0, h = k->tcp;
    tcp_state_t st;

    if (h < 0) {
        if (k->state == S_DONE && k->refs == 0) free_sock(k);
        return 0;
    }
    st = tcp_state(h);

    if (k->state == S_CONNECTING) {
        if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) {
            if (make_pipes(k) < 0) {
                k->err = 23;                           /* ENFILE */
                tcp_close(h);
                k->tcp = -1;
                k->state = S_DONE;
            } else {
                k->state = S_CONNECTED;
            }
            wake_all();
            return 1;
        }
        if (st == TCP_CLOSED) {
            /* A reset, or five unanswered SYNs. tcp.c does not say
             * which; a refusal is what a client is usually told. */
            k->err = 111;                              /* ECONNREFUSED */
            tcp_close(h);
            k->tcp = -1;
            k->state = S_DONE;
            wake_all();
            return 1;
        }
        return 0;
    }

    /* Received bytes into rx, as many as it has room for. With no
     * descriptor left they are taken and dropped, so the window stays
     * open and the peer can finish. */
    for (int round = 0; round < 16; round++) {
        uint32_t have = tcp_recv_ready(h), room;
        int n;
        if (!have) break;
        if (k->rx_open && k->refs > 0) {
            room = (uint32_t)pipe64_space(k->rx);
            if (!room) break;
            if (room > sizeof(buf)) room = sizeof(buf);
            n = tcp_recv(h, buf, room);
            if (n <= 0) break;
            pipe64_write(k->rx, buf, (uint64_t)n);
            sched64_wake(PIPE64_WAIT_KEY_(k->rx), SCHED64_MAX_TASKS);
        } else {
            n = tcp_recv(h, buf, sizeof(buf));
            if (n <= 0) break;
        }
        moved = 1;
    }
    if (k->rx_open && tcp_recv_ready(h) == 0 && tcp_eof(h)) {
        k->rx_open = 0;
        pipe64_unref(k->rx, 0, 1);
        sched64_wake(PIPE64_WAIT_KEY_(k->rx), SCHED64_MAX_TASKS);
        moved = 1;
    }

    /* The process's bytes into TCP, one segment at a time - tcp.c keeps
     * one in flight - and the FIN once there are no more. */
    if (k->tx_open && (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT)) {
        while (pipe64_available(k->tx) > 0 && tcp_tx_idle(h)) {
            int64_t n = pipe64_read(k->tx, buf, sizeof(buf));
            if (n <= 0) break;
            tcp_send(h, buf, (uint32_t)n);
            sched64_wake(PIPE64_WAIT_KEY_(k->tx), SCHED64_MAX_TASKS);
            moved = 1;
            /* Let the segment go out and the stack answer before the
             * next one is asked for. */
            break;
        }
        if (!k->fin_sent && pipe64_available(k->tx) == 0 && tcp_tx_idle(h) &&
            (k->shut_wr || k->refs == 0) &&
            /* With the peer finished, everything it sent has to be out
             * of TCP first: the slot is freed as soon as our FIN is
             * acknowledged, and what is still in it goes with it. */
            (st != TCP_CLOSE_WAIT || tcp_recv_ready(h) == 0)) {
            tcp_close(h);
            k->fin_sent = 1;
            k->linger_until = clock64_ticks() + 60000;
            /* Nothing more will be read from tx; a later write gets
             * -EPIPE, which is what writing after shutdown gets. */
            k->tx_open = 0;
            pipe64_unref(k->tx, 1, 0);
            sched64_wake(PIPE64_WAIT_KEY_(k->tx), SCHED64_MAX_TASKS);
            moved = 1;
        }
    }

    st = tcp_state(h);
    if (st == TCP_CLOSED || (k->fin_sent && st == TCP_TIME_WAIT &&
                             tcp_recv_ready(h) == 0)) {
        if (st == TCP_CLOSED && !k->fin_sent) {
            k->err = 104;                              /* ECONNRESET */
            if (k->tx_open) {
                k->tx_open = 0;
                pipe64_unref(k->tx, 1, 0);
                sched64_wake(PIPE64_WAIT_KEY_(k->tx), SCHED64_MAX_TASKS);
            }
        }
        drop_tcp(k);
        wake_all();
        return 1;
    }

    /* Closed by us and the peer never finishing: Linux gives up on
     * FIN_WAIT_2 after a minute, and so does this. Closing it in tcp.c
     * from that state frees the slot. */
    if (k->fin_sent && k->refs == 0 && clock64_ticks() > k->linger_until) {
        tcp_close(h);
        drop_tcp(k);
        return 1;
    }
    return moved;
}

static int pump(void) {
    int moved = 0;
    for (int i = 0; i < INET64_MAX; i++) {
        isock_t* k = &socks[i];
        if (k->state == S_FREE) continue;
        if (k->type == INET64_DGRAM) {
            if (k->udp_bound && net_udp_pending(k->lport)) wake_all();
            if (k->refs == 0) free_sock(k);
            continue;
        }
        if (k->state == S_LISTEN) {
            if (tcp_accept_ready(k->lport)) wake_all();
            continue;
        }
        moved |= pump_one(k);
    }
    return moved;
}

static int active(void) {
    for (int i = 0; i < INET64_MAX; i++)
        if (socks[i].state != S_FREE) return 1;
    return 0;
}

static void note_activity(void) {
    sched64_external_wakers = ready && active();
}

void inet64_poll(void) {
    if (!ready) return;

    /* Keep the gateway's MAC fresh. net.c forgets an entry after two
     * minutes and drops what is sent while it asks again. */
    if (card && net_config()->gateway &&
        clock64_ticks() - last_arp_refresh > 60000) {
        uint8_t mac[ETH_ALEN];
        last_arp_refresh = clock64_ticks();
        net_arp_lookup(net_config()->gateway, mac);
    }

    /* A few rounds while things are moving: each one lets a segment be
     * answered, which over the loopback is the whole round trip. */
    for (int round = 0; round < 8; round++) {
        uint32_t before = net_stats()->rx_frames + net_loopback_packets() +
                          net_stats()->rx_ip;
        net_poll();
        int moved = pump();
        uint32_t after = net_stats()->rx_frames + net_loopback_packets() +
                         net_stats()->rx_ip;
        if (!moved && after == before) break;
    }
    note_activity();
}

void inet64_timer(void) {
    if (!ready) return;
    if (!card && !active()) return;
    inet64_poll();
}

/* --- sockets ------------------------------------------------------------ */

void inet64_reset(void) {
    for (int i = 0; i < INET64_MAX; i++) {
        isock_t* k = &socks[i];
        if (k->state == S_FREE) continue;
        if (k->state == S_LISTEN) {
            int h;
            tcp_unlisten(k->lport);
            while ((h = tcp_accept(k->lport)) >= 0) tcp_close(h);
        }
        if (k->tcp >= 0) tcp_close(k->tcp);
        if (k->udp_bound) net_udp_unbind(k->lport);
        kmemset(k, 0, sizeof(*k));
        k->tcp = -1;
        k->rx = k->tx = -1;
    }
    sched64_external_wakers = 0;
}

int inet64_socket(int type) {
    /* No card brought up yet: a loopback, without the DHCP wait, which
     * cannot run here - interrupts are off in a system call. */
    if (!ready) { ready = 1; tcp_init(); }
    for (int i = 0; i < INET64_MAX; i++) {
        isock_t* k = &socks[i];
        if (k->state != S_FREE) continue;
        kmemset(k, 0, sizeof(*k));
        k->state = S_NEW;
        k->type = type;
        k->refs = 1;
        k->tcp = -1;
        k->rx = k->tx = -1;
        return i;
    }
    full_once("sockets", INET64_MAX);
    return -23;                                        /* -ENFILE */
}

void inet64_ref(int s) { if (inet64_valid(s)) socks[s].refs++; }

void inet64_unref(int s) {
    isock_t* k;
    if (!inet64_valid(s)) return;
    k = &socks[s];
    if (k->refs > 0) k->refs--;
    if (k->refs) return;

    if (k->state == S_LISTEN) {
        /* Whatever the stack completed and nobody accepted is reset
         * rather than left established forever. */
        int h;
        tcp_unlisten(k->lport);
        while ((h = tcp_accept(k->lport)) >= 0) tcp_close(h);
        free_sock(k);
    } else if (k->state == S_CONNECTING) {
        tcp_close(k->tcp);
        free_sock(k);
    } else if (k->type == INET64_DGRAM || k->state == S_NEW ||
               k->state == S_DONE) {
        free_sock(k);
    }
    /* S_CONNECTED: the pump sends what is left, then the FIN, and frees
     * the record once TCP has finished with the connection. */
    wake_all();
}

int inet64_type(int s) { return inet64_valid(s) ? socks[s].type : 0; }
int inet64_rx(int s) { return inet64_valid(s) ? socks[s].rx : -1; }
int inet64_tx(int s) { return inet64_valid(s) ? socks[s].tx : -1; }

static int port_taken(int type, uint16_t port) {
    for (int i = 0; i < INET64_MAX; i++) {
        isock_t* k = &socks[i];
        if (k->state == S_FREE || k->type != type) continue;
        if (k->lport == port && (k->state == S_LISTEN || k->udp_bound ||
                                 k->state == S_NEW))
            return 1;
    }
    return 0;
}

static uint16_t ephemeral(int type) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_ephemeral++;
        if (next_ephemeral >= 49152) next_ephemeral = 32768;
        if (!port_taken(type, p)) return p;
    }
    return 0;
}

static int udp_attach(isock_t* k) {
    if (k->udp_bound) return 0;
    if (!k->lport) k->lport = ephemeral(INET64_DGRAM);
    if (!k->lport || !net_udp_bind(k->lport)) {
        full_once("UDP ports", UDP_PORTS);
        return -105;                                   /* -ENOBUFS */
    }
    k->udp_bound = 1;
    return 0;
}

int inet64_bind(int s, uint32_t ip, uint16_t port) {
    isock_t* k;
    if (!inet64_valid(s)) return -9;
    k = &socks[s];
    if (k->state != S_NEW || k->lport) return -22;     /* -EINVAL: bound */
    if (ip && !net_is_local(ip) && ip != inet64_ip())
        return -99;                                    /* -EADDRNOTAVAIL */
    if (port && port_taken(k->type, port)) return -98; /* -EADDRINUSE */
    if (!port) port = ephemeral(k->type);
    if (!port) return -98;
    k->lip = ip;
    k->lport = port;
    if (k->type == INET64_DGRAM) return udp_attach(k);
    return 0;
}

int inet64_listen(int s) {
    isock_t* k;
    if (!inet64_valid(s)) return -9;
    k = &socks[s];
    if (k->type != INET64_STREAM) return -95;          /* -EOPNOTSUPP */
    if (k->state == S_LISTEN) return 0;
    if (k->state != S_NEW) return -22;
    if (!k->lport) {
        k->lport = ephemeral(INET64_STREAM);
        if (!k->lport) return -98;
    }
    if (tcp_listen(k->lport) < 0) return -105;         /* -ENOBUFS */
    k->state = S_LISTEN;
    return 0;
}

int inet64_is_listening(int s) {
    return inet64_valid(s) && socks[s].state == S_LISTEN;
}

int inet64_accept_ready(int s) {
    return inet64_is_listening(s) && tcp_accept_ready(socks[s].lport);
}

int inet64_accept(int s) {
    isock_t* k;
    int h, n;
    if (!inet64_valid(s)) return -9;
    k = &socks[s];
    if (k->state != S_LISTEN) return -22;

    inet64_poll();
    if (!tcp_accept_ready(k->lport)) return -11;       /* -EAGAIN */

    n = inet64_socket(INET64_STREAM);
    if (n < 0) return n;
    h = tcp_accept(k->lport);
    if (h < 0) { free_sock(&socks[n]); return -11; }

    if (make_pipes(&socks[n]) < 0) {
        tcp_close(h);
        free_sock(&socks[n]);
        return -23;
    }
    socks[n].state = S_CONNECTED;
    socks[n].tcp = h;
    socks[n].lip = k->lip;
    socks[n].lport = k->lport;
    socks[n].rip = tcp_peer_ip(h);
    socks[n].rport = tcp_peer_port(h);
    return n;
}

int inet64_connect(int s, uint32_t ip, uint16_t port) {
    isock_t* k;
    int h;
    if (!inet64_valid(s)) return -9;
    k = &socks[s];

    if (k->type == INET64_DGRAM) {
        int e = udp_attach(k);
        if (e) return e;
        k->rip = ip;
        k->rport = port;
        return 0;
    }

    switch (k->state) {
    case S_CONNECTED:
        return k->rip == ip && k->rport == port ? 0 : -106;   /* -EISCONN */
    case S_CONNECTING:
        inet64_poll();
        if (k->state == S_CONNECTED) return 0;
        if (k->state == S_CONNECTING) return -115;     /* -EINPROGRESS */
        /* it failed while we looked */
        __attribute__((fallthrough));
    case S_DONE: {
        int e = k->err;
        if (e) { k->err = 0; return -e; }
        return -106;
    }
    case S_LISTEN:
        return -22;
    default:
        break;
    }

    if (!port) return -111;
    if (ip == 0) ip = 0x7F000001u;                     /* INADDR_ANY: here */
    if (!net_is_local(ip) && !inet64_card_up()) return -101;  /* -ENETUNREACH */

    h = tcp_connect(ip, port);
    if (h == -1) return -101;
    if (h < 0) {                                       /* table full */
        full_once("TCP connections", TCP_MAX_CONNECTIONS);
        return -105;
    }
    k->tcp = h;
    k->rip = ip;
    k->rport = port;
    k->state = S_CONNECTING;

    inet64_poll();
    if (k->state == S_CONNECTED) return 0;
    if (k->state == S_DONE) { int e = k->err; k->err = 0; return -(e ? e : 111); }
    return -115;
}

int inet64_connecting(int s) {
    return inet64_valid(s) && socks[s].state == S_CONNECTING;
}

int inet64_take_error(int s) {
    int e;
    if (!inet64_valid(s)) return 0;
    e = socks[s].err;
    socks[s].err = 0;
    return e;
}

int inet64_shutdown(int s, int how) {
    isock_t* k;
    if (!inet64_valid(s)) return -9;
    k = &socks[s];
    if (k->type == INET64_STREAM && k->state != S_CONNECTED) return -107;
    if (how == 1 || how == 2) k->shut_wr = 1;
    inet64_poll();
    return 0;
}

int inet64_local(int s, uint32_t* ip, uint16_t* port) {
    isock_t* k;
    if (!inet64_valid(s)) return -9;
    k = &socks[s];
    *ip = k->lip;
    if (!*ip && (k->rip || k->state == S_CONNECTED))
        *ip = net_source_ip(k->rip);
    *port = k->lport;
    if (k->tcp >= 0 && (k->state == S_CONNECTING || k->state == S_CONNECTED))
        *port = tcp_local_port(k->tcp);
    return 0;
}

int inet64_peer(int s, uint32_t* ip, uint16_t* port) {
    isock_t* k;
    if (!inet64_valid(s)) return -9;
    k = &socks[s];
    if (k->type == INET64_STREAM && k->state != S_CONNECTED &&
        k->state != S_DONE) return -107;               /* -ENOTCONN */
    if (k->type == INET64_DGRAM && !k->rport) return -107;
    *ip = k->rip;
    *port = k->rport;
    return 0;
}

int64_t inet64_sendto(int s, const void* buf, uint64_t n,
                      uint32_t ip, uint16_t port) {
    isock_t* k;
    int r, e;
    if (!inet64_valid(s)) return -9;
    k = &socks[s];
    if (k->type != INET64_DGRAM) return -95;
    if (!port) { ip = k->rip; port = k->rport; }
    if (!port) return -89;                             /* -EDESTADDRREQ */
    if (n > NET_UDP_MAX_PAYLOAD) return -90;           /* -EMSGSIZE */
    e = udp_attach(k);
    if (e) return e;
    if (!net_is_local(ip) && !inet64_card_up()) return -101;

    r = net_send_udp(ip, k->lport, port, (const uint8_t*)buf, (uint32_t)n);
    if (r == -2) {
        /* The next hop's MAC is being asked for. The answer from QEMU's
         * network comes back in microseconds, so wait for it here - a
         * bounded number of polls - rather than drop the datagram. The
         * request went out with the first try; asking again each round
         * would flood the wire with them. */
        for (int i = 0; i < 200000 && r == -2; i++) {
            net_poll();
            if (i % 20000 == 19999)
                r = net_send_udp(ip, k->lport, port, (const uint8_t*)buf,
                                 (uint32_t)n);
        }
        if (r == -2)
            r = net_send_udp(ip, k->lport, port, (const uint8_t*)buf,
                             (uint32_t)n);
    }
    if (r < 0 && r != -2) return -105;
    inet64_poll();
    return (int64_t)n;
}

int inet64_dgram_ready(int s) {
    isock_t* k;
    if (!inet64_valid(s)) return 0;
    k = &socks[s];
    return k->type == INET64_DGRAM && k->udp_bound && net_udp_pending(k->lport);
}

int64_t inet64_recvfrom(int s, void* buf, uint64_t n, int peek,
                        uint32_t* ip, uint16_t* port) {
    static udp_datagram_t d;
    isock_t* k;
    if (!inet64_valid(s)) return -9;
    k = &socks[s];
    if (k->type != INET64_DGRAM) return -95;
    if (!k->udp_bound) return -11;
    inet64_poll();
    (void)peek;     /* net.c's queue cannot be looked at without taking */
    if (!net_udp_recv(k->lport, &d)) return -11;
    if (n > d.len) n = d.len;
    kmemcpy(buf, d.data, n);
    if (ip) *ip = d.src_ip;
    if (port) *port = d.src_port;
    return (int64_t)n;
}

int inet64_sockets_open(void) {
    int n = 0;
    for (int i = 0; i < INET64_MAX; i++) if (socks[i].state != S_FREE) n++;
    return n;
}

/* --- what Linux tells a program about its interfaces ------------------- *
 *
 * Wine's nsiproxy.sys builds the Windows adapter table from these: it
 * walks if_nameindex() (netlink, in syscall64.c), then asks a UDP socket
 * SIOCGIFHWADDR, SIOCGIFFLAGS and SIOCGIFMTU about each name, and reads
 * the routes from /proc/net/route. An interface whose SIOCGIFHWADDR
 * fails is left out, so without these GetAdaptersAddresses found
 * nothing and every caller - Chromium's network code among them -
 * was told ERROR_NOT_SUPPORTED. */

/* Index 1 is lo, 2 is eth0 (when the card is up); 0 for neither. */
static int if_by_name(const char* name) {
    if (!kstrcmp(name, "lo")) return 1;
    if (inet64_card_up() && !kstrcmp(name, "eth0")) return 2;
    return 0;
}

static void put_sin(uint8_t* sa, uint32_t ip) {
    kmemset(sa, 0, 16);
    sa[0] = 2;                                      /* AF_INET */
    sa[4] = (uint8_t)(ip >> 24); sa[5] = (uint8_t)(ip >> 16);
    sa[6] = (uint8_t)(ip >> 8);  sa[7] = (uint8_t)ip;
}

/* One ifreq ioctl. `ifr` is the caller's struct ifreq, already checked
 * to be 40 bytes of writable memory: the name, then a 24-byte union. */
int inet64_ifreq(uint64_t cmd, uint8_t* ifr) {
    char name[16];
    int idx;
    uint8_t* u = ifr + 16;

    if (cmd == 0x8910) {                            /* SIOCGIFNAME */
        idx = *(int*)u;
        if (idx == 1) kstrlcpy((char*)ifr, "lo", 16);
        else if (idx == 2 && inet64_card_up()) kstrlcpy((char*)ifr, "eth0", 16);
        else return -19;                            /* -ENODEV */
        return 0;
    }

    kmemcpy(name, ifr, 16);
    name[15] = 0;
    idx = if_by_name(name);
    if (!idx) return -19;

    switch (cmd) {
    case 0x8913:                                    /* SIOCGIFFLAGS */
        /* IFF_UP|IFF_RUNNING, and LOOPBACK or BROADCAST|MULTICAST */
        *(uint16_t*)u = idx == 1 ? (0x1 | 0x8 | 0x40)
                                 : (0x1 | 0x2 | 0x40 | 0x1000);
        return 0;
    case 0x8927:                                    /* SIOCGIFHWADDR */
        kmemset(u, 0, 16);
        *(uint16_t*)u = idx == 1 ? 772 : 1;         /* ARPHRD_LOOPBACK/ETHER */
        if (idx == 2) kmemcpy(u + 2, inet64_mac(), 6);
        return 0;
    case 0x8921:                                    /* SIOCGIFMTU */
        *(int*)u = idx == 1 ? 65536 : 1500;
        return 0;
    case 0x8933:                                    /* SIOCGIFINDEX */
        *(int*)u = idx;
        return 0;
    case 0x8915:                                    /* SIOCGIFADDR */
        put_sin(u, idx == 1 ? 0x7F000001u : inet64_ip());
        return 0;
    case 0x891B:                                    /* SIOCGIFNETMASK */
        put_sin(u, idx == 1 ? 0xFF000000u : inet64_netmask());
        return 0;
    case 0x8919:                                    /* SIOCGIFBRDADDR */
        put_sin(u, idx == 1 ? 0 : (inet64_ip() | ~inet64_netmask()));
        return 0;
    case 0x8B01:                                    /* SIOCGIWNAME */
        return -95;               /* -EOPNOTSUPP: not a wireless card */
    default:
        return -25;                                 /* -ENOTTY */
    }
}

/* SIOCGIFCONF: an ifreq with its address for each interface, into the
 * caller's buffer. Returns the bytes used. */
int inet64_ifconf(uint8_t* buf, int len) {
    int used = 0;
    for (int idx = 1; idx <= (inet64_card_up() ? 2 : 1); idx++) {
        if (used + 40 > len) break;
        kmemset(buf + used, 0, 40);
        kstrlcpy((char*)buf + used, idx == 1 ? "lo" : "eth0", 16);
        put_sin(buf + used + 16, idx == 1 ? 0x7F000001u : inet64_ip());
        used += 40;
    }
    return used;
}

static char* put_str(char* o, const char* s) { while (*s) *o++ = *s++; return o; }

static char* put_u(char* o, uint64_t v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *o++ = t[--n];
    return o;
}

/* An address as /proc/net/route prints it: the network-order bytes read
 * as a little-endian word, in hex. 10.0.2.2 is 0202000A. */
static char* put_route_hex(char* o, uint32_t ip) {
    static const char hex[] = "0123456789ABCDEF";
    uint32_t le = ((ip & 0xFF) << 24) | ((ip & 0xFF00) << 8) |
                  ((ip >> 8) & 0xFF00) | (ip >> 24);
    for (int s = 28; s >= 0; s -= 4) *o++ = hex[(le >> s) & 15];
    return o;
}

int inet64_proc_route(char* out) {
    char* o = out;
    const net_config_t* c = net_config();
    o = put_str(o, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\t"
                   "Mask\t\tMTU\tWindow\tIRTT\n");
    if (inet64_card_up()) {
        if (c->gateway) {
            o = put_str(o, "eth0\t00000000\t");
            o = put_route_hex(o, c->gateway);
            o = put_str(o, "\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
        }
        o = put_str(o, "eth0\t");
        o = put_route_hex(o, c->ip & c->netmask);
        o = put_str(o, "\t00000000\t0001\t0\t0\t0\t");
        o = put_route_hex(o, c->netmask);
        o = put_str(o, "\t0\t0\t0\n");
    }
    return (int)(o - out);
}

int inet64_proc_dev(char* out) {
    char* o = out;
    o = put_str(o, "Inter-|   Receive                                                |  Transmit\n"
                   " face |bytes    packets errs drop fifo frame compressed multicast|"
                   "bytes    packets errs colls carrier compressed\n");
    o = put_str(o, "    lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    if (inet64_card_up()) {
        const netdev_t* d = netdev_get();
        o = put_str(o, "  eth0: ");
        o = put_u(o, d->rx_bytes);   *o++ = ' ';
        o = put_u(o, d->rx_packets); *o++ = ' ';
        o = put_u(o, d->rx_errors);  *o++ = ' ';
        o = put_u(o, d->rx_dropped);
        o = put_str(o, " 0 0 0 0 ");
        o = put_u(o, d->tx_bytes);   *o++ = ' ';
        o = put_u(o, d->tx_packets); *o++ = ' ';
        o = put_u(o, d->tx_errors);
        o = put_str(o, " 0 0 0 0 0\n");
    }
    return (int)(o - out);
}

/* Every socket at the deadline: what state the socket layer thinks it
 * is in, what TCP thinks, and how much is waiting in its pipes. A page
 * load that never finishes is either a connection that never completed
 * or one whose news never reached the process polling it, and this says
 * which. */
void inet64_report(void) {
    static const char* const names[] = { "free", "new", "listen", "connecting",
                                         "connected", "done" };
    int n = 0;
    for (int i = 0; i < INET64_MAX; i++) {
        isock_t* k = &socks[i];
        if (k->state == S_FREE) continue;
        n++;
        serial64_puts("NOVARIS64: [inet] sock ");
        serial64_putdec((uint64_t)i);
        serial64_puts(k->type == INET64_DGRAM ? " udp " : " tcp ");
        serial64_puts(names[k->state]);
        serial64_puts(" refs ");
        serial64_putdec((uint64_t)k->refs);
        serial64_puts(" lport ");
        serial64_putdec(k->lport);
        serial64_puts(" peer ");
        say_ip(k->rip);
        serial64_putc(':');
        serial64_putdec(k->rport);
        if (k->tcp >= 0) {
            serial64_puts(" tcpstate ");
            serial64_putdec((uint64_t)tcp_state(k->tcp));
            serial64_puts(" idle ");
            serial64_putdec((uint64_t)tcp_tx_idle(k->tcp));
            serial64_puts(" recvq ");
            serial64_putdec(tcp_recv_ready(k->tcp));
        }
        if (k->rx >= 0) {
            serial64_puts(" rx ");
            serial64_putdec(pipe64_available(k->rx));
            serial64_puts("/r");
            serial64_putdec((uint64_t)pipe64_readers(k->rx));
            serial64_puts(" tx ");
            serial64_putdec(pipe64_available(k->tx));
            serial64_puts("/w");
            serial64_putdec((uint64_t)pipe64_writers(k->tx));
        }
        if (k->err) { serial64_puts(" err "); serial64_putdec((uint64_t)k->err); }
        serial64_putc('\n');
    }
    serial64_puts("NOVARIS64: [inet] ");
    serial64_putdec((uint64_t)n);
    serial64_puts(" sockets; frames in ");
    serial64_putdec(net_stats()->rx_frames);
    serial64_puts(" out ");
    serial64_putdec(net_stats()->tx_frames);
    serial64_puts(", tcp retransmits ");
    serial64_putdec(tcp_retransmits());
    serial64_puts(", arp entries ");
    serial64_putdec(net_arp_entries());
    serial64_putc('\n');
}
