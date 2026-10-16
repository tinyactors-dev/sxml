/*
 * Smoke test for the vendored tree-sitter runtime (docs/DECISIONS.md, D12):
 * it links on every platform and runs clean under the sanitizers. Grammar
 * tests arrive with the first grammar.
 */
#include "test_runner.h"

#include <tree_sitter/api.h>

static void test_parser_starts_without_language(sxml_test_state *state) {
    TSParser *parser = ts_parser_new();
    SXML_TEST_REQUIRE(state, parser != NULL);
    SXML_TEST_EXPECT(state, ts_parser_language(parser) == NULL);
    ts_parser_delete(parser);
}

static void test_runtime_supports_current_language_version(
    sxml_test_state *state) {
    SXML_TEST_EXPECT(state, TREE_SITTER_LANGUAGE_VERSION >= 15);
    SXML_TEST_EXPECT(state, TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION <=
                                TREE_SITTER_LANGUAGE_VERSION);
}

static const sxml_test_case tree_sitter_runtime_test_cases[] = {
    {
        .name = "parser_starts_without_language",
        .function = test_parser_starts_without_language,
    },
    {
        .name = "runtime_supports_current_language_version",
        .function = test_runtime_supports_current_language_version,
    },
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "tree_sitter_runtime",
            .cases = tree_sitter_runtime_test_cases,
            .case_count = SXML_TEST_COUNT(tree_sitter_runtime_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
