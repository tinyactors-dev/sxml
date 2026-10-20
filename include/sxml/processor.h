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
       Forms: BYTES, DOCUMENT_NODE, BYTE_READER. */
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
    /* An external entity of an XML document sxml parses through
       sxml_processor_entity_resolver: its external DTD subset, an external
       parameter entity or an external parsed general entity (XML 1.0,
       4.2.2; D199). uri_reference is the system identifier as written,
       absolute_uri it resolved against the base URI of the entity holding
       the declaration (empty when that has none). Form: BYTES, whose
       resource_uri is the base URI of the declarations inside. */
    SXML_RESOURCE_KIND_EXTERNAL_ENTITY,
    /* A file of the IANA Time Zone Database for the timezone database
       feature (sxml_timezone_database_feature; D209): absolute_uri is
       "urn:sxml:timezone-data:" and a path as in a zoneinfo directory
       such as /usr/share/zoneinfo, either a zone name ("Europe/Paris",
       whose file is TZif, RFC 8536) or "zone1970.tab" or "zone.tab" (the
       zone tables). sxml checks a zone name before it asks: ASCII letters,
       digits, "_", "-", "+" and "/" between parts, no part "." or "..".
       Form: BYTES. */
    SXML_RESOURCE_KIND_TIMEZONE_DATA,
    /* The data of the UCA collation feature (sxml_uca_collation_feature;
       D213, D220), absolute_uri one of:
       - "urn:sxml:collation-data:" and a path as in the common/ directory
         of the Unicode CLDR: "uca/allkeys_CLDR.txt", the CLDR root
         collation as a collation element table (UTS #10, section 9.1;
         UTS #35, Part 5, section 2.3), asked for first; or
         "collation/" + a locale + ".xml", the LDML file of a language's
         collations ("collation/de.xml", "collation/de_AT.xml",
         "collation/root.xml"; UTS #35, Part 5). sxml makes the path from
         ASCII letters, digits, "_" and "/" and one ".xml" or ".txt", so a
         resolver can map it onto a directory.
       - "http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt", the DUCET
         of UTS #10 version 16.0.0 in its file format, asked for when the
         CLDR root is not served.
       Form: BYTES. */
    SXML_RESOURCE_KIND_COLLATION_DATA,
    /* The data of the localization feature (sxml_localization_feature;
       D221): absolute_uri is "urn:sxml:locale-data:" and a path as in the
       common/ directory of the Unicode CLDR: "main/" or "rbnf/" + a
       locale + ".xml" (the LDML files of a language's names and spell-out
       rules, "main/de.xml", "main/fr_CA.xml", "rbnf/root.xml"; UTS #35,
       Parts 3 and 4), "supplemental/supplementalData.xml" (the eras'
       dates) or "supplemental/ordinals.xml" (ordinal plural rules). sxml
       makes the path from ASCII letters, digits, "_" and "/" and ".xml",
       so a resolver can map it onto a directory. Form: BYTES. */
    SXML_RESOURCE_KIND_LOCALE_DATA,
    /* A stop word list of Full Text (sxml_full_text_feature; XQuery and
       XPath Full Text 3.0, 3.4.7; D214), asked for while a query is
       compiled: "stop words at" a URI (uri_reference as written,
       absolute_uri resolved against the static base URI), or, with both
       empty, the default list of "stop words default". Form: BYTES,
       UTF-8, the words separated by whitespace. NONE: a list by URI
       raises FTST0008; the default list is then empty. */
    SXML_RESOURCE_KIND_FULL_TEXT_STOP_WORDS,
    /* A thesaurus of Full Text (3.4.3), asked for while a query is
       compiled: "thesaurus at" a URI (as for stop words), or, with both
       URIs empty, the default thesaurus of "thesaurus default". Forms:
       BYTES or DOCUMENT_NODE, an XML document in the thesaurus format of
       the W3C Full Text test suite (namespace
       "http://www.w3.org/2007/xqftts/thesaurus"): entry elements, each a
       term and synonym elements, each a term, a relationship and
       synonyms of its own one level further. NONE: a thesaurus by URI
       raises FTST0018; the default thesaurus is then empty. */
    SXML_RESOURCE_KIND_FULL_TEXT_THESAURUS,
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

/*
 * A reader of a resource's bytes, read a part at a time (glossary
 * `byte_reader`, D233): sxml lends it a buffer and it fills the front of
 * it. Tables are static const.
 */
typedef struct sxml_byte_reading {
    /* How many bytes read wrote to the front of the buffer, at most its
       size; 0 only at the end. */
    size_t byte_count;
    /* True when no bytes follow those written. */
    bool is_end;
} sxml_byte_reading;

typedef struct sxml_byte_reader_operations {
    /* Reads the next bytes into buffer and says how many and whether they
       are the last (*reading is zeroed by sxml before the call). After
       is_end, sxml does not call read again. A failed read returns
       SXML_STATUS_INPUT_OUTPUT_FAILED, which stops the parse as a
       resource that is not available (fn:doc: FODC0002); any other status
       stops the evaluation with that status. */
    sxml_status (*read)(void *state, sxml_memory_view buffer,
                        sxml_byte_reading *reading);
} sxml_byte_reader_operations;

/* Passed by value. */
typedef struct sxml_byte_reader {
    void *state;
    const sxml_byte_reader_operations *operations;
} sxml_byte_reader;

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
    /* The resource's bytes through a byte reader, for an XML document
       (D233): a document streamed by XSLT (xsl:source-document
       streamable="yes", a streamed principal source) is decoded and parsed
       as its bytes arrive, so its size does not bound the memory; fn:doc
       and the other uses read the whole document first, as for BYTES. */
    SXML_RESOURCE_FORM_BYTE_READER,
} sxml_resource_form;

/*
 * What a resolver delivers. sxml hands resolve a zero-initialized
 * sxml_resource; only the fields of the chosen form are read.
 */
typedef struct sxml_resource {
    sxml_resource_form resource_form;
    /* BYTES: the content. Borrowed until release is called. */
    sxml_byte_view content;
    /* BYTES, BYTE_READER: the encoding named outside the content (an
       HTTP charset); empty to detect it from the content. Borrowed until
       release. */
    sxml_byte_view encoding_name;
    /* BYTES, BYTE_READER: the URI the resource was found at, after any
       redirection; it becomes the document URI and base URI of the
       document node. Empty means request->absolute_uri. Borrowed until
       release. */
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
    /* BYTE_READER: the reader, borrowed until release is called; sxml
       reads it at most to its end, and may stop before. encoding_name and
       resource_uri are read as for BYTES. */
    sxml_byte_reader byte_reader;
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

/* ----------------------------------------------------------- File system */

/*
 * The host's file system, for the EXPath File feature
 * (sxml_expath_file_feature below; packet 7.9, D200). sxml never touches
 * files itself: every file: function reaches files through this
 * interface, and without one (the null file system) every file:
 * function that needs files raises file:io-error.
 *
 * Paths given to the operations are absolute and in the host's form:
 * sxml resolves a query's relative paths against the CURRENT directory,
 * turns file: URIs into paths and joins names with directory_separator.
 * They are UTF-8 and borrowed for the call. An operation returns
 * SXML_STATUS_OK, or SXML_STATUS_INPUT_OUTPUT_FAILED when the file
 * system refuses (file:io-error), or the allocator's failures. sxml
 * checks with describe before an operation, so the operations need not
 * tell a missing file from other failures. Views a file system writes
 * to its outputs belong to it and stay valid until its next operation.
 */

/* What a path names. */
typedef enum sxml_file_kind {
    /* Nothing, or nothing the host lets sxml see. */
    SXML_FILE_KIND_NONE,
    SXML_FILE_KIND_FILE,
    SXML_FILE_KIND_DIRECTORY,
    /* Something else: a device, a socket. */
    SXML_FILE_KIND_OTHER,
} sxml_file_kind;

/* What describe tells about a path; a symbolic link is described as what
   it refers to (EXPath File 4.0, 1.3). */
typedef struct sxml_file_description {
    sxml_file_kind file_kind;
    /* The size of a FILE, in octets. */
    uint64_t byte_count;
    /* The last modification, seconds and nanoseconds since
       1970-01-01T00:00:00Z. */
    int64_t modified_seconds;
    uint32_t modified_nanoseconds;
} sxml_file_description;

/* A part of a file to read into memory. */
typedef struct sxml_file_read_request {
    sxml_byte_view path;
    uint64_t offset;
    /* Receives memory.byte_count octets from offset; sxml asks only for
       octets inside the file. */
    sxml_memory_view memory;
} sxml_file_read_request;

/* How write changes a file. */
typedef enum sxml_file_write_mode {
    /* The file becomes content, created when it does not exist. */
    SXML_FILE_WRITE_MODE_REPLACE,
    /* content is added at the end, the file created when it does not
       exist. */
    SXML_FILE_WRITE_MODE_APPEND,
    /* content overwrites the octets from offset (at most the file's size),
       extending the file as needed. */
    SXML_FILE_WRITE_MODE_AT_OFFSET,
} sxml_file_write_mode;

typedef struct sxml_file_write_request {
    sxml_byte_view path;
    sxml_byte_view content;
    sxml_file_write_mode write_mode;
    uint64_t offset;
} sxml_file_write_request;

/* One entry of a directory: its name (no separator), and what it is. */
typedef struct sxml_file_entry {
    sxml_byte_view name;
    sxml_file_kind file_kind;
} sxml_file_entry;

/* Receives the entries list finds; a failure stops the listing and is
   list's. */
typedef struct sxml_file_entry_sink {
    void *state;
    sxml_status (*add)(void *state, const sxml_file_entry *entry);
} sxml_file_entry_sink;

typedef struct sxml_file_list_request {
    /* A directory; empty for the roots of the file system
       (file:list-roots), whose names are absolute paths. */
    sxml_byte_view path;
    sxml_file_entry_sink sink;
} sxml_file_list_request;

/* A rename: the target does not exist or is a file it replaces. */
typedef struct sxml_file_move_request {
    sxml_byte_view source;
    sxml_byte_view target;
} sxml_file_move_request;

/* A new, empty file or directory whose name did not exist before. */
typedef struct sxml_file_temporary_request {
    /* An existing directory. */
    sxml_byte_view directory;
    /* The name starts with prefix and ends with suffix; the rest is the
       host's choice. */
    sxml_byte_view prefix;
    sxml_byte_view suffix;
    bool is_directory;
} sxml_file_temporary_request;

/* The directories get_directory gives. */
typedef enum sxml_file_directory_kind {
    /* Where relative paths are resolved (EXPath File 4.0, 1.2). */
    SXML_FILE_DIRECTORY_KIND_CURRENT,
    /* Where temporary files go by default (file:temp-dir). */
    SXML_FILE_DIRECTORY_KIND_TEMPORARY,
} sxml_file_directory_kind;

/* The operations of a file system. Tables are static const. */
typedef struct sxml_file_system_operations {
    /* The host's conventions (EXPath File 4.0, 6): "/" or "\\", ":" or
       ";", "\n" or "\r\n". On "\\" sxml also accepts "/" in paths and
       reads "C:\\" and "\\\\server\\" as absolute. Static text. */
    sxml_byte_view directory_separator;
    sxml_byte_view path_separator;
    sxml_byte_view line_separator;
    /* Describes path; a path that names nothing is SXML_FILE_KIND_NONE
       with SXML_STATUS_OK. */
    sxml_status (*describe)(void *state, sxml_byte_view path,
                            sxml_file_description *description);
    sxml_status (*read)(void *state, const sxml_file_read_request *request);
    sxml_status (*write)(void *state, const sxml_file_write_request *request);
    /* Calls request->sink.add once per entry, "." and ".." left out. */
    sxml_status (*list)(void *state, const sxml_file_list_request *request);
    /* Creates one directory whose parent exists. */
    sxml_status (*create_directory)(void *state, sxml_byte_view path);
    /* Removes a file, an empty directory or a symbolic link (not what it
       refers to). */
    sxml_status (*remove)(void *state, sxml_byte_view path);
    sxml_status (*move)(void *state, const sxml_file_move_request *request);
    /* Creates the file or directory and writes its absolute path to
       *created_path. */
    sxml_status (*create_temporary)(void *state, const sxml_file_temporary_request *request,
                                    sxml_byte_view *created_path);
    /* Writes the absolute path of a directory to *path. */
    sxml_status (*get_directory)(void *state, sxml_file_directory_kind directory_kind,
                                 sxml_byte_view *path);
    /* Writes the canonical absolute path of an existing path to
       *canonical_path: no ".", "..", or symbolic links
       (file:path-to-native). */
    sxml_status (*canonicalize)(void *state, sxml_byte_view path,
                                sxml_byte_view *canonical_path);
} sxml_file_system_operations;

/*
 * The file system interface, passed by value. A file system whose
 * operations is NULL is the null file system: there are no files.
 */
typedef struct sxml_file_system {
    void *state;
    const sxml_file_system_operations *operations;
} sxml_file_system;

/* ------------------------------------------------------ Optional features */

/*
 * Optional features (glossary `optional_feature`; D183, D184, D189): parts
 * of sxml that a host opts into by naming them in
 * sxml_processor_configuration.optional_features. Each feature has an
 * exported function that returns it, and sxml reaches a feature only
 * through the value the host passed, so a program that never names a
 * feature does not link it. Without a feature, sxml behaves as the
 * specification says a processor without it behaves (D162).
 *
 * Large data a feature needs, such as collation tables or the timezone
 * database, is not compiled in. The feature asks the processor's resource
 * resolver for it, in the data's standard format and under a resource
 * kind of its own, when it is first used, and keeps what it read until
 * the processor is finalized (glossary `feature_data`).
 */

/* What a feature is: static const data inside sxml. */
typedef struct sxml_optional_feature_definition sxml_optional_feature_definition;

/* One optional feature, passed by value. Its definition is never NULL in
   a value sxml returns. */
typedef struct sxml_optional_feature {
    const sxml_optional_feature_definition *definition;
} sxml_optional_feature;

typedef struct sxml_optional_feature_view {
    const sxml_optional_feature *optional_features;
    size_t optional_feature_count;
} sxml_optional_feature_view;

/*
 * A feature that adds nothing to what sxml does: hosts and sxml's own
 * tests name it to check that a feature list reaches the processor
 * (sxml_processor_has_optional_feature). Its name is "test".
 */
SXML_EXPORT sxml_optional_feature sxml_test_optional_feature(void);

/*
 * The EXPath Binary Module 4.0 (namespace http://expath.org/ns/binary):
 * the bin: functions over xs:base64Binary and xs:hexBinary values, and
 * fn:system-properties()?supports-binary-library true. Without it the
 * bin: functions are absent (XPST0017). Its name is "expath-binary"
 * (packet 7.8, D198).
 */
SXML_EXPORT sxml_optional_feature sxml_expath_binary_feature(void);

/*
 * The HTML parser (HTML Living Standard, 13.2): fn:parse-html and
 * fn:html-doc (F&O 4.0, 17.3), which build trees in the XHTML, SVG and
 * MathML namespaces. Without it both functions are absent (XPST0017).
 * Its name is "html" (packet 7.10a, D201).
 */
SXML_EXPORT sxml_optional_feature sxml_html_parser_feature(void);

/*
 * The EXPath File Module 4.0 (namespace http://expath.org/ns/file): the
 * file: functions over the configuration's file_system, and
 * fn:system-properties()?supports-file-library true. Without it the
 * file: functions are absent (XPST0017). It gives queries access beyond
 * the resolver, so sxml_all_optional_features() leaves it out: a host
 * names it on purpose (D184). Its name is "expath-file" (packet 7.9,
 * D200).
 */
SXML_EXPORT sxml_optional_feature sxml_expath_file_feature(void);

/*
 * The timezone database (IANA Time Zone Database; glossary
 * `timezone_database`): fn:civil-timezone for IANA zone names, and the
 * $place argument of fn:format-dateTime, fn:format-date and
 * fn:format-time, which adjusts to a zone's offset and names timezones
 * ([ZN]) for a zone name or an ISO 3166 country code (F&O 4.0, sections
 * 9.7.4 and 9.9.4.8). It reads the zones' TZif files and the zone tables
 * through the resolver as SXML_RESOURCE_KIND_TIMEZONE_DATA when first
 * used. Without it, or without the data, fn:civil-timezone knows only UTC
 * (FODT0004 for other places) and $place changes nothing. Its name is
 * "timezone-database" (packet 7.16, D209).
 */
SXML_EXPORT sxml_optional_feature sxml_timezone_database_feature(void);

/*
 * UCA collations (F&O 4.0, 5.2.4; UTS #10, version 16.0.0; UTS #35, Part
 * 5): a UCA collation URI orders strings by the CLDR root collation (or
 * the DUCET), which the feature asks the resolver for as
 * SXML_RESOURCE_KIND_COLLATION_DATA when a UCA collation is first used,
 * with the parameters strength, alternate, maxVariable, backwards,
 * caseLevel, caseFirst, numeric and normalization, the reorder codes of
 * reorder, and for lang the CLDR collation of the language (its standard
 * one, or another through "-u-co-", as de-u-co-phonebk), compiled from
 * the language's CLDR file at its first use. Without it, or when the
 * resolver has no data, a UCA URI with fallback=yes (the default) uses
 * sxml's fallback (D97), and one with fallback=no is not supported
 * (FOCH0002, XQST0038); the same for a language without CLDR data. Its
 * name is "uca-collation" (packets 7.15a and 7.15b, D213, D220).
 */
SXML_EXPORT sxml_optional_feature sxml_uca_collation_feature(void);

/*
 * Languages and calendars in the formatting functions (F&O 4.0, 9.9.4.8,
 * 4.8.1; XSLT 4.0, 12.4): the names of months, days, eras and day
 * periods of fn:format-date, fn:format-time and fn:format-dateTime in the
 * language their $language (or the default language) names, the
 * calendars BE, JE, OS and AH, and the words and ordinals of
 * fn:format-integer and xsl:number in other languages and by CLDR's
 * spell-out rule sets ("%spellout-ordinal"), from the Unicode CLDR files
 * the feature asks the resolver for as SXML_RESOURCE_KIND_LOCALE_DATA at
 * a language's first use. Without it, or when the resolver has no data
 * for a language, dates are formatted in English after "[Language: en]",
 * the other calendars as AD after "[Calendar: AD]", and words are
 * English. Its name is "localization" (packet 7.17, D221).
 */
SXML_EXPORT sxml_optional_feature sxml_localization_feature(void);

/*
 * Invisible XML (Invisible XML Specification 1.0): fn:invisible-xml
 * (F&O 4.0, 17.4), which compiles an iXML grammar and returns a function
 * that parses strings with it into documents, and
 * fn:system-properties()?supports-invisible-xml true. Without it the
 * function is absent (XPST0017). Its name is "invisible-xml" (packet
 * 7.18, D215).
 */
SXML_EXPORT sxml_optional_feature sxml_invisible_xml_feature(void);

/*
 * Every optional feature sxml has, as static data, for hosts that want
 * them all: the sxml command, the test drivers and the JavaScript
 * package's full module. A program that calls this links every feature.
 * EXPath File is not in it: it reaches files beyond the resolver, so a
 * host names it on purpose (D184, D200).
 */
SXML_EXPORT sxml_optional_feature_view sxml_all_optional_features(void);

/* The name of feature, in ASCII, static ("test"). The empty view when its
   definition is NULL. */
SXML_EXPORT sxml_byte_view sxml_optional_feature_get_name(sxml_optional_feature feature);

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
       default, 100,000,000 steps, a few seconds of work (D89, D259). */
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
    /* The optional features the processor may use (above); the empty view
       names none. Read by initialize: the view need not outlive it. A
       feature may be named more than once. A view with a count but no
       array, or a feature whose definition is NULL, makes the
       configuration invalid. */
    sxml_optional_feature_view optional_features;
    /* The files sxml_expath_file_feature() reads and writes (above); the
       null file system means none. Its state must outlive the processor.
       Ignored when the feature is not named (D200). */
    sxml_file_system file_system;
    /* The version of XML whose rules XQuery query text is read by (XQuery
       4.0, xgc: xml-version leaves it to the implementation; glossary
       query_text_xml_version): false, the default, XML 1.0 and XML Names
       1.0; true, XML 1.1 and XML Names 1.1: NEL and LINE SEPARATOR are
       line ends, character references may denote the C0 controls (not
       U+0000), a direct constructor's xmlns:p="" undeclares p, and
       a query that declares no output:version is serialized with version
       1.1 (D195, D237, D275). XPath and XSLT are not affected. */
    bool reads_query_text_as_xml_1_1;
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

/* True when the configuration of processor named feature. */
SXML_EXPORT bool sxml_processor_has_optional_feature(const sxml_processor *processor,
                                                     sxml_optional_feature feature);

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

/*
 * An entity resolver (include/sxml/xml.h) that asks processor's resource
 * resolver for every external entity, with SXML_RESOURCE_KIND_EXTERNAL_ENTITY
 * (D199): a host that names it in sxml_xml_parse_request.entity_resolver
 * lets its resolver decide which external DTDs and entities a document
 * reads. A resource that is not BYTES is not available. The processor
 * keeps the resource being read until the parser releases it, so the
 * resolver serves one parse at a time; it is valid while processor is.
 */
SXML_EXPORT sxml_xml_entity_resolver sxml_processor_entity_resolver(
    sxml_processor *processor);
