/*
 * XSLT 4.0 patterns: the front end, match expressions, match functions and
 * default priorities (src/xslt_pattern_internal.h; docs/DECISIONS.md D29,
 * D52, D103, D111, D118). Section numbers are of XSLT 4.0 (W3C QT4 CG
 * draft, as of October 2026).
 *
 * Golden files: src/xpath_ast.c (builder use, outputs untouched on
 * failure) and src/xpath_lower.c (lowering through the embedding entry).
 *
 * The match expression is built from the pattern AST by one loop over an
 * explicit work list of tasks in allocator memory (D71): a task writes a
 * few nodes of the match expression and pushes the tasks that fill their
 * operands, so patterns nested however deeply cost no C stack. The output
 * AST starts with a copy of the pattern AST's text store, so a copied node
 * keeps its payload as it is.
 */
#include "xslt_pattern_internal.h"

#include "schema_scope_internal.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>

#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"

/* Defined by the generated parser of grammars/xslt_pattern (D12, D29). */
const struct TSLanguage *
tree_sitter_xslt_pattern(void); // NOLINT(readability-identifier-naming)

#define SXML_XSLT_PATTERN_FUNCTION_NAMESPACE_URI                               \
    "http://www.w3.org/2005/xpath-functions"

/* ============================================================ Front end */

#define SXML_XSLT_PATTERN_NODE_TYPE(name, kind_name)                           \
    {.node_type = SXML_BYTE_VIEW_INITIALIZER(name),                            \
     .kind = SXML_AST_KIND_##kind_name}

/* The node types grammars/xslt_pattern adds to grammars/xpath, sorted by
   name (strcmp order) as the walker's binary search needs (D111). */
static const sxml_ast_node_type_entry sxml_xslt_pattern_node_types[] = {
    SXML_XSLT_PATTERN_NODE_TYPE("absolute_path_pattern", ABSOLUTE_PATH_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("any_item_pattern", ANY_ITEM_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("array_pattern", ARRAY_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("atomic_pattern", ATOMIC_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("axis_step_pattern", AXIS_STEP_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("filter_pattern", FILTER_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("function_call_pattern", FUNCTION_CALL_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("intersect_except_pattern",
                                INTERSECT_EXCEPT_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("jnode_pattern", JNODE_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("map_entry_pattern", MAP_ENTRY_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("map_pattern", MAP_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("parenthesized_pattern", PARENTHESIZED_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("pattern", PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("record_pattern", RECORD_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("relative_path_pattern", RELATIVE_PATH_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("rooted_path_pattern", ROOTED_PATH_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("sequence_pattern", SEQUENCE_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("simple_pattern", SIMPLE_PATTERN),
    SXML_XSLT_PATTERN_NODE_TYPE("union_pattern", UNION_PATTERN),
};

static const sxml_ast_field_entry sxml_xslt_pattern_fields[] = {
    {.field_name = SXML_BYTE_VIEW_INITIALIZER("pattern"),
     .field = SXML_AST_FIELD_PATTERN},
};

/* The "*" of a map pattern is a token in field `open` (6.3.6). */
static bool sxml_xslt_pattern_apply_token(sxml_ast_parser_token token,
                                          sxml_ast_node_request *request) {
    if (request->kind == SXML_AST_KIND_MAP_PATTERN &&
        sxml_byte_view_equals(token.field_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("open"))) {
        request->flags |= (uint32_t)SXML_AST_FLAG_OPEN;
        return true;
    }
    return false;
}

/* A function a FunctionCallP may call (6.3.2), with its arities. */
typedef struct sxml_xslt_pattern_permitted_function {
    sxml_byte_view local_name;
    uint32_t minimum_arity;
    uint32_t maximum_arity;
} sxml_xslt_pattern_permitted_function;

static const sxml_xslt_pattern_permitted_function
    sxml_xslt_pattern_permitted_functions[] = {
        {.local_name = SXML_BYTE_VIEW_INITIALIZER("doc"),
         .minimum_arity = 1,
         .maximum_arity = 1},
        {.local_name = SXML_BYTE_VIEW_INITIALIZER("id"),
         .minimum_arity = 1,
         .maximum_arity = 2},
        {.local_name = SXML_BYTE_VIEW_INITIALIZER("element-with-id"),
         .minimum_arity = 1,
         .maximum_arity = 2},
        {.local_name = SXML_BYTE_VIEW_INITIALIZER("key"),
         .minimum_arity = 2,
         .maximum_arity = 3},
        {.local_name = SXML_BYTE_VIEW_INITIALIZER("root"),
         .minimum_arity = 0,
         .maximum_arity = 0},
};

/* A function name in the fn namespace and an arity (6.3.2). */
typedef struct sxml_xslt_pattern_call_signature {
    sxml_byte_view local_name;
    uint32_t arity;
} sxml_xslt_pattern_call_signature;

static bool sxml_xslt_pattern_is_permitted_call(
    sxml_xslt_pattern_call_signature signature) {
    const size_t function_count = sizeof sxml_xslt_pattern_permitted_functions /
                                  sizeof sxml_xslt_pattern_permitted_functions[0];
    for (size_t function_index = 0; function_index < function_count;
         function_index += 1) {
        const sxml_xslt_pattern_permitted_function *function =
            &sxml_xslt_pattern_permitted_functions[function_index];
        if (sxml_byte_view_equals(function->local_name, signature.local_name) &&
            signature.arity >= function->minimum_arity &&
            signature.arity <= function->maximum_arity) {
            return true;
        }
    }
    return false;
}

static uint32_t sxml_xslt_pattern_count_children(const sxml_ast *ast,
                                                 sxml_ast_child_query query) {
    uint32_t child_count = 0;
    for (sxml_ast_node_index child = sxml_ast_find_child(ast, query);
         child.value != 0; child = sxml_ast_find_next_with_same_field(ast, child)) {
        child_count += 1;
    }
    return child_count;
}

static sxml_ast_node_index sxml_xslt_pattern_child(const sxml_ast *ast,
                                                   sxml_ast_node_index parent,
                                                   sxml_ast_field field) {
    return sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

static const sxml_ast_node *sxml_xslt_pattern_node(const sxml_ast *ast,
                                                   sxml_ast_node_index index) {
    return sxml_ast_get_node(ast, index);
}

/*
 * A function call at the outermost level must bind to a permitted function
 * (6.3.2). An unprefixed name is in the default function namespace (fn in
 * XSLT) and a Q{...} name names its namespace, so both are checked here; a
 * prefixed name waits for the namespaces (match expression).
 */
static sxml_status sxml_xslt_pattern_finish_node(sxml_ast_front_end *front_end,
                                                 sxml_ast_node_index node) {
    const sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    const sxml_ast_node *finished = sxml_xslt_pattern_node(ast, node);
    if (finished->kind != SXML_AST_KIND_FUNCTION_CALL_PATTERN) {
        return SXML_STATUS_OK;
    }
    const sxml_ast_node_index name =
        sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_NAME);
    const sxml_ast_node *name_node = sxml_xslt_pattern_node(ast, name);
    if (name_node->kind == SXML_AST_KIND_PREFIXED_NAME) {
        return SXML_STATUS_OK;
    }
    if (name_node->kind == SXML_AST_KIND_URI_QUALIFIED_NAME &&
        !sxml_byte_view_equals(
            sxml_ast_get_text(ast, name_node->payload.name.namespace_uri),
            SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_PATTERN_FUNCTION_NAMESPACE_URI))) {
        return sxml_ast_front_end_raise(
            front_end,
            (sxml_ast_static_error){
                .error_code = SXML_BYTE_VIEW_FROM_LITERAL("XTSE0340"),
                .description = SXML_BYTE_VIEW_FROM_LITERAL(
                    "a function call in a pattern must call fn:doc, fn:id, "
                    "fn:element-with-id, fn:key or fn:root (XSLT 4.0, 6.3.2)"),
                .byte_offset = finished->source_span.byte_offset});
    }
    const sxml_xslt_pattern_call_signature signature = {
        .local_name = sxml_ast_get_text(ast, name_node->payload.name.local_name),
        .arity = sxml_xslt_pattern_count_children(
            ast, (sxml_ast_child_query){.parent = node,
                                        .field = SXML_AST_FIELD_ARGUMENT})};
    if (sxml_xslt_pattern_is_permitted_call(signature)) {
        return SXML_STATUS_OK;
    }
    return sxml_ast_front_end_raise(
        front_end,
        (sxml_ast_static_error){
            .error_code = SXML_BYTE_VIEW_FROM_LITERAL("XTSE0340"),
            .description = SXML_BYTE_VIEW_FROM_LITERAL(
                "a function call in a pattern must call fn:doc#1, fn:id#1 or "
                "#2, fn:element-with-id#1 or #2, fn:key#2 or #3, or fn:root#0 "
                "(XSLT 4.0, 6.3.2)"),
            .byte_offset = finished->source_span.byte_offset});
}

static const sxml_ast_parser_part sxml_xslt_pattern_parser_part = {
    .language = tree_sitter_xslt_pattern,
    .root_node_type = SXML_BYTE_VIEW_INITIALIZER("pattern"),
    .node_types = {.entries = sxml_xslt_pattern_node_types,
                   .entry_count = sizeof sxml_xslt_pattern_node_types /
                                  sizeof sxml_xslt_pattern_node_types[0]},
    .fields = {.entries = sxml_xslt_pattern_fields,
               .entry_count = sizeof sxml_xslt_pattern_fields /
                              sizeof sxml_xslt_pattern_fields[0]},
    .unnamed_children = {.entries = NULL, .entry_count = 0},
    .syntax_error_description = SXML_BYTE_VIEW_INITIALIZER(
        "the text is not a pattern (XSLT 4.0, 6.3.1)"),
    .missing_token_description = SXML_BYTE_VIEW_INITIALIZER(
        "the pattern is incomplete (XSLT 4.0, 6.3.1)"),
    .apply_token = sxml_xslt_pattern_apply_token,
    .describe_payload = NULL,
    .finish_node = sxml_xslt_pattern_finish_node,
};

sxml_status sxml_xslt_pattern_build_ast(const sxml_ast_build_request *request,
                                        sxml_ast **ast) {
    if (request == NULL || ast == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_status status = sxml_ast_build_with_parser_part(
        request, &sxml_xslt_pattern_parser_part, ast);
    /* 6.3.1: where an attribute holds a pattern, text that does not match
       the production Pattern is XTSE0340, not the XPath code. */
    sxml_error_record *error_record = request->error_record;
    if (status == SXML_STATUS_STATIC_ERROR && error_record != NULL &&
        sxml_byte_view_equals(error_record->code.local_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("XPST0003"))) {
        error_record->code.local_name = SXML_BYTE_VIEW_FROM_LITERAL("XTSE0340");
    }
    return status;
}

/* ===================================================== Shared reading */

static bool sxml_xslt_pattern_has_predicate(const sxml_ast *ast,
                                            sxml_ast_node_index node) {
    return sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_PREDICATE).value != 0;
}

/* Functions whose value is never a number, so a predicate calling one at
   its top selects by its effective boolean value, never by position. */
static bool sxml_xslt_pattern_is_boolean_function(sxml_byte_view local_name) {
    static const char *const names[] = {
        "not",         "exists",      "empty",    "boolean",    "true",
        "false",       "contains",    "starts-with", "ends-with", "matches",
        "lang",        "deep-equal",  "has-children", "contains-token",
        "is-NaN",      "string",      "local-name", "name",      "normalize-space"};
    for (size_t name_index = 0; name_index < sizeof(names) / sizeof(names[0]);
         name_index += 1) {
        if (sxml_byte_view_equals(local_name, sxml_byte_view_from_c_string(names[name_index]))) {
            return true;
        }
    }
    return false;
}

/* True when a predicate expression's value can never be a number: a
   comparison, a logical, instance-of or castable expression, a literal
   string or boolean, nodes (a step, a path ending in a step, a union), or
   a call of a function above, unprefixed. */
static bool sxml_xslt_pattern_is_boolean_shaped(const sxml_ast *ast,
                                                sxml_ast_node_index expression) {
    static const sxml_ast_kind boolean_kinds[] = {
        SXML_AST_KIND_COMPARISON_EXPRESSION,    SXML_AST_KIND_AND_EXPRESSION,
        SXML_AST_KIND_OR_EXPRESSION,            SXML_AST_KIND_INSTANCE_OF_EXPRESSION,
        SXML_AST_KIND_CASTABLE_EXPRESSION,      SXML_AST_KIND_QUANTIFIED_EXPRESSION,
        SXML_AST_KIND_BOOLEAN_CONSTANT,         SXML_AST_KIND_STRING_LITERAL,
        SXML_AST_KIND_AXIS_STEP,                SXML_AST_KIND_UNION_EXPRESSION,
        SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION, SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION};
    const sxml_ast_node *node = sxml_xslt_pattern_node(ast, expression);
    for (size_t kind_index = 0; kind_index < sizeof(boolean_kinds) / sizeof(boolean_kinds[0]);
         kind_index += 1) {
        if (node->kind == boolean_kinds[kind_index]) {
            return true;
        }
    }
    if (node->kind == SXML_AST_KIND_RELATIVE_PATH_EXPRESSION) {
        return sxml_xslt_pattern_node(
                   ast, sxml_xslt_pattern_child(ast, expression, SXML_AST_FIELD_RIGHT))
                   ->kind == SXML_AST_KIND_AXIS_STEP;
    }
    if (node->kind == SXML_AST_KIND_FUNCTION_CALL) {
        const sxml_ast_node *name = sxml_xslt_pattern_node(
            ast, sxml_xslt_pattern_child(ast, expression, SXML_AST_FIELD_NAME));
        return name->kind == SXML_AST_KIND_NCNAME &&
               sxml_xslt_pattern_is_boolean_function(
                   sxml_ast_get_text(ast, name->payload.name.local_name));
    }
    return false;
}

/* True when the subtree of root calls or names fn:position or fn:last
   (any prefix, to be safe). A walk along the parent links: no stack. */
static bool sxml_xslt_pattern_reads_position(const sxml_ast *ast, sxml_ast_node_index root) {
    sxml_ast_node_index node = root;
    while (node.value != 0) {
        const sxml_ast_node *current = sxml_xslt_pattern_node(ast, node);
        if (current->kind == SXML_AST_KIND_FUNCTION_CALL ||
            current->kind == SXML_AST_KIND_NAMED_FUNCTION_REFERENCE) {
            const sxml_ast_node *name = sxml_xslt_pattern_node(
                ast, sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_NAME));
            const sxml_byte_view local_name =
                sxml_ast_get_text(ast, name->payload.name.local_name);
            if (sxml_byte_view_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("position")) ||
                sxml_byte_view_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("last"))) {
                return true;
            }
        }
        if (current->first_child.value != 0) {
            node = current->first_child;
            continue;
        }
        while (node.value != root.value &&
               sxml_xslt_pattern_node(ast, node)->next_sibling.value == 0) {
            node = sxml_xslt_pattern_node(ast, node)->parent;
        }
        node = node.value == root.value ? (sxml_ast_node_index){.value = 0}
                                        : sxml_xslt_pattern_node(ast, node)->next_sibling;
    }
    return false;
}

/*
 * True when every predicate of step selects by its effective boolean value
 * alone: its value is never a number and it reads neither position() nor
 * last(). Such a step need not be selected again from its origin (D118):
 * the predicates give the same value for the item with a singleton focus
 * as at its position among the nodes selected (XSLT 4.0, 6.3.2.3; D149).
 */
static bool sxml_xslt_pattern_has_positional_predicate(const sxml_ast *ast,
                                                       sxml_ast_node_index step) {
    for (sxml_ast_node_index predicate =
             sxml_xslt_pattern_child(ast, step, SXML_AST_FIELD_PREDICATE);
         predicate.value != 0; predicate = sxml_ast_find_next_with_same_field(ast, predicate)) {
        const sxml_ast_node_index expression =
            sxml_xslt_pattern_child(ast, predicate, SXML_AST_FIELD_EXPRESSION);
        if (!sxml_xslt_pattern_is_boolean_shaped(ast, expression) ||
            sxml_xslt_pattern_reads_position(ast, expression)) {
            return true;
        }
    }
    return false;
}

static bool sxml_xslt_pattern_is_name_kind(sxml_ast_kind kind) {
    return kind == SXML_AST_KIND_NCNAME || kind == SXML_AST_KIND_PREFIXED_NAME ||
           kind == SXML_AST_KIND_URI_QUALIFIED_NAME;
}

/*
 * The axis a step of a pattern uses: the one written, else the default of
 * XPath 4.0, 4.6.5 (attribute for attribute tests, namespace for
 * namespace-node(), child otherwise), except that a leading step testing
 * document-node() uses self (6.3.2.2).
 */
typedef struct sxml_xslt_pattern_step_axis_request {
    sxml_ast_node_index step;
    bool is_leading;
} sxml_xslt_pattern_step_axis_request;

static sxml_ast_axis sxml_xslt_pattern_step_axis(
    const sxml_ast *ast, sxml_xslt_pattern_step_axis_request request) {
    const sxml_ast_node *step = sxml_xslt_pattern_node(ast, request.step);
    if (step->detail.axis != SXML_AST_AXIS_NONE) {
        return step->detail.axis;
    }
    const sxml_ast_kind test_kind =
        sxml_xslt_pattern_node(
            ast, sxml_xslt_pattern_child(ast, request.step, SXML_AST_FIELD_TEST))
            ->kind;
    if (test_kind == SXML_AST_KIND_ATTRIBUTE_NODE_TYPE ||
        test_kind == SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE) {
        return SXML_AST_AXIS_ATTRIBUTE;
    }
    if (test_kind == SXML_AST_KIND_NAMESPACE_NODE_TYPE) {
        return SXML_AST_AXIS_NAMESPACE;
    }
    if (test_kind == SXML_AST_KIND_DOCUMENT_NODE_TYPE && request.is_leading) {
        return SXML_AST_AXIS_SELF;
    }
    return SXML_AST_AXIS_CHILD;
}

/* A node test the self tests below can write without selecting again: not
   a dynamic node test or a JNode type. */
static bool sxml_xslt_pattern_test_is_simple(const sxml_ast *ast,
                                             sxml_ast_node_index test) {
    const sxml_ast_node *test_node = sxml_xslt_pattern_node(ast, test);
    if (test_node->kind == SXML_AST_KIND_UNION_NODE_TEST) {
        for (sxml_ast_node_index member =
                 sxml_xslt_pattern_child(ast, test, SXML_AST_FIELD_TEST);
             member.value != 0;
             member = sxml_ast_find_next_with_same_field(ast, member)) {
            const sxml_ast_kind member_kind =
                sxml_xslt_pattern_node(ast, member)->kind;
            if (member_kind == SXML_AST_KIND_DYNAMIC_NODE_TEST ||
                member_kind == SXML_AST_KIND_JNODE_TYPE) {
                return false;
            }
        }
        return true;
    }
    return test_node->kind != SXML_AST_KIND_DYNAMIC_NODE_TEST &&
           test_node->kind != SXML_AST_KIND_JNODE_TYPE;
}

static bool sxml_xslt_pattern_kind_may_match_attribute(sxml_ast_kind kind) {
    return kind == SXML_AST_KIND_GNODE_TYPE || kind == SXML_AST_KIND_ANY_XNODE_TYPE ||
           kind == SXML_AST_KIND_ATTRIBUTE_NODE_TYPE ||
           kind == SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE ||
           kind == SXML_AST_KIND_NAMESPACE_NODE_TYPE;
}

/* True when the test, read on the self axis, may accept an attribute or
   namespace node. */
static bool sxml_xslt_pattern_test_may_match_attribute(const sxml_ast *ast,
                                                       sxml_ast_node_index test) {
    const sxml_ast_node *test_node = sxml_xslt_pattern_node(ast, test);
    if (test_node->kind != SXML_AST_KIND_UNION_NODE_TEST) {
        return sxml_xslt_pattern_kind_may_match_attribute(test_node->kind);
    }
    for (sxml_ast_node_index member =
             sxml_xslt_pattern_child(ast, test, SXML_AST_FIELD_TEST);
         member.value != 0; member = sxml_ast_find_next_with_same_field(ast, member)) {
        if (sxml_xslt_pattern_kind_may_match_attribute(
                sxml_xslt_pattern_node(ast, member)->kind)) {
            return true;
        }
    }
    return false;
}

/* ================================================== Default priority */

/* 6.4.1, the table for ElementNodeType and AttributeNodeType: by the name
   test (E, W or *) and whether a type is given. */
typedef struct sxml_xslt_pattern_name_priority_request {
    sxml_ast_node_index name_test;
    bool has_type;
    bool is_element;
    bool is_any_namespace;
} sxml_xslt_pattern_name_priority_request;

static double sxml_xslt_pattern_name_priority(
    const sxml_ast *ast, sxml_xslt_pattern_name_priority_request request) {
    const sxml_ast_node *name = sxml_xslt_pattern_node(ast, request.name_test);
    bool is_any_name = false;
    bool is_wildcard = false;
    if (name->kind == SXML_AST_KIND_WILDCARD) {
        is_any_name = name->detail.name_test_kind == SXML_NAME_TEST_KIND_ANY_NAME;
        is_wildcard = !is_any_name;
    } else if (name->kind == SXML_AST_KIND_NCNAME && request.is_element &&
               request.is_any_namespace) {
        /* 6.4.1: with ##any, an unprefixed element name E counts as *:E. */
        is_wildcard = true;
    }
    if (is_any_name) {
        return request.has_type ? 0.0 : -0.5;
    }
    if (is_wildcard) {
        return request.has_type ? 0.125 : -0.25;
    }
    return request.has_type ? 0.25 : 0.0;
}

/* element(...) or attribute(...): the lowest priority of its names
   (6.4.1, "element(A|B)"). */
typedef struct sxml_xslt_pattern_node_type_priority_request {
    sxml_ast_node_index node_type;
    bool is_any_namespace;
} sxml_xslt_pattern_node_type_priority_request;

static double sxml_xslt_pattern_node_type_priority(
    const sxml_ast *ast, sxml_xslt_pattern_node_type_priority_request request) {
    const sxml_ast_node *node_type = sxml_xslt_pattern_node(ast, request.node_type);
    const bool is_element = node_type->kind != SXML_AST_KIND_ATTRIBUTE_NODE_TYPE;
    sxml_ast_node_index names = request.node_type;
    bool has_type = false;
    if (node_type->kind != SXML_AST_KIND_NAME_TEST_UNION) {
        names = sxml_xslt_pattern_child(ast, request.node_type, SXML_AST_FIELD_NAME);
        has_type = sxml_xslt_pattern_child(ast, request.node_type,
                                           SXML_AST_FIELD_TYPE)
                       .value != 0;
    }
    if (names.value == 0) {
        /* element() and attribute(): as * and @*. */
        return -0.5;
    }
    double lowest = 1.0;
    for (sxml_ast_node_index name_test =
             sxml_xslt_pattern_child(ast, names, SXML_AST_FIELD_TEST);
         name_test.value != 0;
         name_test = sxml_ast_find_next_with_same_field(ast, name_test)) {
        const double name_priority = sxml_xslt_pattern_name_priority(
            ast, (sxml_xslt_pattern_name_priority_request){
                     .name_test = name_test,
                     .has_type = has_type,
                     .is_element = is_element,
                     .is_any_namespace = request.is_any_namespace});
        lowest = fmin(lowest, name_priority);
    }
    return lowest;
}

/* One node test of a step without predicates (6.4.1). */
typedef struct sxml_xslt_pattern_test_priority_request {
    sxml_ast_node_index test;
    sxml_ast_axis axis;
    bool is_any_namespace;
} sxml_xslt_pattern_test_priority_request;

static double sxml_xslt_pattern_simple_test_priority(
    const sxml_ast *ast, sxml_xslt_pattern_test_priority_request request) {
    const sxml_ast_node *test = sxml_xslt_pattern_node(ast, request.test);
    const bool is_element_axis = request.axis != SXML_AST_AXIS_ATTRIBUTE &&
                                 request.axis != SXML_AST_AXIS_NAMESPACE;
    if (sxml_xslt_pattern_is_name_kind(test->kind) ||
        test->kind == SXML_AST_KIND_WILDCARD) {
        return sxml_xslt_pattern_name_priority(
            ast, (sxml_xslt_pattern_name_priority_request){
                     .name_test = request.test,
                     .has_type = false,
                     .is_element = is_element_axis,
                     .is_any_namespace = request.is_any_namespace});
    }
    if (test->kind == SXML_AST_KIND_PROCESSING_INSTRUCTION_NODE_TYPE) {
        return sxml_xslt_pattern_child(ast, request.test, SXML_AST_FIELD_NAME).value != 0
                   ? 0.0
                   : -0.5;
    }
    if (test->kind == SXML_AST_KIND_ELEMENT_NODE_TYPE ||
        test->kind == SXML_AST_KIND_ATTRIBUTE_NODE_TYPE) {
        return sxml_xslt_pattern_node_type_priority(
            ast, (sxml_xslt_pattern_node_type_priority_request){
                     .node_type = request.test,
                     .is_any_namespace = request.is_any_namespace});
    }
    if (test->kind == SXML_AST_KIND_SCHEMA_ELEMENT_NODE_TYPE ||
        test->kind == SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE) {
        return 0.25;
    }
    if (test->kind == SXML_AST_KIND_DOCUMENT_NODE_TYPE) {
        /* document-node(E): the priority of its element test. */
        const sxml_ast_node_index content =
            sxml_xslt_pattern_child(ast, request.test, SXML_AST_FIELD_CONTENT);
        const sxml_ast_kind content_kind = sxml_xslt_pattern_node(ast, content)->kind;
        if (content_kind == SXML_AST_KIND_SCHEMA_ELEMENT_NODE_TYPE) {
            return 0.25;
        }
        if (content_kind == SXML_AST_KIND_ELEMENT_NODE_TYPE ||
            content_kind == SXML_AST_KIND_NAME_TEST_UNION) {
            return sxml_xslt_pattern_node_type_priority(
                ast, (sxml_xslt_pattern_node_type_priority_request){
                         .node_type = content,
                         .is_any_namespace = request.is_any_namespace});
        }
        return -0.5;
    }
    return -0.5;
}

static double sxml_xslt_pattern_step_priority(const sxml_ast *ast,
                                              sxml_xslt_pattern_test_priority_request request) {
    const sxml_ast_node_index step = request.test;
    const bool is_any_namespace = request.is_any_namespace;
    if (sxml_xslt_pattern_has_predicate(ast, step)) {
        return 0.5;
    }
    const sxml_ast_axis axis = sxml_xslt_pattern_step_axis(
        ast, (sxml_xslt_pattern_step_axis_request){.step = step, .is_leading = true});
    const sxml_ast_node_index test =
        sxml_xslt_pattern_child(ast, step, SXML_AST_FIELD_TEST);
    if (sxml_xslt_pattern_node(ast, test)->kind != SXML_AST_KIND_UNION_NODE_TEST) {
        return sxml_xslt_pattern_simple_test_priority(
            ast, (sxml_xslt_pattern_test_priority_request){
                     .test = test, .axis = axis, .is_any_namespace = is_any_namespace});
    }
    /* child::(a|b) is a union of the two steps (6.4.1). */
    double lowest = 1.0;
    for (sxml_ast_node_index member =
             sxml_xslt_pattern_child(ast, test, SXML_AST_FIELD_TEST);
         member.value != 0; member = sxml_ast_find_next_with_same_field(ast, member)) {
        lowest = fmin(lowest,
                      sxml_xslt_pattern_simple_test_priority(
                          ast, (sxml_xslt_pattern_test_priority_request){
                                   .test = member,
                                   .axis = axis,
                                   .is_any_namespace = is_any_namespace}));
    }
    return lowest;
}

/* The number of derivation steps from xs:anyAtomicType to an atomic type
   gives atomic(T) the priority 0.5 - 2^-N (6.4.3). */
static sxml_status sxml_xslt_pattern_atomic_type_priority(
    sxml_type_registry *type_registry, sxml_type_identifier type, double *priority) {
    const sxml_type_identifier any_atomic =
        sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_ATOMIC_TYPE);
    if (type.index == any_atomic.index) {
        *priority = -0.5;
        return SXML_STATUS_OK;
    }
    sxml_type_description description;
    sxml_status status =
        sxml_type_registry_describe_type(type_registry, type, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.variety == SXML_TYPE_VARIETY_UNION) {
        /* 6.4.3: a union type as the list of its member types. */
        const sxml_type_identifier_view members =
            description.variety_details.member_types;
        double lowest = 0.5;
        for (size_t member_index = 0; member_index < members.type_identifier_count;
             member_index += 1) {
            sxml_type_description member;
            status = sxml_type_registry_describe_type(
                type_registry, members.type_identifiers[member_index], &member);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            uint32_t step_count = 0;
            sxml_type_identifier current = members.type_identifiers[member_index];
            while (status == SXML_STATUS_OK && current.index != any_atomic.index &&
                   current.index != SXML_TYPE_NONE && step_count < 64U) {
                step_count += 1;
                status = sxml_type_registry_describe_type(type_registry, current,
                                                          &member);
                current = member.base_type;
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            lowest = fmin(lowest, 0.5 - ldexp(1.0, -(int)step_count));
        }
        *priority = lowest;
        return SXML_STATUS_OK;
    }
    uint32_t step_count = 0;
    sxml_type_identifier current = type;
    while (current.index != any_atomic.index && current.index != SXML_TYPE_NONE &&
           step_count < 64U) {
        step_count += 1;
        status = sxml_type_registry_describe_type(type_registry, current, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        current = description.base_type;
    }
    *priority = 0.5 - ldexp(1.0, -(int)step_count);
    return SXML_STATUS_OK;
}

static sxml_status sxml_xslt_pattern_atomic_priority(
    const sxml_xslt_pattern_priority_request *request, sxml_ast_node_index atomic,
    double *priority) {
    const sxml_ast *ast = request->pattern_ast;
    double lowest = 0.5;
    for (sxml_ast_node_index type_name =
             sxml_xslt_pattern_child(ast, atomic, SXML_AST_FIELD_TYPE);
         type_name.value != 0;
         type_name = sxml_ast_find_next_with_same_field(ast, type_name)) {
        sxml_type_identifier type = {0};
        sxml_status status = sxml_xpath_resolve_item_type(
            &(sxml_xpath_type_request){.ast = ast,
                                       .type_node = type_name,
                                       .static_context = request->static_context,
                                       .error_record = request->error_record},
            &type);
        double type_priority = 0.0;
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_atomic_type_priority(
                sxml_item_heap_get_type_registry(request->static_context->item_heap),
                type, &type_priority);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        lowest = fmin(lowest, type_priority);
    }
    *priority = lowest;
    return SXML_STATUS_OK;
}

/* 6.4.4. */
static double sxml_xslt_pattern_map_priority(const sxml_ast *ast,
                                             sxml_ast_node_index map) {
    const sxml_ast_node_index first_entry =
        sxml_xslt_pattern_child(ast, map, SXML_AST_FIELD_ENTRY);
    if (first_entry.value == 0) {
        const bool is_open =
            (sxml_xslt_pattern_node(ast, map)->flags & (uint32_t)SXML_AST_FLAG_OPEN) != 0;
        return is_open ? -0.25 : 0.5;
    }
    for (sxml_ast_node_index entry = first_entry; entry.value != 0;
         entry = sxml_ast_find_next_with_same_field(ast, entry)) {
        if (sxml_xslt_pattern_child(ast, entry, SXML_AST_FIELD_VALUE).value != 0) {
            return 0.25;
        }
    }
    return 0.0;
}

/* Work of the priority computation: evaluate a node, or combine the two
   values on top of the value stack (MINIMUM), or the top with bound
   (MAXIMUM). */
typedef enum sxml_xslt_pattern_priority_work_kind {
    SXML_XSLT_PATTERN_PRIORITY_WORK_EVALUATE,
    SXML_XSLT_PATTERN_PRIORITY_WORK_MINIMUM,
    SXML_XSLT_PATTERN_PRIORITY_WORK_MAXIMUM,
} sxml_xslt_pattern_priority_work_kind;

typedef struct sxml_xslt_pattern_priority_work {
    sxml_xslt_pattern_priority_work_kind work_kind;
    sxml_ast_node_index node;
    double bound;
} sxml_xslt_pattern_priority_work;

typedef struct sxml_xslt_pattern_priority_state {
    const sxml_xslt_pattern_priority_request *request;
    sxml_xslt_pattern_priority_work *works;
    size_t work_count;
    double *values;
    size_t value_count;
} sxml_xslt_pattern_priority_state;

static void sxml_xslt_pattern_priority_push(sxml_xslt_pattern_priority_state *state,
                                            sxml_xslt_pattern_priority_work work) {
    state->works[state->work_count] = work;
    state->work_count += 1;
}

static void sxml_xslt_pattern_priority_evaluate(
    sxml_xslt_pattern_priority_state *state, sxml_ast_node_index node) {
    sxml_xslt_pattern_priority_push(
        state, (sxml_xslt_pattern_priority_work){
                   .work_kind = SXML_XSLT_PATTERN_PRIORITY_WORK_EVALUATE,
                   .node = node,
                   .bound = 0.0});
}

static void sxml_xslt_pattern_priority_value(sxml_xslt_pattern_priority_state *state,
                                             double value) {
    state->values[state->value_count] = value;
    state->value_count += 1;
}

/* Evaluates one node: a value, or more work. */
static sxml_status sxml_xslt_pattern_priority_step(
    sxml_xslt_pattern_priority_state *state, sxml_ast_node_index node) {
    const sxml_ast *ast = state->request->pattern_ast;
    const sxml_ast_node *pattern = sxml_xslt_pattern_node(ast, node);
    const bool is_any_namespace = state->request->is_any_namespace;
    switch ((uint32_t)pattern->kind) {
    case SXML_AST_KIND_PATTERN:
    case SXML_AST_KIND_PARENTHESIZED_PATTERN:
        sxml_xslt_pattern_priority_evaluate(
            state, sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_PATTERN));
        return SXML_STATUS_OK;
    case SXML_AST_KIND_UNION_PATTERN:
        /* 6.4.1: the lowest of the alternatives. */
        sxml_xslt_pattern_priority_push(
            state, (sxml_xslt_pattern_priority_work){
                       .work_kind = SXML_XSLT_PATTERN_PRIORITY_WORK_MINIMUM,
                       .node = node,
                       .bound = 0.0});
        sxml_xslt_pattern_priority_evaluate(
            state, sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_RIGHT));
        sxml_xslt_pattern_priority_evaluate(
            state, sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_LEFT));
        return SXML_STATUS_OK;
    case SXML_AST_KIND_INTERSECT_EXCEPT_PATTERN:
        /* 6.4.1: that of the first operand. */
        sxml_xslt_pattern_priority_evaluate(
            state, sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_LEFT));
        return SXML_STATUS_OK;
    case SXML_AST_KIND_AXIS_STEP_PATTERN:
        sxml_xslt_pattern_priority_value(
            state, sxml_xslt_pattern_step_priority(
                       ast, (sxml_xslt_pattern_test_priority_request){
                                .test = node,
                                .axis = SXML_AST_AXIS_NONE,
                                .is_any_namespace = is_any_namespace}));
        return SXML_STATUS_OK;
    case SXML_AST_KIND_ABSOLUTE_PATH_PATTERN:
        /* 6.4.1: "/" is -0.5. */
        sxml_xslt_pattern_priority_value(
            state,
            sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_PATH).value == 0 ? -0.5
                                                                               : 0.5);
        return SXML_STATUS_OK;
    case SXML_AST_KIND_SIMPLE_PATTERN: {
        const sxml_ast_node_index item =
            sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_ITEM);
        if (sxml_xslt_pattern_has_predicate(ast, node)) {
            /* 6.4: +0.5 with predicates; 6.4.7: +1 for a JNode pattern. */
            sxml_xslt_pattern_priority_value(
                state,
                sxml_xslt_pattern_node(ast, item)->kind == SXML_AST_KIND_JNODE_PATTERN
                    ? 1.0
                    : 0.5);
            return SXML_STATUS_OK;
        }
        sxml_xslt_pattern_priority_evaluate(state, item);
        return SXML_STATUS_OK;
    }
    case SXML_AST_KIND_ANY_ITEM_PATTERN:
        /* 6.4.2. */
        sxml_xslt_pattern_priority_value(state, -1.0);
        return SXML_STATUS_OK;
    case SXML_AST_KIND_ATOMIC_PATTERN: {
        double priority = 0.0;
        const sxml_status status =
            sxml_xslt_pattern_atomic_priority(state->request, node, &priority);
        if (status == SXML_STATUS_OK) {
            sxml_xslt_pattern_priority_value(state, priority);
        }
        return status;
    }
    case SXML_AST_KIND_MAP_PATTERN:
        sxml_xslt_pattern_priority_value(state, sxml_xslt_pattern_map_priority(ast, node));
        return SXML_STATUS_OK;
    case SXML_AST_KIND_RECORD_PATTERN:
        /* 6.4.5. */
        sxml_xslt_pattern_priority_value(state, 0.0);
        return SXML_STATUS_OK;
    case SXML_AST_KIND_ARRAY_PATTERN:
        /* 6.4.6: that of the member pattern, +0.5 for empty-sequence(). */
        sxml_xslt_pattern_priority_evaluate(
            state, sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_MEMBER));
        return SXML_STATUS_OK;
    case SXML_AST_KIND_SEQUENCE_PATTERN: {
        const sxml_ast_node_index inner =
            sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_PATTERN);
        if (inner.value == 0) {
            sxml_xslt_pattern_priority_value(state, 0.5);
            return SXML_STATUS_OK;
        }
        sxml_xslt_pattern_priority_evaluate(state, inner);
        return SXML_STATUS_OK;
    }
    case SXML_AST_KIND_JNODE_PATTERN: {
        /* 6.4.7: the highest of +0.5 for a constant selector, +0.5 for
           empty-sequence() and the priority of the value's pattern. */
        const sxml_ast_node_index selector =
            sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_SELECTOR);
        const sxml_ast_node_index value =
            sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_VALUE);
        const sxml_ast_node_index inner =
            sxml_xslt_pattern_child(ast, value, SXML_AST_FIELD_PATTERN);
        const bool has_constant_selector =
            sxml_xslt_pattern_node(ast, selector)->kind != SXML_AST_KIND_WILDCARD;
        const double bound = has_constant_selector || inner.value == 0 ? 0.5 : -1.0;
        if (inner.value == 0) {
            sxml_xslt_pattern_priority_value(state, bound);
            return SXML_STATUS_OK;
        }
        sxml_xslt_pattern_priority_push(
            state, (sxml_xslt_pattern_priority_work){
                       .work_kind = SXML_XSLT_PATTERN_PRIORITY_WORK_MAXIMUM,
                       .node = node,
                       .bound = bound});
        sxml_xslt_pattern_priority_evaluate(state, inner);
        return SXML_STATUS_OK;
    }
    default:
        /* 6.4.1: every other XNode pattern (paths, rooted paths, function
           calls, filters) is +0.5. */
        sxml_xslt_pattern_priority_value(state, 0.5);
        return SXML_STATUS_OK;
    }
}

sxml_status sxml_xslt_pattern_compute_default_priority(
    const sxml_xslt_pattern_priority_request *request, double *priority) {
    if (request == NULL || priority == NULL || request->pattern_ast == NULL ||
        request->static_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_ast *ast = request->pattern_ast;
    if (sxml_xslt_pattern_node(ast, ast->root)->kind != SXML_AST_KIND_PATTERN) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Each node is evaluated at most once and pushes at most one combining
       work, so twice the node count bounds both stacks. */
    const size_t capacity = 2U * (size_t)ast->node_count + 2U;
    void *work_memory = NULL;
    void *value_memory = NULL;
    const sxml_allocator allocator = request->static_context->allocator;
    sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count =
                                      capacity * sizeof(sxml_xslt_pattern_priority_work),
                                  .alignment = alignof(sxml_xslt_pattern_priority_work)},
        &work_memory);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = capacity * sizeof(double),
                                      .alignment = alignof(double)},
            &value_memory);
    }
    sxml_xslt_pattern_priority_state state = {
        .request = request,
        .works = work_memory,
        .work_count = 0,
        .values = value_memory,
        .value_count = 0};
    if (status == SXML_STATUS_OK) {
        sxml_xslt_pattern_priority_evaluate(&state, ast->root);
    }
    while (status == SXML_STATUS_OK && state.work_count > 0) {
        state.work_count -= 1;
        const sxml_xslt_pattern_priority_work work = state.works[state.work_count];
        switch (work.work_kind) {
        case SXML_XSLT_PATTERN_PRIORITY_WORK_EVALUATE:
            status = sxml_xslt_pattern_priority_step(&state, work.node);
            break;
        case SXML_XSLT_PATTERN_PRIORITY_WORK_MINIMUM:
            state.value_count -= 1;
            state.values[state.value_count - 1U] =
                fmin(state.values[state.value_count - 1U], state.values[state.value_count]);
            break;
        case SXML_XSLT_PATTERN_PRIORITY_WORK_MAXIMUM:
            state.values[state.value_count - 1U] =
                fmax(state.values[state.value_count - 1U], work.bound);
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        *priority = state.values[0];
    }
    sxml_allocator_release(allocator, work_memory);
    sxml_allocator_release(allocator, value_memory);
    return status;
}

/* ================================================== Match expression */

/* Texts the match expression uses beyond the pattern's own, appended once
   to the output's text store. */
typedef enum sxml_xslt_pattern_text {
    SXML_XSLT_PATTERN_TEXT_FUNCTION_NAMESPACE,
    SXML_XSLT_PATTERN_TEXT_MAP_NAMESPACE,
    SXML_XSLT_PATTERN_TEXT_ARRAY_NAMESPACE,
    SXML_XSLT_PATTERN_TEXT_VARIABLE_NAMESPACE,
    SXML_XSLT_PATTERN_TEXT_BOOLEAN,
    SXML_XSLT_PATTERN_TEXT_EXISTS,
    SXML_XSLT_PATTERN_TEXT_EMPTY,
    SXML_XSLT_PATTERN_TEXT_NOT,
    SXML_XSLT_PATTERN_TEXT_TRUE,
    SXML_XSLT_PATTERN_TEXT_FALSE,
    SXML_XSLT_PATTERN_TEXT_COUNT,
    SXML_XSLT_PATTERN_TEXT_LOCAL_NAME,
    SXML_XSLT_PATTERN_TEXT_JKEY,
    SXML_XSLT_PATTERN_TEXT_JVALUE,
    SXML_XSLT_PATTERN_TEXT_ATOMIC_EQUAL,
    SXML_XSLT_PATTERN_TEXT_CONTAINS,
    SXML_XSLT_PATTERN_TEXT_GET,
    SXML_XSLT_PATTERN_TEXT_SIZE,
    SXML_XSLT_PATTERN_TEXT_REMOVE,
    /* Variable local names: the item, a candidate context node, a value,
       a member position. */
    SXML_XSLT_PATTERN_TEXT_ITEM_VARIABLE,
    SXML_XSLT_PATTERN_TEXT_CONTEXT_VARIABLE,
    SXML_XSLT_PATTERN_TEXT_VALUE_VARIABLE,
    SXML_XSLT_PATTERN_TEXT_POSITION_VARIABLE,
    SXML_XSLT_PATTERN_TEXT_ZERO,
    SXML_XSLT_PATTERN_TEXT_ONE,
    SXML_XSLT_PATTERN_TEXT_KIND_COUNT,
} sxml_xslt_pattern_text;

static const sxml_byte_view
    sxml_xslt_pattern_texts[SXML_XSLT_PATTERN_TEXT_KIND_COUNT] = {
        SXML_BYTE_VIEW_INITIALIZER(SXML_XSLT_PATTERN_FUNCTION_NAMESPACE_URI),
        SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2005/xpath-functions/map"),
        SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2005/xpath-functions/array"),
        SXML_BYTE_VIEW_INITIALIZER(SXML_XSLT_PATTERN_VARIABLE_NAMESPACE_URI),
        SXML_BYTE_VIEW_INITIALIZER("boolean"),
        SXML_BYTE_VIEW_INITIALIZER("exists"),
        SXML_BYTE_VIEW_INITIALIZER("empty"),
        SXML_BYTE_VIEW_INITIALIZER("not"),
        SXML_BYTE_VIEW_INITIALIZER("true"),
        SXML_BYTE_VIEW_INITIALIZER("false"),
        SXML_BYTE_VIEW_INITIALIZER("count"),
        SXML_BYTE_VIEW_INITIALIZER("local-name"),
        SXML_BYTE_VIEW_INITIALIZER("jkey"),
        SXML_BYTE_VIEW_INITIALIZER("jvalue"),
        SXML_BYTE_VIEW_INITIALIZER("atomic-equal"),
        SXML_BYTE_VIEW_INITIALIZER("contains"),
        SXML_BYTE_VIEW_INITIALIZER("get"),
        SXML_BYTE_VIEW_INITIALIZER("size"),
        SXML_BYTE_VIEW_INITIALIZER("remove"),
        SXML_BYTE_VIEW_INITIALIZER("n"),
        SXML_BYTE_VIEW_INITIALIZER("x"),
        SXML_BYTE_VIEW_INITIALIZER("v"),
        SXML_BYTE_VIEW_INITIALIZER("p"),
        SXML_BYTE_VIEW_INITIALIZER("0"),
        SXML_BYTE_VIEW_INITIALIZER("1"),
};

/* The functions a match expression calls: namespace and local name. */
typedef enum sxml_xslt_pattern_function {
    SXML_XSLT_PATTERN_FUNCTION_BOOLEAN,
    SXML_XSLT_PATTERN_FUNCTION_EXISTS,
    SXML_XSLT_PATTERN_FUNCTION_EMPTY,
    SXML_XSLT_PATTERN_FUNCTION_NOT,
    SXML_XSLT_PATTERN_FUNCTION_TRUE,
    SXML_XSLT_PATTERN_FUNCTION_FALSE,
    SXML_XSLT_PATTERN_FUNCTION_COUNT,
    SXML_XSLT_PATTERN_FUNCTION_LOCAL_NAME,
    SXML_XSLT_PATTERN_FUNCTION_JKEY,
    SXML_XSLT_PATTERN_FUNCTION_JVALUE,
    SXML_XSLT_PATTERN_FUNCTION_ATOMIC_EQUAL,
    SXML_XSLT_PATTERN_FUNCTION_MAP_CONTAINS,
    SXML_XSLT_PATTERN_FUNCTION_MAP_GET,
    SXML_XSLT_PATTERN_FUNCTION_MAP_SIZE,
    SXML_XSLT_PATTERN_FUNCTION_MAP_REMOVE,
    SXML_XSLT_PATTERN_FUNCTION_ARRAY_GET,
    SXML_XSLT_PATTERN_FUNCTION_ARRAY_SIZE,
} sxml_xslt_pattern_function;

typedef struct sxml_xslt_pattern_function_name {
    sxml_xslt_pattern_text namespace_uri;
    sxml_xslt_pattern_text local_name;
} sxml_xslt_pattern_function_name;

static sxml_xslt_pattern_function_name
sxml_xslt_pattern_function_name_of(sxml_xslt_pattern_function function) {
    const sxml_xslt_pattern_text functions = SXML_XSLT_PATTERN_TEXT_FUNCTION_NAMESPACE;
    const sxml_xslt_pattern_text map = SXML_XSLT_PATTERN_TEXT_MAP_NAMESPACE;
    const sxml_xslt_pattern_text array = SXML_XSLT_PATTERN_TEXT_ARRAY_NAMESPACE;
    switch (function) {
    case SXML_XSLT_PATTERN_FUNCTION_BOOLEAN:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_BOOLEAN};
    case SXML_XSLT_PATTERN_FUNCTION_EXISTS:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_EXISTS};
    case SXML_XSLT_PATTERN_FUNCTION_EMPTY:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_EMPTY};
    case SXML_XSLT_PATTERN_FUNCTION_NOT:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_NOT};
    case SXML_XSLT_PATTERN_FUNCTION_TRUE:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_TRUE};
    case SXML_XSLT_PATTERN_FUNCTION_FALSE:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_FALSE};
    case SXML_XSLT_PATTERN_FUNCTION_COUNT:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_COUNT};
    case SXML_XSLT_PATTERN_FUNCTION_LOCAL_NAME:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_LOCAL_NAME};
    case SXML_XSLT_PATTERN_FUNCTION_JKEY:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_JKEY};
    case SXML_XSLT_PATTERN_FUNCTION_JVALUE:
        return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_JVALUE};
    case SXML_XSLT_PATTERN_FUNCTION_ATOMIC_EQUAL:
        return (sxml_xslt_pattern_function_name){functions,
                                                 SXML_XSLT_PATTERN_TEXT_ATOMIC_EQUAL};
    case SXML_XSLT_PATTERN_FUNCTION_MAP_CONTAINS:
        return (sxml_xslt_pattern_function_name){map, SXML_XSLT_PATTERN_TEXT_CONTAINS};
    case SXML_XSLT_PATTERN_FUNCTION_MAP_GET:
        return (sxml_xslt_pattern_function_name){map, SXML_XSLT_PATTERN_TEXT_GET};
    case SXML_XSLT_PATTERN_FUNCTION_MAP_SIZE:
        return (sxml_xslt_pattern_function_name){map, SXML_XSLT_PATTERN_TEXT_SIZE};
    case SXML_XSLT_PATTERN_FUNCTION_MAP_REMOVE:
        return (sxml_xslt_pattern_function_name){map, SXML_XSLT_PATTERN_TEXT_REMOVE};
    case SXML_XSLT_PATTERN_FUNCTION_ARRAY_GET:
        return (sxml_xslt_pattern_function_name){array, SXML_XSLT_PATTERN_TEXT_GET};
    case SXML_XSLT_PATTERN_FUNCTION_ARRAY_SIZE:
        return (sxml_xslt_pattern_function_name){array, SXML_XSLT_PATTERN_TEXT_SIZE};
    }
    return (sxml_xslt_pattern_function_name){functions, SXML_XSLT_PATTERN_TEXT_FALSE};
}

/* Where a new node goes: the root when parent is 0. */
typedef struct sxml_xslt_pattern_destination {
    sxml_ast_node_index parent;
    sxml_ast_field field;
} sxml_xslt_pattern_destination;

/* What a task writes at its destination (see each process_ function). */
typedef enum sxml_xslt_pattern_task_kind {
    /* The item matches the pattern node (any pattern kind). */
    SXML_XSLT_PATTERN_TASK_MATCH,
    /* The node, a part of an XNode pattern, selects the context node;
       prefix stands for the steps left of its leftmost step. */
    SXML_XSLT_PATTERN_TASK_CONDITION,
    /* The context node is one the prefix's steps select. */
    SXML_XSLT_PATTERN_TASK_PREFIX_CONDITION,
    /* The item matches the item pattern node. */
    SXML_XSLT_PATTERN_TASK_ITEM,
    /* The value in the value variable matches the sequence pattern. */
    SXML_XSLT_PATTERN_TASK_SEQUENCE,
    /* The context node passes the node test on the axis. */
    SXML_XSLT_PATTERN_TASK_SELF_TEST,
    /* As SELF_TEST for the node test and the ones after it in its union. */
    SXML_XSLT_PATTERN_TASK_SELF_TEST_UNION,
    /* The context node is in the value of the rooted path's variable. */
    SXML_XSLT_PATTERN_TASK_MEMBERSHIP,
    /* The equivalent expression of an XNode pattern node (6.3.2.2). */
    SXML_XSLT_PATTERN_TASK_EQUIVALENT,
    /* The formal definition of matching for an XNode pattern node. */
    SXML_XSLT_PATTERN_TASK_EQUIVALENT_MATCH,
    /* A copy of the subtree. */
    SXML_XSLT_PATTERN_TASK_COPY,
    /* Copies of the node and its following siblings, each in its own
       field, under destination.parent. */
    SXML_XSLT_PATTERN_TASK_COPY_SIBLINGS,
    /* A constant as an expression. */
    SXML_XSLT_PATTERN_TASK_CONSTANT,
    /* A step pattern as an AXIS_STEP (axis replaced unless NONE). */
    SXML_XSLT_PATTERN_TASK_STEP_COPY,
    /* As STEP_COPY without the predicates. */
    SXML_XSLT_PATTERN_TASK_STEP_TEST_COPY,
    /* A function call pattern as a FUNCTION_CALL. */
    SXML_XSLT_PATTERN_TASK_FUNCTION_CALL,
} sxml_xslt_pattern_task_kind;

typedef struct sxml_xslt_pattern_task {
    sxml_xslt_pattern_task_kind task_kind;
    sxml_ast_node_index source;
    sxml_xslt_pattern_destination destination;
    /* CONDITION and PREFIX_CONDITION: a prefix index (0 for none). */
    uint32_t prefix;
    /* SELF_TEST, SELF_TEST_UNION, STEP_COPY, STEP_TEST_COPY. */
    sxml_ast_axis axis;
    /* EQUIVALENT: whether the steps of source are leading steps. */
    bool is_leading;
} sxml_xslt_pattern_task;

/* What is left of the leftmost step of a CONDITION. */
typedef enum sxml_xslt_pattern_prefix_kind {
    /* "/" or "//" at the start: the root, a document node. */
    SXML_XSLT_PATTERN_PREFIX_KIND_DOCUMENT,
    /* A rooted path's variable and predicates (node). */
    SXML_XSLT_PATTERN_PREFIX_KIND_ROOTED,
    /* A pattern node, itself with a prefix (parent). */
    SXML_XSLT_PATTERN_PREFIX_KIND_PATTERN,
} sxml_xslt_pattern_prefix_kind;

typedef struct sxml_xslt_pattern_prefix {
    sxml_xslt_pattern_prefix_kind prefix_kind;
    sxml_ast_node_index node;
    /* SLASH or DOUBLE_SLASH: how the prefix joins the step. */
    sxml_ast_operator separator;
    uint32_t parent;
} sxml_xslt_pattern_prefix;

typedef struct sxml_xslt_pattern_builder {
    const sxml_xslt_pattern_match_expression_request *request;
    const sxml_ast *pattern;
    sxml_ast *output;
    sxml_ast_text texts[SXML_XSLT_PATTERN_TEXT_KIND_COUNT];
    sxml_xslt_pattern_task *tasks;
    size_t task_count;
    size_t task_capacity;
    /* Entry 0 is unused, so index 0 means no prefix. */
    sxml_xslt_pattern_prefix *prefixes;
    uint32_t prefix_count;
    uint32_t prefix_capacity;
    /* The span new nodes get: that of the pattern node being turned. */
    sxml_ast_source_span span;
    /* Per pattern node, true for a name test read as schema-element()
       (6.9.4, typed modes); NULL for none. */
    bool *schema_element_tests;
} sxml_xslt_pattern_builder;

/* Grows an array of the builder to hold one more element. */
typedef struct sxml_xslt_pattern_growth {
    void **elements;
    size_t element_size;
    size_t element_alignment;
    size_t element_count;
    size_t *capacity;
} sxml_xslt_pattern_growth;

static sxml_status sxml_xslt_pattern_grow(const sxml_xslt_pattern_builder *builder,
                                          sxml_xslt_pattern_growth growth) {
    if (growth.element_count < *growth.capacity) {
        return SXML_STATUS_OK;
    }
    const size_t new_capacity = *growth.capacity < 16U ? 16U : *growth.capacity * 2U;
    if (new_capacity > SIZE_MAX / growth.element_size) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        builder->request->allocator,
        (sxml_allocation_request){.byte_count = new_capacity * growth.element_size,
                                  .alignment = growth.element_alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (growth.element_count > 0) {
        memcpy(memory, *growth.elements, growth.element_count * growth.element_size);
    }
    sxml_allocator_release(builder->request->allocator, *growth.elements);
    *growth.elements = memory;
    *growth.capacity = new_capacity;
    return SXML_STATUS_OK;
}

static sxml_status sxml_xslt_pattern_push(sxml_xslt_pattern_builder *builder,
                                          sxml_xslt_pattern_task task) {
    void *tasks = builder->tasks;
    const sxml_status status = sxml_xslt_pattern_grow(
        builder, (sxml_xslt_pattern_growth){
                     .elements = &tasks,
                     .element_size = sizeof(sxml_xslt_pattern_task),
                     .element_alignment = alignof(sxml_xslt_pattern_task),
                     .element_count = builder->task_count,
                     .capacity = &builder->task_capacity});
    builder->tasks = tasks;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    builder->tasks[builder->task_count] = task;
    builder->task_count += 1;
    return SXML_STATUS_OK;
}

/* A task of kind on source at destination. */
typedef struct sxml_xslt_pattern_simple_task {
    sxml_xslt_pattern_task_kind task_kind;
    sxml_ast_node_index source;
    sxml_xslt_pattern_destination destination;
} sxml_xslt_pattern_simple_task;

static sxml_status sxml_xslt_pattern_push_simple(sxml_xslt_pattern_builder *builder,
                                                 sxml_xslt_pattern_simple_task task) {
    return sxml_xslt_pattern_push(
        builder, (sxml_xslt_pattern_task){.task_kind = task.task_kind,
                                          .source = task.source,
                                          .destination = task.destination,
                                          .prefix = 0,
                                          .axis = SXML_AST_AXIS_NONE,
                                          .is_leading = false});
}

static sxml_status sxml_xslt_pattern_push_match(sxml_xslt_pattern_builder *builder,
                                                sxml_ast_node_index source,
                                                sxml_xslt_pattern_destination destination) {
    return sxml_xslt_pattern_push_simple(
        builder, (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_MATCH,
                                                 .source = source,
                                                 .destination = destination});
}

static sxml_status sxml_xslt_pattern_push_copy(sxml_xslt_pattern_builder *builder,
                                               sxml_ast_node_index source,
                                               sxml_xslt_pattern_destination destination) {
    return sxml_xslt_pattern_push_simple(
        builder, (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_COPY,
                                                 .source = source,
                                                 .destination = destination});
}

/* A CONDITION or PREFIX_CONDITION task. */
typedef struct sxml_xslt_pattern_condition_task {
    sxml_xslt_pattern_task_kind task_kind;
    sxml_ast_node_index source;
    uint32_t prefix;
} sxml_xslt_pattern_condition_task;

static sxml_status sxml_xslt_pattern_push_condition(
    sxml_xslt_pattern_builder *builder, sxml_xslt_pattern_condition_task task,
    sxml_xslt_pattern_destination destination) {
    return sxml_xslt_pattern_push(
        builder, (sxml_xslt_pattern_task){.task_kind = task.task_kind,
                                          .source = task.source,
                                          .destination = destination,
                                          .prefix = task.prefix,
                                          .axis = SXML_AST_AXIS_NONE,
                                          .is_leading = false});
}

/* A task that takes an axis (SELF_TEST, STEP_COPY, ...). */
typedef struct sxml_xslt_pattern_axis_task {
    sxml_xslt_pattern_task_kind task_kind;
    sxml_ast_node_index source;
    sxml_ast_axis axis;
} sxml_xslt_pattern_axis_task;

static sxml_status sxml_xslt_pattern_push_axis_task(
    sxml_xslt_pattern_builder *builder, sxml_xslt_pattern_axis_task task,
    sxml_xslt_pattern_destination destination) {
    return sxml_xslt_pattern_push(
        builder, (sxml_xslt_pattern_task){.task_kind = task.task_kind,
                                          .source = task.source,
                                          .destination = destination,
                                          .prefix = 0,
                                          .axis = task.axis,
                                          .is_leading = false});
}

/* An EQUIVALENT task. */
typedef struct sxml_xslt_pattern_equivalent_task {
    sxml_ast_node_index source;
    bool is_leading;
    sxml_xslt_pattern_destination destination;
} sxml_xslt_pattern_equivalent_task;

static sxml_status
sxml_xslt_pattern_push_equivalent(sxml_xslt_pattern_builder *builder,
                                  sxml_xslt_pattern_equivalent_task task) {
    return sxml_xslt_pattern_push(
        builder, (sxml_xslt_pattern_task){.task_kind = SXML_XSLT_PATTERN_TASK_EQUIVALENT,
                                          .source = task.source,
                                          .destination = task.destination,
                                          .prefix = 0,
                                          .axis = SXML_AST_AXIS_NONE,
                                          .is_leading = task.is_leading});
}

static sxml_status sxml_xslt_pattern_add_prefix(sxml_xslt_pattern_builder *builder,
                                                sxml_xslt_pattern_prefix prefix,
                                                uint32_t *index) {
    if (builder->prefix_count == UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *prefixes = builder->prefixes;
    size_t capacity = builder->prefix_capacity;
    const sxml_status status = sxml_xslt_pattern_grow(
        builder, (sxml_xslt_pattern_growth){
                     .elements = &prefixes,
                     .element_size = sizeof(sxml_xslt_pattern_prefix),
                     .element_alignment = alignof(sxml_xslt_pattern_prefix),
                     .element_count = builder->prefix_count,
                     .capacity = &capacity});
    builder->prefixes = prefixes;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    builder->prefix_capacity = capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)capacity;
    builder->prefixes[builder->prefix_count] = prefix;
    *index = builder->prefix_count;
    builder->prefix_count += 1;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Nodes */

/* A node to add with the builder's current span and no payload. */
typedef struct sxml_xslt_pattern_new_node {
    sxml_xslt_pattern_destination destination;
    sxml_ast_kind kind;
    sxml_ast_detail detail;
    uint32_t flags;
} sxml_xslt_pattern_new_node;

static sxml_status sxml_xslt_pattern_place(sxml_xslt_pattern_builder *builder,
                                           sxml_xslt_pattern_destination destination,
                                           sxml_ast_node_index node) {
    if (destination.parent.value == 0) {
        builder->output->root = node;
        return SXML_STATUS_OK;
    }
    return sxml_ast_append_child(
        builder->output, (sxml_ast_child_request){.parent = destination.parent,
                                                  .field = destination.field,
                                                  .child = node});
}

/* Adds a node with request's kind, detail, flags, span and payload at
   destination. */
typedef struct sxml_xslt_pattern_placed_request {
    const sxml_ast_node_request *request;
    sxml_xslt_pattern_destination destination;
} sxml_xslt_pattern_placed_request;

static sxml_status sxml_xslt_pattern_add_request(sxml_xslt_pattern_builder *builder,
                                                 sxml_xslt_pattern_placed_request placed,
                                                 sxml_ast_node_index *node) {
    const sxml_ast_node_request *request = placed.request;
    const sxml_xslt_pattern_destination destination = placed.destination;
    sxml_ast_node_index added = {0};
    sxml_status status = sxml_ast_add_node(builder->output, request, &added);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_place(builder, destination, added);
    }
    if (status == SXML_STATUS_OK && node != NULL) {
        *node = added;
    }
    return status;
}

static sxml_status sxml_xslt_pattern_add(sxml_xslt_pattern_builder *builder,
                                         sxml_xslt_pattern_new_node new_node,
                                         sxml_ast_node_index *node) {
    const sxml_ast_node_request request = {
        .kind = new_node.kind,
        .detail = new_node.detail,
        .flags = new_node.flags,
        .source_span = builder->span,
        .payload = {.name = {.namespace_prefix = {0},
                             .local_name = {0},
                             .namespace_uri = {0}}}};
    return sxml_xslt_pattern_add_request(builder, (sxml_xslt_pattern_placed_request){.request = &request, .destination = new_node.destination}, node);
}

/* A node of kind (no fields filled) at destination. */
typedef struct sxml_xslt_pattern_kind_node {
    sxml_xslt_pattern_destination destination;
    sxml_ast_kind kind;
} sxml_xslt_pattern_kind_node;

static sxml_status sxml_xslt_pattern_add_kind(sxml_xslt_pattern_builder *builder,
                                              sxml_xslt_pattern_kind_node kind_node,
                                              sxml_ast_node_index *node) {
    const sxml_xslt_pattern_destination destination = kind_node.destination;
    const sxml_ast_kind kind = kind_node.kind;
    return sxml_xslt_pattern_add(builder,
                                 (sxml_xslt_pattern_new_node){
                                     .destination = destination,
                                     .kind = kind,
                                     .detail = {.operator= SXML_AST_OPERATOR_NONE},
                                     .flags = 0},
                                 node);
}

static sxml_xslt_pattern_destination
sxml_xslt_pattern_into(sxml_ast_node_index parent, sxml_ast_field field) {
    return (sxml_xslt_pattern_destination){.parent = parent, .field = field};
}

/* A URI-qualified name Q{namespace_uri}local_name of the builder's
   texts. */
typedef struct sxml_xslt_pattern_qualified_name {
    sxml_xslt_pattern_text namespace_uri;
    sxml_xslt_pattern_text local_name;
} sxml_xslt_pattern_qualified_name;

static sxml_status sxml_xslt_pattern_add_name(sxml_xslt_pattern_builder *builder,
                                              sxml_xslt_pattern_destination destination,
                                              sxml_xslt_pattern_qualified_name name) {
    const sxml_ast_node_request request = {
        .kind = SXML_AST_KIND_URI_QUALIFIED_NAME,
        .detail = {.operator= SXML_AST_OPERATOR_NONE},
        .flags = 0,
        .source_span = builder->span,
        .payload = {.name = {.namespace_prefix = {0},
                             .local_name = builder->texts[name.local_name],
                             .namespace_uri = builder->texts[name.namespace_uri]}}};
    return sxml_xslt_pattern_add_request(builder, (sxml_xslt_pattern_placed_request){.request = &request, .destination = destination}, NULL);
}

/* An integer literal "0" or "1". */
static sxml_status sxml_xslt_pattern_add_integer(sxml_xslt_pattern_builder *builder,
                                                 sxml_xslt_pattern_destination destination,
                                                 sxml_xslt_pattern_text digits) {
    const sxml_ast_node_request request = {
        .kind = SXML_AST_KIND_INTEGER_LITERAL,
        .detail = {.operator= SXML_AST_OPERATOR_NONE},
        .flags = 0,
        .source_span = builder->span,
        .payload = {.text = builder->texts[digits]}};
    return sxml_xslt_pattern_add_request(builder, (sxml_xslt_pattern_placed_request){.request = &request, .destination = destination}, NULL);
}

/* A call of function; writes its ARGUMENT_LIST, where the arguments go in
   field ARGUMENT. */
typedef struct sxml_xslt_pattern_call {
    sxml_xslt_pattern_destination destination;
    sxml_xslt_pattern_function function;
} sxml_xslt_pattern_call;

static sxml_status sxml_xslt_pattern_add_call(sxml_xslt_pattern_builder *builder,
                                              sxml_xslt_pattern_call call_node,
                                              sxml_ast_node_index *arguments) {
    const sxml_xslt_pattern_destination destination = call_node.destination;
    const sxml_xslt_pattern_function function = call_node.function;
    sxml_ast_node_index call = {0};
    sxml_status status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = destination, .kind = SXML_AST_KIND_FUNCTION_CALL}, &call);
    const sxml_xslt_pattern_function_name name =
        sxml_xslt_pattern_function_name_of(function);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_name(
            builder, sxml_xslt_pattern_into(call, SXML_AST_FIELD_NAME),
            (sxml_xslt_pattern_qualified_name){.namespace_uri = name.namespace_uri,
                                               .local_name = name.local_name});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(call, SXML_AST_FIELD_ARGUMENTS), .kind = SXML_AST_KIND_ARGUMENT_LIST}, arguments);
    }
    return status;
}

static sxml_xslt_pattern_destination
sxml_xslt_pattern_argument_of(sxml_ast_node_index arguments) {
    return sxml_xslt_pattern_into(arguments, SXML_AST_FIELD_ARGUMENT);
}

/* A call of function with "." as its only argument. */
static sxml_status
sxml_xslt_pattern_add_call_on_context(sxml_xslt_pattern_builder *builder,
                                      sxml_xslt_pattern_destination destination,
                                      sxml_xslt_pattern_function function) {
    sxml_ast_node_index arguments = {0};
    sxml_status status =
        sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = destination, .function = function}, &arguments);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_argument_of(arguments), .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    return status;
}

/* A call without arguments: fn:true() or fn:false(). */
static sxml_status
sxml_xslt_pattern_add_constant_call(sxml_xslt_pattern_builder *builder,
                                    sxml_xslt_pattern_destination destination,
                                    sxml_xslt_pattern_function function) {
    sxml_ast_node_index arguments = {0};
    return sxml_xslt_pattern_add_call(
        builder, (sxml_xslt_pattern_call){.destination = destination, .function = function},
        &arguments);
}

/* A binary operator node; writes where its operands go. */
typedef struct sxml_xslt_pattern_operands {
    sxml_xslt_pattern_destination left;
    sxml_xslt_pattern_destination right;
} sxml_xslt_pattern_operands;

typedef struct sxml_xslt_pattern_binary {
    sxml_ast_kind kind;
    sxml_ast_operator operator;
} sxml_xslt_pattern_binary;

typedef struct sxml_xslt_pattern_binary_node {
    sxml_xslt_pattern_destination destination;
    sxml_xslt_pattern_binary binary;
} sxml_xslt_pattern_binary_node;

static sxml_status sxml_xslt_pattern_add_binary(sxml_xslt_pattern_builder *builder,
                                                sxml_xslt_pattern_binary_node binary_node,
                                                sxml_xslt_pattern_operands *operands) {
    const sxml_xslt_pattern_destination destination = binary_node.destination;
    const sxml_xslt_pattern_binary binary = binary_node.binary;
    sxml_ast_node_index node = {0};
    const sxml_status status = sxml_xslt_pattern_add(
        builder,
        (sxml_xslt_pattern_new_node){.destination = destination,
                                     .kind = binary.kind,
                                     .detail = {.operator= binary.operator},
                                     .flags = 0},
        &node);
    if (status == SXML_STATUS_OK) {
        *operands = (sxml_xslt_pattern_operands){
            .left = sxml_xslt_pattern_into(node, SXML_AST_FIELD_LEFT),
            .right = sxml_xslt_pattern_into(node, SXML_AST_FIELD_RIGHT)};
    }
    return status;
}

static sxml_status sxml_xslt_pattern_add_and(sxml_xslt_pattern_builder *builder,
                                             sxml_xslt_pattern_destination destination,
                                             sxml_xslt_pattern_operands *operands) {
    return sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = destination, .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_AND_EXPRESSION,
                                   .operator= SXML_AST_OPERATOR_AND }}, operands);
}

static sxml_status sxml_xslt_pattern_add_or(sxml_xslt_pattern_builder *builder,
                                            sxml_xslt_pattern_destination destination,
                                            sxml_xslt_pattern_operands *operands) {
    return sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = destination, .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_OR_EXPRESSION,
                                   .operator= SXML_AST_OPERATOR_OR }}, operands);
}

/* An axis step with a kind test of test_kind (GNODE_TYPE is node()); writes
   the step, whose predicates go in field PREDICATE. */
typedef struct sxml_xslt_pattern_kind_step {
    sxml_ast_axis axis;
    sxml_ast_kind test_kind;
} sxml_xslt_pattern_kind_step;

typedef struct sxml_xslt_pattern_step_node {
    sxml_xslt_pattern_destination destination;
    sxml_xslt_pattern_kind_step step;
} sxml_xslt_pattern_step_node;

static sxml_status sxml_xslt_pattern_add_kind_step(sxml_xslt_pattern_builder *builder,
                                                   sxml_xslt_pattern_step_node step_node,
                                                   sxml_ast_node_index *node) {
    const sxml_xslt_pattern_destination destination = step_node.destination;
    const sxml_xslt_pattern_kind_step step = step_node.step;
    sxml_ast_node_index added = {0};
    sxml_status status = sxml_xslt_pattern_add(
        builder,
        (sxml_xslt_pattern_new_node){.destination = destination,
                                     .kind = SXML_AST_KIND_AXIS_STEP,
                                     .detail = {.axis = step.axis},
                                     .flags = 0},
        &added);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(added, SXML_AST_FIELD_TEST), .kind = step.test_kind}, NULL);
    }
    if (status == SXML_STATUS_OK && node != NULL) {
        *node = added;
    }
    return status;
}

/* A predicate of step or filter node; writes where its expression goes. */
static sxml_status sxml_xslt_pattern_add_predicate(
    sxml_xslt_pattern_builder *builder, sxml_ast_node_index owner,
    sxml_xslt_pattern_destination *expression) {
    sxml_ast_node_index predicate = {0};
    const sxml_status status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(owner, SXML_AST_FIELD_PREDICATE), .kind = SXML_AST_KIND_PREDICATE}, &predicate);
    if (status == SXML_STATUS_OK) {
        *expression = sxml_xslt_pattern_into(predicate, SXML_AST_FIELD_EXPRESSION);
    }
    return status;
}

/* fn:exists(axis::node()) with one predicate; writes where the predicate's
   expression goes. */
typedef struct sxml_xslt_pattern_axis_node {
    sxml_xslt_pattern_destination destination;
    sxml_ast_axis axis;
} sxml_xslt_pattern_axis_node;

static sxml_status sxml_xslt_pattern_add_exists_on_axis(
    sxml_xslt_pattern_builder *builder, sxml_xslt_pattern_axis_node axis_node,
    sxml_xslt_pattern_destination *predicate) {
    const sxml_xslt_pattern_destination destination = axis_node.destination;
    const sxml_ast_axis axis = axis_node.axis;
    sxml_ast_node_index arguments = {0};
    sxml_ast_node_index step = {0};
    sxml_status status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = destination, .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = sxml_xslt_pattern_argument_of(arguments), .step = (sxml_xslt_pattern_kind_step){.axis = axis,
                                          .test_kind = SXML_AST_KIND_GNODE_TYPE}}, &step);
    }
    if (status == SXML_STATUS_OK && predicate != NULL) {
        status = sxml_xslt_pattern_add_predicate(builder, step, predicate);
    }
    return status;
}

/* fn:exists(parent::node()) or fn:empty(parent::node()). */
static sxml_status sxml_xslt_pattern_add_has_parent(
    sxml_xslt_pattern_builder *builder, sxml_xslt_pattern_destination destination,
    sxml_xslt_pattern_function function) {
    sxml_ast_node_index arguments = {0};
    sxml_status status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = destination, .function = function}, &arguments);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = sxml_xslt_pattern_argument_of(arguments), .step = (sxml_xslt_pattern_kind_step){.axis = SXML_AST_AXIS_PARENT,
                                          .test_kind = SXML_AST_KIND_GNODE_TYPE}}, NULL);
    }
    return status;
}

/* ". instance of T" with T of kind item_kind (NONE: the caller adds the
   item type in field ITEM_TYPE of *sequence_type). */
static sxml_status sxml_xslt_pattern_add_instance_of(sxml_xslt_pattern_builder *builder,
                                                    sxml_xslt_pattern_kind_node item_node,
                                                    sxml_ast_node_index *sequence_type) {
    const sxml_xslt_pattern_destination destination = item_node.destination;
    const sxml_ast_kind item_kind = item_node.kind;
    sxml_ast_node_index instance_of = {0};
    sxml_ast_node_index type = {0};
    sxml_status status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = destination, .kind = SXML_AST_KIND_INSTANCE_OF_EXPRESSION}, &instance_of);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(instance_of, SXML_AST_FIELD_VALUE), .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add(
            builder,
            (sxml_xslt_pattern_new_node){
                .destination = sxml_xslt_pattern_into(instance_of, SXML_AST_FIELD_TYPE),
                .kind = SXML_AST_KIND_SEQUENCE_TYPE,
                .detail = {.occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                .flags = 0},
            &type);
    }
    if (status == SXML_STATUS_OK && item_kind != SXML_AST_KIND_NONE) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(type, SXML_AST_FIELD_ITEM_TYPE), .kind = item_kind}, NULL);
    }
    if (status == SXML_STATUS_OK && sequence_type != NULL) {
        *sequence_type = type;
    }
    return status;
}

/* $p:name. */
static sxml_status sxml_xslt_pattern_add_variable(sxml_xslt_pattern_builder *builder,
                                                  sxml_xslt_pattern_destination destination,
                                                  sxml_xslt_pattern_text local_name) {
    sxml_ast_node_index reference = {0};
    const sxml_status status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = destination, .kind = SXML_AST_KIND_VARIABLE_REFERENCE}, &reference);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_pattern_add_name(
        builder, sxml_xslt_pattern_into(reference, SXML_AST_FIELD_NAME),
        (sxml_xslt_pattern_qualified_name){
            .namespace_uri = SXML_XSLT_PATTERN_TEXT_VARIABLE_NAMESPACE,
            .local_name = local_name});
}

/* The binding of a variable (VARIABLE_NAME_AND_TYPE) in destination. */
static sxml_status sxml_xslt_pattern_add_binding_name(
    sxml_xslt_pattern_builder *builder, sxml_xslt_pattern_destination destination,
    sxml_xslt_pattern_text local_name) {
    sxml_ast_node_index variable = {0};
    const sxml_status status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = destination, .kind = SXML_AST_KIND_VARIABLE_NAME_AND_TYPE}, &variable);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_pattern_add_name(
        builder, sxml_xslt_pattern_into(variable, SXML_AST_FIELD_NAME),
        (sxml_xslt_pattern_qualified_name){
            .namespace_uri = SXML_XSLT_PATTERN_TEXT_VARIABLE_NAMESPACE,
            .local_name = local_name});
}

/* "let $p:name := V return R"; writes where V and R go. */
typedef struct sxml_xslt_pattern_let {
    sxml_xslt_pattern_destination value;
    sxml_xslt_pattern_destination result;
} sxml_xslt_pattern_let;

typedef struct sxml_xslt_pattern_let_node {
    sxml_xslt_pattern_destination destination;
    sxml_xslt_pattern_text local_name;
} sxml_xslt_pattern_let_node;

static sxml_status sxml_xslt_pattern_add_let(sxml_xslt_pattern_builder *builder,
                                             sxml_xslt_pattern_let_node let_node,
                                             sxml_xslt_pattern_let *let) {
    const sxml_xslt_pattern_destination destination = let_node.destination;
    const sxml_xslt_pattern_text local_name = let_node.local_name;
    sxml_ast_node_index flwor = {0};
    sxml_ast_node_index clause = {0};
    sxml_ast_node_index binding = {0};
    sxml_ast_node_index return_clause = {0};
    sxml_status status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = destination, .kind = SXML_AST_KIND_FLWOR_EXPRESSION}, &flwor);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(flwor, SXML_AST_FIELD_CLAUSE), .kind = SXML_AST_KIND_LET_CLAUSE}, &clause);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(clause, SXML_AST_FIELD_BINDING), .kind = SXML_AST_KIND_LET_VALUE_BINDING}, &binding);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_binding_name(
            builder, sxml_xslt_pattern_into(binding, SXML_AST_FIELD_VARIABLE),
            local_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(flwor, SXML_AST_FIELD_RETURN), .kind = SXML_AST_KIND_RETURN_CLAUSE}, &return_clause);
    }
    if (status == SXML_STATUS_OK) {
        *let = (sxml_xslt_pattern_let){
            .value = sxml_xslt_pattern_into(binding, SXML_AST_FIELD_VALUE),
            .result = sxml_xslt_pattern_into(return_clause, SXML_AST_FIELD_VALUE)};
    }
    return status;
}

/* "let $p:n := . return R"; writes where R goes. */
static sxml_status
sxml_xslt_pattern_add_item_let(sxml_xslt_pattern_builder *builder,
                               sxml_xslt_pattern_destination destination,
                               sxml_xslt_pattern_destination *result) {
    sxml_xslt_pattern_let let = {0};
    sxml_status status = sxml_xslt_pattern_add_let(builder, (sxml_xslt_pattern_let_node){.destination = destination, .local_name = SXML_XSLT_PATTERN_TEXT_ITEM_VARIABLE}, &let);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = let.value, .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        *result = let.result;
    }
    return status;
}

/* "E[P]": writes where E and P go. */
typedef struct sxml_xslt_pattern_filter {
    sxml_xslt_pattern_destination value;
    sxml_xslt_pattern_destination predicate;
} sxml_xslt_pattern_filter;

static sxml_status sxml_xslt_pattern_add_filter(sxml_xslt_pattern_builder *builder,
                                                sxml_xslt_pattern_destination destination,
                                                sxml_xslt_pattern_filter *filter) {
    sxml_ast_node_index node = {0};
    sxml_xslt_pattern_destination expression = {0};
    sxml_status status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = destination, .kind = SXML_AST_KIND_FILTER_EXPRESSION}, &node);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_predicate(builder, node, &expression);
    }
    if (status == SXML_STATUS_OK) {
        *filter = (sxml_xslt_pattern_filter){
            .value = sxml_xslt_pattern_into(node, SXML_AST_FIELD_VALUE),
            .predicate = expression};
    }
    return status;
}

/* ". is $p:n". */
static sxml_status sxml_xslt_pattern_add_is_item(sxml_xslt_pattern_builder *builder,
                                                 sxml_xslt_pattern_destination destination) {
    sxml_xslt_pattern_operands operands = {0};
    sxml_status status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = destination, .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_COMPARISON_EXPRESSION,
                                   .operator= SXML_AST_OPERATOR_IS }}, &operands);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = operands.left, .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_variable(builder, operands.right,
                                                SXML_XSLT_PATTERN_TEXT_ITEM_VARIABLE);
    }
    return status;
}

/* "E[. is $p:n]": writes where E goes. */
static sxml_status sxml_xslt_pattern_add_is_item_filter(
    sxml_xslt_pattern_builder *builder, sxml_xslt_pattern_destination destination,
    sxml_xslt_pattern_destination *value) {
    sxml_xslt_pattern_filter filter = {0};
    sxml_status status = sxml_xslt_pattern_add_filter(builder, destination, &filter);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_is_item(builder, filter.predicate);
    }
    if (status == SXML_STATUS_OK) {
        *value = filter.value;
    }
    return status;
}

/* The n-th child (from 1) of owner in field PREDICATE. */
static sxml_ast_node_index sxml_xslt_pattern_predicate_at(const sxml_ast *ast,
                                                          sxml_ast_node_index owner,
                                                          uint32_t position) {
    sxml_ast_node_index predicate =
        sxml_xslt_pattern_child(ast, owner, SXML_AST_FIELD_PREDICATE);
    for (uint32_t skipped = 1; skipped < position && predicate.value != 0;
         skipped += 1) {
        predicate = sxml_ast_find_next_with_same_field(ast, predicate);
    }
    return predicate;
}

/*
 * "E[p1]...[pn]" for the predicates of owner, as nested filters (the last
 * predicate outermost); writes where E goes. Without predicates E goes to
 * destination.
 */
typedef struct sxml_xslt_pattern_owner_node {
    sxml_ast_node_index owner;
    sxml_xslt_pattern_destination destination;
} sxml_xslt_pattern_owner_node;

static sxml_status sxml_xslt_pattern_add_predicates(sxml_xslt_pattern_builder *builder,
                                                    sxml_xslt_pattern_owner_node owner_node,
                                                    sxml_xslt_pattern_destination *value) {
    const sxml_ast_node_index owner = owner_node.owner;
    const sxml_xslt_pattern_destination destination = owner_node.destination;
    const sxml_ast *ast = builder->pattern;
    uint32_t position = sxml_xslt_pattern_count_children(
        ast, (sxml_ast_child_query){.parent = owner, .field = SXML_AST_FIELD_PREDICATE});
    sxml_xslt_pattern_destination current = destination;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && position > 0) {
        sxml_ast_node_index filter = {0};
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = current, .kind = SXML_AST_KIND_FILTER_EXPRESSION}, &filter);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_copy(
                builder, sxml_xslt_pattern_predicate_at(ast, owner, position),
                sxml_xslt_pattern_into(filter, SXML_AST_FIELD_PREDICATE));
        }
        current = sxml_xslt_pattern_into(filter, SXML_AST_FIELD_VALUE);
        position -= 1;
    }
    if (status == SXML_STATUS_OK) {
        *value = current;
    }
    return status;
}

/* A chain t1 and (t2 and ... tn): writes where term term_index goes,
   given that term_count terms are joined. */
typedef struct sxml_xslt_pattern_conjunction {
    sxml_xslt_pattern_destination rest;
    size_t term_count;
    size_t term_index;
} sxml_xslt_pattern_conjunction;

static sxml_status
sxml_xslt_pattern_next_term(sxml_xslt_pattern_builder *builder,
                            sxml_xslt_pattern_conjunction *conjunction,
                            sxml_xslt_pattern_destination *term) {
    conjunction->term_index += 1;
    if (conjunction->term_index == conjunction->term_count) {
        *term = conjunction->rest;
        return SXML_STATUS_OK;
    }
    sxml_xslt_pattern_operands operands = {0};
    const sxml_status status =
        sxml_xslt_pattern_add_and(builder, conjunction->rest, &operands);
    if (status == SXML_STATUS_OK) {
        *term = operands.left;
        conjunction->rest = operands.right;
    }
    return status;
}

/* ------------------------------------------------- Task processing */

/*
 * MATCH. Unions are disjunctions; a simple pattern tests the item (6.3.3);
 * an XNode pattern requires a node (6.3.2.2) and then its CONDITION, or the
 * formal definition when its form needs it (sxml_xslt_pattern_needs_
 * formal_definition).
 */
typedef struct sxml_xslt_pattern_scan_entry sxml_xslt_pattern_scan_entry;
/* Whether an XNode pattern needs the formal definition (see below). */
typedef struct sxml_xslt_pattern_form {
    bool needs_formal_definition;
} sxml_xslt_pattern_form;
static sxml_status
sxml_xslt_pattern_needs_formal_definition(const sxml_xslt_pattern_builder *builder,
                                          sxml_ast_node_index source, sxml_xslt_pattern_form *form);

static sxml_status sxml_xslt_pattern_process_match(sxml_xslt_pattern_builder *builder,
                                                   const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node *pattern = sxml_xslt_pattern_node(ast, task->source);
    sxml_status status = SXML_STATUS_OK;
    switch ((uint32_t)pattern->kind) {
    case SXML_AST_KIND_PATTERN:
    case SXML_AST_KIND_PARENTHESIZED_PATTERN:
        return sxml_xslt_pattern_push_match(
            builder, sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_PATTERN),
            task->destination);
    case SXML_AST_KIND_UNION_PATTERN: {
        sxml_xslt_pattern_operands operands = {0};
        status = sxml_xslt_pattern_add_or(builder, task->destination, &operands);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_match(
                builder, sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_RIGHT),
                operands.right);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_match(
                builder, sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_LEFT),
                operands.left);
        }
        return status;
    }
    case SXML_AST_KIND_SIMPLE_PATTERN: {
        const sxml_ast_node_index item =
            sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_ITEM);
        if (!sxml_xslt_pattern_has_predicate(ast, task->source)) {
            return sxml_xslt_pattern_push_simple(
                builder,
                (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_ITEM,
                                                .source = item,
                                                .destination = task->destination});
        }
        /* fn:exists(.[I][p1]...[pn]): the predicates with a singleton focus
           on the item (6.3.3). */
        sxml_ast_node_index arguments = {0};
        sxml_xslt_pattern_destination innermost = {0};
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = task->destination, .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_predicates(builder, (sxml_xslt_pattern_owner_node){.owner = task->source, .destination = sxml_xslt_pattern_argument_of(arguments)}, &innermost);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (sxml_xslt_pattern_node(ast, item)->kind == SXML_AST_KIND_ANY_ITEM_PATTERN) {
            return sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = innermost, .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
        }
        sxml_xslt_pattern_filter filter = {0};
        status = sxml_xslt_pattern_add_filter(builder, innermost, &filter);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = filter.value, .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_simple(
                builder,
                (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_ITEM,
                                                .source = item,
                                                .destination = filter.predicate});
        }
        return status;
    }
    default: {
        /* An XNode pattern matches only nodes (6.3.2.2: "N is an XNode";
           GNodes, for $v). */
        sxml_xslt_pattern_operands operands = {0};
        status = sxml_xslt_pattern_add_and(builder, task->destination, &operands);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_instance_of(builder, (sxml_xslt_pattern_kind_node){.destination = operands.left, .kind = SXML_AST_KIND_GNODE_TYPE}, NULL);
        }
        sxml_xslt_pattern_form form = {.needs_formal_definition = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_needs_formal_definition(builder, task->source, &form);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (form.needs_formal_definition) {
            return sxml_xslt_pattern_push_simple(
                builder, (sxml_xslt_pattern_simple_task){
                             .task_kind = SXML_XSLT_PATTERN_TASK_EQUIVALENT_MATCH,
                             .source = task->source,
                             .destination = operands.right});
        }
        return sxml_xslt_pattern_push_condition(
            builder,
            (sxml_xslt_pattern_condition_task){.task_kind = SXML_XSLT_PATTERN_TASK_CONDITION,
                                               .source = task->source,
                                               .prefix = 0},
            operands.right);
    }
    }
}

/* Where an XNode pattern's parts may be read from right to left (see
   CONDITION): a part on the right of "/" or "//" (prefixed) must be a
   step, a union or a parenthesized pattern of those; intersect, except
   and a filter on a parenthesized pattern need the formal definition. */
typedef struct sxml_xslt_pattern_scan_entry {
    sxml_ast_node_index node;
    bool is_prefixed;
} sxml_xslt_pattern_scan_entry;

static sxml_status
sxml_xslt_pattern_needs_formal_definition(const sxml_xslt_pattern_builder *builder,
                                          sxml_ast_node_index source, sxml_xslt_pattern_form *form) {
    const sxml_ast *ast = builder->pattern;
    /* Each pattern node is visited once (patterns are trees), so the node
       count bounds the stack. */
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        builder->request->allocator,
        (sxml_allocation_request){
            .byte_count = ((size_t)ast->node_count + 1U) * sizeof(sxml_xslt_pattern_scan_entry),
            .alignment = alignof(sxml_xslt_pattern_scan_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_pattern_scan_entry *stack = memory;
    size_t stack_count = 1;
    stack[0] = (sxml_xslt_pattern_scan_entry){.node = source, .is_prefixed = false};
    bool is_needed = false;
    while (!is_needed && stack_count > 0) {
        stack_count -= 1;
        const sxml_xslt_pattern_scan_entry entry = stack[stack_count];
        const sxml_ast_node *pattern = sxml_xslt_pattern_node(ast, entry.node);
        sxml_ast_node_index first = {0};
        sxml_ast_node_index second = {0};
        bool is_second_prefixed = entry.is_prefixed;
        switch ((uint32_t)pattern->kind) {
        case SXML_AST_KIND_INTERSECT_EXCEPT_PATTERN:
            is_needed = true;
            break;
        case SXML_AST_KIND_UNION_PATTERN:
            first = sxml_xslt_pattern_child(ast, entry.node, SXML_AST_FIELD_LEFT);
            second = sxml_xslt_pattern_child(ast, entry.node, SXML_AST_FIELD_RIGHT);
            break;
        case SXML_AST_KIND_PARENTHESIZED_PATTERN:
            first = sxml_xslt_pattern_child(ast, entry.node, SXML_AST_FIELD_PATTERN);
            break;
        case SXML_AST_KIND_RELATIVE_PATH_PATTERN:
            first = sxml_xslt_pattern_child(ast, entry.node, SXML_AST_FIELD_LEFT);
            second = sxml_xslt_pattern_child(ast, entry.node, SXML_AST_FIELD_RIGHT);
            is_second_prefixed = true;
            break;
        case SXML_AST_KIND_ABSOLUTE_PATH_PATTERN:
        case SXML_AST_KIND_ROOTED_PATH_PATTERN:
            is_needed = entry.is_prefixed;
            second = sxml_xslt_pattern_child(ast, entry.node, SXML_AST_FIELD_PATH);
            is_second_prefixed = true;
            break;
        case SXML_AST_KIND_FILTER_PATTERN:
            is_needed =
                entry.is_prefixed ||
                sxml_xslt_pattern_node(
                    ast, sxml_xslt_pattern_child(ast, entry.node, SXML_AST_FIELD_VALUE))
                        ->kind == SXML_AST_KIND_PARENTHESIZED_PATTERN;
            break;
        case SXML_AST_KIND_FUNCTION_CALL_PATTERN:
        case SXML_AST_KIND_SIMPLE_PATTERN:
            is_needed = entry.is_prefixed;
            break;
        default:
            break;
        }
        if (second.value != 0) {
            stack[stack_count] = (sxml_xslt_pattern_scan_entry){
                .node = second, .is_prefixed = is_second_prefixed};
            stack_count += 1;
        }
        if (first.value != 0) {
            stack[stack_count] = (sxml_xslt_pattern_scan_entry){
                .node = first, .is_prefixed = entry.is_prefixed};
            stack_count += 1;
        }
    }
    sxml_allocator_release(builder->request->allocator, memory);
    form->needs_formal_definition = is_needed;
    return SXML_STATUS_OK;
}

/*
 * The context set of a step that is not leading (see CONDITION): the nodes
 * the item may have been selected from by its axis, and the condition they
 * must meet; for "//" a node selected from one that meets it.
 */
static sxml_ast_axis sxml_xslt_pattern_origin_axis(sxml_ast_axis step_axis) {
    switch (step_axis) {
    case SXML_AST_AXIS_SELF:
        return SXML_AST_AXIS_SELF;
    case SXML_AST_AXIS_DESCENDANT:
        return SXML_AST_AXIS_ANCESTOR;
    case SXML_AST_AXIS_DESCENDANT_OR_SELF:
        return SXML_AST_AXIS_ANCESTOR_OR_SELF;
    case SXML_AST_AXIS_NONE:
    case SXML_AST_AXIS_ANCESTOR:
    case SXML_AST_AXIS_ANCESTOR_OR_SELF:
    case SXML_AST_AXIS_ATTRIBUTE:
    case SXML_AST_AXIS_CHILD:
    case SXML_AST_AXIS_FOLLOWING:
    case SXML_AST_AXIS_FOLLOWING_OR_SELF:
    case SXML_AST_AXIS_FOLLOWING_SIBLING:
    case SXML_AST_AXIS_FOLLOWING_SIBLING_OR_SELF:
    case SXML_AST_AXIS_ITEM:
    case SXML_AST_AXIS_NAMESPACE:
    case SXML_AST_AXIS_PARENT:
    case SXML_AST_AXIS_PRECEDING:
    case SXML_AST_AXIS_PRECEDING_OR_SELF:
    case SXML_AST_AXIS_PRECEDING_SIBLING:
    case SXML_AST_AXIS_PRECEDING_SIBLING_OR_SELF:
        break;
    }
    return SXML_AST_AXIS_PARENT;
}

/* exists(.[p1]...[pn]) at destination: the predicates of step with a
   singleton focus on the item. */
static sxml_status
sxml_xslt_pattern_add_singleton_predicates(sxml_xslt_pattern_builder *builder,
                                           sxml_ast_node_index step,
                                           sxml_xslt_pattern_destination destination) {
    sxml_ast_node_index arguments = {0};
    sxml_xslt_pattern_destination innermost = {0};
    sxml_status status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = destination, .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_predicates(builder, (sxml_xslt_pattern_owner_node){.owner = step, .destination = sxml_xslt_pattern_argument_of(arguments)}, &innermost);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = innermost, .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    return status;
}

/* The condition the origin meets: the prefix after "/", some
   ancestor-or-self meeting it after "//". */
static sxml_status sxml_xslt_pattern_add_prefix_condition(
    sxml_xslt_pattern_builder *builder, uint32_t prefix,
    sxml_xslt_pattern_destination destination) {
    sxml_xslt_pattern_destination target = destination;
    if (builder->prefixes[prefix].separator == SXML_AST_OPERATOR_DOUBLE_SLASH) {
        const sxml_status status = sxml_xslt_pattern_add_exists_on_axis(builder, (sxml_xslt_pattern_axis_node){.destination = destination, .axis = SXML_AST_AXIS_ANCESTOR_OR_SELF}, &target);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return sxml_xslt_pattern_push_condition(
        builder,
        (sxml_xslt_pattern_condition_task){
            .task_kind = SXML_XSLT_PATTERN_TASK_PREFIX_CONDITION,
            .source = builder->prefixes[prefix].node,
            .prefix = prefix},
        target);
}

/* A step on the right of "/" or "//" (6.3.2.3): */
static sxml_status sxml_xslt_pattern_process_step(sxml_xslt_pattern_builder *builder,
                                                  const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node_index step = task->source;
    const sxml_ast_axis axis = sxml_xslt_pattern_step_axis(
        ast, (sxml_xslt_pattern_step_axis_request){.step = step, .is_leading = false});
    const sxml_ast_node_index test = sxml_xslt_pattern_child(ast, step, SXML_AST_FIELD_TEST);
    const sxml_ast_axis origin_axis = sxml_xslt_pattern_origin_axis(axis);
    sxml_status status = SXML_STATUS_OK;
    if (!sxml_xslt_pattern_has_predicate(ast, step) &&
        sxml_xslt_pattern_test_is_simple(ast, test)) {
        /* self-test(T) and exists(origin::node()[C]). */
        sxml_xslt_pattern_operands operands = {0};
        sxml_xslt_pattern_destination condition = {0};
        status = sxml_xslt_pattern_add_and(builder, task->destination, &operands);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_axis_task(
                builder,
                (sxml_xslt_pattern_axis_task){.task_kind = SXML_XSLT_PATTERN_TASK_SELF_TEST,
                                              .source = test,
                                              .axis = axis},
                operands.left);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_exists_on_axis(builder, (sxml_xslt_pattern_axis_node){.destination = operands.right, .axis = origin_axis}, &condition);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_prefix_condition(builder, task->prefix,
                                                            condition);
        }
        return status;
    }
    if (sxml_xslt_pattern_test_is_simple(ast, test) &&
        !sxml_xslt_pattern_has_positional_predicate(ast, step)) {
        /* No predicate counts positions (D149): self-test(T) and
           (exists(origin::node()[C]) and exists(.[p1]...[pn])). */
        sxml_xslt_pattern_operands operands = {0};
        sxml_xslt_pattern_operands rest = {0};
        sxml_xslt_pattern_destination condition = {0};
        status = sxml_xslt_pattern_add_and(builder, task->destination, &operands);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_axis_task(
                builder,
                (sxml_xslt_pattern_axis_task){.task_kind = SXML_XSLT_PATTERN_TASK_SELF_TEST,
                                              .source = test,
                                              .axis = axis},
                operands.left);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_and(builder, operands.right, &rest);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_exists_on_axis(builder, (sxml_xslt_pattern_axis_node){.destination = rest.left, .axis = origin_axis}, &condition);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_prefix_condition(builder, task->prefix,
                                                            condition);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_singleton_predicates(builder, step, rest.right);
        }
        return status;
    }
    /* Positions count among the nodes selected from the origin, so select
       again: let $n := . return exists(origin::node()[C][S[. is $n]]). */
    sxml_xslt_pattern_destination result = {0};
    sxml_xslt_pattern_destination condition = {0};
    sxml_ast_node_index arguments = {0};
    sxml_ast_node_index origin = {0};
    sxml_xslt_pattern_destination selected = {0};
    sxml_xslt_pattern_destination step_destination = {0};
    status = sxml_xslt_pattern_add_item_let(builder, task->destination, &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = result, .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = sxml_xslt_pattern_argument_of(arguments), .step = (sxml_xslt_pattern_kind_step){.axis = origin_axis,
                                          .test_kind = SXML_AST_KIND_GNODE_TYPE}}, &origin);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_predicate(builder, origin, &condition);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_prefix_condition(builder, task->prefix, condition);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_predicate(builder, origin, &selected);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_is_item_filter(builder, selected, &step_destination);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_axis_task(
            builder,
            (sxml_xslt_pattern_axis_task){.task_kind = SXML_XSLT_PATTERN_TASK_STEP_COPY,
                                          .source = step,
                                          .axis = SXML_AST_AXIS_NONE},
            step_destination);
    }
    return status;
}

/* "self::attribute() | self::namespace-node()" at destination. */
static sxml_status
sxml_xslt_pattern_add_attribute_or_namespace(sxml_xslt_pattern_builder *builder,
                                             sxml_xslt_pattern_destination destination) {
    sxml_xslt_pattern_operands operands = {0};
    sxml_status status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = destination, .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_UNION_EXPRESSION,
                                   .operator= SXML_AST_OPERATOR_UNION }}, &operands);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = operands.left, .step = (sxml_xslt_pattern_kind_step){.axis = SXML_AST_AXIS_SELF,
                                          .test_kind = SXML_AST_KIND_ATTRIBUTE_NODE_TYPE}}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = operands.right, .step = (sxml_xslt_pattern_kind_step){.axis = SXML_AST_AXIS_SELF,
                                          .test_kind = SXML_AST_KIND_NAMESPACE_NODE_TYPE}}, NULL);
    }
    return status;
}

/* "empty(parent::node()) or not(self::attribute() | self::namespace-node())":
   a node the root(.)// of 6.3.2.2 reaches, since descendant-or-self never
   gives an attribute or namespace node that has a parent. */
static sxml_status sxml_xslt_pattern_add_reachable(sxml_xslt_pattern_builder *builder,
                                                   sxml_xslt_pattern_destination destination) {
    sxml_xslt_pattern_operands operands = {0};
    sxml_ast_node_index arguments = {0};
    sxml_status status = sxml_xslt_pattern_add_or(builder, destination, &operands);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_has_parent(builder, operands.left,
                                                  SXML_XSLT_PATTERN_FUNCTION_EMPTY);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = operands.right, .function = SXML_XSLT_PATTERN_FUNCTION_NOT}, &arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_attribute_or_namespace(
            builder, sxml_xslt_pattern_argument_of(arguments));
    }
    return status;
}

/* A leading step (6.3.2.2), on the adjusted axes child-or-top,
   attribute-or-top and namespace-or-top: */
static sxml_status
sxml_xslt_pattern_process_leading_step(sxml_xslt_pattern_builder *builder,
                                       const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node_index step = task->source;
    const sxml_ast_axis axis = sxml_xslt_pattern_step_axis(
        ast, (sxml_xslt_pattern_step_axis_request){.step = step, .is_leading = true});
    const sxml_ast_node_index test = sxml_xslt_pattern_child(ast, step, SXML_AST_FIELD_TEST);
    const bool is_simple = sxml_xslt_pattern_test_is_simple(ast, test);
    const bool has_predicate = sxml_xslt_pattern_has_predicate(ast, step) || !is_simple;
    const sxml_xslt_pattern_axis_task self_test = {
        .task_kind = SXML_XSLT_PATTERN_TASK_SELF_TEST, .source = test, .axis = axis};
    sxml_status status = SXML_STATUS_OK;
    sxml_xslt_pattern_destination target = task->destination;
    if ((axis == SXML_AST_AXIS_SELF || axis == SXML_AST_AXIS_DESCENDANT_OR_SELF) &&
        sxml_xslt_pattern_test_may_match_attribute(ast, test)) {
        /* The item is its own context node, and one with a parent must not
           be an attribute or namespace node (6.3.2.2, root(.)//). */
        sxml_xslt_pattern_operands operands = {0};
        status = sxml_xslt_pattern_add_and(builder, target, &operands);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_reachable(builder, operands.right);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        target = operands.left;
    }
    if (has_predicate && is_simple && axis != SXML_AST_AXIS_SELF &&
        !sxml_xslt_pattern_has_positional_predicate(ast, step)) {
        /* No predicate counts positions (D149): the test without
           predicates, and exists(.[p1]...[pn]). */
        sxml_xslt_pattern_operands operands = {0};
        status = sxml_xslt_pattern_add_and(builder, target, &operands);
        sxml_xslt_pattern_destination test_destination = operands.left;
        if (status == SXML_STATUS_OK && axis == SXML_AST_AXIS_DESCENDANT) {
            sxml_xslt_pattern_operands parent_operands = {0};
            status = sxml_xslt_pattern_add_and(builder, operands.left, &parent_operands);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_has_parent(
                    builder, parent_operands.right, SXML_XSLT_PATTERN_FUNCTION_EXISTS);
            }
            test_destination = parent_operands.left;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_axis_task(builder, self_test, test_destination);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_singleton_predicates(builder, step, operands.right);
        }
        return status;
    }
    if (axis == SXML_AST_AXIS_SELF || axis == SXML_AST_AXIS_DESCENDANT_OR_SELF) {
        if (!has_predicate) {
            return sxml_xslt_pattern_push_axis_task(builder, self_test, target);
        }
        if (axis == SXML_AST_AXIS_SELF) {
            sxml_ast_node_index arguments = {0};
            status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = target, .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_push_axis_task(
                    builder,
                    (sxml_xslt_pattern_axis_task){.task_kind = SXML_XSLT_PATTERN_TASK_STEP_COPY,
                                                  .source = step,
                                                  .axis = SXML_AST_AXIS_SELF},
                    sxml_xslt_pattern_argument_of(arguments));
            }
            return status;
        }
    }
    if (!has_predicate) {
        if (axis == SXML_AST_AXIS_DESCENDANT) {
            sxml_xslt_pattern_operands operands = {0};
            status = sxml_xslt_pattern_add_and(builder, target, &operands);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_has_parent(
                    builder, operands.right, SXML_XSLT_PATTERN_FUNCTION_EXISTS);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_push_axis_task(builder, self_test, operands.left);
            }
            return status;
        }
        /* child-or-top, attribute-or-top, namespace-or-top: a node of the
           axis's kinds, with or without a parent. */
        return sxml_xslt_pattern_push_axis_task(builder, self_test, target);
    }
    sxml_xslt_pattern_destination result = {0};
    status = sxml_xslt_pattern_add_item_let(builder, target, &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (axis == SXML_AST_AXIS_DESCENDANT || axis == SXML_AST_AXIS_DESCENDANT_OR_SELF) {
        /* exists(ancestor::node()[S[. is $n]]). */
        sxml_xslt_pattern_destination selected = {0};
        sxml_xslt_pattern_destination step_destination = {0};
        status = sxml_xslt_pattern_add_exists_on_axis(builder, (sxml_xslt_pattern_axis_node){.destination = result, .axis = axis == SXML_AST_AXIS_DESCENDANT ? SXML_AST_AXIS_ANCESTOR
                                             : SXML_AST_AXIS_ANCESTOR_OR_SELF}, &selected);
        if (status == SXML_STATUS_OK) {
            status =
                sxml_xslt_pattern_add_is_item_filter(builder, selected, &step_destination);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_axis_task(
                builder,
                (sxml_xslt_pattern_axis_task){.task_kind = SXML_XSLT_PATTERN_TASK_STEP_COPY,
                                              .source = step,
                                              .axis = SXML_AST_AXIS_NONE},
                step_destination);
        }
        return status;
    }
    /* if (exists(parent::node()))
       then exists((parent::node()/S)[. is $n])
       else (self-test(T) and exists(.[p1]...[pn])) */
    sxml_ast_node_index conditional = {0};
    status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = result, .kind = SXML_AST_KIND_IF_EXPRESSION}, &conditional);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_has_parent(
            builder, sxml_xslt_pattern_into(conditional, SXML_AST_FIELD_CONDITION),
            SXML_XSLT_PATTERN_FUNCTION_EXISTS);
    }
    sxml_ast_node_index arguments = {0};
    sxml_xslt_pattern_destination path_destination = {0};
    sxml_xslt_pattern_operands path = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = sxml_xslt_pattern_into(conditional, SXML_AST_FIELD_THEN), .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_is_item_filter(
            builder, sxml_xslt_pattern_argument_of(arguments), &path_destination);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = path_destination, .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_RELATIVE_PATH_EXPRESSION,
                                       .operator= SXML_AST_OPERATOR_SLASH }}, &path);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = path.left, .step = (sxml_xslt_pattern_kind_step){.axis = SXML_AST_AXIS_PARENT,
                                          .test_kind = SXML_AST_KIND_GNODE_TYPE}}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_axis_task(
            builder,
            (sxml_xslt_pattern_axis_task){.task_kind = SXML_XSLT_PATTERN_TASK_STEP_COPY,
                                          .source = step,
                                          .axis = SXML_AST_AXIS_NONE},
            path.right);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xslt_pattern_destination otherwise =
        sxml_xslt_pattern_into(conditional, SXML_AST_FIELD_ELSE);
    if (!is_simple) {
        /* A dynamic node test or JNode type: only nodes with a parent. */
        return sxml_xslt_pattern_add_constant_call(builder, otherwise, SXML_XSLT_PATTERN_FUNCTION_FALSE);
    }
    sxml_xslt_pattern_operands operands = {0};
    sxml_ast_node_index exists_arguments = {0};
    sxml_xslt_pattern_destination innermost = {0};
    status = sxml_xslt_pattern_add_and(builder, otherwise, &operands);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = operands.right, .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &exists_arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_predicates(builder, (sxml_xslt_pattern_owner_node){.owner = step, .destination = sxml_xslt_pattern_argument_of(exists_arguments)}, &innermost);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = innermost, .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_axis_task(builder, self_test, operands.left);
    }
    return status;
}

/* "let $n := . return exists(E[. is $n])", E at *value. */
static sxml_status sxml_xslt_pattern_add_selected_by(
    sxml_xslt_pattern_builder *builder, sxml_xslt_pattern_destination destination,
    sxml_xslt_pattern_destination *value) {
    sxml_xslt_pattern_destination result = {0};
    sxml_ast_node_index arguments = {0};
    sxml_status status = sxml_xslt_pattern_add_item_let(builder, destination, &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = result, .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_is_item_filter(
            builder, sxml_xslt_pattern_argument_of(arguments), value);
    }
    return status;
}

/*
 * CONDITION: the context node is selected by the XNode pattern part,
 * read from right to left (6.3.2.3); prefix holds what stands left of its
 * leftmost step (0: that step is a leading step).
 */
static sxml_status
sxml_xslt_pattern_process_condition(sxml_xslt_pattern_builder *builder,
                                    const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node_index source = task->source;
    const sxml_ast_node *pattern = sxml_xslt_pattern_node(ast, source);
    sxml_status status = SXML_STATUS_OK;
    uint32_t prefix = 0;
    switch ((uint32_t)pattern->kind) {
    case SXML_AST_KIND_RELATIVE_PATH_PATTERN: {
        /* The left part, joined by the operator, is the prefix of the
           right part; a parenthesized right part is read in place (path
           expressions are associative, "/" distributes over "|"). */
        status = sxml_xslt_pattern_add_prefix(
            builder,
            (sxml_xslt_pattern_prefix){
                .prefix_kind = SXML_XSLT_PATTERN_PREFIX_KIND_PATTERN,
                .node = sxml_xslt_pattern_child(ast, source, SXML_AST_FIELD_LEFT),
                .separator = pattern->detail.operator,
                .parent = task->prefix},
            &prefix);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xslt_pattern_push_condition(
            builder,
            (sxml_xslt_pattern_condition_task){
                .task_kind = SXML_XSLT_PATTERN_TASK_CONDITION,
                .source = sxml_xslt_pattern_child(ast, source, SXML_AST_FIELD_RIGHT),
                .prefix = prefix},
            task->destination);
    }
    case SXML_AST_KIND_AXIS_STEP_PATTERN:
        if (task->prefix == 0) {
            return sxml_xslt_pattern_process_leading_step(builder, task);
        }
        return sxml_xslt_pattern_process_step(builder, task);
    case SXML_AST_KIND_PARENTHESIZED_PATTERN:
        return sxml_xslt_pattern_push_condition(
            builder,
            (sxml_xslt_pattern_condition_task){
                .task_kind = SXML_XSLT_PATTERN_TASK_CONDITION,
                .source = sxml_xslt_pattern_child(ast, source, SXML_AST_FIELD_PATTERN),
                .prefix = task->prefix},
            task->destination);
    case SXML_AST_KIND_UNION_PATTERN: {
        sxml_xslt_pattern_operands operands = {0};
        status = sxml_xslt_pattern_add_or(builder, task->destination, &operands);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_condition(
                builder,
                (sxml_xslt_pattern_condition_task){
                    .task_kind = SXML_XSLT_PATTERN_TASK_CONDITION,
                    .source = sxml_xslt_pattern_child(ast, source, SXML_AST_FIELD_RIGHT),
                    .prefix = task->prefix},
                operands.right);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_condition(
                builder,
                (sxml_xslt_pattern_condition_task){
                    .task_kind = SXML_XSLT_PATTERN_TASK_CONDITION,
                    .source = sxml_xslt_pattern_child(ast, source, SXML_AST_FIELD_LEFT),
                    .prefix = task->prefix},
                operands.left);
        }
        return status;
    }
    case SXML_AST_KIND_ABSOLUTE_PATH_PATTERN:
    case SXML_AST_KIND_ROOTED_PATH_PATTERN: {
        const sxml_ast_node_index path =
            sxml_xslt_pattern_child(ast, source, SXML_AST_FIELD_PATH);
        const bool is_rooted = pattern->kind == SXML_AST_KIND_ROOTED_PATH_PATTERN;
        if (path.value == 0) {
            if (is_rooted) {
                return sxml_xslt_pattern_push_simple(
                    builder, (sxml_xslt_pattern_simple_task){
                                 .task_kind = SXML_XSLT_PATTERN_TASK_MEMBERSHIP,
                                 .source = source,
                                 .destination = task->destination});
            }
            /* "/": self::document-node() (6.3.2.2). */
            return sxml_xslt_pattern_add_instance_of(builder, (sxml_xslt_pattern_kind_node){.destination = task->destination, .kind = SXML_AST_KIND_DOCUMENT_NODE_TYPE}, NULL);
        }
        status = sxml_xslt_pattern_add_prefix(
            builder,
            (sxml_xslt_pattern_prefix){
                .prefix_kind = is_rooted ? SXML_XSLT_PATTERN_PREFIX_KIND_ROOTED
                                         : SXML_XSLT_PATTERN_PREFIX_KIND_DOCUMENT,
                .node = source,
                .separator = pattern->detail.operator,
                .parent = 0},
            &prefix);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xslt_pattern_push_condition(
            builder,
            (sxml_xslt_pattern_condition_task){.task_kind = SXML_XSLT_PATTERN_TASK_CONDITION,
                                               .source = path,
                                               .prefix = prefix},
            task->destination);
    }
    case SXML_AST_KIND_FUNCTION_CALL_PATTERN:
    case SXML_AST_KIND_FILTER_PATTERN: {
        /* "let $n := . return exists(F[p1]...[pn][. is $n])", F evaluated
           with the context node as focus. */
        sxml_xslt_pattern_destination value = {0};
        status = sxml_xslt_pattern_add_selected_by(builder, task->destination, &value);
        sxml_ast_node_index call = source;
        if (status == SXML_STATUS_OK && pattern->kind == SXML_AST_KIND_FILTER_PATTERN) {
            status = sxml_xslt_pattern_add_predicates(builder, (sxml_xslt_pattern_owner_node){.owner = source, .destination = value}, &value);
            call = sxml_xslt_pattern_child(ast, source, SXML_AST_FIELD_VALUE);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_simple(
                builder, (sxml_xslt_pattern_simple_task){
                             .task_kind = SXML_XSLT_PATTERN_TASK_FUNCTION_CALL,
                             .source = call,
                             .destination = value});
        }
        return status;
    }
    case SXML_AST_KIND_SIMPLE_PATTERN:
        return sxml_xslt_pattern_push_match(builder, source, task->destination);
    default:
        return sxml_xslt_pattern_push_simple(
            builder,
            (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_EQUIVALENT_MATCH,
                                            .source = source,
                                            .destination = task->destination});
    }
}

/* PREFIX_CONDITION: the context node is one the prefix selects. */
static sxml_status
sxml_xslt_pattern_process_prefix_condition(sxml_xslt_pattern_builder *builder,
                                           const sxml_xslt_pattern_task *task) {
    const sxml_xslt_pattern_prefix prefix = builder->prefixes[task->prefix];
    switch (prefix.prefix_kind) {
    case SXML_XSLT_PATTERN_PREFIX_KIND_DOCUMENT:
        /* "/" is the root, which must be a document node (XPath 4.0,
           4.6.1). */
        return sxml_xslt_pattern_add_instance_of(builder, (sxml_xslt_pattern_kind_node){.destination = task->destination, .kind = SXML_AST_KIND_DOCUMENT_NODE_TYPE}, NULL);
    case SXML_XSLT_PATTERN_PREFIX_KIND_ROOTED:
        return sxml_xslt_pattern_push_simple(
            builder,
            (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_MEMBERSHIP,
                                            .source = prefix.node,
                                            .destination = task->destination});
    case SXML_XSLT_PATTERN_PREFIX_KIND_PATTERN:
        return sxml_xslt_pattern_push_condition(
            builder,
            (sxml_xslt_pattern_condition_task){.task_kind = SXML_XSLT_PATTERN_TASK_CONDITION,
                                               .source = prefix.node,
                                               .prefix = prefix.parent},
            task->destination);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* MEMBERSHIP: "let $n := . return exists($v[p...][. instance of node()]
   [. is $n])" (6.3.2.2: "$V matches all XNodes in the value of $V"). */
static sxml_status
sxml_xslt_pattern_process_membership(sxml_xslt_pattern_builder *builder,
                                     const sxml_xslt_pattern_task *task) {
    sxml_xslt_pattern_destination value = {0};
    sxml_xslt_pattern_filter node_filter = {0};
    sxml_xslt_pattern_destination innermost = {0};
    sxml_status status = sxml_xslt_pattern_add_selected_by(builder, task->destination, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_filter(builder, value, &node_filter);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_instance_of(builder, (sxml_xslt_pattern_kind_node){.destination = node_filter.predicate, .kind = SXML_AST_KIND_GNODE_TYPE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_predicates(builder, (sxml_xslt_pattern_owner_node){.owner = task->source, .destination = node_filter.value}, &innermost);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_copy(
            builder,
            sxml_xslt_pattern_child(builder->pattern, task->source, SXML_AST_FIELD_VARIABLE),
            innermost);
    }
    return status;
}

/* SELF_TEST: the context node passes the node test as the axis applies it
   (its principal node kind and the node kinds it selects, XPath 4.0,
   4.6.4), without selecting. */
static sxml_status
sxml_xslt_pattern_process_self_test(sxml_xslt_pattern_builder *builder,
                                    const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node *test = sxml_xslt_pattern_node(ast, task->source);
    const sxml_ast_axis axis = task->axis;
    const bool is_attribute_axis = axis == SXML_AST_AXIS_ATTRIBUTE;
    const bool is_namespace_axis = axis == SXML_AST_AXIS_NAMESPACE;
    const bool is_child_axis =
        axis == SXML_AST_AXIS_CHILD || axis == SXML_AST_AXIS_DESCENDANT;
    sxml_status status = SXML_STATUS_OK;
    sxml_ast_node_index step = {0};
    const sxml_xslt_pattern_new_node self_step = {
        .destination = task->destination,
        .kind = SXML_AST_KIND_AXIS_STEP,
        .detail = {.axis = SXML_AST_AXIS_SELF},
        .flags = 0};
    if (test->kind == SXML_AST_KIND_UNION_NODE_TEST) {
        return sxml_xslt_pattern_push_axis_task(
            builder,
            (sxml_xslt_pattern_axis_task){
                .task_kind = SXML_XSLT_PATTERN_TASK_SELF_TEST_UNION,
                .source = sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_TEST),
                .axis = axis},
            task->destination);
    }
    if (sxml_xslt_pattern_is_name_kind(test->kind) || test->kind == SXML_AST_KIND_WILDCARD) {
        if (is_namespace_axis) {
            /* Namespace nodes have a local name (the prefix) and no
               namespace URI. */
            const bool is_any_name = test->kind == SXML_AST_KIND_WILDCARD &&
                                     test->detail.name_test_kind == SXML_NAME_TEST_KIND_ANY_NAME;
            const bool is_local_name =
                test->kind == SXML_AST_KIND_NCNAME ||
                (test->kind == SXML_AST_KIND_WILDCARD &&
                 test->detail.name_test_kind == SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD);
            if (is_any_name) {
                return sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = task->destination, .step = (sxml_xslt_pattern_kind_step){
                        .axis = SXML_AST_AXIS_SELF,
                        .test_kind = SXML_AST_KIND_NAMESPACE_NODE_TYPE}}, NULL);
            }
            if (!is_local_name) {
                return sxml_xslt_pattern_add_constant_call(builder, task->destination, SXML_XSLT_PATTERN_FUNCTION_FALSE);
            }
            sxml_xslt_pattern_operands operands = {0};
            sxml_xslt_pattern_operands comparison = {0};
            status = sxml_xslt_pattern_add_and(builder, task->destination, &operands);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = operands.left, .step = (sxml_xslt_pattern_kind_step){
                        .axis = SXML_AST_AXIS_SELF,
                        .test_kind = SXML_AST_KIND_NAMESPACE_NODE_TYPE}}, NULL);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = operands.right, .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_COMPARISON_EXPRESSION,
                                               .operator= SXML_AST_OPERATOR_VALUE_EQUAL }}, &comparison);
            }
            if (status == SXML_STATUS_OK) {
                sxml_ast_node_index arguments = {0};
                status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = comparison.left, .function = SXML_XSLT_PATTERN_FUNCTION_LOCAL_NAME}, &arguments);
            }
            if (status == SXML_STATUS_OK) {
                const sxml_ast_node_request literal = {
                    .kind = SXML_AST_KIND_STRING_LITERAL,
                    .detail = {.operator= SXML_AST_OPERATOR_NONE},
                    .flags = 0,
                    .source_span = test->source_span,
                    .payload = {.text = test->payload.name.local_name}};
                status = sxml_xslt_pattern_add_request(builder, (sxml_xslt_pattern_placed_request){.request = &literal, .destination = comparison.right}, NULL);
            }
            return status;
        }
        status = sxml_xslt_pattern_add(builder, self_step, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_xslt_pattern_destination test_destination =
            sxml_xslt_pattern_into(step, SXML_AST_FIELD_TEST);
        if (is_attribute_axis) {
            /* self::attribute(N): the attribute axis's principal node kind
               on the self axis. */
            sxml_ast_node_index attribute = {0};
            sxml_ast_node_index names = {0};
            status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = test_destination, .kind = SXML_AST_KIND_ATTRIBUTE_NODE_TYPE}, &attribute);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(attribute, SXML_AST_FIELD_NAME), .kind = SXML_AST_KIND_NAME_TEST_UNION}, &names);
            }
            test_destination = sxml_xslt_pattern_into(names, SXML_AST_FIELD_TEST);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_copy(builder, task->source, test_destination);
        }
        return status;
    }
    const sxml_ast_kind kind = test->kind;
    bool is_possible = true;
    bool needs_exclusion = false;
    sxml_ast_kind replacement = SXML_AST_KIND_NONE;
    if (kind == SXML_AST_KIND_GNODE_TYPE || kind == SXML_AST_KIND_ANY_XNODE_TYPE) {
        if (is_attribute_axis) {
            replacement = SXML_AST_KIND_ATTRIBUTE_NODE_TYPE;
        } else if (is_namespace_axis) {
            replacement = SXML_AST_KIND_NAMESPACE_NODE_TYPE;
        } else {
            needs_exclusion = is_child_axis;
        }
    } else if (kind == SXML_AST_KIND_ATTRIBUTE_NODE_TYPE ||
               kind == SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE) {
        is_possible = !is_child_axis && !is_namespace_axis;
    } else if (kind == SXML_AST_KIND_NAMESPACE_NODE_TYPE) {
        is_possible = !is_child_axis && !is_attribute_axis;
    } else if (kind == SXML_AST_KIND_DOCUMENT_NODE_TYPE) {
        is_possible = !is_child_axis && !is_attribute_axis && !is_namespace_axis;
    } else if (kind != SXML_AST_KIND_JNODE_TYPE && kind != SXML_AST_KIND_DYNAMIC_NODE_TEST) {
        /* element(), text(), comment(), processing-instruction(),
           schema-element(). */
        is_possible = !is_attribute_axis && !is_namespace_axis;
    }
    if (!is_possible) {
        return sxml_xslt_pattern_add_constant_call(builder, task->destination, SXML_XSLT_PATTERN_FUNCTION_FALSE);
    }
    if (replacement != SXML_AST_KIND_NONE) {
        return sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = task->destination, .step = (sxml_xslt_pattern_kind_step){.axis = SXML_AST_AXIS_SELF,
                                          .test_kind = replacement}}, NULL);
    }
    sxml_xslt_pattern_new_node copied_step = self_step;
    sxml_xslt_pattern_operands operands = {0};
    if (needs_exclusion) {
        /* node() on the child and descendant axes: never an attribute,
           namespace or document node. */
        sxml_ast_node_index arguments = {0};
        sxml_xslt_pattern_operands kinds = {0};
        status = sxml_xslt_pattern_add_and(builder, task->destination, &operands);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = operands.right, .function = SXML_XSLT_PATTERN_FUNCTION_NOT}, &arguments);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = sxml_xslt_pattern_argument_of(arguments), .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_UNION_EXPRESSION,
                                           .operator= SXML_AST_OPERATOR_UNION }}, &kinds);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_attribute_or_namespace(builder, kinds.left);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = kinds.right, .step = (sxml_xslt_pattern_kind_step){.axis = SXML_AST_AXIS_SELF,
                                              .test_kind = SXML_AST_KIND_DOCUMENT_NODE_TYPE}}, NULL);
        }
        copied_step.destination = operands.left;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add(builder, copied_step, &step);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_copy(
            builder, task->source, sxml_xslt_pattern_into(step, SXML_AST_FIELD_TEST));
    }
    return status;
}

static sxml_status
sxml_xslt_pattern_process_self_test_union(sxml_xslt_pattern_builder *builder,
                                          const sxml_xslt_pattern_task *task) {
    const sxml_ast_node_index next =
        sxml_ast_find_next_with_same_field(builder->pattern, task->source);
    const sxml_xslt_pattern_axis_task self_test = {
        .task_kind = SXML_XSLT_PATTERN_TASK_SELF_TEST,
        .source = task->source,
        .axis = task->axis};
    if (next.value == 0) {
        return sxml_xslt_pattern_push_axis_task(builder, self_test, task->destination);
    }
    sxml_xslt_pattern_operands operands = {0};
    sxml_status status = sxml_xslt_pattern_add_or(builder, task->destination, &operands);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_axis_task(
            builder,
            (sxml_xslt_pattern_axis_task){.task_kind = SXML_XSLT_PATTERN_TASK_SELF_TEST_UNION,
                                          .source = next,
                                          .axis = task->axis},
            operands.right);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_axis_task(builder, self_test, operands.left);
    }
    return status;
}

/* "let $v := V return SEQUENCE(sequence_pattern)": writes where V goes. */
typedef struct sxml_xslt_pattern_sequence_node {
    sxml_xslt_pattern_destination destination;
    sxml_ast_node_index sequence_pattern;
} sxml_xslt_pattern_sequence_node;

static sxml_status sxml_xslt_pattern_add_sequence_test(
    sxml_xslt_pattern_builder *builder, sxml_xslt_pattern_sequence_node sequence_node,
    sxml_xslt_pattern_destination *value) {
    const sxml_xslt_pattern_destination destination = sequence_node.destination;
    const sxml_ast_node_index sequence_pattern = sequence_node.sequence_pattern;
    sxml_xslt_pattern_let let = {0};
    sxml_status status = sxml_xslt_pattern_add_let(builder, (sxml_xslt_pattern_let_node){.destination = destination, .local_name = SXML_XSLT_PATTERN_TEXT_VALUE_VARIABLE}, &let);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_simple(
            builder,
            (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_SEQUENCE,
                                            .source = sequence_pattern,
                                            .destination = let.result});
    }
    if (status == SXML_STATUS_OK) {
        *value = let.value;
    }
    return status;
}

/* A map entry key, map:contains(., K), map:get(., K): the call with "."
   and a copy of the constant. */
typedef struct sxml_xslt_pattern_keyed_call {
    sxml_xslt_pattern_function function;
    sxml_ast_node_index key;
} sxml_xslt_pattern_keyed_call;

static sxml_status sxml_xslt_pattern_add_keyed_call(
    sxml_xslt_pattern_builder *builder, sxml_xslt_pattern_destination destination,
    sxml_xslt_pattern_keyed_call call) {
    sxml_ast_node_index arguments = {0};
    sxml_status status =
        sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = destination, .function = call.function}, &arguments);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_argument_of(arguments), .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_simple(
            builder,
            (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_CONSTANT,
                                            .source = call.key,
                                            .destination =
                                                sxml_xslt_pattern_argument_of(arguments)});
    }
    return status;
}

/* map:size(map:remove(., (K1, ..., Kn))) eq 0: no entry beyond the listed
   keys (6.3.6). */
static sxml_status sxml_xslt_pattern_add_map_closed(sxml_xslt_pattern_builder *builder,
                                                    sxml_xslt_pattern_destination destination,
                                                    sxml_ast_node_index map) {
    const sxml_ast *ast = builder->pattern;
    sxml_xslt_pattern_operands comparison = {0};
    sxml_ast_node_index size_arguments = {0};
    sxml_ast_node_index remove_arguments = {0};
    sxml_status status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = destination, .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_COMPARISON_EXPRESSION,
                                   .operator= SXML_AST_OPERATOR_VALUE_EQUAL }}, &comparison);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_integer(builder, comparison.right,
                                               SXML_XSLT_PATTERN_TEXT_ZERO);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = comparison.left, .function = SXML_XSLT_PATTERN_FUNCTION_MAP_SIZE}, &size_arguments);
    }
    const sxml_ast_node_index first_entry =
        sxml_xslt_pattern_child(ast, map, SXML_AST_FIELD_ENTRY);
    if (status != SXML_STATUS_OK || first_entry.value == 0) {
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_argument_of(size_arguments), .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
        }
        return status;
    }
    status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = sxml_xslt_pattern_argument_of(size_arguments), .function = SXML_XSLT_PATTERN_FUNCTION_MAP_REMOVE}, &remove_arguments);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_argument_of(remove_arguments), .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    sxml_ast_node_index keys = {0};
    sxml_ast_node_index sequence = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_argument_of(remove_arguments), .kind = SXML_AST_KIND_PARENTHESIZED_EXPRESSION}, &keys);
    }
    const bool has_one_key =
        sxml_ast_find_next_with_same_field(ast, first_entry).value == 0;
    sxml_xslt_pattern_destination key_destination =
        sxml_xslt_pattern_into(keys, SXML_AST_FIELD_EXPRESSION);
    if (status == SXML_STATUS_OK && !has_one_key) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = key_destination, .kind = SXML_AST_KIND_SEQUENCE_EXPRESSION}, &sequence);
        key_destination = sxml_xslt_pattern_into(sequence, SXML_AST_FIELD_ITEM);
    }
    for (sxml_ast_node_index entry = first_entry;
         status == SXML_STATUS_OK && entry.value != 0;
         entry = sxml_ast_find_next_with_same_field(ast, entry)) {
        status = sxml_xslt_pattern_push_simple(
            builder,
            (sxml_xslt_pattern_simple_task){
                .task_kind = SXML_XSLT_PATTERN_TASK_CONSTANT,
                .source = sxml_xslt_pattern_child(ast, entry, SXML_AST_FIELD_KEY),
                .destination = key_destination});
    }
    return status;
}

/* A map pattern (6.3.6): a map, every listed key present with a value
   matching its sequence pattern, and no other key unless open. */
static sxml_status sxml_xslt_pattern_process_map(sxml_xslt_pattern_builder *builder,
                                                 const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node_index map = task->source;
    const bool is_open =
        (sxml_xslt_pattern_node(ast, map)->flags & (uint32_t)SXML_AST_FLAG_OPEN) != 0;
    size_t term_count = is_open ? 1U : 2U;
    for (sxml_ast_node_index entry = sxml_xslt_pattern_child(ast, map, SXML_AST_FIELD_ENTRY);
         entry.value != 0; entry = sxml_ast_find_next_with_same_field(ast, entry)) {
        term_count +=
            sxml_xslt_pattern_child(ast, entry, SXML_AST_FIELD_VALUE).value != 0 ? 2U : 1U;
    }
    sxml_xslt_pattern_conjunction conjunction = {
        .rest = task->destination, .term_count = term_count, .term_index = 0};
    sxml_xslt_pattern_destination term = {0};
    sxml_status status = sxml_xslt_pattern_next_term(builder, &conjunction, &term);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_instance_of(builder, (sxml_xslt_pattern_kind_node){.destination = term, .kind = SXML_AST_KIND_ANY_MAP_TYPE}, NULL);
    }
    for (sxml_ast_node_index entry = sxml_xslt_pattern_child(ast, map, SXML_AST_FIELD_ENTRY);
         status == SXML_STATUS_OK && entry.value != 0;
         entry = sxml_ast_find_next_with_same_field(ast, entry)) {
        const sxml_ast_node_index key =
            sxml_xslt_pattern_child(ast, entry, SXML_AST_FIELD_KEY);
        const sxml_ast_node_index value =
            sxml_xslt_pattern_child(ast, entry, SXML_AST_FIELD_VALUE);
        status = sxml_xslt_pattern_next_term(builder, &conjunction, &term);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_keyed_call(
                builder, term,
                (sxml_xslt_pattern_keyed_call){
                    .function = SXML_XSLT_PATTERN_FUNCTION_MAP_CONTAINS, .key = key});
        }
        if (status == SXML_STATUS_OK && value.value != 0) {
            sxml_xslt_pattern_destination bound = {0};
            status = sxml_xslt_pattern_next_term(builder, &conjunction, &term);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_sequence_test(builder, (sxml_xslt_pattern_sequence_node){.destination = term, .sequence_pattern = value}, &bound);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_keyed_call(
                    builder, bound,
                    (sxml_xslt_pattern_keyed_call){
                        .function = SXML_XSLT_PATTERN_FUNCTION_MAP_GET, .key = key});
            }
        }
    }
    if (status == SXML_STATUS_OK && !is_open) {
        status = sxml_xslt_pattern_next_term(builder, &conjunction, &term);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_map_closed(builder, term, map);
        }
    }
    return status;
}

/* An array pattern (6.3.8): ". instance of array(*) and (every $p in 1 to
   array:size(.) satisfies (let $v := array:get(., $p) return S))". */
static sxml_status sxml_xslt_pattern_process_array(sxml_xslt_pattern_builder *builder,
                                                   const sxml_xslt_pattern_task *task) {
    sxml_xslt_pattern_operands operands = {0};
    sxml_ast_node_index quantified = {0};
    sxml_ast_node_index binding = {0};
    sxml_xslt_pattern_operands range = {0};
    sxml_xslt_pattern_destination member = {0};
    sxml_ast_node_index arguments = {0};
    sxml_status status = sxml_xslt_pattern_add_and(builder, task->destination, &operands);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_instance_of(builder, (sxml_xslt_pattern_kind_node){.destination = operands.left, .kind = SXML_AST_KIND_ANY_ARRAY_TYPE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add(
            builder,
            (sxml_xslt_pattern_new_node){.destination = operands.right,
                                         .kind = SXML_AST_KIND_QUANTIFIED_EXPRESSION,
                                         .detail = {.operator= SXML_AST_OPERATOR_EVERY},
                                         .flags = 0},
            &quantified);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(quantified, SXML_AST_FIELD_BINDING), .kind = SXML_AST_KIND_QUANTIFIER_BINDING}, &binding);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_binding_name(
            builder, sxml_xslt_pattern_into(binding, SXML_AST_FIELD_VARIABLE),
            SXML_XSLT_PATTERN_TEXT_POSITION_VARIABLE);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = sxml_xslt_pattern_into(binding, SXML_AST_FIELD_SEQUENCE), .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_RANGE_EXPRESSION,
                                       .operator= SXML_AST_OPERATOR_TO }}, &range);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_integer(builder, range.left, SXML_XSLT_PATTERN_TEXT_ONE);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call_on_context(builder, range.right,
                                                       SXML_XSLT_PATTERN_FUNCTION_ARRAY_SIZE);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_sequence_test(builder, (sxml_xslt_pattern_sequence_node){.destination = sxml_xslt_pattern_into(quantified, SXML_AST_FIELD_CONDITION), .sequence_pattern = sxml_xslt_pattern_child(builder->pattern, task->source, SXML_AST_FIELD_MEMBER)}, &member);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = member, .function = SXML_XSLT_PATTERN_FUNCTION_ARRAY_GET}, &arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_argument_of(arguments), .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_variable(builder,
                                                sxml_xslt_pattern_argument_of(arguments),
                                                SXML_XSLT_PATTERN_TEXT_POSITION_VARIABLE);
    }
    return status;
}

/* A JNode pattern (6.3.9): a JNode, its jkey equal to the constant
   (fn:atomic-equal) unless the selector is "*", its jvalue matching the
   sequence pattern. */
static sxml_status sxml_xslt_pattern_process_jnode(sxml_xslt_pattern_builder *builder,
                                                   const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node_index selector =
        sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_SELECTOR);
    const bool has_key =
        sxml_xslt_pattern_node(ast, selector)->kind != SXML_AST_KIND_WILDCARD;
    sxml_xslt_pattern_conjunction conjunction = {
        .rest = task->destination, .term_count = has_key ? 3U : 2U, .term_index = 0};
    sxml_xslt_pattern_destination term = {0};
    sxml_status status = sxml_xslt_pattern_next_term(builder, &conjunction, &term);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_instance_of(builder, (sxml_xslt_pattern_kind_node){.destination = term, .kind = SXML_AST_KIND_JNODE_TYPE}, NULL);
    }
    if (status == SXML_STATUS_OK && has_key) {
        /* exists(jkey(.)[atomic-equal(., K)]). */
        sxml_ast_node_index arguments = {0};
        sxml_xslt_pattern_filter filter = {0};
        status = sxml_xslt_pattern_next_term(builder, &conjunction, &term);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = term, .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_filter(
                builder, sxml_xslt_pattern_argument_of(arguments), &filter);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_call_on_context(builder, filter.value,
                                                           SXML_XSLT_PATTERN_FUNCTION_JKEY);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_keyed_call(
                builder, filter.predicate,
                (sxml_xslt_pattern_keyed_call){
                    .function = SXML_XSLT_PATTERN_FUNCTION_ATOMIC_EQUAL, .key = selector});
        }
    }
    sxml_xslt_pattern_destination bound = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_next_term(builder, &conjunction, &term);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_sequence_test(builder, (sxml_xslt_pattern_sequence_node){.destination = term, .sequence_pattern = sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_VALUE)}, &bound);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call_on_context(builder, bound,
                                                       SXML_XSLT_PATTERN_FUNCTION_JVALUE);
    }
    return status;
}

/* ITEM: the item matches the item pattern (6.3.3 to 6.3.9). */
static sxml_status sxml_xslt_pattern_process_item(sxml_xslt_pattern_builder *builder,
                                                  const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node *item = sxml_xslt_pattern_node(ast, task->source);
    sxml_status status = SXML_STATUS_OK;
    sxml_ast_node_index type = {0};
    switch ((uint32_t)item->kind) {
    case SXML_AST_KIND_ANY_ITEM_PATTERN:
        return sxml_xslt_pattern_add_constant_call(builder, task->destination, SXML_XSLT_PATTERN_FUNCTION_TRUE);
    case SXML_AST_KIND_ATOMIC_PATTERN: {
        /* ". instance of T" or ". instance of (T1 | T2 ...)" (6.3.4). */
        status = sxml_xslt_pattern_add_instance_of(builder, (sxml_xslt_pattern_kind_node){.destination = task->destination, .kind = SXML_AST_KIND_NONE}, &type);
        const sxml_ast_node_index first_type =
            sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_TYPE);
        sxml_xslt_pattern_destination type_destination =
            sxml_xslt_pattern_into(type, SXML_AST_FIELD_ITEM_TYPE);
        if (status == SXML_STATUS_OK &&
            sxml_ast_find_next_with_same_field(ast, first_type).value != 0) {
            sxml_ast_node_index choice = {0};
            status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = type_destination, .kind = SXML_AST_KIND_CHOICE_ITEM_TYPE}, &choice);
            type_destination = sxml_xslt_pattern_into(choice, SXML_AST_FIELD_ALTERNATIVE);
        }
        for (sxml_ast_node_index type_name = first_type;
             status == SXML_STATUS_OK && type_name.value != 0;
             type_name = sxml_ast_find_next_with_same_field(ast, type_name)) {
            status = sxml_xslt_pattern_push_copy(builder, type_name, type_destination);
        }
        return status;
    }
    case SXML_AST_KIND_RECORD_PATTERN:
        /* ". instance of N", N a named record type (6.3.7). */
        status = sxml_xslt_pattern_add_instance_of(builder, (sxml_xslt_pattern_kind_node){.destination = task->destination, .kind = SXML_AST_KIND_NONE}, &type);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_copy(
                builder, sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_NAME),
                sxml_xslt_pattern_into(type, SXML_AST_FIELD_ITEM_TYPE));
        }
        return status;
    case SXML_AST_KIND_MAP_PATTERN:
        return sxml_xslt_pattern_process_map(builder, task);
    case SXML_AST_KIND_ARRAY_PATTERN:
        return sxml_xslt_pattern_process_array(builder, task);
    case SXML_AST_KIND_JNODE_PATTERN:
        return sxml_xslt_pattern_process_jnode(builder, task);
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
}

/* SEQUENCE: the value of $v matches the sequence pattern (6.3.5): its count
   fits the occurrence indicator and every item matches the pattern. */
static sxml_status sxml_xslt_pattern_process_sequence(sxml_xslt_pattern_builder *builder,
                                                      const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node *sequence = sxml_xslt_pattern_node(ast, task->source);
    sxml_ast_node_index arguments = {0};
    sxml_status status = SXML_STATUS_OK;
    if (sequence->detail.occurrence == SXML_OCCURRENCE_ZERO) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = task->destination, .function = SXML_XSLT_PATTERN_FUNCTION_EMPTY}, &arguments);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_variable(builder,
                                                    sxml_xslt_pattern_argument_of(arguments),
                                                    SXML_XSLT_PATTERN_TEXT_VALUE_VARIABLE);
        }
        return status;
    }
    const sxml_occurrence occurrence = sequence->detail.occurrence;
    sxml_xslt_pattern_destination every = task->destination;
    if (occurrence != SXML_OCCURRENCE_ZERO_OR_MORE) {
        sxml_xslt_pattern_operands operands = {0};
        status = sxml_xslt_pattern_add_and(builder, task->destination, &operands);
        every = operands.right;
        if (status == SXML_STATUS_OK && occurrence == SXML_OCCURRENCE_ONE_OR_MORE) {
            status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = operands.left, .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_variable(
                    builder, sxml_xslt_pattern_argument_of(arguments),
                    SXML_XSLT_PATTERN_TEXT_VALUE_VARIABLE);
            }
        } else if (status == SXML_STATUS_OK) {
            /* count($v) eq 1, or le 1 for "?". */
            sxml_xslt_pattern_operands comparison = {0};
            status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = operands.left, .binary = (sxml_xslt_pattern_binary){
                    .kind = SXML_AST_KIND_COMPARISON_EXPRESSION,
                    .operator= occurrence == SXML_OCCURRENCE_ZERO_OR_ONE
                                   ? SXML_AST_OPERATOR_VALUE_LESS_OR_EQUAL
                                   : SXML_AST_OPERATOR_VALUE_EQUAL}}, &comparison);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = comparison.left, .function = SXML_XSLT_PATTERN_FUNCTION_COUNT}, &arguments);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_variable(
                    builder, sxml_xslt_pattern_argument_of(arguments),
                    SXML_XSLT_PATTERN_TEXT_VALUE_VARIABLE);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_integer(builder, comparison.right,
                                                       SXML_XSLT_PATTERN_TEXT_ONE);
            }
        }
    }
    /* empty($v[not(M)]): every item matches. */
    sxml_xslt_pattern_filter filter = {0};
    sxml_ast_node_index not_arguments = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = every, .function = SXML_XSLT_PATTERN_FUNCTION_EMPTY}, &arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_filter(
            builder, sxml_xslt_pattern_argument_of(arguments), &filter);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_variable(builder, filter.value,
                                                SXML_XSLT_PATTERN_TEXT_VALUE_VARIABLE);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = filter.predicate, .function = SXML_XSLT_PATTERN_FUNCTION_NOT}, &not_arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_match(
            builder, sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_PATTERN),
            sxml_xslt_pattern_argument_of(not_arguments));
    }
    return status;
}

/* CONSTANT: "-1" and "true()" are pattern syntax; as expressions they are
   a unary minus and a call (XPath 4.0, A.1 Constant). */
static sxml_status sxml_xslt_pattern_process_constant(sxml_xslt_pattern_builder *builder,
                                                      const sxml_xslt_pattern_task *task) {
    const sxml_ast_node *constant = sxml_xslt_pattern_node(builder->pattern, task->source);
    if (constant->kind == SXML_AST_KIND_BOOLEAN_CONSTANT) {
        return sxml_xslt_pattern_add_constant_call(
            builder, task->destination,
            (constant->flags & (uint32_t)SXML_AST_FLAG_TRUE) != 0
                ? SXML_XSLT_PATTERN_FUNCTION_TRUE
                : SXML_XSLT_PATTERN_FUNCTION_FALSE);
    }
    if (constant->kind == SXML_AST_KIND_NEGATIVE_NUMERIC_LITERAL) {
        sxml_ast_node_index minus = {0};
        const sxml_status status = sxml_xslt_pattern_add(
            builder,
            (sxml_xslt_pattern_new_node){.destination = task->destination,
                                         .kind = SXML_AST_KIND_UNARY_EXPRESSION,
                                         .detail = {.operator= SXML_AST_OPERATOR_MINUS},
                                         .flags = 0},
            &minus);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xslt_pattern_push_copy(
            builder, sxml_xslt_pattern_child(builder->pattern, task->source,
                                             SXML_AST_FIELD_VALUE),
            sxml_xslt_pattern_into(minus, SXML_AST_FIELD_OPERAND));
    }
    return sxml_xslt_pattern_push_copy(builder, task->source, task->destination);
}

/* COPY: the node as it is (its payload texts are at the same offsets of
   the output's store), then its children. */
static sxml_status sxml_xslt_pattern_process_copy(sxml_xslt_pattern_builder *builder,
                                                  const sxml_xslt_pattern_task *task) {
    const sxml_ast_node *source = sxml_xslt_pattern_node(builder->pattern, task->source);
    sxml_xslt_pattern_destination destination = task->destination;
    if (builder->schema_element_tests != NULL &&
        builder->schema_element_tests[task->source.value]) {
        /* 6.9.4: the name test E of a typed mode's leading step is
           schema-element(E); the name (a leaf) goes into its NAME. */
        sxml_ast_node_index schema_element = {0};
        const sxml_status status = sxml_xslt_pattern_add_kind(
            builder,
            (sxml_xslt_pattern_kind_node){.destination = destination,
                                          .kind = SXML_AST_KIND_SCHEMA_ELEMENT_NODE_TYPE},
            &schema_element);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        destination = sxml_xslt_pattern_into(schema_element, SXML_AST_FIELD_NAME);
    }
    const sxml_ast_node_request request = {.kind = source->kind,
                                           .detail = source->detail,
                                           .flags = source->flags,
                                           .source_span = source->source_span,
                                           .payload = source->payload};
    sxml_ast_node_index copy = {0};
    sxml_status status =
        sxml_xslt_pattern_add_request(builder, (sxml_xslt_pattern_placed_request){.request = &request, .destination = destination}, &copy);
    if (status == SXML_STATUS_OK && source->first_child.value != 0) {
        status = sxml_xslt_pattern_push_simple(
            builder,
            (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_COPY_SIBLINGS,
                                            .source = source->first_child,
                                            .destination = sxml_xslt_pattern_into(
                                                copy, SXML_AST_FIELD_NONE)});
    }
    return status;
}

/* COPY_SIBLINGS: the source, then (afterwards, so in order) its next
   sibling and the rest. */
static sxml_status
sxml_xslt_pattern_process_copy_siblings(sxml_xslt_pattern_builder *builder,
                                        const sxml_xslt_pattern_task *task) {
    const sxml_ast_node *source = sxml_xslt_pattern_node(builder->pattern, task->source);
    sxml_status status = SXML_STATUS_OK;
    if (source->next_sibling.value != 0) {
        status = sxml_xslt_pattern_push_simple(
            builder,
            (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_COPY_SIBLINGS,
                                            .source = source->next_sibling,
                                            .destination = task->destination});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_copy(
            builder, task->source,
            sxml_xslt_pattern_into(task->destination.parent, source->field));
    }
    return status;
}

/* STEP_COPY and STEP_TEST_COPY: an AXIS_STEP_PATTERN as an AXIS_STEP. */
static sxml_status sxml_xslt_pattern_process_step_copy(sxml_xslt_pattern_builder *builder,
                                                       const sxml_xslt_pattern_task *task) {
    const sxml_ast_node *source = sxml_xslt_pattern_node(builder->pattern, task->source);
    sxml_ast_detail detail = source->detail;
    if (task->axis != SXML_AST_AXIS_NONE) {
        detail.axis = task->axis;
    }
    const sxml_ast_node_request request = {.kind = SXML_AST_KIND_AXIS_STEP,
                                           .detail = detail,
                                           .flags = source->flags,
                                           .source_span = source->source_span,
                                           .payload = source->payload};
    sxml_ast_node_index step = {0};
    sxml_status status =
        sxml_xslt_pattern_add_request(builder, (sxml_xslt_pattern_placed_request){.request = &request, .destination = task->destination}, &step);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (task->task_kind == SXML_XSLT_PATTERN_TASK_STEP_TEST_COPY) {
        return sxml_xslt_pattern_push_copy(
            builder,
            sxml_xslt_pattern_child(builder->pattern, task->source, SXML_AST_FIELD_TEST),
            sxml_xslt_pattern_into(step, SXML_AST_FIELD_TEST));
    }
    return sxml_xslt_pattern_push_simple(
        builder,
        (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_COPY_SIBLINGS,
                                        .source = source->first_child,
                                        .destination =
                                            sxml_xslt_pattern_into(step, SXML_AST_FIELD_NONE)});
}

/* A prefixed function name, checked once the namespaces are known
   (6.3.2). */
static sxml_status sxml_xslt_pattern_check_function(sxml_xslt_pattern_builder *builder,
                                                    sxml_ast_node_index call) {
    const sxml_ast *ast = builder->pattern;
    const sxml_xpath_static_context *static_context = builder->request->static_context;
    const sxml_ast_node_index name = sxml_xslt_pattern_child(ast, call, SXML_AST_FIELD_NAME);
    if (static_context == NULL ||
        sxml_xslt_pattern_node(ast, name)->kind != SXML_AST_KIND_PREFIXED_NAME) {
        return SXML_STATUS_OK;
    }
    sxml_xpath_resolved_name resolved = {.namespace_uri = {0},
                                         .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = ast,
                                   .name_node = name,
                                   .name_role = SXML_XPATH_NAME_ROLE_FUNCTION,
                                   .static_context = static_context,
                                   .error_record = builder->request->error_record},
        &resolved);
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(
            sxml_item_heap_get_name_pool(static_context->item_heap),
            resolved.namespace_uri, &namespace_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xslt_pattern_call_signature signature = {
        .local_name = resolved.local_name,
        .arity = sxml_xslt_pattern_count_children(
            ast, (sxml_ast_child_query){.parent = call, .field = SXML_AST_FIELD_ARGUMENT})};
    if (sxml_byte_view_equals(
            namespace_uri,
            SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_PATTERN_FUNCTION_NAMESPACE_URI)) &&
        sxml_xslt_pattern_is_permitted_call(signature)) {
        return SXML_STATUS_OK;
    }
    return sxml_xpath_report_static_error(
        &(sxml_xpath_static_error){
            .ast = ast,
            .node = call,
            .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XTSE0340"),
            .description = SXML_BYTE_VIEW_FROM_LITERAL(
                "a function call in a pattern must call fn:doc, fn:id, "
                "fn:element-with-id, fn:key or fn:root (XSLT 4.0, 6.3.2)")},
        builder->request->error_record);
}

/* FUNCTION_CALL: the call pattern as a FUNCTION_CALL with an
   ARGUMENT_LIST. */
static sxml_status
sxml_xslt_pattern_process_function_call(sxml_xslt_pattern_builder *builder,
                                        const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    sxml_status status = sxml_xslt_pattern_check_function(builder, task->source);
    sxml_ast_node_index call = {0};
    sxml_ast_node_index arguments = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = task->destination, .kind = SXML_AST_KIND_FUNCTION_CALL}, &call);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_copy(
            builder, sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_NAME),
            sxml_xslt_pattern_into(call, SXML_AST_FIELD_NAME));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(call, SXML_AST_FIELD_ARGUMENTS), .kind = SXML_AST_KIND_ARGUMENT_LIST}, &arguments);
    }
    /* Pushed in reverse so the arguments are added in order. */
    uint32_t position = sxml_xslt_pattern_count_children(
        ast, (sxml_ast_child_query){.parent = task->source, .field = SXML_AST_FIELD_ARGUMENT});
    while (status == SXML_STATUS_OK && position > 0) {
        sxml_ast_node_index argument =
            sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_ARGUMENT);
        for (uint32_t skipped = 1; skipped < position; skipped += 1) {
            argument = sxml_ast_find_next_with_same_field(ast, argument);
        }
        status = sxml_xslt_pattern_push_simple(
            builder,
            (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_CONSTANT,
                                            .source = argument,
                                            .destination =
                                                sxml_xslt_pattern_argument_of(arguments)});
        position -= 1;
    }
    return status;
}

/*
 * EQUIVALENT_MATCH, the formal definition (6.3.2.2) with the candidate
 * context nodes limited to those that can select the item:
 *     let $n := . return
 *     some $x in (ancestor::node() | .[reachable]) satisfies
 *       exists($x/(EE)[. is $n])
 * Steps go down from their context node and the other parts ignore it, so
 * an ancestor-or-self of the item is enough.
 */
static sxml_status
sxml_xslt_pattern_process_equivalent_match(sxml_xslt_pattern_builder *builder,
                                           const sxml_xslt_pattern_task *task) {
    sxml_xslt_pattern_destination result = {0};
    sxml_ast_node_index quantified = {0};
    sxml_ast_node_index binding = {0};
    sxml_xslt_pattern_operands candidates = {0};
    sxml_xslt_pattern_filter self_filter = {0};
    sxml_ast_node_index arguments = {0};
    sxml_xslt_pattern_destination selected = {0};
    sxml_xslt_pattern_operands path = {0};
    sxml_ast_node_index parenthesized = {0};
    sxml_status status = sxml_xslt_pattern_add_item_let(builder, task->destination, &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add(
            builder,
            (sxml_xslt_pattern_new_node){.destination = result,
                                         .kind = SXML_AST_KIND_QUANTIFIED_EXPRESSION,
                                         .detail = {.operator= SXML_AST_OPERATOR_SOME},
                                         .flags = 0},
            &quantified);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(quantified, SXML_AST_FIELD_BINDING), .kind = SXML_AST_KIND_QUANTIFIER_BINDING}, &binding);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_binding_name(
            builder, sxml_xslt_pattern_into(binding, SXML_AST_FIELD_VARIABLE),
            SXML_XSLT_PATTERN_TEXT_CONTEXT_VARIABLE);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = sxml_xslt_pattern_into(binding, SXML_AST_FIELD_SEQUENCE), .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_UNION_EXPRESSION,
                                       .operator= SXML_AST_OPERATOR_UNION }}, &candidates);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = candidates.left, .step = (sxml_xslt_pattern_kind_step){.axis = SXML_AST_AXIS_ANCESTOR,
                                          .test_kind = SXML_AST_KIND_GNODE_TYPE}}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_filter(builder, candidates.right, &self_filter);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = self_filter.value, .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_reachable(builder, self_filter.predicate);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = sxml_xslt_pattern_into(quantified, SXML_AST_FIELD_CONDITION), .function = SXML_XSLT_PATTERN_FUNCTION_EXISTS}, &arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_is_item_filter(
            builder, sxml_xslt_pattern_argument_of(arguments), &selected);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = selected, .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_RELATIVE_PATH_EXPRESSION,
                                       .operator= SXML_AST_OPERATOR_SLASH }}, &path);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_variable(builder, path.left,
                                                SXML_XSLT_PATTERN_TEXT_CONTEXT_VARIABLE);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = path.right, .kind = SXML_AST_KIND_PARENTHESIZED_EXPRESSION}, &parenthesized);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_equivalent(builder, (sxml_xslt_pattern_equivalent_task){.source = task->source, .is_leading = true, .destination = sxml_xslt_pattern_into(parenthesized, SXML_AST_FIELD_EXPRESSION)});
    }
    return status;
}

/* A leading step on child-or-top, attribute-or-top or namespace-or-top
   written out (6.3.2.2):
       (if (exists(parent::node()) or <not of the top kind>)
        then axis::T else .[self-test(T)])[p1]...[pn] */
static sxml_status
sxml_xslt_pattern_add_adjusted_step(sxml_xslt_pattern_builder *builder,
                                    const sxml_xslt_pattern_task *task, sxml_ast_axis axis) {
    sxml_xslt_pattern_destination innermost = {0};
    sxml_ast_node_index parenthesized = {0};
    sxml_ast_node_index conditional = {0};
    sxml_xslt_pattern_operands condition = {0};
    sxml_status status =
        sxml_xslt_pattern_add_predicates(builder, (sxml_xslt_pattern_owner_node){.owner = task->source, .destination = task->destination}, &innermost);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = innermost, .kind = SXML_AST_KIND_PARENTHESIZED_EXPRESSION}, &parenthesized);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into(parenthesized, SXML_AST_FIELD_EXPRESSION), .kind = SXML_AST_KIND_IF_EXPRESSION}, &conditional);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_or(
            builder, sxml_xslt_pattern_into(conditional, SXML_AST_FIELD_CONDITION),
            &condition);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_has_parent(builder, condition.left,
                                                  SXML_XSLT_PATTERN_FUNCTION_EXISTS);
    }
    if (status == SXML_STATUS_OK) {
        if (axis == SXML_AST_AXIS_CHILD) {
            /* A parentless document node's children are still children. */
            status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = condition.right, .step = (sxml_xslt_pattern_kind_step){.axis = SXML_AST_AXIS_SELF,
                                              .test_kind = SXML_AST_KIND_DOCUMENT_NODE_TYPE}}, NULL);
        } else {
            sxml_ast_node_index arguments = {0};
            status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = condition.right, .function = SXML_XSLT_PATTERN_FUNCTION_NOT}, &arguments);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_add_kind_step(builder, (sxml_xslt_pattern_step_node){.destination = sxml_xslt_pattern_argument_of(arguments), .step = (sxml_xslt_pattern_kind_step){
                        .axis = SXML_AST_AXIS_SELF,
                        .test_kind = axis == SXML_AST_AXIS_ATTRIBUTE
                                         ? SXML_AST_KIND_ATTRIBUTE_NODE_TYPE
                                         : SXML_AST_KIND_NAMESPACE_NODE_TYPE}}, NULL);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_axis_task(
            builder,
            (sxml_xslt_pattern_axis_task){.task_kind = SXML_XSLT_PATTERN_TASK_STEP_TEST_COPY,
                                          .source = task->source,
                                          .axis = SXML_AST_AXIS_NONE},
            sxml_xslt_pattern_into(conditional, SXML_AST_FIELD_THEN));
    }
    sxml_xslt_pattern_filter self_filter = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_filter(
            builder, sxml_xslt_pattern_into(conditional, SXML_AST_FIELD_ELSE), &self_filter);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = self_filter.value, .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_axis_task(
            builder,
            (sxml_xslt_pattern_axis_task){
                .task_kind = SXML_XSLT_PATTERN_TASK_SELF_TEST,
                .source = sxml_xslt_pattern_child(builder->pattern, task->source,
                                                  SXML_AST_FIELD_TEST),
                .axis = axis},
            self_filter.predicate);
    }
    return status;
}

/* EQUIVALENT: the equivalent expression of 6.3.2.2: the pattern as an
   expression, its leading steps on the adjusted axes. */
static sxml_status
sxml_xslt_pattern_process_equivalent(sxml_xslt_pattern_builder *builder,
                                     const sxml_xslt_pattern_task *task) {
    const sxml_ast *ast = builder->pattern;
    const sxml_ast_node *pattern = sxml_xslt_pattern_node(ast, task->source);
    const sxml_ast_node_index left = sxml_xslt_pattern_child(ast, task->source,
                                                             SXML_AST_FIELD_LEFT);
    const sxml_ast_node_index right = sxml_xslt_pattern_child(ast, task->source,
                                                              SXML_AST_FIELD_RIGHT);
    sxml_xslt_pattern_operands operands = {0};
    sxml_status status = SXML_STATUS_OK;
    switch ((uint32_t)pattern->kind) {
    case SXML_AST_KIND_UNION_PATTERN:
    case SXML_AST_KIND_INTERSECT_EXCEPT_PATTERN:
    case SXML_AST_KIND_RELATIVE_PATH_PATTERN: {
        sxml_ast_kind kind = SXML_AST_KIND_RELATIVE_PATH_EXPRESSION;
        if (pattern->kind == SXML_AST_KIND_UNION_PATTERN) {
            kind = SXML_AST_KIND_UNION_EXPRESSION;
        } else if (pattern->kind == SXML_AST_KIND_INTERSECT_EXCEPT_PATTERN) {
            kind = SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION;
        }
        status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = task->destination, .binary = (sxml_xslt_pattern_binary){.kind = kind, .operator= pattern->detail.operator}}, &operands);
        if (status == SXML_STATUS_OK) {
            /* The right operand of "/" and "//" holds no leading step. */
            status = sxml_xslt_pattern_push_equivalent(builder, (sxml_xslt_pattern_equivalent_task){.source = right, .is_leading = task->is_leading && pattern->kind != SXML_AST_KIND_RELATIVE_PATH_PATTERN, .destination = operands.right});
        }
        if (status == SXML_STATUS_OK) {
            status =
                sxml_xslt_pattern_push_equivalent(builder, (sxml_xslt_pattern_equivalent_task){.source = left, .is_leading = task->is_leading, .destination = operands.left});
        }
        return status;
    }
    case SXML_AST_KIND_PARENTHESIZED_PATTERN: {
        sxml_ast_node_index parenthesized = {0};
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = task->destination, .kind = SXML_AST_KIND_PARENTHESIZED_EXPRESSION}, &parenthesized);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_equivalent(builder, (sxml_xslt_pattern_equivalent_task){.source = sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_PATTERN), .is_leading = task->is_leading, .destination = sxml_xslt_pattern_into(parenthesized, SXML_AST_FIELD_EXPRESSION)});
        }
        return status;
    }
    case SXML_AST_KIND_ABSOLUTE_PATH_PATTERN: {
        sxml_ast_node_index absolute = {0};
        const sxml_ast_node_index path =
            sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_PATH);
        status = sxml_xslt_pattern_add(
            builder,
            (sxml_xslt_pattern_new_node){.destination = task->destination,
                                         .kind = SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION,
                                         .detail = pattern->detail,
                                         .flags = 0},
            &absolute);
        if (status == SXML_STATUS_OK && path.value != 0) {
            status = sxml_xslt_pattern_push_equivalent(builder, (sxml_xslt_pattern_equivalent_task){.source = path, .is_leading = false, .destination = sxml_xslt_pattern_into(absolute, SXML_AST_FIELD_PATH)});
        }
        return status;
    }
    case SXML_AST_KIND_ROOTED_PATH_PATTERN: {
        const sxml_ast_node_index path =
            sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_PATH);
        sxml_xslt_pattern_destination base = task->destination;
        if (path.value != 0) {
            status = sxml_xslt_pattern_add_binary(builder, (sxml_xslt_pattern_binary_node){.destination = task->destination, .binary = (sxml_xslt_pattern_binary){.kind = SXML_AST_KIND_RELATIVE_PATH_EXPRESSION,
                                           .operator= pattern->detail.operator}}, &operands);
            base = operands.left;
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_pattern_push_equivalent(builder, (sxml_xslt_pattern_equivalent_task){.source = path, .is_leading = false, .destination = operands.right});
            }
        }
        sxml_xslt_pattern_destination innermost = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_predicates(builder, (sxml_xslt_pattern_owner_node){.owner = task->source, .destination = base}, &innermost);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_copy(
                builder,
                sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_VARIABLE),
                innermost);
        }
        return status;
    }
    case SXML_AST_KIND_FUNCTION_CALL_PATTERN:
        return sxml_xslt_pattern_push_simple(
            builder,
            (sxml_xslt_pattern_simple_task){.task_kind = SXML_XSLT_PATTERN_TASK_FUNCTION_CALL,
                                            .source = task->source,
                                            .destination = task->destination});
    case SXML_AST_KIND_FILTER_PATTERN: {
        sxml_xslt_pattern_destination innermost = {0};
        status = sxml_xslt_pattern_add_predicates(builder, (sxml_xslt_pattern_owner_node){.owner = task->source, .destination = task->destination}, &innermost);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_equivalent(builder, (sxml_xslt_pattern_equivalent_task){.source = sxml_xslt_pattern_child(ast, task->source, SXML_AST_FIELD_VALUE), .is_leading = task->is_leading, .destination = innermost});
        }
        return status;
    }
    case SXML_AST_KIND_AXIS_STEP_PATTERN: {
        const sxml_ast_axis axis = sxml_xslt_pattern_step_axis(
            ast, (sxml_xslt_pattern_step_axis_request){.step = task->source,
                                                       .is_leading = task->is_leading});
        if (task->is_leading &&
            (axis == SXML_AST_AXIS_CHILD || axis == SXML_AST_AXIS_ATTRIBUTE ||
             axis == SXML_AST_AXIS_NAMESPACE)) {
            return sxml_xslt_pattern_add_adjusted_step(builder, task, axis);
        }
        return sxml_xslt_pattern_push_axis_task(
            builder,
            (sxml_xslt_pattern_axis_task){
                .task_kind = SXML_XSLT_PATTERN_TASK_STEP_COPY,
                .source = task->source,
                .axis = task->is_leading ? axis : SXML_AST_AXIS_NONE},
            task->destination);
    }
    default: {
        /* A simple pattern inside an XNode pattern: ".[M]". */
        sxml_xslt_pattern_filter filter = {0};
        status = sxml_xslt_pattern_add_filter(builder, task->destination, &filter);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = filter.value, .kind = SXML_AST_KIND_CONTEXT_VALUE_REFERENCE}, NULL);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_push_match(builder, task->source, filter.predicate);
        }
        return status;
    }
    }
}

static sxml_status sxml_xslt_pattern_process(sxml_xslt_pattern_builder *builder,
                                             const sxml_xslt_pattern_task *task) {
    builder->span = sxml_xslt_pattern_node(builder->pattern, task->source)->source_span;
    switch (task->task_kind) {
    case SXML_XSLT_PATTERN_TASK_MATCH:
        return sxml_xslt_pattern_process_match(builder, task);
    case SXML_XSLT_PATTERN_TASK_CONDITION:
        return sxml_xslt_pattern_process_condition(builder, task);
    case SXML_XSLT_PATTERN_TASK_PREFIX_CONDITION:
        return sxml_xslt_pattern_process_prefix_condition(builder, task);
    case SXML_XSLT_PATTERN_TASK_ITEM:
        return sxml_xslt_pattern_process_item(builder, task);
    case SXML_XSLT_PATTERN_TASK_SEQUENCE:
        return sxml_xslt_pattern_process_sequence(builder, task);
    case SXML_XSLT_PATTERN_TASK_SELF_TEST:
        return sxml_xslt_pattern_process_self_test(builder, task);
    case SXML_XSLT_PATTERN_TASK_SELF_TEST_UNION:
        return sxml_xslt_pattern_process_self_test_union(builder, task);
    case SXML_XSLT_PATTERN_TASK_MEMBERSHIP:
        return sxml_xslt_pattern_process_membership(builder, task);
    case SXML_XSLT_PATTERN_TASK_EQUIVALENT:
        return sxml_xslt_pattern_process_equivalent(builder, task);
    case SXML_XSLT_PATTERN_TASK_EQUIVALENT_MATCH:
        return sxml_xslt_pattern_process_equivalent_match(builder, task);
    case SXML_XSLT_PATTERN_TASK_COPY:
        return sxml_xslt_pattern_process_copy(builder, task);
    case SXML_XSLT_PATTERN_TASK_COPY_SIBLINGS:
        return sxml_xslt_pattern_process_copy_siblings(builder, task);
    case SXML_XSLT_PATTERN_TASK_CONSTANT:
        return sxml_xslt_pattern_process_constant(builder, task);
    case SXML_XSLT_PATTERN_TASK_STEP_COPY:
    case SXML_XSLT_PATTERN_TASK_STEP_TEST_COPY:
        return sxml_xslt_pattern_process_step_copy(builder, task);
    case SXML_XSLT_PATTERN_TASK_FUNCTION_CALL:
        return sxml_xslt_pattern_process_function_call(builder, task);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* One attempt with a node capacity; SXML_STATUS_CAPACITY_EXCEEDED asks for
   a larger one. */
static sxml_status sxml_xslt_pattern_build_with_capacity(sxml_xslt_pattern_builder *builder,
                                                         size_t node_capacity) {
    const sxml_ast *pattern = builder->pattern;
    size_t text_byte_capacity = pattern->text_byte_count;
    for (size_t text_index = 0; text_index < SXML_XSLT_PATTERN_TEXT_KIND_COUNT;
         text_index += 1) {
        text_byte_capacity += sxml_xslt_pattern_texts[text_index].byte_count;
    }
    sxml_status status = sxml_ast_create(
        builder->request->allocator,
        &(sxml_ast_configuration){.node_capacity = node_capacity,
                                  .text_byte_capacity = text_byte_capacity,
                                  .source_name = pattern->source_name},
        &builder->output);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The pattern's texts first, so copied payloads keep their offsets. */
    if (pattern->text_byte_count > 0) {
        sxml_ast_text copied = {.byte_offset = 0, .byte_count = 0};
        status = sxml_ast_append_text(
            builder->output,
            (sxml_byte_view){.bytes = pattern->text_bytes,
                             .byte_count = pattern->text_byte_count},
            &copied);
    }
    for (size_t text_index = 0;
         status == SXML_STATUS_OK && text_index < SXML_XSLT_PATTERN_TEXT_KIND_COUNT;
         text_index += 1) {
        builder->texts[text_index] = (sxml_ast_text){.byte_offset = 0, .byte_count = 0};
        status = sxml_ast_append_text(builder->output, sxml_xslt_pattern_texts[text_index],
                                      &builder->texts[text_index]);
    }
    builder->task_count = 0;
    builder->prefix_count = 1;
    builder->span = sxml_xslt_pattern_node(pattern, pattern->root)->source_span;
    sxml_ast_node_index root = {0};
    sxml_ast_node_index arguments = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_kind(builder, (sxml_xslt_pattern_kind_node){.destination = sxml_xslt_pattern_into((sxml_ast_node_index){0}, SXML_AST_FIELD_NONE), .kind = SXML_AST_KIND_XPATH}, &root);
    }
    /* fn:boolean(M): the match function's result is xs:boolean. */
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_call(builder, (sxml_xslt_pattern_call){.destination = sxml_xslt_pattern_into(root, SXML_AST_FIELD_BODY), .function = SXML_XSLT_PATTERN_FUNCTION_BOOLEAN}, &arguments);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_push_match(builder, pattern->root,
                                              sxml_xslt_pattern_argument_of(arguments));
    }
    while (status == SXML_STATUS_OK && builder->task_count > 0) {
        builder->task_count -= 1;
        const sxml_xslt_pattern_task task = builder->tasks[builder->task_count];
        status = sxml_xslt_pattern_process(builder, &task);
    }
    if (status != SXML_STATUS_OK) {
        sxml_ast_destroy(builder->output);
        builder->output = NULL;
    }
    return status;
}

/* The functions whose value no pattern can see, with the static error a
   pattern that calls or names one raises: fn:current-group (14.2.1),
   fn:current-grouping-key (14.2.2), fn:current-merge-group (15.5.1),
   fn:current-merge-key and fn:current-merge-key-array (15.5.2, 15.5.3). */
typedef struct sxml_xslt_pattern_forbidden_function {
    const char *local_name;
    const char *code_local_name;
    const char *description;
} sxml_xslt_pattern_forbidden_function;

static const sxml_xslt_pattern_forbidden_function sxml_xslt_pattern_forbidden_functions[] = {
    {"current-group", "XTSE1060", "fn:current-group is used in a pattern (XSLT 4.0, 14.2.1)"},
    {"current-grouping-key", "XTSE1070",
     "fn:current-grouping-key is used in a pattern (XSLT 4.0, 14.2.2)"},
    {"current-merge-group", "XTSE3470",
     "fn:current-merge-group is used in a pattern (XSLT 4.0, 15.5.1)"},
    {"current-merge-key", "XTSE3500",
     "fn:current-merge-key is used in a pattern (XSLT 4.0, 15.5.3)"},
    {"current-merge-key-array", "XTSE3500",
     "fn:current-merge-key-array is used in a pattern (XSLT 4.0, 15.5.2)"},
};

/* The static error of a call of, or reference to, one of the forbidden
   functions anywhere in the pattern (one pass over its nodes). */
static sxml_status sxml_xslt_pattern_check_forbidden_functions(
    const sxml_xslt_pattern_match_expression_request *request) {
    const sxml_ast *ast = request->pattern_ast;
    const sxml_xpath_static_context *static_context = request->static_context;
    if (static_context == NULL) {
        return SXML_STATUS_OK;
    }
    for (uint32_t node_number = 1; node_number < ast->node_count; node_number += 1) {
        const sxml_ast_node_index node = {.value = node_number};
        const sxml_ast_kind kind = sxml_xslt_pattern_node(ast, node)->kind;
        if (kind != SXML_AST_KIND_FUNCTION_CALL &&
            kind != SXML_AST_KIND_NAMED_FUNCTION_REFERENCE) {
            continue;
        }
        sxml_xpath_resolved_name resolved = {.namespace_uri = {0},
                                             .local_name = {.bytes = NULL, .byte_count = 0}};
        if (sxml_xpath_resolve_name(
                &(sxml_xpath_name_request){
                    .ast = ast,
                    .name_node = sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_NAME),
                    .name_role = SXML_XPATH_NAME_ROLE_FUNCTION,
                    .static_context = static_context,
                    .error_record = NULL},
                &resolved) != SXML_STATUS_OK ||
            resolved.namespace_uri.index !=
                sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS).index) {
            /* An unknown prefix is lowering's error (XPST0081). */
            continue;
        }
        for (size_t function_index = 0;
             function_index < sizeof(sxml_xslt_pattern_forbidden_functions) /
                                  sizeof(sxml_xslt_pattern_forbidden_functions[0]);
             function_index += 1) {
            const sxml_xslt_pattern_forbidden_function *forbidden =
                &sxml_xslt_pattern_forbidden_functions[function_index];
            if (sxml_byte_view_equals(resolved.local_name,
                                      sxml_byte_view_from_c_string(forbidden->local_name))) {
                return sxml_xpath_report_static_error(
                    &(sxml_xpath_static_error){
                        .ast = ast,
                        .node = node,
                        .code_local_name = sxml_byte_view_from_c_string(forbidden->code_local_name),
                        .description = sxml_byte_view_from_c_string(forbidden->description)},
                    request->error_record);
            }
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Typed modes */

/* Whether the element name test at test is declared in the in-scope
   schemas (a global element declaration, 3.13). */
static sxml_status sxml_xslt_pattern_is_declared_element(
    const sxml_xslt_pattern_match_expression_request *request, sxml_ast_node_index test,
    bool *is_declared) {
    const sxml_xpath_static_context *static_context = request->static_context;
    sxml_xpath_resolved_name resolved = {.namespace_uri = {0},
                                         .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = request->pattern_ast,
                                   .name_node = test,
                                   .name_role = SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE,
                                   .static_context = static_context,
                                   .error_record = request->error_record},
        &resolved);
    sxml_name_identifier name = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            sxml_item_heap_get_name_pool(static_context->item_heap),
            &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                                 .local_name = resolved.local_name},
            &name);
    }
    if (status == SXML_STATUS_OK) {
        sxml_schema_scope_element element;
        *is_declared = sxml_schema_scope_find_element(
            static_context->unit_static_context.in_scope_schemas, name, &element);
    }
    return status;
}

/* Marks in tests the name tests of the first steps of the pattern's
   relative paths that a typed mode reads as schema-element() (6.9.4):
   EQNames on an axis of principal node kind element, outside predicates;
   XTSE3105 for one a strict mode finds undeclared. A walk on an explicit
   stack (D71) of at most the node count. */
static sxml_status sxml_xslt_pattern_mark_schema_element_tests(
    const sxml_xslt_pattern_match_expression_request *request, bool *tests) {
    const sxml_ast *ast = request->pattern_ast;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        request->allocator,
        (sxml_allocation_request){.byte_count = ((size_t)ast->node_count + 1U) *
                                                sizeof(sxml_ast_node_index),
                                  .alignment = alignof(sxml_ast_node_index)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ast_node_index *stack = memory;
    size_t stack_count = 1;
    stack[0] = ast->root;
    while (status == SXML_STATUS_OK && stack_count > 0) {
        stack_count -= 1;
        const sxml_ast_node_index node = stack[stack_count];
        const sxml_ast_node *pattern = sxml_xslt_pattern_node(ast, node);
        sxml_ast_node_index first = {0};
        sxml_ast_node_index second = {0};
        switch ((uint32_t)pattern->kind) {
        case SXML_AST_KIND_PATTERN:
        case SXML_AST_KIND_PARENTHESIZED_PATTERN:
            first = sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_PATTERN);
            break;
        case SXML_AST_KIND_UNION_PATTERN:
        case SXML_AST_KIND_INTERSECT_EXCEPT_PATTERN:
            first = sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_LEFT);
            second = sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_RIGHT);
            break;
        case SXML_AST_KIND_RELATIVE_PATH_PATTERN:
            /* The first step is in the left part. */
            first = sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_LEFT);
            break;
        case SXML_AST_KIND_ABSOLUTE_PATH_PATTERN:
        case SXML_AST_KIND_ROOTED_PATH_PATTERN:
            first = sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_PATH);
            break;
        case SXML_AST_KIND_AXIS_STEP_PATTERN: {
            const sxml_ast_axis axis = sxml_xslt_pattern_step_axis(
                ast, (sxml_xslt_pattern_step_axis_request){.step = node, .is_leading = true});
            const sxml_ast_node_index test = sxml_xslt_pattern_child(ast, node, SXML_AST_FIELD_TEST);
            const sxml_ast_node *test_node = sxml_xslt_pattern_node(ast, test);
            if (axis == SXML_AST_AXIS_ATTRIBUTE || axis == SXML_AST_AXIS_NAMESPACE ||
                !sxml_xslt_pattern_is_name_kind(test_node->kind) ||
                test_node->first_child.value != 0) {
                break;
            }
            bool is_declared = false;
            status = sxml_xslt_pattern_is_declared_element(request, test, &is_declared);
            if (status == SXML_STATUS_OK && !is_declared &&
                request->typed_names == SXML_XSLT_PATTERN_TYPED_NAMES_STRICT) {
                status = sxml_xpath_report_static_error(
                    &(sxml_xpath_static_error){
                        .ast = ast,
                        .node = test,
                        .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XTSE3105"),
                        .description = SXML_BYTE_VIEW_FROM_LITERAL(
                            "a template rule of a mode with typed=\"strict\" names an element "
                            "no global element declaration has (XSLT 4.0, 6.9.4)")},
                    request->error_record);
            }
            tests[test.value] = is_declared ||
                                request->typed_names == SXML_XSLT_PATTERN_TYPED_NAMES_STRICT;
            break;
        }
        default:
            break;
        }
        if (second.value != 0) {
            stack[stack_count] = second;
            stack_count += 1;
        }
        if (first.value != 0) {
            stack[stack_count] = first;
            stack_count += 1;
        }
    }
    sxml_allocator_release(request->allocator, memory);
    return status;
}

sxml_status sxml_xslt_pattern_build_match_expression(
    const sxml_xslt_pattern_match_expression_request *request,
    sxml_ast **match_expression) {
    if (request == NULL || match_expression == NULL || request->pattern_ast == NULL ||
        sxml_allocator_is_null(request->allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_ast *pattern = request->pattern_ast;
    if (sxml_xslt_pattern_node(pattern, pattern->root)->kind != SXML_AST_KIND_PATTERN) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_status forbidden = sxml_xslt_pattern_check_forbidden_functions(request);
    if (forbidden != SXML_STATUS_OK) {
        return forbidden;
    }
    sxml_xslt_pattern_builder builder = {
        .request = request,
        .pattern = pattern,
        .output = NULL,
        .tasks = NULL,
        .task_count = 0,
        .task_capacity = 0,
        .prefixes = NULL,
        .prefix_count = 1,
        .prefix_capacity = 0,
        .schema_element_tests = NULL,
    };
    sxml_status status = SXML_STATUS_OK;
    if (request->typed_names != SXML_XSLT_PATTERN_TYPED_NAMES_NONE &&
        request->static_context != NULL) {
        void *memory = NULL;
        status = sxml_allocator_allocate(
            request->allocator,
            (sxml_allocation_request){.byte_count = (size_t)pattern->node_count + 1U,
                                      .alignment = alignof(bool)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        builder.schema_element_tests = memory;
        memset(memory, 0, (size_t)pattern->node_count + 1U);
        status = sxml_xslt_pattern_mark_schema_element_tests(request, builder.schema_element_tests);
    }
    /* Entry 0 of the prefixes means none. */
    uint32_t unused_prefix = 0;
    builder.prefix_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_add_prefix(
        &builder,
        (sxml_xslt_pattern_prefix){.prefix_kind = SXML_XSLT_PATTERN_PREFIX_KIND_DOCUMENT,
                                   .node = {0},
                                   .separator = SXML_AST_OPERATOR_NONE,
                                   .parent = 0},
        &unused_prefix);
    }
    /* Most patterns need fewer than 48 nodes of match expression per
       pattern node; a larger capacity is tried when one does not. */
    size_t node_capacity = 64U + 48U * (size_t)pattern->node_count;
    while (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_build_with_capacity(&builder, node_capacity);
        if (status != SXML_STATUS_CAPACITY_EXCEEDED) {
            break;
        }
        if (node_capacity >= (size_t)(UINT32_MAX - 1U) / 2U) {
            status = SXML_STATUS_LIMIT_EXCEEDED;
            break;
        }
        node_capacity *= 2U;
        status = SXML_STATUS_OK;
    }
    sxml_allocator_release(request->allocator, builder.tasks);
    sxml_allocator_release(request->allocator, builder.prefixes);
    if (builder.schema_element_tests != NULL) {
        sxml_allocator_release(request->allocator, builder.schema_element_tests);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *match_expression = builder.output;
    return SXML_STATUS_OK;
}

/* ===================================================== Match function */

sxml_status sxml_xslt_pattern_lower_match_function(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_pattern_match_function_request *request,
    sxml_ir_function_index *match_function) {
    if (lowerer == NULL || request == NULL || match_function == NULL ||
        request->pattern_ast == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xpath_static_context *static_context =
        request->static_context != NULL ? request->static_context : lowerer->static_context;
    sxml_ast *match_expression = NULL;
    sxml_status status = sxml_xslt_pattern_build_match_expression(
        &(sxml_xslt_pattern_match_expression_request){
            .pattern_ast = request->pattern_ast,
            .static_context = static_context,
            .allocator = lowerer->allocator,
            .error_record = lowerer->error_record,
            .typed_names = request->typed_names},
        &match_expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* function(item()) as xs:boolean (src/ir_internal.h, template rules). */
    const sxml_sequence_type parameter_type = {
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
        .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    sxml_type_identifier signature = {0};
    status = sxml_xpath_lowerer_register_signature(
        lowerer,
        (sxml_xpath_lowering_signature_request){
            .parameter_types = {.sequence_types = &parameter_type, .sequence_type_count = 1},
            .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_BOOLEAN),
                            .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}},
        &signature);
    const sxml_ast_source_span span =
        sxml_ast_get_node(request->pattern_ast, request->pattern_ast->root)->source_span;
    sxml_ir_function_index function = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            lowerer->unit,
            &(sxml_ir_function){.signature = signature,
                                .parameter_count = 1,
                                .slot_count = 1,
                                .source_span = {.byte_offset = span.byte_offset,
                                                .byte_count = span.byte_count,
                                                .line_number = span.line_number,
                                                .column_number = span.column_number}},
            &function);
    }
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = function,
                     .function_level = 0,
                     .slot_count = 1,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED,
                  .slots = {.context_value = {0},
                            .context_position = {0},
                            .context_size = {0}},
                  .fixed_value = {.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = {.value = 1}}},
    };
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        sxml_xpath_lowerer_enter_function(lowerer, &frame);
        status = sxml_xpath_lowerer_lower_embedded(
            lowerer,
            &(sxml_xpath_lowering_embedding){.ast = match_expression,
                                             .node = {0},
                                             .static_context = request->static_context,
                                             .focus = NULL},
            &body);
        sxml_xpath_lowerer_leave_function(lowerer, &frame);
    }
    sxml_ast_destroy(match_expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = function, .function = *sxml_ir_unit_get_function(lowerer->unit, function)};
    replacement.function.body = body;
    replacement.function.slot_count = frame.function.slot_count;
    status = sxml_ir_unit_replace_function(lowerer->unit, &replacement);
    if (status == SXML_STATUS_OK) {
        *match_function = function;
    }
    return status;
}
