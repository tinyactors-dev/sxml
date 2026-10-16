#include "test_runner.h"

#include <stdio.h>
#include <string.h>

struct sxml_test_state {
    size_t failed_check_count;
};

bool sxml_test_record_check(sxml_test_state *state, sxml_test_check check) {
    if (!check.passed) {
        state->failed_check_count += 1;
        (void)printf("    %s:%lu: check failed: %s\n", check.file_name,
                     (unsigned long)check.line_number, check.expression);
    }
    return check.passed;
}

/* True when the command line names no cases (run all) or names this one. */
static bool sxml_test_case_is_selected(const sxml_test_case *test_case,
                                       sxml_test_command_line command_line) {
    if (command_line.argument_count <= 1) {
        return true;
    }
    for (int argument_index = 1; argument_index < command_line.argument_count;
         argument_index += 1) {
        if (strcmp(command_line.arguments[argument_index], test_case->name) ==
            0) {
            return true;
        }
    }
    return false;
}

/* True when the argument names a case of the suite. */
static bool sxml_test_suite_has_case(const sxml_test_suite *suite,
                                     const char *case_name) {
    for (size_t case_index = 0; case_index < suite->case_count;
         case_index += 1) {
        if (strcmp(suite->cases[case_index].name, case_name) == 0) {
            return true;
        }
    }
    return false;
}

int sxml_test_run_suite(const sxml_test_suite *suite,
                        sxml_test_command_line command_line) {
    size_t unknown_case_count = 0;
    for (int argument_index = 1; argument_index < command_line.argument_count;
         argument_index += 1) {
        const char *case_name = command_line.arguments[argument_index];
        if (!sxml_test_suite_has_case(suite, case_name)) {
            (void)printf("%s: no test case named '%s'\n", suite->name,
                         case_name);
            unknown_case_count += 1;
        }
    }

    size_t run_count = 0;
    size_t failed_case_count = 0;
    for (size_t case_index = 0; case_index < suite->case_count;
         case_index += 1) {
        const sxml_test_case *test_case = &suite->cases[case_index];
        if (!sxml_test_case_is_selected(test_case, command_line)) {
            continue;
        }
        sxml_test_state state = {
            .failed_check_count = 0,
        };
        test_case->function(&state);
        run_count += 1;
        if (state.failed_check_count == 0) {
            (void)printf("  pass  %s\n", test_case->name);
        } else {
            failed_case_count += 1;
            (void)printf("  FAIL  %s (%zu failed checks)\n", test_case->name,
                         state.failed_check_count);
        }
    }

    (void)printf("%s: %zu run, %zu passed, %zu failed\n", suite->name,
                 run_count, run_count - failed_case_count, failed_case_count);
    (void)fflush(stdout);
    return (failed_case_count == 0 && unknown_case_count == 0) ? 0 : 1;
}
