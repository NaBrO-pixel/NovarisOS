#ifndef LIBC_H
#define LIBC_H

/* Novaris's syscall ABI (see kernel/syscall.c): eax = syscall number,
 * ebx/ecx/edx = args, return value comes back in eax. */

/* Writes a NUL-terminated string to the console (SYS_WRITE). */
void write(const char* str);

/* Maps `size` bytes of fresh anonymous memory (SYS_MMAP) and returns a
 * pointer to it, or NULL on failure. There's no matching "munmap" yet -
 * see kernel/syscall.c's sys_mmap for why. */
void* mmap_anon(unsigned int size);

/* Ends the process (SYS_EXIT) and hands control back to the kernel.
 * Never returns. */
void exit(int code) __attribute__((noreturn));

#endif
