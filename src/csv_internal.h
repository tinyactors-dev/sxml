#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * CSV texts (RFC 4180 with the extensions of F&O 4.0, section 17.5
 * "Functions on CSV Data"; docs/DECISIONS.md, D97, D141). Internal to sxml.
 *
 * A CSV text is scanned by hand, not by a tree-sitter grammar, because its
 * separator, quote character and comment marker are run-time options
 * (D97). Reading gives one csv_token at a time and keeps its place in a
 * csv_cursor, a plain value the caller holds: there is no object and no
 * memory. A field's text is a view into the CSV text; a quoted field whose
 * text holds a doubled quote character or a carriage return says so, and
 * sxml_csv_decode_field writes its value into memory the caller gives.
 * fn:parse-csv, fn:csv-to-xml and fn:csv-doc
 * (src/function_library_csv.c) build their results from the tokens.
 *
 * The rules (F&O 4.0, sections 17.5.1 to 17.5.3 and fn:parse-csv):
 * - CR LF and CR are newlines, read as LF, inside quoted fields too.
 * - A field is quoted when its first character is the quote character;
 *   inside, a doubled quote character is one. The closing quote must be
 *   followed by the separator, a newline or the end (FOCV0001 otherwise,
 *   and for a missing closing quote). Elsewhere the quote character is an
 *   ordinary character.
 * - With trimming, unquoted fields lose leading and trailing whitespace
 *   (U+0020, U+0009; newlines end the field); a quote character after
 *   whitespace does not start a quoted field.
 * - A row whose first character is the comment marker is skipped, up to
 *   and including the next newline (quoted or not).
 * - The empty text has no rows; a newline right at the start of a row is
 *   a blank row (with trimming, after whitespace); no row follows the
 *   final newline, and a row of only whitespace at the end of the text is
 *   no row when trimming.
 *
 * Thread safety: no state outside the caller's values.
 */

/* The characters that structure a CSV text (glossary `csv_dialect`): each
   one character as its UTF-8 bytes, borrowed. */
typedef struct sxml_csv_dialect {
    sxml_byte_view separator;
    sxml_byte_view quote_character;
    /* Empty: comments are not recognized. */
    sxml_byte_view comment_marker;
    /* Unquoted fields lose leading and trailing whitespace. */
    bool trims_whitespace;
} sxml_csv_dialect;

/*
 * True when dialect may be used: separator, quote character and (when
 * present) comment marker are each exactly one character of well-formed
 * UTF-8, none is LF or CR, and no two are the same (F&O 4.0, fn:parse-csv:
 * FOCV0002 otherwise).
 */
bool sxml_csv_dialect_is_valid(const sxml_csv_dialect *dialect);

/* What reading gives (glossary `csv_token`). */
typedef enum sxml_csv_token_kind {
    /* A field of the current row. */
    SXML_CSV_TOKEN_KIND_FIELD,
    /* The end of the current row; with no field before it, a blank row. */
    SXML_CSV_TOKEN_KIND_ROW_END,
    /* The end of the text; every later read gives it again. */
    SXML_CSV_TOKEN_KIND_INPUT_END,
} sxml_csv_token_kind;

typedef struct sxml_csv_token {
    sxml_csv_token_kind token_kind;
    /* FIELD: the field's text, borrowed from the CSV text: without its
       quotes when quoted, trimmed when the dialect trims an unquoted
       field. */
    sxml_byte_view field_text;
    bool is_quoted;
    /* FIELD: the text holds a doubled quote character or a carriage
       return, so its value needs sxml_csv_decode_field. */
    bool needs_decoding;
} sxml_csv_token;

/* Where reading continues (glossary `csv_cursor`). Start from
   sxml_csv_cursor_start(); its fields are private to src/csv.c. */
typedef struct sxml_csv_cursor {
    size_t byte_offset;
    bool is_at_row_start;
    bool has_pending_row_end;
    bool is_finished;
} sxml_csv_cursor;

/* A cursor at the start of a CSV text. */
sxml_csv_cursor sxml_csv_cursor_start(void);

/* A CSV text and its dialect, borrowed. csv_text is well-formed UTF-8;
   the dialect is valid (sxml_csv_dialect_is_valid). */
typedef struct sxml_csv_source {
    sxml_byte_view csv_text;
    sxml_csv_dialect dialect;
} sxml_csv_source;

/*
 * Reads the next token of source at cursor_state (in/out) into *token.
 * Returns SXML_STATUS_MALFORMED_INPUT (FOCV0001) for a quoted field
 * without its closing quote or with a character other than the separator
 * or a newline after it; cursor_state->byte_offset is then where the
 * field starts, and *token is untouched.
 */
SXML_NODISCARD sxml_status sxml_csv_read_token(const sxml_csv_source *source,
                                               sxml_csv_cursor *cursor_state,
                                               sxml_csv_token *token);

/* A field to decode: its token and the quote character it was read
   with. */
typedef struct sxml_csv_field_decoding {
    const sxml_csv_token *token;
    sxml_byte_view quote_character;
} sxml_csv_field_decoding;

/*
 * Writes the value of a FIELD token into destination, a doubled quote
 * character as one and CR LF and CR as LF, and writes the view of what it
 * wrote to *field_value. The value is never longer than the token's text.
 * Returns SXML_STATUS_CAPACITY_EXCEEDED when destination is shorter than
 * the token's text.
 */
SXML_NODISCARD sxml_status sxml_csv_decode_field(const sxml_csv_field_decoding *decoding,
                                                 sxml_memory_view destination,
                                                 sxml_byte_view *field_value);

/* text without leading and trailing U+0020, U+0009, U+000A and U+000D
   (header names are trimmed whether quoted or not, F&O 4.0, section
   17.5.6). */
sxml_byte_view sxml_csv_trim_whitespace(sxml_byte_view text);
