#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "catalog.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * The resource table of one QT4 test case (glossary: qt4_resource_table):
 * what its environment serves through the processor's resource resolver
 * (processor.h, "Resource resolver"; catalog-schema.xsd, elements
 * "resource" and "collection").
 *
 * The resolver answers
 * - XML_DOCUMENT with a suite file for a file:///SUITE-PATH URI;
 * - TEXT and BINARY with the file of the first <resource> whose URI equals
 *   the request's (TEXT with the resource's encoding as the encoding named
 *   outside the content), else with a suite file for file:///SUITE-PATH;
 * - COLLECTION and URI_COLLECTION with the items or the document URIs of
 *   the collection whose URI equals the request's, or of the default
 *   collection (<collection uri="">) when the request asks for it;
 * - XQUERY_MODULE with the file of a <module> of the test case whose uri
 *   is the request's target namespace (packet 3.13): the one whose
 *   location is the request's location hint, else the one whose file
 *   name is the hint's last segment, else the first; its URI
 *   file:///SUITE-PATH is the module's base URI;
 * - SCHEMA_DOCUMENT with a suite file for a file:///SUITE-PATH URI (the
 *   includes and imports of a schema document), else with the file of the
 *   environment's first <schema> whose uri is the request's target
 *   namespace, whatever the location hint says (an XQuery "import
 *   schema", D174); its URI file:///SUITE-PATH is the document's base
 *   URI;
 * and everything else, other kinds included, as not available. Sources
 * with a URI are not served here: they are available documents of the
 * dynamic context (test_run.c). The request's absolute URI is compared
 * when there is one, else its URI reference.
 *
 * The table is a plain value whose arrays the caller owns; the resolver
 * keeps a pointer to it, so it must outlive every evaluation that uses the
 * resolver.
 */

/* A <resource> as served: an absolute URI and the suite file behind it. */
typedef struct sxml_qt4_served_resource {
    sxml_byte_view uri;
    sxml_byte_view file_path;
    /* The encoding attribute; empty for none. */
    sxml_byte_view encoding;
} sxml_qt4_served_resource;

typedef struct sxml_qt4_served_resource_view {
    const sxml_qt4_served_resource *served_resources;
    size_t served_resource_count;
} sxml_qt4_served_resource_view;

/* A <collection> as served. */
typedef struct sxml_qt4_served_collection {
    /* uri="": the default collection (XPath 4.0, section 2.2.2). */
    bool is_default;
    /* The absolute URI; empty for the default collection. */
    sxml_byte_view uri;
    /* What fn:collection returns: the document nodes of the sources, in
       order. Borrowed: the test owns it; the resolver hands out a new
       reference each time. */
    sxml_sequence items;
    /* What fn:uri-collection returns: their document URIs, then the URIs
       of the collection's resources (served as other resources), as
       xs:anyURI. Borrowed as items. */
    sxml_sequence uris;
} sxml_qt4_served_collection;

typedef struct sxml_qt4_served_collection_view {
    const sxml_qt4_served_collection *served_collections;
    size_t served_collection_count;
} sxml_qt4_served_collection_view;

/* A <module> as served (XQuery 4.0, "Module Import"). */
typedef struct sxml_qt4_served_module {
    /* The target namespace. */
    sxml_byte_view namespace_uri;
    /* The location attribute as written; empty for none. */
    sxml_byte_view location;
    sxml_byte_view file_path;
    /* file:///SUITE-PATH of the file. */
    sxml_byte_view file_uri;
} sxml_qt4_served_module;

typedef struct sxml_qt4_served_module_view {
    const sxml_qt4_served_module *served_modules;
    size_t served_module_count;
} sxml_qt4_served_module_view;

/* A <schema> of the environment as served. */
typedef struct sxml_qt4_served_schema {
    /* The target namespace; empty for none. */
    sxml_byte_view namespace_uri;
    sxml_byte_view file_path;
    /* file:///SUITE-PATH. */
    sxml_byte_view file_uri;
} sxml_qt4_served_schema;

typedef struct sxml_qt4_served_schema_view {
    const sxml_qt4_served_schema *served_schemas;
    size_t served_schema_count;
} sxml_qt4_served_schema_view;

typedef struct sxml_qt4_resource_table {
    /* Reads the suite files. */
    sxml_qt4_catalog *catalog;
    /* Holds the bytes of a served file until sxml releases it. */
    sxml_allocator allocator;
    sxml_qt4_served_resource_view served_resources;
    sxml_qt4_served_collection_view served_collections;
    sxml_qt4_served_module_view served_modules;
    sxml_qt4_served_schema_view served_schemas;
} sxml_qt4_resource_table;

/* The resolver over *table, as described above. */
sxml_resource_resolver
sxml_qt4_resource_table_resolver(const sxml_qt4_resource_table *table);
