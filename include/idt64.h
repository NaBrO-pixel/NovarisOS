#ifndef IDT64_H
#define IDT64_H

#include <stdint.h>

/* The 64-bit interrupt frame.
 *
 * Three things changed from the 32-bit registers_t and each one is a
 * source of silent corruption if it is got wrong:
 *
 *  - There is no `pusha` in long mode. All fifteen general-purpose
 *    registers are pushed by hand in isr64.s, and this struct has to
 *    describe exactly the order they land in. rsp is deliberately not
 *    among them: the CPU's own frame carries the real one.
 *
 *  - The CPU *always* pushes ss and rsp, even when no privilege change
 *    happened. In 32-bit protected mode it pushed them only on a ring
 *    transition, which is why the old struct's useresp/ss were readable
 *    only for a fault from user mode. Here they are always valid.
 *
 *  - Segment registers are gone from the frame. In long mode ds/es/ss are
 *    ignored by the hardware and fs/gs are set through MSRs, so saving
 *    them means saving FSBASE/GSBASE, which belongs with the thread
 *    context rather than here.
 *
 * kernel/scheduler.c will need to build this layout by hand for a process
 * that has never run, the same as the 32-bit one does, so the two have to
 * stay in step. */
typedef struct registers64 {
    /* Pushed by isr64.s, listed in ascending address order. */
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

    /* Pushed by the stub: a real error code, or a zero standing in for one
     * on the vectors that do not have one, so the frame is uniform. */
    uint64_t int_no, err_code;

    /* Pushed by the CPU. */
    uint64_t rip, cs, rflags, rsp, ss;
} registers64_t;

typedef void (*isr64_handler_t)(registers64_t*);

void idt64_install(void);

/* Unmask (masked = 0) or mask (masked = 1) one IRQ line. idt64_install
 * leaves every line masked, because a line with no handler behind it can
 * only produce an interrupt nobody is prepared to service. */
void idt64_irq_set_mask(int irq, int masked);

/* Same contract as the 32-bit register_interrupt_handler: vectors 0-31 are
 * CPU exceptions, 32-47 the remapped IRQs. */
void register_interrupt_handler64(uint8_t n, isr64_handler_t handler);

/* A handler may write to the frame it is given - changing rip to step over
 * a faulting instruction, for instance - and the change takes effect when
 * the stub returns, because iretq reloads from the same memory. This is
 * the mechanism Wine's exception dispatch is built on, so it is load
 * bearing rather than a convenience. */

#endif
