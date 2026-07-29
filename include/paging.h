#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

/* Replaces boot.s's coarse 4MB-page bootstrap mapping with a real
 * 4KB-granularity page directory: identity-maps the first 4MB (so the
 * VGA buffer and low-memory structures keep working) and maps the
 * kernel's own physical image at its higher-half virtual addresses.
 * Must run after pmm_init(), since it allocates page-table frames
 * through the physical memory manager. */
void paging_init(uint32_t kernel_physical_start, uint32_t kernel_physical_end);

/* Maps a single 4KB page. virt_addr and phys_addr must both be
 * page-aligned. Allocates a new page table via the PMM if needed. */
void paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);

/* Removes a mapping and invalidates the TLB entry for it. Does not free
 * the underlying physical frame - call pmm_free_frame() separately if
 * that's what you want. */
void paging_unmap_page(uint32_t virt_addr);

/* Raw page-table-entry access, added for the PE loader (kernel/pe.c).
 * A Windows .exe wants to be mapped at its own ImageBase - classically
 * 0x400000, which lands inside the first 4MB that paging_init()
 * identity-maps. Rather than refuse to honor ImageBase, the loader saves
 * whatever PTEs it is about to overwrite and puts them back when the
 * program exits, so borrowing that address range is invisible to the rest
 * of the kernel. paging_get_entry() returns 0 for an address whose page
 * table doesn't exist yet, which paging_set_entry() treats as "unmap". */
uint32_t paging_get_entry(uint32_t virt_addr);
void paging_set_entry(uint32_t virt_addr, uint32_t entry);

/* Virtual-address reservations. Some identity mappings genuinely cannot
 * be borrowed the way the previous paragraph describes, because the
 * kernel still dereferences them while a user program runs - the initrd
 * (a program that reads a file goes through it) and the framebuffer
 * (every terminal_putchar draws into it) are the two that matter.
 * kernel.c registers those, the Win32 layer registers its own arenas, and
 * the PE loader checks candidate load addresses against the list before
 * mapping anything. Regions are half-open [start, end). */
#define PAGING_MAX_RESERVATIONS 16
void paging_reserve_region(uint32_t start, uint32_t end, const char* name);

/* Returns the name of the first reserved region overlapping
 * [start, end), or 0 if the range is clear. */
const char* paging_region_conflict(uint32_t start, uint32_t end);

#endif
