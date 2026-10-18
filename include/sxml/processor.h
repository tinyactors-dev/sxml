#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"

/*
 * The processor: the object a host creates first (docs/DECISIONS.md, D30,
 * D47, D51).
 *
 * A processor owns one name pool, one type registry and one item heap
 * (include/sxml/name.h, types.h, xdm.h), and everything compiled or
 * evaluated with it lives in them: XPath expressions (include/sxml/xpath.h),
 * later XQuery queries and XSLT stylesheets, the documents they read and the
 * values they produce. Values of one processor never meet values of
 * another.
 *
 * It also holds what the host provides to every evaluation:
 * - a resource resolver, through which fn:doc, fn:unparsed-text,
 *   fn:unparsed-binary, fn:collection and fn:uri-collection retrieve
 *   resources, and XQuery and XSLT compilation their modules and
 *   packages; sxml itself never opens a file or a network connection;
 * - a clock, which gives fn:current-dateTime and the implicit timezone
 *   (XPath 4.0, section 2.2.2 "Dynamic Context");
 * - a trace sink, which receives the output of fn:trace, fn:message and
 *   xsl:message;
 * - limits for untrusted documents and expressions.
 * An evaluation may override the resolver, the clock and the trace sink
 * (sxml_dynamic_context_configuration, include/sxml/xpath.h).
 *
 * Lifetime: everything made with a processor (items, sequences, trees,
 * compiled expressions) is released, finalized or destroyed before the
 * processor is.
 *
 * Thread safety: a processor, and everything made with it, is used by one
 * thread at a time (D35). Separate processors are independent: a host runs
 * one per thread.
 */

typedef struct sxml_processor sxml_processor;

/* ----------------------------------------------------------------- Clock */

/*
 * What a clock reads. An evaluation reads its clock at most once, when it
 * first needs either value, so fn:current-dateTime, fn:current-date and
 * fn:current-time agree within one evaluation (F&O 4.0,
 * "fn:current-dateTime": the function is deterministic).
 */
typedef struct sxml_clock_reading {
    /* The current instant. has_timezone must be true; the offset may be
       any valid one: sxml presents the instant in the implicit timezone,
       as fn:current-dateTime requires. */
    sxml_date_time current_date_time;
    /* The implicit timezone, minutes east of UTC, -840 to 840. */
    int16_t implicit_timezone_offset_minutes;
} sxml_clock_reading;

/* The operations of a clock. Tables are static const. */
typedef struct sxml_clock_operations {
    /* Writes the current reading to *reading. A failure stops the
       evaluation that asked, with the status returned. */
    sxml_status (*read)(void *state, sxml_clock_reading *reading);
} sxml_clock_operations;

/*
 * The clock interface, passed by value. A clock whose operations is NULL is
 * the null clock; in a configuration it means "the default" as the field
 * says.
 */
typedef struct sxml_clock {
    void *state;
    const sxml_clock_operations *operations;
} sxml_clock;

/*
 * The default clock: the platform's UTC clock (C17 timespec_get with
 * TIME_UTC), with implicit timezone UTC (offset 0). Its read fails with
 * SXML_STATUS_UNSUPPORTED_FEATURE on a platform without a clock. Its state
 * is NULL.
 *
 * Thread safety: safe to use from any thread.
 */
SXML_EXPORT sxml_clock sxml_system_clock(void);

/*
 * A clock that always reads *reading: for tests, and for hosts that want
 * repeatable results. The clock keeps a reference to reading, which must
 * outlive it.
 */
SXML_EXPORT sxml_clock sxml_fixed_clock(const sxml_clock_reading *reading);

/*
 * Reads clock and writes the reading to *reading. Returns
 * SXML_STATUS_INVALID_ARGUMENT for the null clock or a reading outside the
 * contract above (no timezone, a component or offset out of range), and the
 * clock's failures; *reading is then untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_clock_read(
    sxml_clock clock, sxml_clock_reading *reading);

/* ------------------------------------------------------ Resource resolver */

/*
 * What a resource is wanted for, and the forms a resolver may deliver it
 * in (sxml_resource_form). The list only grows. A resolver answers a kind
 * it does not know with SXML_RESOURCE_FORM_NONE; a form the kind does not
 * allow stops the evaluation or compilation with
 * SXML_STATUS_INVALID_ARGUMENT.
 */
typedef enum sxml_resource_kind {
    /* An XML document for fn:doc and fn:doc-available (F&O 4.0,
       "fn:doc"), and in XSLT for fn:document and xsl:source-document.
       Forms: BYTES, DOCUMENT_NODE. */
    SXML_RESOURCE_KIND_XML_DOCUMENT,
    /* A text resource for fn:unparsed-text, fn:unparsed-text-lines,
       fn:unparsed-text-available and fn:json-doc (F&O 4.0,
       "fn:unparsed-text"). Form: BYTES, which sxml decodes: with the
       resource's encoding_name when given, else the request's, else from a
       byte order mark, else as UTF-8 (FOUT1190 when the bytes do not
       decode or hold a character XML does not allow). */
    SXML_RESOURCE_KIND_TEXT,
    /* A binary resource for fn:unparsed-binary (F&O 4.0,
       "fn:unparsed-binary"). Form: BYTES, taken as they are. */
    SXML_RESOURCE_KIND_BINARY,
    /* A collection for fn:collection (F&O 4.0, "fn:collection"). Form:
       SEQUENCE, any items. */
    SXML_RESOURCE_KIND_COLLECTION,
    /* A URI collection for fn:uri-collection (F&O 4.0,
       "fn:uri-collection"). Form: SEQUENCE of xs:anyURI items. */
    SXML_RESOURCE_KIND_URI_COLLECTION,
    /* An XQuery library module for "import module" (XQuery 4.0, "Module
       Import"): uri_reference is one location hint, or empty when the
       import has none, and target_namespace_uri the module's namespace.
       Form: BYTES, the module's text. */
    SXML_RESOURCE_KIND_XQUERY_MODULE,
    /* An XSLT stylesheet module for xsl:include and xsl:import (XSLT 4.0,
       "Stylesheet Inclusion", "Stylesheet Import"). Form: BYTES. */
    SXML_RESOURCE_KIND_XSLT_MODULE,
    /* An XSLT package for xsl:use-package (XSLT 4.0, "Dependencies between
       Packages"): uri_reference and absolute_uri are the package name and
       package_version the versions accepted. Form: BYTES, the text of
       the module whose top element is xsl:package. */
    SXML_RESOURCE_KIND_XSLT_PACKAGE,
    /* An XSD schema document for schema compilation (include/sxml/
       schema.h): xs:include, xs:import, xs:redefine and xs:override,
       XQuery's "import schema", XSLT's xsl:import-schema. uri_reference
       is the schemaLocation or location hint, empty when there is none;
       target_namespace_uri the namespace (D165). Forms: BYTES,
       DOCUMENT_NODE (a document the host has parsed). */
    SXML_RESOURCE_KIND_SCHEMA_DOCUMENT,
} sxml_resource_kind;

/* What sxml asks a resolver for. Views are borrowed for the call. */
typedef struct sxml_resource_request {
    sxml_resource_kind resource_kind;
    /* The URI as the query wrote it, for example the argument of fn:doc. */
    sxml_byte_view uri_reference;
    /* uri_reference resolved against the static base URI (RFC 3986,
       section 5); empty when uri_reference is relative and the static base
       URI is absent. */
    sxml_byte_view absolute_uri;
    /* The processor of the evaluation, for a resolver that builds the
       document node itself (sxml_processor_parse_document is the one
       processor function a resolver may call) or the items of a
       collection (through sxml_processor_get_item_heap). */
    sxml_processor *processor;
    /* TEXT: the encoding the query names (the $encoding argument of
       fn:unparsed-text); empty when it names none. Informational: sxml
       decodes the bytes itself. */
    sxml_byte_view encoding_name;
    /* COLLECTION, URI_COLLECTION: true for the default collection
       (fn:collection and fn:uri-collection without an argument or with the
       empty sequence; XPath 4.0, section 2.2.2, "default collection");
       uri_reference and absolute_uri are then empty. */
    bool is_default_collection;
    /* XQUERY_MODULE: the target namespace of the imported module, never
       empty. SCHEMA_DOCUMENT: the namespace imported (xs:import, import
       schema, xsl:import-schema; empty for no namespace), or for
       xs:include, xs:redefine and xs:override the referring document's
       target namespace. */
    sxml_byte_view target_namespace_uri;
    /* XSLT_PACKAGE: the package-version attribute of xsl:use-package, "*"
       when it has none (XSLT 4.0, "Versions of a Package"). */
    sxml_byte_view package_version;
} sxml_resource_request;

/* How a resolver delivers a resource. */
typedef enum sxml_resource_form {
    /* There is no such resource, or it cannot be retrieved: fn:doc raises
       FODC0002 and fn:doc-available returns false. */
    SXML_RESOURCE_FORM_NONE = 0,
    /* The resource's bytes, which sxml parses (for an XML document, with
       the limits of the processor). */
    SXML_RESOURCE_FORM_BYTES,
    /* A document node made with request->processor, for an XML document
       the host has already parsed or built. */
    SXML_RESOURCE_FORM_DOCUMENT_NODE,
    /* A sequence made with request->processor: the items of a collection,
       in the order fn:collection returns them, or the URIs of a URI
       collection. */
    SXML_RESOURCE_FORM_SEQUENCE,
} sxml_resource_form;

/*
 * What a resolver delivers. sxml hands resolve a zero-initialized
 * sxml_resource; only the fields of the chosen form are read.
 */
typedef struct sxml_resource {
    sxml_resource_form resource_form;
    /* BYTES: the content. Borrowed until release is called. */
    sxml_byte_view content;
    /* BYTES: the encoding named outside the content (an HTTP charset);
       empty to detect it from the content. Borrowed until release. */
    sxml_byte_view encoding_name;
    /* BYTES: the URI the resource was found at, after any redirection; it
       becomes the document URI and base URI of the document node. Empty
       means request->absolute_uri. Borrowed until release. */
    sxml_byte_view resource_uri;
    /* DOCUMENT_NODE: the node, owned: sxml takes the reference. */
    sxml_item document_node;
    /* NONE: why the resource is not available, for the error description
       (for example "No such file"); may be empty. Borrowed until release. */
    sxml_byte_view description;
    /* SEQUENCE: the items, owned: sxml takes the reference. Nodes keep
       their document URIs (F&O 4.0, "fn:collection": they need not be
       distinct or present). */
    sxml_sequence items;
} sxml_resource;

/*
 * The operations of a resource resolver. Tables are static const.
 *
 * A resolver decides what a query may read: sxml asks it for every
 * resource that is not an available document of the dynamic context, and
 * reads nothing else.
 */
typedef struct sxml_resource_resolver_operations {
    /* Retrieves the resource request describes into *resource. Returns
       SXML_STATUS_OK, with SXML_RESOURCE_FORM_NONE, for anything a query
       should see as "not available" (a missing file, a refused URI, a
       failed read, a network failure). Any other status stops the
       evaluation with that status: keep those for conditions such as
       running out of memory. */
    sxml_status (*resolve)(void *state, const sxml_resource_request *request,
                           sxml_resource *resource);
    /* Optional. Called once for every resolve that returned SXML_STATUS_OK,
       when sxml no longer needs the resource's views. */
    void (*release)(void *state, const sxml_resource *resource);
} sxml_resource_resolver_operations;

/*
 * The resource resolver interface, passed by value. A resolver whose
 * operations is NULL is the null resolver: nothing is available.
 */
typedef struct sxml_resource_resolver {
    void *state;
    const sxml_resource_resolver_operations *operations;
} sxml_resource_resolver;

/* One resource of a table resolver. */
typedef struct sxml_resource_entry {
    /* Compared byte for byte with the request's absolute URI, or with its
       URI reference when the absolute URI is empty. */
    sxml_byte_view uri;
    sxml_byte_view content;
} sxml_resource_entry;

typedef struct sxml_resource_entry_view {
    const sxml_resource_entry *resource_entries;
    size_t resource_entry_count;
} sxml_resource_entry_view;

/*
 * A resolver that serves the resources in *resource_entries, as bytes, for
 * every resource kind whose forms include BYTES, and nothing else (no
 * collections): for tests and for hosts that hold their documents in
 * memory (a browser). The resolver keeps a reference to resource_entries
 * and the memory it points to, which must outlive it. The first matching
 * entry wins.
 */
SXML_EXPORT sxml_resource_resolver sxml_table_resource_resolver(
    const sxml_resource_entry_view *resource_entries);

/* ------------------------------------------------------------ Trace sink */

/* What produced a trace event. The list only grows. */
typedef enum sxml_trace_event_kind {
    /* fn:trace (F&O 4.0, "fn:trace"): value is $input. */
    SXML_TRACE_EVENT_KIND_TRACE,
    /* fn:message (F&O 4.0, "fn:message"): value is $input. */
    SXML_TRACE_EVENT_KIND_MESSAGE,
    /* xsl:message and a failed xsl:assert (XSLT 4.0, "Messages",
       "Assertions"): value is the message, a document node unless the
       instruction's select gives another value. */
    SXML_TRACE_EVENT_KIND_XSLT_MESSAGE,
} sxml_trace_event_kind;

/* One piece of trace output. Everything is borrowed for the call. */
typedef struct sxml_trace_event {
    sxml_trace_event_kind trace_event_kind;
    /* The $label argument of fn:trace and fn:message; empty when the call
       has none, and for xsl:message. */
    sxml_byte_view label;
    /* The value to show, in the item heap of processor. A sink that keeps
       it retains it (sxml_sequence_retain). */
    sxml_sequence value;
    /* XSLT_MESSAGE: true when the instruction terminates the
       transformation (terminate="yes", and every failed xsl:assert): after
       the sink returns SXML_STATUS_OK, sxml raises error_code (XTMM9000 or XTMM9001 by
       default). */
    bool is_terminating;
    /* XSLT_MESSAGE: the error code of the error-code attribute, or the
       default of the instruction; empty otherwise. */
    sxml_error_code error_code;
    /* Where the call or instruction is, as far as known. */
    sxml_source_location location;
    /* The processor of the evaluation, for serializing value. */
    sxml_processor *processor;
} sxml_trace_event;

/* The operations of a trace sink. Tables are static const. */
typedef struct sxml_trace_sink_operations {
    /* Receives one event, in the order the evaluation produces them (which
       is not always the order of the expression text: F&O 4.0 leaves the
       order of evaluation open). A failure, for example D50's
       SXML_STATUS_INPUT_OUTPUT_FAILED, stops the evaluation with the status
       returned. A sink that starts an evaluation nests it on the C stack,
       as a host function body does (include/sxml/xdm.h). */
    sxml_status (*receive)(void *state, const sxml_trace_event *event);
} sxml_trace_sink_operations;

/*
 * The trace sink interface, passed by value. A sink whose operations is
 * NULL is the null sink, which discards every event; in a configuration it
 * means "the default" as the field says.
 */
typedef struct sxml_trace_sink {
    void *state;
    const sxml_trace_sink_operations *operations;
} sxml_trace_sink;

/*
 * A trace sink that writes each event to *output_stream as one line, in
 * UTF-8: the label and ": " when the label is not empty, then the value
 * serialized with the adaptive method and the item separator " "
 * (include/sxml/serialize.h, sxml_serialize_sequence), then U+000A. A
 * terminating XSLT message is written the same way. The sink keeps a reference to output_stream, which
 * must outlive it; the stream's failures are the sink's.
 */
SXML_EXPORT sxml_trace_sink sxml_stream_trace_sink(
    const sxml_output_stream *output_stream);

/* ------------------------------------------------------------- Processor */

/*
 * Bounds for untrusted input. Unless a field says otherwise, 0 means no
 * bound, and reaching a bound gives SXML_STATUS_LIMIT_EXCEEDED. Every bound
 * limits memory or work, never C stack: no part of sxml recurses (D71), so
 * deep input costs heap, which these bounds and the allocator limit.
 */
typedef struct sxml_processor_limits {
    /* sxml_name_pool_configuration.maximum_byte_count. */
    size_t maximum_name_pool_byte_count;
    /* sxml_type_registry_configuration.maximum_registered_type_count. */
    uint32_t maximum_registered_type_count;
    /* sxml_item_heap_configuration.maximum_byte_count. */
    size_t maximum_item_heap_byte_count;
    /* For every document parsed: by sxml_processor_parse_document, fn:doc
       and fn:parse-xml. */
    sxml_xml_limits xml_limits;
    /* For every tree the processor builds, parsed or constructed
       (sxml_arena_tree_configuration). */
    uint32_t maximum_tree_node_count;
    size_t maximum_tree_text_byte_count;
    /* Deepest nesting of an expression's syntax that compilation accepts
       (parsing and lowering). sxml never recurses in C (D71): each level
       costs a frame of a few hundred bytes of allocator memory, so this
       bounds heap use, not the C stack. 0 means sxml's default, 10,000
       levels (D63). */
    uint32_t maximum_expression_depth;
    /* Most function calls in progress at once in one evaluation (IR
       function calls and the calls of higher-order library functions; a
       tail call does not add one). Each costs some hundred bytes of
       allocator memory, not C stack (D71, D75); beyond it the evaluation
       stops with SXML_STATUS_LIMIT_EXCEEDED. 0 means sxml's default,
       1,000,000 calls (D75). */
    uint32_t maximum_evaluation_depth;
    /* Most evaluation steps of one evaluation: every step of the
       evaluator's machine counts one, and work that reads many items
       without taking steps charges them (a general comparison counts its
       pairs). Bounds the time of an evaluation that allocates little, such
       as "some $x in 1 to 100000000000 satisfies $x = 0"; beyond it the
       evaluation stops with SXML_STATUS_LIMIT_EXCEEDED. 0 means sxml's
       default, 1,000,000,000 steps (D89). */
    uint64_t maximum_evaluation_step_count;
} sxml_processor_limits;

typedef struct sxml_processor_configuration {
    /* Provides all memory: the name pool, type registry and item heap,
       documents, compiled expressions. Must not be the null allocator;
       must outlive the processor. */
    sxml_allocator allocator;
    sxml_processor_limits limits;
    /* How fn:doc retrieves documents that are not available documents of
       the dynamic context. The null resolver makes nothing available.
       Its state must outlive the processor. */
    sxml_resource_resolver resource_resolver;
    /* The null clock means sxml_system_clock(). Its state must outlive the
       processor. */
    sxml_clock clock;
    /* Receives fn:trace, fn:message and xsl:message output. The null sink
       discards it. Its state must outlive the processor. */
    sxml_trace_sink trace_sink;
} sxml_processor_configuration;

/*
 * Writes the memory requirements of a processor to *requirements: the
 * processor with its name pool, type registry and item heap, without what
 * they allocate as they grow. Returns SXML_STATUS_INVALID_ARGUMENT for an
 * invalid configuration (the null allocator).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_processor_memory_requirements(
    const sxml_processor_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a processor in memory, with its name pool, type registry and item
 * heap, and writes its address to *processor. memory must stay valid,
 * unmoved, until sxml_processor_finalize.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration,
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements, and the failures of
 * sxml_type_registry_initialize (which interns the built-in type names);
 * on failure nothing is left allocated.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_processor_initialize(
    sxml_memory_view memory, const sxml_processor_configuration *configuration,
    sxml_processor **processor);

/*
 * Finalizes the item heap, type registry and name pool. Everything made
 * with the processor must have been released, finalized or destroyed
 * before; sxml cannot check this. Does nothing when processor is NULL.
 */
SXML_EXPORT void sxml_processor_finalize(sxml_processor *processor);

/*
 * Allocates a processor through allocator and initializes it. allocator is
 * stored and must outlive the processor; it may also be the
 * configuration's allocator. Returns the failures of
 * sxml_processor_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_processor_create(
    sxml_allocator allocator, const sxml_processor_configuration *configuration,
    sxml_processor **processor);

/*
 * Finalizes a processor made by sxml_processor_create and releases its
 * memory. Does nothing when processor is NULL.
 */
SXML_EXPORT void sxml_processor_destroy(sxml_processor *processor);

/*
 * The processor's item heap, name pool and type registry, for working with
 * values through include/sxml/xdm.h, name.h and types.h: interning the
 * names of variables and namespaces, making items to bind, reading
 * results. Borrowed: valid until the processor is finalized.
 */
SXML_EXPORT sxml_item_heap *sxml_processor_get_item_heap(
    const sxml_processor *processor);
SXML_EXPORT sxml_name_pool *sxml_processor_get_name_pool(
    const sxml_processor *processor);
SXML_EXPORT sxml_type_registry *sxml_processor_get_type_registry(
    const sxml_processor *processor);

/*
 * Parses request->input as an XML document (include/sxml/xml.h) into a new
 * arena tree with the processor's limits, and writes its document node,
 * owned, to *document_node: the context item of an evaluation, the value of
 * a variable or an available document. The tree is destroyed when the last
 * item referring to it is released.
 *
 * Returns the failures of sxml_xml_parser_parse, with request->diagnostic
 * filled, and those of building the tree; *document_node is then
 * untouched and nothing is left allocated.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_processor_parse_document(
    sxml_processor *processor, const sxml_xml_parse_request *request,
    sxml_item *document_node);
