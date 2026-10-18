/*
 * Accumulators (src/xslt_accumulator_internal.h; XSLT 4.0, section 19;
 * docs/DECISIONS.md D118, D144, D149, D155; packet 4.14).
 *
 * The functions are IR functions lowered from XPath texts made here, as
 * src/xslt_key.c lowers fn:key (D149), in a static context of their own
 * whose function table adds private names: the internal rows
 * accumulator-name, accumulator-scope and accumulator-values (no
 * namespace, src/function_library_xslt.c) and the IR functions below in
 * the namespace ACCUMULATOR_FUNCTION_NAMESPACE_URI. For an accumulator K
 * whose rules are R1 (phase start), R2 (phase end) and R3 (phase start):
 *
 *     match-N($item)        R_N's pattern (D118), every error false (6.5)
 *     rule-N($node, $value) R_N's select or sequence constructor, focus
 *                           $node, coerced to K's as type (19.1)
 *     initial-K($root)      the initial-value, focus $root, coerced
 *     start-K($node, $value):
 *         if (match-3($node)) then rule-3($node, $value)
 *         else if (match-1($node)) then rule-1($node, $value) else $value
 *     end-K($node, $value): the same over the end rules (capture="yes":
 *         an element is passed as snapshot($node), 19.9)
 *     close-K($state, $nodes): the end events of $nodes, innermost first:
 *         fold-left($nodes, $state, fn($inner, $done) {
 *           let $value := end-K($done, $inner?v)
 *           return map:put(map:put($inner, "v", $value), "a",
 *                          map:put($inner?a, generate-id($done), $value)) })
 *     build-K($root): the traversal (19.4), one start and one end event
 *         per node but attributes and namespaces, the stack "s" holding
 *         the nodes whose end event is still to come:
 *         let $final := fold-left($root/descendant-or-self::node(),
 *               {"v": initial-K($root), "s": (), "b": {}, "a": {}},
 *               fn($state, $node) {
 *                 let $stack := $state?s, $depth := (the ancestors of $node:
 *                       the entries up to its parent, found at the top or
 *                       below it in one step for a first child or a
 *                       sibling, else count($node/ancestor::node())),
 *                     $closed := close-K($state,
 *                                        reverse(subsequence($stack, $depth + 1))),
 *                     $value := start-K($node, $closed?v)
 *                 return {"v": $value,
 *                         "s": (subsequence($closed?s, 1, $depth), $node),
 *                         "b": map:put($closed?b, generate-id($node), $value),
 *                         "a": $closed?a} })
 *         return close-K($final, reverse($final?s))
 *
 * The nodes on the stack are the ancestors-or-self of the node visited
 * last, so the ancestors of the next node are its first $depth entries.
 * fn:accumulator-before($name, $context) is
 *
 *     if (not($context instance of node()) or $context instance of attribute()
 *         or $context instance of namespace-node()) then error(XTTE3360)
 *     else let $name := accumulator-name($name),
 *              $anchor := (the innermost ancestor-or-self of $context that
 *                          accumulator-link made a copy of a node),
 *              $node := $context, or the node of the original at its place,
 *              $root := root($node), $id := generate-id($node)
 *          return if ($name eq QName("", "k"))
 *                 then accumulator-values(1, $root, build-1#1)?b?($id)
 *                 else ... else error(XTDE3340)
 *
 * and fn:accumulator-after the same with ?a. Copies keep the values of
 * what they copy (19.10): fn:copy-of, fn:snapshot and xsl:copy-of with
 * copy-accumulators="yes" link each copy to its original
 * (sxml_xslt_bind_accumulator_link). The main function binds
 * accumulator-scope(root(.), (numbers)) for the initial match selection,
 * xsl:source-document the same for its document.
 *
 * Golden file: src/xslt_key.c (a function table in front of the static
 * contexts, private functions lowered from texts outside a machine);
 * src/xslt_lower_construct.c's xsl:copy for xsl:source-document.
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_accumulator_internal.h"

#include "function_library_core_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xpath_ast_internal.h"
#include "xslt_pattern_internal.h"
#include "xslt_snippet_text_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The namespace of the private functions of the accumulator texts. */
#define ACCUMULATOR_FUNCTION_NAMESPACE_URI "urn:x-sxml:accumulator"

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* ------------------------------------------------------- Table functions */

/* The most parameters of a function made here. */
#define FUNCTION_MAXIMUM_PARAMETER_COUNT 2U

/* One function of the unit the table answers: its types and, once
   declared, its IR function. */
typedef struct table_function {
    sxml_name_identifier function_name;
    sxml_function_parameter_view parameters;
    sxml_sequence_type parameter_types[FUNCTION_MAXIMUM_PARAMETER_COUNT];
    sxml_sequence_type result_type;
    sxml_type_identifier signature;
    /* Without the last parameter, for fn:accumulator-before#1, which
       passes the context value itself; 0 for the private functions. */
    sxml_type_identifier short_signature;
    sxml_ir_function_index function;
} table_function;

/* The two accumulator functions (19.6, 19.7). */
typedef enum accumulator_function_kind {
    SXML_ACCUMULATOR_FUNCTION_BEFORE,
    SXML_ACCUMULATOR_FUNCTION_AFTER,
    SXML_ACCUMULATOR_FUNCTION_COUNT,
} accumulator_function_kind;

static const sxml_function_parameter accumulator_function_parameters[] = {
    SXML_FUNCTION_PARAMETER("name", "xs:anyAtomicType", SXML_PARAMETER_DEFAULT_REQUIRED),
    SXML_FUNCTION_PARAMETER("context", "item()*", SXML_PARAMETER_DEFAULT_CONTEXT_VALUE),
};

static const sxml_byte_view accumulator_function_names[SXML_ACCUMULATOR_FUNCTION_COUNT] = {
    [SXML_ACCUMULATOR_FUNCTION_BEFORE] = SXML_BYTE_VIEW_INITIALIZER("accumulator-before"),
    [SXML_ACCUMULATOR_FUNCTION_AFTER] = SXML_BYTE_VIEW_INITIALIZER("accumulator-after"),
};

static const sxml_function_parameter item_parameters[] = {
    SXML_FUNCTION_PARAMETER("item", "item()", SXML_PARAMETER_DEFAULT_REQUIRED),
};

static const sxml_function_parameter root_parameters[] = {
    SXML_FUNCTION_PARAMETER("root", "node()", SXML_PARAMETER_DEFAULT_REQUIRED),
};

static const sxml_function_parameter node_value_parameters[] = {
    SXML_FUNCTION_PARAMETER("node", "node()", SXML_PARAMETER_DEFAULT_REQUIRED),
    SXML_FUNCTION_PARAMETER("value", "item()*", SXML_PARAMETER_DEFAULT_REQUIRED),
};

static const sxml_function_parameter state_nodes_parameters[] = {
    SXML_FUNCTION_PARAMETER("state", "map(*)", SXML_PARAMETER_DEFAULT_REQUIRED),
    SXML_FUNCTION_PARAMETER("nodes", "node()*", SXML_PARAMETER_DEFAULT_REQUIRED),
};

/* ---------------------------------------------------------- Accumulators */

/* One xsl:accumulator-rule. */
typedef struct accumulator_rule {
    sxml_xslt_ast_node_index node;
    /* Its accumulator in table->accumulators. */
    size_t accumulator_index;
    bool is_end_phase;
    /* capture="yes" (19.9). */
    bool captures;
    table_function match;
    table_function rule;
} accumulator_rule;

/* The xsl:accumulator of one name that has the highest import precedence
   (19.8). */
typedef struct accumulator_definition {
    sxml_xslt_ast_node_index node;
    sxml_name_identifier accumulator_name;
    table_function initial;
    table_function start;
    table_function end;
    table_function close;
    table_function build;
} accumulator_definition;

/* The variables of the texts, in table->variable_names. */
typedef enum snippet_variable {
    SXML_SNIPPET_VARIABLE_NAME,
    SXML_SNIPPET_VARIABLE_CONTEXT,
    SXML_SNIPPET_VARIABLE_ROOT,
    SXML_SNIPPET_VARIABLE_NODE,
    SXML_SNIPPET_VARIABLE_VALUE,
    SXML_SNIPPET_VARIABLE_STATE,
    SXML_SNIPPET_VARIABLE_NODES,
    SXML_SNIPPET_VARIABLE_COUNT,
} snippet_variable;

static const char *const snippet_variable_names[SXML_SNIPPET_VARIABLE_COUNT] = {
    "name", "context", "root", "node", "value", "state", "nodes"};

struct sxml_xslt_accumulator_table {
    sxml_xslt_lowering *lowering;
    sxml_allocator allocator;
    /* What the static contexts had: the XSLT function table. */
    sxml_function_table inner_table;
    /* The internal rows. */
    sxml_function_library *library;
    table_function functions[SXML_ACCUMULATOR_FUNCTION_COUNT];
    accumulator_definition *accumulators;
    size_t accumulator_count;
    accumulator_rule *rules;
    size_t rule_count;
    bool are_accumulators_lowered;
    /* The static context of the texts made here: the base context with
       the private table. */
    sxml_xpath_static_context snippet_context;
    /* The texts' ASTs, kept until release (their spans are borrowed). */
    sxml_ast **snippets;
    size_t snippet_count;
    size_t snippet_capacity;
    sxml_name_identifier variable_names[SXML_SNIPPET_VARIABLE_COUNT];
    sxml_namespace_uri_identifier private_namespace;
};

/* --------------------------------------------------------------- Helpers */

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

static const sxml_xpath_static_context *
expression_context(const sxml_xslt_lowering *lowering, sxml_xslt_ast_expression_index expression) {
    const uint32_t context =
        sxml_xslt_ast_get_expression(lowering->ast, expression)->static_context.value;
    if (context == 0 || context >= lowering->static_context_count) {
        return NULL;
    }
    return &lowering->static_contexts[context];
}

static uint32_t precedence_of(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_module(lowering->ast, node_of(lowering, node)->module)
        ->import_precedence;
}

/* The name attribute's name; 0 when absent. */
static sxml_name_identifier name_of(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *name =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_NAME);
    if (name->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME) {
        return (sxml_name_identifier){.index = 0};
    }
    return name->value.payload.name;
}

/* An array of element_count (at least one) zeroed elements. */
typedef struct zeroed_request {
    size_t element_count;
    sxml_memory_requirements element;
} zeroed_request;

static sxml_status allocate_zeroed(sxml_allocator allocator, zeroed_request request,
                                   void **memory) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(request.element_count == 0 ? 1 : request.element_count,
                                            request.element.byte_count, &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = request.element.alignment},
            memory);
    }
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, byte_count);
    }
    return status;
}

static sxml_status report_static_error(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_ast_node_index node, sxml_byte_view text) {
    return sxml_xslt_lowering_report_static_error(
        lowering, (sxml_xslt_static_error){.node = node, .code_and_description = text});
}

/* ------------------------------------------------------------------ Text */

/* Q{ACCUMULATOR_FUNCTION_NAMESPACE_URI}, the start of a private name. */
#define PRIVATE_PREFIX "Q{" ACCUMULATOR_FUNCTION_NAMESPACE_URI "}"

/* A call of a private function: PRIVATE_PREFIX prefix number "(". */
typedef struct private_call {
    const char *prefix;
    size_t number;
} private_call;

static void append_private_call(sxml_xslt_snippet_text *text, private_call call) {
    SXML_XSLT_SNIPPET_APPEND(text, PRIVATE_PREFIX);
    sxml_xslt_snippet_text_append(text, sxml_byte_view_from_c_string(call.prefix));
    sxml_xslt_snippet_text_append_number(text, call.number);
    SXML_XSLT_SNIPPET_APPEND(text, "(");
}

/* --------------------------------------------------------- Function types */

/* What a table function is: its name, parameters and result, and whether
   a name of one parameter fewer passes the last one. */
typedef struct table_function_shape {
    sxml_name_identifier function_name;
    sxml_function_parameter_view parameters;
    sxml_byte_view result_type;
    bool has_short_signature;
} table_function_shape;

static sxml_status prepare_function(const sxml_xslt_accumulator_table *table,
                                    table_function_shape shape, table_function *function) {
    const size_t parameter_count = shape.parameters.function_parameter_count;
    if (parameter_count > FUNCTION_MAXIMUM_PARAMETER_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *function =
        (table_function){.function_name = shape.function_name, .parameters = shape.parameters};
    sxml_status status = SXML_STATUS_OK;
    for (size_t parameter_index = 0; status == SXML_STATUS_OK && parameter_index < parameter_count;
         parameter_index += 1) {
        status = sxml_function_library_resolve_type_text(
            table->library, shape.parameters.function_parameters[parameter_index].parameter_type,
            &function->parameter_types[parameter_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_resolve_type_text(table->library, shape.result_type,
                                                         &function->result_type);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_register_signature(
            table->lowering->lowerer,
            (sxml_xpath_lowering_signature_request){
                .parameter_types = {.sequence_types = function->parameter_types,
                                    .sequence_type_count = parameter_count},
                .result_type = function->result_type},
            &function->signature);
    }
    if (status == SXML_STATUS_OK && shape.has_short_signature) {
        status = sxml_xpath_lowerer_register_signature(
            table->lowering->lowerer,
            (sxml_xpath_lowering_signature_request){
                .parameter_types = {.sequence_types = function->parameter_types,
                                    .sequence_type_count = parameter_count - 1},
                .result_type = function->result_type},
            &function->short_signature);
    }
    return status;
}

/* Adds function's IR function to the unit on first use. */
static sxml_status declare_function(const sxml_xslt_accumulator_table *table,
                                    table_function *function) {
    if (function->function.value != 0) {
        return SXML_STATUS_OK;
    }
    return sxml_ir_unit_add_function(
        table->lowering->lowerer->unit,
        &(sxml_ir_function){.function_name = {.index = 0},
                            .signature = function->signature,
                            .parameter_count =
                                (uint32_t)function->parameters.function_parameter_count},
        &function->function);
}

/* The entry of function for a call or reference of arity. */
static void write_entry(const table_function *function, size_t arity, sxml_function_entry *entry) {
    const size_t parameter_count = function->parameters.function_parameter_count;
    *entry = (sxml_function_entry){
        .entry_kind = SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION,
        .native_function = NULL,
        .ir_function_number = function->function.value,
        .parameters = function->parameters,
        .signature = arity < parameter_count ? function->short_signature : function->signature,
        /* fn:accumulator-before#1 takes its second parameter, the context
           value, itself (src/function_library_internal.h, D149). */
        .parameter_types = {.sequence_types = function->parameter_types,
                            .sequence_type_count = parameter_count},
        .result_type = function->result_type};
}

/* -------------------------------------------------------------- Lookups */

static sxml_status find_in_table(sxml_function_table table, const sxml_function_query *query,
                                 sxml_function_entry *entry) {
    *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    if (table.operations == NULL) {
        return SXML_STATUS_OK;
    }
    return sxml_function_table_find(table, query, entry);
}

/* The accumulator function a query names, or SXML_ACCUMULATOR_FUNCTION_COUNT. */
static accumulator_function_kind find_kind(const sxml_xslt_accumulator_table *table,
                                           const sxml_function_query *query) {
    if (query->arity != 1) {
        return SXML_ACCUMULATOR_FUNCTION_COUNT;
    }
    for (uint32_t kind_number = 0; kind_number < (uint32_t)SXML_ACCUMULATOR_FUNCTION_COUNT;
         kind_number += 1) {
        if (table->functions[kind_number].function_name.index == query->function_name.index) {
            return (accumulator_function_kind)kind_number;
        }
    }
    return SXML_ACCUMULATOR_FUNCTION_COUNT;
}

/* The table in front of the static contexts: the accumulator functions,
   then the table that was there. */
static sxml_status find_accumulator_function(void *state, const sxml_function_query *query,
                                             sxml_function_entry *entry) {
    sxml_xslt_accumulator_table *table = state;
    const accumulator_function_kind kind = find_kind(table, query);
    if (kind != SXML_ACCUMULATOR_FUNCTION_COUNT) {
        table_function *function = &table->functions[kind];
        const sxml_status status = declare_function(table, function);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        write_entry(function, query->arity, entry);
        return SXML_STATUS_OK;
    }
    return find_in_table(table->inner_table, query, entry);
}

static const sxml_function_table_operations accumulator_function_table_operations = {
    .find_function = find_accumulator_function};

/* The private functions of one accumulator and its rules. */
static const table_function *find_private(const sxml_xslt_accumulator_table *table,
                                          const sxml_function_query *query) {
    for (size_t rule_index = 0; rule_index < table->rule_count; rule_index += 1) {
        const accumulator_rule *rule = &table->rules[rule_index];
        const table_function *candidates[] = {&rule->match, &rule->rule};
        for (size_t candidate_index = 0; candidate_index < 2; candidate_index += 1) {
            if (candidates[candidate_index]->function_name.index == query->function_name.index) {
                return candidates[candidate_index];
            }
        }
    }
    for (size_t accumulator_index = 0; accumulator_index < table->accumulator_count;
         accumulator_index += 1) {
        const accumulator_definition *accumulator = &table->accumulators[accumulator_index];
        const table_function *candidates[] = {&accumulator->initial, &accumulator->start,
                                              &accumulator->end, &accumulator->close,
                                              &accumulator->build};
        for (size_t candidate_index = 0; candidate_index < 5; candidate_index += 1) {
            if (candidates[candidate_index]->function_name.index == query->function_name.index) {
                return candidates[candidate_index];
            }
        }
    }
    return NULL;
}

/* The private functions of the texts, then the internal rows, then the
   accumulator table. */
static sxml_status find_private_function(void *state, const sxml_function_query *query,
                                         sxml_function_entry *entry) {
    sxml_xslt_accumulator_table *table = state;
    const table_function *function = find_private(table, query);
    if (function != NULL && function->parameters.function_parameter_count == query->arity) {
        write_entry(function, query->arity, entry);
        return SXML_STATUS_OK;
    }
    const sxml_status status =
        find_in_table(sxml_function_library_as_table(table->library), query, entry);
    if (status != SXML_STATUS_OK || entry->entry_kind != SXML_FUNCTION_ENTRY_KIND_NONE) {
        return status;
    }
    return find_accumulator_function(state, query, entry);
}

static const sxml_function_table_operations private_function_table_operations = {
    .find_function = find_private_function};

/* ------------------------------------------------------------ Declaring */

/* Interns Q{ACCUMULATOR_FUNCTION_NAMESPACE_URI}prefix-number. */
typedef struct private_name_request {
    const char *prefix;
    size_t number;
} private_name_request;

static sxml_status make_private_name(sxml_xslt_accumulator_table *table,
                                     private_name_request request, sxml_name_identifier *name) {
    sxml_xslt_snippet_text text = {.allocator = table->allocator, .status = SXML_STATUS_OK};
    sxml_xslt_snippet_text_append(&text, sxml_byte_view_from_c_string(request.prefix));
    sxml_xslt_snippet_text_append_number(&text, request.number);
    sxml_status status = text.status;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            table->lowering->lowerer->name_pool,
            &(sxml_name_request){.namespace_uri = table->private_namespace,
                                 .local_name = sxml_xslt_snippet_text_view(&text)},
            name);
    }
    sxml_xslt_snippet_text_release(&text);
    return status;
}

/* A private function prefix-number of a shape. */
typedef struct private_function_request {
    const char *prefix;
    size_t number;
    sxml_function_parameter_view parameters;
    sxml_byte_view result_type;
} private_function_request;

static sxml_status prepare_private(sxml_xslt_accumulator_table *table,
                                   private_function_request request, table_function *function) {
    sxml_name_identifier name = {.index = 0};
    sxml_status status = make_private_name(
        table, (private_name_request){.prefix = request.prefix, .number = request.number}, &name);
    if (status == SXML_STATUS_OK) {
        status = prepare_function(table,
                                  (table_function_shape){.function_name = name,
                                                         .parameters = request.parameters,
                                                         .result_type = request.result_type,
                                                         .has_short_signature = false},
                                  function);
    }
    return status;
}

static bool is_true_attribute(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                              sxml_xslt_ast_attribute_kind kind) {
    const sxml_xslt_ast_attribute *attribute = find_attribute(lowering, node, kind);
    return attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
           attribute->value.payload.is_true;
}

/* The rules of the accumulator at accumulator_index, from its children
   (19.1: XTSE3355, XTSE3185). */
static sxml_status add_rules(sxml_xslt_accumulator_table *table, size_t accumulator_index) {
    const sxml_xslt_lowering *lowering = table->lowering;
    const sxml_xslt_ast_node_index declaration = table->accumulators[accumulator_index].node;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_xslt_ast_node_index child = node_of(lowering, declaration)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_ACCUMULATOR_RULE) {
            continue;
        }
        const sxml_xslt_ast_attribute *phase =
            find_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_PHASE);
        const bool is_end_phase = phase->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD &&
                                  phase->value.payload.keyword == SXML_XSLT_AST_KEYWORD_END;
        const bool captures = is_true_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_CAPTURE);
        if (captures && !is_end_phase) {
            return report_static_error(
                lowering, child,
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE3355 an xsl:accumulator-rule has capture=\"yes\" "
                                            "without phase=\"end\" (19.9)"));
        }
        const bool has_select =
            find_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_SELECT)->value.kind ==
            SXML_XSLT_AST_VALUE_KIND_EXPRESSION;
        if (has_select &&
            sxml_xslt_lowering_first_constructor_child(lowering->ast, child).value != 0) {
            return report_static_error(
                lowering, child,
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE3185 an xsl:accumulator-rule has both a select "
                                            "attribute and a sequence constructor (19.1)"));
        }
        const size_t rule_number = table->rule_count + 1;
        accumulator_rule *rule = &table->rules[table->rule_count];
        *rule = (accumulator_rule){.node = child,
                                   .accumulator_index = accumulator_index,
                                   .is_end_phase = is_end_phase,
                                   .captures = captures};
        table->rule_count += 1;
        status = prepare_private(
            table,
            (private_function_request){.prefix = "match-",
                                       .number = rule_number,
                                       .parameters = {item_parameters, 1},
                                       .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean")},
            &rule->match);
        if (status == SXML_STATUS_OK) {
            status = prepare_private(
                table,
                (private_function_request){.prefix = "rule-",
                                           .number = rule_number,
                                           .parameters = {node_value_parameters, 2},
                                           .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*")},
                &rule->rule);
        }
    }
    return status;
}

/* One private function of an accumulator to prepare. */
typedef struct private_shape {
    const char *prefix;
    sxml_function_parameter_view parameters;
    sxml_byte_view result_type;
    table_function *function;
} private_shape;

/* The private functions of the accumulator at accumulator_index. */
static sxml_status prepare_accumulator(sxml_xslt_accumulator_table *table,
                                       size_t accumulator_index) {
    accumulator_definition *accumulator = &table->accumulators[accumulator_index];
    const size_t number = accumulator_index + 1;
    const private_shape shapes[] = {
        {.prefix = "initial-",
         .parameters = {root_parameters, 1},
         .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
         .function = &accumulator->initial},
        {.prefix = "start-",
         .parameters = {node_value_parameters, 2},
         .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
         .function = &accumulator->start},
        {.prefix = "end-",
         .parameters = {node_value_parameters, 2},
         .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
         .function = &accumulator->end},
        {.prefix = "close-",
         .parameters = {state_nodes_parameters, 2},
         .result_type = SXML_BYTE_VIEW_INITIALIZER("map(*)"),
         .function = &accumulator->close},
        {.prefix = "build-",
         .parameters = {root_parameters, 1},
         .result_type = SXML_BYTE_VIEW_INITIALIZER("map(*)"),
         .function = &accumulator->build},
    };
    sxml_status status = SXML_STATUS_OK;
    for (size_t shape_index = 0;
         status == SXML_STATUS_OK && shape_index < sizeof(shapes) / sizeof(shapes[0]);
         shape_index += 1) {
        status = prepare_private(
            table,
            (private_function_request){.prefix = shapes[shape_index].prefix,
                                       .number = number,
                                       .parameters = shapes[shape_index].parameters,
                                       .result_type = shapes[shape_index].result_type},
            shapes[shape_index].function);
    }
    if (status == SXML_STATUS_OK) {
        status = add_rules(table, accumulator_index);
    }
    return status;
}

/* The declaration of name that wins (19.8): the highest import
   precedence; XTSE3350 for two at it. */
static sxml_status find_winner(const sxml_xslt_lowering *lowering, sxml_name_identifier name,
                               sxml_xslt_ast_node_index *winner) {
    const sxml_xslt_ast *ast = lowering->ast;
    *winner = (sxml_xslt_ast_node_index){.value = 0};
    bool is_tied = false;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, node)->kind != SXML_XSLT_AST_KIND_ACCUMULATOR ||
            name_of(lowering, node).index != name.index) {
            continue;
        }
        if (winner->value == 0 || precedence_of(lowering, node) > precedence_of(lowering, *winner)) {
            *winner = node;
            is_tied = false;
        } else if (precedence_of(lowering, node) == precedence_of(lowering, *winner)) {
            is_tied = true;
        }
    }
    if (is_tied) {
        return report_static_error(
            lowering, *winner,
            SXML_BYTE_VIEW_FROM_LITERAL("XTSE3350 two xsl:accumulator declarations of one name "
                                        "have the same import precedence, and none of a higher "
                                        "one (19.8)"));
    }
    return SXML_STATUS_OK;
}

static sxml_status collect_accumulators(sxml_xslt_accumulator_table *table) {
    const sxml_xslt_lowering *lowering = table->lowering;
    const sxml_xslt_ast *ast = lowering->ast;
    size_t declaration_count = 0;
    size_t rule_count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, node)->kind != SXML_XSLT_AST_KIND_ACCUMULATOR) {
            continue;
        }
        declaration_count += 1;
        for (sxml_xslt_ast_node_index child = node_of(lowering, node)->first_child;
             child.value != 0; child = node_of(lowering, child)->next_sibling) {
            rule_count += 1;
        }
    }
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        table->allocator,
        (zeroed_request){.element_count = declaration_count,
                         .element = {.byte_count = sizeof(accumulator_definition),
                                     .alignment = alignof(accumulator_definition)}},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->accumulators = memory;
    status = allocate_zeroed(table->allocator,
                             (zeroed_request){.element_count = rule_count,
                                              .element = {.byte_count = sizeof(accumulator_rule),
                                                          .alignment = alignof(accumulator_rule)}},
                             &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->rules = memory;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, node)->kind != SXML_XSLT_AST_KIND_ACCUMULATOR) {
            continue;
        }
        sxml_xslt_ast_node_index winner = {.value = 0};
        status = find_winner(lowering, name_of(lowering, node), &winner);
        if (status != SXML_STATUS_OK || winner.value != node.value) {
            continue;
        }
        table->accumulators[table->accumulator_count] =
            (accumulator_definition){.node = node, .accumulator_name = name_of(lowering, node)};
        table->accumulator_count += 1;
        status = prepare_accumulator(table, table->accumulator_count - 1);
    }
    return status;
}

/* The number (from 1) of the accumulator name; 0 when none. */
static size_t accumulator_number(const sxml_xslt_accumulator_table *table,
                                 sxml_name_identifier name) {
    for (size_t accumulator_index = 0; accumulator_index < table->accumulator_count;
         accumulator_index += 1) {
        if (table->accumulators[accumulator_index].accumulator_name.index == name.index) {
            return accumulator_index + 1;
        }
    }
    return 0;
}

/* The use-accumulators attribute of an xsl:mode (19.2, XTSE3300: names of
   accumulators, none twice, #all alone). */
static sxml_status check_use_accumulators(const sxml_xslt_accumulator_table *table,
                                          sxml_xslt_ast_node_index node) {
    const sxml_xslt_lowering *lowering = table->lowering;
    const sxml_xslt_ast_attribute *attribute =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_name_token_view tokens =
        sxml_xslt_ast_get_name_tokens(lowering->ast, attribute->value.payload.name_tokens);
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        const sxml_xslt_ast_name_token *token = &tokens.name_tokens[token_index];
        bool is_valid = token->kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME
                            ? accumulator_number(table, token->name) != 0
                            : token->kind == SXML_XSLT_AST_NAME_TOKEN_KIND_ALL &&
                                  tokens.name_token_count == 1;
        for (size_t earlier = 0; is_valid && earlier < token_index; earlier += 1) {
            is_valid = tokens.name_tokens[earlier].name.index != token->name.index;
        }
        if (!is_valid) {
            return report_static_error(
                lowering, node,
                SXML_BYTE_VIEW_FROM_LITERAL("XTSE3300 use-accumulators names something that is no "
                                            "accumulator, one twice, or #all with other names "
                                            "(19.2)"));
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status check_modes(const sxml_xslt_accumulator_table *table) {
    const sxml_xslt_ast *ast = table->lowering->ast;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(table->lowering, node)->kind == SXML_XSLT_AST_KIND_MODE) {
            status = check_use_accumulators(table, node);
        }
    }
    return status;
}

static sxml_status prepare_table(sxml_xslt_accumulator_table *table) {
    static const sxml_native_function_view *const parts[] = {&sxml_xslt_accumulator_functions};
    sxml_xpath_lowerer *lowerer = table->lowering->lowerer;
    sxml_status status = sxml_function_library_create(
        table->allocator,
        &(sxml_function_library_configuration){
            .allocator = table->allocator,
            .item_heap = lowerer->item_heap,
            .parts = {.native_function_views = parts, .native_function_view_count = 1}},
        &table->library);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            lowerer->name_pool, SXML_BYTE_VIEW_FROM_LITERAL(ACCUMULATOR_FUNCTION_NAMESPACE_URI),
            &table->private_namespace);
    }
    for (uint32_t kind_number = 0;
         status == SXML_STATUS_OK && kind_number < (uint32_t)SXML_ACCUMULATOR_FUNCTION_COUNT;
         kind_number += 1) {
        sxml_name_identifier name = {.index = 0};
        status = sxml_name_pool_intern_name(
            lowerer->name_pool,
            &(sxml_name_request){.namespace_uri =
                                     sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS),
                                 .local_name = accumulator_function_names[kind_number]},
            &name);
        if (status == SXML_STATUS_OK) {
            status = prepare_function(
                table,
                (table_function_shape){.function_name = name,
                                       .parameters = {accumulator_function_parameters, 2},
                                       .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
                                       .has_short_signature = true},
                &table->functions[kind_number]);
        }
    }
    for (size_t name_index = 0;
         status == SXML_STATUS_OK && name_index < (size_t)SXML_SNIPPET_VARIABLE_COUNT;
         name_index += 1) {
        status = sxml_name_pool_intern_name(
            lowerer->name_pool,
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = sxml_byte_view_from_c_string(
                                     snippet_variable_names[name_index])},
            &table->variable_names[name_index]);
    }
    return status;
}

sxml_status sxml_xslt_declare_accumulators(sxml_xslt_lowering *lowering,
                                           sxml_xpath_static_context *static_contexts,
                                           sxml_xslt_accumulator_table **table) {
    *table = NULL;
    const sxml_allocator allocator = lowering->lowerer->allocator;
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        allocator,
        (zeroed_request){.element_count = 1,
                         .element = {.byte_count = sizeof(sxml_xslt_accumulator_table),
                                     .alignment = alignof(sxml_xslt_accumulator_table)}},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_accumulator_table *made = memory;
    made->lowering = lowering;
    made->allocator = allocator;
    made->inner_table = lowering->static_context_count > 1
                            ? static_contexts[1].function_table
                            : lowering->lowerer->static_context->function_table;
    made->snippet_context = *lowering->lowerer->static_context;
    made->snippet_context.namespace_bindings =
        (sxml_namespace_binding_view){.namespace_bindings = NULL, .namespace_binding_count = 0};
    made->snippet_context.variable_declarations = (sxml_variable_declaration_view){
        .variable_declarations = NULL, .variable_declaration_count = 0};
    made->snippet_context.function_table =
        (sxml_function_table){.state = made, .operations = &private_function_table_operations};
    status = prepare_table(made);
    if (status == SXML_STATUS_OK) {
        status = collect_accumulators(made);
    }
    if (status == SXML_STATUS_OK) {
        status = check_modes(made);
    }
    if (status != SXML_STATUS_OK) {
        sxml_xslt_release_accumulators(made);
        return status;
    }
    for (uint32_t context = 1; context < lowering->static_context_count; context += 1) {
        static_contexts[context].function_table =
            (sxml_function_table){.state = made, .operations = &accumulator_function_table_operations};
    }
    *table = made;
    return SXML_STATUS_OK;
}

void sxml_xslt_release_accumulators(sxml_xslt_accumulator_table *table) {
    if (table == NULL) {
        return;
    }
    for (size_t snippet_index = 0; snippet_index < table->snippet_count; snippet_index += 1) {
        sxml_ast_destroy(table->snippets[snippet_index]);
    }
    sxml_allocator_release(table->allocator, (void *)table->snippets);
    sxml_function_library_destroy(table->library);
    sxml_allocator_release(table->allocator, table->accumulators);
    sxml_allocator_release(table->allocator, table->rules);
    sxml_allocator_release(table->allocator, table);
}

/* ------------------------------------------------------------- Lowering */

static sxml_ir_atom slot_atom(uint32_t slot) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = {.value = slot}};
}

/* Parses text and keeps the AST until release. */
static sxml_status parse_snippet(sxml_xslt_accumulator_table *table, sxml_byte_view text,
                                 sxml_ast **ast) {
    if (table->snippet_count == table->snippet_capacity) {
        const size_t capacity = table->snippet_capacity == 0 ? 8 : table->snippet_capacity * 2;
        void *memory = NULL;
        sxml_status status =
            allocate_zeroed(table->allocator,
                            (zeroed_request){.element_count = capacity,
                                             .element = {.byte_count = sizeof(sxml_ast *),
                                                         .alignment = alignof(sxml_ast *)}},
                            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (table->snippet_count > 0) {
            memcpy(memory, (const void *)table->snippets,
                   table->snippet_count * sizeof(sxml_ast *));
        }
        sxml_allocator_release(table->allocator, (void *)table->snippets);
        table->snippets = (sxml_ast **)memory;
        table->snippet_capacity = capacity;
    }
    const sxml_status status = sxml_xpath_build_ast(
        &(sxml_ast_build_request){.expression_text = text,
                                  .source_name = {.bytes = NULL, .byte_count = 0},
                                  .allocator = table->allocator,
                                  .maximum_expression_depth = 0,
                                  .error_record = table->lowering->error_record},
        ast);
    if (status == SXML_STATUS_OK) {
        table->snippets[table->snippet_count] = *ast;
        table->snippet_count += 1;
    }
    return status;
}

/* A body for a table function's IR function, after its frame is left
   (whose slot count it takes). */
typedef struct function_body {
    const table_function *function;
    const sxml_xpath_lowering_function_frame *frame;
    sxml_ir_expression_index body;
} function_body;

static sxml_status replace_body(const sxml_xslt_accumulator_table *table, function_body request) {
    sxml_xpath_lowerer *lowerer = table->lowering->lowerer;
    sxml_ir_function_replacement replacement = {
        .index = request.function->function,
        .function = *sxml_ir_unit_get_function(lowerer->unit, request.function->function)};
    replacement.function.body = request.body;
    replacement.function.slot_count = request.frame->function.slot_count;
    return sxml_ir_unit_replace_function(lowerer->unit, &replacement);
}

/* Pushes the variables of a function's parameters, slots 1.. (indices of
   table->variable_names). */
typedef struct parameter_variables {
    const snippet_variable *variables;
    size_t variable_count;
} parameter_variables;

static sxml_status push_parameters(const sxml_xslt_accumulator_table *table,
                                   parameter_variables parameters) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t parameter = 0; status == SXML_STATUS_OK && parameter < parameters.variable_count;
         parameter += 1) {
        status = sxml_xpath_lowerer_push_variable(
            table->lowering->lowerer,
            (sxml_xpath_lowering_scope_entry){
                .variable_name = table->variable_names[parameters.variables[parameter]],
                .function_level = 0,
                .atom = slot_atom((uint32_t)parameter + 1)});
    }
    return status;
}

/* A function whose body is a text made here, its parameters bound to
   variables. */
typedef struct snippet_function {
    const table_function *function;
    sxml_byte_view text;
    parameter_variables parameters;
} snippet_function;

static sxml_status lower_snippet_function(sxml_xslt_accumulator_table *table,
                                          snippet_function request) {
    sxml_xpath_lowerer *lowerer = table->lowering->lowerer;
    sxml_ast *ast = NULL;
    sxml_status status = parse_snippet(table, request.text, &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t parameter_count =
        (uint32_t)request.function->parameters.function_parameter_count;
    if (request.parameters.variable_count != parameter_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = request.function->function,
                     .function_level = 0,
                     .slot_count = parameter_count,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &frame);
    status = push_parameters(table, request.parameters);
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_lower_embedded(
            lowerer,
            &(sxml_xpath_lowering_embedding){.ast = ast,
                                             .node = {.value = 0},
                                             .static_context = &table->snippet_context,
                                             .focus = NULL},
            &body);
    }
    sxml_xpath_lowerer_leave_function(lowerer, &frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return replace_body(
        table, (function_body){.function = request.function, .frame = &frame, .body = body});
}

/* An expression to add without an AST node. */
static sxml_status add_expression(sxml_xpath_lowerer *lowerer, sxml_ir_expression expression,
                                  sxml_ir_expression_index *index) {
    return sxml_xpath_lowerer_add_expression(
        lowerer,
        (sxml_xpath_lowering_expression_to_add){.node = no_ast_node, .expression = expression},
        index);
}

/* match-N: the pattern's match function inside a TRY_CATCH whose one
   clause catches every error and gives false (6.5, 19.5). */
static sxml_status lower_match_wrapper(sxml_xslt_accumulator_table *table,
                                       const accumulator_rule *rule) {
    const sxml_xslt_lowering *lowering = table->lowering;
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    const sxml_xslt_ast_attribute *match =
        find_attribute(lowering, rule->node, SXML_XSLT_AST_ATTRIBUTE_MATCH);
    if (match->value.kind != SXML_XSLT_AST_VALUE_KIND_PATTERN) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ir_function_index pattern_function = {0};
    sxml_status status = sxml_xslt_pattern_lower_match_function(
        lowerer,
        &(sxml_xslt_pattern_match_function_request){
            .pattern_ast =
                sxml_xslt_ast_get_expression(lowering->ast, match->value.payload.expression)->ast,
            .static_context = expression_context(lowering, match->value.payload.expression)},
        &pattern_function);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = rule->match.function,
                     .function_level = 0,
                     .slot_count = 1,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &frame);
    const sxml_ir_atom item = slot_atom(1);
    sxml_ir_atom_list arguments = {0};
    status = sxml_ir_unit_add_atoms(
        lowerer->unit, (sxml_ir_atom_view){.atoms = &item, .atom_count = 1}, &arguments);
    sxml_ir_expression_index call = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            lowerer,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                .static_call = {.target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                           .function = pattern_function},
                                .arguments = arguments}},
            &call);
    }
    sxml_ir_expression guarded = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_make_guarded_match(lowerer, call, &guarded);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(lowerer, guarded, &body);
    }
    sxml_xpath_lowerer_leave_function(lowerer, &frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return replace_body(table,
                        (function_body){.function = &rule->match, .frame = &frame, .body = body});
}

/* The sequence constructor of an xsl:accumulator-rule as a body, in a
   machine. */
typedef struct rule_content_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
} rule_content_locals;

static sxml_status continue_rule_content(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame) {
    rule_content_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = locals->head.lowering,
                         .first = sxml_xslt_lowering_first_constructor_child(
                             locals->head.lowering->ast, locals->head.node),
                         .block = frame->block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &locals->value,
                                         .atom_block = frame->block,
                                         .focused = NULL},
                         .focus = NULL});
    }
    sxml_ir_expression_index result = {0};
    const sxml_status status = add_expression(
        machine->state,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = locals->value},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

static const sxml_xpath_lowering_procedure rule_content_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:accumulator-rule"),
    .step = continue_rule_content,
    .locals_size = sizeof(rule_content_locals)};

/* The as type of an accumulator (19.1, default item()*); has_type false
   when absent. */
typedef struct value_type {
    bool has_type;
    sxml_sequence_type sequence_type;
} value_type;

static sxml_status resolve_value_type(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_node_index node, value_type *type) {
    *type = (value_type){.has_type = false};
    const sxml_xslt_ast_attribute *as_attribute =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_AS);
    if (as_attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, as_attribute->value.payload.expression);
    const sxml_xpath_static_context *context =
        expression_context(lowering, as_attribute->value.payload.expression);
    if (expression->ast == NULL || context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    type->has_type = true;
    return sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = expression->ast,
                                   .type_node = expression->ast->root,
                                   .static_context = context,
                                   .error_record = lowering->error_record},
        &type->sequence_type);
}

/* A value function: an expression (the initial-value, or a rule's select)
   or a sequence constructor (a rule's content), with slot 1 as its fixed
   focus and, for a rule, $value as slot 2, coerced to the as type. */
typedef struct value_function {
    const table_function *function;
    /* The xsl:accumulator (its as attribute). */
    sxml_xslt_ast_node_index accumulator;
    /* The element whose expression attribute or content is the value. */
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_attribute_kind attribute;
} value_function;

static sxml_status lower_value_function(sxml_xslt_accumulator_table *table,
                                        value_function request) {
    sxml_xslt_lowering *lowering = table->lowering;
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    value_type type = {.has_type = false};
    sxml_status status = resolve_value_type(lowering, request.accumulator, &type);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t parameter_count =
        (uint32_t)request.function->parameters.function_parameter_count;
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = request.function->function,
                     .function_level = 0,
                     .slot_count = parameter_count,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED, .fixed_value = slot_atom(1)},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &frame);
    if (parameter_count == 2) {
        /* 19.1: $value (no namespace) is the accumulator's value. */
        status = sxml_xpath_lowerer_push_variable(
            lowerer,
            (sxml_xpath_lowering_scope_entry){
                .variable_name = table->variable_names[SXML_SNIPPET_VARIABLE_VALUE],
                .function_level = 0,
                .atom = slot_atom(2)});
    }
    const sxml_xslt_ast_attribute *attribute =
        find_attribute(lowering, request.node, request.attribute);
    sxml_ir_expression_index value = {0};
    if (status != SXML_STATUS_OK) {
        /* Left as it is. */
    } else if (attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
        status = sxml_xpath_lowerer_lower_embedded(
            lowerer,
            &(sxml_xpath_lowering_embedding){
                .ast = sxml_xslt_ast_get_expression(lowering->ast, attribute->value.payload.expression)
                           ->ast,
                .node = {.value = 0},
                .static_context = expression_context(lowering, attribute->value.payload.expression),
                .focus = NULL},
            &value);
    } else {
        sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
        const sxml_xslt_instruction_locals initial = {.lowering = lowering, .node = request.node};
        status = sxml_xpath_lowerer_run_procedure(
            lowerer, &(sxml_xpath_lowering_procedure_call){
                         .procedure = &rule_content_procedure,
                         .node = no_ast_node,
                         .block = &block,
                         .is_body = false,
                         .destination = {.expression = &value,
                                         .atom = NULL,
                                         .atom_block = NULL,
                                         .focused = NULL},
                         .initial_locals = {.bytes = (const uint8_t *)&initial,
                                            .byte_count = sizeof(initial)}});
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_finish_block(
                lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = value},
                &value);
        }
    }
    sxml_ir_expression_index body = value;
    if (status == SXML_STATUS_OK && type.has_type) {
        /* 19.1: the value is coerced to the as type (XPTY0004). */
        sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
        sxml_ir_atom atom = {0};
        status = sxml_xpath_lowerer_bind_expression(
            lowerer, (sxml_xpath_lowering_expression_in_block){.block = &block, .expression = value},
            &atom);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_coerce_atom(
                lowerer, (sxml_xpath_lowering_coercion_request){
                             .block = &block,
                             .node = no_ast_node,
                             .value = atom,
                             .sequence_type = type.sequence_type,
                             .role = SXML_IR_COERCION_ROLE_VARIABLE},
                &atom);
        }
        sxml_ir_expression_index result = {0};
        if (status == SXML_STATUS_OK) {
            status = add_expression(
                lowerer,
                (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = atom},
                &result);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_finish_block(
                lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = result},
                &body);
        }
    }
    sxml_xpath_lowerer_leave_function(lowerer, &frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return replace_body(
        table, (function_body){.function = request.function, .frame = &frame, .body = body});
}

/* The text of start-K or end-K: the rules of the phase, last first. */
typedef struct phase_text_request {
    size_t accumulator_index;
    bool is_end_phase;
} phase_text_request;

static void write_phase_text(const sxml_xslt_accumulator_table *table, phase_text_request request,
                             sxml_xslt_snippet_text *text) {
    for (size_t rule_count = table->rule_count; rule_count > 0; rule_count -= 1) {
        const accumulator_rule *rule = &table->rules[rule_count - 1];
        if (rule->accumulator_index != request.accumulator_index ||
            rule->is_end_phase != request.is_end_phase) {
            continue;
        }
        SXML_XSLT_SNIPPET_APPEND(text, "if (");
        append_private_call(text, (private_call){.prefix = "match-", .number = rule_count});
        SXML_XSLT_SNIPPET_APPEND(text, "$node)) then ");
        append_private_call(text, (private_call){.prefix = "rule-", .number = rule_count});
        if (rule->captures) {
            /* 19.9: an element is seen as a snapshot of itself. */
            SXML_XSLT_SNIPPET_APPEND(
                text, "if ($node instance of element()) then snapshot($node) else $node");
        } else {
            SXML_XSLT_SNIPPET_APPEND(text, "$node");
        }
        SXML_XSLT_SNIPPET_APPEND(text, ", $value) else ");
    }
    SXML_XSLT_SNIPPET_APPEND(text, "$value");
}

static void write_close_text(size_t number, sxml_xslt_snippet_text *text) {
    SXML_XSLT_SNIPPET_APPEND(text, "fold-left($nodes, $state, fn($inner, $done) { let $value := ");
    append_private_call(text, (private_call){.prefix = "end-", .number = number});
    SXML_XSLT_SNIPPET_APPEND(
        text, "$done, $inner?v) return "
              "Q{http://www.w3.org/2005/xpath-functions/map}put("
              "Q{http://www.w3.org/2005/xpath-functions/map}put($inner, \"v\", $value), "
              "\"a\", Q{http://www.w3.org/2005/xpath-functions/map}put($inner?a, "
              "generate-id($done), $value)) })");
}

static void write_build_text(size_t number, sxml_xslt_snippet_text *text) {
    SXML_XSLT_SNIPPET_APPEND(text, "let $final := fold-left($root/descendant-or-self::node(), "
                                   "map { \"v\": ");
    append_private_call(text, (private_call){.prefix = "initial-", .number = number});
    SXML_XSLT_SNIPPET_APPEND(text,
                             "$root), \"s\": (), \"b\": map { }, \"a\": map { } }, "
                             "fn($state, $node) { let $stack := $state?s, $count := count($stack), "
                             "$parent := $node/.., $depth := if (empty($parent)) then 0 "
                             "else if ($stack[$count] is $parent) then $count "
                             "else if ($stack[$count - 1] is $parent) then $count - 1 "
                             "else count($node/ancestor::node()), "
                             "$closed := ");
    append_private_call(text, (private_call){.prefix = "close-", .number = number});
    SXML_XSLT_SNIPPET_APPEND(text, "$state, reverse(subsequence($stack, $depth + 1))), $value := ");
    append_private_call(text, (private_call){.prefix = "start-", .number = number});
    SXML_XSLT_SNIPPET_APPEND(text,
                             "$node, $closed?v) return map { \"v\": $value, "
                             "\"s\": (subsequence($closed?s, 1, $depth), $node), "
                             "\"b\": Q{http://www.w3.org/2005/xpath-functions/map}put($closed?b, "
                             "generate-id($node), $value), \"a\": $closed?a } }) return ");
    append_private_call(text, (private_call){.prefix = "close-", .number = number});
    SXML_XSLT_SNIPPET_APPEND(text, "$final, reverse($final?s))");
}

/* A text function of one accumulator. */
typedef struct accumulator_text {
    const table_function *function;
    sxml_xslt_snippet_text *text;
    parameter_variables parameters;
} accumulator_text;

static sxml_status lower_text(sxml_xslt_accumulator_table *table, accumulator_text request) {
    sxml_status status = request.text->status;
    if (status == SXML_STATUS_OK) {
        status = lower_snippet_function(
            table, (snippet_function){.function = request.function,
                                      .text = sxml_xslt_snippet_text_view(request.text),
                                      .parameters = request.parameters});
    }
    sxml_xslt_snippet_text_release(request.text);
    return status;
}

/* The functions of the accumulator at accumulator_index. */
static sxml_status lower_accumulator(sxml_xslt_accumulator_table *table,
                                     size_t accumulator_index) {
    const accumulator_definition *accumulator = &table->accumulators[accumulator_index];
    const size_t number = accumulator_index + 1;
    static const snippet_variable node_value[] = {SXML_SNIPPET_VARIABLE_NODE,
                                                  SXML_SNIPPET_VARIABLE_VALUE};
    static const snippet_variable state_nodes[] = {SXML_SNIPPET_VARIABLE_STATE,
                                                   SXML_SNIPPET_VARIABLE_NODES};
    static const snippet_variable root[] = {SXML_SNIPPET_VARIABLE_ROOT};
    sxml_status status = lower_value_function(
        table, (value_function){.function = &accumulator->initial,
                                .accumulator = accumulator->node,
                                .node = accumulator->node,
                                .attribute = SXML_XSLT_AST_ATTRIBUTE_INITIAL_VALUE});
    for (uint32_t phase = 0; status == SXML_STATUS_OK && phase < 2; phase += 1) {
        sxml_xslt_snippet_text text = {.allocator = table->allocator, .status = SXML_STATUS_OK};
        write_phase_text(table,
                         (phase_text_request){.accumulator_index = accumulator_index,
                                              .is_end_phase = phase == 1},
                         &text);
        status = lower_text(
            table, (accumulator_text){.function = phase == 1 ? &accumulator->end : &accumulator->start,
                                      .text = &text,
                                      .parameters = {node_value, 2}});
    }
    if (status == SXML_STATUS_OK) {
        sxml_xslt_snippet_text text = {.allocator = table->allocator, .status = SXML_STATUS_OK};
        write_close_text(number, &text);
        status = lower_text(table, (accumulator_text){.function = &accumulator->close,
                                                      .text = &text,
                                                      .parameters = {state_nodes, 2}});
    }
    if (status == SXML_STATUS_OK) {
        sxml_xslt_snippet_text text = {.allocator = table->allocator, .status = SXML_STATUS_OK};
        write_build_text(number, &text);
        status = lower_text(table, (accumulator_text){.function = &accumulator->build,
                                                      .text = &text,
                                                      .parameters = {root, 1}});
    }
    return status;
}

/* Every accumulator's functions, once: declared first, since each text
   may name any of them. */
static sxml_status lower_all_accumulators(sxml_xslt_accumulator_table *table) {
    if (table->are_accumulators_lowered) {
        return SXML_STATUS_OK;
    }
    table->are_accumulators_lowered = true;
    sxml_status status = SXML_STATUS_OK;
    for (size_t rule_index = 0; status == SXML_STATUS_OK && rule_index < table->rule_count;
         rule_index += 1) {
        status = declare_function(table, &table->rules[rule_index].match);
        if (status == SXML_STATUS_OK) {
            status = declare_function(table, &table->rules[rule_index].rule);
        }
    }
    for (size_t accumulator_index = 0;
         status == SXML_STATUS_OK && accumulator_index < table->accumulator_count;
         accumulator_index += 1) {
        accumulator_definition *accumulator = &table->accumulators[accumulator_index];
        table_function *functions[] = {&accumulator->initial, &accumulator->start,
                                       &accumulator->end, &accumulator->close, &accumulator->build};
        for (size_t function_index = 0; status == SXML_STATUS_OK && function_index < 5;
             function_index += 1) {
            status = declare_function(table, functions[function_index]);
        }
    }
    for (size_t rule_index = 0; status == SXML_STATUS_OK && rule_index < table->rule_count;
         rule_index += 1) {
        const accumulator_rule *rule = &table->rules[rule_index];
        status = lower_match_wrapper(table, rule);
        if (status == SXML_STATUS_OK) {
            status = lower_value_function(
                table, (value_function){.function = &rule->rule,
                                        .accumulator =
                                            table->accumulators[rule->accumulator_index].node,
                                        .node = rule->node,
                                        .attribute = SXML_XSLT_AST_ATTRIBUTE_SELECT});
        }
    }
    for (size_t accumulator_index = 0;
         status == SXML_STATUS_OK && accumulator_index < table->accumulator_count;
         accumulator_index += 1) {
        status = lower_accumulator(table, accumulator_index);
    }
    return status;
}

/* The text of fn:accumulator-before or fn:accumulator-after (see the file
   comment). */
static void write_function_text(const sxml_xslt_accumulator_table *table,
                                accumulator_function_kind kind, sxml_xslt_snippet_text *text) {
    SXML_XSLT_SNIPPET_APPEND(
        text, "if (not($context instance of node()) or $context instance of attribute() "
              "or $context instance of namespace-node()) then "
              "error(QName(\"http://www.w3.org/2005/xqt-errors\", \"XTTE3360\"), "
              "\"fn:accumulator-before or fn:accumulator-after is called where the context "
              "item is not a node, or is an attribute or namespace node (XSLT 4.0, "
              "19.6)\") else let $name := Q{}accumulator-name($name), "
              /* 19.10: in a copy, the node it copies. */
              "$anchor := ($context/ancestor-or-self::node()"
              "[exists(Q{}accumulator-original(.))])[last()], "
              "$node := if (empty($anchor)) then $context "
              "else (Q{}accumulator-original($anchor)/descendant-or-self::node())"
              "[index-where($anchor/descendant-or-self::node(), "
              "fn($candidate) { $candidate is $context })], "
              "$root := root($node), $id := generate-id($node) return ");
    for (size_t accumulator_index = 0; accumulator_index < table->accumulator_count;
         accumulator_index += 1) {
        SXML_XSLT_SNIPPET_APPEND(text, "if ($name eq ");
        sxml_xslt_snippet_text_append_qname(
            text, table->lowering->lowerer->name_pool,
            table->accumulators[accumulator_index].accumulator_name);
        SXML_XSLT_SNIPPET_APPEND(text, ") then Q{}accumulator-values(");
        sxml_xslt_snippet_text_append_number(text, accumulator_index + 1);
        SXML_XSLT_SNIPPET_APPEND(text, ", $root, " PRIVATE_PREFIX "build-");
        sxml_xslt_snippet_text_append_number(text, accumulator_index + 1);
        sxml_xslt_snippet_text_append(text, kind == SXML_ACCUMULATOR_FUNCTION_AFTER
                                                ? SXML_BYTE_VIEW_FROM_LITERAL("#1)?a?($id) else ")
                                                : SXML_BYTE_VIEW_FROM_LITERAL("#1)?b?($id) else "));
    }
    SXML_XSLT_SNIPPET_APPEND(
        text, "error(QName(\"http://www.w3.org/2005/xqt-errors\", \"XTDE3340\"), "
              "\"no xsl:accumulator declares the name given to fn:accumulator-before or "
              "fn:accumulator-after (XSLT 4.0, 19.6)\")");
}

static sxml_status lower_accumulator_function(sxml_xslt_accumulator_table *table,
                                              accumulator_function_kind kind) {
    static const snippet_variable name_context[] = {SXML_SNIPPET_VARIABLE_NAME,
                                                    SXML_SNIPPET_VARIABLE_CONTEXT};
    sxml_status status = lower_all_accumulators(table);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_snippet_text text = {.allocator = table->allocator, .status = SXML_STATUS_OK};
    write_function_text(table, kind, &text);
    return lower_text(table, (accumulator_text){.function = &table->functions[kind],
                                                .text = &text,
                                                .parameters = {name_context, 2}});
}

sxml_status sxml_xslt_lower_accumulators(sxml_xslt_accumulator_table *table) {
    if (table == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    bool is_lowered[SXML_ACCUMULATOR_FUNCTION_COUNT] = {false, false};
    /* A rule may name the other function: until none is left unlowered. */
    bool has_lowered = true;
    while (status == SXML_STATUS_OK && has_lowered) {
        has_lowered = false;
        for (uint32_t kind_number = 0;
             status == SXML_STATUS_OK && kind_number < (uint32_t)SXML_ACCUMULATOR_FUNCTION_COUNT;
             kind_number += 1) {
            const table_function *function = &table->functions[kind_number];
            if (function->function.value == 0 || is_lowered[kind_number] ||
                sxml_ir_unit_get_function(table->lowering->lowerer->unit, function->function)
                        ->body.value != 0) {
                continue;
            }
            is_lowered[kind_number] = true;
            has_lowered = true;
            status = lower_accumulator_function(table, (accumulator_function_kind)kind_number);
        }
    }
    return status;
}

/* ------------------------------------------------------------ Copies */

/* The internal row of local_name; NULL when there is none. */
static const sxml_native_function *find_row(sxml_byte_view local_name) {
    for (size_t row_index = 0; row_index < sxml_xslt_accumulator_functions.native_function_count;
         row_index += 1) {
        const sxml_native_function *row =
            &sxml_xslt_accumulator_functions.native_functions[row_index];
        if (sxml_byte_view_equals(row->local_name, local_name)) {
            return row;
        }
    }
    return NULL;
}

/* A call of an internal row with arguments of its types, bound in block. */
typedef struct row_call {
    sxml_xpath_lowering_block *block;
    sxml_byte_view local_name;
    sxml_ir_atom_view arguments;
} row_call;

static sxml_status bind_row_call(sxml_xpath_lowerer *lowerer, row_call call, sxml_ir_atom *atom) {
    const sxml_native_function *row = find_row(call.local_name);
    if (row == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ir_atom_list arguments = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(lowerer->unit, call.arguments, &arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_bind_expression(
        lowerer,
        &(sxml_xslt_expression_to_bind){
            .block = call.block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                           .static_call = {.target = {.target_kind =
                                                          SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                      .native_function = row},
                                           .arguments = arguments}}},
        atom);
}

bool sxml_xslt_has_accumulators(const sxml_xslt_lowering *lowering) {
    return lowering->accumulators != NULL && lowering->accumulators->accumulator_count > 0;
}

sxml_status sxml_xslt_bind_accumulator_link(sxml_xpath_lowerer *lowerer,
                                            sxml_xslt_accumulator_link_request request,
                                            sxml_ir_atom *atom) {
    const sxml_ir_atom arguments[2] = {request.copies, request.originals};
    return bind_row_call(lowerer,
                         (row_call){.block = request.block,
                                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("accumulator-link"),
                                    .arguments = {.atoms = arguments, .atom_count = 2}},
                         atom);
}

/* ----------------------------------------------------------- The scope */

/* The name of the initial mode (2.4.3): the one asked for, else the
   default mode of the principal module (3.6.3); 0 for the unnamed mode. */
static sxml_name_identifier initial_mode_name(const sxml_xslt_lowering *lowering,
                                              const sxml_xslt_invocation *invocation) {
    if (invocation->selects_unnamed_mode) {
        return (sxml_name_identifier){.index = 0};
    }
    if (invocation->initial_mode.index != 0) {
        return invocation->initial_mode;
    }
    const sxml_xslt_ast_node_index root =
        sxml_xslt_ast_get_module(lowering->ast, (sxml_xslt_ast_module_index){.value = 1})->root;
    const sxml_xslt_ast_static_context *context =
        sxml_xslt_ast_get_static_context(lowering->ast, node_of(lowering, root)->static_context);
    if (context->default_mode.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME) {
        return context->default_mode.name;
    }
    return (sxml_name_identifier){.index = 0};
}

/* The xsl:mode of mode_name with use-accumulators of the highest import
   precedence; 0 when none. */
static sxml_xslt_ast_node_index find_mode_declaration(const sxml_xslt_lowering *lowering,
                                                      sxml_name_identifier mode_name) {
    const sxml_xslt_ast *ast = lowering->ast;
    sxml_xslt_ast_node_index found = {.value = 0};
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, node)->kind != SXML_XSLT_AST_KIND_MODE ||
            name_of(lowering, node).index != mode_name.index ||
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS)->value.kind !=
                SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
            continue;
        }
        if (found.value == 0 || precedence_of(lowering, node) >= precedence_of(lowering, found)) {
            found = node;
        }
    }
    return found;
}

/* Whether the accumulator at accumulator_index is in the tokens of a
   use-accumulators attribute (19.2): named, or #all; with streamed, only
   one declared streamable="yes". */
typedef struct scope_test {
    sxml_xslt_ast_name_token_view tokens;
    size_t accumulator_index;
    bool is_streamed;
} scope_test;

static bool is_in_scope(const sxml_xslt_accumulator_table *table, scope_test test) {
    const accumulator_definition *accumulator = &table->accumulators[test.accumulator_index];
    if (test.is_streamed &&
        !is_true_attribute(table->lowering, accumulator->node, SXML_XSLT_AST_ATTRIBUTE_STREAMABLE)) {
        return false;
    }
    for (size_t token_index = 0; token_index < test.tokens.name_token_count; token_index += 1) {
        const sxml_xslt_ast_name_token *token = &test.tokens.name_tokens[token_index];
        if (token->kind == SXML_XSLT_AST_NAME_TOKEN_KIND_ALL ||
            (token->kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME &&
             token->name.index == accumulator->accumulator_name.index)) {
            return true;
        }
    }
    return false;
}

/* Where the accumulator scope of a tree comes from. */
typedef struct scope_source {
    const sxml_xslt_accumulator_table *table;
    sxml_xpath_lowering_block *block;
    /* The root of the tree. */
    sxml_ir_atom root;
    /* The element with use-accumulators (xsl:mode, xsl:source-document);
       0 for none, an empty list. */
    sxml_xslt_ast_node_index node;
    bool is_streamed;
} scope_source;

/* Binds accumulator-scope(root, numbers) in block and writes its atom. */
static sxml_status bind_scope(sxml_xpath_lowerer *lowerer, scope_source source,
                              sxml_ir_atom *atom) {
    const sxml_xslt_accumulator_table *table = source.table;
    sxml_xslt_ast_name_token_view tokens = {.name_tokens = NULL, .name_token_count = 0};
    if (source.node.value != 0) {
        const sxml_xslt_ast_attribute *attribute =
            find_attribute(table->lowering, source.node, SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS);
        if (attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
            tokens = sxml_xslt_ast_get_name_tokens(table->lowering->ast,
                                                  attribute->value.payload.name_tokens);
        }
    }
    void *memory = NULL;
    sxml_status status =
        allocate_zeroed(table->allocator,
                        (zeroed_request){.element_count = table->accumulator_count,
                                         .element = {.byte_count = sizeof(sxml_ir_atom),
                                                     .alignment = alignof(sxml_ir_atom)}},
                        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_atom *numbers = memory;
    size_t number_count = 0;
    for (size_t accumulator_index = 0;
         status == SXML_STATUS_OK && accumulator_index < table->accumulator_count;
         accumulator_index += 1) {
        if (is_in_scope(table, (scope_test){.tokens = tokens,
                                            .accumulator_index = accumulator_index,
                                            .is_streamed = source.is_streamed})) {
            status = sxml_xpath_lowerer_add_item_constant(
                lowerer, sxml_atomic_item_from_integer((int64_t)accumulator_index + 1),
                &numbers[number_count]);
            number_count += 1;
        }
    }
    sxml_ir_atom_list list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit, (sxml_ir_atom_view){.atoms = numbers, .atom_count = number_count},
            &list);
    }
    sxml_allocator_release(table->allocator, memory);
    sxml_ir_atom number_sequence = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_expression(
            lowerer,
            &(sxml_xslt_expression_to_bind){
                .block = source.block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                               .sequence = list}},
            &number_sequence);
    }
    const sxml_ir_atom arguments[2] = {source.root, number_sequence};
    if (status == SXML_STATUS_OK) {
        status = bind_row_call(lowerer,
                               (row_call){.block = source.block,
                                          .local_name =
                                              SXML_BYTE_VIEW_FROM_LITERAL("accumulator-scope"),
                                          .arguments = {.atoms = arguments, .atom_count = 2}},
                               atom);
    }
    return status;
}

sxml_status sxml_xslt_bind_accumulator_scope(sxml_xpath_lowerer *lowerer,
                                             const sxml_xslt_accumulator_scope_call *call) {
    const sxml_xslt_accumulator_table *table = call->table;
    if (table == NULL || table->accumulator_count == 0 || call->invocation.template_node.value != 0 ||
        call->invocation.is_unknown_template) {
        return SXML_STATUS_OK;
    }
    /* if (has a focus) then (if (. instance of node()) then
       accumulator-scope(root(.), numbers) else ()) else () */
    sxml_ir_atom has_focus = {0};
    sxml_status status = sxml_xslt_lowering_bind_has_focus(lowerer, call->block, &has_focus);
    sxml_xpath_lowering_block then_block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom item = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_read_focus(
            lowerer,
            (sxml_xpath_lowering_focus_read){.block = &then_block,
                                             .node = no_ast_node,
                                             .part = SXML_XPATH_LOWERING_FOCUS_PART_VALUE},
            &item);
    }
    sxml_ir_atom is_node = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_expression(
            lowerer,
            &(sxml_xslt_expression_to_bind){
                .block = &then_block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_INSTANCE_OF,
                               .type_test = {.value = item,
                                             .sequence_type =
                                                 {.item_type = sxml_builtin_type_identifier(
                                                      SXML_TYPE_NODE),
                                                  .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}}}},
            &is_node);
    }
    sxml_xpath_lowering_block scope_block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom root = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_library_call(
            lowerer, &(sxml_xslt_library_call){.block = &scope_block,
                                               .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                               .local_name = SXML_BYTE_VIEW_FROM_LITERAL("root"),
                                               .arguments = {.atoms = &item, .atom_count = 1}},
            &root);
    }
    sxml_ir_atom scope = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_scope(
            lowerer,
            (scope_source){.table = table,
                           .block = &scope_block,
                           .root = root,
                           .node = find_mode_declaration(
                               table->lowering, initial_mode_name(table->lowering, &call->invocation)),
                           .is_streamed = false},
            &scope);
    }
    /* Each branch its own empty sequence: an expression is reached once. */
    sxml_ir_expression_index nothing[2] = {{0}, {0}};
    for (size_t branch = 0; status == SXML_STATUS_OK && branch < 2; branch += 1) {
        status = add_expression(lowerer,
                                (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE},
                                &nothing[branch]);
    }
    sxml_ir_expression_index scope_expression = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            lowerer, (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = scope},
            &scope_expression);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer,
            (sxml_xpath_lowering_block_result){.block = scope_block, .result = scope_expression},
            &scope_expression);
    }
    sxml_ir_expression_index node_test = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                 .conditional = {.condition = is_node,
                                                 .then_branch = scope_expression,
                                                 .else_branch = nothing[0]}},
            &node_test);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer, (sxml_xpath_lowering_block_result){.block = then_block, .result = node_test},
            &node_test);
    }
    sxml_ir_expression_index conditional = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                 .conditional = {.condition = has_focus,
                                                 .then_branch = node_test,
                                                 .else_branch = nothing[1]}},
            &conditional);
    }
    sxml_ir_atom bound = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = call->block,
                                                      .expression = conditional},
            &bound);
    }
    return status;
}

/* ------------------------------------------------- xsl:source-document */

/* xsl:source-document (18.1) without streaming: the document fn:doc gives
   for href, its accumulator scope the use-accumulators attribute (19.2:
   none when absent; with streamable="yes" only accumulators declared
   streamable), then the sequence constructor with the document node as
   singleton focus. */
typedef struct source_document_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom href;
    sxml_ir_atom document;
    sxml_ir_atom value;
    sxml_xpath_lowering_focus document_focus;
} source_document_locals;

static sxml_status continue_source_document(sxml_xpath_lowering_machine *machine,
                                            sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    source_document_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_xslt_ast_attribute *href =
            find_attribute(lowering, locals->head.node, SXML_XSLT_AST_ATTRIBUTE_HREF);
        if (href->value.kind != SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        return sxml_xslt_lowering_call_value_template(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = href->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->href,
                                                  .focus = NULL});
    }
    if (frame->step == 1) {
        frame->step = 2;
        sxml_status status = SXML_STATUS_OK;
        if (lowering->accumulators != NULL) {
            status = check_use_accumulators(lowering->accumulators, locals->head.node);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_bind_library_call(
                lowerer, &(sxml_xslt_library_call){.block = frame->block,
                                                   .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                                   .local_name = SXML_BYTE_VIEW_FROM_LITERAL("doc"),
                                                   .arguments = {.atoms = &locals->href,
                                                                 .atom_count = 1}},
                &locals->document);
        }
        sxml_ir_atom scope = {0};
        if (status == SXML_STATUS_OK && lowering->accumulators != NULL &&
            lowering->accumulators->accumulator_count > 0) {
            status = bind_scope(
                lowerer,
                (scope_source){.table = lowering->accumulators,
                               .block = frame->block,
                               .root = locals->document,
                               .node = locals->head.node,
                               .is_streamed = is_true_attribute(lowering, locals->head.node,
                                                                SXML_XSLT_AST_ATTRIBUTE_STREAMABLE)},
                &scope);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->document_focus = (sxml_xpath_lowering_focus){
            .kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED, .fixed_value = locals->document};
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
                         .focus = &locals->document_focus});
    }
    sxml_ir_expression_index result = {0};
    const sxml_status status = add_expression(
        lowerer,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = locals->value},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

static const sxml_xpath_lowering_procedure source_document_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:source-document"),
    .step = continue_source_document,
    .locals_size = sizeof(source_document_locals)};

static const sxml_xslt_instruction_rule accumulator_rules[] = {
    {.kind = SXML_XSLT_AST_KIND_SOURCE_DOCUMENT, .procedure = &source_document_procedure},
};

const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_accumulator = {
    .rules = accumulator_rules,
    .rule_count = sizeof(accumulator_rules) / sizeof(accumulator_rules[0])};
