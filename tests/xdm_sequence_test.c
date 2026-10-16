/*
 * Tests for integer ranges, concatenations, lazy sequences and
 * sxml_sequence_test_emptiness (include/sxml/xdm.h, src/xdm_sequence.c;
 * D34, D41, D43, D60, D62).
 *
 * Imitates tests/xdm_test.c: behaviour first, then allocation-failure
 * loops (D23). Every case that makes values checks with
 * sxml_item_heap_read_counts that it released them all.
 */
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xdm_internal.h"

#include <string.h>

/* A name pool, a type registry and an item heap over one allocator. */
typedef struct heap_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} heap_fixture;

static sxml_status heap_fixture_create(sxml_allocator allocator,
                                       heap_fixture *fixture) {
    *fixture = (heap_fixture){
        .name_pool = NULL, .registry = NULL, .item_heap = NULL};
    sxml_status status =
        sxml_name_pool_create(sxml_default_allocator(),
                              &(sxml_name_pool_configuration){
                                  .allocator = sxml_default_allocator(),
                                  .maximum_byte_count = 0},
                              &fixture->name_pool);
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_create(
            sxml_default_allocator(),
            &(sxml_type_registry_configuration){
                .allocator = sxml_default_allocator(),
                .name_pool = fixture->name_pool,
                .maximum_registered_type_count = 0},
            &fixture->registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            sxml_default_allocator(),
            &(sxml_item_heap_configuration){
                .allocator = allocator,
                .name_pool = fixture->name_pool,
                .type_registry = fixture->registry,
                .maximum_byte_count = 0},
            &fixture->item_heap);
    }
    return status;
}

static void heap_fixture_destroy(heap_fixture *fixture) {
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

static sxml_status default_heap_fixture_create(heap_fixture *fixture) {
    return heap_fixture_create(sxml_default_allocator(), fixture);
}

static uint64_t live_cell_count(const heap_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
}

static uint64_t live_byte_count(const heap_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_byte_count;
}

/* An xs:string item, which needs a cell. */
static sxml_status make_string_item(sxml_item_heap *item_heap,
                                    sxml_byte_view text, sxml_item *item) {
    sxml_string string;
    sxml_status status = sxml_string_from_utf8(text, &string);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = string}}},
        item);
}

/* The integer of an inline xs:integer item; INT64_MIN + 7 for anything
   else, a value no test expects. */
static int64_t integer_of(sxml_item item) {
    if (item.representation != SXML_ITEM_REPRESENTATION_INTEGER) {
        return INT64_MIN + 7;
    }
    return item.payload.integer;
}

/* The integer at item_index of sequence, read with sxml_sequence_get_item. */
static int64_t integer_at(sxml_sequence sequence, uint64_t item_index) {
    sxml_item item = {.representation = 0};
    if (sxml_sequence_get_item(
            sequence,
            (sxml_item_request){.item_index = item_index,
                                .error_record = NULL},
            &item) != SXML_STATUS_OK) {
        return INT64_MIN + 7;
    }
    return integer_of(item);
}

static uint64_t count_of(sxml_sequence sequence) {
    uint64_t item_count = UINT64_MAX;
    if (sxml_sequence_count(sequence, NULL, &item_count) != SXML_STATUS_OK) {
        return UINT64_MAX;
    }
    return item_count;
}

static sxml_sequence make_range(sxml_item_heap *item_heap, int64_t first,
                                int64_t last) {
    sxml_sequence sequence = sxml_sequence_empty();
    if (sxml_sequence_make_integer_range(
            item_heap,
            (sxml_integer_range){.first_integer = first, .last_integer = last},
            &sequence) != SXML_STATUS_OK) {
        return sxml_sequence_empty();
    }
    return sequence;
}

static bool error_code_is(const sxml_error_record *record,
                          const char *local_name) {
    return sxml_byte_view_equals(
        record->code.local_name,
        (sxml_byte_view){.bytes = (const uint8_t *)local_name,
                         .byte_count = strlen(local_name)});
}

/* ------------------------------------------------------- Integer ranges */

static void test_ranges_of_zero_and_one_need_no_cell(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_sequence sequence = make_range(fixture.item_heap, 5, 4);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(sequence) ==
                                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    sequence = make_range(fixture.item_heap, -3, -3);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(sequence) ==
                                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM);
    SXML_TEST_EXPECT(state, integer_at(sequence, 0) == -3);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_range_reads_its_integers(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_sequence sequence = make_range(fixture.item_heap, -2, 3);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(sequence) ==
                                SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 1);
    SXML_TEST_EXPECT(state, count_of(sequence) == 6);
    SXML_TEST_EXPECT(state, integer_at(sequence, 0) == -2);
    SXML_TEST_EXPECT(state, integer_at(sequence, 5) == 3);
    sxml_item item = {.representation = 0};
    SXML_TEST_EXPECT(state, sxml_sequence_get_item(
                                sequence,
                                (sxml_item_request){.item_index = 6,
                                                    .error_record = NULL},
                                &item) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    /* XPath 4.0, section 4.8.2: the items are xs:integer. */
    sxml_item first = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_sequence_get_item(
                                 sequence,
                                 (sxml_item_request){.item_index = 0,
                                                     .error_record = NULL},
                                 &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_item_get_type_annotation(first).index ==
                         sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER)
                             .index);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(sequence);
    int64_t expected = -2;
    for (;;) {
        sxml_iteration_step step = {.has_item = false,
                                    .item = {.representation = 0}};
        SXML_TEST_REQUIRE(state, sxml_sequence_iterator_next(
                                     &iterator, NULL, &step) ==
                                     SXML_STATUS_OK);
        if (!step.has_item) {
            break;
        }
        SXML_TEST_EXPECT(state, integer_of(step.item) == expected);
        expected += 1;
    }
    SXML_TEST_EXPECT(state, expected == 4);
    sxml_sequence_release(sequence);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

/* The packet's done criterion: uint64_t counts, right on 32-bit wasm. */
static void test_range_beyond_32_bits_counts_right(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_sequence sequence =
        make_range(fixture.item_heap, INT64_C(100000000000),
                   INT64_C(100000000005));
    SXML_TEST_EXPECT(state, count_of(sequence) == 6);
    SXML_TEST_EXPECT(state, integer_at(sequence, 5) == INT64_C(100000000005));
    sxml_sequence_release(sequence);
    sequence = make_range(fixture.item_heap, 1, INT64_C(100000000000));
    SXML_TEST_EXPECT(state, count_of(sequence) == UINT64_C(100000000000));
    SXML_TEST_EXPECT(state, integer_at(sequence, UINT64_C(99999999999)) ==
                                INT64_C(100000000000));
    sxml_emptiness emptiness = {.is_empty = true};
    SXML_TEST_EXPECT(state, sxml_sequence_test_emptiness(sequence, NULL,
                                                         &emptiness) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !emptiness.is_empty);
    sxml_sequence_release(sequence);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_range_at_int64_limits(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_sequence top = make_range(fixture.item_heap, INT64_MAX - 1, INT64_MAX);
    SXML_TEST_EXPECT(state, count_of(top) == 2);
    SXML_TEST_EXPECT(state, integer_at(top, 1) == INT64_MAX);
    /* The iterator stops after INT64_MAX instead of wrapping. */
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(top);
    uint64_t step_count = 0;
    for (;;) {
        sxml_iteration_step step = {.has_item = false,
                                    .item = {.representation = 0}};
        SXML_TEST_REQUIRE(state, sxml_sequence_iterator_next(
                                     &iterator, NULL, &step) ==
                                     SXML_STATUS_OK);
        if (!step.has_item) {
            break;
        }
        step_count += 1;
        SXML_TEST_REQUIRE(state, step_count <= 2);
    }
    SXML_TEST_EXPECT(state, step_count == 2);
    sxml_sequence_release(top);

    /* All 2^64 integers: every index is readable, the count is not. */
    sxml_sequence whole = make_range(fixture.item_heap, INT64_MIN, INT64_MAX);
    SXML_TEST_EXPECT(state, integer_at(whole, 0) == INT64_MIN);
    SXML_TEST_EXPECT(state, integer_at(whole, UINT64_C(1) << 63) == 0);
    SXML_TEST_EXPECT(state, integer_at(whole, UINT64_MAX) == INT64_MAX);
    sxml_error_record record;
    uint64_t item_count = 7;
    SXML_TEST_EXPECT(state, sxml_sequence_count(whole, &record, &item_count) ==
                                SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&record, "XPDY0130"));
    SXML_TEST_EXPECT(state, item_count == 7);
    sxml_sequence_release(whole);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

/* ------------------------------------------------------- Concatenations */

static void test_concatenation_drops_empty_parts(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_sequence result = make_range(fixture.item_heap, 1, 1);
    sxml_sequence no_parts[] = {sxml_sequence_empty(), sxml_sequence_empty()};
    SXML_TEST_EXPECT(state, sxml_sequence_concatenate(
                                fixture.item_heap,
                                (sxml_sequence_view){.sequences = no_parts,
                                                     .sequence_count = 2},
                                &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(result) ==
                                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    /* One part left: that part itself, retained, no new cell. */
    sxml_sequence range = make_range(fixture.item_heap, 1, 3);
    sxml_sequence one_part[] = {sxml_sequence_empty(), range,
                                sxml_sequence_empty()};
    SXML_TEST_EXPECT(state, sxml_sequence_concatenate(
                                fixture.item_heap,
                                (sxml_sequence_view){.sequences = one_part,
                                                     .sequence_count = 3},
                                &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(result) ==
                                SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 1);
    sxml_sequence_release(range);
    SXML_TEST_EXPECT(state, count_of(result) == 3);
    sxml_sequence_release(result);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_concatenation_flattens_and_reads(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_item text = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string_item(
                                 fixture.item_heap,
                                 SXML_BYTE_VIEW_FROM_LITERAL("text"),
                                 &text) == SXML_STATUS_OK);
    sxml_item pair_items[] = {sxml_atomic_item_from_integer(10),
                              sxml_atomic_item_from_integer(11)};
    sxml_sequence pair = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_sequence_make_vector(
                                 fixture.item_heap,
                                 (sxml_item_view){.items = pair_items,
                                                  .item_count = 2},
                                 &pair) == SXML_STATUS_OK);
    sxml_sequence range = make_range(fixture.item_heap, 1, 3);
    /* (1 to 3, (), "text", (10, 11)) */
    sxml_sequence inputs[] = {range, sxml_sequence_empty(),
                              sxml_sequence_from_item(text), pair};
    sxml_sequence inner = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_sequence_concatenate(
                                 fixture.item_heap,
                                 (sxml_sequence_view){.sequences = inputs,
                                                      .sequence_count = 4},
                                 &inner) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(inner) ==
                                SXML_SEQUENCE_REPRESENTATION_CONCATENATION);
    sxml_sequence_release(range);
    sxml_sequence_release(pair);
    sxml_item_release(text);
    /* range, string, vector and the concatenation itself. */
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 4);
    /* (inner, 0, inner): flattened into 7 parts, one new cell. */
    sxml_sequence outer_inputs[] = {
        inner, sxml_sequence_from_item(sxml_atomic_item_from_integer(0)),
        inner};
    sxml_sequence outer = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state,
                      sxml_sequence_concatenate(
                          fixture.item_heap,
                          (sxml_sequence_view){.sequences = outer_inputs,
                                               .sequence_count = 3},
                          &outer) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 5);
    sxml_sequence_release(inner);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 4);
    SXML_TEST_EXPECT(state, count_of(outer) == 13);
    const int64_t expected[] = {1, 2, 3, -1, 10, 11, 0,
                                1, 2, 3, -1, 10, 11};
    /* Random access, from the end first so the binary search is used. */
    for (uint64_t item_index = 13; item_index > 0; item_index -= 1) {
        int64_t expected_integer = expected[item_index - 1];
        sxml_item item = {.representation = 0};
        SXML_TEST_REQUIRE(state, sxml_sequence_get_item(
                                     outer,
                                     (sxml_item_request){
                                         .item_index = item_index - 1,
                                         .error_record = NULL},
                                     &item) == SXML_STATUS_OK);
        if (expected_integer == -1) {
            SXML_TEST_EXPECT(state, item.representation ==
                                        SXML_ITEM_REPRESENTATION_ATOMIC_CELL);
        } else {
            SXML_TEST_EXPECT(state, integer_of(item) == expected_integer);
        }
    }
    sxml_item beyond = {.representation = 0};
    SXML_TEST_EXPECT(state, sxml_sequence_get_item(
                                outer,
                                (sxml_item_request){.item_index = 13,
                                                    .error_record = NULL},
                                &beyond) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(outer);
    size_t position = 0;
    for (;;) {
        sxml_iteration_step step = {.has_item = false,
                                    .item = {.representation = 0}};
        SXML_TEST_REQUIRE(state, sxml_sequence_iterator_next(
                                     &iterator, NULL, &step) ==
                                     SXML_STATUS_OK);
        if (!step.has_item) {
            break;
        }
        SXML_TEST_REQUIRE(state, position < 13);
        if (expected[position] != -1) {
            SXML_TEST_EXPECT(state,
                             integer_of(step.item) == expected[position]);
        }
        position += 1;
    }
    SXML_TEST_EXPECT(state, position == 13);
    sxml_sequence_release(outer);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_concatenation_count_beyond_uint64(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    /* 2^63 negative integers, then 2^63 non-negative ones. */
    sxml_sequence halves[] = {make_range(fixture.item_heap, INT64_MIN, -1),
                              make_range(fixture.item_heap, 0, INT64_MAX)};
    sxml_sequence both = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_sequence_concatenate(
                                 fixture.item_heap,
                                 (sxml_sequence_view){.sequences = halves,
                                                      .sequence_count = 2},
                                 &both) == SXML_STATUS_OK);
    sxml_sequence_release(halves[0]);
    sxml_sequence_release(halves[1]);
    SXML_TEST_EXPECT(state, integer_at(both, UINT64_MAX) == INT64_MAX);
    SXML_TEST_EXPECT(state, integer_at(both, UINT64_C(1) << 63) == 0);
    sxml_error_record record;
    uint64_t item_count = 7;
    SXML_TEST_EXPECT(state, sxml_sequence_count(both, &record, &item_count) ==
                                SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&record, "XPDY0130"));
    SXML_TEST_EXPECT(state, item_count == 7);
    sxml_sequence_release(both);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

/* ------------------------------------------------------ Lazy sequences */

/*
 * A test generator: yields the integers 0 to item_count - 1 (as strings
 * when makes_strings, so the items have cells), then ends, or raises
 * FOER0000 with a computed description and value instead of item
 * failing_item_index.
 */
typedef struct counting_generator {
    size_t item_count;
    bool makes_strings;
    bool fails;
    size_t failing_item_index;
    /* What the lazy sequence did with it. */
    size_t next_call_count;
    size_t release_call_count;
    /* For the reentry test: the lazy sequence itself, read from next. */
    const sxml_sequence *reentered_sequence;
    sxml_status reentry_status;
} counting_generator;

static sxml_status counting_generator_next(
    void *state, const sxml_generation_request *request,
    sxml_iteration_step *step) {
    counting_generator *generator = state;
    size_t item_index = generator->next_call_count;
    generator->next_call_count += 1;
    if (generator->reentered_sequence != NULL) {
        sxml_item item = {.representation = 0};
        generator->reentry_status = sxml_sequence_get_item(
            *generator->reentered_sequence,
            (sxml_item_request){.item_index = 0, .error_record = NULL},
            &item);
    }
    if (generator->fails && item_index == generator->failing_item_index) {
        uint8_t description[] = {'f', 'a', 'i', 'l', 'e', 'd', ' ', '#',
                                 (uint8_t)('0' + item_index % 10)};
        sxml_item value = {.representation = 0};
        sxml_status status = make_string_item(
            request->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("value"), &value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        status = sxml_item_heap_raise_error(
            request->item_heap,
            &(sxml_error_raise_request){
                .error_status = SXML_STATUS_DYNAMIC_ERROR,
                .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(
                             SXML_ERROR_NAMESPACE_URI),
                         .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FOER0000"),
                         .namespace_prefix =
                             SXML_BYTE_VIEW_FROM_LITERAL("err")},
                .description = {.bytes = description,
                                .byte_count = sizeof(description)},
                .has_value = true,
                .value = sxml_sequence_from_item(value),
                .location = {.source_name =
                                 SXML_BYTE_VIEW_FROM_LITERAL("lazy.xq"),
                             .line_number = 2,
                             .column_number = 5,
                             .byte_offset = 9}},
            request->error_record);
        sxml_item_release(value);
        return status;
    }
    if (item_index >= generator->item_count) {
        *step = (sxml_iteration_step){.has_item = false,
                                      .item = {.representation = 0}};
        return SXML_STATUS_OK;
    }
    sxml_item item = sxml_atomic_item_from_integer((int64_t)item_index);
    if (generator->makes_strings) {
        sxml_status status = make_string_item(
            request->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("item"), &item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *step = (sxml_iteration_step){.has_item = true, .item = item};
    return SXML_STATUS_OK;
}

static void counting_generator_release(void *state) {
    counting_generator *generator = state;
    generator->release_call_count += 1;
}

static const sxml_sequence_generator_operations counting_generator_operations =
    {
        .next = counting_generator_next,
        .release = counting_generator_release,
};

static counting_generator counting_generator_of(size_t item_count) {
    return (counting_generator){.item_count = item_count,
                                .makes_strings = false,
                                .fails = false,
                                .failing_item_index = 0,
                                .next_call_count = 0,
                                .release_call_count = 0,
                                .reentered_sequence = NULL,
                                .reentry_status = SXML_STATUS_OK};
}

static sxml_status make_lazy_over(sxml_item_heap *item_heap,
                                  counting_generator *generator,
                                  sxml_sequence *sequence) {
    return sxml_sequence_make_lazy(
        item_heap,
        (sxml_sequence_generator){.state = generator,
                                  .operations = &counting_generator_operations},
        sequence);
}

static void test_lazy_sequence_rejects_missing_operations(
    sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_sequence sequence = sxml_sequence_empty();
    SXML_TEST_EXPECT(state,
                     sxml_sequence_make_lazy(
                         fixture.item_heap,
                         (sxml_sequence_generator){.state = NULL,
                                                   .operations = NULL},
                         &sequence) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(sequence) ==
                                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    heap_fixture_destroy(&fixture);
}

static void test_lazy_sequence_produces_each_item_once(
    sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    counting_generator generator = counting_generator_of(20);
    generator.makes_strings = true;
    sxml_sequence lazy = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, make_lazy_over(fixture.item_heap, &generator,
                                            &lazy) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(lazy) ==
                                SXML_SEQUENCE_REPRESENTATION_LAZY);
    SXML_TEST_EXPECT(state, generator.next_call_count == 0);
    sxml_emptiness emptiness = {.is_empty = true};
    SXML_TEST_EXPECT(state, sxml_sequence_test_emptiness(lazy, NULL,
                                                         &emptiness) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !emptiness.is_empty);
    SXML_TEST_EXPECT(state, generator.next_call_count == 1);
    sxml_item third = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_sequence_get_item(
                                 lazy,
                                 (sxml_item_request){.item_index = 2,
                                                     .error_record = NULL},
                                 &third) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, generator.next_call_count == 3);
    /* Iterating twice sees the same items and calls the generator once
       per item, plus once for the end. */
    for (size_t pass_index = 0; pass_index < 2; pass_index += 1) {
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(lazy);
        size_t position = 0;
        for (;;) {
            sxml_iteration_step step = {.has_item = false,
                                        .item = {.representation = 0}};
            SXML_TEST_REQUIRE(state, sxml_sequence_iterator_next(
                                         &iterator, NULL, &step) ==
                                         SXML_STATUS_OK);
            if (!step.has_item) {
                break;
            }
            if (position == 2) {
                SXML_TEST_EXPECT(state, step.item.payload.cell ==
                                            third.payload.cell);
            }
            position += 1;
        }
        SXML_TEST_EXPECT(state, position == 20);
    }
    SXML_TEST_EXPECT(state, generator.next_call_count == 21);
    SXML_TEST_EXPECT(state, count_of(lazy) == 20);
    SXML_TEST_EXPECT(state, generator.next_call_count == 21);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 21);
    SXML_TEST_EXPECT(state, generator.release_call_count == 0);
    sxml_sequence_release(lazy);
    SXML_TEST_EXPECT(state, generator.release_call_count == 1);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_lazy_sequence_may_be_empty(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    counting_generator generator = counting_generator_of(0);
    sxml_sequence lazy = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, make_lazy_over(fixture.item_heap, &generator,
                                            &lazy) == SXML_STATUS_OK);
    sxml_emptiness emptiness = {.is_empty = false};
    SXML_TEST_EXPECT(state, sxml_sequence_test_emptiness(lazy, NULL,
                                                         &emptiness) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, emptiness.is_empty);
    SXML_TEST_EXPECT(state, count_of(lazy) == 0);
    SXML_TEST_EXPECT(state, generator.next_call_count == 1);
    sxml_sequence_release(lazy);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

/* Expects the generator's error, FOER0000 "failed #2", in record. */
static void expect_kept_error(sxml_test_state *state, sxml_status status,
                              const sxml_error_record *record) {
    SXML_TEST_EXPECT(state, status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(record, "FOER0000"));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record->diagnostic.description,
                                SXML_BYTE_VIEW_FROM_LITERAL("failed #2")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record->diagnostic.location.source_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("lazy.xq")));
    SXML_TEST_EXPECT(state, record->diagnostic.location.line_number == 2);
    SXML_TEST_EXPECT(state, record->value != NULL);
}

static void test_lazy_sequence_raises_its_error_on_every_read(
    sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    counting_generator generator = counting_generator_of(10);
    generator.fails = true;
    generator.failing_item_index = 2;
    sxml_sequence lazy = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, make_lazy_over(fixture.item_heap, &generator,
                                            &lazy) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, integer_at(lazy, 1) == 1);
    sxml_error_record record;
    sxml_item item = sxml_atomic_item_from_integer(42);
    sxml_status status = sxml_sequence_get_item(
        lazy, (sxml_item_request){.item_index = 2, .error_record = &record},
        &item);
    expect_kept_error(state, status, &record);
    SXML_TEST_EXPECT(state, integer_of(item) == 42);
    /* The heap's raised error is gone; the lazy sequence still has its
       own copy. */
    sxml_item_heap_clear_raised_error(fixture.item_heap);
    uint64_t item_count = 7;
    sxml_error_record count_record;
    status = sxml_sequence_count(lazy, &count_record, &item_count);
    expect_kept_error(state, status, &count_record);
    SXML_TEST_EXPECT(state, item_count == 7);
    /* Items before the error stay readable. */
    SXML_TEST_EXPECT(state, integer_at(lazy, 0) == 0);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(lazy);
    sxml_iteration_step step = {.has_item = false,
                                .item = {.representation = 0}};
    SXML_TEST_EXPECT(state, sxml_sequence_iterator_next(&iterator, NULL,
                                                        &step) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_iterator_next(&iterator, NULL,
                                                        &step) ==
                                SXML_STATUS_OK);
    sxml_error_record iteration_record;
    status = sxml_sequence_iterator_next(&iterator, &iteration_record, &step);
    expect_kept_error(state, status, &iteration_record);
    SXML_TEST_EXPECT(state, iterator.next_item_index == 2);
    /* Without a record, the status alone. */
    SXML_TEST_EXPECT(state, sxml_sequence_get_item(
                                lazy,
                                (sxml_item_request){.item_index = 5,
                                                    .error_record = NULL},
                                &item) == SXML_STATUS_DYNAMIC_ERROR);
    /* The generator ran once per item up to and including its error. */
    SXML_TEST_EXPECT(state, generator.next_call_count == 3);
    sxml_item_heap_clear_raised_error(fixture.item_heap);
    sxml_sequence_release(lazy);
    SXML_TEST_EXPECT(state, generator.release_call_count == 1);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_emptiness_reports_a_first_item_error(
    sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    counting_generator generator = counting_generator_of(10);
    generator.fails = true;
    generator.failing_item_index = 0;
    sxml_sequence lazy = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, make_lazy_over(fixture.item_heap, &generator,
                                            &lazy) == SXML_STATUS_OK);
    /* D43: fn:empty on a lazy sequence raises the generator's error. */
    sxml_emptiness emptiness = {.is_empty = true};
    sxml_error_record record;
    SXML_TEST_EXPECT(state, sxml_sequence_test_emptiness(lazy, &record,
                                                         &emptiness) ==
                                SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&record, "FOER0000"));
    SXML_TEST_EXPECT(state, emptiness.is_empty);
    sxml_item_heap_clear_raised_error(fixture.item_heap);
    sxml_sequence_release(lazy);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_emptiness_of_every_representation(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_emptiness emptiness = {.is_empty = false};
    SXML_TEST_EXPECT(state, sxml_sequence_test_emptiness(
                                sxml_sequence_empty(), NULL, &emptiness) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, emptiness.is_empty);
    SXML_TEST_EXPECT(state, sxml_sequence_test_emptiness(
                                sxml_sequence_from_item(
                                    sxml_atomic_item_true()),
                                NULL, &emptiness) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !emptiness.is_empty);
    sxml_item items[] = {sxml_atomic_item_from_integer(1),
                         sxml_atomic_item_from_integer(2)};
    sxml_sequence vector = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_sequence_make_vector(
                                 fixture.item_heap,
                                 (sxml_item_view){.items = items,
                                                  .item_count = 2},
                                 &vector) == SXML_STATUS_OK);
    emptiness.is_empty = true;
    SXML_TEST_EXPECT(state, sxml_sequence_test_emptiness(vector, NULL,
                                                         &emptiness) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !emptiness.is_empty);
    /* A concatenation whose first part is a lazy empty sequence. */
    counting_generator generator = counting_generator_of(0);
    sxml_sequence lazy = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, make_lazy_over(fixture.item_heap, &generator,
                                            &lazy) == SXML_STATUS_OK);
    sxml_sequence parts[] = {lazy, vector};
    sxml_sequence both = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_sequence_concatenate(
                                 fixture.item_heap,
                                 (sxml_sequence_view){.sequences = parts,
                                                      .sequence_count = 2},
                                 &both) == SXML_STATUS_OK);
    emptiness.is_empty = true;
    SXML_TEST_EXPECT(state, sxml_sequence_test_emptiness(both, NULL,
                                                         &emptiness) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !emptiness.is_empty);
    SXML_TEST_EXPECT(state, integer_at(both, 0) == 1);
    SXML_TEST_EXPECT(state, count_of(both) == 2);
    sxml_sequence_release(both);
    sxml_sequence_release(lazy);
    sxml_sequence_release(vector);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_concatenation_reads_lazy_parts_lazily(
    sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    counting_generator generator = counting_generator_of(10);
    sxml_sequence lazy = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, make_lazy_over(fixture.item_heap, &generator,
                                            &lazy) == SXML_STATUS_OK);
    sxml_sequence parts[] = {make_range(fixture.item_heap, 100, 104), lazy,
                             make_range(fixture.item_heap, 200, 201)};
    sxml_sequence all = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_sequence_concatenate(
                                 fixture.item_heap,
                                 (sxml_sequence_view){.sequences = parts,
                                                      .sequence_count = 3},
                                 &all) == SXML_STATUS_OK);
    for (size_t part_index = 0; part_index < 3; part_index += 1) {
        sxml_sequence_release(parts[part_index]);
    }
    SXML_TEST_EXPECT(state, integer_at(all, 4) == 104);
    SXML_TEST_EXPECT(state, generator.next_call_count == 0);
    SXML_TEST_EXPECT(state, integer_at(all, 7) == 2);
    SXML_TEST_EXPECT(state, generator.next_call_count == 3);
    SXML_TEST_EXPECT(state, integer_at(all, 16) == 201);
    SXML_TEST_EXPECT(state, generator.next_call_count == 11);
    SXML_TEST_EXPECT(state, integer_at(all, 6) == 1);
    SXML_TEST_EXPECT(state, count_of(all) == 17);
    SXML_TEST_EXPECT(state, generator.next_call_count == 11);
    sxml_sequence_release(all);
    SXML_TEST_EXPECT(state, generator.release_call_count == 1);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_lazy_sequence_refuses_reentry(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    counting_generator generator = counting_generator_of(1);
    sxml_sequence lazy = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, make_lazy_over(fixture.item_heap, &generator,
                                            &lazy) == SXML_STATUS_OK);
    generator.reentered_sequence = &lazy;
    SXML_TEST_EXPECT(state, integer_at(lazy, 0) == 0);
    SXML_TEST_EXPECT(state,
                     generator.reentry_status == SXML_STATUS_INVALID_ARGUMENT);
    sxml_sequence_release(lazy);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Allocation failures */

/*
 * Makes a range, a lazy sequence of 12 strings that fails at item 10, and
 * their concatenation, and reads it all; any failure other than the
 * expected FOER0000 is returned after everything made is released.
 */
static sxml_status exercise_sequences(sxml_item_heap *item_heap,
                                      counting_generator *generator) {
    sxml_sequence range = sxml_sequence_empty();
    sxml_status status = sxml_sequence_make_integer_range(
        item_heap,
        (sxml_integer_range){.first_integer = 1, .last_integer = 4}, &range);
    sxml_sequence lazy = sxml_sequence_empty();
    bool is_lazy_made = false;
    if (status == SXML_STATUS_OK) {
        status = make_lazy_over(item_heap, generator, &lazy);
        is_lazy_made = status == SXML_STATUS_OK;
    }
    sxml_sequence all = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        sxml_sequence parts[] = {range, lazy, range};
        status = sxml_sequence_concatenate(
            item_heap,
            (sxml_sequence_view){.sequences = parts, .sequence_count = 3},
            &all);
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(all);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false,
                                    .item = {.representation = 0}};
        sxml_error_record record;
        status = sxml_sequence_iterator_next(&iterator, &record, &step);
        if (status == SXML_STATUS_DYNAMIC_ERROR &&
            error_code_is(&record, "FOER0000")) {
            /* Again, from the kept copy. */
            uint64_t item_count = 0;
            status = sxml_sequence_count(all, &record, &item_count);
            if (status == SXML_STATUS_DYNAMIC_ERROR &&
                error_code_is(&record, "FOER0000")) {
                status = SXML_STATUS_OK;
            }
            break;
        }
        if (status == SXML_STATUS_OK && !step.has_item) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    sxml_item_heap_clear_raised_error(item_heap);
    sxml_sequence_release(all);
    sxml_sequence_release(lazy);
    sxml_sequence_release(range);
    if (!is_lazy_made) {
        /* The state was not taken (xdm.h): release it here. */
        counting_generator_release(generator);
    }
    return status;
}

/* D23: fail each heap allocation in turn; every failure leaks nothing,
   and the generator's state is released exactly once. */
static void test_sequences_survive_every_allocation_failure(
    sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(state,
                          sxml_counting_allocator_create(
                              sxml_default_allocator(),
                              &(sxml_counting_allocator_configuration){
                                  .backing_allocator = sxml_default_allocator(),
                                  .failing_allocation_number =
                                      failing_allocation_number},
                              &counting_allocator) == SXML_STATUS_OK);
        heap_fixture fixture;
        SXML_TEST_REQUIRE(
            state,
            heap_fixture_create(
                sxml_counting_allocator_as_allocator(counting_allocator),
                &fixture) == SXML_STATUS_OK);
        counting_generator generator = counting_generator_of(12);
        generator.makes_strings = true;
        generator.fails = true;
        generator.failing_item_index = 10;
        sxml_status status = exercise_sequences(fixture.item_heap, &generator);
        SXML_TEST_EXPECT(state, generator.release_call_count == 1);
        SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
        SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
        heap_fixture_destroy(&fixture);
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
        SXML_TEST_REQUIRE(state, failing_allocation_number < 200);
    }
    /* Range, lazy, concatenation, 10 strings, item arrays, the error. */
    SXML_TEST_EXPECT(state, failing_allocation_number > 15);
}

static const sxml_test_case xdm_sequence_test_cases[] = {
    {.name = "ranges_of_zero_and_one_need_no_cell",
     .function = test_ranges_of_zero_and_one_need_no_cell},
    {.name = "range_reads_its_integers",
     .function = test_range_reads_its_integers},
    {.name = "range_beyond_32_bits_counts_right",
     .function = test_range_beyond_32_bits_counts_right},
    {.name = "range_at_int64_limits", .function = test_range_at_int64_limits},
    {.name = "concatenation_drops_empty_parts",
     .function = test_concatenation_drops_empty_parts},
    {.name = "concatenation_flattens_and_reads",
     .function = test_concatenation_flattens_and_reads},
    {.name = "concatenation_count_beyond_uint64",
     .function = test_concatenation_count_beyond_uint64},
    {.name = "lazy_sequence_rejects_missing_operations",
     .function = test_lazy_sequence_rejects_missing_operations},
    {.name = "lazy_sequence_produces_each_item_once",
     .function = test_lazy_sequence_produces_each_item_once},
    {.name = "lazy_sequence_may_be_empty",
     .function = test_lazy_sequence_may_be_empty},
    {.name = "lazy_sequence_raises_its_error_on_every_read",
     .function = test_lazy_sequence_raises_its_error_on_every_read},
    {.name = "emptiness_reports_a_first_item_error",
     .function = test_emptiness_reports_a_first_item_error},
    {.name = "emptiness_of_every_representation",
     .function = test_emptiness_of_every_representation},
    {.name = "concatenation_reads_lazy_parts_lazily",
     .function = test_concatenation_reads_lazy_parts_lazily},
    {.name = "lazy_sequence_refuses_reentry",
     .function = test_lazy_sequence_refuses_reentry},
    {.name = "sequences_survive_every_allocation_failure",
     .function = test_sequences_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xdm_sequence",
            .cases = xdm_sequence_test_cases,
            .case_count = SXML_TEST_COUNT(xdm_sequence_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
