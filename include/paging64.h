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
void paging64_init(void);

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
