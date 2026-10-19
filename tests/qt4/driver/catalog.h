#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "applicability.h"
#include "cli_command.h"
#include "document.h"
#include "environment.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * The QT4 catalog (glossary: qt4_catalog; docs/QT4_CATALOG.md): catalog.xml
 * with its test-set entries and global environments, and one open test set
 * at a time, whose test cases are read with their environments resolved.
 *
 * Life of a catalog: initialize or create it; sxml_qt4_catalog_load reads
 * catalog.xml; then, any number of times, sxml_qt4_catalog_open_test_set
 * and sxml_qt4_catalog_read_test_case. Everything a test case holds (its
 * views, its environment's arrays, its result element) is valid until the
 * next open_test_set or until the catalog is finalized; views into
 * catalog.xml are valid until it is finalized.
 *
 * Files are read through the configured file reader only, by the path of
 * catalog.xml's directory followed by their suite path, so the driver needs
 * nothing but the suite directory, also under WASI.
 *
 * Thread safety: one thread at a time.
 */

typedef struct sxml_qt4_catalog sxml_qt4_catalog;

typedef struct sxml_qt4_catalog_configuration {
    /* Provides every allocation. Must not be the null allocator; must
       outlive the catalog. */
    sxml_allocator allocator;
    /* Reads catalog.xml and every file it names. Its state must outlive
       the catalog. */
    sxml_cli_file_reader file_reader;
    /* The path of catalog.xml as the file reader opens it, such as
       "third_party/qt4tests/catalog.xml". Borrowed: must outlive the
       catalog. */
    sxml_byte_view catalog_path;
} sxml_qt4_catalog_configuration;

/*
 * Writes the memory requirements of a catalog, without what it allocates
 * as it loads files, to *requirements. Returns SXML_STATUS_INVALID_ARGUMENT
 * for the null allocator or an empty catalog path.
 */
SXML_NODISCARD sxml_status sxml_qt4_catalog_memory_requirements(
    const sxml_qt4_catalog_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an empty catalog in memory and writes its address to *catalog.
 * Allocates nothing. Returns the failures of
 * sxml_qt4_catalog_memory_requirements and
 * sxml_memory_view_check_requirements.
 */
SXML_NODISCARD sxml_status
sxml_qt4_catalog_initialize(sxml_memory_view memory,
                            const sxml_qt4_catalog_configuration *configuration,
                            sxml_qt4_catalog **catalog);

/* Releases everything the catalog allocated. Accepts NULL. */
void sxml_qt4_catalog_finalize(sxml_qt4_catalog *catalog);

/*
 * Allocates a catalog through allocator, which is stored and must outlive
 * it, and initializes it.
 */
SXML_NODISCARD sxml_status
sxml_qt4_catalog_create(sxml_allocator allocator,
                        const sxml_qt4_catalog_configuration *configuration,
                        sxml_qt4_catalog **catalog);

/* Finalizes a catalog made by create and releases its memory. Accepts
   NULL. */
void sxml_qt4_catalog_destroy(sxml_qt4_catalog *catalog);

/*
 * Reads catalog.xml: makes the name pool, type registry, item heap and XML
 * parser the documents need, then collects the test-set entries and the
 * global environments. Returns, with *problem filled when it is not NULL:
 * SXML_STATUS_INPUT_OUTPUT_FAILED when the file cannot be read; the
 * parser's failures; SXML_STATUS_MALFORMED_INPUT when the document element
 * is not a catalog or a test-set entry has no name or file;
 * SXML_STATUS_INVALID_ARGUMENT when the catalog is loaded already; and the
 * allocator's failures. After a failure the catalog can only be finalized.
 */
SXML_NODISCARD sxml_status sxml_qt4_catalog_load(sxml_qt4_catalog *catalog,
                                                 sxml_qt4_problem *problem);

/* One <test-set name file> of catalog.xml. */
typedef struct sxml_qt4_test_set_entry {
    sxml_byte_view name;
    /* The suite path of the test-set file. */
    sxml_byte_view file_path;
} sxml_qt4_test_set_entry;

/* The number of test-set entries, 0 before a successful load. */
size_t sxml_qt4_catalog_test_set_count(const sxml_qt4_catalog *catalog);

/*
 * Writes entry test_set_index (from 0, in catalog order) to *entry.
 * Returns SXML_STATUS_INDEX_OUT_OF_RANGE, leaving *entry untouched, when
 * there is none.
 */
SXML_NODISCARD sxml_status sxml_qt4_catalog_get_test_set_entry(
    const sxml_qt4_catalog *catalog, size_t test_set_index,
    sxml_qt4_test_set_entry *entry);

/*
 * Finds the first test-set entry called name; writes its index to
 * *test_set_index and returns true, or returns false and leaves it
 * untouched.
 */
bool sxml_qt4_catalog_find_test_set(const sxml_qt4_catalog *catalog,
                                    sxml_byte_view name,
                                    size_t *test_set_index);

/* Which test set to open. */
typedef struct sxml_qt4_test_set_request {
    size_t test_set_index;
    /* Filled on failure; may be NULL. */
    sxml_qt4_problem *problem;
} sxml_qt4_test_set_request;

/*
 * Closes the open test set, if any, and opens the one request names:
 * parses its file, its local environments, its dependencies and the list
 * of its test cases. Returns SXML_STATUS_INDEX_OUT_OF_RANGE for an index
 * past the entries, the failures of sxml_qt4_document_load,
 * SXML_STATUS_MALFORMED_INPUT when the document element is not a test set,
 * and the allocator's; no test set is open after a failure.
 */
SXML_NODISCARD sxml_status sxml_qt4_catalog_open_test_set(
    sxml_qt4_catalog *catalog, const sxml_qt4_test_set_request *request);

/*
 * Writes the index of the open test set to *test_set_index and returns
 * true, or returns false when none is open.
 */
bool sxml_qt4_catalog_get_open_test_set(const sxml_qt4_catalog *catalog,
                                        size_t *test_set_index);

/* The document of the open test set, for walking a test case's result
   (packet 1.25); NULL when none is open. */
const sxml_qt4_document *
sxml_qt4_catalog_get_test_set_document(const sxml_qt4_catalog *catalog);

/* The number of test cases of the open test set; 0 when none is open. */
size_t sxml_qt4_catalog_test_case_count(const sxml_qt4_catalog *catalog);

/*
 * Finds the first test case of the open test set called name; writes its
 * index to *test_case_index and returns true, or returns false and leaves
 * it untouched.
 */
bool sxml_qt4_catalog_find_test_case(const sxml_qt4_catalog *catalog,
                                     sxml_byte_view name,
                                     size_t *test_case_index);

/* A <module uri file location> of a test case (XQuery). */
typedef struct sxml_qt4_module {
    sxml_byte_view uri;
    sxml_byte_view file_path;
    /* The location hint of import module; empty for none. */
    sxml_byte_view location;
} sxml_qt4_module;

typedef struct sxml_qt4_module_view {
    const sxml_qt4_module *modules;
    size_t module_count;
} sxml_qt4_module_view;

/* The query of one <test>: inline text, or the suite path of its file. */
typedef struct sxml_qt4_query {
    sxml_byte_view query_text;
    sxml_byte_view query_file_path;
} sxml_qt4_query;

typedef struct sxml_qt4_query_view {
    const sxml_qt4_query *queries;
    size_t query_count;
} sxml_qt4_query_view;

/* A test case, read with its environment resolved (glossary:
   qt4_test_case). */
typedef struct sxml_qt4_test_case {
    sxml_byte_view test_set_name;
    /* The suite path of the test-set file; with
       SXML_QT4_STATIC_BASE_URI_TEST_SET_FILE, its URI is the static base
       URI. */
    sxml_byte_view test_set_file_path;
    sxml_byte_view name;
    /* The query of the last <test>, whose result the assertions check,
       when it is given inline; empty otherwise. */
    sxml_byte_view query_text;
    /* The suite path of the query file of <test file>; empty when the
       query is inline. */
    sxml_byte_view query_file_path;
    /* The <test> elements before the last, in order: the update queries
       of an XQuery Update test case (catalog-schema.xsd, test/@update;
       D187), which run first, each over the trees the ones before it
       changed. Empty for a test case of one <test>. */
    sxml_qt4_query_view earlier_queries;
    sxml_qt4_environment environment;
    /* The test set's dependencies and the test case's own. */
    sxml_qt4_dependency_view test_set_dependencies;
    sxml_qt4_dependency_view dependencies;
    sxml_qt4_module_view modules;
    /* The <result> element in the test-set document. */
    bool has_result;
    sxml_node_index result;
} sxml_qt4_test_case;

typedef struct sxml_qt4_test_case_request {
    size_t test_case_index;
    /* Filled on failure; may be NULL. */
    sxml_qt4_problem *problem;
} sxml_qt4_test_case_request;

/*
 * Reads test case test_case_index of the open test set into *test_case,
 * resolving its environment: a ref is looked up among the test set's
 * environments, then among the catalog's (catalog-schema.xsd: local
 * environments take precedence). Returns SXML_STATUS_INVALID_ARGUMENT when
 * no test set is open, SXML_STATUS_INDEX_OUT_OF_RANGE for an index past
 * its test cases, SXML_STATUS_MALFORMED_INPUT, with the problem filled,
 * for a reference to an environment that does not exist or a test case
 * without <test>, and the allocator's failures; *test_case is then
 * untouched.
 */
SXML_NODISCARD sxml_status sxml_qt4_catalog_read_test_case(
    sxml_qt4_catalog *catalog, const sxml_qt4_test_case_request *request,
    sxml_qt4_test_case *test_case);

/* Decides whether test_case runs for target (applicability.h). */
typedef struct sxml_qt4_test_case_target {
    const sxml_qt4_test_case *test_case;
    sxml_qt4_target target;
} sxml_qt4_test_case_target;

void sxml_qt4_test_case_decide_applicability(
    sxml_qt4_test_case_target request, sxml_qt4_applicability *applicability);

/* The directory of catalog.xml as the file reader opens it, ending with
   "/" (empty for the working directory): what a suite path is appended
   to, for example a sandpit's (D200). Borrowed from the catalog. */
sxml_byte_view sxml_qt4_catalog_get_directory(const sxml_qt4_catalog *catalog);

/* A suite file to read. */
typedef struct sxml_qt4_file_request {
    sxml_byte_view suite_path;
    /* Provides the memory of the contents. */
    sxml_allocator allocator;
} sxml_qt4_file_request;

/*
 * Reads the suite file request names (a source, a query file, a resource)
 * through the file reader into memory from request->allocator, which the
 * caller releases it to. Returns the failures of
 * sxml_cli_file_reader_read and the allocator's.
 */
SXML_NODISCARD sxml_status sxml_qt4_catalog_read_file(
    sxml_qt4_catalog *catalog, const sxml_qt4_file_request *request,
    sxml_byte_view *contents);
