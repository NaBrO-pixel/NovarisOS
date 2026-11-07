#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include "harness.h"
#include "../../src/internal/heap.h"

#define LIVE_PTRS 64

void test_alloc(void)
{
    void *p[LIVE_PTRS];
    unsigned char *c, *d;
    size_t i, j;
    size_t sizes[] = { 1, 7, 15, 16, 17, 31, 100, 1000, 4096, 5000 };

    /* basic + alignment across many sizes */
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        void *q = malloc(sizes[i]);
        CHECK(q != NULL);
        CHECK_EQ((uintptr_t)q % 16, 0);
        memset(q, 0x5A, sizes[i]);            /* whole range must be writable */
        CHECK_EQ(novac__heap_check(), 0);
        free(q);
        CHECK_EQ(novac__heap_check(), 0);
    }
    free(NULL);                               /* no-op */

    /* malloc(0): valid, freeable pointer */
    c = malloc(0);
    CHECK(c != NULL);
    free(c);

    /* distinct, non-overlapping allocations keep their contents */
    for (i = 0; i < LIVE_PTRS; i++) {
        p[i] = malloc(24 + i * 8);
        CHECK(p[i] != NULL);
        memset(p[i], (int)i + 1, 24 + i * 8);
    }
    for (i = 0; i < LIVE_PTRS; i++) {
        c = p[i];
        for (j = 0; j < 24 + i * 8; j++)
            if (c[j] != (unsigned char)(i + 1)) { CHECK(0 && "allocation corrupted"); break; }
    }
    CHECK_EQ(novac__heap_check(), 0);
    for (i = 0; i < LIVE_PTRS; i += 2) free(p[i]);        /* free alternate: fragment */
    CHECK_EQ(novac__heap_check(), 0);
    for (i = 1; i < LIVE_PTRS; i += 2) free(p[i]);        /* free rest: must coalesce */
    CHECK_EQ(novac__heap_check(), 0);

    /* reuse: freeing then allocating the same size returns the same block */
    c = malloc(200);
    free(c);
    d = malloc(200);
    CHECK(c == d);
    free(d);

    /* coalescing: after everything is freed one big block can satisfy a large request */
    {
        void *a = malloc(20000), *b = malloc(20000), *e = malloc(20000);
        int before;
        free(b); free(a); free(e);
        before = th_sys_map_calls();
        a = malloc(55000);                    /* fits only if the three merged */
        CHECK(a != NULL);
        CHECK_EQ(th_sys_map_calls(), before); /* served from the merged block */
        free(a);
        CHECK_EQ(novac__heap_check(), 0);
    }

    /* calloc: zeroed (the fake kernel fills fresh pages with 0xA5) + overflow */
    c = malloc(512); memset(c, 0xCC, 512); free(c);   /* dirty a block, then recycle */
    c = calloc(64, 8);
    CHECK(c != NULL);
    for (i = 0; i < 512; i++) if (c[i]) { CHECK(0 && "calloc not zeroed"); break; }
    free(c);
    errno = 0;
    CHECK(calloc((size_t)-1, 2) == NULL);  CHECK_EQ(errno, ENOMEM);
    errno = 0;
    CHECK(calloc((size_t)1 << (sizeof(size_t) * 4), (size_t)1 << (sizeof(size_t) * 4)) == NULL);
    CHECK_EQ(errno, ENOMEM);
    c = calloc(0, 10);
    free(c);

    /* absurd sizes fail cleanly with ENOMEM */
    errno = 0; CHECK(malloc((size_t)-1) == NULL);      CHECK_EQ(errno, ENOMEM);
    errno = 0; CHECK(malloc((size_t)-16) == NULL);     CHECK_EQ(errno, ENOMEM);
    errno = 0; CHECK(malloc((size_t)-1 / 2 + 1) == NULL); CHECK_EQ(errno, ENOMEM);

    /* realloc: NULL acts as malloc, 0 frees, grow preserves, shrink preserves */
    c = realloc(NULL, 10);
    CHECK(c != NULL);
    memcpy(c, "0123456789", 10);
    c = realloc(c, 5000);                     /* grow (move or in place) */
    CHECK(c != NULL);
    CHECK(memcmp(c, "0123456789", 10) == 0);
    CHECK_EQ((uintptr_t)c % 16, 0);
    c = realloc(c, 20);                       /* shrink in place */
    CHECK(memcmp(c, "0123456789", 10) == 0);
    CHECK_EQ(novac__heap_check(), 0);
    CHECK(realloc(c, 0) == NULL);             /* frees */
    CHECK_EQ(novac__heap_check(), 0);

    /* grow in place into a free neighbour keeps the same address */
    c = malloc(100);
    d = malloc(100);
    {
        void *tail = malloc(100);             /* separates d from the arena's free tail */
        free(d);
        d = realloc(c, 180);                  /* absorbs freed neighbour */
        CHECK(d == c);
        CHECK_EQ(novac__heap_check(), 0);
        free(d); free(tail);
        CHECK_EQ(novac__heap_check(), 0);
    }

    /* realloc preserves data across a forced move */
    c = malloc(64);
    d = malloc(64);                           /* blocks in-place growth of c */
    for (i = 0; i < 64; i++) c[i] = (unsigned char)i;
    c = realloc(c, 4000);
    CHECK(c != NULL);
    for (i = 0; i < 64; i++) if (c[i] != (unsigned char)i) { CHECK(0 && "realloc lost data"); break; }
    free(c); free(d);

    /* failure handling: kernel refuses memory */
    {
        void *keep = malloc(32);
        size_t mapped = novac__heap_mapped_bytes();
        void *big;
        memset(keep, 0x77, 32);
        th_sys_fail_maps(-1);
        errno = 0;
        big = malloc(10 * 1024 * 1024);
        CHECK(big == NULL); CHECK_EQ(errno, ENOMEM);
        errno = 0;
        CHECK(realloc(keep, 10 * 1024 * 1024) == NULL);   /* original must survive */
        CHECK_EQ(errno, ENOMEM);
        CHECK(((unsigned char *)keep)[0] == 0x77 && ((unsigned char *)keep)[31] == 0x77);
        th_sys_fail_maps(0);
        CHECK_EQ(novac__heap_mapped_bytes(), mapped);
        CHECK_EQ(novac__heap_check(), 0);
        free(keep);
    }

    /* large allocation gets its own arena and is returned on free */
    {
        size_t before = th_sys_live_bytes();
        int un = th_sys_unmap_calls();
        void *big = malloc(1024 * 1024);
        CHECK(big != NULL);
        CHECK(th_sys_live_bytes() > before);
        memset(big, 1, 1024 * 1024);
        free(big);
        CHECK_EQ(th_sys_live_bytes(), before);
        CHECK_EQ(th_sys_unmap_calls(), un + 1);
        CHECK_EQ(novac__heap_check(), 0);
    }

    /* randomised stress with data verification */
    {
        unsigned state = 12345;
        void *slot[48] = {0};
        size_t len[48] = {0};
        unsigned char tag[48] = {0};
        int step;
        for (step = 0; step < 20000; step++) {
            unsigned k;
            state = state * 1103515245u + 12345u;
            k = (state >> 16) % 48;
            if (slot[k]) {
                unsigned char *q = slot[k];
                for (j = 0; j < len[k]; j++)
                    if (q[j] != tag[k]) { CHECK(0 && "stress: corruption"); return; }
                if (step & 1) { free(slot[k]); slot[k] = NULL; }
                else {
                    size_t nl = (state >> 8) % 3000 + 1;
                    void *np = realloc(slot[k], nl);
                    CHECK(np != NULL);
                    q = np;
                    for (j = 0; j < (nl < len[k] ? nl : len[k]); j++)
                        if (q[j] != tag[k]) { CHECK(0 && "stress: realloc corruption"); return; }
                    memset(q, tag[k], nl);
                    slot[k] = np; len[k] = nl;
                }
            } else {
                len[k] = (state >> 4) % 2000 + 1;
                tag[k] = (unsigned char)(state >> 12);
                slot[k] = malloc(len[k]);
                CHECK(slot[k] != NULL);
                CHECK_EQ((uintptr_t)slot[k] % 16, 0);
                memset(slot[k], tag[k], len[k]);
            }
            if (step % 500 == 0) CHECK_EQ(novac__heap_check(), 0);
        }
        for (i = 0; i < 48; i++) free(slot[i]);
        CHECK_EQ(novac__heap_check(), 0);
    }
}
