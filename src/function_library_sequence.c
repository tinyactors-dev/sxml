/*
 * The sequence part of the function library (packet 1.20, D78): F&O 4.0,
 * section 7 "Functions on Boolean values" (fn:true, fn:false, fn:boolean,
 * fn:not), section 2 "Accessors" (fn:data) and section 14 "Functions on
 * sequences" (fn:count, fn:empty, fn:exists, fn:head, fn:tail,
 * fn:reverse, fn:subsequence, fn:zero-or-one, fn:one-or-more,
 * fn:exactly-one, fn:index-of, fn:insert-before, fn:remove,
 * fn:distinct-values, fn:deep-equal, fn:sort).
 *
 * Golden file: src/memory.c (layout and comments); rows as in
 * src/function_library_internal.h. fn:sort is a resumable body (D75): it
 * asks the evaluator to call $key once per item, then sorts without
 * calling anything; fn:deep-equal lives in
 * src/function_library_deep_equal.c.
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>

/* --------------------------------------------------------------- Booleans */

static sxml_status call_true(const sxml_function_call *call, sxml_sequence *result) {
    (void)call;
    *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = true});
    return SXML_STATUS_OK;
}

static sxml_status call_false(const sxml_function_call *call, sxml_sequence *result) {
    (void)call;
    *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = false});
    return SXML_STATUS_OK;
}

/* fn:boolean($input) (F&O 4.0, section 7.3.1): the effective boolean
   value (FORG0006 when there is none). */
static sxml_status call_boolean(const sxml_function_call *call, sxml_sequence *result) {
    sxml_effective_boolean_value value;
    sxml_status status = sxml_evaluator_compute_effective_boolean_value(
        sxml_function_library_get_argument(call, 0), call->error_record, &value);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = value.is_true});
    }
    return status;
}

/* fn:not($input) (F&O 4.0, section 7.3.2). */
static sxml_status call_not(const sxml_function_call *call, sxml_sequence *result) {
    sxml_effective_boolean_value value;
    sxml_status status = sxml_evaluator_compute_effective_boolean_value(
        sxml_function_library_get_argument(call, 0), call->error_record, &value);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = !value.is_true});
    }
    return status;
}

/* ---------------------------------------------------------------- fn:data */

static const sxml_static_error no_dynamic_context_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The function needs the dynamic context of an evaluation (XPath 4.0, "
        "section 2.2.2)"),
};

/* fn:data($input := .) (F&O 4.0, section 2.1.5): atomization. */
static sxml_status call_data(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    return sxml_evaluator_atomize(
        call->dynamic_context,
        &(sxml_atomization_request){.value = sxml_function_library_get_argument(call, 0),
                                    .error_record = call->error_record},
        result);
}

/* --------------------------------------------------------------- Counting */

static sxml_status call_count(const sxml_function_call *call, sxml_sequence *result) {
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(sxml_function_library_get_argument(call, 0),
                                             call->error_record, &item_count);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)item_count));
    }
    return status;
}

/* fn:empty and fn:exists read at most one item (F&O 4.0, sections 14.2.2
   and 14.2.3). */
typedef struct emptiness_question {
    bool is_empty_wanted;
} emptiness_question;

static sxml_status test_emptiness(const sxml_function_call *call,
                                  emptiness_question question, sxml_sequence *result) {
    const bool is_empty_wanted = question.is_empty_wanted;
    sxml_emptiness emptiness;
    sxml_status status = sxml_sequence_test_emptiness(
        sxml_function_library_get_argument(call, 0), call->error_record, &emptiness);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = emptiness.is_empty == is_empty_wanted});
    }
    return status;
}

static sxml_status call_empty(const sxml_function_call *call, sxml_sequence *result) {
    return test_emptiness(call, (emptiness_question){.is_empty_wanted = true}, result);
}

static sxml_status call_exists(const sxml_function_call *call, sxml_sequence *result) {
    return test_emptiness(call, (emptiness_question){.is_empty_wanted = false}, result);
}

/* ------------------------------------------------------------ Selecting */

/* The items from first_index (from 0) up to, not including, end_index,
   or to the end; a range stays a range. */
typedef struct item_span {
    sxml_sequence input;
    uint64_t first_index;
    bool has_end;
    uint64_t end_index;
} item_span;

static sxml_status make_span(const sxml_function_call *call, item_span span,
                             sxml_sequence *result) {
    const sxml_sequence input = span.input;
    if (sxml_sequence_get_representation(input) == SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE) {
        uint64_t item_count = 0;
        sxml_item first = {.representation = 0};
        sxml_status status = sxml_sequence_count(input, call->error_record, &item_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                input, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
                &first);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const uint64_t end_index =
            span.has_end && span.end_index < item_count ? span.end_index : item_count;
        if (span.first_index >= end_index) {
            *result = sxml_sequence_empty();
            return SXML_STATUS_OK;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(first, &datum);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* Members are first + index, computed in uint64_t (D62). */
        const uint64_t start = (uint64_t)datum.datum_details.integer;
        return sxml_sequence_make_integer_range(
            call->item_heap,
            (sxml_integer_range){
                .first_integer = (int64_t)(start + span.first_index),
                .last_integer = (int64_t)(start + end_index - 1)},
            result);
    }
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    for (uint64_t item_index = span.first_index;
         status == SXML_STATUS_OK && (!span.has_end || item_index < span.end_index);
         item_index += 1) {
        sxml_item item = {.representation = 0};
        status = sxml_sequence_get_item(
            input,
            (sxml_item_request){.item_index = item_index, .error_record = call->error_record},
            &item);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            status = SXML_STATUS_OK;
            break;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, item);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* fn:head($input) (F&O 4.0, section 14.1.6). */
static sxml_status call_head(const sxml_function_call *call, sxml_sequence *result) {
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        sxml_function_library_get_argument(call, 0),
        (sxml_item_request){.item_index = 0, .error_record = call->error_record}, &item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        sxml_item_retain(item);
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:tail($input) (F&O 4.0, section 14.1.15). */
static sxml_status call_tail(const sxml_function_call *call, sxml_sequence *result) {
    return make_span(call,
                     (item_span){.input = sxml_function_library_get_argument(call, 0),
                                 .first_index = 1,
                                 .has_end = false,
                                 .end_index = 0},
                     result);
}

/* fn:subsequence($input, $start, $length := ()) (F&O 4.0, section
   14.1.14): items at positions p with round($start) <= p < round($start)
   + round($length). */
static sxml_status call_subsequence(const sxml_function_call *call,
                                    sxml_sequence *result) {
    sxml_position_span positions;
    sxml_status status = sxml_function_library_read_position_span(
        call, (sxml_position_arguments){.start_argument = 1, .length_argument = 2}, &positions);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (positions.is_empty) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    return make_span(call,
                     (item_span){.input = sxml_function_library_get_argument(call, 0),
                                 .first_index = positions.first_index,
                                 .has_end = positions.has_end,
                                 .end_index = positions.end_index},
                     result);
}

/* fn:reverse($input) (F&O 4.0, section 14.1.12). */
static sxml_status call_reverse(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(input, call->error_record, &item_count);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    for (uint64_t remaining = item_count; status == SXML_STATUS_OK && remaining > 0;
         remaining -= 1) {
        sxml_item item = {.representation = 0};
        status = sxml_sequence_get_item(
            input,
            (sxml_item_request){.item_index = remaining - 1, .error_record = call->error_record},
            &item);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, item);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* --------------------------------------------------- Cardinality checks */

static const sxml_static_error zero_or_one_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0003"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:zero-or-one was called with more than one item (F&O 4.0, section 14.3.1)"),
};

static const sxml_static_error one_or_more_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:one-or-more was called with the empty sequence (F&O 4.0, section 14.3.2)"),
};

static const sxml_static_error exactly_one_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0005"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:exactly-one was not called with one item (F&O 4.0, section 14.3.3)"),
};

/* The cardinalities fn:zero-or-one, fn:one-or-more and fn:exactly-one
   accept. */
typedef struct cardinality_check {
    bool allows_none;
    bool allows_several;
    const sxml_static_error *error;
} cardinality_check;

static sxml_status check_cardinality(const sxml_function_call *call,
                                     cardinality_check check, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        input, (sxml_item_request){.item_index = 0, .error_record = call->error_record}, &item);
    const bool has_one = status == SXML_STATUS_OK;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            input, (sxml_item_request){.item_index = 1, .error_record = call->error_record},
            &item);
    }
    const bool has_several = status == SXML_STATUS_OK;
    if (status != SXML_STATUS_OK && status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
        return status;
    }
    if ((!has_one && !check.allows_none) || (has_several && !check.allows_several)) {
        return sxml_function_library_report(call, check.error);
    }
    sxml_sequence_retain(input);
    *result = input;
    return SXML_STATUS_OK;
}

static sxml_status call_zero_or_one(const sxml_function_call *call, sxml_sequence *result) {
    return check_cardinality(call,
                             (cardinality_check){.allows_none = true,
                                                 .allows_several = false,
                                                 .error = &zero_or_one_error},
                             result);
}

static sxml_status call_one_or_more(const sxml_function_call *call, sxml_sequence *result) {
    return check_cardinality(call,
                             (cardinality_check){.allows_none = false,
                                                 .allows_several = true,
                                                 .error = &one_or_more_error},
                             result);
}

static sxml_status call_exactly_one(const sxml_function_call *call, sxml_sequence *result) {
    return check_cardinality(call,
                             (cardinality_check){.allows_none = false,
                                                 .allows_several = false,
                                                 .error = &exactly_one_error},
                             result);
}

/* -------------------------------------------------- Positions and edits */

/* fn:index-of($input, $target, $collation := default) (F&O 4.0, section
   14.1.7): the positions of the items equal to $target under fn:compare
   (items that do not compare are not equal; NaN equals NaN in 4.0, QT4
   K-SeqIndexOfFunc-9a). */
static sxml_status call_index_of(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item target;
    sxml_status status = sxml_function_library_check_collation(call, 2);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &target);
    }
    int16_t implicit_timezone = 0;
    if (status == SXML_STATUS_OK && call->dynamic_context != NULL) {
        status = sxml_dynamic_context_read_implicit_timezone(call->dynamic_context,
                                                             &implicit_timezone);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    int64_t position = 0;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        position += 1;
        sxml_ordering ordering = SXML_ORDERING_LESS;
        const sxml_status compared = sxml_atomic_item_compare_in_heap(
            call->item_heap,
            &(sxml_atomic_comparison){.first = step.item,
                                      .second = target.item,
                                      .implicit_timezone_offset_minutes = implicit_timezone,
                                      .error_record = NULL,
                                      .name_pool = sxml_item_heap_get_name_pool(call->item_heap)},
            &ordering);
        if (compared != SXML_STATUS_OK && compared != SXML_STATUS_TYPE_ERROR) {
            status = compared;
            break;
        }
        if (compared == SXML_STATUS_OK && ordering == SXML_ORDERING_EQUAL) {
            status = sxml_sequence_builder_append_item(builder,
                                                       sxml_atomic_item_from_integer(position));
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* The integer of an xs:integer argument, clamped to int64_t. */
static int64_t read_integer(const sxml_function_call *call, size_t argument_index) {
    sxml_optional_item argument;
    sxml_atomic_datum datum;
    if (sxml_function_library_read_optional_item(call, argument_index, &argument) !=
            SXML_STATUS_OK ||
        !argument.has_item || sxml_atomic_item_get_datum(argument.item, &datum) != SXML_STATUS_OK) {
        return 0;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        return datum.datum_details.integer;
    }
    const bool is_negative = datum.datum_details.decimal_text.byte_count > 0 &&
                             datum.datum_details.decimal_text.bytes[0] == '-';
    return is_negative ? INT64_MIN : INT64_MAX;
}

/* fn:insert-before($input, $position, $insert) (F&O 4.0, section
   14.1.8). */
static sxml_status call_insert_before(const sxml_function_call *call,
                                      sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    const int64_t position = read_integer(call, 1);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(input, call->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t split = position < 1 ? 0
                           : (uint64_t)position - 1 > item_count ? item_count
                                                                 : (uint64_t)position - 1;
    sxml_sequence before = sxml_sequence_empty();
    sxml_sequence after = sxml_sequence_empty();
    status = make_span(
        call,
        (item_span){.input = input, .first_index = 0, .has_end = true, .end_index = split},
        &before);
    if (status == SXML_STATUS_OK) {
        status = make_span(
            call,
            (item_span){.input = input, .first_index = split, .has_end = false, .end_index = 0},
            &after);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_sequence parts[3] = {before, sxml_function_library_get_argument(call, 2),
                                        after};
        status = sxml_sequence_concatenate(
            call->item_heap, (sxml_sequence_view){.sequences = parts, .sequence_count = 3},
            result);
    }
    sxml_sequence_release(before);
    sxml_sequence_release(after);
    return status;
}

/* fn:remove($input, $positions) (F&O 4.0, section 14.1.11): without the
   items at the given positions. */
static sxml_status call_remove(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    const sxml_sequence positions = sxml_function_library_get_argument(call, 1);
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(input);
    int64_t position = 0;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        position += 1;
        bool is_removed = false;
        sxml_sequence_iterator position_iterator = sxml_sequence_iterator_start(positions);
        for (;;) {
            sxml_iteration_step position_step;
            status = sxml_sequence_iterator_next(&position_iterator, call->error_record,
                                                 &position_step);
            if (status != SXML_STATUS_OK || !position_step.has_item) {
                break;
            }
            sxml_atomic_datum datum;
            if (sxml_atomic_item_get_datum(position_step.item, &datum) == SXML_STATUS_OK &&
                datum.datum_kind == SXML_DATUM_KIND_INTEGER &&
                datum.datum_details.integer == position) {
                is_removed = true;
                break;
            }
        }
        if (status == SXML_STATUS_OK && !is_removed) {
            status = sxml_sequence_builder_append_item(builder, step.item);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* ----------------------------------------------------- distinct-values */

/* An open-addressing set of item positions, by fn:atomic-equal. */
typedef struct distinct_set {
    sxml_item_heap *item_heap;
    /* Kept items, borrowed from the input; slots hold position + 1. */
    sxml_item *items;
    size_t item_count;
    size_t *slots;
    size_t slot_count;
} distinct_set;

static void release_distinct_set(distinct_set *set) {
    sxml_item_heap_release_bytes(
        set->item_heap, (sxml_heap_bytes){.memory = set->items,
                                          .byte_count = set->slot_count * sizeof(sxml_item)});
    sxml_item_heap_release_bytes(
        set->item_heap, (sxml_heap_bytes){.memory = set->slots,
                                          .byte_count = set->slot_count * sizeof(size_t)});
    set->items = NULL;
    set->slots = NULL;
}

/* Makes room for slot_count slots (a power of two) and items. */
static sxml_status allocate_distinct_set(distinct_set *set, size_t slot_count) {
    size_t item_bytes = 0;
    size_t slot_bytes = 0;
    sxml_status status = sxml_size_multiply(slot_count, sizeof(sxml_item), &item_bytes);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(slot_count, sizeof(size_t), &slot_bytes);
    }
    void *items = NULL;
    void *slots = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            set->item_heap,
            (sxml_allocation_request){.byte_count = item_bytes, .alignment = alignof(sxml_item)},
            &items);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            set->item_heap,
            (sxml_allocation_request){.byte_count = slot_bytes, .alignment = alignof(size_t)},
            &slots);
        if (status != SXML_STATUS_OK) {
            sxml_item_heap_release_bytes(
                set->item_heap, (sxml_heap_bytes){.memory = items, .byte_count = item_bytes});
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(slots, 0, slot_bytes);
    if (set->item_count > 0) {
        memcpy(items, set->items, set->item_count * sizeof(sxml_item));
    }
    const size_t old_count = set->item_count;
    release_distinct_set(set);
    set->items = items;
    set->slots = slots;
    set->slot_count = slot_count;
    for (size_t item_index = 0; item_index < old_count; item_index += 1) {
        size_t slot = (size_t)sxml_atomic_item_hash(set->items[item_index]) & (slot_count - 1);
        while (set->slots[slot] != 0) {
            slot = (slot + 1) & (slot_count - 1);
        }
        set->slots[slot] = item_index + 1;
    }
    return SXML_STATUS_OK;
}

/* Whether add_distinct kept the item. */
typedef struct distinct_addition {
    bool is_new;
} distinct_addition;

/* Adds item unless an atomic-equal one is in set; addition tells. */
static sxml_status add_distinct(distinct_set *set, sxml_item item,
                                distinct_addition *addition) {
    if ((set->item_count + 1) * 2 > set->slot_count) {
        sxml_status status = allocate_distinct_set(
            set, set->slot_count == 0 ? 16 : set->slot_count * 2);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    size_t slot = (size_t)sxml_atomic_item_hash(item) & (set->slot_count - 1);
    while (set->slots[slot] != 0) {
        if (sxml_atomic_item_is_atomic_equal(set->items[set->slots[slot] - 1], item)) {
            addition->is_new = false;
            return SXML_STATUS_OK;
        }
        slot = (slot + 1) & (set->slot_count - 1);
    }
    set->items[set->item_count] = item;
    set->item_count += 1;
    set->slots[slot] = set->item_count;
    addition->is_new = true;
    return SXML_STATUS_OK;
}

/* fn:distinct-values($values, $collation := default) (F&O 4.0, section
   14.2.5): the first of each group of atomic-equal items, in order (NaN
   equals NaN; strings, xs:anyURI and xs:untypedAtomic compare by
   codepoints). */
static sxml_status call_distinct_values(const sxml_function_call *call,
                                        sxml_sequence *result) {
    sxml_status status = sxml_function_library_check_collation(call, 1);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    distinct_set set = {.item_heap = call->item_heap,
                        .items = NULL,
                        .item_count = 0,
                        .slots = NULL,
                        .slot_count = 0};
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    for (;;) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        distinct_addition addition = {.is_new = false};
        status = add_distinct(&set, step.item, &addition);
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            call->item_heap, (sxml_item_view){.items = set.items, .item_count = set.item_count},
            result);
    }
    release_distinct_set(&set);
    return status;
}

/* ---------------------------------------------------------------- fn:sort */

/*
 * fn:sort($input, $collation := default, $key := fn:data#1) (F&O 4.0,
 * section 14.2.9) as a resumable body (D75): step 0 reads the input and,
 * with a $key function, asks for its first call; each later step keeps
 * the key it received and asks for the next; the last sorts the
 * positions by their sort key values, stably, and finishes. Without $key
 * the keys are atomized items and no call is made.
 */
typedef struct sort_state {
    sxml_item_heap *item_heap;
    size_t item_count;
    /* Elements each array was allocated with: item_count, at least 1. */
    size_t allocated_count;
    /* Borrowed from the input argument, which outlives the steps. */
    sxml_item *items;
    /* Owned sort key values, one per item. */
    sxml_sequence *keys;
    size_t *positions;
    size_t next_key_index;
    /* Owned while a call of $key runs. */
    sxml_sequence call_argument;
} sort_state;

static void release_array(const sort_state *sort, void *memory, size_t element_size) {
    sxml_item_heap_release_bytes(
        sort->item_heap,
        (sxml_heap_bytes){.memory = memory, .byte_count = sort->allocated_count * element_size});
}

static void release_sort_state(void *state) {
    sort_state *sort = state;
    for (size_t key_index = 0; sort->keys != NULL && key_index < sort->item_count;
         key_index += 1) {
        sxml_sequence_release(sort->keys[key_index]);
    }
    sxml_sequence_release(sort->call_argument);
    if (sort->item_heap != NULL) {
        release_array(sort, sort->items, sizeof(sxml_item));
        release_array(sort, sort->keys, sizeof(sxml_sequence));
        release_array(sort, sort->positions, sizeof(size_t));
    }
    *sort = (sort_state){.item_heap = NULL};
}

/* Zeroed heap memory for sort->allocated_count elements. */
static sxml_status allocate_array(const sort_state *sort, size_t element_size,
                                  void **memory) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(sort->allocated_count, element_size, &byte_count);
    if (status == SXML_STATUS_OK) {
        /* Every array holds pointers or items: 16 is enough alignment. */
        status = sxml_item_heap_allocate_bytes(
            sort->item_heap,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(sxml_item)},
            memory);
    }
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, byte_count);
    }
    return status;
}

/* Reads the input into the state's arrays (step 0). On failure what was
   allocated is left to release_sort_state. */
static sxml_status start_sort(const sxml_function_call *call, sort_state *sort) {
    sxml_status status = sxml_function_library_check_collation(call, 1);
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(input, call->error_record, &item_count);
    }
    if (status == SXML_STATUS_OK && item_count > SIZE_MAX / sizeof(sxml_sequence)) {
        status = SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sort->item_heap = call->item_heap;
    sort->allocated_count = item_count == 0 ? 1 : (size_t)item_count;
    void *memory = NULL;
    status = allocate_array(sort, sizeof(sxml_item), &memory);
    if (status == SXML_STATUS_OK) {
        sort->items = memory;
        status = allocate_array(sort, sizeof(sxml_sequence), &memory);
    }
    if (status == SXML_STATUS_OK) {
        sort->keys = memory;
        status = allocate_array(sort, sizeof(size_t), &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sort->positions = memory;
    sort->item_count = (size_t)item_count;
    for (size_t item_index = 0; status == SXML_STATUS_OK && item_index < sort->item_count;
         item_index += 1) {
        status = sxml_sequence_get_item(
            input,
            (sxml_item_request){.item_index = item_index, .error_record = call->error_record},
            &sort->items[item_index]);
        sort->positions[item_index] = item_index;
    }
    return status;
}

/* Orders two sort key values (F&O 4.0, fn:sort): item by item with
   fn:compare semantics, NaN first, a shorter key that is a prefix first;
   items that do not compare are XPTY0004. */
typedef struct key_comparison {
    const sxml_function_call *call;
    const sort_state *sort;
    int16_t implicit_timezone_offset_minutes;
} key_comparison;

static sxml_status compare_keys(void *state, sxml_index_pair pair, sxml_ordering *ordering) {
    const key_comparison *comparison = state;
    const sxml_sequence first = comparison->sort->keys[pair.first_index];
    const sxml_sequence second = comparison->sort->keys[pair.second_index];
    for (uint64_t item_index = 0;; item_index += 1) {
        sxml_item first_item = {.representation = 0};
        sxml_item second_item = {.representation = 0};
        sxml_status first_status = sxml_sequence_get_item(
            first,
            (sxml_item_request){.item_index = item_index,
                                .error_record = comparison->call->error_record},
            &first_item);
        sxml_status second_status = sxml_sequence_get_item(
            second,
            (sxml_item_request){.item_index = item_index,
                                .error_record = comparison->call->error_record},
            &second_item);
        if (first_status != SXML_STATUS_OK && first_status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
            return first_status;
        }
        if (second_status != SXML_STATUS_OK &&
            second_status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
            return second_status;
        }
        const bool first_ended = first_status == SXML_STATUS_INDEX_OUT_OF_RANGE;
        const bool second_ended = second_status == SXML_STATUS_INDEX_OUT_OF_RANGE;
        if (first_ended || second_ended) {
            *ordering = first_ended && second_ended
                            ? SXML_ORDERING_EQUAL
                            : (first_ended ? SXML_ORDERING_LESS : SXML_ORDERING_GREATER);
            return SXML_STATUS_OK;
        }
        sxml_ordering item_ordering = SXML_ORDERING_EQUAL;
        sxml_status status = sxml_atomic_item_compare_in_heap(
            comparison->call->item_heap,
            &(sxml_atomic_comparison){
                .first = first_item,
                .second = second_item,
                .implicit_timezone_offset_minutes =
                    comparison->implicit_timezone_offset_minutes,
                .error_record = comparison->call->error_record,
                .name_pool = sxml_item_heap_get_name_pool(comparison->call->item_heap)},
            &item_ordering);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (item_ordering != SXML_ORDERING_EQUAL) {
            *ordering = item_ordering;
            return SXML_STATUS_OK;
        }
    }
}

/* Sorts the positions and makes the result (the last step). */
static sxml_status finish_sort(const sxml_function_call *call, sort_state *sort,
                               sxml_native_step *step) {
    int16_t implicit_timezone = 0;
    sxml_status status = sxml_dynamic_context_read_implicit_timezone(call->dynamic_context,
                                                                     &implicit_timezone);
    key_comparison comparison = {.call = call,
                                 .sort = sort,
                                 .implicit_timezone_offset_minutes = implicit_timezone};
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_sort_indices(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_index_sort_request){.indices = sort->positions,
                                       .index_count = sort->item_count,
                                       .compare = compare_keys,
                                       .compare_state = &comparison});
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    for (size_t position = 0; status == SXML_STATUS_OK && position < sort->item_count;
         position += 1) {
        status = sxml_sequence_builder_append_item(builder,
                                                   sort->items[sort->positions[position]]);
    }
    sxml_sequence sorted = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, &sorted);
    }
    sxml_sequence_builder_destroy(builder);
    if (status == SXML_STATUS_OK) {
        *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                                   .result = sorted,
                                   .function_item = {.representation = 0},
                                   .arguments = {.sequences = NULL, .sequence_count = 0}};
    }
    return status;
}

static sxml_status step_sort(const sxml_native_step_request *request,
                             sxml_native_step *step) {
    sort_state *sort = request->state;
    const sxml_function_call *call = request->call;
    if (call->dynamic_context == NULL) {
        sxml_sequence_release(request->call_result);
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    sxml_optional_item key_function = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 2, &key_function);
    if (request->step_number == 0) {
        if (status == SXML_STATUS_OK) {
            status = start_sort(call, sort);
        }
        for (size_t key_index = 0;
             status == SXML_STATUS_OK && !key_function.has_item && key_index < sort->item_count;
             key_index += 1) {
            status = sxml_evaluator_atomize(
                call->dynamic_context,
                &(sxml_atomization_request){
                    .value = sxml_sequence_from_item(sort->items[key_index]),
                    .error_record = call->error_record},
                &sort->keys[key_index]);
        }
    } else {
        sort->keys[sort->next_key_index] = request->call_result;
        sort->next_key_index += 1;
        sxml_sequence_release(sort->call_argument);
        sort->call_argument = sxml_sequence_empty();
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!key_function.has_item || sort->next_key_index == sort->item_count) {
        return finish_sort(call, sort, step);
    }
    sxml_item item = sort->items[sort->next_key_index];
    sxml_item_retain(item);
    sort->call_argument = sxml_sequence_from_item(item);
    *step = (sxml_native_step){
        .step_kind = SXML_NATIVE_STEP_KIND_CALL,
        .result = sxml_sequence_empty(),
        .function_item = key_function.item,
        .arguments = {.sequences = &sort->call_argument, .sequence_count = 1}};
    return SXML_STATUS_OK;
}

static const sxml_resumable_native_body sort_body = {
    .step = step_sort,
    .release_state = release_sort_state,
    .state_byte_count = sizeof(sort_state),
    .state_alignment = alignof(sort_state),
};

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter input_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*")};
static const sxml_function_parameter data_parameters[] = {
    SXML_CONTEXT_PARAMETER("input", "item()*")};
static const sxml_function_parameter index_of_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "xs:anyAtomicType*"),
    SXML_REQUIRED_PARAMETER("target", "xs:anyAtomicType"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?")};
static const sxml_function_parameter insert_before_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_REQUIRED_PARAMETER("position", "xs:integer"),
    SXML_REQUIRED_PARAMETER("insert", "item()*")};
static const sxml_function_parameter remove_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_REQUIRED_PARAMETER("positions", "xs:integer*")};
static const sxml_function_parameter subsequence_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_REQUIRED_PARAMETER("start", "xs:numeric"),
    SXML_OPTIONAL_PARAMETER("length", "xs:numeric?")};
static const sxml_function_parameter distinct_values_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:anyAtomicType*"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?")};
static const sxml_function_parameter deep_equal_parameters[] = {
    SXML_REQUIRED_PARAMETER("input1", "item()*"),
    SXML_REQUIRED_PARAMETER("input2", "item()*"),
    SXML_OPTIONAL_PARAMETER("options", "(xs:string | map(*))?")};
static const sxml_function_parameter sort_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("key", "(fn(item()) as xs:anyAtomicType*)?")};

static const sxml_native_function sequence_functions[] = {
    SXML_NULLARY_FUNCTION_ROW("true", "xs:boolean", 0, call_true),
    SXML_NULLARY_FUNCTION_ROW("false", "xs:boolean", 0, call_false),
    SXML_FUNCTION_ROW("boolean", input_parameters, "xs:boolean", 0, call_boolean),
    SXML_FUNCTION_ROW("not", input_parameters, "xs:boolean", 0, call_not),
    SXML_FUNCTION_ROW("data", data_parameters, "xs:anyAtomicType*", 0, call_data),
    SXML_FUNCTION_ROW("count", input_parameters, "xs:integer", 0, call_count),
    SXML_FUNCTION_ROW("empty", input_parameters, "xs:boolean", 0, call_empty),
    SXML_FUNCTION_ROW("exists", input_parameters, "xs:boolean", 0, call_exists),
    SXML_FUNCTION_ROW("head", input_parameters, "item()?", 0, call_head),
    SXML_FUNCTION_ROW("tail", input_parameters, "item()*", 0, call_tail),
    SXML_FUNCTION_ROW("subsequence", subsequence_parameters, "item()*", 0,
                      call_subsequence),
    SXML_FUNCTION_ROW("reverse", input_parameters, "item()*", 0, call_reverse),
    SXML_FUNCTION_ROW("zero-or-one", input_parameters, "item()?", 0, call_zero_or_one),
    SXML_FUNCTION_ROW("one-or-more", input_parameters, "item()+", 0, call_one_or_more),
    SXML_FUNCTION_ROW("exactly-one", input_parameters, "item()", 0, call_exactly_one),
    SXML_FUNCTION_ROW("index-of", index_of_parameters, "xs:integer*", 0, call_index_of),
    SXML_FUNCTION_ROW("insert-before", insert_before_parameters, "item()*", 0,
                      call_insert_before),
    SXML_FUNCTION_ROW("remove", remove_parameters, "item()*", 0, call_remove),
    SXML_FUNCTION_ROW("distinct-values", distinct_values_parameters, "xs:anyAtomicType*", 0,
                      call_distinct_values),
    SXML_FUNCTION_ROW("deep-equal", deep_equal_parameters, "xs:boolean", 0,
                      sxml_function_library_call_deep_equal),
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("sort"),
     .parameters = {sort_parameters, sizeof(sort_parameters) / sizeof(sort_parameters[0])},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
     .call = NULL,
     .resumable_body = &sort_body},
};

const sxml_native_function_view sxml_function_library_sequence =
    SXML_LIBRARY_PART(sequence_functions);
