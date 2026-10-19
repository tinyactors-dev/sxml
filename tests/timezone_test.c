/*
 * Tests for the timezone database (src/timezone_internal.h, packet 7.16,
 * D209): TZ rules (POSIX.1-2024, XBD 8.3; RFC 8536, section 3.3.1), the
 * TZif reader (RFC 8536) on files built here and every truncation of
 * one, and the feature through the public API: fn:civil-timezone and the
 * $place of fn:format-dateTime, fn:format-date and fn:format-time over the
 * vendored tzdata (third_party/tzdata/zoneinfo, found from this file's
 * path; the cases that need it say so and pass where it cannot be read),
 * without the feature, and under every allocation failure (D23). Every
 * case checks for leaks.
 *
 * Golden file: tests/function_library_temporal_test.c (fixture, running,
 * expression cases); the source-tree lookup imitates
 * tests/xml_conformance_test.c.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"
#include "timezone_internal.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

enum { SXML_TEST_PATH_BYTE_COUNT = 4096, SXML_TEST_MAXIMUM_FILE_BYTE_COUNT = 1 << 20 };

/* The number of digit_count digits at text. */
static int read_digits(const char *text, size_t digit_count) {
    int value = 0;
    for (size_t digit_index = 0; digit_index < digit_count; digit_index += 1) {
        value = value * 10 + (text[digit_index] - '0');
    }
    return value;
}

/* POSIX seconds of a UTC date and time written YYYY-MM-DDThh:mm:ss, for
   the cases below. */
static int64_t instant_of(const char *date_time_text) {
    int64_t instant_seconds = 0;
    (void)sxml_timezone_instant_of(
        (sxml_date_time){.year = read_digits(date_time_text, 4),
                         .month = (uint8_t)read_digits(date_time_text + 5, 2),
                         .day = (uint8_t)read_digits(date_time_text + 8, 2),
                         .hour = (uint8_t)read_digits(date_time_text + 11, 2),
                         .minute = (uint8_t)read_digits(date_time_text + 14, 2),
                         .second = (uint8_t)read_digits(date_time_text + 17, 2),
                         .nanosecond = 0,
                         .has_timezone = true,
                         .timezone_offset_minutes = 0},
        &instant_seconds);
    return instant_seconds;
}

static bool abbreviation_is(sxml_local_time_type type, const char *expected) {
    return sxml_byte_view_equals(type.abbreviation, sxml_byte_view_from_c_string(expected));
}

/* -------------------------------------------------------------- Rules */

static void test_rules_read_posix_strings(sxml_test_state *state) {
    sxml_timezone_rule rule;
    SXML_TEST_REQUIRE(state,
                      sxml_timezone_rule_read(TEXT("EST5EDT,M3.2.0,M11.1.0"), &rule) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, rule.standard_time.offset_seconds == -18000);
    SXML_TEST_EXPECT(state, rule.has_daylight_saving);
    SXML_TEST_EXPECT(state, rule.daylight_saving_time.offset_seconds == -14400);
    SXML_TEST_EXPECT(state, rule.start.day_kind == SXML_TIMEZONE_RULE_DAY_MONTH_WEEK);
    SXML_TEST_EXPECT(state, rule.start.month == 3 && rule.start.week == 2 && rule.start.day == 0);
    SXML_TEST_EXPECT(state, rule.end.time_seconds == 7200);
    SXML_TEST_EXPECT(state, abbreviation_is(rule.daylight_saving_time, "EDT"));
    /* Quoted names, minutes, no daylight saving time. */
    SXML_TEST_REQUIRE(state, sxml_timezone_rule_read(TEXT("<+0530>-5:30"), &rule) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, rule.standard_time.offset_seconds == 19800);
    SXML_TEST_EXPECT(state, !rule.has_daylight_saving);
    SXML_TEST_EXPECT(state, abbreviation_is(rule.standard_time, "+0530"));
    /* RFC 8536, section 3.3.1: negative and long times. */
    SXML_TEST_REQUIRE(state, sxml_timezone_rule_read(TEXT("<-02>2<-01>,M3.5.0/-1,M10.5.0/0"),
                                                     &rule) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, rule.start.time_seconds == -3600);
    SXML_TEST_REQUIRE(state,
                      sxml_timezone_rule_read(TEXT("EST5EDT,0/0,J365/167"), &rule) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, rule.end.day_kind == SXML_TIMEZONE_RULE_DAY_JULIAN);
    SXML_TEST_EXPECT(state, rule.end.time_seconds == (int64_t)167 * 3600);
    /* A daylight saving time without a rule takes the United States'. */
    SXML_TEST_REQUIRE(state, sxml_timezone_rule_read(TEXT("PST8PDT"), &rule) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, rule.end.month == 11 && rule.end.week == 1);
    static const char *const malformed[] = {
        "", "ES5", "EST", "EST5EDT,M3.2.0", "EST5EDT,M13.2.0,M11.1.0", "EST5EDT,M3.6.0,M11.1.0",
        "EST25", "<EST5", "EST5EDT,J0,J365", "EST5EDT,M3.2.0,M11.1.0,", "EST5EDT,M3.2.0/168,M11.1.0",
        "EST5:60",
    };
    for (size_t text_index = 0; text_index < SXML_TEST_COUNT(malformed); text_index += 1) {
        SXML_TEST_EXPECT(state, sxml_timezone_rule_read(sxml_byte_view_from_c_string(
                                                            malformed[text_index]),
                                                        &rule) == SXML_STATUS_MALFORMED_INPUT);
    }
}

static void test_rules_find_daylight_saving_time(sxml_test_state *state) {
    sxml_timezone_rule rule;
    SXML_TEST_REQUIRE(state,
                      sxml_timezone_rule_read(TEXT("EST5EDT,M3.2.0,M11.1.0"), &rule) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_timezone_rule_find(&rule, instant_of("2015-02-15T12:00:00"))
                                    .offset_seconds == -18000);
    SXML_TEST_EXPECT(state, sxml_timezone_rule_find(&rule, instant_of("2015-08-15T12:00:00"))
                                    .offset_seconds == -14400);
    /* 2015-03-08 02:00 EST is 07:00Z; 2015-11-01 02:00 EDT is 06:00Z. */
    SXML_TEST_EXPECT(state, !sxml_timezone_rule_find(&rule, instant_of("2015-03-08T06:59:59"))
                                 .is_daylight_saving);
    SXML_TEST_EXPECT(state, sxml_timezone_rule_find(&rule, instant_of("2015-03-08T07:00:00"))
                                .is_daylight_saving);
    SXML_TEST_EXPECT(state, sxml_timezone_rule_find(&rule, instant_of("2015-11-01T05:59:59"))
                                .is_daylight_saving);
    SXML_TEST_EXPECT(state, !sxml_timezone_rule_find(&rule, instant_of("2015-11-01T06:00:00"))
                                 .is_daylight_saving);
    /* The southern hemisphere: Sydney is in daylight saving time in
       February. */
    SXML_TEST_REQUIRE(state, sxml_timezone_rule_read(TEXT("AEST-10AEDT,M10.1.0,M4.1.0/3"), &rule) ==
                                 SXML_STATUS_OK);
    const sxml_local_time_type february =
        sxml_timezone_rule_find(&rule, instant_of("2015-02-15T12:00:00"));
    SXML_TEST_EXPECT(state, february.offset_seconds == 39600 && abbreviation_is(february, "AEDT"));
    SXML_TEST_EXPECT(state, sxml_timezone_rule_find(&rule, instant_of("2015-08-15T12:00:00"))
                                    .offset_seconds == 36000);
    /* Daylight saving time all year (tzdata's way of writing it). */
    SXML_TEST_REQUIRE(state,
                      sxml_timezone_rule_read(TEXT("EST5EDT,0/0,J365/25"), &rule) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_timezone_rule_find(&rule, instant_of("2030-01-01T05:00:00"))
                                .is_daylight_saving);
    SXML_TEST_EXPECT(state, sxml_timezone_rule_find(&rule, instant_of("2030-12-31T23:00:00"))
                                .is_daylight_saving);
    /* Week 5 is the last: in February 2015 the last Sunday is the 22nd. */
    SXML_TEST_REQUIRE(state, sxml_timezone_rule_read(TEXT("XST0XDT,M2.5.0/0,M12.1.0/0"), &rule) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !sxml_timezone_rule_find(&rule, instant_of("2015-02-21T23:59:59"))
                                 .is_daylight_saving);
    SXML_TEST_EXPECT(state, sxml_timezone_rule_find(&rule, instant_of("2015-02-22T00:00:00"))
                                .is_daylight_saving);
}

/* --------------------------------------------------------------- TZif */

/* A TZif file built byte by byte. */
typedef struct tzif_builder {
    uint8_t bytes[512];
    size_t byte_count;
} tzif_builder;

static void put_bytes(tzif_builder *builder, const void *bytes, size_t byte_count) {
    memcpy(builder->bytes + builder->byte_count, bytes, byte_count);
    builder->byte_count += byte_count;
}

static void put_unsigned(tzif_builder *builder, uint64_t value, size_t byte_count) {
    for (size_t byte_index = 0; byte_index < byte_count; byte_index += 1) {
        builder->bytes[builder->byte_count + byte_index] =
            (uint8_t)(value >> (8U * (byte_count - 1 - byte_index)));
    }
    builder->byte_count += byte_count;
}

/* The counts of a header: transitions, types, abbreviation bytes. */
typedef struct tzif_counts {
    uint32_t transition_count;
    uint32_t type_count;
    uint32_t character_count;
} tzif_counts;

static void put_header(tzif_builder *builder, uint8_t version, tzif_counts counts) {
    put_bytes(builder, "TZif", 4);
    put_bytes(builder, &version, 1);
    for (size_t padding = 0; padding < 15; padding += 1) {
        put_unsigned(builder, 0, 1);
    }
    put_unsigned(builder, 0, 4);
    put_unsigned(builder, 0, 4);
    put_unsigned(builder, 0, 4);
    put_unsigned(builder, counts.transition_count, 4);
    put_unsigned(builder, counts.type_count, 4);
    put_unsigned(builder, counts.character_count, 4);
}

/* The test zone: "BBB" +00:00 before 0, "AAA" +01:00 daylight saving
   time from 0, "BBB" from 1000, then the footer's rule. type_index_of_last
   breaks the file when it is 2 or more. */
typedef struct test_zone {
    uint8_t version;
    const char *footer;
    uint8_t type_index_of_last;
    int64_t last_transition;
} test_zone;

static void build_test_zone(tzif_builder *builder, test_zone zone) {
    builder->byte_count = 0;
    const size_t time_size = zone.version == 0 ? 4 : 8;
    if (zone.version != 0) {
        /* A slim file's 32-bit block: one type, one byte of abbreviation. */
        put_header(builder, zone.version,
                   (tzif_counts){.transition_count = 0, .type_count = 1, .character_count = 1});
        put_unsigned(builder, 0, 4);
        put_unsigned(builder, 0, 1);
        put_unsigned(builder, 0, 1);
        put_unsigned(builder, 0, 1);
    }
    put_header(builder, zone.version,
               (tzif_counts){.transition_count = 2, .type_count = 2, .character_count = 8});
    put_unsigned(builder, 0, time_size);
    put_unsigned(builder, (uint64_t)zone.last_transition, time_size);
    put_unsigned(builder, 1, 1);
    put_unsigned(builder, zone.type_index_of_last, 1);
    /* Type 0: BBB, +0, standard; type 1: AAA, +3600, daylight saving. */
    put_unsigned(builder, 0, 4);
    put_unsigned(builder, 0, 1);
    put_unsigned(builder, 0, 1);
    put_unsigned(builder, 3600, 4);
    put_unsigned(builder, 1, 1);
    put_unsigned(builder, 4, 1);
    put_bytes(builder, "BBB\0AAA\0", 8);
    if (zone.version != 0) {
        put_bytes(builder, "\n", 1);
        put_bytes(builder, zone.footer, strlen(zone.footer));
        put_bytes(builder, "\n", 1);
    }
}

static const test_zone valid_zone = {
    .version = '2', .footer = "BBB0", .type_index_of_last = 0, .last_transition = 1000};

static void test_zones_read_tzif_files(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_REQUIRE(state, sxml_counting_allocator_create(
                                 sxml_default_allocator(),
                                 &(sxml_counting_allocator_configuration){
                                     .backing_allocator = sxml_default_allocator(),
                                     .failing_allocation_number = 0},
                                 &counting_allocator) == SXML_STATUS_OK);
    const sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting_allocator);
    tzif_builder builder;
    static const uint8_t versions[] = {'2', '3', '4', 0};
    for (size_t version_index = 0; version_index < SXML_TEST_COUNT(versions);
         version_index += 1) {
        test_zone description = valid_zone;
        description.version = versions[version_index];
        build_test_zone(&builder, description);
        sxml_timezone_zone zone;
        SXML_TEST_REQUIRE(state, sxml_timezone_zone_read(
                                     allocator,
                                     (sxml_byte_view){.bytes = builder.bytes,
                                                      .byte_count = builder.byte_count},
                                     &zone) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, zone.transition_count == 2 && zone.local_time_type_count == 2);
        SXML_TEST_EXPECT(state, abbreviation_is(sxml_timezone_zone_find(&zone, -5), "BBB"));
        SXML_TEST_EXPECT(state, abbreviation_is(sxml_timezone_zone_find(&zone, 0), "AAA"));
        SXML_TEST_EXPECT(state, sxml_timezone_zone_find(&zone, 999).offset_seconds == 3600);
        SXML_TEST_EXPECT(state, sxml_timezone_zone_find(&zone, 5000).offset_seconds == 0);
        SXML_TEST_EXPECT(state, zone.has_rule == (versions[version_index] != 0));
        SXML_TEST_EXPECT(state, abbreviation_is(sxml_timezone_zone_standard_time(&zone), "BBB"));
        sxml_timezone_zone_finalize(&zone, allocator);
    }
    /* Every truncation of a valid file is malformed. */
    build_test_zone(&builder, valid_zone);
    for (size_t byte_count = 0; byte_count < builder.byte_count; byte_count += 1) {
        sxml_timezone_zone zone;
        SXML_TEST_EXPECT(state, sxml_timezone_zone_read(
                                    allocator,
                                    (sxml_byte_view){.bytes = builder.bytes, .byte_count = byte_count},
                                    &zone) == SXML_STATUS_MALFORMED_INPUT);
    }
    /* A type index out of range, transitions out of order, a bad footer,
       a bad magic number. */
    static const test_zone broken_zones[] = {
        {.version = '2', .footer = "BBB0", .type_index_of_last = 2, .last_transition = 1000},
        {.version = '2', .footer = "BBB0", .type_index_of_last = 0, .last_transition = 0},
        {.version = '2', .footer = "1BBB", .type_index_of_last = 0, .last_transition = 1000},
    };
    for (size_t zone_index = 0; zone_index < SXML_TEST_COUNT(broken_zones); zone_index += 1) {
        build_test_zone(&builder, broken_zones[zone_index]);
        sxml_timezone_zone zone;
        SXML_TEST_EXPECT(state, sxml_timezone_zone_read(
                                    allocator,
                                    (sxml_byte_view){.bytes = builder.bytes,
                                                     .byte_count = builder.byte_count},
                                    &zone) == SXML_STATUS_MALFORMED_INPUT);
    }
    build_test_zone(&builder, valid_zone);
    builder.bytes[0] = 'X';
    sxml_timezone_zone zone;
    SXML_TEST_EXPECT(state, sxml_timezone_zone_read(allocator,
                                                    (sxml_byte_view){.bytes = builder.bytes,
                                                                     .byte_count = builder.byte_count},
                                                    &zone) == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state,
                     sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count == 0);
    sxml_counting_allocator_destroy(counting_allocator);
}

static void test_instants_need_a_timezone_and_a_year_in_range(sxml_test_state *state) {
    int64_t instant_seconds = 0;
    SXML_TEST_EXPECT(state, instant_of("1970-01-02T00:00:01") == 86401);
    SXML_TEST_EXPECT(state, instant_of("1969-12-31T23:00:00") == -3600);
    SXML_TEST_EXPECT(state, !sxml_timezone_instant_of((sxml_date_time){.year = 2000,
                                                                      .month = 1,
                                                                      .day = 1,
                                                                      .has_timezone = false},
                                                      &instant_seconds));
    SXML_TEST_EXPECT(state, !sxml_timezone_instant_of((sxml_date_time){.year = INT64_MAX,
                                                                      .month = 1,
                                                                      .day = 1,
                                                                      .has_timezone = true},
                                                      &instant_seconds));
}

/* ---------------------------------------------------- Through the API */

/* Serves urn:sxml:timezone-data:PATH from a zoneinfo directory, or
   nothing when directory is NULL. */
typedef struct zoneinfo_resolver {
    const char *directory;
    size_t request_count;
} zoneinfo_resolver;

static sxml_status resolve_zoneinfo(void *state, const sxml_resource_request *request,
                                    sxml_resource *resource) {
    zoneinfo_resolver *resolver = state;
    const sxml_byte_view prefix = TEXT(SXML_TIMEZONE_DATA_URI_PREFIX);
    resource->resource_form = SXML_RESOURCE_FORM_NONE;
    const sxml_byte_view uri = request->absolute_uri;
    if (request->resource_kind != SXML_RESOURCE_KIND_TIMEZONE_DATA || resolver->directory == NULL ||
        uri.byte_count <= prefix.byte_count || uri.byte_count > 300 ||
        memcmp(uri.bytes, prefix.bytes, prefix.byte_count) != 0) {
        return SXML_STATUS_OK;
    }
    resolver->request_count += 1;
    char path[SXML_TEST_PATH_BYTE_COUNT];
    (void)snprintf(path, sizeof path, "%s%.*s", resolver->directory,
                   (int)(uri.byte_count - prefix.byte_count),
                   (const char *)uri.bytes + prefix.byte_count);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return SXML_STATUS_OK;
    }
    uint8_t *contents = NULL;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        sxml_default_allocator(),
        (sxml_allocation_request){.byte_count = SXML_TEST_MAXIMUM_FILE_BYTE_COUNT, .alignment = 1}, &memory);
    if (status == SXML_STATUS_OK) {
        contents = memory;
        const size_t byte_count = fread(contents, 1, SXML_TEST_MAXIMUM_FILE_BYTE_COUNT, file);
        resource->resource_form = SXML_RESOURCE_FORM_BYTES;
        resource->content = (sxml_byte_view){.bytes = contents, .byte_count = byte_count};
    }
    (void)fclose(file);
    return status;
}

static void release_zoneinfo(void *state, const sxml_resource *resource) {
    (void)state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        sxml_allocator_release(sxml_default_allocator(), (void *)resource->content.bytes);
    }
}

static const sxml_resource_resolver_operations zoneinfo_resolver_operations = {
    .resolve = resolve_zoneinfo,
    .release = release_zoneinfo,
};

/* The vendored zoneinfo directory with a trailing "/", in path; false
   when it cannot be read. */
static bool find_vendored_zoneinfo(char path[SXML_TEST_PATH_BYTE_COUNT]) {
    const char *source_file = __FILE__;
    const char *last_separator = strrchr(source_file, '/');
    const size_t directory_byte_count =
        last_separator == NULL ? 0 : (size_t)(last_separator - source_file) + 1;
    (void)snprintf(path, SXML_TEST_PATH_BYTE_COUNT, "%.*s../third_party/tzdata/zoneinfo/",
                   (int)directory_byte_count, source_file);
    char probe[SXML_TEST_PATH_BYTE_COUNT + 16];
    (void)snprintf(probe, sizeof probe, "%szone1970.tab", path);
    FILE *file = fopen(probe, "rb");
    if (file == NULL) {
        printf("skipped: the vendored tzdata is not reachable at %s\n", probe);
        return false;
    }
    (void)fclose(file);
    return true;
}

/* 2026-10-16T12:30:45-05:00, implicit timezone +02:00, as in
   tests/function_library_temporal_test.c. */
static const sxml_clock_reading fixture_clock_reading = {
    .current_date_time = {.year = 2026,
                          .month = 10,
                          .day = 16,
                          .hour = 12,
                          .minute = 30,
                          .second = 45,
                          .nanosecond = 0,
                          .has_timezone = true,
                          .timezone_offset_minutes = -300},
    .implicit_timezone_offset_minutes = 120,
};

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    zoneinfo_resolver resolver;
} fixture;

/* How a fixture is made. */
typedef struct fixture_setup {
    uint64_t failing_allocation_number;
    bool names_feature;
    const char *zoneinfo_directory;
} fixture_setup;

static sxml_status fixture_create(fixture_setup setup, fixture *test_fixture) {
    memset(test_fixture, 0, sizeof(*test_fixture));
    test_fixture->resolver.directory = setup.zoneinfo_directory;
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = setup.failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
    const sxml_optional_feature feature = sxml_timezone_database_feature();
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .clock = sxml_fixed_clock(&fixture_clock_reading),
            .resource_resolver = {.state = &test_fixture->resolver,
                                  .operations = &zoneinfo_resolver_operations},
            .optional_features = {.optional_features = &feature,
                                  .optional_feature_count = setup.names_feature ? 1 : 0}},
        &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    return status;
}

static bool fixture_destroy(fixture *test_fixture) {
    bool has_no_leak = true;
    if (test_fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(test_fixture->processor);
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && sxml_counting_allocator_read_counts(
                                         test_fixture->counting_allocator)
                                             .live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

typedef struct query_run {
    sxml_byte_view expression_text;
    sxml_error_record error_record;
} query_run;

/* Compiles and evaluates the expression and writes its value by the
   adaptive method into the fixture's output buffer. */
static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        sxml_counting_allocator_as_allocator(test_fixture->counting_allocator),
        &(sxml_xpath_expression_configuration){.processor = test_fixture->processor,
                                               .expression_text = run->expression_text,
                                               .source_name = TEXT("test.xpath"),
                                               .error_record = &run->error_record},
        &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xpath_expression_evaluate(
        expression, &(sxml_xpath_evaluation_request){.error_record = &run->error_record},
        &result);
    sxml_xpath_expression_destroy(expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_serialize_sequence(
        sxml_processor_get_item_heap(test_fixture->processor),
        &(sxml_serialization_request){
            .value = result,
            .serialization_parameters = {.serialization_method =
                                             SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                         .has_item_separator = true,
                                         .item_separator = TEXT(" ")},
            .error_record = &run->error_record},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    sxml_sequence_release(result);
    return status;
}

typedef struct expression_case {
    const char *expression_text;
    const char *expected_output;
    const char *expected_error_code;
} expression_case;

#define VALUE_CASE(text, output)                                              \
    {.expression_text = (text), .expected_output = (output), .expected_error_code = NULL}
#define ERROR_CASE(text, code)                                                \
    {.expression_text = (text), .expected_output = NULL, .expected_error_code = (code)}

static bool case_holds(const fixture *test_fixture, const expression_case *test_case) {
    query_run run = {.expression_text = sxml_byte_view_from_c_string(test_case->expression_text)};
    const sxml_status status = run_query(test_fixture, &run);
    bool holds = false;
    if (test_case->expected_error_code != NULL) {
        holds = status != SXML_STATUS_OK &&
                sxml_byte_view_equals(run.error_record.code.local_name,
                                      sxml_byte_view_from_c_string(test_case->expected_error_code));
    } else {
        holds = status == SXML_STATUS_OK &&
                sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                      sxml_byte_view_from_c_string(test_case->expected_output));
    }
    if (!holds) {
        const sxml_byte_view output = sxml_output_buffer_get_bytes(test_fixture->output_buffer);
        const sxml_byte_view status_name = sxml_status_name(status);
        (void)fprintf(stderr, "case %s: status %.*s, error %.*s, output %.*s\n",
                      test_case->expression_text, (int)status_name.byte_count,
                      (const char *)status_name.bytes,
                      (int)run.error_record.code.local_name.byte_count,
                      (const char *)run.error_record.code.local_name.bytes,
                      (int)output.byte_count, (const char *)output.bytes);
    }
    return holds;
}

/* Expression cases to run in one fixture. */
typedef struct case_list {
    const expression_case *cases;
    size_t case_count;
} case_list;

#define CASE_LIST(array) ((case_list){.cases = (array), .case_count = SXML_TEST_COUNT(array)})

static void expect_all(sxml_test_state *state, fixture_setup setup, case_list list) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(setup, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        SXML_TEST_EXPECT(state, case_holds(&test_fixture, &list.cases[case_index]));
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_civil_timezone_reads_zones(sxml_test_state *state) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    if (!find_vendored_zoneinfo(directory)) {
        return;
    }
    static const expression_case cases[] = {
        /* F&O 4.0, section 9.7.4 examples. */
        VALUE_CASE("civil-timezone(xs:dateTime('2024-12-31T23:59:59'), 'America/New_York')",
                   "xs:dayTimeDuration(\"-PT5H\")"),
        VALUE_CASE("civil-timezone(xs:dateTime('2024-06-30T23:59:59'), 'America/New_York')",
                   "xs:dayTimeDuration(\"-PT4H\")"),
        /* QT4 civil-timezone-003 to -007 and -009. */
        VALUE_CASE("civil-timezone(xs:dateTime('2024-11-05T12:00:00Z'), 'Europe/Paris')",
                   "xs:dayTimeDuration(\"PT1H\")"),
        VALUE_CASE("civil-timezone(xs:dateTime('2024-05-05T12:00:00Z'), 'Europe/Paris')",
                   "xs:dayTimeDuration(\"PT2H\")"),
        VALUE_CASE("civil-timezone(current-dateTime(), 'Asia/Kolkata')",
                   "xs:dayTimeDuration(\"PT5H30M\")"),
        VALUE_CASE("civil-timezone(current-dateTime(), 'America/Lima')",
                   "xs:dayTimeDuration(\"-PT5H\")"),
        ERROR_CASE("civil-timezone(current-dateTime(), 'North/Pole')", "FODT0004"),
        VALUE_CASE("civil-timezone(xs:dateTime('2024-01-01T12:00:00'), 'Asia/Kolkata')",
                   "xs:dayTimeDuration(\"PT5H30M\")"),
        /* A backward link, local mean time before the first transition,
           the footer's rule far ahead. */
        VALUE_CASE("civil-timezone(xs:dateTime('2024-01-01T12:00:00Z'), 'US/Eastern')",
                   "xs:dayTimeDuration(\"-PT5H\")"),
        VALUE_CASE("civil-timezone(xs:dateTime('1850-01-01T12:00:00Z'), 'Europe/Paris')",
                   "xs:dayTimeDuration(\"PT9M21S\")"),
        VALUE_CASE("civil-timezone(xs:dateTime('2400-07-01T12:00:00Z'), 'Europe/Paris')",
                   "xs:dayTimeDuration(\"PT2H\")"),
        /* No place: the implicit timezone; UTC without the database. */
        VALUE_CASE("civil-timezone(current-dateTime())", "xs:dayTimeDuration(\"PT2H\")"),
        VALUE_CASE("civil-timezone(current-dateTime(), 'UTC')", "xs:dayTimeDuration(\"PT0S\")"),
        /* Names the database never asks for; a country code is no zone. */
        ERROR_CASE("civil-timezone(current-dateTime(), '../etc/passwd')", "FODT0004"),
        ERROR_CASE("civil-timezone(current-dateTime(), '/Europe/Paris')", "FODT0004"),
        ERROR_CASE("civil-timezone(current-dateTime(), 'Europe//Paris')", "FODT0004"),
        ERROR_CASE("civil-timezone(current-dateTime(), 'zone1970.tab')", "FODT0004"),
        ERROR_CASE("civil-timezone(current-dateTime(), 'fr')", "FODT0004"),
        ERROR_CASE("civil-timezone(current-dateTime(), '')", "FODT0004"),
        /* A year beyond the database's range. */
        ERROR_CASE("civil-timezone(xs:dateTime('99999999999-01-01T00:00:00Z'), 'Europe/Paris')",
                   "FODT0004"),
    };
    expect_all(state,
               (fixture_setup){.names_feature = true, .zoneinfo_directory = directory},
               CASE_LIST(cases));
}

static void test_formatting_uses_the_place(sxml_test_state *state) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    if (!find_vendored_zoneinfo(directory)) {
        return;
    }
    static const expression_case cases[] = {
        /* QT4 format-dateTime-019a to -019h. */
        VALUE_CASE("format-dateTime(xs:dateTime('2015-02-15T12:00:00Z'), "
                   "'[D] [MNn] [Y0001] @ [H01]:[m01] [Z]', 'en', (), 'America/New_York')",
                   "\"15 February 2015 @ 07:00 -05:00\""),
        VALUE_CASE("format-dateTime(xs:dateTime('2015-08-15T12:00:00Z'), "
                   "'[D] [MNn] [Y0001] @ [H01]:[m01] [ZN]', 'en', (), 'America/New_York')",
                   "\"15 August 2015 @ 08:00 EDT\""),
        VALUE_CASE("format-dateTime(xs:dateTime('2015-02-15T12:00:00Z'), "
                   "'[D] [MNn] [Y0001] @ [H01]:[m01] [ZN]', 'en', (), 'Europe/Paris')",
                   "\"15 February 2015 @ 13:00 CET\""),
        VALUE_CASE("format-dateTime(xs:dateTime('2015-08-15T12:00:00Z'), "
                   "'[D] [MNn] [Y0001] @ [H01]:[m01] [ZN]', 'en', (), 'Europe/Paris')",
                   "\"15 August 2015 @ 14:00 CEST\""),
        /* Across midnight; a zone without a name in English. */
        VALUE_CASE("format-dateTime(xs:dateTime('2015-02-15T02:00:00Z'), "
                   "'[Y0001]-[M01]-[D01] [H01]:[m01] [ZN]', (), (), 'America/Lima')",
                   "\"2015-02-14 21:00 -05:00\""),
        /* A country code names without adjusting (QT4 format-dateTime-025b
           and -025d). */
        VALUE_CASE("format-dateTime(xs:dateTime('2018-01-01T12:00:00-10:00'), '[ZN]', (), (), "
                   "'us')",
                   "\"HST\""),
        VALUE_CASE("format-dateTime(xs:dateTime('2018-07-01T12:00:00-05:00'), '[H01] [ZN]', (), "
                   "(), 'US')",
                   "\"12 CDT\""),
        VALUE_CASE("format-dateTime(xs:dateTime('2018-07-01T12:00:00+05:30'), '[ZN]', (), (), "
                   "'us')",
                   "\"+05:30\""),
        /* A time alone takes the zone's standard time (QT4
           format-time-025c). */
        VALUE_CASE("format-time(xs:time('12:00:00-10:00'), '[H00]:[m00] [ZN]', (), (), "
                   "'America/New_York')",
                   "\"17:00 EST\""),
        VALUE_CASE("format-time(xs:time('12:00:00-05:00'), '[ZN]', (), (), 'us')", "\"EST\""),
        /* A date moves with its timezone. */
        VALUE_CASE("format-date(xs:date('2015-08-15Z'), '[D] [ZN]', (), (), 'America/New_York')",
                   "\"14 EDT\""),
        /* Unknown places and values without a timezone change nothing;
           no place gives the fallback format (QT4 format-time-030). */
        VALUE_CASE("format-dateTime(xs:dateTime('2015-02-15T12:00:00Z'), '[H01] [ZN]', (), (), "
                   "'North/Pole')",
                   "\"12 +00:00\""),
        VALUE_CASE("format-dateTime(xs:dateTime('2015-02-15T12:00:00'), '[H01][ZN]', (), (), "
                   "'Europe/Paris')",
                   "\"12\""),
        VALUE_CASE("format-time(xs:time('12:00:00+11:15'), '[H01]:[m01]:[s01] [ZN]')",
                   "\"12:00:00 +11:15\""),
    };
    expect_all(state,
               (fixture_setup){.names_feature = true, .zoneinfo_directory = directory},
               CASE_LIST(cases));
}

static void test_without_the_feature_or_its_data(sxml_test_state *state) {
    static const expression_case cases[] = {
        ERROR_CASE("civil-timezone(current-dateTime(), 'Europe/Paris')", "FODT0004"),
        VALUE_CASE("civil-timezone(current-dateTime(), 'Etc/UTC')", "xs:dayTimeDuration(\"PT0S\")"),
        VALUE_CASE("format-dateTime(xs:dateTime('2015-02-15T12:00:00Z'), '[H01] [ZN]', (), (), "
                   "'Europe/Paris')",
                   "\"12 +00:00\""),
        VALUE_CASE("format-dateTime(xs:dateTime('2015-02-15T12:00:00Z'), '[H01] [ZN]', (), (), "
                   "'fr')",
                   "\"12 +00:00\""),
    };
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    const bool has_data = find_vendored_zoneinfo(directory);
    /* Without the feature, with data the resolver would serve. */
    expect_all(state,
               (fixture_setup){.names_feature = false,
                               .zoneinfo_directory = has_data ? directory : NULL},
               CASE_LIST(cases));
    /* With the feature and no data. */
    expect_all(state, (fixture_setup){.names_feature = true, .zoneinfo_directory = NULL},
               CASE_LIST(cases));
}

static void test_zones_are_read_once(sxml_test_state *state) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    if (!find_vendored_zoneinfo(directory)) {
        return;
    }
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create((fixture_setup){.names_feature = true,
                                                            .zoneinfo_directory = directory},
                                            &test_fixture) == SXML_STATUS_OK);
    static const expression_case paris = VALUE_CASE(
        "(1 to 20) ! civil-timezone(xs:dateTime('2024-11-05T12:00:00Z'), 'Europe/Paris') => "
        "distinct-values()",
        "xs:dayTimeDuration(\"PT1H\")");
    SXML_TEST_EXPECT(state, case_holds(&test_fixture, &paris));
    SXML_TEST_EXPECT(state, case_holds(&test_fixture, &paris));
    SXML_TEST_EXPECT(state, test_fixture.resolver.request_count == 1);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_every_allocation_failure(sxml_test_state *state) {
    char directory[SXML_TEST_PATH_BYTE_COUNT];
    if (!find_vendored_zoneinfo(directory)) {
        return;
    }
    static const expression_case mixed = VALUE_CASE(
        "civil-timezone(xs:dateTime('2024-05-05T12:00:00Z'), 'Europe/Paris'), "
        "format-dateTime(xs:dateTime('2018-07-01T12:00:00-05:00'), '[ZN]', (), (), 'us')",
        "xs:dayTimeDuration(\"PT2H\") \"CDT\"");
    bool succeeded = false;
    for (uint64_t failing_number = 1; failing_number < 5000 && !succeeded; failing_number += 1) {
        fixture test_fixture;
        sxml_status status = fixture_create((fixture_setup){.failing_allocation_number =
                                                                failing_number,
                                                            .names_feature = true,
                                                            .zoneinfo_directory = directory},
                                            &test_fixture);
        if (status == SXML_STATUS_OK) {
            query_run run = {.expression_text = sxml_byte_view_from_c_string(mixed.expression_text)};
            status = run_query(&test_fixture, &run);
            succeeded = status == SXML_STATUS_OK;
            if (succeeded) {
                SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                            sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                            sxml_byte_view_from_c_string(mixed.expected_output)));
            } else {
                SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
            }
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, succeeded);
}

static const sxml_test_case timezone_test_cases[] = {
    {.name = "rules_read_posix_strings", .function = test_rules_read_posix_strings},
    {.name = "rules_find_daylight_saving_time", .function = test_rules_find_daylight_saving_time},
    {.name = "zones_read_tzif_files", .function = test_zones_read_tzif_files},
    {.name = "instants_need_a_timezone_and_a_year_in_range",
     .function = test_instants_need_a_timezone_and_a_year_in_range},
    {.name = "civil_timezone_reads_zones", .function = test_civil_timezone_reads_zones},
    {.name = "formatting_uses_the_place", .function = test_formatting_uses_the_place},
    {.name = "without_the_feature_or_its_data", .function = test_without_the_feature_or_its_data},
    {.name = "zones_are_read_once", .function = test_zones_are_read_once},
    {.name = "every_allocation_failure", .function = test_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "timezone",
            .cases = timezone_test_cases,
            .case_count = SXML_TEST_COUNT(timezone_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
