#include "syscall.h"
#include "idt.h"
#include "posix.h"

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

static void syscall_handler(registers_t* regs) {
    posix_syscall(regs);
}

void syscall_install(void) {
    register_interrupt_handler(0x80, syscall_handler);
}
