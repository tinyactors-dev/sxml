/*
 * Tests for the URI, QName and hash functions of packet 2.11 through the
 * public API: fn:decode-from-uri, fn:parse-uri, fn:build-uri
 * (src/function_library_uri.c), fn:parse-QName, fn:resolve-QName
 * (src/function_library_qname.c) and fn:hash (src/function_library_hash.c).
 * Values are written by the adaptive method, errors checked by code. Each
 * test checks that no cell or allocation leaked, and one runs a mix of the
 * functions under every allocation failure.
 *
 * Golden file: tests/function_library_element_map_test.c (fixture,
 * running, case tables), itself after tests/memory_test.c. Cases follow
 * the F&O 4.0 examples and QT4 test sets fn-parse-uri, fn-build-uri,
 * fn-decode-from-uri, fn-parse-QName, fn-resolve-QName and fn-hash.
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

/* ---------------------------------------------------------- Decoding */

static void test_decoding(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("decode-from-uri(())", "\"\""),
        VALUE_CASE("decode-from-uri('~b%C3%A9b%C3%A9?a=b+c')", "\"~b\xC3\xA9" "b\xC3\xA9?a=b+c\""),
        /* Broken escapes and octet sequences give U+FFFD (F&O 4.0, 7.1). */
        VALUE_CASE("decode-from-uri('%F0%F0%9F%92%A1-%4X-%00+')",
                   "\"\xEF\xBF\xBD\xF0\x9F\x92\xA1-\xEF\xBF\xBD-\xEF\xBF\xBD+\""),
        VALUE_CASE("decode-from-uri('%\xC3\xBC\xC3\xBC')", "\"\xEF\xBF\xBD\xC3\xBC\""),
        VALUE_CASE("string-to-codepoints(decode-from-uri('%01%F0'))", "1 65533"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ----------------------------------------------------------- Parsing */

static void test_parsing(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("parse-uri(())", ""),
        VALUE_CASE("parse-uri('http://u:p@x.org:8080/a/b%20c?k=v+w&k=2&z#f%41')",
                   "{\"uri\":\"http://u:p@x.org:8080/a/b%20c?k=v+w&k=2&z#f%41\","
                   "\"scheme\":\"http\",\"absolute\":(),\"hierarchical\":true(),"
                   "\"authority\":\"u:p@x.org:8080\",\"userinfo\":(),\"host\":\"x.org\","
                   "\"port\":8080,\"path\":\"/a/b%20c\",\"query\":\"k=v+w&k=2&z\","
                   "\"fragment\":\"fA\",\"path-segments\":(\"\",\"a\",\"b c\"),"
                   "\"query-parameters\":{\"k\":(\"v w\",\"2\"),\"\":\"z\"},\"filepath\":()}"),
        VALUE_CASE("parse-uri('http://u:p@x.org:80/a', {'allow-deprecated-features': true(), "
                   "'omit-default-ports': true()}) ! (?userinfo, ?port, ?absolute)",
                   "\"u:p\" true()"),
        VALUE_CASE("parse-uri('c:\\dir\\a%20b.txt') ! (?scheme, ?path, ?filepath)",
                   "\"file\" \"/c:/dir/a%20b.txt\" \"c:/dir/a b.txt\""),
        VALUE_CASE("parse-uri('mailto:a@b.c') ! (?hierarchical, ?path-segments)",
                   "false() \"a@b.c\""),
        VALUE_CASE("parse-uri('file:////unc/share/f', {'unc-path': true()})?filepath",
                   "\"//unc/share/f\""),
        VALUE_CASE("parse-uri('ldap://[2001:db8::7]/c=GB?objectClass?one') ! (?host, "
                   "?query-parameters?(''))",
                   "\"[2001:db8::7]\" \"objectClass?one\""),
        VALUE_CASE("parse-uri('////somehost') ! (?authority, ?path)", "\"//somehost\""),
        VALUE_CASE("parse-uri('?q=1') ! (?hierarchical, ?path, ?query)", "\"q=1\""),
        VALUE_CASE("parse-uri('x') instance of fn:uri-structure-record", "true()"),
        VALUE_CASE("count(map:keys(parse-uri('#f')))", "14"),
        ERROR_CASE("parse-uri('http://[::1/')", "FOUR0001"),
        ERROR_CASE("parse-uri('x', {'colour': 1})", "XPTY0004"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ---------------------------------------------------------- Building */

static void test_building(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("build-uri(parse-uri('http://u@x.org:8080/a/b%20c?k=v+w&k=2&z#f%41'))",
                   "\"http://u@x.org:8080/a/b%20c?k=v%20w&k=2&z#fA\""),
        VALUE_CASE("build-uri({'scheme': 'https', 'host': 'qt4cg.org', 'port': (), "
                   "'path': '/specifications/index.html'})",
                   "\"https://qt4cg.org/specifications/index.html\""),
        VALUE_CASE("build-uri({'scheme': 'mailto', 'path-segments': 'a b@c'})",
                   "\"mailto:a b@c\""),
        VALUE_CASE("build-uri({'scheme': 'file', 'path': '/srv/x'}, {'unc-path': true()})",
                   "\"file://///srv/x\""),
        VALUE_CASE("build-uri({'scheme': 'http', 'host': 'h', 'port': 80, 'userinfo': 'u:p'}, "
                   "{'omit-default-ports': true()})",
                   "\"http://h\""),
        VALUE_CASE("build-uri({'scheme': 'q', 'hierarchical': false(), 'path': 'abc'})",
                   "\"q:abc\""),
        VALUE_CASE("build-uri({'path-segments': ('', 'a/b', 'c?'), "
                   "'query-parameters': {'k=': ('1+', '2'), '': 'v'}, 'fragment': 'f #'})",
                   "\"/a%2Fb/c%3F?k%3D=1%2B&k%3D=2&v#f%20%23\""),
        /* Without a scheme an authority still reads back (D-record of 2.11). */
        VALUE_CASE("build-uri({'host': 'x', 'path': '/p'})", "\"//x/p\""),
        VALUE_CASE("parse-uri('a+b/c') => build-uri()", "\"a+b/c\""),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ------------------------------------------------------------ QNames */

static void test_qnames(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("parse-QName(())", ""),
        VALUE_CASE("expanded-QName(parse-QName(' Q{http://e/}p:l '))", "\"Q{http://e/}l\""),
        VALUE_CASE("prefix-from-QName(parse-QName('Q{http://e/}p:l'))", "\"p\""),
        VALUE_CASE("prefix-from-QName(parse-QName('xs:integer'))", "\"xs\""),
        VALUE_CASE("parse-QName('fn:abs') eq QName('http://www.w3.org/2005/xpath-functions', "
                   "'abs')",
                   "true()"),
        VALUE_CASE("expanded-QName(parse-QName('plain'))", "\"Q{}plain\""),
        ERROR_CASE("parse-QName('Q{}p:l')", "FOCA0002"),
        ERROR_CASE("parse-QName('Q{a{b}c')", "FOCA0002"),
        ERROR_CASE("parse-QName('a:b:c')", "FOCA0002"),
        ERROR_CASE("parse-QName('nope:l')", "FONS0004"),
        VALUE_CASE("expanded-QName(resolve-QName('e:x', parse-xml('<a xmlns:e=\"http://e/\"/>')/*))",
                   "\"Q{http://e/}x\""),
        VALUE_CASE("expanded-QName(resolve-QName('x', parse-xml('<a xmlns=\"http://d/\"/>')/*))",
                   "\"Q{http://d/}x\""),
        VALUE_CASE("prefix-from-QName(resolve-QName('e:x', parse-xml('<a xmlns:e=\"u\"/>')/*))",
                   "\"e\""),
        VALUE_CASE("resolve-QName((), parse-xml('<a/>')/*)", ""),
        ERROR_CASE("resolve-QName('q:x', parse-xml('<a/>')/*)", "FONS0004"),
        ERROR_CASE("resolve-QName('xs:x', parse-xml('<a/>')/*)", "FONS0004"),
        ERROR_CASE("resolve-QName('1x', parse-xml('<a/>')/*)", "FOCA0002"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ------------------------------------------------------------- Hashes */

static void test_hashes(sxml_test_state *state) {
    static const expression_case cases[] = {
        VALUE_CASE("hash(())", ""),
        VALUE_CASE("string(hash('abc'))", "\"900150983CD24FB0D6963F7D28E17F72\""),
        VALUE_CASE("string(hash('abc', ()))", "\"900150983CD24FB0D6963F7D28E17F72\""),
        VALUE_CASE("string(hash('abc', ' sha-256 '))",
                   "\"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD\""),
        VALUE_CASE("string(hash(xs:hexBinary('CAFEBABE'), 'CRC-32'))", "\"B51D571D\""),
        VALUE_CASE("string(hash(xs:base64Binary('yv66vg=='), 'crc-32'))", "\"B51D571D\""),
        VALUE_CASE("hash('ABC', 'BLAKE3') => xs:base64Binary() => string()",
                   "\"0XFydFl88CiWlPddltREuZKglvGv2Oe7+m67HTYP7fw=\""),
        VALUE_CASE("hash(parse-xml('<doc>abc</doc>')) instance of xs:hexBinary", "true()"),
        ERROR_CASE("hash('abc', 'SHA-512')", "FOHA0001"),
        ERROR_CASE("hash('abc', 'MD5', {'salt': 1})", "XPTY0004"),
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* ------------------------------------------------- Allocation failures */

static const char failure_expression[] =
    "(parse-uri('http://u@h:81/p/a%20b?x=1&y=2&x=3#f') => build-uri(), "
    "decode-from-uri('%C3%A9%F0'), "
    "expanded-QName(parse-QName('Q{u}l')), "
    "expanded-QName(resolve-QName('e:x', parse-xml('<a xmlns:e=\"v\"/>')/*)), "
    "string(hash('abc', 'SHA-1')))";

static const char failure_output[] =
    "\"http://u@h:81/p/a%20b?x=1&x=3&y=2#f\" \"\xC3\xA9\xEF\xBF\xBD\" \"Q{u}l\" \"Q{v}x\" "
    "\"A9993E364706816ABA3E25717850C26C9CD0D89D\"";
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

static const sxml_test_case function_library_uri_test_cases[] = {
    {.name = "decoding", .function = test_decoding},
    {.name = "parsing", .function = test_parsing},
    {.name = "building", .function = test_building},
    {.name = "qnames", .function = test_qnames},
    {.name = "hashes", .function = test_hashes},
    {.name = "functions_survive_every_allocation_failure",
     .function = test_functions_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "function_library_uri",
            .cases = function_library_uri_test_cases,
            .case_count = SXML_TEST_COUNT(function_library_uri_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
