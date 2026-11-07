#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include "harness.h"

static int vwrap(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

#define T(expect, ...) do { char b_[128]; int r_ = snprintf(b_, sizeof b_, __VA_ARGS__); \
    CHECK_STR(b_, expect); CHECK_EQ(r_, (int)strlen(expect)); } while (0)

void test_format(void)
{
    char b[64];
    int r;

    /* literals, %%, %c, %s */
    T("hello", "hello");
    T("100%", "100%%");
    T("x", "%c", 'x');
    T("  x", "%3c", 'x');
    T("x  |", "%-3c|", 'x');
    T("abc", "%s", "abc");
    T("(null)", "%s", (const char *)0);
    T("ab", "%.2s", "abcdef");
    T("   ab", "%5.2s", "abcdef");
    T("ab   |", "%-5s|", "ab");
    T("abc", "%.*s", 3, "abcdef");
    T("", "%.0s", "abc");

    /* signed */
    T("0", "%d", 0);
    T("-5", "%d", -5);
    T("2147483647", "%d", INT_MAX);
    T("-2147483648", "%d", INT_MIN);
    T("42", "%i", 42);
    T("+5", "%+d", 5);
    T(" 5", "% d", 5);
    T("-5", "% d", -5);
    T("   42", "%5d", 42);
    T("42   |", "%-5d|", 42);
    T("00042", "%05d", 42);
    T("-0042", "%05d", -42);
    T("  042", "%5.3d", 42);              /* precision disables the 0 flag */
    T("042", "%.3d", 42);
    T("", "%.0d", 0);
    T("-9223372036854775807", "%lld", -9223372036854775807LL);
    T("-9223372036854775808", "%lld", LLONG_MIN);
    T("-128", "%hhd", (signed char)-128);
    T("-1", "%hhd", 255);                 /* wraps to signed char */
    T("-1", "%hd", 65535);
    T("-3", "%zd", (ptrdiff_t)-3);
    T("7", "%td", (ptrdiff_t)7);
    T("-9", "%jd", (intmax_t)-9);
    T("    7|", "%*d|", 4 + 1, 7);
    T("7    |", "%*d|", -5, 7);           /* negative * width = left-justify */

    /* unsigned / hex / octal */
    T("4294967295", "%u", 4294967295u);
    T("18446744073709551615", "%llu", 18446744073709551615ULL);
    T("ff", "%x", 255);
    T("FF", "%X", 255);
    T("0xff", "%#x", 255);
    T("0XFF", "%#X", 255);
    T("0", "%#x", 0);                     /* no prefix for zero */
    T("0x00ff", "%#.4x", 255);
    T("  0xff", "%#6x", 255);
    T("0x00ff", "%#06x", 255);
    T("00ff", "%04x", 255);
    T("17", "%o", 15);
    T("017", "%#o", 15);
    T("0", "%#o", 0);
    T("0", "%#.0o", 0);
    T("ffffffff", "%x", 0xffffffffu);
    T("ffffffffffffffff", "%llx", 0xffffffffffffffffULL);
    T("1777777777777777777777", "%llo", 0xffffffffffffffffULL);
    T("255", "%hhu", 511);
    T("65535", "%hu", 131071);
    T("123", "%zu", (size_t)123);

    /* pointers */
    T("0x0", "%p", (void *)0);
    T("0x1234", "%p", (void *)(uintptr_t)0x1234);
    T("  0xab", "%6p", (void *)(uintptr_t)0xab);

    /* multiple conversions */
    T("a=1 b=-2 c=z s=hi", "a=%d b=%d c=%c s=%s", 1, -2, 'z', "hi");

    /* truncation and return value = would-be length */
    r = snprintf(b, 5, "%s", "abcdefgh");
    CHECK_EQ(r, 8); CHECK_STR(b, "abcd");
    r = snprintf(b, 1, "%s", "abc");
    CHECK_EQ(r, 3); CHECK_EQ(b[0], 0);
    r = snprintf(b, 4, "abc");              /* exactly fits */
    CHECK_EQ(r, 3); CHECK_STR(b, "abc");
    r = snprintf(b, 3, "abc");
    CHECK_EQ(r, 3); CHECK_STR(b, "ab");

    /* zero-length buffer: nothing written, NULL allowed, length returned */
    memset(b, 'X', sizeof b);
    r = snprintf(b, 0, "%d", 12345);
    CHECK_EQ(r, 5); CHECK_EQ(b[0], 'X');
    r = snprintf(NULL, 0, "%s-%d", "ab", 9);
    CHECK_EQ(r, 4);

    /* guard bytes: never write past the limit */
    memset(b, 'G', sizeof b);
    snprintf(b + 8, 6, "%s", "0123456789");
    CHECK(b[7] == 'G' && b[14] == 'G');
    CHECK_STR(b + 8, "01234");

    /* padding wider than the buffer still truncates safely */
    memset(b, 'G', sizeof b);
    r = snprintf(b, 6, "%10d", 1);
    CHECK_EQ(r, 10); CHECK_STR(b, "     "); CHECK_EQ(b[6], 'G');

    /* sprintf / vsnprintf / NULL buffer with nonzero size */
    CHECK_EQ(sprintf(b, "%d-%s", 5, "x"), 3); CHECK_STR(b, "5-x");
    CHECK_EQ(vwrap(b, sizeof b, "%03d", 7), 3); CHECK_STR(b, "007");
    errno = 0;
    CHECK_EQ(snprintf(NULL, 5, "x"), -1); CHECK_EQ(errno, EINVAL);

    /* errors: unsupported or malformed specifiers fail visibly */
    { const char *f = "%f"; errno = 0;
      CHECK_EQ(snprintf(b, sizeof b, f, 1.0), -1); CHECK_EQ(errno, EINVAL); }
    { const char *f = "abc%"; errno = 0;
      CHECK_EQ(snprintf(b, sizeof b, f), -1); CHECK_EQ(errno, EINVAL); }
    { const char *f = "%q"; errno = 0;
      CHECK_EQ(snprintf(b, sizeof b, f), -1); CHECK_EQ(errno, EINVAL); }
    { const char *f = "%n"; errno = 0;
      CHECK_EQ(snprintf(b, sizeof b, f, (int *)0), -1); CHECK_EQ(errno, EINVAL); }
    { const char *f = "%99999999999d"; errno = 0;
      CHECK_EQ(snprintf(b, sizeof b, f, 1), -1); CHECK_EQ(errno, EOVERFLOW); }
    /* buffer stays terminated after an error */
    { const char *f = "ok%f"; snprintf(b, sizeof b, f, 1.0); CHECK_STR(b, "ok"); }
}
