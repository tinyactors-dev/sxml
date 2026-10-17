/*
 * The internal native rows of group by and window clauses
 * (src/xquery_flwor_internal.h; D55; packet 3.8). They are in no function
 * table: only lowering calls them, by a STATIC_CALL whose target is the
 * row.
 *
 * Golden file: src/function_library_sequence.c (fn:distinct-values' set of
 * atomic-equal keys) and src/function_library_higher_order.c (resumable
 * bodies, D75).
 *
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026).
 */
#include "xquery_flwor_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct heap_array_request {
    size_t element_count;
    size_t element_size;
} heap_array_request;

/* Heap memory for element_count elements, zeroed; at least one element,
   so it is never NULL. */
static sxml_status allocate_heap_array(sxml_item_heap *item_heap, heap_array_request request,
                                       void **memory) {
    *memory = NULL;
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(
        request.element_count == 0 ? 1 : request.element_count, request.element_size, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *allocated = NULL;
    status = sxml_item_heap_allocate_bytes(
        item_heap,
        (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(max_align_t)},
        &allocated);
    if (status == SXML_STATUS_OK && allocated == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        memset(allocated, 0, byte_count);
        *memory = allocated;
    }
    return status;
}

/* Releases memory from allocate_heap_array with the same request; NULL
   is allowed. */
static void release_heap_array(sxml_item_heap *item_heap, void *memory,
                               heap_array_request request) {
    sxml_item_heap_release_bytes(
        item_heap,
        (sxml_heap_bytes){.memory = memory,
                          .byte_count = (request.element_count == 0 ? 1 : request.element_count) *
                                        request.element_size});
}

/* The integer of an xs:integer item; 0 for any other. */
static int64_t integer_of(sxml_item item) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_INTEGER) {
        return 0;
    }
    return datum.datum_details.integer;
}

/* ============================================================ group by */

/* One grouping key of a tuple: empty or one atomic item (borrowed), with
   its collation key when the collation compares it by one. */
typedef struct grouping_key {
    bool is_empty;
    sxml_item item;
    sxml_collated_key collated;
} grouping_key;

/* The work of one group-by call. */
typedef struct grouping {
    const sxml_function_call *call;
    size_t key_count;
    /* key_count member indices (from 0) and collations. */
    size_t *key_members;
    sxml_collation *collations;
    /* The tuple arrays, borrowed from the argument. */
    sxml_item *tuples;
    size_t tuple_count;
    /* The group of each tuple. */
    size_t *tuple_groups;
    /* Per group: its first tuple, its keys (key_count each), its hash. */
    size_t *group_firsts;
    grouping_key *group_keys;
    uint64_t *group_hashes;
    size_t group_count;
    /* Open addressing over groups: group + 1, 0 for a free slot. */
    size_t *slots;
    size_t slot_count;
} grouping;

static void release_keys(grouping_key *keys, size_t key_count) {
    for (size_t key_index = 0; key_index < key_count; key_index += 1) {
        if (keys[key_index].collated.is_keyed) {
            sxml_collation_key_release(&keys[key_index].collated.key);
            keys[key_index].collated.is_keyed = false;
        }
    }
}

/* FNV-1a over bytes. */
static uint64_t hash_bytes(sxml_byte_view bytes) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t byte_index = 0; byte_index < bytes.byte_count; byte_index += 1) {
        hash = (hash ^ bytes.bytes[byte_index]) * UINT64_C(0x100000001b3);
    }
    return hash;
}

static uint64_t hash_keys(const grouping_key *keys, size_t key_count) {
    uint64_t hash = UINT64_C(0x9e3779b97f4a7c15);
    for (size_t key_index = 0; key_index < key_count; key_index += 1) {
        const grouping_key *key = &keys[key_index];
        uint64_t key_hash = 0;
        if (key->is_empty) {
            key_hash = UINT64_C(0x5bd1e995);
        } else if (key->collated.is_keyed) {
            key_hash = hash_bytes(sxml_collation_key_view(&key->collated.key));
        } else {
            key_hash = sxml_atomic_item_hash(key->item);
        }
        hash = (hash ^ key_hash) * UINT64_C(0x100000001b3);
    }
    return hash;
}

/* 4.13.8: equivalent grouping keys are both empty or fn:deep-equal under
   the collation, which for single atomic items is fn:atomic-equal with
   strings compared under the collation. */
static bool keys_are_equal(const grouping_key *first, const grouping_key *second,
                           size_t key_count) {
    for (size_t key_index = 0; key_index < key_count; key_index += 1) {
        const grouping_key *left = &first[key_index];
        const grouping_key *right = &second[key_index];
        if (left->is_empty || right->is_empty) {
            if (left->is_empty != right->is_empty) {
                return false;
            }
            continue;
        }
        if (left->collated.is_keyed || right->collated.is_keyed) {
            if (!left->collated.is_keyed || !right->collated.is_keyed ||
                !sxml_byte_view_equals(sxml_collation_key_view(&left->collated.key),
                                       sxml_collation_key_view(&right->collated.key))) {
                return false;
            }
            continue;
        }
        if (!sxml_atomic_item_is_atomic_equal(left->item, right->item)) {
            return false;
        }
    }
    return true;
}

/* Reads the keys of tuple tuple_index into keys. */
static sxml_status read_keys(grouping *work, size_t tuple_index, grouping_key *keys) {
    const sxml_function_call *call = work->call;
    sxml_status status = SXML_STATUS_OK;
    for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < work->key_count;
         key_index += 1) {
        grouping_key *key = &keys[key_index];
        *key = (grouping_key){.is_empty = true};
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(work->tuples[tuple_index], work->key_members[key_index],
                                       &member);
        sxml_emptiness emptiness = {.is_empty = true};
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_test_emptiness(member, call->error_record, &emptiness);
        }
        if (status != SXML_STATUS_OK || emptiness.is_empty) {
            continue;
        }
        key->is_empty = false;
        status = sxml_sequence_get_item(
            member, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
            &key->item);
        if (status == SXML_STATUS_OK &&
            !sxml_collation_is_codepoint_order(&work->collations[key_index])) {
            status = sxml_function_library_make_collated_key(
                call->item_heap,
                (sxml_collated_key_request){.collation = &work->collations[key_index],
                                            .item = key->item},
                &key->collated);
        }
    }
    return status;
}

/* Doubles the slots (a power of two) and puts every group in again. */
static sxml_status grow_slots(grouping *work) {
    const size_t slot_count = work->slot_count == 0 ? 16 : work->slot_count * 2;
    void *memory = NULL;
    sxml_status status = allocate_heap_array(
        work->call->item_heap,
        (heap_array_request){.element_count = slot_count, .element_size = sizeof(size_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    release_heap_array(
        work->call->item_heap, work->slots,
        (heap_array_request){.element_count = work->slot_count, .element_size = sizeof(size_t)});
    work->slots = memory;
    work->slot_count = slot_count;
    for (size_t group_index = 0; group_index < work->group_count; group_index += 1) {
        size_t slot = (size_t)work->group_hashes[group_index] & (slot_count - 1);
        while (work->slots[slot] != 0) {
            slot = (slot + 1) & (slot_count - 1);
        }
        work->slots[slot] = group_index + 1;
    }
    return SXML_STATUS_OK;
}

/* Puts tuple tuple_index in the group of its keys (probe, taken over),
   making the group when it is the first. */
static sxml_status assign_group(grouping *work, size_t tuple_index, grouping_key *probe) {
    if ((work->group_count + 1) * 2 > work->slot_count) {
        const sxml_status status = grow_slots(work);
        if (status != SXML_STATUS_OK) {
            release_keys(probe, work->key_count);
            return status;
        }
    }
    const uint64_t hash = hash_keys(probe, work->key_count);
    size_t slot = (size_t)hash & (work->slot_count - 1);
    while (work->slots[slot] != 0) {
        const size_t group_index = work->slots[slot] - 1;
        if (work->group_hashes[group_index] == hash &&
            keys_are_equal(&work->group_keys[group_index * work->key_count], probe,
                           work->key_count)) {
            work->tuple_groups[tuple_index] = group_index;
            release_keys(probe, work->key_count);
            return SXML_STATUS_OK;
        }
        slot = (slot + 1) & (work->slot_count - 1);
    }
    const size_t group_index = work->group_count;
    work->group_count += 1;
    work->group_firsts[group_index] = tuple_index;
    work->group_hashes[group_index] = hash;
    memcpy(&work->group_keys[group_index * work->key_count], probe,
           work->key_count * sizeof(grouping_key));
    work->slots[slot] = group_index + 1;
    work->tuple_groups[tuple_index] = group_index;
    return SXML_STATUS_OK;
}

/* The arrays of grouping, sized by tuple and key counts. */
typedef struct grouping_memory {
    heap_array_request requests[7];
    void *arrays[7];
} grouping_memory;

enum {
    SXML_XQUERY_GROUPING_KEY_MEMBERS,
    SXML_XQUERY_GROUPING_COLLATIONS,
    SXML_XQUERY_GROUPING_TUPLES,
    SXML_XQUERY_GROUPING_TUPLE_GROUPS,
    SXML_XQUERY_GROUPING_GROUP_FIRSTS,
    SXML_XQUERY_GROUPING_GROUP_KEYS,
    SXML_XQUERY_GROUPING_GROUP_HASHES,
    SXML_XQUERY_GROUPING_ARRAY_COUNT,
};

static void release_grouping(grouping *work, grouping_memory *memory) {
    for (size_t group_index = 0; work->group_keys != NULL && group_index < work->group_count;
         group_index += 1) {
        release_keys(&work->group_keys[group_index * work->key_count], work->key_count);
    }
    for (size_t array_index = 0; array_index < SXML_XQUERY_GROUPING_ARRAY_COUNT; array_index += 1) {
        release_heap_array(work->call->item_heap, memory->arrays[array_index],
                           memory->requests[array_index]);
    }
    release_heap_array(
        work->call->item_heap, work->slots,
        (heap_array_request){.element_count = work->slot_count, .element_size = sizeof(size_t)});
}

/* Reads the arguments: the key members, their collations, the tuples. */
static sxml_status start_grouping(grouping *work, grouping_memory *memory) {
    const sxml_function_call *call = work->call;
    const sxml_sequence tuples = sxml_function_library_get_argument(call, 0);
    const sxml_sequence keys = sxml_function_library_get_argument(call, 1);
    const sxml_sequence collations = sxml_function_library_get_argument(call, 2);
    uint64_t tuple_count = 0;
    uint64_t key_count = 0;
    sxml_status status = sxml_sequence_count(tuples, call->error_record, &tuple_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(keys, call->error_record, &key_count);
    }
    if (status == SXML_STATUS_OK && (tuple_count > SIZE_MAX / 4 || key_count > SIZE_MAX / 4)) {
        status = SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    work->key_count = (size_t)key_count;
    work->tuple_count = (size_t)tuple_count;
    const heap_array_request requests[SXML_XQUERY_GROUPING_ARRAY_COUNT] = {
        [SXML_XQUERY_GROUPING_KEY_MEMBERS] = {work->key_count, sizeof(size_t)},
        [SXML_XQUERY_GROUPING_COLLATIONS] = {work->key_count, sizeof(sxml_collation)},
        [SXML_XQUERY_GROUPING_TUPLES] = {work->tuple_count, sizeof(sxml_item)},
        [SXML_XQUERY_GROUPING_TUPLE_GROUPS] = {work->tuple_count, sizeof(size_t)},
        [SXML_XQUERY_GROUPING_GROUP_FIRSTS] = {work->tuple_count, sizeof(size_t)},
        [SXML_XQUERY_GROUPING_GROUP_KEYS] = {work->tuple_count * work->key_count, sizeof(grouping_key)},
        [SXML_XQUERY_GROUPING_GROUP_HASHES] = {work->tuple_count, sizeof(uint64_t)},
    };
    for (size_t array_index = 0; status == SXML_STATUS_OK && array_index < SXML_XQUERY_GROUPING_ARRAY_COUNT;
         array_index += 1) {
        memory->requests[array_index] = requests[array_index];
        status = allocate_heap_array(call->item_heap, requests[array_index],
                                     &memory->arrays[array_index]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    work->key_members = memory->arrays[SXML_XQUERY_GROUPING_KEY_MEMBERS];
    work->collations = memory->arrays[SXML_XQUERY_GROUPING_COLLATIONS];
    work->tuples = memory->arrays[SXML_XQUERY_GROUPING_TUPLES];
    work->tuple_groups = memory->arrays[SXML_XQUERY_GROUPING_TUPLE_GROUPS];
    work->group_firsts = memory->arrays[SXML_XQUERY_GROUPING_GROUP_FIRSTS];
    work->group_keys = memory->arrays[SXML_XQUERY_GROUPING_GROUP_KEYS];
    work->group_hashes = memory->arrays[SXML_XQUERY_GROUPING_GROUP_HASHES];
    for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < work->key_count;
         key_index += 1) {
        sxml_item key = {.representation = 0};
        sxml_item collation = {.representation = 0};
        status = sxml_sequence_get_item(
            keys, (sxml_item_request){.item_index = key_index, .error_record = call->error_record},
            &key);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                collations,
                (sxml_item_request){.item_index = key_index, .error_record = call->error_record},
                &collation);
        }
        sxml_atomic_datum datum;
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(collation, &datum);
        }
        if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_STRING) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        const int64_t member_number = status == SXML_STATUS_OK ? integer_of(key) : 0;
        if (status == SXML_STATUS_OK && member_number < 1) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status == SXML_STATUS_OK) {
            work->key_members[key_index] = (size_t)member_number - 1U;
            status = sxml_function_library_find_collation(
                call, datum.datum_details.string.utf8, &work->collations[key_index]);
        }
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(tuples);
    for (size_t tuple_index = 0; status == SXML_STATUS_OK && tuple_index < work->tuple_count;
         tuple_index += 1) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status == SXML_STATUS_OK && !step.has_item) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status == SXML_STATUS_OK) {
            work->tuples[tuple_index] = step.item;
        }
    }
    return status;
}

/* The tuple array of group group_index: the keys of its first tuple, every
   other member the concatenation of its values in the group's tuples, in
   order (4.13.8). members_of_group lists the group's tuples. */
typedef struct group_tuple_request {
    grouping *work;
    const size_t *group_tuples;
    size_t group_tuple_count;
} group_tuple_request;

static sxml_status make_group_tuple(group_tuple_request request, sxml_sequence *parts,
                                    sxml_item *tuple) {
    grouping *work = request.work;
    const sxml_function_call *call = work->call;
    const sxml_item first = work->tuples[request.group_tuples[0]];
    size_t member_count = 0;
    sxml_status status = sxml_array_get_size(first, &member_count);
    void *member_memory = NULL;
    const heap_array_request member_request = {.element_count = member_count,
                                               .element_size = sizeof(sxml_sequence)};
    if (status == SXML_STATUS_OK) {
        status = allocate_heap_array(call->item_heap, member_request, &member_memory);
    }
    sxml_sequence *members = member_memory;
    size_t made_count = 0;
    for (size_t member_index = 0; status == SXML_STATUS_OK && member_index < member_count;
         member_index += 1) {
        bool is_key = false;
        for (size_t key_index = 0; key_index < work->key_count; key_index += 1) {
            is_key = is_key || work->key_members[key_index] == member_index;
        }
        const size_t part_count = is_key ? 1 : request.group_tuple_count;
        for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < part_count;
             part_index += 1) {
            status = sxml_array_get_member(work->tuples[request.group_tuples[part_index]],
                                           member_index, &parts[part_index]);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_concatenate(
                call->item_heap,
                (sxml_sequence_view){.sequences = parts, .sequence_count = part_count},
                &members[member_index]);
        }
        if (status == SXML_STATUS_OK) {
            made_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_make(
            call->item_heap,
            (sxml_sequence_view){.sequences = members, .sequence_count = member_count}, tuple);
    }
    for (size_t member_index = 0; member_index < made_count; member_index += 1) {
        sxml_sequence_release(members[member_index]);
    }
    release_heap_array(call->item_heap, member_memory, member_request);
    return status;
}

/* The tuples of each group, in order: a counting sort of the tuples by
   group. */
static sxml_status make_groups(grouping *work, sxml_sequence *result) {
    const sxml_function_call *call = work->call;
    const heap_array_request index_request = {.element_count = work->tuple_count,
                                              .element_size = sizeof(size_t)};
    const heap_array_request start_request = {.element_count = work->group_count + 1,
                                              .element_size = sizeof(size_t)};
    const heap_array_request part_request = {.element_count = work->tuple_count,
                                             .element_size = sizeof(sxml_sequence)};
    const heap_array_request item_request = {.element_count = work->group_count,
                                             .element_size = sizeof(sxml_item)};
    void *ordered_memory = NULL;
    void *start_memory = NULL;
    void *part_memory = NULL;
    void *item_memory = NULL;
    sxml_status status = allocate_heap_array(call->item_heap, index_request, &ordered_memory);
    if (status == SXML_STATUS_OK) {
        status = allocate_heap_array(call->item_heap, start_request, &start_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_heap_array(call->item_heap, part_request, &part_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_heap_array(call->item_heap, item_request, &item_memory);
    }
    size_t made_count = 0;
    if (status == SXML_STATUS_OK) {
        size_t *ordered = ordered_memory;
        size_t *starts = start_memory;
        sxml_item *items = item_memory;
        for (size_t tuple_index = 0; tuple_index < work->tuple_count; tuple_index += 1) {
            starts[work->tuple_groups[tuple_index] + 1] += 1;
        }
        for (size_t group_index = 0; group_index < work->group_count; group_index += 1) {
            starts[group_index + 1] += starts[group_index];
        }
        for (size_t tuple_index = 0; tuple_index < work->tuple_count; tuple_index += 1) {
            const size_t group_index = work->tuple_groups[tuple_index];
            ordered[starts[group_index]] = tuple_index;
            starts[group_index] += 1;
        }
        /* starts[g] is now the end of group g, the start of group g + 1. */
        size_t group_start = 0;
        for (size_t group_index = 0; status == SXML_STATUS_OK && group_index < work->group_count;
             group_index += 1) {
            const size_t group_end = starts[group_index];
            status = make_group_tuple(
                (group_tuple_request){.work = work,
                                      .group_tuples = &ordered[group_start],
                                      .group_tuple_count = group_end - group_start},
                part_memory, &items[group_index]);
            if (status == SXML_STATUS_OK) {
                made_count += 1;
            }
            group_start = group_end;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_make_vector(
                call->item_heap,
                (sxml_item_view){.items = items, .item_count = work->group_count}, result);
        }
        for (size_t item_index = 0; item_index < made_count; item_index += 1) {
            sxml_item_release(items[item_index]);
        }
    }
    release_heap_array(call->item_heap, ordered_memory, index_request);
    release_heap_array(call->item_heap, start_memory, start_request);
    release_heap_array(call->item_heap, part_memory, part_request);
    release_heap_array(call->item_heap, item_memory, item_request);
    return status;
}

/* group-by($tuples, $keys, $collations) (src/xquery_flwor_internal.h). */
static sxml_status call_group_by(const sxml_function_call *call, sxml_sequence *result) {
    grouping work = {.call = call};
    grouping_memory memory = {0};
    sxml_status status = start_grouping(&work, &memory);
    void *probe_memory = NULL;
    const heap_array_request probe_request = {.element_count = work.key_count,
                                              .element_size = sizeof(grouping_key)};
    if (status == SXML_STATUS_OK) {
        status = allocate_heap_array(call->item_heap, probe_request, &probe_memory);
    }
    for (size_t tuple_index = 0; status == SXML_STATUS_OK && tuple_index < work.tuple_count;
         tuple_index += 1) {
        /* One step per tuple (D89, D91). */
        status = sxml_function_library_charge_steps(call, 1);
        if (status == SXML_STATUS_OK) {
            status = read_keys(&work, tuple_index, probe_memory);
            if (status == SXML_STATUS_OK) {
                status = assign_group(&work, tuple_index, probe_memory);
            } else {
                release_keys(probe_memory, work.key_count);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = make_groups(&work, result);
    }
    release_heap_array(call->item_heap, probe_memory, probe_request);
    release_grouping(&work, &memory);
    return status;
}

static const sxml_function_parameter group_by_parameters[] = {
    SXML_REQUIRED_PARAMETER("tuples", "array(*)*"),
    SXML_REQUIRED_PARAMETER("keys", "xs:integer*"),
    SXML_REQUIRED_PARAMETER("collations", "xs:string*"),
};

const sxml_native_function sxml_xquery_group_by_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    /* The default collation and the collations' base URI. */
    .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("group-by"),
    .parameters = {group_by_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("array(*)*"),
    .call = call_group_by,
    .resumable_body = NULL};

/* ============================================================= windows */

/* Where the window search is (4.13.4.1, 4.13.4.2). */
typedef enum window_phase {
    /* Testing item search_index as a start. */
    SXML_XQUERY_WINDOW_PHASE_FIND_START,
    /* Testing item end_index as the end of the window at start_index. */
    SXML_XQUERY_WINDOW_PHASE_FIND_END,
    /* A tumbling window without end condition: testing item end_index as
       the next start. */
    SXML_XQUERY_WINDOW_PHASE_FIND_NEXT_START,
    SXML_XQUERY_WINDOW_PHASE_DONE,
} window_phase;

typedef struct window_state {
    sxml_item_heap *item_heap;
    /* The input's items, borrowed from the argument. */
    sxml_item *items;
    size_t item_count;
    uint32_t kind;
    window_phase phase;
    /* Whether a call for the current phase is running. */
    bool is_calling;
    size_t search_index;
    size_t start_index;
    size_t end_index;
    /* Owned while a call runs. */
    sxml_sequence call_arguments[SXML_XQUERY_WINDOW_END_PARAMETER_COUNT];
    sxml_sequence_builder *builder;
} window_state;

static void release_call_arguments(window_state *state) {
    for (size_t argument_index = 0; argument_index < SXML_XQUERY_WINDOW_END_PARAMETER_COUNT;
         argument_index += 1) {
        sxml_sequence_release(state->call_arguments[argument_index]);
        state->call_arguments[argument_index] = sxml_sequence_empty();
    }
}

static void release_window_state(void *memory) {
    window_state *state = memory;
    release_call_arguments(state);
    sxml_sequence_builder_destroy(state->builder);
    state->builder = NULL;
    release_heap_array(
        state->item_heap, state->items,
        (heap_array_request){.element_count = state->item_count, .element_size = sizeof(sxml_item)});
    state->items = NULL;
}

/* The item at index as a sequence, owned; empty outside the input. */
static sxml_sequence item_sequence(const window_state *state, size_t index_plus_one) {
    if (index_plus_one == 0 || index_plus_one > state->item_count) {
        return sxml_sequence_empty();
    }
    const sxml_item item = state->items[index_plus_one - 1];
    sxml_item_retain(item);
    return sxml_sequence_from_item(item);
}

/* The four variables of item index as a start or end (current, position,
   previous, next) into variables, owned. */
static sxml_status bind_variables(const window_state *state, size_t index,
                                  sxml_sequence *variables) {
    variables[0] = item_sequence(state, index + 1);
    variables[1] = sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)index + 1));
    variables[2] = item_sequence(state, index);
    variables[3] = item_sequence(state, index + 2);
    return SXML_STATUS_OK;
}

/* Appends the window array of items start_index to end_index. */
static sxml_status emit_window(window_state *state, size_t start_index, size_t end_index) {
    sxml_sequence members[9];
    sxml_status status = sxml_sequence_make_vector(
        state->item_heap,
        (sxml_item_view){.items = &state->items[start_index],
                         .item_count = end_index - start_index + 1},
        &members[0]);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    (void)bind_variables(state, start_index, &members[1]);
    (void)bind_variables(state, end_index, &members[5]);
    sxml_item array = {.representation = 0};
    status = sxml_array_make(state->item_heap,
                             (sxml_sequence_view){.sequences = members, .sequence_count = 9},
                             &array);
    for (size_t member_index = 0; member_index < 9; member_index += 1) {
        sxml_sequence_release(members[member_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_item(state->builder, array);
        sxml_item_release(array);
    }
    return status;
}

/* A window from start_index to end_index was found: where the search for
   the next start goes on (4.13.4.1: after the window's end; 4.13.4.2:
   after its start). */
static void continue_after_window(window_state *state, size_t end_index) {
    state->search_index = (state->kind & SXML_XQUERY_WINDOW_SLIDING) != 0
                              ? state->start_index + 1
                              : end_index + 1;
    state->phase = SXML_XQUERY_WINDOW_PHASE_FIND_START;
}

/* Applies the truth of the condition of the current phase. */
static sxml_status take_truth(window_state *state, bool is_true) {
    switch (state->phase) {
    case SXML_XQUERY_WINDOW_PHASE_FIND_START:
        if (!is_true) {
            state->search_index += 1;
            return SXML_STATUS_OK;
        }
        state->start_index = state->search_index;
        if ((state->kind & SXML_XQUERY_WINDOW_HAS_END) != 0) {
            state->end_index = state->start_index;
            state->phase = SXML_XQUERY_WINDOW_PHASE_FIND_END;
        } else {
            state->end_index = state->start_index + 1;
            state->phase = SXML_XQUERY_WINDOW_PHASE_FIND_NEXT_START;
        }
        return SXML_STATUS_OK;
    case SXML_XQUERY_WINDOW_PHASE_FIND_END: {
        if (!is_true) {
            state->end_index += 1;
            return SXML_STATUS_OK;
        }
        const size_t end_index = state->end_index;
        const sxml_status status = emit_window(state, state->start_index, end_index);
        continue_after_window(state, end_index);
        return status;
    }
    case SXML_XQUERY_WINDOW_PHASE_FIND_NEXT_START: {
        if (!is_true) {
            state->end_index += 1;
            return SXML_STATUS_OK;
        }
        /* The window ends before the next start, which starts the next
           window. */
        const sxml_status status =
            emit_window(state, state->start_index, state->end_index - 1);
        state->start_index = state->end_index;
        state->end_index += 1;
        return status;
    }
    case SXML_XQUERY_WINDOW_PHASE_DONE:
        break;
    }
    return SXML_STATUS_OK;
}

/* The input ran out in the current phase. */
static sxml_status take_end_of_input(window_state *state) {
    sxml_status status = SXML_STATUS_OK;
    switch (state->phase) {
    case SXML_XQUERY_WINDOW_PHASE_FIND_END:
        /* No end item: "only end" makes no window, otherwise it ends at
           the last item (4.13.4). */
        if ((state->kind & SXML_XQUERY_WINDOW_ONLY_END) == 0) {
            status = emit_window(state, state->start_index, state->item_count - 1);
        }
        if ((state->kind & SXML_XQUERY_WINDOW_SLIDING) != 0) {
            continue_after_window(state, state->item_count - 1);
            return status;
        }
        break;
    case SXML_XQUERY_WINDOW_PHASE_FIND_NEXT_START:
        status = emit_window(state, state->start_index, state->item_count - 1);
        break;
    case SXML_XQUERY_WINDOW_PHASE_FIND_START:
    case SXML_XQUERY_WINDOW_PHASE_DONE:
        break;
    }
    state->phase = SXML_XQUERY_WINDOW_PHASE_DONE;
    return status;
}

static sxml_status step_window(const sxml_native_step_request *request, sxml_native_step *step) {
    window_state *state = request->state;
    const sxml_function_call *call = request->call;
    const sxml_sequence start_function = sxml_function_library_get_argument(call, 1);
    const sxml_sequence end_function = sxml_function_library_get_argument(call, 2);
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        state->item_heap = call->item_heap;
        uint64_t item_count = 0;
        const sxml_sequence input = sxml_function_library_get_argument(call, 0);
        sxml_item kind = {.representation = 0};
        status = sxml_sequence_count(input, call->error_record, &item_count);
        if (status == SXML_STATUS_OK && item_count > SIZE_MAX / sizeof(sxml_item)) {
            status = SXML_STATUS_LIMIT_EXCEEDED;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                sxml_function_library_get_argument(call, 3),
                (sxml_item_request){.item_index = 0, .error_record = call->error_record}, &kind);
        }
        void *item_memory = NULL;
        if (status == SXML_STATUS_OK) {
            state->kind = (uint32_t)integer_of(kind);
            status = allocate_heap_array(
                call->item_heap,
                (heap_array_request){.element_count = (size_t)item_count,
                                     .element_size = sizeof(sxml_item)},
                &item_memory);
            state->items = item_memory;
        }
        if (status == SXML_STATUS_OK) {
            state->item_count = (size_t)item_count;
            status = sxml_sequence_builder_create(
                sxml_item_heap_as_allocator(call->item_heap),
                &(sxml_sequence_builder_configuration){.item_heap = call->item_heap},
                &state->builder);
        }
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(input);
        for (size_t item_index = 0; status == SXML_STATUS_OK && item_index < state->item_count;
             item_index += 1) {
            sxml_iteration_step item_step;
            status = sxml_sequence_iterator_next(&iterator, call->error_record, &item_step);
            if (status == SXML_STATUS_OK && !item_step.has_item) {
                status = SXML_STATUS_INVALID_ARGUMENT;
            }
            if (status == SXML_STATUS_OK) {
                state->items[item_index] = item_step.item;
            }
        }
        state->phase = SXML_XQUERY_WINDOW_PHASE_FIND_START;
    } else {
        sxml_effective_boolean_value truth = {.is_true = false};
        status = sxml_evaluator_compute_effective_boolean_value(request->call_result,
                                                                call->error_record, &truth);
        sxml_sequence_release(request->call_result);
        release_call_arguments(state);
        state->is_calling = false;
        if (status == SXML_STATUS_OK) {
            status = take_truth(state, truth.is_true);
        }
    }
    while (status == SXML_STATUS_OK && state->phase != SXML_XQUERY_WINDOW_PHASE_DONE) {
        /* One step per condition (D89, D91). */
        status = sxml_function_library_charge_steps(call, 1);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const bool is_start_test = state->phase != SXML_XQUERY_WINDOW_PHASE_FIND_END;
        const size_t tested_index =
            state->phase == SXML_XQUERY_WINDOW_PHASE_FIND_START ? state->search_index : state->end_index;
        if (tested_index >= state->item_count) {
            status = take_end_of_input(state);
            continue;
        }
        const sxml_sequence function = is_start_test ? start_function : end_function;
        sxml_emptiness emptiness = {.is_empty = true};
        status = sxml_sequence_test_emptiness(function, call->error_record, &emptiness);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (emptiness.is_empty) {
            /* No "when": the condition is true (4.13.4). */
            status = take_truth(state, true);
            continue;
        }
        sxml_item function_item = {.representation = 0};
        status = sxml_sequence_get_item(
            function, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
            &function_item);
        if (status != SXML_STATUS_OK) {
            break;
        }
        size_t argument_count = SXML_XQUERY_WINDOW_START_PARAMETER_COUNT;
        if (is_start_test) {
            (void)bind_variables(state, tested_index, state->call_arguments);
        } else {
            (void)bind_variables(state, state->start_index, state->call_arguments);
            (void)bind_variables(state, tested_index,
                                 &state->call_arguments[SXML_XQUERY_WINDOW_START_PARAMETER_COUNT]);
            argument_count = SXML_XQUERY_WINDOW_END_PARAMETER_COUNT;
        }
        state->is_calling = true;
        *step = (sxml_native_step){
            .step_kind = SXML_NATIVE_STEP_KIND_CALL,
            .result = sxml_sequence_empty(),
            .function_item = function_item,
            .arguments = {.sequences = state->call_arguments, .sequence_count = argument_count}};
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_sequence_builder_finish(state->builder, &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                               .result = result,
                               .function_item = {.representation = 0},
                               .arguments = {.sequences = NULL, .sequence_count = 0}};
    return SXML_STATUS_OK;
}

static const sxml_resumable_native_body window_body = {
    .step = step_window,
    .release_state = release_window_state,
    .state_byte_count = sizeof(window_state),
    .state_alignment = alignof(window_state)};

static const sxml_function_parameter window_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_REQUIRED_PARAMETER("start", "fn(*)?"),
    SXML_REQUIRED_PARAMETER("end", "fn(*)?"),
    SXML_REQUIRED_PARAMETER("kind", "xs:integer"),
};

const sxml_native_function sxml_xquery_window_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("window"),
    .parameters = {window_parameters, 4},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("array(*)*"),
    .call = NULL,
    .resumable_body = &window_body};
