#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
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
 * - a resource resolver, through which fn:doc retrieves documents (and, in
 *   later phases, fn:unparsed-text, XQuery modules and XSLT stylesheet
 *   modules); sxml itself never opens a file or a network connection;
 * - a clock, which gives fn:current-dateTime and the implicit timezone
 *   (XPath 4.0, section 2.2.2 "Dynamic Context");
 * - limits for untrusted documents and expressions.
 * An evaluation may override the resolver and the clock
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
 * What a resource is wanted for. The list only grows: later phases add text
 * resources (fn:unparsed-text, fn:json-doc), XQuery library modules and
 * XSLT stylesheet modules. A resolver answers a kind it does not know with
 * SXML_RESOURCE_FORM_NONE.
 */
typedef enum sxml_resource_kind {
    /* An XML document for fn:doc and fn:doc-available (F&O 4.0,
       "fn:doc"). */
    SXML_RESOURCE_KIND_XML_DOCUMENT,
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
       processor function a resolver may call). */
    sxml_processor *processor;
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
 * every resource kind, and nothing else: for tests and for hosts that hold
 * their documents in memory (a browser). The resolver keeps a reference to
 * resource_entries and the memory it points to, which must outlive it.
 * The first matching entry wins.
 */
SXML_EXPORT sxml_resource_resolver sxml_table_resource_resolver(
    const sxml_resource_entry_view *resource_entries);

/* ------------------------------------------------------------- Processor */

/*
 * Bounds for untrusted input. Unless a field says otherwise, 0 means no
 * bound, and reaching a bound gives SXML_STATUS_LIMIT_EXCEEDED.
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
    /* Deepest nesting of an expression's syntax, so that compiling an
       untrusted expression cannot exhaust the C stack. 0 means sxml's
       default, chosen to fit the smallest C stack of a supported
       platform. */
    uint32_t maximum_expression_depth;
    /* Deepest nesting of evaluation (function calls, nested expressions),
       so that deep recursion gives SXML_STATUS_LIMIT_EXCEEDED rather than a
       stack overflow. 0 means sxml's default, as above. */
    uint32_t maximum_evaluation_depth;
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
