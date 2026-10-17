/*
 * The functions on CSV data (F&O 4.0, section 17.5): one library part,
 * sxml_function_library_csv (D141). fn:parse-csv reads its options by
 * the option parameter conventions (section 1.7), scans the text with
 * src/csv.c and makes an fn:parsed-csv-structure-record (D133) whose get
 * function captures the rows and the column index. fn:csv-to-xml is
 * defined by the specification as an expression over fn:parse-csv, and is
 * made so here: the record's columns and rows become the elements of
 * section 17.5.9. fn:csv-doc is fn:unparsed-text($source) =>
 * fn:parse-csv($options) (section 17.5.8): the text comes from the
 * fn:unparsed-text row of the resource part, so decoding (D131) is
 * written once.
 *
 * Golden file: src/function_library_json.c (options, a new tree per
 * call); the record function after src/function_library_regex.c.
 */
#include "csv_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "function_library_parts_internal.h"
#include "sxml/name.h"
#include "sxml/string.h"
#include "sxml/tree.h"
#include "sxml/types.h"

#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

static const sxml_static_error csv_syntax_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCV0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A quoted field has no closing quote, or a character other than the "
        "separator or a newline follows it (F&O 4.0, fn:parse-csv)"),
};

static const sxml_static_error csv_dialect_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCV0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "separator, quote-character and comment-marker must each be one "
        "character other than a newline, and all different (F&O 4.0, "
        "fn:parse-csv)"),
};

static const sxml_static_error csv_unknown_column_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCV0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The column name is not in the column index (F&O 4.0, section "
        "17.5.6, get)"),
};

static const sxml_static_error csv_header_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The header option is one xs:boolean or a sequence of xs:string "
        "(F&O 4.0, fn:parse-csv)"),
};

/* --------------------------------------------------------------- Helpers */

static bool csv_sequence_is_empty(sxml_sequence sequence) {
    return sxml_sequence_get_representation(sequence) == SXML_SEQUENCE_REPRESENTATION_EMPTY;
}

/* Makes an xs:integer item, owned, as a sequence. */
static sxml_status csv_make_integer(sxml_item_heap *item_heap, int64_t integer,
                                    sxml_sequence *result) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
            .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER, .datum_details = {.integer = integer}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* The text of a string item (any atomic item with a string datum). */
typedef struct csv_item_text {
    bool is_string;
    sxml_byte_view text;
} csv_item_text;

static csv_item_text csv_read_item_text(sxml_item item) {
    sxml_atomic_datum datum;
    if (sxml_item_get_kind(item) != SXML_ITEM_KIND_ATOMIC ||
        sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return (csv_item_text){.is_string = false, .text = {.bytes = NULL, .byte_count = 0}};
    }
    return (csv_item_text){.is_string = true, .text = datum.datum_details.string.utf8};
}

/* A 1-based position from an xs:positiveInteger item: 0 for a value
   beyond SIZE_MAX (outside int64_t it is held as a decimal), which no
   row or column reaches. */
static size_t csv_read_position(sxml_item item) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_INTEGER || datum.datum_details.integer <= 0 ||
        (uint64_t)datum.datum_details.integer > (uint64_t)SIZE_MAX) {
        return 0;
    }
    return (size_t)datum.datum_details.integer;
}

/* ------------------------------------------------------- Sequence lists */

/* Sequences collected in item heap memory, each owned by the list.
   Zero-initialize, set item_heap, release with csv_list_release. */
typedef struct csv_list {
    sxml_item_heap *item_heap;
    sxml_sequence *sequences;
    size_t sequence_count;
    size_t capacity;
} csv_list;

/* Appends sequence, taking it; released on failure. */
static sxml_status csv_list_take(csv_list *list, sxml_sequence sequence) {
    if (list->sequence_count == list->capacity) {
        const size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        if (capacity > SIZE_MAX / sizeof(sxml_sequence)) {
            sxml_sequence_release(sequence);
            return SXML_STATUS_ARITHMETIC_OVERFLOW;
        }
        void *memory = NULL;
        const sxml_status status = sxml_item_heap_allocate_bytes(
            list->item_heap,
            (sxml_allocation_request){.byte_count = capacity * sizeof(sxml_sequence),
                                      .alignment = alignof(sxml_sequence)},
            &memory);
        if (status != SXML_STATUS_OK) {
            sxml_sequence_release(sequence);
            return status;
        }
        if (list->sequence_count > 0) {
            memcpy(memory, list->sequences, list->sequence_count * sizeof(sxml_sequence));
        }
        sxml_item_heap_release_bytes(
            list->item_heap, (sxml_heap_bytes){.memory = list->sequences,
                                               .byte_count = list->capacity * sizeof(sxml_sequence)});
        list->sequences = memory;
        list->capacity = capacity;
    }
    list->sequences[list->sequence_count] = sequence;
    list->sequence_count += 1;
    return SXML_STATUS_OK;
}

/* Appends sequence, retaining it. */
static sxml_status csv_list_append(csv_list *list, sxml_sequence sequence) {
    sxml_sequence_retain(sequence);
    return csv_list_take(list, sequence);
}

/* Releases the sequences and keeps the memory. */
static void csv_list_clear(csv_list *list) {
    for (size_t sequence_index = 0; sequence_index < list->sequence_count; sequence_index += 1) {
        sxml_sequence_release(list->sequences[sequence_index]);
    }
    list->sequence_count = 0;
}

static void csv_list_release(csv_list *list) {
    csv_list_clear(list);
    sxml_item_heap_release_bytes(
        list->item_heap, (sxml_heap_bytes){.memory = list->sequences,
                                           .byte_count = list->capacity * sizeof(sxml_sequence)});
    list->sequences = NULL;
    list->capacity = 0;
}

/* The concatenation of the list's sequences, owned. */
static sxml_status csv_list_concatenate(const sxml_function_call *call, const csv_list *list,
                                        sxml_sequence *result) {
    return sxml_sequence_concatenate(
        call->item_heap,
        (sxml_sequence_view){.sequences = list->sequences, .sequence_count = list->sequence_count},
        result);
}

/* --------------------------------------------------------------- Options */

#define SXML_CSV_OPTION(name_text, type_index, occurrence_value)              \
    {.option_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                    \
     .option_type = {.item_type = {.index = (type_index)},                    \
                     .occurrence = (occurrence_value)}}

/* The options of fn:parse-csv, fn:csv-to-xml and fn:csv-doc, by position
   (F&O 4.0, fn:parse-csv). header's type, (xs:boolean | xs:string)*, is
   checked by csv_read_header. */
enum {
    SXML_CSV_OPTION_SEPARATOR,
    SXML_CSV_OPTION_QUOTE_CHARACTER,
    SXML_CSV_OPTION_COMMENT_MARKER,
    SXML_CSV_OPTION_TRIM_WHITESPACE,
    SXML_CSV_OPTION_HEADER,
    SXML_CSV_OPTION_SELECT_COLUMNS,
    SXML_CSV_OPTION_TRIM_ROWS,
};

static const sxml_option_definition csv_options[] = {
    SXML_CSV_OPTION("separator", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_CSV_OPTION("quote-character", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_CSV_OPTION("comment-marker", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_ZERO_OR_ONE),
    SXML_CSV_OPTION("trim-whitespace", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_CSV_OPTION("header", SXML_TYPE_XS_ANY_ATOMIC_TYPE, SXML_OCCURRENCE_ZERO_OR_MORE),
    SXML_CSV_OPTION("select-columns", SXML_TYPE_XS_POSITIVE_INTEGER,
                    SXML_OCCURRENCE_ZERO_OR_MORE),
    SXML_CSV_OPTION("trim-rows", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
};

/* How column names are found (option header). */
typedef enum csv_header_kind {
    /* false, the default: no names. */
    SXML_CSV_HEADER_KIND_NONE,
    /* true: the first row. */
    SXML_CSV_HEADER_KIND_FIRST_ROW,
    /* A sequence of strings: the names given. */
    SXML_CSV_HEADER_KIND_GIVEN,
} csv_header_kind;

/* What the options ask for. The views borrow from values. */
typedef struct csv_settings {
    sxml_option_values values;
    sxml_csv_dialect dialect;
    csv_header_kind header_kind;
    /* select-columns: xs:positiveInteger items, or empty for all. */
    sxml_sequence selected_columns;
    uint64_t selected_column_count;
    bool trims_rows;
} csv_settings;

/* The text of a present string option, else fallback. */
typedef struct csv_character_option {
    size_t option_index;
    sxml_byte_view fallback;
} csv_character_option;

static sxml_byte_view csv_character(const csv_settings *settings, csv_character_option option) {
    if (!settings->values.is_present[option.option_index] ||
        csv_sequence_is_empty(settings->values.values[option.option_index])) {
        return option.fallback;
    }
    return sxml_function_library_option_string(
               (sxml_option_lookup){.values = &settings->values, .option_index = option.option_index})
        .utf8;
}

/* The header option: one boolean, or strings (XPTY0004 otherwise). */
static sxml_status csv_read_header(const sxml_function_call *call, csv_settings *settings) {
    settings->header_kind = SXML_CSV_HEADER_KIND_NONE;
    if (!settings->values.is_present[SXML_CSV_OPTION_HEADER]) {
        return SXML_STATUS_OK;
    }
    const sxml_sequence header = settings->values.values[SXML_CSV_OPTION_HEADER];
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(header);
    sxml_iteration_step step = {.has_item = false};
    sxml_status status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
    uint64_t item_count = 0;
    bool is_boolean = false;
    bool boolean_value = false;
    while (status == SXML_STATUS_OK && step.has_item) {
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        if (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
            is_boolean = true;
            boolean_value = datum.datum_details.boolean;
        } else if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_STRING) {
            return sxml_function_library_report(call, &csv_header_type_error);
        }
        item_count += 1;
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (is_boolean && item_count > 1) {
        return sxml_function_library_report(call, &csv_header_type_error);
    }
    if (is_boolean) {
        settings->header_kind =
            boolean_value ? SXML_CSV_HEADER_KIND_FIRST_ROW : SXML_CSV_HEADER_KIND_NONE;
    } else {
        settings->header_kind = SXML_CSV_HEADER_KIND_GIVEN;
    }
    return SXML_STATUS_OK;
}

/* Reads and checks the options of argument 1 (FOCV0002 for the dialect).
   Release settings->values on every path. */
static sxml_status csv_read_settings(const sxml_function_call *call, csv_settings *settings) {
    memset(settings, 0, sizeof(*settings));
    settings->selected_columns = sxml_sequence_empty();
    sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){
            .argument_index = 1,
            .definitions = {csv_options, sizeof(csv_options) / sizeof(csv_options[0])}},
        &settings->values);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    settings->dialect = (sxml_csv_dialect){
        .separator = csv_character(settings, (csv_character_option){
                                                 .option_index = SXML_CSV_OPTION_SEPARATOR,
                                                 .fallback = SXML_BYTE_VIEW_FROM_LITERAL(",")}),
        .quote_character = csv_character(
            settings, (csv_character_option){.option_index = SXML_CSV_OPTION_QUOTE_CHARACTER,
                                             .fallback = SXML_BYTE_VIEW_FROM_LITERAL("\"")}),
        .comment_marker = {.bytes = NULL, .byte_count = 0},
        .trims_whitespace = sxml_function_library_option_is_true(
            (sxml_option_lookup){.values = &settings->values,
                                 .option_index = SXML_CSV_OPTION_TRIM_WHITESPACE})};
    const bool has_comment_marker =
        settings->values.is_present[SXML_CSV_OPTION_COMMENT_MARKER] &&
        !csv_sequence_is_empty(settings->values.values[SXML_CSV_OPTION_COMMENT_MARKER]);
    if (has_comment_marker) {
        settings->dialect.comment_marker = csv_character(
            settings, (csv_character_option){.option_index = SXML_CSV_OPTION_COMMENT_MARKER,
                                             .fallback = {.bytes = NULL, .byte_count = 0}});
        if (settings->dialect.comment_marker.byte_count == 0) {
            return sxml_function_library_report(call, &csv_dialect_error);
        }
    }
    if (!sxml_csv_dialect_is_valid(&settings->dialect)) {
        return sxml_function_library_report(call, &csv_dialect_error);
    }
    if (settings->values.is_present[SXML_CSV_OPTION_SELECT_COLUMNS]) {
        settings->selected_columns = settings->values.values[SXML_CSV_OPTION_SELECT_COLUMNS];
        status = sxml_sequence_count(settings->selected_columns, call->error_record,
                                     &settings->selected_column_count);
    }
    settings->trims_rows = sxml_function_library_option_is_true(
        (sxml_option_lookup){.values = &settings->values, .option_index = SXML_CSV_OPTION_TRIM_ROWS});
    if (status == SXML_STATUS_OK) {
        status = csv_read_header(call, settings);
    }
    return status;
}

/* --------------------------------------------------------------- Parsing */

/* What fn:parse-csv finds (F&O 4.0, section 17.5.6), each owned. */
typedef struct csv_parsed {
    sxml_sequence columns;
    sxml_sequence column_index;
    sxml_sequence rows;
} csv_parsed;

static void csv_parsed_release(csv_parsed *parsed) {
    sxml_sequence_release(parsed->columns);
    sxml_sequence_release(parsed->column_index);
    sxml_sequence_release(parsed->rows);
    *parsed = (csv_parsed){.columns = sxml_sequence_empty(),
                           .column_index = sxml_sequence_empty(),
                           .rows = sxml_sequence_empty()};
}

/* The work of one parse: the fields of the row being read, the header
   names, the rows made, and memory for decoded fields. */
typedef struct csv_parse_state {
    const sxml_function_call *call;
    const csv_settings *settings;
    csv_list fields;
    csv_list names;
    csv_list rows;
    /* The members of the row being made, retained. */
    csv_list members;
    sxml_sequence empty_string;
    bool has_first_row;
    size_t first_row_width;
    sxml_memory_view decoding_memory;
} csv_parse_state;

static void csv_parse_state_release(csv_parse_state *state) {
    csv_list_release(&state->fields);
    csv_list_release(&state->names);
    csv_list_release(&state->rows);
    csv_list_release(&state->members);
    sxml_sequence_release(state->empty_string);
    sxml_item_heap_release_bytes(state->call->item_heap,
                                 (sxml_heap_bytes){.memory = state->decoding_memory.bytes,
                                                   .byte_count = state->decoding_memory.byte_count});
}

/* The value of a field token: its text, or its decoded text in the
   state's decoding memory. */
static sxml_status csv_field_value(csv_parse_state *state, const sxml_csv_token *token,
                                   sxml_byte_view *field_value) {
    if (!token->needs_decoding) {
        *field_value = token->field_text;
        return SXML_STATUS_OK;
    }
    if (state->decoding_memory.byte_count < token->field_text.byte_count) {
        sxml_item_heap_release_bytes(
            state->call->item_heap,
            (sxml_heap_bytes){.memory = state->decoding_memory.bytes,
                              .byte_count = state->decoding_memory.byte_count});
        state->decoding_memory = (sxml_memory_view){.bytes = NULL, .byte_count = 0};
        void *memory = NULL;
        const sxml_status status = sxml_item_heap_allocate_bytes(
            state->call->item_heap,
            (sxml_allocation_request){.byte_count = token->field_text.byte_count, .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        state->decoding_memory =
            (sxml_memory_view){.bytes = memory, .byte_count = token->field_text.byte_count};
    }
    return sxml_csv_decode_field(
        &(sxml_csv_field_decoding){.token = token,
                                   .quote_character = state->settings->dialect.quote_character},
        state->decoding_memory, field_value);
}

/* The header row: its fields, trimmed whatever the trim-whitespace option
   says, become the names (F&O 4.0, section 17.5.6, columns). */
static sxml_status csv_take_header_row(csv_parse_state *state) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t field_index = 0; status == SXML_STATUS_OK && field_index < state->fields.sequence_count;
         field_index += 1) {
        const csv_item_text field = csv_read_item_text(state->fields.sequences[field_index].content);
        sxml_sequence name = sxml_sequence_empty();
        status = sxml_function_library_make_string(state->call->item_heap,
                                                   sxml_csv_trim_whitespace(field.text), &name);
        if (status == SXML_STATUS_OK) {
            status = csv_list_take(&state->names, name);
        }
    }
    return status;
}

/* The member at 1-based position of the row being read: its field, or
   the zero-length string past its end. */
static sxml_sequence csv_member_at(const csv_parse_state *state, size_t position) {
    if (position == 0 || position > state->fields.sequence_count) {
        return state->empty_string;
    }
    return state->fields.sequences[position - 1];
}

/* The members of the row being read after select-columns, or trim-rows
   (ignored when select-columns is given), into state->members. */
static sxml_status csv_collect_members(csv_parse_state *state) {
    const csv_settings *settings = state->settings;
    csv_list_clear(&state->members);
    sxml_status status = SXML_STATUS_OK;
    if (settings->selected_column_count > 0) {
        for (uint64_t selection_index = 0;
             status == SXML_STATUS_OK && selection_index < settings->selected_column_count;
             selection_index += 1) {
            sxml_item selection = {.representation = 0};
            status = sxml_sequence_get_item(
                settings->selected_columns,
                (sxml_item_request){.item_index = selection_index,
                                    .error_record = state->call->error_record},
                &selection);
            if (status == SXML_STATUS_OK) {
                status = csv_list_append(&state->members,
                                         csv_member_at(state, csv_read_position(selection)));
            }
        }
        return status;
    }
    const size_t width = settings->trims_rows ? state->first_row_width
                                              : state->fields.sequence_count;
    for (size_t position = 1; status == SXML_STATUS_OK && position <= width; position += 1) {
        status = csv_list_append(&state->members, csv_member_at(state, position));
    }
    return status;
}

/* Ends a row: the first becomes the names when the header is the first
   row; every other becomes an array of strings. */
static sxml_status csv_end_row(csv_parse_state *state) {
    sxml_status status = sxml_function_library_charge_steps(
        state->call, (uint64_t)state->fields.sequence_count + 1);
    const bool is_first_row = !state->has_first_row;
    if (is_first_row) {
        state->has_first_row = true;
        state->first_row_width = state->fields.sequence_count;
    }
    if (status == SXML_STATUS_OK && is_first_row &&
        state->settings->header_kind == SXML_CSV_HEADER_KIND_FIRST_ROW) {
        status = csv_take_header_row(state);
        csv_list_clear(&state->fields);
        return status;
    }
    if (status == SXML_STATUS_OK) {
        status = csv_collect_members(state);
    }
    sxml_item row = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_array_make(state->call->item_heap,
                                 (sxml_sequence_view){.sequences = state->members.sequences,
                                                      .sequence_count = state->members.sequence_count},
                                 &row);
    }
    if (status == SXML_STATUS_OK) {
        status = csv_list_take(&state->rows, sxml_sequence_from_item(row));
    }
    csv_list_clear(&state->members);
    csv_list_clear(&state->fields);
    return status;
}

/* Reads every token of text into rows and header names. */
static sxml_status csv_read_rows(csv_parse_state *state, sxml_byte_view text) {
    const sxml_csv_source source = {.csv_text = text, .dialect = state->settings->dialect};
    sxml_csv_cursor cursor = sxml_csv_cursor_start();
    sxml_csv_token token = {.token_kind = SXML_CSV_TOKEN_KIND_FIELD};
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && token.token_kind != SXML_CSV_TOKEN_KIND_INPUT_END) {
        status = sxml_csv_read_token(&source, &cursor, &token);
        if (status == SXML_STATUS_MALFORMED_INPUT) {
            return sxml_function_library_report(state->call, &csv_syntax_error);
        }
        sxml_byte_view field_value = {.bytes = NULL, .byte_count = 0};
        sxml_sequence field = sxml_sequence_empty();
        if (status == SXML_STATUS_OK && token.token_kind == SXML_CSV_TOKEN_KIND_FIELD) {
            status = csv_field_value(state, &token, &field_value);
            if (status == SXML_STATUS_OK) {
                status = sxml_function_library_make_string(state->call->item_heap, field_value,
                                                           &field);
            }
            if (status == SXML_STATUS_OK) {
                status = csv_list_take(&state->fields, field);
            }
        } else if (status == SXML_STATUS_OK && token.token_kind == SXML_CSV_TOKEN_KIND_ROW_END) {
            status = csv_end_row(state);
        }
    }
    return status;
}

/* The given names (header as strings), as new xs:string items. */
static sxml_status csv_take_given_names(csv_parse_state *state) {
    const sxml_sequence header = state->settings->values.values[SXML_CSV_OPTION_HEADER];
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(header);
    sxml_iteration_step step = {.has_item = false};
    sxml_status status = sxml_sequence_iterator_next(&iterator, state->call->error_record, &step);
    while (status == SXML_STATUS_OK && step.has_item) {
        sxml_sequence name = sxml_sequence_empty();
        status = sxml_function_library_make_string(state->call->item_heap,
                                                   csv_read_item_text(step.item).text, &name);
        if (status == SXML_STATUS_OK) {
            status = csv_list_take(&state->names, name);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&iterator, state->call->error_record, &step);
        }
    }
    return status;
}

/* The names from the first row after select-columns: the name of each
   selected column, the zero-length string past the header's end. */
static sxml_status csv_select_names(csv_parse_state *state) {
    const csv_settings *settings = state->settings;
    if (settings->header_kind != SXML_CSV_HEADER_KIND_FIRST_ROW ||
        settings->selected_column_count == 0) {
        return SXML_STATUS_OK;
    }
    csv_list selected = {.item_heap = state->call->item_heap};
    sxml_status status = SXML_STATUS_OK;
    for (uint64_t selection_index = 0;
         status == SXML_STATUS_OK && selection_index < settings->selected_column_count;
         selection_index += 1) {
        sxml_item selection = {.representation = 0};
        status = sxml_sequence_get_item(settings->selected_columns,
                                        (sxml_item_request){.item_index = selection_index,
                                                            .error_record = state->call->error_record},
                                        &selection);
        if (status == SXML_STATUS_OK) {
            const size_t position = csv_read_position(selection);
            status = csv_list_append(&selected, position == 0 || position > state->names.sequence_count
                                                    ? state->empty_string
                                                    : state->names.sequences[position - 1]);
        }
    }
    if (status == SXML_STATUS_OK) {
        csv_list_release(&state->names);
        state->names = selected;
    } else {
        csv_list_release(&selected);
    }
    return status;
}

/* The column index: each non-empty name not seen before, to its 1-based
   position (F&O 4.0, section 17.5.6, column-index). */
static sxml_status csv_make_column_index(csv_parse_state *state, sxml_sequence *column_index) {
    const size_t name_count = state->names.sequence_count;
    sxml_map_entry *entries = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (name_count > SIZE_MAX / sizeof(sxml_map_entry)) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    if (name_count > 0) {
        void *memory = NULL;
        status = sxml_item_heap_allocate_bytes(
            state->call->item_heap,
            (sxml_allocation_request){.byte_count = name_count * sizeof(sxml_map_entry),
                                      .alignment = alignof(sxml_map_entry)},
            &memory);
        entries = memory;
    }
    size_t entry_count = 0;
    for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < name_count;
         name_index += 1) {
        const sxml_sequence name = state->names.sequences[name_index];
        if (csv_read_item_text(name.content).text.byte_count == 0) {
            continue;
        }
        sxml_sequence position = sxml_sequence_empty();
        status = csv_make_integer(state->call->item_heap, (int64_t)name_index + 1, &position);
        if (status == SXML_STATUS_OK) {
            entries[entry_count] = (sxml_map_entry){.key = name.content, .value = position};
            entry_count += 1;
        }
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            state->call->item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries, .map_entry_count = entry_count},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                .error_record = state->call->error_record},
            &map);
    }
    for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
        sxml_sequence_release(entries[entry_index].value);
    }
    sxml_item_heap_release_bytes(state->call->item_heap,
                                 (sxml_heap_bytes){.memory = entries,
                                                   .byte_count = name_count * sizeof(sxml_map_entry)});
    if (status == SXML_STATUS_OK) {
        *column_index = sxml_sequence_from_item(map);
    }
    return status;
}

/*
 * Parses text with the settings (F&O 4.0, fn:parse-csv) into *parsed:
 * the names (from the first row, trimmed and then selected, or as given,
 * unselected), the column index and the rows.
 */
typedef struct csv_parse_request {
    sxml_byte_view text;
    const csv_settings *settings;
} csv_parse_request;

static sxml_status csv_parse(const sxml_function_call *call, csv_parse_request request,
                             csv_parsed *parsed) {
    csv_parse_state state = {.call = call,
                             .settings = request.settings,
                             .fields = {.item_heap = call->item_heap},
                             .names = {.item_heap = call->item_heap},
                             .rows = {.item_heap = call->item_heap},
                             .members = {.item_heap = call->item_heap},
                             .empty_string = sxml_sequence_empty(),
                             .has_first_row = false,
                             .first_row_width = 0,
                             .decoding_memory = {.bytes = NULL, .byte_count = 0}};
    *parsed = (csv_parsed){.columns = sxml_sequence_empty(),
                           .column_index = sxml_sequence_empty(),
                           .rows = sxml_sequence_empty()};
    sxml_status status = sxml_function_library_make_string(
        call->item_heap, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &state.empty_string);
    if (status == SXML_STATUS_OK && request.settings->header_kind == SXML_CSV_HEADER_KIND_GIVEN) {
        status = csv_take_given_names(&state);
    }
    if (status == SXML_STATUS_OK) {
        status = csv_read_rows(&state, request.text);
    }
    if (status == SXML_STATUS_OK) {
        status = csv_select_names(&state);
    }
    if (status == SXML_STATUS_OK) {
        status = csv_list_concatenate(call, &state.names, &parsed->columns);
    }
    if (status == SXML_STATUS_OK) {
        status = csv_make_column_index(&state, &parsed->column_index);
    }
    if (status == SXML_STATUS_OK) {
        status = csv_list_concatenate(call, &state.rows, &parsed->rows);
    }
    if (status != SXML_STATUS_OK) {
        csv_parsed_release(parsed);
    }
    csv_parse_state_release(&state);
    return status;
}

/* Reads $value (argument 0, a string) and the options, and parses. */
static sxml_status csv_parse_call(const sxml_function_call *call, csv_parsed *parsed) {
    sxml_string text = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    csv_settings settings;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &text);
    if (status == SXML_STATUS_OK) {
        status = csv_read_settings(call, &settings);
    } else {
        memset(&settings, 0, sizeof(settings));
    }
    if (status == SXML_STATUS_OK) {
        status = csv_parse(call, (csv_parse_request){.text = text.utf8, .settings = &settings},
                           parsed);
    }
    sxml_function_library_release_options(&settings.values);
    return status;
}

/* ------------------------------------------------------------ The record */

/* The fields of fn:parsed-csv-structure-record: columns, column-index,
   rows, get. */
#define SXML_CSV_RECORD_FIELD_COUNT ((size_t)4)
#define SXML_CSV_RECORD_GET_FIELD ((size_t)3)

/*
 * get($r as xs:positiveInteger, $c as (xs:positiveInteger | xs:string))
 * as xs:string (F&O 4.0, section 17.5.6): the field at row $r and column
 * $c, the zero-length string out of range; a column name goes through
 * the column index (FOCV0004 when it is not there). The rows and the
 * column index are the function item's captured values.
 */
static sxml_status call_csv_get(const sxml_function_call *call, sxml_sequence *result) {
    sxml_function_item_description description;
    sxml_status status = sxml_function_item_describe(call->function_item, &description);
    if (status == SXML_STATUS_OK && description.captured_values.sequence_count != 2) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_optional_item row_argument;
    sxml_optional_item column_argument;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 0, &row_argument);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &column_argument);
    }
    if (status == SXML_STATUS_OK && (!row_argument.has_item || !column_argument.has_item)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence rows = description.captured_values.sequences[0];
    sxml_item column = column_argument.item;
    if (csv_read_item_text(column).is_string) {
        sxml_map_lookup lookup = {.is_found = false};
        status = sxml_map_find(description.captured_values.sequences[1].content, column, &lookup);
        if (status == SXML_STATUS_OK && !lookup.is_found) {
            return sxml_function_library_report(call, &csv_unknown_column_error);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        column = lookup.entry.value.content;
    }
    const size_t row_position = csv_read_position(row_argument.item);
    const size_t column_position = csv_read_position(column);
    sxml_item row = {.representation = 0};
    status = row_position == 0
                 ? SXML_STATUS_INDEX_OUT_OF_RANGE
                 : sxml_sequence_get_item(rows,
                                          (sxml_item_request){.item_index = row_position - 1,
                                                              .error_record = call->error_record},
                                          &row);
    sxml_sequence member = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = column_position == 0 ? SXML_STATUS_INDEX_OUT_OF_RANGE
                                      : sxml_array_get_member(row, column_position - 1, &member);
    }
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        return sxml_function_library_make_string(
            call->item_heap, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, result);
    }
    if (status == SXML_STATUS_OK) {
        sxml_sequence_retain(member);
        *result = member;
    }
    return status;
}

static const sxml_function_parameter csv_get_parameters[] = {
    SXML_REQUIRED_PARAMETER("r", "xs:positiveInteger"),
    SXML_REQUIRED_PARAMETER("c", "(xs:positiveInteger | xs:string)")};

/* The body of get: not a function of the library, so not in the part's
   rows. */
static const sxml_native_function csv_get_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("get"),
    .parameters = {csv_get_parameters, sizeof(csv_get_parameters) / sizeof(csv_get_parameters[0])},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
    .call = call_csv_get,
    .resumable_body = NULL};

/* The type of the record's get field, the signature of its function item
   (D133). */
static sxml_status csv_get_signature(const sxml_function_call *call,
                                     sxml_type_identifier *signature) {
    sxml_type_identifier record_type = {.index = SXML_TYPE_NONE};
    sxml_status status = sxml_function_library_find_record_type(
        call->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("parsed-csv-structure-record"), &record_type);
    sxml_type_description description = {.variety = SXML_TYPE_VARIETY_ANY_ITEM};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_describe_type(
            sxml_item_heap_get_type_registry(call->item_heap), record_type, &description);
    }
    if (status == SXML_STATUS_OK &&
        (description.variety != SXML_TYPE_VARIETY_RECORD ||
         description.variety_details.record_type.record_fields.record_field_count !=
             SXML_CSV_RECORD_FIELD_COUNT)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        *signature = description.variety_details.record_type.record_fields
                         .record_fields[SXML_CSV_RECORD_GET_FIELD]
                         .field_type.item_type;
    }
    return status;
}

/* The fn:parsed-csv-structure-record of parsed, owned, in *result. */
static sxml_status csv_make_record(const sxml_function_call *call, const csv_parsed *parsed,
                                   sxml_sequence *result) {
    sxml_type_identifier signature = {.index = SXML_TYPE_NONE};
    sxml_status status = csv_get_signature(call, &signature);
    const sxml_sequence captured[2] = {parsed->rows, parsed->column_index};
    sxml_item get_function = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_item_make(
            call->item_heap,
            &(sxml_function_item_request){
                .function_name = {.index = 0},
                .signature = signature,
                .function_annotations = {.function_annotations = NULL,
                                         .function_annotation_count = 0},
                .captured_values = {.sequences = captured, .sequence_count = 2},
                .body = {.state = (void *)&csv_get_row,
                         .operations = &sxml_native_function_body_operations}},
            &get_function);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence field_values[SXML_CSV_RECORD_FIELD_COUNT] = {
        parsed->columns, parsed->column_index, parsed->rows,
        sxml_sequence_from_item(get_function)};
    status = sxml_function_library_make_record(
        call,
        &(sxml_builtin_record_request){
            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("parsed-csv-structure-record"),
            .field_values = {.sequences = field_values,
                             .sequence_count = SXML_CSV_RECORD_FIELD_COUNT}},
        result);
    sxml_item_release(get_function);
    return status;
}

/* fn:parse-csv($value as xs:string?, $options as map(*)? := {}) as
   fn:parsed-csv-structure-record? (F&O 4.0, section 17.5.7). */
static sxml_status call_parse_csv(const sxml_function_call *call, sxml_sequence *result) {
    if (csv_sequence_is_empty(sxml_function_library_get_argument(call, 0))) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    csv_parsed parsed;
    sxml_status status = csv_parse_call(call, &parsed);
    if (status == SXML_STATUS_OK) {
        status = csv_make_record(call, &parsed, result);
        csv_parsed_release(&parsed);
    }
    return status;
}

/* ------------------------------------------------------------ The XML */

/* The element and attribute names of section 17.5.9. */
typedef struct csv_names {
    sxml_name_identifier csv_name;
    sxml_name_identifier columns_name;
    sxml_name_identifier column_name;
    sxml_name_identifier rows_name;
    sxml_name_identifier row_name;
    sxml_name_identifier field_name;
    sxml_name_identifier column_attribute_name;
} csv_names;

static sxml_status csv_intern_names(sxml_name_pool *name_pool, csv_names *names) {
    const char *const local_names[] = {"csv", "columns", "column", "rows", "row", "field", "column"};
    sxml_name_identifier *const outputs[] = {
        &names->csv_name, &names->columns_name, &names->column_name, &names->rows_name,
        &names->row_name, &names->field_name,   &names->column_attribute_name};
    const size_t name_count = sizeof(outputs) / sizeof(outputs[0]);
    sxml_status status = SXML_STATUS_OK;
    for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < name_count;
         name_index += 1) {
        /* The attribute, last, is in no namespace. */
        const sxml_well_known_namespace name_namespace =
            name_index + 1 == name_count ? SXML_NAMESPACE_NONE : SXML_NAMESPACE_FUNCTIONS;
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(name_namespace),
                .local_name = sxml_byte_view_from_c_string(local_names[name_index])},
            outputs[name_index]);
    }
    return status;
}

static const sxml_source_location csv_no_location = {
    .source_name = {.bytes = NULL, .byte_count = 0},
    .line_number = 0,
    .column_number = 0,
    .byte_offset = 0};

/* Where the elements go. */
typedef struct csv_xml_sink {
    sxml_tree_builder builder;
    csv_names names;
} csv_xml_sink;

static sxml_status csv_start_element(const csv_xml_sink *sink, sxml_name_identifier name) {
    return sink->builder.operations->start_element(
        sink->builder.state, &(sxml_element_start_event){.element_name = name,
                                                         .namespace_prefix = {.index = 0},
                                                         .type_annotation = {.index = SXML_TYPE_NONE},
                                                         .is_id = false,
                                                         .is_idrefs = false,
                                                         .is_nilled = false,
                                                         .ignores_parent_namespaces = false,
                                                         .location = csv_no_location});
}

static sxml_status csv_add_text(const csv_xml_sink *sink, sxml_byte_view text) {
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    return sink->builder.operations->add_text(
        sink->builder.state, &(sxml_text_event){.text = text, .location = csv_no_location});
}

/* An element with text content, and a column attribute when column_name
   is not empty. */
typedef struct csv_text_element {
    sxml_name_identifier name;
    sxml_byte_view text;
    sxml_byte_view column_name;
} csv_text_element;

static sxml_status csv_send_text_element(const csv_xml_sink *sink, csv_text_element element) {
    sxml_status status = csv_start_element(sink, element.name);
    if (status == SXML_STATUS_OK && element.column_name.byte_count > 0) {
        status = sink->builder.operations->add_attribute(
            sink->builder.state,
            &(sxml_attribute_event){.attribute_name = sink->names.column_attribute_name,
                                    .namespace_prefix = {.index = 0},
                                    .attribute_value = element.column_name,
                                    .type_annotation = {.index = SXML_TYPE_NONE},
                                    .is_id = false,
                                    .is_idrefs = false,
                                    .location = csv_no_location});
    }
    if (status == SXML_STATUS_OK) {
        status = csv_add_text(sink, element.text);
    }
    if (status == SXML_STATUS_OK) {
        status = sink->builder.operations->end_element(sink->builder.state);
    }
    return status;
}

/* The name of column position (from 1), empty when there is none. */
static sxml_byte_view csv_column_name(const csv_parsed *parsed, size_t position) {
    sxml_item name = {.representation = 0};
    if (sxml_sequence_get_item(parsed->columns,
                               (sxml_item_request){.item_index = position - 1, .error_record = NULL},
                               &name) != SXML_STATUS_OK) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return csv_read_item_text(name).text;
}

/* The columns element: the names up to the last non-empty one, if any. */
static sxml_status csv_send_columns(const csv_xml_sink *sink, const csv_parsed *parsed) {
    uint64_t name_count = 0;
    sxml_status status = sxml_sequence_count(parsed->columns, NULL, &name_count);
    size_t last_named = 0;
    for (size_t position = 1; status == SXML_STATUS_OK && position <= name_count; position += 1) {
        if (csv_column_name(parsed, position).byte_count > 0) {
            last_named = position;
        }
    }
    if (status != SXML_STATUS_OK || last_named == 0) {
        return status;
    }
    status = csv_start_element(sink, sink->names.columns_name);
    for (size_t position = 1; status == SXML_STATUS_OK && position <= last_named; position += 1) {
        status = csv_send_text_element(
            sink, (csv_text_element){.name = sink->names.column_name,
                                     .text = csv_column_name(parsed, position),
                                     .column_name = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK) {
        status = sink->builder.operations->end_element(sink->builder.state);
    }
    return status;
}

/* One row element: a field element per member. */
static sxml_status csv_send_row(const csv_xml_sink *sink, const csv_parsed *parsed,
                                sxml_item row) {
    size_t member_count = 0;
    sxml_status status = sxml_array_get_size(row, &member_count);
    if (status == SXML_STATUS_OK) {
        status = csv_start_element(sink, sink->names.row_name);
    }
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(row, member_index, &member);
        if (status == SXML_STATUS_OK) {
            status = csv_send_text_element(
                sink, (csv_text_element){.name = sink->names.field_name,
                                         .text = csv_read_item_text(member.content).text,
                                         .column_name = csv_column_name(parsed, member_index + 1)});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sink->builder.operations->end_element(sink->builder.state);
    }
    return status;
}

/* The csv element with its columns and rows (F&O 4.0, fn:csv-to-xml). */
static sxml_status csv_send_document(const sxml_function_call *call, const csv_parsed *parsed,
                                     const csv_xml_sink *sink) {
    const sxml_tree_builder builder = sink->builder;
    const sxml_byte_view base_uri =
        call->dynamic_context == NULL
            ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
            : sxml_dynamic_context_get_static_base_uri(call->dynamic_context);
    sxml_status status = builder.operations->start_document(
        builder.state, &(sxml_document_start_event){.document_uri = {.bytes = NULL, .byte_count = 0},
                                                    .base_uri = base_uri,
                                                    .location = csv_no_location});
    if (status == SXML_STATUS_OK) {
        status = csv_start_element(sink, sink->names.csv_name);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->add_namespace_binding(
            builder.state,
            &(sxml_namespace_binding){
                .namespace_prefix = {.index = 0},
                .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS)});
    }
    if (status == SXML_STATUS_OK) {
        status = csv_send_columns(sink, parsed);
    }
    if (status == SXML_STATUS_OK) {
        status = csv_start_element(sink, sink->names.rows_name);
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(parsed->rows);
    sxml_iteration_step step = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
    }
    while (status == SXML_STATUS_OK && step.has_item) {
        status = csv_send_row(sink, parsed, step.item);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        }
    }
    for (size_t level = 0; status == SXML_STATUS_OK && level < 2; level += 1) {
        status = builder.operations->end_element(builder.state);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    return status;
}

/* fn:csv-to-xml($value as xs:string?, $options as map(*)? := {}) as
   document-node(fn:csv)? (F&O 4.0, section 17.5.10): a new untyped tree
   on every call, its memory from the heap. */
static sxml_status call_csv_to_xml(const sxml_function_call *call, sxml_sequence *result) {
    if (csv_sequence_is_empty(sxml_function_library_get_argument(call, 0))) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    csv_parsed parsed;
    sxml_status status = csv_parse_call(call, &parsed);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    csv_xml_sink sink;
    status = csv_intern_names(sxml_item_heap_get_name_pool(call->item_heap), &sink.names);
    const sxml_allocator tree_allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_arena_tree *arena_tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_create(
            tree_allocator,
            &(sxml_arena_tree_configuration){
                .allocator = tree_allocator,
                .initial_memory = {.bytes = NULL, .byte_count = 0},
                .item_heap = call->item_heap,
                .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                .is_destroyed_when_unreferenced = true,
                .error_record = NULL},
            &arena_tree);
    }
    if (status == SXML_STATUS_OK) {
        sink.builder = sxml_arena_tree_as_tree_builder(arena_tree);
        status = csv_send_document(call, &parsed, &sink);
    }
    csv_parsed_release(&parsed);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *result = sxml_sequence_from_item(sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0}));
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ fn:csv-doc */

/* The fn:unparsed-text row of the resource part, found by name among the
   parts that serve XPath; NULL when there is none. */
static const sxml_native_function *csv_find_unparsed_text(void) {
    const sxml_native_function_view_list parts =
        sxml_function_library_parts_for_language(SXML_LANGUAGE_XPATH);
    for (size_t part_index = 0; part_index < parts.native_function_view_count; part_index += 1) {
        const sxml_native_function_view *part = parts.native_function_views[part_index];
        for (size_t row_index = 0; row_index < part->native_function_count; row_index += 1) {
            const sxml_native_function *row = &part->native_functions[row_index];
            if (row->function_namespace == SXML_NAMESPACE_FUNCTIONS && row->call != NULL &&
                sxml_byte_view_equals(row->local_name,
                                      SXML_BYTE_VIEW_FROM_LITERAL("unparsed-text"))) {
                return row;
            }
        }
    }
    return NULL;
}

/* fn:csv-doc($source as xs:string?, $options as map(*)? := {}) as
   fn:parsed-csv-structure-record? (F&O 4.0, section 17.5.8):
   fn:unparsed-text($source) => fn:parse-csv($options); the errors of
   both. */
static sxml_status call_csv_doc(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_native_function *unparsed_text = csv_find_unparsed_text();
    if (unparsed_text == NULL) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    if (csv_sequence_is_empty(sxml_function_library_get_argument(call, 0))) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    const sxml_sequence source = sxml_function_library_get_argument(call, 0);
    sxml_sequence text = sxml_sequence_empty();
    sxml_status status = unparsed_text->call(
        &(sxml_function_call){.function_item = call->function_item,
                              .arguments = {.sequences = &source, .sequence_count = 1},
                              .dynamic_context = call->dynamic_context,
                              .item_heap = call->item_heap,
                              .error_record = call->error_record},
        &text);
    if (status == SXML_STATUS_OK) {
        const sxml_sequence arguments[2] = {text, sxml_function_library_get_argument(call, 1)};
        status = call_parse_csv(
            &(sxml_function_call){.function_item = call->function_item,
                                  .arguments = {.sequences = arguments, .sequence_count = 2},
                                  .dynamic_context = call->dynamic_context,
                                  .item_heap = call->item_heap,
                                  .error_record = call->error_record},
            result);
        sxml_sequence_release(text);
    }
    return status;
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter parse_csv_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?")};
static const sxml_function_parameter csv_doc_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?")};

static const sxml_native_function csv_functions[] = {
    SXML_FUNCTION_ROW("parse-csv", parse_csv_parameters, "fn:parsed-csv-structure-record?", 0,
                      call_parse_csv),
    SXML_FUNCTION_ROW("csv-to-xml", parse_csv_parameters, "document-node()?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                          SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
                      call_csv_to_xml),
    /* Reads the evaluation environment, like fn:unparsed-text. */
    SXML_FUNCTION_ROW("csv-doc", csv_doc_parameters, "fn:parsed-csv-structure-record?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                          SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
                      call_csv_doc),
};

const sxml_native_function_view sxml_function_library_csv = SXML_LIBRARY_PART(csv_functions);
