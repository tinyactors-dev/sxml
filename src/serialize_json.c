/*
 * The JSON output method (Serialization 4.0, section 9 "JSON Output
 * Method"; docs/DECISIONS.md D82, D120b, D128): maps as objects, arrays as
 * arrays, atomic items as JSON values, nodes as strings by
 * json-node-output-method (a sub-serialization, src/serialize.c), JSON
 * Lines, indentation and RFC 8785 canonical JSON.
 *
 * No recursion (D71): arrays and maps being written are frames on an
 * explicit stack in heap memory. Strings go through the character
 * pipeline with the JSON escape style.
 *
 * Golden file: src/serialize.c (the adaptive method's frames).
 */
#include "serialize_internal.h"

#include "sxml/name.h"
#include "sxml/types.h"
#include "xdm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Frames */

typedef enum sxml_json_frame_kind {
    /* The items of the input with json-lines: one JSON text per line. */
    SXML_JSON_FRAME_KIND_LINES,
    SXML_JSON_FRAME_KIND_ARRAY,
    SXML_JSON_FRAME_KIND_MAP,
} sxml_json_frame_kind;

/* An array, map or JSON Lines sequence being written. */
typedef struct sxml_json_frame {
    sxml_json_frame_kind frame_kind;
    sxml_item container;
    sxml_sequence sequence;
    uint64_t next_index;
    uint64_t count;
    /* For a map: the order its entries are written in (canonical JSON
       sorts them), in heap memory; NULL for the map's own order. */
    size_t *entry_order;
} sxml_json_frame;

typedef struct sxml_json_writer {
    sxml_serializer *serializer;
    bool is_indenting;
    bool is_canonical;
    bool allows_duplicate_names;
    sxml_byte_view indent_unit;
    sxml_json_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
} sxml_json_writer;

static sxml_status sxml_json_raise(sxml_json_writer *writer, sxml_serialization_error error) {
    return sxml_serializer_raise_error(writer->serializer, error);
}

#define SXML_JSON_ERROR(code_literal, description_literal)                     \
    ((sxml_serialization_error){                                               \
        .error_local_name = SXML_BYTE_VIEW_INITIALIZER(code_literal),          \
        .description = SXML_BYTE_VIEW_INITIALIZER(description_literal)})

static sxml_status sxml_json_markup(sxml_json_writer *writer, sxml_byte_view markup) {
    return sxml_serializer_write_markup(writer->serializer, markup);
}

/* A newline and one indent unit per open array or map (9.3.3). */
static sxml_status sxml_json_indent(sxml_json_writer *writer, size_t depth) {
    if (!writer->is_indenting) {
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\n"));
    for (size_t level = 0; status == SXML_STATUS_OK && level < depth; level += 1) {
        status = sxml_json_markup(writer, writer->indent_unit);
    }
    return status;
}

/* The number of open arrays and maps. */
static size_t sxml_json_depth(const sxml_json_writer *writer) {
    size_t depth = 0;
    for (size_t frame_index = 0; frame_index < writer->frame_count; frame_index += 1) {
        depth += writer->frames[frame_index].frame_kind != SXML_JSON_FRAME_KIND_LINES ? 1U : 0U;
    }
    return depth;
}

static sxml_status sxml_json_push(sxml_json_writer *writer, const sxml_json_frame *frame) {
    const sxml_status status = sxml_serializer_grow(
        writer->serializer->allocator,
        &(sxml_serializer_growth){.elements = (void **)&writer->frames,
                                  .element_capacity = &writer->frame_capacity,
                                  .element_count = writer->frame_count,
                                  .element_byte_count = sizeof(sxml_json_frame),
                                  .alignment = alignof(sxml_json_frame)});
    if (status == SXML_STATUS_OK) {
        writer->frames[writer->frame_count] = *frame;
        writer->frame_count += 1;
    }
    return status;
}

/* ------------------------------------------------------------- Strings */

/* Writes text as a JSON string (section 9.1, "serialized to a JSON
   string"). */
static sxml_status sxml_json_write_string(sxml_json_writer *writer, sxml_byte_view text) {
    sxml_status status = sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\""));
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write_character_data(
            writer->serializer,
            &(sxml_character_data){.text = text, .character_context = SXML_CHARACTER_CONTEXT_TEXT});
    }
    return status == SXML_STATUS_OK ? sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\""))
                                    : status;
}

/* Writes the string value of an atomic item (fn:string) as a JSON
   string. */
static sxml_status sxml_json_write_atomic_string(sxml_json_writer *writer, sxml_item item) {
    sxml_atomic_text atomic_text;
    sxml_status status = sxml_serializer_get_atomic_text(writer->serializer, item, &atomic_text);
    if (status == SXML_STATUS_OK) {
        status = sxml_json_write_string(writer, atomic_text.text);
    }
    sxml_serializer_release_atomic_text(writer->serializer, &atomic_text);
    return status;
}

/* ------------------------------------------------------------- Numbers */

/*
 * Writes a finite double as ECMAScript's Number::toString does (RFC 8785,
 * section 3.2.2.3), from the shortest digits of its canonical xs:double
 * text: plain digits for magnitudes from 1e-6 to below 1e21, else
 * d.ddde+N.
 */
static sxml_status sxml_json_write_canonical_number(sxml_json_writer *writer,
                                                    sxml_byte_view canonical) {
    uint8_t digits[SXML_ATOMIC_TEXT_INLINE_CAPACITY];
    size_t digit_count = 0;
    int64_t point_position = 0;
    bool has_point = false;
    bool is_negative = false;
    size_t offset = 0;
    if (offset < canonical.byte_count && canonical.bytes[offset] == '-') {
        is_negative = true;
        offset += 1;
    }
    for (; offset < canonical.byte_count; offset += 1) {
        const uint8_t byte = canonical.bytes[offset];
        if (byte == '.') {
            has_point = true;
        } else if (byte == 'E' || byte == 'e') {
            offset += 1;
            break;
        } else if (digit_count < sizeof(digits)) {
            digits[digit_count] = byte;
            digit_count += 1;
            point_position += has_point ? 0 : 1;
        }
    }
    bool is_exponent_negative = false;
    int64_t exponent = 0;
    if (offset < canonical.byte_count && canonical.bytes[offset] == '-') {
        is_exponent_negative = true;
        offset += 1;
    }
    for (; offset < canonical.byte_count; offset += 1) {
        exponent = exponent * 10 + (canonical.bytes[offset] - '0');
    }
    point_position += is_exponent_negative ? -exponent : exponent;
    size_t first_digit = 0;
    while (first_digit < digit_count && digits[first_digit] == '0') {
        first_digit += 1;
        point_position -= 1;
    }
    while (digit_count > first_digit && digits[digit_count - 1] == '0') {
        digit_count -= 1;
    }
    if (first_digit == digit_count) {
        return sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("0"));
    }
    const uint8_t *significant = digits + first_digit;
    const int64_t count = (int64_t)(digit_count - first_digit);
    const int64_t position = point_position;
    uint8_t output[SXML_ATOMIC_TEXT_INLINE_CAPACITY + 48];
    size_t output_count = 0;
    if (is_negative) {
        output[output_count++] = '-';
    }
    if (count <= position && position <= 21) {
        memcpy(output + output_count, significant, (size_t)count);
        output_count += (size_t)count;
        for (int64_t zero_index = count; zero_index < position; zero_index += 1) {
            output[output_count++] = '0';
        }
    } else if (0 < position && position <= 21) {
        memcpy(output + output_count, significant, (size_t)position);
        output_count += (size_t)position;
        output[output_count++] = '.';
        memcpy(output + output_count, significant + position, (size_t)(count - position));
        output_count += (size_t)(count - position);
    } else if (-6 < position && position <= 0) {
        output[output_count++] = '0';
        output[output_count++] = '.';
        for (int64_t zero_index = position; zero_index < 0; zero_index += 1) {
            output[output_count++] = '0';
        }
        memcpy(output + output_count, significant, (size_t)count);
        output_count += (size_t)count;
    } else {
        output[output_count++] = significant[0];
        if (count > 1) {
            output[output_count++] = '.';
            memcpy(output + output_count, significant + 1, (size_t)(count - 1));
            output_count += (size_t)(count - 1);
        }
        output[output_count++] = 'e';
        int64_t written_exponent = position - 1;
        output[output_count++] = written_exponent < 0 ? '-' : '+';
        written_exponent = written_exponent < 0 ? -written_exponent : written_exponent;
        uint8_t reversed[24];
        size_t reversed_count = 0;
        do {
            reversed[reversed_count++] = (uint8_t)('0' + written_exponent % 10);
            written_exponent /= 10;
        } while (written_exponent != 0);
        while (reversed_count > 0) {
            output[output_count++] = reversed[--reversed_count];
        }
    }
    return sxml_json_markup(writer, (sxml_byte_view){.bytes = output, .byte_count = output_count});
}

/* Writes an xs:float or xs:double (section 9.1): cast to xs:double; NaN
   as null and the infinities as 1e9999 and -1e9999, or SERE0020 when
   canonical. */
static sxml_status sxml_json_write_double(sxml_json_writer *writer, double number) {
    if (isnan(number) || isinf(number)) {
        if (writer->is_canonical) {
            return sxml_json_raise(writer, SXML_JSON_ERROR("SERE0020", "canonical JSON cannot "
                                                                       "represent NaN or "
                                                                       "infinity"));
        }
        if (isnan(number)) {
            return sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("null"));
        }
        return sxml_json_markup(writer, number > 0 ? SXML_BYTE_VIEW_FROM_LITERAL("1e9999")
                                                   : SXML_BYTE_VIEW_FROM_LITERAL("-1e9999"));
    }
    sxml_atomic_text atomic_text;
    sxml_status status = sxml_serializer_get_atomic_text(
        writer->serializer, sxml_atomic_item_from_double(number), &atomic_text);
    if (status == SXML_STATUS_OK) {
        status = writer->is_canonical ? sxml_json_write_canonical_number(writer, atomic_text.text)
                                      : sxml_json_markup(writer, atomic_text.text);
    }
    sxml_serializer_release_atomic_text(writer->serializer, &atomic_text);
    return status;
}

/* ---------------------------------------------------------- Atomic items */

static sxml_status sxml_json_write_atomic(sxml_json_writer *writer, sxml_item item) {
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (sxml_atomic_item_get_primitive_type(item).index) {
    case SXML_TYPE_XS_BOOLEAN:
        return sxml_json_markup(writer, datum.datum_details.boolean
                                            ? SXML_BYTE_VIEW_FROM_LITERAL("true")
                                            : SXML_BYTE_VIEW_FROM_LITERAL("false"));
    case SXML_TYPE_XS_DECIMAL: {
        if (writer->is_canonical) {
            /* Section 9.1: canonical JSON casts every number to
               xs:double. */
            double number = 0;
            if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
                number = (double)datum.datum_details.integer;
            } else {
                char text[128];
                const size_t count = datum.datum_details.decimal_text.byte_count < sizeof(text) - 1
                                         ? datum.datum_details.decimal_text.byte_count
                                         : sizeof(text) - 1;
                memcpy(text, datum.datum_details.decimal_text.bytes, count);
                text[count] = '\0';
                number = strtod(text, NULL);
            }
            return sxml_json_write_double(writer, number);
        }
        sxml_atomic_text atomic_text;
        status = sxml_serializer_get_atomic_text(writer->serializer, item, &atomic_text);
        if (status == SXML_STATUS_OK) {
            status = sxml_json_markup(writer, atomic_text.text);
        }
        sxml_serializer_release_atomic_text(writer->serializer, &atomic_text);
        return status;
    }
    case SXML_TYPE_XS_FLOAT:
        return sxml_json_write_double(writer, (double)datum.datum_details.float_number);
    case SXML_TYPE_XS_DOUBLE:
        return sxml_json_write_double(writer, datum.datum_details.double_number);
    case SXML_TYPE_XS_QNAME: {
        /* Section 9.1: fn:null is the JSON null. */
        sxml_name_parts parts;
        status = sxml_name_pool_get_name_parts(writer->serializer->name_pool,
                                               datum.datum_details.qname.expanded_name, &parts);
        if (status == SXML_STATUS_OK && parts.namespace_uri.index == SXML_NAMESPACE_FUNCTIONS &&
            sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("null"))) {
            return sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("null"));
        }
        return status == SXML_STATUS_OK ? sxml_json_write_atomic_string(writer, item) : status;
    }
    default:
        break;
    }
    return sxml_json_write_atomic_string(writer, item);
}

/* ---------------------------------------------------------------- Maps */

/* The keys of a map as strings, for sorting and the duplicate check. */
typedef struct sxml_json_key_texts {
    sxml_atomic_text *texts;
    size_t count;
} sxml_json_key_texts;

/* Compares two strings by their UTF-16 code units (RFC 8785, section
   3.2.3), which equals bytewise order except that characters above
   U+FFFF sort before U+E000 to U+FFFF. */
static int sxml_json_compare_keys(sxml_byte_view first, sxml_byte_view second) {
    size_t first_offset = 0;
    size_t second_offset = 0;
    while (first_offset < first.byte_count && second_offset < second.byte_count) {
        const uint8_t first_byte = first.bytes[first_offset];
        const uint8_t second_byte = second.bytes[second_offset];
        if (first_byte != second_byte) {
            /* A four-byte sequence (F0-F4) is a surrogate pair, D800 to
               DBFF, below the three-byte sequences from EE (U+E000). */
            const bool first_is_supplementary = first_byte >= 0xF0U;
            const bool second_is_supplementary = second_byte >= 0xF0U;
            if (first_is_supplementary != second_is_supplementary) {
                const uint8_t other = first_is_supplementary ? second_byte : first_byte;
                if (other >= 0xEEU) {
                    return first_is_supplementary ? -1 : 1;
                }
            }
            return first_byte < second_byte ? -1 : 1;
        }
        first_offset += 1;
        second_offset += 1;
    }
    if (first.byte_count - first_offset == second.byte_count - second_offset) {
        return 0;
    }
    return first_offset == first.byte_count ? -1 : 1;
}

/* Sorts order (indices into keys) by heapsort (no recursion, O(n log
   n)). */
static void sxml_json_sort_order(const sxml_json_key_texts *keys, size_t *order) {
    const size_t count = keys->count;
    for (size_t sort_pass = 0; sort_pass < 2 * count; sort_pass += 1) {
        const bool is_building = sort_pass < count;
        const size_t heap_size = is_building ? count : 2 * count - sort_pass;
        size_t root = 0;
        if (is_building) {
            root = count - 1 - sort_pass;
        } else {
            const size_t largest = order[0];
            order[0] = order[heap_size - 1];
            order[heap_size - 1] = largest;
        }
        const size_t sift_size = is_building ? heap_size : heap_size - 1;
        for (;;) {
            size_t largest_index = root;
            const size_t left = 2 * root + 1;
            const size_t right = left + 1;
            if (left < sift_size && sxml_json_compare_keys(keys->texts[order[left]].text,
                                                           keys->texts[order[largest_index]].text) > 0) {
                largest_index = left;
            }
            if (right < sift_size &&
                sxml_json_compare_keys(keys->texts[order[right]].text,
                                       keys->texts[order[largest_index]].text) > 0) {
                largest_index = right;
            }
            if (largest_index == root) {
                break;
            }
            const size_t swapped = order[root];
            order[root] = order[largest_index];
            order[largest_index] = swapped;
            root = largest_index;
        }
    }
}

/* Whether every key of map is a string, xs:anyURI or xs:untypedAtomic:
   then no two keys have the same string value (they are not
   atomic-equal). */
static sxml_status sxml_json_keys_are_strings(sxml_item map, size_t size,
                                              sxml_answer *are_strings_answer) {
    are_strings_answer->is_true = true;
    for (size_t entry_index = 0; are_strings_answer->is_true && entry_index < size;
         entry_index += 1) {
        sxml_map_entry entry;
        const sxml_status status = sxml_map_get_entry(map, entry_index, &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const uint32_t primitive = sxml_atomic_item_get_primitive_type(entry.key).index;
        are_strings_answer->is_true = primitive == SXML_TYPE_XS_STRING ||
                                      primitive == SXML_TYPE_XS_ANY_URI ||
                                      primitive == SXML_TYPE_XS_UNTYPED_ATOMIC;
    }
    return SXML_STATUS_OK;
}

/* Prepares the order of a map's entries: sorted when canonical, and
   checked for keys with the same string value (SERE0022) unless
   allow-duplicate-names. *entry_order stays NULL for the map's order. */
typedef struct sxml_json_map_order {
    sxml_item map;
    size_t size;
} sxml_json_map_order;

static sxml_status sxml_json_order_map(sxml_json_writer *writer, sxml_json_map_order map_order,
                                       size_t **entry_order) {
    *entry_order = NULL;
    const bool checks_duplicates = !writer->allows_duplicate_names || writer->is_canonical;
    sxml_answer are_strings = {.is_true = true};
    sxml_status status = SXML_STATUS_OK;
    if (checks_duplicates && !writer->is_canonical) {
        status = sxml_json_keys_are_strings(map_order.map, map_order.size, &are_strings);
    }
    if (status != SXML_STATUS_OK || map_order.size < 2 ||
        (!writer->is_canonical && (!checks_duplicates || are_strings.is_true))) {
        return status;
    }
    const sxml_allocator allocator = writer->serializer->allocator;
    size_t text_byte_count = 0;
    size_t order_byte_count = 0;
    status = sxml_size_multiply(map_order.size, sizeof(sxml_atomic_text), &text_byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(map_order.size, sizeof(size_t), &order_byte_count);
    }
    void *text_memory = NULL;
    void *order_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = text_byte_count,
                                      .alignment = alignof(sxml_atomic_text)},
            &text_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = order_byte_count, .alignment = alignof(size_t)},
            &order_memory);
    }
    sxml_json_key_texts keys = {.texts = text_memory, .count = 0};
    size_t *order = order_memory;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < map_order.size;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map_order.map, entry_index, &entry);
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_get_atomic_text(writer->serializer, entry.key,
                                                     &keys.texts[entry_index]);
            keys.count = entry_index + 1;
            order[entry_index] = entry_index;
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_json_sort_order(&keys, order);
    }
    for (size_t position = 1; status == SXML_STATUS_OK && checks_duplicates &&
                              position < map_order.size;
         position += 1) {
        if (sxml_json_compare_keys(keys.texts[order[position - 1]].text,
                                   keys.texts[order[position]].text) == 0) {
            status = sxml_json_raise(writer, SXML_JSON_ERROR("SERE0022", "two keys of a map have "
                                                                         "the same string value"));
        }
    }
    for (size_t key_index = 0; key_index < keys.count; key_index += 1) {
        sxml_serializer_release_atomic_text(writer->serializer, &keys.texts[key_index]);
    }
    sxml_allocator_release(allocator, text_memory);
    if (status == SXML_STATUS_OK && writer->is_canonical) {
        *entry_order = order;
    } else {
        sxml_allocator_release(allocator, order_memory);
    }
    return status;
}

/* --------------------------------------------------------------- Values */

/* Writes one value, a sequence (section 9.1): the empty sequence as null,
   one item as itself, more as SERE0023. An array or a map is opened and
   pushed as a frame. */
static sxml_status sxml_json_write_value(sxml_json_writer *writer, sxml_sequence value) {
    sxml_sequence current = value;
    for (;;) {
        uint64_t item_count = 0;
        sxml_status status =
            sxml_sequence_count(current, writer->serializer->error_record, &item_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (item_count == 0) {
            return sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("null"));
        }
        if (item_count > 1) {
            return sxml_json_raise(writer, SXML_JSON_ERROR("SERE0023", "the JSON method cannot "
                                                                       "write a sequence of more "
                                                                       "than one item"));
        }
        sxml_item item = {.representation = 0};
        status = sxml_sequence_get_item(
            current,
            (sxml_item_request){.item_index = 0, .error_record = writer->serializer->error_record},
            &item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        switch (sxml_item_get_kind(item)) {
        case SXML_ITEM_KIND_ATOMIC:
            return sxml_json_write_atomic(writer, item);
        case SXML_ITEM_KIND_NODE: {
            sxml_node_reference node;
            status = sxml_item_get_node(item, &node);
            if (status == SXML_STATUS_OK) {
                status = sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\""));
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_serializer_write_node_as_string(writer->serializer, node);
            }
            return status == SXML_STATUS_OK
                       ? sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\""))
                       : status;
        }
        case SXML_ITEM_KIND_JSON_NODE: {
            /* Section 9.1: a JNode is written as its ·jvalue·. */
            sxml_json_node_description description;
            status = sxml_json_node_describe(item, &description);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            current = description.json_value;
            continue;
        }
        case SXML_ITEM_KIND_ARRAY:
        case SXML_ITEM_KIND_MAP: {
            const bool is_map = sxml_item_get_kind(item) == SXML_ITEM_KIND_MAP;
            size_t size = 0;
            status = is_map ? sxml_map_get_size(item, &size) : sxml_array_get_size(item, &size);
            size_t *entry_order = NULL;
            if (status == SXML_STATUS_OK && is_map) {
                status = sxml_json_order_map(
                    writer, (sxml_json_map_order){.map = item, .size = size}, &entry_order);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_json_markup(writer, is_map ? SXML_BYTE_VIEW_FROM_LITERAL("{")
                                                         : SXML_BYTE_VIEW_FROM_LITERAL("["));
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_json_push(
                    writer, &(sxml_json_frame){.frame_kind = is_map ? SXML_JSON_FRAME_KIND_MAP
                                                                    : SXML_JSON_FRAME_KIND_ARRAY,
                                               .container = item,
                                               .sequence = sxml_sequence_empty(),
                                               .next_index = 0,
                                               .count = size,
                                               .entry_order = entry_order});
            }
            if (status != SXML_STATUS_OK) {
                sxml_allocator_release(writer->serializer->allocator, entry_order);
            }
            return status;
        }
        case SXML_ITEM_KIND_FUNCTION:
            break;
        }
        return sxml_json_raise(writer, SXML_JSON_ERROR("SERE0021", "the JSON method cannot write "
                                                                   "a function item"));
    }
}

/* Runs the top frame one step: its next member, entry or line, or its
   end. */
static sxml_status sxml_json_step(sxml_json_writer *writer) {
    sxml_json_frame *frame = &writer->frames[writer->frame_count - 1];
    if (frame->next_index == frame->count) {
        const sxml_json_frame finished = *frame;
        writer->frame_count -= 1;
        sxml_allocator_release(writer->serializer->allocator, finished.entry_order);
        if (finished.frame_kind == SXML_JSON_FRAME_KIND_LINES) {
            return SXML_STATUS_OK;
        }
        sxml_status status = finished.count > 0 ? sxml_json_indent(writer, sxml_json_depth(writer))
                                                : SXML_STATUS_OK;
        if (status == SXML_STATUS_OK) {
            status = sxml_json_markup(writer, finished.frame_kind == SXML_JSON_FRAME_KIND_MAP
                                                  ? SXML_BYTE_VIEW_FROM_LITERAL("}")
                                                  : SXML_BYTE_VIEW_FROM_LITERAL("]"));
        }
        return status;
    }
    const uint64_t index = frame->next_index;
    frame->next_index += 1;
    const sxml_json_frame current = *frame;
    sxml_status status = SXML_STATUS_OK;
    if (current.frame_kind == SXML_JSON_FRAME_KIND_LINES) {
        /* Section 9.2: one JSON text per line, no newline at the end. */
        if (index > 0) {
            status = sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL("\n"));
        }
        sxml_item item = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                current.sequence,
                (sxml_item_request){.item_index = index,
                                    .error_record = writer->serializer->error_record},
                &item);
        }
        return status == SXML_STATUS_OK
                   ? sxml_json_write_value(writer, sxml_sequence_from_item(item))
                   : status;
    }
    if (index > 0) {
        status = sxml_json_markup(writer, SXML_BYTE_VIEW_FROM_LITERAL(","));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_indent(writer, sxml_json_depth(writer));
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (current.frame_kind == SXML_JSON_FRAME_KIND_ARRAY) {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(current.container, (size_t)index, &member);
        return status == SXML_STATUS_OK ? sxml_json_write_value(writer, member) : status;
    }
    const size_t entry_index =
        current.entry_order != NULL ? current.entry_order[index] : (size_t)index;
    sxml_map_entry entry;
    status = sxml_map_get_entry(current.container, entry_index, &entry);
    if (status == SXML_STATUS_OK) {
        status = sxml_json_write_atomic_string(writer, entry.key);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_markup(writer, writer->is_indenting ? SXML_BYTE_VIEW_FROM_LITERAL(": ")
                                                               : SXML_BYTE_VIEW_FROM_LITERAL(":"));
    }
    return status == SXML_STATUS_OK ? sxml_json_write_value(writer, entry.value) : status;
}

sxml_status sxml_serialize_json_method(sxml_serializer *serializer, sxml_sequence value) {
    const sxml_serialization_parameters *parameters = serializer->parameters;
    const bool is_json_lines = parameters->json_lines == SXML_SERIALIZATION_FLAG_YES;
    sxml_json_writer writer = {
        .serializer = serializer,
        /* Section 9.2: with json-lines no newline is added; sxml adds no
           whitespace then. */
        .is_indenting = parameters->indent == SXML_SERIALIZATION_FLAG_YES && !is_json_lines &&
                        !serializer->is_canonical_json,
        .is_canonical = serializer->is_canonical_json,
        .allows_duplicate_names = parameters->allow_duplicate_names == SXML_SERIALIZATION_FLAG_YES,
        .indent_unit = parameters->has_indent_unit ? parameters->indent_unit
                                                   : SXML_BYTE_VIEW_FROM_LITERAL("  "),
        .frames = NULL,
        .frame_count = 0,
        .frame_capacity = 0,
    };
    sxml_status status = SXML_STATUS_OK;
    if (is_json_lines) {
        uint64_t item_count = 0;
        status = sxml_sequence_count(value, serializer->error_record, &item_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_json_push(&writer, &(sxml_json_frame){
                                                 .frame_kind = SXML_JSON_FRAME_KIND_LINES,
                                                 .container = {.representation = 0},
                                                 .sequence = value,
                                                 .next_index = 0,
                                                 .count = item_count,
                                                 .entry_order = NULL});
        }
    } else {
        status = sxml_json_write_value(&writer, value);
    }
    while (status == SXML_STATUS_OK && writer.frame_count > 0) {
        status = sxml_json_step(&writer);
    }
    for (size_t frame_index = 0; frame_index < writer.frame_count; frame_index += 1) {
        sxml_allocator_release(serializer->allocator, writer.frames[frame_index].entry_order);
    }
    sxml_allocator_release(serializer->allocator, writer.frames);
    return status;
}
