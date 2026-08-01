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

    printf("\nucontext test done.\n");
    return 0;
}
