/* Heap allocator.
 *
 * Design: memory arrives from the kernel in "arenas" via novac_sys_map().
 * Each arena is a run of boundary-tagged blocks ending in a zero-size "used"
 * sentinel. Free blocks sit on one doubly linked list (first fit). Adjacent
 * free blocks are always merged, so the invariant "no two physically adjacent
 * free blocks" holds between calls. Arenas larger than ARENA_MIN (created for
 * big requests) go back to the kernel when they become entirely free.
 *
 * Only novac_sys_map/unmap touch the kernel. Not thread-safe (no locking
 * primitive exists yet); callers must serialise.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include "../internal/sys.h"
#include "../internal/heap.h"

#define ALIGN      16u
#define HDR        16u
#define MIN_BLOCK  32u                 /* header + room for two free-list links */
#define ARENA_MIN  (64u * 1024u)
#define USED       1u

typedef union blk {
    struct { size_t prev_size; size_t size_flags; } h;
    unsigned char raw[HDR];
} blk_t;

typedef struct fnode { struct fnode *next, *prev; } fnode_t;

_Static_assert(sizeof(blk_t) == HDR, "block header must be 16 bytes");
_Static_assert(sizeof(fnode_t) <= MIN_BLOCK - HDR, "free links must fit in a minimal block");

static fnode_t *free_head;
static size_t   mapped_bytes;

static size_t bsize(const blk_t *b) { return b->h.size_flags & ~(size_t)(ALIGN - 1); }
static int    bused(const blk_t *b) { return (int)(b->h.size_flags & USED); }
static void   bset(blk_t *b, size_t size, int used) { b->h.size_flags = size | (used ? USED : 0u); }
static blk_t *bnext(blk_t *b) { return (blk_t *)((unsigned char *)b + bsize(b)); }
static blk_t *bprev(blk_t *b) { return (blk_t *)((unsigned char *)b - b->h.prev_size); }
static fnode_t *node_of(blk_t *b) { return (fnode_t *)((unsigned char *)b + HDR); }
static blk_t *blk_of_node(fnode_t *n) { return (blk_t *)((unsigned char *)n - HDR); }
static void *payload(blk_t *b) { return (unsigned char *)b + HDR; }

static void fl_insert(blk_t *b)
{
    fnode_t *n = node_of(b);
    n->prev = NULL;
    n->next = free_head;
    if (free_head) free_head->prev = n;
    free_head = n;
}

static void fl_remove(blk_t *b)
{
    fnode_t *n = node_of(b);
    if (n->prev) n->prev->next = n->next; else free_head = n->next;
    if (n->next) n->next->prev = n->prev;
}

/* Bytes of block needed for an n-byte payload; 0 if impossible. */
static size_t block_for(size_t n)
{
    size_t need;
    if (n > (size_t)PTRDIFF_MAX - 4 * HDR) return 0;
    need = ((n + ALIGN - 1) & ~(size_t)(ALIGN - 1)) + HDR;
    return need < MIN_BLOCK ? MIN_BLOCK : need;
}

static blk_t *arena_new(size_t need)
{
    size_t want = need + HDR;                      /* + end sentinel */
    size_t got;
    unsigned char *mem;
    blk_t *b, *sentinel;

    if (want < ARENA_MIN) want = ARENA_MIN;
    got = want;
    mem = novac_sys_map(&got);
    if (!mem) return NULL;
    /* Enforce the novac_sys_map contract rather than trusting it blindly. */
    if (got < want || (got & (ALIGN - 1)) || ((uintptr_t)mem & (ALIGN - 1))) {
        novac_sys_unmap(mem, got);
        return NULL;
    }
    mapped_bytes += got;
    b = (blk_t *)mem;
    bset(b, got - HDR, 0);
    b->h.prev_size = 0;
    sentinel = (blk_t *)(mem + got - HDR);
    bset(sentinel, 0, 1);
    sentinel->h.prev_size = got - HDR;
    return b;
}

/* b is a used block of at least `need` bytes: give the excess back. */
static void split_used(blk_t *b, size_t need)
{
    size_t size = bsize(b), rem = size - need;
    blk_t *r, *nxt;
    if (rem < MIN_BLOCK) return;
    bset(b, need, 1);
    r = (blk_t *)((unsigned char *)b + need);
    r->h.prev_size = need;
    nxt = (blk_t *)((unsigned char *)r + rem);
    if (!bused(nxt)) {                              /* keep free blocks merged */
        fl_remove(nxt);
        rem += bsize(nxt);
        nxt = (blk_t *)((unsigned char *)r + rem);
    }
    bset(r, rem, 0);
    nxt->h.prev_size = rem;
    fl_insert(r);
}

void *malloc(size_t n)
{
    size_t need = block_for(n);
    blk_t *b = NULL;
    fnode_t *f;

    if (!need) { errno = ENOMEM; return NULL; }
    for (f = free_head; f; f = f->next)
        if (bsize(blk_of_node(f)) >= need) { b = blk_of_node(f); break; }
    if (!b) {
        b = arena_new(need);
        if (!b) { errno = ENOMEM; return NULL; }
        fl_insert(b);
    }
    fl_remove(b);
    bset(b, bsize(b), 1);
    split_used(b, need);
    return payload(b);
}

void *calloc(size_t nmemb, size_t size)
{
    void *p;
    if (nmemb && size > SIZE_MAX / nmemb) { errno = ENOMEM; return NULL; }
    p = malloc(nmemb * size);
    if (p) memset(p, 0, nmemb * size);
    return p;
}

/* Heap corruption or invalid free: there is no safe way to continue. */
static blk_t *checked_block(void *p)
{
    blk_t *b;
    if ((uintptr_t)p & (ALIGN - 1)) abort();
    b = (blk_t *)((unsigned char *)p - HDR);
    if (!bused(b) || bsize(b) < MIN_BLOCK) abort();   /* double free / bad pointer */
    return b;
}

void free(void *p)
{
    blk_t *b, *nx;
    size_t size;
    if (!p) return;
    b = checked_block(p);
    size = bsize(b);
    bset(b, size, 0);

    nx = bnext(b);
    if (!bused(nx)) {
        fl_remove(nx);
        size += bsize(nx);
        bset(b, size, 0);
    }
    if (b->h.prev_size) {
        blk_t *pv = bprev(b);
        if (!bused(pv)) {
            fl_remove(pv);
            size += bsize(pv);
            bset(pv, size, 0);
            b = pv;
        }
    }
    nx = bnext(b);
    nx->h.prev_size = size;

    if (b->h.prev_size == 0 && bsize(nx) == 0 && size + HDR > ARENA_MIN) {
        mapped_bytes -= size + HDR;
        if (novac_sys_unmap(b, size + HDR) != 0) abort();   /* never ignore */
        return;
    }
    fl_insert(b);
}

void *realloc(void *p, size_t n)
{
    blk_t *b, *nx;
    size_t need, size;
    void *q;

    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    need = block_for(n);
    if (!need) { errno = ENOMEM; return NULL; }     /* original left intact */

    b = checked_block(p);
    size = bsize(b);

    if (need <= size) { split_used(b, need); return p; }

    nx = bnext(b);
    if (!bused(nx) && size + bsize(nx) >= need) {   /* grow in place */
        size_t total = size + bsize(nx);
        blk_t *after;
        fl_remove(nx);
        bset(b, total, 1);
        after = bnext(b);
        after->h.prev_size = total;
        split_used(b, need);
        return p;
    }

    q = malloc(n);
    if (!q) return NULL;                            /* errno set; p still valid */
    memcpy(q, p, size - HDR);                       /* size-HDR < n here */
    free(p);
    return q;
}

int novac__heap_check(void)
{
    fnode_t *f;
    for (f = free_head; f; f = f->next) {
        blk_t *b = blk_of_node(f), *nx;
        if (bused(b) || bsize(b) < MIN_BLOCK) return 1;
        if (f->prev == NULL ? free_head != f : f->prev->next != f) return 2;
        if (f->next && f->next->prev != f) return 3;
        nx = bnext(b);
        if (nx->h.prev_size != bsize(b)) return 4;
        if (!bused(nx)) return 5;                   /* unmerged neighbours */
        if (b->h.prev_size) {
            blk_t *pv = bprev(b);
            if (bnext(pv) != b || !bused(pv)) return 6;
        }
    }
    return 0;
}

size_t novac__heap_mapped_bytes(void) { return mapped_bytes; }
