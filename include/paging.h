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

/* --- address spaces ------------------------------------------------------
 *
 * Milestone 14. Through Milestone 13 there was exactly one page directory
 * and every program ran in it, which is why the Win32 layer can read a
 * program's stack by casting a pointer. These functions add the other
 * half: as many directories as you like, each with a private user half
 * and a kernel half identical to every other one's.
 *
 * An address space is identified by the physical address of its page
 * directory - the value that goes in CR3 - rather than by a struct, so
 * there is nothing to allocate, free or keep in step, and
 * paging_current_address_space() can answer by reading the register.
 *
 * The order of operations at boot matters:
 *
 *   1. paging_init()                     - build the kernel directory
 *   2. ... map the heap, initrd, framebuffer, whatever else is kernel-wide
 *   3. paging_reserve_kernel_tables()    - for kernel ranges that grow later
 *   4. paging_finalize_kernel_space()    - freeze what "kernel" means
 *   5. paging_create_address_space()     - any number of times, afterwards
 */

/* Pre-creates the page tables covering [start, end) in the kernel
 * directory, so that later mappings into that range only fill in page
 * table entries and never add a directory entry. Call before
 * paging_finalize_kernel_space() for every kernel range that can grow
 * after boot - the kernel heap being the one that does. */
void paging_reserve_kernel_tables(uint32_t start, uint32_t end);

/* Freezes the current set of directory entries as the shared kernel set.
 * Every address space created afterwards gets exactly these, which is
 * what makes the kernel reachable at the same addresses in all of them -
 * including from an interrupt that arrives while a user address space is
 * loaded. Call once, at the end of boot-time kernel mapping. */
void paging_finalize_kernel_space(void);

/* Non-zero if the kernel tried to add a directory entry to its own half
 * after finalizing - i.e. a kernel mapping that address spaces created
 * before it would not have. Surfaced by the `vmtest` shell command rather
 * than being made fatal, since it is a design error to find and fix, not
 * a runtime condition to recover from. */
int paging_kernel_pde_violated(void);

/* True if every page table covering [start, end) is private to the
 * current address space - i.e. mapping something there affects only this
 * process. A process's own mappings *must* satisfy this: a page table in
 * the shared kernel set is shared by every address space, so writing an
 * entry into one would hand the mapping to the kernel and to every other
 * process too. The shared set is 4MB-granular, so a range can fail this
 * while being nowhere near anything the kernel actually uses. */
int paging_range_is_private(uint32_t start, uint32_t end);

/* Index of the first shared directory entry at or after `from`, or 1024
 * if there is none. For reporting the layout; see the `vmtest` command. */
uint32_t paging_next_global_pde(uint32_t from);

/* Allocates a page directory whose kernel half is the frozen shared set
 * and whose user half is empty, with its own recursive mapping in place.
 * Returns its physical address, or 0 (out of memory, or called before
 * finalizing). */
uint32_t paging_create_address_space(void);

/* Frees an address space: every page table in its private half, every
 * frame those tables map, and the directory itself. Shared kernel tables
 * are left alone. Refuses to destroy the kernel's own directory, or the
 * one currently loaded in CR3. */
void paging_destroy_address_space(uint32_t pd_phys);

/* Loads a page directory into CR3. */
void paging_switch_address_space(uint32_t pd_phys);

/* The directory currently in CR3, and the kernel's own. */
uint32_t paging_current_address_space(void);
uint32_t paging_kernel_address_space(void);

/* paging_map_page() / paging_get_entry() against an address space that
 * is *not* the one currently loaded, reached through a second recursive
 * directory slot rather than by switching CR3 - see the comment on
 * FOREIGN_PD_INDEX in paging.c. Passing the current address space (or 0)
 * falls through to the ordinary versions. paging_map_page_in() returns 0
 * if it needed a page table and no physical memory was left. */
int      paging_map_page_in(uint32_t pd_phys, uint32_t virt_addr,
                            uint32_t phys_addr, uint32_t flags);
uint32_t paging_get_entry_in(uint32_t pd_phys, uint32_t virt_addr);

#endif
