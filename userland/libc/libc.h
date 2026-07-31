#ifndef LIBC_H
#define LIBC_H

/* The Linux/i386 syscall ABI, which Novaris adopted wholesale in
 * Milestone 18: eax = syscall number, ebx/ecx/edx/esi/edi/ebp = args 1-6,
 * return value in eax with errors as -errno. See include/posix.h.
 *
 * These three wrappers are a convenience for the in-tree demos, not a
 * porting layer: the calls underneath them are the real ones, which is
 * why userland/posix_test.c can skip this header entirely and still run. */

/* Writes a NUL-terminated string to stdout, via write(1, str, len). */
void write(const char* str);

/* mmap2 with MAP_ANONYMOUS|MAP_PRIVATE and PROT_READ|PROT_WRITE.
 * Returns NULL on failure. munmap exists now too, though this wrapper
 * does not expose it. */
void* mmap_anon(unsigned int size);

/* exit(). Never returns. */
void exit(int code) __attribute__((noreturn));

#endif
