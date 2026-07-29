/* win32_dtoa.c - turning an IEEE-754 double into decimal text.
 *
 * printf("%f") has to work for an emulated program to be much use, and
 * doing it properly is more interesting than it looks. Two constraints
 * shape this file:
 *
 *   1. The value never becomes a C `double` anywhere in here. It arrives
 *      as the 64 bits the caller pushed on the stack and stays an
 *      integer. If the kernel loaded it into a floating-point register,
 *      GCC would emit x87 instructions inside an interrupt handler and
 *      quietly trash the user program's own FPU state - this kernel has
 *      no FPU save/restore on entry (nothing needed one before now).
 *
 *   2. Scaling by a power of ten has to be exact. A double's value is
 *      m * 2^e, and turning that into decimal digits means multiplying
 *      or dividing by powers of both 2 and 10, with intermediate values
 *      well past 64 bits (1e308 is over a thousand bits). So there's a
 *      small fixed-size bignum below, and the digits it produces are
 *      correct rather than approximated.
 *
 * Precision is capped at DTOA_MAX_PRECISION digits, which bounds the
 * bignum: beyond that the output is zero-padded. */

#include "win32_internal.h"
#include "kstring.h"

#define DTOA_MAX_PRECISION 40

/* 64 x 32 bits = 2048, comfortably over the ~1160 needed for the widest
 * case (a maximal exponent scaled by 10^40). */
#define BN_LIMBS 64

typedef struct {
    uint32_t v[BN_LIMBS]; /* little-endian limbs, base 2^32 */
    int n;                /* limbs in use */
} bn_t;

static void bn_from_u64(bn_t* b, uint64_t value) {
    kmemset(b, 0, sizeof(*b));
    b->v[0] = (uint32_t)(value & 0xFFFFFFFFull);
    b->v[1] = (uint32_t)(value >> 32);
    b->n = b->v[1] ? 2 : (b->v[0] ? 1 : 0);
}

static int bn_is_zero(const bn_t* b) { return b->n == 0; }

static void bn_trim(bn_t* b) {
    while (b->n > 0 && b->v[b->n - 1] == 0) b->n--;
}

static void bn_mul_small(bn_t* b, uint32_t m) {
    uint64_t carry = 0;
    for (int i = 0; i < b->n; i++) {
        uint64_t p = (uint64_t)b->v[i] * m + carry;
        b->v[i] = (uint32_t)p;
        carry = p >> 32;
    }
    while (carry && b->n < BN_LIMBS) {
        b->v[b->n++] = (uint32_t)carry;
        carry >>= 32;
    }
}

static void bn_add_small(bn_t* b, uint32_t a) {
    uint64_t carry = a;
    for (int i = 0; i < b->n && carry; i++) {
        uint64_t s = (uint64_t)b->v[i] + carry;
        b->v[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry && b->n < BN_LIMBS) b->v[b->n++] = (uint32_t)carry;
}

/* Divides in place, returning the remainder. Each limb is one DIV step
 * with the previous limb's remainder as the high word - see kdiv_hi_lo. */
static uint32_t bn_div_small(bn_t* b, uint32_t d) {
    uint32_t rem = 0;
    for (int i = b->n - 1; i >= 0; i--) {
        b->v[i] = kdiv_hi_lo(rem, b->v[i], d, &rem);
    }
    bn_trim(b);
    return rem;
}

static void bn_shl(bn_t* b, uint32_t bits) {
    if (bn_is_zero(b) || bits == 0) return;

    uint32_t limbs = bits / 32;
    uint32_t rest = bits % 32;

    if (limbs) {
        int top = b->n + (int)limbs;
        if (top > BN_LIMBS) top = BN_LIMBS;
        for (int i = top - 1; i >= (int)limbs; i--) b->v[i] = b->v[i - limbs];
        for (uint32_t i = 0; i < limbs && (int)i < BN_LIMBS; i++) b->v[i] = 0;
        b->n = top;
    }

    if (rest) {
        uint32_t carry = 0;
        for (int i = 0; i < b->n; i++) {
            uint32_t next = b->v[i] >> (32 - rest);
            b->v[i] = (b->v[i] << rest) | carry;
            carry = next;
        }
        if (carry && b->n < BN_LIMBS) b->v[b->n++] = carry;
    }
    bn_trim(b);
}

/* Right shift, rounding to nearest with ties going to even - the rule C
 * specifies and the one the host printf uses, so "%.0f" of 2.5 is "2"
 * rather than "3". A tie is "the top dropped bit is 1 and every bit below
 * it is 0"; anything below it being set makes the value more than half,
 * which always rounds up. */
static void bn_shr_round(bn_t* b, uint32_t bits) {
    if (bn_is_zero(b)) return;
    if (bits >= (uint32_t)BN_LIMBS * 32) {
        kmemset(b, 0, sizeof(*b));
        return;
    }

    int half = 0;   /* the highest dropped bit, at position bits-1 */
    int sticky = 0; /* any dropped bit below it */
    if (bits > 0) {
        uint32_t idx = (bits - 1) / 32;
        uint32_t off = (bits - 1) % 32;
        if ((int)idx < b->n && ((b->v[idx] >> off) & 1)) half = 1;

        for (uint32_t i = 0; i < idx && !sticky; i++) {
            if ((int)i < b->n && b->v[i]) sticky = 1;
        }
        if (!sticky && off > 0 && (int)idx < b->n) {
            if (b->v[idx] & ((1u << off) - 1u)) sticky = 1;
        }
    }

    uint32_t limbs = bits / 32;
    uint32_t rest = bits % 32;

    if (limbs) {
        for (int i = 0; i + (int)limbs < b->n; i++) b->v[i] = b->v[i + limbs];
        for (int i = b->n - (int)limbs; i < b->n; i++) {
            if (i >= 0) b->v[i] = 0;
        }
        b->n -= (int)limbs;
        if (b->n < 0) b->n = 0;
    }

    if (rest && b->n > 0) {
        for (int i = 0; i < b->n; i++) {
            uint32_t hi = (i + 1 < b->n) ? b->v[i + 1] : 0;
            b->v[i] = (b->v[i] >> rest) | (hi << (32 - rest));
        }
    }
    bn_trim(b);

    /* Ties to even: only round up on an exact half if the surviving
     * low bit is odd. */
    if (half && (sticky || (b->n > 0 && (b->v[0] & 1)))) bn_add_small(b, 1);
}

static void bn_mul_pow10(bn_t* b, uint32_t n) {
    while (n >= 9) { bn_mul_small(b, 1000000000u); n -= 9; }
    static const uint32_t small_pow10[9] = {
        1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
    };
    if (n) bn_mul_small(b, small_pow10[n]);
}

/* Divides by 10^n, rounding half-up. The last division is done on its own
 * so its remainder is available to round with. */
static void bn_div_pow10_round(bn_t* b, uint32_t n) {
    if (n == 0) return;

    /* Same nearest-with-ties-to-even rule as bn_shr_round: any non-zero
     * remainder dropped before the final digit makes the value more than
     * an exact half. */
    int sticky = 0;
    for (uint32_t i = 0; i + 1 < n; i++) {
        if (bn_div_small(b, 10)) sticky = 1;
    }
    uint32_t r = bn_div_small(b, 10);
    if (r > 5 || (r == 5 && (sticky || (b->n > 0 && (b->v[0] & 1))))) {
        bn_add_small(b, 1);
    }
}

/* Renders as decimal digits, most significant first. Returns the length;
 * zero renders as "0". */
static uint32_t bn_to_dec(bn_t* b, char* out, uint32_t out_size) {
    if (bn_is_zero(b)) {
        if (out_size >= 2) { out[0] = '0'; out[1] = '\0'; }
        return 1;
    }

    char tmp[512];
    uint32_t n = 0;
    while (!bn_is_zero(b) && n < sizeof(tmp)) {
        uint32_t chunk = bn_div_small(b, 1000000000u);
        for (int i = 0; i < 9; i++) {
            if (n >= sizeof(tmp)) break;
            tmp[n++] = (char)('0' + (chunk % 10));
            chunk /= 10;
        }
    }
    while (n > 1 && tmp[n - 1] == '0') n--; /* strip the chunk's zero padding */

    uint32_t len = n < out_size - 1 ? n : out_size - 1;
    for (uint32_t i = 0; i < len; i++) out[i] = tmp[n - 1 - i];
    out[len] = '\0';
    return len;
}

/* --- the double itself -------------------------------------------------- */

/* Splits the IEEE-754 bits into a significand and a base-2 exponent such
 * that |value| == mantissa * 2^exp2 exactly. */
static void decompose(uint64_t bits, uint64_t* mantissa, int32_t* exp2,
                      int* negative, int* is_inf, int* is_nan) {
    *negative = (int)(bits >> 63);
    uint32_t raw_exp = (uint32_t)((bits >> 52) & 0x7FF);
    uint64_t frac = bits & 0x000FFFFFFFFFFFFFull;

    *is_inf = 0;
    *is_nan = 0;

    if (raw_exp == 0x7FF) {
        if (frac) *is_nan = 1; else *is_inf = 1;
        *mantissa = 0;
        *exp2 = 0;
        return;
    }

    if (raw_exp == 0) {
        /* Subnormal: no implicit leading 1, and the exponent is fixed. */
        *mantissa = frac;
        *exp2 = -1074;
    } else {
        *mantissa = frac | 0x0010000000000000ull;
        *exp2 = (int32_t)raw_exp - 1075; /* 1023 bias + 52 fraction bits */
    }
}

/* Computes round(|value| * 10^scale) exactly into `out`. */
static void scaled_integer(bn_t* out, uint64_t mantissa, int32_t exp2,
                           int32_t scale) {
    bn_from_u64(out, mantissa);

    if (scale > 0) bn_mul_pow10(out, (uint32_t)scale);

    if (exp2 > 0) {
        bn_shl(out, (uint32_t)exp2);
    } else if (exp2 < 0) {
        bn_shr_round(out, (uint32_t)(-exp2));
    }

    if (scale < 0) bn_div_pow10_round(out, (uint32_t)(-scale));
}

/* Estimates floor(log10(|value|)). log10(2) is 0.30103, applied to the
 * position of the significand's top bit; the caller corrects the answer
 * by checking the digit count it actually produces. */
static int32_t log10_estimate(uint64_t mantissa, int32_t exp2) {
    /* Zero has no logarithm. C renders it as 0.000000e+00, so the caller
     * needs an exponent of 0, not the -324 the arithmetic below would
     * produce from a zero significand. */
    if (mantissa == 0) return 0;

    int32_t top_bit = 63;
    while (top_bit > 0 && !((mantissa >> top_bit) & 1)) top_bit--;
    /* Stays in 32 bits: the exponent is bounded by +/-1074 and 30103 is
     * five digits, so the product is well under 2^31. */
    int32_t bit_position = top_bit + exp2;
    /* 30103/100000 == log10(2), kept in integers. Floor division, so that
     * negative exponents round the right way. */
    int32_t v = bit_position * 30103;
    return v >= 0 ? v / 100000 : -(((-v) + 99999) / 100000);
}

static uint32_t emit(char* out, uint32_t out_size, uint32_t at, char c) {
    if (at + 1 < out_size) out[at] = c;
    return at + 1;
}

static uint32_t emit_str(char* out, uint32_t out_size, uint32_t at,
                         const char* s) {
    while (*s) at = emit(out, out_size, at, *s++);
    return at;
}

/* Fixed-point form: <digits>.<precision digits>. */
static uint32_t format_fixed(char* out, uint32_t out_size, uint64_t mantissa,
                             int32_t exp2, int precision) {
    bn_t scaled;
    scaled_integer(&scaled, mantissa, exp2, precision);

    char digits[600];
    uint32_t len = bn_to_dec(&scaled, digits, sizeof(digits));

    uint32_t at = 0;
    if ((int)len <= precision) {
        /* Value is below 1: "0." then leading zeros then the digits. */
        at = emit(out, out_size, at, '0');
        if (precision > 0) {
            at = emit(out, out_size, at, '.');
            for (int i = 0; i < precision - (int)len; i++) {
                at = emit(out, out_size, at, '0');
            }
            for (uint32_t i = 0; i < len; i++) {
                at = emit(out, out_size, at, digits[i]);
            }
        }
    } else {
        uint32_t int_len = len - (uint32_t)precision;
        for (uint32_t i = 0; i < int_len; i++) at = emit(out, out_size, at, digits[i]);
        if (precision > 0) {
            at = emit(out, out_size, at, '.');
            for (uint32_t i = int_len; i < len; i++) {
                at = emit(out, out_size, at, digits[i]);
            }
        }
    }

    if (at < out_size) out[at] = '\0';
    else if (out_size) out[out_size - 1] = '\0';
    return at;
}

/* Scientific form: d.ddde±XX. */
static uint32_t format_exponential(char* out, uint32_t out_size,
                                   uint64_t mantissa, int32_t exp2,
                                   int precision, int uppercase,
                                   int32_t* out_exponent) {
    int32_t k = log10_estimate(mantissa, exp2);
    char digits[600];
    uint32_t len = 0;
    bn_t scaled;

    if (mantissa == 0) {
        /* Zero has no significant digits to find, and the correction loop
         * below would keep decrementing the exponent looking for them.
         * C renders it as 0.000...e+00. */
        for (int i = 0; i <= precision && i < (int)sizeof(digits) - 1; i++) {
            digits[i] = '0';
        }
        len = (uint32_t)precision + 1;
        digits[len] = '\0';
        k = 0;
        goto emit_digits;
    }

    /* We want exactly precision+1 significant digits, i.e. the integer
     * round(value * 10^(precision - k)). The estimate for k can be off by
     * one either way, which shows up as a digit count that isn't
     * precision+1 - so correct k and redo. Two passes always suffice. */
    for (int attempt = 0; attempt < 3; attempt++) {
        scaled_integer(&scaled, mantissa, exp2, precision - k);
        len = bn_to_dec(&scaled, digits, sizeof(digits));

        if ((int)len == precision + 1) break;
        if ((int)len > precision + 1) k++;
        else k--;
    }

emit_digits:;
    uint32_t at = 0;
    at = emit(out, out_size, at, digits[0]);
    if (precision > 0) {
        at = emit(out, out_size, at, '.');
        for (int i = 1; i <= precision && i < (int)len; i++) {
            at = emit(out, out_size, at, digits[i]);
        }
        for (int i = (int)len; i <= precision; i++) at = emit(out, out_size, at, '0');
    }

    at = emit(out, out_size, at, uppercase ? 'E' : 'e');
    at = emit(out, out_size, at, k < 0 ? '-' : '+');

    uint32_t mag = (uint32_t)(k < 0 ? -k : k);
    /* At least two exponent digits, per C. */
    if (mag >= 100) at = emit(out, out_size, at, (char)('0' + (mag / 100) % 10));
    at = emit(out, out_size, at, (char)('0' + (mag / 10) % 10));
    at = emit(out, out_size, at, (char)('0' + mag % 10));

    if (at < out_size) out[at] = '\0';
    else if (out_size) out[out_size - 1] = '\0';

    if (out_exponent) *out_exponent = k;
    return at;
}

static void strip_trailing_zeros(char* s) {
    uint32_t len = kstrlen(s);
    if (!kstrchr(s, '.')) return;

    /* Only touch the fractional part - "1.500e+02" keeps its exponent. */
    char* e = kstrchr(s, 'e');
    if (!e) e = kstrchr(s, 'E');

    uint32_t frac_end = e ? (uint32_t)(e - s) : len;
    uint32_t cut = frac_end;
    while (cut > 0 && s[cut - 1] == '0') cut--;
    if (cut > 0 && s[cut - 1] == '.') cut--;

    if (cut == frac_end) return;
    if (e) {
        kmemmove(s + cut, e, kstrlen(e) + 1);
    } else {
        s[cut] = '\0';
    }
}

uint32_t w32_format_double(char* out, uint32_t out_size, uint64_t bits,
                           int precision, char conv, int alternate) {
    uint64_t mantissa;
    int32_t exp2;
    int negative, is_inf, is_nan;
    decompose(bits, &mantissa, &exp2, &negative, &is_inf, &is_nan);

    int uppercase = (conv == 'E' || conv == 'F' || conv == 'G');
    char lower = (char)(conv | 0x20);

    if (is_nan) {
        uint32_t n = emit_str(out, out_size, 0, uppercase ? "NAN" : "nan");
        if (n < out_size) out[n] = '\0';
        return n;
    }
    if (is_inf) {
        uint32_t n = emit_str(out, out_size, 0, uppercase ? "INF" : "inf");
        if (n < out_size) out[n] = '\0';
        return n;
    }

    if (precision < 0) precision = 6;
    if (precision > DTOA_MAX_PRECISION) precision = DTOA_MAX_PRECISION;

    if (lower == 'f') {
        return format_fixed(out, out_size, mantissa, exp2, precision);
    }
    if (lower == 'e') {
        return format_exponential(out, out_size, mantissa, exp2, precision,
                                  uppercase, 0);
    }

    /* %g: significant digits, choosing the shorter-looking of the two
     * forms exactly the way C specifies, then dropping trailing zeros
     * unless the caller asked for the alternate form. */
    int significant = precision == 0 ? 1 : precision;
    int32_t exponent = 0;

    char scratch[640];
    format_exponential(scratch, sizeof(scratch), mantissa, exp2,
                       significant - 1, uppercase, &exponent);

    uint32_t n;
    if (exponent < -4 || exponent >= significant) {
        n = kstrlen(scratch);
        if (n >= out_size) n = out_size ? out_size - 1 : 0;
        kmemcpy(out, scratch, n);
        out[n] = '\0';
    } else {
        n = format_fixed(out, out_size, mantissa, exp2,
                         significant - 1 - (int)exponent);
        if (n >= out_size && out_size) out[out_size - 1] = '\0';
    }

    if (!alternate) strip_trailing_zeros(out);
    return kstrlen(out);
}
