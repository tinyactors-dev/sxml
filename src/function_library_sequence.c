/*
 * The sequence part of the function library (packet 1.20, D78): F&O 4.0,
 * section 7 "Functions on Boolean values" (fn:true, fn:false, fn:boolean,
 * fn:not), section 2 "Accessors" (fn:data) and section 14 "Functions on
 * sequences" (fn:count, fn:empty, fn:exists, fn:head, fn:tail,
 * fn:reverse, fn:subsequence, fn:zero-or-one, fn:one-or-more,
 * fn:exactly-one, fn:index-of, fn:insert-before, fn:remove,
 * fn:distinct-values, fn:deep-equal, fn:sort); from packet 1.23b (D91)
 * fn:identity, fn:unordered, fn:void, fn:foot, fn:trunk, fn:slice,
 * fn:items-at, fn:replicate, fn:insert-separator, fn:compare,
 * fn:atomic-equal, fn:all-equal, fn:all-different, fn:duplicate-values
 * and fn:default-collation; from packet 2.17 fn:starts-with-subsequence,
 * fn:ends-with-subsequence and fn:contains-subsequence. From packet 2.13
 * (D116) the collation
 * arguments of fn:compare, fn:index-of, fn:distinct-values,
 * fn:duplicate-values, fn:all-equal, fn:all-different and fn:sort go
 * through the resolver of src/function_library.c: strings compare under
 * the collation, and the distinct-value sets hash collation keys.
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
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(input, call->error_record, &item_count);
    /* Counting a lazy sequence or a concatenation reads its items: one step
       each (D89, D91); other representations know their length. */
    const sxml_sequence_representation representation = sxml_sequence_get_representation(input);
    if (status == SXML_STATUS_OK && (representation == SXML_SEQUENCE_REPRESENTATION_LAZY ||
                                     representation == SXML_SEQUENCE_REPRESENTATION_CONCATENATION)) {
        status = sxml_function_library_charge_steps(call, item_count);
    }
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

/* fn:reverse($input) (F&O 4.0, section 14.1.12): a range in either order
   reverses in O(1) (D143), anything else into a new sequence. */
static sxml_status call_reverse(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(input, call->error_record, &item_count);
    const sxml_sequence_representation representation = sxml_sequence_get_representation(input);
    if (status == SXML_STATUS_OK &&
        (representation == SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE ||
         representation == SXML_SEQUENCE_REPRESENTATION_DESCENDING_INTEGER_RANGE)) {
        sxml_item head = {.representation = 0};
        sxml_item foot = {.representation = 0};
        status = sxml_sequence_get_item(
            input, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
            &head);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                input,
                (sxml_item_request){.item_index = item_count - 1,
                                    .error_record = call->error_record},
                &foot);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* A range's items are inline xs:integer items. */
        const bool is_ascending = representation == SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE;
        const sxml_integer_range bounds = {
            .first_integer = is_ascending ? head.payload.integer : foot.payload.integer,
            .last_integer = is_ascending ? foot.payload.integer : head.payload.integer};
        return is_ascending
                   ? sxml_sequence_make_descending_integer_range(call->item_heap, bounds, result)
                   : sxml_sequence_make_integer_range(call->item_heap, bounds, result);
    }
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
    sxml_collation collation;
    sxml_status status = sxml_function_library_resolve_collation(call, 2, &collation);
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
        const sxml_status compared = sxml_function_library_compare_collated(
            call->item_heap,
            &(sxml_collated_comparison){
                .collation = &collation,
                .comparison = {.first = step.item,
                               .second = target.item,
                               .implicit_timezone_offset_minutes = implicit_timezone,
                               .error_record = NULL,
                               .name_pool = sxml_item_heap_get_name_pool(call->item_heap)}},
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

/* An open-addressing set of item positions, by fn:atomic-equal, or by
   collation keys for strings under a collation other than the codepoint
   collation (F&O 4.0, fn:distinct-values: "equal ... using the
   collation"). */
typedef struct distinct_set {
    const sxml_function_call *call;
    sxml_item_heap *item_heap;
    const sxml_collation *collation;
    /* Kept items, borrowed from the input; slots hold position + 1. */
    sxml_item *items;
    /* What each kept item is compared as when that differs: a date or
       time without timezone given the implicit one (owned); a zero item
       otherwise (sxml_function_library_adjust_to_implicit_timezone). */
    sxml_item *compared;
    /* The collation key of each kept item; NULL under the codepoint
       collation. */
    sxml_collated_key *keys;
    size_t item_count;
    size_t *slots;
    size_t slot_count;
} distinct_set;

static bool uses_keys(const distinct_set *set) {
    return !sxml_collation_is_codepoint_order(set->collation);
}

static void release_distinct_set(distinct_set *set) {
    for (size_t item_index = 0; set->keys != NULL && item_index < set->item_count;
         item_index += 1) {
        sxml_collation_key_release(&set->keys[item_index].key);
    }
    for (size_t item_index = 0; set->compared != NULL && item_index < set->item_count;
         item_index += 1) {
        if (set->compared[item_index].representation != 0) {
            sxml_item_release(set->compared[item_index]);
        }
    }
    sxml_item_heap_release_bytes(
        set->item_heap, (sxml_heap_bytes){.memory = set->items,
                                          .byte_count = set->slot_count * sizeof(sxml_item)});
    sxml_item_heap_release_bytes(
        set->item_heap, (sxml_heap_bytes){.memory = set->compared,
                                          .byte_count = set->slot_count * sizeof(sxml_item)});
    sxml_item_heap_release_bytes(
        set->item_heap,
        (sxml_heap_bytes){.memory = set->keys,
                          .byte_count = set->keys == NULL
                                            ? 0
                                            : set->slot_count * sizeof(sxml_collated_key)});
    sxml_item_heap_release_bytes(
        set->item_heap, (sxml_heap_bytes){.memory = set->slots,
                                          .byte_count = set->slot_count * sizeof(size_t)});
    set->items = NULL;
    set->compared = NULL;
    set->keys = NULL;
    set->slots = NULL;
}

/* FNV-1a over the bytes of a collation key. */
static uint64_t hash_key_bytes(sxml_byte_view bytes) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t byte_index = 0; byte_index < bytes.byte_count; byte_index += 1) {
        hash = (hash ^ bytes.bytes[byte_index]) * UINT64_C(0x100000001b3);
    }
    return hash;
}

/* The hash of an item with its collation key (when keyed). */
typedef struct keyed_item {
    sxml_item item;
    const sxml_collated_key *key;
} keyed_item;

static uint64_t hash_keyed_item(keyed_item item) {
    if (item.key != NULL && item.key->is_keyed) {
        return hash_key_bytes(sxml_collation_key_view(&item.key->key));
    }
    return sxml_atomic_item_hash(item.item);
}

static bool keyed_items_are_equal(keyed_item first, keyed_item second) {
    const bool is_first_keyed = first.key != NULL && first.key->is_keyed;
    const bool is_second_keyed = second.key != NULL && second.key->is_keyed;
    if (is_first_keyed || is_second_keyed) {
        return is_first_keyed && is_second_keyed &&
               sxml_byte_view_equals(sxml_collation_key_view(&first.key->key),
                                     sxml_collation_key_view(&second.key->key));
    }
    return sxml_atomic_item_is_atomic_equal(first.item, second.item);
}

static keyed_item kept_item(const distinct_set *set, size_t item_index) {
    const sxml_item compared = set->compared[item_index];
    return (keyed_item){
        .item = compared.representation != 0 ? compared : set->items[item_index],
        .key = set->keys == NULL ? NULL : &set->keys[item_index]};
}

/* Heap memory for byte_count bytes, or nothing for a count of zero. */
static sxml_status allocate_set_array(sxml_item_heap *item_heap, size_t byte_count,
                                      void **memory) {
    *memory = NULL;
    if (byte_count == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_item_heap_allocate_bytes(
        item_heap,
        (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(sxml_item)},
        memory);
}

/* Makes room for slot_count slots (a power of two) and items. */
static sxml_status allocate_distinct_set(distinct_set *set, size_t slot_count) {
    size_t item_bytes = 0;
    size_t key_bytes = 0;
    size_t slot_bytes = 0;
    sxml_status status = sxml_size_multiply(slot_count, sizeof(sxml_item), &item_bytes);
    if (status == SXML_STATUS_OK && uses_keys(set)) {
        status = sxml_size_multiply(slot_count, sizeof(sxml_collated_key), &key_bytes);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(slot_count, sizeof(size_t), &slot_bytes);
    }
    void *items = NULL;
    void *compared = NULL;
    void *keys = NULL;
    void *slots = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_set_array(set->item_heap, item_bytes, &items);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_set_array(set->item_heap, item_bytes, &compared);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_set_array(set->item_heap, key_bytes, &keys);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_set_array(set->item_heap, slot_bytes, &slots);
    }
    if (status == SXML_STATUS_OK && (items == NULL || compared == NULL || slots == NULL)) {
        /* slot_count is never 0. */
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        sxml_item_heap_release_bytes(set->item_heap,
                                     (sxml_heap_bytes){.memory = items, .byte_count = item_bytes});
        sxml_item_heap_release_bytes(
            set->item_heap, (sxml_heap_bytes){.memory = compared, .byte_count = item_bytes});
        sxml_item_heap_release_bytes(set->item_heap,
                                     (sxml_heap_bytes){.memory = keys, .byte_count = key_bytes});
        sxml_item_heap_release_bytes(set->item_heap,
                                     (sxml_heap_bytes){.memory = slots, .byte_count = slot_bytes});
        return status;
    }
    memset(slots, 0, slot_bytes);
    const size_t old_count = set->item_count;
    if (old_count > 0) {
        memcpy(items, set->items, old_count * sizeof(sxml_item));
        memcpy(compared, set->compared, old_count * sizeof(sxml_item));
        if (keys != NULL) {
            memcpy(keys, set->keys, old_count * sizeof(sxml_collated_key));
        }
    }
    /* The keys moved: release the old arrays without their keys. */
    set->item_count = 0;
    release_distinct_set(set);
    set->items = items;
    set->compared = compared;
    set->keys = keys;
    set->slots = slots;
    set->slot_count = slot_count;
    set->item_count = old_count;
    for (size_t item_index = 0; item_index < old_count; item_index += 1) {
        size_t slot = (size_t)hash_keyed_item(kept_item(set, item_index)) & (slot_count - 1);
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

/* Adds item unless an equal one is in set; addition tells. */
static sxml_status add_distinct(distinct_set *set, sxml_item item,
                                distinct_addition *addition) {
    if ((set->item_count + 1) * 2 > set->slot_count) {
        sxml_status status = allocate_distinct_set(
            set, set->slot_count == 0 ? 16 : set->slot_count * 2);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (set->items == NULL || set->slots == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_item compared = {.representation = 0};
    sxml_status status =
        sxml_function_library_adjust_to_implicit_timezone(set->call, item, &compared);
    sxml_collated_key key = {.is_keyed = false};
    if (status == SXML_STATUS_OK && uses_keys(set)) {
        status = sxml_function_library_make_collated_key(
            set->item_heap,
            (sxml_collated_key_request){.collation = set->collation, .item = item}, &key);
    }
    if (status != SXML_STATUS_OK) {
        if (compared.representation != 0) {
            sxml_item_release(compared);
        }
        return status;
    }
    const keyed_item probe = {.item = compared.representation != 0 ? compared : item,
                              .key = uses_keys(set) ? &key : NULL};
    size_t slot = (size_t)hash_keyed_item(probe) & (set->slot_count - 1);
    while (set->slots[slot] != 0) {
        if (keyed_items_are_equal(kept_item(set, set->slots[slot] - 1), probe)) {
            if (key.is_keyed) {
                sxml_collation_key_release(&key.key);
            }
            if (compared.representation != 0) {
                sxml_item_release(compared);
            }
            addition->is_new = false;
            return SXML_STATUS_OK;
        }
        slot = (slot + 1) & (set->slot_count - 1);
    }
    set->items[set->item_count] = item;
    set->compared[set->item_count] = compared;
    if (set->keys != NULL) {
        set->keys[set->item_count] = key;
    }
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
    sxml_collation collation;
    sxml_status status = sxml_function_library_resolve_collation(call, 1, &collation);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    distinct_set set = {.call = call,
                        .item_heap = call->item_heap,
                        .collation = &collation,
                        .items = NULL,
                        .compared = NULL,
                        .keys = NULL,
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
        /* One step per item (D89, D91). */
        status = sxml_function_library_charge_steps(call, 1);
        if (status == SXML_STATUS_OK) {
            status = add_distinct(&set, step.item, &addition);
        }
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
    /* The collation the keys compare under. */
    sxml_collation collation;
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
    sxml_status status = sxml_function_library_resolve_collation(call, 1, &sort->collation);
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
        sxml_status status = sxml_function_library_compare_collated(
            comparison->call->item_heap,
            &(sxml_collated_comparison){
                .collation = &comparison->sort->collation,
                .comparison = {.first = first_item,
                               .second = second_item,
                               .implicit_timezone_offset_minutes =
                                   comparison->implicit_timezone_offset_minutes,
                               .error_record = comparison->call->error_record,
                               .name_pool =
                                   sxml_item_heap_get_name_pool(comparison->call->item_heap)}},
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
        /* About n log2 n comparisons (D89, D91). */
        uint64_t comparison_count = sort->item_count;
        for (size_t remaining = sort->item_count; remaining > 1; remaining /= 2) {
            comparison_count += sort->item_count;
        }
        status = sxml_function_library_charge_steps(call, comparison_count);
    }
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

/* ------------------------------------------------ Subsequence questions */

/*
 * fn:starts-with-subsequence, fn:ends-with-subsequence and
 * fn:contains-subsequence ($input, $subsequence, $compare :=
 * fn:deep-equal#2) (F&O 4.0, sections 2.2.9, 2.2.7 and 2.2.3) as one
 * resumable body (D75): each candidate offset of $subsequence in $input
 * (0 for starts-with, count($input) - count($subsequence) for ends-with,
 * every one in order for contains) is tried item by item; a step asks for
 * one call of $compare and the next step reads its xs:boolean? (() is
 * false). Without $compare the items are compared by fn:deep-equal's leaf
 * entry, with no call at all.
 */
typedef enum subsequence_question {
    SXML_SUBSEQUENCE_QUESTION_STARTS_WITH,
    SXML_SUBSEQUENCE_QUESTION_ENDS_WITH,
    SXML_SUBSEQUENCE_QUESTION_CONTAINS,
} subsequence_question;

typedef struct subsequence_state {
    uint64_t input_count;
    uint64_t subsequence_count;
    /* The candidate offset in $input and its last value. */
    uint64_t offset;
    uint64_t last_offset;
    /* The position in $subsequence being compared. */
    uint64_t position;
    /* Owned while a call of $compare runs. */
    sxml_sequence call_arguments[2];
} subsequence_state;

static void release_subsequence_state(void *state) {
    subsequence_state *subsequence = state;
    sxml_sequence_release(subsequence->call_arguments[0]);
    sxml_sequence_release(subsequence->call_arguments[1]);
    subsequence->call_arguments[0] = sxml_sequence_empty();
    subsequence->call_arguments[1] = sxml_sequence_empty();
}

/* The pair of items to compare next, borrowed from the arguments. */
static sxml_status read_compared_pair(const sxml_function_call *call,
                                      const subsequence_state *subsequence,
                                      sxml_item_pair *pair) {
    sxml_status status = sxml_sequence_get_item(
        sxml_function_library_get_argument(call, 0),
        (sxml_item_request){.item_index = subsequence->offset + subsequence->position,
                            .error_record = call->error_record},
        &pair->first);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            sxml_function_library_get_argument(call, 1),
            (sxml_item_request){.item_index = subsequence->position,
                                .error_record = call->error_record},
            &pair->second);
    }
    return status;
}

/* Moves to the next pair after a comparison: the next position when the
   items matched, otherwise the next candidate offset. */
typedef struct comparison_outcome {
    bool is_match;
} comparison_outcome;

static void advance_subsequence(subsequence_state *subsequence, comparison_outcome outcome) {
    if (outcome.is_match) {
        subsequence->position += 1;
    } else {
        subsequence->offset += 1;
        subsequence->position = 0;
    }
}

/* The deep equality of two items (the default $compare). */
static sxml_status compare_deeply(const sxml_function_call *call, sxml_item_pair pair,
                                  comparison_outcome *outcome) {
    sxml_sequence comparison_arguments[2] = {sxml_sequence_from_item(pair.first),
                                             sxml_sequence_from_item(pair.second)};
    const sxml_function_call comparison = {
        .function_item = {.representation = 0},
        .arguments = {.sequences = comparison_arguments, .sequence_count = 2},
        .dynamic_context = call->dynamic_context,
        .item_heap = call->item_heap,
        .error_record = call->error_record};
    sxml_sequence is_equal = sxml_sequence_empty();
    const sxml_status status = sxml_function_library_call_deep_equal(&comparison, &is_equal);
    outcome->is_match = status == SXML_STATUS_OK &&
                        sxml_atomic_item_is_atomic_equal(is_equal.content, sxml_atomic_item_true());
    sxml_sequence_release(is_equal);
    return status;
}

static sxml_status step_subsequence(const sxml_native_step_request *request,
                                    sxml_native_step *step, subsequence_question question) {
    subsequence_state *subsequence = request->state;
    const sxml_function_call *call = request->call;
    sxml_optional_item compare = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 2, &compare);
    if (request->step_number == 0) {
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(sxml_function_library_get_argument(call, 0),
                                         call->error_record, &subsequence->input_count);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(sxml_function_library_get_argument(call, 1),
                                         call->error_record, &subsequence->subsequence_count);
        }
        const bool fits = subsequence->subsequence_count <= subsequence->input_count;
        const uint64_t room = fits ? subsequence->input_count - subsequence->subsequence_count : 0;
        subsequence->offset = question == SXML_SUBSEQUENCE_QUESTION_ENDS_WITH ? room : 0;
        subsequence->last_offset = question == SXML_SUBSEQUENCE_QUESTION_CONTAINS ? room
                                                                                 : subsequence->offset;
        if (!fits) {
            /* No candidate offset at all. */
            subsequence->offset = 1;
            subsequence->last_offset = 0;
        }
    } else {
        sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN,
                                   .datum_details = {.boolean = false}};
        const sxml_sequence answer = request->call_result;
        if (sxml_sequence_get_representation(answer) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
            sxml_item_get_kind(answer.content) == SXML_ITEM_KIND_ATOMIC) {
            status = sxml_atomic_item_get_datum(answer.content, &datum);
        }
        sxml_sequence_release(answer);
        release_subsequence_state(subsequence);
        advance_subsequence(subsequence,
                            (comparison_outcome){.is_match = datum.datum_kind ==
                                                                 SXML_DATUM_KIND_BOOLEAN &&
                                                             datum.datum_details.boolean});
    }
    while (status == SXML_STATUS_OK && subsequence->offset <= subsequence->last_offset &&
           subsequence->position < subsequence->subsequence_count) {
        sxml_item_pair pair = {.first = {.representation = 0}, .second = {.representation = 0}};
        status = read_compared_pair(call, subsequence, &pair);
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_charge_steps(call, 1);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (compare.has_item) {
            sxml_item_retain(pair.first);
            sxml_item_retain(pair.second);
            subsequence->call_arguments[0] = sxml_sequence_from_item(pair.first);
            subsequence->call_arguments[1] = sxml_sequence_from_item(pair.second);
            *step = (sxml_native_step){
                .step_kind = SXML_NATIVE_STEP_KIND_CALL,
                .result = sxml_sequence_empty(),
                .function_item = compare.item,
                .arguments = {.sequences = subsequence->call_arguments, .sequence_count = 2}};
            return SXML_STATUS_OK;
        }
        comparison_outcome outcome = {.is_match = false};
        status = compare_deeply(call, pair, &outcome);
        advance_subsequence(subsequence, outcome);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_found = subsequence->offset <= subsequence->last_offset;
    *step = (sxml_native_step){
        .step_kind = SXML_NATIVE_STEP_KIND_FINISH,
        .result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_found}),
        .function_item = {.representation = 0},
        .arguments = {.sequences = NULL, .sequence_count = 0}};
    return SXML_STATUS_OK;
}

static sxml_status step_starts_with_subsequence(const sxml_native_step_request *request,
                                                sxml_native_step *step) {
    return step_subsequence(request, step, SXML_SUBSEQUENCE_QUESTION_STARTS_WITH);
}

static sxml_status step_ends_with_subsequence(const sxml_native_step_request *request,
                                              sxml_native_step *step) {
    return step_subsequence(request, step, SXML_SUBSEQUENCE_QUESTION_ENDS_WITH);
}

static sxml_status step_contains_subsequence(const sxml_native_step_request *request,
                                             sxml_native_step *step) {
    return step_subsequence(request, step, SXML_SUBSEQUENCE_QUESTION_CONTAINS);
}

#define SXML_SUBSEQUENCE_BODY(body_name, step_function)                       \
    static const sxml_resumable_native_body body_name = {                     \
        .step = (step_function),                                              \
        .release_state = release_subsequence_state,                           \
        .state_byte_count = sizeof(subsequence_state),                        \
        .state_alignment = alignof(subsequence_state),                        \
    }

SXML_SUBSEQUENCE_BODY(starts_with_subsequence_body, step_starts_with_subsequence);
SXML_SUBSEQUENCE_BODY(ends_with_subsequence_body, step_ends_with_subsequence);
SXML_SUBSEQUENCE_BODY(contains_subsequence_body, step_contains_subsequence);

/* ------------------------------------------------- 4.0 sequence functions */

/* fn:identity($input) and fn:unordered($input) (F&O 4.0, sections 14.1.4
   and 14.1.17): the input itself (the order of fn:unordered's result is
   implementation-dependent; sxml keeps it). */
static sxml_status call_identity(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    sxml_sequence_retain(input);
    *result = input;
    return SXML_STATUS_OK;
}

/* fn:void($input := ()) (F&O 4.0, section 14.1.19): the empty sequence;
   the argument is not read. */
static sxml_status call_void(const sxml_function_call *call, sxml_sequence *result) {
    (void)call;
    *result = sxml_sequence_empty();
    return SXML_STATUS_OK;
}

/* fn:foot($input) and fn:trunk($input) (F&O 4.0, sections 14.1.3 and
   14.1.16): the last item, and every item but the last. */
typedef struct end_part {
    bool is_foot;
} end_part;

static sxml_status call_end_part(const sxml_function_call *call, end_part part,
                                 sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    const sxml_status status = sxml_sequence_count(input, call->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count == 0) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    return make_span(call,
                     (item_span){.input = input,
                                 .first_index = part.is_foot ? item_count - 1 : 0,
                                 .has_end = true,
                                 .end_index = part.is_foot ? item_count : item_count - 1},
                     result);
}

static sxml_status call_foot(const sxml_function_call *call, sxml_sequence *result) {
    return call_end_part(call, (end_part){.is_foot = true}, result);
}

static sxml_status call_trunk(const sxml_function_call *call, sxml_sequence *result) {
    return call_end_part(call, (end_part){.is_foot = false}, result);
}

/* An optional xs:integer argument, clamped to +-2^62 (beyond any sequence
   length, so clamping changes no result) and default_value when absent. */
typedef struct integer_argument {
    size_t argument_index;
    int64_t default_value;
} integer_argument;

static sxml_status read_clamped_integer(const sxml_function_call *call,
                                        integer_argument argument, int64_t *value) {
    sxml_optional_item item;
    sxml_status status = sxml_function_library_read_optional_item(call, argument.argument_index,
                                                                  &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!item.has_item) {
        *value = argument.default_value;
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(item.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        *value = datum.datum_details.integer;
        return SXML_STATUS_OK;
    }
    /* An integer beyond int64_t, as decimal text: the nearest int64_t
       (positions this far out select the same items, but for the parity
       of a step). */
    const sxml_byte_view text = datum.datum_details.decimal_text;
    *value = text.byte_count > 0 && text.bytes[0] == '-' ? INT64_MIN : INT64_MAX;
    return SXML_STATUS_OK;
}

/* Appends the item at item_index of input (from 0) to builder. */
typedef struct item_copy {
    sxml_sequence input;
    uint64_t item_index;
} item_copy;

static sxml_status append_item_at(const sxml_function_call *call, item_copy copy,
                                  sxml_sequence_builder *builder) {
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        copy.input, (sxml_item_request){.item_index = copy.item_index,
                                        .error_record = call->error_record},
        &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, 1);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_item(builder, item);
    }
    return status;
}

/* The positions fn:slice selects, over a count of items. */
typedef struct slice_bounds {
    int64_t start;
    int64_t end;
    int64_t step;
} slice_bounds;

/* $S and $E of fn:slice for a positive or negative $step. A negative
   bound gives at least INT64_MIN + 2 (item_count is at least 1), so it
   can be negated. */
static int64_t normalize_bound(int64_t bound, int64_t item_count, int64_t zero_value) {
    if (bound == 0) {
        return zero_value;
    }
    return bound < 0 ? item_count + bound + 1 : bound;
}

/* -bound, INT64_MAX for INT64_MIN (a raw positive bound is never
   INT64_MIN; a normalized negative one never is). */
static int64_t negate_bound(int64_t bound) {
    return bound == INT64_MIN ? INT64_MAX : -bound;
}

/*
 * fn:slice($input, $start := 0, $end := 0, $step := 0) (F&O 4.0, section
 * 14.1.13). A negative $STEP is fn:slice(fn:reverse($input), -$S, -$E,
 * -$STEP); the reversed positions are read from the input in place.
 */
static sxml_status call_slice(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    uint64_t unsigned_count = 0;
    slice_bounds requested = {.start = 0, .end = 0, .step = 0};
    sxml_status status = sxml_sequence_count(input, call->error_record, &unsigned_count);
    if (status == SXML_STATUS_OK) {
        status = read_clamped_integer(
            call, (integer_argument){.argument_index = 1, .default_value = 0}, &requested.start);
    }
    if (status == SXML_STATUS_OK) {
        status = read_clamped_integer(
            call, (integer_argument){.argument_index = 2, .default_value = 0}, &requested.end);
    }
    if (status == SXML_STATUS_OK) {
        status = read_clamped_integer(
            call, (integer_argument){.argument_index = 3, .default_value = 0}, &requested.step);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (unsigned_count == 0) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    const int64_t item_count = (int64_t)unsigned_count;
    const bool is_negative_step = requested.step < 0;
    int64_t start = normalize_bound(requested.start, item_count, is_negative_step ? item_count : 1);
    int64_t end = normalize_bound(requested.end, item_count, is_negative_step ? 1 : item_count);
    const int64_t signed_step = requested.step != 0 ? requested.step : (end >= start ? 1 : -1);
    /* Positions are computed without overflow for every int64_t argument
       (slice-195): the step's magnitude is unsigned. */
    uint64_t step = (uint64_t)signed_step;
    bool is_reversed = false;
    if (signed_step < 0) {
        /* Over the reversed input: -$S and -$E normalized again, $STEP
           made positive. */
        is_reversed = true;
        start = normalize_bound(negate_bound(start), item_count, 1);
        end = normalize_bound(negate_bound(end), item_count, item_count);
        step = UINT64_C(0) - (uint64_t)signed_step;
    }
    int64_t first = start;
    if (first < 1) {
        /* The first position at or after 1 with (p - S) mod STEP = 0:
           S + k * STEP for the least k with k * STEP >= 1 - S. */
        const uint64_t distance = (uint64_t)1 - (uint64_t)start;
        const uint64_t multiple = distance / step + (distance % step != 0 ? 1U : 0U);
        const uint64_t past_one = multiple * step - distance;
        if (past_one >= (uint64_t)item_count) {
            *result = sxml_sequence_empty();
            return SXML_STATUS_OK;
        }
        first = 1 + (int64_t)past_one;
    }
    const int64_t last = end < item_count ? end : item_count;
    sxml_sequence_builder *builder = NULL;
    status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    for (int64_t position = first; status == SXML_STATUS_OK && position <= last;
         position += (int64_t)step) {
        const uint64_t index = is_reversed ? (uint64_t)(item_count - position)
                                           : (uint64_t)(position - 1);
        status = append_item_at(call, (item_copy){.input = input, .item_index = index}, builder);
        if (step > (uint64_t)(last - position)) {
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* fn:items-at($input, $at) (F&O 4.0, section 14.1.5): the items at the
   positions of $at, in the order of $at; positions out of range give
   nothing. */
static sxml_status call_items_at(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(input, call->error_record, &item_count);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 1));
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        if (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_INTEGER &&
            datum.datum_details.integer >= 1 &&
            (uint64_t)datum.datum_details.integer <= item_count) {
            status = append_item_at(
                call,
                (item_copy){.input = input,
                            .item_index = (uint64_t)datum.datum_details.integer - 1},
                builder);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* fn:replicate($input, $count) (F&O 4.0, section 14.1.11): (1 to $count) !
   $input. One step per item made (D91). */
static sxml_status call_replicate(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    int64_t count = 0;
    uint64_t item_count = 0;
    sxml_status status = read_clamped_integer(
        call, (integer_argument){.argument_index = 1, .default_value = 0}, &count);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(input, call->error_record, &item_count);
    }
    if (status == SXML_STATUS_OK && item_count > 0 && count > 0) {
        status = (uint64_t)count > UINT64_MAX / item_count
                     ? SXML_STATUS_LIMIT_EXCEEDED
                     : sxml_function_library_charge_steps(call, (uint64_t)count * item_count);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    for (int64_t copy_number = 0; status == SXML_STATUS_OK && item_count > 0 && copy_number < count;
         copy_number += 1) {
        status = sxml_sequence_builder_append_sequence(builder, input, call->error_record);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* fn:insert-separator($input, $separator) (F&O 4.0, section 14.1.7):
   $separator between adjacent items. */
static sxml_status call_insert_separator(const sxml_function_call *call,
                                         sxml_sequence *result) {
    const sxml_sequence separator = sxml_function_library_get_argument(call, 1);
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    bool is_first = true;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        status = sxml_function_library_charge_steps(call, 1);
        if (status == SXML_STATUS_OK && !is_first) {
            status = sxml_sequence_builder_append_sequence(builder, separator, call->error_record);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, step.item);
        }
        is_first = false;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* ------------------------------------------------------- Comparing values */

/* fn:compare($value1, $value2, $collation) (F&O 4.0, section 5.3.6): -1,
   0 or 1 by the total order of fn:compare (sxml_atomic_item_compare); the
   empty sequence when either is empty; XPTY0004 for values of different
   type families. */
static sxml_status call_compare(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item first;
    sxml_optional_item second;
    sxml_collation collation;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &second);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_resolve_collation(call, 2, &collation);
    }
    if (status != SXML_STATUS_OK || !first.has_item || !second.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    int16_t implicit_timezone = 0;
    if (call->dynamic_context != NULL) {
        status = sxml_dynamic_context_read_implicit_timezone(call->dynamic_context,
                                                             &implicit_timezone);
    }
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_compare_collated(
            call->item_heap,
            &(sxml_collated_comparison){
                .collation = &collation,
                .comparison = {.first = first.item,
                               .second = second.item,
                               .implicit_timezone_offset_minutes = implicit_timezone,
                               .error_record = call->error_record,
                               .name_pool = sxml_item_heap_get_name_pool(call->item_heap)}},
            &ordering);
    }
    if (status == SXML_STATUS_OK) {
        const int64_t value =
            ordering == SXML_ORDERING_LESS ? -1 : (ordering == SXML_ORDERING_GREATER ? 1 : 0);
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer(value));
    }
    return status;
}

/* fn:atomic-equal($value1, $value2) (F&O 4.0, section 2.2.1). */
static sxml_status call_atomic_equal(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item first;
    sxml_optional_item second;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &second);
    }
    if (status == SXML_STATUS_OK && (!first.has_item || !second.has_item)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){
            .is_true = sxml_atomic_item_is_atomic_equal(first.item, second.item)});
    }
    return status;
}

/* fn:duplicate-values($values, $collation) (F&O 4.0, section 14.2.4):
   each value equal to exactly one earlier one, by the rules of
   fn:distinct-values, in the order of these second appearances. */
static sxml_status call_duplicate_values(const sxml_function_call *call,
                                         sxml_sequence *result) {
    sxml_collation collation;
    sxml_status status = sxml_function_library_resolve_collation(call, 1, &collation);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    distinct_set seen = {.call = call,
                         .item_heap = call->item_heap,
                         .collation = &collation,
                         .items = NULL,
                         .compared = NULL,
                         .keys = NULL,
                         .item_count = 0,
                         .slots = NULL,
                         .slot_count = 0};
    distinct_set duplicates = seen;
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    for (;;) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        /* One step per item (D89, D91). */
        status = sxml_function_library_charge_steps(call, 1);
        distinct_addition addition = {.is_new = false};
        if (status == SXML_STATUS_OK) {
            status = add_distinct(&seen, step.item, &addition);
        }
        if (status == SXML_STATUS_OK && !addition.is_new) {
            status = add_distinct(&duplicates, step.item, &addition);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            call->item_heap,
            (sxml_item_view){.items = duplicates.items, .item_count = duplicates.item_count},
            result);
    }
    release_distinct_set(&seen);
    release_distinct_set(&duplicates);
    return status;
}

/* What fn:all-equal and fn:all-different ask of the distinct values. */
typedef struct distinctness_question {
    bool is_all_equal;
} distinctness_question;

/* fn:all-equal($values, $collation) and fn:all-different($values,
   $collation) (F&O 4.0, sections 14.2.1 and 14.2.2): count(distinct-
   values($values, $collation)) le 1, and eq count($values). */
static sxml_status call_distinctness(const sxml_function_call *call,
                                     distinctness_question question, sxml_sequence *result) {
    sxml_sequence distinct = sxml_sequence_empty();
    sxml_status status = call_distinct_values(call, &distinct);
    uint64_t distinct_count = 0;
    uint64_t item_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(distinct, call->error_record, &distinct_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(sxml_function_library_get_argument(call, 0),
                                     call->error_record, &item_count);
    }
    sxml_sequence_release(distinct);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){
            .is_true = question.is_all_equal ? distinct_count <= 1 : distinct_count == item_count});
    }
    return status;
}

static sxml_status call_all_equal(const sxml_function_call *call, sxml_sequence *result) {
    return call_distinctness(call, (distinctness_question){.is_all_equal = true}, result);
}

static sxml_status call_all_different(const sxml_function_call *call, sxml_sequence *result) {
    return call_distinctness(call, (distinctness_question){.is_all_equal = false}, result);
}

/* fn:default-collation() (F&O 4.0, section 5.3.4): the default collation
   of the static context, read lexically (D131); the Unicode codepoint
   collation when it names none. */
static sxml_status call_default_collation(const sxml_function_call *call,
                                          sxml_sequence *result) {
    const sxml_ir_static_context *context =
        call->dynamic_context == NULL
            ? NULL
            : sxml_dynamic_context_get_static_context(call->dynamic_context);
    const sxml_byte_view default_uri =
        context == NULL || context->default_collation_uri.byte_count == 0
            ? sxml_function_library_codepoint_collation_uri()
            : context->default_collation_uri;
    return sxml_function_library_make_string(call->item_heap, default_uri, result);
}

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
static const sxml_function_parameter void_parameters[] = {
    SXML_OPTIONAL_PARAMETER("input", "item()*")};
static const sxml_function_parameter slice_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"), SXML_OPTIONAL_PARAMETER("start", "xs:integer?"),
    SXML_OPTIONAL_PARAMETER("end", "xs:integer?"), SXML_OPTIONAL_PARAMETER("step", "xs:integer?")};
static const sxml_function_parameter items_at_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"), SXML_REQUIRED_PARAMETER("at", "xs:integer*")};
static const sxml_function_parameter replicate_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_REQUIRED_PARAMETER("count", "xs:nonNegativeInteger")};
static const sxml_function_parameter insert_separator_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_REQUIRED_PARAMETER("separator", "item()*")};
static const sxml_function_parameter compare_parameters[] = {
    SXML_REQUIRED_PARAMETER("value1", "xs:anyAtomicType?"),
    SXML_REQUIRED_PARAMETER("value2", "xs:anyAtomicType?"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?")};
static const sxml_function_parameter atomic_equal_parameters[] = {
    SXML_REQUIRED_PARAMETER("value1", "xs:anyAtomicType"),
    SXML_REQUIRED_PARAMETER("value2", "xs:anyAtomicType")};
static const sxml_function_parameter values_collation_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:anyAtomicType*"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?")};
static const sxml_function_parameter sort_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("key", "(fn(item()) as xs:anyAtomicType*)?")};

static const sxml_function_parameter subsequence_question_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_REQUIRED_PARAMETER("subsequence", "item()*"),
    SXML_OPTIONAL_PARAMETER("compare", "(fn(item(), item()) as xs:boolean?)?")};

#define SXML_SUBSEQUENCE_ROW(name_text, body)                                  \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                           \
     .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,                  \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                      \
     .parameters = {subsequence_question_parameters,                           \
                    sizeof(subsequence_question_parameters) /                  \
                        sizeof(subsequence_question_parameters[0])},           \
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),                  \
     .call = NULL,                                                             \
     .resumable_body = &(body)}

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
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("deep-equal"),
     .parameters = {deep_equal_parameters,
                    sizeof(deep_equal_parameters) / sizeof(deep_equal_parameters[0])},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
     .call = NULL,
     .resumable_body = &sxml_function_library_deep_equal_body},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("sort"),
     .parameters = {sort_parameters, sizeof(sort_parameters) / sizeof(sort_parameters[0])},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
     .call = NULL,
     .resumable_body = &sort_body},
    SXML_SUBSEQUENCE_ROW("starts-with-subsequence", starts_with_subsequence_body),
    SXML_SUBSEQUENCE_ROW("ends-with-subsequence", ends_with_subsequence_body),
    SXML_SUBSEQUENCE_ROW("contains-subsequence", contains_subsequence_body),
    SXML_FUNCTION_ROW("identity", input_parameters, "item()*", 0, call_identity),
    SXML_FUNCTION_ROW("unordered", input_parameters, "item()*", 0, call_identity),
    SXML_FUNCTION_ROW("void", void_parameters, "empty-sequence()", 0, call_void),
    SXML_FUNCTION_ROW("foot", input_parameters, "item()?", 0, call_foot),
    SXML_FUNCTION_ROW("trunk", input_parameters, "item()*", 0, call_trunk),
    SXML_FUNCTION_ROW("slice", slice_parameters, "item()*", 0, call_slice),
    SXML_FUNCTION_ROW("items-at", items_at_parameters, "item()*", 0, call_items_at),
    SXML_FUNCTION_ROW("replicate", replicate_parameters, "item()*", 0, call_replicate),
    SXML_FUNCTION_ROW("insert-separator", insert_separator_parameters, "item()*", 0,
                      call_insert_separator),
    SXML_FUNCTION_ROW("compare", compare_parameters, "xs:integer?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_compare),
    SXML_FUNCTION_ROW("atomic-equal", atomic_equal_parameters, "xs:boolean", 0,
                      call_atomic_equal),
    SXML_FUNCTION_ROW("all-equal", values_collation_parameters, "xs:boolean", 0,
                      call_all_equal),
    SXML_FUNCTION_ROW("all-different", values_collation_parameters, "xs:boolean", 0,
                      call_all_different),
    SXML_FUNCTION_ROW("duplicate-values", values_collation_parameters, "xs:anyAtomicType*", 0,
                      call_duplicate_values),
    SXML_NULLARY_FUNCTION_ROW("default-collation", "xs:string",
                              SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_default_collation),
};

const sxml_native_function_view sxml_function_library_sequence =
    SXML_LIBRARY_PART(sequence_functions);
