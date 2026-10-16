/*
 * Tests for the item heap, cells, generic items, raised errors, vector
 * sequences, the sequence read interface and the sequence builder
 * (include/sxml/xdm.h, src/xdm_internal.h; D33, D34, D41, D60).
 *
 * Imitates tests/memory_test.c: lifecycle checks, then behaviour, then
 * allocation-failure and limit loops (D23). Every case that makes values
 * checks with sxml_item_heap_read_counts that it released them all.
 */
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* Caller memory for objects placed with initialize. */
#define TEST_STORAGE_ALIGNMENT 16
#define TEST_STORAGE_BYTE_COUNT 1024

/* A name pool, a type registry and an item heap over one allocator. */
typedef struct heap_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} heap_fixture;

typedef struct heap_fixture_request {
    sxml_allocator allocator;
    size_t maximum_byte_count;
} heap_fixture_request;

static sxml_status heap_fixture_create(heap_fixture_request request,
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
                .allocator = request.allocator,
                .name_pool = fixture->name_pool,
                .type_registry = fixture->registry,
                .maximum_byte_count = request.maximum_byte_count},
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
    return heap_fixture_create(
        (heap_fixture_request){.allocator = sxml_default_allocator(),
                               .maximum_byte_count = 0},
        fixture);
}

static uint64_t live_cell_count(const heap_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
}

static uint64_t live_byte_count(const heap_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_byte_count;
}

/* An xs:string item made through sxml_atomic_item_make, which needs a
   cell; the cell lets these tests see retain and release. */
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

/* ------------------------------------------------------------ Item heap */

static void test_heap_rejects_invalid_configuration(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_memory_requirements requirements = {.byte_count = 1,
                                             .alignment = 1};
    sxml_item_heap_configuration configuration = {
        .allocator = {.state = NULL, .operations = NULL},
        .name_pool = fixture.name_pool,
        .type_registry = fixture.registry,
        .maximum_byte_count = 0,
    };
    SXML_TEST_EXPECT(state, sxml_item_heap_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    configuration.allocator = sxml_default_allocator();
    configuration.name_pool = NULL;
    SXML_TEST_EXPECT(state, sxml_item_heap_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    configuration.name_pool = fixture.name_pool;
    configuration.type_registry = NULL;
    SXML_TEST_EXPECT(state, sxml_item_heap_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, requirements.byte_count == 1);
    heap_fixture_destroy(&fixture);
}

static void test_heap_initializes_in_caller_memory(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_item_heap_configuration configuration = {
        .allocator = sxml_default_allocator(),
        .name_pool = fixture.name_pool,
        .type_registry = fixture.registry,
        .maximum_byte_count = 0,
    };
    sxml_memory_requirements requirements = {.byte_count = 0,
                                             .alignment = 1};
    SXML_TEST_REQUIRE(state, sxml_item_heap_memory_requirements(
                                 &configuration, &requirements) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      requirements.byte_count <= TEST_STORAGE_BYTE_COUNT);
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[TEST_STORAGE_BYTE_COUNT];
    sxml_item_heap *item_heap = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_item_heap_initialize(
                         (sxml_memory_view){.bytes = storage,
                                            .byte_count =
                                                requirements.byte_count - 1},
                         &configuration,
                         &item_heap) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, sxml_item_heap_initialize(
                                (sxml_memory_view){
                                    .bytes = storage + 1,
                                    .byte_count = requirements.byte_count},
                                &configuration, &item_heap) ==
                                SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, item_heap == NULL);
    SXML_TEST_REQUIRE(state, sxml_item_heap_initialize(
                                 (sxml_memory_view){
                                     .bytes = storage,
                                     .byte_count = requirements.byte_count},
                                 &configuration,
                                 &item_heap) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_item_heap_get_name_pool(item_heap) ==
                         fixture.name_pool);
    SXML_TEST_EXPECT(state, sxml_item_heap_get_type_registry(item_heap) ==
                                fixture.registry);
    sxml_item_heap_counts counts = sxml_item_heap_read_counts(item_heap);
    SXML_TEST_EXPECT(state, counts.live_cell_count == 0);
    SXML_TEST_EXPECT(state, counts.live_byte_count == 0);
    SXML_TEST_EXPECT(state, counts.issued_tree_identifier_count == 0);
    sxml_item_heap_finalize(item_heap);
    sxml_item_heap_finalize(NULL);
    sxml_item_heap_destroy(NULL);
    heap_fixture_destroy(&fixture);
}

static void test_tree_identifiers_increase(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_tree_identifier first =
        sxml_item_heap_issue_tree_identifier(fixture.item_heap);
    sxml_tree_identifier second =
        sxml_item_heap_issue_tree_identifier(fixture.item_heap);
    SXML_TEST_EXPECT(state, first.value != 0);
    SXML_TEST_EXPECT(state, second.value > first.value);
    SXML_TEST_EXPECT(
        state, sxml_item_heap_read_counts(fixture.item_heap)
                       .issued_tree_identifier_count == 2);
    heap_fixture_destroy(&fixture);
}

/* ----------------------------------------------------------------- Items */

static void test_inline_items_need_no_memory(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_item integer = sxml_atomic_item_from_integer(42);
    sxml_item_retain(integer);
    sxml_item_release(integer);
    sxml_item_release(integer);
    SXML_TEST_EXPECT(state,
                     sxml_item_get_kind(integer) == SXML_ITEM_KIND_ATOMIC);
    SXML_TEST_EXPECT(state, !sxml_item_is_function_item(integer));
    SXML_TEST_EXPECT(state, !sxml_item_is_generic_node(integer));
    SXML_TEST_EXPECT(state, sxml_item_get_type_annotation(integer).index ==
                                SXML_TYPE_XS_INTEGER);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_cell_items_are_counted_until_released(
    sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_item item = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string_item(fixture.item_heap,
                                              SXML_BYTE_VIEW_FROM_LITERAL(
                                                  "cell"),
                                              &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 1);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) > 4);
    sxml_item_retain(item);
    sxml_item_release(item);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 1);
    sxml_item_release(item);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

/* A test cell that holds one reference to another cell, or none. */
typedef struct chain_cell {
    sxml_cell cell;
    sxml_cell *next_cell;
} chain_cell;

static void chain_cell_release_contents(sxml_cell *cell) {
    chain_cell *link = (chain_cell *)cell;
    if (link->next_cell != NULL) {
        sxml_cell_release(link->next_cell);
    }
}

static const sxml_cell_class chain_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("chain"),
    .release_contents = chain_cell_release_contents,
    .sequence_operations = NULL,
};

/* A chain of a million cells, each holding the next: releasing the head
   must not recurse once per cell (the pending list of src/xdm.c). */
static void test_deep_values_release_without_recursion(
    sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    const size_t link_count = 1000000;
    sxml_cell *head = NULL;
    for (size_t link_index = 0; link_index < link_count; link_index += 1) {
        sxml_cell *cell = NULL;
        SXML_TEST_REQUIRE(state,
                          sxml_item_heap_allocate_cell(
                              fixture.item_heap,
                              &(sxml_cell_request){
                                  .cell_class = &chain_cell_class,
                                  .byte_count = sizeof(chain_cell),
                                  .alignment = alignof(chain_cell)},
                              &cell) == SXML_STATUS_OK);
        ((chain_cell *)cell)->next_cell = head;
        head = cell;
    }
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == link_count);
    SXML_TEST_EXPECT(state, sxml_cell_get_item_heap(head) == fixture.item_heap);
    sxml_cell_release(head);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_immortal_cells_ignore_counting(sxml_test_state *state) {
    static chain_cell immortal = {
        .cell = {.cell_class = &chain_cell_class,
                 .item_heap = NULL,
                 .byte_count = sizeof(chain_cell),
                 .state = {.reference_count =
                               SXML_CELL_IMMORTAL_REFERENCE_COUNT}},
        .next_cell = NULL,
    };
    sxml_cell_retain(&immortal.cell);
    sxml_cell_release(&immortal.cell);
    sxml_cell_release(&immortal.cell);
    SXML_TEST_EXPECT(state, immortal.cell.state.reference_count ==
                                SXML_CELL_IMMORTAL_REFERENCE_COUNT);
}

static void test_cell_requests_are_checked(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_cell *cell = NULL;
    SXML_TEST_EXPECT(state, sxml_item_heap_allocate_cell(
                                fixture.item_heap,
                                &(sxml_cell_request){
                                    .cell_class = &chain_cell_class,
                                    .byte_count = sizeof(sxml_cell) - 1,
                                    .alignment = alignof(sxml_cell)},
                                &cell) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_item_heap_allocate_cell(
                                fixture.item_heap,
                                &(sxml_cell_request){
                                    .cell_class = &chain_cell_class,
                                    .byte_count = sizeof(chain_cell),
                                    .alignment = 3},
                                &cell) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_item_heap_allocate_cell(
                                fixture.item_heap,
                                &(sxml_cell_request){
                                    .cell_class = NULL,
                                    .byte_count = sizeof(chain_cell),
                                    .alignment = alignof(chain_cell)},
                                &cell) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, cell == NULL);
    SXML_TEST_REQUIRE(state, sxml_item_heap_allocate_cell(
                                 fixture.item_heap,
                                 &(sxml_cell_request){
                                     .cell_class = &chain_cell_class,
                                     .byte_count = sizeof(chain_cell),
                                     .alignment = alignof(chain_cell)},
                                 &cell) == SXML_STATUS_OK);
    sxml_item_heap_discard_cell(cell);
    sxml_item_heap_discard_cell(NULL);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_heap_stops_at_maximum_byte_count(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      heap_fixture_create(
                          (heap_fixture_request){
                              .allocator = sxml_default_allocator(),
                              .maximum_byte_count = 2 * sizeof(chain_cell)},
                          &fixture) == SXML_STATUS_OK);
    sxml_cell *cells[3] = {NULL, NULL, NULL};
    sxml_cell_request request = {.cell_class = &chain_cell_class,
                                 .byte_count = sizeof(chain_cell),
                                 .alignment = alignof(chain_cell)};
    SXML_TEST_EXPECT(state, sxml_item_heap_allocate_cell(
                                fixture.item_heap, &request, &cells[0]) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_item_heap_allocate_cell(
                                fixture.item_heap, &request, &cells[1]) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_item_heap_allocate_cell(
                                fixture.item_heap, &request, &cells[2]) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, cells[2] == NULL);
    ((chain_cell *)cells[0])->next_cell = NULL;
    ((chain_cell *)cells[1])->next_cell = NULL;
    sxml_cell_release(cells[0]);
    /* Released bytes are available again. */
    SXML_TEST_EXPECT(state, sxml_item_heap_allocate_cell(
                                fixture.item_heap, &request, &cells[2]) ==
                                SXML_STATUS_OK);
    ((chain_cell *)cells[2])->next_cell = NULL;
    sxml_cell_release(cells[1]);
    sxml_cell_release(cells[2]);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_heap_allocator_counts_hidden_headers(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_allocator allocator = sxml_item_heap_as_allocator(fixture.item_heap);
    void *small = NULL;
    void *aligned = NULL;
    SXML_TEST_REQUIRE(state, sxml_allocator_allocate(
                                 allocator,
                                 (sxml_allocation_request){.byte_count = 3,
                                                           .alignment = 1},
                                 &small) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_allocator_allocate(
                          allocator,
                          (sxml_allocation_request){.byte_count = 100,
                                                    .alignment = 64},
                          &aligned) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, ((uintptr_t)aligned & 63U) == 0);
    memset(small, 1, 3);
    memset(aligned, 2, 100);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) >= 103);
    sxml_allocator_release(allocator, small);
    sxml_allocator_release(allocator, aligned);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    const sxml_decimal_configuration *decimal_configuration =
        sxml_item_heap_get_decimal_configuration(fixture.item_heap);
    SXML_TEST_EXPECT(state, decimal_configuration->allocator.state ==
                                fixture.item_heap);
    heap_fixture_destroy(&fixture);
}

/* ------------------------------------------------------------ Sequences */

static void test_empty_and_single_item_sequences(sxml_test_state *state) {
    sxml_sequence zero = {.content = {.representation = 0}};
    sxml_sequence empty = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(zero) ==
                                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(empty) ==
                                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    uint64_t item_count = 99;
    SXML_TEST_EXPECT(state, sxml_sequence_count(empty, NULL, &item_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item_count == 0);
    sxml_item item = {.representation = 0};
    SXML_TEST_EXPECT(state,
                     sxml_sequence_get_item(
                         empty,
                         (sxml_item_request){.item_index = 0,
                                             .error_record = NULL},
                         &item) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    sxml_sequence single =
        sxml_sequence_from_item(sxml_atomic_item_from_integer(7));
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(single) ==
                                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM);
    SXML_TEST_EXPECT(state, sxml_sequence_count(single, NULL, &item_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item_count == 1);
    SXML_TEST_EXPECT(state, sxml_sequence_get_item(
                                single,
                                (sxml_item_request){.item_index = 0,
                                                    .error_record = NULL},
                                &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_atomic_equal(
                                item, sxml_atomic_item_from_integer(7)));
    SXML_TEST_EXPECT(state,
                     sxml_sequence_get_item(
                         single,
                         (sxml_item_request){.item_index = 1,
                                             .error_record = NULL},
                         &item) == SXML_STATUS_INDEX_OUT_OF_RANGE);
}

/* Reads every item of sequence through an iterator into items. */
static size_t iterate_into(sxml_sequence sequence, sxml_item *items,
                           size_t capacity) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(sequence);
    size_t item_count = 0;
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        if (sxml_sequence_iterator_next(&iterator, NULL, &step) !=
                SXML_STATUS_OK ||
            !step.has_item || item_count == capacity) {
            return item_count;
        }
        items[item_count] = step.item;
        item_count += 1;
    }
}

static void test_vectors_hold_their_items(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_item text = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string_item(fixture.item_heap,
                                              SXML_BYTE_VIEW_FROM_LITERAL("b"),
                                              &text) == SXML_STATUS_OK);
    const sxml_item items[3] = {sxml_atomic_item_from_integer(1), text,
                                sxml_atomic_item_from_double(3.0)};
    sxml_sequence vector = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state,
                      sxml_sequence_make_vector(
                          fixture.item_heap,
                          (sxml_item_view){.items = items, .item_count = 3},
                          &vector) == SXML_STATUS_OK);
    /* The vector retained the string: the caller's reference can go. */
    sxml_item_release(text);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 2);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(vector) ==
                                SXML_SEQUENCE_REPRESENTATION_VECTOR);
    uint64_t item_count = 0;
    SXML_TEST_EXPECT(state, sxml_sequence_count(vector, NULL, &item_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item_count == 3);
    sxml_item item = {.representation = 0};
    SXML_TEST_EXPECT(state, sxml_sequence_get_item(
                                vector,
                                (sxml_item_request){.item_index = 2,
                                                    .error_record = NULL},
                                &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_atomic_equal(
                                item, sxml_atomic_item_from_integer(3)));
    SXML_TEST_EXPECT(state,
                     sxml_sequence_get_item(
                         vector,
                         (sxml_item_request){.item_index = 3,
                                             .error_record = NULL},
                         &item) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    sxml_item read_items[4] = {{.representation = 0}};
    SXML_TEST_EXPECT(state, iterate_into(vector, read_items, 4) == 3);
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_atomic_equal(read_items[1],
                                                             items[1]));
    sxml_sequence_retain(vector);
    sxml_sequence_release(vector);
    sxml_sequence_release(vector);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_short_vectors_need_no_cell(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_sequence sequence = sxml_sequence_from_item(
        sxml_atomic_item_from_integer(1));
    SXML_TEST_EXPECT(state,
                     sxml_sequence_make_vector(
                         fixture.item_heap,
                         (sxml_item_view){.items = NULL, .item_count = 0},
                         &sequence) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(sequence) ==
                                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    sxml_item text = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string_item(fixture.item_heap,
                                              SXML_BYTE_VIEW_FROM_LITERAL("x"),
                                              &text) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_sequence_make_vector(
                         fixture.item_heap,
                         (sxml_item_view){.items = &text, .item_count = 1},
                         &sequence) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(sequence) ==
                                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 1);
    sxml_item_release(text);
    sxml_sequence_release(sequence);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_iterator_walks_single_item_once(sxml_test_state *state) {
    sxml_sequence single =
        sxml_sequence_from_item(sxml_atomic_item_true());
    sxml_item read_items[2] = {{.representation = 0}};
    SXML_TEST_EXPECT(state, iterate_into(single, read_items, 2) == 1);
    SXML_TEST_EXPECT(state,
                     iterate_into(sxml_sequence_empty(), read_items, 2) == 0);
}

/* ------------------------------------------------------ Sequence builder */

static void test_builder_lifecycle(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_sequence_builder_configuration configuration = {.item_heap = NULL};
    sxml_memory_requirements requirements = {.byte_count = 0,
                                             .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_sequence_builder_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    configuration.item_heap = fixture.item_heap;
    SXML_TEST_REQUIRE(state, sxml_sequence_builder_memory_requirements(
                                 &configuration, &requirements) ==
                                 SXML_STATUS_OK);
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[TEST_STORAGE_BYTE_COUNT];
    SXML_TEST_REQUIRE(state,
                      requirements.byte_count <= TEST_STORAGE_BYTE_COUNT);
    sxml_sequence_builder *builder = NULL;
    SXML_TEST_EXPECT(state, sxml_sequence_builder_initialize(
                                (sxml_memory_view){
                                    .bytes = storage + 1,
                                    .byte_count = requirements.byte_count},
                                &configuration, &builder) ==
                                SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_REQUIRE(state, sxml_sequence_builder_initialize(
                                 (sxml_memory_view){
                                     .bytes = storage,
                                     .byte_count = requirements.byte_count},
                                 &configuration,
                                 &builder) == SXML_STATUS_OK);
    sxml_item text = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string_item(fixture.item_heap,
                                              SXML_BYTE_VIEW_FROM_LITERAL("t"),
                                              &text) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_builder_append_item(builder, text) ==
                                SXML_STATUS_OK);
    sxml_item_release(text);
    /* Finalizing releases what was collected and not finished. */
    sxml_sequence_builder_finalize(builder);
    sxml_sequence_builder_finalize(NULL);
    sxml_sequence_builder_destroy(NULL);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_builder_collects_and_is_reused(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_sequence_builder *builder = NULL;
    SXML_TEST_REQUIRE(state, sxml_sequence_builder_create(
                                 sxml_default_allocator(),
                                 &(sxml_sequence_builder_configuration){
                                     .item_heap = fixture.item_heap},
                                 &builder) == SXML_STATUS_OK);
    sxml_sequence result = sxml_sequence_from_item(sxml_atomic_item_true());
    SXML_TEST_EXPECT(state, sxml_sequence_builder_finish(builder, &result) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(result) ==
                                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    for (int64_t integer = 0; integer < 100; integer += 1) {
        SXML_TEST_REQUIRE(state, sxml_sequence_builder_append_item(
                                     builder, sxml_atomic_item_from_integer(
                                                  integer)) == SXML_STATUS_OK);
    }
    SXML_TEST_REQUIRE(state, sxml_sequence_builder_finish(builder, &result) ==
                                 SXML_STATUS_OK);
    uint64_t item_count = 0;
    SXML_TEST_EXPECT(state, sxml_sequence_count(result, NULL, &item_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item_count == 100);
    sxml_item item = {.representation = 0};
    SXML_TEST_EXPECT(state, sxml_sequence_get_item(
                                result,
                                (sxml_item_request){.item_index = 99,
                                                    .error_record = NULL},
                                &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_atomic_equal(
                                item, sxml_atomic_item_from_integer(99)));
    /* Reused: appending a sequence copies its items. */
    SXML_TEST_EXPECT(state, sxml_sequence_builder_append_sequence(
                                builder, result, NULL) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_builder_append_sequence(
                                builder, sxml_sequence_empty(), NULL) ==
                                SXML_STATUS_OK);
    sxml_sequence copy = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_sequence_builder_finish(builder, &copy) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_count(copy, NULL, &item_count) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item_count == 100);
    sxml_sequence_release(result);
    sxml_sequence_release(copy);
    /* A single item finishes as a single-item sequence. */
    sxml_item text = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string_item(fixture.item_heap,
                                              SXML_BYTE_VIEW_FROM_LITERAL("s"),
                                              &text) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_builder_append_item(builder, text) ==
                                SXML_STATUS_OK);
    sxml_item_release(text);
    SXML_TEST_EXPECT(state, sxml_sequence_builder_finish(builder, &result) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(result) ==
                                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM);
    sxml_sequence_release(result);
    sxml_sequence_builder_destroy(builder);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

/* Builds the vector of 20 strings and reads it back; any failure is
   returned after everything made is released. */
static sxml_status build_twenty_strings(sxml_item_heap *item_heap) {
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_default_allocator(),
        &(sxml_sequence_builder_configuration){.item_heap = item_heap},
        &builder);
    for (size_t string_index = 0;
         status == SXML_STATUS_OK && string_index < 20; string_index += 1) {
        sxml_item text = {.representation = 0};
        status = make_string_item(item_heap,
                                  SXML_BYTE_VIEW_FROM_LITERAL("string"), &text);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, text);
            sxml_item_release(text);
        }
    }
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, &result);
    }
    sxml_sequence copy = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_sequence(builder, result, NULL);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, &copy);
    }
    sxml_sequence_release(copy);
    sxml_sequence_release(result);
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* D23: fail each heap allocation in turn; every failure leaks nothing. */
static void test_builder_survives_every_allocation_failure(
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
                (heap_fixture_request){
                    .allocator =
                        sxml_counting_allocator_as_allocator(counting_allocator),
                    .maximum_byte_count = 0},
                &fixture) == SXML_STATUS_OK);
        sxml_status status = build_twenty_strings(fixture.item_heap);
        SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
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
    /* 20 strings, item arrays of 8, 16 and 32, two vectors. */
    SXML_TEST_EXPECT(state, failing_allocation_number > 20);
}

/* -------------------------------------------------------- Raised errors */

static void test_raised_error_is_copied_and_kept(sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_item value_item = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string_item(fixture.item_heap,
                                              SXML_BYTE_VIEW_FROM_LITERAL(
                                                  "value"),
                                              &value_item) == SXML_STATUS_OK);
    uint8_t description[] = {'c', 'o', 'm', 'p', 'u', 't', 'e', 'd'};
    sxml_error_record record;
    sxml_status status = sxml_item_heap_raise_error(
        fixture.item_heap,
        &(sxml_error_raise_request){
            .error_status = SXML_STATUS_DYNAMIC_ERROR,
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(
                         "http://example.com/"),
                     .local_name = SXML_BYTE_VIEW_FROM_LITERAL("E1"),
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("x")},
            .description = {.bytes = description,
                            .byte_count = sizeof(description)},
            .has_value = true,
            .value = sxml_sequence_from_item(value_item),
            .location = {.source_name = SXML_BYTE_VIEW_FROM_LITERAL("q.xq"),
                         .line_number = 3,
                         .column_number = 4,
                         .byte_offset = 20}},
        &record);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_DYNAMIC_ERROR);
    /* The heap keeps its own copy and its own reference. */
    memset(description, 'X', sizeof(description));
    sxml_item_release(value_item);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.diagnostic.description,
                                SXML_BYTE_VIEW_FROM_LITERAL("computed")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.code.local_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("E1")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.diagnostic.location.source_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("q.xq")));
    SXML_TEST_EXPECT(state, record.diagnostic.location.line_number == 3);
    SXML_TEST_REQUIRE(state, record.value != NULL);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(*record.value) ==
                                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 1);
    /* Raising again from the record's own views replaces the error. */
    sxml_error_record second_record;
    status = sxml_item_heap_raise_error(
        fixture.item_heap,
        &(sxml_error_raise_request){.error_status = SXML_STATUS_TYPE_ERROR,
                                    .code = record.code,
                                    .description =
                                        record.diagnostic.description,
                                    .has_value = false,
                                    .value = sxml_sequence_empty(),
                                    .location = record.diagnostic.location},
        &second_record);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(state, second_record.value == NULL);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                second_record.diagnostic.description,
                                SXML_BYTE_VIEW_FROM_LITERAL("computed")));
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    sxml_item_heap_clear_raised_error(fixture.item_heap);
    sxml_item_heap_clear_raised_error(fixture.item_heap);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    heap_fixture_destroy(&fixture);
}

static void test_raise_error_checks_status_and_null_record(
    sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state, default_heap_fixture_create(&fixture) ==
                                 SXML_STATUS_OK);
    sxml_error_raise_request request = {
        .error_status = SXML_STATUS_OUT_OF_MEMORY,
        .code = {.namespace_uri = {.bytes = NULL, .byte_count = 0},
                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FOER0000"),
                 .namespace_prefix = {.bytes = NULL, .byte_count = 0}},
        .description = {.bytes = NULL, .byte_count = 0},
        .has_value = false,
        .value = sxml_sequence_empty(),
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    sxml_error_record record;
    SXML_TEST_EXPECT(state, sxml_item_heap_raise_error(fixture.item_heap,
                                                       &request, &record) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    request.error_status = SXML_STATUS_DYNAMIC_ERROR;
    SXML_TEST_EXPECT(state, sxml_item_heap_raise_error(fixture.item_heap,
                                                       &request, NULL) ==
                                SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    /* The error the heap keeps lasts until the heap ends. */
    SXML_TEST_EXPECT(state, sxml_item_heap_raise_error(fixture.item_heap,
                                                       &request, &record) ==
                                SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 8);
    heap_fixture_destroy(&fixture);
}

static void test_raise_error_failure_keeps_previous_error(
    sxml_test_state *state) {
    heap_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      heap_fixture_create(
                          (heap_fixture_request){
                              .allocator = sxml_default_allocator(),
                              .maximum_byte_count = 10},
                          &fixture) == SXML_STATUS_OK);
    sxml_error_raise_request request = {
        .error_status = SXML_STATUS_DYNAMIC_ERROR,
        .code = {.namespace_uri = {.bytes = NULL, .byte_count = 0},
                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FOER0000"),
                 .namespace_prefix = {.bytes = NULL, .byte_count = 0}},
        .description = {.bytes = NULL, .byte_count = 0},
        .has_value = false,
        .value = sxml_sequence_empty(),
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    sxml_error_record record;
    SXML_TEST_REQUIRE(state, sxml_item_heap_raise_error(fixture.item_heap,
                                                        &request, &record) ==
                                 SXML_STATUS_DYNAMIC_ERROR);
    sxml_error_record unchanged = record;
    request.description = SXML_BYTE_VIEW_FROM_LITERAL("too long to fit");
    SXML_TEST_EXPECT(state, sxml_item_heap_raise_error(fixture.item_heap,
                                                       &request, &record) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, record.code.local_name.bytes ==
                                unchanged.code.local_name.bytes);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.code.local_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("FOER0000")));
    heap_fixture_destroy(&fixture);
}

static void test_static_error_report_fills_record(sxml_test_state *state) {
    static const sxml_static_error error = {
        .error_status = SXML_STATUS_TYPE_ERROR,
        .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
        .description = SXML_BYTE_VIEW_INITIALIZER("wrong type"),
    };
    sxml_error_record record;
    SXML_TEST_EXPECT(state, sxml_error_record_report(&record, &error) ==
                                SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.code.namespace_uri,
                                SXML_BYTE_VIEW_FROM_LITERAL(
                                    SXML_ERROR_NAMESPACE_URI)));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.code.local_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("XPTY0004")));
    SXML_TEST_EXPECT(state, record.value == NULL);
    SXML_TEST_EXPECT(state, sxml_error_record_report(NULL, &error) ==
                                SXML_STATUS_TYPE_ERROR);
}

static const sxml_test_case xdm_test_cases[] = {
    {.name = "heap_rejects_invalid_configuration",
     .function = test_heap_rejects_invalid_configuration},
    {.name = "heap_initializes_in_caller_memory",
     .function = test_heap_initializes_in_caller_memory},
    {.name = "tree_identifiers_increase",
     .function = test_tree_identifiers_increase},
    {.name = "inline_items_need_no_memory",
     .function = test_inline_items_need_no_memory},
    {.name = "cell_items_are_counted_until_released",
     .function = test_cell_items_are_counted_until_released},
    {.name = "deep_values_release_without_recursion",
     .function = test_deep_values_release_without_recursion},
    {.name = "immortal_cells_ignore_counting",
     .function = test_immortal_cells_ignore_counting},
    {.name = "cell_requests_are_checked",
     .function = test_cell_requests_are_checked},
    {.name = "heap_stops_at_maximum_byte_count",
     .function = test_heap_stops_at_maximum_byte_count},
    {.name = "heap_allocator_counts_hidden_headers",
     .function = test_heap_allocator_counts_hidden_headers},
    {.name = "empty_and_single_item_sequences",
     .function = test_empty_and_single_item_sequences},
    {.name = "vectors_hold_their_items",
     .function = test_vectors_hold_their_items},
    {.name = "short_vectors_need_no_cell",
     .function = test_short_vectors_need_no_cell},
    {.name = "iterator_walks_single_item_once",
     .function = test_iterator_walks_single_item_once},
    {.name = "builder_lifecycle", .function = test_builder_lifecycle},
    {.name = "builder_collects_and_is_reused",
     .function = test_builder_collects_and_is_reused},
    {.name = "builder_survives_every_allocation_failure",
     .function = test_builder_survives_every_allocation_failure},
    {.name = "raised_error_is_copied_and_kept",
     .function = test_raised_error_is_copied_and_kept},
    {.name = "raise_error_checks_status_and_null_record",
     .function = test_raise_error_checks_status_and_null_record},
    {.name = "raise_error_failure_keeps_previous_error",
     .function = test_raise_error_failure_keeps_previous_error},
    {.name = "static_error_report_fills_record",
     .function = test_static_error_report_fills_record},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xdm",
            .cases = xdm_test_cases,
            .case_count = SXML_TEST_COUNT(xdm_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
