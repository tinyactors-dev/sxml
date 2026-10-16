/*
 * Tests for output streams, writers and the memory stream
 * (apps/cli_framework/cli_output.h).
 */
#include "cli_output.h"
#include "sxml/memory.h"
#include "test_runner.h"

#include <stdalign.h>
#include <stdint.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

enum { SXML_TEST_STORAGE_BYTE_COUNT = 1024 };

/* A memory stream in caller memory, for one test case. */
typedef struct captured_output {
    alignas(16) uint8_t storage[SXML_TEST_STORAGE_BYTE_COUNT];
    sxml_cli_memory_stream *memory_stream;
    sxml_cli_writer writer;
} captured_output;

static bool capture_output(captured_output *output,
                           size_t capacity_byte_count) {
    const sxml_status status = sxml_cli_memory_stream_initialize(
        (sxml_memory_view){.bytes = output->storage,
                           .byte_count = sizeof output->storage},
        &(sxml_cli_memory_stream_configuration){
            .capacity_byte_count = capacity_byte_count},
        &output->memory_stream);
    if (status != SXML_STATUS_OK) {
        return false;
    }
    output->writer = sxml_cli_writer_from_stream(
        sxml_cli_memory_stream_as_output_stream(output->memory_stream));
    return true;
}

static bool output_equals(const captured_output *output,
                          sxml_byte_view expected) {
    return sxml_byte_view_equals(
        sxml_cli_memory_stream_contents(output->memory_stream), expected);
}

static void test_writer_writes_text_lines_and_bytes(sxml_test_state *state) {
    captured_output output;
    SXML_TEST_REQUIRE(state, capture_output(&output, 256));
    sxml_cli_writer_write(&output.writer, TEXT("ab"));
    sxml_cli_writer_write(&output.writer, TEXT(""));
    sxml_cli_writer_write_byte(&output.writer, 'c');
    sxml_cli_writer_write_line(&output.writer, TEXT("d"));
    SXML_TEST_EXPECT(state, output.writer.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_equals(&output, TEXT("abcd\n")));
}

static void test_writer_writes_decimals(sxml_test_state *state) {
    captured_output output;
    SXML_TEST_REQUIRE(state, capture_output(&output, 256));
    sxml_cli_writer_write_decimal(&output.writer, 0);
    sxml_cli_writer_write_byte(&output.writer, ' ');
    sxml_cli_writer_write_decimal(&output.writer, 1234567890);
    sxml_cli_writer_write_byte(&output.writer, ' ');
    sxml_cli_writer_write_decimal(&output.writer, UINT32_MAX);
    SXML_TEST_EXPECT(state,
                     output_equals(&output, TEXT("0 1234567890 4294967295")));
}

static void test_writer_writes_spaces(sxml_test_state *state) {
    captured_output output;
    SXML_TEST_REQUIRE(state, capture_output(&output, 256));
    sxml_cli_writer_write_spaces(&output.writer, 0);
    sxml_cli_writer_write_byte(&output.writer, '|');
    /* More than the 16-space chunk the writer uses. */
    sxml_cli_writer_write_spaces(&output.writer, 20);
    sxml_cli_writer_write_byte(&output.writer, '|');
    SXML_TEST_EXPECT(state,
                     output_equals(&output, TEXT("|                    |")));
}

static void test_quoted_text_escapes_control_bytes(sxml_test_state *state) {
    captured_output output;
    SXML_TEST_REQUIRE(state, capture_output(&output, 256));
    sxml_cli_writer_write_quoted(&output.writer, TEXT("a\x1b[2Jb\n\x7f\xc3\xa9"));
    SXML_TEST_EXPECT(state, output_equals(&output, TEXT("'a\\x1B[2Jb\\x0A\\x7F\xc3\xa9'")));
}

static void test_quoted_empty_text(sxml_test_state *state) {
    captured_output output;
    SXML_TEST_REQUIRE(state, capture_output(&output, 256));
    sxml_cli_writer_write_quoted(&output.writer, TEXT(""));
    SXML_TEST_EXPECT(state, output_equals(&output, TEXT("''")));
}

static void test_json_string_escapes_per_rfc_8259(sxml_test_state *state) {
    captured_output output;
    SXML_TEST_REQUIRE(state, capture_output(&output, 256));
    sxml_cli_writer_write_json_string(
        &output.writer, TEXT("a\"b\\c\n\r\t\b\f\x01\x1f\x7f\xc3\xa9/"));
    SXML_TEST_EXPECT(
        state,
        output_equals(&output, TEXT("\"a\\\"b\\\\c\\n\\r\\t\\b\\f\\u0001\\u001F"
                                    "\x7f\xc3\xa9/\"")));
}

static void test_writer_stops_after_first_failure(sxml_test_state *state) {
    captured_output output;
    SXML_TEST_REQUIRE(state, capture_output(&output, 4));
    sxml_cli_writer_write(&output.writer, TEXT("abc"));
    sxml_cli_writer_write(&output.writer, TEXT("de"));
    SXML_TEST_EXPECT(state,
                     output.writer.status == SXML_STATUS_CAPACITY_EXCEEDED);
    /* "f" would fit, but a failed writer writes nothing more. */
    sxml_cli_writer_write(&output.writer, TEXT("f"));
    SXML_TEST_EXPECT(state, output_equals(&output, TEXT("abc")));
    SXML_TEST_EXPECT(state,
                     output.writer.status == SXML_STATUS_CAPACITY_EXCEEDED);
}

static void test_memory_stream_clear_forgets_contents(sxml_test_state *state) {
    captured_output output;
    SXML_TEST_REQUIRE(state, capture_output(&output, 4));
    sxml_cli_writer_write(&output.writer, TEXT("abcd"));
    sxml_cli_memory_stream_clear(output.memory_stream);
    SXML_TEST_EXPECT(state, output_equals(&output, TEXT("")));
    SXML_TEST_EXPECT(state, sxml_cli_output_stream_write(
                                sxml_cli_memory_stream_as_output_stream(
                                    output.memory_stream),
                                TEXT("wxyz")) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_equals(&output, TEXT("wxyz")));
}

static void test_memory_stream_requirements(sxml_test_state *state) {
    sxml_memory_requirements small = {.byte_count = 0, .alignment = 1};
    sxml_memory_requirements large = {.byte_count = 0, .alignment = 1};
    SXML_TEST_REQUIRE(state, sxml_cli_memory_stream_memory_requirements(
                                 &(sxml_cli_memory_stream_configuration){
                                     .capacity_byte_count = 10},
                                 &small) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_cli_memory_stream_memory_requirements(
                                 &(sxml_cli_memory_stream_configuration){
                                     .capacity_byte_count = 110},
                                 &large) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, large.byte_count == small.byte_count + 100);
    sxml_memory_requirements untouched = {.byte_count = 7, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_cli_memory_stream_memory_requirements(
                                &(sxml_cli_memory_stream_configuration){
                                    .capacity_byte_count = SIZE_MAX},
                                &untouched) ==
                                SXML_STATUS_ARITHMETIC_OVERFLOW);
    SXML_TEST_EXPECT(state, untouched.byte_count == 7);
}

static void test_memory_stream_rejects_small_memory(sxml_test_state *state) {
    alignas(16) uint8_t storage[16];
    sxml_cli_memory_stream *memory_stream = NULL;
    SXML_TEST_EXPECT(state, sxml_cli_memory_stream_initialize(
                                (sxml_memory_view){.bytes = storage,
                                                   .byte_count = sizeof storage},
                                &(sxml_cli_memory_stream_configuration){
                                    .capacity_byte_count = 64},
                                &memory_stream) ==
                                SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, memory_stream == NULL);
}

static void test_memory_stream_create_and_destroy(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_REQUIRE(state, sxml_counting_allocator_create(
                                 sxml_default_allocator(),
                                 &(sxml_counting_allocator_configuration){
                                     .backing_allocator =
                                         sxml_default_allocator(),
                                     .failing_allocation_number = 0},
                                 &counting_allocator) == SXML_STATUS_OK);
    sxml_cli_memory_stream *memory_stream = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_cli_memory_stream_create(
                          sxml_counting_allocator_as_allocator(counting_allocator),
                          &(sxml_cli_memory_stream_configuration){
                              .capacity_byte_count = 32},
                          &memory_stream) == SXML_STATUS_OK);
    sxml_cli_writer writer = sxml_cli_writer_from_stream(
        sxml_cli_memory_stream_as_output_stream(memory_stream));
    sxml_cli_writer_write(&writer, TEXT("kept"));
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         sxml_cli_memory_stream_contents(memory_stream),
                         TEXT("kept")));
    sxml_cli_memory_stream_destroy(memory_stream);
    sxml_cli_memory_stream_destroy(NULL);
    SXML_TEST_EXPECT(
        state,
        sxml_counting_allocator_read_counts(counting_allocator)
                .live_allocation_count == 0);
    sxml_counting_allocator_destroy(counting_allocator);
}

static const sxml_test_case cli_output_test_cases[] = {
    {.name = "writer_writes_text_lines_and_bytes",
     .function = test_writer_writes_text_lines_and_bytes},
    {.name = "writer_writes_decimals", .function = test_writer_writes_decimals},
    {.name = "writer_writes_spaces", .function = test_writer_writes_spaces},
    {.name = "quoted_text_escapes_control_bytes",
     .function = test_quoted_text_escapes_control_bytes},
    {.name = "quoted_empty_text", .function = test_quoted_empty_text},
    {.name = "json_string_escapes_per_rfc_8259",
     .function = test_json_string_escapes_per_rfc_8259},
    {.name = "writer_stops_after_first_failure",
     .function = test_writer_stops_after_first_failure},
    {.name = "memory_stream_clear_forgets_contents",
     .function = test_memory_stream_clear_forgets_contents},
    {.name = "memory_stream_requirements",
     .function = test_memory_stream_requirements},
    {.name = "memory_stream_rejects_small_memory",
     .function = test_memory_stream_rejects_small_memory},
    {.name = "memory_stream_create_and_destroy",
     .function = test_memory_stream_create_and_destroy},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "cli_output",
            .cases = cli_output_test_cases,
            .case_count = SXML_TEST_COUNT(cli_output_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
