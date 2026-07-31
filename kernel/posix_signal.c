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
 * Wine use. */

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
    blocked = 0;
    pending = 0;
    any_handler = 0;
    in_delivery = 0;
}

/* --- the sigframe -------------------------------------------------------
 *
 * Novaris's own layout, not Linux's. That is fine and worth being clear
 * about: the program never looks inside it. It builds the frame, the
 * program's restorer issues rt_sigreturn without touching it, and the
 * same kernel consumes it. Only the *interface* - SA_RESTORER, the
 * handler's argument, rt_sigreturn's number - has to match Linux, and it
 * does. */
typedef struct {
    uint32_t    magic;
    uint32_t    saved_blocked;
    registers_t saved;
    /* siginfo_t's first three fields, which is all a handler that reads
     * si_signo needs. SA_SIGINFO handlers get a pointer to this. */
    uint32_t    si_signo, si_errno, si_code;
    /* Stand-in for ucontext_t. Not populated - a handler that walks it
     * for the faulting register state gets zeroes, which ROADMAP.md
     * Milestone 19 says out loud. Present so the third argument is a
     * readable pointer rather than null. */
    uint32_t    ucontext[24];
    /* The trampoline the handler returns through when the program did not
     * supply one. Linux plants its own the same way, and it works for the
     * same reason: 32-bit x86 without PAE has no NX bit, so the user
     * stack is executable. */
    uint8_t     trampoline[8];
} sigframe_t;

#define SIGFRAME_MAGIC 0x5347464Du /* "SGFM" */

/* --- delivery ------------------------------------------------------------ */

static int deliver(registers_t* regs, int signo) {
    k_sigaction_t* sa = &actions[signo];

    /* Build the frame below the interrupted esp, 16-byte aligned so a
     * handler compiled with SSE in mind finds what it expects. */
    uint32_t sp = regs->useresp;
    sp -= 128;                       /* red zone / breathing room */
    sp &= ~15u;

    sp -= sizeof(sigframe_t);
    sigframe_t* frame = (sigframe_t*)sp;
    frame->magic = SIGFRAME_MAGIC;
    frame->saved_blocked = blocked;
    frame->saved = *regs;

    frame->si_signo = (uint32_t)signo;
    frame->si_errno = 0;
    frame->si_code = 0;
    for (int i = 0; i < 24; i++) frame->ucontext[i] = 0;

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
    sp -= 4; *(uint32_t*)sp = (uint32_t)&frame->ucontext;
    sp -= 4; *(uint32_t*)sp = (uint32_t)&frame->si_signo;
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
    switch (vector) {
        case 0:  signo = SIGFPE;  break;  /* divide error */
        case 4:  signo = SIGSEGV; break;  /* overflow */
        case 5:  signo = SIGSEGV; break;  /* bound range */
        case 6:  signo = SIGILL;  break;  /* invalid opcode */
        case 13: signo = SIGSEGV; break;  /* general protection */
        case 14: signo = SIGSEGV; break;  /* page fault */
        default: return 0;
    }

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

    in_delivery++;
    deliver(regs, signo);
    in_delivery--;
    return 1;
}

int posix_raise(int signo) {
    if (signo <= 0 || signo >= NSIG) return -EINVAL;
    if (signo == 0) return 0; /* kill(pid, 0) is an existence check */
    pending |= SIGBIT(signo);
    return 0;
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

    blocked = frame->saved_blocked;

    /* Restore everything except the segment selectors and the fields the
     * interrupt epilogue owns: putting back a user-supplied cs or ss
     * would be a privilege-escalation hole, so those keep the values the
     * kernel already trusts. */
    registers_t* s = &frame->saved;
    regs->edi = s->edi; regs->esi = s->esi; regs->ebp = s->ebp;
    regs->ebx = s->ebx; regs->edx = s->edx; regs->ecx = s->ecx;
    regs->eax = s->eax;
    regs->eip = s->eip;
    regs->useresp = s->useresp;
    /* eflags: keep the caller's arithmetic flags but never let it set
     * IOPL, IF or the trap flag from user data. */
    regs->eflags = (s->eflags & 0x00000CD5u) | 0x202u;

    return (int32_t)regs->eax;
}
