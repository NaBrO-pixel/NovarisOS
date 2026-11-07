/* Formatting engine behind snprintf and friends. Output-agnostic: everything
 * goes through a sink callback.
 *
 * Supported: flags "-+ #0"; width and precision (number or '*'); length
 * modifiers hh h l ll z t j; conversions d i u o x X c s p %.
 * Unsupported (returns -1/EINVAL): floating point, %n, wide chars. */
#include <novac/format.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <ctype.h>

typedef struct {
    novac_sink_fn sink;
    void *ctx;
    size_t total;       /* characters produced so far, always <= INT_MAX */
    int overflow;
} out_t;

enum { L_NONE, L_HH, L_H, L_L, L_LL, L_Z, L_T, L_J };

typedef struct {
    int minus, plus, space, alt, zero;
    int width;          /* 0 = none */
    int prec;           /* -1 = none */
    int len;
} spec_t;

static void emit(out_t *o, const char *s, size_t n)
{
    if (n == 0 || o->overflow) return;
    if (n > (size_t)INT_MAX - o->total) { o->overflow = 1; return; }
    o->sink(o->ctx, s, n);
    o->total += n;
}

static void emit_rep(out_t *o, char c, size_t n)
{
    char chunk[16];
    memset(chunk, c, sizeof chunk);
    while (n && !o->overflow) {
        size_t k = n < sizeof chunk ? n : sizeof chunk;
        emit(o, chunk, k);
        n -= k;
    }
}

static void emit_padded(out_t *o, const spec_t *s, const char *str, size_t n)
{
    size_t w = (size_t)s->width, pad = w > n ? w - n : 0;
    if (!s->minus) emit_rep(o, ' ', pad);
    emit(o, str, n);
    if (s->minus) emit_rep(o, ' ', pad);
}

static void emit_integer(out_t *o, const spec_t *s, unsigned long long v,
                         int neg, int is_signed, unsigned base, int upper,
                         int force_prefix)
{
    char buf[24];                                   /* >= 22 octal digits for 64 bits */
    char *end = buf + sizeof buf, *q = end;
    const char *alpha = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char pre[2];
    size_t npre = 0, nd, zeros = 0, len, w = (size_t)s->width, pad;
    int nonzero = (v != 0);

    if (!(v == 0 && s->prec == 0))
        do { *--q = alpha[v % base]; v /= base; } while (v);
    nd = (size_t)(end - q);

    if (is_signed) {
        if (neg) pre[npre++] = '-';
        else if (s->plus) pre[npre++] = '+';
        else if (s->space) pre[npre++] = ' ';
    }
    if (base == 16 && (force_prefix || (s->alt && nonzero))) {
        pre[0] = '0'; pre[1] = upper ? 'X' : 'x'; npre = 2;   /* never combined with a sign */
    }
    if (s->prec > 0 && (size_t)s->prec > nd) zeros = (size_t)s->prec - nd;
    if (base == 8 && s->alt && zeros == 0 && (nd == 0 || *q != '0')) zeros = 1;

    len = npre + zeros + nd;
    pad = w > len ? w - len : 0;

    if (s->minus) {
        emit(o, pre, npre); emit_rep(o, '0', zeros); emit(o, q, nd);
        emit_rep(o, ' ', pad);
    } else if (s->zero && s->prec < 0) {
        emit(o, pre, npre); emit_rep(o, '0', zeros + pad); emit(o, q, nd);
    } else {
        emit_rep(o, ' ', pad);
        emit(o, pre, npre); emit_rep(o, '0', zeros); emit(o, q, nd);
    }
}

/* Parses a decimal number; returns -1 on int overflow. */
static int parse_num(const char **pp)
{
    const char *p = *pp;
    int v = 0;
    while (isdigit((unsigned char)*p)) {
        int d = *p++ - '0';
        if (v > (INT_MAX - d) / 10) return -1;
        v = v * 10 + d;
    }
    *pp = p;
    return v;
}

static int fail(int err) { errno = err; return -1; }

int novac_format(novac_sink_fn sink, void *ctx, const char *fmt, va_list ap)
{
    out_t o = { sink, ctx, 0, 0 };
    const char *p = fmt;

    if (!sink || !fmt) return fail(EINVAL);

    while (*p) {
        spec_t s = { 0, 0, 0, 0, 0, 0, -1, L_NONE };
        const char *lit = p;
        char conv;

        while (*p && *p != '%') p++;
        emit(&o, lit, (size_t)(p - lit));
        if (!*p) break;
        p++;

        for (;; p++) {
            if      (*p == '-') s.minus = 1;
            else if (*p == '+') s.plus = 1;
            else if (*p == ' ') s.space = 1;
            else if (*p == '#') s.alt = 1;
            else if (*p == '0') s.zero = 1;
            else break;
        }
        if (*p == '*') {
            int w = va_arg(ap, int);
            p++;
            if (w < 0) {
                if (w == INT_MIN) return fail(EOVERFLOW);
                s.minus = 1; w = -w;
            }
            s.width = w;
        } else if (isdigit((unsigned char)*p)) {
            s.width = parse_num(&p);
            if (s.width < 0) return fail(EOVERFLOW);
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                int pr = va_arg(ap, int);
                p++;
                s.prec = pr < 0 ? -1 : pr;          /* negative precision = absent */
            } else {
                s.prec = parse_num(&p);
                if (s.prec < 0) return fail(EOVERFLOW);
            }
        }
        switch (*p) {
        case 'h': p++; if (*p == 'h') { s.len = L_HH; p++; } else s.len = L_H; break;
        case 'l': p++; if (*p == 'l') { s.len = L_LL; p++; } else s.len = L_L; break;
        case 'z': s.len = L_Z; p++; break;
        case 't': s.len = L_T; p++; break;
        case 'j': s.len = L_J; p++; break;
        default: break;
        }

        conv = *p;
        if (!conv) return fail(EINVAL);             /* format ends after '%' */
        p++;

        switch (conv) {
        case '%':
            emit(&o, "%", 1);
            break;
        case 'c': {
            char ch;
            if (s.len != L_NONE) return fail(EINVAL);
            ch = (char)va_arg(ap, int);
            emit_padded(&o, &s, &ch, 1);
            break;
        }
        case 's': {
            const char *str;
            size_t n;
            if (s.len != L_NONE) return fail(EINVAL);
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            n = s.prec < 0 ? strlen(str) : strnlen(str, (size_t)s.prec);
            emit_padded(&o, &s, str, n);
            break;
        }
        case 'd': case 'i': {
            long long sv;
            switch (s.len) {
            case L_HH: sv = (signed char)va_arg(ap, int); break;
            case L_H:  sv = (short)va_arg(ap, int); break;
            case L_L:  sv = va_arg(ap, long); break;
            case L_LL: sv = va_arg(ap, long long); break;
            case L_Z:
            case L_T:  sv = va_arg(ap, ptrdiff_t); break;
            case L_J:  sv = va_arg(ap, intmax_t); break;
            default:   sv = va_arg(ap, int); break;
            }
            emit_integer(&o, &s, sv < 0 ? 0ull - (unsigned long long)sv
                                        : (unsigned long long)sv,
                         sv < 0, 1, 10, 0, 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long uv;
            switch (s.len) {
            case L_HH: uv = (unsigned char)va_arg(ap, unsigned); break;
            case L_H:  uv = (unsigned short)va_arg(ap, unsigned); break;
            case L_L:  uv = va_arg(ap, unsigned long); break;
            case L_LL: uv = va_arg(ap, unsigned long long); break;
            case L_Z:
            case L_T:  uv = va_arg(ap, size_t); break;
            case L_J:  uv = va_arg(ap, uintmax_t); break;
            default:   uv = va_arg(ap, unsigned); break;
            }
            emit_integer(&o, &s, uv, 0, 0,
                         conv == 'o' ? 8u : (conv == 'u' ? 10u : 16u),
                         conv == 'X', 0);
            break;
        }
        case 'p': {
            spec_t ps = s;
            if (s.len != L_NONE) return fail(EINVAL);
            ps.zero = 0; ps.plus = 0; ps.space = 0;
            emit_integer(&o, &ps, (uintptr_t)va_arg(ap, void *), 0, 0, 16, 0, 1);
            break;
        }
        default:
            return fail(EINVAL);                    /* floats, %n, unknown */
        }
        if (o.overflow) return fail(EOVERFLOW);
    }
    return (int)o.total;
}
