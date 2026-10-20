/*
 * fn:load-xquery-module (F&O 4.0, 18.2; docs/DECISIONS.md, D154): the
 * function's resumable body, whose row is the library part xquery_module
 * (src/function_library_xquery_module.c, D272), and the XQuery-only
 * function table of src/xquery_load_module_internal.h.
 *
 * The body never nests the evaluator in C (D71, D75). Its first step
 * compiles, which evaluates nothing, then asks the machine for a unit
 * evaluation (glossary `unit_evaluation`): the machine gives the compiled
 * unit a global table of its own, computes its globals on its own task
 * stack, runs its main function, and runs the second step with the value,
 * which is the module record (glossary `module_record`, 18.1).
 *
 * The module record is the value of a main module that the first step
 * writes as text, as the function's rules describe it (18.1: "a function
 * item obtained as if by evaluating a named function reference Q#N"):
 *
 *     xquery version "4.0";
 *     import module namespace sxml-loaded = "NS" at "HINT", ...;
 *     map { "variables": map { QName("NS", "v"): $sxml-loaded:v, ... },
 *           "functions": map { QName("NS", "f"): map { 1: sxml-loaded:f#1,
 *                                                     2: sxml-loaded:f#2 },
 *                              ... } }
 *
 * so the front end, the module loader and the lowering do all the work.
 * To write it, the step first loads the module set of the import alone
 * (src/xquery_module_internal.h) and reads the public declarations of the
 * modules of the namespace from their ASTs: functions that are not
 * external, with the arity range their defaults give (5.18.3), and
 * variables. The content option is served to the module loader by a
 * resolver in front of the evaluation's, as the module of the namespace
 * asked for without a hint.
 *
 * Errors: FOQM0001 for an empty namespace; FOQM0002 when no module of the
 * namespace is found (the import's XQST0059); FOQM0003 for any other
 * static error of the module; FOQM0005 for a variable value or context
 * value that does not match its declared type without conversion, or for
 * both context options; FOQM0006 for a request of an XQuery version above
 * 4.0 and for a call without the processor's evaluation environment.
 * Dynamic errors of the module's globals are raised as they are.
 *
 * Golden file: src/function_library_higher_order.c (a resumable body).
 */
#include "xquery_load_module_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "processor_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "xpath_ast_internal.h"
#include "xpath_api_internal.h"
#include "xquery_lower_internal.h"
#include "xquery_module_internal.h"
#include "xquery_update_internal.h"

#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

static const sxml_static_error no_namespace_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOQM0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:load-xquery-module needs a module URI (F&O 4.0, 18.2)")};

static const sxml_static_error module_not_found_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOQM0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "no library module with this target namespace is found (F&O 4.0, 18.2)")};

static const sxml_static_error static_error_in_module_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOQM0003"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the loaded library module has a static error (F&O 4.0, 18.2)")};

static const sxml_static_error wrong_value_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOQM0005"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "a supplied variable or context value does not match the type the module "
        "declares, or both context-value and context-item are given (F&O 4.0, 18.2)")};

static const sxml_static_error no_processor_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOQM0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "no XQuery processor for this request is available (F&O 4.0, 18.2)")};

static const sxml_static_error variable_key_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "a key of the variables or vendor-options option is not an xs:QName (F&O 4.0, 18.2)")};

/* ---------------------------------------------------------------- Options */

/* The indices of option_definitions. */
typedef enum load_option {
    SXML_LOAD_OPTION_XQUERY_VERSION,
    SXML_LOAD_OPTION_TRUSTED,
    SXML_LOAD_OPTION_LOCATION_HINTS,
    SXML_LOAD_OPTION_CONTENT,
    SXML_LOAD_OPTION_CONTEXT_VALUE,
    SXML_LOAD_OPTION_CONTEXT_ITEM,
    SXML_LOAD_OPTION_VARIABLES,
    SXML_LOAD_OPTION_VENDOR_OPTIONS,
    SXML_LOAD_OPTION_COUNT,
} load_option;

static const sxml_option_definition option_definitions[] = {
    SXML_FUNCTION_OPTION("xquery-version", SXML_TYPE_XS_DECIMAL, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("trusted", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("location-hints", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_ZERO_OR_MORE),
    SXML_FUNCTION_OPTION("content", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("context-value", SXML_TYPE_ITEM, SXML_OCCURRENCE_ZERO_OR_MORE),
    SXML_FUNCTION_OPTION("context-item", SXML_TYPE_ITEM, SXML_OCCURRENCE_ZERO_OR_ONE),
    SXML_FUNCTION_OPTION("variables", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("vendor-options", SXML_TYPE_MAP, SXML_OCCURRENCE_EXACTLY_ONE),
};

/* --------------------------------------------------------------- State */

/* What the call holds between its steps. */
typedef struct load_state {
    sxml_allocator allocator;
    /* Owned: the compiled module record unit. */
    sxml_ir_unit *unit;
    /* Owned values, one slot per global of the unit. */
    sxml_evaluator_slot *global_values;
    size_t global_value_count;
    /* Owned. */
    sxml_sequence context_value;
    sxml_evaluation_request evaluation;
} load_state;

static void release_load_state(void *state) {
    load_state *load = state;
    for (size_t slot_index = 0; slot_index < load->global_value_count; slot_index += 1) {
        sxml_sequence_release(load->global_values[slot_index].value);
    }
    if (load->global_values != NULL) {
        sxml_allocator_release(load->allocator, load->global_values);
    }
    load->global_values = NULL;
    load->global_value_count = 0;
    sxml_sequence_release(load->context_value);
    load->context_value = sxml_sequence_empty();
    sxml_ir_unit_release(load->unit);
    load->unit = NULL;
}

/* ------------------------------------------------------------- The text */

/* Appends text to the query being written. */
static sxml_status append(sxml_library_text *text, sxml_byte_view bytes) {
    return sxml_function_library_text_append(text, bytes);
}

/* Appends value as an XQuery string literal: in quotation marks, with
   '"' doubled and '&' as "&amp;" (XQuery 4.0, A.2.1, StringLiteral). */
static sxml_status append_string_literal(sxml_library_text *text, sxml_byte_view value) {
    sxml_status status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("\""));
    size_t run_start = 0;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index <= value.byte_count;
         byte_index += 1) {
        const bool is_end = byte_index == value.byte_count;
        const uint8_t byte = is_end ? 0 : value.bytes[byte_index];
        if (!is_end && byte != '"' && byte != '&') {
            continue;
        }
        status = sxml_function_library_text_append(
            text, (sxml_byte_view){.bytes = value.bytes + run_start,
                                   .byte_count = byte_index - run_start});
        if (status == SXML_STATUS_OK && !is_end) {
            status = append(text, byte == '"' ? SXML_BYTE_VIEW_FROM_LITERAL("\"\"") : SXML_BYTE_VIEW_FROM_LITERAL("&amp;"));
        }
        run_start = byte_index + 1;
    }
    if (status == SXML_STATUS_OK) {
        status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("\""));
    }
    return status;
}

/* Appends a decimal number. */
static sxml_status append_number(sxml_library_text *text, size_t number) {
    char digits[24];
    size_t digit_count = 0;
    do {
        digits[sizeof digits - 1 - digit_count] = (char)('0' + (number % 10));
        number /= 10;
        digit_count += 1;
    } while (number != 0);
    return sxml_function_library_text_append(
        text, (sxml_byte_view){.bytes = (const uint8_t *)&digits[sizeof digits - digit_count],
                               .byte_count = digit_count});
}

/* What the prolog of the module record query needs. */
typedef struct prolog_request {
    sxml_byte_view namespace_uri;
    /* xs:string*; ignored with content. */
    sxml_sequence location_hints;
    bool has_content;
} prolog_request;

/* Appends the version declaration and the import. */
static sxml_status append_prolog(sxml_library_text *text, const prolog_request *request) {
    sxml_status status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("xquery version \"4.0\";\nimport module namespace "
                                      "sxml-loaded = "));
    if (status == SXML_STATUS_OK) {
        status = append_string_literal(text, request->namespace_uri);
    }
    uint64_t hint_count = 0;
    if (status == SXML_STATUS_OK && !request->has_content) {
        status = sxml_sequence_count(request->location_hints, NULL, &hint_count);
    }
    for (uint64_t hint_index = 0; status == SXML_STATUS_OK && hint_index < hint_count;
         hint_index += 1) {
        sxml_item hint = {.representation = 0};
        status = sxml_sequence_get_item(request->location_hints, (sxml_item_request){.item_index = hint_index, .error_record = NULL}, &hint);
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof datum);
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(hint, &datum);
        }
        if (status == SXML_STATUS_OK) {
            status = append(text, hint_index == 0 ? SXML_BYTE_VIEW_FROM_LITERAL(" at ") : SXML_BYTE_VIEW_FROM_LITERAL(", "));
        }
        if (status == SXML_STATUS_OK) {
            status = append_string_literal(text, datum.datum_details.string.utf8);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = append(text, SXML_BYTE_VIEW_FROM_LITERAL(";\n"));
    }
    return status;
}

/* ------------------------------------------------- Public declarations */

/* One public function name of the loaded modules and its arity range. */
typedef struct public_function {
    sxml_byte_view local_name;
    size_t minimum_arity;
    size_t maximum_arity;
} public_function;

/* The public declarations found; arrays allocated with allocator. */
typedef struct public_declarations {
    sxml_allocator allocator;
    sxml_byte_view *variable_names;
    size_t variable_count;
    size_t variable_capacity;
    public_function *functions;
    size_t function_count;
    size_t function_capacity;
} public_declarations;

static void release_declarations(public_declarations *declarations) {
    sxml_allocator_release(declarations->allocator, declarations->variable_names);
    sxml_allocator_release(declarations->allocator, declarations->functions);
}

/* Makes room for one more element in an array of element_size bytes. */
typedef struct growable_array {
    void **elements;
    size_t *capacity;
    size_t count;
    size_t element_size;
    size_t element_alignment;
} growable_array;

static sxml_status grow(sxml_allocator allocator, growable_array array) {
    if (array.count < *array.capacity) {
        return SXML_STATUS_OK;
    }
    const size_t capacity = *array.capacity == 0 ? 8 : *array.capacity * 2;
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(capacity, array.element_size, &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = array.element_alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (array.count > 0) {
        memcpy(memory, *array.elements, array.count * array.element_size);
    }
    sxml_allocator_release(allocator, *array.elements);
    *array.elements = memory;
    *array.capacity = capacity;
    return SXML_STATUS_OK;
}

static sxml_ast_node_index child_of(const sxml_ast *ast, sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

/* The local name of a Name node as written. */
static sxml_byte_view local_name_of(const sxml_ast *ast, sxml_ast_node_index name) {
    return sxml_ast_get_text(ast, sxml_ast_get_node(ast, name)->payload.name.local_name);
}

/* True when a declaration has the annotation %private (5.15), written
   unprefixed, with the prefix of the XQuery namespace's usual binding or
   as a URIQualifiedName. */
static bool is_private(const sxml_ast *ast, sxml_ast_node_index declaration) {
    for (sxml_ast_node_index annotation = child_of(ast, declaration, SXML_AST_FIELD_ANNOTATION);
         annotation.value != 0; annotation = sxml_ast_find_next_with_same_field(ast, annotation)) {
        const sxml_ast_node_index name = child_of(ast, annotation, SXML_AST_FIELD_NAME);
        const sxml_ast_name written = sxml_ast_get_node(ast, name)->payload.name;
        const sxml_byte_view uri = sxml_ast_get_text(ast, written.namespace_uri);
        if (sxml_byte_view_equals(local_name_of(ast, name), SXML_BYTE_VIEW_FROM_LITERAL("private")) &&
            (uri.byte_count == 0 ||
             sxml_byte_view_equals(uri, SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/2012/xquery")))) {
            return true;
        }
    }
    return false;
}

/* Adds a public function declaration to the list, joining the arity
   ranges of declarations of one name (5.18). */
static sxml_status add_function(public_declarations *declarations, const sxml_ast *ast,
                                sxml_ast_node_index declaration) {
    const sxml_byte_view local_name =
        local_name_of(ast, child_of(ast, declaration, SXML_AST_FIELD_NAME));
    size_t parameter_count = 0;
    size_t required_count = 0;
    for (sxml_ast_node_index parameter = child_of(ast, declaration, SXML_AST_FIELD_PARAMETER);
         parameter.value != 0; parameter = sxml_ast_find_next_with_same_field(ast, parameter)) {
        parameter_count += 1;
        if (child_of(ast, parameter, SXML_AST_FIELD_DEFAULT).value == 0) {
            required_count = parameter_count;
        }
    }
    for (size_t function_index = 0; function_index < declarations->function_count;
         function_index += 1) {
        public_function *known = &declarations->functions[function_index];
        if (sxml_byte_view_equals(known->local_name, local_name)) {
            known->minimum_arity =
                required_count < known->minimum_arity ? required_count : known->minimum_arity;
            known->maximum_arity =
                parameter_count > known->maximum_arity ? parameter_count : known->maximum_arity;
            return SXML_STATUS_OK;
        }
    }
    const sxml_status status =
        grow(declarations->allocator,
             (growable_array){.elements = (void **)&declarations->functions,
                              .capacity = &declarations->function_capacity,
                              .count = declarations->function_count,
                              .element_size = sizeof(public_function),
                              .element_alignment = alignof(public_function)});
    if (status == SXML_STATUS_OK) {
        declarations->functions[declarations->function_count] =
            (public_function){.local_name = local_name,
                              .minimum_arity = required_count,
                              .maximum_arity = parameter_count};
        declarations->function_count += 1;
    }
    return status;
}

static sxml_status add_variable(public_declarations *declarations, const sxml_ast *ast,
                                sxml_ast_node_index declaration) {
    const sxml_ast_node_index variable = child_of(ast, declaration, SXML_AST_FIELD_VARIABLE);
    const sxml_status status =
        grow(declarations->allocator,
             (growable_array){.elements = (void **)&declarations->variable_names,
                              .capacity = &declarations->variable_capacity,
                              .count = declarations->variable_count,
                              .element_size = sizeof(sxml_byte_view),
                              .element_alignment = alignof(sxml_byte_view)});
    if (status == SXML_STATUS_OK) {
        declarations->variable_names[declarations->variable_count] =
            local_name_of(ast, child_of(ast, variable, SXML_AST_FIELD_NAME));
        declarations->variable_count += 1;
    }
    return status;
}

/* Reads the public declarations of a library module's prolog (5.15:
   without %private; functions without "external"). */
static sxml_status read_declarations(public_declarations *declarations, const sxml_ast *ast) {
    const sxml_ast_node_index module = child_of(ast, ast->root, SXML_AST_FIELD_MODULE);
    const sxml_ast_node_index prolog = child_of(ast, module, SXML_AST_FIELD_PROLOG);
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index declaration = child_of(ast, prolog, SXML_AST_FIELD_DECLARATION);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = sxml_ast_find_next_with_same_field(ast, declaration)) {
        const sxml_ast_node *node = sxml_ast_get_node(ast, declaration);
        if (is_private(ast, declaration)) {
            continue;
        }
        if (node->kind == SXML_AST_KIND_FUNCTION_DECLARATION &&
            (node->flags & (uint32_t)SXML_AST_FLAG_EXTERNAL) == 0) {
            status = add_function(declarations, ast, declaration);
        } else if (node->kind == SXML_AST_KIND_VARIABLE_DECLARATION) {
            status = add_variable(declarations, ast, declaration);
        }
    }
    return status;
}

/* Appends the body that makes the module record (18.1). */
static sxml_status append_record(sxml_library_text *text, const public_declarations *declarations,
                                 sxml_byte_view namespace_uri) {
    sxml_status status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("map {\"variables\": map {"));
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && variable_index < declarations->variable_count;
         variable_index += 1) {
        const sxml_byte_view name = declarations->variable_names[variable_index];
        status = append(text, variable_index == 0 ? SXML_BYTE_VIEW_FROM_LITERAL("\nQName(") : SXML_BYTE_VIEW_FROM_LITERAL(",\nQName("));
        if (status == SXML_STATUS_OK) {
            status = append_string_literal(text, namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = append(text, SXML_BYTE_VIEW_FROM_LITERAL(", \""));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, name);
        }
        if (status == SXML_STATUS_OK) {
            status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("\"): $sxml-loaded:"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, name);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("},\n\"functions\": map {"));
    }
    for (size_t function_index = 0;
         status == SXML_STATUS_OK && function_index < declarations->function_count;
         function_index += 1) {
        const public_function *function = &declarations->functions[function_index];
        status = append(text, function_index == 0 ? SXML_BYTE_VIEW_FROM_LITERAL("\nQName(") : SXML_BYTE_VIEW_FROM_LITERAL(",\nQName("));
        if (status == SXML_STATUS_OK) {
            status = append_string_literal(text, namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = append(text, SXML_BYTE_VIEW_FROM_LITERAL(", \""));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, function->local_name);
        }
        if (status == SXML_STATUS_OK) {
            status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("\"): map {"));
        }
        for (size_t arity = function->minimum_arity;
             status == SXML_STATUS_OK && arity <= function->maximum_arity; arity += 1) {
            status = append(text, arity == function->minimum_arity ? SXML_BYTE_VIEW_FROM_LITERAL("") : SXML_BYTE_VIEW_FROM_LITERAL(", "));
            if (status == SXML_STATUS_OK) {
                status = append_number(text, arity);
            }
            if (status == SXML_STATUS_OK) {
                status = append(text, SXML_BYTE_VIEW_FROM_LITERAL(": sxml-loaded:"));
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_function_library_text_append(text, function->local_name);
            }
            if (status == SXML_STATUS_OK) {
                status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("#"));
            }
            if (status == SXML_STATUS_OK) {
                status = append_number(text, arity);
            }
        }
        if (status == SXML_STATUS_OK) {
            status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("}"));
        }
    }
    if (status == SXML_STATUS_OK) {
        status = append(text, SXML_BYTE_VIEW_FROM_LITERAL("}}\n"));
    }
    return status;
}

/* ------------------------------------------------- The content resolver */

/* Serves the content option as the module of its namespace that the
   loader asks for without a hint; asks inner for everything else. */
typedef struct content_resolver {
    sxml_resource_resolver inner;
    sxml_byte_view namespace_uri;
    /* Empty without the content option. */
    sxml_byte_view content;
    /* The module's URI: the caller's static base URI (18.2: the content's
       static base URI is the caller's executable base URI); may be
       empty. */
    sxml_byte_view resource_uri;
    /* Content loaded without trust (18.2, the trusted option): its own
       module and schema imports, external resources, are not served. */
    bool refuses_imports;
} content_resolver;

static sxml_status resolve_content(void *state, const sxml_resource_request *request,
                                   sxml_resource *resource) {
    const content_resolver *resolver = state;
    if (resolver->content.byte_count != 0 &&
        request->resource_kind == SXML_RESOURCE_KIND_XQUERY_MODULE &&
        request->uri_reference.byte_count == 0 &&
        sxml_byte_view_equals(request->target_namespace_uri, resolver->namespace_uri)) {
        memset(resource, 0, sizeof *resource);
        resource->resource_form = SXML_RESOURCE_FORM_BYTES;
        resource->content = resolver->content;
        resource->resource_uri = resolver->resource_uri;
        return SXML_STATUS_OK;
    }
    if (resolver->inner.operations == NULL ||
        (resolver->refuses_imports &&
         (request->resource_kind == SXML_RESOURCE_KIND_XQUERY_MODULE ||
          request->resource_kind == SXML_RESOURCE_KIND_SCHEMA_DOCUMENT))) {
        memset(resource, 0, sizeof *resource);
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        return SXML_STATUS_OK;
    }
    return resolver->inner.operations->resolve(resolver->inner.state, request, resource);
}

static void release_content(void *state, const sxml_resource *resource) {
    const content_resolver *resolver = state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES &&
        resource->content.bytes == resolver->content.bytes) {
        return;
    }
    if (resolver->inner.operations != NULL && resolver->inner.operations->release != NULL &&
        resource->resource_form != SXML_RESOURCE_FORM_NONE) {
        resolver->inner.operations->release(resolver->inner.state, resource);
    }
}

static const sxml_resource_resolver_operations content_resolver_operations = {
    .resolve = resolve_content, .release = release_content};

/* ------------------------------------------------------------ Compiling */

/* The source name of the module record query, static so the unit and
   error records may keep it. */
static const sxml_byte_view record_source_name =
    SXML_BYTE_VIEW_INITIALIZER("fn:load-xquery-module");

/* What both passes over the module record query share. */
typedef struct compilation {
    const sxml_function_call *call;
    sxml_processor *processor;
    sxml_resource_resolver resolver;
    sxml_byte_view static_base_uri;
    sxml_namespace_uri_identifier namespace_identifier;
} compilation;

/* Maps a static error of the module record query to FOQM0002 (the
   import found no module) or FOQM0003; other failures stay. */
static sxml_status map_compile_error(const compilation *compiling, sxml_status status) {
    if (status != SXML_STATUS_STATIC_ERROR) {
        return status;
    }
    const sxml_error_record *record = compiling->call->error_record;
    if (record != NULL &&
        sxml_byte_view_equals(record->code.local_name, SXML_BYTE_VIEW_FROM_LITERAL("XQST0059")) &&
        sxml_byte_view_equals(record->diagnostic.location.source_name, record_source_name)) {
        return sxml_function_library_report(compiling->call, &module_not_found_error);
    }
    return sxml_function_library_report(compiling->call, &static_error_in_module_error);
}

/* Parses query_text, the module record query. */
static sxml_status parse_query(const compilation *compiling, sxml_byte_view query_text,
                               sxml_ast **ast) {
    return sxml_xquery_build_ast(
        &(sxml_ast_build_request){
            .expression_text = query_text,
            .source_name = record_source_name,
            .allocator = compiling->processor->allocator,
            .maximum_expression_depth =
                sxml_processor_get_maximum_expression_depth(compiling->processor),
            .error_record = compiling->call->error_record},
        ast);
}

/* The query text being written: its prolog is the first pass's query. */
typedef struct record_text {
    sxml_library_text *text;
    size_t prolog_byte_count;
    sxml_byte_view namespace_uri;
} record_text;

/* The first pass: loads the module set of the import alone, reads the
   public declarations of the modules of the namespace and, while their
   ASTs hold the names, writes the body after the prolog. */
static sxml_status find_declarations(const compilation *compiling, const record_text *record,
                                     public_declarations *declarations) {
    sxml_ast *ast = NULL;
    sxml_status status =
        parse_query(compiling, sxml_function_library_text_view(record->text), &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xquery_module_set set = {.allocator = compiling->processor->allocator};
    status = sxml_xquery_load_modules(
        &(sxml_xquery_module_loading){
            .main_ast = ast,
            .item_heap = compiling->processor->item_heap,
            .allocator = compiling->processor->allocator,
            .resource_resolver = compiling->resolver,
            .processor = compiling->processor,
            .static_base_uri = compiling->static_base_uri,
            .maximum_expression_depth =
                sxml_processor_get_maximum_expression_depth(compiling->processor),
            .error_record = compiling->call->error_record},
        &set);
    for (size_t module_index = 1; status == SXML_STATUS_OK && module_index < set.module_count;
         module_index += 1) {
        if (set.modules[module_index].target_namespace.index ==
            compiling->namespace_identifier.index) {
            status = read_declarations(declarations, set.modules[module_index].ast);
        }
    }
    if (status == SXML_STATUS_OK) {
        record->text->byte_count = record->prolog_byte_count;
        status = append_record(record->text, declarations, record->namespace_uri);
    }
    sxml_xquery_release_modules(&set);
    sxml_ast_destroy(ast);
    return status;
}

/* The second pass: compiles the whole module record query. */
static sxml_status compile_record(const compilation *compiling, sxml_byte_view query_text,
                                  sxml_ir_unit **unit) {
    sxml_ast *ast = NULL;
    sxml_status status = parse_query(compiling, query_text, &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_static_context_configuration configuration;
    memset(&configuration, 0, sizeof configuration);
    configuration.static_base_uri = compiling->static_base_uri;
    const sxml_xpath_static_context static_context =
        sxml_processor_static_context(compiling->processor, &configuration);
    status = sxml_xquery_compile_module(
        &(sxml_xquery_lowering_request){
            .ast = ast,
            .static_context = &static_context,
            .maximum_expression_count = 0,
            .maximum_expression_depth =
                sxml_processor_get_maximum_expression_depth(compiling->processor),
            .error_record = compiling->call->error_record,
            .resource_resolver = compiling->resolver,
            .processor = compiling->processor},
        unit);
    sxml_ast_destroy(ast);
    return status;
}

/* ------------------------------------------------------ Supplied values */

/* True when value matches type without conversion (18.2: "must conform
   to the required type ... without conversion"). */
static sxml_status check_match(const sxml_function_call *call, sxml_sequence value,
                               sxml_sequence_type sequence_type) {
    sxml_type_match match = {.is_match = false};
    const sxml_status status = sxml_evaluator_match_sequence_type(
        call->dynamic_context,
        &(sxml_type_match_request){.value = value,
                                   .sequence_type = sequence_type,
                                   .error_record = call->error_record},
        &match);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return match.is_match ? SXML_STATUS_OK : sxml_function_library_report(call, &wrong_value_error);
}

/* Binds the values of the variables option to the unit's external
   globals of their names (F&O 4.0, 18.2: "values for external
   variables"), each checked against its declared type. Other names, a
   variable with a value among them, are ignored. */
static sxml_status bind_variables(const sxml_function_call *call, sxml_item variables,
                                  load_state *load) {
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(variables, &entry_count);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry = {.key = {.representation = 0}};
        status = sxml_map_get_entry(variables, entry_index, &entry);
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof datum);
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(entry.key, &datum);
        }
        if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_QNAME) {
            status = sxml_function_library_report(call, &variable_key_error);
        }
        for (uint32_t global_number = 1;
             status == SXML_STATUS_OK && global_number <= load->global_value_count;
             global_number += 1) {
            const sxml_ir_global_variable *global = sxml_ir_unit_get_global_variable(
                load->unit, (sxml_ir_global_index){.value = global_number});
            if (!global->is_external ||
                global->variable_name.index != datum.datum_details.qname.expanded_name.index) {
                continue;
            }
            status = check_match(call, entry.value, global->declared_type);
            if (status == SXML_STATUS_OK) {
                sxml_evaluator_slot *slot = &load->global_values[global_number - 1];
                sxml_sequence_release(slot->value);
                sxml_sequence_retain(entry.value);
                *slot = (sxml_evaluator_slot){.value = entry.value,
                                              .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
            }
        }
    }
    return status;
}

/* XPTY0004 unless every key of the map option is an xs:QName: the
   vendor-options option is a map(xs:QName, item()*) (F&O 4.0, 18.2). */
static sxml_status check_qname_keys(const sxml_function_call *call, sxml_sequence option) {
    sxml_item map = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        option, (sxml_item_request){.item_index = 0, .error_record = NULL}, &map);
    size_t entry_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_map_get_size(map, &entry_count);
    }
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry = {.key = {.representation = 0}};
        status = sxml_map_get_entry(map, entry_index, &entry);
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof datum);
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(entry.key, &datum);
        }
        if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_QNAME) {
            status = sxml_function_library_report(call, &variable_key_error);
        }
    }
    return status;
}

/* Checks the context value against each context value declaration's
   type (the context globals, name 0). */
static sxml_status check_context_value(const sxml_function_call *call, const load_state *load) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t global_number = 1;
         status == SXML_STATUS_OK && global_number <= load->global_value_count;
         global_number += 1) {
        const sxml_ir_global_variable *global = sxml_ir_unit_get_global_variable(
            load->unit, (sxml_ir_global_index){.value = global_number});
        if (global->variable_name.index == 0) {
            status = check_match(call, load->context_value, global->declared_type);
        }
    }
    return status;
}

/* Allocates the unit's global values, all ABSENT. */
static sxml_status make_global_values(load_state *load) {
    if (load->unit == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const size_t global_count =
        load->unit->global_variable_count > 1 ? load->unit->global_variable_count - 1 : 0;
    if (global_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(global_count, sizeof(sxml_evaluator_slot), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            load->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_evaluator_slot)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    load->global_values = memory;
    load->global_value_count = global_count;
    for (size_t slot_index = 0; slot_index < global_count; slot_index += 1) {
        load->global_values[slot_index] = (sxml_evaluator_slot){
            .value = sxml_sequence_empty(), .slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT};
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------- The steps */

/* True when an xs:decimal option names a version above 4.0. */
static bool is_above_version_four(sxml_sequence version) {
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof datum);
    if (sxml_sequence_get_item(version, (sxml_item_request){.item_index = 0, .error_record = NULL}, &item) != SXML_STATUS_OK ||
        sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK) {
        return false;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        return datum.datum_details.integer > 4;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_DECIMAL) {
        return false;
    }
    /* The canonical form: optional "-", digits, and "." with digits. */
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
    /* 4.something: above 4.0 when a fraction digit is not 0. */
    for (byte_index += 1; byte_index < text.byte_count; byte_index += 1) {
        if (text.bytes[byte_index] != '0') {
            return true;
        }
    }
    return false;
}

/* What the first step reads from its options. */
typedef struct load_request {
    const sxml_function_call *call;
    const sxml_option_values *options;
    sxml_byte_view namespace_uri;
} load_request;

/* Writes the module record query and compiles it into load->unit. */
static sxml_status compile_module(const load_request *request, load_state *load) {
    const sxml_function_call *call = request->call;
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    if (environment == NULL || environment->processor == NULL) {
        return sxml_function_library_report(call, &no_processor_error);
    }
    const sxml_option_values *options = request->options;
    const sxml_string content = sxml_function_library_option_string(
        (sxml_option_lookup){.values = options, .option_index = SXML_LOAD_OPTION_CONTENT});
    const sxml_byte_view static_base_uri =
        sxml_dynamic_context_get_static_base_uri(call->dynamic_context);
    const bool is_trusted =
        sxml_function_library_option_is_true(
            (sxml_option_lookup){.values = options, .option_index = SXML_LOAD_OPTION_TRUSTED}) &&
        sxml_dynamic_context_check_trust(call->dynamic_context, NULL) == SXML_STATUS_OK;
    content_resolver resolver = {.inner = environment->resource_resolver,
                                 .namespace_uri = request->namespace_uri,
                                 .content = content.utf8,
                                 .resource_uri = static_base_uri,
                                 .refuses_imports = content.utf8.byte_count != 0 && !is_trusted};
    compilation compiling = {
        .call = call,
        .processor = environment->processor,
        .resolver = {.state = &resolver, .operations = &content_resolver_operations},
        .static_base_uri = static_base_uri,
        .namespace_identifier = {.index = 0}};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        sxml_item_heap_get_name_pool(call->item_heap), request->namespace_uri,
        &compiling.namespace_identifier);
    sxml_library_text text = {.item_heap = call->item_heap};
    if (status == SXML_STATUS_OK) {
        status = append_prolog(
            &text, &(prolog_request){
                       .namespace_uri = request->namespace_uri,
                       .location_hints = options->values[SXML_LOAD_OPTION_LOCATION_HINTS],
                       .has_content = content.utf8.byte_count != 0});
    }
    const size_t prolog_byte_count = text.byte_count;
    if (status == SXML_STATUS_OK) {
        status = append(&text, SXML_BYTE_VIEW_FROM_LITERAL("()"));
    }
    public_declarations declarations = {.allocator = load->allocator};
    if (status == SXML_STATUS_OK) {
        status = find_declarations(&compiling,
                                   &(record_text){.text = &text,
                                                  .prolog_byte_count = prolog_byte_count,
                                                  .namespace_uri = request->namespace_uri},
                                   &declarations);
    }
    if (status == SXML_STATUS_OK) {
        status = compile_record(&compiling, sxml_function_library_text_view(&text), &load->unit);
    }
    if (status == SXML_STATUS_OK && !is_trusted) {
        /* 18.2: trusted is false by default, and a module an untrusted
           call loads is untrusted too. */
        sxml_ir_unit_set_untrusted(load->unit);
    }
    release_declarations(&declarations);
    sxml_function_library_text_release(&text);
    return map_compile_error(&compiling, status);
}

/* The first step: compiles, binds the supplied values and asks for the
   unit evaluation. */
static sxml_status start_loading(const sxml_native_step_request *request, sxml_native_step *step) {
    const sxml_function_call *call = request->call;
    load_state *load = request->state;
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *load = (load_state){.allocator = call->dynamic_context->allocator,
                         .context_value = sxml_sequence_empty()};
    sxml_string namespace_uri = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &namespace_uri);
    if (status == SXML_STATUS_OK && namespace_uri.utf8.byte_count == 0) {
        return sxml_function_library_report(call, &no_namespace_error);
    }
    sxml_option_values options;
    memset(&options, 0, sizeof options);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_options(
            call, &(sxml_option_request){
                      .argument_index = 1,
                      .definitions = {.definitions = option_definitions,
                                      .definition_count = SXML_LOAD_OPTION_COUNT}},
            &options);
    }
    if (status == SXML_STATUS_OK && options.is_present[SXML_LOAD_OPTION_XQUERY_VERSION] &&
        is_above_version_four(options.values[SXML_LOAD_OPTION_XQUERY_VERSION])) {
        status = sxml_function_library_report(call, &no_processor_error);
    }
    if (status == SXML_STATUS_OK && options.is_present[SXML_LOAD_OPTION_CONTEXT_VALUE] &&
        options.is_present[SXML_LOAD_OPTION_CONTEXT_ITEM]) {
        status = sxml_function_library_report(call, &wrong_value_error);
    }
    if (status == SXML_STATUS_OK && options.is_present[SXML_LOAD_OPTION_VENDOR_OPTIONS]) {
        status = check_qname_keys(call, options.values[SXML_LOAD_OPTION_VENDOR_OPTIONS]);
    }
    if (status == SXML_STATUS_OK) {
        status = compile_module(&(load_request){.call = call,
                                                .options = &options,
                                                .namespace_uri = namespace_uri.utf8},
                                load);
    }
    if (status == SXML_STATUS_OK) {
        status = make_global_values(load);
    }
    if (status == SXML_STATUS_OK && options.is_present[SXML_LOAD_OPTION_VARIABLES]) {
        sxml_item variables = {.representation = 0};
        status = sxml_sequence_get_item(options.values[SXML_LOAD_OPTION_VARIABLES], (sxml_item_request){.item_index = 0, .error_record = NULL}, &variables);
        if (status == SXML_STATUS_OK) {
            status = bind_variables(call, variables, load);
        }
    }
    /* 18.2: context-item () is the absence of a context value;
       context-value () is an empty one. */
    bool has_context_value = options.is_present[SXML_LOAD_OPTION_CONTEXT_VALUE];
    if (has_context_value) {
        load->context_value = options.values[SXML_LOAD_OPTION_CONTEXT_VALUE];
        sxml_sequence_retain(load->context_value);
    } else if (options.is_present[SXML_LOAD_OPTION_CONTEXT_ITEM]) {
        uint64_t item_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(options.values[SXML_LOAD_OPTION_CONTEXT_ITEM],
                                         call->error_record, &item_count);
        }
        has_context_value = item_count != 0;
        if (has_context_value) {
            load->context_value = options.values[SXML_LOAD_OPTION_CONTEXT_ITEM];
            sxml_sequence_retain(load->context_value);
        }
    }
    if (status == SXML_STATUS_OK && has_context_value) {
        status = check_context_value(call, load);
    }
    sxml_function_library_release_options(&options);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    load->evaluation = (sxml_evaluation_request){
        .unit = load->unit,
        .has_context_value = has_context_value,
        .context_value = load->context_value,
        .global_values = {.slots = load->global_values, .slot_count = load->global_value_count},
        .static_base_uri = {.bytes = NULL, .byte_count = 0},
        .error_record = NULL};
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_EVALUATE_UNIT,
                               .result = sxml_sequence_empty(),
                               .function_item = {.representation = 0},
                               .arguments = {.sequences = NULL, .sequence_count = 0},
                               .unit_evaluation = &load->evaluation};
    return SXML_STATUS_OK;
}

static sxml_status step_load_xquery_module(const sxml_native_step_request *request,
                                           sxml_native_step *step) {
    if (request->step_number == 0) {
        return start_loading(request, step);
    }
    /* The module record the unit's main function made. */
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                               .result = request->call_result,
                               .function_item = {.representation = 0},
                               .arguments = {.sequences = NULL, .sequence_count = 0},
                               .unit_evaluation = NULL};
    return SXML_STATUS_OK;
}

/* The body of the row in src/function_library_xquery_module.c. */
const sxml_resumable_native_body sxml_xquery_load_module_body = {
    .step = step_load_xquery_module,
    .release_state = release_load_state,
    .state_byte_count = sizeof(load_state),
    .state_alignment = alignof(load_state)};

/* ------------------------------------------------------ Function table */

static sxml_status find_xquery_function(void *state, const sxml_function_query *query,
                                        sxml_function_entry *entry) {
    const sxml_xquery_function_table *table = state;
    sxml_status status = sxml_xpath_find_function(table->host_table, query, entry);
    if (status == SXML_STATUS_OK && entry->entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE) {
        status = sxml_xpath_find_function(sxml_function_library_as_table(table->library), query,
                                          entry);
    }
    return status;
}

static const sxml_function_table_operations xquery_function_table_operations = {
    .find_function = find_xquery_function};

sxml_status sxml_xquery_function_table_initialize(sxml_xquery_function_table *table,
                                                  const sxml_xpath_static_context *host) {
    /* fn:put is XQuery's, of the Update Facility (XQUF 3.0, 6.1). */
    static const sxml_native_function_view *const parts[] = {&sxml_xquery_put_functions};
    *table = (sxml_xquery_function_table){.host_table = host->function_table, .library = NULL};
    return sxml_function_library_create(
        host->allocator,
        &(sxml_function_library_configuration){
            .allocator = host->allocator,
            .item_heap = host->item_heap,
            .parts = {.native_function_views = parts,
                      .native_function_view_count = sizeof parts / sizeof parts[0]}},
        &table->library);
}

sxml_function_table sxml_xquery_function_table_as_table(sxml_xquery_function_table *table) {
    return (sxml_function_table){.state = table, .operations = &xquery_function_table_operations};
}

void sxml_xquery_function_table_finalize(sxml_xquery_function_table *table) {
    sxml_function_library_destroy(table->library);
    table->library = NULL;
}
