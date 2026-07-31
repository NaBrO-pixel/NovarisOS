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
 * The one genuinely new thing is futex. Waiting works the way Milestone
 * 17 made WaitForSingleObject work - rewind the trap frame so the whole
 * syscall re-executes, and yield - because the scheduler resumes tasks
 * from a trap frame and cannot suspend one mid-syscall. The honest cost
 * is unchanged and worth repeating: a waiting thread stays runnable and
 * burns its slices retrying rather than sleeping. */

#include "posix.h"
#include "scheduler.h"
#include "gdt.h"
#include "console.h"
#include "kstring.h"

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

/* Novaris has exactly one TLS descriptor (GDT entry 7), reprogrammed on
 * every thread switch. Linux offers three (entries 6-8) and glibc uses
 * the first it is given, so answering with a fixed one is enough - and
 * refusing a request for a second is more honest than handing back a
 * descriptor that quietly aliases the first. */
#define TLS_ENTRY_NUMBER 7

/* Set by an implementation that wants its whole syscall re-executed
 * later; consumed by posix_syscall(). See posix_retry_pending(). */
static int retry_wanted = 0;

int posix_retry_pending(void) {
    int r = retry_wanted;
    retry_wanted = 0;
    return r;
}

int32_t posix_sys_set_thread_area(uint32_t udesc) {
    user_desc_t* u = (user_desc_t*)udesc;
    if (!u) return -EFAULT;

    if (u->entry_number == 0xFFFFFFFFu) {
        u->entry_number = TLS_ENTRY_NUMBER;   /* "allocate one for me" */
    } else if (u->entry_number != TLS_ENTRY_NUMBER) {
        return -EINVAL;
    }

    gdt_set_tls(u->base_addr, u->limit);
    scheduler_set_current_tls(u->base_addr, u->limit);
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

    uint32_t tls_base = 0, tls_limit = 0;
    if ((flags & CLONE_SETTLS) && udesc) {
        user_desc_t* u = (user_desc_t*)udesc;
        tls_base = u->base_addr;
        tls_limit = u->limit;
    }

    uint32_t clear_tid = (flags & CLONE_CHILD_CLEARTID) ? (uint32_t)ctid : 0;

    /* What makes clone "return twice": the child starts at the
     * instruction after the caller's `int $0x80` - which is exactly
     * regs->eip, since the CPU already advanced past it - on the stack
     * the caller supplied. The scheduler puts 0 in the child's eax; the
     * parent gets the tid from this function's return value. */
    int tid = scheduler_spawn_posix_thread("thread", regs->eip, child_stack,
                                           tls_base, tls_limit, clear_tid);
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
    /* No explicit wake is needed: every futex waiter is on the run queue
     * re-testing its word, so zeroing it is the wake. */
}

/* --- futex ---------------------------------------------------------------
 *
 * The contract is small and exact. FUTEX_WAIT sleeps only if the word
 * still holds the value the caller expected, which is what closes the
 * race between "I tested the lock" and "I went to sleep"; FUTEX_WAKE
 * wakes up to `val` waiters.
 *
 * Retry-based waiting means there is no wait queue: a waiter re-tests the
 * word every time it is scheduled. So FUTEX_WAKE has nothing to wake - it
 * only has to report a plausible count, which callers do use. */

static uint32_t futex_waiters = 0;

int32_t posix_sys_futex(uint32_t uaddr_v, int op, uint32_t val) {
    uint32_t* uaddr = (uint32_t*)uaddr_v;
    if (!uaddr) return -EFAULT;

    switch (op & FUTEX_CMD_MASK) {
        case FUTEX_WAIT:
            if (*uaddr != val) {
                /* Changed between the caller's test and this call, which
                 * is the entire reason the expected value is passed. */
                return -EAGAIN;
            }
            /* Re-execute the whole call when next scheduled, and re-test
             * the word then. Nothing above has been modified, so running
             * it again is harmless. */
            retry_wanted = 1;
            return 0;

        case FUTEX_WAKE: {
            uint32_t woken = futex_waiters < val ? futex_waiters : val;
            return (int32_t)woken;
        }

        default:
            return -ENOSYS;
    }
}
