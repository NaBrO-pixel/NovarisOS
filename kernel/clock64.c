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

/* The time, from the TSC once it has been measured against the PIT.
 *
 * Counting PIT interrupts loses time: QEMU merges a tick that arrives
 * while the last one is still pending, so every stretch of more than a
 * millisecond with interrupts off - a long syscall, or a CPU spinning for
 * the kernel lock - drops ticks. A four-CPU Chromium run counted 134
 * seconds of clock across 268 of real time, and every timeout in it ran
 * twice as long as asked. The TSC keeps counting through all of that, and
 * QEMU's is the host's, the same on every CPU. */
static uint64_t tsc_per_ms, tsc_base, tick_base;

static inline uint64_t clock_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void clock64_use_tsc(uint64_t per_ms) {
    if (!per_ms) return;
    tick_base = ticks;
    tsc_base  = clock_tsc();
    __atomic_store_n(&tsc_per_ms, per_ms, __ATOMIC_RELEASE);
}

uint64_t clock64_tsc_per_ms(void) { return tsc_per_ms; }

uint64_t clock64_ticks(void) {
    uint64_t r = __atomic_load_n(&tsc_per_ms, __ATOMIC_ACQUIRE);
    if (!r) return ticks;
    return tick_base + (clock_tsc() - tsc_base) / r;
}

void clock64_now(uint64_t* sec, uint64_t* nsec) {
    uint64_t t = clock64_ticks();
    if (sec)  *sec  = t / CLOCK64_HZ;
    if (nsec) *nsec = (t % CLOCK64_HZ) * (1000000000ull / CLOCK64_HZ);
}

void clock64_set_epoch_from_rtc(void) {
    /* Whatever the RTC says, minus the time already elapsed, so that
     * realtime is continuous across this call rather than jumping. */
    uint64_t now = (uint64_t)rtc_unix_time();
    uint64_t up  = clock64_ticks() / CLOCK64_HZ;
    boot_epoch = (now > up) ? now - up : now;
}

void clock64_realtime(uint64_t* sec, uint64_t* nsec) {
    uint64_t t = clock64_ticks();
    if (sec)  *sec  = boot_epoch + t / CLOCK64_HZ;
    if (nsec) *nsec = (t % CLOCK64_HZ) * (1000000000ull / CLOCK64_HZ);
}
