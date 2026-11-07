/* String-to-integer conversions. One shared parser; the public functions only
 * differ in range limits. */
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

typedef struct {
    unsigned long long mag;   /* magnitude, valid if !ovf */
    int neg;
    int ovf;                  /* magnitude did not fit unsigned long long */
    int any;                  /* at least one digit consumed */
} parsed_t;

static int digit_value(int c)
{
    if (isdigit(c)) return c - '0';
    if (isalpha(c)) return tolower(c) - 'a' + 10;
    return 99;
}

static parsed_t parse(const char *s, char **end, int base)
{
    parsed_t r = {0, 0, 0, 0};
    const char *p = s;

    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') { r.neg = (*p == '-'); p++; }

    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2; base = 16;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }

    for (;; p++) {
        int d = digit_value((unsigned char)*p);
        if (d >= base) break;
        r.any = 1;
        if (!r.ovf) {
            if (r.mag > (ULLONG_MAX - (unsigned)d) / (unsigned)base) r.ovf = 1;
            else r.mag = r.mag * (unsigned)base + (unsigned)d;
        }
    }
    if (end) *end = (char *)(r.any ? p : s);
    return r;
}

static int bad_base(int base, char **end, const char *s)
{
    if (base == 0 || (base >= 2 && base <= 36)) return 0;
    errno = EINVAL;
    if (end) *end = (char *)s;
    return 1;
}

static unsigned long long conv_unsigned(const char *s, char **end, int base,
                                        unsigned long long max)
{
    parsed_t r;
    if (bad_base(base, end, s)) return 0;
    r = parse(s, end, base);
    if (!r.any) return 0;
    if (r.ovf || r.mag > max) { errno = ERANGE; return max; }
    return r.neg ? 0ull - r.mag : r.mag;     /* negation wraps, per the standard */
}

static long long conv_signed(const char *s, char **end, int base, long long max)
{
    parsed_t r;
    unsigned long long limit;
    if (bad_base(base, end, s)) return 0;
    r = parse(s, end, base);
    if (!r.any) return 0;
    limit = (unsigned long long)max + (r.neg ? 1u : 0u);
    if (r.ovf || r.mag > limit) { errno = ERANGE; return r.neg ? -max - 1 : max; }
    if (r.neg) return r.mag == limit ? -max - 1 : -(long long)r.mag;
    return (long long)r.mag;
}

long strtol(const char *restrict s, char **restrict e, int b)
{ return (long)conv_signed(s, e, b, LONG_MAX); }
long long strtoll(const char *restrict s, char **restrict e, int b)
{ return conv_signed(s, e, b, LLONG_MAX); }
unsigned long strtoul(const char *restrict s, char **restrict e, int b)
{ return (unsigned long)conv_unsigned(s, e, b, ULONG_MAX); }
unsigned long long strtoull(const char *restrict s, char **restrict e, int b)
{ return conv_unsigned(s, e, b, ULLONG_MAX); }

/* atoi family: no error reporting, errno left untouched; atoi clamps to int. */
long long atoll(const char *s)
{
    int saved = errno;
    long long v = strtoll(s, NULL, 10);
    errno = saved;
    return v;
}
long atol(const char *s) { return (long)atoll(s) ; }
int atoi(const char *s)
{
    long v = atol(s);
    return v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : (int)v;
}
