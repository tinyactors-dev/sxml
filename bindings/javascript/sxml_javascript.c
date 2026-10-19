/*
 * The JavaScript shim (docs/GLOSSARY.md, "javascript_shim"): the C entry
 * points that sxml.wasm exports for the TypeScript package in
 * bindings/javascript/src/ (D10, D138). Imitates apps/sxml/command_xpath.c,
 * which drives the same public API for the CLI: one processor, a resource
 * resolver, compile, evaluate, serialize.
 *
 * The full module links the XQuery, XSLT and schema entry points besides
 * (sxml_javascript_xquery.c, sxml_javascript_xslt.c,
 * sxml_javascript_schema.c; D189's two links): the core module has none of
 * them, and the package finds out which it has by their exports. The
 * handles and helpers they share are in sxml_javascript_internal.h.
 *
 * Only the public API (include/sxml/) is used. Every object the package
 * holds is a handle (docs/GLOSSARY.md): the address of one of the structs
 * below, made by a *_create or by an operation that returns a value, and
 * given back to the matching *_destroy or *_release exactly once. The
 * request structs are written by the package into WebAssembly memory; their
 * fields are 4 bytes each on wasm32 (pointers, sizes, uint32_t), so the
 * package reads and writes them as arrays of 32-bit words. The static
 * assertions at the end of this file keep the two sides in step.
 *
 * Memory: each processor has a counting allocator over the default
 * allocator, and everything made for it (the processor, values,
 * expressions, the bytes the package copies in) comes from it, so the
 * package's tests can check for leaks (live allocation count) and drive
 * the allocation failure paths (failing allocation number), as the C tests
 * do with sxml_counting_allocator (D23).
 *
 * Errors: an operation that fails returns NULL (or its status) and leaves
 * the details in the processor's error, which the package reads at once:
 * its views point into the item heap or static text and stay valid until
 * the next operation on the processor.
 *
 * Resources: fn:doc and the other resource functions ask the host import
 * sxml_host.resolve, a JavaScript function, which answers with bytes it
 * copied into memory from this processor's allocator; the shim releases
 * them when sxml is done (include/sxml/processor.h, "Resource resolver").
 */
#include "sxml_javascript_internal.h"

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/version.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* As sxml xpath: entity expansion is bounded, nesting is not. */
enum { SXML_JAVASCRIPT_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* ---------------------------------------------------------------- Handles */

/* A compiled XPath expression and the bindings of its variables, whose
   names are fixed at compilation and whose values each evaluation sets. */
struct sxml_javascript_expression {
    sxml_javascript_processor *owner;
    sxml_xpath_expression *expression;
    /* variable_count names, in the order of the compile request. */
    sxml_name_identifier *variable_names;
    /* Room for variable_count bindings: those of one evaluation. */
    sxml_variable_binding *bindings;
    size_t variable_count;
};

/* ------------------------------------------------------------ Host import */

#if defined(__wasm__)
/* Implemented by the JavaScript package (src/index.ts, "resolve"). Returns
   a status: SXML_STATUS_OK, or another to stop the evaluation (the
   package keeps the exception it caught and rethrows it). */
__attribute__((import_module("sxml_host"), import_name("resolve"))) extern uint32_t
sxml_javascript_host_resolve(sxml_javascript_processor *processor,
                             const sxml_javascript_resource_request *request,
                             sxml_javascript_resource *resource);
#else
/* Native builds (lint, -Werror) have no host: nothing is available. */
static uint32_t sxml_javascript_host_resolve(sxml_javascript_processor *processor,
                                             const sxml_javascript_resource_request *request,
                                             sxml_javascript_resource *resource) {
    (void)processor;
    (void)request;
    resource->resource_form = SXML_RESOURCE_FORM_NONE;
    return SXML_STATUS_OK;
}
#endif

static sxml_status sxml_javascript_resolve(void *state, const sxml_resource_request *request,
                                           sxml_resource *resource) {
    sxml_javascript_processor *processor = state;
    const sxml_javascript_resource_request host_request = {
        .resource_kind = (uint32_t)request->resource_kind,
        .uri_reference = request->uri_reference,
        .absolute_uri = request->absolute_uri};
    sxml_javascript_resource host_resource = {.resource_form = SXML_RESOURCE_FORM_NONE,
                                              .content = {.bytes = NULL, .byte_count = 0}};
    const uint32_t status =
        sxml_javascript_host_resolve(processor, &host_request, &host_resource);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, (void *)host_resource.content.bytes);
        return (sxml_status)status;
    }
    if (host_resource.resource_form == SXML_RESOURCE_FORM_BYTES) {
        resource->resource_form = SXML_RESOURCE_FORM_BYTES;
        resource->content = host_resource.content;
    } else {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        resource->description = SXML_BYTE_VIEW_FROM_LITERAL("The resolver has no such resource");
    }
    return SXML_STATUS_OK;
}

static void sxml_javascript_release_resource(void *state, const sxml_resource *resource) {
    const sxml_javascript_processor *processor = state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        sxml_allocator_release(processor->allocator, (void *)resource->content.bytes);
    }
}

static const sxml_resource_resolver_operations sxml_javascript_resolver_operations = {
    .resolve = sxml_javascript_resolve,
    .release = sxml_javascript_release_resource,
};

/* --------------------------------------------------------------- Memory */

/*
 * Allocates byte_count bytes (at least 1) from the processor's allocator,
 * aligned for any request struct, or returns NULL when out of memory.
 * The package copies its texts and request structs here.
 */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_allocate")
void *sxml_javascript_allocate(sxml_javascript_processor *processor, size_t byte_count) {
    return sxml_javascript_allocate_bytes(processor->allocator, byte_count == 0 ? 1 : byte_count,
                                          alignof(max_align_t));
}

/* Releases memory from sxml_javascript_allocate; NULL does nothing. */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_release")
void sxml_javascript_release(sxml_javascript_processor *processor, void *memory) {
    sxml_allocator_release(processor->allocator, memory);
}

/* The allocations of the processor's allocator not yet released. */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_live_allocation_count")
uint32_t sxml_javascript_live_allocation_count(const sxml_javascript_processor *processor) {
    return (uint32_t)sxml_counting_allocator_read_counts(processor->counting_allocator)
        .live_allocation_count;
}

/* ------------------------------------------------------ Optional features */

enum { SXML_JAVASCRIPT_MAXIMUM_FEATURE_COUNT = 32 };

/* The module's features, at most SXML_JAVASCRIPT_MAXIMUM_FEATURE_COUNT. */
static sxml_optional_feature_view sxml_javascript_features(void) {
    sxml_optional_feature_view features = sxml_javascript_module_features();
    if (features.optional_feature_count > SXML_JAVASCRIPT_MAXIMUM_FEATURE_COUNT) {
        features.optional_feature_count = SXML_JAVASCRIPT_MAXIMUM_FEATURE_COUNT;
    }
    return features;
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_feature_count")
uint32_t sxml_javascript_feature_count(void) {
    return (uint32_t)sxml_javascript_features().optional_feature_count;
}

/* The name of the feature at feature_index; the empty view past the last. */
static sxml_byte_view sxml_javascript_feature_name(uint32_t feature_index) {
    const sxml_optional_feature_view features = sxml_javascript_features();
    if (feature_index >= features.optional_feature_count) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return sxml_optional_feature_get_name(features.optional_features[feature_index]);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_feature_name_bytes")
const uint8_t *sxml_javascript_feature_name_bytes(uint32_t feature_index) {
    return sxml_javascript_feature_name(feature_index).bytes;
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_feature_name_byte_count")
uint32_t sxml_javascript_feature_name_byte_count(uint32_t feature_index) {
    return (uint32_t)sxml_javascript_feature_name(feature_index).byte_count;
}

/* The features of feature_mask, written to selected (room for
   SXML_JAVASCRIPT_MAXIMUM_FEATURE_COUNT), as a view of it. */
static sxml_optional_feature_view sxml_javascript_select_features(uint32_t feature_mask,
                                                                  sxml_optional_feature *selected) {
    const sxml_optional_feature_view features = sxml_javascript_features();
    size_t selected_count = 0;
    for (size_t feature_index = 0; feature_index < features.optional_feature_count;
         feature_index += 1) {
        if ((feature_mask & (UINT32_C(1) << feature_index)) != 0) {
            selected[selected_count] = features.optional_features[feature_index];
            selected_count += 1;
        }
    }
    return (sxml_optional_feature_view){.optional_features = selected,
                                        .optional_feature_count = selected_count};
}

/* ------------------------------------------------------------- Processor */

/*
 * Makes a processor. failing_allocation_number is that of
 * sxml_counting_allocator_configuration (0 fails none), counted from the
 * processor's own first allocation; feature_mask selects the module's
 * features. Returns NULL when out of memory.
 */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_processor_create")
sxml_javascript_processor *sxml_javascript_processor_create(uint32_t failing_allocation_number,
                                                            uint32_t feature_mask) {
    const sxml_allocator default_allocator = sxml_default_allocator();
    sxml_javascript_processor *processor = sxml_javascript_allocate_bytes(
        default_allocator, sizeof(sxml_javascript_processor), alignof(sxml_javascript_processor));
    if (processor == NULL) {
        return NULL;
    }
    memset(processor, 0, sizeof(*processor));
    sxml_status status = sxml_counting_allocator_create(
        default_allocator,
        &(sxml_counting_allocator_configuration){
            .backing_allocator = default_allocator,
            .failing_allocation_number = failing_allocation_number},
        &processor->counting_allocator);
    if (status == SXML_STATUS_OK) {
        processor->allocator = sxml_counting_allocator_as_allocator(processor->counting_allocator);
        sxml_optional_feature selected_features[SXML_JAVASCRIPT_MAXIMUM_FEATURE_COUNT];
        status = sxml_processor_create(
            processor->allocator,
            &(sxml_processor_configuration){
                .allocator = processor->allocator,
                .limits = {.xml_limits = {.maximum_depth = 0,
                                          .maximum_entity_expansion_byte_count =
                                              SXML_JAVASCRIPT_MAXIMUM_EXPANSION_BYTE_COUNT}},
                .resource_resolver = {.state = processor,
                                      .operations = &sxml_javascript_resolver_operations},
                .clock = sxml_system_clock(),
                .optional_features =
                    sxml_javascript_select_features(feature_mask, selected_features)},
            &processor->processor);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            processor->allocator,
            &(sxml_output_buffer_configuration){.allocator = processor->allocator,
                                                .maximum_byte_count = 0},
            &processor->output_buffer);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serialization_parameter_reader_create(
            processor->allocator,
            &(sxml_serialization_parameter_reader_configuration){
                .allocator = processor->allocator,
                .item_heap = sxml_processor_get_item_heap(processor->processor)},
            &processor->parameter_reader);
    }
    if (status != SXML_STATUS_OK) {
        sxml_output_buffer_destroy(processor->output_buffer);
        sxml_processor_destroy(processor->processor);
        sxml_counting_allocator_destroy(processor->counting_allocator);
        sxml_allocator_release(default_allocator, processor);
        return NULL;
    }
    return processor;
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_processor_destroy")
uint32_t sxml_javascript_processor_destroy(sxml_javascript_processor *processor) {
    if (processor == NULL) {
        return 0;
    }
    sxml_item_heap_clear_raised_error(sxml_processor_get_item_heap(processor->processor));
    sxml_serialization_parameter_reader_destroy(processor->parameter_reader);
    sxml_output_buffer_destroy(processor->output_buffer);
    sxml_processor_destroy(processor->processor);
    const uint32_t leaked_allocation_count = sxml_javascript_live_allocation_count(processor);
    sxml_counting_allocator_destroy(processor->counting_allocator);
    sxml_allocator_release(sxml_default_allocator(), processor);
    return leaked_allocation_count;
}

/* The failure of the processor's last operation. */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_error_of")
const sxml_javascript_error *sxml_javascript_error_of(const sxml_javascript_processor *processor) {
    return &processor->error;
}

/* The bytes of the processor's last serialization. */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_output_of")
const sxml_byte_view *sxml_javascript_output_of(const sxml_javascript_processor *processor) {
    return &processor->output;
}

/* The library version as NUL-terminated ASCII ("0.2.0"). */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_version_text")
const char *sxml_javascript_version_text(void) {
    return sxml_version_text();
}

/* ----------------------------------------------------------------- Values */

/* Releases a value; NULL does nothing. */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_value_release")
void sxml_javascript_value_release(sxml_javascript_value *value) {
    if (value == NULL) {
        return;
    }
    sxml_sequence_release(value->sequence);
    sxml_allocator_release(value->owner->allocator, value);
}

/*
 * Parses an XML document (include/sxml/xml.h) and returns its document
 * node as a value, or NULL with the processor's error set (the parser's
 * description and location).
 */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_parse")
sxml_javascript_value *sxml_javascript_parse(sxml_javascript_processor *processor,
                                             const sxml_javascript_parse_request *request) {
    sxml_javascript_begin(processor);
    sxml_item document_node = {.representation = 0};
    const sxml_status status = sxml_processor_parse_document(
        processor->processor,
        &(sxml_xml_parse_request){.input = request->input,
                                  .source_name = request->document_uri,
                                  .document_uri = request->document_uri,
                                  .base_uri = request->document_uri,
                                  .encoding_name = sxml_javascript_no_text,
                                  .diagnostic = &processor->error_record.diagnostic},
        &document_node);
    if (status != SXML_STATUS_OK) {
        sxml_javascript_fail(processor, status);
        return NULL;
    }
    return sxml_javascript_value_make(processor, sxml_sequence_from_item(document_node));
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_string")
sxml_javascript_value *sxml_javascript_string(sxml_javascript_processor *processor,
                                              const sxml_byte_view *text) {
    sxml_javascript_begin(processor);
    sxml_item string = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        sxml_processor_get_item_heap(processor->processor),
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = *text,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        &string);
    if (status != SXML_STATUS_OK) {
        sxml_javascript_fail(processor, status);
        return NULL;
    }
    return sxml_javascript_value_make(processor, sxml_sequence_from_item(string));
}

/* ------------------------------------------------------------ Expressions */

/* Destroys an expression; NULL does nothing. */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_expression_destroy")
void sxml_javascript_expression_destroy(sxml_javascript_expression *expression) {
    if (expression == NULL) {
        return;
    }
    sxml_xpath_expression_destroy(expression->expression);
    sxml_allocator_release(expression->owner->allocator, expression->variable_names);
    sxml_allocator_release(expression->owner->allocator, expression->bindings);
    sxml_allocator_release(expression->owner->allocator, expression);
}

/* The static context's namespaces and variables, in allocator memory. */
typedef struct sxml_javascript_static_names {
    sxml_namespace_binding *namespace_bindings;
    sxml_variable_declaration *declarations;
} sxml_javascript_static_names;

/* Interns the namespaces and variable names of request into names and
   expression->variable_names. */
static sxml_status sxml_javascript_intern_static_names(
    sxml_javascript_expression *expression, const sxml_javascript_compile_request *request,
    sxml_javascript_static_names *names) {
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(expression->owner->processor);
    sxml_status status = SXML_STATUS_OK;
    for (size_t namespace_index = 0;
         status == SXML_STATUS_OK && namespace_index < request->namespace_count;
         namespace_index += 1) {
        const sxml_javascript_namespace *source = &request->namespaces[namespace_index];
        sxml_namespace_binding *binding = &names->namespace_bindings[namespace_index];
        status = sxml_name_pool_intern_namespace_prefix(name_pool, source->namespace_prefix,
                                                        &binding->namespace_prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_uri(name_pool, source->namespace_uri,
                                                         &binding->namespace_uri);
        }
    }
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && variable_index < request->variable_count;
         variable_index += 1) {
        const sxml_javascript_variable *source = &request->variables[variable_index];
        sxml_namespace_uri_identifier namespace_uri = {.index = 0};
        status = sxml_name_pool_intern_namespace_uri(name_pool, source->namespace_uri,
                                                     &namespace_uri);
        sxml_name_identifier name = {.index = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_name(
                name_pool,
                &(sxml_name_request){.namespace_uri = namespace_uri,
                                     .local_name = source->local_name},
                &name);
        }
        names->declarations[variable_index] = (sxml_variable_declaration){
            .variable_name = name,
            .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                              .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
        expression->variable_names[variable_index] = name;
    }
    return status;
}

/*
 * Compiles an XPath 4.0 expression (include/sxml/xpath.h) with the
 * namespaces and external variables of request, and returns it, or NULL
 * with the processor's error set (a static or type error, an invalid
 * namespace binding, a limit, out of memory).
 */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_compile")
sxml_javascript_expression *sxml_javascript_compile(
    sxml_javascript_processor *processor, const sxml_javascript_compile_request *request) {
    sxml_javascript_begin(processor);
    const sxml_allocator allocator = processor->allocator;
    sxml_javascript_expression *expression = sxml_javascript_allocate_bytes(
        allocator, sizeof(sxml_javascript_expression), alignof(sxml_javascript_expression));
    if (expression == NULL) {
        sxml_javascript_fail(processor, SXML_STATUS_OUT_OF_MEMORY);
        return NULL;
    }
    *expression = (sxml_javascript_expression){.owner = processor,
                                               .expression = NULL,
                                               .variable_names = NULL,
                                               .bindings = NULL,
                                               .variable_count = request->variable_count};
    sxml_javascript_static_names names = {.namespace_bindings = NULL, .declarations = NULL};
    void *memory = NULL;
    sxml_status status = sxml_javascript_allocate_array(
        allocator,
        (sxml_javascript_array_request){
            .element = {.byte_count = sizeof(sxml_namespace_binding), .alignment = alignof(sxml_namespace_binding)},
            .element_count = request->namespace_count},
        &memory);
    names.namespace_bindings = memory;
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_allocate_array(
            allocator,
            (sxml_javascript_array_request){
                .element = {.byte_count = sizeof(sxml_variable_declaration),
                            .alignment = alignof(sxml_variable_declaration)},
                .element_count = request->variable_count},
            &memory);
        names.declarations = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_allocate_array(
            allocator,
            (sxml_javascript_array_request){
                .element = {.byte_count = sizeof(sxml_variable_binding),
                            .alignment = alignof(sxml_variable_binding)},
                .element_count = request->variable_count},
            &memory);
        expression->bindings = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_allocate_array(
            allocator,
            (sxml_javascript_array_request){
                .element = {.byte_count = sizeof(sxml_name_identifier),
                            .alignment = alignof(sxml_name_identifier)},
                .element_count = request->variable_count},
            &memory);
        expression->variable_names = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_javascript_intern_static_names(expression, request, &names);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_create(
            allocator,
            &(sxml_xpath_expression_configuration){
                .processor = processor->processor,
                .expression_text = request->expression_text,
                .source_name = SXML_BYTE_VIEW_FROM_LITERAL("expression"),
                .static_context = {.namespace_bindings = {.namespace_bindings =
                                                              names.namespace_bindings,
                                                          .namespace_binding_count =
                                                              request->namespace_count},
                                   .static_base_uri = request->static_base_uri,
                                   .variable_declarations = {.variable_declarations =
                                                                 names.declarations,
                                                             .variable_declaration_count =
                                                                 request->variable_count}},
                .error_record = &processor->error_record},
            &expression->expression);
    }
    sxml_allocator_release(allocator, names.namespace_bindings);
    sxml_allocator_release(allocator, names.declarations);
    if (status != SXML_STATUS_OK) {
        sxml_javascript_expression_destroy(expression);
        sxml_javascript_fail(processor, status);
        return NULL;
    }
    return expression;
}

/*
 * Evaluates an expression and returns the result as a value, or NULL with
 * the processor's error set. request->variable_value_count must be the
 * expression's variable count (SXML_STATUS_INVALID_ARGUMENT otherwise).
 */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_evaluate")
sxml_javascript_value *sxml_javascript_evaluate(sxml_javascript_expression *expression,
                                                const sxml_javascript_evaluate_request *request) {
    sxml_javascript_processor *processor = expression->owner;
    sxml_javascript_begin(processor);
    if (request->variable_value_count != expression->variable_count) {
        sxml_javascript_fail(processor, SXML_STATUS_INVALID_ARGUMENT);
        return NULL;
    }
    size_t bound_count = 0;
    for (size_t variable_index = 0; variable_index < expression->variable_count;
         variable_index += 1) {
        const sxml_javascript_value *value = request->variable_values[variable_index];
        if (value != NULL) {
            /* Unbound variables are left out, so they raise XPDY0002. */
            expression->bindings[bound_count] = (sxml_variable_binding){
                .variable_name = expression->variable_names[variable_index],
                .value = value->sequence};
            bound_count += 1;
        }
    }
    const sxml_javascript_value *context = request->context_value;
    sxml_sequence result = sxml_sequence_empty();
    const sxml_status status = sxml_xpath_expression_evaluate(
        expression->expression,
        &(sxml_xpath_evaluation_request){
            .dynamic_context = {.has_context_value = context != NULL,
                                .context_value =
                                    context != NULL ? context->sequence : sxml_sequence_empty(),
                                .variable_bindings = {.variable_bindings = expression->bindings,
                                                      .variable_binding_count = bound_count}},
            .error_record = &processor->error_record},
        &result);
    if (status != SXML_STATUS_OK) {
        sxml_javascript_fail(processor, status);
        return NULL;
    }
    return sxml_javascript_value_make(processor, result);
}

/* ---------------------------------------------------------- Serialization */

/*
 * Serializes request->value (Serialization 4.0) into the processor's
 * output (sxml_javascript_output_of): by the adaptive method when there
 * are no options, else as fn:serialize does with request->options (F&O
 * 4.0, "fn:serialize": method xml unless the options name another).
 * Returns the status; on failure the processor's error is set.
 */
SXML_JAVASCRIPT_EXPORT("sxml_javascript_serialize")
uint32_t sxml_javascript_serialize(sxml_javascript_processor *processor,
                                   const sxml_javascript_serialize_request *request) {
    sxml_javascript_begin(processor);
    sxml_output_buffer_clear(processor->output_buffer);
    processor->output = sxml_javascript_no_text;
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_ADAPTIVE;
    sxml_status status = SXML_STATUS_OK;
    if (request->options != NULL) {
        status = sxml_serialization_parameter_reader_read(
            processor->parameter_reader,
            &(sxml_serialization_parameter_read_request){
                .options = request->options->sequence,
                .error_record = &processor->error_record},
            &parameters);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(processor->processor),
            &(sxml_serialization_request){.value = request->value->sequence,
                                          .serialization_parameters = parameters,
                                          .error_record = &processor->error_record},
            sxml_output_buffer_as_output_stream(processor->output_buffer));
    }
    if (status != SXML_STATUS_OK) {
        sxml_javascript_fail(processor, status);
        return (uint32_t)status;
    }
    processor->output = sxml_output_buffer_get_bytes(processor->output_buffer);
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------------- Layout */

#if defined(__wasm32__)
/* The package (src/index.ts) reads and writes these as 32-bit words at
   these offsets. */
_Static_assert(sizeof(sxml_byte_view) == 8, "a byte view is two words");
_Static_assert(sizeof(sxml_javascript_error) == 60, "error: 15 words");
_Static_assert(offsetof(sxml_javascript_error, line_number) == 52, "error line");
_Static_assert(sizeof(sxml_javascript_resource_request) == 20, "resource request: 5 words");
_Static_assert(sizeof(sxml_javascript_resource) == 12, "resource: 3 words");
_Static_assert(sizeof(sxml_javascript_parse_request) == 16, "parse request: 4 words");
_Static_assert(sizeof(sxml_javascript_namespace) == 16, "namespace: 4 words");
_Static_assert(sizeof(sxml_javascript_variable) == 16, "variable: 4 words");
_Static_assert(sizeof(sxml_javascript_compile_request) == 32, "compile request: 8 words");
_Static_assert(sizeof(sxml_javascript_evaluate_request) == 12, "evaluate request: 3 words");
_Static_assert(sizeof(sxml_javascript_serialize_request) == 8, "serialize request: 2 words");
#endif
