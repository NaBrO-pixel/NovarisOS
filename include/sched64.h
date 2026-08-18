#ifndef SCHED64_H
#define SCHED64_H

#include <stdint.h>
#include "idt64.h"
#include "vmspace64.h"
#include "proc64.h"

/* Round-robin preemption for ring-3 tasks.
 *
 * The whole switch happens inside the timer interrupt, and it is short
 * because of where the state already is. When the CPU takes an interrupt
 * at CPL 3 it switches to the stack in the TSS and pushes the ring-3
 * ss:rsp, rflags and cs:rip; isr64.s pushes the fifteen general-purpose
 * registers after that. So by the time a handler runs, *the entire
 * user-visible state of the interrupted task is already laid out in
 * memory as a registers64_t*. Switching tasks is then: copy that struct
 * into the outgoing task, copy the incoming task's struct over it, load
 * the incoming CR3, and return. iretq reloads from the same memory the
 * handler just rewrote.
 *
 * This works because these tasks only ever run in ring 3 and are never
 * interrupted inside the kernel, so a task has no kernel stack of its
 * own to preserve. A task that can block in a syscall needs one, and
 * that is the next thing this has to grow.
 *
 * There are no priorities, no accounting and no run queue - a fixed
 * array, walked in order. */

#define SCHED64_MAX_TASKS 4

void sched64_init(void);

/* Builds the frame a task that has never run needs: cs/ss are the ring-3
 * selectors, rflags has IF set so it can be preempted at all, rdi carries
 * one argument. Returns the task index, or -1 if the table is full. */
int  sched64_add(uint64_t rip, uint64_t rsp, uint64_t arg,
                 const vmspace64_t* space);

/* Adds a task from a register set that already exists - what `clone`
 * needs, since a new thread starts as a copy of its parent rather than
 * at a fresh entry point. `fs_base` is the thread pointer: each thread
 * has its own, so the scheduler swaps it on every switch.
 *
 * Returns the task index (its tid), or -1 if the table is full. */
int  sched64_add_frame(const registers64_t* regs, const vmspace64_t* space,
                       uint64_t fs_base);

/* The same, for a thread belonging to a *different* process - which is
 * what fork produces, and the reason the two are separate calls. */
int  sched64_exit_process(int pid, registers64_t* out_regs,
                          vmspace64_t* out_space, uint64_t* out_fs_base);

int  sched64_add_frame_for(const registers64_t* regs,
                           const vmspace64_t* space,
                           uint64_t fs_base, int pid);

/* The address space and thread pointer of whatever is running, which is
 * what a thread inherits from the thread that cloned it. */
void sched64_set_current_space(const vmspace64_t* space);
const vmspace64_t* sched64_current_space(void);

/* Ends the calling thread and picks a successor.
 *
 * Returns 0 if there was none - the caller was the last thread, and the
 * process is over. Otherwise the successor's state is written out and
 * the caller is expected to hand it to sched64_resume(), because a
 * thread that has exited cannot be returned to.
 */
int sched64_exit_current(registers64_t* out_regs, vmspace64_t* out_space,
                         uint64_t* out_fs_base);

/* Blocks the calling thread on `addr` - a futex wait.
 *
 * `regs` is the frame the thread resumes from when something wakes it,
 * so it IS the thread from here on. Returns 0 if nothing else can run,
 * in which case the caller is left runnable and should report a
 * deadlock rather than stopping the machine.
 */
int sched64_block_current(const registers64_t* regs, uint64_t addr,
                          uint64_t wake_rax,
                          registers64_t* out_regs, vmspace64_t* out_space,
                          uint64_t* out_fs_base);

/* Wakes at most `max` threads blocked on `addr`; returns how many.
 * A woken thread's saved rax becomes 0, which is what futex(2) returns
 * to a waiter that was woken rather than timed out. */
int sched64_wake(uint64_t addr, int max);

/* Enters a saved thread directly. Never returns. Implemented in
 * syscall64.s, and it depends on registers64_t's exact layout. */
extern void sched64_resume(const registers64_t* regs)
    __attribute__((noreturn));

/* Called from the timer handler with the interrupted frame. */
void sched64_tick(registers64_t* frame);

/* Marks a task as the one currently running - the first task is entered
 * directly rather than switched to, so the scheduler has to be told. */
void sched64_set_current(int index);

/* After this many switches, the next task resumed is sent to `exit_rip`
 * instead of where it was. It is how a test ends after a known number of
 * ticks rather than a guessed number of iterations. 0 disables. */
void sched64_stop_after(uint64_t switches, uint64_t exit_rip);

uint64_t sched64_switches(void);
int      sched64_current(void);

#endif
