#pragma once

#include <stdbool.h>

#include "applicability.h"
#include "sxml/view.h"
#include "xsd_catalog.h"

/*
 * Whether an XSD test runs, and what it must give (glossary:
 * version_token, expected_validity; third_party/xsdtests/common/xsts.xsd,
 * "version-info" and "expected"; D165, D168).
 *
 * The driver is an XSD 1.1 processor that supports these version tokens,
 * one per token class of xsts.xsd's known-token: 1.1 (known-xsd-version),
 * XML-1.0 and XML-1.0-5e (xml-substrate: XSD 1.1 takes XML 1.0 fifth
 * edition), Unicode_6.0.0, CTR-all-compile (runtime-schema-error: faulty
 * restrictions are found by looking at the schema), full-xpath-in-CTA
 * and comments-and-PIs-excluded (XDM-filtering: the default); no XSD 1.0
 * edition token. A token of no known class is a class of its own, which
 * the driver does not support.
 *
 * A test applies when its test set, its test group and the test itself
 * each apply: a version attribute applies when it is absent or when, for
 * every token class it names, it names a supported token ("if a processor
 * configuration supports any of them, the tests included are
 * applicable", per element; D168 reads it per element, not over the union
 * of the three attributes as D165 counted). It is in scope (the gate's
 * denominator) when it applies, its current status is not queried,
 * disputed-test or disputed-spec, and the expected element that applies
 * prescribes valid or invalid: the first whose version tokens are all
 * supported ("if a processor configuration supports all of them, the
 * result given is prescribed"), else the first without a version. The
 * introspection test set (common/introspection.testSet) checks the suite's
 * own metadata and is not run (D165).
 *
 * Every applicable test is in scope: the XSD suite has no non-goals of its
 * own, so the answer's is_in_scope is is_applicable.
 */

/* What a test must give. */
typedef enum sxml_xsd_test_validity {
    SXML_XSD_TEST_VALIDITY_VALID,
    SXML_XSD_TEST_VALIDITY_INVALID,
} sxml_xsd_test_validity;

/* True when a version attribute's value applies (see above); an empty
   value applies. */
bool sxml_xsd_test_version_applies(sxml_byte_view version);

/* True when the driver supports every token of an expected element's
   version (an empty value: true). */
bool sxml_xsd_test_version_is_supported(sxml_byte_view version);

/* The decision about one test. */
typedef struct sxml_xsd_test_decision {
    /* is_applicable and is_in_scope are equal; reason "version",
       "status", "expected" or "test set" and its detail (the version
       attribute, the status, the validity, the test set) when not. */
    sxml_qt4_applicability applicability;
    /* When applicable: what the test must give. */
    sxml_xsd_test_validity expected_validity;
    /* The validity attribute of the expected element chosen; empty when
       none applies. */
    sxml_byte_view expected_text;
} sxml_xsd_test_decision;

/* Decides as described above. Views point into static text or the test
   case. */
void sxml_xsd_test_decide(const sxml_xsd_test_case *test_case,
                          sxml_xsd_test_decision *decision);
