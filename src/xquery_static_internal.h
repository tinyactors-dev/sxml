#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "ir_internal.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xpath.h"
#include "xpath_ast_internal.h"
#include "xpath_static_internal.h"

/*
 * The static context of an XQuery main module (docs/DECISIONS.md, D135;
 * packet 3.10). Internal to sxml.
 *
 * sxml_xquery_read_static_context applies a main module's prolog
 * (XQuery 4.0, section 5) to the host's static context and checks what
 * the prolog alone decides: one XPath static context
 * (sxml_xpath_static_context, src/xpath_static_internal.h) that lowering
 * then uses for every expression of the module, so the core lowers names,
 * types and calls as it does for XPath, with no XQuery machinery of its
 * own:
 *
 * - namespace bindings: the namespace declarations and the default element
 *   namespace (prefix 0) in prolog order, then the host's bindings, then
 *   the prefixes XQuery predeclares beyond XPath's (local, output; 2.1.4);
 * - variable declarations: the host's, then the prolog's in prolog order,
 *   so prolog variable n is global variable host count + n of the unit;
 * - a function table that finds the declared functions (one IR function
 *   of the unit per arity of the arity range, added here so calls and
 *   recursion find their index before any body is lowered), then the
 *   host's table; an unprefixed name of a call, which the core resolves to
 *   the fn namespace, is searched among no-namespace declared functions
 *   first when no default function namespace is declared (5.14.2);
 * - the static base URI of a base URI declaration, resolved against the
 *   host's.
 *
 * Checked here (static errors in the error record): XQST0033, XQST0066,
 * XQST0070 (namespaces), XQST0068, XQST0038, XQST0032, XQST0067,
 * XQST0065, XQST0069, XQST0055, XQST0099 (a setter or context value
 * declaration twice), XQST0038 (an unknown default collation), XQST0049
 * (a variable twice, also against the host's), XQST0034 (arity ranges of
 * one name overlap), XQST0039, XQST0045, XQST0106, XQST0116, XQST0148,
 * XQST0009 (schema import, D97), XQST0109 and XQST0110 (output
 * declarations, whose values the serializer reads), XPST0081 and the
 * type errors of sxml_xpath_resolve_sequence_type. Annotations in a reserved namespace
 * other than %public and %private are XQST0045 (5.15), so %updating is
 * rejected (D97).
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* One declared function: its IR functions are first_function and the
   ones after it, one per arity from required_count to parameter_count. */
typedef struct sxml_xquery_declared_function {
    sxml_name_identifier function_name;
    /* The FUNCTION_DECLARATION node. */
    sxml_ast_node_index declaration;
    uint32_t parameter_count;
    /* Parameters without a default (5.18.3). */
    uint32_t required_count;
    /* The IR function of arity required_count. */
    sxml_ir_function_index first_function;
    /* parameter_count entries of the context's parameter arrays from
       first_parameter on. */
    size_t first_parameter;
    /* parameter_count - required_count + 1 entries of the context's
       signatures from first_signature on, one per arity. */
    size_t first_signature;
    sxml_sequence_type result_type;
} sxml_xquery_declared_function;

/* What sxml_xquery_read_static_context reads. */
typedef struct sxml_xquery_static_request {
    /* A MODULE whose module is a MAIN_MODULE; borrowed for the context's
       life. */
    const sxml_ast *ast;
    /* The host's static context; borrowed for the context's life. */
    const sxml_xpath_static_context *host_static_context;
    /* The unit the declared functions' IR functions are added to (empty
       bodies, which lowering replaces). */
    sxml_ir_unit *unit;
    sxml_error_record *error_record;
} sxml_xquery_static_request;

/*
 * A main module's static context. A plain value whose arrays are allocated
 * with the host's allocator (sxml_xquery_release_static_context releases
 * them); xpath borrows them and the AST.
 */
typedef struct sxml_xquery_static_context {
    /* What lowering uses for every expression of the module. */
    sxml_xpath_static_context xpath;
    const sxml_ast *ast;
    sxml_function_table host_function_table;
    sxml_namespace_binding *namespace_bindings;
    size_t namespace_binding_count;
    sxml_variable_declaration *variable_declarations;
    size_t variable_declaration_count;
    /* The host's variables are the first host_variable_count. */
    size_t host_variable_count;
    /* The VARIABLE_DECLARATION of prolog variable n is entry n - 1. */
    sxml_ast_node_index *variable_nodes;
    sxml_xquery_declared_function *declared_functions;
    size_t declared_function_count;
    /* Parameter types and names (for keyword arguments) of every declared
       function, and one signature per arity. */
    sxml_sequence_type *parameter_types;
    sxml_function_parameter *function_parameters;
    sxml_type_identifier *signatures;
    /* The CONTEXT_VALUE_DECLARATION; 0 for none. */
    sxml_ast_node_index context_value_declaration;
    /* The required type of the initial context value (5.17). */
    sxml_sequence_type context_value_type;
    bool has_default_function_namespace;
    sxml_namespace_uri_identifier default_function_namespace;
    /* What xpath.static_base_uri views when a base URI declaration made it;
       NULL otherwise. */
    void *static_base_uri_memory;
} sxml_xquery_static_context;

/*
 * Reads request->ast's prolog into *context (see the file comment).
 * Returns SXML_STATUS_STATIC_ERROR with the error record filled,
 * SXML_STATUS_UNSUPPORTED_FEATURE for declarations not supported yet
 * (module imports, packet 3.11), SXML_STATUS_INVALID_ARGUMENT for a
 * request without an AST, host static context or unit, and the failures of
 * the name pool, type registry, unit and allocator; on failure nothing is
 * left allocated and *context is untouched.
 */
SXML_NODISCARD sxml_status sxml_xquery_read_static_context(
    const sxml_xquery_static_request *request, sxml_xquery_static_context *context);

/* Releases what sxml_xquery_read_static_context allocated. NULL is
   allowed. */
void sxml_xquery_release_static_context(sxml_xquery_static_context *context);

/* The declared function of a FUNCTION_DECLARATION node; NULL for none. */
const sxml_xquery_declared_function *sxml_xquery_find_declared_function(
    const sxml_xquery_static_context *context, sxml_ast_node_index declaration);
