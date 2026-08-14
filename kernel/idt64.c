/* idt64.c - the long-mode interrupt descriptor table.
 *
 * The table is the same idea as the 32-bit one and a different shape: a
 * gate is 16 bytes rather than 8, because the handler address is 64-bit,
 * and it carries an IST index the 32-bit gate had no room for.
 *
 * The IST is the part worth understanding. Normally an interrupt keeps
 * running on whatever stack was current, which is fine until the reason
 * for the interrupt *is* the stack - a kernel stack that overflowed into
 * an unmapped page, say. The CPU then tries to push a fault frame onto
 * that same broken stack, fails, escalates to a double fault, tries again,
 * fails again, and triple faults, which on real hardware is a reset and in
 * QEMU is a silent reboot loop with no diagnostic whatsoever. An IST entry
 * tells the CPU to switch to a known-good stack for that vector
 * unconditionally, which turns the worst failure in the kernel into one
 * that can print where it happened. */

#include <stdint.h>
#include "idt64.h"
#include "gdt64.h"
#include "serial64.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

/* Where the IRQs are made to land. The gates below are installed at 32-47
 * on the assumption that this remap has happened - and until Milestone 47
 * it had not, because nothing in the 64-bit tree had yet enabled
 * interrupts, so nothing noticed.
 *
 * The default the BIOS leaves behind puts IRQ0-7 at vectors 8-15, which
 * collides with the CPU's own exception vectors: a timer tick arrives as
 * vector 8, which is #DF. That is not a theoretical clash. It presents as
 * a double fault whose register dump is shifted by one quadword, because
 * the handler for vector 8 expects an error code on the stack and a
 * hardware interrupt does not push one. */
#define PIC1_VECTOR_BASE 32
#define PIC2_VECTOR_BASE 40

#define IDT_ENTRIES 256

struct idt64_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;           /* bits 0-2 select an IST stack; 0 = don't switch */
    uint8_t  type_attr;     /* P | DPL | 0 | type */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt64_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt64_entry idt[IDT_ENTRIES];
static struct idt64_ptr   idt_ptr;
static isr64_handler_t    handlers[IDT_ENTRIES];

extern void idt64_flush(struct idt64_ptr* ptr);

/* isr64.s */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

static const char* const exception_names[32] = {
    "divide error", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range exceeded", "invalid opcode",
    "device not available", "double fault", "coprocessor segment overrun",
    "invalid TSS", "segment not present", "stack-segment fault",
    "general protection fault", "page fault", "reserved",
    "x87 floating point exception", "alignment check", "machine check",
    "SIMD floating point exception", "virtualization exception",
    "control protection exception", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "hypervisor injection",
    "VMM communication exception", "security exception", "reserved"
};

static void set_gate(int n, uint64_t handler, uint8_t ist, uint8_t dpl) {
    idt[n].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[n].selector    = GDT64_KCODE_SEL;
    idt[n].ist         = (uint8_t)(ist & 0x7);
    /* Type 0xE is a 64-bit interrupt gate, which clears IF on entry. A trap
     * gate (0xF) would leave interrupts on, and the common stub is not
     * reentrant. */
    idt[n].type_attr   = (uint8_t)(0x80 | ((dpl & 3) << 5) | 0x0E);
    idt[n].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].offset_high = (uint32_t)(handler >> 32);
    idt[n].zero        = 0;
}

void register_interrupt_handler64(uint8_t n, isr64_handler_t handler) {
    handlers[n] = handler;
}

static uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void panic(registers64_t* r) {
    uint64_t n = r->int_no;

    serial64_puts("\nNOVARIS64: *** unhandled exception: ");
    serial64_puts(n < 32 ? exception_names[n] : "interrupt");
    serial64_puts(" (vector ");
    serial64_putdec(n);
    serial64_puts(")\n");

    serial64_puts("NOVARIS64:   err=");   serial64_puthex(r->err_code);
    serial64_puts(" rip=");               serial64_puthex(r->rip);
    serial64_puts(" cs=");                serial64_puthex(r->cs);
    serial64_puts("\nNOVARIS64:   rflags="); serial64_puthex(r->rflags);
    serial64_puts(" rsp=");               serial64_puthex(r->rsp);
    serial64_puts(" ss=");                serial64_puthex(r->ss);
    if (n == 14) {
        serial64_puts("\nNOVARIS64:   cr2=");
        serial64_puthex(read_cr2());
    }
    serial64_puts("\nNOVARIS64: *** halted\n");

    for (;;) __asm__ __volatile__("cli; hlt");
}

/* Called from isr_common_stub with rdi pointing at the frame. */
void isr64_handler(registers64_t* r) {
    if (r->int_no < IDT_ENTRIES && handlers[r->int_no]) {
        handlers[r->int_no](r);
        return;
    }
    panic(r);
}

/* Called from irq_common_stub. The PIC end-of-interrupt is sent here
 * rather than in the stub so a handler cannot forget it. */
void irq64_handler(registers64_t* r) {
    if (r->int_no >= 40) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);

    if (r->int_no < IDT_ENTRIES && handlers[r->int_no]) {
        handlers[r->int_no](r);
    }
}

/* The 8259 pair, moved off the exception vectors and then silenced.
 *
 * Everything is masked afterwards on purpose: the 64-bit tree has no
 * device drivers yet, so an unmasked line can only produce an interrupt
 * nobody is prepared to service. Callers unmask what they have a handler
 * for, one line at a time, with idt64_irq_set_mask(). */
static void pic64_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    (void)mask1; (void)mask2;   /* read for the io delay, then discarded */

    outb(PIC1_CMD, 0x11);       /* ICW1: begin init, expect ICW4 */
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, PIC1_VECTOR_BASE);  /* ICW2: vector offsets */
    outb(PIC2_DATA, PIC2_VECTOR_BASE);
    outb(PIC1_DATA, 0x04);      /* ICW3: a slave is wired to IRQ2 */
    outb(PIC2_DATA, 0x02);      /* ICW3: and this is which line that is */
    outb(PIC1_DATA, 0x01);      /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01);

    outb(PIC1_DATA, 0xFF);      /* every line masked */
    outb(PIC2_DATA, 0xFF);
}

void idt64_irq_set_mask(int irq, int masked) {
    uint16_t port;
    uint8_t bit, value;

    if (irq < 0 || irq > 15) return;
    if (irq < 8) {
        port = PIC1_DATA;
        bit = (uint8_t)irq;
    } else {
        port = PIC2_DATA;
        bit = (uint8_t)(irq - 8);
    }
    value = inb(port);
    if (masked) value |= (uint8_t)(1u << bit);
    else        value &= (uint8_t)~(1u << bit);
    outb(port, value);

    /* Unmasking a line on the slave does nothing unless IRQ2, the line
     * the slave is cascaded onto, is unmasked too. */
    if (irq >= 8 && !masked) idt64_irq_set_mask(2, 0);
}

void idt64_install(void) {
    int i;

    for (i = 0; i < IDT_ENTRIES; i++) {
        handlers[i] = 0;
        set_gate(i, 0, 0, 0);
    }

    set_gate(0,  (uint64_t)isr0,  0, 0);
    set_gate(1,  (uint64_t)isr1,  0, 0);
    set_gate(2,  (uint64_t)isr2,  IST_NMI, 0);
    set_gate(3,  (uint64_t)isr3,  0, 0);
    set_gate(4,  (uint64_t)isr4,  0, 0);
    set_gate(5,  (uint64_t)isr5,  0, 0);
    set_gate(6,  (uint64_t)isr6,  0, 0);
    set_gate(7,  (uint64_t)isr7,  0, 0);
    set_gate(8,  (uint64_t)isr8,  IST_DOUBLE_FAULT, 0);
    set_gate(9,  (uint64_t)isr9,  0, 0);
    set_gate(10, (uint64_t)isr10, 0, 0);
    set_gate(11, (uint64_t)isr11, 0, 0);
    set_gate(12, (uint64_t)isr12, 0, 0);
    set_gate(13, (uint64_t)isr13, 0, 0);
    set_gate(14, (uint64_t)isr14, 0, 0);
    set_gate(15, (uint64_t)isr15, 0, 0);
    set_gate(16, (uint64_t)isr16, 0, 0);
    set_gate(17, (uint64_t)isr17, 0, 0);
    set_gate(18, (uint64_t)isr18, 0, 0);
    set_gate(19, (uint64_t)isr19, 0, 0);
    set_gate(20, (uint64_t)isr20, 0, 0);
    set_gate(21, (uint64_t)isr21, 0, 0);
    set_gate(22, (uint64_t)isr22, 0, 0);
    set_gate(23, (uint64_t)isr23, 0, 0);
    set_gate(24, (uint64_t)isr24, 0, 0);
    set_gate(25, (uint64_t)isr25, 0, 0);
    set_gate(26, (uint64_t)isr26, 0, 0);
    set_gate(27, (uint64_t)isr27, 0, 0);
    set_gate(28, (uint64_t)isr28, 0, 0);
    set_gate(29, (uint64_t)isr29, 0, 0);
    set_gate(30, (uint64_t)isr30, 0, 0);
    set_gate(31, (uint64_t)isr31, 0, 0);

    set_gate(32, (uint64_t)irq0,  0, 0);
    set_gate(33, (uint64_t)irq1,  0, 0);
    set_gate(34, (uint64_t)irq2,  0, 0);
    set_gate(35, (uint64_t)irq3,  0, 0);
    set_gate(36, (uint64_t)irq4,  0, 0);
    set_gate(37, (uint64_t)irq5,  0, 0);
    set_gate(38, (uint64_t)irq6,  0, 0);
    set_gate(39, (uint64_t)irq7,  0, 0);
    set_gate(40, (uint64_t)irq8,  0, 0);
    set_gate(41, (uint64_t)irq9,  0, 0);
    set_gate(42, (uint64_t)irq10, 0, 0);
    set_gate(43, (uint64_t)irq11, 0, 0);
    set_gate(44, (uint64_t)irq12, 0, 0);
    set_gate(45, (uint64_t)irq13, 0, 0);
    set_gate(46, (uint64_t)irq14, 0, 0);
    set_gate(47, (uint64_t)irq15, 0, 0);

    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base  = (uint64_t)&idt;

    idt64_flush(&idt_ptr);

    /* After the table is loaded, so that if a line is somehow already
     * asserted the gate it lands on exists. */
    pic64_remap();
}
