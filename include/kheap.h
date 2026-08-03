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
/* Raised from 48MB in Milestone 32, and the number came from a failure.
 *
 * A Wine prefix on the disk means the DLLs a Windows program maps are
 * read off the disk into the heap - there is no page cache, so a mapping
 * is the file's bytes and the file's bytes are heap. shell32.dll alone is
 * nine megabytes, user32 is another two, and wineboot maps a few dozen of
 * them before it starts anything. At 48MB the heap ran out part way
 * through, and Wine reported it as "Loading library gdi32.dll failed" -
 * which is what a failed mmap looks like from inside a PE loader.
 *
 * The ceiling is now the address space rather than a guess: the heap
 * starts at 0xD0000000 and the paging scratch page is at 0xE0000000, so
 * 192MB leaves a clear margin. It costs 48 pre-created page tables at
 * boot (see paging_reserve_kernel_tables) and no physical memory at all
 * until something allocates. */
#define KHEAP_MAX_SIZE     (192u * 1024u * 1024u)

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
