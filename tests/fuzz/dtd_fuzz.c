/*
 * Fuzz target: DTD processing (packets 7.14a and 7.14b, D199): the
 * external subset, external parameter and general entities, conditional
 * sections, parameter-entity references inside declarations, and DTD
 * validation through include/sxml/schema.h's DTD validator.
 *
 * The input is a document, a zero byte and the text of every external
 * entity (without a zero byte, the entities are the document itself):
 * the entity resolver serves that text for whatever it is asked, so one
 * input reaches the external subset, external parameter entities and
 * external general entities at once, recursion included. The document is
 * parsed twice with the resolver: without validation into nothing, and
 * with the DTD validator, whose null report sink stops at the first
 * error. Entity expansion and the processor's item heap are bounded. Every
 * allocation goes through the fixture's counting allocator, so a leak of
 * the parser, the entities, the schema or the validator stops the fuzzer.
 *
 * Golden file: tests/fuzz/schema_fuzz.c (the processor, the split input,
 * the leak checks).
 */
#include "fuzz_fixture.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bytes entity replacement may produce in one document. */
#define SXML_FUZZ_MAXIMUM_ENTITY_EXPANSION_BYTE_COUNT ((size_t)1 << 18U)

/* The two parts of the input. */
typedef struct fuzz_input {
    sxml_byte_view document_text;
    sxml_byte_view entity_text;
} fuzz_input;

static fuzz_input split_input(sxml_byte_view input) {
    const uint8_t *separator =
        input.byte_count == 0 ? NULL : memchr(input.bytes, 0, input.byte_count);
    if (separator == NULL) {
        return (fuzz_input){.document_text = input, .entity_text = input};
    }
    const size_t document_byte_count = (size_t)(separator - input.bytes);
    return (fuzz_input){
        .document_text = {.bytes = input.bytes, .byte_count = document_byte_count},
        .entity_text = {.bytes = separator + 1,
                        .byte_count = input.byte_count - document_byte_count - 1}};
}

/* Serves the entity text for every request. */
static sxml_status resolve_entity(void *state, const sxml_xml_external_entity_request *request,
                                  sxml_xml_external_entity *entity) {
    (void)request;
    const fuzz_input *input = state;
    entity->is_available = true;
    entity->content = input->entity_text;
    return SXML_STATUS_OK;
}

static const sxml_xml_entity_resolver_operations entity_resolver_operations = {
    .resolve = resolve_entity,
    .release = NULL,
};

/* Parses the document with the resolver, validating when validator is
   not NULL. */
typedef struct fuzz_parse {
    sxml_xml_parser *parser;
    const fuzz_input *input;
    sxml_dtd_validator *validator;
} fuzz_parse;

static void parse(const fuzz_parse *run) {
    sxml_diagnostic diagnostic = {.description = {.byte_count = 0}};
    const sxml_status status = sxml_xml_parser_check(
        run->parser,
        &(sxml_xml_parse_request){
            .input = run->input->document_text,
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("fuzz.xml"),
            .base_uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///fuzz.xml"),
            .diagnostic = &diagnostic,
            .entity_resolver = {.state = (void *)run->input,
                                .operations = &entity_resolver_operations},
            .validator = run->validator == NULL
                             ? (sxml_xml_validator){.state = NULL, .operations = NULL}
                             : sxml_dtd_validator_as_xml_validator(run->validator)});
    if (status != SXML_STATUS_DYNAMIC_ERROR) {
        sxml_fuzz_check_status(status, "sxml_xml_parser_check");
    }
    if (status == SXML_STATUS_OK && run->validator != NULL) {
        sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_VALID, .error_count = 0};
        sxml_fuzz_check_status(sxml_dtd_validator_get_outcome(run->validator, &outcome),
                               "sxml_dtd_validator_get_outcome");
    }
}

static void run_input(const sxml_fuzz_fixture *fixture, sxml_processor *processor,
                      const fuzz_input *input) {
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(fixture);
    sxml_xml_parser *parser = NULL;
    sxml_status status = sxml_xml_parser_create(
        allocator,
        &(sxml_xml_parser_configuration){
            .allocator = allocator,
            .name_pool = sxml_processor_get_name_pool(processor),
            .limits = {.maximum_depth = 0,
                       .maximum_entity_expansion_byte_count =
                           SXML_FUZZ_MAXIMUM_ENTITY_EXPANSION_BYTE_COUNT}},
        &parser);
    sxml_fuzz_check_status(status, "sxml_xml_parser_create");
    if (status != SXML_STATUS_OK) {
        return;
    }
    parse(&(fuzz_parse){.parser = parser, .input = input, .validator = NULL});
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof error_record);
    sxml_dtd_validator *validator = NULL;
    status = sxml_dtd_validator_create(
        allocator,
        &(sxml_dtd_validator_configuration){.processor = processor,
                                            .report_sink = {.state = NULL, .operations = NULL},
                                            .error_record = &error_record},
        &validator);
    sxml_fuzz_check_status(status, "sxml_dtd_validator_create");
    if (status == SXML_STATUS_OK) {
        parse(&(fuzz_parse){.parser = parser, .input = input, .validator = validator});
        sxml_dtd_validator_destroy(validator);
    }
    sxml_xml_parser_destroy(parser);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(&(sxml_fuzz_fixture_configuration){.has_function_library = false},
                                 &fixture) != SXML_STATUS_OK) {
        return 0;
    }
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(&fixture);
    sxml_processor *processor = NULL;
    const sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .limits = {.maximum_item_heap_byte_count = SXML_FUZZ_MAXIMUM_HEAP_BYTE_COUNT}},
        &processor);
    sxml_fuzz_check_status(status, "sxml_processor_create");
    if (status == SXML_STATUS_OK) {
        const fuzz_input input = split_input(sxml_fuzz_input_view(data, size));
        run_input(&fixture, processor, &input);
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
