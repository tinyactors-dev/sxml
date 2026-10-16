/*
 * The functions on arrays of F&O 4.0, section 15.2 (array:append to
 * array:trunk): one library part, sxml_function_library_array
 * (src/function_library_internal.h, D55, D79). The higher-order rows are
 * the traversals, folds and sorts of src/function_library_higher_order.c,
 * run as resumable bodies (D71, D75); the others are leaves over
 * src/xdm_array.c's persistent arrays (D64). Positions count from 1;
 * FOAY0001 for one out of bounds.
 *
 * Golden file: src/function_library_higher_order.c.
 */
#include "function_library_higher_order_internal.h"

#include "function_library_core_internal.h"

#include "evaluator_internal.h"
#include "xdm_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Helpers */

static sxml_sequence array_argument(const sxml_function_call *call, size_t argument_index) {
    if (argument_index >= call->arguments.sequence_count) {
        return sxml_sequence_empty();
    }
    return call->arguments.sequences[argument_index];
}

static bool array_sequence_is_empty(sxml_sequence sequence) {
    return sxml_sequence_get_representation(sequence) ==
           SXML_SEQUENCE_REPRESENTATION_EMPTY;
}

/* The allocator for work memory: the dynamic context's, or the heap's for
   a call from outside an evaluation. */
static sxml_allocator array_call_allocator(const sxml_function_call *call) {
    return call->dynamic_context != NULL ? call->dynamic_context->allocator
                                         : sxml_item_heap_as_allocator(call->item_heap);
}

static sxml_status create_array_builder(const sxml_function_call *call,
                                        sxml_sequence_builder **builder) {
    return sxml_sequence_builder_create(
        array_call_allocator(call),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, builder);
}

static sxml_status out_of_bounds(const sxml_function_call *call) {
    return sxml_higher_order_report_error(call->error_record,
                                          SXML_HIGHER_ORDER_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS);
}

/* An xs:integer argument as int64_t, saturated beyond its range (any such
   position is out of bounds). */
static sxml_status read_array_integer(sxml_sequence argument, int64_t *integer) {
    sxml_atomic_datum datum;
    const sxml_status status = sxml_atomic_item_get_datum(argument.content, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        *integer = datum.datum_details.integer;
        return SXML_STATUS_OK;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_DECIMAL) {
        const sxml_byte_view text = datum.datum_details.decimal_text;
        *integer = text.byte_count > 0 && text.bytes[0] == '-' ? INT64_MIN : INT64_MAX;
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The 0-based index of the 1-based position argument, which must lie in
   1 to limit (FOAY0001 otherwise). */
typedef struct position_check {
    size_t argument_index;
    size_t limit;
} position_check;

static sxml_status read_member_index(const sxml_function_call *call, position_check check,
                                     size_t *member_index) {
    int64_t position = 0;
    const sxml_status status =
        read_array_integer(array_argument(call, check.argument_index), &position);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (position < 1 || (uint64_t)position > check.limit) {
        return out_of_bounds(call);
    }
    *member_index = (size_t)position - 1;
    return SXML_STATUS_OK;
}

static sxml_status read_size(const sxml_function_call *call, size_t *member_count) {
    return sxml_array_get_size(array_argument(call, 0).content, member_count);
}

static sxml_sequence array_sequence(sxml_item array) {
    return sxml_sequence_from_item(array);
}

/* -------------------------------------------------------------- Leaves */

/* array:size($array) (F&O 4.0, 15.2.23). */
static sxml_status call_size(const sxml_function_call *call, sxml_sequence *result) {
    size_t member_count = 0;
    const sxml_status status = read_size(call, &member_count);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)member_count));
    }
    return status;
}

/* array:empty($array) (15.2.3). */
static sxml_status call_empty(const sxml_function_call *call, sxml_sequence *result) {
    size_t member_count = 0;
    const sxml_status status = read_size(call, &member_count);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(member_count == 0 ? sxml_atomic_item_true()
                                                            : sxml_atomic_item_false());
    }
    return status;
}

/* array:get($array, $position, $default) (15.2.11): FOAY0001 out of bounds
   without a default, the default with one. */
static sxml_status call_get(const sxml_function_call *call, sxml_sequence *result) {
    size_t member_count = 0;
    int64_t position = 0;
    sxml_status status = read_size(call, &member_count);
    if (status == SXML_STATUS_OK) {
        status = read_array_integer(array_argument(call, 1), &position);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence member = sxml_sequence_empty();
    if (position < 1 || (uint64_t)position > member_count) {
        if (call->arguments.sequence_count < 3) {
            return out_of_bounds(call);
        }
        member = array_argument(call, 2);
    } else {
        status = sxml_array_get_member(array_argument(call, 0).content,
                                       (size_t)position - 1, &member);
    }
    if (status == SXML_STATUS_OK) {
        sxml_sequence_retain(member);
        *result = member;
    }
    return status;
}

/* array:head and array:foot (15.2.12, 15.2.8): the first or last member,
   FOAY0001 for an empty array. */
typedef enum array_end {
    SXML_ARRAY_END_FIRST,
    SXML_ARRAY_END_LAST,
} array_end;

static sxml_status read_end_member(const sxml_function_call *call, array_end end,
                                   sxml_sequence *result) {
    size_t member_count = 0;
    sxml_status status = read_size(call, &member_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (member_count == 0) {
        return out_of_bounds(call);
    }
    sxml_sequence member = sxml_sequence_empty();
    status = sxml_array_get_member(array_argument(call, 0).content,
                                   end == SXML_ARRAY_END_FIRST ? 0 : member_count - 1, &member);
    if (status == SXML_STATUS_OK) {
        sxml_sequence_retain(member);
        *result = member;
    }
    return status;
}

static sxml_status call_head(const sxml_function_call *call, sxml_sequence *result) {
    return read_end_member(call, SXML_ARRAY_END_FIRST, result);
}

static sxml_status call_foot(const sxml_function_call *call, sxml_sequence *result) {
    return read_end_member(call, SXML_ARRAY_END_LAST, result);
}

/* A slice of the array argument, owned, as a sequence. */
static sxml_status make_slice(const sxml_function_call *call,
                              sxml_array_slice_request request, sxml_sequence *result) {
    sxml_item array = {.representation = 0};
    const sxml_status status = sxml_array_slice(call->item_heap, &request, &array);
    if (status == SXML_STATUS_OK) {
        *result = array_sequence(array);
    }
    return status;
}

/* array:tail and array:trunk (15.2.31, 15.2.32): all but the first or
   last member, FOAY0001 for an empty array. */
static sxml_status drop_end_member(const sxml_function_call *call, array_end end,
                                   sxml_sequence *result) {
    size_t member_count = 0;
    const sxml_status status = read_size(call, &member_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (member_count == 0) {
        return out_of_bounds(call);
    }
    return make_slice(call,
                      (sxml_array_slice_request){
                          .array = array_argument(call, 0).content,
                          .first_member_index = end == SXML_ARRAY_END_FIRST ? 1 : 0,
                          .member_count = member_count - 1},
                      result);
}

static sxml_status call_tail(const sxml_function_call *call, sxml_sequence *result) {
    return drop_end_member(call, SXML_ARRAY_END_FIRST, result);
}

static sxml_status call_trunk(const sxml_function_call *call, sxml_sequence *result) {
    return drop_end_member(call, SXML_ARRAY_END_LAST, result);
}

/* array:subarray($array, $start, $length := ()) (15.2.30). */
static sxml_status call_subarray(const sxml_function_call *call, sxml_sequence *result) {
    size_t member_count = 0;
    int64_t start = 0;
    sxml_status status = read_size(call, &member_count);
    if (status == SXML_STATUS_OK) {
        status = read_array_integer(array_argument(call, 1), &start);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (start < 1 || (uint64_t)start > (uint64_t)member_count + 1) {
        return out_of_bounds(call);
    }
    uint64_t length = (uint64_t)member_count + 1 - (uint64_t)start;
    const sxml_sequence length_argument = array_argument(call, 2);
    if (!array_sequence_is_empty(length_argument)) {
        int64_t given_length = 0;
        status = read_array_integer(length_argument, &given_length);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (given_length < 0) {
            return sxml_higher_order_report_error(
                call->error_record, SXML_HIGHER_ORDER_ERROR_NEGATIVE_ARRAY_LENGTH);
        }
        if ((uint64_t)given_length > length) {
            return out_of_bounds(call);
        }
        length = (uint64_t)given_length;
    }
    return make_slice(call,
                      (sxml_array_slice_request){.array = array_argument(call, 0).content,
                                                 .first_member_index = (size_t)start - 1,
                                                 .member_count = (size_t)length},
                      result);
}

/* array:append($array, $member) (15.2.1). */
static sxml_status call_append(const sxml_function_call *call, sxml_sequence *result) {
    sxml_item array = {.representation = 0};
    const sxml_status status = sxml_array_append(
        call->item_heap,
        &(sxml_array_update){.array = array_argument(call, 0).content,
                             .member_index = 0,
                             .member = array_argument(call, 1)},
        &array);
    if (status == SXML_STATUS_OK) {
        *result = array_sequence(array);
    }
    return status;
}

/* array:put($array, $position, $member) (15.2.20). */
static sxml_status call_put(const sxml_function_call *call, sxml_sequence *result) {
    size_t member_count = 0;
    size_t member_index = 0;
    sxml_status status = read_size(call, &member_count);
    if (status == SXML_STATUS_OK) {
        status = read_member_index(
            call, (position_check){.argument_index = 1, .limit = member_count},
            &member_index);
    }
    sxml_item array = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_array_put(
            call->item_heap,
            &(sxml_array_update){.array = array_argument(call, 0).content,
                                 .member_index = member_index,
                                 .member = array_argument(call, 2)},
            &array);
    }
    if (status == SXML_STATUS_OK) {
        *result = array_sequence(array);
    }
    return status;
}

/* array:insert-before($array, $position, $member) (15.2.15): positions 1
   to size + 1. */
static sxml_status call_insert_before(const sxml_function_call *call,
                                      sxml_sequence *result) {
    size_t member_count = 0;
    size_t member_index = 0;
    sxml_status status = read_size(call, &member_count);
    if (status == SXML_STATUS_OK) {
        status = read_member_index(
            call, (position_check){.argument_index = 1, .limit = member_count + 1},
            &member_index);
    }
    sxml_item array = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_array_insert(
            call->item_heap,
            &(sxml_array_update){.array = array_argument(call, 0).content,
                                 .member_index = member_index,
                                 .member = array_argument(call, 2)},
            &array);
    }
    if (status == SXML_STATUS_OK) {
        *result = array_sequence(array);
    }
    return status;
}

/* array:items($array) (15.2.16): the members' concatenation. */
static sxml_status call_items(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_item array = array_argument(call, 0).content;
    size_t member_count = 0;
    sxml_status status = read_size(call, &member_count);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = create_array_builder(call, &builder);
    }
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(array, member_index, &member);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(builder, member,
                                                           call->error_record);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* array:split($array) (15.2.29): one single-member array per member. */
static sxml_status call_split(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_item array = array_argument(call, 0).content;
    size_t member_count = 0;
    sxml_status status = read_size(call, &member_count);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = create_array_builder(call, &builder);
    }
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        sxml_item single = {.representation = 0};
        status = sxml_array_slice(call->item_heap,
                                  &(sxml_array_slice_request){.array = array,
                                                              .first_member_index =
                                                                  member_index,
                                                              .member_count = 1},
                                  &single);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, single);
            sxml_item_release(single);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* Collects members (retained) into a list and makes the array. */
typedef struct member_collection {
    sxml_allocator allocator;
    sxml_member_list members;
    /* The first failure while collecting. */
    sxml_status status;
} member_collection;

static sxml_status collect_member(member_collection *collection, sxml_sequence member) {
    sxml_sequence_retain(member);
    return sxml_member_list_take(collection->allocator, &collection->members, member);
}

static sxml_status finish_collection(const sxml_function_call *call,
                                     member_collection *collection, sxml_sequence *result) {
    sxml_status status = collection->status;
    sxml_item array = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_member_list_make_array(call->item_heap, &collection->members, &array);
    }
    sxml_member_list_release(collection->allocator, &collection->members);
    if (status == SXML_STATUS_OK) {
        *result = array_sequence(array);
    }
    return status;
}

/* array:reverse($array) (15.2.22). */
static sxml_status call_reverse(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_item array = array_argument(call, 0).content;
    member_collection collection = {.allocator = array_call_allocator(call)};
    size_t member_count = 0;
    sxml_status status = read_size(call, &member_count);
    for (size_t index = 0; status == SXML_STATUS_OK && index < member_count; index += 1) {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(array, member_count - index - 1, &member);
        if (status == SXML_STATUS_OK) {
            status = collect_member(&collection, member);
        }
    }
    collection.status = status;
    return finish_collection(call, &collection, result);
}

/* array:join($arrays) (15.2.17). */
static sxml_status call_join(const sxml_function_call *call, sxml_sequence *result) {
    member_collection collection = {.allocator = array_call_allocator(call)};
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(array_argument(call, 0));
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        sxml_iteration_step array_step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &array_step);
        if (status != SXML_STATUS_OK || !array_step.has_item) {
            break;
        }
        size_t member_count = 0;
        status = sxml_array_get_size(array_step.item, &member_count);
        for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
             member_index += 1) {
            sxml_sequence member = sxml_sequence_empty();
            status = sxml_array_get_member(array_step.item, member_index, &member);
            if (status == SXML_STATUS_OK) {
                status = collect_member(&collection, member);
            }
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    collection.status = status;
    return finish_collection(call, &collection, result);
}

/* array:remove($array, $positions) (15.2.21): every position must be in
   bounds; duplicates remove once. */
static sxml_status call_remove(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_item array = array_argument(call, 0).content;
    member_collection collection = {.allocator = array_call_allocator(call)};
    size_t member_count = 0;
    sxml_status status = read_size(call, &member_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            collection.allocator,
            (sxml_allocation_request){.byte_count = member_count == 0 ? 1 : member_count,
                                      .alignment = alignof(bool)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool *is_removed = memory;
    memset(is_removed, 0, member_count == 0 ? 1 : member_count);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(array_argument(call, 1));
    for (;;) {
        sxml_iteration_step position_step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &position_step);
        if (status != SXML_STATUS_OK || !position_step.has_item) {
            break;
        }
        int64_t position = 0;
        status = read_array_integer(sxml_sequence_from_item(position_step.item), &position);
        if (status == SXML_STATUS_OK &&
            (position < 1 || (uint64_t)position > member_count)) {
            status = out_of_bounds(call);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        is_removed[position - 1] = true;
    }
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        if (is_removed[member_index]) {
            continue;
        }
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(array, member_index, &member);
        if (status == SXML_STATUS_OK) {
            status = collect_member(&collection, member);
        }
    }
    sxml_allocator_release(collection.allocator, is_removed);
    collection.status = status;
    return finish_collection(call, &collection, result);
}

/* The integer argument of array:slice, 0 when absent or empty. */
static sxml_status read_slice_bound(const sxml_function_call *call, size_t argument_index,
                                    int64_t *bound) {
    const sxml_sequence argument = array_argument(call, argument_index);
    *bound = 0;
    if (array_sequence_is_empty(argument)) {
        return SXML_STATUS_OK;
    }
    const sxml_status status = read_array_integer(argument, bound);
    /* Keep every bound well inside int64_t, so the rules' sums cannot
       overflow; positions this far out select the same members. */
    if (*bound > INT64_MAX / 4) {
        *bound = INT64_MAX / 4;
    } else if (*bound < -(INT64_MAX / 4)) {
        *bound = -(INT64_MAX / 4);
    }
    return status;
}

/* $S, $E and $STEP of fn:slice (F&O 4.0, 2.1.12) for count items. */
typedef struct slice_bounds {
    int64_t start;
    int64_t end;
    int64_t step;
} slice_bounds;

static slice_bounds resolve_slice_bounds(slice_bounds given, int64_t count) {
    slice_bounds resolved = given;
    if (given.start == 0) {
        resolved.start = given.step < 0 ? count : 1;
    } else if (given.start < 0) {
        resolved.start = count + given.start + 1;
    }
    if (given.end == 0) {
        resolved.end = given.step < 0 ? 1 : count;
    } else if (given.end < 0) {
        resolved.end = count + given.end + 1;
    }
    if (given.step == 0) {
        resolved.step = resolved.end >= resolved.start ? 1 : -1;
    }
    return resolved;
}

/* array:slice($array, $start, $end, $step) (15.2.24): fn:slice on the
   members. A negative step slices the reversed members with the negated
   bounds. */
static sxml_status call_slice(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_item array = array_argument(call, 0).content;
    member_collection collection = {.allocator = array_call_allocator(call)};
    size_t member_count = 0;
    slice_bounds given = {0, 0, 0};
    sxml_status status = read_size(call, &member_count);
    if (status == SXML_STATUS_OK) {
        status = read_slice_bound(call, 1, &given.start);
    }
    if (status == SXML_STATUS_OK) {
        status = read_slice_bound(call, 2, &given.end);
    }
    if (status == SXML_STATUS_OK) {
        status = read_slice_bound(call, 3, &given.step);
    }
    if (status != SXML_STATUS_OK || member_count == 0) {
        collection.status = status;
        return finish_collection(call, &collection, result);
    }
    const int64_t count = (int64_t)member_count;
    slice_bounds bounds = resolve_slice_bounds(given, count);
    const bool is_reversed = bounds.step < 0;
    if (is_reversed) {
        bounds = resolve_slice_bounds(
            (slice_bounds){.start = -bounds.start, .end = -bounds.end, .step = -bounds.step},
            count);
    }
    /* The first position at or after 1 that the step reaches from start. */
    int64_t position = bounds.start;
    if (position < 1) {
        position += ((1 - position + bounds.step - 1) / bounds.step) * bounds.step;
    }
    for (; status == SXML_STATUS_OK && position <= bounds.end && position <= count;
         position += bounds.step) {
        const size_t member_index =
            is_reversed ? (size_t)(count - position) : (size_t)(position - 1);
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(array, member_index, &member);
        if (status == SXML_STATUS_OK) {
            status = collect_member(&collection, member);
        }
    }
    collection.status = status;
    return finish_collection(call, &collection, result);
}

/* array:flatten($input) (15.2.5): arrays replaced by their members at any
   depth, walked with an explicit stack (D71). */
typedef struct flatten_frame {
    /* A sequence whose items, or an array whose members, come next. */
    sxml_sequence value;
    bool is_array;
    uint64_t next_index;
} flatten_frame;

typedef struct flatten_stack {
    sxml_allocator allocator;
    flatten_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
} flatten_stack;

static sxml_status flatten_push(flatten_stack *stack, flatten_frame frame) {
    if (stack->frame_count == stack->frame_capacity) {
        void *frames = stack->frames;
        const sxml_status status = sxml_evaluator_grow(
            stack->allocator,
            &(sxml_evaluator_growth){.elements = &frames,
                                     .element_capacity = &stack->frame_capacity,
                                     .element_count = stack->frame_count,
                                     .required_count = stack->frame_count + 1,
                                     .element_byte_count = sizeof(flatten_frame),
                                     .alignment = alignof(flatten_frame)});
        stack->frames = frames;
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    stack->frames[stack->frame_count] = frame;
    stack->frame_count += 1;
    return SXML_STATUS_OK;
}

static sxml_status flatten_step(flatten_stack *stack, const sxml_function_call *call,
                                sxml_sequence_builder *builder) {
    flatten_frame *frame = &stack->frames[stack->frame_count - 1];
    if (frame->is_array) {
        sxml_sequence member = sxml_sequence_empty();
        const sxml_status status = sxml_array_get_member(
            frame->value.content, (size_t)frame->next_index, &member);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            stack->frame_count -= 1;
            return SXML_STATUS_OK;
        }
        frame->next_index += 1;
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return flatten_push(stack, (flatten_frame){.value = member, .is_array = false,
                                                   .next_index = 0});
    }
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_sequence_get_item(
        frame->value,
        (sxml_item_request){.item_index = frame->next_index,
                            .error_record = call->error_record},
        &item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        stack->frame_count -= 1;
        return SXML_STATUS_OK;
    }
    frame->next_index += 1;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_item_get_kind(item) == SXML_ITEM_KIND_ARRAY) {
        return flatten_push(stack, (flatten_frame){.value = sxml_sequence_from_item(item),
                                                   .is_array = true,
                                                   .next_index = 0});
    }
    return sxml_sequence_builder_append_item(builder, item);
}

static sxml_status call_flatten(const sxml_function_call *call, sxml_sequence *result) {
    flatten_stack stack = {.allocator = array_call_allocator(call)};
    sxml_sequence_builder *builder = NULL;
    sxml_status status = create_array_builder(call, &builder);
    if (status == SXML_STATUS_OK) {
        status = flatten_push(&stack, (flatten_frame){.value = array_argument(call, 0),
                                                      .is_array = false,
                                                      .next_index = 0});
    }
    while (status == SXML_STATUS_OK && stack.frame_count > 0) {
        status = flatten_step(&stack, call, builder);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    if (stack.frames != NULL) {
        sxml_allocator_release(stack.allocator, stack.frames);
    }
    return status;
}

/* array:members($array) (15.2.18): jtree($array)/child::*, the JNodes of
   the members, keyed by position. */
static sxml_status call_members(const sxml_function_call *call, sxml_sequence *result) {
    size_t member_count = 0;
    sxml_status status = read_size(call, &member_count);
    sxml_item root = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_json_node_make_root(call->item_heap, array_argument(call, 0), &root);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_builder *builder = NULL;
    status = create_array_builder(call, &builder);
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        sxml_item child = {.representation = 0};
        status = sxml_json_node_make_child(
            call->item_heap,
            (sxml_json_child_request){.json_node = root, .child_index = member_index},
            &child);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, child);
            sxml_item_release(child);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    sxml_item_release(root);
    return status;
}

/* array:of-members($input) (15.2.19): the ·jvalue· of each JNode. */
static sxml_status call_of_members(const sxml_function_call *call, sxml_sequence *result) {
    member_collection collection = {.allocator = array_call_allocator(call)};
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(array_argument(call, 0));
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        sxml_iteration_step node_step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &node_step);
        if (status != SXML_STATUS_OK || !node_step.has_item) {
            break;
        }
        sxml_json_node_description description;
        status = sxml_json_node_describe(node_step.item, &description);
        if (status == SXML_STATUS_OK) {
            status = collect_member(&collection, description.json_value);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    collection.status = status;
    return finish_collection(call, &collection, result);
}

/* array:index-of($array, $target, $collation) (15.2.13): the positions of
   the members deep-equal to $target with the collation (the formal
   equivalent is array:index-where($array, deep-equal(?, $target,
   $collation))). Deep equality is fn:deep-equal's body
   (src/function_library_deep_equal.c), called once per member; it walks
   on an explicit stack and calls no function items, so this is a leaf. */
static sxml_status call_index_of(const sxml_function_call *call, sxml_sequence *result) {
    size_t member_count = 0;
    sxml_status status = read_size(call, &member_count);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = create_array_builder(call, &builder);
    }
    const sxml_item array = array_argument(call, 0).content;
    /* deep-equal's $options takes the collation string as it is. */
    sxml_sequence comparison_arguments[3] = {sxml_sequence_empty(), array_argument(call, 1),
                                             array_argument(call, 2)};
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        status = sxml_array_get_member(array, member_index, &comparison_arguments[0]);
        sxml_sequence is_equal = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            const sxml_function_call comparison = {
                .function_item = {.representation = 0},
                .arguments = {.sequences = comparison_arguments,
                              .sequence_count = call->arguments.sequence_count},
                .dynamic_context = call->dynamic_context,
                .item_heap = call->item_heap,
                .error_record = call->error_record};
            status = sxml_function_library_call_deep_equal(&comparison, &is_equal);
        }
        if (status == SXML_STATUS_OK &&
            sxml_atomic_item_is_atomic_equal(is_equal.content, sxml_atomic_item_true())) {
            status = sxml_sequence_builder_append_item(
                builder, sxml_atomic_item_from_integer((int64_t)member_index + 1));
        }
        sxml_sequence_release(is_equal);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* ----------------------------------------------------------------- Rows */

SXML_TRAVERSAL_BODY(build_body, SXML_TRAVERSAL_OPERATION_ARRAY_BUILD);
SXML_TRAVERSAL_BODY(for_each_body, SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH);
SXML_TRAVERSAL_BODY(filter_body, SXML_TRAVERSAL_OPERATION_ARRAY_FILTER);
SXML_TRAVERSAL_BODY(index_where_body, SXML_TRAVERSAL_OPERATION_ARRAY_INDEX_WHERE);
SXML_TRAVERSAL_BODY(for_each_pair_body, SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH_PAIR);
SXML_FOLD_BODY(fold_left_body, SXML_FOLD_OPERATION_ARRAY_FOLD_LEFT);
SXML_FOLD_BODY(fold_right_body, SXML_FOLD_OPERATION_ARRAY_FOLD_RIGHT);
SXML_SORT_BODY(sort_body, SXML_SORT_OPERATION_ARRAY_SORT);
SXML_SORT_BODY(sort_by_body, SXML_SORT_OPERATION_ARRAY_SORT_BY);
SXML_SORT_BODY(sort_with_body, SXML_SORT_OPERATION_ARRAY_SORT_WITH);

#define PARAMETER SXML_HIGHER_ORDER_PARAMETER
#define OPTIONAL_PARAMETER SXML_HIGHER_ORDER_OPTIONAL_PARAMETER

/* Parameter types in the F&O 4.0 spelling (15.2). */
#define MEMBER_PREDICATE "fn(item()*, xs:integer) as xs:boolean?"
#define MEMBER_SORT_KEY "(fn(item()*) as xs:anyAtomicType*)?"

static const sxml_function_parameter array_parameters[] = {PARAMETER("array", "array(*)")};
static const sxml_function_parameter append_parameters[] = {
    PARAMETER("array", "array(*)"), PARAMETER("member", "item()*")};
static const sxml_function_parameter build_parameters[] = {
    PARAMETER("input", "item()*"),
    OPTIONAL_PARAMETER("action", "(fn(item(), xs:integer) as item()*)?")};
static const sxml_function_parameter predicate_parameters[] = {
    PARAMETER("array", "array(*)"), PARAMETER("predicate", MEMBER_PREDICATE)};
static const sxml_function_parameter flatten_parameters[] = {PARAMETER("input", "item()*")};
static const sxml_function_parameter fold_left_parameters[] = {
    PARAMETER("array", "array(*)"), PARAMETER("init", "item()*"),
    PARAMETER("action", "fn(item()*, item()*) as item()*")};
static const sxml_function_parameter fold_right_parameters[] = {
    PARAMETER("array", "array(*)"), PARAMETER("init", "item()*"),
    PARAMETER("action", "fn(item()*, item()*) as item()*")};
static const sxml_function_parameter for_each_parameters[] = {
    PARAMETER("array", "array(*)"),
    PARAMETER("action", "fn(item()*, xs:integer) as item()*")};
static const sxml_function_parameter for_each_pair_parameters[] = {
    PARAMETER("array1", "array(*)"), PARAMETER("array2", "array(*)"),
    PARAMETER("action", "fn(item()*, item()*, xs:integer) as item()*")};
static const sxml_function_parameter get_parameters[] = {
    PARAMETER("array", "array(*)"), PARAMETER("position", "xs:integer"),
    OPTIONAL_PARAMETER("default", "item()*")};
static const sxml_function_parameter index_of_parameters[] = {
    PARAMETER("array", "array(*)"), PARAMETER("target", "item()*"),
    OPTIONAL_PARAMETER("collation", "xs:string?")};
static const sxml_function_parameter insert_before_parameters[] = {
    PARAMETER("array", "array(*)"), PARAMETER("position", "xs:integer"),
    PARAMETER("member", "item()*")};
static const sxml_function_parameter join_parameters[] = {PARAMETER("arrays", "array(*)*")};
static const sxml_function_parameter of_members_parameters[] = {
    PARAMETER("input", "jnode()*")};
static const sxml_function_parameter remove_parameters[] = {
    PARAMETER("array", "array(*)"), PARAMETER("positions", "xs:integer*")};
static const sxml_function_parameter slice_parameters[] = {
    PARAMETER("array", "array(*)"), OPTIONAL_PARAMETER("start", "xs:integer?"),
    OPTIONAL_PARAMETER("end", "xs:integer?"), OPTIONAL_PARAMETER("step", "xs:integer?")};
static const sxml_function_parameter sort_parameters[] = {
    PARAMETER("array", "array(*)"), OPTIONAL_PARAMETER("collation", "xs:string?"),
    OPTIONAL_PARAMETER("key", MEMBER_SORT_KEY)};
static const sxml_function_parameter sort_by_parameters[] = {
    PARAMETER("array", "array(*)"),
    PARAMETER("keys", "map(*)*")};
static const sxml_function_parameter sort_with_parameters[] = {
    PARAMETER("array", "array(*)"),
    PARAMETER("comparators", "(fn(item()*, item()*) as xs:integer)+")};
static const sxml_function_parameter subarray_parameters[] = {
    PARAMETER("array", "array(*)"), PARAMETER("start", "xs:integer"),
    OPTIONAL_PARAMETER("length", "xs:integer?")};

#define RESUMABLE(name_text, parameter_array, result_text, body)               \
    SXML_HIGHER_ORDER_RESUMABLE_ROW(SXML_NAMESPACE_ARRAY_FUNCTIONS, name_text, \
                                    parameter_array, result_text, body)
#define LEAF(name_text, parameter_array, result_text, body)                    \
    SXML_HIGHER_ORDER_LEAF_ROW(SXML_NAMESPACE_ARRAY_FUNCTIONS, name_text,      \
                               parameter_array, result_text, body)

static const sxml_native_function array_functions[] = {
    LEAF("append", append_parameters, "array(*)", call_append),
    RESUMABLE("build", build_parameters, "array(*)", build_body),
    LEAF("empty", array_parameters, "xs:boolean", call_empty),
    RESUMABLE("filter", predicate_parameters, "array(*)", filter_body),
    LEAF("flatten", flatten_parameters, "item()*", call_flatten),
    RESUMABLE("fold-left", fold_left_parameters, "item()*", fold_left_body),
    RESUMABLE("fold-right", fold_right_parameters, "item()*", fold_right_body),
    LEAF("foot", array_parameters, "item()*", call_foot),
    RESUMABLE("for-each", for_each_parameters, "array(*)", for_each_body),
    RESUMABLE("for-each-pair", for_each_pair_parameters, "array(*)", for_each_pair_body),
    LEAF("get", get_parameters, "item()*", call_get),
    LEAF("head", array_parameters, "item()*", call_head),
    LEAF("index-of", index_of_parameters, "xs:integer*", call_index_of),
    RESUMABLE("index-where", predicate_parameters, "xs:integer*", index_where_body),
    LEAF("insert-before", insert_before_parameters, "array(*)", call_insert_before),
    LEAF("items", array_parameters, "item()*", call_items),
    LEAF("join", join_parameters, "array(*)", call_join),
    LEAF("members", array_parameters, "jnode()*", call_members),
    LEAF("of-members", of_members_parameters, "array(*)", call_of_members),
    LEAF("put", insert_before_parameters, "array(*)", call_put),
    LEAF("remove", remove_parameters, "array(*)", call_remove),
    LEAF("reverse", array_parameters, "array(*)", call_reverse),
    LEAF("size", array_parameters, "xs:integer", call_size),
    LEAF("slice", slice_parameters, "array(*)", call_slice),
    RESUMABLE("sort", sort_parameters, "array(*)", sort_body),
    RESUMABLE("sort-by", sort_by_parameters, "array(*)", sort_by_body),
    RESUMABLE("sort-with", sort_with_parameters, "array(*)", sort_with_body),
    LEAF("split", array_parameters, "array(*)*", call_split),
    LEAF("subarray", subarray_parameters, "array(*)", call_subarray),
    LEAF("tail", array_parameters, "array(*)", call_tail),
    LEAF("trunk", array_parameters, "array(*)", call_trunk),
};

const sxml_native_function_view sxml_function_library_array = {
    .native_functions = array_functions,
    .native_function_count = sizeof(array_functions) / sizeof(array_functions[0]),
};
