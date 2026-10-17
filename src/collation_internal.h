#pragma once

/*
 * Collations (F&O 4.0, section 5.2 "Collations"; docs/DECISIONS.md D97,
 * D116). Internal to sxml; used by the string, sequence and deep-equal
 * parts of the function library through the resolver of
 * src/function_library.c, and by sxml_collation_is_supported
 * (include/sxml/xpath.h).
 *
 * sxml knows four collations:
 * - the Unicode codepoint collation (section 5.2.3);
 * - the Unicode case-insensitive collation (section 5.2.5): codepoints of
 *   fn:lower-case;
 * - the HTML ASCII case-insensitive collation (section 5.2.6): codepoints
 *   with A-Z read as a-z;
 * - UCA collation URIs (section 5.2.4) with fallback (D97,
 *   simple-uca-fallback): no DUCET tables, but the levels of UTS #10 over
 *   weights sxml derives from Unicode properties. A codepoint's full
 *   compatibility decomposition gives its collation elements: each base
 *   character a primary weight (its lower-case codepoint, so the primary
 *   order is codepoint order without case), each combining mark (combining
 *   class above 0) a secondary weight, case and compatibility variants a
 *   tertiary weight. Strength, alternate (shifted, blanked), maxVariable,
 *   caseFirst, caseLevel and backwards are honoured; lang, version,
 *   normalization, numeric and reorder are accepted and ignored. A URI
 *   with fallback=no is not supported (FOCH0002), since sxml does not
 *   claim to conform to UCA.
 *
 * Every collation here supports collation keys and substring matching
 * (section 5.2.2), with collation units as UTS #10 defines them over these
 * weights.
 *
 * Nothing here recurses (D71) or keeps state between calls; every
 * function is safe to call from any thread, except that keys and working
 * memory belong to the thread of their allocator.
 */
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/string.h"
#include "sxml/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The collation family a URI names. */
typedef enum sxml_collation_kind {
    SXML_COLLATION_KIND_CODEPOINT,
    SXML_COLLATION_KIND_UNICODE_CASE_INSENSITIVE,
    SXML_COLLATION_KIND_HTML_ASCII_CASE_INSENSITIVE,
    SXML_COLLATION_KIND_UCA_FALLBACK,
} sxml_collation_kind;

/* The UCA strength parameter (UTS #10, section 3; F&O 4.0, section
   5.2.4): how many levels are compared. */
typedef enum sxml_collation_strength {
    SXML_COLLATION_STRENGTH_PRIMARY = 1,
    SXML_COLLATION_STRENGTH_SECONDARY = 2,
    SXML_COLLATION_STRENGTH_TERTIARY = 3,
    SXML_COLLATION_STRENGTH_QUATERNARY = 4,
    SXML_COLLATION_STRENGTH_IDENTICAL = 5,
} sxml_collation_strength;

/* The UCA alternate parameter: how variable characters weigh. */
typedef enum sxml_collation_alternate {
    SXML_COLLATION_ALTERNATE_NON_IGNORABLE,
    /* Ignored on levels 1 to 3, compared on level 4. */
    SXML_COLLATION_ALTERNATE_SHIFTED,
    /* Ignored but on the identical level. */
    SXML_COLLATION_ALTERNATE_BLANKED,
} sxml_collation_alternate;

/* The UCA maxVariable parameter: the last group of variable characters
   (each group includes the ones before it). */
typedef enum sxml_collation_variable_group {
    /* Whitespace: general category Z and U+0009 to U+000D. */
    SXML_COLLATION_VARIABLE_GROUP_SPACE,
    /* Punctuation: general category P. */
    SXML_COLLATION_VARIABLE_GROUP_PUNCTUATION,
    /* Symbols other than currency: Sm, Sk, So. */
    SXML_COLLATION_VARIABLE_GROUP_SYMBOL,
    /* Currency symbols: Sc. */
    SXML_COLLATION_VARIABLE_GROUP_CURRENCY,
} sxml_collation_variable_group;

/*
 * A collation. A plain value: copy it freely. The fields after
 * collation_kind matter only for SXML_COLLATION_KIND_UCA_FALLBACK; make
 * one with sxml_collation_find, or use sxml_collation_codepoint().
 */
typedef struct sxml_collation {
    sxml_collation_kind collation_kind;
    sxml_collation_strength strength;
    sxml_collation_alternate alternate;
    sxml_collation_variable_group maximum_variable;
    /* caseLevel=yes: a level of case alone after the secondary one. */
    bool has_case_level;
    /* caseFirst=upper: upper case before lower case. */
    bool is_upper_first;
    /* backwards=yes: the secondary level compares from the end. */
    bool is_backwards;
} sxml_collation;

/* The Unicode codepoint collation. */
sxml_collation sxml_collation_codepoint(void);

/* The URI of the Unicode codepoint collation (F&O 4.0, section 5.2.3). */
sxml_byte_view sxml_collation_codepoint_uri(void);

/*
 * Writes the collation collation_uri names to *collation and returns true;
 * returns false, leaving *collation untouched, for a URI sxml does not
 * support: one it does not know, a relative one, and a UCA URI with
 * fallback=no or a fallback value other than yes and no. Compares bytes.
 */
bool sxml_collation_find(sxml_byte_view collation_uri, sxml_collation *collation);

/*
 * True when collation orders and matches strings exactly as the Unicode
 * codepoint collation does, so callers may compare bytes.
 */
bool sxml_collation_is_codepoint_order(const sxml_collation *collation);

/* Two strings to compare under a collation. */
typedef struct sxml_collation_comparison {
    const sxml_collation *collation;
    /* The working memory of the UCA collations; unused for the others. */
    sxml_allocator allocator;
    sxml_string first;
    sxml_string second;
} sxml_collation_comparison;

/*
 * Orders comparison->first and comparison->second under the collation
 * (F&O 4.0, fn:compare) into *ordering. Returns the allocator's failures
 * and SXML_STATUS_ARITHMETIC_OVERFLOW; *ordering is then untouched.
 */
SXML_NODISCARD sxml_status sxml_collation_compare(
    const sxml_collation_comparison *comparison, sxml_ordering *ordering);

/*
 * A collation key being built or made (glossary `collation_key`): bytes
 * whose order, octet by octet with a proper prefix first, is the order of
 * the strings under the collation (F&O 4.0, fn:collation-key). Owned
 * through allocator; zero-initialize it with the allocator set, and
 * release it with sxml_collation_key_release on every path.
 */
typedef struct sxml_collation_key {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} sxml_collation_key;

/* What sxml_collation_make_key works on. */
typedef struct sxml_collation_key_request {
    const sxml_collation *collation;
    /* Borrowed for the call. */
    sxml_string input;
} sxml_collation_key_request;

/*
 * Writes the collation key of request->input to *key, replacing what it
 * held. Returns the allocator's failures and
 * SXML_STATUS_ARITHMETIC_OVERFLOW; *key is then empty.
 */
SXML_NODISCARD sxml_status sxml_collation_make_key(
    const sxml_collation_key_request *request, sxml_collation_key *key);

/* The bytes of key, borrowed from it. */
sxml_byte_view sxml_collation_key_view(const sxml_collation_key *key);

/* Releases the memory of key and leaves it empty. */
void sxml_collation_key_release(sxml_collation_key *key);

/* Where a substring match is looked for (F&O 4.0, section 5.4). */
typedef enum sxml_collation_match_kind {
    /* The leftmost match (fn:contains, fn:substring-before, -after). */
    SXML_COLLATION_MATCH_KIND_FIRST,
    /* The rightmost match (fn:substring-before-last, -after-last). */
    SXML_COLLATION_MATCH_KIND_LAST,
    /* A match at the start (fn:starts-with). */
    SXML_COLLATION_MATCH_KIND_START,
    /* A match at the end (fn:ends-with). */
    SXML_COLLATION_MATCH_KIND_END,
} sxml_collation_match_kind;

/* What sxml_collation_find_match looks for. */
typedef struct sxml_collation_match_request {
    const sxml_collation *collation;
    /* The working memory. */
    sxml_allocator allocator;
    /* Both borrowed for the call. */
    sxml_string input;
    sxml_string search;
    sxml_collation_match_kind match_kind;
} sxml_collation_match_request;

/* A match: the bytes of input it covers. */
typedef struct sxml_collation_match {
    bool is_found;
    size_t byte_offset;
    size_t byte_count;
} sxml_collation_match;

/*
 * Looks for a substring of request->input that is equal to
 * request->search under the collation and starts and ends at collation
 * unit boundaries (F&O 4.0, section 5.4, "minimal match"), and writes the
 * result to *match. A search of no collation units (the zero-length
 * string, or ignorable characters only) matches the empty range at the
 * start, or at the end for SXML_COLLATION_MATCH_KIND_LAST and _END.
 * Returns the allocator's failures and SXML_STATUS_ARITHMETIC_OVERFLOW;
 * *match is then untouched.
 */
SXML_NODISCARD sxml_status sxml_collation_find_match(
    const sxml_collation_match_request *request, sxml_collation_match *match);
