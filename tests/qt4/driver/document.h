#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cli_command.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"

/*
 * Catalog and test-set files as trees (docs/QT4_CATALOG.md, D44, D51).
 *
 * The driver reads every file of the QT4 catalog with sxml's own XML parser
 * into sxml's arena tree, through the public headers only, and walks the
 * tree with the public node accessors. Only elements in the catalog
 * namespace (http://www.w3.org/2010/09/qt-fots-catalog) are seen; text,
 * comments and elements of other namespaces are skipped.
 *
 * Thread safety: one thread, the thread of the item heap.
 */

/* The namespace of every QT4 catalog and test-set element. */
#define SXML_QT4_CATALOG_NAMESPACE_URI                                         \
    "http://www.w3.org/2010/09/qt-fots-catalog"

/*
 * What went wrong, for the driver's diagnostics. Views are borrowed from
 * static text, from the file being read or from the documents of the
 * catalog: they are valid until the catalog opens another test set or is
 * finalized.
 */
typedef struct sxml_qt4_problem {
    /* One line without a final period, such as "unknown environment". */
    sxml_byte_view description;
    /* The name or path the description is about; empty for none. */
    sxml_byte_view subject;
    /* The file (a suite path or a test list) and, when known, the line
       and column; line_number 0 means unknown. */
    sxml_source_location location;
} sxml_qt4_problem;

/* What a document needs from its catalog. Every pointer is borrowed and
   must outlive the documents read with it. */
typedef struct sxml_qt4_document_reader {
    /* Memory of the file's bytes, released after parsing, and of the
       tree. */
    sxml_allocator allocator;
    sxml_cli_file_reader file_reader;
    sxml_xml_parser *xml_parser;
    /* Issues tree identifiers; its name pool is the parser's. */
    sxml_item_heap *item_heap;
} sxml_qt4_document_reader;

/* A parsed file. A plain value: load it, use it, unload it. */
typedef struct sxml_qt4_document {
    sxml_arena_tree *arena_tree;
    sxml_tree *tree;
    sxml_name_pool *name_pool;
    sxml_namespace_uri_identifier catalog_namespace;
} sxml_qt4_document;

/* The file to load. Views are borrowed for the call. */
typedef struct sxml_qt4_document_request {
    /* The path the file reader opens. */
    sxml_byte_view file_path;
    /* The suite path: the source name of diagnostics, and the document
       URI and base URI of the document node. */
    sxml_byte_view suite_path;
    /* Filled on failure; may be NULL. */
    sxml_qt4_problem *problem;
} sxml_qt4_document_request;

/*
 * Reads and parses the file request names into a new tree and writes it to
 * *document. Returns SXML_STATUS_INPUT_OUTPUT_FAILED when the file cannot
 * be read, the parser's failures (SXML_STATUS_MALFORMED_INPUT and the
 * others of sxml_xml_parser_parse) with the problem's location set, and
 * the allocator's; *document is then untouched and nothing is left
 * allocated.
 */
SXML_NODISCARD sxml_status sxml_qt4_document_load(
    const sxml_qt4_document_reader *reader,
    const sxml_qt4_document_request *request, sxml_qt4_document *document);

/* Releases the tree. Does nothing for a document whose tree is NULL. */
void sxml_qt4_document_unload(sxml_qt4_document *document);

/* The node reference of node_index in document's tree. */
sxml_node_reference
sxml_qt4_document_reference(const sxml_qt4_document *document,
                            sxml_node_index node_index);

/*
 * Walks the child elements of one element that are in the catalog
 * namespace, in document order. A plain value; it does not retain the
 * document, which must outlive it.
 */
typedef struct sxml_qt4_element_cursor {
    const sxml_qt4_document *document;
    bool has_next_node;
    sxml_node_index next_node;
} sxml_qt4_element_cursor;

/* Starts *cursor at the first child of parent. Returns the tree's
   failures. */
SXML_NODISCARD sxml_status sxml_qt4_element_cursor_start(
    const sxml_qt4_document *document, sxml_node_index parent,
    sxml_qt4_element_cursor *cursor);

/* One step of an element cursor. local_name is owned by the name pool. */
typedef struct sxml_qt4_element_step {
    bool has_element;
    sxml_node_index element;
    sxml_byte_view local_name;
} sxml_qt4_element_step;

/*
 * Moves cursor (in/out) to the next child element in the catalog namespace
 * and writes it to *step, or sets step->has_element false at the end.
 * Returns the tree's failures.
 */
SXML_NODISCARD sxml_status sxml_qt4_element_cursor_next(
    sxml_qt4_element_cursor *cursor, sxml_qt4_element_step *step);

/* An attribute in no namespace of one element. */
typedef struct sxml_qt4_attribute_lookup {
    sxml_node_index element;
    sxml_byte_view local_name;
} sxml_qt4_attribute_lookup;

/* The value of an attribute, owned by the tree. */
typedef struct sxml_qt4_attribute_value {
    bool is_present;
    sxml_byte_view value;
} sxml_qt4_attribute_value;

/*
 * Finds an attribute and writes what was found to *value (is_present false
 * and an empty value when there is none). Returns the tree's failures.
 */
SXML_NODISCARD sxml_status sxml_qt4_document_find_attribute(
    const sxml_qt4_document *document, sxml_qt4_attribute_lookup lookup,
    sxml_qt4_attribute_value *value);

/* The text of one element, and where to put it when it must be joined. */
typedef struct sxml_qt4_text_request {
    sxml_node_index element;
    sxml_arena *arena;
} sxml_qt4_text_request;

/*
 * Writes the text node children of the element, joined, to *text: owned by
 * the tree when there is one text node, by the arena when there are
 * several (a comment between CDATA sections splits them). Child elements
 * and comments add nothing. Returns the tree's and the arena's failures.
 */
SXML_NODISCARD sxml_status sxml_qt4_document_get_text(
    const sxml_qt4_document *document, const sxml_qt4_text_request *request,
    sxml_byte_view *text);
