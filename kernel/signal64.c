/* signal64.c - delivering a fault to a ring-3 handler. */

#include "signal64.h"
#include "kstring.h"

#define NSIG 64

/* The offsets Linux's ABI fixes. A handler that rewrites RIP - which is
 * the only reason Wine wants signals at all - indexes uc_mcontext by
 * these, so getting one wrong means a handler that appears to work and
 * resumes somewhere arbitrary. */
_Static_assert(__builtin_offsetof(ucontext64_t, uc_mcontext) == 40,
               "uc_mcontext must be at offset 40 in ucontext");
_Static_assert(__builtin_offsetof(sigcontext64_t, rsp) == 120, "sc.rsp");
_Static_assert(__builtin_offsetof(sigcontext64_t, rip) == 128, "sc.rip");
_Static_assert(__builtin_offsetof(sigcontext64_t, eflags) == 136, "sc.eflags");
_Static_assert(__builtin_offsetof(rt_sigframe64_t, uc) == 8, "frame.uc");

static ksigaction64_t handlers[NSIG];
static uint64_t delivered, returns;

void signal64_reset(void) {
    for (int i = 0; i < NSIG; i++) {
        handlers[i].handler  = 0;
        handlers[i].flags    = 0;
        handlers[i].restorer = 0;
        handlers[i].mask     = 0;
    }
    delivered = 0;
    returns = 0;
}

uint64_t signal64_delivered(void) { return delivered; }
uint64_t signal64_returns(void)   { return returns; }

int signal64_sigaction(int sig, const ksigaction64_t* act,
                       ksigaction64_t* oact) {
    if (sig <= 0 || sig >= NSIG) return -22;          /* -EINVAL */

    if (oact) *oact = handlers[sig];

    if (act) {
        /* SA_RESTORER is not optional on x86-64: the kernel does not
         * supply a return trampoline, so a handler installed without one
         * would return to whatever `pretcode` happened to be. Linux
         * rejects it and so does this. */
        if (act->handler && !(act->flags & SA_RESTORER)) return -22;
        handlers[sig] = *act;
    }
    return 0;
}

int signal64_deliver(int sig, registers64_t* r, uint64_t fault_addr) {
    const ksigaction64_t* sa;
    rt_sigframe64_t* frame;
    uint64_t sp;

    if (sig <= 0 || sig >= NSIG) return 0;
    sa = &handlers[sig];
    if (!sa->handler) return 0;

    /* Only a ring-3 fault can be handed to a ring-3 handler. A fault in
     * the kernel with cs = 0x08 is a kernel bug, and pushing a frame on
     * whatever the kernel was using as a stack would bury it. */
    if ((r->cs & 3) != 3) return 0;

    /* Below the red zone: the ABI lets a leaf function use the 128
     * bytes under rsp without adjusting it, so a signal frame written
     * there would corrupt the interrupted function's locals. */
    sp = r->rsp - 128;
    sp -= sizeof(rt_sigframe64_t);
    sp &= ~15ULL;
    /* The frame is written from the kernel while the faulting thread's
     * address space is current - a fault handler runs in the space that
     * faulted - so this is an ordinary store. It is also unchecked: a
     * thread whose stack pointer is the reason it faulted will fault
     * again here, in the kernel. See ROADMAP.md. */
    frame = (rt_sigframe64_t*)sp;

    kmemset(frame, 0, sizeof(*frame));

    frame->pretcode = sa->restorer;

    frame->uc.uc_mcontext.r8     = r->r8;
    frame->uc.uc_mcontext.r9     = r->r9;
    frame->uc.uc_mcontext.r10    = r->r10;
    frame->uc.uc_mcontext.r11    = r->r11;
    frame->uc.uc_mcontext.r12    = r->r12;
    frame->uc.uc_mcontext.r13    = r->r13;
    frame->uc.uc_mcontext.r14    = r->r14;
    frame->uc.uc_mcontext.r15    = r->r15;
    frame->uc.uc_mcontext.rdi    = r->rdi;
    frame->uc.uc_mcontext.rsi    = r->rsi;
    frame->uc.uc_mcontext.rbp    = r->rbp;
    frame->uc.uc_mcontext.rbx    = r->rbx;
    frame->uc.uc_mcontext.rdx    = r->rdx;
    frame->uc.uc_mcontext.rax    = r->rax;
    frame->uc.uc_mcontext.rcx    = r->rcx;
    frame->uc.uc_mcontext.rsp    = r->rsp;
    frame->uc.uc_mcontext.rip    = r->rip;
    frame->uc.uc_mcontext.eflags = r->rflags;
    frame->uc.uc_mcontext.cs     = (uint16_t)r->cs;
    frame->uc.uc_mcontext.ss     = (uint16_t)r->ss;
    frame->uc.uc_mcontext.err    = r->err_code;
    frame->uc.uc_mcontext.trapno = r->int_no;
    frame->uc.uc_mcontext.cr2    = fault_addr;

    /* siginfo: si_signo, si_errno, si_code, then si_addr for a fault.
     * Written as words rather than through a struct because only these
     * four fields are filled and the rest of siginfo_t is a union. */
    ((uint32_t*)frame->info)[0] = (uint32_t)sig;      /* si_signo */
    ((uint32_t*)frame->info)[1] = 0;                  /* si_errno */
    ((uint32_t*)frame->info)[2] = 1;                  /* si_code = SEGV_MAPERR */
    frame->info[2]              = fault_addr;         /* si_addr   */

    /* Enter the handler. The System V arguments are (signo, siginfo*,
     * ucontext*) - the third is what makes resuming elsewhere possible. */
    r->rip = sa->handler;
    r->rsp = sp;
    r->rdi = (uint64_t)sig;
    r->rsi = (uint64_t)frame->info;
    r->rdx = (uint64_t)&frame->uc;
    /* Direction flag clear on entry to a handler, as the ABI requires,
     * and traps off so a single-stepping debugger does not fire inside
     * the handler's first instruction. */
    r->rflags &= ~((uint64_t)(1 << 10) | (uint64_t)(1 << 8));

    delivered++;
    return 1;
}

int signal64_sigreturn(uint64_t user_rsp, registers64_t* out) {
    /* Linux computes the frame as sp - 8, because the handler's `ret`
     * popped pretcode before the restorer ran. So the ucontext starts
     * exactly at the current rsp. */
    const ucontext64_t* uc = (const ucontext64_t*)user_rsp;
    const sigcontext64_t* sc = &uc->uc_mcontext;

    out->r8  = sc->r8;
    out->r9  = sc->r9;
    out->r10 = sc->r10;
    out->r11 = sc->r11;
    out->r12 = sc->r12;
    out->r13 = sc->r13;
    out->r14 = sc->r14;
    out->r15 = sc->r15;
    out->rdi = sc->rdi;
    out->rsi = sc->rsi;
    out->rbp = sc->rbp;
    out->rbx = sc->rbx;
    out->rdx = sc->rdx;
    out->rax = sc->rax;
    out->rcx = sc->rcx;
    out->rsp = sc->rsp;
    out->rip = sc->rip;

    out->int_no   = 0;
    out->err_code = 0;

    /* The selectors and the interrupt flag are the kernel's to decide,
     * not the handler's: a process that could write its own cs, or turn
     * interrupts off, would be writing itself into ring 0. Only the
     * flags a program may legitimately change are taken from the frame. */
    out->cs     = 0x23;
    out->ss     = 0x1B;
    out->rflags = (sc->eflags & 0x0CD5ULL) | 0x202ULL;

    returns++;
    return 1;
}
