/*
 * Tests for src/regex_internal.h: flags, compilation and its errors, the
 * lifecycle of compiled regexes and matchers, matching, the disjoint
 * matching segments behind fn:tokenize, and limits. The fn:tokenize cases
 * of the torture slice (fn-tokenize-1a, fn-tokenize-35) are run here at
 * the level of matching segments; the function itself is packet 1.20.
 */
#include "regex_internal.h"
#include "test_runner.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- Helpers */

static sxml_string test_string(const char *text) {
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                          .codepoint_count = 0,
                          .is_ascii = true};
    /* Test texts are well-formed UTF-8; a failure leaves the empty string
       and the test's expectations fail. */
    if (sxml_string_from_utf8(sxml_byte_view_from_c_string(text), &string) !=
        SXML_STATUS_OK) {
        string.codepoint_count = 0;
    }
    return string;
}

static bool test_view_equals_text(sxml_byte_view view, const char *text) {
    return sxml_byte_view_equals(view, sxml_byte_view_from_c_string(text));
}

/* A regex compiled with its matcher, from texts, through a counting
   allocator so leaks show. */
typedef struct test_compilation {
    const char *regex_text;
    const char *flags_text;
    /* Optional limits; 0 means the defaults. */
    uint32_t maximum_instruction_count;
    uint64_t maximum_step_count;
    /* The XSD dialect of pattern facets instead of flags_text's flags. */
    bool is_xsd_pattern;
    sxml_status status;
    sxml_error_record error_record;
    sxml_counting_allocator *counting_allocator;
    sxml_regex *regex;
    sxml_regex_matcher *matcher;
} test_compilation;

static void test_compile(test_compilation *compilation) {
    compilation->regex = NULL;
    compilation->matcher = NULL;
    compilation->error_record = (sxml_error_record){.value = NULL};
    compilation->status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = 0,
        },
        &compilation->counting_allocator);
    if (compilation->status != SXML_STATUS_OK) {
        return;
    }
    sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(compilation->counting_allocator);
    sxml_regex_flags flags = {.is_dot_all = false};
    compilation->status = sxml_regex_parse_flags(
        &(sxml_regex_flags_request){
            .flags_text = test_string(compilation->flags_text),
            .error_record = &compilation->error_record,
        },
        &flags);
    if (compilation->status != SXML_STATUS_OK) {
        return;
    }
    flags.is_xsd_pattern = compilation->is_xsd_pattern;
    compilation->status = sxml_regex_create(
        allocator,
        &(sxml_regex_configuration){
            .regex_text = test_string(compilation->regex_text),
            .flags = flags,
            .maximum_instruction_count = compilation->maximum_instruction_count,
            .maximum_group_depth = 0,
            .error_record = &compilation->error_record,
        },
        &compilation->regex);
    if (compilation->status != SXML_STATUS_OK) {
        return;
    }
    compilation->status = sxml_regex_matcher_create(
        allocator,
        &(sxml_regex_matcher_configuration){
            .regex = compilation->regex,
            .backtrack_allocator = allocator,
            .initial_backtrack_entry_count = 0,
            .maximum_backtrack_byte_count = 0,
            .maximum_step_count = compilation->maximum_step_count,
        },
        &compilation->matcher);
}

/* Releases what test_compile made; true when nothing leaked. */
static bool test_release(test_compilation *compilation) {
    sxml_regex_matcher_destroy(compilation->matcher);
    sxml_regex_destroy(compilation->regex);
    if (compilation->counting_allocator == NULL) {
        return true;
    }
    sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(compilation->counting_allocator);
    sxml_counting_allocator_destroy(compilation->counting_allocator);
    compilation->counting_allocator = NULL;
    return counts.live_allocation_count == 0;
}

/* Checks that compiling regex_text with flags_text raises error_name. */
static bool test_raises(const char *regex_text, const char *flags_text,
                        const char *error_name) {
    test_compilation compilation = {.regex_text = regex_text,
                                    .flags_text = flags_text};
    test_compile(&compilation);
    bool is_raised =
        compilation.status == SXML_STATUS_DYNAMIC_ERROR &&
        test_view_equals_text(compilation.error_record.code.local_name,
                              error_name) &&
        test_view_equals_text(compilation.error_record.code.namespace_uri,
                              SXML_ERROR_NAMESPACE_URI) &&
        compilation.error_record.diagnostic.description.byte_count != 0;
    return test_release(&compilation) && is_raised;
}

/* Text written by the search helpers, for comparison with expectations. */
typedef struct test_text {
    char characters[256];
    size_t character_count;
} test_text;

static void test_text_append(test_text *text, sxml_byte_view bytes) {
    if (bytes.byte_count >= sizeof(text->characters) - text->character_count) {
        return;
    }
    if (bytes.byte_count != 0) {
        memcpy(text->characters + text->character_count, bytes.bytes,
               bytes.byte_count);
    }
    text->character_count += bytes.byte_count;
    text->characters[text->character_count] = '\0';
}

static sxml_byte_view test_slice(sxml_string input, sxml_byte_range range) {
    return (sxml_byte_view){
        .bytes = range.byte_count == 0 ? NULL : input.utf8.bytes + range.byte_offset,
        .byte_count = range.byte_count,
    };
}

/* A search to run: regex, flags, input. */
typedef struct test_search {
    const char *regex_text;
    const char *flags_text;
    const char *input_text;
} test_search;

/*
 * Writes "[match]" for the first matching segment and "<group>" for each
 * captured group, "<->" when a group captured nothing; "none" when nothing
 * matches; "error" when compiling or searching fails.
 */
static void test_first_match(test_search search, test_text *result) {
    *result = (test_text){.character_count = 0};
    result->characters[0] = '\0';
    test_compilation compilation = {.regex_text = search.regex_text,
                                    .flags_text = search.flags_text};
    test_compile(&compilation);
    sxml_string input = test_string(search.input_text);
    sxml_regex_search_result search_result = {.is_found = false};
    if (compilation.status == SXML_STATUS_OK) {
        compilation.status = sxml_regex_matcher_find(
            compilation.matcher,
            &(sxml_regex_search_request){.regex_input = input,
                                         .start_byte_offset = 0},
            &search_result);
    }
    if (compilation.status != SXML_STATUS_OK) {
        test_text_append(result, SXML_BYTE_VIEW_FROM_LITERAL("error"));
    } else if (!search_result.is_found) {
        test_text_append(result, SXML_BYTE_VIEW_FROM_LITERAL("none"));
    } else {
        for (uint32_t group_number = 0;
             group_number <= sxml_regex_group_count(compilation.regex);
             group_number++) {
            sxml_regex_captured_group group = {.is_captured = false};
            if (sxml_regex_matcher_get_captured_group(
                    compilation.matcher, group_number, &group) !=
                SXML_STATUS_OK) {
                group.is_captured = false;
            }
            test_text_append(result, group_number == 0
                                         ? SXML_BYTE_VIEW_FROM_LITERAL("[")
                                         : SXML_BYTE_VIEW_FROM_LITERAL("<"));
            test_text_append(result, group.is_captured
                                         ? test_slice(input, group.byte_range)
                                         : SXML_BYTE_VIEW_FROM_LITERAL("-"));
            test_text_append(result, group_number == 0
                                         ? SXML_BYTE_VIEW_FROM_LITERAL("]")
                                         : SXML_BYTE_VIEW_FROM_LITERAL(">"));
        }
    }
    if (!test_release(&compilation)) {
        test_text_append(result, SXML_BYTE_VIEW_FROM_LITERAL(" leaked"));
    }
}

static bool test_first_match_is(test_search search, const char *expected) {
    test_text result = {.character_count = 0};
    test_first_match(search, &result);
    return strcmp(result.characters, expected) == 0;
}

#define TEST_MAXIMUM_SEGMENT_COUNT 64

/*
 * fn:tokenize (F&O 4.0, section 6.3.3) on the disjoint matching segments:
 * prepend a zero-length segment at the start unless the first segment is
 * one, append one at the end unless the last segment is one, and return
 * what lies between adjacent segments. Writes each token as "[token]".
 */
static void test_tokenize(test_search search, test_text *result) {
    *result = (test_text){.character_count = 0};
    result->characters[0] = '\0';
    test_compilation compilation = {.regex_text = search.regex_text,
                                    .flags_text = search.flags_text};
    test_compile(&compilation);
    sxml_string input = test_string(search.input_text);
    sxml_byte_range segments[TEST_MAXIMUM_SEGMENT_COUNT + 2];
    size_t segment_count = 0;
    segments[segment_count++] =
        (sxml_byte_range){.byte_offset = 0, .byte_count = 0};
    sxml_regex_search_result search_result = {.is_found = false};
    if (compilation.status == SXML_STATUS_OK) {
        compilation.status = sxml_regex_matcher_find(
            compilation.matcher,
            &(sxml_regex_search_request){.regex_input = input,
                                         .start_byte_offset = 0},
            &search_result);
    }
    while (compilation.status == SXML_STATUS_OK && search_result.is_found &&
           segment_count <= TEST_MAXIMUM_SEGMENT_COUNT) {
        sxml_regex_captured_group group = {.is_captured = false};
        if (sxml_regex_matcher_get_captured_group(compilation.matcher, 0,
                                                  &group) != SXML_STATUS_OK) {
            break;
        }
        if (segment_count == 1 && group.byte_range.byte_offset == 0 &&
            group.byte_range.byte_count == 0) {
            /* The first segment is zero-length at the start: no prepended
               segment. */
            segment_count = 0;
        }
        segments[segment_count++] = group.byte_range;
        compilation.status =
            sxml_regex_matcher_find_next(compilation.matcher, &search_result);
    }
    sxml_byte_range last_segment = segments[segment_count - 1];
    if (last_segment.byte_count != 0 ||
        last_segment.byte_offset != input.utf8.byte_count) {
        segments[segment_count++] = (sxml_byte_range){
            .byte_offset = input.utf8.byte_count, .byte_count = 0};
    }
    if (compilation.status != SXML_STATUS_OK) {
        test_text_append(result, SXML_BYTE_VIEW_FROM_LITERAL("error"));
    } else {
        for (size_t segment_index = 1; segment_index < segment_count;
             segment_index++) {
            size_t token_start = segments[segment_index - 1].byte_offset +
                                 segments[segment_index - 1].byte_count;
            sxml_byte_range token = {
                .byte_offset = token_start,
                .byte_count = segments[segment_index].byte_offset - token_start,
            };
            test_text_append(result, SXML_BYTE_VIEW_FROM_LITERAL("["));
            test_text_append(result, test_slice(input, token));
            test_text_append(result, SXML_BYTE_VIEW_FROM_LITERAL("]"));
        }
    }
    if (!test_release(&compilation)) {
        test_text_append(result, SXML_BYTE_VIEW_FROM_LITERAL(" leaked"));
    }
}

static bool test_tokenize_is(test_search search, const char *expected) {
    test_text result = {.character_count = 0};
    test_tokenize(search, &result);
    return strcmp(result.characters, expected) == 0;
}

/* ------------------------------------------------------------------ Flags */

static void test_flags_are_read(sxml_test_state *state) {
    sxml_regex_flags flags = {.is_dot_all = false};
    sxml_status status = sxml_regex_parse_flags(
        &(sxml_regex_flags_request){.flags_text = test_string("cqxims"),
                                    .error_record = NULL},
        &flags);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, flags.is_dot_all && flags.is_multi_line &&
                                flags.is_case_insensitive &&
                                flags.is_whitespace_removed &&
                                flags.is_literal && flags.has_comments);
    status = sxml_regex_parse_flags(
        &(sxml_regex_flags_request){.flags_text = test_string("ss"),
                                    .error_record = NULL},
        &flags);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && flags.is_dot_all &&
                                !flags.is_multi_line);
}

static void test_unknown_flag_raises_forx0001(sxml_test_state *state) {
    /* fn-tokenize-2 uses the flag "t". */
    SXML_TEST_EXPECT(state, test_raises("\\s+", "t", "FORX0001"));
    SXML_TEST_EXPECT(state, test_raises("a", "S", "FORX0001"));
    sxml_regex_flags flags = {.is_dot_all = true};
    sxml_status status = sxml_regex_parse_flags(
        &(sxml_regex_flags_request){.flags_text = test_string("j"),
                                    .error_record = NULL},
        &flags);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, flags.is_dot_all);
}

/* --------------------------------------------------------------- Lifecycle */

static void test_regex_initializes_in_caller_memory(sxml_test_state *state) {
    sxml_regex_configuration configuration = {
        .regex_text = test_string("(a|b)*c[d-f]"),
        .flags = {.is_dot_all = false},
        .maximum_instruction_count = 0,
        .maximum_group_depth = 0,
        .error_record = NULL,
    };
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_REQUIRE(state, sxml_regex_memory_requirements(
                                 &configuration, &requirements) ==
                                 SXML_STATUS_OK);
    alignas(16) uint8_t storage[1024];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(storage) &&
                                 requirements.alignment <= 16);
    sxml_regex *regex = NULL;
    sxml_status status = sxml_regex_initialize(
        (sxml_memory_view){.bytes = storage,
                           .byte_count = requirements.byte_count - 1},
        &configuration, &regex);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, regex == NULL);
    if (requirements.alignment > 1) {
        status = sxml_regex_initialize(
            (sxml_memory_view){.bytes = storage + 1,
                               .byte_count = sizeof(storage) - 1},
            &configuration, &regex);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_MISALIGNED_MEMORY);
    }
    status = sxml_regex_initialize(
        (sxml_memory_view){.bytes = storage,
                           .byte_count = requirements.byte_count},
        &configuration, &regex);
    SXML_TEST_REQUIRE(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_regex_group_count(regex) == 1);

    sxml_regex_matcher_configuration matcher_configuration = {
        .regex = regex,
        .backtrack_allocator = {.state = NULL, .operations = NULL},
        .initial_backtrack_entry_count = 64,
        .maximum_backtrack_byte_count = 0,
        .maximum_step_count = 0,
    };
    SXML_TEST_REQUIRE(state, sxml_regex_matcher_memory_requirements(
                                 &matcher_configuration, &requirements) ==
                                 SXML_STATUS_OK);
    alignas(16) uint8_t matcher_storage[4096];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(matcher_storage));
    sxml_regex_matcher *matcher = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_regex_matcher_initialize(
                   (sxml_memory_view){.bytes = matcher_storage,
                                      .byte_count = sizeof(matcher_storage)},
                   &matcher_configuration, &matcher) == SXML_STATUS_OK);
    sxml_regex_search_result result = {.is_found = false};
    sxml_string input = test_string("xxabacd");
    status = sxml_regex_matcher_find(
        matcher,
        &(sxml_regex_search_request){.regex_input = input,
                                     .start_byte_offset = 0},
        &result);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && result.is_found);
    sxml_regex_captured_group group = {.is_captured = false};
    SXML_TEST_EXPECT(state, sxml_regex_matcher_get_captured_group(
                                matcher, 0, &group) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, group.is_captured &&
                                group.byte_range.byte_offset == 2 &&
                                group.byte_range.byte_count == 5);
    SXML_TEST_EXPECT(state, sxml_regex_matcher_get_captured_group(
                                matcher, 2, &group) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    sxml_regex_matcher_finalize(matcher);
    sxml_regex_finalize(regex);
    sxml_regex_matcher_finalize(NULL);
    sxml_regex_finalize(NULL);
    sxml_regex_matcher_destroy(NULL);
    sxml_regex_destroy(NULL);
}

static void test_regex_rejects_too_large_instruction_limit(
    sxml_test_state *state) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_regex_memory_requirements(
        &(sxml_regex_configuration){
            .regex_text = test_string("a"),
            .maximum_instruction_count =
                SXML_REGEX_MAXIMUM_INSTRUCTION_COUNT_LIMIT + 1,
        },
        &requirements);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_ARGUMENT);
    status = sxml_regex_matcher_memory_requirements(
        &(sxml_regex_matcher_configuration){.regex = NULL}, &requirements);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_ARGUMENT);
}

/* Compiles, grows the backtrack stack and searches with every allocation
   failing in turn (D23). */
static void test_allocation_failures_leave_nothing_behind(
    sxml_test_state *state) {
    sxml_string input = test_string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab");
    for (uint64_t failing_allocation_number = 1;; failing_allocation_number++) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(
            state, sxml_counting_allocator_create(
                       sxml_default_allocator(),
                       &(sxml_counting_allocator_configuration){
                           .backing_allocator = sxml_default_allocator(),
                           .failing_allocation_number = failing_allocation_number,
                       },
                       &counting_allocator) == SXML_STATUS_OK);
        sxml_allocator allocator =
            sxml_counting_allocator_as_allocator(counting_allocator);
        sxml_regex *regex = NULL;
        sxml_regex_matcher *matcher = NULL;
        sxml_regex_search_result result = {.is_found = false};
        sxml_status status = sxml_regex_create(
            allocator,
            &(sxml_regex_configuration){.regex_text = test_string("(a|b)*b")},
            &regex);
        if (status == SXML_STATUS_OK) {
            status = sxml_regex_matcher_create(
                allocator,
                &(sxml_regex_matcher_configuration){
                    .regex = regex,
                    .backtrack_allocator = allocator,
                    .initial_backtrack_entry_count = 2,
                },
                &matcher);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_regex_matcher_find(
                matcher,
                &(sxml_regex_search_request){.regex_input = input,
                                             .start_byte_offset = 0},
                &result);
        }
        sxml_regex_matcher_destroy(matcher);
        sxml_regex_destroy(regex);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_REQUIRE(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, result.is_found);
            SXML_TEST_EXPECT(state, failing_allocation_number > 3);
            break;
        }
        SXML_TEST_REQUIRE(state, status == SXML_STATUS_OUT_OF_MEMORY);
    }
}

/* ---------------------------------------------------------------- Matching */

static void test_literals_classes_and_escapes(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"cat", "", "concatenate"}, "[cat]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"dog", "", "concatenate"}, "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[0-9]+", "", "abc123def"}, "[123]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[^a-c]+", "", "abcxyzabc"}, "[xyz]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[a-z-[aeiou]]+", "", "aebcdo"},
                                "[bcd]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[-a]+", "", "x-a-y"}, "[-a-]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[a-]+", "", "x-a-y"}, "[-a-]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\$\\.\\[\\]\\{\\}", "", "a$.[]{}b"},
                                "[$.[]{}]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\s+", "", "a \t\nb"}, "[ \t\n]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\S+", "", "  ab  "}, "[ab]"));
    /* \d is \p{Nd}: ARABIC-INDIC DIGIT THREE matches. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\d+", "", "x1\xD9\xA3" "2y"},
                                "[1\xD9\xA3" "2]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\D", "", "12a"}, "[a]"));
    /* \w excludes punctuation, separators and other characters. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\w+", "", "!?caf\xC3\xA9 x"},
                                "[caf\xC3\xA9]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\W+", "", "ab, cd"}, "[, ]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[\\d\\s]+", "", "a1 2b"}, "[1 2]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\xC3\xA9+", "", "x\xC3\xA9\xC3\xA9"},
                                "[\xC3\xA9\xC3\xA9]"));
}

static void test_dot_and_the_s_flag(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a.c", "", "a\nc a\rc abc"}, "[abc]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a.c", "s", "a\nc"}, "[a\nc]"));
}

static void test_quantifiers_greedy_and_reluctant(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a+", "", "baaa"}, "[aaa]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a+?", "", "baaa"}, "[a]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a*?b", "", "aab"}, "[aab]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a??", "", "a"}, "[]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a{2,3}", "", "aaaa"}, "[aaa]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a{2,3}?", "", "aaaa"}, "[aa]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a{2}", "", "a aa"}, "[aa]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a{2,}", "", "aaaaa"}, "[aaaaa]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"<.*>", "", "<a><b>"}, "[<a><b>]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"<.*?>", "", "<a><b>"}, "[<a>]"));
    /* An iteration that matches nothing ends the loop, and its captures
       are undone (which substring is captured here is
       implementation-dependent, F&O 4.0, section 6.1.5). */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(a*)*", "", "b"}, "[]<->"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(a*)+b", "", "aab"}, "[aab]<aa>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?:a|b)*c", "", "ababc"}, "[ababc]"));
}

static void test_groups_and_alternation(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(\\d+)-(\\d+)", "", "x10-20"},
                                "[10-20]<10><20>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(a)|b", "", "b"}, "[b]<->"));
    /* The first matching branch is chosen (F&O 4.0, section 6.1.1). */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a|ab", "", "ab"}, "[a]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"x(a|b|c)y", "", "xcy"}, "[xcy]<c>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?:(a)|b)c", "", "bc"}, "[bc]<->"));
    /* The last iteration's capture is kept. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(\\w)+", "", "abc"}, "[abc]<c>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a()b", "", "ab"}, "[ab]<>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"", "", "ab"}, "[]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a|", "", "b"}, "[]"));
}

static void test_back_references(sxml_test_state *state) {
    /* F&O 4.0, section 6.1.6. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"('|\").*\\1", "", "x'a\"b'y"},
                                "['a\"b']<'>"));
    /* \10 is group 10 only when ten groups precede it. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(a)\\10", "", "aa0"}, "[aa0]<a>"));
    SXML_TEST_EXPECT(
        state, test_first_match_is(
                   (test_search){"(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\10", "",
                                 "abcdefghijj"},
                   "[abcdefghijj]<a><b><c><d><e><f><g><h><i><j>"));
    /* A group that captured nothing matches the empty string. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(a){0}b\\1", "", "b"}, "[b]<->"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(a)\\1", "", "ab"}, "none"));
}

static void test_anchors_and_the_m_flag(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"^b", "", "ab"}, "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"^a|b$", "", "ab"}, "[a]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"^b", "m", "a\nb"}, "[b]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a$", "m", "a\nb"}, "[a]"));
    /* F&O 4.0, section 6.1.8.1 examples: no line after a final newline. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"^$", "m", "a\n"}, "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\n$", "m", "a\n"}, "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"^$", "", ""}, "[]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a$", "", "a\n"}, "none"));
}

static void test_case_insensitive_matching(sxml_test_state *state) {
    /* F&O 4.0, section 6.2, flag i. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"z", "i", "Z"}, "[Z]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[A-Z]+", "i", "1aBc2"}, "[aBc]"));
    /* KELVIN SIGN is a case variant of k. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[A-Z]", "i", "\xE2\x84\xAA"},
                                "[\xE2\x84\xAA]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"s", "i", "\xC5\xBF"}, "[\xC5\xBF]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[^Q]", "i", "qQa"}, "[a]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[A-Z-[IO]]+", "i", "ioab"}, "[ab]"));
    /* Other constructs are unaffected. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"B. OBAMA", "iq", "Mr. B. Obama"},
                                "[B. Obama]"));
}

static void test_whitespace_comments_and_literal_flags(sxml_test_state *state) {
    /* F&O 4.0, section 6.2, flag x examples. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"hello world", "x", "helloworld"},
                                "[helloworld]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"hello[ ]world", "x", "helloworld"},
                                "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"hello\\ sworld", "x", "hello world"},
                                "[hello world]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"hello world", "x", "hello world"},
                                "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a { 2 , 3 }", "x", "aaaa"}, "[aaa]"));
    /* Flag c: comments run from # to # (section 6.1.2). */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(..#month#)/(..#day#)", "c", "03/24"},
                                "[03/24]<03><24>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a\\#b", "c", "a#b"}, "[a#b]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a[#]b#comment", "c", "a#b"}, "[a#b]"));
    /* Flag q. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){".*", "q", "abcd"}, "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a.*(", "q", "xa.*("}, "[a.*(]"));
}

/* ---------------------------------------------------- Disjoint segments */

static void test_tokenize_slice_cases(sxml_test_state *state) {
    /* fn-tokenize-1a: the regex may match the empty string (4.0). */
    SXML_TEST_EXPECT(state, test_tokenize_is((test_search){".?", "", "abba"},
                                             "[][][][][]"));
    /* fn-tokenize-35: "." matches CR in dot-all mode. */
    SXML_TEST_EXPECT(state, test_tokenize_is(
                                (test_search){"y.J", "s", "Mary\rJones"},
                                "[Mar][ones]"));
    SXML_TEST_EXPECT(state, test_tokenize_is(
                                (test_search){"y.J", "", "Mary\rJones"},
                                "[Mary\rJones]"));
}

static void test_tokenize_specification_examples(sxml_test_state *state) {
    /* F&O 4.0, section 6.3.3, examples. */
    SXML_TEST_EXPECT(state, test_tokenize_is(
                                (test_search){"\\s+", "", "The cat sat on the mat"},
                                "[The][cat][sat][on][the][mat]"));
    SXML_TEST_EXPECT(state, test_tokenize_is(
                                (test_search){",\\s*", "", "1, 15, 24, 50"},
                                "[1][15][24][50]"));
    SXML_TEST_EXPECT(state, test_tokenize_is(
                                (test_search){",", "", "1,15,,24,50,"},
                                "[1][15][][24][50][]"));
    SXML_TEST_EXPECT(state,
                     test_tokenize_is(
                         (test_search){"\\s*<br>\\s*", "i",
                                       "Some unparsed <br> HTML <BR> text"},
                         "[Some unparsed][HTML][text]"));
    SXML_TEST_EXPECT(state, test_tokenize_is(
                                (test_search){".", "q", "12.3.5.6"},
                                "[12][3][5][6]"));
    /* A zero-length match at the start is not preceded by an empty token. */
    SXML_TEST_EXPECT(state, test_tokenize_is((test_search){"", "", "abc"},
                                             "[a][b][c]"));
    SXML_TEST_EXPECT(state, test_tokenize_is((test_search){"b*", "", "abba"},
                                             "[a][][a]"));
}

static void test_search_positions(sxml_test_state *state) {
    test_compilation compilation = {.regex_text = "b", .flags_text = ""};
    test_compile(&compilation);
    SXML_TEST_REQUIRE(state, compilation.status == SXML_STATUS_OK);
    sxml_regex_search_result result = {.is_found = true};
    SXML_TEST_EXPECT(state, sxml_regex_matcher_find_next(compilation.matcher,
                                                         &result) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_regex_captured_group group = {.is_captured = false};
    SXML_TEST_EXPECT(state, sxml_regex_matcher_get_captured_group(
                                compilation.matcher, 0, &group) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_string input = test_string("\xC3\xA9" "ab");
    /* Not a codepoint boundary. */
    SXML_TEST_EXPECT(state,
                     sxml_regex_matcher_find(
                         compilation.matcher,
                         &(sxml_regex_search_request){.regex_input = input,
                                                      .start_byte_offset = 1},
                         &result) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_regex_matcher_find(
                         compilation.matcher,
                         &(sxml_regex_search_request){.regex_input = input,
                                                      .start_byte_offset = 5},
                         &result) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_regex_search_result match_result = {.is_found = true};
    SXML_TEST_EXPECT(state,
                     sxml_regex_matcher_match_at(
                         compilation.matcher,
                         &(sxml_regex_search_request){.regex_input = input,
                                                      .start_byte_offset = 2},
                         &match_result) == SXML_STATUS_OK &&
                         !match_result.is_found);
    SXML_TEST_EXPECT(state,
                     sxml_regex_matcher_match_at(
                         compilation.matcher,
                         &(sxml_regex_search_request){.regex_input = input,
                                                      .start_byte_offset = 3},
                         &match_result) == SXML_STATUS_OK &&
                         match_result.is_found);
    SXML_TEST_EXPECT(state, sxml_regex_matcher_find_next(compilation.matcher,
                                                         &result) ==
                                    SXML_STATUS_OK &&
                                !result.is_found);
    SXML_TEST_EXPECT(state, test_release(&compilation));
}

/* ----------------------------------------------------------------- Errors */

static void test_invalid_regexes_raise_forx0002(sxml_test_state *state) {
    static const char *const invalid_regexes[] = {
        "(", ")", "a)", "(a", "a**", "*a", "+", "?", "{", "}", "]", "a{",
        "a{,2}", "a{3,2}", "a{2", "a{x}", "[", "[a", "[]", "[^]", "[a-", "[z-a]",
        "[a[b]]", "[\\d-z]", "[a-\\d]", "\\", "\\0", "\\x", "\\1",
        "(a\\1)", "\\1(a)", "^*", "$+", "a|*", "(?", "(?x)", "[a-[b]c]",
        "a{2}{3}", "[\\1]",
    };
    for (size_t regex_index = 0;
         regex_index < SXML_TEST_COUNT(invalid_regexes); regex_index++) {
        bool is_raised = test_raises(invalid_regexes[regex_index], "", "FORX0002");
        SXML_TEST_EXPECT(state, is_raised);
        if (!is_raised) {
            /* Names the regex in the failure report. */
            SXML_TEST_EXPECT(state, invalid_regexes[regex_index] == NULL);
        }
    }
}

static void test_invalid_assertions_and_escapes_raise_forx0002(
    sxml_test_state *state) {
    static const char *const invalid_regexes[] = {
        /* Category and block escapes (XSD 1.1 Part 2, appendix G; F&O 4.0,
           section 6.1.7). */
        "\\p{Cs}", "\\p{Lx}", "\\p{IsNoSuchBlock}", "\\p{Is}", "\\p", "\\p{L",
        "\\p{IsaA0-a9}", "[\\p{Lu}-z]",
        /* Assertions take no quantifier (section 6.1.8). */
        "(?=a)*", "(?!a)+", "\\b?", "\\B{2}", "(?<=a){1}",
        /* Lookbehind assertions hold single characters (6.1.8.5). */
        "(?<=a*)b", "(?<=(a))b", "(?<=a{2})b", "(?<=\\1)b", "(?<=^a)b",
        "(?<=\\b)a", "(?<=(?:a))b",
        /* Named groups and \k (sections 6.1.5 and 6.1.6). */
        "(?<1a>x)", "(?<>x)", "(?<a-b>x)", "(?<a>x)(?<a>y)", "\\k<b>",
        "(?<a>x)\\k<a", "\\k<a>(?<a>x)", "(?<a>x\\k<a>)", "\\k",
        /* Unknown "(*" and "(?" constructs. */
        "(*foo:a)", "(*positive_lookahead", "(?<", "(?>a)",
        /* \b is no escape in a class. */
        "[\\b]", "[\\k]",
    };
    for (size_t regex_index = 0;
         regex_index < SXML_TEST_COUNT(invalid_regexes); regex_index++) {
        bool is_raised = test_raises(invalid_regexes[regex_index], "", "FORX0002");
        SXML_TEST_EXPECT(state, is_raised);
        if (!is_raised) {
            SXML_TEST_EXPECT(state, invalid_regexes[regex_index] == NULL);
        }
    }
}

/* ------------------------------------------------------------ XSD dialect */

/* Whether the XSD pattern regex_text matches all of input; false also
   when it does not compile or leaks. */
typedef struct test_xsd_case {
    const char *regex_text;
    const char *input;
    bool is_match;
} test_xsd_case;

static bool test_xsd_pattern_matches(test_xsd_case xsd_case) {
    test_compilation compilation = {.regex_text = xsd_case.regex_text,
                                    .flags_text = "",
                                    .is_xsd_pattern = true};
    test_compile(&compilation);
    sxml_regex_search_result result = {.is_found = false};
    bool is_run = compilation.status == SXML_STATUS_OK &&
                  sxml_regex_matcher_match_at(
                      compilation.matcher,
                      &(sxml_regex_search_request){
                          .regex_input = test_string(xsd_case.input),
                          .start_byte_offset = 0},
                      &result) == SXML_STATUS_OK;
    return test_release(&compilation) && is_run &&
           result.is_found == xsd_case.is_match;
}

/* XSD 1.1 Part 2, appendix G: anchored at both ends, "^" and "$" plain
   characters. */
static void test_xsd_patterns_match_the_whole_value(sxml_test_state *state) {
    static const test_xsd_case cases[] = {
        {"a|ab", "ab", true},     {"a|ab", "a", true},
        {"a|ab", "abc", false},   {"a*", "", true},
        {"\\d{3}-[A-Z]{2}", "123-AB", true},
        {"\\d{3}-[A-Z]{2}", "x123-AB", false},
        {"\\d{3}-[A-Z]{2}", "123-ABC", false},
        {"^a$", "^a$", true},     {"^a$", "a", false},
        {"[^a]b", "cb", true},    {"(ab)+", "abab", true},
        {"(ab)+", "aba", false},  {"\\p{Lu}\\c*", "Ab-c", true},
        {".", "\n", false},      {"x{2,3}", "xxxx", false},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index++) {
        bool is_expected = test_xsd_pattern_matches(cases[case_index]);
        SXML_TEST_EXPECT(state, is_expected);
        if (!is_expected) {
            SXML_TEST_EXPECT(state, cases[case_index].regex_text == NULL);
        }
    }
}

/* What the XSD grammar lacks is FORX0002 in the dialect. */
static void test_xsd_patterns_reject_fo_extensions(sxml_test_state *state) {
    static const char *const invalid_patterns[] = {
        "(a)\\1", "a*?", "a+?", "a??", "a{2}?", "(?:a)", "(?=a)",
        "(?<n>a)", "(*positive_lookahead:a)", "\\b", "\\B", "\\k<n>",
    };
    for (size_t pattern_index = 0;
         pattern_index < SXML_TEST_COUNT(invalid_patterns); pattern_index++) {
        test_compilation compilation = {
            .regex_text = invalid_patterns[pattern_index],
            .flags_text = "",
            .is_xsd_pattern = true};
        test_compile(&compilation);
        bool is_raised =
            compilation.status == SXML_STATUS_DYNAMIC_ERROR &&
            test_view_equals_text(compilation.error_record.code.local_name,
                                  "FORX0002");
        is_raised = test_release(&compilation) && is_raised;
        SXML_TEST_EXPECT(state, is_raised);
        if (!is_raised) {
            SXML_TEST_EXPECT(state, invalid_patterns[pattern_index] == NULL);
        }
    }
}

/* ------------------------------------------------- Class escapes and case */

static void test_category_and_block_escapes(sxml_test_state *state) {
    /* XSD 1.1 Part 2, appendix G, "Category escapes" and "Block
       escapes". */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\p{Lu}+", "", "abCD\xC3\x89" "e"},
                                "[CD\xC3\x89]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\P{L}+", "", "ab12.c"}, "[12.]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[\\p{Nd}\\p{Pd}]+", "", "x1-2y"},
                                "[1-2]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\p{IsGreekandCoptic}+", "",
                                              "a\xCE\xB1\xCE\xB2" "b"},
                                "[\xCE\xB1\xCE\xB2]"));
    /* The XSD 1.0 name of the same block. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\p{IsGreek}", "", "\xCE\xB1"},
                                "[\xCE\xB1]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\P{IsBasicLatin}", "", "ab\xC3\xA9"},
                                "[\xC3\xA9]"));
    /* An unassigned codepoint is Cn (U+0378). */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\p{Cn}", "", "a\xCD\xB8"},
                                "[\xCD\xB8]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\p{C}", "", "a\t"}, "[\t]"));
    /* With x, whitespace inside \p{..} is removed (F&O 4.0, section 6.2). */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\p{ I s B a s i c L a t i n }",
                                              "x", "a"},
                                "[a]"));
    /* The i flag does not affect category escapes. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\p{Lu}", "i", "a"}, "none"));
    /* Subtraction of a category from a block. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[\\p{IsBasicLatin}-[\\p{Ll}]]+",
                                              "", "abC1d"},
                                "[C1]"));
}

static void test_name_character_escapes(sxml_test_state *state) {
    /* \i and \c: NameStartChar and NameChar of XML. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\i\\c*", "", "1.x_:y-2.z w"},
                                "[x_:y-2.z]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\I+", "", "a1.-b"}, "[1.-]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\C+", "", "ab \t\r"}, "[ \t\r]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[\\i-[:]]+", "", ":ab:"}, "[ab]"));
}

static void test_case_insensitive_unicode(sxml_test_state *state) {
    /* F&O 4.0, section 6.2: case variants by fn:lower-case and
       fn:upper-case, beyond ASCII. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\xC3\xA9", "i", "\xC3\x89"},
                                "[\xC3\x89]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[\xC3\xA0-\xC3\xBE]+", "i",
                                              "x\xC3\x80\xC3\x89y"},
                                "[\xC3\x80\xC3\x89]"));
    /* Sigma: final sigma and capital sigma have the same upper case. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\xCF\x83", "i", "\xCF\x82"},
                                "[\xCF\x82]"));
    /* ANGSTROM SIGN lowers to a with ring above. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[\xC3\xA5]", "i", "\xE2\x84\xAB"},
                                "[\xE2\x84\xAB]"));
    /* Back-references compare case-blind: section 6.2 example. */
    static const char *const words[] = {"Mum", "mom", "Dad", "DUD"};
    for (size_t word_index = 0; word_index < SXML_TEST_COUNT(words);
         word_index++) {
        test_text expected = {.character_count = 0};
        expected.characters[0] = '\0';
        test_text_append(&expected, SXML_BYTE_VIEW_FROM_LITERAL("["));
        test_text_append(&expected,
                         sxml_byte_view_from_c_string(words[word_index]));
        test_text_append(&expected, SXML_BYTE_VIEW_FROM_LITERAL("]<"));
        test_text_append(&expected, (sxml_byte_view){
                                        .bytes = (const uint8_t *)words[word_index],
                                        .byte_count = 1});
        test_text_append(&expected, SXML_BYTE_VIEW_FROM_LITERAL(">"));
        SXML_TEST_EXPECT(state, test_first_match_is(
                                    (test_search){"([md])[aeiou]\\1", "i",
                                                  words[word_index]},
                                    expected.characters));
    }
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(\xC3\xA9)\\1", "i", "\xC3\xA9\xC3\x89"},
                                "[\xC3\xA9\xC3\x89]<\xC3\xA9>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(a)\\1", "i", "ab"}, "none"));
}

/* ------------------------------------------------------------- Assertions */

static void test_word_boundaries(sxml_test_state *state) {
    /* F&O 4.0, section 6.1.8.2. */
    SXML_TEST_EXPECT(state, test_tokenize_is(
                                (test_search){"\\b", "", "Do not eat"},
                                "[Do][ ][not][ ][eat]"));
    SXML_TEST_EXPECT(state, test_tokenize_is((test_search){"\\b", "", "the end"},
                                             "[the][ ][end]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\b[A-Z]{3}\\b", "", "LHRX LAX"},
                                "[LAX]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a\\Bb", "", "a b ab"}, "[ab]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\b", "", ""}, "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\B", "", ""}, "[]"));
}

static void test_lookahead_and_lookbehind(sxml_test_state *state) {
    /* F&O 4.0, sections 6.1.8.3 to 6.1.8.6. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"Chapter(?=\\s+[1-9])", "",
                                              "Chapter x Chapter 5"},
                                "[Chapter]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"Chapter(?!\\s*[1-9])", "",
                                              "Chapter 5 Chapter x"},
                                "[Chapter]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<=\\[)[0-9]+(?=\\])", "",
                                              "1 [23] 4"},
                                "[23]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<!\\$)[0-9]+", "", "$12 34"},
                                "[2]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<=ab|c)d", "", "abd"}, "[d]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<=ab|c)d", "", "bd"}, "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<!ab|c)d", "", "cdabdbd"},
                                "[d]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<=\\p{Lu}.)x", "",
                                              "axAbx"},
                                "[x]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<=\xC3\xA9)x", "", "x\xC3\xA9x"},
                                "[x]"));
    /* The other spelling. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a(*positive_lookahead:b)", "", "acab"},
                                "[a]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(*negative_lookbehind:c)a", "", "caba"},
                                "[a]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(*positive_lookbehind:b|cd)e", "", "cde"},
                                "[e]"));
    /* Groups within a lookahead capture (section 6.1.8.3), from its first
       match; a negative one captures nothing. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(Chapter|Appendix)(?=\\s+([0-9]+))",
                                              "", "Chapter 5"},
                                "[Chapter]<Chapter><5>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"\\b(?=(\\w+))", "", "There we"},
                                "[]<There>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"a(?!(b))", "", "abac"}, "[a]<->"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?=(a+))a*b\\1", "", "baaabac"},
                                "[aba]<a>"));
    /* Backtracking into what precedes a lookahead undoes its captures. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?:a(?=(c))x|ac)", "", "ac"},
                                "[ac]<->"));
    /* The section 6.1.1 note: a*(?=x) on aaax gives two segments. */
    SXML_TEST_EXPECT(state, test_tokenize_is((test_search){"a*(?=x)", "", "aaax"},
                                             "[][][x]"));
    /* Nested assertions, and a quantified group holding one. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?=a(?!b))..", "", "abacd"},
                                "[ac]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?:(?<=a)b|c){2,3}", "", "xcabcb"},
                                "[bc]"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?:x(?=y)|y)+", "", "xyxz"},
                                "[xy]"));
    /* The replace example of section 6.3.2 at the segment level. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"[a-z](?=.*(.)$)", "", "abcd!"},
                                "[a]<!>"));
}

static void test_named_groups(sxml_test_state *state) {
    /* F&O 4.0, sections 6.1.5 and 6.1.6. */
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<x>abc)\\k<x>", "", "abcabc"},
                                "[abcabc]<abc>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<x>abc)\\k<x>", "", "abcabd"},
                                "none"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?<quote>'|\").*\\k<quote>", "",
                                              "x'a\"b'"},
                                "['a\"b']<'>"));
    SXML_TEST_EXPECT(state, test_first_match_is(
                                (test_search){"(?< y e a r >\\d+)", "x", "x12"},
                                "[12]<12>"));
    test_compilation compilation = {
        .regex_text = "(?<year>\\d{4})-(\\d{2})-(?<Day1>\\d{2})(?=(?<more>.))",
        .flags_text = ""};
    test_compile(&compilation);
    SXML_TEST_REQUIRE(state, compilation.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_regex_group_count(compilation.regex) == 4);
    SXML_TEST_EXPECT(state, test_view_equals_text(
                                sxml_regex_group_name(compilation.regex, 1),
                                "year"));
    SXML_TEST_EXPECT(state,
                     sxml_regex_group_name(compilation.regex, 2).byte_count == 0);
    SXML_TEST_EXPECT(state, test_view_equals_text(
                                sxml_regex_group_name(compilation.regex, 3),
                                "Day1"));
    SXML_TEST_EXPECT(state,
                     sxml_regex_group_name(compilation.regex, 0).byte_count == 0);
    SXML_TEST_EXPECT(state,
                     sxml_regex_group_name(compilation.regex, 9).byte_count == 0);
    SXML_TEST_EXPECT(state, sxml_regex_find_named_group(
                                compilation.regex,
                                SXML_BYTE_VIEW_FROM_LITERAL("Day1")) == 3);
    SXML_TEST_EXPECT(state, sxml_regex_find_named_group(
                                compilation.regex,
                                SXML_BYTE_VIEW_FROM_LITERAL("day1")) == 0);
    SXML_TEST_EXPECT(state, !sxml_regex_group_is_in_lookahead(compilation.regex, 3));
    SXML_TEST_EXPECT(state, sxml_regex_group_is_in_lookahead(compilation.regex, 4));
    SXML_TEST_EXPECT(state, !sxml_regex_group_is_in_lookahead(compilation.regex, 5));
    SXML_TEST_EXPECT(state, sxml_regex_group_parent(compilation.regex, 1) == 0 &&
                                sxml_regex_group_parent(compilation.regex, 4) == 0 &&
                                sxml_regex_group_parent(compilation.regex, 9) == 0);
    SXML_TEST_EXPECT(state, test_release(&compilation));
    test_compilation nested = {.regex_text = "a(b(?:c(d))(e))(f)", .flags_text = ""};
    test_compile(&nested);
    SXML_TEST_REQUIRE(state, nested.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_regex_group_parent(nested.regex, 1) == 0 &&
                                sxml_regex_group_parent(nested.regex, 2) == 1 &&
                                sxml_regex_group_parent(nested.regex, 3) == 1 &&
                                sxml_regex_group_parent(nested.regex, 4) == 0);
    SXML_TEST_EXPECT(state, test_release(&nested));
    /* More names than the compiler's own name table holds. */
    char many_names[2048];
    size_t written_count = 0;
    for (size_t group_index = 0; group_index < 60; group_index++) {
        int printed = snprintf(many_names + written_count,
                               sizeof(many_names) - written_count,
                               "(?<groupname%zu>a)", group_index);
        written_count += printed > 0 ? (size_t)printed : 0;
    }
    (void)snprintf(many_names + written_count,
                   sizeof(many_names) - written_count, "\\k<groupname59>");
    test_compilation many = {.regex_text = many_names, .flags_text = ""};
    test_compile(&many);
    SXML_TEST_EXPECT(state, many.status == SXML_STATUS_OK &&
                                sxml_regex_find_named_group(
                                    many.regex, SXML_BYTE_VIEW_FROM_LITERAL(
                                                    "groupname42")) == 43);
    SXML_TEST_EXPECT(state, test_release(&many));
}

/* ----------------------------------------------------------------- Limits */

static void test_catastrophic_backtracking_reaches_the_step_limit(
    sxml_test_state *state) {
    test_compilation compilation = {.regex_text = "(a*)*b",
                                    .flags_text = "",
                                    .maximum_step_count = 100000};
    test_compile(&compilation);
    SXML_TEST_REQUIRE(state, compilation.status == SXML_STATUS_OK);
    sxml_regex_search_result result = {.is_found = true};
    sxml_status status = sxml_regex_matcher_find(
        compilation.matcher,
        &(sxml_regex_search_request){
            .regex_input = test_string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
            .start_byte_offset = 0},
        &result);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, result.is_found);
    sxml_regex_captured_group group = {.is_captured = false};
    SXML_TEST_EXPECT(state, sxml_regex_matcher_get_captured_group(
                                compilation.matcher, 0, &group) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, test_release(&compilation));
}

static void test_compile_limits(sxml_test_state *state) {
    test_compilation compilation = {.regex_text = "a{1000}{1000}",
                                    .flags_text = ""};
    test_compile(&compilation);
    /* a{1000}{1000} is invalid: one quantifier per atom. */
    SXML_TEST_EXPECT(state, compilation.status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, test_release(&compilation));
    /* Counted loops (D147): large counts compile to a few instructions. */
    static const char *const counted_texts[] = {
        "(?:a{1000}){1000}",
        "(?:(?:){1000}){1000}",
        "a{4294967296}",
        "(?:(?:(?:(?:a{9}){9}){9}){9}){9}",
    };
    for (size_t text_index = 0;
         text_index < sizeof(counted_texts) / sizeof(counted_texts[0]);
         text_index++) {
        compilation = (test_compilation){.regex_text = counted_texts[text_index],
                                         .flags_text = "",
                                         .maximum_instruction_count = 1000};
        test_compile(&compilation);
        SXML_TEST_EXPECT(state, compilation.status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, test_release(&compilation));
    }
    compilation = (test_compilation){.regex_text = "a{1000}",
                                     .flags_text = "",
                                     .maximum_instruction_count = 5};
    test_compile(&compilation);
    SXML_TEST_EXPECT(state, compilation.status == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, test_release(&compilation));
    compilation = (test_compilation){.regex_text = "abcdef",
                                     .flags_text = "",
                                     .maximum_instruction_count = 4};
    test_compile(&compilation);
    SXML_TEST_EXPECT(state, compilation.status == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, test_release(&compilation));

    char nested_text[2 * 150 + 2];
    for (size_t index = 0; index < 150; index++) {
        nested_text[index] = '(';
        nested_text[150 + 1 + index] = ')';
    }
    nested_text[150] = 'a';
    nested_text[2 * 150 + 1] = '\0';
    compilation = (test_compilation){.regex_text = nested_text,
                                     .flags_text = ""};
    test_compile(&compilation);
    SXML_TEST_EXPECT(state, compilation.status == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, test_release(&compilation));
}

static void test_fixed_backtrack_stack_reports_capacity(
    sxml_test_state *state) {
    sxml_regex *regex = NULL;
    SXML_TEST_REQUIRE(state, sxml_regex_create(sxml_default_allocator(),
                                               &(sxml_regex_configuration){
                                                   .regex_text = test_string("a*b"),
                                               },
                                               &regex) == SXML_STATUS_OK);
    sxml_regex_matcher *matcher = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_regex_matcher_create(
                   sxml_default_allocator(),
                   &(sxml_regex_matcher_configuration){
                       .regex = regex,
                       .backtrack_allocator = {.state = NULL, .operations = NULL},
                       .initial_backtrack_entry_count = 4,
                   },
                   &matcher) == SXML_STATUS_OK);
    sxml_regex_search_result result = {.is_found = false};
    sxml_status status = sxml_regex_matcher_find(
        matcher,
        &(sxml_regex_search_request){.regex_input = test_string("aaaaaaaab"),
                                     .start_byte_offset = 0},
        &result);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_CAPACITY_EXCEEDED);
    status = sxml_regex_matcher_find(
        matcher,
        &(sxml_regex_search_request){.regex_input = test_string("b"),
                                     .start_byte_offset = 0},
        &result);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK && result.is_found);
    sxml_regex_matcher_destroy(matcher);
    sxml_regex_destroy(regex);
}

/* ------------------------------------------- Copies and deep nesting (D71) */

/* Quantified atoms are emitted once and copied; these match as when every
   copy was parsed again (the results are those of the earlier parser). */
static void test_quantified_copies_match_alike(sxml_test_state *state) {
    static const struct {
        test_search search;
        const char *expected;
    } cases[] = {
        {{"(a*b){2,3}", "", "abaabbx"}, "[abaabb]<b>"},
        {{"(?:x(a|b)*y){2}", "", "xabyxbaay"}, "[xabyxbaay]<a>"},
        {{"(a|b){0,2}c", "", "abc"}, "[abc]<b>"},
        {{"(a|b){0,2}c", "", "c"}, "[c]<->"},
        {{"(a){0}b\\1", "", "b"}, "[b]<->"},
        {{"(?:a{2}){2,}", "", "aaaaaaa"}, "[aaaaaa]"},
        {{"a{2,4}?", "", "aaaa"}, "[aa]"},
        {{"(?:ab){1,2}?c", "", "ababc"}, "[ababc]"},
        {{"a*?b", "", "aab"}, "[aab]"},
        {{"(?:a?b)+", "", "abbab"}, "[abbab]"},
        {{"(?:a|bc){3}", "", "bcabca"}, "[bcabc]"},
        {{"(a|)+b", "", "aab"}, "[aab]<a>"},
        {{"((a)|b)*?c", "", "abac"}, "[abac]<a><a>"},
        {{"(?:(a*)+){2}b", "", "aab"}, "[aab]<>"},
        {{"x(?:a*|b)*y", "", "xabay"}, "[xabay]"},
        {{"([ab])(?:[a-c-[b]]\\1){2,3}", "", "aaacaba"}, "[aaaca]<a>"},
        {{"(?:a|b|c){2}d", "", "cbd"}, "[cbd]"},
        /* What a dropped x{0} wrote at the end still fits the arrays. */
        {{"a(bc|[de]){0}", "", "xa"}, "[a]<->"},
        {{"a[a-z-[b]]{0}", "", "a"}, "[a]"},
    };
    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]);
         case_index++) {
        SXML_TEST_EXPECT(state, test_first_match_is(cases[case_index].search,
                                                    cases[case_index].expected));
    }
}

/*
 * Counted loops (D147): counts whose copies would pass the expansion
 * limit run as one atom and a counter, and match as the copies would.
 */
static void test_counted_loops_match_like_copies(sxml_test_state *state) {
    static const struct {
        test_search search;
        const char *expected;
    } cases[] = {
        /* QT4 cbcl-matches-038 and -004. */
        {{"a{2147483647}", "", "aaa"}, "none"},
        {{"a{99999999999999999999999999}", "", "aaa"}, "none"},
        {{"a{100000,}", "", "aaaa"}, "none"},
        {{"ba{0,100000}", "", "baaac"}, "[baaa]"},
        {{"ba{0,100000}?", "", "baaac"}, "[b]"},
        {{"ba{0,100000}?c", "", "baaac"}, "[baaac]"},
        /* The cases of quantified_copies_match_alike, with counts large
           enough for a loop and inputs too short to tell. */
        {{"(a*b){2,3000}", "", "abaabbx"}, "[abaabb]<b>"},
        {{"(?:x(a|b)*y){2,1000}", "", "xabyxbaay"}, "[xabyxbaay]<a>"},
        {{"(a|b){0,2000}c", "", "abc"}, "[abc]<b>"},
        {{"(a|b){0,2000}c", "", "c"}, "[c]<->"},
        {{"(?:a{2}){2,1000}", "", "aaaaaaa"}, "[aaaaaa]"},
        {{"a{2,4000}?", "", "aaaa"}, "[aa]"},
        {{"(?:ab){1,2000}?c", "", "ababc"}, "[ababc]"},
        {{"(?:a?b){1,1000}", "", "abbab"}, "[abbab]"},
        {{"(a|){1,1000}b", "", "aab"}, "[aab]<a>"},
        {{"((a)|b){0,1000}?c", "", "abac"}, "[abac]<a><a>"},
        {{"(?:(a*)+){2,1000}b", "", "aab"}, "[aab]<>"},
        {{"x(?:a*|b){0,1000}y", "", "xabay"}, "[xabay]"},
        {{"([ab])(?:[a-c-[b]]\\1){2,3000}", "", "aaacaba"}, "[aaaca]<a>"},
        {{"(?:a|b|c){2,1000}d", "", "cbd"}, "[cbd]"},
        {{"(a)(?:\\1){2,1000}", "", "aaaa"}, "[aaaa]<a>"},
        {{"(?:(?=(.))a){1,1000}", "", "aab"}, "[aa]<a>"},
        {{"(?:a|ab){1000}", "", "ab"}, "none"},
        /* Iterations that match nothing: below the minimum one stands for
           all those still needed; above it, one ends the loop. */
        {{"(?:){2147483647}", "", "x"}, "[]"},
        {{"(a?){2147483647}b", "", "aab"}, "[aab]<>"},
        {{"(a?){3,2147483647}b", "", "aaaab"}, "[aaaab]<a>"},
        {{"(?:a|()){500}$", "", "aa"}, "[aa]<>"},
        /* A dropped x{0} with a loop inside still fits the arrays. */
        {{"(?:b{1000}){0}c", "", "c"}, "[c]"},
        {{"a(b{1000}){0}", "", "xa"}, "[a]<->"},
    };
    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]);
         case_index++) {
        SXML_TEST_EXPECT(state, test_first_match_is(cases[case_index].search,
                                                    cases[case_index].expected));
    }
}

/* A loop of a thousand iterations, nested in another, at its edge. */
static void test_counted_loops_count_exactly(sxml_test_state *state) {
    static char input_text[2001];
    static const struct {
        const char *regex_text;
        size_t input_byte_count;
        bool is_found;
    } cases[] = {
        {"a{1000}", 999, false},
        {"a{1000}", 1000, true},
        {"(?:a{500}){2}$", 1000, true},
        {"(?:a{500}){2}$", 1001, false},
        {"^(?:a{3}){600,700}$", 2000, false},
        {"^(?:a{4}){500,700}$", 2000, true},
        {"^(?:a{1000}){2,300}$", 2000, true},
        {"^(?:a{1000}){2,300}$", 1999, false},
    };
    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]);
         case_index++) {
        test_compilation compilation = {.regex_text =
                                            cases[case_index].regex_text,
                                        .flags_text = ""};
        test_compile(&compilation);
        SXML_TEST_REQUIRE(state, compilation.status == SXML_STATUS_OK);
        memset(input_text, 'a', cases[case_index].input_byte_count);
        input_text[cases[case_index].input_byte_count] = '\0';
        sxml_regex_search_result result = {.is_found = false};
        sxml_status status = sxml_regex_matcher_match_at(
            compilation.matcher,
            &(sxml_regex_search_request){.regex_input = test_string(input_text),
                                         .start_byte_offset = 0},
            &result);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, result.is_found == cases[case_index].is_found);
        SXML_TEST_EXPECT(state, !result.is_found ||
                                    result.matching_segment.byte_count ==
                                        cases[case_index].input_byte_count);
        SXML_TEST_EXPECT(state, test_release(&compilation));
    }
}

/* A text of opening repeated depth times, middle, then closing repeated
   depth times. */
typedef struct test_nested_text {
    const char *opening;
    const char *middle;
    const char *closing;
    size_t depth;
} test_nested_text;

/* Builds the text in memory from the default allocator; NULL when that
   fails. Release with sxml_allocator_release. */
static char *test_build_nested_text(const test_nested_text *nested) {
    size_t opening_count = strlen(nested->opening);
    size_t middle_count = strlen(nested->middle);
    size_t closing_count = strlen(nested->closing);
    size_t byte_count =
        nested->depth * (opening_count + closing_count) + middle_count + 1;
    void *memory = NULL;
    if (sxml_allocator_allocate(sxml_default_allocator(),
                                (sxml_allocation_request){
                                    .byte_count = byte_count, .alignment = 1},
                                &memory) != SXML_STATUS_OK) {
        return NULL;
    }
    char *text = memory;
    size_t position = 0;
    for (size_t level = 0; level < nested->depth; level++) {
        memcpy(text + position, nested->opening, opening_count);
        position += opening_count;
    }
    memcpy(text + position, nested->middle, middle_count);
    position += middle_count;
    for (size_t level = 0; level < nested->depth; level++) {
        memcpy(text + position, nested->closing, closing_count);
        position += closing_count;
    }
    text[position] = '\0';
    return text;
}

/* A search with a deeply nested regex. */
typedef struct test_deep_search {
    test_nested_text regex;
    uint32_t maximum_group_depth;
    const char *input_text;
} test_deep_search;

typedef struct test_deep_outcome {
    /* Of compiling, then of searching. */
    sxml_status status;
    sxml_regex_search_result result;
    uint32_t group_count;
    /* The captured group with the highest number. */
    sxml_regex_captured_group last_group;
    bool is_leak_free;
} test_deep_outcome;

/* Compiles through a counting allocator, which also holds the group stack,
   and finds the first match. */
static test_deep_outcome test_run_deep_search(test_deep_search search) {
    test_deep_outcome outcome = {.status = SXML_STATUS_OUT_OF_MEMORY,
                                 .result = {.is_found = false},
                                 .group_count = 0,
                                 .last_group = {.is_captured = false},
                                 .is_leak_free = false};
    char *regex_text = test_build_nested_text(&search.regex);
    sxml_counting_allocator *counting_allocator = NULL;
    if (regex_text == NULL ||
        sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){
                .backing_allocator = sxml_default_allocator(),
                .failing_allocation_number = 0,
            },
            &counting_allocator) != SXML_STATUS_OK) {
        sxml_allocator_release(sxml_default_allocator(), regex_text);
        return outcome;
    }
    sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(counting_allocator);
    sxml_regex *regex = NULL;
    sxml_regex_matcher *matcher = NULL;
    outcome.status = sxml_regex_create(
        allocator,
        &(sxml_regex_configuration){
            .regex_text = test_string(regex_text),
            .maximum_group_depth = search.maximum_group_depth,
        },
        &regex);
    if (outcome.status == SXML_STATUS_OK) {
        outcome.group_count = sxml_regex_group_count(regex);
        outcome.status = sxml_regex_matcher_create(
            allocator,
            &(sxml_regex_matcher_configuration){.regex = regex,
                                                .backtrack_allocator =
                                                    allocator},
            &matcher);
    }
    if (outcome.status == SXML_STATUS_OK) {
        outcome.status = sxml_regex_matcher_find(
            matcher,
            &(sxml_regex_search_request){
                .regex_input = test_string(search.input_text),
                .start_byte_offset = 0},
            &outcome.result);
    }
    if (outcome.status == SXML_STATUS_OK && outcome.result.is_found &&
        sxml_regex_matcher_get_captured_group(matcher, outcome.group_count,
                                              &outcome.last_group) !=
            SXML_STATUS_OK) {
        outcome.last_group.is_captured = false;
    }
    sxml_regex_matcher_destroy(matcher);
    sxml_regex_destroy(regex);
    sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(counting_allocator);
    sxml_counting_allocator_destroy(counting_allocator);
    sxml_allocator_release(sxml_default_allocator(), regex_text);
    outcome.is_leak_free = counts.live_allocation_count == 0;
    return outcome;
}

/* True when outcome found the segment at byte_offset of byte_count. */
static bool test_deep_found(test_deep_outcome outcome, sxml_byte_range segment) {
    return outcome.status == SXML_STATUS_OK && outcome.is_leak_free &&
           outcome.result.is_found &&
           outcome.result.matching_segment.byte_offset == segment.byte_offset &&
           outcome.result.matching_segment.byte_count == segment.byte_count;
}

#define TEST_DEEP_NESTING 10000U

static void test_groups_nested_ten_thousand_deep(sxml_test_state *state) {
    test_nested_text capturing = {.opening = "(", .middle = "a", .closing = ")",
                                  .depth = TEST_DEEP_NESTING};
    test_deep_outcome outcome = test_run_deep_search((test_deep_search){
        .regex = capturing,
        .maximum_group_depth = TEST_DEEP_NESTING,
        .input_text = "xay",
    });
    SXML_TEST_EXPECT(state, test_deep_found(outcome, (sxml_byte_range){
                                                         .byte_offset = 1,
                                                         .byte_count = 1}));
    SXML_TEST_EXPECT(state, outcome.group_count == TEST_DEEP_NESTING);
    SXML_TEST_EXPECT(state, outcome.last_group.is_captured &&
                                outcome.last_group.byte_range.byte_offset == 1 &&
                                outcome.last_group.byte_range.byte_count == 1);
    outcome = test_run_deep_search((test_deep_search){
        .regex = capturing,
        .maximum_group_depth = TEST_DEEP_NESTING - 1,
        .input_text = "a",
    });
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_LIMIT_EXCEEDED &&
                                outcome.is_leak_free);

    test_nested_text optional = {.opening = "(?:", .middle = "a",
                                 .closing = ")?", .depth = TEST_DEEP_NESTING};
    outcome = test_run_deep_search((test_deep_search){
        .regex = optional,
        .maximum_group_depth = TEST_DEEP_NESTING,
        .input_text = "ba",
    });
    SXML_TEST_EXPECT(state, test_deep_found(outcome, (sxml_byte_range){
                                                         .byte_offset = 0,
                                                         .byte_count = 0}));
    outcome = test_run_deep_search((test_deep_search){
        .regex = optional,
        .maximum_group_depth = TEST_DEEP_NESTING,
        .input_text = "ab",
    });
    SXML_TEST_EXPECT(state, test_deep_found(outcome, (sxml_byte_range){
                                                         .byte_offset = 0,
                                                         .byte_count = 1}));

    test_nested_text alternatives = {.opening = "(?:a|", .middle = "b",
                                     .closing = ")", .depth = TEST_DEEP_NESTING};
    outcome = test_run_deep_search((test_deep_search){
        .regex = alternatives,
        .maximum_group_depth = TEST_DEEP_NESTING,
        .input_text = "xb",
    });
    SXML_TEST_EXPECT(state, test_deep_found(outcome, (sxml_byte_range){
                                                         .byte_offset = 1,
                                                         .byte_count = 1}));

    test_nested_text unclosed = {.opening = "(", .middle = "a", .closing = "",
                                 .depth = TEST_DEEP_NESTING};
    outcome = test_run_deep_search((test_deep_search){
        .regex = unclosed,
        .maximum_group_depth = TEST_DEEP_NESTING,
        .input_text = "a",
    });
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_DYNAMIC_ERROR &&
                                outcome.is_leak_free);
}

static void test_class_subtraction_nested_ten_thousand_deep(
    sxml_test_state *state) {
    /* [a-[a-[...[a]...]]]: a chain of n classes holds "a" when n is odd. */
    test_nested_text odd_chain = {.opening = "[a-", .middle = "[a]",
                                  .closing = "]", .depth = TEST_DEEP_NESTING};
    test_deep_outcome outcome = test_run_deep_search((test_deep_search){
        .regex = odd_chain,
        .maximum_group_depth = TEST_DEEP_NESTING + 1,
        .input_text = "ba",
    });
    SXML_TEST_EXPECT(state, test_deep_found(outcome, (sxml_byte_range){
                                                         .byte_offset = 1,
                                                         .byte_count = 1}));
    test_nested_text even_chain = odd_chain;
    even_chain.depth = TEST_DEEP_NESTING - 1;
    outcome = test_run_deep_search((test_deep_search){
        .regex = even_chain,
        .maximum_group_depth = TEST_DEEP_NESTING,
        .input_text = "ba",
    });
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK &&
                                outcome.is_leak_free &&
                                !outcome.result.is_found);
    outcome = test_run_deep_search((test_deep_search){
        .regex = odd_chain,
        .maximum_group_depth = TEST_DEEP_NESTING,
        .input_text = "a",
    });
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_LIMIT_EXCEEDED &&
                                outcome.is_leak_free);
    test_nested_text unclosed = {.opening = "[a-", .middle = "[a]",
                                 .closing = "", .depth = TEST_DEEP_NESTING};
    outcome = test_run_deep_search((test_deep_search){
        .regex = unclosed,
        .maximum_group_depth = TEST_DEEP_NESTING + 1,
        .input_text = "a",
    });
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_DYNAMIC_ERROR &&
                                outcome.is_leak_free);
}

/* Without a stack allocator the compiler holds
   SXML_REGEX_INLINE_GROUP_DEPTH nested groups. */
static void test_group_stack_without_allocator(sxml_test_state *state) {
    for (size_t depth = SXML_REGEX_INLINE_GROUP_DEPTH;
         depth <= SXML_REGEX_INLINE_GROUP_DEPTH + 1; depth++) {
        char *regex_text = test_build_nested_text(&(test_nested_text){
            .opening = "(", .middle = "a", .closing = ")", .depth = depth});
        SXML_TEST_REQUIRE(state, regex_text != NULL);
        sxml_memory_requirements requirements = {.byte_count = 0,
                                                 .alignment = 1};
        sxml_status status = sxml_regex_memory_requirements(
            &(sxml_regex_configuration){.regex_text = test_string(regex_text)},
            &requirements);
        sxml_allocator_release(sxml_default_allocator(), regex_text);
        SXML_TEST_EXPECT(state, status == (depth == SXML_REGEX_INLINE_GROUP_DEPTH
                                               ? SXML_STATUS_OK
                                               : SXML_STATUS_CAPACITY_EXCEEDED));
    }
}

/* Growing the group stack fails cleanly at every allocation (D23). */
static void test_group_stack_allocation_failures(sxml_test_state *state) {
    char *regex_text = test_build_nested_text(&(test_nested_text){
        .opening = "(?:", .middle = "a", .closing = ")*", .depth = 60});
    SXML_TEST_REQUIRE(state, regex_text != NULL);
    for (uint64_t failing_allocation_number = 1;; failing_allocation_number++) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(
            state, sxml_counting_allocator_create(
                       sxml_default_allocator(),
                       &(sxml_counting_allocator_configuration){
                           .backing_allocator = sxml_default_allocator(),
                           .failing_allocation_number = failing_allocation_number,
                       },
                       &counting_allocator) == SXML_STATUS_OK);
        sxml_regex *regex = NULL;
        sxml_status status = sxml_regex_create(
            sxml_counting_allocator_as_allocator(counting_allocator),
            &(sxml_regex_configuration){.regex_text = test_string(regex_text)},
            &regex);
        sxml_regex_destroy(regex);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            /* Two passes, each growing the stack twice, then the regex. */
            SXML_TEST_EXPECT(state, failing_allocation_number > 4);
            break;
        }
        SXML_TEST_REQUIRE(state, status == SXML_STATUS_OUT_OF_MEMORY);
    }
    sxml_allocator_release(sxml_default_allocator(), regex_text);
}

static const sxml_test_case regex_test_cases[] = {
    {.name = "flags_are_read", .function = test_flags_are_read},
    {.name = "unknown_flag_raises_forx0001",
     .function = test_unknown_flag_raises_forx0001},
    {.name = "regex_initializes_in_caller_memory",
     .function = test_regex_initializes_in_caller_memory},
    {.name = "regex_rejects_too_large_instruction_limit",
     .function = test_regex_rejects_too_large_instruction_limit},
    {.name = "allocation_failures_leave_nothing_behind",
     .function = test_allocation_failures_leave_nothing_behind},
    {.name = "literals_classes_and_escapes",
     .function = test_literals_classes_and_escapes},
    {.name = "dot_and_the_s_flag", .function = test_dot_and_the_s_flag},
    {.name = "quantifiers_greedy_and_reluctant",
     .function = test_quantifiers_greedy_and_reluctant},
    {.name = "groups_and_alternation", .function = test_groups_and_alternation},
    {.name = "back_references", .function = test_back_references},
    {.name = "anchors_and_the_m_flag", .function = test_anchors_and_the_m_flag},
    {.name = "case_insensitive_matching",
     .function = test_case_insensitive_matching},
    {.name = "whitespace_comments_and_literal_flags",
     .function = test_whitespace_comments_and_literal_flags},
    {.name = "tokenize_slice_cases", .function = test_tokenize_slice_cases},
    {.name = "tokenize_specification_examples",
     .function = test_tokenize_specification_examples},
    {.name = "search_positions", .function = test_search_positions},
    {.name = "invalid_regexes_raise_forx0002",
     .function = test_invalid_regexes_raise_forx0002},
    {.name = "invalid_assertions_and_escapes_raise_forx0002",
     .function = test_invalid_assertions_and_escapes_raise_forx0002},
    {.name = "xsd_patterns_match_the_whole_value",
     .function = test_xsd_patterns_match_the_whole_value},
    {.name = "xsd_patterns_reject_fo_extensions",
     .function = test_xsd_patterns_reject_fo_extensions},
    {.name = "category_and_block_escapes",
     .function = test_category_and_block_escapes},
    {.name = "name_character_escapes", .function = test_name_character_escapes},
    {.name = "case_insensitive_unicode",
     .function = test_case_insensitive_unicode},
    {.name = "word_boundaries", .function = test_word_boundaries},
    {.name = "lookahead_and_lookbehind",
     .function = test_lookahead_and_lookbehind},
    {.name = "named_groups", .function = test_named_groups},
    {.name = "catastrophic_backtracking_reaches_the_step_limit",
     .function = test_catastrophic_backtracking_reaches_the_step_limit},
    {.name = "compile_limits", .function = test_compile_limits},
    {.name = "fixed_backtrack_stack_reports_capacity",
     .function = test_fixed_backtrack_stack_reports_capacity},
    {.name = "quantified_copies_match_alike",
     .function = test_quantified_copies_match_alike},
    {.name = "counted_loops_match_like_copies",
     .function = test_counted_loops_match_like_copies},
    {.name = "counted_loops_count_exactly",
     .function = test_counted_loops_count_exactly},
    {.name = "groups_nested_ten_thousand_deep",
     .function = test_groups_nested_ten_thousand_deep},
    {.name = "class_subtraction_nested_ten_thousand_deep",
     .function = test_class_subtraction_nested_ten_thousand_deep},
    {.name = "group_stack_without_allocator",
     .function = test_group_stack_without_allocator},
    {.name = "group_stack_allocation_failures",
     .function = test_group_stack_allocation_failures},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "regex",
            .cases = regex_test_cases,
            .case_count = SXML_TEST_COUNT(regex_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
