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

void vmspace64_switch(const vmspace64_t* vs);

/* The space the CPU is in, as a physical PML4 address (CR3 without its
 * flag bits). Captured at boot for the kernel's own space. */
uint64_t vmspace64_current_phys(void);
void     vmspace64_kernel_space(vmspace64_t* vs);

/* Map into a space that may not be the current one. */
int  vmspace64_map(vmspace64_t* vs, uint64_t virt, uint64_t phys,
                   uint64_t flags);

#endif
