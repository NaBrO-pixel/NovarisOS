#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include "harness.h"

void test_convert(void)
{
    char *e;

    /* basic, whitespace, signs */
    CHECK_EQ(strtol("123", &e, 10), 123);          CHECK_EQ(*e, 0);
    CHECK_EQ(strtol("  \t\n-42xyz", &e, 10), -42); CHECK_STR(e, "xyz");
    CHECK_EQ(strtol("+7", &e, 10), 7);
    CHECK_EQ(strtol("0", &e, 10), 0);

    /* bases and prefixes */
    CHECK_EQ(strtol("0x1F", &e, 16), 31);
    CHECK_EQ(strtol("0x1F", &e, 0), 31);
    CHECK_EQ(strtol("1f", &e, 16), 31);
    CHECK_EQ(strtol("017", &e, 0), 15);
    CHECK_EQ(strtol("017", &e, 10), 17);
    CHECK_EQ(strtol("101", &e, 2), 5);
    CHECK_EQ(strtol("zz", &e, 36), 35 * 36 + 35);
    CHECK_EQ(strtol("-0x10", &e, 0), -16);

    /* "0x" with no hex digit parses just the "0" */
    CHECK_EQ(strtol("0xg", &e, 16), 0);            CHECK_STR(e, "xg");
    CHECK_EQ(strtol("0x", &e, 0), 0);              CHECK_STR(e, "x");

    /* no conversion: endptr == input, value 0, errno untouched */
    { const char *s = "  abc"; errno = 0;
      CHECK_EQ(strtol(s, &e, 10), 0);
      CHECK(e == s); CHECK_EQ(errno, 0); }
    { const char *s = "-"; CHECK_EQ(strtol(s, &e, 10), 0); CHECK(e == s); }
    { const char *s = ""; CHECK_EQ(strtol(s, &e, 10), 0); CHECK(e == s); }
    CHECK_EQ(strtol("12", NULL, 10), 12);          /* endptr may be NULL */

    /* invalid base */
    { const char *s = "10"; errno = 0;
      CHECK_EQ(strtol(s, &e, 1), 0);   CHECK_EQ(errno, EINVAL); CHECK(e == s);
      errno = 0;
      CHECK_EQ(strtol(s, &e, 37), 0);  CHECK_EQ(errno, EINVAL);
      errno = 0;
      CHECK_EQ(strtoul(s, &e, -5), 0); CHECK_EQ(errno, EINVAL); }

    /* long limits and overflow */
    errno = 0;
    CHECK_EQ(strtol("9999999999999999999999999", &e, 10), LONG_MAX);
    CHECK_EQ(errno, ERANGE); CHECK_EQ(*e, 0);
    errno = 0;
    CHECK_EQ(strtol("-9999999999999999999999999", &e, 10), LONG_MIN);
    CHECK_EQ(errno, ERANGE);
    errno = 0;
    CHECK(strtol("9223372036854775807", NULL, 10) == LONG_MAX || sizeof(long) == 4);
    errno = 0;
    CHECK_EQ(strtol("-9223372036854775808", NULL, 10) == LONG_MIN || sizeof(long) == 4, 1);
    if (sizeof(long) == 8) CHECK_EQ(errno, 0);     /* exact LONG_MIN is in range */
    errno = 0;
    if (sizeof(long) == 8) {
        strtol("9223372036854775808", NULL, 10);   /* LONG_MAX + 1 */
        CHECK_EQ(errno, ERANGE);
    }

    /* long long */
    errno = 0;
    CHECK(strtoll("-9223372036854775808", NULL, 10) == LLONG_MIN);
    CHECK_EQ(errno, 0);
    CHECK(strtoll("9223372036854775807", NULL, 10) == LLONG_MAX);
    CHECK_EQ(errno, 0);
    strtoll("9223372036854775808", NULL, 10);
    CHECK_EQ(errno, ERANGE);
    errno = 0;
    CHECK(strtoll("-9223372036854775809", NULL, 10) == LLONG_MIN);
    CHECK_EQ(errno, ERANGE);

    /* unsigned */
    errno = 0;
    CHECK(strtoull("18446744073709551615", NULL, 10) == ULLONG_MAX);
    CHECK_EQ(errno, 0);
    CHECK(strtoull("18446744073709551616", NULL, 10) == ULLONG_MAX);
    CHECK_EQ(errno, ERANGE);
    errno = 0;
    CHECK(strtoul("-1", NULL, 10) == ULONG_MAX);   /* negation wraps, no error */
    CHECK_EQ(errno, 0);
    CHECK_EQ(strtoul("ff", NULL, 16), 255);
    CHECK(strtoull("0xFFFFFFFFFFFFFFFF", NULL, 0) == ULLONG_MAX);
    errno = 0;
    strtoul("99999999999999999999999", NULL, 10);
    CHECK_EQ(errno, ERANGE);

    /* atoi family: clamped, errno preserved */
    errno = 0;
    CHECK_EQ(atoi("  -17abc"), -17);
    CHECK_EQ(atoi("junk"), 0);
    CHECK_EQ(atoi("99999999999999999999"), INT_MAX);
    CHECK_EQ(atoi("-99999999999999999999"), INT_MIN);
    CHECK_EQ(atol("123456"), 123456);
    CHECK(atoll("-5000000000") == -5000000000LL);
    CHECK_EQ(errno, 0);
}
