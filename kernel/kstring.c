#include "kstring.h"

void kmemcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

void kmemmove(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return;
    if (d < s) {
        for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (uint32_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
}

void kmemset(void* dst, uint8_t value, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = value;
}

int kmemcmp(const void* a, const void* b, uint32_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (uint32_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

void* kmemchr(const void* s, uint8_t c, uint32_t n) {
    const uint8_t* p = (const uint8_t*)s;
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] == c) return (void*)(p + i);
    }
    return 0;
}

uint32_t kstrlen(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

uint32_t kstrnlen(const char* s, uint32_t max) {
    uint32_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int kstrcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int kstrncmp(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

int kstrncasecmp(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (!ca) return 0;
    }
    return 0;
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int kstricmp(const char* a, const char* b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return (int)(uint8_t)lower(*a) - (int)(uint8_t)lower(*b);
}

char* kstrcpy(char* dst, const char* src) {
    char* out = dst;
    while ((*out++ = *src++)) { }
    return dst;
}

char* kstrlcpy(char* dst, const char* src, uint32_t size) {
    if (size == 0) return dst;
    uint32_t i = 0;
    for (; i + 1 < size && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
    return dst;
}

char* kstrcat(char* dst, const char* src) {
    char* out = dst + kstrlen(dst);
    while ((*out++ = *src++)) { }
    return dst;
}

char* kstrchr(const char* s, char c) {
    for (;; s++) {
        if (*s == c) return (char*)s;
        if (*s == '\0') return 0;
    }
}

char* kstrrchr(const char* s, char c) {
    const char* found = 0;
    for (;; s++) {
        if (*s == c) found = s;
        if (*s == '\0') return (char*)found;
    }
}

char* kstrstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return 0;
}

int kstr_ends_with_ci(const char* s, const char* suffix) {
    uint32_t ls = kstrlen(s);
    uint32_t lx = kstrlen(suffix);
    if (lx > ls) return 0;
    return kstricmp(s + (ls - lx), suffix) == 0;
}

uint32_t ku32_to_dec(uint32_t value, char* out) {
    char tmp[12];
    int i = 0;
    if (value == 0) tmp[i++] = '0';
    while (value > 0) {
        tmp[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (int j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
    out[i] = '\0';
    return (uint32_t)i;
}

uint32_t ku32_to_hex(uint32_t value, char* out, int uppercase, int width) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[9];
    int i = 0;
    if (value == 0) tmp[i++] = '0';
    while (value > 0) {
        tmp[i++] = digits[value & 0xF];
        value >>= 4;
    }
    int pad = width > i ? width - i : 0;
    int n = 0;
    for (int j = 0; j < pad; j++) out[n++] = '0';
    for (int j = 0; j < i; j++) out[n++] = tmp[i - 1 - j];
    out[n] = '\0';
    return (uint32_t)n;
}

uint32_t kdiv_hi_lo(uint32_t hi, uint32_t lo, uint32_t divisor,
                    uint32_t* remainder) {
    uint32_t quotient, rem;
    /* DIV takes its dividend in EDX:EAX and leaves the quotient in EAX,
     * the remainder in EDX. With hi < divisor the quotient is guaranteed
     * to fit, so this can't raise #DE. */
    __asm__("divl %4"
            : "=a"(quotient), "=d"(rem)
            : "a"(lo), "d"(hi), "rm"(divisor));
    if (remainder) *remainder = rem;
    return quotient;
}

uint64_t kdiv64(uint64_t value, uint32_t divisor, uint32_t* remainder) {
    uint32_t hi = (uint32_t)(value >> 32);
    uint32_t lo = (uint32_t)value;
    uint32_t rem = 0;

    /* Two chained steps: the high half's remainder becomes the high word
     * of the low half's dividend. */
    uint32_t q_hi = kdiv_hi_lo(0, hi, divisor, &rem);
    uint32_t q_lo = kdiv_hi_lo(rem, lo, divisor, &rem);

    if (remainder) *remainder = rem;
    return ((uint64_t)q_hi << 32) | q_lo;
}

int32_t kstr_to_int(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    int32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}
