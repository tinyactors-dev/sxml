/*
 * The string part of the function library (packet 1.20, D78): F&O 4.0,
 * section 5 "Functions on strings" (fn:string, fn:codepoints-to-string,
 * fn:string-to-codepoints, fn:concat, fn:string-join, fn:substring,
 * fn:string-length, fn:normalize-space, fn:upper-case, fn:lower-case,
 * fn:contains, fn:starts-with, fn:ends-with, fn:substring-before,
 * fn:substring-after); the functions of section 6 "Regular expressions"
 * moved to src/function_library_regex.c (packet 2.3, D114). From packet
 * 1.23b (D91): fn:substring-before-last,
 * fn:substring-after-last, fn:translate, fn:codepoint-equal,
 * fn:contains-token, fn:trim-space and fn:parse-integer. From packet 2.17:
 * fn:pad-string and fn:index-of-substring (F&O 4.0, sections 5.3.8 and
 * 5.3.10).
 *
 * Golden file: src/memory.c (layout and comments); rows as in
 * src/function_library_internal.h.
 *
 * From packet 2.13 (D116): the collation functions of F&O 4.0, section
 * 5.2 (fn:collation, fn:collation-available, fn:collation-key), and every
 * collation argument through the resolver of src/function_library.c, so
 * substring matching and fn:contains-token honour the collations of
 * src/collation_internal.h.
 *
 * Strings are sequences of codepoints (XDM 4.0); under the Unicode
 * codepoint collation comparing strings is comparing bytes of UTF-8.
 */
#include "cast_internal.h"
#include "decimal_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"
#include "xml_internal.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>

/* The part of bytes from byte_offset, byte_count long (within bytes); no
   pointer arithmetic on the NULL of an empty view. */
typedef struct byte_part {
    size_t byte_offset;
    size_t byte_count;
} byte_part;

static sxml_byte_view slice_of(sxml_byte_view bytes, byte_part part) {
    if (part.byte_count == 0) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = bytes.bytes + part.byte_offset,
                            .byte_count = part.byte_count};
}

/* -------------------------------------------------------- Atomic text */

static const sxml_static_error function_has_no_string_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOTY0014"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A function item, map or array has no string value (F&O 4.0, fn:string)"),
};

static const sxml_static_error jvalue_sequence_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The jvalue of a JNode is a sequence of more than one item, which has no string "
        "value (F&O 4.0, fn:string)"),
};

/* Appends the canonical text of an atomic item (casting to xs:string, F&O
   4.0, section 19.1.2); declared in src/function_library_core_internal.h. */
sxml_status sxml_function_library_text_append_atomic(sxml_library_text *text, sxml_item item) {
    uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    sxml_canonical_text canonical;
    sxml_status status = sxml_cast_write_canonical_text(
        text->item_heap,
        &(sxml_canonical_text_request){
            .item = item, .output = {.bytes = output, .byte_count = sizeof(output)}},
        &canonical);
    if (status == SXML_STATUS_OK) {
        return sxml_function_library_text_append(text, canonical.text);
    }
    if (status != SXML_STATUS_CAPACITY_EXCEEDED) {
        return status;
    }
    /* Long QNames and binary values: write into heap scratch memory. */
    void *memory = NULL;
    status = sxml_item_heap_allocate_bytes(
            text->item_heap,
            (sxml_allocation_request){.byte_count = canonical.required_byte_count,
                                      .alignment = 1},
            &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_cast_write_canonical_text(
        text->item_heap,
        &(sxml_canonical_text_request){
            .item = item,
            .output = {.bytes = memory, .byte_count = canonical.required_byte_count}},
        &canonical);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(text, canonical.text);
    }
    sxml_item_heap_release_bytes(
        text->item_heap, (sxml_heap_bytes){.memory = memory,
                                           .byte_count = canonical.required_byte_count});
    return status;
}

/* The string value of one item as fn:string gives it (F&O 4.0, section
   12.1.6), appended to text. A JNode gives string(jvalue($value)): the
   string value of its ·jvalue·, when that is a single atomic item, XNode
   or JNode (whose own ·jvalue· is then taken, in a loop). */
static sxml_status append_string_value(const sxml_function_call *call,
                                       sxml_library_text *text, sxml_item item) {
    sxml_item current = item;
    while (sxml_item_get_kind(current) == SXML_ITEM_KIND_JSON_NODE) {
        sxml_json_node_description description;
        sxml_status status = sxml_json_node_describe(current, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_optional_item value = {.has_item = false};
        uint64_t item_count = 0;
        status = sxml_sequence_count(description.json_value, call->error_record, &item_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (item_count == 0) {
            return SXML_STATUS_OK;
        }
        if (item_count == 1) {
            value.has_item = true;
            value.item = description.json_value.content;
        }
        if (!value.has_item) {
            return sxml_function_library_report(call, &jvalue_sequence_error);
        }
        current = value.item;
    }
    switch (sxml_item_get_kind(current)) {
    case SXML_ITEM_KIND_ATOMIC:
        return sxml_function_library_text_append_atomic(text, current);
    case SXML_ITEM_KIND_NODE: {
        sxml_node_reference node;
        sxml_status status = sxml_item_get_node(current, &node);
        sxml_item string_value = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_node_make_string_value(call->item_heap, node, &string_value);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append_atomic(text, string_value);
            sxml_item_release(string_value);
        }
        return status;
    }
    case SXML_ITEM_KIND_JSON_NODE:
    case SXML_ITEM_KIND_FUNCTION:
    case SXML_ITEM_KIND_MAP:
    case SXML_ITEM_KIND_ARRAY:
        break;
    }
    return sxml_function_library_report(call, &function_has_no_string_error);
}

/* Makes the string of text and releases text. */
static sxml_status finish_text(sxml_library_text *text, sxml_sequence *result) {
    sxml_status status = sxml_function_library_make_string(
        text->item_heap, sxml_function_library_text_view(text), result);
    sxml_function_library_text_release(text);
    return status;
}

/* fn:string($value := .) (F&O 4.0, section 2.1.4). */
static sxml_status call_string(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (value.has_item &&
        sxml_item_get_type_annotation(value.item).index == SXML_TYPE_XS_STRING) {
        sxml_item_retain(value.item);
        *result = sxml_sequence_from_item(value.item);
        return SXML_STATUS_OK;
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    if (value.has_item) {
        status = append_string_value(call, &text, value.item);
    }
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&text);
        return status;
    }
    return finish_text(&text, result);
}

/* Appends the text of every item of sequence, separated by separator. */
typedef struct join_request {
    sxml_sequence values;
    sxml_byte_view separator;
    /* For several arguments (fn:concat): a separator before the first
       item of this argument too. */
    bool has_items_before;
} join_request;

static sxml_status append_joined(const sxml_function_call *call, sxml_library_text *text,
                                 join_request *request) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(request->values);
    for (;;) {
        sxml_iteration_step step;
        sxml_status status =
            sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            return status;
        }
        if (request->has_items_before) {
            status = sxml_function_library_text_append(text, request->separator);
        }
        request->has_items_before = true;
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append_atomic(text, step.item);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
}

/* fn:concat($values ...) (F&O 4.0, section 5.4.1): every item of every
   argument, cast to xs:string, without separators. */
static sxml_status call_concat(const sxml_function_call *call, sxml_sequence *result) {
    sxml_library_text text = {.item_heap = call->item_heap};
    join_request request = {.values = sxml_sequence_empty(),
                            .separator = {.bytes = NULL, .byte_count = 0},
                            .has_items_before = false};
    sxml_status status = SXML_STATUS_OK;
    for (size_t argument_index = 0;
         status == SXML_STATUS_OK && argument_index < call->arguments.sequence_count;
         argument_index += 1) {
        request.values = call->arguments.sequences[argument_index];
        status = append_joined(call, &text, &request);
    }
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&text);
        return status;
    }
    return finish_text(&text, result);
}

/* fn:string-join($values, $separator := "") (F&O 4.0, section 5.4.2). */
static sxml_status call_string_join(const sxml_function_call *call,
                                    sxml_sequence *result) {
    sxml_string separator;
    sxml_status status = sxml_function_library_read_string_argument(call, 1, &separator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* One step per item joined (D89, D91). */
    status = sxml_function_library_charge_items(call, sxml_function_library_get_argument(call, 0));
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    join_request request = {.values = sxml_function_library_get_argument(call, 0),
                            .separator = separator.utf8,
                            .has_items_before = false};
    status = append_joined(call, &text, &request);
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&text);
        return status;
    }
    return finish_text(&text, result);
}

/* ----------------------------------------------------------- Codepoints */

static const sxml_static_error invalid_codepoint_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCH0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The integer is not the codepoint of an XML 1.1 character (F&O 4.0, "
        "fn:codepoints-to-string)"),
};

/* XML 1.1, production [2] Char: F&O 4.0 accepts the control characters
   from U+0001 (QT4 fn-codepoints-to-string-40-001). */
static bool is_xml_character(int64_t codepoint) {
    return (codepoint >= 0x1 && codepoint <= 0xD7FF) ||
           (codepoint >= 0xE000 && codepoint <= 0xFFFD) ||
           (codepoint >= 0x10000 && codepoint <= 0x10FFFF);
}

/* True when a range of integers holds a codepoint that is not an XML 1.1
   character, so it fails without building its text (QT4
   cbcl-codepoints-to-string-021 asks for 2^32 codepoints). */
typedef struct codepoint_check {
    bool has_invalid;
} codepoint_check;

static sxml_status test_range_codepoints(const sxml_function_call *call,
                                         sxml_sequence values, codepoint_check *check) {
    check->has_invalid = false;
    if (sxml_sequence_get_representation(values) != SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE) {
        return SXML_STATUS_OK;
    }
    uint64_t item_count = 0;
    sxml_item first = {.representation = 0};
    sxml_item last = {.representation = 0};
    sxml_status status = sxml_sequence_count(values, call->error_record, &item_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            values, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
            &first);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            values,
            (sxml_item_request){.item_index = item_count - 1, .error_record = call->error_record},
            &last);
    }
    sxml_atomic_datum first_datum;
    sxml_atomic_datum last_datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(first, &first_datum);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(last, &last_datum);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const int64_t low = first_datum.datum_details.integer;
    const int64_t high = last_datum.datum_details.integer;
    check->has_invalid = first_datum.datum_kind != SXML_DATUM_KIND_INTEGER ||
                   last_datum.datum_kind != SXML_DATUM_KIND_INTEGER || low < 1 || high > 0x10FFFF ||
                   (low <= 0xDFFF && high >= 0xD800) || (low <= 0xFFFF && high >= 0xFFFE);
    return SXML_STATUS_OK;
}

/* fn:codepoints-to-string($values) (F&O 4.0, section 5.2.1). */
static sxml_status call_codepoints_to_string(const sxml_function_call *call,
                                             sxml_sequence *result) {
    codepoint_check check = {.has_invalid = false};
    sxml_status range_status =
        test_range_codepoints(call, sxml_function_library_get_argument(call, 0), &check);
    if (range_status != SXML_STATUS_OK) {
        return range_status;
    }
    if (check.has_invalid) {
        return sxml_function_library_report(call, &invalid_codepoint_error);
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        if (status == SXML_STATUS_OK && (datum.datum_kind != SXML_DATUM_KIND_INTEGER ||
                                         !is_xml_character(datum.datum_details.integer))) {
            status = sxml_function_library_report(call, &invalid_codepoint_error);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append_codepoint(
                &text, (uint32_t)datum.datum_details.integer);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&text);
        return status;
    }
    return finish_text(&text, result);
}

/* fn:string-to-codepoints($value) (F&O 4.0, section 5.2.2). */
static sxml_status call_string_to_codepoints(const sxml_function_call *call,
                                             sxml_sequence *result) {
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    sxml_codepoint_iterator iterator = {.string = value, .byte_offset = 0};
    uint32_t codepoint = 0;
    while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        status = sxml_sequence_builder_append_item(builder,
                                                   sxml_atomic_item_from_integer(codepoint));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* ------------------------------------------------------------ Substrings */

/* The byte offset of codepoint position (from 0) codepoint_index. */
static size_t byte_offset_of(sxml_string string, size_t codepoint_index) {
    if (string.is_ascii) {
        return codepoint_index;
    }
    sxml_codepoint_iterator iterator = {.string = string, .byte_offset = 0};
    uint32_t codepoint = 0;
    for (size_t index = 0; index < codepoint_index; index += 1) {
        if (!sxml_codepoint_iterator_next(&iterator, &codepoint)) {
            break;
        }
    }
    return iterator.byte_offset;
}

/* fn:substring($value, $start, $length := ()) (F&O 4.0, section 5.4.3):
   the codepoints at positions p with round($start) <= p < round($start) +
   round($length); $start and $length are xs:numeric (integers exact). */
static sxml_status call_substring(const sxml_function_call *call,
                                  sxml_sequence *result) {
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    sxml_position_span positions = {.is_empty = true};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_position_span(
            call, (sxml_position_arguments){.start_argument = 1, .length_argument = 2},
            &positions);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t codepoint_count = value.codepoint_count;
    const uint64_t end_index =
        positions.has_end && positions.end_index < codepoint_count ? positions.end_index
                                                                   : codepoint_count;
    if (positions.is_empty || positions.first_index >= end_index) {
        return sxml_function_library_make_string(
            call->item_heap, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, result);
    }
    const size_t start_byte = byte_offset_of(value, (size_t)positions.first_index);
    const size_t end_byte = byte_offset_of(value, (size_t)end_index);
    return sxml_function_library_make_string(
        call->item_heap,
        slice_of(value.utf8,
                 (byte_part){.byte_offset = start_byte, .byte_count = end_byte - start_byte}),
        result);
}

/* The argument of fn:string-length or fn:normalize-space, an
   "xs:anyAtomicType?" (an explicit argument atomized, the default
   fn:string(.)), cast to xs:string, into text. */
static sxml_status read_string_value(const sxml_function_call *call, sxml_library_text *text) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status == SXML_STATUS_OK && value.has_item) {
        status = append_string_value(call, text, value.item);
    }
    return status;
}

/* fn:string-length($value := fn:string(.)) (F&O 4.0, section 5.3.11). */
static sxml_status call_string_length(const sxml_function_call *call,
                                      sxml_sequence *result) {
    sxml_library_text text = {.item_heap = call->item_heap};
    sxml_status status = read_string_value(call, &text);
    sxml_utf8_validation validation = {.codepoint_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_utf8_validate(sxml_function_library_text_view(&text), &validation);
    }
    sxml_function_library_text_release(&text);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(
            sxml_atomic_item_from_integer((int64_t)validation.codepoint_count));
    }
    return status;
}

/* fn:normalize-space($value := fn:string(.)) (F&O 4.0, section 5.3.12). */
static sxml_status call_normalize_space(const sxml_function_call *call,
                                        sxml_sequence *result) {
    sxml_library_text input = {.item_heap = call->item_heap};
    sxml_status status = read_string_value(call, &input);
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&input);
        return status;
    }
    const sxml_string value = {.utf8 = sxml_function_library_text_view(&input),
                               .codepoint_count = 0,
                               .is_ascii = false};
    sxml_library_text text = {.item_heap = call->item_heap};
    bool is_pending_space = false;
    for (size_t byte_index = 0;
         status == SXML_STATUS_OK && byte_index < value.utf8.byte_count; byte_index += 1) {
        const uint8_t byte = value.utf8.bytes[byte_index];
        if (sxml_xml_byte_is_whitespace(byte)) {
            is_pending_space = text.byte_count > 0;
            continue;
        }
        if (is_pending_space) {
            status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL(" "));
            is_pending_space = false;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(
                &text, slice_of(value.utf8, (byte_part){.byte_offset = byte_index, .byte_count = 1}));
        }
    }
    sxml_function_library_text_release(&input);
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&text);
        return status;
    }
    return finish_text(&text, result);
}

/* fn:upper-case and fn:lower-case (F&O 4.0, sections 5.4.7 and 5.4.8). */
static sxml_status call_case(const sxml_function_call *call,
                             sxml_case_mapping_kind mapping_kind, sxml_sequence *result) {
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    sxml_codepoint_iterator iterator = {.string = value, .byte_offset = 0};
    uint32_t codepoint = 0;
    while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        sxml_case_mapping mapping;
        sxml_function_library_map_case(
            (sxml_case_mapping_request){.codepoint = codepoint, .mapping_kind = mapping_kind},
            &mapping);
        for (size_t mapped_index = 0;
             status == SXML_STATUS_OK && mapped_index < mapping.codepoint_count;
             mapped_index += 1) {
            status = sxml_function_library_text_append_codepoint(
                &text, mapping.codepoints[mapped_index]);
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&text);
        return status;
    }
    return finish_text(&text, result);
}

static sxml_status call_upper_case(const sxml_function_call *call,
                                   sxml_sequence *result) {
    return call_case(call, SXML_CASE_MAPPING_KIND_UPPER, result);
}

static sxml_status call_lower_case(const sxml_function_call *call,
                                   sxml_sequence *result) {
    return call_case(call, SXML_CASE_MAPPING_KIND_LOWER, result);
}

/* -------------------------------------------------------- String options */

/* A string option (F&O 4.0, section 1.7): the value coerced to xs:string. */
#define SXML_STRING_OPTION(name_text)                                         \
    SXML_FUNCTION_OPTION(name_text, SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE)

/* Which sides fn:trim-space strips or fn:pad-string pads. */
typedef struct string_sides {
    bool has_start;
    bool has_end;
} string_sides;

/* Reads a 'side' value, one of 'start', 'end' and 'both'; anything else
   is option_error. */
typedef struct side_request {
    sxml_byte_view side_text;
    const sxml_static_error *option_error;
} side_request;

static sxml_status read_side(const sxml_function_call *call, side_request request,
                             string_sides *sides) {
    if (sxml_byte_view_equals(request.side_text, SXML_BYTE_VIEW_FROM_LITERAL("start"))) {
        *sides = (string_sides){.has_start = true, .has_end = false};
    } else if (sxml_byte_view_equals(request.side_text, SXML_BYTE_VIEW_FROM_LITERAL("end"))) {
        *sides = (string_sides){.has_start = false, .has_end = true};
    } else if (sxml_byte_view_equals(request.side_text, SXML_BYTE_VIEW_FROM_LITERAL("both"))) {
        *sides = (string_sides){.has_start = true, .has_end = true};
    } else {
        return sxml_function_library_report(call, request.option_error);
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- fn:trim-space */

static const sxml_static_error trim_option_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The options of fn:trim-space take only 'side', one of 'start', 'end' or 'both' "
        "(F&O 4.0, fn:trim-space; section 1.7, option parameter conventions)"),
};

static const sxml_option_definition trim_options[] = {SXML_STRING_OPTION("side")};

/* Reads the 'side' option of argument 1 (F&O 4.0, fn:trim-space). */
static sxml_status read_trim_sides(const sxml_function_call *call, string_sides *sides) {
    *sides = (string_sides){.has_start = true, .has_end = true};
    sxml_option_values values = {.is_present = {false}};
    sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 1,
                               .definitions = {trim_options, 1}},
        &values);
    if (status == SXML_STATUS_OK && values.is_present[0]) {
        status = read_side(
            call,
            (side_request){.side_text = sxml_function_library_option_string(
                                            (sxml_option_lookup){.values = &values,
                                                                 .option_index = 0})
                                            .utf8,
                           .option_error = &trim_option_error},
            sides);
    }
    sxml_function_library_release_options(&values);
    return status;
}

static bool is_trimmed_whitespace(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* fn:trim-space($value := fn:string(.), $options := {}) (F&O 4.0, section
   5.4.6): the string without leading and trailing whitespace (U+0020,
   U+0009, U+000D, U+000A), on the sides the 'side' option names. */
static sxml_status call_trim_space(const sxml_function_call *call, sxml_sequence *result) {
    string_sides sides;
    sxml_status status = read_trim_sides(call, &sides);
    sxml_library_text text = {.item_heap = call->item_heap};
    if (status == SXML_STATUS_OK) {
        status = read_string_value(call, &text);
    }
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&text);
        return status;
    }
    const sxml_byte_view value = sxml_function_library_text_view(&text);
    size_t first = 0;
    size_t end = value.byte_count;
    while (sides.has_start && first < end && is_trimmed_whitespace(value.bytes[first])) {
        first += 1;
    }
    while (sides.has_end && end > first && is_trimmed_whitespace(value.bytes[end - 1])) {
        end -= 1;
    }
    status = sxml_function_library_make_string(
        call->item_heap, slice_of(value, (byte_part){.byte_offset = first, .byte_count = end - first}),
        result);
    sxml_function_library_text_release(&text);
    return status;
}

/* ------------------------------------------------------- fn:pad-string */

static const sxml_static_error pad_option_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The options of fn:pad-string are 'padding', a string, and 'side', one of 'start', "
        "'end' or 'both' (F&O 4.0, fn:pad-string; section 1.7, option parameter conventions)"),
};

static const sxml_static_error empty_padding_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The padding option of fn:pad-string is a zero-length string (F&O 4.0, "
        "fn:pad-string)"),
};

static const sxml_option_definition pad_options[] = {SXML_STRING_OPTION("padding"),
                                                    SXML_STRING_OPTION("side")};

/* A run of padding: codepoint_count characters of the repeated padding,
   truncated on the right (F&O 4.0, fn:pad-string). */
typedef struct padding_run {
    sxml_string padding;
    uint64_t codepoint_count;
} padding_run;

static sxml_status append_padding(sxml_library_text *text, padding_run run) {
    sxml_status status = SXML_STATUS_OK;
    uint64_t remaining = run.codepoint_count;
    while (status == SXML_STATUS_OK && remaining >= run.padding.codepoint_count) {
        status = sxml_function_library_text_append(text, run.padding.utf8);
        remaining -= run.padding.codepoint_count;
    }
    if (status == SXML_STATUS_OK && remaining > 0) {
        status = sxml_function_library_text_append(
            text, slice_of(run.padding.utf8,
                           (byte_part){.byte_offset = 0,
                                       .byte_count = byte_offset_of(run.padding,
                                                                    (size_t)remaining)}));
    }
    return status;
}

/* Pads argument 0 to argument 1 characters with padding on sides. */
typedef struct pad_request {
    sxml_string padding;
    string_sides sides;
} pad_request;

static sxml_status pad(const sxml_function_call *call, const pad_request *request,
                       sxml_sequence *result) {
    const sxml_string padding = request->padding;
    const string_sides sides = request->sides;
    sxml_optional_item value = {.has_item = false};
    sxml_optional_item length = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &length);
    }
    sxml_library_text input = {.item_heap = call->item_heap};
    if (status == SXML_STATUS_OK && value.has_item) {
        status = sxml_function_library_text_append_atomic(&input, value.item);
    }
    sxml_utf8_validation validation = {.codepoint_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_utf8_validate(sxml_function_library_text_view(&input), &validation);
    }
    sxml_atomic_datum length_datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                                      .datum_details = {.integer = 0}};
    if (status == SXML_STATUS_OK && length.has_item) {
        status = sxml_atomic_item_get_datum(length.item, &length_datum);
    }
    /* An integer beyond int64_t asks for more characters than memory
       holds: the step bound or the heap stops it. */
    const uint64_t target_count =
        length_datum.datum_kind == SXML_DATUM_KIND_INTEGER
            ? (length_datum.datum_details.integer < 0 ? 0
                                                      : (uint64_t)length_datum.datum_details.integer)
            : (length_datum.datum_details.decimal_text.bytes[0] == '-' ? 0 : UINT64_MAX);
    const uint64_t padding_count =
        target_count > validation.codepoint_count ? target_count - validation.codepoint_count : 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, padding_count);
    }
    const uint64_t start_count =
        sides.has_start ? (sides.has_end ? padding_count / 2 : padding_count) : 0;
    sxml_library_text text = {.item_heap = call->item_heap};
    if (status == SXML_STATUS_OK) {
        status = append_padding(&text, (padding_run){.padding = padding,
                                                     .codepoint_count = start_count});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&text, sxml_function_library_text_view(&input));
    }
    if (status == SXML_STATUS_OK) {
        status = append_padding(&text, (padding_run){.padding = padding,
                                                     .codepoint_count = padding_count - start_count});
    }
    sxml_function_library_text_release(&input);
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&text);
        return status;
    }
    return finish_text(&text, result);
}

/* fn:pad-string($value, $length, $options := {}) (F&O 4.0, section
   5.3.8): $value cast to xs:string, extended to $length characters with
   the repeated 'padding' (" ") on the 'side' ("end") option names; with
   "both", each side's run is formed on its own and the end side gets the
   odd character. Never truncates. */
static sxml_status call_pad_string(const sxml_function_call *call, sxml_sequence *result) {
    sxml_option_values options = {.is_present = {false}};
    sxml_status status = sxml_function_library_read_options(
        call, &(sxml_option_request){.argument_index = 2, .definitions = {pad_options, 2}},
        &options);
    string_sides sides = {.has_start = false, .has_end = true};
    if (status == SXML_STATUS_OK && options.is_present[1]) {
        status = read_side(
            call,
            (side_request){.side_text = sxml_function_library_option_string(
                                            (sxml_option_lookup){.values = &options,
                                                                 .option_index = 1})
                                            .utf8,
                           .option_error = &pad_option_error},
            &sides);
    }
    const sxml_string padding =
        options.is_present[0]
            ? sxml_function_library_option_string(
                  (sxml_option_lookup){.values = &options, .option_index = 0})
            : (sxml_string){.utf8 = SXML_BYTE_VIEW_FROM_LITERAL(" "),
                            .codepoint_count = 1,
                            .is_ascii = true};
    if (status == SXML_STATUS_OK && padding.codepoint_count == 0) {
        status = sxml_function_library_report(call, &empty_padding_error);
    }
    status = status == SXML_STATUS_OK ? pad(call, &(pad_request){.padding = padding, .sides = sides}, result) : status;
    sxml_function_library_release_options(&options);
    return status;
}

/* ------------------------------------------------ fn:index-of-substring */

/* fn:index-of-substring($value, $substring) (F&O 4.0, section 5.3.10):
   the positions, counted in characters from 1, of every occurrence of
   $substring in $value, overlapping ones included, compared codepoint by
   codepoint; a zero-length $substring occurs at every position from 1 to
   string-length($value) + 1. */
static sxml_status call_index_of_substring(const sxml_function_call *call,
                                           sxml_sequence *result) {
    sxml_string value;
    sxml_string substring;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &substring);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, value.utf8.byte_count);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    sxml_codepoint_iterator iterator = {.string = value, .byte_offset = 0};
    int64_t position = 1;
    while (status == SXML_STATUS_OK) {
        const size_t byte_offset = iterator.byte_offset;
        if (value.utf8.byte_count - byte_offset < substring.utf8.byte_count) {
            break;
        }
        if (substring.utf8.byte_count == 0 ||
            memcmp(value.utf8.bytes + byte_offset, substring.utf8.bytes,
                   substring.utf8.byte_count) == 0) {
            status = sxml_sequence_builder_append_item(builder,
                                                       sxml_atomic_item_from_integer(position));
        }
        uint32_t codepoint = 0;
        if (!sxml_codepoint_iterator_next(&iterator, &codepoint)) {
            break;
        }
        position += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* ----------------------------------------------------- fn:parse-integer */

static const sxml_static_error radix_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0011"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The radix is not in the range 2 to 36 (F&O 4.0, fn:parse-integer)"),
};

static const sxml_static_error digits_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0012"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The value has no digits, or a character that is not a digit of the radix (F&O 4.0, "
        "fn:parse-integer)"),
};

/* The value of a generalized digit (0-9, a-z, A-Z), or 36 for none. */
static uint32_t digit_value(uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        return (uint32_t)(byte - '0');
    }
    if (byte >= 'a' && byte <= 'z') {
        return (uint32_t)(byte - 'a') + 10U;
    }
    if (byte >= 'A' && byte <= 'Z') {
        return (uint32_t)(byte - 'A') + 10U;
    }
    return 36U;
}

/* An integer being accumulated: in int64_t while it fits, then as a
   decimal. */
typedef struct integer_accumulator {
    bool is_decimal;
    int64_t small_value;
    sxml_decimal large_value;
} integer_accumulator;

/* accumulator := accumulator * radix + digit, exactly. */
typedef struct digit_step {
    uint32_t radix;
    uint32_t digit;
} digit_step;

static sxml_status accumulate_digit(const sxml_decimal_configuration *configuration,
                                    integer_accumulator *accumulator, digit_step step) {
    const int64_t radix = (int64_t)step.radix;
    const int64_t digit = (int64_t)step.digit;
    if (!accumulator->is_decimal) {
        if (accumulator->small_value <= (INT64_MAX - digit) / radix) {
            accumulator->small_value = accumulator->small_value * radix + digit;
            return SXML_STATUS_OK;
        }
        sxml_status status = sxml_decimal_from_integer(configuration, accumulator->small_value,
                                                       &accumulator->large_value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        accumulator->is_decimal = true;
    }
    sxml_decimal radix_decimal;
    sxml_status status = sxml_decimal_from_integer(configuration, radix, &radix_decimal);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal product;
    status = sxml_decimal_multiply(
        configuration,
        &(sxml_decimal_operands){.left_operand = &accumulator->large_value,
                                 .right_operand = &radix_decimal},
        &product);
    sxml_decimal_finalize(&radix_decimal);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal digit_decimal;
    status = sxml_decimal_from_integer(configuration, digit, &digit_decimal);
    sxml_decimal sum;
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_add(configuration,
                                  &(sxml_decimal_operands){.left_operand = &product,
                                                           .right_operand = &digit_decimal},
                                  &sum);
        sxml_decimal_finalize(&digit_decimal);
    }
    sxml_decimal_finalize(&product);
    if (status == SXML_STATUS_OK) {
        sxml_decimal_finalize(&accumulator->large_value);
        accumulator->large_value = sum;
    }
    return status;
}

/* Makes the xs:integer of accumulator (negated when is_negative), taking
   its decimal. */
typedef struct signed_accumulator {
    integer_accumulator *accumulator;
    bool is_negative;
} signed_accumulator;

static sxml_status make_accumulated_integer(const sxml_function_call *call,
                                            signed_accumulator value, sxml_sequence *result) {
    integer_accumulator *accumulator = value.accumulator;
    if (!accumulator->is_decimal) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer(
            value.is_negative ? -accumulator->small_value : accumulator->small_value));
        return SXML_STATUS_OK;
    }
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(call->item_heap);
    sxml_status status = SXML_STATUS_OK;
    if (value.is_negative) {
        sxml_decimal negated;
        status = sxml_decimal_negate(configuration, &accumulator->large_value, &negated);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_decimal_finalize(&accumulator->large_value);
        accumulator->large_value = negated;
    }
    sxml_item item = {.representation = 0};
    status = sxml_atomic_item_make_taking_decimal(
        call->item_heap,
        &(sxml_decimal_item_request){.type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                                     .decimal = &accumulator->large_value},
        &item);
    if (status == SXML_STATUS_OK) {
        accumulator->is_decimal = false;
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:parse-integer($value, $radix := 10) (F&O 4.0, section 4.6.2):
   whitespace and underscores are stripped anywhere, then an optional sign
   and one or more digits of the radix (FORG0012); the radix is 2 to 36
   (FORG0011). */
static sxml_status call_parse_integer(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value_item;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value_item);
    sxml_optional_item radix_item;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &radix_item);
    }
    int64_t radix = 10;
    if (status == SXML_STATUS_OK && radix_item.has_item) {
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(radix_item.item, &datum);
        radix = status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_INTEGER
                    ? datum.datum_details.integer
                    : 0;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (radix < 2 || radix > 36) {
        return sxml_function_library_report(call, &radix_error);
    }
    if (!value_item.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_string value;
    status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(call->item_heap);
    integer_accumulator accumulator = {.is_decimal = false, .small_value = 0};
    bool is_negative = false;
    bool has_sign = false;
    bool has_digit = false;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < value.utf8.byte_count;
         byte_index += 1) {
        const uint8_t byte = value.utf8.bytes[byte_index];
        if (is_trimmed_whitespace(byte) || byte == '_') {
            continue;
        }
        if ((byte == '+' || byte == '-') && !has_sign && !has_digit) {
            has_sign = true;
            is_negative = byte == '-';
            continue;
        }
        const uint32_t digit = digit_value(byte);
        if (digit >= (uint32_t)radix) {
            status = sxml_function_library_report(call, &digits_error);
            break;
        }
        has_digit = true;
        status = accumulate_digit(configuration, &accumulator,
                                  (digit_step){.radix = (uint32_t)radix, .digit = digit});
    }
    if (status == SXML_STATUS_OK && !has_digit) {
        status = sxml_function_library_report(call, &digits_error);
    }
    if (status == SXML_STATUS_OK) {
        status = make_accumulated_integer(
            call, (signed_accumulator){.accumulator = &accumulator, .is_negative = is_negative},
            result);
    }
    if (accumulator.is_decimal) {
        sxml_decimal_finalize(&accumulator.large_value);
    }
    return status;
}

/* ------------------------------------------------ Substring matching */

/* What a substring function asks about $substring in $value (F&O 4.0,
   section 5.5, with the codepoint collation). */
typedef enum substring_question {
    SXML_SUBSTRING_QUESTION_CONTAINS,
    SXML_SUBSTRING_QUESTION_STARTS_WITH,
    SXML_SUBSTRING_QUESTION_ENDS_WITH,
    SXML_SUBSTRING_QUESTION_BEFORE,
    SXML_SUBSTRING_QUESTION_AFTER,
    /* fn:substring-before-last and fn:substring-after-last (F&O 4.0,
       sections 5.5.6 and 5.5.7; packet 1.23b). */
    SXML_SUBSTRING_QUESTION_BEFORE_LAST,
    SXML_SUBSTRING_QUESTION_AFTER_LAST,
} substring_question;

/* The match kind each question looks for. */
static sxml_collation_match_kind match_kind_of(substring_question question) {
    switch (question) {
    case SXML_SUBSTRING_QUESTION_STARTS_WITH:
        return SXML_COLLATION_MATCH_KIND_START;
    case SXML_SUBSTRING_QUESTION_ENDS_WITH:
        return SXML_COLLATION_MATCH_KIND_END;
    case SXML_SUBSTRING_QUESTION_BEFORE_LAST:
    case SXML_SUBSTRING_QUESTION_AFTER_LAST:
        return SXML_COLLATION_MATCH_KIND_LAST;
    case SXML_SUBSTRING_QUESTION_CONTAINS:
    case SXML_SUBSTRING_QUESTION_BEFORE:
    case SXML_SUBSTRING_QUESTION_AFTER:
        break;
    }
    return SXML_COLLATION_MATCH_KIND_FIRST;
}

/* The functions of F&O 4.0, section 5.4: the minimal match of $substring
   in $value under the collation (sxml_collation_find_match); a
   zero-length $substring matches at the start (the end for the "-last"
   functions). */
static sxml_status call_substring_question(const sxml_function_call *call,
                                           substring_question question,
                                           sxml_sequence *result) {
    sxml_string value;
    sxml_string substring;
    sxml_collation collation;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &substring);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_resolve_collation(call, 2, &collation);
    }
    sxml_collation_match match = {.is_found = false, .byte_offset = 0, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_collation_find_match(
            &(sxml_collation_match_request){
                .collation = &collation,
                .allocator = sxml_item_heap_as_allocator(call->item_heap),
                .input = value,
                .search = substring,
                .match_kind = match_kind_of(question)},
            &match);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view haystack = value.utf8;
    switch (question) {
    case SXML_SUBSTRING_QUESTION_CONTAINS:
    case SXML_SUBSTRING_QUESTION_STARTS_WITH:
    case SXML_SUBSTRING_QUESTION_ENDS_WITH:
        *result = sxml_function_library_make_boolean(
            (sxml_boolean_result){.is_true = match.is_found});
        return SXML_STATUS_OK;
    case SXML_SUBSTRING_QUESTION_BEFORE:
    case SXML_SUBSTRING_QUESTION_AFTER:
    case SXML_SUBSTRING_QUESTION_BEFORE_LAST:
    case SXML_SUBSTRING_QUESTION_AFTER_LAST:
        break;
    }
    sxml_byte_view part = {.bytes = NULL, .byte_count = 0};
    if (match.is_found) {
        const size_t match_end = match.byte_offset + match.byte_count;
        part = question == SXML_SUBSTRING_QUESTION_BEFORE ||
                       question == SXML_SUBSTRING_QUESTION_BEFORE_LAST
                   ? slice_of(haystack, (byte_part){.byte_offset = 0,
                                                    .byte_count = match.byte_offset})
                   : slice_of(haystack, (byte_part){.byte_offset = match_end,
                                                    .byte_count = haystack.byte_count - match_end});
    }
    return sxml_function_library_make_string(call->item_heap, part, result);
}

static sxml_status call_contains(const sxml_function_call *call, sxml_sequence *result) {
    return call_substring_question(call, SXML_SUBSTRING_QUESTION_CONTAINS, result);
}

static sxml_status call_starts_with(const sxml_function_call *call,
                                    sxml_sequence *result) {
    return call_substring_question(call, SXML_SUBSTRING_QUESTION_STARTS_WITH, result);
}

static sxml_status call_ends_with(const sxml_function_call *call, sxml_sequence *result) {
    return call_substring_question(call, SXML_SUBSTRING_QUESTION_ENDS_WITH, result);
}

static sxml_status call_substring_before(const sxml_function_call *call,
                                         sxml_sequence *result) {
    return call_substring_question(call, SXML_SUBSTRING_QUESTION_BEFORE, result);
}

static sxml_status call_substring_before_last(const sxml_function_call *call,
                                              sxml_sequence *result) {
    return call_substring_question(call, SXML_SUBSTRING_QUESTION_BEFORE_LAST, result);
}

static sxml_status call_substring_after_last(const sxml_function_call *call,
                                             sxml_sequence *result) {
    return call_substring_question(call, SXML_SUBSTRING_QUESTION_AFTER_LAST, result);
}

/* ------------------------------------------------------ Character maps */

/* The codepoint at codepoint_position (from 0) of text, or false when text
   is shorter. */
typedef struct codepoint_lookup {
    bool is_found;
    uint32_t codepoint;
} codepoint_lookup;

static codepoint_lookup find_codepoint_at(sxml_string text, size_t codepoint_position) {
    sxml_codepoint_iterator iterator = {.string = text, .byte_offset = 0};
    uint32_t codepoint = 0;
    size_t position = 0;
    while (sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        if (position == codepoint_position) {
            return (codepoint_lookup){.is_found = true, .codepoint = codepoint};
        }
        position += 1;
    }
    return (codepoint_lookup){.is_found = false, .codepoint = 0};
}

/* The position (from 0) of the first occurrence of codepoint in text, or
   SIZE_MAX. */
static size_t find_codepoint_position(sxml_string text, uint32_t codepoint) {
    sxml_codepoint_iterator iterator = {.string = text, .byte_offset = 0};
    uint32_t candidate = 0;
    size_t position = 0;
    while (sxml_codepoint_iterator_next(&iterator, &candidate)) {
        if (candidate == codepoint) {
            return position;
        }
        position += 1;
    }
    return SIZE_MAX;
}

/* fn:translate($value, $replace, $with) (F&O 4.0, section 5.4.9): each
   character of $value found first at position M of $replace becomes the
   character at M of $with, or is dropped when $with is shorter. */
static sxml_status call_translate(const sxml_function_call *call, sxml_sequence *result) {
    sxml_string value;
    sxml_string replace;
    sxml_string with;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &replace);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 2, &with);
    }
    if (status == SXML_STATUS_OK) {
        /* One step per pair of characters compared (D89, D91). */
        status = sxml_function_library_charge_steps(
            call, (uint64_t)value.utf8.byte_count * ((uint64_t)replace.utf8.byte_count + 1));
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    sxml_codepoint_iterator iterator = {.string = value, .byte_offset = 0};
    uint32_t codepoint = 0;
    while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        const size_t position = find_codepoint_position(replace, codepoint);
        if (position == SIZE_MAX) {
            status = sxml_function_library_text_append_codepoint(&text, codepoint);
            continue;
        }
        const codepoint_lookup replacement = find_codepoint_at(with, position);
        if (replacement.is_found) {
            status = sxml_function_library_text_append_codepoint(&text, replacement.codepoint);
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_function_library_text_release(&text);
        return status;
    }
    return finish_text(&text, result);
}

/* fn:codepoint-equal($value1, $value2) (F&O 4.0, section 5.3.7): the
   empty sequence when either is empty. */
static sxml_status call_codepoint_equal(const sxml_function_call *call,
                                        sxml_sequence *result) {
    sxml_optional_item first;
    sxml_optional_item second;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &second);
    }
    if (status != SXML_STATUS_OK || !first.has_item || !second.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    sxml_string first_text;
    sxml_string second_text;
    status = sxml_function_library_read_string_argument(call, 0, &first_text);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &second_text);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){
            .is_true = sxml_byte_view_equals(first_text.utf8, second_text.utf8)});
    }
    return status;
}

/* A token to look for and the collation to compare under. */
typedef struct token_search {
    const sxml_function_call *call;
    const sxml_collation *collation;
    sxml_byte_view token;
} token_search;

/* What find_token found. */
typedef struct token_finding {
    bool is_found;
} token_finding;

/* Whether a whitespace-separated token of text equals search.token under
   the collation (compare($t, $token, $collation) eq 0, F&O 4.0,
   fn:contains-token), in *finding. */
static sxml_status find_token(token_search search, sxml_byte_view text, token_finding *finding) {
    size_t offset = 0;
    finding->is_found = false;
    while (offset < text.byte_count && !finding->is_found) {
        while (offset < text.byte_count && sxml_xml_byte_is_whitespace(text.bytes[offset])) {
            offset += 1;
        }
        size_t end = offset;
        while (end < text.byte_count && !sxml_xml_byte_is_whitespace(text.bytes[end])) {
            end += 1;
        }
        const sxml_byte_view candidate =
            slice_of(text, (byte_part){.byte_offset = offset, .byte_count = end - offset});
        offset = end;
        if (candidate.byte_count == 0) {
            continue;
        }
        if (sxml_collation_is_codepoint_order(search.collation)) {
            finding->is_found = sxml_byte_view_equals(candidate, search.token);
            continue;
        }
        sxml_string first;
        sxml_string second;
        sxml_status status = sxml_string_from_utf8(candidate, &first);
        if (status == SXML_STATUS_OK) {
            status = sxml_string_from_utf8(search.token, &second);
        }
        sxml_ordering ordering = SXML_ORDERING_LESS;
        if (status == SXML_STATUS_OK) {
            status = sxml_collation_compare(
                &(sxml_collation_comparison){
                    .collation = search.collation,
                    .allocator = sxml_item_heap_as_allocator(search.call->item_heap),
                    .first = first,
                    .second = second},
                &ordering);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        finding->is_found = ordering == SXML_ORDERING_EQUAL;
    }
    return SXML_STATUS_OK;
}

/* fn:contains-token($value, $token, $collation) (F&O 4.0, section 5.4.1):
   true when some item of $value, split at whitespace, has a token equal
   under the collation to $token with its whitespace trimmed; false for an
   empty token. */
static sxml_status call_contains_token(const sxml_function_call *call,
                                       sxml_sequence *result) {
    sxml_string token;
    sxml_collation collation;
    sxml_status status = sxml_function_library_read_string_argument(call, 1, &token);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_resolve_collation(call, 2, &collation);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view trimmed = sxml_xml_text_trim_whitespace(token.utf8);
    token_finding finding = {.is_found = false};
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    while (trimmed.byte_count > 0 && !finding.is_found) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_charge_steps(call, 1);
        }
        if (status == SXML_STATUS_OK) {
            status = find_token(
                (token_search){.call = call, .collation = &collation, .token = trimmed},
                datum.datum_details.string.utf8, &finding);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean(
            (sxml_boolean_result){.is_true = finding.is_found});
    }
    return status;
}

/* ------------------------------------------------------------ Collations */

/* fn:collation-available($collation) (F&O 4.0, section 5.2.10): whether
   the collation URI is one sxml recognizes, after the rules of section
   5.2.7 (a relative URI against the static base URI). */
static sxml_status call_collation_available(const sxml_function_call *call,
                                            sxml_sequence *result) {
    sxml_string collation_uri;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &collation_uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_collation collation;
    /* The resolver reports FOCH0002 for what it does not recognize; that
       is the answer false, not an error. */
    status = sxml_function_library_find_collation(
        &(sxml_function_call){.function_item = call->function_item,
                              .arguments = call->arguments,
                              .dynamic_context = call->dynamic_context,
                              .item_heap = call->item_heap,
                              .error_record = NULL},
        collation_uri.utf8, &collation);
    if (status != SXML_STATUS_OK && status != SXML_STATUS_DYNAMIC_ERROR) {
        return status;
    }
    *result = sxml_function_library_make_boolean(
        (sxml_boolean_result){.is_true = status == SXML_STATUS_OK});
    return SXML_STATUS_OK;
}

/* fn:collation-key($value, $collation := default) (F&O 4.0, section
   5.2.11): the collation key as xs:base64Binary, whose order as binary
   values is the order of the strings (sxml_collation_make_key). */
static sxml_status call_collation_key(const sxml_function_call *call, sxml_sequence *result) {
    sxml_string value;
    sxml_collation collation;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_resolve_collation(call, 1, &collation);
    }
    sxml_collation_key key = {.allocator = sxml_item_heap_as_allocator(call->item_heap),
                              .bytes = NULL};
    if (status == SXML_STATUS_OK) {
        status = sxml_collation_make_key(
            &(sxml_collation_key_request){.collation = &collation, .input = value}, &key);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_BASE64_BINARY),
                .datum = {.datum_kind = SXML_DATUM_KIND_BINARY,
                          .datum_details = {.binary = sxml_collation_key_view(&key)}}},
            &item);
    }
    sxml_collation_key_release(&key);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

static const sxml_static_error collation_option_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An option of fn:collation has a key that is not a string or a value that is "
        "not a single atomic item (F&O 4.0, section 5.2.9)"),
};

/* append_collation_parameter after the value's atomization. */
static sxml_status append_collation_entry(const sxml_function_call *call,
                                          sxml_library_text *text, sxml_map_entry entry) {
    sxml_atomic_datum key_datum;
    if (sxml_atomic_item_get_datum(entry.key, &key_datum) != SXML_STATUS_OK ||
        key_datum.datum_kind != SXML_DATUM_KIND_STRING ||
        sxml_sequence_get_representation(entry.value) !=
            SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_item_get_kind(entry.value.content) != SXML_ITEM_KIND_ATOMIC) {
        return sxml_function_library_report(call, &collation_option_error);
    }
    sxml_status status = sxml_function_library_text_append(text, key_datum.datum_details.string.utf8);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("="));
    }
    sxml_atomic_datum value_datum;
    if (status == SXML_STATUS_OK &&
        sxml_atomic_item_get_datum(entry.value.content, &value_datum) == SXML_STATUS_OK &&
        value_datum.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
        return sxml_function_library_text_append(
            text, value_datum.datum_details.boolean ? SXML_BYTE_VIEW_FROM_LITERAL("yes")
                                                     : SXML_BYTE_VIEW_FROM_LITERAL("no"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append_atomic(text, entry.value.content);
    }
    return status;
}

/* Appends one keyword=value parameter for a map entry of fn:collation:
   the key's string, "=", and the value's string, with true and false as
   yes and no. A node value is atomized first, as the option parameter
   conventions' coercion does (fn-contains-47: an attribute). */
static sxml_status append_collation_parameter(const sxml_function_call *call,
                                              sxml_library_text *text,
                                              sxml_map_entry entry) {
    sxml_sequence atomized = sxml_sequence_empty();
    if (call->dynamic_context != NULL &&
        sxml_sequence_get_representation(entry.value) ==
            SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_item_get_kind(entry.value.content) == SXML_ITEM_KIND_NODE) {
        const sxml_status status = sxml_evaluator_atomize(
            call->dynamic_context,
            &(sxml_atomization_request){.value = entry.value, .error_record = call->error_record},
            &atomized);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        entry.value = atomized;
    }
    const sxml_status status = append_collation_entry(call, text, entry);
    sxml_sequence_release(atomized);
    return status;
}

/* fn:collation($options) (F&O 4.0, section 5.2.9): the UCA collation URI
   with one keyword=value parameter per entry, in entry order. */
static sxml_status call_collation(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item options;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &options);
    size_t entry_count = 0;
    if (status == SXML_STATUS_OK && options.has_item) {
        status = sxml_map_get_size(options.item, &entry_count);
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(
            &text, SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/2013/collation/UCA"));
    }
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(options.item, entry_index, &entry);
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(
                &text, entry_index == 0 ? SXML_BYTE_VIEW_FROM_LITERAL("?")
                                        : SXML_BYTE_VIEW_FROM_LITERAL(";"));
        }
        if (status == SXML_STATUS_OK) {
            status = append_collation_parameter(call, &text, entry);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

static sxml_status call_substring_after(const sxml_function_call *call,
                                        sxml_sequence *result) {
    return call_substring_question(call, SXML_SUBSTRING_QUESTION_AFTER, result);
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter string_parameters[] = {
    SXML_CONTEXT_PARAMETER("value", "item()?")};
static const sxml_function_parameter codepoints_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:integer*")};
static const sxml_function_parameter optional_string_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?")};
/* fn:string-length and fn:normalize-space: "$value as xs:anyAtomicType?
   := fn:string(.)" (F&O 4.0, 5.3.11 and 5.3.12). */
static const sxml_function_parameter context_string_parameters[] = {
    SXML_CONTEXT_STRING_PARAMETER("value", "xs:anyAtomicType?")};
static const sxml_function_parameter concat_parameters[] = {
    SXML_OPTIONAL_PARAMETER("values", "xs:anyAtomicType*")};
static const sxml_function_parameter string_join_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:anyAtomicType*"),
    SXML_OPTIONAL_PARAMETER("separator", "xs:string?")};
static const sxml_function_parameter substring_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("start", "xs:numeric"),
    SXML_OPTIONAL_PARAMETER("length", "xs:numeric?")};
static const sxml_function_parameter substring_question_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("substring", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?")};
static const sxml_function_parameter trim_space_parameters[] = {
    SXML_CONTEXT_PARAMETER("value", "xs:anyAtomicType?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?")};
static const sxml_function_parameter parse_integer_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("radix", "xs:integer?")};
static const sxml_function_parameter translate_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("replace", "xs:string"),
    SXML_REQUIRED_PARAMETER("with", "xs:string")};
static const sxml_function_parameter codepoint_equal_parameters[] = {
    SXML_REQUIRED_PARAMETER("value1", "xs:string?"),
    SXML_REQUIRED_PARAMETER("value2", "xs:string?")};
static const sxml_function_parameter contains_token_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string*"),
    SXML_REQUIRED_PARAMETER("token", "xs:string"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?")};
static const sxml_function_parameter collation_parameters[] = {
    SXML_REQUIRED_PARAMETER("options", "map(*)")};
static const sxml_function_parameter collation_available_parameters[] = {
    SXML_REQUIRED_PARAMETER("collation", "xs:string")};
static const sxml_function_parameter collation_key_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?")};
static const sxml_function_parameter pad_string_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:anyAtomicType?"),
    SXML_REQUIRED_PARAMETER("length", "xs:integer"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?")};
static const sxml_function_parameter index_of_substring_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("substring", "xs:string?")};
static const sxml_native_function string_functions[] = {
    SXML_FUNCTION_ROW("collation", collation_parameters, "xs:string", 0, call_collation),
    SXML_FUNCTION_ROW("collation-available", collation_available_parameters, "xs:boolean", 0,
                      call_collation_available),
    SXML_FUNCTION_ROW("collation-key", collation_key_parameters, "xs:base64Binary", 0,
                      call_collation_key),
    SXML_FUNCTION_ROW("string", string_parameters, "xs:string", 0, call_string),
    SXML_FUNCTION_ROW("codepoints-to-string", codepoints_parameters, "xs:string", 0,
                      call_codepoints_to_string),
    SXML_FUNCTION_ROW("string-to-codepoints", optional_string_parameters, "xs:integer*", 0,
                      call_string_to_codepoints),
    SXML_FUNCTION_ROW("concat", concat_parameters, "xs:string",
                      SXML_NATIVE_FUNCTION_IS_VARIADIC, call_concat),
    SXML_FUNCTION_ROW("string-join", string_join_parameters, "xs:string", 0,
                      call_string_join),
    SXML_FUNCTION_ROW("substring", substring_parameters, "xs:string", 0, call_substring),
    SXML_FUNCTION_ROW("string-length", context_string_parameters, "xs:integer", 0,
                      call_string_length),
    SXML_FUNCTION_ROW("normalize-space", context_string_parameters, "xs:string", 0,
                      call_normalize_space),
    SXML_FUNCTION_ROW("upper-case", optional_string_parameters, "xs:string", 0,
                      call_upper_case),
    SXML_FUNCTION_ROW("lower-case", optional_string_parameters, "xs:string", 0,
                      call_lower_case),
    SXML_FUNCTION_ROW("contains", substring_question_parameters, "xs:boolean", 0,
                      call_contains),
    SXML_FUNCTION_ROW("starts-with", substring_question_parameters, "xs:boolean", 0,
                      call_starts_with),
    SXML_FUNCTION_ROW("ends-with", substring_question_parameters, "xs:boolean", 0,
                      call_ends_with),
    SXML_FUNCTION_ROW("substring-before", substring_question_parameters, "xs:string", 0,
                      call_substring_before),
    SXML_FUNCTION_ROW("substring-after", substring_question_parameters, "xs:string", 0,
                      call_substring_after),
    SXML_FUNCTION_ROW("substring-before-last", substring_question_parameters, "xs:string", 0,
                      call_substring_before_last),
    SXML_FUNCTION_ROW("substring-after-last", substring_question_parameters, "xs:string", 0,
                      call_substring_after_last),
    SXML_FUNCTION_ROW("translate", translate_parameters, "xs:string", 0, call_translate),
    SXML_FUNCTION_ROW("trim-space", trim_space_parameters, "xs:string", 0, call_trim_space),
    SXML_FUNCTION_ROW("parse-integer", parse_integer_parameters, "xs:integer?", 0,
                      call_parse_integer),
    SXML_FUNCTION_ROW("codepoint-equal", codepoint_equal_parameters, "xs:boolean?", 0,
                      call_codepoint_equal),
    SXML_FUNCTION_ROW("contains-token", contains_token_parameters, "xs:boolean", 0,
                      call_contains_token),
    SXML_FUNCTION_ROW("pad-string", pad_string_parameters, "xs:string", 0, call_pad_string),
    SXML_FUNCTION_ROW("index-of-substring", index_of_substring_parameters, "xs:integer*", 0,
                      call_index_of_substring),
};

const sxml_native_function_view sxml_function_library_string =
    SXML_LIBRARY_PART(string_functions);
