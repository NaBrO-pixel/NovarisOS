/* posix_signal.c - signal delivery.
 *
 * Milestone 19. See include/posix.h for the numbers and structures.
 *
 * The mechanism, which is simpler than it sounds once the right moment is
 * identified: a signal can only be delivered on the way back to ring 3,
 * because the trap frame the kernel is about to `iret` from *is* the
 * thread's user-mode state. Delivering means
 *
 *   1. copying that whole frame onto the user stack, below the current
 *      esp, along with the saved signal mask - this is the "sigframe";
 *   2. pushing the signal number as the handler's argument and, above it,
 *      the program's own restorer trampoline as the return address;
 *   3. rewriting eip/esp in the trap frame to point at the handler and
 *      the frame just built;
 *   4. iret'ing, which now enters the handler instead of resuming what
 *      was interrupted.
 *
 * When the handler returns it lands on the restorer, which issues
 * rt_sigreturn, and the kernel copies the saved frame back over the trap
 * frame. The interrupted instruction resumes as though nothing happened.
 *
 * That last property is the whole reason Wine needs this: it installs a
 * SIGSEGV handler, lets a page fault happen on purpose, fixes the mapping
 * inside the handler, and returns - and the faulting instruction is
 * retried and now succeeds.
 *
 * The restorer is the program's when it supplies one (SA_RESTORER, which
 * is what glibc does) and the kernel's otherwise, planted in the signal
 * frame itself. Linux accepts both, so Novaris accepts both - being
 * stricter would have been defensible in isolation and would have made
 * the same binary behave differently on the two systems, which is the one
 * thing this ABI must not do.
 *
 * One i386 wrinkle worth recording: SA_SIGINFO is not merely a richer
 * handler signature, it selects which *frame* and which *return* syscall
 * are in play. Without it Linux builds the old sigframe and expects
 * sigreturn (119); with it, the rt_ frame and rt_sigreturn (173) go
 * together. Novaris implements the rt_ path, which is the one glibc and
 * Wine use.
 *
 * Milestone 23 fills in the two arguments an SA_SIGINFO handler gets
 * besides the signal number: a real siginfo_t and a real ucontext_t, in
 * Linux/i386's layout (see include/posix.h). Both directions matter. Wine
 * reads the faulting registers out of the ucontext to build a Windows
 * CONTEXT record, and then - having decided what the exception handler
 * chain wants - writes the modified registers *back* into the same
 * ucontext and returns, expecting the kernel to resume from those. So
 * rt_sigreturn no longer restores from a private copy of the trap frame;
 * it restores from uc_mcontext, which is the memory the handler was
 * invited to edit. */

#include "posix.h"
#include "posix_proc.h"
#include "console.h"
#include "fpu.h"
#include "kstring.h"
#include "process.h"

/* --- per-process signal state -------------------------------------------
 *
 * Every one of these was a file-scope global until Milestone 29, which
 * was correct while there was only ever one program running and wrong the
 * moment fork existed: a child that installs a SIGSEGV handler must not
 * install its parent's. They now live in the process structure keyed by
 * address space (include/posix_proc.h), so threads share them - which is
 * what POSIX says - and processes do not.
 *
 * The accessors below are what keeps the rest of this file looking the
 * way it did: `S->blocked` rather than `blocked`, and nothing else moved.
 *
 * `last_trapno`/`last_err`/`last_cr2` are the last CPU fault this process
 * took. Linux keeps them in the thread struct and copies them into every
 * sigcontext it builds, fault or not, so a signal delivered by kill()
 * carries whatever the last real fault left behind. Matching that is free
 * and avoids the same binary seeing different numbers on the two systems -
 * even meaningless ones. */

#define SIGBIT(n) (1u << ((n) - 1))

/* SIGKILL and SIGSTOP can never be caught or blocked. */
#define UNMASKABLE (SIGBIT(SIGKILL))

void posix_signal_reset(void) {
    posix_proc_t* S = posix_current();
    for (int i = 0; i < NSIG; i++) {
        S->actions[i].handler = SIG_DFL;
        S->actions[i].flags = 0;
        S->actions[i].restorer = 0;
        S->actions[i].mask[0] = 0;
        S->actions[i].mask[1] = 0;
    }
    for (int i = 0; i < NSIG; i++) {
        S->info_of[i].code = SI_USER;
        S->info_of[i].addr = 0;
        S->info_of[i].pid  = 0;
    }
    S->blocked = 0;
    S->pending = 0;
    S->any_handler = 0;
    S->in_delivery = 0;
    S->last_trapno = S->last_err = S->last_cr2 = 0;
}

/* --- the sigframe -------------------------------------------------------
 *
 * The *ordering* is Novaris's, not Linux's, and that stays fine: the
 * program never looks at the frame as a whole, only at the two pointers
 * it is handed. What is no longer Novaris's own is the shape of the two
 * things those pointers point at - `info` and `uc` are the real
 * Linux/i386 siginfo_t and ucontext_t, because a handler compiled against
 * glibc's headers reads them by offset.
 *
 * The interrupted register state is now kept only in uc.uc_mcontext -
 * there is deliberately no second private copy. A handler is allowed to
 * edit uc_mcontext, and rt_sigreturn has to honour the edit; keeping a
 * shadow copy would silently discard it, which is exactly the bug that
 * would make Wine's exception dispatch a no-op. */
typedef struct {
    /* First, because FXSAVE needs 16-byte alignment and this member's
     * image half sits at offset 112 within it - a multiple of 16 only if
     * the struct itself starts on one. The frame address is aligned down
     * to 16 in deliver() to guarantee that. */
    k_fpstate_t  fpstate;
    uint32_t     magic;
    k_siginfo_t  info;
    k_ucontext_t uc;
    /* The trampoline the handler returns through when the program did not
     * supply one. Linux plants its own the same way, and it works for the
     * same reason: 32-bit x86 without PAE has no NX bit, so the user
     * stack is executable. */
    uint8_t      trampoline[8];
} sigframe_t;

#define SIGFRAME_MAGIC 0x5347464Du /* "SGFM" */

/* --- x87 state, in two shapes at once (Milestone 24) --------------------
 *
 * FXSAVE's tag word is one bit per register ("in use" / "empty"); the
 * legacy i387 one is two bits ("valid" / "zero" / "special" / "empty"),
 * and reconstructing it means looking at each register the CPU says is in
 * use and classifying its value. Linux does exactly this, and a program
 * that reads the tag word - Wine copies it into CONTEXT.FloatSave.TagWord
 * - would otherwise see something the x87 never produces.
 *
 * Offsets into the FXSAVE image are architectural: FCW at 0, FSW at 2,
 * the abridged tag byte at 4, FIP at 8, FDP at 16, and ST(i) in 16-byte
 * slots from 32 with only the first 10 bytes meaningful. */
#define FX_CW   0
#define FX_SW   2
#define FX_TW   4
#define FX_OP   6
#define FX_IP   8
#define FX_CS  12
#define FX_DP  16
#define FX_DS  20
#define FX_ST(i) (32 + (i) * 16)

static uint32_t tag_fxsr_to_i387(const uint8_t* fx) {
    uint32_t abridged = fx[FX_TW];
    uint32_t sw  = (uint32_t)(fx[FX_SW] | (fx[FX_SW + 1] << 8));
    uint32_t tos = (sw >> 11) & 7;
    uint32_t ret = 0xFFFF0000u;

    /* The two tag words are indexed differently and this is the whole
     * subtlety of the conversion: the abridged one is by *physical*
     * register, while FXSAVE stores the registers in *stack* order with
     * ST(0) first. So bit i, which is about physical register i, has to
     * be answered by looking at stack slot (i - TOP) & 7.
     *
     * Getting this wrong is invisible until TOP is non-zero, and then it
     * is very visible: with three values pushed the tag word reads 0x57FF
     * ("three empty registers hold zero") instead of 0x03FF ("three
     * registers hold valid numbers"). That is how it was found - the
     * first version had no rotation and disagreed with the host. */
    for (int i = 0; i < 8; i++) {
        uint32_t tag;
        if (!(abridged & (1u << i))) {
            tag = 3;                       /* empty */
        } else {
            const uint8_t* st = fx + FX_ST((i - tos) & 7u);
            uint32_t exp = (uint32_t)(st[8] | (st[9] << 8)) & 0x7FFFu;
            if (exp == 0x7FFF) {
                tag = 2;                   /* infinity or NaN */
            } else if (exp == 0) {
                int zero = 1;
                for (int b = 0; b < 8; b++) if (st[b]) { zero = 0; break; }
                tag = zero ? 1 : 2;        /* zero, or a denormal */
            } else {
                /* The explicit integer bit: set means an ordinary value,
                 * clear means unnormal, which the x87 calls special. */
                tag = (st[7] & 0x80) ? 0 : 2;
            }
        }
        ret |= tag << (2 * i);
    }
    return ret;
}

/* The inverse, for the restore path: two bits per register down to one,
 * "not empty" becoming "in use". */
static uint8_t tag_i387_to_fxsr(uint32_t twd) {
    uint8_t ret = 0;
    for (int i = 0; i < 8; i++) {
        if (((twd >> (2 * i)) & 3u) != 3u) ret |= (uint8_t)(1u << i);
    }
    return ret;
}

/* A clean unit, captured once, to hand to a handler. Linux enters a
 * signal handler with the FPU reset - control word 0x37f, an empty x87
 * stack, zeroed XMM registers, MXCSR 0x1f80 - so a handler that does
 * arithmetic cannot see or corrupt the interrupted computation's
 * registers. Checked against the host, not assumed. */
static fpu_state_t clean_fpu;
static int clean_fpu_ready = 0;

/* fninit resets the x87 half and leaves MXCSR and the XMM registers
 * exactly as they were, so capturing after it alone would bake whatever
 * happened to be in XMM0-7 into every signal handler's starting state.
 * The registers are zeroed and MXCSR set explicitly, and the CPU is then
 * asked what that looks like rather than 512 bytes of layout being
 * written out by hand. Destroys the live FPU state, so the caller must
 * have saved anything it wants. */
static void capture_clean_fpu(fpu_state_t* out) {
    static const uint32_t default_mxcsr = 0x1F80;
    __asm__ __volatile__("fninit\n\t"
                         "ldmxcsr %1\n\t"
                         "xorps %%xmm0, %%xmm0\n\t"
                         "xorps %%xmm1, %%xmm1\n\t"
                         "xorps %%xmm2, %%xmm2\n\t"
                         "xorps %%xmm3, %%xmm3\n\t"
                         "xorps %%xmm4, %%xmm4\n\t"
                         "xorps %%xmm5, %%xmm5\n\t"
                         "xorps %%xmm6, %%xmm6\n\t"
                         "xorps %%xmm7, %%xmm7\n\t"
                         "fxsave (%0)\n\t"
                         : : "r"(out->data), "m"(default_mxcsr) : "memory");
}

/* --- delivery ------------------------------------------------------------ */

static int deliver(registers_t* regs, int signo) {
    posix_proc_t* S = posix_current();
    k_sigaction_t* sa = &S->actions[signo];
    siginfo_rec_t* rec = &S->info_of[signo];

    /* Build the frame below the interrupted esp, 16-byte aligned so a
     * handler compiled with SSE in mind finds what it expects.
     *
     * Unless the handler asked for the alternate stack and the thread is
     * not already on it, which is the whole point of having one: a
     * SIGSEGV caused by running out of stack cannot be reported on that
     * same stack. Wine installs its exception handler this way. */
    uint32_t sp = regs->useresp;
    int use_alt = (sa->flags & SA_ONSTACK) && S->altstack_sp &&
                  S->altstack_size && !S->altstack_on;
    if (use_alt) {
        sp = S->altstack_sp + S->altstack_size;
        S->altstack_on = 1;
    }
    sp -= 128;                       /* red zone / breathing room */
    sp &= ~15u;

    sp -= sizeof(sigframe_t);
    sp &= ~15u;                      /* FXSAVE's alignment, not a nicety */
    sigframe_t* frame = (sigframe_t*)sp;
    frame->magic = SIGFRAME_MAGIC;

    /* siginfo_t. Zero the whole thing first: the union is 116 bytes and
     * a handler is entitled to read the member its si_code names, so the
     * ones this signal does not use must read as zero rather than as
     * whatever was on the stack. */
    kmemset(&frame->info, 0, sizeof(frame->info));
    frame->info.si_signo = signo;
    frame->info.si_errno = 0;
    frame->info.si_code  = rec->code;
    if (signo == SIGSEGV || signo == SIGBUS || signo == SIGFPE ||
        signo == SIGILL  || signo == SIGTRAP) {
        frame->info._sifields._sigfault.si_addr = rec->addr;
    } else if (rec->code == SI_USER || rec->code == SI_TKILL) {
        frame->info._sifields._kill.si_pid = rec->pid;
        frame->info._sifields._kill.si_uid = 0;
    }

    /* ucontext_t. uc_stack describes the alternate signal stack, and
     * there isn't one: a current Linux reports that as all three fields
     * zero (ss_flags carries the task's own SS_AUTODISARM-style flags,
     * not sas_ss_flags(sp), which is what makes it 0 rather than
     * SS_DISABLE - checked against the host rather than assumed).
     *
     * uc_flags stays 0 for the same reason fpstate does: Linux sets
     * UC_FP_XSTATE there when it has attached extended FP state, and
     * Novaris attaches none, so 0 and a null fpstate are consistent with
     * each other - Linux with an XSAVE machine reports uc_flags = 1
     * (UC_FP_XSTATE) and attaches an xstate header past the FXSAVE image;
     * Novaris saves plain FXSAVE and says so. */
    kmemset(&frame->uc, 0, sizeof(frame->uc));
    frame->uc.uc_flags = 0;
    frame->uc.uc_link  = 0;
    /* uc_stack describes the alternate signal stack. Zero when there is
     * none - which is what a current Linux reports, ss_flags carrying the
     * task's own flags rather than sas_ss_flags(sp), checked against the
     * host rather than assumed. */
    frame->uc.uc_stack.ss_sp    = S->altstack_sp;
    frame->uc.uc_stack.ss_flags = use_alt ? 1 /* SS_ONSTACK */ : 0;
    frame->uc.uc_stack.ss_size  = S->altstack_size;

    k_sigcontext_t* sc = &frame->uc.uc_mcontext;
    sc->gs = regs->gs; sc->fs = regs->fs; sc->es = regs->es; sc->ds = regs->ds;
    sc->edi = regs->edi; sc->esi = regs->esi; sc->ebp = regs->ebp;
    sc->esp = regs->useresp;   /* the *user* esp, not pusha's kernel one */
    sc->ebx = regs->ebx; sc->edx = regs->edx; sc->ecx = regs->ecx;
    sc->eax = regs->eax;
    /* Not regs->int_no/err_code: on the way out of a syscall or a timer
     * IRQ those name the syscall gate or the IRQ, and Linux reports the
     * last *fault* here regardless of what is delivering the signal. */
    sc->trapno = S->last_trapno;
    sc->err    = S->last_err;
    sc->eip    = regs->eip;
    sc->cs     = regs->cs;
    sc->eflags = regs->eflags;
    sc->esp_at_signal = regs->useresp;
    sc->ss     = regs->ss;
    sc->oldmask = S->blocked;
    sc->cr2     = S->last_cr2;

    frame->uc.uc_sigmask[0] = S->blocked;
    frame->uc.uc_sigmask[1] = 0;

    /* --- the x87/SSE state (Milestone 24) -------------------------------
     *
     * The live FPU registers *are* the interrupted thread's at this point:
     * delivery happens on the way back to ring 3, in that thread's
     * context, and the kernel does not use the FPU in between. So FXSAVE
     * straight into the frame. */
    k_fpstate_t* fp = &frame->fpstate;
    __asm__ __volatile__("fxsave (%0)" : : "r"(fp->fxsave) : "memory");

    /* Then the legacy half, derived from that image. The 0xffff0000 in
     * the top halves is Linux's, and is what a 16-bit-wide x87 field
     * widened to 32 bits has looked like since the 386. */
    const uint8_t* fx = fp->fxsave;
    fp->cw      = 0xFFFF0000u | (uint32_t)(fx[FX_CW] | (fx[FX_CW + 1] << 8));
    fp->sw      = 0xFFFF0000u | (uint32_t)(fx[FX_SW] | (fx[FX_SW + 1] << 8));
    fp->tag     = tag_fxsr_to_i387(fx);
    fp->ipoff   = *(const uint32_t*)(fx + FX_IP);
    /* Not the image's FCS/FDS: on 32-bit Linux these come from the
     * interrupted frame's cs and ds instead, which is a deliberate quirk
     * rather than an oversight - checked against the host. */
    fp->cssel   = regs->cs;
    fp->dataoff = *(const uint32_t*)(fx + FX_DP);
    fp->datasel = 0xFFFF0000u | (regs->ds & 0xFFFFu);
    for (int i = 0; i < 8; i++) {
        for (int b = 0; b < 10; b++) fp->st[i][b] = fx[FX_ST(i) + b];
    }
    fp->status = (uint16_t)(fx[FX_SW] | (fx[FX_SW + 1] << 8));
    fp->magic  = X86_FXSR_MAGIC;   /* the FXSAVE image above is present */

    sc->fpstate = (uint32_t)fp;

    /* The handler starts on a clean unit, exactly as it would on Linux:
     * an empty x87 stack, control word 0x37f, zeroed XMM registers and
     * MXCSR 0x1f80. Without this a handler that does any arithmetic at
     * all silently destroys the interrupted computation's registers -
     * which is what Novaris did until this milestone. */
    if (!clean_fpu_ready) { capture_clean_fpu(&clean_fpu); clean_fpu_ready = 1; }
    fpu_restore(&clean_fpu);

    /* mov $173, %eax ; int $0x80  - rt_sigreturn. Only used when the
     * program supplied no restorer of its own; Linux is equally happy to
     * be given one or to plant one, so accepting both is what keeps a
     * binary portable between them. */
    frame->trampoline[0] = 0xB8;
    frame->trampoline[1] = SYS_rt_sigreturn;
    frame->trampoline[2] = 0x00;
    frame->trampoline[3] = 0x00;
    frame->trampoline[4] = 0x00;
    frame->trampoline[5] = 0xCD;
    frame->trampoline[6] = 0x80;
    frame->trampoline[7] = 0x90;

    uint32_t ret_addr = (sa->flags & SA_RESTORER) && sa->restorer
                            ? sa->restorer
                            : (uint32_t)frame->trampoline;

    /* cdecl arguments, pushed right to left, then the return address.
     * A plain handler reads only the first; an SA_SIGINFO one reads all
     * three, and passing them unconditionally is harmless to the first. */
    sp -= 4; *(uint32_t*)sp = (uint32_t)&frame->uc;
    sp -= 4; *(uint32_t*)sp = (uint32_t)&frame->info;
    sp -= 4; *(uint32_t*)sp = (uint32_t)signo;
    sp -= 4; *(uint32_t*)sp = ret_addr;

    /* While the handler runs, this signal is blocked (unless the program
     * asked otherwise), plus whatever else its sa_mask names - which is
     * what stops a SIGSEGV handler that faults from recursing forever. */
    S->blocked |= sa->mask[0];
    if (!(sa->flags & SA_NODEFER)) S->blocked |= SIGBIT(signo);
    S->blocked &= ~UNMASKABLE;

    uint32_t handler = sa->handler;
    if (sa->flags & SA_RESETHAND) {
        sa->handler = SIG_DFL;
    }

    regs->eip = handler;
    regs->useresp = sp;

    /* The flags a handler is *entered* with are not the interrupted
     * thread's. Linux clears three, and each has a reason:
     *
     *   DF - the cdecl ABI says a function may assume the direction flag
     *        is clear on entry, and compiled code does: `rep movsb` in an
     *        inlined memcpy walks backwards otherwise. A program that
     *        legitimately had DF set when the signal arrived would
     *        otherwise hand its handler a broken string library.
     *   TF - or a single-stepping debugger would trap on the handler's
     *        first instruction, and the trap would raise another signal.
     *   RF - it suppresses one instruction-breakpoint fault, and the
     *        instruction it was suppressing is not the one about to run.
     *
     * The saved copy in uc_mcontext keeps all three, so rt_sigreturn puts
     * the interrupted thread's flags back exactly. */
    regs->eflags &= ~(0x400u   /* DF */
                    | 0x100u   /* TF */
                    | 0x10000u /* RF */);
    return 1;
}

static const char* signal_name(int signo) {
    switch (signo) {
        case SIGILL:  return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGKILL: return "SIGKILL";
        case SIGUSR1: return "SIGUSR1";
        case SIGSEGV: return "SIGSEGV";
        case SIGUSR2: return "SIGUSR2";
        case SIGTERM: return "SIGTERM";
        case SIGTRAP: return "SIGTRAP";
        default:      return "signal";
    }
}

/* The default action for everything Novaris raises is to terminate. The
 * exit status carries the signal number in Linux's encoding, so a parent
 * in wait4 can tell a program that died from one that exited. */
static void default_action(int signo) {
    terminal_writestring_color("[posix] ", VGA_COLOR_LIGHT_RED);
    terminal_writestring("terminated by ");
    terminal_writestring(signal_name(signo));
    terminal_writestring(" (no handler installed)\n");
    posix_exit_status(signo);
}

void posix_deliver_pending(registers_t* regs) {
    posix_proc_t* S = posix_current();
    if (!S->any_handler && !S->pending) return;
    if ((regs->cs & 3) != 3) return;  /* only ever into ring 3 */

    uint32_t ready = S->pending & ~S->blocked;
    if (!ready) return;

    for (int signo = 1; signo < NSIG; signo++) {
        if (!(ready & SIGBIT(signo))) continue;
        S->pending &= ~SIGBIT(signo);

        uint32_t h = S->actions[signo].handler;
        if (h == SIG_IGN) continue;
        if (h == SIG_DFL) {
            default_action(signo); /* does not return */
            return;
        }
        deliver(regs, signo);
        return; /* one per return to ring 3, like Linux */
    }
}

int posix_handle_fault_signal(registers_t* regs, uint32_t vector) {
    posix_proc_t* S = posix_current();
    int signo;
    int32_t code;
    uint32_t addr;

    /* cr2 holds the faulting address and is only meaningful for vector 14,
     * but it has to be read before anything else can fault. */
    uint32_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));

    switch (vector) {
        case 0:  signo = SIGFPE;  code = FPE_INTDIV;  addr = regs->eip; break;
        case 4:  signo = SIGSEGV; code = SI_KERNEL;   addr = 0; break;
        case 5:  signo = SIGSEGV; code = SI_KERNEL;   addr = 0; break;
        case 6:  signo = SIGILL;  code = ILL_ILLOPN;  addr = regs->eip; break;
        case 13: signo = SIGSEGV; code = SI_KERNEL;   addr = 0; break;
        case 14:
            signo = SIGSEGV;
            addr = cr2;
            /* err_code bit 0 = the page was present, so the mapping
             * existed and the *access* was refused; clear means there was
             * no mapping at all. That is exactly Linux's MAPERR/ACCERR
             * split, and Wine reads it to tell a guard-page hit from a
             * genuinely bad pointer. */
            code = (regs->err_code & 1) ? SEGV_ACCERR : SEGV_MAPERR;
            break;
        default: return 0;
    }

    /* Recorded whether or not a handler is installed and whether or not
     * this fault is fatal, because the *next* signal's sigcontext reports
     * it too - which is what Linux does. */
    S->last_trapno = vector;
    S->last_err    = regs->err_code;
    S->last_cr2    = (vector == 14) ? cr2 : 0;

    if (!S->any_handler) return 0;
    uint32_t h = S->actions[signo].handler;
    if (h == SIG_DFL || h == SIG_IGN) return 0;

    /* A fault *inside* the handler for that same fault would loop for
     * ever. The signal is blocked during its own handler (see deliver),
     * so this catches the case where the program cleared that with
     * SA_NODEFER and then faulted again. */
    if (S->blocked & SIGBIT(signo)) {
        terminal_writestring_color("[posix] ", VGA_COLOR_LIGHT_RED);
        terminal_writestring("fault inside its own signal handler - "
                             "terminating rather than looping\n");
        return 0;
    }
    if (S->in_delivery > 8) return 0;

    S->info_of[signo].code = code;
    S->info_of[signo].addr = addr;
    S->info_of[signo].pid  = 0;

    S->in_delivery++;
    deliver(regs, signo);
    S->in_delivery--;
    return 1;
}

int posix_raise_from(int signo, int32_t code, int32_t pid) {
    posix_proc_t* S = posix_current();
    if (signo <= 0 || signo >= NSIG) return -EINVAL;
    S->pending |= SIGBIT(signo);
    S->info_of[signo].code = code;
    S->info_of[signo].addr = 0;
    S->info_of[signo].pid  = pid;
    return 0;
}

/* Raises a signal against *another* process, which is what kill() means
 * now that there is more than one. The target's own pending mask is set,
 * and it finds out on its next return to ring 3 - the only moment a
 * signal can be delivered here. */
/* sigaltstack(new, old). Registering one is all this does; deliver()
 * above is where it is used, and only for a handler that asked with
 * SA_ONSTACK. */
int32_t posix_sys_sigaltstack(const k_stack_t* ss, k_stack_t* old) {
    posix_proc_t* S = posix_current();

    if (old) {
        old->ss_sp = S->altstack_sp;
        old->ss_size = S->altstack_size;
        old->ss_flags = S->altstack_on ? 1 /* SS_ONSTACK */
                      : (S->altstack_sp ? 0 : SS_DISABLE);
    }
    if (!ss) return 0;
    /* Changing it while a handler is running on it would move the ground
     * out from under that handler. */
    if (S->altstack_on) return -EPERM;

    if (ss->ss_flags & SS_DISABLE) {
        S->altstack_sp = 0;
        S->altstack_size = 0;
        return 0;
    }
    if (ss->ss_flags) return -EINVAL;
    /* MINSIGSTKSZ on i386. A stack too small to hold the frame this
     * kernel builds would fault inside delivery, which is the one place
     * a fault cannot be reported. */
    if (ss->ss_size < 2048) return -ENOMEM;
    S->altstack_sp = ss->ss_sp;
    S->altstack_size = ss->ss_size;
    return 0;
}

int posix_raise_pid(int pid, int signo, int32_t code, int32_t from) {
    if (signo < 0 || signo >= NSIG) return -EINVAL;
    posix_proc_t* t = posix_proc_by_pid(pid);
    if (!t || t->exited) return -ESRCH;
    if (signo == 0) return 0;   /* kill(pid, 0) is an existence check */
    t->pending |= SIGBIT(signo);
    t->info_of[signo].code = code;
    t->info_of[signo].addr = 0;
    t->info_of[signo].pid  = from;
    return 0;
}

int posix_raise(int signo) {
    if (signo <= 0 || signo >= NSIG) return -EINVAL;
    if (signo == 0) return 0; /* kill(pid, 0) is an existence check */
    return posix_raise_from(signo, SI_USER, 0);
}

/* --- the syscalls -------------------------------------------------------- */

int32_t posix_sys_rt_sigaction(int signo, const k_sigaction_t* act,
                               k_sigaction_t* old, uint32_t sigsetsize) {
    posix_proc_t* S = posix_current();
    if (signo <= 0 || signo >= NSIG) return -EINVAL;
    if (sigsetsize != 8) return -EINVAL; /* i386 rt_ sigsets are 8 bytes */
    if (signo == SIGKILL) return -EINVAL;

    if (old) *old = S->actions[signo];
    if (act) {
        S->actions[signo] = *act;
        if (act->handler != SIG_DFL && act->handler != SIG_IGN) {
            /* A missing SA_RESTORER is accepted, because Linux accepts it:
             * the kernel plants a trampoline in the signal frame instead.
             * Being stricter here would have been defensible in isolation
             * and would have made a program behave differently on the two
             * systems, which is the one thing this ABI must not do. */
            S->any_handler = 1;
        }
    }
    return 0;
}

int32_t posix_sys_rt_sigprocmask(int how, const uint32_t* set, uint32_t* old,
                                 uint32_t sigsetsize) {
    posix_proc_t* S = posix_current();
    if (sigsetsize != 8) return -EINVAL;
    if (old) { old[0] = S->blocked; old[1] = 0; }
    if (!set) return 0;

    switch (how) {
        case SIG_BLOCK:   S->blocked |= set[0]; break;
        case SIG_UNBLOCK: S->blocked &= ~set[0]; break;
        case SIG_SETMASK: S->blocked = set[0]; break;
        default: return -EINVAL;
    }
    S->blocked &= ~UNMASKABLE;
    return 0;
}

int32_t posix_sys_rt_sigreturn(registers_t* regs) {
    posix_proc_t* S = posix_current();
    /* The handler's `ret` popped the return address, leaving its three
     * cdecl arguments on the stack, and the frame sits immediately above
     * them. */
    uint32_t sp = regs->useresp + 12;
    sigframe_t* frame = (sigframe_t*)sp;

    if (frame->magic != SIGFRAME_MAGIC) {
        terminal_writestring_color("[posix] ", VGA_COLOR_LIGHT_RED);
        terminal_writestring("rt_sigreturn with no valid signal frame - "
                             "terminating\n");
        posix_exit_process();
        return -EFAULT;
    }

    /* The mask comes back from uc_sigmask rather than from a shadow copy,
     * because a handler is allowed to change it - Linux reloads it from
     * there too. SIGKILL can never end up blocked whatever was written. */
    S->blocked = frame->uc.uc_sigmask[0] & ~UNMASKABLE;
    /* Off the alternate stack again, if that is where the handler ran. */
    if (frame->uc.uc_stack.ss_flags & 1) S->altstack_on = 0;

    /* Everything else comes out of uc_mcontext, which is the memory the
     * handler was given a pointer to and may have edited. That is the
     * point of Milestone 23: a handler that rewrites gregs[REG_EIP] or
     * gregs[REG_EAX] and returns gets what it asked for, which is how
     * Wine turns a SIGSEGV into a Windows exception dispatch.
     *
     * The segment selectors and the fields the interrupt epilogue owns
     * are still *not* taken from the frame: putting back a user-supplied
     * cs or ss would be a privilege-escalation hole, so those keep the
     * values the kernel already trusts. */
    const k_sigcontext_t* s = &frame->uc.uc_mcontext;
    regs->edi = s->edi; regs->esi = s->esi; regs->ebp = s->ebp;
    regs->ebx = s->ebx; regs->edx = s->edx; regs->ecx = s->ecx;
    regs->eax = s->eax;
    regs->eip = s->eip;
    regs->useresp = s->esp;
    /* eflags: keep the caller's arithmetic flags but never let it set
     * IOPL, IF or the trap flag from user data. DF is in that mask, so
     * a thread that had the direction flag set when the signal arrived
     * gets it back, having been given a handler that ran with it clear. */
    regs->eflags = (s->eflags & 0x00000CD5u) | 0x202u;

    /* --- and the x87/SSE state (Milestone 24) ---------------------------
     *
     * Restored, not merely reported, and from both halves of the fpstate
     * the way Linux does it: the FXSAVE image carries the XMM registers
     * and MXCSR, and the legacy fields then overwrite the x87 half of it.
     * Both matter because Wine writes both - CONTEXT.FloatSave into the
     * legacy half, CONTEXT.ExtendedRegisters into the image - and a
     * handler that edited only the one it knows about would otherwise
     * find the edit ignored.
     *
     * A null fpstate is accepted rather than treated as an error: a
     * handler is entitled to clear the pointer, and Linux reads that as
     * "leave the FPU alone". */
    if (s->fpstate) {
        k_fpstate_t* fp = (k_fpstate_t*)s->fpstate;
        uint8_t* fx = fp->fxsave;

        /* FXRSTOR faults if MXCSR has any reserved bit set, and this
         * buffer has been in the program's hands. Masking to the defined
         * bits turns a would-be #GP in the kernel into a value the CPU
         * will accept. */
        uint32_t* mxcsr = (uint32_t*)(fx + 24);
        *mxcsr &= 0x0000FFBFu;

        if (fp->magic == X86_FXSR_MAGIC) {
            /* The legacy fields win for the x87 half, exactly as Linux's
             * convert_to_fxsr does. fop is packed into the top half of
             * cssel on this path and nowhere else - an asymmetry with the
             * save side that is Linux's, not a mistake here. */
            fx[FX_CW]     = (uint8_t)(fp->cw & 0xFF);
            fx[FX_CW + 1] = (uint8_t)((fp->cw >> 8) & 0xFF);
            fx[FX_SW]     = (uint8_t)(fp->sw & 0xFF);
            fx[FX_SW + 1] = (uint8_t)((fp->sw >> 8) & 0xFF);
            fx[FX_TW]     = tag_i387_to_fxsr(fp->tag);
            fx[FX_TW + 1] = 0;
            fx[FX_OP]     = (uint8_t)((fp->cssel >> 16) & 0xFF);
            fx[FX_OP + 1] = (uint8_t)((fp->cssel >> 24) & 0xFF);
            *(uint32_t*)(fx + FX_IP) = fp->ipoff;
            fx[FX_CS]     = (uint8_t)(fp->cssel & 0xFF);
            fx[FX_CS + 1] = (uint8_t)((fp->cssel >> 8) & 0xFF);
            *(uint32_t*)(fx + FX_DP) = fp->dataoff;
            fx[FX_DS]     = (uint8_t)(fp->datasel & 0xFF);
            fx[FX_DS + 1] = (uint8_t)((fp->datasel >> 8) & 0xFF);
            for (int i = 0; i < 8; i++) {
                for (int b = 0; b < 10; b++) fx[FX_ST(i) + b] = fp->st[i][b];
            }
        }
        __asm__ __volatile__("fxrstor (%0)" : : "r"(fx) : "memory");
    }

    return (int32_t)regs->eax;
}
