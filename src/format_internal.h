#pragma once

/*
 * The picture engine for numbers (packet 2.9, docs/DECISIONS.md D97,
 * D104): pictures of fn:format-integer (F&O 4.0, section 4.8.1,
 * "fn:format-integer") and fn:format-number (F&O 4.0, section 4.9,
 * "Formatting numbers"), and decimal formats (section 4.9.1, "Defining a
 * decimal format"). Internal to sxml; used by
 * src/function_library_format.c and open to fn:format-date and its
 * relatives (packet 2.14), xsl:number and xsl:decimal-format (4.10), and
 * the XQuery decimal-format declaration (3.x).
 *
 * Picture strings are scanned by hand (D97): their active characters are
 * run-time properties of a decimal format. A parsed picture is a plain
 * value that borrows the picture string; formatting scans the borrowed
 * parts again for grouping separators, so parsing needs no memory.
 * Numbers reach the engine as decimal digit text, so xs:integer and
 * xs:decimal values of any size are formatted exactly, and xs:double and
 * xs:float values by their shortest decimal form (section 4.9.5, "the
 * value that is chosen should be one with the smallest possible number of
 * digits").
 *
 * Text is UTF-8 throughout and must be well-formed (strings of the item
 * heap are). Nothing here recurses (D71) or keeps state between calls.
 *
 * Thread safety: every function is safe to call from any thread; a
 * sxml_format_text belongs to the thread of its allocator.
 */

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xpath.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ Text */

/*
 * UTF-8 being built through an allocator: zero-initialize with an
 * allocator set, append, read the view, and call sxml_format_text_release
 * on every path.
 */
typedef struct sxml_format_text {
    /* Receives the bytes; not the null allocator. */
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} sxml_format_text;

/* Appends bytes (copied). Returns the allocator's failures and
   SXML_STATUS_ARITHMETIC_OVERFLOW; text is then unchanged. */
SXML_NODISCARD sxml_status sxml_format_text_append(sxml_format_text *text,
                                                   sxml_byte_view bytes);

/* Appends one codepoint (below 0x110000, not a surrogate) as UTF-8;
   failures as sxml_format_text_append. */
SXML_NODISCARD sxml_status sxml_format_text_append_codepoint(
    sxml_format_text *text, uint32_t codepoint);

/* The text built so far, borrowed from text. */
sxml_byte_view sxml_format_text_view(const sxml_format_text *text);

/* Releases the bytes of text and leaves it empty (the allocator stays). */
void sxml_format_text_release(sxml_format_text *text);

/* ------------------------------------------------------- Integer pictures */

/* How a primary format token numbers (F&O 4.0, section 4.8.1). */
typedef enum sxml_numbering_kind {
    /* A digit pattern, in radix 10 or in the radix before "^". */
    SXML_NUMBERING_KIND_DIGITS,
    /* The first letter of an alphabet ("a", "A", Greek alpha and Alpha):
       the letters as a bijective numeration, a b ... z aa ab ...; the
       alphabet is sequence_index. */
    SXML_NUMBERING_KIND_ALPHABETIC,
    /* The symbol for 1 of a sequence with one symbol per number, such as
       CIRCLED DIGIT ONE (U+2460, 0 to 50) or DIGIT ONE FULL STOP (U+2488,
       0 to 20): the symbols of the sequence's range; the sequence is
       sequence_index. */
    SXML_NUMBERING_KIND_SYMBOLS,
    /* "i" and "I": Roman numerals, 1 to 3999. */
    SXML_NUMBERING_KIND_LOWER_ROMAN,
    SXML_NUMBERING_KIND_UPPER_ROMAN,
    /* "w", "W" and "Ww": English words. */
    SXML_NUMBERING_KIND_LOWER_WORDS,
    SXML_NUMBERING_KIND_UPPER_WORDS,
    SXML_NUMBERING_KIND_TITLE_WORDS,
    /* Any other token: a numbering sequence sxml does not support, so the
       number is formatted with the token "1" (section 4.8.1: "it must use
       a format token of 1"). */
    SXML_NUMBERING_KIND_UNSUPPORTED,
} sxml_numbering_kind;

/*
 * A parsed fn:format-integer picture: a plain value that borrows the
 * picture string.
 */
typedef struct sxml_integer_picture {
    sxml_numbering_kind numbering_kind;
    /* SXML_NUMBERING_KIND_ALPHABETIC and SXML_NUMBERING_KIND_SYMBOLS: which
       alphabet or symbol sequence of src/format.c; 0 for the other kinds. */
    uint32_t sequence_index;
    /* SXML_NUMBERING_KIND_DIGITS: the digit pattern (after "R^" when a
       radix is given), borrowed; well-formed by the rules of section
       4.8.1. Empty for the other kinds. */
    sxml_byte_view digit_pattern;
    /* 2 to 36; 10 without "R^". */
    uint32_t radix;
    /* The digit 0 of the digit family: a codepoint of category Nd for
       radix 10, '0' with a radix (whose digits beyond 9 are letters). */
    uint32_t zero_digit;
    /* With a radix: true when the mandatory digit signs are "X", so digits
       beyond 9 are upper-case letters. */
    bool has_upper_case_digits;
    /* The "o" format modifier: ordinal numbering. */
    bool is_ordinal;
    /* The text in parentheses after "c" or "o", borrowed; empty when
       absent. sxml gives it no meaning (section 4.8.1 lets it). */
    sxml_byte_view numbering_variant;
} sxml_integer_picture;

/*
 * Parses picture_string, a fn:format-integer picture (F&O 4.0, section
 * 4.8.1): an optional radix and "^", the primary format token up to the
 * last semicolon, and the format modifier after it, which must match
 * ^([co](\(.+\))?)?[at]?$. Writes the result, borrowing picture_string,
 * to *picture.
 *
 * Returns SXML_STATUS_MALFORMED_INPUT when the picture breaks a rule of
 * the section (the caller raises FODF1310); *picture is then untouched.
 */
SXML_NODISCARD sxml_status sxml_integer_picture_parse(
    sxml_byte_view picture_string, sxml_integer_picture *picture);

/*
 * Parses a primary format token alone (with its optional "R^"), as
 * fn:format-date's components need (F&O 4.0, section 9.9.1.2): the
 * format modifier is taken as absent. Returns SXML_STATUS_MALFORMED_INPUT
 * as sxml_integer_picture_parse.
 */
SXML_NODISCARD sxml_status sxml_integer_picture_parse_primary_format_token(
    sxml_byte_view primary_format_token, sxml_integer_picture *picture);

/* --------------------------------------------------------- Number words */

/*
 * Words and ordinal suffixes of a language other than English: the
 * spell-out rules of the localization feature (src/language_table_
 * internal.h; packet 7.17, D221), which the callers of
 * sxml_format_integer find for their language and pass in
 * sxml_integer_formatting. Zero-initialized: English, sxml's own words.
 */
typedef struct sxml_number_words_request {
    /* The absolute value as ASCII digits without leading zeros; empty for
       zero. */
    sxml_byte_view magnitude_digits;
    /* LOWER_WORDS, UPPER_WORDS or TITLE_WORDS: the number in words, in
       that case. DIGITS: only the ordinal suffix written after the digits
       ("." in German, "e" in French). */
    sxml_numbering_kind numbering_kind;
    bool is_ordinal;
    /* The picture's text in parentheses after "c" or "o", borrowed: a
       rule set ("%spellout-ordinal-feminine") or an ending ("-er");
       empty when absent. */
    sxml_byte_view numbering_variant;
    /* Receives the words. */
    sxml_format_text *text;
} sxml_number_words_request;

typedef struct sxml_number_words_outcome {
    /* False when the language has no rule set for the request or the
       number is too large for its rules: the caller writes English and
       text is unchanged. */
    bool is_written;
} sxml_number_words_outcome;

typedef struct sxml_number_words_operations {
    /* Appends request's words to request->text. Returns the allocator's
       failures. */
    sxml_status (*append_words)(const void *state, const sxml_number_words_request *request,
                                sxml_number_words_outcome *outcome);
} sxml_number_words_operations;

/* The words of one language; operations NULL for English. */
typedef struct sxml_number_words {
    const void *state;
    const sxml_number_words_operations *operations;
} sxml_number_words;

/* An integer to format by a picture. */
typedef struct sxml_integer_formatting {
    /* Borrowed for the call. */
    const sxml_integer_picture *picture;
    /* The absolute value as ASCII decimal digits, without sign; leading
       zeros are allowed, and an empty view is 0. */
    sxml_byte_view magnitude_digits;
    /* The value is below zero: the minus sign "-" is written first. */
    bool is_negative;
    /* xsl:number's grouping (XSLT 4.0, 12.4, grouping-separator and
       grouping-size): when grouping_size is above 0, the digits of a
       decimal representation (a digit pattern or the fallback "1") are
       written with grouping_separator between groups of grouping_size
       digits, counted from the right, instead of the pattern's own
       grouping. Zero-initialized: no such grouping. */
    sxml_byte_view grouping_separator;
    size_t grouping_size;
    /* The words and ordinal suffixes of the language; zero for English. */
    sxml_number_words number_words;
} sxml_integer_formatting;

/*
 * Appends the integer formatted by its picture to text (F&O 4.0, section
 * 4.8.1): digits with grouping and padding, letters, symbol sequences
 * (within their ranges), Roman numerals (1 to 3999) or words (English
 * below 10^21, or the language's number_words), each falling back to the
 * token "1" outside its range, and ordinal suffixes or words for "o" (the
 * language's, else English). Uses text's allocator for scratch memory.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for magnitude digits that are not
 * ASCII digits, and the allocator's failures and
 * SXML_STATUS_ARITHMETIC_OVERFLOW; text may then hold part of the result.
 */
SXML_NODISCARD sxml_status sxml_format_integer(
    const sxml_integer_formatting *formatting, sxml_format_text *text);

/* ------------------------------------------- xsl:number format strings */

/*
 * The format attribute of xsl:number (XSLT 4.0, 12.4) splits into
 * alternating runs of alphanumeric characters (format tokens) and of other
 * characters: a run before the first format token is the prefix, a run
 * after the last one the suffix, and the runs between format tokens are
 * separators. Without format tokens the one other run is both prefix and
 * suffix and the format token is "1". The functions below rescan the
 * format string; they need no memory.
 */

/* What formats the number at a position of the place marker. */
typedef struct sxml_number_format_part {
    /* Written before the number when it is not the first: the separator
       before its format token, or "." when that is the first token. */
    sxml_byte_view separator;
    /* The format token: the token at the number's position, or the last
       token for numbers beyond them, or "1". */
    sxml_byte_view format_token;
} sxml_number_format_part;

/* A number of a place marker (0 for the first) whose part to find. */
typedef struct sxml_number_format_query {
    sxml_byte_view format;
    size_t number_index;
} sxml_number_format_query;

/* The part that formats the number at query.number_index; views borrow
   the format (or static data). */
sxml_number_format_part sxml_number_format_find_part(sxml_number_format_query query);

/* The prefix and the suffix of format (borrowed; empty when absent). */
typedef struct sxml_number_format_affixes {
    sxml_byte_view prefix;
    sxml_byte_view suffix;
} sxml_number_format_affixes;

sxml_number_format_affixes sxml_number_format_find_affixes(sxml_byte_view format);

/* -------------------------------------------------------- Decimal formats */

/* A property with marker and rendition (F&O 4.0, section 4.9.2: type
   xs:string (: matching '.|.:.*' :)). */
typedef struct sxml_decimal_format_marker {
    uint32_t marker;
    /* UTF-8, borrowed from the property's text (or static data). */
    sxml_byte_view rendition;
} sxml_decimal_format_marker;

/*
 * A decimal format with every property decoded (F&O 4.0, section 4.9.2,
 * table of options; glossary decimal_format_settings). A plain value
 * whose renditions borrow the texts it was made from.
 */
typedef struct sxml_decimal_format_settings {
    sxml_decimal_format_marker decimal_separator;
    sxml_decimal_format_marker grouping_separator;
    sxml_decimal_format_marker exponent_separator;
    sxml_decimal_format_marker percent;
    sxml_decimal_format_marker per_mille;
    /* Of category Nd with digit value 0. */
    uint32_t zero_digit;
    uint32_t digit;
    uint32_t pattern_separator;
    sxml_byte_view infinity;
    sxml_byte_view not_a_number;
    sxml_byte_view minus_sign;
} sxml_decimal_format_settings;

/* The defaults of the table in F&O 4.0, section 4.9.2 (".", ",", "e",
   "%", U+2030, "0", "#", ";", "Infinity", "NaN", "-"): the unnamed
   decimal format sxml starts with. */
sxml_decimal_format_settings sxml_decimal_format_default_settings(void);

/* Properties to apply to a decimal format. */
typedef struct sxml_decimal_format_change {
    /* Borrowed for the call. */
    const sxml_decimal_format_settings *base_settings;
    /* Each kind at most once; values borrowed by the result. */
    sxml_decimal_format_property_view decimal_format_properties;
} sxml_decimal_format_change;

/*
 * Writes base_settings with the properties applied to *settings, which
 * borrows the property texts. Checks every property's form (one
 * character; one character, ":" and any rendition; or any text for
 * infinity, NaN and minus-sign), that zero-digit is a character of
 * category Nd with digit value 0, and that the seven markers differ from
 * each other and from the ten digits of the family (F&O 4.0, section
 * 4.9.1).
 *
 * Returns SXML_STATUS_MALFORMED_INPUT when a check fails (fn:format-number
 * raises FODF1290; the XPath API reports INVALID_ARGUMENT, XQuery and
 * XSLT their own codes), and SXML_STATUS_INVALID_ARGUMENT for a kind
 * outside the enumeration; *settings is then untouched.
 */
SXML_NODISCARD sxml_status sxml_decimal_format_apply(
    const sxml_decimal_format_change *change,
    sxml_decimal_format_settings *settings);

/* -------------------------------------------------------- Number pictures */

/*
 * The variables of one sub-picture after analysis (F&O 4.0, section
 * 4.9.4, "Analyzing the picture string"). Views borrow the picture
 * string.
 */
typedef struct sxml_number_sub_picture {
    /* The passive characters before the leftmost and after the rightmost
       active character. */
    sxml_byte_view prefix;
    sxml_byte_view suffix;
    /* The active characters of the integer part and of the fractional
       part (digit signs and grouping separators), for the grouping
       positions. */
    sxml_byte_view integer_part;
    sxml_byte_view fraction_part;
    /* 1, or 100 with the percent marker, or 1000 with per-mille. */
    uint32_t multiplier;
    size_t minimum_integer_part_size;
    size_t scaling_factor;
    size_t minimum_fraction_part_size;
    size_t maximum_fraction_part_size;
    /* 0 when the sub-picture has no exponent separator sign. */
    size_t minimum_exponent_size;
    /* Grouping of the integer part: regular with grouping_size, or at the
       positions integer_part shows. */
    bool has_regular_grouping;
    size_t grouping_size;
} sxml_number_sub_picture;

/* Which sub-picture and variables apply (section 4.9.5). */
typedef enum sxml_number_sign {
    /* Positive numbers, xs:decimal zero and positive zero. */
    SXML_NUMBER_SIGN_POSITIVE,
    /* Negative numbers and negative zero. */
    SXML_NUMBER_SIGN_NEGATIVE,
} sxml_number_sign;

/* A parsed fn:format-number picture: a plain value that borrows the
   picture string and the settings' renditions. */
typedef struct sxml_number_picture {
    sxml_decimal_format_settings settings;
    sxml_number_sub_picture positive_sub_picture;
    sxml_number_sub_picture negative_sub_picture;
    /* False for a picture of one sub-picture: negative numbers then take
       its variables with the minus-sign rendition before the prefix. */
    bool has_negative_sub_picture;
} sxml_number_picture;

/* A picture string to parse with a decimal format. */
typedef struct sxml_number_picture_request {
    sxml_byte_view picture_string;
    /* Borrowed for the call; the picture copies the settings. */
    const sxml_decimal_format_settings *settings;
} sxml_number_picture_request;

/*
 * Parses request->picture_string with the decimal format's markers (F&O
 * 4.0, sections 4.9.3 and 4.9.4) and writes the sub-pictures' variables
 * to *picture, which borrows the picture string.
 *
 * Returns SXML_STATUS_MALFORMED_INPUT when the picture breaks a rule of
 * section 4.9.3 (the caller raises FODF1310); *picture is then untouched.
 */
SXML_NODISCARD sxml_status sxml_number_picture_parse(
    const sxml_number_picture_request *request, sxml_number_picture *picture);

/* The multiplier of the sub-picture for sign: 1, 100 or 1000. The caller
   multiplies xs:double and xs:float values in their own type (section
   4.9.5: the adjusted number). */
uint32_t sxml_number_picture_get_multiplier(const sxml_number_picture *picture,
                                            sxml_number_sign sign);

/* What kind of number is formatted. */
typedef enum sxml_number_class {
    SXML_NUMBER_CLASS_FINITE,
    SXML_NUMBER_CLASS_INFINITE,
    SXML_NUMBER_CLASS_NOT_A_NUMBER,
} sxml_number_class;

/* A number to format by a picture. */
typedef struct sxml_number_formatting {
    /* Borrowed for the call. */
    const sxml_number_picture *picture;
    sxml_number_class number_class;
    sxml_number_sign sign;
    /* SXML_NUMBER_CLASS_FINITE: the absolute value as ASCII decimal text,
       digits with at most one ".", without sign or exponent (the
       canonical form of xs:decimal, for example). */
    sxml_byte_view magnitude_text;
    /* Decimal places the point moves right before formatting: 2 or 3 for
       an xs:decimal under percent or per-mille, 0 otherwise. */
    uint32_t decimal_point_shift;
} sxml_number_formatting;

/*
 * Appends the number formatted by its picture to text (F&O 4.0, section
 * 4.9.5, "Formatting the number"): NaN as the NaN rendition, infinity
 * between prefix and suffix, and finite numbers scaled to mantissa and
 * exponent when the sub-picture has an exponent, rounded half to even to
 * the maximum fractional part size, padded, grouped and written in the
 * digit family with the renditions of the decimal format. Uses text's
 * allocator for scratch memory.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for magnitude text that is not
 * decimal digits, and the allocator's failures and
 * SXML_STATUS_ARITHMETIC_OVERFLOW; text may then hold part of the result.
 */
SXML_NODISCARD sxml_status sxml_format_number(
    const sxml_number_formatting *formatting, sxml_format_text *text);
