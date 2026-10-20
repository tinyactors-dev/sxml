/*
 * Full Text tokenization, folding and wildcards (src/full_text_internal.h,
 * "Tokens"; docs/DECISIONS.md D214). Part of the optional feature
 * full-text: only src/full_text.c and src/full_text_match.c call it.
 *
 * Golden file: src/regex.c (a compiled pattern run by a loop over state
 * sets, D53, D71).
 *
 * Section numbers are of XQuery and XPath Full Text 3.0 (W3C
 * Recommendation, 24 November 2015).
 */
#include "full_text_internal.h"

#include "function_library_core_internal.h"
#include "regex_internal.h"
#include "sxml/string.h"
#include "unicode_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Growth */

/* Grows a token list to hold one more token. */
static sxml_status reserve_token(sxml_full_text_token_list *list) {
    if (list->token_count < list->token_capacity) {
        return SXML_STATUS_OK;
    }
    if (list->token_capacity >= UINT32_MAX / 2U) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const size_t capacity = list->token_capacity == 0 ? 64U : list->token_capacity * 2U;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        list->allocator,
        (sxml_allocation_request){.byte_count = capacity * sizeof(sxml_full_text_token),
                                  .alignment = alignof(sxml_full_text_token)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (list->token_count != 0) {
        memcpy(memory, list->tokens, list->token_count * sizeof(sxml_full_text_token));
    }
    sxml_allocator_release(list->allocator, list->tokens);
    list->tokens = memory;
    list->token_capacity = capacity;
    return SXML_STATUS_OK;
}

void sxml_full_text_token_list_release(sxml_full_text_token_list *list) {
    sxml_allocator_release(list->allocator, list->tokens);
    list->tokens = NULL;
    list->token_count = 0;
    list->token_capacity = 0;
}

/* ------------------------------------------------------ Tokenization */

/* The categories a token consists of: L, M, N and Pc (4.1, sxml's
   implementation-defined tokenizer). */
static bool is_word_codepoint(uint32_t codepoint) {
    const uint32_t category_bit = 1U << (uint32_t)sxml_regex_get_general_category(codepoint);
    return (category_bit & (SXML_REGEX_CATEGORY_MASK_L | SXML_REGEX_CATEGORY_MASK_M |
                            SXML_REGEX_CATEGORY_MASK_N |
                            (1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_PC))) != 0;
}

/* Whether codepoint is an ideograph or a hiragana letter: a word by itself
   (UAX #29, "Word Boundaries": they are neither ALetter nor Katakana, so
   WB999 breaks around each), which is how Chinese and Japanese text is
   segmented without a dictionary (D214). The blocks of the Unicode 16.0
   Ideographic property, and Hiragana. */
static bool is_single_character_word(uint32_t codepoint) {
    static const uint32_t ranges[][2] = {
        {0x3005, 0x3007},   {0x3021, 0x3029},   {0x3038, 0x303C},   {0x3041, 0x309F},
        {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xF900, 0xFAFF},   {0x16FE4, 0x16FE4},
        {0x17000, 0x18CFF}, {0x18D00, 0x18D08}, {0x1B170, 0x1B2FB}, {0x20000, 0x2FA1F},
        {0x30000, 0x323AF},
    };
    for (size_t range = 0; range < sizeof ranges / sizeof ranges[0]; range += 1) {
        if (codepoint >= ranges[range][0] && codepoint <= ranges[range][1]) {
            return true;
        }
    }
    return false;
}

/* Whether codepoint ends a sentence: ".", "!", "?" and their ideographic
   and fullwidth forms. */
static bool is_sentence_end(uint32_t codepoint) {
    return codepoint == '.' || codepoint == '!' || codepoint == '?' || codepoint == 0x3002U ||
           codepoint == 0xFF01U || codepoint == 0xFF0EU || codepoint == 0xFF1FU;
}

static bool is_letter(uint32_t codepoint) {
    return ((1U << (uint32_t)sxml_regex_get_general_category(codepoint)) &
            SXML_REGEX_CATEGORY_MASK_L) != 0;
}

/* The codepoint at byte_offset, and 0 past the end. */
static uint32_t codepoint_at(sxml_byte_view text, size_t byte_offset) {
    if (byte_offset >= text.byte_count) {
        return 0;
    }
    return sxml_xml_decode_codepoint(text, byte_offset).codepoint;
}

/* The codepoint that ends just before byte_offset, and 0 at the start. */
static uint32_t codepoint_before(sxml_byte_view text, size_t byte_offset) {
    if (byte_offset == 0) {
        return 0;
    }
    size_t start = byte_offset - 1;
    while (start > 0 && (text.bytes[start] & 0xC0U) == 0x80U) {
        start -= 1;
    }
    return sxml_xml_decode_codepoint(text, start).codepoint;
}

/* Whether the codepoint at byte_offset (a non-word character) continues
   the token that ends before it: an apostrophe between letters, a full
   stop or comma between digits (UAX #29 MidLetter and MidNum, in part). */
static bool joins_token(sxml_byte_view text, size_t byte_offset,
                        sxml_xml_decoded_codepoint decoded) {
    const uint32_t codepoint = decoded.codepoint;
    const uint32_t before = codepoint_before(text, byte_offset);
    const uint32_t after = codepoint_at(text, byte_offset + decoded.encoded_byte_count);
    if (codepoint == 0x27U || codepoint == 0x2019U) {
        return is_letter(before) && is_letter(after);
    }
    if (codepoint == '.' || codepoint == ',') {
        return sxml_regex_is_decimal_digit(before) && sxml_regex_is_decimal_digit(after);
    }
    return false;
}

/* How many bytes from byte_offset a wildcard of query text takes in a
   token (3.4.2): "." with its qualifier, or "\" with the character it
   escapes; 0 when the character there starts none. */
static size_t wildcard_length(sxml_byte_view text, size_t byte_offset) {
    const uint8_t byte = text.bytes[byte_offset];
    if (byte == '\\') {
        if (byte_offset + 1 >= text.byte_count) {
            /* An unescaped backslash at the end: FTDY0020 when matched. */
            return 1;
        }
        /* An escaped word character stays in the token; an escaped
           punctuation character is punctuation (it separates tokens, as
           "specialist\." is the token "specialist"; is_escaped_separator). */
        const sxml_xml_decoded_codepoint escaped = sxml_xml_decode_codepoint(text, byte_offset + 1);
        return is_word_codepoint(escaped.codepoint) ? 1 + escaped.encoded_byte_count : 0;
    }
    if (byte != '.') {
        return 0;
    }
    size_t length = 1;
    if (byte_offset + 1 < text.byte_count) {
        const uint8_t qualifier = text.bytes[byte_offset + 1];
        if (qualifier == '?' || qualifier == '*' || qualifier == '+') {
            length = 2;
        } else if (qualifier == '{') {
            length = 2;
            while (byte_offset + length < text.byte_count && text.bytes[byte_offset + length] != '}' &&
                   (text.bytes[byte_offset + length] == ',' ||
                    (text.bytes[byte_offset + length] >= '0' &&
                     text.bytes[byte_offset + length] <= '9'))) {
                length += 1;
            }
            if (byte_offset + length < text.byte_count && text.bytes[byte_offset + length] == '}') {
                length += 1;
            }
        }
    }
    return length;
}

/* The bytes "\" and an escaped non-word character take in query text
   with wildcards, which separate tokens; 0 elsewhere. */
static size_t escaped_separator_length(sxml_byte_view text, size_t byte_offset) {
    if (text.bytes[byte_offset] != '\\' || byte_offset + 1 >= text.byte_count) {
        return 0;
    }
    const sxml_xml_decoded_codepoint escaped = sxml_xml_decode_codepoint(text, byte_offset + 1);
    return is_word_codepoint(escaped.codepoint) ? 0 : 1 + escaped.encoded_byte_count;
}

/* The tokenizer's place in a text. */
typedef struct tokenizer {
    const sxml_full_text_source *source;
    sxml_full_text_token_list *list;
    size_t break_index;
    bool has_sentence_end;
    bool has_paragraph_end;
    bool is_in_token;
    size_t token_start;
} tokenizer;

/* Ends the token that started at token_start before byte_offset. */
static sxml_status emit_token(tokenizer *state, size_t byte_offset) {
    sxml_full_text_token_list *list = state->list;
    state->is_in_token = false;
    sxml_status status = reserve_token(list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (byte_offset > UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_full_text_token token = {.byte_offset = (uint32_t)state->token_start,
                                  .byte_count = (uint32_t)(byte_offset - state->token_start),
                                  .position = 1,
                                  .sentence = 1,
                                  .paragraph = 1};
    if (list->token_count != 0) {
        const sxml_full_text_token *last = &list->tokens[list->token_count - 1];
        token.position = last->position + 1;
        token.paragraph = last->paragraph + (state->has_paragraph_end ? 1U : 0U);
        token.sentence =
            last->sentence + (state->has_paragraph_end || state->has_sentence_end ? 1U : 0U);
    }
    state->has_paragraph_end = false;
    state->has_sentence_end = false;
    list->tokens[list->token_count] = token;
    list->token_count += 1;
    return SXML_STATUS_OK;
}

sxml_status sxml_full_text_tokenize(const sxml_full_text_source *source,
                                    sxml_full_text_token_list *list) {
    const sxml_byte_view text = source->text;
    if (text.byte_count > UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    tokenizer state = {.source = source, .list = list};
    size_t byte_offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && byte_offset < text.byte_count) {
        while (status == SXML_STATUS_OK && state.break_index < source->paragraph_break_count &&
               source->paragraph_breaks[state.break_index] <= byte_offset) {
            /* A paragraph break ends the token before it (the caller puts
               no break inside markup of a word). */
            if (state.is_in_token) {
                status = emit_token(&state, byte_offset);
            }
            state.has_paragraph_end = true;
            state.break_index += 1;
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        const size_t separator =
            source->keeps_wildcards ? escaped_separator_length(text, byte_offset) : 0;
        if (separator != 0) {
            if (state.is_in_token) {
                status = emit_token(&state, byte_offset);
            }
            byte_offset += separator;
            continue;
        }
        const size_t wildcard =
            source->keeps_wildcards ? wildcard_length(text, byte_offset) : 0;
        if (wildcard != 0) {
            if (!state.is_in_token) {
                state.is_in_token = true;
                state.token_start = byte_offset;
            }
            byte_offset += wildcard;
            continue;
        }
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(text, byte_offset);
        if (is_single_character_word(decoded.codepoint)) {
            if (state.is_in_token) {
                status = emit_token(&state, byte_offset);
            }
            state.is_in_token = true;
            state.token_start = byte_offset;
            if (status == SXML_STATUS_OK) {
                status = emit_token(&state, byte_offset + decoded.encoded_byte_count);
            }
        } else if (is_word_codepoint(decoded.codepoint)) {
            if (!state.is_in_token) {
                state.is_in_token = true;
                state.token_start = byte_offset;
            }
        } else if (!state.is_in_token ||
                   !joins_token(text, byte_offset, decoded)) {
            if (state.is_in_token) {
                status = emit_token(&state, byte_offset);
            }
            if (is_sentence_end(decoded.codepoint)) {
                state.has_sentence_end = true;
            }
        }
        byte_offset += decoded.encoded_byte_count;
    }
    if (status == SXML_STATUS_OK && state.is_in_token) {
        status = emit_token(&state, text.byte_count);
    }
    return status;
}

/* ----------------------------------------------------------- Folding */

/* Appends the codepoints of word, case mapped as folding says. */
static sxml_status fold_case(const sxml_full_text_fold_request *request,
                             sxml_xml_buffer *output) {
    const sxml_byte_view word = request->word;
    sxml_status status = SXML_STATUS_OK;
    for (size_t byte_offset = 0; status == SXML_STATUS_OK && byte_offset < word.byte_count;) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(word, byte_offset);
        byte_offset += decoded.encoded_byte_count;
        if (!request->folding.folds_case) {
            status = sxml_xml_buffer_append_codepoint(output, decoded.codepoint);
            continue;
        }
        sxml_case_mapping mapping;
        sxml_function_library_map_case(
            (sxml_case_mapping_request){.codepoint = decoded.codepoint,
                                        .mapping_kind = request->folding.uses_upper_case
                                                            ? SXML_CASE_MAPPING_KIND_UPPER
                                                            : SXML_CASE_MAPPING_KIND_LOWER},
            &mapping);
        for (size_t mapped_index = 0;
             status == SXML_STATUS_OK && mapped_index < mapping.codepoint_count;
             mapped_index += 1) {
            status = sxml_xml_buffer_append_codepoint(output, mapping.codepoints[mapped_index]);
        }
    }
    return status;
}

sxml_status sxml_full_text_fold(const sxml_full_text_fold_request *request,
                                sxml_xml_buffer *output) {
    if (!request->folding.strips_diacritics) {
        return fold_case(request, output);
    }
    /* 3.4.6: diacritics insensitive compares without the combining marks
       of the canonical decomposition (NFD, then every Mn dropped). */
    sxml_xml_buffer cased = {.allocator = output->allocator};
    sxml_status status = fold_case(request, &cased);
    sxml_unicode_normalized_text decomposed = {.allocator = output->allocator};
    sxml_string input;
    if (status == SXML_STATUS_OK && cased.byte_count != 0) {
        status = sxml_string_from_utf8(
            (sxml_byte_view){.bytes = cased.bytes, .byte_count = cased.byte_count}, &input);
    }
    if (status == SXML_STATUS_OK && cased.byte_count != 0) {
        status = sxml_unicode_normalize(
            &(sxml_unicode_normalization_request){
                .allocator = output->allocator,
                .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFD,
                .input = input},
            &decomposed);
    }
    const sxml_byte_view text = sxml_unicode_normalized_text_view(&decomposed);
    for (size_t byte_offset = 0; status == SXML_STATUS_OK && byte_offset < text.byte_count;) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(text, byte_offset);
        byte_offset += decoded.encoded_byte_count;
        if (sxml_regex_get_general_category(decoded.codepoint) !=
            SXML_REGEX_GENERAL_CATEGORY_MN) {
            status = sxml_xml_buffer_append_codepoint(output, decoded.codepoint);
        }
    }
    sxml_unicode_normalized_text_release(&decomposed);
    sxml_xml_buffer_release(&cased);
    return status;
}

/* --------------------------------------------------------- Wildcards */

/* One element of a wildcard pattern: a literal codepoint, or any
   codepoint between minimum and maximum times (UINT32_MAX: no bound). */
typedef struct wildcard_element {
    bool is_any;
    uint32_t codepoint;
    uint32_t minimum;
    uint32_t maximum;
    /* The first of the element's states: one per count it can have seen
       (up to maximum, or up to minimum without a bound); one for a
       literal. */
    size_t first_state;
    size_t state_count;
} wildcard_element;

/* The most states a pattern may have: a bound such as .{0,100000} makes
   that many. */
#define SXML_FULL_TEXT_MAXIMUM_WILDCARD_STATES 65536U

/* Reads the decimal digits from *byte_offset; false when there are none
   or the number is too large. */
static bool read_count(sxml_byte_view pattern, size_t *byte_offset, uint32_t *count) {
    size_t offset = *byte_offset;
    uint64_t value = 0;
    while (offset < pattern.byte_count && pattern.bytes[offset] >= '0' &&
           pattern.bytes[offset] <= '9') {
        value = value * 10U + (uint64_t)(pattern.bytes[offset] - '0');
        if (value > SXML_FULL_TEXT_MAXIMUM_WILDCARD_STATES) {
            return false;
        }
        offset += 1;
    }
    if (offset == *byte_offset) {
        return false;
    }
    *byte_offset = offset;
    *count = (uint32_t)value;
    return true;
}

/* Reads the element at *byte_offset (3.4.2); false for invalid syntax. */
static bool read_wildcard_element(sxml_byte_view pattern, size_t *byte_offset,
                                  wildcard_element *element) {
    const uint8_t byte = pattern.bytes[*byte_offset];
    if (byte == '\\') {
        if (*byte_offset + 1 >= pattern.byte_count) {
            return false;
        }
        const sxml_xml_decoded_codepoint decoded =
            sxml_xml_decode_codepoint(pattern, *byte_offset + 1);
        *element = (wildcard_element){.codepoint = decoded.codepoint};
        *byte_offset += 1 + decoded.encoded_byte_count;
        return true;
    }
    if (byte != '.') {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(pattern, *byte_offset);
        *element = (wildcard_element){.codepoint = decoded.codepoint};
        *byte_offset += decoded.encoded_byte_count;
        return true;
    }
    *element = (wildcard_element){.is_any = true, .minimum = 1, .maximum = 1};
    size_t offset = *byte_offset + 1;
    const uint8_t qualifier = offset < pattern.byte_count ? pattern.bytes[offset] : 0;
    if (qualifier == '?') {
        element->minimum = 0;
        offset += 1;
    } else if (qualifier == '*') {
        element->minimum = 0;
        element->maximum = UINT32_MAX;
        offset += 1;
    } else if (qualifier == '+') {
        element->maximum = UINT32_MAX;
        offset += 1;
    } else if (qualifier == '{') {
        /* .{n,m} (FTDY0020 for anything else after ".{"). */
        offset += 1;
        if (!read_count(pattern, &offset, &element->minimum) || offset >= pattern.byte_count ||
            pattern.bytes[offset] != ',') {
            return false;
        }
        offset += 1;
        if (!read_count(pattern, &offset, &element->maximum) || offset >= pattern.byte_count ||
            pattern.bytes[offset] != '}' || element->maximum < element->minimum) {
            return false;
        }
        offset += 1;
    }
    *byte_offset = offset;
    return true;
}

/* A compiled pattern and the matcher's two state sets. */
typedef struct wildcard_matcher {
    sxml_allocator allocator;
    wildcard_element *elements;
    size_t element_count;
    size_t state_count;
    bool *current;
    bool *next;
} wildcard_matcher;

static void release_matcher(wildcard_matcher *matcher) {
    sxml_allocator_release(matcher->allocator, matcher->elements);
    sxml_allocator_release(matcher->allocator, matcher->current);
    sxml_allocator_release(matcher->allocator, matcher->next);
}

/* Compiles the pattern; outcome->is_invalid for bad syntax. */
static sxml_status compile_pattern(sxml_byte_view pattern, wildcard_matcher *matcher,
                                   sxml_full_text_wildcard_outcome *outcome) {
    size_t element_capacity = pattern.byte_count + 1;
    sxml_status status = sxml_allocator_allocate(
        matcher->allocator,
        (sxml_allocation_request){.byte_count = element_capacity * sizeof(wildcard_element),
                                  .alignment = alignof(wildcard_element)},
        (void **)&matcher->elements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t state_count = 0;
    for (size_t byte_offset = 0; byte_offset < pattern.byte_count;) {
        wildcard_element element;
        if (!read_wildcard_element(pattern, &byte_offset, &element)) {
            outcome->is_invalid = true;
            return SXML_STATUS_OK;
        }
        element.first_state = state_count;
        element.state_count =
            !element.is_any ? 1U
                            : (size_t)(element.maximum == UINT32_MAX ? element.minimum
                                                                     : element.maximum) +
                                  1U;
        state_count += element.state_count;
        if (state_count > SXML_FULL_TEXT_MAXIMUM_WILDCARD_STATES) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        matcher->elements[matcher->element_count] = element;
        matcher->element_count += 1;
    }
    /* One more state: the end of the pattern. */
    matcher->state_count = state_count + 1;
    status = sxml_allocator_allocate(
        matcher->allocator,
        (sxml_allocation_request){.byte_count = matcher->state_count, .alignment = 1},
        (void **)&matcher->current);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            matcher->allocator,
            (sxml_allocation_request){.byte_count = matcher->state_count, .alignment = 1},
            (void **)&matcher->next);
    }
    return status;
}

/* The state of element element_index having seen count codepoints, or
   the end state after the last element. */
static size_t state_of(const wildcard_matcher *matcher, size_t element_index, uint32_t count) {
    if (element_index == matcher->element_count) {
        return matcher->state_count - 1;
    }
    return matcher->elements[element_index].first_state + count;
}

/* Adds the states reachable without input: an "any" element that has
   seen its minimum may end. Elements are visited in order, so one pass
   reaches every later element. */
typedef struct state_set {
    bool *is_reached;
} state_set;

static void close_states(const wildcard_matcher *matcher, state_set set) {
    bool *states = set.is_reached;
    for (size_t element_index = 0; element_index < matcher->element_count; element_index += 1) {
        const wildcard_element *element = &matcher->elements[element_index];
        if (!element->is_any) {
            continue;
        }
        for (size_t count = element->minimum; count < element->state_count; count += 1) {
            if (states[element->first_state + count]) {
                states[state_of(matcher, element_index + 1, 0)] = true;
                break;
            }
        }
    }
}

/* Moves the states over one codepoint into matcher->next. */
static void step_states(const wildcard_matcher *matcher, uint32_t codepoint) {
    memset(matcher->next, 0, matcher->state_count);
    for (size_t element_index = 0; element_index < matcher->element_count; element_index += 1) {
        const wildcard_element *element = &matcher->elements[element_index];
        for (size_t count = 0; count < element->state_count; count += 1) {
            if (!matcher->current[element->first_state + count]) {
                continue;
            }
            if (!element->is_any) {
                if (element->codepoint == codepoint) {
                    matcher->next[state_of(matcher, element_index + 1, 0)] = true;
                }
                continue;
            }
            if (element->maximum == UINT32_MAX) {
                /* Past the minimum the count stays at the minimum. */
                const size_t seen = count + 1 > element->minimum ? element->minimum : count + 1;
                matcher->next[element->first_state + seen] = true;
            } else if (count < element->maximum) {
                matcher->next[element->first_state + count + 1] = true;
            }
        }
    }
    close_states(matcher, (state_set){.is_reached = matcher->next});
}

sxml_status sxml_full_text_match_wildcards(const sxml_full_text_wildcard_request *request,
                                           sxml_full_text_wildcard_outcome *outcome) {
    *outcome = (sxml_full_text_wildcard_outcome){.is_match = false, .is_invalid = false};
    wildcard_matcher matcher = {.allocator = request->allocator};
    sxml_status status = compile_pattern(request->pattern, &matcher, outcome);
    if (status != SXML_STATUS_OK || outcome->is_invalid) {
        release_matcher(&matcher);
        return status;
    }
    memset(matcher.current, 0, matcher.state_count);
    matcher.current[state_of(&matcher, 0, 0)] = true;
    close_states(&matcher, (state_set){.is_reached = matcher.current});
    const sxml_byte_view word = request->word;
    bool has_state = true;
    for (size_t byte_offset = 0; has_state && byte_offset < word.byte_count;) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(word, byte_offset);
        byte_offset += decoded.encoded_byte_count;
        step_states(&matcher, decoded.codepoint);
        bool *swapped = matcher.current;
        matcher.current = matcher.next;
        matcher.next = swapped;
        has_state = memchr(matcher.current, 1, matcher.state_count) != NULL;
    }
    outcome->is_match = has_state && matcher.current[matcher.state_count - 1];
    release_matcher(&matcher);
    return SXML_STATUS_OK;
}
