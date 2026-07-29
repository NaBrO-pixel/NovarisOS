#include "paging.h"
#include "pmm.h"

#define PAGE_SIZE          4096
#define ENTRIES_PER_TABLE  1024
#define KERNEL_VIRTUAL_BASE 0xC0000000u

#define PD_INDEX(v) ((v) >> 22)
#define PT_INDEX(v) (((v) >> 12) & 0x3FFu)

/* Recursive mapping trick: the last page-directory entry (index 1023)
 * points at the directory itself. That makes the directory readable as
 * ordinary memory at 0xFFFFF000, and page table N readable at
 * 0xFFC00000 + N*4096 - so once this is set up, we never need to assume
 * physical memory is identity-mapped to inspect or edit page tables. */
#define RECURSIVE_PD_INDEX 1023
#define PAGE_DIRECTORY      ((uint32_t*)0xFFFFF000u)
#define PAGE_TABLE(pd_index) ((uint32_t*)(0xFFC00000u + ((pd_index) << 12)))

static inline void invlpg(uint32_t addr) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(addr) : "memory");
}

void paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pd_idx = PD_INDEX(virt_addr);
    uint32_t pt_idx = PT_INDEX(virt_addr);

    if (!(PAGE_DIRECTORY[pd_idx] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        /* Point the directory at the new table, then immediately zero it
         * out through its own recursive mapping (safe: no physical
         * pointer dereference needed). */
        PAGE_DIRECTORY[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        invlpg((uint32_t)PAGE_TABLE(pd_idx)); /* the PDE itself changed */
        uint32_t* pt = PAGE_TABLE(pd_idx);
        for (int i = 0; i < ENTRIES_PER_TABLE; i++) pt[i] = 0;
    }

    uint32_t* pt = PAGE_TABLE(pd_idx);
    pt[pt_idx] = (phys_addr & ~0xFFFu) | (flags & 0xFFFu) | PAGE_PRESENT;
    invlpg(virt_addr);
}

void paging_unmap_page(uint32_t virt_addr) {
    uint32_t pd_idx = PD_INDEX(virt_addr);
    if (!(PAGE_DIRECTORY[pd_idx] & PAGE_PRESENT)) return;

    uint32_t* pt = PAGE_TABLE(pd_idx);
    pt[PT_INDEX(virt_addr)] = 0;
    invlpg(virt_addr);
}

uint32_t paging_get_entry(uint32_t virt_addr) {
    uint32_t pd_idx = PD_INDEX(virt_addr);
    if (!(PAGE_DIRECTORY[pd_idx] & PAGE_PRESENT)) return 0;
    return PAGE_TABLE(pd_idx)[PT_INDEX(virt_addr)];
}

void paging_set_entry(uint32_t virt_addr, uint32_t entry) {
    if (!(entry & PAGE_PRESENT)) {
        paging_unmap_page(virt_addr);
        return;
    }
    /* Goes through paging_map_page so a page table gets allocated if the
     * saved entry belonged to a directory slot that has since been torn
     * down. The flags are already baked into `entry`. */
    paging_map_page(virt_addr, entry & ~0xFFFu, entry & 0xFFFu);
}

/* Virtual-address reservations - see paging.h. A fixed-size table: this
 * is a handful of kernel-owned regions registered once at boot, not a
 * general-purpose allocator. */
static struct {
    uint32_t start, end;
    const char* name;
} reservations[PAGING_MAX_RESERVATIONS];
static uint32_t reservation_count = 0;

void paging_reserve_region(uint32_t start, uint32_t end, const char* name) {
    if (reservation_count >= PAGING_MAX_RESERVATIONS) return;
    if (end <= start) return;
    reservations[reservation_count].start = start;
    reservations[reservation_count].end = end;
    reservations[reservation_count].name = name;
    reservation_count++;
}

const char* paging_region_conflict(uint32_t start, uint32_t end) {
    if (end <= start) return 0;
    for (uint32_t i = 0; i < reservation_count; i++) {
        if (start < reservations[i].end && reservations[i].start < end) {
            return reservations[i].name;
        }
    }
    return 0;
}

void paging_init(uint32_t kernel_physical_start, uint32_t kernel_physical_end) {
    /* Build a brand new page directory. We're still running under
     * boot.s's boot-time 4MB-page mapping at this point, which identity-
     * maps the first 8MB - since pmm_init() reserves the first 1MB and
     * the (small) kernel image before handing out any frames, the very
     * first few pmm_alloc_frame() calls are guaranteed to land in that
     * still-identity-mapped range, so we can dereference their physical
     * addresses directly here, before the recursive mapping exists yet. */
    uint32_t pd_phys = pmm_alloc_frame();
    uint32_t* pd = (uint32_t*)pd_phys;
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) pd[i] = 0;

    pd[RECURSIVE_PD_INDEX] = pd_phys | PAGE_PRESENT | PAGE_RW;

    /* Identity-map the first 4MB: BIOS/VGA memory, the Multiboot info
     * struct, and anything else low memory that needs to stay reachable
     * at its physical address. */
    uint32_t pt_low_phys = pmm_alloc_frame();
    uint32_t* pt_low = (uint32_t*)pt_low_phys;
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        pt_low[i] = (uint32_t)(i * PAGE_SIZE) | PAGE_PRESENT | PAGE_RW;
    }
    pd[0] = pt_low_phys | PAGE_PRESENT | PAGE_RW;

    /* Map the kernel's own physical image at its higher-half virtual
     * addresses, one page table per 4MB chunk spanned. */
    uint32_t start = kernel_physical_start & ~(PAGE_SIZE - 1);
    uint32_t end = (kernel_physical_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint32_t phys = start;
    while (phys < end) {
        uint32_t virt = phys + KERNEL_VIRTUAL_BASE;
        uint32_t pd_idx = PD_INDEX(virt);

        if (!(pd[pd_idx] & PAGE_PRESENT)) {
            uint32_t pt_phys = pmm_alloc_frame();
            uint32_t* pt = (uint32_t*)pt_phys;
            for (int i = 0; i < ENTRIES_PER_TABLE; i++) pt[i] = 0;
            pd[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_RW;
        }

        uint32_t* pt = (uint32_t*)(pd[pd_idx] & ~0xFFFu);
        while (phys < end && PD_INDEX(phys + KERNEL_VIRTUAL_BASE) == pd_idx) {
            pt[PT_INDEX(phys + KERNEL_VIRTUAL_BASE)] = phys | PAGE_PRESENT | PAGE_RW;
            phys += PAGE_SIZE;
        }
    }

    /* Switch to the new directory. We're currently executing out of the
     * higher-half alias of boot.s's 4MB mapping, and we've just recreated
     * that same mapping (kernel image + low 4MB) with 4KB granularity
     * above, so this switch is seamless - no instructions or data we're
     * using right now become unmapped. */
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}
