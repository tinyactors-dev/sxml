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
 *                         w wildcards (0 off, 1 on), l language operand,
 *                         t stop word operand (the stop words, 0 for
 *                         none), h thesaurus (0 none);
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
 * and MidNum, simplified). A sentence ends after ".", "!" or "?" between
 * tokens. A node's text is its string value, so markup inside a word does
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
 *   binding (2.3.2): 1 when the value's effective boolean value would be
 *   true (any node first, a true boolean, a non-empty string, a non-zero
 *   number), else 0; a for binding's score is 1 for every item (2.3.1:
 *   scores are implementation-dependent within [0, 1]).
 */
extern const sxml_native_function sxml_full_text_contains_function;
extern const sxml_native_function sxml_full_text_score_function;


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
 * in one FTMatchOptions (3.4), FTST0006 for a stop word list by URI (sxml
 * restricts stop words to lists of strings, 5.2.11) and FTST0018 for a
 * thesaurus by URI (none is known) until packet 7.12c, FTST0009 for a
 * language other than en, de, fr, it, es and nl (3.4.1),
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
    /* sxml_full_text_check_rules. */
    sxml_status (*check_rules)(const sxml_xquery_full_text_rules_request *request);
    /* sxml_full_text_lower_step. */
    sxml_status (*lower_step)(sxml_xpath_lowering_machine *machine,
                              sxml_xpath_lowering_frame *frame);
} sxml_full_text_operations;
