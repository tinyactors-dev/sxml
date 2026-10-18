#pragma once

#include <stdbool.h>

#include "sxml/status.h"
#include "sxml/view.h"

/*
 * Package versions for the test resolver (XSLT 4.0, 3.4.1 "Versions of a
 * Package"; packet 4.16, D160).
 */

/* A version and a range (borrowed). */
typedef struct sxml_xslt_test_package_version_query {
    sxml_byte_view package_version;
    sxml_byte_view version_range;
} sxml_xslt_test_package_version_query;

/* Whether the version matches the range. */
typedef struct sxml_xslt_test_package_version_match {
    bool matches;
} sxml_xslt_test_package_version_match;

/* Writes whether the version matches the range ("*", lists of versions,
   "V.*", "V+", "to V", "V to W") to *match; SXML_STATUS_INVALID_ARGUMENT
   when either breaks the grammar. */
SXML_NODISCARD sxml_status sxml_xslt_test_package_version_matches(
    sxml_xslt_test_package_version_query query, sxml_xslt_test_package_version_match *match);

/* Two versions to order. */
typedef struct sxml_xslt_test_package_version_pair {
    sxml_byte_view first;
    sxml_byte_view second;
} sxml_xslt_test_package_version_pair;

/* -1, 0 or 1 as first is lower, equal or higher (3.4.1); a version that
   breaks the grammar is lower than every valid one. */
int sxml_xslt_test_compare_package_versions(sxml_xslt_test_package_version_pair pair);

/* An attribute of a package's xsl:package element, read from its text
   (for catalog <package> entries without uri=, whose name is in the
   file). */
typedef struct sxml_xslt_test_package_attribute_query {
    sxml_byte_view module_text;
    sxml_byte_view attribute_name;
} sxml_xslt_test_package_attribute_query;

/* The attribute's value as written (trimmed); empty when the document
   element is no xsl:package or has no such attribute. */
sxml_byte_view sxml_xslt_test_package_attribute(sxml_xslt_test_package_attribute_query query);
