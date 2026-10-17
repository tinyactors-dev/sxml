/*
 * The temporal part of the function library (packet 2.8): the date, time
 * and duration functions that F&O 4.0 adds or changes beyond the component
 * functions of src/function_library_context.c.
 *
 * - fn:seconds, fn:duration-to-seconds (section 8.4);
 * - fn:build-dateTime, fn:unix-dateTime (section 9.4);
 * - fn:days-in-month, fn:parts-of-dateTime (section 9.6);
 * - fn:adjust-dateTime-to-timezone, fn:adjust-date-to-timezone,
 *   fn:adjust-time-to-timezone, fn:civil-timezone (section 9.7);
 * - fn:parse-ietf-date (section 9.10.1), scanned by hand (D97).
 *
 * Golden file: src/function_library_math.c (rows, argument reading); rows
 * as in src/function_library_internal.h.
 *
 * Arguments arrive coerced to the parameter types (XPath 4.0, section
 * 3.4). The record argument of fn:build-dateTime is declared as map(*)
 * because the row type text cannot spell optional record fields yet; the
 * body coerces each field to the type of fn:dateTime-record itself
 * (section 9.3), with the coercion rules' casts (xs:double and xs:float
 * to xs:decimal, xs:untypedAtomic to the field type) and relabelings
 * (an integral xs:decimal to xs:integer, an xs:duration without months to
 * xs:dayTimeDuration).
 *
 * Shifting a date or dateTime by minutes goes through the date arithmetic
 * of src/operator_temporal.c (D72), so every int64_t year works and
 * results beyond it are FODT0001. Seconds keep nine fraction digits;
 * further digits are truncated, as casting does (D36, D66).
 *
 * fn:civil-timezone has no timezone database (D97): with no place it gives
 * the implicit timezone (the default place is taken to be one whose civil
 * timezone is the implicit timezone), for the names of UTC it gives PT0S,
 * and for every other place FODT0004.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

static const sxml_static_error no_dynamic_context_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The function needs the dynamic context of an evaluation (XPath 4.0, "
        "section 2.2.2)"),
};

static const sxml_static_error invalid_timezone_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODT0003"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A timezone must be a whole number of minutes from -PT14H to PT14H (F&O 4.0, "
        "section 9.7)"),
};

static const sxml_static_error duration_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODT0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The duration is beyond the supported range (F&O 4.0, section 8.1.2)"),
};

static const sxml_static_error date_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODT0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The date/time value is beyond the supported years (F&O 4.0, section 9.1.2)"),
};

static const sxml_static_error not_a_record_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The value does not match fn:dateTime-record (F&O 4.0, section 9.3; XPath 4.0, "
        "section 3.4, coercion rules)"),
};

static const sxml_static_error invalid_component_set_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODT0005"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The components given are not those of a Gregorian type (F&O 4.0, "
        "fn:build-dateTime)"),
};

static const sxml_static_error component_out_of_range_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A component is outside the range of its type (F&O 4.0, fn:build-dateTime)"),
};

static const sxml_static_error no_civil_timezone_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODT0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "No timezone information is available for the place (F&O 4.0, "
        "fn:civil-timezone; sxml has no timezone database)"),
};

static const sxml_static_error invalid_ietf_date_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0010"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The string is not a valid IETF date (F&O 4.0, fn:parse-ietf-date)"),
};

/* ------------------------------------------------------- Gregorian values */

/* A Gregorian value (F&O 4.0, section 9.3): a date/time datum and the
   built-in type it is made as. */
typedef struct gregorian_value {
    sxml_builtin_type type;
    sxml_date_time date_time;
} gregorian_value;

static sxml_status make_gregorian_item(sxml_item_heap *item_heap, gregorian_value value,
                                       sxml_item *item) {
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(value.type),
            .datum = {.datum_kind = SXML_DATUM_KIND_DATE_TIME,
                      .datum_details = {.date_time = value.date_time}}},
        item);
}

static sxml_status make_day_time_duration(sxml_item_heap *item_heap, sxml_duration duration,
                                          sxml_item *item) {
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DAY_TIME_DURATION),
            .datum = {.datum_kind = SXML_DATUM_KIND_DURATION,
                      .datum_details = {.duration = duration}}},
        item);
}

/* Which components a Gregorian primitive type has (XDM 4.0, section 5.4;
   F&O 4.0, section 9.5): the others are absent. */
typedef struct gregorian_components {
    bool has_year;
    bool has_month;
    bool has_day;
    bool has_time;
} gregorian_components;

static gregorian_components gregorian_components_of(sxml_type_identifier primitive_type) {
    switch (primitive_type.index) {
    case SXML_TYPE_XS_DATE_TIME:
        return (gregorian_components){true, true, true, true};
    case SXML_TYPE_XS_DATE:
        return (gregorian_components){true, true, true, false};
    case SXML_TYPE_XS_TIME:
        return (gregorian_components){false, false, false, true};
    case SXML_TYPE_XS_G_YEAR_MONTH:
        return (gregorian_components){true, true, false, false};
    case SXML_TYPE_XS_G_YEAR:
        return (gregorian_components){true, false, false, false};
    case SXML_TYPE_XS_G_MONTH_DAY:
        return (gregorian_components){false, true, true, false};
    case SXML_TYPE_XS_G_DAY:
        return (gregorian_components){false, false, true, false};
    case SXML_TYPE_XS_G_MONTH:
        return (gregorian_components){false, true, false, false};
    default:
        return (gregorian_components){false, false, false, false};
    }
}

/* A date/time argument: has_value false for the empty sequence. */
typedef struct date_time_argument {
    bool has_value;
    sxml_type_identifier primitive_type;
    sxml_date_time value;
} date_time_argument;

static sxml_status read_date_time_argument(const sxml_function_call *call,
                                           size_t argument_index,
                                           date_time_argument *argument) {
    *argument = (date_time_argument){.has_value = false};
    sxml_optional_item item;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &item);
    if (status != SXML_STATUS_OK || !item.has_item) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(item.item, &datum);
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_DATE_TIME) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        *argument = (date_time_argument){
            .has_value = true,
            .primitive_type = sxml_atomic_item_get_primitive_type(item.item),
            .value = datum.datum_details.date_time};
    }
    return status;
}

/* ---------------------------------------------------------------- Shifts */

/* A dateTime (or a date at midnight) to move by minute_count minutes of
   local time. */
typedef struct date_time_shift {
    sxml_date_time date_time;
    int64_t minute_count;
} date_time_shift;

/* Adds minute_count minutes to the local value as op:add-dayTimeDuration-
   to-dateTime does (F&O 4.0, section 9.8.10), keeping the timezone
   component; FODT0001 beyond the supported years. */
static sxml_status shift_date_time(const sxml_function_call *call, date_time_shift shift,
                                   sxml_date_time *shifted) {
    if (shift.minute_count == 0) {
        *shifted = shift.date_time;
        return SXML_STATUS_OK;
    }
    sxml_item instant = {.representation = 0};
    sxml_status status = make_gregorian_item(
        call->item_heap,
        (gregorian_value){.type = SXML_TYPE_XS_DATE_TIME, .date_time = shift.date_time}, &instant);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item difference = {.representation = 0};
    status = make_day_time_duration(
        call->item_heap,
        (sxml_duration){.months = 0, .seconds = shift.minute_count * 60, .nanoseconds = 0},
        &difference);
    sxml_item sum = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_operator_make_temporal_value(
            call->item_heap,
            &(sxml_temporal_arithmetic){.arithmetic_operator = SXML_ARITHMETIC_OPERATOR_ADD,
                                        .left_item = instant,
                                        .right_item = difference,
                                        .implicit_timezone_offset_minutes = 0,
                                        .error_record = call->error_record},
            &sum);
        sxml_item_release(difference);
    }
    sxml_item_release(instant);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(sum, &datum);
    if (status == SXML_STATUS_OK) {
        *shifted = datum.datum_details.date_time;
    }
    sxml_item_release(sum);
    return status;
}

/* ------------------------------------------------------------- Timezones */

/* A timezone argument or default: has_timezone false for "no timezone". */
typedef struct target_timezone {
    bool has_timezone;
    int16_t offset_minutes;
} target_timezone;

/* Checks a timezone given as a duration (F&O 4.0, section 9.7: FODT0003
   unless a whole number of minutes from -PT14H to PT14H). */
static sxml_status check_timezone_duration(const sxml_function_call *call,
                                           sxml_duration duration,
                                           target_timezone *timezone) {
    if (duration.months != 0 || duration.nanoseconds != 0 || duration.seconds % 60 != 0 ||
        duration.seconds < (int64_t)-50400 || duration.seconds > (int64_t)50400) {
        return sxml_function_library_report(call, &invalid_timezone_error);
    }
    *timezone = (target_timezone){.has_timezone = true,
                                  .offset_minutes = (int16_t)(duration.seconds / 60)};
    return SXML_STATUS_OK;
}

static sxml_status read_implicit_timezone(const sxml_function_call *call,
                                          target_timezone *timezone) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    int16_t offset_minutes = 0;
    const sxml_status status =
        sxml_dynamic_context_read_implicit_timezone(call->dynamic_context, &offset_minutes);
    if (status == SXML_STATUS_OK) {
        *timezone = (target_timezone){.has_timezone = true, .offset_minutes = offset_minutes};
    }
    return status;
}

/* $timezone as xs:dayTimeDuration? := fn:implicit-timezone(). */
static sxml_status read_timezone_argument(const sxml_function_call *call,
                                          size_t argument_index, target_timezone *timezone) {
    if (call->arguments.sequence_count <= argument_index) {
        return read_implicit_timezone(call, timezone);
    }
    sxml_optional_item item;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!item.has_item) {
        *timezone = (target_timezone){.has_timezone = false, .offset_minutes = 0};
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(item.item, &datum);
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_DURATION) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return check_timezone_duration(call, datum.datum_details.duration, timezone);
}

/* Which function of section 9.7 runs. */
typedef enum adjustment_kind {
    SXML_ADJUSTMENT_KIND_DATE_TIME,
    SXML_ADJUSTMENT_KIND_DATE,
    SXML_ADJUSTMENT_KIND_TIME,
} adjustment_kind;

/* fn:adjust-dateTime-to-timezone and its siblings (F&O 4.0, sections
   9.7.1 to 9.7.3). With a timezone on both sides the local value moves by
   the difference of the offsets: a date through its starting instant
   (fn:dateTime($value, xs:time('00:00:00'))), a time within its day (its
   dateTime on 1972-12-31, of which only the time is kept). Otherwise the
   timezone is only set or removed. */
static sxml_status call_adjust(const sxml_function_call *call, adjustment_kind kind,
                               sxml_sequence *result) {
    date_time_argument value;
    sxml_status status = read_date_time_argument(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_value) {
        *result = sxml_sequence_empty();
        return status;
    }
    target_timezone timezone = {.has_timezone = false};
    status = read_timezone_argument(call, 1, &timezone);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_date_time adjusted = value.value;
    if (adjusted.has_timezone && timezone.has_timezone) {
        const int64_t minute_count =
            (int64_t)timezone.offset_minutes - adjusted.timezone_offset_minutes;
        if (kind == SXML_ADJUSTMENT_KIND_TIME) {
            const int64_t local_minutes =
                (int64_t)adjusted.hour * 60 + adjusted.minute + minute_count;
            const int64_t day_minutes = ((local_minutes % 1440) + 1440) % 1440;
            adjusted.hour = (uint8_t)(day_minutes / 60);
            adjusted.minute = (uint8_t)(day_minutes % 60);
        } else {
            status = shift_date_time(
                call, (date_time_shift){.date_time = adjusted, .minute_count = minute_count},
                &adjusted);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (kind == SXML_ADJUSTMENT_KIND_DATE) {
                adjusted.hour = 0;
                adjusted.minute = 0;
            }
        }
    }
    adjusted.has_timezone = timezone.has_timezone;
    adjusted.timezone_offset_minutes = timezone.offset_minutes;
    sxml_builtin_type type = SXML_TYPE_XS_DATE_TIME;
    if (kind == SXML_ADJUSTMENT_KIND_DATE) {
        type = SXML_TYPE_XS_DATE;
    } else if (kind == SXML_ADJUSTMENT_KIND_TIME) {
        type = SXML_TYPE_XS_TIME;
    }
    sxml_item item = {.representation = 0};
    status = make_gregorian_item(call->item_heap,
                                 (gregorian_value){.type = type, .date_time = adjusted}, &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

static sxml_status call_adjust_date_time(const sxml_function_call *call,
                                         sxml_sequence *result) {
    return call_adjust(call, SXML_ADJUSTMENT_KIND_DATE_TIME, result);
}

static sxml_status call_adjust_date(const sxml_function_call *call, sxml_sequence *result) {
    return call_adjust(call, SXML_ADJUSTMENT_KIND_DATE, result);
}

static sxml_status call_adjust_time(const sxml_function_call *call, sxml_sequence *result) {
    return call_adjust(call, SXML_ADJUSTMENT_KIND_TIME, result);
}

/* ------------------------------------------------------- fn:days-in-month */

/* fn:days-in-month($value as (xs:dateTime | xs:date | xs:gYearMonth)?)
   (F&O 4.0, section 9.6.11): 28 to 31 days, from the year and month of
   the local value. */
static sxml_status call_days_in_month(const sxml_function_call *call, sxml_sequence *result) {
    date_time_argument value;
    const sxml_status status = read_date_time_argument(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_value) {
        *result = sxml_sequence_empty();
        return status;
    }
    if (value.value.month < 1 || value.value.month > 12) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *result = sxml_sequence_from_item(
        sxml_atomic_item_from_integer(sxml_days_in_month(value.value.year, value.value.month)));
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- fn:parts-of-dateTime */

/* The fields of fn:dateTime-record, in record order (F&O 4.0, section
   9.3). */
typedef enum record_field {
    SXML_RECORD_FIELD_YEAR,
    SXML_RECORD_FIELD_MONTH,
    SXML_RECORD_FIELD_DAY,
    SXML_RECORD_FIELD_HOURS,
    SXML_RECORD_FIELD_MINUTES,
    SXML_RECORD_FIELD_SECONDS,
    SXML_RECORD_FIELD_TIMEZONE,
} record_field;

#define SXML_RECORD_FIELD_COUNT ((size_t)7)

static const char *const record_field_names[SXML_RECORD_FIELD_COUNT] = {
    "year", "month", "day", "hours", "minutes", "seconds", "timezone"};

/* Writes whole seconds and nanoseconds as xs:decimal text: digits, and a
   fraction without trailing zeros when there is one (F&O 4.0, section
   23.1.2.1, canonical xs:decimal). text holds 32 bytes. */
typedef struct seconds_text {
    bool is_negative;
    uint64_t whole_seconds;
    uint32_t nanoseconds;
} seconds_text;

static size_t write_seconds_text(seconds_text seconds, uint8_t text[32]) {
    size_t byte_count = 0;
    if (seconds.is_negative && (seconds.whole_seconds > 0 || seconds.nanoseconds > 0)) {
        text[byte_count++] = '-';
    }
    uint8_t digits[20];
    size_t digit_count = 0;
    uint64_t whole = seconds.whole_seconds;
    do {
        digits[digit_count++] = (uint8_t)('0' + whole % 10U);
        whole /= 10U;
    } while (whole > 0);
    while (digit_count > 0) {
        digit_count -= 1;
        text[byte_count++] = digits[digit_count];
    }
    uint32_t fraction = seconds.nanoseconds;
    if (fraction > 0) {
        text[byte_count++] = '.';
    }
    for (uint32_t divisor = 100000000U; divisor > 0 && fraction > 0; divisor /= 10U) {
        text[byte_count++] = (uint8_t)('0' + fraction / divisor);
        fraction %= divisor;
    }
    return byte_count;
}

static sxml_status make_decimal_seconds(sxml_item_heap *item_heap, seconds_text seconds,
                                        sxml_item *item) {
    uint8_t text[32];
    const size_t byte_count = write_seconds_text(seconds, text);
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL),
            .datum = {.datum_kind = SXML_DATUM_KIND_DECIMAL,
                      .datum_details = {.decimal_text = {.bytes = text,
                                                         .byte_count = byte_count}}}},
        item);
}

/* The value of one field of the record of value: the empty sequence for
   an absent component. */
typedef struct field_request {
    const date_time_argument *value;
    record_field field;
} field_request;

static sxml_status make_field_value(const sxml_function_call *call, field_request request,
                                    sxml_sequence *field_value) {
    const gregorian_components components =
        gregorian_components_of(request.value->primitive_type);
    const sxml_date_time date_time = request.value->value;
    *field_value = sxml_sequence_empty();
    sxml_item item = {.representation = 0};
    sxml_status status = SXML_STATUS_OK;
    switch (request.field) {
    case SXML_RECORD_FIELD_YEAR:
        if (components.has_year) {
            *field_value = sxml_sequence_from_item(sxml_atomic_item_from_integer(date_time.year));
        }
        return SXML_STATUS_OK;
    case SXML_RECORD_FIELD_MONTH:
        if (components.has_month) {
            *field_value =
                sxml_sequence_from_item(sxml_atomic_item_from_integer(date_time.month));
        }
        return SXML_STATUS_OK;
    case SXML_RECORD_FIELD_DAY:
        if (components.has_day) {
            *field_value = sxml_sequence_from_item(sxml_atomic_item_from_integer(date_time.day));
        }
        return SXML_STATUS_OK;
    case SXML_RECORD_FIELD_HOURS:
        if (components.has_time) {
            *field_value = sxml_sequence_from_item(sxml_atomic_item_from_integer(date_time.hour));
        }
        return SXML_STATUS_OK;
    case SXML_RECORD_FIELD_MINUTES:
        if (components.has_time) {
            *field_value =
                sxml_sequence_from_item(sxml_atomic_item_from_integer(date_time.minute));
        }
        return SXML_STATUS_OK;
    case SXML_RECORD_FIELD_SECONDS:
        if (!components.has_time) {
            return SXML_STATUS_OK;
        }
        status = make_decimal_seconds(call->item_heap,
                                      (seconds_text){.is_negative = false,
                                                     .whole_seconds = date_time.second,
                                                     .nanoseconds = date_time.nanosecond},
                                      &item);
        break;
    case SXML_RECORD_FIELD_TIMEZONE:
        if (!date_time.has_timezone) {
            return SXML_STATUS_OK;
        }
        status = make_day_time_duration(
            call->item_heap,
            (sxml_duration){.months = 0,
                            .seconds = (int64_t)date_time.timezone_offset_minutes * 60,
                            .nanoseconds = 0},
            &item);
        break;
    }
    if (status == SXML_STATUS_OK) {
        *field_value = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:parts-of-dateTime($value) as fn:dateTime-record? (F&O 4.0, section
   9.6.17): every field, the empty sequence for an absent component, in a
   record annotated with fn:dateTime-record (D133). */
static sxml_status call_parts_of_date_time(const sxml_function_call *call,
                                           sxml_sequence *result) {
    date_time_argument value;
    sxml_status status = read_date_time_argument(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_value) {
        *result = sxml_sequence_empty();
        return status;
    }
    sxml_sequence field_values[SXML_RECORD_FIELD_COUNT];
    size_t field_count = 0;
    while (status == SXML_STATUS_OK && field_count < SXML_RECORD_FIELD_COUNT) {
        status = make_field_value(
            call, (field_request){.value = &value, .field = (record_field)field_count},
            &field_values[field_count]);
        if (status == SXML_STATUS_OK) {
            field_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_record(
            call,
            &(sxml_builtin_record_request){
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("dateTime-record"),
                .field_values = {.sequences = field_values, .sequence_count = field_count}},
            result);
    }
    for (size_t field_index = 0; field_index < field_count; field_index += 1) {
        sxml_sequence_release(field_values[field_index]);
    }
    return status;
}

/* ------------------------------------------------------- Decimal numbers */

/* An xs:decimal split into whole and fraction: whole beyond uint64_t sets
   is_too_large; fraction digits past nine are truncated (D36). */
typedef struct decimal_parts {
    bool is_negative;
    bool is_too_large;
    bool has_fraction;
    uint64_t whole;
    uint32_t nanoseconds;
} decimal_parts;

/* Splits the canonical text of an xs:decimal: "-"?, digits, ("." digits)?. */
static decimal_parts split_decimal_text(sxml_byte_view text) {
    decimal_parts parts = {.is_negative = false};
    size_t position = 0;
    if (position < text.byte_count && text.bytes[position] == '-') {
        parts.is_negative = true;
        position += 1;
    }
    while (position < text.byte_count && text.bytes[position] >= '0' &&
           text.bytes[position] <= '9') {
        const uint64_t digit = (uint64_t)(text.bytes[position] - '0');
        if (parts.whole > (UINT64_MAX - digit) / 10U) {
            parts.is_too_large = true;
        } else {
            parts.whole = parts.whole * 10U + digit;
        }
        position += 1;
    }
    if (position < text.byte_count && text.bytes[position] == '.') {
        position += 1;
        uint32_t scale = 100000000U;
        while (position < text.byte_count && text.bytes[position] >= '0' &&
               text.bytes[position] <= '9') {
            const uint32_t digit = (uint32_t)(text.bytes[position] - '0');
            if (digit != 0) {
                parts.has_fraction = true;
            }
            parts.nanoseconds += digit * scale;
            scale /= 10U;
            position += 1;
        }
    }
    return parts;
}

/* The parts of an xs:decimal (or xs:integer) item. */
static sxml_status read_decimal_parts(sxml_item item, decimal_parts *parts) {
    sxml_atomic_datum datum;
    const sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        const int64_t integer = datum.datum_details.integer;
        *parts = (decimal_parts){
            .is_negative = integer < 0,
            .whole = integer < 0 ? (uint64_t)0 - (uint64_t)integer : (uint64_t)integer};
        return SXML_STATUS_OK;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_DECIMAL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *parts = split_decimal_text(datum.datum_details.decimal_text);
    return SXML_STATUS_OK;
}

/* --------------------------------------------------- fn:build-dateTime */

/* The fields of a record as read: which are non-empty, and their values. */
typedef struct record_fields {
    bool is_present[SXML_RECORD_FIELD_COUNT];
    /* year, month, day, hours and minutes; is_beyond set for an integer
       outside int64_t. */
    int64_t integers[5];
    bool is_beyond[5];
    decimal_parts seconds;
    sxml_duration timezone;
} record_fields;

static bool item_has_type(const sxml_function_call *call, sxml_item item,
                          sxml_builtin_type type) {
    return sxml_operator_item_has_type(call->item_heap,
                                       (sxml_type_test){.item = item, .type = type});
}

/* A field value to coerce to its field type, and the type it casts to
   from xs:untypedAtomic. */
typedef struct field_coercion {
    sxml_item item;
    sxml_builtin_type target_type;
} field_coercion;

/* Casts an xs:untypedAtomic field value (coercion rules: cast to the
   required type); *cast_item is owned, or a zero item when no cast is
   needed. */
static sxml_status cast_if_untyped(const sxml_function_call *call, field_coercion coercion,
                                   sxml_item *cast_item) {
    *cast_item = (sxml_item){.representation = 0};
    if (!item_has_type(call, coercion.item, SXML_TYPE_XS_UNTYPED_ATOMIC)) {
        return SXML_STATUS_OK;
    }
    return sxml_operator_cast_item(call->item_heap,
                                   &(sxml_operator_cast){.item = coercion.item,
                                                         .target_type = coercion.target_type,
                                                         .error_record = call->error_record},
                                   cast_item);
}

/* Where an integer field goes. */
typedef struct integer_field {
    size_t integer_index;
    sxml_item item;
} integer_field;

/* Coerces to xs:integer: an xs:integer, an integral xs:decimal
   (relabeled) or xs:untypedAtomic (cast); other types, a fractional
   decimal and every xs:double are XPTY0004 (XPath 4.0, section 3.4.3). */
static sxml_status read_integer_field(const sxml_function_call *call, integer_field field,
                                      record_fields *fields) {
    sxml_item cast_item = {.representation = 0};
    sxml_status status = cast_if_untyped(
        call, (field_coercion){.item = field.item, .target_type = SXML_TYPE_XS_INTEGER},
        &cast_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_item item = cast_item.representation != 0 ? cast_item : field.item;
    decimal_parts parts = {.is_negative = false};
    if (!item_has_type(call, item, SXML_TYPE_XS_DECIMAL)) {
        status = sxml_function_library_report(call, &not_a_record_error);
    } else {
        status = read_decimal_parts(item, &parts);
    }
    sxml_item_release(cast_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (parts.has_fraction) {
        return sxml_function_library_report(call, &not_a_record_error);
    }
    const uint64_t limit = parts.is_negative ? (uint64_t)INT64_MAX + 1U : (uint64_t)INT64_MAX;
    if (parts.is_too_large || parts.whole > limit) {
        fields->is_beyond[field.integer_index] = true;
        return SXML_STATUS_OK;
    }
    fields->integers[field.integer_index] =
        parts.is_negative ? (int64_t)(0U - parts.whole) : (int64_t)parts.whole;
    return SXML_STATUS_OK;
}

/* Coerces to xs:decimal: a decimal, or a number or xs:untypedAtomic cast
   to it (XPath 4.0, section 3.4.3: xs:double and xs:float cast to
   xs:decimal). */
static sxml_status read_seconds_field(const sxml_function_call *call, sxml_item item,
                                      record_fields *fields) {
    sxml_item cast_item = {.representation = 0};
    sxml_status status = SXML_STATUS_OK;
    if (item_has_type(call, item, SXML_TYPE_XS_DOUBLE) ||
        item_has_type(call, item, SXML_TYPE_XS_FLOAT) ||
        item_has_type(call, item, SXML_TYPE_XS_UNTYPED_ATOMIC)) {
        status = sxml_operator_cast_item(call->item_heap,
                                         &(sxml_operator_cast){.item = item,
                                                               .target_type = SXML_TYPE_XS_DECIMAL,
                                                               .error_record = call->error_record},
                                         &cast_item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    } else if (!item_has_type(call, item, SXML_TYPE_XS_DECIMAL)) {
        return sxml_function_library_report(call, &not_a_record_error);
    }
    status = read_decimal_parts(cast_item.representation != 0 ? cast_item : item,
                                &fields->seconds);
    sxml_item_release(cast_item);
    return status;
}

/* Coerces to xs:dayTimeDuration: a duration without months (relabeled) or
   xs:untypedAtomic (cast). */
static sxml_status read_timezone_field(const sxml_function_call *call, sxml_item item,
                                       record_fields *fields) {
    sxml_item cast_item = {.representation = 0};
    sxml_status status = cast_if_untyped(
        call, (field_coercion){.item = item, .target_type = SXML_TYPE_XS_DAY_TIME_DURATION},
        &cast_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_item duration_item = cast_item.representation != 0 ? cast_item : item;
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    if (item_has_type(call, duration_item, SXML_TYPE_XS_DURATION)) {
        status = sxml_atomic_item_get_datum(duration_item, &datum);
    }
    sxml_item_release(cast_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_DURATION || datum.datum_details.duration.months != 0) {
        return sxml_function_library_report(call, &not_a_record_error);
    }
    fields->timezone = datum.datum_details.duration;
    return SXML_STATUS_OK;
}

/* Reads one entry of the record: its key must name a field, its value be
   empty or one atomic item. */
static sxml_status read_record_entry(const sxml_function_call *call, sxml_map_entry entry,
                                     record_fields *fields) {
    sxml_atomic_datum key;
    sxml_status status = SXML_STATUS_INVALID_ARGUMENT;
    if (sxml_item_is_atomic(entry.key)) {
        status = sxml_atomic_item_get_datum(entry.key, &key);
    }
    if (status != SXML_STATUS_OK || key.datum_kind != SXML_DATUM_KIND_STRING) {
        return sxml_function_library_report(call, &not_a_record_error);
    }
    size_t field_index = 0;
    while (field_index < SXML_RECORD_FIELD_COUNT &&
           !sxml_byte_view_equals(key.datum_details.string.utf8,
                                  sxml_byte_view_from_c_string(record_field_names[field_index]))) {
        field_index += 1;
    }
    if (field_index == SXML_RECORD_FIELD_COUNT) {
        return sxml_function_library_report(call, &not_a_record_error);
    }
    sxml_single_operand single;
    status = sxml_operator_read_single_operand(entry.value, call->error_record, &single);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        return sxml_function_library_report(call, &not_a_record_error);
    }
    if (status != SXML_STATUS_OK || !single.has_item) {
        return status;
    }
    fields->is_present[field_index] = true;
    switch ((record_field)field_index) {
    case SXML_RECORD_FIELD_YEAR:
    case SXML_RECORD_FIELD_MONTH:
    case SXML_RECORD_FIELD_DAY:
    case SXML_RECORD_FIELD_HOURS:
    case SXML_RECORD_FIELD_MINUTES:
        return read_integer_field(
            call, (integer_field){.integer_index = field_index, .item = single.item}, fields);
    case SXML_RECORD_FIELD_SECONDS:
        return read_seconds_field(call, single.item, fields);
    case SXML_RECORD_FIELD_TIMEZONE:
        return read_timezone_field(call, single.item, fields);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The Gregorian type a set of non-empty fields gives (F&O 4.0,
   fn:build-dateTime, FODT0005); false for any other set. */
static bool type_of_fields(const record_fields *fields, sxml_builtin_type *type) {
    static const struct {
        bool has_year;
        bool has_month;
        bool has_day;
        bool has_time;
        sxml_builtin_type type;
    } shapes[] = {
        {true, true, true, true, SXML_TYPE_XS_DATE_TIME},
        {true, true, true, false, SXML_TYPE_XS_DATE},
        {true, false, false, false, SXML_TYPE_XS_G_YEAR},
        {true, true, false, false, SXML_TYPE_XS_G_YEAR_MONTH},
        {false, true, false, false, SXML_TYPE_XS_G_MONTH},
        {false, true, true, false, SXML_TYPE_XS_G_MONTH_DAY},
        {false, false, true, false, SXML_TYPE_XS_G_DAY},
        {false, false, false, true, SXML_TYPE_XS_TIME},
    };
    const bool *present = fields->is_present;
    const bool has_any_time = present[SXML_RECORD_FIELD_HOURS] ||
                              present[SXML_RECORD_FIELD_MINUTES] ||
                              present[SXML_RECORD_FIELD_SECONDS];
    const bool has_time = present[SXML_RECORD_FIELD_HOURS] &&
                          present[SXML_RECORD_FIELD_MINUTES] &&
                          present[SXML_RECORD_FIELD_SECONDS];
    if (has_any_time && !has_time) {
        return false;
    }
    for (size_t shape_index = 0; shape_index < sizeof(shapes) / sizeof(shapes[0]);
         shape_index += 1) {
        if (shapes[shape_index].has_year == present[SXML_RECORD_FIELD_YEAR] &&
            shapes[shape_index].has_month == present[SXML_RECORD_FIELD_MONTH] &&
            shapes[shape_index].has_day == present[SXML_RECORD_FIELD_DAY] &&
            shapes[shape_index].has_time == has_time) {
            *type = shapes[shape_index].type;
            return true;
        }
    }
    return false;
}

/* Checks the ranges of the components (FORG0001; FODT0003 for the
   timezone, FODT0001 for a year beyond int64_t) and writes the datum. */
static sxml_status make_record_datum(const sxml_function_call *call,
                                     const record_fields *fields, sxml_date_time *date_time) {
    const bool *present = fields->is_present;
    *date_time = (sxml_date_time){.year = 0};
    if (present[SXML_RECORD_FIELD_YEAR]) {
        if (fields->is_beyond[SXML_RECORD_FIELD_YEAR]) {
            return sxml_function_library_report(call, &date_overflow_error);
        }
        date_time->year = fields->integers[SXML_RECORD_FIELD_YEAR];
    }
    for (size_t field_index = SXML_RECORD_FIELD_MONTH; field_index <= SXML_RECORD_FIELD_MINUTES;
         field_index += 1) {
        static const int64_t maximums[5] = {0, 12, 31, 23, 59};
        static const int64_t minimums[5] = {0, 1, 1, 0, 0};
        if (present[field_index] &&
            (fields->is_beyond[field_index] ||
             fields->integers[field_index] < minimums[field_index] ||
             fields->integers[field_index] > maximums[field_index])) {
            return sxml_function_library_report(call, &component_out_of_range_error);
        }
    }
    date_time->month = (uint8_t)fields->integers[SXML_RECORD_FIELD_MONTH];
    date_time->day = (uint8_t)fields->integers[SXML_RECORD_FIELD_DAY];
    date_time->hour = (uint8_t)fields->integers[SXML_RECORD_FIELD_HOURS];
    date_time->minute = (uint8_t)fields->integers[SXML_RECORD_FIELD_MINUTES];
    if (present[SXML_RECORD_FIELD_DAY]) {
        /* A gMonthDay may be --02-29 (the reference year 1972 is a leap
           year, F&O 4.0, section 9.5); a gDay may be ---31. */
        const int64_t year = present[SXML_RECORD_FIELD_YEAR] ? date_time->year : 1972;
        const uint8_t month = present[SXML_RECORD_FIELD_MONTH] ? date_time->month : 1;
        if (date_time->day > sxml_days_in_month(year, month)) {
            return sxml_function_library_report(call, &component_out_of_range_error);
        }
    }
    if (present[SXML_RECORD_FIELD_SECONDS]) {
        const decimal_parts seconds = fields->seconds;
        const bool is_zero = seconds.whole == 0 && seconds.nanoseconds == 0;
        if ((seconds.is_negative && !is_zero) || seconds.is_too_large || seconds.whole >= 60) {
            return sxml_function_library_report(call, &component_out_of_range_error);
        }
        date_time->second = (uint8_t)seconds.whole;
        date_time->nanosecond = seconds.nanoseconds;
    }
    if (present[SXML_RECORD_FIELD_TIMEZONE]) {
        target_timezone timezone = {.has_timezone = false};
        const sxml_status status = check_timezone_duration(call, fields->timezone, &timezone);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        date_time->has_timezone = true;
        date_time->timezone_offset_minutes = timezone.offset_minutes;
    }
    return SXML_STATUS_OK;
}

/* fn:build-dateTime($value as fn:dateTime-record?) (F&O 4.0, section
   9.4.2): the Gregorian value whose parts-of-dateTime is $value; with all
   seven components an xs:dateTimeStamp. */
static sxml_status call_build_date_time(const sxml_function_call *call,
                                        sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    size_t entry_count = 0;
    status = sxml_map_get_size(value.item, &entry_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    record_fields fields;
    memset(&fields, 0, sizeof(fields));
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(value.item, entry_index, &entry);
        if (status == SXML_STATUS_OK) {
            status = read_record_entry(call, entry, &fields);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_builtin_type type = SXML_TYPE_XS_DATE_TIME;
    if (!type_of_fields(&fields, &type)) {
        return sxml_function_library_report(call, &invalid_component_set_error);
    }
    sxml_date_time date_time;
    status = make_record_datum(call, &fields, &date_time);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (type == SXML_TYPE_XS_DATE_TIME && date_time.has_timezone) {
        type = SXML_TYPE_XS_DATE_TIME_STAMP;
    }
    sxml_item item = {.representation = 0};
    status = make_gregorian_item(call->item_heap,
                                 (gregorian_value){.type = type, .date_time = date_time}, &item);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        return sxml_function_library_report(call, &component_out_of_range_error);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ----------------------------------------------- Seconds and Unix time */

/* fn:seconds($value as xs:decimal?) as xs:dayTimeDuration? (F&O 4.0,
   section 8.4.1): a duration of $value seconds, made from the decimal's
   digits so that fraction digits past nine are truncated as by casting
   (D36); FODT0002 beyond int64_t seconds (section 8.1.2). */
static sxml_status call_seconds(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    decimal_parts parts;
    status = read_decimal_parts(value.item, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (parts.is_too_large || parts.whole > (uint64_t)INT64_MAX) {
        return sxml_function_library_report(call, &duration_overflow_error);
    }
    const int64_t sign = parts.is_negative ? -1 : 1;
    sxml_item item = {.representation = 0};
    status = make_day_time_duration(
        call->item_heap,
        (sxml_duration){.months = 0,
                        .seconds = sign * (int64_t)parts.whole,
                        .nanoseconds = (int32_t)(sign * (int64_t)parts.nanoseconds)},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:duration-to-seconds($value as xs:dayTimeDuration?) as xs:decimal?
   (F&O 4.0, section 8.4.2): $value div xs:dayTimeDuration('PT1S'),
   exactly. */
static sxml_status call_duration_to_seconds(const sxml_function_call *call,
                                            sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(value.item, &datum);
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_DURATION) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_duration duration = datum.datum_details.duration;
    const bool is_negative = duration.seconds < 0 || duration.nanoseconds < 0;
    const uint64_t whole = duration.seconds < 0 ? (uint64_t)0 - (uint64_t)duration.seconds
                                                : (uint64_t)duration.seconds;
    const uint32_t nanoseconds = (uint32_t)(duration.nanoseconds < 0
                                                ? -(int64_t)duration.nanoseconds
                                                : (int64_t)duration.nanoseconds);
    sxml_item item = {.representation = 0};
    status = make_decimal_seconds(call->item_heap,
                                  (seconds_text){.is_negative = is_negative,
                                                 .whole_seconds = whole,
                                                 .nanoseconds = nanoseconds},
                                  &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:unix-dateTime($value as xs:nonNegativeInteger? := 0) as
   xs:dateTimeStamp (F&O 4.0, section 9.4.3): xs:dateTime('1970-01-01T00:
   00:00Z') + ($value otherwise 0) * seconds(0.001); FODT0001 beyond the
   supported years. */
static sxml_status call_unix_date_time(const sxml_function_call *call,
                                       sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    decimal_parts milliseconds = {.whole = 0};
    if (value.has_item) {
        status = read_decimal_parts(value.item, &milliseconds);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (milliseconds.is_too_large) {
        return sxml_function_library_report(call, &date_overflow_error);
    }
    const int64_t whole_seconds = (int64_t)(milliseconds.whole / 1000U);
    const int32_t nanoseconds = (int32_t)(milliseconds.whole % 1000U) * 1000000;
    sxml_item epoch = {.representation = 0};
    status = make_gregorian_item(
        call->item_heap,
        (gregorian_value){.type = SXML_TYPE_XS_DATE_TIME,
                          .date_time = {.year = 1970, .month = 1, .day = 1, .has_timezone = true}},
        &epoch);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item length = {.representation = 0};
    status = make_day_time_duration(
        call->item_heap,
        (sxml_duration){.months = 0, .seconds = whole_seconds, .nanoseconds = nanoseconds},
        &length);
    sxml_item sum = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_operator_make_temporal_value(
            call->item_heap,
            &(sxml_temporal_arithmetic){.arithmetic_operator = SXML_ARITHMETIC_OPERATOR_ADD,
                                        .left_item = epoch,
                                        .right_item = length,
                                        .implicit_timezone_offset_minutes = 0,
                                        .error_record = call->error_record},
            &sum);
        sxml_item_release(length);
    }
    sxml_item_release(epoch);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(sum, &datum);
    sxml_item stamp = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = make_gregorian_item(call->item_heap,
                                     (gregorian_value){.type = SXML_TYPE_XS_DATE_TIME_STAMP,
                                                       .date_time = datum.datum_details.date_time},
                                     &stamp);
    }
    sxml_item_release(sum);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(stamp);
    }
    return status;
}

/* ------------------------------------------------------ fn:civil-timezone */

/* fn:civil-timezone($value as xs:dateTime, $place as xs:string? := ())
   as xs:dayTimeDuration (F&O 4.0, section 9.7.4), without a timezone
   database (D97): see the comment at the top of the file. */
static sxml_status call_civil_timezone(const sxml_function_call *call,
                                       sxml_sequence *result) {
    static const char *const universal_places[] = {"UTC", "Etc/UTC", "GMT", "Etc/GMT"};
    sxml_optional_item place;
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &place);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    target_timezone timezone = {.has_timezone = true, .offset_minutes = 0};
    if (!place.has_item) {
        status = read_implicit_timezone(call, &timezone);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    } else {
        sxml_string name;
        status = sxml_function_library_read_string_argument(call, 1, &name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        bool is_universal = false;
        for (size_t place_index = 0;
             place_index < sizeof(universal_places) / sizeof(universal_places[0]);
             place_index += 1) {
            is_universal = is_universal ||
                           sxml_byte_view_equals(name.utf8, sxml_byte_view_from_c_string(
                                                                universal_places[place_index]));
        }
        if (!is_universal) {
            return sxml_function_library_report(call, &no_civil_timezone_error);
        }
    }
    sxml_item item = {.representation = 0};
    status = make_day_time_duration(
        call->item_heap,
        (sxml_duration){.months = 0,
                        .seconds = (int64_t)timezone.offset_minutes * 60,
                        .nanoseconds = 0},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------------ fn:parse-ietf-date */

/* The text of fn:parse-ietf-date and how far it is read. */
typedef struct ietf_scanner {
    sxml_byte_view text;
    size_t position;
} ietf_scanner;

/* What the grammar of F&O 4.0, section 9.10.1, gives. */
typedef struct ietf_date {
    int64_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint32_t nanosecond;
    int16_t timezone_offset_minutes;
} ietf_date;

static int ietf_byte_at(const ietf_scanner *scanner, size_t offset) {
    const size_t position = scanner->position + offset;
    return position < scanner->text.byte_count ? (int)scanner->text.bytes[position] : -1;
}

static bool ietf_is_digit(int byte) {
    return byte >= '0' && byte <= '9';
}

/* S ::= (x09 | x0A | x0D | x20)+ ; returns whether any was skipped. */
static bool ietf_skip_spaces(ietf_scanner *scanner) {
    const size_t start = scanner->position;
    int next_byte = ietf_byte_at(scanner, 0);
    while (next_byte == 0x09 || next_byte == 0x0A || next_byte == 0x0D || next_byte == 0x20) {
        scanner->position += 1;
        next_byte = ietf_byte_at(scanner, 0);
    }
    return scanner->position > start;
}

/* Matches word without regard to ASCII case and advances past it. */
static bool ietf_match_word(ietf_scanner *scanner, sxml_byte_view word) {
    const size_t byte_count = word.byte_count;
    if (scanner->text.byte_count - scanner->position < byte_count) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < byte_count; byte_index += 1) {
        int byte = scanner->text.bytes[scanner->position + byte_index];
        if (byte >= 'A' && byte <= 'Z') {
            byte += 'a' - 'A';
        }
        if (byte != (int)word.bytes[byte_index]) {
            return false;
        }
    }
    scanner->position += byte_count;
    return true;
}

/* How many digits to read at most, and at least. */
typedef struct digit_run {
    size_t minimum_count;
    size_t maximum_count;
} digit_run;

/* Reads minimum_count to maximum_count digits; *digit_count says how many. */
typedef struct digit_value {
    uint32_t value;
    size_t digit_count;
} digit_value;

static bool ietf_read_digits(ietf_scanner *scanner, digit_run run, digit_value *digits) {
    *digits = (digit_value){.value = 0, .digit_count = 0};
    while (digits->digit_count < run.maximum_count && ietf_is_digit(ietf_byte_at(scanner, 0))) {
        digits->value = digits->value * 10U + (uint32_t)(ietf_byte_at(scanner, 0) - '0');
        digits->digit_count += 1;
        scanner->position += 1;
    }
    return digits->digit_count >= run.minimum_count;
}

/* dayname ::= "Mon" | ... | "Sunday" (the full names first, so that the
   longest one matches). */
static bool ietf_match_dayname(ietf_scanner *scanner) {
    static const char *const daynames[] = {
        "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
        "mon",    "tue",     "wed",       "thu",      "fri",    "sat",      "sun"};
    for (size_t name_index = 0; name_index < sizeof(daynames) / sizeof(daynames[0]);
         name_index += 1) {
        if (ietf_match_word(scanner, sxml_byte_view_from_c_string(daynames[name_index]))) {
            return true;
        }
    }
    return false;
}

/* monthname ::= "Jan" | ... | "Dec". */
static bool ietf_read_monthname(ietf_scanner *scanner, ietf_date *date) {
    static const char *const monthnames[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                             "jul", "aug", "sep", "oct", "nov", "dec"};
    for (size_t month_index = 0; month_index < 12; month_index += 1) {
        if (ietf_match_word(scanner, sxml_byte_view_from_c_string(monthnames[month_index]))) {
            date->month = (uint8_t)(month_index + 1);
            return true;
        }
    }
    return false;
}

/* dsep ::= S | (S? "-" S?). */
static bool ietf_read_date_separator(ietf_scanner *scanner) {
    const bool has_space = ietf_skip_spaces(scanner);
    if (ietf_byte_at(scanner, 0) == '-') {
        scanner->position += 1;
        (void)ietf_skip_spaces(scanner);
        return true;
    }
    return has_space;
}

/* daynum ::= digit digit? */
static bool ietf_read_daynum(ietf_scanner *scanner, ietf_date *date) {
    digit_value digits;
    if (!ietf_read_digits(scanner, (digit_run){.minimum_count = 1, .maximum_count = 2},
                          &digits)) {
        return false;
    }
    date->day = (uint8_t)digits.value;
    return true;
}

/* year ::= digit digit (digit digit)? ; two digits have 1900 added. */
static bool ietf_read_year(ietf_scanner *scanner, ietf_date *date) {
    digit_value digits;
    if (!ietf_read_digits(scanner, (digit_run){.minimum_count = 2, .maximum_count = 4},
                          &digits) ||
        digits.digit_count == 3) {
        return false;
    }
    date->year = digits.digit_count == 2 ? 1900 + (int64_t)digits.value : (int64_t)digits.value;
    return true;
}

/* tzname ::= "UT" | "UTC" | "GMT" | "EST" | ... ; *offset_minutes is its
   offset ("UTC" before "UT", so that the longest name matches). */
static bool ietf_read_tzname(ietf_scanner *scanner, int16_t *offset_minutes) {
    static const struct {
        const char *name;
        int16_t offset_minutes;
    } tznames[] = {{"utc", 0},    {"ut", 0},     {"gmt", 0},    {"est", -300},
                   {"edt", -240}, {"cst", -360}, {"cdt", -300}, {"mst", -420},
                   {"mdt", -360}, {"pst", -480}, {"pdt", -420}};
    for (size_t name_index = 0; name_index < sizeof(tznames) / sizeof(tznames[0]);
         name_index += 1) {
        if (ietf_match_word(scanner, sxml_byte_view_from_c_string(tznames[name_index].name))) {
            *offset_minutes = tznames[name_index].offset_minutes;
            return true;
        }
    }
    return false;
}

/* tzoffset ::= ("+"|"-") hours ":"? minutes? : with a colon, hours and
   minutes; without, one to four digits as H, HH, HMM or HHMM. Then
   (S? "(" S? tzname S? ")")?, whose name is ignored. */
static bool ietf_read_tzoffset(ietf_scanner *scanner, ietf_date *date) {
    const int sign = ietf_byte_at(scanner, 0) == '-' ? -1 : 1;
    scanner->position += 1;
    digit_value digits;
    if (!ietf_read_digits(scanner, (digit_run){.minimum_count = 1, .maximum_count = 4},
                          &digits)) {
        return false;
    }
    uint32_t hours = digits.value;
    uint32_t minutes = 0;
    if (digits.digit_count >= 3) {
        hours = digits.value / 100U;
        minutes = digits.value % 100U;
    } else if (ietf_byte_at(scanner, 0) == ':') {
        scanner->position += 1;
        if (ietf_is_digit(ietf_byte_at(scanner, 0))) {
            if (!ietf_read_digits(scanner, (digit_run){.minimum_count = 2, .maximum_count = 2},
                                  &digits)) {
                return false;
            }
            minutes = digits.value;
        }
    }
    if (minutes > 59 || hours * 60U + minutes > 840U) {
        return false;
    }
    date->timezone_offset_minutes = (int16_t)(sign * (int)(hours * 60U + minutes));
    const size_t before_comment = scanner->position;
    (void)ietf_skip_spaces(scanner);
    if (ietf_byte_at(scanner, 0) != '(') {
        scanner->position = before_comment;
        return true;
    }
    scanner->position += 1;
    (void)ietf_skip_spaces(scanner);
    int16_t ignored_offset = 0;
    if (!ietf_read_tzname(scanner, &ignored_offset)) {
        return false;
    }
    (void)ietf_skip_spaces(scanner);
    if (ietf_byte_at(scanner, 0) != ')') {
        return false;
    }
    scanner->position += 1;
    return true;
}

/* time ::= hours ":" minutes (":" seconds)? (S? timezone)? ; a timezone
   that is not there leaves the spaces before it unread. */
static bool ietf_read_time(ietf_scanner *scanner, ietf_date *date) {
    digit_value digits;
    if (!ietf_read_digits(scanner, (digit_run){.minimum_count = 1, .maximum_count = 2},
                          &digits) ||
        ietf_byte_at(scanner, 0) != ':') {
        return false;
    }
    date->hour = (uint8_t)digits.value;
    scanner->position += 1;
    if (!ietf_read_digits(scanner, (digit_run){.minimum_count = 2, .maximum_count = 2},
                          &digits)) {
        return false;
    }
    date->minute = (uint8_t)digits.value;
    if (ietf_byte_at(scanner, 0) == ':') {
        scanner->position += 1;
        if (!ietf_read_digits(scanner, (digit_run){.minimum_count = 2, .maximum_count = 2},
                              &digits)) {
            return false;
        }
        date->second = (uint8_t)digits.value;
        if (ietf_byte_at(scanner, 0) == '.') {
            scanner->position += 1;
            if (!ietf_is_digit(ietf_byte_at(scanner, 0))) {
                return false;
            }
            uint32_t scale = 100000000U;
            while (ietf_is_digit(ietf_byte_at(scanner, 0))) {
                date->nanosecond += (uint32_t)(ietf_byte_at(scanner, 0) - '0') * scale;
                scale /= 10U;
                scanner->position += 1;
            }
        }
    }
    const size_t before_timezone = scanner->position;
    (void)ietf_skip_spaces(scanner);
    const int byte = ietf_byte_at(scanner, 0);
    if (byte == '+' || byte == '-') {
        return ietf_read_tzoffset(scanner, date);
    }
    if (ietf_read_tzname(scanner, &date->timezone_offset_minutes)) {
        return true;
    }
    scanner->position = before_timezone;
    return true;
}

/* input ::= S? (dayname ","? S)? ((datespec S time) | asctime) S? ;
   datespec ::= daynum dsep monthname dsep year ;
   asctime ::= monthname dsep daynum S time S year. */
static bool ietf_read_input(ietf_scanner *scanner, ietf_date *date) {
    (void)ietf_skip_spaces(scanner);
    if (ietf_match_dayname(scanner)) {
        if (ietf_byte_at(scanner, 0) == ',') {
            scanner->position += 1;
        }
        if (!ietf_skip_spaces(scanner)) {
            return false;
        }
    }
    if (ietf_is_digit(ietf_byte_at(scanner, 0))) {
        if (!ietf_read_daynum(scanner, date) || !ietf_read_date_separator(scanner) ||
            !ietf_read_monthname(scanner, date) || !ietf_read_date_separator(scanner) ||
            !ietf_read_year(scanner, date) || !ietf_skip_spaces(scanner) ||
            !ietf_read_time(scanner, date)) {
            return false;
        }
    } else if (!ietf_read_monthname(scanner, date) || !ietf_read_date_separator(scanner) ||
               !ietf_read_daynum(scanner, date) || !ietf_skip_spaces(scanner) ||
               !ietf_read_time(scanner, date) || !ietf_skip_spaces(scanner) ||
               !ietf_read_year(scanner, date)) {
        return false;
    }
    (void)ietf_skip_spaces(scanner);
    return scanner->position == scanner->text.byte_count;
}

/* True when the components make a valid xs:dateTime; hour 24 is allowed
   only as 24:00:00 (the next day's midnight, XSD 1.1 Part 2, D.3.2). */
static bool ietf_date_is_valid(const ietf_date *date) {
    if (date->day < 1 || date->day > sxml_days_in_month(date->year, date->month) ||
        date->minute > 59 || date->second > 59) {
        return false;
    }
    return date->hour < 24 ||
           (date->hour == 24 && date->minute == 0 && date->second == 0 && date->nanosecond == 0);
}

/* fn:parse-ietf-date($value as xs:string?) as xs:dateTime? (F&O 4.0,
   section 9.10.1): FORG0010 for text outside the grammar or an invalid
   date; no timezone means 00:00. */
static sxml_status call_parse_ietf_date(const sxml_function_call *call,
                                        sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    sxml_string text;
    status = sxml_function_library_read_string_argument(call, 0, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ietf_scanner scanner = {.text = text.utf8, .position = 0};
    ietf_date date = {.year = 0};
    if (!ietf_read_input(&scanner, &date) || !ietf_date_is_valid(&date)) {
        return sxml_function_library_report(call, &invalid_ietf_date_error);
    }
    const bool is_end_of_day = date.hour == 24;
    sxml_date_time date_time = {.year = date.year,
                                .month = date.month,
                                .day = date.day,
                                .hour = is_end_of_day ? 0 : date.hour,
                                .minute = date.minute,
                                .second = date.second,
                                .nanosecond = date.nanosecond,
                                .has_timezone = true,
                                .timezone_offset_minutes = date.timezone_offset_minutes};
    if (is_end_of_day) {
        status = shift_date_time(
            call, (date_time_shift){.date_time = date_time, .minute_count = 1440}, &date_time);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_item item = {.representation = 0};
    status = make_gregorian_item(
        call->item_heap, (gregorian_value){.type = SXML_TYPE_XS_DATE_TIME, .date_time = date_time},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter adjust_date_time_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:dateTime?"),
    SXML_OPTIONAL_PARAMETER("timezone", "xs:dayTimeDuration?")};
static const sxml_function_parameter adjust_date_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:date?"),
    SXML_OPTIONAL_PARAMETER("timezone", "xs:dayTimeDuration?")};
static const sxml_function_parameter adjust_time_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:time?"),
    SXML_OPTIONAL_PARAMETER("timezone", "xs:dayTimeDuration?")};
static const sxml_function_parameter civil_timezone_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:dateTime"),
    SXML_OPTIONAL_PARAMETER("place", "xs:string?")};
static const sxml_function_parameter days_in_month_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "(xs:dateTime | xs:date | xs:gYearMonth)?")};
static const sxml_function_parameter gregorian_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "(xs:dateTime | xs:date | xs:time | xs:gYear | "
                                     "xs:gYearMonth | xs:gMonth | xs:gMonthDay | xs:gDay)?")};
static const sxml_function_parameter record_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "map(*)?")};
static const sxml_function_parameter seconds_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:decimal?")};
static const sxml_function_parameter duration_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:dayTimeDuration?")};
static const sxml_function_parameter unix_parameters[] = {
    SXML_OPTIONAL_PARAMETER("value", "xs:nonNegativeInteger?")};
static const sxml_function_parameter string_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?")};

static const sxml_native_function temporal_functions[] = {
    SXML_FUNCTION_ROW("adjust-dateTime-to-timezone", adjust_date_time_parameters,
                      "xs:dateTime?", SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
                      call_adjust_date_time),
    SXML_FUNCTION_ROW("adjust-date-to-timezone", adjust_date_parameters, "xs:date?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_adjust_date),
    SXML_FUNCTION_ROW("adjust-time-to-timezone", adjust_time_parameters, "xs:time?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_adjust_time),
    SXML_FUNCTION_ROW("civil-timezone", civil_timezone_parameters, "xs:dayTimeDuration",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_civil_timezone),
    SXML_FUNCTION_ROW("days-in-month", days_in_month_parameters, "xs:integer?", 0,
                      call_days_in_month),
    SXML_FUNCTION_ROW("parts-of-dateTime", gregorian_parameters, "fn:dateTime-record?", 0,
                      call_parts_of_date_time),
    SXML_FUNCTION_ROW("build-dateTime", record_parameters,
                      "(xs:dateTime | xs:date | xs:time | xs:gYear | xs:gYearMonth | xs:gMonth "
                      "| xs:gMonthDay | xs:gDay)?",
                      0, call_build_date_time),
    SXML_FUNCTION_ROW("seconds", seconds_parameters, "xs:dayTimeDuration?", 0, call_seconds),
    SXML_FUNCTION_ROW("duration-to-seconds", duration_parameters, "xs:decimal?", 0,
                      call_duration_to_seconds),
    SXML_FUNCTION_ROW("unix-dateTime", unix_parameters, "xs:dateTimeStamp", 0,
                      call_unix_date_time),
    SXML_FUNCTION_ROW("parse-ietf-date", string_parameters, "xs:dateTime?", 0,
                      call_parse_ietf_date),
};

const sxml_native_function_view sxml_function_library_temporal =
    SXML_LIBRARY_PART(temporal_functions);
