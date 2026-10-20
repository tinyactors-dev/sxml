/*
 * Filter indexes at run time (src/evaluator_machine_internal.h; glossary
 * `filter_index`, `index_key`, `index_probe`; docs/DECISIONS.md D224).
 *
 * The machine's FILTER (src/evaluator.c) pushes the filter index task for
 * a FILTER with a filter index (src/ir_loop_passes.c) and an input of
 * SXML_FILTER_INDEX_MINIMUM_ITEM_COUNT items or more. The task finishes
 * with the candidates, the items of the input whose index key compares
 * with the index probe's value, in input order; the FILTER then tests its
 * predicate on them only. The task finishes with the input itself, so the
 * FILTER tests every item as it would without an index, whenever the
 * index cannot answer exactly:
 * - the first time a FILTER sees an input (the memo remembers the input;
 *   an index is built the second time, so a FILTER whose input changes
 *   every time never pays for one);
 * - when a key or the probe raises a dynamic or type error (the task is a
 *   catching task while they run): the predicate then raises it, or not,
 *   as it would;
 * - when a key or probe value is of a type the index does not order, or
 *   the default collation is not the codepoint collation.
 *
 * The index (XPath 4.0, 4.11.1 to 4.11.3). For "=" and "eq" every atomized
 * key value is an xs:string, xs:anyURI or xs:untypedAtomic (or a type
 * derived from one), and so is every atomized probe value: two such
 * values are equal under the general and the value comparison exactly
 * when their codepoints are, with the codepoint collation (an
 * xs:untypedAtomic operand is cast to xs:string). Or every key and probe
 * value is a number other than NaN (D229): numbers compare by exact
 * value, so two that compare equal have the same nearest double, so the keys of the probe's double are the candidates
 * (integers beyond 2^53 may share one; the predicate decides). For "eq" a
 * key and the probe have at most one value. For "is" a key and the probe
 * are each empty or one XNode or JNode, compared by identity. The index
 * is the pairs (key value, item position) ordered by key (codepoints,
 * doubles, document order for nodes), as two vectors in an array kept in
 * the evaluation memo under the FILTER with its input; a probe is a
 * binary search.
 *
 * Steps (D89): the keys are evaluated on the machine, a step each as
 * usual; building charges a step per key value, and a probe a step per
 * probe value and per candidate.
 *
 * Golden file: src/evaluator_try.c (a part task's state, phases and
 * catching on the machine API).
 */
#include "evaluator_machine_internal.h"

#include "collation_internal.h"
#include "function_library_core_internal.h"
#include "memory_internal.h"
#include "operator_internal.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>

/* What the memo keeps for a FILTER, as the first members of its array. */
typedef enum filter_index_mode {
    /* The input was seen once: no index yet. */
    SXML_FILTER_INDEX_MODE_SEEN = 0,
    /* The index of the input: keys and positions follow. */
    SXML_FILTER_INDEX_MODE_INDEXED = 1,
    /* The input's keys cannot be indexed: test every item. */
    SXML_FILTER_INDEX_MODE_UNUSABLE = 2,
} filter_index_mode;

/* The members of the memo's array. */
enum {
    SXML_FILTER_INDEX_MEMBER_INPUT = 0,
    SXML_FILTER_INDEX_MEMBER_MODE = 1,
    SXML_FILTER_INDEX_MEMBER_KEYS = 2,
    SXML_FILTER_INDEX_MEMBER_POSITIONS = 3,
    /* The number view of an index of xs:untypedAtomic keys (see "Numbers
       against untyped keys"): its number_view_mode, then its keys (the
       nearest doubles of the keys cast to numbers) and their positions. */
    SXML_FILTER_INDEX_MEMBER_NUMBER_VIEW_MODE = 4,
    SXML_FILTER_INDEX_MEMBER_NUMBER_KEYS = 5,
    SXML_FILTER_INDEX_MEMBER_NUMBER_POSITIONS = 6,
    /* True when an index of numbers has an xs:float key. */
    SXML_FILTER_INDEX_MEMBER_HAS_FLOAT_KEYS = 7,
    SXML_FILTER_INDEX_MEMBER_COUNT = 8,
};

/* The number view of an index (SXML_FILTER_INDEX_MEMBER_NUMBER_VIEW_MODE). */
typedef enum number_view_mode {
    /* Not made yet: made at the first numeric probe value. */
    SXML_NUMBER_VIEW_MODE_ABSENT = 0,
    SXML_NUMBER_VIEW_MODE_MADE = 1,
    /* A key is no xs:untypedAtomic or cannot be cast to a number: the
       comparison with a number raises an error or is not the cast's. */
    SXML_NUMBER_VIEW_MODE_UNUSABLE = 2,
} number_view_mode;

typedef enum filter_index_phase {
    SXML_FILTER_INDEX_PHASE_START = 0,
    /* The key of the item at position is being evaluated. */
    SXML_FILTER_INDEX_PHASE_KEY = 1,
    /* The probe is being evaluated. */
    SXML_FILTER_INDEX_PHASE_PROBE = 2,
} filter_index_phase;

/* One key value and the position (from 0) of its item. */
typedef struct index_entry {
    /* Owned: an atomic item for "=" and "eq", a node for "is". */
    sxml_item key;
    uint64_t position;
} index_entry;

/* The kinds of values an index orders; one index holds one kind. */
typedef enum key_class {
    /* Not indexed: the FILTER tests every item. */
    SXML_KEY_CLASS_NONE,
    /* xs:string, xs:anyURI, xs:untypedAtomic: by codepoints. */
    SXML_KEY_CLASS_STRING,
    /* xs:decimal (with the integers), xs:float and xs:double, not NaN:
       by the double nearest to each. */
    SXML_KEY_CLASS_NUMBER,
    /* XNodes and JNodes: by document order. */
    SXML_KEY_CLASS_NODE,
} key_class;

typedef struct filter_index_state {
    const sxml_ir_filter *filter;
    /* Owned. */
    sxml_sequence input;
    sxml_sequence_iterator iterator;
    uint64_t position;
    /* The entries being built, in allocator memory. */
    index_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    /* False once a key cannot be indexed. */
    bool is_usable;
    /* The class of the keys so far (NONE before the first). */
    key_class key_class;
    /* The memo's array for the FILTER and this input, retained (a FILTER
       evaluated again inside its own key or probe may replace the memo's
       entry); a zero item until there is one. */
    sxml_item index_array;
} filter_index_state;

static void release_entries(filter_index_state *state, sxml_allocator allocator) {
    for (size_t entry_index = 0; entry_index < state->entry_count; entry_index += 1) {
        sxml_item_release(state->entries[entry_index].key);
    }
    if (state->entries != NULL) {
        sxml_allocator_release(allocator, state->entries);
    }
    state->entries = NULL;
    state->entry_count = 0;
    state->entry_capacity = 0;
}

static void release_filter_index_state(sxml_dynamic_context *dynamic_context, void *memory) {
    filter_index_state *state = memory;
    release_entries(state, dynamic_context->allocator);
    sxml_sequence_release(state->input);
    sxml_item_release(state->index_array);
    sxml_allocator_release(dynamic_context->allocator, state);
}

static filter_index_state *state_of(sxml_evaluator_task *task) {
    return task->details.part.state;
}

/* ------------------------------------------------------ Kinds of keys */

/* The double nearest to a number of the NUMBER class. */
static double number_of(sxml_item item) {
    const sxml_item_representation representation = sxml_item_get_representation(item);
    if (representation == SXML_ITEM_REPRESENTATION_INTEGER) {
        return (double)item.payload.integer;
    }
    if (representation == SXML_ITEM_REPRESENTATION_DOUBLE) {
        return item.payload.double_number;
    }
    if (representation == SXML_ITEM_REPRESENTATION_FLOAT) {
        return (double)item.payload.float_number;
    }
    const sxml_decimal *decimal = sxml_atomic_item_get_decimal(item);
    return decimal != NULL ? sxml_decimal_to_double(decimal) : 0.0;
}

/*
 * The class of an atomic value "=" and "eq" compare (4.11.1, 4.11.2):
 * strings by codepoints; xs:decimal, the integers, xs:float and xs:double
 * as numbers. Numbers of different types compare by their exact values
 * (F&O 4.0, fn:compare; PR2218, D57; src/operator_compare.c), so two that
 * are equal are one real number with one nearest double: a probe finds
 * every key equal to it among the keys of its double, and the predicate
 * decides among them (integers beyond 2^53 share doubles, and so do
 * decimals close together). NaN equals nothing and is not indexed.
 */
static key_class atomic_class(sxml_item item) {
    const uint32_t primitive = sxml_atomic_item_get_primitive_type(item).index;
    if (primitive == SXML_TYPE_XS_STRING || primitive == SXML_TYPE_XS_ANY_URI ||
        primitive == SXML_TYPE_XS_UNTYPED_ATOMIC) {
        return SXML_KEY_CLASS_STRING;
    }
    if (primitive == SXML_TYPE_XS_DECIMAL ||
        (primitive == SXML_TYPE_XS_DOUBLE && !isnan(item.payload.double_number)) ||
        (primitive == SXML_TYPE_XS_FLOAT && !isnan(item.payload.float_number))) {
        return SXML_KEY_CLASS_NUMBER;
    }
    return SXML_KEY_CLASS_NONE;
}

static sxml_byte_view string_bytes(sxml_item item) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return datum.datum_details.string.utf8;
}

/* The order of two keys of one class: codepoints for strings (UTF-8
   bytes order as codepoints do), nearest doubles for numbers, document
   order for nodes. */
static sxml_ordering compare_keys(sxml_item first, sxml_item second) {
    if (sxml_item_is_atomic(first) && atomic_class(first) == SXML_KEY_CLASS_NUMBER) {
        const double first_number = number_of(first);
        const double second_number = number_of(second);
        if (first_number < second_number) {
            return SXML_ORDERING_LESS;
        }
        return first_number > second_number ? SXML_ORDERING_GREATER : SXML_ORDERING_EQUAL;
    }
    if (sxml_item_is_generic_node(first)) {
        sxml_ordering ordering = SXML_ORDERING_EQUAL;
        if (sxml_generic_node_compare_document_order(
                (sxml_item_pair){.first = first, .second = second}, &ordering) != SXML_STATUS_OK) {
            return SXML_ORDERING_EQUAL;
        }
        return ordering;
    }
    const sxml_byte_view first_bytes = string_bytes(first);
    const sxml_byte_view second_bytes = string_bytes(second);
    const size_t common = first_bytes.byte_count < second_bytes.byte_count
                              ? first_bytes.byte_count
                              : second_bytes.byte_count;
    const int compared = common == 0 ? 0 : memcmp(first_bytes.bytes, second_bytes.bytes, common);
    if (compared != 0) {
        return compared < 0 ? SXML_ORDERING_LESS : SXML_ORDERING_GREATER;
    }
    if (first_bytes.byte_count == second_bytes.byte_count) {
        return SXML_ORDERING_EQUAL;
    }
    return first_bytes.byte_count < second_bytes.byte_count ? SXML_ORDERING_LESS
                                                            : SXML_ORDERING_GREATER;
}

/* True when the default collation is the codepoint collation, so strings
   compare as compare_keys orders them. */
static sxml_status uses_codepoint_collation(sxml_dynamic_context *dynamic_context,
                                            bool *is_codepoint) {
    sxml_collation collation = sxml_collation_codepoint();
    const sxml_function_call call = {.dynamic_context = dynamic_context,
                                     .item_heap = dynamic_context->item_heap,
                                     .error_record = dynamic_context->error_record};
    const sxml_status status = sxml_function_library_find_collation(
        &call, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &collation);
    *is_codepoint = status == SXML_STATUS_OK && sxml_collation_is_codepoint_order(&collation);
    return status;
}

/* A dynamic or type error, which the index answers by testing every item
   instead (the predicate raises it, or not, as it would). */
static bool is_catchable(sxml_status status) {
    return status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR;
}

/* A key's or the probe's value (borrowed) under the FILTER's operator. */
typedef struct operand_value {
    sxml_ir_binary_operator key_operator;
    sxml_sequence value;
} operand_value;

/* The values a key or the probe compares: atomized for "=" and "eq"
   (4.11.1, 4.11.2), as they are for "is" (4.11.3). Owned. */
static sxml_status comparison_values(sxml_dynamic_context *dynamic_context,
                                     operand_value operand, sxml_sequence *values) {
    if (operand.key_operator == SXML_IR_BINARY_OPERATOR_IS) {
        sxml_sequence_retain(operand.value);
        *values = operand.value;
        return SXML_STATUS_OK;
    }
    return sxml_evaluator_atomize(dynamic_context,
                                  &(sxml_atomization_request){
                                      .value = operand.value,
                                      .error_record = dynamic_context->error_record},
                                  values);
}

/* The class of a key or probe value under the operator. */
static key_class value_class(sxml_ir_binary_operator key_operator, sxml_item item) {
    if (key_operator == SXML_IR_BINARY_OPERATOR_IS) {
        return sxml_item_is_generic_node(item) ? SXML_KEY_CLASS_NODE : SXML_KEY_CLASS_NONE;
    }
    return sxml_item_is_atomic(item) ? atomic_class(item) : SXML_KEY_CLASS_NONE;
}

/* --------------------------------------------------------- The memo */

/* The memo key of a FILTER: one entry per FILTER, holding the last input
   it saw (the subject is the same for all). */
static sxml_evaluation_memo_key memo_key_of(const sxml_ir_filter *filter) {
    return (sxml_evaluation_memo_key){.computation = filter,
                                      .computation_number = 0,
                                      .subject = sxml_atomic_item_from_integer(0)};
}

/* True when two inputs have the same items: the same value, or the same
   nodes and atomic-equal atomic items of one type, one by one. */
static bool inputs_are_same(sxml_sequence first, sxml_sequence second) {
    if (first.content.representation == second.content.representation &&
        first.content.payload.bits == second.content.payload.bits &&
        first.content.type_or_node_index == second.content.type_or_node_index) {
        return true;
    }
    uint64_t first_count = 0;
    uint64_t second_count = 0;
    if (sxml_sequence_count(first, NULL, &first_count) != SXML_STATUS_OK ||
        sxml_sequence_count(second, NULL, &second_count) != SXML_STATUS_OK ||
        first_count != second_count) {
        return false;
    }
    sxml_sequence_iterator first_iterator = sxml_sequence_iterator_start(first);
    sxml_sequence_iterator second_iterator = sxml_sequence_iterator_start(second);
    for (;;) {
        sxml_iteration_step first_step;
        sxml_iteration_step second_step;
        if (sxml_sequence_iterator_next(&first_iterator, NULL, &first_step) != SXML_STATUS_OK ||
            sxml_sequence_iterator_next(&second_iterator, NULL, &second_step) != SXML_STATUS_OK) {
            return false;
        }
        if (!first_step.has_item || !second_step.has_item) {
            return first_step.has_item == second_step.has_item;
        }
        const sxml_item_pair pair = {.first = first_step.item, .second = second_step.item};
        bool is_same = false;
        if (sxml_item_is_generic_node(pair.first)) {
            is_same = sxml_generic_node_is_same_node(pair);
        } else if (sxml_item_get_kind(pair.first) == SXML_ITEM_KIND_ATOMIC &&
                   sxml_item_get_kind(pair.second) == SXML_ITEM_KIND_ATOMIC) {
            is_same = sxml_item_get_type_annotation(pair.first).index ==
                          sxml_item_get_type_annotation(pair.second).index &&
                      sxml_atomic_item_is_atomic_equal(pair.first, pair.second);
        } else {
            is_same = pair.first.representation == pair.second.representation &&
                      pair.first.payload.bits == pair.second.payload.bits;
        }
        if (!is_same) {
            return false;
        }
    }
}

/* Keeps array (retained) as the task's memo array. */
static void keep_index_array(filter_index_state *state, sxml_item array) {
    sxml_item_retain(array);
    sxml_item_release(state->index_array);
    state->index_array = array;
}

/* The keys of an index in their order and the positions of their items,
   borrowed. */
typedef struct index_view {
    sxml_sequence keys;
    sxml_sequence positions;
} index_view;

static index_view index_view_empty(void) {
    return (index_view){.keys = sxml_sequence_empty(), .positions = sxml_sequence_empty()};
}

/* What the memo's array for the FILTER holds besides its input: the mode,
   the index (empty unless indexed) and its number view. */
typedef struct memo_index {
    filter_index_mode mode;
    index_view view;
    number_view_mode number_view_mode;
    index_view number_view;
    bool has_float_keys;
} memo_index;

/* Stores the memo's array for the FILTER and keeps it in the task. */
static sxml_status store_index(sxml_dynamic_context *dynamic_context, filter_index_state *state,
                               memo_index index) {
    const sxml_sequence members[SXML_FILTER_INDEX_MEMBER_COUNT] = {
        state->input,
        sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)index.mode)),
        index.view.keys,
        index.view.positions,
        sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)index.number_view_mode)),
        index.number_view.keys,
        index.number_view.positions,
        sxml_sequence_from_item(index.has_float_keys ? sxml_atomic_item_true()
                                                     : sxml_atomic_item_false()),
    };
    sxml_item array = {.representation = 0};
    sxml_status status = sxml_array_make(
        dynamic_context->item_heap,
        (sxml_sequence_view){.sequences = members, .sequence_count = SXML_FILTER_INDEX_MEMBER_COUNT},
        &array);
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_store_memo(
            dynamic_context,
            &(sxml_evaluation_memo_entry){.key = memo_key_of(state->filter),
                                          .value = sxml_sequence_from_item(array)});
        if (status == SXML_STATUS_OK) {
            keep_index_array(state, array);
        }
        sxml_item_release(array);
    }
    return status;
}

/* A memo index without keys. */
static memo_index memo_index_without_keys(filter_index_mode mode) {
    return (memo_index){.mode = mode,
                        .view = index_view_empty(),
                        .number_view_mode = SXML_NUMBER_VIEW_MODE_ABSENT,
                        .number_view = index_view_empty(),
                        .has_float_keys = false};
}

/* The member of the memo's array at member_index, borrowed. */
static sxml_sequence index_member(sxml_item array, size_t member_index) {
    sxml_sequence member = sxml_sequence_empty();
    if (sxml_array_get_member(array, member_index, &member) != SXML_STATUS_OK) {
        return sxml_sequence_empty();
    }
    return member;
}

static filter_index_mode index_mode(sxml_item array) {
    const sxml_sequence mode = index_member(array, SXML_FILTER_INDEX_MEMBER_MODE);
    return (filter_index_mode)mode.content.payload.integer;
}

/* ---------------------------------------------------------- Building */

/* Ends the task with the input: the FILTER tests every item. */
static sxml_status finish_with_input(sxml_dynamic_context *dynamic_context,
                                     filter_index_state *state) {
    const sxml_sequence input = state->input;
    state->input = sxml_sequence_empty();
    return sxml_evaluator_machine_finish(dynamic_context, input);
}

/* Records that the input's keys cannot be indexed and ends the task with
   the input. */
static sxml_status give_up(sxml_dynamic_context *dynamic_context, filter_index_state *state) {
    sxml_status status = store_index(
        dynamic_context, state,
        memo_index_without_keys(SXML_FILTER_INDEX_MODE_UNUSABLE));
    if (status == SXML_STATUS_OK) {
        status = finish_with_input(dynamic_context, state);
    }
    return status;
}

static sxml_status append_entry(sxml_dynamic_context *dynamic_context, filter_index_state *state,
                                index_entry entry) {
    if (state->entry_count == state->entry_capacity) {
        void *elements = state->entries;
        const sxml_status status = sxml_evaluator_grow(
            dynamic_context->allocator,
            &(sxml_evaluator_growth){.elements = &elements,
                                     .element_capacity = &state->entry_capacity,
                                     .element_count = state->entry_count,
                                     .required_count = state->entry_count + 1,
                                     .element_byte_count = sizeof(index_entry),
                                     .alignment = alignof(index_entry)});
        if (status != SXML_STATUS_OK) {
            sxml_item_release(entry.key);
            return status;
        }
        state->entries = elements;
    }
    state->entries[state->entry_count] = entry;
    state->entry_count += 1;
    return SXML_STATUS_OK;
}

/* Binds the next item as the focus value and evaluates its key; at the
   end, makes the index and evaluates the probe. */
static sxml_status next_key(sxml_dynamic_context *dynamic_context, sxml_evaluator_task *task);

/* Adds the values of the key just evaluated (owned) for the item at
   state->position. */
static sxml_status add_key_values(sxml_dynamic_context *dynamic_context,
                                  filter_index_state *state, sxml_sequence key) {
    const sxml_ir_binary_operator key_operator = state->filter->index.key_operator;
    sxml_sequence values = sxml_sequence_empty();
    sxml_status status = comparison_values(
        dynamic_context, (operand_value){.key_operator = key_operator, .value = key}, &values);
    sxml_sequence_release(key);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint64_t value_count = 0;
    status = sxml_sequence_count(values, dynamic_context->error_record, &value_count);
    if (status == SXML_STATUS_OK && value_count > 1 &&
        key_operator != SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL) {
        /* "eq" and "is" raise XPTY0004 for more than one value: the
           predicate does, as it would. */
        state->is_usable = false;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_charge_steps(dynamic_context, value_count);
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(values);
    while (status == SXML_STATUS_OK && state->is_usable) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, dynamic_context->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        const key_class found_class = value_class(key_operator, step.item);
        if (found_class == SXML_KEY_CLASS_NONE ||
            (state->key_class != SXML_KEY_CLASS_NONE && found_class != state->key_class)) {
            /* One index holds one class: keys of two classes compare
               otherwise (an xs:untypedAtomic with a number is cast) or
               raise an error, which the predicate then does. */
            state->is_usable = false;
            break;
        }
        state->key_class = found_class;
        sxml_item_retain(step.item);
        status = append_entry(dynamic_context, state,
                              (index_entry){.key = step.item, .position = state->position - 1});
    }
    sxml_sequence_release(values);
    return status;
}

/* Orders two entries by key, then position. */
static sxml_status compare_entries(void *memory, sxml_index_pair pair, sxml_ordering *ordering) {
    const filter_index_state *state = memory;
    const index_entry *first = &state->entries[pair.first_index];
    const index_entry *second = &state->entries[pair.second_index];
    *ordering = compare_keys(first->key, second->key);
    if (*ordering == SXML_ORDERING_EQUAL && first->position != second->position) {
        *ordering = first->position < second->position ? SXML_ORDERING_LESS
                                                       : SXML_ORDERING_GREATER;
    }
    return SXML_STATUS_OK;
}

/* Sorts the entries of the task by key, then position, into two vectors
   (owned, empty for no entries), and releases the entries. */
static sxml_status sort_entries(sxml_dynamic_context *dynamic_context, filter_index_state *state,
                                index_view *sorted) {
    const sxml_allocator allocator = dynamic_context->allocator;
    const size_t entry_count = state->entry_count;
    *sorted = index_view_empty();
    if (entry_count == 0) {
        release_entries(state, allocator);
        return SXML_STATUS_OK;
    }
    size_t *order = NULL;
    sxml_item *keys = NULL;
    sxml_item *positions = NULL;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = entry_count,
                                        .element_size = sizeof(size_t),
                                        .element_alignment = alignof(size_t)},
        &memory);
    order = memory;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            allocator,
            (sxml_array_allocation_request){.element_count = entry_count * 2,
                                            .element_size = sizeof(sxml_item),
                                            .element_alignment = alignof(sxml_item)},
            &memory);
        keys = memory;
    }
    if (status == SXML_STATUS_OK && (order == NULL || keys == NULL)) {
        /* Only an array of no elements is NULL. */
        status = SXML_STATUS_OUT_OF_MEMORY;
    }
    if (status == SXML_STATUS_OK) {
        positions = keys + entry_count;
        for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
            order[entry_index] = entry_index;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_sort_indices(
            allocator, &(sxml_index_sort_request){.indices = order,
                                                  .index_count = entry_count,
                                                  .compare = compare_entries,
                                                  .compare_state = state});
    }
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && positions != NULL && entry_index < entry_count;
         entry_index += 1) {
        const index_entry *entry = &state->entries[order[entry_index]];
        keys[entry_index] = entry->key;
        positions[entry_index] = sxml_atomic_item_from_integer((int64_t)entry->position);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            dynamic_context->item_heap,
            (sxml_item_view){.items = keys, .item_count = entry_count}, &sorted->keys);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            dynamic_context->item_heap,
            (sxml_item_view){.items = positions, .item_count = entry_count}, &sorted->positions);
    }
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(sorted->keys);
        sxml_sequence_release(sorted->positions);
        *sorted = index_view_empty();
    }
    if (order != NULL) {
        sxml_allocator_release(allocator, order);
    }
    if (keys != NULL) {
        sxml_allocator_release(allocator, keys);
    }
    release_entries(state, allocator);
    return status;
}

/* Sorts the entries and stores them as the index of the input. */
static sxml_status store_entries(sxml_dynamic_context *dynamic_context,
                                 filter_index_state *state) {
    bool has_float_keys = false;
    for (size_t entry_index = 0; entry_index < state->entry_count; entry_index += 1) {
        const sxml_item key = state->entries[entry_index].key;
        has_float_keys = has_float_keys ||
                         (sxml_item_is_atomic(key) &&
                          sxml_atomic_item_get_primitive_type(key).index == SXML_TYPE_XS_FLOAT);
    }
    index_view sorted = index_view_empty();
    sxml_status status = sort_entries(dynamic_context, state, &sorted);
    if (status == SXML_STATUS_OK) {
        status = store_index(dynamic_context, state,
                             (memo_index){.mode = SXML_FILTER_INDEX_MODE_INDEXED,
                                          .view = sorted,
                                          .number_view_mode = SXML_NUMBER_VIEW_MODE_ABSENT,
                                          .number_view = index_view_empty(),
                                          .has_float_keys = has_float_keys});
    }
    sxml_sequence_release(sorted.keys);
    sxml_sequence_release(sorted.positions);
    return status;
}

/* Evaluates the probe, as a catching task. */
static sxml_status start_probe(sxml_dynamic_context *dynamic_context, sxml_evaluator_task *task) {
    const filter_index_state *state = state_of(task);
    task->phase = SXML_FILTER_INDEX_PHASE_PROBE;
    task->catches_errors = true;
    return sxml_evaluator_machine_push_expression(dynamic_context, state->filter->index.probe);
}

static sxml_status next_key(sxml_dynamic_context *dynamic_context, sxml_evaluator_task *task) {
    filter_index_state *state = state_of(task);
    sxml_iteration_step step;
    sxml_status status =
        sxml_sequence_iterator_next(&state->iterator, dynamic_context->error_record, &step);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!step.has_item) {
        task->catches_errors = false;
        if (!state->is_usable) {
            return give_up(dynamic_context, state);
        }
        status = store_entries(dynamic_context, state);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return start_probe(dynamic_context, task);
    }
    state->position += 1;
    sxml_item_retain(step.item);
    sxml_evaluator_machine_bind_slot(dynamic_context, state->filter->focus.context_value,
                                     sxml_sequence_from_item(step.item));
    task->phase = SXML_FILTER_INDEX_PHASE_KEY;
    task->catches_errors = true;
    return sxml_evaluator_machine_push_expression(dynamic_context, state->filter->index.key);
}

/* Starts building the index of the input. */
static sxml_status start_building(sxml_dynamic_context *dynamic_context,
                                  sxml_evaluator_task *task) {
    filter_index_state *state = state_of(task);
    bool is_codepoint = false;
    sxml_status status = SXML_STATUS_OK;
    if (state->filter->index.key_operator != SXML_IR_BINARY_OPERATOR_IS) {
        status = uses_codepoint_collation(dynamic_context, &is_codepoint);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!is_codepoint) {
            return give_up(dynamic_context, state);
        }
    }
    state->iterator = sxml_sequence_iterator_start(state->input);
    state->position = 0;
    state->is_usable = true;
    state->key_class = SXML_KEY_CLASS_NONE;
    return next_key(dynamic_context, task);
}

/* ---------------------------------------------------------- Probing */

/* The first position of the sorted keys not ordered before value. */
typedef struct key_search {
    sxml_sequence keys;
    uint64_t key_count;
    sxml_item value;
} key_search;

static uint64_t lower_bound(key_search search) {
    uint64_t low = 0;
    uint64_t high = search.key_count;
    while (low < high) {
        const uint64_t middle = low + (high - low) / 2;
        sxml_item key = {.representation = 0};
        if (sxml_sequence_get_item(search.keys,
                                   (sxml_item_request){.item_index = middle, .error_record = NULL},
                                   &key) != SXML_STATUS_OK) {
            return search.key_count;
        }
        if (compare_keys(key, search.value) == SXML_ORDERING_LESS) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/* Positions (from 0) of the candidates, collected per probe value. */
typedef struct candidate_positions {
    size_t *positions;
    size_t position_count;
    size_t position_capacity;
} candidate_positions;

/* A probe value to look up in one view of an index. */
typedef struct probe_lookup {
    index_view view;
    sxml_item value;
} probe_lookup;

static sxml_status add_candidates_of(sxml_dynamic_context *dynamic_context, probe_lookup lookup,
                                     candidate_positions *candidates) {
    const sxml_item value = lookup.value;
    const sxml_sequence keys = lookup.view.keys;
    const sxml_sequence positions = lookup.view.positions;
    uint64_t key_count = 0;
    sxml_status status = sxml_sequence_count(keys, NULL, &key_count);
    uint64_t key_index = status == SXML_STATUS_OK
                             ? lower_bound((key_search){.keys = keys,
                                                        .key_count = key_count,
                                                        .value = value})
                             : key_count;
    while (status == SXML_STATUS_OK && key_index < key_count) {
        sxml_item key = {.representation = 0};
        sxml_item position = {.representation = 0};
        status = sxml_sequence_get_item(
            keys, (sxml_item_request){.item_index = key_index, .error_record = NULL}, &key);
        if (status != SXML_STATUS_OK || compare_keys(key, value) != SXML_ORDERING_EQUAL) {
            break;
        }
        status = sxml_sequence_get_item(
            positions, (sxml_item_request){.item_index = key_index, .error_record = NULL},
            &position);
        if (status == SXML_STATUS_OK && candidates->position_count == candidates->position_capacity) {
            void *elements = candidates->positions;
            status = sxml_evaluator_grow(
                dynamic_context->allocator,
                &(sxml_evaluator_growth){.elements = &elements,
                                         .element_capacity = &candidates->position_capacity,
                                         .element_count = candidates->position_count,
                                         .required_count = candidates->position_count + 1,
                                         .element_byte_count = sizeof(size_t),
                                         .alignment = alignof(size_t)});
            candidates->positions = elements;
        }
        if (status == SXML_STATUS_OK) {
            candidates->positions[candidates->position_count] = (size_t)position.payload.integer;
            candidates->position_count += 1;
        }
        key_index += 1;
    }
    return status;
}

/* Orders two candidate positions. */
static sxml_status compare_positions(void *memory, sxml_index_pair pair, sxml_ordering *ordering) {
    const size_t *positions = memory;
    const size_t first = positions[pair.first_index];
    const size_t second = positions[pair.second_index];
    *ordering = first < second ? SXML_ORDERING_LESS
                               : (first > second ? SXML_ORDERING_GREATER : SXML_ORDERING_EQUAL);
    return SXML_STATUS_OK;
}

/* The positions of the candidates found and the input they are of. */
typedef struct candidate_request {
    const filter_index_state *state;
    const candidate_positions *candidates;
} candidate_request;

/* The candidates' items, in input order without repeats, owned. */
static sxml_status make_candidates(sxml_dynamic_context *dynamic_context,
                                   candidate_request request, sxml_sequence *result) {
    const filter_index_state *state = request.state;
    const candidate_positions *candidates = request.candidates;
    const sxml_allocator allocator = dynamic_context->allocator;
    const size_t count = candidates->position_count;
    if (count == 0) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = count,
                                        .element_size = sizeof(size_t),
                                        .element_alignment = alignof(size_t)},
        &memory);
    size_t *order = memory;
    sxml_item *items = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            allocator,
            (sxml_array_allocation_request){.element_count = count,
                                            .element_size = sizeof(sxml_item),
                                            .element_alignment = alignof(sxml_item)},
            &memory);
        items = memory;
    }
    for (size_t index = 0; status == SXML_STATUS_OK && index < count; index += 1) {
        order[index] = index;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_sort_indices(
            allocator, &(sxml_index_sort_request){.indices = order,
                                                  .index_count = count,
                                                  .compare = compare_positions,
                                                  .compare_state = candidates->positions});
    }
    size_t item_count = 0;
    size_t previous = SIZE_MAX;
    for (size_t index = 0; status == SXML_STATUS_OK && index < count; index += 1) {
        const size_t position = candidates->positions[order[index]];
        if (position == previous) {
            continue;
        }
        previous = position;
        status = sxml_sequence_get_item(
            state->input,
            (sxml_item_request){.item_index = position, .error_record = dynamic_context->error_record},
            &items[item_count]);
        item_count += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_charge_steps(dynamic_context, item_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            dynamic_context->item_heap, (sxml_item_view){.items = items, .item_count = item_count},
            result);
    }
    if (order != NULL) {
        sxml_allocator_release(allocator, order);
    }
    if (items != NULL) {
        sxml_allocator_release(allocator, items);
    }
    return status;
}

/* The class of an index's keys: its first key's; NONE for no keys. */
static key_class index_key_class(const filter_index_state *state) {
    const sxml_sequence keys = index_member(state->index_array, SXML_FILTER_INDEX_MEMBER_KEYS);
    sxml_item first_key = {.representation = 0};
    if (sxml_sequence_get_item(keys, (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &first_key) != SXML_STATUS_OK) {
        return SXML_KEY_CLASS_NONE;
    }
    return value_class(state->filter->index.key_operator, first_key);
}

static index_view index_view_of(const filter_index_state *state, size_t keys_member) {
    return (index_view){.keys = index_member(state->index_array, keys_member),
                        .positions = index_member(state->index_array, keys_member + 1)};
}

/* ------------------------------------- Numbers against untyped keys */

/*
 * "=" compares an xs:untypedAtomic value with a number by casting it to
 * the number's primitive type, or to xs:double when that fails (XPath 4.0,
 * 4.11.2; src/operator_compare.c), as in //order[@id = $order/@id + 1].
 * An index of strings whose keys are all xs:untypedAtomic answers such a
 * probe from its number view: the keys cast as the comparison casts them
 * against an xs:decimal (to xs:decimal, else xs:double), by nearest double,
 * made at the first numeric probe value and kept in the memo with the
 * index. A key cast to xs:decimal and to xs:double has one nearest double
 * (both casts are exact or correctly rounded), so the view finds the keys
 * equal to an xs:decimal, integer or xs:double probe value among the keys
 * of its double; the predicate decides among them. An xs:float probe value
 * (the key is cast to xs:float, whose double may differ) is not answered.
 * The view is unusable, and every item is tested, when a key is no
 * xs:untypedAtomic (an xs:string with a number is XPTY0004) or cannot be
 * cast (FORG0001): the predicate raises the error, or not, as it would.
 * NaN equals nothing and is left out. The other way round, an
 * xs:untypedAtomic probe value against an index of numbers is cast to
 * xs:decimal, else xs:double, and looked up by its double, unless a key is
 * an xs:float. Making the view charges a step per key.
 */

/* Casts an xs:untypedAtomic item as "=" casts it against an xs:decimal:
   to xs:decimal, else xs:double. Owned; false when neither cast works. */
typedef struct number_cast {
    bool is_cast;
    sxml_item number;
} number_cast;

static sxml_status cast_to_number(sxml_dynamic_context *dynamic_context, sxml_item untyped,
                                  number_cast *cast) {
    cast->is_cast = false;
    cast->number = (sxml_item){.representation = 0};
    sxml_operator_cast request = {
        .item = untyped,
        .target_type = SXML_TYPE_XS_DECIMAL,
        .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
        .error_record = NULL,
    };
    sxml_status status =
        sxml_operator_cast_item(dynamic_context->item_heap, &request, &cast->number);
    if (is_catchable(status)) {
        request.target_type = SXML_TYPE_XS_DOUBLE;
        status = sxml_operator_cast_item(dynamic_context->item_heap, &request, &cast->number);
    }
    if (is_catchable(status)) {
        return SXML_STATUS_OK;
    }
    cast->is_cast = status == SXML_STATUS_OK;
    return status;
}

static number_view_mode stored_number_view_mode(const filter_index_state *state) {
    const sxml_sequence mode =
        index_member(state->index_array, SXML_FILTER_INDEX_MEMBER_NUMBER_VIEW_MODE);
    return (number_view_mode)mode.content.payload.integer;
}

/* A number view to store: its mode and its keys. */
typedef struct number_view {
    number_view_mode mode;
    index_view view;
} number_view;

/* Stores the index again with its number view. */
static sxml_status store_number_view(sxml_dynamic_context *dynamic_context,
                                     filter_index_state *state, number_view made) {
    const sxml_sequence has_float_keys =
        index_member(state->index_array, SXML_FILTER_INDEX_MEMBER_HAS_FLOAT_KEYS);
    return store_index(dynamic_context, state,
                       (memo_index){.mode = SXML_FILTER_INDEX_MODE_INDEXED,
                                    .view = index_view_of(state, SXML_FILTER_INDEX_MEMBER_KEYS),
                                    .number_view_mode = made.mode,
                                    .number_view = made.view,
                                    .has_float_keys = has_float_keys.content.payload.integer != 0});
}

/* Whether the number view can be used. */
typedef struct number_view_outcome {
    bool is_usable;
} number_view_outcome;

/* Makes the number view of the index (once) and writes whether it can be
   used. */
static sxml_status make_number_view(sxml_dynamic_context *dynamic_context,
                                    filter_index_state *state, number_view_outcome *outcome) {
    bool *is_usable = &outcome->is_usable;
    number_view_mode mode = stored_number_view_mode(state);
    if (mode != SXML_NUMBER_VIEW_MODE_ABSENT) {
        *is_usable = mode == SXML_NUMBER_VIEW_MODE_MADE;
        return SXML_STATUS_OK;
    }
    const index_view view = index_view_of(state, SXML_FILTER_INDEX_MEMBER_KEYS);
    uint64_t key_count = 0;
    sxml_status status = sxml_sequence_count(view.keys, NULL, &key_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_charge_steps(dynamic_context, key_count);
    }
    mode = SXML_NUMBER_VIEW_MODE_MADE;
    for (uint64_t key_index = 0; status == SXML_STATUS_OK && key_index < key_count;
         key_index += 1) {
        sxml_item key = {.representation = 0};
        sxml_item position = {.representation = 0};
        status = sxml_sequence_get_item(
            view.keys, (sxml_item_request){.item_index = key_index, .error_record = NULL}, &key);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                view.positions,
                (sxml_item_request){.item_index = key_index, .error_record = NULL}, &position);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (sxml_atomic_item_get_primitive_type(key).index != SXML_TYPE_XS_UNTYPED_ATOMIC) {
            mode = SXML_NUMBER_VIEW_MODE_UNUSABLE;
            break;
        }
        number_cast cast = {.is_cast = false};
        status = cast_to_number(dynamic_context, key, &cast);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!cast.is_cast) {
            mode = SXML_NUMBER_VIEW_MODE_UNUSABLE;
            break;
        }
        const double number = number_of(cast.number);
        sxml_item_release(cast.number);
        if (!isnan(number)) {
            status = append_entry(
                dynamic_context, state,
                (index_entry){.key = sxml_atomic_item_from_double(number),
                              .position = (uint64_t)position.payload.integer});
        }
    }
    if (status != SXML_STATUS_OK) {
        release_entries(state, dynamic_context->allocator);
        return status;
    }
    number_view made = {.mode = mode, .view = index_view_empty()};
    if (mode == SXML_NUMBER_VIEW_MODE_MADE) {
        status = sort_entries(dynamic_context, state, &made.view);
    } else {
        release_entries(state, dynamic_context->allocator);
    }
    if (status == SXML_STATUS_OK) {
        status = store_number_view(dynamic_context, state, made);
    }
    sxml_sequence_release(made.view.keys);
    sxml_sequence_release(made.view.positions);
    *is_usable = mode == SXML_NUMBER_VIEW_MODE_MADE;
    return status;
}

/* How one probe value is looked up: in which view, for which value
   (owned when is_owned). */
typedef struct probe_plan {
    bool is_answerable;
    probe_lookup lookup;
    bool is_owned;
} probe_plan;

/* Plans the lookup of plan->lookup.value, a probe value (borrowed). */
static sxml_status plan_probe_value(sxml_dynamic_context *dynamic_context,
                                    filter_index_state *state, probe_plan *plan) {
    const sxml_item value = plan->lookup.value;
    const sxml_ir_binary_operator key_operator = state->filter->index.key_operator;
    const key_class index_class = index_key_class(state);
    const key_class probe_class = value_class(key_operator, value);
    *plan = (probe_plan){
        .is_answerable = false,
        .lookup = {.view = index_view_of(state, SXML_FILTER_INDEX_MEMBER_KEYS), .value = value},
        .is_owned = false};
    if (probe_class == SXML_KEY_CLASS_NONE) {
        return SXML_STATUS_OK;
    }
    if (index_class == SXML_KEY_CLASS_NONE || probe_class == index_class) {
        plan->is_answerable = true;
        return SXML_STATUS_OK;
    }
    if (key_operator != SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL) {
        return SXML_STATUS_OK;
    }
    const uint32_t primitive = sxml_atomic_item_get_primitive_type(value).index;
    if (index_class == SXML_KEY_CLASS_STRING && primitive != SXML_TYPE_XS_FLOAT) {
        number_view_outcome outcome = {.is_usable = false};
        const sxml_status status = make_number_view(dynamic_context, state, &outcome);
        plan->is_answerable = outcome.is_usable;
        plan->lookup.view = index_view_of(state, SXML_FILTER_INDEX_MEMBER_NUMBER_KEYS);
        return status;
    }
    const sxml_sequence has_float_keys =
        index_member(state->index_array, SXML_FILTER_INDEX_MEMBER_HAS_FLOAT_KEYS);
    if (index_class == SXML_KEY_CLASS_NUMBER && primitive == SXML_TYPE_XS_UNTYPED_ATOMIC &&
        has_float_keys.content.payload.integer == 0) {
        number_cast cast = {.is_cast = false};
        const sxml_status status = cast_to_number(dynamic_context, value, &cast);
        if (status == SXML_STATUS_OK && cast.is_cast) {
            const double number = number_of(cast.number);
            sxml_item_release(cast.number);
            /* NaN equals no key. */
            plan->is_answerable = true;
            plan->lookup.value = sxml_atomic_item_from_double(number);
            plan->is_owned = true;
            if (isnan(number)) {
                plan->lookup.view = index_view_empty();
            }
        }
        return status;
    }
    return SXML_STATUS_OK;
}

/* Looks the probe's value (owned) up and ends the task with the
   candidates, or with the input when the index cannot answer. */
static sxml_status answer_probe(sxml_dynamic_context *dynamic_context, filter_index_state *state,
                                sxml_sequence probe) {
    const sxml_ir_binary_operator key_operator = state->filter->index.key_operator;
    sxml_sequence values = sxml_sequence_empty();
    sxml_status status = comparison_values(
        dynamic_context, (operand_value){.key_operator = key_operator, .value = probe}, &values);
    sxml_sequence_release(probe);
    if (is_catchable(status)) {
        return finish_with_input(dynamic_context, state);
    }
    uint64_t value_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(values, dynamic_context->error_record, &value_count);
    }
    bool is_answerable =
        value_count <= 1 || key_operator == SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL;
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_charge_steps(dynamic_context, value_count);
    }
    candidate_positions candidates = {.positions = NULL, .position_count = 0, .position_capacity = 0};
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(values);
    while (status == SXML_STATUS_OK && is_answerable) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, dynamic_context->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        probe_plan plan = {.is_answerable = false,
                           .lookup = {.view = index_view_empty(), .value = step.item},
                           .is_owned = false};
        status = plan_probe_value(dynamic_context, state, &plan);
        if (status != SXML_STATUS_OK || !plan.is_answerable) {
            is_answerable = false;
            break;
        }
        status = add_candidates_of(dynamic_context, plan.lookup, &candidates);
        if (plan.is_owned) {
            sxml_item_release(plan.lookup.value);
        }
    }
    sxml_sequence_release(values);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK && is_answerable) {
        status = make_candidates(dynamic_context,
                                 (candidate_request){.state = state, .candidates = &candidates},
                                 &result);
    }
    if (candidates.positions != NULL) {
        sxml_allocator_release(dynamic_context->allocator, candidates.positions);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!is_answerable) {
        return finish_with_input(dynamic_context, state);
    }
    return sxml_evaluator_machine_finish(dynamic_context, result);
}

/* ------------------------------------------------------------ The task */

/* Looks the FILTER up in the memo: answers from its index, builds one the
   second time it sees the input, or only remembers the input. */
static sxml_status look_up(sxml_dynamic_context *dynamic_context, sxml_evaluator_task *task) {
    filter_index_state *state = state_of(task);
    const sxml_evaluation_memo_key key = memo_key_of(state->filter);
    sxml_sequence value = sxml_sequence_empty();
    const bool is_found = sxml_dynamic_context_find_memo(dynamic_context, &key, &value);
    if (is_found &&
        inputs_are_same(index_member(value.content, SXML_FILTER_INDEX_MEMBER_INPUT), state->input)) {
        keep_index_array(state, value.content);
        switch (index_mode(value.content)) {
        case SXML_FILTER_INDEX_MODE_SEEN:
            return start_building(dynamic_context, task);
        case SXML_FILTER_INDEX_MODE_INDEXED:
            return start_probe(dynamic_context, task);
        case SXML_FILTER_INDEX_MODE_UNUSABLE:
            return finish_with_input(dynamic_context, state);
        }
        return finish_with_input(dynamic_context, state);
    }
    const sxml_status status = store_index(
        dynamic_context, state,
        memo_index_without_keys(SXML_FILTER_INDEX_MODE_SEEN));
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_input(dynamic_context, state);
}

static sxml_status step_filter_index(sxml_dynamic_context *dynamic_context,
                                     sxml_evaluator_task *task) {
    filter_index_state *state = state_of(task);
    switch ((filter_index_phase)task->phase) {
    case SXML_FILTER_INDEX_PHASE_START:
        return look_up(dynamic_context, task);
    case SXML_FILTER_INDEX_PHASE_KEY: {
        const sxml_status caught = sxml_evaluator_machine_take_caught_status(dynamic_context);
        if (caught != SXML_STATUS_OK) {
            task->catches_errors = false;
            release_entries(state, dynamic_context->allocator);
            return give_up(dynamic_context, state);
        }
        const sxml_status status =
            add_key_values(dynamic_context, state, sxml_evaluator_machine_take_value(dynamic_context));
        if (is_catchable(status)) {
            task->catches_errors = false;
            release_entries(state, dynamic_context->allocator);
            return give_up(dynamic_context, state);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return next_key(dynamic_context, task);
    }
    case SXML_FILTER_INDEX_PHASE_PROBE: {
        task->catches_errors = false;
        const sxml_status caught = sxml_evaluator_machine_take_caught_status(dynamic_context);
        if (caught != SXML_STATUS_OK) {
            return finish_with_input(dynamic_context, state);
        }
        return answer_probe(dynamic_context, state,
                            sxml_evaluator_machine_take_value(dynamic_context));
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

sxml_status sxml_evaluator_start_filter_index(sxml_dynamic_context *dynamic_context,
                                              const sxml_filter_index_request *request) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        dynamic_context->allocator,
        (sxml_allocation_request){.byte_count = sizeof(filter_index_state),
                                  .alignment = alignof(filter_index_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    filter_index_state *state = memory;
    sxml_sequence_retain(request->input);
    *state = (filter_index_state){.filter = request->filter,
                                  .input = request->input,
                                  .iterator = sxml_sequence_iterator_start(sxml_sequence_empty()),
                                  .position = 0,
                                  .entries = NULL,
                                  .entry_count = 0,
                                  .entry_capacity = 0,
                                  .is_usable = true,
                                  .key_class = SXML_KEY_CLASS_NONE,
                                  .index_array = {.representation = 0}};
    return sxml_evaluator_machine_push_part_task(
        dynamic_context, &(sxml_evaluator_part_task_state){.step = step_filter_index,
                                                           .release_state =
                                                               release_filter_index_state,
                                                           .receive = NULL,
                                                           .state = state});
}
