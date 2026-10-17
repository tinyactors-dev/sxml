#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "document.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * The result assertions of XSLT 4.0 test cases (glossary:
 * assertion_verdict; third_party/xslt40-test/admin/catalog-schema.xsd,
 * the substitution group abstractAssertion), surveyed before a run.
 *
 * The XSLT suite has the QT4 assertions with these differences, which the
 * checks keep when the transformation runs (D117):
 * - assert: the XPath expression is evaluated with the document node of
 *   the principal result as the context item (not with $result), the
 *   namespaces in scope on the element, the default element namespace
 *   none; with <output result-var="NAME"> the raw result is also bound
 *   to $NAME;
 * - assert-xml, assert-string-value, assert-eq and the others compare the
 *   principal result as QT4 compares $result;
 * - assert-message: one xsl:message, taken as a document, satisfies the
 *   contained assertion;
 * - assert-result-document uri=: the result document of that URI,
 *   resolved against the base output URI, satisfies the contained
 *   assertion (include/sxml/xslt.h, the result document collector);
 * - assert-serialization, serialization-matches and
 *   assert-serialization-error: on the serialized principal result;
 * - assert-warning and assert-posture-and-sweep are not checked.
 *
 * all-of, any-of and not combine as for QT4; assert-message and
 * assert-result-document hold one assertion each. The survey walks the
 * whole tree with an explicit stack (D71) and says which kinds occur.
 */

typedef enum sxml_xslt_test_assertion_kind {
    SXML_XSLT_TEST_ASSERTION_KIND_ALL_OF,
    SXML_XSLT_TEST_ASSERTION_KIND_ANY_OF,
    SXML_XSLT_TEST_ASSERTION_KIND_NOT,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_EQ,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_DEEP_EQ,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_PERMUTATION,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_COUNT,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_EMPTY,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_TRUE,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_FALSE,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_TYPE,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_STRING_VALUE,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_XML,
    SXML_XSLT_TEST_ASSERTION_KIND_ERROR,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_SERIALIZATION,
    SXML_XSLT_TEST_ASSERTION_KIND_SERIALIZATION_MATCHES,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_SERIALIZATION_ERROR,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_MESSAGE,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_RESULT_DOCUMENT,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_WARNING,
    SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_POSTURE_AND_SWEEP,
} sxml_xslt_test_assertion_kind;

/* The number of kinds, for tables of counts. */
enum { SXML_XSLT_TEST_ASSERTION_KIND_COUNT = 22 };

/* The deepest nesting of combinations, assert-message and
   assert-result-document. */
enum { SXML_XSLT_TEST_MAXIMUM_ASSERTION_DEPTH = 32 };

/* The element name of a kind, such as "assert-result-document". */
sxml_byte_view
sxml_xslt_test_assertion_kind_name(sxml_xslt_test_assertion_kind kind);

/* False for assert-warning and assert-posture-and-sweep, which the driver
   does not check. */
bool sxml_xslt_test_assertion_kind_is_checked(sxml_xslt_test_assertion_kind kind);

/* What the assertions under one <result> are. */
typedef struct sxml_xslt_test_assertion_survey {
    /* How often each kind occurs, by sxml_xslt_test_assertion_kind. */
    size_t kind_counts[SXML_XSLT_TEST_ASSERTION_KIND_COUNT];
    /* The first kind that is not checked; meaningful when
       has_unchecked_kind. */
    sxml_xslt_test_assertion_kind first_unchecked_kind;
    bool has_unchecked_kind;
} sxml_xslt_test_assertion_survey;

/* The <result> element to survey. */
typedef struct sxml_xslt_test_assertion_request {
    const sxml_qt4_document *document;
    sxml_node_index result_element;
    /* The suite path of the document, for problems. */
    sxml_byte_view test_set_file_path;
    /* Filled on failure; may be NULL. */
    sxml_qt4_problem *problem;
} sxml_xslt_test_assertion_request;

/*
 * Surveys the assertions under request->result_element into *survey.
 * Returns SXML_STATUS_MALFORMED_INPUT, with the problem filled, for an
 * element that is not an assertion, a result or a single-assertion
 * wrapper (not, assert-message, assert-result-document) without exactly
 * one assertion, or a combination without any;
 * SXML_STATUS_LIMIT_EXCEEDED for nesting deeper than
 * SXML_XSLT_TEST_MAXIMUM_ASSERTION_DEPTH; and the tree's failures.
 * *survey is then untouched.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_survey_assertions(const sxml_xslt_test_assertion_request *request,
                                 sxml_xslt_test_assertion_survey *survey);
