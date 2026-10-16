/*
 * Test lists (test_list.h), read line by line as tools/check-test-list.py
 * does: the text before "#" is split at spaces and tabs.
 */
#include "test_list.h"

#include <stdalign.h>
#include <stdbool.h>

static bool sxml_qt4_is_field_separator(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\r';
}

/* The fields of one line, up to three (a third one is an error). */
typedef struct sxml_qt4_line_fields {
    sxml_byte_view fields[3];
    size_t field_count;
} sxml_qt4_line_fields;

static void sxml_qt4_split_line(sxml_byte_view line,
                                sxml_qt4_line_fields *fields) {
    fields->field_count = 0;
    size_t byte_index = 0;
    while (byte_index < line.byte_count && line.bytes[byte_index] != '#') {
        if (sxml_qt4_is_field_separator(line.bytes[byte_index])) {
            byte_index += 1;
            continue;
        }
        const size_t start = byte_index;
        while (byte_index < line.byte_count && line.bytes[byte_index] != '#' &&
               !sxml_qt4_is_field_separator(line.bytes[byte_index])) {
            byte_index += 1;
        }
        if (fields->field_count < 3) {
            fields->fields[fields->field_count] = (sxml_byte_view){
                .bytes = line.bytes + start, .byte_count = byte_index - start};
            fields->field_count += 1;
        }
    }
}

/* Walks the lines of a text. */
typedef struct sxml_qt4_line_cursor {
    sxml_byte_view text;
    size_t byte_index;
    uint32_t line_number;
} sxml_qt4_line_cursor;

static bool sxml_qt4_next_line(sxml_qt4_line_cursor *cursor,
                               sxml_byte_view *line) {
    if (cursor->byte_index >= cursor->text.byte_count) {
        return false;
    }
    const size_t start = cursor->byte_index;
    while (cursor->byte_index < cursor->text.byte_count &&
           cursor->text.bytes[cursor->byte_index] != '\n') {
        cursor->byte_index += 1;
    }
    *line = (sxml_byte_view){.bytes = cursor->text.bytes + start,
                             .byte_count = cursor->byte_index - start};
    cursor->byte_index += 1;
    cursor->line_number += 1;
    return true;
}

sxml_status sxml_qt4_test_list_parse(const sxml_qt4_test_list_request *request,
                                     sxml_qt4_test_list *test_list) {
    /* One pass to count and check, one to fill. */
    size_t entry_count = 0;
    sxml_qt4_line_cursor cursor = {
        .text = request->text, .byte_index = 0, .line_number = 0};
    sxml_byte_view line = {.bytes = NULL, .byte_count = 0};
    while (sxml_qt4_next_line(&cursor, &line)) {
        sxml_qt4_line_fields fields = {.field_count = 0};
        sxml_qt4_split_line(line, &fields);
        if (fields.field_count == 0) {
            continue;
        }
        if (fields.field_count != 2) {
            if (request->problem != NULL) {
                *request->problem = (sxml_qt4_problem){
                    .description = SXML_BYTE_VIEW_FROM_LITERAL(
                        "expected '<test-set> <test-case>'"),
                    .subject = {.bytes = NULL, .byte_count = 0},
                    .location = {.source_name = request->source_name,
                                 .line_number = cursor.line_number,
                                 .column_number = 0,
                                 .byte_offset = 0}};
            }
            return SXML_STATUS_MALFORMED_INPUT;
        }
        entry_count += 1;
    }
    if (entry_count == 0) {
        *test_list = (sxml_qt4_test_list){.entries = NULL, .entry_count = 0};
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(
        entry_count, sizeof(sxml_qt4_test_list_entry), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_allocate(
            request->arena,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment =
                                          alignof(sxml_qt4_test_list_entry)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_test_list_entry *entries = memory;
    size_t entry_index = 0;
    cursor = (sxml_qt4_line_cursor){
        .text = request->text, .byte_index = 0, .line_number = 0};
    while (sxml_qt4_next_line(&cursor, &line)) {
        sxml_qt4_line_fields fields = {.field_count = 0};
        sxml_qt4_split_line(line, &fields);
        if (fields.field_count == 0 || entry_index == entry_count) {
            continue;
        }
        entries[entry_index] =
            (sxml_qt4_test_list_entry){.test_set_name = fields.fields[0],
                                       .test_case_name = fields.fields[1],
                                       .line_number = cursor.line_number};
        entry_index += 1;
    }
    *test_list =
        (sxml_qt4_test_list){.entries = entries, .entry_count = entry_count};
    return SXML_STATUS_OK;
}
