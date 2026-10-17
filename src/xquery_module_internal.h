#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "xpath_ast_internal.h"

/*
 * The module set of one XQuery compilation (docs/DECISIONS.md, D106,
 * D148; packet 3.11). Internal to sxml.
 *
 * sxml_xquery_load_modules starts from the main module's AST and loads
 * every library module its module imports reach, each once (XQuery 4.0,
 * 5.12):
 *
 * - every module import asks the resource resolver, kind XQUERY_MODULE,
 *   once per location hint (the hint as uri_reference, resolved against
 *   the importing module's static base URI as absolute_uri), or once with
 *   an empty uri_reference when it has none and no module of its
 *   namespace is loaded yet; the module's namespace is the request's
 *   target_namespace_uri (include/sxml/xquery.h);
 * - a module is identified by its module URI (glossary): the resource URI
 *   the resolver gives, else the absolute hint, else the hint, else (no
 *   hint) the target namespace; a module URI met again is the module
 *   loaded before, so import cycles end (5.12.3, 5.12.4);
 * - the text (UTF-8, a byte order mark skipped) is parsed with the module
 *   URI as source name and must be a library module of the imported
 *   namespace.
 *
 * Imports name namespaces: a module imports every module of the set whose
 * target namespace it imports, itself excepted (5.12.2), so the set
 * records the imported namespaces of each module and, once loaded, the
 * imported modules. The reading order puts imported modules before their
 * importers (an iterative depth-first walk from the main module, D71); a
 * cycle is broken where the walk meets a module on its path.
 *
 * Errors (static, in the error record): XQST0088 for an empty target
 * namespace, XQST0047 for two imports of one namespace in a prolog,
 * XQST0059 for a hint the resolver has no module for, a module that is
 * not a library module of the namespace, and an import that loads none;
 * the front end's errors (XPST0003) of a library module, located in it.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* One module of a module set. */
typedef struct sxml_xquery_module {
    /* The main module's AST is the caller's; a library module's is
       owned_ast. */
    const sxml_ast *ast;
    sxml_ast *owned_ast;
    /* A library module's target namespace; 0 for the main module. */
    sxml_namespace_uri_identifier target_namespace;
    /* A library module's module URI, interned in the name pool (so it
       outlives the set as an error record's source name): its source name
       and static base URI; empty for the main module. */
    sxml_byte_view module_uri;
    /* The namespaces it imports: the set's imported_namespaces from
       first_import on. */
    size_t first_import;
    size_t import_count;
    /* The modules it imports: the set's imported_modules from
       first_imported_module on. */
    size_t first_imported_module;
    size_t imported_module_count;
} sxml_xquery_module;

/* The modules of one compilation; module 0 is the main module. Arrays
   are allocated with allocator. */
typedef struct sxml_xquery_module_set {
    sxml_allocator allocator;
    sxml_xquery_module *modules;
    size_t module_count;
    size_t module_capacity;
    sxml_namespace_uri_identifier *imported_namespaces;
    size_t imported_namespace_count;
    size_t imported_namespace_capacity;
    /* Module indices. */
    size_t *imported_modules;
    size_t imported_module_count;
    /* module_count module indices, imported modules first. */
    size_t *reading_order;
} sxml_xquery_module_set;

/* What loading needs. Everything is borrowed for the call. */
typedef struct sxml_xquery_module_loading {
    /* A MODULE whose module is a MAIN_MODULE; it stays the caller's and
       must outlive the set. */
    const sxml_ast *main_ast;
    /* Interns the target namespaces. */
    sxml_item_heap *item_heap;
    /* The set's memory and its library modules' ASTs. */
    sxml_allocator allocator;
    /* Asked for every library module; the null resolver has none. */
    sxml_resource_resolver resource_resolver;
    /* The request's processor (for a resolver that wants it). */
    sxml_processor *processor;
    /* The main module's static base URI before its base URI declaration;
       may be empty. */
    sxml_byte_view static_base_uri;
    /* For the front end; 0 means no bound. */
    uint32_t maximum_expression_depth;
    sxml_error_record *error_record;
} sxml_xquery_module_loading;

/*
 * Loads the module set of request->main_ast into *set (see the file
 * comment). Returns SXML_STATUS_STATIC_ERROR with the error record filled,
 * SXML_STATUS_INVALID_ARGUMENT for a request without an AST or for a
 * module the resolver delivered in a form other than BYTES, the
 * resolver's failures, and the front end's, name pool's and allocator's;
 * on failure nothing is left allocated and *set is untouched.
 */
SXML_NODISCARD sxml_status sxml_xquery_load_modules(const sxml_xquery_module_loading *request,
                                                    sxml_xquery_module_set *set);

/* Releases the library modules' ASTs and the set's arrays. NULL is
   allowed. */
void sxml_xquery_release_modules(sxml_xquery_module_set *set);
