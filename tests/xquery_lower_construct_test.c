/*
 * Tests for the XQuery constructor lowering part
 * (src/xquery_lower_construct.c; D103, D110, D124, packet 3.9).
 *
 * Golden file: tests/xquery_lower_test.c (fixture, compiling with
 * sxml_xquery_compile against a processor's function table, golden IR
 * text, allocation-failure loops). Queries are also evaluated and their
 * value written by the adaptive method (items separated by a space), so
 * the lowering is checked against the evaluator's constructors (D110).
 * Every case checks that the counting allocator got every block back.
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

/* 4.12.1 and 4.12.3: content atoms, namespace declarations, attribute
   values, computed names. */
static const golden_case golden_cases[] = {
    {"<p:a xmlns:p=\"urn:p\" xmlns=\"urn:d\" b=\"{1 + 1}\">x<c/></p:a>",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (let $1 (binary add 1 1)\n"
     "    (let $2 (attribute-constructor #b $1)\n"
     "      (let $3 (element-constructor #Q{urn:d}c _ (namespaces p=\"urn:p\" \"\"=\"urn:d\")\n"
     "                (sequence))\n"
     "        (element-constructor #Q{urn:p}a _ (namespaces p=\"urn:p\" \"\"=\"urn:d\")\n"
     "          (sequence $2 \"x\" $3))))))\n"},
    {"<a b=\"x{1, 2}y\">{1}{2} <![CDATA[ ]]></a>",
     "(function 1 main (parameters 0) (captures) (slots 5)\n"
     "  (let $1 (sequence 1 2)\n"
     "    (let $2 (text-constructor _ $1)\n"
     "      (let $3 (binary concatenate \"x\" $2)\n"
     "        (let $4 (binary concatenate $3 \"y\")\n"
     "          (let $5 (attribute-constructor #b $4)\n"
     "            (element-constructor #a _\n"
     "              (sequence $5 1 2 \" \" \" \"))))))))\n"},
    {"element {'e'} {attribute a {1}, text {'t'}}, document {<a/>}",
     "(function 1 main (parameters 0) (captures) (slots 6)\n"
     "  (let $1 (attribute-constructor #a 1)\n"
     "    (let $2 (text-constructor _ \"t\")\n"
     "      (let $3 (sequence $1 $2)\n"
     "        (let $4 (element-constructor \"e\" _\n"
     "                  $3)\n"
     "          (let $5 (element-constructor #a _\n"
     "                    (sequence))\n"
     "            (let $6 (document-constructor _ _\n"
     "                      $5)\n"
     "              (sequence $4 $6))))))))\n"},
    {"<a xmlns:p=\"urn:p\">{element {'p:b'} {}}</a>",
     "(function 1 main (parameters 0) (captures) (slots 2)\n"
     "  (let $1 (sequence)\n"
     "    (let $2 (element-constructor \"p:b\" _ (namespaces p=\"urn:p\") (name-namespaces "
     "p=\"urn:p\" local=\"http://www.w3.org/2005/xquery-local-functions\" "
     "output=\"http://www.w3.org/2010/xslt-xquery-serialization\" "
     "xq=\"http://www.w3.org/2012/xquery\")\n"
     "              $1)\n"
     "      (element-constructor #a _ (namespaces p=\"urn:p\")\n"
     "        (sequence $2)))))\n"},
    {"namespace p {'urn:p'}, processing-instruction #t {'v'}, comment {'c'}, "
     "<!--d-->, <?t c?>",
     "(function 1 main (parameters 0) (captures) (slots 5)\n"
     "  (let $1 (namespace-constructor \"p\" \"urn:p\")\n"
     "    (let $2 (processing-instruction-constructor \"t\" \"v\")\n"
     "      (let $3 (comment-constructor _ \"c\")\n"
     "        (let $4 (comment-constructor _ \"d\")\n"
     "          (let $5 (processing-instruction-constructor \"t\" \"c\")\n"
     "            (sequence $1 $2 $3 $4 $5)))))))\n"},
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
    /* 4.12.1.3: atomic items joined by spaces within one enclosed
       expression only; boundary whitespace dropped (4.12.1.4). */
    {"<a>{1}{2} {3, 4} x{5}</a>", "<a>123 4 x5</a>"},
    {"<a> <b/> &#x20;<![CDATA[ ]]>{()} </a>", "<a><b/>   </a>"},
    /* 4.12.1.1: attribute values. */
    {"<a b=\"x{1, 2}y{()}z\" c=\"{3}\" d=\"\" e=\"&lt;{'&amp;'}\"/>",
     "<a b=\"x1 2yz\" c=\"3\" d=\"\" e=\"&lt;&amp;\"/>"},
    {"<a b=\"{<x>1</x>, 2}\"/>", "<a b=\"1 2\"/>"},
    {"string(<a b='{1}{2}'/>/@b)", "\"12\""},
    /* 4.12.1.2: namespace declaration attributes. */
    {"<p:a xmlns:p=\"urn:p\" xmlns=\"urn:d\" b=\"{1 + 1}\">x<c/></p:a>",
     "<p:a xmlns:p=\"urn:p\" xmlns=\"urn:d\" b=\"2\">x<c/></p:a>"},
    {"namespace-uri(<a xmlns=\"urn:d\"><b/></a>/*)", "\"urn:d\""},
    {"<a xmlns:p=\"urn:p\">{namespace-uri-from-QName(#p:b)}</a>", "<a xmlns:p=\"urn:p\">urn:p</a>"},
    {"<a xmlns:p=\"urn:p\">{<p:b/>}</a>", "<a xmlns:p=\"urn:p\"><p:b/></a>"},
    {"<a xmlns:p=\"urn:p\"><b xmlns:p=\"urn:q\"><p:c/></b></a>",
     "<a xmlns:p=\"urn:p\"><b xmlns:p=\"urn:q\"><p:c/></b></a>"},
    {"<a xmlns=\"urn:d\"><b xmlns=\"\"/></a>", "<a xmlns=\"urn:d\"><b xmlns=\"\"/></a>"},
    {"<a xmlns:p=\"urn:p\">{element {'p:b'} {}}</a>", "<a xmlns:p=\"urn:p\"><p:b/></a>"},
    {"in-scope-prefixes(<a xmlns:p=\"urn:p\">{element b {}}</a>/b) => sort()", "\"p\" \"xml\""},
    {"<a xmlns:p=\"urn:p\" p:b=\"1\"/>/@*:b ! namespace-uri(.)", "\"urn:p\""},
    /* 4.12.2: comments and processing instructions. */
    {"<!--c-->, <?t d ?>, <a><!--x--><?y?></a>", "<!--c--> <?t d ?> <a><!--x--><?y?></a>"},
    /* 4.12.3: computed constructors. */
    {"element e {attribute a {1, 2}, 'x', 3}", "<e a=\"1 2\">x 3</e>"},
    {"element {'e'} {}, element {xs:QName('e')} {}, element #f {}", "<e/> <e/> <f/>"},
    {"attribute {'a'} {'v'}", "a=\"v\""},
    {"document {<a/>, 'x'} instance of document-node()", "true()"},
    {"text {1, 2}, comment {'c'}, processing-instruction {'t'} {' v'}", "1 2 <!--c--> <?t v?>"},
    {"namespace p {'urn:p'}, namespace {'q'} {'urn:q'}", "xmlns:p=\"urn:p\" xmlns:q=\"urn:q\""},
    /* Errors raised by the evaluator for computed names (4.12.3.1). */
    {"element {'1'} {}", "error:XQDY0074"},
    {"element {()} {}", "error:XPTY0004"},
    {"processing-instruction {'xml'} {}", "error:XQDY0064"},
    {"<a>{attribute b {1}}</a>, <a>{'x', attribute b {1}}</a>", "error:XQTY0024"},
    {"<a b='1'>{attribute b {2}}</a>", "error:XQDY0025"},
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
    {"<a xmlns:p='urn:x' xmlns:q='urn:x' p:b='' q:b=''/>", "XQST0040"},
    {"<p:a/>", "XPST0081"},
    {"<a p:b=''/>", "XPST0081"},
    {"<a xmlns:p='urn:x'/>, <p:b/>", "XPST0081"},
    {"element p:a {}", "XPST0081"},
    {"attribute p:a {}", "XPST0081"},
    {"<a xmlns:p='urn:x'>{$p:v}</a>", "XPST0008"},
    /* A computed namespace constructor binds no prefix statically. */
    {"element a {namespace p {'urn:p'}, element p:b {}}", "XPST0081"},
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

static const sxml_test_case xquery_lower_construct_test_cases[] = {
    {.name = "queries_match_golden_text", .function = test_queries_match_golden_text},
    {.name = "queries_give_values", .function = test_queries_give_values},
    {.name = "static_errors_have_codes", .function = test_static_errors_have_codes},
    {.name = "queries_survive_every_allocation_failure",
     .function = test_queries_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery_lower_construct",
            .cases = xquery_lower_construct_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_lower_construct_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
