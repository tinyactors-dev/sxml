/*
 * The internal native row of xsl:for-each-group
 * (src/xslt_group_internal.h; XSLT 4.0, section 14; D137, D152; packet
 * 4.9). In no function table: only src/xslt_lower_group.c calls it.
 *
 * Golden file: src/xquery_flwor.c (the group-by row, whose body this row
 * calls for group-by, so grouping keys compare exactly as XQuery's do).
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_group_internal.h"

#include "collation_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"
#include "xquery_flwor_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const sxml_static_error adjacent_key_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE1100"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A group-adjacent key is not exactly one atomic item (XSLT 4.0, 14.1)")};

/* The work of one call: its arguments read. */
typedef struct grouping_work {
    const sxml_function_call *call;
    sxml_allocator allocator;
    sxml_xslt_grouping_method method;
    /* The $collation item, borrowed from the call, and the collation. */
    sxml_item collation_item;
    sxml_collation collation;
    /* The grouping pairs, borrowed from the argument. */
    sxml_item *pairs;
    size_t pair_count;
} grouping_work;

/* Allocator memory for element_count elements (at least one). */
typedef struct element_request {
    size_t element_count;
    size_t element_size;
} element_request;

static sxml_status allocate_elements(sxml_allocator allocator, element_request request,
                                     void **memory) {
    size_t byte_count = 0;
    const sxml_status status = sxml_size_multiply(
        request.element_count == 0 ? 1 : request.element_count, request.element_size,
        &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(max_align_t)},
        memory);
}

/* Member member_index of pair pair_index, borrowed. */
typedef struct pair_member {
    size_t pair_index;
    size_t member_index;
} pair_member;

static sxml_status read_pair_member(const grouping_work *work, pair_member request,
                                    sxml_sequence *member) {
    return sxml_array_get_member(work->pairs[request.pair_index], request.member_index, member);
}

/* Reads $pairs, $method and $collation. */
static sxml_status start_work(grouping_work *work) {
    const sxml_function_call *call = work->call;
    const sxml_sequence pairs = sxml_function_library_get_argument(call, 0);
    const sxml_sequence method = sxml_function_library_get_argument(call, 1);
    const sxml_sequence collation = sxml_function_library_get_argument(call, 2);
    uint64_t pair_count = 0;
    sxml_status status = sxml_sequence_count(pairs, call->error_record, &pair_count);
    if (status == SXML_STATUS_OK && pair_count > SIZE_MAX / sizeof(sxml_item)) {
        status = SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_item method_item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            method, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
            &method_item);
    }
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(method_item, &datum);
    }
    if (status == SXML_STATUS_OK &&
        (datum.datum_kind != SXML_DATUM_KIND_INTEGER || datum.datum_details.integer < 0 ||
         datum.datum_details.integer > SXML_XSLT_GROUPING_METHOD_ENDING_WITH)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        work->method = (sxml_xslt_grouping_method)datum.datum_details.integer;
        status = sxml_sequence_get_item(
            collation, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
            &work->collation_item);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(work->collation_item, &datum);
    }
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_STRING) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        /* 14.1: the collation is resolved like any other (D116). */
        status = sxml_function_library_find_collation(call, datum.datum_details.string.utf8,
                                                      &work->collation);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        work->pair_count = (size_t)pair_count;
        status = allocate_elements(
            work->allocator,
            (element_request){.element_count = work->pair_count, .element_size = sizeof(sxml_item)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    work->pairs = memory;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(pairs);
    for (size_t pair_index = 0; status == SXML_STATUS_OK && pair_index < work->pair_count;
         pair_index += 1) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status == SXML_STATUS_OK && !step.has_item) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status == SXML_STATUS_OK) {
            work->pairs[pair_index] = step.item;
        }
    }
    return status;
}

/* ================================================================ group-by */

/* The tuples for the group-by row: [item, key] for each key of each item,
   or [item, key items...] padded to key_member_count for composite keys. */
typedef struct tuple_list {
    sxml_item *tuples;
    size_t tuple_count;
    size_t key_member_count;
} tuple_list;

static void release_tuples(const grouping_work *work, tuple_list *list) {
    if (list->tuples == NULL) {
        return;
    }
    for (size_t tuple_index = 0; tuple_index < list->tuple_count; tuple_index += 1) {
        sxml_item_release(list->tuples[tuple_index]);
    }
    sxml_allocator_release(work->allocator, list->tuples);
    list->tuples = NULL;
}

/* Counts the tuples to make and the key members of each. */
static sxml_status count_tuples(const grouping_work *work, tuple_list *list) {
    const bool is_composite = work->method == SXML_XSLT_GROUPING_METHOD_BY_COMPOSITE;
    list->tuple_count = is_composite ? work->pair_count : 0;
    list->key_member_count = is_composite ? 0 : 1;
    sxml_status status = SXML_STATUS_OK;
    for (size_t pair_index = 0; status == SXML_STATUS_OK && pair_index < work->pair_count;
         pair_index += 1) {
        sxml_sequence keys = sxml_sequence_empty();
        status = read_pair_member(work, (pair_member){.pair_index = pair_index, .member_index = 1},
                                  &keys);
        uint64_t key_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(keys, work->call->error_record, &key_count);
        }
        if (status == SXML_STATUS_OK && key_count > SIZE_MAX / 4) {
            status = SXML_STATUS_LIMIT_EXCEEDED;
        }
        if (status == SXML_STATUS_OK && is_composite) {
            if (key_count > list->key_member_count) {
                list->key_member_count = (size_t)key_count;
            }
        } else if (status == SXML_STATUS_OK) {
            status = sxml_size_add(list->tuple_count, (size_t)key_count, &list->tuple_count);
        }
    }
    return status;
}

/* The tuple array of one pair: its item, then members (each one key item,
   or empty past the pair's keys) taken from keys starting at first_key. */
typedef struct tuple_request {
    size_t pair_index;
    size_t first_key;
    size_t member_count;
    /* Room for 1 + member_count sequences. */
    sxml_sequence *members;
} tuple_request;

static sxml_status make_tuple(const grouping_work *work, tuple_request request, sxml_item *tuple) {
    const sxml_function_call *call = work->call;
    sxml_sequence keys = sxml_sequence_empty();
    sxml_status status = read_pair_member(
        work, (pair_member){.pair_index = request.pair_index, .member_index = 0},
        &request.members[0]);
    if (status == SXML_STATUS_OK) {
        status = read_pair_member(
            work, (pair_member){.pair_index = request.pair_index, .member_index = 1}, &keys);
    }
    uint64_t key_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(keys, call->error_record, &key_count);
    }
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < request.member_count;
         member_index += 1) {
        const size_t key_index = request.first_key + member_index;
        request.members[member_index + 1] = sxml_sequence_empty();
        if (key_index < key_count) {
            sxml_item key = {.representation = 0};
            status = sxml_sequence_get_item(
                keys,
                (sxml_item_request){.item_index = key_index, .error_record = call->error_record},
                &key);
            if (status == SXML_STATUS_OK) {
                /* Borrowed: sxml_array_make retains what it keeps. */
                request.members[member_index + 1] = sxml_sequence_from_item(key);
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_array_make(call->item_heap,
                           (sxml_sequence_view){.sequences = request.members,
                                                .sequence_count = request.member_count + 1},
                           tuple);
}

static sxml_status make_tuples(const grouping_work *work, tuple_list *list) {
    const bool is_composite = work->method == SXML_XSLT_GROUPING_METHOD_BY_COMPOSITE;
    sxml_status status = count_tuples(work, list);
    void *tuple_memory = NULL;
    void *member_memory = NULL;
    const size_t tuple_count = list->tuple_count;
    list->tuple_count = 0;
    if (status == SXML_STATUS_OK) {
        status = allocate_elements(
            work->allocator,
            (element_request){.element_count = tuple_count, .element_size = sizeof(sxml_item)},
            &tuple_memory);
    }
    if (status == SXML_STATUS_OK) {
        list->tuples = tuple_memory;
        status = allocate_elements(work->allocator,
                                   (element_request){.element_count = list->key_member_count + 1,
                                                     .element_size = sizeof(sxml_sequence)},
                                   &member_memory);
    }
    for (size_t pair_index = 0; status == SXML_STATUS_OK && pair_index < work->pair_count;
         pair_index += 1) {
        status = sxml_function_library_charge_steps(work->call, 1);
        sxml_sequence keys = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            status = read_pair_member(
                work, (pair_member){.pair_index = pair_index, .member_index = 1}, &keys);
        }
        uint64_t key_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(keys, work->call->error_record, &key_count);
        }
        /* 14.1: one tuple per (distinct) key, or one for a composite key. */
        const size_t made_count = is_composite ? 1 : (size_t)key_count;
        for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < made_count;
             key_index += 1) {
            status = make_tuple(work,
                                (tuple_request){.pair_index = pair_index,
                                                .first_key = is_composite ? 0 : key_index,
                                                .member_count = list->key_member_count,
                                                .members = member_memory},
                                &list->tuples[list->tuple_count]);
            if (status == SXML_STATUS_OK) {
                list->tuple_count += 1;
            }
        }
    }
    if (member_memory != NULL) {
        sxml_allocator_release(work->allocator, member_memory);
    }
    if (status == SXML_STATUS_OK && tuple_memory == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    return status;
}

/* Calls the group-by row over the tuples: key members 2 to
   key_member_count + 1, each under the collation. */
static sxml_status call_group_by_row(const grouping_work *work, const tuple_list *list,
                                     sxml_sequence *groups) {
    const sxml_function_call *call = work->call;
    sxml_sequence arguments[3] = {sxml_sequence_empty(), sxml_sequence_empty(),
                                  sxml_sequence_empty()};
    void *collation_memory = NULL;
    sxml_status status = sxml_sequence_make_vector(
        call->item_heap, (sxml_item_view){.items = list->tuples, .item_count = list->tuple_count},
        &arguments[0]);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_integer_range(
            call->item_heap,
            (sxml_integer_range){.first_integer = 2,
                                 .last_integer = (int64_t)list->key_member_count + 1},
            &arguments[1]);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_elements(work->allocator,
                                   (element_request){.element_count = list->key_member_count,
                                                     .element_size = sizeof(sxml_item)},
                                   &collation_memory);
    }
    if (status == SXML_STATUS_OK) {
        sxml_item *collations = collation_memory;
        for (size_t key_index = 0; key_index < list->key_member_count; key_index += 1) {
            collations[key_index] = work->collation_item;
        }
        status = sxml_sequence_make_vector(
            call->item_heap,
            (sxml_item_view){.items = collations, .item_count = list->key_member_count},
            &arguments[2]);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_function_call group_by_call = {
            .function_item = call->function_item,
            .arguments = {.sequences = arguments, .sequence_count = 3},
            .dynamic_context = call->dynamic_context,
            .item_heap = call->item_heap,
            .error_record = call->error_record};
        status = sxml_xquery_group_by_function.call(&group_by_call, groups);
    }
    if (collation_memory != NULL) {
        sxml_allocator_release(work->allocator, collation_memory);
    }
    for (size_t argument_index = 0; argument_index < 3; argument_index += 1) {
        sxml_sequence_release(arguments[argument_index]);
    }
    return status;
}

/* A composite group's array [items, key items...] as [items, key]. */
static sxml_status join_composite_key(const grouping_work *work, sxml_item group,
                                      sxml_item *joined) {
    const sxml_function_call *call = work->call;
    size_t member_count = 0;
    sxml_status status = sxml_array_get_size(group, &member_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_elements(
            work->allocator,
            (element_request){.element_count = member_count, .element_size = sizeof(sxml_sequence)},
            &memory);
    }
    sxml_sequence *members = memory;
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        status = sxml_array_get_member(group, member_index, &members[member_index]);
    }
    sxml_sequence parts[2] = {sxml_sequence_empty(), sxml_sequence_empty()};
    if (status == SXML_STATUS_OK && member_count > 0) {
        parts[0] = members[0];
        status = sxml_sequence_concatenate(
            call->item_heap,
            (sxml_sequence_view){.sequences = members + 1, .sequence_count = member_count - 1},
            &parts[1]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_make(call->item_heap,
                                 (sxml_sequence_view){.sequences = parts, .sequence_count = 2},
                                 joined);
    }
    sxml_sequence_release(parts[1]);
    if (memory != NULL) {
        sxml_allocator_release(work->allocator, memory);
    }
    return status;
}

static sxml_status group_by(const grouping_work *work, sxml_sequence *result) {
    tuple_list list = {.tuples = NULL, .tuple_count = 0, .key_member_count = 0};
    sxml_status status = make_tuples(work, &list);
    sxml_sequence groups = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = call_group_by_row(work, &list, &groups);
    }
    release_tuples(work, &list);
    if (status != SXML_STATUS_OK || work->method != SXML_XSLT_GROUPING_METHOD_BY_COMPOSITE) {
        if (status == SXML_STATUS_OK) {
            *result = groups;
        }
        return status;
    }
    uint64_t group_count = 0;
    status = sxml_sequence_count(groups, work->call->error_record, &group_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_elements(work->allocator,
                                   (element_request){.element_count = (size_t)group_count,
                                                     .element_size = sizeof(sxml_item)},
                                   &memory);
    }
    sxml_item *joined = memory;
    size_t joined_count = 0;
    for (size_t group_index = 0; status == SXML_STATUS_OK && group_index < group_count;
         group_index += 1) {
        sxml_item group = {.representation = 0};
        status = sxml_sequence_get_item(
            groups,
            (sxml_item_request){.item_index = group_index,
                                .error_record = work->call->error_record},
            &group);
        if (status == SXML_STATUS_OK) {
            status = join_composite_key(work, group, &joined[joined_count]);
        }
        if (status == SXML_STATUS_OK) {
            joined_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            work->call->item_heap, (sxml_item_view){.items = joined, .item_count = joined_count},
            result);
    }
    for (size_t group_index = 0; group_index < joined_count; group_index += 1) {
        sxml_item_release(joined[group_index]);
    }
    if (memory != NULL) {
        sxml_allocator_release(work->allocator, memory);
    }
    sxml_sequence_release(groups);
    return status;
}

/* ======================================================= runs of items */

/* Whether two grouping keys are equal (14.1): as many items, each pair
   atomic-equal, strings, xs:anyURI and xs:untypedAtomic compared under the
   collation (as the group-by row compares them, D137). */
typedef struct key_pair {
    sxml_sequence first;
    sxml_sequence second;
} key_pair;

/* Written by items_are_equal and keys_are_equal. */
typedef struct key_equality {
    bool is_equal;
} key_equality;

/* Written by starts_group. */
typedef struct group_start {
    bool is_start;
} group_start;

static sxml_status items_are_equal(const grouping_work *work, sxml_item_pair items,
                                   key_equality *equality) {
    sxml_atomic_datum first;
    sxml_atomic_datum second;
    if (!sxml_collation_is_codepoint_order(&work->collation) &&
        sxml_atomic_item_get_datum(items.first, &first) == SXML_STATUS_OK &&
        sxml_atomic_item_get_datum(items.second, &second) == SXML_STATUS_OK &&
        first.datum_kind == SXML_DATUM_KIND_STRING &&
        second.datum_kind == SXML_DATUM_KIND_STRING) {
        sxml_ordering ordering = SXML_ORDERING_EQUAL;
        const sxml_status status = sxml_collation_compare(
            &(sxml_collation_comparison){.collation = &work->collation,
                                         .allocator = work->allocator,
                                         .first = first.datum_details.string,
                                         .second = second.datum_details.string},
            &ordering);
        equality->is_equal = ordering == SXML_ORDERING_EQUAL;
        return status;
    }
    equality->is_equal = sxml_atomic_item_is_atomic_equal(items.first, items.second);
    return SXML_STATUS_OK;
}

static sxml_status keys_are_equal(const grouping_work *work, key_pair keys,
                                  key_equality *equality) {
    sxml_sequence_iterator first_iterator = sxml_sequence_iterator_start(keys.first);
    sxml_sequence_iterator second_iterator = sxml_sequence_iterator_start(keys.second);
    sxml_error_record *error_record = work->call->error_record;
    for (;;) {
        sxml_iteration_step first_step;
        sxml_iteration_step second_step;
        sxml_status status = sxml_sequence_iterator_next(&first_iterator, error_record, &first_step);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&second_iterator, error_record, &second_step);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!first_step.has_item || !second_step.has_item) {
            equality->is_equal = first_step.has_item == second_step.has_item;
            return SXML_STATUS_OK;
        }
        status = items_are_equal(
            work, (sxml_item_pair){.first = first_step.item, .second = second_step.item},
            equality);
        if (status != SXML_STATUS_OK || !equality->is_equal) {
            return status;
        }
    }
}

/* Whether a new group starts at pair pair_index (> 0). */
static sxml_status starts_group(const grouping_work *work, size_t pair_index,
                                group_start *boundary) {
    boundary->is_start = false;
    if (work->method == SXML_XSLT_GROUPING_METHOD_STARTING_WITH ||
        work->method == SXML_XSLT_GROUPING_METHOD_ENDING_WITH) {
        /* 14.1: a matching item starts a group, or the item after one. */
        const size_t tested =
            work->method == SXML_XSLT_GROUPING_METHOD_STARTING_WITH ? pair_index : pair_index - 1;
        sxml_sequence matches = sxml_sequence_empty();
        sxml_status status = read_pair_member(
            work, (pair_member){.pair_index = tested, .member_index = 1}, &matches);
        sxml_effective_boolean_value truth = {.is_true = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_compute_effective_boolean_value(
                matches, work->call->error_record, &truth);
        }
        boundary->is_start = truth.is_true;
        return status;
    }
    sxml_sequence previous = sxml_sequence_empty();
    sxml_sequence current = sxml_sequence_empty();
    sxml_status status = read_pair_member(
        work, (pair_member){.pair_index = pair_index - 1, .member_index = 1}, &previous);
    if (status == SXML_STATUS_OK) {
        status = read_pair_member(work, (pair_member){.pair_index = pair_index, .member_index = 1},
                                  &current);
    }
    key_equality equality = {.is_equal = false};
    if (status == SXML_STATUS_OK) {
        status = keys_are_equal(work, (key_pair){.first = previous, .second = current}, &equality);
    }
    boundary->is_start = !equality.is_equal;
    return status;
}

/* group-adjacent without composite: every key one item (XTTE1100). */
static sxml_status check_adjacent_keys(const grouping_work *work) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t pair_index = 0; status == SXML_STATUS_OK && pair_index < work->pair_count;
         pair_index += 1) {
        sxml_sequence key = sxml_sequence_empty();
        status = read_pair_member(work, (pair_member){.pair_index = pair_index, .member_index = 1},
                                  &key);
        uint64_t key_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(key, work->call->error_record, &key_count);
        }
        if (status == SXML_STATUS_OK && key_count != 1) {
            status = sxml_function_library_report(work->call, &adjacent_key_error);
        }
    }
    return status;
}

/* The group array of pairs first to end (excluded): [items, key]. */
typedef struct run_request {
    size_t first;
    size_t end;
    /* Room for end - first sequences. */
    sxml_sequence *parts;
} run_request;

static sxml_status make_run(const grouping_work *work, run_request request, sxml_item *group) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t pair_index = request.first; status == SXML_STATUS_OK && pair_index < request.end;
         pair_index += 1) {
        status = read_pair_member(work, (pair_member){.pair_index = pair_index, .member_index = 0},
                                  &request.parts[pair_index - request.first]);
    }
    sxml_sequence members[2] = {sxml_sequence_empty(), sxml_sequence_empty()};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_concatenate(
            work->call->item_heap,
            (sxml_sequence_view){.sequences = request.parts,
                                 .sequence_count = request.end - request.first},
            &members[0]);
    }
    const bool has_key = work->method == SXML_XSLT_GROUPING_METHOD_ADJACENT ||
                         work->method == SXML_XSLT_GROUPING_METHOD_ADJACENT_COMPOSITE;
    if (status == SXML_STATUS_OK && has_key) {
        /* 14.2.2: the key of the group's initial item. */
        status = read_pair_member(
            work, (pair_member){.pair_index = request.first, .member_index = 1}, &members[1]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_make(work->call->item_heap,
                                 (sxml_sequence_view){.sequences = members, .sequence_count = 2},
                                 group);
    }
    sxml_sequence_release(members[0]);
    return status;
}

static sxml_status group_runs(const grouping_work *work, sxml_sequence *result) {
    sxml_status status = SXML_STATUS_OK;
    if (work->method == SXML_XSLT_GROUPING_METHOD_ADJACENT) {
        status = check_adjacent_keys(work);
    }
    /* starts[g] is the first pair of group g. */
    void *start_memory = NULL;
    void *group_memory = NULL;
    void *part_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_elements(
            work->allocator,
            (element_request){.element_count = work->pair_count + 1, .element_size = sizeof(size_t)},
            &start_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_elements(
            work->allocator,
            (element_request){.element_count = work->pair_count, .element_size = sizeof(sxml_item)},
            &group_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_elements(work->allocator,
                                   (element_request){.element_count = work->pair_count,
                                                     .element_size = sizeof(sxml_sequence)},
                                   &part_memory);
    }
    size_t *starts = start_memory;
    size_t group_count = 0;
    for (size_t pair_index = 0; status == SXML_STATUS_OK && pair_index < work->pair_count;
         pair_index += 1) {
        group_start boundary = {.is_start = pair_index == 0};
        status = sxml_function_library_charge_steps(work->call, 1);
        if (status == SXML_STATUS_OK && pair_index > 0) {
            status = starts_group(work, pair_index, &boundary);
        }
        if (status == SXML_STATUS_OK && boundary.is_start) {
            starts[group_count] = pair_index;
            group_count += 1;
        }
    }
    sxml_item *groups = group_memory;
    size_t made_count = 0;
    for (size_t group_index = 0; status == SXML_STATUS_OK && group_index < group_count;
         group_index += 1) {
        const size_t end =
            group_index + 1 < group_count ? starts[group_index + 1] : work->pair_count;
        status = make_run(
            work,
            (run_request){.first = starts[group_index], .end = end, .parts = part_memory},
            &groups[made_count]);
        if (status == SXML_STATUS_OK) {
            made_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            work->call->item_heap, (sxml_item_view){.items = groups, .item_count = made_count},
            result);
    }
    for (size_t group_index = 0; group_index < made_count; group_index += 1) {
        sxml_item_release(groups[group_index]);
    }
    if (start_memory != NULL) {
        sxml_allocator_release(work->allocator, start_memory);
    }
    if (group_memory != NULL) {
        sxml_allocator_release(work->allocator, group_memory);
    }
    if (part_memory != NULL) {
        sxml_allocator_release(work->allocator, part_memory);
    }
    return status;
}

/* ================================================================== row */

static sxml_status call_for_each_group(const sxml_function_call *call, sxml_sequence *result) {
    grouping_work work = {.call = call,
                          .allocator = sxml_item_heap_as_allocator(call->item_heap),
                          .method = SXML_XSLT_GROUPING_METHOD_BY,
                          .collation_item = {.representation = 0},
                          .collation = sxml_collation_codepoint(),
                          .pairs = NULL,
                          .pair_count = 0};
    sxml_status status = start_work(&work);
    if (status == SXML_STATUS_OK) {
        status = work.method == SXML_XSLT_GROUPING_METHOD_BY ||
                         work.method == SXML_XSLT_GROUPING_METHOD_BY_COMPOSITE
                     ? group_by(&work, result)
                     : group_runs(&work, result);
    }
    if (work.pairs != NULL) {
        sxml_allocator_release(work.allocator, work.pairs);
    }
    return status;
}

static const sxml_function_parameter for_each_group_parameters[] = {
    SXML_REQUIRED_PARAMETER("pairs", "array(*)*"),
    SXML_REQUIRED_PARAMETER("method", "xs:integer"),
    SXML_REQUIRED_PARAMETER("collation", "xs:string"),
};

const sxml_native_function sxml_xslt_for_each_group_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    /* The default collation and the collation's base URI. */
    .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("for-each-group"),
    .parameters = {for_each_group_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("array(*)*"),
    .call = call_for_each_group,
    .resumable_body = NULL};
