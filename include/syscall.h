#ifndef SYSCALL_H
#define SYSCALL_H

/* Registers the int 0x80 handler. Its IDT gate (set up separately in
 * idt.c) has DPL=3, so ring-3 code is allowed to invoke it directly. */
void syscall_install(void);

/* Prints calls and TSC cycles per syscall number to COM1 - on Shift+F12,
 * with the timer samples. See syscall.c. */
void syscall_dump_profile(void);

#endif
