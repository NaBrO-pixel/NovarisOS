/* signal64.c - delivering a fault to a ring-3 handler. */

#include "signal64.h"
#include "kstring.h"
#include "serial64.h"
#include "proc64.h"
#include "sched64.h"
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

/* One table per process, not one for the machine.
 *
 * It was a single global array, so every process in the system shared
 * one set of signal handlers: a handler installed by one was found by
 * all, and execve - which on Linux resets every caught signal to its
 * default - left them in place. A process that installed no SIGSEGV
 * handler therefore did not get the default action of dying; it was
 * sent to whatever address another process had registered, in an
 * address space where that address means something else.
 *
 * Measured: services.exe took 673,430 SIGSEGVs at one instruction,
 * reading a page it had itself unmapped, with six rt_sigreturns in the
 * whole run - so the handler was never really running and never
 * returning. rsp walked down a little over 2KB per delivery, which is a
 * frame being pushed for a handler that faults before it does anything.
 *
 * Threads share their process's table, which falls out of indexing by
 * process slot rather than by task. */
static ksigaction64_t handlers_by_slot[PROC64_MAX][NSIG];

/* The calling process's handlers. Slot -1 - before there is a process,
 * which is where the bring-up layers run - gets slot 0, so the early
 * tests keep the single table they were written against. */
static ksigaction64_t* handlers_of(int slot) {
    if (slot < 0 || slot >= PROC64_MAX) slot = 0;
    return handlers_by_slot[slot];
}

#define handlers (handlers_of(proc64_current_slot()))
static uint64_t delivered, returns, raised;

/* What kill(2) has recorded and the target has not acted on yet, one
 * bit per signal, indexed by process slot exactly as the handler table
 * is. A set rather than a queue: this is the non-realtime behaviour,
 * where a second SIGTERM arriving before the first is taken does not
 * produce two SIGTERMs. Linux does the same for signals below 32. */
static uint64_t pending_by_slot[PROC64_MAX];

/* The same thing for a signal addressed to one thread rather than to a
 * process, indexed by task slot - which is what a tid is here.
 *
 * Linux keeps these two sets apart and so must this: kill(2) may be
 * taken by whichever thread reaches the boundary first, but tgkill(2)
 * names a thread, and the wineserver's use of it depends on that. It
 * stops a thread by sending it SIGUSR1 and then asks that thread for
 * its register context; delivered to a sibling, the context handed back
 * would be the wrong thread's and the suspended one would never stop. */
static uint64_t pending_by_task[SCHED64_MAX_TASKS];

/* Who raised each pending signal, for the siginfo rt_sigtimedwait hands
 * back: the pid of the process that called kill or tgkill. One per
 * signal, like the pending bits - a second raise before the first is
 * taken overwrites it, as Linux's non-realtime signals coalesce. */
static int sender_by_slot[PROC64_MAX][NSIG];
static int sender_by_task[SCHED64_MAX_TASKS][NSIG];

/* Each thread's blocked set, in the same encoding as the pending sets -
 * bit `sig`, where a user sigset_t has bit sig-1 - and per thread
 * because that is what a signal mask is on Linux.
 *
 * rt_sigprocmask was accepted and ignored, on the reasoning that masking
 * only matters for asynchronous signals. It matters as much for a fault
 * in a handler: Linux blocks the signal being handled, so a second
 * SIGSEGV inside a SIGSEGV handler finds it blocked and kills the
 * process, where this kernel delivered it again - and again. That is how
 * one Wine thread whose stack had been freed under it took 5.8 million
 * SIGSEGVs and spun through the rest of the run instead of dying.
 *
 * And Wine leans on masks everywhere else: it blocks its server signals
 * around every call to the wineserver, and a SIGUSR1 suspend arriving
 * inside one of those calls has to wait for it to finish. */
static uint64_t blocked_by_task[SCHED64_MAX_TASKS];

/* The mask a thread had before rt_sigsuspend or ppoll replaced it for
 * the length of the call, and whether there is one. Linux's
 * saved_sigmask.
 *
 * It is what the call's caller gets back, but not necessarily when the
 * call returns: if the call ends because a handler is about to run, the
 * handler runs under the temporary mask (plus its own) and the saved one
 * goes into the frame, so it comes back at rt_sigreturn. That is the
 * whole point of sigsuspend - the window in which the signal may arrive
 * is exactly the call, and nothing between the unblock and the sleep. */
static uint64_t saved_by_task[SCHED64_MAX_TASKS];
static uint8_t  has_saved_by_task[SCHED64_MAX_TASKS];

/* SIGKILL and SIGSTOP cannot be blocked; Linux drops them from any mask
 * it is given rather than refusing the call. */
#define UNBLOCKABLE ((1ull << SIG64_KILLSIG) | (1ull << SIG64_STOP))

/* Each thread's alternate signal stack - see on_altstack below. */
static uint64_t sas_sp_by_task[SCHED64_MAX_TASKS];
static uint64_t sas_size_by_task[SCHED64_MAX_TASKS];

static int my_task(void) {
    int t = sched64_current();
    return (t >= 0 && t < SCHED64_MAX_TASKS) ? t : -1;
}

static uint64_t* my_blocked(void) {
    int t = sched64_current();
    static uint64_t none;
    if (t < 0 || t >= SCHED64_MAX_TASKS) { none = 0; return &none; }
    return &blocked_by_task[t];
}

/* A user sigset_t's first word and the kernel's encoding, both ways. */
static uint64_t from_user_set(uint64_t u) { return (u << 1) & ~UNBLOCKABLE; }
static uint64_t to_user_set(uint64_t k)   { return k >> 1; }

/* Replaces the calling thread's mask with `user_set` for the length of
 * a call. A call that parks is re-issued from the start when it wakes,
 * so this is reached again on every turn; only the first saves, or the
 * second would save the temporary mask as the one to go back to. */
void signal64_push_mask(uint64_t user_set) {
    int t = sched64_current();
    if (t < 0 || t >= SCHED64_MAX_TASKS) return;
    if (!has_saved_by_task[t]) {
        saved_by_task[t] = blocked_by_task[t];
        has_saved_by_task[t] = 1;
    }
    blocked_by_task[t] = from_user_set(user_set);
}

/* Puts the saved mask back, if there is one. Returns 1 if there was. */
int signal64_pop_mask(void) {
    int t = sched64_current();
    if (t < 0 || t >= SCHED64_MAX_TASKS || !has_saved_by_task[t]) return 0;
    blocked_by_task[t] = saved_by_task[t];
    has_saved_by_task[t] = 0;
    return 1;
}

uint64_t signal64_blocked(int tid) {
    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return 0;
    return blocked_by_task[tid];
}

int signal64_sigprocmask(int how, const uint64_t* set, uint64_t* oldset,
                         uint64_t setsize) {
    uint64_t* b = my_blocked();
    uint64_t was = *b;

    /* Linux insists on its own sigset_t size, eight bytes, and glibc
     * always passes it. */
    if (setsize != 8) return -22;                       /* -EINVAL */

    if (set) {
        uint64_t k = from_user_set(*set);
        switch (how) {
        case SIG64_BLOCK:   *b = was | k;  break;
        case SIG64_UNBLOCK: *b = was & ~k; break;
        case SIG64_SETMASK: *b = k;        break;
        default:            return -22;                 /* -EINVAL */
        }
    }
    if (oldset) *oldset = to_user_set(was);
    return 0;
}

/* What is pending for the calling thread, blocked or not - which is the
 * question rt_sigpending asks. */
uint64_t signal64_sigpending(void) {
    int t = sched64_current();
    int slot = proc64_current_slot();
    uint64_t k = 0;
    if (t >= 0 && t < SCHED64_MAX_TASKS) k |= pending_by_task[t];
    if (slot >= 0 && slot < PROC64_MAX)  k |= pending_by_slot[slot];
    return to_user_set(k) & *my_blocked() >> 1;
}

/* A new task's mask: its creator's, as clone and fork give it on Linux,
 * or empty for a task with no creator - the first one of a run. */
void signal64_task_start(int tid, int from_tid) {
    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return;
    pending_by_task[tid] = 0;
    blocked_by_task[tid] = (from_tid >= 0 && from_tid < SCHED64_MAX_TASKS)
                         ? blocked_by_task[from_tid] : 0;
    /* Inside a sigsuspend is not something a new thread can be. And
     * clone is never called from inside one, so the creator's own
     * blocked set is its real mask. */
    has_saved_by_task[tid] = 0;
    /* And no alternate stack: a new thread's is wherever it registers
     * one. fork copies the creator's afterwards - see below. */
    sas_sp_by_task[tid] = sas_size_by_task[tid] = 0;
}

/* Whether the default action for a signal is to ignore it.
 *
 * The rest end the process, and getting this list wrong is not a
 * subtle failure: SIGCHLD is sent to every process whose child exits,
 * and a kernel that treats an uncaught SIGCHLD as fatal would kill
 * every parent in the system the moment it reaped anything. */
static int default_is_ignore(int sig) {
    switch (sig) {
    case SIG64_CHLD: case SIG64_CONT: case SIG64_URG: case SIG64_WINCH:
        return 1;

    /* The four stopping signals, recorded and dropped. There is no job
     * control here - nothing to stop a process into, and nothing to
     * continue it from - so this is wrong in a way worth naming: a
     * program sent SIGSTOP keeps running. It is the lesser wrong.
     * Treating them as fatal would end processes that Linux merely
     * pauses, which is a difference a test could not recover from. */
    case SIG64_STOP: case SIG64_TSTP: case SIG64_TTIN: case SIG64_TTOU:
        return 1;

    default:
        return 0;
    }
}

int signal64_disposition(int pid, int sig) {
    const ksigaction64_t* h;

    if (sig <= 0 || sig >= NSIG) return SIG64_DISP_IGNORE;

    /* SIGKILL cannot be caught or ignored. Linux refuses to install a
     * handler for it in sigaction; this kernel's sigaction does not
     * refuse, so the rule is enforced here, where it is acted on -
     * otherwise a process could make itself unkillable by installing
     * one. */
    if (sig == SIG64_KILLSIG) return SIG64_DISP_TERM;

    h = handlers_of(proc64_slot_of(pid));
    if (h[sig].handler == SIG64_IGN) return SIG64_DISP_IGNORE;
    if (h[sig].handler)              return SIG64_DISP_HANDLER;
    return default_is_ignore(sig) ? SIG64_DISP_IGNORE : SIG64_DISP_TERM;
}

int signal64_raise(int pid, int sig) {
    int slot = proc64_slot_of(pid);

    if (sig <= 0 || sig >= NSIG)     return -22;       /* -EINVAL */
    if (slot < 0 || slot >= PROC64_MAX) return -3;     /* -ESRCH  */

    pending_by_slot[slot] |= 1ull << sig;
    sender_by_slot[slot][sig] = proc64_current_pid();
    raised++;
    return 0;
}

/* These, and their _task forms below, answer for the calling thread:
 * what it blocks is not pending as far as it is concerned, and stays
 * set for a thread that does not block it, or for later. */
int signal64_has_pending(int pid) {
    int slot = proc64_slot_of(pid);
    if (slot < 0 || slot >= PROC64_MAX) return 0;
    return (pending_by_slot[slot] & ~*my_blocked()) != 0;
}

int signal64_take_pending(int pid) {
    int slot = proc64_slot_of(pid);
    uint64_t set;

    if (slot < 0 || slot >= PROC64_MAX) return 0;
    set = pending_by_slot[slot] & ~*my_blocked();
    if (!set) return 0;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(set & (1ull << sig))) continue;
        pending_by_slot[slot] &= ~(1ull << sig);
        return sig;
    }
    return 0;
}

int signal64_raise_task(int tid, int sig) {
    if (sig <= 0 || sig >= NSIG)            return -22;  /* -EINVAL */
    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return -3;  /* -ESRCH  */
    if (sched64_task_pid(tid) < 0)           return -3;  /* -ESRCH  */

    pending_by_task[tid] |= 1ull << sig;
    sender_by_task[tid][sig] = proc64_current_pid();
    raised++;
    return 0;
}

int signal64_has_pending_task(int tid) {
    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return 0;
    return (pending_by_task[tid] & ~blocked_by_task[tid]) != 0;
}

int signal64_take_pending_task(int tid) {
    uint64_t set;

    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return 0;
    set = pending_by_task[tid] & ~blocked_by_task[tid];
    if (!set) return 0;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(set & (1ull << sig))) continue;
        pending_by_task[tid] &= ~(1ull << sig);
        return sig;
    }
    return 0;
}

/* rt_sigtimedwait's dequeue: the lowest signal in `user_set` that is
 * pending for the calling thread, blocked or not - the thread's own
 * first, then its process's, as delivery takes them. Taken off the
 * pending set, and described: whether it was sent to the thread
 * (tgkill) or the process (kill), and by whom. 0 if none. */
int signal64_take_in_set(uint64_t user_set, int* to_thread, int* sender) {
    uint64_t want = from_user_set(user_set);
    int t = sched64_current();
    int slot = proc64_current_slot();

    for (int pass = 0; pass < 2; pass++) {
        uint64_t* set;
        if (pass == 0) {
            if (t < 0 || t >= SCHED64_MAX_TASKS) continue;
            set = &pending_by_task[t];
        } else {
            if (slot < 0 || slot >= PROC64_MAX) continue;
            set = &pending_by_slot[slot];
        }
        for (int sig = 1; sig < NSIG; sig++) {
            if (!(*set & want & (1ull << sig))) continue;
            *set &= ~(1ull << sig);
            if (to_thread) *to_thread = pass == 0;
            if (sender) *sender = pass == 0 ? sender_by_task[t][sig]
                                            : sender_by_slot[slot][sig];
            return sig;
        }
    }
    return 0;
}

/* Task slots are recycled, so a new thread must not inherit what was
 * addressed to the one that used to live in its slot. clone(2) and
 * fork(2) call this for the slot they were given. */
void signal64_clear_task(int tid) {
    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return;
    pending_by_task[tid] = 0;
}

/* What is pending without taking any of it, so that a caller about to
 * block can ask whether it should return -EINTR instead. */
uint64_t signal64_pending_set(int pid) {
    int slot = proc64_slot_of(pid);
    if (slot < 0 || slot >= PROC64_MAX) return 0;
    return pending_by_slot[slot] & ~*my_blocked();
}

uint64_t signal64_pending_set_task(int tid) {
    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return 0;
    return pending_by_task[tid] & ~blocked_by_task[tid];
}

uint64_t signal64_raised(void) { return raised; }

void signal64_reset(void) {
    for (int i = 0; i < NSIG; i++) {
        handlers[i].handler  = 0;
        handlers[i].flags    = 0;
        handlers[i].restorer = 0;
        handlers[i].mask     = 0;
    }
    /* Every slot, not just this one. The handler table above is reset
     * per process because that is what sigaction owns, but a pending
     * signal belongs to a process that may since have gone: leaving a
     * bit set means the next process to land in that slot inherits a
     * SIGTERM nobody sent it. */
    for (int i = 0; i < PROC64_MAX; i++) pending_by_slot[i] = 0;
    for (int i = 0; i < SCHED64_MAX_TASKS; i++) {
        pending_by_task[i] = 0;
        blocked_by_task[i] = 0;
        has_saved_by_task[i] = 0;
        sas_sp_by_task[i] = sas_size_by_task[i] = 0;
    }

    delivered = 0;
    returns = 0;
    raised = 0;
}

/* fork(2): the child starts with the parent's handlers.
 *
 * Linux copies the table, and a Wine process depends on it - the loader
 * installs its handlers once and every process it forks expects to
 * still have them. */
void signal64_fork(int parent_pid, int child_pid) {
    ksigaction64_t* from = handlers_of(proc64_slot_of(parent_pid));
    ksigaction64_t* to   = handlers_of(proc64_slot_of(child_pid));
    int cslot = proc64_slot_of(child_pid);

    /* The child starts with nothing pending, which is Linux's rule and
     * is also what stops a recycled slot from carrying a dead process's
     * signals into a live one. */
    if (cslot >= 0 && cslot < PROC64_MAX) pending_by_slot[cslot] = 0;

    if (from == to) return;
    for (int i = 0; i < NSIG; i++) to[i] = from[i];
}

/* execve(2): a caught signal goes back to its default, an ignored one
 * stays ignored.
 *
 * That is Linux's rule and the reason is the new image: it has never
 * seen the old program's handler addresses, and jumping to one of them
 * lands in whatever the new image happens to have at that address.
 * Ignoring survives because a program that inherited SIG_IGN is
 * entitled to keep not caring. */
void signal64_exec(void) {
    ksigaction64_t* h = handlers_of(proc64_current_slot());
    int t = my_task();

    /* The alternate stack was an address in the old image. */
    if (t >= 0) sas_sp_by_task[t] = sas_size_by_task[t] = 0;
    for (int i = 0; i < NSIG; i++) {
        if (h[i].handler == SIG64_IGN) continue;
        h[i].handler  = 0;
        h[i].flags    = 0;
        h[i].restorer = 0;
        h[i].mask     = 0;
    }
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

/* The alternate signal stack, per thread - which is what it is on
 * Linux, and what Wine assumes: every thread it starts registers its own,
 * inside its own TEB block.
 *
 * It was per process, and before that global; the per-process comment
 * said plainly that this was wrong for multithreaded programs and would
 * be wrong "in the same direction as before". It was, and this is what it
 * looked like: winedevice.exe crashing during every first wineboot, at a
 * different place each run. Each new thread's sigaltstack replaced the
 * process's, so every thread's signals were delivered onto the last
 * thread's stack. Two threads taking signals at once wrote their frames
 * over each other, and rt_sigreturn then restored somebody else's half-
 * written context: rip 0 in one thread, and rip 0x001b000000000023 - the
 * ss and cs words of a sigcontext - in another.
 *
 * ss_size 0 means none set. */
/* (sas_sp_by_task and sas_size_by_task are declared with the blocked
 * sets at the top of the file.) */

/* Is `sp` inside the calling thread's alternate stack? A handler that
 * faults again while already running on it must not be given a second
 * frame at the same place - Linux keeps delivering on the current stack
 * in that case, and so does this. */
static int on_altstack(uint64_t sp) {
    int t = my_task();
    return t >= 0 && sas_size_by_task[t] &&
           sp >= sas_sp_by_task[t] && sp < sas_sp_by_task[t] + sas_size_by_task[t];
}

/* fork and vfork: the child is a copy of the calling thread, alternate
 * stack included. Called after signal64_task_start, which clears it -
 * the right answer for a new thread, whose stack is somewhere else. */
void signal64_task_inherit_altstack(int tid, int from_tid) {
    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return;
    if (from_tid < 0 || from_tid >= SCHED64_MAX_TASKS) return;
    sas_sp_by_task[tid]   = sas_sp_by_task[from_tid];
    sas_size_by_task[tid] = sas_size_by_task[from_tid];
}

int signal64_sigaltstack(const altstack64_t* ss, altstack64_t* oss,
                         uint64_t cur_rsp) {
    int t = my_task();

    if (t < 0) return -22;                             /* -EINVAL */
    if (oss) {
        oss->ss_sp    = sas_sp_by_task[t];
        oss->ss_size  = sas_size_by_task[t];
        oss->ss_flags = on_altstack(cur_rsp) ? SS_ONSTACK
                      : (sas_size_by_task[t] ? 0 : SS_DISABLE);
        oss->__pad    = 0;
    }
    if (ss) {
        /* Changing it while running on it is what Linux refuses, and
         * for the obvious reason: the frame under the caller's feet
         * would move. */
        if (on_altstack(cur_rsp)) return -1;           /* -EPERM */
        if (ss->ss_flags & SS_DISABLE) {
            sas_sp_by_task[t] = sas_size_by_task[t] = 0;
        } else {
            if (ss->ss_size < 2048) return -12;        /* -ENOMEM */
            sas_sp_by_task[t]   = ss->ss_sp;
            sas_size_by_task[t] = ss->ss_size;
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

    /* Blocked. Every asynchronous caller has already left blocked
     * signals pending rather than come here, so this is a fault - a
     * SIGSEGV or SIGTRAP the thread itself caused - and Linux's answer
     * to a fault the thread is blocking is to kill it (force_sig takes
     * the block off and resets the handler, and the default ends the
     * process). Saying "not handled" gives the caller exactly that.
     *
     * This is the case that used to loop: a SIGSEGV handler that
     * faults, with SIGSEGV blocked because the handler is running. */
    if (*my_blocked() & (1ull << sig)) return 0;

    sa = &handlers[sig];
    if (!sa->handler) return 0;
    /* SIG_IGN is the value 1, not an address. Without this the check
     * above lets it through and the frame is built to enter the handler
     * at 1. Discarded instead, which is what ignoring means; a fault
     * signal that nothing catches still ends the process, because the
     * caller treats 0 as "nobody handled this". */
    if (sa->handler == SIG64_IGN) return 0;

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
        int t = my_task();
        if ((sa->flags & SA_ONSTACK) && t >= 0 && sas_size_by_task[t] &&
            !on_altstack(r->rsp))
            sp = sas_sp_by_task[t] + sas_size_by_task[t];
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

    /* The mask to go back to, where rt_sigreturn will look for it - and
     * where a handler that wants to change it on return can. */
    {
        /* Inside rt_sigsuspend or ppoll, the mask to return to is the
         * one the call replaced, not the temporary one - and the call is
         * over, so it is no longer saved anywhere else. */
        int t = sched64_current();
        uint64_t back = *my_blocked();
        if (t >= 0 && t < SCHED64_MAX_TASKS && has_saved_by_task[t]) {
            back = saved_by_task[t];
            has_saved_by_task[t] = 0;
        }
        frame->uc.uc_sigmask[0]       = to_user_set(back);
        frame->uc.uc_mcontext.oldmask = frame->uc.uc_sigmask[0];
    }

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

    /* For the handler's duration: what sigaction asked to block, and
     * the signal itself unless SA_NODEFER said otherwise. */
    *my_blocked() |= from_user_set(sa->mask);
    if (!(sa->flags & SA_NODEFER)) *my_blocked() |= 1ull << sig;
    *my_blocked() &= ~UNBLOCKABLE;

    /* SA_RESETHAND: one delivery, then back to the default. */
    if (sa->flags & SA_RESETHAND) {
        ksigaction64_t* h = &handlers[sig];
        h->handler = 0;
        h->flags = 0;
        h->restorer = 0;
        h->mask = 0;
    }

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

    /* The mask from before the handler ran - or whatever the handler
     * wrote there, which is a documented way to change it on return. */
    *my_blocked() = from_user_set(uc->uc_sigmask[0]);

    returns++;
    return 1;
}
