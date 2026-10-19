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
 * The static context of an XQuery module (docs/DECISIONS.md, D135, D148;
 * packets 3.10, 3.11). Internal to sxml.
 *
 * sxml_xquery_read_static_context applies a module's prolog
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
 * - variable declarations: the host's, then the prolog's in prolog order
 *   (prolog variable n is global variable global_number_base + n of the
 *   unit), then, after sxml_xquery_link_static_context, the public
 *   variables of the imported modules, each with its global number;
 * - a function table that finds the declared functions (one IR function
 *   of the unit per arity of the arity range, added here so calls and
 *   recursion find their index before any body is lowered), then the
 *   public functions of the imported modules (after linking), then the
 *   host's table; an unprefixed name of a call, which the core resolves to
 *   the fn namespace, is searched among no-namespace declared functions
 *   first when no default function namespace is declared (5.14.2);
 * - the static base URI of a base URI declaration, resolved against the
 *   host's;
 * - the decimal formats (5.10: the host's, those the prolog declares
 *   replacing the host's of the same name) and the default collation (5.4)
 *   in unit_static_context, which lowering copies into the unit (D131);
 * - named item types (5.19) as aliases of the static context (D135), and
 *   nominative record types (5.20) declared in the type registry (a
 *   record of the same name declared before, by an earlier compilation
 *   for the same item heap, is reused), with a constructor function for
 *   %constructor records;
 * - the in-scope schema definitions (5.11; D174): the host's schemas,
 *   then one schema compiled from the schema imports of the namespaces
 *   no host schema defines (RESOLVED documents: the location hints, or
 *   the namespace alone, the target namespace checked), owned by the
 *   processor; a main module without schemas gets the schema of the built-in
 *   components; a schema import also binds its prefix like a namespace
 *   declaration, or sets the default element namespace.
 *
 * Checked here (static errors in the error record): XQST0033, XQST0066,
 * XQST0070 (namespaces), XQST0068, XQST0038, XQST0032, XQST0067,
 * XQST0065, XQST0069, XQST0055, XQST0099 (a setter or context value
 * declaration twice), XQST0038 (an unknown default collation), XQST0049
 * (a variable twice, also against the host's), XQST0034 (arity ranges of
 * one name overlap), XQST0039, XQST0045, XQST0106, XQST0116, XQST0148,
 * XQST0057, XQST0058, XQST0059, XQST0035 and XQST0012 (schema imports,
 * D174), XQST0111, XQST0114, XQST0097 and
 * XQST0098 (decimal formats), XQST0109 and XQST0110 (output
 * declarations, whose values the serializer reads), XPST0081 and the
 * type errors of sxml_xpath_resolve_sequence_type. Annotations in a reserved namespace
 * other than %public, %private, %updating and %simple are XQST0045
 * (5.15); the XQuery Update Facility adds XUST0003, XUST0032 and XUST0033
 * (D187); every revalidation mode is supported (D205).
 *
 * A library module (D148): every module starts from XQuery 4.0's default
 * static context (namespaces, no host variables, the module URI as static
 * base URI), its module declaration and module imports bind prefixes
 * (XQST0033, XQST0070, XQST0088), its public functions, variables, item
 * types and records are in its target namespace (XQST0048), it declares
 * no output declaration (XQST0108) and no context value (XQST0113).
 * Imports name namespaces: the module sees the public declarations of
 * every module of the module set with an imported namespace (5.12.2), but
 * not what those import. Linking checks XQST0049 and XQST0034 against
 * them.
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
    /* The default functions (glossary `default_function`) of the
       parameters whose default is SXML_PARAMETER_DEFAULT_EXPRESSION, in
       parameter order, the IR functions after the full arity's. */
    uint32_t default_function_count;
    /* parameter_count entries of the context's parameter arrays from
       first_parameter on. */
    size_t first_parameter;
    /* parameter_count - required_count + 1 entries of the context's
       signatures from first_signature on, one per arity. */
    size_t first_signature;
    sxml_sequence_type result_type;
    /* For the constructor of a record (declaration a
       NOMINATIVE_RECORD_TYPE_DECLARATION, 5.20.2): the record type; 0
       for a FUNCTION_DECLARATION. */
    sxml_type_identifier record_type;
    /* %private: hidden from module import (5.18). */
    bool is_private;
} sxml_xquery_declared_function;

/* A nominative record type the prolog declares (5.20). */
typedef struct sxml_xquery_declared_record {
    sxml_ast_node_index declaration;
    sxml_name_identifier record_name;
    sxml_type_identifier record_type;
    /* With %constructor (5.20.2). */
    bool has_constructor;
    /* Declared by this prolog, not by an earlier compilation for the same
       item heap. */
    bool is_new;
    /* %private: hidden from module import (5.20). */
    bool is_private;
} sxml_xquery_declared_record;

typedef struct sxml_xquery_static_context sxml_xquery_static_context;

/* The static contexts of the modules a module imports. */
typedef struct sxml_xquery_static_context_view {
    const sxml_xquery_static_context *const *static_contexts;
    size_t static_context_count;
} sxml_xquery_static_context_view;

/* What sxml_xquery_read_static_context reads. */
typedef struct sxml_xquery_static_request {
    /* A MODULE; borrowed for the context's life. */
    const sxml_ast *ast;
    /* The host's static context; borrowed for the context's life. For a
       library module: no namespace bindings or variables, the module URI
       as static base URI. */
    const sxml_xpath_static_context *host_static_context;
    /* The unit the declared functions' IR functions are added to (empty
       bodies, which lowering replaces). */
    sxml_ir_unit *unit;
    sxml_error_record *error_record;
    /* A library module's target namespace (5.2); 0 for a main module. */
    sxml_namespace_uri_identifier target_namespace;
    /* Prolog variable n (from 1) is global variable global_number_base +
       n of the unit. */
    uint32_t global_number_base;
    /* The modules of the imported namespaces (this module excluded),
       read already or not yet (an import cycle): the public item types of
       those read are in scope. Borrowed for the context's life. */
    sxml_xquery_static_context_view imported_modules;
} sxml_xquery_static_request;

/*
 * A module's static context. A plain value whose arrays are allocated
 * with the host's allocator (sxml_xquery_release_static_context releases
 * them); xpath borrows them and the AST. It does not move once read: its
 * function table points to it.
 */
struct sxml_xquery_static_context {
    /* What lowering uses for every expression of the module. */
    sxml_xpath_static_context xpath;
    const sxml_ast *ast;
    sxml_function_table host_function_table;
    sxml_namespace_binding *namespace_bindings;
    size_t namespace_binding_count;
    sxml_variable_declaration *variable_declarations;
    size_t variable_declaration_count;
    /* The host's variables are the first host_variable_count, the
       prolog's the next prolog_variable_count, then the imported ones. */
    size_t host_variable_count;
    size_t prolog_variable_count;
    /* The global number of each variable declaration (xpath's
       global_numbers). */
    uint32_t *global_numbers;
    /* The VARIABLE_DECLARATION of prolog variable n is entry n - 1, and
       whether it is %private. */
    sxml_ast_node_index *variable_nodes;
    bool *private_variables;
    sxml_xquery_declared_function *declared_functions;
    size_t declared_function_count;
    /* Parameter types and names (for keyword arguments) of every declared
       function, and one signature per arity. */
    sxml_sequence_type *parameter_types;
    sxml_function_parameter *function_parameters;
    sxml_type_identifier *signatures;
    /* Named item types (5.19) and nominative record types (5.20). */
    sxml_xpath_named_item_type *named_item_types;
    size_t named_item_type_count;
    /* The first imported_item_type_count named item types are the
       imported modules' public ones; the prolog's follow, each with its
       %private in private_item_types (entry n for type n). */
    size_t imported_item_type_count;
    bool *private_item_types;
    sxml_xquery_declared_record *declared_records;
    size_t declared_record_count;
    /* The CONTEXT_VALUE_DECLARATION; 0 for none. */
    sxml_ast_node_index context_value_declaration;
    /* The required type of the initial context value (5.17). */
    sxml_sequence_type context_value_type;
    bool has_default_function_namespace;
    sxml_namespace_uri_identifier default_function_namespace;
    /* What xpath.static_base_uri views when a base URI declaration made it;
       NULL otherwise. */
    void *static_base_uri_memory;
    /* What xpath.unit_static_context's default collation URI views when a
       default collation declaration made it, and its decimal formats and
       their properties when the prolog declares any; NULL otherwise. */
    void *default_collation_memory;
    sxml_decimal_format *decimal_formats;
    sxml_decimal_format_property *decimal_format_properties;
    /* A library module's target namespace; 0 for a main module. */
    sxml_namespace_uri_identifier target_namespace;
    /* The imported modules (after sxml_xquery_link_static_context). */
    sxml_xquery_static_context_view imported_modules;
    /* The in-scope schemas when the prolog imports schemas (5.11): the
       host's, then the one schema the imports compile, which the
       processor owns (sxml_processor_keep_schema); xpath's
       unit_static_context views it. NULL otherwise. */
    const sxml_schema **in_scope_schemas;
};

/*
 * Reads request->ast's prolog into *context (see the file comment).
 * Returns SXML_STATUS_STATIC_ERROR with the error record filled,
 * SXML_STATUS_INVALID_ARGUMENT for a
 * request without an AST, host static context or unit, and the failures of
 * the name pool, type registry, unit and allocator; on failure nothing is
 * left allocated and *context is untouched.
 */
SXML_NODISCARD sxml_status sxml_xquery_read_static_context(
    const sxml_xquery_static_request *request, sxml_xquery_static_context *context);

/*
 * Adds the public variables of imports (the imported modules, read) to
 * *context's variables in scope, with their global numbers, and makes its
 * function table find their public functions; checks XQST0049 (a variable
 * declared and imported, or imported twice) and XQST0034 (functions of
 * one name with overlapping arities among the declared and imported
 * ones). Returns SXML_STATUS_STATIC_ERROR with the error record filled
 * and the allocator's failures; *context then imports no function and
 * stays releasable.
 */
typedef struct sxml_xquery_link_request {
    sxml_xquery_static_context_view imports;
    sxml_error_record *error_record;
} sxml_xquery_link_request;

SXML_NODISCARD sxml_status sxml_xquery_link_static_context(
    const sxml_xquery_link_request *request, sxml_xquery_static_context *context);

/* A URILiteral to intern, its whitespace normalized as for xs:anyURI
   (XQuery 4.0, A.3.1). */
typedef struct sxml_xquery_uri_literal {
    sxml_name_pool *name_pool;
    /* For a copy while collapsing whitespace. */
    sxml_allocator allocator;
    sxml_byte_view text;
} sxml_xquery_uri_literal;

SXML_NODISCARD sxml_status sxml_xquery_intern_uri_literal(const sxml_xquery_uri_literal *literal,
                                                          sxml_namespace_uri_identifier *uri);

/* Releases what sxml_xquery_read_static_context allocated. NULL is
   allowed. */
void sxml_xquery_release_static_context(sxml_xquery_static_context *context);

/*
 * The parameters of a declared function: the PARAMETER_WITH_DEFAULT
 * children of a FUNCTION_DECLARATION, or the fields of a
 * NOMINATIVE_RECORD_TYPE_DECLARATION but the %static ones (its
 * constructor's, 5.20.2). Each has a DEFAULT child when it has a default.
 * 0 after the last.
 */
sxml_ast_node_index sxml_xquery_first_parameter(const sxml_ast *ast,
                                                sxml_ast_node_index declaration);
sxml_ast_node_index sxml_xquery_next_parameter(const sxml_ast *ast,
                                               sxml_ast_node_index parameter);

/* A parameter's name node: a Name, or for a field an NCNAME or
   STRING_LITERAL. */
sxml_ast_node_index sxml_xquery_parameter_name(const sxml_ast *ast,
                                               sxml_ast_node_index parameter);

/* A parameter's SEQUENCE_TYPE node; 0 when it has no declared type. */
sxml_ast_node_index sxml_xquery_parameter_type(const sxml_ast *ast,
                                               sxml_ast_node_index parameter);

/* The name of an EXTENDED_FIELD_DECLARATION, borrowed from the AST. */
sxml_byte_view sxml_xquery_field_name(const sxml_ast *ast, sxml_ast_node_index field);

/* The declared function of a FUNCTION_DECLARATION node; NULL for none. */
const sxml_xquery_declared_function *sxml_xquery_find_declared_function(
    const sxml_xquery_static_context *context, sxml_ast_node_index declaration);
