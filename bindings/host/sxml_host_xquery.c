/*
 * The full module's XQuery entry points (include/sxml/xquery.h): compile a
 * main module once, evaluate it many times, serialize a result with the
 * query's output declarations, and hand the trees an updating query
 * changed, the documents fn:put stores and the trace output to the host
 * as side results. Linked into the full links only (D189's two links).
 *
 * Golden files: sxml_host.c (compile, evaluate, serialize for XPath)
 * and apps/sxml/command_xquery.c (the same public API for the CLI, with
 * its update sink).
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
#include "sxml/xquery.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A compiled query and the side results of its last evaluation. */
struct sxml_host_query {
    sxml_host_processor *owner;
    sxml_xquery_query *query;
    sxml_host_side_results side_results;
};

/* The query's list, for adding to it. */
static sxml_host_side_result_target sxml_host_query_target(
    sxml_host_query *query) {
    return (sxml_host_side_result_target){.processor = query->owner,
                                                .side_results = &query->side_results};
}

void sxml_host_query_destroy(sxml_host_query *query) {
    if (query == NULL) {
        return;
    }
    const sxml_host_side_result_target target = sxml_host_query_target(query);
    sxml_host_side_results_finalize(&target);
    sxml_xquery_query_destroy(query->query);
    sxml_allocator_release(query->owner->allocator, query);
}

/* Interns request's namespaces into an array the caller releases. */
static sxml_status sxml_host_query_namespaces(
    sxml_host_processor *processor, const sxml_host_query_compile_request *request,
    sxml_namespace_binding **namespace_bindings) {
    void *memory = NULL;
    sxml_status status = sxml_host_allocate_array(
        processor->allocator,
        (sxml_host_array_request){
            .element = {.byte_count = sizeof(sxml_namespace_binding),
                        .alignment = alignof(sxml_namespace_binding)},
            .element_count = request->namespace_count},
        &memory);
    sxml_namespace_binding *bindings = memory;
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(processor->processor);
    for (size_t namespace_index = 0;
         status == SXML_STATUS_OK && namespace_index < request->namespace_count;
         namespace_index += 1) {
        const sxml_host_namespace *source = &request->namespaces[namespace_index];
        status = sxml_name_pool_intern_namespace_prefix(name_pool, source->namespace_prefix,
                                                        &bindings[namespace_index].namespace_prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_uri(name_pool, source->namespace_uri,
                                                         &bindings[namespace_index].namespace_uri);
        }
    }
    *namespace_bindings = bindings;
    return status;
}

/*
 * Compiles an XQuery 4.0 main module with the namespaces of request, and
 * returns it, or NULL with the processor's error set (a static or type
 * error, a module the resolver does not deliver, a limit, out of memory).
 */
sxml_host_query *sxml_host_query_compile(
    sxml_host_processor *processor, const sxml_host_query_compile_request *request) {
    sxml_host_begin(processor);
    sxml_host_query *query = sxml_host_allocate_bytes(
        processor->allocator, sizeof(sxml_host_query), alignof(sxml_host_query));
    if (query == NULL) {
        sxml_host_fail(processor, SXML_STATUS_OUT_OF_MEMORY);
        return NULL;
    }
    *query = (sxml_host_query){
        .owner = processor,
        .query = NULL,
        .side_results = {.side_results = NULL, .side_result_count = 0, .side_result_capacity = 0}};
    sxml_namespace_binding *namespace_bindings = NULL;
    sxml_status status = sxml_host_query_namespaces(processor, request, &namespace_bindings);
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_query_create(
            processor->allocator,
            &(sxml_xquery_query_configuration){
                .processor = processor->processor,
                .expression_text = request->query_text,
                .source_name = SXML_BYTE_VIEW_FROM_LITERAL("query"),
                .static_context = {.namespace_bindings = {.namespace_bindings = namespace_bindings,
                                                          .namespace_binding_count =
                                                              request->namespace_count},
                                   .static_base_uri = request->static_base_uri},
                .error_record = &processor->error_record},
            &query->query);
    }
    sxml_allocator_release(processor->allocator, namespace_bindings);
    if (status != SXML_STATUS_OK) {
        sxml_host_query_destroy(query);
        sxml_host_fail(processor, status);
        return NULL;
    }
    return query;
}

uint32_t sxml_host_query_is_updating(const sxml_host_query *query) {
    return sxml_xquery_query_is_updating(query->query) ? 1 : 0;
}

/* ---------------------------------------------------------- Update sink */

/* An updated tree as an UPDATED_TREE side result (XQUF 3.0, 3.2.2
   upd:applyUpdates; include/sxml/xquery.h): both roots, retained. */
static sxml_status sxml_host_receive_updated_tree(
    void *state, const sxml_xquery_updated_tree *updated_tree) {
    const sxml_host_side_result_target *target = state;
    sxml_host_value *original_root = sxml_host_value_retain(
        target->processor, sxml_sequence_from_item(updated_tree->original_root));
    sxml_host_value *updated_root =
        original_root == NULL
            ? NULL
            : sxml_host_value_retain(target->processor,
                                     sxml_sequence_from_item(updated_tree->updated_root));
    if (updated_root == NULL) {
        sxml_host_value_release(original_root);
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    return sxml_host_side_results_add(
        target,
        &(sxml_host_side_result){.side_result_kind =
                                           SXML_HOST_SIDE_RESULT_KIND_UPDATED_TREE,
                                       .uri = sxml_host_no_text,
                                       .text = sxml_host_no_text,
                                       .code = sxml_host_no_text,
                                       .first_value = original_root,
                                       .second_value = updated_root,
                                       .line_number = 0,
                                       .column_number = 0,
                                       .is_terminating = 0});
}

/* A document fn:put stores as a PUT_DOCUMENT side result (XQUF 3.0, 6.1):
   the host decides where it goes. */
static sxml_status sxml_host_receive_put_document(
    void *state, const sxml_xquery_put_document *put_document) {
    const sxml_host_side_result_target *target = state;
    sxml_host_value *node = sxml_host_value_retain(
        target->processor, sxml_sequence_from_item(put_document->node));
    if (node == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    sxml_host_value *parameters = NULL;
    sxml_emptiness emptiness = {.is_empty = true};
    const sxml_status status =
        sxml_sequence_test_emptiness(put_document->serialization_parameters, NULL, &emptiness);
    if (status != SXML_STATUS_OK) {
        sxml_host_value_release(node);
        return status;
    }
    if (!emptiness.is_empty) {
        parameters =
            sxml_host_value_retain(target->processor, put_document->serialization_parameters);
        if (parameters == NULL) {
            sxml_host_value_release(node);
            return SXML_STATUS_OUT_OF_MEMORY;
        }
    }
    return sxml_host_side_results_add(
        target,
        &(sxml_host_side_result){.side_result_kind =
                                           SXML_HOST_SIDE_RESULT_KIND_PUT_DOCUMENT,
                                       .uri = put_document->uri,
                                       .text = sxml_host_no_text,
                                       .code = sxml_host_no_text,
                                       .first_value = node,
                                       .second_value = parameters,
                                       .line_number = 0,
                                       .column_number = 0,
                                       .is_terminating = 0});
}

static const sxml_xquery_update_sink_operations sxml_host_update_operations = {
    .receive_updated_tree = sxml_host_receive_updated_tree,
    .receive_put_document = sxml_host_receive_put_document,
};

/* Without receive_put_document, fn:put fails with FOUP0002 (xquery.h). */
static const sxml_xquery_update_sink_operations sxml_host_tree_update_operations = {
    .receive_updated_tree = sxml_host_receive_updated_tree,
    .receive_put_document = NULL,
};

/* ----------------------------------------------------------- Evaluation */

/*
 * Evaluates a query against the context value and external variables of
 * request, and returns its result, or NULL with the processor's error
 * set. Writes the side results into request, also on failure.
 */
sxml_host_value *sxml_host_query_evaluate(
    sxml_host_query *query, sxml_host_query_evaluate_request *request) {
    sxml_host_processor *processor = query->owner;
    sxml_host_begin(processor);
    sxml_host_side_result_target target = sxml_host_query_target(query);
    sxml_host_side_results_clear(&target);
    sxml_variable_binding *bindings = NULL;
    sxml_status status = sxml_host_read_bindings(processor, &request->variables, &bindings);
    const sxml_host_value *context = request->context_value;
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_query_evaluate(
            query->query,
            &(sxml_xquery_evaluation_request){
                .dynamic_context =
                    {.has_context_value = context != NULL,
                     .context_value = context != NULL ? context->sequence : sxml_sequence_empty(),
                     .variable_bindings = {.variable_bindings = bindings,
                                           .variable_binding_count =
                                               request->variables.binding_count},
                     .trace_sink = sxml_host_message_sink(&target)},
                .error_record = &processor->error_record,
                .update_sink = {.state = &target,
                                .operations = request->accepts_put_documents != 0
                                                  ? &sxml_host_update_operations
                                                  : &sxml_host_tree_update_operations}},
            &result);
    }
    sxml_allocator_release(processor->allocator, bindings);
    request->side_results = sxml_host_side_results_view(&query->side_results);
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
        return NULL;
    }
    return sxml_host_value_make(processor, result);
}

/*
 * Serializes value into the processor's output, or to its output writer
 * (D294), with the parameters of the query's output declarations (XQuery
 * 4.0, 5.22; method xml unless they name another). Returns the status; on
 * failure the processor's error is set.
 */
uint32_t sxml_host_query_serialize(sxml_host_query *query,
                                   const sxml_host_value *value) {
    sxml_host_processor *processor = query->owner;
    sxml_host_begin(processor);
    sxml_output_stream output_stream;
    sxml_status status = sxml_host_output_begin(processor, &output_stream);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(processor->processor),
            &(sxml_serialization_request){
                .value = value->sequence,
                .serialization_parameters =
                    sxml_xquery_query_get_serialization_parameters(query->query),
                .error_record = &processor->error_record},
            output_stream);
    }
    status = sxml_host_output_end(processor, status);
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
        return (uint32_t)status;
    }
    return SXML_STATUS_OK;
}
