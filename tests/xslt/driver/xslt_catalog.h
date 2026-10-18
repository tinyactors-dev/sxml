#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "applicability.h"
#include "catalog.h"
#include "cli_command.h"
#include "document.h"
#include "selection.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xslt.h"
#include "xslt_environment.h"
#include "xslt_reading.h"

/*
 * The XSLT 4.0 test catalog (glossary: xslt_test_catalog;
 * docs/QT4_CATALOG.md, "xslt40-test layout"; third_party/xslt40-test/
 * admin/catalog-schema.xsd): catalog.xml with its test-set entries, and
 * one open test set at a time, whose test cases are read with their
 * environments resolved and their <test> read into an invocation.
 *
 * Imitates the QT4 catalog (catalog.h) and has its life: initialize or
 * create; sxml_xslt_test_catalog_load reads catalog.xml; then, any number
 * of times, sxml_xslt_test_catalog_open_test_set and
 * sxml_xslt_test_catalog_read_test_case. Everything a test case holds is
 * valid until the next open_test_set or until the catalog is finalized.
 * Test-set entries, test-set requests and test-case requests are the QT4
 * driver's types.
 *
 * An environment ref names an environment of the test set, then one of
 * catalog.xml (the suite has none there today, catalog-schema.xsd says);
 * an environment element without ref is the test case's own.
 *
 * Thread safety: one thread at a time.
 */

typedef struct sxml_xslt_test_catalog sxml_xslt_test_catalog;

/* As the QT4 catalog's: the allocator, the file reader and the path of
   catalog.xml, such as "third_party/xslt40-test/catalog.xml". */
typedef sxml_qt4_catalog_configuration sxml_xslt_test_catalog_configuration;

/*
 * Writes the memory requirements of a catalog, without what it allocates
 * as it loads files, to *requirements. Returns SXML_STATUS_INVALID_ARGUMENT
 * for the null allocator or an empty catalog path.
 */
SXML_NODISCARD sxml_status sxml_xslt_test_catalog_memory_requirements(
    const sxml_xslt_test_catalog_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places an empty catalog in memory. Allocates nothing. Returns the
   failures of the memory requirements and of
   sxml_memory_view_check_requirements. */
SXML_NODISCARD sxml_status sxml_xslt_test_catalog_initialize(
    sxml_memory_view memory,
    const sxml_xslt_test_catalog_configuration *configuration,
    sxml_xslt_test_catalog **catalog);

/* Releases everything the catalog allocated. Accepts NULL. */
void sxml_xslt_test_catalog_finalize(sxml_xslt_test_catalog *catalog);

/* Allocates a catalog through allocator, which is stored and must outlive
   it, and initializes it. */
SXML_NODISCARD sxml_status sxml_xslt_test_catalog_create(
    sxml_allocator allocator,
    const sxml_xslt_test_catalog_configuration *configuration,
    sxml_xslt_test_catalog **catalog);

/* Finalizes a catalog made by create and releases its memory. Accepts
   NULL. */
void sxml_xslt_test_catalog_destroy(sxml_xslt_test_catalog *catalog);

/*
 * Reads catalog.xml: the parts the documents need, the test-set entries
 * and the global environments. Returns, with *problem filled when it is
 * not NULL, SXML_STATUS_INPUT_OUTPUT_FAILED when the file cannot be read,
 * the parser's failures, SXML_STATUS_MALFORMED_INPUT when the document
 * element is not an XSLT catalog or a test-set entry has no name or file,
 * SXML_STATUS_INVALID_ARGUMENT when the catalog is loaded already, and the
 * allocator's failures. After a failure the catalog can only be finalized.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_catalog_load(sxml_xslt_test_catalog *catalog,
                            sxml_qt4_problem *problem);

/* The number of test-set entries, 0 before a successful load. */
size_t sxml_xslt_test_catalog_test_set_count(
    const sxml_xslt_test_catalog *catalog);

/* Writes entry test_set_index to *entry. Returns
   SXML_STATUS_INDEX_OUT_OF_RANGE, leaving it untouched, when there is
   none. */
SXML_NODISCARD sxml_status sxml_xslt_test_catalog_get_test_set_entry(
    const sxml_xslt_test_catalog *catalog, size_t test_set_index,
    sxml_qt4_test_set_entry *entry);

/* Finds the first test-set entry called name. */
bool sxml_xslt_test_catalog_find_test_set(
    const sxml_xslt_test_catalog *catalog, sxml_byte_view name,
    size_t *test_set_index);

/*
 * Closes the open test set and opens the one request names: its file, its
 * environments, its dependencies (the children of its <dependencies>) and
 * its test cases. Returns SXML_STATUS_INDEX_OUT_OF_RANGE for an index past
 * the entries, the failures of sxml_qt4_document_load,
 * SXML_STATUS_MALFORMED_INPUT when the document element is not a test set,
 * and the allocator's; no test set is open after a failure.
 */
SXML_NODISCARD sxml_status sxml_xslt_test_catalog_open_test_set(
    sxml_xslt_test_catalog *catalog, const sxml_qt4_test_set_request *request);

/* Writes the index of the open test set and returns true, or returns
   false when none is open. */
bool sxml_xslt_test_catalog_get_open_test_set(
    const sxml_xslt_test_catalog *catalog, size_t *test_set_index);

/* The document of the open test set, for walking a test case's result;
   NULL when none is open. */
const sxml_qt4_document *sxml_xslt_test_catalog_get_test_set_document(
    const sxml_xslt_test_catalog *catalog);

/* The number of test cases of the open test set; 0 when none is open. */
size_t sxml_xslt_test_catalog_test_case_count(
    const sxml_xslt_test_catalog *catalog);

/* Finds the first test case of the open test set called name. */
bool sxml_xslt_test_catalog_find_test_case(
    const sxml_xslt_test_catalog *catalog, sxml_byte_view name,
    size_t *test_case_index);

/*
 * How the test case invokes the stylesheet (catalog-schema.xsd, "test";
 * include/sxml/xslt.h, sxml_xslt_invocation_kind): initial-function gives
 * CALL_FUNCTION, else initial-template CALL_TEMPLATE, else
 * APPLY_TEMPLATES in the initial mode of initial-mode or the default mode.
 * (One test case gives a template and a mode; the template is called.)
 */
typedef struct sxml_xslt_test_invocation {
    /* CALL_TEMPLATE: empty means xsl:initial-template. */
    sxml_xslt_test_name initial_template;
    /* APPLY_TEMPLATES: a mode name, "#default" or "#unnamed"; empty
       means the default mode. */
    sxml_xslt_test_name initial_mode;
    /* APPLY_TEMPLATES: select= of initial-mode, the initial match
       selection as an XPath expression; empty for the global context
       item. */
    sxml_byte_view initial_match_selection;
    sxml_xslt_test_name initial_function;
    /* The params of the initial template, mode or function: by name for
       templates and modes (tunnel ones marked), by position for a
       function. */
    sxml_xslt_test_parameter_view parameters;
    sxml_xslt_invocation_kind invocation_kind;
    bool has_initial_template;
    bool has_initial_mode;
    /* An initial-template element besides an initial-mode one. */
    bool has_ignored_initial_mode;
} sxml_xslt_test_invocation;

/* A test case, read with its environment resolved (glossary:
   xslt_test_case). */
typedef struct sxml_xslt_test_case {
    sxml_byte_view test_set_name;
    /* The suite path of the test-set file, which relative references of
       the test case resolve against. */
    sxml_byte_view test_set_file_path;
    /* The group of the test set: the directory under tests/ ("insn",
       "strm", ...; D95 leaves "strm" and "sandp" out of scope). */
    sxml_byte_view group_name;
    sxml_byte_view name;
    sxml_xslt_test_environment environment;
    /* The stylesheets and packages of <test>, in document order. */
    sxml_xslt_test_module_view modules;
    /* The params of <test>: stylesheet parameters, static or not. */
    sxml_xslt_test_parameter_view parameters;
    sxml_xslt_test_invocation invocation;
    /* <output> of <test>, else of the environment. */
    sxml_xslt_test_output output;
    /* <posture-and-sweep>: a streamability assessment, not a run. */
    bool has_posture_and_sweep;
    sxml_qt4_dependency_view test_set_dependencies;
    sxml_qt4_dependency_view dependencies;
    /* The <result> element in the test-set document. */
    sxml_node_index result;
} sxml_xslt_test_case;

/*
 * Reads test case request->test_case_index of the open test set into
 * *test_case. Returns SXML_STATUS_INVALID_ARGUMENT when no test set is
 * open, SXML_STATUS_INDEX_OUT_OF_RANGE for an index past its test cases,
 * SXML_STATUS_MALFORMED_INPUT, with the problem filled, for an unknown
 * environment, a test case without <test> or <result>, or a name with an
 * unbound prefix, and the allocator's failures; *test_case is then
 * untouched.
 */
SXML_NODISCARD sxml_status sxml_xslt_test_catalog_read_test_case(
    sxml_xslt_test_catalog *catalog, const sxml_qt4_test_case_request *request,
    sxml_xslt_test_case *test_case);

/*
 * Reads the suite file request names into memory from request->allocator,
 * which the caller releases it to (for running tests: stylesheets,
 * sources, expected results). Returns the failures of
 * sxml_cli_file_reader_read and the allocator's; an empty suite path is
 * SXML_STATUS_INPUT_OUTPUT_FAILED and a catalog that is not loaded
 * SXML_STATUS_INVALID_ARGUMENT. The path is kept in the test set's arena
 * until the next open_test_set.
 */
SXML_NODISCARD sxml_status sxml_xslt_test_catalog_read_file(
    sxml_xslt_test_catalog *catalog, const sxml_qt4_file_request *request,
    sxml_byte_view *contents);

/*
 * A QT4 catalog over the same suite files, never loaded: the file reader
 * of the QT4 resource table (resource_table.h) and assertions
 * (assertion.h) when they run XSLT test cases. NULL before a successful
 * sxml_xslt_test_catalog_load; owned by the catalog.
 */
sxml_qt4_catalog *
sxml_xslt_test_catalog_get_file_catalog(const sxml_xslt_test_catalog *catalog);

/* The catalog as a test catalog (selection.h), for the test selection of
   list and run. The catalog must outlive it. */
sxml_qt4_test_catalog
sxml_xslt_test_catalog_as_test_catalog(sxml_xslt_test_catalog *catalog);

/* The XSLT catalog a selection made with
   sxml_xslt_test_catalog_as_test_catalog walks. */
sxml_xslt_test_catalog *
sxml_xslt_test_selection_catalog(const sxml_qt4_selection *selection);
