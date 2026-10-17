/*
 * Tests for prologs: the static context of a main module
 * (src/xquery_static.c), lowering declared functions and variables
 * (src/xquery_prolog.c) and computing global variables
 * (src/evaluator_global.c); D101, D135, packet 3.10.
 *
 * Golden file: tests/xquery_lower_construct_test.c (fixture, compiling
 * with sxml_xquery_compile against a processor's function table, golden
 * IR text, values written by the adaptive method, allocation-failure
 * loops). Every case checks that the counting allocator got every block
 * back.
 */
#include "evaluator_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "test_runner.h"
#include "xquery_lower_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

typedef struct fixture {
    sxml_processor *processor;
    sxml_counting_allocator *counting_allocator;
    alignas(16) uint8_t counting_storage[256];
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.processor = NULL, .counting_allocator = NULL};
    sxml_status status = sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = test_fixture->counting_storage,
                           .byte_count = sizeof(test_fixture->counting_storage)},
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_create(
            sxml_default_allocator(),
            &(sxml_processor_configuration){.allocator = sxml_default_allocator()},
            &test_fixture->processor);
    }
    return status;
}

static void fixture_destroy(fixture *test_fixture) {
    sxml_processor_destroy(test_fixture->processor);
    sxml_counting_allocator_finalize(test_fixture->counting_allocator);
}

static bool fixture_released_everything(const fixture *test_fixture) {
    return sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
               .live_allocation_count == 0;
}

/* What compiling (and evaluating) one query gave. */
typedef struct query_outcome {
    sxml_status status;
    sxml_error_record error_record;
    /* The compiled unit, owned; NULL after a failure. */
    sxml_ir_unit *unit;
    char text[8192];
} query_outcome;

/* Compiles query into outcome->unit. */
static sxml_status compile_query(fixture *test_fixture, const char *query, query_outcome *outcome) {
    sxml_processor *processor = test_fixture->processor;
    const sxml_xpath_static_context static_context = {
        .item_heap = processor->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .function_table = sxml_processor_get_function_table(processor)};
    memset(&outcome->error_record, 0, sizeof(outcome->error_record));
    outcome->text[0] = '\0';
    outcome->unit = NULL;
    return sxml_xquery_compile(
        &(sxml_xpath_compile_request){
            .expression_text = {.bytes = (const uint8_t *)query, .byte_count = strlen(query)},
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xq"),
            .static_context = &static_context,
            .error_record = &outcome->error_record},
        &outcome->unit);
}

/* Compiles query and writes the unit's IR text to outcome. */
static void write_query_text(fixture *test_fixture, const char *query, query_outcome *outcome) {
    outcome->status = compile_query(test_fixture, query, outcome);
    sxml_ir_unit *unit = outcome->unit;
    if (outcome->status != SXML_STATUS_OK) {
        return;
    }
    size_t byte_count = 0;
    outcome->status =
        sxml_ir_unit_write_text(unit,
                                (sxml_memory_view){.bytes = (uint8_t *)outcome->text,
                                                   .byte_count = sizeof(outcome->text) - 1},
                                &byte_count);
    if (outcome->status == SXML_STATUS_OK) {
        outcome->text[byte_count] = '\0';
    }
    sxml_ir_unit_release(unit);
}

/* Writes value by the adaptive method into outcome->text. */
static sxml_status serialize_value(fixture *test_fixture, sxml_sequence value,
                                   query_outcome *outcome) {
    sxml_output_buffer *output_buffer = NULL;
    sxml_status status = sxml_output_buffer_create(
        fixture_allocator(test_fixture),
        &(sxml_output_buffer_configuration){.allocator = fixture_allocator(test_fixture),
                                            .maximum_byte_count = sizeof(outcome->text) - 1},
        &output_buffer);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            test_fixture->processor->item_heap,
            &(sxml_serialization_request){
                .value = value,
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                             .has_item_separator = true,
                                             .item_separator = SXML_BYTE_VIEW_FROM_LITERAL(" ")},
                .error_record = &outcome->error_record},
            sxml_output_buffer_as_output_stream(output_buffer));
    }
    if (status == SXML_STATUS_OK) {
        const sxml_byte_view bytes = sxml_output_buffer_get_bytes(output_buffer);
        memcpy(outcome->text, bytes.bytes, bytes.byte_count);
        outcome->text[bytes.byte_count] = '\0';
    }
    sxml_output_buffer_destroy(output_buffer);
    return status;
}

/* Compiles and evaluates query; outcome->text is the value written, or
   "error:CODE" for an error with a code. */
typedef struct evaluation_input {
    const char *query;
    /* The host's context value: an xs:integer, when has_context_value. */
    bool has_context_value;
    int64_t context_integer;
} evaluation_input;

static void evaluate_query(fixture *test_fixture, evaluation_input input, query_outcome *outcome) {
    const char *query = input.query;
    outcome->status = compile_query(test_fixture, query, outcome);
    sxml_ir_unit *unit = outcome->unit;
    sxml_dynamic_context *dynamic_context = NULL;
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_dynamic_context_create(
            fixture_allocator(test_fixture),
            &(sxml_evaluator_configuration){
                .item_heap = test_fixture->processor->item_heap,
                .allocator = fixture_allocator(test_fixture),
                .function_table = sxml_processor_get_function_table(test_fixture->processor)},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    /* No host binds a variable: every global starts absent. */
    static sxml_evaluator_slot absent_globals[64];
    for (size_t slot_index = 0; slot_index < SXML_TEST_COUNT(absent_globals); slot_index += 1) {
        absent_globals[slot_index] = (sxml_evaluator_slot){
            .value = sxml_sequence_empty(), .slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT};
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit,
                                       .has_context_value = input.has_context_value,
                                       .context_value = sxml_sequence_from_item(
                                           sxml_atomic_item_from_integer(input.context_integer)),
                                       .global_values = {.slots = absent_globals,
                                                         .slot_count =
                                                             unit->global_variable_count > 1
                                                                 ? unit->global_variable_count - 1
                                                                 : 0},
                                       .static_base_uri = {.bytes = NULL, .byte_count = 0},
                                       .error_record = &outcome->error_record},
            &value);
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = serialize_value(test_fixture, value, outcome);
    }
    if (outcome->status != SXML_STATUS_OK && sxml_status_has_error_record(outcome->status)) {
        const sxml_byte_view code = outcome->error_record.code.local_name;
        (void)snprintf(outcome->text, sizeof(outcome->text), "error:%.*s", (int)code.byte_count,
                       (const char *)code.bytes);
    }
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_ir_unit_release(unit);
    sxml_item_heap_clear_raised_error(test_fixture->processor->item_heap);
}

/* ------------------------------------------------------- Golden text */

typedef struct golden_case {
    const char *query;
    const char *expected_text;
} golden_case;

static const golden_case golden_cases[] = {
    /* src/xpath_ast_internal.h, example 5: a declared function. */
    {"declare function local:twice($x as xs:integer) as xs:integer { $x * 2 }; local:twice(21)",
     "(function 1 main (parameters 0) (captures) (slots 0)\n"
     "  (static-call function:2#1 21))\n"
     "(function 2 Q{http://www.w3.org/2005/xquery-local-functions}twice (parameters 1) "
     "(captures) (slots 2)\n"
     "  (let $2 (binary multiply $1 2)\n"
     "    (coerce $2 xs:integer function-result)))\n"},
};

static void test_queries_match_golden_text(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(golden_cases); case_index += 1) {
        static fixture test_fixture;
        static query_outcome outcome;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        write_query_text(&test_fixture, golden_cases[case_index].query, &outcome);
        const bool matches = outcome.status == SXML_STATUS_OK &&
                             strcmp(outcome.text, golden_cases[case_index].expected_text) == 0;
        if (!matches) {
            (void)fprintf(
                stderr, "query %s\nstatus %d %.*s\ngot:\n%s\n", golden_cases[case_index].query,
                (int)outcome.status, (int)outcome.error_record.diagnostic.description.byte_count,
                (const char *)outcome.error_record.diagnostic.description.bytes, outcome.text);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
        fixture_destroy(&test_fixture);
    }
}

/* ---------------------------------------------------------- Values */

typedef struct value_case {
    evaluation_input input;
    /* The value written by the adaptive method, or "error:CODE". */
    const char *expected;
} value_case;

static const value_case value_cases[] = {
    /* 5.13, 5.14: namespaces. */
    {{"declare namespace p = 'urn:p'; <p:a/>", false, 0}, "<p:a xmlns:p=\"urn:p\"/>"},
    {{"declare default element namespace 'urn:d'; <a/>", false, 0}, "<a xmlns=\"urn:d\"/>"},
    {{"declare default element namespace 'urn:d'; namespace-uri(<a/>)", false, 0}, "\"urn:d\""},
    {{"declare namespace xs = 'urn:x'; xs:b instance of xs:QName", false, 0}, "error:XPST0051"},
    {{"declare default function namespace 'http://www.w3.org/2005/xpath-functions/math'; "
      "pi() gt 3", false, 0},
     "true()"},
    /* 5.3, 5.5: setters. */
    {{"declare boundary-space preserve; <a> </a>", false, 0}, "<a> </a>"},
    {{"declare option output:indent 'yes'; declare option o 'x'; 1", false, 0}, "1"},
    {{"declare default order empty greatest; for $x in (2, 1) "
      "order by (if ($x eq 1) then () else $x) return $x", false, 0},
     "2 1"},
    {{"declare default order empty greatest; for $x in (2, 1) "
      "order by (if ($x eq 1) then () else $x) empty least return $x", false, 0},
     "1 2"},
    {{"declare base-uri 'http://example.com/a/'; static-base-uri()", false, 0},
     "\"http://example.com/a/\""},
    /* 5.16: variables, also forward and through a function. */
    {{"declare variable $x := 1; $x + 1", false, 0}, "2"},
    {{"declare variable $x as xs:integer := 3; declare variable $y := $x * 2; $y", false, 0}, "6"},
    {{"declare variable $y := $x * 2; declare variable $x := 3; $y", false, 0}, "6"},
    {{"declare variable $a := local:f(); declare variable $b := 1; "
      "declare function local:f() { $b }; $a", false, 0},
     "1"},
    {{"declare variable $x := <a/>; $x is $x", false, 0}, "true()"},
    {{"declare variable $x as xs:integer := 'a'; $x", false, 0}, "error:XPTY0004"},
    {{"declare variable $unused := 1 div 0; 2", false, 0}, "2"},
    {{"declare variable $x := 5; declare variable $f := function() { $x }; $f()", false, 0}, "5"},
    {{"declare variable $e external := 7; $e", false, 0}, "7"},
    {{"declare variable $e external; $e", false, 0}, "error:XPDY0002"},
    {{"declare variable $a := $b; declare variable $b := $a; $a", false, 0}, "error:XQDY0054"},
    {{"declare variable $a := local:f(); declare function local:f() { $a }; $a", false, 0},
     "error:XQDY0054"},
    /* 5.17: the context value, of the host or declared. */
    {{"declare variable $x := .; $x", true, 4}, "4"},
    {{"declare variable $x := .; $x", false, 0}, "error:XPDY0002"},
    {{"declare variable $x := 1; . + $x", true, 4}, "5"},
    {{"declare context value := 5; . + 1", false, 0}, "6"},
    {{"declare context value := 5; . + 1", true, 4}, "6"},
    {{"declare context value external := 5; . + 1", true, 4}, "5"},
    {{"declare context value external := 5; . + 1", false, 0}, "6"},
    {{"declare context item as xs:string external; .", true, 4}, "error:XPTY0004"},
    {{"declare context value := 5; declare variable $x := . * 2; $x", false, 0}, "10"},
    /* 5.18: functions, recursion, mutual recursion, defaults. */
    {{"declare function local:f($n) { if ($n le 1) then 1 else $n * local:f($n - 1) }; "
      "local:f(10)", false, 0},
     "3628800"},
    {{"declare function local:even($n) { $n eq 0 or local:odd($n - 1) }; "
      "declare function local:odd($n) { $n ne 0 and local:even($n - 1) }; "
      "local:even(1000), local:odd(7)", false, 0},
     "true() true()"},
    {{"declare function local:count($n) { if ($n eq 0) then 0 else 1 + local:count($n - 1) }; "
      "local:count(100000)", false, 0},
     "100000"},
    {{"declare function local:f($a, $b := 10) { $a + $b }; local:f(1), local:f(1, 2)", false, 0}, "11 3"},
    {{"declare function local:f($a := .) { $a * 2 }; local:f()", true, 4}, "8"},
    {{"declare function f($x) { $x * 2 }; f(3)", false, 0}, "6"},
    {{"declare function local:f($a, $b) { $a - $b }; local:f(b := 1, a := 5)", false, 0}, "4"},
    {{"declare function local:sq($x) { $x * $x }; (1 to 3) ! local:sq#1(.)", false, 0}, "1 4 9"},
    {{"declare function local:f() as xs:integer { 'a' }; local:f()", false, 0}, "error:XPTY0004"},
    {{"declare function local:f($x as xs:integer) { $x }; local:f('a')", false, 0}, "error:XPTY0004"},
    {{"declare %private function local:f() { 1 }; local:f()", false, 0}, "1"},
    /* 5.10, 5.4: decimal formats and the default collation. */
    {{"declare decimal-format local:f grouping-separator = \"'\"; "
      "format-number(1234, \"#'##0\", 'local:f')", false, 0},
     "\"1'234\""},
    {{"declare default decimal-format NaN = 'none'; format-number(xs:double('NaN'), '0')",
      false, 0},
     "\"none\""},
    {{"declare default collation 'http://www.w3.org/2005/xpath-functions/collation/"
      "html-ascii-case-insensitive'; compare('a', 'A'), default-collation()", false, 0},
     "0 \"http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive\""},
    /* 5.19, 5.20: named item types and records. */
    {{"declare type local:i as xs:integer; 3 instance of local:i, local:i('5') + 1", false, 0},
     "true() 6"},
    {{"declare variable $v as local:s := 'a'; declare type local:s as xs:string; $v", false, 0},
     "\"a\""},
    {{"declare type local:a as local:b; declare type local:b as xs:string; 'x' instance of local:a",
      false, 0},
     "true()"},
    {{"declare record local:p(x as xs:integer, y as xs:integer := 0); "
      "local:p(3)?y, local:p(3, 4) instance of local:p, map{'x': 1} instance of local:p",
      false, 0},
     "0 true() false()"},
    {{"declare record local:p(x as xs:integer); local:p('a')", false, 0}, "error:XPTY0004"},
    {{"declare function local:f() { . }; local:f()", true, 4}, "error:XPDY0002"},
};

static void test_queries_give_values(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(value_cases); case_index += 1) {
        static fixture test_fixture;
        static query_outcome outcome;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        const value_case *test_case = &value_cases[case_index];
        evaluate_query(&test_fixture, test_case->input, &outcome);
        const bool matches =
            (outcome.status == SXML_STATUS_OK || sxml_status_has_error_record(outcome.status)) &&
            strcmp(outcome.text, test_case->expected) == 0;
        if (!matches) {
            (void)fprintf(stderr, "query %s\nstatus %d (%.*s), got %s\nexpected %s\n",
                          test_case->input.query, (int)outcome.status,
                          (int)outcome.error_record.diagnostic.description.byte_count,
                          (const char *)outcome.error_record.diagnostic.description.bytes,
                          outcome.text, test_case->expected);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
        fixture_destroy(&test_fixture);
    }
}

/* ------------------------------------------------------ Static errors */

typedef struct error_case {
    const char *query;
    const char *code;
} error_case;

static const error_case error_cases[] = {
    {"declare namespace p = 'urn:a'; declare namespace p = 'urn:b'; 1", "XQST0033"},
    {"declare namespace xml = 'urn:a'; 1", "XQST0070"},
    {"declare namespace p = 'urn:a'; declare namespace p = ''; 1", "XQST0033"},
    {"declare namespace xs = ''; xs:integer(1)", "XPST0081"},
    {"%fn:x function() { 1 }", "XQST0045"},
    {"%private function() { 1 }", "XQST0125"},
    {"declare namespace p = 'http://www.w3.org/XML/1998/namespace'; 1", "XQST0070"},
    {"declare default element namespace 'urn:a'; declare default element namespace 'urn:b'; 1",
     "XQST0066"},
    {"declare boundary-space strip; declare boundary-space preserve; 1", "XQST0068"},
    {"declare base-uri 'a'; declare base-uri 'b'; 1", "XQST0032"},
    {"declare construction strip; declare construction strip; 1", "XQST0067"},
    {"declare ordering ordered; declare ordering unordered; 1", "XQST0065"},
    {"declare default order empty least; declare default order empty greatest; 1", "XQST0069"},
    {"declare copy-namespaces preserve, inherit; declare copy-namespaces preserve, inherit; 1",
     "XQST0055"},
    {"declare default collation 'urn:no-such-collation'; 1", "XQST0038"},
    {"declare context value := 1; declare context value := 2; 1", "XQST0099"},
    {"declare variable $x := 1; declare variable $x := 2; $x", "XQST0049"},
    {"declare function local:f($a) { 1 }; declare function local:f($b) { 2 }; 1", "XQST0034"},
    {"declare function local:f($a, $b := 1) { 1 }; declare function local:f($c) { 2 }; 1",
     "XQST0034"},
    {"declare function local:f($a, $a) { 1 }; 1", "XQST0039"},
    {"declare function fn:f() { 1 }; 1", "XQST0045"},
    {"declare function local:f($a := 1, $b) { 1 }; 1", "XQST0148"},
    {"declare %private %public function local:f() { 1 }; 1", "XQST0106"},
    {"declare %private %private variable $x := 1; 1", "XQST0116"},
    {"declare %x variable $x := 1; 1", "XQST0045"},
    {"declare %updating function local:f() { 1 }; 1", "XQST0045"},
    {"import schema 'urn:s'; 1", "XQST0009"},
    {"declare option output:no-such-parameter 'yes'; 1", "XQST0109"},
    {"declare option output:use-character-maps 'm'; 1", "XQST0109"},
    {"declare option output:indent 'yes'; declare option output:indent 'no'; 1", "XQST0110"},
    {"declare option p:o 'v'; 1", "XPST0081"},
    {"declare variable $x := $y; 1", "XPST0008"},
    {"declare function local:f() { $p }; 1", "XPST0008"},
    {"declare function local:f() { local:g() }; 1", "XPST0017"},
    {"declare variable $x as p:t := 1; 1", "XPST0081"},
    {"declare type local:a as local:a; 1", "XPST0051"},
    {"declare default decimal-format zero-digit = 'A'; 1", "XQST0097"},
    {"declare default decimal-format decimal-separator = ','; 1", "XQST0098"},
    {"declare default decimal-format digit = 'x' digit = 'y'; 1", "XQST0114"},
    {"declare decimal-format f digit = 'x'; declare decimal-format f digit = 'y'; 1",
     "XQST0111"},
    {"declare type local:a as xs:integer; declare type local:a as xs:string; 1", "XQST0048"},
    {"declare record local:r(a, a); 1", "XPST0021"},
};

static void test_static_errors_have_codes(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(error_cases); case_index += 1) {
        static fixture test_fixture;
        static query_outcome outcome;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        const error_case *test_case = &error_cases[case_index];
        outcome.status = compile_query(&test_fixture, test_case->query, &outcome);
        sxml_ir_unit_release(outcome.unit);
        const sxml_byte_view code = outcome.error_record.code.local_name;
        const bool matches = outcome.status == SXML_STATUS_STATIC_ERROR &&
                             code.byte_count == strlen(test_case->code) &&
                             memcmp(code.bytes, test_case->code, code.byte_count) == 0;
        if (!matches) {
            (void)fprintf(stderr, "query %s: status %d code %.*s\n", test_case->query,
                          (int)outcome.status, (int)code.byte_count, (const char *)code.bytes);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
        fixture_destroy(&test_fixture);
    }
}

/* -------------------------------------------------- Allocation failure */

/* Value cases with a prolog of each kind, failing each allocation in turn
   until one run succeeds: every failure is OUT_OF_MEMORY and releases
   everything. */
static const value_case allocation_cases[] = {
    {{"declare namespace p = 'urn:p'; declare variable $y := $x * 2; declare variable $x := 3; "
      "declare function local:f($a, $b := 1) { $a + $b + $y }; <p:a>{local:f(1)}</p:a>", false, 0},
     "<p:a xmlns:p=\"urn:p\">8</p:a>"},
    {{"declare context value external := 5; declare variable $v := . + 1; $v", true, 4}, "5"},
    {{"declare type local:i as xs:integer; declare record local:p(x as local:i, y := 2); "
      "local:p(local:i('1'))?y", false, 0},
     "2"},
};

static void test_queries_survive_every_allocation_failure(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(allocation_cases); case_index += 1) {
        const value_case *test_case = &allocation_cases[case_index];
        uint64_t failing_allocation_number = 1;
        for (;;) {
            static fixture test_fixture;
            static query_outcome outcome;
            SXML_TEST_REQUIRE(state, fixture_create(failing_allocation_number, &test_fixture) ==
                                         SXML_STATUS_OK);
            evaluate_query(&test_fixture, test_case->input, &outcome);
            SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
            fixture_destroy(&test_fixture);
            if (strcmp(outcome.text, test_case->expected) == 0) {
                break;
            }
            const bool is_out_of_memory = outcome.status == SXML_STATUS_OUT_OF_MEMORY;
            SXML_TEST_EXPECT(state, is_out_of_memory);
            if (!is_out_of_memory) {
                (void)fprintf(stderr, "query %s allocation %llu: status %d %s\n",
                              test_case->input.query,
                              (unsigned long long)failing_allocation_number, (int)outcome.status,
                              outcome.text);
                break;
            }
            failing_allocation_number += 1;
            SXML_TEST_REQUIRE(state, failing_allocation_number < 20000);
        }
    }
}

static const sxml_test_case xquery_prolog_test_cases[] = {
    {.name = "queries_match_golden_text", .function = test_queries_match_golden_text},
    {.name = "queries_give_values", .function = test_queries_give_values},
    {.name = "static_errors_have_codes", .function = test_static_errors_have_codes},
    {.name = "queries_survive_every_allocation_failure",
     .function = test_queries_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery_prolog",
            .cases = xquery_prolog_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_prolog_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
