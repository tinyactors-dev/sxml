#pragma once

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/xdm.h"

/*
 * Resumable engines shared by the three library parts of packet 1.21
 * (docs/DECISIONS.md, D75, D79): src/function_library_higher_order.c
 * defines them; src/function_library_map.c and src/function_library_array.c
 * use them for their higher-order rows. Internal to sxml.
 *
 * Every engine is a native step function (src/function_library_internal.h,
 * "Resumable native bodies") parameterized by an operation: a row's step
 * function is a one-line wrapper that names its operation, and its
 * resumable body names the engine's state size and release function. No
 * engine calls a function item itself; each asks the evaluator for the
 * call and resumes with the result (D71), so no C stack grows with the
 * input.
 *
 * Thread safety: as the evaluator's (D35).
 */

/* ------------------------------------------------------------ Traversal */

/*
 * A traversal: one call of a function item per item of a sequence, member
 * of an array or entry of a map, in order, with the item (member; key and
 * value) and its 1-based position as arguments (glossary `traversal`).
 * What becomes of each call's value depends on the operation.
 */
typedef enum sxml_traversal_operation {
    /* fn:for-each: the concatenation of the values (F&O 4.0, 2.5.8). */
    SXML_TRAVERSAL_OPERATION_FOR_EACH,
    /* fn:filter: the items whose predicate is true (2.5.5). */
    SXML_TRAVERSAL_OPERATION_FILTER,
    /* fn:index-where: the positions whose predicate is true (2.5.11). */
    SXML_TRAVERSAL_OPERATION_INDEX_WHERE,
    /* fn:take-while: the items before the first false predicate (2.5.21). */
    SXML_TRAVERSAL_OPERATION_TAKE_WHILE,
    /* fn:drop-while: the items from the first false predicate (2.5.3). */
    SXML_TRAVERSAL_OPERATION_DROP_WHILE,
    /* fn:every, with fn:boolean#1 when the predicate is absent (2.5.4). */
    SXML_TRAVERSAL_OPERATION_EVERY,
    /* fn:some (2.5.16). */
    SXML_TRAVERSAL_OPERATION_SOME,
    /* fn:for-each-pair: items of two sequences and the position (2.5.9). */
    SXML_TRAVERSAL_OPERATION_FOR_EACH_PAIR,
    /* array:build: each value becomes a member; fn:identity#1 when the
       action is absent (15.2.2). */
    SXML_TRAVERSAL_OPERATION_ARRAY_BUILD,
    /* array:for-each (15.2.9). */
    SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH,
    /* array:filter (15.2.4). */
    SXML_TRAVERSAL_OPERATION_ARRAY_FILTER,
    /* array:index-where (15.2.14). */
    SXML_TRAVERSAL_OPERATION_ARRAY_INDEX_WHERE,
    /* array:for-each-pair (15.2.10). */
    SXML_TRAVERSAL_OPERATION_ARRAY_FOR_EACH_PAIR,
    /* map:for-each: key, value and position (14.4.8). */
    SXML_TRAVERSAL_OPERATION_MAP_FOR_EACH,
    /* map:filter (14.4.6). */
    SXML_TRAVERSAL_OPERATION_MAP_FILTER,
} sxml_traversal_operation;

/* Array members collected in allocator memory, owned. */
typedef struct sxml_member_list {
    sxml_sequence *sequences;
    size_t sequence_count;
    size_t sequence_capacity;
} sxml_member_list;

/* Map entries collected in allocator memory, borrowed from a map that
   outlives the list. */
typedef struct sxml_map_entry_list {
    sxml_map_entry *map_entries;
    size_t map_entry_count;
    size_t map_entry_capacity;
} sxml_map_entry_list;

/* The state of a traversal between its steps. Zeroed by the evaluator. */
typedef struct sxml_traversal_state {
    sxml_traversal_operation operation;
    /* The dynamic context's allocator, for the builder and lists. */
    sxml_allocator allocator;
    /* For sequence inputs. */
    sxml_sequence_iterator iterator;
    sxml_sequence_iterator second_iterator;
    /* The current item of the iterator (borrowed from the input). */
    sxml_item current_item;
    /* For array and map inputs: the member or entry count. */
    uint64_t input_count;
    /* The position of the item, member or entry last taken, from 1. */
    uint64_t position;
    bool is_finished;
    /* The xs:boolean of fn:every and fn:some once decided. */
    bool decided_value;
    sxml_sequence_builder *builder;
    sxml_member_list members;
    sxml_map_entry_list map_entries;
    /* Owned while a call runs. */
    sxml_sequence call_arguments[3];
} sxml_traversal_state;

/* The traversal step for operation; rows wrap it (see above). */
SXML_NODISCARD sxml_status sxml_traversal_step(
    const sxml_native_step_request *request, sxml_native_step *step,
    sxml_traversal_operation operation);

/* release_state of every traversal row. */
void sxml_traversal_release_state(void *state);

/* --------------------------------------------------------------- Folds */

/* The folds (F&O 4.0, 2.5.6, 2.5.7, 2.5.15, 15.2.6, 15.2.7). */
typedef enum sxml_fold_operation {
    /* fn:fold-left: $action($acc, $item). */
    SXML_FOLD_OPERATION_FOLD_LEFT,
    /* fn:fold-right: $action($item, $acc), last item first. */
    SXML_FOLD_OPERATION_FOLD_RIGHT,
    /* fn:scan: as fold-left with the position, every accumulator kept as a
       single-member array. */
    SXML_FOLD_OPERATION_SCAN,
    /* array:fold-left over members. */
    SXML_FOLD_OPERATION_ARRAY_FOLD_LEFT,
    /* array:fold-right over members. */
    SXML_FOLD_OPERATION_ARRAY_FOLD_RIGHT,
} sxml_fold_operation;

typedef struct sxml_fold_state {
    sxml_fold_operation operation;
    sxml_allocator allocator;
    /* Items or members; for the right folds the next is input_count -
       position. */
    uint64_t input_count;
    uint64_t position;
    sxml_sequence_iterator iterator;
    /* glossary `accumulator`; owned. */
    sxml_sequence accumulator;
    /* fn:scan's arrays. */
    sxml_sequence_builder *builder;
    sxml_sequence call_arguments[3];
} sxml_fold_state;

SXML_NODISCARD sxml_status sxml_fold_step(const sxml_native_step_request *request,
                                         sxml_native_step *step,
                                         sxml_fold_operation operation);

void sxml_fold_release_state(void *state);

/* --------------------------------------------------------------- Sorts */

/* The sorts and the selections built on sort keys (F&O 4.0, 2.5.10,
   2.5.12, 2.5.18, 2.5.20, 15.2.25, 15.2.26, 15.2.28). */
typedef enum sxml_sort_operation {
    /* fn:sort-by($input, $keys). */
    SXML_SORT_OPERATION_SORT_BY,
    /* fn:sort-with($input, $comparators). */
    SXML_SORT_OPERATION_SORT_WITH,
    /* fn:highest($input, $collation, $key). */
    SXML_SORT_OPERATION_HIGHEST,
    /* fn:lowest($input, $collation, $key). */
    SXML_SORT_OPERATION_LOWEST,
    /* array:sort($array, $collation, $key). */
    SXML_SORT_OPERATION_ARRAY_SORT,
    /* array:sort-by($array, $keys). */
    SXML_SORT_OPERATION_ARRAY_SORT_BY,
    /* array:sort-with($array, $comparators). */
    SXML_SORT_OPERATION_ARRAY_SORT_WITH,
} sxml_sort_operation;

/* One sort key definition (F&O 4.0, 2.5.19 fn:sort-key-record): its key
   function (borrowed; none means fn:data#1) and its order direction. */
typedef struct sxml_sort_key_definition {
    bool has_key_function;
    sxml_item key_function;
    bool is_descending;
} sxml_sort_key_definition;

typedef struct sxml_sort_state {
    sxml_sort_operation operation;
    sxml_allocator allocator;
    sxml_item_heap *item_heap;
    int16_t implicit_timezone_offset_minutes;
    /* The items (as single-item sequences) or members to order; owned. */
    sxml_sequence *sorted_values;
    size_t sorted_value_count;
    sxml_sort_key_definition *key_definitions;
    size_t key_definition_count;
    /* sorted_value_count x key_definition_count sort key values, owned;
       glossary `sort_key_value`. */
    sxml_sequence *key_values;
    size_t next_key_value_index;
    /* The order: positions into sorted_values. */
    size_t *indices;
    size_t *scratch_indices;
    /* fn:sort-with's merge sort, run a comparison at a time: the run
       width, the bounds of the two runs being merged and their cursors. */
    size_t run_width;
    size_t left_start;
    size_t left_cursor;
    size_t middle;
    size_t right_cursor;
    size_t right_end;
    size_t output_cursor;
    size_t comparator_index;
    /* The call's error record, borrowed for one step (for the
       comparisons of sxml_evaluator_sort_indices). */
    sxml_error_record *error_record;
    sxml_sequence call_arguments[2];
} sxml_sort_state;

SXML_NODISCARD sxml_status sxml_sort_step(const sxml_native_step_request *request,
                                         sxml_native_step *step,
                                         sxml_sort_operation operation);

void sxml_sort_release_state(void *state);

/* --------------------------------------------------------- Shared help */

/* A status for a failure with a static error record (src/xdm_internal.h)
   naming an F&O error code: FOAY0001, FOAY0002, ... */
typedef enum sxml_higher_order_error {
    /* FOAY0001: an array position out of bounds. */
    SXML_HIGHER_ORDER_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS,
    /* FOAY0002: a negative array length. */
    SXML_HIGHER_ORDER_ERROR_NEGATIVE_ARRAY_LENGTH,
    /* FOCH0002: a collation other than the codepoint collation. */
    SXML_HIGHER_ORDER_ERROR_UNSUPPORTED_COLLATION,
    /* FOJS0003: a duplicate key under "reject". */
    SXML_HIGHER_ORDER_ERROR_DUPLICATE_KEY,
    /* FOJS0005: an option with an invalid value. */
    SXML_HIGHER_ORDER_ERROR_INVALID_OPTION,
    /* XPTY0004: a value of the wrong type. */
    SXML_HIGHER_ORDER_ERROR_WRONG_TYPE,
} sxml_higher_order_error;

/* Fills error_record (may be NULL) with error and returns its status. */
SXML_NODISCARD sxml_status sxml_higher_order_report_error(
    sxml_error_record *error_record, sxml_higher_order_error error);

/*
 * Checks a collation argument (F&O 4.0, 5.2.7): the empty sequence and the
 * Unicode codepoint collation URI are accepted (Phase 1 knows no other,
 * glossary `collation`); anything else is FOCH0002.
 */
SXML_NODISCARD sxml_status sxml_higher_order_check_collation(
    sxml_sequence collation, sxml_error_record *error_record);

/* Appends sequence (owned: taken over, also on failure) to list, growing
   it through allocator. */
SXML_NODISCARD sxml_status sxml_member_list_take(sxml_allocator allocator,
                                                  sxml_member_list *list,
                                                  sxml_sequence sequence);

/* Releases the sequences of list and its memory. */
void sxml_member_list_release(sxml_allocator allocator, sxml_member_list *list);

/* Makes the array of the list's members, owned, in *array. Returns the
   heap's failures. */
SXML_NODISCARD sxml_status sxml_member_list_make_array(
    sxml_item_heap *item_heap, const sxml_member_list *list, sxml_item *array);

/* ------------------------------------------------------- Row helpers */

/*
 * Rows of the three parts are written with these macros, so each reads
 * like its F&O 4.0 signature. A parameter whose default the body supplies
 * (F&O 4.0, 1.5) is an optional one.
 */
#define SXML_HIGHER_ORDER_PARAMETER(name_text, type_text)                      \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                  \
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER(type_text),                  \
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED}

#define SXML_HIGHER_ORDER_OPTIONAL_PARAMETER(name_text, type_text)             \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                  \
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER(type_text),                  \
     .parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY}

#define SXML_HIGHER_ORDER_PARAMETER_COUNT(parameter_array)                     \
    (sizeof(parameter_array) / sizeof((parameter_array)[0]))

/* A row whose body calls no function item. */
#define SXML_HIGHER_ORDER_LEAF_ROW(namespace_name, name_text, parameter_array,  \
                                   result_text, body)                          \
    {.function_namespace = (namespace_name),                                   \
     .properties = 0,                                                          \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                      \
     .parameters = {(parameter_array),                                         \
                    SXML_HIGHER_ORDER_PARAMETER_COUNT(parameter_array)},       \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                   \
     .call = (body),                                                           \
     .resumable_body = NULL}

/* A row with a resumable body (D75). */
#define SXML_HIGHER_ORDER_RESUMABLE_ROW(namespace_name, name_text,              \
                                        parameter_array, result_text, body)    \
    {.function_namespace = (namespace_name),                                   \
     .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,                  \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                      \
     .parameters = {(parameter_array),                                         \
                    SXML_HIGHER_ORDER_PARAMETER_COUNT(parameter_array)},       \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                   \
     .call = NULL,                                                             \
     .resumable_body = &(body)}

/* Defines body_name, a resumable body that runs engine_step (a
   sxml_traversal_step, sxml_fold_step or sxml_sort_step) for operation,
   with that engine's state type and release function. */
#define SXML_HIGHER_ORDER_ENGINE_BODY(body_name, engine_step, state_type,       \
                                      release_function, operation)             \
    static sxml_status body_name##_step(                                       \
        const sxml_native_step_request *request, sxml_native_step *step) {     \
        return engine_step(request, step, (operation));                        \
    }                                                                          \
    static const sxml_resumable_native_body body_name = {                      \
        .step = body_name##_step,                                              \
        .release_state = (release_function),                                   \
        .state_byte_count = sizeof(state_type),                                \
        .state_alignment = alignof(state_type),                                \
    }

#define SXML_TRAVERSAL_BODY(body_name, operation)                              \
    SXML_HIGHER_ORDER_ENGINE_BODY(body_name, sxml_traversal_step,              \
                                  sxml_traversal_state,                        \
                                  sxml_traversal_release_state, operation)

#define SXML_FOLD_BODY(body_name, operation)                                   \
    SXML_HIGHER_ORDER_ENGINE_BODY(body_name, sxml_fold_step, sxml_fold_state,  \
                                  sxml_fold_release_state, operation)

#define SXML_SORT_BODY(body_name, operation)                                   \
    SXML_HIGHER_ORDER_ENGINE_BODY(body_name, sxml_sort_step, sxml_sort_state,  \
                                  sxml_sort_release_state, operation)
