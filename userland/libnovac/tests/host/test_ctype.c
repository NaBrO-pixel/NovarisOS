#include <ctype.h>
#include "harness.h"

void test_ctype(void)
{
    int c, alpha = 0, dig = 0, up = 0, lo = 0, sp = 0, xd = 0, pr = 0, pu = 0, cn = 0;
    for (c = 0; c < 128; c++) {
        alpha += !!isalpha(c); dig += !!isdigit(c); up += !!isupper(c);
        lo += !!islower(c); sp += !!isspace(c); xd += !!isxdigit(c);
        pr += !!isprint(c); pu += !!ispunct(c); cn += !!iscntrl(c);
        CHECK(!!isalnum(c) == (!!isalpha(c) || !!isdigit(c)));
        CHECK(!!isgraph(c) == (!!isprint(c) && c != ' '));
        CHECK(!!ispunct(c) == (!!isgraph(c) && !isalnum(c)));
    }
    CHECK_EQ(alpha, 52); CHECK_EQ(dig, 10); CHECK_EQ(up, 26); CHECK_EQ(lo, 26);
    CHECK_EQ(sp, 6);     CHECK_EQ(xd, 22); CHECK_EQ(pr, 95); CHECK_EQ(pu, 32);
    CHECK_EQ(cn, 33);
    CHECK(isblank(' ') && isblank('\t') && !isblank('\n'));
    CHECK(isspace('\v') && isspace('\f') && isspace('\r'));

    /* EOF and out-of-range values: classified as nothing, unchanged. */
    CHECK(!isalpha(-1) && !isspace(-1) && !isprint(-1) && !iscntrl(-1));
    CHECK(!isalpha(128) && !isalpha(255) && !isprint(200) && !iscntrl(200));
    CHECK_EQ(tolower(-1), -1);
    CHECK_EQ(toupper(200), 200);

    CHECK_EQ(tolower('A'), 'a'); CHECK_EQ(tolower('Z'), 'z'); CHECK_EQ(tolower('a'), 'a');
    CHECK_EQ(tolower('@'), '@'); CHECK_EQ(tolower('['), '[');
    CHECK_EQ(toupper('a'), 'A'); CHECK_EQ(toupper('z'), 'Z'); CHECK_EQ(toupper('0'), '0');
    CHECK_EQ(toupper('`'), '`'); CHECK_EQ(toupper('{'), '{');
}
