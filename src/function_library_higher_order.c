/*
 * The higher-order functions of F&O 4.0 (section 2.5, except fn:sort,
 * D78), the functions on function items (section 13) and fn:op (section
 * 18.4): one library part, sxml_function_library_higher_order
 * (src/function_library_internal.h, D55, D79). Also the resumable engines
 * that src/function_library_map.c and src/function_library_array.c share
 * (src/function_library_higher_order_internal.h).
 *
 * Golden file: tests/evaluator_test.c's resumable bodies (D75) for the
 * native steps, src/memory.c for layout and comments. No function here
 * calls a function item: every row that needs one is a resumable body
 * whose steps ask the evaluator for the call (D71), so fold-right over a
 * million items uses constant C stack.
 */
#include "function_library_higher_order_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"
#include "schema_scope_internal.h"
#include "xdm_internal.h"
#include "sxml/name.h"
#include "sxml/string.h"
#include "sxml/types.h"

#include <stdalign.h>
#include <string.h>

/* --------------------------------------------------------------- Errors */

static const sxml_static_error higher_order_errors[] = {
    [SXML_HIGHER_ORDER_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAY0001"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "The array position is out of bounds")},
    [SXML_HIGHER_ORDER_ERROR_NEGATIVE_ARRAY_LENGTH] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAY0002"),
         .description = SXML_BYTE_VIEW_INITIALIZER("The array length is negative")},
    [SXML_HIGHER_ORDER_ERROR_DUPLICATE_KEY] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0003"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "Duplicate keys are rejected by the duplicates option")},
    [SXML_HIGHER_ORDER_ERROR_INVALID_OPTION] =
        {.error_status = SXML_STATUS_DYNAMIC_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0005"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "An option has a value that is not permitted")},
    [SXML_HIGHER_ORDER_ERROR_WRONG_TYPE] =
        {.error_status = SXML_STATUS_TYPE_ERROR,
         .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
         .description = SXML_BYTE_VIEW_INITIALIZER(
             "A value does not have the required type")},
};

/* F&O 4.0, fn:apply: fewer members than the function's arity. */
static const sxml_static_error apply_arity_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAP0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The array has fewer members than the function has parameters")};

/* F&O 4.0, fn:op: an operator that is not in the list. */
static const sxml_static_error unknown_operator_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER("fn:op does not know this operator")};

/* XPath 4.0, section 2.2.2: a fn:function-lookup made without a focus
   (or by a dynamic call, which passes none) cannot bind one. */
static const sxml_static_error lookup_focus_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A function found by fn:function-lookup needs the focus, which this "
        "lookup does not have")};

sxml_status sxml_higher_order_report_error(sxml_error_record *error_record,
                                           sxml_higher_order_error error) {
    return sxml_error_record_report(error_record, &higher_order_errors[error]);
}

/* ------------------------------------------------------------- Helpers */

/* The dynamic context's allocator: every resumable call has a dynamic
   context (D75). */
static sxml_allocator call_allocator(const sxml_function_call *call) {
    return call->dynamic_context->allocator;
}

/* The argument at argument_index, or the empty sequence when the call has
   fewer (a parameter whose default the body supplies). */
static sxml_sequence call_argument(const sxml_function_call *call,
                                  size_t argument_index) {
    if (argument_index >= call->arguments.sequence_count) {
        return sxml_sequence_empty();
    }
    return call->arguments.sequences[argument_index];
}

static bool sequence_is_empty(sxml_sequence sequence) {
    return sxml_sequence_get_representation(sequence) ==
           SXML_SEQUENCE_REPRESENTATION_EMPTY;
}

/* The function item of an optional argument: absent when the argument is
   missing or the empty sequence ("default-on-empty", F&O 4.0, 1.5). */
typedef struct optional_function {
    bool has_function;
    sxml_item function_item;
} optional_function;

static optional_function read_optional_function(const sxml_function_call *call,
                                                size_t argument_index) {
    const sxml_sequence argument = call_argument(call, argument_index);
    if (sequence_is_empty(argument)) {
        return (optional_function){.has_function = false,
                                   .function_item = {.representation = 0}};
    }
    return (optional_function){.has_function = true, .function_item = argument.content};
}

/* Releases count call arguments and leaves them empty. */
static void release_call_arguments(sxml_sequence *arguments, size_t count) {
    for (size_t argument_index = 0; argument_index < count; argument_index += 1) {
        sxml_sequence_release(arguments[argument_index]);
        arguments[argument_index] = sxml_sequence_empty();
    }
}

/* Asks the evaluator to call function_item with count arguments (borrowed
   from the state until the next step). */
typedef struct requested_call {
    sxml_item function_item;
    const sxml_sequence *arguments;
    size_t argument_count;
} requested_call;

static sxml_status request_call(requested_call request, sxml_native_step *step) {
    *step = (sxml_native_step){
        .step_kind = SXML_NATIVE_STEP_KIND_CALL,
        .result = sxml_sequence_empty(),
        .function_item = request.function_item,
        .arguments = {.sequences = request.arguments,
                      .sequence_count = request.argument_count}};
    return SXML_STATUS_OK;
}

/* Finishes the call with value (owned: the evaluator takes it). */
static sxml_status finish_with(sxml_sequence value, sxml_native_step *step) {
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                               .result = value,
                               .function_item = {.representation = 0},
                               .arguments = {.sequences = NULL, .sequence_count = 0}};
    return SXML_STATUS_OK;
}

static sxml_sequence boolean_sequence(sxml_effective_boolean_value truth) {
    return sxml_sequence_from_item(truth.is_true ? sxml_atomic_item_true()
                                                 : sxml_atomic_item_false());
}

/* The truth of a predicate's value, coerced to xs:boolean? by the call:
   the empty sequence is false (F&O 4.0, 2.5.5). */
static sxml_status read_truth(sxml_sequence value, sxml_error_record *error_record,
                              sxml_effective_boolean_value *truth) {
    *truth = (sxml_effective_boolean_value){.is_true = false};
    return sxml_evaluator_compute_effective_boolean_value(value, error_record, truth);
}

/* Whether a walk has a next item, member or entry, or must stop. */
typedef struct walk_progress {
    bool has_next;
} walk_progress;

/* An xs:integer position as a single-item sequence. */
static sxml_sequence position_sequence(uint64_t position) {
    return sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)position));
}

/* Makes a sequence builder in the dynamic context's allocator. */
static sxml_status create_builder(const sxml_function_call *call,
                                  sxml_sequence_builder **builder) {
    return sxml_sequence_builder_create(
        call_allocator(call),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, builder);
}

/* Reads an xs:integer item as int64_t, saturating values beyond its range
   (a decimal cell) at INT64_MIN or INT64_MAX. */
static sxml_status read_integer(sxml_item item, int64_t *integer) {
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(item, &datum);
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

/* Grows an array of element_byte_count elements to hold one more. */
typedef struct list_growth {
    void **elements;
    size_t *capacity;
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} list_growth;

static sxml_status grow_list(sxml_allocator allocator, list_growth growth) {
    if (growth.element_count < *growth.capacity) {
        return SXML_STATUS_OK;
    }
    return sxml_evaluator_grow(
        allocator, &(sxml_evaluator_growth){.elements = growth.elements,
                                           .element_capacity = growth.capacity,
                                           .element_count = growth.element_count,
                                           .required_count = growth.element_count + 1,
                                           .element_byte_count = growth.element_byte_count,
                                           .alignment = growth.alignment});
}

/* ---------------------------------------------------------- Member lists */

sxml_status sxml_member_list_take(sxml_allocator allocator, sxml_member_list *list,
                                  sxml_sequence sequence) {
    void *elements = list->sequences;
    sxml_status status =
        grow_list(allocator, (list_growth){.elements = &elements,
                                           .capacity = &list->sequence_capacity,
                                           .element_count = list->sequence_count,
                                           .element_byte_count = sizeof(sxml_sequence),
                                           .alignment = alignof(sxml_sequence)});
    list->sequences = elements;
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(sequence);
        return status;
    }
    list->sequences[list->sequence_count] = sequence;
    list->sequence_count += 1;
    return SXML_STATUS_OK;
}

void sxml_member_list_release(sxml_allocator allocator, sxml_member_list *list) {
    for (size_t member_index = 0; member_index < list->sequence_count;
         member_index += 1) {
        sxml_sequence_release(list->sequences[member_index]);
    }
    if (list->sequences != NULL) {
        sxml_allocator_release(allocator, list->sequences);
    }
    *list = (sxml_member_list){.sequences = NULL, .sequence_count = 0,
                               .sequence_capacity = 0};
}

sxml_status sxml_member_list_make_array(sxml_item_heap *item_heap,
                                        const sxml_member_list *list,
                                        sxml_item *array) {
    return sxml_array_make(item_heap,
                           (sxml_sequence_view){.sequences = list->sequences,
                                                .sequence_count = list->sequence_count},
                           array);
}

/* Appends a borrowed entry to list. */
static sxml_status map_entry_list_append(sxml_allocator allocator,
                                         sxml_map_entry_list *list,
                                         sxml_map_entry entry) {
    void *elements = list->map_entries;
    sxml_status status =
        grow_list(allocator, (list_growth){.elements = &elements,
                                           .capacity = &list->map_entry_capacity,
                                           .element_count = list->map_entry_count,
                                           .element_byte_count = sizeof(sxml_map_entry),
                                           .alignment = alignof(sxml_map_entry)});
    list->map_entries = elements;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    list->map_entries[list->map_entry_count] = entry;
    list->map_entry_count += 1;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Traversal */

/* Where a traversal's items, members or entries come from. */
typedef enum traversal_source {
    SXML_TRAVERSAL_SOURCE_SEQUENCE,
    SXML_TRAVERSAL_SOURCE_SEQUENCE_PAIR,
    SXML_TRAVERSAL_SOURCE_ARRAY,
    SXML_TRAVERSAL_SOURCE_ARRAY_PAIR,
    SXML_TRAVERSAL_SOURCE_MAP,
} traversal_source;

/* How each operation reads its arguments: the source, which argument
   holds the function item, and how many arguments each call takes. */
typedef struct traversal_shape {
    traversal_source source;
    size_t function_argument_index;
    size_t call_argument_count;
} traversal_shape;

static const traversal_shape traversal_shapes[] = {
    [SXML_TRAVERSAL_OPERATION_FOR_EACH] = {SXML_TRAVERSAL_SOURCE_SEQUENCE, 1, 2},
    [SXML_TRAVERSAL_OPERATION_FILTER] = {SXML_TRAVERSAL_SOURCE_SEQUENCE, 1, 2},
    [SXML_TRAVERSAL_OPERATION_INDEX_WHERE] = {SXML_TRAVERSAL_SOURCE_SEQUENCE, 1, 2},
    [SXML_TRAVERSAL_OPERATION_TAKE_WHILE] = {SXML_TRAVERSAL_SOURCE_SEQUENCE, 1, 2},
    [SXML_TRAVERSAL_OPERATION_DROP_WHILE] = {SXML_TRAVERSAL_SOURCE_SEQUENCE, 1, 2},
    [SXML_TRAVERSAL_OPERATION_EVERY] = {SXML_TRAVERSAL_SOURCE_SEQUENCE, 1, 2},
    [SXML_TRAVERSAL_OPERATION_SOME] = {SXML_TRAVERSAL_SOURCE_SEQUENCE, 1, 2},
    [SXML_TRAVERSAL_OPERATION_FOR_EACH_PAIR] = {SXML_TRAVERSAL_SOURCE_SEQUENCE_PAIR, 2, 3},
    [SXML_TRAVERSAL_OPERATION_ARRAY_BUILD] = {SXML_TRAVERSAL_SOURCE_SEQUENCE, 1, 2},
    [SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH] = {SXML_TRAVERSAL_SOURCE_ARRAY, 1, 2},
    [SXML_TRAVERSAL_OPERATION_ARRAY_FILTER] = {SXML_TRAVERSAL_SOURCE_ARRAY, 1, 2},
    [SXML_TRAVERSAL_OPERATION_ARRAY_INDEX_WHERE] = {SXML_TRAVERSAL_SOURCE_ARRAY, 1, 2},
    [SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH_PAIR] = {SXML_TRAVERSAL_SOURCE_ARRAY_PAIR, 2, 3},
    [SXML_TRAVERSAL_OPERATION_MAP_FOR_EACH] = {SXML_TRAVERSAL_SOURCE_MAP, 1, 3},
    [SXML_TRAVERSAL_OPERATION_MAP_FILTER] = {SXML_TRAVERSAL_SOURCE_MAP, 1, 3},
};

/* True for operations whose value is built in the sequence builder. */
static bool traversal_uses_builder(sxml_traversal_operation operation) {
    switch (operation) {
    case SXML_TRAVERSAL_OPERATION_FOR_EACH:
    case SXML_TRAVERSAL_OPERATION_FILTER:
    case SXML_TRAVERSAL_OPERATION_INDEX_WHERE:
    case SXML_TRAVERSAL_OPERATION_TAKE_WHILE:
    case SXML_TRAVERSAL_OPERATION_DROP_WHILE:
    case SXML_TRAVERSAL_OPERATION_FOR_EACH_PAIR:
    case SXML_TRAVERSAL_OPERATION_ARRAY_INDEX_WHERE:
    case SXML_TRAVERSAL_OPERATION_MAP_FOR_EACH:
        return true;
    case SXML_TRAVERSAL_OPERATION_EVERY:
    case SXML_TRAVERSAL_OPERATION_SOME:
    case SXML_TRAVERSAL_OPERATION_ARRAY_BUILD:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FILTER:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH_PAIR:
    case SXML_TRAVERSAL_OPERATION_MAP_FILTER:
        return false;
    }
    return false;
}

void sxml_traversal_release_state(void *state) {
    sxml_traversal_state *traversal = state;
    sxml_sequence_builder_destroy(traversal->builder);
    sxml_member_list_release(traversal->allocator, &traversal->members);
    if (traversal->map_entries.map_entries != NULL) {
        sxml_allocator_release(traversal->allocator, traversal->map_entries.map_entries);
    }
    release_call_arguments(traversal->call_arguments, 3);
}

/* The minimum of two array sizes (the pair traversals stop at the
   shorter, F&O 4.0, 15.2.10). */
static sxml_status read_pair_count(const sxml_function_call *call,
                                   uint64_t *input_count) {
    size_t first_count = 0;
    size_t second_count = 0;
    sxml_status status = sxml_array_get_size(call_argument(call, 0).content, &first_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_array_get_size(call_argument(call, 1).content, &second_count);
    }
    if (status == SXML_STATUS_OK) {
        *input_count = first_count < second_count ? first_count : second_count;
    }
    return status;
}

static sxml_status traversal_start(sxml_traversal_state *state,
                                   const sxml_function_call *call,
                                   sxml_traversal_operation operation) {
    state->operation = operation;
    state->allocator = call_allocator(call);
    state->decided_value = operation == SXML_TRAVERSAL_OPERATION_EVERY;
    sxml_status status = SXML_STATUS_OK;
    if (traversal_uses_builder(operation)) {
        status = create_builder(call, &state->builder);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (traversal_shapes[operation].source) {
    case SXML_TRAVERSAL_SOURCE_SEQUENCE_PAIR:
        state->second_iterator = sxml_sequence_iterator_start(call_argument(call, 1));
        state->iterator = sxml_sequence_iterator_start(call_argument(call, 0));
        return SXML_STATUS_OK;
    case SXML_TRAVERSAL_SOURCE_SEQUENCE:
        state->iterator = sxml_sequence_iterator_start(call_argument(call, 0));
        return SXML_STATUS_OK;
    case SXML_TRAVERSAL_SOURCE_ARRAY: {
        size_t member_count = 0;
        status = sxml_array_get_size(call_argument(call, 0).content, &member_count);
        state->input_count = member_count;
        return status;
    }
    case SXML_TRAVERSAL_SOURCE_ARRAY_PAIR:
        return read_pair_count(call, &state->input_count);
    case SXML_TRAVERSAL_SOURCE_MAP: {
        size_t entry_count = 0;
        status = sxml_map_get_size(call_argument(call, 0).content, &entry_count);
        state->input_count = entry_count;
        return status;
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/*
 * Moves to the next item, member or entry and puts the call's arguments
 * (retained) in state->call_arguments; *has_next is false at the end.
 */
static sxml_status traversal_advance(sxml_traversal_state *state,
                                     const sxml_function_call *call,
                                     walk_progress *progress) {
    const traversal_shape shape = traversal_shapes[state->operation];
    sxml_status status = SXML_STATUS_OK;
    progress->has_next = false;
    switch (shape.source) {
    case SXML_TRAVERSAL_SOURCE_SEQUENCE:
    case SXML_TRAVERSAL_SOURCE_SEQUENCE_PAIR: {
        sxml_iteration_step item_step;
        status = sxml_sequence_iterator_next(&state->iterator, call->error_record,
                                             &item_step);
        if (status != SXML_STATUS_OK || !item_step.has_item) {
            return status;
        }
        sxml_iteration_step second_step = {.has_item = false};
        if (shape.source == SXML_TRAVERSAL_SOURCE_SEQUENCE_PAIR) {
            status = sxml_sequence_iterator_next(&state->second_iterator,
                                                 call->error_record, &second_step);
            if (status != SXML_STATUS_OK || !second_step.has_item) {
                return status;
            }
        }
        state->position += 1;
        state->current_item = item_step.item;
        sxml_item_retain(item_step.item);
        state->call_arguments[0] = sxml_sequence_from_item(item_step.item);
        if (shape.source == SXML_TRAVERSAL_SOURCE_SEQUENCE_PAIR) {
            sxml_item_retain(second_step.item);
            state->call_arguments[1] = sxml_sequence_from_item(second_step.item);
        }
        break;
    }
    case SXML_TRAVERSAL_SOURCE_ARRAY:
    case SXML_TRAVERSAL_SOURCE_ARRAY_PAIR: {
        if (state->position >= state->input_count) {
            return SXML_STATUS_OK;
        }
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(call_argument(call, 0).content,
                                       (size_t)state->position, &member);
        sxml_sequence second_member = sxml_sequence_empty();
        if (status == SXML_STATUS_OK && shape.source == SXML_TRAVERSAL_SOURCE_ARRAY_PAIR) {
            status = sxml_array_get_member(call_argument(call, 1).content,
                                           (size_t)state->position, &second_member);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        state->position += 1;
        sxml_sequence_retain(member);
        state->call_arguments[0] = member;
        if (shape.source == SXML_TRAVERSAL_SOURCE_ARRAY_PAIR) {
            sxml_sequence_retain(second_member);
            state->call_arguments[1] = second_member;
        }
        break;
    }
    case SXML_TRAVERSAL_SOURCE_MAP: {
        if (state->position >= state->input_count) {
            return SXML_STATUS_OK;
        }
        sxml_map_entry entry;
        status = sxml_map_get_entry(call_argument(call, 0).content,
                                    (size_t)state->position, &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        state->position += 1;
        sxml_item_retain(entry.key);
        state->call_arguments[0] = sxml_sequence_from_item(entry.key);
        sxml_sequence_retain(entry.value);
        state->call_arguments[1] = entry.value;
        break;
    }
    }
    state->call_arguments[shape.call_argument_count - 1] =
        position_sequence(state->position);
    progress->has_next = true;
    return SXML_STATUS_OK;
}

/* fn:drop-while once a predicate was false: the current item and every
   item after it (F&O 4.0, 2.5.3). */
static sxml_status traversal_copy_rest(sxml_traversal_state *state,
                                       const sxml_function_call *call) {
    sxml_status status =
        sxml_sequence_builder_append_item(state->builder, state->current_item);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step item_step;
        status = sxml_sequence_iterator_next(&state->iterator, call->error_record,
                                             &item_step);
        if (status != SXML_STATUS_OK || !item_step.has_item) {
            break;
        }
        status = sxml_sequence_builder_append_item(state->builder, item_step.item);
    }
    state->is_finished = true;
    return status;
}

/* Uses the value of the call for the current position (owned: taken). */
static sxml_status traversal_take_value(sxml_traversal_state *state,
                                        const sxml_function_call *call,
                                        sxml_sequence value) {
    sxml_status status = SXML_STATUS_OK;
    sxml_effective_boolean_value truth = {.is_true = false};
    switch (state->operation) {
    case SXML_TRAVERSAL_OPERATION_FOR_EACH:
    case SXML_TRAVERSAL_OPERATION_FOR_EACH_PAIR:
    case SXML_TRAVERSAL_OPERATION_MAP_FOR_EACH:
        if (state->item_demand != 0) {
            uint64_t value_count = 0;
            status = sxml_sequence_count(value, call->error_record, &value_count);
            state->item_count += value_count;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(state->builder, value,
                                                           call->error_record);
        }
        sxml_sequence_release(value);
        break;
    case SXML_TRAVERSAL_OPERATION_ARRAY_BUILD:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH_PAIR:
        status = sxml_member_list_take(state->allocator, &state->members, value);
        break;
    case SXML_TRAVERSAL_OPERATION_FILTER:
    case SXML_TRAVERSAL_OPERATION_INDEX_WHERE:
    case SXML_TRAVERSAL_OPERATION_TAKE_WHILE:
    case SXML_TRAVERSAL_OPERATION_DROP_WHILE:
    case SXML_TRAVERSAL_OPERATION_EVERY:
    case SXML_TRAVERSAL_OPERATION_SOME:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FILTER:
    case SXML_TRAVERSAL_OPERATION_ARRAY_INDEX_WHERE:
    case SXML_TRAVERSAL_OPERATION_MAP_FILTER:
        status = read_truth(value, call->error_record, &truth);
        sxml_sequence_release(value);
        break;
    }
    if (status != SXML_STATUS_OK) {
        release_call_arguments(state->call_arguments, 3);
        return status;
    }
    const bool is_true = truth.is_true;
    switch (state->operation) {
    case SXML_TRAVERSAL_OPERATION_FILTER:
        if (is_true) {
            status = sxml_sequence_builder_append_item(state->builder, state->current_item);
            state->item_count += 1;
        }
        break;
    case SXML_TRAVERSAL_OPERATION_INDEX_WHERE:
    case SXML_TRAVERSAL_OPERATION_ARRAY_INDEX_WHERE:
        if (is_true) {
            status = sxml_sequence_builder_append_item(
                state->builder, sxml_atomic_item_from_integer((int64_t)state->position));
            state->item_count += 1;
        }
        break;
    case SXML_TRAVERSAL_OPERATION_TAKE_WHILE:
        if (is_true) {
            status = sxml_sequence_builder_append_item(state->builder, state->current_item);
            state->item_count += 1;
        } else {
            state->is_finished = true;
        }
        break;
    case SXML_TRAVERSAL_OPERATION_DROP_WHILE:
        if (!is_true) {
            status = traversal_copy_rest(state, call);
        }
        break;
    case SXML_TRAVERSAL_OPERATION_EVERY:
    case SXML_TRAVERSAL_OPERATION_SOME:
        /* fn:every stops at the first false, fn:some at the first true. */
        if (is_true == (state->operation == SXML_TRAVERSAL_OPERATION_SOME)) {
            state->decided_value = is_true;
            state->is_finished = true;
        }
        break;
    case SXML_TRAVERSAL_OPERATION_ARRAY_FILTER:
        if (is_true) {
            sxml_sequence_retain(state->call_arguments[0]);
            status = sxml_member_list_take(state->allocator, &state->members,
                                           state->call_arguments[0]);
        }
        break;
    case SXML_TRAVERSAL_OPERATION_MAP_FILTER:
        if (is_true) {
            sxml_map_entry entry;
            status = sxml_map_get_entry(call_argument(call, 0).content,
                                        (size_t)state->position - 1, &entry);
            if (status == SXML_STATUS_OK) {
                status = map_entry_list_append(state->allocator, &state->map_entries,
                                               entry);
            }
        }
        break;
    case SXML_TRAVERSAL_OPERATION_FOR_EACH:
    case SXML_TRAVERSAL_OPERATION_FOR_EACH_PAIR:
    case SXML_TRAVERSAL_OPERATION_MAP_FOR_EACH:
    case SXML_TRAVERSAL_OPERATION_ARRAY_BUILD:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH_PAIR:
        break;
    }
    release_call_arguments(state->call_arguments, 3);
    /* The items the caller reads are made (an item demand, D259): the
       value is the same for them. fn:drop-while copies the rest at once. */
    if (status == SXML_STATUS_OK && state->item_demand != 0 &&
        state->item_count >= state->item_demand && traversal_uses_builder(state->operation) &&
        state->operation != SXML_TRAVERSAL_OPERATION_DROP_WHILE) {
        state->is_finished = true;
    }
    return status;
}

/* The defaults of an absent function: fn:boolean#1 for fn:every and
   fn:some, fn:identity#1 for array:build (F&O 4.0, 2.5.4, 15.2.2). */
static sxml_status traversal_take_default(sxml_traversal_state *state,
                                          const sxml_function_call *call) {
    if (state->operation == SXML_TRAVERSAL_OPERATION_ARRAY_BUILD) {
        const sxml_sequence member = state->call_arguments[0];
        state->call_arguments[0] = sxml_sequence_empty();
        release_call_arguments(state->call_arguments, 3);
        return sxml_member_list_take(state->allocator, &state->members, member);
    }
    sxml_effective_boolean_value truth = {.is_true = false};
    const sxml_status status =
        read_truth(state->call_arguments[0], call->error_record, &truth);
    release_call_arguments(state->call_arguments, 3);
    if (status == SXML_STATUS_OK &&
        truth.is_true == (state->operation == SXML_TRAVERSAL_OPERATION_SOME)) {
        state->decided_value = truth.is_true;
        state->is_finished = true;
    }
    return status;
}

static sxml_status traversal_finish(sxml_traversal_state *state,
                                    const sxml_function_call *call,
                                    sxml_native_step *step) {
    sxml_status status = SXML_STATUS_OK;
    sxml_sequence value = sxml_sequence_empty();
    sxml_item item = {.representation = 0};
    switch (state->operation) {
    case SXML_TRAVERSAL_OPERATION_EVERY:
    case SXML_TRAVERSAL_OPERATION_SOME:
        return finish_with(
            boolean_sequence((sxml_effective_boolean_value){.is_true = state->decided_value}),
            step);
    case SXML_TRAVERSAL_OPERATION_ARRAY_BUILD:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FILTER:
    case SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH_PAIR:
        status = sxml_member_list_make_array(call->item_heap, &state->members, &item);
        value = sxml_sequence_from_item(item);
        break;
    case SXML_TRAVERSAL_OPERATION_MAP_FILTER:
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = state->map_entries.map_entries,
                                .map_entry_count = state->map_entries.map_entry_count},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                .error_record = call->error_record},
            &item);
        value = sxml_sequence_from_item(item);
        break;
    case SXML_TRAVERSAL_OPERATION_FOR_EACH:
    case SXML_TRAVERSAL_OPERATION_FILTER:
    case SXML_TRAVERSAL_OPERATION_INDEX_WHERE:
    case SXML_TRAVERSAL_OPERATION_TAKE_WHILE:
    case SXML_TRAVERSAL_OPERATION_DROP_WHILE:
    case SXML_TRAVERSAL_OPERATION_FOR_EACH_PAIR:
    case SXML_TRAVERSAL_OPERATION_ARRAY_INDEX_WHERE:
    case SXML_TRAVERSAL_OPERATION_MAP_FOR_EACH:
        status = sxml_sequence_builder_finish(state->builder, &value);
        break;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with(value, step);
}

/* fn:drop-while over an integer range: once the predicate is false, the
   rest of the range from the current item is the result, kept as a range
   (QT4 drop-while-014: head(drop-while(1 to 1000000000, ...))). */
typedef struct drop_while_value {
    sxml_traversal_state *state;
    const sxml_function_call *call;
    /* The predicate's value, owned: taken by the callee. */
    sxml_sequence predicate_value;
} drop_while_value;

typedef struct drop_while_outcome {
    bool is_done;
    /* Owned when is_done. */
    sxml_sequence rest;
} drop_while_outcome;

static sxml_status drop_while_take_range_value(drop_while_value taken,
                                               drop_while_outcome *outcome) {
    sxml_traversal_state *state = taken.state;
    const sxml_function_call *call = taken.call;
    sxml_effective_boolean_value truth = {.is_true = false};
    sxml_status status = read_truth(taken.predicate_value, call->error_record, &truth);
    sxml_sequence_release(taken.predicate_value);
    release_call_arguments(state->call_arguments, 3);
    if (status != SXML_STATUS_OK || truth.is_true) {
        return status;
    }
    const sxml_sequence input = call_argument(call, 0);
    sxml_item first = {.representation = 0};
    uint64_t item_count = 0;
    status = sxml_sequence_get_item(
        input, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
        &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(input, call->error_record, &item_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_integer_range(
            call->item_heap,
            (sxml_integer_range){
                .first_integer = state->current_item.payload.integer,
                .last_integer = (int64_t)((uint64_t)first.payload.integer + (item_count - 1))},
            &outcome->rest);
    }
    outcome->is_done = status == SXML_STATUS_OK;
    return status;
}

sxml_status sxml_traversal_step(const sxml_native_step_request *request,
                                sxml_native_step *step,
                                sxml_traversal_operation operation) {
    sxml_traversal_state *state = request->state;
    const sxml_function_call *call = request->call;
    const traversal_shape shape = traversal_shapes[operation];
    const optional_function action =
        read_optional_function(call, shape.function_argument_index);
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        status = traversal_start(state, call, operation);
        state->item_demand = request->item_demand;
        state->item_count = 0;
    } else if (operation == SXML_TRAVERSAL_OPERATION_DROP_WHILE &&
               sxml_sequence_get_representation(call_argument(call, 0)) ==
                   SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE) {
        drop_while_outcome outcome = {.is_done = false, .rest = sxml_sequence_empty()};
        status = drop_while_take_range_value(
            (drop_while_value){
                .state = state, .call = call, .predicate_value = request->call_result},
            &outcome);
        if (status == SXML_STATUS_OK && outcome.is_done) {
            return finish_with(outcome.rest, step);
        }
    } else {
        status = traversal_take_value(state, call, request->call_result);
    }
    while (status == SXML_STATUS_OK) {
        if (state->is_finished) {
            return traversal_finish(state, call, step);
        }
        walk_progress progress = {.has_next = false};
        status = traversal_advance(state, call, &progress);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!progress.has_next) {
            state->is_finished = true;
            continue;
        }
        if (!action.has_function) {
            status = traversal_take_default(state, call);
            continue;
        }
        return request_call((requested_call){.function_item = action.function_item,
                                             .arguments = state->call_arguments,
                                             .argument_count = shape.call_argument_count},
                            step);
    }
    return status;
}

/* ---------------------------------------------------------------- Folds */

void sxml_fold_release_state(void *state) {
    sxml_fold_state *fold = state;
    sxml_sequence_release(fold->accumulator);
    sxml_sequence_builder_destroy(fold->builder);
    release_call_arguments(fold->call_arguments, 3);
}

static bool fold_is_over_array(sxml_fold_operation operation) {
    return operation == SXML_FOLD_OPERATION_ARRAY_FOLD_LEFT ||
           operation == SXML_FOLD_OPERATION_ARRAY_FOLD_RIGHT;
}

static bool fold_is_from_the_right(sxml_fold_operation operation) {
    return operation == SXML_FOLD_OPERATION_FOLD_RIGHT ||
           operation == SXML_FOLD_OPERATION_ARRAY_FOLD_RIGHT;
}

/* Appends [accumulator] to fn:scan's result (F&O 4.0, 2.5.15). */
static sxml_status scan_append_accumulator(sxml_fold_state *state,
                                           const sxml_function_call *call) {
    sxml_item array = {.representation = 0};
    sxml_status status = sxml_array_make(
        call->item_heap,
        (sxml_sequence_view){.sequences = &state->accumulator, .sequence_count = 1},
        &array);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_item(state->builder, array);
        sxml_item_release(array);
    }
    return status;
}

static sxml_status fold_start(sxml_fold_state *state, const sxml_function_call *call,
                              sxml_fold_operation operation) {
    state->operation = operation;
    state->allocator = call_allocator(call);
    state->accumulator = call_argument(call, 1);
    sxml_sequence_retain(state->accumulator);
    const sxml_sequence input = call_argument(call, 0);
    sxml_status status = SXML_STATUS_OK;
    if (fold_is_over_array(operation)) {
        size_t member_count = 0;
        status = sxml_array_get_size(input.content, &member_count);
        state->input_count = member_count;
    } else if (operation == SXML_FOLD_OPERATION_FOLD_RIGHT) {
        status = sxml_sequence_count(input, call->error_record, &state->input_count);
    } else {
        state->iterator = sxml_sequence_iterator_start(input);
    }
    if (status == SXML_STATUS_OK && operation == SXML_FOLD_OPERATION_SCAN) {
        status = create_builder(call, &state->builder);
        if (status == SXML_STATUS_OK) {
            status = scan_append_accumulator(state, call);
        }
    }
    return status;
}

/* The next item or member of a fold, borrowed; has_next false at the
   end. */
typedef struct fold_element {
    sxml_sequence value;
    bool has_next;
} fold_element;

static sxml_status fold_read_next(sxml_fold_state *state, const sxml_function_call *call,
                                  fold_element *element) {
    element->has_next = false;
    sxml_sequence *value = &element->value;
    const sxml_sequence input = call_argument(call, 0);
    if (state->operation == SXML_FOLD_OPERATION_FOLD_LEFT ||
        state->operation == SXML_FOLD_OPERATION_SCAN) {
        sxml_iteration_step item_step;
        const sxml_status status =
            sxml_sequence_iterator_next(&state->iterator, call->error_record, &item_step);
        if (status == SXML_STATUS_OK && item_step.has_item) {
            *value = sxml_sequence_from_item(item_step.item);
            element->has_next = true;
        }
        return status;
    }
    if (state->position >= state->input_count) {
        return SXML_STATUS_OK;
    }
    const uint64_t index = fold_is_from_the_right(state->operation)
                               ? state->input_count - state->position - 1
                               : state->position;
    sxml_status status = SXML_STATUS_OK;
    if (fold_is_over_array(state->operation)) {
        status = sxml_array_get_member(input.content, (size_t)index, value);
    } else {
        sxml_item item = {.representation = 0};
        status = sxml_sequence_get_item(
            input,
            (sxml_item_request){.item_index = index, .error_record = call->error_record},
            &item);
        *value = sxml_sequence_from_item(item);
    }
    element->has_next = status == SXML_STATUS_OK;
    return status;
}

sxml_status sxml_fold_step(const sxml_native_step_request *request,
                           sxml_native_step *step, sxml_fold_operation operation) {
    sxml_fold_state *state = request->state;
    const sxml_function_call *call = request->call;
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        status = fold_start(state, call, operation);
    } else {
        release_call_arguments(state->call_arguments, 3);
        sxml_sequence_release(state->accumulator);
        state->accumulator = request->call_result;
        if (operation == SXML_FOLD_OPERATION_SCAN) {
            status = scan_append_accumulator(state, call);
        }
    }
    fold_element element = {.value = sxml_sequence_empty(), .has_next = false};
    if (status == SXML_STATUS_OK) {
        status = fold_read_next(state, call, &element);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence value = element.value;
    if (!element.has_next) {
        sxml_sequence result = sxml_sequence_empty();
        if (operation == SXML_FOLD_OPERATION_SCAN) {
            status = sxml_sequence_builder_finish(state->builder, &result);
        } else {
            result = state->accumulator;
            state->accumulator = sxml_sequence_empty();
        }
        return status == SXML_STATUS_OK ? finish_with(result, step) : status;
    }
    if (operation != SXML_FOLD_OPERATION_SCAN && call->dynamic_context != NULL) {
        /* An early exit: every later call would return the accumulator. */
        sxml_fixpoint_answer answer = {.is_fixpoint = false};
        status = sxml_evaluator_test_fixpoint(
            call->dynamic_context,
            &(sxml_fixpoint_question){.function_item = call_argument(call, 2).content,
                                      .accumulator = state->accumulator,
                                      .accumulator_index =
                                          fold_is_from_the_right(operation) ? 1 : 0},
            &answer);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (answer.is_fixpoint) {
            const sxml_sequence result = state->accumulator;
            state->accumulator = sxml_sequence_empty();
            return finish_with(result, step);
        }
    }
    state->position += 1;
    sxml_sequence_retain(value);
    sxml_sequence_retain(state->accumulator);
    size_t argument_count = 2;
    if (fold_is_from_the_right(operation)) {
        state->call_arguments[0] = value;
        state->call_arguments[1] = state->accumulator;
    } else {
        state->call_arguments[0] = state->accumulator;
        state->call_arguments[1] = value;
    }
    if (operation == SXML_FOLD_OPERATION_SCAN) {
        state->call_arguments[2] = position_sequence(state->position);
        argument_count = 3;
    }
    return request_call((requested_call){.function_item = call_argument(call, 2).content,
                                         .arguments = state->call_arguments,
                                         .argument_count = argument_count},
                        step);
}

/* ------------------------------------------------- do-until and while-do */

/* fn:do-until and fn:while-do (F&O 4.0, 2.5.2, 2.5.23): the value and
   its position, and which function the call in progress is. */
typedef struct loop_state {
    sxml_sequence value;
    uint64_t position;
    bool is_calling_action;
    sxml_sequence call_arguments[2];
} loop_state;

static void release_loop_state(void *state) {
    loop_state *loop = state;
    sxml_sequence_release(loop->value);
    release_call_arguments(loop->call_arguments, 2);
}

typedef enum loop_kind {
    SXML_LOOP_KIND_DO_UNTIL,
    SXML_LOOP_KIND_WHILE_DO,
} loop_kind;

/* Calls the action or the predicate (the function argument
   function_index) with the value and position. */
typedef struct loop_call {
    const sxml_function_call *call;
    size_t function_index;
} loop_call;

static sxml_status loop_request(loop_state *state, loop_call request,
                                sxml_native_step *step) {
    sxml_sequence_retain(state->value);
    state->call_arguments[0] = state->value;
    state->call_arguments[1] = position_sequence(state->position);
    return request_call(
        (requested_call){.function_item =
                             call_argument(request.call, request.function_index).content,
                         .arguments = state->call_arguments,
                         .argument_count = 2},
        step);
}

static sxml_status step_loop(const sxml_native_step_request *request,
                             sxml_native_step *step, loop_kind kind) {
    loop_state *state = request->state;
    const sxml_function_call *call = request->call;
    /* do-until: (input, action, predicate); while-do: (input, predicate,
       action). */
    const size_t action_index = kind == SXML_LOOP_KIND_DO_UNTIL ? 1 : 2;
    const size_t predicate_index = kind == SXML_LOOP_KIND_DO_UNTIL ? 2 : 1;
    if (request->step_number == 0) {
        state->value = call_argument(call, 0);
        sxml_sequence_retain(state->value);
        state->position = 1;
        state->is_calling_action = kind == SXML_LOOP_KIND_DO_UNTIL;
        return loop_request(
            state,
            (loop_call){.call = call,
                        .function_index =
                            state->is_calling_action ? action_index : predicate_index},
            step);
    }
    release_call_arguments(state->call_arguments, 2);
    if (state->is_calling_action) {
        sxml_sequence_release(state->value);
        state->value = request->call_result;
        state->is_calling_action = false;
        if (kind == SXML_LOOP_KIND_WHILE_DO) {
            state->position += 1;
        }
        return loop_request(
            state, (loop_call){.call = call, .function_index = predicate_index}, step);
    }
    sxml_effective_boolean_value truth = {.is_true = false};
    const sxml_status status = read_truth(request->call_result, call->error_record, &truth);
    sxml_sequence_release(request->call_result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* do-until stops when the predicate is true, while-do when false. */
    if (truth.is_true == (kind == SXML_LOOP_KIND_DO_UNTIL)) {
        const sxml_sequence value = state->value;
        state->value = sxml_sequence_empty();
        return finish_with(value, step);
    }
    if (kind == SXML_LOOP_KIND_DO_UNTIL) {
        state->position += 1;
    }
    state->is_calling_action = true;
    return loop_request(state, (loop_call){.call = call, .function_index = action_index},
                        step);
}

static sxml_status step_do_until(const sxml_native_step_request *request,
                                 sxml_native_step *step) {
    return step_loop(request, step, SXML_LOOP_KIND_DO_UNTIL);
}

static sxml_status step_while_do(const sxml_native_step_request *request,
                                 sxml_native_step *step) {
    return step_loop(request, step, SXML_LOOP_KIND_WHILE_DO);
}

/* ------------------------------------------------------------ partition */

/* fn:partition (F&O 4.0, 2.5.14): the items of the current partition
   (retained) and the arrays made so far. */
typedef struct partition_state {
    sxml_allocator allocator;
    sxml_sequence_iterator iterator;
    uint64_t position;
    /* The items of the current partition, each a single-item sequence
       (retained): the members of its array. */
    sxml_sequence *partition_items;
    size_t partition_item_count;
    size_t partition_item_capacity;
    sxml_item current_item;
    sxml_sequence_builder *builder;
    sxml_sequence call_arguments[3];
} partition_state;

static void clear_partition(partition_state *state) {
    for (size_t item_index = 0; item_index < state->partition_item_count;
         item_index += 1) {
        sxml_sequence_release(state->partition_items[item_index]);
    }
    state->partition_item_count = 0;
}

static void release_partition_state(void *state) {
    partition_state *partition = state;
    clear_partition(partition);
    if (partition->partition_items != NULL) {
        sxml_allocator_release(partition->allocator, partition->partition_items);
    }
    sxml_sequence_builder_destroy(partition->builder);
    release_call_arguments(partition->call_arguments, 3);
}

static sxml_status partition_add(partition_state *state, sxml_item item) {
    void *elements = state->partition_items;
    const sxml_status status =
        grow_list(state->allocator, (list_growth){.elements = &elements,
                                                  .capacity = &state->partition_item_capacity,
                                                  .element_count = state->partition_item_count,
                                                  .element_byte_count = sizeof(sxml_sequence),
                                                  .alignment = alignof(sxml_sequence)});
    state->partition_items = elements;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item_retain(item);
    state->partition_items[state->partition_item_count] = sxml_sequence_from_item(item);
    state->partition_item_count += 1;
    return SXML_STATUS_OK;
}

/* The current partition as a sequence, owned. */
static sxml_status partition_sequence(const partition_state *state,
                                      const sxml_function_call *call,
                                      sxml_sequence *sequence) {
    return sxml_sequence_concatenate(
        call->item_heap,
        (sxml_sequence_view){.sequences = state->partition_items,
                             .sequence_count = state->partition_item_count},
        sequence);
}

/* Wraps the current partition as an array, one member per item, appends
   it, and empties it (F&O 4.0, 2.5.14). */
static sxml_status partition_close(partition_state *state, const sxml_function_call *call) {
    sxml_item array = {.representation = 0};
    sxml_status status = sxml_array_make(
        call->item_heap,
        (sxml_sequence_view){.sequences = state->partition_items,
                             .sequence_count = state->partition_item_count},
        &array);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_item(state->builder, array);
        sxml_item_release(array);
    }
    clear_partition(state);
    return status;
}

static sxml_status step_partition(const sxml_native_step_request *request,
                                  sxml_native_step *step) {
    partition_state *state = request->state;
    const sxml_function_call *call = request->call;
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        state->allocator = call_allocator(call);
        state->iterator = sxml_sequence_iterator_start(call_argument(call, 0));
        status = create_builder(call, &state->builder);
    } else {
        release_call_arguments(state->call_arguments, 3);
        sxml_effective_boolean_value truth = {.is_true = false};
        status = read_truth(request->call_result, call->error_record, &truth);
        sxml_sequence_release(request->call_result);
        if (status == SXML_STATUS_OK && truth.is_true) {
            status = partition_close(state, call);
        }
        if (status == SXML_STATUS_OK) {
            status = partition_add(state, state->current_item);
        }
    }
    sxml_iteration_step item_step = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_iterator_next(&state->iterator, call->error_record,
                                             &item_step);
    }
    /* The first item opens the first partition without a call. */
    if (status == SXML_STATUS_OK && item_step.has_item && state->position == 0) {
        state->position = 1;
        status = partition_add(state, item_step.item);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&state->iterator, call->error_record,
                                                 &item_step);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!item_step.has_item) {
        if (state->partition_item_count > 0) {
            status = partition_close(state, call);
        }
        sxml_sequence result = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_finish(state->builder, &result);
        }
        return status == SXML_STATUS_OK ? finish_with(result, step) : status;
    }
    state->position += 1;
    state->current_item = item_step.item;
    status = partition_sequence(state, call, &state->call_arguments[0]);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item_retain(item_step.item);
    state->call_arguments[1] = sxml_sequence_from_item(item_step.item);
    state->call_arguments[2] = position_sequence(state->position);
    return request_call((requested_call){.function_item = call_argument(call, 1).content,
                                         .arguments = state->call_arguments,
                                         .argument_count = 3},
                        step);
}

/* ------------------------------------------- apply and partial application */

/* fn:apply and the body of a partially applied function: one call with
   arguments in allocator memory (owned), then its value. */
typedef struct single_call_state {
    sxml_allocator allocator;
    sxml_sequence *arguments;
    size_t argument_count;
} single_call_state;

static void release_single_call_state(void *state) {
    single_call_state *single = state;
    release_call_arguments(single->arguments, single->argument_count);
    if (single->arguments != NULL) {
        sxml_allocator_release(single->allocator, single->arguments);
    }
}

static sxml_status allocate_arguments(single_call_state *state,
                                      const sxml_function_call *call,
                                      size_t argument_count) {
    state->allocator = call_allocator(call);
    if (argument_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(argument_count, sizeof(sxml_sequence), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            state->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_sequence)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->arguments = memory;
    state->argument_count = argument_count;
    for (size_t argument_index = 0; argument_index < argument_count;
         argument_index += 1) {
        state->arguments[argument_index] = sxml_sequence_empty();
    }
    return SXML_STATUS_OK;
}

/* fn:apply($function, $arguments) (F&O 4.0, 2.5.1): the first arity
   members are the arguments; excess members are ignored; fewer is
   FOAP0001. */
static sxml_status step_apply(const sxml_native_step_request *request,
                              sxml_native_step *step) {
    single_call_state *state = request->state;
    const sxml_function_call *call = request->call;
    if (request->step_number > 0) {
        return finish_with(request->call_result, step);
    }
    const sxml_item function_item = call_argument(call, 0).content;
    const sxml_item array = call_argument(call, 1).content;
    sxml_function_item_description description;
    size_t member_count = 0;
    sxml_status status = sxml_function_item_describe(function_item, &description);
    if (status == SXML_STATUS_OK) {
        status = sxml_array_get_size(array, &member_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.arity > member_count) {
        return sxml_error_record_report(call->error_record, &apply_arity_error);
    }
    status = allocate_arguments(state, call, description.arity);
    for (size_t member_index = 0; status == SXML_STATUS_OK &&
                                  member_index < description.arity;
         member_index += 1) {
        status = sxml_array_get_member(array, member_index,
                                       &state->arguments[member_index]);
        sxml_sequence_retain(state->arguments[member_index]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return request_call((requested_call){.function_item = function_item,
                                         .arguments = state->arguments,
                                         .argument_count = state->argument_count},
                        step);
}

/* Reads the next xs:integer of iterator into *position; 0 at the end. */
static sxml_status read_next_position(sxml_sequence_iterator *iterator,
                                      int64_t *position) {
    sxml_iteration_step position_step = {.has_item = false};
    sxml_status status = sxml_sequence_iterator_next(iterator, NULL, &position_step);
    *position = 0;
    if (status == SXML_STATUS_OK && position_step.has_item) {
        status = read_integer(position_step.item, position);
    }
    return status;
}

/*
 * A partially applied function (F&O 4.0, 2.5.13; XPath 4.0, 4.5.2.5) is a
 * function item with this row's body and the captured values: [0] the
 * wrapped function item, [1] the positions bound (xs:integer, ascending),
 * then the bound values in that order. Its signature is the wrapped
 * signature without the bound parameters.
 */
static sxml_status step_partial_application(const sxml_native_step_request *request,
                                            sxml_native_step *step) {
    single_call_state *state = request->state;
    const sxml_function_call *call = request->call;
    if (request->step_number > 0) {
        return finish_with(request->call_result, step);
    }
    sxml_function_item_description description;
    sxml_status status = sxml_function_item_describe(call->function_item, &description);
    if (status != SXML_STATUS_OK || description.captured_values.sequence_count < 2) {
        return status != SXML_STATUS_OK ? status : SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_sequence *captured = description.captured_values.sequences;
    const sxml_item wrapped = captured[0].content;
    sxml_function_item_description wrapped_description;
    status = sxml_function_item_describe(wrapped, &wrapped_description);
    if (status == SXML_STATUS_OK) {
        status = allocate_arguments(state, call, wrapped_description.arity);
    }
    sxml_sequence_iterator bound_positions = sxml_sequence_iterator_start(captured[1]);
    /* The next bound position, 0 after the last. */
    int64_t bound_position = 0;
    if (status == SXML_STATUS_OK) {
        status = read_next_position(&bound_positions, &bound_position);
    }
    size_t bound_index = 0;
    size_t supplied_index = 0;
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < wrapped_description.arity;
         parameter_index += 1) {
        sxml_sequence argument = sxml_sequence_empty();
        if (bound_position == (int64_t)parameter_index + 1) {
            argument = captured[2 + bound_index];
            bound_index += 1;
            status = read_next_position(&bound_positions, &bound_position);
        } else if (supplied_index < call->arguments.sequence_count) {
            argument = call->arguments.sequences[supplied_index];
            supplied_index += 1;
        }
        sxml_sequence_retain(argument);
        state->arguments[parameter_index] = argument;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return request_call((requested_call){.function_item = wrapped,
                                         .arguments = state->arguments,
                                         .argument_count = state->argument_count},
                        step);
}

static const sxml_resumable_native_body partial_application_body = {
    .step = step_partial_application,
    .release_state = release_single_call_state,
    .state_byte_count = sizeof(single_call_state),
    .state_alignment = alignof(single_call_state),
};

/* The row of every partially applied function's body; no library part
   lists it, so no name finds it. */
static const sxml_native_function partial_application_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("partial-apply"),
    .parameters = {NULL, 0},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = NULL,
    .resumable_body = &partial_application_body,
};

/* What fn:partial-apply collects: per parameter of the wrapped function,
   whether it is bound and the coerced value. */
typedef struct partial_binding {
    sxml_allocator allocator;
    size_t parameter_count;
    bool *is_bound;
    sxml_sequence *bound_values;
    sxml_sequence_type *remaining_types;
} partial_binding;

static void release_partial_binding(partial_binding *binding) {
    if (binding->bound_values != NULL) {
        release_call_arguments(binding->bound_values, binding->parameter_count);
        sxml_allocator_release(binding->allocator, binding->bound_values);
    }
    if (binding->is_bound != NULL) {
        sxml_allocator_release(binding->allocator, binding->is_bound);
    }
    if (binding->remaining_types != NULL) {
        sxml_allocator_release(binding->allocator, binding->remaining_types);
    }
}

/* Allocates count elements of element_byte_count bytes, zeroed. */
typedef struct zeroed_allocation {
    size_t count;
    size_t element_byte_count;
    size_t alignment;
} zeroed_allocation;

static sxml_status allocate_zeroed(sxml_allocator allocator,
                                   zeroed_allocation allocation, void **memory) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(
        allocation.count == 0 ? 1 : allocation.count, allocation.element_byte_count,
        &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = allocation.alignment},
            memory);
    }
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, byte_count);
    }
    return status;
}

static sxml_status allocate_partial_binding(partial_binding *binding) {
    void *is_bound = NULL;
    void *bound_values = NULL;
    void *remaining_types = NULL;
    sxml_status status = allocate_zeroed(
        binding->allocator,
        (zeroed_allocation){.count = binding->parameter_count,
                            .element_byte_count = sizeof(bool),
                            .alignment = alignof(bool)},
        &is_bound);
    if (status == SXML_STATUS_OK) {
        binding->is_bound = is_bound;
        status = allocate_zeroed(binding->allocator,
                                 (zeroed_allocation){.count = binding->parameter_count,
                                                     .element_byte_count =
                                                         sizeof(sxml_sequence),
                                                     .alignment = alignof(sxml_sequence)},
                                 &bound_values);
    }
    if (status == SXML_STATUS_OK) {
        binding->bound_values = bound_values;
        status = allocate_zeroed(
            binding->allocator,
            (zeroed_allocation){.count = binding->parameter_count,
                                .element_byte_count = sizeof(sxml_sequence_type),
                                .alignment = alignof(sxml_sequence_type)},
            &remaining_types);
    }
    if (status == SXML_STATUS_OK) {
        binding->remaining_types = remaining_types;
    }
    return status;
}

/* The signature of a function item, with the parameter types of a map or
   array called as a function (XDM 4.0, sections 8.2, 8.4). */
static sxml_sequence_type parameter_type_of(const sxml_call_signature *signature,
                                            sxml_item function_item,
                                            size_t parameter_index) {
    if (signature->has_function_type) {
        return signature->parameter_types.sequence_types[parameter_index];
    }
    return (sxml_sequence_type){
        .item_type = sxml_builtin_type_identifier(
            sxml_item_get_kind(function_item) == SXML_ITEM_KIND_ARRAY
                ? SXML_TYPE_XS_INTEGER
                : SXML_TYPE_XS_ANY_ATOMIC_TYPE),
        .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
}

/* Binds the entries of the map to the parameters they name, coerced to
   the parameter types (F&O 4.0, 2.5.13: keys beyond the arity are
   ignored). */
static sxml_status bind_partial_arguments(const sxml_function_call *call,
                                          const sxml_call_signature *signature,
                                          partial_binding *binding) {
    const sxml_item function_item = call_argument(call, 0).content;
    const sxml_item map = call_argument(call, 1).content;
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        int64_t position = 0;
        if (status == SXML_STATUS_OK) {
            status = read_integer(entry.key, &position);
        }
        if (status == SXML_STATUS_OK && position < 1) {
            status = sxml_higher_order_report_error(call->error_record,
                                                    SXML_HIGHER_ORDER_ERROR_WRONG_TYPE);
        }
        if (status != SXML_STATUS_OK || (uint64_t)position > binding->parameter_count) {
            continue;
        }
        const size_t parameter_index = (size_t)position - 1;
        status = sxml_evaluator_coerce(
            call->dynamic_context,
            &(sxml_coercion_request){
                .value = entry.value,
                .sequence_type = parameter_type_of(signature, function_item, parameter_index),
                .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
                .error_record = call->error_record},
            &binding->bound_values[parameter_index]);
        binding->is_bound[parameter_index] = status == SXML_STATUS_OK;
    }
    return status;
}

/* The wrapped function, its signature and the binding of its parameters;
   the new function's name (0 for an anonymous one, as fn:partial-apply
   makes). */
typedef struct partial_application_parts {
    sxml_item function_item;
    sxml_name_identifier function_name;
    const sxml_call_signature *signature;
    partial_binding *binding;
} partial_application_parts;

/* Makes the partially applied function from a complete binding. */
static sxml_status make_partial_application(const sxml_function_call *call,
                                            const partial_application_parts *parts,
                                            sxml_sequence *result) {
    const sxml_call_signature *signature = parts->signature;
    partial_binding *binding = parts->binding;
    const sxml_item function_item = parts->function_item;
    /* captured: function, positions, values; at most parameter_count + 2. */
    sxml_sequence *captured = NULL;
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        binding->allocator,
        (zeroed_allocation){.count = binding->parameter_count + 2,
                            .element_byte_count = sizeof(sxml_sequence),
                            .alignment = alignof(sxml_sequence)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    captured = memory;
    sxml_sequence_builder *positions = NULL;
    status = create_builder(call, &positions);
    size_t captured_count = 2;
    size_t remaining_count = 0;
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < binding->parameter_count;
         parameter_index += 1) {
        if (binding->is_bound[parameter_index]) {
            status = sxml_sequence_builder_append_item(
                positions, sxml_atomic_item_from_integer((int64_t)parameter_index + 1));
            captured[captured_count] = binding->bound_values[parameter_index];
            captured_count += 1;
        } else {
            binding->remaining_types[remaining_count] =
                parameter_type_of(signature, function_item, parameter_index);
            remaining_count += 1;
        }
    }
    captured[0] = sxml_sequence_from_item(function_item);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(positions, &captured[1]);
    }
    sxml_sequence_builder_destroy(positions);
    sxml_type_identifier partial_signature = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_register_item_type(
            sxml_item_heap_get_type_registry(call->item_heap),
            &(sxml_type_description){
                .variety = SXML_TYPE_VARIETY_FUNCTION,
                .variety_details = {.function_type = {
                                        .is_any_function = false,
                                        .parameter_types = {.sequence_types =
                                                                binding->remaining_types,
                                                            .sequence_type_count =
                                                                remaining_count},
                                        .result_type = signature->result_type}}},
            &partial_signature);
    }
    /* "the annotations of $function" (F&O 4.0, 2.5.13), borrowed from it:
       the new item captures it. */
    sxml_function_item_description description = {.function_annotations = {
                                                       .function_annotations = NULL,
                                                       .function_annotation_count = 0}};
    if (status == SXML_STATUS_OK &&
        sxml_item_get_kind(function_item) == SXML_ITEM_KIND_FUNCTION) {
        status = sxml_function_item_describe(function_item, &description);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_item_make(
            call->item_heap,
            &(sxml_function_item_request){
                .function_name = parts->function_name,
                .signature = partial_signature,
                .function_annotations = description.function_annotations,
                .captured_values = {.sequences = captured, .sequence_count = captured_count},
                .body = {.state = (void *)&partial_application_row,
                         .operations = &sxml_native_function_body_operations}},
            &item);
    }
    sxml_sequence_release(captured[1]);
    sxml_allocator_release(binding->allocator, captured);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:partial-apply($function, $arguments) (F&O 4.0, 2.5.13). */
static sxml_status call_partial_apply(const sxml_function_call *call,
                                      sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_item function_item = call_argument(call, 0).content;
    sxml_function_item_description description;
    sxml_call_signature signature;
    size_t entry_count = 0;
    sxml_status status = sxml_function_item_describe(function_item, &description);
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_describe_call_signature(call->dynamic_context,
                                                        function_item, &signature);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_map_get_size(call_argument(call, 1).content, &entry_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* "If $arguments is the empty map then the function returns $function
       unchanged" (F&O 4.0, 2.5.13). A partial application written with
       placeholders only, such as $m(?), is lowered to fn:partial-apply with
       the empty map, and its value is a new anonymous function (XPath 4.0,
       4.5.2.5), which is observable for a map or array (not a map any more)
       and a named function (no name any more): those are wrapped (D80). */
    if (entry_count == 0 && description.function_name.index == 0 &&
        sxml_item_get_kind(function_item) == SXML_ITEM_KIND_FUNCTION) {
        sxml_item_retain(function_item);
        *result = sxml_sequence_from_item(function_item);
        return SXML_STATUS_OK;
    }
    partial_binding binding = {.allocator = call_allocator(call),
                               .parameter_count = description.arity};
    status = allocate_partial_binding(&binding);
    if (status == SXML_STATUS_OK) {
        status = bind_partial_arguments(call, &signature, &binding);
    }
    if (status == SXML_STATUS_OK) {
        status = make_partial_application(
            call,
            &(partial_application_parts){.function_item = function_item,
                                         .function_name = {.index = 0},
                                         .signature = &signature,
                                         .binding = &binding},
            result);
    }
    release_partial_binding(&binding);
    return status;
}

/* ------------------------------------------------- transitive-closure */

/* fn:transitive-closure (F&O 4.0, 2.5.22): the GNodes found so far, the
   GNodes whose step runs this round, and the values of their steps. */
typedef struct closure_state {
    sxml_sequence found;
    sxml_sequence frontier;
    uint64_t frontier_position;
    sxml_sequence_builder *builder;
    sxml_sequence call_arguments[1];
} closure_state;

static void release_closure_state(void *state) {
    closure_state *closure = state;
    sxml_sequence_release(closure->found);
    sxml_sequence_release(closure->frontier);
    sxml_sequence_builder_destroy(closure->builder);
    release_call_arguments(closure->call_arguments, 1);
}

/* left op right, owned, in *result (node sets in document order). */
typedef struct node_set_operation {
    sxml_node_set_operator node_set_operator;
    sxml_sequence left_operand;
    sxml_sequence right_operand;
} node_set_operation;

static sxml_status apply_node_set_operator(const sxml_function_call *call,
                                           node_set_operation operation,
                                           sxml_sequence *result) {
    return sxml_evaluator_make_node_set_value(
        call->dynamic_context,
        &(sxml_node_set_request){.node_set_operator = operation.node_set_operator,
                                 .left_operand = operation.left_operand,
                                 .right_operand = operation.right_operand,
                                 .error_record = call->error_record},
        result);
}

/* Ends a round: the new GNodes are the steps' values except those found;
   none ends the closure. */
static sxml_status closure_end_round(closure_state *state,
                                     const sxml_function_call *call,
                                     walk_progress *progress) {
    sxml_sequence reached = sxml_sequence_empty();
    sxml_status status = sxml_sequence_builder_finish(state->builder, &reached);
    sxml_sequence fresh = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = apply_node_set_operator(
            call,
            (node_set_operation){.node_set_operator = SXML_NODE_SET_OPERATOR_EXCEPT,
                                 .left_operand = reached,
                                 .right_operand = state->found},
            &fresh);
        sxml_sequence_release(reached);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sequence_is_empty(fresh)) {
        progress->has_next = false;
        return SXML_STATUS_OK;
    }
    sxml_sequence found = sxml_sequence_empty();
    status = apply_node_set_operator(
        call,
        (node_set_operation){.node_set_operator = SXML_NODE_SET_OPERATOR_UNION,
                             .left_operand = state->found,
                             .right_operand = fresh},
        &found);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(fresh);
        return status;
    }
    sxml_sequence_release(state->found);
    state->found = found;
    sxml_sequence_release(state->frontier);
    state->frontier = fresh;
    state->frontier_position = 0;
    progress->has_next = true;
    return SXML_STATUS_OK;
}

static sxml_status step_transitive_closure(const sxml_native_step_request *request,
                                           sxml_native_step *step) {
    closure_state *state = request->state;
    const sxml_function_call *call = request->call;
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        state->frontier = call_argument(call, 0);
        sxml_sequence_retain(state->frontier);
        status = create_builder(call, &state->builder);
    } else {
        release_call_arguments(state->call_arguments, 1);
        status = sxml_sequence_builder_append_sequence(state->builder, request->call_result,
                                                       call->error_record);
        sxml_sequence_release(request->call_result);
    }
    while (status == SXML_STATUS_OK) {
        sxml_item node = {.representation = 0};
        status = sxml_sequence_get_item(
            state->frontier,
            (sxml_item_request){.item_index = state->frontier_position,
                                .error_record = call->error_record},
            &node);
        if (status == SXML_STATUS_OK) {
            state->frontier_position += 1;
            sxml_item_retain(node);
            state->call_arguments[0] = sxml_sequence_from_item(node);
            return request_call(
                (requested_call){.function_item = call_argument(call, 1).content,
                                 .arguments = state->call_arguments,
                                 .argument_count = 1},
                step);
        }
        if (status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
            break;
        }
        walk_progress progress = {.has_next = false};
        status = SXML_STATUS_OK;
        if (state->frontier_position > 0) {
            status = closure_end_round(state, call, &progress);
        }
        if (status == SXML_STATUS_OK && !progress.has_next) {
            const sxml_sequence found = state->found;
            state->found = sxml_sequence_empty();
            return finish_with(found, step);
        }
    }
    return status;
}

/* ----------------------------------------------------------------- Sorts */

void sxml_sort_release_state(void *state) {
    sxml_sort_state *sort = state;
    if (sort->sorted_values != NULL) {
        release_call_arguments(sort->sorted_values, sort->sorted_value_count);
        sxml_allocator_release(sort->allocator, sort->sorted_values);
    }
    if (sort->key_values != NULL) {
        release_call_arguments(sort->key_values,
                               sort->sorted_value_count * sort->key_definition_count);
        sxml_allocator_release(sort->allocator, sort->key_values);
    }
    if (sort->key_definitions != NULL) {
        sxml_allocator_release(sort->allocator, sort->key_definitions);
    }
    if (sort->indices != NULL) {
        sxml_allocator_release(sort->allocator, sort->indices);
    }
    if (sort->scratch_indices != NULL) {
        sxml_allocator_release(sort->allocator, sort->scratch_indices);
    }
    release_call_arguments(sort->call_arguments, 2);
}

static bool sort_is_over_array(sxml_sort_operation operation) {
    return operation == SXML_SORT_OPERATION_ARRAY_SORT ||
           operation == SXML_SORT_OPERATION_ARRAY_SORT_BY ||
           operation == SXML_SORT_OPERATION_ARRAY_SORT_WITH;
}

static bool sort_uses_comparators(sxml_sort_operation operation) {
    return operation == SXML_SORT_OPERATION_SORT_WITH ||
           operation == SXML_SORT_OPERATION_ARRAY_SORT_WITH;
}

static bool sort_uses_key_records(sxml_sort_operation operation) {
    return operation == SXML_SORT_OPERATION_SORT_BY ||
           operation == SXML_SORT_OPERATION_ARRAY_SORT_BY;
}

/* Collects the items (as single-item sequences) or members, retained. */
static sxml_status sort_collect_values(sxml_sort_state *state,
                                       const sxml_function_call *call) {
    const sxml_sequence input = call_argument(call, 0);
    uint64_t value_count = 0;
    sxml_status status = SXML_STATUS_OK;
    if (sort_is_over_array(state->operation)) {
        size_t member_count = 0;
        status = sxml_array_get_size(input.content, &member_count);
        value_count = member_count;
    } else {
        status = sxml_sequence_count(input, call->error_record, &value_count);
    }
    if (status == SXML_STATUS_OK && value_count > SIZE_MAX / sizeof(sxml_sequence)) {
        status = SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(state->allocator,
                                 (zeroed_allocation){.count = (size_t)value_count,
                                                     .element_byte_count =
                                                         sizeof(sxml_sequence),
                                                     .alignment = alignof(sxml_sequence)},
                                 &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->sorted_values = memory;
    state->sorted_value_count = (size_t)value_count;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(input);
    for (size_t value_index = 0;
         status == SXML_STATUS_OK && value_index < state->sorted_value_count;
         value_index += 1) {
        sxml_sequence value = sxml_sequence_empty();
        if (sort_is_over_array(state->operation)) {
            status = sxml_array_get_member(input.content, value_index, &value);
        } else {
            sxml_iteration_step item_step = {.has_item = false};
            status = sxml_sequence_iterator_next(&iterator, call->error_record, &item_step);
            value = sxml_sequence_from_item(item_step.item);
        }
        if (status == SXML_STATUS_OK) {
            sxml_sequence_retain(value);
            state->sorted_values[value_index] = value;
        }
    }
    return status;
}

/* Looks a field of a record (a map with xs:string keys) up. */
typedef struct record_field_lookup {
    sxml_item record;
    sxml_byte_view field_name;
} record_field_lookup;

static sxml_status find_record_field(const sxml_function_call *call,
                                     record_field_lookup lookup, sxml_map_lookup *found) {
    sxml_item key = {.representation = 0};
    sxml_status status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = lookup.field_name,
                                                   .codepoint_count =
                                                       lookup.field_name.byte_count,
                                                   .is_ascii = true}}}},
        &key);
    if (status == SXML_STATUS_OK) {
        status = sxml_map_find(lookup.record, key, found);
        sxml_item_release(key);
    }
    return status;
}

/* Reads one fn:sort-key-record (F&O 4.0, 2.5.19). */
static sxml_status read_sort_key_record(const sxml_function_call *call,
                                        sxml_item record,
                                        sxml_sort_key_definition *definition) {
    sxml_map_lookup found;
    sxml_status status = find_record_field(
        call, (record_field_lookup){.record = record,
                                    .field_name = SXML_BYTE_VIEW_FROM_LITERAL("key")},
        &found);
    if (status == SXML_STATUS_OK && found.is_found && !sequence_is_empty(found.entry.value)) {
        if (sxml_sequence_get_representation(found.entry.value) !=
                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
            !sxml_item_is_function_item(found.entry.value.content)) {
            return sxml_higher_order_report_error(call->error_record,
                                                  SXML_HIGHER_ORDER_ERROR_WRONG_TYPE);
        }
        definition->has_key_function = true;
        definition->key_function = found.entry.value.content;
    }
    if (status == SXML_STATUS_OK) {
        status = find_record_field(
            call,
            (record_field_lookup){.record = record,
                                  .field_name = SXML_BYTE_VIEW_FROM_LITERAL("collation")},
            &found);
    }
    if (status == SXML_STATUS_OK && found.is_found && !sequence_is_empty(found.entry.value)) {
        sxml_atomic_datum collation_uri;
        status = sxml_atomic_item_get_datum(found.entry.value.content, &collation_uri);
        if (status == SXML_STATUS_OK && collation_uri.datum_kind != SXML_DATUM_KIND_STRING) {
            status = sxml_higher_order_report_error(call->error_record,
                                                    SXML_HIGHER_ORDER_ERROR_WRONG_TYPE);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_find_collation(
                call, collation_uri.datum_details.string.utf8, &definition->collation);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = find_record_field(
            call, (record_field_lookup){.record = record,
                                        .field_name = SXML_BYTE_VIEW_FROM_LITERAL("order")},
            &found);
    }
    if (status == SXML_STATUS_OK && found.is_found && !sequence_is_empty(found.entry.value)) {
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(found.entry.value.content, &datum);
        const bool is_ascending =
            status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_STRING &&
            sxml_byte_view_equals(datum.datum_details.string.utf8,
                                  SXML_BYTE_VIEW_FROM_LITERAL("ascending"));
        definition->is_descending =
            status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_STRING &&
            sxml_byte_view_equals(datum.datum_details.string.utf8,
                                  SXML_BYTE_VIEW_FROM_LITERAL("descending"));
        if (status == SXML_STATUS_OK && !is_ascending && !definition->is_descending) {
            /* enum('ascending', 'descending') (F&O 4.0, 2.5.19). */
            status = sxml_higher_order_report_error(call->error_record,
                                                    SXML_HIGHER_ORDER_ERROR_WRONG_TYPE);
        }
    }
    return status;
}

/* The sort key definitions: from records (sort-by), or one from the
   collation and key arguments. */
static sxml_status sort_read_definitions(sxml_sort_state *state,
                                         const sxml_function_call *call) {
    const sxml_sequence records = call_argument(call, 1);
    uint64_t definition_count = 1;
    sxml_status status = SXML_STATUS_OK;
    /* The default collation for definitions that name none, or the
       collation argument (F&O 4.0, 5.2.7). */
    sxml_collation collation;
    if (sort_uses_key_records(state->operation)) {
        status = sxml_sequence_count(records, call->error_record, &definition_count);
        if (definition_count == 0) {
            definition_count = 1;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_find_collation(
                call, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &collation);
        }
    } else {
        status = sxml_function_library_resolve_collation(call, 1, &collation);
    }
    if (status == SXML_STATUS_OK &&
        definition_count > SIZE_MAX / sizeof(sxml_sort_key_definition)) {
        status = SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            state->allocator,
            (zeroed_allocation){.count = (size_t)definition_count,
                                .element_byte_count = sizeof(sxml_sort_key_definition),
                                .alignment = alignof(sxml_sort_key_definition)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->key_definitions = memory;
    state->key_definition_count = (size_t)definition_count;
    for (size_t definition_index = 0; definition_index < state->key_definition_count;
         definition_index += 1) {
        state->key_definitions[definition_index].collation = collation;
    }
    if (!sort_uses_key_records(state->operation)) {
        const optional_function key = read_optional_function(call, 2);
        state->key_definitions[0].has_key_function = key.has_function;
        state->key_definitions[0].key_function = key.function_item;
        return SXML_STATUS_OK;
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(records);
    for (size_t definition_index = 0;
         status == SXML_STATUS_OK && definition_index < state->key_definition_count;
         definition_index += 1) {
        sxml_iteration_step record_step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &record_step);
        if (status == SXML_STATUS_OK && record_step.has_item) {
            status = read_sort_key_record(call, record_step.item,
                                          &state->key_definitions[definition_index]);
        }
    }
    return status;
}

/* positions 0 to count - 1 in *indices, allocated. */
static sxml_status allocate_indices(sxml_allocator allocator, size_t count,
                                    size_t **indices) {
    void *memory = NULL;
    const sxml_status status = allocate_zeroed(
        allocator,
        (zeroed_allocation){.count = count, .element_byte_count = sizeof(size_t),
                            .alignment = alignof(size_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *indices = memory;
    for (size_t index = 0; index < count; index += 1) {
        (*indices)[index] = index;
    }
    return SXML_STATUS_OK;
}

static sxml_status sort_start(sxml_sort_state *state, const sxml_function_call *call,
                              sxml_sort_operation operation) {
    state->operation = operation;
    state->allocator = call_allocator(call);
    state->item_heap = call->item_heap;
    sxml_status status = sxml_dynamic_context_read_implicit_timezone(
        call->dynamic_context, &state->implicit_timezone_offset_minutes);
    if (status == SXML_STATUS_OK) {
        status = sort_collect_values(state, call);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_indices(state->allocator, state->sorted_value_count,
                                  &state->indices);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sort_uses_comparators(operation)) {
        state->run_width = 1;
        state->middle = state->sorted_value_count < 1 ? state->sorted_value_count : 1;
        state->right_end = state->sorted_value_count < 2 ? state->sorted_value_count : 2;
        state->right_cursor = state->middle;
        return allocate_indices(state->allocator, state->sorted_value_count,
                                &state->scratch_indices);
    }
    status = sort_read_definitions(state, call);
    size_t key_value_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(state->sorted_value_count, state->key_definition_count,
                                    &key_value_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(state->allocator,
                                 (zeroed_allocation){.count = key_value_count,
                                                     .element_byte_count =
                                                         sizeof(sxml_sequence),
                                                     .alignment = alignof(sxml_sequence)},
                                 &memory);
    }
    state->key_values = memory;
    return status;
}

/*
 * fn:highest and fn:lowest compare keys with xs:untypedAtomic cast to
 * xs:double (F&O 4.0, 2.5.10 $modified-key): replaces *key (owned) when it
 * holds such an item.
 */
static sxml_status cast_untyped_keys(const sxml_function_call *call, sxml_sequence *key) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(*key);
    sxml_sequence_builder *builder = NULL;
    sxml_status status = create_builder(call, &builder);
    bool has_untyped = false;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step item_step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &item_step);
        if (status != SXML_STATUS_OK || !item_step.has_item) {
            break;
        }
        if (!sxml_operator_item_has_type(
                call->item_heap,
                (sxml_type_test){.item = item_step.item,
                                 .type = SXML_TYPE_XS_UNTYPED_ATOMIC})) {
            status = sxml_sequence_builder_append_item(builder, item_step.item);
            continue;
        }
        has_untyped = true;
        sxml_item double_item = {.representation = 0};
        status = sxml_operator_cast_item(
            call->item_heap,
            &(sxml_operator_cast){.item = item_step.item,
                                  .target_type = SXML_TYPE_XS_DOUBLE,
                                  .namespace_bindings = {.namespace_bindings = NULL,
                                                         .namespace_binding_count = 0},
                                  .error_record = call->error_record},
            &double_item);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, double_item);
            sxml_item_release(double_item);
        }
    }
    sxml_sequence cast_key = sxml_sequence_empty();
    if (status == SXML_STATUS_OK && has_untyped) {
        status = sxml_sequence_builder_finish(builder, &cast_key);
        if (status == SXML_STATUS_OK) {
            sxml_sequence_release(*key);
            *key = cast_key;
        }
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* Stores the sort key value next_key_value_index (owned: taken). */
static sxml_status sort_store_key(sxml_sort_state *state, const sxml_function_call *call,
                                  sxml_sequence key) {
    sxml_status status = SXML_STATUS_OK;
    if (state->operation == SXML_SORT_OPERATION_HIGHEST ||
        state->operation == SXML_SORT_OPERATION_LOWEST) {
        status = cast_untyped_keys(call, &key);
    }
    state->key_values[state->next_key_value_index] = key;
    state->next_key_value_index += 1;
    return status;
}

/* op:lexicographic-compare of two sort key values (F&O 4.0, 2.5.18): the
   empty sequence first, then item by item. */
typedef struct key_value_pair {
    sxml_sequence first;
    sxml_sequence second;
    const sxml_collation *collation;
} key_value_pair;

/* op:simple-compare of two atomic items (F&O 4.0, 2.5.18): fn:compare
   with the key's collation, XPTY0004 across type families. */
typedef struct key_item_pair {
    sxml_item_pair items;
    const sxml_collation *collation;
} key_item_pair;

static sxml_status compare_key_items(const sxml_sort_state *state, key_item_pair pair,
                                     sxml_ordering *ordering) {
    return sxml_function_library_compare_collated(
        state->item_heap,
        &(sxml_collated_comparison){
            .collation = pair.collation,
            .comparison = {.first = pair.items.first,
                           .second = pair.items.second,
                           .implicit_timezone_offset_minutes =
                               state->implicit_timezone_offset_minutes,
                           .error_record = state->error_record,
                           .name_pool = sxml_item_heap_get_name_pool(state->item_heap)}},
        ordering);
}

static sxml_status compare_key_values(const sxml_sort_state *state, key_value_pair pair,
                                      sxml_ordering *ordering) {
    sxml_sequence_iterator first_iterator = sxml_sequence_iterator_start(pair.first);
    sxml_sequence_iterator second_iterator = sxml_sequence_iterator_start(pair.second);
    for (;;) {
        sxml_iteration_step first_step;
        sxml_iteration_step second_step;
        sxml_status status =
            sxml_sequence_iterator_next(&first_iterator, state->error_record, &first_step);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&second_iterator, state->error_record,
                                                 &second_step);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!first_step.has_item || !second_step.has_item) {
            *ordering = first_step.has_item == second_step.has_item ? SXML_ORDERING_EQUAL
                        : first_step.has_item                       ? SXML_ORDERING_GREATER
                                                                    : SXML_ORDERING_LESS;
            return SXML_STATUS_OK;
        }
        status = compare_key_items(
            state,
            (key_item_pair){
                .items = {.first = first_step.item, .second = second_step.item},
                .collation = pair.collation},
            ordering);
        if (status != SXML_STATUS_OK || *ordering != SXML_ORDERING_EQUAL) {
            return status;
        }
    }
}

/* Orders two sorted values by their keys, major to minor, each in its
   direction (F&O 4.0, 2.5.18). */
static sxml_status compare_key_rows(void *state, sxml_index_pair pair,
                                    sxml_ordering *ordering) {
    const sxml_sort_state *sort = state;
    const size_t definition_count = sort->key_definition_count;
    *ordering = SXML_ORDERING_EQUAL;
    for (size_t definition_index = 0; definition_index < definition_count;
         definition_index += 1) {
        sxml_status status = compare_key_values(
            sort,
            (key_value_pair){
                .first =
                    sort->key_values[pair.first_index * definition_count + definition_index],
                .second =
                    sort->key_values[pair.second_index * definition_count + definition_index],
                .collation = &sort->key_definitions[definition_index].collation},
            ordering);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (*ordering != SXML_ORDERING_EQUAL) {
            if (sort->key_definitions[definition_index].is_descending) {
                *ordering = *ordering == SXML_ORDERING_LESS ? SXML_ORDERING_GREATER
                                                            : SXML_ORDERING_LESS;
            }
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_OK;
}

/* The sorted values in the order of indices: a sequence, or an array for
   the array: functions. */
static sxml_status sort_make_result(const sxml_sort_state *state,
                                    const sxml_function_call *call,
                                    sxml_sequence *result) {
    if (!sort_is_over_array(state->operation)) {
        sxml_sequence_builder *builder = NULL;
        sxml_status status = create_builder(call, &builder);
        for (size_t index = 0; status == SXML_STATUS_OK && index < state->sorted_value_count;
             index += 1) {
            status = sxml_sequence_builder_append_sequence(
                builder, state->sorted_values[state->indices[index]], call->error_record);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_finish(builder, result);
        }
        sxml_sequence_builder_destroy(builder);
        return status;
    }
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        state->allocator,
        (zeroed_allocation){.count = state->sorted_value_count,
                            .element_byte_count = sizeof(sxml_sequence),
                            .alignment = alignof(sxml_sequence)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence *members = memory;
    for (size_t index = 0; index < state->sorted_value_count; index += 1) {
        members[index] = state->sorted_values[state->indices[index]];
    }
    sxml_item array = {.representation = 0};
    status = sxml_array_make(call->item_heap,
                             (sxml_sequence_view){.sequences = members,
                                                  .sequence_count = state->sorted_value_count},
                             &array);
    sxml_allocator_release(state->allocator, members);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(array);
    }
    return status;
}

/* fn:highest and fn:lowest: the items whose key equals the extreme key,
   in input order (F&O 4.0, 2.5.10, 2.5.12). */
static sxml_status select_extreme(sxml_sort_state *state, const sxml_function_call *call,
                                  sxml_sequence *result) {
    const sxml_ordering wanted = state->operation == SXML_SORT_OPERATION_HIGHEST
                                     ? SXML_ORDERING_GREATER
                                     : SXML_ORDERING_LESS;
    size_t best_index = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t value_index = 1;
         status == SXML_STATUS_OK && value_index < state->sorted_value_count;
         value_index += 1) {
        sxml_ordering ordering = SXML_ORDERING_EQUAL;
        status = compare_key_rows(
            state, (sxml_index_pair){.first_index = value_index, .second_index = best_index},
            &ordering);
        if (status == SXML_STATUS_OK && ordering == wanted) {
            best_index = value_index;
        }
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = create_builder(call, &builder);
    }
    for (size_t value_index = 0;
         status == SXML_STATUS_OK && value_index < state->sorted_value_count;
         value_index += 1) {
        sxml_ordering ordering = SXML_ORDERING_LESS;
        status = compare_key_rows(
            state, (sxml_index_pair){.first_index = value_index, .second_index = best_index},
            &ordering);
        if (status == SXML_STATUS_OK && ordering == SXML_ORDERING_EQUAL) {
            status = sxml_sequence_builder_append_sequence(
                builder, state->sorted_values[value_index], call->error_record);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* Computes the remaining sort key values, asking for a call per key
   function; then sorts (or selects) and finishes. */
static sxml_status sort_by_keys(sxml_sort_state *state, const sxml_function_call *call,
                                sxml_native_step *step) {
    const size_t definition_count = state->key_definition_count;
    const size_t key_value_count = state->sorted_value_count * definition_count;
    while (state->next_key_value_index < key_value_count) {
        const size_t value_index = state->next_key_value_index / definition_count;
        const sxml_sort_key_definition *definition =
            &state->key_definitions[state->next_key_value_index % definition_count];
        if (definition->has_key_function) {
            sxml_sequence_retain(state->sorted_values[value_index]);
            state->call_arguments[0] = state->sorted_values[value_index];
            return request_call((requested_call){.function_item = definition->key_function,
                                                 .arguments = state->call_arguments,
                                                 .argument_count = 1},
                                step);
        }
        /* fn:data#1, the default key (F&O 4.0, 2.5.19). */
        sxml_sequence key = sxml_sequence_empty();
        sxml_status status = sxml_evaluator_atomize(
            call->dynamic_context,
            &(sxml_atomization_request){.value = state->sorted_values[value_index],
                                        .error_record = call->error_record},
            &key);
        if (status == SXML_STATUS_OK) {
            status = sort_store_key(state, call, key);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_sequence result = sxml_sequence_empty();
    sxml_status status = SXML_STATUS_OK;
    if (state->operation == SXML_SORT_OPERATION_HIGHEST ||
        state->operation == SXML_SORT_OPERATION_LOWEST) {
        status = select_extreme(state, call, &result);
    } else {
        status = sxml_evaluator_sort_indices(
            state->allocator, &(sxml_index_sort_request){.indices = state->indices,
                                                         .index_count = state->sorted_value_count,
                                                         .compare = compare_key_rows,
                                                         .compare_state = state});
        if (status == SXML_STATUS_OK) {
            status = sort_make_result(state, call, &result);
        }
    }
    return status == SXML_STATUS_OK ? finish_with(result, step) : status;
}

/* Starts merging the two runs at left_start. */
static void merge_set_runs(sxml_sort_state *state) {
    const size_t count = state->sorted_value_count;
    const size_t middle = state->left_start + state->run_width;
    const size_t right_end = middle + state->run_width;
    state->middle = middle < count ? middle : count;
    state->right_end = right_end < count ? right_end : count;
    state->left_cursor = state->left_start;
    state->right_cursor = state->middle;
    state->output_cursor = state->left_start;
}

/* Moves the head of the left or the right run to the output. */
static void merge_take_left(sxml_sort_state *state) {
    state->scratch_indices[state->output_cursor] = state->indices[state->left_cursor];
    state->output_cursor += 1;
    state->left_cursor += 1;
}

static void merge_take_right(sxml_sort_state *state) {
    state->scratch_indices[state->output_cursor] = state->indices[state->right_cursor];
    state->output_cursor += 1;
    state->right_cursor += 1;
}

/* Asks for comparator comparator_index on the two run heads. */
static sxml_status merge_request_comparison(sxml_sort_state *state,
                                            const sxml_function_call *call,
                                            sxml_native_step *step) {
    sxml_item comparator = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        call_argument(call, 1),
        (sxml_item_request){.item_index = state->comparator_index,
                            .error_record = call->error_record},
        &comparator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    release_call_arguments(state->call_arguments, 2);
    state->call_arguments[0] = state->sorted_values[state->indices[state->left_cursor]];
    state->call_arguments[1] = state->sorted_values[state->indices[state->right_cursor]];
    sxml_sequence_retain(state->call_arguments[0]);
    sxml_sequence_retain(state->call_arguments[1]);
    return request_call((requested_call){.function_item = comparator,
                                         .arguments = state->call_arguments,
                                         .argument_count = 2},
                        step);
}

/* A comparator's xs:integer: which run head goes first (F&O 4.0,
   2.5.20); equal under the last comparator keeps input order. */
typedef struct comparison_outcome {
    /* The comparator's value (owned: taken). */
    sxml_sequence comparison;
    /* Set when the next comparator must decide. */
    bool needs_next;
} comparison_outcome;

static sxml_status merge_take_comparison(sxml_sort_state *state,
                                         const sxml_function_call *call,
                                         comparison_outcome *outcome) {
    const sxml_sequence comparison = outcome->comparison;
    bool *needs_next = &outcome->needs_next;
    int64_t sign = 0;
    sxml_status status = SXML_STATUS_OK;
    if (sxml_sequence_get_representation(comparison) ==
        SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        status = read_integer(comparison.content, &sign);
    } else {
        status = sxml_higher_order_report_error(call->error_record,
                                                SXML_HIGHER_ORDER_ERROR_WRONG_TYPE);
    }
    sxml_sequence_release(comparison);
    *needs_next = false;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sign == 0) {
        uint64_t comparator_count = 0;
        status = sxml_sequence_count(call_argument(call, 1), call->error_record,
                                     &comparator_count);
        if (status == SXML_STATUS_OK && state->comparator_index + 1 < comparator_count) {
            state->comparator_index += 1;
            *needs_next = true;
            return SXML_STATUS_OK;
        }
    }
    if (sign <= 0) {
        merge_take_left(state);
    } else {
        merge_take_right(state);
    }
    return status;
}

/* fn:sort-with's bottom-up merge sort, one comparison per call. */
static sxml_status sort_with_comparators(sxml_sort_state *state,
                                         const sxml_function_call *call,
                                         sxml_native_step *step) {
    const size_t count = state->sorted_value_count;
    for (;;) {
        if (state->run_width >= count) {
            sxml_sequence result = sxml_sequence_empty();
            const sxml_status status = sort_make_result(state, call, &result);
            return status == SXML_STATUS_OK ? finish_with(result, step) : status;
        }
        if (state->left_cursor < state->middle && state->right_cursor < state->right_end) {
            state->comparator_index = 0;
            return merge_request_comparison(state, call, step);
        }
        while (state->left_cursor < state->middle) {
            merge_take_left(state);
        }
        while (state->right_cursor < state->right_end) {
            merge_take_right(state);
        }
        state->left_start += 2 * state->run_width;
        if (state->left_start >= count) {
            size_t *swapped = state->indices;
            state->indices = state->scratch_indices;
            state->scratch_indices = swapped;
            state->run_width *= 2;
            state->left_start = 0;
        }
        merge_set_runs(state);
    }
}

sxml_status sxml_sort_step(const sxml_native_step_request *request,
                           sxml_native_step *step, sxml_sort_operation operation) {
    sxml_sort_state *state = request->state;
    const sxml_function_call *call = request->call;
    state->error_record = call->error_record;
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        status = sort_start(state, call, operation);
    } else if (sort_uses_comparators(operation)) {
        comparison_outcome outcome = {.comparison = request->call_result,
                                      .needs_next = false};
        status = merge_take_comparison(state, call, &outcome);
        if (status == SXML_STATUS_OK && outcome.needs_next) {
            return merge_request_comparison(state, call, step);
        }
    } else {
        release_call_arguments(state->call_arguments, 2);
        /* fn:sort-by's key functions are not coerced by the signature
           (map(*)*, D80): their values are atomized here. */
        sxml_sequence key = sxml_sequence_empty();
        status = sxml_evaluator_atomize(
            call->dynamic_context,
            &(sxml_atomization_request){.value = request->call_result,
                                        .error_record = call->error_record},
            &key);
        sxml_sequence_release(request->call_result);
        if (status == SXML_STATUS_OK) {
            status = sort_store_key(state, call, key);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sort_uses_comparators(operation)) {
        return sort_with_comparators(state, call, step);
    }
    return sort_by_keys(state, call, step);
}

/* ------------------------------------------------- Functions on functions */

/* The prefix sxml gives a function name it reports (F&O 4.0, 13.2: the
   prefix is implementation-dependent): the conventional one of the
   well-known namespaces, none otherwise. */
static sxml_status conventional_prefix(sxml_name_pool *name_pool,
                                       sxml_namespace_uri_identifier namespace_uri,
                                       sxml_namespace_prefix_identifier *prefix) {
    static const struct {
        sxml_well_known_namespace namespace_name;
        sxml_byte_view prefix_text;
    } conventions[] = {
        {SXML_NAMESPACE_FUNCTIONS, SXML_BYTE_VIEW_INITIALIZER("fn")},
        {SXML_NAMESPACE_MAP_FUNCTIONS, SXML_BYTE_VIEW_INITIALIZER("map")},
        {SXML_NAMESPACE_ARRAY_FUNCTIONS, SXML_BYTE_VIEW_INITIALIZER("array")},
        {SXML_NAMESPACE_MATH_FUNCTIONS, SXML_BYTE_VIEW_INITIALIZER("math")},
        {SXML_NAMESPACE_XML_SCHEMA, SXML_BYTE_VIEW_INITIALIZER("xs")},
    };
    *prefix = (sxml_namespace_prefix_identifier){.index = 0};
    for (size_t convention_index = 0;
         convention_index < sizeof(conventions) / sizeof(conventions[0]);
         convention_index += 1) {
        if (namespace_uri.index == (uint32_t)conventions[convention_index].namespace_name) {
            return sxml_name_pool_intern_namespace_prefix(
                name_pool, conventions[convention_index].prefix_text, prefix);
        }
    }
    return SXML_STATUS_OK;
}

/* An xs:QName item for name, owned. */
static sxml_status make_qname(sxml_item_heap *item_heap, sxml_name_identifier name,
                              sxml_item *item) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    sxml_name_parts parts;
    sxml_status status = sxml_name_pool_get_name_parts(name_pool, name, &parts);
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = conventional_prefix(name_pool, parts.namespace_uri, &prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = name,
                                                  .namespace_prefix = prefix}}}},
        item);
}

/* fn:function-name($function) (F&O 4.0, 13.2). */
static sxml_status call_function_name(const sxml_function_call *call,
                                      sxml_sequence *result) {
    sxml_function_item_description description;
    sxml_status status =
        sxml_function_item_describe(call_argument(call, 0).content, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.function_name.index == 0) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_item name = {.representation = 0};
    status = make_qname(call->item_heap, description.function_name, &name);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(name);
    }
    return status;
}

/* fn:function-arity($function) (F&O 4.0, 13.3). */
static sxml_status call_function_arity(const sxml_function_call *call,
                                       sxml_sequence *result) {
    sxml_function_item_description description;
    const sxml_status status =
        sxml_function_item_describe(call_argument(call, 0).content, &description);
    if (status == SXML_STATUS_OK) {
        *result = position_sequence(description.arity);
    }
    return status;
}

/* fn:function-annotations($function) (F&O 4.0, 13.4): one single-entry
   map per annotation, in order. */
static sxml_status call_function_annotations(const sxml_function_call *call,
                                             sxml_sequence *result) {
    sxml_function_item_description description;
    sxml_status status =
        sxml_function_item_describe(call_argument(call, 0).content, &description);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = create_builder(call, &builder);
    }
    const sxml_function_annotation_view annotations = description.function_annotations;
    for (size_t annotation_index = 0;
         status == SXML_STATUS_OK &&
         annotation_index < annotations.function_annotation_count;
         annotation_index += 1) {
        const sxml_function_annotation *annotation =
            &annotations.function_annotations[annotation_index];
        sxml_item name = {.representation = 0};
        sxml_sequence values = sxml_sequence_empty();
        status = make_qname(call->item_heap, annotation->annotation_name, &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_make_vector(call->item_heap,
                                               annotation->annotation_values, &values);
        }
        sxml_item map = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_map_make(
                call->item_heap,
                &(sxml_map_request){
                    .map_entries = {.map_entries = &(sxml_map_entry){.key = name,
                                                                     .value = values},
                                    .map_entry_count = 1},
                    .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                    .error_record = call->error_record},
                &map);
        }
        sxml_item_release(name);
        sxml_sequence_release(values);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, map);
            sxml_item_release(map);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* True when the row at this arity needs the focus: fn:position, fn:last,
   or a context-value parameter left out (F&O 4.0, 1.5). */
static bool entry_needs_focus(const sxml_function_entry *entry, size_t arity) {
    const sxml_native_function *row = entry->native_function;
    if ((row->properties & (SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION |
                            SXML_NATIVE_FUNCTION_READS_CONTEXT_SIZE)) != 0) {
        return true;
    }
    for (size_t parameter_index = arity;
         parameter_index < row->parameters.function_parameter_count;
         parameter_index += 1) {
        if (row->parameters.function_parameters[parameter_index].parameter_default ==
            SXML_PARAMETER_DEFAULT_CONTEXT_VALUE) {
            return true;
        }
    }
    return false;
}

/* The caller's focus a static call of fn:function-lookup passes after its
   two arguments (glossary `caller_focus`): context value, position and
   size, borrowed; has_focus is false without them. */
typedef struct caller_focus {
    bool has_focus;
    sxml_sequence parts[3];
} caller_focus;

/* The function item for an entry, as a named function reference makes it
   (XPath 4.0, 4.5.4): a native row's body or an xs: constructor body; a
   row that needs the focus is bound to the caller's focus. */
typedef struct found_function {
    const sxml_function_query *query;
    const sxml_function_entry *entry;
    const caller_focus *focus;
} found_function;

/* The body of fn:position#0 or fn:last#0 bound to a focus (F&O 4.0,
   13.1): the one value its function item captured. */
static sxml_status call_captured_value(const sxml_function_call *call, sxml_sequence *result) {
    sxml_function_item_description description;
    const sxml_status status = sxml_function_item_describe(call->function_item, &description);
    if (status != SXML_STATUS_OK || description.captured_values.sequence_count != 1) {
        return status != SXML_STATUS_OK ? status : SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_sequence_retain(description.captured_values.sequences[0]);
    *result = description.captured_values.sequences[0];
    return SXML_STATUS_OK;
}

/* The row of those bodies; no library part lists it, so no name finds
   it. */
static const sxml_native_function captured_value_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("function-lookup"),
    .parameters = {NULL, 0},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = call_captured_value,
    .resumable_body = NULL,
};

static sxml_status make_plain_entry_function(const sxml_function_call *call,
                                             found_function found, sxml_sequence *result);

/*
 * A row that needs the focus, bound to the caller's (F&O 4.0, 13.1: the
 * function is context-dependent, its context that of the fn:function-
 * lookup call): fn:position#0 and fn:last#0 return the captured position
 * or size; a function whose next parameter defaults to the context value
 * is the next arity partially applied to the captured context value
 * (XPath 4.0, 4.5.4), keeping its name.
 */
/* The function full of arity full_query->arity with its last parameter
   bound to the caller's context value: what fn:function-lookup returns
   for query, of one arity less (F&O 4.0, 13.1; the context value is
   captured when the function is found, as for a named function
   reference, XPath 4.0, 4.5.4). */
typedef struct context_binding {
    const sxml_function_query *query;
    const sxml_function_query *full_query;
    const sxml_function_entry *full;
    /* Borrowed. */
    sxml_sequence context_value;
} context_binding;

static sxml_status bind_context_argument(const sxml_function_call *call,
                                         const context_binding *request,
                                         sxml_sequence *result) {
    const sxml_function_query *query = request->query;
    const size_t bound_index = query->arity;
    sxml_sequence full_function = sxml_sequence_empty();
    sxml_status status = make_plain_entry_function(
        call, (found_function){.query = request->full_query, .entry = request->full, .focus = NULL},
        &full_function);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_call_signature signature;
    partial_binding binding = {.allocator = call_allocator(call),
                               .parameter_count = bound_index + 1};
    status = sxml_evaluator_describe_call_signature(call->dynamic_context,
                                                    full_function.content, &signature);
    if (status == SXML_STATUS_OK && (!signature.has_function_type ||
                                     signature.parameter_types.sequence_type_count <=
                                         bound_index)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_partial_binding(&binding);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_coerce(
            call->dynamic_context,
            &(sxml_coercion_request){
                .value = request->context_value,
                .sequence_type = signature.parameter_types.sequence_types[bound_index],
                .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
                .error_record = call->error_record},
            &binding.bound_values[bound_index]);
        binding.is_bound[bound_index] = status == SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        status = make_partial_application(
            call,
            &(partial_application_parts){.function_item = full_function.content,
                                         .function_name = query->function_name,
                                         .signature = &signature,
                                         .binding = &binding},
            result);
    }
    release_partial_binding(&binding);
    sxml_sequence_release(full_function);
    return status;
}

/* fn:string(value) by the fn:string row's own body, a leaf (no
   evaluator call; D71), with value coerced to its parameter type. */
static sxml_status make_string_value(const sxml_function_call *call, sxml_sequence value,
                                     sxml_sequence *result) {
    sxml_name_identifier string_name = {0};
    sxml_status status = sxml_name_pool_intern_name(
        sxml_item_heap_get_name_pool(call->item_heap),
        &(sxml_name_request){
            .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS),
            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string")},
        &string_name);
    const sxml_function_table table = call->dynamic_context->function_table;
    sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    if (status == SXML_STATUS_OK) {
        status = table.operations->find_function(
            table.state, &(sxml_function_query){.function_name = string_name, .arity = 1},
            &entry);
    }
    if (status == SXML_STATUS_OK && (entry.entry_kind != SXML_FUNCTION_ENTRY_KIND_NATIVE ||
                                     entry.native_function->call == NULL ||
                                     entry.parameter_types.sequence_type_count < 1)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_sequence coerced = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_coerce(
            call->dynamic_context,
            &(sxml_coercion_request){.value = value,
                                     .sequence_type = entry.parameter_types.sequence_types[0],
                                     .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
                                     .error_record = call->error_record},
            &coerced);
    }
    if (status == SXML_STATUS_OK) {
        sxml_function_call string_call = *call;
        string_call.function_item = (sxml_item){.representation = 0};
        string_call.arguments = (sxml_sequence_view){.sequences = &coerced, .sequence_count = 1};
        status = entry.native_function->call(&string_call, result);
    }
    sxml_sequence_release(coerced);
    return status;
}

static sxml_status bind_to_caller_focus(const sxml_function_call *call, found_function found,
                                        sxml_sequence *result) {
    const sxml_function_query *query = found.query;
    const sxml_native_function *row = found.entry->native_function;
    const sxml_sequence *parts = found.focus->parts;
    if ((row->properties & (SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION |
                            SXML_NATIVE_FUNCTION_READS_CONTEXT_SIZE)) != 0) {
        const sxml_sequence captured =
            parts[(row->properties & SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION) != 0 ? 1 : 2];
        sxml_item item = {.representation = 0};
        const sxml_status status = sxml_function_item_make(
            call->item_heap,
            &(sxml_function_item_request){
                .function_name = query->function_name,
                .signature = found.entry->signature,
                .function_annotations = {.function_annotations = NULL,
                                         .function_annotation_count = 0},
                .captured_values = {.sequences = &captured, .sequence_count = 1},
                .body = {.state = (void *)&captured_value_row,
                         .operations = &sxml_native_function_body_operations}},
            &item);
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(item);
        }
        return status;
    }
    const size_t bound_index = query->arity;
    if (bound_index >= row->parameters.function_parameter_count ||
        row->parameters.function_parameters[bound_index].parameter_default !=
            SXML_PARAMETER_DEFAULT_CONTEXT_VALUE) {
        return sxml_error_record_report(call->error_record, &lookup_focus_error);
    }
    const sxml_function_table table = call->dynamic_context->function_table;
    const sxml_function_query full_query = {.function_name = query->function_name,
                                            .arity = bound_index + 1};
    sxml_function_entry full = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    const sxml_status status = table.operations->find_function(table.state, &full_query, &full);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (full.entry_kind != SXML_FUNCTION_ENTRY_KIND_NATIVE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (!row->parameters.function_parameters[bound_index].defaults_to_string_value) {
        return bind_context_argument(
            call,
            &(context_binding){.query = query, .full_query = &full_query, .full = &full,
                               .context_value = parts[0]},
            result);
    }
    /* "$value := fn:string(.)": the string value of the caller's context
       value is bound (F&O 4.0, fn:string-length, fn:normalize-space). */
    sxml_sequence string_value = sxml_sequence_empty();
    sxml_status bound = make_string_value(call, parts[0], &string_value);
    if (bound == SXML_STATUS_OK) {
        bound = bind_context_argument(
            call,
            &(context_binding){.query = query, .full_query = &full_query, .full = &full,
                               .context_value = string_value},
            result);
    }
    sxml_sequence_release(string_value);
    return bound;
}

static sxml_status make_entry_function(const sxml_function_call *call, found_function found,
                                       sxml_sequence *result) {
    if (found.entry->entry_kind == SXML_FUNCTION_ENTRY_KIND_NATIVE &&
        entry_needs_focus(found.entry, found.query->arity)) {
        if (found.focus == NULL || !found.focus->has_focus) {
            return sxml_error_record_report(call->error_record, &lookup_focus_error);
        }
        return bind_to_caller_focus(call, found, result);
    }
    return make_plain_entry_function(call, found, result);
}

static sxml_status make_plain_entry_function(const sxml_function_call *call,
                                             found_function found, sxml_sequence *result) {
    const sxml_function_query *query = found.query;
    const sxml_function_entry *entry = found.entry;
    sxml_sequence constructor_type = sxml_sequence_empty();
    sxml_function_item_request request = {
        .function_name = query->function_name,
        .signature = entry->signature,
        .function_annotations = {.function_annotations = NULL,
                                 .function_annotation_count = 0},
        .captured_values = {.sequences = NULL, .sequence_count = 0},
        .body = {.state = (void *)entry->native_function,
                 .operations = &sxml_native_function_body_operations}};
    if (entry->entry_kind == SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR) {
        /* As src/evaluator_function.c makes xs:T#1: the target type's
           identifier captured as an xs:integer. */
        constructor_type = position_sequence(entry->constructor_type.index);
        request.captured_values =
            (sxml_sequence_view){.sequences = &constructor_type, .sequence_count = 1};
        request.body = (sxml_function_body){
            .state = NULL, .operations = &sxml_evaluator_constructor_body_operations};
    }
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_function_item_make(call->item_heap, &request, &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* A constructor function looked up with arity 0: found with arity 1
   (through the table, then the in-scope schema types), and bound to the
   caller's focus; the empty sequence when there is none, XPDY0002 when
   the lookup has no focus. */
typedef struct constructor_lookup {
    const sxml_function_query *query;
    const caller_focus *focus;
} constructor_lookup;

static sxml_status find_focused_constructor(const sxml_function_call *call,
                                            const constructor_lookup *lookup,
                                            sxml_sequence *result) {
    const sxml_function_query full_query = {.function_name = lookup->query->function_name,
                                            .arity = 1};
    const sxml_function_table table = call->dynamic_context->function_table;
    sxml_function_entry full = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    sxml_status status = table.operations->find_function(table.state, &full_query, &full);
    const sxml_ir_static_context *static_context =
        sxml_dynamic_context_get_static_context(call->dynamic_context);
    if (status == SXML_STATUS_OK && full.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE &&
        static_context != NULL) {
        status = sxml_schema_scope_find_constructor(
            &(sxml_schema_scope_constructor_request){
                .schemas = static_context->in_scope_schemas,
                .type_registry = sxml_item_heap_get_type_registry(call->item_heap),
                .query = &full_query},
            &full);
    }
    if (status != SXML_STATUS_OK || full.entry_kind != SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR) {
        return status;
    }
    if (!lookup->focus->has_focus) {
        return sxml_error_record_report(call->error_record, &lookup_focus_error);
    }
    return bind_context_argument(call,
                                 &(context_binding){.query = lookup->query,
                                                    .full_query = &full_query,
                                                    .full = &full,
                                                    .context_value = lookup->focus->parts[0]},
                                 result);
}

/* fn:function-lookup($name, $arity) (F&O 4.0, 13.1): through the
   dynamic context's function table, the one static analysis used. */
static sxml_status call_function_lookup(const sxml_function_call *call,
                                        sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *result = sxml_sequence_empty();
    sxml_atomic_datum name;
    int64_t arity = 0;
    sxml_status status = sxml_atomic_item_get_datum(call_argument(call, 0).content, &name);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call_argument(call, 1).content, &arity);
    }
    const sxml_function_table table = call->dynamic_context->function_table;
    if (status != SXML_STATUS_OK || arity < 0 || (uint64_t)arity > SIZE_MAX ||
        table.operations == NULL) {
        return status;
    }
    const sxml_function_query query = {
        .function_name = name.datum_details.qname.expanded_name, .arity = (size_t)arity};
    /* The caller's focus, when a static call passed it. */
    caller_focus focus = {.has_focus = call->arguments.sequence_count == 5};
    for (size_t part_index = 0; focus.has_focus && part_index < 3; part_index += 1) {
        focus.parts[part_index] = call_argument(call, 2 + part_index);
    }
    /* The calling unit's own functions first: XQuery's declared
       functions, XSLT's stylesheet functions. */
    sxml_item found = {.representation = 0};
    status = sxml_evaluator_find_unit_function(
        call->dynamic_context,
        (sxml_unit_function_query){.function_name = query.function_name,
                                   .arity = query.arity,
                                   .caller_focus = focus.has_focus ? focus.parts : NULL},
        &found);
    if (status != SXML_STATUS_OK || found.representation != 0) {
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(found);
        }
        return status;
    }
    sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    status = table.operations->find_function(table.state, &query, &entry);
    const sxml_ir_static_context *static_context =
        sxml_dynamic_context_get_static_context(call->dynamic_context);
    if (status == SXML_STATUS_OK && entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE &&
        static_context != NULL) {
        /* The constructor functions of the in-scope schema types, as
           static analysis finds them (D174). */
        status = sxml_schema_scope_find_constructor(
            &(sxml_schema_scope_constructor_request){
                .schemas = static_context->in_scope_schemas,
                .type_registry = sxml_item_heap_get_type_registry(call->item_heap),
                .query = &query},
            &entry);
    }
    if (status == SXML_STATUS_OK && entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE &&
        query.arity == 0) {
        /* xs:T#0, $value := . (XPath 4.0, 4.5.1.3): the constructor of
           arity 1 bound to the caller's context value. */
        return find_focused_constructor(call, &(constructor_lookup){.query = &query,
                                                                    .focus = &focus},
                                        result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (entry.entry_kind) {
    case SXML_FUNCTION_ENTRY_KIND_NONE:
        return SXML_STATUS_OK;
    case SXML_FUNCTION_ENTRY_KIND_NATIVE:
    case SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR: {
        return make_entry_function(
            call, (found_function){.query = &query, .entry = &entry, .focus = &focus}, result);
    }
    case SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION:
        /* Declared functions arrive with XQuery (Phase 3). */
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    case SXML_FUNCTION_ENTRY_KIND_FUNCTION_ITEM:
        /* The item the table binds (D196). */
        sxml_item_retain(entry.function_item);
        *result = sxml_sequence_from_item(entry.function_item);
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------------ fn:op */

/* The operators of fn:op (F&O 4.0, 18.4). */
typedef enum operator_code {
    SXML_OPERATOR_FUNCTION_COMMA,
    SXML_OPERATOR_FUNCTION_AND,
    SXML_OPERATOR_FUNCTION_OR,
    SXML_OPERATOR_FUNCTION_ADD,
    SXML_OPERATOR_FUNCTION_SUBTRACT,
    SXML_OPERATOR_FUNCTION_MULTIPLY,
    SXML_OPERATOR_FUNCTION_DIVIDE,
    SXML_OPERATOR_FUNCTION_INTEGER_DIVIDE,
    SXML_OPERATOR_FUNCTION_MODULO,
    SXML_OPERATOR_FUNCTION_GENERAL_EQUAL,
    SXML_OPERATOR_FUNCTION_GENERAL_LESS,
    SXML_OPERATOR_FUNCTION_GENERAL_LESS_OR_EQUAL,
    SXML_OPERATOR_FUNCTION_GENERAL_GREATER,
    SXML_OPERATOR_FUNCTION_GENERAL_GREATER_OR_EQUAL,
    SXML_OPERATOR_FUNCTION_GENERAL_NOT_EQUAL,
    SXML_OPERATOR_FUNCTION_VALUE_EQUAL,
    SXML_OPERATOR_FUNCTION_VALUE_LESS,
    SXML_OPERATOR_FUNCTION_VALUE_LESS_OR_EQUAL,
    SXML_OPERATOR_FUNCTION_VALUE_GREATER,
    SXML_OPERATOR_FUNCTION_VALUE_GREATER_OR_EQUAL,
    SXML_OPERATOR_FUNCTION_VALUE_NOT_EQUAL,
    SXML_OPERATOR_FUNCTION_PRECEDES,
    SXML_OPERATOR_FUNCTION_FOLLOWS,
    SXML_OPERATOR_FUNCTION_PRECEDES_OR_IS,
    SXML_OPERATOR_FUNCTION_FOLLOWS_OR_IS,
    SXML_OPERATOR_FUNCTION_IS,
    SXML_OPERATOR_FUNCTION_IS_NOT,
    SXML_OPERATOR_FUNCTION_CONCATENATE,
    SXML_OPERATOR_FUNCTION_UNION,
    SXML_OPERATOR_FUNCTION_EXCEPT,
    SXML_OPERATOR_FUNCTION_INTERSECT,
    SXML_OPERATOR_FUNCTION_RANGE,
    SXML_OPERATOR_FUNCTION_OTHERWISE,
} operator_code;

static const struct {
    sxml_byte_view operator_text;
    operator_code code;
} operator_names[] = {
    {SXML_BYTE_VIEW_INITIALIZER(","), SXML_OPERATOR_FUNCTION_COMMA},
    {SXML_BYTE_VIEW_INITIALIZER("and"), SXML_OPERATOR_FUNCTION_AND},
    {SXML_BYTE_VIEW_INITIALIZER("or"), SXML_OPERATOR_FUNCTION_OR},
    {SXML_BYTE_VIEW_INITIALIZER("+"), SXML_OPERATOR_FUNCTION_ADD},
    {SXML_BYTE_VIEW_INITIALIZER("-"), SXML_OPERATOR_FUNCTION_SUBTRACT},
    {SXML_BYTE_VIEW_INITIALIZER("*"), SXML_OPERATOR_FUNCTION_MULTIPLY},
    {SXML_BYTE_VIEW_INITIALIZER("div"), SXML_OPERATOR_FUNCTION_DIVIDE},
    {SXML_BYTE_VIEW_INITIALIZER("idiv"), SXML_OPERATOR_FUNCTION_INTEGER_DIVIDE},
    {SXML_BYTE_VIEW_INITIALIZER("mod"), SXML_OPERATOR_FUNCTION_MODULO},
    {SXML_BYTE_VIEW_INITIALIZER("="), SXML_OPERATOR_FUNCTION_GENERAL_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("<"), SXML_OPERATOR_FUNCTION_GENERAL_LESS},
    {SXML_BYTE_VIEW_INITIALIZER("<="), SXML_OPERATOR_FUNCTION_GENERAL_LESS_OR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER(">"), SXML_OPERATOR_FUNCTION_GENERAL_GREATER},
    {SXML_BYTE_VIEW_INITIALIZER(">="), SXML_OPERATOR_FUNCTION_GENERAL_GREATER_OR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("!="), SXML_OPERATOR_FUNCTION_GENERAL_NOT_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("eq"), SXML_OPERATOR_FUNCTION_VALUE_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("lt"), SXML_OPERATOR_FUNCTION_VALUE_LESS},
    {SXML_BYTE_VIEW_INITIALIZER("le"), SXML_OPERATOR_FUNCTION_VALUE_LESS_OR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("gt"), SXML_OPERATOR_FUNCTION_VALUE_GREATER},
    {SXML_BYTE_VIEW_INITIALIZER("ge"), SXML_OPERATOR_FUNCTION_VALUE_GREATER_OR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("ne"), SXML_OPERATOR_FUNCTION_VALUE_NOT_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("<<"), SXML_OPERATOR_FUNCTION_PRECEDES},
    {SXML_BYTE_VIEW_INITIALIZER(">>"), SXML_OPERATOR_FUNCTION_FOLLOWS},
    {SXML_BYTE_VIEW_INITIALIZER("precedes"), SXML_OPERATOR_FUNCTION_PRECEDES},
    {SXML_BYTE_VIEW_INITIALIZER("follows"), SXML_OPERATOR_FUNCTION_FOLLOWS},
    {SXML_BYTE_VIEW_INITIALIZER("precedes-or-is"), SXML_OPERATOR_FUNCTION_PRECEDES_OR_IS},
    {SXML_BYTE_VIEW_INITIALIZER("follows-or-is"), SXML_OPERATOR_FUNCTION_FOLLOWS_OR_IS},
    {SXML_BYTE_VIEW_INITIALIZER("is"), SXML_OPERATOR_FUNCTION_IS},
    {SXML_BYTE_VIEW_INITIALIZER("is-not"), SXML_OPERATOR_FUNCTION_IS_NOT},
    {SXML_BYTE_VIEW_INITIALIZER("||"), SXML_OPERATOR_FUNCTION_CONCATENATE},
    {SXML_BYTE_VIEW_INITIALIZER("|"), SXML_OPERATOR_FUNCTION_UNION},
    {SXML_BYTE_VIEW_INITIALIZER("union"), SXML_OPERATOR_FUNCTION_UNION},
    {SXML_BYTE_VIEW_INITIALIZER("except"), SXML_OPERATOR_FUNCTION_EXCEPT},
    {SXML_BYTE_VIEW_INITIALIZER("intersect"), SXML_OPERATOR_FUNCTION_INTERSECT},
    {SXML_BYTE_VIEW_INITIALIZER("to"), SXML_OPERATOR_FUNCTION_RANGE},
    {SXML_BYTE_VIEW_INITIALIZER("otherwise"), SXML_OPERATOR_FUNCTION_OTHERWISE},
};

/* The two operands of an operator function's call, atomized. */
typedef struct atomized_operands {
    sxml_sequence left_operand;
    sxml_sequence right_operand;
} atomized_operands;

static void release_operands(atomized_operands *operands) {
    sxml_sequence_release(operands->left_operand);
    sxml_sequence_release(operands->right_operand);
}

static sxml_status atomize_operands(const sxml_function_call *call,
                                    atomized_operands *operands) {
    *operands = (atomized_operands){.left_operand = sxml_sequence_empty(),
                                    .right_operand = sxml_sequence_empty()};
    sxml_status status = sxml_evaluator_atomize(
        call->dynamic_context,
        &(sxml_atomization_request){.value = call_argument(call, 0),
                                    .error_record = call->error_record},
        &operands->left_operand);
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_atomize(
            call->dynamic_context,
            &(sxml_atomization_request){.value = call_argument(call, 1),
                                        .error_record = call->error_record},
            &operands->right_operand);
    }
    if (status != SXML_STATUS_OK) {
        release_operands(operands);
    }
    return status;
}

/* The string of an atomized operand of "||": "" for the empty sequence
   (XPath 4.0, 4.10: fn:concat); item holds the string (owned, or not an
   item). */
typedef struct operand_string {
    sxml_item item;
    sxml_string string;
} operand_string;

static sxml_status operand_as_string(const sxml_function_call *call, sxml_sequence operand,
                                     operand_string *converted) {
    sxml_item *item = &converted->item;
    sxml_string *string = &converted->string;
    *item = (sxml_item){.representation = 0};
    *string = (sxml_string){.utf8 = {.bytes = NULL, .byte_count = 0},
                            .codepoint_count = 0, .is_ascii = true};
    sxml_single_operand single;
    sxml_status status =
        sxml_operator_read_single_operand(operand, call->error_record, &single);
    if (status != SXML_STATUS_OK || !single.has_item) {
        return status;
    }
    status = sxml_operator_cast_item(
        call->item_heap,
        &(sxml_operator_cast){.item = single.item,
                              .target_type = SXML_TYPE_XS_STRING,
                              .namespace_bindings = {.namespace_bindings = NULL,
                                                     .namespace_binding_count = 0},
                              .error_record = call->error_record},
        item);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(*item, &datum);
    }
    if (status == SXML_STATUS_OK) {
        *string = datum.datum_details.string;
    }
    return status;
}

/* a || b (XPath 4.0, 4.10). */
static sxml_status concatenate_operands(const sxml_function_call *call,
                                        const atomized_operands *operands,
                                        sxml_sequence *result) {
    operand_string converted[2] = {{.item = {.representation = 0}},
                                   {.item = {.representation = 0}}};
    sxml_status status = operand_as_string(call, operands->left_operand, &converted[0]);
    if (status == SXML_STATUS_OK) {
        status = operand_as_string(call, operands->right_operand, &converted[1]);
    }
    const sxml_string strings[2] = {converted[0].string, converted[1].string};
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(strings[0].utf8.byte_count, strings[1].utf8.byte_count,
                               &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            call_allocator(call),
            (sxml_allocation_request){.byte_count = byte_count == 0 ? 1 : byte_count,
                                      .alignment = 1},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        uint8_t *bytes = memory;
        if (strings[0].utf8.byte_count > 0) {
            memcpy(bytes, strings[0].utf8.bytes, strings[0].utf8.byte_count);
        }
        if (strings[1].utf8.byte_count > 0) {
            memcpy(bytes + strings[0].utf8.byte_count, strings[1].utf8.bytes,
                   strings[1].utf8.byte_count);
        }
        sxml_item joined = {.representation = 0};
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {
                              .string = {.utf8 = {.bytes = bytes, .byte_count = byte_count},
                                         .codepoint_count = strings[0].codepoint_count +
                                                            strings[1].codepoint_count,
                                         .is_ascii = strings[0].is_ascii &&
                                                     strings[1].is_ascii}}}},
            &joined);
        sxml_allocator_release(call_allocator(call), memory);
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(joined);
        }
    }
    sxml_item_release(converted[0].item);
    sxml_item_release(converted[1].item);
    return status;
}

/* a to b (XPath 4.0, 4.8.2): bounds within int64_t. */
static sxml_status range_of_operands(const sxml_function_call *call,
                                     const atomized_operands *operands,
                                     sxml_sequence *result) {
    sxml_single_operand bounds[2];
    sxml_status status = sxml_operator_read_single_operand(
        operands->left_operand, call->error_record, &bounds[0]);
    if (status == SXML_STATUS_OK) {
        status = sxml_operator_read_single_operand(operands->right_operand,
                                                   call->error_record, &bounds[1]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!bounds[0].has_item || !bounds[1].has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    int64_t integers[2] = {0, 0};
    for (size_t bound_index = 0; bound_index < 2; bound_index += 1) {
        sxml_item integer_item = {.representation = 0};
        status = sxml_operator_cast_item(
            call->item_heap,
            &(sxml_operator_cast){.item = bounds[bound_index].item,
                                  .target_type = SXML_TYPE_XS_INTEGER,
                                  .namespace_bindings = {.namespace_bindings = NULL,
                                                         .namespace_binding_count = 0},
                                  .error_record = call->error_record},
            &integer_item);
        if (status == SXML_STATUS_OK) {
            status = read_integer(integer_item, &integers[bound_index]);
            sxml_item_release(integer_item);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return sxml_sequence_make_integer_range(
        call->item_heap,
        (sxml_integer_range){.first_integer = integers[0], .last_integer = integers[1]},
        result);
}

static sxml_status apply_comparison(const sxml_function_call *call, operator_code code,
                                    sxml_sequence *result) {
    static const sxml_comparison_operator comparison_operators[] = {
        SXML_COMPARISON_OPERATOR_EQUAL,         SXML_COMPARISON_OPERATOR_LESS,
        SXML_COMPARISON_OPERATOR_LESS_OR_EQUAL, SXML_COMPARISON_OPERATOR_GREATER,
        SXML_COMPARISON_OPERATOR_GREATER_OR_EQUAL, SXML_COMPARISON_OPERATOR_NOT_EQUAL,
    };
    const bool is_general = code <= SXML_OPERATOR_FUNCTION_GENERAL_NOT_EQUAL;
    const size_t operator_index =
        (size_t)code - (is_general ? (size_t)SXML_OPERATOR_FUNCTION_GENERAL_EQUAL
                                   : (size_t)SXML_OPERATOR_FUNCTION_VALUE_EQUAL);
    atomized_operands operands;
    int16_t timezone = 0;
    sxml_status status =
        sxml_dynamic_context_read_implicit_timezone(call->dynamic_context, &timezone);
    if (status == SXML_STATUS_OK) {
        status = atomize_operands(call, &operands);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_comparison_request request = {
        .comparison_operator = comparison_operators[operator_index],
        .left_operand = operands.left_operand,
        .right_operand = operands.right_operand,
        .implicit_timezone_offset_minutes = timezone,
        .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
        .error_record = call->error_record};
    if (is_general) {
        sxml_item truth = {.representation = 0};
        status = sxml_operator_make_general_comparison(call->item_heap, &request, &truth);
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(truth);
        }
    } else {
        status = sxml_operator_make_value_comparison(call->item_heap, &request, result);
    }
    release_operands(&operands);
    return status;
}

static sxml_status apply_arithmetic(const sxml_function_call *call, operator_code code,
                                    sxml_sequence *result) {
    static const sxml_arithmetic_operator arithmetic_operators[] = {
        SXML_ARITHMETIC_OPERATOR_ADD,    SXML_ARITHMETIC_OPERATOR_SUBTRACT,
        SXML_ARITHMETIC_OPERATOR_MULTIPLY, SXML_ARITHMETIC_OPERATOR_DIVIDE,
        SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE, SXML_ARITHMETIC_OPERATOR_MODULO,
    };
    atomized_operands operands;
    int16_t timezone = 0;
    sxml_status status =
        sxml_dynamic_context_read_implicit_timezone(call->dynamic_context, &timezone);
    if (status == SXML_STATUS_OK) {
        status = atomize_operands(call, &operands);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_operator_make_arithmetic_value(
        call->item_heap,
        &(sxml_arithmetic_request){
            .arithmetic_operator =
                arithmetic_operators[(size_t)code - (size_t)SXML_OPERATOR_FUNCTION_ADD],
            .left_operand = operands.left_operand,
            .right_operand = operands.right_operand,
            .implicit_timezone_offset_minutes = timezone,
            .error_record = call->error_record},
        result);
    release_operands(&operands);
    return status;
}

static sxml_status apply_node_comparison(const sxml_function_call *call,
                                         operator_code code, sxml_sequence *result) {
    static const sxml_node_comparison_operator node_operators[] = {
        SXML_NODE_COMPARISON_OPERATOR_PRECEDES,
        SXML_NODE_COMPARISON_OPERATOR_FOLLOWS,
        SXML_NODE_COMPARISON_OPERATOR_PRECEDES_OR_IS,
        SXML_NODE_COMPARISON_OPERATOR_FOLLOWS_OR_IS,
        SXML_NODE_COMPARISON_OPERATOR_IS,
        SXML_NODE_COMPARISON_OPERATOR_IS_NOT,
    };
    return sxml_operator_make_node_comparison(
        &(sxml_node_comparison_request){
            .node_comparison_operator =
                node_operators[(size_t)code - (size_t)SXML_OPERATOR_FUNCTION_PRECEDES],
            .left_operand = call_argument(call, 0),
            .right_operand = call_argument(call, 1),
            .error_record = call->error_record},
        result);
}

/* and, or: the effective boolean values (XPath 4.0, 4.13). */
static sxml_status apply_logical(const sxml_function_call *call, operator_code code,
                                 sxml_sequence *result) {
    sxml_effective_boolean_value truths[2] = {{.is_true = false}, {.is_true = false}};
    sxml_status status = read_truth(call_argument(call, 0), call->error_record, &truths[0]);
    if (status == SXML_STATUS_OK) {
        status = read_truth(call_argument(call, 1), call->error_record, &truths[1]);
    }
    if (status == SXML_STATUS_OK) {
        *result = boolean_sequence((sxml_effective_boolean_value){
            .is_true = code == SXML_OPERATOR_FUNCTION_AND
                           ? truths[0].is_true && truths[1].is_true
                           : truths[0].is_true || truths[1].is_true});
    }
    return status;
}

/* The body of fn:op's functions: fn($x, $y) { $x op $y }, the operator's
   code captured as an xs:integer. */
static sxml_status call_operator_function(const sxml_function_call *call,
                                          sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_function_item_description description;
    sxml_status status = sxml_function_item_describe(call->function_item, &description);
    int64_t code_number = 0;
    if (status == SXML_STATUS_OK && description.captured_values.sequence_count == 1) {
        status = read_integer(description.captured_values.sequences[0].content, &code_number);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (code_number < 0 || code_number > (int64_t)SXML_OPERATOR_FUNCTION_OTHERWISE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const operator_code code = (operator_code)code_number;
    const sxml_sequence left = call_argument(call, 0);
    const sxml_sequence right = call_argument(call, 1);
    switch (code) {
    case SXML_OPERATOR_FUNCTION_COMMA:
        return sxml_sequence_concatenate(
            call->item_heap,
            (sxml_sequence_view){.sequences = call->arguments.sequences, .sequence_count = 2},
            result);
    case SXML_OPERATOR_FUNCTION_AND:
    case SXML_OPERATOR_FUNCTION_OR:
        return apply_logical(call, code, result);
    case SXML_OPERATOR_FUNCTION_ADD:
    case SXML_OPERATOR_FUNCTION_SUBTRACT:
    case SXML_OPERATOR_FUNCTION_MULTIPLY:
    case SXML_OPERATOR_FUNCTION_DIVIDE:
    case SXML_OPERATOR_FUNCTION_INTEGER_DIVIDE:
    case SXML_OPERATOR_FUNCTION_MODULO:
        return apply_arithmetic(call, code, result);
    case SXML_OPERATOR_FUNCTION_GENERAL_EQUAL:
    case SXML_OPERATOR_FUNCTION_GENERAL_LESS:
    case SXML_OPERATOR_FUNCTION_GENERAL_LESS_OR_EQUAL:
    case SXML_OPERATOR_FUNCTION_GENERAL_GREATER:
    case SXML_OPERATOR_FUNCTION_GENERAL_GREATER_OR_EQUAL:
    case SXML_OPERATOR_FUNCTION_GENERAL_NOT_EQUAL:
    case SXML_OPERATOR_FUNCTION_VALUE_EQUAL:
    case SXML_OPERATOR_FUNCTION_VALUE_LESS:
    case SXML_OPERATOR_FUNCTION_VALUE_LESS_OR_EQUAL:
    case SXML_OPERATOR_FUNCTION_VALUE_GREATER:
    case SXML_OPERATOR_FUNCTION_VALUE_GREATER_OR_EQUAL:
    case SXML_OPERATOR_FUNCTION_VALUE_NOT_EQUAL:
        return apply_comparison(call, code, result);
    case SXML_OPERATOR_FUNCTION_PRECEDES:
    case SXML_OPERATOR_FUNCTION_FOLLOWS:
    case SXML_OPERATOR_FUNCTION_PRECEDES_OR_IS:
    case SXML_OPERATOR_FUNCTION_FOLLOWS_OR_IS:
    case SXML_OPERATOR_FUNCTION_IS:
    case SXML_OPERATOR_FUNCTION_IS_NOT:
        return apply_node_comparison(call, code, result);
    case SXML_OPERATOR_FUNCTION_UNION:
    case SXML_OPERATOR_FUNCTION_EXCEPT:
    case SXML_OPERATOR_FUNCTION_INTERSECT:
        return apply_node_set_operator(
            call,
            (node_set_operation){
                .node_set_operator = code == SXML_OPERATOR_FUNCTION_UNION ? SXML_NODE_SET_OPERATOR_UNION
                                     : code == SXML_OPERATOR_FUNCTION_EXCEPT
                                         ? SXML_NODE_SET_OPERATOR_EXCEPT
                                         : SXML_NODE_SET_OPERATOR_INTERSECT,
                .left_operand = left,
                .right_operand = right},
            result);
    case SXML_OPERATOR_FUNCTION_CONCATENATE:
    case SXML_OPERATOR_FUNCTION_RANGE: {
        atomized_operands operands;
        status = atomize_operands(call, &operands);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        status = code == SXML_OPERATOR_FUNCTION_CONCATENATE
                     ? concatenate_operands(call, &operands, result)
                     : range_of_operands(call, &operands, result);
        release_operands(&operands);
        return status;
    }
    case SXML_OPERATOR_FUNCTION_OTHERWISE: {
        const sxml_sequence chosen = sequence_is_empty(left) ? right : left;
        sxml_sequence_retain(chosen);
        *result = chosen;
        return SXML_STATUS_OK;
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The row of every function fn:op makes; no part lists it. */
static const sxml_native_function operator_function_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("op"),
    .parameters = {NULL, 0},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = call_operator_function,
    .resumable_body = NULL,
};

/* fn:op($operator) (F&O 4.0, 18.4): an anonymous function of signature
   fn(item()*, item()*) as item()*. */
static sxml_status call_op(const sxml_function_call *call, sxml_sequence *result) {
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(call_argument(call, 0).content, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t name_index = 0;
    const size_t name_count = sizeof(operator_names) / sizeof(operator_names[0]);
    while (name_index < name_count &&
           !sxml_byte_view_equals(operator_names[name_index].operator_text,
                                  datum.datum_details.string.utf8)) {
        name_index += 1;
    }
    if (name_index == name_count) {
        return sxml_error_record_report(call->error_record, &unknown_operator_error);
    }
    const sxml_sequence_type any_sequence = {
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    const sxml_sequence_type parameter_types[2] = {any_sequence, any_sequence};
    sxml_type_identifier signature = {0};
    status = sxml_type_registry_register_item_type(
        sxml_item_heap_get_type_registry(call->item_heap),
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {.is_any_function = false,
                                                  .parameter_types = {.sequence_types =
                                                                          parameter_types,
                                                                      .sequence_type_count = 2},
                                                  .result_type = any_sequence}}},
        &signature);
    const sxml_sequence code = position_sequence((uint64_t)operator_names[name_index].code);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_item_make(
            call->item_heap,
            &(sxml_function_item_request){
                .function_name = {.index = 0},
                .signature = signature,
                .function_annotations = {.function_annotations = NULL,
                                         .function_annotation_count = 0},
                .captured_values = {.sequences = &code, .sequence_count = 1},
                .body = {.state = (void *)&operator_function_row,
                         .operations = &sxml_native_function_body_operations}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ----------------------------------------------------------------- Rows */

SXML_TRAVERSAL_BODY(for_each_body, SXML_TRAVERSAL_OPERATION_FOR_EACH);
SXML_TRAVERSAL_BODY(filter_body, SXML_TRAVERSAL_OPERATION_FILTER);
SXML_TRAVERSAL_BODY(index_where_body, SXML_TRAVERSAL_OPERATION_INDEX_WHERE);
SXML_TRAVERSAL_BODY(take_while_body, SXML_TRAVERSAL_OPERATION_TAKE_WHILE);
SXML_TRAVERSAL_BODY(drop_while_body, SXML_TRAVERSAL_OPERATION_DROP_WHILE);
SXML_TRAVERSAL_BODY(every_body, SXML_TRAVERSAL_OPERATION_EVERY);
SXML_TRAVERSAL_BODY(some_body, SXML_TRAVERSAL_OPERATION_SOME);
SXML_TRAVERSAL_BODY(for_each_pair_body, SXML_TRAVERSAL_OPERATION_FOR_EACH_PAIR);
SXML_FOLD_BODY(fold_left_body, SXML_FOLD_OPERATION_FOLD_LEFT);
SXML_FOLD_BODY(fold_right_body, SXML_FOLD_OPERATION_FOLD_RIGHT);
SXML_FOLD_BODY(scan_body, SXML_FOLD_OPERATION_SCAN);
SXML_SORT_BODY(sort_by_body, SXML_SORT_OPERATION_SORT_BY);
SXML_SORT_BODY(sort_with_body, SXML_SORT_OPERATION_SORT_WITH);
SXML_SORT_BODY(highest_body, SXML_SORT_OPERATION_HIGHEST);
SXML_SORT_BODY(lowest_body, SXML_SORT_OPERATION_LOWEST);

static const sxml_resumable_native_body apply_body = {
    .step = step_apply,
    .release_state = release_single_call_state,
    .state_byte_count = sizeof(single_call_state),
    .state_alignment = alignof(single_call_state),
};

static const sxml_resumable_native_body do_until_body = {
    .step = step_do_until,
    .release_state = release_loop_state,
    .state_byte_count = sizeof(loop_state),
    .state_alignment = alignof(loop_state),
};

static const sxml_resumable_native_body while_do_body = {
    .step = step_while_do,
    .release_state = release_loop_state,
    .state_byte_count = sizeof(loop_state),
    .state_alignment = alignof(loop_state),
};

static const sxml_resumable_native_body partition_body = {
    .step = step_partition,
    .release_state = release_partition_state,
    .state_byte_count = sizeof(partition_state),
    .state_alignment = alignof(partition_state),
};

static const sxml_resumable_native_body transitive_closure_body = {
    .step = step_transitive_closure,
    .release_state = release_closure_state,
    .state_byte_count = sizeof(closure_state),
    .state_alignment = alignof(closure_state),
};

#define PARAMETER SXML_HIGHER_ORDER_PARAMETER
#define OPTIONAL_PARAMETER SXML_HIGHER_ORDER_OPTIONAL_PARAMETER

/* Parameter types in the F&O 4.0 spelling (2.5, 13, 18.4). */
#define POSITIONAL_PREDICATE "fn(item(), xs:integer) as xs:boolean?"
#define SORT_KEY "(fn(item()) as xs:anyAtomicType*)?"

static const sxml_function_parameter apply_parameters[] = {
    PARAMETER("function", "fn(*)"), PARAMETER("arguments", "array(*)")};
static const sxml_function_parameter do_until_parameters[] = {
    PARAMETER("input", "item()*"),
    PARAMETER("action", "fn(item()*, xs:integer) as item()*"),
    PARAMETER("predicate", "fn(item()*, xs:integer) as xs:boolean?")};
static const sxml_function_parameter while_do_parameters[] = {
    PARAMETER("input", "item()*"),
    PARAMETER("predicate", "fn(item()*, xs:integer) as xs:boolean?"),
    PARAMETER("action", "fn(item()*, xs:integer) as item()*")};
static const sxml_function_parameter predicate_parameters[] = {
    PARAMETER("input", "item()*"), PARAMETER("predicate", POSITIONAL_PREDICATE)};
static const sxml_function_parameter optional_predicate_parameters[] = {
    PARAMETER("input", "item()*"),
    OPTIONAL_PARAMETER("predicate", "(" POSITIONAL_PREDICATE ")?")};
static const sxml_function_parameter for_each_parameters[] = {
    PARAMETER("input", "item()*"),
    PARAMETER("action", "fn(item(), xs:integer) as item()*")};
static const sxml_function_parameter for_each_pair_parameters[] = {
    PARAMETER("input1", "item()*"), PARAMETER("input2", "item()*"),
    PARAMETER("action", "fn(item(), item(), xs:integer) as item()*")};
static const sxml_function_parameter fold_left_parameters[] = {
    PARAMETER("input", "item()*"), PARAMETER("init", "item()*"),
    PARAMETER("action", "fn(item()*, item()) as item()*")};
static const sxml_function_parameter fold_right_parameters[] = {
    PARAMETER("input", "item()*"), PARAMETER("init", "item()*"),
    PARAMETER("action", "fn(item(), item()*) as item()*")};
static const sxml_function_parameter scan_parameters[] = {
    PARAMETER("input", "item()*"), PARAMETER("init", "item()*"),
    PARAMETER("action", "fn(item()*, item(), xs:integer) as item()*")};
static const sxml_function_parameter partition_parameters[] = {
    PARAMETER("input", "item()*"),
    PARAMETER("split-when", "fn(item()*, item(), xs:integer) as xs:boolean?")};
static const sxml_function_parameter extreme_parameters[] = {
    PARAMETER("input", "item()*"), OPTIONAL_PARAMETER("collation", "xs:string?"),
    OPTIONAL_PARAMETER("key", SORT_KEY)};
static const sxml_function_parameter sort_by_parameters[] = {
    PARAMETER("input", "item()*"),
    PARAMETER("keys", "map(*)*")};
static const sxml_function_parameter sort_with_parameters[] = {
    PARAMETER("input", "item()*"),
    PARAMETER("comparators", "(fn(item(), item()) as xs:integer)+")};
static const sxml_function_parameter partial_apply_parameters[] = {
    PARAMETER("function", "fn(*)"),
    PARAMETER("arguments", "map(xs:integer, item()*)")};
static const sxml_function_parameter transitive_closure_parameters[] = {
    PARAMETER("node", "node()?"), PARAMETER("step", "fn(node()) as node()*")};
static const sxml_function_parameter function_lookup_parameters[] = {
    PARAMETER("name", "xs:QName"), PARAMETER("arity", "xs:integer")};
static const sxml_function_parameter function_parameters[] = {
    PARAMETER("function", "fn(*)")};
static const sxml_function_parameter op_parameters[] = {
    PARAMETER("operator", "xs:string")};

#define RESUMABLE(name_text, parameter_array, result_text, body)               \
    SXML_HIGHER_ORDER_RESUMABLE_ROW(SXML_NAMESPACE_FUNCTIONS, name_text,       \
                                    parameter_array, result_text, body)
#define LEAF(name_text, parameter_array, result_text, body)                    \
    SXML_HIGHER_ORDER_LEAF_ROW(SXML_NAMESPACE_FUNCTIONS, name_text,            \
                               parameter_array, result_text, body)

static const sxml_native_function higher_order_functions[] = {
    RESUMABLE("apply", apply_parameters, "item()*", apply_body),
    RESUMABLE("do-until", do_until_parameters, "item()*", do_until_body),
    RESUMABLE("drop-while", predicate_parameters, "item()*", drop_while_body),
    RESUMABLE("every", optional_predicate_parameters, "xs:boolean", every_body),
    RESUMABLE("filter", predicate_parameters, "item()*", filter_body),
    RESUMABLE("fold-left", fold_left_parameters, "item()*", fold_left_body),
    RESUMABLE("fold-right", fold_right_parameters, "item()*", fold_right_body),
    RESUMABLE("for-each", for_each_parameters, "item()*", for_each_body),
    RESUMABLE("for-each-pair", for_each_pair_parameters, "item()*", for_each_pair_body),
    RESUMABLE("highest", extreme_parameters, "item()*", highest_body),
    RESUMABLE("index-where", predicate_parameters, "xs:integer*", index_where_body),
    RESUMABLE("lowest", extreme_parameters, "item()*", lowest_body),
    LEAF("partial-apply", partial_apply_parameters, "fn(*)", call_partial_apply),
    RESUMABLE("partition", partition_parameters, "array(item()*)*", partition_body),
    RESUMABLE("scan", scan_parameters, "array(item()*)*", scan_body),
    RESUMABLE("some", optional_predicate_parameters, "xs:boolean", some_body),
    RESUMABLE("sort-by", sort_by_parameters, "item()*", sort_by_body),
    RESUMABLE("sort-with", sort_with_parameters, "item()*", sort_with_body),
    RESUMABLE("take-while", predicate_parameters, "item()*", take_while_body),
    RESUMABLE("transitive-closure", transitive_closure_parameters, "node()*",
              transitive_closure_body),
    RESUMABLE("while-do", while_do_parameters, "item()*", while_do_body),
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                   SXML_NATIVE_FUNCTION_TAKES_CALLER_FOCUS,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("function-lookup"),
     .parameters = {function_lookup_parameters,
                    SXML_HIGHER_ORDER_PARAMETER_COUNT(function_lookup_parameters)},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("fn(*)?"),
     .call = call_function_lookup,
     .resumable_body = NULL},
    LEAF("function-name", function_parameters, "xs:QName?", call_function_name),
    LEAF("function-arity", function_parameters, "xs:integer", call_function_arity),
    LEAF("function-annotations", function_parameters,
         "map(xs:QName, xs:anyAtomicType*)*", call_function_annotations),
    LEAF("op", op_parameters, "fn(item()*, item()*) as item()*", call_op),
};

const sxml_native_function_view sxml_function_library_higher_order = {
    .native_functions = higher_order_functions,
    .native_function_count =
        sizeof(higher_order_functions) / sizeof(higher_order_functions[0]),
};
