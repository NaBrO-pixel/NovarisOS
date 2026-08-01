/* ucontext_test.c - the same claim as signal_test.c's tests 5-10, made
 * through the real headers instead of Novaris's idea of them.
 *
 * signal_test.c writes out the Linux/i386 siginfo_t and ucontext_t by
 * hand, because it links against nothing. That proves Novaris matches
 * what *this project believes* Linux's layout to be, which is a weaker
 * claim than it looks: if the belief were wrong in both places, the test
 * would still pass.
 *
 * This one is an ordinary dynamically linked glibc program. `siginfo_t`,
 * `ucontext_t`, `REG_EIP` and the rest come from /usr/include, so the
 * offsets are whatever glibc says they are, and the program runs through
 * the real ld-linux.so.2 and libc.so.6 on the way. That is also, almost
 * exactly, how Wine reads a faulting context:
 *
 *     ucontext_t *context = sigcontext;
 *     context->uc_mcontext.gregs[REG_EIP] = ...;
 *
 * Nothing here prints an address, a selector or a pid: those legitimately
 * differ between the host and Novaris, and a transcript comparison that
 * demanded they match would be testing the wrong thing. Every line is a
 * relation between values the program already knows.
 *
 * See ROADMAP.md Milestone 23.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>

static void check(const char* label, int ok) {
    printf("  [%s]   %s\n", ok ? "ok" : "FAIL", label);
    fflush(stdout);
}

/* --- what the handlers see ----------------------------------------------- */

static volatile int   h_ran = 0;
static volatile int   h_signo = -1, h_errno = -1, h_code = -1;
static volatile void* h_addr = 0;
static volatile int   h_uc_null = 1;
static volatile unsigned long h_trapno = 0, h_err = 0, h_cr2 = 0, h_eip = 0;
static volatile unsigned long h_esp = 0, h_uesp = 0, h_efl = 0;

static void* fault_addr = 0;

/* The address the redirecting handler sends execution to, and the probe
 * that faults. The landmark is an assembler label rather than a C one:
 * with -O2 the compiler places a C label anywhere in the basic block, and
 * "anywhere" turned out to include a point before the enclosing loop. */
extern char uc_probe_land[];

__attribute__((noinline))
static int fault_probe(volatile unsigned long* p) {
    int fell = 0;
    __asm__ __volatile__("movl $0x44444444, (%1)\n\t"
                         "movl $1, %0\n"
                         ".globl uc_probe_land\n"
                         "uc_probe_land:\n"
                         : "+r"(fell)
                         : "r"(p)
                         : "memory");
    return fell;
}

static void on_segv(int signo, siginfo_t* si, void* ctx) {
    ucontext_t* uc = ctx;
    h_ran++;
    h_signo = si->si_signo;
    h_errno = si->si_errno;
    h_code  = si->si_code;
    h_addr  = si->si_addr;
    h_uc_null = (uc == 0);
    if (!uc) return;
    h_trapno = (unsigned long)uc->uc_mcontext.gregs[REG_TRAPNO];
    h_err    = (unsigned long)uc->uc_mcontext.gregs[REG_ERR];
    h_eip    = (unsigned long)uc->uc_mcontext.gregs[REG_EIP];
    h_esp    = (unsigned long)uc->uc_mcontext.gregs[REG_ESP];
    h_uesp   = (unsigned long)uc->uc_mcontext.gregs[REG_UESP];
    h_efl    = (unsigned long)uc->uc_mcontext.gregs[REG_EFL];
    h_cr2    = (unsigned long)uc->uc_mcontext.cr2;
    /* Wine's move: do not fix the fault, send execution somewhere else. */
    uc->uc_mcontext.gregs[REG_EIP] = (greg_t)(unsigned long)uc_probe_land;
}

static volatile int u_code = -1, u_ran = 0;
static void on_usr(int signo, siginfo_t* si, void* ctx) {
    (void)signo; (void)ctx;
    u_ran++;
    u_code = si->si_code;
}

/* --- Milestone 24: the x87/SSE state behind sigcontext.fpstate --------- */

static volatile int fp_ran = 0, fp_null = 1;
static volatile unsigned fp_cw, fp_sw, fp_tag, fp_mxcsr, fp_xmm3;
static volatile unsigned fp_magic, fp_status_hi;
static volatile unsigned fp_h_cw, fp_h_top, fp_h_xmm3, fp_h_mxcsr, fp_h_df;
static long double fp_st0, fp_st1, fp_st2;

static void on_fp(int signo, siginfo_t* si, void* ctx) {
    (void)signo; (void)si;
    ucontext_t* uc = ctx;
    struct _fpstate* f = (struct _fpstate*)uc->uc_mcontext.fpregs;

    fp_ran++;
    fp_null = (f == 0);
    if (!f) return;

    /* The legacy half - which is byte for byte Windows' FLOATING_SAVE_AREA,
     * and what Wine reads. */
    fp_cw  = (unsigned)f->cw;
    fp_sw  = (unsigned)f->sw;
    fp_tag = (unsigned)f->tag;
    fp_magic = f->magic;
    /* Wine finds the FXSAVE image by testing `status >> 16`, reading the
     * adjacent status and magic fields as one 32-bit word. Copied rather
     * than type-punned in place, which is the same bytes without the
     * strict-aliasing warning. */
    unsigned status_word;
    memcpy(&status_word, &f->status, 4);
    fp_status_hi = status_word >> 16;
    memcpy((void*)&fp_st0, &f->_st[0], 10);
    memcpy((void*)&fp_st1, &f->_st[1], 10);
    memcpy((void*)&fp_st2, &f->_st[2], 10);

    /* And the FXSAVE image, which is everything from offset 112 on:
     * MXCSR at 24 into it, XMM(n) at 160 + 16n. */
    const unsigned char* img = (const unsigned char*)f + 112;
    fp_mxcsr = *(const unsigned*)(img + 24);
    fp_xmm3  = *(const unsigned*)(img + 160 + 3 * 16);

    /* What the handler's *own* FPU looks like: it should be a clean unit,
     * not the interrupted thread's registers. */
    unsigned short hcw, hsw;
    unsigned hmx, hx3, hfl;
    __asm__ __volatile__("fnstcw %0" : "=m"(hcw));
    __asm__ __volatile__("fnstsw %0" : "=m"(hsw));
    __asm__ __volatile__("stmxcsr %0" : "=m"(hmx));
    __asm__ __volatile__("movd %%xmm3, %0" : "=r"(hx3));
    __asm__ __volatile__("pushf; pop %0" : "=r"(hfl));
    fp_h_cw = hcw;
    fp_h_top = (hsw >> 11) & 7;
    fp_h_mxcsr = hmx;
    fp_h_xmm3 = hx3;
    fp_h_df = (hfl >> 10) & 1;

    /* Do some arithmetic, to prove it cannot reach the interrupted state.
     * Before Milestone 24 these three pushes came straight off the
     * program's own x87 stack. */
    volatile double junk = 9.0;
    __asm__ __volatile__("fldl %0; fldl %0; fldl %0" :: "m"(junk));
    __asm__ __volatile__("movd %0, %%xmm3" :: "r"(0xDEADBEEFu));
}

/* The write-back direction for FP state. Wine's set_context writes
 * CONTEXT.FloatSave into the legacy half and CONTEXT.ExtendedRegisters
 * into the FXSAVE image, so both have to be honoured on return - and they
 * are honoured differently, the legacy half winning for x87 and the image
 * for XMM and MXCSR. Editing one of each proves both paths. */
#define FP_NEW_ST0  7.75L
#define FP_NEW_XMM3 0x0BADF00Du

static void on_fp_write(int signo, siginfo_t* si, void* ctx) {
    (void)signo; (void)si;
    ucontext_t* uc = ctx;
    struct _fpstate* f = (struct _fpstate*)uc->uc_mcontext.fpregs;
    fp_ran++;
    if (!f) return;
    long double v = FP_NEW_ST0;
    memcpy(&f->_st[0], &v, 10);                 /* the legacy half */
    unsigned char* img = (unsigned char*)f + 112;
    *(unsigned*)(img + 160 + 3 * 16) = FP_NEW_XMM3;   /* the FXSAVE image */
}

static int install(int signo, void (*h)(int, siginfo_t*, void*)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = h;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    return sigaction(signo, &sa, 0);
}

int main(void) {
    printf("ucontext test - real glibc headers, real siginfo_t/ucontext_t\n\n");

    /* 1. si_code through glibc's own wrappers. kill() is SI_USER; raise()
     *    goes through tgkill on i386, which is SI_TKILL. That the two
     *    differ at all is the point - it means si_code carries
     *    information rather than a constant. */
    printf("1. si_code as glibc's wrappers produce it\n");
    check("sigaction(SIGUSR1, SA_SIGINFO) accepted",
          install(SIGUSR1, on_usr) == 0);
    kill(getpid(), SIGUSR1);
    check("the handler ran", u_ran == 1);
    check("kill() gives si_code == SI_USER", u_code == SI_USER);
    u_code = -1;
    raise(SIGUSR1);
    check("raise() gives si_code == SI_TKILL", u_code == SI_TKILL);

    /* 2. A real page fault, read back through glibc's ucontext_t. */
    printf("\n2. a page fault, read through glibc's ucontext_t\n");
    check("sigaction(SIGSEGV, SA_SIGINFO) accepted",
          install(SIGSEGV, on_segv) == 0);

    void* two = mmap(0, 8192, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mapped two pages", two != MAP_FAILED);
    if (two == MAP_FAILED) return 1;

    fault_addr = (char*)two + 4096;
    check("unmapped the second of them", munmap(fault_addr, 4096) == 0);

    int fell_through = fault_probe((volatile unsigned long*)fault_addr);

    check("the handler ran exactly once", h_ran == 1);
    check("si_signo is SIGSEGV", h_signo == SIGSEGV);
    check("si_errno is 0", h_errno == 0);
    check("si_code is SEGV_MAPERR (nothing mapped there)",
          h_code == SEGV_MAPERR);
    check("si_addr is the address the store was aimed at",
          h_addr == fault_addr);
    check("the third argument is not null", !h_uc_null);
    check("gregs[REG_TRAPNO] is 14 (page fault)", h_trapno == 14);
    check("gregs[REG_ERR] says write, not-present, from user mode",
          (h_err & 2) != 0 && (h_err & 1) == 0 && (h_err & 4) != 0);
    check("uc_mcontext.cr2 agrees with si_addr",
          h_cr2 == (unsigned long)fault_addr);
    check("gregs[REG_EIP] is inside the faulting probe",
          h_eip != 0 && h_eip < (unsigned long)uc_probe_land &&
          h_eip > (unsigned long)uc_probe_land - 64);
    check("gregs[REG_ESP] and gregs[REG_UESP] agree",
          h_esp == h_uesp && h_esp != 0);
    check("gregs[REG_EFL] has IF set", (h_efl & 0x200) != 0);

    /* 3. And the write-back: the handler rewrote REG_EIP rather than
     *    fixing the mapping, so the store must never have completed. */
    printf("\n3. the handler's write to gregs[REG_EIP] took effect\n");
    check("execution resumed where the handler pointed eip", !fell_through);
    check("the faulting store never happened",
          mprotect(fault_addr, 4096, PROT_READ | PROT_WRITE) != 0);

    /* 4. Milestone 24. sigcontext.fpstate used to be null, which meant a
     *    handler could not see the interrupted x87/SSE state and - worse,
     *    and not obvious - could silently destroy it just by doing
     *    arithmetic. Wine casts this pointer straight to a Windows
     *    FLOATING_SAVE_AREA, and finds the FXSAVE image behind it by
     *    testing `status >> 16`, which is the `magic` field. */
    printf("\n4. the x87/SSE state in sigcontext.fpstate\n");
    check("sigaction(SIGUSR2, SA_SIGINFO) accepted",
          install(SIGUSR2, on_fp) == 0);
    {
        /* A known starting point, so the tag word is a fixed value rather
         * than whatever the C library left behind. Three pushes onto an
         * empty stack puts TOP at 5 and marks physical registers 5, 6 and
         * 7 in use. */
        volatile double a = 1.5, b = 2.25, c = 3.125;
        unsigned mxcsr_set = 0x1F80 | 0x8000;   /* flush-to-zero on */
        unsigned xmm_seed = 0xCAFEBABE;

        __asm__ __volatile__("fninit");
        __asm__ __volatile__("fldl %0" :: "m"(a));
        __asm__ __volatile__("fldl %0" :: "m"(b));
        __asm__ __volatile__("fldl %0" :: "m"(c));
        __asm__ __volatile__("movd %0, %%xmm3" :: "r"(xmm_seed));
        __asm__ __volatile__("ldmxcsr %0" :: "m"(mxcsr_set));
        __asm__ __volatile__("std");            /* and a set direction flag */

        raise(SIGUSR2);

        unsigned efl_after;
        __asm__ __volatile__("pushf; pop %0" : "=r"(efl_after));
        __asm__ __volatile__("cld");

        /* What survived: the x87 stack, XMM3, MXCSR and DF. */
        unsigned short sw_after;
        unsigned mxcsr_after, xmm3_after;
        long double st0_after;
        __asm__ __volatile__("fnstsw %0" : "=m"(sw_after));
        __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr_after));
        __asm__ __volatile__("movd %%xmm3, %0" : "=r"(xmm3_after));
        __asm__ __volatile__("fstpt %0" : "=m"(st0_after));

        check("the handler ran", fp_ran == 1);
        check("sigcontext.fpstate is not null", !fp_null);
        check("magic says the FXSAVE image is present (Wine's own test)",
              fp_magic == 0 && (fp_status_hi == 0));
        check("the control word is the x87 default, widened to 32 bits",
              fp_cw == 0xFFFF037Fu);
        check("the status word's TOP is 5, as three pushes leave it",
              ((fp_sw >> 11) & 7) == 5 && (fp_sw >> 16) == 0xFFFF);
        /* 0x03FF is only reachable if the abridged FXSAVE tag word was
         * re-indexed through TOP on the way to the legacy one. Without
         * that rotation this reads 0x57FF. */
        check("the tag word marks three registers valid (0x03FF)",
              fp_tag == 0xFFFF03FFu);
        check("st[0], st[1], st[2] are the three values pushed",
              fp_st0 == 3.125L && fp_st1 == 2.25L && fp_st2 == 1.5L);
        check("the FXSAVE image carries MXCSR as the program set it",
              fp_mxcsr == mxcsr_set);
        check("the FXSAVE image carries XMM3 as the program set it",
              fp_xmm3 == xmm_seed);

        printf("\n5. the handler runs on a clean unit, and gets the old one"
               " back\n");
        check("the handler's own x87 was reset (control word 0x037f)",
              fp_h_cw == 0x037F);
        check("the handler's own x87 stack was empty (TOP 0)",
              fp_h_top == 0);
        check("the handler's own XMM3 was zero", fp_h_xmm3 == 0);
        check("the handler's own MXCSR was the default 0x1f80",
              fp_h_mxcsr == 0x1F80);
        check("the handler ran with the direction flag clear", fp_h_df == 0);

        /* The interrupted state comes back. Until this milestone the
         * handler's own arithmetic simply overwrote it. */
        check("the interrupted x87 stack came back (TOP 5)",
              ((sw_after >> 11) & 7) == 5);
        check("st(0) is still the value pushed last", st0_after == 3.125L);
        check("XMM3 came back", xmm3_after == xmm_seed);
        check("MXCSR came back", mxcsr_after == mxcsr_set);
        check("the direction flag came back set", (efl_after & 0x400) != 0);
    }

    /* 6. And the FP write-back, which is the direction Wine needs when it
     *    resumes a thread with a CONTEXT it has edited. */
    printf("\n6. a handler's writes to the FP state take effect\n");
    check("sigaction(SIGUSR2, FP-writing handler) accepted",
          install(SIGUSR2, on_fp_write) == 0);
    {
        volatile double a = 1.5;
        unsigned xmm_seed = 0x11112222;
        fp_ran = 0;

        __asm__ __volatile__("fninit");
        __asm__ __volatile__("fldl %0" :: "m"(a));
        __asm__ __volatile__("movd %0, %%xmm3" :: "r"(xmm_seed));

        raise(SIGUSR2);

        long double st0_after;
        unsigned xmm3_after;
        __asm__ __volatile__("fstpt %0" : "=m"(st0_after));
        __asm__ __volatile__("movd %%xmm3, %0" : "=r"(xmm3_after));

        check("the handler ran", fp_ran == 1);
        check("st(0) is what the handler wrote into the legacy half",
              st0_after == FP_NEW_ST0);
        check("XMM3 is what the handler wrote into the FXSAVE image",
              xmm3_after == FP_NEW_XMM3);
    }

    printf("\nucontext test done.\n");
    return 0;
}
