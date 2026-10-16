#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * Serialization: a sequence to bytes (Serialization 4.0;
 * docs/DECISIONS.md, D47, D51).
 *
 * Bytes go to an sxml_output_stream, an interface the host implements over
 * its own output (a file, standard output, a JavaScript callback);
 * sxml_output_buffer is the built-in one, collecting the bytes in memory.
 * Output is always UTF-8, without a byte order mark.
 *
 * Phase 1 has one method, the adaptive method (Serialization 4.0,
 * "Adaptive Output Method"): every kind of item has a form, so any result
 * can be shown. Phase 3 adds the XML, JSON and text methods to
 * sxml_serialization_method, with their parameters.
 *
 * Thread safety: the thread of the item heap whose values are serialized
 * (D35).
 */

/* ----------------------------------------------------------- Output stream */

/* The operations of an output stream. Tables are static const. */
typedef struct sxml_output_stream_operations {
    /* Writes all of bytes, after everything written before. Returns
       SXML_STATUS_INPUT_OUTPUT_FAILED when the platform refuses the write
       (D50). A failure stops the serialization, which returns the status
       unchanged. */
    sxml_status (*write)(void *state, sxml_byte_view bytes);
} sxml_output_stream_operations;

/* The output stream interface, passed by value. */
typedef struct sxml_output_stream {
    void *state;
    const sxml_output_stream_operations *operations;
} sxml_output_stream;

/*
 * Writes bytes to output_stream. Writing no bytes does nothing. Returns
 * SXML_STATUS_INVALID_ARGUMENT when output_stream has no operations, and
 * the stream's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_output_stream_write(
    sxml_output_stream output_stream, sxml_byte_view bytes);

/* ----------------------------------------------------------- Output buffer */

/*
 * An output stream that collects what is written in memory, growing
 * geometrically through its allocator: for tests, for fn:serialize, and for
 * hosts that want the bytes at once (the WebAssembly package).
 *
 * Thread safety: one thread at a time.
 */
typedef struct sxml_output_buffer sxml_output_buffer;

typedef struct sxml_output_buffer_configuration {
    /* Provides the memory for the bytes. Must not be the null allocator;
       must outlive the buffer. */
    sxml_allocator allocator;
    /* Most bytes the buffer may hold; 0 means no bound. A write that would
       pass it gives SXML_STATUS_LIMIT_EXCEEDED and writes nothing. */
    size_t maximum_byte_count;
} sxml_output_buffer_configuration;

/*
 * Writes the memory requirements of an output buffer, without the bytes
 * it holds, to *requirements. Returns SXML_STATUS_INVALID_ARGUMENT for an
 * invalid configuration.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_output_buffer_memory_requirements(
    const sxml_output_buffer_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an empty output buffer in memory and writes its address to
 * *output_buffer. memory must stay valid, unmoved, until
 * sxml_output_buffer_finalize. Allocates nothing until the first write.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_output_buffer_initialize(
    sxml_memory_view memory,
    const sxml_output_buffer_configuration *configuration,
    sxml_output_buffer **output_buffer);

/* Releases the bytes. Does nothing when output_buffer is NULL. */
SXML_EXPORT void sxml_output_buffer_finalize(sxml_output_buffer *output_buffer);

/*
 * Allocates an output buffer through allocator and initializes it.
 * allocator is stored and must outlive the buffer. Returns the failures of
 * sxml_output_buffer_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_output_buffer_create(
    sxml_allocator allocator,
    const sxml_output_buffer_configuration *configuration,
    sxml_output_buffer **output_buffer);

/*
 * Finalizes an output buffer made by sxml_output_buffer_create and releases
 * its memory. Does nothing when output_buffer is NULL.
 */
SXML_EXPORT void sxml_output_buffer_destroy(sxml_output_buffer *output_buffer);

/*
 * The output stream that appends to output_buffer. Its write returns
 * SXML_STATUS_LIMIT_EXCEEDED at maximum_byte_count, the allocator's
 * failures and SXML_STATUS_ARITHMETIC_OVERFLOW, leaving the buffer
 * unchanged. Valid until output_buffer is finalized.
 */
SXML_EXPORT sxml_output_stream sxml_output_buffer_as_output_stream(
    sxml_output_buffer *output_buffer);

/*
 * The bytes written so far, in order. Borrowed: valid until the next write,
 * sxml_output_buffer_clear or finalization.
 */
SXML_EXPORT sxml_byte_view sxml_output_buffer_get_bytes(
    const sxml_output_buffer *output_buffer);

/* Forgets the bytes written, keeping the memory for reuse. */
SXML_EXPORT void sxml_output_buffer_clear(sxml_output_buffer *output_buffer);

/* ----------------------------------------------------------- Serialization */

/*
 * The output methods (Serialization 4.0, section "Serialization
 * Parameters", parameter method). The list only grows.
 */
typedef enum sxml_serialization_method {
    /* Serialization 4.0, "Adaptive Output Method": items one after the
       other with the item separator between them; atomic items in a form
       that shows their type (strings quoted, xs:double with an exponent,
       other types as constructor calls such as xs:date("2026-10-16")),
       nodes as the XML method writes them, maps, arrays and function items
       in XPath-like syntax. */
    SXML_SERIALIZATION_METHOD_ADAPTIVE,
} sxml_serialization_method;

/*
 * The serialization parameters (Serialization 4.0, section "Serialization
 * Parameters") that Phase 1 supports. A zero-initialized value is the
 * adaptive method with its defaults. Later phases add fields; a field left
 * out of a designated initializer keeps its default.
 */
typedef struct sxml_serialization_parameters {
    sxml_serialization_method serialization_method;
    /* item-separator: when has_item_separator is false, the method's
       default (a newline, U+000A, for the adaptive method). UTF-8;
       borrowed for the call. */
    bool has_item_separator;
    sxml_byte_view item_separator;
} sxml_serialization_parameters;

/* A sequence to serialize. */
typedef struct sxml_serialization_request {
    /* Borrowed for the call. A lazy sequence is read to its end. */
    sxml_sequence value;
    sxml_serialization_parameters serialization_parameters;
    /* Receives a serialization error (SENR0001, SERE0014, ... for later
       methods) or an error raised while reading value; may be NULL. */
    sxml_error_record *error_record;
} sxml_serialization_request;

/*
 * Serializes request->value with request->serialization_parameters and
 * writes the bytes to output_stream, in pieces, as they are produced. The
 * item heap must be the one request->value lives in; it provides the names
 * and types to write.
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR with the error record filled for a
 * serialization error or an error raised by a lazy value,
 * SXML_STATUS_INVALID_ARGUMENT for invalid parameters (an item separator
 * that is not well-formed UTF-8), and the failures of output_stream and
 * of the heap. On failure part of the output may have been written.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_serialize_sequence(
    sxml_item_heap *item_heap, const sxml_serialization_request *request,
    sxml_output_stream output_stream);
