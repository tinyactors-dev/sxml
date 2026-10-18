/*
 * The regular expression part of the function library (packet 2.3, D105,
 * D114): the functions of F&O 4.0, section 6.3 "Functions using regular
 * expressions": fn:matches, fn:replace, fn:tokenize (moved here from
 * src/function_library_string.c), fn:analyze-string,
 * fn:matching-segments and fn:regex, over the engine of
 * src/regex_internal.h.
 *
 * Golden files: src/function_library_math.c (rows, a part found by glob,
 * D101) and src/function_library_higher_order.c (the resumable body of
 * fn:replace with a function replacement, D75; function items with
 * captured values, as fn:partial-apply makes them).
 *
 * Positions in results are codepoint positions from 1 (F&O 4.0, section
 * 6.1.1); the engine works in byte offsets, so a position is counted from
 * the last one known. The record that fn:regex returns holds function
 * items whose bodies are the rows of record_function_rows below: they
 * capture the pattern and the flags and compile them for each call.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"
#include "regex_internal.h"
#include "xslt_number_internal.h"

#include <stdalign.h>
#include <string.h>

/* --------------------------------------------------------------- Helpers */

/* True for the empty sequence (a captured value or the unset action). */
static bool is_empty_sequence(sxml_sequence sequence) {
    return sxml_sequence_get_representation(sequence) == SXML_SEQUENCE_REPRESENTATION_EMPTY;
}

/* The part of bytes from byte_offset, byte_count long; no pointer
   arithmetic on the NULL of an empty view. */
static sxml_byte_view slice_of(sxml_byte_view bytes, sxml_byte_range range) {
    if (range.byte_count == 0) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = bytes.bytes + range.byte_offset,
                            .byte_count = range.byte_count};
}

/* A codepoint position known for a byte offset of the regex input: the
   start of a count to a later offset. */
typedef struct codepoint_cursor {
    size_t byte_offset;
    uint64_t position;
} codepoint_cursor;

/* The position (from 1) of the codepoint at byte_offset, counted from
   cursor, which lies at or before it. */
static uint64_t position_at(sxml_string value, codepoint_cursor cursor,
                            size_t byte_offset) {
    uint64_t position = cursor.position;
    for (size_t byte_index = cursor.byte_offset; byte_index < byte_offset;
         byte_index += 1) {
        if ((value.utf8.bytes[byte_index] & 0xC0U) != 0x80U) {
            position += 1;
        }
    }
    return position;
}

/* Writes the decimal digits of number into digits (at least 20 bytes)
   and returns their view. */
typedef struct decimal_digits {
    uint8_t bytes[20];
} decimal_digits;

static sxml_byte_view write_decimal(uint64_t number, decimal_digits *digits) {
    size_t first_index = sizeof(digits->bytes);
    uint64_t rest = number;
    do {
        first_index -= 1;
        digits->bytes[first_index] = (uint8_t)('0' + rest % 10U);
        rest /= 10U;
    } while (rest != 0);
    return (sxml_byte_view){.bytes = digits->bytes + first_index,
                            .byte_count = sizeof(digits->bytes) - first_index};
}

/* ------------------------------------------------------------- Patterns */

/* A compiled $pattern with its matcher, for one call. */
typedef struct pattern_matcher {
    sxml_regex *regex;
    sxml_regex_matcher *matcher;
} pattern_matcher;

/* The texts of a regex: $pattern and $flags. */
typedef struct regex_source {
    sxml_string pattern_text;
    sxml_string flags_text;
} regex_source;

/* Compiles a regex (F&O 4.0, sections 6.1 and 6.2): FORX0001 and FORX0002
   into the call's error record. Memory comes from the item heap, so it
   counts against the heap's limits. */
static sxml_status compile_pattern(const sxml_function_call *call, regex_source source,
                                   pattern_matcher *pattern) {
    *pattern = (pattern_matcher){.regex = NULL, .matcher = NULL};
    sxml_regex_flags flags = {.is_dot_all = false};
    sxml_status status = sxml_regex_parse_flags(
        &(sxml_regex_flags_request){.flags_text = source.flags_text,
                                    .error_record = call->error_record},
        &flags);
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_create(
            allocator,
            &(sxml_regex_configuration){.regex_text = source.pattern_text,
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

/* After a search that gave search_status: charges the instructions it ran
   to the evaluation (D89, D91), so a bound on evaluation steps also bounds
   regular expressions that run without tasks. */
static sxml_status charge_search(const sxml_function_call *call,
                                 const pattern_matcher *pattern, sxml_status search_status) {
    if (search_status != SXML_STATUS_OK) {
        return search_status;
    }
    return sxml_function_library_charge_steps(
        call, sxml_regex_matcher_get_step_count(pattern->matcher));
}

/* A compiled pattern and the regex input it searches. */
typedef struct search_start {
    const pattern_matcher *pattern;
    sxml_string value;
} search_start;

/* The first search of start.value. */
static sxml_status find_first(const sxml_function_call *call, search_start start,
                              sxml_regex_search_result *found) {
    return charge_search(
        call, start.pattern,
        sxml_regex_matcher_find(
            start.pattern->matcher,
            &(sxml_regex_search_request){.regex_input = start.value, .start_byte_offset = 0},
            found));
}

static sxml_status find_next(const sxml_function_call *call, const pattern_matcher *pattern,
                             sxml_regex_search_result *found) {
    return charge_search(call, pattern, sxml_regex_matcher_find_next(pattern->matcher, found));
}

/* ------------------------------------------------------------ Arguments */

/*
 * Where a call finds its inputs: a static call of fn:matches and its
 * kin reads $value, $pattern and $flags as arguments; a function of a
 * fn:regex record reads $value as its first argument and the pattern and
 * flags from what it captured (captured values 0 and 1).
 */
typedef struct call_shape {
    bool is_record_function;
    /* For a static call: the argument that holds $flags. */
    size_t flags_argument;
    /* fn:matching-segments only: the non-matching segments are given too,
       as xs:string items between the records (analyze-segments). */
    bool includes_gaps;
} call_shape;

/* The inputs of a call. */
typedef struct regex_inputs {
    sxml_string value;
    regex_source source;
} regex_inputs;

/* Reads an xs:string item of a captured value; the empty string for the
   empty sequence. */
static sxml_status read_captured_string(sxml_sequence captured, sxml_string *string) {
    *string = (sxml_string){.utf8 = {.bytes = NULL, .byte_count = 0},
                            .codepoint_count = 0,
                            .is_ascii = true};
    if (is_empty_sequence(captured)) {
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_STRING};
    sxml_status status = sxml_atomic_item_get_datum(captured.content, &datum);
    if (status == SXML_STATUS_OK) {
        *string = datum.datum_details.string;
    }
    return status;
}

static sxml_status read_inputs(const sxml_function_call *call, call_shape shape,
                               regex_inputs *inputs) {
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &inputs->value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!shape.is_record_function) {
        status = sxml_function_library_read_string_argument(call, 1,
                                                            &inputs->source.pattern_text);
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_read_string_argument(call, shape.flags_argument,
                                                                &inputs->source.flags_text);
        }
        return status;
    }
    sxml_function_item_description description;
    status = sxml_function_item_describe(call->function_item, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.captured_values.sequence_count != 2) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    status = read_captured_string(description.captured_values.sequences[0],
                                  &inputs->source.pattern_text);
    if (status == SXML_STATUS_OK) {
        status = read_captured_string(description.captured_values.sequences[1],
                                      &inputs->source.flags_text);
    }
    return status;
}

/* ------------------------------------------------------------ fn:matches */

/* fn:matches($value, $pattern, $flags := "") (F&O 4.0, section 6.3.1). */
static sxml_status matches_with(const sxml_function_call *call, call_shape shape,
                                sxml_sequence *result) {
    regex_inputs inputs;
    sxml_status status = read_inputs(call, shape, &inputs);
    pattern_matcher pattern = {.regex = NULL, .matcher = NULL};
    if (status == SXML_STATUS_OK) {
        status = compile_pattern(call, inputs.source, &pattern);
    }
    sxml_regex_search_result found = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = find_first(call, (search_start){.pattern = &pattern, .value = inputs.value}, &found);
    }
    release_pattern(&pattern);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = found.is_found});
    }
    return status;
}

static sxml_status call_matches(const sxml_function_call *call, sxml_sequence *result) {
    return matches_with(call, (call_shape){.is_record_function = false, .flags_argument = 2},
                        result);
}

static sxml_status call_record_matches(const sxml_function_call *call, sxml_sequence *result) {
    return matches_with(call, (call_shape){.is_record_function = true, .flags_argument = 0},
                        result);
}

/* ----------------------------------------------------------- fn:tokenize */

/* Appends the token value[start, end) to builder. */
typedef struct token_request {
    sxml_string value;
    size_t start_byte;
    size_t end_byte;
} token_request;

static sxml_status append_token(const sxml_function_call *call, sxml_sequence_builder *builder,
                                token_request token) {
    sxml_sequence string = sxml_sequence_empty();
    sxml_status status = sxml_function_library_make_string(
        call->item_heap,
        slice_of(token.value.utf8,
                 (sxml_byte_range){.byte_offset = token.start_byte,
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

static sxml_status tokenize_with_pattern(const sxml_function_call *call, tokenization request) {
    const sxml_string value = request.value;
    sxml_regex_search_result found = {.is_found = false};
    sxml_status status = find_first(call, (search_start){.pattern = request.pattern, .value = value}, &found);
    size_t token_start = 0;
    bool is_first = true;
    bool ends_with_empty_segment = false;
    while (status == SXML_STATUS_OK && found.is_found) {
        const sxml_byte_range segment = found.matching_segment;
        const bool is_leading_empty =
            is_first && segment.byte_offset == 0 && segment.byte_count == 0;
        if (!is_leading_empty) {
            status = append_token(call, request.builder,
                                  (token_request){.value = value,
                                                  .start_byte = token_start,
                                                  .end_byte = segment.byte_offset});
        }
        is_first = false;
        token_start = segment.byte_offset + segment.byte_count;
        ends_with_empty_segment =
            segment.byte_count == 0 && segment.byte_offset == value.utf8.byte_count;
        if (status == SXML_STATUS_OK) {
            status = find_next(call, request.pattern, &found);
        }
    }
    if (status == SXML_STATUS_OK && !ends_with_empty_segment) {
        status = append_token(call, request.builder,
                              (token_request){.value = value,
                                              .start_byte = token_start,
                                              .end_byte = value.utf8.byte_count});
    }
    return status;
}

static bool is_xml_whitespace(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
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
static sxml_status tokenize_with(const sxml_function_call *call, call_shape shape,
                                 sxml_sequence *result) {
    regex_inputs inputs;
    sxml_status status = read_inputs(call, shape, &inputs);
    bool has_pattern = shape.is_record_function;
    if (status == SXML_STATUS_OK && !shape.is_record_function) {
        sxml_optional_item pattern_argument = {.has_item = false};
        status = sxml_function_library_read_optional_item(call, 1, &pattern_argument);
        has_pattern = pattern_argument.has_item;
    }
    pattern_matcher pattern = {.regex = NULL, .matcher = NULL};
    if (status == SXML_STATUS_OK && has_pattern) {
        status = compile_pattern(call, inputs.source, &pattern);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    if (status == SXML_STATUS_OK && inputs.value.utf8.byte_count > 0) {
        status = has_pattern ? tokenize_with_pattern(call, (tokenization){.pattern = &pattern,
                                                                         .value = inputs.value,
                                                                         .builder = builder})
                             : tokenize_whitespace(call, inputs.value, builder);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    release_pattern(&pattern);
    return status;
}

static sxml_status call_tokenize(const sxml_function_call *call, sxml_sequence *result) {
    return tokenize_with(call, (call_shape){.is_record_function = false, .flags_argument = 2},
                         result);
}

static sxml_status call_record_tokenize(const sxml_function_call *call, sxml_sequence *result) {
    return tokenize_with(call, (call_shape){.is_record_function = true, .flags_argument = 0},
                         result);
}

/* ------------------------------------------------------------ fn:replace */

static const sxml_static_error invalid_replacement_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORX0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The replacement string has a \"\\\" not followed by \"\\\" or \"$\", or a \"$\" "
        "not followed by a digit or by \"<\", a group name and \">\" (F&O 4.0, fn:replace)"),
};

/* A variable marker $<name> of a replacement string at byte_offset (just
   after the "$"): the group it names and where the marker ends. */
typedef struct named_marker {
    uint32_t group_number;
    size_t end_byte_offset;
} named_marker;

/* Reads $<name> (F&O 4.0, section 6.3.2): FORX0004 unless the name is the
   name of a named capturing subexpression. */
typedef struct marker_request {
    const pattern_matcher *pattern;
    /* The replacement string. */
    sxml_byte_view bytes;
    /* Of the "<". */
    size_t byte_offset;
} marker_request;

static sxml_status read_named_marker(const sxml_function_call *call, marker_request request,
                                     named_marker *marker) {
    const pattern_matcher *pattern = request.pattern;
    const sxml_byte_view bytes = request.bytes;
    const size_t byte_offset = request.byte_offset;
    size_t name_end = byte_offset + 1;
    while (name_end < bytes.byte_count && bytes.bytes[name_end] != '>') {
        name_end += 1;
    }
    if (name_end >= bytes.byte_count) {
        return sxml_function_library_report(call, &invalid_replacement_error);
    }
    const uint32_t group_number = sxml_regex_find_named_group(
        pattern->regex,
        slice_of(bytes, (sxml_byte_range){.byte_offset = byte_offset + 1,
                                          .byte_count = name_end - byte_offset - 1}));
    if (group_number == 0) {
        return sxml_function_library_report(call, &invalid_replacement_error);
    }
    *marker = (named_marker){.group_number = group_number, .end_byte_offset = name_end + 1};
    return SXML_STATUS_OK;
}

/* A replacement string and the regex input it is applied to. */
typedef struct replacement_text {
    const pattern_matcher *pattern;
    sxml_string value;
    sxml_string replacement;
} replacement_text;

/* Appends the text of captured group group_number of the current match,
   nothing when it did not take part. */
typedef struct group_text {
    const pattern_matcher *pattern;
    sxml_string value;
    uint32_t group_number;
} group_text;

static sxml_status append_group(group_text request, sxml_library_text *text) {
    sxml_regex_captured_group group;
    sxml_status status = sxml_regex_matcher_get_captured_group(request.pattern->matcher,
                                                              request.group_number, &group);
    if (status == SXML_STATUS_OK && group.is_captured) {
        status = sxml_function_library_text_append(text,
                                                   slice_of(request.value.utf8, group.byte_range));
    }
    return status;
}

/* Appends the replacement of the current match: $N is captured group N
   (the longest number of digits naming a group), $<name> the named group,
   \$ and \\ are "$" and "\" (F&O 4.0, section 6.3.2); with the q flag the
   replacement is literal. */
static sxml_status append_replacement(const sxml_function_call *call, replacement_text input,
                                      sxml_library_text *text) {
    const pattern_matcher *pattern = input.pattern;
    if (sxml_regex_get_flags(pattern->regex).is_literal) {
        return sxml_function_library_text_append(text, input.replacement.utf8);
    }
    const sxml_byte_view bytes = input.replacement.utf8;
    const uint32_t group_count = sxml_regex_group_count(pattern->regex);
    sxml_status status = SXML_STATUS_OK;
    size_t byte_index = 0;
    while (status == SXML_STATUS_OK && byte_index < bytes.byte_count) {
        const uint8_t byte = bytes.bytes[byte_index];
        if (byte == '\\') {
            if (byte_index + 1 >= bytes.byte_count ||
                (bytes.bytes[byte_index + 1] != '\\' && bytes.bytes[byte_index + 1] != '$')) {
                return sxml_function_library_report(call, &invalid_replacement_error);
            }
            status = sxml_function_library_text_append(
                text, slice_of(bytes, (sxml_byte_range){.byte_offset = byte_index + 1,
                                                        .byte_count = 1}));
            byte_index += 2;
            continue;
        }
        if (byte != '$') {
            status = sxml_function_library_text_append(
                text,
                slice_of(bytes, (sxml_byte_range){.byte_offset = byte_index, .byte_count = 1}));
            byte_index += 1;
            continue;
        }
        byte_index += 1;
        if (byte_index < bytes.byte_count && bytes.bytes[byte_index] == '<') {
            named_marker marker = {.group_number = 0};
            status = read_named_marker(
                call, (marker_request){.pattern = pattern, .bytes = bytes, .byte_offset = byte_index},
                &marker);
            if (status == SXML_STATUS_OK) {
                status = append_group((group_text){.pattern = pattern,
                                                   .value = input.value,
                                                   .group_number = marker.group_number},
                                      text);
                byte_index = marker.end_byte_offset;
            }
            continue;
        }
        if (byte_index >= bytes.byte_count || bytes.bytes[byte_index] < '0' ||
            bytes.bytes[byte_index] > '9') {
            return sxml_function_library_report(call, &invalid_replacement_error);
        }
        uint32_t group_number = (uint32_t)(bytes.bytes[byte_index] - '0');
        byte_index += 1;
        while (byte_index < bytes.byte_count && bytes.bytes[byte_index] >= '0' &&
               bytes.bytes[byte_index] <= '9') {
            const uint64_t longer =
                (uint64_t)group_number * 10U + (uint32_t)(bytes.bytes[byte_index] - '0');
            if (longer > group_count) {
                break;
            }
            group_number = (uint32_t)longer;
            byte_index += 1;
        }
        if (group_number <= group_count) {
            status = append_group((group_text){.pattern = pattern,
                                               .value = input.value,
                                               .group_number = group_number},
                                  text);
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
        } else if (byte == '$' && byte_index + 1 < bytes.byte_count &&
                   bytes.bytes[byte_index + 1] == '<') {
            named_marker marker = {.group_number = 0};
            sxml_status status = read_named_marker(
                call,
                (marker_request){.pattern = pattern, .bytes = bytes, .byte_offset = byte_index + 1},
                &marker);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            byte_index = marker.end_byte_offset - 1;
        } else if (byte == '$' && (byte_index + 1 >= bytes.byte_count ||
                                   bytes.bytes[byte_index + 1] < '0' ||
                                   bytes.bytes[byte_index + 1] > '9')) {
            return sxml_function_library_report(call, &invalid_replacement_error);
        }
    }
    return SXML_STATUS_OK;
}

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
        (sxml_allocation_request){.byte_count = canonical.required_byte_count, .alignment = 1},
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
        text->item_heap,
        (sxml_heap_bytes){.memory = memory, .byte_count = canonical.required_byte_count});
    return status;
}

/* The string value of the replacement function's result, as fn:string
   gives it (F&O 4.0, section 6.3.2: "applying the function fn:string to
   the result"). A JNode gives the string value of its ·jvalue·. */
static sxml_status append_string_value(const sxml_function_call *call, sxml_library_text *text,
                                       sxml_item item) {
    sxml_item current = item;
    if (sxml_item_get_kind(current) == SXML_ITEM_KIND_JSON_NODE) {
        sxml_json_node_description description;
        sxml_status status = sxml_json_node_describe(current, &description);
        uint64_t item_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(description.json_value, call->error_record, &item_count);
        }
        if (status != SXML_STATUS_OK || item_count == 0) {
            return status;
        }
        if (item_count != 1 ||
            sxml_item_get_kind(description.json_value.content) == SXML_ITEM_KIND_JSON_NODE) {
            return sxml_function_library_report(call, &function_has_no_string_error);
        }
        current = description.json_value.content;
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

/* ---------------------------------------------------------- Group maps */

/* The key of captured group group_number in a groups map: its name when
   it is a named capturing subexpression, else its number (F&O 4.0,
   sections 6.3.2 and 6.3.6). Owned. */
typedef struct group_reference {
    const sxml_regex *regex;
    uint32_t group_number;
} group_reference;

static sxml_status make_group_key(const sxml_function_call *call, group_reference reference,
                                  sxml_item *key) {
    const sxml_regex *regex = reference.regex;
    const uint32_t group_number = reference.group_number;
    const sxml_byte_view name = sxml_regex_group_name(regex, group_number);
    if (name.byte_count == 0) {
        *key = sxml_atomic_item_from_integer(group_number);
        return SXML_STATUS_OK;
    }
    sxml_sequence key_sequence = sxml_sequence_empty();
    const sxml_status status =
        sxml_function_library_make_string(call->item_heap, name, &key_sequence);
    if (status == SXML_STATUS_OK) {
        *key = key_sequence.content;
    }
    return status;
}

/* Map entries in heap memory, keys and values owned until released. */
typedef struct entry_list {
    sxml_allocator allocator;
    sxml_map_entry *map_entries;
    size_t entry_count;
    size_t entry_capacity;
} entry_list;

static sxml_status entry_list_reserve(const sxml_function_call *call, size_t entry_capacity,
                                      entry_list *list) {
    *list = (entry_list){.allocator = sxml_item_heap_as_allocator(call->item_heap),
                         .map_entries = NULL,
                         .entry_count = 0,
                         .entry_capacity = entry_capacity};
    if (entry_capacity == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(entry_capacity, sizeof(sxml_map_entry), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            list->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_map_entry)},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        list->map_entries = memory;
    }
    return status;
}

/* Adds an entry, taking over key and value; releases them when full. */
static void entry_list_add(entry_list *list, sxml_map_entry entry) {
    if (list->entry_count == list->entry_capacity) {
        sxml_item_release(entry.key);
        sxml_sequence_release(entry.value);
        return;
    }
    list->map_entries[list->entry_count] = entry;
    list->entry_count += 1;
}

static void entry_list_release(entry_list *list) {
    for (size_t entry_index = 0; entry_index < list->entry_count; entry_index += 1) {
        sxml_item_release(list->map_entries[entry_index].key);
        sxml_sequence_release(list->map_entries[entry_index].value);
    }
    if (list->map_entries != NULL) {
        sxml_allocator_release(list->allocator, list->map_entries);
    }
    *list = (entry_list){.map_entries = NULL};
}

/* Makes the map of list's entries and releases the list. */
static sxml_status entry_list_finish(const sxml_function_call *call, entry_list *list,
                                     sxml_sequence *map) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_map_make(
        call->item_heap,
        &(sxml_map_request){
            .map_entries = {.map_entries = list->map_entries, .map_entry_count = list->entry_count},
            .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
            .error_record = call->error_record},
        &item);
    entry_list_release(list);
    if (status == SXML_STATUS_OK) {
        *map = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------- fn:replace, the body */

/*
 * fn:replace($value, $pattern, $replacement, $flags := "") (F&O 4.0,
 * section 6.3.2) as a resumable body (D75): a string replacement is
 * applied in the first step; a function replacement is called once per
 * match, with the matched string and a map of the captured groups that
 * took part, keyed by name or number, and its result's string value
 * replaces the match. Zero-length matches are replaced too (4.0 no
 * longer raises FORX0003).
 */
typedef struct replace_state {
    pattern_matcher pattern;
    sxml_library_text text;
    sxml_regex_search_result found;
    size_t copied_to;
    /* Owned while a call of the replacement function runs. */
    sxml_sequence call_arguments[2];
    /* The replacement function, owned. */
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
                 (sxml_byte_range){.byte_offset = replace->copied_to,
                                   .byte_count = replace->found.matching_segment.byte_offset -
                                                 replace->copied_to}));
}

/* Moves past the current match to the next one. */
static sxml_status find_next_match(const sxml_function_call *call, replace_state *replace) {
    replace->copied_to = replace->found.matching_segment.byte_offset +
                         replace->found.matching_segment.byte_count;
    return find_next(call, &replace->pattern, &replace->found);
}

/* Appends the rest of the input and finishes with the text. */
static sxml_status finish_replace(replace_state *replace, sxml_string value,
                                  sxml_native_step *step) {
    sxml_status status = sxml_function_library_text_append(
        &replace->text,
        slice_of(value.utf8,
                 (sxml_byte_range){.byte_offset = replace->copied_to,
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
static sxml_status make_action_arguments(const sxml_function_call *call, replace_state *replace,
                                         sxml_string value) {
    const sxml_regex *regex = replace->pattern.regex;
    const uint32_t group_count = sxml_regex_group_count(regex);
    entry_list entries;
    sxml_status status = entry_list_reserve(call, group_count, &entries);
    for (uint32_t group_number = 1; status == SXML_STATUS_OK && group_number <= group_count;
         group_number += 1) {
        sxml_regex_captured_group group;
        status = sxml_regex_matcher_get_captured_group(replace->pattern.matcher, group_number,
                                                      &group);
        if (status != SXML_STATUS_OK || !group.is_captured) {
            continue;
        }
        sxml_sequence captured = sxml_sequence_empty();
        status = make_untyped(call->item_heap, slice_of(value.utf8, group.byte_range), &captured);
        sxml_item key = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = make_group_key(
                call, (group_reference){.regex = regex, .group_number = group_number}, &key);
            if (status != SXML_STATUS_OK) {
                sxml_sequence_release(captured);
            }
        }
        if (status == SXML_STATUS_OK) {
            entry_list_add(&entries, (sxml_map_entry){.key = key, .value = captured});
        }
    }
    sxml_sequence groups = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = entry_list_finish(call, &entries, &groups);
    } else {
        entry_list_release(&entries);
    }
    sxml_sequence matched = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = make_untyped(call->item_heap,
                              slice_of(value.utf8, replace->found.matching_segment), &matched);
        if (status != SXML_STATUS_OK) {
            sxml_sequence_release(groups);
        }
    }
    if (status == SXML_STATUS_OK) {
        replace->call_arguments[0] = matched;
        replace->call_arguments[1] = groups;
    }
    return status;
}

/* Where the arguments of a replace call are. */
typedef struct replace_shape {
    call_shape inputs;
    size_t replacement_argument;
} replace_shape;

/* The first step: compile, then replace with a string, or ask for the
   first call of the function. */
static sxml_status start_replace(const sxml_function_call *call, replace_shape shape,
                                 replace_state *replace) {
    replace->text = (sxml_library_text){.item_heap = call->item_heap};
    regex_inputs inputs;
    sxml_status status = read_inputs(call, shape.inputs, &inputs);
    if (status == SXML_STATUS_OK) {
        status = compile_pattern(call, inputs.source, &replace->pattern);
    }
    sxml_optional_item replacement = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status =
            sxml_function_library_read_optional_item(call, shape.replacement_argument, &replacement);
    }
    const bool is_function = status == SXML_STATUS_OK && replacement.has_item &&
                             sxml_item_is_function_item(replacement.item);
    if (status == SXML_STATUS_OK && replacement.has_item && !is_function &&
        !sxml_operator_item_has_type(call->item_heap,
                                     (sxml_type_test){.item = replacement.item,
                                                      .type = SXML_TYPE_XS_STRING}) &&
        sxml_atomic_item_get_primitive_type(replacement.item).index !=
            SXML_TYPE_XS_UNTYPED_ATOMIC &&
        sxml_atomic_item_get_primitive_type(replacement.item).index != SXML_TYPE_XS_ANY_URI) {
        status = sxml_function_library_report(call, &replacement_type_error);
    }
    if (status == SXML_STATUS_OK && is_function) {
        /* Already coerced to the function type of the parameter (XPath
           4.0, section 3.4.2: function coercion within a choice). */
        replace->action = sxml_sequence_from_item(replacement.item);
        sxml_sequence_retain(replace->action);
    } else if (status == SXML_STATUS_OK && replacement.has_item) {
        sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_STRING};
        status = sxml_atomic_item_get_datum(replacement.item, &datum);
        if (status == SXML_STATUS_OK) {
            status = check_replacement(call, &replace->pattern, datum.datum_details.string);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = find_first(call, (search_start){.pattern = &replace->pattern, .value = inputs.value},
                            &replace->found);
    }
    return status;
}

/* The replacement string of a string replacement; empty for (). */
static sxml_status read_replacement_string(const sxml_function_call *call, replace_shape shape,
                                           sxml_string *replacement) {
    sxml_optional_item item = {.has_item = false};
    sxml_status status =
        sxml_function_library_read_optional_item(call, shape.replacement_argument, &item);
    *replacement = (sxml_string){.utf8 = {.bytes = NULL, .byte_count = 0},
                                 .codepoint_count = 0,
                                 .is_ascii = true};
    if (status == SXML_STATUS_OK && item.has_item && !sxml_item_is_function_item(item.item)) {
        sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_STRING};
        status = sxml_atomic_item_get_datum(item.item, &datum);
        if (status == SXML_STATUS_OK) {
            *replacement = datum.datum_details.string;
        }
    }
    return status;
}

/* Takes the result of the replacement function's call: its string value
   follows the text so far. */
static sxml_status take_action_result(const sxml_native_step_request *request,
                                      replace_state *replace) {
    const sxml_function_call *call = request->call;
    sxml_sequence_release(replace->call_arguments[0]);
    sxml_sequence_release(replace->call_arguments[1]);
    replace->call_arguments[0] = sxml_sequence_empty();
    replace->call_arguments[1] = sxml_sequence_empty();
    sxml_optional_item value = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(
        &(sxml_function_call){
            .arguments = {.sequences = &request->call_result, .sequence_count = 1},
            .error_record = call->error_record},
        0, &value);
    if (status == SXML_STATUS_OK && value.has_item) {
        status = append_string_value(call, &replace->text, value.item);
    }
    sxml_sequence_release(request->call_result);
    if (status == SXML_STATUS_OK) {
        status = find_next_match(call, replace);
    }
    return status;
}

static sxml_status step_replace_with(const sxml_native_step_request *request,
                                     replace_shape shape, sxml_native_step *step) {
    replace_state *replace = request->state;
    const sxml_function_call *call = request->call;
    sxml_status status = request->step_number == 0 ? start_replace(call, shape, replace)
                                                   : take_action_result(request, replace);
    regex_inputs inputs;
    if (status == SXML_STATUS_OK) {
        status = read_inputs(call, shape.inputs, &inputs);
    }
    const bool is_function = !is_empty_sequence(replace->action);
    sxml_string replacement = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK && !is_function) {
        status = read_replacement_string(call, shape, &replacement);
    }
    while (status == SXML_STATUS_OK && replace->found.is_found && !is_function) {
        status = append_before_match(replace, inputs.value);
        if (status == SXML_STATUS_OK) {
            status = append_replacement(
                call,
                (replacement_text){.pattern = &replace->pattern,
                                   .value = inputs.value,
                                   .replacement = replacement},
                &replace->text);
        }
        if (status == SXML_STATUS_OK) {
            status = find_next_match(call, replace);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!replace->found.is_found) {
        return finish_replace(replace, inputs.value, step);
    }
    status = append_before_match(replace, inputs.value);
    if (status == SXML_STATUS_OK) {
        status = make_action_arguments(call, replace, inputs.value);
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

static sxml_status step_replace(const sxml_native_step_request *request, sxml_native_step *step) {
    return step_replace_with(
        request,
        (replace_shape){.inputs = {.is_record_function = false, .flags_argument = 3},
                        .replacement_argument = 2},
        step);
}

static sxml_status step_record_replace(const sxml_native_step_request *request,
                                       sxml_native_step *step) {
    return step_replace_with(
        request,
        (replace_shape){.inputs = {.is_record_function = true, .flags_argument = 0},
                        .replacement_argument = 1},
        step);
}

static const sxml_resumable_native_body replace_body = {
    .step = step_replace,
    .release_state = release_replace_state,
    .state_byte_count = sizeof(replace_state),
    .state_alignment = alignof(replace_state),
};

static const sxml_resumable_native_body record_replace_body = {
    .step = step_record_replace,
    .release_state = release_replace_state,
    .state_byte_count = sizeof(replace_state),
    .state_alignment = alignof(replace_state),
};


/* -------------------------------------------------- fn:matching-segments */

/* The current match of a search of value, for the records and the tree
   built from it. */
typedef struct current_match {
    const pattern_matcher *pattern;
    sxml_string value;
    /* The position of the match's start. */
    codepoint_cursor cursor;
    /* The matching segment. */
    sxml_byte_range segment;
} current_match;

/* An xs:integer item as a sequence. */
static sxml_sequence integer_sequence(uint64_t integer) {
    return sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)integer));
}

/* A string item of utf8 as a sequence, owned. */
static sxml_status string_sequence(const sxml_function_call *call, sxml_byte_view utf8,
                                   sxml_sequence *sequence) {
    return sxml_function_library_make_string(call->item_heap, utf8, sequence);
}

/* A captured group of the current match. */
typedef struct match_group {
    const current_match *match;
    uint32_t group_number;
    sxml_regex_captured_group group;
} match_group;

/* Makes the built-in record local_name of field_values (D133) when
   making the values succeeded (making_status), and releases the values. */
typedef struct record_values {
    const char *local_name;
    sxml_sequence *field_values;
    size_t field_count;
    sxml_status making_status;
} record_values;

static sxml_status finish_record(const sxml_function_call *call, record_values values,
                                 sxml_sequence *record) {
    sxml_status status = values.making_status;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_record(
            call,
            &(sxml_builtin_record_request){
                .local_name = sxml_byte_view_from_c_string(values.local_name),
                .field_values = {.sequences = values.field_values,
                                 .sequence_count = values.field_count}},
            record);
    }
    for (size_t field_index = 0; field_index < values.field_count; field_index += 1) {
        sxml_sequence_release(values.field_values[field_index]);
    }
    return status;
}

/* The fn:captured-group-record of a captured group (F&O 4.0, section
   6.3.7): value, position, nr and name. */
static sxml_status make_group_record(const sxml_function_call *call, match_group request,
                                     sxml_sequence *record) {
    const current_match *match = request.match;
    const sxml_regex_captured_group group = request.group;
    sxml_sequence field_values[4] = {
        sxml_sequence_empty(),
        integer_sequence(position_at(match->value, match->cursor, group.byte_range.byte_offset)),
        integer_sequence(request.group_number), sxml_sequence_empty()};
    sxml_status status =
        string_sequence(call, slice_of(match->value.utf8, group.byte_range), &field_values[0]);
    const sxml_byte_view name = sxml_regex_group_name(match->pattern->regex, request.group_number);
    if (status == SXML_STATUS_OK && name.byte_count != 0) {
        status = string_sequence(call, name, &field_values[3]);
    }
    return finish_record(call,
                         (record_values){.local_name = "captured-group-record",
                                         .field_values = field_values,
                                         .field_count = 4,
                                         .making_status = status},
                         record);
}

/* The groups map of the current match (F&O 4.0, section 6.3.6): every
   captured group that took part, lookahead groups included, in the order
   of the capturing subexpressions, keyed by name or number. */
static sxml_status make_groups_map(const sxml_function_call *call, const current_match *match,
                                   sxml_sequence *groups) {
    const sxml_regex *regex = match->pattern->regex;
    const uint32_t group_count = sxml_regex_group_count(regex);
    entry_list entries;
    sxml_status status = entry_list_reserve(call, group_count, &entries);
    for (uint32_t group_number = 1; status == SXML_STATUS_OK && group_number <= group_count;
         group_number += 1) {
        sxml_regex_captured_group group;
        status =
            sxml_regex_matcher_get_captured_group(match->pattern->matcher, group_number, &group);
        if (status != SXML_STATUS_OK || !group.is_captured) {
            continue;
        }
        sxml_sequence record = sxml_sequence_empty();
        status = make_group_record(
            call, (match_group){.match = match, .group_number = group_number, .group = group},
            &record);
        sxml_item key = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = make_group_key(
                call, (group_reference){.regex = regex, .group_number = group_number}, &key);
            if (status != SXML_STATUS_OK) {
                sxml_sequence_release(record);
            }
        }
        if (status == SXML_STATUS_OK) {
            entry_list_add(&entries, (sxml_map_entry){.key = key, .value = record});
        }
    }
    if (status != SXML_STATUS_OK) {
        entry_list_release(&entries);
        return status;
    }
    return entry_list_finish(call, &entries, groups);
}

/* The fn:matching-segment-record of the current match (F&O 4.0, section
   6.3.6): substring, position and groups. */
static sxml_status make_segment_record(const sxml_function_call *call, const current_match *match,
                                       sxml_sequence *record) {
    sxml_sequence field_values[3] = {sxml_sequence_empty(),
                                     integer_sequence(match->cursor.position),
                                     sxml_sequence_empty()};
    sxml_status status =
        string_sequence(call, slice_of(match->value.utf8, match->segment), &field_values[0]);
    if (status == SXML_STATUS_OK) {
        status = make_groups_map(call, match, &field_values[2]);
    }
    return finish_record(call,
                         (record_values){.local_name = "matching-segment-record",
                                         .field_values = field_values,
                                         .field_count = 3,
                                         .making_status = status},
                         record);
}

/* A non-matching segment of value: appended as an xs:string unless it
   is empty. */
typedef struct gap_request {
    sxml_sequence_builder *builder;
    sxml_byte_view value;
    sxml_byte_range gap;
} gap_request;

static sxml_status append_gap(const sxml_function_call *call, gap_request request) {
    if (request.gap.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_sequence gap = sxml_sequence_empty();
    sxml_status status = string_sequence(call, slice_of(request.value, request.gap), &gap);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_item(request.builder, gap.content);
        sxml_sequence_release(gap);
    }
    return status;
}

/* fn:matching-segments($value, $pattern, $flags := "") (F&O 4.0, section
   6.3.5): a record per disjoint matching segment, in order. */
static sxml_status matching_segments_with(const sxml_function_call *call, call_shape shape,
                                          sxml_sequence *result) {
    regex_inputs inputs;
    sxml_status status = read_inputs(call, shape, &inputs);
    pattern_matcher pattern = {.regex = NULL, .matcher = NULL};
    if (status == SXML_STATUS_OK) {
        status = compile_pattern(call, inputs.source, &pattern);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    sxml_regex_search_result found = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = find_first(call, (search_start){.pattern = &pattern, .value = inputs.value}, &found);
    }
    current_match match = {.pattern = &pattern,
                           .value = inputs.value,
                           .cursor = {.byte_offset = 0, .position = 1},
                           .segment = {.byte_offset = 0, .byte_count = 0}};
    size_t gap_start = 0;
    while (status == SXML_STATUS_OK && found.is_found) {
        const size_t start = found.matching_segment.byte_offset;
        if (shape.includes_gaps) {
            status = append_gap(call, (gap_request){.builder = builder,
                                                    .value = inputs.value.utf8,
                                                    .gap = {.byte_offset = gap_start,
                                                            .byte_count = start - gap_start}});
            gap_start = start + found.matching_segment.byte_count;
        }
        match.segment = found.matching_segment;
        match.cursor = (codepoint_cursor){
            .byte_offset = start, .position = position_at(inputs.value, match.cursor, start)};
        sxml_sequence record = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            status = make_segment_record(call, &match, &record);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, record.content);
            sxml_sequence_release(record);
        }
        if (status == SXML_STATUS_OK) {
            status = find_next(call, &pattern, &found);
        }
    }
    if (status == SXML_STATUS_OK && shape.includes_gaps) {
        status = append_gap(call, (gap_request){.builder = builder,
                                                .value = inputs.value.utf8,
                                                .gap = {.byte_offset = gap_start,
                                                        .byte_count = inputs.value.utf8.byte_count -
                                                                      gap_start}});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    release_pattern(&pattern);
    return status;
}

static sxml_status call_matching_segments(const sxml_function_call *call, sxml_sequence *result) {
    return matching_segments_with(
        call, (call_shape){.is_record_function = false, .flags_argument = 2}, result);
}

/* analyze-segments (src/xslt_number_internal.h): fn:matching-segments
   with the non-matching segments between the records. */
static sxml_status call_analyze_segments(const sxml_function_call *call, sxml_sequence *result) {
    return matching_segments_with(
        call, (call_shape){.is_record_function = false, .flags_argument = 2, .includes_gaps = true},
        result);
}

static const sxml_function_parameter analyze_segments_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string"),
    SXML_REQUIRED_PARAMETER("pattern", "xs:string"),
    SXML_REQUIRED_PARAMETER("flags", "xs:string"),
};

const sxml_native_function sxml_xslt_analyze_segments_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("analyze-segments"),
    .parameters = {analyze_segments_parameters,
                   sizeof(analyze_segments_parameters) / sizeof(analyze_segments_parameters[0])},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = call_analyze_segments,
    .resumable_body = NULL};

static sxml_status call_record_matching_segments(const sxml_function_call *call,
                                                 sxml_sequence *result) {
    return matching_segments_with(
        call, (call_shape){.is_record_function = true, .flags_argument = 0}, result);
}

/* ------------------------------------------------------ fn:analyze-string */

/* The names of the result tree of fn:analyze-string (F&O 4.0, section
   6.3.4, and appendix D.1). */
typedef struct analyze_names {
    sxml_name_identifier result_name;
    sxml_name_identifier match_name;
    sxml_name_identifier non_match_name;
    sxml_name_identifier group_name;
    sxml_name_identifier lookahead_group_name;
    sxml_name_identifier nr_name;
    sxml_name_identifier name_name;
    sxml_name_identifier value_name;
    sxml_name_identifier position_name;
} analyze_names;

static sxml_status intern_analyze_names(sxml_name_pool *name_pool, analyze_names *names) {
    const struct {
        sxml_well_known_namespace name_namespace;
        const char *local_name;
        sxml_name_identifier *output;
    } texts[] = {
        {SXML_NAMESPACE_FUNCTIONS, "analyze-string-result", &names->result_name},
        {SXML_NAMESPACE_FUNCTIONS, "match", &names->match_name},
        {SXML_NAMESPACE_FUNCTIONS, "non-match", &names->non_match_name},
        {SXML_NAMESPACE_FUNCTIONS, "group", &names->group_name},
        {SXML_NAMESPACE_FUNCTIONS, "lookahead-group", &names->lookahead_group_name},
        {SXML_NAMESPACE_NONE, "nr", &names->nr_name},
        {SXML_NAMESPACE_NONE, "name", &names->name_name},
        {SXML_NAMESPACE_NONE, "value", &names->value_name},
        {SXML_NAMESPACE_NONE, "position", &names->position_name},
    };
    for (size_t name_index = 0; name_index < sizeof(texts) / sizeof(texts[0]);
         name_index += 1) {
        const sxml_status status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){
                .namespace_uri =
                    sxml_well_known_namespace_identifier(texts[name_index].name_namespace),
                .local_name = sxml_byte_view_from_c_string(texts[name_index].local_name)},
            texts[name_index].output);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* Sends the events of the result tree to a tree builder. */
typedef struct analyze_writer {
    sxml_tree_builder builder;
    analyze_names names;
} analyze_writer;

static const sxml_source_location no_location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                                 .line_number = 0,
                                                 .column_number = 0,
                                                 .byte_offset = 0};

static sxml_status write_start(const analyze_writer *writer, sxml_name_identifier name) {
    return writer->builder.operations->start_element(
        writer->builder.state,
        &(sxml_element_start_event){.element_name = name,
                                    .namespace_prefix = {.index = 0},
                                    .type_annotation = {.index = SXML_TYPE_NONE},
                                    .is_id = false,
                                    .is_idrefs = false,
                                    .is_nilled = false,
                                    .ignores_parent_namespaces = false,
                                    .location = no_location});
}

static sxml_status write_end(const analyze_writer *writer) {
    return writer->builder.operations->end_element(writer->builder.state);
}

static sxml_status write_text(const analyze_writer *writer, sxml_byte_view text) {
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    return writer->builder.operations->add_text(
        writer->builder.state, &(sxml_text_event){.text = text, .location = no_location});
}

static sxml_status write_attribute(const analyze_writer *writer, sxml_name_identifier name,
                                   sxml_byte_view value) {
    return writer->builder.operations->add_attribute(
        writer->builder.state,
        &(sxml_attribute_event){.attribute_name = name,
                                .namespace_prefix = {.index = 0},
                                .attribute_value = value,
                                .type_annotation = {.index = SXML_TYPE_NONE},
                                .is_id = false,
                                .is_idrefs = false,
                                .location = no_location});
}

/* The nr and, for a named group, name attributes of a group element. */
static sxml_status write_group_attributes(const analyze_writer *writer,
                                          const current_match *match, uint32_t group_number) {
    decimal_digits digits;
    sxml_status status =
        write_attribute(writer, writer->names.nr_name, write_decimal(group_number, &digits));
    const sxml_byte_view name = sxml_regex_group_name(match->pattern->regex, group_number);
    if (status == SXML_STATUS_OK && name.byte_count != 0) {
        status = write_attribute(writer, writer->names.name_name, name);
    }
    return status;
}

/* An ordinary captured group of the current match. */
typedef struct ordinary_group {
    uint32_t group_number;
    size_t start_byte;
    size_t end_byte;
} ordinary_group;

/* True when left comes before right in the nesting order: by start, then
   the longer first, then by number (an outer group's "(" comes first). */
static bool ordinary_group_precedes(ordinary_group left, ordinary_group right) {
    if (left.start_byte != right.start_byte) {
        return left.start_byte < right.start_byte;
    }
    if (left.end_byte != right.end_byte) {
        return left.end_byte > right.end_byte;
    }
    return left.group_number < right.group_number;
}

/* Heap memory for the ordinary groups of one match and the stack of the
   group elements open while writing it. */
typedef struct group_scratch {
    sxml_allocator allocator;
    ordinary_group *groups;
    size_t group_count;
    /* Indices into groups. */
    size_t *open_groups;
} group_scratch;

static void group_scratch_release(group_scratch *scratch) {
    if (scratch->groups != NULL) {
        sxml_allocator_release(scratch->allocator, scratch->groups);
    }
    if (scratch->open_groups != NULL) {
        sxml_allocator_release(scratch->allocator, scratch->open_groups);
    }
    scratch->groups = NULL;
    scratch->open_groups = NULL;
}

static sxml_status group_scratch_reserve(group_scratch *scratch, size_t capacity) {
    if (capacity == 0) {
        return SXML_STATUS_OK;
    }
    size_t group_byte_count = 0;
    size_t index_byte_count = 0;
    sxml_status status = sxml_size_multiply(capacity, sizeof(ordinary_group), &group_byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(capacity, sizeof(size_t), &index_byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            scratch->allocator,
            (sxml_allocation_request){.byte_count = group_byte_count,
                                      .alignment = alignof(ordinary_group)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    scratch->groups = memory;
    status = sxml_allocator_allocate(
        scratch->allocator,
        (sxml_allocation_request){.byte_count = index_byte_count, .alignment = alignof(size_t)},
        &memory);
    if (status == SXML_STATUS_OK) {
        scratch->open_groups = memory;
    }
    return status;
}

/* Collects the ordinary groups that took part, sorted for nesting (an
   insertion sort: groups are few, and nothing recurses). */
static sxml_status collect_ordinary_groups(const current_match *match, group_scratch *scratch) {
    const sxml_regex *regex = match->pattern->regex;
    scratch->group_count = 0;
    if (scratch->groups == NULL) {
        /* A regex without groups. */
        return SXML_STATUS_OK;
    }
    for (uint32_t group_number = 1; group_number <= sxml_regex_group_count(regex);
         group_number += 1) {
        if (sxml_regex_group_is_in_lookahead(regex, group_number)) {
            continue;
        }
        sxml_regex_captured_group group;
        const sxml_status status =
            sxml_regex_matcher_get_captured_group(match->pattern->matcher, group_number, &group);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!group.is_captured) {
            continue;
        }
        const ordinary_group new_group = {
            .group_number = group_number,
            .start_byte = group.byte_range.byte_offset,
            .end_byte = group.byte_range.byte_offset + group.byte_range.byte_count};
        size_t insert_index = scratch->group_count;
        while (insert_index > 0 &&
               ordinary_group_precedes(new_group, scratch->groups[insert_index - 1])) {
            scratch->groups[insert_index] = scratch->groups[insert_index - 1];
            insert_index -= 1;
        }
        scratch->groups[insert_index] = new_group;
        scratch->group_count += 1;
    }
    return SXML_STATUS_OK;
}

/* Two groups of a regex. */
typedef struct group_pair {
    uint32_t outer_group_number;
    uint32_t inner_group_number;
} group_pair;

/* True when the parentheses of the outer group enclose those of the inner
   one: an empty group at the end of another is its child only then (QT4
   analyzeString-017 and -017a). */
static bool is_enclosing_group(const sxml_regex *regex, group_pair pair) {
    uint32_t group_number = sxml_regex_group_parent(regex, pair.inner_group_number);
    /* Parents have smaller numbers, so the walk ends. */
    while (group_number != 0 && group_number >= pair.outer_group_number) {
        if (group_number == pair.outer_group_number) {
            return true;
        }
        group_number = sxml_regex_group_parent(regex, group_number);
    }
    return false;
}

/* How far the text of a match has been written. */
typedef struct match_progress {
    const current_match *match;
    size_t written_to;
} match_progress;

/* Writes the text from progress->written_to up to end_byte and moves it
   there. */
static sxml_status write_input_up_to(const analyze_writer *writer, match_progress *progress,
                                     size_t end_byte) {
    const sxml_status status = write_text(
        writer, slice_of(progress->match->value.utf8,
                         (sxml_byte_range){.byte_offset = progress->written_to,
                                           .byte_count = end_byte - progress->written_to}));
    progress->written_to = end_byte;
    return status;
}

/*
 * Writes the content of a fn:match element: text and fn:group elements
 * of the ordinary groups, nested by their ranges and the nesting of their
 * parentheses, with a stack of the open elements. A group that a repetition captured outside its
 * enclosing group's last capture would cross it and is left out (at most
 * one fn:group per group, F&O 4.0, section 6.3.4).
 */
static sxml_status write_match_groups(const analyze_writer *writer, const current_match *match,
                                      group_scratch *scratch) {
    const sxml_byte_range segment = match->segment;
    sxml_status status = collect_ordinary_groups(match, scratch);
    match_progress progress = {.match = match, .written_to = segment.byte_offset};
    size_t open_count = 0;
    for (size_t group_index = 0; status == SXML_STATUS_OK && group_index < scratch->group_count;
         group_index += 1) {
        const ordinary_group group = scratch->groups[group_index];
        bool is_crossing = false;
        while (status == SXML_STATUS_OK && open_count > 0) {
            const ordinary_group open_group = scratch->groups[scratch->open_groups[open_count - 1]];
            if (group.start_byte >= open_group.start_byte &&
                group.end_byte <= open_group.end_byte &&
                is_enclosing_group(match->pattern->regex,
                                   (group_pair){.outer_group_number = open_group.group_number,
                                                .inner_group_number = group.group_number})) {
                break;
            }
            if (group.start_byte < open_group.end_byte) {
                is_crossing = true;
                break;
            }
            status = write_input_up_to(writer, &progress, open_group.end_byte);
            if (status == SXML_STATUS_OK) {
                status = write_end(writer);
            }
            open_count -= 1;
        }
        if (status != SXML_STATUS_OK || is_crossing) {
            continue;
        }
        status = write_input_up_to(writer, &progress, group.start_byte);
        if (status == SXML_STATUS_OK) {
            status = write_start(writer, writer->names.group_name);
        }
        if (status == SXML_STATUS_OK) {
            status = write_group_attributes(writer, match, group.group_number);
        }
        scratch->open_groups[open_count] = group_index;
        open_count += 1;
    }
    while (status == SXML_STATUS_OK && open_count > 0) {
        status = write_input_up_to(writer, &progress,
                                   scratch->groups[scratch->open_groups[open_count - 1]].end_byte);
        if (status == SXML_STATUS_OK) {
            status = write_end(writer);
        }
        open_count -= 1;
    }
    if (status == SXML_STATUS_OK) {
        status =
            write_input_up_to(writer, &progress, segment.byte_offset + segment.byte_count);
    }
    return status;
}

/* The fn:lookahead-group elements of the current match, in group order:
   nr, name, value and position attributes (F&O 4.0, section 6.3.4). */
static sxml_status write_lookahead_groups(const analyze_writer *writer,
                                          const current_match *match) {
    const sxml_regex *regex = match->pattern->regex;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t group_number = 1;
         status == SXML_STATUS_OK && group_number <= sxml_regex_group_count(regex);
         group_number += 1) {
        if (!sxml_regex_group_is_in_lookahead(regex, group_number)) {
            continue;
        }
        sxml_regex_captured_group group;
        status =
            sxml_regex_matcher_get_captured_group(match->pattern->matcher, group_number, &group);
        if (status != SXML_STATUS_OK || !group.is_captured) {
            continue;
        }
        status = write_start(writer, writer->names.lookahead_group_name);
        if (status == SXML_STATUS_OK) {
            status = write_group_attributes(writer, match, group_number);
        }
        if (status == SXML_STATUS_OK) {
            status = write_attribute(writer, writer->names.value_name,
                                     slice_of(match->value.utf8, group.byte_range));
        }
        decimal_digits digits;
        if (status == SXML_STATUS_OK) {
            status = write_attribute(
                writer, writer->names.position_name,
                write_decimal(
                    position_at(match->value, match->cursor, group.byte_range.byte_offset),
                    &digits));
        }
        if (status == SXML_STATUS_OK) {
            status = write_end(writer);
        }
    }
    return status;
}

/* A fn:non-match element of value[start, end), when that is not empty. */
static sxml_status write_non_match(const analyze_writer *writer, sxml_string value,
                                   sxml_byte_range range) {
    if (range.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_status status = write_start(writer, writer->names.non_match_name);
    if (status == SXML_STATUS_OK) {
        status = write_text(writer, slice_of(value.utf8, range));
    }
    if (status == SXML_STATUS_OK) {
        status = write_end(writer);
    }
    return status;
}

/* Writes the whole result element for the matches of pattern in value. */
static sxml_status write_analysis(const sxml_function_call *call, const analyze_writer *writer,
                                  current_match *match) {
    group_scratch scratch = {.allocator = sxml_item_heap_as_allocator(call->item_heap),
                             .groups = NULL,
                             .group_count = 0,
                             .open_groups = NULL};
    sxml_status status =
        group_scratch_reserve(&scratch, sxml_regex_group_count(match->pattern->regex));
    if (status == SXML_STATUS_OK) {
        status = write_start(writer, writer->names.result_name);
    }
    if (status == SXML_STATUS_OK) {
        status = writer->builder.operations->add_namespace_binding(
            writer->builder.state,
            &(sxml_namespace_binding){
                .namespace_prefix = {.index = 0},
                .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS)});
    }
    sxml_regex_search_result found = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = find_first(call, (search_start){.pattern = match->pattern, .value = match->value},
                            &found);
    }
    size_t written_to = 0;
    while (status == SXML_STATUS_OK && found.is_found) {
        const sxml_byte_range segment = found.matching_segment;
        status = write_non_match(writer, match->value,
                                 (sxml_byte_range){.byte_offset = written_to,
                                                   .byte_count = segment.byte_offset - written_to});
        match->cursor = (codepoint_cursor){
            .byte_offset = segment.byte_offset,
            .position = position_at(match->value, match->cursor, segment.byte_offset)};
        match->segment = segment;
        if (status == SXML_STATUS_OK) {
            status = write_start(writer, writer->names.match_name);
        }
        if (status == SXML_STATUS_OK) {
            status = write_match_groups(writer, match, &scratch);
        }
        if (status == SXML_STATUS_OK) {
            status = write_lookahead_groups(writer, match);
        }
        if (status == SXML_STATUS_OK) {
            status = write_end(writer);
        }
        written_to = segment.byte_offset + segment.byte_count;
        if (status == SXML_STATUS_OK) {
            status = find_next(call, match->pattern, &found);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = write_non_match(
            writer, match->value,
            (sxml_byte_range){.byte_offset = written_to,
                              .byte_count = match->value.utf8.byte_count - written_to});
    }
    if (status == SXML_STATUS_OK) {
        status = write_end(writer);
    }
    group_scratch_release(&scratch);
    return status;
}

/* fn:analyze-string($value, $pattern, $flags := "") (F&O 4.0, section
   6.3.4): a new untyped parentless fn:analyze-string-result element, its
   memory from the heap, destroyed when unreferenced. */
static sxml_status analyze_string_with(const sxml_function_call *call, call_shape shape,
                                       sxml_sequence *result) {
    regex_inputs inputs;
    sxml_status status = read_inputs(call, shape, &inputs);
    pattern_matcher pattern = {.regex = NULL, .matcher = NULL};
    if (status == SXML_STATUS_OK) {
        status = compile_pattern(call, inputs.source, &pattern);
    }
    analyze_writer writer = {.builder = {.state = NULL, .operations = NULL}};
    if (status == SXML_STATUS_OK) {
        status = intern_analyze_names(sxml_item_heap_get_name_pool(call->item_heap),
                                      &writer.names);
    }
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
        writer.builder = sxml_arena_tree_as_tree_builder(arena_tree);
        current_match match = {.pattern = &pattern,
                               .value = inputs.value,
                               .cursor = {.byte_offset = 0, .position = 1},
                               .segment = {.byte_offset = 0, .byte_count = 0}};
        status = write_analysis(call, &writer, &match);
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    release_pattern(&pattern);
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *result = sxml_sequence_from_item(sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0}));
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

static sxml_status call_analyze_string(const sxml_function_call *call, sxml_sequence *result) {
    return analyze_string_with(
        call, (call_shape){.is_record_function = false, .flags_argument = 2}, result);
}

static sxml_status call_record_analyze_string(const sxml_function_call *call,
                                              sxml_sequence *result) {
    return analyze_string_with(
        call, (call_shape){.is_record_function = true, .flags_argument = 0}, result);
}

/* -------------------------------------------------------------- fn:regex */

/*
 * The bodies of the functions in a fn:compiled-regex-record (F&O 4.0,
 * section 6.3.9). No part lists them, so no name finds them; each
 * captures the pattern and the flags (captured values 0 and 1) and
 * behaves as the function of the same name with them.
 */
static const sxml_native_function record_function_rows[] = {
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("matches"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
     .call = call_record_matches,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("tokenize"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string*"),
     .call = call_record_tokenize,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("replace"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
     .call = NULL,
     .resumable_body = &record_replace_body},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("analyze-string"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("element(fn:analyze-string-result)"),
     .call = call_record_analyze_string,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("matching-segments"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("fn:matching-segment-record*"),
     .call = call_record_matching_segments,
     .resumable_body = NULL},
};

/* The fields of fn:compiled-regex-record after pattern and flags are the
   function items of record_function_rows, in that order. */
#define SXML_REGEX_RECORD_FIELD_COUNT ((size_t)7)
#define SXML_REGEX_RECORD_FUNCTION_COUNT                                    \
    (sizeof(record_function_rows) / sizeof(record_function_rows[0]))

/* Makes the function item over row, with signature, capturing pattern
   and flags (both owned by the caller). */
typedef struct record_function_request {
    const sxml_native_function *row;
    sxml_type_identifier signature;
    const sxml_sequence *captured;
} record_function_request;

static sxml_status make_record_function(const sxml_function_call *call,
                                        const record_function_request *request,
                                        sxml_sequence *function_item) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_function_item_make(
        call->item_heap,
        &(sxml_function_item_request){
            .function_name = {.index = 0},
            .signature = request->signature,
            .function_annotations = {.function_annotations = NULL,
                                     .function_annotation_count = 0},
            .captured_values = {.sequences = request->captured, .sequence_count = 2},
            .body = {.state = (void *)request->row,
                     .operations = &sxml_native_function_body_operations}},
        &item);
    if (status == SXML_STATUS_OK) {
        *function_item = sxml_sequence_from_item(item);
    }
    return status;
}

/* The fields of fn:compiled-regex-record (F&O 4.0, section 6.3.9). */
static sxml_status describe_regex_record(const sxml_function_call *call,
                                         sxml_record_field_view *fields) {
    sxml_type_identifier record_type = {.index = SXML_TYPE_NONE};
    sxml_status status = sxml_function_library_find_record_type(
        call->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("compiled-regex-record"), &record_type);
    sxml_type_description description = {.variety = SXML_TYPE_VARIETY_ANY_ITEM};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_describe_type(
            sxml_item_heap_get_type_registry(call->item_heap), record_type, &description);
    }
    if (status == SXML_STATUS_OK &&
        (description.variety != SXML_TYPE_VARIETY_RECORD ||
         description.variety_details.record_type.record_fields.record_field_count !=
             SXML_REGEX_RECORD_FIELD_COUNT)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        *fields = description.variety_details.record_type.record_fields;
    }
    return status;
}

/* fn:regex($pattern, $flags := "") (F&O 4.0, section 6.3.8): the
   fn:compiled-regex-record of the pattern and flags, after compiling them
   once for their errors. Each function item's signature is the item type
   of its field (D133). */
static sxml_status call_regex(const sxml_function_call *call, sxml_sequence *result) {
    regex_source source;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &source.pattern_text);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &source.flags_text);
    }
    pattern_matcher pattern = {.regex = NULL, .matcher = NULL};
    if (status == SXML_STATUS_OK) {
        status = compile_pattern(call, source, &pattern);
    }
    release_pattern(&pattern);
    sxml_record_field_view fields = {.record_fields = NULL, .record_field_count = 0};
    if (status == SXML_STATUS_OK) {
        status = describe_regex_record(call, &fields);
    }
    sxml_sequence field_values[SXML_REGEX_RECORD_FIELD_COUNT];
    for (size_t field_index = 0; field_index < SXML_REGEX_RECORD_FIELD_COUNT; field_index += 1) {
        field_values[field_index] = sxml_sequence_empty();
    }
    if (status == SXML_STATUS_OK) {
        status = string_sequence(call, source.pattern_text.utf8, &field_values[0]);
    }
    if (status == SXML_STATUS_OK) {
        status = string_sequence(call, source.flags_text.utf8, &field_values[1]);
    }
    for (size_t function_index = 0;
         status == SXML_STATUS_OK && function_index < SXML_REGEX_RECORD_FUNCTION_COUNT;
         function_index += 1) {
        status = make_record_function(
            call,
            &(record_function_request){
                .row = &record_function_rows[function_index],
                .signature = fields.record_fields[function_index + 2].field_type.item_type,
                .captured = field_values},
            &field_values[function_index + 2]);
    }
    return finish_record(call,
                         (record_values){.local_name = "compiled-regex-record",
                                         .field_values = field_values,
                                         .field_count = SXML_REGEX_RECORD_FIELD_COUNT,
                                         .making_status = status},
                         result);
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter matches_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("pattern", "xs:string"),
    SXML_OPTIONAL_PARAMETER("flags", "xs:string?")};
static const sxml_function_parameter replace_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_REQUIRED_PARAMETER("pattern", "xs:string"),
    /* F&O 4.0, section 6.3.2. */
    SXML_REQUIRED_PARAMETER("replacement",
                            "(xs:string | fn(xs:untypedAtomic, map((xs:integer | xs:string), "
                            "xs:untypedAtomic)) as item()?)?"),
    SXML_OPTIONAL_PARAMETER("flags", "xs:string?")};
static const sxml_function_parameter tokenize_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("pattern", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("flags", "xs:string?")};
static const sxml_function_parameter regex_parameters[] = {
    SXML_REQUIRED_PARAMETER("pattern", "xs:string"),
    SXML_OPTIONAL_PARAMETER("flags", "xs:string?")};

static const sxml_native_function regex_functions[] = {
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
    SXML_FUNCTION_ROW("analyze-string", matches_parameters,
                      "element(fn:analyze-string-result)", 0, call_analyze_string),
    SXML_FUNCTION_ROW("matching-segments", matches_parameters, "fn:matching-segment-record*",
                      0, call_matching_segments),
    SXML_FUNCTION_ROW("regex", regex_parameters, "fn:compiled-regex-record", 0, call_regex),
};

const sxml_native_function_view sxml_function_library_regex = SXML_LIBRARY_PART(regex_functions);
