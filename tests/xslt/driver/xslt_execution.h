#pragma once

#include "reason.h"
#include "test_run.h"
#include "xslt_assertion.h"
#include "xslt_test_run.h"

/*
 * Executing one planned XSLT 4.0 test case through include/sxml/xslt.h
 * (glossary: xslt_test_execution; D117, D157). Imitates the isolation of
 * tests/qt4/driver/test_run.c: a processor of its own made through an
 * allocator that holds at most SXML_QT4_TEST_MAXIMUM_BYTE_COUNT live bytes
 * and must have none left at the end ("leaked" otherwise), at most
 * SXML_QT4_TEST_MAXIMUM_STEP_COUNT evaluation steps.
 *
 * The plan (xslt_test_run.h) is applied as follows:
 * - the resolver is the QT4 resource table's (resource_table.h) over the
 *   catalog's suite files, with stylesheet modules asked for as
 *   XSLT_MODULE served as the suite files of their file:/// URIs and
 *   packages (XSLT_PACKAGE) as the <package> of the test or environment
 *   whose uri is the package name;
 * - sources are parsed (inline content or file; document URI and base URI
 *   their uri made absolute, or file:///SUITE-PATH); one with a uri is an
 *   available document; role "." is the global context item, or the item
 *   its select gives;
 * - params are evaluated as XPath (no context item) and bound by name:
 *   static ones at compile time, others as stylesheet parameters; the
 *   params of the initial template or mode as template or tunnel
 *   parameters, of an initial function as its arguments by position; the
 *   select of initial-mode, evaluated with the global context item, is the
 *   initial match selection;
 * - the principal result is delivered as the plan says, except that a test
 *   with a serialization assertion and no <output> is serialized; a
 *   serialized result is also parsed back, as an XML document when it is
 *   well-formed and as an xs:string otherwise, for the other assertions;
 * - result documents go to a result document collector (values) and
 *   messages to a trace sink that keeps them.
 *
 * Outcomes as for QT4: pass, wrong error, fail; error for what is not the
 * test's (a limit, running out of memory, a leak, a source that cannot be
 * read); a transformation that stops at SXML_STATUS_UNSUPPORTED_FEATURE
 * is a fail.
 */

/* What executing a test case decided. */
typedef struct sxml_xslt_test_answer {
    sxml_qt4_test_outcome outcome;
    sxml_qt4_reason *reason;
} sxml_xslt_test_answer;

/* One planned test case to execute. Borrowed for the call. */
typedef struct sxml_xslt_test_execution_request {
    const sxml_xslt_test_run_request *run;
    const sxml_xslt_test_plan *plan;
    const sxml_xslt_test_assertion_survey *survey;
} sxml_xslt_test_execution_request;

/* Executes the test case and writes the outcome and its reason to
   *answer. */
void sxml_xslt_test_execute(const sxml_xslt_test_execution_request *request,
                            sxml_xslt_test_answer *answer);
