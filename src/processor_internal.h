#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "evaluator_internal.h"
#include "function_library_internal.h"
#include "optional_feature_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "xdm_internal.h"

/*
 * The processor's internals (include/sxml/processor.h; docs/DECISIONS.md,
 * D30, D51, D88), shared by src/processor.c and src/xpath.c. Internal to
 * sxml.
 *
 * The processor owns, in the memory it was given, its name pool, type
 * registry, item heap and the function library over every part of
 * src/function_library_*.c (D55, D78), so a compiled expression and the
 * evaluations of it find them here.
 *
 * Every evaluation brings its own document loader (src/evaluator_internal.h,
 * "Document loader"): an sxml_processor_document_loader, made from the
 * dynamic context configuration, that serves fn:doc from the available
 * documents, then from the resource resolver, and keeps what it loaded
 * until the evaluation ends, so fn:doc is stable (F&O 4.0, "fn:doc").
 * The loader also carries the evaluation environment (D131), which the
 * library functions of src/function_library_resource.c and the trace
 * functions reach from their dynamic context through
 * sxml_processor_get_evaluation_environment.
 *
 * Thread safety: the thread of the processor (D35).
 */

/*
 * External entities asked of a resource resolver for the XML parser
 * (include/sxml/xml.h; D199): the resource being read is kept until the
 * parser releases it. With refuses set nothing is asked and was_refused
 * records that a document referred to an external entity (fn:doc and
 * fn:parse-xml without trust-external: FODC0016).
 */
typedef struct sxml_entity_resolution {
    sxml_processor *processor;
    sxml_resource_resolver resolver;
    bool refuses;
    bool was_refused;
    sxml_resource resource;
    bool holds_resource;
} sxml_entity_resolution;

/* The entity resolver over *resolution. */
sxml_xml_entity_resolver sxml_entity_resolution_as_resolver(sxml_entity_resolution *resolution);

struct sxml_processor {
    /* The configuration's allocator: every object and value of the
       processor is made through it. */
    sxml_allocator allocator;
    /* The allocator of sxml_processor_create, which destroy frees the
       processor with; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    sxml_processor_limits limits;
    sxml_resource_resolver resource_resolver;
    /* Never the null clock: a null configuration clock becomes
       sxml_system_clock(). */
    sxml_clock clock;
    /* May be the null sink, which discards. */
    sxml_trace_sink trace_sink;
    /* The state of sxml_processor_entity_resolver (D199). */
    sxml_entity_resolution entity_resolution;
    /* In the processor's memory. */
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    sxml_item_heap *item_heap;
    sxml_function_library *function_library;
    /* The schemas compiled for the processor's own use (XQuery's import
       schema, D174), destroyed when it is finalized; allocator memory,
       NULL when there are none. */
    sxml_schema **kept_schemas;
    size_t kept_schema_count;
    size_t kept_schema_capacity;
    /* The schema of the built-in components alone, made on first use by
       sxml_processor_get_builtin_schema and kept; NULL until then. */
    sxml_schema *builtin_schema;
    /* The schema of the fn: namespace (F&O 4.0, appendix D), made on
       first use by sxml_processor_get_function_namespace_schema and kept;
       NULL until then. */
    sxml_schema *function_namespace_schema;
    /* The optional features the configuration named, by kind, with the
       state each keeps here (src/optional_feature_internal.h, D189). */
    sxml_optional_feature_slots optional_features;
    /* The configuration's file system, for the EXPath File feature
       (D200); the null file system when none. */
    sxml_file_system file_system;
};

/*
 * The processor takes schema, which it destroys when it is finalized
 * (D174): the schema an XQuery prolog imports, which the compiled unit's
 * static contexts name and its validate expressions use, and which must
 * live as long as the unit and the trees annotated with its types. The
 * processor, not the unit, owns it, as it owns the registered types: the
 * schema's own XPath expressions are units, so a unit owning schemas
 * would make finalization a cycle (D71). On failure the caller still owns
 * schema.
 */
SXML_NODISCARD sxml_status sxml_processor_keep_schema(sxml_processor *processor,
                                                      sxml_schema *schema);

/*
 * The schema of the built-in components alone (XSD 1.1 Part 1, 3.4.7,
 * 3.16.7), made once per processor and kept (D174): the in-scope schema
 * of an XQuery main module that has no other, so lax validation and
 * xsi:type always have a schema. Returns the compilation's failures.
 */
SXML_NODISCARD sxml_status sxml_processor_get_builtin_schema(sxml_processor *processor,
                                                             const sxml_schema **schema);

/*
 * The schema of the fn: namespace (F&O 4.0, appendix D: the results of
 * fn:analyze-string, fn:json-to-xml and fn:csv-to-xml), made once per
 * processor from the documents every processor knows
 * (sxml_schema_function_namespace_document) and kept: what
 * fn:json-to-xml's validate option and the typed results of
 * fn:analyze-string are validated against. Returns the compilation's
 * failures.
 */
SXML_NODISCARD sxml_status sxml_processor_get_function_namespace_schema(
    sxml_processor *processor, const sxml_schema **schema);

/* An instant as seconds and nanoseconds since 1970-01-01T00:00:00Z. */
typedef struct sxml_unix_time {
    int64_t seconds;
    uint32_t nanoseconds;
} sxml_unix_time;

/* The instant as an xs:dateTime in UTC (timezone Z), proleptic Gregorian:
   what the system clock reads and file:last-modified gives (D200). */
sxml_date_time sxml_date_time_from_unix_time(sxml_unix_time unix_time);

/* The function table of the processor's library, for compilation and for
   fn:function-lookup. */
sxml_function_table sxml_processor_get_function_table(
    const sxml_processor *processor);

/* The evaluation depth the processor's limits give, with 0 resolved to
   SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_DEPTH. */
uint32_t sxml_processor_get_maximum_evaluation_depth(
    const sxml_processor *processor);

/* The step bound the processor's limits give, with 0 resolved to
   SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_STEP_COUNT (D89). */
uint64_t sxml_processor_get_maximum_evaluation_step_count(
    const sxml_processor *processor);

/* The expression depth the processor's limits give, with 0 resolved to
   SXML_XPATH_DEFAULT_MAXIMUM_EXPRESSION_DEPTH (src/xpath_static_internal.h). */
uint32_t sxml_processor_get_maximum_expression_depth(
    const sxml_processor *processor);

/* ------------------------------------------------- Evaluation environment */

/*
 * What the library functions of one evaluation read from the host beyond
 * fn:doc (glossary `evaluation_environment`; D104, D131): the
 * resolver for text, binary and collection resources, the trace sink, and
 * the default language and environment variables of the dynamic context.
 * (The decimal formats and collations of the static context belong to
 * the IR unit, sxml_ir_static_context, and are read lexically.) Views are
 * borrowed from the evaluation request and outlive the evaluation's
 * loader.
 */
typedef struct sxml_evaluation_environment {
    sxml_processor *processor;
    /* The dynamic context's resolver, or the processor's when that is the
       null resolver. */
    sxml_resource_resolver resource_resolver;
    /* The dynamic context's sink, or the processor's when that is the null
       sink; the null sink discards. */
    sxml_trace_sink trace_sink;
    /* An xs:language; never empty ("en" when the host gives none). */
    sxml_byte_view default_language;
    /* Names distinct, UTF-8. */
    sxml_environment_variable_view environment_variables;
    /* The result writer of the XSLT transformation the evaluation runs
       (src/xslt_output_internal.h, packet 4.13, D156): xsl:result-document
       and fn:current-output-uri reach it here. NULL outside one. */
    struct sxml_xslt_result_writer *result_writer;
} sxml_evaluation_environment;

/* ----------------------------------------------------- Document loader */

/* One document fn:doc loaded in this evaluation, by the URI it was asked
   for (absolute when the static base URI allowed resolving it). */
typedef struct sxml_processor_loaded_document {
    /* Allocated from the processor's allocator. */
    uint8_t *uri_bytes;
    size_t uri_byte_count;
    /* Owned. */
    sxml_item document_node;
} sxml_processor_loaded_document;

/* One collection fn:collection or fn:uri-collection read in this
   evaluation, kept so the functions are deterministic (F&O 4.0,
   "fn:collection"). */
typedef struct sxml_processor_loaded_collection {
    /* COLLECTION or URI_COLLECTION. */
    sxml_resource_kind resource_kind;
    bool is_default_collection;
    /* The absolute URI; allocated from the processor's allocator, NULL for
       the default collection. */
    uint8_t *uri_bytes;
    size_t uri_byte_count;
    /* Owned. */
    sxml_sequence items;
} sxml_processor_loaded_collection;

/* A step every document the loader gets from the resolver goes through
   before it is kept: XSLT's whitespace stripping of source documents
   (XSLT 4.0, 4.2; src/xslt_whitespace_internal.h, D150). */
typedef struct sxml_document_preparation_operations {
    /* Writes the document to keep, owned, to *prepared; document_node
       stays the caller's. */
    sxml_status (*prepare_document)(void *state, sxml_item document_node, sxml_item *prepared);
} sxml_document_preparation_operations;

typedef struct sxml_document_preparation {
    void *state;
    /* NULL: documents are kept as delivered. */
    const sxml_document_preparation_operations *operations;
} sxml_document_preparation;

/* What one evaluation's document loader serves. Views are borrowed and must
   outlive the loader. */
typedef struct sxml_processor_document_loader_configuration {
    sxml_evaluation_environment evaluation_environment;
    sxml_available_document_view available_documents;
    /* Applied to each document fn:doc gets from the resolver (not to
       available documents, which the host prepares). */
    sxml_document_preparation document_preparation;
} sxml_processor_document_loader_configuration;

/* The loader of one evaluation. Its fields are private to
   src/processor.c. */
typedef struct sxml_processor_document_loader {
    sxml_processor_document_loader_configuration configuration;
    sxml_processor_loaded_document *loaded_documents;
    size_t loaded_document_count;
    size_t loaded_document_capacity;
    sxml_processor_loaded_collection *loaded_collections;
    size_t loaded_collection_count;
    size_t loaded_collection_capacity;
} sxml_processor_document_loader;

/* Writes sizeof(sxml_processor_document_loader) and its alignment. Returns
   SXML_STATUS_INVALID_ARGUMENT without a processor. */
SXML_NODISCARD sxml_status sxml_processor_document_loader_memory_requirements(
    const sxml_processor_document_loader_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a loader that has loaded nothing in memory. It allocates as it
 * loads, from the processor's allocator. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration and the memory
 * checks of the conventions.
 */
SXML_NODISCARD sxml_status sxml_processor_document_loader_initialize(
    sxml_memory_view memory,
    const sxml_processor_document_loader_configuration *configuration,
    sxml_processor_document_loader **loader);

/* Releases the documents and collections it loaded; accepts NULL. */
void sxml_processor_document_loader_finalize(
    sxml_processor_document_loader *loader);

/* The loader as the evaluator's interface; keeps a reference to loader. */
sxml_document_loader sxml_processor_document_loader_as_document_loader(
    sxml_processor_document_loader *loader);

/*
 * The processor's loader behind a dynamic context, for the library
 * functions that read the evaluation environment (fn:unparsed-text,
 * fn:collection, fn:trace, ...): NULL when dynamic_context is NULL or its
 * document loader is not one the processor made (a unit test's), in which
 * case those functions see no resources, no environment variables, the
 * default language "en" and the null trace sink.
 */
sxml_processor_document_loader *sxml_processor_document_loader_from_dynamic_context(
    const sxml_dynamic_context *dynamic_context);

/* The loader's evaluation environment, borrowed. */
const sxml_evaluation_environment *sxml_processor_document_loader_get_environment(
    const sxml_processor_document_loader *loader);

/* The evaluation environment behind a dynamic context, borrowed: NULL
   when sxml_processor_document_loader_from_dynamic_context is. */
const sxml_evaluation_environment *sxml_processor_get_evaluation_environment(
    const sxml_dynamic_context *dynamic_context);

/* -------------------------------------------------- Parameter documents */

/*
 * A loader placed in memory over the processor's resolver and trace sink,
 * with no available documents, environment variables or document
 * preparation: what a compiler reads documents through (a parameter
 * document of XQuery's output:parameter-document or of xsl:output).
 * Fails as sxml_processor_document_loader_initialize.
 */
SXML_NODISCARD sxml_status sxml_processor_compile_document_loader_initialize(
    sxml_memory_view memory, sxml_processor *processor,
    sxml_processor_document_loader **loader);

/* A serialization parameter document to read (glossary
   `parameter_document`; Serialization 4.0, section 3.1). Views are
   borrowed for the call. */
typedef struct sxml_parameter_document_request {
    sxml_document_loader loader;
    /* As written; resolved against base_uri by the loader. */
    sxml_byte_view uri_reference;
    sxml_byte_view base_uri;
} sxml_parameter_document_request;

/* A parameter document as read. */
typedef struct sxml_parameter_document {
    /* False when the loader raised a dynamic error (the document is not
       available or not well-formed): the caller decides what that is
       (XQST0119 in XQuery, ignored by XSLT 4.0, 26.1.5). */
    bool is_loaded;
    /* Owned when is_loaded. */
    sxml_item document_node;
    /* The document element, when the document has one. */
    bool has_document_element;
    sxml_node_reference document_element;
} sxml_parameter_document;

/* Reads request's document into *document. A dynamic error of the loader
   is no failure (is_loaded is false); returns the loader's other failures,
   with *document not loaded. */
SXML_NODISCARD sxml_status sxml_parameter_document_load(
    const sxml_parameter_document_request *request, sxml_parameter_document *document);

/* Releases a loaded document's node and marks it not loaded. */
void sxml_parameter_document_release(sxml_parameter_document *document);

/* ---------------------------------------------------- Streamed documents */

/* A document xsl:source-document reads streamed (XSLT 4.0, 18.1; D219).
   Views are borrowed for the call. */
typedef struct sxml_streamed_document_request {
    /* The href, as written. */
    sxml_byte_view uri_reference;
    /* The static base URI to resolve it against; may be empty. */
    sxml_byte_view base_uri;
    sxml_error_record *error_record;
} sxml_streamed_document_request;

/* An open streamed document (glossary `streamed_document`). It must not
   move while open: the parse request's entity resolver points into it. */
typedef struct sxml_streamed_document {
    /* True: parse_request reads the bytes the resolver delivered (with an
       entity resolver over the evaluation's resolver). False: the
       document is document_node, owned, the node fn:doc gives. */
    bool is_streamed;
    sxml_xml_parse_request parse_request;
    sxml_item document_node;
    /* What fn:doc applies to each document it loads (XSLT's whitespace
       stripping, 4.2); a streamed document's windows go through it. */
    sxml_document_preparation preparation;
    /* Private to src/processor.c. */
    sxml_entity_resolution entity_resolution;
    sxml_resource resource;
    bool holds_resource;
    sxml_uri_resolution resolution;
} sxml_streamed_document;

/*
 * Opens request's document into *document (as fn:doc resolves and finds
 * it: available and loaded documents first, then the resolver). Returns
 * fn:doc's errors (FODC0002, FODC0005) and the resolver's and allocator's
 * failures; *document is then closed. Close it on every path.
 */
SXML_NODISCARD sxml_status sxml_processor_document_loader_open_streamed(
    sxml_processor_document_loader *loader, const sxml_streamed_document_request *request,
    sxml_streamed_document *document);

/* Gives the resource back and releases what document holds; accepts a
   closed document. */
void sxml_processor_document_loader_close_streamed(sxml_processor_document_loader *loader,
                                                   sxml_streamed_document *document);

/* ------------------------------------------------------------ Resources */

/* A text or binary resource to retrieve (F&O 4.0, "fn:unparsed-text",
   "fn:unparsed-binary"). Views are borrowed for the call. */
typedef struct sxml_resource_retrieval_request {
    /* TEXT or BINARY. */
    sxml_resource_kind resource_kind;
    /* As the query wrote it. */
    sxml_byte_view uri_reference;
    /* The static base URI of the caller; may be empty. */
    sxml_byte_view base_uri;
    /* TEXT: the encoding option, or empty. */
    sxml_byte_view encoding_name;
} sxml_resource_retrieval_request;

/*
 * A retrieved resource. When is_available, content, encoding_name and
 * resource_uri are borrowed until sxml_processor_document_loader_release
 * is called; otherwise description says why (may be empty) and
 * has_absolute_uri whether the reference could be made absolute.
 */
typedef struct sxml_retrieved_resource {
    bool is_available;
    bool has_absolute_uri;
    sxml_byte_view content;
    /* Named outside the content (an HTTP charset), or empty. */
    sxml_byte_view encoding_name;
    sxml_byte_view description;
    /* Private: what the resolver delivered and the resolved URI. */
    bool is_delivered;
    sxml_resource resource;
    sxml_uri_resolution resolution;
} sxml_retrieved_resource;

/*
 * Resolves request->uri_reference against its base URI (RFC 3986,
 * section 5) and asks the environment's resolver for it. Writes the
 * outcome to *retrieved, which the caller hands to
 * sxml_processor_document_loader_release on every path where this returns
 * SXML_STATUS_OK. A resource that is not available is not a failure.
 * Returns SXML_STATUS_INVALID_ARGUMENT for a form the kind does not allow
 * (include/sxml/processor.h), the resolver's and the allocator's
 * failures.
 */
SXML_NODISCARD sxml_status sxml_processor_document_loader_retrieve(
    sxml_processor_document_loader *loader,
    const sxml_resource_retrieval_request *request,
    sxml_retrieved_resource *retrieved);

/* Gives the resource back to the resolver and frees the resolved URI. */
void sxml_processor_document_loader_release(
    sxml_processor_document_loader *loader, sxml_retrieved_resource *retrieved);

/* A collection to read (F&O 4.0, "fn:collection", "fn:uri-collection"). */
typedef struct sxml_collection_request {
    /* COLLECTION or URI_COLLECTION. */
    sxml_resource_kind resource_kind;
    /* True for the default collection: uri_reference is then ignored. */
    bool is_default_collection;
    sxml_byte_view uri_reference;
    sxml_byte_view base_uri;
} sxml_collection_request;

/* What a collection request gives. */
typedef struct sxml_loaded_collection {
    bool is_available;
    /* Owned when is_available. */
    sxml_sequence items;
    /* Why not, for the error description; borrowed from static data. */
    sxml_byte_view description;
} sxml_loaded_collection;

/*
 * Writes the collection request names to *collection: the one this
 * evaluation read before for the same kind and absolute URI, or what the
 * resolver delivers, kept for the rest of the evaluation. A relative
 * reference without a base URI, an unknown collection and the null
 * resolver give is_available false. Returns SXML_STATUS_INVALID_ARGUMENT
 * for a form other than SEQUENCE, the resolver's and the allocator's
 * failures.
 */
SXML_NODISCARD sxml_status sxml_processor_document_loader_load_collection(
    sxml_processor_document_loader *loader, const sxml_collection_request *request,
    sxml_loaded_collection *collection);

/*
 * Hands event to the environment's trace sink (fn:trace, fn:message);
 * the null sink discards it. event->processor is set by the call. Returns
 * the sink's failures.
 */
SXML_NODISCARD sxml_status sxml_processor_document_loader_trace(
    const sxml_processor_document_loader *loader, const sxml_trace_event *event);
