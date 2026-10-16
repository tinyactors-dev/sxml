/*
 * Tests for include/sxml/memory.h: views, checked size arithmetic, memory
 * requirements, the allocator wrappers and the arena. The default and
 * counting allocators are tested in tests/allocator_test.c.
 */
#include "sxml/memory.h"
#include "test_runner.h"

#include <stdalign.h>

/* Caller memory for objects placed with initialize; 16 suits every type. */
#define TEST_STORAGE_ALIGNMENT 16

/* ---------------------------------------------------------------- Views */

static void test_byte_views_with_same_bytes_are_equal(sxml_test_state *state) {
    static const uint8_t other_bytes[] = {'a', 'b', 'c'};
    sxml_byte_view literal = SXML_BYTE_VIEW_FROM_LITERAL("abc");
    sxml_byte_view other = {.bytes = other_bytes, .byte_count = 3};
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(literal, other));
    SXML_TEST_EXPECT(state, !sxml_byte_view_equals(
                                literal, SXML_BYTE_VIEW_FROM_LITERAL("abd")));
    SXML_TEST_EXPECT(state, !sxml_byte_view_equals(
                                literal, SXML_BYTE_VIEW_FROM_LITERAL("ab")));
}

static void test_empty_byte_views_are_equal_whatever_their_pointers(
    sxml_test_state *state) {
    sxml_byte_view null_view = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                null_view, SXML_BYTE_VIEW_FROM_LITERAL("")));
}

static void test_byte_view_from_c_string_excludes_terminator(
    sxml_test_state *state) {
    sxml_byte_view view = sxml_byte_view_from_c_string("sxml");
    SXML_TEST_EXPECT(state, view.byte_count == 4);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                view, SXML_BYTE_VIEW_FROM_LITERAL("sxml")));
}

static void test_memory_view_slice_selects_range(sxml_test_state *state) {
    uint8_t bytes[8] = {0};
    sxml_memory_view memory = {.bytes = bytes, .byte_count = sizeof(bytes)};
    sxml_memory_view slice = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_memory_view_slice(
        memory, (sxml_byte_range){.byte_offset = 2, .byte_count = 6}, &slice);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, slice.bytes == bytes + 2);
    SXML_TEST_EXPECT(state, slice.byte_count == 6);
}

static void test_memory_view_slice_rejects_range_outside(
    sxml_test_state *state) {
    uint8_t bytes[8] = {0};
    sxml_memory_view memory = {.bytes = bytes, .byte_count = sizeof(bytes)};
    sxml_memory_view slice = {.bytes = NULL, .byte_count = 99};
    sxml_status status = sxml_memory_view_slice(
        memory, (sxml_byte_range){.byte_offset = 2, .byte_count = 7}, &slice);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INDEX_OUT_OF_RANGE);
    /* An offset and count whose sum wraps around must not pass. */
    status = sxml_memory_view_slice(
        memory, (sxml_byte_range){.byte_offset = 4, .byte_count = SIZE_MAX},
        &slice);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INDEX_OUT_OF_RANGE);
    status = sxml_memory_view_slice(
        memory, (sxml_byte_range){.byte_offset = 9, .byte_count = 0}, &slice);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, slice.byte_count == 99);
}

static void test_memory_view_slice_may_be_empty(sxml_test_state *state) {
    uint8_t bytes[4] = {0};
    sxml_memory_view memory = {.bytes = bytes, .byte_count = sizeof(bytes)};
    sxml_memory_view slice = {.bytes = bytes, .byte_count = 1};
    sxml_status status = sxml_memory_view_slice(
        memory, (sxml_byte_range){.byte_offset = 4, .byte_count = 0}, &slice);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, slice.byte_count == 0);
}

/* ---------------------------------------------- Checked size arithmetic */

static void test_size_add_computes_sum(sxml_test_state *state) {
    size_t sum = 0;
    SXML_TEST_EXPECT(state, sxml_size_add(40, 2, &sum) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sum == 42);
    SXML_TEST_EXPECT(state,
                     sxml_size_add(SIZE_MAX - 1, 1, &sum) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sum == SIZE_MAX);
}

static void test_size_add_reports_overflow_and_keeps_output(
    sxml_test_state *state) {
    size_t sum = 7;
    SXML_TEST_EXPECT(state, sxml_size_add(SIZE_MAX, 1, &sum) ==
                                SXML_STATUS_ARITHMETIC_OVERFLOW);
    SXML_TEST_EXPECT(state, sxml_size_add(SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1,
                                          &sum) ==
                                SXML_STATUS_ARITHMETIC_OVERFLOW);
    SXML_TEST_EXPECT(state, sum == 7);
}

static void test_size_multiply_computes_product(sxml_test_state *state) {
    size_t product = 1;
    SXML_TEST_EXPECT(state,
                     sxml_size_multiply(6, 7, &product) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, product == 42);
    SXML_TEST_EXPECT(state, sxml_size_multiply(SIZE_MAX, 0, &product) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, product == 0);
    SXML_TEST_EXPECT(state, sxml_size_multiply(0, SIZE_MAX, &product) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, product == 0);
}

static void test_size_multiply_reports_overflow_and_keeps_output(
    sxml_test_state *state) {
    size_t product = 7;
    SXML_TEST_EXPECT(state, sxml_size_multiply(SIZE_MAX / 2 + 1, 2,
                                               &product) ==
                                SXML_STATUS_ARITHMETIC_OVERFLOW);
    SXML_TEST_EXPECT(state, product == 7);
}

static void test_alignment_is_valid_for_powers_of_two(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, sxml_alignment_is_valid(1));
    SXML_TEST_EXPECT(state, sxml_alignment_is_valid(2));
    SXML_TEST_EXPECT(state, sxml_alignment_is_valid(4096));
    SXML_TEST_EXPECT(state, sxml_alignment_is_valid(SIZE_MAX / 2 + 1));
    SXML_TEST_EXPECT(state, !sxml_alignment_is_valid(0));
    SXML_TEST_EXPECT(state, !sxml_alignment_is_valid(3));
    SXML_TEST_EXPECT(state, !sxml_alignment_is_valid(SIZE_MAX));
}

static void test_size_align_up_rounds_to_multiple(sxml_test_state *state) {
    size_t aligned = 99;
    SXML_TEST_EXPECT(state, sxml_size_align_up(0, 8, &aligned) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, aligned == 0);
    SXML_TEST_EXPECT(state, sxml_size_align_up(1, 8, &aligned) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, aligned == 8);
    SXML_TEST_EXPECT(state, sxml_size_align_up(16, 8, &aligned) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, aligned == 16);
    SXML_TEST_EXPECT(state, sxml_size_align_up(17, 1, &aligned) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, aligned == 17);
}

static void test_size_align_up_reports_failures_and_keeps_output(
    sxml_test_state *state) {
    size_t aligned = 99;
    SXML_TEST_EXPECT(state, sxml_size_align_up(SIZE_MAX, 2, &aligned) ==
                                SXML_STATUS_ARITHMETIC_OVERFLOW);
    SXML_TEST_EXPECT(state, sxml_size_align_up(5, 3, &aligned) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_size_align_up(5, 0, &aligned) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, aligned == 99);
}

/* --------------------------------------------------- Memory requirements */

static void test_requirements_append_lays_out_members(sxml_test_state *state) {
    sxml_memory_requirements composite = {.byte_count = 0, .alignment = 1};
    size_t flag_offset = 99;
    size_t counter_offset = 99;
    size_t table_offset = 99;
    sxml_status status = sxml_memory_requirements_append(
        &composite, (sxml_memory_requirements){.byte_count = 1, .alignment = 1},
        &flag_offset);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    status = sxml_memory_requirements_append(
        &composite, (sxml_memory_requirements){.byte_count = 8, .alignment = 8},
        &counter_offset);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    status = sxml_memory_requirements_append(
        &composite,
        (sxml_memory_requirements){.byte_count = 12, .alignment = 4},
        &table_offset);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, flag_offset == 0);
    SXML_TEST_EXPECT(state, counter_offset == 8);
    SXML_TEST_EXPECT(state, table_offset == 16);
    SXML_TEST_EXPECT(state, composite.byte_count == 28);
    SXML_TEST_EXPECT(state, composite.alignment == 8);
}

static void test_requirements_append_rejects_invalid_alignment(
    sxml_test_state *state) {
    sxml_memory_requirements composite = {.byte_count = 4, .alignment = 4};
    size_t member_offset = 99;
    sxml_status status = sxml_memory_requirements_append(
        &composite, (sxml_memory_requirements){.byte_count = 4, .alignment = 6},
        &member_offset);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_ARGUMENT);
    sxml_memory_requirements broken = {.byte_count = 4, .alignment = 0};
    status = sxml_memory_requirements_append(
        &broken, (sxml_memory_requirements){.byte_count = 4, .alignment = 4},
        &member_offset);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, member_offset == 99);
    SXML_TEST_EXPECT(state, composite.byte_count == 4);
}

static void test_requirements_append_reports_overflow_and_keeps_composite(
    sxml_test_state *state) {
    sxml_memory_requirements composite = {.byte_count = SIZE_MAX - 4,
                                          .alignment = 1};
    size_t member_offset = 99;
    /* Fails in the rounding up of the offset. */
    sxml_status status = sxml_memory_requirements_append(
        &composite,
        (sxml_memory_requirements){.byte_count = 1, .alignment = 16},
        &member_offset);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_ARITHMETIC_OVERFLOW);
    /* Fails in the addition of the member's size. */
    status = sxml_memory_requirements_append(
        &composite, (sxml_memory_requirements){.byte_count = 8, .alignment = 1},
        &member_offset);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_ARITHMETIC_OVERFLOW);
    SXML_TEST_EXPECT(state, composite.byte_count == SIZE_MAX - 4);
    SXML_TEST_EXPECT(state, composite.alignment == 1);
    SXML_TEST_EXPECT(state, member_offset == 99);
}

static void test_check_requirements_accepts_matching_memory(
    sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[32] = {0};
    sxml_memory_view memory = {.bytes = storage, .byte_count = 32};
    SXML_TEST_EXPECT(state, sxml_memory_view_check_requirements(
                                memory, (sxml_memory_requirements){
                                            .byte_count = 32,
                                            .alignment = 16,
                                        }) == SXML_STATUS_OK);
}

static void test_check_requirements_reports_each_mismatch(
    sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[32] = {0};
    sxml_memory_requirements requirements = {.byte_count = 16,
                                             .alignment = 8};
    sxml_memory_view small = {.bytes = storage, .byte_count = 15};
    sxml_memory_view misaligned = {.bytes = storage + 1, .byte_count = 31};
    sxml_memory_view null_with_bytes = {.bytes = NULL, .byte_count = 16};
    sxml_memory_view empty = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(state,
                     sxml_memory_view_check_requirements(small, requirements) ==
                         SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, sxml_memory_view_check_requirements(
                                misaligned, requirements) ==
                                SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, sxml_memory_view_check_requirements(
                                null_with_bytes, requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_memory_view_check_requirements(empty, requirements) ==
                         SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, sxml_memory_view_check_requirements(
                                small, (sxml_memory_requirements){
                                           .byte_count = 1,
                                           .alignment = 5,
                                       }) == SXML_STATUS_INVALID_ARGUMENT);
}

/* ----------------------------------------------------- Allocator wrappers */

static void test_allocate_rejects_invalid_requests(sxml_test_state *state) {
    static uint8_t untouched_marker = 0;
    void *memory = &untouched_marker;
    sxml_allocator null_allocator = {.state = NULL, .operations = NULL};
    SXML_TEST_EXPECT(state, sxml_allocator_is_null(null_allocator));
    SXML_TEST_EXPECT(state, !sxml_allocator_is_null(sxml_default_allocator()));
    SXML_TEST_EXPECT(state,
                     sxml_allocator_allocate(
                         null_allocator,
                         (sxml_allocation_request){.byte_count = 8,
                                                   .alignment = 8},
                         &memory) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_allocator_allocate(
                         sxml_default_allocator(),
                         (sxml_allocation_request){.byte_count = 0,
                                                   .alignment = 8},
                         &memory) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_allocator_allocate(
                         sxml_default_allocator(),
                         (sxml_allocation_request){.byte_count = 8,
                                                   .alignment = 12},
                         &memory) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, memory == &untouched_marker);
}

static void test_release_of_null_does_nothing(sxml_test_state *state) {
    sxml_allocator_release(sxml_default_allocator(), NULL);
    sxml_allocator_release((sxml_allocator){.state = NULL, .operations = NULL},
                           NULL);
    SXML_TEST_EXPECT(state, true);
}

/*
 * An allocator that breaks its contract: it hands out memory one byte past
 * an aligned address, and remembers whether it was asked to take it back.
 */
typedef struct misaligning_allocator_state {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[32];
    bool was_released;
} misaligning_allocator_state;

static sxml_status misaligning_allocate(void *state,
                                        sxml_allocation_request request,
                                        void **memory) {
    (void)request;
    misaligning_allocator_state *allocator_state = state;
    *memory = allocator_state->storage + 1;
    return SXML_STATUS_OK;
}

static void misaligning_release(void *state, void *memory) {
    (void)memory;
    misaligning_allocator_state *allocator_state = state;
    allocator_state->was_released = true;
}

static const sxml_allocator_operations misaligning_operations = {
    .allocate = misaligning_allocate,
    .release = misaligning_release,
};

static void test_allocate_catches_misaligned_memory(sxml_test_state *state) {
    misaligning_allocator_state allocator_state = {.storage = {0},
                                                   .was_released = false};
    sxml_allocator allocator = {.state = &allocator_state,
                                .operations = &misaligning_operations};
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        allocator, (sxml_allocation_request){.byte_count = 8, .alignment = 8},
        &memory);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, memory == NULL);
    SXML_TEST_EXPECT(state, allocator_state.was_released);
}

/* ------------------------------------------------------------------- Arena */

/* A counting allocator over the default allocator, placed on the stack. */
typedef struct counting_fixture {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[128];
    sxml_counting_allocator *counting_allocator;
} counting_fixture;

static sxml_status counting_fixture_initialize(
    counting_fixture *fixture, uint64_t failing_allocation_number) {
    return sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = fixture->storage,
                           .byte_count = sizeof(fixture->storage)},
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = failing_allocation_number,
        },
        &fixture->counting_allocator);
}

static sxml_allocation_counts counting_fixture_counts(
    const counting_fixture *fixture) {
    return sxml_counting_allocator_read_counts(fixture->counting_allocator);
}

static sxml_allocation_request request_of(size_t byte_count,
                                          size_t alignment) {
    return (sxml_allocation_request){.byte_count = byte_count,
                                     .alignment = alignment};
}

static bool is_aligned(const void *memory, size_t alignment) {
    return ((uintptr_t)memory % alignment) == 0;
}

/* Writes value to every byte of memory, so AddressSanitizer checks bounds. */
static void fill_memory(sxml_memory_view memory, uint8_t value) {
    for (size_t byte_index = 0; byte_index < memory.byte_count;
         byte_index += 1) {
        memory.bytes[byte_index] = value;
    }
}

static sxml_memory_view memory_of(void *memory, size_t byte_count) {
    return (sxml_memory_view){.bytes = memory, .byte_count = byte_count};
}

static void test_arena_works_within_initial_memory(sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t arena_storage[128] = {0};
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t initial_bytes[64] = {0};
    sxml_arena *arena = NULL;
    sxml_status status = sxml_arena_initialize(
        (sxml_memory_view){.bytes = arena_storage,
                           .byte_count = sizeof(arena_storage)},
        &(sxml_arena_configuration){
            .block_allocator = {.state = NULL, .operations = NULL},
            .block_byte_count = 0,
            .maximum_block_byte_count = 0,
            .initial_memory = {.bytes = initial_bytes,
                               .byte_count = sizeof(initial_bytes)},
        },
        &arena);
    SXML_TEST_REQUIRE(state, status == SXML_STATUS_OK);

    void *first = NULL;
    void *second = NULL;
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(1, 1),
                                                &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(8, 8),
                                                &second) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, first == initial_bytes);
    SXML_TEST_EXPECT(state, second == initial_bytes + 8);

    /* 48 of 64 bytes remain; 49 do not fit and the arena cannot grow. */
    void *too_large = NULL;
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(49, 1),
                                                &too_large) ==
                                SXML_STATUS_CAPACITY_EXCEEDED);
    SXML_TEST_EXPECT(state, too_large == NULL);
    void *rest = NULL;
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(48, 1),
                                                &rest) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, rest == initial_bytes + 16);

    sxml_arena_reset(arena);
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(64, 1),
                                                &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, first == initial_bytes);
    sxml_arena_finalize(arena);
}

static void test_arena_rejects_invalid_configuration(sxml_test_state *state) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state,
                     sxml_arena_memory_requirements(
                         &(sxml_arena_configuration){
                             .block_allocator = sxml_default_allocator(),
                             .block_byte_count = 0,
                             .maximum_block_byte_count = 0,
                             .initial_memory = {.bytes = NULL,
                                                .byte_count = 0},
                         },
                         &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_arena_memory_requirements(
                         &(sxml_arena_configuration){
                             .block_allocator = {.state = NULL,
                                                 .operations = NULL},
                             .block_byte_count = 0,
                             .maximum_block_byte_count = 0,
                             .initial_memory = {.bytes = NULL,
                                                .byte_count = 8},
                         },
                         &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, requirements.byte_count == 0);
}

static void test_arena_initialize_checks_memory(sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t arena_storage[128] = {0};
    sxml_arena_configuration configuration = {
        .block_allocator = sxml_default_allocator(),
        .block_byte_count = 256,
        .maximum_block_byte_count = 0,
        .initial_memory = {.bytes = NULL, .byte_count = 0},
    };
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_REQUIRE(state, sxml_arena_memory_requirements(
                                 &configuration, &requirements) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(arena_storage));
    sxml_arena *arena = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_arena_initialize(
                         (sxml_memory_view){
                             .bytes = arena_storage,
                             .byte_count = requirements.byte_count - 1},
                         &configuration,
                         &arena) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state,
                     sxml_arena_initialize(
                         (sxml_memory_view){
                             .bytes = arena_storage + 1,
                             .byte_count = sizeof(arena_storage) - 1},
                         &configuration,
                         &arena) == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, arena == NULL);
}

/* An arena placed in arena_storage that takes blocks from a counting fixture. */
static sxml_status initialize_growing_arena(
    sxml_memory_view arena_storage, const counting_fixture *fixture,
    sxml_arena **arena) {
    return sxml_arena_initialize(
        arena_storage,
        &(sxml_arena_configuration){
            .block_allocator = sxml_counting_allocator_as_allocator(
                fixture->counting_allocator),
            .block_byte_count = 256,
            .maximum_block_byte_count = 0,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
        },
        arena);
}

static void test_arena_grows_by_blocks(sxml_test_state *state) {
    counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
    SXML_TEST_REQUIRE(state, counting_fixture_initialize(&fixture, 0) ==
                                 SXML_STATUS_OK);
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t arena_storage[128] = {0};
    sxml_arena *arena = NULL;
    SXML_TEST_REQUIRE(
        state, initialize_growing_arena(
                   (sxml_memory_view){.bytes = arena_storage,
                                      .byte_count = sizeof(arena_storage)},
                   &fixture, &arena) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     counting_fixture_counts(&fixture)
                             .attempted_allocation_count == 0);

    /* 64-byte pieces: a 256-byte block holds three after its header. */
    for (size_t piece_index = 0; piece_index < 10; piece_index += 1) {
        void *piece = NULL;
        SXML_TEST_REQUIRE(state, sxml_arena_allocate(arena, request_of(64, 16),
                                                     &piece) ==
                                     SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, is_aligned(piece, 16));
        fill_memory(memory_of(piece, 64), (uint8_t)piece_index);
    }
    sxml_allocation_counts counts = counting_fixture_counts(&fixture);
    SXML_TEST_EXPECT(state, counts.live_allocation_count == 4);
    SXML_TEST_EXPECT(state, counts.allocated_byte_count == UINT64_C(4) * 256);

    sxml_arena_finalize(arena);
    SXML_TEST_EXPECT(state,
                     counting_fixture_counts(&fixture).live_allocation_count ==
                         0);
    sxml_counting_allocator_finalize(fixture.counting_allocator);
}

static void test_arena_gives_large_request_own_block(sxml_test_state *state) {
    counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
    SXML_TEST_REQUIRE(state, counting_fixture_initialize(&fixture, 0) ==
                                 SXML_STATUS_OK);
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t arena_storage[128] = {0};
    sxml_arena *arena = NULL;
    SXML_TEST_REQUIRE(
        state, initialize_growing_arena(
                   (sxml_memory_view){.bytes = arena_storage,
                                      .byte_count = sizeof(arena_storage)},
                   &fixture, &arena) == SXML_STATUS_OK);

    void *small = NULL;
    void *large = NULL;
    void *second_small = NULL;
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(16, 8),
                                                &small) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(1000, 64),
                                                &large) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, is_aligned(large, 64));
    fill_memory(memory_of(large, 1000), 0xAB);
    /* The first block still has room, so no third block is needed. */
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(16, 8),
                                                &second_small) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, (uint8_t *)second_small == (uint8_t *)small + 16);
    SXML_TEST_EXPECT(state,
                     counting_fixture_counts(&fixture).live_allocation_count ==
                         2);

    sxml_arena_finalize(arena);
    SXML_TEST_EXPECT(state,
                     counting_fixture_counts(&fixture).live_allocation_count ==
                         0);
    sxml_counting_allocator_finalize(fixture.counting_allocator);
}

static void test_arena_stops_at_maximum_block_byte_count(
    sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t arena_storage[128] = {0};
    sxml_arena *arena = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_arena_initialize(
                          (sxml_memory_view){.bytes = arena_storage,
                                             .byte_count =
                                                 sizeof(arena_storage)},
                          &(sxml_arena_configuration){
                              .block_allocator = sxml_default_allocator(),
                              .block_byte_count = 128,
                              .maximum_block_byte_count = 256,
                              .initial_memory = {.bytes = NULL,
                                                 .byte_count = 0},
                          },
                          &arena) == SXML_STATUS_OK);
    void *piece = NULL;
    /* Each 100-byte piece fills most of a 128-byte block; two blocks fit. */
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(100, 1),
                                                &piece) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(100, 1),
                                                &piece) == SXML_STATUS_OK);
    void *refused = NULL;
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(100, 1),
                                                &refused) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, refused == NULL);
    /* Reset gives the budget back. */
    sxml_arena_reset(arena);
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(100, 1),
                                                &piece) == SXML_STATUS_OK);
    sxml_arena_finalize(arena);
}

static void test_arena_rejects_invalid_and_huge_requests(
    sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t arena_storage[128] = {0};
    sxml_arena *arena = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_arena_initialize(
                          (sxml_memory_view){.bytes = arena_storage,
                                             .byte_count =
                                                 sizeof(arena_storage)},
                          &(sxml_arena_configuration){
                              .block_allocator = sxml_default_allocator(),
                              .block_byte_count = 128,
                              .maximum_block_byte_count = 0,
                              .initial_memory = {.bytes = NULL,
                                                 .byte_count = 0},
                          },
                          &arena) == SXML_STATUS_OK);
    void *memory = NULL;
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(0, 1),
                                                &memory) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena, request_of(8, 3),
                                                &memory) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_arena_allocate(arena,
                                                request_of(SIZE_MAX - 2, 8),
                                                &memory) ==
                                SXML_STATUS_ARITHMETIC_OVERFLOW);
    SXML_TEST_EXPECT(state, memory == NULL);
    sxml_arena_finalize(arena);
}

static void test_arena_as_allocator_allocates_from_arena(
    sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t arena_storage[128] = {0};
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t initial_bytes[32] = {0};
    sxml_arena *arena = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_arena_initialize(
                          (sxml_memory_view){.bytes = arena_storage,
                                             .byte_count =
                                                 sizeof(arena_storage)},
                          &(sxml_arena_configuration){
                              .block_allocator = {.state = NULL,
                                                  .operations = NULL},
                              .block_byte_count = 0,
                              .maximum_block_byte_count = 0,
                              .initial_memory = {.bytes = initial_bytes,
                                                 .byte_count =
                                                     sizeof(initial_bytes)},
                          },
                          &arena) == SXML_STATUS_OK);
    sxml_allocator allocator = sxml_arena_as_allocator(arena);
    void *memory = NULL;
    SXML_TEST_EXPECT(state, sxml_allocator_allocate(allocator,
                                                    request_of(32, 4),
                                                    &memory) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, memory == initial_bytes);
    /* Release does not return memory to the arena. */
    sxml_allocator_release(allocator, memory);
    SXML_TEST_EXPECT(state, sxml_allocator_allocate(allocator,
                                                    request_of(1, 1),
                                                    &memory) ==
                                SXML_STATUS_CAPACITY_EXCEEDED);
    sxml_arena_finalize(arena);
}

static void test_arena_finalize_and_destroy_accept_null(
    sxml_test_state *state) {
    sxml_arena_finalize(NULL);
    sxml_arena_destroy(NULL);
    SXML_TEST_EXPECT(state, true);
}

/*
 * Creates an arena through allocator, takes two blocks' worth of memory from
 * it and destroys it. Used to drive every allocation failure in turn.
 */
static sxml_status create_use_and_destroy_arena(sxml_allocator allocator) {
    sxml_arena *arena = NULL;
    sxml_status status = sxml_arena_create(allocator,
                                           &(sxml_arena_configuration){
                                               .block_allocator = allocator,
                                               .block_byte_count = 128,
                                               .maximum_block_byte_count = 0,
                                               .initial_memory =
                                                   {.bytes = NULL,
                                                    .byte_count = 0},
                                           },
                                           &arena);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t piece_index = 0;
         piece_index < 4 && status == SXML_STATUS_OK; piece_index += 1) {
        void *piece = NULL;
        status = sxml_arena_allocate(arena, request_of(48, 8), &piece);
    }
    sxml_arena_destroy(arena);
    return status;
}

/*
 * The pattern every module's tests follow for allocation failures: fail the
 * first allocation, then the second, and so on until the operation succeeds,
 * and check that each failure leaked nothing.
 */
static void test_arena_survives_every_allocation_failure(
    sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        counting_fixture fixture = {.storage = {0},
                                    .counting_allocator = NULL};
        SXML_TEST_REQUIRE(state, counting_fixture_initialize(
                                     &fixture, failing_allocation_number) ==
                                     SXML_STATUS_OK);
        sxml_status status = create_use_and_destroy_arena(
            sxml_counting_allocator_as_allocator(fixture.counting_allocator));
        sxml_allocation_counts counts = counting_fixture_counts(&fixture);
        sxml_counting_allocator_finalize(fixture.counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, counts.failed_allocation_count == 1);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 100);
    }
    /* The arena itself and two blocks: three allocations could fail. */
    SXML_TEST_EXPECT(state, failing_allocation_number == 4);
}

static const sxml_test_case memory_test_cases[] = {
    {.name = "byte_views_with_same_bytes_are_equal",
     .function = test_byte_views_with_same_bytes_are_equal},
    {.name = "empty_byte_views_are_equal_whatever_their_pointers",
     .function = test_empty_byte_views_are_equal_whatever_their_pointers},
    {.name = "byte_view_from_c_string_excludes_terminator",
     .function = test_byte_view_from_c_string_excludes_terminator},
    {.name = "memory_view_slice_selects_range",
     .function = test_memory_view_slice_selects_range},
    {.name = "memory_view_slice_rejects_range_outside",
     .function = test_memory_view_slice_rejects_range_outside},
    {.name = "memory_view_slice_may_be_empty",
     .function = test_memory_view_slice_may_be_empty},
    {.name = "size_add_computes_sum", .function = test_size_add_computes_sum},
    {.name = "size_add_reports_overflow_and_keeps_output",
     .function = test_size_add_reports_overflow_and_keeps_output},
    {.name = "size_multiply_computes_product",
     .function = test_size_multiply_computes_product},
    {.name = "size_multiply_reports_overflow_and_keeps_output",
     .function = test_size_multiply_reports_overflow_and_keeps_output},
    {.name = "alignment_is_valid_for_powers_of_two",
     .function = test_alignment_is_valid_for_powers_of_two},
    {.name = "size_align_up_rounds_to_multiple",
     .function = test_size_align_up_rounds_to_multiple},
    {.name = "size_align_up_reports_failures_and_keeps_output",
     .function = test_size_align_up_reports_failures_and_keeps_output},
    {.name = "requirements_append_lays_out_members",
     .function = test_requirements_append_lays_out_members},
    {.name = "requirements_append_rejects_invalid_alignment",
     .function = test_requirements_append_rejects_invalid_alignment},
    {.name = "requirements_append_reports_overflow_and_keeps_composite",
     .function = test_requirements_append_reports_overflow_and_keeps_composite},
    {.name = "check_requirements_accepts_matching_memory",
     .function = test_check_requirements_accepts_matching_memory},
    {.name = "check_requirements_reports_each_mismatch",
     .function = test_check_requirements_reports_each_mismatch},
    {.name = "allocate_rejects_invalid_requests",
     .function = test_allocate_rejects_invalid_requests},
    {.name = "release_of_null_does_nothing",
     .function = test_release_of_null_does_nothing},
    {.name = "allocate_catches_misaligned_memory",
     .function = test_allocate_catches_misaligned_memory},
    {.name = "arena_works_within_initial_memory",
     .function = test_arena_works_within_initial_memory},
    {.name = "arena_rejects_invalid_configuration",
     .function = test_arena_rejects_invalid_configuration},
    {.name = "arena_initialize_checks_memory",
     .function = test_arena_initialize_checks_memory},
    {.name = "arena_grows_by_blocks", .function = test_arena_grows_by_blocks},
    {.name = "arena_gives_large_request_own_block",
     .function = test_arena_gives_large_request_own_block},
    {.name = "arena_stops_at_maximum_block_byte_count",
     .function = test_arena_stops_at_maximum_block_byte_count},
    {.name = "arena_rejects_invalid_and_huge_requests",
     .function = test_arena_rejects_invalid_and_huge_requests},
    {.name = "arena_as_allocator_allocates_from_arena",
     .function = test_arena_as_allocator_allocates_from_arena},
    {.name = "arena_finalize_and_destroy_accept_null",
     .function = test_arena_finalize_and_destroy_accept_null},
    {.name = "arena_survives_every_allocation_failure",
     .function = test_arena_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "memory",
            .cases = memory_test_cases,
            .case_count = SXML_TEST_COUNT(memory_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
