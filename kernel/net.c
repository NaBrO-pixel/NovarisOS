/* net.c - Ethernet, ARP, IPv4, ICMP and UDP.
 *
 * See include/net.h for the two decisions that shape this file. What
 * follows is the ordinary shape of a small IP stack: a receive path that
 * demultiplexes by header, a send path that fills headers in and hands
 * the result to the card, and an ARP cache in between because Ethernet
 * has no idea what an IP address is.
 *
 * The one thing worth knowing that is *not* ordinary: there is no timer
 * thread and no softirq. A frame is processed inside the card's interrupt
 * handler, and anything that has to wait - an ARP resolution, a DHCP
 * lease, a TCP retransmit - waits in a structure that net_poll() walks.
 * net_poll() is called from the desktop's idle loop and from any code
 * that is waiting for the network. That is why the stack has no
 * background activity at all when nothing is using it, and it is why a
 * program that waits on the network has to call something rather than
 * simply block.
 */

#include "net.h"
#include "netdev.h"
#include "rtl8139.h"
#include "kstring.h"
#include "pit.h"
#include "console.h"
#include "vga_text.h"

/* --- headers, as they are on the wire ----------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  dest[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} eth_header_t;

typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;
} arp_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dest;
} ip_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, code;
    uint16_t checksum;
    uint16_t id, seq;
} icmp_header_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port, dest_port;
    uint16_t length, checksum;
} udp_header_t;

/* --- state -------------------------------------------------------------- */

static netdev_t*    dev;
static net_config_t config;
static net_stats_t  stats;
static uint16_t     ip_id;

static const uint8_t broadcast_mac[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* The ARP cache. Sixteen entries, no eviction policy beyond "oldest
 * first": a machine on one subnet talks to a gateway and a handful of
 * hosts, and a cache that needs a policy is a cache that is too small. */
#define ARP_ENTRIES 16
#define ARP_TTL_TICKS (100 * 120)      /* two minutes at 100Hz */

typedef struct {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    uint32_t stamp;
    int      valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_ENTRIES];

/* --- checksum ----------------------------------------------------------- */

uint16_t net_checksum(const void* data, uint32_t len, uint32_t initial) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = initial;

    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)(p[0] << 8);

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* --- addresses as text -------------------------------------------------- */

static void put_dec(char** out, uint32_t v) {
    char tmp[4];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *(*out)++ = tmp[--n];
}

void net_format_ip(uint32_t ip, char* out) {
    char* p = out;
    put_dec(&p, (ip >> 24) & 0xFF); *p++ = '.';
    put_dec(&p, (ip >> 16) & 0xFF); *p++ = '.';
    put_dec(&p, (ip >> 8) & 0xFF);  *p++ = '.';
    put_dec(&p, ip & 0xFF);
    *p = '\0';
}

int net_parse_ip(const char* text, uint32_t* out) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int n = 0;

    while (*text && n < 4) {
        if (*text < '0' || *text > '9') return 0;
        uint32_t v = 0;
        while (*text >= '0' && *text <= '9') {
            v = v * 10 + (uint32_t)(*text++ - '0');
            if (v > 255) return 0;
        }
        parts[n++] = v;
        if (*text == '.') text++;
        else break;
    }
    if (n != 4 || *text) return 0;

    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

/* --- the ARP cache ------------------------------------------------------ */

static void arp_store(uint32_t ip, const uint8_t* mac) {
    arp_entry_t* slot = 0;

    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { slot = &arp_cache[i]; break; }
        if (!arp_cache[i].valid && !slot) slot = &arp_cache[i];
    }
    if (!slot) {
        slot = &arp_cache[0];
        for (int i = 1; i < ARP_ENTRIES; i++) {
            if (arp_cache[i].stamp < slot->stamp) slot = &arp_cache[i];
        }
    }

    slot->ip = ip;
    kmemcpy(slot->mac, mac, ETH_ALEN);
    slot->stamp = pit_get_ticks();
    slot->valid = 1;
}

static const uint8_t* arp_find(uint32_t ip) {
    uint32_t now = pit_get_ticks();
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!arp_cache[i].valid || arp_cache[i].ip != ip) continue;
        if (now - arp_cache[i].stamp > ARP_TTL_TICKS) {
            arp_cache[i].valid = 0;
            return 0;
        }
        return arp_cache[i].mac;
    }
    return 0;
}

uint32_t net_arp_entries(void) {
    uint32_t n = 0;
    for (int i = 0; i < ARP_ENTRIES; i++) if (arp_cache[i].valid) n++;
    return n;
}

/* --- sending ------------------------------------------------------------ */

static int eth_send(const uint8_t dest_mac[ETH_ALEN], uint16_t type,
                    const uint8_t* payload, uint32_t len) {
    static uint8_t frame[ETH_FRAME_MAX];

    if (!dev || !dev->send) return -1;
    if (len > ETH_MTU) return -1;

    eth_header_t* eh = (eth_header_t*)frame;
    kmemcpy(eh->dest, dest_mac, ETH_ALEN);
    kmemcpy(eh->src, dev->mac, ETH_ALEN);
    eh->type = htons(type);
    kmemcpy(frame + ETH_HDR_LEN, payload, len);

    int r = dev->send(dev, frame, ETH_HDR_LEN + len);
    if (r == 0) stats.tx_frames++;
    return r;
}

static void arp_request(uint32_t ip) {
    arp_packet_t a;

    a.htype = htons(1);            /* Ethernet */
    a.ptype = htons(ETH_P_IP);
    a.hlen = ETH_ALEN;
    a.plen = 4;
    a.oper = htons(1);             /* request */
    kmemcpy(a.sender_mac, dev->mac, ETH_ALEN);
    a.sender_ip = htonl(config.ip);
    kmemset(a.target_mac, 0, ETH_ALEN);
    a.target_ip = htonl(ip);

    eth_send(broadcast_mac, ETH_P_ARP, (const uint8_t*)&a, sizeof(a));
    stats.tx_arp++;
}

int net_arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    const uint8_t* mac = arp_find(ip);
    if (mac) {
        kmemcpy(mac_out, mac, ETH_ALEN);
        return 1;
    }
    if (dev) arp_request(ip);
    return 0;
}

/* Which MAC a datagram for `dest_ip` goes to: the host itself if it is on
 * our subnet, the gateway otherwise. */
static int next_hop_mac(uint32_t dest_ip, uint8_t mac_out[ETH_ALEN]) {
    if (dest_ip == 0xFFFFFFFFu ||
        (config.netmask && (dest_ip | ~config.netmask) == 0xFFFFFFFFu &&
         (dest_ip & config.netmask) == (config.ip & config.netmask))) {
        kmemcpy(mac_out, broadcast_mac, ETH_ALEN);
        return 1;
    }

    uint32_t hop = dest_ip;
    if (config.netmask &&
        (dest_ip & config.netmask) != (config.ip & config.netmask)) {
        if (!config.gateway) {
            stats.dropped_no_route++;
            return 0;
        }
        hop = config.gateway;
    }
    return net_arp_lookup(hop, mac_out);
}

int net_send_ip(uint32_t dest_ip, uint8_t protocol,
                const uint8_t* payload, uint32_t len) {
    static uint8_t packet[ETH_MTU];
    uint8_t mac[ETH_ALEN];

    if (!dev) return -1;
    if (len + sizeof(ip_header_t) > ETH_MTU) return -1;
    if (!next_hop_mac(dest_ip, mac)) return -2;   /* ARP pending: retry */

    ip_header_t* ih = (ip_header_t*)packet;
    ih->version_ihl = 0x45;         /* IPv4, 5 dwords of header */
    ih->tos = 0;
    ih->total_length = htons((uint16_t)(sizeof(ip_header_t) + len));
    ih->id = htons(ip_id++);
    ih->flags_fragment = htons(0x4000);   /* don't fragment */
    ih->ttl = 64;
    ih->protocol = protocol;
    ih->checksum = 0;
    ih->src = htonl(config.ip);
    ih->dest = htonl(dest_ip);
    ih->checksum = htons(net_checksum(ih, sizeof(*ih), 0));

    kmemcpy(packet + sizeof(ip_header_t), payload, len);

    int r = eth_send(mac, ETH_P_IP, packet, sizeof(ip_header_t) + len);
    if (r == 0) stats.tx_ip++;
    return r;
}

int net_send_udp(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
                 const uint8_t* payload, uint32_t len) {
    static uint8_t datagram[ETH_MTU];

    if (len > NET_UDP_MAX_PAYLOAD) return -1;

    udp_header_t* uh = (udp_header_t*)datagram;
    uh->src_port = htons(src_port);
    uh->dest_port = htons(dest_port);
    uh->length = htons((uint16_t)(sizeof(udp_header_t) + len));
    /* Zero, which UDP over IPv4 explicitly allows and means "not
     * computed". IP's own header checksum still covers the addresses, and
     * Ethernet's CRC covers the payload. TCP will not have this luxury. */
    uh->checksum = 0;
    kmemcpy(datagram + sizeof(udp_header_t), payload, len);

    return net_send_ip(dest_ip, IP_PROTO_UDP, datagram,
                       sizeof(udp_header_t) + len);
}

/* --- UDP delivery ------------------------------------------------------- */

#define UDP_PORTS 8
#define UDP_QUEUE 4

typedef struct {
    uint16_t port;
    int      bound;
    udp_datagram_t q[UDP_QUEUE];
    uint32_t head, tail;
} udp_port_t;

static udp_port_t udp_ports[UDP_PORTS];

int net_udp_bind(uint16_t port) {
    for (int i = 0; i < UDP_PORTS; i++) {
        if (udp_ports[i].bound && udp_ports[i].port == port) return 1;
    }
    for (int i = 0; i < UDP_PORTS; i++) {
        if (udp_ports[i].bound) continue;
        udp_ports[i].port = port;
        udp_ports[i].bound = 1;
        udp_ports[i].head = udp_ports[i].tail = 0;
        return 1;
    }
    return 0;
}

void net_udp_unbind(uint16_t port) {
    for (int i = 0; i < UDP_PORTS; i++) {
        if (udp_ports[i].bound && udp_ports[i].port == port) {
            udp_ports[i].bound = 0;
            return;
        }
    }
}

int net_udp_recv(uint16_t port, udp_datagram_t* out) {
    for (int i = 0; i < UDP_PORTS; i++) {
        udp_port_t* p = &udp_ports[i];
        if (!p->bound || p->port != port) continue;
        if (p->tail == p->head) return 0;
        *out = p->q[p->tail];
        p->tail = (p->tail + 1) % UDP_QUEUE;
        return 1;
    }
    return 0;
}

static void udp_deliver(uint32_t src_ip, uint16_t src_port, uint16_t dest_port,
                        const uint8_t* data, uint32_t len) {
    for (int i = 0; i < UDP_PORTS; i++) {
        udp_port_t* p = &udp_ports[i];
        if (!p->bound || p->port != dest_port) continue;

        uint32_t next = (p->head + 1) % UDP_QUEUE;
        if (next == p->tail) return;         /* full: drop the newest */
        if (len > NET_UDP_MAX_PAYLOAD) len = NET_UDP_MAX_PAYLOAD;

        p->q[p->head].src_ip = src_ip;
        p->q[p->head].src_port = src_port;
        p->q[p->head].len = (uint16_t)len;
        kmemcpy(p->q[p->head].data, data, len);
        p->head = next;
        return;
    }
}

/* --- ICMP --------------------------------------------------------------- */

static uint32_t ping_replies;
static uint32_t ping_sent_tick;
static uint32_t ping_last_ticks;

int net_ping(uint32_t dest_ip, uint16_t seq) {
    uint8_t packet[sizeof(icmp_header_t) + 32];
    icmp_header_t* ih = (icmp_header_t*)packet;

    ih->type = 8;                  /* echo request */
    ih->code = 0;
    ih->checksum = 0;
    ih->id = htons(0x4E56);        /* 'NV' */
    ih->seq = htons(seq);
    for (uint32_t i = 0; i < 32; i++) {
        packet[sizeof(icmp_header_t) + i] = (uint8_t)('a' + (i % 26));
    }
    ih->checksum = htons(net_checksum(packet, sizeof(packet), 0));

    ping_sent_tick = pit_get_ticks();
    return net_send_ip(dest_ip, IP_PROTO_ICMP, packet, sizeof(packet));
}

uint32_t net_ping_replies(void) { return ping_replies; }
uint32_t net_ping_last_ticks(void) { return ping_last_ticks; }

static void icmp_receive(uint32_t src_ip, const uint8_t* data, uint32_t len) {
    if (len < sizeof(icmp_header_t)) return;
    const icmp_header_t* ih = (const icmp_header_t*)data;

    if (ih->type == 0) {                   /* echo reply */
        ping_replies++;
        ping_last_ticks = pit_get_ticks() - ping_sent_tick;
        return;
    }

    if (ih->type == 8) {                   /* echo request: answer it */
        static uint8_t reply[ETH_MTU];
        if (len > sizeof(reply)) return;
        kmemcpy(reply, data, len);
        icmp_header_t* rh = (icmp_header_t*)reply;
        rh->type = 0;
        rh->checksum = 0;
        rh->checksum = htons(net_checksum(reply, len, 0));
        net_send_ip(src_ip, IP_PROTO_ICMP, reply, len);
    }
}

/* --- receiving ---------------------------------------------------------- */

/* TCP lives in its own file and registers itself here, so that a kernel
 * built without it links. */
static void (*tcp_input)(uint32_t src_ip, uint32_t dest_ip,
                         const uint8_t* seg, uint32_t len);

void net_register_tcp(void (*fn)(uint32_t, uint32_t, const uint8_t*, uint32_t));
void net_register_tcp(void (*fn)(uint32_t, uint32_t, const uint8_t*, uint32_t)) {
    tcp_input = fn;
}

static void arp_receive(const uint8_t* data, uint32_t len) {
    if (len < sizeof(arp_packet_t)) return;
    const arp_packet_t* a = (const arp_packet_t*)data;

    if (ntohs(a->ptype) != ETH_P_IP || a->plen != 4) return;

    uint32_t sender = ntohl(a->sender_ip);
    uint32_t target = ntohl(a->target_ip);

    /* Learn from every ARP that crosses the wire, request or reply -
     * which is what every implementation does and is why a host that has
     * just been asked for its address rarely has to ask back. */
    if (sender) arp_store(sender, a->sender_mac);

    if (ntohs(a->oper) == 1 && config.configured && target == config.ip) {
        arp_packet_t r;
        r.htype = htons(1);
        r.ptype = htons(ETH_P_IP);
        r.hlen = ETH_ALEN;
        r.plen = 4;
        r.oper = htons(2);         /* reply */
        kmemcpy(r.sender_mac, dev->mac, ETH_ALEN);
        r.sender_ip = htonl(config.ip);
        kmemcpy(r.target_mac, a->sender_mac, ETH_ALEN);
        r.target_ip = a->sender_ip;
        eth_send(a->sender_mac, ETH_P_ARP, (const uint8_t*)&r, sizeof(r));
        stats.tx_arp++;
    }
}

static void ip_receive(const uint8_t* data, uint32_t len) {
    if (len < sizeof(ip_header_t)) return;
    const ip_header_t* ih = (const ip_header_t*)data;

    if ((ih->version_ihl >> 4) != 4) return;
    uint32_t hdr_len = (uint32_t)(ih->version_ihl & 0x0F) * 4;
    if (hdr_len < sizeof(ip_header_t) || hdr_len > len) return;

    if (net_checksum(ih, hdr_len, 0) != 0) {
        stats.dropped_checksum++;
        return;
    }

    uint32_t total = ntohs(ih->total_length);
    if (total < hdr_len || total > len) return;

    uint32_t src = ntohl(ih->src);
    uint32_t dest = ntohl(ih->dest);

    /* Ours, broadcast, or not for us. No forwarding: this is a host. */
    if (config.configured && dest != config.ip && dest != 0xFFFFFFFFu &&
        !(config.netmask && (dest | ~config.netmask) == 0xFFFFFFFFu)) {
        return;
    }

    const uint8_t* payload = data + hdr_len;
    uint32_t payload_len = total - hdr_len;

    switch (ih->protocol) {
        case IP_PROTO_ICMP:
            stats.rx_icmp++;
            icmp_receive(src, payload, payload_len);
            break;
        case IP_PROTO_UDP: {
            stats.rx_udp++;
            if (payload_len < sizeof(udp_header_t)) break;
            const udp_header_t* uh = (const udp_header_t*)payload;
            uint32_t ulen = ntohs(uh->length);
            if (ulen < sizeof(udp_header_t) || ulen > payload_len) break;
            udp_deliver(src, ntohs(uh->src_port), ntohs(uh->dest_port),
                        payload + sizeof(udp_header_t),
                        ulen - sizeof(udp_header_t));
            break;
        }
        case IP_PROTO_TCP:
            stats.rx_tcp++;
            if (tcp_input) tcp_input(src, dest, payload, payload_len);
            break;
        default:
            stats.rx_other++;
            break;
    }
}

void netdev_receive(netdev_t* d, const uint8_t* frame, uint32_t len) {
    if (len < ETH_HDR_LEN) return;

    d->rx_packets++;
    d->rx_bytes += len;
    stats.rx_frames++;

    const eth_header_t* eh = (const eth_header_t*)frame;
    const uint8_t* payload = frame + ETH_HDR_LEN;
    uint32_t payload_len = len - ETH_HDR_LEN;

    switch (ntohs(eh->type)) {
        case ETH_P_ARP:
            stats.rx_arp++;
            arp_receive(payload, payload_len);
            break;
        case ETH_P_IP:
            stats.rx_ip++;
            ip_receive(payload, payload_len);
            break;
        default:
            stats.rx_other++;
            break;
    }
}

/* --- the device register ------------------------------------------------ */

void netdev_register(netdev_t* d) { dev = d; }
netdev_t* netdev_get(void) { return dev; }

/* --- lifecycle ---------------------------------------------------------- */

const net_config_t* net_config(void) { return &config; }
const net_stats_t* net_stats(void) { return &stats; }

void net_set_config(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns) {
    config.ip = ip;
    config.netmask = mask;
    config.gateway = gw;
    config.dns = dns;
    config.configured = ip != 0;
}

int net_available(void) { return dev != 0 && dev->up; }

int net_init(void) {
    if (!rtl8139_init()) return 0;
    return net_available();
}

void net_poll(void) {
    rtl8139_poll();
}
