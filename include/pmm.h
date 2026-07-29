#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "multiboot.h"

#define PMM_FRAME_SIZE 4096

/* Builds the free/used frame bitmap from the Multiboot memory map (or
 * mem_lower/mem_upper as a fallback), then marks the first 1MB and the
 * kernel's own physical image as permanently used regardless of what the
 * map says about them. Must run before paging_init(), which allocates
 * page-table frames through this allocator. */
void pmm_init(multiboot_info_t* mbi, uint32_t kernel_physical_start,
              uint32_t kernel_physical_end);

/* Returns the physical address of a free 4KB frame (and marks it used),
 * or 0 if we're out of memory. */
uint32_t pmm_alloc_frame(void);

/* Marks a previously-allocated frame free again. */
void pmm_free_frame(uint32_t phys_addr);

/* Marks every frame overlapping [start_addr, end_addr) as used, without
 * needing to know frame-aligned boundaries. Used to reserve things the
 * Multiboot memory map doesn't know are spoken for yet - e.g. an initrd
 * module's backing memory, which must stay untouched until it's been
 * parsed, but sits in what the memory map calls "available RAM". Call
 * this before paging_init() or kheap_init() run any allocations, or they
 * could hand out (and overwrite) the very memory you're trying to protect. */
void pmm_reserve_region(uint32_t start_addr, uint32_t end_addr);

uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_free_frames(void);

#endif
