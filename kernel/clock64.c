/* clock64.c - the tick counter, and time in Linux's units. */

#include "clock64.h"

static volatile uint64_t ticks;

void clock64_tick(void) { ticks++; }

uint64_t clock64_ticks(void) { return ticks; }

void clock64_now(uint64_t* sec, uint64_t* nsec) {
    uint64_t t = ticks;
    if (sec)  *sec  = t / CLOCK64_HZ;
    if (nsec) *nsec = (t % CLOCK64_HZ) * (1000000000ull / CLOCK64_HZ);
}
