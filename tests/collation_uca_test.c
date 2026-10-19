/*
 * Tests for the UCA collation feature (src/collation_uca.c, packet 7.15a,
 * D213) through src/collation_internal.h: a processor that names
 * sxml_uca_collation_feature() and serves the vendored DUCET
 * (third_party/unicode-collation/allkeys.txt) reads its collation element
 * table once; the samples of Unicode's CollationTest (non-ignorable and
 * shifted) sort in their files' order; the parameters of F&O 4.0, 5.2.4
 * (strength, alternate, maxVariable, caseFirst, caseLevel, backwards,
 * numeric) and fallback=no; substring matching over contractions and
 * expansions; implicit weights; data that is missing or malformed. Every
 * case checks for leaks; an allocation-failure loop covers reading the
 * table and comparing (D23).
 *
 * Golden files: tests/collation_test.c (direct calls through a counting
 * allocator), tests/optional_feature_test.c (a processor with a feature
 * and a table resolver), tests/xml_conformance_test.c (reading a vendored
 * suite next to the sources, skipped when it is not there).
 */
#include "collation_internal.h"
#include "optional_feature_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)
#define UCA_URI "http://www.w3.org/2013/collation/UCA"

enum { SXML_TEST_PATH_BYTE_COUNT = 4096 };

/* ------------------------------------------------------------- Files */

/* The path of a file in third_party/unicode-collation/. */
static void data_path(const char *file_name, char *path) {
    const char *source = __FILE__;
    const char *slash = strrchr(source, '/');
    const size_t directory_byte_count = slash == NULL ? 0 : (size_t)(slash - source) + 1;
    (void)snprintf(path, SXML_TEST_PATH_BYTE_COUNT, "%.*s../third_party/unicode-collation/%s",
                   (int)directory_byte_count, source, file_name);
}

/* Reads a whole file through the default allocator; NULL bytes when it
   cannot be read. */
static sxml_byte_view read_file(const char *file_name) {
    char path[SXML_TEST_PATH_BYTE_COUNT];
    data_path(file_name, path);
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return contents;
    }
    size_t capacity = (size_t)1 << 22;
    size_t byte_count = 0;
    void *memory = NULL;
    while (true) {
        void *grown = NULL;
        if (sxml_allocator_allocate(sxml_default_allocator(),
                                    (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
                                    &grown) != SXML_STATUS_OK) {
            sxml_allocator_release(sxml_default_allocator(), memory);
            (void)fclose(file);
            return contents;
        }
        if (byte_count > 0) {
            memcpy(grown, memory, byte_count);
        }
        sxml_allocator_release(sxml_default_allocator(), memory);
        memory = grown;
        const size_t read_count = fread((uint8_t *)memory + byte_count, 1, capacity - byte_count, file);
        byte_count += read_count;
        if (byte_count < capacity) {
            break;
        }
        capacity *= 2;
    }
    (void)fclose(file);
    contents.bytes = memory;
    contents.byte_count = byte_count;
    return contents;
}

static void release_file(sxml_byte_view contents) {
    sxml_allocator_release(sxml_default_allocator(), (void *)contents.bytes);
}

/* ---------------------------------------------------------- Fixture */

static const sxml_byte_view ducet_uri =
    SXML_BYTE_VIEW_INITIALIZER("http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt");

/* A processor with the UCA collation feature over a table resolver. */
typedef struct uca_fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_allocator allocator;
    sxml_resource_entry entry;
    sxml_resource_entry_view entries;
    sxml_processor *processor;
} uca_fixture;

/* What a fixture is made with: the data the resolver serves for the
   DUCET's URI (none when has_data is false), whether the processor names
   the feature, and the allocation to fail. */
typedef struct fixture_request {
    sxml_byte_view data;
    bool has_data;
    bool has_feature;
    uint64_t failing_number;
} fixture_request;

static sxml_status make_fixture(fixture_request request, uca_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     request.failing_number},
        &fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    fixture->allocator = sxml_counting_allocator_as_allocator(fixture->counting_allocator);
    fixture->entry = (sxml_resource_entry){.uri = ducet_uri, .content = request.data};
    fixture->entries = (sxml_resource_entry_view){.resource_entries = &fixture->entry,
                                                  .resource_entry_count = request.has_data ? 1 : 0};
    const sxml_optional_feature feature = sxml_uca_collation_feature();
    return sxml_processor_create(
        fixture->allocator,
        &(sxml_processor_configuration){
            .allocator = fixture->allocator,
            .resource_resolver = sxml_table_resource_resolver(&fixture->entries),
            .optional_features = {.optional_features = &feature,
                                  .optional_feature_count = request.has_feature ? 1 : 0}},
        &fixture->processor);
}

/* Destroys the fixture; false when something leaked. */
static bool finish_fixture(uca_fixture *fixture) {
    sxml_processor_destroy(fixture->processor);
    if (fixture->counting_allocator == NULL) {
        return true;
    }
    const bool has_no_leak =
        sxml_counting_allocator_read_counts(fixture->counting_allocator).live_allocation_count == 0;
    sxml_counting_allocator_destroy(fixture->counting_allocator);
    return has_no_leak;
}

static sxml_collation_resolution resolve(const uca_fixture *fixture, const char *uri,
                                         sxml_status *status) {
    sxml_collation_resolution resolution;
    *status = sxml_collation_resolve(
        &(sxml_collation_resolution_request){.processor = fixture->processor,
                                             .collation_uri = sxml_byte_view_from_c_string(uri)},
        &resolution);
    return resolution;
}

static sxml_string test_string(sxml_byte_view text) {
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                          .codepoint_count = 0,
                          .is_ascii = true};
    if (sxml_string_from_utf8(text, &string) != SXML_STATUS_OK) {
        string.codepoint_count = 0;
    }
    return string;
}

typedef struct comparison_case {
    const char *collation_uri;
    const char *first_text;
    const char *second_text;
    sxml_ordering expected_ordering;
} comparison_case;

static sxml_status compare_texts(const uca_fixture *fixture, const comparison_case *test_case,
                                 sxml_ordering *ordering) {
    sxml_status status = SXML_STATUS_OK;
    const sxml_collation_resolution resolution =
        resolve(fixture, test_case->collation_uri, &status);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!resolution.is_supported) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    return sxml_collation_compare(
        &(sxml_collation_comparison){
            .collation = &resolution.collation,
            .allocator = fixture->allocator,
            .first = test_string(sxml_byte_view_from_c_string(test_case->first_text)),
            .second = test_string(sxml_byte_view_from_c_string(test_case->second_text))},
        ordering);
}

/* ---------------------------------------------------------- The table */

static void test_feature_reads_the_table_once(sxml_test_state *state) {
    const sxml_byte_view data = read_file("allkeys.txt");
    if (data.bytes == NULL) {
        printf("skipped: third_party/unicode-collation/allkeys.txt is not reachable\n");
        return;
    }
    uca_fixture fixture;
    SXML_TEST_REQUIRE(state, make_fixture((fixture_request){.data = data,
                                                            .has_data = true,
                                                            .has_feature = true,
                                                            .failing_number = 0},
                                          &fixture) == SXML_STATUS_OK);
    sxml_status status = SXML_STATUS_OK;
    const sxml_collation_resolution first = resolve(&fixture, UCA_URI, &status);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && first.is_supported &&
                                first.collation.collation_kind == SXML_COLLATION_KIND_UCA &&
                                first.collation.element_table != NULL);
    const sxml_collation_resolution second =
        resolve(&fixture, UCA_URI "?fallback=no;version=16.0.0;lang=en-GB", &status);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && second.is_supported &&
                                second.collation.element_table == first.collation.element_table);
    /* fallback=no with what sxml cannot honour: FOCH0002 (F&O 4.0,
       5.2.4). */
    const char *const unsupported_uris[] = {
        UCA_URI "?fallback=no;version=6.0.0", UCA_URI "?fallback=no;lang=de",
        UCA_URI "?fallback=no;keyword=unknown", UCA_URI "?fallback=no;strength=unknown",
        UCA_URI "?fallback=no;reorder=Grek"};
    for (size_t uri_index = 0; uri_index < SXML_TEST_COUNT(unsupported_uris); uri_index += 1) {
        const sxml_collation_resolution resolution =
            resolve(&fixture, unsupported_uris[uri_index], &status);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && !resolution.is_supported);
    }
    /* The same with fallback=yes: the table, the parameter ignored. */
    const sxml_collation_resolution tolerant =
        resolve(&fixture, UCA_URI "?lang=de;keyword=unknown", &status);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && tolerant.is_supported &&
                                tolerant.collation.collation_kind == SXML_COLLATION_KIND_UCA);
    SXML_TEST_EXPECT(state, finish_fixture(&fixture));
    release_file(data);
}

static void test_without_feature_or_data_falls_back(sxml_test_state *state) {
    for (size_t run_index = 0; run_index < 3; run_index += 1) {
        /* No feature; the feature without data; no processor at all. */
        uca_fixture fixture;
        SXML_TEST_REQUIRE(state,
                          make_fixture((fixture_request){.data = TEXT(""),
                                                         .has_data = false,
                                                         .has_feature = run_index == 1,
                                                         .failing_number = 0},
                                       &fixture) == SXML_STATUS_OK);
        if (run_index == 2) {
            sxml_processor_destroy(fixture.processor);
            fixture.processor = NULL;
        }
        sxml_status status = SXML_STATUS_OK;
        const sxml_collation_resolution fallback = resolve(&fixture, UCA_URI, &status);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && fallback.is_supported &&
                                    fallback.collation.collation_kind ==
                                        SXML_COLLATION_KIND_UCA_FALLBACK);
        const sxml_collation_resolution strict =
            resolve(&fixture, UCA_URI "?fallback=no", &status);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && !strict.is_supported);
        const sxml_collation_resolution unknown =
            resolve(&fixture, "http://example.com/collation", &status);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && !unknown.is_supported);
        SXML_TEST_EXPECT(state, finish_fixture(&fixture));
    }
}

static void test_malformed_data_is_reported(sxml_test_state *state) {
    const sxml_byte_view malformed[] = {
        TEXT("0041 ; [.2380.0020.0008\n"),
        TEXT("0041 ; [.12345.0020.0008]\n"),
        TEXT("@version 16.0.0\n0041 [.2380.0020.0008]\n"),
        /* No digit zero, and no version. */
        TEXT("@version 16.0.0\n0041 ; [.2380.0020.0008]\n"),
        TEXT("0030 ; [.217D.0020.0002]\n"),
        TEXT("@implicitweights 17000; FB00\n"),
    };
    for (size_t data_index = 0; data_index < SXML_TEST_COUNT(malformed); data_index += 1) {
        uca_fixture fixture;
        SXML_TEST_REQUIRE(state, make_fixture((fixture_request){.data = malformed[data_index],
                                                                .has_data = true,
                                                                .has_feature = true,
                                                                .failing_number = 0},
                                              &fixture) == SXML_STATUS_OK);
        sxml_status status = SXML_STATUS_OK;
        const sxml_collation_resolution resolution = resolve(&fixture, UCA_URI, &status);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_MALFORMED_INPUT && !resolution.is_supported);
        SXML_TEST_EXPECT(state, finish_fixture(&fixture));
    }
    /* A small well-formed table: what it lacks weighs implicitly. */
    uca_fixture fixture;
    SXML_TEST_REQUIRE(
        state, make_fixture((fixture_request){.data = TEXT("# comment\n@version 16.0.0\n"
                                                           "0030 ; [.217D.0020.0002] # zero\n"
                                                           "0061 ; [.2380.0020.0002]\r\n"
                                                           "0062 0063 ; [.2381.0020.0002]\n"),
                                              .has_data = true,
                                              .has_feature = true,
                                              .failing_number = 0},
                            &fixture) == SXML_STATUS_OK);
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    SXML_TEST_EXPECT(state, compare_texts(&fixture,
                                          &(comparison_case){.collation_uri = UCA_URI,
                                                             .first_text = "bc",
                                                             .second_text = "a"},
                                          &ordering) == SXML_STATUS_OK &&
                                ordering == SXML_ORDERING_GREATER);
    SXML_TEST_EXPECT(state, compare_texts(&fixture,
                                          &(comparison_case){.collation_uri = UCA_URI,
                                                             .first_text = "b",
                                                             .second_text = "bc"},
                                          &ordering) == SXML_STATUS_OK &&
                                ordering == SXML_ORDERING_GREATER);
    SXML_TEST_EXPECT(state, finish_fixture(&fixture));
}

/* ------------------------------------------------------ CollationTest */

/* Reads one line of a CollationTest file into UTF-8 in text; false for a
   line without codepoints, or one with a surrogate, which sxml strings
   cannot hold. */
typedef struct test_line {
    uint8_t bytes[256];
    size_t byte_count;
} test_line;

static bool read_test_line(sxml_byte_view line, test_line *text) {
    text->byte_count = 0;
    size_t byte_index = 0;
    while (byte_index < line.byte_count && line.bytes[byte_index] != ';' &&
           line.bytes[byte_index] != '#') {
        if (line.bytes[byte_index] == ' ') {
            byte_index += 1;
            continue;
        }
        uint32_t codepoint = 0;
        while (byte_index < line.byte_count && line.bytes[byte_index] != ' ' &&
               line.bytes[byte_index] != ';') {
            const uint8_t byte = line.bytes[byte_index];
            const uint32_t digit = byte <= '9' ? (uint32_t)(byte - '0') : (uint32_t)(byte - 'A' + 10);
            codepoint = (codepoint * 16U) + digit;
            byte_index += 1;
        }
        if ((codepoint >= 0xD800U && codepoint <= 0xDFFFU) || text->byte_count + 4 > 256) {
            return false;
        }
        uint8_t *out = text->bytes + text->byte_count;
        if (codepoint < 0x80U) {
            out[0] = (uint8_t)codepoint;
            text->byte_count += 1;
        } else if (codepoint < 0x800U) {
            out[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
            out[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            text->byte_count += 2;
        } else if (codepoint < 0x10000U) {
            out[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
            out[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
            out[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            text->byte_count += 3;
        } else {
            out[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
            out[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
            out[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
            out[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            text->byte_count += 4;
        }
    }
    return text->byte_count > 0;
}

/* Checks that every line of a CollationTest file sorts at or after the
   one before it (CollationTest.html: the keys, then the codepoints). */
typedef struct collation_test_run {
    const char *file_name;
    const char *collation_uri;
} collation_test_run;

static void check_collation_test(sxml_test_state *state, collation_test_run run) {
    const sxml_byte_view data = read_file("allkeys.txt");
    const sxml_byte_view tests = read_file(run.file_name);
    if (data.bytes == NULL || tests.bytes == NULL) {
        printf("skipped: third_party/unicode-collation/ is not reachable\n");
        release_file(data);
        release_file(tests);
        return;
    }
    uca_fixture fixture;
    SXML_TEST_REQUIRE(state, make_fixture((fixture_request){.data = data,
                                                            .has_data = true,
                                                            .has_feature = true,
                                                            .failing_number = 0},
                                          &fixture) == SXML_STATUS_OK);
    sxml_status status = SXML_STATUS_OK;
    const sxml_collation_resolution resolution = resolve(&fixture, run.collation_uri, &status);
    SXML_TEST_REQUIRE(state, status == SXML_STATUS_OK && resolution.is_supported);
    test_line previous = {.byte_count = 0};
    bool has_previous = false;
    size_t line_count = 0;
    size_t failure_count = 0;
    size_t line_start = 0;
    while (line_start < tests.byte_count && status == SXML_STATUS_OK) {
        size_t line_end = line_start;
        while (line_end < tests.byte_count && tests.bytes[line_end] != '\n') {
            line_end += 1;
        }
        const sxml_byte_view line = {.bytes = tests.bytes + line_start,
                                     .byte_count = line_end - line_start};
        line_start = line_end + 1;
        test_line current;
        if (line.byte_count == 0 || line.bytes[0] == '#' || !read_test_line(line, &current)) {
            continue;
        }
        line_count += 1;
        if (has_previous) {
            sxml_ordering ordering = SXML_ORDERING_EQUAL;
            status = sxml_collation_compare(
                &(sxml_collation_comparison){
                    .collation = &resolution.collation,
                    .allocator = fixture.allocator,
                    .first = test_string((sxml_byte_view){.bytes = previous.bytes,
                                                          .byte_count = previous.byte_count}),
                    .second = test_string((sxml_byte_view){.bytes = current.bytes,
                                                           .byte_count = current.byte_count})},
                &ordering);
            if (status == SXML_STATUS_OK && ordering == SXML_ORDERING_GREATER) {
                if (failure_count < 10) {
                    printf("%s: out of order: %.*s\n", run.file_name, (int)line.byte_count,
                           (const char *)line.bytes);
                }
                failure_count += 1;
            }
        }
        previous = current;
        has_previous = true;
    }
    printf("%s: %zu lines, %zu out of order\n", run.file_name, line_count, failure_count);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, line_count > 10000 && failure_count == 0);
    SXML_TEST_EXPECT(state, finish_fixture(&fixture));
    release_file(data);
    release_file(tests);
}

static void test_collation_test_non_ignorable(sxml_test_state *state) {
    check_collation_test(state, (collation_test_run){
                                    .file_name = "CollationTest_NON_IGNORABLE_SAMPLE.txt",
                                    .collation_uri = UCA_URI "?strength=identical"});
}

static void test_collation_test_shifted(sxml_test_state *state) {
    /* The DUCET's variable elements are space, punctuation and symbols:
       maxVariable=symbol. */
    check_collation_test(state, (collation_test_run){
                                    .file_name = "CollationTest_SHIFTED_SAMPLE.txt",
                                    .collation_uri = UCA_URI "?strength=identical;"
                                                             "alternate=shifted;maxVariable=symbol"});
}

/* ---------------------------------------------------------- Parameters */

static const comparison_case parameter_cases[] = {
    /* QT4 compare-031, -034, -035, -037, -039, -040, -041, -043. */
    {UCA_URI "?lang=en;strength=tertiary;caseFirst=upper", "database", "Database",
     SXML_ORDERING_GREATER},
    {UCA_URI "?lang=en;numeric=yes", "Chap2", "Chap10", SXML_ORDERING_LESS},
    {UCA_URI "?lang=en;numeric=yes", "Chap2-b", "Chap10-b", SXML_ORDERING_LESS},
    {UCA_URI "?lang=en;strength=primary;caseLevel=yes;caseFirst=lower", "DATABASE",
     "D\xC3\x83TAbase", SXML_ORDERING_GREATER},
    {UCA_URI "?lang=en;strength=tertiary;backwards=yes", "DATAB\xC3\x83SE", "D\xC3\x83TABASE",
     SXML_ORDERING_GREATER},
    {UCA_URI "?lang=en;alternate=blanked;strength=tertiary", "database", "data base",
     SXML_ORDERING_EQUAL},
    {UCA_URI "?lang=en;alternate=blanked;strength=quaternary", "database", "data base",
     SXML_ORDERING_EQUAL},
    {UCA_URI "?lang=en;alternate=shifted;strength=tertiary", "database", "data base",
     SXML_ORDERING_EQUAL},
    {UCA_URI "?alternate=shifted;strength=quaternary", "database", "data base",
     SXML_ORDERING_GREATER},
    /* QT4 collation-key-009u and -204. */
    {UCA_URI "?lang=en;caseFirst=upper", "abc", "ABC", SXML_ORDERING_GREATER},
    {UCA_URI "?strength=primary", "abc123", "ABC 123", SXML_ORDERING_GREATER},
    /* Without numeric, digits weigh one by one. */
    {UCA_URI, "Chap2", "Chap10", SXML_ORDERING_GREATER},
    {UCA_URI "?numeric=yes", "007", "7", SXML_ORDERING_EQUAL},
    {UCA_URI "?numeric=yes", "a99", "a100", SXML_ORDERING_LESS},
    {UCA_URI "?numeric=yes", "x10y", "x9z", SXML_ORDERING_GREATER},
    {UCA_URI, "a", "A", SXML_ORDERING_LESS},
    {UCA_URI "?strength=secondary", "a", "A", SXML_ORDERING_EQUAL},
    {UCA_URI "?strength=secondary", "e", "\xC3\xA9", SXML_ORDERING_LESS},
    {UCA_URI "?strength=primary", "e", "\xC3\xA9", SXML_ORDERING_EQUAL},
    /* Canonically equivalent strings: e + U+0301 and U+00E9, also on the
       identical level, which compares NFD (UTS #10, section 7.4). */
    {UCA_URI, "e\xCC\x81", "\xC3\xA9", SXML_ORDERING_EQUAL},
    {UCA_URI "?strength=identical", "e\xCC\x81", "\xC3\xA9", SXML_ORDERING_EQUAL},
    /* An expansion: U+00DF weighs as ss on the primary level. */
    {UCA_URI "?strength=primary", "\xC3\x9F", "ss", SXML_ORDERING_EQUAL},
    {UCA_URI, "\xC3\x9F", "ss", SXML_ORDERING_GREATER},
    /* Implicit weights: Han in codepoint order, after Latin; unassigned
       after Han. */
    {UCA_URI, "\xE4\xB8\x80", "\xE4\xB8\x81", SXML_ORDERING_LESS},
    {UCA_URI, "z", "\xE4\xB8\x80", SXML_ORDERING_LESS},
    {UCA_URI, "\xF0\xA0\x80\x80", "\xE4\xB8\x80", SXML_ORDERING_GREATER},
    {UCA_URI, "\xE4\xB8\x80", "\xF3\xB0\x80\x80", SXML_ORDERING_LESS},
    /* maxVariable: "$" is a currency symbol, variable only under currency;
       otherwise its primary weight is below b's. */
    {UCA_URI "?alternate=shifted;maxVariable=symbol", "a$b", "ab", SXML_ORDERING_LESS},
    {UCA_URI "?alternate=shifted;maxVariable=currency", "a$b", "ab", SXML_ORDERING_EQUAL},
    {UCA_URI "?alternate=shifted;maxVariable=space", "a-b", "ab", SXML_ORDERING_LESS},
    {UCA_URI "?alternate=shifted", "a-b", "ab", SXML_ORDERING_EQUAL},
};

static void test_parameters_order_strings(sxml_test_state *state) {
    const sxml_byte_view data = read_file("allkeys.txt");
    if (data.bytes == NULL) {
        printf("skipped: third_party/unicode-collation/allkeys.txt is not reachable\n");
        return;
    }
    uca_fixture fixture;
    SXML_TEST_REQUIRE(state, make_fixture((fixture_request){.data = data,
                                                            .has_data = true,
                                                            .has_feature = true,
                                                            .failing_number = 0},
                                          &fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(parameter_cases); case_index += 1) {
        const comparison_case *test_case = &parameter_cases[case_index];
        sxml_ordering ordering = SXML_ORDERING_EQUAL;
        const sxml_status status = compare_texts(&fixture, test_case, &ordering);
        const bool is_expected =
            status == SXML_STATUS_OK && ordering == test_case->expected_ordering;
        if (!is_expected) {
            printf("case %zu: %s \"%s\" \"%s\": status %d, ordering %d\n", case_index,
                   test_case->collation_uri, test_case->first_text, test_case->second_text,
                   (int)status, (int)ordering);
        }
        SXML_TEST_EXPECT(state, is_expected);
    }
    SXML_TEST_EXPECT(state, finish_fixture(&fixture));
    release_file(data);
}

/* ------------------------------------------------------------ Matching */

typedef struct match_case {
    const char *collation_uri;
    const char *input_text;
    const char *search_text;
    sxml_collation_match_kind match_kind;
    bool is_found;
    size_t byte_offset;
    size_t byte_count;
} match_case;

static const match_case match_cases[] = {
    /* The expansion of U+00DF matches ss, and the other way round. */
    {UCA_URI "?strength=primary", "Strasse", "\xC3\x9F", SXML_COLLATION_MATCH_KIND_FIRST, true, 4,
     2},
    {UCA_URI "?strength=primary", "Stra\xC3\x9F" "e", "ss", SXML_COLLATION_MATCH_KIND_FIRST, true,
     4, 2},
    {UCA_URI "?strength=primary", "caf\xC3\xA9s", "E", SXML_COLLATION_MATCH_KIND_LAST, true, 3, 2},
    {UCA_URI, "caf\xC3\xA9s", "e", SXML_COLLATION_MATCH_KIND_FIRST, false, 0, 0},
    /* The mark after e is a unit of its own on the secondary level, so
       "e" is not at the end. */
    {UCA_URI "?strength=secondary", "cafe\xCC\x81", "e", SXML_COLLATION_MATCH_KIND_END, false, 0,
     0},
    {UCA_URI "?alternate=shifted", "data base", "ab", SXML_COLLATION_MATCH_KIND_FIRST, true, 3,
     3},
    {UCA_URI "?numeric=yes", "file10.txt", "10", SXML_COLLATION_MATCH_KIND_FIRST, true, 4, 2},
    {UCA_URI "?numeric=yes", "file10.txt", "1", SXML_COLLATION_MATCH_KIND_FIRST, false, 0, 0},
    {UCA_URI "?strength=identical", "abc", "B", SXML_COLLATION_MATCH_KIND_FIRST, false, 0, 0},
    {UCA_URI "?strength=identical", "aBc", "B", SXML_COLLATION_MATCH_KIND_START, false, 0, 0},
    {UCA_URI "?strength=identical", "aBc", "aB", SXML_COLLATION_MATCH_KIND_START, true, 0, 2},
};

static void test_matching_uses_collation_units(sxml_test_state *state) {
    const sxml_byte_view data = read_file("allkeys.txt");
    if (data.bytes == NULL) {
        printf("skipped: third_party/unicode-collation/allkeys.txt is not reachable\n");
        return;
    }
    uca_fixture fixture;
    SXML_TEST_REQUIRE(state, make_fixture((fixture_request){.data = data,
                                                            .has_data = true,
                                                            .has_feature = true,
                                                            .failing_number = 0},
                                          &fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(match_cases); case_index += 1) {
        const match_case *test_case = &match_cases[case_index];
        sxml_status status = SXML_STATUS_OK;
        const sxml_collation_resolution resolution =
            resolve(&fixture, test_case->collation_uri, &status);
        sxml_collation_match match = {.is_found = false, .byte_offset = 0, .byte_count = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_collation_find_match(
                &(sxml_collation_match_request){
                    .collation = &resolution.collation,
                    .allocator = fixture.allocator,
                    .input = test_string(sxml_byte_view_from_c_string(test_case->input_text)),
                    .search = test_string(sxml_byte_view_from_c_string(test_case->search_text)),
                    .match_kind = test_case->match_kind},
                &match);
        }
        const bool is_expected =
            status == SXML_STATUS_OK && match.is_found == test_case->is_found &&
            (!match.is_found || (match.byte_offset == test_case->byte_offset &&
                                 match.byte_count == test_case->byte_count));
        if (!is_expected) {
            printf("match case %zu: status %d, found %d at %zu, %zu bytes\n", case_index,
                   (int)status, (int)match.is_found, match.byte_offset, match.byte_count);
        }
        SXML_TEST_EXPECT(state, is_expected);
    }
    SXML_TEST_EXPECT(state, finish_fixture(&fixture));
    release_file(data);
}

/* --------------------------------------------------- Allocation failure */

/* Reads the table and compares two strings with the allocation
   failing_number failing. */
static sxml_status compare_with_failure(sxml_test_state *state, sxml_byte_view data,
                                        uint64_t failing_number) {
    uca_fixture fixture;
    sxml_status status = make_fixture((fixture_request){.data = data,
                                                        .has_data = true,
                                                        .has_feature = true,
                                                        .failing_number = failing_number},
                                      &fixture);
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    if (status == SXML_STATUS_OK) {
        status = compare_texts(&fixture,
                               &(comparison_case){.collation_uri = UCA_URI "?numeric=yes",
                                                  .first_text = "Chap\xC3\xA9 10",
                                                  .second_text = "chape\xCC\x81 9"},
                               &ordering);
    }
    if (status == SXML_STATUS_OK) {
        SXML_TEST_EXPECT(state, ordering == SXML_ORDERING_GREATER);
    }
    SXML_TEST_EXPECT(state, finish_fixture(&fixture));
    return status;
}

static void test_survives_every_allocation_failure(sxml_test_state *state) {
    const sxml_byte_view data = read_file("allkeys.txt");
    if (data.bytes == NULL) {
        printf("skipped: third_party/unicode-collation/allkeys.txt is not reachable\n");
        return;
    }
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; failing_number < 1000 && status != SXML_STATUS_OK; failing_number += 1) {
        status = compare_with_failure(state, data, failing_number);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    release_file(data);
}

static const sxml_test_case collation_uca_test_cases[] = {
    {.name = "feature_reads_the_table_once", .function = test_feature_reads_the_table_once},
    {.name = "without_feature_or_data_falls_back",
     .function = test_without_feature_or_data_falls_back},
    {.name = "malformed_data_is_reported", .function = test_malformed_data_is_reported},
    {.name = "collation_test_non_ignorable", .function = test_collation_test_non_ignorable},
    {.name = "collation_test_shifted", .function = test_collation_test_shifted},
    {.name = "parameters_order_strings", .function = test_parameters_order_strings},
    {.name = "matching_uses_collation_units", .function = test_matching_uses_collation_units},
    {.name = "survives_every_allocation_failure",
     .function = test_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "collation_uca",
            .cases = collation_uca_test_cases,
            .case_count = SXML_TEST_COUNT(collation_uca_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
