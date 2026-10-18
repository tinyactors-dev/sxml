#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "catalog.h"
#include "document.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

/*
 * QT4 result assertions (glossary: assertion_verdict; docs/QT4_CATALOG.md,
 * "Result assertions"; third_party/qt4tests/catalog-schema.xsd, the
 * substitution group abstractAssertion).
 *
 * The assertion under a test case's <result> is checked against what the
 * test's evaluation gave: a value or a raised error. Assertions that
 * compare values are XPath expressions evaluated with the processor that
 * evaluated the test, with $result bound to the value and the test's
 * namespaces and static base URI in scope:
 *
 * - assert-eq: one atomic item equal to the expected value under eq, an
 *   xs:untypedAtomic result cast as by "=", NaN equal to NaN;
 * - assert-deep-eq: fn:deep-equal; assert-permutation: some permutation
 *   is deep-equal (NaN equal to NaN);
 * - assert: the effective boolean value of the expression;
 * - assert-true, assert-false: exactly that xs:boolean, not an effective
 *   boolean value; assert-empty; assert-count;
 * - assert-type: "$result instance of" the sequence type;
 * - assert-string-value: string-join($result ! string(), " ") equals the
 *   text, both normalized with normalize-space="true";
 * - assert-xml: the result written as the XML output method would
 *   (atomic items as text separated by spaces, nodes by the adaptive
 *   method, which writes nodes as the XML method does) and the expected
 *   XML, each parsed inside a wrapper element, are deep-equal with
 *   comments and processing instructions compared: a canonical
 *   comparison without full serialization (prefixes never matter);
 * - error: an error was raised with the code (any code for "*"); another
 *   code gives the verdict "wrong error", which passes (catalog-schema.xsd,
 *   element "error");
 * - all-of, any-of, not: combinations, to any depth, walked with an
 *   explicit stack (D71);
 * - serialization-matches: the value serialized with the evaluation's
 *   serialization parameters (sxml_serialize_sequence), as a string,
 *   matches the regular expression (the element's text or file) under
 *   fn:matches with the flags attribute; output that is not UTF-8 (an
 *   encoding parameter) fails;
 * - assert-serialization-error: an error with the code raised by the
 *   evaluation or by serializing its value, as "error" judges it;
 * - assert-serialization (only in comments of the suite) and the XSLT
 *   assertions: "unsupported".
 *
 * Verdicts combine from worst to best as fail, unsupported, wrong error,
 * pass: all-of takes the worst of its children, any-of the best, not
 * turns a pass or wrong error into a fail and a fail into a pass.
 */

/* From worst to best, so combining is a minimum or a maximum. */
typedef enum sxml_qt4_verdict_kind {
    SXML_QT4_VERDICT_FAIL,
    /* The driver cannot check this assertion. */
    SXML_QT4_VERDICT_UNSUPPORTED,
    /* An error was expected and raised, with another code. */
    SXML_QT4_VERDICT_WRONG_ERROR,
    SXML_QT4_VERDICT_PASS,
} sxml_qt4_verdict_kind;

typedef struct sxml_qt4_assertion_verdict {
    sxml_qt4_verdict_kind verdict_kind;
    /* Why, for every verdict but a pass: text in the request's arena or
       static text. */
    sxml_byte_view reason;
} sxml_qt4_assertion_verdict;

/* What the test's evaluation gave. Borrowed for the check. */
typedef struct sxml_qt4_evaluation_outcome {
    /* SXML_STATUS_OK for a value; a status with an error record
       (sxml_status_has_error_record) for a raised error. */
    sxml_status status;
    /* The value, for SXML_STATUS_OK. */
    sxml_sequence value;
    /* The raised error, for the other statuses. */
    const sxml_error_record *error_record;
    /* What the serialization assertions serialize the value with: the
       query's own parameters (XQuery's output declarations), else the XML
       method's defaults. */
    sxml_serialization_parameters serialization_parameters;
} sxml_qt4_evaluation_outcome;

/* The assertions to check and what they need. Borrowed for the call. */
typedef struct sxml_qt4_assertion_request {
    /* The processor that evaluated the test; it evaluates the assertions. */
    sxml_processor *processor;
    /* Namespaces and static base URI for the assertions' expressions; its
       variable declarations are not used. */
    sxml_static_context_configuration static_context;
    /* The test-set document and its <result> element. */
    const sxml_qt4_document *document;
    sxml_node_index result_element;
    sxml_qt4_evaluation_outcome evaluation;
    /* Reads assert-xml files, which are relative to test_set_file_path. */
    sxml_qt4_catalog *catalog;
    sxml_byte_view test_set_file_path;
    /* Temporary memory: file contents, serialized results. */
    sxml_allocator allocator;
    /* Receives the reasons and resolved paths; must outlive the verdict. */
    sxml_arena *arena;
    /* assert-string-value without normalize-space normalizes: the XSLT 4.0
       catalog's default (catalog-schema.xsd), false for QT4's. */
    bool normalizes_string_values_by_default;
} sxml_qt4_assertion_request;

/*
 * Checks the assertion under request->result_element and writes the
 * verdict to *verdict. A result element with no assertion, or more than
 * one, is a fail. Returns the failures that are not the assertion's own:
 * a limit or the allocator's failures while evaluating an assertion
 * (SXML_STATUS_LIMIT_EXCEEDED, SXML_STATUS_OUT_OF_MEMORY), the arena's,
 * the tree's, and SXML_STATUS_LIMIT_EXCEEDED for combinations nested more
 * than SXML_QT4_MAXIMUM_ASSERTION_DEPTH deep; *verdict is then untouched.
 * The assertion's own errors (an expected value that raises an error, a
 * type error comparing the values) make a fail.
 */
SXML_NODISCARD sxml_status
sxml_qt4_check_assertion(const sxml_qt4_assertion_request *request,
                         sxml_qt4_assertion_verdict *verdict);

/*
 * Checks one assertion element, request->result_element itself (a leaf or
 * a combination, whose local name is element_name), as
 * sxml_qt4_check_assertion checks the children of a <result>. For drivers
 * that walk assertions of their own around the QT4 ones (the XSLT 4.0
 * driver's assert-message and assert-result-document, D157). Returns the
 * failures sxml_qt4_check_assertion returns.
 */
SXML_NODISCARD sxml_status
sxml_qt4_check_assertion_element(const sxml_qt4_assertion_request *request,
                                 sxml_byte_view element_name,
                                 sxml_qt4_assertion_verdict *verdict);

/* The deepest nesting of all-of, any-of and not. */
enum { SXML_QT4_MAXIMUM_ASSERTION_DEPTH = 32 };
