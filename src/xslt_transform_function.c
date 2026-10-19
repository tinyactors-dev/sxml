/*
 * fn:transform (F&O 4.0, 18.3; docs/DECISIONS.md, D203): the optional
 * feature sxml_xslt_transform_feature() and the function's resumable
 * body. A program that never names the feature does not link XSLT
 * through it (D189, D191): without it the function is absent (XPST0017).
 *
 * The body never nests the evaluator in C (D71). Its first step reads the
 * options, compiles the stylesheet with include/sxml/xslt.h's compiler
 * and primes the transformation (src/xslt_transform_internal.h), then asks
 * the machine for a unit evaluation (glossary `unit_evaluation`, D154), as
 * fn:load-xquery-module and xsl:evaluate do. The second step delivers the
 * raw result as the delivery format asks and collects the result
 * documents; with post-process, each later step calls the function on
 * one result. The last makes the result map.
 *
 * Compiling evaluates the stylesheet's static expressions (use-when,
 * static parameters) on an evaluator of their own, which has no
 * evaluation environment: fn:transform there raises FOXT0004, so the
 * compiler is never re-entered from its own evaluation.
 *
 * Errors: FOXT0001 for an XSLT version above 4.0 or requested properties
 * sxml does not have; FOXT0002 for options in conflict, a missing
 * stylesheet or invocation, a delivery format outside the list and a
 * stylesheet location that cannot be read; FOXT0003 for a stylesheet that
 * is not well-formed XML; FOXT0004 for delivery-format "file" and for a
 * call without an evaluation environment (a static expression); the
 * stylesheet's static and dynamic errors keep their codes, raised as
 * dynamic errors.
 *
 * Golden file: src/xquery_load_module.c (a resumable body that compiles,
 * then asks for a unit evaluation).
 */
#include "xslt_transform_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "memory_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "system_property_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/string.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

static const sxml_static_error no_processor_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOXT0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "no XSLT processor with the requested version or properties is available "
        "(F&O 4.0, 18.3)")};

static const sxml_static_error option_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOXT0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the options of fn:transform are in conflict, incomplete or invalid (F&O 4.0, 18.3)")};

static const sxml_static_error location_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOXT0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the stylesheet or source location cannot be read (F&O 4.0, 18.3)")};

static const sxml_static_error malformed_stylesheet_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOXT0003"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the stylesheet is not well-formed XML (F&O 4.0, 18.3)")};

static const sxml_static_error disabled_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOXT0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:transform is not available here: in a static expression, or with "
        "delivery-format \"file\" (F&O 4.0, 18.3)")};

static const sxml_static_error character_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOXT0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "a serialized result is not text the caller accepts (F&O 4.0, 18.3)")};

static const sxml_static_error key_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "a key of a parameter map of fn:transform is not an xs:QName (F&O 4.0, 18.3)")};

/* The source name of errors located in memory the call releases. */
static const sxml_byte_view call_source_name = SXML_BYTE_VIEW_INITIALIZER("fn:transform");

/* ---------------------------------------------------------------- Options */

/* The indices of option_definitions (F&O 4.0, 18.3, the option table). */
typedef enum transform_option {
    SXML_TRANSFORM_OPTION_BASE_OUTPUT_URI,
    SXML_TRANSFORM_OPTION_CACHE,
    SXML_TRANSFORM_OPTION_DELIVERY_FORMAT,
    SXML_TRANSFORM_OPTION_ENABLE_ASSERTIONS,
    SXML_TRANSFORM_OPTION_ENABLE_MESSAGES,
    SXML_TRANSFORM_OPTION_ENABLE_TRACE,
    SXML_TRANSFORM_OPTION_FUNCTION_PARAMS,
    SXML_TRANSFORM_OPTION_GLOBAL_CONTEXT_ITEM,
    SXML_TRANSFORM_OPTION_INITIAL_FUNCTION,
    SXML_TRANSFORM_OPTION_INITIAL_MATCH_SELECTION,
    SXML_TRANSFORM_OPTION_INITIAL_MODE,
    SXML_TRANSFORM_OPTION_INITIAL_TEMPLATE,
    SXML_TRANSFORM_OPTION_PACKAGE_LOCATION,
    SXML_TRANSFORM_OPTION_PACKAGE_NAME,
    SXML_TRANSFORM_OPTION_PACKAGE_NODE,
    SXML_TRANSFORM_OPTION_PACKAGE_TEXT,
    SXML_TRANSFORM_OPTION_PACKAGE_VERSION,
    SXML_TRANSFORM_OPTION_POST_PROCESS,
    SXML_TRANSFORM_OPTION_REQUESTED_PROPERTIES,
    SXML_TRANSFORM_OPTION_SERIALIZATION_PARAMS,
    SXML_TRANSFORM_OPTION_SOURCE_LOCATION,
    SXML_TRANSFORM_OPTION_SOURCE_NODE,
    SXML_TRANSFORM_OPTION_STATIC_PARAMS,
    SXML_TRANSFORM_OPTION_STYLESHEET_BASE_URI,
    SXML_TRANSFORM_OPTION_STYLESHEET_LOCATION,
    SXML_TRANSFORM_OPTION_STYLESHEET_NODE,
    SXML_TRANSFORM_OPTION_STYLESHEET_PARAMS,
    SXML_TRANSFORM_OPTION_STYLESHEET_TEXT,
    SXML_TRANSFORM_OPTION_TEMPLATE_PARAMS,
    SXML_TRANSFORM_OPTION_TRUSTED,
    SXML_TRANSFORM_OPTION_TUNNEL_PARAMS,
    SXML_TRANSFORM_OPTION_VENDOR_OPTIONS,
    SXML_TRANSFORM_OPTION_XSLT_VERSION,
    SXML_TRANSFORM_OPTION_COUNT,
} transform_option;

static const sxml_option_definition option_definitions[] = {
    SXML_FUNCTION_OPTION("base-output-uri", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("cache", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("delivery-format", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("enable-assertions", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("enable-messages", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("enable-trace", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("function-params", SXML_TYPE_ARRAY, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("global-context-item", SXML_TYPE_ITEM, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("initial-function", SXML_TYPE_XS_QNAME, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("initial-match-selection", SXML_TYPE_ITEM,
                         SXML_OCCURRENCE_ZERO_OR_MORE),
    SXML_FUNCTION_OPTION("initial-mode", SXML_TYPE_XS_QNAME, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("initial-template", SXML_TYPE_XS_QNAME, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("package-location", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("package-name", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("package-node", SXML_TYPE_NODE, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("package-text", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("package-version", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("post-process", SXML_TYPE_FUNCTION, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("requested-properties", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("serialization-params", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("source-location", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("source-node", SXML_TYPE_NODE, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("static-params", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("stylesheet-base-uri", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("stylesheet-location", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("stylesheet-node", SXML_TYPE_NODE, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("stylesheet-params", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("stylesheet-text", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("template-params", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("trusted", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("tunnel-params", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("vendor-options", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("xslt-version", SXML_TYPE_XS_DECIMAL, SXML_OCCURRENCE_EXACTLY_ONE),
};

_Static_assert(sizeof option_definitions / sizeof option_definitions[0] ==
                   SXML_TRANSFORM_OPTION_COUNT,
               "one definition per option");

/* ------------------------------------------------------------------ State */

/* The bindings read from one parameter map; values borrowed from it. */
typedef struct parameter_bindings {
    sxml_variable_binding *bindings;
    size_t binding_count;
} parameter_bindings;

/* The indices of transform_state.parameters. */
typedef enum parameter_map_kind {
    SXML_PARAMETER_MAP_STATIC,
    SXML_PARAMETER_MAP_STYLESHEET,
    SXML_PARAMETER_MAP_TEMPLATE,
    SXML_PARAMETER_MAP_TUNNEL,
    SXML_PARAMETER_MAP_COUNT,
} parameter_map_kind;

/* One entry of the result map; both owned. */
typedef struct result_entry {
    sxml_item key;
    sxml_sequence value;
} result_entry;

/* A URI resolved for the call; memory owned when not NULL. */
typedef struct resolved_uri {
    sxml_byte_view uri;
    void *memory;
} resolved_uri;

/* What the call holds between its steps. */
typedef struct transform_state {
    sxml_processor *processor;
    sxml_dynamic_context *dynamic_context;
    sxml_error_record *error_record;
    /* Owned values of the options. */
    sxml_option_values options;
    /* The stylesheet's base URI and the base output URI, resolved. */
    resolved_uri stylesheet_base_uri;
    resolved_uri base_output_uri;
    /* A stylesheet node serialized, the principal module's text. */
    sxml_output_buffer *stylesheet_text;
    /* Owned: the document of source-location. */
    sxml_sequence source_document;
    parameter_bindings parameters[SXML_PARAMETER_MAP_COUNT];
    /* The members of function-params, borrowed from the option. */
    sxml_sequence *function_arguments;
    size_t function_argument_count;
    /* Owned: the global context item. */
    sxml_sequence global_context_item;
    sxml_xslt_stylesheet *stylesheet;
    sxml_xslt_result_document_collector *collector;
    /* The serialized principal result. */
    sxml_output_buffer *principal_output;
    /* Keeps the texts of the merged serialization parameters. */
    sxml_serialization_parameter_reader *parameter_reader;
    sxml_xslt_output_definition output_definition;
    sxml_xslt_result_delivery result_delivery;
    sxml_xslt_transformation_request request;
    sxml_xslt_machine_transformation *running;
    result_entry *entries;
    size_t entry_count;
    /* The entry the post-process call running has as its argument. */
    size_t post_processed_count;
    sxml_sequence call_arguments[2];
} transform_state;

/* True when text lies in memory the state owns or borrowed from its
   options, which an error location may name. */
static bool state_holds_text(const transform_state *state, sxml_byte_view text) {
    const sxml_byte_view held[] = {
        state->stylesheet_base_uri.uri,
        state->base_output_uri.uri,
        sxml_function_library_option_string(
            (sxml_option_lookup){.values = &state->options,
                                 .option_index = SXML_TRANSFORM_OPTION_PACKAGE_NAME})
            .utf8};
    for (size_t held_index = 0; held_index < sizeof held / sizeof held[0]; held_index += 1) {
        if (held[held_index].bytes != NULL && text.bytes >= held[held_index].bytes &&
            text.bytes < held[held_index].bytes + held[held_index].byte_count + 1) {
            return true;
        }
    }
    return sxml_xslt_stylesheet_holds_text(state->stylesheet, text);
}

static void release_transform_state(void *opaque_state) {
    transform_state *state = opaque_state;
    if (state->processor == NULL) {
        return;
    }
    const sxml_allocator allocator = state->processor->allocator;
    if (state->running != NULL && state->dynamic_context != NULL) {
        sxml_dynamic_context_drop_unit_extension(
            state->dynamic_context, sxml_xslt_machine_transformation_get_extension(state->running));
    }
    sxml_xslt_machine_transformation_destroy(state->running);
    state->running = NULL;
    sxml_error_record *record = state->error_record;
    if (record != NULL && record->diagnostic.location.source_name.byte_count != 0 &&
        state_holds_text(state, record->diagnostic.location.source_name)) {
        record->diagnostic.location.source_name = call_source_name;
    }
    for (size_t entry_index = 0; entry_index < state->entry_count; entry_index += 1) {
        sxml_item_release(state->entries[entry_index].key);
        sxml_sequence_release(state->entries[entry_index].value);
    }
    sxml_allocator_release(allocator, state->entries);
    for (size_t argument_index = 0; argument_index < 2; argument_index += 1) {
        sxml_sequence_release(state->call_arguments[argument_index]);
    }
    sxml_xslt_result_document_collector_destroy(state->collector);
    sxml_output_buffer_destroy(state->principal_output);
    sxml_serialization_parameter_reader_destroy(state->parameter_reader);
    sxml_xslt_stylesheet_destroy(state->stylesheet);
    sxml_output_buffer_destroy(state->stylesheet_text);
    sxml_sequence_release(state->global_context_item);
    sxml_sequence_release(state->source_document);
    for (size_t map_index = 0; map_index < SXML_PARAMETER_MAP_COUNT; map_index += 1) {
        sxml_allocator_release(allocator, state->parameters[map_index].bindings);
    }
    sxml_allocator_release(allocator, state->function_arguments);
    sxml_allocator_release(allocator, state->stylesheet_base_uri.memory);
    sxml_allocator_release(allocator, state->base_output_uri.memory);
    sxml_function_library_release_options(&state->options);
    memset(state, 0, sizeof(*state));
}

/* -------------------------------------------------------------- Helpers */

static bool has_option(const transform_state *state, transform_option option) {
    return state->options.is_present[option];
}

static sxml_sequence option_value(const transform_state *state, transform_option option) {
    return state->options.values[option];
}

static sxml_byte_view option_text(const transform_state *state, transform_option option) {
    return sxml_function_library_option_string(
               (sxml_option_lookup){.values = &state->options, .option_index = option})
        .utf8;
}

/* The first item of a present option. */
static sxml_item option_item(const transform_state *state, transform_option option) {
    sxml_item item = {.representation = 0};
    if (sxml_sequence_get_item(option_value(state, option),
                               (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &item) != SXML_STATUS_OK) {
        item.representation = 0;
    }
    return item;
}

/* The datum of an option of one atomic item. */
static sxml_atomic_datum option_datum(const transform_state *state, transform_option option) {
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (sxml_atomic_item_get_datum(option_item(state, option), &datum) != SXML_STATUS_OK) {
        memset(&datum, 0, sizeof(datum));
    }
    return datum;
}

static sxml_name_identifier option_name(const transform_state *state, transform_option option) {
    if (!has_option(state, option)) {
        return (sxml_name_identifier){.index = 0};
    }
    return option_datum(state, option).datum_details.qname.expanded_name;
}

/* Resolves reference against the static base URI of the call (the
   executable base URI, F&O 4.0, 18.3). */
static sxml_status resolve_against_call(const sxml_function_call *call, sxml_byte_view reference,
                                        resolved_uri *resolved) {
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
    const sxml_status status = sxml_uri_resolve(
        &(sxml_uri_resolution_request){
            .base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context),
            .reference = reference,
            .allocator = call->dynamic_context->allocator},
        &resolution);
    if (status == SXML_STATUS_OK) {
        *resolved = (resolved_uri){.uri = resolution.uri, .memory = resolution.memory};
    }
    return status;
}

/* Reads a map(xs:QName, item()*) option into bindings (values borrowed
   from the map): XPTY0004 for another key. */
typedef struct parameter_map_request {
    const sxml_function_call *call;
    transform_option option;
    parameter_map_kind map_kind;
} parameter_map_request;

static sxml_status read_parameter_map(transform_state *state,
                                      const parameter_map_request *request) {
    if (!has_option(state, request->option)) {
        return SXML_STATUS_OK;
    }
    const sxml_item map = option_item(state, request->option);
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    if (status != SXML_STATUS_OK || entry_count == 0) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate_array(
        state->processor->allocator,
        (sxml_array_allocation_request){.element_count = entry_count,
                                        .element_size = sizeof(sxml_variable_binding),
                                        .element_alignment = alignof(sxml_variable_binding)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    parameter_bindings *bindings = &state->parameters[request->map_kind];
    bindings->bindings = memory;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry = {.key = {.representation = 0}};
        status = sxml_map_get_entry(map, entry_index, &entry);
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof(datum));
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(entry.key, &datum);
        }
        if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_QNAME) {
            status = sxml_function_library_report(request->call, &key_error);
        }
        if (status == SXML_STATUS_OK) {
            bindings->bindings[bindings->binding_count] = (sxml_variable_binding){
                .variable_name = datum.datum_details.qname.expanded_name, .value = entry.value};
            bindings->binding_count += 1;
        }
    }
    return status;
}

/* The members of function-params as the initial function's arguments. */
static sxml_status read_function_arguments(transform_state *state) {
    if (!has_option(state, SXML_TRANSFORM_OPTION_FUNCTION_PARAMS)) {
        return SXML_STATUS_OK;
    }
    const sxml_item array = option_item(state, SXML_TRANSFORM_OPTION_FUNCTION_PARAMS);
    size_t member_count = 0;
    sxml_status status = sxml_array_get_size(array, &member_count);
    if (status != SXML_STATUS_OK || member_count == 0) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate_array(
        state->processor->allocator,
        (sxml_array_allocation_request){.element_count = member_count,
                                        .element_size = sizeof(sxml_sequence),
                                        .element_alignment = alignof(sxml_sequence)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->function_arguments = memory;
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        status = sxml_array_get_member(array, member_index,
                                       &state->function_arguments[member_index]);
        if (status == SXML_STATUS_OK) {
            state->function_argument_count += 1;
        }
    }
    return status;
}

/* The root of the tree containing node, owned. */
static sxml_status root_of(sxml_item node_item, sxml_sequence *root) {
    sxml_node_reference node;
    sxml_status status = sxml_item_get_node(node_item, &node);
    while (status == SXML_STATUS_OK) {
        sxml_node_description description;
        status = sxml_node_describe(node, &description);
        if (status != SXML_STATUS_OK || !description.has_parent) {
            break;
        }
        node = (sxml_node_reference){
            .tree = node.tree, .node_index = description.parent, .namespace_number = 0};
    }
    if (status == SXML_STATUS_OK) {
        *root = sxml_sequence_from_item(sxml_node_item_make(node));
    }
    return status;
}

static bool is_same_node(sxml_item first, sxml_sequence second) {
    sxml_item second_item = {.representation = 0};
    return sxml_sequence_get_item(second,
                                  (sxml_item_request){.item_index = 0, .error_record = NULL},
                                  &second_item) == SXML_STATUS_OK &&
           sxml_generic_node_is_same_node((sxml_item_pair){.first = first, .second = second_item});
}

/* --------------------------------------------------------- Checks (18.3) */

/* The checks of the options before anything is compiled. */
static sxml_status check_options(const sxml_function_call *call, const transform_state *state) {
    /* Exactly one stylesheet. */
    static const transform_option stylesheet_options[] = {
        SXML_TRANSFORM_OPTION_STYLESHEET_LOCATION, SXML_TRANSFORM_OPTION_STYLESHEET_NODE,
        SXML_TRANSFORM_OPTION_STYLESHEET_TEXT,     SXML_TRANSFORM_OPTION_PACKAGE_LOCATION,
        SXML_TRANSFORM_OPTION_PACKAGE_NODE,        SXML_TRANSFORM_OPTION_PACKAGE_TEXT,
        SXML_TRANSFORM_OPTION_PACKAGE_NAME};
    size_t stylesheet_count = 0;
    for (size_t option_index = 0;
         option_index < sizeof stylesheet_options / sizeof stylesheet_options[0];
         option_index += 1) {
        stylesheet_count += has_option(state, stylesheet_options[option_index]) ? 1U : 0U;
    }
    /* At most one of the sources of an apply-templates invocation. */
    const size_t source_count =
        (has_option(state, SXML_TRANSFORM_OPTION_SOURCE_NODE) ? 1U : 0U) +
        (has_option(state, SXML_TRANSFORM_OPTION_SOURCE_LOCATION) ? 1U : 0U) +
        (has_option(state, SXML_TRANSFORM_OPTION_INITIAL_MATCH_SELECTION) ? 1U : 0U);
    const bool calls_function = has_option(state, SXML_TRANSFORM_OPTION_INITIAL_FUNCTION);
    const bool calls_template = !calls_function &&
                                has_option(state, SXML_TRANSFORM_OPTION_INITIAL_TEMPLATE);
    if (stylesheet_count != 1 || source_count > 1 ||
        (calls_function && !has_option(state, SXML_TRANSFORM_OPTION_FUNCTION_PARAMS)) ||
        ((calls_function || calls_template) &&
         has_option(state, SXML_TRANSFORM_OPTION_INITIAL_MODE)) ||
        (has_option(state, SXML_TRANSFORM_OPTION_PACKAGE_VERSION) &&
         !has_option(state, SXML_TRANSFORM_OPTION_PACKAGE_NAME))) {
        return sxml_function_library_report(call, &option_error);
    }
    return SXML_STATUS_OK;
}

/* True when an xs:decimal names a version above 4.0. */
static bool is_above_version_four(sxml_atomic_datum datum) {
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        return datum.datum_details.integer > 4;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_DECIMAL) {
        return false;
    }
    const sxml_byte_view text = datum.datum_details.decimal_text;
    if (text.byte_count == 0 || text.bytes[0] == '-') {
        return false;
    }
    size_t whole = 0;
    size_t byte_index = 0;
    for (; byte_index < text.byte_count && text.bytes[byte_index] != '.'; byte_index += 1) {
        whole = whole > 1000 ? whole : whole * 10 + (size_t)(text.bytes[byte_index] - '0');
    }
    if (whole != 4) {
        return whole > 4;
    }
    for (byte_index += 1; byte_index < text.byte_count; byte_index += 1) {
        if (text.bytes[byte_index] != '0') {
            return true;
        }
    }
    return false;
}

/* The text of a requested or actual property value: "yes"/"no" for a
   boolean (F&O 4.0, 18.3: true and false equal "yes" and "no"). */
static sxml_status append_property_value(sxml_library_text *text, sxml_item value) {
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    sxml_status status = sxml_atomic_item_get_datum(value, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
        return sxml_function_library_text_append(
            text, datum.datum_details.boolean ? SXML_BYTE_VIEW_FROM_LITERAL("yes")
                                              : SXML_BYTE_VIEW_FROM_LITERAL("no"));
    }
    return sxml_function_library_text_append_atomic(text, value);
}

/* Whether sxml has one requested property (F&O 4.0, 18.3,
   requested-properties): its XSLT system property (XSLT 4.0, 24.2.1) has
   the value asked for. xsl:version is ignored; a property sxml does not
   know, or computes, does not match. */
typedef struct property_check {
    const sxml_function_call *call;
    sxml_map_entry entry;
} property_check;

static sxml_status check_property(const property_check *check, bool *is_met) {
    *is_met = false;
    sxml_atomic_datum key;
    memset(&key, 0, sizeof(key));
    sxml_status status = sxml_atomic_item_get_datum(check->entry.key, &key);
    if (status != SXML_STATUS_OK || key.datum_kind != SXML_DATUM_KIND_QNAME) {
        return status == SXML_STATUS_OK ? sxml_function_library_report(check->call, &key_error)
                                        : status;
    }
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(check->call->item_heap);
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    status = sxml_name_pool_get_name_parts(name_pool, key.datum_details.qname.expanded_name,
                                           &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (parts.namespace_uri.index !=
        sxml_well_known_namespace_identifier(SXML_NAMESPACE_XSLT).index) {
        return SXML_STATUS_OK;
    }
    if (sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("version"))) {
        *is_met = true;
        return SXML_STATUS_OK;
    }
    const sxml_system_property_view properties = sxml_system_properties();
    const sxml_system_property *found = NULL;
    for (size_t property_index = 0; property_index < properties.property_count;
         property_index += 1) {
        if (properties.properties[property_index].value_kind == SXML_SYSTEM_PROPERTY_VALUE_GIVEN &&
            sxml_byte_view_equals(properties.properties[property_index].xslt_name,
                                  parts.local_name)) {
            found = &properties.properties[property_index];
        }
    }
    sxml_item requested = {.representation = 0};
    if (found == NULL ||
        sxml_sequence_get_item(check->entry.value,
                               (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &requested) != SXML_STATUS_OK) {
        return SXML_STATUS_OK;
    }
    sxml_item actual = {.representation = 0};
    status = sxml_atomic_item_make(
        check->call->item_heap,
        &(sxml_atomic_item_request){.type = sxml_builtin_type_identifier(found->value_type),
                                    .datum = found->value},
        &actual);
    sxml_library_text requested_text = {.item_heap = check->call->item_heap};
    sxml_library_text actual_text = {.item_heap = check->call->item_heap};
    if (status == SXML_STATUS_OK) {
        status = append_property_value(&requested_text, requested);
    }
    if (status == SXML_STATUS_OK) {
        status = append_property_value(&actual_text, actual);
    }
    if (status == SXML_STATUS_OK) {
        *is_met = sxml_byte_view_equals(sxml_function_library_text_view(&requested_text),
                                        sxml_function_library_text_view(&actual_text));
    }
    sxml_function_library_text_release(&requested_text);
    sxml_function_library_text_release(&actual_text);
    sxml_item_release(actual);
    return status;
}

/* FOXT0001 for an XSLT version above 4.0 or a requested property sxml
   does not have (F&O 4.0, 18.3). */
static sxml_status check_processor(const sxml_function_call *call, const transform_state *state) {
    if (has_option(state, SXML_TRANSFORM_OPTION_XSLT_VERSION) &&
        is_above_version_four(option_datum(state, SXML_TRANSFORM_OPTION_XSLT_VERSION))) {
        return sxml_function_library_report(call, &no_processor_error);
    }
    if (!has_option(state, SXML_TRANSFORM_OPTION_REQUESTED_PROPERTIES)) {
        return SXML_STATUS_OK;
    }
    const sxml_item map = option_item(state, SXML_TRANSFORM_OPTION_REQUESTED_PROPERTIES);
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        property_check check = {.call = call, .entry = {.key = {.representation = 0}}};
        status = sxml_map_get_entry(map, entry_index, &check.entry);
        bool is_met = false;
        if (status == SXML_STATUS_OK) {
            status = check_property(&check, &is_met);
        }
        if (status == SXML_STATUS_OK && !is_met) {
            status = sxml_function_library_report(call, &no_processor_error);
        }
    }
    return status;
}

/* The delivery of the results (F&O 4.0, 18.3, delivery-format):
   document by default, which build-tree="no" makes raw. */
static sxml_status read_delivery(const sxml_function_call *call, transform_state *state) {
    state->result_delivery = SXML_XSLT_RESULT_DELIVERY_DEFAULT;
    if (!has_option(state, SXML_TRANSFORM_OPTION_DELIVERY_FORMAT)) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view format = option_text(state, SXML_TRANSFORM_OPTION_DELIVERY_FORMAT);
    if (sxml_byte_view_equals(format, SXML_BYTE_VIEW_FROM_LITERAL("document"))) {
        state->result_delivery = SXML_XSLT_RESULT_DELIVERY_TREE;
    } else if (sxml_byte_view_equals(format, SXML_BYTE_VIEW_FROM_LITERAL("serialized"))) {
        state->result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED;
    } else if (sxml_byte_view_equals(format, SXML_BYTE_VIEW_FROM_LITERAL("raw"))) {
        state->result_delivery = SXML_XSLT_RESULT_DELIVERY_RAW;
    } else if (sxml_byte_view_equals(format, SXML_BYTE_VIEW_FROM_LITERAL("file"))) {
        return sxml_function_library_report(call, &disabled_error);
    } else {
        return sxml_function_library_report(call, &option_error);
    }
    return SXML_STATUS_OK;
}

/* -------------------------------------------------- The stylesheet (3.4) */

/* Serializes node into the state's stylesheet text: the principal module
   of a stylesheet or package node. */
static sxml_status serialize_stylesheet_node(const sxml_function_call *call,
                                             transform_state *state, sxml_item node) {
    const sxml_allocator allocator = state->processor->allocator;
    sxml_status status = sxml_output_buffer_create(
        allocator,
        &(sxml_output_buffer_configuration){.allocator = allocator, .maximum_byte_count = 0},
        &state->stylesheet_text);
    if (status == SXML_STATUS_OK) {
        sxml_serialization_parameters parameters;
        memset(&parameters, 0, sizeof(parameters));
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
        status = sxml_serialize_sequence(
            call->item_heap,
            &(sxml_serialization_request){.value = sxml_sequence_from_item(node),
                                          .serialization_parameters = parameters,
                                          .error_record = call->error_record},
            sxml_output_buffer_as_output_stream(state->stylesheet_text));
    }
    return status;
}

/* The base URI of node as an owned string in *resolved (empty when
   absent). */
static sxml_status node_base_uri(const sxml_function_call *call, sxml_item node,
                                 resolved_uri *resolved) {
    sxml_node_reference reference;
    sxml_status status = sxml_item_get_node(node, &reference);
    sxml_sequence base_uri = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_node_make_base_uri(
            call->item_heap,
            &(sxml_node_value_request){.node = reference, .error_record = call->error_record},
            &base_uri);
    }
    sxml_item item = {.representation = 0};
    if (status != SXML_STATUS_OK ||
        sxml_sequence_get_item(base_uri,
                               (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &item) != SXML_STATUS_OK) {
        sxml_sequence_release(base_uri);
        return status;
    }
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    status = sxml_atomic_item_get_datum(item, &datum);
    void *memory = NULL;
    const sxml_byte_view text = datum.datum_details.string.utf8;
    if (status == SXML_STATUS_OK && text.byte_count > 0) {
        status = sxml_allocator_allocate(
            call->dynamic_context->allocator,
            (sxml_allocation_request){.byte_count = text.byte_count, .alignment = 1}, &memory);
    }
    if (status == SXML_STATUS_OK && memory != NULL) {
        memcpy(memory, text.bytes, text.byte_count);
        *resolved = (resolved_uri){
            .uri = {.bytes = memory, .byte_count = text.byte_count}, .memory = memory};
    }
    sxml_sequence_release(base_uri);
    return status;
}

/* Reads a stylesheet location as fn:doc does (the caller's trust and
   documents, F&O 4.0, 18.3); FOXT0002 when it cannot be read. */
static sxml_status load_location(const sxml_function_call *call, sxml_byte_view location,
                                 sxml_item *document) {
    const sxml_status status = sxml_dynamic_context_load_document(
        call->dynamic_context,
        &(sxml_document_request){
            .uri_reference = location,
            .base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context),
            .error_record = call->error_record},
        document);
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        return sxml_function_library_report(call, &location_error);
    }
    return status;
}

/* Compiles the stylesheet the options name into state->stylesheet. */
static sxml_status compile_stylesheet(const sxml_function_call *call, transform_state *state) {
    sxml_xslt_stylesheet_configuration configuration;
    memset(&configuration, 0, sizeof(configuration));
    configuration.processor = state->processor;
    configuration.source_name = call_source_name;
    configuration.error_record = call->error_record;
    const parameter_bindings *static_parameters = &state->parameters[SXML_PARAMETER_MAP_STATIC];
    configuration.static_parameters = (sxml_variable_binding_view){
        .variable_bindings = static_parameters->bindings,
        .variable_binding_count = static_parameters->binding_count};
    sxml_status status = SXML_STATUS_OK;
    /* stylesheet-base-uri, else the node's or location's base URI, else
       the executable base URI (F&O 4.0, 18.3). */
    if (has_option(state, SXML_TRANSFORM_OPTION_STYLESHEET_BASE_URI)) {
        status = resolve_against_call(
            call, option_text(state, SXML_TRANSFORM_OPTION_STYLESHEET_BASE_URI),
            &state->stylesheet_base_uri);
    }
    const bool has_base_uri = has_option(state, SXML_TRANSFORM_OPTION_STYLESHEET_BASE_URI);
    sxml_item node = {.representation = 0};
    bool owns_node = false;
    const transform_option node_option = has_option(state, SXML_TRANSFORM_OPTION_STYLESHEET_NODE)
                                             ? SXML_TRANSFORM_OPTION_STYLESHEET_NODE
                                             : SXML_TRANSFORM_OPTION_PACKAGE_NODE;
    const transform_option location_option =
        has_option(state, SXML_TRANSFORM_OPTION_STYLESHEET_LOCATION)
            ? SXML_TRANSFORM_OPTION_STYLESHEET_LOCATION
            : SXML_TRANSFORM_OPTION_PACKAGE_LOCATION;
    const transform_option text_option = has_option(state, SXML_TRANSFORM_OPTION_STYLESHEET_TEXT)
                                             ? SXML_TRANSFORM_OPTION_STYLESHEET_TEXT
                                             : SXML_TRANSFORM_OPTION_PACKAGE_TEXT;
    if (status == SXML_STATUS_OK && has_option(state, SXML_TRANSFORM_OPTION_PACKAGE_NAME)) {
        configuration.principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE;
        configuration.principal_module_uri_reference =
            option_text(state, SXML_TRANSFORM_OPTION_PACKAGE_NAME);
        configuration.package_version = option_text(state, SXML_TRANSFORM_OPTION_PACKAGE_VERSION);
    } else if (status == SXML_STATUS_OK && has_option(state, text_option)) {
        configuration.principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT;
        configuration.principal_module_text = option_text(state, text_option);
        if (!has_base_uri) {
            state->stylesheet_base_uri.uri =
                sxml_dynamic_context_get_static_base_uri(call->dynamic_context);
        }
    } else if (status == SXML_STATUS_OK) {
        if (has_option(state, node_option)) {
            node = option_item(state, node_option);
        } else {
            status = load_location(call, option_text(state, location_option), &node);
            owns_node = status == SXML_STATUS_OK;
        }
        if (status == SXML_STATUS_OK && !has_base_uri) {
            status = node_base_uri(call, node, &state->stylesheet_base_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = serialize_stylesheet_node(call, state, node);
        }
        if (status == SXML_STATUS_OK) {
            configuration.principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT;
            configuration.principal_module_text =
                sxml_output_buffer_get_bytes(state->stylesheet_text);
        }
    }
    if (owns_node) {
        sxml_item_release(node);
    }
    configuration.static_context.static_base_uri = state->stylesheet_base_uri.uri;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_stylesheet_create(state->processor->allocator, &configuration,
                                             &state->stylesheet);
    }
    if (status == SXML_STATUS_STATIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        /* F&O 4.0, 18.3: the XSLT error code is kept, as a dynamic
           error. */
        return SXML_STATUS_DYNAMIC_ERROR;
    }
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8) {
        return sxml_function_library_report(call, &malformed_stylesheet_error);
    }
    return status;
}

/* ------------------------------------------------- Output (26.1, 18.3) */

/* A serialization parameter by its fn:serialize name, and where it lives
   in sxml_serialization_parameters. */
typedef struct parameter_field {
    sxml_byte_view parameter_name;
    size_t offset;
    size_t byte_count;
    /* The has_ flag of a text parameter; SIZE_MAX for none. */
    size_t presence_offset;
} parameter_field;

#define SXML_PARAMETER_FIELD(name_text, field)                                         \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                          \
     .offset = offsetof(sxml_serialization_parameters, field),                         \
     .byte_count = sizeof(((sxml_serialization_parameters *)NULL)->field),             \
     .presence_offset = SIZE_MAX}
#define SXML_PRESENT_PARAMETER_FIELD(name_text, field, presence)                       \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                          \
     .offset = offsetof(sxml_serialization_parameters, field),                         \
     .byte_count = sizeof(((sxml_serialization_parameters *)NULL)->field),             \
     .presence_offset = offsetof(sxml_serialization_parameters, presence)}

static const parameter_field parameter_fields[] = {
    SXML_PARAMETER_FIELD("allow-duplicate-names", allow_duplicate_names),
    SXML_PARAMETER_FIELD("byte-order-mark", byte_order_mark),
    SXML_PARAMETER_FIELD("canonical", canonical),
    SXML_PARAMETER_FIELD("cdata-section-elements", cdata_section_elements),
    SXML_PARAMETER_FIELD("csv-header", csv_header),
    SXML_PARAMETER_FIELD("csv-quote-character", csv_quote_character_codepoint),
    SXML_PARAMETER_FIELD("csv-field-separator", csv_separator_codepoint),
    SXML_PRESENT_PARAMETER_FIELD("doctype-public", doctype_public, has_doctype_public),
    SXML_PRESENT_PARAMETER_FIELD("doctype-system", doctype_system, has_doctype_system),
    SXML_PARAMETER_FIELD("encoding", character_encoding),
    SXML_PARAMETER_FIELD("escape-solidus", escape_solidus),
    SXML_PARAMETER_FIELD("escape-uri-attributes", escape_uri_attributes),
    SXML_PRESENT_PARAMETER_FIELD("html-version", html_version, has_html_version),
    SXML_PARAMETER_FIELD("include-content-type", include_content_type),
    SXML_PARAMETER_FIELD("indent", indent),
    SXML_PARAMETER_FIELD("indent-attributes", indent_attributes),
    SXML_PRESENT_PARAMETER_FIELD("indent-unit", indent_unit, has_indent_unit),
    SXML_PRESENT_PARAMETER_FIELD("item-separator", item_separator, has_item_separator),
    SXML_PARAMETER_FIELD("json-lines", json_lines),
    SXML_PARAMETER_FIELD("json-node-output-method", json_node_output_method),
    SXML_PARAMETER_FIELD("line-ending", line_ending),
    SXML_PRESENT_PARAMETER_FIELD("media-type", media_type, has_media_type),
    SXML_PARAMETER_FIELD("method", serialization_method),
    SXML_PARAMETER_FIELD("normalization-form", normalization_form),
    SXML_PARAMETER_FIELD("omit-xml-declaration", omit_xml_declaration),
    SXML_PARAMETER_FIELD("standalone", standalone),
    SXML_PARAMETER_FIELD("suppress-indentation", suppress_indentation),
    SXML_PARAMETER_FIELD("undeclare-prefixes", undeclare_prefixes),
    SXML_PARAMETER_FIELD("use-character-maps", use_character_maps),
    SXML_PRESENT_PARAMETER_FIELD("version", version, has_version),
};

/* Copies the parameters serialization-params gives over the unnamed
   output definition (F&O 4.0, 18.3, serialization-params: as an
   xsl:output of higher import precedence). */
static sxml_status merge_serialization_parameters(const sxml_function_call *call,
                                                  transform_state *state) {
    const sxml_allocator allocator = state->processor->allocator;
    sxml_status status = sxml_serialization_parameter_reader_create(
        allocator,
        &(sxml_serialization_parameter_reader_configuration){.allocator = allocator,
                                                             .item_heap = call->item_heap},
        &state->parameter_reader);
    sxml_serialization_parameters given;
    memset(&given, 0, sizeof(given));
    given.serialization_method = SXML_SERIALIZATION_METHOD_XML;
    const sxml_sequence options = option_value(state, SXML_TRANSFORM_OPTION_SERIALIZATION_PARAMS);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialization_parameter_reader_read(
            state->parameter_reader,
            &(sxml_serialization_parameter_read_request){.options = options,
                                                         .error_record = call->error_record},
            &given);
    }
    const sxml_item map = option_item(state, SXML_TRANSFORM_OPTION_SERIALIZATION_PARAMS);
    size_t entry_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_map_get_size(map, &entry_count);
    }
    uint8_t *target = (uint8_t *)&state->output_definition.serialization_parameters;
    const uint8_t *source = (const uint8_t *)&given;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry = {.key = {.representation = 0}};
        status = sxml_map_get_entry(map, entry_index, &entry);
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof(datum));
        if (status != SXML_STATUS_OK ||
            sxml_atomic_item_get_datum(entry.key, &datum) != SXML_STATUS_OK ||
            datum.datum_kind != SXML_DATUM_KIND_STRING) {
            continue;
        }
        for (size_t field_index = 0;
             field_index < sizeof parameter_fields / sizeof parameter_fields[0];
             field_index += 1) {
            const parameter_field *field = &parameter_fields[field_index];
            if (!sxml_byte_view_equals(field->parameter_name, datum.datum_details.string.utf8)) {
                continue;
            }
            memcpy(target + field->offset, source + field->offset, field->byte_count);
            if (field->presence_offset != SIZE_MAX) {
                memcpy(target + field->presence_offset, source + field->presence_offset,
                       sizeof(bool));
            }
            if (field->offset == offsetof(sxml_serialization_parameters, serialization_method)) {
                state->output_definition.has_serialization_method = true;
            }
        }
    }
    return status;
}

/* The output definition and the delivery of the principal result. */
static sxml_status prepare_output(const sxml_function_call *call, transform_state *state) {
    sxml_status status = sxml_xslt_stylesheet_get_output_definition(
        state->stylesheet, (sxml_name_identifier){.index = 0}, &state->output_definition);
    if (status == SXML_STATUS_OK && has_option(state, SXML_TRANSFORM_OPTION_SERIALIZATION_PARAMS)) {
        status = merge_serialization_parameters(call, state);
    }
    sxml_serialization_parameters *parameters = &state->output_definition.serialization_parameters;
    if (state->result_delivery == SXML_XSLT_RESULT_DELIVERY_SERIALIZED) {
        /* 18.3: the encoding stage is skipped, as for fn:serialize. */
        parameters->character_encoding = SXML_CHARACTER_ENCODING_UTF_8;
        parameters->byte_order_mark = SXML_SERIALIZATION_FLAG_NO;
    }
    const sxml_allocator allocator = state->processor->allocator;
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            allocator,
            &(sxml_output_buffer_configuration){.allocator = allocator, .maximum_byte_count = 0},
            &state->principal_output);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_result_document_collector_create(
            allocator,
            &(sxml_xslt_result_document_collector_configuration){
                .allocator = allocator,
                .maximum_byte_count = 0,
                .result_delivery = state->result_delivery},
            &state->collector);
    }
    if (status == SXML_STATUS_OK && has_option(state, SXML_TRANSFORM_OPTION_BASE_OUTPUT_URI)) {
        status = resolve_against_call(call,
                                      option_text(state, SXML_TRANSFORM_OPTION_BASE_OUTPUT_URI),
                                      &state->base_output_uri);
    }
    return status;
}

/* ------------------------------------------------- Invocation (2.4, 18.3) */

/* The source document of source-location, owned. */
static sxml_status load_source(const sxml_function_call *call, transform_state *state) {
    if (!has_option(state, SXML_TRANSFORM_OPTION_SOURCE_LOCATION)) {
        return SXML_STATUS_OK;
    }
    sxml_item document = {.representation = 0};
    const sxml_status status = load_location(
        call, option_text(state, SXML_TRANSFORM_OPTION_SOURCE_LOCATION), &document);
    if (status == SXML_STATUS_OK) {
        state->source_document = sxml_sequence_from_item(document);
    }
    return status;
}

/* Fills state->request: the invocation the options choose (18.3: the
   first of initial-function, initial-template, a source, else
   xsl:initial-template), its values and the global context item. */
static sxml_status choose_invocation(const sxml_function_call *call, transform_state *state) {
    sxml_xslt_transformation_request *request = &state->request;
    memset(request, 0, sizeof(*request));
    const bool has_source_node = has_option(state, SXML_TRANSFORM_OPTION_SOURCE_NODE);
    const sxml_item source_node = option_item(state, SXML_TRANSFORM_OPTION_SOURCE_NODE);
    sxml_status status = SXML_STATUS_OK;
    /* The global context item: global-context-item, else the root of
       source-node, else the document of source-location. */
    if (has_option(state, SXML_TRANSFORM_OPTION_GLOBAL_CONTEXT_ITEM)) {
        state->global_context_item = option_value(state, SXML_TRANSFORM_OPTION_GLOBAL_CONTEXT_ITEM);
        sxml_sequence_retain(state->global_context_item);
    } else if (has_source_node) {
        status = root_of(source_node, &state->global_context_item);
    } else if (has_option(state, SXML_TRANSFORM_OPTION_SOURCE_LOCATION)) {
        state->global_context_item = state->source_document;
        sxml_sequence_retain(state->global_context_item);
    }
    uint64_t context_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(state->global_context_item, NULL, &context_count);
    }
    request->dynamic_context.has_context_value = context_count == 1;
    request->dynamic_context.context_value = state->global_context_item;
    const parameter_bindings *stylesheet_parameters =
        &state->parameters[SXML_PARAMETER_MAP_STYLESHEET];
    request->dynamic_context.variable_bindings = (sxml_variable_binding_view){
        .variable_bindings = stylesheet_parameters->bindings,
        .variable_binding_count = stylesheet_parameters->binding_count};
    if (has_option(state, SXML_TRANSFORM_OPTION_INITIAL_FUNCTION)) {
        request->invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION;
        request->initial_function_name = option_name(state, SXML_TRANSFORM_OPTION_INITIAL_FUNCTION);
        request->function_arguments = (sxml_sequence_view){
            .sequences = state->function_arguments,
            .sequence_count = state->function_argument_count};
        return status;
    }
    const parameter_bindings *template_parameters = &state->parameters[SXML_PARAMETER_MAP_TEMPLATE];
    const parameter_bindings *tunnel_parameters = &state->parameters[SXML_PARAMETER_MAP_TUNNEL];
    request->template_parameters = (sxml_variable_binding_view){
        .variable_bindings = template_parameters->bindings,
        .variable_binding_count = template_parameters->binding_count};
    request->tunnel_parameters = (sxml_variable_binding_view){
        .variable_bindings = tunnel_parameters->bindings,
        .variable_binding_count = tunnel_parameters->binding_count};
    const bool has_selection = has_option(state, SXML_TRANSFORM_OPTION_INITIAL_MATCH_SELECTION);
    if (has_option(state, SXML_TRANSFORM_OPTION_INITIAL_TEMPLATE)) {
        request->invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE;
        request->initial_template_name = option_name(state, SXML_TRANSFORM_OPTION_INITIAL_TEMPLATE);
    } else if (has_source_node || has_selection ||
               has_option(state, SXML_TRANSFORM_OPTION_SOURCE_LOCATION)) {
        request->invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES;
        request->initial_mode_name = option_name(state, SXML_TRANSFORM_OPTION_INITIAL_MODE);
        if (has_selection) {
            request->has_initial_match_selection = true;
            request->initial_match_selection =
                option_value(state, SXML_TRANSFORM_OPTION_INITIAL_MATCH_SELECTION);
        } else if (has_source_node && (!request->dynamic_context.has_context_value ||
                                       !is_same_node(source_node, state->global_context_item))) {
            /* The source node is the initial match selection; when it is
               the global context item, the default selection is that
               item, stripped (2.4.3, 4.2.2). */
            request->has_initial_match_selection = true;
            request->initial_match_selection = option_value(state, SXML_TRANSFORM_OPTION_SOURCE_NODE);
        } else if (!request->dynamic_context.has_context_value) {
            request->has_initial_match_selection = true;
            request->initial_match_selection = state->source_document;
        }
    } else {
        request->invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE;
    }
    (void)call;
    return status;
}

/* Primes the transformation of the compiled stylesheet. */
static sxml_status prime_transformation(const sxml_function_call *call, transform_state *state) {
    sxml_xslt_transformation_request *request = &state->request;
    request->base_output_uri = state->base_output_uri.uri;
    request->output_definition = &state->output_definition;
    request->output_stream = sxml_output_buffer_as_output_stream(state->principal_output);
    request->result_document_handler =
        sxml_xslt_result_document_collector_as_handler(state->collector);
    request->error_record = call->error_record;
    request->result_delivery = state->result_delivery;
    const sxml_status status = sxml_xslt_machine_transformation_create(
        &(sxml_xslt_machine_transformation_request){
            .stylesheet = state->stylesheet,
            .transformation = request,
            .discards_messages = sxml_function_library_option_is_false((sxml_option_lookup){
                .values = &state->options, .option_index = SXML_TRANSFORM_OPTION_ENABLE_MESSAGES})},
        &state->running);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        return sxml_function_library_report(call, &option_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* 18.3, trusted: false by default, and a stylesheet an untrusted
       caller loads is untrusted too. */
    if (!sxml_function_library_option_is_true(
            (sxml_option_lookup){.values = &state->options,
                                 .option_index = SXML_TRANSFORM_OPTION_TRUSTED}) ||
        sxml_dynamic_context_check_trust(call->dynamic_context, NULL) != SXML_STATUS_OK) {
        sxml_ir_unit_set_untrusted(sxml_xslt_machine_transformation_get_evaluation(state->running)->unit);
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ The steps */

static sxml_status start_transform(const sxml_native_step_request *step_request,
                                   sxml_native_step *step) {
    const sxml_function_call *call = step_request->call;
    transform_state *state = step_request->state;
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    if (environment == NULL || environment->processor == NULL) {
        return sxml_function_library_report(call, &disabled_error);
    }
    *state = (transform_state){.processor = environment->processor,
                               .dynamic_context = call->dynamic_context,
                               .error_record = call->error_record,
                               .source_document = sxml_sequence_empty(),
                               .global_context_item = sxml_sequence_empty(),
                               .call_arguments = {sxml_sequence_empty(), sxml_sequence_empty()}};
    sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 0,
                               .definitions = {.definitions = option_definitions,
                                               .definition_count = SXML_TRANSFORM_OPTION_COUNT}},
        &state->options);
    if (status == SXML_STATUS_OK) {
        status = check_options(call, state);
    }
    if (status == SXML_STATUS_OK) {
        status = check_processor(call, state);
    }
    if (status == SXML_STATUS_OK) {
        status = read_delivery(call, state);
    }
    static const struct {
        transform_option option;
        parameter_map_kind map_kind;
    } parameter_maps[] = {
        {SXML_TRANSFORM_OPTION_STATIC_PARAMS, SXML_PARAMETER_MAP_STATIC},
        {SXML_TRANSFORM_OPTION_STYLESHEET_PARAMS, SXML_PARAMETER_MAP_STYLESHEET},
        {SXML_TRANSFORM_OPTION_TEMPLATE_PARAMS, SXML_PARAMETER_MAP_TEMPLATE},
        {SXML_TRANSFORM_OPTION_TUNNEL_PARAMS, SXML_PARAMETER_MAP_TUNNEL},
    };
    for (size_t map_index = 0; status == SXML_STATUS_OK && map_index < SXML_PARAMETER_MAP_COUNT;
         map_index += 1) {
        status = read_parameter_map(
            state, &(parameter_map_request){.call = call,
                                            .option = parameter_maps[map_index].option,
                                            .map_kind = parameter_maps[map_index].map_kind});
    }
    if (status == SXML_STATUS_OK) {
        status = read_function_arguments(state);
    }
    if (status == SXML_STATUS_OK) {
        status = compile_stylesheet(call, state);
    }
    if (status == SXML_STATUS_OK) {
        status = prepare_output(call, state);
    }
    if (status == SXML_STATUS_OK) {
        status = load_source(call, state);
    }
    if (status == SXML_STATUS_OK) {
        status = choose_invocation(call, state);
    }
    if (status == SXML_STATUS_OK) {
        status = prime_transformation(call, state);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *step = (sxml_native_step){
        .step_kind = SXML_NATIVE_STEP_KIND_EVALUATE_UNIT,
        .result = sxml_sequence_empty(),
        .function_item = {.representation = 0},
        .arguments = {.sequences = NULL, .sequence_count = 0},
        .unit_evaluation = sxml_xslt_machine_transformation_get_evaluation(state->running)};
    return SXML_STATUS_OK;
}

/* Makes an xs:string of serialized bytes (18.3: the encoding stage is
   skipped); FOXT0006 when they are no UTF-8 text. */
static sxml_status string_of_bytes(const sxml_function_call *call, sxml_byte_view bytes,
                                   sxml_sequence *string) {
    sxml_string checked = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    if (sxml_string_from_utf8(bytes, &checked) != SXML_STATUS_OK) {
        return sxml_function_library_report(call, &character_error);
    }
    return sxml_function_library_make_string(call->item_heap, bytes, string);
}

/* Adds an entry to the result map, taking value over. */
typedef struct entry_request {
    const sxml_function_call *call;
    sxml_byte_view key;
} entry_request;

static sxml_status add_entry(transform_state *state, const entry_request *request,
                             sxml_sequence value) {
    sxml_sequence key = sxml_sequence_empty();
    sxml_status status =
        sxml_function_library_make_string(request->call->item_heap, request->key, &key);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(value);
        return status;
    }
    state->entries[state->entry_count] = (result_entry){.key = key.content, .value = value};
    state->entry_count += 1;
    return SXML_STATUS_OK;
}

/* The second step: delivers the principal result, collects the result
   documents (18.3: one entry each, keyed by the base output URI or
   "output", and by absolute URI). */
static sxml_status collect_results(const sxml_native_step_request *step_request) {
    const sxml_function_call *call = step_request->call;
    transform_state *state = step_request->state;
    sxml_sequence principal = sxml_sequence_empty();
    sxml_status status = sxml_xslt_machine_transformation_deliver(
        state->running, step_request->call_result, &principal);
    sxml_sequence_release(step_request->call_result);
    sxml_dynamic_context_drop_unit_extension(
        state->dynamic_context, sxml_xslt_machine_transformation_get_extension(state->running));
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t document_count =
        sxml_xslt_result_document_collector_get_result_document_count(state->collector);
    void *memory = NULL;
    status = sxml_allocator_allocate_array(
        state->processor->allocator,
        (sxml_array_allocation_request){.element_count = document_count + 1,
                                        .element_size = sizeof(result_entry),
                                        .element_alignment = alignof(result_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(principal);
        return status;
    }
    state->entries = memory;
    if (state->result_delivery == SXML_XSLT_RESULT_DELIVERY_SERIALIZED) {
        sxml_sequence_release(principal);
        principal = sxml_sequence_empty();
        status = string_of_bytes(call, sxml_output_buffer_get_bytes(state->principal_output),
                                 &principal);
    }
    if (status == SXML_STATUS_OK) {
        status = add_entry(state,
                           &(entry_request){.call = call,
                                            .key = has_option(state,
                                                              SXML_TRANSFORM_OPTION_BASE_OUTPUT_URI)
                                                       ? state->base_output_uri.uri
                                                       : SXML_BYTE_VIEW_FROM_LITERAL("output")},
                           principal);
    }
    for (size_t document_index = 0; status == SXML_STATUS_OK && document_index < document_count;
         document_index += 1) {
        sxml_xslt_collected_result_document document;
        memset(&document, 0, sizeof(document));
        status = sxml_xslt_result_document_collector_get_result_document(state->collector,
                                                                         document_index, &document);
        sxml_sequence value = sxml_sequence_empty();
        if (status == SXML_STATUS_OK &&
            state->result_delivery == SXML_XSLT_RESULT_DELIVERY_SERIALIZED) {
            status = string_of_bytes(call, document.serialized_bytes, &value);
        } else if (status == SXML_STATUS_OK) {
            value = document.value;
            sxml_sequence_retain(value);
        }
        if (status == SXML_STATUS_OK) {
            status = add_entry(state, &(entry_request){.call = call, .key = document.absolute_uri},
                               value);
        }
    }
    return status;
}

/* Asks for the post-process call on the next entry (18.3, post-process),
   or finishes with the result map. */
static sxml_status next_result(const sxml_native_step_request *step_request,
                               sxml_native_step *step) {
    transform_state *state = step_request->state;
    const sxml_function_call *call = step_request->call;
    if (has_option(state, SXML_TRANSFORM_OPTION_POST_PROCESS) &&
        state->post_processed_count < state->entry_count) {
        const result_entry *entry = &state->entries[state->post_processed_count];
        for (size_t argument_index = 0; argument_index < 2; argument_index += 1) {
            sxml_sequence_release(state->call_arguments[argument_index]);
        }
        sxml_item_retain(entry->key);
        state->call_arguments[0] = sxml_sequence_from_item(entry->key);
        sxml_sequence_retain(entry->value);
        state->call_arguments[1] = entry->value;
        *step = (sxml_native_step){
            .step_kind = SXML_NATIVE_STEP_KIND_CALL,
            .result = sxml_sequence_empty(),
            .function_item = option_item(state, SXML_TRANSFORM_OPTION_POST_PROCESS),
            .arguments = {.sequences = state->call_arguments, .sequence_count = 2},
            .unit_evaluation = NULL};
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        state->processor->allocator,
        (sxml_array_allocation_request){.element_count = state->entry_count,
                                        .element_size = sizeof(sxml_map_entry),
                                        .element_alignment = alignof(sxml_map_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_entry *map_entries = memory;
    for (size_t entry_index = 0; entry_index < state->entry_count; entry_index += 1) {
        map_entries[entry_index] = (sxml_map_entry){.key = state->entries[entry_index].key,
                                                    .value = state->entries[entry_index].value};
    }
    sxml_item map = {.representation = 0};
    status = sxml_map_make(
        call->item_heap,
        &(sxml_map_request){
            .map_entries = {.map_entries = map_entries, .map_entry_count = state->entry_count},
            .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_LAST,
            .error_record = NULL},
        &map);
    sxml_allocator_release(state->processor->allocator, memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                               .result = sxml_sequence_from_item(map),
                               .function_item = {.representation = 0},
                               .arguments = {.sequences = NULL, .sequence_count = 0},
                               .unit_evaluation = NULL};
    return SXML_STATUS_OK;
}

static sxml_status step_transform(const sxml_native_step_request *request,
                                  sxml_native_step *step) {
    transform_state *state = request->state;
    if (request->step_number == 0) {
        return start_transform(request, step);
    }
    if (request->step_number == 1) {
        const sxml_status status = collect_results(request);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    } else {
        /* The value post-process gave for the entry. */
        result_entry *entry = &state->entries[state->post_processed_count];
        sxml_sequence_release(entry->value);
        entry->value = request->call_result;
        state->post_processed_count += 1;
    }
    return next_result(request, step);
}

static const sxml_resumable_native_body transform_body = {
    .step = step_transform,
    .release_state = release_transform_state,
    .state_byte_count = sizeof(transform_state),
    .state_alignment = alignof(transform_state)};

static const sxml_function_parameter transform_parameters[] = {
    SXML_REQUIRED_PARAMETER("options", "map(*)")};

static const sxml_native_function transform_rows[] = {
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                   SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC |
                   SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("transform"),
     .parameters = {transform_parameters, 1},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("map(*)"),
     .call = NULL,
     .resumable_body = &transform_body},
};

/* Not named sxml_function_library_<part>: not a registered part (D191). */
const sxml_native_function_view sxml_xslt_transform_functions = {
    .native_functions = transform_rows,
    .native_function_count = sizeof transform_rows / sizeof transform_rows[0]};

/* --------------------------------------------------------------- Feature */

const sxml_optional_feature_definition sxml_xslt_transform_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_XSLT_TRANSFORM,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("xslt-transform"),
    .operations = &sxml_xslt_transform_functions,
    .finalize_state = NULL,
    .library_functions = &sxml_xslt_transform_functions,
};

sxml_optional_feature sxml_xslt_transform_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_xslt_transform_feature_definition};
}
