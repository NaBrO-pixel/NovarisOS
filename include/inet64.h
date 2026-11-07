#ifndef INET64_H
#define INET64_H

#include <stdint.h>

/* inet64 - AF_INET sockets for the 64-bit kernel.
 *
 * The protocols are the 32-bit kernel's (kernel/net.c, tcp.c), which
 * this kernel compiles as they are; what is here is the part a process
 * sees. A connected TCP socket is presented as a pair of pipe64 pipes -
 * one the stack fills with what arrives, one it drains into segments -
 * so read, write, poll, fork, dup and close are the descriptor layer
 * that already exists, and what this file adds is the pump between the
 * pipes and TCP. Listening sockets, connects in progress and UDP have no
 * pipes and are asked about directly.
 *
 * Everything here runs with interrupts off: from a system call, or from
 * the timer interrupt through inet64_timer(). */

#define INET64_MAX 512

/* What a blocked accept, connect or UDP receive waits on. Every event
 * the pump sees wakes it, and the waiters look again. */
#define INET64_WAIT_KEY 0x5000000000000000ULL

#define INET64_STREAM 1
#define INET64_DGRAM  2

/* Brings the card up and asks DHCP for an address, falling back to
 * QEMU's user-mode network numbers if nothing answers. Needs the timer
 * running and interrupts on, because DHCP waits. Returns 1 if a card was
 * found; the loopback works either way. */
int  inet64_init(void);
int  inet64_card_up(void);       /* a card, and an address for it */

/* Turns the crank: drains the card and the loopback, runs TCP's timers,
 * and moves bytes between the connections and their pipes. */
void inet64_poll(void);
void inet64_timer(void);         /* the same, from the 1kHz timer */

/* Forgets every socket without touching its pipes: pipe64_init, which
 * calls this, has just forgotten those. Connections are aborted. */
void inet64_reset(void);

int  inet64_socket(int type);    /* an index, or -errno */
void inet64_ref(int s);
void inet64_unref(int s);        /* the last one closes it */
int  inet64_type(int s);
int  inet64_valid(int s);

/* The pipes of a connected stream socket: what the stack fills and what
 * it drains. -1 until connected. */
int  inet64_rx(int s);
int  inet64_tx(int s);

int  inet64_bind(int s, uint32_t ip, uint16_t port);
int  inet64_listen(int s);
int  inet64_is_listening(int s);
int  inet64_accept(int s);       /* a new socket's index, -EAGAIN, -errno */
int  inet64_accept_ready(int s);

/* Starts a connection, or for UDP sets the default peer. 0 when it is
 * already established (UDP always), -EINPROGRESS (-115) while the
 * handshake runs - call again to find out - or -errno. */
int  inet64_connect(int s, uint32_t ip, uint16_t port);
int  inet64_connecting(int s);
int  inet64_take_error(int s);   /* SO_ERROR: returned once, then 0 */
int  inet64_shutdown(int s, int how);

/* Addresses, host order. 0 if there is none to report. */
int  inet64_local(int s, uint32_t* ip, uint16_t* port);
int  inet64_peer(int s, uint32_t* ip, uint16_t* port);

/* UDP. sendto with ip 0 uses the connected peer. Returns bytes or
 * -errno; recv returns -EAGAIN when nothing is queued. */
int64_t inet64_sendto(int s, const void* buf, uint64_t n,
                      uint32_t ip, uint16_t port);
int64_t inet64_recvfrom(int s, void* buf, uint64_t n, int peek,
                        uint32_t* ip, uint16_t* port);
int  inet64_dgram_ready(int s);

/* For /proc and the interface list. */
int  inet64_sockets_open(void);
uint32_t inet64_ip(void);
uint32_t inet64_netmask(void);
uint32_t inet64_dns(void);       /* the server DHCP named, 0 for none */
const uint8_t* inet64_mac(void);

/* The ifreq ioctls (SIOCGIFFLAGS, SIOCGIFHWADDR, ...), SIOCGIFCONF,
 * and the text of /proc/net/route and /proc/net/dev (under 1KB each). */
int  inet64_ifreq(uint64_t cmd, uint8_t* ifr);
int  inet64_ifconf(uint8_t* buf, int len);
int  inet64_proc_route(char* out);
int  inet64_proc_dev(char* out);
void inet64_report(void);
int  inet64_watched(int s);
int  inet64_stalled(uint64_t ticks);   /* once: a connection carrying nothing */
void inet64_note_poll(int s, int pid, int want_out, int gave_out);         /* every socket, for the deadline */

#endif
