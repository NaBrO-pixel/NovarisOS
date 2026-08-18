/* pmm64.c - physical frames, on a machine whose addresses are 64 bits.
 *
 * The bitmap is placed rather than declared. boot64.s maps the first
 * gigabyte of physical memory twice - once identity, once at KERNEL_VMA -
 * so anything below 1GB physical is reachable at KERNEL_VMA + phys with no
 * further work. The bitmap goes above the kernel image and above whatever
 * the bootloader loaded after it, which is still well inside that window:
 * even 64GB of RAM needs only a 2MB bitmap.
 *
 * The bitmap has to live in that window rather than in the direct map,
 * because the direct map is built out of page tables that come from this
 * allocator. It is the one thing that cannot use the map that made
 * everything else's physical address irrelevant.
 *
 * That window is the one real constraint here, and init checks it rather
 * than trusting it. If a machine ever turns up whose bitmap would not fit,
 * the allocator reports itself not ready instead of writing over whatever
 * happens to be at the far end of the window. */

#include "pmm64.h"

#define KERNEL_VMA        0xFFFFFFFF80000000ULL
/* How much physical memory boot64.s maps at KERNEL_VMA. */
#define BOOT_WINDOW_BYTES 0x40000000ULL          /* 1GB */

static uint8_t* bitmap;                  /* through the KERNEL_VMA window */
static uint64_t bitmap_phys_addr;
static uint64_t bitmap_size_bytes;
static uint64_t frame_count;             /* frames the bitmap describes */

/* How many *extra* owners a frame has, one byte each, laid out beside the
 * bitmap and by the same argument (Milestone 69).
 *
 * Zero means one owner, which is what every allocation before
 * copy-on-write existed had - so nothing that already calls
 * pmm64_free_frame has to change, and a frame nobody shared is freed on
 * the first free exactly as before. pmm64_ref_frame raises it, and a
 * free at a non-zero count drops an owner rather than the frame.
 *
 * A byte saturates at 255. Saturating silently would leak; refusing to
 * share past that is the caller's cue to copy instead, which is what
 * vmspace64's clone does. */
static uint8_t* refs;
static uint64_t refs_phys_addr;
static uint64_t refs_size_bytes;
#define REF_MAX 255
static uint64_t highest_addr;
static uint64_t double_frees;
static uint64_t alloc_hint;              /* where the last upward scan left off */
static uint64_t high_hint;               /* and the downward one */
static int      ready;

static inline void bitmap_set(uint64_t bit) {
    bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}
static inline void bitmap_clear(uint64_t bit) {
    bitmap[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}
static inline int bitmap_test(uint64_t bit) {
    return bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7));
}

static void mark_range(uint64_t start_frame, uint64_t count, int used) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t f = start_frame + i;
        if (f >= frame_count) break;
        if (used) bitmap_set(f); else bitmap_clear(f);
    }
}

/* Walk the Multiboot memory map, calling back per AVAILABLE entry. Written
 * once and used twice - to size the bitmap, then to fill it - because the
 * two passes disagreeing about which entries are usable is exactly the kind
 * of bug that hands out a frame of firmware. */
typedef void (*mmap_fn)(uint64_t start, uint64_t end);

static void for_each_available(const multiboot_info_t* mbi, mmap_fn fn) {
    uint64_t addr, end;

    if (!(mbi->flags & MULTIBOOT_INFO_MMAP)) {
        /* No map. mem_lower/mem_upper are in KB and describe conventional
         * and extended memory; it is a worse answer than the map but it is
         * the same answer the 32-bit kernel falls back on. */
        fn(0, (uint64_t)mbi->mem_lower * 1024ULL);
        fn(0x100000ULL, 0x100000ULL + (uint64_t)mbi->mem_upper * 1024ULL);
        return;
    }

    addr = mbi->mmap_addr;
    end  = (uint64_t)mbi->mmap_addr + mbi->mmap_length;
    while (addr < end) {
        /* The mbi and its map are below 4GB and the identity mapping from
         * boot64.s is still live, so a physical address is a valid pointer. */
        const multiboot_mmap_entry_t* e = (const multiboot_mmap_entry_t*)addr;
        if (e->type == MULTIBOOT_MEMORY_AVAILABLE && e->len > 0)
            fn(e->addr, e->addr + e->len);
        addr += e->size + sizeof(e->size);
    }
}

/* Pass 1: how high does RAM go. */
static void note_highest(uint64_t start, uint64_t end) {
    (void)start;
    if (end > highest_addr) highest_addr = end;
}

/* Pass 2: mark it free. */
static void free_region(uint64_t start, uint64_t end) {
    uint64_t sf = start / PMM64_FRAME_SIZE;
    uint64_t ef = end / PMM64_FRAME_SIZE;      /* a partial tail frame is
                                                * not a whole frame, so it
                                                * stays marked used */
    if (ef > sf) mark_range(sf, ef - sf, 0);
}

void pmm64_init(const multiboot_info_t* mbi,
                uint64_t kernel_phys_start, uint64_t kernel_phys_end) {
    uint64_t place;

    ready = 0;
    highest_addr = 0;
    double_frees = 0;
    alloc_hint = 0;

    for_each_available(mbi, note_highest);
    if (highest_addr == 0) return;

    frame_count = (highest_addr + PMM64_FRAME_SIZE - 1) / PMM64_FRAME_SIZE;
    bitmap_size_bytes = (frame_count + 7) / 8;

    /* Set here rather than to 0 above, because pmm64_free_frame raises
     * this hint to include whatever was just returned - and 0 is a
     * position, not an "unset" marker. Left at 0, the first free call
     * would pin the downward scan near the bottom of RAM and the high
     * allocator would quietly behave like the low one. */
    high_hint = frame_count;

    /* Above the kernel *and* above anything the bootloader loaded, page
     * aligned.
     *
     * "Above the kernel" alone was wrong, and wrong in the way that
     * survives testing: GRUB puts the initrd a few pages past the kernel
     * image, and the bitmap is written before anything has had a chance
     * to reserve that. With 128MB of RAM the bitmap was 4KB and stopped
     * 20KB short of the module, so it passed for twenty milestones. At
     * 2GB it is 64KB and lands squarely on the initrd's header - which
     * presents as "no filesystem", not as "memory bug". The bitmap's
     * size scales with RAM; its distance from the module does not. */
    place = kernel_phys_end;
    {
        const multiboot_module_t* mods;
        if ((mbi->flags & MULTIBOOT_INFO_MODS) && mbi->mods_count) {
            mods = (const multiboot_module_t*)(uint64_t)mbi->mods_addr;
            for (uint32_t i = 0; i < mbi->mods_count; i++)
                if (mods[i].mod_end > place) place = mods[i].mod_end;
        }
    }
    place = (place + PMM64_FRAME_SIZE - 1) & ~(PMM64_FRAME_SIZE - 1);

    /* The refcount table is eight times the bitmap and goes immediately
     * after it, so both are covered by the one window check below. At
     * 6GB that is 1.5MB of bitmap-plus-counts against a 1GB window. */
    refs_size_bytes = frame_count;

    if (place + bitmap_size_bytes + refs_size_bytes > BOOT_WINDOW_BYTES) {
        /* Would fall outside what boot64.s mapped. Refusing is the only
         * safe answer: the write would land somewhere unmapped, or worse,
         * somewhere mapped and in use. */
        bitmap_phys_addr = 0;
        refs_phys_addr   = 0;
        return;
    }

    bitmap_phys_addr = place;
    bitmap = (uint8_t*)(KERNEL_VMA + place);

    refs_phys_addr = place + bitmap_size_bytes;
    refs = (uint8_t*)(KERNEL_VMA + refs_phys_addr);
    for (uint64_t i = 0; i < refs_size_bytes; i++) refs[i] = 0;

    /* Everything used, then free only what the bootloader vouched for.
     * The reverse - assume free, mark the exceptions - hands out MMIO and
     * firmware whenever the map has a hole nobody described. */
    for (uint64_t i = 0; i < bitmap_size_bytes; i++) bitmap[i] = 0xFF;
    for_each_available(mbi, free_region);

    /* The first megabyte is never ours: real-mode IVT, BDA, VGA, and the
     * BIOS areas live there whatever the map says. */
    mark_range(0, 0x100000ULL / PMM64_FRAME_SIZE, 1);

    pmm64_reserve_region(kernel_phys_start, kernel_phys_end);
    /* Both tables, in one reservation. Reserving only the bitmap is the
     * Milestone 66 bug with a new name: the refcount table is eight
     * times the size, so it is the one that would reach whatever sits
     * above it, and it would do so only at the RAM sizes nobody tested. */
    pmm64_reserve_region(bitmap_phys_addr,
                         refs_phys_addr + refs_size_bytes);

    ready = 1;
}

void pmm64_reserve_region(uint64_t start_addr, uint64_t end_addr) {
    uint64_t sf = start_addr / PMM64_FRAME_SIZE;
    uint64_t ef = (end_addr + PMM64_FRAME_SIZE - 1) / PMM64_FRAME_SIZE;
    if (ef > sf) mark_range(sf, ef - sf, 1);
}

uint64_t pmm64_alloc_frame(void) {
    if (!ready) return 0;

    /* Two sweeps from a rolling hint rather than one from zero. The 32-bit
     * allocator restarts at frame 0 every time, which is O(frames) per
     * call and turned into a measurable cost once something allocated in
     * bulk - see the kmalloc note in Milestone 43. */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? alloc_hint : 0;
        uint64_t stop  = pass == 0 ? frame_count : alloc_hint;
        for (uint64_t f = start; f < stop; f++) {
            if (!bitmap_test(f)) {
                bitmap_set(f);
                alloc_hint = f + 1;
                return f * PMM64_FRAME_SIZE;
            }
        }
    }
    return 0;
}

/* Allocates from the top of RAM downwards, for pages that will belong to
 * a user process.
 *
 * The kernel's own frames - page tables, the frame bitmap, anything that
 * has to be reachable before the direct map exists - want to be low.
 * User pages have no such constraint now that the direct map reaches
 * everything, and there are far more of them. Handing them out from the
 * top keeps the two populations apart instead of letting a process eat
 * the low memory the kernel still needs.
 *
 * It also means every existing userland test now runs on memory above
 * the old 1GB ceiling, on any machine with more than 1GB. That is worth
 * more than a single test that reaches high memory on purpose: the claim
 * "where a frame lives no longer matters" is only believable if the
 * ordinary path is the one exercising it. */
uint64_t pmm64_alloc_high(void) {
    if (!ready) return 0;
    if (high_hint > frame_count) high_hint = frame_count;

    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? high_hint : frame_count;
        uint64_t stop  = pass == 0 ? 0 : high_hint;
        for (uint64_t f = start; f-- > stop; ) {
            if (!bitmap_test(f)) {
                bitmap_set(f);
                high_hint = f;
                return f * PMM64_FRAME_SIZE;
            }
        }
    }

    /* Out of high memory is not out of memory: fall back rather than
     * failing an allocation the low path could still have served. */
    return pmm64_alloc_frame();
}

/* A frame at or above `min_phys`, so that a test can ask for exactly the
 * kind of memory the kernel used to be unable to touch. Nothing in the
 * kernel proper cares where a frame comes from - which is the point of
 * Milestone 66 - so this exists to make the ceiling's absence provable
 * rather than to be used by the allocator's normal callers. */
uint64_t pmm64_alloc_above(uint64_t min_phys) {
    uint64_t first = (min_phys + PMM64_FRAME_SIZE - 1) / PMM64_FRAME_SIZE;

    if (!ready) return 0;

    for (uint64_t f = first; f < frame_count; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            return f * PMM64_FRAME_SIZE;
        }
    }
    return 0;
}

void pmm64_free_frame(uint64_t phys) {
    uint64_t f = phys / PMM64_FRAME_SIZE;
    if (!ready || f >= frame_count) return;

    /* Freeing something already free is the bug this allocator cannot
     * survive quietly: if the frame was handed out in between, two owners
     * now share it and the damage surfaces somewhere else entirely. Count
     * it rather than clearing the bit a second time. */
    if (!bitmap_test(f)) { double_frees++; return; }

    /* Shared: this drops one owner, not the frame. The frame goes back
     * only when the last owner lets go, which is the whole contract
     * copy-on-write is built on - a page mapped into a parent and a
     * child must survive either of them exiting. */
    if (refs && refs[f]) { refs[f]--; return; }

    bitmap_clear(f);
    if (f < alloc_hint) alloc_hint = f;
    /* Both hints move to include the frame just returned, so neither
     * allocator has to wrap around to find it again. */
    if (f >= high_hint) high_hint = f + 1;
}

uint64_t pmm64_total_frames(void) { return ready ? frame_count : 0; }

uint64_t pmm64_free_frames(void) {
    uint64_t n = 0;
    if (!ready) return 0;
    for (uint64_t f = 0; f < frame_count; f++)
        if (!bitmap_test(f)) n++;
    return n;
}

uint64_t pmm64_used_frames(void) {
    return pmm64_total_frames() - pmm64_free_frames();
}

uint64_t pmm64_double_frees(void) { return double_frees; }
uint64_t pmm64_highest_addr(void) { return highest_addr; }
uint64_t pmm64_bitmap_phys(void)  { return bitmap_phys_addr; }
uint64_t pmm64_bitmap_bytes(void) { return bitmap_size_bytes; }
int      pmm64_ready(void)        { return ready; }

/* --- sharing (Milestone 69) ----------------------------------------- */

/* Adds an owner. Returns 0 if the frame cannot take another - either it
 * is not a frame this allocator owns, or the count has saturated - and
 * the caller must copy rather than share. */
int pmm64_ref_frame(uint64_t phys) {
    uint64_t f = phys / PMM64_FRAME_SIZE;

    if (!ready || !refs || f >= frame_count) return 0;
    if (!bitmap_test(f)) return 0;          /* not allocated: not shareable */
    if (refs[f] >= REF_MAX) return 0;
    refs[f]++;
    return 1;
}

/* How many owners the frame has: 1 for an ordinary allocation, more once
 * it has been shared. 0 means it is not allocated at all. */
uint64_t pmm64_frame_owners(uint64_t phys) {
    uint64_t f = phys / PMM64_FRAME_SIZE;

    if (!ready || f >= frame_count) return 0;
    if (!bitmap_test(f)) return 0;
    return 1 + (refs ? refs[f] : 0);
}

/* Whether this address is RAM this allocator manages at all. VRAM and
 * anything else above the memory map is not, and freeing it would either
 * do nothing or corrupt the bitmap's idea of the world. */
int pmm64_owns(uint64_t phys) {
    return ready && (phys / PMM64_FRAME_SIZE) < frame_count;
}

uint64_t pmm64_refs_phys(void)  { return refs_phys_addr; }
uint64_t pmm64_refs_bytes(void) { return refs_size_bytes; }
