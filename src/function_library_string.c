/*
 * The string part of the function library (packet 1.20, D78): F&O 4.0,
 * section 5 "Functions on strings" (fn:string, fn:codepoints-to-string,
 * fn:string-to-codepoints, fn:concat, fn:string-join, fn:substring,
 * fn:string-length, fn:normalize-space, fn:upper-case, fn:lower-case,
 * fn:contains, fn:starts-with, fn:ends-with, fn:substring-before,
 * fn:substring-after) and section 6 "String functions that use regular
 * expressions" (fn:matches, fn:replace, fn:tokenize) over the regex seed
 * (D48, D53).
 *
 * Golden file: src/memory.c (layout and comments); rows as in
 * src/function_library_internal.h.
 *
 * Strings are sequences of codepoints (XDM 4.0); every collation argument
 * must name the Unicode codepoint collation (FOCH0002 otherwise), so
 * comparing strings is comparing bytes of UTF-8.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"
#include "regex_internal.h"

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

/* Appends the canonical text of an atomic item (casting to xs:string, F&O
   4.0, section 19.1.2). */
static sxml_status append_atomic_text(sxml_library_text *text, sxml_item item) {
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
   2.1.4), appended to text. A JNode gives the string value of its
   ·jvalue·, when that is a single atomic item or XNode. */
static sxml_status append_string_value(const sxml_function_call *call,
                                       sxml_library_text *text, sxml_item item) {
    sxml_item current = item;
    if (sxml_item_get_kind(current) == SXML_ITEM_KIND_JSON_NODE) {
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
        if (!value.has_item || sxml_item_get_kind(value.item) == SXML_ITEM_KIND_JSON_NODE) {
            return sxml_function_library_report(call, &function_has_no_string_error);
        }
        current = value.item;
    }
    switch (sxml_item_get_kind(current)) {
    case SXML_ITEM_KIND_ATOMIC:
        return append_atomic_text(text, current);
    case SXML_ITEM_KIND_NODE: {
        sxml_node_reference node;
        sxml_status status = sxml_item_get_node(current, &node);
        sxml_item string_value = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_node_make_string_value(call->item_heap, node, &string_value);
        }
        if (status == SXML_STATUS_OK) {
            status = append_atomic_text(text, string_value);
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
            status = append_atomic_text(text, step.item);
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

/* The string value of an "item()?" argument, as fn:string gives it, into
   text (F&O 4.0: fn:string-length and fn:normalize-space accept any
   item). */
static sxml_status read_string_value(const sxml_function_call *call, sxml_library_text *text) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status == SXML_STATUS_OK && value.has_item) {
        status = append_string_value(call, text, value.item);
    }
    return status;
}

/* fn:string-length($value := .) (F&O 4.0, section 5.4.4). */
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

static bool is_xml_whitespace(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* fn:normalize-space($value := .) (F&O 4.0, section 5.4.5). */
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
        if (is_xml_whitespace(byte)) {
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

/* ------------------------------------------------ Substring matching */

/* What a substring function asks about $substring in $value (F&O 4.0,
   section 5.5, with the codepoint collation). */
typedef enum substring_question {
    SXML_SUBSTRING_QUESTION_CONTAINS,
    SXML_SUBSTRING_QUESTION_STARTS_WITH,
    SXML_SUBSTRING_QUESTION_ENDS_WITH,
    SXML_SUBSTRING_QUESTION_BEFORE,
    SXML_SUBSTRING_QUESTION_AFTER,
} substring_question;

/* The byte offset of the first occurrence of needle in haystack, or
   SIZE_MAX. UTF-8 is self-synchronizing, so a byte match is a codepoint
   match. */
static size_t find_bytes(sxml_byte_view haystack, sxml_byte_view needle) {
    if (needle.byte_count == 0) {
        return 0;
    }
    if (needle.byte_count > haystack.byte_count) {
        return SIZE_MAX;
    }
    for (size_t offset = 0; offset + needle.byte_count <= haystack.byte_count;
         offset += 1) {
        if (memcmp(haystack.bytes + offset, needle.bytes, needle.byte_count) == 0) {
            return offset;
        }
    }
    return SIZE_MAX;
}

static sxml_status call_substring_question(const sxml_function_call *call,
                                           substring_question question,
                                           sxml_sequence *result) {
    sxml_string value;
    sxml_string substring;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &substring);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_check_collation(call, 2);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view haystack = value.utf8;
    const sxml_byte_view needle = substring.utf8;
    switch (question) {
    case SXML_SUBSTRING_QUESTION_CONTAINS:
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = find_bytes(haystack, needle) != SIZE_MAX});
        return SXML_STATUS_OK;
    case SXML_SUBSTRING_QUESTION_STARTS_WITH:
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = 
            needle.byte_count <= haystack.byte_count &&
            (needle.byte_count == 0 ||
             memcmp(haystack.bytes, needle.bytes, needle.byte_count) == 0)});
        return SXML_STATUS_OK;
    case SXML_SUBSTRING_QUESTION_ENDS_WITH:
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = 
            needle.byte_count <= haystack.byte_count &&
            (needle.byte_count == 0 ||
             memcmp(haystack.bytes + haystack.byte_count - needle.byte_count, needle.bytes,
                    needle.byte_count) == 0)});
        return SXML_STATUS_OK;
    case SXML_SUBSTRING_QUESTION_BEFORE:
    case SXML_SUBSTRING_QUESTION_AFTER:
        break;
    }
    const size_t offset = find_bytes(haystack, needle);
    sxml_byte_view part = {.bytes = NULL, .byte_count = 0};
    if (offset != SIZE_MAX) {
        part = question == SXML_SUBSTRING_QUESTION_BEFORE
                   ? (sxml_byte_view){.bytes = haystack.bytes, .byte_count = offset}
                   : slice_of(haystack,
                              (byte_part){.byte_offset = offset + needle.byte_count,
                                          .byte_count =
                                              haystack.byte_count - offset - needle.byte_count});
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

static sxml_status call_substring_after(const sxml_function_call *call,
                                        sxml_sequence *result) {
    return call_substring_question(call, SXML_SUBSTRING_QUESTION_AFTER, result);
}

/* ---------------------------------------------- Regular expressions */

/* A compiled $pattern with its matcher, for one call. */
typedef struct pattern_matcher {
    sxml_regex *regex;
    sxml_regex_matcher *matcher;
} pattern_matcher;

typedef struct pattern_request {
    size_t pattern_argument;
    size_t flags_argument;
} pattern_request;

/* Compiles $pattern with $flags (F&O 4.0, sections 6.1 and 6.2): FORX0001
   and FORX0002 into the call's error record. Memory comes from the item
   heap, so it counts against the heap's limits. */
static sxml_status compile_pattern(const sxml_function_call *call, pattern_request request,
                                   pattern_matcher *pattern) {
    *pattern = (pattern_matcher){.regex = NULL, .matcher = NULL};
    sxml_string pattern_text;
    sxml_string flags_text;
    sxml_status status =
        sxml_function_library_read_string_argument(call, request.pattern_argument, &pattern_text);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_function_library_read_string_argument(call, request.flags_argument, &flags_text);
    }
    sxml_regex_flags flags = {.is_dot_all = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_parse_flags(
            &(sxml_regex_flags_request){.flags_text = flags_text,
                                        .error_record = call->error_record},
            &flags);
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_create(
            allocator,
            &(sxml_regex_configuration){.regex_text = pattern_text,
                                        .flags = flags,
                                        .maximum_instruction_count = 0,
                                        .maximum_group_depth = 0,
                                        .stack_allocator = {.state = NULL, .operations = NULL},
                                        .error_record = call->error_record},
            &pattern->regex);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_matcher_create(
            allocator,
            &(sxml_regex_matcher_configuration){.regex = pattern->regex,
                                                .backtrack_allocator = allocator,
                                                .initial_backtrack_entry_count = 0,
                                                .maximum_backtrack_byte_count = 0,
                                                .maximum_step_count = 0},
            &pattern->matcher);
    }
    if (status != SXML_STATUS_OK) {
        sxml_regex_destroy(pattern->regex);
        pattern->regex = NULL;
    }
    return status;
}

static void release_pattern(pattern_matcher *pattern) {
    sxml_regex_matcher_destroy(pattern->matcher);
    sxml_regex_destroy(pattern->regex);
    *pattern = (pattern_matcher){.regex = NULL, .matcher = NULL};
}

/* fn:matches($value, $pattern, $flags := "") (F&O 4.0, section 6.3.1). */
static sxml_status call_matches(const sxml_function_call *call, sxml_sequence *result) {
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    pattern_matcher pattern = {.regex = NULL, .matcher = NULL};
    if (status == SXML_STATUS_OK) {
        status = compile_pattern(
            call, (pattern_request){.pattern_argument = 1, .flags_argument = 2}, &pattern);
    }
    sxml_regex_search_result found = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_matcher_find(
            pattern.matcher,
            &(sxml_regex_search_request){.regex_input = value, .start_byte_offset = 0},
            &found);
    }
    release_pattern(&pattern);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = found.is_found});
    }
    return status;
}

static const sxml_static_error invalid_replacement_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORX0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The replacement string has a \"\\\" not followed by \"\\\" or \"$\", or a \"$\" "
        "not followed by a digit (F&O 4.0, fn:replace)"),
};

/* Appends the replacement of the current match: $N is captured group N
   (the longest number of digits naming a group), \$ and \\ are "$" and
   "\" (F&O 4.0, section 6.3.2); with the q flag the replacement is
   literal. */
static sxml_status append_replacement(const sxml_function_call *call,
                                      const pattern_matcher *pattern,
                                      sxml_library_text *text) {
    sxml_string value;
    sxml_string replacement;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 2, &replacement);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_regex_get_flags(pattern->regex).is_literal) {
        return sxml_function_library_text_append(text, replacement.utf8);
    }
    const sxml_byte_view bytes = replacement.utf8;
    const uint32_t group_count = sxml_regex_group_count(pattern->regex);
    size_t byte_index = 0;
    while (status == SXML_STATUS_OK && byte_index < bytes.byte_count) {
        const uint8_t byte = bytes.bytes[byte_index];
        if (byte == '\\') {
            if (byte_index + 1 >= bytes.byte_count ||
                (bytes.bytes[byte_index + 1] != '\\' && bytes.bytes[byte_index + 1] != '$')) {
                return sxml_function_library_report(call, &invalid_replacement_error);
            }
            status = sxml_function_library_text_append(
                text, slice_of(bytes, (byte_part){.byte_offset = byte_index + 1, .byte_count = 1}));
            byte_index += 2;
            continue;
        }
        if (byte != '$') {
            status = sxml_function_library_text_append(
                text, slice_of(bytes, (byte_part){.byte_offset = byte_index, .byte_count = 1}));
            byte_index += 1;
            continue;
        }
        byte_index += 1;
        if (byte_index >= bytes.byte_count || bytes.bytes[byte_index] < '0' ||
            bytes.bytes[byte_index] > '9') {
            return sxml_function_library_report(call, &invalid_replacement_error);
        }
        uint32_t group_number = (uint32_t)(bytes.bytes[byte_index] - '0');
        byte_index += 1;
        while (byte_index < bytes.byte_count && bytes.bytes[byte_index] >= '0' &&
               bytes.bytes[byte_index] <= '9') {
            const uint32_t longer =
                group_number * 10U + (uint32_t)(bytes.bytes[byte_index] - '0');
            if (longer > group_count) {
                break;
            }
            group_number = longer;
            byte_index += 1;
        }
        if (group_number > group_count) {
            continue;
        }
        sxml_regex_captured_group group;
        status = sxml_regex_matcher_get_captured_group(pattern->matcher, group_number, &group);
        if (status == SXML_STATUS_OK && group.is_captured) {
            status = sxml_function_library_text_append(
                text, slice_of(value.utf8, (byte_part){.byte_offset = group.byte_range.byte_offset,
                                                       .byte_count = group.byte_range.byte_count}));
        }
    }
    return status;
}

/* Checks a replacement string before any match (F&O 4.0, fn:replace:
   FORX0004 is raised even when nothing matches, QT4 fn-replace-338). */
static sxml_status check_replacement(const sxml_function_call *call,
                                     const pattern_matcher *pattern, sxml_string replacement) {
    if (sxml_regex_get_flags(pattern->regex).is_literal) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view bytes = replacement.utf8;
    for (size_t byte_index = 0; byte_index < bytes.byte_count; byte_index += 1) {
        const uint8_t byte = bytes.bytes[byte_index];
        if (byte == '\\') {
            if (byte_index + 1 >= bytes.byte_count ||
                (bytes.bytes[byte_index + 1] != '\\' && bytes.bytes[byte_index + 1] != '$')) {
                return sxml_function_library_report(call, &invalid_replacement_error);
            }
            byte_index += 1;
        } else if (byte == '$' && (byte_index + 1 >= bytes.byte_count ||
                                   bytes.bytes[byte_index + 1] < '0' ||
                                   bytes.bytes[byte_index + 1] > '9')) {
            return sxml_function_library_report(call, &invalid_replacement_error);
        }
    }
    return SXML_STATUS_OK;
}

/*
 * fn:replace($value, $pattern, $replacement, $flags := "") (F&O 4.0,
 * section 6.3.2) as a resumable body (D75): a string replacement is
 * applied in the first step; a function replacement is called once per
 * match, with the matched string and a map from group numbers to the
 * captured strings of the groups that took part, and its result's string
 * value replaces the match. Zero-length matches are replaced too (4.0 no
 * longer raises FORX0003).
 */
typedef struct replace_state {
    pattern_matcher pattern;
    sxml_library_text text;
    sxml_regex_search_result found;
    size_t copied_to;
    /* Owned while a call of the replacement function runs. */
    sxml_sequence call_arguments[2];
    /* The replacement function coerced to its type, owned. */
    sxml_sequence action;
} replace_state;

static void release_replace_state(void *state) {
    replace_state *replace = state;
    release_pattern(&replace->pattern);
    if (replace->text.item_heap != NULL) {
        sxml_function_library_text_release(&replace->text);
    }
    sxml_sequence_release(replace->call_arguments[0]);
    sxml_sequence_release(replace->call_arguments[1]);
    replace->call_arguments[0] = sxml_sequence_empty();
    replace->call_arguments[1] = sxml_sequence_empty();
    sxml_sequence_release(replace->action);
    replace->action = sxml_sequence_empty();
}

/* An xs:untypedAtomic item of utf8: the replacement function receives
   the match and the captured groups untyped (F&O 4.0, section 6.3.2). */
static sxml_status make_untyped(sxml_item_heap *item_heap, sxml_byte_view utf8,
                                sxml_sequence *result) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_UNTYPED_ATOMIC),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = utf8,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

static const sxml_static_error replacement_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The replacement of fn:replace is neither a string nor a function (F&O 4.0, "
        "section 6.3.2)"),
};

/* Appends the input before the current match. */
static sxml_status append_before_match(replace_state *replace, sxml_string value) {
    return sxml_function_library_text_append(
        &replace->text,
        slice_of(value.utf8,
                 (byte_part){.byte_offset = replace->copied_to,
                             .byte_count = replace->found.matching_segment.byte_offset -
                                           replace->copied_to}));
}

/* Moves past the current match to the next one. */
static sxml_status find_next_match(replace_state *replace) {
    replace->copied_to = replace->found.matching_segment.byte_offset +
                         replace->found.matching_segment.byte_count;
    return sxml_regex_matcher_find_next(replace->pattern.matcher, &replace->found);
}

/* Appends the rest of the input and finishes with the text. */
static sxml_status finish_replace(replace_state *replace, sxml_string value,
                                  sxml_native_step *step) {
    sxml_status status = sxml_function_library_text_append(
        &replace->text,
        slice_of(value.utf8, (byte_part){.byte_offset = replace->copied_to,
                                         .byte_count = value.utf8.byte_count - replace->copied_to}));
    sxml_sequence string = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            replace->text.item_heap, sxml_function_library_text_view(&replace->text), &string);
    }
    if (status == SXML_STATUS_OK) {
        *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                                   .result = string,
                                   .function_item = {.representation = 0},
                                   .arguments = {.sequences = NULL, .sequence_count = 0}};
    }
    return status;
}

/* The arguments of a call of the replacement function: the matched
   string and the map of captured groups. */
static sxml_status make_action_arguments(const sxml_function_call *call,
                                         replace_state *replace, sxml_string value) {
    const uint32_t group_count = sxml_regex_group_count(replace->pattern.regex);
    sxml_map_entry entries[64];
    size_t entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t group_number = 1;
         status == SXML_STATUS_OK && group_number <= group_count && entry_count < 64;
         group_number += 1) {
        sxml_regex_captured_group group;
        status = sxml_regex_matcher_get_captured_group(replace->pattern.matcher, group_number,
                                                      &group);
        if (status != SXML_STATUS_OK || !group.is_captured) {
            continue;
        }
        sxml_sequence captured = sxml_sequence_empty();
        status = make_untyped(
            call->item_heap,
            slice_of(value.utf8, (byte_part){.byte_offset = group.byte_range.byte_offset,
                                             .byte_count = group.byte_range.byte_count}),
            &captured);
        if (status == SXML_STATUS_OK) {
            entries[entry_count] = (sxml_map_entry){
                .key = sxml_atomic_item_from_integer(group_number), .value = captured};
            entry_count += 1;
        }
    }
    sxml_item groups = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries, .map_entry_count = entry_count},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                .error_record = call->error_record},
            &groups);
    }
    for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
        sxml_sequence_release(entries[entry_index].value);
    }
    sxml_sequence matched = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = make_untyped(
            call->item_heap,
            slice_of(value.utf8,
                     (byte_part){.byte_offset = replace->found.matching_segment.byte_offset,
                                 .byte_count = replace->found.matching_segment.byte_count}),
            &matched);
        if (status != SXML_STATUS_OK) {
            sxml_item_release(groups);
        }
    }
    if (status == SXML_STATUS_OK) {
        replace->call_arguments[0] = matched;
        replace->call_arguments[1] = sxml_sequence_from_item(groups);
    }
    return status;
}

/* The first step: compile, then replace with a string, or ask for the
   first call of the function. */
static sxml_status start_replace(const sxml_function_call *call, replace_state *replace,
                                 sxml_optional_item *replacement) {
    replace->text = (sxml_library_text){.item_heap = call->item_heap};
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = compile_pattern(
            call, (pattern_request){.pattern_argument = 1, .flags_argument = 3}, &replace->pattern);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 2, replacement);
    }
    const bool is_function = status == SXML_STATUS_OK && replacement->has_item &&
                             sxml_item_is_function_item(replacement->item);
    if (status == SXML_STATUS_OK && replacement->has_item && !is_function &&
        !sxml_operator_item_has_type(call->item_heap,
                                     (sxml_type_test){.item = replacement->item,
                                                      .type = SXML_TYPE_XS_STRING}) &&
        sxml_atomic_item_get_primitive_type(replacement->item).index !=
            SXML_TYPE_XS_UNTYPED_ATOMIC &&
        sxml_atomic_item_get_primitive_type(replacement->item).index != SXML_TYPE_XS_ANY_URI) {
        status = sxml_function_library_report(call, &replacement_type_error);
    }
    if (status == SXML_STATUS_OK && is_function) {
        /* Already coerced to the function type of the parameter (XPath
           4.0, section 3.4.2: function coercion within a choice). */
        replace->action = sxml_sequence_from_item(replacement->item);
        sxml_sequence_retain(replace->action);
    } else if (status == SXML_STATUS_OK) {
        sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                                   .datum_details = {.string = {.utf8 = {.bytes = NULL,
                                                                         .byte_count = 0}}}};
        if (replacement->has_item) {
            status = sxml_atomic_item_get_datum(replacement->item, &datum);
        }
        if (status == SXML_STATUS_OK) {
            status = check_replacement(call, &replace->pattern, datum.datum_details.string);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_matcher_find(
            replace->pattern.matcher,
            &(sxml_regex_search_request){.regex_input = value, .start_byte_offset = 0},
            &replace->found);
    }
    return status;
}

static sxml_status step_replace(const sxml_native_step_request *request,
                                sxml_native_step *step) {
    replace_state *replace = request->state;
    const sxml_function_call *call = request->call;
    sxml_optional_item replacement = {.has_item = false};
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        status = start_replace(call, replace, &replacement);
    } else {
        status = sxml_function_library_read_optional_item(call, 2, &replacement);
        sxml_sequence_release(replace->call_arguments[0]);
        sxml_sequence_release(replace->call_arguments[1]);
        replace->call_arguments[0] = sxml_sequence_empty();
        replace->call_arguments[1] = sxml_sequence_empty();
        sxml_optional_item value = {.has_item = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_read_optional_item(
                &(sxml_function_call){.arguments = {.sequences = &request->call_result,
                                                    .sequence_count = 1},
                                      .error_record = call->error_record},
                0, &value);
        }
        if (status == SXML_STATUS_OK && value.has_item) {
            status = append_string_value(call, &replace->text, value.item);
        }
        sxml_sequence_release(request->call_result);
        if (status == SXML_STATUS_OK) {
            status = find_next_match(replace);
        }
    }
    sxml_string value;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 0, &value);
    }
    const bool is_function = replacement.has_item &&
                             sxml_item_is_function_item(replacement.item);
    while (status == SXML_STATUS_OK && replace->found.is_found && !is_function) {
        status = append_before_match(replace, value);
        if (status == SXML_STATUS_OK) {
            status = append_replacement(call, &replace->pattern, &replace->text);
        }
        if (status == SXML_STATUS_OK) {
            status = find_next_match(replace);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!replace->found.is_found) {
        return finish_replace(replace, value, step);
    }
    status = append_before_match(replace, value);
    if (status == SXML_STATUS_OK) {
        status = make_action_arguments(call, replace, value);
    }
    if (status == SXML_STATUS_OK) {
        *step = (sxml_native_step){
            .step_kind = SXML_NATIVE_STEP_KIND_CALL,
            .result = sxml_sequence_empty(),
            .function_item = replace->action.content,
            .arguments = {.sequences = replace->call_arguments, .sequence_count = 2}};
    }
    return status;
}

static const sxml_resumable_native_body replace_body = {
    .step = step_replace,
    .release_state = release_replace_state,
    .state_byte_count = sizeof(replace_state),
    .state_alignment = alignof(replace_state),
};

/* Appends the token value[start, end) to builder. */
typedef struct token_request {
    sxml_string value;
    size_t start_byte;
    size_t end_byte;
} token_request;

static sxml_status append_token(const sxml_function_call *call,
                                sxml_sequence_builder *builder, token_request token) {
    sxml_sequence string = sxml_sequence_empty();
    sxml_status status = sxml_function_library_make_string(
        call->item_heap,
        slice_of(token.value.utf8, (byte_part){.byte_offset = token.start_byte,
                                               .byte_count = token.end_byte - token.start_byte}),
        &string);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_item(builder, string.content);
        sxml_sequence_release(string);
    }
    return status;
}

/* The tokens between the disjoint matching segments (F&O 4.0, section
   6.3.3, and section 6.1.1): a zero-length segment at the start or the end
   adds no empty token there. */
typedef struct tokenization {
    const pattern_matcher *pattern;
    sxml_string value;
    sxml_sequence_builder *builder;
} tokenization;

static sxml_status tokenize_with_pattern(const sxml_function_call *call,
                                         tokenization request) {
    const pattern_matcher *pattern = request.pattern;
    const sxml_string value = request.value;
    sxml_sequence_builder *builder = request.builder;
    sxml_regex_search_result found = {.is_found = false};
    sxml_status status = sxml_regex_matcher_find(
        pattern->matcher,
        &(sxml_regex_search_request){.regex_input = value, .start_byte_offset = 0}, &found);
    size_t token_start = 0;
    bool is_first = true;
    bool ends_with_empty_segment = false;
    while (status == SXML_STATUS_OK && found.is_found) {
        const sxml_byte_range segment = found.matching_segment;
        const bool is_leading_empty =
            is_first && segment.byte_offset == 0 && segment.byte_count == 0;
        if (!is_leading_empty) {
            status = append_token(call, builder,
                                  (token_request){.value = value,
                                                  .start_byte = token_start,
                                                  .end_byte = segment.byte_offset});
        }
        is_first = false;
        token_start = segment.byte_offset + segment.byte_count;
        ends_with_empty_segment =
            segment.byte_count == 0 && segment.byte_offset == value.utf8.byte_count;
        if (status == SXML_STATUS_OK) {
            status = sxml_regex_matcher_find_next(pattern->matcher, &found);
        }
    }
    if (status == SXML_STATUS_OK && !ends_with_empty_segment) {
        status = append_token(call, builder,
                              (token_request){.value = value,
                                              .start_byte = token_start,
                                              .end_byte = value.utf8.byte_count});
    }
    return status;
}

/* Splits at runs of whitespace after normalizing it: the one-argument
   form, tokenize(normalize-space($value), ' ') (F&O 4.0, section
   6.3.3). */
static sxml_status tokenize_whitespace(const sxml_function_call *call, sxml_string value,
                                       sxml_sequence_builder *builder) {
    sxml_status status = SXML_STATUS_OK;
    size_t byte_index = 0;
    while (status == SXML_STATUS_OK && byte_index < value.utf8.byte_count) {
        while (byte_index < value.utf8.byte_count &&
               is_xml_whitespace(value.utf8.bytes[byte_index])) {
            byte_index += 1;
        }
        const size_t start = byte_index;
        while (byte_index < value.utf8.byte_count &&
               !is_xml_whitespace(value.utf8.bytes[byte_index])) {
            byte_index += 1;
        }
        if (byte_index > start) {
            status = append_token(
                call, builder,
                (token_request){.value = value, .start_byte = start, .end_byte = byte_index});
        }
    }
    return status;
}

/* fn:tokenize($value, $pattern := (), $flags := "") (F&O 4.0, section
   6.3.3). The empty string gives the empty sequence. */
static sxml_status call_tokenize(const sxml_function_call *call, sxml_sequence *result) {
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    sxml_optional_item pattern_argument = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &pattern_argument);
    }
    pattern_matcher pattern = {.regex = NULL, .matcher = NULL};
    if (status == SXML_STATUS_OK && pattern_argument.has_item) {
        status = compile_pattern(
            call, (pattern_request){.pattern_argument = 1, .flags_argument = 2}, &pattern);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    if (status == SXML_STATUS_OK && value.utf8.byte_count > 0) {
        status = pattern_argument.has_item
                     ? tokenize_with_pattern(call, (tokenization){.pattern = &pattern,
                                                                  .value = value,
                                                                  .builder = builder})
                     : tokenize_whitespace(call, value, builder);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    release_pattern(&pattern);
    return status;
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter string_parameters[] = {
    SXML_CONTEXT_PARAMETER("value", "item()?")};
static const sxml_function_parameter codepoints_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:integer*")};
static const sxml_function_parameter optional_string_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?")};
static const sxml_function_parameter context_string_parameters[] = {
    SXML_CONTEXT_PARAMETER("value", "item()?")};
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
static const sxml_function_parameter matches_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("pattern", "xs:string"),
    SXML_OPTIONAL_PARAMETER("flags", "xs:string?")};
static const sxml_function_parameter replace_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("pattern", "xs:string"),
    /* F&O 4.0, section 6.3.2 (the groups map has integer keys only: the
       regex seed has no named groups, D53). */
    SXML_REQUIRED_PARAMETER("replacement",
                            "(xs:string | fn(xs:untypedAtomic, map((xs:integer | xs:string), "
                            "xs:untypedAtomic)) as item()?)?"),
    SXML_OPTIONAL_PARAMETER("flags", "xs:string?")};
static const sxml_function_parameter tokenize_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("pattern", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("flags", "xs:string?")};

static const sxml_native_function string_functions[] = {
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
    SXML_FUNCTION_ROW("matches", matches_parameters, "xs:boolean", 0, call_matches),
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("replace"),
     .parameters = {replace_parameters,
                    sizeof(replace_parameters) / sizeof(replace_parameters[0])},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
     .call = NULL,
     .resumable_body = &replace_body},
    SXML_FUNCTION_ROW("tokenize", tokenize_parameters, "xs:string*", 0, call_tokenize),
};

const sxml_native_function_view sxml_function_library_string =
    SXML_LIBRARY_PART(string_functions);
