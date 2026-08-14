/* syscall64.c - the MSRs that make SYSCALL work, and what it dispatches to.
 *
 * This is the bring-up half of Milestone 44's item 4, and it is worth
 * being clear about what it is not. Novaris's 32-bit kernel implements
 * *Linux's i386 syscall ABI* - the numbers, the register convention and
 * the structure layouts - and that is precisely what lets real glibc and
 * real Wine run on it unmodified. None of that transfers to x86-64, which
 * has different numbers, a different register convention and different
 * structures. What is here is the mechanism: ring 3 is reachable, a
 * syscall arrives in C, and a value comes back. The ABI on top of it is
 * still to be written. */

#include "syscall64.h"
#include "gdt64.h"

#define IA32_EFER   0xC0000080u
#define IA32_STAR   0xC0000081u
#define IA32_LSTAR  0xC0000082u
#define IA32_FMASK  0xC0000084u

#define EFER_SCE    (1ULL << 0)

extern void syscall64_entry(void);

static uint64_t call_count;
static uint64_t last_arg;
static uint64_t exit_code;

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ __volatile__("wrmsr"
                         :: "c"(msr),
                            "a"((uint32_t)value),
                            "d"((uint32_t)(value >> 32)));
}

void syscall64_init(void) {
    uint64_t star;

    call_count = last_arg = exit_code = 0;

    /* Without SCE the instruction raises #UD rather than doing anything,
     * and the fault looks like a bad opcode rather than a missing bit. */
    write_msr(IA32_EFER, read_msr(IA32_EFER) | EFER_SCE);

    /* STAR[47:32] is the SYSCALL base, STAR[63:48] the SYSRET base. See
     * syscall64.h for how each expands into a CS/SS pair; the values are
     * 0x08 and 0x10 because of how gdt64.c orders its descriptors, and
     * changing that order silently changes which selectors land here. */
    star = ((uint64_t)GDT64_KCODE_SEL << 32)
         | ((uint64_t)GDT64_KDATA_SEL << 48);
    write_msr(IA32_STAR, star);

    write_msr(IA32_LSTAR, (uint64_t)&syscall64_entry);

    /* Bits set here are *cleared* in RFLAGS on entry. IF, so the handler
     * does not run with interrupts on while it is still sorting out its
     * stack; DF, so the string instructions in any C it calls count
     * upwards, which the SysV ABI requires and a ring-3 program is under
     * no obligation to leave true; TF, so a single-stepping debugger in
     * ring 3 does not trap inside the kernel. */
    write_msr(IA32_FMASK, (1ULL << 9) | (1ULL << 10) | (1ULL << 8));
}

/* Called from syscall64_entry with the number and one argument. */
uint64_t syscall64_dispatch(uint64_t nr, uint64_t arg1) {
    call_count++;
    switch (nr) {
    case SYS64_ECHO:
        last_arg = arg1;
        return arg1 + 0x1111;
    case SYS64_EXIT:
        exit_code = arg1;
        return arg1;
    default:
        return (uint64_t)-1;
    }
}

uint64_t syscall64_count(void)     { return call_count; }
uint64_t syscall64_last_arg(void)  { return last_arg; }
uint64_t syscall64_exit_code(void) { return exit_code; }
