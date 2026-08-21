#ifndef VMSPACE64_H
#define VMSPACE64_H

#include <stdint.h>

/* Address spaces: one PML4 per process, sharing the kernel.
 *
 * The split is the usual one and it is decided by the sign bit. PML4
 * slots 0-255 are the low half and belong to the process; slots 256-511
 * are the high half and are the kernel, identical in every space, so that
 * a syscall or an interrupt does not have to change CR3 to find the
 * kernel it is entering. Creating a space therefore means copying the
 * kernel's high-half entries and leaving the low half empty.
 *
 * Editing a space that is not the current one is the interesting problem.
 * paging64.c reaches page tables through the recursive slot, which by
 * construction only describes the *current* address space. The classic
 * answer is a second recursive slot pointing at the other space's PML4,
 * with every derived address shifted one level down.
 *
 * This does the simpler thing: switch CR3 to the space being edited, use
 * the ordinary mapping code, and switch back. That is safe precisely
 * because of the split above - the kernel's code, stack, heap and frame
 * bitmap are all in the high half, which is mapped identically in both
 * spaces, so the switch changes nothing the mapping code is standing on.
 * It costs two CR3 loads per edit, which matters when a process is being
 * built and never again. */

typedef struct vmspace64 {
    uint64_t pml4_phys;
} vmspace64_t;

/* Fills in `vs` with a fresh space sharing this one's kernel half.
 * Returns 0 if a frame could not be had. */
int  vmspace64_create(vmspace64_t* vs);

/* Frees the space's *page tables* and its PML4. The frames those tables
 * pointed at are not freed: this layer does not know whether they are a
 * program's pages, a shared buffer or something mapped from a device, so
 * ownership stays with whoever mapped them. */
void vmspace64_destroy(vmspace64_t* vs);

/* Copies the CURRENT address space's low half into `dst`, which must be
 * a space fresh from vmspace64_create.
 *
 * A real fork shares the pages copy-on-write and duplicates them only
 * when one side writes; this copies everything up front. That is
 * correct, much simpler, and much more expensive - a fork costs the
 * whole resident size of the process. It is the right trade while the
 * question is whether fork works at all, and the wrong one as soon as
 * anything forks in a loop.
 */
int  vmspace64_clone(vmspace64_t* dst);

/* The same thing, sharing rather than copying (Milestone 69).
 *
 * Every page of the source's low half is mapped into `dst` as well, both
 * sides made read-only and marked PAGE64_COW, and the frame gains an
 * owner. The copy happens in the page fault handler, one page at a time,
 * and only for pages somebody actually writes.
 *
 * It walks the tables through the direct map, so - unlike the eager
 * clone - it needs no intermediate page list and has no size ceiling.
 * `src_pml4` is a physical PML4 address, normally the current one.
 */
int  vmspace64_clone_cow(uint64_t src_pml4, vmspace64_t* dst);

/* Handles a write fault on a shared page: copies it if anyone else still
 * holds it, reclaims it if not. Returns 0 if `va` was not a
 * copy-on-write page, which means the fault was a real one. */
int  vmspace64_break_cow(uint64_t va);

/* Changes whether one already-mapped page of the current space may be
 * written. Returns 0 if nothing is mapped at `va`, which mprotect(2)
 * treats as "nothing to do" rather than an error.
 *
 * It lives here rather than in uspace64.c because a shared page cannot
 * simply be made writable: doing that would hand the same frame to a
 * parent and a child that fork separated. On a copy-on-write page the
 * write bit stays clear whatever the caller asked for, and the request
 * is recorded in PAGE64_COW_RW - which is precisely the bit break_cow
 * consults to tell a page it should copy from a page that is genuinely
 * read-only. */
int  vmspace64_set_writable(uint64_t va, int writable);

void vmspace64_switch(const vmspace64_t* vs);

/* The space the CPU is in, as a physical PML4 address (CR3 without its
 * flag bits). Captured at boot for the kernel's own space. */
uint64_t vmspace64_current_phys(void);
void     vmspace64_kernel_space(vmspace64_t* vs);

/* Map into a space that may not be the current one. */
int  vmspace64_map(vmspace64_t* vs, uint64_t virt, uint64_t phys,
                   uint64_t flags);

#endif
