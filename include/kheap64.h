#ifndef KHEAP64_H
#define KHEAP64_H

#include <stdint.h>
#include <stddef.h>

/* The 64-bit kernel heap.
 *
 * Not a port of kheap.c. That allocator keeps one singly-linked list of
 * every block, used and free alike, and kmalloc walks it from the start -
 * so an allocation costs the number of allocations before it, and N
 * allocations cost N squared. That is not a theoretical complaint:
 * Milestone 43 measured Wine's syscall rate collapsing by twenty-one times
 * partway through startup and traced it here.
 *
 * So this one keeps two structures instead. Blocks are doubly linked in
 * address order, which makes coalescing a free block with its neighbours
 * O(1) instead of a scan; and free blocks are additionally on a free list,
 * which is the only thing kmalloc walks. The free-list pointers live in
 * the payload of the free block itself, so they cost nothing in a block
 * that is in use - which is why the minimum allocation is 16 bytes.
 *
 * There is no console in the 64-bit tree yet, so exhaustion is reported by
 * returning NULL and by the counters below, not by printing. */

#define KHEAP64_BASE      0xFFFF900000000000ULL   /* PML4 slot 288, unused */
#define KHEAP64_MAX_SIZE  (256ULL * 1024 * 1024)
#define KHEAP64_INITIAL   (64ULL * 1024)
#define KHEAP64_ALIGN     16ULL   /* the SysV ABI's alignment for xmm data */

void  kheap64_init(void);
void* kmalloc64(size_t size);
void  kfree64(void* ptr);

/* Bytes currently in used blocks, and bytes currently mapped. */
void  kheap64_stats(uint64_t* used, uint64_t* total);

/* Free-list steps taken and calls made. The ratio is the thing worth
 * looking at: it is what tells you the allocator is not quadratic, and it
 * is why the counters exist rather than being inferred from a stopwatch. */
void  kheap64_walk_stats(uint64_t* steps, uint64_t* calls);

/* Freeing a pointer that is already free is counted, not obeyed. */
uint64_t kheap64_double_frees(void);

/* How many blocks are on the free list. This is the only way to observe
 * coalescing from outside: a heap that merges properly returns to the same
 * free-block count after a batch of allocations is freed, and one that
 * merely marks blocks unused does not. */
uint64_t kheap64_free_blocks(void);

#endif
