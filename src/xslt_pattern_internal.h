#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir_internal.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "xpath_ast_internal.h"
#include "xpath_lower_internal.h"
#include "xpath_static_internal.h"

/*
 * XSLT 4.0 patterns (docs/DECISIONS.md, D118). Internal to sxml.
 *
 *     pattern text -> pattern AST (sxml_xslt_pattern_build_ast, through the
 *     parser part seam of D111 with grammars/xslt_pattern)
 *     -> match expression (an XPath AST, sxml_xslt_pattern_build_match_
 *     expression) -> match function (sxml_xslt_pattern_lower_match_function)
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, as of October 2026).
 *
 * The pattern AST. Its root is PATTERN; its pattern nodes are the
 * ..._PATTERN kinds of sxml_ast_kind, and predicates, node tests, variable
 * references, names and constants inside them are the XPath kinds, as the
 * XPath front end builds them. For "chapter/section[1] | @id":
 *
 *     PATTERN
 *       pattern: UNION_PATTERN operator UNION
 *         left: RELATIVE_PATH_PATTERN operator SLASH
 *           left: AXIS_STEP_PATTERN axis NONE  test: NCNAME "chapter"
 *           right: AXIS_STEP_PATTERN axis NONE  test: NCNAME "section"
 *             predicate: PREDICATE  expression: INTEGER_LITERAL "1"
 *         right: AXIS_STEP_PATTERN axis ATTRIBUTE (ABBREVIATED)
 *           test: NCNAME "id"
 *
 * Syntax errors are XTSE0340 (6.3.1), as is a function call whose
 * unprefixed name and arity are not one of fn:doc#1, fn:id#1, fn:id#2,
 * fn:element-with-id#1, fn:element-with-id#2, fn:key#2, fn:key#3 and
 * fn:root#0 (6.3.2); a prefixed name is checked when the match expression
 * is built, where the namespaces are known.
 *
 * The match expression. A pattern is turned into an XPath expression of
 * xs:boolean whose context value is the item being matched: the item
 * matches the pattern exactly when the expression is true. It is an XPath
 * AST (root XPATH) whose leaves are copies of the pattern's predicates,
 * node tests and names, with their source spans, so static errors in them
 * point into the pattern; it is lowered like any XPath expression, through
 * the embedding entry (D103). The variables it binds are in the namespace
 * SXML_XSLT_PATTERN_VARIABLE_NAMESPACE_URI, which no stylesheet can write.
 *
 * XNode patterns (6.3.2.2) are not evaluated as their equivalent
 * expression root(.)//(EE) but read from right to left, as 6.3.2.3
 * describes: a step is a node test on the item, and the steps to its left
 * are a condition on its parent (after "/") or on one of its ancestors
 * (after "//"). For "chapter/section" the match expression is
 *
 *     boolean(. instance of node() and
 *             (self::section and exists(parent::node()[self::chapter])))
 *
 * A step with predicates is tested by selecting it again from the nodes it
 * may have been selected from, because positions count relative to them:
 * "section[1]" (a leading step on the child-or-top axis) is
 *
 *     boolean(. instance of node() and
 *       (let $p:n := . return
 *          if (exists(parent::node()))
 *          then exists(parent::node()/child::section[1][. is $p:n])
 *          else (self::section and exists(.[1]))))
 *
 * Patterns with intersect or except, a filter on a parenthesized pattern,
 * or a parenthesized right-hand step that holds more than steps and unions
 * use the formal definition: some candidate context node $p:x (an
 * ancestor of the item or the item itself, never an attribute or namespace
 * node with a parent) has the item in $p:x/(EE), where EE is the
 * equivalent expression with child-or-top, attribute-or-top and
 * namespace-or-top written out.
 *
 * Simple patterns (6.3.3 to 6.3.9) test the item directly:
 * "atomic(xs:integer)[. gt 0]" is
 * boolean(exists(.[. instance of xs:integer][. gt 0])); map, array and
 * JNode patterns test their entries, members and jvalue with the sequence
 * pattern rules of 6.3.5 (count, then every item matching).
 *
 * Errors in patterns (6.5): a dynamic or type error while a match
 * expression is evaluated means the item does not match. The match
 * expression itself raises such errors; whoever evaluates it (the
 * evaluator's APPLY_TEMPLATES, xsl:number, grouping, keys) treats an error
 * as false.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* The namespace of the variables a match expression binds. */
#define SXML_XSLT_PATTERN_VARIABLE_NAMESPACE_URI "urn:x-sxml:pattern-variable"

/*
 * Parses request->expression_text, an XSLT 4.0 pattern, with
 * grammars/xslt_pattern and writes an AST, owned (release with
 * sxml_ast_destroy), whose root is a PATTERN node, to *ast; see above.
 * Returns SXML_STATUS_STATIC_ERROR (XTSE0340) for text that is not a
 * pattern, SXML_STATUS_INVALID_UTF8, SXML_STATUS_LIMIT_EXCEEDED, and the
 * failures of the allocator. Declared in src/xpath_ast_internal.h with the
 * other front ends. */

/*
 * How the template rules of a typed mode read element names (XSLT 4.0,
 * 6.9.4): in a mode with typed="strict" or "lax", an EQName test on an
 * axis whose principal node kind is element, in the first step of a
 * relative path of the match pattern, is read as schema-element(E); with
 * "lax" only when a global element declaration of E is in scope, with
 * "strict" always, and its absence is XTSE3105.
 */
typedef enum sxml_xslt_pattern_typed_names {
    SXML_XSLT_PATTERN_TYPED_NAMES_NONE = 0,
    SXML_XSLT_PATTERN_TYPED_NAMES_LAX,
    SXML_XSLT_PATTERN_TYPED_NAMES_STRICT,
} sxml_xslt_pattern_typed_names;

/* A pattern to turn into its match expression. */
typedef struct sxml_xslt_pattern_match_expression_request {
    /* Root PATTERN. Borrowed for the call; the match expression borrows
       its source name. */
    const sxml_ast *pattern_ast;
    /* For the names of function calls at the outermost level (6.3.2):
       namespaces and the default function namespace. */
    const sxml_xpath_static_context *static_context;
    /* Memory for the match expression, which is made with
       sxml_ast_create. */
    sxml_allocator allocator;
    /* Receives XTSE0340 or XPST0081 for a function name that does not
       bind to a permitted function, XTSE3105 (typed_names); may be
       NULL. */
    sxml_error_record *error_record;
    /* The typed mode the match expression is for (6.9.4); NONE needs no
       static context. */
    sxml_xslt_pattern_typed_names typed_names;
} sxml_xslt_pattern_match_expression_request;

/*
 * Writes the match expression of request->pattern_ast, an AST owned by
 * the caller (release with sxml_ast_destroy) whose root is an XPATH node,
 * to *match_expression. Returns SXML_STATUS_STATIC_ERROR (see the
 * request), SXML_STATUS_INVALID_ARGUMENT for an AST whose root is not
 * PATTERN, SXML_STATUS_LIMIT_EXCEEDED for a match expression beyond
 * UINT32_MAX nodes, and the failures of the allocator. On failure nothing
 * is left allocated.
 */
SXML_NODISCARD sxml_status sxml_xslt_pattern_build_match_expression(
    const sxml_xslt_pattern_match_expression_request *request,
    sxml_ast **match_expression);

/* A pattern to lower into a match function. */
typedef struct sxml_xslt_pattern_match_function_request {
    /* Root PATTERN. Borrowed for the call. */
    const sxml_ast *pattern_ast;
    /* The pattern's static context (namespaces, function table, the
       stylesheet's global variables as the unit's globals); NULL keeps
       the lowerer's. Its item heap and allocator must be the lowerer's. */
    const sxml_xpath_static_context *static_context;
    /* As sxml_xslt_pattern_match_expression_request's. */
    sxml_xslt_pattern_typed_names typed_names;
} sxml_xslt_pattern_match_function_request;

/*
 * Adds a match function to the lowerer's unit and writes its index to
 * *match_function: an anonymous IR function of one parameter, item(), with
 * result xs:boolean, defined at the top level, whose body is the lowered
 * match expression with the parameter as its fixed focus (the function
 * the IR example of src/ir_internal.h shows for a template rule). For
 * callers outside a lowering machine (D103) at the top level of a unit:
 * no local variable may be in the lowerer's scope, since the function
 * captures nothing. Fails as sxml_xslt_pattern_build_match_expression,
 * sxml_xpath_lowerer_lower_embedded and the unit do.
 */
SXML_NODISCARD sxml_status sxml_xslt_pattern_lower_match_function(
    sxml_xpath_lowerer *lowerer,
    const sxml_xslt_pattern_match_function_request *request,
    sxml_ir_function_index *match_function);

/* A pattern whose default priority to compute. */
typedef struct sxml_xslt_pattern_priority_request {
    /* Root PATTERN. Borrowed for the call. */
    const sxml_ast *pattern_ast;
    /* For the type names of atomic patterns (6.4.3); also gives the
       default namespace for elements and types. */
    const sxml_xpath_static_context *static_context;
    /* True when the default namespace for elements and types is "##any"
       ([xsl:]xpath-default-namespace, 5.1.2.1): an unprefixed element
       name E then counts as *:E (6.4.1). */
    bool is_any_namespace;
    /* Receives the static errors of resolving those type names; may be
       NULL. */
    sxml_error_record *error_record;
} sxml_xslt_pattern_priority_request;

/*
 * Writes the default priority of request->pattern_ast (6.4) to *priority,
 * a value from -1 to +1. Returns SXML_STATUS_STATIC_ERROR (XPST0081,
 * XPST0051 for a name in atomic(...) that is not a type),
 * SXML_STATUS_INVALID_ARGUMENT for an AST whose root is not PATTERN, and
 * the failures of the allocator and type registry.
 */
SXML_NODISCARD sxml_status sxml_xslt_pattern_compute_default_priority(
    const sxml_xslt_pattern_priority_request *request, double *priority);

/* An element or attribute name a pattern may match (perf-15). */
typedef struct sxml_xslt_pattern_named_category {
    sxml_ir_item_category item_category;
    sxml_name_identifier node_name;
} sxml_xslt_pattern_named_category;

/*
 * The item requirement of a pattern (glossary `item_requirement`,
 * perf-15): the item categories and names of the items it may match. An
 * item outside it does not match the pattern; an item inside it may.
 * Read from the pattern AST as XSLT 4.0, 6.3 defines matching: the items
 * a pattern matches are those of the last step of a path (after "/",
 * "//" or a rooted start), of both sides of a union, of the left of
 * intersect and except, of the value of a filter and of the inside of
 * parentheses. An axis step gives the kind of its node test, or for a
 * name test or wildcard the principal node kind of its axis (XPath 4.0,
 * 4.6.4.1), and for an EQName test of an element or attribute its
 * expanded name; "/" gives document nodes. Every axis step may also match
 * items that are not XNodes (OTHER: the analysis does not decide JNodes).
 * Every other pattern ($v, id(), key(), doc(), a simple pattern, ".") may
 * match any item. An element name read under the default namespace
 * "##any" (any namespace) or in a typed mode (6.9.4: schema-element(E)
 * matches its substitution group) gives elements of any name.
 */
typedef struct sxml_xslt_pattern_requirement {
    /* Bit 1 << c is set when the pattern may match an item of category c
       (sxml_ir_item_category) whatever its name. */
    uint32_t any_name_categories;
    /* The elements and attributes it may match beyond those, by name,
       perhaps with duplicates; allocator memory, owned (release with
       sxml_xslt_pattern_requirement_finalize). */
    sxml_xslt_pattern_named_category *named_categories;
    size_t named_category_count;
    size_t named_category_capacity;
    sxml_allocator allocator;
} sxml_xslt_pattern_requirement;

/* A pattern whose item requirement to find. */
typedef struct sxml_xslt_pattern_requirement_request {
    /* Root PATTERN. Borrowed for the call. */
    const sxml_ast *pattern_ast;
    /* The pattern's static context, as its match function's: names
       resolve in it; its allocator holds the requirement's names. */
    const sxml_xpath_static_context *static_context;
    /* The typed mode the rule is in (6.9.4). */
    sxml_xslt_pattern_typed_names typed_names;
} sxml_xslt_pattern_requirement_request;

/*
 * Writes the item requirement of request->pattern_ast to *requirement,
 * which the caller releases with sxml_xslt_pattern_requirement_finalize
 * whatever the outcome. A name that does not resolve gives its category
 * whatever the name (the match function reports the error). Returns
 * SXML_STATUS_INVALID_ARGUMENT for an AST whose root is not PATTERN, and
 * the failures of the allocator and the name pool.
 */
SXML_NODISCARD sxml_status sxml_xslt_pattern_find_requirement(
    const sxml_xslt_pattern_requirement_request *request,
    sxml_xslt_pattern_requirement *requirement);

/* Releases the names of requirement and leaves it empty. */
void sxml_xslt_pattern_requirement_finalize(sxml_xslt_pattern_requirement *requirement);
