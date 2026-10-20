#pragma once

/*
 * What the files of the host shim share (docs/GLOSSARY.md, "host_shim"):
 * the processor and value handles and the helpers every operation uses.
 * sxml_host.c (every link) and the files linked only into the full links
 * (sxml_host_full.c, sxml_host_xquery.c, sxml_host_xslt.c,
 * sxml_host_schema.c; D189's two links) include it. The helpers are
 * static, as they were in the JavaScript shim, so the core module
 * compiles to the same bytes (an inline hint on all of them made
 * sxml.wasm 1,609 bytes larger); each file that includes this one uses
 * every helper but sxml_host_begin, which alone is static inline so the
 * files that do not use it do not warn.
 */
#include "sxml_host.h"

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
#include <string.h>

/* ---------------------------------------------------------------- Handles */

struct sxml_host_processor {
    /* From the default allocator; everything else from allocator. */
    sxml_counting_allocator *counting_allocator;
    sxml_allocator allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    sxml_serialization_parameter_reader *parameter_reader;
    /* The host's resolver, asked by the processor's resource resolver. */
    sxml_host_resolver resolver;
    sxml_error_record error_record;
    sxml_host_error error;
    /* The bytes of the last serialization, in output_buffer. */
    sxml_byte_view output;
};

/* A value: an owned sequence in the processor's item heap. */
struct sxml_host_value {
    sxml_host_processor *owner;
    sxml_sequence sequence;
};

/* ---------------------------------------------------------------- Helpers */

static const sxml_byte_view sxml_host_no_text = {.bytes = NULL, .byte_count = 0};

/* Starts an operation: forgets the last error and the text it kept. */
static inline void sxml_host_begin(sxml_host_processor *processor) {
    sxml_item_heap_clear_raised_error(sxml_processor_get_item_heap(processor->processor));
    memset(&processor->error_record, 0, sizeof(processor->error_record));
    memset(&processor->error, 0, sizeof(processor->error));
}

/* Ends a failed operation: copies the error record into the flat error. */
static void sxml_host_fail(sxml_host_processor *processor,
                           sxml_status status) {
    const sxml_error_record *record = &processor->error_record;
    const bool has_code = sxml_status_has_error_record(status);
    processor->error = (sxml_host_error){
        .status = (uint32_t)status,
        .status_name = sxml_status_name(status),
        .code_namespace_uri = has_code ? record->code.namespace_uri : sxml_host_no_text,
        .code_local_name = has_code ? record->code.local_name : sxml_host_no_text,
        .code_namespace_prefix =
            has_code ? record->code.namespace_prefix : sxml_host_no_text,
        .description = record->diagnostic.description,
        .source_name = record->diagnostic.location.source_name,
        .line_number = record->diagnostic.location.line_number,
        .column_number = record->diagnostic.location.column_number};
}

static void *sxml_host_allocate_bytes(sxml_allocator allocator, size_t byte_count,
                                      size_t alignment) {
    void *memory = NULL;
    if (sxml_allocator_allocate(allocator,
                                (sxml_allocation_request){.byte_count = byte_count,
                                                          .alignment = alignment},
                                &memory) != SXML_STATUS_OK) {
        return NULL;
    }
    return memory;
}

/* Wraps sequence, owned, in a value; releases it and returns NULL when out
   of memory. */
static sxml_host_value *sxml_host_value_make(
    sxml_host_processor *processor, sxml_sequence sequence) {
    sxml_host_value *value = sxml_host_allocate_bytes(
        processor->allocator, sizeof(sxml_host_value), alignof(sxml_host_value));
    if (value == NULL) {
        sxml_sequence_release(sequence);
        sxml_host_fail(processor, SXML_STATUS_OUT_OF_MEMORY);
        return NULL;
    }
    *value = (sxml_host_value){.owner = processor, .sequence = sequence};
    return value;
}

/* An array to allocate: element_count elements of element's size and
   alignment. */
typedef struct sxml_host_array_request {
    sxml_allocation_request element;
    size_t element_count;
} sxml_host_array_request;

/* Allocates the array request describes, or sets *memory to NULL for no
   elements. */
static sxml_status sxml_host_allocate_array(sxml_allocator allocator,
                                            sxml_host_array_request request,
                                            void **memory) {
    *memory = NULL;
    if (request.element_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_allocation_request element = request.element;
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(request.element_count, element.byte_count, &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = element.alignment},
            memory);
    }
    return status;
}

/* -------------------------------------------- Full module: shared parts */

/*
 * Defined in sxml_host_full.c, which only the full module links,
 * with the files that use them.
 */

/* The side results a handle keeps (docs/GLOSSARY.md, "side_result"):
   entries reused from call to call, their views its own copies. */
typedef struct sxml_host_side_results {
    sxml_host_side_result *side_results;
    size_t side_result_count;
    size_t side_result_capacity;
} sxml_host_side_results;

/* Where a side result goes: a handle's list and the processor whose
   allocator holds it. */
typedef struct sxml_host_side_result_target {
    sxml_host_processor *processor;
    sxml_host_side_results *side_results;
} sxml_host_side_result_target;

/* Adds a copy of side_result's views, and its values (taken over: released
   on failure), to target's list. Returns the allocator's failures. */
sxml_status sxml_host_side_results_add(const sxml_host_side_result_target *target,
                                       const sxml_host_side_result *side_result);

/* Releases the copies of target's list and empties it, keeping its
   entries for reuse; the values belong to the package by now. */
void sxml_host_side_results_clear(const sxml_host_side_result_target *target);

/* Clears target's list and releases its entries. */
void sxml_host_side_results_finalize(const sxml_host_side_result_target *target);

/* The list as the package reads it. */
sxml_host_side_result_list sxml_host_side_results_view(
    const sxml_host_side_results *side_results);

/* Interns name in the processor's name pool; index 0 for an empty local
   name. */
sxml_status sxml_host_intern_name(sxml_host_processor *processor,
                                  const sxml_host_name *name,
                                  sxml_name_identifier *identifier);

/* Writes list as variable bindings to *bindings, an array of
   list->binding_count from the processor's allocator (NULL for none) that
   the caller releases. Returns the allocator's and name pool's failures;
   *bindings is then NULL. */
sxml_status sxml_host_read_bindings(sxml_host_processor *processor,
                                    const sxml_host_binding_list *list,
                                    sxml_variable_binding **bindings);

/* A new value holding sequence, retained (sequence stays the caller's), or
   NULL with the processor's error set. */
sxml_host_value *sxml_host_value_retain(sxml_host_processor *processor,
                                        sxml_sequence sequence);

/* A trace sink whose state is an sxml_host_side_result_target: each
   event becomes a MESSAGE side result. */
sxml_trace_sink sxml_host_message_sink(sxml_host_side_result_target *target);
