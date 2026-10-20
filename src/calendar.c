/*
 * Calendar arithmetic (src/calendar_internal.h; packet 7.17, D221).
 *
 * Golden file: src/format_date.c of packet 2.14 (the Gregorian day
 * numbers moved here from it, so every calendar converts through one
 * day number).
 */
#include "calendar_internal.h"

/* floor(numerator / denominator) for a positive denominator. */
static int64_t floor_divide(int64_t numerator, int64_t denominator) {
    const int64_t quotient = numerator / denominator;
    return numerator % denominator < 0 ? quotient - 1 : quotient;
}

sxml_calendar_cycle_day sxml_calendar_cycle_day_of(sxml_calendar_date date) {
    /* The year starts on March 1, so a leap day ends it. */
    const int64_t year = date.month <= 2 ? date.year - 1 : date.year;
    const int64_t cycle_number = floor_divide(year, 400);
    const int64_t year_of_cycle = year - cycle_number * 400;
    const int64_t shifted_month = date.month > 2 ? date.month - 3 : date.month + 9;
    const int64_t day_of_shifted_year = (153 * shifted_month + 2) / 5 + date.day - 1;
    return (sxml_calendar_cycle_day){
        .cycle_number = cycle_number,
        .day_of_cycle = year_of_cycle * 365 + year_of_cycle / 4 - year_of_cycle / 100 +
                        day_of_shifted_year};
}

sxml_calendar_date sxml_calendar_date_in_cycle(int64_t day_of_cycle) {
    const int64_t year_of_cycle = (day_of_cycle - day_of_cycle / 1460 + day_of_cycle / 36524 -
                                   day_of_cycle / 146096) /
                                  365;
    const int64_t day_of_shifted_year =
        day_of_cycle - (365 * year_of_cycle + year_of_cycle / 4 - year_of_cycle / 100);
    const int64_t shifted_month = (5 * day_of_shifted_year + 2) / 153;
    const int64_t day = day_of_shifted_year - (153 * shifted_month + 2) / 5 + 1;
    const int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    return (sxml_calendar_date){.year = year_of_cycle + (month <= 2 ? 1 : 0),
                                .month = month,
                                .day = day,
                                .day_of_year = 0};
}

/* 719,468: the days from 0000-03-01, the start of cycle 0, to 1970-01-01. */
int64_t sxml_calendar_gregorian_days(sxml_calendar_date date) {
    const sxml_calendar_cycle_day cycle_day = sxml_calendar_cycle_day_of(date);
    return cycle_day.cycle_number * SXML_CALENDAR_DAYS_PER_CYCLE + cycle_day.day_of_cycle - 719468;
}

sxml_calendar_date sxml_calendar_gregorian_date(int64_t day_number) {
    const int64_t shifted = day_number + 719468;
    const int64_t cycle_number = floor_divide(shifted, SXML_CALENDAR_DAYS_PER_CYCLE);
    sxml_calendar_date date =
        sxml_calendar_date_in_cycle(shifted - cycle_number * SXML_CALENDAR_DAYS_PER_CYCLE);
    date.year += cycle_number * 400;
    date.day_of_year =
        day_number -
        sxml_calendar_gregorian_days((sxml_calendar_date){.year = date.year, .month = 1, .day = 1}) +
        1;
    return date;
}

/* The days before each month of a common year. */
static const int64_t days_before_month[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

sxml_calendar_date sxml_calendar_julian_date(int64_t day_number) {
    /* The Julian Day Number of the day (2440588 for 1970-01-01), shifted
       by 32082 days to a year -4800 March 1, then E. G. Richards'
       algorithm (Explanatory Supplement to the Astronomical Almanac,
       15.11). */
    const int64_t shifted = day_number + 2440588 + 32082;
    const int64_t cycle = floor_divide(4 * shifted + 3, 1461);
    const int64_t day_of_cycle = shifted - floor_divide(1461 * cycle, 4);
    const int64_t shifted_month = (5 * day_of_cycle + 2) / 153;
    sxml_calendar_date date;
    date.day = day_of_cycle - (153 * shifted_month + 2) / 5 + 1;
    date.month = shifted_month + 3 - 12 * (shifted_month / 10);
    date.year = cycle - 4800 + shifted_month / 10;
    const bool is_leap = floor_divide(date.year, 4) * 4 == date.year;
    date.day_of_year =
        days_before_month[date.month - 1] + date.day + (is_leap && date.month > 2 ? 1 : 0);
    return date;
}

/* The days of the 30-year cycle, and the day number of 1 Muharram 1 AH
   (Julian Day Number 1948440). */
#define SXML_ISLAMIC_CYCLE_DAYS 10631
#define SXML_ISLAMIC_EPOCH_DAY (1948440 - 2440588)

static bool is_islamic_leap_year(int64_t year_of_cycle) {
    return (11 * year_of_cycle + 14) % 30 < 11;
}

sxml_calendar_date sxml_calendar_islamic_date(int64_t day_number) {
    const int64_t days = day_number - SXML_ISLAMIC_EPOCH_DAY;
    const int64_t cycle = floor_divide(days, SXML_ISLAMIC_CYCLE_DAYS);
    int64_t remaining = days - cycle * SXML_ISLAMIC_CYCLE_DAYS;
    int64_t year_of_cycle = 1;
    while (year_of_cycle < 30) {
        const int64_t year_length = is_islamic_leap_year(year_of_cycle) ? 355 : 354;
        if (remaining < year_length) {
            break;
        }
        remaining -= year_length;
        year_of_cycle += 1;
    }
    sxml_calendar_date date = {
        .year = cycle * 30 + year_of_cycle, .month = 1, .day = 1, .day_of_year = remaining + 1};
    /* Odd months have 30 days, even ones 29, the twelfth 30 in a leap
       year. */
    while (date.month < 12) {
        const int64_t month_length = date.month % 2 == 1 ? 30 : 29;
        if (remaining < month_length) {
            break;
        }
        remaining -= month_length;
        date.month += 1;
    }
    date.day = remaining + 1;
    return date;
}

int64_t sxml_calendar_iso_weekday(int64_t day_number) {
    /* 1970-01-01 was a Thursday. */
    return ((day_number % 7) + 7 + 3) % 7 + 1;
}
