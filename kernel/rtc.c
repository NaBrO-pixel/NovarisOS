#include "rtc.h"
#include "io.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

#define CMOS_SECOND   0x00
#define CMOS_MINUTE   0x02
#define CMOS_HOUR     0x04
#define CMOS_WEEKDAY  0x06
#define CMOS_DAY      0x07
#define CMOS_MONTH    0x08
#define CMOS_YEAR     0x09
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
    /* Bit 7 of the index port disables NMI while we're mid-read, which is
     * the conventional way to keep an NMI from landing between the index
     * write and the data read and leaving the index register pointing
     * somewhere unexpected. */
    outb(CMOS_ADDRESS, (uint8_t)(0x80 | reg));
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(CMOS_STATUS_A) & 0x80;
}

static uint8_t from_bcd(uint8_t value) {
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

void rtc_read(rtc_time_t* out) {
    rtc_time_t first;
    uint8_t status_b;

    /* Read twice and require agreement: the clock ticks over between the
     * individual register reads otherwise, and 10:59:59 can be sampled as
     * 10:59:00 (minutes read before the tick, seconds after). */
    for (;;) {
        while (update_in_progress()) { }

        first.second  = cmos_read(CMOS_SECOND);
        first.minute  = cmos_read(CMOS_MINUTE);
        first.hour    = cmos_read(CMOS_HOUR);
        first.weekday = cmos_read(CMOS_WEEKDAY);
        first.day     = cmos_read(CMOS_DAY);
        first.month   = cmos_read(CMOS_MONTH);
        first.year    = cmos_read(CMOS_YEAR);

        while (update_in_progress()) { }

        if (first.second  == cmos_read(CMOS_SECOND) &&
            first.minute  == cmos_read(CMOS_MINUTE) &&
            first.hour    == cmos_read(CMOS_HOUR) &&
            first.day     == cmos_read(CMOS_DAY) &&
            first.month   == cmos_read(CMOS_MONTH) &&
            first.year    == cmos_read(CMOS_YEAR)) {
            break;
        }
    }

    status_b = cmos_read(CMOS_STATUS_B);

    /* Bit 2 clear means the values are BCD-encoded; bit 1 clear means
     * 12-hour time, with bit 7 of the hour register as the PM flag. */
    int pm = 0;
    if (!(status_b & 0x02) && (first.hour & 0x80)) {
        pm = 1;
        first.hour &= 0x7F;
    }

    if (!(status_b & 0x04)) {
        first.second  = from_bcd(first.second);
        first.minute  = from_bcd(first.minute);
        first.hour    = from_bcd(first.hour);
        first.weekday = from_bcd(first.weekday);
        first.day     = from_bcd(first.day);
        first.month   = from_bcd(first.month);
        first.year    = from_bcd((uint8_t)first.year);
    }

    if (!(status_b & 0x02)) {
        if (pm && first.hour < 12) first.hour = (uint8_t)(first.hour + 12);
        if (!pm && first.hour == 12) first.hour = 0;
    }

    /* The year register is two digits and there is no reliable century
     * register across chipsets, so pivot: 70-99 means 19xx, 00-69 means
     * 20xx. Good until 2070, which is a fine horizon for a hobby OS. */
    out->year    = (uint16_t)(first.year >= 70 ? 1900 + first.year
                                               : 2000 + first.year);
    out->month   = first.month ? first.month : 1;
    out->day     = first.day ? first.day : 1;
    out->hour    = first.hour;
    out->minute  = first.minute;
    out->second  = first.second;
    out->weekday = first.weekday ? (uint8_t)(first.weekday - 1) : 0;
}

int32_t rtc_days_from_civil(int32_t year, uint32_t month, uint32_t day) {
    /* Days-from-civil, the standard era-based formulation: shift the year
     * so it starts in March (making the leap day the last day of the
     * "year"), then count whole 400-year eras plus the offset inside one.
     * Exact for any date this OS will ever see, with no lookup tables. */
    year -= (month <= 2);
    int32_t era = (year >= 0 ? year : year - 399) / 400;
    uint32_t yoe = (uint32_t)(year - era * 400);                  /* 0..399 */
    uint32_t doy = (153u * (month + (month > 2 ? (uint32_t)-3 : 9u)) + 2u) / 5u
                   + day - 1u;                                     /* 0..365 */
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;       /* 0..146096 */
    return era * 146097 + (int32_t)doe - 719468; /* rebase 0000-03-01 -> 1970-01-01 */
}

uint32_t rtc_unix_time(void) {
    rtc_time_t t;
    rtc_read(&t);

    int32_t days = rtc_days_from_civil(t.year, t.month, t.day);
    return (uint32_t)days * 86400u + t.hour * 3600u + t.minute * 60u + t.second;
}
