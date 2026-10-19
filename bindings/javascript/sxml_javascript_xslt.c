/*
 * The full module's XSLT entry points (include/sxml/xslt.h): compile a
 * stylesheet once, with its static parameters, and run transformations
 * with stylesheet parameters, an initial mode, template or function, the
 * principal result as a value or serialized to the processor's output,
 * and the result documents (kept by the built-in result document
 * collector) and messages as side results. Linked into sxml-full.wasm only
 * (D189's two links).
 *
 * Golden files: sxml_javascript_xquery.c, and apps/sxml/command_transform.c
 * (the same public API for the CLI: output definition, invocation,
 * messages).
 */
#include "sxml_javascript_internal.h"

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "sxml/xslt.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A compiled stylesheet and the side results of its last transformation. */
struct sxml_javascript_stylesheet {
    sxml_javascript_processor *owner;
    sxml_xslt_stylesheet *stylesheet;
    sxml_javascript_side_results side_results;
};

static sxml_javascript_side_result_target sxml_javascript_stylesheet_target(
    sxml_javascript_stylesheet *stylesheet) {
    return (sxml_javascript_side_result_target){.processor = stylesheet->owner,
                                                .side_results = &stylesheet->side_results};
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_stylesheet_destroy")
void sxml_javascript_stylesheet_destroy(sxml_javascript_stylesheet *stylesheet) {
    if (stylesheet == NULL) {
        return;
    }
    const sxml_javascript_side_result_target target =
        sxml_javascript_stylesheet_target(stylesheet);
    sxml_javascript_side_results_finalize(&target);
    sxml_xslt_stylesheet_destroy(stylesheet->stylesheet);
    sxml_allocator_release(stylesheet->owner->allocator, stylesheet);
}

/*
 * Compiles a stylesheet whose principal module is request's text, with its
 * static parameters (XSLT 4.0, 9.6), and returns it, or NULL with the
 * processor's error set (a static error, a module that is not well-formed
 * or that the resolver does not deliver, a limit, out of memory).
 */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_stylesheet_compile")
sxml_javascript_stylesheet *sxml_javascript_stylesheet_compile(
    sxml_javascript_processor *processor,
    const sxml_javascript_stylesheet_compile_request *request) {
    sxml_javascript_begin(processor);
    sxml_javascript_stylesheet *stylesheet =
        sxml_javascript_allocate_bytes(processor->allocator, sizeof(sxml_javascript_stylesheet),
                                       alignof(sxml_javascript_stylesheet));
    if (stylesheet == NULL) {
        sxml_javascript_fail(processor, SXML_STATUS_OUT_OF_MEMORY);
        return NULL;
    }
    *stylesheet = (sxml_javascript_stylesheet){
        .owner = processor,
        .stylesheet = NULL,
        .side_results = {.side_results = NULL, .side_result_count = 0, .side_result_capacity = 0}};
    sxml_variable_binding *static_parameters = NULL;
    sxml_status status =
        sxml_javascript_read_bindings(processor, &request->static_parameters, &static_parameters);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_stylesheet_create(
            processor->allocator,
            &(sxml_xslt_stylesheet_configuration){
                .processor = processor->processor,
                .principal_module_text = request->stylesheet_text,
                .static_parameters = {.variable_bindings = static_parameters,
                                      .variable_binding_count =
                                          request->static_parameters.binding_count},
                .static_context = {.static_base_uri = request->static_base_uri},
                .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
                .error_record = &processor->error_record},
            &stylesheet->stylesheet);
    }
    sxml_allocator_release(processor->allocator, static_parameters);
    if (status != SXML_STATUS_OK) {
        sxml_javascript_stylesheet_destroy(stylesheet);
        sxml_javascript_fail(processor, status);
        return NULL;
    }
    return stylesheet;
}

/* ------------------------------------------------------- Transformation */

/* What one transformation reads besides the request: the parameters,
   arguments and names in sxml's form, in allocator memory. */
typedef struct sxml_javascript_transform_inputs {
    sxml_variable_binding *stylesheet_parameters;
    sxml_variable_binding *template_parameters;
    sxml_variable_binding *tunnel_parameters;
    sxml_sequence *function_arguments;
    sxml_name_identifier initial_mode_name;
    sxml_name_identifier initial_template_name;
    sxml_name_identifier initial_function_name;
    sxml_xslt_output_definition output_definition;
} sxml_javascript_transform_inputs;

static void sxml_javascript_release_inputs(sxml_allocator allocator,
                                           const sxml_javascript_transform_inputs *inputs) {
    sxml_allocator_release(allocator, inputs->stylesheet_parameters);
    sxml_allocator_release(allocator, inputs->template_parameters);
    sxml_allocator_release(allocator, inputs->tunnel_parameters);
    sxml_allocator_release(allocator, inputs->function_arguments);
}

/* The function arguments of request as sequences. */
static sxml_status sxml_javascript_read_arguments(sxml_javascript_processor *processor,
                                                  const sxml_javascript_transform_request *request,
                                                  sxml_sequence **arguments) {
    void *memory = NULL;
    const sxml_status status = sxml_javascript_allocate_array(
        processor->allocator,
        (sxml_javascript_array_request){
            .element = {.byte_count = sizeof(sxml_sequence), .alignment = alignof(sxml_sequence)},
            .element_count = request->function_argument_count},
        &memory);
    sxml_sequence *sequences = memory;
    for (size_t argument_index = 0;
         status == SXML_STATUS_OK && argument_index < request->function_argument_count;
         argument_index += 1) {
        sequences[argument_index] = request->function_arguments[argument_index]->sequence;
    }
    *arguments = sequences;
    return status;
}

/*
 * The output definition of the principal result (XSLT 4.0, 26.1.1): the
 * stylesheet's unnamed one, whose serialization parameters the request's
 * options replace, method included, as sxml transform's --method replaces
 * its method.
 */
static sxml_status sxml_javascript_read_output_definition(
    const sxml_javascript_stylesheet *stylesheet,
    const sxml_javascript_transform_request *request,
    sxml_xslt_output_definition *output_definition) {
    sxml_javascript_processor *processor = stylesheet->owner;
    sxml_status status = sxml_xslt_stylesheet_get_output_definition(
        stylesheet->stylesheet, (sxml_name_identifier){.index = 0}, output_definition);
    if (status == SXML_STATUS_OK && request->serialization_options != NULL) {
        status = sxml_serialization_parameter_reader_read(
            processor->parameter_reader,
            &(sxml_serialization_parameter_read_request){
                .options = request->serialization_options->sequence,
                .error_record = &processor->error_record},
            &output_definition->serialization_parameters);
        output_definition->has_serialization_method = true;
    }
    return status;
}

/* Reads the parameters, arguments, names and output definition. */
static sxml_status sxml_javascript_read_inputs(const sxml_javascript_stylesheet *stylesheet,
                                               const sxml_javascript_transform_request *request,
                                               sxml_javascript_transform_inputs *inputs) {
    sxml_javascript_processor *processor = stylesheet->owner;
    sxml_status status = sxml_javascript_read_bindings(
        processor, &request->stylesheet_parameters, &inputs->stylesheet_parameters);
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_read_bindings(processor, &request->template_parameters,
                                               &inputs->template_parameters);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_read_bindings(processor, &request->tunnel_parameters,
                                               &inputs->tunnel_parameters);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_read_arguments(processor, request, &inputs->function_arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_intern_name(processor, &request->initial_mode,
                                             &inputs->initial_mode_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_intern_name(processor, &request->initial_template,
                                             &inputs->initial_template_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_intern_name(processor, &request->initial_function,
                                             &inputs->initial_function_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_read_output_definition(stylesheet, request,
                                                        &inputs->output_definition);
    }
    return status;
}

/* The transformation itself, once its inputs are read. */
typedef struct sxml_javascript_transformation {
    const sxml_javascript_transform_request *request;
    const sxml_javascript_transform_inputs *inputs;
    sxml_xslt_result_document_collector *collector;
    sxml_javascript_side_result_target *target;
} sxml_javascript_transformation;

/* Hands each collected result document to the list as a RESULT_DOCUMENT
   side result: its URI, and its bytes or its value (XSLT 4.0, 26.2). */
static sxml_status sxml_javascript_add_result_documents(
    const sxml_javascript_transformation *transformation) {
    const sxml_javascript_side_result_target *target = transformation->target;
    const sxml_xslt_result_document_collector *collector = transformation->collector;
    const bool is_serialized = transformation->request->result_document_delivery ==
                               (uint32_t)SXML_XSLT_RESULT_DELIVERY_SERIALIZED;
    const size_t document_count =
        sxml_xslt_result_document_collector_get_result_document_count(collector);
    sxml_status status = SXML_STATUS_OK;
    for (size_t document_index = 0; status == SXML_STATUS_OK && document_index < document_count;
         document_index += 1) {
        sxml_xslt_collected_result_document document;
        memset(&document, 0, sizeof(document));
        status = sxml_xslt_result_document_collector_get_result_document(collector, document_index,
                                                                         &document);
        sxml_javascript_value *value = NULL;
        if (status == SXML_STATUS_OK && !is_serialized) {
            value = sxml_javascript_value_retain(target->processor, document.value);
            status = value == NULL ? SXML_STATUS_OUT_OF_MEMORY : SXML_STATUS_OK;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_javascript_side_results_add(
                target, &(sxml_javascript_side_result){
                            .side_result_kind = SXML_JAVASCRIPT_SIDE_RESULT_KIND_RESULT_DOCUMENT,
                            .uri = document.absolute_uri,
                            .text = document.serialized_bytes,
                            .code = sxml_javascript_no_text,
                            .first_value = value,
                            .second_value = NULL,
                            .line_number = 0,
                            .column_number = 0,
                            .is_terminating = 0});
        }
    }
    return status;
}

static sxml_status sxml_javascript_run_transformation(
    const sxml_javascript_stylesheet *stylesheet,
    const sxml_javascript_transformation *transformation, sxml_sequence *principal_result) {
    sxml_javascript_processor *processor = stylesheet->owner;
    const sxml_javascript_transform_request *request = transformation->request;
    const sxml_javascript_transform_inputs *inputs = transformation->inputs;
    const sxml_javascript_value *context = request->global_context_item;
    const sxml_javascript_value *selection = request->initial_match_selection;
    return sxml_xslt_stylesheet_transform(
        stylesheet->stylesheet,
        &(sxml_xslt_transformation_request){
            .dynamic_context =
                {.has_context_value = context != NULL,
                 .context_value = context != NULL ? context->sequence : sxml_sequence_empty(),
                 .variable_bindings = {.variable_bindings = inputs->stylesheet_parameters,
                                       .variable_binding_count =
                                           request->stylesheet_parameters.binding_count},
                 .trace_sink = sxml_javascript_message_sink(transformation->target)},
            .initial_match_selection =
                selection != NULL ? selection->sequence : sxml_sequence_empty(),
            .template_parameters = {.variable_bindings = inputs->template_parameters,
                                    .variable_binding_count =
                                        request->template_parameters.binding_count},
            .tunnel_parameters = {.variable_bindings = inputs->tunnel_parameters,
                                  .variable_binding_count =
                                      request->tunnel_parameters.binding_count},
            .function_arguments = {.sequences = inputs->function_arguments,
                                   .sequence_count = request->function_argument_count},
            .base_output_uri = request->base_output_uri,
            .output_definition = &inputs->output_definition,
            .output_stream = sxml_output_buffer_as_output_stream(processor->output_buffer),
            .result_document_handler =
                sxml_xslt_result_document_collector_as_handler(transformation->collector),
            .error_record = &processor->error_record,
            .initial_mode_name = inputs->initial_mode_name,
            .initial_template_name = inputs->initial_template_name,
            .initial_function_name = inputs->initial_function_name,
            .invocation_kind = (sxml_xslt_invocation_kind)request->invocation_kind,
            .result_delivery = (sxml_xslt_result_delivery)request->result_delivery,
            .has_initial_match_selection = selection != NULL,
            .selects_unnamed_mode = request->selects_unnamed_mode != 0},
        principal_result);
}

/*
 * Runs one transformation as request describes and returns the principal
 * result as a value (the empty sequence when it is serialized: the bytes
 * are then the processor's output), or NULL with the processor's error
 * set. Writes the side results into request, also on failure: the result
 * documents completed and the messages written before it.
 */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_stylesheet_transform")
sxml_javascript_value *sxml_javascript_stylesheet_transform(
    sxml_javascript_stylesheet *stylesheet, sxml_javascript_transform_request *request) {
    sxml_javascript_processor *processor = stylesheet->owner;
    sxml_javascript_begin(processor);
    sxml_output_buffer_clear(processor->output_buffer);
    processor->output = sxml_javascript_no_text;
    sxml_javascript_side_result_target target = sxml_javascript_stylesheet_target(stylesheet);
    sxml_javascript_side_results_clear(&target);
    sxml_javascript_transform_inputs inputs;
    memset(&inputs, 0, sizeof(inputs));
    sxml_status status = sxml_javascript_read_inputs(stylesheet, request, &inputs);
    sxml_xslt_result_document_collector *collector = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_result_document_collector_create(
            processor->allocator,
            &(sxml_xslt_result_document_collector_configuration){
                .allocator = processor->allocator,
                .maximum_byte_count = 0,
                .result_delivery = (sxml_xslt_result_delivery)request->result_document_delivery},
            &collector);
    }
    sxml_sequence principal_result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        const sxml_javascript_transformation transformation = {
            .request = request, .inputs = &inputs, .collector = collector, .target = &target};
        status = sxml_javascript_run_transformation(stylesheet, &transformation, &principal_result);
        /* Result documents closed before a failure stay with the handler
           (xslt.h): the package sees them with the error. */
        const sxml_status added = sxml_javascript_add_result_documents(&transformation);
        if (status == SXML_STATUS_OK) {
            status = added;
        }
    }
    sxml_xslt_result_document_collector_destroy(collector);
    sxml_javascript_release_inputs(processor->allocator, &inputs);
    request->side_results = sxml_javascript_side_results_view(&stylesheet->side_results);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(principal_result);
        sxml_javascript_fail(processor, status);
        return NULL;
    }
    processor->output = sxml_output_buffer_get_bytes(processor->output_buffer);
    return sxml_javascript_value_make(processor, principal_result);
}
