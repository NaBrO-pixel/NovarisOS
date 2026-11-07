/* Host-side runner. Compiled with the HOST's headers (not libNovaC's). */
#include <stdio.h>
#include <string.h>
#include "harness.h"

static int checks, failures;

void th_check(int cond, const char *expr, const char *file, int line)
{
    checks++;
    if (!cond) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr); }
}

void th_check_ll(long long got, long long want, const char *expr, const char *file, int line)
{
    checks++;
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL %s:%d: %s (got %lld, want %lld)\n", file, line, expr, got, want);
    }
}

void th_check_str(const char *got, const char *want, const char *expr, const char *file, int line)
{
    checks++;
    if (!got || !want || strcmp(got, want) != 0) {
        failures++;
        fprintf(stderr, "FAIL %s:%d: %s (got \"%s\", want \"%s\")\n", file, line, expr,
                got ? got : "(null)", want ? want : "(null)");
    }
}

#define RUN(t) do { int f0 = failures; t(); printf("%-14s %s\n", #t, failures == f0 ? "ok" : "FAILED"); } while (0)

int main(void)
{
    RUN(test_string);
    RUN(test_ctype);
    RUN(test_convert);
    RUN(test_format);
    RUN(test_alloc);
    RUN(test_misc);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
