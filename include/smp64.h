#ifndef SMP64_H
#define SMP64_H

#include <stdint.h>

/* smp64 - the other processors. See kernel/smp64.c. */

#define SMP64_MAX_CPUS 8

/* Finds and starts the application processors. Needs the PIT clock
 * running with interrupts on (it waits). Returns how many CPUs there are,
 * this one included. */
int      smp64_init(void);
int      smp64_cpus(void);
uint32_t smp64_lapic_id(void);
void     smp64_lapic_eoi(void);
int      smp64_idle_here(void);   /* halted with nothing to run */
void     smp64_idle_irq(int vector);

/* The vector of the APs' LAPIC timer. */
#define SMP64_TIMER_VECTOR 0xF0
#define SMP64_KICK_VECTOR  0xF1   /* "something to run": wakes a halted CPU */
#define SMP64_SAMPLE_VECTOR 0xF2  /* the bootstrap CPU's sampling timer */
#define SMP64_FLUSH_VECTOR  0xF3  /* TLB shootdown (Milestone 105) */
void smp64_sample_irq(void* frame);

/* The big kernel lock (see smp64.c). No-ops until smp64_start_scheduling. */
extern volatile int smp64_active;
void smp64_enter(void);
void smp64_exit(void);
void smp64_leave_to_user(void);
int  smp64_release_all(void);
void smp64_reacquire(int depth);
void smp64_kernel_holds(void);
int  smp64_this(void);            /* this CPU */
int  smp64_held_here(void);       /* this CPU holds the kernel lock */

int  smp64_pick_cpu(void);        /* the CPU for a new process */
int  smp64_pick_cpu_for(int self);  /* after an exec; -1 to stay put */
void smp64_start_scheduling(void);
void smp64_quiesce(void);
int  smp64_quiescing(void);
void smp64_kernel_space(void);
void smp64_kick_halted(void);
void smp64_report(void);
void smp64_set_halted(int on);
void smp64_timer_tick(void* frame);
struct registers64;
void smp64_note_tick(int cpu, const void* frame, int ms);
void smp64_ap_idle(void) __attribute__((noreturn));

/* Threads of one process on several CPUs (Milestone 105). */
int  smp64_spread_threads(void);
void smp64_note_cr3(uint64_t pml4);
void smp64_note_wake(int cpu);
void smp64_shootdown(uint64_t pml4);
int  smp64_flush_irq(void);
int  smp64_take_dead_pending(void);
void smp64_kernel_space_now(void);
int  smp64_defer_free(uint64_t phys);
void smp64_mm_dirty(void);
int  smp64_pick_thread_cpu(int pid, int home);
void smp64_run_ended_elsewhere(void) __attribute__((noreturn));
void smp64_bsp_abort_check(void);
void smp64_spread_report(void);
struct proc64;
void smp64_mm_hold_current(void);
int  smp64_dead_pending_here(void);
int  smp64_idle_has_work(void);

#endif
