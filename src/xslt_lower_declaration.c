/*
 * Lowering XSLT 4.0 declarations (src/xslt_lower_internal.h; packet 4.12,
 * docs/DECISIONS.md D150): stylesheet functions (xsl:function, 10.3) and
 * attribute sets (xsl:attribute-set, 10.2), the named components a
 * stylesheet calls from anywhere.
 *
 * Golden files: src/xquery_prolog.c (declared functions as IR functions,
 * one per arity, the shorter arities binding the defaults and calling the
 * full one; D135) and src/xslt_lower_template.c (bodies lowered by a
 * procedure run outside a machine, D144).
 *
 * Stylesheet functions. sxml_xslt_declare_components reads every
 * xsl:function, decides which ones are components (10.3.4: not eclipsed,
 * not given up for an extension function), adds one IR function per arity
 * of each one's arity range, and puts a function table in front of the
 * table of every XPath static context of the stylesheet, so every
 * expression, pattern and value template sees them; nothing is lowered yet,
 * so recursion and mutual recursion are STATIC_CALLs (D75).
 * sxml_xslt_lower_components then lowers the bodies: the full arity binds
 * the parameters (slots 1 onwards) and lowers the sequence constructor with
 * an absent focus (10.3.3), its value coerced to the as type (XTTE0780);
 * a shorter arity computes the defaults of the parameters left out (9.2.2,
 * 10.3.2: as global variables are, so they see no parameter) and calls the
 * full arity. A default that is "." is the caller's context value, which
 * the core supplies at the call (as XQuery's ":= .").
 *
 * Attribute sets. The declarations of one name (10.2.1: by import
 * precedence, then declaration order) are one IR function of the focus
 * (item()?, position, size in slots 1 to 3, as a template function's) that
 * returns the attribute set invocations of each declaration's
 * use-attribute-sets followed by its xsl:attribute children; a second one
 * is lowered with an absent focus for callers without a focus. An
 * [xsl:]use-attribute-sets attribute is a STATIC_CALL of each named set
 * with the caller's focus (10.2.3: the focus of the element's sequence
 * constructor). Cycles are XTSE0720, unknown names XTSE0710.
 *
 * No step recurses (D71): cycles are found with an explicit stack.
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_lower_internal.h"

#include "format_internal.h"
#include "sxml/name.h"
#include "xslt_whitespace_internal.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sort_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* The focus slots of an attribute set's function (as a template's). */
static const sxml_ir_slot item_slot = {.value = 1};
static const sxml_ir_slot position_slot = {.value = 2};
static const sxml_ir_slot size_slot = {.value = 3};
#define ATTRIBUTE_SET_PARAMETER_COUNT 3U

/* One xsl:function. */
typedef struct declared_function {
    sxml_xslt_ast_node_index node;
    sxml_name_identifier function_name;
    uint32_t import_precedence;
    /* The arity range is required_count to parameter_count (10.3.1). */
    uint32_t required_count;
    uint32_t parameter_count;
    /* A component (10.3.4): neither eclipsed nor given up for an
       extension function of the same name. Only these have IR functions. */
    bool is_component;
    /* The IR function of arity required_count; one more per arity. */
    sxml_ir_function_index first_function;
    /* Its parameters in the parameter arrays. */
    size_t first_parameter;
    /* Its signatures, one per arity, in the signature array. */
    size_t first_signature;
    sxml_sequence_type result_type;
    bool has_result_type;
    /* cache="yes" without new-each-time="yes" (10.3.7, memoization): the
       full arity calls the cached-call row, which calls cached_body (the
       IR function of the sequence constructor) once per argument values
       (D163). */
    bool is_cached;
    sxml_ir_function_index cached_body;
} declared_function;

/* One attribute set: every xsl:attribute-set of one name in one package
   (3.4.5: sets of different packages never merge). */
typedef struct attribute_set {
    sxml_name_identifier set_name;
    uint32_t package;
    sxml_ir_function_index function;
    sxml_ir_function_index absent_function;
    /* Its declarations in declaration_nodes, in evaluation order. */
    size_t first_declaration;
    size_t declaration_count;
} attribute_set;

/* The function table of one static context: the stylesheet's functions in
   front of the table it had. */
typedef struct context_table {
    const sxml_xslt_component_lowering *components;
    sxml_function_table inner_table;
    /* The AST static context: its package binds the calls (3.4.5.5),
       its overriding declaration xsl:original (3.4.5.4). */
    sxml_xslt_ast_static_context_index static_context;
} context_table;

struct sxml_xslt_component_lowering {
    sxml_name_pool *name_pool;
    /* For binding references through the packages (packet 4.16). */
    const sxml_xslt_lowering *lowering;
    /* xsl:original (3.4.5.4); 0 when never interned. */
    sxml_name_identifier original_name;
    /* Sorted by name, then lowest arity. */
    declared_function *functions;
    size_t function_count;
    sxml_function_parameter *function_parameters;
    sxml_sequence_type *parameter_types;
    sxml_xslt_ast_node_index *parameter_nodes;
    size_t parameter_count;
    sxml_type_identifier *signatures;
    size_t signature_count;
    /* Sorted by name. */
    attribute_set *attribute_sets;
    size_t attribute_set_count;
    sxml_xslt_ast_node_index *declaration_nodes;
    size_t declaration_count;
    context_table *context_tables;
    uint32_t context_table_count;
    /* A stylesheet function in no namespace: unprefixed names are looked
       up there first, then in fn (10.3.1). */
    bool has_no_namespace_function;
    sxml_type_identifier attribute_set_signature;
};

/* ------------------------------------------------------------ AST reads */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

static const sxml_xslt_ast_attribute *find_attribute(const sxml_xslt_lowering *lowering,
                                                     sxml_xslt_ast_node_index node,
                                                     sxml_xslt_ast_attribute_kind kind) {
    return sxml_xslt_ast_get_attribute(
        lowering->ast,
        sxml_xslt_ast_find_attribute(lowering->ast,
                                     (sxml_xslt_ast_attribute_query){.node = node, .kind = kind}));
}

static bool has_attribute(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                          sxml_xslt_ast_attribute_kind kind) {
    return find_attribute(lowering, node, kind)->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
}

/* A BOOLEAN attribute's value, or fallback when absent. */
typedef struct boolean_query {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_attribute_kind kind;
    bool fallback;
} boolean_query;

static bool boolean_of(const sxml_xslt_lowering *lowering, boolean_query query) {
    const sxml_xslt_ast_attribute *attribute = find_attribute(lowering, query.node, query.kind);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_BOOLEAN) {
        return query.fallback;
    }
    return attribute->value.payload.is_true;
}

static sxml_name_identifier name_of(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *attribute =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_NAME);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME) {
        return (sxml_name_identifier){.index = 0};
    }
    return attribute->value.payload.name;
}

static uint32_t precedence_of(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_module(lowering->ast, node_of(lowering, node)->module)
        ->import_precedence;
}

static sxml_status report_static_error(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_ast_node_index node,
                                       sxml_byte_view code_and_description) {
    return sxml_xslt_lowering_report_static_error(
        lowering,
        (sxml_xslt_static_error){.node = node, .code_and_description = code_and_description});
}

/* True for a name in a reserved namespace (3.5.1): XSLT, XML, xmlns, XML
   Schema, schema instance, fn, map, array, math, err. */
static bool is_reserved_name(const sxml_xslt_lowering *lowering, sxml_name_identifier name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (name.index == 0 || sxml_name_pool_get_name_parts(lowering->lowerer->name_pool, name,
                                                         &parts) != SXML_STATUS_OK) {
        return false;
    }
    const uint32_t uri = parts.namespace_uri.index;
    return uri != (uint32_t)SXML_NAMESPACE_NONE &&
           (uri == (uint32_t)SXML_NAMESPACE_XSLT || uri <= (uint32_t)SXML_NAMESPACE_ERRORS);
}

static bool is_in_no_namespace(const sxml_name_pool *name_pool, sxml_name_identifier name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    return sxml_name_pool_get_name_parts(name_pool, name, &parts) == SXML_STATUS_OK &&
           parts.namespace_uri.index == (uint32_t)SXML_NAMESPACE_NONE;
}

static sxml_ir_expression atom_expression(sxml_ir_atom atom) {
    return (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = atom};
}

static sxml_status bind_expression(sxml_xpath_lowerer *lowerer, sxml_xslt_expression_to_bind binding,
                                   sxml_ir_atom *atom) {
    return sxml_xslt_lowering_bind_expression(lowerer, &binding, atom);
}

/* The atoms list as one SEQUENCE expression. */
static sxml_status add_sequence(sxml_xpath_lowerer *lowerer, sxml_ir_atom_view atoms,
                                sxml_ir_expression_index *expression) {
    if (atoms.atom_count == 1) {
        return sxml_xslt_lowering_add_expression(lowerer, atom_expression(atoms.atoms[0]), expression);
    }
    sxml_ir_atom_list list = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(lowerer->unit, atoms, &list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE, .sequence = list},
        expression);
}

/* A STATIC_CALL of an IR function of the unit. */
typedef struct ir_call {
    sxml_ir_function_index function;
    sxml_ir_atom_view arguments;
} ir_call;

static sxml_status add_ir_call(sxml_xpath_lowerer *lowerer, ir_call call,
                               sxml_ir_expression_index *expression) {
    sxml_ir_atom_list list = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(lowerer->unit, call.arguments, &list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
            .static_call = {.target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                       .function = call.function},
                            .arguments = list}},
        expression);
}

/* Allocates count zeroed elements as lowering scratch (NULL for 0). */
typedef struct array_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} array_request;

static sxml_status allocate_array(const sxml_xslt_lowering *lowering, array_request request,
                                  void **memory) {
    *memory = NULL;
    if (request.element_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        lowering->lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = request.element_count,
                                              .element_size = request.element_size,
                                              .element_alignment = request.element_alignment},
        memory);
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, request.element_count * request.element_size);
    }
    return status;
}

/* ------------------------------------------------------------- Sorting */

/* An order over indices 0 to count - 1. */
typedef struct index_order {
    const void *state;
    bool (*comes_before)(const void *state, size_t first, size_t second);
} index_order;

typedef struct index_heap {
    index_order order;
    size_t *indices;
    size_t index_count;
} index_heap;

static bool index_is_before(const void *state, sxml_sort_pair pair) {
    const index_order *order = state;
    return order->comes_before(order->state, *(const size_t *)pair.first,
                               *(const size_t *)pair.second);
}

/* Sorts the indices (src/sort.c: a heapsort, no recursion). */
static void sort_indices(index_heap heap) {
    sxml_sort_in_place(&(sxml_sort_request){.elements = heap.indices,
                                            .element_count = heap.index_count,
                                            .element_size = sizeof(size_t),
                                            .is_before = index_is_before,
                                            .state = &heap.order});
}

/* ------------------------------------------------------- Function table */

/* The function a call of query from static context binds to (3.4.5.5):
   the package's component of that name and arity, or the one its
   overriding declaration overrides for xsl:original (3.4.5.4); NULL for
   none. */
static const declared_function *find_bound(const context_table *table,
                                           sxml_function_query query) {
    const sxml_xslt_component_lowering *components = table->components;
    const sxml_xslt_lowering *lowering = components->lowering;
    sxml_xslt_component_binding binding = {.declaration = {.value = 0}};
    if (query.function_name.index != 0 &&
        query.function_name.index == components->original_name.index) {
        const sxml_xslt_ast_node_index overriding =
            sxml_xslt_package_overriding_declaration_of(lowering, table->static_context);
        if (sxml_xslt_ast_get_node(lowering->ast, overriding)->kind !=
            SXML_XSLT_AST_KIND_FUNCTION) {
            return NULL;
        }
        binding = sxml_xslt_package_bind_original(lowering, overriding);
    } else {
        binding = sxml_xslt_package_bind(
            lowering, &(sxml_xslt_component_reference){
                          .component_kind = SXML_XSLT_COMPONENT_KIND_FUNCTION,
                          .component_name = query.function_name,
                          .arity = (uint32_t)query.arity,
                          .package = sxml_xslt_package_of_static_context(lowering,
                                                                         table->static_context)});
    }
    if (binding.declaration.value == 0) {
        return NULL;
    }
    for (size_t function_index = 0; function_index < components->function_count;
         function_index += 1) {
        const declared_function *function = &components->functions[function_index];
        if (function->node.value == binding.declaration.value && function->is_component &&
            query.arity >= function->required_count && query.arity <= function->parameter_count) {
            return function;
        }
    }
    return NULL;
}

/* The parameter types and parameters of function (NULL when the
   stylesheet's functions have no parameters at all). */
static const sxml_sequence_type *parameter_types_of(const sxml_xslt_component_lowering *components,
                                                    const declared_function *function) {
    return components->parameter_types == NULL
               ? NULL
               : &components->parameter_types[function->first_parameter];
}

static const sxml_function_parameter *
parameters_of(const sxml_xslt_component_lowering *components, const declared_function *function) {
    return components->function_parameters == NULL
               ? NULL
               : &components->function_parameters[function->first_parameter];
}

static sxml_function_entry entry_of(const sxml_xslt_component_lowering *components,
                                    const declared_function *function, size_t arity) {
    const size_t arity_offset = arity - function->required_count;
    return (sxml_function_entry){
        .entry_kind = SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION,
        .native_function = NULL,
        .ir_function_number = function->first_function.value + (uint32_t)arity_offset,
        .parameters = {.function_parameters = parameters_of(components, function),
                       .function_parameter_count = function->parameter_count},
        .signature = components->signatures[function->first_signature + arity_offset],
        .parameter_types = {.sequence_types = parameter_types_of(components, function),
                            .sequence_type_count = arity},
        .result_type = function->result_type};
}

/*
 * The stylesheet's functions, then the table the static context had. With
 * a function in no namespace the core expands unprefixed names to no
 * namespace (SXML_XPATH_FUNCTION_NAMESPACE_RULE_NO_NAMESPACE_FIRST), so a
 * no-namespace name no stylesheet function has is looked up in fn
 * (10.3.1).
 */
static sxml_status find_function(void *state, const sxml_function_query *query,
                                 sxml_function_entry *entry) {
    const context_table *table = state;
    const sxml_xslt_component_lowering *components = table->components;
    const declared_function *function = find_bound(table, *query);
    if (function != NULL) {
        *entry = entry_of(components, function, query->arity);
        return SXML_STATUS_OK;
    }
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (components->has_no_namespace_function &&
        sxml_name_pool_get_name_parts(components->name_pool, query->function_name, &parts) ==
            SXML_STATUS_OK &&
        parts.namespace_uri.index == (uint32_t)SXML_NAMESPACE_NONE) {
        sxml_function_query standard = {.function_name = {.index = 0}, .arity = query->arity};
        if (!sxml_name_pool_find_name(
                components->name_pool,
                &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                         SXML_NAMESPACE_FUNCTIONS),
                                     .local_name = parts.local_name},
                &standard.function_name)) {
            *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
            return SXML_STATUS_OK;
        }
        return sxml_xpath_find_function(table->inner_table, &standard, entry);
    }
    return sxml_xpath_find_function(table->inner_table, query, entry);
}

static const sxml_function_table_operations component_table_operations = {.find_function =
                                                                              find_function};

/* ------------------------------------------------- Reading xsl:function */

/* The parameters of an xsl:function (10.3.2): names, types, defaults;
   XTSE0080, XTSE0580, XTSE0760, XTSE0761. */
static sxml_status read_parameters(const sxml_xslt_lowering *lowering,
                                   sxml_xslt_component_lowering *components,
                                   declared_function *function) {
    sxml_status status = SXML_STATUS_OK;
    bool has_optional = false;
    size_t parameter_index = function->first_parameter;
    for (sxml_xslt_ast_node_index child = node_of(lowering, function->node)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_PARAM) {
            continue;
        }
        const sxml_name_identifier parameter_name = name_of(lowering, child);
        const bool is_required = boolean_of(
            lowering,
            (boolean_query){.node = child, .kind = SXML_XSLT_AST_ATTRIBUTE_REQUIRED, .fallback = true});
        const bool has_select = has_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_SELECT);
        const bool has_content = node_of(lowering, child)->first_child.value != 0;
        if (is_reserved_name(lowering, parameter_name)) {
            return report_static_error(
                lowering, child,
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE0080 a function parameter is named in a reserved "
                                            "namespace (XSLT 4.0, 3.5.1)"));
        }
        for (size_t earlier = function->first_parameter; earlier < parameter_index; earlier += 1) {
            if (name_of(lowering, components->parameter_nodes[earlier]).index ==
                parameter_name.index) {
                return report_static_error(
                    lowering, child,
                    SXML_BYTE_VIEW_FROM_LITERAL("XTSE0580 two parameters of a function have the "
                                                "same name (XSLT 4.0, 9.2)"));
            }
        }
        if (is_required && (has_select || has_content)) {
            return report_static_error(
                lowering, child,
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE0760 a required function parameter has a "
                                            "default value (XSLT 4.0, 10.3.2)"));
        }
        if (is_required && has_optional) {
            return report_static_error(
                lowering, child,
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE0761 a required function parameter follows an "
                                            "optional one (XSLT 4.0, 10.3.2)"));
        }
        has_optional = has_optional || !is_required;
        sxml_xslt_declared_type type = {.has_type = false};
        status = sxml_xslt_lowering_resolve_declared_type(lowering, child, &type);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        status = sxml_name_pool_get_name_parts(lowering->lowerer->name_pool, parameter_name, &parts);
        sxml_parameter_default parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED;
        if (!is_required) {
            parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY;
            const sxml_xslt_ast_attribute *select =
                find_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_SELECT);
            if (select->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
                /* 10.3.2: the default sees the caller's focus; "." is the
                   caller's context value, which the core supplies. */
                const sxml_ast *ast =
                    sxml_xslt_ast_get_expression(lowering->ast, select->value.payload.expression)
                        ->ast;
                const sxml_ast_node_index body =
                    ast == NULL ? no_ast_node
                                : sxml_ast_find_child(ast, (sxml_ast_child_query){
                                                               .parent = ast->root,
                                                               .field = SXML_AST_FIELD_BODY});
                if (body.value != 0 && sxml_ast_get_node(ast, body)->kind ==
                                           SXML_AST_KIND_CONTEXT_VALUE_REFERENCE) {
                    parameter_default = SXML_PARAMETER_DEFAULT_CONTEXT_VALUE;
                }
            }
        } else {
            function->required_count += 1;
        }
        components->parameter_nodes[parameter_index] = child;
        components->parameter_types[parameter_index] = type.sequence_type;
        components->function_parameters[parameter_index] = (sxml_function_parameter){
            .parameter_name = parts.local_name,
            .parameter_type = {.bytes = NULL, .byte_count = 0},
            .parameter_default = parameter_default};
        parameter_index += 1;
    }
    function->parameter_count = (uint32_t)(parameter_index - function->first_parameter);
    return status;
}

static size_t count_parameters(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    size_t count = 0;
    for (sxml_xslt_ast_node_index child = node_of(lowering, node)->first_child; child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        count += node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_PARAM ? 1U : 0U;
    }
    return count;
}

static bool function_comes_before(const void *state, size_t first, size_t second) {
    const declared_function *functions = state;
    if (functions[first].function_name.index != functions[second].function_name.index) {
        return functions[first].function_name.index < functions[second].function_name.index;
    }
    if (functions[first].import_precedence != functions[second].import_precedence) {
        return functions[first].import_precedence > functions[second].import_precedence;
    }
    return functions[first].node.value < functions[second].node.value;
}

/* Sorts the functions by name, highest precedence first, then
   declaration. */
static sxml_status sort_functions(const sxml_xslt_lowering *lowering,
                                  sxml_xslt_component_lowering *components) {
    const size_t count = components->function_count;
    void *memory = NULL;
    sxml_status status = allocate_array(
        lowering,
        (array_request){.element_count = count * 2,
                        .element_size = sizeof(declared_function),
                        .element_alignment = alignof(declared_function)},
        &memory);
    void *index_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(lowering,
                                (array_request){.element_count = count,
                                                .element_size = sizeof(size_t),
                                                .element_alignment = alignof(size_t)},
                                &index_memory);
    }
    if (status != SXML_STATUS_OK || count == 0 || memory == NULL || index_memory == NULL ||
        components->functions == NULL) {
        sxml_xpath_lowerer_release_scratch(lowering->lowerer, index_memory);
        sxml_xpath_lowerer_release_scratch(lowering->lowerer, memory);
        return status;
    }
    declared_function *sorted = memory;
    size_t *indices = index_memory;
    for (size_t index = 0; index < count; index += 1) {
        indices[index] = index;
    }
    sort_indices((index_heap){
        .order = {.state = components->functions, .comes_before = function_comes_before},
        .indices = indices,
        .index_count = count});
    for (size_t index = 0; index < count; index += 1) {
        sorted[index] = components->functions[indices[index]];
    }
    memcpy(components->functions, sorted, count * sizeof(declared_function));
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, index_memory);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, memory);
    return SXML_STATUS_OK;
}

static bool ranges_overlap(const declared_function *first, const declared_function *second) {
    return first->required_count <= second->parameter_count &&
           second->required_count <= first->parameter_count;
}

static bool range_contains(const declared_function *outer, const declared_function *inner) {
    return outer->required_count <= inner->required_count &&
           inner->parameter_count <= outer->parameter_count;
}

/* 10.3.4.1: a function is eclipsed by one of the same name, higher
   precedence and an arity range holding all of its own; XTSE0769 for one
   that holds part of it, XTSE0770 for overlapping ranges of one
   precedence (unless eclipsed). Functions of one name are adjacent,
   highest precedence first. */
static sxml_status choose_components(const sxml_xslt_lowering *lowering,
                                     sxml_xslt_component_lowering *components) {
    declared_function *functions = components->functions;
    if (functions == NULL) {
        return SXML_STATUS_OK;
    }
    for (size_t group = 0; group < components->function_count;) {
        size_t group_end = group + 1;
        while (group_end < components->function_count &&
               functions[group_end].function_name.index == functions[group].function_name.index) {
            group_end += 1;
        }
        for (size_t index = group; index < group_end; index += 1) {
            functions[index].is_component = true;
            for (size_t other = group; other < group_end; other += 1) {
                /* Packages do not eclipse each other's functions (3.4.5). */
                if (sxml_xslt_package_is_same(lowering, functions[other].node,
                                              functions[index].node) &&
                    functions[other].import_precedence > functions[index].import_precedence &&
                    range_contains(&functions[other], &functions[index])) {
                    functions[index].is_component = false;
                }
            }
        }
        for (size_t index = group; index < group_end; index += 1) {
            for (size_t other = group; other < group_end; other += 1) {
                const declared_function *first = &functions[index];
                const declared_function *second = &functions[other];
                if (index == other || !first->is_component || !second->is_component ||
                    !ranges_overlap(first, second) ||
                    !sxml_xslt_package_is_same(lowering, first->node, second->node)) {
                    continue;
                }
                if (first->import_precedence == second->import_precedence) {
                    return report_static_error(
                        lowering, second->node,
                        SXML_BYTE_VIEW_FROM_LITERAL(
                            "XTSE0770 two functions of one name and import precedence have "
                            "overlapping arity ranges (XSLT 4.0, 10.3.4.1)"));
                }
                if (first->import_precedence > second->import_precedence) {
                    return report_static_error(
                        lowering, first->node,
                        SXML_BYTE_VIEW_FROM_LITERAL(
                            "XTSE0769 a function overrides part, not all, of the arity range of a "
                            "function of lower import precedence (XSLT 4.0, 10.3.4.1)"));
                }
            }
        }
        group = group_end;
    }
    return SXML_STATUS_OK;
}

/* 10.3.4.2: override-extension-function="no" (or override="no") gives the
   function up when the processor has one of that name in its arity
   range. */
static sxml_status yield_to_extension_functions(const sxml_xslt_lowering *lowering,
                                                sxml_xslt_component_lowering *components,
                                                sxml_function_table inner_table) {
    if (components->functions == NULL) {
        return SXML_STATUS_OK;
    }
    for (size_t index = 0; index < components->function_count; index += 1) {
        declared_function *function = &components->functions[index];
        const bool overrides =
            boolean_of(lowering,
                       (boolean_query){.node = function->node,
                                       .kind = SXML_XSLT_AST_ATTRIBUTE_OVERRIDE_EXTENSION_FUNCTION,
                                       .fallback = true}) &&
            boolean_of(lowering, (boolean_query){.node = function->node,
                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_OVERRIDE,
                                                 .fallback = true});
        for (uint32_t arity = function->required_count;
             !overrides && function->is_component && arity <= function->parameter_count;
             arity += 1) {
            sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
            const sxml_status status = sxml_xpath_find_function(
                inner_table,
                &(sxml_function_query){.function_name = function->function_name, .arity = arity},
                &entry);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            function->is_component = entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE;
        }
    }
    return SXML_STATUS_OK;
}

/* Reads every xsl:function into components->functions. */
static sxml_status read_functions(const sxml_xslt_lowering *lowering,
                                  sxml_xslt_component_lowering *components) {
    const sxml_xslt_ast *ast = lowering->ast;
    size_t function_count = 0;
    size_t parameter_count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, declaration)->kind == SXML_XSLT_AST_KIND_FUNCTION) {
            function_count += 1;
            parameter_count += count_parameters(lowering, declaration);
        }
    }
    void *memory = NULL;
    sxml_status status = allocate_array(
        lowering,
        (array_request){.element_count = function_count,
                        .element_size = sizeof(declared_function),
                        .element_alignment = alignof(declared_function)},
        &memory);
    components->functions = memory;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(lowering,
                                (array_request){.element_count = parameter_count,
                                                .element_size = sizeof(sxml_function_parameter),
                                                .element_alignment =
                                                    alignof(sxml_function_parameter)},
                                &memory);
        components->function_parameters = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_array(lowering,
                                (array_request){.element_count = parameter_count,
                                                .element_size = sizeof(sxml_sequence_type),
                                                .element_alignment = alignof(sxml_sequence_type)},
                                &memory);
        components->parameter_types = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_array(
            lowering,
            (array_request){.element_count = parameter_count,
                            .element_size = sizeof(sxml_xslt_ast_node_index),
                            .element_alignment = alignof(sxml_xslt_ast_node_index)},
            &memory);
        components->parameter_nodes = memory;
    }
    components->parameter_count = parameter_count;
    size_t next_parameter = 0;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, declaration)->kind != SXML_XSLT_AST_KIND_FUNCTION) {
            continue;
        }
        declared_function *function = &components->functions[components->function_count];
        *function = (declared_function){.node = declaration,
                                        .function_name = name_of(lowering, declaration),
                                        .import_precedence = precedence_of(lowering, declaration),
                                        .first_parameter = next_parameter};
        components->function_count += 1;
        if (is_reserved_name(lowering, function->function_name)) {
            return report_static_error(
                lowering, declaration,
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE0080 a stylesheet function is named in a "
                                            "reserved namespace (XSLT 4.0, 3.5.1)"));
        }
        const bool is_in_no_function_namespace =
            is_in_no_namespace(lowering->lowerer->name_pool, function->function_name);
        const sxml_xslt_ast_attribute *visibility =
            find_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_VISIBILITY);
        if (is_in_no_function_namespace &&
            visibility->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD &&
            visibility->value.payload.keyword != SXML_XSLT_AST_KEYWORD_PRIVATE) {
            return report_static_error(
                lowering, declaration,
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE0740 a stylesheet function in no namespace is "
                                            "not private (XSLT 4.0, 10.3.1)"));
        }
        const sxml_xslt_ast_attribute *overrides = find_attribute(
            lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_OVERRIDE_EXTENSION_FUNCTION);
        const sxml_xslt_ast_attribute *deprecated =
            find_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_OVERRIDE);
        if (overrides->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
            deprecated->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
            overrides->value.payload.is_true != deprecated->value.payload.is_true) {
            return report_static_error(
                lowering, declaration,
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE0020 override and override-extension-function "
                                            "have different values (XSLT 4.0, 10.3.4.2)"));
        }
        components->has_no_namespace_function =
            components->has_no_namespace_function || is_in_no_function_namespace;
        status = read_parameters(lowering, components, function);
        next_parameter += function->parameter_count;
        sxml_xslt_declared_type type = {.has_type = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_resolve_declared_type(lowering, declaration, &type);
        }
        function->result_type = type.sequence_type;
        function->has_result_type = type.has_type;
        const sxml_xslt_ast_attribute *cache =
            find_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_CACHE);
        const sxml_xslt_ast_attribute *new_each_time =
            find_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_NEW_EACH_TIME);
        /* 10.3.7: a proactive function's results may not be reused (its
           nodes must be new each time), so its cache request is not
           honoured. */
        function->is_cached = cache->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
                              cache->value.payload.is_true &&
                              !(new_each_time->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD &&
                                new_each_time->value.payload.keyword == SXML_XSLT_AST_KEYWORD_YES);
    }
    return status;
}

/* One signature and IR function per arity of every component. */
static sxml_status add_function_shells(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_component_lowering *components) {
    if (components->functions == NULL) {
        return SXML_STATUS_OK;
    }
    size_t signature_count = 0;
    for (size_t index = 0; index < components->function_count; index += 1) {
        const declared_function *function = &components->functions[index];
        signature_count += function->parameter_count - function->required_count + 1U;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(
        lowering,
        (array_request){.element_count = signature_count,
                        .element_size = sizeof(sxml_type_identifier),
                        .element_alignment = alignof(sxml_type_identifier)},
        &memory);
    components->signatures = memory;
    components->signature_count = signature_count;
    size_t next_signature = 0;
    for (size_t index = 0; status == SXML_STATUS_OK && index < components->function_count;
         index += 1) {
        declared_function *function = &components->functions[index];
        function->first_signature = next_signature;
        for (uint32_t arity = function->required_count;
             status == SXML_STATUS_OK && arity <= function->parameter_count; arity += 1) {
            sxml_type_identifier *signature = &components->signatures[next_signature];
            next_signature += 1;
            status = sxml_xpath_lowerer_register_signature(
                lowering->lowerer,
                (sxml_xpath_lowering_signature_request){
                    .parameter_types = {.sequence_types = parameter_types_of(components, function),
                                        .sequence_type_count = arity},
                    .result_type = function->result_type},
                signature);
            if (status != SXML_STATUS_OK || !function->is_component) {
                continue;
            }
            sxml_ir_function_index function_index = {0};
            /* Its module: a dynamic error in it names its stylesheet
               module (D169). */
            const uint32_t module_number = sxml_xslt_lowering_module_of_node(lowering, function->node);
            status = sxml_ir_unit_add_function(
                lowering->lowerer->unit,
                &(sxml_ir_function){.function_name = function->function_name,
                                    .signature = *signature,
                                    .parameter_count = arity,
                                    .module_number = module_number},
                &function_index);
            if (arity == function->required_count) {
                function->first_function = function_index;
            }
            if (status == SXML_STATUS_OK && function->is_cached &&
                arity == function->parameter_count) {
                status = sxml_ir_unit_add_function(
                    lowering->lowerer->unit,
                    &(sxml_ir_function){.function_name = function->function_name,
                                        .signature = *signature,
                                        .parameter_count = arity,
                                        .module_number = module_number},
                    &function->cached_body);
            }
        }
    }
    return status;
}

/* ---------------------------------------------- Reading attribute sets */

typedef struct attribute_set_sort {
    const sxml_xslt_lowering *lowering;
    const sxml_xslt_ast_node_index *nodes;
} attribute_set_sort;

/* By name, then import precedence (lowest first), then declaration
   order (10.2.1). */
static bool declaration_comes_before(const void *state, size_t first, size_t second) {
    const attribute_set_sort *sort = state;
    const sxml_name_identifier first_name = name_of(sort->lowering, sort->nodes[first]);
    const sxml_name_identifier second_name = name_of(sort->lowering, sort->nodes[second]);
    if (first_name.index != second_name.index) {
        return first_name.index < second_name.index;
    }
    const uint32_t first_package = sxml_xslt_package_of_node(sort->lowering, sort->nodes[first]);
    const uint32_t second_package = sxml_xslt_package_of_node(sort->lowering, sort->nodes[second]);
    if (first_package != second_package) {
        return first_package < second_package;
    }
    const uint32_t first_precedence = precedence_of(sort->lowering, sort->nodes[first]);
    const uint32_t second_precedence = precedence_of(sort->lowering, sort->nodes[second]);
    if (first_precedence != second_precedence) {
        return first_precedence < second_precedence;
    }
    return first < second;
}

/* The attribute set of name declared in package. */
typedef struct attribute_set_key {
    sxml_name_identifier set_name;
    uint32_t package;
} attribute_set_key;

static const attribute_set *find_attribute_set(const sxml_xslt_component_lowering *components,
                                               attribute_set_key key) {
    const sxml_name_identifier name = key.set_name;
    size_t low = 0;
    size_t high = components->attribute_set_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (components->attribute_sets[middle].set_name.index < name.index) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    for (size_t index = low; index < components->attribute_set_count &&
                             components->attribute_sets[index].set_name.index == name.index;
         index += 1) {
        if (components->attribute_sets[index].package == key.package) {
            return &components->attribute_sets[index];
        }
    }
    return NULL;
}

/* The attribute set an [xsl:]use-attribute-sets token on node binds to
   (3.4.5.5): its package's set of that name, through the packages, or
   for xsl:original in an overriding attribute set the overridden one
   (3.4.5.4); NULL for none. */
typedef struct attribute_set_reference {
    sxml_xslt_ast_node_index node;
    sxml_name_identifier set_name;
} attribute_set_reference;

static const attribute_set *resolve_attribute_set(const sxml_xslt_lowering *lowering,
                                                  attribute_set_reference reference) {
    const sxml_xslt_component_lowering *components = lowering->components;
    if (components == NULL) {
        return NULL;
    }
    sxml_xslt_component_binding binding = {.declaration = {.value = 0}};
    if (reference.set_name.index != 0 &&
        reference.set_name.index == components->original_name.index) {
        const sxml_xslt_ast_node_index overriding =
            sxml_xslt_package_overriding_ancestor_of(lowering, reference.node);
        if (node_of(lowering, overriding)->kind != SXML_XSLT_AST_KIND_ATTRIBUTE_SET) {
            return NULL;
        }
        binding = sxml_xslt_package_bind_original(lowering, overriding);
    } else {
        binding = sxml_xslt_package_bind(
            lowering, &(sxml_xslt_component_reference){
                          .component_kind = SXML_XSLT_COMPONENT_KIND_ATTRIBUTE_SET,
                          .component_name = reference.set_name,
                          .arity = 0,
                          .package = sxml_xslt_package_of_node(lowering, reference.node)});
    }
    if (binding.declaration.value == 0) {
        return NULL;
    }
    return find_attribute_set(components,
                              (attribute_set_key){.set_name = name_of(lowering, binding.declaration),
                                                  .package = binding.package});
}

/* The use-attribute-sets tokens of node; empty without the attribute. */
static sxml_xslt_ast_name_token_view used_sets_of(const sxml_xslt_lowering *lowering,
                                                  sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *attribute =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        return (sxml_xslt_ast_name_token_view){.name_tokens = NULL, .name_token_count = 0};
    }
    return sxml_xslt_ast_get_name_tokens(lowering->ast, attribute->value.payload.name_tokens);
}

static sxml_status report_unknown_set(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_node_index node) {
    return report_static_error(
        lowering, node,
        SXML_BYTE_VIEW_FROM_LITERAL("XTSE0710 use-attribute-sets names no attribute set of the "
                                    "stylesheet (XSLT 4.0, 10.2.1)"));
}

/* Collects the xsl:attribute-set declarations by name. */
static sxml_status read_attribute_sets(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_component_lowering *components) {
    const sxml_xslt_ast *ast = lowering->ast;
    size_t count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        count += node_of(lowering, sxml_xslt_ast_get_declaration(ast, order))->kind ==
                         SXML_XSLT_AST_KIND_ATTRIBUTE_SET
                     ? 1U
                     : 0U;
    }
    if (count == 0) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(
        lowering,
        (array_request){.element_count = count,
                        .element_size = sizeof(sxml_xslt_ast_node_index),
                        .element_alignment = alignof(sxml_xslt_ast_node_index)},
        &memory);
    components->declaration_nodes = memory;
    void *index_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(lowering,
                                (array_request){.element_count = count,
                                                .element_size = sizeof(size_t),
                                                .element_alignment = alignof(size_t)},
                                &index_memory);
    }
    void *sorted_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(
            lowering,
            (array_request){.element_count = count,
                            .element_size = sizeof(sxml_xslt_ast_node_index),
                            .element_alignment = alignof(sxml_xslt_ast_node_index)},
            &sorted_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_array(lowering,
                                (array_request){.element_count = count,
                                                .element_size = sizeof(attribute_set),
                                                .element_alignment = alignof(attribute_set)},
                                &memory);
        components->attribute_sets = memory;
    }
    if (status != SXML_STATUS_OK) {
        sxml_xpath_lowerer_release_scratch(lowering->lowerer, index_memory);
        sxml_xpath_lowerer_release_scratch(lowering->lowerer, sorted_memory);
        return status;
    }
    size_t filled = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, declaration)->kind == SXML_XSLT_AST_KIND_ATTRIBUTE_SET) {
            components->declaration_nodes[filled] = declaration;
            filled += 1;
        }
    }
    size_t *indices = index_memory;
    sxml_xslt_ast_node_index *sorted = sorted_memory;
    for (size_t index = 0; index < count; index += 1) {
        indices[index] = index;
    }
    const attribute_set_sort sort = {.lowering = lowering, .nodes = components->declaration_nodes};
    sort_indices((index_heap){.order = {.state = &sort, .comes_before = declaration_comes_before},
                              .indices = indices,
                              .index_count = count});
    for (size_t index = 0; index < count; index += 1) {
        sorted[index] = components->declaration_nodes[indices[index]];
    }
    memcpy(components->declaration_nodes, sorted, count * sizeof(sxml_xslt_ast_node_index));
    components->declaration_count = count;
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, index_memory);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, sorted_memory);
    for (size_t index = 0; index < count; index += 1) {
        const sxml_name_identifier set_name = name_of(lowering, components->declaration_nodes[index]);
        const uint32_t package =
            sxml_xslt_package_of_node(lowering, components->declaration_nodes[index]);
        if (is_reserved_name(lowering, set_name)) {
            return report_static_error(
                lowering, components->declaration_nodes[index],
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE0080 an attribute set is named in a reserved "
                                            "namespace (XSLT 4.0, 3.5.1)"));
        }
        if (components->attribute_set_count > 0 &&
            components->attribute_sets[components->attribute_set_count - 1].set_name.index ==
                set_name.index &&
            components->attribute_sets[components->attribute_set_count - 1].package == package) {
            components->attribute_sets[components->attribute_set_count - 1].declaration_count += 1;
            continue;
        }
        components->attribute_sets[components->attribute_set_count] =
            (attribute_set){.set_name = set_name,
                            .package = package,
                            .first_declaration = index,
                            .declaration_count = 1};
        components->attribute_set_count += 1;
    }
    return SXML_STATUS_OK;
}

/* One level of the search below: a set, its next declaration and the
   next token of that declaration's use-attribute-sets. */
typedef struct path_entry {
    size_t set_index;
    size_t declaration_offset;
    size_t token_index;
} path_entry;

/* 9.11: XTSE0720 for an attribute set that uses itself, directly or
   through others; XTSE0710 for a name no set has. An iterative depth
   first search with colours (0 unvisited, 1 on the path, 2 done). */
static sxml_status check_attribute_set_cycles(const sxml_xslt_lowering *lowering,
                                              const sxml_xslt_component_lowering *components) {
    const size_t count = components->attribute_set_count;
    if (count == 0) {
        return SXML_STATUS_OK;
    }
    void *colour_memory = NULL;
    sxml_status status = allocate_array(
        lowering,
        (array_request){.element_count = count, .element_size = 1, .element_alignment = 1},
        &colour_memory);
    void *path_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(lowering,
                                (array_request){.element_count = count,
                                                .element_size = sizeof(path_entry),
                                                .element_alignment = alignof(path_entry)},
                                &path_memory);
    }
    uint8_t *colours = colour_memory;
    path_entry *path = path_memory;
    for (size_t root_index = 0; status == SXML_STATUS_OK && root_index < count; root_index += 1) {
        if (colours[root_index] != 0) {
            continue;
        }
        size_t depth = 1;
        path[0] = (path_entry){.set_index = root_index, .declaration_offset = 0, .token_index = 0};
        colours[root_index] = 1;
        while (status == SXML_STATUS_OK && depth > 0) {
            path_entry *top = &path[depth - 1];
            const attribute_set *set = &components->attribute_sets[top->set_index];
            if (top->declaration_offset == set->declaration_count) {
                colours[top->set_index] = 2;
                depth -= 1;
                continue;
            }
            const sxml_xslt_ast_node_index declaration =
                components->declaration_nodes[set->first_declaration + top->declaration_offset];
            const sxml_xslt_ast_name_token_view tokens = used_sets_of(lowering, declaration);
            if (top->token_index >= tokens.name_token_count) {
                top->declaration_offset += 1;
                top->token_index = 0;
                continue;
            }
            const sxml_xslt_ast_name_token token = tokens.name_tokens[top->token_index];
            top->token_index += 1;
            const attribute_set *used = resolve_attribute_set(
                lowering, (attribute_set_reference){.node = declaration, .set_name = token.name});
            if (used == NULL) {
                status = report_unknown_set(lowering, declaration);
                break;
            }
            const size_t used_index = (size_t)(used - components->attribute_sets);
            if (colours[used_index] == 1) {
                status = report_static_error(
                    lowering, declaration,
                    SXML_BYTE_VIEW_FROM_LITERAL("XTSE0720 an attribute set uses itself, directly "
                                                "or through others (XSLT 4.0, 9.11)"));
                break;
            }
            if (colours[used_index] == 0) {
                colours[used_index] = 1;
                path[depth] =
                    (path_entry){.set_index = used_index, .declaration_offset = 0, .token_index = 0};
                depth += 1;
            }
        }
    }
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, path_memory);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, colour_memory);
    return status;
}

/* The signature of attribute sets, and their IR functions. */
static sxml_status add_attribute_set_shells(const sxml_xslt_lowering *lowering,
                                            sxml_xslt_component_lowering *components) {
    if (components->attribute_set_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_sequence_type parameter_types[ATTRIBUTE_SET_PARAMETER_COUNT] = {
        {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
         .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE},
        {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
         .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
        {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
         .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}};
    sxml_status status = sxml_xpath_lowerer_register_signature(
        lowering->lowerer,
        (sxml_xpath_lowering_signature_request){
            .parameter_types = {.sequence_types = parameter_types,
                                .sequence_type_count = ATTRIBUTE_SET_PARAMETER_COUNT},
            .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}},
        &components->attribute_set_signature);
    for (size_t index = 0; status == SXML_STATUS_OK && index < components->attribute_set_count;
         index += 1) {
        attribute_set *set = &components->attribute_sets[index];
        const sxml_ir_function shell = {
            .signature = components->attribute_set_signature,
            .parameter_count = ATTRIBUTE_SET_PARAMETER_COUNT,
            .module_number = sxml_xslt_lowering_module_of_node(
                lowering, components->declaration_nodes[set->first_declaration])};
        status = sxml_ir_unit_add_function(lowering->lowerer->unit, &shell, &set->function);
        if (status == SXML_STATUS_OK) {
            status =
                sxml_ir_unit_add_function(lowering->lowerer->unit, &shell, &set->absent_function);
        }
    }
    return status;
}

/* ------------------------------------------------------------- Declare */

/*
 * The core looks function names up without interning them (a name never
 * interned names no function, D135), so with a function in no namespace
 * the no-namespace names that unprefixed calls, references and arrows
 * expand to are interned first, letting the function table see them and
 * fall back to fn: one pass over the nodes of every expression.
 */
static sxml_status intern_unprefixed_calls(const sxml_xslt_lowering *lowering) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t expression_index = 1;
         status == SXML_STATUS_OK && expression_index < lowering->ast->expression_count;
         expression_index += 1) {
        const sxml_ast *ast =
            sxml_xslt_ast_get_expression(lowering->ast,
                                         (sxml_xslt_ast_expression_index){.value = expression_index})
                ->ast;
        for (uint32_t node_number = 1;
             ast != NULL && status == SXML_STATUS_OK && node_number < ast->node_count;
             node_number += 1) {
            const sxml_ast_node *node =
                sxml_ast_get_node(ast, (sxml_ast_node_index){.value = node_number});
            if (node->kind != SXML_AST_KIND_NCNAME ||
                (node->field != SXML_AST_FIELD_NAME && node->field != SXML_AST_FIELD_FUNCTION)) {
                continue;
            }
            const sxml_ast_kind parent_kind = sxml_ast_get_node(ast, node->parent)->kind;
            if (parent_kind != SXML_AST_KIND_FUNCTION_CALL &&
                parent_kind != SXML_AST_KIND_NAMED_FUNCTION_REFERENCE &&
                parent_kind != SXML_AST_KIND_ARROW_EXPRESSION) {
                continue;
            }
            sxml_name_identifier name = {0};
            status = sxml_name_pool_intern_name(
                lowering->lowerer->name_pool,
                &(sxml_name_request){
                    .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE),
                    .local_name = sxml_ast_get_text(ast, node->payload.name.local_name)},
                &name);
        }
    }
    return status;
}

sxml_status sxml_xslt_declare_components(sxml_xslt_lowering *lowering,
                                         sxml_xpath_static_context *static_contexts) {
    void *memory = NULL;
    sxml_status status = allocate_array(
        lowering,
        (array_request){.element_count = 1,
                        .element_size = sizeof(sxml_xslt_component_lowering),
                        .element_alignment = alignof(sxml_xslt_component_lowering)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_component_lowering *components = memory;
    components->name_pool = lowering->lowerer->name_pool;
    components->lowering = lowering;
    lowering->components = components;
    status = sxml_name_pool_intern_name(
        components->name_pool,
        &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                 SXML_NAMESPACE_XSLT),
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("original")},
        &components->original_name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_ast_node_index conflict = {.value = 0};
    if (sxml_xslt_check_space_declarations(lowering->ast, &conflict) != SXML_STATUS_OK) {
        return report_static_error(
            lowering, conflict,
            SXML_BYTE_VIEW_FROM_LITERAL("XTSE0270 one name test is in an xsl:strip-space and an "
                                        "xsl:preserve-space of one import precedence (XSLT 4.0, "
                                        "4.2.2)"));
    }
    const uint32_t context_count = lowering->static_context_count;
    if (context_count < 2) {
        return SXML_STATUS_OK;
    }
    const sxml_function_table inner_table = static_contexts[1].function_table;
    status = read_functions(lowering, components);
    if (status == SXML_STATUS_OK) {
        status = sort_functions(lowering, components);
    }
    if (status == SXML_STATUS_OK) {
        status = choose_components(lowering, components);
    }
    if (status == SXML_STATUS_OK) {
        status = yield_to_extension_functions(lowering, components, inner_table);
    }
    if (status == SXML_STATUS_OK) {
        status = add_function_shells(lowering, components);
    }
    if (status == SXML_STATUS_OK) {
        status = read_attribute_sets(lowering, components);
    }
    if (status == SXML_STATUS_OK) {
        status = check_attribute_set_cycles(lowering, components);
    }
    if (status == SXML_STATUS_OK) {
        status = add_attribute_set_shells(lowering, components);
    }
    if (status != SXML_STATUS_OK || components->function_count == 0) {
        return status;
    }
    status = allocate_array(lowering,
                            (array_request){.element_count = context_count,
                                            .element_size = sizeof(context_table),
                                            .element_alignment = alignof(context_table)},
                            &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    components->context_tables = memory;
    components->context_table_count = context_count;
    if (components->has_no_namespace_function) {
        status = intern_unprefixed_calls(lowering);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    for (uint32_t context = 1; context < context_count; context += 1) {
        components->context_tables[context] = (context_table){
            .components = components,
            .inner_table = static_contexts[context].function_table,
            .static_context = {.value = context}};
        static_contexts[context].function_table = (sxml_function_table){
            .state = &components->context_tables[context], .operations = &component_table_operations};
        if (components->has_no_namespace_function) {
            static_contexts[context].function_namespace_rule =
                SXML_XPATH_FUNCTION_NAMESPACE_RULE_NO_NAMESPACE_FIRST;
        }
    }
    return SXML_STATUS_OK;
}

void sxml_xslt_release_components(sxml_xslt_lowering *lowering) {
    sxml_xslt_component_lowering *components = lowering->components;
    if (components == NULL) {
        return;
    }
    const sxml_xpath_lowerer *lowerer = lowering->lowerer;
    sxml_xpath_lowerer_release_scratch(lowerer, components->functions);
    sxml_xpath_lowerer_release_scratch(lowerer, components->function_parameters);
    sxml_xpath_lowerer_release_scratch(lowerer, components->parameter_types);
    sxml_xpath_lowerer_release_scratch(lowerer, components->parameter_nodes);
    sxml_xpath_lowerer_release_scratch(lowerer, components->signatures);
    sxml_xpath_lowerer_release_scratch(lowerer, components->attribute_sets);
    sxml_xpath_lowerer_release_scratch(lowerer, components->declaration_nodes);
    sxml_xpath_lowerer_release_scratch(lowerer, components->context_tables);
    sxml_xpath_lowerer_release_scratch(lowerer, components);
    lowering->components = NULL;
}

/* ======================================================= Function bodies */

typedef enum function_body_step {
    SXML_FUNCTION_BODY_STEP_START = 0,
    /* The next default, or the call of the full arity. */
    SXML_FUNCTION_BODY_STEP_NEXT_DEFAULT,
    /* A default was lowered as a body. */
    SXML_FUNCTION_BODY_STEP_DEFAULT_LOWERED,
    /* The sequence constructor was lowered. */
    SXML_FUNCTION_BODY_STEP_CONSTRUCTOR_LOWERED,
} function_body_step;

typedef struct function_body_locals {
    sxml_xslt_instruction_locals head;
    const declared_function *function;
    const sxml_xslt_component_lowering *components;
    uint32_t arity;
    /* The parameter whose default is being lowered (an index of the
       function's parameters). */
    uint32_t parameter_offset;
    /* parameter_count arguments of the full arity (owned memory 0). */
    sxml_ir_atom *arguments;
    sxml_ir_expression_index default_value;
    sxml_ir_atom value;
    /* The full arity of a cached function: the call of the cached-call row
       (10.3.7); its sequence constructor is lowered into cached_body. */
    bool is_cache_call;
} function_body_locals;

/* Starts the default of the parameter at parameter_offset (9.2.2): select,
   content (a temporary tree without as), or (). *has_pushed when a frame
   lowers it. */
typedef struct default_start {
    bool has_pushed;
} default_start;

static sxml_status start_default(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame, default_start *start) {
    sxml_xpath_lowerer *lowerer = machine->state;
    function_body_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index parameter =
        locals->components->parameter_nodes[locals->function->first_parameter +
                                            locals->parameter_offset];
    start->has_pushed = false;
    const sxml_xslt_ast_attribute *select =
        find_attribute(lowering, parameter, SXML_XSLT_AST_ATTRIBUTE_SELECT);
    if (select->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
        start->has_pushed = true;
        return sxml_xpath_lowering_call_embedded(
            machine,
            &(sxml_xpath_lowering_embedded_call){
                .embedding = {.ast = sxml_xslt_ast_get_expression(lowering->ast,
                                                                  select->value.payload.expression)
                                         ->ast,
                              .node = {.value = 0},
                              .static_context =
                                  sxml_xslt_lowering_expression_context(lowering, select->value.payload.expression),
                              .focus = NULL},
                .block = NULL,
                .is_body = true,
                .destination = {.expression = &locals->default_value,
                                .atom = NULL,
                                .atom_block = NULL,
                                .focused = NULL}});
    }
    const sxml_xslt_ast_node_index first = node_of(lowering, parameter)->first_child;
    if (first.value != 0) {
        start->has_pushed = true;
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = first,
                         .block = NULL,
                         .is_body = true,
                         .destination = {.expression = &locals->default_value,
                                         .atom = NULL,
                                         .atom_block = NULL,
                                         .focused = NULL},
                         .focus = NULL});
    }
    /* 10.3.1: no select and no content is the empty sequence. */
    return sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, &locals->default_value);
}

/* After the default: content without as is a temporary tree (9.4); the
   value is coerced to the parameter's type and becomes the argument. */
static sxml_status bind_default(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_frame *frame) {
    function_body_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const size_t parameter_index = locals->function->first_parameter + locals->parameter_offset;
    const sxml_xslt_ast_node_index parameter = locals->components->parameter_nodes[parameter_index];
    sxml_status status = SXML_STATUS_OK;
    if (!has_attribute(lowering, parameter, SXML_XSLT_AST_ATTRIBUTE_SELECT) &&
        node_of(lowering, parameter)->first_child.value != 0 &&
        !has_attribute(lowering, parameter, SXML_XSLT_AST_ATTRIBUTE_AS)) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                                 .node_construction = {.content = locals->default_value,
                                                       /* 25.2: as preserve (D179). */
                                                       .construction_flags =
                                                           (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES |
                                                           (uint32_t)
                                                               SXML_IR_CONSTRUCTION_PRESERVES_TYPES,
                                                       .module_number =
                                                           sxml_xslt_lowering_module_of_node(
                                                               lowering, parameter)}},
            &locals->default_value);
    }
    sxml_ir_atom value = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = frame->block,
                                                      .expression = locals->default_value},
            &value);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_coerce_atom(
            lowerer,
            (sxml_xpath_lowering_coercion_request){
                .block = frame->block,
                .node = no_ast_node,
                .value = value,
                .sequence_type = locals->components->parameter_types[parameter_index],
                .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
            &locals->arguments[locals->parameter_offset]);
    }
    locals->parameter_offset += 1;
    return status;
}

/* The shorter arity's call of the full one. */
static sxml_status finish_defaults(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    const function_body_locals *locals = frame->locals;
    const declared_function *function = locals->function;
    sxml_ir_expression_index call = {0};
    const sxml_status status = add_ir_call(
        lowerer,
        (ir_call){.function = {.value = function->first_function.value +
                                        (function->parameter_count - function->required_count)},
                  .arguments = {.atoms = locals->arguments,
                                .atom_count = function->parameter_count}},
        &call);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, call);
}

/* The full arity's value, coerced to the result type (10.3.3,
   XTTE0780). */
static sxml_status finish_function_body(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    const function_body_locals *locals = frame->locals;
    sxml_ir_atom value = locals->value;
    sxml_status status = SXML_STATUS_OK;
    if (locals->function->has_result_type) {
        static const sxml_xslt_error_mapping mappings[] = {
            {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
             .raised_status = SXML_STATUS_TYPE_ERROR,
             .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTTE0780"),
             .description = SXML_BYTE_VIEW_INITIALIZER(
                 "the result of a stylesheet function does not match its as type (XSLT 4.0, "
                 "10.3.3)")},
        };
        status = sxml_xslt_lowering_bind_mapped_expression(
            lowerer,
            &(sxml_xslt_mapped_expression){
                .block = frame->block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_COERCE,
                               .coercion = {.value = value,
                                            .sequence_type = locals->function->result_type,
                                            .role = SXML_IR_COERCION_ROLE_FUNCTION_RESULT}},
                .mappings = {.mappings = mappings,
                             .mapping_count = sizeof(mappings) / sizeof(mappings[0])}},
            &value);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, atom_expression(value), &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

/* The full arity: the parameters in scope as slots 1 onwards, then the
   sequence constructor. */
static sxml_status start_full_arity(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    function_body_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t offset = 0; status == SXML_STATUS_OK && offset < locals->arity; offset += 1) {
        const sxml_xslt_ast_node_index parameter =
            locals->components->parameter_nodes[locals->function->first_parameter + offset];
        status = sxml_xpath_lowerer_push_variable(
            lowerer,
            (sxml_xpath_lowering_scope_entry){.variable_name = name_of(lowering, parameter),
                                              .function_level = lowerer->function->function_level,
                                              .atom = sxml_ir_slot_atom((sxml_ir_slot){.value = offset + 1})});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = (uint32_t)SXML_FUNCTION_BODY_STEP_CONSTRUCTOR_LOWERED;
    return sxml_xslt_lowering_call_sequence_constructor(
        machine, &(sxml_xslt_sequence_constructor_call){
                     .lowering = lowering,
                     .first = sxml_xslt_lowering_first_constructor_child(lowering->ast,
                                                                         locals->head.node),
                     .block = frame->block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = &locals->value,
                                     .atom_block = frame->block,
                                     .focused = NULL},
                     .focus = NULL});
}

/* A cached function's full arity (10.3.7): cached-call(n, [$p1, ...,
   $pn], body#n), n the number of cached_body, so equal arguments give the
   value the first call computed. */
static sxml_status finish_cache_call(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    function_body_locals *locals = frame->locals;
    const declared_function *function = locals->function;
    sxml_status status = SXML_STATUS_OK;
    if (function->parameter_count > 0) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = function->parameter_count,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_atom *members = function->parameter_count > 0 ? frame->owned_memory[0] : NULL;
    for (uint32_t offset = 0; offset < function->parameter_count; offset += 1) {
        members[offset] = sxml_ir_slot_atom((sxml_ir_slot){.value = offset + 1});
    }
    sxml_ir_atom_list member_list = {0};
    status = sxml_ir_unit_add_atoms(
        lowerer->unit,
        (sxml_ir_atom_view){.atoms = members, .atom_count = function->parameter_count},
        &member_list);
    sxml_ir_atom arguments[3] = {{0}, {0}, {0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(
            lowerer, sxml_atomic_item_from_integer((int64_t)function->cached_body.value),
            &arguments[0]);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            lowerer,
            (sxml_xslt_expression_to_bind){
                .block = frame->block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
                               .array_constructor = {.array_form = SXML_IR_ARRAY_FORM_SQUARE,
                                                     .members = member_list}}},
            &arguments[1]);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            lowerer,
            (sxml_xslt_expression_to_bind){
                .block = frame->block,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE,
                    .function_reference = {
                        .target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                   .function = function->cached_body},
                        .arity = function->parameter_count,
                        .signature = sxml_ir_unit_get_function(lowerer->unit,
                                                               function->cached_body)
                                         ->signature}}},
            &arguments[2]);
    }
    sxml_ir_atom_list argument_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(lowerer->unit,
                                        (sxml_ir_atom_view){.atoms = arguments, .atom_count = 3},
                                        &argument_list);
    }
    sxml_ir_expression_index call = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                .static_call = {.target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                           .native_function = &sxml_xslt_cached_call_function},
                                .arguments = argument_list}},
            &call);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, call);
}

static sxml_status continue_function_body(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    function_body_locals *locals = frame->locals;
    const declared_function *function = locals->function;
    sxml_status status = SXML_STATUS_OK;
    switch ((function_body_step)frame->step) {
    case SXML_FUNCTION_BODY_STEP_START:
        if (locals->is_cache_call) {
            return finish_cache_call(machine, frame);
        }
        if (locals->arity == function->parameter_count) {
            return start_full_arity(machine, frame);
        }
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = function->parameter_count,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->arguments = frame->owned_memory[0];
        for (uint32_t offset = 0; offset < locals->arity; offset += 1) {
            locals->arguments[offset] = sxml_ir_slot_atom((sxml_ir_slot){.value = offset + 1});
        }
        locals->parameter_offset = locals->arity;
        break;
    case SXML_FUNCTION_BODY_STEP_NEXT_DEFAULT:
        break;
    case SXML_FUNCTION_BODY_STEP_DEFAULT_LOWERED:
        status = bind_default(lowerer, frame);
        break;
    case SXML_FUNCTION_BODY_STEP_CONSTRUCTOR_LOWERED:
        return finish_function_body(machine, frame);
    }
    frame->step = (uint32_t)SXML_FUNCTION_BODY_STEP_NEXT_DEFAULT;
    while (status == SXML_STATUS_OK && locals->parameter_offset < function->parameter_count) {
        default_start start = {.has_pushed = false};
        status = start_default(machine, frame, &start);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (start.has_pushed) {
            frame->step = (uint32_t)SXML_FUNCTION_BODY_STEP_DEFAULT_LOWERED;
            return SXML_STATUS_OK;
        }
        status = bind_default(lowerer, frame);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_defaults(machine, frame);
}

static const sxml_xpath_lowering_procedure function_body_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:function"),
    .step = continue_function_body,
    .locals_size = sizeof(function_body_locals)};

/* ====================================================== Attribute sets */

/* The focus atoms an attribute set is called with: the caller's focus, or
   (), 0, 0 for an absent one (the set's absent-focus function ignores
   them). */
typedef struct focus_atoms {
    sxml_ir_atom item;
    sxml_ir_atom position;
    sxml_ir_atom size;
} focus_atoms;

static sxml_status bind_focus_atoms(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_block *block,
                                    focus_atoms *atoms) {
    if (lowerer->function->focus->kind == SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT) {
        sxml_status status = bind_expression(
            lowerer,
            (sxml_xslt_expression_to_bind){
                .block = block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                               .sequence = {.first_atom_index = 0, .atom_count = 0}}},
            &atoms->item);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_item_constant(
                lowerer, sxml_atomic_item_from_integer(0), &atoms->position);
        }
        atoms->size = atoms->position;
        return status;
    }
    const sxml_xpath_lowering_focus_part parts[3] = {SXML_XPATH_LOWERING_FOCUS_PART_VALUE,
                                                     SXML_XPATH_LOWERING_FOCUS_PART_POSITION,
                                                     SXML_XPATH_LOWERING_FOCUS_PART_SIZE};
    sxml_ir_atom *targets[3] = {&atoms->item, &atoms->position, &atoms->size};
    sxml_status status = SXML_STATUS_OK;
    for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < 3; part_index += 1) {
        status = sxml_xpath_lowerer_read_focus(
            lowerer,
            (sxml_xpath_lowering_focus_read){
                .block = block, .node = no_ast_node, .part = parts[part_index]},
            targets[part_index]);
    }
    return status;
}

/* Binds the calls of the attribute sets tokens names (attribute set
   invocations, 10.2.1) in block, one atom each into atoms. */
typedef struct invocation_request {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atoms;
} invocation_request;

static sxml_status bind_invocations(sxml_xpath_lowerer *lowerer, const invocation_request *request,
                                    size_t *atom_count) {
    const sxml_xslt_lowering *lowering = request->lowering;
    const sxml_xslt_ast_name_token_view tokens = used_sets_of(lowering, request->node);
    *atom_count = 0;
    if (tokens.name_token_count == 0) {
        return SXML_STATUS_OK;
    }
    focus_atoms focus = {{0}, {0}, {0}};
    sxml_status status = bind_focus_atoms(lowerer, request->block, &focus);
    const bool is_focus_absent =
        lowerer->function->focus->kind == SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT;
    for (size_t token_index = 0; status == SXML_STATUS_OK && token_index < tokens.name_token_count;
         token_index += 1) {
        const attribute_set *set = resolve_attribute_set(
            lowering, (attribute_set_reference){.node = request->node,
                                                .set_name = tokens.name_tokens[token_index].name});
        if (set == NULL) {
            return report_unknown_set(lowering, request->node);
        }
        const sxml_ir_atom arguments[ATTRIBUTE_SET_PARAMETER_COUNT] = {focus.item, focus.position,
                                                                       focus.size};
        sxml_ir_expression_index call = {0};
        status = add_ir_call(
            lowerer,
            (ir_call){.function = is_focus_absent ? set->absent_function : set->function,
                      .arguments = {.atoms = arguments, .atom_count = ATTRIBUTE_SET_PARAMETER_COUNT}},
            &call);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_bind_expression(
                lowerer,
                (sxml_xpath_lowering_expression_in_block){.block = request->block,
                                                          .expression = call},
                &request->atoms[*atom_count]);
            *atom_count += 1;
        }
    }
    return status;
}

sxml_status sxml_xslt_lowering_bind_attribute_sets(sxml_xpath_lowerer *lowerer,
                                                   const sxml_xslt_attribute_set_call *call,
                                                   sxml_ir_atom *atom) {
    const sxml_xslt_lowering *lowering = call->lowering;
    const size_t token_count = used_sets_of(lowering, call->node).name_token_count;
    void *memory = NULL;
    sxml_status status = allocate_array(
        lowering,
        (array_request){.element_count = token_count == 0 ? 1 : token_count,
                        .element_size = sizeof(sxml_ir_atom),
                        .element_alignment = alignof(sxml_ir_atom)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_atom *atoms = memory;
    size_t atom_count = 0;
    status = bind_invocations(
        lowerer,
        &(invocation_request){
            .lowering = lowering, .node = call->node, .block = call->block, .atoms = atoms},
        &atom_count);
    sxml_ir_expression_index sequence = {0};
    if (status == SXML_STATUS_OK) {
        status = add_sequence(lowerer, (sxml_ir_atom_view){.atoms = atoms, .atom_count = atom_count},
                              &sequence);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = call->block, .expression = sequence},
            atom);
    }
    sxml_xpath_lowerer_release_scratch(lowerer, memory);
    return status;
}

/* The body of an attribute set: each declaration's invocations, then its
   xsl:attribute children (10.2.1), in order. */
typedef struct attribute_set_locals {
    sxml_xslt_instruction_locals head;
    const attribute_set *set;
    size_t declaration_offset;
    /* One atom per invocation and declaration (owned memory 0). */
    sxml_ir_atom *atoms;
    size_t atom_count;
} attribute_set_locals;

static size_t attribute_set_atom_capacity(const sxml_xslt_lowering *lowering,
                                          const attribute_set *set) {
    const sxml_xslt_component_lowering *components = lowering->components;
    size_t capacity = 0;
    for (size_t offset = 0; offset < set->declaration_count; offset += 1) {
        capacity +=
            used_sets_of(lowering, components->declaration_nodes[set->first_declaration + offset])
                .name_token_count +
            1U;
    }
    return capacity;
}

static sxml_status continue_attribute_set(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    attribute_set_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_component_lowering *components = lowering->components;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_status status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = attribute_set_atom_capacity(lowering, locals->set),
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->atoms = frame->owned_memory[0];
    }
    if (locals->declaration_offset == locals->set->declaration_count) {
        sxml_ir_expression_index result = {0};
        sxml_status status = SXML_STATUS_OK;
        if (locals->atom_count == 0) {
            status = sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, &result);
        } else {
            status = add_sequence(
                lowerer, (sxml_ir_atom_view){.atoms = locals->atoms, .atom_count = locals->atom_count},
                &result);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowering_finish_frame(machine, frame, result);
    }
    const sxml_xslt_ast_node_index declaration =
        components->declaration_nodes[locals->set->first_declaration + locals->declaration_offset];
    locals->declaration_offset += 1;
    size_t invocation_count = 0;
    const sxml_status status =
        bind_invocations(lowerer,
                         &(invocation_request){.lowering = lowering,
                                               .node = declaration,
                                               .block = frame->block,
                                               .atoms = &locals->atoms[locals->atom_count]},
                         &invocation_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->atom_count += invocation_count;
    const sxml_xslt_ast_node_index first = node_of(lowering, declaration)->first_child;
    if (first.value == 0) {
        return SXML_STATUS_OK;
    }
    sxml_ir_atom *content = &locals->atoms[locals->atom_count];
    locals->atom_count += 1;
    return sxml_xslt_lowering_call_sequence_constructor(
        machine, &(sxml_xslt_sequence_constructor_call){.lowering = lowering,
                                                        .first = first,
                                                        .block = frame->block,
                                                        .is_body = false,
                                                        .destination = {.expression = NULL,
                                                                        .atom = content,
                                                                        .atom_block = frame->block,
                                                                        .focused = NULL},
                                                        .focus = NULL});
}

static const sxml_xpath_lowering_procedure attribute_set_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:attribute-set"),
    .step = continue_attribute_set,
    .locals_size = sizeof(attribute_set_locals)};

/* ============================================================== Lowering */

/* One IR function to lower outside a machine by a procedure. */
typedef struct component_function {
    sxml_ir_function_index function;
    uint32_t parameter_count;
    /* An abstract component's: the body raises XTDE3052 (3.4.5.2). */
    bool is_abstract;
    sxml_xpath_lowering_focus focus;
    const sxml_xpath_lowering_procedure *procedure;
    /* The procedure's initial locals. */
    sxml_byte_view initial_locals;
} component_function;

static sxml_status lower_component_function(const sxml_xslt_lowering *lowering,
                                            const component_function *request) {
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    sxml_xpath_lowering_function_frame function_frame = {
        .function = {.function_index = request->function,
                     .function_level = 0,
                     .slot_count = request->parameter_count,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = request->focus,
    };
    sxml_xpath_lowerer_enter_function(lowerer, &function_frame);
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    sxml_ir_expression_index body = {0};
    if (request->is_abstract) {
        sxml_status status = sxml_xpath_lowerer_add_raise(
            lowerer,
            (sxml_xpath_lowering_raise_request){
                .node = no_ast_node,
                .error_status = SXML_STATUS_DYNAMIC_ERROR,
                .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XTDE3052"),
                .description = SXML_BYTE_VIEW_FROM_LITERAL(
                    "An abstract component no package overrides is called (XSLT 4.0, 3.4.5.2)")},
            &body);
        sxml_xpath_lowerer_leave_function(lowerer, &function_frame);
        sxml_ir_function_replacement replacement = {
            .index = request->function,
            .function = *sxml_ir_unit_get_function(lowerer->unit, request->function)};
        replacement.function.body = body;
        replacement.function.slot_count = function_frame.function.slot_count;
        return status == SXML_STATUS_OK ? sxml_ir_unit_replace_function(lowerer->unit, &replacement)
                                        : status;
    }
    sxml_status status = sxml_xpath_lowerer_run_procedure(
        lowerer, &(sxml_xpath_lowering_procedure_call){
                     .procedure = request->procedure,
                     .node = no_ast_node,
                     .block = &block,
                     .is_body = false,
                     .destination = {.expression = &body,
                                     .atom = NULL,
                                     .atom_block = NULL,
                                     .focused = NULL},
                     .initial_locals = request->initial_locals});
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = body}, &body);
    }
    sxml_xpath_lowerer_leave_function(lowerer, &function_frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = request->function,
        .function = *sxml_ir_unit_get_function(lowerer->unit, request->function)};
    replacement.function.body = body;
    replacement.function.slot_count = function_frame.function.slot_count;
    return sxml_ir_unit_replace_function(lowerer->unit, &replacement);
}

static sxml_status lower_functions(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_component_lowering *components = lowering->components;
    if (components->functions == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    for (size_t index = 0; status == SXML_STATUS_OK && index < components->function_count;
         index += 1) {
        const declared_function *function = &components->functions[index];
        for (uint32_t arity = function->required_count;
             function->is_component && status == SXML_STATUS_OK &&
             arity <= function->parameter_count;
             arity += 1) {
            const bool is_cache_call = function->is_cached && arity == function->parameter_count;
            const sxml_ir_function_index full_function = {
                .value = function->first_function.value + (arity - function->required_count)};
            /* A cached function's sequence constructor goes to cached_body;
               the full arity then becomes its cache call. */
            for (uint32_t pass_number = 0;
                 status == SXML_STATUS_OK && pass_number < (is_cache_call ? 2U : 1U);
                 pass_number += 1) {
                const bool lowers_cache_call = is_cache_call && pass_number == 1;
                const function_body_locals initial = {
                    .head = {.lowering = lowering, .node = function->node},
                    .function = function,
                    .components = components,
                    .arity = arity,
                    .is_cache_call = lowers_cache_call};
                status = lower_component_function(
                    lowering,
                    &(component_function){
                        .function = is_cache_call && !lowers_cache_call ? function->cached_body
                                                                         : full_function,
                        .parameter_count = arity,
                        .is_abstract = sxml_xslt_package_is_abstract(lowering, function->node),
                        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT},
                        .procedure = &function_body_procedure,
                        .initial_locals = {.bytes = (const uint8_t *)&initial,
                                           .byte_count = sizeof(initial)}});
            }
        }
    }
    return status;
}

static sxml_status lower_attribute_sets(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_component_lowering *components = lowering->components;
    sxml_status status = SXML_STATUS_OK;
    for (size_t index = 0; status == SXML_STATUS_OK && index < components->attribute_set_count;
         index += 1) {
        const attribute_set *set = &components->attribute_sets[index];
        const attribute_set_locals initial = {
            .head = {.lowering = lowering,
                     .node = components->declaration_nodes[set->first_declaration]},
            .set = set};
        for (size_t variant = 0; status == SXML_STATUS_OK && variant < 2; variant += 1) {
            const bool is_focus_absent = variant == 1;
            status = lower_component_function(
                lowering,
                &(component_function){
                    .function = is_focus_absent ? set->absent_function : set->function,
                    .parameter_count = ATTRIBUTE_SET_PARAMETER_COUNT,
                    .is_abstract = sxml_xslt_package_is_abstract(
                        lowering, components->declaration_nodes[set->first_declaration]),
                    .focus = {.kind = is_focus_absent ? SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT
                                                      : SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
                              .slots = {.context_value = item_slot,
                                        .context_position = position_slot,
                                        .context_size = size_slot}},
                    .procedure = &attribute_set_procedure,
                    .initial_locals = {.bytes = (const uint8_t *)&initial,
                                       .byte_count = sizeof(initial)}});
        }
    }
    return status;
}

/* ======================================================= Decimal formats */

/* The attributes of xsl:decimal-format and the properties they set. */
static const struct {
    sxml_xslt_ast_attribute_kind attribute_kind;
    sxml_decimal_format_property_kind property_kind;
} decimal_format_attributes[] = {
    {SXML_XSLT_AST_ATTRIBUTE_DECIMAL_SEPARATOR, SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR},
    {SXML_XSLT_AST_ATTRIBUTE_GROUPING_SEPARATOR, SXML_DECIMAL_FORMAT_PROPERTY_GROUPING_SEPARATOR},
    {SXML_XSLT_AST_ATTRIBUTE_EXPONENT_SEPARATOR, SXML_DECIMAL_FORMAT_PROPERTY_EXPONENT_SEPARATOR},
    {SXML_XSLT_AST_ATTRIBUTE_PERCENT, SXML_DECIMAL_FORMAT_PROPERTY_PERCENT},
    {SXML_XSLT_AST_ATTRIBUTE_PER_MILLE, SXML_DECIMAL_FORMAT_PROPERTY_PER_MILLE},
    {SXML_XSLT_AST_ATTRIBUTE_ZERO_DIGIT, SXML_DECIMAL_FORMAT_PROPERTY_ZERO_DIGIT},
    {SXML_XSLT_AST_ATTRIBUTE_DIGIT, SXML_DECIMAL_FORMAT_PROPERTY_DIGIT},
    {SXML_XSLT_AST_ATTRIBUTE_PATTERN_SEPARATOR, SXML_DECIMAL_FORMAT_PROPERTY_PATTERN_SEPARATOR},
    {SXML_XSLT_AST_ATTRIBUTE_INFINITY, SXML_DECIMAL_FORMAT_PROPERTY_INFINITY},
    {SXML_XSLT_AST_ATTRIBUTE_NAN, SXML_DECIMAL_FORMAT_PROPERTY_NAN},
    {SXML_XSLT_AST_ATTRIBUTE_MINUS_SIGN, SXML_DECIMAL_FORMAT_PROPERTY_MINUS_SIGN},
};
#define DECIMAL_FORMAT_ATTRIBUTE_COUNT \
    (sizeof(decimal_format_attributes) / sizeof(decimal_format_attributes[0]))

/* One property of a decimal format being merged: its text and the
   precedence of the declaration that set it. */
typedef struct decimal_format_setting {
    bool is_set;
    uint32_t import_precedence;
    sxml_byte_view text;
    /* Two values at import_precedence: XTSE1290 unless a higher one
       replaces them. */
    sxml_xslt_ast_node_index conflict;
} decimal_format_setting;

/* Merges declaration into the settings of its format (one row of
   DECIMAL_FORMAT_ATTRIBUTE_COUNT): a higher import precedence wins; two
   values of one precedence are a conflict. */
static sxml_status merge_decimal_format(const sxml_xslt_lowering *lowering,
                                        sxml_xslt_ast_node_index declaration,
                                        decimal_format_setting *settings) {
    const uint32_t precedence = precedence_of(lowering, declaration);
    for (size_t index = 0; index < DECIMAL_FORMAT_ATTRIBUTE_COUNT; index += 1) {
        const sxml_xslt_ast_attribute *attribute =
            find_attribute(lowering, declaration, decimal_format_attributes[index].attribute_kind);
        if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_TEXT) {
            continue;
        }
        const sxml_byte_view text = sxml_xslt_ast_get_text(lowering->ast, attribute->value.payload.text);
        decimal_format_setting *setting = &settings[index];
        if (setting->is_set && setting->import_precedence == precedence &&
            !sxml_byte_view_equals(setting->text, text)) {
            setting->conflict = declaration;
        }
        if (!setting->is_set || precedence > setting->import_precedence) {
            *setting = (decimal_format_setting){.is_set = true,
                                                .import_precedence = precedence,
                                                .text = text,
                                                .conflict = {.value = 0}};
        }
    }
    return SXML_STATUS_OK;
}

/* The scratch of the decimal formats: one format, one row of settings and
   up to one property per attribute for each declaration. */
typedef struct decimal_format_storage {
    sxml_decimal_format *formats;
    decimal_format_setting *settings;
    sxml_decimal_format_property *properties;
} decimal_format_storage;

static void release_decimal_formats(const sxml_xslt_lowering *lowering,
                                    const decimal_format_storage *storage) {
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, storage->formats);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, storage->settings);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, storage->properties);
}

/* Each format's settings as its properties (XTSE1300 for markers that are
   not distinct or a value of the wrong form, F&O 4.0, 4.9.1). */
static sxml_status finish_decimal_formats(const sxml_xslt_lowering *lowering,
                                          const decimal_format_storage *storage,
                                          size_t format_count) {
    size_t property_count = 0;
    for (size_t format_index = 0; format_index < format_count; format_index += 1) {
        const size_t first = property_count;
        for (size_t index = 0; index < DECIMAL_FORMAT_ATTRIBUTE_COUNT; index += 1) {
            const decimal_format_setting *setting =
                &storage->settings[format_index * DECIMAL_FORMAT_ATTRIBUTE_COUNT + index];
            if (setting->conflict.value != 0) {
                return report_static_error(
                    lowering, setting->conflict,
                    SXML_BYTE_VIEW_FROM_LITERAL("XTSE1290 two xsl:decimal-format declarations of "
                                                "one name and import precedence give a property "
                                                "different values (XSLT 4.0, 5.5)"));
            }
            if (setting->is_set) {
                storage->properties[property_count] = (sxml_decimal_format_property){
                    .property_kind = decimal_format_attributes[index].property_kind,
                    .property_value = setting->text};
                property_count += 1;
            }
        }
        storage->formats[format_index].decimal_format_properties =
            (sxml_decimal_format_property_view){.decimal_format_properties =
                                                    &storage->properties[first],
                                                .decimal_format_property_count =
                                                    property_count - first};
        const sxml_decimal_format_settings defaults = sxml_decimal_format_default_settings();
        sxml_decimal_format_settings settings = defaults;
        const sxml_status status = sxml_decimal_format_apply(
            &(sxml_decimal_format_change){
                .base_settings = &defaults,
                .decimal_format_properties = storage->formats[format_index].decimal_format_properties},
            &settings);
        if (status == SXML_STATUS_MALFORMED_INPUT) {
            return sxml_xslt_lowering_report_static_error(
                lowering,
                (sxml_xslt_static_error){
                    .node = sxml_xslt_ast_get_declaration(lowering->ast, 1),
                    .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                        "XTSE1300 the markers of a decimal format are not distinct, or a property "
                        "has a value of the wrong form (XSLT 4.0, 5.5)")});
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* xsl:decimal-format (XSLT 4.0, 5.5): the declarations of one name (none
   for the unnamed format) in package (3.4.8: decimal formats are local to
   a package) merged by import precedence, into *storage (released by the
   caller) and *format_count. */
typedef struct package_decimal_formats {
    decimal_format_storage storage;
    size_t format_count;
} package_decimal_formats;

static sxml_status make_decimal_formats(const sxml_xslt_lowering *lowering, uint32_t package,
                                        package_decimal_formats *formats) {
    const sxml_xslt_ast *ast = lowering->ast;
    size_t declaration_count = 0;
    *formats = (package_decimal_formats){
        .storage = {.formats = NULL, .settings = NULL, .properties = NULL}, .format_count = 0};
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        declaration_count += node_of(lowering, declaration)->kind ==
                                         SXML_XSLT_AST_KIND_DECIMAL_FORMAT &&
                                     sxml_xslt_package_of_node(lowering, declaration) == package
                                 ? 1U
                                 : 0U;
    }
    if (declaration_count == 0) {
        return SXML_STATUS_OK;
    }
    decimal_format_storage storage = {.formats = NULL, .settings = NULL, .properties = NULL};
    void *memory = NULL;
    sxml_status status = allocate_array(
        lowering,
        (array_request){.element_count = declaration_count,
                        .element_size = sizeof(sxml_decimal_format),
                        .element_alignment = alignof(sxml_decimal_format)},
        &memory);
    storage.formats = memory;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(
            lowering,
            (array_request){.element_count = declaration_count * DECIMAL_FORMAT_ATTRIBUTE_COUNT,
                            .element_size = sizeof(decimal_format_setting),
                            .element_alignment = alignof(decimal_format_setting)},
            &memory);
        storage.settings = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_array(
            lowering,
            (array_request){.element_count = declaration_count * DECIMAL_FORMAT_ATTRIBUTE_COUNT,
                            .element_size = sizeof(sxml_decimal_format_property),
                            .element_alignment = alignof(sxml_decimal_format_property)},
            &memory);
        storage.properties = memory;
    }
    size_t format_count = 0;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, declaration)->kind != SXML_XSLT_AST_KIND_DECIMAL_FORMAT ||
            sxml_xslt_package_of_node(lowering, declaration) != package) {
            continue;
        }
        const sxml_name_identifier format_name = name_of(lowering, declaration);
        size_t format_index = 0;
        while (format_index < format_count &&
               storage.formats[format_index].format_name.index != format_name.index) {
            format_index += 1;
        }
        if (format_index == format_count) {
            storage.formats[format_count] = (sxml_decimal_format){.format_name = format_name};
            format_count += 1;
        }
        status = merge_decimal_format(
            lowering, declaration,
            &storage.settings[format_index * DECIMAL_FORMAT_ATTRIBUTE_COUNT]);
    }
    if (status == SXML_STATUS_OK) {
        status = finish_decimal_formats(lowering, &storage, format_count);
    }
    *formats = (package_decimal_formats){.storage = storage, .format_count = format_count};
    return status;
}

/* The static context a module of the unit keeps for an AST static
   context: its base URI, namespaces and default collation, its package's
   decimal formats. */
typedef struct static_context_module {
    const sxml_xpath_static_context *context;
    const sxml_xslt_ast_static_context *ast_context;
    const package_decimal_formats *formats;
    sxml_byte_view module_uri;
} static_context_module;

static sxml_status add_static_context_module(const sxml_xslt_lowering *lowering,
                                             static_context_module request,
                                             uint32_t *module_number) {
    return sxml_ir_unit_add_module(
        lowering->lowerer->unit,
        &(sxml_ir_module){
            /* Its stylesheet module's URI, which never moves (D102): the
               source name of errors in code that reads the module. */
            .source_name = request.module_uri,
            .static_base_uri = request.context->static_base_uri,
            .text_memory = NULL,
            .has_static_context = true,
            .static_context =
                {.decimal_formats = {.decimal_formats = request.formats->storage.formats,
                                     .decimal_format_count = request.formats->format_count},
                 .default_collation_uri =
                     sxml_xslt_ast_get_text(lowering->ast, request.ast_context->default_collation),
                 .collation_bindings = {.collation_bindings = NULL, .collation_binding_count = 0},
                 .in_scope_schemas = request.context->unit_static_context.in_scope_schemas},
            .static_context_memory = NULL,
            .has_static_namespaces = true,
            .static_namespaces = request.context->namespace_bindings,
            .namespace_memory = NULL},
        module_number);
}

sxml_status sxml_xslt_declare_static_context_modules(const sxml_xslt_lowering *lowering,
                                                     sxml_xpath_static_context *contexts) {
    const uint32_t package_count = sxml_xslt_package_count(lowering);
    void *memory = NULL;
    sxml_status status = allocate_array(
        lowering,
        (array_request){.element_count = (size_t)package_count + 1U,
                        .element_size = sizeof(package_decimal_formats),
                        .element_alignment = alignof(package_decimal_formats)},
        &memory);
    package_decimal_formats *formats = memory;
    for (uint32_t package = 1; status == SXML_STATUS_OK && package <= package_count;
         package += 1) {
        status = make_decimal_formats(lowering, package, &formats[package]);
    }
    /* Code no module names (the main function's, XSLT's own calls) reads
       the top-level package's formats (package 1). */
    const sxml_schema_view schemas = sxml_xslt_ast_get_in_scope_schemas(lowering->ast);
    if (status == SXML_STATUS_OK && (formats[1].format_count > 0 || schemas.schema_count > 0)) {
        /* The in-scope schemas too (validation, type-available; D179). */
        status = sxml_ir_unit_set_static_context(
            lowering->lowerer->unit,
            &(sxml_ir_static_context){
                .decimal_formats = {.decimal_formats = formats[1].storage.formats,
                                    .decimal_format_count = formats[1].format_count},
                .default_collation_uri = {.bytes = NULL, .byte_count = 0},
                .collation_bindings = {.collation_bindings = NULL, .collation_binding_count = 0},
                .in_scope_schemas = schemas});
    }
    /* The stylesheet module of each static context: that of a node that
       has it (a static context belongs to one element of one module). */
    uint32_t *context_modules = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(
            lowering, (array_request){.element_count = lowering->static_context_count,
                                      .element_size = sizeof(uint32_t),
                                      .element_alignment = alignof(uint32_t)},
            &memory);
        context_modules = memory;
    }
    for (uint32_t node_number = 1; context_modules != NULL && node_number < lowering->ast->node_count;
         node_number += 1) {
        const sxml_xslt_ast_node *node =
            node_of(lowering, (sxml_xslt_ast_node_index){.value = node_number});
        if (node->static_context.value < lowering->static_context_count) {
            context_modules[node->static_context.value] = node->module.value;
        }
    }
    for (uint32_t context_index = 1;
         status == SXML_STATUS_OK && context_index < lowering->static_context_count;
         context_index += 1) {
        const sxml_xslt_ast_static_context_index index = {.value = context_index};
        const sxml_byte_view module_uri =
            context_modules[context_index] == 0
                ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                : sxml_xslt_ast_get_module(
                      lowering->ast,
                      (sxml_xslt_ast_module_index){.value = context_modules[context_index]})
                      ->module_uri;
        uint32_t package = sxml_xslt_package_of_static_context(lowering, index);
        if (package == 0 || package > package_count) {
            package = 1;
        }
        status = add_static_context_module(
            lowering,
            (static_context_module){
                .context = &contexts[context_index],
                .ast_context = sxml_xslt_ast_get_static_context(lowering->ast, index),
                .formats = &formats[package],
                .module_uri = module_uri},
            &contexts[context_index].ir_module_number);
    }
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, context_modules);
    for (uint32_t package = 1; formats != NULL && package <= package_count; package += 1) {
        release_decimal_formats(lowering, &formats[package].storage);
    }
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, formats);
    return status;
}

sxml_status sxml_xslt_lower_components(const sxml_xslt_lowering *lowering) {
    if (lowering->components == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_status status = lower_functions(lowering);
    if (status == SXML_STATUS_OK) {
        status = lower_attribute_sets(lowering);
    }
    return status;
}

/* ================================================== Global context item */

/* The keyword of node's use attribute; fallback when absent. */
static sxml_xslt_ast_keyword use_of(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *use = find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_USE);
    return use->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD ? use->value.payload.keyword
                                                               : SXML_XSLT_AST_KEYWORD_OPTIONAL;
}

/* The item type of node's as attribute; item() without one. */
static sxml_status resolve_item_type(const sxml_xslt_lowering *lowering,
                                     sxml_xslt_ast_node_index node, sxml_type_identifier *item_type) {
    *item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM);
    const sxml_xslt_ast_attribute *as_attribute =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_AS);
    if (as_attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, as_attribute->value.payload.expression);
    const sxml_xpath_static_context *context =
        sxml_xslt_lowering_expression_context(lowering, as_attribute->value.payload.expression);
    if (expression->ast == NULL || context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* The AST's root is a SEQUENCE_TYPE whose occurrence the reader
       checked to be one (D139). */
    sxml_sequence_type sequence_type = {.item_type = *item_type,
                                        .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    const sxml_status status = sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = expression->ast,
                                   .type_node = expression->ast->root,
                                   .static_context = context,
                                   .error_record = lowering->error_record},
        &sequence_type);
    *item_type = sequence_type.item_type;
    return status;
}

/* The xsl:global-context-item in force (3.4.9): at most one per module,
   all of one stylesheet alike (XTSE3087); 0 for none. */
static sxml_status find_global_context_item(const sxml_xslt_lowering *lowering,
                                            sxml_xslt_ast_node_index *found) {
    const sxml_xslt_ast *ast = lowering->ast;
    found->value = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, declaration)->kind != SXML_XSLT_AST_KIND_GLOBAL_CONTEXT_ITEM) {
            continue;
        }
        if (found->value != 0) {
            sxml_type_identifier found_type = {0};
            sxml_type_identifier type = {0};
            sxml_status status = resolve_item_type(lowering, *found, &found_type);
            if (status == SXML_STATUS_OK) {
                status = resolve_item_type(lowering, declaration, &type);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            const bool is_same_module =
                node_of(lowering, *found)->module.value == node_of(lowering, declaration)->module.value;
            const bool is_consistent = use_of(lowering, *found) == use_of(lowering, declaration) &&
                                       found_type.index == type.index;
            if (is_same_module || !is_consistent) {
                return report_static_error(
                    lowering, declaration,
                    SXML_BYTE_VIEW_FROM_LITERAL("XTSE3087 two xsl:global-context-item "
                                                "declarations in one module, or two that "
                                                "differ (XSLT 4.0, 3.4.9)"));
            }
        }
        *found = declaration;
    }
    if (found->value != 0 && use_of(lowering, *found) == SXML_XSLT_AST_KEYWORD_ABSENT &&
        has_attribute(lowering, *found, SXML_XSLT_AST_ATTRIBUTE_AS)) {
        return report_static_error(
            lowering, *found,
            SXML_BYTE_VIEW_FROM_LITERAL("XTSE3089 xsl:global-context-item has an as attribute "
                                        "and use=\"absent\" (XSLT 4.0, 3.4.9)"));
    }
    return SXML_STATUS_OK;
}

/* A RAISE as the body of a branch. */
typedef struct raise_text {
    sxml_status error_status;
    sxml_byte_view code;
    sxml_byte_view description;
} raise_text;

static sxml_status add_raise(sxml_xpath_lowerer *lowerer, raise_text text,
                             sxml_ir_expression_index *expression) {
    return sxml_xpath_lowerer_add_raise(
        lowerer,
        (sxml_xpath_lowering_raise_request){.node = no_ast_node,
                                            .error_status = text.error_status,
                                            .code_local_name = text.code,
                                            .description = text.description},
        expression);
}

/* The branch for a supplied global context item: its type checked
   (XTTE0590), or XTSE3088 when it must be absent. */
typedef struct supplied_item {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index declaration;
} supplied_item;

static sxml_status lower_supplied_item(sxml_xpath_lowerer *lowerer, supplied_item request,
                                       sxml_ir_expression_index *branch) {
    const sxml_xslt_lowering *lowering = request.lowering;
    const sxml_xslt_ast_node_index declaration = request.declaration;
    if (use_of(lowering, declaration) == SXML_XSLT_AST_KEYWORD_ABSENT) {
        return add_raise(lowerer,
                         (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                      .code = SXML_BYTE_VIEW_FROM_LITERAL("XTSE3088"),
                                      .description = SXML_BYTE_VIEW_FROM_LITERAL(
                                          "a global context item is supplied to a stylesheet "
                                          "whose xsl:global-context-item has use=\"absent\" "
                                          "(XSLT 4.0, 3.4.9)")},
                         branch);
    }
    if (!has_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_AS)) {
        return sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, branch);
    }
    sxml_type_identifier item_type = {0};
    sxml_status status = resolve_item_type(lowering, declaration, &item_type);
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom item = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_read_focus(
            lowerer,
            (sxml_xpath_lowering_focus_read){
                .block = &block, .node = no_ast_node, .part = SXML_XPATH_LOWERING_FOCUS_PART_VALUE},
            &item);
    }
    /* 3.4.9: the error of xsl:param, XTTE0590. */
    static const sxml_xslt_error_mapping mappings[] = {
        {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
         .raised_status = SXML_STATUS_TYPE_ERROR,
         .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTTE0590"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "the global context item does not match the type of xsl:global-context-item (XSLT "
             "4.0, 3.4.9)")},
    };
    sxml_ir_atom checked = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_mapped_expression(
            lowerer,
            &(sxml_xslt_mapped_expression){
                .block = &block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_COERCE,
                               .coercion = {.value = item,
                                            .sequence_type = {.item_type = item_type,
                                                              .occurrence =
                                                                  SXML_OCCURRENCE_EXACTLY_ONE},
                                            .role = SXML_IR_COERCION_ROLE_VARIABLE}},
                .mappings = {.mappings = mappings,
                             .mapping_count = sizeof(mappings) / sizeof(mappings[0])}},
            &checked);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, atom_expression(checked), &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = result}, branch);
}

sxml_status sxml_xslt_lowering_bind_global_context_item(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_global_context_item_call *call) {
    const sxml_xslt_lowering *lowering = call->lowering;
    sxml_xslt_ast_node_index declaration = {.value = 0};
    sxml_status status = find_global_context_item(lowering, &declaration);
    if (status != SXML_STATUS_OK || declaration.value == 0) {
        return status;
    }
    const sxml_xslt_ast_keyword use = use_of(lowering, declaration);
    if (use == SXML_XSLT_AST_KEYWORD_OPTIONAL &&
        !has_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_AS)) {
        return SXML_STATUS_OK;
    }
    sxml_ir_atom has_focus = {0};
    status = sxml_xslt_lowering_bind_has_focus(lowerer, call->block, &has_focus);
    sxml_ir_expression_index supplied = {0};
    if (status == SXML_STATUS_OK) {
        status = lower_supplied_item(
            lowerer, (supplied_item){.lowering = lowering, .declaration = declaration}, &supplied);
    }
    sxml_ir_expression_index missing = {0};
    if (status == SXML_STATUS_OK && use == SXML_XSLT_AST_KEYWORD_REQUIRED) {
        status = add_raise(lowerer,
                           (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                        .code = SXML_BYTE_VIEW_FROM_LITERAL("XTDE3086"),
                                        .description = SXML_BYTE_VIEW_FROM_LITERAL(
                                            "xsl:global-context-item requires a global context "
                                            "item and none is supplied (XSLT 4.0, 3.4.9)")},
                           &missing);
    } else if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, &missing);
    }
    sxml_ir_atom ignored = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            lowerer,
            (sxml_xslt_expression_to_bind){
                .block = call->block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                               .conditional = {.condition = has_focus,
                                               .then_branch = supplied,
                                               .else_branch = missing}}},
            &ignored);
    }
    return status;
}
