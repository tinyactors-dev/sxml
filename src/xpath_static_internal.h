#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "ir_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "xpath_ast_internal.h"

/*
 * Static analysis and lowering of XPath 4.0 (docs/DECISIONS.md, D46, D55,
 * D63). Internal to sxml.
 *
 *     text -> AST (src/xpath_parse.c) -> static analysis and lowering
 *     (src/xpath_static.c, src/xpath_lower.c) -> passes (src/ir.c) -> seal
 *
 * Static analysis resolves what the AST keeps as written (D52) against the
 * static context (XPath 4.0, section 2.2.1): namespace prefixes
 * (XPST0081), variable names to slots or global variables (XPST0008),
 * function names and arities through a function table (XPST0017), type
 * names (XPST0051) and the item types of sequence types, which it
 * registers. Lowering writes the IR of src/ir_internal.h; the IR there
 * documents how each construct lowers.
 *
 * src/xpath_static.c holds the static context, names and types (also used
 * by the function library to resolve the type text of its rows);
 * src/xpath_lower.c holds lowering and compilation.
 *
 * Errors are static errors (SXML_STATUS_STATIC_ERROR) in the request's
 * error record, with the code, a static description and the location of
 * the AST node in the expression text. A failure of sxml itself or of a
 * host object is an interface status with nothing in the record, except
 * SXML_STATUS_UNSUPPORTED_FEATURE for a construct not lowered yet, whose
 * record's diagnostic names it.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* A named item type of the static context (XQuery 4.0 "declare type",
   XSLT 4.0 xsl:item-type): an alias of a registered item type (D98,
   D135). */
typedef struct sxml_xpath_named_item_type {
    sxml_name_identifier type_name;
    sxml_type_identifier item_type;
} sxml_xpath_named_item_type;

typedef struct sxml_xpath_named_item_type_view {
    const sxml_xpath_named_item_type *named_item_types;
    size_t named_item_type_count;
} sxml_xpath_named_item_type_view;

/* What an unprefixed function name expands to (XPath 4.0, section 2.2.1
   "default function namespace"; XQuery 4.0, 5.14.2). */
typedef enum sxml_xpath_function_namespace_rule {
    /* The fn namespace (XPath). */
    SXML_XPATH_FUNCTION_NAMESPACE_RULE_STANDARD = 0,
    /* default_function_namespace (no namespace for SXML_NAMESPACE_NONE):
       an XQuery default function namespace declaration. */
    SXML_XPATH_FUNCTION_NAMESPACE_RULE_DECLARED,
    /* No namespace: XQuery without a declaration, whose function table
       then searches no-namespace functions before fn (D135). */
    SXML_XPATH_FUNCTION_NAMESPACE_RULE_NO_NAMESPACE_FIRST,
} sxml_xpath_function_namespace_rule;

/*
 * The static context of one compilation. A plain value; every pointer and
 * view is borrowed for the calls that receive it.
 */
typedef struct sxml_xpath_static_context {
    /* Its name pool interns names; its type registry registers item
       types; constants are made in it. */
    sxml_item_heap *item_heap;
    /* The processor of item_heap, whose resource resolver reads the
       schema documents a prolog imports (XQuery 4.0, 5.11; D174); NULL
       where nothing is imported (unit tests). */
    sxml_processor *processor;
    /* Memory for units and for scratch arrays. Not the null allocator. */
    sxml_allocator allocator;
    /* Added to the predeclared prefixes xml, xs, xsi, fn, map, array,
       math and err; prefix 0 sets the default namespace for elements and
       types (sxml_static_context_configuration). */
    sxml_namespace_binding_view namespace_bindings;
    /* The external variables in scope; variable n (from 1) becomes global
       variable n of the unit, in this order, unless global_numbers maps
       it. */
    sxml_variable_declaration_view variable_declarations;
    /* NULL, or one global number per variable declaration: the global
       variable each one reads (XQuery modules, whose variables in scope
       are not the unit's first globals; D148). */
    const uint32_t *global_numbers;
    /* The statically known function definitions. A table whose operations
       is NULL knows none. */
    sxml_function_table function_table;
    /* The static base URI (XPath 4.0, section 2.2.1), an absolute URI, or
       empty when absent; copied into the unit by lowering (D91). */
    sxml_byte_view static_base_uri;
    /* The decimal formats and collations, already checked; copied into
       the unit by lowering (D131). Zero for none. */
    sxml_ir_static_context unit_static_context;
    /* Named item types, found by name before the type registry's named
       types (XPath 4.0, section 3.2.3); empty for none. */
    sxml_xpath_named_item_type_view named_item_types;
    /* How unprefixed function names expand; 0 is the fn namespace. */
    sxml_xpath_function_namespace_rule function_namespace_rule;
    /* For SXML_XPATH_FUNCTION_NAMESPACE_RULE_DECLARED. */
    sxml_namespace_uri_identifier default_function_namespace;
    /* The default namespace for elements and types is "##any" (XQuery
       4.0, 5.14.1; XPath 4.0, 2.2.1): an unprefixed element name in a
       name test or element test matches the local name in any namespace,
       an unprefixed type name is in the xs namespace, and prefix 0 is
       bound to no namespace (for constructed elements). */
    bool has_any_default_element_namespace;
    /* The unit module (glossary ir_module) whose static context the
       function calls lowered in this context read at run time: an XSLT
       element's (D163); 0 for the calling function's. */
    uint32_t ir_module_number;
} sxml_xpath_static_context;

/* Writes a static error: the error code is err:code_local_name. */
typedef struct sxml_xpath_static_error {
    const sxml_ast *ast;
    /* Where the error is; 0 for no location. */
    sxml_ast_node_index node;
    /* ASCII, such as "XPST0008"; static. */
    sxml_byte_view code_local_name;
    /* Static text. */
    sxml_byte_view description;
} sxml_xpath_static_error;

/*
 * Fills error_record (which may be NULL) with error and returns
 * SXML_STATUS_STATIC_ERROR.
 */
SXML_NODISCARD sxml_status sxml_xpath_report_static_error(
    const sxml_xpath_static_error *error, sxml_error_record *error_record);

/* Which namespace an unprefixed name is in (XPath 4.0, section 2.2.1). */
typedef enum sxml_xpath_name_role {
    /* Element names in name tests and types: the default namespace for
       elements and types. */
    SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE,
    /* Function names: by the static context's function namespace rule. */
    SXML_XPATH_NAME_ROLE_FUNCTION,
    /* Variables, attributes, QName literals, processing-instruction
       targets: no namespace. */
    SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
} sxml_xpath_name_role;

/* A name of the AST to resolve. */
typedef struct sxml_xpath_name_request {
    /* Its root's namespace declarations come first (XPath 4.0, A.1). */
    const sxml_ast *ast;
    /* An NCNAME, PREFIXED_NAME or URI_QUALIFIED_NAME node, or a WILDCARD
       with a prefix or a braced URI. */
    sxml_ast_node_index name_node;
    sxml_xpath_name_role name_role;
    const sxml_xpath_static_context *static_context;
    sxml_error_record *error_record;
} sxml_xpath_name_request;

/* A resolved name: an interned namespace URI and the local name, borrowed
   from the AST (empty for a wildcard). */
typedef struct sxml_xpath_resolved_name {
    sxml_namespace_uri_identifier namespace_uri;
    sxml_byte_view local_name;
    /* An unprefixed element or type name under the default namespace
       "##any" (has_any_default_element_namespace; namespace_uri is no
       namespace): a name test or element test matches any namespace, a
       type name is in the xs namespace. */
    bool is_any_namespace;
} sxml_xpath_resolved_name;

/*
 * Resolves request->name_node: the prefix through the statically known
 * namespaces, a braced URI with its whitespace normalized, an unprefixed
 * name by its role. Interns the namespace URI. Returns
 * SXML_STATUS_STATIC_ERROR (XPST0081 for an unknown prefix), and the name
 * pool's and allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_xpath_resolve_name(
    const sxml_xpath_name_request *request,
    sxml_xpath_resolved_name *resolved_name);

/*
 * The statically known namespaces beyond the predeclared prefixes, for
 * what reads them at run time: casts to xs:QName and xs:QName#1 (F&O 4.0,
 * section 19.1.9). bindings is allocated with the static context's
 * allocator, which releases it; NULL when binding_count is 0.
 */
typedef struct sxml_xpath_namespace_collection {
    sxml_namespace_binding *bindings;
    size_t binding_count;
} sxml_xpath_namespace_collection;

/*
 * Collects the namespace declarations of ast's root (a prefix bound to no
 * namespace undeclares it; the default element namespace declaration is
 * prefix 0), then the static context's bindings for prefixes the
 * expression does not declare, into *collection. Returns the name pool's
 * and allocator's failures; *collection is then untouched.
 */
SXML_NODISCARD sxml_status sxml_xpath_collect_namespaces(
    const sxml_ast *ast, const sxml_xpath_static_context *static_context,
    sxml_xpath_namespace_collection *collection);

/* A sequence type or item type of the AST to resolve. */
typedef struct sxml_xpath_type_request {
    const sxml_ast *ast;
    /* A SEQUENCE_TYPE node for a sequence type; any item type kind or
       name for an item type. */
    sxml_ast_node_index type_node;
    const sxml_xpath_static_context *static_context;
    sxml_error_record *error_record;
} sxml_xpath_type_request;

/*
 * Resolves a sequence type (XPath 4.0, section 3.1), registering its item
 * type, and writes it to *sequence_type. Returns SXML_STATUS_STATIC_ERROR
 * (XPST0081, XPST0051 for a name that is not an item type, XPST0008 for an
 * unknown type or schema declaration in a node test, XPST0152 for a map
 * key type that is not atomic, XPST0021 for duplicate record fields),
 * SXML_STATUS_LIMIT_EXCEEDED for types nested deeper than
 * SXML_XPATH_MAXIMUM_TYPE_DEPTH, SXML_STATUS_UNSUPPORTED_FEATURE, and the
 * type registry's, name pool's and allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_xpath_resolve_sequence_type(
    const sxml_xpath_type_request *request, sxml_sequence_type *sequence_type);

/* As sxml_xpath_resolve_sequence_type for an item type. */
SXML_NODISCARD sxml_status sxml_xpath_resolve_item_type(
    const sxml_xpath_type_request *request, sxml_type_identifier *item_type);

/* The type a name names in static_context: one of its named item types,
   else a named schema or record type of its type registry. Returns false,
   leaving *type untouched, for none. */
bool sxml_xpath_find_named_type(const sxml_xpath_static_context *static_context,
                                sxml_name_identifier type_name, sxml_type_identifier *type);

/* The global number of variable declaration declaration_index of
   static_context: global_numbers' entry, else declaration_index + 1. */
uint32_t sxml_xpath_global_number(const sxml_xpath_static_context *static_context,
                                  size_t declaration_index);

/* Item types nest at most this deep (map(K, map(K, ...))). */
#define SXML_XPATH_MAXIMUM_TYPE_DEPTH 64U

/*
 * Finds a function through table: what the function table interface does
 * (sxml_function_table_find), with a table whose operations is NULL
 * finding nothing.
 */
SXML_NODISCARD sxml_status sxml_xpath_find_function(
    sxml_function_table table, const sxml_function_query *query,
    sxml_function_entry *entry);

/* ------------------------------------------------------------ Lowering */

/*
 * The deepest nesting lowering accepts when the request gives 0. Lowering
 * keeps one frame of a few hundred bytes of allocator memory per level of
 * nesting, never C stack (D71), so this bounds heap use: about a few MB at
 * the default, far deeper than expressions people write.
 */
#define SXML_XPATH_DEFAULT_MAXIMUM_EXPRESSION_DEPTH 10000U

typedef struct sxml_xpath_lowering_request {
    /* An AST whose root is an XPATH node. */
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
    /* sxml_ir_unit_configuration.maximum_expression_count. */
    uint32_t maximum_expression_count;
    /* Deepest nesting lowering accepts; 0 means
       SXML_XPATH_DEFAULT_MAXIMUM_EXPRESSION_DEPTH. Beyond it lowering
       stops with SXML_STATUS_LIMIT_EXCEEDED and the record's diagnostic
       filled. */
    uint32_t maximum_expression_depth;
    sxml_error_record *error_record;
} sxml_xpath_lowering_request;

/*
 * Lowers request->ast into a new unit, created through the static
 * context's allocator, and writes it, owned (one reference), to *unit. The
 * unit is not sealed and no pass has run. Its global variables are the
 * static context's variable declarations, in order; its source name is the
 * AST's.
 *
 * Returns SXML_STATUS_STATIC_ERROR with the error record filled,
 * SXML_STATUS_LIMIT_EXCEEDED, SXML_STATUS_UNSUPPORTED_FEATURE for a
 * construct not lowered yet (computed node constructors, Phase 3) or one
 * lowered to a function the function table lacks (D113),
 * SXML_STATUS_INVALID_ARGUMENT for an invalid static
 * context (duplicate variable names, a prefix bound twice, the xml
 * prefix rebound), and the failures of the function table, type registry,
 * name pool, item heap and allocator. On failure nothing is left
 * allocated, except item types the type registry interned.
 */
SXML_NODISCARD sxml_status sxml_xpath_lower(
    const sxml_xpath_lowering_request *request, sxml_ir_unit **unit);

/* An expression to compile, as the public sxml_xpath_expression does. */
typedef struct sxml_xpath_compile_request {
    /* UTF-8; borrowed for the call. */
    sxml_byte_view expression_text;
    /* Copied into the unit; may be empty. */
    sxml_byte_view source_name;
    const sxml_xpath_static_context *static_context;
    uint32_t maximum_expression_count;
    /* For the front end and lowering; 0 means
       SXML_XPATH_DEFAULT_MAXIMUM_EXPRESSION_DEPTH. */
    uint32_t maximum_expression_depth;
    sxml_error_record *error_record;
} sxml_xpath_compile_request;

/*
 * Parses, lowers, runs sxml_ir_standard_passes and seals: writes a sealed
 * unit, owned, to *unit. Returns the failures of sxml_xpath_build_ast
 * (XPST0003), sxml_xpath_lower and sxml_ir_run_passes (an
 * SXML_STATUS_INVALID_ARGUMENT from the verifier is an error in sxml; the
 * record's diagnostic then describes it).
 */
SXML_NODISCARD sxml_status sxml_xpath_compile(
    const sxml_xpath_compile_request *request, sxml_ir_unit **unit);
