/* clock64.c - which clock is which, and what an absolute deadline means.
 *
 * Milestone 86. Two bugs in this kernel's clock syscalls, both of the
 * same shape: an argument that selects behaviour was not read, so every
 * call got one behaviour and the wrong callers got the wrong one.
 *
 *   - clock_gettime answered CLOCK_REALTIME_COARSE from the uptime
 *     counter, because the list of ids that mean "a date" said 8 where
 *     it should have said 5. That is not an obscure id. Wine's
 *     NtQuerySystemTime asks clock_getres whether REALTIME_COARSE
 *     resolves to a millisecond or better and, if it does, uses that
 *     clock for every date it reports for the rest of the process's
 *     life. So every date Wine read was 1970 plus a few seconds, while
 *     the wineserver - which calls gettimeofday - read the RTC and had
 *     the real year. The two disagreed by fifty-six years, and since
 *     Wine hands the server absolute deadlines of "now plus the
 *     timeout", every timed wait in the prefix arrived already expired
 *     and came straight back. One of those waits is the one that gives
 *     explorer time to create the desktop window, which is why this
 *     showed up as twelve missing-display-driver errors.
 *
 *   - clock_nanosleep ignored its flags argument, so TIMER_ABSTIME was
 *     silently treated as a relative sleep. "Wake at 09:15" became
 *     "sleep for 09:15 from now". On CLOCK_REALTIME that is a sleep of
 *     fifty-six years; on CLOCK_MONOTONIC it is a sleep of the machine's
 *     uptime. Nothing in the Wine prefix calls it today, which is
 *     exactly why it needed a test of its own - it would have been found
 *     by whatever broke first and blamed on something else.
 *
 * Also here: clock_getres used to answer for any id at all, valid or
 * not, which tells a caller that a clock it does not have is present.
 *
 * The value of running this on Linux too is that none of the expected
 * answers below are this test's opinion. They are what Linux does. The
 * calls go through syscall(2) rather than through glibc's wrappers so
 * that what is compared is the kernel's ABI and not the library's
 * caching of it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE 5
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE 6
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

/* An id no clock uses. Linux's table tops out well below this and
 * answers EINVAL for anything past it. */
#define CLOCK_NONSENSE 99

/* 2020-01-01. A date is after it; an uptime is not. The whole of the
 * first bug is the difference between those two statements, and this is
 * the line that says so without printing a number that would differ
 * between the two machines. */
#define YEAR_2020 1577836800L

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

static int get(int id, struct timespec *ts)
{
    return (int)syscall(SYS_clock_gettime, id, ts);
}

static int res(int id, struct timespec *ts)
{
    return (int)syscall(SYS_clock_getres, id, ts);
}

static int nsleep(int id, int flags, const struct timespec *req,
                  struct timespec *rem)
{
    return (int)syscall(SYS_clock_nanosleep, id, flags, req, rem);
}

/* Milliseconds between two readings of the same clock. */
static long ms_between(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000L
         + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

int main(void)
{
    struct timespec r, t, before, after, deadline, rem;
    long waited;
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* --- clock_getres knows which clocks exist ----------------------- */

    ok("clock_getres answers for CLOCK_REALTIME", res(CLOCK_REALTIME, &r) == 0);
    ok("clock_getres answers for CLOCK_MONOTONIC", res(CLOCK_MONOTONIC, &r) == 0);
    ok("clock_getres answers for CLOCK_REALTIME_COARSE",
       res(CLOCK_REALTIME_COARSE, &r) == 0);
    ok("clock_getres answers for CLOCK_BOOTTIME", res(CLOCK_BOOTTIME, &r) == 0);

    /* The resolution itself is the machine's, so it is not compared
     * across the two - only that it is a well-formed timespec. */
    res(CLOCK_REALTIME, &r);
    ok("and the resolution it reports is a valid timespec",
       r.tv_sec >= 0 && r.tv_nsec >= 0 && r.tv_nsec < 1000000000L);

    /* A NULL res is legal: the call is then only asking whether the
     * clock is there. */
    ok("clock_getres(valid, NULL) succeeds", res(CLOCK_REALTIME, NULL) == 0);

    errno = 0;
    rc = res(CLOCK_NONSENSE, &r);
    ok("clock_getres refuses a clock that does not exist",
       rc == -1 && errno == EINVAL);

    errno = 0;
    rc = get(CLOCK_NONSENSE, &t);
    ok("clock_gettime refuses a clock that does not exist",
       rc == -1 && errno == EINVAL);

    /* --- a date is a date, and an interval is an interval ------------- */

    ok("CLOCK_REALTIME reads as a date", get(CLOCK_REALTIME, &t) == 0
       && t.tv_sec > YEAR_2020);

    /* The one that was wrong. It has to be a date for the same reason
     * CLOCK_REALTIME does - it is the same clock, read cheaply. */
    ok("CLOCK_REALTIME_COARSE reads as a date",
       get(CLOCK_REALTIME_COARSE, &t) == 0 && t.tv_sec > YEAR_2020);

    ok("CLOCK_MONOTONIC reads as an interval, not a date",
       get(CLOCK_MONOTONIC, &t) == 0 && t.tv_sec >= 0 && t.tv_sec < YEAR_2020);
    ok("CLOCK_BOOTTIME reads as an interval, not a date",
       get(CLOCK_BOOTTIME, &t) == 0 && t.tv_sec >= 0 && t.tv_sec < YEAR_2020);

    /* Same clock read two ways: they must agree to within a second or
     * two, which is a much sharper statement than "both are dates" and
     * is the one the fifty-six-year gap broke. */
    get(CLOCK_REALTIME, &before);
    get(CLOCK_REALTIME_COARSE, &after);
    waited = ms_between(&before, &after);
    ok("REALTIME and REALTIME_COARSE agree to within two seconds",
       waited > -2000 && waited < 2000);

    get(CLOCK_MONOTONIC, &before);
    get(CLOCK_MONOTONIC, &after);
    ok("CLOCK_MONOTONIC does not go backwards",
       ms_between(&before, &after) >= 0);

    /* --- clock_nanosleep, relative ------------------------------------ */

    t.tv_sec = 0;
    t.tv_nsec = 50000000L;                             /* 50ms */
    rem.tv_sec = rem.tv_nsec = -1;
    get(CLOCK_MONOTONIC, &before);
    ok("a relative clock_nanosleep returns 0",
       nsleep(CLOCK_MONOTONIC, 0, &t, &rem) == 0);
    get(CLOCK_MONOTONIC, &after);
    waited = ms_between(&before, &after);
    ok("and it really waited about 50ms", waited >= 40 && waited < 5000);
    /* Linux fills the remainder in only when a signal cut the sleep
     * short, and otherwise leaves it exactly as the caller left it. This
     * was written the other way round first, on the assumption that a
     * completed sleep reports a remainder of zero, and the host said
     * no. */
    ok("and it left the remainder alone, having slept the whole time",
       rem.tv_sec == -1 && rem.tv_nsec == -1);

    /* --- clock_nanosleep, absolute ------------------------------------ *
     *
     * The deadline is a point on the named clock, not a length. Read the
     * clock, add 50ms, and sleep until then.
     *
     * The lower bound says the sleep happened; the upper bound is the
     * one that catches the bug. Read as a length, a MONOTONIC deadline
     * of now+50ms is a sleep of the machine's whole uptime, and a
     * REALTIME one is a sleep of every second since 1970. Neither of
     * those finishes inside five seconds, or inside a working day. */

    get(CLOCK_MONOTONIC, &before);
    deadline = before;
    deadline.tv_nsec += 50000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }
    ok("an absolute clock_nanosleep on MONOTONIC returns 0",
       nsleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == 0);
    get(CLOCK_MONOTONIC, &after);
    waited = ms_between(&before, &after);
    ok("and it woke at the deadline, not uptime later",
       waited >= 40 && waited < 5000);

    get(CLOCK_REALTIME, &before);
    deadline = before;
    deadline.tv_nsec += 50000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }
    ok("an absolute clock_nanosleep on REALTIME returns 0",
       nsleep(CLOCK_REALTIME, TIMER_ABSTIME, &deadline, NULL) == 0);
    get(CLOCK_REALTIME, &after);
    waited = ms_between(&before, &after);
    ok("and it woke at the deadline, not in the next century",
       waited >= 40 && waited < 5000);

    /* A deadline already gone is not an error and is not a sleep. On
     * REALTIME, one second past the epoch is fifty-six years ago - and
     * read as a length it would be fifty-six years of sleeping. */
    deadline.tv_sec = 1;
    deadline.tv_nsec = 0;
    get(CLOCK_REALTIME, &before);
    ok("an absolute REALTIME deadline in 1970 returns 0 at once",
       nsleep(CLOCK_REALTIME, TIMER_ABSTIME, &deadline, NULL) == 0);
    get(CLOCK_REALTIME, &after);
    ok("and really did not wait", ms_between(&before, &after) < 5000);

    get(CLOCK_MONOTONIC, &before);
    deadline.tv_sec = 0;
    deadline.tv_nsec = 0;
    ok("an absolute MONOTONIC deadline of zero returns 0 at once",
       nsleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == 0);
    get(CLOCK_MONOTONIC, &after);
    ok("and really did not wait either",
       ms_between(&before, &after) < 5000);

    /* --- what clock_nanosleep refuses --------------------------------- */

    t.tv_sec = 0;
    t.tv_nsec = 0;
    errno = 0;
    rc = nsleep(CLOCK_NONSENSE, 0, &t, NULL);
    ok("clock_nanosleep refuses a clock that does not exist",
       rc == -1 && errno == EINVAL);

    /* Bits outside TIMER_ABSTIME are ignored, not refused. Measured on
     * the host rather than assumed: this was written as a refusal first,
     * and Linux slept and returned 0. */
    ok("clock_nanosleep ignores a flag it does not know",
       nsleep(CLOCK_MONOTONIC, 0x40, &t, NULL) == 0);

    printf("clock64: %d failures\n", failures);
    return failures ? 1 : 127;
}
