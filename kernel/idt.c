#include "idt.h"
#include "pic.h"
#include "console.h"
#include "posix.h"

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;        /* kernel segment selector the handler runs in */
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

#define IDT_ENTRIES 256
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   ip;

extern void idt_flush(uint32_t);

/* One slot per interrupt number so drivers can register handlers without
 * this file needing to know about PIT/keyboard/etc. NULL = unhandled. */
static isr_handler_t handlers[IDT_ENTRIES];

/* Forward-declare all 48 assembly stubs from isr.s. */
#define DECLARE_ISR(n) extern void isr##n(void)
#define DECLARE_IRQ(n) extern void irq##n(void)
DECLARE_ISR(0);  DECLARE_ISR(1);  DECLARE_ISR(2);  DECLARE_ISR(3);
DECLARE_ISR(4);  DECLARE_ISR(5);  DECLARE_ISR(6);  DECLARE_ISR(7);
DECLARE_ISR(8);  DECLARE_ISR(9);  DECLARE_ISR(10); DECLARE_ISR(11);
DECLARE_ISR(12); DECLARE_ISR(13); DECLARE_ISR(14); DECLARE_ISR(15);
DECLARE_ISR(16); DECLARE_ISR(17); DECLARE_ISR(18); DECLARE_ISR(19);
DECLARE_ISR(20); DECLARE_ISR(21); DECLARE_ISR(22); DECLARE_ISR(23);
DECLARE_ISR(24); DECLARE_ISR(25); DECLARE_ISR(26); DECLARE_ISR(27);
DECLARE_ISR(28); DECLARE_ISR(29); DECLARE_ISR(30); DECLARE_ISR(31);
DECLARE_ISR(128); /* syscall interrupt (int 0x80) */
DECLARE_ISR(129); /* Win32 emulation entry (int 0x81) */
DECLARE_ISR(130); /* Win32 ring-3 callback return (int 0x82) */
DECLARE_IRQ(0);  DECLARE_IRQ(1);  DECLARE_IRQ(2);  DECLARE_IRQ(3);
DECLARE_IRQ(4);  DECLARE_IRQ(5);  DECLARE_IRQ(6);  DECLARE_IRQ(7);
DECLARE_IRQ(8);  DECLARE_IRQ(9);  DECLARE_IRQ(10); DECLARE_IRQ(11);
DECLARE_IRQ(12); DECLARE_IRQ(13); DECLARE_IRQ(14); DECLARE_IRQ(15);

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

static const char* exception_name(uint8_t n) {
    static const char* names[32] = {
        "Divide-by-zero", "Debug", "Non-maskable interrupt", "Breakpoint",
        "Overflow", "Bound range exceeded", "Invalid opcode",
        "Device not available", "Double fault", "Coprocessor segment overrun",
        "Invalid TSS", "Segment not present", "Stack-segment fault",
        "General protection fault", "Page fault", "Reserved",
        "x87 floating point", "Alignment check", "Machine check",
        "SIMD floating point", "Virtualization", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
        "Reserved", "Reserved", "Security exception", "Reserved"
    };
    return names[n];
}

/* Set by kernel/scheduler.c when a context switch is pending - see
 * irq_handler() below for why signal delivery has to stand aside for it. */
extern uint32_t scheduler_next_esp;

static fault_hook_t user_fault_hook = 0;

void idt_set_user_fault_hook(fault_hook_t hook) {
    user_fault_hook = hook;
}

/* Called from isr_common_stub for CPU exceptions (vectors 0-31). */
void isr_handler(registers_t* regs) {
    if (handlers[regs->int_no]) {
        handlers[regs->int_no](regs);
        /* On the way back to ring 3 is the one moment a signal can be
         * injected: the frame about to be iret'ed from is the thread's
         * user-mode state, so redirecting it into a handler is all
         * delivery takes. See kernel/posix_signal.c.
         *
         * Not while a switch is pending, for the reason irq_handler()
         * gives below and one more since Milestone 29: the syscall that
         * set it may have been exit, in which case `regs` belongs to a
         * task whose user stack has already been handed back to the
         * physical allocator. Writing a signal frame onto it would fault
         * in the kernel. */
        if (!scheduler_next_esp) posix_deliver_pending(regs);
        return;
    }

    /* Before panicking, let the user-program hook look at it: a fault
     * that happened in ring 3 belongs to whatever program is running, and
     * killing that program is a far better outcome than halting the
     * machine. The hook returns 0 (or isn't installed) for anything that
     * genuinely is the kernel's fault, which falls through below. */
    if (regs->int_no < 32 && user_fault_hook && user_fault_hook(regs)) {
        return;
    }

    /* No handler registered for this exception: this is a real fault we
     * don't know how to recover from, so stop and report it clearly
     * instead of silently corrupting state or triple-faulting. */
    terminal_setcolor(0x4F); /* white on red */
    terminal_writestring("\n\n*** KERNEL PANIC ***\n");
    if (regs->int_no < 32) {
        terminal_writestring("Exception: ");
        terminal_writestring(exception_name(regs->int_no));
    } else {
        terminal_writestring("Unhandled interrupt vector (no handler registered)");
    }
    terminal_writestring("\n");
    terminal_writestring("System halted.\n");

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/* Called from irq_common_stub for hardware IRQs (remapped to 32-47). */
void irq_handler(registers_t* regs) {
    uint8_t irq = regs->int_no - 32;

    if (handlers[regs->int_no]) {
        handlers[regs->int_no](regs);
    }

    pic_send_eoi(irq);

    /* Not while a context switch is pending: the scheduler is about to
     * repoint esp at a different task's frame, and `regs` is the outgoing
     * one - delivering into it would install a handler nobody is going to
     * run. The signal stays pending and arrives on the next return to
     * ring 3 instead. */
    if (!scheduler_next_esp) posix_deliver_pending(regs);
}

void register_interrupt_handler(uint8_t n, isr_handler_t handler) {
    handlers[n] = handler;
}

void idt_install(void) {
    ip.limit = (sizeof(struct idt_entry) * IDT_ENTRIES) - 1;
    ip.base  = (uint32_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    /* Remap the PIC before wiring up IRQ gates, so IRQ0-15 land on
     * vectors 32-47 instead of colliding with CPU exceptions 0-31. */
    pic_remap(32, 40);

    /* 0x8E = present, ring 0, 32-bit interrupt gate. Selector 0x08 is our
     * kernel code segment from the GDT. */
    #define GATE(n, fn) idt_set_gate(n, (uint32_t)fn, 0x08, 0x8E)
    GATE(0, isr0);   GATE(1, isr1);   GATE(2, isr2);   GATE(3, isr3);
    GATE(4, isr4);   GATE(5, isr5);   GATE(6, isr6);   GATE(7, isr7);
    GATE(8, isr8);   GATE(9, isr9);   GATE(10, isr10); GATE(11, isr11);
    GATE(12, isr12); GATE(13, isr13); GATE(14, isr14); GATE(15, isr15);
    GATE(16, isr16); GATE(17, isr17); GATE(18, isr18); GATE(19, isr19);
    GATE(20, isr20); GATE(21, isr21); GATE(22, isr22); GATE(23, isr23);
    GATE(24, isr24); GATE(25, isr25); GATE(26, isr26); GATE(27, isr27);
    GATE(28, isr28); GATE(29, isr29); GATE(30, isr30); GATE(31, isr31);

    GATE(32, irq0);  GATE(33, irq1);  GATE(34, irq2);  GATE(35, irq3);
    GATE(36, irq4);  GATE(37, irq5);  GATE(38, irq6);  GATE(39, irq7);
    GATE(40, irq8);  GATE(41, irq9);  GATE(42, irq10); GATE(43, irq11);
    GATE(44, irq12); GATE(45, irq13); GATE(46, irq14); GATE(47, irq15);
    #undef GATE

    /* Syscall gate: DPL=3 (0xEE, vs 0x8E for every other gate) so ring-3
     * code is allowed to trigger it directly with `int 0x80`. The Win32
     * emulation layer's entry point (int 0x81) needs the same treatment. */
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);
    idt_set_gate(129, (uint32_t)isr129, 0x08, 0xEE);
    /* And the callback-return vector, which ring 3 executes from a stub
     * the kernel itself planted - see include/win32_callback.h. */
    idt_set_gate(130, (uint32_t)isr130, 0x08, 0xEE);

    idt_flush((uint32_t)&ip);
}
