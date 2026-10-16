/*
 * Tests for edit distance and "did you mean" suggestions
 * (apps/cli_framework/cli_suggestion.h).
 */
#include "cli_suggestion.h"
#include "sxml/memory.h"
#include "test_runner.h"

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static size_t distance_of(sxml_byte_view first, sxml_byte_view second) {
    size_t distance = SIZE_MAX;
    const sxml_status status = sxml_cli_edit_distance(
        (sxml_cli_text_pair){.first = first, .second = second}, &distance);
    return status == SXML_STATUS_OK ? distance : SIZE_MAX;
}

static void test_edit_distance_counts_edits(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, distance_of(TEXT("version"), TEXT("version")) == 0);
    SXML_TEST_EXPECT(state, distance_of(TEXT(""), TEXT("abc")) == 3);
    SXML_TEST_EXPECT(state, distance_of(TEXT("abc"), TEXT("")) == 3);
    SXML_TEST_EXPECT(state, distance_of(TEXT("kitten"), TEXT("sitting")) == 3);
    SXML_TEST_EXPECT(state, distance_of(TEXT("format"), TEXT("fromat")) == 1);
    SXML_TEST_EXPECT(state, distance_of(TEXT("verison"), TEXT("version")) == 1);
    SXML_TEST_EXPECT(state, distance_of(TEXT("hlep"), TEXT("help")) == 1);
    /* Optimal string alignment: "ca" to "abc" is 3, not 2 as in the
       unrestricted Damerau-Levenshtein distance. */
    SXML_TEST_EXPECT(state, distance_of(TEXT("ca"), TEXT("abc")) == 3);
}

static void test_edit_distance_ignores_ascii_case(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, distance_of(TEXT("JSON"), TEXT("json")) == 0);
    SXML_TEST_EXPECT(state, distance_of(TEXT("Help"), TEXT("hELP")) == 0);
    SXML_TEST_EXPECT(state, distance_of(TEXT("["), TEXT("{")) == 1);
}

static void test_edit_distance_limits_length(sxml_test_state *state) {
    static const sxml_byte_view long_text = SXML_BYTE_VIEW_INITIALIZER(
        "0123456789012345678901234567890123456789012345678901234567890123"
        "4");
    size_t distance = 7;
    SXML_TEST_EXPECT(state, sxml_cli_edit_distance(
                                (sxml_cli_text_pair){.first = long_text,
                                                     .second = TEXT("a")},
                                &distance) == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, distance == 7);
    const sxml_byte_view longest = {
        .bytes = long_text.bytes,
        .byte_count = SXML_CLI_EDIT_DISTANCE_MAXIMUM_BYTE_COUNT};
    SXML_TEST_EXPECT(state, distance_of(longest, longest) == 0);
}

static sxml_byte_view suggest(sxml_byte_view typed_text,
                              const sxml_byte_view *candidates,
                              size_t candidate_count) {
    sxml_cli_suggestion suggestion = sxml_cli_suggestion_start(typed_text);
    for (size_t candidate_index = 0; candidate_index < candidate_count;
         candidate_index += 1) {
        sxml_cli_suggestion_consider(&suggestion, candidates[candidate_index]);
    }
    return suggestion.has_candidate ? suggestion.best_candidate : TEXT("");
}

static const sxml_byte_view test_candidates[] = {
    SXML_BYTE_VIEW_INITIALIZER("help"),
    SXML_BYTE_VIEW_INITIALIZER("version"),
    SXML_BYTE_VIEW_INITIALIZER("format"),
    SXML_BYTE_VIEW_INITIALIZER("force"),
};

static sxml_byte_view suggest_candidate(sxml_byte_view typed_text) {
    return suggest(typed_text, test_candidates, SXML_TEST_COUNT(test_candidates));
}

static void test_suggestion_finds_close_names(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(suggest_candidate(TEXT("verison")),
                                                  TEXT("version")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(suggest_candidate(TEXT("hlep")),
                                                  TEXT("help")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(suggest_candidate(TEXT("fromat")),
                                                  TEXT("format")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(suggest_candidate(TEXT("HELP")),
                                                  TEXT("help")));
}

static void test_suggestion_rejects_distant_names(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, suggest_candidate(TEXT("transmogrify")).byte_count == 0);
    /* Up to 3 bytes typed, only one edit is allowed. */
    SXML_TEST_EXPECT(state, suggest_candidate(TEXT("hx")).byte_count == 0);
    SXML_TEST_EXPECT(state, suggest_candidate(TEXT("")).byte_count == 0);
}

static void test_suggestion_completes_prefixes(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(suggest_candidate(TEXT("vers")),
                                                  TEXT("version")));
    /* "form" is a prefix of "format" (ranked as 2 edits) and 2 edits from
       "force": on the tie the earlier candidate wins. */
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(suggest_candidate(TEXT("form")),
                                                  TEXT("format")));
    /* "forc" is 1 edit from "force", closer than any prefix match. */
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(suggest_candidate(TEXT("forc")),
                                                  TEXT("force")));
}

static void test_suggestion_prefers_first_on_tie(sxml_test_state *state) {
    static const sxml_byte_view tied[] = {
        SXML_BYTE_VIEW_INITIALIZER("json"),
        SXML_BYTE_VIEW_INITIALIZER("jsox"),
    };
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(suggest(TEXT("jsoy"), tied, 2),
                                           TEXT("json")));
}

static const sxml_test_case cli_suggestion_test_cases[] = {
    {.name = "edit_distance_counts_edits",
     .function = test_edit_distance_counts_edits},
    {.name = "edit_distance_ignores_ascii_case",
     .function = test_edit_distance_ignores_ascii_case},
    {.name = "edit_distance_limits_length",
     .function = test_edit_distance_limits_length},
    {.name = "suggestion_finds_close_names",
     .function = test_suggestion_finds_close_names},
    {.name = "suggestion_rejects_distant_names",
     .function = test_suggestion_rejects_distant_names},
    {.name = "suggestion_completes_prefixes",
     .function = test_suggestion_completes_prefixes},
    {.name = "suggestion_prefers_first_on_tie",
     .function = test_suggestion_prefers_first_on_tie},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "cli_suggestion",
            .cases = cli_suggestion_test_cases,
            .case_count = SXML_TEST_COUNT(cli_suggestion_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
