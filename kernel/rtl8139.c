/* rtl8139.c - a driver for the Realtek RTL8139, the network card.
 *
 * Milestone 38. Chosen over the e1000 and virtio-net for one reason: it
 * is the smallest real Ethernet controller. Receive is a single circular
 * buffer the card writes into and the driver reads out of, transmit is
 * four registers and four buffers used round-robin, and there are no
 * descriptor rings on either side. Every other card worth having is
 * better hardware and more code, and the thing being built here is the IP
 * stack above it.
 *
 * The receive path is the part worth reading twice. The card is given one
 * physically contiguous buffer and writes frames into it end to end,
 * wrapping at the end - so a frame can straddle the wrap, which is why
 * the buffer is over-allocated by 1500 bytes and the WRAP bit is set: the
 * card is then allowed to run past the end into the slack rather than
 * splitting a frame in two. Each frame is preceded by a 4-byte header of
 * status and length. CAPR - the "current address of packet read" register
 * - is how the driver tells the card how far it has consumed, and it is
 * biased by 16 for reasons lost to history and preserved by every driver
 * that has ever worked.
 */

#include "rtl8139.h"
#include "netdev.h"
#include "pci.h"
#include "paging.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "kstring.h"
#include "console.h"
#include "vga_text.h"

/* Realtek's vendor id, and the 8139's device id. QEMU's `-device rtl8139`
 * is this pair. */
#define RTL_VENDOR 0x10EC
#define RTL_DEVICE 0x8139

/* Registers, as offsets from the I/O base in BAR0. */
#define REG_MAC0        0x00   /* 6 bytes */
#define REG_MAR0        0x08   /* multicast filter, 8 bytes */
#define REG_TXSTATUS0   0x10   /* 4 of them, 4 bytes apart */
#define REG_TXADDR0     0x20   /* likewise */
#define REG_RXBUF       0x30   /* physical address of the receive ring */
#define REG_CAPR        0x38   /* how far the driver has read */
#define REG_CBR         0x3A   /* how far the card has written */
#define REG_CMD         0x37
#define REG_IMR         0x3C   /* interrupt mask */
#define REG_ISR         0x3E   /* interrupt status */
#define REG_TXCONFIG    0x40
#define REG_RXCONFIG    0x44
#define REG_CONFIG1     0x52

#define CMD_RESET       0x10
#define CMD_RX_ENABLE   0x08
#define CMD_TX_ENABLE   0x04
#define CMD_RX_EMPTY    0x01

#define ISR_ROK         0x0001  /* a frame arrived */
#define ISR_RER         0x0002
#define ISR_TOK         0x0004  /* a frame went out */
#define ISR_TER         0x0008

/* Accept broadcast, multicast, unicast-to-us and runt/error frames off,
 * with an 8KB ring, unlimited DMA burst and the WRAP bit. */
#define RXCFG_ACCEPT    0x0000000F
#define RXCFG_WRAP      0x00000080
#define RXCFG_8K        0x00000000  /* bits 12:11 = 00 -> 8K + 16 */
#define RXCFG_MXDMA     0x00000700  /* unlimited */
#define RXCFG_RXFTH     0x0000E000  /* no threshold: whole frames */

#define RX_BUF_LEN      8192
/* The slack the WRAP bit needs, plus the 16 bytes the card's own
 * bookkeeping wants past the end. */
#define RX_BUF_PAD      (16 + 1536)

#define TX_BUFFERS      4
#define TX_BUF_SIZE     2048

typedef struct {
    uint16_t io;                /* the I/O base from BAR0 */

    uint8_t* rx;                /* the ring, in the DMA window */
    uint32_t rx_phys;
    uint32_t rx_offset;         /* how far this driver has read */

    uint8_t* tx[TX_BUFFERS];
    uint32_t tx_phys[TX_BUFFERS];
    uint32_t tx_next;           /* which of the four to use next */

    netdev_t dev;
} rtl8139_t;

static rtl8139_t nic;
static int present;

/* --- receive ------------------------------------------------------------ */

static void rx_drain(void) {
    /* CMD_RX_EMPTY clears when the card has written something the driver
     * has not read. Draining in a loop rather than one frame per
     * interrupt: the card coalesces, and a frame left in the ring is a
     * frame that arrives late by however long until the next one. */
    while (!(inb(nic.io + REG_CMD) & CMD_RX_EMPTY)) {
        uint8_t* p = nic.rx + nic.rx_offset;

        /* The 4-byte header the card puts in front of every frame:
         * 16 bits of status, then the length *including* the 4-byte CRC
         * the card checked and left on the end. */
        uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t length = (uint16_t)(p[2] | (p[3] << 8));

        if (!(status & 0x0001) || length < 4 || length > ETH_FRAME_MAX + 4) {
            /* A bad frame means the ring's framing cannot be trusted any
             * further - the next header is wherever this length said it
             * was. Reset the receiver rather than walk off into it. */
            nic.dev.rx_errors++;
            outb(nic.io + REG_CMD, CMD_TX_ENABLE);
            outl(nic.io + REG_RXBUF, nic.rx_phys);
            outb(nic.io + REG_CMD, CMD_TX_ENABLE | CMD_RX_ENABLE);
            nic.rx_offset = 0;
            outw(nic.io + REG_CAPR, (uint16_t)-16);
            return;
        }

        netdev_receive(&nic.dev, p + 4, (uint32_t)(length - 4));

        /* Frames are dword-aligned in the ring, header included. */
        nic.rx_offset = (nic.rx_offset + length + 4 + 3) & ~3u;
        nic.rx_offset %= RX_BUF_LEN;

        /* The bias of 16. CAPR is "read pointer minus 16", and every
         * driver that works does this. */
        outw(nic.io + REG_CAPR, (uint16_t)(nic.rx_offset - 16));
    }
}

/* --- transmit ----------------------------------------------------------- */

static int rtl_send(netdev_t* dev, const uint8_t* frame, uint32_t len) {
    (void)dev;
    if (!present) return -1;
    if (len > TX_BUF_SIZE) return -1;

    /* Ethernet's minimum frame is 60 bytes before the CRC; the card does
     * not pad for us, and a switch would drop a runt. */
    uint32_t pad = len < 60 ? 60 - len : 0;

    uint32_t slot = nic.tx_next;
    uint32_t status_reg = nic.io + REG_TXSTATUS0 + slot * 4;

    /* Bit 13 (OWN) is set by the card when it has finished with the
     * buffer. Spinning here rather than queueing: four buffers deep is
     * already more than this stack has in flight, and a queue would need
     * a lock the interrupt handler also takes. */
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        if (inl(status_reg) & 0x2000) break;
        if (spin == 999999) {
            nic.dev.tx_errors++;
            return -1;
        }
    }

    kmemcpy(nic.tx[slot], frame, len);
    if (pad) kmemset(nic.tx[slot] + len, 0, pad);

    outl(nic.io + REG_TXADDR0 + slot * 4, nic.tx_phys[slot]);
    /* Writing the length starts the transmission. Bits 12:11 are the
     * early-transmit threshold; 0 means "send when the whole frame is in
     * the FIFO", which is what a driver that memcpy'd it already wants. */
    outl(status_reg, len + pad);

    nic.tx_next = (slot + 1) % TX_BUFFERS;
    nic.dev.tx_packets++;
    nic.dev.tx_bytes += len;
    return 0;
}

/* --- interrupt ---------------------------------------------------------- */

static void rtl_interrupt(registers_t* regs) {
    (void)regs;
    uint16_t status = inw(nic.io + REG_ISR);
    if (!status) return;          /* shared line, not ours */

    /* Acknowledged by writing the bits back, and *before* the work: a
     * frame that arrives while this handler runs must leave the bit set
     * again rather than be acknowledged away unseen. */
    outw(nic.io + REG_ISR, status);

    if (status & ISR_ROK) rx_drain();
    if (status & ISR_RER) nic.dev.rx_errors++;
    if (status & ISR_TER) nic.dev.tx_errors++;
}

/* --- bring-up ----------------------------------------------------------- */

int rtl8139_init(void) {
    const pci_device_t* pd = pci_find(RTL_VENDOR, RTL_DEVICE);
    if (!pd) return 0;

    uint32_t io = pci_bar_io(pd, 0);
    if (!io) return 0;

    pci_enable_bus_master(pd);
    nic.io = (uint16_t)io;

    /* Out of low-power mode. The card comes up with its transmitter and
     * receiver clocked down and answers nothing until this is written. */
    outb(nic.io + REG_CONFIG1, 0x00);

    /* Software reset, then wait for the card to clear the bit. */
    outb(nic.io + REG_CMD, CMD_RESET);
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        if (!(inb(nic.io + REG_CMD) & CMD_RESET)) break;
    }

    /* The buffers. Physically contiguous and physically addressed,
     * because the card does the addressing - see paging_alloc_dma(). */
    nic.rx = (uint8_t*)paging_alloc_dma(RX_BUF_LEN + RX_BUF_PAD, &nic.rx_phys);
    if (!nic.rx) return 0;
    kmemset(nic.rx, 0, RX_BUF_LEN + RX_BUF_PAD);

    for (int i = 0; i < TX_BUFFERS; i++) {
        nic.tx[i] = (uint8_t*)paging_alloc_dma(TX_BUF_SIZE, &nic.tx_phys[i]);
        if (!nic.tx[i]) return 0;
    }

    outl(nic.io + REG_RXBUF, nic.rx_phys);

    /* Receive and transmit OK, and their error twins. Deliberately not
     * every bit the card can raise: a cable-unplugged interrupt this
     * kernel has nothing to do about is an interrupt storm. */
    outw(nic.io + REG_IMR, ISR_ROK | ISR_RER | ISR_TOK | ISR_TER);
    outl(nic.io + REG_RXCONFIG,
         RXCFG_ACCEPT | RXCFG_WRAP | RXCFG_8K | RXCFG_MXDMA | RXCFG_RXFTH);
    outl(nic.io + REG_TXCONFIG, 0x03000700);   /* default IFG, unlimited DMA */

    outb(nic.io + REG_CMD, CMD_TX_ENABLE | CMD_RX_ENABLE);

    for (int i = 0; i < ETH_ALEN; i++) {
        nic.dev.mac[i] = inb(nic.io + REG_MAC0 + i);
    }

    kstrcpy(nic.dev.name, "eth0");
    nic.dev.up = 1;
    nic.dev.send = rtl_send;
    nic.dev.driver = &nic;

    /* PCI interrupt lines land on the PIC's IRQ pins, remapped to vectors
     * 32-47 by pic.c. The card's line comes from its configuration
     * space - unlike every other device in this kernel, it is not a
     * number anyone can know in advance. */
    register_interrupt_handler(32 + pd->irq, rtl_interrupt);
    pic_unmask_irq(pd->irq);
    if (pd->irq >= 8) pic_unmask_irq(2);    /* the cascade line */

    present = 1;
    netdev_register(&nic.dev);
    return 1;
}

int rtl8139_present(void) { return present; }

uint16_t rtl8139_io_base(void) { return nic.io; }

uint8_t rtl8139_irq(void) {
    const pci_device_t* pd = pci_find(RTL_VENDOR, RTL_DEVICE);
    return pd ? pd->irq : 0;
}

/* Called from the stack's poll: the card's interrupt is the normal path,
 * but a kernel that spends minutes inside one shell command (a Wine run)
 * can leave the ring filling. Draining on demand costs one port read. */
void rtl8139_poll(void) {
    if (present) rx_drain();
}
