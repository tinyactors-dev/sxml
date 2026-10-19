/*
 * The XQuery Update Facility 3.0: the static rules on updating
 * expressions and the update primitives that updating expressions make
 * (src/xquery_update_internal.h; docs/DECISIONS.md D187; packet 7.7a).
 *
 * Golden files: src/xquery_static.c (static checks over the AST, errors
 * as "CODE description"), src/xquery_flwor.c (internal native rows in no
 * function table), src/function_library_schema.c (a copy in a new arena
 * tree whose memory is the item heap's).
 *
 * The static rules (XQUF 3.0, 2.2) give every expression a category:
 * simple, updating, or vacuous (the empty sequence "()", a call of
 * fn:error, and the expressions made only of those, which may stand where
 * either is required). One walk over the AST in post-order, by first
 * child, next sibling and parent links, so no stack at all (D71), keeps a
 * category per node and checks each node's operands as section 2.2 and
 * the extensions of 5.1 to 5.4 say.
 *
 * Section numbers are of the XQuery Update Facility 3.0 (W3C
 * Recommendation, 24 January 2017) unless another specification is named.
 */
#include "xquery_update_internal.h"

#include "cast_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* The namespace of %updating and %simple (XQuery 4.0, 5.15). */
static const sxml_byte_view xquery_namespace_text =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2012/xquery");

/* ----------------------------------------------------------- AST access */

static const sxml_ast_node *node_of(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_get_node(ast, node);
}

static sxml_ast_node_index child_of(const sxml_ast *ast, sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ast_node_index next_of(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_find_next_with_same_field(ast, node);
}

/* ======================================================== Static rules */

/* An expression's category (2.2). SIMPLE is 0, what a node gets that no
   rule below names: literals, names, types, declarations. */
typedef enum update_category {
    SXML_UPDATE_CATEGORY_SIMPLE = 0,
    SXML_UPDATE_CATEGORY_VACUOUS = 1,
    SXML_UPDATE_CATEGORY_UPDATING = 2,
} update_category;

/* What the walk over one module works with. */
typedef struct rules_walk {
    const sxml_xquery_update_rules_request *request;
    const sxml_ast *ast;
    /* One category per AST node, by node index. */
    uint8_t *categories;
    sxml_namespace_uri_identifier xquery_namespace;
} rules_walk;

static update_category category_of(const rules_walk *walk, sxml_ast_node_index node) {
    return node.value == 0 ? SXML_UPDATE_CATEGORY_VACUOUS : (update_category)walk->categories[node.value];
}

static sxml_status report(const rules_walk *walk, sxml_ast_node_index node,
                          sxml_byte_view code_and_description) {
    return sxml_xpath_report_static_error(
        &(sxml_xpath_static_error){
            .ast = walk->ast,
            .node = node,
            .code_local_name = {.bytes = code_and_description.bytes, .byte_count = 8},
            .description = {.bytes = code_and_description.bytes + 9,
                            .byte_count = code_and_description.byte_count - 9}},
        walk->request->error_record);
}

static sxml_status report_updating_operand(const rules_walk *walk, sxml_ast_node_index node) {
    return report(walk, node,
                  SXML_BYTE_VIEW_FROM_LITERAL(
                      "XUST0001 an updating expression is used where only a simple expression "
                      "is allowed (XQuery Update Facility 3.0, 2.2.2)"));
}

/* XUST0001 for the first child of node that is updating. */
static sxml_status require_simple_children(const rules_walk *walk, sxml_ast_node_index node) {
    for (sxml_ast_node_index child = node_of(walk->ast, node)->first_child; child.value != 0;
         child = node_of(walk->ast, child)->next_sibling) {
        if (category_of(walk, child) == SXML_UPDATE_CATEGORY_UPDATING) {
            return report_updating_operand(walk, child);
        }
    }
    return SXML_STATUS_OK;
}

/* XUST0001 for the child of node with field when it is updating. */
static sxml_status require_simple_field(const rules_walk *walk, sxml_ast_node_index node,
                                        sxml_ast_field field) {
    for (sxml_ast_node_index child = child_of(walk->ast, node, field); child.value != 0;
         child = next_of(walk->ast, child)) {
        if (category_of(walk, child) == SXML_UPDATE_CATEGORY_UPDATING) {
            return report_updating_operand(walk, child);
        }
    }
    return SXML_STATUS_OK;
}

/* The branches of a conditional, comma, switch, typeswitch or try/catch
   expression combined (2.2.2): all updating or vacuous, or all simple or
   vacuous (XUST0001 otherwise). */
typedef struct branch_merge {
    bool has_updating;
    bool has_simple;
} branch_merge;

static void merge_branch(branch_merge *merge, update_category category) {
    merge->has_updating = merge->has_updating || category == SXML_UPDATE_CATEGORY_UPDATING;
    merge->has_simple = merge->has_simple || category == SXML_UPDATE_CATEGORY_SIMPLE;
}

/* The children of a node with one field. */
typedef struct field_children {
    sxml_ast_node_index node;
    sxml_ast_field field;
} field_children;

/* The category of the children merged into *merge. */
static void merge_field(const rules_walk *walk, field_children children, branch_merge *merge) {
    for (sxml_ast_node_index child = child_of(walk->ast, children.node, children.field);
         child.value != 0; child = next_of(walk->ast, child)) {
        merge_branch(merge, category_of(walk, child));
    }
}

static sxml_status finish_merge(const rules_walk *walk, sxml_ast_node_index node,
                                branch_merge merge) {
    if (merge.has_updating && merge.has_simple) {
        return report(walk, node,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XUST0001 the branches of an expression mix updating and simple "
                          "expressions (XQuery Update Facility 3.0, 2.2.2)"));
    }
    walk->categories[node.value] = (uint8_t)(merge.has_updating ? SXML_UPDATE_CATEGORY_UPDATING
                                             : merge.has_simple ? SXML_UPDATE_CATEGORY_SIMPLE
                                                                : SXML_UPDATE_CATEGORY_VACUOUS);
    return SXML_STATUS_OK;
}

/* The category of node's child with field (vacuous when absent). */
static void inherit_field(const rules_walk *walk, sxml_ast_node_index node, sxml_ast_field field) {
    walk->categories[node.value] = (uint8_t)category_of(walk, child_of(walk->ast, node, field));
}

/* -------------------------------------------------------- Annotations */

/* Whether the ANNOTATION node is %updating, prefixes resolved with
   static_context (an unprefixed annotation is in the XQuery namespace,
   XQuery 4.0, 5.15). */
typedef struct annotation_test {
    const sxml_ast *ast;
    sxml_ast_node_index annotation;
    const sxml_xpath_static_context *static_context;
} annotation_test;

static sxml_status annotation_is_updating(const annotation_test *test,
                                          const sxml_xquery_annotation_query *query,
                                          bool *is_updating) {
    *is_updating = false;
    const sxml_ast_node *annotation = node_of(test->ast, test->annotation);
    if (annotation->kind == SXML_AST_KIND_COMPATIBILITY_ANNOTATION) {
        *is_updating = true;
        return SXML_STATUS_OK;
    }
    const sxml_ast_node_index name = child_of(test->ast, test->annotation, SXML_AST_FIELD_NAME);
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status =
        sxml_xpath_resolve_name(&(sxml_xpath_name_request){.ast = test->ast,
                                                           .name_node = name,
                                                           .name_role =
                                                               SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                                                           .static_context = test->static_context,
                                                           .error_record = query->error_record},
                                &resolved);
    if (status != SXML_STATUS_OK ||
        !sxml_byte_view_equals(resolved.local_name, SXML_BYTE_VIEW_FROM_LITERAL("updating"))) {
        return status;
    }
    if (node_of(test->ast, name)->kind == SXML_AST_KIND_NCNAME) {
        *is_updating = true;
        return SXML_STATUS_OK;
    }
    sxml_namespace_uri_identifier xquery_namespace = {0};
    status = sxml_name_pool_intern_namespace_uri(
        sxml_item_heap_get_name_pool(test->static_context->item_heap), xquery_namespace_text,
        &xquery_namespace);
    *is_updating = status == SXML_STATUS_OK && resolved.namespace_uri.index == xquery_namespace.index;
    return status;
}

sxml_status sxml_xquery_declaration_is_updating(const sxml_xquery_annotation_query *query,
                                                bool *is_updating) {
    *is_updating = false;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index annotation =
             child_of(query->ast, query->declaration, SXML_AST_FIELD_ANNOTATION);
         status == SXML_STATUS_OK && annotation.value != 0 && !*is_updating;
         annotation = next_of(query->ast, annotation)) {
        status = annotation_is_updating(&(annotation_test){.ast = query->ast,
                                                           .annotation = annotation,
                                                           .static_context = query->static_context},
                                        query, is_updating);
    }
    return status;
}

/* ------------------------------------------------------ Function calls */

/* Whether IR function number function_number is an updating declared
   function of context, or of a module it imports. */
typedef struct declared_function_search {
    const sxml_xquery_static_context *context;
    uint32_t function_number;
    sxml_error_record *error_record;
} declared_function_search;

static sxml_status search_declared_functions(const declared_function_search *search,
                                             bool *is_found, bool *is_updating) {
    const sxml_xquery_static_context *context = search->context;
    for (size_t function_index = 0; function_index < context->declared_function_count;
         function_index += 1) {
        const sxml_xquery_declared_function *function = &context->declared_functions[function_index];
        const uint32_t first = function->first_function.value;
        const uint32_t last = first + function->parameter_count - function->required_count;
        if (function->record_type.index != 0 || search->function_number < first ||
            search->function_number > last) {
            continue;
        }
        *is_found = true;
        return sxml_xquery_declaration_is_updating(
            &(sxml_xquery_annotation_query){.ast = context->ast,
                                            .declaration = function->declaration,
                                            .static_context = &context->xpath,
                                            .error_record = search->error_record},
            is_updating);
    }
    return SXML_STATUS_OK;
}

static sxml_status function_number_is_updating(const rules_walk *walk, uint32_t function_number,
                                               bool *is_updating) {
    const sxml_xquery_static_context *context = walk->request->context;
    bool is_found = false;
    sxml_status status = search_declared_functions(
        &(declared_function_search){.context = context,
                                    .function_number = function_number,
                                    .error_record = walk->request->error_record},
        &is_found, is_updating);
    for (size_t import_index = 0;
         status == SXML_STATUS_OK && !is_found &&
         import_index < context->imported_modules.static_context_count;
         import_index += 1) {
        status = search_declared_functions(
            &(declared_function_search){
                .context = context->imported_modules.static_contexts[import_index],
                .function_number = function_number,
                .error_record = walk->request->error_record},
            &is_found, is_updating);
    }
    return status;
}

/* The category of a FUNCTION_CALL: updating for a call of an updating
   function (2.2.2), vacuous for fn:error (2.2.2, "vacuous expression"),
   simple otherwise, a partial application included. A name that does not
   resolve is simple here: lowering reports it. */
static sxml_status classify_function_call(const rules_walk *walk, sxml_ast_node_index call) {
    const sxml_ast *ast = walk->ast;
    const sxml_ast_node_index arguments = child_of(ast, call, SXML_AST_FIELD_ARGUMENTS);
    size_t arity = 0;
    for (sxml_ast_node_index argument = node_of(ast, arguments)->first_child; argument.value != 0;
         argument = node_of(ast, argument)->next_sibling) {
        if (node_of(ast, argument)->kind == SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
            return SXML_STATUS_OK;
        }
        arity += 1;
    }
    const sxml_xpath_static_context *static_context = &walk->request->context->xpath;
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = ast,
                                   .name_node = child_of(ast, call, SXML_AST_FIELD_NAME),
                                   .name_role = SXML_XPATH_NAME_ROLE_FUNCTION,
                                   .static_context = static_context,
                                   .error_record = NULL},
        &resolved);
    if (status == SXML_STATUS_STATIC_ERROR) {
        return SXML_STATUS_OK;
    }
    sxml_name_identifier function_name = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            sxml_item_heap_get_name_pool(static_context->item_heap),
            &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                                 .local_name = resolved.local_name},
            &function_name);
    }
    sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_find_function(
            static_context->function_table,
            &(sxml_function_query){.function_name = function_name, .arity = arity}, &entry);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NATIVE &&
        entry.native_function->function_namespace == SXML_NAMESPACE_FUNCTIONS &&
        sxml_byte_view_equals(entry.native_function->local_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("error"))) {
        walk->categories[call.value] = SXML_UPDATE_CATEGORY_VACUOUS;
        return SXML_STATUS_OK;
    }
    if (entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NATIVE &&
        sxml_xquery_is_put_function(entry.native_function)) {
        walk->categories[call.value] = SXML_UPDATE_CATEGORY_UPDATING;
        return SXML_STATUS_OK;
    }
    if (entry.entry_kind != SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION) {
        return SXML_STATUS_OK;
    }
    bool is_updating = false;
    status = function_number_is_updating(walk, entry.ir_function_number, &is_updating);
    if (status == SXML_STATUS_OK && is_updating) {
        walk->categories[call.value] = SXML_UPDATE_CATEGORY_UPDATING;
    }
    return status;
}

/* ---------------------------------------------------------- Functions */

/* The body of a function declaration or inline function: updating or
   vacuous for an updating function (XUST0002), not updating otherwise
   (XUST0001); an updating function declares no result type (XUST0028;
   5.4, 5.3). */
static sxml_status check_function_body(const rules_walk *walk, sxml_ast_node_index function) {
    bool is_updating = false;
    sxml_status status = sxml_xquery_declaration_is_updating(
        &(sxml_xquery_annotation_query){.ast = walk->ast,
                                        .declaration = function,
                                        .static_context = &walk->request->context->xpath,
                                        .error_record = walk->request->error_record},
        &is_updating);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ast_node_index body = child_of(walk->ast, function, SXML_AST_FIELD_BODY);
    if (!is_updating) {
        return body.value != 0 && category_of(walk, body) == SXML_UPDATE_CATEGORY_UPDATING
                   ? report_updating_operand(walk, body)
                   : SXML_STATUS_OK;
    }
    if (child_of(walk->ast, function, SXML_AST_FIELD_RETURN_TYPE).value != 0) {
        return report(walk, function,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XUST0028 an updating function declares a result type (XQuery "
                          "Update Facility 3.0, 5.4)"));
    }
    if (body.value != 0 && category_of(walk, body) == SXML_UPDATE_CATEGORY_SIMPLE) {
        return report(walk, body,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XUST0002 the body of an updating function is a simple expression "
                          "that is not vacuous (XQuery Update Facility 3.0, 5.4)"));
    }
    return SXML_STATUS_OK;
}

/* The modify clause of copy-modify and transform-with (5.4.5, 5.4.6). */
static sxml_status require_updating_field(const rules_walk *walk, sxml_ast_node_index node,
                                          sxml_ast_field field) {
    const sxml_ast_node_index modify = child_of(walk->ast, node, field);
    if (modify.value != 0 && category_of(walk, modify) == SXML_UPDATE_CATEGORY_SIMPLE) {
        return report(walk, modify,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XUST0002 the modify clause is a simple expression that is not "
                          "vacuous (XQuery Update Facility 3.0, 5.4.5)"));
    }
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- One node */

/* The extensions of the existing expressions (2.2.2, 5.1, 5.2): their
   operands, and the categories of the expressions that pass a branch's
   on. */
static sxml_status classify_composite(const rules_walk *walk, sxml_ast_node_index node,
                                      bool *is_handled) {
    *is_handled = true;
    branch_merge merge = {.has_updating = false, .has_simple = false};
    sxml_status status = SXML_STATUS_OK;
    switch ((uint32_t)node_of(walk->ast, node)->kind) {
    case SXML_AST_KIND_PARENTHESIZED_EXPRESSION:
    case SXML_AST_KIND_ENCLOSED_EXPRESSION:
    case SXML_AST_KIND_EXTENSION_EXPRESSION:
        inherit_field(walk, node, SXML_AST_FIELD_EXPRESSION);
        return SXML_STATUS_OK;
    case SXML_AST_KIND_ORDERED_EXPRESSION:
    case SXML_AST_KIND_UNORDERED_EXPRESSION:
        inherit_field(walk, node, SXML_AST_FIELD_CONTENT);
        return SXML_STATUS_OK;
    case SXML_AST_KIND_RETURN_CLAUSE:
    case SXML_AST_KIND_SWITCH_CASE_CLAUSE:
    case SXML_AST_KIND_TYPESWITCH_CASE_CLAUSE:
    case SXML_AST_KIND_TYPESWITCH_DEFAULT_CLAUSE:
        status = require_simple_field(walk, node, SXML_AST_FIELD_OPERAND);
        inherit_field(walk, node, SXML_AST_FIELD_VALUE);
        return status;
    case SXML_AST_KIND_CATCH_CLAUSE:
        inherit_field(walk, node, SXML_AST_FIELD_BODY);
        return SXML_STATUS_OK;
    case SXML_AST_KIND_SEQUENCE_EXPRESSION:
        merge_field(walk, (field_children){.node = node, .field = SXML_AST_FIELD_ITEM}, &merge);
        return finish_merge(walk, node, merge);
    case SXML_AST_KIND_FLWOR_EXPRESSION:
        /* 2.2.2: every clause but return is simple. */
        status = require_simple_field(walk, node, SXML_AST_FIELD_CLAUSE);
        inherit_field(walk, node, SXML_AST_FIELD_RETURN);
        return status;
    case SXML_AST_KIND_IF_EXPRESSION:
        status = require_simple_field(walk, node, SXML_AST_FIELD_CONDITION);
        merge_field(walk, (field_children){.node = node, .field = SXML_AST_FIELD_THEN}, &merge);
        merge_field(walk, (field_children){.node = node, .field = SXML_AST_FIELD_ELSE}, &merge);
        return status == SXML_STATUS_OK ? finish_merge(walk, node, merge) : status;
    case SXML_AST_KIND_SWITCH_EXPRESSION:
        status = require_simple_field(walk, node, SXML_AST_FIELD_COMPARAND);
        merge_field(walk, (field_children){.node = node, .field = SXML_AST_FIELD_CASE}, &merge);
        merge_field(walk, (field_children){.node = node, .field = SXML_AST_FIELD_DEFAULT}, &merge);
        return status == SXML_STATUS_OK ? finish_merge(walk, node, merge) : status;
    case SXML_AST_KIND_TYPESWITCH_EXPRESSION:
        status = require_simple_field(walk, node, SXML_AST_FIELD_OPERAND);
        merge_field(walk, (field_children){.node = node, .field = SXML_AST_FIELD_CASE}, &merge);
        merge_field(walk, (field_children){.node = node, .field = SXML_AST_FIELD_DEFAULT}, &merge);
        return status == SXML_STATUS_OK ? finish_merge(walk, node, merge) : status;
    case SXML_AST_KIND_TRY_CATCH_EXPRESSION:
        status = require_simple_field(walk, node, SXML_AST_FIELD_FINALLY);
        merge_field(walk, (field_children){.node = node, .field = SXML_AST_FIELD_BODY}, &merge);
        merge_field(walk, (field_children){.node = node, .field = SXML_AST_FIELD_CATCH}, &merge);
        return status == SXML_STATUS_OK ? finish_merge(walk, node, merge) : status;
    default:
        *is_handled = false;
        return SXML_STATUS_OK;
    }
}

/* The update expressions themselves and the declarations (5.3, 5.4). */
static sxml_status classify_node(const rules_walk *walk, sxml_ast_node_index node) {
    bool is_handled = false;
    sxml_status status = classify_composite(walk, node, &is_handled);
    if (is_handled) {
        return status;
    }
    switch ((uint32_t)node_of(walk->ast, node)->kind) {
    case SXML_AST_KIND_INSERT_EXPRESSION:
    case SXML_AST_KIND_DELETE_EXPRESSION:
    case SXML_AST_KIND_REPLACE_EXPRESSION:
    case SXML_AST_KIND_RENAME_EXPRESSION:
    case SXML_AST_KIND_UPDATING_FUNCTION_CALL:
        status = require_simple_children(walk, node);
        walk->categories[node.value] = SXML_UPDATE_CATEGORY_UPDATING;
        return status;
    case SXML_AST_KIND_FUNCTION_CALL:
        status = require_simple_children(walk, node);
        return status == SXML_STATUS_OK ? classify_function_call(walk, node) : status;
    case SXML_AST_KIND_COPY_MODIFY_EXPRESSION:
        /* The return clause may be updating, and the expression is then
           updating too, its pending update list the return clause's
           (XQuery Update Facility 4.0 draft, "Copy Modify"; 3.0 had
           XUST0001 there). */
        status = require_simple_field(walk, node, SXML_AST_FIELD_BINDING);
        inherit_field(walk, node, SXML_AST_FIELD_RETURN);
        return status == SXML_STATUS_OK ? require_updating_field(walk, node, SXML_AST_FIELD_MODIFY)
                                        : status;
    case SXML_AST_KIND_TRANSFORM_WITH_EXPRESSION:
        status = require_simple_field(walk, node, SXML_AST_FIELD_VALUE);
        return status == SXML_STATUS_OK ? require_updating_field(walk, node, SXML_AST_FIELD_MODIFY)
                                        : status;
    case SXML_AST_KIND_FUNCTION_DECLARATION:
    case SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION:
        return check_function_body(walk, node);
    case SXML_AST_KIND_MODULE:
    case SXML_AST_KIND_MAIN_MODULE:
    case SXML_AST_KIND_LIBRARY_MODULE:
    case SXML_AST_KIND_PROLOG:
        /* The body's category is read by the caller; declarations are
           checked by their own rules. */
        return SXML_STATUS_OK;
    default:
        return require_simple_children(walk, node);
    }
}

/* The deepest first descendant of node. */
static sxml_ast_node_index first_leaf(const sxml_ast *ast, sxml_ast_node_index node) {
    sxml_ast_node_index leaf = node;
    while (node_of(ast, leaf)->first_child.value != 0) {
        leaf = node_of(ast, leaf)->first_child;
    }
    return leaf;
}

sxml_status sxml_xquery_check_update_rules(const sxml_xquery_update_rules_request *request,
                                           bool *body_is_updating) {
    *body_is_updating = false;
    const sxml_ast *ast = request->context->ast;
    const sxml_allocator allocator = request->context->xpath.allocator;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        allocator, (sxml_allocation_request){.byte_count = ast->node_count, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, ast->node_count);
    rules_walk walk = {.request = request, .ast = ast, .categories = memory};
    /* Post-order: every node after its children. */
    sxml_ast_node_index node = first_leaf(ast, ast->root);
    while (status == SXML_STATUS_OK) {
        status = classify_node(&walk, node);
        if (node.value == ast->root.value) {
            break;
        }
        const sxml_ast_node_index next = node_of(ast, node)->next_sibling;
        node = next.value != 0 ? first_leaf(ast, next) : node_of(ast, node)->parent;
    }
    if (status == SXML_STATUS_OK) {
        const sxml_ast_node_index module = child_of(ast, ast->root, SXML_AST_FIELD_MODULE);
        const sxml_ast_node_index body = child_of(ast, module, SXML_AST_FIELD_BODY);
        *body_is_updating = body.value != 0 && category_of(&walk, body) == SXML_UPDATE_CATEGORY_UPDATING;
    }
    sxml_allocator_release(allocator, memory);
    return status;
}

/* ------------------------------------------------- Updating functions */

static sxml_status intern_updating_name(sxml_name_pool *name_pool, sxml_name_identifier *name) {
    sxml_namespace_uri_identifier xquery_namespace = {0};
    const sxml_status status =
        sxml_name_pool_intern_namespace_uri(name_pool, xquery_namespace_text, &xquery_namespace);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_pool_intern_name(
        name_pool,
        &(sxml_name_request){.namespace_uri = xquery_namespace,
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("updating")},
        name);
}

sxml_status sxml_xquery_mark_updating_function(const sxml_xquery_updating_mark *mark) {
    sxml_function_annotation annotation = {.annotation_name = {0},
                                           .annotation_values = {.items = NULL, .item_count = 0}};
    sxml_status status = intern_updating_name(sxml_item_heap_get_name_pool(mark->unit->item_heap),
                                              &annotation.annotation_name);
    sxml_function_annotation_view annotations = {.function_annotations = NULL,
                                                 .function_annotation_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function_annotations(
            mark->unit,
            (sxml_function_annotation_view){.function_annotations = &annotation,
                                            .function_annotation_count = 1},
            &annotations);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = mark->function_index,
        .function = *sxml_ir_unit_get_function(mark->unit, mark->function_index)};
    replacement.function.function_annotations = annotations;
    return sxml_ir_unit_replace_function(mark->unit, &replacement);
}

bool sxml_xquery_annotations_are_updating(const sxml_item_heap *item_heap,
                                          sxml_function_annotation_view annotations) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    for (size_t annotation_index = 0; annotation_index < annotations.function_annotation_count;
         annotation_index += 1) {
        sxml_name_parts parts;
        if (sxml_name_pool_get_name_parts(
                name_pool, annotations.function_annotations[annotation_index].annotation_name,
                &parts) != SXML_STATUS_OK ||
            !sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("updating"))) {
            continue;
        }
        sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
        if (sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri, &namespace_uri) ==
                SXML_STATUS_OK &&
            sxml_byte_view_equals(namespace_uri, xquery_namespace_text)) {
            return true;
        }
    }
    return false;
}

/* ================================================== Update primitives */

static const sxml_byte_view primitive_local_names[] = {
    [SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO] = SXML_BYTE_VIEW_INITIALIZER("insertInto"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_FIRST] =
        SXML_BYTE_VIEW_INITIALIZER("insertIntoAsFirst"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_LAST] =
        SXML_BYTE_VIEW_INITIALIZER("insertIntoAsLast"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_BEFORE] = SXML_BYTE_VIEW_INITIALIZER("insertBefore"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_AFTER] = SXML_BYTE_VIEW_INITIALIZER("insertAfter"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_ATTRIBUTES] =
        SXML_BYTE_VIEW_INITIALIZER("insertAttributes"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_DELETE] = SXML_BYTE_VIEW_INITIALIZER("delete"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_NODE] = SXML_BYTE_VIEW_INITIALIZER("replaceNode"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_VALUE] = SXML_BYTE_VIEW_INITIALIZER("replaceValue"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_ELEMENT_CONTENT] =
        SXML_BYTE_VIEW_INITIALIZER("replaceElementContent"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_RENAME] = SXML_BYTE_VIEW_INITIALIZER("rename"),
    [SXML_XQUERY_UPDATE_PRIMITIVE_PUT] = SXML_BYTE_VIEW_INITIALIZER("put"),
};

/* The captured values of a primitive: its target, its parameter, its
   kind as an xs:integer. */
enum {
    SXML_PRIMITIVE_TARGET_VALUE = 0,
    SXML_PRIMITIVE_PARAMETER_VALUE = 1,
    SXML_PRIMITIVE_KIND_VALUE = 2,
    SXML_PRIMITIVE_VALUE_COUNT = 3,
};

static const sxml_static_error not_callable_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XUDY0038"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an update primitive is not a function to call (XQuery Update Facility 3.0, 5.3)")};

static sxml_status call_primitive(void *state, const sxml_function_call *call,
                                  sxml_sequence *result) {
    (void)state;
    (void)result;
    return sxml_function_library_report(call, &not_callable_error);
}

const sxml_function_body_operations sxml_xquery_update_primitive_operations = {
    .call = call_primitive, .retain = NULL, .release = NULL};

bool sxml_xquery_is_update_primitive(sxml_item item) {
    if (sxml_item_get_kind(item) != SXML_ITEM_KIND_FUNCTION) {
        return false;
    }
    sxml_function_item_description description;
    return sxml_function_item_describe(item, &description) == SXML_STATUS_OK &&
           description.body.operations == &sxml_xquery_update_primitive_operations;
}

sxml_status sxml_xquery_read_update_primitive(sxml_item item,
                                              sxml_xquery_update_primitive *primitive) {
    sxml_function_item_description description;
    if (sxml_item_get_kind(item) != SXML_ITEM_KIND_FUNCTION ||
        sxml_function_item_describe(item, &description) != SXML_STATUS_OK ||
        description.body.operations != &sxml_xquery_update_primitive_operations ||
        description.captured_values.sequence_count != SXML_PRIMITIVE_VALUE_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_sequence *values = description.captured_values.sequences;
    sxml_item target = {.representation = 0};
    sxml_item kind = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        values[SXML_PRIMITIVE_TARGET_VALUE], (sxml_item_request){.item_index = 0, .error_record = NULL},
        &target);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            values[SXML_PRIMITIVE_KIND_VALUE],
            (sxml_item_request){.item_index = 0, .error_record = NULL}, &kind);
    }
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(kind, &datum);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(target, &primitive->target);
    }
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const int64_t kind_value = datum.datum_details.integer;
    primitive->primitive_kind = (sxml_xquery_update_primitive_kind)(kind_value & 0xFF);
    primitive->preserves_namespaces = (kind_value & (SXML_XQUERY_UPDATE_NO_PRESERVE_NAMESPACES << 8)) == 0;
    primitive->inherits_namespaces = (kind_value & (SXML_XQUERY_UPDATE_NO_INHERIT_NAMESPACES << 8)) == 0;
    primitive->preserves_types = (kind_value & (SXML_XQUERY_UPDATE_PRESERVES_TYPES << 8)) != 0;
    primitive->parameter = values[SXML_PRIMITIVE_PARAMETER_VALUE];
    return SXML_STATUS_OK;
}

/* One primitive to make: its kind, target (a node item) and parameter. */
typedef struct primitive_request {
    sxml_xquery_update_primitive_kind primitive_kind;
    sxml_item target;
    sxml_sequence parameter;
    /* SXML_XQUERY_UPDATE_NO_PRESERVE_NAMESPACES and _NO_INHERIT_. */
    uint32_t copy_flags;
} primitive_request;

/* Makes the primitive and appends it to builder; request->target is
   owned and released, request->parameter borrowed. */
typedef struct primitive_output {
    const sxml_function_call *call;
    sxml_sequence_builder *builder;
} primitive_output;

static sxml_status append_primitive(const primitive_output *output,
                                    const primitive_request *request) {
    sxml_item_heap *item_heap = output->call->item_heap;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    sxml_namespace_uri_identifier update_namespace = {0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        name_pool, SXML_BYTE_VIEW_FROM_LITERAL(SXML_XQUERY_UPDATE_NAMESPACE), &update_namespace);
    sxml_name_identifier function_name = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = update_namespace,
                                 .local_name = primitive_local_names[request->primitive_kind]},
            &function_name);
    }
    /* function() as empty-sequence(): a primitive takes no argument. */
    sxml_type_identifier signature = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_register_item_type(
            sxml_item_heap_get_type_registry(item_heap),
            &(sxml_type_description){
                .variety = SXML_TYPE_VARIETY_FUNCTION,
                .variety_details = {.function_type = {.is_any_function = false,
                                                      .parameter_types = {NULL, 0},
                                                      .result_type = {
                                                          .item_type = sxml_builtin_type_identifier(
                                                              SXML_TYPE_ITEM),
                                                          .occurrence = SXML_OCCURRENCE_ZERO}}}},
            &signature);
    }
    sxml_item primitive = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        const sxml_sequence values[SXML_PRIMITIVE_VALUE_COUNT] = {
            [SXML_PRIMITIVE_TARGET_VALUE] = sxml_sequence_from_item(request->target),
            [SXML_PRIMITIVE_PARAMETER_VALUE] = request->parameter,
            [SXML_PRIMITIVE_KIND_VALUE] = sxml_sequence_from_item(sxml_atomic_item_from_integer(
                (int64_t)request->primitive_kind | ((int64_t)request->copy_flags << 8)))};
        status = sxml_function_item_make(
            item_heap,
            &(sxml_function_item_request){
                .function_name = function_name,
                .signature = signature,
                .function_annotations = {.function_annotations = NULL,
                                         .function_annotation_count = 0},
                .captured_values = {.sequences = values, .sequence_count = SXML_PRIMITIVE_VALUE_COUNT},
                .body = {.state = NULL, .operations = &sxml_xquery_update_primitive_operations}},
            &primitive);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_item(output->builder, primitive);
        sxml_item_release(primitive);
    }
    sxml_item_release(request->target);
    return status;
}

/* ------------------------------------------------------ Reading values */

/* Errors with static text (XQUF 3.0, Appendix C). */
#define UPDATE_ERROR(name, status_value, code, text)                                      \
    static const sxml_static_error name = {.error_status = (status_value),                \
                                           .code_local_name = SXML_BYTE_VIEW_INITIALIZER(code), \
                                           .description = SXML_BYTE_VIEW_INITIALIZER(text)}

UPDATE_ERROR(late_attribute_error, SXML_STATUS_TYPE_ERROR, "XUTY0004",
             "the insertion sequence has an attribute after a node that is not one (5.4.1)");
UPDATE_ERROR(into_target_error, SXML_STATUS_TYPE_ERROR, "XUTY0005",
             "the target of insert into is not one element or document node (5.4.1)");
UPDATE_ERROR(sibling_target_error, SXML_STATUS_TYPE_ERROR, "XUTY0006",
             "the target of insert before or after is not one element, text, comment or "
             "processing instruction node (5.4.1)");
UPDATE_ERROR(delete_target_error, SXML_STATUS_TYPE_ERROR, "XUTY0007",
             "the target of delete is not a sequence of nodes (5.4.2)");
UPDATE_ERROR(replace_target_error, SXML_STATUS_TYPE_ERROR, "XUTY0008",
             "the target of replace is not one element, attribute, text, comment or "
             "processing instruction node (5.4.3)");
UPDATE_ERROR(replace_parent_error, SXML_STATUS_DYNAMIC_ERROR, "XUDY0009",
             "the target of replace node has no parent (5.4.3.1)");
UPDATE_ERROR(replacement_attribute_error, SXML_STATUS_TYPE_ERROR, "XUTY0010",
             "the replacement of a node that is not an attribute contains an attribute "
             "(5.4.3.1)");
UPDATE_ERROR(replacement_not_attribute_error, SXML_STATUS_TYPE_ERROR, "XUTY0011",
             "the replacement of an attribute contains a node that is not an attribute "
             "(5.4.3.1)");
UPDATE_ERROR(rename_target_error, SXML_STATUS_TYPE_ERROR, "XUTY0012",
             "the target of rename is not one element, attribute or processing instruction "
             "node (5.4.4)");
UPDATE_ERROR(document_attribute_error, SXML_STATUS_TYPE_ERROR, "XUTY0022",
             "attributes are inserted into a document node (5.4.1)");
UPDATE_ERROR(namespace_conflict_error, SXML_STATUS_DYNAMIC_ERROR, "XUDY0023",
             "a new name's namespace binding conflicts with a binding of the target's "
             "namespaces (5.4)");
UPDATE_ERROR(processing_instruction_prefix_error, SXML_STATUS_DYNAMIC_ERROR, "XUDY0025",
             "a processing instruction is renamed to a name with a prefix (5.4.4)");
UPDATE_ERROR(empty_target_error, SXML_STATUS_DYNAMIC_ERROR, "XUDY0027",
             "the target of an update expression is the empty sequence (5.4)");
UPDATE_ERROR(parentless_target_error, SXML_STATUS_DYNAMIC_ERROR, "XUDY0029",
             "the target of insert before or after has no parent (5.4.1)");
UPDATE_ERROR(document_sibling_attribute_error, SXML_STATUS_DYNAMIC_ERROR, "XUDY0030",
             "attributes are inserted before or after a child of a document node (5.4.1)");
UPDATE_ERROR(content_function_error, SXML_STATUS_TYPE_ERROR, "XQTY0105",
             "the source of an update contains a function item (XQuery 4.0, 4.12.1.3)");
UPDATE_ERROR(name_type_error, SXML_STATUS_TYPE_ERROR, "XPTY0004",
             "the new name is not one xs:QName, xs:string or xs:untypedAtomic (5.4.4)");
UPDATE_ERROR(invalid_name_error, SXML_STATUS_DYNAMIC_ERROR, "XQDY0074",
             "the new name is not a valid QName with a bound prefix (5.4.4)");
UPDATE_ERROR(invalid_target_name_error, SXML_STATUS_DYNAMIC_ERROR, "XQDY0041",
             "the new name of a processing instruction is not an NCName (5.4.4)");
UPDATE_ERROR(xml_target_name_error, SXML_STATUS_DYNAMIC_ERROR, "XQDY0064",
             "the new name of a processing instruction is \"xml\" in some case (5.4.4)");
UPDATE_ERROR(comment_value_error, SXML_STATUS_DYNAMIC_ERROR, "XQDY0072",
             "the new value of a comment contains \"--\" or ends with \"-\" (5.4.3.2)");
UPDATE_ERROR(instruction_value_error, SXML_STATUS_DYNAMIC_ERROR, "XQDY0026",
             "the new value of a processing instruction contains \"?>\" (5.4.3.2)");

/* The one node of a target value: XUDY0027 when it is empty,
   cardinality_error when it is more than one item or not a node. */
typedef struct target_request {
    sxml_sequence value;
    const sxml_static_error *cardinality_error;
} target_request;

typedef struct target_node {
    sxml_item item;
    sxml_node_reference node;
    sxml_node_description description;
} target_node;

static sxml_status read_target(const sxml_function_call *call, const target_request *request,
                               target_node *target) {
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(request->value, call->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count == 0) {
        return sxml_function_library_report(call, &empty_target_error);
    }
    if (item_count == 1) {
        status = sxml_sequence_get_item(
            request->value, (sxml_item_request){.item_index = 0, .error_record = NULL},
            &target->item);
    }
    if (status == SXML_STATUS_OK && item_count == 1 &&
        sxml_item_get_node(target->item, &target->node) == SXML_STATUS_OK &&
        target->node.namespace_number == 0) {
        return sxml_node_describe(target->node, &target->description);
    }
    return status == SXML_STATUS_OK ? sxml_function_library_report(call, request->cardinality_error)
                                    : status;
}

static bool kind_is_one_of(sxml_node_kind kind, uint32_t kind_bits) {
    return ((1U << (uint32_t)kind) & kind_bits) != 0;
}

#define KIND_BIT(kind) (1U << (uint32_t)(kind))

/* ------------------------------------------------- Insertion sequences */

/* The nodes and texts a source makes, split into attributes and the rest
   (5.4.1 steps 1 and 2; 5.4.3.1): two builders, the text being joined,
   and whether an atomic item came last (adjacent ones join with a
   space). */
typedef struct insertion_sequence {
    const sxml_function_call *call;
    sxml_sequence_builder *attributes;
    sxml_sequence_builder *others;
    sxml_library_text text;
    bool has_text;
    bool follows_atomic;
    bool has_other;
    bool has_late_attribute;
} insertion_sequence;

static sxml_status flush_text(insertion_sequence *insertion) {
    if (!insertion->has_text) {
        return SXML_STATUS_OK;
    }
    insertion->has_text = false;
    const sxml_byte_view text = sxml_function_library_text_view(&insertion->text);
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_sequence string = sxml_sequence_empty();
    sxml_status status =
        sxml_function_library_make_string(insertion->call->item_heap, text, &string);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_sequence(insertion->others, string, NULL);
        sxml_sequence_release(string);
    }
    sxml_function_library_text_release(&insertion->text);
    insertion->text.item_heap = insertion->call->item_heap;
    insertion->has_other = true;
    return status;
}

static sxml_status add_node(insertion_sequence *insertion, sxml_item item,
                            sxml_node_kind node_kind) {
    if (node_kind == SXML_NODE_KIND_ATTRIBUTE) {
        insertion->has_late_attribute = insertion->has_late_attribute || insertion->has_other;
        return sxml_sequence_builder_append_item(insertion->attributes, item);
    }
    insertion->has_other = true;
    return sxml_sequence_builder_append_item(insertion->others, item);
}

/* A node of the source: a document node by its children (5.4.1 step 1). */
static sxml_status add_source_node(insertion_sequence *insertion, sxml_item item) {
    sxml_node_reference node;
    sxml_status status = sxml_item_get_node(item, &node);
    if (status != SXML_STATUS_OK) {
        return sxml_function_library_report(insertion->call, &content_function_error);
    }
    sxml_node_description description;
    status = flush_text(insertion);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(node, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.node_kind != SXML_NODE_KIND_DOCUMENT) {
        return add_node(insertion, item, description.node_kind);
    }
    bool has_child = description.has_children;
    sxml_node_index child = description.first_child;
    while (status == SXML_STATUS_OK && has_child) {
        const sxml_node_reference child_node = {
            .tree = node.tree, .node_index = child, .namespace_number = 0};
        sxml_node_description child_description;
        status = sxml_node_describe(child_node, &child_description);
        if (status == SXML_STATUS_OK) {
            const sxml_item child_item = sxml_node_item_make(child_node);
            status = add_node(insertion, child_item, child_description.node_kind);
            sxml_item_release(child_item);
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    return status;
}

static sxml_status add_source_item(insertion_sequence *insertion, sxml_item item) {
    switch (sxml_item_get_kind(item)) {
    case SXML_ITEM_KIND_ATOMIC: {
        /* Each run of adjacent atomic items is one text, the items
           separated by one space (XQuery 4.0, 4.12.1.3, rule 1e). */
        sxml_status status = SXML_STATUS_OK;
        if (insertion->follows_atomic) {
            status = sxml_function_library_text_append(&insertion->text,
                                                       SXML_BYTE_VIEW_FROM_LITERAL(" "));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append_atomic(&insertion->text, item);
        }
        insertion->has_text = true;
        insertion->follows_atomic = true;
        return status;
    }
    case SXML_ITEM_KIND_NODE:
        insertion->follows_atomic = false;
        return add_source_node(insertion, item);
    case SXML_ITEM_KIND_JSON_NODE:
    case SXML_ITEM_KIND_FUNCTION:
    case SXML_ITEM_KIND_MAP:
    case SXML_ITEM_KIND_ARRAY:
        break;
    }
    return sxml_function_library_report(insertion->call, &content_function_error);
}

/* The insertion sequence of source, owned in *parts: the attributes and
   the other nodes and texts. */
typedef struct insertion_parts {
    sxml_sequence attributes;
    sxml_sequence others;
    bool has_late_attribute;
} insertion_parts;

static void release_insertion_parts(insertion_parts *parts) {
    sxml_sequence_release(parts->attributes);
    sxml_sequence_release(parts->others);
    parts->attributes = sxml_sequence_empty();
    parts->others = sxml_sequence_empty();
}

static sxml_status make_insertion_sequence(const sxml_function_call *call, sxml_sequence source,
                                           insertion_parts *parts) {
    *parts = (insertion_parts){.attributes = sxml_sequence_empty(),
                               .others = sxml_sequence_empty(),
                               .has_late_attribute = false};
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    const sxml_sequence_builder_configuration configuration = {.item_heap = call->item_heap};
    insertion_sequence insertion = {.call = call, .text = {.item_heap = call->item_heap}};
    sxml_status status =
        sxml_sequence_builder_create(allocator, &configuration, &insertion.attributes);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(allocator, &configuration, &insertion.others);
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(source);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        status = sxml_function_library_charge_steps(call, 1);
        if (status == SXML_STATUS_OK) {
            status = add_source_item(&insertion, step.item);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = flush_text(&insertion);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(insertion.attributes, &parts->attributes);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(insertion.others, &parts->others);
    }
    parts->has_late_attribute = insertion.has_late_attribute;
    sxml_function_library_text_release(&insertion.text);
    sxml_sequence_builder_destroy(insertion.attributes);
    sxml_sequence_builder_destroy(insertion.others);
    if (status != SXML_STATUS_OK) {
        release_insertion_parts(parts);
    }
    return status;
}

static bool sequence_is_empty(sxml_sequence sequence) {
    sxml_emptiness emptiness = {.is_empty = true};
    return sxml_sequence_test_emptiness(sequence, NULL, &emptiness) != SXML_STATUS_OK ||
           emptiness.is_empty;
}

/* ------------------------------------------------- Namespace conflicts */

/* The binding a name needs: its prefix and namespace URI; none for a
   name in no namespace without prefix. */
typedef struct name_binding {
    bool has_binding;
    sxml_namespace_binding binding;
} name_binding;

static sxml_status binding_of_name(const sxml_name_pool *name_pool, sxml_qname_datum name,
                                   name_binding *binding) {
    sxml_name_parts parts;
    const sxml_status status = sxml_name_pool_get_name_parts(name_pool, name.expanded_name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *binding = (name_binding){
        .has_binding = name.namespace_prefix.index != 0 ||
                       parts.namespace_uri.index != SXML_NAMESPACE_NONE,
        .binding = {.namespace_prefix = name.namespace_prefix,
                    .namespace_uri = parts.namespace_uri}};
    return SXML_STATUS_OK;
}

/* Whether binding conflicts with element's in-scope namespaces: its
   prefix is bound there to another URI (XUDY0023, 5.4). An unprefixed
   name conflicts only with a default namespace it does not have when it
   is in a namespace itself. */
typedef struct conflict_test {
    sxml_node_reference element;
    name_binding binding;
} conflict_test;

static sxml_status test_conflict(const conflict_test *test, bool *is_conflict) {
    *is_conflict = false;
    if (!test->binding.has_binding) {
        return SXML_STATUS_OK;
    }
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    const sxml_status status = sxml_node_get_in_scope_namespaces(test->element, &bindings);
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding existing = bindings.namespace_bindings[binding_index];
        if (existing.namespace_prefix.index == test->binding.binding.namespace_prefix.index &&
            existing.namespace_uri.index != test->binding.binding.namespace_uri.index &&
            (test->binding.binding.namespace_prefix.index != 0 ||
             test->binding.binding.namespace_uri.index != SXML_NAMESPACE_NONE)) {
            *is_conflict = true;
        }
    }
    return status;
}

/* XUDY0023 when an attribute of attributes needs a binding that conflicts
   with element's namespaces (5.4.1 step 5, 5.4.3.1). */
typedef struct attribute_conflict_check {
    const sxml_function_call *call;
    sxml_node_reference element;
    sxml_sequence attributes;
} attribute_conflict_check;

static sxml_status check_attribute_conflicts(const attribute_conflict_check *check) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(check->call->item_heap);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(check->attributes);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_node_reference attribute;
        sxml_node_description description;
        status = sxml_item_get_node(step.item, &attribute);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(attribute, &description);
        }
        name_binding binding = {.has_binding = false};
        if (status == SXML_STATUS_OK) {
            status = binding_of_name(
                name_pool,
                (sxml_qname_datum){.expanded_name = description.node_name,
                                   .namespace_prefix = description.namespace_prefix},
                &binding);
        }
        bool is_conflict = false;
        if (status == SXML_STATUS_OK) {
            status = test_conflict(
                &(conflict_test){.element = check->element, .binding = binding}, &is_conflict);
        }
        if (status == SXML_STATUS_OK && is_conflict) {
            return sxml_function_library_report(check->call, &namespace_conflict_error);
        }
    }
    return status;
}

/* ------------------------------------------------------------- insert */

/* The primitive kind of insert's $position (src/xquery_update_internal.h). */
static const sxml_xquery_update_primitive_kind insert_kinds[] = {
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO,
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_FIRST,
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_LAST,
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_BEFORE,
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_AFTER,
};

/* An xs:integer argument the lowering made: its low three bits a variant
   in the range of its table, the copy flags above. */
typedef struct update_variant {
    size_t variant;
    uint32_t copy_flags;
} update_variant;

static sxml_status read_variant(const sxml_function_call *call, size_t variant_count,
                                update_variant *variant) {
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    sxml_status status = sxml_sequence_get_item(
        call->arguments.sequences[0], (sxml_item_request){.item_index = 0, .error_record = NULL},
        &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    if (status != SXML_STATUS_OK || datum.datum_kind != SXML_DATUM_KIND_INTEGER ||
        datum.datum_details.integer < 0 ||
        (uint64_t)(datum.datum_details.integer & 7) >= variant_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *variant = (update_variant){.variant = (size_t)(datum.datum_details.integer & 7),
                                .copy_flags = (uint32_t)(datum.datum_details.integer & ~7)};
    return SXML_STATUS_OK;
}

/* The checks of 5.4.1 steps 3 to 5 on the target and the attributes. */
typedef struct insert_plan {
    sxml_xquery_update_primitive_kind primitive_kind;
    uint32_t copy_flags;
    target_node target;
    /* Where the attributes go: the target, or its parent for before and
       after. */
    sxml_node_reference attribute_target;
    insertion_parts parts;
} insert_plan;

static sxml_status check_insert_target(const sxml_function_call *call, insert_plan *plan) {
    const bool is_into = plan->primitive_kind != SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_BEFORE &&
                         plan->primitive_kind != SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_AFTER;
    sxml_status status = read_target(
        call,
        &(target_request){.value = call->arguments.sequences[2],
                          .cardinality_error = is_into ? &into_target_error : &sibling_target_error},
        &plan->target);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_node_kind kind = plan->target.description.node_kind;
    const bool has_attributes = !sequence_is_empty(plan->parts.attributes);
    if (is_into) {
        if (!kind_is_one_of(kind, KIND_BIT(SXML_NODE_KIND_ELEMENT) |
                                      KIND_BIT(SXML_NODE_KIND_DOCUMENT))) {
            return sxml_function_library_report(call, &into_target_error);
        }
        if (has_attributes && kind == SXML_NODE_KIND_DOCUMENT) {
            return sxml_function_library_report(call, &document_attribute_error);
        }
        plan->attribute_target = plan->target.node;
    } else {
        if (!kind_is_one_of(kind, KIND_BIT(SXML_NODE_KIND_ELEMENT) | KIND_BIT(SXML_NODE_KIND_TEXT) |
                                      KIND_BIT(SXML_NODE_KIND_COMMENT) |
                                      KIND_BIT(SXML_NODE_KIND_PROCESSING_INSTRUCTION))) {
            return sxml_function_library_report(call, &sibling_target_error);
        }
        if (!plan->target.description.has_parent) {
            return sxml_function_library_report(call, &parentless_target_error);
        }
        plan->attribute_target = (sxml_node_reference){.tree = plan->target.node.tree,
                                                       .node_index =
                                                           plan->target.description.parent,
                                                       .namespace_number = 0};
        sxml_node_description parent;
        status = sxml_node_describe(plan->attribute_target, &parent);
        if (status == SXML_STATUS_OK && has_attributes &&
            parent.node_kind == SXML_NODE_KIND_DOCUMENT) {
            return sxml_function_library_report(call, &document_sibling_attribute_error);
        }
    }
    if (status == SXML_STATUS_OK && has_attributes) {
        status = check_attribute_conflicts(&(attribute_conflict_check){
            .call = call, .element = plan->attribute_target, .attributes = plan->parts.attributes});
    }
    return status;
}

/* Makes the insert's primitives into output. */
static sxml_status make_insert_primitives(const primitive_output *output,
                                          const insert_plan *plan) {
    sxml_status status = SXML_STATUS_OK;
    if (!sequence_is_empty(plan->parts.attributes)) {
        status = append_primitive(
            output, &(primitive_request){
                        .primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_ATTRIBUTES,
                        .target = sxml_node_item_make(plan->attribute_target),
                        .parameter = plan->parts.attributes,
                        .copy_flags = plan->copy_flags});
    }
    if (status == SXML_STATUS_OK && !sequence_is_empty(plan->parts.others)) {
        sxml_item_retain(plan->target.item);
        status = append_primitive(output, &(primitive_request){.primitive_kind = plan->primitive_kind,
                                                               .target = plan->target.item,
                                                               .parameter = plan->parts.others,
                                                               .copy_flags = plan->copy_flags});
    }
    return status;
}

/* Runs make with a builder for the primitives and finishes *result. */
typedef sxml_status (*primitive_maker)(const primitive_output *output);

static sxml_status collect_primitives(const sxml_function_call *call, primitive_maker make,
                                      sxml_sequence *result) {
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    if (status == SXML_STATUS_OK) {
        status = make(&(primitive_output){.call = call, .builder = builder});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

static sxml_status make_insert(const primitive_output *output) {
    const sxml_function_call *call = output->call;
    update_variant variant = {.variant = 0, .copy_flags = 0};
    sxml_status status =
        read_variant(call, sizeof insert_kinds / sizeof insert_kinds[0], &variant);
    insert_plan plan = {.primitive_kind = insert_kinds[variant.variant],
                        .copy_flags = variant.copy_flags,
                        .parts = {.attributes = sxml_sequence_empty(),
                                  .others = sxml_sequence_empty()}};
    if (status == SXML_STATUS_OK) {
        status = make_insertion_sequence(call, call->arguments.sequences[1], &plan.parts);
    }
    if (status == SXML_STATUS_OK && plan.parts.has_late_attribute) {
        status = sxml_function_library_report(call, &late_attribute_error);
    }
    if (status == SXML_STATUS_OK) {
        status = check_insert_target(call, &plan);
    }
    if (status == SXML_STATUS_OK) {
        status = make_insert_primitives(output, &plan);
    }
    release_insertion_parts(&plan.parts);
    return status;
}

static sxml_status call_insert(const sxml_function_call *call, sxml_sequence *result) {
    return collect_primitives(call, make_insert, result);
}

/* ------------------------------------------------------------- delete */

static sxml_status make_delete(const primitive_output *output) {
    const sxml_function_call *call = output->call;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(call->arguments.sequences[0]);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_node_reference node;
        if (sxml_item_get_node(step.item, &node) != SXML_STATUS_OK) {
            return sxml_function_library_report(call, &delete_target_error);
        }
        /* A node without a parent is not changed (3.1.2, upd:delete). */
        sxml_item_retain(step.item);
        status = append_primitive(output,
                                  &(primitive_request){.primitive_kind =
                                                           SXML_XQUERY_UPDATE_PRIMITIVE_DELETE,
                                                       .target = step.item,
                                                       .parameter = sxml_sequence_empty()});
    }
    return status;
}

static sxml_status call_delete(const sxml_function_call *call, sxml_sequence *result) {
    return collect_primitives(call, make_delete, result);
}

/* ------------------------------------------------------------ replace */

#define REPLACE_TARGET_KINDS                                                                  \
    (KIND_BIT(SXML_NODE_KIND_ELEMENT) | KIND_BIT(SXML_NODE_KIND_ATTRIBUTE) |                   \
     KIND_BIT(SXML_NODE_KIND_TEXT) | KIND_BIT(SXML_NODE_KIND_COMMENT) |                        \
     KIND_BIT(SXML_NODE_KIND_PROCESSING_INSTRUCTION))

/* replace node (5.4.3.1). */
typedef struct replace_plan {
    const target_node *target;
    uint32_t copy_flags;
} replace_plan;

static sxml_status make_replace_node(const primitive_output *output, replace_plan plan) {
    const target_node *target = plan.target;
    const sxml_function_call *call = output->call;
    if (!target->description.has_parent) {
        return sxml_function_library_report(call, &replace_parent_error);
    }
    insertion_parts parts;
    sxml_status status = make_insertion_sequence(call, call->arguments.sequences[2], &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence replacement;
    if (target->description.node_kind == SXML_NODE_KIND_ATTRIBUTE) {
        if (!sequence_is_empty(parts.others)) {
            status = sxml_function_library_report(call, &replacement_not_attribute_error);
        } else {
            status = check_attribute_conflicts(&(attribute_conflict_check){
                .call = call,
                .element = {.tree = target->node.tree,
                            .node_index = target->description.parent,
                            .namespace_number = 0},
                .attributes = parts.attributes});
        }
        replacement = parts.attributes;
    } else {
        if (!sequence_is_empty(parts.attributes)) {
            status = sxml_function_library_report(call, &replacement_attribute_error);
        }
        replacement = parts.others;
    }
    if (status == SXML_STATUS_OK) {
        sxml_item_retain(target->item);
        status = append_primitive(output,
                                  &(primitive_request){.primitive_kind =
                                                           SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_NODE,
                                                       .target = target->item,
                                                       .parameter = replacement,
                                                       .copy_flags = plan.copy_flags});
    }
    release_insertion_parts(&parts);
    return status;
}

/* The string value of the text node the lowering made of the new value
   (5.4.3.2: "as in a text node constructor"); "" for none. */
static sxml_status read_new_value(const sxml_function_call *call, sxml_sequence *string) {
    sxml_item text = {.representation = 0};
    if (sequence_is_empty(call->arguments.sequences[2])) {
        return sxml_function_library_make_string(call->item_heap,
                                                 (sxml_byte_view){.bytes = NULL, .byte_count = 0},
                                                 string);
    }
    sxml_status status = sxml_sequence_get_item(
        call->arguments.sequences[2], (sxml_item_request){.item_index = 0, .error_record = NULL},
        &text);
    sxml_node_reference node;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(text, &node);
    }
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_node_get_content(node, &content);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(call->item_heap, content, string);
    }
    return status;
}

static bool contains_text(sxml_byte_view text, sxml_byte_view part) {
    for (size_t byte_index = 0; byte_index + part.byte_count <= text.byte_count;
         byte_index += 1) {
        if (memcmp(text.bytes + byte_index, part.bytes, part.byte_count) == 0) {
            return true;
        }
    }
    return false;
}

/* XQDY0072 and XQDY0026 for a comment and a processing instruction
   (5.4.3.2, as for their constructors). */
static sxml_status check_new_value(const sxml_function_call *call, sxml_node_kind kind,
                                   sxml_sequence string) {
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    sxml_status status = sxml_sequence_get_item(
        string, (sxml_item_request){.item_index = 0, .error_record = NULL}, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view text = datum.datum_details.string.utf8;
    if (kind == SXML_NODE_KIND_COMMENT &&
        (contains_text(text, SXML_BYTE_VIEW_FROM_LITERAL("--")) ||
         (text.byte_count > 0 && text.bytes[text.byte_count - 1] == '-'))) {
        return sxml_function_library_report(call, &comment_value_error);
    }
    if (kind == SXML_NODE_KIND_PROCESSING_INSTRUCTION &&
        contains_text(text, SXML_BYTE_VIEW_FROM_LITERAL("?>"))) {
        return sxml_function_library_report(call, &instruction_value_error);
    }
    return SXML_STATUS_OK;
}

/* replace value of node (5.4.3.2). */
static sxml_status make_replace_value(const primitive_output *output, const target_node *target) {
    const sxml_function_call *call = output->call;
    sxml_sequence string = sxml_sequence_empty();
    sxml_status status = read_new_value(call, &string);
    const sxml_node_kind kind = target->description.node_kind;
    if (status == SXML_STATUS_OK) {
        status = check_new_value(call, kind, string);
    }
    if (status == SXML_STATUS_OK) {
        sxml_item_retain(target->item);
        status = append_primitive(
            output,
            &(primitive_request){.primitive_kind =
                                     kind == SXML_NODE_KIND_ELEMENT
                                         ? SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_ELEMENT_CONTENT
                                         : SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_VALUE,
                                 .target = target->item,
                                 .parameter = string});
    }
    sxml_sequence_release(string);
    return status;
}

static sxml_status make_replace(const primitive_output *output) {
    const sxml_function_call *call = output->call;
    update_variant variant = {.variant = 0, .copy_flags = 0};
    sxml_status status = read_variant(call, 2, &variant);
    target_node target = {.item = {.representation = 0}};
    if (status == SXML_STATUS_OK) {
        status = read_target(call,
                             &(target_request){.value = call->arguments.sequences[1],
                                               .cardinality_error = &replace_target_error},
                             &target);
    }
    if (status == SXML_STATUS_OK &&
        !kind_is_one_of(target.description.node_kind, REPLACE_TARGET_KINDS)) {
        status = sxml_function_library_report(call, &replace_target_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return variant.variant == 0
               ? make_replace_node(output,
                                   (replace_plan){.target = &target, .copy_flags = variant.copy_flags})
               : make_replace_value(output, &target);
}

static sxml_status call_replace(const sxml_function_call *call, sxml_sequence *result) {
    return collect_primitives(call, make_replace, result);
}

/* ------------------------------------------------------------- rename */

/* A lexical QName or EQName as text: its name, resolved with the
   statically known namespaces of the code running (5.4.4: as the name of
   a computed element constructor; for an attribute an unprefixed name is
   in no namespace). */
typedef struct name_text {
    sxml_byte_view text;
    bool is_attribute_name;
} name_text;

static sxml_status resolve_name_text(const sxml_function_call *call, const name_text *request,
                                     sxml_qname_datum *name) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    const sxml_byte_view text = sxml_xml_text_trim_whitespace(request->text);
    const bool has_colon = text.byte_count != 0 && memchr(text.bytes, ':', text.byte_count) != NULL;
    const bool is_eqname = text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{';
    if (request->is_attribute_name && !has_colon && !is_eqname) {
        if (!sxml_xml_text_is_ncname(text)) {
            return sxml_function_library_report(call, &invalid_name_error);
        }
        name->namespace_prefix = (sxml_namespace_prefix_identifier){.index = 0};
        return sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri =
                                     sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE),
                                 .local_name = text},
            &name->expanded_name);
    }
    /* Casting to xs:QName resolves the prefix, and an unprefixed name with
       the default element namespace (F&O 4.0, 19.1.9). */
    sxml_sequence qname = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value_from_text(
        call->item_heap,
        &(sxml_text_cast_request){
            .text = text,
            .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .namespace_bindings = sxml_dynamic_context_get_static_namespaces(call->dynamic_context),
            .error_record = NULL},
        &qname);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        return sxml_function_library_report(call, &invalid_name_error);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            qname, (sxml_item_request){.item_index = 0, .error_record = NULL}, &item);
    }
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    if (status == SXML_STATUS_OK) {
        *name = datum.datum_details.qname;
    }
    sxml_sequence_release(qname);
    return status;
}

/* The new name of a rename (5.4.4), from its one item. */
typedef struct new_name_request {
    sxml_item item;
    sxml_node_kind target_kind;
} new_name_request;

static sxml_status read_new_name(const sxml_function_call *call, const new_name_request *request,
                                 sxml_qname_datum *name) {
    sxml_atomic_datum datum;
    if (sxml_item_get_kind(request->item) == SXML_ITEM_KIND_NODE) {
        /* Atomized: an untyped node's value is xs:untypedAtomic. */
        sxml_node_reference node;
        sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
        sxml_item string = {.representation = 0};
        sxml_status status = sxml_item_get_node(request->item, &node);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_make_string_value(call->item_heap, node, &string);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(string, &datum);
            content = datum.datum_details.string.utf8;
        }
        if (status == SXML_STATUS_OK) {
            status = resolve_name_text(
                call,
                &(name_text){.text = content,
                             .is_attribute_name = request->target_kind == SXML_NODE_KIND_ATTRIBUTE},
                name);
        }
        sxml_item_release(string);
        return status;
    }
    if (sxml_item_get_kind(request->item) != SXML_ITEM_KIND_ATOMIC ||
        sxml_atomic_item_get_datum(request->item, &datum) != SXML_STATUS_OK) {
        return sxml_function_library_report(call, &name_type_error);
    }
    if (datum.datum_kind == SXML_DATUM_KIND_QNAME &&
        !sxml_type_identifier_equals(sxml_item_get_type_annotation(request->item),
                                     sxml_builtin_type_identifier(SXML_TYPE_XS_NOTATION))) {
        *name = datum.datum_details.qname;
        return SXML_STATUS_OK;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return sxml_function_library_report(call, &name_type_error);
    }
    return resolve_name_text(
        call,
        &(name_text){.text = datum.datum_details.string.utf8,
                     .is_attribute_name = request->target_kind == SXML_NODE_KIND_ATTRIBUTE},
        name);
}

/* The new name of a processing instruction: an NCName (XQDY0041), in no
   namespace, given as text or as an xs:QName without prefix (XUDY0025). */
static sxml_status check_target_name(const sxml_function_call *call, sxml_qname_datum *name) {
    if (name->namespace_prefix.index != 0) {
        return sxml_function_library_report(call, &processing_instruction_prefix_error);
    }
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_name_parts parts;
    sxml_status status = sxml_name_pool_get_name_parts(name_pool, name->expanded_name, &parts);
    if (status == SXML_STATUS_OK && parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri =
                                     sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE),
                                 .local_name = parts.local_name},
            &name->expanded_name);
    }
    if (status == SXML_STATUS_OK && !sxml_xml_text_is_ncname(parts.local_name)) {
        status = sxml_function_library_report(call, &invalid_target_name_error);
    }
    /* As for a computed processing-instruction constructor (XQuery 4.0,
       4.12.3.5): XQDY0064 for a target "xml" in any case. */
    if (status == SXML_STATUS_OK && parts.local_name.byte_count == 3 &&
        (parts.local_name.bytes[0] | 0x20U) == 'x' && (parts.local_name.bytes[1] | 0x20U) == 'm' &&
        (parts.local_name.bytes[2] | 0x20U) == 'l') {
        status = sxml_function_library_report(call, &xml_target_name_error);
    }
    return status;
}

/* XUDY0023: the element itself, or an attribute's parent element. */
static sxml_status check_rename_conflict(const sxml_function_call *call, const target_node *target,
                                         sxml_qname_datum name) {
    const bool is_element = target->description.node_kind == SXML_NODE_KIND_ELEMENT;
    if (!is_element && !target->description.has_parent) {
        return SXML_STATUS_OK;
    }
    name_binding binding = {.has_binding = false};
    sxml_status status =
        binding_of_name(sxml_item_heap_get_name_pool(call->item_heap), name, &binding);
    bool is_conflict = false;
    if (status == SXML_STATUS_OK) {
        status = test_conflict(
            &(conflict_test){.element = is_element ? target->node
                                                   : (sxml_node_reference){
                                                         .tree = target->node.tree,
                                                         .node_index = target->description.parent,
                                                         .namespace_number = 0},
                             .binding = binding},
            &is_conflict);
    }
    if (status == SXML_STATUS_OK && is_conflict) {
        status = sxml_function_library_report(call, &namespace_conflict_error);
    }
    return status;
}

static sxml_status make_rename(const primitive_output *output) {
    const sxml_function_call *call = output->call;
    target_node target = {.item = {.representation = 0}};
    update_variant variant = {.variant = 0, .copy_flags = 0};
    sxml_status status = read_variant(call, 1, &variant);
    if (status == SXML_STATUS_OK) {
        status = read_target(call,
                             &(target_request){.value = call->arguments.sequences[1],
                                               .cardinality_error = &rename_target_error},
                             &target);
    }
    const sxml_node_kind kind = target.description.node_kind;
    if (status == SXML_STATUS_OK &&
        !kind_is_one_of(kind, KIND_BIT(SXML_NODE_KIND_ELEMENT) | KIND_BIT(SXML_NODE_KIND_ATTRIBUTE) |
                                  KIND_BIT(SXML_NODE_KIND_PROCESSING_INSTRUCTION))) {
        status = sxml_function_library_report(call, &rename_target_error);
    }
    uint64_t item_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(call->arguments.sequences[2], call->error_record, &item_count);
    }
    if (status == SXML_STATUS_OK && item_count != 1) {
        status = sxml_function_library_report(call, &name_type_error);
    }
    sxml_item new_name_item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            call->arguments.sequences[2],
            (sxml_item_request){.item_index = 0, .error_record = NULL}, &new_name_item);
    }
    sxml_qname_datum name = {.expanded_name = {0}, .namespace_prefix = {0}};
    if (status == SXML_STATUS_OK) {
        status = read_new_name(call, &(new_name_request){.item = new_name_item, .target_kind = kind},
                               &name);
    }
    if (status == SXML_STATUS_OK && kind == SXML_NODE_KIND_PROCESSING_INSTRUCTION) {
        status = check_target_name(call, &name);
    } else if (status == SXML_STATUS_OK) {
        status = check_rename_conflict(call, &target, name);
    }
    sxml_item qname = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum = {.datum_kind = SXML_DATUM_KIND_QNAME, .datum_details = {.qname = name}}},
            &qname);
    }
    if (status == SXML_STATUS_OK) {
        sxml_item_retain(target.item);
        status = append_primitive(output,
                                  &(primitive_request){.primitive_kind =
                                                           SXML_XQUERY_UPDATE_PRIMITIVE_RENAME,
                                                       .target = target.item,
                                                       .parameter = sxml_sequence_from_item(qname),
                                                       .copy_flags = variant.copy_flags});
        sxml_item_release(qname);
    }
    return status;
}

static sxml_status call_rename(const sxml_function_call *call, sxml_sequence *result) {
    return collect_primitives(call, make_rename, result);
}

/* ------------------------------------------------------------- fn:put */

UPDATE_ERROR(put_node_error, SXML_STATUS_DYNAMIC_ERROR, "FOUP0001",
             "fn:put stores only a document or element node (6.1)");
UPDATE_ERROR(put_uri_error, SXML_STATUS_DYNAMIC_ERROR, "FOUP0002",
             "the $uri of fn:put is not a URI that names a location to store the node at "
             "(6.1)");

static bool is_hex_digit(uint8_t byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F') ||
           (byte >= 'a' && byte <= 'f');
}

/* Whether text is a URI reference: none of the characters that RFC 3986
   (2, Appendix A) and RFC 3987 (2.2) leave out of every URI and IRI
   reference (controls, space, " < > \ ^ ` { | }), and % only before two
   hexadecimal digits. Other non-ASCII characters are an IRI's. */
static bool is_uri_reference(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte <= 0x20 || byte == 0x7F || byte == '"' || byte == '<' || byte == '>' ||
            byte == '\\' || byte == '^' || byte == '`' || byte == '{' || byte == '|' ||
            byte == '}') {
            return false;
        }
        if (byte == '%' && (byte_index + 2 >= text.byte_count ||
                            !is_hex_digit(text.bytes[byte_index + 1]) ||
                            !is_hex_digit(text.bytes[byte_index + 2]))) {
            return false;
        }
    }
    return text.byte_count > 0;
}

/* The absolute URI fn:put stores at, as an xs:anyURI item, owned: $uri
   resolved against the static base URI (6.1; 3.1.x upd:put: an absolute
   URI). */
static sxml_status read_put_uri(const sxml_function_call *call, sxml_item *uri_item) {
    sxml_optional_item argument = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!argument.has_item) {
        return sxml_function_library_report(call, &put_uri_error);
    }
    sxml_string reference;
    status = sxml_function_library_read_string_argument(call, 1, &reference);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!is_uri_reference(reference.utf8)) {
        return sxml_function_library_report(call, &put_uri_error);
    }
    const sxml_byte_view base_uri = call->dynamic_context == NULL
                                        ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                        : sxml_dynamic_context_get_static_base_uri(
                                              call->dynamic_context);
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
    status = sxml_uri_resolve(&(sxml_uri_resolution_request){.base_uri = base_uri,
                                                             .reference = reference.utf8,
                                                             .allocator = allocator},
                              &resolution);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!sxml_uri_has_scheme(resolution.uri)) {
        status = sxml_function_library_report(call, &put_uri_error);
    } else {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_URI),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = resolution.uri,
                                                       .codepoint_count = 0,
                                                       .is_ascii = false}}}},
            uri_item);
    }
    sxml_allocator_release(allocator, resolution.memory);
    return status;
}

/* fn:put (6.1): one upd:put primitive whose parameter is the URI and,
   when given, $params. */
static sxml_status make_put(const primitive_output *output) {
    const sxml_function_call *call = output->call;
    sxml_optional_item node_item = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &node_item);
    sxml_node_reference node;
    sxml_node_description description;
    if (status == SXML_STATUS_OK &&
        (!node_item.has_item || sxml_item_get_node(node_item.item, &node) != SXML_STATUS_OK)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(node, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* 6.1: other kinds of node are implementation-defined; sxml stores
       only what a document can be made of. */
    if (description.node_kind != SXML_NODE_KIND_DOCUMENT &&
        description.node_kind != SXML_NODE_KIND_ELEMENT) {
        return sxml_function_library_report(call, &put_node_error);
    }
    sxml_item uri_item = {.representation = 0};
    status = read_put_uri(call, &uri_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item parameter_items[2] = {uri_item, {.representation = 0}};
    size_t parameter_count = 1;
    if (call->arguments.sequence_count > 2) {
        sxml_optional_item parameters = {.has_item = false};
        status = sxml_function_library_read_optional_item(call, 2, &parameters);
        if (status == SXML_STATUS_OK && parameters.has_item) {
            parameter_items[1] = parameters.item;
            parameter_count = 2;
        }
    }
    sxml_sequence parameter = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            call->item_heap,
            (sxml_item_view){.items = parameter_items, .item_count = parameter_count},
            &parameter);
    }
    sxml_item_release(uri_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item_retain(node_item.item);
    status = append_primitive(output,
                              &(primitive_request){.primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_PUT,
                                                   .target = node_item.item,
                                                   .parameter = parameter,
                                                   .copy_flags = 0});
    sxml_sequence_release(parameter);
    return status;
}

static sxml_status call_put(const sxml_function_call *call, sxml_sequence *result) {
    return collect_primitives(call, make_put, result);
}

static const sxml_function_parameter put_parameters[] = {
    SXML_REQUIRED_PARAMETER("node", "node()"),
    SXML_REQUIRED_PARAMETER("uri", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("params", "item()?"),
};

static const sxml_native_function put_rows[] = {
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     /* The static base URI resolves $uri. */
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("put"),
     .parameters = {put_parameters, 3},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("empty-sequence()"),
     .call = call_put,
     .resumable_body = NULL},
};

const sxml_native_function_view sxml_xquery_put_functions = {
    .native_functions = put_rows, .native_function_count = sizeof put_rows / sizeof put_rows[0]};

bool sxml_xquery_is_put_function(const sxml_native_function *native_function) {
    return native_function == &put_rows[0];
}

/* ------------------------------------------------ Revalidation mode */

sxml_xquery_revalidation_mode sxml_xquery_declared_revalidation(const sxml_ast *ast) {
    if (node_of(ast, ast->root)->kind != SXML_AST_KIND_MODULE) {
        return SXML_XQUERY_REVALIDATION_SKIP;
    }
    const sxml_ast_node_index prolog =
        child_of(ast, child_of(ast, ast->root, SXML_AST_FIELD_MODULE), SXML_AST_FIELD_PROLOG);
    for (sxml_ast_node_index declaration = child_of(ast, prolog, SXML_AST_FIELD_DECLARATION);
         prolog.value != 0 && declaration.value != 0; declaration = next_of(ast, declaration)) {
        const sxml_ast_node *node = node_of(ast, declaration);
        if (node->kind != SXML_AST_KIND_REVALIDATION_DECLARATION) {
            continue;
        }
        /* 3.1: at most one (XUST0003, src/xquery_static.c). */
        if (node->detail.keyword == SXML_AST_KEYWORD_LAX) {
            return SXML_XQUERY_REVALIDATION_LAX;
        }
        if (node->detail.keyword == SXML_AST_KEYWORD_STRICT) {
            return SXML_XQUERY_REVALIDATION_STRICT;
        }
    }
    return SXML_XQUERY_REVALIDATION_SKIP;
}

/* --------------------------------------------------------------- Rows */

static const sxml_function_parameter one_parameter[] = {
    SXML_REQUIRED_PARAMETER("target", "item()*"),
};


static const sxml_function_parameter three_parameters[] = {
    SXML_REQUIRED_PARAMETER("variant", "xs:integer"),
    SXML_REQUIRED_PARAMETER("source", "item()*"),
    SXML_REQUIRED_PARAMETER("target", "item()*"),
};

const sxml_native_function sxml_xquery_insert_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("insert"),
    .parameters = {three_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("function(*)*"),
    .call = call_insert,
    .resumable_body = NULL};

const sxml_native_function sxml_xquery_delete_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("delete"),
    .parameters = {one_parameter, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("function(*)*"),
    .call = call_delete,
    .resumable_body = NULL};

const sxml_native_function sxml_xquery_replace_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("replace"),
    .parameters = {three_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("function(*)*"),
    .call = call_replace,
    .resumable_body = NULL};

const sxml_native_function sxml_xquery_rename_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    /* The statically known namespaces of the code running. */
    .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("rename"),
    .parameters = {three_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("function(*)*"),
    .call = call_rename,
    .resumable_body = NULL};
