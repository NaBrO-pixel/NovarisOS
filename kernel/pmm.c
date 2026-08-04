#include "pmm.h"

/* One bit per 4KB physical frame, 1 = used/reserved, 0 = free. Sized to
 * cover a full 32-bit address space (4GB / 4KB / 8 bits = 128KB) so we
 * never have to worry about a memory map entry falling outside it; we
 * only ever touch the portion covering RAM the bootloader actually
 * reported, and 128KB of .bss is cheap. */
#define MAX_FRAMES   (0x100000000ULL / PMM_FRAME_SIZE)
#define BITMAP_BYTES (MAX_FRAMES / 8)

static uint8_t frame_bitmap[BITMAP_BYTES];
static uint32_t highest_frame = 0;     /* highest frame index we know about */

static inline void bitmap_set(uint32_t bit) {
    frame_bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}
static inline void bitmap_clear(uint32_t bit) {
    frame_bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}
static inline int bitmap_test(uint32_t bit) {
    return frame_bitmap[bit / 8] & (uint8_t)(1u << (bit % 8));
}

static void mark_region(uint32_t start_frame, uint32_t frame_count, int used) {
    for (uint32_t i = 0; i < frame_count; i++) {
        uint32_t frame = start_frame + i;
        if (frame >= MAX_FRAMES) break;

        if (used) {
            bitmap_set(frame);
        } else {
            bitmap_clear(frame);
        }
        if (frame > highest_frame) highest_frame = frame;
    }
}

void pmm_init(multiboot_info_t* mbi, uint32_t kernel_physical_start,
              uint32_t kernel_physical_end) {
    /* Start with everything marked used. We then free only the regions
     * the bootloader actually reports as available RAM - safer than the
     * reverse, since unknown regions (MMIO, reserved BIOS areas, holes
     * above installed RAM) default to "don't touch" rather than "assume
     * it's free". */
    for (uint32_t i = 0; i < BITMAP_BYTES; i++) frame_bitmap[i] = 0xFF;

    if (mbi->flags & MULTIBOOT_INFO_MMAP) {
        uint32_t entry_addr = mbi->mmap_addr;
        uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;

        while (entry_addr < mmap_end) {
            multiboot_mmap_entry_t* entry = (multiboot_mmap_entry_t*)entry_addr;

            if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                uint64_t start = entry->addr;
                uint64_t len = entry->len;

                if (start < 0x100000000ULL) {
                    uint64_t end = start + len;
                    if (end > 0x100000000ULL) end = 0x100000000ULL;

                    uint32_t start_frame = (uint32_t)(start / PMM_FRAME_SIZE);
                    uint32_t end_frame = (uint32_t)(end / PMM_FRAME_SIZE);
                    if (end_frame > start_frame) {
                        mark_region(start_frame, end_frame - start_frame, 0);
                    }
                }
            }
            entry_addr += entry->size + (uint32_t)sizeof(entry->size);
        }
    } else {
        /* No memory map available - fall back to mem_lower/mem_upper,
         * which at least covers conventional + extended RAM. */
        mark_region(0, (mbi->mem_lower * 1024) / PMM_FRAME_SIZE, 0);
        mark_region(0x100000 / PMM_FRAME_SIZE,
                    (mbi->mem_upper * 1024) / PMM_FRAME_SIZE, 0);
    }

    /* Never hand out the first 1MB (real-mode IVT/BDA, VGA memory, BIOS
     * areas) or the kernel's own image, no matter what the memory map
     * says about them. */
    mark_region(0, 0x100000 / PMM_FRAME_SIZE, 1);

    uint32_t kstart_frame = kernel_physical_start / PMM_FRAME_SIZE;
    uint32_t kend_frame = (kernel_physical_end + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    mark_region(kstart_frame, kend_frame - kstart_frame, 1);
}

void pmm_reserve_region(uint32_t start_addr, uint32_t end_addr) {
    uint32_t start_frame = start_addr / PMM_FRAME_SIZE;
    uint32_t end_frame = (end_addr + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    if (end_frame > start_frame) {
        mark_region(start_frame, end_frame - start_frame, 1);
    }
}

uint32_t pmm_alloc_frame(void) {
    for (uint32_t frame = 0; frame <= highest_frame; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            return frame * PMM_FRAME_SIZE;
        }
    }
    return 0; /* out of memory */
}

uint32_t pmm_alloc_contiguous(uint32_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_frame();

    for (uint32_t start = 0; start + count <= highest_frame + 1; start++) {
        uint32_t i = 0;
        while (i < count && !bitmap_test(start + i)) i++;
        if (i < count) {
            /* Resume past the frame that was taken rather than at
             * start+1: every window overlapping it would fail the same
             * way, and a boot-time scan of a 768MB bitmap is worth not
             * doing 190,000 times over. */
            start += i;
            continue;
        }
        for (i = 0; i < count; i++) bitmap_set(start + i);
        return start * PMM_FRAME_SIZE;
    }
    return 0;
}

void pmm_free_contiguous(uint32_t phys_addr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_frame(phys_addr + i * PMM_FRAME_SIZE);
    }
}

/* Freeing a frame that is already free is the one memory bug this
 * allocator cannot survive quietly: the bit is simply cleared, and if the
 * frame was handed out again in between, two owners now share it and the
 * damage shows up somewhere else entirely, one program later. Counting
 * them turns "the next thing that runs behaves oddly" into a number the
 * shell can print. Milestone 29 added it while chasing exactly that. */
static uint32_t double_frees = 0;

uint32_t pmm_double_frees(void) { return double_frees; }

void pmm_free_frame(uint32_t phys_addr) {
    uint32_t frame = phys_addr / PMM_FRAME_SIZE;
    if (frame < MAX_FRAMES) {
        if (!bitmap_test(frame)) { double_frees++; return; }
        bitmap_clear(frame);
    }
}

uint32_t pmm_total_frames(void) { return highest_frame + 1; }

uint32_t pmm_free_frames(void) {
    /* Scanned on demand rather than tracked incrementally: highest_frame
     * only ever spans the range mark_region() has touched for *real*
     * reasons (freeing actual RAM, or reserving something within it), so
     * this stays internally consistent no matter how many alloc/free
     * calls happened - unlike a running counter spanning the full 4GB
     * bitmap, which drifted out of sync with pmm_total_frames()'s much
     * smaller scope and underflowed here in practice. highest_frame is
     * at most ~1M for 4GB of RAM, so this scan is cheap. */
    uint32_t free_count = 0;
    for (uint32_t frame = 0; frame <= highest_frame; frame++) {
        if (!bitmap_test(frame)) free_count++;
    }
    return free_count;
}

uint32_t pmm_used_frames(void) { return pmm_total_frames() - pmm_free_frames(); }
