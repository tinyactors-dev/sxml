/*
 * Fuzz target: XQuery 4.0 through include/sxml/xquery.h (src/xquery*.c,
 * the evaluator's constructors, try/catch and globals, and serialization;
 * packet 3.15).
 *
 * The input is the text of a main module. It is compiled with
 * sxml_xquery_query_create, evaluated with a small document as context
 * value and serialized with the parameters its prolog declares into a
 * bounded output buffer. The processor has small limits (item heap
 * bytes, expression and evaluation depth, evaluation steps, D89), so
 * long or large queries stop instead of timing out. Its resource resolver
 * is a table with one library module (namespace urn:m, location m.xq) and
 * one document (d.xml), so imports, fn:doc and output:parameter-document
 * are reached too. Every allocation goes through the fixture's counting
 * allocator, so a leak of the query, the evaluation or the processor
 * stops the fuzzer.
 *
 * Golden file: tests/xquery_test.c (run_query) and
 * tests/fuzz/xpath_evaluate_fuzz.c.
 */
#include "fuzz_fixture.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xquery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SXML_FUZZ_MAXIMUM_OUTPUT_BYTE_COUNT ((size_t)1 << 20U)
#define SXML_FUZZ_MAXIMUM_QUERY_EVALUATION_DEPTH 1000U
#define SXML_FUZZ_MAXIMUM_QUERY_STEP_COUNT 10000000U

static const char module_text[] =
    "module namespace m = 'urn:m'; declare variable $m:v := <v a='1'>x</v>; "
    "declare function m:f($x) { if ($x gt 0) then m:f($x - 1) + 1 else 0 };";

static const char document_text[] =
    "<works xmlns:p='urn:p'><employee id='e1' p:gender='female'>"
    "<hours>40</hours><hours>20</hours></employee><employee id='e2'>"
    "<hours>30</hours><!--c--><?pi data?></employee></works>";

static const sxml_resource_entry fuzz_resources[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:fuzz/m.xq"),
     .content = {.bytes = (const uint8_t *)module_text,
                 .byte_count = sizeof(module_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:fuzz/d.xml"),
     .content = {.bytes = (const uint8_t *)document_text,
                 .byte_count = sizeof(document_text) - 1}},
};

static const sxml_resource_entry_view fuzz_resource_view = {
    .resource_entries = fuzz_resources,
    .resource_entry_count = sizeof(fuzz_resources) / sizeof(fuzz_resources[0])};

/* Compiles, evaluates and serializes text on processor. */
static void run_query(const sxml_fuzz_fixture *fixture, sxml_processor *processor,
                      sxml_byte_view text) {
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(fixture);
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_xquery_query *query = NULL;
    sxml_status status = sxml_xquery_query_create(
        allocator,
        &(sxml_xquery_query_configuration){
            .processor = processor,
            .expression_text = text,
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("fuzz.xq"),
            .static_context = {.static_base_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/")},
            .error_record = &error_record},
        &query);
    sxml_fuzz_check_status(status, "sxml_xquery_query_create");
    if (status != SXML_STATUS_OK) {
        return;
    }
    sxml_output_buffer *output_buffer = NULL;
    status = sxml_output_buffer_create(
        allocator,
        &(sxml_output_buffer_configuration){
            .allocator = allocator, .maximum_byte_count = SXML_FUZZ_MAXIMUM_OUTPUT_BYTE_COUNT},
        &output_buffer);
    sxml_fuzz_check_status(status, "sxml_output_buffer_create");
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        memset(&error_record, 0, sizeof(error_record));
        status = sxml_xquery_query_evaluate(
            query, &(sxml_xquery_evaluation_request){.error_record = &error_record}, &result);
        sxml_fuzz_check_status(status, "sxml_xquery_query_evaluate");
    }
    if (status == SXML_STATUS_OK) {
        memset(&error_record, 0, sizeof(error_record));
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(processor),
            &(sxml_serialization_request){
                .value = result,
                .serialization_parameters = sxml_xquery_query_get_serialization_parameters(query),
                .error_record = &error_record},
            sxml_output_buffer_as_output_stream(output_buffer));
        sxml_fuzz_check_status(status, "sxml_serialize_sequence");
        sxml_sequence_release(result);
    }
    sxml_output_buffer_destroy(output_buffer);
    sxml_xquery_query_destroy(query);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(
            &(sxml_fuzz_fixture_configuration){.has_function_library = false},
            &fixture) != SXML_STATUS_OK) {
        return 0;
    }
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(&fixture);
    sxml_processor *processor = NULL;
    const sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .limits = {.maximum_item_heap_byte_count = SXML_FUZZ_MAXIMUM_HEAP_BYTE_COUNT,
                       .maximum_expression_depth = SXML_FUZZ_MAXIMUM_EXPRESSION_DEPTH,
                       .maximum_evaluation_depth = SXML_FUZZ_MAXIMUM_QUERY_EVALUATION_DEPTH,
                       .maximum_evaluation_step_count = SXML_FUZZ_MAXIMUM_QUERY_STEP_COUNT},
            .resource_resolver = sxml_table_resource_resolver(&fuzz_resource_view)},
        &processor);
    sxml_fuzz_check_status(status, "sxml_processor_create");
    if (status == SXML_STATUS_OK) {
        run_query(&fixture, processor, sxml_fuzz_input_view(data, size));
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(processor);
        sxml_item_heap_clear_raised_error(item_heap);
        if (sxml_item_heap_read_counts(item_heap).live_cell_count != 0) {
            (void)fprintf(stderr, "leak: %llu cells in the processor's heap\n",
                          (unsigned long long)sxml_item_heap_read_counts(item_heap)
                              .live_cell_count);
            abort();
        }
        sxml_processor_destroy(processor);
    }
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
