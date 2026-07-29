#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>

/* Maps in the heap's initial pages. Must run after paging_init(). */
void kheap_init(void);

/* Simple first-fit allocator over the kernel heap. Returns NULL if the
 * heap's max size is exhausted. */
void* kmalloc(size_t size);

/* Frees a block previously returned by kmalloc() and coalesces it with
 * any adjacent free blocks. */
void kfree(void* ptr);

#endif
