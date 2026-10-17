#include "sxml/version.h"
#include "test_runner.h"

#include <string.h>

static void test_version_text_matches_version_macros(sxml_test_state *state) {
    const char *version_text = sxml_version_text();
    SXML_TEST_REQUIRE(state, version_text != NULL);
    SXML_TEST_EXPECT(state, strcmp(version_text, "0.2.0") == 0);
    SXML_TEST_EXPECT(state, SXML_VERSION_MAJOR == 0);
    SXML_TEST_EXPECT(state, SXML_VERSION_NUMBER == SXML_VERSION_ENCODE(0, 2, 0));
    /* Release 0.2.0 is the first public release: its API is its own (D104). */
    SXML_TEST_EXPECT(state, SXML_API_VERSION == SXML_VERSION_NUMBER);
}

static const sxml_test_case version_test_cases[] = {
    {
        .name = "version_text_matches_version_macros",
        .function = test_version_text_matches_version_macros,
    },
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "version",
            .cases = version_test_cases,
            .case_count = SXML_TEST_COUNT(version_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
