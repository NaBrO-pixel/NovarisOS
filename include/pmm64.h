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
uint64_t pmm64_bitmap_phys(void);
uint64_t pmm64_bitmap_bytes(void);

/* Non-zero if init found usable RAM and a place to describe it from. */
int      pmm64_ready(void);

#endif
