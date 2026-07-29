/* win32_format.c - the printf format engine.
 *
 * Split out from win32_msvcrt.c because it is the one piece of the Win32
 * emulation with no kernel dependencies at all: it is a pure function
 * from a format string plus a block of argument words to a character
 * buffer. That makes it directly testable on the build host against the
 * host C library's own printf, which tests/format_host_test.c does - a
 * much sharper feedback loop than reading formatted output off a
 * framebuffer in QEMU.
 *
 * Everything variadic in the emulated CRT funnels through here: printf,
 * fprintf, sprintf, snprintf, their v* forms, and user32's wsprintfA.
 * Arguments are read straight off the caller's ring-3 stack, which is
 * exactly what cdecl makes possible.
 *
 * Floating-point conversions hand off to win32_dtoa.c, which never lets
 * the value become a C `double` - see the comment at the top of that
 * file for why that matters inside an interrupt handler. */

#include "win32_internal.h"
#include "kstring.h"


typedef struct {
    int left;       /* '-' */
    int plus;       /* '+' */
    int space;      /* ' ' */
    int zero;       /* '0' */
    int alternate;  /* '#' */
    int width;
    int precision;  /* -1 when unspecified */
    int longlong;   /* ll / I64 / j / z / t */
} fmt_spec_t;

typedef struct {
    char* out;
    uint32_t size;
    uint32_t written; /* counts past the end, so truncation is detectable */
} fmt_sink_t;

static void sink_char(fmt_sink_t* s, char c) {
    if (s->written + 1 < s->size) s->out[s->written] = c;
    s->written++;
}

static void sink_pad(fmt_sink_t* s, char c, int n) {
    for (int i = 0; i < n; i++) sink_char(s, c);
}

/* Emits `body` honoring width/left-alignment/zero-fill, with `prefix`
 * (a sign or "0x") kept in front of any zero padding. */
static void sink_padded(fmt_sink_t* s, const fmt_spec_t* f, const char* prefix,
                        const char* body, uint32_t body_len) {
    uint32_t prefix_len = prefix ? kstrlen(prefix) : 0;
    int total = (int)(prefix_len + body_len);
    int pad = f->width > total ? f->width - total : 0;

    if (!f->left && !f->zero) sink_pad(s, ' ', pad);
    for (uint32_t i = 0; i < prefix_len; i++) sink_char(s, prefix[i]);
    if (!f->left && f->zero) sink_pad(s, '0', pad);
    for (uint32_t i = 0; i < body_len; i++) sink_char(s, body[i]);
    if (f->left) sink_pad(s, ' ', pad);
}

static uint32_t u64_to_text(uint64_t value, uint32_t base, int uppercase,
                            char* out) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    uint32_t n = 0;
    if (value == 0) tmp[n++] = '0';
    while (value > 0) {
        uint32_t digit;
        value = kdiv64(value, base, &digit); /* not `/` - see kstring.h */
        tmp[n++] = digits[digit];
    }
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return n;
}

static void format_integer(fmt_sink_t* s, const fmt_spec_t* f, uint64_t value,
                           int is_signed, uint32_t base, int uppercase) {
    char prefix[4] = {0};
    int p = 0;

    if (is_signed) {
        int64_t signed_value = (int64_t)value;
        if (signed_value < 0) {
            prefix[p++] = '-';
            value = (uint64_t)(-signed_value);
        } else if (f->plus) {
            prefix[p++] = '+';
        } else if (f->space) {
            prefix[p++] = ' ';
        }
    } else if (f->alternate && base == 16 && value != 0) {
        prefix[p++] = '0';
        prefix[p++] = uppercase ? 'X' : 'x';
    }
    prefix[p] = '\0';

    char body[24];
    uint32_t len = u64_to_text(value, base, uppercase, body);

    /* An explicit precision is a minimum digit count, and it also
     * cancels zero-padding, per C. */
    fmt_spec_t local = *f;
    char padded[64];
    if (f->precision >= 0) {
        local.zero = 0;
        int lead = f->precision - (int)len;
        if (lead > 0 && lead + (int)len < (int)sizeof(padded)) {
            for (int i = 0; i < lead; i++) padded[i] = '0';
            kmemcpy(padded + lead, body, len);
            len += (uint32_t)lead;
            padded[len] = '\0';
            sink_padded(s, &local, prefix, padded, len);
            return;
        }
    }
    sink_padded(s, &local, prefix, body, len);
}

uint32_t w32_format(char* out, uint32_t out_size, const char* fmt,
                    const uint32_t* argp) {
    fmt_sink_t sink = { out, out_size, 0 };
    if (!fmt) fmt = "(null)";

    while (*fmt) {
        if (*fmt != '%') {
            sink_char(&sink, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            sink_char(&sink, '%');
            fmt++;
            continue;
        }

        fmt_spec_t f;
        kmemset(&f, 0, sizeof(f));
        f.precision = -1;

        for (;; fmt++) {
            if (*fmt == '-') f.left = 1;
            else if (*fmt == '+') f.plus = 1;
            else if (*fmt == ' ') f.space = 1;
            else if (*fmt == '0') f.zero = 1;
            else if (*fmt == '#') f.alternate = 1;
            else break;
        }

        if (*fmt == '*') {
            f.width = (int)*argp++;
            if (f.width < 0) { f.left = 1; f.width = -f.width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') f.width = f.width * 10 + (*fmt++ - '0');
        }

        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                f.precision = (int)*argp++;
                fmt++;
            } else {
                f.precision = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    f.precision = f.precision * 10 + (*fmt++ - '0');
                }
            }
        }

        /* Length modifiers. Only the 64-bit ones change how many stack
         * slots a conversion consumes, which is the part that matters. */
        for (;;) {
            if (fmt[0] == 'l' && fmt[1] == 'l') { f.longlong = 1; fmt += 2; }
            else if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') {
                f.longlong = 1; fmt += 3;
            }
            else if (fmt[0] == 'I' && fmt[1] == '3' && fmt[2] == '2') { fmt += 3; }
            else if (*fmt == 'l' || *fmt == 'h' || *fmt == 'j' || *fmt == 'z' ||
                     *fmt == 't' || *fmt == 'L' || *fmt == 'I' || *fmt == 'w') {
                fmt++;
            }
            else break;
        }

        char conv = *fmt++;
        switch (conv) {
            case 'd': case 'i': {
                int64_t v;
                if (f.longlong) {
                    v = (int64_t)(((uint64_t)argp[1] << 32) | argp[0]);
                    argp += 2;
                } else {
                    v = (int32_t)*argp++;
                }
                format_integer(&sink, &f, (uint64_t)v, 1, 10, 0);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': case 'b': {
                uint64_t v;
                if (f.longlong) {
                    v = ((uint64_t)argp[1] << 32) | argp[0];
                    argp += 2;
                } else {
                    v = *argp++;
                }
                uint32_t base = (conv == 'u') ? 10 : (conv == 'o') ? 8
                              : (conv == 'b') ? 2 : 16;
                format_integer(&sink, &f, v, 0, base, conv == 'X');
                break;
            }
            case 'p': {
                uint32_t v = *argp++;
                char body[16];
                uint32_t len = ku32_to_hex(v, body, 1, 8);
                fmt_spec_t local = f;
                local.zero = 0;
                sink_padded(&sink, &local, "0x", body, len);
                break;
            }
            case 'c': {
                char c = (char)*argp++;
                fmt_spec_t local = f;
                local.zero = 0;
                sink_padded(&sink, &local, 0, &c, 1);
                break;
            }
            case 's': {
                const char* s = (const char*)*argp++;
                if (!s) s = "(null)";
                uint32_t len = (f.precision >= 0)
                             ? kstrnlen(s, (uint32_t)f.precision)
                             : kstrlen(s);
                fmt_spec_t local = f;
                local.zero = 0;
                sink_padded(&sink, &local, 0, s, len);
                break;
            }
            case 'S': {
                /* A wide string in an ANSI format, which the CRT allows.
                 * Rendered a chunk at a time to bound the scratch. */
                const uint16_t* w = (const uint16_t*)*argp++;
                char narrow[128];
                uint32_t len = w32_wide_to_ascii(w, narrow, sizeof(narrow));
                fmt_spec_t local = f;
                local.zero = 0;
                sink_padded(&sink, &local, 0, narrow, len);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            case 'a': case 'A': {
                /* A double occupies two stack slots regardless of the
                 * length modifier - floats are promoted by the ABI. */
                uint64_t bits = ((uint64_t)argp[1] << 32) | argp[0];
                argp += 2;

                char body[680];
                char conv_eff = (conv == 'a' || conv == 'A')
                              ? (char)(conv == 'A' ? 'E' : 'e') : conv;
                uint32_t len = w32_format_double(body, sizeof(body), bits,
                                                 f.precision, conv_eff,
                                                 f.alternate);
                const char* prefix = 0;
                if (bits >> 63) prefix = "-";
                else if (f.plus) prefix = "+";
                else if (f.space) prefix = " ";
                sink_padded(&sink, &f, prefix, body, len);
                break;
            }
            case 'n': {
                int* p = (int*)*argp++;
                if (p) *p = (int)sink.written;
                break;
            }
            case '\0':
                /* A trailing '%' - emit it literally and stop. */
                sink_char(&sink, '%');
                fmt--;
                break;
            default:
                sink_char(&sink, '%');
                sink_char(&sink, conv);
                break;
        }
    }

    if (sink.size > 0) {
        uint32_t at = sink.written < sink.size ? sink.written : sink.size - 1;
        sink.out[at] = '\0';
    }
    return sink.written;
}

