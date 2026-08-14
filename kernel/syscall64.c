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
#include "serial64.h"

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

static uint64_t bytes_written;

uint64_t syscall64_bytes_written(void) { return bytes_written; }

/* Called from syscall64_entry. The arguments have already been shifted
 * from Linux's (rdi, rsi, rdx) into this function's own. */
uint64_t syscall64_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                            uint64_t a3) {
    call_count++;
    switch (nr) {
    case SYS64_WRITE: {
        /* write(fd, buf, count). `buf` is a ring-3 pointer, and it is
         * dereferenced directly because the kernel is running in the
         * caller's address space - the high half is shared, so a syscall
         * never had to leave the space it was called from.
         *
         * It is also completely unchecked, which is the honest state of
         * this ABI: a real implementation validates that the range is
         * mapped and belongs to the caller before touching it, and does
         * so without racing the caller. Nothing here does that yet. */
        const char* buf = (const char*)a2;
        uint64_t n = a3;
        if (a1 != 1 && a1 != 2) return (uint64_t)-1;   /* stdout/stderr */
        for (uint64_t i = 0; i < n; i++) serial64_putc(buf[i]);
        bytes_written += n;
        return n;
    }
    case SYS64_ECHO:
        last_arg = a1;
        return a1 + 0x1111;
    case SYS64_EXIT:
    case SYS64_EXIT_GROUP:
        exit_code = a1;
        return a1;
    default:
        /* Linux answers an unimplemented call with -ENOSYS, and programs
         * do check for it, so this is -38 rather than -1. */
        return (uint64_t)-38;
    }
}

uint64_t syscall64_count(void)     { return call_count; }
uint64_t syscall64_last_arg(void)  { return last_arg; }
uint64_t syscall64_exit_code(void) { return exit_code; }
