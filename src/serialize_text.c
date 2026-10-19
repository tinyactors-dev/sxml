/*
 * The text and CSV output methods (Serialization 4.0, sections 8 "Text
 * Output Method" and 10 "CSV Output Method"; docs/DECISIONS.md D120b,
 * D128). Both write through the character pipeline of src/serialize.c:
 * the text method with the escape style that escapes nothing, the CSV
 * method with the one that doubles the quote character.
 *
 * Golden file: src/serialize_xml.c (normalized events, node indices in
 * document order instead of recursion, D71).
 */
#include "serialize_internal.h"

#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "xdm_internal.h"

#include <string.h>

/* ------------------------------------------------------- The text method */

/* Writes the text descendants of an element, in document order: the
   element's string value (XDM 4.0, 7.6.12). */
static sxml_status sxml_text_write_descendants(sxml_serializer *serializer,
                                               sxml_node_reference element) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(element, &description);
    const uint64_t last_index = status == SXML_STATUS_OK ? description.last_descendant.index : 0;
    for (uint64_t node_index = (uint64_t)element.node_index.index + 1;
         status == SXML_STATUS_OK && node_index <= last_index; node_index += 1) {
        const sxml_node_reference node = {.tree = element.tree,
                                          .node_index = {.index = (uint32_t)node_index},
                                          .namespace_number = 0};
        sxml_node_description node_description;
        status = sxml_node_describe(node, &node_description);
        if (status != SXML_STATUS_OK || node_description.node_kind != SXML_NODE_KIND_TEXT) {
            continue;
        }
        sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_get_content(node, &content);
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_write_character_data(
                serializer, &(sxml_character_data){
                                .text = content, .character_context = SXML_CHARACTER_CONTEXT_TEXT});
        }
    }
    return status;
}

sxml_status sxml_serialize_text_method(sxml_serializer *serializer, sxml_sequence value) {
    sxml_sequence_normalizer normalizer;
    sxml_sequence_normalizer_start(serializer, value, &normalizer);
    sxml_normalized_event event = {.event_kind = SXML_NORMALIZED_EVENT_KIND_TEXT};
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        status = sxml_sequence_normalizer_next(&normalizer, &event);
        if (status != SXML_STATUS_OK || event.event_kind == SXML_NORMALIZED_EVENT_KIND_END) {
            break;
        }
        if (event.event_kind == SXML_NORMALIZED_EVENT_KIND_TEXT) {
            status = sxml_serializer_write_character_data(
                serializer, &(sxml_character_data){.text = event.text,
                                                   .character_context =
                                                       SXML_CHARACTER_CONTEXT_TEXT});
            continue;
        }
        /* Section 8: the string value of the result tree; comments and
           processing instructions have none in it. */
        sxml_node_description description;
        status = sxml_node_describe(event.node, &description);
        if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT) {
            status = sxml_text_write_descendants(serializer, event.node);
        } else if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_TEXT) {
            /* A text node whose escaping is disabled: the text method
               escapes nothing anyway (XSLT 4.0, section 26.1.8). */
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(event.node, &content);
            if (status == SXML_STATUS_OK) {
                status = sxml_serializer_write_character_data(
                    serializer, &(sxml_character_data){
                                    .text = content,
                                    .character_context = SXML_CHARACTER_CONTEXT_TEXT});
            }
        }
    }
    sxml_sequence_normalizer_finish(&normalizer);
    return status;
}

/* -------------------------------------------------------- The CSV method */

static sxml_status sxml_csv_raise(sxml_serializer *serializer) {
    return sxml_serializer_raise_error(
        serializer,
        (sxml_serialization_error){
            .error_local_name = SXML_BYTE_VIEW_FROM_LITERAL("SERE0025"),
            .description = SXML_BYTE_VIEW_FROM_LITERAL(
                "the CSV method needs a parsed CSV structure or a sequence of arrays of atomic "
                "items")});
}

/* A codepoint encoded as UTF-8. */
typedef struct sxml_csv_encoded {
    uint8_t bytes[4];
    size_t byte_count;
} sxml_csv_encoded;

static sxml_csv_encoded sxml_csv_encode(uint32_t codepoint) {
    sxml_csv_encoded encoded = {.bytes = {0}, .byte_count = 0};
    if (codepoint < 0x80U) {
        encoded.bytes[0] = (uint8_t)codepoint;
        encoded.byte_count = 1;
    } else if (codepoint < 0x800U) {
        encoded.bytes[0] = (uint8_t)(0xC0U | (codepoint >> 6));
        encoded.bytes[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded.byte_count = 2;
    } else if (codepoint < 0x10000U) {
        encoded.bytes[0] = (uint8_t)(0xE0U | (codepoint >> 12));
        encoded.bytes[1] = (uint8_t)(0x80U | ((codepoint >> 6) & 0x3FU));
        encoded.bytes[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded.byte_count = 3;
    } else {
        encoded.bytes[0] = (uint8_t)(0xF0U | (codepoint >> 18));
        encoded.bytes[1] = (uint8_t)(0x80U | ((codepoint >> 12) & 0x3FU));
        encoded.bytes[2] = (uint8_t)(0x80U | ((codepoint >> 6) & 0x3FU));
        encoded.bytes[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded.byte_count = 4;
    }
    return encoded;
}

/* The separator and quote character, as UTF-8 (section 10.3). */
typedef struct sxml_csv_delimiters {
    sxml_csv_encoded separator;
    sxml_csv_encoded quote;
} sxml_csv_delimiters;

/* Whether text holds the separator, the quote character, a newline or a
   carriage return: then the field is quoted (section 10.2). */
static bool sxml_csv_text_needs_quotes(const sxml_csv_delimiters *delimiters,
                                       sxml_byte_view text) {
    for (size_t offset = 0; offset < text.byte_count; offset += 1) {
        const uint8_t byte = text.bytes[offset];
        if (byte == '\n' || byte == '\r') {
            return true;
        }
        const bool is_separator =
            offset + delimiters->separator.byte_count <= text.byte_count &&
            memcmp(text.bytes + offset, delimiters->separator.bytes,
                   delimiters->separator.byte_count) == 0;
        const bool is_quote =
            offset + delimiters->quote.byte_count <= text.byte_count &&
            memcmp(text.bytes + offset, delimiters->quote.bytes, delimiters->quote.byte_count) == 0;
        if (is_separator || is_quote) {
            return true;
        }
    }
    return false;
}

/* Whether a field needs quotes after character mapping: its own text, or
   a replacement of one of its characters (section 10.2, step 3 after
   step 1). */
static bool sxml_csv_field_needs_quotes(const sxml_serializer *serializer,
                                        const sxml_csv_delimiters *delimiters,
                                        sxml_byte_view text) {
    if (sxml_csv_text_needs_quotes(delimiters, text)) {
        return true;
    }
    const sxml_character_map_view maps = serializer->parameters->use_character_maps;
    for (size_t mapping_index = 0; mapping_index < maps.character_mapping_count;
         mapping_index += 1) {
        const sxml_character_mapping mapping = maps.character_mappings[mapping_index];
        const sxml_csv_encoded encoded = sxml_csv_encode(mapping.codepoint);
        bool occurs = false;
        for (size_t offset = 0; !occurs && offset + encoded.byte_count <= text.byte_count;
             offset += 1) {
            occurs = memcmp(text.bytes + offset, encoded.bytes, encoded.byte_count) == 0;
        }
        if (occurs && sxml_csv_text_needs_quotes(delimiters, mapping.replacement_text)) {
            return true;
        }
    }
    return false;
}

static sxml_status sxml_csv_write_field(sxml_serializer *serializer,
                                        const sxml_csv_delimiters *delimiters,
                                        sxml_byte_view text) {
    const bool is_quoted = sxml_csv_field_needs_quotes(serializer, delimiters, text);
    const sxml_byte_view quote = {.bytes = delimiters->quote.bytes,
                                  .byte_count = delimiters->quote.byte_count};
    sxml_status status = is_quoted ? sxml_serializer_write_markup(serializer, quote) : SXML_STATUS_OK;
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write_character_data(
            serializer,
            &(sxml_character_data){.text = text, .character_context = SXML_CHARACTER_CONTEXT_TEXT});
    }
    if (status == SXML_STATUS_OK && is_quoted) {
        status = sxml_serializer_write_markup(serializer, quote);
    }
    return status;
}

/* The one atomic item a member of an input row atomizes to (section 10.1:
   coercion to array(xs:anyAtomicType)*): arrays of one member are
   unwrapped, nodes give their string value; anything else is SERE0025.
   *owned is an item to release, or 0. */
typedef struct sxml_csv_member {
    sxml_item atomic_item;
    sxml_item owned_item;
} sxml_csv_member;

static sxml_status sxml_csv_atomize_member(sxml_serializer *serializer, sxml_sequence member,
                                           sxml_csv_member *result) {
    result->owned_item = (sxml_item){.representation = 0};
    sxml_sequence current = member;
    for (;;) {
        uint64_t item_count = 0;
        sxml_status status = sxml_sequence_count(current, serializer->error_record, &item_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (item_count != 1) {
            return sxml_csv_raise(serializer);
        }
        sxml_item item = {.representation = 0};
        status = sxml_sequence_get_item(
            current,
            (sxml_item_request){.item_index = 0, .error_record = serializer->error_record}, &item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        switch (sxml_item_get_kind(item)) {
        case SXML_ITEM_KIND_ATOMIC:
            result->atomic_item = item;
            return SXML_STATUS_OK;
        case SXML_ITEM_KIND_NODE: {
            sxml_node_reference node;
            status = sxml_item_get_node(item, &node);
            if (status == SXML_STATUS_OK) {
                status = sxml_node_make_string_value(serializer->item_heap, node,
                                                     &result->owned_item);
            }
            result->atomic_item = result->owned_item;
            return status;
        }
        case SXML_ITEM_KIND_ARRAY: {
            size_t size = 0;
            status = sxml_array_get_size(item, &size);
            if (status == SXML_STATUS_OK && size != 1) {
                return sxml_csv_raise(serializer);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_array_get_member(item, 0, &current);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            break;
        }
        case SXML_ITEM_KIND_MAP:
        case SXML_ITEM_KIND_FUNCTION:
        case SXML_ITEM_KIND_JSON_NODE:
            return sxml_csv_raise(serializer);
        }
    }
}

/* Writes one row: the strings of fields, separated, then a line end. */
typedef struct sxml_csv_row {
    /* An array of members, or 0 for the sequence of strings below. */
    sxml_item array;
    sxml_sequence strings;
} sxml_csv_row;

static sxml_status sxml_csv_write_row(sxml_serializer *serializer,
                                      const sxml_csv_delimiters *delimiters,
                                      const sxml_csv_row *row) {
    const bool is_array = row->array.representation != 0;
    uint64_t field_count = 0;
    sxml_status status = SXML_STATUS_OK;
    if (is_array) {
        size_t size = 0;
        status = sxml_array_get_size(row->array, &size);
        field_count = size;
    } else {
        status = sxml_sequence_count(row->strings, serializer->error_record, &field_count);
    }
    for (uint64_t field_index = 0; status == SXML_STATUS_OK && field_index < field_count;
         field_index += 1) {
        if (field_index > 0) {
            status = sxml_serializer_write_markup(
                serializer, (sxml_byte_view){.bytes = delimiters->separator.bytes,
                                             .byte_count = delimiters->separator.byte_count});
        }
        sxml_csv_member member = {.atomic_item = {.representation = 0},
                                  .owned_item = {.representation = 0}};
        if (status == SXML_STATUS_OK && is_array) {
            sxml_sequence member_sequence = sxml_sequence_empty();
            status = sxml_array_get_member(row->array, (size_t)field_index, &member_sequence);
            if (status == SXML_STATUS_OK) {
                status = sxml_csv_atomize_member(serializer, member_sequence, &member);
            }
        } else if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                row->strings,
                (sxml_item_request){.item_index = field_index,
                                    .error_record = serializer->error_record},
                &member.atomic_item);
            if (status == SXML_STATUS_OK &&
                sxml_item_get_kind(member.atomic_item) != SXML_ITEM_KIND_ATOMIC) {
                status = sxml_csv_raise(serializer);
            }
        }
        sxml_atomic_text atomic_text = {.memory = NULL};
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_get_atomic_text(serializer, member.atomic_item, &atomic_text);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_csv_write_field(serializer, delimiters, atomic_text.text);
        }
        sxml_serializer_release_atomic_text(serializer, &atomic_text);
        if (member.owned_item.representation != 0) {
            sxml_item_release(member.owned_item);
        }
    }
    return status == SXML_STATUS_OK
               ? sxml_serializer_write_markup(serializer, SXML_BYTE_VIEW_FROM_LITERAL("\n"))
               : status;
}

/* The value of the entry of map whose key is the string name, or the
   empty sequence. */
typedef struct sxml_csv_entry_query {
    sxml_item map;
    sxml_byte_view name;
} sxml_csv_entry_query;

static sxml_status sxml_csv_find_entry(sxml_csv_entry_query query, sxml_sequence *value) {
    *value = sxml_sequence_empty();
    size_t size = 0;
    sxml_status status = sxml_map_get_size(query.map, &size);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < size;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(query.map, entry_index, &entry);
        if (status != SXML_STATUS_OK || sxml_item_get_kind(entry.key) != SXML_ITEM_KIND_ATOMIC ||
            sxml_atomic_item_get_primitive_type(entry.key).index != SXML_TYPE_XS_STRING) {
            continue;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(entry.key, &datum);
        if (status == SXML_STATUS_OK &&
            sxml_byte_view_equals(datum.datum_details.string.utf8, query.name)) {
            *value = entry.value;
            return SXML_STATUS_OK;
        }
    }
    return status;
}

/* Writes every row of rows, a sequence of arrays (section 10.2). */
static sxml_status sxml_csv_write_rows(sxml_serializer *serializer,
                                       const sxml_csv_delimiters *delimiters, sxml_sequence rows) {
    uint64_t row_count = 0;
    sxml_status status = sxml_sequence_count(rows, serializer->error_record, &row_count);
    for (uint64_t row_index = 0; status == SXML_STATUS_OK && row_index < row_count;
         row_index += 1) {
        sxml_item row = {.representation = 0};
        status = sxml_sequence_get_item(
            rows, (sxml_item_request){.item_index = row_index, .error_record = serializer->error_record},
            &row);
        if (status == SXML_STATUS_OK && sxml_item_get_kind(row) != SXML_ITEM_KIND_ARRAY) {
            status = sxml_csv_raise(serializer);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_csv_write_row(
                serializer, delimiters,
                &(sxml_csv_row){.array = row, .strings = sxml_sequence_empty()});
        }
    }
    return status;
}

sxml_status sxml_serialize_csv_method(sxml_serializer *serializer, sxml_sequence value) {
    const sxml_serialization_parameters *parameters = serializer->parameters;
    const sxml_csv_delimiters delimiters = {
        .separator = sxml_csv_encode(parameters->csv_separator_codepoint == 0
                                         ? (uint32_t)','
                                         : parameters->csv_separator_codepoint),
        .quote = sxml_csv_encode(serializer->csv_quote_codepoint)};
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(value, serializer->error_record, &item_count);
    sxml_item first = {.representation = 0};
    if (status == SXML_STATUS_OK && item_count == 1) {
        status = sxml_sequence_get_item(
            value, (sxml_item_request){.item_index = 0, .error_record = serializer->error_record},
            &first);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count != 1 || sxml_item_get_kind(first) != SXML_ITEM_KIND_MAP) {
        return sxml_csv_write_rows(serializer, &delimiters, value);
    }
    /* Section 10.1: a map castable to record(rows as
       array(xs:anyAtomicType)*, columns as xs:string*). */
    sxml_sequence rows = sxml_sequence_empty();
    sxml_sequence columns = sxml_sequence_empty();
    status = sxml_csv_find_entry(
        (sxml_csv_entry_query){.map = first, .name = SXML_BYTE_VIEW_FROM_LITERAL("rows")}, &rows);
    if (status == SXML_STATUS_OK) {
        status = sxml_csv_find_entry(
            (sxml_csv_entry_query){.map = first, .name = SXML_BYTE_VIEW_FROM_LITERAL("columns")},
            &columns);
    }
    uint64_t column_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(columns, serializer->error_record, &column_count);
    }
    if (status == SXML_STATUS_OK && parameters->csv_header == SXML_SERIALIZATION_FLAG_YES &&
        column_count > 0) {
        status = sxml_csv_write_row(
            serializer, &delimiters,
            &(sxml_csv_row){.array = {.representation = 0}, .strings = columns});
    }
    return status == SXML_STATUS_OK ? sxml_csv_write_rows(serializer, &delimiters, rows) : status;
}
