/*
 * xsl:evaluate at run time (src/xslt_evaluate_internal.h; XSLT 4.0, 10.5;
 * glossary `dynamic_evaluation`; packet 7.3).
 *
 * The row's body is resumable (D71, D75): its first step builds the
 * target expression's static context (10.5.1) from its arguments and the
 * lexical static context of the call (the xsl:evaluate element's module,
 * D163), compiles the text with the XPath compiler (sxml_xpath_compile),
 * binds the parameters and the context value (10.5.2), and asks the
 * machine for a unit evaluation (glossary `unit_evaluation`, D154); the
 * second step finishes with its value. The compiled unit is evaluated on
 * the same machine, so it runs in the same execution scope (10.5.3):
 * fn:doc and fn:current-dateTime agree with the stylesheet's.
 *
 * Errors: XTDE3160 for a static error of the target expression, XTTE3210
 * for a context-item value of more than one item, XTTE3165
 * for a with-params value that is not one map with xs:QName keys,
 * XTTE3170 for a namespace-context value that is not one node, XTDE0030
 * for a schema-aware or trusted value that is not a boolean word; dynamic
 * errors of the target expression keep their codes (10.5.3).
 *
 * Golden file: src/xquery_load_module.c (a resumable body that compiles,
 * then asks for a unit evaluation).
 */
#include "xslt_evaluate_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "memory_internal.h"
#include "processor_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"
#include "xpath_api_internal.h"
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

static const sxml_static_error invalid_expression_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE3160"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the target expression of xsl:evaluate is not a valid XPath expression (XSLT 4.0, "
        "10.5)")};

static const sxml_static_error with_params_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE3165"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the with-params value of xsl:evaluate is not a single map(xs:QName, item()*) "
        "(XSLT 4.0, 10.5)")};

static const sxml_static_error namespace_context_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE3170"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the namespace-context value of xsl:evaluate is not a single node (XSLT 4.0, "
        "10.5.1)")};

static const sxml_static_error context_item_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE3210"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the context-item value of xsl:evaluate has more than one item (XSLT 3.0, 10.4.2; "
        "D196)")};

static const sxml_static_error boolean_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE0030"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the schema-aware or trusted attribute of xsl:evaluate is not yes, true, 1, no, "
        "false or 0 (XSLT 4.0, 3.3)")};

static const sxml_static_error no_processor_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE3160"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "xsl:evaluate is evaluated without the processor's evaluation environment (XSLT "
        "4.0, 10.5)")};

/* ------------------------------------------------------------- Arguments */

/* The argument positions of the row. */
typedef enum evaluate_argument {
    SXML_EVALUATE_ARGUMENT_XPATH,
    SXML_EVALUATE_ARGUMENT_PARAMETERS,
    SXML_EVALUATE_ARGUMENT_WITH_PARAMS,
    SXML_EVALUATE_ARGUMENT_HAS_WITH_PARAMS,
    SXML_EVALUATE_ARGUMENT_CONTEXT,
    SXML_EVALUATE_ARGUMENT_HAS_CONTEXT,
    SXML_EVALUATE_ARGUMENT_NAMESPACE_CONTEXT,
    SXML_EVALUATE_ARGUMENT_HAS_NAMESPACE_CONTEXT,
    SXML_EVALUATE_ARGUMENT_BASE_URI,
    SXML_EVALUATE_ARGUMENT_SCHEMA_AWARE,
    SXML_EVALUATE_ARGUMENT_TRUSTED,
    SXML_EVALUATE_ARGUMENT_FUNCTIONS,
    SXML_EVALUATE_ARGUMENT_COUNT,
} evaluate_argument;

/* The xs:boolean argument at argument_index. */
static sxml_status read_flag(const sxml_function_call *call, evaluate_argument argument_index,
                             bool *flag) {
    sxml_optional_item item = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &item);
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof datum);
    if (status == SXML_STATUS_OK && item.has_item) {
        status = sxml_atomic_item_get_datum(item.item, &datum);
    }
    *flag = status == SXML_STATUS_OK && item.has_item &&
            datum.datum_kind == SXML_DATUM_KIND_BOOLEAN && datum.datum_details.boolean;
    return status;
}

/* The effective value of a boolean attribute given as its string (3.3:
   yes, true or 1; no, false or 0; whitespace trimmed); "" is no. */
static sxml_status read_boolean_word(const sxml_function_call *call,
                                     evaluate_argument argument_index, bool *is_yes) {
    sxml_string text = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    const sxml_status status =
        sxml_function_library_read_string_argument(call, argument_index, &text);
    *is_yes = false;
    if (status != SXML_STATUS_OK || text.utf8.byte_count == 0) {
        return status;
    }
    const sxml_xslt_boolean_text answer = sxml_xslt_lowering_read_boolean_text(text.utf8);
    if (!answer.is_valid) {
        return sxml_function_library_report(call, &boolean_error);
    }
    *is_yes = answer.is_yes;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ The state */

/* One parameter of the target expression: an external variable. */
typedef struct evaluate_parameter {
    sxml_name_identifier parameter_name;
    /* Borrowed from a map argument. */
    sxml_sequence value;
} evaluate_parameter;

typedef struct evaluate_state {
    sxml_allocator allocator;
    /* Owned: the compiled target expression. */
    sxml_ir_unit *unit;
    /* Owned values, one slot per global of the unit. */
    sxml_evaluator_slot *global_values;
    size_t global_value_count;
    /* Owned. */
    sxml_sequence context_value;
    sxml_evaluation_request evaluation;
} evaluate_state;

static void release_evaluate_state(void *state) {
    evaluate_state *evaluation = state;
    for (size_t slot_index = 0; slot_index < evaluation->global_value_count; slot_index += 1) {
        sxml_sequence_release(evaluation->global_values[slot_index].value);
    }
    sxml_allocator_release(evaluation->allocator, evaluation->global_values);
    evaluation->global_values = NULL;
    evaluation->global_value_count = 0;
    sxml_sequence_release(evaluation->context_value);
    evaluation->context_value = sxml_sequence_empty();
    sxml_ir_unit_release(evaluation->unit);
    evaluation->unit = NULL;
}

/* ------------------------------------------------------------ Parameters */

/* What the first step builds for the compilation, released after it. */
typedef struct evaluate_build {
    const sxml_function_call *call;
    sxml_allocator allocator;
    evaluate_parameter *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
    sxml_variable_declaration *declarations;
    sxml_namespace_binding *namespace_bindings;
    size_t namespace_binding_count;
    sxml_uri_resolution base_uri;
} evaluate_build;

static void release_build(evaluate_build *build) {
    sxml_allocator_release(build->allocator, build->parameters);
    sxml_allocator_release(build->allocator, build->declarations);
    sxml_allocator_release(build->allocator, build->namespace_bindings);
    sxml_allocator_release(build->allocator, build->base_uri.memory);
    build->parameters = NULL;
    build->declarations = NULL;
    build->namespace_bindings = NULL;
    build->base_uri.memory = NULL;
}

/* Puts name = value, replacing an earlier value of the name (10.5.1: the
   with-params map takes precedence over xsl:with-param). */
static void put_parameter(evaluate_build *build, const evaluate_parameter *parameter) {
    for (size_t parameter_index = 0; parameter_index < build->parameter_count;
         parameter_index += 1) {
        if (build->parameters[parameter_index].parameter_name.index ==
            parameter->parameter_name.index) {
            build->parameters[parameter_index].value = parameter->value;
            return;
        }
    }
    if (build->parameter_count < build->parameter_capacity) {
        build->parameters[build->parameter_count] = *parameter;
        build->parameter_count += 1;
    }
}

/* A map whose keys are xs:QName values and the map it is, to read. */
typedef struct parameter_source {
    sxml_item map;
    /* XTTE3165 for a key that is not an xs:QName (with-params); false for
       the xsl:with-param map, whose keys lowering made. */
    bool is_checked;
} parameter_source;

static sxml_status put_parameters(evaluate_build *build, parameter_source source) {
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(source.map, &entry_count);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry = {.key = {.representation = 0}};
        status = sxml_map_get_entry(source.map, entry_index, &entry);
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof datum);
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(entry.key, &datum);
        }
        if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_QNAME) {
            status = source.is_checked ? sxml_function_library_report(build->call, &with_params_error)
                                       : SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status == SXML_STATUS_OK) {
            put_parameter(build, &(evaluate_parameter){
                                     .parameter_name = datum.datum_details.qname.expanded_name,
                                     .value = entry.value});
        }
    }
    return status;
}

/* The map of argument argument_index, or with is_map false when the value
   is not one map. */
typedef struct map_argument {
    bool is_map;
    sxml_item map;
} map_argument;

static sxml_status read_map_argument(const sxml_function_call *call,
                                     evaluate_argument argument_index, map_argument *argument) {
    const sxml_sequence value = call->arguments.sequences[argument_index];
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(value, call->error_record, &item_count);
    *argument = (map_argument){.is_map = false, .map = {.representation = 0}};
    if (status == SXML_STATUS_OK && item_count == 1) {
        status = sxml_sequence_get_item(
            value, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
            &argument->map);
        argument->is_map = status == SXML_STATUS_OK &&
                           sxml_item_get_kind(argument->map) == SXML_ITEM_KIND_MAP;
    }
    return status;
}

/* Collects the parameters: xsl:with-param first, then with-params over
   them; each becomes an external variable of type item()*. */
static sxml_status collect_parameters(evaluate_build *build) {
    const sxml_function_call *call = build->call;
    map_argument static_parameters = {.is_map = false};
    map_argument dynamic_parameters = {.is_map = false};
    bool has_with_params = false;
    sxml_status status =
        read_map_argument(call, SXML_EVALUATE_ARGUMENT_PARAMETERS, &static_parameters);
    if (status == SXML_STATUS_OK) {
        status = read_flag(call, SXML_EVALUATE_ARGUMENT_HAS_WITH_PARAMS, &has_with_params);
    }
    if (status == SXML_STATUS_OK && has_with_params) {
        status = read_map_argument(call, SXML_EVALUATE_ARGUMENT_WITH_PARAMS, &dynamic_parameters);
        if (status == SXML_STATUS_OK && !dynamic_parameters.is_map) {
            status = sxml_function_library_report(call, &with_params_error);
        }
    }
    size_t static_count = 0;
    size_t dynamic_count = 0;
    if (status == SXML_STATUS_OK && static_parameters.is_map) {
        status = sxml_map_get_size(static_parameters.map, &static_count);
    }
    if (status == SXML_STATUS_OK && dynamic_parameters.is_map) {
        status = sxml_map_get_size(dynamic_parameters.map, &dynamic_count);
    }
    if (status == SXML_STATUS_OK) {
        build->parameter_capacity = static_count + dynamic_count;
        status = sxml_allocator_allocate_array(
            build->allocator,
            (sxml_array_allocation_request){.element_count = build->parameter_capacity,
                                            .element_size = sizeof(evaluate_parameter),
                                            .element_alignment = alignof(evaluate_parameter)},
            (void **)&build->parameters);
    }
    if (status == SXML_STATUS_OK && static_parameters.is_map) {
        status = put_parameters(build, (parameter_source){.map = static_parameters.map,
                                                          .is_checked = false});
    }
    if (status == SXML_STATUS_OK && dynamic_parameters.is_map) {
        status = put_parameters(build, (parameter_source){.map = dynamic_parameters.map,
                                                          .is_checked = true});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            build->allocator,
            (sxml_array_allocation_request){.element_count = build->parameter_count,
                                            .element_size = sizeof(sxml_variable_declaration),
                                            .element_alignment =
                                                alignof(sxml_variable_declaration)},
            (void **)&build->declarations);
    }
    for (size_t parameter_index = 0; status == SXML_STATUS_OK &&
                                     parameter_index < build->parameter_count;
         parameter_index += 1) {
        build->declarations[parameter_index] = (sxml_variable_declaration){
            .variable_name = build->parameters[parameter_index].parameter_name,
            .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                              .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    }
    return status;
}

/* ------------------------------------------------------------ Namespaces */

/* True when binding may be given to the XPath static context: not the xml
   prefix (predeclared, never rebound) and no undeclaration of a prefix. */
static bool is_usable_binding(const evaluate_build *build, sxml_namespace_binding binding) {
    if (binding.namespace_prefix.index == 0) {
        return true;
    }
    if (binding.namespace_uri.index == 0) {
        return false;
    }
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    return sxml_name_pool_get_namespace_prefix(sxml_item_heap_get_name_pool(build->call->item_heap),
                                               binding.namespace_prefix,
                                               &prefix) == SXML_STATUS_OK &&
           !sxml_byte_view_equals(prefix, SXML_BYTE_VIEW_FROM_LITERAL("xml"));
}

/* The statically known namespaces (10.5.1): the in-scope namespaces of
   the namespace-context node, or the instruction's own. */
static sxml_status collect_namespaces(evaluate_build *build) {
    const sxml_function_call *call = build->call;
    bool has_namespace_context = false;
    sxml_status status =
        read_flag(call, SXML_EVALUATE_ARGUMENT_HAS_NAMESPACE_CONTEXT, &has_namespace_context);
    sxml_namespace_binding_view bindings =
        sxml_dynamic_context_get_static_namespaces(call->dynamic_context);
    if (status == SXML_STATUS_OK && has_namespace_context) {
        const sxml_sequence value = call->arguments.sequences[SXML_EVALUATE_ARGUMENT_NAMESPACE_CONTEXT];
        uint64_t item_count = 0;
        sxml_item item = {.representation = 0};
        sxml_node_reference node = {.tree = NULL};
        status = sxml_sequence_count(value, call->error_record, &item_count);
        if (status == SXML_STATUS_OK && item_count == 1) {
            status = sxml_sequence_get_item(
                value, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
                &item);
        }
        if (status == SXML_STATUS_OK &&
            (item_count != 1 || sxml_item_get_kind(item) != SXML_ITEM_KIND_NODE)) {
            return sxml_function_library_report(call, &namespace_context_error);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_item_get_node(item, &node);
        }
        sxml_node_description description;
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(node, &description);
        }
        bindings = (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                                 .namespace_binding_count = 0};
        if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT) {
            status = sxml_node_get_in_scope_namespaces(node, &bindings);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            build->allocator,
            (sxml_array_allocation_request){.element_count = bindings.namespace_binding_count,
                                            .element_size = sizeof(sxml_namespace_binding),
                                            .element_alignment = alignof(sxml_namespace_binding)},
            (void **)&build->namespace_bindings);
    }
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
        if (is_usable_binding(build, binding)) {
            build->namespace_bindings[build->namespace_binding_count] = binding;
            build->namespace_binding_count += 1;
        }
    }
    return status;
}

/* ------------------------------------------------------ Function table */

/* The target expression's statically known functions (10.5.1): the
   stylesheet functions of the $functions map, then the library's (F&O, the
   math, map and array namespaces and the constructor functions), then
   fn:current from the stylesheet's table (XSLT 4.0 allows it there: the
   current item of the target expression is its context item, 20.5.1); no
   other XSLT-defined function. */
typedef struct evaluate_function_table {
    sxml_function_table library;
    sxml_item functions;
    sxml_item_heap *item_heap;
    /* The evaluation's table, which has the XSLT functions. */
    sxml_function_table xslt_functions;
    /* fn:current; 0 when never interned. */
    sxml_name_identifier current_name;
} evaluate_function_table;

/* Writes the item of the function name's entry in the map whose arity is
   query->arity, if there is one. */
static sxml_status find_stylesheet_function(const evaluate_function_table *table,
                                            const sxml_function_query *query,
                                            sxml_function_entry *entry) {
    sxml_item key = {.representation = 0};
    sxml_status status = sxml_atomic_item_make(
        table->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = query->function_name,
                                                  .namespace_prefix = {.index = 0}}}}},
        &key);
    sxml_map_lookup lookup = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_find(table->functions, key, &lookup);
    }
    sxml_item_release(key);
    uint64_t item_count = 0;
    if (status == SXML_STATUS_OK && lookup.is_found) {
        status = sxml_sequence_count(lookup.entry.value, NULL, &item_count);
    }
    for (uint64_t item_index = 0; status == SXML_STATUS_OK && item_index < item_count;
         item_index += 1) {
        sxml_item function_item = {.representation = 0};
        sxml_function_item_description description;
        memset(&description, 0, sizeof description);
        status = sxml_sequence_get_item(lookup.entry.value,
                                        (sxml_item_request){.item_index = item_index,
                                                            .error_record = NULL},
                                        &function_item);
        if (status == SXML_STATUS_OK) {
            status = sxml_function_item_describe(function_item, &description);
        }
        if (status != SXML_STATUS_OK || description.arity != query->arity) {
            continue;
        }
        sxml_type_description signature;
        memset(&signature, 0, sizeof signature);
        status = sxml_type_registry_describe_type(sxml_item_heap_get_type_registry(table->item_heap),
                                                  description.signature, &signature);
        if (status == SXML_STATUS_OK) {
            *entry = (sxml_function_entry){
                .entry_kind = SXML_FUNCTION_ENTRY_KIND_FUNCTION_ITEM,
                .function_item = function_item,
                .parameters = {.function_parameters = NULL, .function_parameter_count = 0},
                .signature = description.signature,
                .parameter_types = signature.variety_details.function_type.parameter_types,
                .result_type = signature.variety_details.function_type.result_type};
        }
        return status;
    }
    return status;
}

static sxml_status find_evaluable_function(void *state, const sxml_function_query *query,
                                           sxml_function_entry *entry) {
    const evaluate_function_table *table = state;
    sxml_status status = find_stylesheet_function(table, query, entry);
    if (status == SXML_STATUS_OK && entry->entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE) {
        status = sxml_xpath_find_function(table->library, query, entry);
    }
    if (status == SXML_STATUS_OK && entry->entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE &&
        table->current_name.index != 0 &&
        query->function_name.index == table->current_name.index) {
        status = sxml_xpath_find_function(table->xslt_functions, query, entry);
    }
    return status;
}

static const sxml_function_table_operations evaluate_function_table_operations = {
    .find_function = find_evaluable_function};

/* ------------------------------------------------------------- Compiling */

/* The static base URI (10.5.1): the base-uri attribute's value, resolved
   against the instruction's, or the instruction's. */
static sxml_status resolve_base_uri(evaluate_build *build) {
    const sxml_function_call *call = build->call;
    const sxml_byte_view instruction_base_uri =
        sxml_dynamic_context_get_static_base_uri(call->dynamic_context);
    sxml_string given = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    const sxml_status status =
        sxml_function_library_read_string_argument(call, SXML_EVALUATE_ARGUMENT_BASE_URI, &given);
    if (status != SXML_STATUS_OK || given.utf8.byte_count == 0) {
        build->base_uri = (sxml_uri_resolution){.uri = instruction_base_uri, .memory = NULL};
        return status;
    }
    return sxml_uri_resolve(&(sxml_uri_resolution_request){.base_uri = instruction_base_uri,
                                                           .reference = given.utf8,
                                                           .allocator = build->allocator},
                            &build->base_uri);
}

/* Compiles the target expression into state->unit. */
static sxml_status compile_target(evaluate_build *build, evaluate_state *state) {
    const sxml_function_call *call = build->call;
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    if (environment == NULL || environment->processor == NULL) {
        return sxml_function_library_report(call, &no_processor_error);
    }
    sxml_string xpath = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    bool is_schema_aware = false;
    bool is_trusted = false;
    sxml_status status =
        sxml_function_library_read_string_argument(call, SXML_EVALUATE_ARGUMENT_XPATH, &xpath);
    if (status == SXML_STATUS_OK) {
        status = read_boolean_word(call, SXML_EVALUATE_ARGUMENT_SCHEMA_AWARE, &is_schema_aware);
    }
    if (status == SXML_STATUS_OK) {
        status = read_boolean_word(call, SXML_EVALUATE_ARGUMENT_TRUSTED, &is_trusted);
    }
    if (status == SXML_STATUS_OK) {
        status = collect_parameters(build);
    }
    if (status == SXML_STATUS_OK) {
        status = collect_namespaces(build);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_base_uri(build);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_static_context_configuration configuration;
    memset(&configuration, 0, sizeof configuration);
    configuration.namespace_bindings =
        (sxml_namespace_binding_view){.namespace_bindings = build->namespace_bindings,
                                      .namespace_binding_count = build->namespace_binding_count};
    configuration.static_base_uri = build->base_uri.uri;
    configuration.variable_declarations = (sxml_variable_declaration_view){
        .variable_declarations = build->declarations,
        .variable_declaration_count = build->parameter_count};
    sxml_xpath_static_context static_context =
        sxml_processor_static_context(environment->processor, &configuration);
    /* 10.5.1: the collations, default collation and decimal formats of
       the instruction; its in-scope schema definitions only with
       schema-aware="yes" (the built-in types otherwise). XPath 1.0
       compatibility mode is false. */
    const sxml_ir_static_context *instruction_context =
        sxml_dynamic_context_get_static_context(call->dynamic_context);
    if (instruction_context != NULL) {
        static_context.unit_static_context = *instruction_context;
    }
    /* 10.5.1: the instruction's applicable static namespaces, or the
       in-scope namespaces of namespace-context; no other predeclared
       prefix (D261). */
    static_context.omits_predeclared_namespaces = true;
    map_argument functions = {.is_map = false};
    status = read_map_argument(call, SXML_EVALUATE_ARGUMENT_FUNCTIONS, &functions);
    if (status != SXML_STATUS_OK || !functions.is_map) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    evaluate_function_table function_table = {
        .library = static_context.function_table,
        .functions = functions.map,
        .item_heap = call->item_heap,
        .xslt_functions = call->dynamic_context->function_table,
        .current_name = {.index = 0}};
    (void)sxml_name_pool_find_name(
        sxml_item_heap_get_name_pool(call->item_heap),
        &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                 SXML_NAMESPACE_FUNCTIONS),
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("current")},
        &function_table.current_name);
    static_context.function_table = (sxml_function_table){
        .state = &function_table, .operations = &evaluate_function_table_operations};
    if (!is_schema_aware) {
        static_context.unit_static_context.in_scope_schemas =
            (sxml_schema_view){.schemas = NULL, .schema_count = 0};
    }
    status = sxml_xpath_compile(
        &(sxml_xpath_compile_request){
            .expression_text = xpath.utf8,
            .source_name = {.bytes = NULL, .byte_count = 0},
            .static_context = &static_context,
            .maximum_expression_count = 0,
            .maximum_expression_depth =
                sxml_processor_get_maximum_expression_depth(environment->processor),
            .error_record = call->error_record},
        &state->unit);
    if (status == SXML_STATUS_STATIC_ERROR) {
        /* XTDE3160, with the static error's description and location. */
        if (call->error_record == NULL || call->error_record->code.local_name.byte_count == 0) {
            return sxml_function_library_report(call, &invalid_expression_error);
        }
        call->error_record->code.local_name = invalid_expression_error.code_local_name;
        return SXML_STATUS_DYNAMIC_ERROR;
    }
    if (status == SXML_STATUS_OK &&
        (!is_trusted || sxml_dynamic_context_check_trust(call->dynamic_context, NULL) !=
                            SXML_STATUS_OK)) {
        /* 10.5.1: trusted="no", the default, gives the target expression no
           access to external resources; an untrusted caller's is untrusted
           too. */
        sxml_ir_unit_set_untrusted(state->unit);
    }
    return status;
}

/* Binds each parameter to its global (the unit's globals are the
   variable declarations, in order). */
static sxml_status bind_parameters(const evaluate_build *build, evaluate_state *state) {
    if (state->unit == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const size_t global_count =
        state->unit->global_variable_count > 1 ? state->unit->global_variable_count - 1 : 0;
    if (global_count < build->parameter_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_status status = sxml_allocator_allocate_array(
        state->allocator,
        (sxml_array_allocation_request){.element_count = global_count,
                                        .element_size = sizeof(sxml_evaluator_slot),
                                        .element_alignment = alignof(sxml_evaluator_slot)},
        (void **)&state->global_values);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->global_value_count = global_count;
    for (size_t slot_index = 0; slot_index < global_count; slot_index += 1) {
        state->global_values[slot_index] = (sxml_evaluator_slot){
            .value = sxml_sequence_empty(), .slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT};
    }
    for (size_t parameter_index = 0; parameter_index < build->parameter_count;
         parameter_index += 1) {
        const sxml_sequence value = build->parameters[parameter_index].value;
        sxml_sequence_retain(value);
        state->global_values[parameter_index] = (sxml_evaluator_slot){
            .value = value, .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- The steps */

/* The first step: compiles, binds and asks for the unit evaluation. */
static sxml_status start_evaluation(const sxml_native_step_request *request,
                                    sxml_native_step *step) {
    const sxml_function_call *call = request->call;
    evaluate_state *state = request->state;
    if (call->dynamic_context == NULL ||
        call->arguments.sequence_count != SXML_EVALUATE_ARGUMENT_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *state = (evaluate_state){.allocator = call->dynamic_context->allocator,
                              .context_value = sxml_sequence_empty()};
    evaluate_build build = {.call = call, .allocator = state->allocator};
    sxml_status status = compile_target(&build, state);
    if (status == SXML_STATUS_OK) {
        status = bind_parameters(&build, state);
    }
    release_build(&build);
    /* 10.5.2: the context-item value is the context item, position and
       size 1; without it, or when it is empty, there is no context. More
       than one item is XTTE3210, as in XSLT 3.0 (the 4.0 draft makes it the
       context value; D196). */
    bool has_context = false;
    uint64_t item_count = 0;
    if (status == SXML_STATUS_OK) {
        status = read_flag(call, SXML_EVALUATE_ARGUMENT_HAS_CONTEXT, &has_context);
    }
    const sxml_sequence context = call->arguments.sequences[SXML_EVALUATE_ARGUMENT_CONTEXT];
    if (status == SXML_STATUS_OK && has_context) {
        status = sxml_sequence_count(context, call->error_record, &item_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count > 1) {
        return sxml_function_library_report(call, &context_item_error);
    }
    if (item_count > 0) {
        state->context_value = context;
        sxml_sequence_retain(context);
    }
    state->evaluation = (sxml_evaluation_request){
        .unit = state->unit,
        .has_context_value = item_count > 0,
        .context_value = state->context_value,
        .global_values = {.slots = state->global_values, .slot_count = state->global_value_count},
        .static_base_uri = {.bytes = NULL, .byte_count = 0},
        .error_record = NULL};
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_EVALUATE_UNIT,
                               .result = sxml_sequence_empty(),
                               .function_item = {.representation = 0},
                               .arguments = {.sequences = NULL, .sequence_count = 0},
                               .unit_evaluation = &state->evaluation};
    return SXML_STATUS_OK;
}

static sxml_status step_evaluate(const sxml_native_step_request *request,
                                 sxml_native_step *step) {
    if (request->step_number == 0) {
        return start_evaluation(request, step);
    }
    /* The target expression's value (10.5.3). */
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                               .result = request->call_result,
                               .function_item = {.representation = 0},
                               .arguments = {.sequences = NULL, .sequence_count = 0},
                               .unit_evaluation = NULL};
    return SXML_STATUS_OK;
}

static const sxml_resumable_native_body evaluate_body = {
    .step = step_evaluate,
    .release_state = release_evaluate_state,
    .state_byte_count = sizeof(evaluate_state),
    .state_alignment = alignof(evaluate_state)};

static const sxml_function_parameter evaluate_parameters[] = {
    SXML_REQUIRED_PARAMETER("xpath", "xs:string"),
    SXML_REQUIRED_PARAMETER("parameters", "map(*)"),
    SXML_REQUIRED_PARAMETER("with-params", "item()*"),
    SXML_REQUIRED_PARAMETER("has-with-params", "xs:boolean"),
    SXML_REQUIRED_PARAMETER("context", "item()*"),
    SXML_REQUIRED_PARAMETER("has-context", "xs:boolean"),
    SXML_REQUIRED_PARAMETER("namespace-context", "item()*"),
    SXML_REQUIRED_PARAMETER("has-namespace-context", "xs:boolean"),
    SXML_REQUIRED_PARAMETER("base-uri", "xs:string?"),
    SXML_REQUIRED_PARAMETER("schema-aware", "xs:string?"),
    SXML_REQUIRED_PARAMETER("trusted", "xs:string?"),
    SXML_REQUIRED_PARAMETER("functions", "map(*)"),
};

const sxml_native_function sxml_xslt_evaluate_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = (uint32_t)SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                  (uint32_t)SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC |
                  (uint32_t)SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("evaluate"),
    .parameters = {evaluate_parameters,
                   sizeof evaluate_parameters / sizeof evaluate_parameters[0]},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = NULL,
    .resumable_body = &evaluate_body};
