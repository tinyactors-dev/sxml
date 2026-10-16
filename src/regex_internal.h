#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/string.h"
#include "sxml/view.h"

/*
 * Regular expressions of the F&O 4.0 dialect (F&O 4.0, section 6 "Regular
 * expressions": the XSD 1.1 syntax with the additions of section 6.1, and
 * the flags of section 6.2). Internal to sxml; used by fn:matches,
 * fn:replace, fn:tokenize, fn:analyze-string and fn:matching-segments.
 *
 * docs/DECISIONS.md D9, D48 and D53. This interface is the one the full
 * engine of D9 (a Phase 2 packet, after Saxon HE) implements unchanged;
 * src/regex.c is the Phase 1 seed: a backtracking matcher for a subset of
 * the dialect, which rejects everything outside the subset with FORX0002
 * rather than matching wrongly (D53 lists the subset).
 *
 * Three steps:
 * 1. sxml_regex_parse_flags turns the $flags string into sxml_regex_flags
 *    (FORX0001 for an unknown flag).
 * 2. A regex is compiled from regex_text and flags into an sxml_regex
 *    (FORX0002 for an invalid regex). A compiled regex is immutable: any
 *    number of matchers, on any threads, may use it at once, so a caller
 *    may compile a literal $pattern once and keep it.
 * 3. An sxml_regex_matcher holds the working state of matching: the
 *    backtrack stack, the captured groups of the last match and the step
 *    count. It is used by one thread at a time and reused for every search
 *    over every regex input.
 *
 * Positions and segments
 * ----------------------
 * F&O 4.0, section 6.1.1 numbers the character positions of a string of N
 * characters from 1 to N+1. Here a position is the byte offset of a
 * codepoint boundary in the UTF-8 regex input, from 0 to byte_count, and a
 * matching segment or captured group is an sxml_byte_range of the regex
 * input. A caller that needs codepoint positions counts them itself.
 *
 * Searching
 * ---------
 * - match_at tries one start position only: is there a matching segment
 *   that starts there (the "anchored" match)?
 * - find returns the first matching segment that starts at or after a
 *   position, trying each start position in turn.
 * - find_next continues after the last segment found, so that a loop of
 *   find (from 0) and find_next yields the disjoint matching segments of
 *   section 6.1.1, the input of fn:tokenize, fn:replace,
 *   fn:analyze-string and fn:matching-segments: after a segment that ends
 *   at E the next search starts at E (a zero-length segment at E is
 *   adjacent, not overlapping); after a zero-length segment at P it starts
 *   one codepoint after P, since only one segment may start at P.
 * Among the segments that start at one position, the one returned is the
 * one section 6.1.1 chooses: the first matching branch of a choice, the
 * longest repetition for a greedy quantifier, the shortest for a reluctant
 * one.
 *
 * Errors and limits
 * -----------------
 * A regex that is invalid, or uses syntax this build does not support, is
 * SXML_STATUS_DYNAMIC_ERROR with err:FORX0002 (flags: err:FORX0001) in the
 * error record the request or configuration points to; descriptions are
 * static text (D21). A search that runs past maximum_step_count, or whose
 * backtrack stack would pass maximum_backtrack_byte_count, is
 * SXML_STATUS_LIMIT_EXCEEDED: these bound the work and memory of untrusted
 * regexes (catastrophic backtracking) and are not specification errors.
 * Compiling bounds the instructions (counted repetition is expanded) and
 * the nesting of groups (the parser recurses), also with
 * SXML_STATUS_LIMIT_EXCEEDED.
 */

/* ----------------------------------------------------------------- Flags */

/*
 * The flags of F&O 4.0, section 6.2. A zero-initialized value is "no
 * flags". With is_literal, only is_case_insensitive still has an effect.
 */
typedef struct sxml_regex_flags {
    /* s: "." matches every character, newline and carriage return
       included. */
    bool is_dot_all;
    /* m: "^" and "$" match at the start and end of every line (section
       6.1.8.1). */
    bool is_multi_line;
    /* i: characters and ranges match their case variants too. */
    bool is_case_insensitive;
    /* x: whitespace outside character class expressions is removed
       before the regex is parsed. */
    bool is_whitespace_removed;
    /* q: every character of the regex stands for itself. */
    bool is_literal;
    /* c: "#" outside a character class expression starts a comment that
       runs to the next "#" or the end (section 6.1.2). */
    bool has_comments;
} sxml_regex_flags;

typedef struct sxml_regex_flags_request {
    /* The $flags argument. Borrowed for the call. */
    sxml_string flags_text;
    /* Receives err:FORX0001 on SXML_STATUS_DYNAMIC_ERROR; may be NULL. */
    sxml_error_record *error_record;
} sxml_regex_flags_request;

/*
 * Reads request->flags_text into *flags. Letters may repeat and appear in
 * any order; they are case-sensitive.
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR with err:FORX0001 for any other
 * character (F&O 4.0, section 6.2), leaving *flags untouched.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_NODISCARD sxml_status sxml_regex_parse_flags(
    const sxml_regex_flags_request *request, sxml_regex_flags *flags);

/* ---------------------------------------------------------------- Regex */

/* A compiled regular expression. Immutable once initialized. */
typedef struct sxml_regex sxml_regex;

/* What a configuration field set to 0 means. */
#define SXML_REGEX_DEFAULT_MAXIMUM_INSTRUCTION_COUNT 100000U
#define SXML_REGEX_DEFAULT_MAXIMUM_GROUP_DEPTH 100U
/* The largest maximum_instruction_count accepted, so that every count and
   slot number fits uint32_t. */
#define SXML_REGEX_MAXIMUM_INSTRUCTION_COUNT_LIMIT ((uint32_t)1 << 28U)

typedef struct sxml_regex_configuration {
    /* The regex. Borrowed for the call: the compiled regex keeps no
       reference to it. */
    sxml_string regex_text;
    sxml_regex_flags flags;
    /* Most instructions the compiled regex may have; counted repetition
       ({n,m}) is expanded, so this bounds x{1000}{1000}. 0 means
       SXML_REGEX_DEFAULT_MAXIMUM_INSTRUCTION_COUNT; at most
       SXML_REGEX_MAXIMUM_INSTRUCTION_COUNT_LIMIT. */
    uint32_t maximum_instruction_count;
    /* Deepest nesting of groups and of character class subtraction, so
       that compiling cannot exhaust the C stack. 0 means
       SXML_REGEX_DEFAULT_MAXIMUM_GROUP_DEPTH. */
    uint32_t maximum_group_depth;
    /* Receives err:FORX0002 on SXML_STATUS_DYNAMIC_ERROR; may be NULL. */
    sxml_error_record *error_record;
} sxml_regex_configuration;

/*
 * Compiles configuration->regex_text without storing anything and writes
 * the exact memory requirements of the compiled regex to *requirements.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for a maximum_instruction_count
 * above SXML_REGEX_MAXIMUM_INSTRUCTION_COUNT_LIMIT,
 * SXML_STATUS_DYNAMIC_ERROR (FORX0002) for an invalid or
 * unsupported regex, SXML_STATUS_LIMIT_EXCEEDED when a limit above is
 * reached, SXML_STATUS_ARITHMETIC_OVERFLOW when the size does not fit
 * size_t.
 */
SXML_NODISCARD sxml_status sxml_regex_memory_requirements(
    const sxml_regex_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Compiles configuration->regex_text into memory and writes the address of
 * the regex to *regex. memory must stay valid, unmoved, until
 * sxml_regex_finalize. Returns the failures of
 * sxml_regex_memory_requirements and sxml_memory_view_check_requirements.
 */
SXML_NODISCARD sxml_status sxml_regex_initialize(
    sxml_memory_view memory, const sxml_regex_configuration *configuration,
    sxml_regex **regex);

/* Ends the use of the regex's memory; accepts NULL. */
void sxml_regex_finalize(sxml_regex *regex);

/*
 * Allocates the memory of sxml_regex_memory_requirements through allocator
 * and compiles into it. allocator is stored and must outlive the regex.
 */
SXML_NODISCARD sxml_status sxml_regex_create(
    sxml_allocator allocator, const sxml_regex_configuration *configuration,
    sxml_regex **regex);

/* Finalizes a regex made by sxml_regex_create and frees it; accepts NULL. */
void sxml_regex_destroy(sxml_regex *regex);

/*
 * The number of capturing subexpressions (F&O 4.0, section 6.1.5); captured
 * groups are numbered 1 to this, and group 0 is the whole segment.
 */
uint32_t sxml_regex_group_count(const sxml_regex *regex);

/* The flags the regex was compiled with (fn:replace reads is_literal). */
sxml_regex_flags sxml_regex_get_flags(const sxml_regex *regex);

/* --------------------------------------------------------------- Matcher */

/* The working state of matching one regex. */
typedef struct sxml_regex_matcher sxml_regex_matcher;

#define SXML_REGEX_DEFAULT_INITIAL_BACKTRACK_ENTRY_COUNT 64U
#define SXML_REGEX_DEFAULT_MAXIMUM_BACKTRACK_BYTE_COUNT ((size_t)64 << 20U)
#define SXML_REGEX_DEFAULT_MAXIMUM_STEP_COUNT ((uint64_t)100000000U)

typedef struct sxml_regex_matcher_configuration {
    /* Borrowed: must outlive the matcher. */
    const sxml_regex *regex;
    /* Grows the backtrack stack beyond its initial entries; the null
       allocator means it never grows and a search that needs more gives
       SXML_STATUS_CAPACITY_EXCEEDED. Must outlive the matcher. */
    sxml_allocator backtrack_allocator;
    /* Backtrack entries placed with the matcher itself. 0 means
       SXML_REGEX_DEFAULT_INITIAL_BACKTRACK_ENTRY_COUNT. */
    size_t initial_backtrack_entry_count;
    /* Most bytes the backtrack stack may take from backtrack_allocator.
       0 means SXML_REGEX_DEFAULT_MAXIMUM_BACKTRACK_BYTE_COUNT. */
    size_t maximum_backtrack_byte_count;
    /* Most instructions one call of match_at, find or find_next may run.
       0 means SXML_REGEX_DEFAULT_MAXIMUM_STEP_COUNT. */
    uint64_t maximum_step_count;
} sxml_regex_matcher_configuration;

/*
 * Writes the memory requirements of a matcher (its captured groups and
 * initial backtrack entries) to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT when configuration->regex is NULL and
 * SXML_STATUS_ARITHMETIC_OVERFLOW when the size does not fit size_t.
 */
SXML_NODISCARD sxml_status sxml_regex_matcher_memory_requirements(
    const sxml_regex_matcher_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a matcher in memory and writes its address to *matcher. memory
 * must stay valid, unmoved, until sxml_regex_matcher_finalize. Returns the
 * failures of sxml_regex_matcher_memory_requirements and
 * sxml_memory_view_check_requirements.
 */
SXML_NODISCARD sxml_status sxml_regex_matcher_initialize(
    sxml_memory_view memory,
    const sxml_regex_matcher_configuration *configuration,
    sxml_regex_matcher **matcher);

/*
 * Releases the backtrack stack the matcher grew, never memory itself;
 * accepts NULL.
 */
void sxml_regex_matcher_finalize(sxml_regex_matcher *matcher);

/*
 * Allocates a matcher through allocator and initializes it. allocator is
 * stored and must outlive the matcher; it may also be the backtrack
 * allocator.
 */
SXML_NODISCARD sxml_status sxml_regex_matcher_create(
    sxml_allocator allocator,
    const sxml_regex_matcher_configuration *configuration,
    sxml_regex_matcher **matcher);

/* Finalizes a matcher made by sxml_regex_matcher_create and frees it;
   accepts NULL. */
void sxml_regex_matcher_destroy(sxml_regex_matcher *matcher);

typedef struct sxml_regex_search_request {
    /* The string to search. The matcher keeps a reference to its bytes
       until the next match_at or find, or until it is finalized; captured
       groups are ranges of it. */
    sxml_string regex_input;
    /* Where to start: a codepoint boundary of regex_input, at most its
       byte_count. */
    size_t start_byte_offset;
} sxml_regex_search_request;

/* What a search found. */
typedef struct sxml_regex_search_result {
    /* True when a matching segment was found; it is then the current
       match, whose captured groups describe it. */
    bool is_found;
    /* The matching segment (captured group 0); 0 and 0 when not
       is_found. */
    sxml_byte_range matching_segment;
} sxml_regex_search_result;

/*
 * Writes to *result whether a matching segment starts exactly at
 * request->start_byte_offset, and which.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for a start that is not a codepoint
 * boundary within regex_input, SXML_STATUS_LIMIT_EXCEEDED or
 * SXML_STATUS_CAPACITY_EXCEEDED as described above, and the failures of
 * the backtrack allocator; *result is then untouched and the matcher has
 * no match.
 */
SXML_NODISCARD sxml_status sxml_regex_matcher_match_at(
    sxml_regex_matcher *matcher, const sxml_regex_search_request *request,
    sxml_regex_search_result *result);

/*
 * Writes to *result the first matching segment that starts at or after
 * request->start_byte_offset, if any, and makes it the match.
 * Failures as sxml_regex_matcher_match_at; the step limit counts the
 * attempts at every start position together.
 */
SXML_NODISCARD sxml_status sxml_regex_matcher_find(
    sxml_regex_matcher *matcher, const sxml_regex_search_request *request,
    sxml_regex_search_result *result);

/*
 * Writes to *result the next disjoint matching segment in the regex input
 * of the last search, after its last match (see "Searching" above), if
 * any, and makes it the match. After a search that found nothing,
 * result->is_found is false.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when the matcher has not searched
 * since it was initialized; otherwise the failures of
 * sxml_regex_matcher_find.
 */
SXML_NODISCARD sxml_status sxml_regex_matcher_find_next(
    sxml_regex_matcher *matcher, sxml_regex_search_result *result);

/* A captured group of the current match. */
typedef struct sxml_regex_captured_group {
    /* The segment, a range of the regex input; 0 and 0 when not
       is_captured. */
    sxml_byte_range byte_range;
    /* False when the capturing subexpression took no part in the match,
       as in (a)|b matching "b". */
    bool is_captured;
} sxml_regex_captured_group;

/*
 * Writes captured group group_number of the current match to
 * *captured_group; group 0 is the matching segment itself (F&O 4.0,
 * section 6.1.5: the last substring a repeated subexpression matched).
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when the last search found no
 * match, and SXML_STATUS_INDEX_OUT_OF_RANGE when group_number is above
 * sxml_regex_group_count.
 */
SXML_NODISCARD sxml_status sxml_regex_matcher_get_captured_group(
    const sxml_regex_matcher *matcher, uint32_t group_number,
    sxml_regex_captured_group *captured_group);

/* ------------------------------------------------ Unicode character sets */

/*
 * Used by src/regex.c; defined in src/regex_unicode.c, which
 * tools/generate-regex-tables.py generates from the Unicode Character
 * Database (Unicode 16.0).
 */

/* An inclusive range of codepoints. */
typedef struct sxml_regex_codepoint_range {
    uint32_t first_codepoint;
    uint32_t last_codepoint;
} sxml_regex_codepoint_range;

/* Sorted, disjoint, non-adjacent ranges. */
typedef struct sxml_regex_codepoint_range_list {
    const sxml_regex_codepoint_range *ranges;
    size_t range_count;
} sxml_regex_codepoint_range_list;

/* True when codepoint is in \d: general category Nd. */
bool sxml_regex_is_decimal_digit(uint32_t codepoint);

/* True when codepoint is in \w: not in category P, Z or C (unassigned
   codepoints are Cn). */
bool sxml_regex_is_word_character(uint32_t codepoint);
