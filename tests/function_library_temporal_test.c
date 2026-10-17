/*
 * Tests for the temporal part of the function library
 * (src/function_library_temporal.c, packet 2.8) and the Gregorian
 * component functions of src/function_library_context.c, through the
 * public API: expressions compiled and evaluated by a processor whose
 * fixed clock reads 2026-10-16T12:30:45.5-05:00 with the implicit timezone
 * +02:00 (D88), their values written by the adaptive method, errors
 * checked by code. Each test checks that no cell or allocation leaked, and
 * one runs a mix of the functions under every allocation failure.
 *
 * Golden file: tests/memory_test.c (fixture, lifecycle, allocation-failure
 * loops), with the running of tests/xpath_test.c. Cases are named by the
 * F&O 4.0 example or QT4 test case they follow where there is one.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ---------------------------------------------------------------- Fixture */

/* 2026-10-16T12:30:45.5-05:00, implicit timezone +02:00. */
static const sxml_clock_reading fixture_clock_reading = {
    .current_date_time = {.year = 2026,
                          .month = 10,
                          .day = 16,
                          .hour = 12,
                          .minute = 30,
                          .second = 45,
                          .nanosecond = 500000000,
                          .has_timezone = true,
                          .timezone_offset_minutes = -300},
    .implicit_timezone_offset_minutes = 120,
};

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){.allocator = allocator,
                                        .clock = sxml_fixed_clock(&fixture_clock_reading)},
        &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    return status;
}

/* Destroys the fixture; true when no cell and no allocation leaked. */
static bool fixture_destroy(fixture *test_fixture) {
    bool has_no_leak = true;
    if (test_fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(test_fixture->processor);
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && sxml_counting_allocator_read_counts(
                                         test_fixture->counting_allocator)
                                             .live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

/* --------------------------------------------------------------- Running */

/* Compiles and evaluates expression_text and writes its value by the
   adaptive method, items separated by a space, into the fixture's output
   buffer (cleared first). error_record receives the failure. */
typedef struct query_run {
    sxml_byte_view expression_text;
    sxml_error_record error_record;
} query_run;

static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){.processor = test_fixture->processor,
                                               .expression_text = run->expression_text,
                                               .source_name = TEXT("test.xpath"),
                                               .error_record = &run->error_record},
        &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xpath_expression_evaluate(
        expression, &(sxml_xpath_evaluation_request){.error_record = &run->error_record},
        &result);
    sxml_xpath_expression_destroy(expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_serialize_sequence(
        sxml_processor_get_item_heap(test_fixture->processor),
        &(sxml_serialization_request){
            .value = result,
            .serialization_parameters = {.serialization_method =
                                             SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                         .has_item_separator = true,
                                         .item_separator = TEXT(" ")},
            .error_record = &run->error_record},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    sxml_sequence_release(result);
    return status;
}

/* An expression and its adaptive output, or the code of the error it
   raises. */
typedef struct expression_case {
    const char *expression_text;
    const char *expected_output;
    const char *expected_error_code;
} expression_case;

#define VALUE_CASE(text, output)                                              \
    {.expression_text = (text), .expected_output = (output), .expected_error_code = NULL}
#define ERROR_CASE(text, code)                                                \
    {.expression_text = (text), .expected_output = NULL, .expected_error_code = (code)}

static bool case_holds(const fixture *test_fixture, const expression_case *test_case) {
    query_run run = {.expression_text = sxml_byte_view_from_c_string(test_case->expression_text)};
    const sxml_status status = run_query(test_fixture, &run);
    bool holds = false;
    if (test_case->expected_error_code != NULL) {
        holds = status != SXML_STATUS_OK &&
                sxml_byte_view_equals(run.error_record.code.local_name,
                                      sxml_byte_view_from_c_string(test_case->expected_error_code));
    } else {
        holds = status == SXML_STATUS_OK &&
                sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                      sxml_byte_view_from_c_string(test_case->expected_output));
    }
    if (!holds) {
        const sxml_byte_view output = sxml_output_buffer_get_bytes(test_fixture->output_buffer);
        const sxml_byte_view status_name = sxml_status_name(status);
        (void)fprintf(stderr, "case %s: status %.*s, error %.*s, output %.*s\n",
                      test_case->expression_text, (int)status_name.byte_count,
                      (const char *)status_name.bytes,
                      (int)run.error_record.code.local_name.byte_count,
                      (const char *)run.error_record.code.local_name.bytes,
                      (int)output.byte_count, (const char *)output.bytes);
    }
    return holds;
}

/* Runs every case in one processor, then checks for leaks. */
static void expect_all(sxml_test_state *state, const expression_case *cases,
                       size_t case_count) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        SXML_TEST_EXPECT(state, case_holds(&test_fixture, &cases[case_index]));
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------- Timezones */

static void test_adjusting_timezones(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* F&O 4.0, section 9.7.1 examples, with the implicit timezone
           +02:00 for the one-argument forms. */
        VALUE_CASE("adjust-dateTime-to-timezone(xs:dateTime('2002-03-07T10:00:00'))",
                   "xs:dateTime(\"2002-03-07T10:00:00+02:00\")"),
        VALUE_CASE("adjust-dateTime-to-timezone(xs:dateTime('2002-03-07T10:00:00-07:00'))",
                   "xs:dateTime(\"2002-03-07T19:00:00+02:00\")"),
        VALUE_CASE("adjust-dateTime-to-timezone(xs:dateTime('2002-03-07T10:00:00-07:00'), "
                   "xs:dayTimeDuration('PT10H'))",
                   "xs:dateTime(\"2002-03-08T03:00:00+10:00\")"),
        VALUE_CASE("adjust-dateTime-to-timezone(xs:dateTime('2002-03-07T00:00:00+01:00'), "
                   "xs:dayTimeDuration('-PT8H'))",
                   "xs:dateTime(\"2002-03-06T15:00:00-08:00\")"),
        VALUE_CASE("adjust-dateTime-to-timezone(xs:dateTime('2002-03-07T10:00:00-07:00'), ())",
                   "xs:dateTime(\"2002-03-07T10:00:00\")"),
        VALUE_CASE("adjust-dateTime-to-timezone(())", ""),
        /* fn-adjust-dateTime-to-timezone1args-1: across a year. */
        VALUE_CASE("adjust-dateTime-to-timezone(xs:dateTime('1970-01-01T00:00:00Z'), "
                   "xs:dayTimeDuration('-PT10H'))",
                   "xs:dateTime(\"1969-12-31T14:00:00-10:00\")"),
        /* Section 9.7.2 examples. */
        VALUE_CASE("adjust-date-to-timezone(xs:date('2002-03-07'))",
                   "xs:date(\"2002-03-07+02:00\")"),
        VALUE_CASE("adjust-date-to-timezone(xs:date('2002-03-07-07:00'), "
                   "xs:dayTimeDuration('-PT10H'))",
                   "xs:date(\"2002-03-06-10:00\")"),
        VALUE_CASE("adjust-date-to-timezone(xs:date('2002-03-07-07:00'), ())",
                   "xs:date(\"2002-03-07\")"),
        /* Section 9.7.3 examples. */
        VALUE_CASE("adjust-time-to-timezone(xs:time('10:00:00-07:00'), "
                   "xs:dayTimeDuration('PT10H'))",
                   "xs:time(\"03:00:00+10:00\")"),
        VALUE_CASE("adjust-time-to-timezone(xs:time('10:00:00'), xs:dayTimeDuration('-PT10H'))",
                   "xs:time(\"10:00:00-10:00\")"),
        VALUE_CASE("adjust-time-to-timezone(xs:time('01:30:00.25+01:00'), "
                   "xs:dayTimeDuration('-PT1H'))",
                   "xs:time(\"23:30:00.25-01:00\")"),
        ERROR_CASE("adjust-time-to-timezone(xs:time('10:00:00'), xs:dayTimeDuration('PT15H'))",
                   "FODT0003"),
        ERROR_CASE("adjust-date-to-timezone(xs:date('2002-03-07'), "
                   "xs:dayTimeDuration('PT1H0M30S'))",
                   "FODT0003"),
        /* D97: no timezone database. */
        VALUE_CASE("civil-timezone(xs:dateTime('2024-01-01T12:00:00'))",
                   "xs:dayTimeDuration(\"PT2H\")"),
        VALUE_CASE("civil-timezone(current-dateTime(), 'UTC')", "xs:dayTimeDuration(\"PT0S\")"),
        ERROR_CASE("civil-timezone(current-dateTime(), 'Europe/Paris')", "FODT0004"),
        /* The clock is read once per evaluation (F&O 4.0, section 1.7). */
        VALUE_CASE("current-dateTime() eq adjust-dateTime-to-timezone(current-dateTime(), "
                   "implicit-timezone())",
                   "true()"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ------------------------------------------------------------ Components */

static void test_components_of_gregorian_values(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* F&O 4.0, section 9.6.1 examples (PR 1481). */
        VALUE_CASE("year-from-dateTime(xs:gYearMonth('2007-05Z'))", "2007"),
        VALUE_CASE("year-from-dateTime(xs:time('12:30:00'))", ""),
        VALUE_CASE("month-from-dateTime(xs:gMonthDay('--12-25')), "
                   "day-from-dateTime(xs:gDay('---08')), hours-from-dateTime(xs:date('2001-01-01'))",
                   "12 8"),
        VALUE_CASE("timezone-from-dateTime(xs:gYear('2007-05:00'))",
                   "xs:dayTimeDuration(\"-PT5H\")"),
        /* Section 9.6.11 examples. */
        VALUE_CASE("days-in-month(xs:date('2024-02-10')), days-in-month(xs:date('1900-02-10')), "
                   "days-in-month(xs:dateTime('2000-02-29T12:00:00Z')), "
                   "days-in-month(xs:gYearMonth('2026-04')), days-in-month(())",
                   "29 28 29 30"),
        VALUE_CASE("days-in-month(xs:gYearMonth('0000-02'))", "29"),
        ERROR_CASE("days-in-month(xs:gYear('2024'))", "XPTY0004"),
        /* Section 9.6.17 examples. */
        VALUE_CASE("parts-of-dateTime(xs:dateTime('1999-05-31T13:20:00-05:00'))",
                   "{\"year\":1999,\"month\":5,\"day\":31,\"hours\":13,\"minutes\":20,"
                   "\"seconds\":0,\"timezone\":xs:dayTimeDuration(\"-PT5H\")}"),
        VALUE_CASE("parts-of-dateTime(xs:time('13:30:04.2678'))",
                   "{\"year\":(),\"month\":(),\"day\":(),\"hours\":13,\"minutes\":30,"
                   "\"seconds\":4.2678,\"timezone\":()}"),
        VALUE_CASE("parts-of-dateTime(xs:gYearMonth('2007-05Z'))?*",
                   "2007 5 xs:dayTimeDuration(\"PT0S\")"),
        VALUE_CASE("parts-of-dateTime(())", ""),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ------------------------------------------------------- Building values */

static void test_building_date_times(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* F&O 4.0, section 9.4.2 examples. */
        VALUE_CASE("build-dateTime({'year': 1999, 'month': 5, 'day': 31, 'hours': 13, "
                   "'minutes': 20, 'seconds': 0, 'timezone': xs:dayTimeDuration('-PT5H')}) "
                   "instance of xs:dateTimeStamp",
                   "true()"),
        VALUE_CASE("build-dateTime({'hours': 13, 'minutes': 30, 'seconds': 4.2678})",
                   "xs:time(\"13:30:04.2678\")"),
        VALUE_CASE("build-dateTime({'day': (), 'hours': 13, 'minutes': 30, 'seconds': 4.2678})",
                   "xs:time(\"13:30:04.2678\")"),
        VALUE_CASE("build-dateTime({'year': 2007, 'month': 5, "
                   "'timezone': xs:dayTimeDuration('PT0S')})",
                   "xs:gYearMonth(\"2007-05Z\")"),
        VALUE_CASE("build-dateTime({'month': 2, 'day': 29}), build-dateTime({'day': 31.0}), "
                   "build-dateTime({'year': -1}), build-dateTime({'month': 10})",
                   "xs:gMonthDay(\"--02-29\") xs:gDay(\"---31\") xs:gYear(\"-0001\") "
                   "xs:gMonth(\"--10\")"),
        /* fn-build-dateTime-time-negative-zero, -timezone-coercion-from-
           duration-valid, -time-high-precision. */
        VALUE_CASE("build-dateTime({'hours': 23, 'minutes': 0.0, 'seconds': -0.0e0})",
                   "xs:time(\"23:00:00\")"),
        VALUE_CASE("build-dateTime({'year': 0, 'month': 1, 'day': 1, "
                   "'timezone': xs:duration('PT0S')})",
                   "xs:date(\"0000-01-01Z\")"),
        VALUE_CASE("build-dateTime({'hours': 23, 'minutes': 59, "
                   "'seconds': 59.999999999999999999999})",
                   "xs:time(\"23:59:59.999999999\")"),
        VALUE_CASE("build-dateTime(parts-of-dateTime(xs:dateTime('2024-02-29T23:59:59.5+14:00')))",
                   "xs:dateTimeStamp(\"2024-02-29T23:59:59.5+14:00\")"),
        VALUE_CASE("build-dateTime({'year': xs:untypedAtomic('2024'), 'month': 2})",
                   "xs:gYearMonth(\"2024-02\")"),
        VALUE_CASE("build-dateTime(())", ""),
        ERROR_CASE("build-dateTime({})", "FODT0005"),
        ERROR_CASE("build-dateTime({'year': 1970, 'day': 1})", "FODT0005"),
        ERROR_CASE("build-dateTime({'hours': 12, 'minutes': 30})", "FODT0005"),
        ERROR_CASE("build-dateTime({'year': 1912, 'month': 2, 'day': 30})", "FORG0001"),
        ERROR_CASE("build-dateTime({'hours': 24, 'minutes': 0, 'seconds': 0})", "FORG0001"),
        ERROR_CASE("build-dateTime({'hours': 12, 'minutes': 30, 'seconds': 60})", "FORG0001"),
        ERROR_CASE("build-dateTime({'day': 3.1e1})", "XPTY0004"),
        ERROR_CASE("build-dateTime({'hours': 8.5, 'minutes': 0, 'seconds': 0})", "XPTY0004"),
        ERROR_CASE("build-dateTime({'year': 2000, 'era': 'AD'})", "XPTY0004"),
        ERROR_CASE("build-dateTime({'year': (1, 2)})", "XPTY0004"),
        ERROR_CASE("build-dateTime({'year': 1999, 'timezone': xs:duration('-P1DT5H')})",
                   "FODT0003"),
        ERROR_CASE("build-dateTime({'year': 1999, 'timezone': xs:yearMonthDuration('P1M')})",
                   "XPTY0004"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ----------------------------------------------- Seconds and Unix time */

static void test_seconds_and_unix_time(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* F&O 4.0, section 8.4.1 examples. */
        VALUE_CASE("seconds(1), seconds(0.001), seconds(60), seconds(86400), seconds(-5400)",
                   "xs:dayTimeDuration(\"PT1S\") xs:dayTimeDuration(\"PT0.001S\") "
                   "xs:dayTimeDuration(\"PT1M\") xs:dayTimeDuration(\"P1D\") "
                   "xs:dayTimeDuration(\"-PT1H30M\")"),
        VALUE_CASE("xs:dateTime('1970-01-01T00:00:00Z') + 1706702400 * seconds(1)",
                   "xs:dateTime(\"2024-01-31T12:00:00Z\")"),
        VALUE_CASE("seconds(3.14159265358979323846) eq "
                   "xs:dayTimeDuration('PT3.14159265358979323846S')",
                   "true()"),
        VALUE_CASE("seconds(())", ""),
        ERROR_CASE("seconds(99999999999999999999)", "FODT0002"),
        /* Section 8.4.2 examples. */
        VALUE_CASE("duration-to-seconds(xs:dayTimeDuration('PT1M30S')), "
                   "duration-to-seconds(xs:dayTimeDuration('P1DT0.5S')), "
                   "duration-to-seconds(xs:dayTimeDuration('-PT1H30M'))",
                   "90 86400.5 -5400"),
        VALUE_CASE("duration-to-seconds(seconds(-0.000000001)), "
                   "duration-to-seconds(xs:dayTimeDuration('PT0S')) instance of xs:decimal",
                   "-0.000000001 true()"),
        /* Section 9.4.3 examples. */
        VALUE_CASE("unix-dateTime(), unix-dateTime(1), unix-dateTime(86400000)",
                   "xs:dateTimeStamp(\"1970-01-01T00:00:00Z\") "
                   "xs:dateTimeStamp(\"1970-01-01T00:00:00.001Z\") "
                   "xs:dateTimeStamp(\"1970-01-02T00:00:00Z\")"),
        VALUE_CASE("unix-dateTime(())", "xs:dateTimeStamp(\"1970-01-01T00:00:00Z\")"),
        VALUE_CASE("unix-dateTime(9223372036854775807)",
                   "xs:dateTimeStamp(\"292278994-08-17T07:12:55.807Z\")"),
        ERROR_CASE("unix-dateTime(99999999999999999999999)", "FODT0001"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* --------------------------------------------------------- IETF dates */

static void test_parsing_ietf_dates(sxml_test_state *state) {
    static const expression_case cases[] = {
        /* F&O 4.0, section 9.10.1 examples. */
        VALUE_CASE("parse-ietf-date('Wed, 06 Jun 1994 07:29:35 GMT')",
                   "xs:dateTime(\"1994-06-06T07:29:35Z\")"),
        VALUE_CASE("parse-ietf-date('Wed, 6 Jun 94 07:29:35 GMT')",
                   "xs:dateTime(\"1994-06-06T07:29:35Z\")"),
        VALUE_CASE("parse-ietf-date('Wed Jun 06 11:54:45 EST 2013')",
                   "xs:dateTime(\"2013-06-06T11:54:45-05:00\")"),
        VALUE_CASE("parse-ietf-date('Sunday, 06-Nov-94 08:49:37 GMT')",
                   "xs:dateTime(\"1994-11-06T08:49:37Z\")"),
        VALUE_CASE("parse-ietf-date('Wed, 6 Jun 94 07:29:35 +0500')",
                   "xs:dateTime(\"1994-06-06T07:29:35+05:00\")"),
        /* QT4 parse-ietf-date-40 to -47 and their neighbours. */
        VALUE_CASE("parse-ietf-date(' wed, 20 aug 2014 19:36:01.25 -05:00 ( est ) ')",
                   "xs:dateTime(\"2014-08-20T19:36:01.25-05:00\")"),
        VALUE_CASE("parse-ietf-date('Feb-02 02:02-02: 02')",
                   "xs:dateTime(\"1902-02-02T02:02:00-02:00\")"),
        VALUE_CASE("parse-ietf-date('Aug 20 4:36:01 -500 2014')",
                   "xs:dateTime(\"2014-08-20T04:36:01-05:00\")"),
        VALUE_CASE("parse-ietf-date('Dec 31 24:00 1999')", "xs:dateTime(\"2000-01-01T00:00:00Z\")"),
        VALUE_CASE("parse-ietf-date(())", ""),
        ERROR_CASE("parse-ietf-date('2014-08-20T19:36:01Z')", "FORG0010"),
        ERROR_CASE("parse-ietf-date('Sat, 29 Feb 2014 19:36:01 GMT')", "FORG0010"),
        ERROR_CASE("parse-ietf-date('Wed,20 Aug 2014 19:36:01')", "FORG0010"),
        ERROR_CASE("parse-ietf-date('Aug 20 19:36:01 -05:0 2014')", "FORG0010"),
        ERROR_CASE("parse-ietf-date('Aug 20 19:36:01 -05:00 (CET) 2014')", "FORG0010"),
        ERROR_CASE("parse-ietf-date('Aug 20 19:36:01 -15:00 2014')", "FORG0010"),
        ERROR_CASE("parse-ietf-date('Wed, 20 Aug 114 19:36:01 GMT')", "FORG0010"),
        ERROR_CASE("parse-ietf-date('')", "FORG0010"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ---------------------------------------------- Allocation failure loops */

static const char failure_expression[] =
    "parts-of-dateTime(adjust-dateTime-to-timezone(xs:dateTime('2002-03-07T10:00:00-07:00'))) "
    "=> build-dateTime(), parse-ietf-date('Wed, 6 Jun 94 07:29:35 +0500'), "
    "unix-dateTime(1), duration-to-seconds(seconds(1.5)), days-in-month(current-date())";

static const char failure_output[] =
    "xs:dateTimeStamp(\"2002-03-07T19:00:00+02:00\") "
    "xs:dateTime(\"1994-06-06T07:29:35+05:00\") "
    "xs:dateTimeStamp(\"1970-01-01T00:00:00.001Z\") 1.5 31";

/* Runs the mix with allocation number failing_number failing; returns the
   status and checks the output and that nothing leaked. */
static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(failing_number, &test_fixture);
    if (status == SXML_STATUS_OK) {
        query_run run = {.expression_text = sxml_byte_view_from_c_string(failure_expression)};
        status = run_query(&test_fixture, &run);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(
                state, sxml_byte_view_equals(
                           sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                           sxml_byte_view_from_c_string(failure_output)));
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_functions_survive_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 100000; failing_number += 1) {
        status = run_with_failure(state, failing_number);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case function_library_temporal_test_cases[] = {
    {.name = "adjusting_timezones", .function = test_adjusting_timezones},
    {.name = "components_of_gregorian_values", .function = test_components_of_gregorian_values},
    {.name = "building_date_times", .function = test_building_date_times},
    {.name = "seconds_and_unix_time", .function = test_seconds_and_unix_time},
    {.name = "parsing_ietf_dates", .function = test_parsing_ietf_dates},
    {.name = "functions_survive_every_allocation_failure",
     .function = test_functions_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "function_library_temporal",
            .cases = function_library_temporal_test_cases,
            .case_count = SXML_TEST_COUNT(function_library_temporal_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
