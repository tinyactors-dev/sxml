/*
 * Output streams, writers and the memory stream (cli_output.h). Imitates the
 * golden module src/memory.c and src/allocator.c for the lifecycle functions.
 */
#include "cli_output.h"

#include <stdalign.h>
#include <stdbool.h>

/* ------------------------------------------------------------- Streams */

sxml_status sxml_cli_output_stream_write(sxml_cli_output_stream stream,
                                         sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    return stream.operations->write(stream.state, bytes);
}

/* ------------------------------------------------------------- Writer */

sxml_cli_writer sxml_cli_writer_from_stream(sxml_cli_output_stream stream) {
    return (sxml_cli_writer){.stream = stream, .status = SXML_STATUS_OK};
}

void sxml_cli_writer_write(sxml_cli_writer *writer, sxml_byte_view text) {
    if (writer->status != SXML_STATUS_OK) {
        return;
    }
    writer->status = sxml_cli_output_stream_write(writer->stream, text);
}

void sxml_cli_writer_write_line(sxml_cli_writer *writer, sxml_byte_view text) {
    sxml_cli_writer_write(writer, text);
    sxml_cli_writer_write(writer, SXML_BYTE_VIEW_FROM_LITERAL("\n"));
}

void sxml_cli_writer_write_byte(sxml_cli_writer *writer, uint8_t byte) {
    sxml_cli_writer_write(writer,
                          (sxml_byte_view){.bytes = &byte, .byte_count = 1});
}

void sxml_cli_writer_write_spaces(sxml_cli_writer *writer,
                                  size_t space_count) {
    static const sxml_byte_view spaces =
        SXML_BYTE_VIEW_INITIALIZER("                ");
    size_t remaining_count = space_count;
    while (remaining_count > 0) {
        const size_t chunk_count = remaining_count < spaces.byte_count
                                       ? remaining_count
                                       : spaces.byte_count;
        sxml_cli_writer_write(
            writer,
            (sxml_byte_view){.bytes = spaces.bytes, .byte_count = chunk_count});
        remaining_count -= chunk_count;
    }
}

void sxml_cli_writer_write_decimal(sxml_cli_writer *writer, size_t value) {
    /* 20 digits hold SIZE_MAX of a 64-bit size_t. */
    uint8_t digits[24];
    size_t digit_offset = sizeof digits;
    size_t remaining_value = value;
    do {
        digit_offset -= 1;
        digits[digit_offset] = (uint8_t)('0' + (remaining_value % 10U));
        remaining_value /= 10U;
    } while (remaining_value > 0);
    sxml_cli_writer_write(writer,
                          (sxml_byte_view){
                              .bytes = digits + digit_offset,
                              .byte_count = sizeof digits - digit_offset,
                          });
}

static const uint8_t sxml_hexadecimal_digits[] = "0123456789ABCDEF";

static bool sxml_byte_is_control(uint8_t byte) {
    return byte < 0x20U || byte == 0x7FU;
}

/* Writes text[run_start, run_end) in one piece. */
typedef struct sxml_byte_run {
    size_t start_offset;
    size_t end_offset;
} sxml_byte_run;

static void sxml_cli_writer_write_run(sxml_cli_writer *writer,
                                      sxml_byte_view text, sxml_byte_run run) {
    if (run.end_offset > run.start_offset) {
        sxml_cli_writer_write(writer,
                              (sxml_byte_view){
                                  .bytes = text.bytes + run.start_offset,
                                  .byte_count = run.end_offset - run.start_offset,
                              });
    }
}

static void sxml_cli_writer_write_hexadecimal_byte(sxml_cli_writer *writer,
                                                   uint8_t byte) {
    sxml_cli_writer_write_byte(writer, sxml_hexadecimal_digits[byte >> 4U]);
    sxml_cli_writer_write_byte(writer, sxml_hexadecimal_digits[byte & 0x0FU]);
}

void sxml_cli_writer_write_escaped(sxml_cli_writer *writer,
                                   sxml_byte_view text) {
    size_t run_start_offset = 0;
    for (size_t byte_offset = 0; byte_offset < text.byte_count;
         byte_offset += 1) {
        const uint8_t byte = text.bytes[byte_offset];
        if (!sxml_byte_is_control(byte)) {
            continue;
        }
        sxml_cli_writer_write_run(writer, text,
                                  (sxml_byte_run){
                                      .start_offset = run_start_offset,
                                      .end_offset = byte_offset,
                                  });
        sxml_cli_writer_write(writer, SXML_BYTE_VIEW_FROM_LITERAL("\\x"));
        sxml_cli_writer_write_hexadecimal_byte(writer, byte);
        run_start_offset = byte_offset + 1;
    }
    sxml_cli_writer_write_run(writer, text,
                              (sxml_byte_run){
                                  .start_offset = run_start_offset,
                                  .end_offset = text.byte_count,
                              });
}

void sxml_cli_writer_write_quoted(sxml_cli_writer *writer,
                                  sxml_byte_view text) {
    sxml_cli_writer_write_byte(writer, '\'');
    sxml_cli_writer_write_escaped(writer, text);
    sxml_cli_writer_write_byte(writer, '\'');
}

/*
 * The escape of one byte in a JSON string (RFC 8259, section 7), or an empty
 * view for a byte written as it is. \u00HH escapes are written by the caller.
 */
static sxml_byte_view sxml_json_short_escape(uint8_t byte) {
    switch (byte) {
    case '"':
        return SXML_BYTE_VIEW_FROM_LITERAL("\\\"");
    case '\\':
        return SXML_BYTE_VIEW_FROM_LITERAL("\\\\");
    case '\n':
        return SXML_BYTE_VIEW_FROM_LITERAL("\\n");
    case '\r':
        return SXML_BYTE_VIEW_FROM_LITERAL("\\r");
    case '\t':
        return SXML_BYTE_VIEW_FROM_LITERAL("\\t");
    case '\b':
        return SXML_BYTE_VIEW_FROM_LITERAL("\\b");
    case '\f':
        return SXML_BYTE_VIEW_FROM_LITERAL("\\f");
    default:
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
}

void sxml_cli_writer_write_json_string(sxml_cli_writer *writer,
                                       sxml_byte_view text) {
    sxml_cli_writer_write_byte(writer, '"');
    size_t run_start_offset = 0;
    for (size_t byte_offset = 0; byte_offset < text.byte_count;
         byte_offset += 1) {
        const uint8_t byte = text.bytes[byte_offset];
        const sxml_byte_view short_escape = sxml_json_short_escape(byte);
        /* RFC 8259 requires escaping below 0x20 only; DEL stays as it is. */
        const bool needs_escape = short_escape.byte_count > 0 || byte < 0x20U;
        if (!needs_escape) {
            continue;
        }
        sxml_cli_writer_write_run(writer, text,
                                  (sxml_byte_run){
                                      .start_offset = run_start_offset,
                                      .end_offset = byte_offset,
                                  });
        if (short_escape.byte_count > 0) {
            sxml_cli_writer_write(writer, short_escape);
        } else {
            sxml_cli_writer_write(writer, SXML_BYTE_VIEW_FROM_LITERAL("\\u00"));
            sxml_cli_writer_write_hexadecimal_byte(writer, byte);
        }
        run_start_offset = byte_offset + 1;
    }
    sxml_cli_writer_write_run(writer, text,
                              (sxml_byte_run){
                                  .start_offset = run_start_offset,
                                  .end_offset = text.byte_count,
                              });
    sxml_cli_writer_write_byte(writer, '"');
}

/* -------------------------------------------------------- Memory stream */

struct sxml_cli_memory_stream {
    /* The storage after this struct, capacity_byte_count bytes. */
    uint8_t *storage;
    size_t capacity_byte_count;
    size_t written_byte_count;
    /* The allocator create used; the null allocator after initialize. */
    sxml_allocator owning_allocator;
};

sxml_status sxml_cli_memory_stream_memory_requirements(
    const sxml_cli_memory_stream_configuration *configuration,
    sxml_memory_requirements *requirements) {
    size_t byte_count = 0;
    const sxml_status status =
        sxml_size_add(sizeof(struct sxml_cli_memory_stream),
                      configuration->capacity_byte_count, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = byte_count,
        .alignment = alignof(struct sxml_cli_memory_stream),
    };
    return SXML_STATUS_OK;
}

sxml_status sxml_cli_memory_stream_initialize(
    sxml_memory_view memory,
    const sxml_cli_memory_stream_configuration *configuration,
    sxml_cli_memory_stream **memory_stream) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_cli_memory_stream_memory_requirements(
        configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* memory_view_check_requirements has checked size and alignment. */
    void *placement = memory.bytes;
    sxml_cli_memory_stream *new_memory_stream = placement;
    *new_memory_stream = (sxml_cli_memory_stream){
        .storage = memory.bytes + sizeof(struct sxml_cli_memory_stream),
        .capacity_byte_count = configuration->capacity_byte_count,
        .written_byte_count = 0,
        .owning_allocator = {.state = NULL, .operations = NULL},
    };
    *memory_stream = new_memory_stream;
    return SXML_STATUS_OK;
}

void sxml_cli_memory_stream_finalize(sxml_cli_memory_stream *memory_stream) {
    /* The storage is part of the stream's own memory. */
    (void)memory_stream;
}

sxml_status sxml_cli_memory_stream_create(
    sxml_allocator allocator,
    const sxml_cli_memory_stream_configuration *configuration,
    sxml_cli_memory_stream **memory_stream) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_cli_memory_stream_memory_requirements(
        configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(allocator,
                                     (sxml_allocation_request){
                                         .byte_count = requirements.byte_count,
                                         .alignment = requirements.alignment,
                                     },
                                     &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_cli_memory_stream *new_memory_stream = NULL;
    status = sxml_cli_memory_stream_initialize(
        (sxml_memory_view){
            .bytes = memory,
            .byte_count = requirements.byte_count,
        },
        configuration, &new_memory_stream);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_memory_stream->owning_allocator = allocator;
    *memory_stream = new_memory_stream;
    return SXML_STATUS_OK;
}

void sxml_cli_memory_stream_destroy(sxml_cli_memory_stream *memory_stream) {
    if (memory_stream == NULL) {
        return;
    }
    const sxml_allocator owning_allocator = memory_stream->owning_allocator;
    sxml_cli_memory_stream_finalize(memory_stream);
    sxml_allocator_release(owning_allocator, memory_stream);
}

static sxml_status sxml_cli_memory_stream_write_operation(
    void *state, sxml_byte_view bytes) {
    sxml_cli_memory_stream *memory_stream = state;
    const size_t free_byte_count =
        memory_stream->capacity_byte_count - memory_stream->written_byte_count;
    if (bytes.byte_count > free_byte_count) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    /* A copy loop: the memory stream is a test double, and memcpy trips
       clang-tidy's insecure-API check without a bounds-checked variant on
       every platform. */
    uint8_t *destination =
        memory_stream->storage + memory_stream->written_byte_count;
    for (size_t byte_index = 0; byte_index < bytes.byte_count;
         byte_index += 1) {
        destination[byte_index] = bytes.bytes[byte_index];
    }
    memory_stream->written_byte_count += bytes.byte_count;
    return SXML_STATUS_OK;
}

static const sxml_cli_output_stream_operations
    sxml_cli_memory_stream_operations = {
        .write = sxml_cli_memory_stream_write_operation,
};

sxml_cli_output_stream sxml_cli_memory_stream_as_output_stream(
    sxml_cli_memory_stream *memory_stream) {
    return (sxml_cli_output_stream){
        .state = memory_stream,
        .operations = &sxml_cli_memory_stream_operations,
    };
}

sxml_byte_view sxml_cli_memory_stream_contents(
    const sxml_cli_memory_stream *memory_stream) {
    return (sxml_byte_view){
        .bytes = memory_stream->storage,
        .byte_count = memory_stream->written_byte_count,
    };
}

void sxml_cli_memory_stream_clear(sxml_cli_memory_stream *memory_stream) {
    memory_stream->written_byte_count = 0;
}
