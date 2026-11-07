/* Minimal libNovaC program: formats, allocates and sorts without any host libc.
 * Returns 0 on success so a test runner can check the exit status; there is
 * no printf yet (see README). Entry-point/startup code is supplied by the
 * NovarisOS port, which is why `make example` only compile-checks this. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    int v[] = { 5, 3, 9, 1 };
    char line[32];
    char *copy;

    qsort(v, 4, sizeof v[0], cmp);
    if (snprintf(line, sizeof line, "min=%d max=%d", v[0], v[3]) < 0) return 1;

    copy = malloc(strlen(line) + 1);
    if (!copy) return 2;
    strcpy(copy, line);
    if (strcmp(copy, "min=1 max=9") != 0) return 3;
    free(copy);
    return 0;
}
