/* dhcp.c - getting an address from whoever is handing them out.
 *
 * Milestone 38. A DHCP client is four packets and a lot of constants:
 * DISCOVER goes out as a broadcast from 0.0.0.0, a server OFFERs an
 * address, the client REQUESTs that address (still broadcast, because
 * there may be two servers and the request has to tell both which offer
 * was taken), and the server ACKs.
 *
 * Kept deliberately dim. No lease renewal, no rebinding, no timers: this
 * asks once at boot and keeps what it is given until the machine is
 * switched off. On the network this kernel actually runs on - QEMU's
 * user-mode stack, which hands out a lease of a day and never revokes it
 * - the difference is not observable, and a renewal timer needs a
 * background task the kernel does not have.
 *
 * What is *not* dim: the transaction id is checked, the message type is
 * checked, and the options are walked with a bound rather than to the
 * end of the buffer. A DHCP client is the first thing on this machine
 * that parses bytes an outsider chose.
 */

#include "dhcp.h"
#include "net.h"
#include "netdev.h"
#include "kstring.h"
#include "pit.h"
#include "console.h"
#include "vga_text.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define BOOTREQUEST 1
#define BOOTREPLY   2

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define OPT_SUBNET_MASK   1
#define OPT_ROUTER        3
#define OPT_DNS           6
#define OPT_REQUESTED_IP  50
#define OPT_MESSAGE_TYPE  53
#define OPT_SERVER_ID     54
#define OPT_PARAM_REQUEST 55
#define OPT_END           255

typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[312];
} dhcp_message_t;

/* The four bytes that say "these are DHCP options, not BOOTP vendor
 * data". 99.130.83.99, and it has been that since RFC 1497. */
#define DHCP_MAGIC 0x63825363u

static uint32_t xid;

static uint8_t* put_option(uint8_t* p, uint8_t code, uint8_t len,
                           const void* value) {
    *p++ = code;
    *p++ = len;
    if (len) {
        kmemcpy(p, value, len);
        p += len;
    }
    return p;
}

static void build(dhcp_message_t* m, const uint8_t* mac, uint8_t type,
                  uint32_t requested_ip, uint32_t server_id) {
    kmemset(m, 0, sizeof(*m));

    m->op = BOOTREQUEST;
    m->htype = 1;                  /* Ethernet */
    m->hlen = ETH_ALEN;
    m->xid = htonl(xid);
    /* The broadcast flag. A client with no address yet cannot receive a
     * unicast reply - it would have to answer an ARP for an address it
     * does not have - so it asks the server to broadcast instead. */
    m->flags = htons(0x8000);
    kmemcpy(m->chaddr, mac, ETH_ALEN);
    m->magic = htonl(DHCP_MAGIC);

    uint8_t* p = m->options;
    p = put_option(p, OPT_MESSAGE_TYPE, 1, &type);

    if (requested_ip) {
        uint32_t v = htonl(requested_ip);
        p = put_option(p, OPT_REQUESTED_IP, 4, &v);
    }
    if (server_id) {
        uint32_t v = htonl(server_id);
        p = put_option(p, OPT_SERVER_ID, 4, &v);
    }

    static const uint8_t wanted[] = { OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS };
    p = put_option(p, OPT_PARAM_REQUEST, sizeof(wanted), wanted);

    *p = OPT_END;
}

/* Walks the options of a reply. Every read is bounded by the length the
 * datagram actually arrived with, not by the 312-byte field. */
static int parse(const dhcp_message_t* m, uint32_t len, uint8_t* type_out,
                 uint32_t* mask_out, uint32_t* gw_out, uint32_t* dns_out,
                 uint32_t* server_out) {
    if (len < sizeof(dhcp_message_t) - sizeof(m->options) + 4) return 0;
    if (m->op != BOOTREPLY) return 0;
    if (ntohl(m->magic) != DHCP_MAGIC) return 0;
    if (ntohl(m->xid) != xid) return 0;

    uint32_t options_len = len - (uint32_t)(sizeof(dhcp_message_t) - sizeof(m->options));
    if (options_len > sizeof(m->options)) options_len = sizeof(m->options);

    *type_out = 0;
    uint32_t i = 0;
    while (i < options_len) {
        uint8_t code = m->options[i++];
        if (code == OPT_END) break;
        if (code == 0) continue;               /* pad */
        if (i >= options_len) break;
        uint8_t olen = m->options[i++];
        if (i + olen > options_len) break;

        const uint8_t* v = &m->options[i];
        switch (code) {
            case OPT_MESSAGE_TYPE: if (olen >= 1) *type_out = v[0]; break;
            case OPT_SUBNET_MASK:  if (olen >= 4) *mask_out = ntohl(*(const uint32_t*)v); break;
            case OPT_ROUTER:       if (olen >= 4) *gw_out = ntohl(*(const uint32_t*)v); break;
            case OPT_DNS:          if (olen >= 4) *dns_out = ntohl(*(const uint32_t*)v); break;
            case OPT_SERVER_ID:    if (olen >= 4) *server_out = ntohl(*(const uint32_t*)v); break;
            default: break;
        }
        i += olen;
    }

    return *type_out != 0;
}

/* Waits up to `ticks` for a reply of the wanted type. Polls rather than
 * blocks - see the note at the top of net.c about there being no timer
 * thread. */
static int wait_for(uint8_t want, uint32_t ticks, dhcp_message_t* out,
                    uint32_t* mask, uint32_t* gw, uint32_t* dns,
                    uint32_t* server) {
    uint32_t deadline = pit_get_ticks() + ticks;
    static udp_datagram_t dg;

    while (pit_get_ticks() < deadline) {
        net_poll();
        if (!net_udp_recv(DHCP_CLIENT_PORT, &dg)) {
            __asm__ __volatile__("hlt");
            continue;
        }

        uint8_t type = 0;
        if (!parse((const dhcp_message_t*)dg.data, dg.len, &type,
                   mask, gw, dns, server)) {
            continue;
        }
        if (type != want) continue;

        kmemcpy(out, dg.data, dg.len < sizeof(*out) ? dg.len : sizeof(*out));
        return 1;
    }
    return 0;
}

int dhcp_configure(uint32_t timeout_ticks) {
    netdev_t* dev = netdev_get();
    if (!dev) return 0;

    static dhcp_message_t msg;
    static dhcp_message_t reply;

    /* A transaction id nothing else will pick. The tick count is not a
     * random number, and does not have to be: the only requirement is
     * that a reply to *this* client's request is distinguishable from a
     * reply to somebody else's. */
    xid = 0x4E560000u | (pit_get_ticks() & 0xFFFF);

    if (!net_udp_bind(DHCP_CLIENT_PORT)) return 0;

    /* Sending from 0.0.0.0 to 255.255.255.255, which is the whole reason
     * DHCP works before there is an address: the request is a broadcast
     * frame and needs no ARP. */
    net_set_config(0, 0, 0, 0);

    uint32_t mask = 0, gw = 0, dns = 0, server = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        build(&msg, dev->mac, DHCP_DISCOVER, 0, 0);
        net_send_udp(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                     (const uint8_t*)&msg, sizeof(msg));

        if (!wait_for(DHCP_OFFER, timeout_ticks / 3, &reply,
                      &mask, &gw, &dns, &server)) {
            continue;
        }

        uint32_t offered = ntohl(reply.yiaddr);
        if (!offered) continue;

        build(&msg, dev->mac, DHCP_REQUEST, offered, server);
        net_send_udp(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                     (const uint8_t*)&msg, sizeof(msg));

        if (!wait_for(DHCP_ACK, timeout_ticks / 3, &reply,
                      &mask, &gw, &dns, &server)) {
            continue;
        }

        uint32_t ip = ntohl(reply.yiaddr);
        if (!ip) continue;

        /* A server that offers no mask is offering a /24, which is what
         * every DHCP server on a small network hands out anyway. */
        if (!mask) mask = 0xFFFFFF00u;

        net_set_config(ip, mask, gw, dns);
        net_udp_unbind(DHCP_CLIENT_PORT);
        return 1;
    }

    net_udp_unbind(DHCP_CLIENT_PORT);
    return 0;
}
