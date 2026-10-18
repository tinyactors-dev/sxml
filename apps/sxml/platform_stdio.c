/*
 * The platform layer over ISO C stdio (platform.h). The only file in apps/
 * allowed to call stdio functions (tools/ast-grep/rules/
 * input-output-outside-platform-layer.yml).
 */
#include "platform.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

sxml_status sxml_platform_prepare_streams(void) {
#if defined(_WIN32)
    /* The C runtime translates "\n" to "\r\n" in text mode; sxml writes
       bytes exactly as given, on every platform. */
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
#endif
    return SXML_STATUS_OK;
}

static sxml_status sxml_platform_write_operation(void *state,
                                                 sxml_byte_view bytes) {
    FILE *file = state;
    const size_t written_byte_count =
        fwrite(bytes.bytes, 1, bytes.byte_count, file);
    return written_byte_count == bytes.byte_count
               ? SXML_STATUS_OK
               : SXML_STATUS_INPUT_OUTPUT_FAILED;
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
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return SXML_STATUS_OK;
}

/* Reads all of file into memory from allocator, growing by doubling. */
static sxml_status sxml_platform_read_stream(FILE *file,
                                             sxml_allocator allocator,
                                             sxml_byte_view *contents) {
    size_t capacity = 0;
    size_t byte_count = 0;
    uint8_t *bytes = NULL;
    for (;;) {
        if (byte_count == capacity) {
            size_t grown_capacity = 0;
            sxml_status status = sxml_size_multiply(
                capacity == 0 ? 4096 : capacity, capacity == 0 ? 1 : 2,
                &grown_capacity);
            void *grown = NULL;
            if (status == SXML_STATUS_OK) {
                status = sxml_allocator_allocate(
                    allocator,
                    (sxml_allocation_request){.byte_count = grown_capacity,
                                              .alignment = 1},
                    &grown);
            }
            if (status != SXML_STATUS_OK) {
                sxml_allocator_release(allocator, bytes);
                return status;
            }
            if (byte_count != 0) {
                memcpy(grown, bytes, byte_count);
            }
            sxml_allocator_release(allocator, bytes);
            bytes = grown;
            capacity = grown_capacity;
        }
        const size_t read_count =
            fread(bytes + byte_count, 1, capacity - byte_count, file);
        byte_count += read_count;
        if (read_count == 0) {
            break;
        }
    }
    if (ferror(file) != 0) {
        sxml_allocator_release(allocator, bytes);
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    *contents = (sxml_byte_view){.bytes = bytes, .byte_count = byte_count};
    return SXML_STATUS_OK;
}

static sxml_status sxml_platform_read_file(
    void *state, const sxml_cli_file_read_request *request,
    sxml_byte_view *contents) {
    (void)state;
    if (sxml_byte_view_equals(request->path, SXML_BYTE_VIEW_FROM_LITERAL("-"))) {
        return sxml_platform_read_stream(stdin, request->allocator, contents);
    }
    /* fopen needs a NUL-terminated name; a name with a NUL byte names no
       file. */
    if (request->path.byte_count == 0 ||
        memchr(request->path.bytes, 0, request->path.byte_count) != NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        request->allocator,
        (sxml_allocation_request){.byte_count = request->path.byte_count + 1,
                                  .alignment = alignof(char)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    char *name = memory;
    memcpy(name, request->path.bytes, request->path.byte_count);
    name[request->path.byte_count] = '\0';
    FILE *file = fopen(name, "rb");
    sxml_allocator_release(request->allocator, memory);
    if (file == NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    status = sxml_platform_read_stream(file, request->allocator, contents);
    if (fclose(file) != 0 && status == SXML_STATUS_OK) {
        sxml_allocator_release(request->allocator, (void *)contents->bytes);
        status = SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return status;
}

static const sxml_cli_file_reader_operations sxml_platform_file_reader_operations = {
    .read_file = sxml_platform_read_file,
};

sxml_cli_file_reader sxml_platform_file_reader(void) {
    return (sxml_cli_file_reader){
        .state = NULL,
        .operations = &sxml_platform_file_reader_operations,
    };
}

void sxml_platform_read_working_directory(sxml_memory_view memory, sxml_byte_view *path) {
    *path = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    if (memory.byte_count < 2 || memory.byte_count > (size_t)INT32_MAX) {
        return;
    }
    char *text = (char *)memory.bytes;
#if defined(_WIN32)
    if (_getcwd(text, (int)memory.byte_count) == NULL) {
        return;
    }
#else
    if (getcwd(text, memory.byte_count) == NULL) {
        return;
    }
#endif
    size_t byte_count = strlen(text);
    for (size_t byte_index = 0; byte_index < byte_count; byte_index += 1) {
        if (text[byte_index] == '\\') {
            text[byte_index] = '/';
        }
    }
    *path = (sxml_byte_view){.bytes = memory.bytes, .byte_count = byte_count};
}
