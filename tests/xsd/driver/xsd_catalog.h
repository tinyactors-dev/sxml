#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "catalog.h"
#include "document.h"
#include "selection.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * The XSD 1.1 test catalog (glossary: xsd_test_catalog, xsd_test_set,
 * test_group, xsd_test_case; third_party/xsdtests/common/xsts.xsd):
 * suite.xml with its test-set references, and one open test set at a
 * time, whose schema tests and instance tests are read with the documents
 * they name.
 *
 * Imitates the XSLT 4.0 catalog (tests/xslt/driver/xslt_catalog.h) and has
 * its life: initialize or create; sxml_xsd_test_catalog_load reads
 * suite.xml; then, any number of times, sxml_xsd_test_catalog_open_test_set
 * and sxml_xsd_test_catalog_read_test_case. Everything a test case holds is
 * valid until the next open_test_set or until the catalog is finalized.
 * Test-set entries, test-set requests, test-case requests and file
 * requests are the QT4 driver's types; files are read with its documents
 * (document.h) in the suite's namespace.
 *
 * Names. suite.xml names test sets only by file (xlink:href), and
 * testSet/@name repeats across contributors ("Assert" of Saxonica and of
 * IBM), so a test set is named after its file: the directory without its
 * "Meta" suffix, "-", the file name without its extension
 * ("saxonMeta/Assert.testSet" is "saxon-Assert", "msMeta/DataTypes_w3c.xml"
 * is "ms-DataTypes_w3c"); the part before "-" is the contributor area of
 * the reports. A test case is named "GROUP/TEST" by its test group and its
 * own name, unique within a test set (xsts.xsd, uniqueGroupName and
 * uniqueTestName). Every href is a relative reference resolved against the
 * file that holds it into a suite path (suite_path.h), relative to the
 * directory of suite.xml.
 *
 * Thread safety: one thread at a time.
 */

/* The namespace of every element of suite.xml and the test-set files. */
#define SXML_XSD_TEST_SUITE_NAMESPACE_URI                                      \
    "http://www.w3.org/XML/2004/xml-schema-test-suite/"

/* The namespace of the href attributes. */
#define SXML_XSD_TEST_XLINK_NAMESPACE_URI "http://www.w3.org/1999/xlink"

typedef struct sxml_xsd_test_catalog sxml_xsd_test_catalog;

/* As the QT4 catalog's: the allocator, the file reader and the path of
   suite.xml, such as "third_party/xsdtests/suite.xml". */
typedef sxml_qt4_catalog_configuration sxml_xsd_test_catalog_configuration;

/*
 * Writes the memory requirements of a catalog, without what it allocates
 * as it loads files, to *requirements. Returns SXML_STATUS_INVALID_ARGUMENT
 * for the null allocator or an empty catalog path.
 */
SXML_NODISCARD sxml_status sxml_xsd_test_catalog_memory_requirements(
    const sxml_xsd_test_catalog_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places an empty catalog in memory. Allocates nothing. Returns the
   failures of the memory requirements and of
   sxml_memory_view_check_requirements. */
SXML_NODISCARD sxml_status sxml_xsd_test_catalog_initialize(
    sxml_memory_view memory,
    const sxml_xsd_test_catalog_configuration *configuration,
    sxml_xsd_test_catalog **catalog);

/* Releases everything the catalog allocated. Accepts NULL. */
void sxml_xsd_test_catalog_finalize(sxml_xsd_test_catalog *catalog);

/* Allocates a catalog through allocator, which is stored and must outlive
   it, and initializes it. */
SXML_NODISCARD sxml_status sxml_xsd_test_catalog_create(
    sxml_allocator allocator,
    const sxml_xsd_test_catalog_configuration *configuration,
    sxml_xsd_test_catalog **catalog);

/* Finalizes a catalog made by create and releases its memory. Accepts
   NULL. */
void sxml_xsd_test_catalog_destroy(sxml_xsd_test_catalog *catalog);

/*
 * Reads suite.xml: its testSetRef elements, in order. Returns, with
 * *problem filled when it is not NULL, SXML_STATUS_INPUT_OUTPUT_FAILED
 * when the file cannot be read, the parser's failures,
 * SXML_STATUS_MALFORMED_INPUT when the document element is not a
 * testSuite or a testSetRef has no xlink:href, SXML_STATUS_INVALID_ARGUMENT
 * when the catalog is loaded already, and the allocator's failures. After
 * a failure the catalog can only be finalized.
 */
SXML_NODISCARD sxml_status sxml_xsd_test_catalog_load(
    sxml_xsd_test_catalog *catalog, sxml_qt4_problem *problem);

/* The number of test-set entries, 0 before a successful load. */
size_t
sxml_xsd_test_catalog_test_set_count(const sxml_xsd_test_catalog *catalog);

/* Writes entry test_set_index (its name as above and its suite path) to
   *entry. Returns SXML_STATUS_INDEX_OUT_OF_RANGE, leaving it untouched,
   when there is none. */
SXML_NODISCARD sxml_status sxml_xsd_test_catalog_get_test_set_entry(
    const sxml_xsd_test_catalog *catalog, size_t test_set_index,
    sxml_qt4_test_set_entry *entry);

/* Finds the first test-set entry called name. */
bool sxml_xsd_test_catalog_find_test_set(const sxml_xsd_test_catalog *catalog,
                                         sxml_byte_view name,
                                         size_t *test_set_index);

/*
 * Closes the open test set and opens the one request names: its file and
 * the list of its tests, group by group in document order. Returns
 * SXML_STATUS_INDEX_OUT_OF_RANGE for an index past the entries, the
 * failures of sxml_qt4_document_load, SXML_STATUS_MALFORMED_INPUT when
 * the document element is not a testSet or a test group or test has no
 * name, and the allocator's; no test set is open after a failure.
 */
SXML_NODISCARD sxml_status sxml_xsd_test_catalog_open_test_set(
    sxml_xsd_test_catalog *catalog, const sxml_qt4_test_set_request *request);

/* Writes the index of the open test set and returns true, or returns
   false when none is open. */
bool sxml_xsd_test_catalog_get_open_test_set(
    const sxml_xsd_test_catalog *catalog, size_t *test_set_index);

/* The number of test cases of the open test set; 0 when none is open. */
size_t
sxml_xsd_test_catalog_test_case_count(const sxml_xsd_test_catalog *catalog);

/* Finds the test case of the open test set called name ("GROUP/TEST"). */
bool sxml_xsd_test_catalog_find_test_case(const sxml_xsd_test_catalog *catalog,
                                          sxml_byte_view name,
                                          size_t *test_case_index);

/* What an XSD test checks (glossary: schema_test, instance_test). */
typedef enum sxml_xsd_test_kind {
    /* The schema documents compile into one schema. */
    SXML_XSD_TEST_KIND_SCHEMA,
    /* The instance document is valid against the group's schema. */
    SXML_XSD_TEST_KIND_INSTANCE,
} sxml_xsd_test_kind;

/* One <expected validity version> of a test. Views into the document. */
typedef struct sxml_xsd_test_expectation {
    sxml_byte_view validity;
    /* Version tokens, all of which a processor must support; empty for
       none. */
    sxml_byte_view version;
} sxml_xsd_test_expectation;

typedef struct sxml_xsd_test_expectation_view {
    const sxml_xsd_test_expectation *expectations;
    size_t expectation_count;
} sxml_xsd_test_expectation_view;

/* Suite paths of documents, in document order. */
typedef struct sxml_xsd_test_document_view {
    const sxml_byte_view *file_paths;
    size_t file_path_count;
} sxml_xsd_test_document_view;

/* A schema test or an instance test, read (glossary: xsd_test_case). */
typedef struct sxml_xsd_test_case {
    sxml_byte_view test_set_name;
    /* The suite path of the test-set file. */
    sxml_byte_view test_set_file_path;
    /* testSet/@contributor. */
    sxml_byte_view contributor;
    /* "GROUP/TEST". */
    sxml_byte_view name;
    sxml_byte_view group_name;
    sxml_xsd_test_kind kind;
    /* The version attributes of the testSet, the testGroup and the test;
       empty where there is none (glossary: version_token). */
    sxml_byte_view test_set_version;
    sxml_byte_view group_version;
    sxml_byte_view test_version;
    /* current/@status ("accepted", "queried", ...); empty without
       <current>. */
    sxml_byte_view status;
    sxml_xsd_test_expectation_view expectations;
    /* SCHEMA: its schemaDocuments. INSTANCE: its instanceDocument. */
    sxml_xsd_test_document_view documents;
    /* The schema an INSTANCE test is validated against: the schemaDocuments
       of its group's schema test, or none, the schema of the built-in
       components alone, when the group has no schema test (xsts.xsd,
       testGroup). For a SCHEMA test, its own documents. */
    bool has_schema_test;
    sxml_xsd_test_document_view schema_documents;
} sxml_xsd_test_case;

/*
 * Reads test case request->test_case_index of the open test set into
 * *test_case. Returns SXML_STATUS_INVALID_ARGUMENT when no test set is
 * open, SXML_STATUS_INDEX_OUT_OF_RANGE for an index past its test cases,
 * SXML_STATUS_MALFORMED_INPUT, with the problem filled, for a document
 * element without xlink:href or a test without a document, and the
 * allocator's failures; *test_case is then untouched.
 */
SXML_NODISCARD sxml_status sxml_xsd_test_catalog_read_test_case(
    sxml_xsd_test_catalog *catalog, const sxml_qt4_test_case_request *request,
    sxml_xsd_test_case *test_case);

/*
 * Reads the suite file request names into memory from request->allocator,
 * which the caller releases it to (the documents of a test). Returns the
 * failures of sxml_cli_file_reader_read and the allocator's; a catalog
 * that is not loaded gives SXML_STATUS_INVALID_ARGUMENT.
 */
SXML_NODISCARD sxml_status sxml_xsd_test_catalog_read_file(
    sxml_xsd_test_catalog *catalog, const sxml_qt4_file_request *request,
    sxml_byte_view *contents);

/* The catalog as a test catalog (selection.h). The catalog must outlive
   it. */
sxml_qt4_test_catalog
sxml_xsd_test_catalog_as_test_catalog(sxml_xsd_test_catalog *catalog);

/* The XSD catalog a selection made with
   sxml_xsd_test_catalog_as_test_catalog walks. */
sxml_xsd_test_catalog *
sxml_xsd_test_selection_catalog(const sxml_qt4_selection *selection);
