#ifndef NET_H
#define NET_H

#include <stdint.h>
#include "netdev.h"

/* net - Ethernet, ARP, IPv4, ICMP and UDP.
 *
 * Milestone 38. Everything above the card, up to but not including TCP.
 *
 * Two decisions shape the whole of it.
 *
 * **Addresses are host-order `uint32_t` everywhere except on the wire.**
 * A stack that keeps addresses in network order spends its life calling
 * ntohl to compare two of them and gets one of the calls wrong. Here the
 * conversion happens exactly at the two edges - reading a header and
 * writing one.
 *
 * **Nothing blocks in the interrupt handler.** A frame arriving is
 * classified, and either answered immediately with a frame of its own
 * (ARP requests, ICMP echo) or copied into a small per-socket queue for
 * whoever asked. The alternative - waking a task from the handler - needs
 * the scheduler to be re-entrant from an IRQ, which it is not.
 */

/* --- byte order --------------------------------------------------------- */

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }

static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* --- what is on the wire ------------------------------------------------ */

#define ETH_P_IP   0x0800
#define ETH_P_ARP  0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* --- the interface's configuration -------------------------------------- */

typedef struct {
    uint32_t ip;        /* host order, 0 until DHCP has answered */
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    int      configured;
} net_config_t;

const net_config_t* net_config(void);
void net_set_config(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);

/* Brings up the stack over whichever card netdev has. Returns 1 if there
 * is a card to bring up. */
int net_init(void);

/* Whether there is a card at all - which is a different question from
 * whether it has an address yet. */
int net_available(void);

/* Turns the crank: drains the card, retries anything waiting on ARP, and
 * expires what has timed out. Called from the desktop's idle loop and
 * from any code waiting on the network, because this kernel has no
 * timer thread. */
void net_poll(void);

/* --- sending ------------------------------------------------------------ */

/* Sends an IPv4 datagram. Resolves the next hop through ARP, which may
 * mean the first call queues an ARP request and returns -EAGAIN-ish
 * (negative); the caller retries after net_poll(). */
int net_send_ip(uint32_t dest_ip, uint8_t protocol,
                const uint8_t* payload, uint32_t len);

/* UDP, with source and destination ports in host order. */
int net_send_udp(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
                 const uint8_t* payload, uint32_t len);

/* --- receiving ---------------------------------------------------------- */

/* A UDP datagram waiting for a bound port. The stack keeps a handful; a
 * port with nobody listening drops. */
#define NET_UDP_MAX_PAYLOAD 1472
typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t  data[NET_UDP_MAX_PAYLOAD];
} udp_datagram_t;

/* Claims a port so datagrams to it are kept rather than dropped. One
 * listener per port, and eight ports, which is what DHCP and DNS need
 * with room to spare. */
int  net_udp_bind(uint16_t port);
void net_udp_unbind(uint16_t port);
/* The oldest datagram for a bound port, or 0. */
int  net_udp_recv(uint16_t port, udp_datagram_t* out);
/* Whether there is one, without taking it. What poll() needs: asking by
 * receiving would consume the thing being asked about. */
int  net_udp_pending(uint16_t port);

/* --- ICMP --------------------------------------------------------------- */

/* Sends one echo request. Replies are counted and the last round-trip
 * recorded; see net_ping_replies(). */
int net_ping(uint32_t dest_ip, uint16_t seq);
uint32_t net_ping_replies(void);
uint32_t net_ping_last_ticks(void);

/* --- ARP ---------------------------------------------------------------- */

/* The MAC for an address, if it is known. Returns 0 and sends a request
 * if it is not. */
int net_arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN]);
uint32_t net_arp_entries(void);

/* --- formatting --------------------------------------------------------- */

/* "10.0.2.15" into `out`, which needs 16 bytes. */
void net_format_ip(uint32_t ip, char* out);
/* The other way; returns 0 if it is not four dotted decimals. */
int net_parse_ip(const char* text, uint32_t* out);

/* Statistics, for `netinfo`. */
typedef struct {
    uint32_t rx_frames, rx_arp, rx_ip, rx_icmp, rx_udp, rx_tcp, rx_other;
    uint32_t tx_frames, tx_arp, tx_ip;
    uint32_t dropped_no_route, dropped_checksum;
} net_stats_t;
const net_stats_t* net_stats(void);

/* The one-line internet checksum, exposed because TCP needs it too. */
uint16_t net_checksum(const void* data, uint32_t len, uint32_t initial);

#endif /* NET_H */
