#ifndef NETDEV_H
#define NETDEV_H

#include <stdint.h>

/* netdev - what the network stack sees of a network card.
 *
 * Milestone 38. One driver exists (kernel/rtl8139.c) and this interface
 * has exactly one implementation, which is normally a reason not to have
 * an interface. It is here anyway for a specific reason: the layer above
 * has to be written against "a thing that sends and receives Ethernet
 * frames" rather than against the RTL8139's ring buffer, or the first
 * question a second card asks will be answered by rewriting the IP stack.
 *
 * A frame in, a frame out, and a MAC address. That is the whole of what
 * Ethernet needs from a card.
 */

#define ETH_ALEN      6
#define ETH_MTU       1500
#define ETH_HDR_LEN   14
#define ETH_FRAME_MAX (ETH_HDR_LEN + ETH_MTU)

typedef struct netdev {
    char     name[8];
    uint8_t  mac[ETH_ALEN];
    int      up;

    /* Hands a complete Ethernet frame to the card. Returns 0, or a
     * negative errno. Must be callable with interrupts on, from a
     * syscall or from the network stack's own timer work. */
    int (*send)(struct netdev* dev, const uint8_t* frame, uint32_t len);

    void*    driver;    /* the driver's own state */

    /* Counters, because a network that does not work looks identical to
     * one that is not plugged in until you can see whether anything went
     * out and whether anything came back. */
    uint32_t tx_packets, tx_bytes, tx_errors;
    uint32_t rx_packets, rx_bytes, rx_errors, rx_dropped;
} netdev_t;

/* Registers the one card. The stack has no notion of a second. */
void netdev_register(netdev_t* dev);
netdev_t* netdev_get(void);

/* Called by a driver from its interrupt handler, once per received
 * frame. The frame is only valid for the duration of the call - the
 * stack copies what it wants to keep. */
void netdev_receive(netdev_t* dev, const uint8_t* frame, uint32_t len);

#endif /* NETDEV_H */
