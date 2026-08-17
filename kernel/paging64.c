/* paging64.c - four levels, and a page directory that contains itself.
 *
 * The addresses below are the whole of this file's difficulty, so they are
 * derived here rather than left as constants to be trusted.
 *
 * With PML4 slot R mapped to the PML4 itself, a walk that would normally
 * end at a data page ends one level early - at a page table - because the
 * hardware spends one of its four steps going round the self-reference
 * again. Putting R in the top index and shifting the real indices down one
 * level each therefore addresses the page *table* for a given virtual
 * address; using R twice addresses the page directory, three times the
 * PDPT, and four times the PML4 itself:
 *
 *   PT   for v : R  | PML4(v) | PDPT(v) | PD(v)
 *   PD   for v : R  | R       | PML4(v) | PDPT(v)
 *   PDPT for v : R  | R       | R       | PML4(v)
 *   PML4       : R  | R       | R       | R
 *
 * where each field is 9 bits and they sit at shifts 39, 30, 21 and 12.
 *
 * The consequence worth stating: this edits the *current* address space
 * only. Editing another process's tables needs either a temporary mapping
 * or a second recursive slot, and that is a problem for whichever milestone
 * first has two address spaces. */

#include "paging64.h"
#include "pmm64.h"

#define REC        510ULL
#define SIGN_EXT   0xFFFF000000000000ULL
#define ADDR_MASK  0x000FFFFFFFFFF000ULL   /* physical address bits of a PTE */

#define PML4_IDX(v) (((v) >> 39) & 0x1FFULL)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FFULL)
#define PD_IDX(v)   (((v) >> 21) & 0x1FFULL)
#define PT_IDX(v)   (((v) >> 12) & 0x1FFULL)

static uint64_t tables_allocated;

static inline uint64_t* pml4_addr(void) {
    return (uint64_t*)(SIGN_EXT | (REC << 39) | (REC << 30)
                                | (REC << 21) | (REC << 12));
}
static inline uint64_t* pdpt_addr(uint64_t v) {
    return (uint64_t*)(SIGN_EXT | (REC << 39) | (REC << 30)
                                | (REC << 21) | (PML4_IDX(v) << 12));
}
static inline uint64_t* pd_addr(uint64_t v) {
    return (uint64_t*)(SIGN_EXT | (REC << 39) | (REC << 30)
                                | (PML4_IDX(v) << 21) | (PDPT_IDX(v) << 12));
}
static inline uint64_t* pt_addr(uint64_t v) {
    return (uint64_t*)(SIGN_EXT | (REC << 39) | (PML4_IDX(v) << 30)
                                | (PDPT_IDX(v) << 21) | (PD_IDX(v) << 12));
}

static inline void invlpg(uint64_t v) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(v) : "memory");
}

static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ __volatile__("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void write_cr3(uint64_t v) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(v) : "memory");
}

static void zero_table(uint64_t* table) {
    for (int i = 0; i < 512; i++) table[i] = 0;
}

void paging64_init(void) {
    /* boot64.s left CR3 pointing at a PML4 inside the kernel image, which
     * is below 1GB and therefore reachable through the KERNEL_VMA window -
     * the one moment this file can touch a table without the recursion it
     * is about to install. */
    uint64_t pml4_phys = read_cr3() & ADDR_MASK;
    uint64_t* boot_pml4 = (uint64_t*)(0xFFFFFFFF80000000ULL + pml4_phys);

    boot_pml4[REC] = pml4_phys | PAGE64_PRESENT | PAGE64_WRITE;

    /* The self-reference changes what a whole span of virtual addresses
     * means, and the TLB may hold stale translations for it. */
    write_cr3(pml4_phys);
    tables_allocated = 0;
}

/* Make sure entry[idx] of a table points at a present next level, creating
 * it if it does not. `child` is the recursive address the new table will
 * have once installed, which is where it gets zeroed from - it cannot be
 * touched before the parent entry exists. */
static int ensure_table(uint64_t* table, uint64_t idx,
                        uint64_t* child, uint64_t flags) {
    uint64_t frame;

    if (table[idx] & PAGE64_PRESENT) {
        /* Widen the intermediate entry if this mapping needs it. The
         * hardware ANDs the permission bits down the whole walk, so a
         * user mapping under a supervisor-only PDPT is not reachable. */
        table[idx] |= flags & (PAGE64_WRITE | PAGE64_USER);
        return PAGING64_OK;
    }

    frame = pmm64_alloc_frame();
    if (!frame) return PAGING64_NOMEM;

    table[idx] = frame | PAGE64_PRESENT | PAGE64_WRITE
                       | (flags & PAGE64_USER);
    invlpg((uint64_t)child);
    zero_table(child);
    tables_allocated++;
    return PAGING64_OK;
}

int paging64_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t* pml4 = pml4_addr();
    uint64_t* pdpt = pdpt_addr(virt);
    uint64_t* pd   = pd_addr(virt);
    uint64_t* pt   = pt_addr(virt);
    int rc;

    virt &= ~(PAGE64_SIZE - 1);
    phys &= ~(PAGE64_SIZE - 1);

    rc = ensure_table(pml4, PML4_IDX(virt), pdpt, flags);
    if (rc != PAGING64_OK) return rc;

    /* A 1GB page here would mean this whole slot is already backed and
     * there is no PD to descend into. Splitting it is a real operation and
     * not one this milestone needs, so say so rather than corrupt it. */
    if (pdpt[PDPT_IDX(virt)] & PAGE64_HUGE) return PAGING64_HUGE_IN_WAY;

    rc = ensure_table(pdpt, PDPT_IDX(virt), pd, flags);
    if (rc != PAGING64_OK) return rc;

    /* Same for the 2MB pages boot64.s built the higher half out of. */
    if (pd[PD_IDX(virt)] & PAGE64_HUGE) return PAGING64_HUGE_IN_WAY;

    rc = ensure_table(pd, PD_IDX(virt), pt, flags);
    if (rc != PAGING64_OK) return rc;

    pt[PT_IDX(virt)] = phys | flags | PAGE64_PRESENT;
    invlpg(virt);
    return PAGING64_OK;
}

int paging64_unmap(uint64_t virt) {
    uint64_t* pml4 = pml4_addr();
    uint64_t* pdpt = pdpt_addr(virt);
    uint64_t* pd   = pd_addr(virt);
    uint64_t* pt   = pt_addr(virt);

    virt &= ~(PAGE64_SIZE - 1);

    if (!(pml4[PML4_IDX(virt)] & PAGE64_PRESENT)) return PAGING64_NOT_MAPPED;
    if (!(pdpt[PDPT_IDX(virt)] & PAGE64_PRESENT)) return PAGING64_NOT_MAPPED;
    if (pdpt[PDPT_IDX(virt)] & PAGE64_HUGE)       return PAGING64_HUGE_IN_WAY;
    if (!(pd[PD_IDX(virt)] & PAGE64_PRESENT))     return PAGING64_NOT_MAPPED;
    if (pd[PD_IDX(virt)] & PAGE64_HUGE)           return PAGING64_HUGE_IN_WAY;
    if (!(pt[PT_IDX(virt)] & PAGE64_PRESENT))     return PAGING64_NOT_MAPPED;

    pt[PT_IDX(virt)] = 0;
    invlpg(virt);

    /* The intermediate tables are deliberately left in place. Freeing a
     * page table the moment its last entry goes means reallocating it the
     * moment the next mapping arrives, and the bookkeeping to know it is
     * empty costs more than the frame does. */
    return PAGING64_OK;
}

int paging64_translate(uint64_t virt, uint64_t* phys_out) {
    uint64_t* pml4 = pml4_addr();
    uint64_t* pdpt = pdpt_addr(virt);
    uint64_t* pd   = pd_addr(virt);
    uint64_t* pt   = pt_addr(virt);
    uint64_t e;

    if (!(pml4[PML4_IDX(virt)] & PAGE64_PRESENT)) return PAGING64_NOT_MAPPED;

    e = pdpt[PDPT_IDX(virt)];
    if (!(e & PAGE64_PRESENT)) return PAGING64_NOT_MAPPED;
    if (e & PAGE64_HUGE) {                       /* 1GB page */
        *phys_out = (e & ADDR_MASK & ~0x3FFFFFFFULL) | (virt & 0x3FFFFFFFULL);
        return PAGING64_OK;
    }

    e = pd[PD_IDX(virt)];
    if (!(e & PAGE64_PRESENT)) return PAGING64_NOT_MAPPED;
    if (e & PAGE64_HUGE) {                       /* 2MB page */
        *phys_out = (e & ADDR_MASK & ~0x1FFFFFULL) | (virt & 0x1FFFFFULL);
        return PAGING64_OK;
    }

    e = pt[PT_IDX(virt)];
    if (!(e & PAGE64_PRESENT)) return PAGING64_NOT_MAPPED;
    *phys_out = (e & ADDR_MASK) | (virt & (PAGE64_SIZE - 1));
    return PAGING64_OK;
}

#define HUGE_SIZE 0x200000ULL

int paging64_map_huge(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t* pml4 = pml4_addr();
    uint64_t* pdpt = pdpt_addr(virt);
    uint64_t* pd   = pd_addr(virt);
    int rc;

    virt &= ~(HUGE_SIZE - 1);
    phys &= ~(HUGE_SIZE - 1);

    rc = ensure_table(pml4, PML4_IDX(virt), pdpt, flags);
    if (rc != PAGING64_OK) return rc;
    if (pdpt[PDPT_IDX(virt)] & PAGE64_HUGE) return PAGING64_HUGE_IN_WAY;

    rc = ensure_table(pdpt, PDPT_IDX(virt), pd, flags);
    if (rc != PAGING64_OK) return rc;

    /* PS in a PD entry means "this is 2MB of memory, not a page table".
     * There is no level below it to build. */
    pd[PD_IDX(virt)] = phys | flags | PAGE64_PRESENT | PAGE64_HUGE;
    invlpg(virt);
    return PAGING64_OK;
}

static uint64_t physmap_bytes;

uint64_t paging64_physmap_bytes(void) { return physmap_bytes; }

int paging64_physmap_init(uint64_t bytes) {
    uint64_t covered;

    /* Rounded up: a machine whose last megabyte is not a whole 2MB page
     * still has that megabyte, and mapping past the end of RAM is
     * harmless - nothing hands out those frames. */
    bytes = (bytes + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);

    for (covered = 0; covered < bytes; covered += HUGE_SIZE) {
        if (paging64_map_huge(PHYSMAP64_BASE + covered, covered,
                              PAGE64_PRESENT | PAGE64_WRITE)
                != PAGING64_OK) {
            physmap_bytes = covered;
            return 0;
        }
    }

    physmap_bytes = bytes;
    return 1;
}

uint64_t* paging64_pml4(void) { return pml4_addr(); }

uint64_t paging64_tables_allocated(void) { return tables_allocated; }
