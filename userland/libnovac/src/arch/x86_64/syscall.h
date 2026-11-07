/* x86-64 system-call entry: the Linux x86-64 convention, which NovarisOS
 * implements (kernel/syscall64.s, include/syscall64.h).
 *   rax = number; rdi, rsi, rdx, r10, r8, r9 = arguments 1-6;
 *   result in rax, errors as -errno in [-4095, -1]; rcx and r11 clobbered.
 * This is the only file that executes the `syscall` instruction. */
#ifndef NOVAC_ARCH_X86_64_SYSCALL_H
#define NOVAC_ARCH_X86_64_SYSCALL_H

#if !defined(__x86_64__)
# error "libNovaC's NovarisOS binding currently supports x86-64 only"
#endif

static inline long novac_syscall6(long n, long a1, long a2, long a3,
                                  long a4, long a5, long a6)
{
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    long ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}

#endif
