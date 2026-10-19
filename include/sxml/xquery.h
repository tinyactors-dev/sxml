#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

/*
 * XQuery 4.0: compiling a query and evaluating it (docs/DECISIONS.md, D47,
 * D51, D106).
 *
 * A query is the text of a main module (XQuery 4.0, section 5 "Modules and
 * Prologs"). It is compiled once, with the library modules it imports,
 * into an sxml_xquery_query and evaluated any number of times, each time
 * against a dynamic context, giving a sequence in the processor's item
 * heap. Its prolog may declare serialization parameters, which the host
 * reads to serialize the result as the query asks:
 *
 *     sxml_xquery_query *query = NULL;
 *     status = sxml_xquery_query_create(
 *         allocator,
 *         &(sxml_xquery_query_configuration){
 *             .processor = processor,
 *             .expression_text = query_text,
 *             .source_name = SXML_BYTE_VIEW_FROM_LITERAL("query.xq"),
 *             .static_context = {.static_base_uri = base_uri},
 *             .error_record = &error_record,
 *         },
 *         &query);
 *     ...
 *     status = sxml_xquery_query_evaluate(
 *         query,
 *         &(sxml_xquery_evaluation_request){
 *             .dynamic_context = {.variable_bindings = bindings},
 *             .error_record = &error_record,
 *         },
 *         &result);
 *     ...
 *     status = sxml_serialize_sequence(
 *         sxml_processor_get_item_heap(processor),
 *         &(sxml_serialization_request){
 *             .value = result,
 *             .serialization_parameters =
 *                 sxml_xquery_query_get_serialization_parameters(query),
 *             .error_record = &error_record,
 *         },
 *         output_stream);
 *
 * The static and dynamic contexts are those of XPath
 * (include/sxml/xpath.h); what the prolog declares (namespaces, default
 * namespaces, the base URI, collations, decimal formats, the
 * boundary-space, construction, ordering, empty order and copy-namespaces
 * policies, options) starts from them or from XQuery 4.0's defaults and
 * holds for the module that declares it.
 *
 * Library modules: every "import module" asks the processor's resource
 * resolver for each location hint, with kind
 * SXML_RESOURCE_KIND_XQUERY_MODULE, the hint as uri_reference resolved
 * against the importing module's static base URI, and the module's
 * namespace as target_namespace_uri; an import without hints asks once
 * with an empty uri_reference. A module that no hint delivers, or whose
 * namespace is not the one imported, is XQST0059. A module imported twice
 * is compiled once; cyclic imports are allowed (XQuery 4.0, "Module
 * Import"). Compilation reads nothing else, except the parameter document
 * of an output:parameter-document declaration, asked for with kind
 * SXML_RESOURCE_KIND_XML_DOCUMENT (XQST0119 when it is not available).
 *
 * Errors: as for XPath, static errors (XPST*, XQST*), type errors and
 * dynamic errors (XPDY*, XQDY*, FO*) are reported through the caller's
 * sxml_error_record with code, description and location; a location in a
 * library module names the module's resolved URI as its source.
 *
 * Thread safety: the thread of the processor (D35).
 */

/* ----------------------------------------------------------------- Query */

/*
 * An XQuery main module compiled for one processor, with the library
 * modules it imports. Its fixed part lives in the memory given to
 * initialize (or allocated by create); its compiled form is allocated
 * from the processor's allocator and, as for an XPath expression, lives
 * until the query is finalized and every function item an evaluation
 * returned is released, whichever comes last (D38).
 */
typedef struct sxml_xquery_query sxml_xquery_query;

/* A query to compile. */
typedef struct sxml_xquery_query_configuration {
    /* Kept by reference; must outlive the query. */
    sxml_processor *processor;
    /* The text of the main module, UTF-8 (a library module, one that
       starts with a module declaration, is XPST0003 here). Borrowed for
       the call. */
    sxml_byte_view expression_text;
    /* The name to report in error locations of the main module; may be
       empty. Copied. */
    sxml_byte_view source_name;
    /*
     * The static context the main module starts from, with the rules of
     * include/sxml/xpath.h and these for XQuery:
     * - namespace_bindings are added to XQuery 4.0's predeclared prefixes
     *   (section 2.1.4: those of XPath and local, output, xq); a prolog
     *   namespace declaration may rebind them, except xml and xmlns
     *   (XQST0070); a prolog default element namespace declaration
     *   replaces a binding with prefix 0;
     * - static_base_uri is the base URI before any base-uri declaration,
     *   which is resolved against it; location hints of the main module
     *   resolve against the result;
     * - variable_declarations are in scope in the main module only; a
     *   prolog variable declaration of the same name is XQST0049;
     * - decimal_formats hold until a prolog decimal-format declaration of
     *   the same name replaces one (XQuery checks its own declarations:
     *   XQST0097, XQST0098, XQST0111);
     * - default_collation_uri holds until a default collation declaration
     *   replaces it (XQST0038 for one not among the statically known
     *   collations).
     * Library modules start from XQuery 4.0's default static context with
     * the same collation bindings.
     */
    sxml_static_context_configuration static_context;
    /* Receives a static or type error found by compilation, or the
       location of a limit reached; may be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
} sxml_xquery_query_configuration;

/*
 * Writes the memory requirements of a query's fixed part, which do not
 * depend on the query text, to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (no
 * processor).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xquery_query_memory_requirements(
    const sxml_xquery_query_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Compiles configuration->expression_text, and every library module it
 * imports, into a query placed in memory and writes its address to
 * *query. memory must stay valid, unmoved, until
 * sxml_xquery_query_finalize. Compilation keeps nothing of the texts.
 *
 * Output declarations (XQuery 4.0, section 5.22) are checked here:
 * XQST0108 in a library module, XQST0109 for an unknown parameter name or
 * use-character-maps, XQST0110 for a parameter declared twice, SEPM0016
 * for a value the parameter does not permit (reported as a static error),
 * XQST0119 for a parameter document that cannot be read and XQST0115 for
 * one that raises a serialization error.
 *
 * Returns:
 * - SXML_STATUS_STATIC_ERROR or SXML_STATUS_TYPE_ERROR, with the error
 *   record filled (code, description, location);
 * - SXML_STATUS_LIMIT_EXCEEDED at maximum_expression_depth or another
 *   limit of the processor, with only the record's diagnostic filled;
 * - SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (as for
 *   sxml_xpath_expression_initialize), and for a module the resolver
 *   delivered in a form its kind does not allow;
 * - SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 *   memory does not meet the memory requirements, and the allocator's
 *   and the resolver's failures.
 * On failure *query is untouched and nothing is left allocated, except
 * item types the type registry interned, which stay valid.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xquery_query_initialize(
    sxml_memory_view memory,
    const sxml_xquery_query_configuration *configuration,
    sxml_xquery_query **query);

/*
 * Drops the query's reference to its compiled form. Does nothing when
 * query is NULL.
 */
SXML_EXPORT void sxml_xquery_query_finalize(sxml_xquery_query *query);

/*
 * Allocates a query through allocator and compiles it as
 * sxml_xquery_query_initialize does. allocator is stored and must outlive
 * the query. Returns the failures of sxml_xquery_query_initialize and
 * sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xquery_query_create(
    sxml_allocator allocator,
    const sxml_xquery_query_configuration *configuration,
    sxml_xquery_query **query);

/*
 * Finalizes a query made by sxml_xquery_query_create and releases its
 * memory. Does nothing when query is NULL.
 */
SXML_EXPORT void sxml_xquery_query_destroy(sxml_xquery_query *query);

/* --------------------------------------------------------------- Updates */

/*
 * The XQuery Update Facility 3.0 (docs/DECISIONS.md, D187). A query whose
 * body is an updating expression (XQUF 3.0, section 2.2) is an updating
 * query: evaluating it computes a pending update list over a snapshot of
 * the documents and nodes it reads, then applies it at once
 * (upd:applyUpdates). Trees never change (include/sxml/tree.h): every
 * tree the list changes is rebuilt as a new tree, which the evaluation
 * hands to the request's update sink. How a change is made persistent is
 * the host's (XQUF 3.0, 3.2.2: implementation-defined): a host that bound
 * a document puts the new tree in its place, writes it out, or drops it.
 */

/* Whether query's body is an updating expression. False for NULL. */
SXML_EXPORT bool sxml_xquery_query_is_updating(const sxml_xquery_query *query);

/* One tree an updating query changed. */
typedef struct sxml_xquery_updated_tree {
    /* The root of the tree as the query read it: a document node, or the
       root of a parentless tree (a constructed node, a copy). Borrowed
       for the call. */
    sxml_item original_root;
    /* The root of the tree with every change made, a node of a new tree.
       Borrowed for the call: retain it (sxml_item_retain) to keep it. */
    sxml_item updated_root;
} sxml_xquery_updated_tree;

/*
 * One document fn:put stores (XQUF 3.0, 6.1 fn:put and 3.2.2 upd:put;
 * D205). The library never writes it anywhere: storing it at uri, by
 * serializing it with the parameters or by keeping it for a later fn:doc,
 * is the host's (upd:put: the external effects are implementation-defined).
 */
typedef struct sxml_xquery_put_document {
    /* fn:put's $uri resolved against the static base URI: an absolute
       URI. Borrowed for the call. */
    sxml_byte_view uri;
    /* The document or element node fn:put was given, as the other updates
       of the query left it. Borrowed for the call: retain it
       (sxml_item_retain) to keep it. */
    sxml_item node;
    /* fn:put's $params (an output:serialization-parameters element or a
       map), or the empty sequence. Borrowed for the call. */
    sxml_sequence serialization_parameters;
} sxml_xquery_put_document;

/* Receives the trees an updating query changed. Tables are static
   const. */
typedef struct sxml_xquery_update_sink_operations {
    /* Called once per changed tree, in the order of the tree's first
       update. A failure ends the evaluation with that status (the trees
       not yet received are dropped). */
    sxml_status (*receive_updated_tree)(void *state,
                                        const sxml_xquery_updated_tree *updated_tree);
    /* Called once per fn:put of the query, after every changed tree
       (upd:applyUpdates applies upd:put last), with distinct URIs
       (XUDY0031 otherwise). A failure ends the evaluation with that
       status. NULL, or a sink without operations, makes a query that
       calls fn:put fail with FOUP0002 before any tree is received: no
       location can be written. */
    sxml_status (*receive_put_document)(void *state,
                                        const sxml_xquery_put_document *put_document);
} sxml_xquery_update_sink_operations;

/* An update sink, passed by value. Operations NULL drops every changed
   tree: the query's updates are then not observable. */
typedef struct sxml_xquery_update_sink {
    void *state;
    const sxml_xquery_update_sink_operations *operations;
} sxml_xquery_update_sink;

/* ------------------------------------------------------------ Evaluation */

/* One evaluation of a query. */
typedef struct sxml_xquery_evaluation_request {
    /*
     * The dynamic context, with the rules of include/sxml/xpath.h and
     * these for XQuery:
     * - variable_bindings bind the host's variable declarations and the
     *   external variables the prolog of any module of the query declares
     *   (XQuery 4.0, "Variable Declaration"); an external variable without
     *   a binding takes its default value, or raises XPDY0002 when it has
     *   none and is evaluated;
     * - the context value is the initial context value when the main
     *   module declares none or declares it external (coerced to the
     *   declared type, XPTY0004; XPDY0002 when absent and the declaration
     *   has no default); a context value declaration that is not external
     *   gives the context value itself (XQuery 4.0, "Context Value
     *   Declaration").
     */
    sxml_dynamic_context_configuration dynamic_context;
    /* Receives a dynamic or type error, or the location of a limit
       reached; may be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
    /* For an updating query: receives each tree it changed. Borrowed for
       the call; zero drops them. */
    sxml_xquery_update_sink update_sink;
} sxml_xquery_evaluation_request;

/*
 * Evaluates query against request->dynamic_context and writes the result,
 * owned, to *result (release it with sxml_sequence_release before the
 * processor is finalized). As for sxml_xpath_expression_evaluate, the
 * result is complete, everything else the evaluation made has been
 * released when this returns (apart from documents and function items the
 * result refers to), and evaluations may nest. Global variables are
 * evaluated at most once per evaluation, when first read (XQDY0054 for a
 * circular initialization).
 *
 * An updating query's value is its pending update list: the evaluation
 * applies it (XQUF 3.0, 3.2.2 upd:applyUpdates), hands each changed tree
 * to request->update_sink, and writes the empty sequence to *result
 * (XQUF 3.0, 2.2.3).
 *
 * Returns the statuses of sxml_xpath_expression_evaluate, with the
 * dynamic and type errors of XQuery 4.0 (XQDY*, XQTY*) and of the XQuery
 * Update Facility (XUDY*, XUTY*) besides those of XPath and the function
 * library, and the update sink's failures. On failure *result is
 * untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xquery_query_evaluate(
    const sxml_xquery_query *query,
    const sxml_xquery_evaluation_request *request, sxml_sequence *result);

/* --------------------------------------------- Serialization parameters */

/*
 * The serialization parameters the main module declares (XQuery 4.0,
 * section 5.22 "Output Declarations"): each output declaration's value;
 * for a parameter it does not declare, the value of the parameter document
 * of output:parameter-document, if any; for the rest, XQuery's defaults
 * (5.22.1): method xml and every other field zero, its default in
 * include/sxml/serialize.h. Names in cdata-section-elements and
 * suppress-indentation are expanded with the default element namespace
 * rule. Declarations outside the serialization namespace are ignored.
 *
 * Declaring parameters does not serialize: the host decides whether to,
 * and may override any of them (the CLI's --method) by changing fields of
 * the returned copy. Its views are borrowed from the query: valid until
 * the query is finalized.
 */
SXML_EXPORT sxml_serialization_parameters
sxml_xquery_query_get_serialization_parameters(const sxml_xquery_query *query);
