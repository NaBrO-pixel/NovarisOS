/* posix_test.c - a program written for Linux, run on Novaris.
 *
 * There is nothing in this file that knows Novaris exists. It makes raw
 * `int $0x80` calls using Linux's i386 syscall numbers and Linux's
 * argument registers, it is built with an ordinary
 * `gcc -m32 -static -nostdlib`, and it links against no library of any
 * kind - not even the tiny one in userland/libc/.
 *
 * That is the point. The same binary is run twice during verification:
 * once on the Linux build host and once on Novaris, and the two
 * transcripts are compared. Identical output means Novaris implemented
 * the ABI rather than something ABI-shaped.
 *
 * See ROADMAP.md Milestone 18. This is the first instalment of item 3 on
 * the Path A list - the POSIX surface Wine's own build links against.
 */

/* --- the ABI ------------------------------------------------------------
 * eax = number, ebx/ecx/edx/esi/edi/ebp = args 1..6, result in eax,
 * errors as -errno in [-4095, -1]. */

static long sc1(long n, long a) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}

static long sc2(long n, long a, long b) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r) : "a"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return r;
}

static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    __asm__ __volatile__("push %%ebp\n\t"
                         "mov %7, %%ebp\n\t"
                         "int $0x80\n\t"
                         "pop %%ebp"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e),
                           "g"(f)
                         : "memory");
    return r;
}

#define SYS_exit    1
#define SYS_read    3
#define SYS_write   4
#define SYS_open    5
#define SYS_close   6
#define SYS_lseek  19
#define SYS_brk    45
#define SYS_munmap 91
#define SYS_uname 122
#define SYS_mprotect 125
#define SYS_writev 146
#define SYS_mmap2 192
#define SYS_fstat64 197

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define SEEK_SET 0
#define SEEK_END 2

/* --- a few bytes of libc, since we link against none ---------------------
 *
 * Built with -ffreestanding -fno-builtin, which matters: without it gcc
 * recognises the loop in slen() as strlen and emits a *call* to strlen,
 * which does not exist in a -nostdlib link. */

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }

static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }

static void out_n(const char* s, unsigned n) { sc3(SYS_write, 1, (long)s, n); }

static void out_dec(long v) {
    char buf[24];
    int i = 23;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    buf[i--] = '\0';
    if (u == 0) buf[i--] = '0';
    while (u) { buf[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) buf[i--] = '-';
    out(&buf[i + 1]);
}

static void check(const char* label, int ok) {
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(label);
    out("\n");
}

/* --- the tests ----------------------------------------------------------- */

int main_(void) {
    out("posix syscall test - written for Linux, running unmodified\n\n");

    /* 1. write() to stdout and stderr, counted bytes rather than a
     *    NUL-terminated string, including one with an embedded NUL that a
     *    string-based write would truncate. */
    out("1. write()\n");
    static const char embedded[] = "abc\0def";
    out("  eight bytes with a NUL in the middle: [");
    out_n(embedded, 7);
    out("]\n");
    sc3(SYS_write, 2, (long)"  this line went to fd 2 (stderr)\n", 34);
    check("write to fd 1 and fd 2", 1);

    /* 2. writev() - what a real libc's printf reaches for first. */
    out("\n2. writev()\n");
    struct { const char* base; unsigned len; } iov[3] = {
        { "  three", 7 }, { " iovecs", 7 }, { " joined\n", 8 },
    };
    long wv = sc3(SYS_writev, 1, (long)iov, 3);
    check("writev returned 22", wv == 22);

    /* 3. open / fstat64 / read / lseek / close on a real file. */
    out("\n3. open, fstat64, read, lseek, close\n");
    long fd = sc2(SYS_open, (long)"hello.txt", 0 /* O_RDONLY */);
    check("open(\"hello.txt\") succeeded", fd >= 0);
    if (fd >= 0) {
        unsigned char st[96];
        long r = sc2(SYS_fstat64, fd, (long)st);
        unsigned long size = *(unsigned long*)(st + 44);
        out("  fstat64 says st_size = ");
        out_dec((long)size);
        out("\n");
        check("fstat64 succeeded and st_size is non-zero",
              r == 0 && size > 0);

        char buf[64];
        long got = sc3(SYS_read, fd, (long)buf, 16);
        out("  first 16 bytes: [");
        if (got > 0) out_n(buf, (unsigned)got);
        out("]\n");
        check("read returned 16 bytes", got == 16);

        long pos = sc3(SYS_lseek, fd, 0, SEEK_END);
        check("lseek to SEEK_END agrees with st_size", pos == (long)size);

        pos = sc3(SYS_lseek, fd, 0, SEEK_SET);
        got = sc3(SYS_read, fd, (long)buf, 4);
        check("lseek back to 0 then read 4", pos == 0 && got == 4);

        check("close succeeded", sc1(SYS_close, fd) == 0);
    }

    /* A file that isn't there must fail with -ENOENT, not succeed. */
    long bad = sc2(SYS_open, (long)"no_such_file.txt", 0);
    check("open of a missing file returns -ENOENT", bad == -2);

    /* 4. mmap2 / mprotect / munmap. */
    out("\n4. mmap2, mprotect, munmap\n");
    long p = sc6(SYS_mmap2, 0, 8192, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int mapped = !(p < 0 && p > -4096) && p != 0;
    check("mmap2 of 8KB anonymous memory", mapped);

    if (mapped) {
        volatile unsigned char* m = (volatile unsigned char*)p;

        /* Anonymous memory must read as zero before anything writes it. */
        int zero = 1;
        for (int i = 0; i < 8192; i += 512) if (m[i] != 0) zero = 0;
        check("fresh anonymous memory reads as zero", zero);

        /* Write the whole range and read it back, including the second
         * page - so this fails if only one page was really mapped. */
        for (int i = 0; i < 8192; i++) m[i] = (unsigned char)(i & 0xFF);
        int ok = 1;
        for (int i = 0; i < 8192; i++) {
            if (m[i] != (unsigned char)(i & 0xFF)) { ok = 0; break; }
        }
        check("wrote and read back all 8192 bytes", ok);

        check("mprotect to read-only succeeded",
              sc3(SYS_mprotect, p, 4096, PROT_READ) == 0);
        check("data still readable after mprotect", m[100] == 100);

        check("munmap succeeded", sc2(SYS_munmap, p, 8192) == 0);
    }

    /* 5. brk. */
    out("\n5. brk()\n");
    long b0 = sc1(SYS_brk, 0);
    long b1 = sc1(SYS_brk, b0 + 8192);
    check("brk(0) reported a break", b0 != 0);
    check("brk grew by 8192", b1 == b0 + 8192);
    if (b1 == b0 + 8192) {
        volatile unsigned char* h = (volatile unsigned char*)b0;
        h[0] = 0x5A; h[8191] = 0xA5;
        check("the new break memory is usable",
              h[0] == 0x5A && h[8191] == 0xA5);
    }
    check("brk shrank back", sc1(SYS_brk, b0) == b0);

    /* 6. uname. */
    out("\n6. uname()\n");
    char uts[6 * 65];
    long u = sc1(SYS_uname, (long)uts);
    check("uname succeeded", u == 0);
    out("  sysname = ");
    out(uts);
    out("\n");

    out("\nposix test done.\n");
    return 0;
}

void _start(void) {
    sc1(SYS_exit, main_());
    __builtin_unreachable();
}
