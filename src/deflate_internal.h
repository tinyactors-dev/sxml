#pragma once

/*
 * DEFLATE (RFC 1951), sxml's own: the compressed data format of the EXI
 * compression alignment (EXI 1.0, section 9.3; glossary deflate; D333).
 * Raw streams: no zlib or gzip wrapper.
 */
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/* A stream to inflate. */
typedef struct sxml_inflate_request {
    /* Starts with one DEFLATE stream; bytes after its final block are not
       read (EXI writes several streams back to back). Borrowed. */
    sxml_byte_view input;
    /* Gives the output's memory. */
    sxml_allocator allocator;
    /* The most bytes the stream may inflate to; past it the call fails
       with SXML_STATUS_LIMIT_EXCEEDED (a bound on untrusted input). */
    size_t maximum_output_byte_count;
} sxml_inflate_request;

typedef struct sxml_inflate_result {
    /* The inflated bytes, allocated from the request's allocator, owned by
       the caller (release bytes with that allocator; NULL when empty). */
    sxml_memory_view output;
    /* The bytes of input the stream took, up to and including the byte
       of its final block's end (the rest of that byte's bits unused). */
    size_t consumed_byte_count;
} sxml_inflate_result;

/*
 * Inflates the DEFLATE stream at the start of request->input (RFC 1951:
 * stored, fixed Huffman and dynamic Huffman blocks, up to the block marked
 * final).
 *
 * Returns SXML_STATUS_MALFORMED_INPUT for a stream RFC 1951 does not allow
 * (block type 3, a stored block whose NLEN is not the complement of LEN,
 * code lengths that over-subscribe or (but for a single distance code)
 * leave incomplete a Huffman code, a repeat of lengths with no previous
 * length, a length or distance code that does not exist, a distance past
 * the start of the output), SXML_STATUS_UNEXPECTED_END_OF_INPUT when the
 * input ends before the final block does, SXML_STATUS_LIMIT_EXCEEDED past
 * maximum_output_byte_count, and the allocator's failures. On failure
 * *result is untouched and nothing is left allocated. Never reads outside
 * request->input.
 */
SXML_NODISCARD sxml_status sxml_inflate(const sxml_inflate_request *request, sxml_inflate_result *result);

/* Bytes to deflate. */
typedef struct sxml_deflate_request {
    /* Borrowed. */
    sxml_byte_view input;
    /* Gives the output's memory and the working memory. */
    sxml_allocator allocator;
} sxml_deflate_request;

/*
 * Deflates request->input as one DEFLATE stream ending in a final block,
 * and writes it, allocated from request->allocator and owned by the caller,
 * to *output: LZ77 over the 32 KiB window and dynamic or fixed Huffman
 * blocks, or stored blocks where those are smaller. The same input gives
 * the same output. Returns the allocator's failures; *output is then
 * untouched.
 */
SXML_NODISCARD sxml_status sxml_deflate(const sxml_deflate_request *request, sxml_memory_view *output);
