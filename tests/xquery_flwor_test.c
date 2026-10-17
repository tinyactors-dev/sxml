/*
 * Tests for group by and window clauses (src/xquery_lower_window.c,
 * src/xquery_flwor.c, src/xquery_flwor_internal.h; D55, D126, packet 3.8).
 *
 * Golden file: tests/xquery_lower_construct_test.c (fixture, golden IR
 * text, values written by the adaptive method, static error codes,
 * allocation-failure loops). Every case checks that the counting
 * allocator got every block back.
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
        if (bytes.byte_count > 0) {
            memcpy(outcome->text, bytes.bytes, bytes.byte_count);
        }
        outcome->text[bytes.byte_count] = '\0';
    }
    sxml_output_buffer_destroy(output_buffer);
    return status;
}

/* Compiles and evaluates query; outcome->text is the value written, or
   "error:CODE" for an error with a code. */
static void evaluate_query(fixture *test_fixture, const char *query, query_outcome *outcome) {
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
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit,
                                       .has_context_value = false,
                                       .context_value = sxml_sequence_empty(),
                                       .global_values = {.slots = NULL, .slot_count = 0},
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
    {"for $x in (1, 2) let $c := 7 group by $k := $x mod 2 collation 'http://www.w3.org/2005/xpath-functions/collation/codepoint' return ($k, $c)",
     "(function 1 main (parameters 0) (captures) (slots 12)\n"
     "  (let $1 (sequence 1 2)\n"
     "    (let $5 (map $1 (focus $2 _ _)\n"
     "              (let $3 (binary modulo $2 2)\n"
     "                (let $4 (coerce $3 xs:anyAtomicType? variable)\n"
     "                  (array-constructor square $2 7 $4))))\n"
     "      (let $6 (sequence 3)\n"
     "        (let $7 (sequence \"http://www.w3.org/2005/xpath-functions/collation/codepoint\")\n"
     "          (let $8 (static-call Q{}group-by#3 $5 $6 $7)\n"
     "            (map $8 (focus $9 _ _)\n"
     "              (let $10 (lookup $9 1)\n"
     "                (let $11 (lookup $9 2)\n"
     "                  (let $12 (lookup $9 3)\n"
     "                    (sequence $12 $11)))))))))))\n"},
    {"for $n in (2, 3) for tumbling window $w in 1 to 6 start $s at $p when $s mod $n = 1 only end $e when $e - $s eq . return count($w)",
     "(function 1 main (parameters 0) (captures) (slots 12) (focus $5 _ _)\n"
     "  (let $1 (sequence 2 3)\n"
     "    (map $1 (focus $2 _ _)\n"
     "      (let $3 (binary range 1 6)\n"
     "        (let $4 (function-definition 2)\n"
     "          (let $6 (function-definition 3)\n"
     "            (let $7 (static-call Q{}window#4 $3 $4 $6 6)\n"
     "              (map $7 (focus $8 _ _)\n"
     "                (let $9 (lookup $8 1)\n"
     "                  (let $10 (lookup $8 2)\n"
     "                    (let $11 (lookup $8 3)\n"
     "                      (let $12 (lookup $8 6)\n"
     "                        (static-call fn:count#1 $9)))))))))))))\n"
     "(function 2 anonymous (parameters 4) (captures $2) (slots 9)\n"
     "  (let $9 (binary modulo $1 $5)\n"
     "    (binary general-equal $9 1)))\n"
     "(function 3 anonymous (parameters 8) (captures $5) (slots 13)\n"
     "  (let $13 $9\n"
     "    (let $12 (binary subtract $5 $1)\n"
     "      (binary value-equal $12 $13))))\n"},
    {"for sliding window $w in (1, 2) start when true() end next $x when empty($x) return $w",
     "(function 1 main (parameters 0) (captures) (slots 7)\n"
     "  (let $1 (sequence 1 2)\n"
     "    (let $2 (function-definition 2)\n"
     "      (let $3 (function-definition 3)\n"
     "        (let $4 (static-call Q{}window#4 $1 $2 $3 3)\n"
     "          (map $4 (focus $5 _ _)\n"
     "            (let $6 (lookup $5 1)\n"
     "              (let $7 (lookup $5 9)\n"
     "                $6))))))))\n"
     "(function 2 anonymous (parameters 4) (captures) (slots 7)\n"
     "  (static-call fn:true#0))\n"
     "(function 3 anonymous (parameters 8) (captures) (slots 11)\n"
     "  (static-call fn:empty#1 $8))\n"},
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
    const char *query;
    /* The value written by the adaptive method, or "error:CODE". */
    const char *expected;
} value_case;

static const value_case value_cases[] = {
    /* 4.13.8: groups in the order of their first tuples; non-grouping
       variables become sequences, constants too. */
    {"for $x in (1, 2, 3, 4, 5) let $c := 7 group by $k := $x mod 2 "
     "return $k || ':' || string-join($x, ',') || ':' || count($c)",
     "\"1:1,3,5:3\" \"0:2,4:2\""},
    {"for $x in (1, 2, 3, 4) let $k := $x mod 2 group by $k order by $k return sum($x)",
     "6 4"},
    /* Keys are atomized; untyped atomics equal strings; empty keys form
       a group; NaN equals NaN; 1 and 1.0 are one key. */
    {"for $e in (<a>x</a>, 'x', <a>y</a>) group by $k := $e return count($e)", "2 1"},
    {"for $x in ([1], [], [2], []) group by $k := $x?* return count($x)", "1 2 1"},
    {"for $x in (xs:double('NaN'), 1, 1.0, xs:double('NaN')) group by $k := $x return count($x)",
     "2 2"},
    /* Two keys, a collation. */
    {"for $p in ('a1', 'b2', 'A1', 'a2') group by $l := substring($p, 1, 1) "
     "collation 'http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive', "
     "$n := substring($p, 2) return string-join($p, '+')",
     "\"a1+A1\" \"b2\" \"a2\""},
    {"for $x in 1 to 3 group by $x return $x", "1 2 3"},
    {"for $x in () group by $x return $x", ""},
    /* A grouping key of two items is XPTY0004. */
    {"for $x in 1 to 2 group by $k := (1, 2) return $k", "error:XPTY0004"},
    {"for $x in 1 to 2 group by $k as xs:string := $x return $k", "error:XPTY0004"},
    /* 4.13.4.1: tumbling windows. */
    {"for tumbling window $w in (2, 4, 6, 8, 10, 12, 14) start at $s only end at $e "
     "when $e - $s eq 2 return string-join($w, ' ')",
     "\"2 4 6\" \"8 10 12\""},
    {"for tumbling window $w in (2, 4, 6, 8, 10, 12, 14) start at $s end at $e "
     "when $e - $s eq 2 return string-join($w, ' ')",
     "\"2 4 6\" \"8 10 12\" \"14\""},
    {"for tumbling window $w in (2, 4, 6, 8, 10, 12, 14) start at $s when $s mod 3 = 1 "
     "return string-join($w, ' ')",
     "\"2 4 6\" \"8 10 12\" \"14\""},
    {"for tumbling window $w in (2, 4, 6, 8, 10, 12, 14) end $last when $last mod 3 = 0 "
     "return string-join($w, ' ')",
     "\"2 4 6\" \"8 10 12\" \"14\""},
    {"for tumbling window $w in (2, 4, 6, 8, 10, 12, 14) start $first at $s "
     "only end $last at $e when $e - $s eq 2 return [$first, $last]",
     "[2,6] [8,12]"},
    /* 4.13.4.2: sliding windows. */
    {"for sliding window $w in (2, 4, 6, 8, 10, 12, 14) start at $s only end at $e "
     "when $e - $s eq 2 return avg($w)",
     "4 6 8 10 12"},
    {"for sliding window $w in (2, 4, 6, 8, 10, 12, 14) start at $s end at $e "
     "when $e - $s eq 2 return count($w)",
     "3 3 3 3 3 2 1"},
    /* All nine variables; previous and next are empty at the ends. */
    {"for tumbling window $w in (1, 2, 3) start $s at $sp previous $spr next $sn when true() "
     "end $e at $ep previous $epr next $en when true() "
     "return [$w, $s, $sp, $spr, $sn, $e, $ep, $epr, $en]",
     "[1,1,1,(),2,1,1,(),2] [2,2,2,1,3,2,2,1,3] [3,3,3,2,(),3,3,2,()]"},
    /* Conditions read variables of earlier clauses and the focus. */
    {"for $n in (2, 3) for tumbling window $w in 1 to 6 start $s when $s mod $n = 1 "
     "return count($w)",
     "2 2 2 3 3"},
    {"<a n='2'/>/(for tumbling window $w in 1 to 4 start $s when $s mod @n = 1 return sum($w))",
     "3 7"},
    /* Window variables after the clause, with count and order by. */
    {"for tumbling window $w as xs:integer+ in 1 to 5 start $s when $s mod 2 = 1 count $c "
     "order by $s descending return $c",
     "3 2 1"},
    {"for tumbling window $w as xs:string+ in 1 to 2 start when true() return $w",
     "error:XPTY0004"},
    {"for tumbling window $w in (1, 2) start $s when ($s, $s) return $w", "error:FORG0006"},
    {"for sliding window $w in () start when true() end when true() return $w", ""},
};

static void test_queries_give_values(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(value_cases); case_index += 1) {
        static fixture test_fixture;
        static query_outcome outcome;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        const value_case *test_case = &value_cases[case_index];
        evaluate_query(&test_fixture, test_case->query, &outcome);
        const bool matches =
            (outcome.status == SXML_STATUS_OK || sxml_status_has_error_record(outcome.status)) &&
            strcmp(outcome.text, test_case->expected) == 0;
        if (!matches) {
            (void)fprintf(stderr, "query %s\nstatus %d, got %s\nexpected %s\n", test_case->query,
                          (int)outcome.status, outcome.text, test_case->expected);
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
    {"let $y := 1 return for $x in 1 to 3 group by $y return $y", "XQST0094"},
    {"for $x in 1 to 3 group by $x collation 'urn:unknown' return $x", "XQST0076"},
    {"for tumbling window $w in 1 to 3 start $w when true() return $w", "XQST0103"},
    {"for sliding window $w in 1 to 3 start $s when true() end $s when true() return $w",
     "XQST0103"},
    {"for tumbling window $w in 1 to 3 start $s when $w return $s", "XPST0008"},
    {"for tumbling window $w in 1 to 3 start $s when true() return $e", "XPST0008"},
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

/* Each value case again, failing each allocation in turn until one run
   succeeds: every failure is OUT_OF_MEMORY (or an error the evaluator
   raises for it) and releases everything. */
static void test_queries_survive_every_allocation_failure(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(value_cases); case_index += 1) {
        const value_case *test_case = &value_cases[case_index];
        uint64_t failing_allocation_number = 1;
        for (;;) {
            static fixture test_fixture;
            static query_outcome outcome;
            SXML_TEST_REQUIRE(state, fixture_create(failing_allocation_number, &test_fixture) ==
                                         SXML_STATUS_OK);
            evaluate_query(&test_fixture, test_case->query, &outcome);
            SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
            fixture_destroy(&test_fixture);
            if (strcmp(outcome.text, test_case->expected) == 0) {
                break;
            }
            const bool is_out_of_memory = outcome.status == SXML_STATUS_OUT_OF_MEMORY;
            SXML_TEST_EXPECT(state, is_out_of_memory);
            if (!is_out_of_memory) {
                (void)fprintf(stderr, "query %s allocation %llu: status %d %s\n", test_case->query,
                              (unsigned long long)failing_allocation_number, (int)outcome.status,
                              outcome.text);
                break;
            }
            failing_allocation_number += 1;
            SXML_TEST_REQUIRE(state, failing_allocation_number < 20000);
        }
    }
}

static const sxml_test_case xquery_flwor_test_cases[] = {
    {.name = "queries_match_golden_text", .function = test_queries_match_golden_text},
    {.name = "queries_give_values", .function = test_queries_give_values},
    {.name = "static_errors_have_codes", .function = test_static_errors_have_codes},
    {.name = "queries_survive_every_allocation_failure",
     .function = test_queries_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery_flwor",
            .cases = xquery_flwor_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_flwor_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
