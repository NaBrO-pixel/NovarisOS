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

/* Second recursive slot, for editing *another* address space's tables
 * without switching CR3 to it.
 *
 * The trick is the same one that makes the 1023 slot work, applied once
 * more. Point directory entry 1022 at some other address space's page
 * directory, and the CPU will happily treat that directory as if it were
 * a page table: its 1024 entries become the PTEs of the 4MB window at
 * 0xFF800000, so foreign page table N reads and writes at
 * 0xFF800000 + N*4096. And because entry 1022 is itself reachable through
 * the 1023 recursion, the foreign directory *itself* is addressable at
 * PAGE_TABLE(1022) = 0xFFFFE000.
 *
 * The alternative - a scratch page plus a mapping dance per table - needs
 * an allocator and gets the invalidation wrong in interesting ways. This
 * needs neither: attach, edit, detach.
 *
 * Entry 1022 is deliberately excluded from the global kernel set below,
 * so every address space has its own (initially empty) window and one
 * process never sees another's foreign attachment. */
#define FOREIGN_PD_INDEX 1022
#define FOREIGN_PD        ((uint32_t*)(0xFFC00000u + (FOREIGN_PD_INDEX << 12)))
#define FOREIGN_TABLE(n)  ((uint32_t*)((FOREIGN_PD_INDEX << 22) + ((n) << 12)))

static inline void invlpg(uint32_t addr) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Reloading CR3 drops every non-global TLB entry. Used when a whole 4MB
 * window's meaning changes at once (attaching or detaching a foreign
 * directory), where invalidating page by page would mean 1024 invlpgs. */
static inline void flush_tlb(void) {
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* The kernel's own page directory - the one paging_init() built, and the
 * master copy every other address space takes its kernel half from. */
static uint32_t kernel_pd_phys = 0;

/* Which directory entries are shared by every address space. Captured
 * once by paging_finalize_kernel_space(); see the comment there. */
static uint32_t global_pde[ENTRIES_PER_TABLE / 32];
static int kernel_space_final = 0;

static inline int pde_is_global(uint32_t i) {
    return (global_pde[i >> 5] >> (i & 31)) & 1u;
}

/* Reported once if the kernel ever tries to add a directory entry to its
 * own half after the global set was frozen - see the comment in
 * paging_finalize_kernel_space() for why that would be a silent
 * correctness bug rather than a harmless one. */
static int kernel_pde_violation = 0;

int paging_kernel_pde_violated(void) {
    return kernel_pde_violation;
}

int paging_range_is_private(uint32_t start, uint32_t end) {
    if (end <= start) return 1;
    uint32_t first = PD_INDEX(start);
    uint32_t last = PD_INDEX(end - 1);
    for (uint32_t i = first; i <= last; i++) {
        if (pde_is_global(i)) return 0;
    }
    return 1;
}

uint32_t paging_next_global_pde(uint32_t from) {
    for (uint32_t i = from; i < ENTRIES_PER_TABLE; i++) {
        if (pde_is_global(i)) return i;
    }
    return ENTRIES_PER_TABLE;
}

void paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    uint32_t pd_idx = PD_INDEX(virt_addr);
    uint32_t pt_idx = PT_INDEX(virt_addr);

    if (!(PAGE_DIRECTORY[pd_idx] & PAGE_PRESENT)) {
        /* Creating a *kernel-half* directory entry after the global set
         * was frozen would give this mapping to the current address space
         * only: every address space already created copied the old set
         * and would never see it. Recorded rather than fixed, because the
         * fix belongs at the call site (reserve the range before
         * finalizing), not here. */
        if (kernel_space_final && pd_idx >= PD_INDEX(KERNEL_VIRTUAL_BASE) &&
            pd_idx != FOREIGN_PD_INDEX) {
            kernel_pde_violation = 1;
        }
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

    kernel_pd_phys = pd_phys;
}

/* --- address spaces ----------------------------------------------------- */

uint32_t paging_current_address_space(void) {
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ~0xFFFu;
}

uint32_t paging_kernel_address_space(void) {
    return kernel_pd_phys;
}

void paging_switch_address_space(uint32_t pd_phys) {
    if (!pd_phys) return;
    if (pd_phys == paging_current_address_space()) return;
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

/* Makes `pd_phys` editable through the 0xFF800000 window. Both halves
 * flush the whole TLB: 4MB of virtual address space changes meaning in
 * one go, and there is no cheaper correct answer. */
static void foreign_attach(uint32_t pd_phys) {
    PAGE_DIRECTORY[FOREIGN_PD_INDEX] = pd_phys | PAGE_PRESENT | PAGE_RW;
    flush_tlb();
}

static void foreign_detach(void) {
    PAGE_DIRECTORY[FOREIGN_PD_INDEX] = 0;
    flush_tlb();
}

void paging_reserve_kernel_tables(uint32_t start, uint32_t end) {
    if (kernel_space_final) return;
    for (uint32_t v = start & ~(PAGE_SIZE - 1); v < end; v += (1u << 22)) {
        uint32_t pd_idx = PD_INDEX(v);
        if (PAGE_DIRECTORY[pd_idx] & PAGE_PRESENT) continue;
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) return;
        PAGE_DIRECTORY[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_RW;
        invlpg((uint32_t)PAGE_TABLE(pd_idx));
        uint32_t* pt = PAGE_TABLE(pd_idx);
        for (int i = 0; i < ENTRIES_PER_TABLE; i++) pt[i] = 0;
    }
}

void paging_finalize_kernel_space(void) {
    /* Snapshot every directory entry that exists right now as "shared by
     * all address spaces". Doing it empirically rather than from a
     * hardcoded index range is deliberate: the kernel's own mappings are
     * scattered (the identity-mapped low 4MB, the initrd wherever GRUB
     * put it, the kernel image at 0xC0000000, the heap at 0xD0000000, the
     * linear framebuffer at whatever physical address the bootloader
     * chose - typically up near 0xFD000000), and any list written by hand
     * here would be a list that goes stale.
     *
     * The invariant this buys: a directory entry in the kernel's half
     * never changes after this point, so copying the set into a new
     * address space is enough to make the kernel identical in it -
     * forever, not just at creation time. paging_map_page() flags any
     * later attempt to add one (see kernel_pde_violation).
     *
     * Callers must therefore paging_reserve_kernel_tables() any kernel
     * range that will grow later - the heap being the one that does. */
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (i == RECURSIVE_PD_INDEX || i == FOREIGN_PD_INDEX) continue;
        if (PAGE_DIRECTORY[i] & PAGE_PRESENT) {
            global_pde[i >> 5] |= 1u << (i & 31);
        }
    }
    kernel_space_final = 1;
}

uint32_t paging_create_address_space(void) {
    if (!kernel_space_final) return 0; /* nothing to share yet */

    uint32_t pd_phys = pmm_alloc_frame();
    if (!pd_phys) return 0;

    foreign_attach(pd_phys);
    uint32_t* fpd = FOREIGN_PD;

    /* The frame is whatever the PMM last had in it, so every one of the
     * 1024 entries is written, not just the interesting ones. */
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        fpd[i] = pde_is_global(i) ? PAGE_DIRECTORY[i] : 0;
    }

    /* Its own recursive mapping, pointing at itself - so once this
     * directory is loaded into CR3, paging_map_page() and friends work
     * on it exactly as they do on the kernel's. */
    fpd[RECURSIVE_PD_INDEX] = pd_phys | PAGE_PRESENT | PAGE_RW;
    /* And no foreign window of its own until it asks for one. */
    fpd[FOREIGN_PD_INDEX] = 0;

    foreign_detach();
    return pd_phys;
}

void paging_destroy_address_space(uint32_t pd_phys) {
    if (!pd_phys || pd_phys == kernel_pd_phys) return;
    /* Freeing the directory we are standing on would unmap the code doing
     * the freeing on the next TLB miss. */
    if (pd_phys == paging_current_address_space()) return;

    foreign_attach(pd_phys);
    uint32_t* fpd = FOREIGN_PD;

    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (i == RECURSIVE_PD_INDEX || i == FOREIGN_PD_INDEX) continue;
        /* Shared kernel tables belong to everyone; freeing them here
         * would pull the kernel out from under every other address
         * space, this one included. */
        if (pde_is_global(i)) continue;
        if (!(fpd[i] & PAGE_PRESENT)) continue;

        uint32_t* ft = FOREIGN_TABLE(i);
        for (uint32_t j = 0; j < ENTRIES_PER_TABLE; j++) {
            if (ft[j] & PAGE_PRESENT) pmm_free_frame(ft[j] & ~0xFFFu);
        }
        pmm_free_frame(fpd[i] & ~0xFFFu);
        fpd[i] = 0;
    }

    foreign_detach();
    pmm_free_frame(pd_phys);
}

int paging_map_page_in(uint32_t pd_phys, uint32_t virt_addr,
                       uint32_t phys_addr, uint32_t flags) {
    if (!pd_phys || pd_phys == paging_current_address_space()) {
        paging_map_page(virt_addr, phys_addr, flags);
        return 1;
    }

    uint32_t pd_idx = PD_INDEX(virt_addr);
    uint32_t pt_idx = PT_INDEX(virt_addr);

    foreign_attach(pd_phys);
    uint32_t* fpd = FOREIGN_PD;

    if (!(fpd[pd_idx] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            foreign_detach();
            return 0;
        }
        fpd[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        /* That entry is a PTE of the foreign window as far as this CPU is
         * concerned, so the window's mapping just changed. */
        invlpg((uint32_t)FOREIGN_TABLE(pd_idx));
        uint32_t* ft = FOREIGN_TABLE(pd_idx);
        for (int i = 0; i < ENTRIES_PER_TABLE; i++) ft[i] = 0;
    }

    FOREIGN_TABLE(pd_idx)[pt_idx] =
        (phys_addr & ~0xFFFu) | (flags & 0xFFFu) | PAGE_PRESENT;

    foreign_detach();
    /* No invlpg for virt_addr: it is not mapped in the address space we
     * are running in, and the one it belongs to gets a full TLB flush
     * when CR3 is loaded with it. */
    return 1;
}

/* --- whole-address-space operations (Milestone 29) -----------------------
 *
 * A scratch page, for the one thing neither recursion answers on its own:
 * writing into a *frame* that is mapped in neither address space. fork
 * allocates the child's copy of a page from the PMM, and the only way to
 * put bytes in it is to give it a virtual address here first.
 *
 * The address is deliberately in the private half and deliberately
 * nowhere any loader places anything (the mmap arena starts at
 * 0x42000000, the program at 0x40000000, a PIE at 0x56000000), so the
 * page table it needs is this process's own and goes away with it. The
 * copy loop skips its directory entry, which is why it has to be a fixed
 * address rather than one taken from anywhere. */
#define SCRATCH_VA      0x3F000000u
#define SCRATCH_PD_IDX  PD_INDEX(SCRATCH_VA)

static void* scratch_map(uint32_t phys) {
    paging_map_page(SCRATCH_VA, phys, PAGE_PRESENT | PAGE_RW);
    return (void*)SCRATCH_VA;
}

static void scratch_unmap(void) {
    paging_unmap_page(SCRATCH_VA);
}

/* True for a directory entry this process owns outright: present, not one
 * of the two recursion slots, not part of the shared kernel set, and not
 * the scratch window above. */
static int pde_is_own(uint32_t i) {
    if (i == RECURSIVE_PD_INDEX || i == FOREIGN_PD_INDEX) return 0;
    if (i == SCRATCH_PD_IDX) return 0;
    if (pde_is_global(i)) return 0;
    return (PAGE_DIRECTORY[i] & PAGE_PRESENT) != 0;
}

int paging_copy_user_space(uint32_t dest_pd) {
    if (!dest_pd || dest_pd == paging_current_address_space()) return 0;

    /* Attached once for the whole walk rather than per page: every attach
     * and detach flushes the entire TLB, and a process with a few thousand
     * pages would otherwise spend the copy doing that. Safe because
     * nothing in the loop reads through the foreign window except the
     * tables being built, and the source pages are read through the
     * ordinary mapping, which the window does not disturb. */
    foreign_attach(dest_pd);
    uint32_t* fpd = FOREIGN_PD;

    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (!pde_is_own(i)) continue;
        uint32_t* pt = PAGE_TABLE(i);

        for (uint32_t j = 0; j < ENTRIES_PER_TABLE; j++) {
            uint32_t pte = pt[j];
            if (!(pte & PAGE_PRESENT)) continue;

            if (!(fpd[i] & PAGE_PRESENT)) {
                uint32_t pt_phys = pmm_alloc_frame();
                if (!pt_phys) { foreign_detach(); return 0; }
                fpd[i] = pt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
                invlpg((uint32_t)FOREIGN_TABLE(i));
                uint32_t* ft = FOREIGN_TABLE(i);
                for (uint32_t k = 0; k < ENTRIES_PER_TABLE; k++) ft[k] = 0;
            }

            uint32_t frame = pmm_alloc_frame();
            if (!frame) { foreign_detach(); return 0; }

            /* The child's frame is mapped nowhere yet, so it gets the
             * scratch address for exactly as long as the copy takes. */
            uint8_t* dst = (uint8_t*)scratch_map(frame);
            const uint8_t* src = (const uint8_t*)((i << 22) | (j << 12));
            for (uint32_t b = 0; b < PAGE_SIZE; b++) dst[b] = src[b];
            scratch_unmap();

            /* Same protection bits as the parent's page, including a
             * PROT_NONE reservation's, which the child must inherit as a
             * reservation rather than as usable memory. */
            FOREIGN_TABLE(i)[j] = frame | (pte & 0xFFFu);
        }
    }

    foreign_detach();
    return 1;
}

uint32_t paging_release_user_pages(void) {
    uint32_t freed = 0;

    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        /* The scratch entry is included here on purpose: it is this
         * process's page table like any other, and nothing is mapped
         * through it outside a copy. */
        if (i == RECURSIVE_PD_INDEX || i == FOREIGN_PD_INDEX) continue;
        if (pde_is_global(i)) continue;
        if (!(PAGE_DIRECTORY[i] & PAGE_PRESENT)) continue;

        uint32_t* pt = PAGE_TABLE(i);
        for (uint32_t j = 0; j < ENTRIES_PER_TABLE; j++) {
            if (!(pt[j] & PAGE_PRESENT)) continue;
            pmm_free_frame(pt[j] & ~0xFFFu);
            pt[j] = 0;
            freed++;
        }
        pmm_free_frame(PAGE_DIRECTORY[i] & ~0xFFFu);
        PAGE_DIRECTORY[i] = 0;
    }

    /* A whole half of the address space just changed meaning; page-by-page
     * invalidation would be a million invlpgs. */
    flush_tlb();
    return freed;
}

uint32_t paging_get_entry_in(uint32_t pd_phys, uint32_t virt_addr) {
    if (!pd_phys || pd_phys == paging_current_address_space()) {
        return paging_get_entry(virt_addr);
    }

    uint32_t pd_idx = PD_INDEX(virt_addr);
    uint32_t entry = 0;

    foreign_attach(pd_phys);
    if (FOREIGN_PD[pd_idx] & PAGE_PRESENT) {
        entry = FOREIGN_TABLE(pd_idx)[PT_INDEX(virt_addr)];
    }
    foreign_detach();
    return entry;
}
