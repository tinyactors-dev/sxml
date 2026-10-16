#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * Output streams and writers for command-line programs (docs/CLI.md, D44).
 *
 * The CLI framework never prints: everything it writes goes through an
 * sxml_cli_output_stream, an interface the program supplies. The sxml
 * program's default implementation over standard output and standard error is
 * in apps/sxml/platform_stdio.c; the test double is the memory stream below,
 * which captures what is written so tests can compare it.
 *
 * An sxml_cli_writer wraps a stream and remembers the first failure, so code
 * that writes many pieces checks once at the end instead of after each piece.
 *
 * Thread safety: a stream, a writer and a memory stream are each used by one
 * thread at a time.
 */

/*
 * The operations of an output stream. Tables are static const; per-stream
 * data lives in the state pointer.
 */
typedef struct sxml_cli_output_stream_operations {
    /* Writes all of bytes and returns SXML_STATUS_OK, or returns the
       failure that stopped the write: SXML_STATUS_CAPACITY_EXCEEDED for a
       full memory stream, SXML_STATUS_INVALID_ARGUMENT when the platform
       refused the write (a closed pipe, a full disk). bytes is borrowed for
       the call. Never called with an empty view. INVALID_ARGUMENT stands in
       for an input/output status until one exists (docs/DECISIONS.md, P1). */
    sxml_status (*write)(void *state, sxml_byte_view bytes);
} sxml_cli_output_stream_operations;

/*
 * An output stream: state plus operations, passed by value. The state must
 * outlive every use of the stream.
 */
typedef struct sxml_cli_output_stream {
    void *state;
    const sxml_cli_output_stream_operations *operations;
} sxml_cli_output_stream;

/*
 * Writes bytes to stream. Writing an empty view does nothing and succeeds.
 * Returns what the stream's write operation returns.
 */
SXML_NODISCARD sxml_status sxml_cli_output_stream_write(
    sxml_cli_output_stream stream, sxml_byte_view bytes);

/* ------------------------------------------------------------- Writer */

/*
 * A stream plus the status of the first write that failed. After a failure
 * the writer writes nothing more. A plain value type: create it with
 * sxml_cli_writer_from_stream and pass it by pointer.
 */
typedef struct sxml_cli_writer {
    sxml_cli_output_stream stream;
    /* SXML_STATUS_OK until a write fails; then the status of that write. */
    sxml_status status;
} sxml_cli_writer;

/* A writer over stream that has not failed. */
sxml_cli_writer sxml_cli_writer_from_stream(sxml_cli_output_stream stream);

/* Writes text as it is. */
void sxml_cli_writer_write(sxml_cli_writer *writer, sxml_byte_view text);

/* Writes text followed by a line feed. */
void sxml_cli_writer_write_line(sxml_cli_writer *writer, sxml_byte_view text);

/* Writes one byte. */
void sxml_cli_writer_write_byte(sxml_cli_writer *writer, uint8_t byte);

/* Writes space_count spaces. */
void sxml_cli_writer_write_spaces(sxml_cli_writer *writer, size_t space_count);

/* Writes value in decimal, without sign or leading zeros. */
void sxml_cli_writer_write_decimal(sxml_cli_writer *writer, size_t value);

/*
 * Writes text for showing a user's argument in a diagnostic. Control bytes
 * (below 0x20, and 0x7F) are written as \xHH so an argument cannot move the
 * cursor or change the terminal's state; every other byte is written as it
 * is.
 */
void sxml_cli_writer_write_escaped(sxml_cli_writer *writer,
                                   sxml_byte_view text);

/* Writes text as sxml_cli_writer_write_escaped does, between single quotes. */
void sxml_cli_writer_write_quoted(sxml_cli_writer *writer, sxml_byte_view text);

/*
 * Writes text as a JSON string with its quotes (RFC 8259, section 7): '"'
 * and '\' are escaped, control bytes below 0x20 are written as \n, \r, \t,
 * \b, \f or \u00HH; every other byte is written as it is, so UTF-8 text
 * stays UTF-8. text should be well-formed UTF-8.
 */
void sxml_cli_writer_write_json_string(sxml_cli_writer *writer,
                                       sxml_byte_view text);

/* -------------------------------------------------------- Memory stream */

/*
 * An output stream that keeps what is written in memory, up to a fixed
 * capacity: the test double of the output stream interface. A write that
 * does not fit writes nothing and fails with SXML_STATUS_CAPACITY_EXCEEDED,
 * so a capacity of 0 gives a stream on which every write fails.
 */
typedef struct sxml_cli_memory_stream sxml_cli_memory_stream;

typedef struct sxml_cli_memory_stream_configuration {
    /* Most bytes the stream keeps. */
    size_t capacity_byte_count;
} sxml_cli_memory_stream_configuration;

/*
 * Writes the memory requirements of a memory stream, including its storage
 * for capacity_byte_count bytes. Returns SXML_STATUS_ARITHMETIC_OVERFLOW
 * when they do not fit in size_t.
 */
SXML_NODISCARD sxml_status sxml_cli_memory_stream_memory_requirements(
    const sxml_cli_memory_stream_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a memory stream in memory and writes its address to *memory_stream.
 * Returns the failures of sxml_memory_view_check_requirements.
 */
SXML_NODISCARD sxml_status sxml_cli_memory_stream_initialize(
    sxml_memory_view memory,
    const sxml_cli_memory_stream_configuration *configuration,
    sxml_cli_memory_stream **memory_stream);

/* Does nothing: a memory stream acquires nothing. Accepts NULL. */
void sxml_cli_memory_stream_finalize(sxml_cli_memory_stream *memory_stream);

/*
 * Allocates a memory stream through allocator, which is stored and must
 * outlive the stream, and initializes it.
 */
SXML_NODISCARD sxml_status sxml_cli_memory_stream_create(
    sxml_allocator allocator,
    const sxml_cli_memory_stream_configuration *configuration,
    sxml_cli_memory_stream **memory_stream);

/* Releases a memory stream made by create. Accepts NULL. */
void sxml_cli_memory_stream_destroy(sxml_cli_memory_stream *memory_stream);

/* The stream interface of memory_stream, valid until it is finalized. */
sxml_cli_output_stream sxml_cli_memory_stream_as_output_stream(
    sxml_cli_memory_stream *memory_stream);

/*
 * The bytes written so far. The view points into memory_stream and is valid
 * until the next write, clear or finalize.
 */
sxml_byte_view sxml_cli_memory_stream_contents(
    const sxml_cli_memory_stream *memory_stream);

/* Forgets everything written so far. */
void sxml_cli_memory_stream_clear(sxml_cli_memory_stream *memory_stream);
