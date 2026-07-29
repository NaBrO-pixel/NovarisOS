#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* CMOS real-time clock. Added for the Win32 layer: GetSystemTime(),
 * GetLocalTime(), GetSystemTimeAsFileTime() and the CRT's time() all have
 * to return something real, and "seconds since boot" is not a date. The
 * PIT already provides elapsed time (pit_get_ticks); this provides wall
 * clock time. */

typedef struct {
    uint16_t year;    /* full year, e.g. 2026 */
    uint8_t  month;   /* 1-12 */
    uint8_t  day;     /* 1-31 */
    uint8_t  hour;    /* 0-23 */
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday; /* 0 = Sunday */
} rtc_time_t;

/* Reads the current time. The CMOS clock updates once a second and reads
 * taken mid-update return garbage, so this waits out any update in
 * progress and re-reads until two consecutive reads agree. */
void rtc_read(rtc_time_t* out);

/* The same instant as seconds since the Unix epoch. The CMOS clock has no
 * timezone, so this treats it as UTC - which is what QEMU presents by
 * default. */
uint32_t rtc_unix_time(void);

/* Days from 1970-01-01 to the given date, negative before the epoch.
 * Exposed because the Win32 FILETIME conversion needs it against a
 * different epoch (1601) than the Unix one. */
int32_t rtc_days_from_civil(int32_t year, uint32_t month, uint32_t day);

#endif
