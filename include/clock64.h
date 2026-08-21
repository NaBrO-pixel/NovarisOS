#ifndef CLOCK64_H
#define CLOCK64_H

#include <stdint.h>

/* What time it is, to the extent that this machine knows.
 *
 * The PIT is programmed to ~1kHz in kmain64 and its handler already
 * counted ticks; this only gives that counter a name the syscall layer
 * can reach and a conversion into the units Linux reports.
 *
 * There is no wall clock here - no RTC is read, and the epoch is the
 * moment the timer was installed rather than 1970. That is a real
 * divergence and it is deliberate: a made-up date is worse than an
 * obviously small one, because a program that stores it will produce
 * files stamped with a lie rather than files that are visibly from a
 * machine with no clock. What matters to the wineserver is that time
 * *advances* and that two readings can be subtracted, and both are
 * true.
 */

#define CLOCK64_HZ 1000u

/* Called from the timer interrupt. */
void     clock64_tick(void);

uint64_t clock64_ticks(void);

/* Seconds and nanoseconds since the timer started. */
void     clock64_now(uint64_t* sec, uint64_t* nsec);

#endif
