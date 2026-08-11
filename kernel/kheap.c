#include <stdint.h>
#include "kheap.h"
#include "paging.h"
#include "pmm.h"
#include "console.h"

/* The heap lives in its own dedicated virtual range, well away from the
 * kernel image and the 0xC0000000-0xC0800000 low-memory alias set up by
 * paging_init(), and grows on demand up to HEAP_MAX_SIZE. */
#define HEAP_START        KHEAP_VIRTUAL_BASE
#define HEAP_INITIAL_SIZE (16u * 4096u)        /* 64KB to start */
/* The compositor changed what "a big allocation" means here: the screen
 * back buffer and the pre-rendered wallpaper are ~4MB each at 1280x800,
 * and every open window owns a backing surface besides. 4MB was plenty
 * for a shell reading files; it isn't for a desktop. */
#define HEAP_MAX_SIZE      KHEAP_MAX_SIZE
#define PAGE_SIZE 4096u

typedef struct block_header {
    uint32_t size;   /* usable bytes following this header */
    uint8_t used;
    struct block_header* next;
} block_header_t;

static uint32_t heap_end = HEAP_START;  /* first not-yet-mapped virtual address */
static block_header_t* heap_head = 0;

/* Returns 0 if physical memory ran out partway - the pages it did map are
 * still valid, so the caller decides whether what it got is enough. */
static int heap_expand(uint32_t new_end) {
    while (heap_end < new_end) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return 0;
        paging_map_page(heap_end, phys, PAGE_PRESENT | PAGE_RW);
        heap_end += PAGE_SIZE;
    }
    return 1;
}

void kheap_init(void) {
    heap_expand(HEAP_START + HEAP_INITIAL_SIZE);
    heap_head = (block_header_t*)HEAP_START;
    heap_head->size = HEAP_INITIAL_SIZE - sizeof(block_header_t);
    heap_head->used = 0;
    heap_head->next = 0;
}

/* How far kmalloc walks. The list is every block, used and free, from
 * the start of the heap - so the cost of an allocation is the number of
 * allocations that came before it, and the cost of N allocations is N
 * squared. Wine's startup makes about eleven million syscalls and its
 * syscall rate collapses by twenty-one times partway through, exactly
 * where wineboot starts filling the prefix; this counts the steps to
 * confirm that is what the heap is doing rather than assume it. */
static uint32_t stat_steps, stat_calls, stat_report;

void kheap_walk_stats(uint32_t* steps, uint32_t* calls) {
    if (steps) *steps = stat_steps;
    if (calls) *calls = stat_calls;
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;
    size = (size + 3u) & ~(size_t)3u; /* 4-byte align */

    stat_calls++;
    if (stat_calls - stat_report >= 20000u) {
        stat_report = stat_calls;
        char num[12];
        terminal_writestring_color("[heap] ", VGA_COLOR_LIGHT_CYAN);
        ku32_to_dec(stat_calls / 1000u, num);
        terminal_writestring(num);
        terminal_writestring("k allocs, avg walk ");
        ku32_to_dec(stat_calls ? stat_steps / stat_calls : 0, num);
        terminal_writestring(num);
        terminal_writestring(" blocks\n");
    }

    block_header_t* block = heap_head;
    while (block) {
        stat_steps++;
        if (!block->used && block->size >= size) {
            /* Split off the remainder if it's big enough to be useful;
             * otherwise hand over the whole block to avoid slivers. */
            if (block->size >= size + sizeof(block_header_t) + 4) {
                block_header_t* rest =
                    (block_header_t*)((uint8_t*)(block + 1) + size);
                rest->size = block->size - (uint32_t)size - (uint32_t)sizeof(block_header_t);
                rest->used = 0;
                rest->next = block->next;

                block->size = (uint32_t)size;
                block->next = rest;
            }
            block->used = 1;
            return (void*)(block + 1);
        }
        block = block->next;
    }

    /* Nothing free was big enough - grow the heap and carve the new
     * allocation out of the freshly-mapped space. */
    uint32_t needed = (uint32_t)size + (uint32_t)sizeof(block_header_t);
    uint32_t grow_to = heap_end + needed;
    if (grow_to > HEAP_START + HEAP_MAX_SIZE) {
        /* Said out loud, once. The heap is where a mapped file's bytes
         * live - this kernel has no page cache, so mapping a nine-megabyte
         * DLL costs nine megabytes of heap and keeps costing it - and
         * running out of it arrives everywhere else as something else
         * entirely: a DLL that will not load, an image reported as an
         * invalid format, a program that stops with no message at all.
         * One line here is worth an afternoon there. */
        static int said = 0;
        if (!said) {
            said = 1;
            terminal_writestring_color("[kheap] ", VGA_COLOR_LIGHT_RED);
            terminal_writestring("kernel heap exhausted (max is KHEAP_MAX_SIZE) - "
                                 "allocations fail from here\n");
        }
        return 0;   /* heap exhausted */
    }

    uint32_t old_end = heap_end;
    uint32_t aligned_grow_to = (grow_to + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    heap_expand(aligned_grow_to);

    block_header_t* new_block = (block_header_t*)old_end;
    if (heap_end - old_end < needed) {
        /* The PMM ran dry mid-grow. Whatever pages did get mapped are
         * linked in as a free block so a later, smaller request can use
         * them, and this request fails cleanly. */
        if (heap_end > old_end + sizeof(block_header_t)) {
            new_block->size = (heap_end - old_end) - (uint32_t)sizeof(block_header_t);
            new_block->used = 0;
            new_block->next = 0;
            if (!heap_head) {
                heap_head = new_block;
            } else {
                block_header_t* tail = heap_head;
                while (tail->next) tail = tail->next;
                tail->next = new_block;
            }
        }
        return 0;
    }
    new_block->size = (heap_end - old_end) - (uint32_t)sizeof(block_header_t);
    new_block->used = 1;
    new_block->next = 0;

    if (!heap_head) {
        heap_head = new_block;
    } else {
        block_header_t* tail = heap_head;
        while (tail->next) tail = tail->next;
        tail->next = new_block;
    }

    return (void*)(new_block + 1);
}

/* Walks the block list for Task Manager. `used` and `total` come back
 * in bytes; `total` is what's currently mapped, not the cap. */
void kheap_stats(uint32_t* used, uint32_t* total) {
    uint32_t u = 0, t = 0;
    for (block_header_t* b = heap_head; b; b = b->next) {
        t += b->size + (uint32_t)sizeof(block_header_t);
        if (b->used) u += b->size + (uint32_t)sizeof(block_header_t);
    }
    if (used) *used = u;
    if (total) *total = t;
}

void kfree(void* ptr) {
    if (!ptr) return;
    block_header_t* block = (block_header_t*)ptr - 1;
    block->used = 0;

    /* Blocks are always appended in increasing-address order (splits and
     * heap growth both preserve this), so a single forward pass merging
     * address-adjacent free neighbors is enough to coalesce. */
    block_header_t* cur = heap_head;
    while (cur && cur->next) {
        uint8_t* cur_end = (uint8_t*)(cur + 1) + cur->size;
        if (!cur->used && !cur->next->used && (uint8_t*)cur->next == cur_end) {
            cur->size += (uint32_t)sizeof(block_header_t) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}
