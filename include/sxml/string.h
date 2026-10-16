#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * UTF-8 strings (docs/DECISIONS.md, D6 and D25).
 *
 * An sxml_string is a byte view known to hold well-formed UTF-8, together with
 * its codepoint count and a flag for ASCII-only content. Both are computed in
 * the same pass that validates the bytes, so codepoint indexing into an
 * ASCII-only string is O(1) and the count is never computed twice.
 *
 * XPath strings are sequences of codepoints (XDM 4.0, "xs:string"; F&O 4.0,
 * section 5 "Functions on strings"), so lengths, positions and comparisons
 * here are in codepoints. Comparison is by Unicode codepoint, which is the
 * default collation (F&O 4.0, section 5.3.2 "The Unicode codepoint
 * collation").
 */

/*
 * A well-formed UTF-8 string. A plain value: copy it freely. It borrows its
 * bytes, which must outlive every copy.
 *
 * Make one with sxml_string_from_utf8. A zero-initialized sxml_string is the
 * empty string. The functions below assume utf8 is well-formed; given
 * bytes that are not, they still read only within utf8, but their results
 * are unspecified.
 */
typedef struct sxml_string {
    sxml_byte_view utf8;
    /* Number of codepoints in utf8. */
    size_t codepoint_count;
    /* True when every byte is below 0x80, so codepoint_count equals
       utf8.byte_count. False makes no claim: functions stay correct, only
       slower. */
    bool is_ascii;
} sxml_string;

/*
 * What validating bytes as UTF-8 found. On success it describes the whole
 * input; on SXML_STATUS_INVALID_UTF8 it describes the longest well-formed
 * prefix, so valid_byte_count is the offset of the first byte of the first
 * ill-formed sequence.
 */
typedef struct sxml_utf8_validation {
    size_t valid_byte_count;
    size_t codepoint_count;
    bool is_ascii;
} sxml_utf8_validation;

/*
 * Checks that utf8 is well-formed UTF-8 (Unicode 16.0, section 3.9, D92 and
 * table 3-7; RFC 3629): it rejects overlong forms, surrogates (U+D800 to
 * U+DFFF), codepoints above U+10FFFF and truncated sequences.
 *
 * Returns SXML_STATUS_OK or SXML_STATUS_INVALID_UTF8, and fills *validation
 * in both cases (an exception to D20, stated in sxml_utf8_validation).
 *
 * Thread safety: safe to call from any thread.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_utf8_validate(
    sxml_byte_view utf8, sxml_utf8_validation *validation);

/*
 * Validates utf8 and writes a string over it to *string. The string borrows
 * utf8.bytes.
 *
 * Returns SXML_STATUS_INVALID_UTF8, leaving *string untouched, when utf8 is
 * not well-formed; use sxml_utf8_validate to find where.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_string_from_utf8(
    sxml_byte_view utf8, sxml_string *string);

/*
 * Writes the codepoint at codepoint_index (counting from 0) to *codepoint.
 * O(1) when string.is_ascii, otherwise O(codepoint_index).
 *
 * Returns SXML_STATUS_INDEX_OUT_OF_RANGE, leaving *codepoint untouched, when
 * codepoint_index is not below string.codepoint_count.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_string_codepoint_at(
    sxml_string string, size_t codepoint_index, uint32_t *codepoint);

/*
 * Walks the codepoints of a string in order. A plain value; start one with
 *
 *     sxml_codepoint_iterator iterator = {.string = string, .byte_offset = 0};
 *
 * byte_offset is the offset of the next codepoint to read.
 */
typedef struct sxml_codepoint_iterator {
    sxml_string string;
    size_t byte_offset;
} sxml_codepoint_iterator;

/*
 * Reads the next codepoint into *codepoint and advances iterator (in/out).
 * Returns false, leaving *codepoint untouched, when no codepoint is left.
 *
 * Thread safety: safe to call from any thread for distinct iterators.
 */
SXML_EXPORT bool sxml_codepoint_iterator_next(
    sxml_codepoint_iterator *iterator, uint32_t *codepoint);

/* The result of a three-way comparison. */
typedef enum sxml_ordering {
    SXML_ORDERING_LESS = -1,
    SXML_ORDERING_EQUAL = 0,
    SXML_ORDERING_GREATER = 1,
} sxml_ordering;

/*
 * Compares first and second codepoint by codepoint; a proper prefix is less
 * than the longer string. This is the Unicode codepoint collation (F&O 4.0,
 * section 5.3.2).
 *
 * Thread safety: safe to call from any thread.
 */
SXML_EXPORT sxml_ordering sxml_string_compare(sxml_string first,
                                              sxml_string second);

/*
 * True when first and second hold the same codepoints.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_EXPORT bool sxml_string_equals(sxml_string first, sxml_string second);
