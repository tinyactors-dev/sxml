/*
 * Tests for include/sxml/status.h: status names and the error record.
 */
#include "sxml/memory.h"
#include "sxml/status.h"
#include "test_runner.h"

/* Every status, in declaration order. */
static const sxml_status every_status[] = {
    SXML_STATUS_OK,
    SXML_STATUS_INVALID_ARGUMENT,
    SXML_STATUS_INSUFFICIENT_MEMORY,
    SXML_STATUS_MISALIGNED_MEMORY,
    SXML_STATUS_CAPACITY_EXCEEDED,
    SXML_STATUS_LIMIT_EXCEEDED,
    SXML_STATUS_OUT_OF_MEMORY,
    SXML_STATUS_ARITHMETIC_OVERFLOW,
    SXML_STATUS_INDEX_OUT_OF_RANGE,
    SXML_STATUS_UNEXPECTED_END_OF_INPUT,
    SXML_STATUS_MALFORMED_INPUT,
    SXML_STATUS_INVALID_UTF8,
    SXML_STATUS_UNSUPPORTED_FEATURE,
    SXML_STATUS_INPUT_OUTPUT_FAILED,
    SXML_STATUS_STATIC_ERROR,
    SXML_STATUS_DYNAMIC_ERROR,
    SXML_STATUS_TYPE_ERROR,
};

static void test_ok_is_zero(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, SXML_STATUS_OK == 0);
}

static void test_status_name_spells_the_constant(sxml_test_state *state) {
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         sxml_status_name(SXML_STATUS_OK),
                         SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_OK")));
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(
                   sxml_status_name(SXML_STATUS_INVALID_UTF8),
                   SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_INVALID_UTF8")));
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         sxml_status_name(SXML_STATUS_INPUT_OUTPUT_FAILED),
                         SXML_BYTE_VIEW_FROM_LITERAL(
                             "SXML_STATUS_INPUT_OUTPUT_FAILED")));
    SXML_TEST_EXPECT(state, !sxml_status_has_error_record(
                                SXML_STATUS_INPUT_OUTPUT_FAILED));
}

static void test_every_status_has_a_distinct_name(sxml_test_state *state) {
    sxml_byte_view unknown_name =
        SXML_BYTE_VIEW_FROM_LITERAL("SXML_STATUS_UNKNOWN");
    for (size_t status_index = 0;
         status_index < SXML_TEST_COUNT(every_status); status_index += 1) {
        sxml_byte_view name = sxml_status_name(every_status[status_index]);
        SXML_TEST_EXPECT(state, !sxml_byte_view_equals(name, unknown_name));
        for (size_t other_index = status_index + 1;
             other_index < SXML_TEST_COUNT(every_status); other_index += 1) {
            SXML_TEST_EXPECT(state,
                             !sxml_byte_view_equals(
                                 name, sxml_status_name(
                                           every_status[other_index])));
        }
    }
}

static void test_only_specification_errors_have_error_records(
    sxml_test_state *state) {
    size_t error_record_status_count = 0;
    for (size_t status_index = 0;
         status_index < SXML_TEST_COUNT(every_status); status_index += 1) {
        if (sxml_status_has_error_record(every_status[status_index])) {
            error_record_status_count += 1;
        }
    }
    SXML_TEST_EXPECT(state, error_record_status_count == 3);
    SXML_TEST_EXPECT(state,
                     sxml_status_has_error_record(SXML_STATUS_STATIC_ERROR));
    SXML_TEST_EXPECT(state,
                     sxml_status_has_error_record(SXML_STATUS_DYNAMIC_ERROR));
    SXML_TEST_EXPECT(state,
                     sxml_status_has_error_record(SXML_STATUS_TYPE_ERROR));
}

/* A specification error needs only static text: no allocation (D21). */
static void test_error_record_holds_static_error_without_memory(
    sxml_test_state *state) {
    static const sxml_error_record division_by_zero = {
        .code =
            {
                .namespace_uri =
                    SXML_BYTE_VIEW_INITIALIZER(SXML_ERROR_NAMESPACE_URI),
                .local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0001"),
                .namespace_prefix = SXML_BYTE_VIEW_INITIALIZER("err"),
            },
        .value = NULL,
        .diagnostic =
            {
                .description = SXML_BYTE_VIEW_INITIALIZER("Division by zero"),
                .location =
                    {
                        .source_name = {.bytes = NULL, .byte_count = 0},
                        .line_number = 3,
                        .column_number = 14,
                        .byte_offset = 41,
                    },
            },
    };
    SXML_TEST_EXPECT(state, division_by_zero.code.local_name.byte_count == 8);
    SXML_TEST_EXPECT(state,
                     division_by_zero.code.namespace_uri.byte_count == 33);
    SXML_TEST_EXPECT(state, division_by_zero.value == NULL);
    SXML_TEST_EXPECT(state,
                     division_by_zero.diagnostic.location.line_number == 3);
}

static const sxml_test_case status_test_cases[] = {
    {.name = "ok_is_zero", .function = test_ok_is_zero},
    {.name = "status_name_spells_the_constant",
     .function = test_status_name_spells_the_constant},
    {.name = "every_status_has_a_distinct_name",
     .function = test_every_status_has_a_distinct_name},
    {.name = "only_specification_errors_have_error_records",
     .function = test_only_specification_errors_have_error_records},
    {.name = "error_record_holds_static_error_without_memory",
     .function = test_error_record_holds_static_error_without_memory},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "status",
            .cases = status_test_cases,
            .case_count = SXML_TEST_COUNT(status_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
