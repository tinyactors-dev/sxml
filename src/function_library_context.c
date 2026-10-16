/*
 * The context part of the function library (packet 1.20, D78): fn:position
 * and fn:last, which lowering reads from the focus (XPath 4.0, section
 * 2.2.2), the clock functions fn:current-dateTime, fn:current-date,
 * fn:current-time and fn:implicit-timezone (F&O 4.0, section 16), and the
 * component functions of dates and times (F&O 4.0, section 9.5).
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

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter date_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:date?")};
static const sxml_function_parameter date_time_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:dateTime?")};
static const sxml_function_parameter time_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:time?")};

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
};

const sxml_native_function_view sxml_function_library_context =
    SXML_LIBRARY_PART(context_functions);
