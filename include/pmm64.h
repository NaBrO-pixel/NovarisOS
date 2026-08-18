#ifndef PMM64_H
#define PMM64_H

#include <stdint.h>
#include "multiboot.h"

#define PMM64_FRAME_SIZE 4096ULL

/* The 64-bit physical frame allocator.
 *
 * Same idea as pmm.c - one bit per 4KB frame, 1 = used - and a different
 * shape, because the assumption the 32-bit one is built on does not hold
 * here. That one sizes its bitmap for a full 4GB address space at compile
 * time (128KB of .bss) and stops caring. A 64-bit machine can have more
 * RAM than that bitmap can describe, and sizing statically for the
 * architectural maximum is not an option: one bit per 4KB frame across
 * 2^52 frames is half a petabyte of bitmap.
 *
 * So the bitmap is placed at run time, immediately above the kernel image,
 * and sized from the highest address the bootloader actually reports. */

void     pmm64_init(const multiboot_info_t* mbi,
                    uint64_t kernel_phys_start, uint64_t kernel_phys_end);

/* 0 means out of memory. A real frame is never at physical 0: the first
 * megabyte is reserved unconditionally. */
uint64_t pmm64_alloc_frame(void);

/* A frame for a user page, taken from the top of RAM downwards. Falls
 * back to pmm64_alloc_frame when high memory runs out. See the comment
 * in pmm64.c for why user pages and kernel pages come from opposite
 * ends. */
uint64_t pmm64_alloc_high(void);

/* A frame at or above min_phys, or 0 if there is none. Exists so a test
 * can obtain high memory deliberately rather than hoping for it. */
uint64_t pmm64_alloc_above(uint64_t min_phys);
void     pmm64_free_frame(uint64_t phys);
void     pmm64_reserve_region(uint64_t start_addr, uint64_t end_addr);

uint64_t pmm64_total_frames(void);
uint64_t pmm64_free_frames(void);
uint64_t pmm64_used_frames(void);
uint64_t pmm64_double_frees(void);

/* One past the highest physical byte any AVAILABLE mmap entry described. */
uint64_t pmm64_highest_addr(void);

/* Where the bitmap was placed, and how big it is - reported rather than
 * assumed, because a boot where it did not fit needs to say so. */
/* Sharing, for copy-on-write (Milestone 69). A frame carries a count of
 * its owners; freeing at more than one drops an owner rather than the
 * frame. Zero extra owners is the default, so nothing that allocated
 * before this existed has to change. */
int      pmm64_ref_frame(uint64_t phys);
uint64_t pmm64_frame_owners(uint64_t phys);
int      pmm64_owns(uint64_t phys);
uint64_t pmm64_refs_phys(void);
uint64_t pmm64_refs_bytes(void);

uint64_t pmm64_bitmap_phys(void);
uint64_t pmm64_bitmap_bytes(void);

/* Non-zero if init found usable RAM and a place to describe it from. */
int      pmm64_ready(void);

#endif
