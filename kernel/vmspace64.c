/* vmspace64.c - one PML4 per process, sharing the kernel's half. */

#include "vmspace64.h"
#include "paging64.h"
#include "pmm64.h"
#include "kheap64.h"
#include "kstring.h"

#define KERNEL_VMA  0xFFFFFFFF80000000ULL

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
    /* Low on purpose, unlike the pages this space will hold: a PML4 is a
     * kernel structure, and keeping page tables out of the high pool
     * leaves that pool contiguous for the things that are large. */
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

/* One page of the source, remembered while the source space is still
 * loaded. The walk and the mapping cannot happen at the same time - the
 * recursive addresses only ever describe the current space - so the
 * whole low half is collected first and replayed second. */
typedef struct {
    uint64_t va;
    uint64_t frame;
    uint64_t flags;
} clone_page_t;

#define CLONE_MAX_PAGES 4096

/* --- copy-on-write (Milestone 69) ------------------------------------
 *
 * The clone below copies every page eagerly, and its page list caps at
 * CLONE_MAX_PAGES - 4096 entries, so 16MB of address space. Both are
 * fatal for Wine: wineserver forks, and a process with a prefix mapped
 * is far past 16MB before it does.
 *
 * This walks the tables through the direct map instead of through the
 * recursive addresses. That is the whole reason it needs no page list
 * and has no ceiling: the recursive mapping only ever describes the
 * space that is loaded, which is why the eager version had to collect
 * the source into a list and replay it into the destination. Since
 * Milestone 66 every physical address is reachable at PHYSMAP64_BASE +
 * phys, so both sets of tables can be read and written at once, and
 * neither space has to be the current one.
 */

static uint64_t* table_at(uint64_t phys) {
    return (uint64_t*)phys64_to_virt(phys);
}

/* The PTE for `va` in the space rooted at `pml4_phys`, creating the
 * tables above it when asked. Returns 0 if it is not there, or if a
 * huge page is in the way - this kernel maps user memory in 4KB pages
 * and a 2MB entry down here would mean something else built it. */
static uint64_t* pte_for(uint64_t pml4_phys, uint64_t va, int create) {
    uint64_t idx[4];
    uint64_t* t = table_at(pml4_phys);

    idx[0] = (va >> 39) & 511;
    idx[1] = (va >> 30) & 511;
    idx[2] = (va >> 21) & 511;
    idx[3] = (va >> 12) & 511;

    for (int lvl = 0; lvl < 3; lvl++) {
        uint64_t e = t[idx[lvl]];

        if (!(e & PAGE64_PRESENT)) {
            uint64_t fresh;
            uint64_t* nt;

            if (!create) return 0;
            fresh = pmm64_alloc_frame();
            if (!fresh) return 0;

            nt = table_at(fresh);
            for (int i = 0; i < 512; i++) nt[i] = 0;

            /* Permissive here and restrictive at the leaf: the hardware
             * ANDs the permissions down the walk, so the last level is
             * where read-only and user actually get decided. */
            t[idx[lvl]] = fresh | PAGE64_PRESENT | PAGE64_WRITE | PAGE64_USER;
            e = t[idx[lvl]];
        }
        if (e & PAGE64_HUGE) return 0;
        t = table_at(e & ADDR_MASK);
    }
    return &t[idx[3]];
}

/* What a shared mapping looks like: present, not writable, marked COW,
 * and remembering whether it had been writable. */
static uint64_t share_flags(uint64_t pte) {
    uint64_t f = (pte & ~ADDR_MASK) & ~PAGE64_WRITE;

    f |= PAGE64_COW;
    if (pte & PAGE64_WRITE) f |= PAGE64_COW_RW;
    return f;
}

int vmspace64_clone_cow(uint64_t src_pml4, vmspace64_t* dst) {
    uint64_t* pml4 = table_at(src_pml4);
    int a, b, c, d;

    for (a = 0; a < KERNEL_HALF; a++) {
        uint64_t* pdpt;
        if (!(pml4[a] & PAGE64_PRESENT)) continue;
        if (pml4[a] & PAGE64_HUGE) return 0;
        pdpt = table_at(pml4[a] & ADDR_MASK);

        for (b = 0; b < 512; b++) {
            uint64_t* pd;
            if (!(pdpt[b] & PAGE64_PRESENT) || (pdpt[b] & PAGE64_HUGE)) continue;
            pd = table_at(pdpt[b] & ADDR_MASK);

            for (c = 0; c < 512; c++) {
                uint64_t* pt;
                if (!(pd[c] & PAGE64_PRESENT) || (pd[c] & PAGE64_HUGE)) continue;
                pt = table_at(pd[c] & ADDR_MASK);

                for (d = 0; d < 512; d++) {
                    uint64_t pte = pt[d];
                    uint64_t va, frame;
                    uint64_t* dpte;

                    if (!(pte & PAGE64_PRESENT)) continue;

                    /* The low half is canonical as it stands; nothing
                     * below slot 256 needs sign extending. */
                    va = ((uint64_t)a << 39) | ((uint64_t)b << 30)
                       | ((uint64_t)c << 21) | ((uint64_t)d << 12);
                    frame = pte & ADDR_MASK;

                    dpte = pte_for(dst->pml4_phys, va, 1);
                    if (!dpte) return 0;

                    if (!pmm64_owns(frame)) {
                        /* Not RAM this allocator manages - the mapped
                         * framebuffer is the case that exists. It is
                         * shared by definition and there is nothing to
                         * count, so it is handed over as it stands. */
                        *dpte = pte;
                        continue;
                    }

                    if (pmm64_ref_frame(frame)) {
                        uint64_t shared = share_flags(pte);
                        /* Both sides, because a parent that kept its
                         * writable mapping would write straight through
                         * into the child. */
                        pt[d] = frame | shared;
                        *dpte = frame | shared;
                    } else {
                        /* The count saturated. Copying is always
                         * correct; only the sharing was an
                         * optimisation. */
                        uint64_t fresh = pmm64_alloc_high();
                        if (!fresh) return 0;
                        kmemcpy(phys64_to_virt(fresh),
                                (const void*)phys64_to_virt(frame),
                                PAGE64_SIZE);
                        *dpte = fresh | (pte & ~ADDR_MASK);
                    }
                }
            }
        }
    }

    /* The source's mappings just lost their write bit and the TLB does
     * not know. Reloading cr3 is the blunt version and the right one
     * here: fork has just touched an unbounded number of pages. */
    write_cr3(read_cr3());
    return 1;
}

/* A write to a shared page. Returns 1 if it dealt with it, 0 if this was
 * not a copy-on-write fault at all and the caller should treat it as the
 * real fault it is. */
int vmspace64_set_writable(uint64_t va, int writable) {
    uint64_t pml4 = read_cr3() & ADDR_MASK;
    uint64_t* pte = pte_for(pml4, va & ~(PAGE64_SIZE - 1), 0);
    uint64_t e;

    if (!pte) return 0;
    e = *pte;
    if (!(e & PAGE64_PRESENT)) return 0;

    if (e & PAGE64_COW) {
        /* Shared. The write bit must stay clear so that the next write
         * still traps into break_cow and gets a copy of its own; what
         * changes is only whether break_cow will then oblige. */
        if (writable) e |= PAGE64_COW_RW;
        else          e &= ~PAGE64_COW_RW;
        e &= ~PAGE64_WRITE;
    } else {
        if (writable) e |= PAGE64_WRITE;
        else          e &= ~PAGE64_WRITE;
    }

    *pte = e;
    __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
    return 1;
}

int vmspace64_break_cow(uint64_t va) {
    uint64_t pml4 = read_cr3() & ADDR_MASK;
    uint64_t* pte = pte_for(pml4, va & ~(PAGE64_SIZE - 1), 0);
    uint64_t e, frame, flags;

    if (!pte) return 0;
    e = *pte;
    if (!(e & PAGE64_PRESENT) || !(e & PAGE64_COW)) return 0;

    /* Shared, but it was never writable. A write to it is a real fault
     * and must stay one - this is what the second bit is for. */
    if (!(e & PAGE64_COW_RW)) return 0;

    frame = e & ADDR_MASK;
    flags = ((e & ~ADDR_MASK) & ~(PAGE64_COW | PAGE64_COW_RW)) | PAGE64_WRITE;

    if (pmm64_frame_owners(frame) > 1) {
        uint64_t fresh = pmm64_alloc_high();
        if (!fresh) return 0;

        kmemcpy(phys64_to_virt(fresh),
                (const void*)phys64_to_virt(frame), PAGE64_SIZE);
        pmm64_free_frame(frame);          /* drops an owner, not the frame */
        *pte = fresh | flags;
    } else {
        /* The last owner. There is nothing left to copy it away from, so
         * the page is simply taken back - which is what makes a fork
         * whose child has exited cost nothing to undo. */
        *pte = frame | flags;
    }

    __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
    return 1;
}

int vmspace64_clone(vmspace64_t* dst) {
    uint64_t* pml4 = paging64_pml4();
    clone_page_t* list;
    uint64_t count = 0, saved;
    int a, b, c, d, rc = 1;

    list = (clone_page_t*)kmalloc64(CLONE_MAX_PAGES * sizeof(clone_page_t));
    if (!list) return 0;

    /* --- pass one: what is mapped, read from the source space --- */
    for (a = 0; a < KERNEL_HALF && rc; a++) {
        uint64_t* pdpt;
        if (!(pml4[a] & PAGE64_PRESENT)) continue;
        pdpt = (uint64_t*)(0xFFFF000000000000ULL | (REC << 39) | (REC << 30)
                           | (REC << 21) | ((uint64_t)a << 12));

        for (b = 0; b < 512 && rc; b++) {
            uint64_t* pd;
            if (!(pdpt[b] & PAGE64_PRESENT) || (pdpt[b] & PAGE64_HUGE)) continue;
            pd = (uint64_t*)(0xFFFF000000000000ULL | (REC << 39) | (REC << 30)
                             | ((uint64_t)a << 21) | ((uint64_t)b << 12));

            for (c = 0; c < 512 && rc; c++) {
                uint64_t* pt;
                if (!(pd[c] & PAGE64_PRESENT) || (pd[c] & PAGE64_HUGE)) continue;
                pt = (uint64_t*)(0xFFFF000000000000ULL | (REC << 39)
                                 | ((uint64_t)a << 30) | ((uint64_t)b << 21)
                                 | ((uint64_t)c << 12));

                for (d = 0; d < 512; d++) {
                    if (!(pt[d] & PAGE64_PRESENT)) continue;
                    if (count == CLONE_MAX_PAGES) { rc = 0; break; }
                    list[count].va = ((uint64_t)a << 39) | ((uint64_t)b << 30)
                                   | ((uint64_t)c << 21) | ((uint64_t)d << 12);
                    list[count].frame = pt[d] & ADDR_MASK;
                    /* Permissions carry over; the copy is a copy. */
                    list[count].flags = pt[d] & (PAGE64_PRESENT | PAGE64_WRITE
                                                 | PAGE64_USER);
                    count++;
                }
            }
        }
    }

    /* --- pass two: build the same layout in the destination --- */
    if (rc) {
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(saved));
        __asm__ __volatile__("mov %0, %%cr3" :: "r"(dst->pml4_phys) : "memory");

        for (uint64_t i = 0; i < count; i++) {
            uint64_t fresh = pmm64_alloc_high();
            if (!fresh) {
                if (fresh) pmm64_free_frame(fresh);
                rc = 0;
                break;
            }
            /* Both frames are reachable through the kernel window, which
             * is mapped identically in every space - so the copy does
             * not care which space is loaded. */
            kmemcpy(phys64_to_virt(fresh),
                    (const void*)phys64_to_virt(list[i].frame), PAGE64_SIZE);

            if (paging64_map(list[i].va, fresh, list[i].flags)
                    != PAGING64_OK) {
                pmm64_free_frame(fresh);
                rc = 0;
                break;
            }
        }

        __asm__ __volatile__("mov %0, %%cr3" :: "r"(saved) : "memory");
    }

    kfree64(list);
    return rc;
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
