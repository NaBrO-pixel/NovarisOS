/* signal_test.c - a Linux signal program, run on Novaris.
 *
 * Same rules as posix_test.c: raw `int $0x80`, Linux syscall numbers,
 * built with plain `gcc -m32 -static -nostdlib -ffreestanding`, linked
 * against nothing. The same binary is run on the Linux build host and on
 * Novaris and the transcripts compared.
 *
 * The last test is the one that matters. It is the pattern Wine is built
 * on: install a SIGSEGV handler, touch a deliberately unmapped page, fix
 * the mapping *inside the handler*, return - and the faulting instruction
 * is retried and now succeeds. A kernel that cannot do that cannot run
 * Wine, however much of the rest of the ABI it implements.
 *
 * See ROADMAP.md Milestone 19.
 */

static long sc1(long n, long a) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}
static long sc2(long n, long a, long b) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b)
                         : "memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return r;
}
static long sc4(long n, long a, long b, long c, long d) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
                         : "memory");
    return r;
}
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    __asm__ __volatile__("push %%ebp\n\t" "mov %7, %%ebp\n\t"
                         "int $0x80\n\t" "pop %%ebp"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "g"(f)
                         : "memory");
    return r;
}

#define SYS_exit           1
#define SYS_write          4
#define SYS_getpid        20
#define SYS_kill          37
#define SYS_mprotect     125
#define SYS_rt_sigreturn 173
#define SYS_rt_sigaction 174
#define SYS_rt_sigprocmask 175
#define SYS_mmap2        192

#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12

#define SA_SIGINFO  0x00000004u
#define SA_RESTORER 0x04000000u
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

/* The kernel's rt_sigaction argument. Not libc's struct sigaction - the
 * field order differs, and going through raw syscalls means using the
 * kernel's. */
struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask[2];
};

/* The restorer. i386's raw rt_sigaction requires the caller to supply
 * one - glibc has its own, and a program using raw syscalls needs its
 * own too. The handler `ret`s onto this, and it issues rt_sigreturn,
 * which is what tells the kernel to put the interrupted context back.
 *
 * SA_SIGINFO is not optional here, and the reason is a genuine i386
 * wrinkle: without it Linux builds the *old* sigframe and expects
 * `sigreturn` (119), not `rt_sigreturn` (173). With it, the rt_ frame
 * and the rt_ return go together. SA_SIGINFO is also what glibc and Wine
 * actually use, so this is the path worth supporting. */
extern void restorer(void);
__asm__(".text\n"
        ".globl restorer\n"
        ".type restorer,@function\n"
        "restorer:\n"
        "  movl $173, %eax\n"   /* rt_sigreturn */
        "  int $0x80\n"
        ".size restorer,.-restorer\n");

static int install(int signo, void (*handler)(int)) {
    struct k_sigaction sa;
    sa.handler = handler;
    sa.flags = SA_SIGINFO | SA_RESTORER;
    sa.restorer = restorer;
    sa.mask[0] = 0;
    sa.mask[1] = 0;
    return (int)sc4(SYS_rt_sigaction, signo, (long)&sa, 0, 8);
}

/* --- a few bytes of libc ------------------------------------------------- */

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }
static void out_dec(long v) {
    char buf[24]; int i = 23; int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    buf[i--] = '\0';
    if (!u) buf[i--] = '0';
    while (u) { buf[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) buf[i--] = '-';
    out(&buf[i + 1]);
}
static void check(const char* label, int ok) {
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(label);
    out("\n");
}

/* --- state the handlers touch -------------------------------------------- */

static volatile int usr1_count = 0;
static volatile int usr1_signo = 0;
static volatile int usr2_count = 0;
static volatile int segv_count = 0;
static volatile unsigned long guard_page = 0;

static void on_usr1(int signo) { usr1_count++; usr1_signo = signo; }
static void on_usr2(int signo) { (void)signo; usr2_count++; }

/* The Wine pattern. The faulting store is retried after this returns, so
 * making the page writable here is what lets it succeed. */
static void on_segv(int signo) {
    (void)signo;
    segv_count++;
    sc3(SYS_mprotect, (long)guard_page, 4096, PROT_READ | PROT_WRITE);
}

int main_(void) {
    out("signal test - written for Linux, running unmodified\n\n");

    long pid = sc1(SYS_getpid, 0);

    /* 1. A handler runs, and gets the right signal number. */
    out("1. rt_sigaction + kill\n");
    check("rt_sigaction(SIGUSR1) accepted", install(SIGUSR1, on_usr1) == 0);
    sc2(SYS_kill, pid, SIGUSR1);
    check("handler ran exactly once", usr1_count == 1);
    check("handler received SIGUSR1", usr1_signo == SIGUSR1);

    /* Execution continues correctly after the handler returned - which is
     * the part sigreturn is responsible for. */
    volatile long witness = 0x12345678;
    sc2(SYS_kill, pid, SIGUSR1);
    check("handler ran a second time", usr1_count == 2);
    check("locals survived the handler", witness == 0x12345678);

    /* 2. A handler with no SA_RESTORER. The kernel has to supply the
     *    trampoline itself in that case - Linux does, so Novaris must
     *    too, or the same binary would behave differently on the two. */
    out("\n2. a handler with no SA_RESTORER\n");
    {
        struct k_sigaction sa;
        sa.handler = on_usr2;
        sa.flags = SA_SIGINFO;   /* deliberately no SA_RESTORER */
        sa.restorer = 0;
        sa.mask[0] = 0; sa.mask[1] = 0;
        long r = sc4(SYS_rt_sigaction, SIGUSR2, (long)&sa, 0, 8);
        check("rt_sigaction without SA_RESTORER is accepted", r == 0);
        sc2(SYS_kill, pid, SIGUSR2);
        check("its handler ran on a kernel-supplied trampoline",
              usr2_count == 1);
        usr2_count = 0;
    }

    /* 3. Blocking. A blocked signal stays pending and arrives on unblock. */
    out("\n3. rt_sigprocmask\n");
    check("rt_sigaction(SIGUSR2) accepted", install(SIGUSR2, on_usr2) == 0);
    unsigned long set[2] = { 1UL << (SIGUSR2 - 1), 0 };
    check("blocking SIGUSR2",
          sc4(SYS_rt_sigprocmask, SIG_BLOCK, (long)set, 0, 8) == 0);
    sc2(SYS_kill, pid, SIGUSR2);
    check("blocked signal did NOT run its handler", usr2_count == 0);
    check("unblocking SIGUSR2",
          sc4(SYS_rt_sigprocmask, SIG_UNBLOCK, (long)set, 0, 8) == 0);
    check("pending signal arrived on unblock", usr2_count == 1);

    /* 4. The Wine pattern: fault on purpose, fix it in the handler,
     *    and have the faulting instruction succeed on retry. */
    out("\n4. SIGSEGV handler that fixes the fault and returns\n");
    check("rt_sigaction(SIGSEGV) accepted", install(SIGSEGV, on_segv) == 0);

    long p = sc6(SYS_mmap2, 0, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int have = !(p < 0 && p > -4096) && p != 0;
    check("mapped a page to make unwritable", have);

    if (have) {
        guard_page = (unsigned long)p;
        volatile unsigned long* cell = (volatile unsigned long*)p;
        *cell = 0xAAAAAAAA;                       /* fine: still writable */

        check("made the page read-only",
              sc3(SYS_mprotect, p, 4096, PROT_READ) == 0);

        /* This store faults. The handler mprotects the page back to
         * read/write and returns; the store is then retried. */
        *cell = 0xDEADBEEF;

        check("SIGSEGV handler ran once", segv_count == 1);
        out("  the faulting store now reads back as ");
        out_dec((long)(*cell == 0xDEADBEEF));
        out(" (1 = it completed)\n");
        check("the faulting instruction was retried and succeeded",
              *cell == 0xDEADBEEF);
    }

    out("\nsignal test done.\n");
    return 0;
}

void _start(void) {
    sc1(SYS_exit, main_());
    __builtin_unreachable();
}
