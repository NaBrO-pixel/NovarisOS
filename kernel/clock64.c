/* clock64.c - the tick counter, and time in Linux's units. */

#include "clock64.h"
#include "rtc.h"

static volatile uint64_t ticks;

/* Seconds since the epoch at the moment the timer started, so that
 * realtime is boot_epoch + monotonic. Read once: the CMOS clock is slow
 * to talk to and re-reading it per call would put port I/O in the path
 * of every gettimeofday. */
static uint64_t boot_epoch;

void clock64_tick(void) { ticks++; }

uint64_t clock64_ticks(void) { return ticks; }

void clock64_now(uint64_t* sec, uint64_t* nsec) {
    uint64_t t = ticks;
    if (sec)  *sec  = t / CLOCK64_HZ;
    if (nsec) *nsec = (t % CLOCK64_HZ) * (1000000000ull / CLOCK64_HZ);
}

void clock64_set_epoch_from_rtc(void) {
    /* Whatever the RTC says, minus the time already elapsed, so that
     * realtime is continuous across this call rather than jumping. */
    uint64_t now = (uint64_t)rtc_unix_time();
    uint64_t up  = ticks / CLOCK64_HZ;
    boot_epoch = (now > up) ? now - up : now;
}

void clock64_realtime(uint64_t* sec, uint64_t* nsec) {
    uint64_t t = ticks;
    if (sec)  *sec  = boot_epoch + t / CLOCK64_HZ;
    if (nsec) *nsec = (t % CLOCK64_HZ) * (1000000000ull / CLOCK64_HZ);
}
