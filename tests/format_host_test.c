/* format_host_test.c - runs the kernel's printf engine on the build host.
 *
 * kernel/win32_msvcrt.c's w32_format() and kernel/win32_dtoa.c's
 * double-to-decimal conversion are pure functions over a buffer: no
 * paging, no interrupts, nothing kernel-specific. So they can be compiled
 * straight into a host binary and checked against the host's own printf,
 * which is a far faster and sharper loop than booting a kernel in QEMU to
 * look at what appeared on a framebuffer.
 *
 * Only the format engine is linked in, not the CRT wrappers around it -
 * hence the small amount of scaffolding below standing in for the pieces
 * of win32.c those wrappers would need.
 *
 * Built and run by `make test`.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "win32.h"
#include "win32_internal.h"

/* --- scaffolding -------------------------------------------------------- */

/* w32_format() reads varargs straight off a caller's stack. On the host we
 * build that argument block by hand, which is also a neat way to test
 * conversions with exactly the arguments we mean to pass. */
#define MAX_ARG_WORDS 32

typedef struct {
    uint32_t words[MAX_ARG_WORDS];
    int count;
} args_t;

static void arg_u32(args_t* a, uint32_t v) { a->words[a->count++] = v; }

static void arg_ptr(args_t* a, const void* p) {
    /* The kernel is 32-bit, so a pointer is one word. The host build is
     * -m32 for the same reason. */
    a->words[a->count++] = (uint32_t)(uintptr_t)p;
}

static void arg_double(args_t* a, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    a->words[a->count++] = (uint32_t)(bits & 0xFFFFFFFFu);
    a->words[a->count++] = (uint32_t)(bits >> 32);
}

static void arg_u64(args_t* a, uint64_t v) {
    a->words[a->count++] = (uint32_t)(v & 0xFFFFFFFFu);
    a->words[a->count++] = (uint32_t)(v >> 32);
}

/* Symbols win32_dtoa.c and the format engine reference but that live in
 * parts of the kernel this test doesn't link. */
uint32_t w32_wide_to_ascii(const uint16_t* src, char* dst, uint32_t dst_size) {
    uint32_t n = 0;
    if (!src || !dst || dst_size == 0) return 0;
    while (src[n] && n + 1 < dst_size) {
        dst[n] = src[n] < 128 ? (char)src[n] : '?';
        n++;
    }
    dst[n] = '\0';
    return n;
}

/* --- the tests ---------------------------------------------------------- */

static int failures = 0;
static int checks = 0;

static void check(const char* what, const char* got, const char* expected) {
    checks++;
    if (strcmp(got, expected) == 0) return;
    failures++;
    printf("  FAIL %s\n       got      \"%s\"\n       expected \"%s\"\n",
           what, got, expected);
}

/* Formats with the kernel engine and compares against the host's printf
 * for the same value - the strongest available oracle. */
static void check_double(const char* fmt, double value) {
    char expected[512];
    char got[512];
    args_t a = { {0}, 0 };

    snprintf(expected, sizeof(expected), fmt, value);
    arg_double(&a, value);
    w32_format(got, sizeof(got), fmt, a.words);
    check(fmt, got, expected);
}

int main(void) {
    char out[512];
    args_t a;

    printf("kernel printf engine, checked against the host C library\n\n");

    /* Integers. */
    a = (args_t){ {0}, 0 };
    arg_u32(&a, 42); arg_u32(&a, (uint32_t)-7); arg_u32(&a, 4000000000u);
    w32_format(out, sizeof(out), "%d %i %u", a.words);
    check("integers", out, "42 -7 4000000000");

    a = (args_t){ {0}, 0 };
    arg_u32(&a, 42); arg_u32(&a, 42); arg_u32(&a, 42); arg_u32(&a, 42);
    w32_format(out, sizeof(out), "[%5d][%-5d][%05d][%+d]", a.words);
    check("integer width/flags", out, "[   42][42   ][00042][+42]");

    a = (args_t){ {0}, 0 };
    arg_u32(&a, 48879); arg_u32(&a, 48879); arg_u32(&a, 48879); arg_u32(&a, 8);
    w32_format(out, sizeof(out), "%x %X %#x %o", a.words);
    check("bases", out, "beef BEEF 0xbeef 10");

    a = (args_t){ {0}, 0 };
    arg_u32(&a, 7);
    w32_format(out, sizeof(out), "%.5d", a.words);
    check("integer precision", out, "00007");

    a = (args_t){ {0}, 0 };
    arg_u64(&a, 1234567890123ULL);
    w32_format(out, sizeof(out), "%lld", a.words);
    check("64-bit", out, "1234567890123");

    a = (args_t){ {0}, 0 };
    arg_u64(&a, 0xFEDCBA9876543210ULL);
    w32_format(out, sizeof(out), "%llx", a.words);
    check("64-bit hex", out, "fedcba9876543210");

    /* Strings and characters. */
    a = (args_t){ {0}, 0 };
    arg_ptr(&a, "abc"); arg_ptr(&a, "abc"); arg_ptr(&a, "abc"); arg_ptr(&a, "abcdef");
    w32_format(out, sizeof(out), "[%s][%6s][%-6s][%.3s]", a.words);
    check("strings", out, "[abc][   abc][abc   ][abc]");

    a = (args_t){ {0}, 0 };
    arg_u32(&a, 'x');
    w32_format(out, sizeof(out), "%c%%", a.words);
    check("char and percent", out, "x%");

    a = (args_t){ {0}, 0 };
    arg_ptr(&a, NULL);
    w32_format(out, sizeof(out), "%s", a.words);
    check("null string", out, "(null)");

    /* Width and precision taken from the argument list. */
    a = (args_t){ {0}, 0 };
    arg_u32(&a, 8); arg_u32(&a, 3); arg_ptr(&a, "abcdef");
    w32_format(out, sizeof(out), "[%*.*s]", a.words);
    check("star width/precision", out, "[     abc]");

    /* Truncation must report the length that *would* have been written,
     * the way C99 snprintf does. */
    {
        char small[5];
        a = (args_t){ {0}, 0 };
        arg_ptr(&a, "abcdefgh");
        uint32_t n = w32_format(small, sizeof(small), "%s", a.words);
        checks++;
        if (n != 8 || strcmp(small, "abcd") != 0) {
            failures++;
            printf("  FAIL truncation\n       got \"%s\" (%u)\n"
                   "       expected \"abcd\" (8)\n", small, n);
        }
    }

    printf("\nfloating point (compared against the host printf):\n");

    check_double("%f", 0.0);
    check_double("%f", 1.0);
    check_double("%f", 3.14159265358979);
    check_double("%f", -2.5);
    check_double("%f", 100000.125);
    check_double("%f", 0.0001);
    check_double("%.0f", 2.5);
    check_double("%.2f", 2.5);
    check_double("%.2f", 1234.5678);
    check_double("%.10f", 1.0 / 3.0);
    check_double("%12.3f", 3.14159);
    check_double("%-12.3f", 3.14159);
    check_double("%f", 123456789.0);
    check_double("%f", 1e20);

    check_double("%e", 0.0);
    check_double("%e", 1.0);
    check_double("%e", 12345.6789);
    check_double("%e", 1e-10);
    check_double("%e", 1.5e300);
    check_double("%E", 6.02214076e23);
    check_double("%.3e", 12345.6789);

    check_double("%g", 0.0001);
    check_double("%g", 100000.0);
    check_double("%g", 1234567.0);
    check_double("%g", 0.5);
    check_double("%g", 1.0);
    check_double("%g", 3.14159265358979);

    printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
