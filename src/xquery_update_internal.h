#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "ir_internal.h"
#include "sxml/status.h"
#include "sxml/xdm.h"
#include "xpath_ast_internal.h"
#include "xquery_static_internal.h"

/*
 * The XQuery Update Facility 3.0 (docs/DECISIONS.md, D187; packet 7.7a).
 * Internal to sxml.
 *
 * One data model, one evaluator: an updating expression is lowered like
 * any other expression, and its value is its pending update list
 * (glossary `pending_update_list`), carried as an ordinary sequence whose
 * items are update primitives (glossary `update_primitive`). Each update
 * primitive is a function item of the upd namespace
 * (http://www.w3.org/2007/xquery-update-10) whose body is
 * sxml_xquery_update_primitive_operations: it cannot be called, and the
 * static rules below keep it out of every place where an XDM value is
 * read. So:
 *
 * - the comma, FLWOR, if, switch, typeswitch and try/catch expressions
 *   and function calls merge pending update lists by concatenating their
 *   values (XQUF 3.0, 3.2.2 upd:mergeUpdates is a union; no order is
 *   observable), and a try clause that fails drops its updates with its
 *   value;
 * - the evaluator, its tasks and its passes know nothing of updates;
 * - an updating query is a unit whose main function carries %updating
 *   (src/xquery_prolog.c); src/xquery.c applies its value
 *   (sxml_xquery_apply_updates) and hands every changed tree to the
 *   host's update sink (include/sxml/xquery.h).
 *
 * The expressions (XQUF 3.0, 5.4) lower in src/xquery_lower_update.c to
 * STATIC_CALLs of the internal native rows below, which make the
 * primitives after the checks the expressions' semantics give (XUTY0004
 * to XUTY0013, XUDY0009, XUDY0023, XUDY0027, XUDY0029, XUDY0030,
 * XUTY0022). Application (src/xquery_update_apply.c) checks the
 * compatibility of the list (XUDY0015, XUDY0016, XUDY0017, XUDY0024) and
 * rebuilds each target tree through the tree builder in one pass over
 * the old tree, with the primitives of each node applied in the order of
 * XQUF 3.0, 3.2.2 upd:applyUpdates; trees stay immutable (D99, D183), so
 * node identity across the update is not observable within a snapshot.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* The namespace of update primitives (XQUF 3.0, 1.2: prefix upd). */
#define SXML_XQUERY_UPDATE_NAMESPACE "http://www.w3.org/2007/xquery-update-10"

/* ------------------------------------------------------- Static rules */

/* A module whose updating expressions to check. */
typedef struct sxml_xquery_update_rules_request {
    /* The module's static context, read and linked: its AST, function
       table, declared functions and imported modules. */
    const sxml_xquery_static_context *context;
    sxml_error_record *error_record;
} sxml_xquery_update_rules_request;

/*
 * Checks XQUF 3.0's rules on where updating expressions may appear
 * (sections 2.2 and 5.1 to 5.4): XUST0001 for an updating expression
 * where only a simple one is allowed, XUST0002 for a simple expression
 * that is not vacuous where an updating one is required (the modify
 * clause of a copy-modify or transform-with expression, the body of an
 * updating function), XUST0028 for an updating function with a declared
 * result type. Writes whether the query body of a main module is an
 * updating expression to *body_is_updating (false for a library
 * module). Walks the AST with parent links, without recursion (D71).
 *
 * Returns SXML_STATUS_STATIC_ERROR with the error record filled, and the
 * failures of name resolution and the allocator.
 */
SXML_NODISCARD sxml_status sxml_xquery_check_update_rules(
    const sxml_xquery_update_rules_request *request, bool *body_is_updating);

/* A declaration whose annotations to read. */
typedef struct sxml_xquery_annotation_query {
    const sxml_ast *ast;
    /* A FUNCTION_DECLARATION, VARIABLE_DECLARATION or
       INLINE_FUNCTION_EXPRESSION. */
    sxml_ast_node_index declaration;
    /* Resolves annotation prefixes. */
    const sxml_xpath_static_context *static_context;
    sxml_error_record *error_record;
} sxml_xquery_annotation_query;

/*
 * Writes to *is_updating whether the declaration is %updating, written as
 * the annotation or as "declare updating" (XQUF 3.0, 5.4). Returns the
 * failures of name resolution (XPST0081).
 */
SXML_NODISCARD sxml_status sxml_xquery_declaration_is_updating(
    const sxml_xquery_annotation_query *query, bool *is_updating);

/*
 * Gives IR function function_index of unit the annotation %updating (the
 * main function of an updating query, D187). Returns the unit's and the
 * name pool's failures.
 */
typedef struct sxml_xquery_updating_mark {
    sxml_ir_unit *unit;
    sxml_ir_function_index function_index;
} sxml_xquery_updating_mark;

SXML_NODISCARD sxml_status sxml_xquery_mark_updating_function(
    const sxml_xquery_updating_mark *mark);

/* Whether the annotations, of a function of item_heap's name pool,
   include %updating. */
bool sxml_xquery_annotations_are_updating(const sxml_item_heap *item_heap,
                                          sxml_function_annotation_view annotations);

/* ------------------------------------------------- Update primitives */

/*
 * The body of every update primitive. Calling it raises XUDY0038 (XQUF
 * 3.0, 5.3: a pending update list is not a function to call).
 */
extern const sxml_function_body_operations sxml_xquery_update_primitive_operations;

/* Whether item is an update primitive. */
bool sxml_xquery_is_update_primitive(sxml_item item);

/* The update primitives (3.1), named upd:insertInto ... upd:rename. */
typedef enum sxml_xquery_update_primitive_kind {
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO,
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_FIRST,
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_LAST,
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_BEFORE,
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_AFTER,
    SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_ATTRIBUTES,
    SXML_XQUERY_UPDATE_PRIMITIVE_DELETE,
    SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_NODE,
    SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_VALUE,
    SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_ELEMENT_CONTENT,
    SXML_XQUERY_UPDATE_PRIMITIVE_RENAME,
} sxml_xquery_update_primitive_kind;

/* What one update primitive says. Views are borrowed from its item. */
typedef struct sxml_xquery_update_primitive {
    sxml_xquery_update_primitive_kind primitive_kind;
    sxml_node_reference target;
    /*
     * The inserts and REPLACE_NODE: the nodes to copy in, in order, with
     * xs:string items for the text that atomic items of the source made
     * (3.1.1: the content of an element constructor, 5.4.1 step 1);
     * REPLACE_VALUE and REPLACE_ELEMENT_CONTENT: one xs:string (empty for
     * no text node); RENAME: one xs:QName; DELETE: empty.
     */
    sxml_sequence parameter;
    /* How its nodes are copied in: the prolog's copy-namespaces modes
       (XQuery 4.0, 5.9; preserve, inherit by default). */
    bool preserves_namespaces;
    bool inherits_namespaces;
    /* Construction preserve (XQuery 4.0, 5.6): copies keep their types. */
    bool preserves_types;
} sxml_xquery_update_primitive;

/* Reads an update primitive. Returns SXML_STATUS_INVALID_ARGUMENT for an
   item that is not one. */
SXML_NODISCARD sxml_status sxml_xquery_read_update_primitive(
    sxml_item item, sxml_xquery_update_primitive *primitive);

/*
 * The internal native rows (in no function table) that updating
 * expressions lower to (src/xquery_lower_update.c). Their parameters are
 * item()*; each checks its operands as the expression's semantics say and
 * returns the update primitives, an xs:integer first argument selecting
 * the variant where an expression has several:
 *
 * - insert($position, $source, $target): $position 0 into, 1 as first
 *   into, 2 as last into, 3 before, 4 after (XQUF 3.0, 5.4.1);
 * - delete($target) (5.4.2);
 * - replace($value-of, $target, $replacement): $value-of 0 replaces the
 *   node, 1 its value (5.4.3);
 * - for insert and replace the variant may add
 *   SXML_XQUERY_UPDATE_NO_PRESERVE_NAMESPACES,
 *   SXML_XQUERY_UPDATE_NO_INHERIT_NAMESPACES and
 *   SXML_XQUERY_UPDATE_PRESERVES_TYPES, the prolog's copy-namespaces and
 *   construction modes for the nodes copied in;
 * - rename($target, $new-name) (5.4.4).
 */
#define SXML_XQUERY_UPDATE_NO_PRESERVE_NAMESPACES 8
#define SXML_XQUERY_UPDATE_NO_INHERIT_NAMESPACES 16
#define SXML_XQUERY_UPDATE_PRESERVES_TYPES 32

extern const sxml_native_function sxml_xquery_insert_function;
extern const sxml_native_function sxml_xquery_delete_function;
extern const sxml_native_function sxml_xquery_replace_function;
extern const sxml_native_function sxml_xquery_rename_function;

/*
 * The copy-modify expression (XQUF 3.0, 5.4.5) lowers to
 *
 *     let $c := copy(E) ... let $r := modify-copies(($c, ...), M)
 *     ... return R with $c bound to $r(1), ...
 *
 * - copy($node, $modes): a copy of one node (XUTY0013 otherwise), the
 *   root of a new tree, made as the prolog says for copies (XQuery 4.0,
 *   5.6 and 5.9): $modes adds SXML_XQUERY_UPDATE_NO_PRESERVE_NAMESPACES,
 *   SXML_XQUERY_UPDATE_NO_INHERIT_NAMESPACES and
 *   SXML_XQUERY_UPDATE_PRESERVES_TYPES (construction preserve);
 * - modify-copies($copies, $updates): applies the pending update list
 *   $updates, whose targets must be in the trees of $copies (XUDY0014),
 *   and returns one array of the roots of the trees after it, in the
 *   order of $copies, whose members the return clause binds the copy
 *   variables to.
 */
extern const sxml_native_function sxml_xquery_copy_function;
extern const sxml_native_function sxml_xquery_modify_copies_function;

/* --------------------------------------------------------- Application */

/* A pending update list to apply. */
typedef struct sxml_xquery_update_application {
    sxml_item_heap *item_heap;
    /* Update primitives, borrowed; other items are
       SXML_STATUS_INVALID_ARGUMENT. */
    sxml_sequence pending_updates;
    /* When not empty: the roots of the only trees the primitives may
       target (the copies of a copy-modify expression; XUDY0014
       otherwise). Borrowed. */
    sxml_sequence allowed_roots;
    /* Receives the dynamic errors of merging and applying. */
    sxml_error_record *error_record;
} sxml_xquery_update_application;

/* The trees an application changed, in the order of their first
   primitive: original_roots[n] became updated_roots[n] (both owned). */
typedef struct sxml_xquery_updated_trees {
    sxml_sequence original_roots;
    sxml_sequence updated_roots;
} sxml_xquery_updated_trees;

/*
 * Applies application->pending_updates (XQUF 3.0, 3.2.2
 * upd:applyUpdates): checks compatibility (upd:mergeUpdates: XUDY0015,
 * XUDY0016, XUDY0017, XUDY0024), then rebuilds every tree a primitive
 * targets with every change made, and writes the old and new roots to
 * *updated_trees. Rebuilt elements and their ancestors become xs:untyped
 * (upd:setToUntyped); other nodes keep their annotations. XUDY0021 when a
 * rebuilt element would have two attributes of one name or conflicting
 * namespace bindings.
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR or SXML_STATUS_TYPE_ERROR with the
 * error record filled, and the failures of the trees and the heap; on
 * failure nothing is written and every tree built is released.
 */
SXML_NODISCARD sxml_status sxml_xquery_apply_updates(
    const sxml_xquery_update_application *application,
    sxml_xquery_updated_trees *updated_trees);

/* Releases both sequences of updated_trees and empties them. */
void sxml_xquery_release_updated_trees(sxml_xquery_updated_trees *updated_trees);
