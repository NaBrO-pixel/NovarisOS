#include "syscall.h"
#include "idt.h"
#include "posix.h"
#include "serial.h"   /* syscall_dump_profile */

/* syscall.c - the `int 0x80` gate.
 *
 * Milestone 5 defined three ad-hoc calls here (SYS_EXIT=0, SYS_WRITE=1,
 * SYS_MMAP=2) - enough to prove a ring-3 program could ask the kernel for
 * something and get control back. Milestone 18 replaced that numbering
 * with Linux's, because that is what item 3 on the Path A roadmap needs:
 * Wine is a Unix program, and cross-compiling it means giving it a kernel
 * whose syscall numbers it recognises.
 *
 * The two numberings could not coexist - Linux's exit is 1, which was
 * Novaris's write - so the demo programs that used the old numbers were
 * migrated. Everything about the ABI now lives in kernel/posix.c; this
 * file is just the IDT registration. */

/* What every syscall costs, by number: how many, and how many TSC cycles
 * the kernel spent in them - printed with the timer samples on Shift+F12.
 *
 * The timer cannot answer this. A syscall runs with interrupts off (the
 * gate is an interrupt gate), so a tick that lands during one is taken on
 * the way back to ring 3 and blamed on the user instruction there - which
 * put 97% of a first wineboot on a retried read's int $0x80 and said
 * nothing about where the kernel's time went. Cycles counted around the
 * dispatch do. A syscall that parks its task is counted up to the park;
 * the time it then spends waiting is not the kernel's. */
#define SYSCALL_PROFILE_MAX 512u
static uint32_t prof_calls[SYSCALL_PROFILE_MAX];
static uint64_t prof_cycles[SYSCALL_PROFILE_MAX];

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void syscall_handler(registers_t* regs) {
    uint32_t nr = regs->eax;
    uint64_t t0 = rdtsc();
    posix_syscall(regs);
    if (nr < SYSCALL_PROFILE_MAX) {
        prof_calls[nr]++;
        prof_cycles[nr] += rdtsc() - t0;
    }
}

static void put_hex64(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    char b[17];
    for (int i = 0; i < 16; i++) b[i] = hex[(v >> (60 - 4 * i)) & 0xF];
    b[16] = 0;
    serial_writestring(b);
}

/* "Y nr calls cycles" for every syscall made, and "Y total" with the
 * TSC now, so the share of the run spent in the kernel can be read off. */
void syscall_dump_profile(void) {
    for (uint32_t nr = 0; nr < SYSCALL_PROFILE_MAX; nr++) {
        if (!prof_calls[nr]) continue;
        serial_writestring("Y ");
        put_hex64(nr);
        serial_writestring(" ");
        put_hex64(prof_calls[nr]);
        serial_writestring(" ");
        put_hex64(prof_cycles[nr]);
        serial_writestring("\n");
    }
    serial_writestring("Y total ");
    put_hex64(rdtsc());
    serial_writestring("\n");
}

void syscall_install(void) {
    register_interrupt_handler(0x80, syscall_handler);
}
