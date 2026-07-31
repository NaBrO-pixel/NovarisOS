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

/* Points GDT entry 7 (selector 0x3B) at a thread's local-storage block.
 * The POSIX counterpart of gdt_set_teb: i386 TLS is reached through
 * gs:[...], so gs has to be a segment whose base *is* the thread's TLS
 * area. Set by set_thread_area() and by clone(CLONE_SETTLS), and
 * reprogrammed by the scheduler on every switch, since it is per-thread.
 * See kernel/posix_thread.c. */
void gdt_set_tls(uint32_t base, uint32_t limit);

#endif
