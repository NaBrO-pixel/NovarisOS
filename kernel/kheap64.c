/* kheap64.c - a kernel heap that does not get slower the longer it runs.
 *
 * See kheap64.h for why this is not a port of kheap.c. The short version:
 * one list of every block means every allocation walks every allocation
 * that came before it. Two lists - address order for coalescing, a free
 * list for searching - cost one extra pointer per block and remove the
 * quadratic term. */

#include "kheap64.h"
#include "pmm64.h"
#include "paging64.h"

/* 32 bytes, and a multiple of KHEAP64_ALIGN, which is what keeps every
 * payload 16-byte aligned given a 16-byte-aligned base: each block starts
 * at the previous payload's end, and payload sizes are rounded to 16. */
typedef struct block {
    uint64_t      size;   /* usable bytes following this header */
    struct block* prev;   /* address order */
    struct block* next;   /* address order */
    uint64_t      used;   /* a full word, to keep the header 16-aligned */
} block_t;

/* Overlaid on the payload of a *free* block only. This is why the minimum
 * allocation is 16 bytes: a block has to be able to hold these once freed. */
typedef struct free_link {
    struct block* fnext;
    struct block* fprev;
} free_link_t;

#define MIN_PAYLOAD (sizeof(free_link_t))

static block_t* heap_head;      /* lowest block, address order */
static block_t* heap_tail;      /* highest block, address order */
static block_t* free_head;      /* free list, unordered */
static uint64_t heap_end;       /* first virtual byte not yet mapped */
static uint64_t stat_steps, stat_calls, double_frees;

static inline free_link_t* flink(block_t* b) { return (free_link_t*)(b + 1); }

static inline uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

/* Blocks are contiguous by construction, but the test is kept rather than
 * assumed: if growth ever leaves a gap, merging across it would hand out
 * unmapped address space. */
static inline int adjacent(block_t* a, block_t* b) {
    return (uint8_t*)(a + 1) + a->size == (uint8_t*)b;
}

static void freelist_insert(block_t* b) {
    flink(b)->fprev = 0;
    flink(b)->fnext = free_head;
    if (free_head) flink(free_head)->fprev = b;
    free_head = b;
}

static void freelist_remove(block_t* b) {
    block_t* p = flink(b)->fprev;
    block_t* n = flink(b)->fnext;
    if (p) flink(p)->fnext = n; else free_head = n;
    if (n) flink(n)->fprev = p;
}

/* Absorb b->next into b. Both must be free and adjacent; b stays on the
 * free list, the block it swallows comes off it. */
static void merge_next(block_t* b) {
    block_t* n = b->next;
    freelist_remove(n);
    b->size += sizeof(block_t) + n->size;
    b->next = n->next;
    if (n->next) n->next->prev = b;
    if (n == heap_tail) heap_tail = b;
}

/* Maps pages up to new_end. Returns 0 if physical memory ran out partway;
 * whatever did get mapped stays mapped and heap_end reflects it, so the
 * caller can still use it. */
static int heap_expand(uint64_t new_end) {
    while (heap_end < new_end) {
        uint64_t phys = pmm64_alloc_frame();
        if (!phys) return 0;
        if (paging64_map(heap_end, phys, PAGE64_PRESENT | PAGE64_WRITE)
                != PAGING64_OK) {
            pmm64_free_frame(phys);
            return 0;
        }
        heap_end += PAGE64_SIZE;
    }
    return 1;
}

void kheap64_init(void) {
    heap_head = heap_tail = free_head = 0;
    heap_end = KHEAP64_BASE;
    stat_steps = stat_calls = double_frees = 0;

    if (!heap_expand(KHEAP64_BASE + KHEAP64_INITIAL)) return;
    if (heap_end == KHEAP64_BASE) return;

    heap_head = (block_t*)KHEAP64_BASE;
    heap_head->size = (heap_end - KHEAP64_BASE) - sizeof(block_t);
    heap_head->used = 0;
    heap_head->prev = 0;
    heap_head->next = 0;
    heap_tail = heap_head;
    freelist_insert(heap_head);
}

/* Split b so it holds exactly `size` bytes, if the remainder is big enough
 * to be a block in its own right. The remainder joins both lists. */
static void split(block_t* b, uint64_t size) {
    block_t* rest;

    if (b->size < size + sizeof(block_t) + MIN_PAYLOAD) return;

    rest = (block_t*)((uint8_t*)(b + 1) + size);
    rest->size = b->size - size - sizeof(block_t);
    rest->used = 0;
    rest->prev = b;
    rest->next = b->next;
    if (b->next) b->next->prev = rest;
    b->next = rest;
    if (b == heap_tail) heap_tail = rest;

    b->size = size;
    freelist_insert(rest);
}

/* Add newly mapped space at the top of the heap as one free block, and
 * coalesce it with the previous block if that is free - which is what
 * stops a run of grow-a-little allocations leaving a trail of stubs. */
static int grow(uint64_t payload_needed) {
    uint64_t want = align_up(sizeof(block_t) + payload_needed, PAGE64_SIZE);
    uint64_t old_end = heap_end;
    block_t* b;

    if (heap_end + want > KHEAP64_BASE + KHEAP64_MAX_SIZE) return 0;
    if (!heap_expand(heap_end + want)) {
        /* Partial growth is still usable, as long as it is enough for a
         * header and the smallest possible payload. */
        if (heap_end - old_end < sizeof(block_t) + MIN_PAYLOAD) return 0;
    }

    b = (block_t*)old_end;
    b->size = (heap_end - old_end) - sizeof(block_t);
    b->used = 0;
    b->prev = heap_tail;
    b->next = 0;
    if (heap_tail) heap_tail->next = b; else heap_head = b;
    heap_tail = b;
    freelist_insert(b);

    if (b->prev && !b->prev->used && adjacent(b->prev, b)) merge_next(b->prev);
    return 1;
}

void* kmalloc64(size_t size) {
    uint64_t want;
    block_t* b;
    int grown = 0;

    if (size == 0) return 0;
    want = align_up(size < MIN_PAYLOAD ? MIN_PAYLOAD : size, KHEAP64_ALIGN);

    stat_calls++;
    for (;;) {
        /* The free list, and only the free list. This is the whole
         * difference from kheap.c: blocks that are in use are not on it,
         * so a heap full of long-lived allocations is not something every
         * subsequent kmalloc has to walk past. */
        for (b = free_head; b; b = flink(b)->fnext) {
            stat_steps++;
            if (b->size >= want) {
                freelist_remove(b);
                split(b, want);
                b->used = 1;
                return (void*)(b + 1);
            }
        }
        if (grown) return 0;
        if (!grow(want)) return 0;
        grown = 1;
    }
}

void kfree64(void* ptr) {
    block_t* b;

    if (!ptr) return;
    b = (block_t*)ptr - 1;

    /* Freeing twice would put the block on the free list twice, and the
     * second allocation to find it would hand the same memory to two
     * owners. Counted rather than obeyed, same as the frame allocator. */
    if (!b->used) { double_frees++; return; }

    b->used = 0;
    freelist_insert(b);

    /* O(1), because the address list is doubly linked. kheap.c rescans
     * the whole heap on every free to do this. */
    if (b->next && !b->next->used && adjacent(b, b->next)) merge_next(b);
    if (b->prev && !b->prev->used && adjacent(b->prev, b)) merge_next(b->prev);
}

void kheap64_stats(uint64_t* used, uint64_t* total) {
    uint64_t u = 0, t = 0;
    for (block_t* b = heap_head; b; b = b->next) {
        t += b->size + sizeof(block_t);
        if (b->used) u += b->size + sizeof(block_t);
    }
    if (used) *used = u;
    if (total) *total = t;
}

void kheap64_walk_stats(uint64_t* steps, uint64_t* calls) {
    if (steps) *steps = stat_steps;
    if (calls) *calls = stat_calls;
}

uint64_t kheap64_double_frees(void) { return double_frees; }

uint64_t kheap64_free_blocks(void) {
    uint64_t n = 0;
    for (block_t* b = free_head; b; b = flink(b)->fnext) n++;
    return n;
}
