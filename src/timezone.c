/*
 * The timezone database (src/timezone_internal.h; packet 7.16, D209): the
 * TZif reader (RFC 8536), TZ rules (POSIX.1-2024, XBD 8.3, with RFC 8536,
 * section 3.3.1), and the optional feature sxml_timezone_database_feature,
 * which reads zones and the zone table through the processor's resolver
 * (glossary `feature_data`) and keeps them as its state in the processor.
 *
 * Golden file: src/optional_feature.c (the test feature: reading feature
 * data once, keeping it as the processor's feature state); the byte
 * reading imitates src/function_library_binary.c.
 *
 * Dates are proleptic Gregorian day numbers from 1970-01-01, by the
 * algorithms of src/format_date.c (days_from_civil and its inverse).
 */
#include "timezone_internal.h"

#include "optional_feature_internal.h"
#include "processor_internal.h"

#include <stdalign.h>
#include <string.h>

enum {
    SXML_TIMEZONE_SECONDS_PER_DAY = 86400,
    SXML_TIMEZONE_SECONDS_PER_HOUR = 3600,
    /* RFC 8536, section 3.3.1: hours of a rule's times, -167 to 167. */
    SXML_TIMEZONE_MAXIMUM_RULE_HOURS = 167,
    /* POSIX, XBD 8.3: an offset's hours, 0 to 24. */
    SXML_TIMEZONE_MAXIMUM_OFFSET_HOURS = 24,
    /* The longest zone name sxml asks for (tzdata's are below 40). */
    SXML_TIMEZONE_MAXIMUM_ZONE_NAME_BYTE_COUNT = 255,
};

/* ------------------------------------------------------------- Calendar */

static int64_t floor_divide(int64_t dividend, int64_t divisor) {
    const int64_t quotient = dividend / divisor;
    return (dividend % divisor != 0 && (dividend < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

static int64_t floor_modulo(int64_t dividend, int64_t divisor) {
    return dividend - floor_divide(dividend, divisor) * divisor;
}

static bool is_leap_year(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int64_t month_length(int64_t year, int64_t month) {
    static const int8_t lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap_year(year) ? 29 : lengths[month - 1];
}

/* The day number of a proleptic Gregorian date, 1970-01-01 being 0. */
static int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    const int64_t shifted_year = month <= 2 ? year - 1 : year;
    const int64_t era = floor_divide(shifted_year, 400);
    const int64_t year_of_era = shifted_year - era * 400;
    const int64_t day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

/* The year of a day number. */
static int64_t year_from_days(int64_t day_number) {
    const int64_t shifted = day_number + 719468;
    const int64_t era = floor_divide(shifted, 146097);
    const int64_t day_of_era = shifted - era * 146097;
    const int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t shifted_month = (5 * day_of_year + 2) / 153;
    return year_of_era + era * 400 + (shifted_month >= 10 ? 1 : 0);
}

bool sxml_timezone_instant_of(sxml_date_time value, int64_t *instant_seconds) {
    if (!value.has_timezone || value.year > SXML_TIMEZONE_MAXIMUM_YEAR ||
        value.year < -SXML_TIMEZONE_MAXIMUM_YEAR) {
        return false;
    }
    *instant_seconds = days_from_civil(value.year, value.month, value.day) * SXML_TIMEZONE_SECONDS_PER_DAY +
                       (int64_t)value.hour * SXML_TIMEZONE_SECONDS_PER_HOUR + (int64_t)value.minute * 60 +
                       value.second - (int64_t)value.timezone_offset_minutes * 60;
    return true;
}

/* ------------------------------------------------------ Local time types */

bool sxml_local_time_type_has_name(const sxml_local_time_type *type) {
    if (type->abbreviation.byte_count == 0) {
        return false;
    }
    const uint8_t first = type->abbreviation.bytes[0];
    return (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
}

/* ----------------------------------------------------------- TZ rules */

/* The text of a TZ string and how far it is read. */
typedef struct rule_scanner {
    sxml_byte_view text;
    size_t position;
} rule_scanner;

static bool scanner_is_at_end(const rule_scanner *scanner) {
    return scanner->position >= scanner->text.byte_count;
}

static uint8_t scanner_peek(const rule_scanner *scanner) {
    return scanner_is_at_end(scanner) ? 0 : scanner->text.bytes[scanner->position];
}

static bool scanner_accept(rule_scanner *scanner, uint8_t byte) {
    if (scanner_peek(scanner) == byte && !scanner_is_at_end(scanner)) {
        scanner->position += 1;
        return true;
    }
    return false;
}

static bool is_ascii_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

static bool is_ascii_letter(uint8_t byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

/* Reads a decimal number of at most seven digits into *number. */
static bool scan_number(rule_scanner *scanner, int64_t *number) {
    int64_t value = 0;
    size_t digit_count = 0;
    while (is_ascii_digit(scanner_peek(scanner)) && digit_count < 7) {
        value = value * 10 + (scanner_peek(scanner) - '0');
        scanner->position += 1;
        digit_count += 1;
    }
    *number = value;
    return digit_count > 0 && !is_ascii_digit(scanner_peek(scanner));
}

/* A name: three or more letters, or "<" letters, digits, "+" and "-"
   ">" (POSIX, XBD 8.3, "std and dst"); the brackets are not part of the
   abbreviation. */
static bool scan_name(rule_scanner *scanner, sxml_byte_view *name) {
    const bool is_quoted = scanner_accept(scanner, '<');
    const size_t start = scanner->position;
    while (!scanner_is_at_end(scanner)) {
        const uint8_t byte = scanner_peek(scanner);
        if (!(is_ascii_letter(byte) ||
              (is_quoted && (is_ascii_digit(byte) || byte == '+' || byte == '-')))) {
            break;
        }
        scanner->position += 1;
    }
    *name = (sxml_byte_view){.bytes = scanner->text.bytes + start,
                             .byte_count = scanner->position - start};
    if (is_quoted && !scanner_accept(scanner, '>')) {
        return false;
    }
    return name->byte_count >= (is_quoted ? 1U : 3U);
}

/* [+-]hh[:mm[:ss]] with hours up to maximum_hours, in seconds. */
static bool scan_time(rule_scanner *scanner, int64_t maximum_hours, int64_t *seconds) {
    const bool is_negative = scanner_accept(scanner, '-');
    if (!is_negative) {
        (void)scanner_accept(scanner, '+');
    }
    int64_t hours = 0;
    int64_t minutes = 0;
    int64_t remaining_seconds = 0;
    if (!scan_number(scanner, &hours) || hours > maximum_hours) {
        return false;
    }
    if (scanner_accept(scanner, ':')) {
        if (!scan_number(scanner, &minutes) || minutes > 59) {
            return false;
        }
        if (scanner_accept(scanner, ':') &&
            (!scan_number(scanner, &remaining_seconds) || remaining_seconds > 59)) {
            return false;
        }
    }
    const int64_t magnitude = hours * SXML_TIMEZONE_SECONDS_PER_HOUR + minutes * 60 + remaining_seconds;
    *seconds = is_negative ? -magnitude : magnitude;
    return true;
}

/* ",date[/time]" (POSIX, XBD 8.3, "rule"). */
static bool scan_rule_date(rule_scanner *scanner, sxml_timezone_rule_date *date) {
    int64_t first = 0;
    int64_t second = 0;
    int64_t third = 0;
    *date = (sxml_timezone_rule_date){.day_kind = SXML_TIMEZONE_RULE_DAY_ZERO_BASED,
                                      .day = 0,
                                      .month = 0,
                                      .week = 0,
                                      .time_seconds = (int64_t)2 * SXML_TIMEZONE_SECONDS_PER_HOUR};
    if (scanner_accept(scanner, 'J')) {
        if (!scan_number(scanner, &first) || first < 1 || first > 365) {
            return false;
        }
        date->day_kind = SXML_TIMEZONE_RULE_DAY_JULIAN;
        date->day = (int32_t)first;
    } else if (scanner_accept(scanner, 'M')) {
        if (!scan_number(scanner, &first) || first < 1 || first > 12 ||
            !scanner_accept(scanner, '.') || !scan_number(scanner, &second) || second < 1 ||
            second > 5 || !scanner_accept(scanner, '.') || !scan_number(scanner, &third) ||
            third > 6) {
            return false;
        }
        date->day_kind = SXML_TIMEZONE_RULE_DAY_MONTH_WEEK;
        date->month = (int32_t)first;
        date->week = (int32_t)second;
        date->day = (int32_t)third;
    } else {
        if (!scan_number(scanner, &first) || first > 365) {
            return false;
        }
        date->day = (int32_t)first;
    }
    if (scanner_accept(scanner, '/')) {
        return scan_time(scanner, SXML_TIMEZONE_MAXIMUM_RULE_HOURS, &date->time_seconds);
    }
    return true;
}

sxml_status sxml_timezone_rule_read(sxml_byte_view text, sxml_timezone_rule *rule) {
    rule_scanner scanner = {.text = text, .position = 0};
    memset(rule, 0, sizeof(*rule));
    int64_t standard_offset = 0;
    if (!scan_name(&scanner, &rule->standard_time.abbreviation) ||
        !scan_time(&scanner, SXML_TIMEZONE_MAXIMUM_OFFSET_HOURS, &standard_offset)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    /* POSIX offsets are west of UTC. */
    rule->standard_time.offset_seconds = -standard_offset;
    if (scanner_is_at_end(&scanner)) {
        return SXML_STATUS_OK;
    }
    rule->has_daylight_saving = true;
    rule->daylight_saving_time.is_daylight_saving = true;
    if (!scan_name(&scanner, &rule->daylight_saving_time.abbreviation)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    rule->daylight_saving_time.offset_seconds = rule->standard_time.offset_seconds + SXML_TIMEZONE_SECONDS_PER_HOUR;
    const uint8_t next = scanner_peek(&scanner);
    if (!scanner_is_at_end(&scanner) && next != ',') {
        int64_t daylight_offset = 0;
        if (!scan_time(&scanner, SXML_TIMEZONE_MAXIMUM_OFFSET_HOURS, &daylight_offset)) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
        rule->daylight_saving_time.offset_seconds = -daylight_offset;
    }
    if (scanner_is_at_end(&scanner)) {
        /* No rule: the United States' since 2007, as the tz code's
           default. */
        rule->start = (sxml_timezone_rule_date){.day_kind = SXML_TIMEZONE_RULE_DAY_MONTH_WEEK,
                                                .day = 0,
                                                .month = 3,
                                                .week = 2,
                                                .time_seconds = (int64_t)2 * SXML_TIMEZONE_SECONDS_PER_HOUR};
        rule->end = rule->start;
        rule->end.month = 11;
        rule->end.week = 1;
        return SXML_STATUS_OK;
    }
    if (!scanner_accept(&scanner, ',') || !scan_rule_date(&scanner, &rule->start) ||
        !scanner_accept(&scanner, ',') || !scan_rule_date(&scanner, &rule->end) ||
        !scanner_is_at_end(&scanner)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    return SXML_STATUS_OK;
}

/* The day number of date in year. */
static int64_t rule_date_day(const sxml_timezone_rule_date *date, int64_t year) {
    switch (date->day_kind) {
    case SXML_TIMEZONE_RULE_DAY_JULIAN: {
        const int64_t leap_day = is_leap_year(year) && date->day >= 60 ? 1 : 0;
        return days_from_civil(year, 1, 1) + date->day - 1 + leap_day;
    }
    case SXML_TIMEZONE_RULE_DAY_ZERO_BASED:
        return days_from_civil(year, 1, 1) + date->day;
    case SXML_TIMEZONE_RULE_DAY_MONTH_WEEK:
        break;
    }
    const int64_t first_day = days_from_civil(year, date->month, 1);
    /* 1970-01-01 was a Thursday (weekday 4). */
    const int64_t first_weekday = floor_modulo(first_day + 4, 7);
    int64_t day_offset = (date->day - first_weekday + 7) % 7 + 7 * (int64_t)(date->week - 1);
    if (day_offset >= month_length(year, date->month)) {
        day_offset -= 7;
    }
    return first_day + day_offset;
}

sxml_local_time_type sxml_timezone_rule_find(const sxml_timezone_rule *rule,
                                             int64_t instant_seconds) {
    if (!rule->has_daylight_saving) {
        return rule->standard_time;
    }
    const int64_t standard_offset = rule->standard_time.offset_seconds;
    const int64_t daylight_offset = rule->daylight_saving_time.offset_seconds;
    const int64_t year =
        year_from_days(floor_divide(instant_seconds + standard_offset, SXML_TIMEZONE_SECONDS_PER_DAY));
    /* The start is in standard time, the end in daylight saving time. */
    const int64_t start = rule_date_day(&rule->start, year) * SXML_TIMEZONE_SECONDS_PER_DAY +
                          rule->start.time_seconds - standard_offset;
    const int64_t end = rule_date_day(&rule->end, year) * SXML_TIMEZONE_SECONDS_PER_DAY +
                        rule->end.time_seconds - daylight_offset;
    const bool is_daylight_saving = start < end
                                        ? instant_seconds >= start && instant_seconds < end
                                        : !(instant_seconds >= end && instant_seconds < start);
    return is_daylight_saving ? rule->daylight_saving_time : rule->standard_time;
}

/* ---------------------------------------------------------------- TZif */

/* The bytes of a TZif file and how far they are read. */
typedef struct tzif_cursor {
    sxml_byte_view content;
    size_t position;
} tzif_cursor;

/* Takes byte_count bytes; false when fewer remain. */
static bool cursor_take(tzif_cursor *cursor, uint64_t byte_count, const uint8_t **bytes) {
    if (byte_count > (uint64_t)(cursor->content.byte_count - cursor->position)) {
        return false;
    }
    *bytes = cursor->content.bytes + cursor->position;
    cursor->position += (size_t)byte_count;
    return true;
}

static uint32_t read_unsigned_32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static int64_t read_signed_64(const uint8_t *bytes) {
    const uint64_t high = read_unsigned_32(bytes);
    const uint64_t low = read_unsigned_32(bytes + 4);
    const uint64_t value = (high << 32) | low;
    return value > (uint64_t)INT64_MAX ? -(int64_t)(~value) - 1 : (int64_t)value;
}

static int64_t read_signed_32(const uint8_t *bytes) {
    const uint32_t value = read_unsigned_32(bytes);
    return value > (uint32_t)INT32_MAX ? -(int64_t)(~value) - 1 : (int64_t)value;
}

/* A TZif header (RFC 8536, section 3.1). */
typedef struct tzif_header {
    uint8_t version;
    uint32_t utc_indicator_count;
    uint32_t standard_indicator_count;
    uint32_t leap_second_count;
    uint32_t transition_count;
    uint32_t type_count;
    uint32_t character_count;
} tzif_header;

static bool read_header(tzif_cursor *cursor, tzif_header *header) {
    const uint8_t *bytes = NULL;
    if (!cursor_take(cursor, 44, &bytes) || memcmp(bytes, "TZif", 4) != 0) {
        return false;
    }
    header->version = bytes[4];
    header->utc_indicator_count = read_unsigned_32(bytes + 20);
    header->standard_indicator_count = read_unsigned_32(bytes + 24);
    header->leap_second_count = read_unsigned_32(bytes + 28);
    header->transition_count = read_unsigned_32(bytes + 32);
    header->type_count = read_unsigned_32(bytes + 36);
    header->character_count = read_unsigned_32(bytes + 40);
    return (header->version == 0 || (header->version >= '2' && header->version <= '4')) &&
           (header->utc_indicator_count == 0 ||
            header->utc_indicator_count == header->type_count) &&
           (header->standard_indicator_count == 0 ||
            header->standard_indicator_count == header->type_count) &&
           header->type_count >= 1 && header->type_count <= 256 &&
           header->character_count >= 1;
}

/* The byte count of a data block whose times have time_size bytes. */
static uint64_t data_block_size(const tzif_header *header, uint64_t time_size) {
    return (uint64_t)header->transition_count * (time_size + 1) +
           (uint64_t)header->type_count * 6 + header->character_count +
           (uint64_t)header->leap_second_count * (time_size + 4) +
           header->standard_indicator_count + header->utc_indicator_count;
}

/* Where the bytes of a zone's first block are, after its local time
   types. */
typedef struct zone_layout {
    size_t transition_types_offset;
    size_t characters_offset;
    size_t footer_offset;
    size_t byte_count;
} zone_layout;

static zone_layout lay_out_zone(const tzif_header *header, size_t footer_byte_count) {
    zone_layout layout;
    layout.transition_types_offset = 0;
    layout.characters_offset = header->transition_count;
    layout.footer_offset = layout.characters_offset + header->character_count;
    layout.byte_count = layout.footer_offset + footer_byte_count + 1;
    return layout;
}

/* What the data block of a TZif file holds, its bytes borrowed. */
typedef struct tzif_data {
    const tzif_header *header;
    uint64_t time_size;
    const uint8_t *times;
    const uint8_t *transition_types;
    const uint8_t *types;
    const uint8_t *characters;
} tzif_data;

static bool take_data_block(tzif_cursor *cursor, tzif_data *data) {
    const tzif_header *header = data->header;
    const uint8_t *skipped = NULL;
    return cursor_take(cursor, header->transition_count * data->time_size, &data->times) &&
           cursor_take(cursor, header->transition_count, &data->transition_types) &&
           cursor_take(cursor, (uint64_t)header->type_count * 6, &data->types) &&
           cursor_take(cursor, header->character_count, &data->characters) &&
           cursor_take(cursor,
                       (uint64_t)header->leap_second_count * (data->time_size + 4) +
                           header->standard_indicator_count + header->utc_indicator_count,
                       &skipped);
}

/* The memory a zone's arrays are filled into. */
typedef struct zone_arrays {
    sxml_local_time_type *types;
    int64_t *times;
    /* The transitions' type indexes, the abbreviations, the footer. */
    uint8_t *bytes;
    zone_layout layout;
} zone_arrays;

/* Fills the zone's arrays from data; false when data breaks a rule of
   RFC 8536, section 3.2. */
static bool fill_zone(const tzif_data *data, const zone_arrays *arrays,
                      sxml_timezone_zone *zone) {
    const tzif_header *header = data->header;
    sxml_local_time_type *types = arrays->types;
    int64_t *times = arrays->times;
    uint8_t *transition_types = arrays->bytes + arrays->layout.transition_types_offset;
    uint8_t *characters = arrays->bytes + arrays->layout.characters_offset;
    memcpy(characters, data->characters, header->character_count);
    if (characters[header->character_count - 1] != 0) {
        return false;
    }
    for (size_t type_index = 0; type_index < header->type_count; type_index += 1) {
        const uint8_t *record = data->types + type_index * 6;
        const int64_t offset = read_signed_32(record);
        const uint8_t is_daylight_saving = record[4];
        const uint8_t abbreviation_index = record[5];
        if (offset == INT32_MIN || is_daylight_saving > 1 ||
            abbreviation_index >= header->character_count) {
            return false;
        }
        const uint8_t *abbreviation = characters + abbreviation_index;
        /* The abbreviation ends at the next NUL, which the last character
           is. */
        const uint8_t *end =
            memchr(abbreviation, 0, header->character_count - abbreviation_index);
        types[type_index] = (sxml_local_time_type){
            .offset_seconds = offset,
            .is_daylight_saving = is_daylight_saving == 1,
            .abbreviation = {.bytes = abbreviation, .byte_count = (size_t)(end - abbreviation)}};
    }
    for (size_t transition_index = 0; transition_index < header->transition_count;
         transition_index += 1) {
        const uint8_t *time = data->times + transition_index * data->time_size;
        times[transition_index] =
            data->time_size == 8 ? read_signed_64(time) : read_signed_32(time);
        transition_types[transition_index] = data->transition_types[transition_index];
        if (transition_types[transition_index] >= header->type_count ||
            (transition_index > 0 && times[transition_index] <= times[transition_index - 1])) {
            return false;
        }
    }
    zone->transition_times = times;
    zone->transition_types = transition_types;
    zone->transition_count = header->transition_count;
    zone->local_time_types = types;
    zone->local_time_type_count = header->type_count;
    return true;
}

/* Reads the footer of a version 2 or later file: "\n" TZ string "\n"
   (RFC 8536, section 3.3). */
static bool take_footer(tzif_cursor *cursor, sxml_byte_view *footer) {
    const uint8_t *newline = NULL;
    if (!cursor_take(cursor, 1, &newline) || *newline != '\n') {
        return false;
    }
    const size_t start = cursor->position;
    while (cursor->position < cursor->content.byte_count &&
           cursor->content.bytes[cursor->position] != '\n') {
        cursor->position += 1;
    }
    if (cursor->position >= cursor->content.byte_count) {
        return false;
    }
    *footer = (sxml_byte_view){.bytes = cursor->content.bytes + start,
                               .byte_count = cursor->position - start};
    return true;
}

sxml_status sxml_timezone_zone_read(sxml_allocator allocator, sxml_byte_view content,
                                    sxml_timezone_zone *zone) {
    memset(zone, 0, sizeof(*zone));
    tzif_cursor cursor = {.content = content, .position = 0};
    tzif_header first_header;
    if (content.bytes == NULL || !read_header(&cursor, &first_header)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    tzif_header header = first_header;
    tzif_data data = {.header = &header, .time_size = 4};
    sxml_byte_view footer = {.bytes = NULL, .byte_count = 0};
    if (first_header.version == 0) {
        if (!take_data_block(&cursor, &data)) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
    } else {
        /* Skip the 32-bit block; the 64-bit one follows with its header. */
        const uint8_t *skipped = NULL;
        data.time_size = 8;
        if (!cursor_take(&cursor, data_block_size(&first_header, 4), &skipped) ||
            !read_header(&cursor, &header) || header.version != first_header.version ||
            !take_data_block(&cursor, &data) || !take_footer(&cursor, &footer)) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    const zone_layout layout = lay_out_zone(&header, footer.byte_count);
    const size_t types_byte_count = (size_t)header.type_count * sizeof(sxml_local_time_type);
    void *memory = NULL;
    void *time_memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = types_byte_count + layout.byte_count,
                                  .alignment = alignof(sxml_local_time_type)},
        &memory);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = (size_t)header.transition_count *
                                                        sizeof(int64_t) +
                                                    sizeof(int64_t),
                                      .alignment = alignof(int64_t)},
            &time_memory);
    }
    if (status != SXML_STATUS_OK) {
        if (memory != NULL) {
            sxml_allocator_release(allocator, memory);
        }
        return status;
    }
    sxml_local_time_type *types = memory;
    const zone_arrays arrays = {.types = types,
                                .times = time_memory,
                                .bytes = (uint8_t *)(types + header.type_count),
                                .layout = layout};
    zone->memory = memory;
    zone->time_memory = time_memory;
    if (!fill_zone(&data, &arrays, zone)) {
        sxml_timezone_zone_finalize(zone, allocator);
        return SXML_STATUS_MALFORMED_INPUT;
    }
    if (footer.byte_count > 0) {
        uint8_t *footer_bytes = arrays.bytes + layout.footer_offset;
        memcpy(footer_bytes, footer.bytes, footer.byte_count);
        zone->has_rule = true;
        status = sxml_timezone_rule_read(
            (sxml_byte_view){.bytes = footer_bytes, .byte_count = footer.byte_count},
            &zone->rule);
        if (status != SXML_STATUS_OK) {
            sxml_timezone_zone_finalize(zone, allocator);
            return status;
        }
    }
    return SXML_STATUS_OK;
}

void sxml_timezone_zone_finalize(sxml_timezone_zone *zone, sxml_allocator allocator) {
    if (zone->memory != NULL) {
        sxml_allocator_release(allocator, zone->memory);
    }
    if (zone->time_memory != NULL) {
        sxml_allocator_release(allocator, zone->time_memory);
    }
    memset(zone, 0, sizeof(*zone));
}

sxml_local_time_type sxml_timezone_zone_find(const sxml_timezone_zone *zone,
                                             int64_t instant_seconds) {
    const size_t count = zone->transition_count;
    if (count == 0 || instant_seconds < zone->transition_times[0]) {
        return count == 0 && zone->has_rule
                   ? sxml_timezone_rule_find(&zone->rule, instant_seconds)
                   : zone->local_time_types[0];
    }
    if (instant_seconds >= zone->transition_times[count - 1] && zone->has_rule) {
        return sxml_timezone_rule_find(&zone->rule, instant_seconds);
    }
    /* The last transition at or before the instant. */
    size_t low = 0;
    size_t high = count;
    while (high - low > 1) {
        const size_t middle = low + (high - low) / 2;
        if (zone->transition_times[middle] <= instant_seconds) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return zone->local_time_types[zone->transition_types[low]];
}

sxml_local_time_type sxml_timezone_zone_standard_time(const sxml_timezone_zone *zone) {
    if (zone->has_rule) {
        return zone->rule.standard_time;
    }
    for (size_t remaining = zone->transition_count; remaining > 0; remaining -= 1) {
        const sxml_local_time_type *type =
            &zone->local_time_types[zone->transition_types[remaining - 1]];
        if (!type->is_daylight_saving) {
            return *type;
        }
    }
    return zone->local_time_types[0];
}

/* ------------------------------------------------- The feature's state */

/* A zone read for a processor. */
typedef struct zone_entry {
    /* The zone name, in the block of the entry's name. */
    uint8_t *name_bytes;
    size_t name_byte_count;
    sxml_timezone_zone zone;
} zone_entry;

/* The feature's state in one processor: the zones read so far, and the
   zone table once read. */
typedef struct timezone_database_state {
    zone_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    bool has_read_table;
    /* zone1970.tab, else zone.tab; NULL when neither was available. */
    uint8_t *table_bytes;
    size_t table_byte_count;
} timezone_database_state;

static void finalize_state(void *state, sxml_allocator allocator) {
    timezone_database_state *database = state;
    for (size_t entry_index = 0; entry_index < database->entry_count; entry_index += 1) {
        sxml_allocator_release(allocator, database->entries[entry_index].name_bytes);
        sxml_timezone_zone_finalize(&database->entries[entry_index].zone, allocator);
    }
    if (database->entries != NULL) {
        sxml_allocator_release(allocator, database->entries);
    }
    if (database->table_bytes != NULL) {
        sxml_allocator_release(allocator, database->table_bytes);
    }
    sxml_allocator_release(allocator, database);
}

/* The processor's state, made at the first call. */
static sxml_status get_state(sxml_processor *processor, timezone_database_state **state) {
    *state = sxml_processor_get_optional_feature_state(
        processor, SXML_OPTIONAL_FEATURE_KIND_TIMEZONE_DATABASE);
    if (*state != NULL) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        processor->allocator,
        (sxml_allocation_request){.byte_count = sizeof(timezone_database_state),
                                  .alignment = alignof(timezone_database_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, sizeof(timezone_database_state));
    status = sxml_processor_set_optional_feature_state(
        processor, SXML_OPTIONAL_FEATURE_KIND_TIMEZONE_DATABASE, memory);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, memory);
        return status;
    }
    *state = memory;
    return SXML_STATUS_OK;
}

/* --------------------------------------------------------- Reading data */

/* A file of the database to read: its path after the URI prefix. */
typedef struct data_file_request {
    sxml_processor *processor;
    sxml_byte_view path;
    const sxml_feature_data_reader_operations *operations;
    void *reader_state;
} data_file_request;

static sxml_status read_data_file(const data_file_request *request,
                                  sxml_feature_data_outcome *outcome) {
    enum { SXML_TIMEZONE_PREFIX_BYTE_COUNT = sizeof(SXML_TIMEZONE_DATA_URI_PREFIX) - 1 };
    uint8_t uri[SXML_TIMEZONE_PREFIX_BYTE_COUNT + SXML_TIMEZONE_MAXIMUM_ZONE_NAME_BYTE_COUNT];
    memcpy(uri, SXML_TIMEZONE_DATA_URI_PREFIX, SXML_TIMEZONE_PREFIX_BYTE_COUNT);
    memcpy(uri + SXML_TIMEZONE_PREFIX_BYTE_COUNT, request->path.bytes, request->path.byte_count);
    return sxml_optional_feature_read_data(
        &(sxml_feature_data_request){
            .processor = request->processor,
            .resource_kind = SXML_RESOURCE_KIND_TIMEZONE_DATA,
            .uri = {.bytes = uri, .byte_count = SXML_TIMEZONE_PREFIX_BYTE_COUNT + request->path.byte_count},
            .reader = {.state = request->reader_state, .operations = request->operations}},
        outcome);
}

/* Reading one zone's TZif file. */
typedef struct zone_reading {
    sxml_allocator allocator;
    sxml_timezone_zone zone;
    bool is_valid;
} zone_reading;

static sxml_status read_zone_content(void *state, sxml_byte_view content) {
    zone_reading *reading = state;
    const sxml_status status = sxml_timezone_zone_read(reading->allocator, content, &reading->zone);
    reading->is_valid = status == SXML_STATUS_OK;
    /* A file that is not TZif is a zone sxml does not know. */
    return status == SXML_STATUS_MALFORMED_INPUT ? SXML_STATUS_OK : status;
}

static const sxml_feature_data_reader_operations zone_reader_operations = {
    .read = read_zone_content,
};

/* Reading the zone table: a copy of its bytes. */
typedef struct table_reading {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
} table_reading;

static sxml_status read_table_content(void *state, sxml_byte_view content) {
    table_reading *reading = state;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        reading->allocator,
        (sxml_allocation_request){.byte_count = content.byte_count + 1, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (content.byte_count > 0) {
        memcpy(memory, content.bytes, content.byte_count);
    }
    reading->bytes = memory;
    reading->byte_count = content.byte_count;
    return SXML_STATUS_OK;
}

static const sxml_feature_data_reader_operations table_reader_operations = {
    .read = read_table_content,
};

/* True for a zone name sxml asks the resolver for (include/sxml/
   processor.h, SXML_RESOURCE_KIND_TIMEZONE_DATA): parts of ASCII letters,
   digits, "_", "-" and "+" between single "/". */
static bool is_zone_name(sxml_byte_view name) {
    if (name.byte_count == 0 || name.byte_count > SXML_TIMEZONE_MAXIMUM_ZONE_NAME_BYTE_COUNT) {
        return false;
    }
    bool follows_separator = true;
    for (size_t byte_index = 0; byte_index < name.byte_count; byte_index += 1) {
        const uint8_t byte = name.bytes[byte_index];
        if (byte == '/') {
            if (follows_separator) {
                return false;
            }
            follows_separator = true;
            continue;
        }
        if (!(is_ascii_letter(byte) || is_ascii_digit(byte) || byte == '_' || byte == '-' ||
              byte == '+')) {
            return false;
        }
        follows_separator = false;
    }
    return !follows_separator;
}

/* Keeps a zone just read as a new entry of database; takes it in every
   case. */
static sxml_status keep_zone(timezone_database_state *database, zone_reading *reading,
                             sxml_byte_view name) {
    const sxml_allocator allocator = reading->allocator;
    sxml_status status = SXML_STATUS_OK;
    if (database->entry_count == database->entry_capacity) {
        const size_t capacity = database->entry_capacity == 0 ? 8 : database->entry_capacity * 2;
        void *memory = NULL;
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = capacity * sizeof(zone_entry),
                                      .alignment = alignof(zone_entry)},
            &memory);
        if (status == SXML_STATUS_OK) {
            if (database->entry_count > 0) {
                memcpy(memory, database->entries, database->entry_count * sizeof(zone_entry));
            }
            if (database->entries != NULL) {
                sxml_allocator_release(allocator, database->entries);
            }
            database->entries = memory;
            database->entry_capacity = capacity;
        }
    }
    void *name_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator, (sxml_allocation_request){.byte_count = name.byte_count, .alignment = 1},
            &name_memory);
    }
    if (status != SXML_STATUS_OK) {
        sxml_timezone_zone_finalize(&reading->zone, allocator);
        return status;
    }
    memcpy(name_memory, name.bytes, name.byte_count);
    database->entries[database->entry_count] = (zone_entry){
        .name_bytes = name_memory, .name_byte_count = name.byte_count, .zone = reading->zone};
    database->entry_count += 1;
    return SXML_STATUS_OK;
}

/* The zone of name, read now unless it was before; *zone is NULL for a
   zone the resolver does not deliver as a valid TZif file (not kept, so
   asked again next time). */
static sxml_status find_zone(sxml_processor *processor, sxml_byte_view name,
                             const sxml_timezone_zone **zone) {
    *zone = NULL;
    if (!is_zone_name(name)) {
        return SXML_STATUS_OK;
    }
    timezone_database_state *database = NULL;
    sxml_status status = get_state(processor, &database);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t entry_index = 0; entry_index < database->entry_count; entry_index += 1) {
        const zone_entry *entry = &database->entries[entry_index];
        if (sxml_byte_view_equals(
                (sxml_byte_view){.bytes = entry->name_bytes, .byte_count = entry->name_byte_count},
                name)) {
            *zone = &entry->zone;
            return SXML_STATUS_OK;
        }
    }
    zone_reading reading = {.allocator = processor->allocator, .is_valid = false};
    memset(&reading.zone, 0, sizeof(reading.zone));
    sxml_feature_data_outcome outcome = {.is_available = false};
    status = read_data_file(&(data_file_request){.processor = processor,
                                                 .path = name,
                                                 .operations = &zone_reader_operations,
                                                 .reader_state = &reading},
                            &outcome);
    if (status != SXML_STATUS_OK || !outcome.is_available || !reading.is_valid) {
        sxml_timezone_zone_finalize(&reading.zone, processor->allocator);
        return status;
    }
    status = keep_zone(database, &reading, name);
    if (status == SXML_STATUS_OK) {
        *zone = &database->entries[database->entry_count - 1].zone;
    }
    return status;
}

/* The zone table of database, read at the first call: zone1970.tab, else
   zone.tab. */
static sxml_status read_zone_table(sxml_processor *processor,
                                   timezone_database_state *database) {
    static const char *const table_names[] = {"zone1970.tab", "zone.tab"};
    if (database->has_read_table) {
        return SXML_STATUS_OK;
    }
    for (size_t table_index = 0; table_index < sizeof(table_names) / sizeof(table_names[0]);
         table_index += 1) {
        table_reading reading = {.allocator = processor->allocator, .bytes = NULL, .byte_count = 0};
        sxml_feature_data_outcome outcome = {.is_available = false};
        const sxml_status status = read_data_file(
            &(data_file_request){.processor = processor,
                                 .path = sxml_byte_view_from_c_string(table_names[table_index]),
                                 .operations = &table_reader_operations,
                                 .reader_state = &reading},
            &outcome);
        if (status != SXML_STATUS_OK) {
            if (reading.bytes != NULL) {
                sxml_allocator_release(processor->allocator, reading.bytes);
            }
            return status;
        }
        if (outcome.is_available) {
            database->table_bytes = reading.bytes;
            database->table_byte_count = reading.byte_count;
            break;
        }
    }
    database->has_read_table = true;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Places */

static uint8_t to_upper_case(uint8_t byte) {
    return byte >= 'a' && byte <= 'z' ? (uint8_t)(byte - 'a' + 'A') : byte;
}

/* True when the codes field of a zone table line ("US" or "CH,DE,LI")
   lists code (two upper-case letters). */
static bool codes_list(sxml_byte_view codes, const uint8_t code[2]) {
    for (size_t start = 0; start + 2 <= codes.byte_count; start += 3) {
        if (codes.bytes[start] == code[0] && codes.bytes[start + 1] == code[1]) {
            return true;
        }
    }
    return false;
}

/* One line of a zone table, split at its tabs. */
typedef struct table_line {
    sxml_byte_view codes;
    sxml_byte_view zone_name;
} table_line;

/* Reads the line that starts at *position and moves past it; false for a
   comment or a line without three fields. */
static bool read_table_line(sxml_byte_view table, size_t *position, table_line *line) {
    sxml_byte_view fields[3];
    size_t field_count = 0;
    size_t field_start = *position;
    size_t byte_index = *position;
    const bool is_comment = byte_index < table.byte_count && table.bytes[byte_index] == '#';
    while (byte_index < table.byte_count && table.bytes[byte_index] != '\n') {
        if (table.bytes[byte_index] == '\t') {
            if (field_count < 3) {
                fields[field_count] = (sxml_byte_view){.bytes = table.bytes + field_start,
                                                       .byte_count = byte_index - field_start};
                field_count += 1;
            }
            field_start = byte_index + 1;
        }
        byte_index += 1;
    }
    if (field_count < 3) {
        fields[field_count] = (sxml_byte_view){.bytes = table.bytes + field_start,
                                               .byte_count = byte_index - field_start};
        field_count += 1;
    }
    *position = byte_index + 1;
    if (is_comment || field_count < 3) {
        return false;
    }
    line->codes = fields[0];
    line->zone_name = fields[2];
    return true;
}

/* The local time type of zone for request's moment. */
static sxml_local_time_type local_time_of(const sxml_timezone_zone *zone,
                                          const sxml_timezone_place_request *request) {
    return request->has_instant ? sxml_timezone_zone_find(zone, request->instant_seconds)
                                : sxml_timezone_zone_standard_time(zone);
}

/* A country code: its zones in the table's order, the first with the
   request's offset answering. */
static sxml_status find_country(const sxml_timezone_place_request *request,
                                sxml_timezone_place_outcome *outcome) {
    timezone_database_state *database = NULL;
    sxml_status status = get_state(request->processor, &database);
    if (status == SXML_STATUS_OK) {
        status = read_zone_table(request->processor, database);
    }
    if (status != SXML_STATUS_OK || database->table_bytes == NULL) {
        return status;
    }
    const uint8_t code[2] = {to_upper_case(request->place.bytes[0]),
                             to_upper_case(request->place.bytes[1])};
    const sxml_byte_view table = {.bytes = database->table_bytes,
                                  .byte_count = database->table_byte_count};
    size_t position = 0;
    while (position < table.byte_count) {
        table_line line;
        if (!read_table_line(table, &position, &line) || !codes_list(line.codes, code)) {
            continue;
        }
        outcome->place_kind = SXML_TIMEZONE_PLACE_KIND_COUNTRY;
        const sxml_timezone_zone *zone = NULL;
        status = find_zone(request->processor, line.zone_name, &zone);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (zone == NULL) {
            continue;
        }
        const sxml_local_time_type local_time = local_time_of(zone, request);
        if (local_time.offset_seconds == request->country_offset_seconds) {
            outcome->has_local_time = true;
            outcome->local_time = local_time;
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status find_place(const sxml_timezone_place_request *request,
                              sxml_timezone_place_outcome *outcome) {
    memset(outcome, 0, sizeof(*outcome));
    outcome->place_kind = SXML_TIMEZONE_PLACE_KIND_UNKNOWN;
    if (request->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_byte_view place = request->place;
    if (request->accepts_country_code && place.byte_count == 2 &&
        is_ascii_letter(place.bytes[0]) && is_ascii_letter(place.bytes[1])) {
        const sxml_status status = find_country(request, outcome);
        if (status != SXML_STATUS_OK) {
            memset(outcome, 0, sizeof(*outcome));
        }
        return status;
    }
    const sxml_timezone_zone *zone = NULL;
    const sxml_status status = find_zone(request->processor, place, &zone);
    if (status == SXML_STATUS_OK && zone != NULL) {
        outcome->place_kind = SXML_TIMEZONE_PLACE_KIND_ZONE;
        outcome->has_local_time = true;
        outcome->local_time = local_time_of(zone, request);
    }
    return status;
}

/* ---------------------------------------------------------- Definition */

static const sxml_timezone_feature_operations timezone_feature_operations = {
    .find_place = find_place,
};

const sxml_optional_feature_definition sxml_timezone_database_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_TIMEZONE_DATABASE,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("timezone-database"),
    .operations = &timezone_feature_operations,
    .finalize_state = finalize_state,
    .library_functions = NULL,
};

sxml_optional_feature sxml_timezone_database_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_timezone_database_feature_definition};
}
