/*
 * CSV texts (src/csv_internal.h): a hand-written scanner (D97) over a CSV
 * text in memory. Each read looks at the cursor and the bytes after it
 * and moves the cursor past one token; nothing is allocated.
 *
 * Every special character is matched as its whole UTF-8 sequence at the
 * current byte. Scanning byte by byte cannot match inside another
 * character, because a UTF-8 lead byte never equals a continuation byte.
 *
 * Golden file: src/json.c (module comment, static helpers, status
 * handling), with the interface rules of docs/C_GUIDELINES.md.
 */
#include "csv_internal.h"

#include <string.h>

/* ------------------------------------------------------------- Helpers */

static bool csv_is_space_or_tab(uint8_t byte) {
    return byte == ' ' || byte == '\t';
}

static bool csv_is_newline_byte(uint8_t byte) {
    return byte == '\n' || byte == '\r';
}

/* True when the bytes of character start at offset in text. */
typedef struct csv_match {
    sxml_byte_view text;
    size_t byte_offset;
} csv_match;

static bool csv_matches(csv_match match, sxml_byte_view character) {
    return character.byte_count > 0 && match.byte_offset <= match.text.byte_count &&
           character.byte_count <= match.text.byte_count - match.byte_offset &&
           memcmp(match.text.bytes + match.byte_offset, character.bytes, character.byte_count) == 0;
}

/* The length of the newline at offset (CR LF is 2, CR or LF is 1), 0 when
   there is none. */
static size_t csv_newline_length(sxml_byte_view text, size_t byte_offset) {
    if (byte_offset >= text.byte_count || !csv_is_newline_byte(text.bytes[byte_offset])) {
        return 0;
    }
    if (text.bytes[byte_offset] == '\r' && byte_offset + 1 < text.byte_count &&
        text.bytes[byte_offset + 1] == '\n') {
        return 2;
    }
    return 1;
}

/* The number of bytes of the UTF-8 character starting with lead_byte; 0
   for a byte that cannot start one. */
static size_t csv_character_length(uint8_t lead_byte) {
    if (lead_byte < 0x80U) {
        return 1;
    }
    if (lead_byte >= 0xC2U && lead_byte <= 0xDFU) {
        return 2;
    }
    if (lead_byte >= 0xE0U && lead_byte <= 0xEFU) {
        return 3;
    }
    if (lead_byte >= 0xF0U && lead_byte <= 0xF4U) {
        return 4;
    }
    return 0;
}

/* True when character is exactly one well-formed UTF-8 character other
   than LF and CR. */
static bool csv_is_one_character(sxml_byte_view character) {
    if (character.byte_count == 0 || character.byte_count > 4 ||
        csv_character_length(character.bytes[0]) != character.byte_count ||
        csv_is_newline_byte(character.bytes[0])) {
        return false;
    }
    for (size_t byte_index = 1; byte_index < character.byte_count; byte_index += 1) {
        if ((character.bytes[byte_index] & 0xC0U) != 0x80U) {
            return false;
        }
    }
    return true;
}

bool sxml_csv_dialect_is_valid(const sxml_csv_dialect *dialect) {
    const bool has_comment_marker = dialect->comment_marker.byte_count > 0;
    if (!csv_is_one_character(dialect->separator) ||
        !csv_is_one_character(dialect->quote_character) ||
        (has_comment_marker && !csv_is_one_character(dialect->comment_marker))) {
        return false;
    }
    if (sxml_byte_view_equals(dialect->separator, dialect->quote_character)) {
        return false;
    }
    return !has_comment_marker ||
           (!sxml_byte_view_equals(dialect->comment_marker, dialect->separator) &&
            !sxml_byte_view_equals(dialect->comment_marker, dialect->quote_character));
}

sxml_byte_view sxml_csv_trim_whitespace(sxml_byte_view text) {
    if (text.byte_count == 0) {
        return text;
    }
    size_t first = 0;
    size_t end = text.byte_count;
    while (first < end && (csv_is_space_or_tab(text.bytes[first]) ||
                           csv_is_newline_byte(text.bytes[first]))) {
        first += 1;
    }
    while (end > first && (csv_is_space_or_tab(text.bytes[end - 1]) ||
                           csv_is_newline_byte(text.bytes[end - 1]))) {
        end -= 1;
    }
    return (sxml_byte_view){.bytes = text.bytes + first, .byte_count = end - first};
}

/* -------------------------------------------------------------- Cursor */

sxml_csv_cursor sxml_csv_cursor_start(void) {
    return (sxml_csv_cursor){.byte_offset = 0,
                             .is_at_row_start = true,
                             .has_pending_row_end = false,
                             .is_finished = false};
}

static sxml_csv_token csv_simple_token(sxml_csv_token_kind token_kind) {
    return (sxml_csv_token){.token_kind = token_kind,
                            .field_text = {.bytes = NULL, .byte_count = 0},
                            .is_quoted = false,
                            .needs_decoding = false};
}

/* Moves byte_offset past the comment rows that start there (F&O 4.0,
   section 17.5.3): each up to and including its next newline. */
static size_t csv_skip_comment_rows(const sxml_csv_source *source, size_t byte_offset) {
    const sxml_byte_view text = source->csv_text;
    while (csv_matches((csv_match){.text = text, .byte_offset = byte_offset},
                       source->dialect.comment_marker)) {
        while (byte_offset < text.byte_count && !csv_is_newline_byte(text.bytes[byte_offset])) {
            byte_offset += 1;
        }
        byte_offset += csv_newline_length(text, byte_offset);
    }
    return byte_offset;
}

/* With trimming: the offset of the newline or end after whitespace from
   byte_offset when the rest of the row is whitespace, or byte_offset
   itself when the row has more. */
static size_t csv_skip_blank_row_space(const sxml_csv_source *source, size_t byte_offset) {
    const sxml_byte_view text = source->csv_text;
    if (!source->dialect.trims_whitespace) {
        return byte_offset;
    }
    size_t end = byte_offset;
    while (end < text.byte_count && csv_is_space_or_tab(text.bytes[end])) {
        end += 1;
    }
    return end == text.byte_count || csv_is_newline_byte(text.bytes[end]) ? end : byte_offset;
}

/* After a field ending at byte_offset: moves the cursor past a separator,
   or past a newline or to the end with a row end to give next. */
static void csv_finish_field(const sxml_csv_source *source, size_t byte_offset,
                             sxml_csv_cursor *cursor_state) {
    const sxml_byte_view text = source->csv_text;
    if (csv_matches((csv_match){.text = text, .byte_offset = byte_offset},
                    source->dialect.separator)) {
        cursor_state->byte_offset = byte_offset + source->dialect.separator.byte_count;
        return;
    }
    cursor_state->byte_offset = byte_offset + csv_newline_length(text, byte_offset);
    cursor_state->has_pending_row_end = true;
}

/* An unquoted field from byte_offset up to the separator, a newline or
   the end (F&O 4.0, section 17.5.2: the quote character is ordinary). */
static void csv_read_unquoted_field(const sxml_csv_source *source, sxml_csv_cursor *cursor_state,
                                    sxml_csv_token *token) {
    const sxml_byte_view text = source->csv_text;
    const size_t first = cursor_state->byte_offset;
    size_t end = first;
    while (end < text.byte_count && !csv_is_newline_byte(text.bytes[end]) &&
           !csv_matches((csv_match){.text = text, .byte_offset = end}, source->dialect.separator)) {
        end += 1;
    }
    sxml_byte_view field_text = {.bytes = text.bytes + first, .byte_count = end - first};
    if (source->dialect.trims_whitespace) {
        field_text = sxml_csv_trim_whitespace(field_text);
    }
    *token = (sxml_csv_token){.token_kind = SXML_CSV_TOKEN_KIND_FIELD,
                              .field_text = field_text,
                              .is_quoted = false,
                              .needs_decoding = false};
    csv_finish_field(source, end, cursor_state);
}

/* A quoted field whose opening quote is at the cursor (F&O 4.0, section
   17.5.2). FOCV0001 for a missing closing quote or a character after it
   that is not the separator or a newline. */
static sxml_status csv_read_quoted_field(const sxml_csv_source *source,
                                         sxml_csv_cursor *cursor_state, sxml_csv_token *token) {
    const sxml_byte_view text = source->csv_text;
    const sxml_byte_view quote = source->dialect.quote_character;
    const size_t first = cursor_state->byte_offset + quote.byte_count;
    size_t end = first;
    bool needs_decoding = false;
    bool is_closed = false;
    while (!is_closed && end < text.byte_count) {
        if (csv_matches((csv_match){.text = text, .byte_offset = end}, quote)) {
            if (csv_matches((csv_match){.text = text, .byte_offset = end + quote.byte_count},
                            quote)) {
                needs_decoding = true;
                end += 2 * quote.byte_count;
            } else {
                is_closed = true;
            }
        } else {
            needs_decoding = needs_decoding || text.bytes[end] == '\r';
            end += 1;
        }
    }
    if (!is_closed) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    const size_t after = end + quote.byte_count;
    if (after < text.byte_count && !csv_is_newline_byte(text.bytes[after]) &&
        !csv_matches((csv_match){.text = text, .byte_offset = after}, source->dialect.separator)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    *token = (sxml_csv_token){.token_kind = SXML_CSV_TOKEN_KIND_FIELD,
                              .field_text = {.bytes = text.bytes + first, .byte_count = end - first},
                              .is_quoted = true,
                              .needs_decoding = needs_decoding};
    csv_finish_field(source, after, cursor_state);
    return SXML_STATUS_OK;
}

sxml_status sxml_csv_read_token(const sxml_csv_source *source, sxml_csv_cursor *cursor_state,
                                sxml_csv_token *token) {
    const sxml_byte_view text = source->csv_text;
    if (cursor_state->has_pending_row_end) {
        cursor_state->has_pending_row_end = false;
        cursor_state->is_at_row_start = true;
        *token = csv_simple_token(SXML_CSV_TOKEN_KIND_ROW_END);
        return SXML_STATUS_OK;
    }
    if (cursor_state->is_at_row_start && !cursor_state->is_finished) {
        size_t byte_offset = csv_skip_comment_rows(source, cursor_state->byte_offset);
        byte_offset = csv_skip_blank_row_space(source, byte_offset);
        const size_t newline_length = csv_newline_length(text, byte_offset);
        cursor_state->byte_offset = byte_offset + newline_length;
        if (byte_offset == text.byte_count) {
            cursor_state->is_finished = true;
        } else if (newline_length > 0) {
            *token = csv_simple_token(SXML_CSV_TOKEN_KIND_ROW_END);
            return SXML_STATUS_OK;
        }
        cursor_state->is_at_row_start = false;
    }
    if (cursor_state->is_finished) {
        *token = csv_simple_token(SXML_CSV_TOKEN_KIND_INPUT_END);
        return SXML_STATUS_OK;
    }
    if (csv_matches((csv_match){.text = text, .byte_offset = cursor_state->byte_offset},
                    source->dialect.quote_character)) {
        return csv_read_quoted_field(source, cursor_state, token);
    }
    csv_read_unquoted_field(source, cursor_state, token);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Decoding */

sxml_status sxml_csv_decode_field(const sxml_csv_field_decoding *decoding,
                                  sxml_memory_view destination, sxml_byte_view *field_value) {
    const sxml_byte_view text = decoding->token->field_text;
    const sxml_byte_view quote = decoding->quote_character;
    if (destination.byte_count < text.byte_count) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    size_t byte_offset = 0;
    size_t written = 0;
    while (byte_offset < text.byte_count) {
        const size_t newline_length = csv_newline_length(text, byte_offset);
        if (newline_length > 0) {
            destination.bytes[written] = '\n';
            written += 1;
            byte_offset += newline_length;
        } else if (decoding->token->is_quoted &&
                   csv_matches((csv_match){.text = text, .byte_offset = byte_offset}, quote)) {
            /* Inside a quoted field a quote character is always doubled. */
            memcpy(destination.bytes + written, quote.bytes, quote.byte_count);
            written += quote.byte_count;
            byte_offset += 2 * quote.byte_count;
        } else {
            destination.bytes[written] = text.bytes[byte_offset];
            written += 1;
            byte_offset += 1;
        }
    }
    *field_value = (sxml_byte_view){.bytes = destination.bytes, .byte_count = written};
    return SXML_STATUS_OK;
}
