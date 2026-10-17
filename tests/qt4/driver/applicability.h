#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "environment.h"
#include "sxml/view.h"

/*
 * Applicability of QT4 test cases (glossary: applicability;
 * docs/QT4_CATALOG.md, "Dependencies"; third_party/qt4tests/
 * dependency_meanings.xml).
 *
 * A test case runs when
 * 1. its spec matches the target: the test case's spec dependencies
 *    replace the test set's, every spec value must have a matching token,
 *    and no spec at all means every version of both languages (the rule of
 *    tools/check-test-list.py, D13);
 * 2. every other dependency, of the test set and of the test case, has
 *    the satisfaction it asks for: a dependency is satisfied when one of
 *    its space-separated values is in the driver's table of what sxml
 *    supports; satisfied="false" asks for the opposite;
 * 3. its environment and modules ask only for what sxml can provide.
 * The first rule that fails is the reason the test is skipped.
 *
 * Separately, a test case is in scope when it counts in the denominator
 * of the pass-rate targets (D95): its spec matches the target, its test
 * set is not a non-goal (upd-*, ft-*, bin-*, expath-*, misc-HtmlTestSuite,
 * fn-parse-html, fn-html-doc, fn-invisible-xml, fn-transform) and none of
 * its dependencies asks for a non-goal feature (schemaImport,
 * schemaValidation, typedData, staticTyping, XQUpdate, fullText*,
 * xpath-1.0-compatibility, fn-transform-XSLT*, fn-invisible-xml,
 * remote_http, expath-file, binary, advanced-uca-fallback,
 * schema-location-hint). Scope does not decide whether a test runs: a
 * non-goal test case that is applicable still runs, so that the pass
 * count of earlier runs stays comparable.
 */

typedef enum sxml_qt4_language {
    /* "XP" tokens. */
    SXML_QT4_LANGUAGE_XPATH,
    /* "XQ" tokens. */
    SXML_QT4_LANGUAGE_XQUERY,
} sxml_qt4_language;

/* What a run targets (glossary: qt4_target), such as XP40. */
typedef struct sxml_qt4_target {
    sxml_qt4_language language;
    /* The version as the tokens write it: 40 for 4.0. */
    uint32_t version;
} sxml_qt4_target;

/*
 * True when a spec token such as "XP31+" or "XQ40" admits target: the same
 * language, and the same version or, with a trailing "+", a version at or
 * below the target's. Malformed tokens admit nothing.
 */
bool sxml_qt4_spec_token_matches(sxml_byte_view token, sxml_qt4_target target);

/* One <dependency>. Views point into the document it was read from. */
typedef struct sxml_qt4_dependency {
    sxml_byte_view dependency_type;
    sxml_byte_view value;
    /* satisfied="false" makes this false: the test runs only when the
       dependency is not satisfied. */
    bool requires_satisfaction;
} sxml_qt4_dependency;

typedef struct sxml_qt4_dependency_view {
    const sxml_qt4_dependency *dependencies;
    size_t dependency_count;
} sxml_qt4_dependency_view;

/*
 * True when sxml satisfies dependency_type with one of the space-separated
 * tokens of value (the table in applicability.c). Unknown types and values
 * are not satisfied.
 */
typedef struct sxml_qt4_dependency_value {
    sxml_byte_view dependency_type;
    sxml_byte_view value;
} sxml_qt4_dependency_value;

bool sxml_qt4_dependency_is_supported(sxml_qt4_dependency_value dependency);

/* What decides whether one test case runs. Borrowed for the call. */
typedef struct sxml_qt4_applicability_request {
    sxml_qt4_target target;
    sxml_qt4_dependency_view test_set_dependencies;
    sxml_qt4_dependency_view test_case_dependencies;
    const sxml_qt4_environment *environment;
    /* <module> elements of the test case (XQuery library modules). */
    size_t module_count;
    /* The name of the test set, for the non-goal test sets of D95. */
    sxml_byte_view test_set_name;
} sxml_qt4_applicability_request;

/* The answer. Views point into static text or the request's data. */
typedef struct sxml_qt4_applicability {
    bool is_applicable;
    /* Why not: "spec", "feature" or another dependency type, or
       "environment" or "module". Empty when applicable. */
    sxml_byte_view reason;
    /* The spec values, dependency value or environment element concerned;
       for "spec", the first failing value. Empty when applicable. */
    sxml_byte_view detail;
    /* For a dependency: true when it failed because it is satisfied and
       satisfied="false" asked otherwise. */
    bool is_inverted;
    /* In the D95 denominator (see the top of this header). */
    bool is_in_scope;
    /* When the spec matches but the test case is out of scope: the
       non-goal, "test-set" or "feature", and the test set's name or the
       feature. Empty otherwise. */
    sxml_byte_view non_goal_kind;
    sxml_byte_view non_goal;
} sxml_qt4_applicability;

/*
 * True when a dependency value names only non-goal features of D95 (each
 * space-separated token is one), so that requiring it puts a test case
 * out of scope.
 */
bool sxml_qt4_feature_is_non_goal(sxml_byte_view value);

/* True when a test set is a non-goal of D95. */
bool sxml_qt4_test_set_is_non_goal(sxml_byte_view test_set_name);

/* Decides as described at the top of this header. */
void sxml_qt4_decide_applicability(
    const sxml_qt4_applicability_request *request,
    sxml_qt4_applicability *applicability);
