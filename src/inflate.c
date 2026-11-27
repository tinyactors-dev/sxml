/*
 * Inflate (RFC 1951; src/deflate_internal.h; D333): a reader of raw
 * DEFLATE streams, written from the RFC. A bit reader over the input view
 * that never reads past its end, canonical Huffman codes decoded through a
 * first-level table of SXML_INFLATE_FAST_BIT_COUNT bits with a bit-by-bit fallback for
 * longer codes (RFC 1951, section 3.2.2), and an output that grows by
 * doubling in the request's allocator up to the request's limit.
 *
 * Golden file: src/text_decoding.c (a self-contained byte-level module).
 */
#include "deflate_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    /* RFC 1951, section 3.2.5: literal/length codes 0 to 287 (286 and 287
       take part in the code but never occur), distance codes 0 to 31 (30
       and 31 likewise). */
    SXML_INFLATE_LITERAL_LENGTH_SYMBOL_COUNT = 288,
    SXML_INFLATE_DISTANCE_SYMBOL_COUNT = 32,
    /* RFC 1951, section 3.2.7: 19 code length codes. */
    SXML_INFLATE_CODE_LENGTH_SYMBOL_COUNT = 19,
    /* Huffman codes are at most 15 bits long (section 3.2.7). */
    SXML_INFLATE_MAXIMUM_CODE_BIT_COUNT = 15,
    /* The first-level table resolves codes of up to this many bits. */
    SXML_INFLATE_FAST_BIT_COUNT = 9,
    SXML_INFLATE_FAST_ENTRY_COUNT = 1 << SXML_INFLATE_FAST_BIT_COUNT,
    /* A table entry is (symbol << 4) | bit count; 0 means "a longer code". */
    SXML_INFLATE_FAST_ENTRY_BIT_COUNT_MASK = 0xF,
    SXML_INFLATE_END_OF_BLOCK_SYMBOL = 256,
    SXML_INFLATE_FIRST_LENGTH_SYMBOL = 257,
    SXML_INFLATE_LENGTH_CODE_COUNT = 29,
    SXML_INFLATE_DISTANCE_CODE_COUNT = 30,
    /* The first output buffer is this many times the input, at least
       SXML_INFLATE_MINIMUM_OUTPUT_CAPACITY bytes. */
    SXML_INFLATE_INITIAL_OUTPUT_RATIO = 4,
    SXML_INFLATE_MINIMUM_OUTPUT_CAPACITY = 256,
};

/* RFC 1951, section 3.2.5: the base length and extra bits of codes 257-285. */
static const uint16_t length_bases[SXML_INFLATE_LENGTH_CODE_COUNT] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195,
    227, 258};
static const uint8_t length_extra_bit_counts[SXML_INFLATE_LENGTH_CODE_COUNT] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
/* RFC 1951, section 3.2.5: the base distance and extra bits of codes 0-29. */
static const uint16_t distance_bases[SXML_INFLATE_DISTANCE_CODE_COUNT] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
    4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra_bit_counts[SXML_INFLATE_DISTANCE_CODE_COUNT] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
/* RFC 1951, section 3.2.7: the order in which code length code lengths come. */
static const uint8_t code_length_order[SXML_INFLATE_CODE_LENGTH_SYMBOL_COUNT] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* Reads bits least significant first (RFC 1951, section 3.1.1). Bytes
   enter bit_buffer whole, so the bytes of input not yet used are the
   bit_count / 8 bytes before byte_position. */
typedef struct bit_reader {
    sxml_byte_view input;
    size_t byte_position;
    uint64_t bit_buffer;
    uint32_t bit_count;
} bit_reader;

/* A canonical Huffman code (RFC 1951, section 3.2.2). */
typedef struct huffman_code {
    /* How many symbols have each code length, 1 to 15 (0 unused). */
    uint16_t length_counts[SXML_INFLATE_MAXIMUM_CODE_BIT_COUNT + 1];
    /* The symbols in canonical order: by code length, then by value. */
    uint16_t symbols[SXML_INFLATE_LITERAL_LENGTH_SYMBOL_COUNT];
    /* Indexed by the next SXML_INFLATE_FAST_BIT_COUNT bits of input. */
    uint16_t fast_entries[SXML_INFLATE_FAST_ENTRY_COUNT];
} huffman_code;

/* The code lengths of a code to build. */
typedef struct code_lengths {
    const uint8_t *lengths;
    size_t symbol_count;
    /* Distance codes only: RFC 1951, section 3.2.7 allows one used code of
       one bit (an incomplete code) and no used code at all. */
    bool allows_single_code;
} code_lengths;

typedef struct output_buffer {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t capacity;
    size_t maximum_byte_count;
} output_buffer;

typedef struct inflater {
    bit_reader reader;
    output_buffer output;
    huffman_code literal_length_code;
    huffman_code distance_code;
} inflater;

/* ----------------------------------------------------------- Bit reader */

static void fill_bit_buffer(bit_reader *reader) {
    while (reader->bit_count <= 56 && reader->byte_position < reader->input.byte_count) {
        reader->bit_buffer |= (uint64_t)reader->input.bytes[reader->byte_position] << reader->bit_count;
        reader->byte_position++;
        reader->bit_count += 8;
    }
}

/* The bit count to read and where to put the bits. */
typedef struct bit_request {
    uint32_t bit_count;
    uint32_t *bits;
} bit_request;

/* Reads request.bit_count (at most 16) bits as a number. */
SXML_NODISCARD static sxml_status read_bits(bit_reader *reader, bit_request request) {
    if (reader->bit_count < request.bit_count) {
        fill_bit_buffer(reader);
        if (reader->bit_count < request.bit_count) {
            return SXML_STATUS_UNEXPECTED_END_OF_INPUT;
        }
    }
    *request.bits = (uint32_t)(reader->bit_buffer & ((UINT64_C(1) << request.bit_count) - 1));
    reader->bit_buffer >>= request.bit_count;
    reader->bit_count -= request.bit_count;
    return SXML_STATUS_OK;
}

/* Drops the bits up to the next byte boundary and gives back the whole
   bytes the buffer holds, so a stored block reads the input directly
   (RFC 1951, section 3.2.4). */
static void align_to_byte(bit_reader *reader) {
    reader->byte_position -= reader->bit_count / 8;
    reader->bit_buffer = 0;
    reader->bit_count = 0;
}

/* -------------------------------------------------------- Huffman codes */

static uint32_t reverse_bits(uint32_t code, uint32_t bit_count) {
    uint32_t reversed = 0;
    for (uint32_t bit_index = 0; bit_index < bit_count; bit_index++) {
        reversed = (reversed << 1) | ((code >> bit_index) & 1U);
    }
    return reversed;
}

/*
 * Builds a canonical code from its code lengths (RFC 1951, section 3.2.2).
 * A code that over-subscribes is malformed; so is an incomplete one, but
 * for the distance code's single code of one bit or no code at all
 * (section 3.2.7).
 */
SXML_NODISCARD static sxml_status build_huffman_code(const code_lengths *lengths, huffman_code *code) {
    memset(code, 0, sizeof *code);
    for (size_t symbol = 0; symbol < lengths->symbol_count; symbol++) {
        code->length_counts[lengths->lengths[symbol]]++;
    }
    code->length_counts[0] = 0;
    int32_t codes_left = 1;
    uint32_t used_count = 0;
    for (uint32_t bit_count = 1; bit_count <= SXML_INFLATE_MAXIMUM_CODE_BIT_COUNT; bit_count++) {
        codes_left = codes_left * 2 - (int32_t)code->length_counts[bit_count];
        used_count += code->length_counts[bit_count];
        if (codes_left < 0) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    if (codes_left > 0) {
        bool is_single_code = used_count == 0 || (used_count == 1 && code->length_counts[1] == 1);
        if (!lengths->allows_single_code || !is_single_code) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    /* Canonical order and the first code of each length (section 3.2.2,
       steps 2 and 3). */
    uint16_t offsets[SXML_INFLATE_MAXIMUM_CODE_BIT_COUNT + 2] = {0};
    uint32_t next_codes[SXML_INFLATE_MAXIMUM_CODE_BIT_COUNT + 1] = {0};
    uint32_t next_code = 0;
    for (uint32_t bit_count = 1; bit_count <= SXML_INFLATE_MAXIMUM_CODE_BIT_COUNT; bit_count++) {
        offsets[bit_count + 1] = (uint16_t)(offsets[bit_count] + code->length_counts[bit_count]);
        next_code = (next_code + code->length_counts[bit_count - 1]) << 1;
        next_codes[bit_count] = next_code;
    }
    for (size_t symbol = 0; symbol < lengths->symbol_count; symbol++) {
        const uint32_t bit_count = lengths->lengths[symbol];
        if (bit_count == 0) {
            continue;
        }
        code->symbols[offsets[bit_count]] = (uint16_t)symbol;
        offsets[bit_count]++;
        const uint32_t symbol_code = next_codes[bit_count];
        next_codes[bit_count]++;
        if (bit_count > SXML_INFLATE_FAST_BIT_COUNT) {
            continue;
        }
        /* Codes are packed most significant bit first (section 3.1.1), so
           the table is indexed by the code reversed. */
        const uint32_t reversed = reverse_bits(symbol_code, bit_count);
        const uint16_t entry = (uint16_t)((symbol << 4) | bit_count);
        const uint32_t step = UINT32_C(1) << bit_count;
        for (uint32_t entry_index = reversed; entry_index < SXML_INFLATE_FAST_ENTRY_COUNT; entry_index += step) {
            code->fast_entries[entry_index] = entry;
        }
    }
    return SXML_STATUS_OK;
}

/* The code to decode with and where to put the symbol. */
typedef struct symbol_request {
    const huffman_code *code;
    uint32_t *symbol;
} symbol_request;

/*
 * Decodes one symbol: through the first-level table, or for a longer code
 * bit by bit through the canonical counts (section 3.2.2). A code the
 * lengths leave unused is malformed.
 */
SXML_NODISCARD static sxml_status decode_symbol(bit_reader *reader, symbol_request request) {
    if (reader->bit_count < SXML_INFLATE_MAXIMUM_CODE_BIT_COUNT) {
        fill_bit_buffer(reader);
    }
    const huffman_code *code = request.code;
    const uint16_t entry = code->fast_entries[reader->bit_buffer & (SXML_INFLATE_FAST_ENTRY_COUNT - 1)];
    const uint32_t entry_bit_count = entry & SXML_INFLATE_FAST_ENTRY_BIT_COUNT_MASK;
    if (entry_bit_count > 0) {
        if (entry_bit_count > reader->bit_count) {
            return SXML_STATUS_UNEXPECTED_END_OF_INPUT;
        }
        reader->bit_buffer >>= entry_bit_count;
        reader->bit_count -= entry_bit_count;
        *request.symbol = (uint32_t)entry >> 4;
        return SXML_STATUS_OK;
    }
    uint32_t candidate = 0;
    uint32_t first = 0;
    uint32_t symbol_offset = 0;
    for (uint32_t bit_count = 1; bit_count <= SXML_INFLATE_MAXIMUM_CODE_BIT_COUNT; bit_count++) {
        if (bit_count > reader->bit_count) {
            return SXML_STATUS_UNEXPECTED_END_OF_INPUT;
        }
        candidate |= (uint32_t)(reader->bit_buffer >> (bit_count - 1)) & 1U;
        const uint32_t length_count = code->length_counts[bit_count];
        if (candidate - first < length_count) {
            reader->bit_buffer >>= bit_count;
            reader->bit_count -= bit_count;
            *request.symbol = code->symbols[symbol_offset + candidate - first];
            return SXML_STATUS_OK;
        }
        symbol_offset += length_count;
        first = (first + length_count) << 1;
        candidate <<= 1;
    }
    return SXML_STATUS_MALFORMED_INPUT;
}

/* --------------------------------------------------------------- Output */

/* Makes room for additional_byte_count more bytes, doubling the buffer,
   within the request's limit. */
SXML_NODISCARD static sxml_status reserve_output(output_buffer *output, size_t additional_byte_count) {
    size_t needed = 0;
    if (sxml_size_add(output->byte_count, additional_byte_count, &needed) != SXML_STATUS_OK ||
        needed > output->maximum_byte_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (needed <= output->capacity) {
        return SXML_STATUS_OK;
    }
    size_t capacity = output->capacity;
    if (sxml_size_multiply(capacity, 2, &capacity) != SXML_STATUS_OK) {
        capacity = SIZE_MAX;
    }
    if (capacity < needed) {
        capacity = needed;
    }
    if (capacity > output->maximum_byte_count) {
        capacity = output->maximum_byte_count;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        output->allocator, (sxml_allocation_request){.byte_count = capacity, .alignment = alignof(max_align_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (output->byte_count > 0) {
        memcpy(memory, output->bytes, output->byte_count);
    }
    sxml_allocator_release(output->allocator, output->bytes);
    output->bytes = memory;
    output->capacity = capacity;
    return SXML_STATUS_OK;
}

/* A match: copy length bytes from distance bytes back (section 3.2.3). */
typedef struct match {
    size_t length;
    size_t distance;
} match;

SXML_NODISCARD static sxml_status copy_match(output_buffer *output, match copied) {
    if (copied.distance > output->byte_count) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    sxml_status status = reserve_output(output, copied.length);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Byte by byte: the source may overlap the bytes being written, which
       repeats them (section 3.2.3). */
    size_t source = output->byte_count - copied.distance;
    for (size_t copy_index = 0; copy_index < copied.length; copy_index++) {
        output->bytes[output->byte_count] = output->bytes[source + copy_index];
        output->byte_count++;
    }
    return SXML_STATUS_OK;
}

/* --------------------------------------------------------------- Blocks */

/* RFC 1951, section 3.2.4: LEN, NLEN, then LEN bytes. */
SXML_NODISCARD static sxml_status inflate_stored_block(inflater *state) {
    bit_reader *reader = &state->reader;
    align_to_byte(reader);
    if (reader->input.byte_count - reader->byte_position < 4) {
        return SXML_STATUS_UNEXPECTED_END_OF_INPUT;
    }
    const uint8_t *header = reader->input.bytes + reader->byte_position;
    const uint32_t length = (uint32_t)header[0] | ((uint32_t)header[1] << 8);
    const uint32_t complement = (uint32_t)header[2] | ((uint32_t)header[3] << 8);
    reader->byte_position += 4;
    if ((length ^ 0xFFFFU) != complement) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    if (reader->input.byte_count - reader->byte_position < length) {
        return SXML_STATUS_UNEXPECTED_END_OF_INPUT;
    }
    if (length == 0) {
        return SXML_STATUS_OK;
    }
    sxml_status status = reserve_output(&state->output, length);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy(state->output.bytes + state->output.byte_count, reader->input.bytes + reader->byte_position, length);
    state->output.byte_count += length;
    reader->byte_position += length;
    return SXML_STATUS_OK;
}

/* RFC 1951, section 3.2.6: the fixed literal/length and distance codes. */
SXML_NODISCARD static sxml_status build_fixed_codes(inflater *state) {
    uint8_t lengths[SXML_INFLATE_LITERAL_LENGTH_SYMBOL_COUNT];
    memset(lengths, 8, 144);
    memset(lengths + 144, 9, 256 - 144);
    memset(lengths + 256, 7, 280 - 256);
    memset(lengths + 280, 8, SXML_INFLATE_LITERAL_LENGTH_SYMBOL_COUNT - 280);
    sxml_status status = build_huffman_code(
        &(code_lengths){.lengths = lengths,
                        .symbol_count = SXML_INFLATE_LITERAL_LENGTH_SYMBOL_COUNT,
                        .allows_single_code = false},
        &state->literal_length_code);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(lengths, 5, SXML_INFLATE_DISTANCE_SYMBOL_COUNT);
    return build_huffman_code(
        &(code_lengths){.lengths = lengths,
                        .symbol_count = SXML_INFLATE_DISTANCE_SYMBOL_COUNT,
                        .allows_single_code = false},
        &state->distance_code);
}

/* The repeat codes 16, 17 and 18 of section 3.2.7: extra bits and base. */
typedef struct repeat_rule {
    uint32_t extra_bit_count;
    uint32_t base;
} repeat_rule;

/* RFC 1951, section 3.2.7: HLIT, HDIST, HCLEN, the code length code, then
   the literal/length and distance code lengths as one sequence. */
SXML_NODISCARD static sxml_status build_dynamic_codes(inflater *state) {
    bit_reader *reader = &state->reader;
    uint32_t literal_length_count = 0;
    uint32_t distance_count = 0;
    uint32_t code_length_count = 0;
    sxml_status status = read_bits(reader, (bit_request){.bit_count = 5, .bits = &literal_length_count});
    if (status == SXML_STATUS_OK) {
        status = read_bits(reader, (bit_request){.bit_count = 5, .bits = &distance_count});
    }
    if (status == SXML_STATUS_OK) {
        status = read_bits(reader, (bit_request){.bit_count = 4, .bits = &code_length_count});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    literal_length_count += 257;
    distance_count += 1;
    code_length_count += 4;
    uint8_t code_length_lengths[SXML_INFLATE_CODE_LENGTH_SYMBOL_COUNT] = {0};
    for (uint32_t order_index = 0; order_index < code_length_count; order_index++) {
        uint32_t bits = 0;
        status = read_bits(reader, (bit_request){.bit_count = 3, .bits = &bits});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        code_length_lengths[code_length_order[order_index]] = (uint8_t)bits;
    }
    huffman_code code_length_code;
    status = build_huffman_code(&(code_lengths){.lengths = code_length_lengths,
                                                .symbol_count = SXML_INFLATE_CODE_LENGTH_SYMBOL_COUNT,
                                                .allows_single_code = false},
                                &code_length_code);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t lengths[SXML_INFLATE_LITERAL_LENGTH_SYMBOL_COUNT + SXML_INFLATE_DISTANCE_SYMBOL_COUNT] = {0};
    const uint32_t total_count = literal_length_count + distance_count;
    uint32_t filled_count = 0;
    while (filled_count < total_count) {
        uint32_t symbol = 0;
        status = decode_symbol(reader, (symbol_request){.code = &code_length_code, .symbol = &symbol});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (symbol < 16) {
            lengths[filled_count] = (uint8_t)symbol;
            filled_count++;
            continue;
        }
        uint8_t repeated = 0;
        repeat_rule rule = {.extra_bit_count = 7, .base = 11};
        if (symbol == 16) {
            if (filled_count == 0) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            repeated = lengths[filled_count - 1];
            rule = (repeat_rule){.extra_bit_count = 2, .base = 3};
        } else if (symbol == 17) {
            rule = (repeat_rule){.extra_bit_count = 3, .base = 3};
        }
        uint32_t extra = 0;
        status = read_bits(reader, (bit_request){.bit_count = rule.extra_bit_count, .bits = &extra});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const uint32_t repeat_count = rule.base + extra;
        if (repeat_count > total_count - filled_count) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
        memset(lengths + filled_count, repeated, repeat_count);
        filled_count += repeat_count;
    }
    /* A block whose end-of-block code has no length could never end. */
    if (lengths[SXML_INFLATE_END_OF_BLOCK_SYMBOL] == 0) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    status = build_huffman_code(
        &(code_lengths){.lengths = lengths, .symbol_count = literal_length_count, .allows_single_code = false},
        &state->literal_length_code);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return build_huffman_code(&(code_lengths){.lengths = lengths + literal_length_count,
                                              .symbol_count = distance_count,
                                              .allows_single_code = true},
                              &state->distance_code);
}

/* Reads a length or distance: its extra bits added to the code's base. */
typedef struct extra_request {
    uint32_t base;
    uint32_t extra_bit_count;
    size_t *value;
} extra_request;

SXML_NODISCARD static sxml_status read_extra(bit_reader *reader, extra_request request) {
    uint32_t extra = 0;
    sxml_status status = read_bits(reader, (bit_request){.bit_count = request.extra_bit_count, .bits = &extra});
    if (status == SXML_STATUS_OK) {
        *request.value = (size_t)request.base + extra;
    }
    return status;
}

/* RFC 1951, section 3.2.5: literals, and lengths with distances, up to
   the end-of-block code. */
SXML_NODISCARD static sxml_status inflate_huffman_block(inflater *state) {
    bit_reader *reader = &state->reader;
    for (;;) {
        uint32_t symbol = 0;
        sxml_status status =
            decode_symbol(reader, (symbol_request){.code = &state->literal_length_code, .symbol = &symbol});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (symbol < SXML_INFLATE_END_OF_BLOCK_SYMBOL) {
            status = reserve_output(&state->output, 1);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            state->output.bytes[state->output.byte_count] = (uint8_t)symbol;
            state->output.byte_count++;
            continue;
        }
        if (symbol == SXML_INFLATE_END_OF_BLOCK_SYMBOL) {
            return SXML_STATUS_OK;
        }
        const uint32_t length_code = symbol - SXML_INFLATE_FIRST_LENGTH_SYMBOL;
        if (length_code >= SXML_INFLATE_LENGTH_CODE_COUNT) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
        match copied = {0};
        status = read_extra(reader, (extra_request){.base = length_bases[length_code],
                                                    .extra_bit_count = length_extra_bit_counts[length_code],
                                                    .value = &copied.length});
        uint32_t distance_code = 0;
        if (status == SXML_STATUS_OK) {
            status = decode_symbol(reader, (symbol_request){.code = &state->distance_code, .symbol = &distance_code});
        }
        if (status == SXML_STATUS_OK && distance_code >= SXML_INFLATE_DISTANCE_CODE_COUNT) {
            status = SXML_STATUS_MALFORMED_INPUT;
        }
        if (status == SXML_STATUS_OK) {
            status = read_extra(reader, (extra_request){.base = distance_bases[distance_code],
                                                        .extra_bit_count = distance_extra_bit_counts[distance_code],
                                                        .value = &copied.distance});
        }
        if (status == SXML_STATUS_OK) {
            status = copy_match(&state->output, copied);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
}

/* Every block up to the one marked final (section 3.2.3). */
SXML_NODISCARD static sxml_status inflate_blocks(inflater *state) {
    bool is_final = false;
    while (!is_final) {
        uint32_t header = 0;
        sxml_status status = read_bits(&state->reader, (bit_request){.bit_count = 3, .bits = &header});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        is_final = (header & 1U) != 0;
        const uint32_t block_type = header >> 1;
        if (block_type == 0) {
            status = inflate_stored_block(state);
        } else if (block_type == 1) {
            status = build_fixed_codes(state);
            if (status == SXML_STATUS_OK) {
                status = inflate_huffman_block(state);
            }
        } else if (block_type == 2) {
            status = build_dynamic_codes(state);
            if (status == SXML_STATUS_OK) {
                status = inflate_huffman_block(state);
            }
        } else {
            /* Block type 3 is reserved (section 3.2.3). */
            status = SXML_STATUS_MALFORMED_INPUT;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_inflate(const sxml_inflate_request *request, sxml_inflate_result *result) {
    if (request == NULL || result == NULL || (request->input.bytes == NULL && request->input.byte_count > 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Large enough for the whole state; it lives in the allocator's memory
       rather than on the stack, which WebAssembly keeps small. */
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        request->allocator, (sxml_allocation_request){.byte_count = sizeof(inflater), .alignment = alignof(inflater)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    inflater *state = memory;
    *state = (inflater){.reader = {.input = request->input},
                        .output = {.allocator = request->allocator,
                                   .maximum_byte_count = request->maximum_output_byte_count}};
    size_t initial_capacity = SXML_INFLATE_MINIMUM_OUTPUT_CAPACITY;
    if (request->input.byte_count < SIZE_MAX / SXML_INFLATE_INITIAL_OUTPUT_RATIO &&
        request->input.byte_count * SXML_INFLATE_INITIAL_OUTPUT_RATIO > initial_capacity) {
        initial_capacity = request->input.byte_count * SXML_INFLATE_INITIAL_OUTPUT_RATIO;
    }
    if (initial_capacity > request->maximum_output_byte_count) {
        initial_capacity = request->maximum_output_byte_count;
    }
    if (initial_capacity > 0) {
        status = reserve_output(&state->output, initial_capacity);
    }
    if (status == SXML_STATUS_OK) {
        status = inflate_blocks(state);
    }
    const output_buffer output = state->output;
    /* The final block ends in the byte before the whole bytes the bit
       buffer still holds. */
    const size_t consumed_byte_count = state->reader.byte_position - state->reader.bit_count / 8;
    sxml_allocator_release(request->allocator, state);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(request->allocator, output.bytes);
        return status;
    }
    sxml_memory_view inflated = {.bytes = output.bytes, .byte_count = output.byte_count};
    if (output.byte_count == 0) {
        sxml_allocator_release(request->allocator, output.bytes);
        inflated.bytes = NULL;
    }
    *result = (sxml_inflate_result){.output = inflated, .consumed_byte_count = consumed_byte_count};
    return SXML_STATUS_OK;
}
