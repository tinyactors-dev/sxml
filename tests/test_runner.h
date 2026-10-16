#pragma once

/*
 * Dependency-free unit test runner (docs/DECISIONS.md, D16).
 *
 * Each module has one test executable, tests/<module>_test.c, which defines
 * its test cases as functions, lists them in a static const sxml_test_suite and
 * passes it to sxml_test_run_suite from main. tests/CMakeLists.txt registers
 * the executable with ctest through sxml_add_test(<module>).
 *
 *     static void test_version_text_is_not_empty(sxml_test_state *state) {
 *         SXML_TEST_EXPECT(state, sxml_version_text()[0] != '\0');
 *     }
 *
 *     static const sxml_test_case version_test_cases[] = {
 *         {.name = "version_text_is_not_empty",
 *          .function = test_version_text_is_not_empty},
 *     };
 *
 *     int main(int argument_count, char **arguments) {
 *         return sxml_test_run_suite(
 *             &(sxml_test_suite){
 *                 .name = "version",
 *                 .cases = version_test_cases,
 *                 .case_count = SXML_TEST_COUNT(version_test_cases),
 *             },
 *             (sxml_test_command_line){
 *                 .argument_count = argument_count,
 *                 .arguments = arguments,
 *             });
 *     }
 *
 * Running the executable with case names as arguments runs only those cases.
 * Output goes to standard output; the process exit status is zero when every
 * selected case passed. The runner is test code, not library code, so it may
 * print.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Progress of the test case currently running. Opaque to test cases. */
typedef struct sxml_test_state sxml_test_state;

/* A test case body. Records failures in state through the check macros. */
typedef void (*sxml_test_function)(sxml_test_state *state);

typedef struct sxml_test_case {
    /* NUL-terminated name, unique within the suite; used to select cases. */
    const char *name;
    sxml_test_function function;
} sxml_test_case;

typedef struct sxml_test_suite {
    /* NUL-terminated name, usually the module under test. */
    const char *name;
    /* Borrowed array of case_count cases, run in order. */
    const sxml_test_case *cases;
    size_t case_count;
} sxml_test_suite;

/* The arguments of main, passed through unchanged. */
typedef struct sxml_test_command_line {
    int argument_count;
    char **arguments;
} sxml_test_command_line;

/* The outcome of one check, as recorded by SXML_TEST_EXPECT. */
typedef struct sxml_test_check {
    bool passed;
    /* NUL-terminated source text of the checked expression. */
    const char *expression;
    /* NUL-terminated source file name and line of the check. */
    const char *file_name;
    uint32_t line_number;
} sxml_test_check;

/*
 * Runs the selected cases of suite and reports each one. Returns the process
 * exit status for main: 0 when every selected case passed, 1 otherwise
 * (including when an argument names no case).
 */
int sxml_test_run_suite(const sxml_test_suite *suite,
                        sxml_test_command_line command_line);

/*
 * Records the outcome of a check in the running case and reports a failure.
 * Returns check.passed so callers can stop the case early.
 */
bool sxml_test_record_check(sxml_test_state *state, sxml_test_check check);

/* Number of elements of a test case array. */
#define SXML_TEST_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define SXML_TEST_CHECK_OF(condition)                                         \
    ((sxml_test_check){                                                       \
        .passed = (bool)(condition),                                          \
        .expression = #condition,                                             \
        .file_name = __FILE__,                                                \
        .line_number = (uint32_t)__LINE__,                                    \
    })

/* Checks condition; on failure records it and the case continues. */
#define SXML_TEST_EXPECT(state, condition)                                    \
    ((void)sxml_test_record_check((state), SXML_TEST_CHECK_OF(condition)))

/* Checks condition; on failure records it and returns from the case. */
#define SXML_TEST_REQUIRE(state, condition)                                   \
    do {                                                                      \
        if (!sxml_test_record_check((state),                                  \
                                    SXML_TEST_CHECK_OF(condition))) {         \
            return;                                                           \
        }                                                                     \
    } while (false)
