#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "document.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * Reading the elements of the XSLT 4.0 test suite (glossary:
 * xslt_test_catalog; docs/QT4_CATALOG.md, "xslt40-test layout";
 * third_party/xslt40-test/admin/catalog-schema.xsd).
 *
 * The files are read with the QT4 driver's documents (document.h): a
 * document is loaded as the QT4 driver loads one and then looks at the
 * elements of the XSLT catalog namespace instead of the QT4 one, so the
 * element cursor, the attribute lookups and the QT4 environment reader
 * (environment.h) walk XSLT catalog elements. The helpers here are what
 * the XSLT catalog, environment and test readers share.
 *
 * Thread safety: one thread, the thread of the document's item heap.
 */

/* The namespace of every XSLT catalog and test-set element. */
#define SXML_XSLT_TEST_CATALOG_NAMESPACE_URI                                   \
    "http://www.w3.org/2012/10/xslt-test-catalog"

/*
 * Loads a file as sxml_qt4_document_load does and makes the document's
 * catalog namespace the XSLT catalog namespace. Returns the failures of
 * sxml_qt4_document_load and the name pool's; *document is then untouched
 * and nothing is left allocated.
 */
SXML_NODISCARD sxml_status sxml_xslt_test_document_load(
    const sxml_qt4_document_reader *reader,
    const sxml_qt4_document_request *request, sxml_qt4_document *document);

/* The elements an element's children are read from, and where results
   go. Borrowed for the call. */
typedef struct sxml_xslt_test_element_reader {
    const sxml_qt4_document *document;
    /* The suite path of the document: file attributes are relative to it
       and problems name it. */
    sxml_byte_view containing_file;
    /* Receives arrays and resolved paths. */
    sxml_arena *arena;
    /* Filled on failure; may be NULL. */
    sxml_qt4_problem *problem;
} sxml_xslt_test_element_reader;

/* Node indices of elements, in document order. */
typedef struct sxml_xslt_test_node_list {
    const sxml_node_index *nodes;
    size_t node_count;
} sxml_xslt_test_node_list;

/* The children of parent with one local name. */
typedef struct sxml_xslt_test_children_request {
    sxml_node_index parent;
    sxml_byte_view local_name;
} sxml_xslt_test_children_request;

/*
 * Collects the child elements of request.parent in the catalog namespace
 * called request.local_name, in document order, into an array in the
 * reader's arena (NULL when there are none). Returns the tree's and the
 * arena's failures, leaving *list untouched.
 */
SXML_NODISCARD sxml_status sxml_xslt_test_collect_children(
    const sxml_xslt_test_element_reader *reader,
    sxml_xslt_test_children_request request, sxml_xslt_test_node_list *list);

/*
 * Finds the first child element of request.parent called
 * request.local_name; writes it to *element and sets *is_found, or clears
 * *is_found. Returns the tree's failures.
 */
typedef struct sxml_xslt_test_child_lookup {
    sxml_node_index element;
    bool is_found;
} sxml_xslt_test_child_lookup;

SXML_NODISCARD sxml_status sxml_xslt_test_find_child(
    const sxml_xslt_test_element_reader *reader,
    sxml_xslt_test_children_request request,
    sxml_xslt_test_child_lookup *lookup);

/*
 * Writes the value of the attribute local_name (in no namespace) of
 * element to *value, empty when it is absent. Returns the tree's
 * failures.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_read_attribute(const sxml_xslt_test_element_reader *reader,
                              sxml_qt4_attribute_lookup lookup,
                              sxml_byte_view *value);

/*
 * Writes the value of a yes/no or boolean attribute (catalog-schema.xsd,
 * yesNoType and xs:boolean) to *lookup.value: "yes", "true" and "1" are
 * true; "no", "false" and "0" are false; anything else, an absent
 * attribute included, is lookup.default_value. Returns the tree's
 * failures, leaving *lookup.value untouched.
 */
typedef struct sxml_xslt_test_flag_lookup {
    sxml_node_index element;
    sxml_byte_view local_name;
    bool default_value;
    /* Receives the value. */
    bool *value;
} sxml_xslt_test_flag_lookup;

SXML_NODISCARD sxml_status
sxml_xslt_test_read_flag(const sxml_xslt_test_element_reader *reader,
                         sxml_xslt_test_flag_lookup lookup);

/*
 * Reads the attribute "file" of element as a suite path resolved against
 * the reader's containing file (suite_path.h), empty when absent. Returns
 * the tree's and the arena's failures.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_read_file_attribute(const sxml_xslt_test_element_reader *reader,
                                   sxml_node_index element,
                                   sxml_byte_view *file_path);

/*
 * A name given as an xs:QName attribute of a catalog element (the names
 * of parameters, initial templates, modes and functions), as written and
 * expanded. Views point into the document, its name pool or the arena.
 */
typedef struct sxml_xslt_test_name {
    /* As written; empty for an absent name. */
    sxml_byte_view lexical_name;
    /* Empty for no namespace. A prefix is bound by the element's in-scope
       namespaces; an unprefixed name is in no namespace (the suite's
       names of templates, modes and parameters are, although the
       catalog's default namespace is in scope); Q{uri}local is read as an
       EQName. */
    sxml_byte_view namespace_uri;
    sxml_byte_view local_name;
} sxml_xslt_test_name;

/* The empty name. */
sxml_xslt_test_name sxml_xslt_test_no_name(void);

/*
 * Reads the attribute lookup names as a name into *name: empty when it is
 * absent, and, for "#default", "#unnamed" and other "#" keywords of
 * initial-mode, the keyword as the lexical name with no local name.
 * Returns SXML_STATUS_MALFORMED_INPUT, with the problem filled, for a
 * prefix that is not bound or a malformed EQName, and the tree's and the
 * name pool's failures; *name is then untouched.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_read_name(const sxml_xslt_test_element_reader *reader,
                         sxml_qt4_attribute_lookup lookup,
                         sxml_xslt_test_name *name);

/* Fills reader->problem, when there is one, with description and subject
   at the reader's containing file. */
typedef struct sxml_xslt_test_problem_text {
    sxml_byte_view description;
    sxml_byte_view subject;
} sxml_xslt_test_problem_text;

void sxml_xslt_test_report_problem(const sxml_xslt_test_element_reader *reader,
                                   sxml_xslt_test_problem_text text);

/* An array of element_count elements of element_byte_count bytes. */
typedef struct sxml_xslt_test_array_request {
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_xslt_test_array_request;

/*
 * Allocates the array in arena, or writes NULL for zero elements. Returns
 * SXML_STATUS_ARITHMETIC_OVERFLOW and the arena's failures.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_allocate_array(sxml_arena *arena,
                              sxml_xslt_test_array_request request,
                              void **memory);
