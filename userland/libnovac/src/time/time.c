/* <time.h>: clocks and UTC calendar arithmetic. No time zones. */
#include <time.h>
#include <errno.h>
#include <limits.h>
#include "../internal/syscall.h"

int clock_gettime(clockid_t id, struct timespec *ts)
{ return (int)novac_sys2(NOVAC_SYS_clock_gettime, id, ts); }

int nanosleep(const struct timespec *req, struct timespec *rem)
{ return (int)novac_sys2(NOVAC_SYS_nanosleep, req, rem); }

time_t time(time_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return (time_t)-1;
    if (out) *out = ts.tv_sec;
    return ts.tv_sec;
}

double difftime(time_t end, time_t start) { return (double)end - (double)start; }

static long floor_div(long a, long b) { long q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static int is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* Days since 1970-01-01 for a proleptic Gregorian date (m = 1..12). */
static long days_from_civil(long y, int m, int d)
{
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = floor_div(y, 400);
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct tm *gmtime_r(const time_t *tp, struct tm *out)
{
    static const int cum[2][12] = {
        {0,31,59,90,120,151,181,212,243,273,304,334},
        {0,31,60,91,121,152,182,213,244,274,305,335} };
    long t, days, secs, z, era, doe, yoe, y, doy, mp;
    int m, d;
    if (!tp || !out) { errno = EINVAL; return NULL; }
    t = *tp;
    days = floor_div(t, 86400);
    secs = t - days * 86400;
    z = days + 719468;
    era = floor_div(z, 146097);
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y++;
    if (y - 1900 > INT_MAX || y - 1900 < INT_MIN) { errno = EOVERFLOW; return NULL; }
    out->tm_year = (int)(y - 1900);
    out->tm_mon = m - 1;
    out->tm_mday = d;
    out->tm_hour = (int)(secs / 3600);
    out->tm_min = (int)(secs % 3600 / 60);
    out->tm_sec = (int)(secs % 60);
    out->tm_wday = (int)(((days % 7) + 11) % 7);       /* 1970-01-01 was a Thursday */
    out->tm_yday = cum[is_leap(y)][m - 1] + d - 1;
    out->tm_isdst = 0;
    return out;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm result;               /* not reentrant, as specified */
    return gmtime_r(t, &result);
}

time_t timegm(struct tm *tm)
{
    long y, days, total;
    long mon;
    if (!tm) { errno = EINVAL; return (time_t)-1; }
    y = (long)tm->tm_year + 1900;
    mon = tm->tm_mon;
    y += floor_div(mon, 12);
    mon -= floor_div(mon, 12) * 12;
    days = days_from_civil(y, (int)mon + 1, 1);
    if (__builtin_add_overflow(days, (long)tm->tm_mday - 1, &days) ||
        __builtin_mul_overflow(days, 86400L, &total) ||
        __builtin_add_overflow(total, (long)tm->tm_hour * 3600 + (long)tm->tm_min * 60 + tm->tm_sec, &total)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    { time_t t = total; if (!gmtime_r(&t, tm)) return (time_t)-1; }   /* normalise fields */
    return total;
}

time_t mktime(struct tm *tm) { return timegm(tm); }
