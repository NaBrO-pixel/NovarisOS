#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Registers pushed by our common ISR/IRQ stub, in the order they land on
 * the stack. This struct lets C handlers inspect what the CPU was doing
 * when the interrupt fired.
 *
 * All four segment registers are saved, in reverse push order (gs is
 * pushed last, so it sits lowest). Milestone 10 needed this: a Win32
 * program runs with a per-process fs, and the old "save ds, restore it
 * into all four" epilogue would overwrite it - see the comment in isr.s.
 * kernel/scheduler.c builds this same layout by hand for a process that
 * has never run, so the two have to stay in step. */
typedef struct registers {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; /* pusha order */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

typedef void (*isr_handler_t)(registers_t*);

void idt_install(void);

/* Lets drivers (PIT, keyboard, ...) register a handler for a specific
 * interrupt number (0-31 exceptions, 32-47 remapped IRQs). */
void register_interrupt_handler(uint8_t n, isr_handler_t handler);

/* Gets first refusal on any CPU exception with no registered handler,
 * ahead of the kernel panic screen. Returns 1 if it took ownership of the
 * fault (in practice by terminating the faulting user program and not
 * returning at all), 0 to let the panic run. process.c installs one; see
 * the comment on its handle_user_fault(). */
typedef int (*fault_hook_t)(registers_t*);
void idt_set_user_fault_hook(fault_hook_t hook);

#endif
