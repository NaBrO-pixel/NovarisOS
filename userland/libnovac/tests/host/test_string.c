#include <string.h>
#include <errno.h>
#include "harness.h"

static unsigned char fill[32];

void test_string(void)
{
    char a[16], b[16];
    unsigned char buf[16];
    int i;

    /* strlen / strnlen */
    CHECK_EQ(strlen(""), 0);
    CHECK_EQ(strlen("abc"), 3);
    CHECK_EQ(strnlen("abcdef", 3), 3);
    CHECK_EQ(strnlen("ab", 10), 2);
    CHECK_EQ(strnlen("abc", 0), 0);

    /* strcmp: sign is by unsigned char */
    CHECK_EQ(strcmp("a", "a"), 0);
    CHECK(strcmp("a", "b") < 0);
    CHECK(strcmp("b", "a") > 0);
    CHECK(strcmp("ab", "a") > 0);
    CHECK(strcmp("", "a") < 0);
    CHECK(strcmp("\xff", "a") > 0);

    /* strncmp */
    CHECK_EQ(strncmp("abcd", "abxx", 2), 0);
    CHECK(strncmp("abcd", "abxx", 3) < 0);
    CHECK_EQ(strncmp("abc", "abc", 100), 0);
    CHECK_EQ(strncmp("a", "b", 0), 0);
    CHECK(strncmp("a\0x", "a\0y", 3) == 0);   /* stops at terminator */

    /* strcpy / strncpy */
    CHECK(strcpy(a, "hello") == a);
    CHECK_STR(a, "hello");
    memset(a, 'x', sizeof a);
    strncpy(a, "hi", 6);                      /* must zero-pad */
    CHECK(a[2] == 0 && a[3] == 0 && a[5] == 0 && a[6] == 'x');
    memset(a, 'x', sizeof a);
    strncpy(a, "hello", 3);                   /* no terminator written */
    CHECK(a[2] == 'l' && a[3] == 'x');
    memset(a, 'x', sizeof a);
    strncpy(a, "abc", 0);
    CHECK(a[0] == 'x');

    /* strcat / strncat */
    strcpy(a, "ab"); strcat(a, "cd");
    CHECK_STR(a, "abcd");
    strcpy(a, "ab"); strncat(a, "cdef", 2);
    CHECK_STR(a, "abcd");
    strcpy(a, "ab"); strncat(a, "c", 10);
    CHECK_STR(a, "abc");

    /* strchr / strrchr / strstr / memchr */
    { const char *s = "hello";
      CHECK(strchr(s, 'l') == s + 2);
      CHECK(strrchr(s, 'l') == s + 3);
      CHECK(strchr(s, 'z') == NULL);
      CHECK(strchr(s, '\0') == s + 5);
      CHECK(strrchr(s, '\0') == s + 5);
      CHECK(strstr(s, "llo") == s + 2);
      CHECK(strstr(s, "") == s);
      CHECK(strstr(s, "hellos") == NULL);
      CHECK(strstr("", "a") == NULL);
      CHECK(memchr(s, 'o', 5) == s + 4);
      CHECK(memchr(s, 'o', 4) == NULL);
      CHECK(memchr(s, 0, 0) == NULL); }

    /* memset */
    memset(buf, 0xAB, sizeof buf);
    for (i = 0; i < 16; i++) CHECK(buf[i] == 0xAB);
    CHECK(memset(buf, 0x1FF, 4) == buf);      /* value converted to unsigned char */
    CHECK(buf[0] == 0xFF && buf[4] == 0xAB);
    memset(buf, 0, 0);

    /* memcpy */
    for (i = 0; i < 16; i++) b[i] = (char)i;
    memcpy(a, b, 16);
    CHECK(memcmp(a, b, 16) == 0);
    memcpy(a, "zz", 0);
    CHECK(a[0] == 0);

    /* memcmp */
    CHECK_EQ(memcmp("abc", "abd", 3), -1);
    CHECK_EQ(memcmp("abd", "abc", 3), 1);
    CHECK_EQ(memcmp("abc", "abd", 2), 0);
    CHECK_EQ(memcmp("a", "b", 0), 0);
    CHECK(memcmp("\xff", "\x01", 1) > 0);

    /* memmove: forward overlap, backward overlap, zero length, same pointer */
    for (i = 0; i < 16; i++) buf[i] = (unsigned char)i;
    memmove(buf + 2, buf, 8);                 /* dst > src, overlapping */
    for (i = 0; i < 8; i++) CHECK_EQ(buf[2 + i], i);
    CHECK(buf[0] == 0 && buf[1] == 1);
    for (i = 0; i < 16; i++) buf[i] = (unsigned char)i;
    memmove(buf, buf + 2, 8);                 /* dst < src, overlapping */
    for (i = 0; i < 8; i++) CHECK_EQ(buf[i], i + 2);
    for (i = 0; i < 16; i++) buf[i] = (unsigned char)i;
    memmove(buf + 4, buf, 0);
    memmove(buf, buf, 16);
    for (i = 0; i < 16; i++) CHECK_EQ(buf[i], i);
    memmove(buf + 1, buf, 15);                /* shift by one, maximum overlap */
    CHECK(buf[0] == 0 && buf[1] == 0 && buf[15] == 14);
    (void)fill;

    /* strerror */
    CHECK_STR(strerror(ENOMEM), "Out of memory");
    CHECK_STR(strerror(0), "Success");
    CHECK_STR(strerror(99999), "Unknown error");
}
