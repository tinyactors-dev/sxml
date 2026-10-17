/*
 * Tests for the element map part of the function library
 * (src/function_library_element_map.c, packet 2.10, D129):
 * fn:element-to-map-plan, fn:element-to-map and fn:map-to-element through
 * the public API, values written by the adaptive method (maps compared
 * with fn:deep-equal, elements written as XML), errors checked by code.
 * Each test checks that no cell or allocation leaked, and one runs a mix
 * of the functions under every allocation failure.
 *
 * Golden file: tests/memory_test.c (fixture, lifecycle, allocation-failure
 * loops), with the running of tests/xpath_test.c. Cases follow the F&O 4.0
 * examples and QT4 test cases (fn-element-to-map, fn-map-to-element).
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

/* ------------------------------------------------------- Element to map */

static void test_layouts(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("element-to-map(())", ""),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a/>')), {'a': ''})", "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<foo>bar</foo>')/*), {'foo': 'bar'})",
                   "true()"),
        /* Simple content takes the inferred type (F&O 4.0, 14.6.3). */
        VALUE_CASE("element-to-map(parse-xml('<a>12</a>')/*)?a instance of xs:integer", "true()"),
        VALUE_CASE("element-to-map(parse-xml('<a>012</a>')/*)?a instance of xs:untypedAtomic",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a id=\"zz\">babel</a>')/a), "
                   "{'a': {'@id': 'zz', '#content': 'babel'}})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a><b>x</b><b>y</b></a>')/a), "
                   "{'a': ['x', 'y']})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a id=\"1\"><b>x</b><b>y</b></a>')/a), "
                   "{'a': {'@id': '1', 'b': ['x', 'y']}})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a> <p/> <q/> </a>')/a), "
                   "{'a': {'p': '', 'q': ''}})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a><b/><c/><b/></a>')/a), "
                   "{'a': [{'b': ''}, {'c': ''}, {'b': ''}]})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a>x<!--c--><b/>y<?p d?></a>')/a), "
                   "{'a': ['x', {'#comment': 'c'}, {'b': ''}, 'y', "
                   "{'#processing-instruction': {'#target': 'p', '#data': 'd'}}]})",
                   "true()"),
        VALUE_CASE("element-to-map(parse-xml('<p>lorem</p>')/*, {'plan': {'p': {'layout': 'xml'}}})?p",
                   "\"<p>lorem</p>\""),
        /* Names (F&O 4.0, 14.6.8). */
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<z:a xmlns:z=\"http://a/\"><z:b/><c/></z:a>')/*), "
                   "{'Q{http://a/}a': {'b': '', 'Q{}c': ''}})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<z:a xmlns:z=\"http://a/\"/>')/*, "
                   "{'name-format': 'lexical'}), {'z:a': ''})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a_x0020_b/>')/*, {'escape-names': true()}), "
                   "{'a b': ''})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a id=\"zz\">babel</a>')/a, "
                   "{'content-key': '@id'}), {'a': {'@id': 'zz', '#@id': 'babel'}})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a x=\"1\"><x/></a>')/a, "
                   "{'attribute-marker': ''}), {'a': {'@x': '1', 'x': ''}})",
                   "true()"),
        ERROR_CASE("element-to-map(parse-xml('<a_x0000_/>')/*, {'escape-names': true()})",
                   "FOCH0001"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

static void test_plans(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("deep-equal(element-to-map-plan(parse-xml('<a id=\"42\"><b>12</b><b>13</b></a>')), "
                   "{'a': {'layout': 'list-plus', 'child': 'b', 'type': ()}, "
                   "'b': {'layout': 'simple', 'child': (), 'type': 'integer'}, "
                   "'@id': {'type': 'integer'}})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map-plan((parse-xml('<a><b/><b/></a>'), "
                   "parse-xml('<a><b/><c/></a>'))), "
                   "{'a': {'layout': 'sequence', 'child': (), 'type': ()}, "
                   "'b': {'layout': 'empty', 'child': (), 'type': ()}, "
                   "'c': {'layout': 'empty', 'child': (), 'type': ()}})",
                   "true()"),
        VALUE_CASE("let $in := parse-xml('<a><b><i>1</i></b><b><i>2</i><i>3</i></b></a>') "
                   "return deep-equal(element-to-map($in/a, {'plan': element-to-map-plan($in)}), "
                   "{'a': [[1], [2, 3]]})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a><b/><c/><d/></a>')/a, "
                   "{'plan': {'a': {'layout': 'record'}, 'd': {'layout': 'deep-skip'}}}), "
                   "{'a': {'b': '', 'c': ''}})",
                   "true()"),
        VALUE_CASE("deep-equal(element-to-map(parse-xml('<a>The <i>short</i> one</a>')/a, "
                   "{'plan': {'a': {'layout': 'simple'}, '*': {'layout': 'mixed'}}}), "
                   "{'a': ['The ', {'i': ['short']}, ' one']})",
                   "true()"),
        VALUE_CASE("element-to-map(parse-xml('<a>x</a>')/a, {'plan': {'a': {'layout': 'simple', "
                   "'type': 'integer'}}, 'liberal': true()})?a instance of xs:untypedAtomic",
                   "true()"),
        ERROR_CASE("element-to-map(parse-xml('<a>x</a>')/a, {'plan': {'a': {'layout': 'simple', "
                   "'type': 'integer'}}})",
                   "FOJS0010"),
        ERROR_CASE("element-to-map(parse-xml('<a><b/></a>')/a, {'plan': {'a': {'layout': 'empty'}}})",
                   "FOJS0008"),
        ERROR_CASE("element-to-map(parse-xml('<a/>')/a, {'plan': {'a': {'layout': 'list'}}})",
                   "XPTY0004"),
        ERROR_CASE("element-to-map(parse-xml('<a/>')/a, {'plan': {'a': {'layout': 'xml', 'x': 1}}})",
                   "XPTY0004"),
        ERROR_CASE("element-to-map(parse-xml('<a/>')/a, {'name-format': 'bad'})", "XPTY0004"),
        ERROR_CASE("element-to-map(parse-xml('<a/>')/a, {'colour': 'red'})", "XPTY0004"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ------------------------------------------------------- Map to element */

static void test_maps_to_elements(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("map-to-element(())", ""),
        VALUE_CASE("map-to-element({'foo': 'bar'})", "<foo>bar</foo>"),
        VALUE_CASE("map-to-element({'a': 1e3})", "<a>1000</a>"),
        VALUE_CASE("map-to-element({'price': {'@currency': 'USD', '#content': 12.16}})",
                   "<price currency=\"USD\">12.16</price>"),
        VALUE_CASE("map-to-element({'name': {'first': 'Jane', 'middle': ['E', 'M']}})",
                   "<name><first>Jane</first><middle>E</middle><middle>M</middle></name>"),
        VALUE_CASE("map-to-element({'para': [{'@id': 'x'}, 'A ', {'i': 'fine'}, ' mess!']})",
                   "<para id=\"x\">A <i>fine</i> mess!</para>"),
        VALUE_CASE("map-to-element({'a': [{'#comment': 'c'}, {'#processing-instruction': "
                   "{'#target': 'p', '#data': 'd'}}]})",
                   "<a><!--c--><?p d?></a>"),
        VALUE_CASE("map-to-element({'years': ['2023', '2024']}, "
                   "{'plan': {'years': {'layout': 'list', 'child': 'year'}}})",
                   "<years><year>2023</year><year>2024</year></years>"),
        VALUE_CASE("map-to-element({'a': '<b x=\"1\">t</b>'}, {'plan': {'a': {'layout': 'xml'}}})",
                   "<b x=\"1\">t</b>"),
        VALUE_CASE("map-to-element({'first name': 'Jane'}, {'escape-names': true()})",
                   "<first_x0020_name>Jane</first_x0020_name>"),
        VALUE_CASE("count(map-to-element(fold-left(1 to 100, {'a': 'x'}, "
                   "fn($m, $i) { {'a': $m} }))//*)",
                   "100"),
        VALUE_CASE("let $e := parse-xml('<b xmlns=\"http://u/\">t<c>x</c><c xmlns=\"\">y</c></b>')/* "
                   "return deep-equal(map-to-element(element-to-map($e)), $e)",
                   "true()"),
        VALUE_CASE("deep-equal(map-to-element({'a': {'@Q{http://u/}x': '1'}}), "
                   "parse-xml('<a xmlns:p=\"http://u/\" p:x=\"1\"/>')/*)",
                   "true()"),
        VALUE_CASE("deep-equal(map-to-element({'a': xs:QName('fn:null')}), parse-xml("
                   "'<a xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:nil=\"true\"/>')/*)",
                   "true()"),
        ERROR_CASE("map-to-element({'a': ['x', 'y']})", "FOJS0009"),
        ERROR_CASE("map-to-element({'a': 1, 'b': 2})", "FOJS0009"),
        ERROR_CASE("map-to-element({'a b': 'x'})", "FOJS0009"),
        ERROR_CASE("map-to-element({'a': {'@x': '1', '@Q{}x': '2'}})", "FOJS0009"),
        ERROR_CASE("map-to-element({'a': [{'#comment': 'a--b'}]})", "FOJS0009"),
        ERROR_CASE("map-to-element({'a': '<b/><c/>'}, {'plan': {'a': {'layout': 'xml'}}})",
                   "FOJS0009"),
        ERROR_CASE("map-to-element({'a': 'x'}, {'plan': {'a': {'layout': 'x'}}})", "XPTY0004"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ------------------------------------------------- Allocation failures */

static const char failure_expression[] =
    "let $e := parse-xml('<a x=\"1\"><b>2</b><b>3</b><c>t<i/></c></a>')/a "
    "let $o := {'plan': element-to-map-plan($e)} "
    "return (map-to-element(element-to-map($e, $o), $o), "
    "map-to-element({'r': [{'@y': 'z'}, 'q', {'s': {'@t': 'u', '#content': 1}}]}))";

static const char failure_output[] =
    "<a x=\"1\"><b>2</b><b>3</b><c>t<i/></c></a> <r y=\"z\">q<s t=\"u\">1</s></r>";
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

static const sxml_test_case function_library_element_map_test_cases[] = {
    {.name = "layouts", .function = test_layouts},
    {.name = "plans", .function = test_plans},
    {.name = "maps_to_elements", .function = test_maps_to_elements},
    {.name = "functions_survive_every_allocation_failure",
     .function = test_functions_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "function_library_element_map",
            .cases = function_library_element_map_test_cases,
            .case_count = SXML_TEST_COUNT(function_library_element_map_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
