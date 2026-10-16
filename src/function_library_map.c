/*
 * The functions on maps of F&O 4.0, section 14.4 (map:build to map:size):
 * one library part, sxml_function_library_map (src/function_library_
 * internal.h, D55, D79). map:filter and map:for-each are traversals of
 * src/function_library_higher_order.c; map:build and map:merge share the
 * resumable assembly below, which asks the evaluator for each call of a
 * key, value or duplicates function (D71, D75). The maps themselves are
 * src/xdm_map.c's (D61): entry order is kept, put and remove share
 * structure.
 *
 * Golden file: src/function_library_higher_order.c.
 */
#include "function_library_higher_order_internal.h"

#include "evaluator_internal.h"
#include "xdm_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Helpers */

static sxml_sequence map_argument(const sxml_function_call *call, size_t argument_index) {
    if (argument_index >= call->arguments.sequence_count) {
        return sxml_sequence_empty();
    }
    return call->arguments.sequences[argument_index];
}

static bool map_sequence_is_empty(sxml_sequence sequence) {
    return sxml_sequence_get_representation(sequence) ==
           SXML_SEQUENCE_REPRESENTATION_EMPTY;
}

static sxml_status create_map_builder(const sxml_function_call *call,
                                      sxml_sequence_builder **builder) {
    return sxml_sequence_builder_create(
        call->dynamic_context != NULL ? call->dynamic_context->allocator
                                      : sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, builder);
}

/* The allocator for work memory: the dynamic context's, or the heap's
   for a call from outside an evaluation. */
static sxml_allocator map_call_allocator(const sxml_function_call *call) {
    return call->dynamic_context != NULL ? call->dynamic_context->allocator
                                         : sxml_item_heap_as_allocator(call->item_heap);
}

/* A single-entry map {key: value}, owned. */
typedef struct single_entry {
    sxml_item key;
    sxml_sequence value;
} single_entry;

static sxml_status make_single_entry_map(const sxml_function_call *call,
                                         single_entry entry, sxml_item *map) {
    return sxml_map_make(
        call->item_heap,
        &(sxml_map_request){
            .map_entries = {.map_entries = &(sxml_map_entry){.key = entry.key,
                                                             .value = entry.value},
                            .map_entry_count = 1},
            .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
            .error_record = call->error_record},
        map);
}

/* -------------------------------------------------------------- Leaves */

/* map:contains($map, $key) (F&O 4.0, 14.4.2). */
static sxml_status call_contains(const sxml_function_call *call, sxml_sequence *result) {
    sxml_map_lookup lookup;
    const sxml_status status = sxml_map_find(map_argument(call, 0).content,
                                             map_argument(call, 1).content, &lookup);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(lookup.is_found ? sxml_atomic_item_true()
                                                          : sxml_atomic_item_false());
    }
    return status;
}

/* map:empty($map) (14.4.3). */
static sxml_status call_empty(const sxml_function_call *call, sxml_sequence *result) {
    size_t entry_count = 0;
    const sxml_status status = sxml_map_get_size(map_argument(call, 0).content, &entry_count);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(entry_count == 0 ? sxml_atomic_item_true()
                                                           : sxml_atomic_item_false());
    }
    return status;
}

/* map:size($map) (14.4.15). */
static sxml_status call_size(const sxml_function_call *call, sxml_sequence *result) {
    size_t entry_count = 0;
    const sxml_status status = sxml_map_get_size(map_argument(call, 0).content, &entry_count);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)entry_count));
    }
    return status;
}

/* What map:keys, map:items and map:entries collect from each entry. */
typedef enum entry_part {
    SXML_ENTRY_PART_KEY,
    SXML_ENTRY_PART_VALUE,
    SXML_ENTRY_PART_SINGLE_ENTRY_MAP,
} entry_part;

/* The keys, values or single-entry maps of every entry, in entry order
   (F&O 4.0, 14.4.11, 14.4.10, 14.4.4). */
static sxml_status collect_entries(const sxml_function_call *call, entry_part part,
                                   sxml_sequence *result) {
    const sxml_item map = map_argument(call, 0).content;
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = create_map_builder(call, &builder);
    }
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status != SXML_STATUS_OK) {
            break;
        }
        switch (part) {
        case SXML_ENTRY_PART_KEY:
            status = sxml_sequence_builder_append_item(builder, entry.key);
            break;
        case SXML_ENTRY_PART_VALUE:
            status = sxml_sequence_builder_append_sequence(builder, entry.value,
                                                           call->error_record);
            break;
        case SXML_ENTRY_PART_SINGLE_ENTRY_MAP: {
            sxml_item single = {.representation = 0};
            status = make_single_entry_map(
                call, (single_entry){.key = entry.key, .value = entry.value}, &single);
            if (status == SXML_STATUS_OK) {
                status = sxml_sequence_builder_append_item(builder, single);
                sxml_item_release(single);
            }
            break;
        }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

static sxml_status call_keys(const sxml_function_call *call, sxml_sequence *result) {
    return collect_entries(call, SXML_ENTRY_PART_KEY, result);
}

static sxml_status call_items(const sxml_function_call *call, sxml_sequence *result) {
    return collect_entries(call, SXML_ENTRY_PART_VALUE, result);
}

static sxml_status call_entries(const sxml_function_call *call, sxml_sequence *result) {
    return collect_entries(call, SXML_ENTRY_PART_SINGLE_ENTRY_MAP, result);
}

/* map:entry($key, $value) (14.4.5). */
static sxml_status call_entry(const sxml_function_call *call, sxml_sequence *result) {
    sxml_item map = {.representation = 0};
    const sxml_status status = make_single_entry_map(
        call,
        (single_entry){.key = map_argument(call, 0).content, .value = map_argument(call, 1)},
        &map);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(map);
    }
    return status;
}

/* map:get($map, $key, $default := ()) (14.4.9). */
static sxml_status call_get(const sxml_function_call *call, sxml_sequence *result) {
    sxml_map_lookup lookup;
    const sxml_status status = sxml_map_find(map_argument(call, 0).content,
                                             map_argument(call, 1).content, &lookup);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence value = lookup.is_found ? lookup.entry.value : map_argument(call, 2);
    sxml_sequence_retain(value);
    *result = value;
    return SXML_STATUS_OK;
}

/* map:put($map, $key, $value) (14.4.13). */
static sxml_status call_put(const sxml_function_call *call, sxml_sequence *result) {
    sxml_item map = {.representation = 0};
    const sxml_status status = sxml_map_put(
        call->item_heap,
        &(sxml_map_update){.map = map_argument(call, 0).content,
                           .key = map_argument(call, 1).content,
                           .value = map_argument(call, 2)},
        &map);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(map);
    }
    return status;
}

/* map:remove($map, $keys) (14.4.14): each key in turn; absent keys are
   ignored. */
static sxml_status call_remove(const sxml_function_call *call, sxml_sequence *result) {
    sxml_item map = map_argument(call, 0).content;
    sxml_item_retain(map);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(map_argument(call, 1));
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        sxml_iteration_step key_step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &key_step);
        if (status != SXML_STATUS_OK || !key_step.has_item) {
            break;
        }
        sxml_item smaller = {.representation = 0};
        status = sxml_map_remove(
            call->item_heap,
            &(sxml_map_update){.map = map, .key = key_step.item,
                               .value = sxml_sequence_empty()},
            &smaller);
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_item_release(map);
        map = smaller;
    }
    if (status != SXML_STATUS_OK) {
        sxml_item_release(map);
        return status;
    }
    *result = sxml_sequence_from_item(map);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- map:find */

/* One level of map:find's search: a sequence, an array or a map, and the
   next item, member or entry to look at. */
typedef enum find_frame_kind {
    SXML_FIND_FRAME_KIND_SEQUENCE,
    SXML_FIND_FRAME_KIND_ARRAY,
    SXML_FIND_FRAME_KIND_MAP,
} find_frame_kind;

typedef struct find_frame {
    find_frame_kind frame_kind;
    /* Borrowed from the input, which holds every nested value. */
    sxml_sequence value;
    uint64_t next_index;
} find_frame;

typedef struct find_stack {
    sxml_allocator allocator;
    find_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
} find_stack;

static sxml_status find_stack_push(find_stack *stack, find_frame frame) {
    if (stack->frame_count == stack->frame_capacity) {
        void *frames = stack->frames;
        const sxml_status status = sxml_evaluator_grow(
            stack->allocator,
            &(sxml_evaluator_growth){.elements = &frames,
                                     .element_capacity = &stack->frame_capacity,
                                     .element_count = stack->frame_count,
                                     .required_count = stack->frame_count + 1,
                                     .element_byte_count = sizeof(find_frame),
                                     .alignment = alignof(find_frame)});
        stack->frames = frames;
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    stack->frames[stack->frame_count] = frame;
    stack->frame_count += 1;
    return SXML_STATUS_OK;
}

/* Pushes the frame for an item that is a map or an array; ignores other
   items (F&O 4.0, 14.4.7). */
static sxml_status find_push_item(find_stack *stack, sxml_item item) {
    switch (sxml_item_get_kind(item)) {
    case SXML_ITEM_KIND_ARRAY:
        return find_stack_push(stack, (find_frame){.frame_kind = SXML_FIND_FRAME_KIND_ARRAY,
                                                   .value = sxml_sequence_from_item(item),
                                                   .next_index = 0});
    case SXML_ITEM_KIND_MAP:
        return find_stack_push(stack, (find_frame){.frame_kind = SXML_FIND_FRAME_KIND_MAP,
                                                   .value = sxml_sequence_from_item(item),
                                                   .next_index = 0});
    case SXML_ITEM_KIND_ATOMIC:
    case SXML_ITEM_KIND_NODE:
    case SXML_ITEM_KIND_JSON_NODE:
    case SXML_ITEM_KIND_FUNCTION:
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/* Runs one step of the search on the top frame. */
static sxml_status find_step(find_stack *stack, const sxml_function_call *call,
                             sxml_member_list *found) {
    find_frame *frame = &stack->frames[stack->frame_count - 1];
    const sxml_item key = map_argument(call, 1).content;
    sxml_status status = SXML_STATUS_OK;
    switch (frame->frame_kind) {
    case SXML_FIND_FRAME_KIND_SEQUENCE: {
        sxml_item item = {.representation = 0};
        status = sxml_sequence_get_item(
            frame->value,
            (sxml_item_request){.item_index = frame->next_index,
                                .error_record = call->error_record},
            &item);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            stack->frame_count -= 1;
            return SXML_STATUS_OK;
        }
        frame->next_index += 1;
        return status == SXML_STATUS_OK ? find_push_item(stack, item) : status;
    }
    case SXML_FIND_FRAME_KIND_ARRAY: {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(frame->value.content, (size_t)frame->next_index,
                                       &member);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            stack->frame_count -= 1;
            return SXML_STATUS_OK;
        }
        frame->next_index += 1;
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return find_stack_push(stack, (find_frame){.frame_kind = SXML_FIND_FRAME_KIND_SEQUENCE,
                                                   .value = member,
                                                   .next_index = 0});
    }
    case SXML_FIND_FRAME_KIND_MAP: {
        sxml_map_entry entry;
        status = sxml_map_get_entry(frame->value.content, (size_t)frame->next_index, &entry);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            stack->frame_count -= 1;
            return SXML_STATUS_OK;
        }
        frame->next_index += 1;
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* The value goes into the result before its contents are
           searched. */
        if (sxml_atomic_item_is_atomic_equal(entry.key, key)) {
            sxml_sequence_retain(entry.value);
            status = sxml_member_list_take(stack->allocator, found, entry.value);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return find_stack_push(stack, (find_frame){.frame_kind = SXML_FIND_FRAME_KIND_SEQUENCE,
                                                   .value = entry.value,
                                                   .next_index = 0});
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* map:find($input, $key) (14.4.7): searched with an explicit stack (D71),
   values in the order the rules visit them. */
static sxml_status call_find(const sxml_function_call *call, sxml_sequence *result) {
    find_stack stack = {.allocator = map_call_allocator(call)};
    sxml_member_list found = {.sequences = NULL, .sequence_count = 0,
                              .sequence_capacity = 0};
    sxml_status status = find_stack_push(
        &stack, (find_frame){.frame_kind = SXML_FIND_FRAME_KIND_SEQUENCE,
                             .value = map_argument(call, 0),
                             .next_index = 0});
    while (status == SXML_STATUS_OK && stack.frame_count > 0) {
        status = find_step(&stack, call, &found);
    }
    sxml_item array = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_member_list_make_array(call->item_heap, &found, &array);
    }
    sxml_member_list_release(stack.allocator, &found);
    if (stack.frames != NULL) {
        sxml_allocator_release(stack.allocator, stack.frames);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(array);
    }
    return status;
}

/* ------------------------------------------------- map:build, map:merge */

/* Which function an assembly is. */
typedef enum assembly_kind {
    SXML_ASSEMBLY_KIND_BUILD,
    SXML_ASSEMBLY_KIND_MERGE,
} assembly_kind;

/* Whether an assembly step can go on, or must ask for a call. */
typedef struct assembly_progress {
    bool has_next;
    bool needs_call;
} assembly_progress;

/* The duplicates option (F&O 4.0, 14.4.1). */
typedef enum duplicates_policy {
    SXML_DUPLICATES_POLICY_REJECT,
    SXML_DUPLICATES_POLICY_USE_FIRST,
    SXML_DUPLICATES_POLICY_USE_LAST,
    SXML_DUPLICATES_POLICY_USE_ANY,
    SXML_DUPLICATES_POLICY_COMBINE,
    SXML_DUPLICATES_POLICY_FUNCTION,
} duplicates_policy;

/* What the assembly's call in progress computes. */
typedef enum assembly_phase {
    SXML_ASSEMBLY_PHASE_NEXT,
    SXML_ASSEMBLY_PHASE_KEY,
    SXML_ASSEMBLY_PHASE_VALUE,
    SXML_ASSEMBLY_PHASE_INSERT,
    SXML_ASSEMBLY_PHASE_COMBINE,
} assembly_phase;

/*
 * The state of map:build and map:merge: the map so far (owned), the keys
 * and value of the item or entry being added (owned), and the next key to
 * insert. map:build takes items from its input, map:merge entries from
 * each of its maps in turn.
 */
typedef struct assembly_state {
    bool is_merge;
    duplicates_policy policy;
    sxml_item combine_function;
    sxml_item map;
    sxml_sequence_iterator iterator;
    sxml_item current_item;
    uint64_t position;
    sxml_item current_map;
    size_t entry_index;
    size_t entry_count;
    sxml_sequence keys;
    sxml_sequence value;
    uint64_t key_index;
    sxml_item pending_key;
    assembly_phase phase;
    sxml_sequence call_arguments[2];
} assembly_state;

static void release_assembly_arguments(assembly_state *state) {
    for (size_t argument_index = 0; argument_index < 2; argument_index += 1) {
        sxml_sequence_release(state->call_arguments[argument_index]);
        state->call_arguments[argument_index] = sxml_sequence_empty();
    }
}

static void release_assembly_state(void *state) {
    assembly_state *assembly = state;
    sxml_item_release(assembly->map);
    sxml_sequence_release(assembly->keys);
    sxml_sequence_release(assembly->value);
    release_assembly_arguments(assembly);
}

/* Reads the duplicates option of options (a map or the empty sequence;
   other options are ignored). */
static sxml_status read_duplicates_option(const sxml_function_call *call,
                                          sxml_sequence options,
                                          assembly_state *state) {
    if (map_sequence_is_empty(options)) {
        return SXML_STATUS_OK;
    }
    static const sxml_byte_view option_name = SXML_BYTE_VIEW_INITIALIZER("duplicates");
    sxml_item key = {.representation = 0};
    sxml_status status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = option_name,
                                                   .codepoint_count = option_name.byte_count,
                                                   .is_ascii = true}}}},
        &key);
    sxml_map_lookup lookup = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_find(options.content, key, &lookup);
        sxml_item_release(key);
    }
    if (status != SXML_STATUS_OK || !lookup.is_found ||
        map_sequence_is_empty(lookup.entry.value)) {
        return status;
    }
    const sxml_sequence value = lookup.entry.value;
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        return sxml_higher_order_report_error(call->error_record,
                                              SXML_HIGHER_ORDER_ERROR_INVALID_OPTION);
    }
    if (sxml_item_is_function_item(value.content)) {
        state->policy = SXML_DUPLICATES_POLICY_FUNCTION;
        state->combine_function = value.content;
        return SXML_STATUS_OK;
    }
    static const struct {
        sxml_byte_view policy_text;
        duplicates_policy policy;
    } policies[] = {
        {SXML_BYTE_VIEW_INITIALIZER("reject"), SXML_DUPLICATES_POLICY_REJECT},
        {SXML_BYTE_VIEW_INITIALIZER("use-first"), SXML_DUPLICATES_POLICY_USE_FIRST},
        {SXML_BYTE_VIEW_INITIALIZER("use-last"), SXML_DUPLICATES_POLICY_USE_LAST},
        {SXML_BYTE_VIEW_INITIALIZER("use-any"), SXML_DUPLICATES_POLICY_USE_ANY},
        {SXML_BYTE_VIEW_INITIALIZER("combine"), SXML_DUPLICATES_POLICY_COMBINE},
    };
    sxml_atomic_datum datum;
    if (sxml_item_get_kind(value.content) == SXML_ITEM_KIND_ATOMIC &&
        sxml_atomic_item_get_datum(value.content, &datum) == SXML_STATUS_OK &&
        datum.datum_kind == SXML_DATUM_KIND_STRING) {
        for (size_t policy_index = 0; policy_index < sizeof(policies) / sizeof(policies[0]);
             policy_index += 1) {
            if (sxml_byte_view_equals(policies[policy_index].policy_text,
                                      datum.datum_details.string.utf8)) {
                state->policy = policies[policy_index].policy;
                return SXML_STATUS_OK;
            }
        }
    }
    return sxml_higher_order_report_error(call->error_record,
                                          SXML_HIGHER_ORDER_ERROR_INVALID_OPTION);
}

static sxml_status assembly_start(assembly_state *state, const sxml_function_call *call,
                                  assembly_kind kind) {
    const bool is_merge = kind == SXML_ASSEMBLY_KIND_MERGE;
    state->is_merge = is_merge;
    /* map:build combines duplicates by default, map:merge keeps the first
       (F&O 4.0, 14.4.1, 14.4.12). */
    state->policy = is_merge ? SXML_DUPLICATES_POLICY_USE_FIRST : SXML_DUPLICATES_POLICY_COMBINE;
    state->iterator = sxml_sequence_iterator_start(map_argument(call, 0));
    state->phase = SXML_ASSEMBLY_PHASE_NEXT;
    sxml_status status =
        read_duplicates_option(call, map_argument(call, is_merge ? 1 : 3), state);
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = NULL, .map_entry_count = 0},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                                .error_record = call->error_record},
            &state->map);
    }
    return status;
}

/* Puts key and value (borrowed) into the map. */
static sxml_status assembly_put(assembly_state *state, const sxml_function_call *call,
                                single_entry entry) {
    sxml_item larger = {.representation = 0};
    const sxml_status status = sxml_map_put(
        call->item_heap,
        &(sxml_map_update){.map = state->map, .key = entry.key, .value = entry.value},
        &larger);
    if (status == SXML_STATUS_OK) {
        sxml_item_release(state->map);
        state->map = larger;
    }
    return status;
}

/* Inserts the remaining keys of the current item or entry; stops with
   *needs_call when the duplicates function must combine two values. */
static sxml_status assembly_insert(assembly_state *state, const sxml_function_call *call,
                                   assembly_progress *progress) {
    progress->needs_call = false;
    for (;;) {
        sxml_item key = {.representation = 0};
        sxml_status status = sxml_sequence_get_item(
            state->keys,
            (sxml_item_request){.item_index = state->key_index,
                                .error_record = call->error_record},
            &key);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            return SXML_STATUS_OK;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        state->key_index += 1;
        sxml_map_lookup lookup;
        status = sxml_map_find(state->map, key, &lookup);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!lookup.is_found) {
            status = assembly_put(state, call, (single_entry){.key = key, .value = state->value});
            if (status != SXML_STATUS_OK) {
                return status;
            }
            continue;
        }
        switch (state->policy) {
        case SXML_DUPLICATES_POLICY_REJECT:
            return sxml_higher_order_report_error(call->error_record,
                                                  SXML_HIGHER_ORDER_ERROR_DUPLICATE_KEY);
        case SXML_DUPLICATES_POLICY_USE_FIRST:
        case SXML_DUPLICATES_POLICY_USE_ANY:
            break;
        case SXML_DUPLICATES_POLICY_USE_LAST:
            status = assembly_put(state, call, (single_entry){.key = key, .value = state->value});
            break;
        case SXML_DUPLICATES_POLICY_COMBINE: {
            const sxml_sequence parts[2] = {lookup.entry.value, state->value};
            sxml_sequence combined = sxml_sequence_empty();
            status = sxml_sequence_concatenate(
                call->item_heap, (sxml_sequence_view){.sequences = parts, .sequence_count = 2},
                &combined);
            if (status == SXML_STATUS_OK) {
                status = assembly_put(state, call, (single_entry){.key = key, .value = combined});
                sxml_sequence_release(combined);
            }
            break;
        }
        case SXML_DUPLICATES_POLICY_FUNCTION:
            sxml_sequence_retain(lookup.entry.value);
            state->call_arguments[0] = lookup.entry.value;
            sxml_sequence_retain(state->value);
            state->call_arguments[1] = state->value;
            state->pending_key = key;
            progress->needs_call = true;
            return SXML_STATUS_OK;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
}

/* Moves to the next entry of map:merge's maps; *has_next false at the
   end. Sets keys and value. */
static sxml_status merge_next_entry(assembly_state *state, const sxml_function_call *call,
                                    assembly_progress *progress) {
    progress->has_next = false;
    while (state->entry_index >= state->entry_count) {
        sxml_iteration_step map_step;
        sxml_status status =
            sxml_sequence_iterator_next(&state->iterator, call->error_record, &map_step);
        if (status != SXML_STATUS_OK || !map_step.has_item) {
            return status;
        }
        state->current_map = map_step.item;
        state->entry_index = 0;
        status = sxml_map_get_size(state->current_map, &state->entry_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_map_entry entry;
    const sxml_status status =
        sxml_map_get_entry(state->current_map, state->entry_index, &entry);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->entry_index += 1;
    sxml_item_retain(entry.key);
    state->keys = sxml_sequence_from_item(entry.key);
    sxml_sequence_retain(entry.value);
    state->value = entry.value;
    progress->has_next = true;
    return SXML_STATUS_OK;
}

/* Asks for $key($item, $pos) or $value($item, $pos) of map:build. */
static sxml_status build_request(assembly_state *state, sxml_item function_item,
                                 sxml_native_step *step) {
    sxml_item_retain(state->current_item);
    state->call_arguments[0] = sxml_sequence_from_item(state->current_item);
    state->call_arguments[1] =
        sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)state->position));
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_CALL,
                               .result = sxml_sequence_empty(),
                               .function_item = function_item,
                               .arguments = {.sequences = state->call_arguments,
                                             .sequence_count = 2}};
    return SXML_STATUS_OK;
}

/* map:build's keys for the current item without a $key function:
   fn:identity#1 coerced to xs:anyAtomicType*, that is, atomized. */
static sxml_status build_default_keys(assembly_state *state, const sxml_function_call *call) {
    return sxml_evaluator_atomize(
        call->dynamic_context,
        &(sxml_atomization_request){.value = sxml_sequence_from_item(state->current_item),
                                    .error_record = call->error_record},
        &state->keys);
}

/* Takes the value of the call in progress (owned). */
static sxml_status assembly_take_value(assembly_state *state,
                                       const sxml_function_call *call,
                                       sxml_sequence value) {
    release_assembly_arguments(state);
    switch (state->phase) {
    case SXML_ASSEMBLY_PHASE_KEY:
        state->keys = value;
        state->phase = SXML_ASSEMBLY_PHASE_VALUE;
        return SXML_STATUS_OK;
    case SXML_ASSEMBLY_PHASE_VALUE:
        state->value = value;
        state->phase = SXML_ASSEMBLY_PHASE_INSERT;
        return SXML_STATUS_OK;
    case SXML_ASSEMBLY_PHASE_COMBINE: {
        const sxml_status status = assembly_put(
            state, call, (single_entry){.key = state->pending_key, .value = value});
        sxml_sequence_release(value);
        state->phase = SXML_ASSEMBLY_PHASE_INSERT;
        return status;
    }
    case SXML_ASSEMBLY_PHASE_NEXT:
    case SXML_ASSEMBLY_PHASE_INSERT:
        break;
    }
    sxml_sequence_release(value);
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Runs the assembly until it needs a call or is done. */
static sxml_status assembly_run(assembly_state *state, const sxml_function_call *call,
                                sxml_native_step *step) {
    const sxml_sequence key_function = map_argument(call, 1);
    const sxml_sequence value_function = map_argument(call, 2);
    for (;;) {
        sxml_status status = SXML_STATUS_OK;
        switch (state->phase) {
        case SXML_ASSEMBLY_PHASE_NEXT: {
            sxml_sequence_release(state->keys);
            sxml_sequence_release(state->value);
            state->keys = sxml_sequence_empty();
            state->value = sxml_sequence_empty();
            state->key_index = 0;
            assembly_progress progress = {.has_next = false, .needs_call = false};
            if (state->is_merge) {
                status = merge_next_entry(state, call, &progress);
                state->phase = SXML_ASSEMBLY_PHASE_INSERT;
            } else {
                sxml_iteration_step item_step = {.has_item = false};
                status = sxml_sequence_iterator_next(&state->iterator, call->error_record,
                                                     &item_step);
                progress.has_next = item_step.has_item;
                state->current_item = item_step.item;
                state->position += 1;
                state->phase = SXML_ASSEMBLY_PHASE_KEY;
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (!progress.has_next) {
                const sxml_item map = state->map;
                state->map = (sxml_item){.representation = 0};
                *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                                           .result = sxml_sequence_from_item(map)};
                return SXML_STATUS_OK;
            }
            break;
        }
        case SXML_ASSEMBLY_PHASE_KEY:
            if (!map_sequence_is_empty(key_function)) {
                return build_request(state, key_function.content, step);
            }
            status = build_default_keys(state, call);
            state->phase = SXML_ASSEMBLY_PHASE_VALUE;
            break;
        case SXML_ASSEMBLY_PHASE_VALUE:
            if (!map_sequence_is_empty(value_function)) {
                return build_request(state, value_function.content, step);
            }
            sxml_item_retain(state->current_item);
            state->value = sxml_sequence_from_item(state->current_item);
            state->phase = SXML_ASSEMBLY_PHASE_INSERT;
            break;
        case SXML_ASSEMBLY_PHASE_INSERT: {
            assembly_progress progress = {.has_next = false, .needs_call = false};
            status = assembly_insert(state, call, &progress);
            if (status == SXML_STATUS_OK && progress.needs_call) {
                state->phase = SXML_ASSEMBLY_PHASE_COMBINE;
                *step = (sxml_native_step){
                    .step_kind = SXML_NATIVE_STEP_KIND_CALL,
                    .result = sxml_sequence_empty(),
                    .function_item = state->combine_function,
                    .arguments = {.sequences = state->call_arguments, .sequence_count = 2}};
                return SXML_STATUS_OK;
            }
            state->phase = SXML_ASSEMBLY_PHASE_NEXT;
            break;
        }
        case SXML_ASSEMBLY_PHASE_COMBINE:
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
}

static sxml_status step_assembly(const sxml_native_step_request *request,
                                 sxml_native_step *step, assembly_kind kind) {
    assembly_state *state = request->state;
    const sxml_function_call *call = request->call;
    const sxml_status status =
        request->step_number == 0
            ? assembly_start(state, call, kind)
            : assembly_take_value(state, call, request->call_result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return assembly_run(state, call, step);
}

/* map:build($input, $key, $value, $options) (14.4.1). */
static sxml_status step_build(const sxml_native_step_request *request,
                              sxml_native_step *step) {
    return step_assembly(request, step, SXML_ASSEMBLY_KIND_BUILD);
}

/* map:merge($maps, $options) (14.4.12). */
static sxml_status step_merge(const sxml_native_step_request *request,
                              sxml_native_step *step) {
    return step_assembly(request, step, SXML_ASSEMBLY_KIND_MERGE);
}

static const sxml_resumable_native_body build_body = {
    .step = step_build,
    .release_state = release_assembly_state,
    .state_byte_count = sizeof(assembly_state),
    .state_alignment = alignof(assembly_state),
};

static const sxml_resumable_native_body merge_body = {
    .step = step_merge,
    .release_state = release_assembly_state,
    .state_byte_count = sizeof(assembly_state),
    .state_alignment = alignof(assembly_state),
};

/* ----------------------------------------------------------------- Rows */

SXML_TRAVERSAL_BODY(map_for_each_body, SXML_TRAVERSAL_OPERATION_MAP_FOR_EACH);
SXML_TRAVERSAL_BODY(map_filter_body, SXML_TRAVERSAL_OPERATION_MAP_FILTER);

#define PARAMETER SXML_HIGHER_ORDER_PARAMETER
#define OPTIONAL_PARAMETER SXML_HIGHER_ORDER_OPTIONAL_PARAMETER

static const sxml_function_parameter build_parameters[] = {
    PARAMETER("input", "item()*"),
    OPTIONAL_PARAMETER("key", "(fn(item(), xs:integer) as xs:anyAtomicType*)?"),
    OPTIONAL_PARAMETER("value", "(fn(item(), xs:integer) as item()*)?"),
    OPTIONAL_PARAMETER("options", "map(*)?")};
static const sxml_function_parameter map_parameters[] = {PARAMETER("map", "map(*)")};
static const sxml_function_parameter map_key_parameters[] = {
    PARAMETER("map", "map(*)"), PARAMETER("key", "xs:anyAtomicType")};
static const sxml_function_parameter entry_parameters[] = {
    PARAMETER("key", "xs:anyAtomicType"), PARAMETER("value", "item()*")};
static const sxml_function_parameter filter_parameters[] = {
    PARAMETER("map", "map(*)"),
    PARAMETER("predicate",
              "fn(xs:anyAtomicType, item()*, xs:integer) as xs:boolean?")};
static const sxml_function_parameter find_parameters[] = {
    PARAMETER("input", "item()*"), PARAMETER("key", "xs:anyAtomicType")};
static const sxml_function_parameter for_each_parameters[] = {
    PARAMETER("map", "map(*)"),
    PARAMETER("action", "fn(xs:anyAtomicType, item()*, xs:integer) as item()*")};
static const sxml_function_parameter get_parameters[] = {
    PARAMETER("map", "map(*)"), PARAMETER("key", "xs:anyAtomicType"),
    OPTIONAL_PARAMETER("default", "item()*")};
static const sxml_function_parameter merge_parameters[] = {
    PARAMETER("maps", "map(*)*"), OPTIONAL_PARAMETER("options", "map(*)?")};
static const sxml_function_parameter put_parameters[] = {
    PARAMETER("map", "map(*)"), PARAMETER("key", "xs:anyAtomicType"),
    PARAMETER("value", "item()*")};
static const sxml_function_parameter remove_parameters[] = {
    PARAMETER("map", "map(*)"), PARAMETER("keys", "xs:anyAtomicType*")};

#define RESUMABLE(name_text, parameter_array, result_text, body)               \
    SXML_HIGHER_ORDER_RESUMABLE_ROW(SXML_NAMESPACE_MAP_FUNCTIONS, name_text,   \
                                    parameter_array, result_text, body)
#define LEAF(name_text, parameter_array, result_text, body)                    \
    SXML_HIGHER_ORDER_LEAF_ROW(SXML_NAMESPACE_MAP_FUNCTIONS, name_text,        \
                               parameter_array, result_text, body)

static const sxml_native_function map_functions[] = {
    RESUMABLE("build", build_parameters, "map(*)", build_body),
    LEAF("contains", map_key_parameters, "xs:boolean", call_contains),
    LEAF("empty", map_parameters, "xs:boolean", call_empty),
    LEAF("entries", map_parameters, "map(*)*", call_entries),
    LEAF("entry", entry_parameters, "map(*)", call_entry),
    RESUMABLE("filter", filter_parameters, "map(*)", map_filter_body),
    LEAF("find", find_parameters, "array(*)", call_find),
    RESUMABLE("for-each", for_each_parameters, "item()*", map_for_each_body),
    LEAF("get", get_parameters, "item()*", call_get),
    LEAF("items", map_parameters, "item()*", call_items),
    LEAF("keys", map_parameters, "xs:anyAtomicType*", call_keys),
    RESUMABLE("merge", merge_parameters, "map(*)", merge_body),
    LEAF("put", put_parameters, "map(*)", call_put),
    LEAF("remove", remove_parameters, "map(*)", call_remove),
    LEAF("size", map_parameters, "xs:integer", call_size),
};

const sxml_native_function_view sxml_function_library_map = {
    .native_functions = map_functions,
    .native_function_count = sizeof(map_functions) / sizeof(map_functions[0]),
};
