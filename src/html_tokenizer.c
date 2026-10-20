/*
 * The HTML tokenizer (src/html_internal.h; HTML Living Standard, 13.2.5
 * "Tokenization"; D201): the state machine of the specification as one
 * loop over a state variable, which runs until a token other than
 * characters is ready. Characters accumulate in a run that is handed out
 * before that token, so tree construction sees every token in order and
 * can switch the state (13.2.6.2) before the tokenizer reads on.
 *
 * Named character references (13.5) are looked up in the table fn:char
 * already uses (src/unicode_internal.h), which holds every name with its
 * semicolon; the names that may also appear without one are listed here.
 *
 * Golden file: src/regex.c (a hand-written reader over text, a loop with
 * explicit state, no recursion).
 */
#include "html_internal.h"

#include "unicode_internal.h"

#include <string.h>

/* The conceptual end-of-file character (13.2.5). */
#define SXML_HTML_END_OF_INPUT UINT32_MAX

/* The tokenizer states, in the order of 13.2.5.1 to 13.2.5.84. */
typedef enum sxml_html_tokenizer_state {
    SXML_HTML_STATE_DATA,
    SXML_HTML_STATE_RCDATA,
    SXML_HTML_STATE_RAWTEXT,
    SXML_HTML_STATE_SCRIPT_DATA,
    SXML_HTML_STATE_PLAINTEXT,
    SXML_HTML_STATE_TAG_OPEN,
    SXML_HTML_STATE_END_TAG_OPEN,
    SXML_HTML_STATE_TAG_NAME,
    SXML_HTML_STATE_RCDATA_LESS_THAN_SIGN,
    SXML_HTML_STATE_RCDATA_END_TAG_OPEN,
    SXML_HTML_STATE_RCDATA_END_TAG_NAME,
    SXML_HTML_STATE_RAWTEXT_LESS_THAN_SIGN,
    SXML_HTML_STATE_RAWTEXT_END_TAG_OPEN,
    SXML_HTML_STATE_RAWTEXT_END_TAG_NAME,
    SXML_HTML_STATE_SCRIPT_DATA_LESS_THAN_SIGN,
    SXML_HTML_STATE_SCRIPT_DATA_END_TAG_OPEN,
    SXML_HTML_STATE_SCRIPT_DATA_END_TAG_NAME,
    SXML_HTML_STATE_SCRIPT_DATA_ESCAPE_START,
    SXML_HTML_STATE_SCRIPT_DATA_ESCAPE_START_DASH,
    SXML_HTML_STATE_SCRIPT_DATA_ESCAPED,
    SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_DASH,
    SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_DASH_DASH,
    SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_LESS_THAN_SIGN,
    SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_END_TAG_OPEN,
    SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_END_TAG_NAME,
    SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPE_START,
    SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED,
    SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_DASH,
    SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_DASH_DASH,
    SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_LESS_THAN_SIGN,
    SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPE_END,
    SXML_HTML_STATE_BEFORE_ATTRIBUTE_NAME,
    SXML_HTML_STATE_ATTRIBUTE_NAME,
    SXML_HTML_STATE_AFTER_ATTRIBUTE_NAME,
    SXML_HTML_STATE_BEFORE_ATTRIBUTE_VALUE,
    SXML_HTML_STATE_ATTRIBUTE_VALUE_DOUBLE_QUOTED,
    SXML_HTML_STATE_ATTRIBUTE_VALUE_SINGLE_QUOTED,
    SXML_HTML_STATE_ATTRIBUTE_VALUE_UNQUOTED,
    SXML_HTML_STATE_AFTER_ATTRIBUTE_VALUE_QUOTED,
    SXML_HTML_STATE_SELF_CLOSING_START_TAG,
    SXML_HTML_STATE_BOGUS_COMMENT,
    SXML_HTML_STATE_MARKUP_DECLARATION_OPEN,
    SXML_HTML_STATE_COMMENT_START,
    SXML_HTML_STATE_COMMENT_START_DASH,
    SXML_HTML_STATE_COMMENT,
    SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN,
    SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG,
    SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG_DASH,
    SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG_DASH_DASH,
    SXML_HTML_STATE_COMMENT_END_DASH,
    SXML_HTML_STATE_COMMENT_END,
    SXML_HTML_STATE_COMMENT_END_BANG,
    SXML_HTML_STATE_DOCTYPE,
    SXML_HTML_STATE_BEFORE_DOCTYPE_NAME,
    SXML_HTML_STATE_DOCTYPE_NAME,
    SXML_HTML_STATE_AFTER_DOCTYPE_NAME,
    SXML_HTML_STATE_AFTER_DOCTYPE_PUBLIC_KEYWORD,
    SXML_HTML_STATE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER,
    SXML_HTML_STATE_DOCTYPE_PUBLIC_IDENTIFIER_DOUBLE_QUOTED,
    SXML_HTML_STATE_DOCTYPE_PUBLIC_IDENTIFIER_SINGLE_QUOTED,
    SXML_HTML_STATE_AFTER_DOCTYPE_PUBLIC_IDENTIFIER,
    SXML_HTML_STATE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS,
    SXML_HTML_STATE_AFTER_DOCTYPE_SYSTEM_KEYWORD,
    SXML_HTML_STATE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER,
    SXML_HTML_STATE_DOCTYPE_SYSTEM_IDENTIFIER_DOUBLE_QUOTED,
    SXML_HTML_STATE_DOCTYPE_SYSTEM_IDENTIFIER_SINGLE_QUOTED,
    SXML_HTML_STATE_AFTER_DOCTYPE_SYSTEM_IDENTIFIER,
    SXML_HTML_STATE_BOGUS_DOCTYPE,
    SXML_HTML_STATE_CDATA_SECTION,
    SXML_HTML_STATE_CDATA_SECTION_BRACKET,
    SXML_HTML_STATE_CDATA_SECTION_END,
    SXML_HTML_STATE_PROCESSING_INSTRUCTION_OPEN,
    SXML_HTML_STATE_PROCESSING_INSTRUCTION_TARGET,
    SXML_HTML_STATE_AFTER_PROCESSING_INSTRUCTION_TARGET,
    SXML_HTML_STATE_PROCESSING_INSTRUCTION_DATA,
    SXML_HTML_STATE_PROCESSING_INSTRUCTION_QUESTIONABLE,
    SXML_HTML_STATE_CHARACTER_REFERENCE,
    SXML_HTML_STATE_NAMED_CHARACTER_REFERENCE,
    SXML_HTML_STATE_AMBIGUOUS_AMPERSAND,
    SXML_HTML_STATE_NUMERIC_CHARACTER_REFERENCE,
    SXML_HTML_STATE_HEXADECIMAL_CHARACTER_REFERENCE_START,
    SXML_HTML_STATE_HEXADECIMAL_CHARACTER_REFERENCE,
    SXML_HTML_STATE_DECIMAL_CHARACTER_REFERENCE,
    SXML_HTML_STATE_NUMERIC_CHARACTER_REFERENCE_END,
} sxml_html_tokenizer_state;

/* An attribute of the tag being built: ranges of the token text. */
typedef struct sxml_html_attribute_record {
    sxml_byte_range attribute_name;
    sxml_byte_range attribute_value;
    /* Removed as a duplicate when the name was complete (13.2.5.33). */
    bool is_duplicate;
} sxml_html_attribute_record;

/* --------------------------------------------------------- Characters */

static bool is_ascii_upper_alpha(uint32_t character) {
    return character >= 'A' && character <= 'Z';
}

static bool is_ascii_lower_alpha(uint32_t character) {
    return character >= 'a' && character <= 'z';
}

static bool is_ascii_alpha(uint32_t character) {
    return is_ascii_upper_alpha(character) || is_ascii_lower_alpha(character);
}

static bool is_ascii_digit(uint32_t character) {
    return character >= '0' && character <= '9';
}

static bool is_ascii_alphanumeric(uint32_t character) {
    return is_ascii_alpha(character) || is_ascii_digit(character);
}

static bool is_ascii_hex_digit(uint32_t character) {
    return is_ascii_digit(character) || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

/* Tab, LF, FF and space: the whitespace of the tokenizer's tables (CR
   never reaches it, 13.2.3.5). */
static bool is_tokenizer_whitespace(uint32_t character) {
    return character == '\t' || character == '\n' || character == '\f' || character == ' ';
}

static uint32_t to_ascii_lower(uint32_t character) {
    return is_ascii_upper_alpha(character) ? character + 0x20U : character;
}

/* ------------------------------------------------------------- Input */

/* Consumes the next input character; SXML_HTML_END_OF_INPUT at the end. */
static uint32_t consume_character(sxml_html_tokenizer *tokenizer) {
    if (tokenizer->input_offset >= tokenizer->input.byte_count) {
        tokenizer->current_byte_count = 0;
        tokenizer->current_character = SXML_HTML_END_OF_INPUT;
        return SXML_HTML_END_OF_INPUT;
    }
    const sxml_xml_decoded_codepoint decoded =
        sxml_xml_decode_codepoint(tokenizer->input, tokenizer->input_offset);
    tokenizer->input_offset += decoded.encoded_byte_count;
    tokenizer->current_byte_count = decoded.encoded_byte_count;
    tokenizer->current_character = decoded.codepoint;
    return decoded.codepoint;
}

/* "Reconsume in state": the current input character is read again. */
static void reconsume_in(sxml_html_tokenizer *tokenizer, sxml_html_tokenizer_state state) {
    tokenizer->input_offset -= tokenizer->current_byte_count;
    tokenizer->current_byte_count = 0;
    tokenizer->state = (uint8_t)state;
}

static void switch_to(sxml_html_tokenizer *tokenizer, sxml_html_tokenizer_state state) {
    tokenizer->state = (uint8_t)state;
}

static void report_parse_error(sxml_html_tokenizer *tokenizer) {
    tokenizer->has_parse_error = true;
}

/* True when the input from the next input character starts with word,
   comparing ASCII case-insensitively when ignores_case. */
typedef struct sxml_html_lookahead {
    sxml_byte_view word;
    bool ignores_case;
} sxml_html_lookahead;

static bool next_characters_are(const sxml_html_tokenizer *tokenizer,
                                sxml_html_lookahead lookahead) {
    const size_t rest = tokenizer->input.byte_count - tokenizer->input_offset;
    if (rest < lookahead.word.byte_count) {
        return false;
    }
    const uint8_t *bytes = tokenizer->input.bytes + tokenizer->input_offset;
    for (size_t byte_index = 0; byte_index < lookahead.word.byte_count; byte_index += 1) {
        uint32_t actual = bytes[byte_index];
        uint32_t expected = lookahead.word.bytes[byte_index];
        if (lookahead.ignores_case) {
            actual = to_ascii_lower(actual);
            expected = to_ascii_lower(expected);
        }
        if (actual != expected) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------ Token building */

/* A field of the token text that grows at its end. */
static sxml_byte_range start_field(const sxml_html_tokenizer *tokenizer) {
    return (sxml_byte_range){.byte_offset = tokenizer->token_text.byte_count, .byte_count = 0};
}

/* Appends character to field, the last field started. */
static sxml_status append_to_field(sxml_html_tokenizer *tokenizer, sxml_byte_range *field,
                                   uint32_t character) {
    const size_t before = tokenizer->token_text.byte_count;
    const sxml_status status =
        sxml_xml_buffer_append_codepoint(&tokenizer->token_text, character);
    field->byte_count += tokenizer->token_text.byte_count - before;
    return status;
}

/* Appends bytes to field, the last field started. */
static sxml_status append_bytes_to_field(sxml_html_tokenizer *tokenizer, sxml_byte_range *field,
                                         sxml_byte_view bytes) {
    const sxml_status status = sxml_xml_buffer_append(&tokenizer->token_text, bytes);
    if (status == SXML_STATUS_OK) {
        field->byte_count += bytes.byte_count;
    }
    return status;
}

static void begin_token(sxml_html_tokenizer *tokenizer, sxml_html_token_kind token_kind) {
    tokenizer->token_kind = token_kind;
    tokenizer->token_text.byte_count = 0;
    tokenizer->attribute_records.element_count = 0;
    tokenizer->name_range = (sxml_byte_range){.byte_offset = 0, .byte_count = 0};
    tokenizer->data_range = (sxml_byte_range){.byte_offset = 0, .byte_count = 0};
    tokenizer->public_identifier_range = (sxml_byte_range){.byte_offset = 0, .byte_count = 0};
    tokenizer->system_identifier_range = (sxml_byte_range){.byte_offset = 0, .byte_count = 0};
    tokenizer->has_name = false;
    tokenizer->has_public_identifier = false;
    tokenizer->has_system_identifier = false;
    tokenizer->forces_quirks = false;
    tokenizer->is_self_closing = false;
}

/* The attribute being built. */
static sxml_html_attribute_record *current_attribute(const sxml_html_tokenizer *tokenizer) {
    return sxml_xml_array_at(&tokenizer->attribute_records,
                             tokenizer->attribute_records.element_count - 1);
}

static sxml_status start_attribute(sxml_html_tokenizer *tokenizer) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&tokenizer->attribute_records, &element);
    if (status == SXML_STATUS_OK) {
        sxml_html_attribute_record *record = element;
        record->attribute_name = start_field(tokenizer);
        record->attribute_value = start_field(tokenizer);
        record->is_duplicate = false;
    }
    return status;
}

static sxml_status append_to_attribute_name(sxml_html_tokenizer *tokenizer, uint32_t character) {
    return append_to_field(tokenizer, &current_attribute(tokenizer)->attribute_name, character);
}

static sxml_status append_to_attribute_value(sxml_html_tokenizer *tokenizer, uint32_t character) {
    return append_to_field(tokenizer, &current_attribute(tokenizer)->attribute_value, character);
}

/* The value of the current attribute starts at the end of the text. */
static void start_attribute_value(sxml_html_tokenizer *tokenizer) {
    current_attribute(tokenizer)->attribute_value = start_field(tokenizer);
}

/* 13.2.5.33: when the attribute name state is left, a name that the
   token already has removes the new attribute. */
static void check_duplicate_attribute(sxml_html_tokenizer *tokenizer) {
    const size_t count = tokenizer->attribute_records.element_count;
    sxml_html_attribute_record *last = current_attribute(tokenizer);
    const sxml_byte_view name = sxml_xml_buffer_view(&tokenizer->token_text, last->attribute_name);
    for (size_t record_index = 0; record_index + 1 < count; record_index += 1) {
        const sxml_html_attribute_record *record =
            sxml_xml_array_at(&tokenizer->attribute_records, record_index);
        if (!record->is_duplicate &&
            sxml_byte_view_equals(
                sxml_xml_buffer_view(&tokenizer->token_text, record->attribute_name), name)) {
            report_parse_error(tokenizer);
            last->is_duplicate = true;
            return;
        }
    }
}

/* Emits the current tag, comment, DOCTYPE or processing instruction
   token: it is ready, and the loop stops. */
static sxml_status emit_current_token(sxml_html_tokenizer *tokenizer) {
    tokenizer->has_ready_token = true;
    if (tokenizer->token_kind == SXML_HTML_TOKEN_KIND_START_TAG) {
        tokenizer->last_start_tag_name.byte_count = 0;
        return sxml_xml_buffer_append(
            &tokenizer->last_start_tag_name,
            sxml_xml_buffer_view(&tokenizer->token_text, tokenizer->name_range));
    }
    if (tokenizer->token_kind == SXML_HTML_TOKEN_KIND_END_TAG &&
        (tokenizer->attribute_records.element_count > 0 || tokenizer->is_self_closing)) {
        /* end-tag-with-attributes, end-tag-with-trailing-solidus. */
        report_parse_error(tokenizer);
    }
    return SXML_STATUS_OK;
}

/* Emits the end-of-file token after anything already ready. */
static void emit_end_of_file(sxml_html_tokenizer *tokenizer) {
    tokenizer->has_reached_end = true;
}

static sxml_status emit_character(sxml_html_tokenizer *tokenizer, uint32_t character) {
    return sxml_xml_buffer_append_codepoint(&tokenizer->characters, character);
}

static sxml_status emit_bytes(sxml_html_tokenizer *tokenizer, sxml_byte_view bytes) {
    return sxml_xml_buffer_append(&tokenizer->characters, bytes);
}

/* "An appropriate end tag token" (13.2.5). */
static bool is_appropriate_end_tag(const sxml_html_tokenizer *tokenizer) {
    return tokenizer->last_start_tag_name.byte_count > 0 &&
           sxml_byte_view_equals(
               sxml_xml_buffer_view(&tokenizer->token_text, tokenizer->name_range),
               (sxml_byte_view){.bytes = tokenizer->last_start_tag_name.bytes,
                                .byte_count = tokenizer->last_start_tag_name.byte_count});
}

static sxml_byte_view temporary_buffer_view(const sxml_html_tokenizer *tokenizer) {
    return (sxml_byte_view){.bytes = tokenizer->temporary_buffer.bytes,
                            .byte_count = tokenizer->temporary_buffer.byte_count};
}

/* ------------------------------------------------- Character references */

/* The return state is one of the attribute value states (13.2.5). */
static bool is_consumed_in_attribute(const sxml_html_tokenizer *tokenizer) {
    return tokenizer->return_state == SXML_HTML_STATE_ATTRIBUTE_VALUE_DOUBLE_QUOTED ||
           tokenizer->return_state == SXML_HTML_STATE_ATTRIBUTE_VALUE_SINGLE_QUOTED ||
           tokenizer->return_state == SXML_HTML_STATE_ATTRIBUTE_VALUE_UNQUOTED;
}

/* "Flush code points consumed as a character reference" (13.2.5). */
static sxml_status flush_character_reference(sxml_html_tokenizer *tokenizer) {
    const sxml_byte_view consumed = temporary_buffer_view(tokenizer);
    if (is_consumed_in_attribute(tokenizer)) {
        return append_bytes_to_field(tokenizer, &current_attribute(tokenizer)->attribute_value,
                                     consumed);
    }
    return emit_bytes(tokenizer, consumed);
}

/* The named character references that also match without their
   semicolon: the names of entities.json without ";" (13.5), sorted by
   their bytes. */
static const sxml_byte_view sxml_html_legacy_reference_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("AElig"),  SXML_BYTE_VIEW_INITIALIZER("AMP"),
    SXML_BYTE_VIEW_INITIALIZER("Aacute"), SXML_BYTE_VIEW_INITIALIZER("Acirc"),
    SXML_BYTE_VIEW_INITIALIZER("Agrave"), SXML_BYTE_VIEW_INITIALIZER("Aring"),
    SXML_BYTE_VIEW_INITIALIZER("Atilde"), SXML_BYTE_VIEW_INITIALIZER("Auml"),
    SXML_BYTE_VIEW_INITIALIZER("COPY"),   SXML_BYTE_VIEW_INITIALIZER("Ccedil"),
    SXML_BYTE_VIEW_INITIALIZER("ETH"),    SXML_BYTE_VIEW_INITIALIZER("Eacute"),
    SXML_BYTE_VIEW_INITIALIZER("Ecirc"),  SXML_BYTE_VIEW_INITIALIZER("Egrave"),
    SXML_BYTE_VIEW_INITIALIZER("Euml"),   SXML_BYTE_VIEW_INITIALIZER("GT"),
    SXML_BYTE_VIEW_INITIALIZER("Iacute"), SXML_BYTE_VIEW_INITIALIZER("Icirc"),
    SXML_BYTE_VIEW_INITIALIZER("Igrave"), SXML_BYTE_VIEW_INITIALIZER("Iuml"),
    SXML_BYTE_VIEW_INITIALIZER("LT"),     SXML_BYTE_VIEW_INITIALIZER("Ntilde"),
    SXML_BYTE_VIEW_INITIALIZER("Oacute"), SXML_BYTE_VIEW_INITIALIZER("Ocirc"),
    SXML_BYTE_VIEW_INITIALIZER("Ograve"), SXML_BYTE_VIEW_INITIALIZER("Oslash"),
    SXML_BYTE_VIEW_INITIALIZER("Otilde"), SXML_BYTE_VIEW_INITIALIZER("Ouml"),
    SXML_BYTE_VIEW_INITIALIZER("QUOT"),   SXML_BYTE_VIEW_INITIALIZER("REG"),
    SXML_BYTE_VIEW_INITIALIZER("THORN"),  SXML_BYTE_VIEW_INITIALIZER("Uacute"),
    SXML_BYTE_VIEW_INITIALIZER("Ucirc"),  SXML_BYTE_VIEW_INITIALIZER("Ugrave"),
    SXML_BYTE_VIEW_INITIALIZER("Uuml"),   SXML_BYTE_VIEW_INITIALIZER("Yacute"),
    SXML_BYTE_VIEW_INITIALIZER("aacute"), SXML_BYTE_VIEW_INITIALIZER("acirc"),
    SXML_BYTE_VIEW_INITIALIZER("acute"),  SXML_BYTE_VIEW_INITIALIZER("aelig"),
    SXML_BYTE_VIEW_INITIALIZER("agrave"), SXML_BYTE_VIEW_INITIALIZER("amp"),
    SXML_BYTE_VIEW_INITIALIZER("aring"),  SXML_BYTE_VIEW_INITIALIZER("atilde"),
    SXML_BYTE_VIEW_INITIALIZER("auml"),   SXML_BYTE_VIEW_INITIALIZER("brvbar"),
    SXML_BYTE_VIEW_INITIALIZER("ccedil"), SXML_BYTE_VIEW_INITIALIZER("cedil"),
    SXML_BYTE_VIEW_INITIALIZER("cent"),   SXML_BYTE_VIEW_INITIALIZER("copy"),
    SXML_BYTE_VIEW_INITIALIZER("curren"), SXML_BYTE_VIEW_INITIALIZER("deg"),
    SXML_BYTE_VIEW_INITIALIZER("divide"), SXML_BYTE_VIEW_INITIALIZER("eacute"),
    SXML_BYTE_VIEW_INITIALIZER("ecirc"),  SXML_BYTE_VIEW_INITIALIZER("egrave"),
    SXML_BYTE_VIEW_INITIALIZER("eth"),    SXML_BYTE_VIEW_INITIALIZER("euml"),
    SXML_BYTE_VIEW_INITIALIZER("frac12"), SXML_BYTE_VIEW_INITIALIZER("frac14"),
    SXML_BYTE_VIEW_INITIALIZER("frac34"), SXML_BYTE_VIEW_INITIALIZER("gt"),
    SXML_BYTE_VIEW_INITIALIZER("iacute"), SXML_BYTE_VIEW_INITIALIZER("icirc"),
    SXML_BYTE_VIEW_INITIALIZER("iexcl"),  SXML_BYTE_VIEW_INITIALIZER("igrave"),
    SXML_BYTE_VIEW_INITIALIZER("iquest"), SXML_BYTE_VIEW_INITIALIZER("iuml"),
    SXML_BYTE_VIEW_INITIALIZER("laquo"),  SXML_BYTE_VIEW_INITIALIZER("lt"),
    SXML_BYTE_VIEW_INITIALIZER("macr"),   SXML_BYTE_VIEW_INITIALIZER("micro"),
    SXML_BYTE_VIEW_INITIALIZER("middot"), SXML_BYTE_VIEW_INITIALIZER("nbsp"),
    SXML_BYTE_VIEW_INITIALIZER("not"),    SXML_BYTE_VIEW_INITIALIZER("ntilde"),
    SXML_BYTE_VIEW_INITIALIZER("oacute"), SXML_BYTE_VIEW_INITIALIZER("ocirc"),
    SXML_BYTE_VIEW_INITIALIZER("ograve"), SXML_BYTE_VIEW_INITIALIZER("ordf"),
    SXML_BYTE_VIEW_INITIALIZER("ordm"),   SXML_BYTE_VIEW_INITIALIZER("oslash"),
    SXML_BYTE_VIEW_INITIALIZER("otilde"), SXML_BYTE_VIEW_INITIALIZER("ouml"),
    SXML_BYTE_VIEW_INITIALIZER("para"),   SXML_BYTE_VIEW_INITIALIZER("plusmn"),
    SXML_BYTE_VIEW_INITIALIZER("pound"),  SXML_BYTE_VIEW_INITIALIZER("quot"),
    SXML_BYTE_VIEW_INITIALIZER("raquo"),  SXML_BYTE_VIEW_INITIALIZER("reg"),
    SXML_BYTE_VIEW_INITIALIZER("sect"),   SXML_BYTE_VIEW_INITIALIZER("shy"),
    SXML_BYTE_VIEW_INITIALIZER("sup1"),   SXML_BYTE_VIEW_INITIALIZER("sup2"),
    SXML_BYTE_VIEW_INITIALIZER("sup3"),   SXML_BYTE_VIEW_INITIALIZER("szlig"),
    SXML_BYTE_VIEW_INITIALIZER("thorn"),  SXML_BYTE_VIEW_INITIALIZER("times"),
    SXML_BYTE_VIEW_INITIALIZER("uacute"), SXML_BYTE_VIEW_INITIALIZER("ucirc"),
    SXML_BYTE_VIEW_INITIALIZER("ugrave"), SXML_BYTE_VIEW_INITIALIZER("uml"),
    SXML_BYTE_VIEW_INITIALIZER("uuml"),   SXML_BYTE_VIEW_INITIALIZER("yacute"),
    SXML_BYTE_VIEW_INITIALIZER("yen"),    SXML_BYTE_VIEW_INITIALIZER("yuml"),
};

/* Byte order of two names, as the table is sorted. */
static int compare_names(sxml_byte_view first, sxml_byte_view second) {
    const size_t common = first.byte_count < second.byte_count ? first.byte_count
                                                               : second.byte_count;
    const int order = common == 0 ? 0 : memcmp(first.bytes, second.bytes, common);
    if (order != 0) {
        return order;
    }
    return first.byte_count < second.byte_count   ? -1
           : first.byte_count > second.byte_count ? 1
                                                  : 0;
}

static bool is_legacy_reference_name(sxml_byte_view name) {
    size_t low_index = 0;
    size_t high_index =
        sizeof(sxml_html_legacy_reference_names) / sizeof(sxml_html_legacy_reference_names[0]);
    while (low_index < high_index) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        const int order = compare_names(sxml_html_legacy_reference_names[middle_index], name);
        if (order == 0) {
            return true;
        }
        if (order < 0) {
            low_index = middle_index + 1;
        } else {
            high_index = middle_index;
        }
    }
    return false;
}

/* The longest name in the table (13.5) has 31 characters before ";". */
#define SXML_HTML_LONGEST_REFERENCE_NAME ((size_t)32)

/* What the named character reference state matched. */
typedef struct sxml_html_reference_match {
    bool is_found;
    /* Characters matched, the semicolon included when it was. */
    size_t matched_byte_count;
    bool ends_with_semicolon;
    sxml_unicode_character_reference_value value;
} sxml_html_reference_match;

/* 13.2.5.78: the longest identifier of the table that the input starts
   with, the next input character being its first. */
static sxml_html_reference_match match_reference_name(const sxml_html_tokenizer *tokenizer) {
    sxml_html_reference_match match = {.is_found = false,
                                       .matched_byte_count = 0,
                                       .ends_with_semicolon = false,
                                       .value = {.codepoints = {0, 0}, .codepoint_count = 0}};
    const uint8_t *bytes = tokenizer->input.bytes + tokenizer->input_offset;
    const size_t rest = tokenizer->input.byte_count - tokenizer->input_offset;
    size_t run_count = 0;
    while (run_count < rest && run_count < SXML_HTML_LONGEST_REFERENCE_NAME &&
           is_ascii_alphanumeric(bytes[run_count])) {
        run_count += 1;
    }
    if (run_count < rest && bytes[run_count] == ';') {
        const sxml_byte_view name = {.bytes = bytes, .byte_count = run_count};
        if (run_count > 0 && sxml_unicode_find_character_reference(name, &match.value)) {
            match.is_found = true;
            match.matched_byte_count = run_count + 1;
            match.ends_with_semicolon = true;
            return match;
        }
    }
    for (size_t name_count = run_count; name_count > 0; name_count -= 1) {
        const sxml_byte_view name = {.bytes = bytes, .byte_count = name_count};
        if (is_legacy_reference_name(name) &&
            sxml_unicode_find_character_reference(name, &match.value)) {
            match.is_found = true;
            match.matched_byte_count = name_count;
            return match;
        }
    }
    return match;
}

static sxml_status run_named_character_reference(sxml_html_tokenizer *tokenizer) {
    const sxml_html_reference_match match = match_reference_name(tokenizer);
    if (!match.is_found) {
        switch_to(tokenizer, SXML_HTML_STATE_AMBIGUOUS_AMPERSAND);
        return flush_character_reference(tokenizer);
    }
    const sxml_byte_view matched = {.bytes = tokenizer->input.bytes + tokenizer->input_offset,
                                    .byte_count = match.matched_byte_count};
    tokenizer->input_offset += match.matched_byte_count;
    tokenizer->current_byte_count = 0;
    sxml_status status = sxml_xml_buffer_append(&tokenizer->temporary_buffer, matched);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    tokenizer->state = tokenizer->return_state;
    if (is_consumed_in_attribute(tokenizer) && !match.ends_with_semicolon &&
        tokenizer->input_offset < tokenizer->input.byte_count) {
        const uint8_t next = tokenizer->input.bytes[tokenizer->input_offset];
        if (next == '=' || is_ascii_alphanumeric(next)) {
            /* "For historical reasons" the text stays as it is. */
            return flush_character_reference(tokenizer);
        }
    }
    if (!match.ends_with_semicolon) {
        report_parse_error(tokenizer);
    }
    tokenizer->temporary_buffer.byte_count = 0;
    for (size_t codepoint_index = 0; codepoint_index < match.value.codepoint_count;
         codepoint_index += 1) {
        status = sxml_xml_buffer_append_codepoint(&tokenizer->temporary_buffer,
                                                  match.value.codepoints[codepoint_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return flush_character_reference(tokenizer);
}

/* 13.2.5.84: the replacements for references to C1 controls. */
static uint32_t replace_c1_control(uint32_t code) {
    static const uint32_t replacements[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    };
    return code >= 0x80 && code <= 0x9F ? replacements[code - 0x80] : code;
}

static bool is_noncharacter(uint32_t code) {
    return (code >= 0xFDD0 && code <= 0xFDEF) || (code & 0xFFFEU) == 0xFFFEU;
}

static bool is_control(uint32_t code) {
    return code <= 0x1F || (code >= 0x7F && code <= 0x9F);
}

static sxml_status end_numeric_character_reference(sxml_html_tokenizer *tokenizer) {
    uint32_t code = tokenizer->character_reference_code;
    if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        /* null-, outside-unicode-range-, surrogate-character-reference. */
        report_parse_error(tokenizer);
        code = 0xFFFD;
    } else if (is_noncharacter(code)) {
        report_parse_error(tokenizer);
    } else if (code == 0x0D || (is_control(code) && !is_tokenizer_whitespace(code))) {
        report_parse_error(tokenizer);
        code = replace_c1_control(code);
    }
    tokenizer->temporary_buffer.byte_count = 0;
    const sxml_status status = sxml_xml_buffer_append_codepoint(&tokenizer->temporary_buffer, code);
    tokenizer->state = tokenizer->return_state;
    return status == SXML_STATUS_OK ? flush_character_reference(tokenizer) : status;
}

/* Adds a digit to the reference code, which stays above 0x10FFFF once it
   passes it, so it cannot overflow. */
static void add_reference_digit(sxml_html_tokenizer *tokenizer, uint32_t base, uint32_t digit) {
    if (tokenizer->character_reference_code <= 0x10FFFF) {
        tokenizer->character_reference_code = tokenizer->character_reference_code * base + digit;
    }
}

static uint32_t hex_digit_value(uint32_t character) {
    if (is_ascii_digit(character)) {
        return character - '0';
    }
    return to_ascii_lower(character) - 'a' + 10U;
}

/* ------------------------------------------------------------ States */

/* Characters of the data, RCDATA, RAWTEXT, script data and PLAINTEXT
   states (13.2.5.1 to 13.2.5.5). */
typedef struct sxml_html_text_state_rules {
    /* "&" starts a character reference. */
    bool has_character_references;
    /* What "<" switches to; the state itself when it is a character. */
    sxml_html_tokenizer_state less_than_sign_state;
    /* U+0000 is emitted as U+FFFD (else as itself). */
    bool replaces_null;
} sxml_html_text_state_rules;

static sxml_status run_text_state(sxml_html_tokenizer *tokenizer,
                                  const sxml_html_text_state_rules *rules) {
    const sxml_html_tokenizer_state own = (sxml_html_tokenizer_state)tokenizer->state;
    /* Plain characters run on without a state step each. */
    const uint8_t *bytes = tokenizer->input.bytes;
    size_t run_end = tokenizer->input_offset;
    while (run_end < tokenizer->input.byte_count) {
        const uint8_t byte = bytes[run_end];
        if (byte == 0 || (byte == '&' && rules->has_character_references) ||
            (byte == '<' && rules->less_than_sign_state != own)) {
            break;
        }
        run_end += 1;
    }
    if (run_end > tokenizer->input_offset) {
        const sxml_byte_view run = {.bytes = bytes + tokenizer->input_offset,
                                    .byte_count = run_end - tokenizer->input_offset};
        tokenizer->input_offset = run_end;
        tokenizer->current_byte_count = 0;
        return emit_bytes(tokenizer, run);
    }
    const uint32_t character = consume_character(tokenizer);
    if (character == SXML_HTML_END_OF_INPUT) {
        emit_end_of_file(tokenizer);
        return SXML_STATUS_OK;
    }
    if (character == '&') {
        tokenizer->return_state = (uint8_t)own;
        switch_to(tokenizer, SXML_HTML_STATE_CHARACTER_REFERENCE);
        return SXML_STATUS_OK;
    }
    if (character == '<') {
        switch_to(tokenizer, rules->less_than_sign_state);
        return SXML_STATUS_OK;
    }
    /* U+0000 NULL: unexpected-null-character. */
    report_parse_error(tokenizer);
    return emit_character(tokenizer, rules->replaces_null ? 0xFFFDU : 0U);
}

/* The end tag name states of RCDATA, RAWTEXT and script data, escaped or
   not (13.2.5.11, .14, .17, .25): fallback is the state to return to. */
static sxml_status run_end_tag_name_state(sxml_html_tokenizer *tokenizer,
                                          sxml_html_tokenizer_state fallback) {
    const uint32_t character = consume_character(tokenizer);
    if (is_tokenizer_whitespace(character) && is_appropriate_end_tag(tokenizer)) {
        switch_to(tokenizer, SXML_HTML_STATE_BEFORE_ATTRIBUTE_NAME);
        return SXML_STATUS_OK;
    }
    if (character == '/' && is_appropriate_end_tag(tokenizer)) {
        switch_to(tokenizer, SXML_HTML_STATE_SELF_CLOSING_START_TAG);
        return SXML_STATUS_OK;
    }
    if (character == '>' && is_appropriate_end_tag(tokenizer)) {
        switch_to(tokenizer, SXML_HTML_STATE_DATA);
        return emit_current_token(tokenizer);
    }
    if (is_ascii_alpha(character)) {
        sxml_status status = append_to_field(tokenizer, &tokenizer->name_range,
                                             to_ascii_lower(character));
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(&tokenizer->temporary_buffer, (uint8_t)character);
        }
        return status;
    }
    sxml_status status = emit_bytes(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("</"));
    if (status == SXML_STATUS_OK) {
        status = emit_bytes(tokenizer, temporary_buffer_view(tokenizer));
    }
    reconsume_in(tokenizer, fallback);
    return status;
}

/* The "end tag open" states of RCDATA, RAWTEXT and script data
   (13.2.5.10, .13, .16, .24). */
static sxml_status run_end_tag_open_state(sxml_html_tokenizer *tokenizer,
                                          sxml_html_tokenizer_state name_state,
                                          sxml_html_tokenizer_state fallback) {
    const uint32_t character = consume_character(tokenizer);
    if (is_ascii_alpha(character)) {
        begin_token(tokenizer, SXML_HTML_TOKEN_KIND_END_TAG);
        tokenizer->name_range = start_field(tokenizer);
        reconsume_in(tokenizer, name_state);
        return SXML_STATUS_OK;
    }
    reconsume_in(tokenizer, fallback);
    return emit_bytes(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("</"));
}

/* The "less-than sign" states of RCDATA and RAWTEXT (13.2.5.9, .12). */
static sxml_status run_less_than_sign_state(sxml_html_tokenizer *tokenizer,
                                            sxml_html_tokenizer_state end_tag_open_state,
                                            sxml_html_tokenizer_state fallback) {
    const uint32_t character = consume_character(tokenizer);
    if (character == '/') {
        tokenizer->temporary_buffer.byte_count = 0;
        switch_to(tokenizer, end_tag_open_state);
        return SXML_STATUS_OK;
    }
    reconsume_in(tokenizer, fallback);
    return emit_character(tokenizer, '<');
}

/* The double escape start and end states (13.2.5.26, .31): match is the
   state for a temporary buffer of "script", other for anything else. */
static sxml_status run_double_escape_state(sxml_html_tokenizer *tokenizer,
                                           sxml_html_tokenizer_state match,
                                           sxml_html_tokenizer_state other) {
    const uint32_t character = consume_character(tokenizer);
    if (is_tokenizer_whitespace(character) || character == '/' || character == '>') {
        switch_to(tokenizer, sxml_byte_view_equals(temporary_buffer_view(tokenizer),
                                                   SXML_BYTE_VIEW_FROM_LITERAL("script"))
                                 ? match
                                 : other);
        return emit_character(tokenizer, character);
    }
    if (is_ascii_alpha(character)) {
        const sxml_status status = sxml_xml_buffer_append_byte(
            &tokenizer->temporary_buffer, (uint8_t)to_ascii_lower(character));
        return status == SXML_STATUS_OK ? emit_character(tokenizer, character) : status;
    }
    /* Anything else is reconsumed in the state the buffer did not match
       for: escaped after a start, double escaped after an end. */
    reconsume_in(tokenizer, other);
    return SXML_STATUS_OK;
}

/* The escaped and double escaped script data states and their dash
   states (13.2.5.20 to .22, .27 to .29). */
typedef struct sxml_html_escaped_rules {
    sxml_html_tokenizer_state escaped_state;
    sxml_html_tokenizer_state dash_state;
    sxml_html_tokenizer_state dash_dash_state;
    sxml_html_tokenizer_state less_than_sign_state;
    /* The double escaped states emit "<" when they switch on it. */
    bool emits_less_than_sign;
} sxml_html_escaped_rules;

static sxml_status run_escaped_state(sxml_html_tokenizer *tokenizer,
                                     const sxml_html_escaped_rules *rules) {
    const sxml_html_tokenizer_state own = (sxml_html_tokenizer_state)tokenizer->state;
    const uint32_t character = consume_character(tokenizer);
    if (character == '-') {
        switch_to(tokenizer, own == rules->escaped_state ? rules->dash_state
                                                         : rules->dash_dash_state);
        return emit_character(tokenizer, '-');
    }
    if (character == '<') {
        switch_to(tokenizer, rules->less_than_sign_state);
        return rules->emits_less_than_sign ? emit_character(tokenizer, '<') : SXML_STATUS_OK;
    }
    if (character == '>' && own == rules->dash_dash_state) {
        switch_to(tokenizer, SXML_HTML_STATE_SCRIPT_DATA);
        return emit_character(tokenizer, '>');
    }
    if (character == SXML_HTML_END_OF_INPUT) {
        /* eof-in-script-html-comment-like-text. */
        report_parse_error(tokenizer);
        emit_end_of_file(tokenizer);
        return SXML_STATUS_OK;
    }
    switch_to(tokenizer, rules->escaped_state);
    if (character == 0) {
        report_parse_error(tokenizer);
        return emit_character(tokenizer, 0xFFFD);
    }
    return emit_character(tokenizer, character);
}

/* Appends to the current attribute's value in a quoted value state
   (13.2.5.36, .37). */
static sxml_status run_quoted_value_state(sxml_html_tokenizer *tokenizer, uint32_t quote) {
    const uint8_t *bytes = tokenizer->input.bytes;
    size_t run_end = tokenizer->input_offset;
    while (run_end < tokenizer->input.byte_count && bytes[run_end] != quote &&
           bytes[run_end] != '&' && bytes[run_end] != 0) {
        run_end += 1;
    }
    if (run_end > tokenizer->input_offset) {
        const sxml_byte_view run = {.bytes = bytes + tokenizer->input_offset,
                                    .byte_count = run_end - tokenizer->input_offset};
        tokenizer->input_offset = run_end;
        tokenizer->current_byte_count = 0;
        return append_bytes_to_field(tokenizer, &current_attribute(tokenizer)->attribute_value,
                                     run);
    }
    const uint32_t character = consume_character(tokenizer);
    if (character == quote) {
        switch_to(tokenizer, SXML_HTML_STATE_AFTER_ATTRIBUTE_VALUE_QUOTED);
        return SXML_STATUS_OK;
    }
    if (character == '&') {
        tokenizer->return_state = tokenizer->state;
        switch_to(tokenizer, SXML_HTML_STATE_CHARACTER_REFERENCE);
        return SXML_STATUS_OK;
    }
    if (character == SXML_HTML_END_OF_INPUT) {
        /* eof-in-tag. */
        report_parse_error(tokenizer);
        emit_end_of_file(tokenizer);
        return SXML_STATUS_OK;
    }
    report_parse_error(tokenizer);
    return append_to_attribute_value(tokenizer, 0xFFFD);
}

/* The DOCTYPE identifier states, quoted (13.2.5.59, .60, .65, .66). */
static sxml_status run_doctype_identifier_state(sxml_html_tokenizer *tokenizer, uint32_t quote) {
    const bool is_public = tokenizer->state == SXML_HTML_STATE_DOCTYPE_PUBLIC_IDENTIFIER_DOUBLE_QUOTED ||
                           tokenizer->state == SXML_HTML_STATE_DOCTYPE_PUBLIC_IDENTIFIER_SINGLE_QUOTED;
    sxml_byte_range *field = is_public ? &tokenizer->public_identifier_range
                                       : &tokenizer->system_identifier_range;
    const uint32_t character = consume_character(tokenizer);
    if (character == quote) {
        switch_to(tokenizer, is_public ? SXML_HTML_STATE_AFTER_DOCTYPE_PUBLIC_IDENTIFIER
                                       : SXML_HTML_STATE_AFTER_DOCTYPE_SYSTEM_IDENTIFIER);
        return SXML_STATUS_OK;
    }
    if (character == 0) {
        report_parse_error(tokenizer);
        return append_to_field(tokenizer, field, 0xFFFD);
    }
    if (character == '>') {
        /* abrupt-doctype-public-identifier, abrupt-doctype-system-identifier. */
        report_parse_error(tokenizer);
        tokenizer->forces_quirks = true;
        switch_to(tokenizer, SXML_HTML_STATE_DATA);
        return emit_current_token(tokenizer);
    }
    if (character == SXML_HTML_END_OF_INPUT) {
        report_parse_error(tokenizer);
        tokenizer->forces_quirks = true;
        emit_end_of_file(tokenizer);
        return emit_current_token(tokenizer);
    }
    return append_to_field(tokenizer, field, character);
}

/* A DOCTYPE identifier to start: public or system, and its quote. */
typedef struct sxml_html_identifier_start {
    bool is_public;
    uint32_t quote;
} sxml_html_identifier_start;

static void begin_doctype_identifier(sxml_html_tokenizer *tokenizer,
                                     sxml_html_identifier_start start) {
    if (start.is_public) {
        tokenizer->has_public_identifier = true;
        tokenizer->public_identifier_range = start_field(tokenizer);
        switch_to(tokenizer, start.quote == '"' ? SXML_HTML_STATE_DOCTYPE_PUBLIC_IDENTIFIER_DOUBLE_QUOTED
                                                : SXML_HTML_STATE_DOCTYPE_PUBLIC_IDENTIFIER_SINGLE_QUOTED);
    } else {
        tokenizer->has_system_identifier = true;
        tokenizer->system_identifier_range = start_field(tokenizer);
        switch_to(tokenizer, start.quote == '"' ? SXML_HTML_STATE_DOCTYPE_SYSTEM_IDENTIFIER_DOUBLE_QUOTED
                                                : SXML_HTML_STATE_DOCTYPE_SYSTEM_IDENTIFIER_SINGLE_QUOTED);
    }
}

/* The DOCTYPE states between keyword and identifier (13.2.5.57, .58,
   .61 to .64): what follows, with or without the whitespace a state
   requires. */
typedef struct sxml_html_doctype_gap_rules {
    /* The identifier a quote starts. */
    bool is_public;
    /* Whitespace switches to this state (or is ignored when it is the
       state itself). */
    sxml_html_tokenizer_state whitespace_state;
    /* A quote here is a parse error (missing whitespace). */
    bool quote_is_error;
    /* ">" ends the DOCTYPE without an error (after the public
       identifier). */
    bool closes_cleanly;
} sxml_html_doctype_gap_rules;

static sxml_status run_doctype_gap_state(sxml_html_tokenizer *tokenizer,
                                         const sxml_html_doctype_gap_rules *rules) {
    const uint32_t character = consume_character(tokenizer);
    if (is_tokenizer_whitespace(character)) {
        switch_to(tokenizer, rules->whitespace_state);
        return SXML_STATUS_OK;
    }
    if (character == '"' || character == '\'') {
        if (rules->quote_is_error) {
            report_parse_error(tokenizer);
        }
        begin_doctype_identifier(
            tokenizer, (sxml_html_identifier_start){.is_public = rules->is_public,
                                                    .quote = character});
        return SXML_STATUS_OK;
    }
    if (character == '>') {
        if (!rules->closes_cleanly) {
            /* missing-doctype-public-identifier, -system-identifier. */
            report_parse_error(tokenizer);
            tokenizer->forces_quirks = true;
        }
        switch_to(tokenizer, SXML_HTML_STATE_DATA);
        return emit_current_token(tokenizer);
    }
    if (character == SXML_HTML_END_OF_INPUT) {
        report_parse_error(tokenizer);
        tokenizer->forces_quirks = true;
        emit_end_of_file(tokenizer);
        return emit_current_token(tokenizer);
    }
    /* missing-quote-before-doctype-public-identifier, -system-identifier. */
    report_parse_error(tokenizer);
    tokenizer->forces_quirks = true;
    reconsume_in(tokenizer, SXML_HTML_STATE_BOGUS_DOCTYPE);
    return SXML_STATUS_OK;
}

/* The comment a processing instruction with an invalid target becomes:
   "?" and the temporary buffer (13.2.5, "convert the temporary buffer to
   a comment"). */
static sxml_status convert_temporary_buffer_to_comment(sxml_html_tokenizer *tokenizer) {
    begin_token(tokenizer, SXML_HTML_TOKEN_KIND_COMMENT);
    tokenizer->data_range = start_field(tokenizer);
    sxml_status status = append_to_field(tokenizer, &tokenizer->data_range, '?');
    if (status == SXML_STATUS_OK) {
        status = append_bytes_to_field(tokenizer, &tokenizer->data_range,
                                       temporary_buffer_view(tokenizer));
    }
    return status;
}

static sxml_status begin_comment(sxml_html_tokenizer *tokenizer) {
    begin_token(tokenizer, SXML_HTML_TOKEN_KIND_COMMENT);
    tokenizer->data_range = start_field(tokenizer);
    return SXML_STATUS_OK;
}

static sxml_status append_to_comment(sxml_html_tokenizer *tokenizer, sxml_byte_view text) {
    return append_bytes_to_field(tokenizer, &tokenizer->data_range, text);
}

/* Emits the current token and then the end-of-file token, with the
   parse error the states give for an unexpected end. */
static sxml_status emit_token_at_end(sxml_html_tokenizer *tokenizer) {
    report_parse_error(tokenizer);
    emit_end_of_file(tokenizer);
    return emit_current_token(tokenizer);
}

static sxml_status emit_end_in_doctype(sxml_html_tokenizer *tokenizer) {
    tokenizer->forces_quirks = true;
    return emit_token_at_end(tokenizer);
}

/* An end of input inside a tag: the tag is dropped (eof-in-tag). */
static sxml_status drop_token_at_end(sxml_html_tokenizer *tokenizer) {
    report_parse_error(tokenizer);
    emit_end_of_file(tokenizer);
    return SXML_STATUS_OK;
}

static sxml_status run_data_like_state(sxml_html_tokenizer *tokenizer) {
    static const sxml_html_text_state_rules data_rules = {
        .has_character_references = true,
        .less_than_sign_state = SXML_HTML_STATE_TAG_OPEN,
        .replaces_null = false};
    static const sxml_html_text_state_rules rcdata_rules = {
        .has_character_references = true,
        .less_than_sign_state = SXML_HTML_STATE_RCDATA_LESS_THAN_SIGN,
        .replaces_null = true};
    static const sxml_html_text_state_rules rawtext_rules = {
        .has_character_references = false,
        .less_than_sign_state = SXML_HTML_STATE_RAWTEXT_LESS_THAN_SIGN,
        .replaces_null = true};
    static const sxml_html_text_state_rules script_rules = {
        .has_character_references = false,
        .less_than_sign_state = SXML_HTML_STATE_SCRIPT_DATA_LESS_THAN_SIGN,
        .replaces_null = true};
    static const sxml_html_text_state_rules plaintext_rules = {
        .has_character_references = false,
        .less_than_sign_state = SXML_HTML_STATE_PLAINTEXT,
        .replaces_null = true};
    switch (tokenizer->state) {
    case SXML_HTML_STATE_RCDATA:
        return run_text_state(tokenizer, &rcdata_rules);
    case SXML_HTML_STATE_RAWTEXT:
        return run_text_state(tokenizer, &rawtext_rules);
    case SXML_HTML_STATE_SCRIPT_DATA:
        return run_text_state(tokenizer, &script_rules);
    case SXML_HTML_STATE_PLAINTEXT:
        return run_text_state(tokenizer, &plaintext_rules);
    default:
        return run_text_state(tokenizer, &data_rules);
    }
}

/* 13.2.5.6 to 13.2.5.8. */
static sxml_status run_tag_states(sxml_html_tokenizer *tokenizer) {
    const uint32_t character = consume_character(tokenizer);
    if (tokenizer->state == SXML_HTML_STATE_TAG_OPEN) {
        if (character == '!') {
            switch_to(tokenizer, SXML_HTML_STATE_MARKUP_DECLARATION_OPEN);
        } else if (character == '/') {
            switch_to(tokenizer, SXML_HTML_STATE_END_TAG_OPEN);
        } else if (is_ascii_alpha(character)) {
            begin_token(tokenizer, SXML_HTML_TOKEN_KIND_START_TAG);
            tokenizer->name_range = start_field(tokenizer);
            reconsume_in(tokenizer, SXML_HTML_STATE_TAG_NAME);
        } else if (character == '?') {
            tokenizer->temporary_buffer.byte_count = 0;
            switch_to(tokenizer, SXML_HTML_STATE_PROCESSING_INSTRUCTION_OPEN);
        } else if (character == SXML_HTML_END_OF_INPUT) {
            /* eof-before-tag-name. */
            report_parse_error(tokenizer);
            emit_end_of_file(tokenizer);
            return emit_character(tokenizer, '<');
        } else {
            /* invalid-first-character-of-tag-name. */
            report_parse_error(tokenizer);
            reconsume_in(tokenizer, SXML_HTML_STATE_DATA);
            return emit_character(tokenizer, '<');
        }
        return SXML_STATUS_OK;
    }
    if (tokenizer->state == SXML_HTML_STATE_END_TAG_OPEN) {
        if (is_ascii_alpha(character)) {
            begin_token(tokenizer, SXML_HTML_TOKEN_KIND_END_TAG);
            tokenizer->name_range = start_field(tokenizer);
            reconsume_in(tokenizer, SXML_HTML_STATE_TAG_NAME);
        } else if (character == '>') {
            /* missing-end-tag-name. */
            report_parse_error(tokenizer);
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
        } else if (character == SXML_HTML_END_OF_INPUT) {
            report_parse_error(tokenizer);
            emit_end_of_file(tokenizer);
            return emit_bytes(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("</"));
        } else {
            report_parse_error(tokenizer);
            reconsume_in(tokenizer, SXML_HTML_STATE_BOGUS_COMMENT);
            return begin_comment(tokenizer);
        }
        return SXML_STATUS_OK;
    }
    /* The tag name state. */
    if (is_tokenizer_whitespace(character)) {
        switch_to(tokenizer, SXML_HTML_STATE_BEFORE_ATTRIBUTE_NAME);
        return SXML_STATUS_OK;
    }
    if (character == '/') {
        switch_to(tokenizer, SXML_HTML_STATE_SELF_CLOSING_START_TAG);
        return SXML_STATUS_OK;
    }
    if (character == '>') {
        switch_to(tokenizer, SXML_HTML_STATE_DATA);
        return emit_current_token(tokenizer);
    }
    if (character == 0) {
        report_parse_error(tokenizer);
        return append_to_field(tokenizer, &tokenizer->name_range, 0xFFFD);
    }
    if (character == SXML_HTML_END_OF_INPUT) {
        return drop_token_at_end(tokenizer);
    }
    return append_to_field(tokenizer, &tokenizer->name_range, to_ascii_lower(character));
}

/* 13.2.5.15 to 13.2.5.31. */
static sxml_status run_script_states(sxml_html_tokenizer *tokenizer) {
    static const sxml_html_escaped_rules escaped_rules = {
        .escaped_state = SXML_HTML_STATE_SCRIPT_DATA_ESCAPED,
        .dash_state = SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_DASH,
        .dash_dash_state = SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_DASH_DASH,
        .less_than_sign_state = SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_LESS_THAN_SIGN,
        .emits_less_than_sign = false};
    static const sxml_html_escaped_rules double_escaped_rules = {
        .escaped_state = SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED,
        .dash_state = SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_DASH,
        .dash_dash_state = SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_DASH_DASH,
        .less_than_sign_state = SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_LESS_THAN_SIGN,
        .emits_less_than_sign = true};
    switch (tokenizer->state) {
    case SXML_HTML_STATE_SCRIPT_DATA_END_TAG_OPEN:
        return run_end_tag_open_state(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_END_TAG_NAME,
                                      SXML_HTML_STATE_SCRIPT_DATA);
    case SXML_HTML_STATE_SCRIPT_DATA_END_TAG_NAME:
        return run_end_tag_name_state(tokenizer, SXML_HTML_STATE_SCRIPT_DATA);
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_DASH:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_DASH_DASH:
        return run_escaped_state(tokenizer, &escaped_rules);
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_END_TAG_OPEN:
        return run_end_tag_open_state(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_END_TAG_NAME,
                                      SXML_HTML_STATE_SCRIPT_DATA_ESCAPED);
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_END_TAG_NAME:
        return run_end_tag_name_state(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_ESCAPED);
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPE_START:
        return run_double_escape_state(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED,
                                       SXML_HTML_STATE_SCRIPT_DATA_ESCAPED);
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED:
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_DASH:
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_DASH_DASH:
        return run_escaped_state(tokenizer, &double_escaped_rules);
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPE_END:
        return run_double_escape_state(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_ESCAPED,
                                       SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED);
    default:
        break;
    }
    const uint32_t character = consume_character(tokenizer);
    switch (tokenizer->state) {
    case SXML_HTML_STATE_SCRIPT_DATA_LESS_THAN_SIGN:
        if (character == '/') {
            tokenizer->temporary_buffer.byte_count = 0;
            switch_to(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_END_TAG_OPEN);
            return SXML_STATUS_OK;
        }
        if (character == '!') {
            switch_to(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_ESCAPE_START);
            return emit_bytes(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("<!"));
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_SCRIPT_DATA);
        return emit_character(tokenizer, '<');
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPE_START:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPE_START_DASH:
        if (character == '-') {
            switch_to(tokenizer, tokenizer->state == SXML_HTML_STATE_SCRIPT_DATA_ESCAPE_START
                                     ? SXML_HTML_STATE_SCRIPT_DATA_ESCAPE_START_DASH
                                     : SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_DASH_DASH);
            return emit_character(tokenizer, '-');
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_SCRIPT_DATA);
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_LESS_THAN_SIGN:
        if (character == '/') {
            tokenizer->temporary_buffer.byte_count = 0;
            switch_to(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_END_TAG_OPEN);
            return SXML_STATUS_OK;
        }
        if (is_ascii_alpha(character)) {
            tokenizer->temporary_buffer.byte_count = 0;
            reconsume_in(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPE_START);
            return emit_character(tokenizer, '<');
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_ESCAPED);
        return emit_character(tokenizer, '<');
    default:
        /* The double escaped less-than sign state. */
        if (character == '/') {
            tokenizer->temporary_buffer.byte_count = 0;
            switch_to(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPE_END);
            return emit_character(tokenizer, '/');
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED);
        return SXML_STATUS_OK;
    }
}

/* 13.2.5.32 to 13.2.5.40. */
static sxml_status run_attribute_states(sxml_html_tokenizer *tokenizer) {
    if (tokenizer->state == SXML_HTML_STATE_ATTRIBUTE_VALUE_DOUBLE_QUOTED) {
        return run_quoted_value_state(tokenizer, '"');
    }
    if (tokenizer->state == SXML_HTML_STATE_ATTRIBUTE_VALUE_SINGLE_QUOTED) {
        return run_quoted_value_state(tokenizer, '\'');
    }
    const uint32_t character = consume_character(tokenizer);
    const bool is_end = character == SXML_HTML_END_OF_INPUT;
    switch (tokenizer->state) {
    case SXML_HTML_STATE_BEFORE_ATTRIBUTE_NAME:
        if (is_tokenizer_whitespace(character)) {
            return SXML_STATUS_OK;
        }
        if (character == '/' || character == '>' || is_end) {
            reconsume_in(tokenizer, SXML_HTML_STATE_AFTER_ATTRIBUTE_NAME);
            return SXML_STATUS_OK;
        }
        if (character == '=') {
            /* unexpected-equals-sign-before-attribute-name. */
            report_parse_error(tokenizer);
            switch_to(tokenizer, SXML_HTML_STATE_ATTRIBUTE_NAME);
            const sxml_status status = start_attribute(tokenizer);
            return status == SXML_STATUS_OK ? append_to_attribute_name(tokenizer, '=') : status;
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_ATTRIBUTE_NAME);
        return start_attribute(tokenizer);
    case SXML_HTML_STATE_ATTRIBUTE_NAME:
        if (is_tokenizer_whitespace(character) || character == '/' || character == '>' ||
            is_end) {
            check_duplicate_attribute(tokenizer);
            reconsume_in(tokenizer, SXML_HTML_STATE_AFTER_ATTRIBUTE_NAME);
            return SXML_STATUS_OK;
        }
        if (character == '=') {
            check_duplicate_attribute(tokenizer);
            switch_to(tokenizer, SXML_HTML_STATE_BEFORE_ATTRIBUTE_VALUE);
            return SXML_STATUS_OK;
        }
        if (character == 0) {
            report_parse_error(tokenizer);
            return append_to_attribute_name(tokenizer, 0xFFFD);
        }
        if (character == '"' || character == '\'' || character == '<') {
            /* unexpected-character-in-attribute-name. */
            report_parse_error(tokenizer);
        }
        return append_to_attribute_name(tokenizer, to_ascii_lower(character));
    case SXML_HTML_STATE_AFTER_ATTRIBUTE_NAME:
        if (is_tokenizer_whitespace(character)) {
            return SXML_STATUS_OK;
        }
        if (character == '/') {
            switch_to(tokenizer, SXML_HTML_STATE_SELF_CLOSING_START_TAG);
            return SXML_STATUS_OK;
        }
        if (character == '=') {
            switch_to(tokenizer, SXML_HTML_STATE_BEFORE_ATTRIBUTE_VALUE);
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            return drop_token_at_end(tokenizer);
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_ATTRIBUTE_NAME);
        return start_attribute(tokenizer);
    case SXML_HTML_STATE_BEFORE_ATTRIBUTE_VALUE:
        if (is_tokenizer_whitespace(character)) {
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            /* missing-attribute-value. */
            report_parse_error(tokenizer);
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        start_attribute_value(tokenizer);
        if (character == '"') {
            switch_to(tokenizer, SXML_HTML_STATE_ATTRIBUTE_VALUE_DOUBLE_QUOTED);
        } else if (character == '\'') {
            switch_to(tokenizer, SXML_HTML_STATE_ATTRIBUTE_VALUE_SINGLE_QUOTED);
        } else {
            reconsume_in(tokenizer, SXML_HTML_STATE_ATTRIBUTE_VALUE_UNQUOTED);
        }
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_ATTRIBUTE_VALUE_UNQUOTED:
        if (is_tokenizer_whitespace(character)) {
            switch_to(tokenizer, SXML_HTML_STATE_BEFORE_ATTRIBUTE_NAME);
            return SXML_STATUS_OK;
        }
        if (character == '&') {
            tokenizer->return_state = SXML_HTML_STATE_ATTRIBUTE_VALUE_UNQUOTED;
            switch_to(tokenizer, SXML_HTML_STATE_CHARACTER_REFERENCE);
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (character == 0) {
            report_parse_error(tokenizer);
            return append_to_attribute_value(tokenizer, 0xFFFD);
        }
        if (is_end) {
            return drop_token_at_end(tokenizer);
        }
        if (character == '"' || character == '\'' || character == '<' || character == '=' ||
            character == '`') {
            /* unexpected-character-in-unquoted-attribute-value. */
            report_parse_error(tokenizer);
        }
        return append_to_attribute_value(tokenizer, character);
    case SXML_HTML_STATE_AFTER_ATTRIBUTE_VALUE_QUOTED:
        if (is_tokenizer_whitespace(character)) {
            switch_to(tokenizer, SXML_HTML_STATE_BEFORE_ATTRIBUTE_NAME);
            return SXML_STATUS_OK;
        }
        if (character == '/') {
            switch_to(tokenizer, SXML_HTML_STATE_SELF_CLOSING_START_TAG);
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            return drop_token_at_end(tokenizer);
        }
        /* missing-whitespace-between-attributes. */
        report_parse_error(tokenizer);
        reconsume_in(tokenizer, SXML_HTML_STATE_BEFORE_ATTRIBUTE_NAME);
        return SXML_STATUS_OK;
    default:
        /* The self-closing start tag state. */
        if (character == '>') {
            tokenizer->is_self_closing = true;
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            return drop_token_at_end(tokenizer);
        }
        /* unexpected-solidus-in-tag. */
        report_parse_error(tokenizer);
        reconsume_in(tokenizer, SXML_HTML_STATE_BEFORE_ATTRIBUTE_NAME);
        return SXML_STATUS_OK;
    }
}

/* 13.2.5.42 "Markup declaration open state". */
static sxml_status run_markup_declaration_open(sxml_html_tokenizer *tokenizer) {
    tokenizer->current_byte_count = 0;
    if (next_characters_are(tokenizer, (sxml_html_lookahead){
                                           .word = SXML_BYTE_VIEW_FROM_LITERAL("--"),
                                           .ignores_case = false})) {
        tokenizer->input_offset += 2;
        switch_to(tokenizer, SXML_HTML_STATE_COMMENT_START);
        return begin_comment(tokenizer);
    }
    if (next_characters_are(tokenizer, (sxml_html_lookahead){
                                           .word = SXML_BYTE_VIEW_FROM_LITERAL("doctype"),
                                           .ignores_case = true})) {
        tokenizer->input_offset += 7;
        begin_token(tokenizer, SXML_HTML_TOKEN_KIND_DOCTYPE);
        switch_to(tokenizer, SXML_HTML_STATE_DOCTYPE);
        return SXML_STATUS_OK;
    }
    if (next_characters_are(tokenizer, (sxml_html_lookahead){
                                           .word = SXML_BYTE_VIEW_FROM_LITERAL("[CDATA["),
                                           .ignores_case = false})) {
        tokenizer->input_offset += 7;
        if (tokenizer->allows_cdata_sections) {
            switch_to(tokenizer, SXML_HTML_STATE_CDATA_SECTION);
            return SXML_STATUS_OK;
        }
        /* cdata-in-html-content. */
        report_parse_error(tokenizer);
        switch_to(tokenizer, SXML_HTML_STATE_BOGUS_COMMENT);
        const sxml_status status = begin_comment(tokenizer);
        return status == SXML_STATUS_OK
                   ? append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("[CDATA["))
                   : status;
    }
    /* incorrectly-opened-comment. */
    report_parse_error(tokenizer);
    switch_to(tokenizer, SXML_HTML_STATE_BOGUS_COMMENT);
    return begin_comment(tokenizer);
}

/* 13.2.5.41, 13.2.5.43 to 13.2.5.52. */
static sxml_status run_comment_states(sxml_html_tokenizer *tokenizer) {
    if (tokenizer->state == SXML_HTML_STATE_COMMENT || tokenizer->state == SXML_HTML_STATE_BOGUS_COMMENT) {
        const bool is_bogus = tokenizer->state == SXML_HTML_STATE_BOGUS_COMMENT;
        const uint8_t *bytes = tokenizer->input.bytes;
        size_t run_end = tokenizer->input_offset;
        while (run_end < tokenizer->input.byte_count && bytes[run_end] != 0 &&
               bytes[run_end] != '>' && (is_bogus || (bytes[run_end] != '<' &&
                                                      bytes[run_end] != '-'))) {
            run_end += 1;
        }
        if (!is_bogus && run_end < tokenizer->input.byte_count && bytes[run_end] == '>') {
            run_end += 1;
        }
        if (run_end > tokenizer->input_offset) {
            const sxml_byte_view run = {.bytes = bytes + tokenizer->input_offset,
                                        .byte_count = run_end - tokenizer->input_offset};
            tokenizer->input_offset = run_end;
            tokenizer->current_byte_count = 0;
            return append_to_comment(tokenizer, run);
        }
    }
    const uint32_t character = consume_character(tokenizer);
    const bool is_end = character == SXML_HTML_END_OF_INPUT;
    switch (tokenizer->state) {
    case SXML_HTML_STATE_BOGUS_COMMENT:
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            emit_end_of_file(tokenizer);
            return emit_current_token(tokenizer);
        }
        report_parse_error(tokenizer);
        return append_to_field(tokenizer, &tokenizer->data_range, 0xFFFD);
    case SXML_HTML_STATE_COMMENT_START:
    case SXML_HTML_STATE_COMMENT_START_DASH:
        if (character == '-') {
            switch_to(tokenizer, tokenizer->state == SXML_HTML_STATE_COMMENT_START ? SXML_HTML_STATE_COMMENT_START_DASH
                                                                         : SXML_HTML_STATE_COMMENT_END);
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            /* abrupt-closing-of-empty-comment. */
            report_parse_error(tokenizer);
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end && tokenizer->state == SXML_HTML_STATE_COMMENT_START_DASH) {
            return emit_token_at_end(tokenizer);
        }
        if (tokenizer->state == SXML_HTML_STATE_COMMENT_START_DASH) {
            const sxml_status status = append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("-"));
            reconsume_in(tokenizer, SXML_HTML_STATE_COMMENT);
            return status;
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_COMMENT);
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_COMMENT:
        if (character == '<') {
            switch_to(tokenizer, SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN);
            return append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("<"));
        }
        if (character == '-') {
            switch_to(tokenizer, SXML_HTML_STATE_COMMENT_END_DASH);
            return SXML_STATUS_OK;
        }
        if (is_end) {
            return emit_token_at_end(tokenizer);
        }
        /* U+0000 NULL; ">" was taken by the run above. */
        if (character == 0) {
            report_parse_error(tokenizer);
            return append_to_field(tokenizer, &tokenizer->data_range, 0xFFFD);
        }
        return append_to_field(tokenizer, &tokenizer->data_range, character);
    case SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN:
        if (character == '!') {
            switch_to(tokenizer, SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG);
            return append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("!"));
        }
        if (character == '<') {
            return append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("<"));
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_COMMENT);
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG:
        if (character == '-') {
            switch_to(tokenizer, SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG_DASH);
        } else {
            reconsume_in(tokenizer, SXML_HTML_STATE_COMMENT);
        }
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG_DASH:
        if (character == '-') {
            switch_to(tokenizer, SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG_DASH_DASH);
        } else {
            reconsume_in(tokenizer, SXML_HTML_STATE_COMMENT_END_DASH);
        }
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG_DASH_DASH:
        if (character != '>' && !is_end) {
            /* nested-comment. */
            report_parse_error(tokenizer);
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_COMMENT_END);
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_COMMENT_END_DASH:
        if (character == '-') {
            switch_to(tokenizer, SXML_HTML_STATE_COMMENT_END);
            return SXML_STATUS_OK;
        }
        if (is_end) {
            return emit_token_at_end(tokenizer);
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_COMMENT);
        return append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("-"));
    case SXML_HTML_STATE_COMMENT_END:
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (character == '!') {
            switch_to(tokenizer, SXML_HTML_STATE_COMMENT_END_BANG);
            return SXML_STATUS_OK;
        }
        if (character == '-') {
            return append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("-"));
        }
        if (is_end) {
            return emit_token_at_end(tokenizer);
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_COMMENT);
        return append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("--"));
    default:
        /* The comment end bang state. */
        if (character == '-') {
            switch_to(tokenizer, SXML_HTML_STATE_COMMENT_END_DASH);
            return append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("--!"));
        }
        if (character == '>') {
            /* incorrectly-closed-comment. */
            report_parse_error(tokenizer);
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            return emit_token_at_end(tokenizer);
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_COMMENT);
        return append_to_comment(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("--!"));
    }
}

/* 13.2.5.53 to 13.2.5.56, 13.2.5.67 and 13.2.5.68. */
static sxml_status run_doctype_name_states(sxml_html_tokenizer *tokenizer) {
    const uint32_t character = consume_character(tokenizer);
    const bool is_end = character == SXML_HTML_END_OF_INPUT;
    switch (tokenizer->state) {
    case SXML_HTML_STATE_DOCTYPE:
        if (is_tokenizer_whitespace(character)) {
            switch_to(tokenizer, SXML_HTML_STATE_BEFORE_DOCTYPE_NAME);
            return SXML_STATUS_OK;
        }
        if (is_end) {
            return emit_end_in_doctype(tokenizer);
        }
        if (character != '>') {
            /* missing-whitespace-before-doctype-name. */
            report_parse_error(tokenizer);
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_BEFORE_DOCTYPE_NAME);
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_BEFORE_DOCTYPE_NAME:
        if (is_tokenizer_whitespace(character)) {
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            /* missing-doctype-name. */
            report_parse_error(tokenizer);
            tokenizer->forces_quirks = true;
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            return emit_end_in_doctype(tokenizer);
        }
        tokenizer->has_name = true;
        tokenizer->name_range = start_field(tokenizer);
        switch_to(tokenizer, SXML_HTML_STATE_DOCTYPE_NAME);
        if (character == 0) {
            report_parse_error(tokenizer);
            return append_to_field(tokenizer, &tokenizer->name_range, 0xFFFD);
        }
        return append_to_field(tokenizer, &tokenizer->name_range, to_ascii_lower(character));
    case SXML_HTML_STATE_DOCTYPE_NAME:
        if (is_tokenizer_whitespace(character)) {
            switch_to(tokenizer, SXML_HTML_STATE_AFTER_DOCTYPE_NAME);
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            return emit_end_in_doctype(tokenizer);
        }
        if (character == 0) {
            report_parse_error(tokenizer);
            return append_to_field(tokenizer, &tokenizer->name_range, 0xFFFD);
        }
        return append_to_field(tokenizer, &tokenizer->name_range, to_ascii_lower(character));
    case SXML_HTML_STATE_AFTER_DOCTYPE_NAME:
        if (is_tokenizer_whitespace(character)) {
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            return emit_end_in_doctype(tokenizer);
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_AFTER_DOCTYPE_NAME);
        if (next_characters_are(tokenizer, (sxml_html_lookahead){
                                               .word = SXML_BYTE_VIEW_FROM_LITERAL("public"),
                                               .ignores_case = true})) {
            tokenizer->input_offset += 6;
            switch_to(tokenizer, SXML_HTML_STATE_AFTER_DOCTYPE_PUBLIC_KEYWORD);
            return SXML_STATUS_OK;
        }
        if (next_characters_are(tokenizer, (sxml_html_lookahead){
                                               .word = SXML_BYTE_VIEW_FROM_LITERAL("system"),
                                               .ignores_case = true})) {
            tokenizer->input_offset += 6;
            switch_to(tokenizer, SXML_HTML_STATE_AFTER_DOCTYPE_SYSTEM_KEYWORD);
            return SXML_STATUS_OK;
        }
        /* invalid-character-sequence-after-doctype-name. */
        report_parse_error(tokenizer);
        tokenizer->forces_quirks = true;
        switch_to(tokenizer, SXML_HTML_STATE_BOGUS_DOCTYPE);
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_AFTER_DOCTYPE_SYSTEM_IDENTIFIER:
        if (is_tokenizer_whitespace(character)) {
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            return emit_end_in_doctype(tokenizer);
        }
        /* unexpected-character-after-doctype-system-identifier. */
        report_parse_error(tokenizer);
        reconsume_in(tokenizer, SXML_HTML_STATE_BOGUS_DOCTYPE);
        return SXML_STATUS_OK;
    default:
        /* The bogus DOCTYPE state. */
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        if (is_end) {
            emit_end_of_file(tokenizer);
            return emit_current_token(tokenizer);
        }
        if (character == 0) {
            report_parse_error(tokenizer);
        }
        return SXML_STATUS_OK;
    }
}

/* 13.2.5.57, .58 and .61 to .66. */
static sxml_status run_doctype_identifier_states(sxml_html_tokenizer *tokenizer) {
    static const sxml_html_doctype_gap_rules after_public_keyword = {
        .is_public = true,
        .whitespace_state = SXML_HTML_STATE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER,
        .quote_is_error = true,
        .closes_cleanly = false};
    static const sxml_html_doctype_gap_rules before_public_identifier = {
        .is_public = true,
        .whitespace_state = SXML_HTML_STATE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER,
        .quote_is_error = false,
        .closes_cleanly = false};
    static const sxml_html_doctype_gap_rules after_public_identifier = {
        .is_public = false,
        .whitespace_state = SXML_HTML_STATE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS,
        .quote_is_error = true,
        .closes_cleanly = true};
    static const sxml_html_doctype_gap_rules between_identifiers = {
        .is_public = false,
        .whitespace_state = SXML_HTML_STATE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS,
        .quote_is_error = false,
        .closes_cleanly = true};
    static const sxml_html_doctype_gap_rules after_system_keyword = {
        .is_public = false,
        .whitespace_state = SXML_HTML_STATE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER,
        .quote_is_error = true,
        .closes_cleanly = false};
    static const sxml_html_doctype_gap_rules before_system_identifier = {
        .is_public = false,
        .whitespace_state = SXML_HTML_STATE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER,
        .quote_is_error = false,
        .closes_cleanly = false};
    switch (tokenizer->state) {
    case SXML_HTML_STATE_AFTER_DOCTYPE_PUBLIC_KEYWORD:
        return run_doctype_gap_state(tokenizer, &after_public_keyword);
    case SXML_HTML_STATE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER:
        return run_doctype_gap_state(tokenizer, &before_public_identifier);
    case SXML_HTML_STATE_AFTER_DOCTYPE_PUBLIC_IDENTIFIER:
        return run_doctype_gap_state(tokenizer, &after_public_identifier);
    case SXML_HTML_STATE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS:
        return run_doctype_gap_state(tokenizer, &between_identifiers);
    case SXML_HTML_STATE_AFTER_DOCTYPE_SYSTEM_KEYWORD:
        return run_doctype_gap_state(tokenizer, &after_system_keyword);
    case SXML_HTML_STATE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER:
        return run_doctype_gap_state(tokenizer, &before_system_identifier);
    case SXML_HTML_STATE_DOCTYPE_PUBLIC_IDENTIFIER_DOUBLE_QUOTED:
    case SXML_HTML_STATE_DOCTYPE_SYSTEM_IDENTIFIER_DOUBLE_QUOTED:
        return run_doctype_identifier_state(tokenizer, '"');
    default:
        return run_doctype_identifier_state(tokenizer, '\'');
    }
}

/* 13.2.5.69 to 13.2.5.71. */
static sxml_status run_cdata_states(sxml_html_tokenizer *tokenizer) {
    if (tokenizer->state == SXML_HTML_STATE_CDATA_SECTION) {
        const uint8_t *bytes = tokenizer->input.bytes;
        size_t run_end = tokenizer->input_offset;
        while (run_end < tokenizer->input.byte_count && bytes[run_end] != ']') {
            run_end += 1;
        }
        if (run_end > tokenizer->input_offset) {
            const sxml_byte_view run = {.bytes = bytes + tokenizer->input_offset,
                                        .byte_count = run_end - tokenizer->input_offset};
            tokenizer->input_offset = run_end;
            tokenizer->current_byte_count = 0;
            return emit_bytes(tokenizer, run);
        }
    }
    const uint32_t character = consume_character(tokenizer);
    switch (tokenizer->state) {
    case SXML_HTML_STATE_CDATA_SECTION:
        if (character == ']') {
            switch_to(tokenizer, SXML_HTML_STATE_CDATA_SECTION_BRACKET);
            return SXML_STATUS_OK;
        }
        /* The end: eof-in-cdata. */
        report_parse_error(tokenizer);
        emit_end_of_file(tokenizer);
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_CDATA_SECTION_BRACKET:
        if (character == ']') {
            switch_to(tokenizer, SXML_HTML_STATE_CDATA_SECTION_END);
            return SXML_STATUS_OK;
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_CDATA_SECTION);
        return emit_character(tokenizer, ']');
    default:
        if (character == ']') {
            return emit_character(tokenizer, ']');
        }
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return SXML_STATUS_OK;
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_CDATA_SECTION);
        return emit_bytes(tokenizer, SXML_BYTE_VIEW_FROM_LITERAL("]]"));
    }
}

static bool is_ascii_case_insensitive_match(sxml_byte_view text, sxml_byte_view word) {
    if (text.byte_count != word.byte_count) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (to_ascii_lower(text.bytes[byte_index]) != word.bytes[byte_index]) {
            return false;
        }
    }
    return true;
}

/* 13.2.5.72 to 13.2.5.76. */
static sxml_status run_processing_instruction_states(sxml_html_tokenizer *tokenizer) {
    const uint32_t character = consume_character(tokenizer);
    const bool is_end = character == SXML_HTML_END_OF_INPUT;
    if (is_end) {
        /* eof-in-processing-instruction: the token is dropped. */
        return drop_token_at_end(tokenizer);
    }
    switch (tokenizer->state) {
    case SXML_HTML_STATE_PROCESSING_INSTRUCTION_OPEN:
        if (is_ascii_alpha(character) || character == '_') {
            reconsume_in(tokenizer, SXML_HTML_STATE_PROCESSING_INSTRUCTION_TARGET);
            return SXML_STATUS_OK;
        }
        /* invalid-first-character-of-processing-instruction-target. */
        report_parse_error(tokenizer);
        reconsume_in(tokenizer, SXML_HTML_STATE_BOGUS_COMMENT);
        return convert_temporary_buffer_to_comment(tokenizer);
    case SXML_HTML_STATE_PROCESSING_INSTRUCTION_TARGET:
        if (is_tokenizer_whitespace(character) || character == '?' || character == '>') {
            const sxml_byte_view target = temporary_buffer_view(tokenizer);
            if (is_ascii_case_insensitive_match(target, SXML_BYTE_VIEW_FROM_LITERAL("xml")) ||
                is_ascii_case_insensitive_match(target,
                                                SXML_BYTE_VIEW_FROM_LITERAL("xml-stylesheet"))) {
                /* disallowed-processing-instruction-target. */
                report_parse_error(tokenizer);
                reconsume_in(tokenizer, SXML_HTML_STATE_BOGUS_COMMENT);
                return convert_temporary_buffer_to_comment(tokenizer);
            }
            begin_token(tokenizer, SXML_HTML_TOKEN_KIND_PROCESSING_INSTRUCTION);
            tokenizer->name_range = start_field(tokenizer);
            const sxml_status status =
                append_bytes_to_field(tokenizer, &tokenizer->name_range, target);
            tokenizer->data_range = start_field(tokenizer);
            reconsume_in(tokenizer, SXML_HTML_STATE_AFTER_PROCESSING_INSTRUCTION_TARGET);
            return status;
        }
        if (is_ascii_alphanumeric(character) || character == '-' || character == '_') {
            return sxml_xml_buffer_append_byte(&tokenizer->temporary_buffer, (uint8_t)character);
        }
        /* invalid-processing-instruction-target. */
        report_parse_error(tokenizer);
        reconsume_in(tokenizer, SXML_HTML_STATE_BOGUS_COMMENT);
        return convert_temporary_buffer_to_comment(tokenizer);
    case SXML_HTML_STATE_AFTER_PROCESSING_INSTRUCTION_TARGET:
        if (!is_tokenizer_whitespace(character)) {
            reconsume_in(tokenizer, SXML_HTML_STATE_PROCESSING_INSTRUCTION_DATA);
        }
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_PROCESSING_INSTRUCTION_DATA:
        if (character == '?') {
            switch_to(tokenizer, SXML_HTML_STATE_PROCESSING_INSTRUCTION_QUESTIONABLE);
            return SXML_STATUS_OK;
        }
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        return append_to_field(tokenizer, &tokenizer->data_range, character);
    default:
        /* The processing instruction questionable state. */
        if (character == '>') {
            switch_to(tokenizer, SXML_HTML_STATE_DATA);
            return emit_current_token(tokenizer);
        }
        reconsume_in(tokenizer, SXML_HTML_STATE_PROCESSING_INSTRUCTION_DATA);
        return append_to_field(tokenizer, &tokenizer->data_range, '?');
    }
}

/* 13.2.5.77 and 13.2.5.79 to 13.2.5.84 (13.2.5.78 is
   run_named_character_reference). */
static sxml_status run_character_reference_states(sxml_html_tokenizer *tokenizer) {
    if (tokenizer->state == SXML_HTML_STATE_NAMED_CHARACTER_REFERENCE) {
        return run_named_character_reference(tokenizer);
    }
    if (tokenizer->state == SXML_HTML_STATE_NUMERIC_CHARACTER_REFERENCE_END) {
        return end_numeric_character_reference(tokenizer);
    }
    if (tokenizer->state == SXML_HTML_STATE_CHARACTER_REFERENCE) {
        tokenizer->temporary_buffer.byte_count = 0;
        const sxml_status status = sxml_xml_buffer_append_byte(&tokenizer->temporary_buffer, '&');
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const uint32_t character = consume_character(tokenizer);
    const sxml_html_tokenizer_state return_state =
        (sxml_html_tokenizer_state)tokenizer->return_state;
    switch (tokenizer->state) {
    case SXML_HTML_STATE_CHARACTER_REFERENCE:
        if (is_ascii_alphanumeric(character)) {
            reconsume_in(tokenizer, SXML_HTML_STATE_NAMED_CHARACTER_REFERENCE);
            return SXML_STATUS_OK;
        }
        if (character == '#') {
            switch_to(tokenizer, SXML_HTML_STATE_NUMERIC_CHARACTER_REFERENCE);
            return sxml_xml_buffer_append_byte(&tokenizer->temporary_buffer, '#');
        }
        reconsume_in(tokenizer, return_state);
        return flush_character_reference(tokenizer);
    case SXML_HTML_STATE_AMBIGUOUS_AMPERSAND:
        if (is_ascii_alphanumeric(character)) {
            return is_consumed_in_attribute(tokenizer)
                       ? append_to_attribute_value(tokenizer, character)
                       : emit_character(tokenizer, character);
        }
        if (character == ';') {
            /* unknown-named-character-reference. */
            report_parse_error(tokenizer);
        }
        reconsume_in(tokenizer, return_state);
        return SXML_STATUS_OK;
    case SXML_HTML_STATE_NUMERIC_CHARACTER_REFERENCE:
        tokenizer->character_reference_code = 0;
        if (character == 'x' || character == 'X') {
            switch_to(tokenizer, SXML_HTML_STATE_HEXADECIMAL_CHARACTER_REFERENCE_START);
            return sxml_xml_buffer_append_byte(&tokenizer->temporary_buffer, (uint8_t)character);
        }
        if (is_ascii_digit(character)) {
            reconsume_in(tokenizer, SXML_HTML_STATE_DECIMAL_CHARACTER_REFERENCE);
            return SXML_STATUS_OK;
        }
        /* absence-of-digits-in-numeric-character-reference. */
        report_parse_error(tokenizer);
        reconsume_in(tokenizer, return_state);
        return flush_character_reference(tokenizer);
    case SXML_HTML_STATE_HEXADECIMAL_CHARACTER_REFERENCE_START:
        if (is_ascii_hex_digit(character)) {
            reconsume_in(tokenizer, SXML_HTML_STATE_HEXADECIMAL_CHARACTER_REFERENCE);
            return SXML_STATUS_OK;
        }
        report_parse_error(tokenizer);
        reconsume_in(tokenizer, return_state);
        return flush_character_reference(tokenizer);
    case SXML_HTML_STATE_HEXADECIMAL_CHARACTER_REFERENCE:
        if (is_ascii_hex_digit(character)) {
            add_reference_digit(tokenizer, 16, hex_digit_value(character));
            return SXML_STATUS_OK;
        }
        break;
    default:
        /* The decimal character reference state. */
        if (is_ascii_digit(character)) {
            add_reference_digit(tokenizer, 10, character - '0');
            return SXML_STATUS_OK;
        }
        break;
    }
    if (character == ';') {
        switch_to(tokenizer, SXML_HTML_STATE_NUMERIC_CHARACTER_REFERENCE_END);
    } else {
        /* missing-semicolon-after-character-reference. */
        report_parse_error(tokenizer);
        reconsume_in(tokenizer, SXML_HTML_STATE_NUMERIC_CHARACTER_REFERENCE_END);
    }
    return SXML_STATUS_OK;
}

/* One step of the state machine. */
static sxml_status run_step(sxml_html_tokenizer *tokenizer) {
    switch ((sxml_html_tokenizer_state)tokenizer->state) {
    case SXML_HTML_STATE_DATA:
    case SXML_HTML_STATE_RCDATA:
    case SXML_HTML_STATE_RAWTEXT:
    case SXML_HTML_STATE_SCRIPT_DATA:
    case SXML_HTML_STATE_PLAINTEXT:
        return run_data_like_state(tokenizer);
    case SXML_HTML_STATE_TAG_OPEN:
    case SXML_HTML_STATE_END_TAG_OPEN:
    case SXML_HTML_STATE_TAG_NAME:
        return run_tag_states(tokenizer);
    case SXML_HTML_STATE_RCDATA_LESS_THAN_SIGN:
        return run_less_than_sign_state(tokenizer, SXML_HTML_STATE_RCDATA_END_TAG_OPEN, SXML_HTML_STATE_RCDATA);
    case SXML_HTML_STATE_RCDATA_END_TAG_OPEN:
        return run_end_tag_open_state(tokenizer, SXML_HTML_STATE_RCDATA_END_TAG_NAME, SXML_HTML_STATE_RCDATA);
    case SXML_HTML_STATE_RCDATA_END_TAG_NAME:
        return run_end_tag_name_state(tokenizer, SXML_HTML_STATE_RCDATA);
    case SXML_HTML_STATE_RAWTEXT_LESS_THAN_SIGN:
        return run_less_than_sign_state(tokenizer, SXML_HTML_STATE_RAWTEXT_END_TAG_OPEN, SXML_HTML_STATE_RAWTEXT);
    case SXML_HTML_STATE_RAWTEXT_END_TAG_OPEN:
        return run_end_tag_open_state(tokenizer, SXML_HTML_STATE_RAWTEXT_END_TAG_NAME, SXML_HTML_STATE_RAWTEXT);
    case SXML_HTML_STATE_RAWTEXT_END_TAG_NAME:
        return run_end_tag_name_state(tokenizer, SXML_HTML_STATE_RAWTEXT);
    case SXML_HTML_STATE_SCRIPT_DATA_LESS_THAN_SIGN:
    case SXML_HTML_STATE_SCRIPT_DATA_END_TAG_OPEN:
    case SXML_HTML_STATE_SCRIPT_DATA_END_TAG_NAME:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPE_START:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPE_START_DASH:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_DASH:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_DASH_DASH:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_LESS_THAN_SIGN:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_END_TAG_OPEN:
    case SXML_HTML_STATE_SCRIPT_DATA_ESCAPED_END_TAG_NAME:
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPE_START:
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED:
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_DASH:
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_DASH_DASH:
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPED_LESS_THAN_SIGN:
    case SXML_HTML_STATE_SCRIPT_DATA_DOUBLE_ESCAPE_END:
        return run_script_states(tokenizer);
    case SXML_HTML_STATE_BEFORE_ATTRIBUTE_NAME:
    case SXML_HTML_STATE_ATTRIBUTE_NAME:
    case SXML_HTML_STATE_AFTER_ATTRIBUTE_NAME:
    case SXML_HTML_STATE_BEFORE_ATTRIBUTE_VALUE:
    case SXML_HTML_STATE_ATTRIBUTE_VALUE_DOUBLE_QUOTED:
    case SXML_HTML_STATE_ATTRIBUTE_VALUE_SINGLE_QUOTED:
    case SXML_HTML_STATE_ATTRIBUTE_VALUE_UNQUOTED:
    case SXML_HTML_STATE_AFTER_ATTRIBUTE_VALUE_QUOTED:
    case SXML_HTML_STATE_SELF_CLOSING_START_TAG:
        return run_attribute_states(tokenizer);
    case SXML_HTML_STATE_MARKUP_DECLARATION_OPEN:
        return run_markup_declaration_open(tokenizer);
    case SXML_HTML_STATE_BOGUS_COMMENT:
    case SXML_HTML_STATE_COMMENT_START:
    case SXML_HTML_STATE_COMMENT_START_DASH:
    case SXML_HTML_STATE_COMMENT:
    case SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN:
    case SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG:
    case SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG_DASH:
    case SXML_HTML_STATE_COMMENT_LESS_THAN_SIGN_BANG_DASH_DASH:
    case SXML_HTML_STATE_COMMENT_END_DASH:
    case SXML_HTML_STATE_COMMENT_END:
    case SXML_HTML_STATE_COMMENT_END_BANG:
        return run_comment_states(tokenizer);
    case SXML_HTML_STATE_DOCTYPE:
    case SXML_HTML_STATE_BEFORE_DOCTYPE_NAME:
    case SXML_HTML_STATE_DOCTYPE_NAME:
    case SXML_HTML_STATE_AFTER_DOCTYPE_NAME:
    case SXML_HTML_STATE_AFTER_DOCTYPE_SYSTEM_IDENTIFIER:
    case SXML_HTML_STATE_BOGUS_DOCTYPE:
        return run_doctype_name_states(tokenizer);
    case SXML_HTML_STATE_AFTER_DOCTYPE_PUBLIC_KEYWORD:
    case SXML_HTML_STATE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER:
    case SXML_HTML_STATE_DOCTYPE_PUBLIC_IDENTIFIER_DOUBLE_QUOTED:
    case SXML_HTML_STATE_DOCTYPE_PUBLIC_IDENTIFIER_SINGLE_QUOTED:
    case SXML_HTML_STATE_AFTER_DOCTYPE_PUBLIC_IDENTIFIER:
    case SXML_HTML_STATE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS:
    case SXML_HTML_STATE_AFTER_DOCTYPE_SYSTEM_KEYWORD:
    case SXML_HTML_STATE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER:
    case SXML_HTML_STATE_DOCTYPE_SYSTEM_IDENTIFIER_DOUBLE_QUOTED:
    case SXML_HTML_STATE_DOCTYPE_SYSTEM_IDENTIFIER_SINGLE_QUOTED:
        return run_doctype_identifier_states(tokenizer);
    case SXML_HTML_STATE_CDATA_SECTION:
    case SXML_HTML_STATE_CDATA_SECTION_BRACKET:
    case SXML_HTML_STATE_CDATA_SECTION_END:
        return run_cdata_states(tokenizer);
    case SXML_HTML_STATE_PROCESSING_INSTRUCTION_OPEN:
    case SXML_HTML_STATE_PROCESSING_INSTRUCTION_TARGET:
    case SXML_HTML_STATE_AFTER_PROCESSING_INSTRUCTION_TARGET:
    case SXML_HTML_STATE_PROCESSING_INSTRUCTION_DATA:
    case SXML_HTML_STATE_PROCESSING_INSTRUCTION_QUESTIONABLE:
        return run_processing_instruction_states(tokenizer);
    case SXML_HTML_STATE_CHARACTER_REFERENCE:
    case SXML_HTML_STATE_NAMED_CHARACTER_REFERENCE:
    case SXML_HTML_STATE_AMBIGUOUS_AMPERSAND:
    case SXML_HTML_STATE_NUMERIC_CHARACTER_REFERENCE:
    case SXML_HTML_STATE_HEXADECIMAL_CHARACTER_REFERENCE_START:
    case SXML_HTML_STATE_HEXADECIMAL_CHARACTER_REFERENCE:
    case SXML_HTML_STATE_DECIMAL_CHARACTER_REFERENCE:
    case SXML_HTML_STATE_NUMERIC_CHARACTER_REFERENCE_END:
        return run_character_reference_states(tokenizer);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* ------------------------------------------------------------ Interface */

void sxml_html_tokenizer_initialize(sxml_html_tokenizer *tokenizer,
                                    const sxml_html_tokenizer_configuration *configuration) {
    memset(tokenizer, 0, sizeof(*tokenizer));
    tokenizer->input = configuration->input;
    tokenizer->state = SXML_HTML_STATE_DATA;
    tokenizer->token_text = sxml_xml_buffer_empty(configuration->allocator);
    tokenizer->characters = sxml_xml_buffer_empty(configuration->allocator);
    tokenizer->temporary_buffer = sxml_xml_buffer_empty(configuration->allocator);
    tokenizer->last_start_tag_name = sxml_xml_buffer_empty(configuration->allocator);
    tokenizer->attribute_records = sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = configuration->allocator,
        .element_byte_count = sizeof(sxml_html_attribute_record)});
    tokenizer->emitted_attributes = sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = configuration->allocator,
        .element_byte_count = sizeof(sxml_html_token_attribute)});
}

void sxml_html_tokenizer_finalize(sxml_html_tokenizer *tokenizer) {
    if (tokenizer == NULL) {
        return;
    }
    sxml_xml_buffer_release(&tokenizer->token_text);
    sxml_xml_buffer_release(&tokenizer->characters);
    sxml_xml_buffer_release(&tokenizer->temporary_buffer);
    sxml_xml_buffer_release(&tokenizer->last_start_tag_name);
    sxml_xml_array_release(&tokenizer->attribute_records);
    sxml_xml_array_release(&tokenizer->emitted_attributes);
}

void sxml_html_tokenizer_switch_state(sxml_html_tokenizer *tokenizer,
                                      sxml_html_content_state state) {
    switch (state) {
    case SXML_HTML_CONTENT_STATE_DATA:
        tokenizer->state = SXML_HTML_STATE_DATA;
        return;
    case SXML_HTML_CONTENT_STATE_RCDATA:
        tokenizer->state = SXML_HTML_STATE_RCDATA;
        return;
    case SXML_HTML_CONTENT_STATE_RAWTEXT:
        tokenizer->state = SXML_HTML_STATE_RAWTEXT;
        return;
    case SXML_HTML_CONTENT_STATE_SCRIPT_DATA:
        tokenizer->state = SXML_HTML_STATE_SCRIPT_DATA;
        return;
    case SXML_HTML_CONTENT_STATE_PLAINTEXT:
        tokenizer->state = SXML_HTML_STATE_PLAINTEXT;
        return;
    }
}

/* Writes the ready token to *token. */
static sxml_status hand_out_ready_token(sxml_html_tokenizer *tokenizer, sxml_html_token *token) {
    tokenizer->has_ready_token = false;
    const sxml_xml_buffer *text = &tokenizer->token_text;
    tokenizer->emitted_attributes.element_count = 0;
    for (size_t record_index = 0; record_index < tokenizer->attribute_records.element_count;
         record_index += 1) {
        const sxml_html_attribute_record *record =
            sxml_xml_array_at(&tokenizer->attribute_records, record_index);
        if (record->is_duplicate) {
            continue;
        }
        void *element = NULL;
        const sxml_status status = sxml_xml_array_push(&tokenizer->emitted_attributes, &element);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_html_token_attribute *attribute = element;
        attribute->attribute_name = sxml_xml_buffer_view(text, record->attribute_name);
        attribute->attribute_value = sxml_xml_buffer_view(text, record->attribute_value);
    }
    *token = (sxml_html_token){
        .token_kind = tokenizer->token_kind,
        .data = sxml_xml_buffer_view(text, tokenizer->data_range),
        .name = sxml_xml_buffer_view(text, tokenizer->name_range),
        .attributes = {.attributes = (const sxml_html_token_attribute *)
                           tokenizer->emitted_attributes.elements,
                       .attribute_count = tokenizer->emitted_attributes.element_count},
        .is_self_closing = tokenizer->is_self_closing,
        .has_name = tokenizer->has_name,
        .has_public_identifier = tokenizer->has_public_identifier,
        .has_system_identifier = tokenizer->has_system_identifier,
        .forces_quirks = tokenizer->forces_quirks,
        .public_identifier = sxml_xml_buffer_view(text, tokenizer->public_identifier_range),
        .system_identifier = sxml_xml_buffer_view(text, tokenizer->system_identifier_range)};
    return SXML_STATUS_OK;
}

sxml_status sxml_html_tokenizer_next(sxml_html_tokenizer *tokenizer, sxml_html_token *token) {
    memset(token, 0, sizeof(*token));
    if (tokenizer->has_handed_out_characters) {
        tokenizer->characters.byte_count = 0;
        tokenizer->has_handed_out_characters = false;
    }
    while (!tokenizer->has_ready_token && !tokenizer->has_reached_end) {
        const sxml_status status = run_step(tokenizer);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (tokenizer->characters.byte_count > 0) {
        tokenizer->has_handed_out_characters = true;
        token->token_kind = SXML_HTML_TOKEN_KIND_CHARACTERS;
        token->data = (sxml_byte_view){.bytes = tokenizer->characters.bytes,
                                       .byte_count = tokenizer->characters.byte_count};
        return SXML_STATUS_OK;
    }
    if (tokenizer->has_ready_token) {
        return hand_out_ready_token(tokenizer, token);
    }
    tokenizer->has_emitted_end_of_file = true;
    token->token_kind = SXML_HTML_TOKEN_KIND_END_OF_FILE;
    return SXML_STATUS_OK;
}
