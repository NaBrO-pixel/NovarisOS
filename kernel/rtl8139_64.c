/* rtl8139_64.c - the RTL8139 for the 64-bit kernel.
 *
 * The same card and the same register dance as kernel/rtl8139.c, which
 * the 32-bit kernel uses; two things differ enough to make sharing the
 * file more trouble than it saves.
 *
 * Memory. The card is told physical addresses, and 32-bit ones. The
 * 32-bit kernel has paging_alloc_dma for that; this one has a frame
 * allocator that hands out single frames, anywhere. So the ring and the
 * four transmit buffers are static arrays in the kernel image instead:
 * the image is linked to sit at a fixed physical address below 16MB
 * (linker64.ld - physical = virtual - KERNEL_VMA), which makes them
 * contiguous, below 4GB and known at compile time.
 *
 * Interrupts. None. The card's line would have to be routed through the
 * 8259s, and nothing would gain by it: the stack is polled from the 1kHz
 * timer (inet64_timer) and from the system calls that use it, so a frame
 * waits at most a millisecond. The status register is still read and
 * acknowledged on every poll, because the overflow bits stop the
 * receiver until somebody does.
 *
 * Every entry point runs with interrupts off - from a system call, which
 * the SYSCALL mask keeps that way, or from the timer interrupt - so the
 * ring has one walker at a time without any locking here. */

#include <stdint.h>
#include "rtl8139.h"
#include "netdev.h"
#include "pci.h"
#include "io.h"
#include "kstring.h"

#define RTL_VENDOR 0x10EC
#define RTL_DEVICE 0x8139

#define REG_MAC0        0x00
#define REG_TXSTATUS0   0x10
#define REG_TXADDR0     0x20
#define REG_RXBUF       0x30
#define REG_CAPR        0x38
#define REG_CMD         0x37
#define REG_IMR         0x3C
#define REG_ISR         0x3E
#define REG_TXCONFIG    0x40
#define REG_RXCONFIG    0x44
#define REG_CONFIG1     0x52

#define CMD_RESET       0x10
#define CMD_RX_ENABLE   0x08
#define CMD_TX_ENABLE   0x04
#define CMD_RX_EMPTY    0x01

#define ISR_RER         0x0002
#define ISR_TER         0x0008
#define ISR_RXOVW       0x0010
#define ISR_FOVW        0x0040

#define RXCFG_ACCEPT    0x0000000F
#define RXCFG_WRAP      0x00000080
#define RXCFG_32K       0x00001000
#define RXCFG_MXDMA     0x00000700
#define RXCFG_RXFTH     0x0000E000

#define RX_BUF_LEN      32768
#define RX_BUF_PAD      (16 + 1536)
#define TX_BUFFERS      4
#define TX_BUF_SIZE     2048

#define KERNEL_VMA_     0xFFFFFFFF80000000ULL

static uint8_t rx_ring[RX_BUF_LEN + RX_BUF_PAD] __attribute__((aligned(4096)));
static uint8_t tx_bufs[TX_BUFFERS][TX_BUF_SIZE] __attribute__((aligned(4096)));

static struct {
    uint16_t io;
    uint32_t rx_phys;
    uint32_t rx_offset;
    uint32_t tx_phys[TX_BUFFERS];
    uint32_t tx_next;
    netdev_t dev;
} nic;

static int present;
static uint32_t stat_poll_frames, stat_tx_busy;

/* The physical address of something in the kernel image. */
static uint32_t image_phys(const void* p) {
    return (uint32_t)((uint64_t)p - KERNEL_VMA_);
}

static void rx_reset(void) {
    outb(nic.io + REG_CMD, CMD_TX_ENABLE);
    outl(nic.io + REG_RXBUF, nic.rx_phys);
    outb(nic.io + REG_CMD, CMD_TX_ENABLE | CMD_RX_ENABLE);
    nic.rx_offset = 0;
    outw(nic.io + REG_CAPR, (uint16_t)-16);
}

static void rx_drain(void) {
    /* Bounded, so a flood cannot hold the timer interrupt forever: what
     * is left is there for the next poll. */
    for (int budget = 0; budget < 64; budget++) {
        if (inb(nic.io + REG_CMD) & CMD_RX_EMPTY) return;

        uint8_t* p = rx_ring + nic.rx_offset;
        uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t length = (uint16_t)(p[2] | (p[3] << 8));

        if (!(status & 0x0001) || length < 4 || length > ETH_FRAME_MAX + 4) {
            nic.dev.rx_errors++;
            rx_reset();
            return;
        }

        stat_poll_frames++;
        netdev_receive(&nic.dev, p + 4, (uint32_t)(length - 4));

        nic.rx_offset = (nic.rx_offset + length + 4 + 3) & ~3u;
        nic.rx_offset %= RX_BUF_LEN;
        outw(nic.io + REG_CAPR, (uint16_t)(nic.rx_offset - 16));
    }
}

static int rtl_send(netdev_t* dev, const uint8_t* frame, uint32_t len) {
    (void)dev;
    if (!present || len > TX_BUF_SIZE) return -1;

    uint32_t pad = len < 60 ? 60 - len : 0;
    uint32_t slot = 0, status_reg = 0;

    for (uint32_t i = 0; i < TX_BUFFERS; i++) {
        uint32_t s = (nic.tx_next + i) % TX_BUFFERS;
        uint32_t reg = nic.io + REG_TXSTATUS0 + s * 4;
        if (inl(reg) & 0x2000) { slot = s; status_reg = reg; break; }
    }
    /* All four still owned by the card: dropped, as kernel/rtl8139.c
     * explains - TCP retransmits, and waiting here would be waiting
     * inside the timer interrupt. */
    if (!status_reg) {
        stat_tx_busy++;
        nic.dev.tx_errors++;
        return -1;
    }

    kmemcpy(tx_bufs[slot], frame, len);
    if (pad) kmemset(tx_bufs[slot] + len, 0, pad);
    outl(nic.io + REG_TXADDR0 + slot * 4, nic.tx_phys[slot]);
    outl(status_reg, len + pad);

    nic.tx_next = (slot + 1) % TX_BUFFERS;
    nic.dev.tx_packets++;
    nic.dev.tx_bytes += len;
    return 0;
}

int rtl8139_init(void) {
    if (present) return 1;

    pci_init();
    const pci_device_t* pd = pci_find(RTL_VENDOR, RTL_DEVICE);
    if (!pd) return 0;

    uint32_t io = pci_bar_io(pd, 0);
    if (!io) return 0;

    pci_enable_bus_master(pd);
    nic.io = (uint16_t)io;

    outb(nic.io + REG_CONFIG1, 0x00);
    outb(nic.io + REG_CMD, CMD_RESET);
    for (uint32_t spin = 0; spin < 1000000; spin++)
        if (!(inb(nic.io + REG_CMD) & CMD_RESET)) break;

    kmemset(rx_ring, 0, sizeof(rx_ring));
    nic.rx_phys = image_phys(rx_ring);
    for (int i = 0; i < TX_BUFFERS; i++) nic.tx_phys[i] = image_phys(tx_bufs[i]);

    outl(nic.io + REG_RXBUF, nic.rx_phys);
    outw(nic.io + REG_IMR, 0);                 /* polled: no interrupts */
    outl(nic.io + REG_RXCONFIG,
         RXCFG_ACCEPT | RXCFG_WRAP | RXCFG_32K | RXCFG_MXDMA | RXCFG_RXFTH);
    outl(nic.io + REG_TXCONFIG, 0x03000700);
    outb(nic.io + REG_CMD, CMD_TX_ENABLE | CMD_RX_ENABLE);

    for (int i = 0; i < ETH_ALEN; i++) nic.dev.mac[i] = inb(nic.io + REG_MAC0 + i);

    kstrlcpy(nic.dev.name, "eth0", sizeof(nic.dev.name));
    nic.dev.up = 1;
    nic.dev.send = rtl_send;
    nic.dev.driver = &nic;

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

void rtl8139_poll(void) {
    uint16_t status;

    if (!present) return;

    /* Acknowledged before the drain, so a frame that lands during it
     * leaves its bit set for the next poll. */
    status = inw(nic.io + REG_ISR);
    if (status) outw(nic.io + REG_ISR, status);
    if (status & ISR_RER) nic.dev.rx_errors++;
    if (status & ISR_TER) nic.dev.tx_errors++;

    rx_drain();

    if (status & (ISR_RXOVW | ISR_FOVW)) {
        nic.dev.rx_dropped++;
        outw(nic.io + REG_CAPR, (uint16_t)(nic.rx_offset - 16));
    }
}

uint32_t rtl8139_irq_count(void)   { return 0; }
uint32_t rtl8139_irq_frames(void)  { return 0; }
uint32_t rtl8139_poll_frames(void) { return stat_poll_frames; }
uint32_t rtl8139_tx_busy(void)     { return stat_tx_busy; }
