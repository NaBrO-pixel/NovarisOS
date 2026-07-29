#ifndef PIT_H
#define PIT_H

#include <stdint.h>

/* Programs PIT channel 0 to fire IRQ0 at the given frequency (Hz) and
 * registers a handler that counts ticks. Needed for any kind of delay,
 * scheduling, or "uptime" reporting later on. */
void pit_install(uint32_t frequency_hz);

/* Number of timer ticks since pit_install() was called. */
uint32_t pit_get_ticks(void);

/* Busy-halts (via hlt, waking only for interrupts) until `ticks` more
 * timer ticks have elapsed. */
void pit_sleep_ticks(uint32_t ticks);

#endif
