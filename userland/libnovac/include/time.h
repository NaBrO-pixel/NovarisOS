#ifndef NOVAC_TIME_H
#define NOVAC_TIME_H
#include <stddef.h>
#include <sys/types.h>
#include <novac/defs.h>

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
typedef int clockid_t;

struct timespec { time_t tv_sec; long tv_nsec; };
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

NOVAC_BEGIN_DECLS
time_t time(time_t *t);
int    clock_gettime(clockid_t id, struct timespec *ts);
int    nanosleep(const struct timespec *req, struct timespec *rem);
double difftime(time_t end, time_t start);
/* UTC only: NovarisOS has no timezone database. */
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *gmtime(const time_t *t);
time_t timegm(struct tm *tm);
time_t mktime(struct tm *tm);   /* same as timegm: local time == UTC */
NOVAC_END_DECLS
#endif
