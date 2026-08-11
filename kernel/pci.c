/* pci.c - the PCI bus, enumerated once at boot.
 *
 * See include/pci.h for why this exists at all. The whole of it is two
 * ports: write a bus/slot/function/register address to 0xCF8 with the
 * top bit set, then read or write the doubleword at 0xCFC.
 *
 * The one subtlety is that a slot with nothing in it reads back
 * 0xFFFFFFFF - the bus floats high with no device driving it - which is
 * why "vendor == 0xFFFF" is the test for absence rather than a value
 * anybody chose.
 */

#include "pci.h"
#include "io.h"
#include "console.h"
#include "vga_text.h"

#define PCI_MAX_DEVICES 32

static pci_device_t devices[PCI_MAX_DEVICES];
static uint32_t device_count;

static uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t off) {
    /* Bit 31 enables the transaction; bits 1:0 of the offset are always
     * zero because the data port is a doubleword window. */
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)(slot & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | (off & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDRESS, config_address(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t dword = pci_config_read32(bus, slot, func, off);
    return (uint16_t)((dword >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t dword = pci_config_read32(bus, slot, func, off);
    return (uint8_t)((dword >> ((off & 3) * 8)) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t off, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, config_address(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t off, uint16_t value) {
    /* Read-modify-write, because the data port is a doubleword window and
     * writing a word means writing the other word back unchanged. */
    uint32_t dword = pci_config_read32(bus, slot, func, off);
    uint32_t shift = (off & 2) * 8;
    dword = (dword & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    pci_config_write32(bus, slot, func, off, dword);
}

static void record(uint8_t bus, uint8_t slot, uint8_t func, uint16_t vendor) {
    if (device_count >= PCI_MAX_DEVICES) return;

    pci_device_t* d = &devices[device_count++];
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = vendor;
    d->device = pci_config_read16(bus, slot, func, PCI_DEVICE_ID);
    d->class_code = pci_config_read8(bus, slot, func, PCI_CLASS);
    d->subclass = pci_config_read8(bus, slot, func, PCI_SUBCLASS);
    d->prog_if = pci_config_read8(bus, slot, func, PCI_PROG_IF);
    d->irq = pci_config_read8(bus, slot, func, PCI_INTERRUPT_LINE);
    d->bar[0] = pci_config_read32(bus, slot, func, PCI_BAR0);
    d->bar[1] = pci_config_read32(bus, slot, func, PCI_BAR1);
}

uint32_t pci_init(void) {
    device_count = 0;

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_config_read16((uint8_t)bus, (uint8_t)slot, 0,
                                                PCI_VENDOR_ID);
            if (vendor == 0xFFFF) continue;

            record((uint8_t)bus, (uint8_t)slot, 0, vendor);

            /* Bit 7 of the header type says the device has more than one
             * function; without it, function 0 is the whole device and
             * probing 1-7 would find aliases of it. */
            uint8_t header = pci_config_read8((uint8_t)bus, (uint8_t)slot, 0,
                                              PCI_HEADER_TYPE);
            if (!(header & 0x80)) continue;

            for (uint32_t func = 1; func < 8; func++) {
                uint16_t fv = pci_config_read16((uint8_t)bus, (uint8_t)slot,
                                                (uint8_t)func, PCI_VENDOR_ID);
                if (fv != 0xFFFF) record((uint8_t)bus, (uint8_t)slot,
                                         (uint8_t)func, fv);
            }
        }
    }

    return device_count;
}

uint32_t pci_device_count(void) { return device_count; }

const pci_device_t* pci_device_at(uint32_t index) {
    return index < device_count ? &devices[index] : 0;
}

const pci_device_t* pci_find(uint16_t vendor, uint16_t device) {
    for (uint32_t i = 0; i < device_count; i++) {
        if (devices[i].vendor == vendor && devices[i].device == device) {
            return &devices[i];
        }
    }
    return 0;
}

void pci_enable_bus_master(const pci_device_t* dev) {
    if (!dev) return;
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_config_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

uint32_t pci_bar_io(const pci_device_t* dev, int which) {
    if (!dev || which < 0 || which > 1) return 0;
    uint32_t bar = dev->bar[which];
    if (!(bar & 1)) return 0;          /* a memory BAR, not an I/O one */
    return bar & ~0x3u;
}
