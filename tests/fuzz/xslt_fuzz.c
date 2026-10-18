/*
 * Fuzz target: XSLT 4.0 through include/sxml/xslt.h (the stylesheet
 * reader, patterns and value templates, the XSLT lowering parts, the
 * evaluator's template rules and result documents, and serialization;
 * packet 4.18).
 *
 * The input is the text of a principal stylesheet module. It is compiled
 * with sxml_xslt_stylesheet_create and, when it compiles, transformed
 * twice: by applying templates to a small document (the global context
 * item), serializing the principal result into a bounded output buffer
 * and collecting result documents as trees; then by calling
 * xsl:initial-template without a context item, delivering the raw result.
 * The processor has small limits (item heap bytes, expression and
 * evaluation depth, evaluation steps, D89), so long or large
 * transformations stop instead of timing out. Its resource resolver is a
 * table with one stylesheet module (m.xsl, for xsl:include and
 * xsl:import) and one document (d.xml, for fn:doc). Every allocation goes
 * through the fixture's counting allocator, so a leak of the stylesheet,
 * a transformation or the processor stops the fuzzer.
 *
 * Golden file: tests/fuzz/xquery_fuzz.c and tests/xslt_test.c
 * (run_transformation).
 */
#include "fuzz_fixture.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xml.h"
#include "sxml/xslt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SXML_FUZZ_MAXIMUM_OUTPUT_BYTE_COUNT ((size_t)1 << 20U)
#define SXML_FUZZ_MAXIMUM_TRANSFORMATION_EVALUATION_DEPTH 1000U
#define SXML_FUZZ_MAXIMUM_TRANSFORMATION_STEP_COUNT 10000000U

static const char module_text[] =
    "<xsl:stylesheet version='4.0' xmlns:xsl='http://www.w3.org/1999/XSL/Transform' "
    "xmlns:m='urn:m'><xsl:variable name='m:v' select='1'/>"
    "<xsl:function name='m:f'><xsl:param name='x'/>"
    "<xsl:sequence select='if ($x gt 0) then m:f($x - 1) + 1 else 0'/></xsl:function>"
    "<xsl:template match='hours' mode='m:m'><h><xsl:value-of select='.'/></h></xsl:template>"
    "</xsl:stylesheet>";

static const char document_text[] =
    "<works xmlns:p='urn:p'><employee id='e1' p:gender='female'>"
    "<hours>40</hours><hours>20</hours></employee><employee id='e2'>"
    "<hours>30</hours><!--c--><?pi data?></employee></works>";

static const sxml_resource_entry fuzz_resources[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:fuzz/m.xsl"),
     .content = {.bytes = (const uint8_t *)module_text,
                 .byte_count = sizeof(module_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:fuzz/d.xml"),
     .content = {.bytes = (const uint8_t *)document_text,
                 .byte_count = sizeof(document_text) - 1}},
};

static const sxml_resource_entry_view fuzz_resource_view = {
    .resource_entries = fuzz_resources,
    .resource_entry_count = sizeof(fuzz_resources) / sizeof(fuzz_resources[0])};

/* What one transformation of the target needs. */
typedef struct fuzz_transformation {
    sxml_xslt_stylesheet *stylesheet;
    sxml_output_buffer *output_buffer;
    sxml_xslt_result_document_collector *collector;
    sxml_item document;
} fuzz_transformation;

/* Applies templates to the document, serialized, with result documents
   collected. */
static void apply_templates(const fuzz_transformation *transformation) {
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_dynamic_context_configuration dynamic_context;
    memset(&dynamic_context, 0, sizeof(dynamic_context));
    dynamic_context.has_context_value = true;
    dynamic_context.context_value = sxml_sequence_from_item(transformation->document);
    sxml_sequence result = sxml_sequence_empty();
    const sxml_status status = sxml_xslt_stylesheet_transform(
        transformation->stylesheet,
        &(sxml_xslt_transformation_request){
            .dynamic_context = dynamic_context,
            .base_output_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/out/main.xml"),
            .output_stream = sxml_output_buffer_as_output_stream(transformation->output_buffer),
            .result_document_handler =
                sxml_xslt_result_document_collector_as_handler(transformation->collector),
            .error_record = &error_record,
            .invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES,
            .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED},
        &result);
    sxml_fuzz_check_status(status, "sxml_xslt_stylesheet_transform (apply templates)");
    if (status == SXML_STATUS_OK) {
        sxml_sequence_release(result);
    }
    sxml_xslt_result_document_collector_clear(transformation->collector);
}

/* Calls xsl:initial-template without a context item, the raw result
   delivered and result documents discarded. */
static void call_initial_template(const fuzz_transformation *transformation) {
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_dynamic_context_configuration dynamic_context;
    memset(&dynamic_context, 0, sizeof(dynamic_context));
    sxml_sequence result = sxml_sequence_empty();
    const sxml_status status = sxml_xslt_stylesheet_transform(
        transformation->stylesheet,
        &(sxml_xslt_transformation_request){
            .dynamic_context = dynamic_context,
            .result_document_handler = {.state = NULL, .operations = NULL},
            .error_record = &error_record,
            .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
            .result_delivery = SXML_XSLT_RESULT_DELIVERY_RAW},
        &result);
    sxml_fuzz_check_status(status, "sxml_xslt_stylesheet_transform (call template)");
    if (status == SXML_STATUS_OK) {
        sxml_sequence_release(result);
    }
}

/* Compiles text on processor and runs both transformations. */
static void run_stylesheet(const sxml_fuzz_fixture *fixture, sxml_processor *processor,
                           sxml_byte_view text) {
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(fixture);
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    fuzz_transformation transformation = {.stylesheet = NULL,
                                          .output_buffer = NULL,
                                          .collector = NULL,
                                          .document = {.representation = 0}};
    sxml_status status = sxml_xslt_stylesheet_create(
        allocator,
        &(sxml_xslt_stylesheet_configuration){
            .processor = processor,
            .principal_module_text = text,
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("fuzz.xsl"),
            .static_context = {.static_base_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/main.xsl")},
            .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
            .error_record = &error_record},
        &transformation.stylesheet);
    sxml_fuzz_check_status(status, "sxml_xslt_stylesheet_create");
    if (status != SXML_STATUS_OK) {
        return;
    }
    status = sxml_output_buffer_create(
        allocator,
        &(sxml_output_buffer_configuration){
            .allocator = allocator, .maximum_byte_count = SXML_FUZZ_MAXIMUM_OUTPUT_BYTE_COUNT},
        &transformation.output_buffer);
    sxml_fuzz_check_status(status, "sxml_output_buffer_create");
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_result_document_collector_create(
            allocator,
            &(sxml_xslt_result_document_collector_configuration){
                .allocator = allocator,
                .maximum_byte_count = SXML_FUZZ_MAXIMUM_OUTPUT_BYTE_COUNT,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_TREE},
            &transformation.collector);
        sxml_fuzz_check_status(status, "sxml_xslt_result_document_collector_create");
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_document(
            processor,
            &(sxml_xml_parse_request){
                .input = {.bytes = (const uint8_t *)document_text,
                          .byte_count = sizeof(document_text) - 1},
                .source_name = SXML_BYTE_VIEW_FROM_LITERAL("source.xml"),
                .document_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/source.xml"),
                .base_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/source.xml"),
                .encoding_name = {.bytes = NULL, .byte_count = 0},
                .diagnostic = NULL},
            &transformation.document);
        sxml_fuzz_check_status(status, "sxml_processor_parse_document");
    }
    if (status == SXML_STATUS_OK) {
        apply_templates(&transformation);
        call_initial_template(&transformation);
        sxml_item_release(transformation.document);
    }
    sxml_xslt_result_document_collector_destroy(transformation.collector);
    sxml_output_buffer_destroy(transformation.output_buffer);
    sxml_xslt_stylesheet_destroy(transformation.stylesheet);
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
                       .maximum_evaluation_depth =
                           SXML_FUZZ_MAXIMUM_TRANSFORMATION_EVALUATION_DEPTH,
                       .maximum_evaluation_step_count =
                           SXML_FUZZ_MAXIMUM_TRANSFORMATION_STEP_COUNT},
            .resource_resolver = sxml_table_resource_resolver(&fuzz_resource_view)},
        &processor);
    sxml_fuzz_check_status(status, "sxml_processor_create");
    if (status == SXML_STATUS_OK) {
        run_stylesheet(&fixture, processor, sxml_fuzz_input_view(data, size));
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
