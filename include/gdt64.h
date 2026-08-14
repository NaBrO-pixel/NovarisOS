#ifndef GDT64_H
#define GDT64_H

#include <stdint.h>

/* Selectors, as byte offsets into the GDT. The user pair sits immediately
 * above the kernel pair, and data before code, because sysret computes
 * both from one MSR by fixed offsets - see gdt64.c. */
#define GDT64_KCODE_SEL 0x08
#define GDT64_KDATA_SEL 0x10
#define GDT64_UDATA_SEL 0x18
#define GDT64_UCODE_SEL 0x20
#define GDT64_TSS_SEL   0x28

/* IST slots, as an IDT gate encodes them (1-7; 0 means "no switch"). */
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2

void gdt64_install(void);

/* The stack the CPU switches to when an interrupt arrives while in ring 3.
 * The scheduler updates this on every context switch, exactly as the
 * 32-bit kernel does with esp0. */
void gdt64_set_kernel_stack(uint64_t rsp0);

/* The top of an IST stack, for tests and diagnostics. 0 if out of range. */
uint64_t gdt64_ist_stack(int index);

#endif
