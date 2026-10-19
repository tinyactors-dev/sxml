#pragma once

/*
 * Calendar arithmetic for the date formatting functions (glossary
 * `calendar`; packet 7.17, D221): day numbers of the proleptic Gregorian
 * calendar, and the dates of the arithmetic calendars F&O 4.0, section
 * 9.9.4.8 lists that sxml converts to: OS (the Julian calendar, "Old
 * Style") and AH (the tabular Islamic calendar of CLDR's islamic-civil:
 * a 30-year cycle with leap years 2, 5, 7, 10, 13, 16, 18, 21, 24, 26
 * and 29, epoch Friday 16 July 622 Julian). BE and JE are eras over the
 * Gregorian calendar (src/format_date.c, with CLDR's era start dates).
 * Internal to sxml; defined in src/calendar.c.
 *
 * Day numbers count days from 1970-01-01 (Gregorian). Callers keep the
 * years within SXML_CALENDAR_MAXIMUM_YEAR, so nothing overflows. Nothing
 * here recurses or keeps state (D71).
 */

#include <stdbool.h>
#include <stdint.h>

/* The largest |year| the conversions of the other calendars take. */
#define SXML_CALENDAR_MAXIMUM_YEAR 1000000000000LL

/* A date of a calendar: year (astronomical: 0 is 1 BC), month 1 to 12,
   day of month from 1, and day of year from 1. */
typedef struct sxml_calendar_date {
    int64_t year;
    int64_t month;
    int64_t day;
    int64_t day_of_year;
} sxml_calendar_date;

/* The day number of a proleptic Gregorian date (H. Hinnant's
   days_from_civil); day_of_year is ignored. */
int64_t sxml_calendar_gregorian_days(sxml_calendar_date date);

/* The proleptic Gregorian date of a day number, with its day of year. */
sxml_calendar_date sxml_calendar_gregorian_date(int64_t day_number);

/* The Julian date of a day number, with its day of year. */
sxml_calendar_date sxml_calendar_julian_date(int64_t day_number);

/* The tabular Islamic (civil) date of a day number, with its day of
   year; years before 1 AH are 0 and below. */
sxml_calendar_date sxml_calendar_islamic_date(int64_t day_number);

/* 1 (Monday) to 7 (Sunday) of a day number, as ISO 8601 numbers days. */
int64_t sxml_calendar_iso_weekday(int64_t day_number);
