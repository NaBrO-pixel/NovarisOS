#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "idt.h"

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

typedef enum {
    PROC_READY,
    PROC_RUNNING,
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
    /* 1 if this task created that directory and must destroy it when the
     * batch is reaped. Threads sharing a sibling's directory, and tasks
     * spawned into a directory the caller owns, both leave this 0. */
    int owns_page_directory;

    process_state_t state;
} process_t;

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

/* Registered against IRQ0 (see kernel/pit.c): advances the round-robin
 * scheduler every SCHED_TICKS_PER_SLICE ticks while multitasking is
 * active. A no-op otherwise. `regs` is the interrupted process's saved
 * register frame, exactly as isr.s's shared stub built it. */
void scheduler_tick(registers_t* regs);

#endif
