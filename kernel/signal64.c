/* signal64.c - delivering a fault to a ring-3 handler. */

#include "signal64.h"
#include "kstring.h"
#include "serial64.h"
#include "proc64.h"
#include "paging64.h"

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

static inline uint64_t read_msr_base(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static int signal_trace;
void signal64_set_trace(int on) { signal_trace = on; }

/* Is `sp` inside the alternate stack? A handler that faults again while
 * already running on the alternate stack must not be given a second
 * frame at the same place - Linux keeps delivering on the current stack
 * in that case, and so does this. */
/* The alternate stack lives on the process, not on this file: three
 * processes register one in a single prefix run, and a global would
 * deliver one process's signal onto another's stack. ss_size 0 means
 * none set. */
static int on_altstack(uint64_t sp) {
    proc64_t* p = proc64_current();
    return p && p->sas_size &&
           sp >= p->sas_sp && sp < p->sas_sp + p->sas_size;
}

int signal64_sigaltstack(const altstack64_t* ss, altstack64_t* oss,
                         uint64_t cur_rsp) {
    proc64_t* p = proc64_current();

    if (!p) return -22;                                /* -EINVAL */
    if (oss) {
        oss->ss_sp    = p->sas_sp;
        oss->ss_size  = p->sas_size;
        oss->ss_flags = on_altstack(cur_rsp) ? SS_ONSTACK
                      : (p->sas_size ? 0 : SS_DISABLE);
        oss->__pad    = 0;
    }
    if (ss) {
        /* Changing it while running on it is what Linux refuses, and
         * for the obvious reason: the frame under the caller's feet
         * would move. */
        if (on_altstack(cur_rsp)) return -16;          /* -EPERM/-EBUSY */
        if (ss->ss_flags & SS_DISABLE) {
            p->sas_sp = p->sas_size = 0;
        } else {
            if (ss->ss_size < 2048) return -12;        /* -ENOMEM */
            p->sas_sp   = ss->ss_sp;
            p->sas_size = ss->ss_size;
        }
    }
    return 0;
}

/* Is every page of [addr, addr+len) present? Asked before the kernel
 * writes a signal frame through a pointer the program chose.
 *
 * The comment this replaces said the write was unchecked and that a
 * thread whose stack pointer is the reason it faulted would fault again
 * here, in the kernel. It did:
 *
 *     *** page fault in the program
 *       cr2=0x7ffffe101d48 rip=0xffffffff80115e90 cs=0x08
 *     *** halted (the fault was in the kernel)
 *
 * cs=0x08 being ring 0 - the kernel pushing a frame onto a stack that
 * had just run out, on behalf of the fault that said so. */
static int range_present(uint64_t addr, uint64_t len) {
    uint64_t p, phys;
    for (p = addr & ~0xFFFULL; p < addr + len; p += 0x1000)
        if (paging64_translate(p, &phys) != 0) return 0;
    return 1;
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
    /* On the alternate stack when the handler asked for one and we are
     * not already running on it; otherwise below the caller's red zone.
     *
     * The red zone is the ABI letting a leaf function use the 128 bytes
     * under rsp without adjusting it, so a frame written there would
     * corrupt the interrupted function's locals. The alternate stack
     * has no such caller to protect, so the frame starts at its top. */
    {
        proc64_t* me = proc64_current();
        if ((sa->flags & SA_ONSTACK) && me && me->sas_size &&
            !on_altstack(r->rsp))
            sp = me->sas_sp + me->sas_size;
        else
            sp = r->rsp - 128;
    }
    sp -= sizeof(rt_sigframe64_t);

    /* Aligned the way a `call` leaves the stack, not the way a 16-byte
     * boundary looks.
     *
     * The handler is entered with rsp pointing at the frame, whose first
     * field is pretcode - so from the handler's point of view rsp holds
     * a return address and it is entered exactly as if called. The ABI
     * says rsp+8 is 16-byte aligned there, which means rsp % 16 == 8.
     * Rounding to 16 and stopping gets the parity wrong by eight, and
     * Linux writes the same thing as round_down(sp, 16) - 8.
     *
     * Eight bytes of parity is not a detail the compiler forgives. Every
     * xmm spill in the handler is a movaps against a fixed offset from
     * rsp, and movaps faults on an address that is merely 8-aligned. It
     * killed Wine's own SIGSEGV handler, which is not an edge case here:
     * Wine takes SIGSEGV in the ordinary course of running a prefix, to
     * service its write-watch pages through virtual_handle_fault.
     *
     *     418b9:  movaps %xmm0,0x40(%rsp)   <- #GP, rsp % 16 == 8
     *     418be:  call   virtual_handle_fault
     *
     * so with rsp % 16 == 8 inside the body, 0x40(%rsp) is 8-aligned and
     * the store is a general protection fault in ring 3 with no error
     * code, a long way from anything about signals. */
    sp = (sp & ~15ULL) - 8;
    /* The frame is written from the kernel while the faulting thread's
     * address space is current - a fault handler runs in the space that
     * faulted - so this is an ordinary store. It is also unchecked: a
     * thread whose stack pointer is the reason it faulted will fault
     * again here, in the kernel. See ROADMAP.md. */
    /* Refused rather than written blind. Returning 0 hands the caller
     * back the fault it was trying to deliver, which reports and halts
     * the machine - the same outcome, minus a kernel-mode page fault
     * on the way to it, and with the original address still in cr2. */
    if (!range_present(sp, sizeof(rt_sigframe64_t))) return 0;

    frame = (rt_sigframe64_t*)sp;

    /* Said out loud, for the same reason the pid is on the trace line.
     * A signal delivered to a program that handles it leaves no trace
     * at all otherwise: Wine installs a SIGSEGV handler and services
     * its own write-watch pages through it, so a fault that reaches
     * ring 3 and is dealt with there is invisible, and a fault that
     * reaches ring 3 and kills it looks like the handler crashed for
     * no reason. What is wanted is the *original* fault - where it was
     * and what it touched - not where the handler died. */
    if (signal_trace) {
        serial64_puts("NOVARIS64: [signal ");
        serial64_putdec((uint64_t)sig);
        serial64_puts(" pid ");
        serial64_putdec((uint64_t)proc64_current_pid());
        serial64_puts("] at rip=");
        serial64_puthex(r->rip);
        serial64_puts(" rsp=");
        serial64_puthex(r->rsp);
        serial64_puts(" addr=");
        serial64_puthex(fault_addr);
        serial64_puts(" err=");
        serial64_puthex(r->err_code);
        /* The segment bases, because the address a Windows program
         * faults on is very often reached through one of them. Wine
         * keeps the TEB at GS and reads its own syscall frame out of
         * %gs:0x378, so a GS base that belongs to another thread - or
         * to nobody - turns into a near-null dereference at whatever
         * offset the frame was being read at, and says nothing about
         * segments at all. */
        serial64_puts("\nNOVARIS64:   fsbase=");
        serial64_puthex(read_msr_base(0xC0000100u));
        serial64_puts(" gsbase=");
        serial64_puthex(read_msr_base(0xC0000101u));
        serial64_putc('\n');
    }

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
