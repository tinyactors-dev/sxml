/*
 * Fuzz target: regular expressions (src/regex_internal.h; F&O 4.0, section
 * 6; packet 1.26).
 *
 * The input is a flags byte (bit 0 s, 1 m, 2 i, 3 x, 4 q, 5 c), then the
 * regex up to the first zero byte, then the string to search; both must
 * be well-formed UTF-8. The regex is compiled and every disjoint matching
 * segment of the string is found, as fn:tokenize and fn:analyze-string
 * do, with its captured groups, under small limits on steps and on the
 * backtrack stack. Every segment and group must lie within the string.
 *
 * Golden file: tests/regex_test.c (test_compile).
 */
#include "fuzz_fixture.h"
#include "regex_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SXML_FUZZ_MAXIMUM_INSTRUCTION_COUNT 20000U
#define SXML_FUZZ_MAXIMUM_STEP_COUNT ((uint64_t)1000000U)
#define SXML_FUZZ_MAXIMUM_BACKTRACK_BYTE_COUNT ((size_t)1 << 20U)
/* Matching segments looked at per input. */
#define SXML_FUZZ_MAXIMUM_SEGMENT_COUNT 1000U

static sxml_regex_flags flags_from_byte(uint8_t flags_byte) {
    return (sxml_regex_flags){
        .is_dot_all = (flags_byte & 1U) != 0,
        .is_multi_line = (flags_byte & 2U) != 0,
        .is_case_insensitive = (flags_byte & 4U) != 0,
        .is_whitespace_removed = (flags_byte & 8U) != 0,
        .is_literal = (flags_byte & 16U) != 0,
        .has_comments = (flags_byte & 32U) != 0};
}

/* Stops the process when range is not within a string of byte_count
   bytes. */
static void check_range(sxml_byte_range range, size_t byte_count) {
    if (range.byte_offset > byte_count ||
        range.byte_count > byte_count - range.byte_offset) {
        (void)fprintf(stderr, "range %zu+%zu outside %zu bytes\n",
                      range.byte_offset, range.byte_count, byte_count);
        abort();
    }
}

/* Reads every captured group of the current match. */
static void read_groups(const sxml_regex *regex,
                        const sxml_regex_matcher *matcher, size_t byte_count) {
    const uint32_t group_count = sxml_regex_group_count(regex);
    for (uint32_t group_number = 0; group_number <= group_count;
         group_number++) {
        sxml_regex_captured_group group = {.is_captured = false};
        const sxml_status status =
            sxml_regex_matcher_get_captured_group(matcher, group_number, &group);
        sxml_fuzz_check_status(status, "sxml_regex_matcher_get_captured_group");
        if (status == SXML_STATUS_OK && group.is_captured) {
            check_range(group.byte_range, byte_count);
        }
    }
}

/* Finds the matching segments of input. */
static void search(const sxml_regex *regex, sxml_regex_matcher *matcher,
                   sxml_string input) {
    sxml_regex_search_result result = {.is_found = false};
    sxml_status status = sxml_regex_matcher_match_at(
        matcher,
        &(sxml_regex_search_request){.regex_input = input,
                                     .start_byte_offset = 0},
        &result);
    sxml_fuzz_check_status(status, "sxml_regex_matcher_match_at");
    status = sxml_regex_matcher_find(
        matcher,
        &(sxml_regex_search_request){.regex_input = input,
                                     .start_byte_offset = 0},
        &result);
    sxml_fuzz_check_status(status, "sxml_regex_matcher_find");
    size_t previous_end = 0;
    for (uint32_t segment_number = 0;
         status == SXML_STATUS_OK && result.is_found &&
         segment_number < SXML_FUZZ_MAXIMUM_SEGMENT_COUNT;
         segment_number++) {
        check_range(result.matching_segment, input.utf8.byte_count);
        if (result.matching_segment.byte_offset < previous_end) {
            (void)fprintf(stderr, "segments overlap\n");
            abort();
        }
        previous_end = result.matching_segment.byte_offset +
                       result.matching_segment.byte_count;
        read_groups(regex, matcher, input.utf8.byte_count);
        status = sxml_regex_matcher_find_next(matcher, &result);
        sxml_fuzz_check_status(status, "sxml_regex_matcher_find_next");
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    if (size == 0) {
        return 0;
    }
    const uint8_t *const regex_start = data + 1;
    const size_t rest_byte_count = size - 1;
    const uint8_t *const separator = memchr(regex_start, 0, rest_byte_count);
    const size_t regex_byte_count = separator == NULL
                                        ? rest_byte_count
                                        : (size_t)(separator - regex_start);
    const size_t input_byte_count =
        separator == NULL ? 0 : rest_byte_count - regex_byte_count - 1;
    sxml_string regex_text = {.codepoint_count = 0};
    sxml_string input = {.utf8 = {.bytes = NULL, .byte_count = 0},
                         .codepoint_count = 0,
                         .is_ascii = true};
    if (sxml_string_from_utf8(sxml_fuzz_input_view(regex_start, regex_byte_count),
                              &regex_text) != SXML_STATUS_OK ||
        (separator != NULL &&
         sxml_string_from_utf8(
             sxml_fuzz_input_view(separator + 1, input_byte_count), &input) !=
             SXML_STATUS_OK)) {
        return 0;
    }
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(
            &(sxml_fuzz_fixture_configuration){.has_function_library = false},
            &fixture) != SXML_STATUS_OK) {
        return 0;
    }
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(&fixture);
    sxml_error_record error_record = {.value = NULL};
    sxml_regex *regex = NULL;
    sxml_status status = sxml_regex_create(
        allocator,
        &(sxml_regex_configuration){
            .regex_text = regex_text,
            .flags = flags_from_byte(data[0]),
            .maximum_instruction_count = SXML_FUZZ_MAXIMUM_INSTRUCTION_COUNT,
            .maximum_group_depth = 0,
            .stack_allocator = {.state = NULL, .operations = NULL},
            .error_record = &error_record},
        &regex);
    sxml_fuzz_check_status(status, "sxml_regex_create");
    sxml_regex_matcher *matcher = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_matcher_create(
            allocator,
            &(sxml_regex_matcher_configuration){
                .regex = regex,
                .backtrack_allocator = allocator,
                .initial_backtrack_entry_count = 0,
                .maximum_backtrack_byte_count =
                    SXML_FUZZ_MAXIMUM_BACKTRACK_BYTE_COUNT,
                .maximum_step_count = SXML_FUZZ_MAXIMUM_STEP_COUNT},
            &matcher);
        sxml_fuzz_check_status(status, "sxml_regex_matcher_create");
    }
    if (status == SXML_STATUS_OK) {
        search(regex, matcher, input);
    }
    sxml_regex_matcher_destroy(matcher);
    sxml_regex_destroy(regex);
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
