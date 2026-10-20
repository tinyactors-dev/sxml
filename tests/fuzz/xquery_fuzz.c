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
#include "sxml/xquery.h"

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
        sxml_fuzz_run_xquery(&fixture, processor, sxml_fuzz_input_view(data, size));
        sxml_fuzz_processor_destroy_checking_leaks(processor);
    }
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
