/*
 * Tests for arrays: making, reading, the persistent updates against a plain
 * C model, their cost, sharing, leaks and allocation failures
 * (include/sxml/xdm.h, src/xdm_array.c; D33, D37, D60).
 *
 * Imitates tests/memory_test.c and tests/xdm_test.c (fixture, D23 failure
 * loop). Behaviour follows XDM 4.0, section 8.4 and F&O 4.0, section 18
 * (array:put, array:insert-before, array:remove, array:subarray).
 */
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"

#include <string.h>

typedef struct array_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} array_fixture;

static sxml_status array_fixture_create_with(sxml_allocator allocator,
                                             array_fixture *fixture) {
    *fixture =
        (array_fixture){.name_pool = NULL, .registry = NULL, .item_heap = NULL};
    sxml_status status = sxml_name_pool_create(
        sxml_default_allocator(),
        &(sxml_name_pool_configuration){.allocator = sxml_default_allocator(),
                                        .maximum_byte_count = 0},
        &fixture->name_pool);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_type_registry_create(sxml_default_allocator(),
                                      &(sxml_type_registry_configuration){
                                          .allocator = sxml_default_allocator(),
                                          .name_pool = fixture->name_pool,
                                          .maximum_registered_type_count = 0},
                                      &fixture->registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            sxml_default_allocator(),
            &(sxml_item_heap_configuration){.allocator = allocator,
                                            .name_pool = fixture->name_pool,
                                            .type_registry = fixture->registry,
                                            .maximum_byte_count = 0},
            &fixture->item_heap);
    }
    return status;
}

static sxml_status array_fixture_create(array_fixture *fixture) {
    return array_fixture_create_with(sxml_default_allocator(), fixture);
}

static void array_fixture_destroy(array_fixture *fixture) {
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

static uint64_t live_cell_count(const array_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
}

static uint64_t live_byte_count(const array_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_byte_count;
}

/* The member holding the one integer item number. */
static sxml_sequence integer_member(int64_t number) {
    return sxml_sequence_from_item(sxml_atomic_item_from_integer(number));
}

/* The integer of a single-integer member; -1 for anything else. */
static int64_t member_integer(sxml_sequence member) {
    if (sxml_sequence_get_representation(member) !=
        SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        return -1;
    }
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(member.content, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_INTEGER) {
        return -1;
    }
    return datum.datum_details.integer;
}

static sxml_status make_string_member(sxml_item_heap *item_heap,
                                      sxml_byte_view text,
                                      sxml_sequence *member) {
    sxml_string string;
    sxml_status status = sxml_string_from_utf8(text, &string);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = string}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *member = sxml_sequence_from_item(item);
    }
    return status;
}

/* True when array holds exactly the integers of expected, in order. */
typedef struct integer_list {
    const int64_t *integers;
    size_t integer_count;
} integer_list;

static bool array_holds(sxml_item array, integer_list expected) {
    size_t member_count = 0;
    if (sxml_array_get_size(array, &member_count) != SXML_STATUS_OK ||
        member_count != expected.integer_count) {
        return false;
    }
    for (size_t member_index = 0; member_index < member_count;
         member_index += 1) {
        sxml_sequence member = sxml_sequence_empty();
        if (sxml_array_get_member(array, member_index, &member) !=
                SXML_STATUS_OK ||
            member_integer(member) != expected.integers[member_index]) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------ Making */

static void test_make_and_read(sxml_test_state *state) {
    array_fixture fixture;
    SXML_TEST_REQUIRE(state, array_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_sequence text = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state,
                      make_string_member(fixture.item_heap,
                                         SXML_BYTE_VIEW_FROM_LITERAL("member"),
                                         &text) == SXML_STATUS_OK);
    const sxml_sequence members[3] = {integer_member(1), sxml_sequence_empty(),
                                      text};
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_array_make(fixture.item_heap,
                                      (sxml_sequence_view){.sequences = members,
                                                           .sequence_count = 3},
                                      &array) == SXML_STATUS_OK);
    /* The array retains the string: the caller's reference can go. */
    sxml_sequence_release(text);
    SXML_TEST_EXPECT(state, sxml_item_get_kind(array) == SXML_ITEM_KIND_ARRAY);
    SXML_TEST_EXPECT(state, sxml_item_is_function_item(array));
    SXML_TEST_EXPECT(state, sxml_item_get_type_annotation(array).index ==
                                SXML_TYPE_ARRAY);
    size_t member_count = 0;
    SXML_TEST_EXPECT(state, sxml_array_get_size(array, &member_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, member_count == 3);
    sxml_sequence member = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_array_get_member(array, 0, &member) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, member_integer(member) == 1);
    SXML_TEST_EXPECT(state, sxml_array_get_member(array, 1, &member) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(member) ==
                                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    SXML_TEST_EXPECT(state, sxml_array_get_member(array, 2, &member) ==
                                SXML_STATUS_OK);
    sxml_atomic_datum datum;
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(
                                member.content, &datum) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     datum.datum_kind == SXML_DATUM_KIND_STRING &&
                         datum.datum_details.string.utf8.byte_count == 6);
    SXML_TEST_EXPECT(state, sxml_array_get_member(array, 3, &member) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    sxml_item_release(array);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    array_fixture_destroy(&fixture);
}

static void test_empty_array_and_non_arrays(sxml_test_state *state) {
    array_fixture fixture;
    SXML_TEST_REQUIRE(state, array_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item empty = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_array_make(fixture.item_heap,
                                      (sxml_sequence_view){.sequences = NULL,
                                                           .sequence_count = 0},
                                      &empty) == SXML_STATUS_OK);
    size_t member_count = 9;
    SXML_TEST_EXPECT(state, sxml_array_get_size(empty, &member_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, member_count == 0);
    sxml_sequence member = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_array_get_member(empty, 0, &member) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    /* Removing from or putting into the empty array is out of range;
       inserting at 0 and appending are not. */
    sxml_item changed = {.representation = 0};
    const sxml_array_update at_start = {
        .array = empty, .member_index = 0, .member = integer_member(5)};
    SXML_TEST_EXPECT(
        state, sxml_array_remove(fixture.item_heap, &at_start, &changed) ==
                   SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state,
                     sxml_array_put(fixture.item_heap, &at_start, &changed) ==
                         SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_REQUIRE(state, sxml_array_insert(fixture.item_heap, &at_start,
                                               &changed) == SXML_STATUS_OK);
    const int64_t five[] = {5};
    SXML_TEST_EXPECT(state,
                     array_holds(changed, (integer_list){.integers = five,
                                                         .integer_count = 1}));
    /* Removing the only member gives an empty array again. */
    sxml_item emptied = {.representation = 0};
    SXML_TEST_REQUIRE(
        state,
        sxml_array_remove(fixture.item_heap,
                          &(sxml_array_update){.array = changed,
                                               .member_index = 0,
                                               .member = sxml_sequence_empty()},
                          &emptied) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_array_get_size(emptied, &member_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, member_count == 0);
    /* Slices of nothing, and of everything. */
    sxml_item slice = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_array_slice(fixture.item_heap,
                                              &(sxml_array_slice_request){
                                                  .array = changed,
                                                  .first_member_index = 1,
                                                  .member_count = 0},
                                              &slice) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_array_get_size(slice, &member_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, member_count == 0);
    sxml_item_release(slice);
    SXML_TEST_EXPECT(
        state,
        sxml_array_slice(fixture.item_heap,
                         &(sxml_array_slice_request){.array = changed,
                                                     .first_member_index = 1,
                                                     .member_count = 1},
                         &slice) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(
        state,
        sxml_array_slice(fixture.item_heap,
                         &(sxml_array_slice_request){.array = changed,
                                                     .first_member_index = 2,
                                                     .member_count = 0},
                         &slice) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    /* Non-arrays are rejected by every function. */
    const sxml_item integer = sxml_atomic_item_from_integer(3);
    SXML_TEST_EXPECT(state, sxml_array_get_size(integer, &member_count) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_array_get_member(integer, 0, &member) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    const sxml_array_update on_integer = {
        .array = integer, .member_index = 0, .member = integer_member(1)};
    SXML_TEST_EXPECT(state,
                     sxml_array_append(fixture.item_heap, &on_integer,
                                       &slice) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(
        state,
        sxml_array_slice(fixture.item_heap,
                         &(sxml_array_slice_request){.array = integer,
                                                     .first_member_index = 0,
                                                     .member_count = 0},
                         &slice) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_item_release(emptied);
    sxml_item_release(changed);
    sxml_item_release(empty);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    array_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Updates and a model */

enum {
    SXML_TEST_MODEL_CAPACITY = 400,
    SXML_TEST_MODEL_VERSION_COUNT = 6,
    SXML_TEST_MODEL_STEP_COUNT = 3000
};

/* One version of the array and the integers it should hold. */
typedef struct model_version {
    sxml_item array;
    size_t integer_count;
    int64_t integers[SXML_TEST_MODEL_CAPACITY];
} model_version;

/* A xorshift generator: deterministic, so failures reproduce. */
typedef struct random_source {
    uint64_t random_state;
} random_source;

static uint64_t random_next(random_source *source) {
    uint64_t random_state = source->random_state;
    random_state ^= random_state << 13U;
    random_state ^= random_state >> 7U;
    random_state ^= random_state << 17U;
    source->random_state = random_state;
    return random_state;
}

/* A number below bound (bound at least 1). */
static size_t random_below(random_source *source, size_t bound) {
    return (size_t)(random_next(source) % bound);
}

typedef struct model_step {
    const model_version *from_version;
    model_version *to_version;
    int64_t new_integer;
} model_step;

/*
 * Applies one random update to step->from_version (an array kept as is,
 * since arrays are persistent) and writes the new array and its expected
 * integers to step->to_version.
 */
static sxml_status model_apply_random_step(sxml_item_heap *item_heap,
                                           random_source *source,
                                           model_step *step) {
    const model_version *from = step->from_version;
    model_version *target = step->to_version;
    size_t count = from->integer_count;
    size_t operation =
        random_below(source, count >= SXML_TEST_MODEL_CAPACITY ? 2 : 5);
    sxml_array_update update = {.array = from->array,
                                .member_index = 0,
                                .member = integer_member(step->new_integer)};
    sxml_status status = SXML_STATUS_OK;
    memcpy(target->integers, from->integers, count * sizeof(int64_t));
    target->integer_count = count;
    if (count == 0 && operation < 2) {
        operation = 2;
    }
    switch (operation) {
    case 0: {
        /* A slice. */
        size_t first = random_below(source, count + 1);
        size_t part_count = random_below(source, count - first + 1);
        status = sxml_array_slice(
            item_heap,
            &(sxml_array_slice_request){.array = from->array,
                                        .first_member_index = first,
                                        .member_count = part_count},
            &target->array);
        memmove(target->integers, from->integers + first,
                part_count * sizeof(int64_t));
        target->integer_count = part_count;
        break;
    }
    case 1: {
        update.member_index = random_below(source, count);
        if (random_below(source, 2) == 0) {
            status = sxml_array_remove(item_heap, &update, &target->array);
            memmove(target->integers + update.member_index,
                    from->integers + update.member_index + 1,
                    (count - update.member_index - 1) * sizeof(int64_t));
            target->integer_count = count - 1;
        } else {
            status = sxml_array_put(item_heap, &update, &target->array);
            target->integers[update.member_index] = step->new_integer;
        }
        break;
    }
    case 2:
        status = sxml_array_append(item_heap, &update, &target->array);
        target->integers[count] = step->new_integer;
        target->integer_count = count + 1;
        break;
    default:
        update.member_index = random_below(source, count + 1);
        status = sxml_array_insert(item_heap, &update, &target->array);
        memmove(target->integers + update.member_index + 1,
                from->integers + update.member_index,
                (count - update.member_index) * sizeof(int64_t));
        target->integers[update.member_index] = step->new_integer;
        target->integer_count = count + 1;
        break;
    }
    return status;
}

static bool model_version_holds(const model_version *version) {
    return array_holds(version->array,
                       (integer_list){.integers = version->integers,
                                      .integer_count = version->integer_count});
}

/* Random updates on random versions; every version must keep its members
   (persistence) and the new one must match the model. */
static void test_updates_match_a_model(sxml_test_state *state) {
    array_fixture fixture;
    SXML_TEST_REQUIRE(state, array_fixture_create(&fixture) == SXML_STATUS_OK);
    model_version versions[SXML_TEST_MODEL_VERSION_COUNT];
    model_version scratch_version;
    for (size_t version_index = 0;
         version_index < SXML_TEST_MODEL_VERSION_COUNT; version_index += 1) {
        versions[version_index].integer_count = 0;
        SXML_TEST_REQUIRE(
            state,
            sxml_array_make(
                fixture.item_heap,
                (sxml_sequence_view){.sequences = NULL, .sequence_count = 0},
                &versions[version_index].array) == SXML_STATUS_OK);
    }
    random_source source = {.random_state = 0x9E3779B97F4A7C15U};
    bool all_match = true;
    for (size_t step_index = 0; step_index < SXML_TEST_MODEL_STEP_COUNT;
         step_index += 1) {
        size_t from_index =
            random_below(&source, SXML_TEST_MODEL_VERSION_COUNT);
        size_t to_index = random_below(&source, SXML_TEST_MODEL_VERSION_COUNT);
        sxml_status status = model_apply_random_step(
            fixture.item_heap, &source,
            &(model_step){.from_version = &versions[from_index],
                          .to_version = &scratch_version,
                          .new_integer = (int64_t)step_index + 1});
        SXML_TEST_REQUIRE(state, status == SXML_STATUS_OK);
        sxml_item_release(versions[to_index].array);
        versions[to_index] = scratch_version;
        all_match = all_match && model_version_holds(&versions[to_index]);
        if (step_index % 100 == 0) {
            for (size_t version_index = 0;
                 version_index < SXML_TEST_MODEL_VERSION_COUNT;
                 version_index += 1) {
                all_match =
                    all_match && model_version_holds(&versions[version_index]);
            }
        }
    }
    SXML_TEST_EXPECT(state, all_match);
    for (size_t version_index = 0;
         version_index < SXML_TEST_MODEL_VERSION_COUNT; version_index += 1) {
        SXML_TEST_EXPECT(state, model_version_holds(&versions[version_index]));
        sxml_item_release(versions[version_index].array);
    }
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    array_fixture_destroy(&fixture);
}

/* Updates of a made array: put, insert, remove and slice cut its flat
   node into runs that share its members. */
static void test_updates_of_a_made_array(sxml_test_state *state) {
    array_fixture fixture;
    SXML_TEST_REQUIRE(state, array_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_sequence members[10];
    for (size_t member_index = 0; member_index < 10; member_index += 1) {
        members[member_index] = integer_member((int64_t)member_index);
    }
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(
        state, sxml_array_make(fixture.item_heap,
                               (sxml_sequence_view){.sequences = members,
                                                    .sequence_count = 10},
                               &array) == SXML_STATUS_OK);
    sxml_item put = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_array_put(fixture.item_heap,
                                            &(sxml_array_update){
                                                .array = array,
                                                .member_index = 4,
                                                .member = integer_member(40)},
                                            &put) == SXML_STATUS_OK);
    const int64_t after_put[] = {0, 1, 2, 3, 40, 5, 6, 7, 8, 9};
    SXML_TEST_EXPECT(state,
                     array_holds(put, (integer_list){.integers = after_put,
                                                     .integer_count = 10}));
    sxml_item inserted = {.representation = 0};
    sxml_item unused = {.representation = 0};
    SXML_TEST_REQUIRE(
        state,
        sxml_array_insert(fixture.item_heap,
                          &(sxml_array_update){.array = put,
                                               .member_index = 10,
                                               .member = integer_member(10)},
                          &inserted) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state,
        sxml_array_insert(fixture.item_heap,
                          &(sxml_array_update){.array = put,
                                               .member_index = 11,
                                               .member = integer_member(0)},
                          &unused) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    sxml_item removed = {.representation = 0};
    SXML_TEST_REQUIRE(
        state,
        sxml_array_remove(fixture.item_heap,
                          &(sxml_array_update){.array = inserted,
                                               .member_index = 0,
                                               .member = sxml_sequence_empty()},
                          &removed) == SXML_STATUS_OK);
    sxml_item slice = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_array_slice(fixture.item_heap,
                                              &(sxml_array_slice_request){
                                                  .array = removed,
                                                  .first_member_index = 2,
                                                  .member_count = 5},
                                              &slice) == SXML_STATUS_OK);
    const int64_t sliced[] = {3, 40, 5, 6, 7};
    SXML_TEST_EXPECT(state,
                     array_holds(slice, (integer_list){.integers = sliced,
                                                       .integer_count = 5}));
    /* The original is unchanged (persistence). */
    const int64_t original[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    SXML_TEST_EXPECT(state,
                     array_holds(array, (integer_list){.integers = original,
                                                       .integer_count = 10}));
    sxml_item_release(array);
    sxml_item_release(put);
    sxml_item_release(inserted);
    sxml_item_release(removed);
    /* The slice keeps alive only what it needs. */
    SXML_TEST_EXPECT(state,
                     array_holds(slice, (integer_list){.integers = sliced,
                                                       .integer_count = 5}));
    sxml_item_release(slice);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    array_fixture_destroy(&fixture);
}

/* -------------------------------------------------------------- Cost */

enum { SXML_TEST_APPEND_COUNT = 100000 };

/* The base-2 logarithm of value, rounded up. */
static uint64_t ceiling_log2(uint64_t value) {
    uint64_t logarithm = 0;
    while (((uint64_t)1 << logarithm) < value) {
        logarithm += 1;
    }
    return logarithm;
}

/*
 * 10^5 appends, each to the array the previous one made: allocations per
 * append stay O(log n) (a leaf plus a few nodes per level of the right
 * spine; AVL height is below 1.45 log2 n + 2), so the whole is O(n log n).
 */
static void test_appends_cost_n_log_n(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_counting_allocator_create(
                          sxml_default_allocator(),
                          &(sxml_counting_allocator_configuration){
                              .backing_allocator = sxml_default_allocator(),
                              .failing_allocation_number = 0},
                          &counting_allocator) == SXML_STATUS_OK);
    array_fixture fixture;
    SXML_TEST_REQUIRE(
        state, array_fixture_create_with(
                   sxml_counting_allocator_as_allocator(counting_allocator),
                   &fixture) == SXML_STATUS_OK);
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_array_make(fixture.item_heap,
                                      (sxml_sequence_view){.sequences = NULL,
                                                           .sequence_count = 0},
                                      &array) == SXML_STATUS_OK);
    uint64_t start_count =
        sxml_counting_allocator_read_counts(counting_allocator)
            .attempted_allocation_count;
    uint64_t height_bound = (ceiling_log2(SXML_TEST_APPEND_COUNT) * 3) / 2 + 2;
    uint64_t largest_append_count = 0;
    for (size_t append_index = 0; append_index < SXML_TEST_APPEND_COUNT;
         append_index += 1) {
        uint64_t before =
            sxml_counting_allocator_read_counts(counting_allocator)
                .attempted_allocation_count;
        sxml_item longer = {.representation = 0};
        SXML_TEST_REQUIRE(
            state,
            sxml_array_append(fixture.item_heap,
                              &(sxml_array_update){.array = array,
                                                   .member_index = 0,
                                                   .member = integer_member(
                                                       (int64_t)append_index)},
                              &longer) == SXML_STATUS_OK);
        uint64_t used = sxml_counting_allocator_read_counts(counting_allocator)
                            .attempted_allocation_count -
                        before;
        if (used > largest_append_count) {
            largest_append_count = used;
        }
        sxml_item_release(array);
        array = longer;
    }
    uint64_t total_count =
        sxml_counting_allocator_read_counts(counting_allocator)
            .attempted_allocation_count -
        start_count;
    /* One leaf, and at most three branch nodes per level. */
    SXML_TEST_EXPECT(state, largest_append_count <= 1 + 3 * height_bound);
    SXML_TEST_EXPECT(state, total_count <= SXML_TEST_APPEND_COUNT *
                                               (1 + 3 * height_bound));
    /* A leaf and a branch per member, at most. */
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) <=
                                (uint64_t)2 * SXML_TEST_APPEND_COUNT);
    size_t member_count = 0;
    SXML_TEST_EXPECT(state, sxml_array_get_size(array, &member_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, member_count == SXML_TEST_APPEND_COUNT);
    bool all_in_order = true;
    for (size_t member_index = 0; member_index < SXML_TEST_APPEND_COUNT;
         member_index += 1) {
        sxml_sequence member = sxml_sequence_empty();
        all_in_order = all_in_order &&
                       sxml_array_get_member(array, member_index, &member) ==
                           SXML_STATUS_OK &&
                       member_integer(member) == (int64_t)member_index;
    }
    SXML_TEST_EXPECT(state, all_in_order);
    sxml_item_release(array);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    array_fixture_destroy(&fixture);
    sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(counting_allocator);
    sxml_counting_allocator_destroy(counting_allocator);
    SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
}

/* An array nested 10^5 deep is freed without deep recursion (D60). */
static void test_deeply_nested_arrays_are_freed(sxml_test_state *state) {
    array_fixture fixture;
    SXML_TEST_REQUIRE(state, array_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_array_make(fixture.item_heap,
                                      (sxml_sequence_view){.sequences = NULL,
                                                           .sequence_count = 0},
                                      &array) == SXML_STATUS_OK);
    for (size_t depth = 0; depth < SXML_TEST_APPEND_COUNT; depth += 1) {
        const sxml_sequence member = sxml_sequence_from_item(array);
        sxml_item outer = {.representation = 0};
        SXML_TEST_REQUIRE(
            state, sxml_array_make(fixture.item_heap,
                                   (sxml_sequence_view){.sequences = &member,
                                                        .sequence_count = 1},
                                   &outer) == SXML_STATUS_OK);
        sxml_item_release(array);
        array = outer;
    }
    SXML_TEST_EXPECT(state,
                     live_cell_count(&fixture) == SXML_TEST_APPEND_COUNT + 1);
    sxml_item_release(array);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    array_fixture_destroy(&fixture);
}

/* ------------------------------------------------ Failures and limits */

/* Makes an array of 12 strings and updates it in every way; any failure
   is returned after everything made is released. */
static sxml_status update_twelve_strings(sxml_item_heap *item_heap) {
    sxml_sequence members[12];
    size_t made_count = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && made_count < 12) {
        status =
            make_string_member(item_heap, SXML_BYTE_VIEW_FROM_LITERAL("string"),
                               &members[made_count]);
        if (status == SXML_STATUS_OK) {
            made_count += 1;
        }
    }
    enum { SXML_TEST_RESULT_COUNT = 7 };
    sxml_item results[SXML_TEST_RESULT_COUNT];
    for (size_t result_index = 0; result_index < SXML_TEST_RESULT_COUNT;
         result_index += 1) {
        results[result_index] = (sxml_item){.representation = 0};
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_make(
            item_heap,
            (sxml_sequence_view){.sequences = members, .sequence_count = 12},
            &results[0]);
    }
    const sxml_sequence member =
        made_count > 0 ? members[0] : sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_array_put(item_heap,
                                &(sxml_array_update){.array = results[0],
                                                     .member_index = 5,
                                                     .member = member},
                                &results[1]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_insert(item_heap,
                                   &(sxml_array_update){.array = results[1],
                                                        .member_index = 2,
                                                        .member = member},
                                   &results[2]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_remove(item_heap,
                                   &(sxml_array_update){.array = results[2],
                                                        .member_index = 7,
                                                        .member = member},
                                   &results[3]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_slice(
            item_heap,
            &(sxml_array_slice_request){.array = results[3],
                                        .first_member_index = 3,
                                        .member_count = 6},
            &results[4]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_append(item_heap,
                                   &(sxml_array_update){.array = results[4],
                                                        .member_index = 0,
                                                        .member = member},
                                   &results[5]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_put(item_heap,
                                &(sxml_array_update){.array = results[5],
                                                     .member_index = 6,
                                                     .member = member},
                                &results[6]);
    }
    for (size_t result_index = 0; result_index < SXML_TEST_RESULT_COUNT;
         result_index += 1) {
        sxml_item_release(results[result_index]);
    }
    for (size_t member_index = 0; member_index < made_count;
         member_index += 1) {
        sxml_sequence_release(members[member_index]);
    }
    return status;
}

/* D23: fail each heap allocation in turn; every failure leaks nothing. */
static void
test_updates_survive_every_allocation_failure(sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(
            state,
            sxml_counting_allocator_create(
                sxml_default_allocator(),
                &(sxml_counting_allocator_configuration){
                    .backing_allocator = sxml_default_allocator(),
                    .failing_allocation_number = failing_allocation_number},
                &counting_allocator) == SXML_STATUS_OK);
        array_fixture fixture;
        SXML_TEST_REQUIRE(
            state, array_fixture_create_with(
                       sxml_counting_allocator_as_allocator(counting_allocator),
                       &fixture) == SXML_STATUS_OK);
        sxml_status status = update_twelve_strings(fixture.item_heap);
        SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
        array_fixture_destroy(&fixture);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 500);
    }
    /* 12 strings, the made array and several nodes per update. */
    SXML_TEST_EXPECT(state, failing_allocation_number > 20);
}

static void test_heap_limit_stops_updates(sxml_test_state *state) {
    array_fixture fixture;
    SXML_TEST_REQUIRE(state, array_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item_heap_destroy(fixture.item_heap);
    SXML_TEST_REQUIRE(
        state, sxml_item_heap_create(sxml_default_allocator(),
                                     &(sxml_item_heap_configuration){
                                         .allocator = sxml_default_allocator(),
                                         .name_pool = fixture.name_pool,
                                         .type_registry = fixture.registry,
                                         .maximum_byte_count = 4096},
                                     &fixture.item_heap) == SXML_STATUS_OK);
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_array_make(fixture.item_heap,
                                      (sxml_sequence_view){.sequences = NULL,
                                                           .sequence_count = 0},
                                      &array) == SXML_STATUS_OK);
    sxml_status status = SXML_STATUS_OK;
    size_t append_count = 0;
    while (status == SXML_STATUS_OK && append_count < 1000) {
        sxml_item longer = {.representation = 0};
        status = sxml_array_append(
            fixture.item_heap,
            &(sxml_array_update){
                .array = array, .member_index = 0, .member = integer_member(1)},
            &longer);
        if (status == SXML_STATUS_OK) {
            sxml_item_release(array);
            array = longer;
            append_count += 1;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_LIMIT_EXCEEDED);
    size_t member_count = 0;
    SXML_TEST_EXPECT(state, sxml_array_get_size(array, &member_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, member_count == append_count);
    sxml_item_release(array);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    array_fixture_destroy(&fixture);
}

static const sxml_test_case xdm_array_test_cases[] = {
    {.name = "make_and_read", .function = test_make_and_read},
    {.name = "empty_array_and_non_arrays",
     .function = test_empty_array_and_non_arrays},
    {.name = "updates_match_a_model", .function = test_updates_match_a_model},
    {.name = "updates_of_a_made_array",
     .function = test_updates_of_a_made_array},
    {.name = "appends_cost_n_log_n", .function = test_appends_cost_n_log_n},
    {.name = "deeply_nested_arrays_are_freed",
     .function = test_deeply_nested_arrays_are_freed},
    {.name = "updates_survive_every_allocation_failure",
     .function = test_updates_survive_every_allocation_failure},
    {.name = "heap_limit_stops_updates",
     .function = test_heap_limit_stops_updates},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xdm_array",
            .cases = xdm_array_test_cases,
            .case_count = SXML_TEST_COUNT(xdm_array_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
