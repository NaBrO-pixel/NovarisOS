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
#include "console.h"
#include "kstring.h"
#include "process.h"

/* --- per-process signal state ------------------------------------------- */

static k_sigaction_t actions[NSIG];
static uint32_t blocked = 0;   /* bit (signo-1) set = blocked */
static uint32_t pending = 0;
static int      any_handler = 0; /* fast path: nothing installed, nothing to do */
static int      in_delivery = 0; /* guards against recursion in the fault path */

/* What to put in siginfo_t when each signal is finally delivered.
 * `pending` is a bitmask rather than a queue, so this is one record per
 * signal number rather than one per raise - which is the same shape as
 * the delivery it feeds, and no worse. */
typedef struct {
    int32_t  code;
    uint32_t addr;   /* si_addr, for the fault signals */
    int32_t  pid;    /* si_pid, for kill/tgkill */
} siginfo_rec_t;
static siginfo_rec_t info_of[NSIG];

/* The last CPU fault this process took. Linux keeps these in the thread
 * struct and copies them into every sigcontext it builds, fault or not,
 * so a signal delivered by kill() carries whatever the last real fault
 * left behind. Matching that is free and avoids the same binary seeing
 * different numbers on the two systems - even meaningless ones. */
static uint32_t last_trapno = 0, last_err = 0, last_cr2 = 0;

#define SIGBIT(n) (1u << ((n) - 1))

/* SIGKILL and SIGSTOP can never be caught or blocked. */
#define UNMASKABLE (SIGBIT(SIGKILL))

void posix_signal_reset(void) {
    for (int i = 0; i < NSIG; i++) {
        actions[i].handler = SIG_DFL;
        actions[i].flags = 0;
        actions[i].restorer = 0;
        actions[i].mask[0] = 0;
        actions[i].mask[1] = 0;
    }
    for (int i = 0; i < NSIG; i++) {
        info_of[i].code = SI_USER;
        info_of[i].addr = 0;
        info_of[i].pid  = 0;
    }
    blocked = 0;
    pending = 0;
    any_handler = 0;
    in_delivery = 0;
    last_trapno = last_err = last_cr2 = 0;
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

/* --- delivery ------------------------------------------------------------ */

static int deliver(registers_t* regs, int signo) {
    k_sigaction_t* sa = &actions[signo];
    siginfo_rec_t* rec = &info_of[signo];

    /* Build the frame below the interrupted esp, 16-byte aligned so a
     * handler compiled with SSE in mind finds what it expects. */
    uint32_t sp = regs->useresp;
    sp -= 128;                       /* red zone / breathing room */
    sp &= ~15u;

    sp -= sizeof(sigframe_t);
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
     * each other. */
    kmemset(&frame->uc, 0, sizeof(frame->uc));
    frame->uc.uc_flags = 0;
    frame->uc.uc_link  = 0;
    frame->uc.uc_stack.ss_sp    = 0;
    frame->uc.uc_stack.ss_flags = 0;
    frame->uc.uc_stack.ss_size  = 0;

    k_sigcontext_t* sc = &frame->uc.uc_mcontext;
    sc->gs = regs->gs; sc->fs = regs->fs; sc->es = regs->es; sc->ds = regs->ds;
    sc->edi = regs->edi; sc->esi = regs->esi; sc->ebp = regs->ebp;
    sc->esp = regs->useresp;   /* the *user* esp, not pusha's kernel one */
    sc->ebx = regs->ebx; sc->edx = regs->edx; sc->ecx = regs->ecx;
    sc->eax = regs->eax;
    /* Not regs->int_no/err_code: on the way out of a syscall or a timer
     * IRQ those name the syscall gate or the IRQ, and Linux reports the
     * last *fault* here regardless of what is delivering the signal. */
    sc->trapno = last_trapno;
    sc->err    = last_err;
    sc->eip    = regs->eip;
    sc->cs     = regs->cs;
    sc->eflags = regs->eflags;
    sc->esp_at_signal = regs->useresp;
    sc->ss     = regs->ss;
    sc->fpstate = 0;           /* no FP state is saved - see honest scope */
    sc->oldmask = blocked;
    sc->cr2     = last_cr2;

    frame->uc.uc_sigmask[0] = blocked;
    frame->uc.uc_sigmask[1] = 0;

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
    blocked |= sa->mask[0];
    if (!(sa->flags & SA_NODEFER)) blocked |= SIGBIT(signo);
    blocked &= ~UNMASKABLE;

    uint32_t handler = sa->handler;
    if (sa->flags & SA_RESETHAND) {
        sa->handler = SIG_DFL;
    }

    regs->eip = handler;
    regs->useresp = sp;
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

/* The default action for everything Novaris raises is to terminate. */
static void default_action(int signo) {
    terminal_writestring_color("[posix] ", VGA_COLOR_LIGHT_RED);
    terminal_writestring("terminated by ");
    terminal_writestring(signal_name(signo));
    terminal_writestring(" (no handler installed)\n");
    posix_exit_process();
}

void posix_deliver_pending(registers_t* regs) {
    if (!any_handler && !pending) return;
    if ((regs->cs & 3) != 3) return;  /* only ever into ring 3 */

    uint32_t ready = pending & ~blocked;
    if (!ready) return;

    for (int signo = 1; signo < NSIG; signo++) {
        if (!(ready & SIGBIT(signo))) continue;
        pending &= ~SIGBIT(signo);

        uint32_t h = actions[signo].handler;
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
    last_trapno = vector;
    last_err    = regs->err_code;
    last_cr2    = (vector == 14) ? cr2 : 0;

    if (!any_handler) return 0;
    uint32_t h = actions[signo].handler;
    if (h == SIG_DFL || h == SIG_IGN) return 0;

    /* A fault *inside* the handler for that same fault would loop for
     * ever. The signal is blocked during its own handler (see deliver),
     * so this catches the case where the program cleared that with
     * SA_NODEFER and then faulted again. */
    if (blocked & SIGBIT(signo)) {
        terminal_writestring_color("[posix] ", VGA_COLOR_LIGHT_RED);
        terminal_writestring("fault inside its own signal handler - "
                             "terminating rather than looping\n");
        return 0;
    }
    if (in_delivery > 8) return 0;

    info_of[signo].code = code;
    info_of[signo].addr = addr;
    info_of[signo].pid  = 0;

    in_delivery++;
    deliver(regs, signo);
    in_delivery--;
    return 1;
}

int posix_raise_from(int signo, int32_t code, int32_t pid) {
    if (signo <= 0 || signo >= NSIG) return -EINVAL;
    pending |= SIGBIT(signo);
    info_of[signo].code = code;
    info_of[signo].addr = 0;
    info_of[signo].pid  = pid;
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
    if (signo <= 0 || signo >= NSIG) return -EINVAL;
    if (sigsetsize != 8) return -EINVAL; /* i386 rt_ sigsets are 8 bytes */
    if (signo == SIGKILL) return -EINVAL;

    if (old) *old = actions[signo];
    if (act) {
        actions[signo] = *act;
        if (act->handler != SIG_DFL && act->handler != SIG_IGN) {
            /* A missing SA_RESTORER is accepted, because Linux accepts it:
             * the kernel plants a trampoline in the signal frame instead.
             * Being stricter here would have been defensible in isolation
             * and would have made a program behave differently on the two
             * systems, which is the one thing this ABI must not do. */
            any_handler = 1;
        }
    }
    return 0;
}

int32_t posix_sys_rt_sigprocmask(int how, const uint32_t* set, uint32_t* old,
                                 uint32_t sigsetsize) {
    if (sigsetsize != 8) return -EINVAL;
    if (old) { old[0] = blocked; old[1] = 0; }
    if (!set) return 0;

    switch (how) {
        case SIG_BLOCK:   blocked |= set[0]; break;
        case SIG_UNBLOCK: blocked &= ~set[0]; break;
        case SIG_SETMASK: blocked = set[0]; break;
        default: return -EINVAL;
    }
    blocked &= ~UNMASKABLE;
    return 0;
}

int32_t posix_sys_rt_sigreturn(registers_t* regs) {
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
    blocked = frame->uc.uc_sigmask[0] & ~UNMASKABLE;

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
     * IOPL, IF or the trap flag from user data. */
    regs->eflags = (s->eflags & 0x00000CD5u) | 0x202u;

    return (int32_t)regs->eax;
}
