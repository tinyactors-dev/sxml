/*
 * Tests for src/collation_internal.h (collation URIs, comparison, keys and
 * substring matching under the codepoint, Unicode case-insensitive, HTML
 * ASCII case-insensitive and UCA fallback collations) and, through the
 * public XPath API, for the functions that take collations and the 4.0
 * options of fn:deep-equal (packet 2.13, D116). Every case checks for
 * leaked allocations and cells; allocation-failure loops cover the
 * collation functions and a deep-equal with a callback and unordered
 * comparisons (D23).
 *
 * Golden file: tests/unicode_test.c (direct calls through a counting
 * allocator), tests/xpath_test.c (the processor fixture).
 */
#include "collation_internal.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ---------------------------------------------------------------- Helpers */

static sxml_string test_string(const char *text) {
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                          .codepoint_count = 0,
                          .is_ascii = true};
    /* Test texts are well-formed UTF-8; a failure leaves the empty string
       and the test's expectations fail. */
    if (sxml_string_from_utf8(sxml_byte_view_from_c_string(text), &string) != SXML_STATUS_OK) {
        string.codepoint_count = 0;
    }
    return string;
}

static sxml_collation find_collation(const char *uri) {
    sxml_collation collation = sxml_collation_codepoint();
    collation.strength = SXML_COLLATION_STRENGTH_TERTIARY;
    (void)sxml_collation_find(sxml_byte_view_from_c_string(uri), &collation);
    return collation;
}

/* Two strings ordered under a collation URI, with the expected order. */
typedef struct ordering_case {
    const char *collation_uri;
    const char *first_text;
    const char *second_text;
    sxml_ordering expected_ordering;
} ordering_case;

/* True when compare gives the expected order, the keys order the same way,
   and nothing stays allocated. */
static bool orders_as_expected(const ordering_case *test_case) {
    sxml_counting_allocator *counting_allocator = NULL;
    if (sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                     .failing_allocation_number = 0},
            &counting_allocator) != SXML_STATUS_OK) {
        return false;
    }
    const sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting_allocator);
    const sxml_collation collation = find_collation(test_case->collation_uri);
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    bool is_expected =
        sxml_collation_compare(&(sxml_collation_comparison){.collation = &collation,
                                                            .allocator = allocator,
                                                            .first = test_string(test_case->first_text),
                                                            .second = test_string(test_case->second_text)},
                               &ordering) == SXML_STATUS_OK &&
        ordering == test_case->expected_ordering;
    sxml_collation_key first = {.allocator = allocator, .bytes = NULL};
    sxml_collation_key second = {.allocator = allocator, .bytes = NULL};
    is_expected =
        is_expected &&
        sxml_collation_make_key(&(sxml_collation_key_request){
                                    .collation = &collation,
                                    .input = test_string(test_case->first_text)},
                                &first) == SXML_STATUS_OK &&
        sxml_collation_make_key(&(sxml_collation_key_request){
                                    .collation = &collation,
                                    .input = test_string(test_case->second_text)},
                                &second) == SXML_STATUS_OK;
    if (is_expected) {
        const sxml_byte_view first_bytes = sxml_collation_key_view(&first);
        const sxml_byte_view second_bytes = sxml_collation_key_view(&second);
        const size_t common = first_bytes.byte_count < second_bytes.byte_count
                                  ? first_bytes.byte_count
                                  : second_bytes.byte_count;
        int difference = common == 0 ? 0 : memcmp(first_bytes.bytes, second_bytes.bytes, common);
        if (difference == 0) {
            difference = first_bytes.byte_count < second_bytes.byte_count
                             ? -1
                             : (first_bytes.byte_count > second_bytes.byte_count ? 1 : 0);
        }
        const sxml_ordering key_ordering =
            difference < 0 ? SXML_ORDERING_LESS
                           : (difference > 0 ? SXML_ORDERING_GREATER : SXML_ORDERING_EQUAL);
        is_expected = key_ordering == test_case->expected_ordering;
    }
    sxml_collation_key_release(&first);
    sxml_collation_key_release(&second);
    is_expected = is_expected &&
                  sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count == 0;
    sxml_counting_allocator_destroy(counting_allocator);
    return is_expected;
}

#define CODEPOINT_URI "http://www.w3.org/2005/xpath-functions/collation/codepoint"
#define HTML_URI "http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive"
#define UNICODE_URI "http://www.w3.org/2005/xpath-functions/collation/unicode-case-insensitive"
#define UCA_URI "http://www.w3.org/2013/collation/UCA"

/* ------------------------------------------------------------------ URIs */

static void test_collation_uris(sxml_test_state *state) {
    sxml_collation collation = sxml_collation_codepoint();
    SXML_TEST_EXPECT(state, sxml_collation_find(TEXT(CODEPOINT_URI), &collation) &&
                                collation.collation_kind == SXML_COLLATION_KIND_CODEPOINT);
    SXML_TEST_EXPECT(state,
                     sxml_collation_find(TEXT(HTML_URI), &collation) &&
                         collation.collation_kind ==
                             SXML_COLLATION_KIND_HTML_ASCII_CASE_INSENSITIVE);
    SXML_TEST_EXPECT(state, sxml_collation_find(TEXT(UNICODE_URI), &collation) &&
                                collation.collation_kind ==
                                    SXML_COLLATION_KIND_UNICODE_CASE_INSENSITIVE);
    SXML_TEST_EXPECT(state, sxml_collation_find(TEXT(UCA_URI), &collation) &&
                                collation.collation_kind == SXML_COLLATION_KIND_UCA_FALLBACK &&
                                collation.strength == SXML_COLLATION_STRENGTH_TERTIARY);
    SXML_TEST_EXPECT(state,
                     sxml_collation_find(TEXT(UCA_URI "?lang=de;strength=1;alternate=shifted;"
                                                      "maxVariable=symbol;caseFirst=upper;"
                                                      "caseLevel=yes;backwards=yes;unknown=x"),
                                         &collation) &&
                         collation.strength == SXML_COLLATION_STRENGTH_PRIMARY &&
                         collation.alternate == SXML_COLLATION_ALTERNATE_SHIFTED &&
                         collation.maximum_variable == SXML_COLLATION_VARIABLE_GROUP_SYMBOL &&
                         collation.is_upper_first && collation.has_case_level &&
                         collation.is_backwards);
    /* The last value of a keyword wins; unknown values are ignored. */
    SXML_TEST_EXPECT(state, sxml_collation_find(TEXT(UCA_URI "?strength=primary;strength=bogus;"
                                                             "strength=identical"),
                                                &collation) &&
                                collation.strength == SXML_COLLATION_STRENGTH_IDENTICAL);
    /* fallback=no and bad fallback values are not supported (D97). */
    SXML_TEST_EXPECT(state, !sxml_collation_find(TEXT(UCA_URI "?fallback=no"), &collation));
    SXML_TEST_EXPECT(state, !sxml_collation_find(TEXT(UCA_URI "?fallback=maybe"), &collation));
    SXML_TEST_EXPECT(state, sxml_collation_find(TEXT(UCA_URI "?fallback=no;fallback=yes"),
                                                &collation));
    SXML_TEST_EXPECT(state, !sxml_collation_find(TEXT(UCA_URI "x"), &collation));
    SXML_TEST_EXPECT(state, !sxml_collation_find(TEXT("codepoint"), &collation));
    SXML_TEST_EXPECT(state, !sxml_collation_find(TEXT("http://example.com/c"), &collation));
    SXML_TEST_EXPECT(state, sxml_collation_is_supported(TEXT(HTML_URI)));
    SXML_TEST_EXPECT(state, !sxml_collation_is_supported(TEXT(UCA_URI "?fallback=no")));
    SXML_TEST_EXPECT(state, !sxml_collation_is_supported(TEXT("")));
}

/* -------------------------------------------------------------- Ordering */

static const ordering_case ordering_cases[] = {
    {CODEPOINT_URI, "a", "A", SXML_ORDERING_GREATER},
    {CODEPOINT_URI, "", "a", SXML_ORDERING_LESS},
    {CODEPOINT_URI, "\xC3\xA9", "\xC3\xA9", SXML_ORDERING_EQUAL},
    /* F&O 4.0, section 5.2.6, and QT4 compare-405 to -412. */
    {HTML_URI, "a", "A", SXML_ORDERING_EQUAL},
    {HTML_URI, "a1", "A0", SXML_ORDERING_GREATER},
    {HTML_URI, "Z", "a", SXML_ORDERING_GREATER},
    {HTML_URI, "\xC3\x81", "\xC3\xA1", SXML_ORDERING_LESS},
    /* F&O 4.0, section 5.2.5: lower-case first. */
    {UNICODE_URI, "\xC3\xA4", "\xC3\x84", SXML_ORDERING_EQUAL},
    {UNICODE_URI, "b", "A", SXML_ORDERING_GREATER},
    /* UCA fallback (QT4 compare-017 to -038, fn-collation-UCA-params). */
    {UCA_URI "?strength=primary", "database", "DATABASE", SXML_ORDERING_EQUAL},
    {UCA_URI "?strength=primary", "database", "D\xC3\x83TABASE", SXML_ORDERING_EQUAL},
    {UCA_URI "?strength=primary", "database", "Databases", SXML_ORDERING_LESS},
    {UCA_URI "?strength=secondary", "database", "DATABASE", SXML_ORDERING_EQUAL},
    {UCA_URI "?strength=secondary", "p\xC3\xAA" "che", "p\xC3\xA9" "ch\xC3\xA9",
     SXML_ORDERING_GREATER},
    {UCA_URI "?strength=secondary;backwards=yes", "p\xC3\xAA" "che",
     "p\xC3\xA9" "ch\xC3\xA9", SXML_ORDERING_LESS},
    {UCA_URI "?strength=tertiary", "database", "Database", SXML_ORDERING_LESS},
    {UCA_URI "?caseFirst=upper", "ab", "Ab", SXML_ORDERING_GREATER},
    {UCA_URI "?strength=tertiary", "DATAB\xC3\x83SE", "D\xC3\x83TABASE", SXML_ORDERING_LESS},
    {UCA_URI "?strength=primary;caseLevel=yes", "Epee", "\xC3\xA9pee", SXML_ORDERING_GREATER},
    {UCA_URI "?strength=primary;caseLevel=yes", "Epee", "\xC3\x89pee", SXML_ORDERING_EQUAL},
    /* Canonically equivalent strings are equal below the identical
       level. */
    {UCA_URI, "\xC3\xA9", "e\xCC\x81", SXML_ORDERING_EQUAL},
    {UCA_URI "?strength=identical", "\xC3\xA9", "e\xCC\x81", SXML_ORDERING_GREATER},
    /* A ligature differs from its letters on the tertiary level only. */
    {UCA_URI "?strength=secondary", "\xEF\xAC\x81", "fi", SXML_ORDERING_EQUAL},
    {UCA_URI, "\xEF\xAC\x81", "fi", SXML_ORDERING_GREATER},
    /* Variable characters. */
    {UCA_URI "?alternate=shifted", "a-bc", "abc", SXML_ORDERING_EQUAL},
    {UCA_URI "?alternate=shifted;strength=quaternary", "a-bc", "abc", SXML_ORDERING_LESS},
    {UCA_URI "?alternate=shifted;maxVariable=space", "database", "data-type",
     SXML_ORDERING_GREATER},
    {UCA_URI "?alternate=shifted;maxVariable=symbol", "data=base", "database",
     SXML_ORDERING_EQUAL},
    {UCA_URI "?alternate=blanked", "data base", "database", SXML_ORDERING_EQUAL},
    {UCA_URI "?alternate=blanked;strength=identical", "data base", "database",
     SXML_ORDERING_LESS},
};

static void test_ordering(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(ordering_cases); case_index += 1) {
        const bool passed = orders_as_expected(&ordering_cases[case_index]);
        (void)sxml_test_record_check(state,
                                     (sxml_test_check){.passed = passed,
                                                       .expression =
                                                           ordering_cases[case_index].first_text,
                                                       .file_name = __FILE__,
                                                       .line_number = (uint32_t)__LINE__});
    }
}

/* -------------------------------------------------------------- Matching */

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
    {CODEPOINT_URI, "banana", "ana", SXML_COLLATION_MATCH_KIND_FIRST, true, 1, 3},
    {CODEPOINT_URI, "banana", "ana", SXML_COLLATION_MATCH_KIND_LAST, true, 3, 3},
    {CODEPOINT_URI, "banana", "", SXML_COLLATION_MATCH_KIND_LAST, true, 6, 0},
    {CODEPOINT_URI, "banana", "nab", SXML_COLLATION_MATCH_KIND_FIRST, false, 0, 0},
    {CODEPOINT_URI, "banana", "ban", SXML_COLLATION_MATCH_KIND_START, true, 0, 3},
    {CODEPOINT_URI, "banana", "nan", SXML_COLLATION_MATCH_KIND_END, false, 0, 0},
    {HTML_URI, "BANANA", "ana", SXML_COLLATION_MATCH_KIND_LAST, true, 3, 3},
    {UCA_URI "?strength=primary", "database", "tabaS", SXML_COLLATION_MATCH_KIND_FIRST, true, 2,
     5},
    {UCA_URI "?strength=primary", "d\xC3\xA2tabase", "\xC3\xA2tabase",
     SXML_COLLATION_MATCH_KIND_END, true, 1, 8},
    {UCA_URI "?strength=secondary", "database", "d\xC3\xA2ta", SXML_COLLATION_MATCH_KIND_FIRST,
     false, 0, 0},
    {UCA_URI "?strength=tertiary", "dataBase", "taBas", SXML_COLLATION_MATCH_KIND_FIRST, true, 2,
     5},
    /* A match must not end inside a codepoint's elements: "e" does not
       match the start of "\xC3\xA9" on the secondary level. */
    {UCA_URI "?strength=secondary", "\xC3\xA9t\xC3\xA9", "e", SXML_COLLATION_MATCH_KIND_FIRST,
     false, 0, 0},
    {UCA_URI "?strength=primary", "\xC3\xA9t\xC3\xA9", "e", SXML_COLLATION_MATCH_KIND_LAST, true,
     3, 2},
    {UCA_URI "?alternate=shifted", "abc-def", "c-d", SXML_COLLATION_MATCH_KIND_FIRST, true, 2,
     3},
    {UCA_URI "?alternate=blanked", "---", "-", SXML_COLLATION_MATCH_KIND_FIRST, true, 0, 0},
};

static bool matches_as_expected(const match_case *test_case) {
    sxml_counting_allocator *counting_allocator = NULL;
    if (sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                     .failing_allocation_number = 0},
            &counting_allocator) != SXML_STATUS_OK) {
        return false;
    }
    const sxml_collation collation = find_collation(test_case->collation_uri);
    sxml_collation_match match = {.is_found = false};
    bool is_expected =
        sxml_collation_find_match(
            &(sxml_collation_match_request){
                .collation = &collation,
                .allocator = sxml_counting_allocator_as_allocator(counting_allocator),
                .input = test_string(test_case->input_text),
                .search = test_string(test_case->search_text),
                .match_kind = test_case->match_kind},
            &match) == SXML_STATUS_OK &&
        match.is_found == test_case->is_found &&
        (!match.is_found || (match.byte_offset == test_case->byte_offset &&
                             match.byte_count == test_case->byte_count));
    is_expected = is_expected &&
                  sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count == 0;
    sxml_counting_allocator_destroy(counting_allocator);
    return is_expected;
}

static void test_matching(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(match_cases); case_index += 1) {
        const bool passed = matches_as_expected(&match_cases[case_index]);
        (void)sxml_test_record_check(state,
                                     (sxml_test_check){.passed = passed,
                                                       .expression =
                                                           match_cases[case_index].input_text,
                                                       .file_name = __FILE__,
                                                       .line_number = (uint32_t)__LINE__});
    }
}

/* How one run with a failing allocation ended. */
typedef struct failure_run {
    sxml_status status;
    bool has_no_leak;
} failure_run;

/* Every allocation of compare, make_key and find_match failing in turn
   gives OUT_OF_MEMORY and leaves nothing behind. */
static failure_run run_collation_with_failure(uint64_t failing_number) {
    sxml_counting_allocator *counting_allocator = NULL;
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number = failing_number},
        &counting_allocator);
    if (status != SXML_STATUS_OK) {
        return (failure_run){.status = status, .has_no_leak = true};
    }
    const sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting_allocator);
    const sxml_collation collation = find_collation(UCA_URI "?strength=identical;caseLevel=yes");
    /* Long enough for keys and unit arrays to grow. */
    const sxml_string long_text = test_string(
        "The quick brown fox jumps over the lazy dog; \xC3\x89TUDE \xC3\xA9tude \xEF\xAC\x81n "
        "the quick brown fox jumps over the lazy dog again and again and again.");
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    status = sxml_collation_compare(&(sxml_collation_comparison){.collation = &collation,
                                                                 .allocator = allocator,
                                                                 .first = long_text,
                                                                 .second = test_string("x")},
                                    &ordering);
    sxml_collation_key key = {.allocator = allocator, .bytes = NULL};
    if (status == SXML_STATUS_OK) {
        status = sxml_collation_make_key(
            &(sxml_collation_key_request){.collation = &collation, .input = long_text}, &key);
    }
    sxml_collation_key_release(&key);
    sxml_collation_match match = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_collation_find_match(
            &(sxml_collation_match_request){.collation = &collation,
                                            .allocator = allocator,
                                            .input = long_text,
                                            .search = test_string("again."),
                                            .match_kind = SXML_COLLATION_MATCH_KIND_LAST},
            &match);
    }
    const bool has_no_leak =
        sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count == 0;
    sxml_counting_allocator_destroy(counting_allocator);
    return (failure_run){.status = status, .has_no_leak = has_no_leak};
}

static void test_collation_survives_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 1000; failing_number += 1) {
        const failure_run run = run_collation_with_failure(failing_number);
        status = run.status;
        SXML_TEST_EXPECT(state, run.has_no_leak);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 4);
}

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    status = sxml_processor_create(
        allocator, &(sxml_processor_configuration){.allocator = allocator},
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

/* Destroys the fixture; true when nothing leaked. */
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

/* One query to run and what it gives. */
typedef struct query_run {
    sxml_byte_view expression_text;
    sxml_byte_view static_base_uri;
    /* Out: the error record of the failure. */
    sxml_error_record error_record;
} query_run;

/* Compiles and evaluates run->expression_text and writes the result by the
   adaptive method into the fixture's output buffer (cleared first). */
static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    sxml_output_buffer_clear(test_fixture->output_buffer);
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){
            .processor = test_fixture->processor,
            .expression_text = run->expression_text,
            .source_name = TEXT("collation.xpath"),
            .static_context = {.static_base_uri = run->static_base_uri},
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
    if (status == SXML_STATUS_OK) {
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
    }
    sxml_sequence_release(result);
    return status;
}

/* An expression and its adaptive output, or "!CODE" for the error code it
   raises. */
typedef struct expression_case {
    const char *expression_text;
    const char *expected_text;
} expression_case;

static bool evaluates_as_expected(const expression_case *test_case) {
    fixture test_fixture;
    sxml_status status = fixture_create(0, &test_fixture);
    query_run run = {.expression_text = sxml_byte_view_from_c_string(test_case->expression_text),
                     .static_base_uri =
                         TEXT("http://www.w3.org/2005/xpath-functions/collation/")};
    if (status == SXML_STATUS_OK) {
        status = run_query(&test_fixture, &run);
    }
    bool is_expected = false;
    if (test_case->expected_text[0] == '!') {
        is_expected = status != SXML_STATUS_OK &&
                      sxml_byte_view_equals(run.error_record.code.local_name,
                                            sxml_byte_view_from_c_string(test_case->expected_text + 1));
    } else {
        is_expected = status == SXML_STATUS_OK &&
                      sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                            sxml_byte_view_from_c_string(test_case->expected_text));
    }
    return fixture_destroy(&test_fixture) && is_expected;
}

static void expect_all(sxml_test_state *state, const expression_case *cases, size_t case_count) {
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        const bool passed = evaluates_as_expected(&cases[case_index]);
        (void)sxml_test_record_check(state,
                                     (sxml_test_check){.passed = passed,
                                                       .expression =
                                                           cases[case_index].expression_text,
                                                       .file_name = __FILE__,
                                                       .line_number = (uint32_t)__LINE__});
    }
}

/* ------------------------------------------------------------- Functions */

static void test_string_functions(sxml_test_state *state) {
    static const expression_case cases[] = {
        {"compare('a', 'A', '" HTML_URI "')", "0"},
        {"compare('b', 'A', '" UNICODE_URI "')", "1"},
        {"compare('database', 'DATABASE', '" UCA_URI "?strength=primary')", "0"},
        {"compare('a', 'b', 'http://example.com/unknown')", "!FOCH0002"},
        {"compare('a', 'b', '" UCA_URI "?fallback=no')", "!FOCH0002"},
        /* A relative URI against the static base URI (F&O 4.0, 5.2.7). */
        {"compare('a', 'A', 'html-ascii-case-insensitive')", "0"},
        {"contains('database', 'TABA', '" UCA_URI "?strength=secondary')", "true()"},
        {"starts-with('Database', 'data', '" HTML_URI "')", "true()"},
        {"ends-with('dataBASE', 'base', '" HTML_URI "')", "true()"},
        {"substring-before('xAbAc', 'ab', '" HTML_URI "')", "\"x\""},
        {"substring-after('xAbAbc', 'ab', '" HTML_URI "')", "\"Abc\""},
        {"substring-before-last('xAbAbc', 'ab', '" HTML_URI "')", "\"xAb\""},
        {"substring-after-last('xAbAbc', 'ab', '" HTML_URI "')", "\"c\""},
        {"contains-token(('red GREEN', 'blue'), ' green ', '" HTML_URI "')", "true()"},
        {"contains-token('red green', 'GREEN')", "false()"},
        {"collation-available('" HTML_URI "')", "true()"},
        {"collation-available('" UCA_URI "?fallback=no')", "false()"},
        {"collation-available('http://example.com/unknown')", "false()"},
        {"collation({})", "\"" UCA_URI "\""},
        {"collation({'lang': 'de', 'fallback': true()})", "\"" UCA_URI "?lang=de;fallback=yes\""},
        {"collation({1: 'x'})", "!XPTY0004"},
        {"let $c := collation({'strength': 'primary'}) return "
         "collation-key('A', $c) eq collation-key('a', $c)",
         "true()"},
        {"let $c := '" UCA_URI "?caseFirst=lower' return "
         "collation-key('abc', $c) lt collation-key('ABC', $c)",
         "true()"},
        {"collation-key('a') eq collation-key('A')", "false()"},
        {"default-collation()", "\"" CODEPOINT_URI "\""},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

static void test_sequence_functions(sxml_test_state *state) {
    static const expression_case cases[] = {
        {"index-of(('a', 'B', 'A'), 'b', '" HTML_URI "')", "2"},
        {"distinct-values(('a', 'B', 'A', 'b', 1), '" HTML_URI "')", "\"a\" \"B\" 1"},
        {"duplicate-values(('a', 'B', 'A', 'b', 'c'), '" HTML_URI "')", "\"A\" \"b\""},
        {"all-equal(('a', 'A'), '" HTML_URI "')", "true()"},
        {"all-different(('a', 'A'), '" HTML_URI "')", "false()"},
        {"sort(('b', 'C', 'a'), '" HTML_URI "')", "\"a\" \"b\" \"C\""},
        {"sort(('b', 'C', 'a'))", "\"C\" \"a\" \"b\""},
        {"distinct-values(('x', 'y'), 'http://example.com/unknown')", "!FOCH0002"},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

static void test_deep_equal_options(sxml_test_state *state) {
    static const expression_case cases[] = {
        {"deep-equal(('a', 'B'), ('A', 'b'), '" HTML_URI "')", "true()"},
        {"deep-equal(['a'], ['A'], {'collation': '" HTML_URI "'})", "true()"},
        {"deep-equal({'a': 1}, {'A': 1}, {'collation': '" HTML_URI "'})", "false()"},
        {"deep-equal('\xC3\x85', 'A\xCC\x8A', {'normalization-form': 'NFC'})", "true()"},
        {"deep-equal('\xC3\x85', 'A\xCC\x8A')", "false()"},
        {"deep-equal('a', 'a', {'normalization-form': 'NFX'})", "!XPTY0004"},
        {"deep-equal((1, 2, 3), (3, 1, 2), {'ordered': false()})", "true()"},
        {"deep-equal((1, 1, 2), (1, 2, 2), {'ordered': false()})", "false()"},
        {"deep-equal(([1], [2]), ([2], [1]), {'ordered': false()})", "true()"},
        {"deep-equal({'a': 1, 'b': ()}, {'a': 1}, {'ignore-empty-entries': true()})", "true()"},
        {"deep-equal({'a': 1, 'b': ()}, {'a': 1})", "false()"},
        {"deep-equal({'z': (), 'a': 1}, {'a': 1, 'y': ()}, "
         "{'ignore-empty-entries': true(), 'map-order': true()})",
         "true()"},
        {"deep-equal(xs:time('12:00:00+01:00'), xs:time('11:00:00Z'))", "true()"},
        {"deep-equal(xs:time('12:00:00+01:00'), xs:time('11:00:00Z'), {'timezones': true()})",
         "false()"},
        {"deep-equal(1, 1.0, {'type-annotations': true()})", "false()"},
        {"deep-equal(1 to 3, 11 to 13, {'items-equal': fn($a, $b) { $a mod 10 = $b mod 10 }})",
         "true()"},
        {"deep-equal((1, 2), (1, 2), {'items-equal': fn($a, $b) { if ($a = 2) { false() } }})",
         "false()"},
        {"deep-equal(1 to 4, reverse(11 to 14), "
         "{'items-equal': fn($a, $b) { $a mod 10 = $b mod 10 }, 'ordered': false()})",
         "true()"},
        {"deep-equal(1, 1, {'items-equal': fn($a, $b) { 1 }})", "!XPTY0004"},
        {"deep-equal(1, 1, {'unordered-elements': 'a'})", "!XPTY0004"},
        {"deep-equal(1, 1, {'comments': xs:untypedAtomic('true')})", "true()"},
        {"deep-equal(concat('a', ?), concat('b', ?))", "false()"},
        {"deep-equal(upper-case#1, upper-case#1)", "true()"},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

static void test_deep_equal_of_nodes(sxml_test_state *state) {
    static const expression_case cases[] = {
        {"deep-equal(parse-xml('<a><b/><c/></a>'), parse-xml('<a><c/><b/></a>'))", "false()"},
        {"deep-equal(parse-xml('<a><b/><c/></a>'), parse-xml('<a><c/><b/></a>'), "
         "{'unordered-elements': QName('', 'a')})",
         "true()"},
        {"deep-equal(parse-xml('<a>x<b/>y</a>'), parse-xml('<a>y<b/>x</a>'), "
         "{'unordered-elements': QName('', 'a')})",
         "true()"},
        {"deep-equal(parse-xml('<a><b>1</b><b>2</b></a>'), parse-xml('<a><b>2</b><b>3</b></a>'), "
         "{'unordered-elements': QName('', 'a')})",
         "false()"},
        {"deep-equal(parse-xml('<a xmlns=\"http://one/\"/>'), "
         "parse-xml('<p:a xmlns:p=\"http://one/\"/>'))",
         "true()"},
        {"deep-equal(parse-xml('<a xmlns=\"http://one/\"/>'), "
         "parse-xml('<p:a xmlns:p=\"http://one/\"/>'), {'namespace-prefixes': true()})",
         "false()"},
        {"deep-equal(parse-xml('<a/>'), parse-xml('<a xmlns:p=\"http://one/\"/>'), "
         "{'in-scope-namespaces': true()})",
         "false()"},
        {"deep-equal(parse-xml('<a xml:base=\"http://e.com/1\"><b/></a>')//b, "
         "parse-xml('<a xml:base=\"http://e.com/2\"><b/></a>')//b, {'base-uri': true()})",
         "false()"},
        {"deep-equal(parse-xml('<a> </a>'), parse-xml('<a/>'), {'whitespace': 'normalize'})",
         "true()"},
        {"deep-equal(parse-xml('<a xml:space=\"preserve\"><b> </b></a>'), "
         "parse-xml('<a xml:space=\"preserve\"><b/></a>'), {'whitespace': 'strip'})",
         "false()"},
        {"deep-equal(parse-xml('<a><!--x--></a>'), parse-xml('<a><!--X--></a>'), "
         "{'comments': true(), 'collation': '" HTML_URI "'})",
         "true()"},
        {"deep-equal(parse-xml('<A x=\"1\" z=\"2\"/>'), parse-xml('<A z=\"3\" x=\"1\"/>'), "
         "{'items-equal': fn($a, $b) { if (($a, $b) instance of attribute(z)+) { true() } }})",
         "true()"},
        {"deep-equal(parse-xml('<d><a>1</a><b>2</b></d>'), parse-xml('<d><b>12</b><a>11</a></d>'), "
         "{'items-equal': fn($a, $b) { if (($a, $b) instance of text()+) { $a mod 10 = $b mod 10 } "
         "}, 'unordered-elements': QName('', 'd')})",
         "true()"},
    };
    expect_all(state, cases, SXML_TEST_COUNT(cases));
}

/* Runs a deep-equal with a callback, unordered comparisons and a
   collation with allocation number failing_number failing. */
static sxml_status run_deep_equal_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(failing_number, &test_fixture);
    if (status == SXML_STATUS_OK) {
        query_run run = {
            .expression_text = TEXT(
                "deep-equal((parse-xml('<d><a>x</a>t<b/></d>'), 'A', [1], {'k': 'v'}), "
                "([1], 'a', {'k': 'V'}, parse-xml('<d><b/>t<a>X</a></d>')), "
                "{'ordered': false(), 'collation': '" HTML_URI "', "
                "'unordered-elements': QName('', 'd'), "
                "'items-equal': fn($a, $b) { if ($a instance of xs:integer) { $a = $b } }}), "
                "distinct-values(('a', 'A', 'b'), '" UCA_URI "?strength=primary'), "
                "substring-after('xAbAc', 'ab', '" UCA_URI "?strength=secondary')"),
            .static_base_uri = {.bytes = NULL, .byte_count = 0}};
        status = run_query(&test_fixture, &run);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                        sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                        TEXT("true() \"a\" \"b\" \"Ac\"")));
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_functions_survive_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 100000; failing_number += 1) {
        status = run_deep_equal_with_failure(state, failing_number);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case collation_test_cases[] = {
    {.name = "collation_uris", .function = test_collation_uris},
    {.name = "ordering", .function = test_ordering},
    {.name = "matching", .function = test_matching},
    {.name = "collation_survives_every_allocation_failure",
     .function = test_collation_survives_every_allocation_failure},
    {.name = "string_functions", .function = test_string_functions},
    {.name = "sequence_functions", .function = test_sequence_functions},
    {.name = "deep_equal_options", .function = test_deep_equal_options},
    {.name = "deep_equal_of_nodes", .function = test_deep_equal_of_nodes},
    {.name = "functions_survive_every_allocation_failure",
     .function = test_functions_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "collation",
            .cases = collation_test_cases,
            .case_count = SXML_TEST_COUNT(collation_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
