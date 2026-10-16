/*
 * Tests for the default allocator and the counting allocator
 * (include/sxml/memory.h, src/allocator.c).
 */
#include "sxml/memory.h"
#include "test_runner.h"

#include <stdalign.h>

/* Caller memory for objects placed with initialize; 16 suits every type. */
#define TEST_STORAGE_ALIGNMENT 16

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

/* ------------------------------------------------------- Default allocator */

static void test_default_allocator_allocates_writable_memory(
    sxml_test_state *state) {
    sxml_allocator allocator = sxml_default_allocator();
    void *memory = NULL;
    SXML_TEST_REQUIRE(state, sxml_allocator_allocate(allocator,
                                                     request_of(100, 8),
                                                     &memory) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, memory != NULL);
    SXML_TEST_EXPECT(state, is_aligned(memory, 8));
    fill_memory(memory_of(memory, 100), 0x5A);
    sxml_allocator_release(allocator, memory);
}

static void test_default_allocator_honours_every_alignment(
    sxml_test_state *state) {
    sxml_allocator allocator = sxml_default_allocator();
    /* Up to a page, which is above what malloc guarantees on any platform,
       with sizes that are not multiples of the alignment. */
    for (size_t alignment = 1; alignment <= 4096; alignment *= 2) {
        void *memory = NULL;
        SXML_TEST_REQUIRE(state, sxml_allocator_allocate(
                                     allocator, request_of(alignment + 3,
                                                           alignment),
                                     &memory) == SXML_STATUS_OK);
        SXML_TEST_REQUIRE(state, memory != NULL);
        SXML_TEST_EXPECT(state, is_aligned(memory, alignment));
        fill_memory(memory_of(memory, alignment + 3), 0x5A);
        sxml_allocator_release(allocator, memory);
    }
}

static void test_default_allocator_has_no_state(sxml_test_state *state) {
    sxml_allocator first = sxml_default_allocator();
    sxml_allocator second = sxml_default_allocator();
    SXML_TEST_EXPECT(state, first.state == NULL);
    SXML_TEST_EXPECT(state, first.operations == second.operations);
}

/* ------------------------------------------------------ Counting allocator */

/* A counting allocator placed on the stack. */
typedef struct counting_fixture {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[128];
    sxml_counting_allocator *counting_allocator;
} counting_fixture;

static sxml_status counting_fixture_initialize(
    counting_fixture *fixture,
    const sxml_counting_allocator_configuration *configuration) {
    return sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = fixture->storage,
                           .byte_count = sizeof(fixture->storage)},
        configuration, &fixture->counting_allocator);
}

static void test_counting_allocator_counts_allocations_and_releases(
    sxml_test_state *state) {
    counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
    SXML_TEST_REQUIRE(state,
                      counting_fixture_initialize(
                          &fixture,
                          &(sxml_counting_allocator_configuration){
                              .backing_allocator = sxml_default_allocator(),
                              .failing_allocation_number = 0,
                          }) == SXML_STATUS_OK);
    sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(fixture.counting_allocator);
    void *first = NULL;
    void *second = NULL;
    SXML_TEST_EXPECT(state, sxml_allocator_allocate(allocator,
                                                    request_of(10, 1),
                                                    &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_allocator_allocate(allocator,
                                                    request_of(32, 16),
                                                    &second) ==
                                SXML_STATUS_OK);
    sxml_allocator_release(allocator, first);
    sxml_allocator_release(allocator, NULL);

    sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(fixture.counting_allocator);
    SXML_TEST_EXPECT(state, counts.attempted_allocation_count == 2);
    SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
    SXML_TEST_EXPECT(state, counts.release_count == 1);
    SXML_TEST_EXPECT(state, counts.live_allocation_count == 1);
    SXML_TEST_EXPECT(state, counts.allocated_byte_count == 42);

    sxml_allocator_release(allocator, second);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(
                                fixture.counting_allocator)
                                    .live_allocation_count == 0);
    sxml_counting_allocator_finalize(fixture.counting_allocator);
}

static void test_counting_allocator_fails_only_the_chosen_allocation(
    sxml_test_state *state) {
    counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
    SXML_TEST_REQUIRE(state,
                      counting_fixture_initialize(
                          &fixture,
                          &(sxml_counting_allocator_configuration){
                              .backing_allocator = sxml_default_allocator(),
                              .failing_allocation_number = 2,
                          }) == SXML_STATUS_OK);
    sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(fixture.counting_allocator);
    void *pieces[3] = {NULL, NULL, NULL};
    sxml_status statuses[3] = {SXML_STATUS_OK, SXML_STATUS_OK,
                               SXML_STATUS_OK};
    for (size_t piece_index = 0; piece_index < 3; piece_index += 1) {
        statuses[piece_index] = sxml_allocator_allocate(
            allocator, request_of(8, 8), &pieces[piece_index]);
    }
    SXML_TEST_EXPECT(state, statuses[0] == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, statuses[1] == SXML_STATUS_OUT_OF_MEMORY);
    SXML_TEST_EXPECT(state, pieces[1] == NULL);
    SXML_TEST_EXPECT(state, statuses[2] == SXML_STATUS_OK);

    sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(fixture.counting_allocator);
    SXML_TEST_EXPECT(state, counts.attempted_allocation_count == 3);
    SXML_TEST_EXPECT(state, counts.failed_allocation_count == 1);
    SXML_TEST_EXPECT(state, counts.live_allocation_count == 2);
    SXML_TEST_EXPECT(state, counts.allocated_byte_count == 16);

    for (size_t piece_index = 0; piece_index < 3; piece_index += 1) {
        sxml_allocator_release(allocator, pieces[piece_index]);
    }
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(
                                fixture.counting_allocator)
                                    .live_allocation_count == 0);
    sxml_counting_allocator_finalize(fixture.counting_allocator);
}

static void test_counting_allocator_counts_backing_failures(
    sxml_test_state *state) {
    counting_fixture backing = {.storage = {0}, .counting_allocator = NULL};
    SXML_TEST_REQUIRE(state,
                      counting_fixture_initialize(
                          &backing,
                          &(sxml_counting_allocator_configuration){
                              .backing_allocator = sxml_default_allocator(),
                              .failing_allocation_number = 1,
                          }) == SXML_STATUS_OK);
    counting_fixture outer = {.storage = {0}, .counting_allocator = NULL};
    SXML_TEST_REQUIRE(state,
                      counting_fixture_initialize(
                          &outer,
                          &(sxml_counting_allocator_configuration){
                              .backing_allocator =
                                  sxml_counting_allocator_as_allocator(
                                      backing.counting_allocator),
                              .failing_allocation_number = 0,
                          }) == SXML_STATUS_OK);
    void *memory = NULL;
    SXML_TEST_EXPECT(state, sxml_allocator_allocate(
                                sxml_counting_allocator_as_allocator(
                                    outer.counting_allocator),
                                request_of(8, 8),
                                &memory) == SXML_STATUS_OUT_OF_MEMORY);
    sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(outer.counting_allocator);
    SXML_TEST_EXPECT(state, counts.attempted_allocation_count == 1);
    SXML_TEST_EXPECT(state, counts.failed_allocation_count == 1);
    SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
    sxml_counting_allocator_finalize(outer.counting_allocator);
    sxml_counting_allocator_finalize(backing.counting_allocator);
}

static void test_counting_allocator_needs_a_backing_allocator(
    sxml_test_state *state) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state,
                     sxml_counting_allocator_memory_requirements(
                         &(sxml_counting_allocator_configuration){
                             .backing_allocator = {.state = NULL,
                                                   .operations = NULL},
                             .failing_allocation_number = 0,
                         },
                         &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, requirements.byte_count == 0);
}

static void test_counting_allocator_initialize_checks_memory(
    sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[128] = {0};
    sxml_counting_allocator_configuration configuration = {
        .backing_allocator = sxml_default_allocator(),
        .failing_allocation_number = 0,
    };
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_REQUIRE(state, sxml_counting_allocator_memory_requirements(
                                 &configuration, &requirements) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(storage));
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_counting_allocator_initialize(
                         (sxml_memory_view){
                             .bytes = storage,
                             .byte_count = requirements.byte_count - 1},
                         &configuration, &counting_allocator) ==
                         SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_initialize(
                                (sxml_memory_view){
                                    .bytes = storage + 1,
                                    .byte_count = sizeof(storage) - 1},
                                &configuration, &counting_allocator) ==
                                SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, counting_allocator == NULL);
}

/* create and destroy, observed through a second counting allocator. */
static void test_counting_allocator_create_and_destroy_balance(
    sxml_test_state *state) {
    counting_fixture observer = {.storage = {0}, .counting_allocator = NULL};
    SXML_TEST_REQUIRE(state,
                      counting_fixture_initialize(
                          &observer,
                          &(sxml_counting_allocator_configuration){
                              .backing_allocator = sxml_default_allocator(),
                              .failing_allocation_number = 0,
                          }) == SXML_STATUS_OK);
    sxml_allocator observed =
        sxml_counting_allocator_as_allocator(observer.counting_allocator);
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_counting_allocator_create(
                          observed,
                          &(sxml_counting_allocator_configuration){
                              .backing_allocator = sxml_default_allocator(),
                              .failing_allocation_number = 0,
                          },
                          &counting_allocator) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(
                                observer.counting_allocator)
                                    .live_allocation_count == 1);
    sxml_counting_allocator_destroy(counting_allocator);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(
                                observer.counting_allocator)
                                    .live_allocation_count == 0);
    sxml_counting_allocator_finalize(observer.counting_allocator);
}

static void test_counting_allocator_create_reports_failures(
    sxml_test_state *state) {
    counting_fixture observer = {.storage = {0}, .counting_allocator = NULL};
    SXML_TEST_REQUIRE(state,
                      counting_fixture_initialize(
                          &observer,
                          &(sxml_counting_allocator_configuration){
                              .backing_allocator = sxml_default_allocator(),
                              .failing_allocation_number = 1,
                          }) == SXML_STATUS_OK);
    sxml_allocator observed =
        sxml_counting_allocator_as_allocator(observer.counting_allocator);
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_counting_allocator_create(
                         observed,
                         &(sxml_counting_allocator_configuration){
                             .backing_allocator = sxml_default_allocator(),
                             .failing_allocation_number = 0,
                         },
                         &counting_allocator) == SXML_STATUS_OUT_OF_MEMORY);
    SXML_TEST_EXPECT(state,
                     sxml_counting_allocator_create(
                         observed,
                         &(sxml_counting_allocator_configuration){
                             .backing_allocator = {.state = NULL,
                                                   .operations = NULL},
                             .failing_allocation_number = 0,
                         },
                         &counting_allocator) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, counting_allocator == NULL);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(
                                observer.counting_allocator)
                                    .live_allocation_count == 0);
    sxml_counting_allocator_finalize(observer.counting_allocator);
}

static void test_counting_allocator_finalize_and_destroy_accept_null(
    sxml_test_state *state) {
    sxml_counting_allocator_finalize(NULL);
    sxml_counting_allocator_destroy(NULL);
    SXML_TEST_EXPECT(state, true);
}

static const sxml_test_case allocator_test_cases[] = {
    {.name = "default_allocator_allocates_writable_memory",
     .function = test_default_allocator_allocates_writable_memory},
    {.name = "default_allocator_honours_every_alignment",
     .function = test_default_allocator_honours_every_alignment},
    {.name = "default_allocator_has_no_state",
     .function = test_default_allocator_has_no_state},
    {.name = "counting_allocator_counts_allocations_and_releases",
     .function = test_counting_allocator_counts_allocations_and_releases},
    {.name = "counting_allocator_fails_only_the_chosen_allocation",
     .function = test_counting_allocator_fails_only_the_chosen_allocation},
    {.name = "counting_allocator_counts_backing_failures",
     .function = test_counting_allocator_counts_backing_failures},
    {.name = "counting_allocator_needs_a_backing_allocator",
     .function = test_counting_allocator_needs_a_backing_allocator},
    {.name = "counting_allocator_initialize_checks_memory",
     .function = test_counting_allocator_initialize_checks_memory},
    {.name = "counting_allocator_create_and_destroy_balance",
     .function = test_counting_allocator_create_and_destroy_balance},
    {.name = "counting_allocator_create_reports_failures",
     .function = test_counting_allocator_create_reports_failures},
    {.name = "counting_allocator_finalize_and_destroy_accept_null",
     .function = test_counting_allocator_finalize_and_destroy_accept_null},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "allocator",
            .cases = allocator_test_cases,
            .case_count = SXML_TEST_COUNT(allocator_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
