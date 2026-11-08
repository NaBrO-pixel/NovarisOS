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

#endif
