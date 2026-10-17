/*
 * The date picture engine (packet 2.14, D97, D119): pictures of
 * fn:format-dateTime, fn:format-date and fn:format-time (F&O 4.0, section
 * 9.9.4). See src/format_date_internal.h.
 *
 * Golden file: src/format.c (hand-scanned pictures over views, scratch
 * text through the caller's allocator, every early return releasing what
 * it took).
 *
 * A picture is scanned three times: for its syntax (FOFD1340), for the
 * components its value lacks (FOFD1350), and to write. Each variable
 * marker is copied without whitespace into scratch text and split into
 * component specifier, presentation modifiers and width modifier
 * (section 9.9.4.1). Numbers go through the fn:format-integer engine
 * (src/format_internal.h), with the digit pattern widened for a minimum
 * width (section 9.9.4.3) in a second scratch text.
 *
 * Calendar arithmetic runs on a year of the same 400-year Gregorian cycle
 * (146,097 days, a multiple of 7), so day of week and week numbers are
 * right for every int64_t year without overflow.
 */
#include "format_date_internal.h"

#include "regex_internal.h"
#include "xml_internal.h"

#include <string.h>

/* --------------------------------------------------------- Calendar */

/* A proleptic Gregorian date. */
typedef struct civil_date {
    int64_t year;
    int64_t month;
    int64_t day;
} civil_date;

/* Days since 1970-01-01 (H. Hinnant's days_from_civil). */
static int64_t days_from_civil(civil_date date) {
    const int64_t year = date.month <= 2 ? date.year - 1 : date.year;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t year_of_era = year - era * 400;
    const int64_t shifted_month = date.month > 2 ? date.month - 3 : date.month + 9;
    const int64_t day_of_era_year = (153 * shifted_month + 2) / 5 + date.day - 1;
    const int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_era_year;
    return era * 146097 + day_of_era - 719468;
}

/* The date of a day number (H. Hinnant's civil_from_days). */
static civil_date civil_from_days(int64_t day_number) {
    const int64_t shifted = day_number + 719468;
    const int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const int64_t day_of_era = shifted - era * 146097;
    const int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t shifted_month = (5 * day_of_year + 2) / 153;
    const int64_t day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    const int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    return (civil_date){
        .year = year_of_era + era * 400 + (month <= 2 ? 1 : 0), .month = month, .day = day};
}

/* A year from 2000 to 2399 with the same calendar as year. */
static int64_t cycle_year(int64_t year) {
    return 2000 + ((year % 400) + 400) % 400;
}

static bool is_leap_year(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

/* 1 (Monday) to 7 (Sunday), as ISO 8601 numbers days (section 9.9.4.8). */
static int64_t iso_weekday(int64_t day_number) {
    /* 1970-01-01 was a Thursday. */
    return ((day_number % 7) + 7 + 3) % 7 + 1;
}

static int64_t day_of_year(civil_date date) {
    return days_from_civil(date) - days_from_civil((civil_date){date.year, 1, 1}) + 1;
}

/* 52 or 53: a year has 53 ISO weeks when it starts on a Thursday, or on a
   Wednesday in a leap year. */
static int64_t iso_weeks_in_year(int64_t year) {
    const int64_t first_weekday = iso_weekday(days_from_civil((civil_date){year, 1, 1}));
    return first_weekday == 4 || (first_weekday == 3 && is_leap_year(year)) ? 53 : 52;
}

/* The ISO 8601 week number: week 1 holds the year's first Thursday; days
   before it belong to the last week of the year before (section 9.9.4.8). */
static int64_t iso_week_of_year(civil_date date) {
    const int64_t weekday = iso_weekday(days_from_civil(date));
    const int64_t week = (day_of_year(date) - weekday + 10) / 7;
    if (week < 1) {
        return iso_weeks_in_year(date.year - 1);
    }
    return week > iso_weeks_in_year(date.year) ? 1 : week;
}

/* The week in month: a Monday-to-Sunday week belongs to the month of its
   Thursday, and is numbered among that month's weeks from 1 (section
   9.9.4.8). */
static int64_t week_of_month(civil_date date) {
    const int64_t day_number = days_from_civil(date);
    const civil_date thursday = civil_from_days(day_number + 4 - iso_weekday(day_number));
    return (thursday.day - 1) / 7 + 1;
}

/* The components a picture can name, computed once. */
typedef struct date_components {
    /* The year as written: in AD 1 - year for year 0 and before (1 BC),
       in ISO its absolute value. */
    uint64_t year;
    bool is_before_era;
    int64_t month;
    int64_t day;
    int64_t day_of_year;
    int64_t weekday;
    int64_t week_of_year;
    int64_t week_of_month;
    int64_t hour;
    int64_t minute;
    int64_t second;
    uint32_t nanosecond;
    bool has_timezone;
    int64_t timezone_offset_minutes;
} date_components;

static date_components compute_components(const sxml_date_formatting *formatting) {
    const sxml_date_time value = formatting->value;
    date_components components = {.hour = value.hour,
                                  .minute = value.minute,
                                  .second = value.second,
                                  .nanosecond = value.nanosecond,
                                  .has_timezone = value.has_timezone,
                                  .timezone_offset_minutes = value.timezone_offset_minutes};
    if (formatting->value_kind == SXML_DATE_VALUE_KIND_TIME) {
        return components;
    }
    /* |year| without overflow for INT64_MIN. */
    const uint64_t magnitude =
        value.year < 0 ? (uint64_t)(-(value.year + 1)) + 1U : (uint64_t)value.year;
    if (formatting->calendar.calendar_kind == SXML_CALENDAR_KIND_ISO) {
        components.year = magnitude;
        components.is_before_era = value.year < 0;
    } else {
        /* "the year before 1 CE is thus 1 BCE" (section 9.9.4.8). */
        components.year = value.year <= 0 ? magnitude + 1U : magnitude;
        components.is_before_era = value.year <= 0;
    }
    const civil_date date = {
        .year = cycle_year(value.year), .month = value.month, .day = value.day};
    components.month = value.month;
    components.day = value.day;
    components.day_of_year = day_of_year(date);
    components.weekday = iso_weekday(days_from_civil(date));
    components.week_of_year = iso_week_of_year(date);
    components.week_of_month = week_of_month(date);
    return components;
}

/* ---------------------------------------------------- Calendar names */

/* The designators of section 9.9.4.8. */
static const char *const calendar_designators[] = {
    "AD", "AH", "AME", "AM", "AP", "AS", "BE", "CB", "CE", "CL", "CS", "EE", "FE", "ISO", "JE",
    "KE", "KY", "ME",  "MS", "NS", "OS", "RS", "SE", "SH", "SS", "TE", "VE", "VS",
};

static bool is_ncname(sxml_byte_view text) {
    return text.byte_count > 0 && memchr(text.bytes, ':', text.byte_count) == NULL &&
           sxml_xml_text_is_name(text);
}

sxml_status sxml_calendar_choose(sxml_byte_view calendar_name, sxml_calendar_choice *choice) {
    const sxml_calendar_choice fallback = {.calendar_kind = SXML_CALENDAR_KIND_AD,
                                           .needs_fallback_marker = true};
    sxml_byte_view local_name = calendar_name;
    bool is_in_namespace = false;
    if (calendar_name.byte_count >= 2 && calendar_name.bytes[0] == 'Q' &&
        calendar_name.bytes[1] == '{') {
        /* A URIQualifiedName: Q{uri}local, the URI without "{" or "}". */
        const uint8_t *close = memchr(calendar_name.bytes + 2, '}', calendar_name.byte_count - 2);
        if (close == NULL) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
        const size_t uri_byte_count = (size_t)(close - calendar_name.bytes) - 2;
        if (memchr(calendar_name.bytes + 2, '{', uri_byte_count) != NULL) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
        is_in_namespace = uri_byte_count > 0;
        local_name = (sxml_byte_view){
            .bytes = close + 1,
            .byte_count = calendar_name.byte_count - uri_byte_count - 3};
    } else {
        const uint8_t *colon = calendar_name.byte_count == 0
                                   ? NULL
                                   : memchr(calendar_name.bytes, ':', calendar_name.byte_count);
        if (colon != NULL) {
            const sxml_byte_view prefix = {.bytes = calendar_name.bytes,
                                           .byte_count = (size_t)(colon - calendar_name.bytes)};
            if (!is_ncname(prefix)) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            is_in_namespace = true;
            local_name = (sxml_byte_view){
                .bytes = colon + 1, .byte_count = calendar_name.byte_count - prefix.byte_count - 1};
        }
    }
    if (!is_ncname(local_name)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    if (is_in_namespace) {
        /* "If the expanded QName is in a namespace then it identifies the
           calendar in an implementation-defined way": none is known. */
        *choice = fallback;
        return SXML_STATUS_OK;
    }
    if (sxml_byte_view_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("AD"))) {
        *choice = (sxml_calendar_choice){.calendar_kind = SXML_CALENDAR_KIND_AD,
                                         .needs_fallback_marker = false};
        return SXML_STATUS_OK;
    }
    if (sxml_byte_view_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("ISO"))) {
        *choice = (sxml_calendar_choice){.calendar_kind = SXML_CALENDAR_KIND_ISO,
                                         .needs_fallback_marker = false};
        return SXML_STATUS_OK;
    }
    for (size_t designator_index = 0;
         designator_index < sizeof(calendar_designators) / sizeof(calendar_designators[0]);
         designator_index += 1) {
        if (sxml_byte_view_equals(local_name, sxml_byte_view_from_c_string(
                                                  calendar_designators[designator_index]))) {
            *choice = fallback;
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_MALFORMED_INPUT;
}

static uint8_t to_lower_ascii(uint8_t byte) {
    return byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte - 'A' + 'a') : byte;
}

static uint8_t to_upper_ascii(uint8_t byte) {
    return byte >= 'a' && byte <= 'z' ? (uint8_t)(byte - 'a' + 'A') : byte;
}

bool sxml_date_language_is_english(sxml_byte_view language) {
    return language.byte_count >= 2 && to_lower_ascii(language.bytes[0]) == 'e' &&
           to_lower_ascii(language.bytes[1]) == 'n' &&
           (language.byte_count == 2 || language.bytes[2] == '-');
}

/* ----------------------------------------------------- Variable markers */

/* The width modifier: ","  min-width ("-" max-width)? (section 9.9.4.2). */
typedef struct width_modifier {
    bool has_minimum;
    size_t minimum;
    bool has_maximum;
    size_t maximum;
} width_modifier;

/* How a component is named: the first presentation modifiers N, n and
   Nn. */
typedef enum name_style {
    SXML_NAME_STYLE_NONE,
    SXML_NAME_STYLE_UPPER,
    SXML_NAME_STYLE_LOWER,
    SXML_NAME_STYLE_TITLE,
} name_style;

/* A variable marker split into its parts (section 9.9.4.1). */
typedef struct variable_marker {
    uint8_t component_specifier;
    /* Borrowed from the scratch text or static data; never empty. */
    sxml_byte_view first_modifier;
    name_style style;
    /* 0, or one of 'a', 't', 'c', 'o'. */
    uint8_t second_modifier;
    width_modifier width;
} variable_marker;

/* The default presentation modifier of each component (the table of
   section 9.9.4.1). */
static sxml_byte_view default_modifier(uint8_t component_specifier) {
    switch (component_specifier) {
    case 'F':
    case 'P':
    case 'C':
    case 'E':
        return SXML_BYTE_VIEW_FROM_LITERAL("n");
    case 'm':
    case 's':
        return SXML_BYTE_VIEW_FROM_LITERAL("01");
    case 'Z':
    case 'z':
        return SXML_BYTE_VIEW_FROM_LITERAL("01:01");
    default:
        return SXML_BYTE_VIEW_FROM_LITERAL("1");
    }
}

/* True when byte is one of the bytes of set. */
static bool is_one_of(uint8_t byte, sxml_byte_view set) {
    for (size_t byte_index = 0; byte_index < set.byte_count; byte_index += 1) {
        if (set.bytes[byte_index] == byte) {
            return true;
        }
    }
    return false;
}

static bool is_component_specifier(uint8_t byte) {
    return is_one_of(byte, SXML_BYTE_VIEW_FROM_LITERAL("YMDdFWwHhPmsfZzCE"));
}

/* Parses an unsigned width or "*" (absent); false for anything else and
   for zero (section 9.9.4.2: less than one is FOFD1340). Saturates at
   SIZE_MAX / 8. */
typedef struct width_number {
    bool is_present;
    size_t value;
} width_number;

static bool parse_width_number(sxml_byte_view text, width_number *number) {
    if (text.byte_count == 1 && text.bytes[0] == '*') {
        *number = (width_number){.is_present = false, .value = 0};
        return true;
    }
    if (text.byte_count == 0) {
        return false;
    }
    size_t value = 0;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte < '0' || byte > '9') {
            return false;
        }
        if (value < SIZE_MAX / 80U) {
            value = value * 10U + (size_t)(byte - '0');
        }
    }
    if (value == 0) {
        return false;
    }
    *number = (width_number){.is_present = true, .value = value};
    return true;
}

static bool parse_width_modifier(sxml_byte_view text, width_modifier *width) {
    const uint8_t *hyphen = text.byte_count == 0 ? NULL : memchr(text.bytes, '-', text.byte_count);
    const size_t minimum_byte_count =
        hyphen == NULL ? text.byte_count : (size_t)(hyphen - text.bytes);
    width_number minimum;
    width_number maximum = {.is_present = false, .value = 0};
    if (!parse_width_number((sxml_byte_view){.bytes = text.bytes, .byte_count = minimum_byte_count},
                            &minimum)) {
        return false;
    }
    if (hyphen != NULL &&
        !parse_width_number((sxml_byte_view){.bytes = hyphen + 1,
                                             .byte_count = text.byte_count - minimum_byte_count - 1},
                            &maximum)) {
        return false;
    }
    if (minimum.is_present && maximum.is_present && maximum.value < minimum.value) {
        return false;
    }
    *width = (width_modifier){.has_minimum = minimum.is_present,
                              .minimum = minimum.value,
                              .has_maximum = maximum.is_present,
                              .maximum = maximum.value};
    return true;
}

/* The scratch texts of one call and the text written to. */
typedef struct date_writer {
    const sxml_date_formatting *formatting;
    date_components components;
    sxml_format_text *text;
    /* The marker being parsed, without whitespace. */
    sxml_format_text marker_text;
    /* A digit pattern being widened or reversed. */
    sxml_format_text pattern_text;
} date_writer;

static void reset_text(sxml_format_text *text) {
    text->byte_count = 0;
}

/* Writes the codepoints of token in reverse order to the pattern text and
   parses that as a primary format token (section 9.9.4.5: fractional
   seconds use the reversed digit pattern). */
static sxml_status parse_reversed_token(date_writer *writer, sxml_byte_view token,
                                        sxml_integer_picture *picture) {
    reset_text(&writer->pattern_text);
    sxml_status status = SXML_STATUS_OK;
    size_t end_offset = token.byte_count;
    while (status == SXML_STATUS_OK && end_offset > 0) {
        size_t start_offset = end_offset - 1;
        while (start_offset > 0 && (token.bytes[start_offset] & 0xC0U) == 0x80U) {
            start_offset -= 1;
        }
        status = sxml_format_text_append(
            &writer->pattern_text, (sxml_byte_view){.bytes = token.bytes + start_offset,
                                                    .byte_count = end_offset - start_offset});
        end_offset = start_offset;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_integer_picture_parse_primary_format_token(
            sxml_format_text_view(&writer->pattern_text), picture);
    }
    return status;
}

/* The scratch text's view of the first modifier must outlive the next
   use of the pattern text, so a reversed parse only checks the token. */
static sxml_status check_first_modifier(date_writer *writer, const variable_marker *marker) {
    sxml_integer_picture picture;
    if (marker->style != SXML_NAME_STYLE_NONE) {
        return SXML_STATUS_OK;
    }
    if (marker->component_specifier == 'f') {
        return parse_reversed_token(writer, marker->first_modifier, &picture);
    }
    return sxml_integer_picture_parse_primary_format_token(marker->first_modifier, &picture);
}

/* A marker's text between "[" and "]". */
typedef struct marker_source {
    sxml_byte_view marker_text;
} marker_source;

/*
 * Splits a marker (section 9.9.4.1): whitespace is ignored; the component
 * specifier is one letter; the last comma introduces the width modifier;
 * of two or more modifier characters, a last 'a', 't', 'c' or 'o' is the
 * second presentation modifier. Returns SXML_STATUS_MALFORMED_INPUT for a
 * syntax error.
 */
static sxml_status parse_marker(date_writer *writer, marker_source source,
                                variable_marker *marker) {
    reset_text(&writer->marker_text);
    sxml_status status = SXML_STATUS_OK;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < source.marker_text.byte_count;
         byte_index += 1) {
        const uint8_t byte = source.marker_text.bytes[byte_index];
        if (!sxml_xml_byte_is_whitespace(byte)) {
            status = sxml_format_text_append(&writer->marker_text,
                                             (sxml_byte_view){.bytes = &byte, .byte_count = 1});
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view compact = sxml_format_text_view(&writer->marker_text);
    if (compact.byte_count == 0 || !is_component_specifier(compact.bytes[0])) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    sxml_byte_view modifiers = {.bytes = compact.bytes + 1, .byte_count = compact.byte_count - 1};
    width_modifier width = {
        .has_minimum = false, .minimum = 0, .has_maximum = false, .maximum = 0};
    for (size_t byte_offset = modifiers.byte_count; byte_offset > 0; byte_offset -= 1) {
        if (modifiers.bytes[byte_offset - 1] == ',') {
            if (!parse_width_modifier(
                    (sxml_byte_view){.bytes = modifiers.bytes + byte_offset,
                                     .byte_count = modifiers.byte_count - byte_offset},
                    &width)) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            modifiers.byte_count = byte_offset - 1;
            break;
        }
    }
    uint8_t second_modifier = 0;
    if (modifiers.byte_count >= 2 &&
        is_one_of(modifiers.bytes[modifiers.byte_count - 1], SXML_BYTE_VIEW_FROM_LITERAL("atco"))) {
        second_modifier = modifiers.bytes[modifiers.byte_count - 1];
        modifiers.byte_count -= 1;
    }
    *marker = (variable_marker){
        .component_specifier = compact.bytes[0],
        .first_modifier = modifiers.byte_count == 0 ? default_modifier(compact.bytes[0]) : modifiers,
        .style = SXML_NAME_STYLE_NONE,
        .second_modifier = second_modifier,
        .width = width};
    if (sxml_byte_view_equals(marker->first_modifier, SXML_BYTE_VIEW_FROM_LITERAL("N"))) {
        marker->style = SXML_NAME_STYLE_UPPER;
    } else if (sxml_byte_view_equals(marker->first_modifier, SXML_BYTE_VIEW_FROM_LITERAL("n"))) {
        marker->style = SXML_NAME_STYLE_LOWER;
    } else if (sxml_byte_view_equals(marker->first_modifier, SXML_BYTE_VIEW_FROM_LITERAL("Nn"))) {
        marker->style = SXML_NAME_STYLE_TITLE;
    }
    return check_first_modifier(writer, marker);
}

/* True when the value of the formatting function has the component
   (section 9.9.4.1, FOFD1350). */
static bool has_component(sxml_date_value_kind value_kind, uint8_t component_specifier) {
    const bool is_date_component = is_one_of(component_specifier, SXML_BYTE_VIEW_FROM_LITERAL("YMDdFWwE"));
    const bool is_time_component = is_one_of(component_specifier, SXML_BYTE_VIEW_FROM_LITERAL("HhPmsf"));
    return !(is_date_component && value_kind == SXML_DATE_VALUE_KIND_TIME) &&
           !(is_time_component && value_kind == SXML_DATE_VALUE_KIND_DATE);
}

/* ---------------------------------------------------------- Numbers */

/* Appends value as decimal digits of the family of zero_digit, padded
   with zeros to minimum_digit_count. */
typedef struct padded_number {
    uint64_t value;
    size_t minimum_digit_count;
    uint32_t zero_digit;
} padded_number;

static sxml_status append_padded_number(sxml_format_text *text, padded_number number) {
    uint8_t digits[24];
    size_t digit_count = 0;
    uint64_t remaining = number.value;
    do {
        digits[digit_count] = (uint8_t)(remaining % 10U);
        digit_count += 1;
        remaining /= 10U;
    } while (remaining > 0);
    sxml_status status = SXML_STATUS_OK;
    for (size_t pad_index = digit_count;
         status == SXML_STATUS_OK && pad_index < number.minimum_digit_count; pad_index += 1) {
        status = sxml_format_text_append_codepoint(text, number.zero_digit);
    }
    for (size_t digit_index = digit_count; status == SXML_STATUS_OK && digit_index > 0;
         digit_index -= 1) {
        status = sxml_format_text_append_codepoint(text, number.zero_digit +
                                                             digits[digit_index - 1]);
    }
    return status;
}

/* The number of codepoints from byte_offset to the end of text. */
static size_t count_codepoints_after(const sxml_format_text *text, size_t byte_offset) {
    size_t codepoint_count = 0;
    for (size_t byte_index = byte_offset; byte_index < text->byte_count; byte_index += 1) {
        if ((text->bytes[byte_index] & 0xC0U) != 0x80U) {
            codepoint_count += 1;
        }
    }
    return codepoint_count;
}

/* Appends spaces until the text after byte_offset has minimum
   codepoints (section 9.9.4.3: "padded ... by appending spaces"). */
typedef struct space_padding {
    size_t byte_offset;
    const width_modifier *width;
} space_padding;

static sxml_status pad_with_spaces(sxml_format_text *text, space_padding padding) {
    if (!padding.width->has_minimum) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    for (size_t codepoint_count = count_codepoints_after(text, padding.byte_offset);
         status == SXML_STATUS_OK && codepoint_count < padding.width->minimum;
         codepoint_count += 1) {
        status = sxml_format_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL(" "));
    }
    return status;
}

/* The digit signs of a decimal digit pattern. */
typedef struct pattern_counts {
    size_t mandatory_count;
    size_t optional_count;
    size_t separator_count;
} pattern_counts;

static pattern_counts count_pattern(sxml_byte_view digit_pattern) {
    pattern_counts counts = {.mandatory_count = 0, .optional_count = 0, .separator_count = 0};
    size_t byte_offset = 0;
    while (byte_offset < digit_pattern.byte_count) {
        const sxml_xml_decoded_codepoint decoded =
            sxml_xml_decode_codepoint(digit_pattern, byte_offset);
        byte_offset += decoded.encoded_byte_count;
        if (decoded.codepoint == '#') {
            counts.optional_count += 1;
        } else if (sxml_regex_get_general_category(decoded.codepoint) ==
                   SXML_REGEX_GENERAL_CATEGORY_ND) {
            counts.mandatory_count += 1;
        } else {
            counts.separator_count += 1;
        }
    }
    return counts;
}

/*
 * Widens a decimal digit pattern without grouping separators to minimum
 * mandatory digits (section 9.9.4.3): optional digit signs become
 * mandatory from the right, then mandatory digits of the pattern's family
 * are prepended. Writes the pattern to the pattern text and parses it
 * into *picture.
 */
typedef struct pattern_widening {
    const sxml_integer_picture *picture;
    size_t minimum;
} pattern_widening;

static sxml_status widen_pattern(date_writer *writer, pattern_widening widening,
                                 sxml_integer_picture *picture) {
    const pattern_counts counts = count_pattern(widening.picture->digit_pattern);
    const size_t needed = widening.minimum - counts.mandatory_count;
    const size_t replaced = needed < counts.optional_count ? needed : counts.optional_count;
    const uint32_t zero_digit = widening.picture->zero_digit;
    reset_text(&writer->pattern_text);
    sxml_status status = SXML_STATUS_OK;
    for (size_t prepend_index = replaced; status == SXML_STATUS_OK && prepend_index < needed;
         prepend_index += 1) {
        status = sxml_format_text_append_codepoint(&writer->pattern_text, zero_digit);
    }
    const sxml_byte_view pattern = widening.picture->digit_pattern;
    size_t byte_offset = 0;
    size_t optional_index = 0;
    while (status == SXML_STATUS_OK && byte_offset < pattern.byte_count) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(pattern, byte_offset);
        byte_offset += decoded.encoded_byte_count;
        uint32_t codepoint = decoded.codepoint;
        if (codepoint == '#') {
            if (optional_index >= counts.optional_count - replaced) {
                codepoint = zero_digit;
            }
            optional_index += 1;
        }
        status = sxml_format_text_append_codepoint(&writer->pattern_text, codepoint);
    }
    if (status == SXML_STATUS_OK) {
        const bool is_ordinal = widening.picture->is_ordinal;
        status = sxml_integer_picture_parse_primary_format_token(
            sxml_format_text_view(&writer->pattern_text), picture);
        picture->is_ordinal = is_ordinal;
    }
    return status;
}

/* An integer-valued component (sections 9.9.4.3 and 9.9.4.4). */
typedef struct integer_component {
    const variable_marker *marker;
    uint64_t value;
    bool is_year;
} integer_component;

static sxml_status append_integer_component(date_writer *writer, integer_component component) {
    const variable_marker *marker = component.marker;
    /* A name for a component that has none: the default presentation
       modifier (section 9.9.4.1). */
    const sxml_byte_view token = marker->style == SXML_NAME_STYLE_NONE
                                     ? marker->first_modifier
                                     : default_modifier(marker->component_specifier);
    sxml_integer_picture picture;
    sxml_status status = sxml_integer_picture_parse_primary_format_token(token, &picture);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    picture.is_ordinal = marker->second_modifier == 'o';
    const bool is_decimal = picture.numbering_kind == SXML_NUMBERING_KIND_DIGITS &&
                            picture.radix == 10;
    const pattern_counts counts = is_decimal ? count_pattern(picture.digit_pattern)
                                             : (pattern_counts){0, 0, 0};
    uint64_t value = component.value;
    if (component.is_year) {
        /* The year modulo 10^N: N the maximum width, or the digit signs of
           a pattern of two or more (section 9.9.4.4). */
        size_t modulus_digits = 0;
        if (marker->width.has_maximum) {
            modulus_digits = marker->width.maximum;
        } else if (counts.mandatory_count + counts.optional_count >= 2) {
            modulus_digits = counts.mandatory_count + counts.optional_count;
        }
        if (modulus_digits > 0 && modulus_digits < 20) {
            uint64_t modulus = 1;
            for (size_t digit_index = 0; digit_index < modulus_digits; digit_index += 1) {
                modulus *= 10U;
            }
            value %= modulus;
        }
    }
    sxml_integer_picture widened = picture;
    if (is_decimal && counts.separator_count == 0 && marker->width.has_minimum &&
        marker->width.minimum > counts.mandatory_count) {
        status = widen_pattern(
            writer, (pattern_widening){.picture = &picture, .minimum = marker->width.minimum},
            &widened);
    }
    uint8_t digits[24];
    size_t digit_count = 0;
    do {
        digits[sizeof(digits) - 1 - digit_count] = (uint8_t)('0' + value % 10U);
        digit_count += 1;
        value /= 10U;
    } while (value > 0);
    const size_t start_offset = writer->text->byte_count;
    if (status == SXML_STATUS_OK) {
        status = sxml_format_integer(
            &(sxml_integer_formatting){
                .picture = &widened,
                .magnitude_digits = {.bytes = digits + sizeof(digits) - digit_count,
                                     .byte_count = digit_count},
                .is_negative = false},
            writer->text);
    }
    if (status == SXML_STATUS_OK && widened.numbering_kind == SXML_NUMBERING_KIND_TITLE_WORDS) {
        /* Date components in title case capitalize after a hyphen too
           ("Twenty-One", as the QT4 cases of section 9.9.5 expect), where
           fn:format-integer writes "Twenty-one" (D119). */
        uint8_t *bytes = writer->text->bytes;
        for (size_t byte_index = start_offset + 1; byte_index < writer->text->byte_count;
             byte_index += 1) {
            if (bytes[byte_index - 1] == '-') {
                bytes[byte_index] = to_upper_ascii(bytes[byte_index]);
            }
        }
    }
    if (status == SXML_STATUS_OK && !is_decimal) {
        status = pad_with_spaces(writer->text, (space_padding){.byte_offset = start_offset,
                                                               .width = &marker->width});
    }
    return status;
}

/* ------------------------------------------------------------ Names */

/* An English name and its conventional abbreviations, longest first. */
typedef struct component_name {
    const char *full_name;
    const char *const *abbreviations;
    size_t abbreviation_count;
    /* Always written in capitals (AD, BC, ISO): the case of section
       9.9.4.7's names is the implementation's. */
    bool is_acronym;
} component_name;

static const char *const monday_abbreviations[] = {"Mon"};
static const char *const tuesday_abbreviations[] = {"Tues", "Tue"};
static const char *const wednesday_abbreviations[] = {"Weds", "Wed"};
static const char *const thursday_abbreviations[] = {"Thurs", "Thur", "Thu"};
static const char *const friday_abbreviations[] = {"Fri"};
static const char *const saturday_abbreviations[] = {"Sat"};
static const char *const sunday_abbreviations[] = {"Sun"};

#define SXML_PLAIN_NAME(text) {(text), NULL, 0, false}
#define SXML_ACRONYM(text) {(text), NULL, 0, true}

static const component_name month_names[] = {
    SXML_PLAIN_NAME("January"),   SXML_PLAIN_NAME("February"), SXML_PLAIN_NAME("March"),
    SXML_PLAIN_NAME("April"),     SXML_PLAIN_NAME("May"),      SXML_PLAIN_NAME("June"),
    SXML_PLAIN_NAME("July"),      SXML_PLAIN_NAME("August"),   SXML_PLAIN_NAME("September"),
    SXML_PLAIN_NAME("October"),   SXML_PLAIN_NAME("November"), SXML_PLAIN_NAME("December"),
};

static const component_name weekday_names[] = {
    {"Monday", monday_abbreviations, 1, false},
    {"Tuesday", tuesday_abbreviations, 2, false},
    {"Wednesday", wednesday_abbreviations, 2, false},
    {"Thursday", thursday_abbreviations, 3, false},
    {"Friday", friday_abbreviations, 1, false},
    {"Saturday", saturday_abbreviations, 1, false},
    {"Sunday", sunday_abbreviations, 1, false},
};

static const component_name morning_name = SXML_PLAIN_NAME("am");
static const component_name afternoon_name = SXML_PLAIN_NAME("pm");
static const component_name anno_domini_name = SXML_ACRONYM("AD");
static const component_name before_christ_name = SXML_ACRONYM("BC");
static const component_name iso_calendar_name = SXML_ACRONYM("ISO");

/* A name to write for a marker. */
typedef struct name_request {
    const variable_marker *marker;
    const component_name *name;
} name_request;

/*
 * Appends the name in the marker's case, abbreviated to the maximum width
 * (a conventional abbreviation that fits, or the name cut at the end) and
 * padded with spaces to the minimum (section 9.9.4.3). A marker without a
 * name style gives the title case.
 */
static sxml_status append_name(date_writer *writer, name_request request) {
    const width_modifier *width = &request.marker->width;
    sxml_byte_view chosen = sxml_byte_view_from_c_string(request.name->full_name);
    if (width->has_maximum && chosen.byte_count > width->maximum) {
        bool has_abbreviation = false;
        for (size_t abbreviation_index = 0;
             !has_abbreviation && abbreviation_index < request.name->abbreviation_count;
             abbreviation_index += 1) {
            const sxml_byte_view abbreviation =
                sxml_byte_view_from_c_string(request.name->abbreviations[abbreviation_index]);
            if (abbreviation.byte_count <= width->maximum) {
                chosen = abbreviation;
                has_abbreviation = true;
            }
        }
        if (!has_abbreviation) {
            chosen.byte_count = width->maximum;
        }
    }
    const size_t start_offset = writer->text->byte_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < chosen.byte_count;
         byte_index += 1) {
        uint8_t byte = chosen.bytes[byte_index];
        const name_style style =
            request.name->is_acronym ? SXML_NAME_STYLE_UPPER : request.marker->style;
        switch (style) {
        case SXML_NAME_STYLE_UPPER:
            byte = to_upper_ascii(byte);
            break;
        case SXML_NAME_STYLE_LOWER:
            byte = to_lower_ascii(byte);
            break;
        case SXML_NAME_STYLE_TITLE:
        case SXML_NAME_STYLE_NONE:
            byte = byte_index == 0 ? to_upper_ascii(byte) : to_lower_ascii(byte);
            break;
        }
        status = sxml_format_text_append(writer->text,
                                         (sxml_byte_view){.bytes = &byte, .byte_count = 1});
    }
    if (status == SXML_STATUS_OK) {
        status = pad_with_spaces(writer->text,
                                 (space_padding){.byte_offset = start_offset, .width = width});
    }
    return status;
}

/* ---------------------------------------------------- Fractional seconds */

/*
 * Appends the fractional seconds (section 9.9.4.5), with the effect of
 * the section's reversal algorithm: the digits of the fraction truncated
 * to the digit signs of the adjusted pattern (unbounded for a single
 * mandatory digit without maximum width), padded with zeros to its
 * mandatory digits, trailing zeros beyond those dropped, and the
 * pattern's grouping separators placed from the left.
 */
static sxml_status append_fractional_seconds(date_writer *writer, const variable_marker *marker) {
    sxml_byte_view token = marker->style == SXML_NAME_STYLE_NONE ? marker->first_modifier
                                                                  : default_modifier('f');
    sxml_integer_picture picture;
    sxml_status status = parse_reversed_token(writer, token, &picture);
    if (status == SXML_STATUS_OK &&
        (picture.numbering_kind != SXML_NUMBERING_KIND_DIGITS || picture.radix != 10)) {
        /* "If the first presentation modifier contains no Unicode digit,
           then the output is implementation-defined": as for "1". */
        token = default_modifier('f');
        status = parse_reversed_token(writer, token, &picture);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const pattern_counts counts = count_pattern(token);
    const width_modifier width = marker->width;
    const bool is_unbounded = counts.mandatory_count == 1 && counts.optional_count == 0 &&
                              counts.separator_count == 0 && !width.has_maximum;
    size_t mandatory_count = counts.mandatory_count;
    if (width.has_minimum && width.minimum > mandatory_count) {
        mandatory_count = width.minimum;
    }
    size_t digit_sign_count = counts.mandatory_count + counts.optional_count;
    if (mandatory_count > digit_sign_count) {
        digit_sign_count = mandatory_count;
    }
    if (width.has_maximum && width.maximum > digit_sign_count) {
        digit_sign_count = width.maximum;
    }
    uint8_t digits[9];
    size_t digit_count = 0;
    uint32_t nanosecond = writer->components.nanosecond;
    for (size_t digit_index = 9; digit_index > 0; digit_index -= 1) {
        digits[digit_index - 1] = (uint8_t)(nanosecond % 10U);
        nanosecond /= 10U;
    }
    for (size_t digit_index = 0; digit_index < 9; digit_index += 1) {
        if (digits[digit_index] != 0) {
            digit_count = digit_index + 1;
        }
    }
    if (!is_unbounded && digit_count > digit_sign_count) {
        digit_count = digit_sign_count;
    }
    size_t output_count = digit_count > mandatory_count ? digit_count : mandatory_count;
    while (output_count > mandatory_count && output_count <= 9 && digits[output_count - 1] == 0) {
        output_count -= 1;
    }
    size_t written_count = 0;
    size_t byte_offset = 0;
    while (status == SXML_STATUS_OK && byte_offset < token.byte_count &&
           written_count < output_count) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(token, byte_offset);
        byte_offset += decoded.encoded_byte_count;
        const bool is_digit_sign = decoded.codepoint == '#' ||
                                   sxml_regex_get_general_category(decoded.codepoint) ==
                                       SXML_REGEX_GENERAL_CATEGORY_ND;
        if (is_digit_sign) {
            status = sxml_format_text_append_codepoint(
                writer->text,
                picture.zero_digit + (written_count < 9 ? digits[written_count] : 0U));
            written_count += 1;
        } else if (written_count > 0) {
            /* A separator stays only before a digit that is written. */
            status = sxml_format_text_append_codepoint(writer->text, decoded.codepoint);
        }
    }
    for (; status == SXML_STATUS_OK && written_count < output_count; written_count += 1) {
        status = sxml_format_text_append_codepoint(
            writer->text, picture.zero_digit + (written_count < 9 ? digits[written_count] : 0U));
    }
    return status;
}

/* ---------------------------------------------------------- Timezones */

/* The military timezone letters of offsets -12 to +12 hours (section
   9.9.4.6): Z, A to M without J, N to Y. */
static uint8_t military_letter(int64_t hours) {
    if (hours == 0) {
        return 'Z';
    }
    if (hours > 0) {
        return (uint8_t)(hours < 10 ? 'A' + hours - 1 : 'K' + hours - 10);
    }
    return (uint8_t)('N' - hours - 1);
}

/* The groups of a timezone digit pattern: the hours before the first
   grouping separator, the minutes after it. */
typedef struct timezone_layout {
    size_t hour_mandatory_count;
    size_t hour_digit_count;
    bool has_separator;
    uint32_t separator;
    size_t minute_digit_count;
} timezone_layout;

static timezone_layout read_timezone_layout(sxml_byte_view digit_pattern) {
    timezone_layout layout = {.hour_mandatory_count = 0,
                              .hour_digit_count = 0,
                              .has_separator = false,
                              .separator = 0,
                              .minute_digit_count = 0};
    size_t byte_offset = 0;
    while (byte_offset < digit_pattern.byte_count) {
        const sxml_xml_decoded_codepoint decoded =
            sxml_xml_decode_codepoint(digit_pattern, byte_offset);
        byte_offset += decoded.encoded_byte_count;
        const bool is_mandatory = decoded.codepoint != '#' &&
                                  sxml_regex_get_general_category(decoded.codepoint) ==
                                      SXML_REGEX_GENERAL_CATEGORY_ND;
        if (decoded.codepoint != '#' && !is_mandatory) {
            if (!layout.has_separator) {
                layout.has_separator = true;
                layout.separator = decoded.codepoint;
            }
        } else if (layout.has_separator) {
            layout.minute_digit_count += 1;
        } else {
            layout.hour_digit_count += 1;
            layout.hour_mandatory_count += is_mandatory ? 1U : 0U;
        }
    }
    return layout;
}

/*
 * Appends the timezone (section 9.9.4.6): nothing without one ("J" for
 * ZZ); a military letter for Z where one exists; for a decimal digit
 * pattern hours, then minutes after the pattern's separator, or after ":"
 * when not whole for one or two digits, or directly for three or more;
 * "Z" for UTC with "t". Names (N) need a timezone database (D97), so they
 * and every other token take the fallback format 01:01. z adds "GMT"
 * first. Padded with spaces to the minimum width, never shortened.
 */
static sxml_status append_timezone(date_writer *writer, const variable_marker *marker) {
    const date_components *components = &writer->components;
    const bool is_military = sxml_byte_view_equals(marker->first_modifier,
                                                   SXML_BYTE_VIEW_FROM_LITERAL("Z"));
    if (!components->has_timezone) {
        return is_military ? sxml_format_text_append(writer->text, SXML_BYTE_VIEW_FROM_LITERAL("J"))
                           : SXML_STATUS_OK;
    }
    const size_t start_offset = writer->text->byte_count;
    const int64_t offset = components->timezone_offset_minutes;
    const uint64_t magnitude = (uint64_t)(offset < 0 ? -offset : offset);
    sxml_status status = SXML_STATUS_OK;
    if (marker->component_specifier == 'z') {
        status = sxml_format_text_append(writer->text, SXML_BYTE_VIEW_FROM_LITERAL("GMT"));
    }
    if (is_military && offset % 60 == 0 && magnitude <= 720U) {
        const uint8_t letter = military_letter(offset / 60);
        if (status == SXML_STATUS_OK) {
            status = sxml_format_text_append(writer->text,
                                             (sxml_byte_view){.bytes = &letter, .byte_count = 1});
        }
        return status == SXML_STATUS_OK
                   ? pad_with_spaces(writer->text, (space_padding){.byte_offset = start_offset,
                                                                   .width = &marker->width})
                   : status;
    }
    sxml_integer_picture picture;
    if (sxml_integer_picture_parse_primary_format_token(marker->first_modifier, &picture) !=
            SXML_STATUS_OK ||
        picture.numbering_kind != SXML_NUMBERING_KIND_DIGITS || picture.radix != 10) {
        /* The fallback format 01:01. */
        picture = (sxml_integer_picture){.numbering_kind = SXML_NUMBERING_KIND_DIGITS,
                                         .digit_pattern = default_modifier('Z'),
                                         .radix = 10,
                                         .zero_digit = '0',
                                         .has_upper_case_digits = false,
                                         .is_ordinal = false,
                                         .numbering_variant = {.bytes = NULL, .byte_count = 0}};
    }
    const timezone_layout layout = read_timezone_layout(picture.digit_pattern);
    const uint64_t hours = magnitude / 60U;
    const uint64_t minutes = magnitude % 60U;
    if (status == SXML_STATUS_OK && marker->second_modifier == 't' && offset == 0) {
        status = sxml_format_text_append(writer->text, SXML_BYTE_VIEW_FROM_LITERAL("Z"));
    } else if (status == SXML_STATUS_OK) {
        status = sxml_format_text_append(writer->text, offset < 0
                                                           ? SXML_BYTE_VIEW_FROM_LITERAL("-")
                                                           : SXML_BYTE_VIEW_FROM_LITERAL("+"));
        size_t hour_digits = layout.hour_mandatory_count;
        bool writes_minutes = true;
        uint32_t separator = layout.separator;
        if (!layout.has_separator) {
            if (layout.hour_digit_count <= 2) {
                writes_minutes = minutes != 0;
                separator = ':';
            } else {
                hour_digits = hour_digits > 2 ? hour_digits - 2 : 1;
                separator = 0;
            }
        }
        if (status == SXML_STATUS_OK) {
            status = append_padded_number(writer->text,
                                          (padded_number){.value = hours,
                                                          .minimum_digit_count = hour_digits,
                                                          .zero_digit = picture.zero_digit});
        }
        if (status == SXML_STATUS_OK && writes_minutes && separator != 0) {
            status = sxml_format_text_append_codepoint(writer->text, separator);
        }
        if (status == SXML_STATUS_OK && writes_minutes) {
            status = append_padded_number(
                writer->text,
                (padded_number){.value = minutes,
                                .minimum_digit_count =
                                    layout.has_separator && layout.minute_digit_count > 2
                                        ? layout.minute_digit_count
                                        : 2,
                                .zero_digit = picture.zero_digit});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = pad_with_spaces(writer->text,
                                 (space_padding){.byte_offset = start_offset, .width = &marker->width});
    }
    return status;
}

/* --------------------------------------------------------- Components */

static sxml_status append_integer(date_writer *writer, const variable_marker *marker,
                                  int64_t value) {
    return append_integer_component(
        writer, (integer_component){.marker = marker, .value = (uint64_t)value, .is_year = false});
}

/* Appends the era (sections 9.9.4.7 and 9.9.4.8): AD or BC, and in the
   ISO calendar "-" for negative years and nothing otherwise. */
static sxml_status append_era(date_writer *writer, const variable_marker *marker) {
    if (writer->formatting->calendar.calendar_kind == SXML_CALENDAR_KIND_ISO) {
        return writer->components.is_before_era
                   ? sxml_format_text_append(writer->text, SXML_BYTE_VIEW_FROM_LITERAL("-"))
                   : SXML_STATUS_OK;
    }
    return append_name(writer,
                       (name_request){.marker = marker,
                                      .name = writer->components.is_before_era
                                                  ? &before_christ_name
                                                  : &anno_domini_name});
}

/* Appends one variable marker's component. P, C and E are always names
   (section 9.9.4.7 leaves them to the implementation). */
static sxml_status append_component(date_writer *writer, const variable_marker *marker) {
    const date_components *components = &writer->components;
    const bool is_named = marker->style != SXML_NAME_STYLE_NONE;
    switch (marker->component_specifier) {
    case 'Y':
        return append_integer_component(
            writer, (integer_component){.marker = marker, .value = components->year, .is_year = true});
    case 'M':
        return is_named ? append_name(writer, (name_request){.marker = marker,
                                                            .name = &month_names[components->month - 1]})
                        : append_integer(writer, marker, components->month);
    case 'D':
        return append_integer(writer, marker, components->day);
    case 'd':
        return append_integer(writer, marker, components->day_of_year);
    case 'F':
        return is_named ? append_name(writer,
                                      (name_request){.marker = marker,
                                                     .name = &weekday_names[components->weekday - 1]})
                        : append_integer(writer, marker, components->weekday);
    case 'W':
        return append_integer(writer, marker, components->week_of_year);
    case 'w':
        return append_integer(writer, marker, components->week_of_month);
    case 'H':
        return append_integer(writer, marker, components->hour);
    case 'h':
        return append_integer(writer, marker,
                              components->hour % 12 == 0 ? 12 : components->hour % 12);
    case 'P':
        return append_name(writer, (name_request){.marker = marker,
                                                  .name = components->hour < 12 ? &morning_name
                                                                                : &afternoon_name});
    case 'm':
        return append_integer(writer, marker, components->minute);
    case 's':
        return append_integer(writer, marker, components->second);
    case 'f':
        return append_fractional_seconds(writer, marker);
    case 'Z':
    case 'z':
        return append_timezone(writer, marker);
    case 'C':
        return append_name(writer,
                           (name_request){.marker = marker,
                                          .name = writer->formatting->calendar.calendar_kind ==
                                                          SXML_CALENDAR_KIND_ISO
                                                      ? &iso_calendar_name
                                                      : &anno_domini_name});
    default:
        return append_era(writer, marker);
    }
}

/* ----------------------------------------------------------- Pictures */

/* What a scan of the picture does. */
typedef enum scan_mode {
    SXML_SCAN_MODE_CHECK_SYNTAX,
    SXML_SCAN_MODE_CHECK_COMPONENTS,
    SXML_SCAN_MODE_WRITE,
} scan_mode;

/*
 * Scans the picture (section 9.9.4.1): "[" starts a variable marker that
 * the next "]" ends; "[[" and "]]" stand for one bracket; a lone "]" is a
 * syntax error. Sets *picture_error and returns
 * SXML_STATUS_MALFORMED_INPUT for a rejected picture.
 */
static sxml_status scan_picture(date_writer *writer, scan_mode mode,
                                sxml_date_picture_error *picture_error) {
    const sxml_byte_view picture = writer->formatting->picture_string;
    sxml_status status = SXML_STATUS_OK;
    size_t byte_offset = 0;
    while (status == SXML_STATUS_OK && byte_offset < picture.byte_count) {
        const uint8_t byte = picture.bytes[byte_offset];
        const bool is_doubled = byte_offset + 1 < picture.byte_count &&
                                picture.bytes[byte_offset + 1] == byte;
        if ((byte == '[' || byte == ']') && is_doubled) {
            if (mode == SXML_SCAN_MODE_WRITE) {
                status = sxml_format_text_append(writer->text,
                                                 (sxml_byte_view){.bytes = &byte, .byte_count = 1});
            }
            byte_offset += 2;
            continue;
        }
        if (byte == ']') {
            *picture_error = SXML_DATE_PICTURE_ERROR_SYNTAX;
            return SXML_STATUS_MALFORMED_INPUT;
        }
        if (byte != '[') {
            size_t end_offset = byte_offset + 1;
            while (end_offset < picture.byte_count && picture.bytes[end_offset] != '[' &&
                   picture.bytes[end_offset] != ']') {
                end_offset += 1;
            }
            if (mode == SXML_SCAN_MODE_WRITE) {
                status = sxml_format_text_append(
                    writer->text, (sxml_byte_view){.bytes = picture.bytes + byte_offset,
                                                   .byte_count = end_offset - byte_offset});
            }
            byte_offset = end_offset;
            continue;
        }
        const uint8_t *close =
            memchr(picture.bytes + byte_offset + 1, ']', picture.byte_count - byte_offset - 1);
        if (close == NULL) {
            *picture_error = SXML_DATE_PICTURE_ERROR_SYNTAX;
            return SXML_STATUS_MALFORMED_INPUT;
        }
        variable_marker marker;
        status = parse_marker(
            writer,
            (marker_source){.marker_text = {.bytes = picture.bytes + byte_offset + 1,
                                            .byte_count =
                                                (size_t)(close - picture.bytes) - byte_offset - 1}},
            &marker);
        if (status == SXML_STATUS_MALFORMED_INPUT) {
            *picture_error = SXML_DATE_PICTURE_ERROR_SYNTAX;
            return status;
        }
        if (status == SXML_STATUS_OK && mode == SXML_SCAN_MODE_CHECK_COMPONENTS &&
            !has_component(writer->formatting->value_kind, marker.component_specifier)) {
            *picture_error = SXML_DATE_PICTURE_ERROR_UNAVAILABLE_COMPONENT;
            return SXML_STATUS_MALFORMED_INPUT;
        }
        if (status == SXML_STATUS_OK && mode == SXML_SCAN_MODE_WRITE) {
            status = append_component(writer, &marker);
        }
        byte_offset = (size_t)(close - picture.bytes) + 1;
    }
    return status;
}

sxml_status sxml_format_date_time(const sxml_date_formatting *formatting, sxml_format_text *text,
                                  sxml_date_picture_error *picture_error) {
    *picture_error = SXML_DATE_PICTURE_ERROR_NONE;
    date_writer writer = {
        .formatting = formatting,
        .components = compute_components(formatting),
        .text = text,
        .marker_text = {.allocator = text->allocator, .bytes = NULL, .byte_count = 0, .byte_capacity = 0},
        .pattern_text = {.allocator = text->allocator, .bytes = NULL, .byte_count = 0, .byte_capacity = 0}};
    sxml_status status = scan_picture(&writer, SXML_SCAN_MODE_CHECK_SYNTAX, picture_error);
    if (status == SXML_STATUS_OK) {
        status = scan_picture(&writer, SXML_SCAN_MODE_CHECK_COMPONENTS, picture_error);
    }
    /* The fallback markers of section 9.9.4.8. */
    if (status == SXML_STATUS_OK && formatting->needs_language_marker) {
        status = sxml_format_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("[Language: en]"));
    }
    if (status == SXML_STATUS_OK && formatting->calendar.needs_fallback_marker) {
        status = sxml_format_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("[Calendar: AD]"));
    }
    if (status == SXML_STATUS_OK) {
        status = scan_picture(&writer, SXML_SCAN_MODE_WRITE, picture_error);
    }
    sxml_format_text_release(&writer.marker_text);
    sxml_format_text_release(&writer.pattern_text);
    return status;
}
