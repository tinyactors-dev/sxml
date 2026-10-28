/*
 * The host shim (docs/GLOSSARY.md, "host_shim"; D138, D238): the C entry
 * points under the JavaScript, Kotlin and Swift bindings. Moved from the
 * JavaScript shim (bindings/javascript/sxml_javascript.c) as it was, so
 * that a fix here fixes all three hosts. Imitates
 * apps/sxml/command_xpath.c, which drives the same public API for the
 * CLI: one processor, a resource resolver, compile, evaluate, serialize.
 *
 * The XQuery, XSLT and schema entry points are in sxml_host_xquery.c,
 * sxml_host_xslt.c and sxml_host_schema.c, which every link has (D272).
 * The handles and
 * helpers they share are in sxml_host_internal.h.
 *
 * Only the public API (include/sxml/) is used. Every object a host holds
 * is a handle (docs/GLOSSARY.md): the address of one of the structs
 * below, made by a *_create or by an operation that returns a value, and
 * given back to the matching *_destroy or *_release exactly once. The
 * request structs are written by the host; on wasm32 their fields are 4
 * bytes each (pointers, sizes, uint32_t), so the JavaScript package reads
 * and writes them as arrays of 32-bit words. The static assertions at the
 * end of this file keep the two sides in step.
 *
 * Memory: each processor has a counting allocator over the default
 * allocator, and everything made for it (the processor, values,
 * expressions, the bytes a host copies in) comes from it, so the
 * bindings' tests can check for leaks (live allocation count) and drive
 * the allocation failure paths (failing allocation number), as the C tests
 * do with sxml_counting_allocator (D23).
 *
 * Errors: an operation that fails returns NULL (or its status) and leaves
 * the details in the processor's error, which the host reads at once:
 * its views point into the item heap or static text and stay valid until
 * the next operation on the processor.
 *
 * Resources: fn:doc and the other resource functions ask the host's
 * resolver (sxml_host_resolver, a state and a function; under WASI the
 * JavaScript export file passes one that calls the import
 * sxml_host.resolve), which answers with bytes it copied into memory from
 * this processor's allocator; the shim releases them when sxml is done
 * (include/sxml/processor.h, "Resource resolver").
 */
#include "sxml_host_internal.h"

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/string.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/version.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* As sxml xpath: entity expansion is bounded, nesting is not. */
enum { SXML_HOST_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* ---------------------------------------------------------------- Handles */

/* A compiled XPath expression and the bindings of its variables, whose
   names are fixed at compilation and whose values each evaluation sets. */
struct sxml_host_expression {
    sxml_host_processor *owner;
    sxml_xpath_expression *expression;
    /* variable_count names, in the order of the compile request. */
    sxml_name_identifier *variable_names;
    /* Room for variable_count bindings: those of one evaluation. */
    sxml_variable_binding *bindings;
    size_t variable_count;
};

/* -------------------------------------------------------------- Resolver */

static sxml_status sxml_host_resolve(void *state, const sxml_resource_request *request,
                                     sxml_resource *resource) {
    sxml_host_processor *processor = state;
    const sxml_host_resource_request host_request = {
        .resource_kind = (uint32_t)request->resource_kind,
        .uri_reference = request->uri_reference,
        .absolute_uri = request->absolute_uri,
        .processor = processor};
    sxml_host_resource host_resource = {.resource_form = SXML_RESOURCE_FORM_NONE,
                                        .content = {.bytes = NULL, .byte_count = 0},
                                        .reader_identifier = 0};
    const sxml_host_resolver resolver = processor->resolver;
    const uint32_t status =
        resolver.resolve == NULL
            ? (uint32_t)SXML_STATUS_OK
            : resolver.resolve(resolver.state, &host_request, &host_resource);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, (void *)host_resource.content.bytes);
        return (sxml_status)status;
    }
    if (host_resource.resource_form == SXML_RESOURCE_FORM_BYTES) {
        resource->resource_form = SXML_RESOURCE_FORM_BYTES;
        resource->content = host_resource.content;
    } else if (host_resource.resource_form == SXML_RESOURCE_FORM_BYTE_READER) {
        return sxml_host_reader_resource(processor, &host_resource, resource);
    } else {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        resource->description = SXML_BYTE_VIEW_FROM_LITERAL("The resolver has no such resource");
    }
    return SXML_STATUS_OK;
}

static void sxml_host_release_resource(void *state, const sxml_resource *resource) {
    sxml_host_processor *processor = state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        sxml_allocator_release(processor->allocator, (void *)resource->content.bytes);
    } else if (resource->resource_form == SXML_RESOURCE_FORM_BYTE_READER) {
        sxml_host_reader_release(processor, resource);
    }
}

static const sxml_resource_resolver_operations sxml_host_resolver_operations = {
    .resolve = sxml_host_resolve,
    .release = sxml_host_release_resource,
};

/* --------------------------------------------------------------- Memory */

/*
 * Allocates byte_count bytes (at least 1) from the processor's allocator,
 * aligned for any request struct, or returns NULL when out of memory.
 * A host copies its texts and request structs here.
 */
void *sxml_host_allocate(sxml_host_processor *processor, size_t byte_count) {
    return sxml_host_allocate_bytes(processor->allocator, byte_count == 0 ? 1 : byte_count,
                                    alignof(max_align_t));
}

/* Releases memory from sxml_host_allocate; NULL does nothing. */
void sxml_host_release(sxml_host_processor *processor, void *memory) {
    sxml_allocator_release(processor->allocator, memory);
}

/* The allocations of the processor's allocator not yet released. */
uint32_t sxml_host_live_allocation_count(const sxml_host_processor *processor) {
    return (uint32_t)sxml_counting_allocator_read_counts(processor->counting_allocator)
        .live_allocation_count;
}

/* ------------------------------------------------------ Optional features */

enum { SXML_HOST_MAXIMUM_FEATURE_COUNT = 32 };

/* The link's features, at most SXML_HOST_MAXIMUM_FEATURE_COUNT. */
static sxml_optional_feature_view sxml_host_linked_features(void) {
    sxml_optional_feature_view features = sxml_host_features();
    if (features.optional_feature_count > SXML_HOST_MAXIMUM_FEATURE_COUNT) {
        features.optional_feature_count = SXML_HOST_MAXIMUM_FEATURE_COUNT;
    }
    return features;
}

uint32_t sxml_host_feature_count(void) {
    return (uint32_t)sxml_host_linked_features().optional_feature_count;
}

/* The name of the feature at feature_index; the empty view past the last. */
static sxml_byte_view sxml_host_feature_name(uint32_t feature_index) {
    const sxml_optional_feature_view features = sxml_host_linked_features();
    if (feature_index >= features.optional_feature_count) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return sxml_optional_feature_get_name(features.optional_features[feature_index]);
}

const uint8_t *sxml_host_feature_name_bytes(uint32_t feature_index) {
    return sxml_host_feature_name(feature_index).bytes;
}

uint32_t sxml_host_feature_name_byte_count(uint32_t feature_index) {
    return (uint32_t)sxml_host_feature_name(feature_index).byte_count;
}

/* The features of feature_mask, written to selected (room for
   SXML_HOST_MAXIMUM_FEATURE_COUNT), as a view of it. */
static sxml_optional_feature_view sxml_host_select_features(uint32_t feature_mask,
                                                            sxml_optional_feature *selected) {
    const sxml_optional_feature_view features = sxml_host_linked_features();
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
 * Makes a processor as configuration says: failing_allocation_number
 * counted from the processor's own first allocation, feature_mask over
 * the link's features, resolver asked for every resource. Returns NULL
 * when out of memory.
 */
sxml_host_processor *sxml_host_processor_create(
    const sxml_host_processor_configuration *configuration) {
    const sxml_allocator default_allocator = sxml_default_allocator();
    sxml_host_processor *processor = sxml_host_allocate_bytes(
        default_allocator, sizeof(sxml_host_processor), alignof(sxml_host_processor));
    if (processor == NULL) {
        return NULL;
    }
    memset(processor, 0, sizeof(*processor));
    processor->resolver = configuration->resolver;
    sxml_status status = sxml_counting_allocator_create(
        default_allocator,
        &(sxml_counting_allocator_configuration){
            .backing_allocator = default_allocator,
            .failing_allocation_number = configuration->failing_allocation_number},
        &processor->counting_allocator);
    if (status == SXML_STATUS_OK) {
        processor->allocator = sxml_counting_allocator_as_allocator(processor->counting_allocator);
        sxml_optional_feature selected_features[SXML_HOST_MAXIMUM_FEATURE_COUNT];
        status = sxml_processor_create(
            processor->allocator,
            &(sxml_processor_configuration){
                .allocator = processor->allocator,
                .limits = {.xml_limits = {.maximum_depth = 0,
                                          .maximum_entity_expansion_byte_count =
                                              SXML_HOST_MAXIMUM_EXPANSION_BYTE_COUNT},
                           .maximum_evaluation_step_count =
                               configuration->maximum_evaluation_step_count},
                .resource_resolver = {.state = processor,
                                      .operations = &sxml_host_resolver_operations},
                .clock = sxml_system_clock(),
                .optional_features =
                    sxml_host_select_features(configuration->feature_mask,
                                              selected_features),
                .source_code_languages = configuration->source_code_languages,
                .source_location_policy = configuration->source_location_policy},
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

uint32_t sxml_host_processor_destroy(sxml_host_processor *processor) {
    if (processor == NULL) {
        return 0;
    }
    sxml_item_heap_clear_raised_error(sxml_processor_get_item_heap(processor->processor));
    sxml_serialization_parameter_reader_destroy(processor->parameter_reader);
    sxml_output_buffer_destroy(processor->output_buffer);
    sxml_processor_destroy(processor->processor);
    const uint32_t leaked_allocation_count = sxml_host_live_allocation_count(processor);
    sxml_counting_allocator_destroy(processor->counting_allocator);
    sxml_allocator_release(sxml_default_allocator(), processor);
    return leaked_allocation_count;
}

/* The failure of the processor's last operation. */
const sxml_host_error *sxml_host_error_of(const sxml_host_processor *processor) {
    return &processor->error;
}

/* The bytes of the processor's last serialization. */
const sxml_byte_view *sxml_host_output_of(const sxml_host_processor *processor) {
    return &processor->output;
}

/* The library version as NUL-terminated ASCII ("0.2.0"). */
const char *sxml_host_version_text(void) {
    return sxml_version_text();
}

/* ----------------------------------------------------------------- Values */

/* Releases a value; NULL does nothing. */
void sxml_host_value_release(sxml_host_value *value) {
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
sxml_host_value *sxml_host_parse(sxml_host_processor *processor,
                                 const sxml_host_parse_request *request) {
    sxml_host_begin(processor);
    sxml_item document_node = {.representation = 0};
    const sxml_status status = sxml_processor_parse_document(
        processor->processor,
        &(sxml_xml_parse_request){.input = request->input,
                                  .source_name = request->document_uri,
                                  .document_uri = request->document_uri,
                                  .base_uri = request->document_uri,
                                  .encoding_name = sxml_host_no_text,
                                  .diagnostic = &processor->error_record.diagnostic},
        &document_node);
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
        return NULL;
    }
    return sxml_host_value_make(processor, sxml_sequence_from_item(document_node));
}

/*
 * True when text, well-formed UTF-8, holds a character that is not an XML
 * 1.1 character (XML 1.1, production [2] Char, as fn:codepoints-to-string
 * reads it in F&O 4.0): U+0000, U+FFFE or U+FFFF. Well-formed UTF-8
 * encodes each of them in one way only (RFC 3629, 3), as the bytes 00,
 * EF BF BE and EF BF BF.
 */
static bool sxml_host_has_non_xml_character(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == 0x00) {
            return true;
        }
        if (byte == 0xEF && byte_index + 2 < text.byte_count &&
            text.bytes[byte_index + 1] == 0xBF &&
            (text.bytes[byte_index + 2] == 0xBE || text.bytes[byte_index + 2] == 0xBF)) {
            return true;
        }
    }
    return false;
}

sxml_host_value *sxml_host_string(sxml_host_processor *processor,
                                  const sxml_byte_view *text) {
    sxml_host_begin(processor);
    sxml_string string_text = {.utf8 = sxml_host_no_text, .codepoint_count = 0, .is_ascii = false};
    sxml_status status = sxml_string_from_utf8(*text, &string_text);
    if (status == SXML_STATUS_OK && sxml_host_has_non_xml_character(*text)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_item string = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            sxml_processor_get_item_heap(processor->processor),
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = string_text}}},
            &string);
    }
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
        return NULL;
    }
    return sxml_host_value_make(processor, sxml_sequence_from_item(string));
}

/* ------------------------------------------------------------ Expressions */

/* Destroys an expression; NULL does nothing. */
void sxml_host_expression_destroy(sxml_host_expression *expression) {
    if (expression == NULL) {
        return;
    }
    sxml_xpath_expression_destroy(expression->expression);
    sxml_allocator_release(expression->owner->allocator, expression->variable_names);
    sxml_allocator_release(expression->owner->allocator, expression->bindings);
    sxml_allocator_release(expression->owner->allocator, expression);
}

/* The static context's namespaces and variables, in allocator memory. */
typedef struct sxml_host_static_names {
    sxml_namespace_binding *namespace_bindings;
    sxml_variable_declaration *declarations;
} sxml_host_static_names;

/* Interns the namespaces and variable names of request into names and
   expression->variable_names. */
static sxml_status sxml_host_intern_static_names(
    sxml_host_expression *expression, const sxml_host_compile_request *request,
    sxml_host_static_names *names) {
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(expression->owner->processor);
    sxml_status status = SXML_STATUS_OK;
    for (size_t namespace_index = 0;
         status == SXML_STATUS_OK && namespace_index < request->namespace_count;
         namespace_index += 1) {
        const sxml_host_namespace *source = &request->namespaces[namespace_index];
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
        const sxml_host_variable *source = &request->variables[variable_index];
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
sxml_host_expression *sxml_host_compile(
    sxml_host_processor *processor, const sxml_host_compile_request *request) {
    sxml_host_begin(processor);
    const sxml_allocator allocator = processor->allocator;
    sxml_host_expression *expression = sxml_host_allocate_bytes(
        allocator, sizeof(sxml_host_expression), alignof(sxml_host_expression));
    if (expression == NULL) {
        sxml_host_fail(processor, SXML_STATUS_OUT_OF_MEMORY);
        return NULL;
    }
    *expression = (sxml_host_expression){.owner = processor,
                                               .expression = NULL,
                                               .variable_names = NULL,
                                               .bindings = NULL,
                                               .variable_count = request->variable_count};
    sxml_host_static_names names = {.namespace_bindings = NULL, .declarations = NULL};
    void *memory = NULL;
    sxml_status status = sxml_host_allocate_array(
        allocator,
        (sxml_host_array_request){
            .element = {.byte_count = sizeof(sxml_namespace_binding), .alignment = alignof(sxml_namespace_binding)},
            .element_count = request->namespace_count},
        &memory);
    names.namespace_bindings = memory;
    if (status == SXML_STATUS_OK) {
        status = sxml_host_allocate_array(
            allocator,
            (sxml_host_array_request){
                .element = {.byte_count = sizeof(sxml_variable_declaration),
                            .alignment = alignof(sxml_variable_declaration)},
                .element_count = request->variable_count},
            &memory);
        names.declarations = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_host_allocate_array(
            allocator,
            (sxml_host_array_request){
                .element = {.byte_count = sizeof(sxml_variable_binding),
                            .alignment = alignof(sxml_variable_binding)},
                .element_count = request->variable_count},
            &memory);
        expression->bindings = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_host_allocate_array(
            allocator,
            (sxml_host_array_request){
                .element = {.byte_count = sizeof(sxml_name_identifier),
                            .alignment = alignof(sxml_name_identifier)},
                .element_count = request->variable_count},
            &memory);
        expression->variable_names = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_host_intern_static_names(expression, request, &names);
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
        sxml_host_expression_destroy(expression);
        sxml_host_fail(processor, status);
        return NULL;
    }
    return expression;
}

/*
 * Evaluates an expression and returns the result as a value, or NULL with
 * the processor's error set. request->variable_value_count must be the
 * expression's variable count (SXML_STATUS_INVALID_ARGUMENT otherwise).
 */
sxml_host_value *sxml_host_evaluate(sxml_host_expression *expression,
                                    const sxml_host_evaluate_request *request) {
    sxml_host_processor *processor = expression->owner;
    sxml_host_begin(processor);
    if (request->variable_value_count != expression->variable_count) {
        sxml_host_fail(processor, SXML_STATUS_INVALID_ARGUMENT);
        return NULL;
    }
    size_t bound_count = 0;
    for (size_t variable_index = 0; variable_index < expression->variable_count;
         variable_index += 1) {
        const sxml_host_value *value = request->variable_values[variable_index];
        if (value != NULL) {
            /* Unbound variables are left out, so they raise XPDY0002. */
            expression->bindings[bound_count] = (sxml_variable_binding){
                .variable_name = expression->variable_names[variable_index],
                .value = value->sequence};
            bound_count += 1;
        }
    }
    const sxml_host_value *context = request->context_value;
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
        sxml_host_fail(processor, status);
        return NULL;
    }
    return sxml_host_value_make(processor, result);
}

/* ---------------------------------------------------------- Serialization */

/*
 * Serializes request->value (Serialization 4.0) into the processor's
 * output (sxml_host_output_of): by the adaptive method when there
 * are no options, else as fn:serialize does with request->options (F&O
 * 4.0, "fn:serialize": method xml unless the options name another).
 * Returns the status; on failure the processor's error is set.
 */
uint32_t sxml_host_serialize(sxml_host_processor *processor,
                             const sxml_host_serialize_request *request) {
    sxml_host_begin(processor);
    sxml_output_stream output_stream;
    sxml_status status = sxml_host_output_begin(processor, &output_stream);
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_ADAPTIVE;
    if (status == SXML_STATUS_OK && request->options != NULL) {
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
            output_stream);
    }
    status = sxml_host_output_end(processor, status);
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
        return (uint32_t)status;
    }
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------------- Streams */

/* A chunk's size when the configuration gives none. */
enum { SXML_HOST_DEFAULT_CHUNK_BYTE_COUNT = 64 * 1024 };

uint32_t sxml_host_set_output(sxml_host_processor *processor,
                              const sxml_host_output_configuration *configuration) {
    processor->output_writer = configuration->writer;
    processor->chunk_capacity = configuration->chunk_byte_count == 0
                                    ? (size_t)SXML_HOST_DEFAULT_CHUNK_BYTE_COUNT
                                    : (size_t)configuration->chunk_byte_count;
    return SXML_STATUS_OK;
}

uint32_t sxml_host_set_byte_reader(sxml_host_processor *processor,
                                   const sxml_host_byte_reader *reader) {
    processor->byte_reader = *reader;
    return SXML_STATUS_OK;
}

bool sxml_host_output_is_written(const sxml_host_processor *processor) {
    return processor->output_writer.write != NULL;
}

/* Hands event to the output writer. */
static sxml_status sxml_host_output_send(const sxml_host_processor *processor,
                                         const sxml_host_output_event *event) {
    const sxml_host_output_writer writer = processor->output_writer;
    return (sxml_status)writer.write(writer.state, event);
}

/* Writes bytes as one BYTES event. */
static sxml_status sxml_host_output_send_bytes(const sxml_host_processor *processor,
                                               sxml_byte_view bytes) {
    return sxml_host_output_send(
        processor, &(sxml_host_output_event){.output_event_kind =
                                                 (uint32_t)SXML_HOST_OUTPUT_EVENT_KIND_BYTES,
                                             .bytes = bytes,
                                             .is_abandoned = 0});
}

/* Writes the chunk's bytes, if any, and empties it. */
static sxml_status sxml_host_output_flush(sxml_host_processor *processor) {
    if (processor->chunk_byte_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view bytes = {.bytes = processor->chunk,
                                  .byte_count = processor->chunk_byte_count};
    processor->chunk_byte_count = 0;
    return sxml_host_output_send_bytes(processor, bytes);
}

/*
 * The chunk stream's write (include/sxml/serialize.h): gathers the
 * serializer's small writes in the chunk and writes it when it is full; a
 * write that finds the chunk empty and is at least a chunk long is written
 * a chunk at a time from where it is, without a copy.
 */
static sxml_status sxml_host_chunk_write(void *state, sxml_byte_view bytes) {
    sxml_host_processor *processor = state;
    const size_t capacity = processor->chunk_capacity;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && offset < bytes.byte_count) {
        const size_t remaining = bytes.byte_count - offset;
        if (processor->chunk_byte_count == 0 && remaining >= capacity) {
            status = sxml_host_output_send_bytes(
                processor, (sxml_byte_view){.bytes = bytes.bytes + offset, .byte_count = capacity});
            offset += capacity;
            continue;
        }
        const size_t room = capacity - processor->chunk_byte_count;
        const size_t copied = remaining < room ? remaining : room;
        memcpy(processor->chunk + processor->chunk_byte_count, bytes.bytes + offset, copied);
        processor->chunk_byte_count += copied;
        offset += copied;
        if (processor->chunk_byte_count == capacity) {
            status = sxml_host_output_flush(processor);
        }
    }
    return status;
}

static const sxml_output_stream_operations sxml_host_chunk_stream_operations = {
    .write = sxml_host_chunk_write,
};

sxml_status sxml_host_output_begin(sxml_host_processor *processor,
                                   sxml_output_stream *output_stream) {
    sxml_output_buffer_clear(processor->output_buffer);
    processor->output = sxml_host_no_text;
    if (!sxml_host_output_is_written(processor)) {
        *output_stream = sxml_output_buffer_as_output_stream(processor->output_buffer);
        return SXML_STATUS_OK;
    }
    *output_stream = (sxml_output_stream){.state = processor,
                                          .operations = &sxml_host_chunk_stream_operations};
    processor->chunk_byte_count = 0;
    processor->chunk = sxml_host_allocate_bytes(processor->allocator, processor->chunk_capacity,
                                                alignof(max_align_t));
    return processor->chunk == NULL ? SXML_STATUS_OUT_OF_MEMORY : SXML_STATUS_OK;
}

sxml_status sxml_host_output_end(sxml_host_processor *processor, sxml_status status) {
    if (!sxml_host_output_is_written(processor)) {
        if (status == SXML_STATUS_OK) {
            processor->output = sxml_output_buffer_get_bytes(processor->output_buffer);
        }
        return status;
    }
    if (status == SXML_STATUS_OK && processor->chunk != NULL) {
        status = sxml_host_output_flush(processor);
    }
    sxml_allocator_release(processor->allocator, processor->chunk);
    processor->chunk = NULL;
    processor->chunk_byte_count = 0;
    return status;
}

sxml_status sxml_host_output_mark(sxml_host_processor *processor,
                                  const sxml_host_output_event *event, bool discards_chunk) {
    sxml_status status = SXML_STATUS_OK;
    if (discards_chunk) {
        processor->chunk_byte_count = 0;
    } else {
        status = sxml_host_output_flush(processor);
    }
    return status == SXML_STATUS_OK ? sxml_host_output_send(processor, event) : status;
}

/* A byte reader over the processor's (include/sxml/processor.h): the
   state of one delivered document. */
typedef struct sxml_host_reader_state {
    sxml_host_processor *processor;
    uint32_t reader_identifier;
} sxml_host_reader_state;

/* Reads the next bytes through the host's reader into the lent buffer. */
static sxml_status sxml_host_reader_read(void *state, sxml_memory_view buffer,
                                         sxml_byte_reading *reading) {
    const sxml_host_reader_state *reader_state = state;
    const sxml_host_byte_reader reader = reader_state->processor->byte_reader;
    sxml_host_read_request request = {.reader_identifier = reader_state->reader_identifier,
                                      .buffer = buffer,
                                      .byte_count = 0,
                                      .is_end = 0};
    const sxml_status status = (sxml_status)reader.read(reader.state, &request);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reading->byte_count =
        request.byte_count < buffer.byte_count ? (size_t)request.byte_count : buffer.byte_count;
    reading->is_end = request.is_end != 0 || reading->byte_count == 0;
    return SXML_STATUS_OK;
}

static const sxml_byte_reader_operations sxml_host_reader_operations = {
    .read = sxml_host_reader_read,
};

sxml_status sxml_host_reader_resource(sxml_host_processor *processor,
                                      const sxml_host_resource *host_resource,
                                      sxml_resource *resource) {
    const sxml_host_byte_reader reader = processor->byte_reader;
    sxml_host_reader_state *reader_state =
        reader.read == NULL
            ? NULL
            : sxml_host_allocate_bytes(processor->allocator, sizeof(sxml_host_reader_state),
                                       alignof(sxml_host_reader_state));
    if (reader_state == NULL) {
        if (reader.release != NULL) {
            reader.release(reader.state, host_resource->reader_identifier);
        }
        if (reader.read != NULL) {
            return SXML_STATUS_OUT_OF_MEMORY;
        }
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        resource->description = SXML_BYTE_VIEW_FROM_LITERAL("The host has no byte reader");
        return SXML_STATUS_OK;
    }
    *reader_state = (sxml_host_reader_state){.processor = processor,
                                             .reader_identifier = host_resource->reader_identifier};
    resource->resource_form = SXML_RESOURCE_FORM_BYTE_READER;
    resource->byte_reader =
        (sxml_byte_reader){.state = reader_state, .operations = &sxml_host_reader_operations};
    return SXML_STATUS_OK;
}

void sxml_host_reader_release(sxml_host_processor *processor, const sxml_resource *resource) {
    sxml_host_reader_state *reader_state = resource->byte_reader.state;
    const sxml_host_byte_reader reader = processor->byte_reader;
    if (reader.release != NULL) {
        reader.release(reader.state, reader_state->reader_identifier);
    }
    sxml_allocator_release(processor->allocator, reader_state);
}

/* ----------------------------------------------------------------- Layout */

#if defined(__wasm32__)
/* The JavaScript package (src/index.ts) reads and writes these as 32-bit
   words at these offsets. */
_Static_assert(sizeof(sxml_byte_view) == 8, "a byte view is two words");
_Static_assert(sizeof(sxml_host_error) == 60, "error: 15 words");
_Static_assert(offsetof(sxml_host_error, line_number) == 52, "error line");
_Static_assert(sizeof(sxml_host_resource_request) == 24, "resource request: 6 words");
_Static_assert(sizeof(sxml_host_parse_request) == 16, "parse request: 4 words");
_Static_assert(sizeof(sxml_host_namespace) == 16, "namespace: 4 words");
_Static_assert(sizeof(sxml_host_variable) == 16, "variable: 4 words");
_Static_assert(sizeof(sxml_host_compile_request) == 32, "compile request: 8 words");
_Static_assert(sizeof(sxml_host_evaluate_request) == 12, "evaluate request: 3 words");
_Static_assert(sizeof(sxml_host_serialize_request) == 8, "serialize request: 2 words");
_Static_assert(sizeof(sxml_host_resource) == 16, "resource: 4 words");
_Static_assert(sizeof(sxml_host_output_event) == 16, "output event: 4 words");
_Static_assert(sizeof(sxml_host_read_request) == 20, "read request: 5 words");
#endif
