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

/* Points GDT entry 6 (selector 0x33) at a Thread Environment Block, the
 * one place segmentation still earns its keep on x86: Windows code
 * reaches its TEB through fs:[...], so fs has to be a segment whose base
 * *is* the TEB. Called by the Win32 layer when a program starts - see
 * kernel/win32.c. `limit` is in bytes (byte granularity). */
void gdt_set_teb(uint32_t base, uint32_t limit);

#endif
