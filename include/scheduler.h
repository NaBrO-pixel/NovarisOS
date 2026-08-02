#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "idt.h"
#include "fpu.h"
#include "gdt.h"

/* Milestone 9 - real process structures + a round-robin preemptive
 * scheduler. This is additive to process.c's existing model, not a
 * replacement: the shell's `run`/`runuser` commands still use the
 * original synchronous "one process, blocks the caller" path in
 * process.c, unchanged. This file adds a *second*, independent path -
 * `scheduler_spawn_flat()` + `scheduler_run_until_idle()` - for running
 * several ring-3 programs concurrently under real timer-driven
 * preemption, exercised by the shell's `multitask` command.
 *
 * Milestone 16 gave it address spaces and threads. Each task records the
 * page directory it runs in, and the switch path loads CR3 when the next
 * task's differs from the outgoing one - so concurrently-scheduled tasks
 * no longer have to live at distinct virtual addresses. Three copies of
 * the same program, at the same address, each isolated from the others,
 * is now a thing that works (`multitask`).
 *
 * Threads fall out of the same structure: a thread is a task that shares
 * another task's page directory instead of getting its own. Same
 * scheduler, same preemption, same synthetic-frame trick - the only
 * differences are that nothing is loaded for it, and CR3 does not change
 * when the scheduler switches between siblings (`threadtest`). */

#define PROCESS_MAX_NAME 16

/* How many things one task can be parked on at once. Twelve covers what
 * wineserver's main loop actually polls - a listening socket, a
 * connection per client and a request pipe per thread - and a set larger
 * than this simply falls back to the one-tick re-test that every poll
 * used before. */
#define PROC_WAIT_MAX 12

typedef enum {
    PROC_READY,
    PROC_RUNNING,
    /* Milestone 25: waiting on a futex word, and *not* a candidate for the
     * CPU until somebody wakes it or its deadline passes. Until then a
     * waiter stayed READY and re-tested its word every slice, which was
     * correct and wasted every one of them. */
    PROC_BLOCKED,
    PROC_ZOMBIE
} process_state_t;

typedef struct process {
    int pid;
    char name[PROCESS_MAX_NAME];

    /* Saved kernel-mode ESP for this process when it isn't the one
     * currently running: a pointer to a registers_t-shaped frame sitting
     * at the top of its own dedicated kernel stack (kernel_stack_top -
     * sizeof(registers_t)) - either a real one saved by scheduler_tick()
     * on preemption, or the synthetic one scheduler_spawn_flat() builds
     * for a never-yet-run process. See kernel/isr.s for the exact frame
     * layout this must match, and kernel/scheduler.c's top-of-file
     * comment for why the two cases can share one code path. */
    uint32_t esp;

    uint32_t kernel_stack_base; /* kmalloc'd base - for freeing */
    uint32_t kernel_stack_top;  /* fixed TSS esp0 value while this process runs */

    uint32_t load_vaddr;
    uint32_t load_pages;   /* 0 for a thread: it loaded nothing of its own */
    uint32_t stack_top;

    /* The address space this task runs in. Milestone 16: the switch path
     * loads CR3 with this when it differs from the outgoing task's, which
     * is exactly what makes two tasks at the same virtual address
     * possible - and what makes two tasks sharing one directory a pair of
     * threads rather than two processes. */
    uint32_t page_directory;
    /* This task's Thread Environment Block, or 0 for a task that has
     * none. Non-zero also means "run with fs addressing it", and makes
     * the switch path repoint the TEB GDT descriptor - see switch_to(). */
    uint32_t teb;
    /* This thread's local-storage segments, for POSIX threads: base and
     * limit of each block, and 0 for a slot the thread never asked for.
     * Reprogrammed into GDT entries 6-8 on every switch, the same way
     * `teb` is - all of them are per-thread, and all of them would
     * otherwise leak one thread's view into another's.
     *
     * Three since Milestone 30, because Linux offers three and a program
     * can need two at once: glibc puts its TLS behind gs and Wine puts
     * the Windows TEB behind fs. See include/gdt.h. */
    uint32_t tls_base[GDT_TLS_COUNT];
    uint32_t tls_limit[GDT_TLS_COUNT];

    /* CLONE_CHILD_CLEARTID: the address in the *shared* address space to
     * zero when this thread exits, and futex-wake. That is how a joiner
     * finds out the thread is gone, and is what pthread_join is built
     * from. 0 when the thread was not created with the flag. */
    uint32_t clear_child_tid;

    /* This task's x87 and SSE registers while it is not running. Live
     * state since Milestone 21 enabled SSE: glibc's string functions are
     * SSE2, so two preempted threads inside memcpy would otherwise
     * destroy each other's XMM registers. Pointer rather than inline
     * because FXSAVE requires 16-byte alignment. */
    fpu_state_t* fpu;
    uint32_t fpu_base;   /* the unaligned kmalloc pointer, for freeing */

    /* 1 if this task's user stack page was mapped for it here and is its
     * to free. A Win32 thread's stack comes from the emulation layer's
     * arena instead, which is freed wholesale when the program exits. */
    int owns_stack;
    /* 1 if this task created that directory and must destroy it when the
     * batch is reaped. Threads sharing a sibling's directory, and tasks
     * spawned into a directory the caller owns, both leave this 0. */
    int owns_page_directory;

    process_state_t state;

    /* Only meaningful while state == PROC_BLOCKED. `wait_addr` holds the
     * addresses this task is parked on - a futex word, or the kernel
     * address of a socket - and a wake naming any one of them ends the
     * wait; `wait_deadline` is a PIT tick count after which it expires
     * anyway, or 0 for "no timeout".
     *
     * Several since Milestone 31, and poll() is why. It is by definition
     * interested in a set of descriptors and could register on only one
     * of them, so everything arriving on any of the others waited for the
     * next tick. That is 10ms per wineserver round trip, and Wine's
     * console draws a Windows console one escape sequence at a time. */
    uint32_t wait_addr[PROC_WAIT_MAX];
    int      wait_count;
    uint32_t wait_deadline;
    /* 1 when this task's frame is rewound to re-run its syscall, so a
     * wake must leave eax alone. */
    int      wait_retry;
} process_t;

/* Milestone 29: a task that is a forked child rather than a thread. The
 * scheduler already knew how to run several address spaces at once; what
 * it did not have was a way to *start* one from a running task's own
 * register state, which is the whole of what fork is. */


/* One-time init: clears the process table. Safe to call once at boot,
 * before any process is spawned. */
void scheduler_init(void);

/* Maps `size` bytes of `image` (a flat, non-relocatable binary, same
 * format as process_run_flat_binary()) at `load_vaddr`, gives it a
 * one-page user stack ending at `stack_top`, and adds it to the ready
 * queue in the READY state. Does not start running it - that happens
 * when scheduler_run_until_idle() is called. Returns the new process's
 * PID (>0), or -1 on failure (table full or out of memory). */
int scheduler_spawn_flat(const char* name, const uint8_t* image, uint32_t size,
                          uint32_t load_vaddr, uint32_t stack_top);

/* Same, but in a brand-new address space of its own, which the scheduler
 * destroys when the batch is reaped. Because the task is alone in its
 * address space, `load_vaddr` and `stack_top` are free to be identical
 * across every task spawned this way. Returns -1 if an address space
 * could not be created. */
int scheduler_spawn_process(const char* name, const uint8_t* image,
                            uint32_t size, uint32_t load_vaddr,
                            uint32_t stack_top);

/* Adds another thread of execution to the *current* address space:
 * its own kernel stack, its own one-page user stack ending at
 * `stack_top`, its own register context, starting at `entry` - and
 * nothing else. Nothing is loaded and no address space is created, so
 * `entry` must already be mapped and executable, and every global the
 * code touches is shared with its siblings. That sharing is the whole
 * definition of a thread here. Returns the new PID, or -1. */
int scheduler_spawn_thread(const char* name, uint32_t entry,
                           uint32_t stack_top);

/* A thread of a Win32 program: same as above except that the caller has
 * already built the initial ring-3 stack (so `esp` is a real stack
 * pointer with the start routine's argument and return address on it,
 * not the top of a blank page), the stack belongs to the Win32 arena
 * rather than to this task, and the thread runs with fs addressing its
 * own `teb`. See kernel/win32.c's w32_thread_create(). */
int scheduler_spawn_win32_thread(const char* name, uint32_t entry,
                                 uint32_t esp, uint32_t teb);

/* A POSIX thread, created by clone(). Like the Win32 form, the caller has
 * already built the ring-3 stack, and the stack belongs to the program
 * (it came out of the program's own mmap) rather than to this task.
 *
 * The child's initial state is the *parent's* trap frame - `regs` - with
 * eip left where the caller's `int $0x80` already put it, esp pointed at
 * the caller-supplied stack, and eax zeroed, which is how clone tells
 * parent from child. Copying the frame rather than synthesising one is
 * what carries the parent's segment registers across, and fs matters:
 * Wine's i386 code runs with fs addressing a Windows TEB, and a child
 * that started with the flat 0x23 instead would fault on the first
 * fs-relative access it made.
 *
 * For the same reason the child inherits all three of the parent's TLS
 * descriptors, and CLONE_SETTLS then overwrites the one it names -
 * `tls_slot`, which is the user_desc's entry_number, not a guess. Pass
 * tls_slot < 0 for a clone that set no TLS at all. Returns the new tid,
 * or -1. */
int scheduler_spawn_posix_thread(const char* name, registers_t* regs,
                                 uint32_t esp, int tls_slot,
                                 uint32_t tls_base, uint32_t tls_limit,
                                 uint32_t clear_child_tid);

/* A forked child: a task in `child_pd` (which the caller has already
 * filled with a copy of this address space) whose initial register state
 * is the caller's own trap frame with eax = 0. That single difference is
 * what makes fork return twice.
 *
 * The child owns `child_pd` and the scheduler destroys it when the batch
 * is reaped. It owns no image and no stack of its own: every page it has
 * is a copy that lives in that directory. Returns the new task's pid, or
 * -1 if the table is full or a kernel stack could not be allocated. */
int scheduler_fork_current(registers_t* regs, uint32_t child_pd,
                           const char* name);

/* Points a just-forked task's initial stack somewhere other than where
 * its parent's was. A plain fork resumes the child on its own copy of the
 * parent's stack; a clone that supplied one - glibc's posix_spawn does -
 * runs it on that instead. `tid` is what scheduler_fork_current()
 * returned. */
void scheduler_fork_set_stack(int tid, uint32_t esp);

/* Adopts `pd` as the current task's address space - what execve does
 * after replacing the image in place. The task keeps its identity; only
 * the contents of the space it runs in changed, so this only exists to
 * clear the TLS and TEB descriptors the old image had. */
void scheduler_exec_current(void);

/* How many tasks other than the current one are still alive in address
 * space `pd`. Zero means the caller is the last thread of that process,
 * which is when its memory can be released and its exit status recorded. */
int scheduler_as_sibling_count(uint32_t pd);

/* The address space of the running task, or 0. Same value as
 * paging_current_address_space() while a task is running, and the honest
 * answer rather than CR3 when the kernel has switched away. */
uint32_t scheduler_current_address_space(void);

/* Records the current task's TLS block for GDT entry `slot`, so a switch
 * away and back reprograms the descriptor with this thread's values
 * rather than whichever thread last called set_thread_area. */
void scheduler_set_current_tls_entry(int slot, uint32_t base, uint32_t limit);
void scheduler_set_current_tls(uint32_t base, uint32_t limit);

/* Which entries the current task has already claimed, as a bitmask over
 * slots GDT_TLS_MIN..GDT_TLS_MAX. set_thread_area() asks so it can hand
 * out a free one rather than overwriting a live one. */
uint32_t scheduler_current_tls_used(void);

/* The clear_child_tid of the task that is exiting, or 0. Read by the
 * POSIX layer on thread exit so it can zero the word and wake anyone
 * waiting on it. */
uint32_t scheduler_current_clear_child_tid(void);

/* Runs `entry` as the only task in the current address space and blocks
 * until it (and anything it clones) has finished - the scheduler-based
 * equivalent of process_run_user_mode(). This is what lets an ordinary
 * program call clone(): its main thread has to be a scheduler task before
 * a second one can join it. */
void scheduler_run_single(const char* name, uint32_t entry, uint32_t esp);

/* Starts round-robin preemptive multitasking among every process spawned
 * since the last call, and blocks the caller (same "looks like an
 * ordinary function call" trick process.c uses for the single-process
 * case) until every one of them has called sys_exit. Frees their kernel
 * stacks and resets the table before returning, so it's safe to spawn
 * and run another fresh batch afterward. No-op if nothing was spawned. */
void scheduler_run_until_idle(void);

/* True while scheduler_run_until_idle() is actively multitasking. Lets
 * the SYS_EXIT syscall handler (kernel/syscall.c) tell whether it should
 * terminate the current process into the scheduler's ready queue
 * (this path) or fall back to the older single-process blocking path in
 * process.c (process_exit_to_kernel()). */
int scheduler_is_active(void);

/* Called only from the SYS_EXIT syscall handler while scheduler_is_active()
 * is true. Marks the current process a zombie and switches to the next
 * ready one - or, if that was the last one, unwinds back to whoever
 * called scheduler_run_until_idle(). Returns normally in the common
 * case (the actual stack switch happens in isr.s's shared epilogue,
 * after this returns all the way back up through the interrupt
 * dispatch); does not return at all in the "last process" case. */
void scheduler_exit_current(void);

/* The PID of the task currently running, or 0 outside multitasking. */
int scheduler_current_pid(void);

/* Whether `pid` names a task that has not yet exited. An unknown PID
 * reads as finished rather than as never-finishing, so waiting on a
 * handle whose thread has already been reaped returns immediately
 * instead of hanging. */
int scheduler_task_alive(int pid);

/* Gives up the rest of this task's time slice from inside a trap, using
 * `regs` as the resume point. Returns 1 if it switched, 0 if there was
 * nothing else runnable.
 *
 * The resume point is the whole trick. `regs` is the task's complete
 * ring-3 state, so a caller that rewinds regs->eip to re-execute the
 * instruction that trapped in gets a retry loop with other threads
 * running in between - which is how Milestone 17 implements waiting
 * (WaitForSingleObject, EnterCriticalSection) without needing the
 * scheduler to be able to block a task mid-syscall and resume its kernel
 * stack later. Milestone 17 used it for waiting, and Milestone 25's
 * blocking futex is built on the same observation - see
 * scheduler_block_current(). */
int scheduler_yield_from_trap(registers_t* regs);

/* --- blocking (Milestone 25) --------------------------------------------
 *
 * The roadmap said real blocking needed "a second switch path that can
 * suspend and resume a task's kernel stack". It does not. The trap frame
 * a task is about to iret from is already a complete, resumable snapshot
 * of it - which is exactly why yield_from_trap works - so blocking is
 * that same save plus a state the scheduler declines to pick.
 *
 * Parks the current task on `wait_addr` with `regs` as its resume point,
 * and switches away. `deadline` is an absolute PIT tick count, or 0 for
 * an indefinite wait.
 *
 * Returns 1 if it blocked. Returns 0 - having changed nothing - when no
 * other task could run, because parking the only runnable task would
 * wedge the machine with nobody left to wake it. The caller falls back to
 * retrying, which is what every waiter did before this existed and costs
 * nothing when there is no one else to give the CPU to. */
int scheduler_block_current(registers_t* regs, uint32_t wait_addr,
                            uint32_t deadline);

/* The same, for a caller whose trap frame is already rewound to
 * re-execute the whole syscall (Milestone 28). Waking such a task must
 * *not* write a result into eax - eax holds the syscall number, and
 * overwriting it would send the re-executed `int $0x80` to whatever that
 * value happened to name.
 *
 * `deadline` is an absolute PIT tick count, or 0 for an indefinite wait. */
int scheduler_block_current_retry(registers_t* regs, uint32_t wait_addr,
                                  uint32_t deadline);

/* The same again, for a caller waiting on a *set* of addresses - poll's
 * shape. A wake naming any of the first `n` (up to PROC_WAIT_MAX) ends
 * the wait. `n` of 0 with a deadline is a pure sleep. */
int scheduler_block_current_retry_multi(registers_t* regs,
                                        const uint32_t* addrs, int n,
                                        uint32_t deadline);

/* Wakes up to `max` tasks blocked on `addr`, in table order. Each one's
 * saved trap frame gets `result` in eax, so the syscall it blocked inside
 * returns that. Returns how many were woken - which is what FUTEX_WAKE
 * reports, and callers do use it. */
int scheduler_wake_on(uint32_t addr, int max, int32_t result);

/* The same, restricted to tasks sharing the caller's address space. Every
 * caller that names a *user* address - a futex word, a thread's clear-tid
 * word - must use this: those addresses are virtual, and two processes
 * routinely have different words at the same one. The unscoped form is
 * for kernel object addresses, which are unique across the machine and
 * whose waiters are deliberately in other processes. */
int scheduler_wake_on_local(uint32_t addr, int max, int32_t result);

/* Called from the timer. Wakes any blocked task whose deadline has
 * passed, with `result` in eax (-ETIMEDOUT). Returns how many. */
int scheduler_expire_waits(uint32_t now, int32_t result);

/* How many tasks are blocked right now. Used by the shell's `futexinfo`
 * to show the state rather than only the counters. */
/* The address space a task is running in, by task id, or 0 if there is no
 * such live task. A *thread's* id, which is what tgkill and wineserver's
 * kill() both name, is a task id here - and turning one into the process
 * it belongs to is the only way a signal aimed at another process's
 * thread can be delivered to that process rather than to the sender. */
uint32_t scheduler_address_space_of(int pid);

int scheduler_blocked_count(void);

/* Whether scheduler_yield_from_trap() would actually switch - i.e.
 * whether any task other than the current one is runnable. Lets a caller
 * decide between yielding and waiting some other way, without having to
 * rewind its trap frame speculatively. */
int scheduler_yield_would_switch(void);

/* Ends every task running in address space `pd` - which is every thread
 * of one process, and nothing else. An unhandled fault takes the whole
 * process with it, the way an unhandled SIGSEGV does on Linux, but must
 * leave the other processes of the batch alone: when Wine's client
 * crashes, wineserver is still a running program.
 *
 * Returns normally if other tasks remain (the switch happens in isr.s's
 * epilogue as the call chain unwinds); does not return if that was the
 * last of them, in which case control goes back to whoever called
 * scheduler_run_until_idle(). */
void scheduler_exit_address_space(uint32_t pd);

/* Ends every task in the batch at once and unwinds to whoever called
 * scheduler_run_until_idle(). This is process termination as opposed to
 * thread termination: ExitProcess, and an unhandled exception, both take
 * every thread of the program with them.
 *
 * Does not return when the scheduler is running, which is the only case
 * a caller should reach it in - hence no `noreturn` attribute: it returns
 * immediately, having done nothing, if called while nothing is being
 * scheduled, so a caller that got its state wrong gets a no-op rather
 * than an unwind into a stale saved stack. */
void scheduler_terminate_all(void);

/* Registered against IRQ0 (see kernel/pit.c): advances the round-robin
 * scheduler every SCHED_TICKS_PER_SLICE ticks while multitasking is
 * active. A no-op otherwise. `regs` is the interrupted process's saved
 * register frame, exactly as isr.s's shared stub built it. */
void scheduler_tick(registers_t* regs);

#endif
