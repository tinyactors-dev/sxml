/*
 * Tests for the higher-order, map and array parts of the function library
 * (src/function_library_higher_order.c, src/function_library_map.c,
 * src/function_library_array.c; D55, D75, D79).
 *
 * Golden file: tests/evaluator_test.c (fixture, function table, evaluation
 * as text, allocation-failure loop), itself after tests/memory_test.c.
 * Expressions are compiled with sxml_xpath_compile against a function
 * table over the three parts under test and a test-only part of a few
 * simple fn: functions written here (the core library is packet 1.20's),
 * evaluated, and their values written as text: atomic items in canonical
 * form separated by spaces, other items by kind. Every case checks that the
 * item heap's cells and the allocator's blocks are all released. Slice
 * queries are named by their QT4 test case.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "function_library_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    alignas(16) uint8_t counting_storage[256];
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} fixture;

static sxml_status fixture_create(uint64_t failing_allocation_number,
                                  fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL};
    sxml_status status = sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = test_fixture->counting_storage,
                           .byte_count = sizeof(test_fixture->counting_storage)},
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_create(
            sxml_default_allocator(),
            &(sxml_name_pool_configuration){.allocator = sxml_default_allocator(),
                                            .maximum_byte_count = 0},
            &test_fixture->name_pool);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_create(
            sxml_default_allocator(),
            &(sxml_type_registry_configuration){
                .allocator = sxml_default_allocator(),
                .name_pool = test_fixture->name_pool,
                .maximum_registered_type_count = 0},
            &test_fixture->registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            sxml_default_allocator(),
            &(sxml_item_heap_configuration){
                .allocator = sxml_default_allocator(),
                .name_pool = test_fixture->name_pool,
                .type_registry = test_fixture->registry,
                .maximum_byte_count = 0},
            &test_fixture->item_heap);
    }
    return status;
}

static void fixture_destroy(fixture *test_fixture) {
    sxml_item_heap_destroy(test_fixture->item_heap);
    sxml_type_registry_destroy(test_fixture->registry);
    sxml_name_pool_destroy(test_fixture->name_pool);
    sxml_counting_allocator_finalize(test_fixture->counting_allocator);
}

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static bool fixture_released_everything(const fixture *test_fixture) {
    sxml_item_heap_clear_raised_error(test_fixture->item_heap);
    const bool is_clean =
        sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
                .live_allocation_count == 0 &&
        sxml_item_heap_read_counts(test_fixture->item_heap).live_cell_count == 0;
    if (!is_clean) {
        (void)fprintf(
            stderr, "leak: %llu blocks, %llu cells\n",
            (unsigned long long)sxml_counting_allocator_read_counts(
                test_fixture->counting_allocator)
                .live_allocation_count,
            (unsigned long long)sxml_item_heap_read_counts(test_fixture->item_heap)
                .live_cell_count);
    }
    return is_clean;
}

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* ------------------------------------------- A test-only library part */

static const sxml_static_error test_error_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOER0000"),
    .description = SXML_BYTE_VIEW_INITIALIZER("fn:error() was called"),
};

static sxml_status make_string(sxml_item_heap *item_heap, sxml_byte_view text,
                               sxml_sequence *result) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = text,
                                                   .codepoint_count = text.byte_count,
                                                   .is_ascii = true}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* The canonical text of an optional atomic argument, "" when empty: text
   views output (or the item's own bytes). */
typedef struct argument_text {
    uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    sxml_byte_view text;
} argument_text;

static sxml_status read_argument_text(const sxml_function_call *call,
                                      size_t argument_index, argument_text *text) {
    text->text = (sxml_byte_view){.bytes = text->output, .byte_count = 0};
    const sxml_sequence argument = call->arguments.sequences[argument_index];
    if (sxml_sequence_get_representation(argument) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return SXML_STATUS_OK;
    }
    sxml_canonical_text canonical;
    const sxml_status status = sxml_cast_write_canonical_text(
        call->item_heap,
        &(sxml_canonical_text_request){
            .item = argument.content,
            .output = {.bytes = text->output, .byte_count = sizeof(text->output)}},
        &canonical);
    if (status == SXML_STATUS_OK) {
        text->text = canonical.text;
    }
    return status;
}

static sxml_status call_true(const sxml_function_call *call, sxml_sequence *result) {
    (void)call;
    *result = sxml_sequence_from_item(sxml_atomic_item_true());
    return SXML_STATUS_OK;
}

static sxml_status call_false(const sxml_function_call *call, sxml_sequence *result) {
    (void)call;
    *result = sxml_sequence_from_item(sxml_atomic_item_false());
    return SXML_STATUS_OK;
}

static sxml_status call_error(const sxml_function_call *call, sxml_sequence *result) {
    (void)result;
    return sxml_error_record_report(call->error_record, &test_error_error);
}

static sxml_status call_count(const sxml_function_call *call, sxml_sequence *result) {
    uint64_t item_count = 0;
    const sxml_status status = sxml_sequence_count(call->arguments.sequences[0],
                                                   call->error_record, &item_count);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)item_count));
    }
    return status;
}

static sxml_status call_boolean(const sxml_function_call *call, sxml_sequence *result) {
    sxml_effective_boolean_value truth = {.is_true = false};
    const sxml_status status = sxml_evaluator_compute_effective_boolean_value(
        call->arguments.sequences[0], call->error_record, &truth);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(truth.is_true ? sxml_atomic_item_true()
                                                        : sxml_atomic_item_false());
    }
    return status;
}

static sxml_status call_data(const sxml_function_call *call, sxml_sequence *result) {
    return sxml_evaluator_atomize(
        call->dynamic_context,
        &(sxml_atomization_request){.value = call->arguments.sequences[0],
                                    .error_record = call->error_record},
        result);
}

/* fn:concat over the canonical text of each argument. */
static sxml_status call_concat(const sxml_function_call *call, sxml_sequence *result) {
    char text[512];
    size_t byte_count = 0;
    for (size_t argument_index = 0; argument_index < call->arguments.sequence_count;
         argument_index += 1) {
        argument_text output;
        sxml_byte_view part;
        const sxml_status status = read_argument_text(call, argument_index, &output);
        part = output.text;
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (byte_count + part.byte_count > sizeof(text)) {
            return SXML_STATUS_CAPACITY_EXCEEDED;
        }
        if (part.byte_count > 0) {
            memcpy(text + byte_count, part.bytes, part.byte_count);
        }
        byte_count += part.byte_count;
    }
    return make_string(call->item_heap,
                       (sxml_byte_view){.bytes = (const uint8_t *)text,
                                        .byte_count = byte_count},
                       result);
}

/* ASCII fn:upper-case. */
static sxml_status call_upper_case(const sxml_function_call *call, sxml_sequence *result) {
    argument_text output;
    sxml_byte_view text;
    const sxml_status status = read_argument_text(call, 0, &output);
    text = output.text;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t upper[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        upper[byte_index] = byte >= 'a' && byte <= 'z' ? (uint8_t)(byte - 'a' + 'A') : byte;
    }
    return make_string(call->item_heap,
                       (sxml_byte_view){.bytes = upper, .byte_count = text.byte_count},
                       result);
}

static sxml_status call_starts_with(const sxml_function_call *call, sxml_sequence *result) {
    argument_text first_output;
    argument_text second_output;
    sxml_byte_view first;
    sxml_byte_view second;
    sxml_status status = read_argument_text(call, 0, &first_output);
    first = first_output.text;
    if (status == SXML_STATUS_OK) {
        status = read_argument_text(call, 1, &second_output);
        second = second_output.text;
    }
    if (status == SXML_STATUS_OK) {
        const bool starts = second.byte_count <= first.byte_count &&
                            (second.byte_count == 0 ||
                             memcmp(first.bytes, second.bytes, second.byte_count) == 0);
        *result = sxml_sequence_from_item(starts ? sxml_atomic_item_true()
                                                 : sxml_atomic_item_false());
    }
    return status;
}

static sxml_status call_string_length(const sxml_function_call *call,
                                      sxml_sequence *result) {
    argument_text output;
    sxml_byte_view text;
    const sxml_status status = read_argument_text(call, 0, &output);
    text = output.text;
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(
            sxml_atomic_item_from_integer((int64_t)text.byte_count));
    }
    return status;
}

/* ASCII fn:substring($value, $start, $length) with integral doubles. */
static sxml_status call_substring(const sxml_function_call *call, sxml_sequence *result) {
    argument_text output;
    sxml_byte_view text;
    sxml_atomic_datum start;
    sxml_atomic_datum length;
    sxml_status status = read_argument_text(call, 0, &output);
    text = output.text;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(call->arguments.sequences[1].content, &start);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(call->arguments.sequences[2].content, &length);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const int64_t first = (int64_t)start.datum_details.double_number;
    const int64_t count = (int64_t)length.datum_details.double_number;
    size_t begin = first < 1 ? 0 : (size_t)(first - 1);
    size_t end = (size_t)(first - 1 + count);
    begin = begin > text.byte_count ? text.byte_count : begin;
    end = end > text.byte_count ? text.byte_count : end;
    end = end < begin ? begin : end;
    return make_string(call->item_heap,
                       (sxml_byte_view){.bytes = text.bytes + begin, .byte_count = end - begin},
                       result);
}

/* fn:tokenize#1: split on spaces. */
static sxml_status call_tokenize(const sxml_function_call *call, sxml_sequence *result) {
    argument_text output;
    sxml_byte_view text;
    sxml_status status = read_argument_text(call, 0, &output);
    text = output.text;
    sxml_item tokens[32];
    size_t token_count = 0;
    size_t start = 0;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index <= text.byte_count;
         byte_index += 1) {
        if (byte_index < text.byte_count && text.bytes[byte_index] != ' ') {
            continue;
        }
        if (byte_index > start && token_count < 32) {
            sxml_sequence token = sxml_sequence_empty();
            status = make_string(call->item_heap,
                                 (sxml_byte_view){.bytes = text.bytes + start,
                                                  .byte_count = byte_index - start},
                                 &token);
            tokens[token_count] = token.content;
            token_count += status == SXML_STATUS_OK ? 1 : 0;
        }
        start = byte_index + 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            call->item_heap, (sxml_item_view){.items = tokens, .item_count = token_count},
            result);
    }
    for (size_t token_index = 0; token_index < token_count; token_index += 1) {
        sxml_item_release(tokens[token_index]);
    }
    return status;
}

/* One level of fn:deep-equal: atomic items, and maps and arrays whose
   entries or members are atomic, enough for the slice lines here. */
static bool shallow_sequences_equal(sxml_sequence first, sxml_sequence second);

static bool shallow_items_equal(sxml_item first, sxml_item second) {
    const sxml_item_kind kind = sxml_item_get_kind(first);
    if (kind != sxml_item_get_kind(second)) {
        return false;
    }
    if (kind == SXML_ITEM_KIND_ATOMIC) {
        return sxml_atomic_item_is_atomic_equal(first, second);
    }
    size_t first_count = 0;
    size_t second_count = 0;
    if (kind == SXML_ITEM_KIND_MAP) {
        if (sxml_map_get_size(first, &first_count) != SXML_STATUS_OK ||
            sxml_map_get_size(second, &second_count) != SXML_STATUS_OK ||
            first_count != second_count) {
            return false;
        }
        for (size_t entry_index = 0; entry_index < first_count; entry_index += 1) {
            sxml_map_entry entry;
            sxml_map_lookup lookup;
            if (sxml_map_get_entry(first, entry_index, &entry) != SXML_STATUS_OK ||
                sxml_map_find(second, entry.key, &lookup) != SXML_STATUS_OK ||
                !lookup.is_found || !shallow_sequences_equal(entry.value, lookup.entry.value)) {
                return false;
            }
        }
        return true;
    }
    if (kind == SXML_ITEM_KIND_ARRAY) {
        if (sxml_array_get_size(first, &first_count) != SXML_STATUS_OK ||
            sxml_array_get_size(second, &second_count) != SXML_STATUS_OK ||
            first_count != second_count) {
            return false;
        }
        for (size_t member_index = 0; member_index < first_count; member_index += 1) {
            sxml_sequence first_member = sxml_sequence_empty();
            sxml_sequence second_member = sxml_sequence_empty();
            if (sxml_array_get_member(first, member_index, &first_member) != SXML_STATUS_OK ||
                sxml_array_get_member(second, member_index, &second_member) !=
                    SXML_STATUS_OK ||
                !shallow_sequences_equal(first_member, second_member)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/* Atomic items only: no recursion into nested maps or arrays. */
static bool shallow_sequences_equal(sxml_sequence first, sxml_sequence second) {
    uint64_t first_count = 0;
    uint64_t second_count = 0;
    if (sxml_sequence_count(first, NULL, &first_count) != SXML_STATUS_OK ||
        sxml_sequence_count(second, NULL, &second_count) != SXML_STATUS_OK ||
        first_count != second_count) {
        return false;
    }
    for (uint64_t item_index = 0; item_index < first_count; item_index += 1) {
        sxml_item first_item = {.representation = 0};
        sxml_item second_item = {.representation = 0};
        if (sxml_sequence_get_item(first, (sxml_item_request){.item_index = item_index},
                                   &first_item) != SXML_STATUS_OK ||
            sxml_sequence_get_item(second, (sxml_item_request){.item_index = item_index},
                                   &second_item) != SXML_STATUS_OK ||
            sxml_item_get_kind(first_item) != SXML_ITEM_KIND_ATOMIC ||
            !sxml_atomic_item_is_atomic_equal(first_item, second_item)) {
            return false;
        }
    }
    return true;
}

static sxml_status call_deep_equal(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence first = call->arguments.sequences[0];
    const sxml_sequence second = call->arguments.sequences[1];
    uint64_t first_count = 0;
    uint64_t second_count = 0;
    bool is_equal = sxml_sequence_count(first, NULL, &first_count) == SXML_STATUS_OK &&
                    sxml_sequence_count(second, NULL, &second_count) == SXML_STATUS_OK &&
                    first_count == second_count;
    for (uint64_t item_index = 0; is_equal && item_index < first_count; item_index += 1) {
        sxml_item first_item = {.representation = 0};
        sxml_item second_item = {.representation = 0};
        is_equal = sxml_sequence_get_item(first, (sxml_item_request){.item_index = item_index},
                                          &first_item) == SXML_STATUS_OK &&
                   sxml_sequence_get_item(second,
                                          (sxml_item_request){.item_index = item_index},
                                          &second_item) == SXML_STATUS_OK &&
                   shallow_items_equal(first_item, second_item);
    }
    *result = sxml_sequence_from_item(is_equal ? sxml_atomic_item_true()
                                               : sxml_atomic_item_false());
    return SXML_STATUS_OK;
}

/* fn:subsequence as a stub (no slice line here calls it). */
static sxml_status call_subsequence(const sxml_function_call *call, sxml_sequence *result) {
    sxml_sequence_retain(call->arguments.sequences[0]);
    *result = call->arguments.sequences[0];
    return SXML_STATUS_OK;
}

static sxml_status call_jtree(const sxml_function_call *call, sxml_sequence *result) {
    sxml_item json_node = {.representation = 0};
    const sxml_status status =
        sxml_json_node_make_root(call->item_heap, call->arguments.sequences[0], &json_node);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(json_node);
    }
    return status;
}

/* fn:QName($uri, $qname) without a prefix check. */
static sxml_status call_qname(const sxml_function_call *call, sxml_sequence *result) {
    argument_text uri_output;
    argument_text name_output;
    sxml_byte_view uri = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view qualified_name = {.bytes = NULL, .byte_count = 0};
    sxml_status status = read_argument_text(call, 0, &uri_output);
    uri = uri_output.text;
    if (status == SXML_STATUS_OK) {
        status = read_argument_text(call, 1, &name_output);
        qualified_name = name_output.text;
    }
    size_t colon = 0;
    while (colon < qualified_name.byte_count && qualified_name.bytes[colon] != ':') {
        colon += 1;
    }
    const size_t local_start = colon < qualified_name.byte_count ? colon + 1 : 0;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_name_identifier name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(name_pool, uri, &namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){
                .namespace_uri = namespace_uri,
                .local_name = {.bytes = qualified_name.bytes + local_start,
                               .byte_count = qualified_name.byte_count - local_start}},
            &name);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                          .datum_details = {.qname = {.expanded_name = name,
                                                      .namespace_prefix = {.index = 0}}}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

#define PARAMETER(name_text, type_text)                                       \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                 \
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER(type_text),                 \
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED}

static const sxml_function_parameter input_parameters[] = {PARAMETER("input", "item()*")};
static const sxml_function_parameter two_input_parameters[] = {
    PARAMETER("input1", "item()*"), PARAMETER("input2", "item()*")};
static const sxml_function_parameter string_parameters[] = {
    PARAMETER("value", "xs:string?")};
static const sxml_function_parameter two_string_parameters[] = {
    PARAMETER("value", "xs:string?"), PARAMETER("substring", "xs:string?")};
static const sxml_function_parameter substring_parameters[] = {
    PARAMETER("value", "xs:string?"), PARAMETER("start", "xs:double"),
    PARAMETER("length", "xs:double")};
static const sxml_function_parameter subsequence_parameters[] = {
    PARAMETER("input", "item()*"), PARAMETER("start", "xs:double"),
    PARAMETER("length", "xs:double")};
static const sxml_function_parameter jtree_parameters[] = {PARAMETER("input", "item()?")};
static const sxml_function_parameter qname_parameters[] = {
    PARAMETER("uri", "xs:string?"), PARAMETER("qname", "xs:string")};
static const sxml_function_parameter concat_parameters[] = {
    PARAMETER("values", "xs:anyAtomicType*")};

#define TEST_ROW(name_text, parameter_array, result_text, property_bits, body) \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                          \
     .properties = (property_bits),                                           \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {(parameter_array),                                        \
                    sizeof(parameter_array) / sizeof((parameter_array)[0])},  \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                  \
     .call = (body),                                                          \
     .resumable_body = NULL}

#define NULLARY_ROW(name_text, result_text, body)                             \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                          \
     .properties = 0,                                                         \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {NULL, 0},                                                 \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                  \
     .call = (body),                                                          \
     .resumable_body = NULL}

static const sxml_native_function test_rows[] = {
    NULLARY_ROW("true", "xs:boolean", call_true),
    NULLARY_ROW("false", "xs:boolean", call_false),
    NULLARY_ROW("error", "item()*", call_error),
    TEST_ROW("count", input_parameters, "xs:integer", 0, call_count),
    TEST_ROW("boolean", input_parameters, "xs:boolean", 0, call_boolean),
    TEST_ROW("data", input_parameters, "xs:anyAtomicType*", 0, call_data),
    TEST_ROW("concat", concat_parameters, "xs:string", SXML_NATIVE_FUNCTION_IS_VARIADIC,
             call_concat),
    TEST_ROW("upper-case", string_parameters, "xs:string", 0, call_upper_case),
    TEST_ROW("starts-with", two_string_parameters, "xs:boolean", 0, call_starts_with),
    TEST_ROW("string-length", string_parameters, "xs:integer", 0, call_string_length),
    TEST_ROW("substring", substring_parameters, "xs:string", 0, call_substring),
    TEST_ROW("tokenize", string_parameters, "xs:string*", 0, call_tokenize),
    TEST_ROW("deep-equal", two_input_parameters, "xs:boolean", 0, call_deep_equal),
    TEST_ROW("subsequence", subsequence_parameters, "item()*", 0, call_subsequence),
    TEST_ROW("jtree", jtree_parameters, "jnode()?", 0, call_jtree),
    TEST_ROW("QName", qname_parameters, "xs:QName", 0, call_qname),
};

static const sxml_native_function_view test_part = {
    .native_functions = test_rows,
    .native_function_count = sizeof(test_rows) / sizeof(test_rows[0]),
};

/* --------------------------------------------------- Function table */

/*
 * A table over the parts under test and the test part, resolving type
 * text the way the function library does (D55); the library's own index
 * (src/function_library.c) is packet 1.20's.
 */
#define TABLE_ROW_LIMIT 128
#define TABLE_PARAMETER_LIMIT 12

typedef struct test_table {
    sxml_item_heap *item_heap;
    sxml_allocator allocator;
    const sxml_native_function *rows[TABLE_ROW_LIMIT];
    size_t row_count;
    sxml_name_identifier row_names[TABLE_ROW_LIMIT];
    bool is_resolved[TABLE_ROW_LIMIT];
    sxml_sequence_type parameter_types[TABLE_ROW_LIMIT][TABLE_PARAMETER_LIMIT];
    sxml_sequence_type result_types[TABLE_ROW_LIMIT];
    sxml_sequence_type constructor_parameter_type;
} test_table;

static sxml_status resolve_type_text(const test_table *table, sxml_byte_view type_text,
                                     sxml_sequence_type *sequence_type) {
    sxml_ast *ast = NULL;
    sxml_status status = sxml_xpath_build_sequence_type_ast(
        &(sxml_ast_build_request){.expression_text = type_text,
                                  .allocator = table->allocator},
        &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xpath_static_context static_context = {.item_heap = table->item_heap,
                                                      .allocator = table->allocator};
    status = sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = ast,
                                   .type_node = ast->root,
                                   .static_context = &static_context,
                                   .error_record = NULL},
        sequence_type);
    sxml_ast_destroy(ast);
    return status;
}

static sxml_status resolve_row(test_table *table, size_t row_index) {
    if (table->is_resolved[row_index]) {
        return SXML_STATUS_OK;
    }
    const sxml_native_function *row = table->rows[row_index];
    const size_t parameter_count = row->parameters.function_parameter_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t parameter_index = 0; status == SXML_STATUS_OK && parameter_count > 0 &&
                                     parameter_index < TABLE_PARAMETER_LIMIT;
         parameter_index += 1) {
        const size_t source_index =
            parameter_index < parameter_count ? parameter_index : parameter_count - 1;
        status = resolve_type_text(
            table, row->parameters.function_parameters[source_index].parameter_type,
            &table->parameter_types[row_index][parameter_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_type_text(table, row->result_type, &table->result_types[row_index]);
    }
    if (status != SXML_STATUS_OK) {
        (void)fprintf(stderr, "type text of %.*s does not resolve\n",
                      (int)row->local_name.byte_count, (const char *)row->local_name.bytes);
    }
    table->is_resolved[row_index] = status == SXML_STATUS_OK;
    return status;
}

static bool row_accepts(const sxml_native_function *row, size_t arity) {
    const size_t parameter_count = row->parameters.function_parameter_count;
    size_t required_count = 0;
    while (required_count < parameter_count &&
           row->parameters.function_parameters[required_count].parameter_default ==
               SXML_PARAMETER_DEFAULT_REQUIRED) {
        required_count += 1;
    }
    if (arity > TABLE_PARAMETER_LIMIT || arity < required_count) {
        return false;
    }
    return (row->properties & SXML_NATIVE_FUNCTION_IS_VARIADIC) != 0 ||
           arity <= parameter_count;
}

static sxml_status register_signature(const test_table *table,
                                      const sxml_function_entry *entry,
                                      sxml_type_identifier *signature) {
    return sxml_type_registry_register_item_type(
        sxml_item_heap_get_type_registry(table->item_heap),
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {.is_any_function = false,
                                                  .parameter_types = entry->parameter_types,
                                                  .result_type = entry->result_type}}},
        signature);
}

static sxml_status find_constructor(test_table *table, const sxml_function_query *query,
                                    sxml_function_entry *entry) {
    sxml_type_registry *registry = sxml_item_heap_get_type_registry(table->item_heap);
    sxml_type_identifier type = {0};
    sxml_type_description description = {0};
    if (query->arity != 1 ||
        !sxml_type_registry_find_type(registry, query->function_name, &type) ||
        sxml_type_registry_describe_type(registry, type, &description) != SXML_STATUS_OK ||
        description.variety != SXML_TYPE_VARIETY_ATOMIC ||
        type.index == SXML_TYPE_XS_ANY_ATOMIC_TYPE || type.index == SXML_TYPE_XS_NOTATION) {
        return SXML_STATUS_OK;
    }
    sxml_function_entry found = {
        .entry_kind = SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR,
        .constructor_type = type,
        .parameter_types = {.sequence_types = &table->constructor_parameter_type,
                            .sequence_type_count = 1},
        .result_type = {.item_type = type, .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE}};
    const sxml_status status = register_signature(table, &found, &found.signature);
    if (status == SXML_STATUS_OK) {
        *entry = found;
    }
    return status;
}

static sxml_status find_test_function(void *state, const sxml_function_query *query,
                                      sxml_function_entry *entry) {
    test_table *table = state;
    *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    sxml_name_parts parts = {0};
    if (sxml_name_pool_get_name_parts(sxml_item_heap_get_name_pool(table->item_heap),
                                      query->function_name, &parts) == SXML_STATUS_OK &&
        parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA) {
        return find_constructor(table, query, entry);
    }
    for (size_t row_index = 0; row_index < table->row_count; row_index += 1) {
        if (table->row_names[row_index].index != query->function_name.index ||
            !row_accepts(table->rows[row_index], query->arity)) {
            continue;
        }
        sxml_status status = resolve_row(table, row_index);
        sxml_function_entry found = {
            .entry_kind = SXML_FUNCTION_ENTRY_KIND_NATIVE,
            .native_function = table->rows[row_index],
            .parameters = table->rows[row_index]->parameters,
            .parameter_types = {.sequence_types = table->parameter_types[row_index],
                                .sequence_type_count = query->arity},
            .result_type = table->result_types[row_index]};
        if (status == SXML_STATUS_OK) {
            status = register_signature(table, &found, &found.signature);
        }
        if (status == SXML_STATUS_OK) {
            *entry = found;
        }
        return status;
    }
    return SXML_STATUS_OK;
}

static const sxml_function_table_operations test_table_operations = {
    .find_function = find_test_function,
};

/* The JNode accessors fn:jkey and fn:jvalue (src/function_library_jnode.c,
   packet 2.6, D115); the generated part registry declares the view. */
extern const sxml_native_function_view sxml_function_library_jnode;

static const sxml_native_function_view *const table_parts[] = {
    &sxml_function_library_higher_order,
    &sxml_function_library_jnode,
    &sxml_function_library_map,
    &sxml_function_library_array,
    /* fn:abs, fn:round (round#2 for fn-partial-apply-01), fn:avg. */
    &sxml_function_library_numeric,
    &test_part,
};

static sxml_status test_table_initialize(const fixture *test_fixture, test_table *table) {
    memset(table, 0, sizeof(*table));
    table->item_heap = test_fixture->item_heap;
    table->allocator = sxml_default_allocator();
    table->constructor_parameter_type = (sxml_sequence_type){
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
    for (size_t part_index = 0; part_index < sizeof(table_parts) / sizeof(table_parts[0]);
         part_index += 1) {
        const sxml_native_function_view *part = table_parts[part_index];
        for (size_t row_index = 0; row_index < part->native_function_count; row_index += 1) {
            if (table->row_count == TABLE_ROW_LIMIT) {
                return SXML_STATUS_CAPACITY_EXCEEDED;
            }
            const sxml_native_function *row = &part->native_functions[row_index];
            const sxml_status status = sxml_name_pool_intern_name(
                test_fixture->name_pool,
                &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                         row->function_namespace),
                                     .local_name = row->local_name},
                &table->row_names[table->row_count]);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            table->rows[table->row_count] = row;
            table->row_count += 1;
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Evaluating */

/* An expression with an optional evaluation depth limit. */
typedef struct evaluation_case {
    const char *expression_text;
    uint32_t maximum_evaluation_depth;
} evaluation_case;

typedef struct evaluation_outcome {
    sxml_status status;
    char code[32];
    char text[4096];
} evaluation_outcome;

static void outcome_append(evaluation_outcome *outcome, sxml_byte_view text) {
    const size_t used = strlen(outcome->text);
    size_t byte_count = text.byte_count;
    if (used + byte_count + 1 > sizeof(outcome->text)) {
        byte_count = sizeof(outcome->text) - used - 1;
    }
    if (byte_count > 0) {
        memcpy(outcome->text + used, text.bytes, byte_count);
    }
    outcome->text[used + byte_count] = '\0';
}

/* Writes value as text: atomic items canonically, other items by kind. */
static sxml_status write_value(const fixture *test_fixture, sxml_sequence value,
                               evaluation_outcome *outcome) {
    outcome->text[0] = '\0';
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    bool is_first = true;
    for (;;) {
        sxml_iteration_step step;
        sxml_status status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!step.has_item) {
            return SXML_STATUS_OK;
        }
        if (!is_first) {
            outcome_append(outcome, text_view(" "));
        }
        is_first = false;
        switch (sxml_item_get_kind(step.item)) {
        case SXML_ITEM_KIND_ATOMIC: {
            uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
            sxml_canonical_text canonical;
            status = sxml_cast_write_canonical_text(
                test_fixture->item_heap,
                &(sxml_canonical_text_request){
                    .item = step.item,
                    .output = {.bytes = output, .byte_count = sizeof(output)}},
                &canonical);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            outcome_append(outcome, canonical.text);
            break;
        }
        case SXML_ITEM_KIND_NODE:
            outcome_append(outcome, text_view("#node"));
            break;
        case SXML_ITEM_KIND_JSON_NODE:
            outcome_append(outcome, text_view("#jnode"));
            break;
        case SXML_ITEM_KIND_FUNCTION:
            outcome_append(outcome, text_view("#function"));
            break;
        case SXML_ITEM_KIND_MAP:
            outcome_append(outcome, text_view("#map"));
            break;
        case SXML_ITEM_KIND_ARRAY:
            outcome_append(outcome, text_view("#array"));
            break;
        }
    }
}

typedef struct evaluation_environment {
    fixture *test_fixture;
    test_table *table;
} evaluation_environment;

static void evaluate_with(evaluation_environment environment,
                          const evaluation_case *test_case, evaluation_outcome *outcome) {
    fixture *test_fixture = environment.test_fixture;
    *outcome = (evaluation_outcome){.status = SXML_STATUS_OK};
    const sxml_xpath_static_context static_context = {
        .item_heap = test_fixture->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .function_table = {.state = environment.table, .operations = &test_table_operations},
    };
    sxml_error_record error_record = {.value = NULL};
    sxml_ir_unit *unit = NULL;
    outcome->status = sxml_xpath_compile(
        &(sxml_xpath_compile_request){
            .expression_text = text_view(test_case->expression_text),
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xpath"),
            .static_context = &static_context,
            .error_record = &error_record},
        &unit);
    sxml_dynamic_context *dynamic_context = NULL;
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_dynamic_context_create(
            fixture_allocator(test_fixture),
            &(sxml_evaluator_configuration){
                .item_heap = test_fixture->item_heap,
                .allocator = fixture_allocator(test_fixture),
                .function_table = static_context.function_table,
                .maximum_evaluation_depth = test_case->maximum_evaluation_depth},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit, .error_record = &error_record},
            &value);
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = write_value(test_fixture, value, outcome);
    } else if (sxml_status_has_error_record(outcome->status)) {
        const size_t byte_count = error_record.code.local_name.byte_count <
                                          sizeof(outcome->code) - 1
                                      ? error_record.code.local_name.byte_count
                                      : sizeof(outcome->code) - 1;
        if (byte_count > 0) {
            memcpy(outcome->code, error_record.code.local_name.bytes, byte_count);
        }
        outcome->code[byte_count] = '\0';
    }
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_ir_unit_release(unit);
}

/* A fixture and table for one case. Static: the 64 KiB stack of the
   WebAssembly tests is not for test buffers. */
typedef struct evaluation_fixture {
    fixture test_fixture;
    test_table table;
    evaluation_outcome outcome;
} evaluation_fixture;

static evaluation_fixture shared_fixture;

static bool evaluation_fixture_start(evaluation_fixture *evaluation,
                                     uint64_t failing_allocation_number) {
    if (fixture_create(failing_allocation_number, &evaluation->test_fixture) !=
            SXML_STATUS_OK ||
        test_table_initialize(&evaluation->test_fixture, &evaluation->table) !=
            SXML_STATUS_OK) {
        fixture_destroy(&evaluation->test_fixture);
        return false;
    }
    return true;
}

static evaluation_environment evaluation_fixture_environment(evaluation_fixture *evaluation) {
    return (evaluation_environment){.test_fixture = &evaluation->test_fixture,
                                    .table = &evaluation->table};
}

/* True when test_case gives expected_text and nothing leaks. */
static bool case_evaluates_to(const evaluation_case *test_case, const char *expected_text) {
    evaluation_fixture *evaluation = &shared_fixture;
    if (!evaluation_fixture_start(evaluation, 0)) {
        return false;
    }
    evaluate_with(evaluation_fixture_environment(evaluation), test_case, &evaluation->outcome);
    const bool is_equal = evaluation->outcome.status == SXML_STATUS_OK &&
                          strcmp(evaluation->outcome.text, expected_text) == 0;
    if (!is_equal) {
        (void)fprintf(stderr, "for %s\n  status %d %s, expected \"%s\", got \"%s\"\n",
                      test_case->expression_text, (int)evaluation->outcome.status,
                      evaluation->outcome.code, expected_text, evaluation->outcome.text);
    }
    const bool is_clean = fixture_released_everything(&evaluation->test_fixture);
    fixture_destroy(&evaluation->test_fixture);
    return is_equal && is_clean;
}

static bool evaluates_to(const char *expression_text, const char *expected_text) {
    return case_evaluates_to(&(evaluation_case){.expression_text = expression_text},
                             expected_text);
}

/* True when the expression fails with the error code (and its status),
   and nothing leaks. */
static bool fails_with(const char *expression_text, const char *code) {
    const sxml_status expected_status =
        strncmp(code, "XPST", 4) == 0 ? SXML_STATUS_STATIC_ERROR
        : strncmp(code, "XPTY", 4) == 0 || strncmp(code, "FOTY", 4) == 0 ||
                strncmp(code, "FORG0006", 8) == 0
            ? SXML_STATUS_TYPE_ERROR
            : SXML_STATUS_DYNAMIC_ERROR;
    evaluation_fixture *evaluation = &shared_fixture;
    if (!evaluation_fixture_start(evaluation, 0)) {
        return false;
    }
    evaluate_with(evaluation_fixture_environment(evaluation),
                  &(evaluation_case){.expression_text = expression_text},
                  &evaluation->outcome);
    const bool is_expected = evaluation->outcome.status == expected_status &&
                             strcmp(evaluation->outcome.code, code) == 0;
    if (!is_expected) {
        (void)fprintf(stderr, "for %s\n  expected %d %s, got %d %s \"%s\"\n",
                      expression_text, (int)expected_status, code,
                      (int)evaluation->outcome.status, evaluation->outcome.code,
                      evaluation->outcome.text);
    }
    const bool is_clean = fixture_released_everything(&evaluation->test_fixture);
    fixture_destroy(&evaluation->test_fixture);
    return is_expected && is_clean;
}

/* ------------------------------------------------- Higher-order (slice) */

static void test_higher_order_slice_lines(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("for-each((\"aa\", \"bb\", \"cc\", \"dd\", \"ee\"), "
                                         "upper-case#1)",
                                         "AA BB CC DD EE"));  /* for-each-001 */
    SXML_TEST_EXPECT(state, evaluates_to("let $f := function($x as xs:double*)"
                                         "{for-each($x,round#1)} return $f((1.2345, 6.789))",
                                         "1 7"));  /* for-each-008 */
    SXML_TEST_EXPECT(state, evaluates_to("for-each(22 to 28, fn($it, $pos){$pos, $it})",
                                         "1 22 2 23 3 24 4 25 5 26 6 27 7 28"));
    /* for-each-402 */
    SXML_TEST_EXPECT(state, fails_with("for-each((\"aa\", \"bb\", \"cc\", \"dd\", \"ee\"), "
                                       "starts-with#2)",
                                       "XPTY0004"));  /* for-each-901 */
    SXML_TEST_EXPECT(state, evaluates_to("filter((\"apple\", \"pear\", \"apricot\", "
                                         "\"advocado\", \"orange\"),starts-with(?, \"a\"))",
                                         "apple apricot advocado"));  /* filter-001 */
    SXML_TEST_EXPECT(state, fails_with("fn:filter( fn:boolean#1 )",
                                       "XPST0017"));  /* fn-filter-003 */
    SXML_TEST_EXPECT(state, evaluates_to("count(fn:filter( 1 to 10, function($arg) { if "
                                         "($arg eq 100) then (fn:true(), fn:false()) else "
                                         "fn:true()}))",
                                         "10"));  /* fn-filter-020 */
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 5, 0, function($a, $b) { $a + $b })",
                                         "15"));  /* fold-left-001 */
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 5, (), function($a, $b) {($b, $a)})",
                                         "5 4 3 2 1"));  /* fold-left-005 */
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 5, \"$zero\", fn:concat(\"$f(\", ?, "
                                         "\", \", ?, \")\"))",
                                         "$f($f($f($f($f($zero, 1), 2), 3), 4), 5)"));
    /* fold-left-007 */
    SXML_TEST_EXPECT(state, fails_with("fold-left(1 to 5, 1, function($a, $b) as element(foo) "
                                       "{ $a + $b })",
                                       "XPTY0004"));  /* fold-left-011 */
    SXML_TEST_EXPECT(state, evaluates_to("fold-right(1 to 5, 0,function($a, $b) { $a + $b })",
                                         "15"));  /* fold-right-001 */
    SXML_TEST_EXPECT(state, evaluates_to("for-each-pair((\"aa\", \"bb\", \"cc\", \"dd\", "
                                         "\"ee\"), (\"AA\", \"BB\", \"cc\", \"dd\", \"EE\"), "
                                         "deep-equal#2)",
                                         "false false true true false"));
    /* for-each-pair-001 */
    SXML_TEST_EXPECT(state, evaluates_to("fn:for-each-pair( 1 to 3, 1 to 3, function($arg1, "
                                         "$arg2) { ($arg1, $arg2) })",
                                         "1 1 2 2 3 3"));  /* fn-for-each-pair-027 */
    SXML_TEST_EXPECT(state, evaluates_to("apply(abs#1, [-2])", "2"));  /* fn-apply-01 */
    SXML_TEST_EXPECT(state, evaluates_to("apply(substring('flower', ?, ?), [ 3, 2 ])",
                                         "ow"));  /* fn-apply-06 */
    SXML_TEST_EXPECT(state, evaluates_to("fn:function-arity( function($arg1, $arg2) "
                                         "{ subsequence($arg1, $arg2, 1) } )",
                                         "2"));  /* fn-function-arity-019 */
    SXML_TEST_EXPECT(state, evaluates_to("function-lookup(fn:QName('http://www.w3.org/2005/"
                                         "xpath-functions', 'avg'), 1)((1, 3))",
                                         "2"));  /* fn-function-lookup-336 */
    SXML_TEST_EXPECT(state, evaluates_to("count(function-name(let $a := 92, $b := true() "
                                         "return function($c) { $a, $b, $c }))",
                                         "0"));  /* function-item-9 */
    SXML_TEST_EXPECT(state, evaluates_to("let $m := map { 'a': 1 } return $m(?) instance of "
                                         "map(*)",
                                         "false"));  /* hof-920 */
    SXML_TEST_EXPECT(state, evaluates_to("let $f := (op('+'), op('-')) return $f(12, 8)",
                                         "20 4"));  /* DynamicFunctionCall-146 */
    SXML_TEST_EXPECT(state, evaluates_to("let $f := (op('+'), op('-'))(12, ?) return $f(5)",
                                         "17 7"));  /* DynamicFunctionCall-149 */
    SXML_TEST_EXPECT(state, evaluates_to("let $usd := \"$\" => concat(?) return $usd(3)",
                                         "$3"));  /* ArrowPostfix-108 */
    SXML_TEST_EXPECT(state, evaluates_to("partial-apply(round#2, {2: 1})(1.23456)",
                                         "1.2"));  /* fn-partial-apply-01 */
    SXML_TEST_EXPECT(state, fails_with("partial-apply(string-length#1, {1 : ('a','b') })",
                                       "XPTY0004"));  /* fn-partial-apply-13 */
    SXML_TEST_EXPECT(state, evaluates_to("let $f := concat#8 return $f('a','b','c','d','e',"
                                         "'f','g','h')",
                                         "abcdefgh"));  /* hof-005 */
    SXML_TEST_EXPECT(state, evaluates_to("\"Tu\" => {\"Mo\": \"Monday\", \"Tu\": "
                                         "\"Tuesday\"}()",
                                         "Tuesday"));  /* ArrowExpr-407 */
}

/* fold-right-013: a million items, constant C stack (D71). */
static void test_fold_right_over_a_million_items(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("fold-right(1 to 1000000, 0, function($a, $b)"
                                         "{ $b + 1 })",
                                         "1000000"));
    SXML_TEST_EXPECT(state, evaluates_to("count(for-each(1 to 200000, function($x) "
                                         "{ $x, $x }))",
                                         "400000"));
}

/* ----------------------------------------- Higher-order (4.0 additions) */

static void test_higher_order_functions(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("filter(1 to 10, fn($x, $p) { $p mod 3 = 0 })",
                                         "3 6 9"));
    SXML_TEST_EXPECT(state, evaluates_to("index-where((1, 8, 2, 7), fn($x) { $x > 5 })",
                                         "2 4"));
    SXML_TEST_EXPECT(state, evaluates_to("take-while(1 to 10, fn($x) { $x < 4 })", "1 2 3"));
    SXML_TEST_EXPECT(state, evaluates_to("drop-while(1 to 6, fn($x) { $x < 4 })", "4 5 6"));
    SXML_TEST_EXPECT(state, evaluates_to("drop-while(1 to 3, fn($x) { true() })", ""));
    SXML_TEST_EXPECT(state, evaluates_to("every((1, 2), fn($x) { $x > 0 }), "
                                         "every((1, 0), fn($x) { $x > 0 }), every(())",
                                         "true false true"));
    SXML_TEST_EXPECT(state, evaluates_to("some((0, 2), fn($x) { $x > 1 }), some((1, 0)), "
                                         "some(()), every((1, 'a'))",
                                         "true true false true"));
    SXML_TEST_EXPECT(state, fails_with("every((1, map{}))", "FORG0006"));
    SXML_TEST_EXPECT(state, evaluates_to("do-until(1, fn($x, $p) { $x * 2 }, "
                                         "fn($x, $p) { $x > 100 })",
                                         "128"));
    SXML_TEST_EXPECT(state, evaluates_to("while-do(1, fn($x, $p) { $p < 5 }, "
                                         "fn($x, $p) { $x + $p })",
                                         "11"));
    SXML_TEST_EXPECT(state, evaluates_to("partition((1, 2, 3, 6, 7, 9, 10), fn($a, $n) "
                                         "{ $n != $a[count($a)] + 1 }) ! array:size(.)",
                                         "3 2 2"));
    SXML_TEST_EXPECT(state, evaluates_to("partition((), fn($a, $n) { true() })", ""));
    SXML_TEST_EXPECT(state, evaluates_to("scan(1 to 4, 0, op('+')) ! ?*", "0 1 3 6 10"));
    SXML_TEST_EXPECT(state, evaluates_to("highest((3, 1, 3, 2)), lowest(('b', 'a', 'c')), "
                                         "highest((-3, 2), (), abs#1)",
                                         "3 3 a -3"));
    SXML_TEST_EXPECT(state, evaluates_to("lowest(())", ""));
    SXML_TEST_EXPECT(state, evaluates_to("sort-by((3, 1, 2), {'order': 'descending'})",
                                         "3 2 1"));
    SXML_TEST_EXPECT(state, evaluates_to("sort-by(('bb', 'a', 'ccc', 'dd'), "
                                         "({'key': string-length#1}, {'order': 'descending'}))",
                                         "a dd bb ccc"));
    SXML_TEST_EXPECT(state, evaluates_to("sort-by((2, 1), ())", "1 2"));
    SXML_TEST_EXPECT(state, fails_with("sort-by((1, 'a'), ())", "XPTY0004"));
    SXML_TEST_EXPECT(state, fails_with("sort-by((1, 2), {'collation': 'urn:x'})",
                                       "FOCH0002"));
    SXML_TEST_EXPECT(state, evaluates_to("sort-with((5, 3, 9, 1, 7, 2), fn($a, $b) "
                                         "{ $b - $a })",
                                         "9 7 5 3 2 1"));
    SXML_TEST_EXPECT(state, evaluates_to("sort-with(('ab', 'b', 'aa', 'c', 'a'), "
                                         "(fn($a, $b) { string-length($a) - "
                                         "string-length($b) }, fn($a, $b) { 0 }))",
                                         "b c a ab aa"));
    SXML_TEST_EXPECT(state, evaluates_to("sort-with((), fn($a, $b) { 0 })", ""));
    SXML_TEST_EXPECT(state, evaluates_to("apply(concat#3, ['a', 'b', 'c', 'd'])", "abc"));
    SXML_TEST_EXPECT(state, fails_with("apply(concat#3, ['a'])", "FOAP0001"));
    SXML_TEST_EXPECT(state, evaluates_to("partial-apply(substring#3, {1: 'flower', 3: 2})(3)",
                                         "ow"));
    SXML_TEST_EXPECT(state, evaluates_to("let $f := partial-apply(concat#3, {2: '-'}) "
                                         "return (function-arity($f), $f('a', 'b'))",
                                         "2 a-b"));
    SXML_TEST_EXPECT(state, evaluates_to("partial-apply(concat#2, {1: 'a', 2: 'b'})()",
                                         "ab"));
    SXML_TEST_EXPECT(state, evaluates_to("partial-apply(concat#2, {})('x', 'y'), "
                                         "partial-apply(concat#2, {7: 1})('x', 'y')",
                                         "xy xy"));
    SXML_TEST_EXPECT(state, evaluates_to("partial-apply(['a', 'b'], {1: 2})()", "b"));
    SXML_TEST_EXPECT(state, evaluates_to("function-name(upper-case#1), "
                                         "function-name(map:size#1), function-arity(map{})",
                                         "fn:upper-case map:size 1"));
    SXML_TEST_EXPECT(state, evaluates_to("count(function-annotations(true#0))", "0"));
    SXML_TEST_EXPECT(state, evaluates_to("function-lookup(xs:QName('fn:nothing'), 1), "
                                         "function-lookup(xs:QName('fn:count'), -1), "
                                         "function-lookup(xs:QName('xs:integer'), 1)('12') + 1",
                                         "13"));
    SXML_TEST_EXPECT(state, evaluates_to("op(',')(1, 2), op('and')(1, 0), op('or')(1, 0), "
                                         "op('*')(6, 7), op('idiv')(7, 2), op('mod')(7, 2)",
                                         "1 2 false true 42 3 1"));
    SXML_TEST_EXPECT(state, evaluates_to("op('=')((1, 2), 2), op('eq')(1, 2), op('<')(1, 2), "
                                         "op('ge')(2, 2), op('!=')(1, 1), op('ne')(1, 2)",
                                         "true false true true false true"));
    SXML_TEST_EXPECT(state, evaluates_to("op('||')('a', 1), op('to')(2, 4), "
                                         "op('otherwise')((), 5), op('otherwise')(4, 5)",
                                         "a1 2 3 4 5 4"));
    SXML_TEST_EXPECT(state, evaluates_to("op('div')(1, 4), op('-')(1, 4), op('le')(1, 1), "
                                         "op('lt')(2, 1), op('gt')(2, 1), op('>')(2, 1), "
                                         "op('<=')(3, 1), op('>=')(3, 1)",
                                         "0.25 -3 true false true true false true"));
    SXML_TEST_EXPECT(state, fails_with("op('**')", "XPTY0004"));
    SXML_TEST_EXPECT(state, evaluates_to("for-each-pair(21 to 25, 1 to 5, op('+'))",
                                         "22 24 26 28 30"));
}

/* fn:transitive-closure over the GNodes of a JTree (F&O 4.0, 2.5.22). */
static void test_transitive_closure(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("count(transitive-closure(jtree([[1, [2]], 3]), "
                                         "fn($n) { $n/* }))",
                                         "5"));
    SXML_TEST_EXPECT(state, evaluates_to("count(transitive-closure((), fn($n) { $n/* }))",
                                         "0"));
}

/* ------------------------------------------------------ Maps (slice) */

static void test_map_slice_lines(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("map:get(map{1:\"Sunday\",2:\"Monday\",3:\"Tuesday\","
                                         "4:\"Wednesday\",5:\"Thursday\",6:\"Friday\","
                                         "7:\"Saturday\"}, 4)",
                                         "Wednesday"));  /* map-get-001 */
    SXML_TEST_EXPECT(state, evaluates_to("map:get(map{1:\"Sunday\",2:\"Monday\",3:\"Tuesday\","
                                         "xs:anyURI(\"urn:weds\"):\"Wednesday\",5:\"Thursday\","
                                         "6:\"Friday\",7:\"Saturday\"}, xs:double('NaN'))",
                                         ""));  /* map-get-014 */
    SXML_TEST_EXPECT(state, evaluates_to("let $m := map{\"su\":\"Sunday\",\"mo\":\"Monday\","
                                         "\"tu\":\"Tuesday\",\"we\":\"Wednesday\","
                                         "\"th\":\"Thursday\",\"fr\":\"Friday\","
                                         "\"sa\":\"Saturday\"} return map:get($m, \"bi\", "
                                         "\"Birthday\")",
                                         "Birthday"));  /* map-get-401 */
    SXML_TEST_EXPECT(state, evaluates_to("let $m := map:put(map{}, \"foo\", 1 to 5) return "
                                         "(count($m), $m instance of map(xs:string, "
                                         "xs:integer+), $m instance of function(xs:string) as "
                                         "item()*)",
                                         "1 true true"));  /* map-put-002-hof */
    SXML_TEST_EXPECT(state, evaluates_to("let $foo := map:merge(for $i in 1 to 20 return "
                                         "map:entry($i, $i*$i)), $foobar := map:put($foo, 8, "
                                         "63) return (($foobar, $foo)[2] ! (map:size(.), ?8))",
                                         "20 64"));  /* map-put-008 */
    SXML_TEST_EXPECT(state, evaluates_to("deep-equal(map{}, map:remove(map:put(map:merge(()), "
                                         "1, 2), 1))",
                                         "true"));  /* map-put-019 */
    SXML_TEST_EXPECT(state, fails_with("map:merge()", "XPST0017"));  /* map-merge-001 */
    SXML_TEST_EXPECT(state, evaluates_to("map:get(map:merge((map:entry(\"foo\", 3), "
                                         "map:entry(\"foo\", 4))), \"foo\")",
                                         "3"));  /* map-merge-006e */
    SXML_TEST_EXPECT(state, evaluates_to("map:merge(({17:0}, {29:0}, {'a':0}, {-234:0}, "
                                         "{86:0}, {445:0}, {101:0}, {'e':0})) => map:keys()",
                                         "17 29 a -234 86 445 101 e"));  /* map-merge-401 */
    SXML_TEST_EXPECT(state, evaluates_to("map:keys(map:merge((map:entry(\"a\",1), "
                                         "map:entry(\"a\",2))))",
                                         "a"));  /* map-keys-008 */
    SXML_TEST_EXPECT(state, evaluates_to("map:for-each(map:remove(map{\"a\":1,\"b\":2}, "
                                         "\"c\"), function($k,$v){$k})",
                                         "a b"));  /* map-for-each-013 */
    SXML_TEST_EXPECT(state, evaluates_to("map:contains(map{1:\"Sunday\",2:\"Monday\","
                                         "xs:anyURI(\"urn:weds\"):\"Wednesday\"}, "
                                         "xs:double('NaN'))",
                                         "false"));  /* map-contains-014 */
    SXML_TEST_EXPECT(state, evaluates_to("map:size(map:remove(map{\"a\":1,\"b\":2}, \"b\"))",
                                         "1"));  /* map-size-009 */
    SXML_TEST_EXPECT(state, evaluates_to("let $r := map:entry(xs:untypedAtomic(\"foo\"), "
                                         "map{}) return ($r instance of "
                                         "map(xs:untypedAtomic, map(*)), count($r?foo), "
                                         "count($r?bar))",
                                         "true 1 0"));  /* map-entry-004 */
}

/* --------------------------------------------- Maps (the whole of 14.4) */

static void test_map_functions(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("map:empty(map{}), map:empty({1: 2}), "
                                         "map:contains({1: 2}, 1.0)",
                                         "true false true"));
    SXML_TEST_EXPECT(state, evaluates_to("map:entries({'a': 1, 'b': (2, 3)}) ! map:keys(.)",
                                         "a b"));
    SXML_TEST_EXPECT(state, evaluates_to("map:items({'a': 1, 'b': (2, 3)})", "1 2 3"));
    SXML_TEST_EXPECT(state, evaluates_to("map:keys(map:filter({1: 'a', 2: 'b', 3: 'c'}, "
                                         "fn($k, $v) { $k != 2 }))",
                                         "1 3"));
    SXML_TEST_EXPECT(state, evaluates_to("map:for-each({'x': 1, 'y': 2}, fn($k, $v, $p) "
                                         "{ $k || $v || $p })",
                                         "x11 y22"));
    SXML_TEST_EXPECT(state, evaluates_to("map:find(({'a': 1, 'b': [{'a': 2}]}, [{'a': 3}]), "
                                         "'a') ! ?*",
                                         "1 2 3"));
    SXML_TEST_EXPECT(state, evaluates_to("array:size(map:find(1, 'a'))", "0"));
    SXML_TEST_EXPECT(state, evaluates_to("map:keys(map:remove({1: 2, 3: 4, 5: 6}, (1, 5, 9)))",
                                         "3"));
    SXML_TEST_EXPECT(state, evaluates_to("map:get(map:build((1, 2, 3, 4), fn($x) "
                                         "{ $x mod 2 }), 1)",
                                         "1 3"));
    SXML_TEST_EXPECT(state, evaluates_to("map:keys(map:build(('a', 'b')))", "a b"));
    SXML_TEST_EXPECT(state, evaluates_to("map:build(1 to 3, (), fn($x, $p) { $x * 10 + $p "
                                         "})?2",
                                         "22"));
    SXML_TEST_EXPECT(state, evaluates_to("map:build((1, 2, 3), fn($x) { 'k' }, (), "
                                         "{'duplicates': op('+')})?k",
                                         "6"));
    SXML_TEST_EXPECT(state, evaluates_to("map:build((1, 2), fn($x) { ('k', 'j') }, (), "
                                         "{'duplicates': 'use-last'}) ! (?k, ?j)",
                                         "2 2"));
    SXML_TEST_EXPECT(state, fails_with("map:build((1, 1), (), (), {'duplicates': 'reject'})",
                                       "FOJS0003"));
    SXML_TEST_EXPECT(state, fails_with("map:merge(({1: 2}, {1: 3}), {'duplicates': 'no'})",
                                       "FOJS0005"));
    SXML_TEST_EXPECT(state, evaluates_to("map:merge(({1: 2}, {1: 3}), {'duplicates': "
                                         "'use-last'})?1, map:merge(({1: 2}, {1: 3}), "
                                         "{'duplicates': 'combine'})?1, map:merge(({1: 2}, "
                                         "{1: 3}), {'duplicates': fn($a, $b) { $a * $b }})?1",
                                         "3 2 3 6"));
    SXML_TEST_EXPECT(state, fails_with("map:merge(({1: 2}, {1: 3}), "
                                       "{'duplicates': 'reject'})",
                                       "FOJS0003"));
    /* Earlier maps merged into a later, larger one keep the entry order
       and the duplicates policy of F&O 4.0, 14.4.12 (D90). */
    SXML_TEST_EXPECT(state, evaluates_to("let $large := map:merge((2 to 10) ! {.: .}) return "
                                         "(map:keys(map:merge(({1: 'a', 5: 'x'}, $large))), "
                                         "map:merge(({1: 'a', 5: 'x'}, $large))?5, "
                                         "map:merge(({1: 'a', 5: 'x'}, $large), "
                                         "{'duplicates': 'use-last'})?5, "
                                         "map:merge(({1: 'a', 5: 'x'}, $large), "
                                         "{'duplicates': 'combine'})?5)",
                                         "1 5 2 3 4 6 7 8 9 10 x 5 x 5"));
    SXML_TEST_EXPECT(state, evaluates_to("let $m := map:merge(({5: 'p'}, {1: 'a', 5: 'x'}, "
                                         "map:merge((2 to 10) ! {.: .}), {11: 'k', 5: 'z'}), "
                                         "{'duplicates': 'combine'}) return (map:keys($m), "
                                         "$m?5)",
                                         "5 1 2 3 4 6 7 8 9 10 11 p x 5 z"));
    SXML_TEST_EXPECT(state, evaluates_to("let $m := map:merge(({5: 'p'}, {1: 'a', 5: 'x'}, "
                                         "map:merge((2 to 10) ! {.: .}), {11: 'k', 5: 'z'}), "
                                         "{'duplicates': 'use-last'}) return (map:keys($m), "
                                         "$m?5)",
                                         "5 1 2 3 4 6 7 8 9 10 11 z"));
    SXML_TEST_EXPECT(state, evaluates_to("map:merge(({5: 'p'}, map:merge((1 to 10) ! {.: .})), "
                                         "{'duplicates': fn($a, $b) { $a || $b }})?5",
                                         "p5"));
    SXML_TEST_EXPECT(state, fails_with("map:merge(({5: 'p'}, map:merge((1 to 10) ! {.: .})), "
                                       "{'duplicates': 'reject'})",
                                       "FOJS0003"));
    SXML_TEST_EXPECT(state, evaluates_to("map:keys(map:merge(({0: 0, 9: 0}, {1: 1, 2: 2, 3: "
                                         "3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 8, 9: 9})))",
                                         "0 9 1 2 3 4 5 6 7 8"));
    SXML_TEST_EXPECT(state, evaluates_to("map:keys(map:merge(({3: 0, 1: 0}, {2: 0}, "
                                         "{1: 0, 2: 0, 4: 0})))",
                                         "3 1 2 4"));
    SXML_TEST_EXPECT(state, evaluates_to("map:size(map:merge(for $i in 1 to 1000 return "
                                         "map:entry($i mod 100, $i)))",
                                         "100"));
}

/*
 * Repeated merges and puts stay O(n log n) overall (D90): map:merge starts
 * from its largest map, so a fold step that merges one entry into the
 * accumulator, on either side, costs O(log n). 10,000 steps take about a
 * second under ASan; the quadratic merge took 6 to 18 s for 1,000 (the QT4
 * seed of duplicates-maps-2) and would pass ctest's timeout here.
 */
static void test_map_merge_scales(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("map:size(fold-left(1 to 10000, map{}, "
                                         "fn($m, $i) { map:merge(($m, map{$i: $i})) }))",
                                         "10000"));
    SXML_TEST_EXPECT(state, evaluates_to("let $m := fold-left(1 to 10000, map{}, fn($m, $i) "
                                         "{ map:merge((map{$i: $i}, $m)) }), $keys := "
                                         "map:keys($m) return ($keys[1], $keys[10000], "
                                         "map:size($m))",
                                         "10000 1 10000"));
    SXML_TEST_EXPECT(state, evaluates_to("map:size(fold-left(1 to 2000, map{}, "
                                         "fn($m, $i) { map:merge(($m, map{$i mod 7: $i}), "
                                         "{'duplicates': 'use-last'}) }))",
                                         "7"));
    SXML_TEST_EXPECT(state, evaluates_to("let $keys := map:keys(fold-left(1 to 2000, map{}, "
                                         "fn($m, $i) { map:merge(($m, map{$i: $i, -$i: $i})) "
                                         "})) return ($keys[1], $keys[2], $keys[3999], "
                                         "$keys[4000])",
                                         "1 -1 2000 -2000"));
    /* duplicates-maps-2. */
    SXML_TEST_EXPECT(state, evaluates_to("let $counts := fold-left((1 to 1000, 1, 1), map{}, "
                                         "fn($sofar, $this) { let $seen := ($sofar($this), "
                                         "0)[1] return map:merge((map{$this: $seen + 1}, "
                                         "$sofar)) }) return map:keys($counts)[$counts(.) gt 1]",
                                         "1"));
    SXML_TEST_EXPECT(state, evaluates_to("map:size(map:merge(for $i in 1 to 10000 return "
                                         "map{$i: $i}))",
                                         "10000"));
    SXML_TEST_EXPECT(state, evaluates_to("map:size(fold-left(1 to 10000, map{}, "
                                         "fn($m, $i) { map:put($m, $i, $i) }))",
                                         "10000"));
}

/* ----------------------------------------------------- Arrays (slice) */

static void test_array_slice_lines(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("array:get([5,6,7], 1)", "5"));  /* array-get-001 */
    SXML_TEST_EXPECT(state, fails_with("array:get((), 1)", "XPTY0004"));  /* array-get-009 */
    SXML_TEST_EXPECT(state, evaluates_to("array:get([1,2,3], -3, function($x as node()){22}) "
                                         "instance of function(*)",
                                         "true"));  /* array-get-406 */
    SXML_TEST_EXPECT(state, evaluates_to("let $a := array:append([\"a\",\"b\"],()) return "
                                         "(array:size($a), $a?*)",
                                         "3 a b"));  /* array-append-105 */
    SXML_TEST_EXPECT(state, evaluates_to("array:flatten([1, 4, 6, 5, 3])",
                                         "1 4 6 5 3"));  /* array-flatten-001 */
    SXML_TEST_EXPECT(state, evaluates_to("array:flatten((1, 4, 6, 9))",
                                         "1 4 6 9"));  /* array-flatten-004 */
    SXML_TEST_EXPECT(state, evaluates_to("array:fold-left([1,2,3,4,5,6], 0, "
                                         "function($z,$a){$z + $a})",
                                         "21"));  /* array-fold-left-101 */
    SXML_TEST_EXPECT(state, evaluates_to("array:for-each([\"Monday\", \"Friday\", "
                                         "\"Tuesday\"], map{\"Monday\":true(), "
                                         "\"Tuesday\":false(), \"Wednesday\":true(), "
                                         "\"Thursday\":4, \"Friday\":true()})?*",
                                         "true true false"));  /* array-for-each-008 */
    SXML_TEST_EXPECT(state, evaluates_to("array:sort([\"the cat\", \"sat\", \"on the mat\"], "
                                         "(), function($x) {count(tokenize($x))})?*",
                                         "sat the cat on the mat"));  /* array-sort-012a */
    SXML_TEST_EXPECT(state, evaluates_to("array:size([[],[]])", "2"));  /* array-size-004 */
    SXML_TEST_EXPECT(state, evaluates_to("let $a := array:put([\"a\", \"b\", \"c\", \"d\"], 3, "
                                         "(\"x\", \"y\")) return (array:size($a), $a(3))",
                                         "4 x y"));  /* array-put-001 */
    SXML_TEST_EXPECT(state, fails_with("array:put([], 2, ())", "FOAY0001"));  /* array-put-006 */
    SXML_TEST_EXPECT(state, fails_with("array:subarray([1,2,3], 2, 3)",
                                       "FOAY0001"));  /* array-subarray-312 */
    SXML_TEST_EXPECT(state, evaluates_to("let $array:= ['a','b','c','d','e'] return "
                                         "3=>$array()",
                                         "c"));  /* ArrowPostfix-022 */
}

/* ------------------------------------------- Arrays (the whole of 15.2) */

static void test_array_functions(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("array:build(1 to 3, fn($x) { $x, $x })?*",
                                         "1 1 2 2 3 3"));
    SXML_TEST_EXPECT(state, evaluates_to("array:size(array:build(1 to 3)), "
                                         "array:empty([]), array:empty([()])",
                                         "3 true false"));
    SXML_TEST_EXPECT(state, evaluates_to("array:filter([1, 2, 3, 4], fn($m, $p) "
                                         "{ $p mod 2 = 0 })?*",
                                         "2 4"));
    SXML_TEST_EXPECT(state, evaluates_to("array:index-where([(), 1, (), 2], fn($m) "
                                         "{ count($m) > 0 })",
                                         "2 4"));
    /* array:index-of compares members by deep equality (F&O 4.0,
       15.2.13). */
    SXML_TEST_EXPECT(state, evaluates_to("array:index-of([10, 20, 30, 30, 20, 10], 20)",
                                         "2 5"));  /* array-index-of-001 */
    SXML_TEST_EXPECT(state, evaluates_to("array:index-of([(1, 2), 1, (1, 2), [1, 2]], (1, 2))",
                                         "1 3"));
    SXML_TEST_EXPECT(state, evaluates_to("array:index-of([], 1), array:index-of([()], ())",
                                         "1"));
    SXML_TEST_EXPECT(state, evaluates_to("array:index-of(['a', 'A'], 'a', "
                                         "'http://www.w3.org/2005/xpath-functions/collation/"
                                         "codepoint')",
                                         "1"));
    SXML_TEST_EXPECT(state, fails_with("array:index-of(['a'], 'a', 'http://example.com/c')",
                                       "FOCH0002"));
    SXML_TEST_EXPECT(state, evaluates_to("array:fold-right([1, 2, 3], (), fn($m, $a) "
                                         "{ $a, $m })",
                                         "3 2 1"));
    SXML_TEST_EXPECT(state, evaluates_to("array:for-each-pair([1, 2, 3], [10, 20], "
                                         "fn($a, $b, $p) { $a + $b + $p })?*",
                                         "12 24"));
    SXML_TEST_EXPECT(state, evaluates_to("array:head([1, 2, 3]), array:foot([1, 2, 3]), "
                                         "array:tail([1, 2, 3])?*, '|', array:trunk([1, 2, "
                                         "3])?*",
                                         "1 3 2 3 | 1 2"));
    SXML_TEST_EXPECT(state, fails_with("array:head([])", "FOAY0001"));
    SXML_TEST_EXPECT(state, fails_with("array:trunk([])", "FOAY0001"));
    SXML_TEST_EXPECT(state, fails_with("array:get([1], 2)", "FOAY0001"));
    SXML_TEST_EXPECT(state, evaluates_to("array:get([1], 2, 'none')", "none"));
    SXML_TEST_EXPECT(state, evaluates_to("array:insert-before([1, 2], 3, 9)?*, "
                                         "array:insert-before([1, 2], 1, 9)?*",
                                         "1 2 9 9 1 2"));
    SXML_TEST_EXPECT(state, fails_with("array:insert-before([1, 2], 4, 9)", "FOAY0001"));
    SXML_TEST_EXPECT(state, evaluates_to("array:items([1, (2, 3), [4]]) ! (if (. instance of "
                                         "array(*)) then 'a' else .)",
                                         "1 2 3 a"));
    SXML_TEST_EXPECT(state, evaluates_to("array:join(([1], [], [2, 3]))?*, "
                                         "array:size(array:join(()))",
                                         "1 2 3 0"));
    SXML_TEST_EXPECT(state, evaluates_to("array:members([1, (2, 3)]) ! (. instance of "
                                         "jnode())",
                                         "true true"));
    SXML_TEST_EXPECT(state, evaluates_to("array:of-members(array:members(['x', 'y']))?*",
                                         "x y"));
    SXML_TEST_EXPECT(state, evaluates_to("array:remove([1, 2, 3, 4], (1, 3, 3))?*, "
                                         "array:size(array:remove([], ()))",
                                         "2 4 0"));
    SXML_TEST_EXPECT(state, fails_with("array:remove([1], 2)", "FOAY0001"));
    SXML_TEST_EXPECT(state, evaluates_to("array:reverse([1, (2, 3), 4])?*", "4 2 3 1"));
    SXML_TEST_EXPECT(state, evaluates_to("array:slice(['a','b','c','d','e'], 2, 4)?*, '|', "
                                         "array:slice(['a','b','c','d','e'], -2)?*, '|', "
                                         "array:slice(['a','b','c','d','e'], (), (), 2)?*, '|',"
                                         " array:slice(['a','b','c','d','e'], 4, 3)?*, '|', "
                                         "array:slice(['a','b','c','d','e'], (), (), -2)?*",
                                         "b c d | d e | a c e | d c | e c a"));
    SXML_TEST_EXPECT(state, evaluates_to("array:size(array:slice([], 1, 2))", "0"));
    SXML_TEST_EXPECT(state, evaluates_to("array:sort([3, 1, 2])?*, array:sort(['b', 'a'], "
                                         "'http://www.w3.org/2005/xpath-functions/collation/"
                                         "codepoint')?*",
                                         "1 2 3 a b"));
    SXML_TEST_EXPECT(state, evaluates_to("array:sort([[2, 1], [1, 2], [1]])?* ! array:size(.)",
                                         "1 2 2"));
    SXML_TEST_EXPECT(state, evaluates_to("array:sort-by([(3, 1), (2, 9), ()], "
                                         "{'order': 'descending'})?* ",
                                         "3 1 2 9"));
    SXML_TEST_EXPECT(state, evaluates_to("array:sort-with([3, 1, 2], fn($a, $b) "
                                         "{ $a - $b })?*",
                                         "1 2 3"));
    SXML_TEST_EXPECT(state, evaluates_to("array:split([1, (2, 3)]) ! array:size(.), "
                                         "count(array:split([]))",
                                         "1 1 0"));
    SXML_TEST_EXPECT(state, evaluates_to("array:subarray([1, 2, 3], 2)?*, '|', "
                                         "array:subarray([1, 2, 3], 4)?*, '|', "
                                         "array:subarray([1, 2, 3], 1, 2)?*",
                                         "2 3 | | 1 2"));
    SXML_TEST_EXPECT(state, fails_with("array:subarray([1, 2, 3], 1, -1)", "FOAY0002"));
    SXML_TEST_EXPECT(state, fails_with("array:subarray([1, 2, 3], 0)", "FOAY0001"));
    SXML_TEST_EXPECT(state, evaluates_to("array:flatten(([1, [2, [3, []]]], 4, [[[5]]]))",
                                         "1 2 3 4 5"));
}

/* ------------------------------------------------ Recursion and depth */

/* A user function recursing through native steps (for-each calls the
   function, which calls for-each, ...) costs evaluator tasks, not C stack
   (D71, D75). */
static void test_nested_native_steps_use_no_c_stack(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("let $f := function($f, $n) { if ($n = 0) then 0 "
                                         "else fold-left(1, 0, function($a, $x) { $f($f, $n "
                                         "- 1) + 1 }) } return $f($f, 20000)",
                                         "20000"));
    /* Each level is a native call in progress: past the depth limit, a
       limit status, not a stack overflow. */
    evaluation_fixture *evaluation = &shared_fixture;
    SXML_TEST_REQUIRE(state, evaluation_fixture_start(evaluation, 0));
    evaluate_with(evaluation_fixture_environment(evaluation),
                  &(evaluation_case){
                      .expression_text =
                          "let $f := function($f, $n) { if ($n = 0) then 0 else "
                          "array:fold-left([1], 0, function($a, $x) { $f($f, $n - 1) + 1 }) "
                          "} return $f($f, 1000)",
                      .maximum_evaluation_depth = 100},
                  &evaluation->outcome);
    SXML_TEST_EXPECT(state, evaluation->outcome.status == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, fixture_released_everything(&evaluation->test_fixture));
    fixture_destroy(&evaluation->test_fixture);
}

/* ------------------------------------------------ Allocation failures */

/* Every allocation of compiling and evaluating fails in turn: each run
   fails cleanly with OUT_OF_MEMORY and releases everything, until one
   succeeds. */
static void test_every_allocation_failure_is_clean(sxml_test_state *state) {
    static const char expression_text[] =
        "let $m := map:build(1 to 6, fn($x) { $x mod 3 }), "
        "$a := array:sort-with(array:for-each([3, 1, 2], fn($x) { $x * 2 }), "
        "fn($x, $y) { $y - $x }) "
        "return (sort-by(map:keys($m), {'order': 'descending'}), $a?*, "
        "partial-apply(fold-left#3, {2: 0})(1 to 4, op('+')), "
        "map:merge(($m, {9: 9}))?9, filter(1 to 5, fn($x) { $x > 3 }))";
    uint64_t failing_allocation_number = 1;
    for (;;) {
        evaluation_fixture *evaluation = &shared_fixture;
        SXML_TEST_REQUIRE(state,
                          evaluation_fixture_start(evaluation, failing_allocation_number));
        evaluate_with(evaluation_fixture_environment(evaluation),
                      &(evaluation_case){.expression_text = expression_text},
                      &evaluation->outcome);
        const sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(evaluation->test_fixture.counting_allocator);
        SXML_TEST_EXPECT(state, fixture_released_everything(&evaluation->test_fixture));
        fixture_destroy(&evaluation->test_fixture);
        if (counts.failed_allocation_count == 0) {
            SXML_TEST_EXPECT(state, evaluation->outcome.status == SXML_STATUS_OK);
            SXML_TEST_EXPECT(state,
                             strcmp(evaluation->outcome.text, "2 1 0 6 4 2 10 9 4 5") == 0);
            if (strcmp(evaluation->outcome.text, "2 1 0 6 4 2 10 9 4 5") != 0) {
                (void)fprintf(stderr, "got \"%s\" (%d %s)\n", evaluation->outcome.text,
                              (int)evaluation->outcome.status, evaluation->outcome.code);
            }
            break;
        }
        if (evaluation->outcome.status != SXML_STATUS_OUT_OF_MEMORY) {
            (void)fprintf(stderr, "allocation %llu: status %d %s\n",
                          (unsigned long long)failing_allocation_number,
                          (int)evaluation->outcome.status, evaluation->outcome.code);
        }
        SXML_TEST_EXPECT(state, evaluation->outcome.status == SXML_STATUS_OUT_OF_MEMORY);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 50000);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 20);
}

/* ---------------------------------------- JNodes, selectors (packet 2.6) */

/* F&O 4.0, sections 16.1.2 and 16.1.3; XPath 4.0, sections 4.7.3 and
   4.7.5.4 (the selector interpretation of non-navigational steps). */
static void test_json_node_accessors_and_selectors(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("jtree({\"a\":1, \"b\":2})/b/preceding-sibling::* "
                                         "! jkey(), "
                                         "jtree({\"a\":1, \"b\":2})/b/preceding-sibling::* "
                                         "! jvalue(), count(jkey(jtree([1]))), jkey(())",
                                         "a 1 0"));
    SXML_TEST_EXPECT(state, evaluates_to("jtree([ \"a\", \"b\", \"c\", \"d\" ])/* =!> jkey()",
                                         "1 2 3 4"));
    SXML_TEST_EXPECT(state, evaluates_to("let $map := {\"x\":1, \"y\":2, \"z\":3} "
                                         "return $map/(\"z\", \"x\") =!> jvalue()",
                                         "1 3"));  /* JAxes-202 */
    SXML_TEST_EXPECT(state, evaluates_to("[4, 5, 6]/2 =!> jvalue(), "
                                         "count({ '1': 'a' }/1), count({ 1: 'a' }/'1')",
                                         "5 0 0"));  /* JAxes-288 */
    SXML_TEST_EXPECT(state, evaluates_to("let $m := {'A':(), 'B':1, 'C':(1, 2)} "
                                         "return $m/C/1 =!> jvalue()",
                                         "1"));
    SXML_TEST_EXPECT(state, evaluates_to("{ #Q{u}a: 1, #Q{v}a: 2, #Q{u}b: 3 }/*:a "
                                         "=!> jvalue()",
                                         "1 2"));  /* JAxes-294 */
    SXML_TEST_EXPECT(state, fails_with("{'a': 1}/count(.)", "XPDY0002"));
    SXML_TEST_EXPECT(state, fails_with("[1]/jvalue()", "XPDY0002"));
    SXML_TEST_EXPECT(state, evaluates_to("({ 'a': { 'b': 1 } }/a)?b, "
                                         "({ 'a': [ 1, 2 ] }/a)?*, "
                                         "({ 'a': [ 1 ] }/a, { 'b': [ 2 ] }/b)?1",
                                         "1 1 2 1 2"));  /* PathExpr-J-490 */
    SXML_TEST_EXPECT(state, fails_with("({ 'a': (1, 2) }/a)?*", "XPTY0004"));
}

/* XPath 4.0, section 4.4.1: a predicate of several numbers. */
static void test_numeric_predicates(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 10)[(5, 2)], (1 to 10)[3 to 5], "
                                         "(1 to 3)[-5 to 1], (1 to 3)[2.0, 7]",
                                         "2 5 3 4 5 1 2"));
    SXML_TEST_EXPECT(state, evaluates_to("(['a', 'b'], ['b', 'c'])[ *[1 to 2] = 'b'] "
                                         "=> count()",
                                         "2"));  /* PathExpr-J-007 */
    SXML_TEST_EXPECT(state, fails_with("(1 to 3)[(1, 'a')]", "FORG0006"));
}

/* XPath 4.0, sections 3.2.9 and 3.2.10: maps and arrays as functions. */
static void test_maps_and_arrays_match_function_types(sxml_test_state *state) {
    SXML_TEST_EXPECT(state,
                     evaluates_to("{3: 5} instance of function(xs:anyAtomicType) as xs:integer?, "
                                  "{3: 5} instance of function(xs:anyAtomicType) as xs:integer, "
                                  "{3: 'a'} instance of function(xs:integer) as xs:integer?, "
                                  "[1, 2] instance of function(xs:integer) as xs:integer, "
                                  "[1, 'a'] instance of function(xs:integer) as xs:integer, "
                                  "[1] instance of function(xs:string) as item()*",
                                  "true false false true false false"));
}

/* F&O 4.0, fn:fold-left and fn:fold-right: an accumulator the function
   returns unchanged from then on ends the fold early (QT4 fold-left-022,
   fold-right-023); a condition that can change does not. */
static void test_folds_exit_at_a_fixpoint(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 100000000, 0, function($count, $item)"
                                         "{if ($count gt 100) then $count else $count+1})",
                                         "101"));
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 100000000, 0, function($sum, $item)"
                                         "{if ($sum gt 1000) then $sum else $sum + $item})",
                                         "1035"));
    SXML_TEST_EXPECT(state, evaluates_to("fold-right(1 to 100000000, 0, function($item, $count)"
                                         "{if ($count gt 100) then $count else $count+1})",
                                         "101"));
    SXML_TEST_EXPECT(state, evaluates_to("let $limit := 3 return "
                                         "fold-left(1 to 100000000, 0, function($count, $item)"
                                         "{if ($count >= $limit) then $count else $count + 1})",
                                         "3"));
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 6, 0, function($sum, $item)"
                                         "{if ($sum lt 5) then $sum + $item else $sum})",
                                         "6"));
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 6, 0, function($sum, $item)"
                                         "{if ($item lt 3) then $sum else $sum + $item})",
                                         "18"));
}

/* F&O 4.0, fn:drop-while: the rest of an integer range stays a range
   (QT4 drop-while-014). */
static void test_drop_while_keeps_a_range(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("drop-while(1 to 1000000000, function($x) "
                                         "{ $x lt 3 })[1], "
                                         "count(drop-while(1 to 10, function($x) { $x lt 4 })), "
                                         "drop-while(-3 to -1, function($x) { $x lt -2 })",
                                         "3 7 -2 -1"));
}

/* ------------------------------------------------------------------ Main */

static const sxml_test_case higher_order_test_cases[] = {
    {.name = "higher_order_slice_lines", .function = test_higher_order_slice_lines},
    {.name = "fold_right_over_a_million_items",
     .function = test_fold_right_over_a_million_items},
    {.name = "higher_order_functions", .function = test_higher_order_functions},
    {.name = "transitive_closure", .function = test_transitive_closure},
    {.name = "map_slice_lines", .function = test_map_slice_lines},
    {.name = "map_functions", .function = test_map_functions},
    {.name = "map_merge_scales", .function = test_map_merge_scales},
    {.name = "array_slice_lines", .function = test_array_slice_lines},
    {.name = "array_functions", .function = test_array_functions},
    {.name = "nested_native_steps_use_no_c_stack",
     .function = test_nested_native_steps_use_no_c_stack},
    {.name = "every_allocation_failure_is_clean",
     .function = test_every_allocation_failure_is_clean},
    {.name = "json_node_accessors_and_selectors",
     .function = test_json_node_accessors_and_selectors},
    {.name = "numeric_predicates", .function = test_numeric_predicates},
    {.name = "maps_and_arrays_match_function_types",
     .function = test_maps_and_arrays_match_function_types},
    {.name = "folds_exit_at_a_fixpoint", .function = test_folds_exit_at_a_fixpoint},
    {.name = "drop_while_keeps_a_range", .function = test_drop_while_keeps_a_range},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "function_library_higher_order",
            .cases = higher_order_test_cases,
            .case_count = SXML_TEST_COUNT(higher_order_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
