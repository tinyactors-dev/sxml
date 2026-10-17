/*
 * TEMPORARY (packet 3.13, D134): the QT4 driver's XQuery adapter
 * (xquery_adapter.h), deleted by packet 3.12. The compiling half imitates
 * sxml_xpath_expression_initialize and the evaluating half
 * sxml_xpath_expression_evaluate (src/xpath.c), with sxml_xquery_compile
 * (src/xquery_lower_internal.h) in place of sxml_xpath_compile. It is the
 * one file of the driver that includes src/ headers.
 */
#include "xquery_adapter.h"

#include "evaluator_internal.h"
#include "processor_internal.h"
#include "xpath_static_internal.h"
#include "xquery_lower_internal.h"

#include <stdalign.h>
#include <string.h>

struct sxml_qt4_xquery_query {
    sxml_processor *processor;
    /* The allocator of create, which destroy frees the query with. */
    sxml_allocator creating_allocator;
    /* Sealed; one reference. */
    sxml_ir_unit *unit;
    /* A copy of the static base URI from the processor's allocator; NULL
       when it is absent. */
    uint8_t *static_base_uri_bytes;
    size_t static_base_uri_byte_count;
};

sxml_status sxml_qt4_xquery_query_memory_requirements(
    const sxml_xquery_query_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(sxml_qt4_xquery_query),
        .alignment = alignof(sxml_qt4_xquery_query)};
    return SXML_STATUS_OK;
}

sxml_status sxml_qt4_xquery_query_initialize(
    sxml_memory_view memory,
    const sxml_xquery_query_configuration *configuration,
    sxml_qt4_xquery_query **query) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_qt4_xquery_query_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_processor *processor = configuration->processor;
    const sxml_static_context_configuration *context =
        &configuration->static_context;
    const sxml_byte_view static_base_uri = context->static_base_uri;
    void *base_uri_memory = NULL;
    if (static_base_uri.byte_count > 0) {
        status = sxml_allocator_allocate(
            processor->allocator,
            (sxml_allocation_request){.byte_count = static_base_uri.byte_count,
                                      .alignment = 1},
            &base_uri_memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(base_uri_memory, static_base_uri.bytes,
               static_base_uri.byte_count);
    }
    /* As sxml_processor_static_context of src/xpath.c. */
    const sxml_xpath_static_context static_context = {
        .item_heap = processor->item_heap,
        .allocator = processor->allocator,
        .namespace_bindings = context->namespace_bindings,
        .variable_declarations = context->variable_declarations,
        .function_table = sxml_processor_get_function_table(processor),
        .static_base_uri = static_base_uri};
    sxml_ir_unit *unit = NULL;
    status = sxml_xquery_compile(
        &(sxml_xpath_compile_request){
            .expression_text = configuration->expression_text,
            .source_name = configuration->source_name,
            .static_context = &static_context,
            .maximum_expression_count = 0,
            .maximum_expression_depth =
                sxml_processor_get_maximum_expression_depth(processor),
            .error_record = configuration->error_record},
        &unit);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, base_uri_memory);
        return status;
    }
    void *placement = memory.bytes;
    sxml_qt4_xquery_query *new_query = placement;
    *new_query = (sxml_qt4_xquery_query){
        .processor = processor,
        .creating_allocator = {.state = NULL, .operations = NULL},
        .unit = unit,
        .static_base_uri_bytes = base_uri_memory,
        .static_base_uri_byte_count = static_base_uri.byte_count};
    *query = new_query;
    return SXML_STATUS_OK;
}

void sxml_qt4_xquery_query_finalize(sxml_qt4_xquery_query *query) {
    if (query == NULL) {
        return;
    }
    sxml_ir_unit_release(query->unit);
    sxml_allocator_release(query->processor->allocator,
                           query->static_base_uri_bytes);
    query->unit = NULL;
    query->static_base_uri_bytes = NULL;
}

sxml_status sxml_qt4_xquery_query_create(
    sxml_allocator allocator,
    const sxml_xquery_query_configuration *configuration,
    sxml_qt4_xquery_query **query) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_qt4_xquery_query_memory_requirements(configuration, &requirements);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_xquery_query *new_query = NULL;
    status = sxml_qt4_xquery_query_initialize(
        (sxml_memory_view){.bytes = memory,
                           .byte_count = requirements.byte_count},
        configuration, &new_query);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_query->creating_allocator = allocator;
    *query = new_query;
    return SXML_STATUS_OK;
}

void sxml_qt4_xquery_query_destroy(sxml_qt4_xquery_query *query) {
    if (query == NULL) {
        return;
    }
    const sxml_allocator allocator = query->creating_allocator;
    sxml_qt4_xquery_query_finalize(query);
    sxml_allocator_release(allocator, query);
}

/* ------------------------------------------------------------ Evaluation */

/* Global n gets the binding of its name, or stays absent (XPDY0002 when
   read; D63). */
static void
sxml_qt4_bind_globals(const sxml_ir_unit *unit,
                      const sxml_variable_binding_view *bindings,
                      sxml_evaluator_slot *slots) {
    const uint32_t global_count = unit->global_variable_count;
    for (uint32_t global_number = 1; global_number < global_count;
         global_number += 1) {
        const sxml_ir_global_variable *global = sxml_ir_unit_get_global_variable(
            unit, (sxml_ir_global_index){.value = global_number});
        sxml_evaluator_slot slot = {.value = sxml_sequence_empty(),
                                    .slot_state =
                                        SXML_EVALUATOR_SLOT_STATE_ABSENT};
        for (size_t binding_index = 0;
             binding_index < bindings->variable_binding_count;
             binding_index += 1) {
            const sxml_variable_binding *variable =
                &bindings->variable_bindings[binding_index];
            if (variable->variable_name.index == global->variable_name.index) {
                slot = (sxml_evaluator_slot){
                    .value = variable->value,
                    .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
                break;
            }
        }
        slots[global_number - 1] = slot;
    }
}

/* What one evaluation holds while it runs. */
typedef struct sxml_qt4_xquery_resources {
    sxml_evaluator_slot *global_slots;
    size_t global_slot_count;
    sxml_processor_document_loader loader_memory;
    sxml_processor_document_loader *loader;
    sxml_dynamic_context *dynamic_context;
} sxml_qt4_xquery_resources;

static void
sxml_qt4_xquery_resources_release(const sxml_processor *processor,
                                  sxml_qt4_xquery_resources *resources) {
    sxml_dynamic_context_destroy(resources->dynamic_context);
    sxml_processor_document_loader_finalize(resources->loader);
    sxml_allocator_release(processor->allocator, resources->global_slots);
}

static sxml_status
sxml_qt4_xquery_resources_acquire(const sxml_qt4_xquery_query *query,
                                  const sxml_dynamic_context_configuration *context,
                                  sxml_qt4_xquery_resources *resources) {
    sxml_processor *processor = query->processor;
    const sxml_ir_unit *unit = query->unit;
    sxml_status status = SXML_STATUS_OK;
    if (unit->global_variable_count > 1) {
        resources->global_slot_count = unit->global_variable_count - 1U;
        size_t byte_count = 0;
        status = sxml_size_multiply(resources->global_slot_count,
                                    sizeof(sxml_evaluator_slot), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                processor->allocator,
                (sxml_allocation_request){
                    .byte_count = byte_count,
                    .alignment = alignof(sxml_evaluator_slot)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        resources->global_slots = memory;
        sxml_qt4_bind_globals(unit, &context->variable_bindings, memory);
    }
    status = sxml_processor_document_loader_initialize(
        (sxml_memory_view){.bytes = (uint8_t *)&resources->loader_memory,
                           .byte_count = sizeof(resources->loader_memory)},
        &(sxml_processor_document_loader_configuration){
            /* As src/xpath.c builds it (D131). */
            .evaluation_environment =
                {.processor = processor,
                 .resource_resolver = context->resource_resolver.operations == NULL
                                          ? processor->resource_resolver
                                          : context->resource_resolver,
                 .trace_sink = context->trace_sink.operations == NULL ? processor->trace_sink
                                                                      : context->trace_sink,
                 .default_language = context->default_language.byte_count == 0
                                         ? SXML_BYTE_VIEW_FROM_LITERAL("en")
                                         : context->default_language,
                 .environment_variables = context->environment_variables},
            .available_documents = context->available_documents},
        &resources->loader);
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_create(
            processor->allocator,
            &(sxml_evaluator_configuration){
                .item_heap = processor->item_heap,
                .allocator = processor->allocator,
                .document_loader = sxml_processor_document_loader_as_document_loader(
                    resources->loader),
                .clock = context->clock.operations == NULL ? processor->clock
                                                           : context->clock,
                .function_table = sxml_processor_get_function_table(processor),
                .maximum_evaluation_depth =
                    sxml_processor_get_maximum_evaluation_depth(processor),
                .maximum_frame_slot_count = 0,
                .maximum_evaluation_step_count =
                    sxml_processor_get_maximum_evaluation_step_count(processor)},
            &resources->dynamic_context);
    }
    if (status != SXML_STATUS_OK) {
        sxml_qt4_xquery_resources_release(processor, resources);
    }
    return status;
}

sxml_status sxml_qt4_xquery_query_evaluate(
    const sxml_qt4_xquery_query *query,
    const sxml_xquery_evaluation_request *request, sxml_sequence *result) {
    if (query == NULL || query->unit == NULL || request == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_dynamic_context_configuration *context =
        &request->dynamic_context;
    sxml_qt4_xquery_resources resources;
    memset(&resources, 0, sizeof resources);
    sxml_status status =
        sxml_qt4_xquery_resources_acquire(query, context, &resources);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence value = sxml_sequence_empty();
    status = sxml_evaluator_evaluate(
        resources.dynamic_context,
        &(sxml_evaluation_request){
            .unit = query->unit,
            .has_context_value = context->has_context_value,
            .context_value = context->context_value,
            .global_values = {.slots = resources.global_slots,
                              .slot_count = resources.global_slot_count},
            .static_base_uri = {.bytes = query->static_base_uri_bytes,
                                .byte_count = query->static_base_uri_byte_count},
            .error_record = request->error_record},
        &value);
    sxml_qt4_xquery_resources_release(query->processor, &resources);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = value;
    return SXML_STATUS_OK;
}

sxml_serialization_parameters sxml_qt4_xquery_query_get_serialization_parameters(
    const sxml_qt4_xquery_query *query) {
    (void)query;
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof parameters);
    /* XQuery 4.0, section 2.2.4: the default output method is xml. */
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
    return parameters;
}
