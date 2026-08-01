/* glibc_hello.c - an ordinary C program, built against real glibc.
 *
 * Every other test in this directory is freestanding: raw `int $0x80`,
 * no library, nothing between the program and the kernel. This one is
 * the opposite, and that is the point. It is compiled with plain
 * `gcc -m32 -static` against the host's real glibc, so what runs on
 * Novaris is hundreds of kilobytes of a production C library - its
 * startup code, its allocator, its SSE2 string functions, its printf and
 * its floating-point formatting - none of which has ever heard of
 * Novaris.
 *
 * Getting this far took the whole of Milestone 21, and each obstacle was
 * found by running it rather than predicted:
 *
 *   1. It faulted on the second instruction of `_start` - `pop %esi`,
 *      reading argc. Novaris had never built the System V initial process
 *      stack, because no freestanding test had ever looked at it.
 *   2. It then hit an invalid opcode in `__strrchr_sse2_bsf`: SSE was
 *      never enabled (CR4.OSFXSR), so every SSE2 string function in glibc
 *      was illegal.
 *   3. It then asked for eight syscalls that did not exist.
 *
 * See ROADMAP.md Milestone 21.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    printf("hello from a real glibc-linked program\n");

    /* glibc's allocator, on Novaris's brk/mmap. */
    char* p = malloc(64);
    strcpy(p, "malloc and strcpy work");
    printf("%s\n", p);
    free(p);

    /* glibc's printf, including its floating-point formatting - which
     * needs the x87 unit to be in a sane state and stay that way. */
    printf("%d %s %.2f\n", 42, "str", 3.14159);

    /* Its SSE2 string functions, which is what made an invalid opcode
     * fault the second obstacle above. */
    const char* haystack = "the quick brown fox jumps over the lazy dog";
    printf("strlen=%d strchr=%s strrchr=%s\n",
           (int)strlen(haystack), strchr(haystack, 'q'), strrchr(haystack, 'o'));

    /* qsort, so a libc callback runs too. */
    int v[] = { 42, -7, 0, 1000, 3 };
    int cmp(const void* a, const void* b) {
        int x = *(const int*)a, y = *(const int*)b;
        return (x > y) - (x < y);
    }
    qsort(v, 5, sizeof(int), cmp);
    printf("sorted:");
    for (int i = 0; i < 5; i++) printf(" %d", v[i]);
    printf("\n");

    return 0;
}
