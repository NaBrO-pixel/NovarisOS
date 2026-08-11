#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* pci - finding devices on the PCI bus.
 *
 * Milestone 38. Every driver in this kernel until now talked to hardware
 * at an address someone else had already decided: the PIC at 0x20, the
 * PIT at 0x40, the keyboard at 0x60, ATA at 0x1F0. Those are the legacy
 * ports of the original PC, and they are the same on every machine
 * because they have to be.
 *
 * A network card has no such address. It is a PCI device, and where its
 * registers live is whatever the firmware decided at boot and wrote into
 * its configuration space. So before there can be a driver there has to
 * be a way to ask.
 *
 * This is the older of the two ways to ask - "configuration mechanism
 * #1", an address register at 0xCF8 and a data register at 0xCFC - and it
 * is the one that works everywhere. A brute-force scan of all 256 buses
 * rather than following bridges: it is 32768 doubleword reads, it happens
 * once at boot, and the alternative is a bridge walker that would be more
 * code than the driver it exists to find.
 */

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* The configuration-space registers this kernel reads. Offsets are from
 * the PCI 2.x specification's type-0 header. */
#define PCI_VENDOR_ID   0x00
#define PCI_DEVICE_ID   0x02
#define PCI_COMMAND     0x04
#define PCI_STATUS      0x06
#define PCI_REVISION    0x08
#define PCI_PROG_IF     0x09
#define PCI_SUBCLASS    0x0A
#define PCI_CLASS       0x0B
#define PCI_HEADER_TYPE 0x0E
#define PCI_BAR0        0x10
#define PCI_BAR1        0x14
#define PCI_INTERRUPT_LINE 0x3C

/* Command register bits. A device comes out of reset with its decoders
 * off; nothing works until these are set. */
#define PCI_CMD_IO_SPACE     0x0001
#define PCI_CMD_MEM_SPACE    0x0002
#define PCI_CMD_BUS_MASTER   0x0004

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  irq;
    uint32_t bar[2];
} pci_device_t;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t off, uint32_t value);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t off, uint16_t value);

/* Scans the bus once and remembers what it found. */
uint32_t pci_init(void);

/* How many devices the scan found, and each of them. */
uint32_t pci_device_count(void);
const pci_device_t* pci_device_at(uint32_t index);

/* The first device matching a vendor/device pair, or NULL. */
const pci_device_t* pci_find(uint16_t vendor, uint16_t device);

/* Turns on I/O space decoding and bus mastering, which is what a device
 * needs before it can be talked to and before it can DMA. */
void pci_enable_bus_master(const pci_device_t* dev);

/* The I/O port base from a BAR that is an I/O BAR (bit 0 set), or 0. */
uint32_t pci_bar_io(const pci_device_t* dev, int which);

#endif /* PCI_H */
