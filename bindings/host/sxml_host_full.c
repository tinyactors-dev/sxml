/*
 * What the full links' entry points share (sxml_host_xquery.c,
 * sxml_host_xslt.c, sxml_host_schema.c): side results, names and
 * bindings, and the trace sink that makes messages side results. Linked
 * into sxml.wasm and the native target sxml_host (D272).
 *
 * Golden file: sxml_host.c, whose handles, helpers and memory rules
 * these follow (sxml_host_internal.h).
 */
#include "sxml_host_internal.h"

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------- Side results */

/* A copy of text in allocator memory; the empty view for empty text. */
static sxml_status sxml_host_copy_text(sxml_allocator allocator, sxml_byte_view text,
                                       sxml_byte_view *copy) {
    *copy = sxml_host_no_text;
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    uint8_t *bytes = sxml_host_allocate_bytes(allocator, text.byte_count, 1);
    if (bytes == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    memcpy(bytes, text.bytes, text.byte_count);
    *copy = (sxml_byte_view){.bytes = bytes, .byte_count = text.byte_count};
    return SXML_STATUS_OK;
}

/* Releases the copies an entry holds. */
static void sxml_host_release_copies(sxml_allocator allocator,
                                     const sxml_host_side_result *side_result) {
    sxml_allocator_release(allocator, (void *)side_result->uri.bytes);
    sxml_allocator_release(allocator, (void *)side_result->text.bytes);
    sxml_allocator_release(allocator, (void *)side_result->code.bytes);
}

/* Makes room for one more entry. */
static sxml_status sxml_host_side_results_reserve(
    const sxml_host_side_result_target *target) {
    sxml_host_side_results *list = target->side_results;
    if (list->side_result_count < list->side_result_capacity) {
        return SXML_STATUS_OK;
    }
    const size_t capacity = list->side_result_capacity == 0 ? 8 : list->side_result_capacity * 2;
    size_t byte_count = 0;
    const sxml_status status =
        sxml_size_multiply(capacity, sizeof(sxml_host_side_result), &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_host_side_result *memory = sxml_host_allocate_bytes(
        target->processor->allocator, byte_count, alignof(sxml_host_side_result));
    if (memory == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    if (list->side_result_count > 0) {
        memcpy(memory, list->side_results,
               list->side_result_count * sizeof(sxml_host_side_result));
    }
    sxml_allocator_release(target->processor->allocator, list->side_results);
    list->side_results = memory;
    list->side_result_capacity = capacity;
    return SXML_STATUS_OK;
}

sxml_status sxml_host_side_results_add(const sxml_host_side_result_target *target,
                                       const sxml_host_side_result *side_result) {
    const sxml_allocator allocator = target->processor->allocator;
    sxml_host_side_result entry = *side_result;
    entry.uri = sxml_host_no_text;
    entry.text = sxml_host_no_text;
    entry.code = sxml_host_no_text;
    sxml_status status = sxml_host_side_results_reserve(target);
    if (status == SXML_STATUS_OK) {
        status = sxml_host_copy_text(allocator, side_result->uri, &entry.uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_host_copy_text(allocator, side_result->text, &entry.text);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_host_copy_text(allocator, side_result->code, &entry.code);
    }
    if (status != SXML_STATUS_OK) {
        sxml_host_release_copies(allocator, &entry);
        sxml_host_value_release(entry.first_value);
        sxml_host_value_release(entry.second_value);
        return status;
    }
    sxml_host_side_results *list = target->side_results;
    list->side_results[list->side_result_count] = entry;
    list->side_result_count += 1;
    return SXML_STATUS_OK;
}

void sxml_host_side_results_clear(const sxml_host_side_result_target *target) {
    sxml_host_side_results *list = target->side_results;
    for (size_t entry_index = 0; entry_index < list->side_result_count; entry_index += 1) {
        sxml_host_release_copies(target->processor->allocator,
                                 &list->side_results[entry_index]);
    }
    list->side_result_count = 0;
}

void sxml_host_side_results_finalize(const sxml_host_side_result_target *target) {
    sxml_host_side_results_clear(target);
    sxml_allocator_release(target->processor->allocator, target->side_results->side_results);
    *target->side_results = (sxml_host_side_results){
        .side_results = NULL, .side_result_count = 0, .side_result_capacity = 0};
}

sxml_host_side_result_list sxml_host_side_results_view(
    const sxml_host_side_results *side_results) {
    return (sxml_host_side_result_list){.side_results = side_results->side_results,
                                              .side_result_count =
                                                  side_results->side_result_count};
}

/* ------------------------------------------------------- Names and values */

sxml_status sxml_host_intern_name(sxml_host_processor *processor,
                                  const sxml_host_name *name,
                                  sxml_name_identifier *identifier) {
    *identifier = (sxml_name_identifier){.index = 0};
    if (name->local_name.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(processor->processor);
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_status status =
        sxml_name_pool_intern_namespace_uri(name_pool, name->namespace_uri, &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = name->local_name},
            identifier);
    }
    return status;
}

sxml_status sxml_host_read_bindings(sxml_host_processor *processor,
                                    const sxml_host_binding_list *list,
                                    sxml_variable_binding **bindings) {
    void *memory = NULL;
    sxml_status status = sxml_host_allocate_array(
        processor->allocator,
        (sxml_host_array_request){
            .element = {.byte_count = sizeof(sxml_variable_binding),
                        .alignment = alignof(sxml_variable_binding)},
            .element_count = list->binding_count},
        &memory);
    sxml_variable_binding *written = memory;
    for (size_t binding_index = 0; status == SXML_STATUS_OK && binding_index < list->binding_count;
         binding_index += 1) {
        const sxml_host_binding *binding = &list->bindings[binding_index];
        written[binding_index].value = binding->value->sequence;
        status = sxml_host_intern_name(processor, &binding->name,
                                       &written[binding_index].variable_name);
        if (status == SXML_STATUS_OK && written[binding_index].variable_name.index == 0) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, memory);
        written = NULL;
    }
    *bindings = written;
    return status;
}

sxml_host_value *sxml_host_value_retain(sxml_host_processor *processor,
                                        sxml_sequence sequence) {
    sxml_sequence_retain(sequence);
    return sxml_host_value_make(processor, sequence);
}

/* --------------------------------------------------------------- Messages */

/*
 * Each trace event as a MESSAGE side result: xsl:message and failed
 * xsl:assert written by the text method, as sxml transform writes them
 * (XSLT 4.0, 23.1: the message is a document node, its string value is
 * what to show), fn:trace and fn:message by the adaptive method (F&O 4.0,
 * "fn:trace": the format is implementation-dependent), the label in code.
 */
static sxml_status sxml_host_receive_message(void *state, const sxml_trace_event *event) {
    const sxml_host_side_result_target *target = state;
    const sxml_allocator allocator = target->processor->allocator;
    const bool is_xslt_message = event->trace_event_kind == SXML_TRACE_EVENT_KIND_XSLT_MESSAGE;
    sxml_output_buffer *output_buffer = NULL;
    sxml_status status = sxml_output_buffer_create(
        allocator,
        &(sxml_output_buffer_configuration){.allocator = allocator, .maximum_byte_count = 0},
        &output_buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.serialization_method =
        is_xslt_message ? SXML_SERIALIZATION_METHOD_TEXT : SXML_SERIALIZATION_METHOD_ADAPTIVE;
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    /* A message that cannot be written is shown as far as it goes; the
       evaluation goes on, as sxml transform does. */
    const sxml_status written = sxml_serialize_sequence(
        sxml_processor_get_item_heap(event->processor),
        &(sxml_serialization_request){.value = event->value,
                                      .serialization_parameters = parameters,
                                      .error_record = &error_record},
        sxml_output_buffer_as_output_stream(output_buffer));
    (void)written;
    status = sxml_host_side_results_add(
        target, &(sxml_host_side_result){
                    .side_result_kind = SXML_HOST_SIDE_RESULT_KIND_MESSAGE,
                    .uri = event->location.source_name,
                    .text = sxml_output_buffer_get_bytes(output_buffer),
                    .code = is_xslt_message ? event->error_code.local_name : event->label,
                    .first_value = NULL,
                    .second_value = NULL,
                    .line_number = event->location.line_number,
                    .column_number = event->location.column_number,
                    .is_terminating = event->is_terminating ? 1 : 0});
    sxml_output_buffer_destroy(output_buffer);
    return status;
}

static const sxml_trace_sink_operations sxml_host_message_operations = {
    .receive = sxml_host_receive_message,
};

sxml_trace_sink sxml_host_message_sink(sxml_host_side_result_target *target) {
    return (sxml_trace_sink){.state = target, .operations = &sxml_host_message_operations};
}

/* ----------------------------------------------------------------- Layout */

#if defined(__wasm32__)
/* The JavaScript package (src/index.ts) reads and writes these as words. */
_Static_assert(sizeof(sxml_host_name) == 16, "name: 4 words");
_Static_assert(sizeof(sxml_host_binding) == 20, "binding: 5 words");
_Static_assert(sizeof(sxml_host_side_result) == 48, "side result: 12 words");
_Static_assert(sizeof(sxml_host_query_compile_request) == 24, "query compile: 6 words");
_Static_assert(sizeof(sxml_host_query_evaluate_request) == 24, "query evaluate: 6 words");
_Static_assert(offsetof(sxml_host_query_evaluate_request, side_results) == 16,
               "query evaluate side results");
_Static_assert(sizeof(sxml_host_stylesheet_compile_request) == 24,
               "stylesheet compile: 6 words");
_Static_assert(sizeof(sxml_host_transform_request) == 132, "transform: 33 words");
_Static_assert(offsetof(sxml_host_transform_request, streamed_source_uri) == 124,
               "transform streamed source");
_Static_assert(offsetof(sxml_host_transform_request, side_results) == 116,
               "transform side results");
_Static_assert(sizeof(sxml_host_schema_document) == 16, "schema document: 4 words");
_Static_assert(sizeof(sxml_host_schema_compile_request) == 24, "schema compile: 6 words");
_Static_assert(sizeof(sxml_host_validate_request) == 52, "validate: 13 words");
_Static_assert(offsetof(sxml_host_validate_request, validity) == 40, "validate validity");
#endif
