#ifndef SYSCALL_H
#define SYSCALL_H

/* Registers the int 0x80 handler. Its IDT gate (set up separately in
 * idt.c) has DPL=3, so ring-3 code is allowed to invoke it directly. */
void syscall_install(void);

#endif
