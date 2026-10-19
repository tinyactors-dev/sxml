/*
 * The Full Text match program and AllMatches (src/full_text_internal.h,
 * "The match program"; glossary `full_text_selection`, `all_matches`;
 * docs/DECISIONS.md D214). Part of the optional feature full-text.
 *
 * AllMatches (Full Text 3.0, 4.2) are internal rows, not XDM values: a
 * StringMatch is a token span with its query position, whether it is an
 * include or an exclude and whether it is contiguous; a Match is a range
 * of StringMatches; an AllMatches a range of Matches. All three live in
 * two arrays that only grow during one run, so every operator reads its
 * operands' ranges and appends its result, and the program's stack holds
 * ranges. Each operator is the spec's fts: function as a loop (D71).
 *
 * Where sxml does less than the spec's formal functions (recorded in
 * D214): FTTimes with an upper bound gives, for each combination of
 * occurrences in the range, the others as StringExcludes (the spec's
 * ftand ftnot of FormCombinationsAtLeast, simplified; the same results
 * without positional filters after it), and "at least N" with no upper
 * bound the combinations of exactly N; FTWindow over a match with
 * StringExcludes gives one match per distinct set of retained excludes.
 *
 * Golden file: src/regex.c (a compiled program run by a loop with an
 * explicit stack, D53).
 *
 * Section numbers are of XQuery and XPath Full Text 3.0 (W3C
 * Recommendation, 24 November 2015).
 */
#include "full_text_internal.h"

#include "function_library_core_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Rows */

/* fts:tokenInfo: where a StringMatch is. */
typedef struct token_span {
    uint32_t start_position;
    uint32_t end_position;
    uint32_t start_sentence;
    uint32_t end_sentence;
    uint32_t start_paragraph;
    uint32_t end_paragraph;
} token_span;

/* fts:stringInclude and fts:stringExclude. */
typedef struct string_match {
    token_span span;
    uint32_t query_position;
    bool is_include;
    bool is_contiguous;
} string_match;

/* fts:match: string_match_count StringMatches from first_string_match. */
typedef struct match_row {
    size_t first_string_match;
    size_t string_match_count;
} match_row;

/* fts:allMatches: match_count Matches from first_match. */
typedef struct all_matches {
    size_t first_match;
    size_t match_count;
} all_matches;

/* The most StringMatches and Matches one run may make: past them the
   run fails with SXML_STATUS_LIMIT_EXCEEDED rather than run away
   (FTUnaryNot and FTTimes multiply matches). */
#define SXML_FULL_TEXT_MAXIMUM_ROWS 2000000U

/* The deepest program stack. */
#define SXML_FULL_TEXT_MAXIMUM_STACK 4096U

/* A folded token: byte_count bytes from byte_offset of its variant's
   bytes. */
typedef struct folded_token {
    uint32_t byte_offset;
    uint32_t byte_count;
} folded_token;

/* The text's tokens folded one way: variant number folds_case + 2 *
   strips_diacritics + 4 * stems (3.4.4). Made on first use (a stemmed
   variant again when another language asks for it). */
typedef struct folded_variant {
    bool is_made;
    sxml_full_text_language language;
    sxml_full_text_bytes bytes;
    folded_token *tokens;
} folded_variant;

#define SXML_FULL_TEXT_VARIANT_COUNT 8U

/* One run of a program over one item's tokens. */
typedef struct matcher {
    const sxml_full_text_run_request *request;
    sxml_allocator allocator;
    string_match *string_matches;
    size_t string_match_count;
    size_t string_match_capacity;
    match_row *matches;
    size_t match_count;
    size_t match_capacity;
    all_matches *stack;
    size_t stack_count;
    size_t stack_capacity;
    uint32_t next_query_position;
    folded_variant variants[SXML_FULL_TEXT_VARIANT_COUNT];
    sxml_full_text_stem_workspace stem_workspace;
} matcher;

/* --------------------------------------------------------- Errors */

#define FULL_TEXT_ERROR(name, status_value, code, text)                                    \
    static const sxml_static_error name = {.error_status = (status_value),                 \
                                           .code_local_name = SXML_BYTE_VIEW_INITIALIZER(code), \
                                           .description = SXML_BYTE_VIEW_INITIALIZER(text)}

FULL_TEXT_ERROR(weight_error, SXML_STATUS_DYNAMIC_ERROR, "FTDY0016",
                "the absolute value of a weight is greater than 1000 (Full Text 3.0, 3.1.1)");
FULL_TEXT_ERROR(mild_not_error, SXML_STATUS_DYNAMIC_ERROR, "FTDY0017",
                "an operand of not in has a StringExclude (Full Text 3.0, 4.2.6.4)");
FULL_TEXT_ERROR(wildcard_error, SXML_STATUS_DYNAMIC_ERROR, "FTDY0020",
                "a query string with wildcards is not valid wildcard syntax (Full Text 3.0, "
                "3.4.2)");
FULL_TEXT_ERROR(integer_error, SXML_STATUS_TYPE_ERROR, "XPTY0004",
                "a range or window of a full-text selection is not one xs:integer (Full Text "
                "3.0, 4.3)");

/* ---------------------------------------------------------- Growth */

/* Grows *elements (capacity *capacity) to hold required elements. */
typedef struct growth {
    void **elements;
    size_t *capacity;
    size_t count;
    size_t required;
    size_t element_size;
    size_t alignment;
} growth;

static sxml_status grow(sxml_allocator allocator, const growth *request) {
    if (request->required <= *request->capacity) {
        return SXML_STATUS_OK;
    }
    size_t capacity = *request->capacity == 0 ? 64U : *request->capacity;
    while (capacity < request->required) {
        capacity *= 2U;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = capacity * request->element_size,
                                  .alignment = request->alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (request->count != 0) {
        memcpy(memory, *request->elements, request->count * request->element_size);
    }
    sxml_allocator_release(allocator, *request->elements);
    *request->elements = memory;
    *request->capacity = capacity;
    return SXML_STATUS_OK;
}

/* Makes room for string_match_count more StringMatches. */
static sxml_status reserve_string_matches(matcher *state, size_t string_match_count) {
    if (string_match_count > SXML_FULL_TEXT_MAXIMUM_ROWS ||
        state->string_match_count > SXML_FULL_TEXT_MAXIMUM_ROWS - string_match_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    return grow(state->allocator,
                &(growth){.elements = (void **)&state->string_matches,
                          .capacity = &state->string_match_capacity,
                          .count = state->string_match_count,
                          .required = state->string_match_count + string_match_count,
                          .element_size = sizeof(string_match),
                          .alignment = alignof(string_match)});
}

static sxml_status reserve_matches(matcher *state, size_t match_count) {
    if (match_count > SXML_FULL_TEXT_MAXIMUM_ROWS ||
        state->match_count > SXML_FULL_TEXT_MAXIMUM_ROWS - match_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    return grow(state->allocator, &(growth){.elements = (void **)&state->matches,
                                            .capacity = &state->match_capacity,
                                            .count = state->match_count,
                                            .required = state->match_count + match_count,
                                            .element_size = sizeof(match_row),
                                            .alignment = alignof(match_row)});
}

/* Appends one StringMatch (room reserved). */
static void put_string_match(matcher *state, string_match entry) {
    state->string_matches[state->string_match_count] = entry;
    state->string_match_count += 1;
}

/* Appends a Match of the StringMatches from first to the end (room
   reserved). */
static void put_match(matcher *state, size_t first_string_match) {
    state->matches[state->match_count] =
        (match_row){.first_string_match = first_string_match,
                    .string_match_count = state->string_match_count - first_string_match};
    state->match_count += 1;
}

/* One StringMatch to copy, inverted when is_inverted. */
typedef struct string_copy {
    size_t string_match_index;
    bool is_inverted;
} string_copy;

static sxml_status copy_string_match(matcher *state, string_copy copy) {
    sxml_status status = reserve_string_matches(state, 1);
    if (status == SXML_STATUS_OK) {
        string_match entry = state->string_matches[copy.string_match_index];
        if (copy.is_inverted) {
            entry.is_include = !entry.is_include;
        }
        put_string_match(state, entry);
    }
    return status;
}

/* Appends the StringMatches of a match, inverted when is_inverted. */
typedef struct match_copy {
    size_t match_index;
    bool is_inverted;
} match_copy;

static sxml_status copy_match_entries(matcher *state, match_copy copy) {
    const match_row row = state->matches[copy.match_index];
    sxml_status status = reserve_string_matches(state, row.string_match_count);
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < row.string_match_count;
         entry += 1) {
        string_match copied = state->string_matches[row.first_string_match + entry];
        if (copy.is_inverted) {
            copied.is_include = !copied.is_include;
        }
        put_string_match(state, copied);
    }
    return status;
}

/* ------------------------------------------------------------ Stack */

static sxml_status push(matcher *state, all_matches value) {
    if (state->stack_count == SXML_FULL_TEXT_MAXIMUM_STACK) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const sxml_status status =
        grow(state->allocator, &(growth){.elements = (void **)&state->stack,
                                         .capacity = &state->stack_capacity,
                                         .count = state->stack_count,
                                         .required = state->stack_count + 1,
                                         .element_size = sizeof(all_matches),
                                         .alignment = alignof(all_matches)});
    if (status == SXML_STATUS_OK) {
        state->stack[state->stack_count] = value;
        state->stack_count += 1;
    }
    return status;
}

static sxml_status pop(matcher *state, all_matches *value) {
    if (state->stack_count == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    state->stack_count -= 1;
    *value = state->stack[state->stack_count];
    return SXML_STATUS_OK;
}

/* The AllMatches of the matches appended from first_match. */
static all_matches matches_since(const matcher *state, size_t first_match) {
    return (all_matches){.first_match = first_match,
                         .match_count = state->match_count - first_match};
}

/* ------------------------------------------------------- Operands */

/* The program's member member_number (from 1), borrowed. */
static sxml_status read_member(const matcher *state, uint32_t member_number,
                               sxml_sequence *member) {
    if (member_number == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_array_get_member(state->request->program, member_number - 1, member);
}

/* An xs:integer operand (coerced by the lowering). */
static sxml_status read_integer(const matcher *state, uint32_t member_number, int64_t *value) {
    sxml_sequence member;
    sxml_status status = read_member(state, member_number, &member);
    sxml_item item;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(member,
                                        (sxml_item_request){.item_index = 0,
                                                            .error_record =
                                                                state->request->call->error_record},
                                        &item);
    }
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE || status == SXML_STATUS_INVALID_ARGUMENT ||
        (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_INTEGER &&
         datum.datum_kind != SXML_DATUM_KIND_DECIMAL)) {
        return sxml_function_library_report(state->request->call, &integer_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_DECIMAL) {
        /* Beyond int64_t: as large as any count can be. */
        *value = (datum.datum_details.decimal_text.byte_count != 0 &&
                  datum.datum_details.decimal_text.bytes[0] == '-')
                     ? INT64_MIN
                     : INT64_MAX;
        return SXML_STATUS_OK;
    }
    *value = datum.datum_details.integer;
    return SXML_STATUS_OK;
}

/* --------------------------------------------------- Text folding */

/* Appends the stem of folded (3.4.4) to *output. */
typedef struct stem_target {
    sxml_byte_view folded;
    sxml_full_text_language language;
} stem_target;

static sxml_status append_stem(matcher *state, stem_target target, sxml_full_text_bytes *output) {
    return sxml_full_text_stem(&(sxml_full_text_stem_request){.word = target.folded,
                                                               .language = target.language,
                                                               .workspace = &state->stem_workspace},
                               output);
}

/* A variant of the text's tokens. */
typedef struct variant_choice {
    size_t variant_number;
    sxml_full_text_language language;
} variant_choice;

/* Folds every token of the text as variant variant_number says, and
   stems it (in language) when the variant does. */
static sxml_status make_variant(matcher *state, variant_choice choice) {
    const size_t variant_number = choice.variant_number;
    folded_variant *variant = &state->variants[variant_number];
    const bool stems = (variant_number & 4U) != 0;
    if (variant->is_made && (!stems || variant->language == choice.language)) {
        return SXML_STATUS_OK;
    }
    sxml_full_text_bytes_release(&variant->bytes);
    sxml_allocator_release(state->allocator, variant->tokens);
    variant->tokens = NULL;
    variant->is_made = false;
    variant->language = choice.language;
    const sxml_full_text_search *search = &state->request->search;
    variant->bytes = (sxml_full_text_bytes){.allocator = state->allocator};
    sxml_full_text_bytes folded = {.allocator = state->allocator};
    sxml_status status = SXML_STATUS_OK;
    if (search->token_count != 0) {
        status = sxml_allocator_allocate(
            state->allocator,
            (sxml_allocation_request){.byte_count = search->token_count * sizeof(folded_token),
                                      .alignment = alignof(folded_token)},
            (void **)&variant->tokens);
    }
    for (size_t token_index = 0; status == SXML_STATUS_OK && token_index < search->token_count;
         token_index += 1) {
        const sxml_full_text_token *token = &search->tokens[token_index];
        const size_t start = variant->bytes.byte_count;
        folded.byte_count = 0;
        status = sxml_full_text_fold(
            &(sxml_full_text_fold_request){
                .word = {.bytes = search->text.bytes + token->byte_offset,
                         .byte_count = token->byte_count},
                .folding = {.folds_case = (variant_number & 1U) != 0,
                            .strips_diacritics = (variant_number & 2U) != 0,
                            .uses_upper_case = false}},
            stems ? &folded : &variant->bytes);
        if (status == SXML_STATUS_OK && stems) {
            status = append_stem(
                state,
                (stem_target){.folded = {.bytes = folded.bytes, .byte_count = folded.byte_count},
                              .language = choice.language},
                &variant->bytes);
        }
        if (status == SXML_STATUS_OK && variant->bytes.byte_count > UINT32_MAX) {
            status = SXML_STATUS_LIMIT_EXCEEDED;
        }
        if (status == SXML_STATUS_OK) {
            variant->tokens[token_index] =
                (folded_token){.byte_offset = (uint32_t)start,
                               .byte_count = (uint32_t)(variant->bytes.byte_count - start)};
        }
    }
    sxml_full_text_bytes_release(&folded);
    variant->is_made = status == SXML_STATUS_OK;
    return status;
}

/* The folded text of token token_index in its variant. */
static sxml_byte_view variant_token(const matcher *state, size_t variant_number,
                                    size_t token_index) {
    const folded_variant *variant = &state->variants[variant_number];
    const folded_token token = variant->tokens[token_index];
    return (sxml_byte_view){.bytes = variant->bytes.bytes + token.byte_offset,
                            .byte_count = token.byte_count};
}

/* ----------------------------------------------------- FTWords (4.2.4) */

/* A query token: its folded bytes in the words' buffer, and whether it is
   a stop word (3.4.7: it matches any token). */
typedef struct query_token {
    size_t byte_offset;
    size_t byte_count;
    bool is_stop_word;
} query_token;

/* The tokens of one FTWords. */
typedef struct query_tokens {
    sxml_allocator allocator;
    sxml_full_text_bytes bytes;
    query_token *tokens;
    size_t token_count;
    size_t token_capacity;
    /* Where each query string's tokens start: string_starts[s] for
       string s, and string_starts[string_count] = token_count. */
    size_t *string_starts;
    size_t string_count;
    size_t string_capacity;
} query_tokens;

static void release_query_tokens(query_tokens *query) {
    sxml_full_text_bytes_release(&query->bytes);
    sxml_allocator_release(query->allocator, query->tokens);
    sxml_allocator_release(query->allocator, query->string_starts);
}

/* What one FTWords instruction says. */
typedef struct words_instruction {
    uint32_t words_member;
    sxml_full_text_any_all any_all;
    sxml_full_text_match_options options;
    sxml_full_text_language language;
    uint32_t stop_words_member;
} words_instruction;

/* The folding of the query side and of the text side (3.4.5: case
   insensitive lowercases both; lowercase and uppercase change the query
   and compare it with the text as written). */
static sxml_full_text_folding query_folding(const words_instruction *words) {
    const sxml_full_text_case case_mode = words->options.case_mode;
    return (sxml_full_text_folding){
        .folds_case = case_mode != SXML_FULL_TEXT_CASE_SENSITIVE,
        .strips_diacritics = !words->options.is_diacritics_sensitive,
        .uses_upper_case = case_mode == SXML_FULL_TEXT_CASE_UPPERCASE};
}

/* Whether the words are stemmed (3.4.4; a query token with wildcards
   is a pattern, not a word, so neither side is stemmed then). */
static bool stems_words(const words_instruction *words) {
    return words->options.uses_stemming && !words->options.uses_wildcards;
}

static size_t text_variant(const words_instruction *words) {
    return (words->options.case_mode == SXML_FULL_TEXT_CASE_INSENSITIVE ? 1U : 0U) +
           (words->options.is_diacritics_sensitive ? 0U : 2U) + (stems_words(words) ? 4U : 0U);
}

/* Whether folded is one of the stop words (each folded as the query). */
typedef struct stop_word_check {
    const words_instruction *words;
    sxml_byte_view folded;
} stop_word_check;

/* A yes or no answer. */
typedef struct answer {
    bool is_true;
} answer;

static sxml_status is_stop_word(const matcher *state, stop_word_check check, answer *is_stop) {
    is_stop->is_true = false;
    if (check.words->stop_words_member == 0) {
        return SXML_STATUS_OK;
    }
    sxml_sequence stop_words;
    sxml_status status = read_member(state, check.words->stop_words_member, &stop_words);
    for (uint64_t word_index = 0; status == SXML_STATUS_OK && !is_stop->is_true;
         word_index += 1) {
        sxml_item item;
        status = sxml_sequence_get_item(
            stop_words, (sxml_item_request){.item_index = word_index, .error_record = NULL},
            &item);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            return SXML_STATUS_OK;
        }
        sxml_atomic_datum datum;
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(item, &datum);
        }
        sxml_full_text_bytes folded = {.allocator = state->allocator};
        if (status == SXML_STATUS_OK) {
            status = sxml_full_text_fold(
                &(sxml_full_text_fold_request){.word = datum.datum_details.string.utf8,
                                               .folding = query_folding(check.words)},
                &folded);
        }
        if (status == SXML_STATUS_OK) {
            is_stop->is_true = sxml_byte_view_equals(
                (sxml_byte_view){.bytes = folded.bytes, .byte_count = folded.byte_count},
                check.folded);
        }
        sxml_full_text_bytes_release(&folded);
    }
    return status;
}

/* Adds one query string's tokens. */
typedef struct query_string {
    const words_instruction *words;
    sxml_byte_view text;
} query_string;

static sxml_status add_query_string(matcher *state, query_string request,
                                    query_tokens *query) {
    sxml_full_text_token_list list = {.allocator = state->allocator};
    sxml_full_text_bytes folded = {.allocator = state->allocator};
    sxml_status status =
        sxml_full_text_tokenize(&(sxml_full_text_source){.text = request.text,
                                                         .paragraph_breaks = NULL,
                                                         .paragraph_break_count = 0,
                                                         .keeps_wildcards =
                                                             request.words->options.uses_wildcards},
                                &list);
    if (status == SXML_STATUS_OK) {
        status = grow(state->allocator,
                      &(growth){.elements = (void **)&query->string_starts,
                                .capacity = &query->string_capacity,
                                .count = query->string_count,
                                .required = query->string_count + 2,
                                .element_size = sizeof(size_t),
                                .alignment = alignof(size_t)});
    }
    if (status == SXML_STATUS_OK) {
        query->string_starts[query->string_count] = query->token_count;
        query->string_count += 1;
        status = grow(state->allocator,
                      &(growth){.elements = (void **)&query->tokens,
                                .capacity = &query->token_capacity,
                                .count = query->token_count,
                                .required = query->token_count + list.token_count + 1,
                                .element_size = sizeof(query_token),
                                .alignment = alignof(query_token)});
    }
    for (size_t token_index = 0; status == SXML_STATUS_OK && token_index < list.token_count;
         token_index += 1) {
        const sxml_full_text_token *token = &list.tokens[token_index];
        const size_t start = query->bytes.byte_count;
        folded.byte_count = 0;
        status = sxml_full_text_fold(
            &(sxml_full_text_fold_request){
                .word = {.bytes = request.text.bytes + token->byte_offset,
                         .byte_count = token->byte_count},
                .folding = query_folding(request.words)},
            &folded);
        const sxml_byte_view folded_word = {.bytes = folded.bytes, .byte_count = folded.byte_count};
        answer is_stop = {.is_true = false};
        if (status == SXML_STATUS_OK) {
            status = is_stop_word(
                state, (stop_word_check){.words = request.words, .folded = folded_word}, &is_stop);
        }
        if (status == SXML_STATUS_OK) {
            status = stems_words(request.words)
                         ? append_stem(state,
                                       (stem_target){.folded = folded_word,
                                                     .language = request.words->language},
                                       &query->bytes)
                         : sxml_full_text_append_text(&query->bytes, folded_word);
        }
        if (status == SXML_STATUS_OK) {
            query->tokens[query->token_count] =
                (query_token){.byte_offset = start,
                              .byte_count = query->bytes.byte_count - start,
                              .is_stop_word = is_stop.is_true};
            query->token_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        query->string_starts[query->string_count] = query->token_count;
    }
    sxml_full_text_bytes_release(&folded);
    sxml_full_text_token_list_release(&list);
    return status;
}

/* Tokenizes every query string of the words operand. */
static sxml_status read_query(matcher *state, const words_instruction *words,
                              query_tokens *query) {
    sxml_sequence strings;
    sxml_status status = read_member(state, words->words_member, &strings);
    for (uint64_t string_index = 0; status == SXML_STATUS_OK; string_index += 1) {
        sxml_item item;
        status = sxml_sequence_get_item(
            strings,
            (sxml_item_request){.item_index = string_index,
                                .error_record = state->request->call->error_record},
            &item);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            return SXML_STATUS_OK;
        }
        sxml_atomic_datum datum;
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(item, &datum);
        }
        if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_STRING) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status == SXML_STATUS_OK) {
            status = add_query_string(
                state, (query_string){.words = words, .text = datum.datum_details.string.utf8}, query);
        }
    }
    return status;
}

/* Whether text token token_index matches query token query_index. */
typedef struct token_comparison {
    const words_instruction *words;
    const query_tokens *query;
    size_t query_index;
    size_t token_index;
} token_comparison;

static sxml_status token_matches(const matcher *state, token_comparison comparison,
                                 answer *is_match) {
    const query_token *query_token_entry = &comparison.query->tokens[comparison.query_index];
    if (query_token_entry->is_stop_word) {
        is_match->is_true = true;
        return SXML_STATUS_OK;
    }
    const sxml_byte_view pattern = {
        .bytes = comparison.query->bytes.bytes + query_token_entry->byte_offset,
        .byte_count = query_token_entry->byte_count};
    const sxml_byte_view word =
        variant_token(state, text_variant(comparison.words), comparison.token_index);
    if (!comparison.words->options.uses_wildcards) {
        is_match->is_true = sxml_byte_view_equals(pattern, word);
        return SXML_STATUS_OK;
    }
    sxml_full_text_wildcard_outcome outcome;
    const sxml_status status = sxml_full_text_match_wildcards(
        &(sxml_full_text_wildcard_request){
            .pattern = pattern, .word = word, .allocator = state->allocator},
        &outcome);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (outcome.is_invalid) {
        return sxml_function_library_report(state->request->call, &wildcard_error);
    }
    is_match->is_true = outcome.is_match;
    return SXML_STATUS_OK;
}

/* The query tokens first_query to first_query + query_count - 1 of query,
   matched as a phrase (fts:applyQueryTokensAsPhrase). */
typedef struct phrase_request {
    const words_instruction *words;
    const query_tokens *query;
    size_t first_query;
    size_t query_count;
    uint32_t query_position;
} phrase_request;

static sxml_status match_phrase(matcher *state, phrase_request request, all_matches *result) {
    const size_t first_match = state->match_count;
    const sxml_full_text_search *search = &state->request->search;
    sxml_status status = SXML_STATUS_OK;
    if (request.query_count == 0 || request.query_count > search->token_count) {
        *result = matches_since(state, first_match);
        return status;
    }
    for (size_t start = 0;
         status == SXML_STATUS_OK && start + request.query_count <= search->token_count;
         start += 1) {
        answer is_match = {.is_true = true};
        for (size_t offset = 0;
             status == SXML_STATUS_OK && is_match.is_true && offset < request.query_count;
             offset += 1) {
            status = token_matches(state,
                                   (token_comparison){.words = request.words,
                                                      .query = request.query,
                                                      .query_index = request.first_query + offset,
                                                      .token_index = start + offset},
                                   &is_match);
        }
        if (status != SXML_STATUS_OK || !is_match.is_true) {
            continue;
        }
        status = reserve_string_matches(state, 1);
        if (status == SXML_STATUS_OK) {
            status = reserve_matches(state, 1);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        const sxml_full_text_token *first = &search->tokens[start];
        const sxml_full_text_token *last = &search->tokens[start + request.query_count - 1];
        const size_t first_string_match = state->string_match_count;
        put_string_match(state, (string_match){.span = {.start_position = first->position,
                                                        .end_position = last->position,
                                                        .start_sentence = first->sentence,
                                                        .end_sentence = last->sentence,
                                                        .start_paragraph = first->paragraph,
                                                        .end_paragraph = last->paragraph},
                                               .query_position = request.query_position,
                                               .is_include = true,
                                               .is_contiguous = true});
        put_match(state, first_string_match);
    }
    *result = matches_since(state, first_match);
    return status;
}

/* The two operands of ftor, ftand and not in. */
typedef struct operand_pair {
    all_matches left;
    all_matches right;
} operand_pair;

/* The operand of a positional filter or FTTimes and its instruction's
   arguments. */
typedef struct filter_input {
    const uint32_t *arguments;
    all_matches operand;
} filter_input;

/* ftor and ftand of two AllMatches (4.2.6.1, 4.2.6.2). */
static sxml_status apply_or(matcher *state, operand_pair pair, all_matches *result) {
    const all_matches left = pair.left;
    const all_matches right = pair.right;
    const size_t first_match = state->match_count;
    sxml_status status = reserve_matches(state, left.match_count + right.match_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t match = 0; match < left.match_count; match += 1) {
        state->matches[state->match_count] = state->matches[left.first_match + match];
        state->match_count += 1;
    }
    for (size_t match = 0; match < right.match_count; match += 1) {
        state->matches[state->match_count] = state->matches[right.first_match + match];
        state->match_count += 1;
    }
    *result = matches_since(state, first_match);
    return SXML_STATUS_OK;
}

static sxml_status apply_and(matcher *state, operand_pair pair, all_matches *result) {
    const all_matches left = pair.left;
    const all_matches right = pair.right;
    const size_t first_match = state->match_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t left_match = 0; status == SXML_STATUS_OK && left_match < left.match_count;
         left_match += 1) {
        for (size_t right_match = 0; status == SXML_STATUS_OK && right_match < right.match_count;
             right_match += 1) {
            const size_t first_string_match = state->string_match_count;
            status = copy_match_entries(
                state, (match_copy){.match_index = left.first_match + left_match});
            if (status == SXML_STATUS_OK) {
                status = copy_match_entries(
                    state, (match_copy){.match_index = right.first_match + right_match});
            }
            if (status == SXML_STATUS_OK) {
                status = reserve_matches(state, 1);
            }
            if (status == SXML_STATUS_OK) {
                put_match(state, first_string_match);
            }
        }
    }
    *result = matches_since(state, first_match);
    return status;
}

/* The query tokens of string string_index, as a phrase. */
typedef struct string_phrase {
    phrase_request phrase;
    size_t string_index;
} string_phrase;

static sxml_status match_query_string(matcher *state, string_phrase request,
                                      all_matches *result) {
    phrase_request phrase = request.phrase;
    phrase.first_query = phrase.query->string_starts[request.string_index];
    phrase.query_count = phrase.query->string_starts[request.string_index + 1] - phrase.first_query;
    return match_phrase(state, phrase, result);
}

/* FTWords (4.2.4, fts:ApplyFTWords): any, all, phrase, any word and all
   words; the query positions count up through the query. */
static sxml_status apply_words(matcher *state, const words_instruction *words,
                               all_matches *result) {
    query_tokens query = {.allocator = state->allocator,
                          .bytes = {.allocator = state->allocator}};
    sxml_status status = read_query(state, words, &query);
    sxml_status variant_status = SXML_STATUS_OK;
    if (status == SXML_STATUS_OK) {
        variant_status = make_variant(
            state,
            (variant_choice){.variant_number = text_variant(words), .language = words->language});
        status = variant_status;
    }
    all_matches combined = {.first_match = state->match_count, .match_count = 0};
    const bool is_conjunction = words->any_all == SXML_FULL_TEXT_ANY_ALL_ALL ||
                                words->any_all == SXML_FULL_TEXT_ANY_ALL_ALL_WORDS;
    phrase_request phrase = {.words = words, .query = &query};
    if (status == SXML_STATUS_OK && query.token_count == 0) {
        /* No query tokens: an empty AllMatches. */
    } else if (status == SXML_STATUS_OK && words->any_all == SXML_FULL_TEXT_ANY_ALL_PHRASE) {
        phrase.first_query = 0;
        phrase.query_count = query.token_count;
        phrase.query_position = state->next_query_position;
        state->next_query_position += 1;
        status = match_phrase(state, phrase, &combined);
    } else if (status == SXML_STATUS_OK) {
        const bool by_word = words->any_all == SXML_FULL_TEXT_ANY_ALL_ANY_WORD ||
                             words->any_all == SXML_FULL_TEXT_ANY_ALL_ALL_WORDS;
        const size_t part_count = by_word ? query.token_count : query.string_count;
        for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < part_count;
             part_index += 1) {
            all_matches part_matches;
            phrase.query_position = state->next_query_position;
            state->next_query_position += 1;
            if (by_word) {
                phrase.first_query = part_index;
                phrase.query_count = 1;
                status = match_phrase(state, phrase, &part_matches);
            } else {
                status = match_query_string(
                    state, (string_phrase){.phrase = phrase, .string_index = part_index},
                    &part_matches);
            }
            if (status != SXML_STATUS_OK) {
                break;
            }
            const operand_pair pair = {.left = combined, .right = part_matches};
            if (part_index == 0) {
                combined = part_matches;
            } else if (is_conjunction) {
                status = apply_and(state, pair, &combined);
            } else {
                status = apply_or(state, pair, &combined);
            }
        }
    }
    release_query_tokens(&query);
    *result = combined;
    return status;
}

/* --------------------------------------------- FTUnaryNot (4.2.6.3) */

/* fts:ApplyFTUnaryNot: one match per choice of one StringMatch from each
   input match, every choice inverted; one empty match for no matches. */
static sxml_status apply_not(matcher *state, all_matches operand, all_matches *result) {
    const size_t first_match = state->match_count;
    size_t combination_count = 1;
    for (size_t match = 0; match < operand.match_count; match += 1) {
        const size_t entry_count = state->matches[operand.first_match + match].string_match_count;
        if (entry_count != 0 && combination_count > SXML_FULL_TEXT_MAXIMUM_ROWS / entry_count) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        combination_count *= entry_count;
    }
    sxml_status status = SXML_STATUS_OK;
    size_t *choices = NULL;
    if (operand.match_count != 0) {
        status = sxml_allocator_allocate(
            state->allocator,
            (sxml_allocation_request){.byte_count = operand.match_count * sizeof(size_t),
                                      .alignment = alignof(size_t)},
            (void **)&choices);
        if (status == SXML_STATUS_OK) {
            memset(choices, 0, operand.match_count * sizeof(size_t));
        }
    }
    for (size_t combination = 0; status == SXML_STATUS_OK && combination < combination_count;
         combination += 1) {
        const size_t first_string_match = state->string_match_count;
        for (size_t match = 0; status == SXML_STATUS_OK && match < operand.match_count;
             match += 1) {
            const match_row row = state->matches[operand.first_match + match];
            status = copy_string_match(state, (string_copy){.string_match_index = row.first_string_match + choices[match], .is_inverted = true});
        }
        if (status == SXML_STATUS_OK) {
            status = reserve_matches(state, 1);
        }
        if (status == SXML_STATUS_OK) {
            put_match(state, first_string_match);
        }
        /* The next choice, the last match's StringMatch counting fastest. */
        for (size_t match = operand.match_count; match > 0; match -= 1) {
            const size_t entry_count =
                state->matches[operand.first_match + match - 1].string_match_count;
            choices[match - 1] += 1;
            if (choices[match - 1] < entry_count) {
                break;
            }
            choices[match - 1] = 0;
        }
    }
    sxml_allocator_release(state->allocator, choices);
    *result = matches_since(state, first_match);
    return status;
}

/* ---------------------------------------------- FTMildNot (4.2.6.4) */

/* Whether any match of operand has a StringExclude. */
static bool has_exclude(const matcher *state, all_matches operand) {
    for (size_t match = 0; match < operand.match_count; match += 1) {
        const match_row row = state->matches[operand.first_match + match];
        for (size_t entry = 0; entry < row.string_match_count; entry += 1) {
            if (!state->string_matches[row.first_string_match + entry].is_include) {
                return true;
            }
        }
    }
    return false;
}

/* Whether position is covered by a StringInclude of match_index
   (fts:CoveredIncludePositions). */
typedef struct position_query {
    size_t match_index;
    uint32_t position;
} position_query;

static bool covers_position(const matcher *state, position_query query) {
    const match_row row = state->matches[query.match_index];
    for (size_t entry = 0; entry < row.string_match_count; entry += 1) {
        const string_match *string = &state->string_matches[row.first_string_match + entry];
        if (string->is_include && string->span.start_position <= query.position &&
            query.position <= string->span.end_position) {
            return true;
        }
    }
    return false;
}

/* Whether some position the includes of left_match cover is not covered
   by the includes of right_match. */
static bool has_uncovered_position(const matcher *state, size_t left_match, size_t right_match) {
    const match_row row = state->matches[left_match];
    for (size_t entry = 0; entry < row.string_match_count; entry += 1) {
        const string_match *string = &state->string_matches[row.first_string_match + entry];
        if (!string->is_include) {
            continue;
        }
        for (uint32_t position = string->span.start_position;
             position <= string->span.end_position; position += 1) {
            if (!covers_position(state, (position_query){.match_index = right_match,
                                                         .position = position})) {
                return true;
            }
        }
    }
    return false;
}

static sxml_status apply_mild_not(matcher *state, operand_pair pair, all_matches *result) {
    const all_matches left = pair.left;
    const all_matches right = pair.right;
    if (has_exclude(state, left) || has_exclude(state, right)) {
        return sxml_function_library_report(state->request->call, &mild_not_error);
    }
    if (right.match_count == 0) {
        *result = left;
        return SXML_STATUS_OK;
    }
    bool right_has_include = false;
    for (size_t match = 0; match < right.match_count; match += 1) {
        right_has_include =
            right_has_include || state->matches[right.first_match + match].string_match_count != 0;
    }
    if (!right_has_include) {
        *result = left;
        return SXML_STATUS_OK;
    }
    const size_t first_match = state->match_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t match = 0; status == SXML_STATUS_OK && match < left.match_count; match += 1) {
        bool is_kept = true;
        for (size_t other = 0; is_kept && other < right.match_count; other += 1) {
            is_kept = has_uncovered_position(state, left.first_match + match,
                                             right.first_match + other);
        }
        if (!is_kept) {
            continue;
        }
        status = reserve_matches(state, 1);
        if (status == SXML_STATUS_OK) {
            state->matches[state->match_count] = state->matches[left.first_match + match];
            state->match_count += 1;
        }
    }
    *result = matches_since(state, first_match);
    return status;
}

/* ------------------------------------------------ FTTimes (4.2.6.10) */

/* The bounds of an FTRange: minimum, and maximum or none. */
typedef struct range_bounds {
    int64_t minimum;
    int64_t maximum;
    bool has_maximum;
} range_bounds;

static sxml_status read_range(const matcher *state, const uint32_t *arguments,
                              range_bounds *bounds) {
    *bounds = (range_bounds){.minimum = 0, .maximum = 0, .has_maximum = false};
    sxml_status status = SXML_STATUS_OK;
    if (arguments[0] != 0) {
        status = read_integer(state, arguments[0], &bounds->minimum);
    }
    if (status == SXML_STATUS_OK && arguments[1] != 0) {
        bounds->has_maximum = true;
        status = read_integer(state, arguments[1], &bounds->maximum);
    }
    return status;
}

/* One combination of occurrence_count of the operand's matches: their
   StringMatches, and the others' inverted when excludes_others. */
typedef struct combination_request {
    all_matches operand;
    const size_t *chosen;
    size_t chosen_count;
    bool excludes_others;
} combination_request;

static sxml_status add_combination(matcher *state, combination_request request) {
    const size_t first_string_match = state->string_match_count;
    sxml_status status = SXML_STATUS_OK;
    size_t chosen_index = 0;
    for (size_t match = 0; status == SXML_STATUS_OK && match < request.operand.match_count;
         match += 1) {
        const bool is_chosen =
            chosen_index < request.chosen_count && request.chosen[chosen_index] == match;
        if (is_chosen) {
            chosen_index += 1;
        }
        if (is_chosen || request.excludes_others) {
            status = copy_match_entries(state,
                                        (match_copy){.match_index = request.operand.first_match +
                                                                    match,
                                                     .is_inverted = !is_chosen});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = reserve_matches(state, 1);
    }
    if (status == SXML_STATUS_OK) {
        put_match(state, first_string_match);
    }
    return status;
}

/* Every combination of chosen_count of the operand's matches, in
   lexicographic order (fts:FormCombinations as a loop). */
static sxml_status add_combinations(matcher *state, combination_request request,
                                    size_t *chosen) {
    const size_t total = request.operand.match_count;
    const size_t count = request.chosen_count;
    for (size_t position = 0; position < count; position += 1) {
        chosen[position] = position;
    }
    request.chosen = chosen;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        status = add_combination(state, request);
        size_t position = count;
        while (position > 0 && chosen[position - 1] == total - count + position - 1) {
            position -= 1;
        }
        if (position == 0) {
            break;
        }
        chosen[position - 1] += 1;
        for (size_t later = position; later < count; later += 1) {
            chosen[later] = chosen[later - 1] + 1;
        }
    }
    return status;
}

static sxml_status apply_times(matcher *state, filter_input input, all_matches *result) {
    const uint32_t *arguments = input.arguments;
    const all_matches operand = input.operand;
    range_bounds bounds;
    sxml_status status = read_range(state, arguments, &bounds);
    const size_t first_match = state->match_count;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const int64_t occurrences = (int64_t)operand.match_count;
    int64_t lowest = bounds.minimum < 0 ? 0 : bounds.minimum;
    int64_t highest = bounds.has_maximum ? bounds.maximum : lowest;
    if (highest > occurrences) {
        highest = occurrences;
    }
    size_t *chosen = NULL;
    if (operand.match_count != 0) {
        status = sxml_allocator_allocate(
            state->allocator,
            (sxml_allocation_request){.byte_count = operand.match_count * sizeof(size_t),
                                      .alignment = alignof(size_t)},
            (void **)&chosen);
    }
    for (int64_t count = lowest; status == SXML_STATUS_OK && count <= highest; count += 1) {
        status = add_combinations(state,
                                  (combination_request){.operand = operand,
                                                        .chosen_count = (size_t)count,
                                                        .excludes_others = bounds.has_maximum},
                                  chosen);
    }
    sxml_allocator_release(state->allocator, chosen);
    *result = matches_since(state, first_match);
    return status;
}

/* ------------------------------------------------ FTOrder (4.2.6.5) */

/* Whether two StringMatches are in the order of their query positions. */
static bool are_in_order(const string_match *first, const string_match *second) {
    return (first->span.start_position <= second->span.start_position &&
            first->query_position <= second->query_position) ||
           (first->span.start_position >= second->span.start_position &&
            first->query_position >= second->query_position);
}

/* Whether entry of row is in order with every include of row. */
static bool is_in_order_with_includes(const matcher *state, match_row row,
                                      const string_match *entry) {
    for (size_t other = 0; other < row.string_match_count; other += 1) {
        const string_match *include = &state->string_matches[row.first_string_match + other];
        if (include->is_include && !are_in_order(entry, include)) {
            return false;
        }
    }
    return true;
}

static sxml_status apply_order(matcher *state, all_matches operand, all_matches *result) {
    const size_t first_match = state->match_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t match = 0; status == SXML_STATUS_OK && match < operand.match_count; match += 1) {
        const match_row row = state->matches[operand.first_match + match];
        bool is_ordered = true;
        for (size_t entry = 0; is_ordered && entry < row.string_match_count; entry += 1) {
            const string_match *include = &state->string_matches[row.first_string_match + entry];
            is_ordered = !include->is_include || is_in_order_with_includes(state, row, include);
        }
        if (!is_ordered) {
            continue;
        }
        const size_t first_string_match = state->string_match_count;
        for (size_t entry = 0; status == SXML_STATUS_OK && entry < row.string_match_count;
             entry += 1) {
            const string_match *string = &state->string_matches[row.first_string_match + entry];
            if (string->is_include || is_in_order_with_includes(state, row, string)) {
                status = copy_string_match(state, (string_copy){.string_match_index = row.first_string_match + entry, .is_inverted = false});
            }
        }
        if (status == SXML_STATUS_OK) {
            status = reserve_matches(state, 1);
        }
        if (status == SXML_STATUS_OK) {
            put_match(state, first_string_match);
        }
    }
    *result = matches_since(state, first_match);
    return status;
}

/* ------------------------------------- Units, spans and joinIncludes */

/* The first and last unit of a span: positions, sentences or
   paragraphs. */
typedef struct unit_span {
    int64_t first;
    int64_t last;
} unit_span;

static unit_span unit_span_of(const token_span *span, sxml_full_text_unit unit) {
    switch (unit) {
    case SXML_FULL_TEXT_UNIT_SENTENCES:
        return (unit_span){.first = span->start_sentence, .last = span->end_sentence};
    case SXML_FULL_TEXT_UNIT_PARAGRAPHS:
        return (unit_span){.first = span->start_paragraph, .last = span->end_paragraph};
    case SXML_FULL_TEXT_UNIT_WORDS:
    default:
        return (unit_span){.first = span->start_position, .last = span->end_position};
    }
}

/* Appends fts:joinIncludes of row's includes: one StringInclude over
   them all, contiguous when every position between is covered and every
   include is contiguous (4.2.6.8). Nothing for a match without includes. */
static sxml_status join_includes(matcher *state, size_t match_index) {
    const match_row row = state->matches[match_index];
    bool has_include = false;
    string_match joined = {.is_include = true, .is_contiguous = true};
    for (size_t entry = 0; entry < row.string_match_count; entry += 1) {
        const string_match *string = &state->string_matches[row.first_string_match + entry];
        if (!string->is_include) {
            continue;
        }
        if (!has_include) {
            joined.span = string->span;
            joined.query_position = string->query_position;
            has_include = true;
        } else {
            const token_span *span = &string->span;
            joined.span.start_position = span->start_position < joined.span.start_position
                                             ? span->start_position
                                             : joined.span.start_position;
            joined.span.end_position = span->end_position > joined.span.end_position
                                           ? span->end_position
                                           : joined.span.end_position;
            joined.span.start_sentence = span->start_sentence < joined.span.start_sentence
                                             ? span->start_sentence
                                             : joined.span.start_sentence;
            joined.span.end_sentence = span->end_sentence > joined.span.end_sentence
                                           ? span->end_sentence
                                           : joined.span.end_sentence;
            joined.span.start_paragraph = span->start_paragraph < joined.span.start_paragraph
                                              ? span->start_paragraph
                                              : joined.span.start_paragraph;
            joined.span.end_paragraph = span->end_paragraph > joined.span.end_paragraph
                                            ? span->end_paragraph
                                            : joined.span.end_paragraph;
        }
        joined.is_contiguous = joined.is_contiguous && string->is_contiguous;
    }
    if (!has_include) {
        return SXML_STATUS_OK;
    }
    for (uint32_t position = joined.span.start_position;
         joined.is_contiguous && position <= joined.span.end_position; position += 1) {
        joined.is_contiguous =
            covers_position(state, (position_query){.match_index = match_index,
                                                    .position = position});
    }
    const sxml_status status = reserve_string_matches(state, 1);
    if (status == SXML_STATUS_OK) {
        put_string_match(state, joined);
    }
    return status;
}

/* The smallest first unit and largest last unit of row's includes;
   false without includes. */
typedef struct include_extent {
    bool has_include;
    int64_t first;
    int64_t last;
} include_extent;

static include_extent extent_of(const matcher *state, match_row row, sxml_full_text_unit unit) {
    include_extent extent = {.has_include = false};
    for (size_t entry = 0; entry < row.string_match_count; entry += 1) {
        const string_match *string = &state->string_matches[row.first_string_match + entry];
        if (!string->is_include) {
            continue;
        }
        const unit_span span = unit_span_of(&string->span, unit);
        if (!extent.has_include || span.first < extent.first) {
            extent.first = span.first;
        }
        if (!extent.has_include || span.last > extent.last) {
            extent.last = span.last;
        }
        extent.has_include = true;
    }
    return extent;
}

/* ----------------------------------------------- FTWindow (4.2.6.8) */

/* A window: its unit and first and last unit. */
typedef struct window_frame {
    sxml_full_text_unit unit;
    int64_t first;
    int64_t last;
} window_frame;

static bool is_in_window(const string_match *string, window_frame frame) {
    const unit_span span = unit_span_of(&string->span, frame.unit);
    return span.first >= frame.first && span.last <= frame.last;
}

/* Two windows over one match. */
typedef struct window_pair {
    match_row row;
    window_frame first;
    window_frame second;
} window_pair;

/* Whether the excludes retained by two windows differ. */
static bool retains_differently(const matcher *state, window_pair pair) {
    const match_row row = pair.row;
    for (size_t entry = 0; entry < row.string_match_count; entry += 1) {
        const string_match *string = &state->string_matches[row.first_string_match + entry];
        if (!string->is_include &&
            is_in_window(string, pair.first) != is_in_window(string, pair.second)) {
            return true;
        }
    }
    return false;
}

/* The joined includes, then the excludes inside frame. */
static sxml_status add_window_match(matcher *state, size_t match_index, window_frame frame) {
    const size_t first_string_match = state->string_match_count;
    sxml_status status = join_includes(state, match_index);
    const match_row row = state->matches[match_index];
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < row.string_match_count;
         entry += 1) {
        const string_match *string = &state->string_matches[row.first_string_match + entry];
        if (!string->is_include && is_in_window(string, frame)) {
            status = copy_string_match(state, (string_copy){.string_match_index = row.first_string_match + entry, .is_inverted = false});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = reserve_matches(state, 1);
    }
    if (status == SXML_STATUS_OK) {
        put_match(state, first_string_match);
    }
    return status;
}

static sxml_status apply_window(matcher *state, filter_input input, all_matches *result) {
    const uint32_t *arguments = input.arguments;
    const all_matches operand = input.operand;
    int64_t size = 0;
    sxml_status status = read_integer(state, arguments[0], &size);
    const sxml_full_text_unit unit = (sxml_full_text_unit)arguments[1];
    const size_t first_match = state->match_count;
    for (size_t match = 0; status == SXML_STATUS_OK && match < operand.match_count; match += 1) {
        const size_t match_index = operand.first_match + match;
        const match_row row = state->matches[match_index];
        const include_extent extent = extent_of(state, row, unit);
        if (!extent.has_include || size <= 0 || extent.last - extent.first + 1 > size) {
            continue;
        }
        /* for $windowStartPos in ($maxpos - $n + 1 to $minpos): one match
           per distinct set of retained excludes. */
        window_frame previous = {.unit = unit};
        bool has_previous = false;
        for (int64_t start = extent.last - size + 1; status == SXML_STATUS_OK && start <= extent.first;
             start += 1) {
            const window_frame frame = {.unit = unit, .first = start, .last = start + size - 1};
            if (has_previous &&
                !retains_differently(state,
                                     (window_pair){.row = row, .first = previous, .second = frame})) {
                continue;
            }
            status = add_window_match(state, match_index, frame);
            previous = frame;
            has_previous = true;
            if (start < extent.first && !has_exclude(state, (all_matches){.first_match = match_index,
                                                                         .match_count = 1})) {
                break;
            }
        }
    }
    *result = matches_since(state, first_match);
    return status;
}

/* --------------------------------------------- FTDistance (4.2.6.9) */

/* The distance in units between two StringMatches, ordered by start and
   end position (fts:wordDistance, fts:sentenceDistance,
   fts:paraDistance). */
static int64_t distance_between(const string_match *first, const string_match *second,
                                sxml_full_text_unit unit) {
    const string_match *earlier = first;
    const string_match *later = second;
    if (second->span.start_position < first->span.start_position ||
        (second->span.start_position == first->span.start_position &&
         second->span.end_position < first->span.end_position)) {
        earlier = second;
        later = first;
    }
    return unit_span_of(&later->span, unit).first - unit_span_of(&earlier->span, unit).last - 1;
}

static bool is_in_range(int64_t distance, range_bounds bounds) {
    return distance >= bounds.minimum && (!bounds.has_maximum || distance <= bounds.maximum);
}

/* The includes of row in order of their first unit, then last unit
   (the $sorted of the distance functions); a selection sort over
   indices, the includes being few. */
typedef struct sorted_includes {
    size_t *entries;
    size_t entry_count;
} sorted_includes;

/* The match and unit to sort by. */
typedef struct include_order {
    match_row row;
    sxml_full_text_unit unit;
} include_order;

static void sort_includes(const matcher *state, include_order order, sorted_includes *sorted) {
    const match_row row = order.row;
    const sxml_full_text_unit unit = order.unit;
    sorted->entry_count = 0;
    for (size_t entry = 0; entry < row.string_match_count; entry += 1) {
        if (state->string_matches[row.first_string_match + entry].is_include) {
            sorted->entries[sorted->entry_count] = row.first_string_match + entry;
            sorted->entry_count += 1;
        }
    }
    for (size_t position = 0; position < sorted->entry_count; position += 1) {
        size_t smallest = position;
        for (size_t other = position + 1; other < sorted->entry_count; other += 1) {
            const unit_span candidate =
                unit_span_of(&state->string_matches[sorted->entries[other]].span, unit);
            const unit_span best =
                unit_span_of(&state->string_matches[sorted->entries[smallest]].span, unit);
            if (candidate.first < best.first ||
                (candidate.first == best.first && candidate.last < best.last)) {
                smallest = other;
            }
        }
        const size_t swapped = sorted->entries[position];
        sorted->entries[position] = sorted->entries[smallest];
        sorted->entries[smallest] = swapped;
    }
}

/* Whether some include of row is within the distance range of entry. */
typedef struct distance_check {
    match_row row;
    const string_match *entry;
    range_bounds bounds;
    sxml_full_text_unit unit;
} distance_check;

static bool is_near_an_include(const matcher *state, distance_check check) {
    for (size_t other = 0; other < check.row.string_match_count; other += 1) {
        const string_match *include = &state->string_matches[check.row.first_string_match + other];
        if (include->is_include &&
            is_in_range(distance_between(include, check.entry, check.unit), check.bounds)) {
            return true;
        }
    }
    return false;
}

static sxml_status add_distance_match(matcher *state, size_t match_index,
                                      distance_check check) {
    const size_t first_string_match = state->string_match_count;
    sxml_status status = join_includes(state, match_index);
    const match_row row = state->matches[match_index];
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < row.string_match_count;
         entry += 1) {
        const string_match *string = &state->string_matches[row.first_string_match + entry];
        check.entry = string;
        if (!string->is_include && is_near_an_include(state, check)) {
            status = copy_string_match(state, (string_copy){.string_match_index = row.first_string_match + entry, .is_inverted = false});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = reserve_matches(state, 1);
    }
    if (status == SXML_STATUS_OK) {
        put_match(state, first_string_match);
    }
    return status;
}

static sxml_status apply_distance(matcher *state, filter_input input, all_matches *result) {
    const uint32_t *arguments = input.arguments;
    const all_matches operand = input.operand;
    range_bounds bounds;
    sxml_status status = read_range(state, arguments, &bounds);
    const sxml_full_text_unit unit = (sxml_full_text_unit)arguments[2];
    const size_t first_match = state->match_count;
    size_t largest = 1;
    for (size_t match = 0; match < operand.match_count; match += 1) {
        const size_t count = state->matches[operand.first_match + match].string_match_count;
        largest = count > largest ? count : largest;
    }
    sorted_includes sorted = {.entries = NULL, .entry_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            state->allocator,
            (sxml_allocation_request){.byte_count = largest * sizeof(size_t),
                                      .alignment = alignof(size_t)},
            (void **)&sorted.entries);
    }
    for (size_t match = 0; status == SXML_STATUS_OK && match < operand.match_count; match += 1) {
        const size_t match_index = operand.first_match + match;
        const match_row row = state->matches[match_index];
        sort_includes(state, (include_order){.row = row, .unit = unit}, &sorted);
        bool is_kept = true;
        for (size_t position = 0; is_kept && position + 1 < sorted.entry_count; position += 1) {
            is_kept = is_in_range(
                distance_between(&state->string_matches[sorted.entries[position]],
                                 &state->string_matches[sorted.entries[position + 1]], unit),
                bounds);
        }
        if (is_kept) {
            status = add_distance_match(
                state, match_index, (distance_check){.row = row, .bounds = bounds, .unit = unit});
        }
    }
    sxml_allocator_release(state->allocator, sorted.entries);
    *result = matches_since(state, first_match);
    return status;
}

/* ------------------------------------------------ FTScope (4.2.6.6) */

/* The first and last sentence or paragraph of a StringMatch. */
static unit_span scope_span(const string_match *string, sxml_full_text_unit unit) {
    return unit_span_of(&string->span, unit);
}

/* fts:ApplyFTScopeSameSentence and ...SameParagraph: whether the match's
   includes all lie in one unit. */
/* A match and the unit of its scope. */
typedef struct scope_query {
    match_row row;
    sxml_full_text_unit unit;
} scope_query;

static bool is_same_scope(const matcher *state, scope_query query) {
    const match_row row = query.row;
    const sxml_full_text_unit unit = query.unit;
    bool has_unit = false;
    int64_t common_unit = 0;
    for (size_t entry = 0; entry < row.string_match_count; entry += 1) {
        const string_match *string = &state->string_matches[row.first_string_match + entry];
        if (!string->is_include) {
            continue;
        }
        const unit_span span = scope_span(string, unit);
        if (span.first != span.last || span.first <= 0 ||
            (has_unit && span.first != common_unit)) {
            return false;
        }
        common_unit = span.first;
        has_unit = true;
    }
    return true;
}

/* ...DifferentSentence and ...DifferentParagraph: more than one include,
   no two of them in one unit. */
static bool is_different_scope(const matcher *state, scope_query query) {
    const match_row row = query.row;
    const sxml_full_text_unit unit = query.unit;
    size_t include_count = 0;
    for (size_t entry = 0; entry < row.string_match_count; entry += 1) {
        const string_match *first = &state->string_matches[row.first_string_match + entry];
        if (!first->is_include) {
            continue;
        }
        include_count += 1;
        for (size_t other = entry + 1; other < row.string_match_count; other += 1) {
            const string_match *second = &state->string_matches[row.first_string_match + other];
            if (!second->is_include) {
                continue;
            }
            const unit_span first_span = scope_span(first, unit);
            const unit_span second_span = scope_span(second, unit);
            const bool differ = first_span.first != second_span.first ||
                                first_span.first != first_span.last ||
                                second_span.first != second_span.last;
            if (!differ || first_span.first <= 0 || second_span.last <= 0) {
                return false;
            }
        }
    }
    return include_count > 1;
}

/* Whether an exclude is retained by the scope. */
typedef struct scope_check {
    match_row row;
    const string_match *exclude;
    bool is_different;
    sxml_full_text_unit unit;
} scope_check;

static bool retains_exclude(const matcher *state, scope_check check) {
    const unit_span exclude_span = scope_span(check.exclude, check.unit);
    if (!check.is_different && exclude_span.first == 0) {
        return true;
    }
    if (!check.is_different && exclude_span.first != exclude_span.last) {
        return false;
    }
    for (size_t entry = 0; entry < check.row.string_match_count; entry += 1) {
        const string_match *include = &state->string_matches[check.row.first_string_match + entry];
        if (!include->is_include) {
            continue;
        }
        const unit_span include_span = scope_span(include, check.unit);
        if (check.is_different) {
            if (include_span.first == exclude_span.first && include_span.first == include_span.last &&
                exclude_span.first == exclude_span.last) {
                return false;
            }
        } else if (include_span.first != exclude_span.first) {
            return false;
        }
    }
    return true;
}

static sxml_status apply_scope(matcher *state, filter_input input, all_matches *result) {
    const uint32_t *arguments = input.arguments;
    const all_matches operand = input.operand;
    const bool is_different = arguments[0] != 0;
    const sxml_full_text_unit unit =
        arguments[1] != 0 ? SXML_FULL_TEXT_UNIT_PARAGRAPHS : SXML_FULL_TEXT_UNIT_SENTENCES;
    const size_t first_match = state->match_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t match = 0; status == SXML_STATUS_OK && match < operand.match_count; match += 1) {
        const match_row row = state->matches[operand.first_match + match];
        const scope_query query = {.row = row, .unit = unit};
        if (is_different ? !is_different_scope(state, query) : !is_same_scope(state, query)) {
            continue;
        }
        const size_t first_string_match = state->string_match_count;
        for (size_t entry = 0; status == SXML_STATUS_OK && entry < row.string_match_count;
             entry += 1) {
            const string_match *string = &state->string_matches[row.first_string_match + entry];
            if (string->is_include ||
                retains_exclude(state, (scope_check){.row = row,
                                                     .exclude = string,
                                                     .is_different = is_different,
                                                     .unit = unit})) {
                status = copy_string_match(state, (string_copy){.string_match_index = row.first_string_match + entry, .is_inverted = false});
            }
        }
        if (status == SXML_STATUS_OK) {
            status = reserve_matches(state, 1);
        }
        if (status == SXML_STATUS_OK) {
            put_match(state, first_string_match);
        }
    }
    *result = matches_since(state, first_match);
    return status;
}

/* ----------------------------------------------- FTContent (4.2.6.7) */

/* Whether some include of row (a contiguous one, for entire content)
   covers position. */
typedef struct coverage_query {
    match_row row;
    uint32_t position;
    bool needs_contiguous;
} coverage_query;

static bool is_covered(const matcher *state, coverage_query query) {
    for (size_t entry = 0; entry < query.row.string_match_count; entry += 1) {
        const string_match *string = &state->string_matches[query.row.first_string_match + entry];
        if (string->is_include && (!query.needs_contiguous || string->is_contiguous) &&
            string->span.start_position <= query.position &&
            query.position <= string->span.end_position) {
            return true;
        }
    }
    return false;
}

static sxml_status apply_content(matcher *state, filter_input input, all_matches *result) {
    const uint32_t *arguments = input.arguments;
    const all_matches operand = input.operand;
    const sxml_full_text_content content = (sxml_full_text_content)arguments[0];
    const size_t token_count = state->request->search.token_count;
    const uint32_t lowest = 1;
    const uint32_t highest = (uint32_t)token_count;
    const size_t first_match = state->match_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t match = 0;
         token_count != 0 && status == SXML_STATUS_OK && match < operand.match_count;
         match += 1) {
        const match_row row = state->matches[operand.first_match + match];
        bool is_kept = true;
        if (content == SXML_FULL_TEXT_CONTENT_ENTIRE) {
            for (uint32_t position = lowest; is_kept && position <= highest; position += 1) {
                is_kept = is_covered(state, (coverage_query){.row = row,
                                                             .position = position,
                                                             .needs_contiguous = true});
            }
        } else {
            is_kept = is_covered(
                state,
                (coverage_query){.row = row,
                                 .position = content == SXML_FULL_TEXT_CONTENT_AT_START ? lowest
                                                                                       : highest,
                                 .needs_contiguous = false});
        }
        if (!is_kept) {
            continue;
        }
        status = reserve_matches(state, 1);
        if (status == SXML_STATUS_OK) {
            state->matches[state->match_count] = row;
            state->match_count += 1;
        }
    }
    *result = matches_since(state, first_match);
    return status;
}

/* ---------------------------------------------- Weights (3.1.1) */

static sxml_status check_weight(const matcher *state, uint32_t member_number) {
    sxml_sequence member;
    sxml_status status = read_member(state, member_number, &member);
    sxml_item item;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(member,
                                        (sxml_item_request){.item_index = 0,
                                                            .error_record =
                                                                state->request->call->error_record},
                                        &item);
    }
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    if (status != SXML_STATUS_OK) {
        return status == SXML_STATUS_INDEX_OUT_OF_RANGE ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    const double weight = datum.datum_details.double_number;
    /* NaN is not within the range either. */
    if (!(weight >= -1000.0 && weight <= 1000.0)) {
        return sxml_function_library_report(state->request->call, &weight_error);
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ The program */

/* One decoded instruction. */
typedef struct instruction {
    uint8_t opcode;
    uint32_t arguments[SXML_FULL_TEXT_MAXIMUM_ARGUMENT_COUNT];
    size_t argument_count;
} instruction;

/* Reads the instruction at *byte_offset of text (see the header). */
static sxml_status read_instruction(sxml_byte_view text, size_t *byte_offset,
                                    instruction *decoded) {
    size_t offset = *byte_offset;
    *decoded = (instruction){.opcode = text.bytes[offset]};
    offset += 1;
    while (offset < text.byte_count && text.bytes[offset] != ';') {
        if (decoded->argument_count == SXML_FULL_TEXT_MAXIMUM_ARGUMENT_COUNT) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        uint64_t value = 0;
        while (offset < text.byte_count && text.bytes[offset] >= '0' && text.bytes[offset] <= '9') {
            value = value * 10U + (uint64_t)(text.bytes[offset] - '0');
            if (value > UINT32_MAX) {
                return SXML_STATUS_INVALID_ARGUMENT;
            }
            offset += 1;
        }
        decoded->arguments[decoded->argument_count] = (uint32_t)value;
        decoded->argument_count += 1;
        if (offset < text.byte_count && text.bytes[offset] == ',') {
            offset += 1;
        }
    }
    if (offset >= text.byte_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *byte_offset = offset + 1;
    return SXML_STATUS_OK;
}

/* The words instruction's fields. */
static sxml_status decode_words(const instruction *decoded, words_instruction *words) {
    if (decoded->argument_count != SXML_FULL_TEXT_MAXIMUM_ARGUMENT_COUNT ||
        decoded->arguments[1] > (uint32_t)SXML_FULL_TEXT_ANY_ALL_ALL_WORDS ||
        decoded->arguments[2] > (uint32_t)SXML_FULL_TEXT_CASE_UPPERCASE ||
        decoded->arguments[6] >= SXML_FULL_TEXT_LANGUAGE_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *words = (words_instruction){
        .words_member = decoded->arguments[0],
        .any_all = (sxml_full_text_any_all)decoded->arguments[1],
        .options = {.case_mode = (sxml_full_text_case)decoded->arguments[2],
                    .is_diacritics_sensitive = decoded->arguments[3] != 0,
                    .uses_stemming = decoded->arguments[4] != 0,
                    .uses_wildcards = decoded->arguments[5] != 0},
        .language = (sxml_full_text_language)decoded->arguments[6],
        .stop_words_member = decoded->arguments[7]};
    return SXML_STATUS_OK;
}

/* Runs one instruction on the stack. */
static sxml_status execute(matcher *state, const instruction *decoded) {
    all_matches right = {0};
    all_matches left = {0};
    all_matches result = {0};
    sxml_status status = SXML_STATUS_OK;
    switch (decoded->opcode) {
    case SXML_FULL_TEXT_OPCODE_WORDS: {
        words_instruction words;
        status = decode_words(decoded, &words);
        if (status == SXML_STATUS_OK) {
            status = apply_words(state, &words, &result);
        }
        break;
    }
    case SXML_FULL_TEXT_OPCODE_AND:
    case SXML_FULL_TEXT_OPCODE_OR:
    case SXML_FULL_TEXT_OPCODE_MILD_NOT:
        status = pop(state, &right);
        if (status == SXML_STATUS_OK) {
            status = pop(state, &left);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        const operand_pair pair = {.left = left, .right = right};
        status = decoded->opcode == SXML_FULL_TEXT_OPCODE_AND ? apply_and(state, pair, &result)
                 : decoded->opcode == SXML_FULL_TEXT_OPCODE_OR
                     ? apply_or(state, pair, &result)
                     : apply_mild_not(state, pair, &result);
        break;
    default:
        status = pop(state, &left);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const filter_input input = {.arguments = decoded->arguments, .operand = left};
        switch (decoded->opcode) {
        case SXML_FULL_TEXT_OPCODE_NOT:
            status = apply_not(state, left, &result);
            break;
        case SXML_FULL_TEXT_OPCODE_TIMES:
            status = decoded->argument_count == 2
                         ? apply_times(state, input, &result)
                         : SXML_STATUS_INVALID_ARGUMENT;
            break;
        case SXML_FULL_TEXT_OPCODE_WEIGHT:
            result = left;
            status = decoded->argument_count == 1 ? check_weight(state, decoded->arguments[0])
                                                  : SXML_STATUS_INVALID_ARGUMENT;
            break;
        case SXML_FULL_TEXT_OPCODE_ORDER:
            status = apply_order(state, left, &result);
            break;
        case SXML_FULL_TEXT_OPCODE_WINDOW:
            status = decoded->argument_count == 2
                         ? apply_window(state, input, &result)
                         : SXML_STATUS_INVALID_ARGUMENT;
            break;
        case SXML_FULL_TEXT_OPCODE_DISTANCE:
            status = decoded->argument_count == 3
                         ? apply_distance(state, input, &result)
                         : SXML_STATUS_INVALID_ARGUMENT;
            break;
        case SXML_FULL_TEXT_OPCODE_SCOPE:
            status = decoded->argument_count == 2
                         ? apply_scope(state, input, &result)
                         : SXML_STATUS_INVALID_ARGUMENT;
            break;
        case SXML_FULL_TEXT_OPCODE_CONTENT:
            status = decoded->argument_count == 1
                         ? apply_content(state, input, &result)
                         : SXML_STATUS_INVALID_ARGUMENT;
            break;
        default:
            status = SXML_STATUS_INVALID_ARGUMENT;
            break;
        }
        break;
    }
    return status == SXML_STATUS_OK ? push(state, result) : status;
}

static void release_matcher(matcher *state) {
    sxml_allocator_release(state->allocator, state->string_matches);
    sxml_allocator_release(state->allocator, state->matches);
    sxml_allocator_release(state->allocator, state->stack);
    for (size_t variant = 0; variant < SXML_FULL_TEXT_VARIANT_COUNT; variant += 1) {
        sxml_full_text_bytes_release(&state->variants[variant].bytes);
        sxml_allocator_release(state->allocator, state->variants[variant].tokens);
    }
    sxml_full_text_stem_workspace_release(&state->stem_workspace);
}

sxml_status sxml_full_text_run(const sxml_full_text_run_request *request,
                               sxml_full_text_run_outcome *outcome) {
    outcome->is_match = false;
    matcher state = {.request = request,
                     .allocator = sxml_item_heap_as_allocator(request->call->item_heap),
                     .next_query_position = 1};
    for (size_t variant = 0; variant < SXML_FULL_TEXT_VARIANT_COUNT; variant += 1) {
        state.variants[variant].bytes.allocator = state.allocator;
    }
    state.stem_workspace.allocator = state.allocator;
    sxml_sequence program_text;
    sxml_status status = sxml_array_get_member(request->program, 0, &program_text);
    sxml_item text_item;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            program_text, (sxml_item_request){.item_index = 0, .error_record = NULL}, &text_item);
    }
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(text_item, &datum);
    }
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_STRING) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_byte_view text = status == SXML_STATUS_OK ? datum.datum_details.string.utf8
                                                         : (sxml_byte_view){.bytes = NULL,
                                                                            .byte_count = 0};
    for (size_t byte_offset = 0; status == SXML_STATUS_OK && byte_offset < text.byte_count;) {
        instruction decoded;
        status = read_instruction(text, &byte_offset, &decoded);
        if (status == SXML_STATUS_OK) {
            status = execute(&state, &decoded);
        }
    }
    all_matches all = {0};
    if (status == SXML_STATUS_OK) {
        status = state.stack_count == 1 ? pop(&state, &all) : SXML_STATUS_INVALID_ARGUMENT;
    }
    /* 4.3: true when some match has no StringExclude. */
    for (size_t match = 0;
         status == SXML_STATUS_OK && !outcome->is_match && match < all.match_count; match += 1) {
        outcome->is_match = !has_exclude(&state, (all_matches){.first_match = all.first_match + match,
                                                       .match_count = 1});
    }
    release_matcher(&state);
    return status;
}
