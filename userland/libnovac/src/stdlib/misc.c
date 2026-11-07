/* abs/div, qsort/bsearch, rand. */
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

/* abs(INT_MIN) is undefined in C; we return it unchanged (two's complement
 * wrap) instead of trapping. Documented in the README. */
int       abs(int x)         { return x < 0 ? (int)(0u - (unsigned)x) : x; }
long      labs(long x)       { return x < 0 ? (long)(0ul - (unsigned long)x) : x; }
long long llabs(long long x) { return x < 0 ? (long long)(0ull - (unsigned long long)x) : x; }

div_t   div(int n, int d)             { div_t r   = { n / d, n % d }; return r; }
ldiv_t  ldiv(long n, long d)          { ldiv_t r  = { n / d, n % d }; return r; }
lldiv_t lldiv(long long n, long long d){ lldiv_t r = { n / d, n % d }; return r; }

typedef int (*cmp_fn)(const void *, const void *);

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
    while (size--) { unsigned char t = *a; *a++ = *b; *b++ = t; }
}

static void sift_down(unsigned char *base, size_t root, size_t count,
                      size_t size, cmp_fn cmp)
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= count) return;
        if (child + 1 < count &&
            cmp(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (cmp(base + root * size, base + child * size) >= 0) return;
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

/* Heapsort: O(n log n) worst case, in place, no recursion, no allocation
 * (so it is usable before the heap exists). Not stable. */
void qsort(void *base, size_t nmemb, size_t size, cmp_fn cmp)
{
    unsigned char *b = base;
    size_t i;
    if (nmemb < 2 || size == 0) return;
    if (!base || !cmp || nmemb > SIZE_MAX / 2 || nmemb > SIZE_MAX / size) {
        errno = EINVAL;
        return;
    }
    for (i = nmemb / 2; i-- > 0;) sift_down(b, i, nmemb, size, cmp);
    for (i = nmemb; i-- > 1;) {
        swap_bytes(b, b + i * size, size);
        sift_down(b, 0, i, size, cmp);
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              cmp_fn cmp)
{
    const unsigned char *b = base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, b + mid * size);
        if (c == 0) return (void *)(b + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

static uint32_t rand_state = 1;
void srand(unsigned int seed) { rand_state = seed; }
int rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 16) & 0x7fff);
}
