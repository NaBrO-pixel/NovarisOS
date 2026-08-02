/* posix_thread.c - clone(), futex() and thread-local storage.
 *
 * Milestone 20, and the third instalment of item 3 on the Path A list.
 * Wine is threaded, and it reaches threads the Unix way: clone() to
 * create them, futex() to make them wait, and a gs-based TLS block so
 * each one has its own errno and its own thread pointer.
 *
 * Novaris has had real preemptive threads since Milestone 16 and has run
 * Win32 programs on them since Milestone 17. What was missing was the
 * POSIX doorway to the same machinery, which is what this file is: clone
 * lands on scheduler_spawn_posix_thread(), and the scheduler that already
 * switches CR3 and the TEB descriptor now switches the TLS descriptor too.
 *
 * The one genuinely new thing is futex. Milestone 20 made waiting work
 * the way Milestone 17 made WaitForSingleObject work - rewind the trap
 * frame so the whole syscall re-executes, and yield - and said the honest
 * cost out loud: a waiting thread stayed runnable and burned its slices.
 * Milestone 25 makes it a real block, which turned out to need no new
 * machinery at all, only a task state the scheduler declines to pick. */

#include "posix.h"
#include "scheduler.h"
#include "gdt.h"
#include "console.h"
#include "kstring.h"
#include "pit.h"

/* One task cannot have more joiners than there are tasks, so this is
 * "everybody", spelled without a magic INT_MAX. */
#define MAX_JOINERS 64

/* --- clone flags, as Linux defines them --------------------------------- */
#define CLONE_VM              0x00000100u
#define CLONE_THREAD          0x00010000u
#define CLONE_SETTLS          0x00080000u
#define CLONE_PARENT_SETTID   0x00100000u
#define CLONE_CHILD_CLEARTID  0x00200000u
#define CLONE_CHILD_SETTID    0x01000000u

/* --- futex operations --------------------------------------------------- */
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK      (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

/* i386's struct user_desc: the argument to set_thread_area, and what
 * CLONE_SETTLS points at. Only the fields describing where the block is
 * are used; the permissions are implied by the ring-3 data descriptor
 * gdt_set_tls() writes. */
typedef struct {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags;
} user_desc_t;

/* Linux/i386 gives a thread three TLS descriptors, entries 6 to 8, and
 * hands out whichever is free. Novaris had exactly one and said that was
 * enough because glibc uses the first it is given - which was true right
 * up until something *else* asked for one too.
 *
 * Wine does. Its i386 code reaches the Windows TEB through fs the way
 * glibc reaches its TLS through gs, and it gets that segment by calling
 * set_thread_area(). With one descriptor to go round, Wine's request
 * overwrote glibc's and every glibc function that touched errno
 * afterwards read it out of the middle of a TEB. See include/gdt.h. */

/* Set by an implementation that wants its whole syscall re-executed
 * later; consumed by posix_syscall(). See posix_retry_pending(). */
static int retry_wanted = 0;

void posix_request_retry(void) { retry_wanted = 1; }

int posix_retry_pending(void) {
    int r = retry_wanted;
    retry_wanted = 0;
    return r;
}

/* "Allocate one for me", which is what both glibc and Wine ask. The
 * middle entry goes first so that a program that only ever needs one gets
 * the same selector it always got. Returns -1 if all three are taken. */
static int free_tls_slot(void) {
    uint32_t used = scheduler_current_tls_used();
    static const int order[GDT_TLS_COUNT] = { 7, 6, 8 };
    for (int i = 0; i < GDT_TLS_COUNT; i++) {
        if (!(used & (1u << order[i]))) return order[i];
    }
    return -1;
}

int32_t posix_sys_set_thread_area(uint32_t udesc) {
    user_desc_t* u = (user_desc_t*)udesc;
    if (!u) return -EFAULT;

    int slot;
    if (u->entry_number == 0xFFFFFFFFu) {
        slot = free_tls_slot();
        if (slot < 0) return -ESRCH;   /* what Linux says when all are taken */
        u->entry_number = (uint32_t)slot;
    } else {
        slot = (int)u->entry_number;
        if (slot < GDT_TLS_MIN || slot > GDT_TLS_MAX) return -EINVAL;
    }

    gdt_set_tls_entry(slot, u->base_addr, u->limit);
    scheduler_set_current_tls_entry(slot, u->base_addr, u->limit);
    return 0;
}

/* --- clone --------------------------------------------------------------- */

int32_t posix_sys_clone(uint32_t flags, uint32_t child_stack, uint32_t* ptid,
                        uint32_t udesc, uint32_t* ctid, registers_t* regs) {
    /* Only the thread shape is supported: a new task in the *same*
     * address space. A fork-shaped clone (no CLONE_VM) needs the address
     * space copied, which means copy-on-write, which does not exist -
     * and handing back a broken child would be far worse than saying so. */
    if (!(flags & CLONE_VM)) return -ENOSYS;
    if (!child_stack) return -EINVAL;

    /* CLONE_SETTLS points at a user_desc, and which of the three
     * descriptors it means is in entry_number - it is not always the one
     * glibc happens to use. A caller that asks for "any" here is asking
     * for the child to be given a descriptor the parent has not got, and
     * there is no sensible answer to that, so it falls back to whichever
     * is free the same way set_thread_area() picks one. */
    int tls_slot = -1;
    uint32_t tls_base = 0, tls_limit = 0;
    if ((flags & CLONE_SETTLS) && udesc) {
        user_desc_t* u = (user_desc_t*)udesc;
        tls_base = u->base_addr;
        tls_limit = u->limit;
        if (u->entry_number == 0xFFFFFFFFu) {
            tls_slot = free_tls_slot();
            if (tls_slot < 0) return -ESRCH;
        } else {
            tls_slot = (int)u->entry_number;
            if (tls_slot < GDT_TLS_MIN || tls_slot > GDT_TLS_MAX) return -EINVAL;
        }
    }

    uint32_t clear_tid = (flags & CLONE_CHILD_CLEARTID) ? (uint32_t)ctid : 0;

    /* What makes clone "return twice": the child starts at the
     * instruction after the caller's `int $0x80` - which is exactly
     * regs->eip, since the CPU already advanced past it - on the stack
     * the caller supplied. The scheduler puts 0 in the child's eax; the
     * parent gets the tid from this function's return value. */
    int tid = scheduler_spawn_posix_thread("thread", regs, child_stack,
                                           tls_slot, tls_base, tls_limit,
                                           clear_tid);
    if (tid < 0) return -ENOMEM;

    if ((flags & CLONE_PARENT_SETTID) && ptid) *ptid = (uint32_t)tid;
    if ((flags & CLONE_CHILD_SETTID) && ctid) *ctid = (uint32_t)tid;
    return tid;
}

/* Called when a thread exits. CLONE_CHILD_CLEARTID asked for this word to
 * be zeroed and its waiters woken - that pairing is precisely what
 * pthread_join is built from, and implementing it is what makes a joiner
 * stop waiting. */
void posix_thread_exiting(void) {
    uint32_t ctid = scheduler_current_clear_child_tid();
    if (!ctid) return;
    *(uint32_t*)ctid = 0;
    /* And an explicit wake, which Milestone 20 did not need: a waiter
     * used to be on the run queue re-testing its word, so zeroing it was
     * the wake. A waiter that really sleeps has to be told. This is the
     * whole of pthread_join's kernel side. */
    scheduler_wake_on(ctid, MAX_JOINERS, 0);
}

/* --- futex ---------------------------------------------------------------
 *
 * The contract is small and exact. FUTEX_WAIT sleeps only if the word
 * still holds the value the caller expected, which is what closes the
 * race between "I tested the lock" and "I went to sleep"; FUTEX_WAKE
 * wakes up to `val` waiters and reports how many it woke.
 *
 * Milestone 20 implemented waiting as a retry loop, because the scheduler
 * was believed to need "a second switch path that can suspend and resume
 * a task's kernel stack" before a task could really block. It does not.
 * The trap frame a task is about to iret from is a complete, resumable
 * snapshot of it - which is exactly why the retry loop worked - so
 * blocking is that same save plus a state the scheduler declines to pick.
 * See scheduler_block_current().
 *
 * That is not only cheaper, it is more *correct*, and in a way the old
 * tests could not see. Three things a program can observe changed:
 *
 *   - FUTEX_WAIT now returns 0 when it is woken. Under retry the syscall
 *     re-executed until the word changed and then returned -EAGAIN, so
 *     the same binary got a different answer on Linux and on Novaris.
 *   - FUTEX_WAKE returns the number of waiters it actually woke, not 0.
 *   - A timeout works. It used to be ignored, which turned a bounded wait
 *     into an unbounded one.
 *
 * The counters exist because none of the above proves the *cost* changed,
 * and the cost is the point. `futexinfo` in the shell reports them. */

static uint32_t futex_waits = 0;    /* FUTEX_WAIT calls that had to wait */
static uint32_t futex_blocks = 0;   /* ... of those, ones that really slept */
static uint32_t futex_retries = 0;  /* ... and ones that fell back to spinning */
static uint32_t futex_idles = 0;    /* ... and timed ones that idled in-kernel */
static uint32_t futex_wakes = 0;    /* tasks woken by FUTEX_WAKE */

void posix_futex_stats(uint32_t* waits, uint32_t* blocks, uint32_t* retries,
                       uint32_t* idles, uint32_t* wakes) {
    *waits = futex_waits; *blocks = futex_blocks;
    *retries = futex_retries; *idles = futex_idles; *wakes = futex_wakes;
}

void posix_futex_stats_reset(void) {
    futex_waits = futex_blocks = futex_retries = futex_idles = futex_wakes = 0;
}

/* i386 struct timespec, which is what FUTEX_WAIT's fourth argument points
 * at: a *relative* timeout for this operation. */
typedef struct { int32_t tv_sec; int32_t tv_nsec; } k_timespec_t;

/* The PIT runs at 100Hz (kernel_main's pit_install(100)), so a tick is
 * 10ms. Rounded up, and never to zero: a caller that asks for 1ns wants
 * "give up almost immediately", not "wait forever". */
/* The deadline of a timed wait that is spinning rather than blocked, so
 * re-executing the syscall does not restart its own timeout. */
static uint32_t spin_addr = 0, spin_deadline = 0;

static uint32_t timeout_to_ticks(const k_timespec_t* ts) {
    uint32_t ms = (uint32_t)ts->tv_sec * 1000u + (uint32_t)(ts->tv_nsec / 1000000);
    uint32_t ticks = (ms + 9u) / 10u;
    return ticks ? ticks : 1u;
}

int32_t posix_sys_futex(uint32_t uaddr_v, int op, uint32_t val,
                        uint32_t timeout_v, registers_t* regs) {
    uint32_t* uaddr = (uint32_t*)uaddr_v;
    if (!uaddr) return -EFAULT;

    switch (op & FUTEX_CMD_MASK) {
        case FUTEX_WAIT: {
            if (*uaddr != val) {
                /* Changed between the caller's test and this call, which
                 * is the entire reason the expected value is passed. */
                return -EAGAIN;
            }
            uint32_t deadline = 0;
            if (timeout_v) {
                const k_timespec_t* ts = (const k_timespec_t*)timeout_v;
                if (ts->tv_sec < 0 || ts->tv_nsec < 0 ||
                    ts->tv_nsec >= 1000000000) {
                    return -EINVAL;
                }
                /* The timeout is relative, so a naive implementation
                 * recomputes it from "now" - which on the spin path below
                 * means recomputing it every retry and never expiring.
                 * The deadline is therefore remembered across retries of
                 * the same wait. One slot is enough: the spin path only
                 * happens when this is the *only* runnable task. */
                if (spin_addr == uaddr_v && spin_deadline) {
                    deadline = spin_deadline;
                } else {
                    deadline = pit_get_ticks() + timeout_to_ticks(ts);
                    if (!deadline) deadline = 1;  /* 0 means "no deadline" */
                    spin_addr = uaddr_v;
                    spin_deadline = deadline;
                }
                if ((int32_t)(pit_get_ticks() - deadline) >= 0) {
                    spin_addr = 0; spin_deadline = 0;
                    return -ETIMEDOUT;
                }
            }
            futex_waits++;

            /* Park. The return value is written into this task's saved
             * frame by whoever wakes it - 0 for a FUTEX_WAKE, -ETIMEDOUT
             * for the timer - so the 0 returned here is only what the
             * frame carries until then. */
            if (scheduler_block_current(regs, uaddr_v, deadline)) {
                futex_blocks++;
                spin_addr = 0; spin_deadline = 0;
                return 0;
            }

            /* Nothing else could run: no other task exists, or every
             * other one is itself blocked. There is nobody to hand the
             * CPU to, so parking would only leave the machine with
             * nothing to do and nobody to wake anyone.
             *
             * With a deadline that is still worth doing cheaply. Idle in
             * the kernel until the time runs out or the word changes,
             * exactly as sys_nanosleep does - the syscall gate is an
             * interrupt gate, so IF has to be set by hand before hlt or
             * the timer that ends the wait can never arrive. Measured on
             * the alternative: returning to ring 3 to re-enter this
             * syscall took 1 248 002 round trips for one 1.5-second wait.
             *
             * Without a deadline there is nothing to wait *for* - no
             * other task can change the word - so this is a deadlock in
             * the program either way, and retrying keeps the old
             * behaviour rather than inventing a new way to hang. */
            if (deadline) {
                futex_idles++;
                for (;;) {
                    __asm__ __volatile__("sti");
                    if ((int32_t)(pit_get_ticks() - deadline) >= 0) {
                        __asm__ __volatile__("cli");
                        break;
                    }
                    if (*uaddr != val) {
                        __asm__ __volatile__("cli");
                        spin_addr = 0; spin_deadline = 0;
                        return -EAGAIN;
                    }
                    __asm__ __volatile__("hlt");
                    __asm__ __volatile__("cli");

                    /* Milestone 31. "Nothing else can run" was true when
                     * this loop was entered and stops being true while it
                     * runs: the tick that woke us from hlt is the same
                     * tick scheduler_expire_waits() uses to end another
                     * task's timed wait, and an IRQ can make one runnable
                     * too. Holding the CPU past that moment is not idling,
                     * it is starving the task this one is waiting *for* -
                     * which is exactly what a Wine thread waiting on a
                     * critical section held by a thread whose own timed
                     * wait had just expired did, for the whole five
                     * seconds of its timeout, three times a run.
                     *
                     * So try to park again on every tick. Once this
                     * succeeds the task is properly blocked and whoever
                     * wakes it writes the result into its frame, so there
                     * is nothing left to do here but return. */
                    if (scheduler_block_current(regs, uaddr_v, deadline)) {
                        futex_blocks++;
                        futex_idles--;   /* it slept after all */
                        spin_addr = 0; spin_deadline = 0;
                        return 0;
                    }
                }
                spin_addr = 0; spin_deadline = 0;
                return -ETIMEDOUT;
            }

            futex_retries++;
            retry_wanted = 1;
            return 0;
        }

        case FUTEX_WAKE: {
            int n = scheduler_wake_on(uaddr_v, (int)val, 0);
            futex_wakes += (uint32_t)n;
            return (int32_t)n;
        }

        default:
            return -ENOSYS;
    }
}
