#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>
#include <stdint.h>

/* Where the heap lives and how far it can grow. Public because the page
 * tables covering this range have to be pre-created before the kernel
 * address space is frozen (see paging_reserve_kernel_tables): the heap is
 * the one kernel range that gets bigger after boot, and a page directory
 * entry added later would exist only in whichever address space happened
 * to be loaded at the time. kheap.c uses these directly, so there is one
 * definition rather than two that can drift. */
#define KHEAP_VIRTUAL_BASE 0xD0000000u
#define KHEAP_MAX_SIZE     (48u * 1024u * 1024u)

/* Maps in the heap's initial pages. Must run after paging_init(). */
void kheap_init(void);

/* Simple first-fit allocator over the kernel heap. Returns NULL if the
 * heap's max size is exhausted. */
void* kmalloc(size_t size);

/* Bytes in use and bytes currently mapped, for Task Manager. */
void kheap_stats(uint32_t* used, uint32_t* total);

/* Frees a block previously returned by kmalloc() and coalesces it with
 * any adjacent free blocks. */
void kfree(void* ptr);

#endif
