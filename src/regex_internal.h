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
 * docs/DECISIONS.md D9, D48, D53 and D105. src/regex.c implements the
 * whole dialect of F&O 4.0: the XSD 1.1 syntax (character class
 * subtraction, category and block escapes, \i and \c), back-references
 * (numbered and \k<name>), reluctant quantifiers, non-capturing and named
 * groups, the assertions ^ $ \b \B, lookahead and lookbehind (both
 * spellings), and the flags s m i x q c, with case-blind matching by the
 * Unicode case mappings of fn:lower-case and fn:upper-case.
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
 * A regex that is invalid is SXML_STATUS_DYNAMIC_ERROR with err:FORX0002 (flags: err:FORX0001) in the
 * error record the request or configuration points to; descriptions are
 * static text (D21). A search that runs past maximum_step_count, or whose
 * backtrack stack would pass maximum_backtrack_byte_count, is
 * SXML_STATUS_LIMIT_EXCEEDED: these bound the work and memory of untrusted
 * regexes (catastrophic backtracking) and are not specification errors.
 * Compiling bounds the instructions (a small counted repetition is
 * expanded into copies, a large one runs as a loop with a counter, D147)
 * and the nesting of groups, also with SXML_STATUS_LIMIT_EXCEEDED. A
 * quantity above 4294967295 counts as 4294967295. Nothing
 * recurses (docs/DECISIONS.md D71): the parser keeps the open groups on a
 * stack of its own, which needs memory from an allocator beyond
 * SXML_REGEX_INLINE_GROUP_DEPTH nested groups (D53).
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
    /* i: characters, ranges and back-references match their case
       variants too; category, block and multi-character escapes do
       not. */
    bool is_case_insensitive;
    /* x: whitespace outside character class expressions is removed
       before the regex is parsed. */
    bool is_whitespace_removed;
    /* q: every character of the regex stands for itself. */
    bool is_literal;
    /* c: "#" outside a character class expression starts a comment that
       runs to the next "#" or the end (section 6.1.2). */
    bool has_comments;
    /* Not a flag of F&O 4.0: the XSD dialect of pattern facets (XSD 1.1
       Part 2, 4.3.4 and appendix G; D53). The regex matches the whole
       input (implicitly anchored at both ends, so a search is match_at
       from 0), "^" and "$" are ordinary characters, and what the XSD
       grammar lacks is FORX0002: back-references, reluctant quantifiers,
       "(?" and "(*" groups and assertions, \b and \B. The other flags
       are ignored with it. */
    bool is_xsd_pattern;
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
/* Nested groups the compiler holds without stack_allocator. */
#define SXML_REGEX_INLINE_GROUP_DEPTH 16U

typedef struct sxml_regex_configuration {
    /* The regex. Borrowed for the call: the compiled regex keeps no
       reference to it. */
    sxml_string regex_text;
    sxml_regex_flags flags;
    /* Most instructions the compiled regex may have; counted repetition
       ({n,m}) is expanded while its copies add at most 256 instructions
       (D147), so this bounds (?:x{16}){16} in sequence. 0 means
       SXML_REGEX_DEFAULT_MAXIMUM_INSTRUCTION_COUNT; at most
       SXML_REGEX_MAXIMUM_INSTRUCTION_COUNT_LIMIT. */
    uint32_t maximum_instruction_count;
    /* Deepest nesting of groups and of character class subtraction; it
       bounds the compiler's group stack, which grows through
       stack_allocator. 0 means SXML_REGEX_DEFAULT_MAXIMUM_GROUP_DEPTH. */
    uint32_t maximum_group_depth;
    /* Grows the compiler's group stack beyond SXML_REGEX_INLINE_GROUP_DEPTH
       nested groups; everything it gives is released before the call
       returns. The null allocator means deeper nesting gives
       SXML_STATUS_CAPACITY_EXCEEDED; sxml_regex_create uses its own
       allocator then. Borrowed for the call. */
    sxml_allocator stack_allocator;
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
 * reached, SXML_STATUS_CAPACITY_EXCEEDED for nesting deeper than
 * SXML_REGEX_INLINE_GROUP_DEPTH without a stack allocator, the failures
 * of the stack allocator, and SXML_STATUS_ARITHMETIC_OVERFLOW when the
 * size does not fit size_t.
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
 * and compiles into it. allocator is stored and must outlive the regex; it
 * also grows the group stack when configuration->stack_allocator is the
 * null allocator.
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

/*
 * The name of capturing group group_number when it is a named capturing
 * subexpression, (?<name>...) (F&O 4.0, section 6.1.5): a view of the
 * compiled regex's memory, valid as long as the regex. Empty for an
 * unnamed group and for a group_number that is not a group.
 */
sxml_byte_view sxml_regex_group_name(const sxml_regex *regex,
                                     uint32_t group_number);

/* The number of the capturing group named name; 0 when no group has that
   name (names are unique, section 6.1.5). */
uint32_t sxml_regex_find_named_group(const sxml_regex *regex,
                                     sxml_byte_view name);

/*
 * The number of the innermost capturing group whose parentheses enclose
 * those of group group_number ("A(B(C))": 2 for 3, 1 for 2); 0 for an
 * outermost group and for a group_number that is not a group. Groups
 * always have smaller numbers than the groups they enclose.
 */
uint32_t sxml_regex_group_parent(const sxml_regex *regex,
                                 uint32_t group_number);

/*
 * True when capturing group group_number lies within a lookahead
 * assertion (section 6.1.8.3): its captured group need not be part of the
 * matching segment, and fn:analyze-string writes it as
 * fn:lookahead-group. False for a group_number that is not a group.
 */
bool sxml_regex_group_is_in_lookahead(const sxml_regex *regex,
                                      uint32_t group_number);

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

/*
 * The instructions the last match_at, find or find_next ran (what
 * maximum_step_count bounds), so a caller can charge them to a larger
 * budget (D89, D91).
 */
uint64_t sxml_regex_matcher_get_step_count(const sxml_regex_matcher *matcher);

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

/* The general categories of the Unicode Character Database, in the order
   of tools/generate-regex-tables.py (XSD 1.1 Part 2, appendix G,
   "Category escapes"). */
typedef enum sxml_regex_general_category {
    SXML_REGEX_GENERAL_CATEGORY_LU,
    SXML_REGEX_GENERAL_CATEGORY_LL,
    SXML_REGEX_GENERAL_CATEGORY_LT,
    SXML_REGEX_GENERAL_CATEGORY_LM,
    SXML_REGEX_GENERAL_CATEGORY_LO,
    SXML_REGEX_GENERAL_CATEGORY_MN,
    SXML_REGEX_GENERAL_CATEGORY_MC,
    SXML_REGEX_GENERAL_CATEGORY_ME,
    SXML_REGEX_GENERAL_CATEGORY_ND,
    SXML_REGEX_GENERAL_CATEGORY_NL,
    SXML_REGEX_GENERAL_CATEGORY_NO,
    SXML_REGEX_GENERAL_CATEGORY_PC,
    SXML_REGEX_GENERAL_CATEGORY_PD,
    SXML_REGEX_GENERAL_CATEGORY_PS,
    SXML_REGEX_GENERAL_CATEGORY_PE,
    SXML_REGEX_GENERAL_CATEGORY_PI,
    SXML_REGEX_GENERAL_CATEGORY_PF,
    SXML_REGEX_GENERAL_CATEGORY_PO,
    SXML_REGEX_GENERAL_CATEGORY_ZS,
    SXML_REGEX_GENERAL_CATEGORY_ZL,
    SXML_REGEX_GENERAL_CATEGORY_ZP,
    SXML_REGEX_GENERAL_CATEGORY_SM,
    SXML_REGEX_GENERAL_CATEGORY_SC,
    SXML_REGEX_GENERAL_CATEGORY_SK,
    SXML_REGEX_GENERAL_CATEGORY_SO,
    SXML_REGEX_GENERAL_CATEGORY_CC,
    SXML_REGEX_GENERAL_CATEGORY_CF,
    SXML_REGEX_GENERAL_CATEGORY_CS,
    SXML_REGEX_GENERAL_CATEGORY_CO,
    SXML_REGEX_GENERAL_CATEGORY_CN,
} sxml_regex_general_category;

/* Sets of categories: bit c stands for category c. */
#define SXML_REGEX_CATEGORY_MASK_L 0x0000001FU
#define SXML_REGEX_CATEGORY_MASK_M 0x000000E0U
#define SXML_REGEX_CATEGORY_MASK_N 0x00000700U
#define SXML_REGEX_CATEGORY_MASK_P 0x0003F800U
#define SXML_REGEX_CATEGORY_MASK_Z 0x001C0000U
#define SXML_REGEX_CATEGORY_MASK_S 0x01E00000U
#define SXML_REGEX_CATEGORY_MASK_C 0x3E000000U

/* The general category of codepoint; Cn for unassigned codepoints. */
sxml_regex_general_category sxml_regex_get_general_category(
    uint32_t codepoint);

/* True when codepoint is in \d: general category Nd. */
bool sxml_regex_is_decimal_digit(uint32_t codepoint);

/* True when codepoint is in \w: not in category P, Z or C (unassigned
   codepoints are Cn). */
bool sxml_regex_is_word_character(uint32_t codepoint);

/*
 * Finds the block whose name, without spaces, is name (the X of \p{IsX},
 * case-sensitive: "BasicLatin", "Latin-1Supplement") and writes its range
 * to *range. Returns false, leaving *range untouched, for an unknown
 * name, which F&O 4.0, section 6.1.7 makes FORX0002.
 */
bool sxml_regex_find_block(sxml_byte_view name,
                           sxml_regex_codepoint_range *range);

/* The most case variants one codepoint has, itself included. */
#define SXML_REGEX_MAXIMUM_CASE_VARIANT_COUNT 8U

/* A codepoint and its case variants (F&O 4.0, section 6.2, flag i). */
typedef struct sxml_regex_case_variants {
    uint32_t codepoints[SXML_REGEX_MAXIMUM_CASE_VARIANT_COUNT];
    size_t codepoint_count;
} sxml_regex_case_variants;

/*
 * True when second_codepoint is a case variant of first_codepoint (or
 * equal to it): fn:lower-case of the two is equal or fn:upper-case of the
 * two is (F&O 4.0, section 6.2, flag i).
 */
bool sxml_regex_are_case_variants(uint32_t first_codepoint,
                                  uint32_t second_codepoint);

/* Writes codepoint and all its case variants to *variants, codepoint
   first. */
void sxml_regex_find_case_variants(uint32_t codepoint,
                                   sxml_regex_case_variants *variants);
