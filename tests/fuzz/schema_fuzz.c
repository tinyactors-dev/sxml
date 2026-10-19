/*
 * Fuzz target: XSD 1.1 schemas through include/sxml/schema.h (the schema
 * document reader and assembly, the compile stages of simple and complex
 * types, content models, identity constraints, assertions and type
 * alternatives, and the validator with its value checks; packet 5.14).
 *
 * The input is a schema document, a zero byte and an instance document
 * (without a zero byte, the instance is a small fixed document). The
 * schema is compiled with sxml_schema_create from text; when it compiles,
 * the instance is validated twice: strictly while it is parsed (the
 * validator as the parser's tree builder, the null result builder, no
 * report sink), then laxly by sxml_schema_validate_node on the parsed
 * document, which builds the validated copy. Its resource resolver serves
 * nothing, so includes and imports are documents the resolver does not
 * deliver (XSD 1.1 Part 1, 4.2.3 and 4.2.6.2). The processor has small
 * limits (item heap bytes, expression and evaluation depth, evaluation
 * steps for assertions and identity constraints, D89). Every allocation
 * goes through the fixture's counting allocator, so a leak of the schema,
 * a validation or the processor stops the fuzzer.
 *
 * Golden file: tests/fuzz/xslt_fuzz.c and tests/schema_validate_test.c
 * (validate_with_failure).
 */
#include "fuzz_fixture.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SXML_FUZZ_MAXIMUM_VALIDATION_EVALUATION_DEPTH 1000U
#define SXML_FUZZ_MAXIMUM_VALIDATION_STEP_COUNT 10000000U

static const char default_instance_text[] = "<a xmlns:p='urn:p' p:b='1'>text<c/></a>";

/* The two parts of the input. */
typedef struct fuzz_input {
    sxml_byte_view schema_text;
    sxml_byte_view instance_text;
} fuzz_input;

static fuzz_input split_input(sxml_byte_view input) {
    const uint8_t *separator =
        input.byte_count == 0 ? NULL : memchr(input.bytes, 0, input.byte_count);
    if (separator == NULL) {
        return (fuzz_input){.schema_text = input,
                            .instance_text = {.bytes = (const uint8_t *)default_instance_text,
                                              .byte_count = sizeof(default_instance_text) - 1}};
    }
    const size_t schema_byte_count = (size_t)(separator - input.bytes);
    return (fuzz_input){
        .schema_text = {.bytes = input.bytes, .byte_count = schema_byte_count},
        .instance_text = {.bytes = separator + 1,
                          .byte_count = input.byte_count - schema_byte_count - 1}};
}

/* What both validations of one input use. */
typedef struct fuzz_validation {
    const sxml_fuzz_fixture *fixture;
    sxml_processor *processor;
    const sxml_schema *schema;
    sxml_byte_view instance_text;
} fuzz_validation;

/* Validates the instance strictly while parsing it. */
static void validate_while_parsing(const fuzz_validation *validation) {
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(validation->fixture);
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_schema_validator *validator = NULL;
    sxml_status status = sxml_schema_validator_create(
        allocator,
        &(sxml_schema_validator_configuration){.schema = validation->schema,
                                               .validation_mode = SXML_VALIDATION_MODE_STRICT,
                                               .governing_type = {.index = SXML_TYPE_NONE},
                                               .result_builder = {.state = NULL, .operations = NULL},
                                               .report_sink = {.state = NULL, .operations = NULL},
                                               .error_record = &error_record},
        &validator);
    sxml_fuzz_check_status(status, "sxml_schema_validator_create");
    sxml_xml_parser *parser = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_create(
            allocator,
            &(sxml_xml_parser_configuration){.allocator = allocator,
                                             .name_pool = sxml_processor_get_name_pool(
                                                 validation->processor)},
            &parser);
        sxml_fuzz_check_status(status, "sxml_xml_parser_create");
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(
            parser,
            &(sxml_xml_parse_request){.input = validation->instance_text,
                                      .source_name = SXML_BYTE_VIEW_FROM_LITERAL("fuzz.xml"),
                                      .document_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/fuzz.xml"),
                                      .base_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/fuzz.xml"),
                                      .encoding_name = {.bytes = NULL, .byte_count = 0},
                                      .diagnostic = NULL},
            sxml_schema_validator_as_tree_builder(validator));
        sxml_fuzz_check_status(status, "sxml_xml_parser_parse (validating)");
    }
    if (status == SXML_STATUS_OK) {
        sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0};
        sxml_fuzz_check_status(sxml_schema_validator_get_outcome(validator, &outcome),
                               "sxml_schema_validator_get_outcome");
    }
    sxml_xml_parser_destroy(parser);
    sxml_schema_validator_destroy(validator);
}

/* Validates the parsed instance laxly into a validated copy. */
static void validate_parsed_node(const fuzz_validation *validation) {
    sxml_item document = {.representation = 0};
    sxml_status status = sxml_processor_parse_document(
        validation->processor,
        &(sxml_xml_parse_request){.input = validation->instance_text,
                                  .source_name = SXML_BYTE_VIEW_FROM_LITERAL("fuzz.xml"),
                                  .document_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/fuzz.xml"),
                                  .base_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/fuzz.xml"),
                                  .encoding_name = {.bytes = NULL, .byte_count = 0},
                                  .diagnostic = NULL},
        &document);
    sxml_fuzz_check_status(status, "sxml_processor_parse_document");
    if (status != SXML_STATUS_OK) {
        return;
    }
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_item validated = {.representation = 0};
    status = sxml_schema_validate_node(
        validation->schema,
        &(sxml_schema_validation_request){.node = document,
                                          .validation_mode = SXML_VALIDATION_MODE_LAX,
                                          .governing_type = {.index = SXML_TYPE_NONE},
                                          .report_sink = {.state = NULL, .operations = NULL},
                                          .error_record = &error_record},
        &validated);
    sxml_fuzz_check_status(status, "sxml_schema_validate_node");
    if (status == SXML_STATUS_OK) {
        sxml_item_release(validated);
    }
    sxml_item_release(document);
}

/* Compiles the schema on processor and validates the instance with it. */
static void run_schema(const sxml_fuzz_fixture *fixture, sxml_processor *processor,
                       fuzz_input input) {
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_schema *schema = NULL;
    const sxml_status status = sxml_schema_create(
        sxml_fuzz_fixture_allocator(fixture),
        &(sxml_schema_configuration){
            .processor = processor,
            .schema_documents = {.schema_documents =
                                     &(sxml_schema_document){
                                         .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                         .document_text = input.schema_text,
                                         .document_uri =
                                             SXML_BYTE_VIEW_FROM_LITERAL("urn:fuzz/fuzz.xsd")},
                                 .schema_document_count = 1},
            .error_record = &error_record},
        &schema);
    sxml_fuzz_check_status(status, "sxml_schema_create");
    if (status != SXML_STATUS_OK) {
        return;
    }
    const fuzz_validation validation = {.fixture = fixture,
                                        .processor = processor,
                                        .schema = schema,
                                        .instance_text = input.instance_text};
    validate_while_parsing(&validation);
    validate_parsed_node(&validation);
    sxml_schema_destroy(schema);
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
                       .maximum_evaluation_depth = SXML_FUZZ_MAXIMUM_VALIDATION_EVALUATION_DEPTH,
                       .maximum_evaluation_step_count = SXML_FUZZ_MAXIMUM_VALIDATION_STEP_COUNT}},
        &processor);
    sxml_fuzz_check_status(status, "sxml_processor_create");
    if (status == SXML_STATUS_OK) {
        run_schema(&fixture, processor, split_input(sxml_fuzz_input_view(data, size)));
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
