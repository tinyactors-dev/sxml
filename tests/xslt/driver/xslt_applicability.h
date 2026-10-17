#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "applicability.h"
#include "sxml/view.h"
#include "xslt_catalog.h"

/*
 * Applicability of XSLT 4.0 test cases (glossary: applicability,
 * qt4_scope, test_set_group; third_party/xslt40-test/admin/
 * catalog-schema.xsd, the substitution group "dependency"; D95). The answer is the QT4 driver's
 * sxml_qt4_applicability.
 *
 * A test case runs when
 * 1. its spec matches XSLT 4.0: the test case's <spec> dependencies
 *    replace the test set's, one of their space-separated tokens must
 *    match ("XSLT10+" to "XSLT40+", or "XSLT40"), and no spec at all means
 *    every version (as for QT4, D83);
 * 2. every other dependency of the test set and the test case has the
 *    satisfaction it asks for: a dependency is satisfied when its whole
 *    value is in the driver's table of what sxml provides as a basic
 *    XSLT 4.0 processor with serialization and higher-order functions
 *    (xslt_applicability.c; values such as "support negative year" are
 *    phrases, not token lists); satisfied="false" asks for the opposite;
 * 3. its environment asks only for what sxml can provide: no source
 *    validation, no schema, no XInclude, no XML 1.1 source, no collation
 *    other than codepoint, HTML ASCII and UCA ones (D97);
 * 4. it runs a transformation: <posture-and-sweep> tests ask for a
 *    streamability assessment instead.
 * The first rule that fails is the reason the test is skipped.
 *
 * In scope (the D95 denominator): the spec matches, the test set's group
 * is neither "strm" nor "sandp", and no dependency asks for one of the
 * non-goal features schema_aware, streaming, dtd, XML_1.1, XSD_1.1,
 * backwards_compatibility, HTML4, ICU_spellout to be satisfied (asking for
 * one to be absent keeps the test case in scope). As for QT4, scope does
 * not decide whether a test runs.
 */

/* XSLT 4.0 as the spec tokens write it. */
enum { SXML_XSLT_TEST_TARGET_VERSION = 40 };

/*
 * True when a spec token such as "XSLT30+" or "XSLT40" admits XSLT
 * version (40 for 4.0): the same version or, with a trailing "+", a
 * version at or below it. Malformed tokens admit nothing.
 */
typedef struct sxml_xslt_test_spec_token {
    sxml_byte_view token;
    uint32_t version;
} sxml_xslt_test_spec_token;

bool sxml_xslt_test_spec_token_matches(sxml_xslt_test_spec_token spec_token);

/* True when sxml satisfies the dependency (the table in
   xslt_applicability.c). Unknown types and values are not satisfied. */
bool sxml_xslt_test_dependency_is_supported(
    sxml_qt4_dependency_value dependency);

/* True when a feature value is one of D95's non-goals for XSLT. */
bool sxml_xslt_test_feature_is_non_goal(sxml_byte_view value);

/* True when a test-set group is out of D95's scope ("strm", "sandp"). */
bool sxml_xslt_test_group_is_non_goal(sxml_byte_view group_name);

/* Decides for XSLT 4.0, as described at the top of this header. */
void sxml_xslt_test_decide_applicability(
    const sxml_xslt_test_case *test_case,
    sxml_qt4_applicability *applicability);
