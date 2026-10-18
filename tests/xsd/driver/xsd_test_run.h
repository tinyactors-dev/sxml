#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "test_run.h"
#include "xsd_applicability.h"
#include "xsd_catalog.h"

/*
 * Running one XSD 1.1 test (glossary: schema_test, instance_test,
 * test_outcome; include/sxml/schema.h; D165, D168).
 *
 * A test that does not apply (xsd_applicability.h) is not run, reason
 * "skip: REASON DETAIL". Every other runs in isolation, as a QT4 test
 * does (test_run.h): a processor of its own with the QT4 driver's limits
 * (SXML_QT4_TEST_MAXIMUM_STEP_COUNT evaluation steps, bounded entity
 * expansion), made through a test budget (budget.h) of
 * SXML_QT4_TEST_MAXIMUM_BYTE_COUNT live bytes that must be empty again
 * afterwards. In it:
 *
 * - every document of the test is read: the schema documents of the
 *   schema (for an instance test, its group's schema test's; none for a
 *   group without one, the schema of the built-in components alone) and,
 *   for an instance test, its instance document. A document that cannot
 *   be read is an error, "document SUITE-PATH: cannot read";
 * - a schema test compiles its documents into one schema
 *   (sxml_schema_create, each document SXML_SCHEMA_DOCUMENT_SOURCE_TEXT
 *   with document URI file:///SUITE-PATH, included and imported documents
 *   served as suite files by the resolver, a report sink counting
 *   errors): it passes when the schema compiles and valid was expected,
 *   or it does not and invalid was expected;
 * - an instance test compiles its group's schema the same way (a schema
 *   that does not compile makes the test fail, "schema: ...") and
 *   validates the instance document while parsing it
 *   (sxml_schema_validator in STRICT mode, the null result builder, a
 *   report sink counting errors): it passes when the validity is the
 *   expected one;
 * - a limit, running out of memory, a status that is neither a static
 *   nor a dynamic error, or a leak is an error.
 *
 * Until packets 5.4 (schema documents) and 5.7 (validation) implement
 * include/sxml/schema.h, the compilation and validation are not done:
 * an applicable test whose documents are read is not run, reason "XSD
 * not implemented", and counts in scope.
 */

/* One test to run. Borrowed for the call. */
typedef struct sxml_xsd_test_run_request {
    /* The catalog with the test's test set open; reads its files. */
    sxml_xsd_test_catalog *catalog;
    const sxml_xsd_test_case *test_case;
    /* Backs the test's budget. */
    sxml_allocator allocator;
    /* Receives the reason; must outlive the result. */
    sxml_arena *arena;
} sxml_xsd_test_run_request;

/*
 * Runs request->test_case and writes its outcome to *result. Returns only
 * the driver's own failures (the arena's, which the reason needs);
 * everything that goes wrong in the test is its outcome.
 */
SXML_NODISCARD sxml_status sxml_xsd_test_run_test_case(
    const sxml_xsd_test_run_request *request, sxml_qt4_test_result *result);
