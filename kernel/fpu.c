/* fpu.c - x87/SSE enable and per-task state. See include/fpu.h. */

#include "fpu.h"

void fpu_init(void) {
    uint32_t cr0, cr4;

    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2);   /* EM = 0: do not emulate, we have real hardware */
    cr0 |= (1u << 1);    /* MP = 1: monitor coprocessor */
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));

    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 9);    /* OSFXSR: FXSAVE/FXRSTOR and SSE are legal */
    cr4 |= (1u << 10);   /* OSXMMEXCPT: unmasked SSE exceptions raise #XF */
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));

    __asm__ __volatile__("fninit");
}

void fpu_state_init(fpu_state_t* state) {
    /* Capture what a clean unit looks like once, rather than hand-building
     * 512 bytes of FXSAVE layout - the architecture defines the format and
     * the CPU is the authority on it. */
    __asm__ __volatile__("fninit");
    fpu_save(state);
}

void fpu_save(fpu_state_t* state) {
    __asm__ __volatile__("fxsave (%0)" : : "r"(state->data) : "memory");
}

void fpu_restore(const fpu_state_t* state) {
    __asm__ __volatile__("fxrstor (%0)" : : "r"(state->data) : "memory");
}
