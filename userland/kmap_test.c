/* kmap_test.c - a program that tries to map over the kernel.
 *
 * This is the only test here that is deliberately *not* run on Linux as
 * well, and the reason is the point of it. On Linux, mmap(0x100000, ...,
 * MAP_FIXED) is an ordinary request for an ordinary user address and it
 * succeeds. On Novaris the first 4MB is identity-mapped and shared into
 * every address space, because the kernel's own image lives at physical
 * 0x100000 and has to stay reachable there - so granting the same request
 * replaces the kernel's view of itself.
 *
 * It did exactly that. Wine's preloader reserves the DOS area on startup
 * (0x10000-0x110000, which covers 0x100000) and the machine stopped dead:
 * no panic, no output, nothing. This program is the six lines that
 * reproduced it with no Wine involved, kept as a regression test.
 *
 * What it asserts is not a return value but a survival: the kernel says
 * no, and is still running afterwards to say anything at all.
 *
 * See ROADMAP.md Milestone 27.
 */

static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    __asm__ __volatile__("push %%ebp\n\t" "mov %7, %%ebp\n\t"
                         "int $0x80\n\t" "pop %%ebp"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "g"(f)
                         : "memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return r;
}

#define SYS_exit    1
#define SYS_write   4
#define SYS_mmap2 192

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_NORESERVE 0x4000

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }
static void check(const char* label, int ok) {
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(label);
    out("\n");
}

/* An error is anything in [-4095, -1]. */
static int failed(long r) { return r < 0 && r > -4096; }

void _start(void) {
    out("kernel-mapping guard test\n\n");

    /* Exactly what Wine's preloader asks for, and what hung the machine. */
    long r = sc6(SYS_mmap2, 0x10000, 0x100000, PROT_NONE,
                 MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    check("MAP_FIXED across the kernel's identity map is refused", failed(r));

    /* The kernel's load address on its own. */
    r = sc6(SYS_mmap2, 0x100000, 0x1000, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    check("MAP_FIXED on the kernel's load address is refused", failed(r));

    /* And a request that has nothing to do with the kernel still works,
     * so the guard is a guard and not a blanket refusal. */
    r = sc6(SYS_mmap2, 0, 0x1000, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("an ordinary anonymous mapping still succeeds", !failed(r) && r != 0);
    if (!failed(r)) {
        volatile unsigned* p = (volatile unsigned*)r;
        *p = 0x5A5A5A5A;
        check("and is readable and writable", *p == 0x5A5A5A5A);
    }

    /* The whole assertion: this line is reached. */
    out("\nthe kernel is still running.\n");
    sc3(SYS_exit, 0, 0, 0);
}
