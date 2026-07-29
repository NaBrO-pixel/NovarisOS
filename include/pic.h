#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/* The legacy 8259 PIC (Programmable Interrupt Controller) defaults to
 * mapping IRQs 0-15 onto interrupt vectors 0x08-0x0F and 0x70-0x77, which
 * collides with the CPU's own exception vectors 0-31. remap() moves them
 * out of the way to 32-47 before we ever enable interrupts. */
void pic_remap(uint8_t offset1, uint8_t offset2);

/* Every IRQ handler must send an End-Of-Interrupt to the PIC(s) before
 * returning, or the controller will never signal that IRQ (or, for
 * IRQs 8-15, any IRQ on the slave) again. */
void pic_send_eoi(uint8_t irq);

/* Clears an IRQ's mask bit so the PIC actually delivers it. IRQ0/1 work
 * today because the BIOS happens to leave them unmasked by default, but
 * IRQ12 (PS/2 mouse) and its cascade line (IRQ2) generally don't -
 * mouse_install() calls this explicitly rather than relying on that
 * default holding. */
void pic_unmask_irq(uint8_t irq);

#endif
