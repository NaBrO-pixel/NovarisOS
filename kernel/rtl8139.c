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
#define ISR_RXOVW       0x0010  /* the ring filled before it was drained */
#define ISR_FOVW        0x0040  /* the card's own FIFO filled */

/* Accept broadcast, multicast, unicast-to-us and runt/error frames off,
 * with an 8KB ring, unlimited DMA burst and the WRAP bit. */
#define RXCFG_ACCEPT    0x0000000F
#define RXCFG_WRAP      0x00000080
/* bits 12:11 = 10 -> 32K + 16.
 *
 * Not 64K, which the card also offers and which looks like the obvious
 * choice. CAPR and CBR - the read and write pointers the driver and the
 * card use to find each other in the ring - are both sixteen bits, so a
 * 64K ring makes "how far ahead is the card" ambiguous at exactly the
 * moment the ring is full or empty. 32K leaves the top bit spare and the
 * comparison unambiguous. Every driver that has ever worked reliably
 * uses 8K, 16K or 32K for this reason. */
#define RXCFG_32K       0x00001000
#define RXCFG_MXDMA     0x00000700  /* unlimited */
#define RXCFG_RXFTH     0x0000E000  /* no threshold: whole frames */

#define RX_BUF_LEN      32768
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

/* Milestone 39 diagnostics. A download that stalls looks the same from
 * outside whether the card never raised an interrupt or the driver never
 * drained the ring; these tell the two apart. */
static uint32_t stat_irqs, stat_irq_frames, stat_poll_frames, stat_tx_busy;
static int in_irq;

uint32_t rtl8139_irq_count(void)   { return stat_irqs; }
uint32_t rtl8139_irq_frames(void)  { return stat_irq_frames; }
uint32_t rtl8139_poll_frames(void) { return stat_poll_frames; }
uint32_t rtl8139_tx_busy(void)     { return stat_tx_busy; }

/* Interrupts off and back on again, saving whether they were on to begin
 * with. Both of the routines below are reached from inside the receive
 * interrupt as well as from the shell, and both touch state the other
 * half also touches.
 *
 * Saving and restoring rather than cli/sti, because the caller may
 * already be inside an interrupt handler: an unconditional sti there
 * re-enables interrupts in the middle of a handler that was entered with
 * them off, and the next thing that happens is the handler running inside
 * itself. */
static inline uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags) {
    if (flags & 0x200) __asm__ __volatile__("sti" ::: "memory");
}

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

        if (in_irq) stat_irq_frames++; else stat_poll_frames++;
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

    /* Claiming a transmit buffer runs with interrupts off. An ARP or ICMP
     * reply is sent from inside the receive interrupt, so this routine
     * can interrupt itself; two callers that both read "slot 0 is free"
     * both memcpy into slot 0 and the first frame never leaves. */
    uint32_t flags = irq_save();

    /* Round-robin, but skipping past a buffer the card still owns rather
     * than giving up on it. The four are interchangeable; insisting on
     * the next one in order turns one slow transmission into four
     * buffers' worth of dropped frames. */
    uint32_t slot = nic.tx_next;
    uint32_t status_reg = 0;
    for (uint32_t i = 0; i < TX_BUFFERS; i++) {
        uint32_t s = (nic.tx_next + i) % TX_BUFFERS;
        uint32_t reg = nic.io + REG_TXSTATUS0 + s * 4;
        if (inl(reg) & 0x2000) { slot = s; status_reg = reg; break; }
    }
    if (!status_reg) status_reg = nic.io + REG_TXSTATUS0 + slot * 4;

    /* Bit 13 (OWN) is set by the card when it has finished with the
     * buffer. If it is not set, all four buffers are busy - and this
     * *drops the frame* rather than waiting for one.
     *
     * That is not laziness, it is the fix for the bug that made a 200KB
     * download stop at 45KB. rtl_send() is reached from the interrupt
     * handler: a received segment is acknowledged, and the acknowledgement
     * is a frame. A send that spins waiting for a transmit buffer
     * therefore spins *inside the interrupt handler*, with the receive
     * ring filling behind it and further interrupts blocked - so the
     * cure for a busy transmitter was to stop receiving.
     *
     * Dropping is safe precisely because of what is being sent. A lost
     * acknowledgement is followed by another one; a lost data segment is
     * retransmitted. TCP is built for a link that drops things, and this
     * is a link that drops things. */
    if (!(inl(status_reg) & 0x2000)) {
        stat_tx_busy++;
        nic.dev.tx_errors++;
        irq_restore(flags);
        return -1;
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
    irq_restore(flags);
    return 0;
}

/* --- interrupt ---------------------------------------------------------- */

static void rtl_interrupt(registers_t* regs) {
    (void)regs;
    uint16_t status = inw(nic.io + REG_ISR);
    if (!status) return;          /* shared line, not ours */
    stat_irqs++;
    in_irq = 1;

    /* Acknowledged by writing the bits back, and *before* the work: a
     * frame that arrives while this handler runs must leave the bit set
     * again rather than be acknowledged away unseen. */
    outw(nic.io + REG_ISR, status);

    if (status & ISR_ROK) rx_drain();
    if (status & ISR_RER) nic.dev.rx_errors++;
    if (status & ISR_TER) nic.dev.tx_errors++;

    if (status & (ISR_RXOVW | ISR_FOVW)) {
        /* The ring filled. Whatever is in it is still framed correctly,
         * so drain it first - the frames are real - and then tell the
         * card where the read pointer is, which is what un-stalls the
         * receiver. */
        nic.dev.rx_dropped++;
        rx_drain();
        outw(nic.io + REG_CAPR, (uint16_t)(nic.rx_offset - 16));
    }
    in_irq = 0;
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
    /* Overflow included. Leaving it masked does not mean overflows do
     * not happen - it means they happen silently and the receiver stops,
     * which from userland is a download that stalls with no error
     * anywhere. */
    outw(nic.io + REG_IMR,
         ISR_ROK | ISR_RER | ISR_TOK | ISR_TER | ISR_RXOVW | ISR_FOVW);
    outl(nic.io + REG_RXCONFIG,
         RXCFG_ACCEPT | RXCFG_WRAP | RXCFG_32K | RXCFG_MXDMA | RXCFG_RXFTH);
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
 * can leave the ring filling. Draining on demand costs one port read.
 *
 * With interrupts held off for the duration, because rx_drain() is the
 * one routine in this driver reached from both sides. The ring has a
 * single read pointer; two walkers sharing it do not read the ring twice,
 * they read it wrong - one advances rx_offset past a header the other is
 * still standing on, and the loser then reads length and status out of
 * the middle of a frame's payload. That fails the sanity check, and the
 * sanity check resets the receiver and throws the whole ring away.
 *
 * Which is what a stalled download looks like from the outside: bytes
 * arrive, the driver discards them along with everything queued behind
 * them, and nothing moves again until the sender's retransmission timer
 * fires a second later. */
void rtl8139_poll(void) {
    if (!present) return;

    uint32_t flags = irq_save();
    rx_drain();
    irq_restore(flags);
}
