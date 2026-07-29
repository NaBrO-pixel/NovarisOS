#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>
#include <stdint.h>

/* Maps in the heap's initial pages. Must run after paging_init(). */
void kheap_init(void);

/* Simple first-fit allocator over the kernel heap. Returns NULL if the
 * heap's max size is exhausted. */
void* kmalloc(size_t size);

/* Bytes in use and bytes currently mapped, for Activity Monitor. */
void kheap_stats(uint32_t* used, uint32_t* total);

/* Frees a block previously returned by kmalloc() and coalesces it with
 * any adjacent free blocks. */
void kfree(void* ptr);

#endif
