/*
 * XPath 4.0 through the public API: compiled expressions, evaluation and
 * sequence types given as text (include/sxml/xpath.h; docs/DECISIONS.md,
 * D47, D51, D88).
 *
 * Golden file: src/memory.c (layout, lifecycle, comments).
 *
 * Compilation is sxml_xpath_compile (src/xpath_static_internal.h) against
 * the processor's function library; the sealed unit is the compiled form,
 * reference counted, so function items in a result keep it alive after
 * the expression is finalized (D38, D55).
 *
 * Every evaluation makes its own dynamic context (src/evaluator_internal.h)
 * and its own document loader (src/processor_internal.h) from the
 * processor's allocator and releases both before it returns, so
 * evaluations nest: a host function body may evaluate another expression,
 * of the same processor, while one runs (D51). The clock is read at most
 * once per evaluation, by the dynamic context.
 *
 * The checks of the contexts and the evaluation of a sealed unit are
 * shared with the XQuery API (src/xpath_api_internal.h, src/xquery.c).
 */
#include "sxml/xpath.h"

#include "evaluator_internal.h"
#include "xpath_api_internal.h"
#include "format_internal.h"
#include "name_internal.h"
#include "processor_internal.h"
#include "schema_internal.h"
#include "xdm_internal.h"
#include "xpath_ast_internal.h"
#include "xpath_static_internal.h"
#include "sxml/string.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

struct sxml_xpath_expression {
    sxml_processor *processor;
    /* The allocator of sxml_xpath_expression_create, which destroy frees
       the expression with; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    /* Sealed; one reference. */
    sxml_ir_unit *unit;
    /* A copy of the static base URI, from the processor's allocator; NULL
       when it is absent. */
    uint8_t *static_base_uri_bytes;
    size_t static_base_uri_byte_count;
};

/* ------------------------------------------------------- Static context */

/* True when the text is well-formed UTF-8 (views of the configuration
   become xs:string items and copies). */
static bool sxml_text_is_valid(sxml_byte_view text) {
    if (text.byte_count == 0) {
        return true;
    }
    sxml_utf8_validation validation;
    return text.bytes != NULL && sxml_utf8_validate(text, &validation) == SXML_STATUS_OK;
}

/*
 * Checks one decimal format (F&O 4.0, section 4.9.1, "Defining a decimal
 * format"): each property kind at most once, then every form and the
 * distinct markers through sxml_decimal_format_apply (D119).
 */
static bool sxml_decimal_format_is_valid(const sxml_decimal_format *format) {
    const sxml_decimal_format_property_view properties = format->decimal_format_properties;
    if (properties.decimal_format_property_count > 0 &&
        properties.decimal_format_properties == NULL) {
        return false;
    }
    for (size_t property_index = 0;
         property_index < properties.decimal_format_property_count; property_index += 1) {
        const sxml_decimal_format_property *property =
            &properties.decimal_format_properties[property_index];
        if (!sxml_text_is_valid(property->property_value)) {
            return false;
        }
        for (size_t earlier_index = 0; earlier_index < property_index; earlier_index += 1) {
            if (properties.decimal_format_properties[earlier_index].property_kind ==
                property->property_kind) {
                return false;
            }
        }
    }
    const sxml_decimal_format_settings defaults = sxml_decimal_format_default_settings();
    sxml_decimal_format_settings settings;
    return sxml_decimal_format_apply(
               &(sxml_decimal_format_change){.base_settings = &defaults,
                                             .decimal_format_properties = properties},
               &settings) == SXML_STATUS_OK;
}

/* Checks the decimal formats of a static context: valid formats, names
   the pool issued and distinct (XPath 4.0, section 2.2.1, "statically
   known decimal formats"). */
static bool sxml_decimal_formats_are_valid(const sxml_processor *processor,
                                           sxml_decimal_format_view formats) {
    if (formats.decimal_format_count > 0 && formats.decimal_formats == NULL) {
        return false;
    }
    const uint32_t name_limit = sxml_name_pool_name_limit(processor->name_pool);
    for (size_t format_index = 0; format_index < formats.decimal_format_count;
         format_index += 1) {
        const sxml_decimal_format *format = &formats.decimal_formats[format_index];
        if (format->format_name.index >= name_limit || !sxml_decimal_format_is_valid(format)) {
            return false;
        }
        for (size_t earlier_index = 0; earlier_index < format_index; earlier_index += 1) {
            if (formats.decimal_formats[earlier_index].format_name.index ==
                format->format_name.index) {
                return false;
            }
        }
    }
    return true;
}

/* True when uri is a collation sxml implements or one of bindings. */
static bool sxml_collation_is_known(sxml_byte_view uri, sxml_collation_binding_view bindings) {
    for (size_t binding_index = 0; binding_index < bindings.collation_binding_count;
         binding_index += 1) {
        if (sxml_byte_view_equals(bindings.collation_bindings[binding_index].collation_uri,
                                  uri)) {
            return true;
        }
    }
    return sxml_collation_is_supported(uri);
}

/*
 * Checks the collations of a static context (XPath 4.0, section 2.2.1,
 * "statically known collations", "default collation"): bindings with
 * distinct absolute URIs sxml does not know already, each standing for a
 * supported collation; a default collation among the known ones.
 */
static bool sxml_collations_are_valid(const sxml_static_context_configuration *context) {
    const sxml_collation_binding_view bindings = context->collation_bindings;
    if (bindings.collation_binding_count > 0 && bindings.collation_bindings == NULL) {
        return false;
    }
    for (size_t binding_index = 0; binding_index < bindings.collation_binding_count;
         binding_index += 1) {
        const sxml_collation_binding *binding = &bindings.collation_bindings[binding_index];
        if (!sxml_text_is_valid(binding->collation_uri) ||
            !sxml_uri_has_scheme(binding->collation_uri) ||
            sxml_collation_is_supported(binding->collation_uri) ||
            !sxml_text_is_valid(binding->target_collation_uri) ||
            !sxml_collation_is_supported(binding->target_collation_uri)) {
            return false;
        }
        for (size_t earlier_index = 0; earlier_index < binding_index; earlier_index += 1) {
            if (sxml_byte_view_equals(bindings.collation_bindings[earlier_index].collation_uri,
                                      binding->collation_uri)) {
                return false;
            }
        }
    }
    return context->default_collation_uri.byte_count == 0 ||
           (sxml_text_is_valid(context->default_collation_uri) &&
            sxml_collation_is_known(context->default_collation_uri, bindings));
}

/* Checks the identifiers of the static context against the processor's
   name pool and type registry: values another processor issued would
   index past their tables (include/sxml/xpath.h, "Returns"). The rules
   lowering checks itself (distinct names, the xml prefix) are left to
   it. */
bool sxml_static_context_configuration_is_valid(
    const sxml_processor *processor, const sxml_static_context_configuration *context) {
    const uint32_t uri_limit = sxml_name_pool_namespace_uri_limit(processor->name_pool);
    const uint32_t prefix_limit = sxml_name_pool_namespace_prefix_limit(processor->name_pool);
    const uint32_t name_limit = sxml_name_pool_name_limit(processor->name_pool);
    const sxml_namespace_binding_view bindings = context->namespace_bindings;
    if (bindings.namespace_binding_count > 0 && bindings.namespace_bindings == NULL) {
        return false;
    }
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding *binding = &bindings.namespace_bindings[binding_index];
        if (binding->namespace_uri.index >= uri_limit ||
            binding->namespace_prefix.index >= prefix_limit) {
            return false;
        }
    }
    const sxml_variable_declaration_view declarations = context->variable_declarations;
    if (declarations.variable_declaration_count > 0 &&
        declarations.variable_declarations == NULL) {
        return false;
    }
    for (size_t declaration_index = 0;
         declaration_index < declarations.variable_declaration_count;
         declaration_index += 1) {
        const sxml_variable_declaration *declaration =
            &declarations.variable_declarations[declaration_index];
        sxml_type_description description;
        if (declaration->variable_name.index == 0 ||
            declaration->variable_name.index >= name_limit ||
            sxml_type_registry_describe_type(processor->type_registry,
                                             declaration->declared_type.item_type,
                                             &description) != SXML_STATUS_OK) {
            return false;
        }
    }
    /* The in-scope schemas are the processor's (include/sxml/xpath.h,
       sxml_schema_view). */
    const sxml_schema_view schemas = context->in_scope_schemas;
    if (schemas.schema_count > 0 && schemas.schemas == NULL) {
        return false;
    }
    for (size_t schema_index = 0; schema_index < schemas.schema_count; schema_index += 1) {
        if (schemas.schemas[schema_index] == NULL ||
            schemas.schemas[schema_index]->processor != processor) {
            return false;
        }
    }
    /* XPath 4.0, section 2.2.1: the static base URI is an absolute URI. */
    return (context->static_base_uri.byte_count == 0 ||
            sxml_uri_has_scheme(context->static_base_uri)) &&
           sxml_decimal_formats_are_valid(processor, context->decimal_formats) &&
           sxml_collations_are_valid(context);
}

sxml_xpath_static_context sxml_processor_static_context(
    sxml_processor *processor, const sxml_static_context_configuration *context) {
    return (sxml_xpath_static_context){
        .item_heap = processor->item_heap,
        .processor = processor,
        .allocator = processor->allocator,
        .namespace_bindings = context->namespace_bindings,
        .variable_declarations = context->variable_declarations,
        .function_table = sxml_processor_get_function_table(processor),
        .static_base_uri = context->static_base_uri,
        .unit_static_context = {.decimal_formats = context->decimal_formats,
                                .default_collation_uri = context->default_collation_uri,
                                .collation_bindings = context->collation_bindings,
                                .in_scope_schemas = context->in_scope_schemas}};
}

/* ------------------------------------------------------------ Expressions */

sxml_status sxml_xpath_expression_memory_requirements(
    const sxml_xpath_expression_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_xpath_expression),
                                               .alignment = alignof(sxml_xpath_expression)};
    return SXML_STATUS_OK;
}

sxml_status sxml_xpath_expression_initialize(
    sxml_memory_view memory, const sxml_xpath_expression_configuration *configuration,
    sxml_xpath_expression **expression) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_xpath_expression_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK &&
        !sxml_static_context_configuration_is_valid(configuration->processor,
                                      &configuration->static_context)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_processor *processor = configuration->processor;
    const sxml_byte_view static_base_uri = configuration->static_context.static_base_uri;
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
        memcpy(base_uri_memory, static_base_uri.bytes, static_base_uri.byte_count);
    }
    sxml_xpath_static_context static_context =
        sxml_processor_static_context(processor, &configuration->static_context);
    /* XPath only: XQuery has no XPath 1.0 compatibility mode. */
    static_context.has_xpath_1_compatibility_mode =
        configuration->static_context.has_xpath_1_compatibility_mode;
    sxml_ir_unit *unit = NULL;
    status = sxml_xpath_compile(
        &(sxml_xpath_compile_request){
            .expression_text = configuration->expression_text,
            .source_name = configuration->source_name,
            .static_context = &static_context,
            .maximum_expression_count = 0,
            .maximum_expression_depth = sxml_processor_get_maximum_expression_depth(processor),
            .error_record = configuration->error_record},
        &unit);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, base_uri_memory);
        return status;
    }
    void *placement = memory.bytes;
    sxml_xpath_expression *new_expression = placement;
    *new_expression = (sxml_xpath_expression){
        .processor = processor,
        .creating_allocator = {.state = NULL, .operations = NULL},
        .unit = unit,
        .static_base_uri_bytes = base_uri_memory,
        .static_base_uri_byte_count = static_base_uri.byte_count};
    *expression = new_expression;
    return SXML_STATUS_OK;
}

void sxml_xpath_expression_finalize(sxml_xpath_expression *expression) {
    if (expression == NULL) {
        return;
    }
    sxml_ir_unit_release(expression->unit);
    sxml_allocator_release(expression->processor->allocator,
                           expression->static_base_uri_bytes);
    expression->unit = NULL;
    expression->static_base_uri_bytes = NULL;
}

sxml_status sxml_xpath_expression_create(
    sxml_allocator allocator, const sxml_xpath_expression_configuration *configuration,
    sxml_xpath_expression **expression) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_xpath_expression_memory_requirements(configuration, &requirements);
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
    sxml_xpath_expression *new_expression = NULL;
    status = sxml_xpath_expression_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
        configuration, &new_expression);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_expression->creating_allocator = allocator;
    *expression = new_expression;
    return SXML_STATUS_OK;
}

void sxml_xpath_expression_destroy(sxml_xpath_expression *expression) {
    if (expression == NULL) {
        return;
    }
    const sxml_allocator allocator = expression->creating_allocator;
    sxml_xpath_expression_finalize(expression);
    sxml_allocator_release(allocator, expression);
}

/* ------------------------------------------------------------ Evaluation */

/* True for an ASCII letter. */
static bool sxml_byte_is_ascii_letter(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

/* True when text is an xs:language: [a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*
   (XSD 1.1 Part 2, section 3.4.3). */
static bool sxml_text_is_language(sxml_byte_view text) {
    size_t part_byte_count = 0;
    bool is_first_part = true;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '-') {
            if (part_byte_count == 0) {
                return false;
            }
            part_byte_count = 0;
            is_first_part = false;
            continue;
        }
        part_byte_count += 1;
        const bool is_allowed = sxml_byte_is_ascii_letter(byte) ||
                                (!is_first_part && byte >= '0' && byte <= '9');
        if (part_byte_count > 8 || !is_allowed) {
            return false;
        }
    }
    return part_byte_count > 0;
}

/* Checks the environment variables: UTF-8 names and values, names
   distinct (XPath 4.0, section 2.2.2: "the name is unique"). */
static bool sxml_environment_variables_are_valid(sxml_environment_variable_view variables) {
    if (variables.environment_variable_count > 0 && variables.environment_variables == NULL) {
        return false;
    }
    for (size_t variable_index = 0; variable_index < variables.environment_variable_count;
         variable_index += 1) {
        const sxml_environment_variable *variable =
            &variables.environment_variables[variable_index];
        if (!sxml_text_is_valid(variable->variable_name) ||
            !sxml_text_is_valid(variable->variable_value)) {
            return false;
        }
        for (size_t earlier_index = 0; earlier_index < variable_index; earlier_index += 1) {
            if (sxml_byte_view_equals(variables.environment_variables[earlier_index].variable_name,
                                      variable->variable_name)) {
                return false;
            }
        }
    }
    return true;
}

/* Checks what the dynamic context's rules forbid: duplicate variable and
   document names, an available document that is not a document node, a
   default language that is not an xs:language, environment variables
   that break the rules above. */
bool sxml_dynamic_context_configuration_is_valid(
    const sxml_dynamic_context_configuration *context) {
    if ((context->default_language.byte_count > 0 &&
         (context->default_language.bytes == NULL ||
          !sxml_text_is_language(context->default_language))) ||
        !sxml_environment_variables_are_valid(context->environment_variables)) {
        return false;
    }
    const sxml_variable_binding_view bindings = context->variable_bindings;
    if (bindings.variable_binding_count > 0 && bindings.variable_bindings == NULL) {
        return false;
    }
    for (size_t binding_index = 0; binding_index < bindings.variable_binding_count;
         binding_index += 1) {
        for (size_t earlier_index = 0; earlier_index < binding_index; earlier_index += 1) {
            if (bindings.variable_bindings[earlier_index].variable_name.index ==
                bindings.variable_bindings[binding_index].variable_name.index) {
                return false;
            }
        }
    }
    const sxml_available_document_view documents = context->available_documents;
    if (documents.available_document_count > 0 && documents.available_documents == NULL) {
        return false;
    }
    for (size_t document_index = 0; document_index < documents.available_document_count;
         document_index += 1) {
        const sxml_available_document *document = &documents.available_documents[document_index];
        sxml_node_reference node;
        sxml_node_description description;
        if (sxml_item_get_node(document->document_node, &node) != SXML_STATUS_OK ||
            sxml_node_describe(node, &description) != SXML_STATUS_OK ||
            description.node_kind != SXML_NODE_KIND_DOCUMENT) {
            return false;
        }
        for (size_t earlier_index = 0; earlier_index < document_index; earlier_index += 1) {
            if (sxml_byte_view_equals(
                    documents.available_documents[earlier_index].document_uri,
                    document->document_uri)) {
                return false;
            }
        }
    }
    return true;
}

void sxml_global_binding_fill(const sxml_global_binding *binding) {
    const uint32_t global_count = binding->unit->global_variable_count;
    for (uint32_t global_number = 1; global_number < global_count; global_number += 1) {
        const sxml_ir_global_variable *global = sxml_ir_unit_get_global_variable(
            binding->unit, (sxml_ir_global_index){.value = global_number});
        sxml_evaluator_slot slot = {.value = sxml_sequence_empty(),
                                    .slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT};
        /* The context global (name 0, D135) is no binding's. */
        for (size_t binding_index = 0; global->variable_name.index != 0 &&
                                       binding_index < binding->bindings->variable_binding_count;
             binding_index += 1) {
            const sxml_variable_binding *variable =
                &binding->bindings->variable_bindings[binding_index];
            if (variable->variable_name.index == global->variable_name.index) {
                slot = (sxml_evaluator_slot){.value = variable->value,
                                             .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
                break;
            }
        }
        binding->slots[global_number - 1] = slot;
    }
}

/* What one evaluation holds while it runs. */
typedef struct sxml_evaluation_resources {
    sxml_evaluator_slot *global_slots;
    size_t global_slot_count;
    sxml_processor_document_loader loader_memory;
    sxml_processor_document_loader *loader;
    sxml_dynamic_context *dynamic_context;
} sxml_evaluation_resources;

static void sxml_evaluation_resources_release(const sxml_processor *processor,
                                              sxml_evaluation_resources *resources) {
    sxml_dynamic_context_destroy(resources->dynamic_context);
    sxml_processor_document_loader_finalize(resources->loader);
    sxml_allocator_release(processor->allocator, resources->global_slots);
}

/* Sets up the global slots, the loader and the dynamic context. */
static sxml_status sxml_evaluation_resources_acquire(const sxml_unit_evaluation *evaluation,
                                                     sxml_evaluation_resources *resources) {
    sxml_processor *processor = evaluation->processor;
    const sxml_dynamic_context_configuration *context = evaluation->dynamic_context;
    const sxml_ir_unit *unit = evaluation->unit;
    sxml_status status = SXML_STATUS_OK;
    if (unit->global_variable_count > 1) {
        resources->global_slot_count = unit->global_variable_count - 1U;
        size_t byte_count = 0;
        status = sxml_size_multiply(resources->global_slot_count, sizeof(sxml_evaluator_slot),
                                    &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                processor->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_evaluator_slot)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        resources->global_slots = memory;
        sxml_global_binding_fill(&(sxml_global_binding){
            .unit = unit, .bindings = &context->variable_bindings, .slots = memory});
    }
    const sxml_unit_evaluation_extension no_extension = {
        .function_table = {.state = NULL, .operations = NULL},
        .document_preparation = {.state = NULL, .operations = NULL},
        .result_writer = NULL,
        .principal_result_builder = {.state = NULL, .operations = NULL},
        .principal_result_is_raw = NULL};
    const sxml_unit_evaluation_extension *extension =
        evaluation->extension == NULL ? &no_extension : evaluation->extension;
    status = sxml_processor_document_loader_initialize(
        (sxml_memory_view){.bytes = (uint8_t *)&resources->loader_memory,
                           .byte_count = sizeof(resources->loader_memory)},
        &(sxml_processor_document_loader_configuration){
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
                 .environment_variables = context->environment_variables,
                 .result_writer = extension->result_writer,
                 .principal_result_builder = extension->principal_result_builder,
                 .principal_result_is_raw = extension->principal_result_is_raw},
            .available_documents = context->available_documents,
            .document_preparation = extension->document_preparation},
        &resources->loader);
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_create(
            processor->allocator,
            &(sxml_evaluator_configuration){
                .item_heap = processor->item_heap,
                .allocator = processor->allocator,
                .document_loader =
                    sxml_processor_document_loader_as_document_loader(resources->loader),
                .clock = context->clock.operations == NULL ? processor->clock : context->clock,
                .function_table = extension->function_table.operations == NULL
                                      ? sxml_processor_get_function_table(processor)
                                      : extension->function_table,
                .maximum_evaluation_depth =
                    sxml_processor_get_maximum_evaluation_depth(processor),
                .maximum_frame_slot_count = 0,
                .maximum_evaluation_step_count =
                    sxml_processor_get_maximum_evaluation_step_count(processor)},
            &resources->dynamic_context);
    }
    if (status != SXML_STATUS_OK) {
        sxml_evaluation_resources_release(processor, resources);
    }
    return status;
}

sxml_status sxml_evaluate_unit(const sxml_unit_evaluation *evaluation, sxml_sequence *result) {
    sxml_evaluation_resources resources;
    memset(&resources, 0, sizeof(resources));
    sxml_status status = sxml_evaluation_resources_acquire(evaluation, &resources);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_dynamic_context_configuration *context = evaluation->dynamic_context;
    sxml_sequence value = sxml_sequence_empty();
    status = sxml_evaluator_evaluate(
        resources.dynamic_context,
        &(sxml_evaluation_request){
            .unit = evaluation->unit,
            .has_context_value = context->has_context_value,
            .context_value = context->context_value,
            .global_values = {.slots = resources.global_slots,
                              .slot_count = resources.global_slot_count},
            .static_base_uri = evaluation->static_base_uri,
            .error_record = evaluation->error_record},
        &value);
    sxml_evaluation_resources_release(evaluation->processor, &resources);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = value;
    return SXML_STATUS_OK;
}

sxml_status sxml_xpath_expression_evaluate(const sxml_xpath_expression *expression,
                                           const sxml_xpath_evaluation_request *request,
                                           sxml_sequence *result) {
    if (expression == NULL || expression->unit == NULL || request == NULL ||
        !sxml_dynamic_context_configuration_is_valid(&request->dynamic_context)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_evaluate_unit(
        &(sxml_unit_evaluation){
            .processor = expression->processor,
            .unit = expression->unit,
            .static_base_uri = {.bytes = expression->static_base_uri_bytes,
                                .byte_count = expression->static_base_uri_byte_count},
            .dynamic_context = &request->dynamic_context,
            .error_record = request->error_record},
        result);
}

/* ---------------------------------------------------------- Sequence types */

sxml_status sxml_xpath_parse_sequence_type(const sxml_sequence_type_parse_request *request,
                                           sxml_sequence_type *sequence_type) {
    if (request == NULL || request->processor == NULL ||
        !sxml_static_context_configuration_is_valid(request->processor, &request->static_context)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = request->processor;
    sxml_ast *ast = NULL;
    sxml_status status = sxml_xpath_build_sequence_type_ast(
        &(sxml_ast_build_request){
            .expression_text = request->sequence_type_text,
            .source_name = request->source_name,
            .allocator = processor->allocator,
            .maximum_expression_depth = sxml_processor_get_maximum_expression_depth(processor),
            .error_record = request->error_record},
        &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Only the namespace bindings are used (include/sxml/xpath.h). */
    const sxml_static_context_configuration namespaces_only = {
        .namespace_bindings = request->static_context.namespace_bindings,
        .static_base_uri = {.bytes = NULL, .byte_count = 0},
        .variable_declarations = {.variable_declarations = NULL,
                                  .variable_declaration_count = 0}};
    const sxml_xpath_static_context static_context =
        sxml_processor_static_context(processor, &namespaces_only);
    sxml_sequence_type resolved;
    status = sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = ast,
                                   .type_node = ast->root,
                                   .static_context = &static_context,
                                   .error_record = request->error_record},
        &resolved);
    sxml_ast_destroy(ast);
    if (status == SXML_STATUS_OK) {
        *sequence_type = resolved;
    }
    return status;
}
