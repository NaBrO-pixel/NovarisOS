#include "pic.h"
#include "io.h"

#define PIC1_CMD   0x20   /* master PIC command port */
#define PIC1_DATA  0x21   /* master PIC data port */
#define PIC2_CMD   0xA0   /* slave PIC command port */
#define PIC2_DATA  0xA1   /* slave PIC data port */

#define PIC_EOI    0x20   /* End-Of-Interrupt command */

#define ICW1_INIT  0x10   /* initialization command */
#define ICW1_ICW4  0x01   /* ICW4 will be present */
#define ICW4_8086  0x01   /* 8086/88 (MCS-80/85) mode */

void pic_remap(uint8_t offset1, uint8_t offset2) {
    /* Save the interrupt masks - remapping resets them, we restore after. */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: start initialization sequence, tell each PIC to expect ICW4. */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vector offset - where each PIC's IRQ 0 lands in the IDT. */
    outb(PIC1_DATA, offset1); io_wait();
    outb(PIC2_DATA, offset2); io_wait();

    /* ICW3: tell master PIC there's a slave at IRQ2 (bit 2 = 0x04),
     * and tell the slave its own cascade identity (2). */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    /* ICW4: set 8086 mode on both. */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Restore saved masks instead of leaving everything unmasked. */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI); /* slave handled it too, ack it as well */
    }
    outb(PIC1_CMD, PIC_EOI);
}

void pic_unmask_irq(uint8_t irq) {
    uint16_t port = PIC1_DATA;
    if (irq >= 8) {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t mask = inb(port);
    mask &= (uint8_t)~(1u << irq);
    outb(port, mask);
}
