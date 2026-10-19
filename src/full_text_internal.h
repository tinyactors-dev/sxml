#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "xpath_lower_internal.h"

/*
 * XQuery and XPath Full Text 3.0 (docs/DECISIONS.md D214; glossary
 * `full_text_selection`, `all_matches`). Internal to sxml.
 *
 * The parts and where they live:
 * - src/xquery_lower_full_text.c, linked with XQuery's lowering (also in
 *   sxml.wasm), kept small: the D162 place (without the feature, Full
 *   Text is XPST0003: a processor without Full Text does not accept its
 *   syntax), the lowering rule whose step is the feature's, the internal
 *   native rows below and the score of let score. It never names the
 *   feature's code: it asks the processor for the optional feature
 *   full-text (D189) and calls its operations, so a program links the
 *   rest only when it names sxml_full_text_feature()
 *   (include/sxml/xquery.h).
 * - src/full_text_lower.c: the static rules and the lowering of
 *   "E contains text S" to a match program.
 * - src/full_text.c: the feature's definition and the contains operation:
 *   the search context items, the ignore option, the match program run
 *   over each item's tokens.
 * - src/full_text_tokenize.c: the tokenizer (below), folding, wildcards.
 * - src/full_text_match.c: the match program and AllMatches.
 * - src/full_text_stem.c and src/full_text_stem_table.c: the Snowball
 *   stemmers (below, "Stemming").
 *
 * The match program (glossary `full_text_selection`)
 * ---------------------------------------------------
 * A selection lowers, as a regex compiles (D53), into a program: an XDM
 * array whose member 1 is the program text, an xs:string, and whose other
 * members are the operands the program names, the values of the
 * expressions inside the selection, computed in the focus of the contains
 * expression (Full Text 3.0, 4.3 step 1) and already coerced (words to
 * xs:string*, ranges and windows to xs:integer, weights to xs:double),
 * and static lists such as stop words (xs:string*). An operand number is
 * an array member number (2 and up); 0 means none.
 *
 * The text is postfix: instructions in the order of a post-order walk of
 * the selection, each an opcode letter, its arguments as decimal numbers
 * separated by ",", and ";". Every instruction pops its operands from a
 * stack of AllMatches and pushes its result, so the program runs in one
 * loop (D71) and ends with one AllMatches:
 *
 *   W v,a,c,d,s,w,l,t,h   FTWords (4.2.4): words operand v; a the
 *                         FTAnyallOption (0 any, 1 all, 2 phrase,
 *                         3 any word, 4 all words); the effective match
 *                         options: c case (0 insensitive, 1 sensitive,
 *                         2 lowercase, 3 uppercase), d diacritics
 *                         (0 insensitive, 1 sensitive), s stemming and
 *                         w wildcards (0 off, 1 on), l the language of
                         the stemmer (sxml_full_text_language),
 *                         t stop word operand (the stop words, 0 for
 *                         none), h thesaurus operand (0 for none): per
 *                         thesaurus its minimum and maximum level, its
 *                         relation count n and n pairs (a term, a term
 *                         related to it by the relationship asked for),
 *                         all xs:string (3.4.3; read while compiling,
 *                         FTST0008 and FTST0018 for lists and thesauri
 *                         the resolver does not have);
 *   T m,n                 FTTimes (4.2.6.10): minimum and maximum
 *                         operands (both the same for "exactly"; 0 for no
 *                         bound);
 *   A, O, M, N            ftand, ftor, not in, ftnot (4.2.6.1 to .4);
 *   G g                   a weight (3.1.1; FTDY0016 outside [-1000, 1000]);
 *   R                     ordered (4.2.6.5);
 *   I v,u                 window v units u (0 words, 1 sentences,
 *                         2 paragraphs; 4.2.6.8);
 *   D m,n,u               distance from m to n units u (4.2.6.9);
 *   S x,u                 scope: x 0 same, 1 different; u 0 sentence,
 *                         1 paragraph (4.2.6.6);
 *   C k                   content: k 0 at start, 1 at end, 2 entire
 *                         content (4.2.6.7).
 *
 * For `$p contains text ("a" ftor {$w} all words) ordered window 3 words`
 * the program is the array
 *
 *     ["W2,0,0,0,0,0,0,0,0;W3,4,0,0,0,0,0,0,0;O;R;I4,0;", "a", $w, 3]
 *
 * Tokens (glossary `full_text_selection`; Full Text 3.0, 4.1)
 * ----------------------------------------------------------------
 * sxml's tokenizer is implementation-defined, as 4.1 allows: a token is a
 * maximal run of letters, marks, digits and connector punctuation
 * (general categories L, M, N, Pc), an apostrophe between two letters and
 * a full stop or comma between two digits included (UAX #29's MidLetter
 * and MidNum, simplified); an ideograph or a hiragana letter is a token by
 * itself (UAX #29 breaks around each). A sentence ends after ".", "!" or
 * "?" (or their ideographic and fullwidth forms) between tokens. A node's text is its string value, so markup inside a word does
 * not split it; a paragraph ends between two text nodes (not only
 * whitespace) whose parents are not one inside the other, so element
 * boundaries are paragraph boundaries except for markup inside a
 * paragraph, and a paragraph end ends the sentence and the token too.
 * Positions, sentences and paragraphs count from 1.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* ------------------------------------------------------ The program */

/* The match program's opcodes (see above). */
enum {
    SXML_FULL_TEXT_OPCODE_WORDS = 'W',
    SXML_FULL_TEXT_OPCODE_TIMES = 'T',
    SXML_FULL_TEXT_OPCODE_AND = 'A',
    SXML_FULL_TEXT_OPCODE_OR = 'O',
    SXML_FULL_TEXT_OPCODE_MILD_NOT = 'M',
    SXML_FULL_TEXT_OPCODE_NOT = 'N',
    SXML_FULL_TEXT_OPCODE_WEIGHT = 'G',
    SXML_FULL_TEXT_OPCODE_ORDER = 'R',
    SXML_FULL_TEXT_OPCODE_WINDOW = 'I',
    SXML_FULL_TEXT_OPCODE_DISTANCE = 'D',
    SXML_FULL_TEXT_OPCODE_SCOPE = 'S',
    SXML_FULL_TEXT_OPCODE_CONTENT = 'C',
};

/* The most arguments an instruction has (WORDS). */
#define SXML_FULL_TEXT_MAXIMUM_ARGUMENT_COUNT 9U

/* FTAnyallOption (Full Text 3.0, 3.2). */
typedef enum sxml_full_text_any_all {
    SXML_FULL_TEXT_ANY_ALL_ANY = 0,
    SXML_FULL_TEXT_ANY_ALL_ALL = 1,
    SXML_FULL_TEXT_ANY_ALL_PHRASE = 2,
    SXML_FULL_TEXT_ANY_ALL_ANY_WORD = 3,
    SXML_FULL_TEXT_ANY_ALL_ALL_WORDS = 4,
} sxml_full_text_any_all;

/* FTCaseOption (3.4.5); insensitive is the default. */
typedef enum sxml_full_text_case {
    SXML_FULL_TEXT_CASE_INSENSITIVE = 0,
    SXML_FULL_TEXT_CASE_SENSITIVE = 1,
    SXML_FULL_TEXT_CASE_LOWERCASE = 2,
    SXML_FULL_TEXT_CASE_UPPERCASE = 3,
} sxml_full_text_case;

/* FTUnit and FTBigUnit (3.5.2 to 3.5.4). */
typedef enum sxml_full_text_unit {
    SXML_FULL_TEXT_UNIT_WORDS = 0,
    SXML_FULL_TEXT_UNIT_SENTENCES = 1,
    SXML_FULL_TEXT_UNIT_PARAGRAPHS = 2,
} sxml_full_text_unit;

/* FTContent (3.5.5). */
typedef enum sxml_full_text_content {
    SXML_FULL_TEXT_CONTENT_AT_START = 0,
    SXML_FULL_TEXT_CONTENT_AT_END = 1,
    SXML_FULL_TEXT_CONTENT_ENTIRE = 2,
} sxml_full_text_content;

/* The match options of one FTWords after inheritance (3.4: the innermost
   FTMatchOptions, then the enclosing ones, then the prolog's ft-option
   declaration, then the defaults). */
typedef struct sxml_full_text_match_options {
    sxml_full_text_case case_mode;
    bool is_diacritics_sensitive;
    bool uses_stemming;
    bool uses_wildcards;
} sxml_full_text_match_options;

/* ------------------------------------------------------- The rows */

/*
 * The internal native rows (in no function table) the lowering calls
 * (src/xquery_lower_full_text.c):
 *
 * - contains($search as item()*, $program as array(*), $ignore as
 *   item()*) as xs:boolean: Full Text 3.0, 4.3, fts:FTContainsExpr;
 *   $ignore the nodes of "without content" (empty without it);
 * - score($value as item()*) as xs:double: the score of a let score
 *   binding whose value is not a contains expression (2.3.2): 1 when the
 *   value's effective boolean value would be true (any node first, a true
 *   boolean, a non-empty string, a non-zero number), else 0 (scores are
 *   implementation-dependent within [0, 1]).
 */
extern const sxml_native_function sxml_full_text_contains_function;
extern const sxml_native_function sxml_full_text_score_function;

/*
 * - score-contains($search, $program, $ignore) as xs:double: the score of
 *   the contains expression (2.3; src/full_text.c): what an "E contains
 *   text S" lowers to when its score is wanted, as the value of a let
 *   score binding or the score of a for binding's item (below).
 */
extern const sxml_native_function sxml_full_text_score_contains_function;


/* ------------------------------------------------------- Tokenizer */

/* Where a token is (Full Text 3.0, 4.1: TokenInfo positions). */
typedef struct sxml_full_text_token {
    /* Its bytes in the text: byte_count bytes from byte_offset. */
    uint32_t byte_offset;
    uint32_t byte_count;
    uint32_t position;
    uint32_t sentence;
    uint32_t paragraph;
} sxml_full_text_token;

/* A text to tokenize: UTF-8 and the byte offsets where paragraphs break
   (ascending; a node's text, src/full_text.c). */
typedef struct sxml_full_text_source {
    sxml_byte_view text;
    const uint32_t *paragraph_breaks;
    size_t paragraph_break_count;
    /* Query text with wildcards on: "." with its qualifier ("?", "*", "+"
       or "{n,m}") and "\" with an escaped word character stay in the
       token; "\" with an escaped punctuation character separates tokens
       (3.4.2). */
    bool keeps_wildcards;
} sxml_full_text_source;

/* Tokens in allocator memory. */
typedef struct sxml_full_text_token_list {
    sxml_allocator allocator;
    sxml_full_text_token *tokens;
    size_t token_count;
    size_t token_capacity;
} sxml_full_text_token_list;

/*
 * Appends the tokens of source to list (whose allocator is set), with
 * positions, sentences and paragraphs continuing from the last token in
 * the list (1 for the first). Returns the allocator's failures and
 * SXML_STATUS_LIMIT_EXCEEDED past UINT32_MAX tokens or bytes.
 */
SXML_NODISCARD sxml_status sxml_full_text_tokenize(const sxml_full_text_source *source,
                                                   sxml_full_text_token_list *list);

/* Releases the tokens of list and leaves it empty. */
void sxml_full_text_token_list_release(sxml_full_text_token_list *list);

/* How a token is compared (Full Text 3.0, 3.4.5 and 3.4.6). */
typedef struct sxml_full_text_folding {
    /* Lowercase (case insensitive) or keep the case. */
    bool folds_case;
    /* Remove the combining marks of the canonical decomposition
       (diacritics insensitive). */
    bool strips_diacritics;
    /* Upper case instead of lower case (the query of "uppercase"). */
    bool uses_upper_case;
} sxml_full_text_folding;

/* Bytes in allocator memory. */
typedef struct sxml_full_text_bytes {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} sxml_full_text_bytes;

/* A word to fold and where the result goes. */
typedef struct sxml_full_text_fold_request {
    sxml_byte_view word;
    sxml_full_text_folding folding;
} sxml_full_text_fold_request;

/*
 * Appends the folded form of request->word to *output (whose allocator is
 * set): case folded with the full lower (or upper) case mapping, then
 * diacritics removed. Returns the allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_full_text_fold(const sxml_full_text_fold_request *request,
                                               sxml_full_text_bytes *output);

/* Releases the bytes of output and leaves it empty. */
void sxml_full_text_bytes_release(sxml_full_text_bytes *output);

/* Appends codepoint to *output in UTF-8, or bytes as they are. Return
   the allocator's failures. */
SXML_NODISCARD sxml_status sxml_full_text_append_codepoint(sxml_full_text_bytes *output,
                                                           uint32_t codepoint);
SXML_NODISCARD sxml_status sxml_full_text_append_text(sxml_full_text_bytes *output,
                                                      sxml_byte_view bytes);

/* ------------------------------------------------------- Stemming */

/*
 * Stemming (Full Text 3.0, 3.4.4; packet 7.12c, D214; glossary
 * `snowball_program`): the Snowball algorithms of the languages of D208,
 * compiled by tools/generate-snowball-stemmers.py from
 * the .sbl files of third_party/snowball into programs (src/full_text_stem_table.c)
 * that one machine runs (src/full_text_stem.c): a flat array of 16-bit
 * words, each instruction an opcode and its operands, over the word's
 * code points, with Snowball's cursor, limits and slice, the program's
 * integers, booleans and string variables, cursor save slots (one per
 * construct that restores the cursor; no routine is recursive, so one
 * slot each suffices) and an explicit return stack (D71).
 */

/* The languages with a stemmer, in the order of the programs. */
typedef enum sxml_full_text_language {
    SXML_FULL_TEXT_LANGUAGE_ENGLISH = 0,
    SXML_FULL_TEXT_LANGUAGE_GERMAN = 1,
    SXML_FULL_TEXT_LANGUAGE_FRENCH = 2,
    SXML_FULL_TEXT_LANGUAGE_ITALIAN = 3,
    SXML_FULL_TEXT_LANGUAGE_SPANISH = 4,
    SXML_FULL_TEXT_LANGUAGE_DUTCH = 5,
} sxml_full_text_language;

#define SXML_FULL_TEXT_LANGUAGE_COUNT 6U

/* The opcodes of a Snowball program (keep in step with OPCODES in
   tools/generate-snowball-stemmers.py). */
typedef enum sxml_full_text_stem_opcode {
    SXML_FULL_TEXT_STEM_JUMP = 0,
    SXML_FULL_TEXT_STEM_JUMP_IF_TRUE,
    SXML_FULL_TEXT_STEM_JUMP_IF_FALSE,
    SXML_FULL_TEXT_STEM_TRUE,
    SXML_FULL_TEXT_STEM_FALSE,
    SXML_FULL_TEXT_STEM_SAVE,
    SXML_FULL_TEXT_STEM_RESTORE,
    SXML_FULL_TEXT_STEM_LIMIT_SAVE,
    SXML_FULL_TEXT_STEM_LIMIT_RESTORE,
    SXML_FULL_TEXT_STEM_BACKWARD_BEGIN,
    SXML_FULL_TEXT_STEM_BACKWARD_END,
    SXML_FULL_TEXT_STEM_LITERAL,
    SXML_FULL_TEXT_STEM_IN_GROUPING,
    SXML_FULL_TEXT_STEM_OUT_GROUPING,
    SXML_FULL_TEXT_STEM_NEXT,
    SXML_FULL_TEXT_STEM_HOP,
    SXML_FULL_TEXT_STEM_AT_LIMIT,
    SXML_FULL_TEXT_STEM_OPEN_SLICE,
    SXML_FULL_TEXT_STEM_CLOSE_SLICE,
    SXML_FULL_TEXT_STEM_REPLACE,
    SXML_FULL_TEXT_STEM_SLICE_TO,
    SXML_FULL_TEXT_STEM_INSERT,
    SXML_FULL_TEXT_STEM_INSERT_VARIABLE,
    SXML_FULL_TEXT_STEM_ATTACH,
    SXML_FULL_TEXT_STEM_ATTACH_VARIABLE,
    SXML_FULL_TEXT_STEM_SET_MARK,
    SXML_FULL_TEXT_STEM_TO_MARK,
    SXML_FULL_TEXT_STEM_AT_MARK,
    SXML_FULL_TEXT_STEM_INTEGER_ASSIGN,
    SXML_FULL_TEXT_STEM_INTEGER_COMPARE,
    SXML_FULL_TEXT_STEM_BOOLEAN_SET,
    SXML_FULL_TEXT_STEM_BOOLEAN_TEST,
    SXML_FULL_TEXT_STEM_CALL,
    SXML_FULL_TEXT_STEM_RETURN,
    SXML_FULL_TEXT_STEM_AMONG_FIND,
    SXML_FULL_TEXT_STEM_AMONG_DISPATCH,
} sxml_full_text_stem_opcode;

/* count code points (or entries) from first. */
typedef struct sxml_full_text_stem_span {
    uint16_t first;
    uint16_t count;
} sxml_full_text_stem_span;

/* One string of an among and the action it selects (from 1). */
typedef struct sxml_full_text_stem_among_entry {
    uint16_t string;
    uint16_t result;
} sxml_full_text_stem_among_entry;

/* One compiled Snowball algorithm. Its routine "stem" starts at code 0.
   Strings, groupings (sorted code points) and among entries (longest
   first) are spans of characters and among_entries. */
typedef struct sxml_full_text_stemmer_program {
    const uint16_t *code;
    size_t code_count;
    const uint16_t *characters;
    const sxml_full_text_stem_span *strings;
    const sxml_full_text_stem_span *groupings;
    const sxml_full_text_stem_among_entry *among_entries;
    const sxml_full_text_stem_span *amongs;
    size_t among_count;
    uint16_t slot_count;
    uint16_t limit_slot_count;
    uint16_t integer_count;
    uint16_t boolean_count;
    uint16_t string_variable_count;
    uint16_t call_depth;
} sxml_full_text_stemmer_program;

/* Indexed by sxml_full_text_language (src/full_text_stem_table.c). */
extern const sxml_full_text_stemmer_program sxml_full_text_stemmer_programs[];

/* The language of a language option's tag: its primary subtag, ASCII case
   insensitive; English for an empty tag and any other: zh and ja, which
   have no stemmer (their words of one character stay as they are), and
   those FTST0009 has refused statically (3.4.1). */
sxml_full_text_language sxml_full_text_language_of(sxml_byte_view language_tag);

/* The memory of the stemming machine, reused from word to word. */
typedef struct sxml_full_text_stem_workspace {
    sxml_allocator allocator;
    /* The word's code points while it is stemmed. */
    uint32_t *text;
    size_t text_capacity;
    /* String variables (Dutch's ch), text_capacity code points each. */
    uint32_t *variables;
    size_t variable_capacity;
    /* Slots, limit slots, integers, booleans, among results and the
       return stack. */
    int32_t *registers;
    size_t register_capacity;
} sxml_full_text_stem_workspace;

/* One word to stem. */
typedef struct sxml_full_text_stem_request {
    /* UTF-8; words longer than SXML_FULL_TEXT_MAXIMUM_STEMMED_LENGTH code
       points are not stemmed (they are appended as they are). */
    sxml_byte_view word;
    sxml_full_text_language language;
    sxml_full_text_stem_workspace *workspace;
} sxml_full_text_stem_request;

#define SXML_FULL_TEXT_MAXIMUM_STEMMED_LENGTH 256U

/*
 * Appends the stem of request->word to *output (whose allocator is set):
 * the Snowball algorithm of request->language, run on the word as it is
 * (Snowball's algorithms expect lowercase words; the caller folds first).
 * Returns the allocator's failures, SXML_STATUS_LIMIT_EXCEEDED when a
 * program runs past its step bound, and SXML_STATUS_INVALID_ARGUMENT for
 * a program the generator cannot have made.
 */
SXML_NODISCARD sxml_status sxml_full_text_stem(const sxml_full_text_stem_request *request,
                                               sxml_full_text_bytes *output);

/* Releases what workspace holds and leaves it empty. */
void sxml_full_text_stem_workspace_release(sxml_full_text_stem_workspace *workspace);

/* A query token with wildcards and a folded token of the text. */
typedef struct sxml_full_text_wildcard_request {
    sxml_byte_view pattern;
    sxml_byte_view word;
    /* Scratch memory for the matcher's state sets. */
    sxml_allocator allocator;
} sxml_full_text_wildcard_request;

/* What a wildcard match finds. */
typedef struct sxml_full_text_wildcard_outcome {
    bool is_match;
    /* The pattern is not valid wildcard syntax (FTDY0020, 3.4.2). */
    bool is_invalid;
} sxml_full_text_wildcard_outcome;

/*
 * Matches request->word against request->pattern under the wildcard
 * syntax of Full Text 3.0, 3.4.2: "." any character, ".?", ".*", ".+" and
 * ".{n,m}" that many, "\" before a character takes it literally. A loop
 * over sets of pattern states, no backtracking and no recursion (D71).
 * Returns the allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_full_text_match_wildcards(
    const sxml_full_text_wildcard_request *request, sxml_full_text_wildcard_outcome *outcome);

/* ---------------------------------- Stop word lists and thesauri */

/* A word of a word list: byte_count bytes from byte_offset. */
typedef struct sxml_full_text_word {
    uint32_t byte_offset;
    uint32_t byte_count;
} sxml_full_text_word;

/* Words in allocator memory (bytes.allocator). */
typedef struct sxml_full_text_word_list {
    sxml_full_text_bytes bytes;
    sxml_full_text_word *words;
    size_t word_count;
    size_t word_capacity;
} sxml_full_text_word_list;

/* The word word_index of list, borrowed from it. */
sxml_byte_view sxml_full_text_word_at(const sxml_full_text_word_list *list, size_t word_index);

/* Releases the words of list and leaves it empty. */
void sxml_full_text_word_list_release(sxml_full_text_word_list *list);

/* A stop word list or thesaurus to read (src/full_text_resource.c). */
typedef struct sxml_full_text_resource_request {
    sxml_processor *processor;
    /* SXML_RESOURCE_KIND_FULL_TEXT_STOP_WORDS or
       SXML_RESOURCE_KIND_FULL_TEXT_THESAURUS. */
    sxml_resource_kind resource_kind;
    /* The URI as written; empty for the default list or thesaurus. */
    sxml_byte_view uri_reference;
    /* The static base URI it is resolved against; may be empty. */
    sxml_byte_view base_uri;
} sxml_full_text_resource_request;

/* Whether the resolver delivered it. */
typedef struct sxml_full_text_resource_outcome {
    bool is_found;
} sxml_full_text_resource_outcome;

/*
 * Asks the processor's resolver for request's resource and appends to
 * *list (whose bytes.allocator is set) what it holds: a stop word list's
 * words, or a thesaurus's relations as triples (a term, the relationship,
 * the related term; one triple per synonym element, its parent's term
 * first). outcome->is_found is false when the resolver has no such
 * resource or a thesaurus is not well-formed XML. Returns the resolver's
 * and allocator's failures, and SXML_STATUS_INVALID_ARGUMENT for a form
 * the kind does not allow.
 */
SXML_NODISCARD sxml_status sxml_full_text_read_resource(
    const sxml_full_text_resource_request *request, sxml_full_text_word_list *list,
    sxml_full_text_resource_outcome *outcome);

/* ------------------------------------------------------ The matcher */

/* What the program runs over: one search context item's tokens. */
typedef struct sxml_full_text_search {
    sxml_byte_view text;
    const sxml_full_text_token *tokens;
    size_t token_count;
} sxml_full_text_search;

/* One run of a program over one item. */
typedef struct sxml_full_text_run_request {
    const sxml_function_call *call;
    /* The program array (see above), borrowed. */
    sxml_item program;
    sxml_full_text_search search;
} sxml_full_text_run_request;

/* What a run finds. */
typedef struct sxml_full_text_run_outcome {
    bool is_match;
    /* The sum of the weights of the matches without a StringExclude (each
       1 times the weights of the selections it passed, 3.1.1). */
    double weight;
} sxml_full_text_run_outcome;

/*
 * Runs the program over request->search and writes to outcome->is_match
 * whether its AllMatches has a match without a StringExclude (4.3,
 * fts:FTContainsExpr). Errors go to request->call's error record: FTDY0016
 * (weight), FTDY0017 (not in), FTDY0020 (wildcards), XPTY0004 for an
 * operand of the wrong type; SXML_STATUS_LIMIT_EXCEEDED when the matches
 * grow past the matcher's bound (an error, never a false result); the
 * heap's failures; SXML_STATUS_INVALID_ARGUMENT for a program the
 * lowering cannot have made.
 */
SXML_NODISCARD sxml_status sxml_full_text_run(const sxml_full_text_run_request *request,
                                              sxml_full_text_run_outcome *outcome);

/* --------------------------------------- XQuery (src/xquery_lower_full_text.c) */

/* One module to check. */
typedef struct sxml_xquery_full_text_rules_request {
    const sxml_ast *ast;
    /* Whose configuration names the feature or not; may be NULL. */
    const sxml_processor *processor;
    sxml_error_record *error_record;
} sxml_xquery_full_text_rules_request;

/*
 * The static rules of Full Text over one module's AST, in one pre-order
 * walk by its links (D71): without the feature, the first Full Text
 * construct is XPST0003; with it, FTST0019 for two options of one group
 * in one FTMatchOptions (3.4), FTST0009 for a language other than en,
 * de, fr, it, es, nl, zh and ja (3.4.1),
 * XQST0079 for an extension selection without a selection (3.6, no
 * pragma is recognized), XQST0089 for a score variable named as its for
 * or positional variable (2.3.1). Called by sxml_xquery_lower for every
 * module of the query (src/xquery_prolog.c). Returns
 * SXML_STATUS_STATIC_ERROR with the error in request->error_record.
 */
SXML_NODISCARD sxml_status sxml_xquery_check_full_text_rules(
    const sxml_xquery_full_text_rules_request *request);

/* The value of "let score $s := E" to score, bound in block. */
typedef struct sxml_xquery_full_text_score_request {
    sxml_xpath_lowering_block *block;
    /* The LET_SCORE_BINDING (its source span). */
    sxml_ast_node_index node;
    sxml_ir_atom value;
} sxml_xquery_full_text_score_request;

SXML_NODISCARD sxml_status sxml_xquery_bind_full_text_score(
    sxml_xpath_lowerer *lowerer, const sxml_xquery_full_text_score_request *request,
    sxml_ir_atom *atom);

/* The score of the item of a for binding with a score variable (2.3.1). */
typedef struct sxml_xquery_full_text_for_score_request {
    /* The FOR_ITEM_BINDING. */
    sxml_ast_node_index binding;
    /* The slot of the binding's item. */
    sxml_ir_atom item;
    /* Where the score is bound, and its atom. */
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *score;
} sxml_xquery_full_text_for_score_request;

/*
 * Pushes the procedure that lowers request's score into request->block:
 * when the last step of the binding's sequence has a predicate that is a
 * contains expression ("for $b score $s in //book[. contains text 'x']"),
 * that predicate lowered again, as score-contains, with the item as its
 * focus (a MAP over the item); else the constant 1 (scores are
 * implementation-dependent within [0, 1], 2.3). Called by
 * src/xquery_lower.c; the atom is written when the frame finishes.
 */
SXML_NODISCARD sxml_status sxml_xquery_call_full_text_for_score(
    sxml_xpath_lowering_machine *machine, const sxml_xquery_full_text_for_score_request *request);

/* Whether the FT_CONTAINS_EXPRESSION frame lowers to score-contains: the
   value of a let score binding (through parentheses), or the predicate the
   for score procedure asked for (src/full_text_lower.c). */
bool sxml_xquery_full_text_is_scored(const sxml_xpath_lowerer *lowerer,
                                     const sxml_xpath_lowering_frame *frame);

/* The bytes of locals the core's contains procedure reserves for the
   feature's lowering step (src/full_text_lower.c checks they suffice). */
#define SXML_FULL_TEXT_LOWERING_LOCALS_SIZE 512U

/* The static rules with the feature (src/full_text_lower.c), as
   sxml_xquery_check_full_text_rules says. */
SXML_NODISCARD sxml_status sxml_full_text_check_rules(
    const sxml_xquery_full_text_rules_request *request);

/* The step of the contains procedure (src/full_text_lower.c): lowers an
   FT_CONTAINS_EXPRESSION to contains(E, program, ignore). */
SXML_NODISCARD sxml_status sxml_full_text_lower_step(sxml_xpath_lowering_machine *machine,
                                                     sxml_xpath_lowering_frame *frame);

/* The operations of the feature full-text
   (SXML_OPTIONAL_FEATURE_KIND_FULL_TEXT, src/full_text.c): what the
   core (src/xquery_lower_full_text.c) calls. None calls back into the
   place that called it (D71). */
typedef struct sxml_full_text_operations {
    /* The body of the contains row: as its call function. */
    sxml_status (*contains)(const sxml_function_call *call, sxml_sequence *result);
    /* The body of the score-contains row. */
    sxml_status (*score_contains)(const sxml_function_call *call, sxml_sequence *result);
    /* sxml_full_text_check_rules. */
    sxml_status (*check_rules)(const sxml_xquery_full_text_rules_request *request);
    /* sxml_full_text_lower_step. */
    sxml_status (*lower_step)(sxml_xpath_lowering_machine *machine,
                              sxml_xpath_lowering_frame *frame);
} sxml_full_text_operations;
