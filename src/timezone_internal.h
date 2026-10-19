#pragma once

#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The timezone database (glossary `timezone_database`, `zone_name`,
 * `local_time_type`, `timezone_rule`; packet 7.16, D209): the optional
 * feature sxml_timezone_database_feature (include/sxml/processor.h) and the
 * reader of the data it gets from the resolver. Internal to sxml; defined
 * in src/timezone.c.
 *
 * A zone is read from its TZif file (RFC 8536): the transitions, the local
 * time types they switch to, and the footer's TZ string (RFC 8536, section
 * 3.3; POSIX.1-2024, XBD 8.3 "TZ"), the rule for instants after the last
 * transition. Leap second records are skipped: instants are POSIX
 * seconds, as xs:dateTime has no leap seconds. Country codes map to zones
 * through zone1970.tab, else zone.tab (the tables of tzdata).
 *
 * The feature's D162 places are fn:civil-timezone
 * (src/function_library_temporal.c) and the $place argument of the date
 * formatting functions (src/function_library_format_date.c): both ask
 * sxml_processor_find_optional_feature for
 * SXML_OPTIONAL_FEATURE_KIND_TIMEZONE_DATABASE and keep their behaviour
 * without the feature when it is NULL.
 *
 * Nothing here recurses (D71). Thread safety: the parsing functions are
 * safe from any thread; the feature's operations run on the processor's
 * thread (D35).
 */

/* The URI prefix of the timezone database's files (SXML_RESOURCE_KIND_
   TIMEZONE_DATA): a zone name or a table name follows. */
#define SXML_TIMEZONE_DATA_URI_PREFIX "urn:sxml:timezone-data:"

/* ------------------------------------------------------ Local time types */

/* What local time is at an instant (RFC 8536, section 3.2, "local time
   type records"; glossary `local_time_type`). */
typedef struct sxml_local_time_type {
    /* Seconds east of UTC; -89999 to 93599 in a valid zone. */
    int64_t offset_seconds;
    bool is_daylight_saving;
    /* The abbreviation, such as "CET" or "-03", borrowed from the zone or
       the rule text it was read from. */
    sxml_byte_view abbreviation;
} sxml_local_time_type;

/* True when type's abbreviation is a timezone name: it starts with an
   ASCII letter. tzdata writes "-03" or "+0530" where a zone has no name
   in English. */
bool sxml_local_time_type_has_name(const sxml_local_time_type *type);

/* ----------------------------------------------------------- TZ rules */

/* How a date of a TZ rule names a day (POSIX, XBD 8.3, "rule"). */
typedef enum sxml_timezone_rule_day_kind {
    /* Jn: day 1 to 365, February 29 never counted. */
    SXML_TIMEZONE_RULE_DAY_JULIAN,
    /* n: day 0 to 365, February 29 counted in leap years. */
    SXML_TIMEZONE_RULE_DAY_ZERO_BASED,
    /* Mm.w.d: weekday d (0 Sunday) of week w (1 to 5, 5 the last) of
       month m. */
    SXML_TIMEZONE_RULE_DAY_MONTH_WEEK,
} sxml_timezone_rule_day_kind;

/* When a change of a TZ rule happens each year. */
typedef struct sxml_timezone_rule_date {
    sxml_timezone_rule_day_kind day_kind;
    /* JULIAN, ZERO_BASED: the day number. MONTH_WEEK: the weekday. */
    int32_t day;
    /* MONTH_WEEK only. */
    int32_t month;
    int32_t week;
    /* Seconds after midnight of the local time in effect before the
       change, -167 to 167 hours (RFC 8536, section 3.3.1); default 2
       hours. */
    int64_t time_seconds;
} sxml_timezone_rule_date;

/* A TZ string read (glossary `timezone_rule`): the standard time and,
   when it has one, the daylight saving time and when it starts and
   ends. */
typedef struct sxml_timezone_rule {
    sxml_local_time_type standard_time;
    bool has_daylight_saving;
    sxml_local_time_type daylight_saving_time;
    sxml_timezone_rule_date start;
    sxml_timezone_rule_date end;
} sxml_timezone_rule;

/*
 * Reads the TZ string text (POSIX, XBD 8.3, with RFC 8536's extensions:
 * hours of times up to 167 and signed) into *rule, whose abbreviations
 * borrow from text. A daylight saving time without a rule takes the
 * United States' rule M3.2.0,M11.1.0, as the tz code does. Returns
 * SXML_STATUS_MALFORMED_INPUT for any other text; *rule is then
 * unspecified.
 */
SXML_NODISCARD sxml_status sxml_timezone_rule_read(sxml_byte_view text,
                                                   sxml_timezone_rule *rule);

/* The local time type rule gives at instant_seconds (POSIX seconds since
   1970-01-01T00:00:00Z; years beyond SXML_TIMEZONE_MAXIMUM_YEAR are the
   caller's to avoid). */
sxml_local_time_type sxml_timezone_rule_find(const sxml_timezone_rule *rule,
                                             int64_t instant_seconds);

/* ---------------------------------------------------------------- Zones */

/* One zone read from a TZif file. Its arrays live in two blocks from the
   allocator it was read with. */
typedef struct sxml_timezone_zone {
    /* Ascending POSIX seconds; transition_count of them. */
    const int64_t *transition_times;
    /* The index into local_time_types of each transition's type. */
    const uint8_t *transition_types;
    size_t transition_count;
    /* At least one; the first is in effect before the first transition. */
    const sxml_local_time_type *local_time_types;
    size_t local_time_type_count;
    /* The footer's rule, for instants from the last transition on. */
    bool has_rule;
    sxml_timezone_rule rule;
    /* The block the local time types, the transitions' type indexes,
       the abbreviations and the rule's text live in, and the block of the
       transition times. */
    void *memory;
    void *time_memory;
} sxml_timezone_zone;

/*
 * Reads the TZif file content (RFC 8536, versions 1 to 4: the 64-bit data
 * and the footer when the version is 2 or later) into *zone, with memory
 * from allocator that sxml_timezone_zone_finalize releases. Returns
 * SXML_STATUS_MALFORMED_INPUT when content is not a valid TZif file
 * (section 3: counts, indexes, ascending transitions, abbreviations,
 * footer) and the allocator's failures; *zone then holds nothing to
 * finalize.
 */
SXML_NODISCARD sxml_status sxml_timezone_zone_read(sxml_allocator allocator,
                                                   sxml_byte_view content,
                                                   sxml_timezone_zone *zone);

void sxml_timezone_zone_finalize(sxml_timezone_zone *zone, sxml_allocator allocator);

/* The local time type of zone at instant_seconds (RFC 8536, section 3.2:
   type 0 before the first transition, the footer's rule from the last
   transition on when there is one). */
sxml_local_time_type sxml_timezone_zone_find(const sxml_timezone_zone *zone,
                                             int64_t instant_seconds);

/* The standard time zone keeps now, for a time without a date: the
   rule's standard time, else the last transition's type when that is not
   daylight saving time, else the latest standard type before it, else
   type 0. */
sxml_local_time_type sxml_timezone_zone_standard_time(const sxml_timezone_zone *zone);

/* ------------------------------------------------------------ Instants */

/* The years an instant is computed for: the database answers for none
   beyond (fn:civil-timezone's FODT0004 "does not cover a sufficient range
   of dates"). */
#define SXML_TIMEZONE_MAXIMUM_YEAR INT64_C(1000000000)

/* Writes the POSIX seconds of value (an xs:dateTime or xs:date with a
   timezone; the time of a date is midnight) to *instant_seconds and
   returns true; false when value has no timezone or its year is beyond
   SXML_TIMEZONE_MAXIMUM_YEAR. Nanoseconds are dropped. */
bool sxml_timezone_instant_of(sxml_date_time value, int64_t *instant_seconds);

/* --------------------------------------------------------- The feature */

/* A place to look up (F&O 4.0, sections 9.7.4 and 9.9.4.8). Views are
   borrowed for the call. */
typedef struct sxml_timezone_place_request {
    sxml_processor *processor;
    /* A zone name such as "America/New_York", or, when
       accepts_country_code, also two ASCII letters: an ISO 3166-1 code in
       any case. */
    sxml_byte_view place;
    bool accepts_country_code;
    /* The instant to answer for; without one, the standard time each zone
       keeps now (sxml_timezone_zone_standard_time). */
    bool has_instant;
    int64_t instant_seconds;
    /* A country code: the offset a zone of the country must have at the
       instant to be the one answered for. */
    int64_t country_offset_seconds;
} sxml_timezone_place_request;

/* What place a request named. */
typedef enum sxml_timezone_place_kind {
    /* No zone or country the database knows, or the database's files
       are not available. */
    SXML_TIMEZONE_PLACE_KIND_UNKNOWN,
    SXML_TIMEZONE_PLACE_KIND_ZONE,
    SXML_TIMEZONE_PLACE_KIND_COUNTRY,
} sxml_timezone_place_kind;

typedef struct sxml_timezone_place_outcome {
    sxml_timezone_place_kind place_kind;
    /* ZONE: always. COUNTRY: when one of its zones (in the table's
       order) has country_offset_seconds at the instant. */
    bool has_local_time;
    /* Its abbreviation is borrowed from the processor's feature state,
       which lives as long as the processor. */
    sxml_local_time_type local_time;
} sxml_timezone_place_outcome;

/* The operations of sxml_timezone_database_feature. */
typedef struct sxml_timezone_feature_operations {
    /*
     * Looks request->place up: a zone's TZif file is read through the
     * processor's resolver the first time it is named, and the zone
     * table the first time a country code is, and kept as the feature's
     * state in the processor. A file that is not available or not valid
     * makes its place unknown. Returns the failures of the resolver and
     * the allocator; *outcome is then unknown.
     */
    sxml_status (*find_place)(const sxml_timezone_place_request *request,
                              sxml_timezone_place_outcome *outcome);
} sxml_timezone_feature_operations;
