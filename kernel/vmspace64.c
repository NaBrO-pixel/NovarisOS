/* vmspace64.c - one PML4 per process, sharing the kernel's half. */

#include "vmspace64.h"
#include "paging64.h"
#include "pmm64.h"

#define ADDR_MASK   0x000FFFFFFFFFF000ULL
#define REC         510ULL          /* must match paging64.c */
#define KERNEL_HALF 256             /* first PML4 slot belonging to the kernel */

/* A scratch address used only while a new PML4 is being initialised - at
 * that moment the space has no self-reference yet, so the recursive
 * addresses cannot reach it and it has to be looked at some other way.
 * PML4 slot 384, which nothing else uses. */
#define SCRATCH_VA  0xFFFFC00000001000ULL

static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ __volatile__("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void write_cr3(uint64_t v) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(v) : "memory");
}

uint64_t vmspace64_current_phys(void) { return read_cr3() & ADDR_MASK; }

void vmspace64_kernel_space(vmspace64_t* vs) {
    vs->pml4_phys = read_cr3() & ADDR_MASK;
}

void vmspace64_switch(const vmspace64_t* vs) {
    write_cr3(vs->pml4_phys);
}

int vmspace64_create(vmspace64_t* vs) {
    uint64_t frame = pmm64_alloc_frame();
    uint64_t* fresh;
    const uint64_t* current = paging64_pml4();
    int i;

    if (!frame) return 0;

    if (paging64_map(SCRATCH_VA, frame, PAGE64_PRESENT | PAGE64_WRITE)
            != PAGING64_OK) {
        pmm64_free_frame(frame);
        return 0;
    }
    fresh = (uint64_t*)SCRATCH_VA;

    for (i = 0; i < 512; i++) fresh[i] = 0;

    /* The kernel half, shared rather than copied deeper: these entries
     * point at the same PDPTs the kernel is already using, so a change to
     * a kernel mapping is visible in every space without having to walk
     * them all. */
    for (i = KERNEL_HALF; i < 512; i++) fresh[i] = current[i];

    /* Its own self-reference, which has to be its own and not the one
     * just copied from the kernel - that would leave every space editing
     * the kernel's tables through what it thought were its own. */
    fresh[REC] = frame | PAGE64_PRESENT | PAGE64_WRITE;

    paging64_unmap(SCRATCH_VA);
    vs->pml4_phys = frame;
    return 1;
}

int vmspace64_map(vmspace64_t* vs, uint64_t virt, uint64_t phys,
                  uint64_t flags) {
    uint64_t saved = read_cr3();
    int rc;

    if ((saved & ADDR_MASK) == vs->pml4_phys) return paging64_map(virt, phys, flags);

    write_cr3(vs->pml4_phys);
    rc = paging64_map(virt, phys, flags);
    write_cr3(saved);
    return rc;
}

/* Frees the page tables under the low half. Runs with the space loaded,
 * which is safe for exactly the reason the header gives: everything this
 * code is standing on lives in the high half, and only the low half is
 * being taken apart. */
static void free_low_half_tables(void) {
    uint64_t* pml4 = paging64_pml4();
    int a, b, c;

    for (a = 0; a < KERNEL_HALF; a++) {
        uint64_t pml4e = pml4[a];
        uint64_t* pdpt;
        if (!(pml4e & PAGE64_PRESENT)) continue;

        /* The recursive addresses for a table one, two and three levels
         * down - the same construction paging64.c documents, with the
         * indices of the region being walked substituted in. */
        pdpt = (uint64_t*)(0xFFFF000000000000ULL | (REC << 39) | (REC << 30)
                           | (REC << 21) | ((uint64_t)a << 12));
        for (b = 0; b < 512; b++) {
            uint64_t pdpte = pdpt[b];
            uint64_t* pd;
            if (!(pdpte & PAGE64_PRESENT) || (pdpte & PAGE64_HUGE)) continue;

            pd = (uint64_t*)(0xFFFF000000000000ULL | (REC << 39) | (REC << 30)
                             | ((uint64_t)a << 21) | ((uint64_t)b << 12));
            for (c = 0; c < 512; c++) {
                uint64_t pde = pd[c];
                if (!(pde & PAGE64_PRESENT) || (pde & PAGE64_HUGE)) continue;
                /* The page table itself. Its entries point at pages this
                 * layer does not own, so only the table is freed. */
                pmm64_free_frame(pde & ADDR_MASK);
            }
            pmm64_free_frame(pdpte & ADDR_MASK);
        }
        pmm64_free_frame(pml4e & ADDR_MASK);
        pml4[a] = 0;
    }
}

void vmspace64_destroy(vmspace64_t* vs) {
    uint64_t saved = read_cr3();

    if (!vs->pml4_phys) return;
    /* Destroying the space you are standing in would unmap the tables
     * being walked partway through walking them. */
    if ((saved & ADDR_MASK) == vs->pml4_phys) return;

    write_cr3(vs->pml4_phys);
    free_low_half_tables();
    write_cr3(saved);

    pmm64_free_frame(vs->pml4_phys);
    vs->pml4_phys = 0;
}
