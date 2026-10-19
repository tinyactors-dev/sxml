#pragma once

/*
 * Collations (F&O 4.0, section 5.2 "Collations"; docs/DECISIONS.md D97,
 * D116, D213). Internal to sxml; used by the string, sequence and
 * deep-equal parts of the function library through the resolver of
 * src/function_library.c, by the XQuery static checks, and by
 * sxml_collation_is_supported (include/sxml/xpath.h).
 *
 * sxml knows four collations:
 * - the Unicode codepoint collation (section 5.2.3);
 * - the Unicode case-insensitive collation (section 5.2.5): codepoints of
 *   fn:lower-case;
 * - the HTML ASCII case-insensitive collation (section 5.2.6): codepoints
 *   with A-Z read as a-z;
 * - UCA collation URIs (section 5.2.4). With the UCA collation feature
 *   (sxml_uca_collation_feature, src/collation_uca.c, packet 7.15a, D213)
 *   and its collation element table (glossary `collation_element_table`,
 *   the DUCET the host serves as feature data), a UCA URI is UTS #10 over
 *   that table with the parameters strength, alternate, maxVariable,
 *   backwards, caseLevel, caseFirst, numeric and normalization; lang
 *   (other than the root collation, en and und) and reorder are tailorings
 *   (glossary `tailoring`, packet 7.15b). Without the feature or its data,
 *   the UCA fallback (D97, simple-uca-fallback): the levels of UTS #10 over
 *   weights sxml derives from Unicode properties. A codepoint's full
 *   compatibility decomposition gives its collation elements: each base
 *   character a primary weight (its lower-case codepoint, so the primary
 *   order is codepoint order without case), each combining mark (combining
 *   class above 0) a secondary weight, case and compatibility variants a
 *   tertiary weight; strength, alternate, maxVariable, caseFirst,
 *   caseLevel and backwards are honoured, the other parameters ignored.
 *   A URI with fallback=no is supported only when the table is there and
 *   every parameter can be honoured (FOCH0002 otherwise).
 *
 * Every collation is computed from the collation elements of a string
 * (sxml_collation_element_array): collation keys and substring matching
 * (section 5.2.2), with collation units as UTS #10 defines them, work on
 * them the same way for every collation.
 *
 * Nothing here recurses (D71). The functions are safe to call from any
 * thread, except that keys and working memory belong to the thread of
 * their allocator, and sxml_collation_resolve to the processor's (D35).
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
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
    /* A UCA URI without the collation element table. */
    SXML_COLLATION_KIND_UCA_FALLBACK,
    /* A UCA URI over the UCA collation feature's table
       (sxml_collation_resolve sets it). */
    SXML_COLLATION_KIND_UCA,
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
   (each group includes the ones before it; UTS #35, part 5, section
   3.6). */
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

/* The UCA caseFirst parameter (UTS #35, part 5, section 3.14). */
typedef enum sxml_collation_case_first {
    /* The table's tertiary weights as they are. */
    SXML_COLLATION_CASE_FIRST_OFF,
    SXML_COLLATION_CASE_FIRST_LOWER,
    SXML_COLLATION_CASE_FIRST_UPPER,
} sxml_collation_case_first;

/* src/collation_uca.c: a collation element table the UCA collation
   feature keeps in a processor. */
typedef struct sxml_collation_element_table sxml_collation_element_table;

/* The UCA collation feature's operations (below). */
typedef struct sxml_uca_collation_operations sxml_uca_collation_operations;

/*
 * A collation. A plain value: copy it freely. The fields after
 * collation_kind matter only for the UCA kinds; make one with
 * sxml_collation_find or sxml_collation_resolve, or use
 * sxml_collation_codepoint().
 */
typedef struct sxml_collation {
    sxml_collation_kind collation_kind;
    sxml_collation_strength strength;
    sxml_collation_alternate alternate;
    sxml_collation_variable_group maximum_variable;
    sxml_collation_case_first case_first;
    /* caseLevel=yes: a level of case alone after the secondary one. */
    bool has_case_level;
    /* backwards=yes: the secondary level compares from the end. */
    bool is_backwards;
    /* numeric=yes: runs of decimal digits weigh by their value. */
    bool is_numeric;
    /* fallback=yes, the default (F&O 4.0, section 5.2.4). */
    bool is_fallback_allowed;
    /* A parameter sxml cannot honour: an unknown keyword or value, a lang
       that needs a tailoring, reorder, or a version the table is not. */
    bool has_unsupported_parameter;
    /* The version parameter, borrowed from the URI; empty when absent. */
    sxml_byte_view version;
    /* SXML_COLLATION_KIND_UCA: the feature's operations and the table, in
       the processor's feature state (valid while the processor lives);
       NULL otherwise. */
    const sxml_uca_collation_operations *uca_operations;
    const sxml_collation_element_table *element_table;
} sxml_collation;

/* The Unicode codepoint collation. */
sxml_collation sxml_collation_codepoint(void);

/* The URI of the Unicode codepoint collation (F&O 4.0, section 5.2.3). */
sxml_byte_view sxml_collation_codepoint_uri(void);

/*
 * Writes the collation collation_uri names to *collation and returns true;
 * returns false, leaving *collation untouched, for a URI sxml does not
 * know: an unknown one, a relative one, and a UCA URI with a fallback
 * value other than yes and no. A UCA URI comes back as
 * SXML_COLLATION_KIND_UCA_FALLBACK, its parameters read; with fallback=no
 * it may be served only through sxml_collation_resolve. Compares bytes;
 * the collation borrows collation_uri.
 */
bool sxml_collation_find(sxml_byte_view collation_uri, sxml_collation *collation);

/* What sxml_collation_resolve looks up. */
typedef struct sxml_collation_resolution_request {
    /* The processor whose UCA collation feature serves UCA URIs; NULL for
       none (the fallback only). */
    sxml_processor *processor;
    /* Absolute; borrowed by the collation found. */
    sxml_byte_view collation_uri;
} sxml_collation_resolution_request;

/* What sxml_collation_resolve found. */
typedef struct sxml_collation_resolution {
    bool is_supported;
    sxml_collation collation;
} sxml_collation_resolution;

/*
 * Finds the collation request->collation_uri names, as a processor serves
 * it (F&O 4.0, section 5.2.4): sxml_collation_find, then for a UCA URI the
 * processor's UCA collation feature, which reads its table at the first
 * use. A UCA URI is SXML_COLLATION_KIND_UCA when the table is there; with
 * fallback=no it is supported only then, and only when every parameter
 * can be honoured. Writes is_supported false for anything else sxml does
 * not support. Returns the failures of reading the table
 * (SXML_STATUS_MALFORMED_INPUT for data that is not a collation element
 * table, the resolver's and the allocator's); *resolution then says not
 * supported.
 */
SXML_NODISCARD sxml_status sxml_collation_resolve(
    const sxml_collation_resolution_request *request, sxml_collation_resolution *resolution);

/*
 * True when collation orders and matches strings exactly as the Unicode
 * codepoint collation does, so callers may compare bytes.
 */
bool sxml_collation_is_codepoint_order(const sxml_collation *collation);

/* ---------------------------------------------------- Collation elements */

/* The levels of a sort key, in the order they are compared. */
typedef enum sxml_collation_level {
    SXML_COLLATION_LEVEL_PRIMARY,
    SXML_COLLATION_LEVEL_SECONDARY,
    /* caseLevel=yes: the case of each element with a primary weight (UTS
       #35, part 5, section 3.14). */
    SXML_COLLATION_LEVEL_CASE,
    SXML_COLLATION_LEVEL_TERTIARY,
    SXML_COLLATION_LEVEL_QUATERNARY,
    /* The codepoints themselves (UTS #10, section 7.4, "Identical
       Level"). */
    SXML_COLLATION_LEVEL_IDENTICAL,
} sxml_collation_level;

/* The levels an element has weights for: all but the identical one. */
#define SXML_COLLATION_ELEMENT_LEVEL_COUNT 5U

/*
 * One collation element of a string (glossary `collation_element`), with
 * the parameters of its collation applied: a weight below 2^24 for each
 * level up to the quaternary, zero where it is ignorable, and the bytes of
 * the input it came from. Elements of one codepoint, contraction or digit
 * run share byte_offset; substring matches start and end only between
 * such groups.
 */
typedef struct sxml_collation_element {
    uint32_t weights[SXML_COLLATION_ELEMENT_LEVEL_COUNT];
    size_t byte_offset;
    size_t byte_end;
} sxml_collation_element;

/* The collation elements of a string, in allocator memory; zero-initialize
   it with the allocator set and release it on every path. */
typedef struct sxml_collation_element_array {
    sxml_allocator allocator;
    sxml_collation_element *elements;
    size_t element_count;
    size_t element_capacity;
} sxml_collation_element_array;

/* Appends element. Returns the allocator's failures and
   SXML_STATUS_ARITHMETIC_OVERFLOW; the array is then unchanged. */
SXML_NODISCARD sxml_status sxml_collation_element_array_append(
    sxml_collation_element_array *array, const sxml_collation_element *element);

/* Releases the memory of array and leaves it empty. */
void sxml_collation_element_array_release(sxml_collation_element_array *array);

/* The elements to make of one string. */
typedef struct sxml_collation_element_request {
    const sxml_collation *collation;
    /* Borrowed for the call. */
    sxml_string input;
    /* The working memory. */
    sxml_allocator allocator;
} sxml_collation_element_request;

/* ------------------------------------------------ The UCA collation feature */

/*
 * The operations of sxml_uca_collation_feature (src/collation_uca.c;
 * SXML_OPTIONAL_FEATURE_KIND_UCA_COLLATION). The core reaches them only
 * through sxml_processor_find_optional_feature (D189) and through the
 * collations sxml_collation_resolve makes.
 */
struct sxml_uca_collation_operations {
    /*
     * Writes to *table the processor's collation element table, read at
     * the first call from the DUCET the resolver serves
     * (SXML_RESOURCE_KIND_COLLATION_DATA,
     * "http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt") and kept as
     * the feature's state; NULL when the resolver has no such data (then
     * asked again at the next call). Returns SXML_STATUS_MALFORMED_INPUT
     * for data that is not in the format of UTS #10, section 9.1, and the
     * failures of sxml_optional_feature_read_data and the allocator.
     */
    sxml_status (*find_table)(sxml_processor *processor,
                              const sxml_collation_element_table **table);
    /* The table's @version line ("16.0.0"), borrowed from the table. */
    sxml_byte_view (*get_version)(const sxml_collation_element_table *table);
    /*
     * Appends the collation elements of request->input under
     * request->collation (SXML_COLLATION_KIND_UCA) to *elements (UTS #10,
     * sections 4 to 7, with the parameters of UTS #35, part 5, section
     * 3): normalization to NFD, contractions (also discontiguous ones),
     * expansions, implicit weights, variable weighting, case and numeric
     * ordering. Returns the allocator's failures and
     * SXML_STATUS_ARITHMETIC_OVERFLOW.
     */
    sxml_status (*append_elements)(const sxml_collation_element_request *request,
                                   sxml_collation_element_array *elements);
};

/* ------------------------------------------------------- Collation keys */

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
 * held; the key's allocator is also the working memory. Returns the
 * allocator's failures and SXML_STATUS_ARITHMETIC_OVERFLOW; *key is then
 * empty.
 */
SXML_NODISCARD sxml_status sxml_collation_make_key(
    const sxml_collation_key_request *request, sxml_collation_key *key);

/* The bytes of key, borrowed from it. */
sxml_byte_view sxml_collation_key_view(const sxml_collation_key *key);

/* Releases the memory of key and leaves it empty. */
void sxml_collation_key_release(sxml_collation_key *key);

/* Two strings to compare under a collation. */
typedef struct sxml_collation_comparison {
    const sxml_collation *collation;
    /* The working memory of the collations other than the codepoint one. */
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

/* ------------------------------------------------------------- Matching */

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
