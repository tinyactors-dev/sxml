/*
 * The context part of the function library (packet 1.20, D78): fn:position
 * and fn:last, which lowering reads from the focus (XPath 4.0, section
 * 2.2.2), the clock functions fn:current-dateTime, fn:current-date,
 * fn:current-time and fn:implicit-timezone (F&O 4.0, section 16), and the
 * component functions of dates and times (F&O 4.0, section 9.5) and, from
 * packet 1.23b (D91), of durations (F&O 4.0, section 8.2), and fn:dateTime.
 *
 * Golden file: src/memory.c (layout and comments); rows as in
 * src/function_library_internal.h.
 *
 * The clock is read once per evaluation (sxml_dynamic_context_read_clock),
 * so the clock functions agree within an evaluation (F&O 4.0, section 1.7:
 * deterministic within an execution scope). The reading is presented in
 * the implicit timezone, as fn:current-dateTime requires.
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"

/* ---------------------------------------------------------------- Clock */

static const sxml_static_error no_dynamic_context_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The function needs the dynamic context of an evaluation (XPath 4.0, "
        "section 2.2.2)"),
};

/* A date/time value of a built-in type. */
typedef struct date_time_value {
    sxml_builtin_type type;
    sxml_date_time date_time;
} date_time_value;

static sxml_status make_date_time_item(sxml_item_heap *item_heap, date_time_value value,
                                       sxml_item *item) {
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(value.type),
            .datum = {.datum_kind = SXML_DATUM_KIND_DATE_TIME,
                      .datum_details = {.date_time = value.date_time}}},
        item);
}

/* The current dateTime of the evaluation in the implicit timezone. */
static sxml_status read_current_date_time(const sxml_function_call *call,
                                          sxml_date_time *current) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    sxml_clock_reading reading;
    sxml_status status = sxml_dynamic_context_read_clock(call->dynamic_context, &reading);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const int16_t target = reading.implicit_timezone_offset_minutes;
    const int16_t source = reading.current_date_time.timezone_offset_minutes;
    if (target == source) {
        *current = reading.current_date_time;
        return SXML_STATUS_OK;
    }
    /* The same instant in the implicit timezone: add the difference of
       the offsets, then relabel (F&O 4.0, fn:adjust-dateTime-to-timezone). */
    sxml_item instant = {.representation = 0};
    status = make_date_time_item(call->item_heap,
                                     (date_time_value){.type = SXML_TYPE_XS_DATE_TIME, .date_time = reading.current_date_time},
                                     &instant);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item difference = {.representation = 0};
    status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DAY_TIME_DURATION),
            .datum = {.datum_kind = SXML_DATUM_KIND_DURATION,
                      .datum_details = {.duration = {
                                            .months = 0,
                                            .seconds = ((int64_t)target - source) * 60,
                                            .nanoseconds = 0}}}},
        &difference);
    sxml_item shifted = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_operator_make_temporal_value(
            call->item_heap,
            &(sxml_temporal_arithmetic){.arithmetic_operator = SXML_ARITHMETIC_OPERATOR_ADD,
                                        .left_item = instant,
                                        .right_item = difference,
                                        .implicit_timezone_offset_minutes = target,
                                        .error_record = call->error_record},
            &shifted);
        sxml_item_release(difference);
    }
    sxml_item_release(instant);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(shifted, &datum);
        sxml_item_release(shifted);
    }
    if (status == SXML_STATUS_OK) {
        *current = datum.datum_details.date_time;
        current->timezone_offset_minutes = target;
    }
    return status;
}

/* fn:current-dateTime() as xs:dateTimeStamp (F&O 4.0, section 16.1). */
static sxml_status call_current_date_time(const sxml_function_call *call,
                                          sxml_sequence *result) {
    sxml_date_time current;
    sxml_status status = read_current_date_time(call, &current);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = make_date_time_item(call->item_heap,
                                     (date_time_value){.type = SXML_TYPE_XS_DATE_TIME_STAMP, .date_time = current},
                                     &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:current-date() as xs:date (F&O 4.0, section 16.2). */
static sxml_status call_current_date(const sxml_function_call *call,
                                     sxml_sequence *result) {
    sxml_date_time current;
    sxml_status status = read_current_date_time(call, &current);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        current.hour = 0;
        current.minute = 0;
        current.second = 0;
        current.nanosecond = 0;
        status = make_date_time_item(call->item_heap,
                                     (date_time_value){.type = SXML_TYPE_XS_DATE, .date_time = current},
                                     &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:current-time() as xs:time (F&O 4.0, section 16.3). */
static sxml_status call_current_time(const sxml_function_call *call,
                                     sxml_sequence *result) {
    sxml_date_time current;
    sxml_status status = read_current_date_time(call, &current);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        current.year = 0;
        current.month = 0;
        current.day = 0;
        status = make_date_time_item(call->item_heap,
                                     (date_time_value){.type = SXML_TYPE_XS_TIME, .date_time = current},
                                     &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:implicit-timezone() as xs:dayTimeDuration (F&O 4.0, section 16.4). */
static sxml_status call_implicit_timezone(const sxml_function_call *call,
                                          sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    int16_t offset_minutes = 0;
    sxml_status status =
        sxml_dynamic_context_read_implicit_timezone(call->dynamic_context, &offset_minutes);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DAY_TIME_DURATION),
                .datum = {.datum_kind = SXML_DATUM_KIND_DURATION,
                          .datum_details = {.duration = {.months = 0,
                                                         .seconds = (int64_t)offset_minutes * 60,
                                                         .nanoseconds = 0}}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------------------ Components */

/* Which component of a date, time or dateTime (F&O 4.0, section 9.5). */
typedef enum date_time_component {
    SXML_DATE_TIME_COMPONENT_YEAR,
    SXML_DATE_TIME_COMPONENT_MONTH,
    SXML_DATE_TIME_COMPONENT_DAY,
    SXML_DATE_TIME_COMPONENT_HOURS,
    SXML_DATE_TIME_COMPONENT_MINUTES,
    SXML_DATE_TIME_COMPONENT_SECONDS,
    SXML_DATE_TIME_COMPONENT_TIMEZONE,
} date_time_component;

/* Writes xs:decimal seconds with nine fraction digits at most. */
static sxml_status make_seconds(sxml_item_heap *item_heap, sxml_date_time date_time,
                                sxml_item *item) {
    uint8_t text[16];
    size_t byte_count = 0;
    text[byte_count++] = (uint8_t)('0' + date_time.second / 10U);
    text[byte_count++] = (uint8_t)('0' + date_time.second % 10U);
    uint32_t fraction = date_time.nanosecond;
    if (fraction > 0) {
        text[byte_count++] = '.';
    }
    for (uint32_t divisor = 100000000U; divisor > 0 && fraction > 0; divisor /= 10U) {
        text[byte_count++] = (uint8_t)('0' + fraction / divisor);
        fraction %= divisor;
    }
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL),
            .datum = {.datum_kind = SXML_DATUM_KIND_DECIMAL,
                      .datum_details = {.decimal_text = {.bytes = text,
                                                         .byte_count = byte_count}}}},
        item);
}

/* Whether a value of a Gregorian primitive type has the component (XDM
   4.0, section 5.4: absent components are zero in the datum). */
static bool component_is_present(sxml_type_identifier primitive_type,
                                 date_time_component component) {
    const uint32_t type = primitive_type.index;
    const bool has_date = type == SXML_TYPE_XS_DATE_TIME || type == SXML_TYPE_XS_DATE;
    switch (component) {
    case SXML_DATE_TIME_COMPONENT_YEAR:
        return has_date || type == SXML_TYPE_XS_G_YEAR_MONTH || type == SXML_TYPE_XS_G_YEAR;
    case SXML_DATE_TIME_COMPONENT_MONTH:
        return has_date || type == SXML_TYPE_XS_G_YEAR_MONTH || type == SXML_TYPE_XS_G_MONTH ||
               type == SXML_TYPE_XS_G_MONTH_DAY;
    case SXML_DATE_TIME_COMPONENT_DAY:
        return has_date || type == SXML_TYPE_XS_G_MONTH_DAY || type == SXML_TYPE_XS_G_DAY;
    case SXML_DATE_TIME_COMPONENT_HOURS:
    case SXML_DATE_TIME_COMPONENT_MINUTES:
    case SXML_DATE_TIME_COMPONENT_SECONDS:
        return type == SXML_TYPE_XS_DATE_TIME || type == SXML_TYPE_XS_TIME;
    case SXML_DATE_TIME_COMPONENT_TIMEZONE:
        return true;
    }
    return false;
}

static sxml_status call_component(const sxml_function_call *call,
                                  date_time_component component, sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!value.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(value.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_DATE_TIME) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_date_time date_time = datum.datum_details.date_time;
    if (!component_is_present(sxml_atomic_item_get_primitive_type(value.item), component)) {
        /* F&O 4.0, section 9.6.1: the empty sequence when the component is
           absent, as the year of an xs:time. */
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_item item = sxml_atomic_item_from_integer(0);
    switch (component) {
    case SXML_DATE_TIME_COMPONENT_YEAR:
        item = sxml_atomic_item_from_integer(date_time.year);
        break;
    case SXML_DATE_TIME_COMPONENT_MONTH:
        item = sxml_atomic_item_from_integer(date_time.month);
        break;
    case SXML_DATE_TIME_COMPONENT_DAY:
        item = sxml_atomic_item_from_integer(date_time.day);
        break;
    case SXML_DATE_TIME_COMPONENT_HOURS:
        item = sxml_atomic_item_from_integer(date_time.hour);
        break;
    case SXML_DATE_TIME_COMPONENT_MINUTES:
        item = sxml_atomic_item_from_integer(date_time.minute);
        break;
    case SXML_DATE_TIME_COMPONENT_SECONDS:
        status = make_seconds(call->item_heap, date_time, &item);
        break;
    case SXML_DATE_TIME_COMPONENT_TIMEZONE:
        if (!date_time.has_timezone) {
            *result = sxml_sequence_empty();
            return SXML_STATUS_OK;
        }
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DAY_TIME_DURATION),
                .datum = {.datum_kind = SXML_DATUM_KIND_DURATION,
                          .datum_details = {.duration = {
                                                .months = 0,
                                                .seconds =
                                                    (int64_t)date_time.timezone_offset_minutes * 60,
                                                .nanoseconds = 0}}}},
            &item);
        break;
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

static sxml_status call_year(const sxml_function_call *call, sxml_sequence *result) {
    return call_component(call, SXML_DATE_TIME_COMPONENT_YEAR, result);
}

static sxml_status call_month(const sxml_function_call *call, sxml_sequence *result) {
    return call_component(call, SXML_DATE_TIME_COMPONENT_MONTH, result);
}

static sxml_status call_day(const sxml_function_call *call, sxml_sequence *result) {
    return call_component(call, SXML_DATE_TIME_COMPONENT_DAY, result);
}

static sxml_status call_hours(const sxml_function_call *call, sxml_sequence *result) {
    return call_component(call, SXML_DATE_TIME_COMPONENT_HOURS, result);
}

static sxml_status call_minutes(const sxml_function_call *call, sxml_sequence *result) {
    return call_component(call, SXML_DATE_TIME_COMPONENT_MINUTES, result);
}

static sxml_status call_seconds(const sxml_function_call *call, sxml_sequence *result) {
    return call_component(call, SXML_DATE_TIME_COMPONENT_SECONDS, result);
}

static sxml_status call_timezone(const sxml_function_call *call, sxml_sequence *result) {
    return call_component(call, SXML_DATE_TIME_COMPONENT_TIMEZONE, result);
}

/* ---------------------------------------------------------- fn:dateTime */

static const sxml_static_error different_timezones_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0008"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The date and the time have different timezones (F&O 4.0, fn:dateTime)"),
};

/* Reads a date/time argument: has_value false for the empty sequence. */
typedef struct date_time_argument {
    bool has_value;
    sxml_date_time value;
} date_time_argument;

static sxml_status read_date_time(const sxml_function_call *call, size_t argument_index,
                                  date_time_argument *argument) {
    argument->has_value = false;
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
        *argument = (date_time_argument){.has_value = true,
                                         .value = datum.datum_details.date_time};
    }
    return status;
}

/* fn:dateTime($date, $time) (F&O 4.0, section 9.3.1): the date's
   components with the time's; the timezone of whichever has one, FORG0008
   when both have different ones. */
static sxml_status call_date_time(const sxml_function_call *call, sxml_sequence *result) {
    date_time_argument date = {.has_value = false};
    date_time_argument time = {.has_value = false};
    sxml_status status = read_date_time(call, 0, &date);
    if (status == SXML_STATUS_OK) {
        status = read_date_time(call, 1, &time);
    }
    if (status != SXML_STATUS_OK || !date.has_value || !time.has_value) {
        *result = sxml_sequence_empty();
        return status;
    }
    if (date.value.has_timezone && time.value.has_timezone &&
        date.value.timezone_offset_minutes != time.value.timezone_offset_minutes) {
        return sxml_function_library_report(call, &different_timezones_error);
    }
    const bool has_timezone = date.value.has_timezone || time.value.has_timezone;
    int16_t timezone_offset_minutes = time.value.timezone_offset_minutes;
    if (date.value.has_timezone) {
        timezone_offset_minutes = date.value.timezone_offset_minutes;
    }
    const sxml_date_time combined = {
        .year = date.value.year,
        .month = date.value.month,
        .day = date.value.day,
        .hour = time.value.hour,
        .minute = time.value.minute,
        .second = time.value.second,
        .nanosecond = time.value.nanosecond,
        .has_timezone = has_timezone,
        .timezone_offset_minutes = timezone_offset_minutes};
    sxml_item item = {.representation = 0};
    status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DATE_TIME),
            .datum = {.datum_kind = SXML_DATUM_KIND_DATE_TIME,
                      .datum_details = {.date_time = combined}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------- Duration components */

/* Which component of a duration (F&O 4.0, section 8.2; packet 1.23b). */
typedef enum duration_component {
    SXML_DURATION_COMPONENT_YEARS,
    SXML_DURATION_COMPONENT_MONTHS,
    SXML_DURATION_COMPONENT_DAYS,
    SXML_DURATION_COMPONENT_HOURS,
    SXML_DURATION_COMPONENT_MINUTES,
    SXML_DURATION_COMPONENT_SECONDS,
} duration_component;

/* ($seconds mod 60) as xs:decimal, with the duration's sign (F&O 4.0,
   fn:seconds-from-duration). */
static sxml_status make_duration_seconds(sxml_item_heap *item_heap, sxml_duration duration,
                                         sxml_item *item) {
    const bool is_negative = duration.seconds < 0 || duration.nanoseconds < 0;
    const uint64_t whole = (uint64_t)(duration.seconds < 0 ? -(duration.seconds % 60)
                                                          : duration.seconds % 60);
    uint32_t fraction = (uint32_t)(duration.nanoseconds < 0 ? -(int64_t)duration.nanoseconds
                                                            : (int64_t)duration.nanoseconds);
    uint8_t text[16];
    size_t byte_count = 0;
    if (is_negative && (whole > 0 || fraction > 0)) {
        text[byte_count++] = '-';
    }
    if (whole >= 10) {
        text[byte_count++] = (uint8_t)('0' + whole / 10U);
    }
    text[byte_count++] = (uint8_t)('0' + whole % 10U);
    if (fraction > 0) {
        text[byte_count++] = '.';
    }
    for (uint32_t divisor = 100000000U; divisor > 0 && fraction > 0; divisor /= 10U) {
        text[byte_count++] = (uint8_t)('0' + fraction / divisor);
        fraction %= divisor;
    }
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL),
            .datum = {.datum_kind = SXML_DATUM_KIND_DECIMAL,
                      .datum_details = {.decimal_text = {.bytes = text,
                                                         .byte_count = byte_count}}}},
        item);
}

/* fn:years-from-duration and its siblings (F&O 4.0, sections 8.2.1 to
   8.2.6): a duration is a (months, seconds) tuple; years is months idiv
   12, months is months mod 12, days is seconds idiv 86400, hours (seconds
   mod 86400) idiv 3600, minutes (seconds mod 3600) idiv 60, seconds
   seconds mod 60, each with the sign of the duration. */
static sxml_status call_duration_component(const sxml_function_call *call,
                                           duration_component component,
                                           sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(value.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_DURATION) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_duration duration = datum.datum_details.duration;
    /* C division truncates toward zero, as idiv and mod do. */
    int64_t component_value = 0;
    sxml_item item = {.representation = 0};
    switch (component) {
    case SXML_DURATION_COMPONENT_YEARS:
        component_value = duration.months / 12;
        break;
    case SXML_DURATION_COMPONENT_MONTHS:
        component_value = duration.months % 12;
        break;
    case SXML_DURATION_COMPONENT_DAYS:
        component_value = duration.seconds / 86400;
        break;
    case SXML_DURATION_COMPONENT_HOURS:
        component_value = (duration.seconds % 86400) / 3600;
        break;
    case SXML_DURATION_COMPONENT_MINUTES:
        component_value = (duration.seconds % 3600) / 60;
        break;
    case SXML_DURATION_COMPONENT_SECONDS:
        status = make_duration_seconds(call->item_heap, duration, &item);
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(item);
        }
        return status;
    }
    *result = sxml_sequence_from_item(sxml_atomic_item_from_integer(component_value));
    return SXML_STATUS_OK;
}

static sxml_status call_years_from_duration(const sxml_function_call *call,
                                            sxml_sequence *result) {
    return call_duration_component(call, SXML_DURATION_COMPONENT_YEARS, result);
}

static sxml_status call_months_from_duration(const sxml_function_call *call,
                                             sxml_sequence *result) {
    return call_duration_component(call, SXML_DURATION_COMPONENT_MONTHS, result);
}

static sxml_status call_days_from_duration(const sxml_function_call *call,
                                           sxml_sequence *result) {
    return call_duration_component(call, SXML_DURATION_COMPONENT_DAYS, result);
}

static sxml_status call_hours_from_duration(const sxml_function_call *call,
                                            sxml_sequence *result) {
    return call_duration_component(call, SXML_DURATION_COMPONENT_HOURS, result);
}

static sxml_status call_minutes_from_duration(const sxml_function_call *call,
                                              sxml_sequence *result) {
    return call_duration_component(call, SXML_DURATION_COMPONENT_MINUTES, result);
}

static sxml_status call_seconds_from_duration(const sxml_function_call *call,
                                              sxml_sequence *result) {
    return call_duration_component(call, SXML_DURATION_COMPONENT_SECONDS, result);
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter date_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:date?")};
/* The Gregorian values of F&O 4.0, section 9.6.1 (PR 1481): every
   component function of a dateTime takes them all. */
static const sxml_function_parameter date_time_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "(xs:dateTime | xs:date | xs:time | xs:gYear | "
                                     "xs:gYearMonth | xs:gMonth | xs:gMonthDay | xs:gDay)?")};
static const sxml_function_parameter time_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:time?")};
static const sxml_function_parameter date_and_time_parameters[] = {
    SXML_REQUIRED_PARAMETER("date", "xs:date?"), SXML_REQUIRED_PARAMETER("time", "xs:time?")};
static const sxml_function_parameter duration_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:duration?")};

static const sxml_native_function context_functions[] = {
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("position"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:integer"),
     .call = NULL,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_READS_CONTEXT_SIZE,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("last"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:integer"),
     .call = NULL,
     .resumable_body = NULL},
    SXML_NULLARY_FUNCTION_ROW("current-dateTime", "xs:dateTimeStamp",
                              SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
                              call_current_date_time),
    SXML_NULLARY_FUNCTION_ROW("current-date", "xs:date",
                              SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_current_date),
    SXML_NULLARY_FUNCTION_ROW("current-time", "xs:time",
                              SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_current_time),
    SXML_NULLARY_FUNCTION_ROW("implicit-timezone", "xs:dayTimeDuration",
                              SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
                              call_implicit_timezone),
    SXML_FUNCTION_ROW("year-from-date", date_parameters, "xs:integer?", 0, call_year),
    SXML_FUNCTION_ROW("month-from-date", date_parameters, "xs:integer?", 0, call_month),
    SXML_FUNCTION_ROW("day-from-date", date_parameters, "xs:integer?", 0, call_day),
    SXML_FUNCTION_ROW("year-from-dateTime", date_time_parameters, "xs:integer?", 0,
                      call_year),
    SXML_FUNCTION_ROW("month-from-dateTime", date_time_parameters, "xs:integer?", 0,
                      call_month),
    SXML_FUNCTION_ROW("day-from-dateTime", date_time_parameters, "xs:integer?", 0,
                      call_day),
    SXML_FUNCTION_ROW("hours-from-dateTime", date_time_parameters, "xs:integer?", 0,
                      call_hours),
    SXML_FUNCTION_ROW("minutes-from-dateTime", date_time_parameters, "xs:integer?", 0,
                      call_minutes),
    SXML_FUNCTION_ROW("seconds-from-dateTime", date_time_parameters, "xs:decimal?", 0,
                      call_seconds),
    SXML_FUNCTION_ROW("hours-from-time", time_parameters, "xs:integer?", 0, call_hours),
    SXML_FUNCTION_ROW("minutes-from-time", time_parameters, "xs:integer?", 0,
                      call_minutes),
    SXML_FUNCTION_ROW("seconds-from-time", time_parameters, "xs:decimal?", 0,
                      call_seconds),
    SXML_FUNCTION_ROW("timezone-from-dateTime", date_time_parameters, "xs:dayTimeDuration?",
                      0, call_timezone),
    SXML_FUNCTION_ROW("timezone-from-date", date_parameters, "xs:dayTimeDuration?", 0,
                      call_timezone),
    SXML_FUNCTION_ROW("timezone-from-time", time_parameters, "xs:dayTimeDuration?", 0,
                      call_timezone),
    SXML_FUNCTION_ROW("dateTime", date_and_time_parameters, "xs:dateTime?", 0, call_date_time),
    SXML_FUNCTION_ROW("years-from-duration", duration_parameters, "xs:integer?", 0,
                      call_years_from_duration),
    SXML_FUNCTION_ROW("months-from-duration", duration_parameters, "xs:integer?", 0,
                      call_months_from_duration),
    SXML_FUNCTION_ROW("days-from-duration", duration_parameters, "xs:integer?", 0,
                      call_days_from_duration),
    SXML_FUNCTION_ROW("hours-from-duration", duration_parameters, "xs:integer?", 0,
                      call_hours_from_duration),
    SXML_FUNCTION_ROW("minutes-from-duration", duration_parameters, "xs:integer?", 0,
                      call_minutes_from_duration),
    SXML_FUNCTION_ROW("seconds-from-duration", duration_parameters, "xs:decimal?", 0,
                      call_seconds_from_duration),
};

const sxml_native_function_view sxml_function_library_context =
    SXML_LIBRARY_PART(context_functions);
