/*
 * Tests for the optional features (include/sxml/processor.h, "Optional
 * features"; src/optional_feature_internal.h; D189): the configuration's
 * feature view, the processor's slots and feature states, the feature
 * data path through the resolver, and the test feature, with an
 * allocation-failure loop (D23). Every case checks for leaks.
 *
 * Golden file: tests/processor_test.c.
 */
#include "optional_feature_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* A counting allocator for one case; NULL when it cannot be made. */
static sxml_counting_allocator *make_counting_allocator(uint64_t failing_number) {
    sxml_counting_allocator *counting_allocator = NULL;
    if (sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                     .failing_allocation_number = failing_number},
            &counting_allocator) != SXML_STATUS_OK) {
        return NULL;
    }
    return counting_allocator;
}

static uint64_t live_allocations(sxml_counting_allocator *counting_allocator) {
    return sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count;
}

/* A processor's configuration with features and resolver. */
typedef struct feature_setup {
    sxml_allocator allocator;
    sxml_optional_feature_view optional_features;
    sxml_resource_resolver resource_resolver;
} feature_setup;

static sxml_processor_configuration feature_configuration(const feature_setup *setup) {
    return (sxml_processor_configuration){.allocator = setup->allocator,
                                          .resource_resolver = setup->resource_resolver,
                                          .optional_features = setup->optional_features};
}

/* -------------------------------------------------------------- Features */

static void test_test_feature_has_its_name(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_optional_feature_get_name(sxml_test_optional_feature()),
                                TEXT("test")));
    SXML_TEST_EXPECT(state, sxml_optional_feature_get_name(
                                (sxml_optional_feature){.definition = NULL})
                                    .byte_count == 0);
}

static void test_all_features_lists_every_kind_once(sxml_test_state *state) {
    const sxml_optional_feature_view all = sxml_all_optional_features();
    /* Every kind but EXPath File, which a host names on purpose (D184,
       D200). */
    SXML_TEST_EXPECT(state,
                     all.optional_feature_count == (size_t)SXML_OPTIONAL_FEATURE_KIND_COUNT - 1);
    SXML_TEST_REQUIRE(state, sxml_optional_feature_view_is_valid(all));
    size_t expected_kind = 0;
    for (size_t feature_index = 0; feature_index < all.optional_feature_count;
         feature_index += 1) {
        const sxml_optional_feature_definition *definition =
            all.optional_features[feature_index].definition;
        if (expected_kind == (size_t)SXML_OPTIONAL_FEATURE_KIND_EXPATH_FILE) {
            expected_kind += 1;
        }
        /* In kind order, each kind once, each with operations and a name. */
        SXML_TEST_EXPECT(state, (size_t)definition->feature_kind == expected_kind);
        SXML_TEST_EXPECT(state, definition->operations != NULL);
        SXML_TEST_EXPECT(state, definition->feature_name.byte_count > 0);
        expected_kind += 1;
    }
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_optional_feature_get_name(sxml_expath_file_feature()),
                                TEXT("expath-file")));
}

/* -------------------------------------------------------- Configuration */

static void test_processor_without_features_has_none(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = make_counting_allocator(0);
    SXML_TEST_REQUIRE(state, counting_allocator != NULL);
    const feature_setup setup = {
        .allocator = sxml_counting_allocator_as_allocator(counting_allocator)};
    const sxml_processor_configuration configuration = feature_configuration(&setup);
    sxml_processor *processor = NULL;
    SXML_TEST_REQUIRE(state, sxml_processor_create(setup.allocator, &configuration, &processor) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     !sxml_processor_has_optional_feature(processor, sxml_test_optional_feature()));
    /* The D162 place of a feature asks this and keeps its behaviour. */
    SXML_TEST_EXPECT(state, sxml_processor_find_optional_feature(
                                processor, SXML_OPTIONAL_FEATURE_KIND_TEST) == NULL);
    SXML_TEST_EXPECT(state, sxml_processor_find_optional_feature(
                                NULL, SXML_OPTIONAL_FEATURE_KIND_TEST) == NULL);
    int stand_in_state = 0;
    SXML_TEST_EXPECT(state, sxml_processor_set_optional_feature_state(
                                processor, SXML_OPTIONAL_FEATURE_KIND_TEST,
                                &stand_in_state) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_processor_destroy(processor);
    SXML_TEST_EXPECT(state, live_allocations(counting_allocator) == 0);
    sxml_counting_allocator_destroy(counting_allocator);
}

static void test_configuration_names_features(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = make_counting_allocator(0);
    SXML_TEST_REQUIRE(state, counting_allocator != NULL);
    /* Named twice: allowed. The array need not outlive initialize. */
    sxml_optional_feature features[2] = {sxml_test_optional_feature(),
                                         sxml_test_optional_feature()};
    const feature_setup setup = {
        .allocator = sxml_counting_allocator_as_allocator(counting_allocator),
        .optional_features = {.optional_features = features, .optional_feature_count = 2}};
    const sxml_processor_configuration configuration = feature_configuration(&setup);
    sxml_processor *processor = NULL;
    SXML_TEST_REQUIRE(state, sxml_processor_create(setup.allocator, &configuration, &processor) ==
                                 SXML_STATUS_OK);
    memset(features, 0, sizeof(features));
    SXML_TEST_EXPECT(state,
                     sxml_processor_has_optional_feature(processor, sxml_test_optional_feature()));
    SXML_TEST_EXPECT(state, !sxml_processor_has_optional_feature(
                                processor, (sxml_optional_feature){.definition = NULL}));
    SXML_TEST_EXPECT(state, sxml_processor_find_optional_feature(
                                processor, SXML_OPTIONAL_FEATURE_KIND_TEST) != NULL);
    SXML_TEST_EXPECT(state, sxml_processor_get_optional_feature_state(
                                processor, SXML_OPTIONAL_FEATURE_KIND_TEST) == NULL);
    sxml_processor_destroy(processor);
    SXML_TEST_EXPECT(state, live_allocations(counting_allocator) == 0);
    sxml_counting_allocator_destroy(counting_allocator);
}

static void test_configuration_rejects_malformed_views(sxml_test_state *state) {
    const sxml_optional_feature no_feature = {.definition = NULL};
    const sxml_optional_feature_view malformed_views[] = {
        {.optional_features = NULL, .optional_feature_count = 1},
        {.optional_features = &no_feature, .optional_feature_count = 1},
    };
    for (size_t view_index = 0; view_index < SXML_TEST_COUNT(malformed_views);
         view_index += 1) {
        const feature_setup setup = {.allocator = sxml_default_allocator(),
                                     .optional_features = malformed_views[view_index]};
        const sxml_processor_configuration configuration = feature_configuration(&setup);
        sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
        SXML_TEST_EXPECT(state, sxml_processor_memory_requirements(&configuration,
                                                                   &requirements) ==
                                    SXML_STATUS_INVALID_ARGUMENT);
        sxml_processor *processor = NULL;
        SXML_TEST_EXPECT(state, sxml_processor_create(setup.allocator, &configuration,
                                                      &processor) == SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, processor == NULL);
    }
}

/* ---------------------------------------------------------- Feature data */

static const sxml_resource_entry test_feature_data_entries[] = {
    /* Initializers, not compound literals: a static initializer must be
       constant (C17, 6.7.9; GCC's -Wpedantic). */
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:sxml:test-feature-data"),
     .content = SXML_BYTE_VIEW_INITIALIZER("twelve bytes")},
};

static const sxml_resource_entry_view test_feature_data = {
    .resource_entries = test_feature_data_entries, .resource_entry_count = 1};

/* One run of the test feature over a resolver. */
typedef struct feature_data_run {
    uint64_t failing_number;
    sxml_resource_resolver resolver;
    /* Whether the resolver has the feature's data. */
    bool has_data;
} feature_data_run;

/* Makes a processor with the test feature over run->resolver, asks the
   feature for its data twice, and checks the count it gives and that
   nothing leaked. Returns the first failure. */
static sxml_status read_test_feature_data(sxml_test_state *state, const feature_data_run *run) {
    sxml_counting_allocator *counting_allocator = make_counting_allocator(run->failing_number);
    if (counting_allocator == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    const sxml_optional_feature feature = sxml_test_optional_feature();
    const feature_setup setup = {
        .allocator = sxml_counting_allocator_as_allocator(counting_allocator),
        .optional_features = {.optional_features = &feature, .optional_feature_count = 1},
        .resource_resolver = run->resolver};
    const sxml_processor_configuration configuration = feature_configuration(&setup);
    sxml_processor *processor = NULL;
    sxml_status status = sxml_processor_create(setup.allocator, &configuration, &processor);
    for (int reading = 0; reading < 2 && status == SXML_STATUS_OK; reading += 1) {
        const sxml_test_feature_operations *operations = sxml_processor_find_optional_feature(
            processor, SXML_OPTIONAL_FEATURE_KIND_TEST);
        size_t byte_count = 99;
        status = operations->read_data_byte_count(processor, &byte_count);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, byte_count == (run->has_data ? 12U : 0U));
            /* Kept after the first reading when there was data. */
            SXML_TEST_EXPECT(state, (sxml_processor_get_optional_feature_state(
                                         processor, SXML_OPTIONAL_FEATURE_KIND_TEST) != NULL) ==
                                        run->has_data);
        }
    }
    if (status == SXML_STATUS_OK && run->has_data) {
        /* A feature's state is set once. */
        int other_state = 0;
        SXML_TEST_EXPECT(state, sxml_processor_set_optional_feature_state(
                                    processor, SXML_OPTIONAL_FEATURE_KIND_TEST, &other_state) ==
                                    SXML_STATUS_INVALID_ARGUMENT);
    }
    sxml_processor_destroy(processor);
    SXML_TEST_EXPECT(state, live_allocations(counting_allocator) == 0);
    sxml_counting_allocator_destroy(counting_allocator);
    return status;
}

static void test_feature_reads_its_data_through_the_resolver(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, read_test_feature_data(
                                state, &(feature_data_run){
                                           .failing_number = 0,
                                           .resolver = sxml_table_resource_resolver(
                                               &test_feature_data),
                                           .has_data = true}) == SXML_STATUS_OK);
}

static void test_feature_without_data_reads_none(sxml_test_state *state) {
    const sxml_resource_entry_view no_entries = {.resource_entries = NULL,
                                                 .resource_entry_count = 0};
    SXML_TEST_EXPECT(state, read_test_feature_data(
                                state, &(feature_data_run){
                                           .failing_number = 0,
                                           .resolver = sxml_table_resource_resolver(&no_entries),
                                           .has_data = false}) == SXML_STATUS_OK);
    /* The null resolver has nothing either. */
    SXML_TEST_EXPECT(state, read_test_feature_data(
                                state, &(feature_data_run){
                                           .failing_number = 0,
                                           .resolver = {.state = NULL, .operations = NULL},
                                           .has_data = false}) == SXML_STATUS_OK);
}

static void test_feature_data_survives_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; failing_number < 1000 && status != SXML_STATUS_OK; failing_number += 1) {
        status = read_test_feature_data(
            state, &(feature_data_run){
                       .failing_number = failing_number,
                       .resolver = sxml_table_resource_resolver(&test_feature_data),
                       .has_data = true});
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
}

/* A reader that fails, to check that its failure is returned. */
static sxml_status failing_read(void *state, sxml_byte_view content) {
    (void)state;
    (void)content;
    return SXML_STATUS_MALFORMED_INPUT;
}

static const sxml_feature_data_reader_operations failing_reader_operations = {
    .read = failing_read,
};

static void test_read_data_returns_the_readers_failure(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = make_counting_allocator(0);
    SXML_TEST_REQUIRE(state, counting_allocator != NULL);
    const feature_setup setup = {
        .allocator = sxml_counting_allocator_as_allocator(counting_allocator),
        .resource_resolver = sxml_table_resource_resolver(&test_feature_data)};
    const sxml_processor_configuration configuration = feature_configuration(&setup);
    sxml_processor *processor = NULL;
    SXML_TEST_REQUIRE(state, sxml_processor_create(setup.allocator, &configuration, &processor) ==
                                 SXML_STATUS_OK);
    sxml_feature_data_outcome outcome = {.is_available = true};
    SXML_TEST_EXPECT(state, sxml_optional_feature_read_data(
                                &(sxml_feature_data_request){
                                    .processor = processor,
                                    .resource_kind = SXML_RESOURCE_KIND_BINARY,
                                    .uri = TEXT("urn:sxml:test-feature-data"),
                                    .reader = {.state = NULL,
                                               .operations = &failing_reader_operations}},
                                &outcome) == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, !outcome.is_available);
    /* No reader: an invalid request. */
    SXML_TEST_EXPECT(state, sxml_optional_feature_read_data(
                                &(sxml_feature_data_request){
                                    .processor = processor,
                                    .resource_kind = SXML_RESOURCE_KIND_BINARY,
                                    .uri = TEXT("urn:sxml:test-feature-data"),
                                    .reader = {.state = NULL, .operations = NULL}},
                                &outcome) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_processor_destroy(processor);
    SXML_TEST_EXPECT(state, live_allocations(counting_allocator) == 0);
    sxml_counting_allocator_destroy(counting_allocator);
}

static const sxml_test_case optional_feature_test_cases[] = {
    {.name = "test_feature_has_its_name", .function = test_test_feature_has_its_name},
    {.name = "all_features_lists_every_kind_once",
     .function = test_all_features_lists_every_kind_once},
    {.name = "processor_without_features_has_none",
     .function = test_processor_without_features_has_none},
    {.name = "configuration_names_features", .function = test_configuration_names_features},
    {.name = "configuration_rejects_malformed_views",
     .function = test_configuration_rejects_malformed_views},
    {.name = "feature_reads_its_data_through_the_resolver",
     .function = test_feature_reads_its_data_through_the_resolver},
    {.name = "feature_without_data_reads_none", .function = test_feature_without_data_reads_none},
    {.name = "feature_data_survives_every_allocation_failure",
     .function = test_feature_data_survives_every_allocation_failure},
    {.name = "read_data_returns_the_readers_failure",
     .function = test_read_data_returns_the_readers_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "optional_feature",
            .cases = optional_feature_test_cases,
            .case_count = SXML_TEST_COUNT(optional_feature_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
