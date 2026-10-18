/*
 * The picture engine for numbers (packet 2.9, D97): fn:format-integer
 * pictures (F&O 4.0, section 4.8.1), decimal formats (section 4.9.1) and
 * fn:format-number pictures (sections 4.9.3 to 4.9.5). See
 * src/format_internal.h.
 *
 * Golden file: src/memory.c (plain functions over views, checked size
 * arithmetic, every early return releasing what it took).
 *
 * Pictures are scanned by hand (D97), codepoint by codepoint over the
 * UTF-8 of the picture string; a parsed picture keeps byte views of it,
 * and formatting scans the integer and fractional parts again to place
 * grouping separators, so no picture needs memory. Numbers arrive as
 * decimal digit text and are rounded on their digits, which is exact for
 * any size (section 4.9.5 asks for "no limits on the totalDigits or
 * fractionDigits").
 */
#include "format_internal.h"

#include "regex_internal.h"

#include <string.h>

/* ------------------------------------------------------------ Codepoints */

/* One codepoint decoded from UTF-8 and the bytes it took. */
typedef struct decoded_codepoint {
    uint32_t codepoint;
    size_t byte_count;
} decoded_codepoint;

/* Decodes the codepoint at byte_offset of utf8 (below byte_count). Text is
   well-formed (src/format_internal.h); a stray byte decodes as itself, so
   malformed text cannot read past the end. */
static decoded_codepoint decode_codepoint(sxml_byte_view utf8, size_t byte_offset) {
    const uint8_t *bytes = utf8.bytes + byte_offset;
    const size_t available = utf8.byte_count - byte_offset;
    const uint32_t lead = bytes[0];
    size_t length = 1;
    uint32_t codepoint = lead;
    if (lead >= 0xF0U && available >= 4) {
        length = 4;
        codepoint = lead & 0x07U;
    } else if (lead >= 0xE0U && lead < 0xF0U && available >= 3) {
        length = 3;
        codepoint = lead & 0x0FU;
    } else if (lead >= 0xC0U && lead < 0xE0U && available >= 2) {
        length = 2;
        codepoint = lead & 0x1FU;
    }
    if (length == 1) {
        return (decoded_codepoint){.codepoint = lead, .byte_count = 1};
    }
    for (size_t continuation_index = 1; continuation_index < length; continuation_index += 1) {
        codepoint = (codepoint << 6U) | (bytes[continuation_index] & 0x3FU);
    }
    return (decoded_codepoint){.codepoint = codepoint, .byte_count = length};
}

/* The number of codepoints in utf8. */
static size_t count_codepoints(sxml_byte_view utf8) {
    size_t codepoint_count = 0;
    size_t byte_offset = 0;
    while (byte_offset < utf8.byte_count) {
        byte_offset += decode_codepoint(utf8, byte_offset).byte_count;
        codepoint_count += 1;
    }
    return codepoint_count;
}

static bool is_category(uint32_t codepoint, sxml_regex_general_category category) {
    return sxml_regex_get_general_category(codepoint) == category;
}

/* A letter or a number: categories L and N (Nd, Nl, No, Lu, Ll, Lt, Lm,
   Lo), which F&O 4.0, section 4.8.1, calls alphanumeric. */
static bool is_alphanumeric(uint32_t codepoint) {
    const uint32_t category_bit = 1U << (uint32_t)sxml_regex_get_general_category(codepoint);
    return (category_bit & (SXML_REGEX_CATEGORY_MASK_L | SXML_REGEX_CATEGORY_MASK_N)) != 0;
}

/* The longest run of Nd codepoints (U+1D7CE to U+1D7FF, five families). */
#define SXML_FORMAT_LONGEST_DIGIT_RUN 50U

/*
 * The digit value of a codepoint of category Nd. Unicode assigns decimal
 * digits in runs of ten with values 0 to 9 in codepoint order (The Unicode
 * Standard 16.0, section 4.6, Numeric Value), so the value is the distance
 * from the start of the run, modulo 10.
 */
static uint32_t digit_value(uint32_t codepoint) {
    uint32_t run_start = codepoint;
    uint32_t step_count = 0;
    while (run_start > 0 && step_count < SXML_FORMAT_LONGEST_DIGIT_RUN &&
           is_category(run_start - 1, SXML_REGEX_GENERAL_CATEGORY_ND)) {
        run_start -= 1;
        step_count += 1;
    }
    return (codepoint - run_start) % 10U;
}

/* ------------------------------------------------------------------ Text */

sxml_status sxml_format_text_append(sxml_format_text *text, sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t required = 0;
    sxml_status status = sxml_size_add(text->byte_count, bytes.byte_count, &required);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (required > text->byte_capacity) {
        size_t capacity = text->byte_capacity < 64 ? 64 : text->byte_capacity;
        while (capacity < required) {
            status = sxml_size_multiply(capacity, 2, &capacity);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        void *memory = NULL;
        status = sxml_allocator_allocate(
            text->allocator, (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (text->byte_count > 0) {
            memcpy(memory, text->bytes, text->byte_count);
        }
        if (text->bytes != NULL) {
            sxml_allocator_release(text->allocator, text->bytes);
        }
        text->bytes = memory;
        text->byte_capacity = capacity;
    }
    memcpy(text->bytes + text->byte_count, bytes.bytes, bytes.byte_count);
    text->byte_count = required;
    return SXML_STATUS_OK;
}

sxml_status sxml_format_text_append_codepoint(sxml_format_text *text, uint32_t codepoint) {
    uint8_t encoded[4];
    size_t encoded_byte_count = 0;
    if (codepoint < 0x80U) {
        encoded[0] = (uint8_t)codepoint;
        encoded_byte_count = 1;
    } else if (codepoint < 0x800U) {
        encoded[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
        encoded[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded_byte_count = 2;
    } else if (codepoint < 0x10000U) {
        encoded[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
        encoded[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        encoded[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded_byte_count = 3;
    } else {
        encoded[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
        encoded[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
        encoded[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        encoded[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded_byte_count = 4;
    }
    return sxml_format_text_append(
        text, (sxml_byte_view){.bytes = encoded, .byte_count = encoded_byte_count});
}

sxml_byte_view sxml_format_text_view(const sxml_format_text *text) {
    return (sxml_byte_view){.bytes = text->bytes, .byte_count = text->byte_count};
}

void sxml_format_text_release(sxml_format_text *text) {
    if (text->bytes != NULL) {
        sxml_allocator_release(text->allocator, text->bytes);
    }
    text->bytes = NULL;
    text->byte_count = 0;
    text->byte_capacity = 0;
}

/* Appends repeat_count copies of codepoint. */
typedef struct codepoint_run {
    uint32_t codepoint;
    size_t repeat_count;
} codepoint_run;

static sxml_status append_run(sxml_format_text *text, codepoint_run run) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t repeat_index = 0; status == SXML_STATUS_OK && repeat_index < run.repeat_count;
         repeat_index += 1) {
        status = sxml_format_text_append_codepoint(text, run.codepoint);
    }
    return status;
}

/* --------------------------------------------------------- Digit patterns */

/* What one character of a digit pattern is (F&O 4.0, section 4.8.1). */
typedef enum pattern_sign {
    SXML_PATTERN_SIGN_MANDATORY_DIGIT,
    SXML_PATTERN_SIGN_OPTIONAL_DIGIT,
    SXML_PATTERN_SIGN_GROUPING_SEPARATOR,
    SXML_PATTERN_SIGN_INVALID,
} pattern_sign;

/* How a digit pattern's characters are told apart: decimal (Nd digits of
   one family) or with a radix ("x" or "X"). */
typedef struct pattern_rules {
    bool has_radix;
} pattern_rules;

static pattern_sign classify_pattern_sign(pattern_rules rules, uint32_t codepoint) {
    if (codepoint == '#') {
        return SXML_PATTERN_SIGN_OPTIONAL_DIGIT;
    }
    if (rules.has_radix) {
        if (codepoint == 'x' || codepoint == 'X') {
            return SXML_PATTERN_SIGN_MANDATORY_DIGIT;
        }
    } else if (is_category(codepoint, SXML_REGEX_GENERAL_CATEGORY_ND)) {
        return SXML_PATTERN_SIGN_MANDATORY_DIGIT;
    }
    /* A grouping-separator-sign is a character that is not alphanumeric;
       the patterns ^((\p{Nd}|#|[^\p{N}\p{L}])+?)$ and
       ^(([Xx#]|[^\p{N}\p{L}])+?)$ allow nothing else. */
    return is_alphanumeric(codepoint) ? SXML_PATTERN_SIGN_INVALID : SXML_PATTERN_SIGN_GROUPING_SEPARATOR;
}

/* Checks a digit pattern by the rules of section 4.8.1 and finds its
   digit family. */
typedef struct pattern_check {
    uint32_t zero_digit;
    bool has_upper_case_digits;
} pattern_check;

static sxml_status check_digit_pattern(sxml_byte_view pattern, pattern_rules rules,
                                       pattern_check *check) {
    bool has_mandatory_digit = false;
    bool has_lower_case_x = false;
    bool has_upper_case_x = false;
    bool is_after_separator = true;
    uint32_t zero_digit = '0';
    size_t byte_offset = 0;
    while (byte_offset < pattern.byte_count) {
        const decoded_codepoint decoded = decode_codepoint(pattern, byte_offset);
        byte_offset += decoded.byte_count;
        switch (classify_pattern_sign(rules, decoded.codepoint)) {
        case SXML_PATTERN_SIGN_MANDATORY_DIGIT:
            if (rules.has_radix) {
                has_upper_case_x = has_upper_case_x || decoded.codepoint == 'X';
                has_lower_case_x = has_lower_case_x || decoded.codepoint == 'x';
            } else {
                /* "All mandatory-digit-signs within the format token must
                   be from the same digit family." */
                const uint32_t family_zero = decoded.codepoint - digit_value(decoded.codepoint);
                if (has_mandatory_digit && family_zero != zero_digit) {
                    return SXML_STATUS_MALFORMED_INPUT;
                }
                zero_digit = family_zero;
            }
            has_mandatory_digit = true;
            is_after_separator = false;
            break;
        case SXML_PATTERN_SIGN_OPTIONAL_DIGIT:
            /* Optional-digit-signs "must precede all
               mandatory-digit-signs". */
            if (has_mandatory_digit) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            is_after_separator = false;
            break;
        case SXML_PATTERN_SIGN_GROUPING_SEPARATOR:
            /* Not at the start, nor adjacent to another separator. */
            if (is_after_separator) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            is_after_separator = true;
            break;
        case SXML_PATTERN_SIGN_INVALID:
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    /* At least one mandatory digit; no separator at the end; "X" and "x"
       not mixed. */
    if (!has_mandatory_digit || is_after_separator || (has_upper_case_x && has_lower_case_x)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    *check = (pattern_check){.zero_digit = zero_digit, .has_upper_case_digits = has_upper_case_x};
    return SXML_STATUS_OK;
}

/* A radix prefix "R^" of a primary format token: R from 2 to 36 in ASCII
   digits without a leading zero, recognized only when an "x" or "X"
   follows somewhere in the token (section 4.8.1). */
typedef struct radix_prefix {
    bool is_present;
    uint32_t radix;
    size_t byte_count;
} radix_prefix;

static radix_prefix find_radix_prefix(sxml_byte_view token) {
    const radix_prefix absent = {.is_present = false, .radix = 10, .byte_count = 0};
    uint32_t radix = 0;
    size_t byte_offset = 0;
    while (byte_offset < token.byte_count && byte_offset < 3 && token.bytes[byte_offset] >= '0' &&
           token.bytes[byte_offset] <= '9') {
        radix = radix * 10U + (uint32_t)(token.bytes[byte_offset] - '0');
        byte_offset += 1;
    }
    if (byte_offset == 0 || byte_offset >= token.byte_count || token.bytes[0] == '0' ||
        token.bytes[byte_offset] != '^' || radix < 2 || radix > 36) {
        return absent;
    }
    for (size_t search_offset = byte_offset + 1; search_offset < token.byte_count;
         search_offset += 1) {
        if (token.bytes[search_offset] == 'x' || token.bytes[search_offset] == 'X') {
            return (radix_prefix){.is_present = true, .radix = radix, .byte_count = byte_offset + 1};
        }
    }
    return absent;
}

/* True when token holds a character of category Nd. */
static bool has_decimal_digit(sxml_byte_view token) {
    size_t byte_offset = 0;
    while (byte_offset < token.byte_count) {
        const decoded_codepoint decoded = decode_codepoint(token, byte_offset);
        byte_offset += decoded.byte_count;
        if (is_category(decoded.codepoint, SXML_REGEX_GENERAL_CATEGORY_ND)) {
            return true;
        }
    }
    return false;
}

/* --------------------------------------------- Alphabets and symbols */

/*
 * The alphabets (glossary) sxml numbers with, each named by its first
 * letter (F&O 4.0, section 4.8.1: "a" and "A" and other alphabetic
 * sequences; XSLT 4.0, 12.4). Greek counts the letters of its block in
 * order: lower case with final sigma (U+03C2, as the XSLT test suite's
 * number-0901 expects), upper case without the unassigned U+03A2.
 */
typedef struct numbering_alphabet {
    uint32_t first_letter;
    /* Runs of consecutive codepoints, in order. */
    uint32_t run_starts[2];
    uint32_t run_lengths[2];
} numbering_alphabet;

static const numbering_alphabet numbering_alphabets[] = {
    {.first_letter = 'a', .run_starts = {'a', 0}, .run_lengths = {26, 0}},
    {.first_letter = 'A', .run_starts = {'A', 0}, .run_lengths = {26, 0}},
    {.first_letter = 0x03B1U, .run_starts = {0x03B1U, 0}, .run_lengths = {25, 0}},
    {.first_letter = 0x0391U, .run_starts = {0x0391U, 0x03A3U}, .run_lengths = {17, 7}},
};

/* One range of numbers whose symbols are consecutive codepoints. */
typedef struct numbering_symbol_run {
    uint32_t first_number;
    uint32_t last_number;
    uint32_t first_codepoint;
} numbering_symbol_run;

/*
 * The symbol sequences (glossary), each named by its symbol for 1: the
 * Unicode characters whose names number them (The Unicode Standard 16.0,
 * code charts "Enclosed Alphanumerics", "Dingbats", "Enclosed CJK Letters
 * and Months", "Enclosed Alphanumeric Supplement" and the historic
 * number blocks). A sequence lists up to four runs of numbers; numbers
 * outside them are written with the token "1" (section 4.8.1).
 */
typedef struct numbering_symbols {
    uint32_t one;
    numbering_symbol_run runs[4];
    size_t run_count;
} numbering_symbols;

static const numbering_symbols numbering_symbol_sequences[] = {
    /* CIRCLED DIGIT ZERO, ONE to TWENTY, CIRCLED NUMBER TWENTY ONE to
       THIRTY FIVE, THIRTY SIX to FIFTY. */
    {.one = 0x2460U,
     .runs = {{0, 0, 0x24EAU}, {1, 20, 0x2460U}, {21, 35, 0x3251U}, {36, 50, 0x32B1U}},
     .run_count = 4},
    /* PARENTHESIZED DIGIT ONE to PARENTHESIZED NUMBER TWENTY. */
    {.one = 0x2474U, .runs = {{1, 20, 0x2474U}}, .run_count = 1},
    /* DIGIT ZERO FULL STOP, DIGIT ONE FULL STOP to NUMBER TWENTY FULL STOP. */
    {.one = 0x2488U, .runs = {{0, 0, 0x1F100U}, {1, 20, 0x2488U}}, .run_count = 2},
    /* DIGIT ZERO COMMA to DIGIT NINE COMMA. */
    {.one = 0x1F102U, .runs = {{0, 9, 0x1F101U}}, .run_count = 1},
    /* NEGATIVE CIRCLED DIGIT ZERO, DINGBAT NEGATIVE CIRCLED DIGIT ONE to
       NUMBER TEN, NEGATIVE CIRCLED NUMBER ELEVEN to TWENTY. */
    {.one = 0x2776U,
     .runs = {{0, 0, 0x24FFU}, {1, 10, 0x2776U}, {11, 20, 0x24EBU}},
     .run_count = 3},
    /* DOUBLE CIRCLED DIGIT ONE to DOUBLE CIRCLED NUMBER TEN. */
    {.one = 0x24F5U, .runs = {{1, 10, 0x24F5U}}, .run_count = 1},
    /* DINGBAT CIRCLED SANS-SERIF DIGIT ZERO, ONE to NUMBER TEN. */
    {.one = 0x2780U, .runs = {{0, 0, 0x1F10BU}, {1, 10, 0x2780U}}, .run_count = 2},
    /* DINGBAT NEGATIVE CIRCLED SANS-SERIF DIGIT ZERO, ONE to NUMBER TEN. */
    {.one = 0x278AU, .runs = {{0, 0, 0x1F10CU}, {1, 10, 0x278AU}}, .run_count = 2},
    /* PARENTHESIZED IDEOGRAPH ONE to TEN. */
    {.one = 0x3220U, .runs = {{1, 10, 0x3220U}}, .run_count = 1},
    /* CIRCLED IDEOGRAPH ONE to TEN. */
    {.one = 0x3280U, .runs = {{1, 10, 0x3280U}}, .run_count = 1},
    /* AEGEAN NUMBER ONE to TEN. */
    {.one = 0x10107U, .runs = {{1, 10, 0x10107U}}, .run_count = 1},
    /* COPTIC EPACT DIGIT ONE to NUMBER TEN. */
    {.one = 0x102E1U, .runs = {{1, 10, 0x102E1U}}, .run_count = 1},
    /* RUMI DIGIT ONE to NUMBER TEN. */
    {.one = 0x10E60U, .runs = {{1, 10, 0x10E60U}}, .run_count = 1},
    /* BRAHMI NUMBER ONE to TEN. */
    {.one = 0x11052U, .runs = {{1, 10, 0x11052U}}, .run_count = 1},
    /* SINHALA ARCHAIC DIGIT ONE to NUMBER TEN. */
    {.one = 0x111E1U, .runs = {{1, 10, 0x111E1U}}, .run_count = 1},
    /* COUNTING ROD UNIT DIGIT ONE to NINE. */
    {.one = 0x1D360U, .runs = {{1, 9, 0x1D360U}}, .run_count = 1},
    /* MENDE KIKAKUI DIGIT ONE to NINE. */
    {.one = 0x1E8C7U, .runs = {{1, 9, 0x1E8C7U}}, .run_count = 1},
};

/* When token is one codepoint naming an alphabet or a symbol sequence,
   sets picture's kind and sequence_index and returns true. */
static bool find_numbering_sequence(sxml_byte_view token, sxml_integer_picture *picture) {
    const decoded_codepoint decoded = decode_codepoint(token, 0);
    if (decoded.byte_count != token.byte_count) {
        return false;
    }
    for (size_t alphabet_index = 0;
         alphabet_index < sizeof(numbering_alphabets) / sizeof(numbering_alphabets[0]);
         alphabet_index += 1) {
        if (numbering_alphabets[alphabet_index].first_letter == decoded.codepoint) {
            picture->numbering_kind = SXML_NUMBERING_KIND_ALPHABETIC;
            picture->sequence_index = (uint32_t)alphabet_index;
            return true;
        }
    }
    for (size_t sequence_index = 0;
         sequence_index < sizeof(numbering_symbol_sequences) / sizeof(numbering_symbol_sequences[0]);
         sequence_index += 1) {
        if (numbering_symbol_sequences[sequence_index].one == decoded.codepoint) {
            picture->numbering_kind = SXML_NUMBERING_KIND_SYMBOLS;
            picture->sequence_index = (uint32_t)sequence_index;
            return true;
        }
    }
    return false;
}

sxml_status sxml_integer_picture_parse_primary_format_token(sxml_byte_view primary_format_token,
                                                            sxml_integer_picture *picture) {
    if (primary_format_token.byte_count == 0) {
        /* "A primary format token ... must not be zero-length." */
        return SXML_STATUS_MALFORMED_INPUT;
    }
    sxml_integer_picture result = {.numbering_kind = SXML_NUMBERING_KIND_UNSUPPORTED,
                                   .sequence_index = 0,
                                   .digit_pattern = {.bytes = NULL, .byte_count = 0},
                                   .radix = 10,
                                   .zero_digit = '0',
                                   .has_upper_case_digits = false,
                                   .is_ordinal = false,
                                   .numbering_variant = {.bytes = NULL, .byte_count = 0}};
    const radix_prefix prefix = find_radix_prefix(primary_format_token);
    if (prefix.is_present || has_decimal_digit(primary_format_token)) {
        const sxml_byte_view pattern = {
            .bytes = primary_format_token.bytes + prefix.byte_count,
            .byte_count = primary_format_token.byte_count - prefix.byte_count};
        pattern_check check;
        const sxml_status status = check_digit_pattern(
            pattern, (pattern_rules){.has_radix = prefix.is_present}, &check);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        result.numbering_kind = SXML_NUMBERING_KIND_DIGITS;
        result.digit_pattern = pattern;
        result.radix = prefix.radix;
        result.zero_digit = check.zero_digit;
        result.has_upper_case_digits = check.has_upper_case_digits;
    } else if (find_numbering_sequence(primary_format_token, &result)) {
        /* An alphabet or a symbol sequence named by its first token. */
    } else if (sxml_byte_view_equals(primary_format_token, SXML_BYTE_VIEW_FROM_LITERAL("i"))) {
        result.numbering_kind = SXML_NUMBERING_KIND_LOWER_ROMAN;
    } else if (sxml_byte_view_equals(primary_format_token, SXML_BYTE_VIEW_FROM_LITERAL("I"))) {
        result.numbering_kind = SXML_NUMBERING_KIND_UPPER_ROMAN;
    } else if (sxml_byte_view_equals(primary_format_token, SXML_BYTE_VIEW_FROM_LITERAL("w"))) {
        result.numbering_kind = SXML_NUMBERING_KIND_LOWER_WORDS;
    } else if (sxml_byte_view_equals(primary_format_token, SXML_BYTE_VIEW_FROM_LITERAL("W"))) {
        result.numbering_kind = SXML_NUMBERING_KIND_UPPER_WORDS;
    } else if (sxml_byte_view_equals(primary_format_token, SXML_BYTE_VIEW_FROM_LITERAL("Ww"))) {
        result.numbering_kind = SXML_NUMBERING_KIND_TITLE_WORDS;
    }
    *picture = result;
    return SXML_STATUS_OK;
}

/* Parses a format modifier, ^([co](\(.+\))?)?[at]?$ (section 4.8.1), into
   picture's ordinal fields. */
static sxml_status parse_format_modifier(sxml_byte_view modifier, sxml_integer_picture *picture) {
    size_t byte_offset = 0;
    size_t end_offset = modifier.byte_count;
    bool is_ordinal = false;
    sxml_byte_view numbering_variant = {.bytes = NULL, .byte_count = 0};
    if (byte_offset < end_offset &&
        (modifier.bytes[byte_offset] == 'c' || modifier.bytes[byte_offset] == 'o')) {
        is_ordinal = modifier.bytes[byte_offset] == 'o';
        byte_offset += 1;
        if (byte_offset < end_offset && modifier.bytes[byte_offset] == '(') {
            /* The variant runs to a ")" that is last, or last but an "a"
               or "t". */
            const uint8_t last = modifier.bytes[end_offset - 1];
            size_t close_offset = end_offset - 1;
            if (last == 'a' || last == 't') {
                close_offset = end_offset - 2;
            }
            if (close_offset <= byte_offset + 1 || modifier.bytes[close_offset] != ')') {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            numbering_variant = (sxml_byte_view){.bytes = modifier.bytes + byte_offset + 1,
                                                 .byte_count = close_offset - byte_offset - 1};
            byte_offset = close_offset + 1;
        }
    }
    if (byte_offset < end_offset &&
        (modifier.bytes[byte_offset] == 'a' || modifier.bytes[byte_offset] == 't')) {
        byte_offset += 1;
    }
    if (byte_offset != end_offset) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    picture->is_ordinal = is_ordinal;
    picture->numbering_variant = numbering_variant;
    return SXML_STATUS_OK;
}

sxml_status sxml_integer_picture_parse(sxml_byte_view picture_string,
                                       sxml_integer_picture *picture) {
    /* "If the string contains one or more semicolons then the last
       semicolon is taken as terminating the primary format token." */
    size_t token_byte_count = picture_string.byte_count;
    sxml_byte_view modifier = {.bytes = NULL, .byte_count = 0};
    for (size_t byte_offset = picture_string.byte_count; byte_offset > 0; byte_offset -= 1) {
        if (picture_string.bytes[byte_offset - 1] == ';') {
            token_byte_count = byte_offset - 1;
            modifier = (sxml_byte_view){.bytes = picture_string.bytes + byte_offset,
                                        .byte_count = picture_string.byte_count - byte_offset};
            break;
        }
    }
    sxml_integer_picture result;
    sxml_status status = sxml_integer_picture_parse_primary_format_token(
        (sxml_byte_view){.bytes = picture_string.bytes, .byte_count = token_byte_count}, &result);
    if (status == SXML_STATUS_OK) {
        status = parse_format_modifier(modifier, &result);
    }
    if (status == SXML_STATUS_OK) {
        *picture = result;
    }
    return status;
}

/* ------------------------------------------------------- Integer numbers */

/* The digits of an integer, most significant first, as values below the
   radix; owned through the allocator. */
typedef struct radix_digits {
    sxml_allocator allocator;
    uint8_t *values;
    size_t digit_count;
} radix_digits;

static void radix_digits_release(radix_digits *digits) {
    if (digits->values != NULL) {
        sxml_allocator_release(digits->allocator, digits->values);
    }
    digits->values = NULL;
    digits->digit_count = 0;
}

/* The decimal digits to convert and the radix. */
typedef struct radix_conversion {
    sxml_byte_view decimal_digits;
    uint32_t radix;
} radix_conversion;

/*
 * Converts decimal digits (no leading zeros; empty is 0) to radix digits:
 * schoolbook division of the decimal digits by the radix, one remainder
 * per pass. A number of n decimal digits has at most 4n radix digits
 * (radix 2: log2(10) < 4).
 */
static sxml_status convert_to_radix(radix_conversion conversion, radix_digits *digits) {
    const sxml_byte_view decimal_digits = conversion.decimal_digits;
    const size_t decimal_count = decimal_digits.byte_count;
    size_t capacity = 0;
    sxml_status status = sxml_size_multiply(decimal_count + 1, 5, &capacity);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            digits->allocator, (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The dividend's digit values, then the remainders from the end. */
    uint8_t *dividend = memory;
    uint8_t *remainders = dividend + decimal_count;
    const size_t remainder_capacity = capacity - decimal_count;
    for (size_t digit_index = 0; digit_index < decimal_count; digit_index += 1) {
        dividend[digit_index] = (uint8_t)(decimal_digits.bytes[digit_index] - '0');
    }
    size_t first_nonzero = 0;
    size_t remainder_count = 0;
    while (first_nonzero < decimal_count && remainder_count < remainder_capacity) {
        uint32_t carry = 0;
        for (size_t digit_index = first_nonzero; digit_index < decimal_count; digit_index += 1) {
            const uint32_t current = carry * 10U + dividend[digit_index];
            dividend[digit_index] = (uint8_t)(current / conversion.radix);
            carry = current % conversion.radix;
        }
        remainders[remainder_capacity - 1 - remainder_count] = (uint8_t)carry;
        remainder_count += 1;
        while (first_nonzero < decimal_count && dividend[first_nonzero] == 0) {
            first_nonzero += 1;
        }
    }
    if (remainder_count == 0) {
        remainders[remainder_capacity - 1] = 0;
        remainder_count = 1;
    }
    memmove(memory, remainders + remainder_capacity - remainder_count, remainder_count);
    digits->values = memory;
    digits->digit_count = remainder_count;
    return SXML_STATUS_OK;
}

/* Where a grouping separator goes: its codepoint at a position counted
   from the right end of the digits. */
typedef struct grouping_query {
    sxml_byte_view digit_pattern;
    pattern_rules rules;
    size_t position;
} grouping_query;

/* The grouping separator of the digit pattern at query.position, or 0. */
static uint32_t find_pattern_separator(grouping_query query) {
    size_t digit_sign_count = 0;
    size_t byte_offset = query.digit_pattern.byte_count;
    while (byte_offset > 0) {
        size_t start_offset = byte_offset - 1;
        while (start_offset > 0 && (query.digit_pattern.bytes[start_offset] & 0xC0U) == 0x80U) {
            start_offset -= 1;
        }
        const uint32_t codepoint = decode_codepoint(query.digit_pattern, start_offset).codepoint;
        byte_offset = start_offset;
        if (classify_pattern_sign(query.rules, codepoint) == SXML_PATTERN_SIGN_GROUPING_SEPARATOR) {
            if (digit_sign_count == query.position) {
                return codepoint;
            }
        } else {
            digit_sign_count += 1;
        }
    }
    return 0;
}

/* The grouping separator template of a digit pattern (section 4.8.1). */
typedef struct grouping_template {
    bool is_regular;
    size_t grouping_size;
    uint32_t separator;
    size_t digit_sign_count;
} grouping_template;

static grouping_template read_grouping_template(sxml_byte_view digit_pattern, pattern_rules rules) {
    size_t digit_sign_count = 0;
    size_t separator_count = 0;
    size_t smallest_position = 0;
    uint32_t separator = 0;
    bool is_one_character = true;
    size_t byte_offset = digit_pattern.byte_count;
    while (byte_offset > 0) {
        size_t start_offset = byte_offset - 1;
        while (start_offset > 0 && (digit_pattern.bytes[start_offset] & 0xC0U) == 0x80U) {
            start_offset -= 1;
        }
        const uint32_t codepoint = decode_codepoint(digit_pattern, start_offset).codepoint;
        byte_offset = start_offset;
        if (classify_pattern_sign(rules, codepoint) == SXML_PATTERN_SIGN_GROUPING_SEPARATOR) {
            if (separator_count == 0) {
                smallest_position = digit_sign_count;
                separator = codepoint;
            }
            is_one_character = is_one_character && codepoint == separator;
            separator_count += 1;
        } else {
            digit_sign_count += 1;
        }
    }
    grouping_template result = {.is_regular = false,
                                .grouping_size = smallest_position,
                                .separator = separator,
                                .digit_sign_count = digit_sign_count};
    if (separator_count == 0 || !is_one_character || smallest_position == 0) {
        return result;
    }
    /* Regular: every position a multiple of G, and every multiple of G
       below the number of digit signs a position. */
    size_t multiple_count = 0;
    for (size_t position = smallest_position; position < digit_sign_count;
         position += smallest_position) {
        if (find_pattern_separator((grouping_query){
                .digit_pattern = digit_pattern, .rules = rules, .position = position}) == 0) {
            return result;
        }
        multiple_count += 1;
    }
    result.is_regular = multiple_count == separator_count;
    return result;
}

static const char lower_case_radix_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
static const char upper_case_radix_digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

/* The decimal digits of a number, without leading zeros, and its picture;
   grouping_size above 0 replaces the pattern's grouping by
   grouping_separator every grouping_size digits (xsl:number). */
typedef struct digit_request {
    const sxml_integer_picture *picture;
    sxml_byte_view decimal_digits;
    sxml_byte_view grouping_separator;
    size_t grouping_size;
} digit_request;

/* Writes the number by a digit pattern: S1 to S4 of section 4.8.1. */
static sxml_status append_by_digit_pattern(digit_request request, sxml_format_text *text) {
    const sxml_integer_picture *picture = request.picture;
    radix_digits digits = {.allocator = text->allocator, .values = NULL, .digit_count = 0};
    sxml_status status = convert_to_radix(
        (radix_conversion){.decimal_digits = request.decimal_digits, .radix = picture->radix},
        &digits);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* A pattern after "R^" has "x" signs and no Nd digit, even for
       "10^"; a decimal pattern always has an Nd digit. */
    const pattern_rules pattern_kind = {.has_radix = !has_decimal_digit(picture->digit_pattern)};
    size_t mandatory_count = 0;
    size_t byte_offset = 0;
    while (byte_offset < picture->digit_pattern.byte_count) {
        const decoded_codepoint decoded = decode_codepoint(picture->digit_pattern, byte_offset);
        byte_offset += decoded.byte_count;
        if (classify_pattern_sign(pattern_kind, decoded.codepoint) ==
            SXML_PATTERN_SIGN_MANDATORY_DIGIT) {
            mandatory_count += 1;
        }
    }
    const grouping_template grouping = read_grouping_template(picture->digit_pattern, pattern_kind);
    const size_t padding_count =
        mandatory_count > digits.digit_count ? mandatory_count - digits.digit_count : 0;
    const size_t total_count = padding_count + digits.digit_count;
    const char *radix_alphabet =
        picture->has_upper_case_digits ? upper_case_radix_digits : lower_case_radix_digits;
    for (size_t digit_index = 0; status == SXML_STATUS_OK && digit_index < total_count;
         digit_index += 1) {
        const size_t position = total_count - digit_index;
        if (digit_index > 0 && request.grouping_size > 0) {
            /* XSLT 4.0, 12.4: the separator between groups of
               grouping-size digits. */
            if (position % request.grouping_size == 0) {
                status = sxml_format_text_append(text, request.grouping_separator);
            }
        } else if (digit_index > 0 && grouping.digit_sign_count > 0) {
            uint32_t separator = 0;
            if (grouping.is_regular) {
                separator = position % grouping.grouping_size == 0 ? grouping.separator : 0;
            } else if (position < grouping.digit_sign_count) {
                separator = find_pattern_separator((grouping_query){
                    .digit_pattern = picture->digit_pattern,
                    .rules = pattern_kind,
                    .position = position});
            }
            if (separator != 0) {
                status = sxml_format_text_append_codepoint(text, separator);
            }
        }
        const uint32_t value =
            digit_index < padding_count ? 0U : digits.values[digit_index - padding_count];
        if (status == SXML_STATUS_OK) {
            status = sxml_format_text_append_codepoint(
                text, pattern_kind.has_radix ? (uint32_t)(uint8_t)radix_alphabet[value]
                                             : picture->zero_digit + value);
        }
    }
    radix_digits_release(&digits);
    return status;
}

/* The value of decimal digits when it fits in uint64_t. */
typedef struct small_value {
    bool fits;
    uint64_t value;
} small_value;

static small_value read_small_value(sxml_byte_view decimal_digits) {
    uint64_t value = 0;
    for (size_t digit_index = 0; digit_index < decimal_digits.byte_count; digit_index += 1) {
        const uint64_t digit = (uint64_t)(decimal_digits.bytes[digit_index] - '0');
        if (value > (UINT64_MAX - digit) / 10U) {
            return (small_value){.fits = false, .value = 0};
        }
        value = value * 10U + digit;
    }
    return (small_value){.fits = true, .value = value};
}

/* Letters of an alphabet, a b ... z aa ab ...: the bijective numeration
   in base letter_count (glossary: alphabet). */
typedef struct alphabetic_request {
    uint64_t value;
    const numbering_alphabet *alphabet;
} alphabetic_request;

/* The letter of alphabet with index letter_index (from 0). */
static uint32_t alphabet_letter(const numbering_alphabet *alphabet, uint32_t letter_index) {
    return letter_index < alphabet->run_lengths[0]
               ? alphabet->run_starts[0] + letter_index
               : alphabet->run_starts[1] + (letter_index - alphabet->run_lengths[0]);
}

/* The most letters of a uint64_t value: 14 in base 24 or more. */
#define SXML_FORMAT_LONGEST_LETTERS 16U

static sxml_status append_alphabetic(alphabetic_request request, sxml_format_text *text) {
    const uint32_t letter_count =
        request.alphabet->run_lengths[0] + request.alphabet->run_lengths[1];
    uint32_t letter_indexes[SXML_FORMAT_LONGEST_LETTERS];
    size_t index_count = 0;
    uint64_t remaining = request.value;
    while (remaining > 0 && index_count < SXML_FORMAT_LONGEST_LETTERS) {
        remaining -= 1;
        letter_indexes[index_count] = (uint32_t)(remaining % letter_count);
        index_count += 1;
        remaining /= letter_count;
    }
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && index_count > 0) {
        index_count -= 1;
        status = sxml_format_text_append_codepoint(
            text, alphabet_letter(request.alphabet, letter_indexes[index_count]));
    }
    return status;
}

/* The symbol for value in sequence, or 0 outside its runs. */
static uint32_t find_symbol(const numbering_symbols *sequence, small_value value) {
    for (size_t run_index = 0; value.fits && run_index < sequence->run_count; run_index += 1) {
        const numbering_symbol_run *run = &sequence->runs[run_index];
        if (value.value >= run->first_number && value.value <= run->last_number) {
            return run->first_codepoint + (uint32_t)(value.value - run->first_number);
        }
    }
    return 0;
}

/* Roman numerals, 1 to 3999. */
typedef struct roman_request {
    uint64_t value;
    bool is_upper_case;
} roman_request;

static sxml_status append_roman(roman_request request, sxml_format_text *text) {
    static const uint32_t values[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char *const lower_case_symbols[] = {"m",  "cm", "d",  "cd", "c",  "xc", "l",
                                                     "xl", "x",  "ix", "v",  "iv", "i"};
    static const char *const upper_case_symbols[] = {"M",  "CM", "D",  "CD", "C",  "XC", "L",
                                                     "XL", "X",  "IX", "V",  "IV", "I"};
    uint64_t remaining = request.value;
    sxml_status status = SXML_STATUS_OK;
    for (size_t symbol_index = 0;
         status == SXML_STATUS_OK && symbol_index < sizeof(values) / sizeof(values[0]);
         symbol_index += 1) {
        while (status == SXML_STATUS_OK && remaining >= values[symbol_index]) {
            status = sxml_format_text_append(
                text, sxml_byte_view_from_c_string(request.is_upper_case
                                                       ? upper_case_symbols[symbol_index]
                                                       : lower_case_symbols[symbol_index]));
            remaining -= values[symbol_index];
        }
    }
    return status;
}

/* ---------------------------------------------------------- English words */

/* Words for numbers below 10^21: seven groups of three digits. */
#define SXML_FORMAT_LONGEST_WORDS_DIGITS 21U

static const char *const unit_words[] = {
    "zero",    "one",     "two",       "three",    "four",     "five",    "six",
    "seven",   "eight",   "nine",      "ten",      "eleven",   "twelve",  "thirteen",
    "fourteen", "fifteen", "sixteen",  "seventeen", "eighteen", "nineteen"};
static const char *const ten_words[] = {"",      "",      "twenty",  "thirty", "forty",
                                        "fifty", "sixty", "seventy", "eighty", "ninety"};
static const char *const scale_words[] = {"",         "thousand",    "million",    "billion",
                                          "trillion", "quadrillion", "quintillion"};

/* Words being built in a fixed buffer: the longest number of 21 digits
   takes under 300 bytes. */
typedef struct word_buffer {
    char characters[512];
    size_t character_count;
} word_buffer;

static void add_words(word_buffer *buffer, sxml_byte_view words) {
    const size_t word_length = words.byte_count;
    if (buffer->character_count + word_length < sizeof(buffer->characters)) {
        memcpy(buffer->characters + buffer->character_count, words.bytes, word_length);
        buffer->character_count += word_length;
    }
}

/* Writes 1 to 999 (British usage: "one hundred and twenty-three", the
   example of section 4.8.1). */
static void add_group_words(word_buffer *buffer, uint32_t group_value) {
    const uint32_t hundreds = group_value / 100U;
    const uint32_t rest = group_value % 100U;
    if (hundreds > 0) {
        add_words(buffer, sxml_byte_view_from_c_string(unit_words[hundreds]));
        add_words(buffer, sxml_byte_view_from_c_string(" hundred"));
        if (rest > 0) {
            add_words(buffer, sxml_byte_view_from_c_string(" and "));
        }
    }
    if (rest == 0) {
        return;
    }
    if (rest < 20U) {
        add_words(buffer, sxml_byte_view_from_c_string(unit_words[rest]));
        return;
    }
    add_words(buffer, sxml_byte_view_from_c_string(ten_words[rest / 10U]));
    if (rest % 10U != 0) {
        add_words(buffer, sxml_byte_view_from_c_string("-"));
        add_words(buffer, sxml_byte_view_from_c_string(unit_words[rest % 10U]));
    }
}

/* Writes the cardinal of decimal digits (no leading zeros, at most 21). */
static void add_cardinal_words(word_buffer *buffer, sxml_byte_view decimal_digits) {
    if (decimal_digits.byte_count == 0) {
        add_words(buffer, sxml_byte_view_from_c_string("zero"));
        return;
    }
    const size_t group_count = (decimal_digits.byte_count + 2U) / 3U;
    size_t digit_index = 0;
    for (size_t group_index = 0; group_index < group_count; group_index += 1) {
        const size_t scale_index = group_count - 1 - group_index;
        const size_t digits_in_group =
            group_index == 0 ? decimal_digits.byte_count - 3U * (group_count - 1) : 3U;
        uint32_t group_value = 0;
        for (size_t group_digit = 0; group_digit < digits_in_group; group_digit += 1) {
            group_value =
                group_value * 10U + (uint32_t)(decimal_digits.bytes[digit_index] - '0');
            digit_index += 1;
        }
        if (group_value == 0) {
            continue;
        }
        if (buffer->character_count > 0) {
            add_words(buffer, sxml_byte_view_from_c_string(scale_index == 0 && group_value < 100U ? " and " : " "));
        }
        add_group_words(buffer, group_value);
        if (scale_index > 0) {
            add_words(buffer, sxml_byte_view_from_c_string(" "));
            add_words(buffer, sxml_byte_view_from_c_string(scale_words[scale_index]));
        }
    }
}

/* Turns the last word of a cardinal into its ordinal: one to first,
   twenty to twentieth, hundred to hundredth. */
static void make_ordinal_words(word_buffer *buffer) {
    static const char *const irregular_cardinals[] = {"one",   "two",   "three", "five",
                                                      "eight", "nine",  "twelve"};
    static const char *const irregular_ordinals[] = {"first",  "second", "third", "fifth",
                                                     "eighth", "ninth",  "twelfth"};
    size_t word_start = buffer->character_count;
    while (word_start > 0 && buffer->characters[word_start - 1] != ' ' &&
           buffer->characters[word_start - 1] != '-') {
        word_start -= 1;
    }
    const size_t word_length = buffer->character_count - word_start;
    for (size_t word_index = 0;
         word_index < sizeof(irregular_cardinals) / sizeof(irregular_cardinals[0]);
         word_index += 1) {
        if (strlen(irregular_cardinals[word_index]) == word_length &&
            memcmp(buffer->characters + word_start, irregular_cardinals[word_index],
                   word_length) == 0) {
            buffer->character_count = word_start;
            add_words(buffer, sxml_byte_view_from_c_string(irregular_ordinals[word_index]));
            return;
        }
    }
    if (word_length > 0 && buffer->characters[buffer->character_count - 1] == 'y') {
        buffer->character_count -= 1;
        add_words(buffer, sxml_byte_view_from_c_string("ieth"));
        return;
    }
    add_words(buffer, sxml_byte_view_from_c_string("th"));
}

/* The case of words: "w", "W" or "Ww". */
static void apply_word_case(word_buffer *buffer, sxml_numbering_kind numbering_kind) {
    bool is_word_start = true;
    for (size_t character_index = 0; character_index < buffer->character_count;
         character_index += 1) {
        const char character = buffer->characters[character_index];
        const bool is_lower_letter = character >= 'a' && character <= 'z';
        const bool is_and = is_word_start && character_index + 3 <= buffer->character_count &&
                            memcmp(buffer->characters + character_index, "and", 3) == 0 &&
                            (character_index + 3 == buffer->character_count ||
                             buffer->characters[character_index + 3] == ' ');
        if (is_lower_letter &&
            (numbering_kind == SXML_NUMBERING_KIND_UPPER_WORDS ||
             (numbering_kind == SXML_NUMBERING_KIND_TITLE_WORDS && is_word_start && !is_and))) {
            buffer->characters[character_index] = (char)(character - 'a' + 'A');
        }
        is_word_start = character == ' ';
    }
}

/* The English ordinal suffix of decimal digits: st, nd, rd or th. */
static const char *ordinal_suffix(sxml_byte_view decimal_digits) {
    const size_t digit_count = decimal_digits.byte_count;
    const uint32_t last = digit_count > 0 ? (uint32_t)(decimal_digits.bytes[digit_count - 1] - '0')
                                          : 0U;
    const uint32_t tens =
        digit_count > 1 ? (uint32_t)(decimal_digits.bytes[digit_count - 2] - '0') : 0U;
    if (tens == 1) {
        return "th";
    }
    return last == 1 ? "st" : last == 2 ? "nd" : last == 3 ? "rd" : "th";
}

/* The picture "1": what an unsupported token or a number out of a
   sequence's range is formatted with. */
static const sxml_integer_picture fallback_picture = {
    .numbering_kind = SXML_NUMBERING_KIND_DIGITS,
    .sequence_index = 0,
    .digit_pattern = SXML_BYTE_VIEW_INITIALIZER("1"),
    .radix = 10,
    .zero_digit = '0',
    .has_upper_case_digits = false,
    .is_ordinal = false,
    .numbering_variant = {.bytes = NULL, .byte_count = 0}};

/* Highest value of the Roman numerals sxml writes. */
#define SXML_FORMAT_LARGEST_ROMAN 3999U

sxml_status sxml_format_integer(const sxml_integer_formatting *formatting,
                                sxml_format_text *text) {
    const sxml_integer_picture *picture = formatting->picture;
    sxml_byte_view digits = formatting->magnitude_digits;
    for (size_t digit_index = 0; digit_index < digits.byte_count; digit_index += 1) {
        if (digits.bytes[digit_index] < '0' || digits.bytes[digit_index] > '9') {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    while (digits.byte_count > 0 && digits.bytes[0] == '0') {
        digits.bytes += 1;
        digits.byte_count -= 1;
    }
    /* Negative numbers: the rules are applied to the absolute value and a
       minus sign is prepended (section 4.8.1). */
    sxml_status status =
        formatting->is_negative
            ? sxml_format_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("-"))
            : SXML_STATUS_OK;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const small_value small = read_small_value(digits);
    bool is_digits = false;
    switch (picture->numbering_kind) {
    case SXML_NUMBERING_KIND_ALPHABETIC:
        if (small.fits && small.value > 0) {
            status = append_alphabetic(
                (alphabetic_request){.value = small.value,
                                     .alphabet = &numbering_alphabets[picture->sequence_index]},
                text);
        } else {
            is_digits = true;
        }
        break;
    case SXML_NUMBERING_KIND_SYMBOLS: {
        const uint32_t symbol =
            find_symbol(&numbering_symbol_sequences[picture->sequence_index], small);
        if (symbol != 0) {
            status = sxml_format_text_append_codepoint(text, symbol);
        } else {
            is_digits = true;
        }
        break;
    }
    case SXML_NUMBERING_KIND_LOWER_ROMAN:
    case SXML_NUMBERING_KIND_UPPER_ROMAN:
        if (small.fits && small.value > 0 && small.value <= SXML_FORMAT_LARGEST_ROMAN) {
            status = append_roman(
                (roman_request){.value = small.value,
                                .is_upper_case =
                                    picture->numbering_kind == SXML_NUMBERING_KIND_UPPER_ROMAN},
                text);
        } else {
            is_digits = true;
        }
        break;
    case SXML_NUMBERING_KIND_LOWER_WORDS:
    case SXML_NUMBERING_KIND_UPPER_WORDS:
    case SXML_NUMBERING_KIND_TITLE_WORDS:
        if (digits.byte_count <= SXML_FORMAT_LONGEST_WORDS_DIGITS) {
            word_buffer buffer = {.character_count = 0};
            add_cardinal_words(&buffer, digits);
            if (picture->is_ordinal) {
                make_ordinal_words(&buffer);
            }
            apply_word_case(&buffer, picture->numbering_kind);
            status = sxml_format_text_append(
                text, (sxml_byte_view){.bytes = (const uint8_t *)buffer.characters,
                                       .byte_count = buffer.character_count});
        } else {
            is_digits = true;
        }
        break;
    case SXML_NUMBERING_KIND_DIGITS:
    case SXML_NUMBERING_KIND_UNSUPPORTED:
        is_digits = true;
        break;
    }
    if (status != SXML_STATUS_OK || !is_digits) {
        return status;
    }
    const sxml_integer_picture *digit_picture =
        picture->numbering_kind == SXML_NUMBERING_KIND_DIGITS ? picture : &fallback_picture;
    status = append_by_digit_pattern(
        (digit_request){.picture = digit_picture,
                        .decimal_digits = digits,
                        .grouping_separator = formatting->grouping_separator,
                        .grouping_size = formatting->grouping_size},
        text);
    /* S5: the ordinal form, by the English suffix of the decimal value
       (QT4 format-integer-40-017: 21 in hexadecimal is "15st"). */
    if (status == SXML_STATUS_OK && picture->is_ordinal) {
        status = sxml_format_text_append(text, sxml_byte_view_from_c_string(ordinal_suffix(digits)));
    }
    return status;
}

/* ------------------------------------------- xsl:number format strings */

/* One run of a format attribute: alphanumeric or not (XSLT 4.0, 12.4). */
typedef struct format_run {
    sxml_byte_view text;
    bool is_alphanumeric;
} format_run;

/* The run that starts at byte_offset (below the format's byte count). */
static format_run read_format_run(sxml_byte_view format, size_t byte_offset) {
    const bool is_token = is_alphanumeric(decode_codepoint(format, byte_offset).codepoint);
    size_t end = byte_offset;
    while (end < format.byte_count) {
        const decoded_codepoint decoded = decode_codepoint(format, end);
        if (is_alphanumeric(decoded.codepoint) != is_token) {
            break;
        }
        end += decoded.byte_count;
    }
    return (format_run){
        .text = {.bytes = format.bytes + byte_offset, .byte_count = end - byte_offset},
        .is_alphanumeric = is_token};
}

/* The format tokens of a format, and the separator before each (empty
   before the first). */
typedef struct format_token_scan {
    sxml_byte_view format;
    size_t byte_offset;
} format_token_scan;

typedef struct format_token {
    bool is_found;
    sxml_byte_view separator;
    sxml_byte_view token;
} format_token;

/* The next format token after scan->byte_offset, moving it past. */
static format_token next_format_token(format_token_scan *scan) {
    format_token found = {.is_found = false,
                          .separator = {.bytes = NULL, .byte_count = 0},
                          .token = {.bytes = NULL, .byte_count = 0}};
    while (scan->byte_offset < scan->format.byte_count) {
        const format_run run = read_format_run(scan->format, scan->byte_offset);
        scan->byte_offset += run.text.byte_count;
        if (run.is_alphanumeric) {
            found.is_found = true;
            found.token = run.text;
            return found;
        }
        found.separator = run.text;
    }
    return found;
}

sxml_number_format_part sxml_number_format_find_part(sxml_number_format_query query) {
    /* 12.4: "If there are no format tokens, then a format token of 1 is
       used"; numbers after the first formatted by the first token are
       separated by "." (dot). */
    sxml_number_format_part part = {.separator = SXML_BYTE_VIEW_FROM_LITERAL("."),
                                    .format_token = SXML_BYTE_VIEW_FROM_LITERAL("1")};
    format_token_scan scan = {.format = query.format, .byte_offset = 0};
    for (size_t token_index = 0; token_index <= query.number_index; token_index += 1) {
        const format_token token = next_format_token(&scan);
        if (!token.is_found) {
            break;
        }
        part.format_token = token.token;
        part.separator = token_index == 0 ? SXML_BYTE_VIEW_FROM_LITERAL(".") : token.separator;
    }
    return part;
}

sxml_number_format_affixes sxml_number_format_find_affixes(sxml_byte_view format) {
    sxml_number_format_affixes affixes = {.prefix = {.bytes = NULL, .byte_count = 0},
                                          .suffix = {.bytes = NULL, .byte_count = 0}};
    if (format.byte_count == 0) {
        return affixes;
    }
    const format_run first = read_format_run(format, 0);
    if (!first.is_alphanumeric && first.text.byte_count == format.byte_count) {
        /* A non-alphanumeric token but no format token: it is both the
           prefix and the suffix (12.4). */
        affixes.prefix = first.text;
        affixes.suffix = first.text;
        return affixes;
    }
    if (!first.is_alphanumeric) {
        affixes.prefix = first.text;
    }
    format_run last = first;
    for (size_t byte_offset = first.text.byte_count; byte_offset < format.byte_count;
         byte_offset += last.text.byte_count) {
        last = read_format_run(format, byte_offset);
    }
    if (!last.is_alphanumeric && last.text.bytes != first.text.bytes) {
        affixes.suffix = last.text;
    }
    return affixes;
}

/* -------------------------------------------------------- Decimal formats */

sxml_decimal_format_settings sxml_decimal_format_default_settings(void) {
    return (sxml_decimal_format_settings){
        .decimal_separator = {.marker = '.', .rendition = SXML_BYTE_VIEW_INITIALIZER(".")},
        .grouping_separator = {.marker = ',', .rendition = SXML_BYTE_VIEW_INITIALIZER(",")},
        .exponent_separator = {.marker = 'e', .rendition = SXML_BYTE_VIEW_INITIALIZER("e")},
        .percent = {.marker = '%', .rendition = SXML_BYTE_VIEW_INITIALIZER("%")},
        .per_mille = {.marker = 0x2030U, .rendition = SXML_BYTE_VIEW_INITIALIZER("\xE2\x80\xB0")},
        .zero_digit = '0',
        .digit = '#',
        .pattern_separator = ';',
        .infinity = SXML_BYTE_VIEW_INITIALIZER("Infinity"),
        .not_a_number = SXML_BYTE_VIEW_INITIALIZER("NaN"),
        .minus_sign = SXML_BYTE_VIEW_INITIALIZER("-")};
}

/* Reads "c" or "c:rendition" (section 4.9.2: '.|.:.*'). */
static sxml_status read_marker_property(sxml_byte_view value, sxml_decimal_format_marker *marker) {
    if (value.byte_count == 0) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    const decoded_codepoint decoded = decode_codepoint(value, 0);
    if (decoded.byte_count == value.byte_count) {
        *marker = (sxml_decimal_format_marker){.marker = decoded.codepoint, .rendition = value};
        return SXML_STATUS_OK;
    }
    if (value.bytes[decoded.byte_count] != ':') {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    *marker = (sxml_decimal_format_marker){
        .marker = decoded.codepoint,
        .rendition = {.bytes = value.bytes + decoded.byte_count + 1,
                      .byte_count = value.byte_count - decoded.byte_count - 1}};
    return SXML_STATUS_OK;
}

/* Reads one character (section 4.9.2: '.'). */
static sxml_status read_character_property(sxml_byte_view value, uint32_t *codepoint) {
    if (value.byte_count == 0 || count_codepoints(value) != 1) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    *codepoint = decode_codepoint(value, 0).codepoint;
    return SXML_STATUS_OK;
}

static sxml_status apply_property(const sxml_decimal_format_property *property,
                                  sxml_decimal_format_settings *settings) {
    const sxml_byte_view value = property->property_value;
    switch (property->property_kind) {
    case SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR:
        return read_marker_property(value, &settings->decimal_separator);
    case SXML_DECIMAL_FORMAT_PROPERTY_GROUPING_SEPARATOR:
        return read_marker_property(value, &settings->grouping_separator);
    case SXML_DECIMAL_FORMAT_PROPERTY_EXPONENT_SEPARATOR:
        return read_marker_property(value, &settings->exponent_separator);
    case SXML_DECIMAL_FORMAT_PROPERTY_PERCENT:
        return read_marker_property(value, &settings->percent);
    case SXML_DECIMAL_FORMAT_PROPERTY_PER_MILLE:
        return read_marker_property(value, &settings->per_mille);
    case SXML_DECIMAL_FORMAT_PROPERTY_ZERO_DIGIT:
        return read_character_property(value, &settings->zero_digit);
    case SXML_DECIMAL_FORMAT_PROPERTY_DIGIT:
        return read_character_property(value, &settings->digit);
    case SXML_DECIMAL_FORMAT_PROPERTY_PATTERN_SEPARATOR:
        return read_character_property(value, &settings->pattern_separator);
    case SXML_DECIMAL_FORMAT_PROPERTY_INFINITY:
        settings->infinity = value;
        return SXML_STATUS_OK;
    case SXML_DECIMAL_FORMAT_PROPERTY_NAN:
        settings->not_a_number = value;
        return SXML_STATUS_OK;
    case SXML_DECIMAL_FORMAT_PROPERTY_MINUS_SIGN:
        settings->minus_sign = value;
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The checks of section 4.9.1: a zero digit of category Nd with value 0,
   and seven distinct markers outside the digit family. */
static sxml_status check_settings(const sxml_decimal_format_settings *settings) {
    const uint32_t zero_digit = settings->zero_digit;
    if (!is_category(zero_digit, SXML_REGEX_GENERAL_CATEGORY_ND) || digit_value(zero_digit) != 0) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    const uint32_t markers[] = {
        settings->decimal_separator.marker, settings->grouping_separator.marker,
        settings->exponent_separator.marker, settings->percent.marker,
        settings->per_mille.marker,          settings->digit,
        settings->pattern_separator};
    const size_t marker_count = sizeof(markers) / sizeof(markers[0]);
    for (size_t marker_index = 0; marker_index < marker_count; marker_index += 1) {
        if (markers[marker_index] >= zero_digit && markers[marker_index] <= zero_digit + 9U) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
        for (size_t other_index = marker_index + 1; other_index < marker_count; other_index += 1) {
            if (markers[marker_index] == markers[other_index]) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
        }
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_decimal_format_apply(const sxml_decimal_format_change *change,
                                      sxml_decimal_format_settings *settings) {
    sxml_decimal_format_settings result = *change->base_settings;
    const sxml_decimal_format_property_view properties = change->decimal_format_properties;
    sxml_status status = SXML_STATUS_OK;
    for (size_t property_index = 0;
         status == SXML_STATUS_OK && property_index < properties.decimal_format_property_count;
         property_index += 1) {
        const sxml_decimal_format_property *property =
            &properties.decimal_format_properties[property_index];
        if (property->property_value.byte_count > 0 && property->property_value.bytes == NULL) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        status = apply_property(property, &result);
    }
    if (status == SXML_STATUS_OK) {
        status = check_settings(&result);
    }
    if (status == SXML_STATUS_OK) {
        *settings = result;
    }
    return status;
}

/* -------------------------------------------------------- Number pictures */

/* What a character of a fn:format-number picture is (section 4.9.3). */
typedef enum picture_character {
    SXML_PICTURE_CHARACTER_PASSIVE,
    SXML_PICTURE_CHARACTER_DECIMAL_SEPARATOR,
    SXML_PICTURE_CHARACTER_GROUPING_SEPARATOR,
    SXML_PICTURE_CHARACTER_DIGIT,
    SXML_PICTURE_CHARACTER_OPTIONAL_DIGIT,
    SXML_PICTURE_CHARACTER_PATTERN_SEPARATOR,
    SXML_PICTURE_CHARACTER_EXPONENT_SEPARATOR,
} picture_character;

static bool is_family_digit(const sxml_decimal_format_settings *settings, uint32_t codepoint) {
    return codepoint >= settings->zero_digit && codepoint <= settings->zero_digit + 9U;
}

/* Classifies a codepoint; the exponent-separator character is reported as
   such and is an active sign only between active characters. */
static picture_character classify_picture_character(const sxml_decimal_format_settings *settings,
                                                    uint32_t codepoint) {
    if (is_family_digit(settings, codepoint)) {
        return SXML_PICTURE_CHARACTER_DIGIT;
    }
    if (codepoint == settings->digit) {
        return SXML_PICTURE_CHARACTER_OPTIONAL_DIGIT;
    }
    if (codepoint == settings->decimal_separator.marker) {
        return SXML_PICTURE_CHARACTER_DECIMAL_SEPARATOR;
    }
    if (codepoint == settings->grouping_separator.marker) {
        return SXML_PICTURE_CHARACTER_GROUPING_SEPARATOR;
    }
    if (codepoint == settings->pattern_separator) {
        return SXML_PICTURE_CHARACTER_PATTERN_SEPARATOR;
    }
    if (codepoint == settings->exponent_separator.marker) {
        return SXML_PICTURE_CHARACTER_EXPONENT_SEPARATOR;
    }
    return SXML_PICTURE_CHARACTER_PASSIVE;
}

/* Active apart from the exponent separator. */
static bool is_plainly_active(picture_character character) {
    return character != SXML_PICTURE_CHARACTER_PASSIVE &&
           character != SXML_PICTURE_CHARACTER_EXPONENT_SEPARATOR;
}

/* A sub-picture to analyse with the settings. */
typedef struct sub_picture_request {
    sxml_byte_view sub_picture;
    const sxml_decimal_format_settings *settings;
} sub_picture_request;

/* The digit signs of a part (integer, fraction or exponent). */
typedef struct part_counts {
    size_t mandatory_count;
    size_t optional_count;
} part_counts;

/* The byte offsets of a sub-picture's pieces, found in one pass. */
typedef struct sub_picture_layout {
    size_t active_start;
    size_t active_end;
    bool has_exponent;
    size_t exponent_start;
    size_t exponent_end;
    size_t percent_count;
    size_t per_mille_count;
} sub_picture_layout;

/* Finds the leftmost and rightmost active characters, the
   exponent-separator-sign (one preceded and followed by an active
   character) and the percent and per-mille characters. */
static sxml_status find_layout(sub_picture_request request, sub_picture_layout *layout) {
    const sxml_byte_view sub_picture = request.sub_picture;
    const sxml_decimal_format_settings *settings = request.settings;
    sub_picture_layout result = {.active_start = sub_picture.byte_count,
                                 .active_end = 0,
                                 .has_exponent = false,
                                 .exponent_start = 0,
                                 .exponent_end = 0,
                                 .percent_count = 0,
                                 .per_mille_count = 0};
    size_t byte_offset = 0;
    while (byte_offset < sub_picture.byte_count) {
        const decoded_codepoint decoded = decode_codepoint(sub_picture, byte_offset);
        if (is_plainly_active(classify_picture_character(settings, decoded.codepoint))) {
            if (result.active_start == sub_picture.byte_count) {
                result.active_start = byte_offset;
            }
            result.active_end = byte_offset + decoded.byte_count;
        }
        if (decoded.codepoint == settings->percent.marker) {
            result.percent_count += 1;
        }
        if (decoded.codepoint == settings->per_mille.marker) {
            result.per_mille_count += 1;
        }
        byte_offset += decoded.byte_count;
    }
    if (result.active_start == sub_picture.byte_count) {
        /* No active character: the mantissa has no digit sign. */
        return SXML_STATUS_MALFORMED_INPUT;
    }
    byte_offset = result.active_start;
    while (byte_offset < result.active_end) {
        const decoded_codepoint decoded = decode_codepoint(sub_picture, byte_offset);
        if (classify_picture_character(settings, decoded.codepoint) ==
            SXML_PICTURE_CHARACTER_EXPONENT_SEPARATOR) {
            /* "A sub-picture must not contain more than one character that
               is treated as an exponent-separator-sign." */
            if (result.has_exponent) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            result.has_exponent = true;
            result.exponent_start = byte_offset;
            result.exponent_end = byte_offset + decoded.byte_count;
        } else if (classify_picture_character(settings, decoded.codepoint) ==
                   SXML_PICTURE_CHARACTER_PASSIVE) {
            /* "A sub-picture must not contain a passive character that is
               preceded by an active character and that is followed by
               another active character." */
            return SXML_STATUS_MALFORMED_INPUT;
        }
        byte_offset += decoded.byte_count;
    }
    if (result.percent_count + result.per_mille_count > 1 ||
        (result.has_exponent && result.percent_count + result.per_mille_count > 0)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    *layout = result;
    return SXML_STATUS_OK;
}

/* A part of the mantissa to check: the integer or the fractional part. */
typedef struct part_request {
    sxml_byte_view part;
    const sxml_decimal_format_settings *settings;
    bool is_fraction;
} part_request;

/* Counts the digit signs of a part and checks their order: no mandatory
   digit before an optional one in the integer part, no optional digit
   before a mandatory one in the fractional part, no two adjacent grouping
   separators. */
static sxml_status check_part(part_request request, part_counts *counts) {
    part_counts result = {.mandatory_count = 0, .optional_count = 0};
    bool was_grouping_separator = false;
    size_t byte_offset = 0;
    while (byte_offset < request.part.byte_count) {
        const decoded_codepoint decoded = decode_codepoint(request.part, byte_offset);
        byte_offset += decoded.byte_count;
        const picture_character character =
            classify_picture_character(request.settings, decoded.codepoint);
        if (character == SXML_PICTURE_CHARACTER_GROUPING_SEPARATOR) {
            if (was_grouping_separator) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            was_grouping_separator = true;
            continue;
        }
        was_grouping_separator = false;
        if (character == SXML_PICTURE_CHARACTER_DIGIT) {
            if (request.is_fraction && result.optional_count > 0) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            result.mandatory_count += 1;
        } else if (character == SXML_PICTURE_CHARACTER_OPTIONAL_DIGIT) {
            if (!request.is_fraction && result.mandatory_count > 0) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            result.optional_count += 1;
        } else {
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    *counts = result;
    return SXML_STATUS_OK;
}

/* A position query on a part: integer positions count digit signs to the
   right of a separator, fraction positions to its left. */
typedef struct part_position_query {
    sxml_byte_view part;
    const sxml_decimal_format_settings *settings;
    bool is_fraction;
    size_t position;
} part_position_query;

/* True when the part has a grouping separator at query.position. */
static bool part_has_separator_at(part_position_query query) {
    size_t digit_sign_count = 0;
    if (query.is_fraction) {
        size_t byte_offset = 0;
        while (byte_offset < query.part.byte_count) {
            const decoded_codepoint decoded = decode_codepoint(query.part, byte_offset);
            byte_offset += decoded.byte_count;
            if (classify_picture_character(query.settings, decoded.codepoint) ==
                SXML_PICTURE_CHARACTER_GROUPING_SEPARATOR) {
                if (digit_sign_count == query.position) {
                    return true;
                }
            } else {
                digit_sign_count += 1;
            }
        }
        return false;
    }
    size_t byte_offset = query.part.byte_count;
    while (byte_offset > 0) {
        size_t start_offset = byte_offset - 1;
        while (start_offset > 0 && (query.part.bytes[start_offset] & 0xC0U) == 0x80U) {
            start_offset -= 1;
        }
        const uint32_t codepoint = decode_codepoint(query.part, start_offset).codepoint;
        byte_offset = start_offset;
        if (classify_picture_character(query.settings, codepoint) ==
            SXML_PICTURE_CHARACTER_GROUPING_SEPARATOR) {
            if (digit_sign_count == query.position) {
                return true;
            }
        } else {
            digit_sign_count += 1;
        }
    }
    return false;
}

/* Decides whether the integer part's grouping is regular (section 4.9.4)
   and writes its size to sub->grouping_size. */
static void read_integer_grouping(const sxml_decimal_format_settings *settings,
                                  sxml_number_sub_picture *sub) {
    const sxml_byte_view part = sub->integer_part;
    size_t digit_sign_count = 0;
    size_t separator_count = 0;
    size_t smallest_position = 0;
    size_t byte_offset = part.byte_count;
    while (byte_offset > 0) {
        size_t start_offset = byte_offset - 1;
        while (start_offset > 0 && (part.bytes[start_offset] & 0xC0U) == 0x80U) {
            start_offset -= 1;
        }
        const uint32_t codepoint = decode_codepoint(part, start_offset).codepoint;
        byte_offset = start_offset;
        if (classify_picture_character(settings, codepoint) ==
            SXML_PICTURE_CHARACTER_GROUPING_SEPARATOR) {
            if (separator_count == 0) {
                smallest_position = digit_sign_count;
            }
            separator_count += 1;
        } else {
            digit_sign_count += 1;
        }
    }
    sub->has_regular_grouping = false;
    sub->grouping_size = 0;
    if (separator_count == 0 || smallest_position == 0) {
        return;
    }
    /* Every position a multiple of G and every multiple of G below the
       number of digit signs a position; a separator before every digit
       sign (",##0") stands at that number and counts as a multiple. */
    size_t multiple_count = 0;
    for (size_t position = smallest_position; position <= digit_sign_count;
         position += smallest_position) {
        const bool is_present = part_has_separator_at((part_position_query){
            .part = part, .settings = settings, .is_fraction = false, .position = position});
        if (!is_present && position < digit_sign_count) {
            return;
        }
        multiple_count += is_present ? 1U : 0U;
    }
    if (multiple_count == separator_count) {
        sub->has_regular_grouping = true;
        sub->grouping_size = smallest_position;
    }
}

/* Analyses one sub-picture (sections 4.9.3 and 4.9.4). */
static sxml_status analyse_sub_picture(sub_picture_request request, sxml_number_sub_picture *sub) {
    const sxml_byte_view sub_picture = request.sub_picture;
    const sxml_decimal_format_settings *settings = request.settings;
    sub_picture_layout layout;
    sxml_status status = find_layout(request, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t mantissa_end = layout.has_exponent ? layout.exponent_start : layout.active_end;
    const sxml_byte_view mantissa = {.bytes = sub_picture.bytes + layout.active_start,
                                     .byte_count = mantissa_end - layout.active_start};
    /* The exponent part: one or more digits of the family, and nothing
       else active. */
    size_t exponent_digit_count = 0;
    if (layout.has_exponent) {
        const sxml_byte_view exponent = {.bytes = sub_picture.bytes + layout.exponent_end,
                                         .byte_count = layout.active_end - layout.exponent_end};
        size_t byte_offset = 0;
        while (byte_offset < exponent.byte_count) {
            const decoded_codepoint decoded = decode_codepoint(exponent, byte_offset);
            byte_offset += decoded.byte_count;
            if (!is_family_digit(settings, decoded.codepoint)) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            exponent_digit_count += 1;
        }
        if (exponent_digit_count == 0) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    /* At most one decimal separator; no grouping separator next to it, or
       at the end of the integer part without it. */
    size_t decimal_offset = mantissa.byte_count;
    size_t decimal_byte_count = 0;
    size_t byte_offset = 0;
    while (byte_offset < mantissa.byte_count) {
        const decoded_codepoint decoded = decode_codepoint(mantissa, byte_offset);
        if (classify_picture_character(settings, decoded.codepoint) ==
            SXML_PICTURE_CHARACTER_DECIMAL_SEPARATOR) {
            if (decimal_byte_count != 0) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            decimal_offset = byte_offset;
            decimal_byte_count = decoded.byte_count;
        }
        byte_offset += decoded.byte_count;
    }
    const sxml_byte_view integer_part = {.bytes = mantissa.bytes, .byte_count = decimal_offset};
    const sxml_byte_view fraction_part = {
        .bytes = mantissa.bytes + decimal_offset + decimal_byte_count,
        .byte_count = mantissa.byte_count - decimal_offset - decimal_byte_count};
    if (integer_part.byte_count > 0) {
        size_t last_offset = integer_part.byte_count - 1;
        while (last_offset > 0 && (integer_part.bytes[last_offset] & 0xC0U) == 0x80U) {
            last_offset -= 1;
        }
        if (classify_picture_character(settings,
                                       decode_codepoint(integer_part, last_offset).codepoint) ==
            SXML_PICTURE_CHARACTER_GROUPING_SEPARATOR) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    if (decimal_byte_count != 0 && fraction_part.byte_count > 0 &&
        classify_picture_character(settings, decode_codepoint(fraction_part, 0).codepoint) ==
            SXML_PICTURE_CHARACTER_GROUPING_SEPARATOR) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    part_counts integer_counts;
    part_counts fraction_counts;
    status = check_part(
        (part_request){.part = integer_part, .settings = settings, .is_fraction = false},
        &integer_counts);
    if (status == SXML_STATUS_OK) {
        status = check_part(
            (part_request){.part = fraction_part, .settings = settings, .is_fraction = true},
            &fraction_counts);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (integer_counts.mandatory_count + integer_counts.optional_count +
            fraction_counts.mandatory_count + fraction_counts.optional_count ==
        0) {
        /* "The mantissa part ... must contain at least one character that
           is either an optional digit character or a member of the
           decimal digit family." */
        return SXML_STATUS_MALFORMED_INPUT;
    }
    sxml_number_sub_picture result = {
        .prefix = {.bytes = sub_picture.bytes, .byte_count = layout.active_start},
        .suffix = {.bytes = sub_picture.bytes + layout.active_end,
                   .byte_count = sub_picture.byte_count - layout.active_end},
        .integer_part = integer_part,
        .fraction_part = fraction_part,
        .multiplier = layout.percent_count > 0 ? 100U : layout.per_mille_count > 0 ? 1000U : 1U,
        .minimum_integer_part_size = integer_counts.mandatory_count,
        .scaling_factor = integer_counts.mandatory_count,
        .minimum_fraction_part_size = fraction_counts.mandatory_count,
        .maximum_fraction_part_size =
            fraction_counts.mandatory_count + fraction_counts.optional_count,
        .minimum_exponent_size = exponent_digit_count,
        .has_regular_grouping = false,
        .grouping_size = 0};
    /* The adjustments of section 4.9.4. */
    if (result.minimum_integer_part_size == 0 && result.maximum_fraction_part_size == 0) {
        if (layout.has_exponent) {
            result.minimum_fraction_part_size = 1;
            result.maximum_fraction_part_size = 1;
        } else {
            result.minimum_integer_part_size = 1;
        }
    }
    if (layout.has_exponent && result.minimum_integer_part_size == 0 &&
        integer_counts.optional_count > 0) {
        result.minimum_integer_part_size = 1;
    }
    if (result.minimum_integer_part_size == 0 && result.minimum_fraction_part_size == 0) {
        result.minimum_fraction_part_size = 1;
    }
    read_integer_grouping(settings, &result);
    *sub = result;
    return SXML_STATUS_OK;
}

sxml_status sxml_number_picture_parse(const sxml_number_picture_request *request,
                                      sxml_number_picture *picture) {
    const sxml_byte_view picture_string = request->picture_string;
    const sxml_decimal_format_settings *settings = request->settings;
    /* "A picture-string must not contain more than one instance of the
       pattern-separator character." */
    size_t separator_offset = picture_string.byte_count;
    size_t separator_byte_count = 0;
    size_t byte_offset = 0;
    while (byte_offset < picture_string.byte_count) {
        const decoded_codepoint decoded = decode_codepoint(picture_string, byte_offset);
        if (decoded.codepoint == settings->pattern_separator) {
            if (separator_byte_count != 0) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            separator_offset = byte_offset;
            separator_byte_count = decoded.byte_count;
        }
        byte_offset += decoded.byte_count;
    }
    sxml_number_picture result = {.settings = *settings, .has_negative_sub_picture = false};
    sxml_status status = analyse_sub_picture(
        (sub_picture_request){
            .sub_picture = {.bytes = picture_string.bytes, .byte_count = separator_offset},
            .settings = settings},
        &result.positive_sub_picture);
    result.negative_sub_picture = result.positive_sub_picture;
    if (status == SXML_STATUS_OK && separator_byte_count != 0) {
        result.has_negative_sub_picture = true;
        const size_t negative_start = separator_offset + separator_byte_count;
        status = analyse_sub_picture(
            (sub_picture_request){
                .sub_picture = {.bytes = picture_string.bytes + negative_start,
                                .byte_count = picture_string.byte_count - negative_start},
                .settings = settings},
            &result.negative_sub_picture);
    }
    if (status == SXML_STATUS_OK) {
        *picture = result;
    }
    return status;
}

uint32_t sxml_number_picture_get_multiplier(const sxml_number_picture *picture,
                                            sxml_number_sign sign) {
    return sign == SXML_NUMBER_SIGN_NEGATIVE ? picture->negative_sub_picture.multiplier
                                             : picture->positive_sub_picture.multiplier;
}

/* ------------------------------------------------------ Number formatting */

/*
 * A finite number as significant digits and the position of the decimal
 * point: the value is 0.D x 10^point_position, D without leading or
 * trailing zeros (empty for zero). digits is owned through the allocator.
 */
typedef struct digit_string {
    sxml_allocator allocator;
    uint8_t *digits;
    size_t digit_count;
    int64_t point_position;
} digit_string;

static void digit_string_release(digit_string *number) {
    if (number->digits != NULL) {
        sxml_allocator_release(number->allocator, number->digits);
    }
    number->digits = NULL;
    number->digit_count = 0;
}

/* Reads magnitude text ("123.45") with a decimal point shift. */
typedef struct magnitude_reading {
    sxml_byte_view magnitude_text;
    uint32_t decimal_point_shift;
} magnitude_reading;

static sxml_status read_digit_string(magnitude_reading reading, digit_string *number) {
    const sxml_byte_view text = reading.magnitude_text;
    size_t point_offset = text.byte_count;
    for (size_t byte_offset = 0; byte_offset < text.byte_count; byte_offset += 1) {
        const uint8_t byte = text.bytes[byte_offset];
        if (byte == '.' && point_offset == text.byte_count) {
            point_offset = byte_offset;
        } else if (byte < '0' || byte > '9') {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    if ((uint64_t)text.byte_count > (uint64_t)INT64_MAX / 2U) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        number->allocator,
        (sxml_allocation_request){.byte_count = text.byte_count + 1, .alignment = 1}, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *digits = memory;
    size_t digit_count = 0;
    int64_t point_position = (int64_t)point_offset;
    for (size_t byte_offset = 0; byte_offset < text.byte_count; byte_offset += 1) {
        const uint8_t byte = text.bytes[byte_offset];
        if (byte == '.') {
            continue;
        }
        if (digit_count == 0 && byte == '0') {
            /* A leading zero moves the point left relative to D. */
            point_position -= 1;
            continue;
        }
        digits[digit_count] = byte;
        digit_count += 1;
    }
    while (digit_count > 0 && digits[digit_count - 1] == '0') {
        digit_count -= 1;
    }
    number->digits = digits;
    number->digit_count = digit_count;
    number->point_position = digit_count == 0 ? 0 : point_position + reading.decimal_point_shift;
    return SXML_STATUS_OK;
}

/*
 * Rounds the number half to even to fraction_digit_count places after the
 * point (fn:round-half-to-even, as section 4.9.5 asks), exactly on the
 * digits.
 */
static void round_digit_string(digit_string *number, size_t fraction_digit_count) {
    if (number->digit_count == 0) {
        return;
    }
    const int64_t kept_count = number->point_position + (int64_t)fraction_digit_count;
    if (kept_count >= (int64_t)number->digit_count) {
        return;
    }
    if (kept_count < 0) {
        /* Below half a unit of the last place. */
        number->digit_count = 0;
        number->point_position = 0;
        return;
    }
    const size_t keep = (size_t)kept_count;
    const uint8_t next_digit = number->digits[keep];
    const bool has_more = keep + 1 < number->digit_count;
    const bool is_last_kept_odd = keep > 0 && ((number->digits[keep - 1] - '0') % 2) != 0;
    const bool rounds_up =
        next_digit > '5' || (next_digit == '5' && (has_more || is_last_kept_odd));
    number->digit_count = keep;
    if (rounds_up) {
        size_t digit_index = keep;
        while (digit_index > 0 && number->digits[digit_index - 1] == '9') {
            number->digits[digit_index - 1] = '0';
            digit_index -= 1;
        }
        if (digit_index == 0) {
            /* 0.99... rounds to 1 at the next place up. */
            number->digits[0] = '1';
            number->digit_count = 1;
            number->point_position += 1;
        } else {
            number->digits[digit_index - 1] = (uint8_t)(number->digits[digit_index - 1] + 1U);
        }
    }
    while (number->digit_count > 0 && number->digits[number->digit_count - 1] == '0') {
        number->digit_count -= 1;
    }
    if (number->digit_count == 0) {
        number->point_position = 0;
    }
}

/* Appends a prefix or suffix: passive characters as written, except the
   percent and per-mille markers, which become their renditions. */
typedef struct passive_text {
    sxml_byte_view characters;
    const sxml_decimal_format_settings *settings;
} passive_text;

static sxml_status append_passive(passive_text passive, sxml_format_text *text) {
    sxml_status status = SXML_STATUS_OK;
    size_t byte_offset = 0;
    while (status == SXML_STATUS_OK && byte_offset < passive.characters.byte_count) {
        const decoded_codepoint decoded = decode_codepoint(passive.characters, byte_offset);
        if (decoded.codepoint == passive.settings->percent.marker) {
            status = sxml_format_text_append(text, passive.settings->percent.rendition);
        } else if (decoded.codepoint == passive.settings->per_mille.marker) {
            status = sxml_format_text_append(text, passive.settings->per_mille.rendition);
        } else {
            status = sxml_format_text_append(
                text, (sxml_byte_view){.bytes = passive.characters.bytes + byte_offset,
                                       .byte_count = decoded.byte_count});
        }
        byte_offset += decoded.byte_count;
    }
    return status;
}

/* What the mantissa is written with. */
typedef struct mantissa_writing {
    const sxml_number_sub_picture *sub;
    const sxml_decimal_format_settings *settings;
    const digit_string *number;
} mantissa_writing;

/* The digit at index (from the left) of the integer digits of a number
   whose integer digit count before padding is integer_digit_count and
   which is padded with padding_count zeros. */
typedef struct integer_digit_query {
    const digit_string *number;
    size_t padding_count;
    size_t digit_index;
} integer_digit_query;

static uint32_t integer_digit_value(integer_digit_query query) {
    if (query.digit_index < query.padding_count) {
        return 0;
    }
    const size_t significant_index = query.digit_index - query.padding_count;
    return significant_index < query.number->digit_count
               ? (uint32_t)(query.number->digits[significant_index] - '0')
               : 0U;
}

/* Writes the integer and fractional digits with grouping separators and
   the decimal separator (section 4.9.5). */
static sxml_status append_mantissa(mantissa_writing writing, sxml_format_text *text) {
    const sxml_number_sub_picture *sub = writing.sub;
    const sxml_decimal_format_settings *settings = writing.settings;
    const digit_string *number = writing.number;
    const size_t integer_digit_count =
        number->point_position > 0 ? (size_t)number->point_position : 0U;
    const size_t padding_count = integer_digit_count < sub->minimum_integer_part_size
                                     ? sub->minimum_integer_part_size - integer_digit_count
                                     : 0U;
    const size_t total_integer_count = padding_count + integer_digit_count;
    const size_t integer_sign_count = count_codepoints(sub->integer_part);
    sxml_status status = SXML_STATUS_OK;
    for (size_t digit_index = 0; status == SXML_STATUS_OK && digit_index < total_integer_count;
         digit_index += 1) {
        const size_t position = total_integer_count - digit_index;
        if (digit_index > 0) {
            const bool has_separator =
                sub->has_regular_grouping
                    ? position % sub->grouping_size == 0
                    : position < integer_sign_count &&
                          part_has_separator_at((part_position_query){.part = sub->integer_part,
                                                                      .settings = settings,
                                                                      .is_fraction = false,
                                                                      .position = position});
            if (has_separator) {
                status = sxml_format_text_append(text, settings->grouping_separator.rendition);
            }
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_format_text_append_codepoint(
                text, settings->zero_digit + integer_digit_value((integer_digit_query){
                                                 .number = number,
                                                 .padding_count = padding_count,
                                                 .digit_index = digit_index}));
        }
    }
    /* Fraction digits: leading zeros when the point is left of D, then
       the digits of D right of the point, then zeros to the minimum. */
    const size_t leading_zero_count =
        number->point_position < 0 ? (size_t)(-number->point_position) : 0U;
    const size_t significant_fraction_count =
        number->digit_count > integer_digit_count ? number->digit_count - integer_digit_count : 0U;
    size_t fraction_count = number->digit_count == 0 ? 0U
                                                     : leading_zero_count +
                                                           significant_fraction_count;
    if (fraction_count < sub->minimum_fraction_part_size) {
        fraction_count = sub->minimum_fraction_part_size;
    }
    if (status == SXML_STATUS_OK && fraction_count > 0) {
        status = sxml_format_text_append(text, settings->decimal_separator.rendition);
    }
    const size_t fraction_sign_count = count_codepoints(sub->fraction_part);
    for (size_t digit_index = 0; status == SXML_STATUS_OK && digit_index < fraction_count;
         digit_index += 1) {
        if (digit_index > 0 && digit_index < fraction_sign_count &&
            part_has_separator_at((part_position_query){.part = sub->fraction_part,
                                                        .settings = settings,
                                                        .is_fraction = true,
                                                        .position = digit_index})) {
            status = sxml_format_text_append(text, settings->grouping_separator.rendition);
        }
        uint32_t value = 0;
        if (number->digit_count > 0 && digit_index >= leading_zero_count &&
            digit_index - leading_zero_count < significant_fraction_count) {
            value = (uint32_t)(number->digits[integer_digit_count + digit_index -
                                              leading_zero_count] -
                               '0');
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_format_text_append_codepoint(text, settings->zero_digit + value);
        }
    }
    return status;
}

/* An exponent to write with at least minimum_size digits. */
typedef struct exponent_writing {
    const sxml_decimal_format_settings *settings;
    int64_t exponent;
    size_t minimum_size;
} exponent_writing;

static sxml_status append_exponent(exponent_writing writing, sxml_format_text *text) {
    const sxml_decimal_format_settings *settings = writing.settings;
    sxml_status status = sxml_format_text_append(text, settings->exponent_separator.rendition);
    if (status == SXML_STATUS_OK && writing.exponent < 0) {
        status = sxml_format_text_append(text, settings->minus_sign);
    }
    uint64_t magnitude = writing.exponent < 0 ? (uint64_t)(-(writing.exponent + 1)) + 1U
                                              : (uint64_t)writing.exponent;
    uint8_t digit_values[24];
    size_t digit_count = 0;
    do {
        digit_values[digit_count] = (uint8_t)(magnitude % 10U);
        digit_count += 1;
        magnitude /= 10U;
    } while (magnitude > 0 && digit_count < sizeof(digit_values));
    if (status == SXML_STATUS_OK && digit_count < writing.minimum_size) {
        status = append_run(text, (codepoint_run){.codepoint = settings->zero_digit,
                                                  .repeat_count =
                                                      writing.minimum_size - digit_count});
    }
    for (size_t digit_index = digit_count; status == SXML_STATUS_OK && digit_index > 0;
         digit_index -= 1) {
        status = sxml_format_text_append_codepoint(
            text, settings->zero_digit + digit_values[digit_index - 1]);
    }
    return status;
}

sxml_status sxml_format_number(const sxml_number_formatting *formatting, sxml_format_text *text) {
    const sxml_number_picture *picture = formatting->picture;
    const sxml_decimal_format_settings *settings = &picture->settings;
    if (formatting->number_class == SXML_NUMBER_CLASS_NOT_A_NUMBER) {
        /* "If the input number is NaN (not a number), the result is the
           value of the NaN property (with no prefix or suffix)." */
        return sxml_format_text_append(text, settings->not_a_number);
    }
    const bool is_negative = formatting->sign == SXML_NUMBER_SIGN_NEGATIVE;
    const sxml_number_sub_picture *sub =
        is_negative ? &picture->negative_sub_picture : &picture->positive_sub_picture;
    sxml_status status = SXML_STATUS_OK;
    if (is_negative && !picture->has_negative_sub_picture) {
        /* One sub-picture: the negative prefix is the minus sign and the
           positive prefix (section 4.9.4). */
        status = sxml_format_text_append(text, settings->minus_sign);
    }
    if (status == SXML_STATUS_OK) {
        status = append_passive((passive_text){.characters = sub->prefix, .settings = settings},
                                text);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (formatting->number_class == SXML_NUMBER_CLASS_INFINITE) {
        status = sxml_format_text_append(text, settings->infinity);
    } else {
        digit_string number = {.allocator = text->allocator,
                               .digits = NULL,
                               .digit_count = 0,
                               .point_position = 0};
        status = read_digit_string(
            (magnitude_reading){.magnitude_text = formatting->magnitude_text,
                                .decimal_point_shift = formatting->decimal_point_shift},
            &number);
        int64_t exponent = 0;
        if (status == SXML_STATUS_OK && sub->minimum_exponent_size > 0 && number.digit_count > 0) {
            /* Mantissa M with 10^(N-1) <= M < 10^N, N the scaling factor,
               and exponent E; a mantissa that rounds up to 10^N stays so
               (QT4 numberformat304: 0.99999999 by "0.0e0" is
               "10.0e-1"). */
            exponent = number.point_position - (int64_t)sub->scaling_factor;
            number.point_position = (int64_t)sub->scaling_factor;
        }
        if (status == SXML_STATUS_OK) {
            round_digit_string(&number, sub->maximum_fraction_part_size);
            status = append_mantissa(
                (mantissa_writing){.sub = sub, .settings = settings, .number = &number}, text);
        }
        if (status == SXML_STATUS_OK && sub->minimum_exponent_size > 0) {
            status = append_exponent((exponent_writing){.settings = settings,
                                                        .exponent = exponent,
                                                        .minimum_size = sub->minimum_exponent_size},
                                     text);
        }
        digit_string_release(&number);
    }
    if (status == SXML_STATUS_OK) {
        status = append_passive((passive_text){.characters = sub->suffix, .settings = settings},
                                text);
    }
    return status;
}
