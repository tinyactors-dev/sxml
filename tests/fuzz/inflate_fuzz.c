/*
 * Fuzz target: sxml's DEFLATE (src/inflate.c, src/deflate.c; packet 8.16,
 * D333). The input is inflated with an output limit; when it inflates, the
 * output is deflated and must inflate back to itself. The input is also
 * deflated as it is, and that stream must inflate back to the input. Every
 * allocation goes through the fixture's counting allocator.
 *
 * Golden file: tests/fuzz/xml_parse_fuzz.c.
 */
#include "deflate_internal.h"
#include "fuzz_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deflates bytes and checks that they inflate back. */
static void inflate_fuzz_round_trip(sxml_allocator allocator, sxml_byte_view bytes) {
    sxml_memory_view stream = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_deflate(&(sxml_deflate_request){.input = bytes, .allocator = allocator}, &stream);
    sxml_fuzz_check_status(status, "sxml_deflate");
    if (status != SXML_STATUS_OK) {
        return;
    }
    sxml_inflate_result result = {.consumed_byte_count = 0};
    status = sxml_inflate(&(sxml_inflate_request){.input = {.bytes = stream.bytes, .byte_count = stream.byte_count},
                                                  .allocator = allocator,
                                                  .maximum_output_byte_count = bytes.byte_count},
                          &result);
    if (status == SXML_STATUS_OK &&
        (result.output.byte_count != bytes.byte_count ||
         (bytes.byte_count > 0 && memcmp(result.output.bytes, bytes.bytes, bytes.byte_count) != 0) ||
         result.consumed_byte_count != stream.byte_count)) {
        (void)fprintf(stderr, "deflate's stream does not inflate back to its input\n");
        abort();
    }
    if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
        (void)fprintf(stderr, "deflate's stream does not inflate: %d\n", (int)status);
        abort();
    }
    if (status == SXML_STATUS_OK) {
        sxml_allocator_release(allocator, result.output.bytes);
    }
    sxml_allocator_release(allocator, stream.bytes);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(&(sxml_fuzz_fixture_configuration){.has_function_library = false}, &fixture) !=
        SXML_STATUS_OK) {
        return 0;
    }
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(&fixture);
    const sxml_byte_view input = {.bytes = data, .byte_count = size};
    sxml_inflate_result result = {.consumed_byte_count = 0};
    const sxml_status status = sxml_inflate(
        &(sxml_inflate_request){.input = input, .allocator = allocator, .maximum_output_byte_count = 1U << 20U},
        &result);
    sxml_fuzz_check_status(status, "sxml_inflate");
    if (status == SXML_STATUS_OK) {
        if (result.consumed_byte_count > size) {
            (void)fprintf(stderr, "inflate consumed more than its input\n");
            abort();
        }
        inflate_fuzz_round_trip(allocator, (sxml_byte_view){.bytes = result.output.bytes, .byte_count = result.output.byte_count});
        sxml_allocator_release(allocator, result.output.bytes);
    }
    inflate_fuzz_round_trip(allocator, input);
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
