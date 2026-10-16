/*
 * The platform layer over ISO C stdio (platform.h). The only file in apps/
 * allowed to call stdio functions (tools/ast-grep/rules/
 * input-output-outside-platform-layer.yml).
 */
#include "platform.h"

#include <stdbool.h>
#include <stdio.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

sxml_status sxml_platform_prepare_streams(void) {
#if defined(_WIN32)
    /* The C runtime translates "\n" to "\r\n" in text mode; sxml writes
       bytes exactly as given, on every platform. */
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
#endif
    return SXML_STATUS_OK;
}

static sxml_status sxml_platform_write_operation(void *state,
                                                 sxml_byte_view bytes) {
    FILE *file = state;
    const size_t written_byte_count =
        fwrite(bytes.bytes, 1, bytes.byte_count, file);
    /* No input/output status exists yet (docs/DECISIONS.md, P1). */
    return written_byte_count == bytes.byte_count
               ? SXML_STATUS_OK
               : SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_cli_output_stream_operations sxml_platform_file_operations = {
    .write = sxml_platform_write_operation,
};

sxml_cli_output_stream sxml_platform_standard_output(void) {
    return (sxml_cli_output_stream){
        .state = stdout,
        .operations = &sxml_platform_file_operations,
    };
}

sxml_cli_output_stream sxml_platform_standard_error(void) {
    return (sxml_cli_output_stream){
        .state = stderr,
        .operations = &sxml_platform_file_operations,
    };
}

sxml_status sxml_platform_flush_streams(void) {
    const bool output_failed = fflush(stdout) != 0 || ferror(stdout) != 0;
    const bool error_failed = fflush(stderr) != 0 || ferror(stderr) != 0;
    if (output_failed || error_failed) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}
