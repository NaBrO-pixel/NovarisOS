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

/* The vector of the APs' LAPIC timer. */
#define SMP64_TIMER_VECTOR 0xF0
#define SMP64_KICK_VECTOR  0xF1   /* "something to run": wakes a halted CPU */

/* The big kernel lock (see smp64.c). No-ops until smp64_start_scheduling. */
extern volatile int smp64_active;
void smp64_enter(void);
void smp64_exit(void);
void smp64_leave_to_user(void);
int  smp64_release_all(void);
void smp64_reacquire(int depth);
void smp64_kernel_holds(void);
int  smp64_this(void);            /* this CPU, while it holds the lock */

int  smp64_pick_cpu(void);        /* the CPU for a new process */
void smp64_start_scheduling(void);
void smp64_quiesce(void);
int  smp64_quiescing(void);
void smp64_kernel_space(void);
void smp64_kick_halted(void);
void smp64_report(void);
void smp64_set_halted(int on);
void smp64_timer_tick(void* frame);
void smp64_ap_idle(void) __attribute__((noreturn));

#endif
