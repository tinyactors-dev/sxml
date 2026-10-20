/*
 * The shared fixture of the fuzz targets (tests/fuzz/fuzz_fixture.h).
 *
 * Golden file: the fixture of tests/evaluator_test.c.
 */
#include "fuzz_fixture.h"
#include "function_library_parts_internal.h"
#include "sxml/serialize.h"
#include "sxml/xquery.h"
#include "xpath_static_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bound on the serialized result of sxml_fuzz_run_xquery. */
#define SXML_FUZZ_MAXIMUM_OUTPUT_BYTE_COUNT ((size_t)1 << 20U)


sxml_status
sxml_fuzz_fixture_create(const sxml_fuzz_fixture_configuration *configuration,
                         sxml_fuzz_fixture *fixture) {
    *fixture = (sxml_fuzz_fixture){
        .counting_allocator = NULL,
        .function_library = NULL,
        .function_table = {.state = NULL, .operations = NULL}};
    sxml_status status = sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = fixture->counting_storage,
                           .byte_count = sizeof(fixture->counting_storage)},
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = 0},
        &fixture->counting_allocator);
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(fixture);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_create(
            allocator,
            &(sxml_name_pool_configuration){.allocator = allocator,
                                            .maximum_byte_count = 0},
            &fixture->name_pool);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_create(
            allocator,
            &(sxml_type_registry_configuration){
                .allocator = allocator,
                .name_pool = fixture->name_pool,
                .maximum_registered_type_count = 0},
            &fixture->type_registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            allocator,
            &(sxml_item_heap_configuration){
                .allocator = allocator,
                .name_pool = fixture->name_pool,
                .type_registry = fixture->type_registry,
                .maximum_byte_count = SXML_FUZZ_MAXIMUM_HEAP_BYTE_COUNT},
            &fixture->item_heap);
    }
    if (status == SXML_STATUS_OK && configuration->has_function_library) {
        status = sxml_function_library_create(
            allocator,
            &(sxml_function_library_configuration){
                .allocator = allocator,
                .item_heap = fixture->item_heap,
                /* Every part serving XPath, as the processor takes them
                   (D101). */
                .parts = *sxml_function_library_xpath_parts()},
            &fixture->function_library);
    }
    if (status == SXML_STATUS_OK && configuration->has_function_library) {
        fixture->function_table =
            sxml_function_library_as_table(fixture->function_library);
    }
    if (status != SXML_STATUS_OK) {
        sxml_function_library_destroy(fixture->function_library);
        sxml_item_heap_destroy(fixture->item_heap);
        sxml_type_registry_destroy(fixture->type_registry);
        sxml_name_pool_destroy(fixture->name_pool);
        sxml_counting_allocator_finalize(fixture->counting_allocator);
        /* No input is involved yet: the fixture itself is broken. */
        sxml_fuzz_check_status(SXML_STATUS_INVALID_ARGUMENT,
                               "sxml_fuzz_fixture_create");
    }
    return status;
}

sxml_allocator sxml_fuzz_fixture_allocator(const sxml_fuzz_fixture *fixture) {
    return sxml_counting_allocator_as_allocator(fixture->counting_allocator);
}

void sxml_fuzz_fixture_destroy_checking_leaks(sxml_fuzz_fixture *fixture) {
    sxml_item_heap_clear_raised_error(fixture->item_heap);
    const uint64_t live_cell_count =
        sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
    sxml_function_library_destroy(fixture->function_library);
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->type_registry);
    sxml_name_pool_destroy(fixture->name_pool);
    const uint64_t live_allocation_count =
        sxml_counting_allocator_read_counts(fixture->counting_allocator)
            .live_allocation_count;
    sxml_counting_allocator_finalize(fixture->counting_allocator);
    if (live_cell_count != 0 || live_allocation_count != 0) {
        (void)fprintf(stderr, "leak: %llu cells, %llu blocks\n",
                      (unsigned long long)live_cell_count,
                      (unsigned long long)live_allocation_count);
        abort();
    }
}

void sxml_fuzz_check_status(sxml_status status, const char *operation_name) {
    switch (status) {
    case SXML_STATUS_INVALID_ARGUMENT:
    case SXML_STATUS_INSUFFICIENT_MEMORY:
    case SXML_STATUS_MISALIGNED_MEMORY:
    case SXML_STATUS_INDEX_OUT_OF_RANGE: {
        const sxml_byte_view status_name = sxml_status_name(status);
        (void)fprintf(stderr, "%s: %.*s\n", operation_name,
                      (int)status_name.byte_count,
                      (const char *)status_name.bytes);
        abort();
    }
    case SXML_STATUS_OK:
    case SXML_STATUS_CAPACITY_EXCEEDED:
    case SXML_STATUS_LIMIT_EXCEEDED:
    case SXML_STATUS_OUT_OF_MEMORY:
    case SXML_STATUS_ARITHMETIC_OVERFLOW:
    case SXML_STATUS_UNEXPECTED_END_OF_INPUT:
    case SXML_STATUS_MALFORMED_INPUT:
    case SXML_STATUS_INVALID_UTF8:
    case SXML_STATUS_UNSUPPORTED_FEATURE:
    case SXML_STATUS_INPUT_OUTPUT_FAILED:
    case SXML_STATUS_STATIC_ERROR:
    case SXML_STATUS_DYNAMIC_ERROR:
    case SXML_STATUS_TYPE_ERROR:
        break;
    }
}

sxml_status sxml_fuzz_compile_xpath(const sxml_fuzz_fixture *fixture,
                                    sxml_byte_view expression_text,
                                    sxml_ir_unit **unit) {
    static const char *const variable_names[SXML_FUZZ_VARIABLE_COUNT] = {"x",
                                                                         "doc"};
    sxml_variable_declaration declarations[SXML_FUZZ_VARIABLE_COUNT];
    sxml_status status = SXML_STATUS_OK;
    for (size_t variable_index = 0; variable_index < SXML_FUZZ_VARIABLE_COUNT;
         variable_index++) {
        declarations[variable_index] = (sxml_variable_declaration){
            .variable_name = {.index = 0},
            .declared_type = {.item_type = sxml_builtin_type_identifier(
                                  SXML_TYPE_ITEM),
                              .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_name(
                fixture->name_pool,
                &(sxml_name_request){
                    .namespace_uri = {.index = SXML_NAMESPACE_NONE},
                    .local_name = sxml_byte_view_from_c_string(
                        variable_names[variable_index])},
                &declarations[variable_index].variable_name);
        }
    }
    sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                      .namespace_uri = {.index = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            fixture->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("p"),
            &binding.namespace_prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            fixture->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("urn:p"),
            &binding.namespace_uri);
    }
    sxml_fuzz_check_status(status, "interning the static context's names");
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xpath_static_context static_context = {
        .item_heap = fixture->item_heap,
        .allocator = sxml_fuzz_fixture_allocator(fixture),
        .namespace_bindings = {.namespace_bindings = &binding,
                               .namespace_binding_count = 1},
        .variable_declarations = {.variable_declarations = declarations,
                                  .variable_declaration_count =
                                      SXML_FUZZ_VARIABLE_COUNT},
        .function_table = fixture->function_table,
    };
    sxml_error_record error_record = {.value = NULL};
    status = sxml_xpath_compile(
        &(sxml_xpath_compile_request){
            .expression_text = expression_text,
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("fuzz.xpath"),
            .static_context = &static_context,
            .maximum_expression_count = SXML_FUZZ_MAXIMUM_EXPRESSION_COUNT,
            .maximum_expression_depth = SXML_FUZZ_MAXIMUM_EXPRESSION_DEPTH,
            .error_record = &error_record},
        unit);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        /* The verifier's or the function table's complaint, for triage. */
        (void)fprintf(stderr, "%.*s\n",
                      (int)error_record.diagnostic.description.byte_count,
                      (const char *)error_record.diagnostic.description.bytes);
    }
    sxml_fuzz_check_status(status, "sxml_xpath_compile");
    return status;
}

sxml_byte_view sxml_fuzz_input_view(const uint8_t *data, size_t size) {
    /* Empty views made inside sxml often have no bytes at all; so does
       this one, so that the targets meet them too. */
    return (sxml_byte_view){.bytes = size == 0 ? NULL : data,
                            .byte_count = size};
}

void sxml_fuzz_run_xquery(const sxml_fuzz_fixture *fixture, sxml_processor *processor,
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

void sxml_fuzz_processor_destroy_checking_leaks(sxml_processor *processor) {
    sxml_item_heap *item_heap = sxml_processor_get_item_heap(processor);
    sxml_item_heap_clear_raised_error(item_heap);
    if (sxml_item_heap_read_counts(item_heap).live_cell_count != 0) {
        (void)fprintf(stderr, "leak: %llu cells in the processor's heap\n",
                      (unsigned long long)sxml_item_heap_read_counts(item_heap).live_cell_count);
        abort();
    }
    sxml_processor_destroy(processor);
}
