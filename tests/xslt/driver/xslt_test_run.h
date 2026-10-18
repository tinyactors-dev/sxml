#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "applicability.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xslt.h"
#include "test_run.h"
#include "xslt_assertion.h"
#include "xslt_catalog.h"

/*
 * Running one XSLT 4.0 test case (glossary: xslt_test_plan,
 * test_outcome; include/sxml/xslt.h; D117).
 *
 * A run first plans the transformation from the test case, without
 * reading any file: which module is the principal one, which modules the
 * resolver must serve, which parameters are static, how the stylesheet is
 * invoked and how its principal result is delivered. The plan is what a
 * transformation needs, field for field, from include/sxml/xslt.h:
 *
 * - the principal module: the first stylesheet or package of <test> whose
 *   role is not secondary, else the first such of the environment, else
 *   the source whose defines-stylesheet is true (its xml-stylesheet
 *   processing instruction names the stylesheet). A stylesheet becomes
 *   SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT with the module's bytes and
 *   static base URI file:///SUITE-PATH; a package becomes RESOLVED_PACKAGE
 *   with its uri and package-version;
 * - the secondary modules: every other stylesheet and package of <test>
 *   and the environment, served by the resolver (XSLT_MODULE requests for
 *   file:/// URIs of suite files, XSLT_PACKAGE requests by package name
 *   and version);
 * - static parameters: the params of <test> and the environment marked
 *   static, as sxml_xslt_stylesheet_configuration.static_parameters; the
 *   others are stylesheet parameters (the dynamic context's variable
 *   bindings), each select evaluated as XPath with the catalog element's
 *   namespaces;
 * - the global context item: the source with role "." (parsed with
 *   document URI and base URI its uri, or file:///SUITE-PATH), or the
 *   item its select gives, or none;
 * - the invocation (xslt_catalog.h) and its parameters, by name or, for a
 *   function, by position;
 * - the delivery: SERIALIZED for <output serialize="yes">, RAW for
 *   tree="no", TREE for tree="yes", DEFAULT otherwise; the base output
 *   URI file:///SUITE-PATH of output file=, else
 *   file:///TEST-SET-DIRECTORY/output/TEST-CASE/ so that every result
 *   document of a test has its own URI; result documents go to an
 *   sxml_xslt_result_document_collector for assert-result-document, and
 *   xsl:message to the trace sink for assert-message.
 *
 * Outcomes, as for QT4 (test_run.h): a test case that is not applicable
 * (xslt_applicability.h) is not run, reason "skip: ..."; one with an
 * assertion the driver does not check is not run, "unsupported: KIND";
 * one whose plan cannot be made is an error; one whose principal module
 * is embedded in a source is not run, "unsupported: embedded stylesheet";
 * every other is executed (xslt_execution.h, packet 4.15) in a processor
 * of its own with the QT4 driver's limits, and its assertions checked
 * (xslt_assertion.h).
 */

/* Where the principal module of a plan is. */
typedef enum sxml_xslt_test_principal_kind {
    /* A <stylesheet> or <package> of <test> or of the environment. */
    SXML_XSLT_TEST_PRINCIPAL_KIND_MODULE,
    /* A source whose xml-stylesheet processing instruction names it. */
    SXML_XSLT_TEST_PRINCIPAL_KIND_EMBEDDED,
} sxml_xslt_test_principal_kind;

/* The transformation of one test case, planned. Views and pointers are
   borrowed from the test case. */
typedef struct sxml_xslt_test_plan {
    sxml_xslt_test_principal_kind principal_kind;
    /* MODULE: the principal module. */
    const sxml_xslt_test_module *principal_module;
    /* EMBEDDED: the index of the source in the environment. */
    size_t principal_source_index;
    /* How the principal module is given to sxml_xslt_stylesheet_create. */
    sxml_xslt_principal_module_source principal_module_source;
    /* The modules the resolver serves besides the principal one. */
    size_t secondary_module_count;
    size_t static_parameter_count;
    size_t stylesheet_parameter_count;
    /* The source with role ".", when there is one. */
    bool has_global_context_item;
    size_t global_context_source_index;
    sxml_xslt_invocation_kind invocation_kind;
    sxml_xslt_result_delivery result_delivery;
} sxml_xslt_test_plan;

/*
 * Plans test_case as described above into *plan. Returns
 * SXML_STATUS_MALFORMED_INPUT, with the problem filled, when the test case
 * names no principal module; *plan is then untouched.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_make_plan(const sxml_xslt_test_case *test_case,
                         sxml_qt4_problem *problem, sxml_xslt_test_plan *plan);

/* The name of a delivery as listings write it: "default", "raw", "tree",
   "serialized". */
sxml_byte_view
sxml_xslt_test_delivery_name(sxml_xslt_result_delivery result_delivery);

/* The name of an invocation kind: "apply-templates", "call-template",
   "call-function". */
sxml_byte_view
sxml_xslt_test_invocation_name(sxml_xslt_invocation_kind invocation_kind);

/* One test case to run. Borrowed for the call. */
typedef struct sxml_xslt_test_run_request {
    /* The catalog with the test case's test set open; reads its files. */
    sxml_xslt_test_catalog *catalog;
    const sxml_xslt_test_case *test_case;
    /* Backs the test's own allocator. */
    sxml_allocator allocator;
    /* Receives the reason; must outlive the result. */
    sxml_arena *arena;
} sxml_xslt_test_run_request;

/*
 * Runs request->test_case and writes its outcome to *result. Returns only
 * the driver's own failures (the arena's, which the reason needs);
 * everything that goes wrong in the test is its outcome.
 */
SXML_NODISCARD sxml_status
sxml_xslt_test_run_test_case(const sxml_xslt_test_run_request *request,
                             sxml_qt4_test_result *result);
