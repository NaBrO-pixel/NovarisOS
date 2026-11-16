#ifndef GDT64_H
#define GDT64_H

#include <stdint.h>

/* Selectors, as byte offsets into the GDT - Linux's x86-64 layout for
 * everything ring 3 can see (Milestone 114), because Wine's WoW64 names
 * them: 0x23 is 32-bit user code, which it hard-codes for running 32-bit
 * Windows code in compatibility mode; 0x2b user data; 0x33 64-bit user
 * code. sysret computes cs and ss from one MSR by fixed offsets from the
 * 32-bit code selector - see syscall64_init_cpu. Entries 12-14 are the
 * thread's TLS slots (set_thread_area), and each CPU's TSS sits above. */
#define GDT64_KCODE_SEL   0x08
#define GDT64_KDATA_SEL   0x10
#define GDT64_UCODE32_SEL 0x20
#define GDT64_UDATA_SEL   0x28
#define GDT64_UCODE_SEL   0x30
#define GDT64_TLS_FIRST   12
#define GDT64_TLS_COUNT   3
#define GDT64_TSS_SEL     0x80

/* With RPL 3: what ring 3 actually holds. */
#define USER_CS32 (GDT64_UCODE32_SEL | 3)   /* 0x23 */
#define USER_DS   (GDT64_UDATA_SEL | 3)     /* 0x2b */
#define USER_CS   (GDT64_UCODE_SEL | 3)     /* 0x33 */

/* A thread's TLS descriptors into this CPU's GDT (entries 12-14), on
 * every switch to it. */
void gdt64_load_tls(const uint64_t* tls);

/* IST slots, as an IDT gate encodes them (1-7; 0 means "no switch"). */
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2

void gdt64_install(void);
void gdt64_install_cpu(int cpu);
int  gdt64_cpu(void);            /* this CPU, from its task register */           /* this CPU's own GDT and TSS */
void gdt64_set_kernel_stack_cpu(int cpu, uint64_t rsp0);

/* The stack the CPU switches to when an interrupt arrives while in ring 3.
 * The scheduler updates this on every context switch, exactly as the
 * 32-bit kernel does with esp0. */
void gdt64_set_kernel_stack(uint64_t rsp0);

/* The top of an IST stack, for tests and diagnostics. 0 if out of range. */
uint64_t gdt64_ist_stack(int index);

#endif
