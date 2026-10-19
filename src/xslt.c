/*
 * XSLT 4.0 through the public API: compiled stylesheets and their
 * transformations (include/sxml/xslt.h; docs/DECISIONS.md, D112, D157).
 *
 * Golden file: src/xquery.c (layout, lifecycle, comments), whose context
 * checks and unit evaluation this file shares (src/xpath_api_internal.h):
 * a stylesheet is lowered into an IR unit by the one lowering machine
 * (src/xslt_lower_internal.h) and a transformation evaluates that unit
 * with the code that evaluates an XPath expression or an XQuery query.
 * This file only puts the pieces of the earlier XSLT packets together:
 *
 * - Compiling: the principal module (bytes, or asked of the resolver as
 *   an XSLT_MODULE or XSLT_PACKAGE, 3.4.3) is read with the static
 *   parameters into a stylesheet AST (sxml_xslt_read_stylesheet, D139),
 *   lowered for an apply-templates invocation in the default mode (D144),
 *   which finds every static error of the lowering, and its outputs
 *   compiled (sxml_xslt_stylesheet_outputs, D156). The AST is kept: an
 *   invocation other than that one lowers its own unit from it when a
 *   transformation asks for it (the main function is the invocation,
 *   D144), so the compiled unit serves the common case and nothing is
 *   evaluated at compile time but static expressions.
 * - Priming (2.4.2): the global context item is stripped of the
 *   whitespace the stylesheet strips (sxml_xslt_strip_document, 4.2.2,
 *   D150); stylesheet and static parameters are bound to the unit's
 *   global slots by name (the evaluator coerces them, D150); XTDE0050 is
 *   raised by the initializer of a required parameter without a value.
 * - Evaluating: the dynamic context gets a function table over the XSLT
 *   library parts (fn:function-lookup and function-available see the XSLT
 *   functions, D149's Proposed item), a document preparation that strips
 *   documents fn:doc and document() read (D150) and the transformation's
 *   result writer (D156) in its evaluation environment.
 * - Delivering: the raw result goes to the result writer, which
 *   delivers it as the request asks after checking XTDE1490
 *   (sxml_xslt_result_writer_deliver_principal_result).
 *
 * Section numbers are of XSLT 4.0 unless another specification is named.
 */
#include "sxml/xslt.h"

#include "xslt_transform_internal.h"

#include "function_library_internal.h"
#include "memory_internal.h"
#include "function_library_parts_internal.h"
#include "processor_internal.h"
#include "xdm_internal.h"
#include "xpath_api_internal.h"
#include "xslt_ast_internal.h"
#include "xslt_lower_internal.h"
#include "xslt_output_internal.h"
#include "xslt_read_internal.h"
#include "xslt_schema_internal.h"
#include "xslt_streamability_internal.h"
#include "xslt_whitespace_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

struct sxml_xslt_stylesheet {
    sxml_processor *processor;
    /* The allocator of sxml_xslt_stylesheet_create, which destroy frees the
       stylesheet with; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    /* Sealed; owned. Kept for invocations the compiled unit is not. */
    sxml_xslt_ast *ast;
    /* Sealed; one reference: apply-templates in the default mode. */
    sxml_ir_unit *unit;
    /* Owned (D156). */
    sxml_xslt_stylesheet_outputs *outputs;
    /* The XSLT library parts, the dynamic context's function table. */
    sxml_function_library *function_library;
    /* The static parameters given at compile time, bound to their globals
       in every transformation (their initializers recompute nothing then,
       D145); names and values retained, in the processor's allocator. */
    sxml_variable_binding *static_parameters;
    size_t static_parameter_count;
    /* Over ast; its address is the document preparation's state. */
    sxml_xslt_whitespace_stripping stripping;
    /* The principal module's version is 1.0 (26.1.2). */
    bool prefers_xml_for_xhtml;
};

static const sxml_byte_view no_text = {.bytes = NULL, .byte_count = 0};

/* ------------------------------------------------------------- Errors */

/* Fills error_record (may be NULL) with an XSLT error code and returns
   status. */
typedef struct sxml_xslt_error {
    sxml_status status;
    sxml_byte_view code_local_name;
    sxml_byte_view description;
    sxml_byte_view source_name;
} sxml_xslt_error;

static sxml_status sxml_xslt_report(const sxml_xslt_error *error,
                                    sxml_error_record *error_record) {
    if (error_record != NULL) {
        memset(error_record, 0, sizeof(*error_record));
        error_record->code =
            (sxml_error_code){.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                              .local_name = error->code_local_name,
                              .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")};
        error_record->diagnostic.description = error->description;
        error_record->diagnostic.location.source_name = error->source_name;
    }
    return error->status;
}

/*
 * After a failed compilation the AST is gone, and with it the module URIs
 * error locations borrow (the reader's too): a location is given the
 * caller's name for the principal module (the line and column stay, and
 * an error in another module is reported under the principal module's
 * name).
 */
static void sxml_xslt_detach_location(sxml_byte_view caller_name,
                                      sxml_error_record *error_record) {
    if (error_record != NULL &&
        error_record->diagnostic.location.source_name.byte_count != 0) {
        error_record->diagnostic.location.source_name = caller_name;
    }
}

/* ------------------------------------------------------ Principal module */

/* The principal module as the reader takes it: its bytes and its URI. */
typedef struct sxml_principal_module {
    sxml_byte_view text;
    sxml_byte_view module_uri;
    /* What the resolver delivered, released after reading. */
    sxml_resource resource;
    bool is_resolved;
    /* The absolute URI of a RESOLVED_MODULE request; owned. */
    sxml_uri_resolution resolution;
} sxml_principal_module;

static void sxml_principal_module_release(const sxml_processor *processor,
                                          sxml_principal_module *module) {
    const sxml_resource_resolver resolver = processor->resource_resolver;
    if (module->is_resolved && resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &module->resource);
    }
    module->is_resolved = false;
    sxml_allocator_release(processor->allocator, module->resolution.memory);
    module->resolution.memory = NULL;
}

/*
 * Asks the processor's resolver for the principal module: one request
 * of kind XSLT_MODULE (the reference resolved against the static base URI,
 * 3.10.1) or XSLT_PACKAGE (the name and package-version, 3.4.3). A module
 * not delivered is XTSE0165, a package XTSE3000; a form other than bytes
 * is an invalid argument (include/sxml/xslt.h).
 */
static sxml_status sxml_principal_module_resolve(
    const sxml_xslt_stylesheet_configuration *configuration, sxml_principal_module *module) {
    sxml_processor *processor = configuration->processor;
    const bool is_package = configuration->principal_module_source ==
                            SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE;
    const sxml_byte_view reference = configuration->principal_module_uri_reference;
    sxml_byte_view absolute_uri = reference;
    sxml_status status = SXML_STATUS_OK;
    if (!is_package) {
        status = sxml_uri_resolve(
            &(sxml_uri_resolution_request){.base_uri =
                                               configuration->static_context.static_base_uri,
                                           .reference = reference,
                                           .allocator = processor->allocator},
            &module->resolution);
        absolute_uri = module->resolution.uri;
    }
    const sxml_resource_resolver resolver = processor->resource_resolver;
    memset(&module->resource, 0, sizeof(module->resource));
    if (status == SXML_STATUS_OK && resolver.operations != NULL &&
        resolver.operations->resolve != NULL) {
        status = resolver.operations->resolve(
            resolver.state,
            &(sxml_resource_request){
                .resource_kind = is_package ? SXML_RESOURCE_KIND_XSLT_PACKAGE
                                            : SXML_RESOURCE_KIND_XSLT_MODULE,
                .uri_reference = reference,
                .absolute_uri = absolute_uri,
                .processor = processor,
                .encoding_name = no_text,
                .is_default_collection = false,
                .target_namespace_uri = no_text,
                .package_version = !is_package ? no_text
                                   : configuration->package_version.byte_count == 0
                                       ? SXML_BYTE_VIEW_FROM_LITERAL("*")
                                       : configuration->package_version},
            &module->resource);
        module->is_resolved = status == SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (module->resource.resource_form == SXML_RESOURCE_FORM_BYTES) {
        module->text = module->resource.content;
        module->module_uri = module->resource.resource_uri.byte_count > 0
                                 ? module->resource.resource_uri
                                 : absolute_uri;
        return SXML_STATUS_OK;
    }
    if (module->resource.resource_form != SXML_RESOURCE_FORM_NONE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_xslt_report(
        &(sxml_xslt_error){
            .status = SXML_STATUS_STATIC_ERROR,
            .code_local_name = is_package ? SXML_BYTE_VIEW_FROM_LITERAL("XTSE3000")
                                          : SXML_BYTE_VIEW_FROM_LITERAL("XTSE0165"),
            .description = is_package
                               ? SXML_BYTE_VIEW_FROM_LITERAL(
                                     "the resolver does not deliver the principal package "
                                     "(XSLT 4.0, 3.4.3)")
                               : SXML_BYTE_VIEW_FROM_LITERAL(
                                     "the resolver does not deliver the principal stylesheet "
                                     "module (XSLT 4.0, 3.10.1)"),
            .source_name = reference},
        configuration->error_record);
}

/* The principal module of a configuration: its text, or what the resolver
   delivers. */
static sxml_status sxml_principal_module_find(
    const sxml_xslt_stylesheet_configuration *configuration, sxml_principal_module *module) {
    if (configuration->principal_module_source == SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT) {
        module->text = configuration->principal_module_text;
        module->module_uri = configuration->static_context.static_base_uri.byte_count > 0
                                 ? configuration->static_context.static_base_uri
                                 : configuration->source_name;
        return SXML_STATUS_OK;
    }
    return sxml_principal_module_resolve(configuration, module);
}

/* --------------------------------------------------------------- Lowering */

/* An invocation of a transformation, as the lowering takes it. */
typedef struct sxml_unit_invocation {
    const sxml_xslt_stylesheet *stylesheet;
    sxml_name_identifier initial_template;
    sxml_name_identifier initial_mode;
    sxml_name_identifier initial_function;
    uint32_t initial_function_arity;
    bool applies_templates;
    bool selects_unnamed_mode;
    bool has_initial_match_selection;
    /* D228: the initial match selection is a streamed principal source. */
    bool has_streamed_source;
    /* D231: the principal result goes to the principal stream. */
    bool streams_result_document;
    sxml_error_record *error_record;
} sxml_unit_invocation;

/* xsl:initial-template, interned (2.4.4: the default initial template). */
static sxml_status sxml_initial_template_name(sxml_processor *processor,
                                              sxml_name_identifier *name) {
    return sxml_name_pool_intern_name(
        sxml_item_heap_get_name_pool(processor->item_heap),
        &(sxml_name_request){.namespace_uri =
                                 sxml_well_known_namespace_identifier(SXML_NAMESPACE_XSLT),
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("initial-template")},
        name);
}

/* Lowers the stylesheet for one invocation into a sealed unit, owned. The
   main function returns the raw result: the result writer delivers it. */
static sxml_status sxml_xslt_lower_invocation(const sxml_unit_invocation *invocation,
                                              sxml_ir_unit **unit) {
    const sxml_xslt_stylesheet *stylesheet = invocation->stylesheet;
    return sxml_xslt_lower_initial_template(
        &(sxml_xslt_lowering_request){.ast = stylesheet->ast,
                                      .processor = stylesheet->processor,
                                      .template_node = {.value = 0},
                                      .initial_template = invocation->initial_template,
                                      .applies_templates = invocation->applies_templates,
                                      .initial_mode = invocation->initial_mode,
                                      .selects_unnamed_mode = invocation->selects_unnamed_mode,
                                      .initial_function = invocation->initial_function,
                                      .initial_function_arity =
                                          invocation->initial_function_arity,
                                      .has_initial_match_selection =
                                          invocation->has_initial_match_selection,
                                      .has_streamed_source = invocation->has_streamed_source,
                                      .reads_invocation_parameters = true,
                                      .builds_result_document = false,
                                      .streams_result_document =
                                          invocation->streams_result_document,
                                      .maximum_expression_count = 0,
                                      .error_record = invocation->error_record},
        unit);
}

/* ------------------------------------------------------------- Compiling */

/* Copies the static parameters, retaining their values. */
static sxml_status sxml_static_parameters_keep(sxml_processor *processor,
                                               sxml_variable_binding_view parameters,
                                               sxml_xslt_stylesheet *stylesheet) {
    if (parameters.variable_binding_count == 0) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        processor->allocator,
        (sxml_array_allocation_request){.element_count = parameters.variable_binding_count,
                                        .element_size = sizeof(sxml_variable_binding),
                                        .element_alignment = alignof(sxml_variable_binding)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_variable_binding *kept = memory;
    for (size_t binding_index = 0; binding_index < parameters.variable_binding_count;
         binding_index += 1) {
        kept[binding_index] = parameters.variable_bindings[binding_index];
        sxml_sequence_retain(kept[binding_index].value);
    }
    stylesheet->static_parameters = kept;
    stylesheet->static_parameter_count = parameters.variable_binding_count;
    return SXML_STATUS_OK;
}

/* Releases what a stylesheet holds; every member may be NULL. */
static void sxml_stylesheet_release(sxml_xslt_stylesheet *stylesheet) {
    sxml_processor *processor = stylesheet->processor;
    for (size_t binding_index = 0; binding_index < stylesheet->static_parameter_count;
         binding_index += 1) {
        sxml_sequence_release(stylesheet->static_parameters[binding_index].value);
    }
    sxml_allocator_release(processor->allocator, stylesheet->static_parameters);
    stylesheet->static_parameters = NULL;
    stylesheet->static_parameter_count = 0;
    sxml_function_library_destroy(stylesheet->function_library);
    stylesheet->function_library = NULL;
    sxml_xslt_stylesheet_outputs_destroy(stylesheet->outputs);
    stylesheet->outputs = NULL;
    sxml_ir_unit_release(stylesheet->unit);
    stylesheet->unit = NULL;
    sxml_xslt_ast_destroy(stylesheet->ast);
    stylesheet->ast = NULL;
}

/* 3.4.3: a principal module asked for as a package must be one: an
   xsl:package, or an xsl:stylesheet or xsl:transform, which 3.4 makes an
   implicit package (D160); a simplified module is none. */
static sxml_status sxml_check_package(const sxml_xslt_stylesheet_configuration *configuration,
                                      const sxml_xslt_ast *ast) {
    if (configuration->principal_module_source !=
        SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_module *principal =
        sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = 1});
    const sxml_xslt_ast_kind root_kind = sxml_xslt_ast_get_node(ast, principal->root)->kind;
    if ((root_kind == SXML_XSLT_AST_KIND_PACKAGE || root_kind == SXML_XSLT_AST_KIND_STYLESHEET) &&
        !principal->is_simplified) {
        return SXML_STATUS_OK;
    }
    return sxml_xslt_report(
        &(sxml_xslt_error){.status = SXML_STATUS_STATIC_ERROR,
                           .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XTSE3000"),
                           .description = SXML_BYTE_VIEW_FROM_LITERAL(
                               "the principal package is a simplified stylesheet module, no "
                               "package (XSLT 4.0, 3.4.3)"),
                           .source_name = configuration->principal_module_uri_reference},
        configuration->error_record);
}

/* Reads, lowers and compiles the outputs of the stylesheet into *compiled
   (zeroed by the caller); on failure what was made is released. */
static sxml_status sxml_xslt_compile(const sxml_xslt_stylesheet_configuration *configuration,
                                     sxml_xslt_stylesheet *compiled) {
    sxml_processor *processor = configuration->processor;
    sxml_principal_module module;
    memset(&module, 0, sizeof(module));
    sxml_status status = sxml_principal_module_find(configuration, &module);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_read_stylesheet(
            &(sxml_xslt_read_request){
                .processor = processor,
                .principal_module_text = module.text,
                .principal_module_uri = module.module_uri,
                .static_parameters = configuration->static_parameters,
                .default_collation_uri = configuration->static_context.default_collation_uri,
                .resource_resolver = {.state = NULL, .operations = NULL},
                .error_record = configuration->error_record},
            &compiled->ast);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_check_package(configuration, compiled->ast);
    }
    if (status == SXML_STATUS_OK) {
        /* 3.13: the in-scope schemas, before anything resolves a type. */
        status = sxml_xslt_import_schemas(&(sxml_xslt_schema_request){
            .ast = compiled->ast,
            .processor = processor,
            .host_schemas = configuration->static_context.in_scope_schemas,
            .error_record = configuration->error_record});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lower_invocation(
            &(sxml_unit_invocation){.stylesheet = compiled,
                                    .initial_template = {.index = 0},
                                    .initial_mode = {.index = 0},
                                    .initial_function = {.index = 0},
                                    .initial_function_arity = 0,
                                    .applies_templates = true,
                                    .selects_unnamed_mode = false,
                                    .has_initial_match_selection = false,
                                    .error_record = configuration->error_record},
            &compiled->unit);
    }
    if (status == SXML_STATUS_OK && !configuration->falls_back_to_unstreamed) {
        /* XSLT 4.0, 19.10: XTSE3430 for a construct declared streamable
           that is not guaranteed-streamable (packet 7.6a), after the
           static errors lowering finds, unless the user chose processing
           without streaming instead (D219). */
        status = sxml_xslt_check_streamability(&(sxml_xslt_streamability_request){
            .ast = compiled->ast,
            .allocator = processor->allocator,
            .name_pool = sxml_item_heap_get_name_pool(processor->item_heap),
            .maximum_frame_count = 0,
            .error_record = configuration->error_record});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_stylesheet_outputs_create(
            processor->allocator,
            &(sxml_xslt_stylesheet_outputs_configuration){
                .ast = compiled->ast,
                .processor = processor,
                .error_record = configuration->error_record},
            &compiled->outputs);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_create(
            processor->allocator,
            &(sxml_function_library_configuration){
                .allocator = processor->allocator,
                .item_heap = processor->item_heap,
                .parts = *sxml_function_library_xslt_parts()},
            &compiled->function_library);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_static_parameters_keep(processor, configuration->static_parameters,
                                             compiled);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_xslt_ast_node *root = sxml_xslt_ast_get_node(
            compiled->ast,
            sxml_xslt_ast_get_module(compiled->ast, (sxml_xslt_ast_module_index){.value = 1})
                ->root);
        compiled->prefers_xml_for_xhtml =
            sxml_xslt_ast_get_static_context(compiled->ast, root->static_context)->version <
            2.0;
    }
    if (status != SXML_STATUS_OK) {
        sxml_xslt_detach_location(
            configuration->principal_module_source == SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT
                ? (configuration->source_name.byte_count > 0
                       ? configuration->source_name
                       : configuration->static_context.static_base_uri)
                : configuration->principal_module_uri_reference,
            configuration->error_record);
        sxml_stylesheet_release(compiled);
    }
    sxml_principal_module_release(processor, &module);
    return status;
}

/* ------------------------------------------------------------ Stylesheet */

/* The static context rules of include/sxml/xslt.h: the stylesheet
   declares its namespaces, variables and decimal formats itself. */
static bool sxml_xslt_configuration_is_valid(
    const sxml_xslt_stylesheet_configuration *configuration) {
    const sxml_static_context_configuration *context = &configuration->static_context;
    if (context->namespace_bindings.namespace_binding_count > 0 ||
        context->variable_declarations.variable_declaration_count > 0 ||
        context->decimal_formats.decimal_format_count > 0 ||
        !sxml_static_context_configuration_is_valid(configuration->processor, context)) {
        return false;
    }
    switch (configuration->principal_module_source) {
    case SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT:
    case SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_MODULE:
    case SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE:
        break;
    default:
        return false;
    }
    const sxml_variable_binding_view parameters = configuration->static_parameters;
    if (parameters.variable_binding_count > 0 && parameters.variable_bindings == NULL) {
        return false;
    }
    for (size_t binding_index = 0; binding_index < parameters.variable_binding_count;
         binding_index += 1) {
        for (size_t earlier_index = 0; earlier_index < binding_index; earlier_index += 1) {
            if (parameters.variable_bindings[earlier_index].variable_name.index ==
                parameters.variable_bindings[binding_index].variable_name.index) {
                return false;
            }
        }
    }
    return true;
}

sxml_status sxml_xslt_stylesheet_memory_requirements(
    const sxml_xslt_stylesheet_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->processor == NULL || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_xslt_stylesheet),
                                               .alignment = alignof(sxml_xslt_stylesheet)};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_stylesheet_initialize(
    sxml_memory_view memory, const sxml_xslt_stylesheet_configuration *configuration,
    sxml_xslt_stylesheet **stylesheet) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_xslt_stylesheet_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK &&
        (stylesheet == NULL || !sxml_xslt_configuration_is_valid(configuration))) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_stylesheet compiled;
    memset(&compiled, 0, sizeof(compiled));
    compiled.processor = configuration->processor;
    status = sxml_xslt_compile(configuration, &compiled);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_xslt_stylesheet *new_stylesheet = placement;
    *new_stylesheet = compiled;
    /* The stripping's address is the document preparation's state. */
    new_stylesheet->stripping = (sxml_xslt_whitespace_stripping){
        .ast = new_stylesheet->ast, .processor = new_stylesheet->processor};
    *stylesheet = new_stylesheet;
    return SXML_STATUS_OK;
}

void sxml_xslt_stylesheet_finalize(sxml_xslt_stylesheet *stylesheet) {
    if (stylesheet == NULL) {
        return;
    }
    sxml_stylesheet_release(stylesheet);
}

sxml_status sxml_xslt_stylesheet_create(sxml_allocator allocator,
                                        const sxml_xslt_stylesheet_configuration *configuration,
                                        sxml_xslt_stylesheet **stylesheet) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_xslt_stylesheet_memory_requirements(configuration, &requirements);
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
    sxml_xslt_stylesheet *new_stylesheet = NULL;
    status = sxml_xslt_stylesheet_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
        configuration, &new_stylesheet);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_stylesheet->creating_allocator = allocator;
    *stylesheet = new_stylesheet;
    return SXML_STATUS_OK;
}

void sxml_xslt_stylesheet_destroy(sxml_xslt_stylesheet *stylesheet) {
    if (stylesheet == NULL) {
        return;
    }
    const sxml_allocator allocator = stylesheet->creating_allocator;
    sxml_xslt_stylesheet_finalize(stylesheet);
    sxml_allocator_release(allocator, stylesheet);
}

sxml_status sxml_xslt_stylesheet_get_output_definition(
    const sxml_xslt_stylesheet *stylesheet, sxml_name_identifier output_definition_name,
    sxml_xslt_output_definition *output_definition) {
    if (stylesheet == NULL || stylesheet->outputs == NULL || output_definition == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_xslt_stylesheet_outputs_get_definition(
        stylesheet->outputs, output_definition_name, output_definition);
}

/* -------------------------------------------------------- Transformation */

/* The request rules of include/sxml/xslt.h that are not the dynamic
   context's. */
static bool sxml_transformation_request_is_valid(const sxml_xslt_transformation_request *request) {
    switch (request->invocation_kind) {
    case SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES:
    case SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE:
    case SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION:
        break;
    default:
        return false;
    }
    switch (request->result_delivery) {
    case SXML_XSLT_RESULT_DELIVERY_DEFAULT:
    case SXML_XSLT_RESULT_DELIVERY_RAW:
    case SXML_XSLT_RESULT_DELIVERY_TREE:
        break;
    case SXML_XSLT_RESULT_DELIVERY_SERIALIZED:
        if (request->output_stream.operations == NULL) {
            return false;
        }
        break;
    default:
        return false;
    }
    const bool applies_templates =
        request->invocation_kind == SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES;
    if (!applies_templates &&
        (request->has_initial_match_selection || request->selects_unnamed_mode ||
         request->initial_mode_name.index != 0)) {
        return false;
    }
    if (request->selects_unnamed_mode && request->initial_mode_name.index != 0) {
        return false;
    }
    if (request->streamed_source_uri.byte_count > 0 &&
        (!applies_templates || request->has_initial_match_selection ||
         request->streamed_source_uri.bytes == NULL)) {
        /* D228: a streamed principal source is the initial match
           selection of an apply-templates invocation. */
        return false;
    }
    if (request->invocation_kind != SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE &&
        request->initial_template_name.index != 0) {
        return false;
    }
    if (request->invocation_kind != SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION &&
        (request->initial_function_name.index != 0 ||
         request->function_arguments.sequence_count > 0)) {
        return false;
    }
    if (request->invocation_kind == SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION &&
        (request->initial_function_name.index == 0 ||
         request->template_parameters.variable_binding_count > 0 ||
         request->tunnel_parameters.variable_binding_count > 0)) {
        return false;
    }
    if (request->dynamic_context.has_context_value) {
        uint64_t item_count = 0;
        if (sxml_sequence_count(request->dynamic_context.context_value, NULL, &item_count) !=
                SXML_STATUS_OK ||
            item_count != 1) {
            return false;
        }
    }
    return true;
}

/* The most invocation values a transformation binds: two parameter maps
   and an initial match selection or a streamed principal source's URI
   (D228), or the arguments' array. */
enum { SXML_INVOCATION_VALUE_COUNT = 3 };

/* What one transformation holds while it runs; released at its end. */
typedef struct sxml_transformation {
    const sxml_xslt_stylesheet *stylesheet;
    const sxml_xslt_transformation_request *request;
    /* The stylesheet's unit, or one lowered for this invocation (owned:
       is_unit_owned). */
    sxml_ir_unit *unit;
    bool is_unit_owned;
    /* The context value: the stripped global context item, or the initial
       match selection without one. Owned. */
    sxml_sequence context_value;
    bool has_context_value;
    /* The stylesheet's static parameters, then the request's stylesheet
       parameters that do not repeat their names. */
    sxml_variable_binding *bindings;
    size_t binding_count;
    /* What sxml_transformation_bind_invocation_values made; owned. */
    sxml_sequence invocation_values[SXML_INVOCATION_VALUE_COUNT];
    size_t invocation_value_count;
    sxml_xslt_result_writer *writer;
    /* The request's available documents, stripped as the stylesheet
       strips source documents (4.2.2); owned, NULL when they need no
       copy. */
    sxml_available_document *available_documents;
    size_t available_document_count;
    /* The principal result goes to the writer's principal stream as it is
       made (D231). */
    bool streams_result_document;
} sxml_transformation;

/* The source name of an error's location in memory of the stylesheet:
   a unit lowered for one invocation is released when the transformation
   ends, and the error record must stay valid until the stylesheet is next
   used (include/sxml/status.h, D21). Module URIs never move (D102). */
static void sxml_transformation_keep_error_location(const sxml_transformation *transformation) {
    sxml_error_record *record = transformation->request->error_record;
    if (record == NULL || !transformation->is_unit_owned) {
        return;
    }
    sxml_byte_view *source_name = &record->diagnostic.location.source_name;
    if (source_name->byte_count == 0) {
        return;
    }
    const sxml_xslt_stylesheet *stylesheet = transformation->stylesheet;
    if (stylesheet->unit != NULL && sxml_byte_view_equals(*source_name, stylesheet->unit->source_name)) {
        *source_name = stylesheet->unit->source_name;
        return;
    }
    const sxml_xslt_ast *ast = stylesheet->ast;
    for (uint32_t module_index = 1; module_index < ast->module_count; module_index += 1) {
        const sxml_byte_view module_uri =
            sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = module_index})
                ->module_uri;
        if (sxml_byte_view_equals(*source_name, module_uri)) {
            *source_name = module_uri;
            return;
        }
    }
    *source_name = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
}

static void sxml_transformation_release(sxml_transformation *transformation) {
    sxml_transformation_keep_error_location(transformation);
    sxml_processor *processor = transformation->stylesheet->processor;
    sxml_xslt_result_writer_destroy(transformation->writer);
    transformation->writer = NULL;
    sxml_allocator_release(processor->allocator, transformation->bindings);
    transformation->bindings = NULL;
    for (size_t value_index = 0; value_index < transformation->invocation_value_count;
         value_index += 1) {
        sxml_sequence_release(transformation->invocation_values[value_index]);
    }
    transformation->invocation_value_count = 0;
    if (transformation->has_context_value) {
        sxml_sequence_release(transformation->context_value);
        transformation->has_context_value = false;
    }
    if (transformation->is_unit_owned) {
        sxml_ir_unit_release(transformation->unit);
    }
    transformation->unit = NULL;
    for (size_t document_index = 0; document_index < transformation->available_document_count;
         document_index += 1) {
        sxml_item_release(transformation->available_documents[document_index].document_node);
    }
    sxml_allocator_release(processor->allocator, transformation->available_documents);
    transformation->available_documents = NULL;
    transformation->available_document_count = 0;
}

/* The unit of the request's invocation: the compiled one for
   apply-templates in the default mode, else one lowered now. */
static sxml_status sxml_transformation_choose_unit(sxml_transformation *transformation) {
    const sxml_xslt_transformation_request *request = transformation->request;
    const sxml_xslt_stylesheet *stylesheet = transformation->stylesheet;
    const bool calls_function =
        request->invocation_kind == SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION;
    const bool applies_templates =
        request->invocation_kind == SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES;
    const bool has_streamed_source = request->streamed_source_uri.byte_count > 0;
    if (applies_templates && request->initial_mode_name.index == 0 &&
        !request->selects_unnamed_mode && !request->has_initial_match_selection &&
        !has_streamed_source) {
        transformation->unit = stylesheet->unit;
        return SXML_STATUS_OK;
    }
    if (calls_function && request->function_arguments.sequence_count > UINT32_MAX) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_unit_invocation invocation = {
        .stylesheet = stylesheet,
        .initial_template = request->initial_template_name,
        .initial_mode = request->initial_mode_name,
        .initial_function = request->initial_function_name,
        .initial_function_arity = (uint32_t)request->function_arguments.sequence_count,
        /* A function call applies no templates, but chooses no initial
           template either. */
        .applies_templates = applies_templates || calls_function,
        .selects_unnamed_mode = request->selects_unnamed_mode,
        .has_initial_match_selection = request->has_initial_match_selection,
        .has_streamed_source = has_streamed_source,
        .streams_result_document = transformation->streams_result_document,
        .error_record = request->error_record};
    if (request->invocation_kind == SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE &&
        invocation.initial_template.index == 0) {
        /* 2.4.4: XTDE0040 when the stylesheet has no xsl:initial-template. */
        const sxml_status status =
            sxml_initial_template_name(stylesheet->processor, &invocation.initial_template);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const sxml_status status = sxml_xslt_lower_invocation(&invocation, &transformation->unit);
    transformation->is_unit_owned = status == SXML_STATUS_OK;
    return status;
}

/* The available documents are source documents too (4.2.2: the
   stripping applies to documents fn:doc reads, whatever supplies them):
   each is stripped once per transformation, as the documents a loader
   parses are, so fn:doc gives one stripped tree per URI. */
static sxml_status sxml_transformation_prime_documents(sxml_transformation *transformation) {
    const sxml_available_document_view available =
        transformation->request->dynamic_context.available_documents;
    const sxml_xslt_stylesheet *stylesheet = transformation->stylesheet;
    if (available.available_document_count == 0 || !sxml_xslt_prepares_documents(stylesheet->ast)) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        stylesheet->processor->allocator,
        (sxml_array_allocation_request){.element_count = available.available_document_count,
                                        .element_size = sizeof(sxml_available_document),
                                        .element_alignment = alignof(sxml_available_document)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    transformation->available_documents = memory;
    for (size_t document_index = 0;
         status == SXML_STATUS_OK && document_index < available.available_document_count;
         document_index += 1) {
        const sxml_available_document *document = &available.available_documents[document_index];
        sxml_item stripped = {.representation = 0};
        status = sxml_xslt_strip_document(&stylesheet->stripping, document->document_node,
                                          &stripped);
        if (status == SXML_STATUS_OK) {
            transformation->available_documents[document_index] = (sxml_available_document){
                .document_uri = document->document_uri, .document_node = stripped};
            transformation->available_document_count += 1;
        }
    }
    return status;
}

/* The stripped copy of node when it is an available document; NULL
   otherwise. */
static const sxml_item *sxml_transformation_find_stripped(const sxml_transformation *transformation,
                                                          sxml_item node) {
    const sxml_available_document_view available =
        transformation->request->dynamic_context.available_documents;
    for (size_t document_index = 0; document_index < transformation->available_document_count;
         document_index += 1) {
        if (sxml_generic_node_is_same_node((sxml_item_pair){
                .first = available.available_documents[document_index].document_node,
                .second = node})) {
            return &transformation->available_documents[document_index].document_node;
        }
    }
    return NULL;
}

/* The context value (2.4.2): the global context item, stripped
   (4.2.2); an available document's stripped copy when it is one, so the
   context item and fn:doc of its URI stay one tree. */
static sxml_status sxml_transformation_prime_context(sxml_transformation *transformation) {
    const sxml_xslt_transformation_request *request = transformation->request;
    const sxml_dynamic_context_configuration *context = &request->dynamic_context;
    if (!context->has_context_value) {
        return SXML_STATUS_OK;
    }
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        context->context_value,
        (sxml_item_request){.item_index = 0, .error_record = request->error_record}, &item);
    sxml_item stripped = {.representation = 0};
    const sxml_item *copy = NULL;
    if (status == SXML_STATUS_OK && sxml_item_is_generic_node(item)) {
        copy = sxml_transformation_find_stripped(transformation, item);
    }
    if (copy != NULL) {
        stripped = *copy;
        sxml_item_retain(stripped);
    } else if (status == SXML_STATUS_OK) {
        status = sxml_xslt_strip_document(&transformation->stylesheet->stripping, item, &stripped);
    }
    if (status == SXML_STATUS_OK) {
        transformation->context_value = sxml_sequence_from_item(stripped);
        transformation->has_context_value = true;
    }
    return status;
}

/* ------------------------------------------------- Invocation values */

/* The name Q{urn:x-sxml:invocation}local_name of a global the unit reads
   its run-time invocation values from (src/xslt_lower_internal.h). */
static sxml_status sxml_invocation_global_name(sxml_processor *processor,
                                               sxml_byte_view local_name,
                                               sxml_name_identifier *name) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(processor->item_heap);
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        name_pool, SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_INVOCATION_NAMESPACE), &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = local_name},
            name);
    }
    return status;
}

/* A map from xs:QName to the values of bindings (the template and tunnel
   parameters, as xsl:with-param passes them, 10.1.3), owned. */
static sxml_status sxml_parameter_map_make(sxml_processor *processor,
                                           sxml_variable_binding_view bindings,
                                           sxml_sequence *map) {
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        processor->allocator,
        (sxml_array_allocation_request){.element_count = bindings.variable_binding_count,
                                        .element_size = sizeof(sxml_map_entry),
                                        .element_alignment = alignof(sxml_map_entry)},
        &memory);
    sxml_map_entry *entries = memory;
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < bindings.variable_binding_count;
         binding_index += 1) {
        const sxml_variable_binding *binding = &bindings.variable_bindings[binding_index];
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof(datum));
        datum.datum_kind = SXML_DATUM_KIND_QNAME;
        datum.datum_details.qname = (sxml_qname_datum){
            .expanded_name = binding->variable_name, .namespace_prefix = {.index = 0}};
        status = sxml_atomic_item_make(
            processor->item_heap,
            &(sxml_atomic_item_request){.type = {.index = SXML_TYPE_XS_QNAME}, .datum = datum},
            &entries[binding_index].key);
        entries[binding_index].value = binding->value;
        if (status != SXML_STATUS_OK) {
            bindings.variable_binding_count = binding_index;
        }
    }
    sxml_item made = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            processor->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = entries,
                                                .map_entry_count =
                                                    bindings.variable_binding_count},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_LAST,
                                .error_record = NULL},
            &made);
    }
    for (size_t binding_index = 0; entries != NULL && binding_index < bindings.variable_binding_count;
         binding_index += 1) {
        sxml_item_release(entries[binding_index].key);
    }
    sxml_allocator_release(processor->allocator, entries);
    if (status == SXML_STATUS_OK) {
        *map = sxml_sequence_from_item(made);
    }
    return status;
}

/*
 * The values of the invocation, bound to the globals the unit reads them
 * from (src/xslt_lower_internal.h, D157): the template and tunnel
 * parameters as maps, the initial match selection, and the arguments of
 * the initial function as an array. Each made value is owned by the
 * transformation; there is room for them after the parameters.
 */
static sxml_status
sxml_transformation_bind_invocation_values(sxml_transformation *transformation) {
    const sxml_xslt_transformation_request *request = transformation->request;
    sxml_processor *processor = transformation->stylesheet->processor;
    sxml_sequence values[SXML_INVOCATION_VALUE_COUNT];
    sxml_byte_view local_names[SXML_INVOCATION_VALUE_COUNT];
    size_t value_count = 0;
    sxml_status status = SXML_STATUS_OK;
    if (request->invocation_kind == SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION) {
        sxml_item arguments = {.representation = 0};
        status = sxml_array_make(processor->item_heap, request->function_arguments, &arguments);
        if (status == SXML_STATUS_OK) {
            values[value_count] = sxml_sequence_from_item(arguments);
            local_names[value_count] =
                SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_INVOCATION_FUNCTION_ARGUMENTS);
            value_count += 1;
        }
    } else {
        status = sxml_parameter_map_make(processor, request->template_parameters,
                                         &values[value_count]);
        if (status == SXML_STATUS_OK) {
            local_names[value_count] =
                SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_INVOCATION_TEMPLATE_PARAMETERS);
            value_count += 1;
            status = sxml_parameter_map_make(processor, request->tunnel_parameters,
                                             &values[value_count]);
        }
        if (status == SXML_STATUS_OK) {
            local_names[value_count] =
                SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_INVOCATION_TUNNEL_PARAMETERS);
            value_count += 1;
        }
        if (status == SXML_STATUS_OK && request->has_initial_match_selection) {
            sxml_sequence_retain(request->initial_match_selection);
            values[value_count] = request->initial_match_selection;
            local_names[value_count] =
                SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_INVOCATION_INITIAL_MATCH_SELECTION);
            value_count += 1;
        } else if (status == SXML_STATUS_OK && request->streamed_source_uri.byte_count > 0) {
            /* D228: the URI the unit's STREAM opens, an xs:string. */
            sxml_string uri = {.utf8 = {.bytes = NULL, .byte_count = 0},
                               .codepoint_count = 0,
                               .is_ascii = true};
            status = sxml_string_from_utf8(request->streamed_source_uri, &uri);
            sxml_item item = {.representation = 0};
            if (status == SXML_STATUS_OK) {
                sxml_atomic_datum datum;
                memset(&datum, 0, sizeof(datum));
                datum.datum_kind = SXML_DATUM_KIND_STRING;
                datum.datum_details.string = uri;
                status = sxml_atomic_item_make(
                    processor->item_heap,
                    &(sxml_atomic_item_request){.type = {.index = SXML_TYPE_XS_STRING},
                                                .datum = datum},
                    &item);
            }
            if (status == SXML_STATUS_OK) {
                values[value_count] = sxml_sequence_from_item(item);
                local_names[value_count] =
                    SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_INVOCATION_STREAMED_SOURCE);
                value_count += 1;
            }
        }
    }
    for (size_t value_index = 0; value_index < value_count; value_index += 1) {
        transformation->invocation_values[value_index] = values[value_index];
        transformation->invocation_value_count += 1;
        sxml_name_identifier name = {.index = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_invocation_global_name(processor, local_names[value_index], &name);
        }
        if (status == SXML_STATUS_OK) {
            transformation->bindings[transformation->binding_count] =
                (sxml_variable_binding){.variable_name = name, .value = values[value_index]};
            transformation->binding_count += 1;
        }
    }
    return status;
}

/* The bindings of the globals: the static parameters (9.6), then the
   stylesheet parameters (9.5) whose names they do not have. */
static sxml_status sxml_transformation_bind_parameters(sxml_transformation *transformation) {
    const sxml_xslt_stylesheet *stylesheet = transformation->stylesheet;
    const sxml_variable_binding_view supplied =
        transformation->request->dynamic_context.variable_bindings;
    const size_t total_count = stylesheet->static_parameter_count +
                               supplied.variable_binding_count + SXML_INVOCATION_VALUE_COUNT;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        stylesheet->processor->allocator,
        (sxml_array_allocation_request){.element_count = total_count,
                                        .element_size = sizeof(sxml_variable_binding),
                                        .element_alignment = alignof(sxml_variable_binding)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_variable_binding *bindings = memory;
    size_t binding_count = 0;
    for (size_t binding_index = 0; binding_index < stylesheet->static_parameter_count;
         binding_index += 1) {
        bindings[binding_count] = stylesheet->static_parameters[binding_index];
        binding_count += 1;
    }
    for (size_t binding_index = 0; binding_index < supplied.variable_binding_count;
         binding_index += 1) {
        const sxml_variable_binding *binding = &supplied.variable_bindings[binding_index];
        bool is_static = false;
        for (size_t static_index = 0; static_index < stylesheet->static_parameter_count;
             static_index += 1) {
            is_static = is_static || stylesheet->static_parameters[static_index].variable_name.index ==
                                         binding->variable_name.index;
        }
        if (!is_static) {
            bindings[binding_count] = *binding;
            binding_count += 1;
        }
    }
    transformation->bindings = bindings;
    transformation->binding_count = binding_count;
    return sxml_transformation_bind_invocation_values(transformation);
}

/* The result writer of the transformation (D156). */
static sxml_status sxml_transformation_open_writer(sxml_transformation *transformation) {
    const sxml_xslt_transformation_request *request = transformation->request;
    const sxml_xslt_stylesheet *stylesheet = transformation->stylesheet;
    return sxml_xslt_result_writer_create(
        stylesheet->processor->allocator,
        &(sxml_xslt_result_writer_configuration){
            .outputs = stylesheet->outputs,
            .processor = stylesheet->processor,
            .result_document_handler = request->result_document_handler,
            .base_output_uri = request->base_output_uri,
            .is_in_temporary_output_state =
                request->invocation_kind == SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION,
            .prefers_xml_for_xhtml = stylesheet->prefers_xml_for_xhtml},
        &transformation->writer);
}

/* The dynamic context and the extension of the evaluation of the unit
   (they borrow from transformation). */
static void sxml_transformation_describe(const sxml_transformation *transformation,
                                         sxml_dynamic_context_configuration *context,
                                         sxml_unit_evaluation_extension *extension) {
    const sxml_xslt_stylesheet *stylesheet = transformation->stylesheet;
    *context = transformation->request->dynamic_context;
    context->has_context_value = transformation->has_context_value;
    context->context_value =
        transformation->has_context_value ? transformation->context_value : sxml_sequence_empty();
    context->variable_bindings = (sxml_variable_binding_view){
        .variable_bindings = transformation->bindings,
        .variable_binding_count = transformation->binding_count};
    if (transformation->available_documents != NULL) {
        context->available_documents = (sxml_available_document_view){
            .available_documents = transformation->available_documents,
            .available_document_count = transformation->available_document_count};
    }
    *extension = (sxml_unit_evaluation_extension){
        .function_table = sxml_function_library_as_table(stylesheet->function_library),
        .document_preparation =
            sxml_xslt_prepares_documents(stylesheet->ast)
                ? sxml_xslt_whitespace_stripping_as_document_preparation(&stylesheet->stripping)
                : (sxml_document_preparation){.state = NULL, .operations = NULL},
        .result_writer = transformation->writer,
        .principal_result_builder =
            sxml_xslt_result_writer_principal_builder(transformation->writer),
        .principal_result_is_raw =
            sxml_xslt_result_writer_principal_raw_flag(transformation->writer),
        .discards_messages = false,
        .disables_assertions = false};
}

/* Delivers the raw result as the request asks (2.4.6). */
static sxml_status sxml_transformation_deliver(const sxml_transformation *transformation,
                                               sxml_sequence raw_result,
                                               sxml_sequence *principal_result) {
    const sxml_xslt_transformation_request *request = transformation->request;
    return sxml_xslt_result_writer_deliver_principal_result(
        transformation->writer,
        &(sxml_xslt_principal_result_request){.value = raw_result,
                                              .output_definition = request->output_definition,
                                              .result_delivery = request->result_delivery,
                                              .output_stream = request->output_stream,
                                              .error_record = request->error_record},
        principal_result);
}

/* Evaluates the unit and delivers its raw result. */
static sxml_status sxml_transformation_run(const sxml_transformation *transformation,
                                           sxml_sequence *principal_result) {
    sxml_dynamic_context_configuration context;
    sxml_unit_evaluation_extension extension;
    sxml_transformation_describe(transformation, &context, &extension);
    sxml_sequence raw_result = sxml_sequence_empty();
    sxml_status status = sxml_evaluate_unit(
        &(sxml_unit_evaluation){.processor = transformation->stylesheet->processor,
                                .unit = transformation->unit,
                                .static_base_uri = no_text,
                                .dynamic_context = &context,
                                .error_record = transformation->request->error_record,
                                .extension = &extension},
        &raw_result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_transformation_deliver(transformation, raw_result, principal_result);
    sxml_sequence_release(raw_result);
    return status;
}

/* Everything before the evaluation (2.4.2): the unit of the invocation,
   the stripped documents and context, the bindings and the writer. */
/* D231: a transformation that streams its principal source delivers a
   serialized principal result as it is made, when the result writer can
   (sxml_xslt_result_writer_start_principal_stream). */
static sxml_status sxml_transformation_open_stream(sxml_transformation *transformation) {
    const sxml_xslt_transformation_request *request = transformation->request;
    if (request->streamed_source_uri.byte_count == 0 ||
        request->result_delivery != SXML_XSLT_RESULT_DELIVERY_SERIALIZED) {
        return SXML_STATUS_OK;
    }
    return sxml_xslt_result_writer_start_principal_stream(
        transformation->writer,
        &(sxml_xslt_principal_result_request){.value = sxml_sequence_empty(),
                                              .output_definition = request->output_definition,
                                              .result_delivery = request->result_delivery,
                                              .output_stream = request->output_stream,
                                              .error_record = request->error_record},
        &transformation->streams_result_document);
}

static sxml_status sxml_transformation_prime(sxml_transformation *transformation) {
    /* The writer first: whether the result streams changes the unit. */
    sxml_status status = sxml_transformation_open_writer(transformation);
    if (status == SXML_STATUS_OK) {
        status = sxml_transformation_open_stream(transformation);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_transformation_choose_unit(transformation);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_transformation_prime_documents(transformation);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_transformation_prime_context(transformation);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_transformation_bind_parameters(transformation);
    }
    return status;
}

static bool sxml_transformation_start_is_valid(const sxml_xslt_stylesheet *stylesheet,
                                               const sxml_xslt_transformation_request *request) {
    return stylesheet != NULL && stylesheet->unit != NULL && request != NULL &&
           sxml_dynamic_context_configuration_is_valid(&request->dynamic_context) &&
           sxml_transformation_request_is_valid(request);
}

static sxml_transformation sxml_transformation_start(
    const sxml_xslt_stylesheet *stylesheet, const sxml_xslt_transformation_request *request) {
    return (sxml_transformation){.stylesheet = stylesheet,
                                 .request = request,
                                 .unit = NULL,
                                 .is_unit_owned = false,
                                 .context_value = sxml_sequence_empty(),
                                 .has_context_value = false,
                                 .bindings = NULL,
                                 .binding_count = 0,
                                 .invocation_value_count = 0,
                                 .writer = NULL,
                                 .available_documents = NULL,
                                 .available_document_count = 0,
                                 .streams_result_document = false};
}

sxml_status sxml_xslt_stylesheet_transform(const sxml_xslt_stylesheet *stylesheet,
                                           const sxml_xslt_transformation_request *request,
                                           sxml_sequence *principal_result) {
    if (principal_result == NULL || !sxml_transformation_start_is_valid(stylesheet, request)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_transformation transformation = sxml_transformation_start(stylesheet, request);
    sxml_status status = sxml_transformation_prime(&transformation);
    if (status == SXML_STATUS_OK) {
        status = sxml_transformation_run(&transformation, principal_result);
    }
    sxml_transformation_release(&transformation);
    return status;
}

sxml_status sxml_xslt_stylesheet_streams_source(const sxml_xslt_stylesheet *stylesheet,
                                                const sxml_xslt_transformation_request *request,
                                                bool *streams) {
    if (stylesheet == NULL || request == NULL || streams == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *streams = false;
    if (request->invocation_kind != SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES) {
        return SXML_STATUS_OK;
    }
    /* The initial mode as the lowering picks it (2.4.3), by name. */
    sxml_processor *processor = stylesheet->processor;
    bool reads_accumulators = false;
    const sxml_status status = sxml_xslt_reads_accumulators(
        &(sxml_xslt_accumulator_reading){
            .ast = stylesheet->ast,
            .allocator = processor->allocator,
            .name_pool = sxml_item_heap_get_name_pool(processor->item_heap)},
        &reads_accumulators);
    if (status != SXML_STATUS_OK || reads_accumulators) {
        return status;
    }
    return sxml_xslt_assess_mode(
        &(sxml_xslt_mode_query){
            .ast = stylesheet->ast,
            .allocator = processor->allocator,
            .name_pool = sxml_item_heap_get_name_pool(processor->item_heap),
            .instruction = {.value = 0},
            .mode_name = request->initial_mode_name,
            .is_default_mode =
                request->initial_mode_name.index == 0 && !request->selects_unnamed_mode},
        streams);
}

/* ----------------------------------------- Transformation on the machine */

struct sxml_xslt_machine_transformation {
    sxml_transformation transformation;
    sxml_unit_evaluation_extension extension;
    /* One slot per global of the unit; values borrowed from the
       bindings. */
    sxml_evaluator_slot *global_slots;
    size_t global_slot_count;
    sxml_evaluation_request evaluation;
};

sxml_status sxml_xslt_machine_transformation_create(
    const sxml_xslt_machine_transformation_request *request,
    sxml_xslt_machine_transformation **made) {
    if (request == NULL || made == NULL ||
        !sxml_transformation_start_is_valid(request->stylesheet, request->transformation)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = request->stylesheet->processor;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        processor->allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_xslt_machine_transformation),
                                  .alignment = alignof(sxml_xslt_machine_transformation)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_machine_transformation *running = memory;
    memset(running, 0, sizeof(*running));
    running->transformation =
        sxml_transformation_start(request->stylesheet, request->transformation);
    status = sxml_transformation_prime(&running->transformation);
    sxml_dynamic_context_configuration context;
    memset(&context, 0, sizeof(context));
    if (status == SXML_STATUS_OK) {
        sxml_transformation_describe(&running->transformation, &context, &running->extension);
        running->extension.discards_messages = request->discards_messages;
        running->extension.disables_assertions = request->disables_assertions;
        const sxml_ir_unit *unit = running->transformation.unit;
        running->global_slot_count =
            unit->global_variable_count > 1 ? unit->global_variable_count - 1U : 0;
    }
    if (status == SXML_STATUS_OK && running->global_slot_count > 0) {
        void *slots = NULL;
        status = sxml_allocator_allocate_array(
            processor->allocator,
            (sxml_array_allocation_request){.element_count = running->global_slot_count,
                                            .element_size = sizeof(sxml_evaluator_slot),
                                            .element_alignment = alignof(sxml_evaluator_slot)},
            &slots);
        running->global_slots = slots;
        if (status == SXML_STATUS_OK) {
            sxml_global_binding_fill(&(sxml_global_binding){.unit = running->transformation.unit,
                                                            .bindings = &context.variable_bindings,
                                                            .slots = running->global_slots});
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_xslt_machine_transformation_destroy(running);
        return status;
    }
    running->evaluation = (sxml_evaluation_request){
        .unit = running->transformation.unit,
        .has_context_value = context.has_context_value,
        .context_value = context.context_value,
        .global_values = {.slots = running->global_slots,
                          .slot_count = running->global_slot_count},
        .static_base_uri = no_text,
        .error_record = NULL,
        .extension = &running->extension};
    *made = running;
    return SXML_STATUS_OK;
}

const sxml_evaluation_request *sxml_xslt_machine_transformation_get_evaluation(
    const sxml_xslt_machine_transformation *running) {
    return &running->evaluation;
}

const sxml_unit_evaluation_extension *sxml_xslt_machine_transformation_get_extension(
    const sxml_xslt_machine_transformation *running) {
    return &running->extension;
}

sxml_status sxml_xslt_machine_transformation_deliver(sxml_xslt_machine_transformation *running,
                                                     sxml_sequence raw_result,
                                                     sxml_sequence *principal_result) {
    return sxml_transformation_deliver(&running->transformation, raw_result, principal_result);
}

bool sxml_xslt_machine_transformation_has_principal_document(
    const sxml_xslt_machine_transformation *running) {
    return sxml_xslt_result_writer_has_principal_document(running->transformation.writer);
}

void sxml_xslt_machine_transformation_destroy(sxml_xslt_machine_transformation *running) {
    if (running == NULL) {
        return;
    }
    sxml_processor *processor = running->transformation.stylesheet->processor;
    sxml_transformation_release(&running->transformation);
    sxml_allocator_release(processor->allocator, running->global_slots);
    sxml_allocator_release(processor->allocator, running);
}

bool sxml_xslt_stylesheet_holds_text(const sxml_xslt_stylesheet *stylesheet, sxml_byte_view text) {
    if (stylesheet == NULL || text.bytes == NULL) {
        return false;
    }
    if (stylesheet->unit != NULL && text.bytes == stylesheet->unit->source_name.bytes) {
        return true;
    }
    const sxml_xslt_ast *ast = stylesheet->ast;
    for (uint32_t module_index = 1; ast != NULL && module_index < ast->module_count;
         module_index += 1) {
        if (text.bytes ==
            sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = module_index})
                ->module_uri.bytes) {
            return true;
        }
    }
    return false;
}
