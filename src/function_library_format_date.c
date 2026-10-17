/*
 * The date formatting part of the function library (packet 2.14, D97):
 * fn:format-dateTime, fn:format-date and fn:format-time (F&O 4.0,
 * sections 9.9.1 to 9.9.3), over the picture engine of
 * src/format_date_internal.h.
 *
 * Golden file: src/function_library_format.c (a part over a picture
 * engine).
 *
 * Languages and calendars (section 9.9.4.8, D97): English and the
 * calendars AD (the default) and ISO. Another language is formatted in
 * English after "[Language: en]", another calendar designator or a
 * calendar in a namespace as AD after "[Calendar: AD]".
 * $place is not used: without a timezone database (D97) no place changes
 * the timezone or its name.
 */
#include "format_date_internal.h"
#include "function_library_core_internal.h"

/* ------------------------------------------------------------- Errors */

static const sxml_static_error invalid_picture_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOFD1340"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The picture string or the calendar of a date formatting function is not valid (F&O "
        "4.0, sections 9.9.4.1 and 9.9.4.8)"),
};

static const sxml_static_error unavailable_component_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOFD1350"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The picture names a component the value does not have (F&O 4.0, section 9.9.4.1)"),
};

/* ------------------------------------------------------------- Bodies */

/* Reads $language into the formatting's language marker: needed unless it is English or the
   empty sequence (the default language, which sxml formats in English
   whatever the host names: D97). */
static sxml_status read_language(const sxml_function_call *call,
                                 sxml_date_formatting *formatting) {
    sxml_optional_item argument;
    const sxml_status status = sxml_function_library_read_optional_item(call, 2, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view language = SXML_BYTE_VIEW_FROM_LITERAL("en");
    if (argument.has_item) {
        sxml_atomic_datum datum;
        if (sxml_atomic_item_get_datum(argument.item, &datum) == SXML_STATUS_OK &&
            datum.datum_kind == SXML_DATUM_KIND_STRING) {
            language = datum.datum_details.string.utf8;
        }
    }
    formatting->needs_language_marker =
        language.byte_count > 0 && !sxml_date_language_is_english(language);
    return SXML_STATUS_OK;
}

/* Reads $calendar: AD for the empty sequence; FOFD1340 for a name that is
   not an EQName or a known designator. */
static sxml_status read_calendar(const sxml_function_call *call, sxml_calendar_choice *choice) {
    *choice = (sxml_calendar_choice){.calendar_kind = SXML_CALENDAR_KIND_AD,
                                     .needs_fallback_marker = false};
    sxml_optional_item argument;
    const sxml_status status = sxml_function_library_read_optional_item(call, 3, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(argument.item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING ||
        sxml_calendar_choose(datum.datum_details.string.utf8, choice) != SXML_STATUS_OK) {
        return sxml_function_library_report(call, &invalid_picture_error);
    }
    return SXML_STATUS_OK;
}

/*
 * The body of the three functions ($value, $picture, $language :=
 * (), $calendar := (), $place := ()) as xs:string? (section 9.9.4): the
 * empty sequence for an empty $value.
 */
static sxml_status call_format(const sxml_function_call *call, sxml_date_value_kind value_kind,
                               sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    sxml_string picture_string = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    status = sxml_function_library_read_string_argument(call, 1, &picture_string);
    sxml_date_formatting formatting = {.picture_string = picture_string.utf8,
                                       .value_kind = value_kind,
                                       .needs_language_marker = false};
    if (status == SXML_STATUS_OK) {
        status = read_language(call, &formatting);
    }
    if (status == SXML_STATUS_OK) {
        status = read_calendar(call, &formatting.calendar);
    }
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(value.item, &datum);
    }
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_DATE_TIME) {
        /* Coercion to the parameter type happened before the call. */
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    formatting.picture_string = picture_string.utf8;
    formatting.value = datum.datum_details.date_time;
    sxml_format_text text = {.allocator = sxml_item_heap_as_allocator(call->item_heap),
                             .bytes = NULL,
                             .byte_count = 0,
                             .byte_capacity = 0};
    sxml_date_picture_error picture_error = SXML_DATE_PICTURE_ERROR_NONE;
    status = sxml_format_date_time(&formatting, &text, &picture_error);
    if (status == SXML_STATUS_MALFORMED_INPUT &&
        picture_error != SXML_DATE_PICTURE_ERROR_NONE) {
        status = sxml_function_library_report(
            call, picture_error == SXML_DATE_PICTURE_ERROR_SYNTAX ? &invalid_picture_error
                                                                  : &unavailable_component_error);
    }
    /* The work is linear in the picture and the result. */
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(
            call, (uint64_t)picture_string.utf8.byte_count + text.byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(call->item_heap, sxml_format_text_view(&text),
                                                   result);
    }
    sxml_format_text_release(&text);
    return status;
}

static sxml_status call_format_date_time(const sxml_function_call *call, sxml_sequence *result) {
    return call_format(call, SXML_DATE_VALUE_KIND_DATE_TIME, result);
}

static sxml_status call_format_date(const sxml_function_call *call, sxml_sequence *result) {
    return call_format(call, SXML_DATE_VALUE_KIND_DATE, result);
}

static sxml_status call_format_time(const sxml_function_call *call, sxml_sequence *result) {
    return call_format(call, SXML_DATE_VALUE_KIND_TIME, result);
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter format_date_time_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:dateTime?"),
    SXML_REQUIRED_PARAMETER("picture", "xs:string"),
    SXML_OPTIONAL_PARAMETER("language", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("calendar", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("place", "xs:string?"),
};

static const sxml_function_parameter format_date_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:date?"),
    SXML_REQUIRED_PARAMETER("picture", "xs:string"),
    SXML_OPTIONAL_PARAMETER("language", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("calendar", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("place", "xs:string?"),
};

static const sxml_function_parameter format_time_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:time?"),
    SXML_REQUIRED_PARAMETER("picture", "xs:string"),
    SXML_OPTIONAL_PARAMETER("language", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("calendar", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("place", "xs:string?"),
};

/* Context-dependent: default language, calendar and place (sections
   9.9.1 to 9.9.3). */
static const sxml_native_function format_date_functions[] = {
    SXML_FUNCTION_ROW("format-dateTime", format_date_time_parameters, "xs:string?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_format_date_time),
    SXML_FUNCTION_ROW("format-date", format_date_parameters, "xs:string?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_format_date),
    SXML_FUNCTION_ROW("format-time", format_time_parameters, "xs:string?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_format_time),
};

const sxml_native_function_view sxml_function_library_format_date =
    SXML_LIBRARY_PART(format_date_functions);
