#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* We define our own flat GDT rather than relying on whatever GRUB left set
 * up. "Flat" means each segment spans the entire 4GB address space, so in
 * practice segmentation does nothing and we rely on paging later for
 * protection/isolation - this is standard practice for modern x86 kernels. */

void gdt_install(void);

/* Updates the TSS's ring-0 stack pointer (esp0). The CPU loads esp/ss
 * from here automatically whenever an interrupt or syscall arrives while
 * running in ring 3, so this must point at a valid, currently-unused
 * kernel stack before any user-mode code runs. */
void gdt_set_kernel_stack(uint32_t esp0);

/* Reads it back. The Win32 callback layer lowers esp0 while a ring-3
 * callback runs, so that traps taken during the callback don't land on
 * the kernel frames of the API call that started it, and needs the old
 * value to put back afterwards - see kernel/win32_callback.c. */
uint32_t gdt_get_kernel_stack(void);

/* Points GDT entry 6 (selector 0x33) at a Thread Environment Block, the
 * one place segmentation still earns its keep on x86: Windows code
 * reaches its TEB through fs:[...], so fs has to be a segment whose base
 * *is* the TEB. Called by the Win32 layer when a program starts - see
 * kernel/win32.c. `limit` is in bytes (byte granularity). */
void gdt_set_teb(uint32_t base, uint32_t limit);

/* --- the thread-local storage descriptors -------------------------------
 *
 * Linux/i386 gives a thread *three* of these, numbered 6, 7 and 8, and
 * set_thread_area() hands out whichever is free. Novaris had one, which
 * was enough while the only thing asking was glibc - and exactly one
 * short the moment Wine asked too.
 *
 * Wine's i386 code reaches the Windows TEB through fs:[...] the way glibc
 * reaches its TLS through gs:[...], and it gets that segment by calling
 * set_thread_area() itself. With one descriptor, Wine's request silently
 * overwrote glibc's, and the next glibc function to touch errno read it
 * out of the middle of a TEB. What it looked like from outside was Wine
 * dereferencing a null pointer inside its own SIGSEGV handler.
 *
 * So: three, numbered as Linux numbers them, and all three saved and
 * restored on every thread switch because all three are per-thread. */
#define GDT_TLS_MIN 6
#define GDT_TLS_MAX 8
#define GDT_TLS_COUNT (GDT_TLS_MAX - GDT_TLS_MIN + 1)

/* Points GDT entry `slot` (6, 7 or 8; selector (slot<<3)|3) at a thread's
 * local-storage block. Set by set_thread_area() and by
 * clone(CLONE_SETTLS), and reprogrammed by the scheduler on every switch,
 * since it is per-thread. See kernel/posix_thread.c. */
void gdt_set_tls_entry(int slot, uint32_t base, uint32_t limit);

/* The middle one, kept for the callers that only ever wanted a single
 * descriptor - the Win32 layer's threads. */
void gdt_set_tls(uint32_t base, uint32_t limit);

#endif
