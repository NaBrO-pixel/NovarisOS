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

#define SYS_munmap    91
#define SYS_gettid   224
#define SYS_tgkill   270

#define MAP_FIXED 0x10

#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12

/* si_code values, and the two Linux/i386 structures an SA_SIGINFO handler
 * is handed. Written out here rather than #include'd because this program
 * links against nothing - which is the point of it. */
#define SI_USER     0
#define SI_TKILL    (-6)
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define SS_DISABLE  2

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    union {
        unsigned long _pad[29];
        struct { int si_pid; unsigned int si_uid; } _kill;
        struct { void* si_addr; } _sigfault;
    } _sifields;
} k_siginfo;

/* glibc's gregset_t indices. uc_mcontext is 19 longs in this order,
 * then a struct _fpstate*, then oldmask and cr2. */
#define REG_GS 0
#define REG_FS 1
#define REG_ES 2
#define REG_DS 3
#define REG_EDI 4
#define REG_ESI 5
#define REG_EBP 6
#define REG_ESP 7
#define REG_EBX 8
#define REG_EDX 9
#define REG_ECX 10
#define REG_EAX 11
#define REG_TRAPNO 12
#define REG_ERR 13
#define REG_EIP 14
#define REG_CS 15
#define REG_EFL 16
#define REG_UESP 17
#define REG_SS 18

typedef struct {
    unsigned long gregs[19];
    void*         fpregs;
    unsigned long oldmask;
    unsigned long cr2;
} k_mcontext;

typedef struct {
    unsigned long uc_flags;
    void*         uc_link;
    struct { void* ss_sp; int ss_flags; unsigned long ss_size; } uc_stack;
    k_mcontext    uc_mcontext;
    unsigned long uc_sigmask[2];
} k_ucontext;

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

/* The same, for a handler that actually reads its second and third
 * arguments. The kernel pushes all three either way; this is only about
 * the C type. */
typedef void (*sa3_t)(int, k_siginfo*, k_ucontext*);
static int install3(int signo, sa3_t handler) {
    return install(signo, (void (*)(int))handler);
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

/* --- Milestone 23: what an SA_SIGINFO handler is actually handed -------
 *
 * Everything below reads (and in two cases writes) the siginfo_t and
 * ucontext_t. Nothing here prints an absolute address or a selector
 * value: those legitimately differ between the host and Novaris, and a
 * transcript comparison that demanded they match would be testing the
 * wrong thing. Every check is a relation between values the program
 * already knows. */

static volatile int info_signo = -1, info_errno = -1, info_code = -1;
static volatile int info_pid = -1;
static volatile int uc_was_null = 1, uc_ss_flags = -1;
static volatile unsigned long uc_saved_mask = 0xFFFFFFFFul;
static volatile unsigned long uc_eip = 0, uc_esp = 0, uc_uesp = 0;
static volatile unsigned long uc_cs = 0, uc_efl = 0, uc_ebx = 0;

static void on_usr1_info(int signo, k_siginfo* si, k_ucontext* uc) {
    info_signo = si->si_signo;
    info_errno = si->si_errno;
    info_code  = si->si_code;
    info_pid   = si->_sifields._kill.si_pid;
    uc_was_null = (uc == 0);
    if (!uc) return;
    uc_ss_flags   = uc->uc_stack.ss_flags;
    uc_saved_mask = uc->uc_sigmask[0];
    uc_eip = uc->uc_mcontext.gregs[REG_EIP];
    uc_esp = uc->uc_mcontext.gregs[REG_ESP];
    uc_uesp = uc->uc_mcontext.gregs[REG_UESP];
    uc_cs  = uc->uc_mcontext.gregs[REG_CS];
    uc_efl = uc->uc_mcontext.gregs[REG_EFL];
    uc_ebx = uc->uc_mcontext.gregs[REG_EBX];
    (void)signo;
}

/* The write-back direction. The signal is delivered on the way out of
 * kill(), so the interrupted eax holds kill's return value; overwriting
 * it in the ucontext means kill() appears to return something else. If
 * the kernel restores from a private copy of the trap frame instead of
 * from the ucontext the handler was given, this silently does nothing -
 * which is exactly how Wine's exception dispatch would silently do
 * nothing. */
#define WRITEBACK_EAX 0x5A5A5A5Aul
static void on_usr1_setreg(int signo, k_siginfo* si, k_ucontext* uc) {
    (void)signo; (void)si;
    uc->uc_mcontext.gregs[REG_EAX] = WRITEBACK_EAX;
}

static volatile int tkill_code = -1;
static void on_usr2_info(int signo, k_siginfo* si, k_ucontext* uc) {
    (void)signo; (void)uc;
    tkill_code = si->si_code;
}

/* Fault-signal state: the two si_codes, the faulting address, and the
 * machine registers the fault left behind. */
static volatile int    f_code = -1;
static volatile unsigned long f_addr = 0, f_cr2 = 0, f_trapno = 0, f_err = 0;
static volatile unsigned long f_eip = 0;
static volatile int    f_count = 0;

static void on_segv_accerr(int signo, k_siginfo* si, k_ucontext* uc) {
    (void)signo;
    f_count++;
    f_code   = si->si_code;
    f_addr   = (unsigned long)si->_sifields._sigfault.si_addr;
    f_cr2    = uc->uc_mcontext.cr2;
    f_trapno = uc->uc_mcontext.gregs[REG_TRAPNO];
    f_err    = uc->uc_mcontext.gregs[REG_ERR];
    f_eip    = uc->uc_mcontext.gregs[REG_EIP];
    sc3(SYS_mprotect, (long)guard_page, 4096, PROT_READ | PROT_WRITE);
}

static volatile unsigned long unmapped_page = 0;
static void on_segv_maperr(int signo, k_siginfo* si, k_ucontext* uc) {
    (void)signo; (void)uc;
    f_count++;
    f_code = si->si_code;
    f_addr = (unsigned long)si->_sifields._sigfault.si_addr;
    /* Map the page the faulting instruction wanted and return; the store
     * is retried and now lands. */
    sc6(SYS_mmap2, (long)unmapped_page, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}

/* The other half of the write-back direction, and the one Wine really
 * leans on: rather than fixing the fault, redirect execution somewhere
 * else entirely by rewriting eip in the ucontext. */
static volatile unsigned long resume_target = 0;
static void on_segv_unwind(int signo, k_siginfo* si, k_ucontext* uc) {
    (void)signo; (void)si;
    f_count++;
    uc->uc_mcontext.gregs[REG_EIP] = resume_target;
}

/* --- two probes with their landmarks nailed down in asm -----------------
 *
 * Both of the tests below need to know exactly where an instruction is,
 * and a C label is the wrong tool: with -O2 the compiler is free to place
 * one anywhere in the basic block, which is how the first version of test
 * 10 ended up redirecting eip into the middle of the enclosing block and
 * looping for ever. Assembler labels sitting directly beside the
 * instruction they name cannot drift. */

/* Milestone 24: a handler that does arithmetic. Before the FPU was saved
 * and reset around delivery, these three pushes came off the interrupted
 * thread's own x87 stack and its value never came back. */
static volatile int fp_handler_ran = 0, fp_handler_df = -1, fp_handler_top = -1;
static long double fp_handler_sum = 0;

static void on_usr1_fp(int signo, k_siginfo* si, k_ucontext* uc) {
    (void)signo; (void)si; (void)uc;
    unsigned short sw;
    unsigned long fl;
    __asm__ __volatile__("fnstsw %0" : "=m"(sw));
    __asm__ __volatile__("pushf; pop %0" : "=r"(fl));
    fp_handler_ran++;
    fp_handler_top = (sw >> 11) & 7;
    fp_handler_df  = (int)((fl >> 10) & 1);

    volatile double two = 2.0;
    long double acc;
    __asm__ __volatile__("fldl %1\n\t"
                         "fldl %1\n\t"
                         "fldl %1\n\t"
                         "faddp\n\t"
                         "faddp\n\t"
                         "fstpt %0"
                         : "=m"(acc) : "m"(two));
    fp_handler_sum = acc;
}

extern char kill_probe_ret[];

/* kill(pid, sig), with a label on the instruction the kernel should
 * resume at. Delivery happens on the way out of this `int $0x80`, so the
 * eip in the handler's ucontext must be exactly kill_probe_ret. */
__attribute__((noinline))
static long kill_probe(long pid, long sig) {
    long r;
    __asm__ __volatile__("int $0x80\n"
                         ".globl kill_probe_ret\n"
                         "kill_probe_ret:\n"
                         : "=a"(r)
                         : "a"(SYS_kill), "b"(pid), "c"(sig)
                         : "memory");
    return r;
}

extern char unwind_probe_land[];

/* Stores to *p, which faults, and returns 1 only if that store completed
 * and execution carried on into the next instruction. A handler that
 * points eip at unwind_probe_land instead skips the `movl $1` and gets 0
 * - the register holding the result still carries the zero the compiler
 * put there before the block. */
__attribute__((noinline))
static int unwind_probe(volatile unsigned long* p) {
    int fell = 0;
    __asm__ __volatile__("movl $0x33333333, (%1)\n\t"
                         "movl $1, %0\n"
                         ".globl unwind_probe_land\n"
                         "unwind_probe_land:\n"
                         : "+r"(fell)
                         : "r"(p)
                         : "memory");
    return fell;
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

    /* 5. Milestone 23. Until now the second and third arguments were a
     *    valid pointer to zeroes, which is enough for a handler that only
     *    reads its signal number and no use at all to Wine. */
    out("\n5. siginfo_t and ucontext_t are populated\n");
    check("rt_sigaction(SIGUSR1, 3-argument handler) accepted",
          install3(SIGUSR1, on_usr1_info) == 0);
    {
        /* Block SIGUSR2 first, so the saved mask in the ucontext has
         * something in it to be right about. */
        unsigned long m2[2] = { 1UL << (SIGUSR2 - 1), 0 };
        sc4(SYS_rt_sigprocmask, SIG_BLOCK, (long)m2, 0, 8);

        kill_probe(pid, SIGUSR1);
        sc4(SYS_rt_sigprocmask, SIG_UNBLOCK, (long)m2, 0, 8);

        check("si_signo is SIGUSR1", info_signo == SIGUSR1);
        check("si_errno is 0", info_errno == 0);
        check("si_code is SI_USER (the signal came from kill)",
              info_code == SI_USER);
        check("si_pid is the sending process", info_pid == (int)pid);

        check("the third argument is not null", !uc_was_null);
        check("uc_stack reports no alternate signal stack",
              uc_ss_flags == 0);
        check("uc_sigmask holds the mask from before delivery",
              uc_saved_mask == (1UL << (SIGUSR2 - 1)));

        /* No absolute address is printed: they legitimately differ
         * between the two systems. Every check below is a relation
         * between values this program already knows. */
        check("gregs[REG_EIP] is the instruction after the interrupted "
              "syscall", uc_eip == (unsigned long)kill_probe_ret);
        check("gregs[REG_ESP] and gregs[REG_UESP] agree",
              uc_esp == uc_uesp && uc_esp != 0);
        check("gregs[REG_ESP] is below the handler's own stack",
              uc_esp > (unsigned long)&witness - 65536 &&
              uc_esp < (unsigned long)&witness + 65536);
        check("gregs[REG_CS] is a ring-3 selector", (uc_cs & 3) == 3);
        check("gregs[REG_EFL] has IF set", (uc_efl & 0x200) != 0);
        check("gregs[REG_EBX] still holds the interrupted call's argument",
              uc_ebx == (unsigned long)pid);
    }

    /* 6. tgkill and kill are distinguishable, which is what si_code is
     *    for. */
    out("\n6. si_code tells kill from tgkill\n");
    {
        long tid = sc1(SYS_gettid, 0);
        check("rt_sigaction(SIGUSR2, 3-argument handler) accepted",
              install3(SIGUSR2, on_usr2_info) == 0);
        sc2(SYS_kill, pid, SIGUSR2);
        check("kill gives si_code == SI_USER", tkill_code == SI_USER);
        tkill_code = -1;
        sc3(SYS_tgkill, pid, tid, SIGUSR2);
        check("tgkill gives si_code == SI_TKILL", tkill_code == SI_TKILL);
    }

    /* 7. The other direction, and the one Wine's exception dispatch is
     *    built on: the handler *edits* the register state in the
     *    ucontext, and the edit takes effect when it returns. A kernel
     *    that restores from a private copy of the trap frame passes every
     *    test above and fails this one. */
    out("\n7. a handler's writes to the ucontext take effect\n");
    check("rt_sigaction(SIGUSR1, register-writing handler) accepted",
          install3(SIGUSR1, on_usr1_setreg) == 0);
    {
        /* The signal is delivered on the way out of this kill(), so the
         * interrupted eax is kill's own return value. */
        long r = sc2(SYS_kill, pid, SIGUSR1);
        check("gregs[REG_EAX] written in the handler is what kill returned",
              (unsigned long)r == WRITEBACK_EAX);
    }

    /* 8. A real fault, and what the handler learns about it. */
    out("\n8. siginfo and mcontext for a page fault\n");
    check("rt_sigaction(SIGSEGV, 3-argument handler) accepted",
          install3(SIGSEGV, on_segv_accerr) == 0);
    if (have) {
        volatile unsigned long* cell = (volatile unsigned long*)guard_page;
        f_count = 0;
        check("made the page read-only again",
              sc3(SYS_mprotect, (long)guard_page, 4096, PROT_READ) == 0);
        *cell = 0x11111111;   /* faults: mapped, but not writable */
        check("the handler ran once", f_count == 1);
        check("si_code is SEGV_ACCERR (mapped, access refused)",
              f_code == SEGV_ACCERR);
        check("si_addr is the address the store was aimed at",
              f_addr == guard_page);
        check("uc_mcontext.cr2 agrees with si_addr", f_cr2 == f_addr);
        check("gregs[REG_TRAPNO] is 14 (page fault)", f_trapno == 14);
        check("gregs[REG_ERR] says it was a write to a present page",
              (f_err & 2) != 0 && (f_err & 1) != 0);
        check("gregs[REG_ERR] says the fault came from user mode",
              (f_err & 4) != 0);
        check("gregs[REG_EIP] is the faulting instruction, not the handler",
              f_eip != 0 && f_eip != (unsigned long)on_segv_accerr);
        check("the store completed on retry", *cell == 0x11111111);
    }

    /* 9. The other si_code: nothing mapped there at all. The handler
     *    maps the page the instruction wanted and returns. */
    out("\n9. a fault on an address with no mapping\n");
    {
        long two = sc6(SYS_mmap2, 0, 8192, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int ok2 = !(two < 0 && two > -4096) && two != 0;
        check("mapped two pages", ok2);
        if (ok2) {
            unmapped_page = (unsigned long)two + 4096;
            check("unmapped the second of them",
                  sc3(SYS_munmap, (long)unmapped_page, 4096, 0) == 0);
            check("rt_sigaction(SIGSEGV, mapping handler) accepted",
                  install3(SIGSEGV, on_segv_maperr) == 0);

            f_count = 0;
            volatile unsigned long* gone = (volatile unsigned long*)unmapped_page;
            *gone = 0x22222222;   /* faults: nothing is mapped there */

            check("the handler ran once", f_count == 1);
            check("si_code is SEGV_MAPERR (nothing mapped there)",
                  f_code == SEGV_MAPERR);
            check("si_addr is the unmapped address", f_addr == unmapped_page);
            check("the store completed once the handler mapped the page",
                  *gone == 0x22222222);

            /* 10. And the strongest form of the write-back: rather than
             *     fixing anything, the handler sends execution somewhere
             *     else by rewriting eip. This is how Wine leaves a
             *     SIGSEGV for a Windows __except block. */
            out("\n10. a handler that redirects execution by rewriting eip\n");
            check("unmapped the page again",
                  sc3(SYS_munmap, (long)unmapped_page, 4096, 0) == 0);
            check("rt_sigaction(SIGSEGV, redirecting handler) accepted",
                  install3(SIGSEGV, on_segv_unwind) == 0);
            f_count = 0;
            resume_target = (unsigned long)unwind_probe_land;
            int fell_through = unwind_probe(gone);
            check("the handler ran once", f_count == 1);
            check("execution resumed where the handler pointed eip, "
                  "not at the faulting store", !fell_through);
            check("and the store really never happened",
                  sc3(SYS_mprotect, (long)unmapped_page, 4096,
                      PROT_READ | PROT_WRITE) < 0);
        }
    }

    /* 11. Milestone 24, without looking at any structure at all. The
     *     checks above go through Novaris's transcription of Linux's
     *     _fpstate; these only assert behaviour a program can feel, so
     *     they hold whatever the layout turns out to be. Both were real
     *     bugs until this milestone: a handler ran with whatever
     *     direction flag it was interrupted with, and its arithmetic came
     *     straight off the interrupted thread's x87 stack. */
    out("\n11. state a handler must not be able to damage\n");
    check("rt_sigaction(SIGUSR1, arithmetic handler) accepted",
          install3(SIGUSR1, on_usr1_fp) == 0);
    {
        volatile double one_and_a_half = 1.5;
        unsigned short sw_after;
        long double st0_after;
        unsigned long efl_after;

        __asm__ __volatile__("fninit");
        __asm__ __volatile__("fldl %0" :: "m"(one_and_a_half));
        __asm__ __volatile__("std");

        sc2(SYS_kill, pid, SIGUSR1);

        __asm__ __volatile__("pushf; pop %0" : "=r"(efl_after));
        __asm__ __volatile__("cld");
        __asm__ __volatile__("fnstsw %0" : "=m"(sw_after));
        __asm__ __volatile__("fstpt %0" : "=m"(st0_after));

        check("the handler ran", fp_handler_ran == 1);
        check("the handler was entered with the direction flag clear",
              fp_handler_df == 0);
        check("the handler was given an empty x87 stack",
              fp_handler_top == 0);
        check("the handler's own arithmetic gave the answer it should",
              fp_handler_sum == 6.0L);
        check("the interrupted x87 stack survived (TOP back to 7)",
              ((sw_after >> 11) & 7) == 7);
        check("and still holds the value pushed before the signal",
              st0_after == 1.5L);
        check("the direction flag came back set",
              (efl_after & 0x400) != 0);
    }

    out("\nsignal test done.\n");
    return 0;
}

void _start(void) {
    sc1(SYS_exit, main_());
    __builtin_unreachable();
}
