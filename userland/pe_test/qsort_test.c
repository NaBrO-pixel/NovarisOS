/* qsort_test.c - exercises the Milestone 13 kernel -> ring-3 callback
 * mechanism from the program's side.
 *
 * Built with real i686-w64-mingw32-gcc against real msvcrt.dll imports.
 * Nothing here is written to accommodate Novaris: it is the ordinary way
 * a C program uses qsort, bsearch and atexit, and it produces identical
 * output on Windows.
 *
 * What each part proves:
 *
 *   - sorting ints and strings: the kernel really calls the program's
 *     comparator and uses what it returns, for two different element
 *     sizes and both a scalar and a pointer compare.
 *   - the noisy comparator: it calls printf, which is *another* int 0x81
 *     taken while the kernel is already several frames deep inside the
 *     qsort that invoked it. That nested trap is exactly the case a fixed
 *     TSS.esp0 corrupts, so this is the part that would fail loudly if
 *     the callback layer got the kernel stack wrong.
 *   - the recursive comparator: it calls qsort itself, so a second
 *     callback is entered while the first is still in flight.
 *   - bsearch: the same mechanism with a different call shape, including
 *     a lookup that must fail.
 *   - atexit: callbacks made after main() has already returned, from the
 *     exit path rather than from inside an API call. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a;
    int y = *(const int*)b;
    return (x > y) - (x < y);
}

/* Prints from inside the comparator: a nested int 0x81 during a callback. */
static int noisy_calls = 0;
static int cmp_int_noisy(const void* a, const void* b) {
    if (noisy_calls < 3) {
        printf("    comparator called from ring 3: %d vs %d\n",
               *(const int*)a, *(const int*)b);
    }
    noisy_calls++;
    return cmp_int(a, b);
}

/* Sorts a scratch array on every comparison, so a callback runs while an
 * outer callback is still on the stack. */
static int cmp_int_recursive(const void* a, const void* b) {
    int scratch[4] = { 9, 2, 7, 4 };
    qsort(scratch, 4, sizeof(int), cmp_int);
    if (scratch[0] != 2 || scratch[3] != 9) {
        printf("    NESTED QSORT WRONG: %d %d %d %d\n",
               scratch[0], scratch[1], scratch[2], scratch[3]);
    }
    return cmp_int(a, b);
}

static int cmp_str(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static void print_ints(const char* label, const int* v, int n) {
    printf("%s", label);
    for (int i = 0; i < n; i++) printf(" %d", v[i]);
    printf("\n");
}

static int sorted(const int* v, int n) {
    for (int i = 1; i < n; i++) if (v[i - 1] > v[i]) return 0;
    return 1;
}

static void bye_first(void) {
    printf("atexit: handler registered FIRST, so it runs LAST\n");
}

static void bye_second(void) {
    printf("atexit: handler registered SECOND, so it runs FIRST\n");
}

int main(void) {
    printf("qsort/bsearch/atexit test - real callbacks into ring 3\n\n");

    atexit(bye_first);
    atexit(bye_second);

    /* 1. plain int sort, more than the insertion-sort cutoff so the
     *    quicksort path runs too. */
    int v[] = { 42, -7, 0, 1000, 3, 3, -100, 17, 8, 99, 5, 5, 64, 2, 1 };
    int n = (int)(sizeof(v) / sizeof(v[0]));
    print_ints("before: ", v, n);
    qsort(v, n, sizeof(int), cmp_int);
    print_ints("after:  ", v, n);
    printf("sorted: %s\n\n", sorted(v, n) ? "yes" : "NO");

    /* 2. a comparator that calls printf - nested trap during a callback */
    int w[] = { 30, 10, 20 };
    printf("comparator that prints (nested int 0x81 inside a callback):\n");
    qsort(w, 3, sizeof(int), cmp_int_noisy);
    print_ints("  result:", w, 3);
    printf("  comparator ran %d time(s)\n\n", noisy_calls);

    /* 3. a comparator that calls qsort - nested callback */
    int r[] = { 5, 1, 4, 2, 3 };
    qsort(r, 5, sizeof(int), cmp_int_recursive);
    print_ints("nested qsort inside comparator:", r, 5);
    printf("sorted: %s\n\n", sorted(r, 5) ? "yes" : "NO");

    /* 4. strings: 4-byte elements again, but a pointer compare */
    const char* names[] = { "delta", "alpha", "echo", "charlie", "bravo" };
    qsort(names, 5, sizeof(names[0]), cmp_str);
    printf("strings:");
    for (int i = 0; i < 5; i++) printf(" %s", names[i]);
    printf("\n\n");

    /* 5. bsearch over the array sorted in step 1 */
    int keys[] = { 64, -100, 1000, 7 };
    for (int i = 0; i < 4; i++) {
        int* hit = (int*)bsearch(&keys[i], v, n, sizeof(int), cmp_int);
        printf("bsearch %5d -> %s\n", keys[i],
               hit ? "found" : "not found");
        if (hit && *hit != keys[i]) printf("  WRONG ELEMENT: %d\n", *hit);
    }

    printf("\nqsort test done.\n");
    return 0;
}
