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
} sxml_qt4_applicability;

/* Decides as described at the top of this header. */
void sxml_qt4_decide_applicability(
    const sxml_qt4_applicability_request *request,
    sxml_qt4_applicability *applicability);
