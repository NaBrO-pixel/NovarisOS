#ifndef PAGING64_H
#define PAGING64_H

#include <stdint.h>

/* Four-level paging (PML4 -> PDPT -> PD -> PT), and the recursive-mapping
 * trick reworked for it.
 *
 * The 32-bit paging.c maps its own page directory into itself so that any
 * page table can be edited through a computed virtual address rather than
 * needing a physical-to-virtual mapping that already exists. The same trick
 * works on four levels, and it is worth more here than it was there: a
 * newly allocated page table can sit at any physical address the frame
 * allocator chooses, including above the one gigabyte boot64.s mapped, and
 * the recursive address reaches it regardless.
 *
 * PML4 slot 510 is the self-reference. Slot 0 is the boot identity map and
 * slot 511 is the higher half, so 510 is free, and being >= 256 it makes
 * every derived address canonical without further care. */

#define PAGE64_SIZE      4096ULL

#define PAGE64_PRESENT   (1ULL << 0)
#define PAGE64_WRITE     (1ULL << 1)
#define PAGE64_USER      (1ULL << 2)
#define PAGE64_PWT       (1ULL << 3)
#define PAGE64_PCD       (1ULL << 4)
#define PAGE64_ACCESSED  (1ULL << 5)
#define PAGE64_DIRTY     (1ULL << 6)
#define PAGE64_HUGE      (1ULL << 7)
#define PAGE64_GLOBAL    (1ULL << 8)
#define PAGE64_NX        (1ULL << 63)

#define PAGING64_OK            0
#define PAGING64_NOMEM        -1   /* no frame for an intermediate table */
#define PAGING64_HUGE_IN_WAY  -2   /* a 2MB/1GB page already covers it */
#define PAGING64_NOT_MAPPED   -3

/* Installs the self-reference. Must run after pmm64_init, because every
 * map() after it may need a frame. */
/* The direct map: every byte of physical memory, readable by the kernel
 * at a fixed offset.
 *
 * Until Milestone 66 the kernel could only reach the first gigabyte -
 * the window boot64.s builds at KERNEL_VMA - so every frame handed to a
 * user process had to come from below that line, because the kernel has
 * to zero it, copy into it, or load a program into it. chrome.dll alone
 * is 285MB; the ceiling was not a tuning parameter, it was a wall.
 *
 * PML4 slot 272. Linux puts its own direct map two slots along, for the
 * same reason: it is high-half, so it is shared by every address space,
 * and it is nowhere near anything else this kernel uses. */
#define PHYSMAP64_BASE 0xFFFF880000000000ULL

static inline void* phys64_to_virt(uint64_t phys) {
    return (void*)(PHYSMAP64_BASE + phys);
}

void paging64_init(void);

/* Builds the direct map over `bytes` of physical memory, in 2MB pages.
 * Must run after pmm64_init, which is what knows how much there is. */
int  paging64_physmap_init(uint64_t bytes);

/* Maps one 2MB page. The direct map would need 500,000 page tables at
 * 4KB granularity for a modest machine, and four at 2MB. */
int  paging64_map_huge(uint64_t virt, uint64_t phys, uint64_t flags);

uint64_t paging64_physmap_bytes(void);

int  paging64_map(uint64_t virt, uint64_t phys, uint64_t flags);
int  paging64_unmap(uint64_t virt);

/* Resolves virt the way the hardware would, huge pages included, and
 * writes the physical address (offset within the page kept) to phys_out. */
int  paging64_translate(uint64_t virt, uint64_t* phys_out);

/* The PML4 at its own recursive address - slot 510 followed four times.
 * Exposed so that callers (and the bring-up test) never hand-compute it;
 * the constant is easy to get right once and impossible to notice going
 * wrong later. */
uint64_t* paging64_pml4(void);

uint64_t paging64_tables_allocated(void);

#endif
