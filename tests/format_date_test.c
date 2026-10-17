/*
 * Tests for the date picture engine (src/format_date_internal.h) and the
 * date formatting part of the function library
 * (src/function_library_format_date.c, packet 2.14): calendar names,
 * pictures through the engine, then fn:format-date, fn:format-time and
 * fn:format-dateTime through the public XPath API, with leak checks and
 * allocation-failure loops (D23). Cases are named by their QT4 test case
 * where they come from one.
 *
 * Golden file: tests/format_test.c.
 */
#include "format_date_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* -------------------------------------------------------------- Calendars */

static void test_calendars_are_chosen(sxml_test_state *state) {
    sxml_calendar_choice choice;
    SXML_TEST_EXPECT(state, sxml_calendar_choose(TEXT("AD"), &choice) == SXML_STATUS_OK &&
                                choice.calendar_kind == SXML_CALENDAR_KIND_AD &&
                                !choice.needs_fallback_marker);
    SXML_TEST_EXPECT(state, sxml_calendar_choose(TEXT("Q{}ISO"), &choice) == SXML_STATUS_OK &&
                                choice.calendar_kind == SXML_CALENDAR_KIND_ISO &&
                                !choice.needs_fallback_marker);
    /* QT4 format-date-en152, -en153, -en154: fallback to AD with a marker. */
    static const char *const fallbacks[] = {"CB", "OS",
                                            "Q{http://calendar.example.com/x}CB", "cal:CB"};
    for (size_t name_index = 0; name_index < SXML_TEST_COUNT(fallbacks); name_index += 1) {
        SXML_TEST_EXPECT(state, sxml_calendar_choose(text_view(fallbacks[name_index]), &choice) ==
                                        SXML_STATUS_OK &&
                                    choice.calendar_kind == SXML_CALENDAR_KIND_AD &&
                                    choice.needs_fallback_marker);
    }
    /* QT4 format-date-en155 to -en158, format-date-051. */
    static const char *const invalid[] = {"Q{}ZODIAC", "ZODIAC", ":w", "Q{}1", "", "Q{",
                                          "Q{}unknown", "a:", "1:AD"};
    for (size_t name_index = 0; name_index < SXML_TEST_COUNT(invalid); name_index += 1) {
        SXML_TEST_EXPECT(state, sxml_calendar_choose(text_view(invalid[name_index]), &choice) ==
                                    SXML_STATUS_MALFORMED_INPUT);
    }
    SXML_TEST_EXPECT(state, sxml_date_language_is_english(TEXT("en")));
    SXML_TEST_EXPECT(state, sxml_date_language_is_english(TEXT("EN-gb")));
    SXML_TEST_EXPECT(state, !sxml_date_language_is_english(TEXT("de")));
    SXML_TEST_EXPECT(state, !sxml_date_language_is_english(TEXT("eng")));
}

/* ----------------------------------------------------------- Engine */

/* A value to format with the engine and the text or error expected. */
typedef struct date_case {
    sxml_date_formatting formatting;
    const char *expected;
    sxml_date_picture_error expected_error;
} date_case;

static sxml_status format_case(const date_case *test_case, sxml_allocator allocator,
                               sxml_format_text *text) {
    *text = (sxml_format_text){
        .allocator = allocator, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
    sxml_date_picture_error picture_error = SXML_DATE_PICTURE_ERROR_NONE;
    const sxml_status status = sxml_format_date_time(&test_case->formatting, text, &picture_error);
    if (status == SXML_STATUS_MALFORMED_INPUT && picture_error != test_case->expected_error) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return status;
}

static bool date_case_holds(const date_case *test_case) {
    sxml_counting_allocator *counting_allocator = NULL;
    if (sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                     .failing_allocation_number = 0},
            &counting_allocator) != SXML_STATUS_OK) {
        return false;
    }
    sxml_format_text text;
    const sxml_status status = format_case(
        test_case, sxml_counting_allocator_as_allocator(counting_allocator), &text);
    bool holds = test_case->expected == NULL
                     ? status == SXML_STATUS_MALFORMED_INPUT && text.byte_count == 0
                     : status == SXML_STATUS_OK &&
                           sxml_byte_view_equals(sxml_format_text_view(&text),
                                                 text_view(test_case->expected));
    if (!holds) {
        (void)fprintf(stderr, "%.*s: status %d, gave '%.*s'\n",
                      (int)test_case->formatting.picture_string.byte_count,
                      (const char *)test_case->formatting.picture_string.bytes, (int)status,
                      (int)text.byte_count, text.bytes == NULL ? "" : (const char *)text.bytes);
    }
    sxml_format_text_release(&text);
    holds = holds &&
            sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count == 0;
    sxml_counting_allocator_destroy(counting_allocator);
    return holds;
}

/* 2003-09-07T09:15:06.456+05:30, a Sunday. */
static const sxml_date_time sunday = {.year = 2003,
                                      .month = 9,
                                      .day = 7,
                                      .hour = 9,
                                      .minute = 15,
                                      .second = 6,
                                      .nanosecond = 456000000,
                                      .has_timezone = true,
                                      .timezone_offset_minutes = 330};

/* -0055-12-01, 56 BC, without a timezone. */
static const sxml_date_time antiquity = {.year = -55, .month = 12, .day = 1};

#define AD_CALENDAR {.calendar_kind = SXML_CALENDAR_KIND_AD, .needs_fallback_marker = false}

#define DATE_TIME_CASE(picture, value_text, kind, result)                                    \
    {.formatting = {.picture_string = TEXT(picture),                                         \
                    .value = (value_text),                                                   \
                    .value_kind = (kind),                                                    \
                    .calendar = AD_CALENDAR,                                                 \
                    .needs_language_marker = false},                                         \
     .expected = (result),                                                                   \
     .expected_error = SXML_DATE_PICTURE_ERROR_NONE}

#define VALUE_CASE(picture, result) DATE_TIME_CASE(picture, sunday, SXML_DATE_VALUE_KIND_DATE_TIME, result)

#define ERROR_CASE(picture, kind, error)                                                      \
    {.formatting = {.picture_string = TEXT(picture),                                         \
                    .value = sunday,                                                         \
                    .value_kind = (kind),                                                    \
                    .calendar = AD_CALENDAR,                                                 \
                    .needs_language_marker = false},                                         \
     .expected = NULL,                                                                       \
     .expected_error = (error)}

static void test_pictures_format(sxml_test_state *state) {
    const date_case cases[] = {
        VALUE_CASE("[Y0001]-[M01]-[D01]T[H01]:[m01]:[s01].[f001]", "2003-09-07T09:15:06.456"),
        VALUE_CASE("[[[Y]-[M01]-[D]]]", "[2003-09-7]"),
        VALUE_CASE("([Y01]-[M01]-[D01])", "(03-09-07)"),
        VALUE_CASE("[D1] [MI] [Y]", "7 IX 2003"),
        VALUE_CASE("[FNn], [D1o] [MNn]", "Sunday, 7th September"),
        VALUE_CASE("[FNn,3-4] [MN,*-3] [Mn,4-4]", "Sun SEP sept"),
        VALUE_CASE("[h].[m01][Pn] [PN] [PNn]", "9.15am AM Am"),
        VALUE_CASE("[ D 01 ] [Y9,999,*] [Y,2-2] [Y#0,3]", "07 2,003 03 003"),
        VALUE_CASE("[YWw]", "Two Thousand and Three"),
        VALUE_CASE("[DWwo]", "Seventh"),
        VALUE_CASE("[Yi,6-6]|[d]|[W]|[w]|[F1]", "mmiii |250|36|1|7"),
        VALUE_CASE("[Z]|[z]|[Z0]|[Z0000]|[ZZ]|[ZN]|[Z,8]", "+05:30|GMT+05:30|+5:30|+0530|+05:30|"
                                                     "+05:30|+05:30  "),
        VALUE_CASE("[f]|[f,*-2]|[f,5-5]|[f0'0'0]|[f\xD9\xA0\xD9\xA0]", "456|45|45600|4'5'6|"
                                                                        "\xD9\xA4\xD9\xA5"),
        VALUE_CASE("[C]|[E]", "AD|AD"),
        DATE_TIME_CASE("[Y][EN]|[ZZ]|[Z]", antiquity, SXML_DATE_VALUE_KIND_DATE, "56BC|J|"),
        {.formatting = {.picture_string = TEXT("[Y][E]|[W]"),
                        .value = antiquity,
                        .value_kind = SXML_DATE_VALUE_KIND_DATE,
                        .calendar = {.calendar_kind = SXML_CALENDAR_KIND_ISO,
                                     .needs_fallback_marker = false},
                        .needs_language_marker = false},
         .expected = "55-|48",
         .expected_error = SXML_DATE_PICTURE_ERROR_NONE},
        {.formatting = {.picture_string = TEXT("[MNn]"),
                        .value = sunday,
                        .value_kind = SXML_DATE_VALUE_KIND_DATE,
                        .calendar = {.calendar_kind = SXML_CALENDAR_KIND_AD,
                                     .needs_fallback_marker = true},
                        .needs_language_marker = true},
         .expected = "[Language: en][Calendar: AD]September",
         .expected_error = SXML_DATE_PICTURE_ERROR_NONE},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        SXML_TEST_EXPECT(state, date_case_holds(&cases[case_index]));
    }
}

static void test_invalid_pictures_are_rejected(sxml_test_state *state) {
    const date_case cases[] = {
        ERROR_CASE("[bla]", SXML_DATE_VALUE_KIND_DATE, SXML_DATE_PICTURE_ERROR_SYNTAX),
        ERROR_CASE("[]", SXML_DATE_VALUE_KIND_DATE, SXML_DATE_PICTURE_ERROR_SYNTAX),
        ERROR_CASE("[Y", SXML_DATE_VALUE_KIND_DATE, SXML_DATE_PICTURE_ERROR_SYNTAX),
        ERROR_CASE("Y]", SXML_DATE_VALUE_KIND_DATE, SXML_DATE_PICTURE_ERROR_SYNTAX),
        ERROR_CASE("[Y999#]", SXML_DATE_VALUE_KIND_DATE, SXML_DATE_PICTURE_ERROR_SYNTAX),
        ERROR_CASE("[Y#.0,4-3]", SXML_DATE_VALUE_KIND_DATE, SXML_DATE_PICTURE_ERROR_SYNTAX),
        ERROR_CASE("[Y,0]", SXML_DATE_VALUE_KIND_DATE, SXML_DATE_PICTURE_ERROR_SYNTAX),
        ERROR_CASE("[f#99]", SXML_DATE_VALUE_KIND_TIME, SXML_DATE_PICTURE_ERROR_SYNTAX),
        ERROR_CASE("[f1,*-0]", SXML_DATE_VALUE_KIND_TIME, SXML_DATE_PICTURE_ERROR_SYNTAX),
        /* Syntax is checked before components. */
        ERROR_CASE("[H][yY]", SXML_DATE_VALUE_KIND_DATE, SXML_DATE_PICTURE_ERROR_SYNTAX),
        ERROR_CASE("[D] [H]", SXML_DATE_VALUE_KIND_DATE,
                   SXML_DATE_PICTURE_ERROR_UNAVAILABLE_COMPONENT),
        ERROR_CASE("[E]", SXML_DATE_VALUE_KIND_TIME,
                   SXML_DATE_PICTURE_ERROR_UNAVAILABLE_COMPONENT),
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        SXML_TEST_EXPECT(state, date_case_holds(&cases[case_index]));
    }
}

static void test_engine_survives_every_allocation_failure(sxml_test_state *state) {
    const date_case test_case =
        VALUE_CASE("[Y,8] [D1o] [MNn,3-3] [f,6-6] [z] [YWw]",
                   "00002003 7th Sep 456000 GMT+05:30 Two Thousand and Three");
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 1000; failing_number += 1) {
        sxml_counting_allocator *counting_allocator = NULL;
        status = sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){.backing_allocator =
                                                         sxml_default_allocator(),
                                                     .failing_allocation_number = failing_number},
            &counting_allocator);
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_format_text text;
        status = format_case(&test_case, sxml_counting_allocator_as_allocator(counting_allocator),
                             &text);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_format_text_view(&text),
                                                          text_view(test_case.expected)));
        }
        sxml_format_text_release(&text);
        SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(counting_allocator)
                                        .live_allocation_count == 0);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 2);
}

/* ------------------------------------------------------- Public API */

typedef struct api_fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
} api_fixture;

static sxml_allocator api_allocator(const api_fixture *fixture) {
    return sxml_counting_allocator_as_allocator(fixture->counting_allocator);
}

static sxml_status api_fixture_create(uint64_t failing_allocation_number, api_fixture *fixture) {
    *fixture = (api_fixture){.counting_allocator = NULL, .processor = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &fixture->counting_allocator);
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_create(
            api_allocator(fixture),
            &(sxml_processor_configuration){.allocator = api_allocator(fixture)},
            &fixture->processor);
    }
    return status;
}

/* Destroys the fixture; true when no cell and no block leaked. */
static bool api_fixture_destroy(api_fixture *fixture) {
    bool has_no_leak = true;
    if (fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(fixture->processor);
    if (fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && sxml_counting_allocator_read_counts(
                                         fixture->counting_allocator)
                                             .live_allocation_count == 0;
        sxml_counting_allocator_destroy(fixture->counting_allocator);
    }
    return has_no_leak;
}

/* An expression whose value is one xs:string (or, with expected "",
   possibly the empty sequence), or the error it raises. */
typedef struct api_case {
    const char *expression_text;
    const char *expected;
    const char *expected_error_code;
} api_case;

typedef struct api_outcome {
    sxml_error_record error_record;
    bool matches;
} api_outcome;

static sxml_status run_api_case(const api_fixture *fixture, api_case test_case,
                                api_outcome *outcome) {
    memset(outcome, 0, sizeof(*outcome));
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        api_allocator(fixture),
        &(sxml_xpath_expression_configuration){.processor = fixture->processor,
                                               .expression_text =
                                                   text_view(test_case.expression_text),
                                               .source_name = TEXT("format_date.xpath"),
                                               .error_record = &outcome->error_record},
        &expression);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression, &(sxml_xpath_evaluation_request){.error_record = &outcome->error_record},
            &result);
    }
    sxml_xpath_expression_destroy(expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    outcome->matches =
        test_case.expected != NULL &&
        sxml_sequence_get_representation(result) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_atomic_item_get_datum(result.content, &datum) == SXML_STATUS_OK &&
        datum.datum_kind == SXML_DATUM_KIND_STRING &&
        sxml_byte_view_equals(datum.datum_details.string.utf8, text_view(test_case.expected));
    if (!outcome->matches && test_case.expected != NULL) {
        (void)fprintf(stderr, "%s: wrong value\n", test_case.expression_text);
        if (datum.datum_kind == SXML_DATUM_KIND_STRING) {
            (void)fprintf(stderr, "  gave '%.*s'\n", (int)datum.datum_details.string.utf8.byte_count,
                          (const char *)datum.datum_details.string.utf8.bytes);
        }
    }
    sxml_sequence_release(result);
    return status;
}

static bool api_case_holds(api_case test_case) {
    api_fixture fixture;
    bool holds = false;
    if (api_fixture_create(0, &fixture) == SXML_STATUS_OK) {
        api_outcome outcome;
        const sxml_status status = run_api_case(&fixture, test_case, &outcome);
        if (test_case.expected_error_code != NULL) {
            holds = status != SXML_STATUS_OK &&
                    sxml_byte_view_equals(outcome.error_record.code.local_name,
                                          text_view(test_case.expected_error_code));
        } else {
            holds = status == SXML_STATUS_OK && outcome.matches;
        }
        if (!holds) {
            (void)fprintf(stderr, "%s: status %d '%.*s'\n", test_case.expression_text,
                          (int)status, (int)outcome.error_record.code.local_name.byte_count,
                          outcome.error_record.code.local_name.bytes == NULL
                              ? ""
                              : (const char *)outcome.error_record.code.local_name.bytes);
        }
    }
    return api_fixture_destroy(&fixture) && holds;
}

#define VALUE(expression, expected) {(expression), (expected), NULL}
#define ERROR(expression, code) {(expression), NULL, (code)}

static void test_functions_through_the_api(sxml_test_state *state) {
    static const api_case cases[] = {
        VALUE("format-date(xs:date('2002-12-31'), '[D1o] [MNn], [Y]', 'en', (), ())",
              "31st December, 2002"),
        VALUE("format-date(xs:date('2026-10-02'), '[D] [MNn] [Y]', 'en', 'AD')",
              "2 October 2026"),
        VALUE("format-time(xs:time('15:58:45.762+02:00'), '[h]:[m01]:[s01] [PN] [z,6-6]')",
              "3:58:45 PM GMT+02:00"),
        VALUE("format-dateTime(xs:dateTime('2002-12-31T15:58:45'), "
              "'[M01]/[D01]/[Y0001] at [H01]:[m01]:[s01]')",
              "12/31/2002 at 15:58:45"),
        VALUE("format-date(xs:date('2006-03-01'), '[MNn]', 'xib')", "[Language: en]March"),
        VALUE("format-date(xs:date('2006-03-01'), '[M01]', 'en', 'CB')", "[Calendar: AD]03"),
        VALUE("string(empty(format-date((), '[D]', 'en', 'AD')))", "true"),
        VALUE("format-date(xs:date('2005-01-01'), '[W]', (), 'ISO', ())", "53"),
        ERROR("format-date(xs:date('2006-03-01'), '[M01]', 'en', 'ZODIAC')", "FOFD1340"),
        ERROR("format-date(xs:date('2006-03-01'), '[bla]')", "FOFD1340"),
        ERROR("format-time(xs:time('12:00:00'), '[Y]')", "FOFD1350"),
        ERROR("format-dateTime('abc', '[Y]')", "XPTY0004"),
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        SXML_TEST_EXPECT(state, api_case_holds(cases[case_index]));
    }
}

static void test_functions_survive_every_allocation_failure(sxml_test_state *state) {
    const api_case test_case = VALUE(
        "format-dateTime(xs:dateTime('2003-09-07T09:15:06.456Z'), "
        "'[FNn,3-4] [D1o] [MNn] [Y] [h].[m01][Pn] [Z00:00t]', 'de', 'OS')",
        "[Language: en][Calendar: AD]Sun 7th September 2003 9.15am Z");
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 100000; failing_number += 1) {
        api_fixture fixture;
        status = api_fixture_create(failing_number, &fixture);
        if (status == SXML_STATUS_OK) {
            api_outcome outcome;
            status = run_api_case(&fixture, test_case, &outcome);
            if (status == SXML_STATUS_OK) {
                SXML_TEST_EXPECT(state, outcome.matches);
            }
        }
        SXML_TEST_EXPECT(state, api_fixture_destroy(&fixture));
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            (void)fprintf(stderr, "allocation %llu: status %d\n",
                          (unsigned long long)failing_number, (int)status);
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case format_date_test_cases[] = {
    {.name = "calendars_are_chosen", .function = test_calendars_are_chosen},
    {.name = "pictures_format", .function = test_pictures_format},
    {.name = "invalid_pictures_are_rejected", .function = test_invalid_pictures_are_rejected},
    {.name = "engine_survives_every_allocation_failure",
     .function = test_engine_survives_every_allocation_failure},
    {.name = "functions_through_the_api", .function = test_functions_through_the_api},
    {.name = "functions_survive_every_allocation_failure",
     .function = test_functions_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "format_date",
            .cases = format_date_test_cases,
            .case_count = SXML_TEST_COUNT(format_date_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
