/*
 * Deflate (RFC 1951; src/deflate_internal.h; D333): a writer of raw
 * DEFLATE streams, written from the RFC. LZ77 over the 32 KiB window with
 * hash chains of three-byte strings and one step of lazy matching (the
 * parameters of zlib's level 6, which RFC 1951, section 4 describes in
 * outline), then for each block of tokens the smallest of a dynamic
 * Huffman block, a fixed Huffman block and stored blocks. Code lengths are
 * limited to 15 bits (7 for the code length code, section 3.2.7) by
 * adjusting a Huffman tree's depths until the code is complete again.
 *
 * Everything is deterministic: the same input gives the same stream.
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
    /* RFC 1951, section 3.2.5: matches of 3 to 258 bytes, at most 32768
       bytes back. */
    SXML_DEFLATE_MINIMUM_MATCH_LENGTH = 3,
    SXML_DEFLATE_MAXIMUM_MATCH_LENGTH = 258,
    SXML_DEFLATE_WINDOW_BYTE_COUNT = 32768,
    SXML_DEFLATE_WINDOW_MASK = SXML_DEFLATE_WINDOW_BYTE_COUNT - 1,
    SXML_DEFLATE_HASH_BIT_COUNT = 15,
    SXML_DEFLATE_HASH_ENTRY_COUNT = 1 << SXML_DEFLATE_HASH_BIT_COUNT,
    SXML_DEFLATE_HASH_MASK = SXML_DEFLATE_HASH_ENTRY_COUNT - 1,
    /* zlib's level 6: the longest chain followed, a match long enough to
       stop looking, the length past which no lazy match is tried, and the
       length past which the lazy search follows a quarter of the chain. */
    SXML_DEFLATE_MAXIMUM_CHAIN_LENGTH = 128,
    SXML_DEFLATE_NICE_MATCH_LENGTH = 128,
    SXML_DEFLATE_MAXIMUM_LAZY_LENGTH = 16,
    SXML_DEFLATE_GOOD_MATCH_LENGTH = 8,
    /* A three-byte match this far back costs more than its literals. */
    SXML_DEFLATE_TOO_FAR_DISTANCE = 4096,
    /* Tokens per block. */
    SXML_DEFLATE_BLOCK_TOKEN_CAPACITY = 16384,
    /* A stored block holds at most 65535 bytes (section 3.2.4). */
    SXML_DEFLATE_STORED_BLOCK_MAXIMUM_BYTE_COUNT = 65535,
    SXML_DEFLATE_LITERAL_LENGTH_SYMBOL_COUNT = 286,
    SXML_DEFLATE_FIXED_LITERAL_LENGTH_SYMBOL_COUNT = 288,
    SXML_DEFLATE_DISTANCE_SYMBOL_COUNT = 30,
    SXML_DEFLATE_CODE_LENGTH_SYMBOL_COUNT = 19,
    SXML_DEFLATE_MAXIMUM_CODE_BIT_COUNT = 15,
    SXML_DEFLATE_MAXIMUM_CODE_LENGTH_BIT_COUNT = 7,
    SXML_DEFLATE_END_OF_BLOCK_SYMBOL = 256,
    SXML_DEFLATE_FIRST_LENGTH_SYMBOL = 257,
    SXML_DEFLATE_LENGTH_CODE_COUNT = 29,
    /* The most entries the code length sequence can have (section 3.2.7). */
    SXML_DEFLATE_MAXIMUM_CODE_LENGTH_ENTRY_COUNT =
        SXML_DEFLATE_LITERAL_LENGTH_SYMBOL_COUNT + SXML_DEFLATE_DISTANCE_SYMBOL_COUNT,
    /* Room for every node of a Huffman tree over the literal/length code. */
    SXML_DEFLATE_MAXIMUM_NODE_COUNT = 2 * SXML_DEFLATE_FIXED_LITERAL_LENGTH_SYMBOL_COUNT,
};

/* RFC 1951, section 3.2.5: the base length and extra bits of codes 257-285. */
static const uint16_t length_bases[SXML_DEFLATE_LENGTH_CODE_COUNT] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195,
    227, 258};
static const uint8_t length_extra_bit_counts[SXML_DEFLATE_LENGTH_CODE_COUNT] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
/* RFC 1951, section 3.2.5: the base distance and extra bits of codes 0-29. */
static const uint16_t distance_bases[SXML_DEFLATE_DISTANCE_SYMBOL_COUNT] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
    4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra_bit_counts[SXML_DEFLATE_DISTANCE_SYMBOL_COUNT] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
/* RFC 1951, section 3.2.7: the order in which code length code lengths go. */
static const uint8_t code_length_order[SXML_DEFLATE_CODE_LENGTH_SYMBOL_COUNT] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
/* The extra bits of code length codes 16, 17 and 18 (section 3.2.7). */
static const uint8_t code_length_extra_bit_counts[SXML_DEFLATE_CODE_LENGTH_SYMBOL_COUNT] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7};

/* No position: an empty hash chain. */
static const size_t no_position = SIZE_MAX;

/* A literal (distance 0) or a match, with its codes found once. */
typedef struct token {
    uint16_t literal_or_length;
    uint16_t distance;
    uint8_t length_code;
    uint8_t distance_code;
} token;

/* Writes bits least significant first (RFC 1951, section 3.1.1). */
typedef struct bit_writer {
    sxml_memory_view output;
    size_t byte_count;
    uint64_t bit_buffer;
    uint32_t bit_count;
    /* Never true while the output's bound holds; checked so a mistake in
       it cannot write past the memory. */
    bool has_overflowed;
} bit_writer;

/* A code ready to write: lengths and codes, the codes bit-reversed. */
typedef struct huffman_encoding {
    uint8_t lengths[SXML_DEFLATE_FIXED_LITERAL_LENGTH_SYMBOL_COUNT];
    uint16_t codes[SXML_DEFLATE_FIXED_LITERAL_LENGTH_SYMBOL_COUNT];
} huffman_encoding;

typedef struct deflater {
    sxml_byte_view input;
    /* The latest position of each hash, and for each position in the
       window the one before it with the same hash. */
    size_t *hash_heads;
    size_t *previous_positions;
    token *tokens;
    size_t token_count;
    /* The input bytes the block's tokens cover: block_start to covered. */
    size_t block_start;
    size_t covered;
    /* Blocks chosen to be stored wait here, from stored_run_start to
       block_start, so they are written together in as few stored blocks
       as their size allows. */
    size_t stored_run_start;
    bool has_stored_run;
    bit_writer writer;
} deflater;

/* ------------------------------------------------------------ Bit writer */

typedef struct bit_field {
    uint32_t bits;
    uint32_t bit_count;
} bit_field;

static void write_bits(bit_writer *writer, bit_field field) {
    writer->bit_buffer |= (uint64_t)field.bits << writer->bit_count;
    writer->bit_count += field.bit_count;
    while (writer->bit_count >= 8) {
        if (writer->byte_count < writer->output.byte_count) {
            writer->output.bytes[writer->byte_count] = (uint8_t)writer->bit_buffer;
            writer->byte_count++;
        } else {
            writer->has_overflowed = true;
        }
        writer->bit_buffer >>= 8;
        writer->bit_count -= 8;
    }
}

static void align_writer(bit_writer *writer) {
    if (writer->bit_count > 0) {
        write_bits(writer, (bit_field){.bits = 0, .bit_count = 8 - writer->bit_count});
    }
}

/* -------------------------------------------------------- Huffman codes */

static uint32_t reverse_bits(uint32_t code, uint32_t bit_count) {
    uint32_t reversed = 0;
    for (uint32_t bit_index = 0; bit_index < bit_count; bit_index++) {
        reversed = (reversed << 1) | ((code >> bit_index) & 1U);
    }
    return reversed;
}

/* Assigns the canonical codes of encoding's lengths (section 3.2.2). */
static void assign_codes(huffman_encoding *encoding, size_t symbol_count) {
    uint32_t length_counts[SXML_DEFLATE_MAXIMUM_CODE_BIT_COUNT + 1] = {0};
    for (size_t symbol = 0; symbol < symbol_count; symbol++) {
        length_counts[encoding->lengths[symbol]]++;
    }
    length_counts[0] = 0;
    uint32_t next_codes[SXML_DEFLATE_MAXIMUM_CODE_BIT_COUNT + 1] = {0};
    uint32_t next_code = 0;
    for (uint32_t bit_count = 1; bit_count <= SXML_DEFLATE_MAXIMUM_CODE_BIT_COUNT; bit_count++) {
        next_code = (next_code + length_counts[bit_count - 1]) << 1;
        next_codes[bit_count] = next_code;
    }
    for (size_t symbol = 0; symbol < symbol_count; symbol++) {
        const uint32_t bit_count = encoding->lengths[symbol];
        encoding->codes[symbol] = 0;
        if (bit_count > 0) {
            encoding->codes[symbol] = (uint16_t)reverse_bits(next_codes[bit_count], bit_count);
            next_codes[bit_count]++;
        }
    }
}

/* The frequencies of a code's symbols and its longest code. */
typedef struct code_request {
    const uint32_t *frequencies;
    size_t symbol_count;
    uint32_t maximum_bit_count;
} code_request;

/* True when the first leaf sorts before the second: by frequency, then by
   symbol, so the order (and the stream) is deterministic. */
typedef struct leaf_pair {
    const uint32_t *frequencies;
    uint16_t first;
    uint16_t second;
} leaf_pair;

static bool sorts_before(leaf_pair pair) {
    const uint32_t first_frequency = pair.frequencies[pair.first];
    const uint32_t second_frequency = pair.frequencies[pair.second];
    return first_frequency < second_frequency || (first_frequency == second_frequency && pair.first < pair.second);
}

/* Lengthens or shortens leaves until the code is complete with no code
   longer than maximum_bit_count: Kraft's sum in units of
   2^-maximum_bit_count must be exactly 2^maximum_bit_count. */
typedef struct length_histogram {
    uint32_t counts[SXML_DEFLATE_MAXIMUM_CODE_BIT_COUNT + 2];
    uint32_t maximum_bit_count;
} length_histogram;

static void complete_histogram(length_histogram *histogram) {
    const uint32_t maximum = histogram->maximum_bit_count;
    const uint32_t target = UINT32_C(1) << maximum;
    uint32_t kraft_sum = 0;
    for (uint32_t bit_count = 1; bit_count <= maximum; bit_count++) {
        kraft_sum += histogram->counts[bit_count] << (maximum - bit_count);
    }
    /* Too many codes (leaves clamped to the maximum): move a leaf of the
       deepest level above the maximum one level down. */
    while (kraft_sum > target) {
        uint32_t bit_count = maximum - 1;
        while (histogram->counts[bit_count] == 0) {
            bit_count--;
        }
        histogram->counts[bit_count]--;
        histogram->counts[bit_count + 1]++;
        kraft_sum -= UINT32_C(1) << (maximum - bit_count - 1);
    }
    /* Room left: move a leaf of the deepest level up, whose gain divides
       what is left, until none is. */
    while (kraft_sum < target) {
        uint32_t bit_count = maximum;
        while (histogram->counts[bit_count] == 0) {
            bit_count--;
        }
        histogram->counts[bit_count]--;
        histogram->counts[bit_count - 1]++;
        kraft_sum += UINT32_C(1) << (maximum - bit_count);
    }
}

/*
 * Builds code lengths for request's frequencies into encoding, then its
 * codes: a Huffman tree by the two-queue method over the leaves sorted by
 * frequency, its depths limited to the maximum, the lengths then dealt to
 * the leaves, longest to the least frequent. A code of fewer than two
 * symbols gets two codes of one bit, so every code is complete.
 */
static void build_encoding(const code_request *request, huffman_encoding *encoding) {
    memset(encoding->lengths, 0, sizeof encoding->lengths);
    uint16_t leaves[SXML_DEFLATE_FIXED_LITERAL_LENGTH_SYMBOL_COUNT];
    size_t leaf_count = 0;
    for (size_t symbol = 0; symbol < request->symbol_count; symbol++) {
        if (request->frequencies[symbol] == 0) {
            continue;
        }
        /* Insertion sort: at most 286 leaves. */
        size_t insert_index = leaf_count;
        while (insert_index > 0 && sorts_before((leaf_pair){.frequencies = request->frequencies,
                                                            .first = (uint16_t)symbol,
                                                            .second = leaves[insert_index - 1]})) {
            leaves[insert_index] = leaves[insert_index - 1];
            insert_index--;
        }
        leaves[insert_index] = (uint16_t)symbol;
        leaf_count++;
    }
    if (leaf_count < 2) {
        const size_t used_symbol = leaf_count == 1 ? leaves[0] : 0;
        encoding->lengths[used_symbol] = 1;
        encoding->lengths[used_symbol == 0 ? 1 : 0] = 1;
        assign_codes(encoding, request->symbol_count);
        return;
    }
    /* Nodes 0 to leaf_count - 1 are the leaves in order; the rest are made
       in order of weight, so two queues give the two lightest. */
    uint32_t weights[SXML_DEFLATE_MAXIMUM_NODE_COUNT];
    uint16_t parents[SXML_DEFLATE_MAXIMUM_NODE_COUNT];
    uint32_t depths[SXML_DEFLATE_MAXIMUM_NODE_COUNT];
    for (size_t leaf_index = 0; leaf_index < leaf_count; leaf_index++) {
        weights[leaf_index] = request->frequencies[leaves[leaf_index]];
    }
    size_t next_leaf = 0;
    size_t next_internal = leaf_count;
    size_t node_count = leaf_count;
    while (node_count < 2 * leaf_count - 1) {
        size_t chosen[2] = {0, 0};
        for (size_t choice_index = 0; choice_index < 2; choice_index++) {
            const bool takes_leaf = next_leaf < leaf_count &&
                                    (next_internal >= node_count || weights[next_leaf] <= weights[next_internal]);
            if (takes_leaf) {
                chosen[choice_index] = next_leaf;
                next_leaf++;
            } else {
                chosen[choice_index] = next_internal;
                next_internal++;
            }
        }
        weights[node_count] = weights[chosen[0]] + weights[chosen[1]];
        parents[chosen[0]] = (uint16_t)node_count;
        parents[chosen[1]] = (uint16_t)node_count;
        node_count++;
    }
    /* Parents come after their children: depths from the root down. */
    depths[node_count - 1] = 0;
    length_histogram histogram = {.maximum_bit_count = request->maximum_bit_count};
    for (size_t node_index = node_count - 1; node_index-- > 0;) {
        depths[node_index] = depths[parents[node_index]] + 1;
    }
    for (size_t leaf_index = 0; leaf_index < leaf_count; leaf_index++) {
        const uint32_t depth = depths[leaf_index] > histogram.maximum_bit_count ? histogram.maximum_bit_count
                                                                                : depths[leaf_index];
        histogram.counts[depth]++;
    }
    complete_histogram(&histogram);
    size_t leaf_index = 0;
    for (uint32_t bit_count = histogram.maximum_bit_count; bit_count >= 1; bit_count--) {
        for (uint32_t count_index = 0; count_index < histogram.counts[bit_count]; count_index++) {
            encoding->lengths[leaves[leaf_index]] = (uint8_t)bit_count;
            leaf_index++;
        }
    }
    assign_codes(encoding, request->symbol_count);
}

/* RFC 1951, section 3.2.6: the fixed codes. */
static void build_fixed_encodings(huffman_encoding *literal_length, huffman_encoding *distance) {
    memset(literal_length->lengths, 8, 144);
    memset(literal_length->lengths + 144, 9, 256 - 144);
    memset(literal_length->lengths + 256, 7, 280 - 256);
    memset(literal_length->lengths + 280, 8, SXML_DEFLATE_FIXED_LITERAL_LENGTH_SYMBOL_COUNT - 280);
    assign_codes(literal_length, SXML_DEFLATE_FIXED_LITERAL_LENGTH_SYMBOL_COUNT);
    memset(distance->lengths, 0, sizeof distance->lengths);
    memset(distance->lengths, 5, SXML_DEFLATE_DISTANCE_SYMBOL_COUNT);
    assign_codes(distance, SXML_DEFLATE_DISTANCE_SYMBOL_COUNT);
}

/* ----------------------------------------------------------- A block */

/* How often each symbol occurs in a block. */
typedef struct block_frequencies {
    uint32_t literal_length[SXML_DEFLATE_LITERAL_LENGTH_SYMBOL_COUNT];
    uint32_t distance[SXML_DEFLATE_DISTANCE_SYMBOL_COUNT];
} block_frequencies;

/* A dynamic block's header (section 3.2.7): the code length sequence,
   run-length coded with codes 16, 17 and 18. */
typedef struct dynamic_header {
    huffman_encoding literal_length;
    huffman_encoding distance;
    huffman_encoding code_length;
    uint32_t literal_length_count;
    uint32_t distance_count;
    uint32_t code_length_count;
    uint8_t entry_symbols[SXML_DEFLATE_MAXIMUM_CODE_LENGTH_ENTRY_COUNT];
    uint8_t entry_extras[SXML_DEFLATE_MAXIMUM_CODE_LENGTH_ENTRY_COUNT];
    size_t entry_count;
} dynamic_header;

/* One entry of the sequence: a code length code and its extra bits. */
typedef struct header_entry {
    uint8_t symbol;
    uint8_t extra;
} header_entry;

static void add_header_entry(dynamic_header *header, header_entry entry) {
    header->entry_symbols[header->entry_count] = entry.symbol;
    header->entry_extras[header->entry_count] = entry.extra;
    header->entry_count++;
}

/* Run-length codes the literal/length and distance code lengths as one
   sequence (section 3.2.7 lets repeats cross from one to the other). */
static void code_length_sequence(dynamic_header *header) {
    uint8_t sequence[SXML_DEFLATE_MAXIMUM_CODE_LENGTH_ENTRY_COUNT];
    memcpy(sequence, header->literal_length.lengths, header->literal_length_count);
    memcpy(sequence + header->literal_length_count, header->distance.lengths, header->distance_count);
    const size_t total_count = (size_t)header->literal_length_count + header->distance_count;
    header->entry_count = 0;
    size_t run_start = 0;
    while (run_start < total_count) {
        const uint8_t length = sequence[run_start];
        size_t run_end = run_start + 1;
        while (run_end < total_count && sequence[run_end] == length) {
            run_end++;
        }
        size_t remaining = run_end - run_start;
        if (length == 0) {
            while (remaining >= 11) {
                const size_t repeat_count = remaining > 138 ? 138 : remaining;
                add_header_entry(header, (header_entry){.symbol = 18, .extra = (uint8_t)(repeat_count - 11)});
                remaining -= repeat_count;
            }
            if (remaining >= 3) {
                add_header_entry(header, (header_entry){.symbol = 17, .extra = (uint8_t)(remaining - 3)});
                remaining = 0;
            }
        } else {
            add_header_entry(header, (header_entry){.symbol = length, .extra = 0});
            remaining--;
            while (remaining >= 3) {
                const size_t repeat_count = remaining > 6 ? 6 : remaining;
                add_header_entry(header, (header_entry){.symbol = 16, .extra = (uint8_t)(repeat_count - 3)});
                remaining -= repeat_count;
            }
        }
        for (; remaining > 0; remaining--) {
            add_header_entry(header, (header_entry){.symbol = length, .extra = 0});
        }
        run_start = run_end;
    }
}

/* Builds the dynamic codes of a block and its header, and returns the
   header's size in bits. */
static uint64_t build_dynamic_header(const block_frequencies *frequencies, dynamic_header *header) {
    build_encoding(&(code_request){.frequencies = frequencies->literal_length,
                                   .symbol_count = SXML_DEFLATE_LITERAL_LENGTH_SYMBOL_COUNT,
                                   .maximum_bit_count = SXML_DEFLATE_MAXIMUM_CODE_BIT_COUNT},
                   &header->literal_length);
    build_encoding(&(code_request){.frequencies = frequencies->distance,
                                   .symbol_count = SXML_DEFLATE_DISTANCE_SYMBOL_COUNT,
                                   .maximum_bit_count = SXML_DEFLATE_MAXIMUM_CODE_BIT_COUNT},
                   &header->distance);
    header->literal_length_count = SXML_DEFLATE_LITERAL_LENGTH_SYMBOL_COUNT;
    while (header->literal_length_count > SXML_DEFLATE_FIRST_LENGTH_SYMBOL &&
           header->literal_length.lengths[header->literal_length_count - 1] == 0) {
        header->literal_length_count--;
    }
    header->distance_count = SXML_DEFLATE_DISTANCE_SYMBOL_COUNT;
    while (header->distance_count > 1 && header->distance.lengths[header->distance_count - 1] == 0) {
        header->distance_count--;
    }
    code_length_sequence(header);
    uint32_t code_length_frequencies[SXML_DEFLATE_CODE_LENGTH_SYMBOL_COUNT] = {0};
    for (size_t entry_index = 0; entry_index < header->entry_count; entry_index++) {
        code_length_frequencies[header->entry_symbols[entry_index]]++;
    }
    build_encoding(&(code_request){.frequencies = code_length_frequencies,
                                   .symbol_count = SXML_DEFLATE_CODE_LENGTH_SYMBOL_COUNT,
                                   .maximum_bit_count = SXML_DEFLATE_MAXIMUM_CODE_LENGTH_BIT_COUNT},
                   &header->code_length);
    header->code_length_count = SXML_DEFLATE_CODE_LENGTH_SYMBOL_COUNT;
    while (header->code_length_count > 4 &&
           header->code_length.lengths[code_length_order[header->code_length_count - 1]] == 0) {
        header->code_length_count--;
    }
    uint64_t bit_count = 5 + 5 + 4 + 3 * (uint64_t)header->code_length_count;
    for (size_t entry_index = 0; entry_index < header->entry_count; entry_index++) {
        const uint8_t symbol = header->entry_symbols[entry_index];
        bit_count += (uint64_t)header->code_length.lengths[symbol] + code_length_extra_bit_counts[symbol];
    }
    return bit_count;
}

/* The bits a block's symbols take with a pair of codes. */
typedef struct block_codes {
    const huffman_encoding *literal_length;
    const huffman_encoding *distance;
} block_codes;

static uint64_t count_symbol_bits(const block_frequencies *frequencies, block_codes codes) {
    uint64_t bit_count = 0;
    for (size_t symbol = 0; symbol < SXML_DEFLATE_LITERAL_LENGTH_SYMBOL_COUNT; symbol++) {
        uint64_t symbol_bit_count = codes.literal_length->lengths[symbol];
        if (symbol >= SXML_DEFLATE_FIRST_LENGTH_SYMBOL) {
            symbol_bit_count += length_extra_bit_counts[symbol - SXML_DEFLATE_FIRST_LENGTH_SYMBOL];
        }
        bit_count += symbol_bit_count * frequencies->literal_length[symbol];
    }
    for (size_t symbol = 0; symbol < SXML_DEFLATE_DISTANCE_SYMBOL_COUNT; symbol++) {
        bit_count += ((uint64_t)codes.distance->lengths[symbol] + distance_extra_bit_counts[symbol]) *
                     frequencies->distance[symbol];
    }
    return bit_count;
}

static void write_symbol(bit_writer *writer, const huffman_encoding *encoding, size_t symbol) {
    write_bits(writer, (bit_field){.bits = encoding->codes[symbol], .bit_count = encoding->lengths[symbol]});
}

static void write_dynamic_header(bit_writer *writer, const dynamic_header *header) {
    write_bits(writer, (bit_field){.bits = header->literal_length_count - 257, .bit_count = 5});
    write_bits(writer, (bit_field){.bits = header->distance_count - 1, .bit_count = 5});
    write_bits(writer, (bit_field){.bits = header->code_length_count - 4, .bit_count = 4});
    for (uint32_t order_index = 0; order_index < header->code_length_count; order_index++) {
        write_bits(writer,
                   (bit_field){.bits = header->code_length.lengths[code_length_order[order_index]], .bit_count = 3});
    }
    for (size_t entry_index = 0; entry_index < header->entry_count; entry_index++) {
        const uint8_t symbol = header->entry_symbols[entry_index];
        write_symbol(writer, &header->code_length, symbol);
        if (code_length_extra_bit_counts[symbol] > 0) {
            write_bits(writer, (bit_field){.bits = header->entry_extras[entry_index],
                                           .bit_count = code_length_extra_bit_counts[symbol]});
        }
    }
}

static void write_tokens(deflater *state, block_codes codes) {
    bit_writer *writer = &state->writer;
    for (size_t token_index = 0; token_index < state->token_count; token_index++) {
        const token written = state->tokens[token_index];
        if (written.distance == 0) {
            write_symbol(writer, codes.literal_length, written.literal_or_length);
            continue;
        }
        write_symbol(writer, codes.literal_length, (size_t)SXML_DEFLATE_FIRST_LENGTH_SYMBOL + written.length_code);
        write_bits(writer, (bit_field){.bits = (uint32_t)written.literal_or_length - length_bases[written.length_code],
                                       .bit_count = length_extra_bit_counts[written.length_code]});
        write_symbol(writer, codes.distance, written.distance_code);
        write_bits(writer, (bit_field){.bits = (uint32_t)written.distance - distance_bases[written.distance_code],
                                       .bit_count = distance_extra_bit_counts[written.distance_code]});
    }
    write_symbol(writer, codes.literal_length, SXML_DEFLATE_END_OF_BLOCK_SYMBOL);
}

/* The bytes of input from run_start to run_end as stored blocks of at
   most 65535 bytes (section 3.2.4), the last marked final when asked. */
typedef struct stored_run {
    size_t run_start;
    size_t run_end;
    bool is_final;
} stored_run;

static void write_stored_run(deflater *state, stored_run run) {
    bit_writer *writer = &state->writer;
    size_t chunk_start = run.run_start;
    do {
        size_t chunk_byte_count = run.run_end - chunk_start;
        if (chunk_byte_count > SXML_DEFLATE_STORED_BLOCK_MAXIMUM_BYTE_COUNT) {
            chunk_byte_count = SXML_DEFLATE_STORED_BLOCK_MAXIMUM_BYTE_COUNT;
        }
        const bool is_last = chunk_start + chunk_byte_count == run.run_end;
        write_bits(writer, (bit_field){.bits = run.is_final && is_last ? 1U : 0U, .bit_count = 3});
        align_writer(writer);
        write_bits(writer, (bit_field){.bits = (uint32_t)chunk_byte_count, .bit_count = 16});
        write_bits(writer, (bit_field){.bits = (uint32_t)chunk_byte_count ^ 0xFFFFU, .bit_count = 16});
        if (writer->output.byte_count - writer->byte_count >= chunk_byte_count) {
            if (chunk_byte_count > 0) {
                memcpy(writer->output.bytes + writer->byte_count, state->input.bytes + chunk_start, chunk_byte_count);
            }
            writer->byte_count += chunk_byte_count;
        } else {
            writer->has_overflowed = true;
        }
        chunk_start += chunk_byte_count;
    } while (chunk_start < run.run_end);
}

static void count_frequencies(const deflater *state, block_frequencies *frequencies) {
    memset(frequencies, 0, sizeof *frequencies);
    for (size_t token_index = 0; token_index < state->token_count; token_index++) {
        const token counted = state->tokens[token_index];
        if (counted.distance == 0) {
            frequencies->literal_length[counted.literal_or_length]++;
        } else {
            frequencies->literal_length[SXML_DEFLATE_FIRST_LENGTH_SYMBOL + counted.length_code]++;
            frequencies->distance[counted.distance_code]++;
        }
    }
    frequencies->literal_length[SXML_DEFLATE_END_OF_BLOCK_SYMBOL] = 1;
}

/* The block's encodings, kept off the stack's hot path in one place. */
typedef struct block_choice {
    block_frequencies frequencies;
    dynamic_header header;
    huffman_encoding fixed_literal_length;
    huffman_encoding fixed_distance;
} block_choice;

/* Writes the block of tokens as the smallest of its three encodings. */
typedef struct block_end {
    bool is_final;
} block_end;

static void flush_block(deflater *state, block_end end) {
    block_choice choice;
    count_frequencies(state, &choice.frequencies);
    const uint64_t dynamic_bit_count =
        3 + build_dynamic_header(&choice.frequencies, &choice.header) +
        count_symbol_bits(&choice.frequencies,
                          (block_codes){.literal_length = &choice.header.literal_length,
                                        .distance = &choice.header.distance});
    build_fixed_encodings(&choice.fixed_literal_length, &choice.fixed_distance);
    const uint64_t fixed_bit_count =
        3 + count_symbol_bits(&choice.frequencies, (block_codes){.literal_length = &choice.fixed_literal_length,
                                                                 .distance = &choice.fixed_distance});
    const uint64_t block_byte_count = state->covered - state->block_start;
    const uint64_t stored_chunk_count = block_byte_count / SXML_DEFLATE_STORED_BLOCK_MAXIMUM_BYTE_COUNT + 1;
    const uint64_t stored_bit_count = (block_byte_count + 5 * stored_chunk_count) * 8;
    /* An empty last block after stored ones ends their run instead. */
    const bool is_stored = block_byte_count > 0
                               ? stored_bit_count <= dynamic_bit_count && stored_bit_count <= fixed_bit_count
                               : state->has_stored_run;
    if (is_stored) {
        if (!state->has_stored_run) {
            state->stored_run_start = state->block_start;
            state->has_stored_run = true;
        }
        if (end.is_final) {
            write_stored_run(state, (stored_run){.run_start = state->stored_run_start,
                                                 .run_end = state->covered,
                                                 .is_final = true});
        }
    } else {
        if (state->has_stored_run) {
            write_stored_run(state, (stored_run){.run_start = state->stored_run_start,
                                                 .run_end = state->block_start,
                                                 .is_final = false});
            state->has_stored_run = false;
        }
        const uint32_t final_bit = end.is_final ? 1U : 0U;
        if (dynamic_bit_count < fixed_bit_count) {
            write_bits(&state->writer, (bit_field){.bits = final_bit | (2U << 1), .bit_count = 3});
            write_dynamic_header(&state->writer, &choice.header);
            write_tokens(state, (block_codes){.literal_length = &choice.header.literal_length,
                                              .distance = &choice.header.distance});
        } else {
            write_bits(&state->writer, (bit_field){.bits = final_bit | (1U << 1), .bit_count = 3});
            write_tokens(state, (block_codes){.literal_length = &choice.fixed_literal_length,
                                              .distance = &choice.fixed_distance});
        }
    }
    if (end.is_final) {
        align_writer(&state->writer);
    }
    state->block_start = state->covered;
    state->token_count = 0;
}

static void add_token(deflater *state, token added) {
    state->tokens[state->token_count] = added;
    state->token_count++;
    state->covered += added.distance == 0 ? 1 : added.literal_or_length;
    if (state->token_count == SXML_DEFLATE_BLOCK_TOKEN_CAPACITY) {
        flush_block(state, (block_end){.is_final = false});
    }
}

static void add_literal(deflater *state, uint8_t literal) {
    add_token(state, (token){.literal_or_length = literal});
}

/* A match: length bytes from distance bytes back (section 3.2.5). */
typedef struct match {
    size_t length;
    size_t distance;
} match;

static void add_match(deflater *state, match found) {
    uint8_t length_code = SXML_DEFLATE_LENGTH_CODE_COUNT - 1;
    while (length_bases[length_code] > found.length) {
        length_code--;
    }
    uint8_t distance_code = SXML_DEFLATE_DISTANCE_SYMBOL_COUNT - 1;
    while (distance_bases[distance_code] > found.distance) {
        distance_code--;
    }
    add_token(state, (token){.literal_or_length = (uint16_t)found.length,
                             .distance = (uint16_t)found.distance,
                             .length_code = length_code,
                             .distance_code = distance_code});
}

/* ---------------------------------------------------------------- LZ77 */

static size_t hash_at(const deflater *state, size_t position) {
    const uint8_t *bytes = state->input.bytes + position;
    return (((size_t)bytes[0] << 10) ^ ((size_t)bytes[1] << 5) ^ bytes[2]) & SXML_DEFLATE_HASH_MASK;
}

/* Adds position to its hash chain; a position with fewer than three bytes
   after it starts no match. */
static void insert_position(deflater *state, size_t position) {
    if (state->input.byte_count - position < SXML_DEFLATE_MINIMUM_MATCH_LENGTH) {
        return;
    }
    const size_t hash = hash_at(state, position);
    state->previous_positions[position & SXML_DEFLATE_WINDOW_MASK] = state->hash_heads[hash];
    state->hash_heads[hash] = position;
}

/* Where to search and how far along the chain. */
typedef struct match_search {
    size_t position;
    size_t chain_length;
} match_search;

/*
 * The longest earlier match of the bytes at search.position, before that
 * position joins its chain. A chain entry within the window was written
 * when its position was inserted and not since, because the position that
 * shares its slot, 32768 later, is not inserted yet; and chains only go
 * back, so the walk ends.
 */
static match find_longest_match(const deflater *state, match_search search) {
    match best = {.length = SXML_DEFLATE_MINIMUM_MATCH_LENGTH - 1, .distance = 0};
    const size_t position = search.position;
    size_t maximum_length = state->input.byte_count - position;
    if (maximum_length > SXML_DEFLATE_MAXIMUM_MATCH_LENGTH) {
        maximum_length = SXML_DEFLATE_MAXIMUM_MATCH_LENGTH;
    }
    if (maximum_length < SXML_DEFLATE_MINIMUM_MATCH_LENGTH) {
        return best;
    }
    const uint8_t *bytes = state->input.bytes;
    size_t candidate = state->hash_heads[hash_at(state, position)];
    for (size_t chain_index = 0; chain_index < search.chain_length; chain_index++) {
        if (candidate == no_position || position - candidate > SXML_DEFLATE_WINDOW_BYTE_COUNT) {
            break;
        }
        if (bytes[candidate + best.length] == bytes[position + best.length] && bytes[candidate] == bytes[position]) {
            size_t length = 1;
            while (length < maximum_length && bytes[candidate + length] == bytes[position + length]) {
                length++;
            }
            if (length > best.length) {
                best = (match){.length = length, .distance = position - candidate};
                if (length >= SXML_DEFLATE_NICE_MATCH_LENGTH || length == maximum_length) {
                    break;
                }
            }
        }
        candidate = state->previous_positions[candidate & SXML_DEFLATE_WINDOW_MASK];
    }
    if (best.length < SXML_DEFLATE_MINIMUM_MATCH_LENGTH ||
        (best.length == SXML_DEFLATE_MINIMUM_MATCH_LENGTH && best.distance > SXML_DEFLATE_TOO_FAR_DISTANCE)) {
        best = (match){.length = 0, .distance = 0};
    }
    return best;
}

/*
 * LZ77 with one step of lazy matching: a match found at a position is
 * taken only when the next position has no longer one; otherwise the
 * position's byte goes as a literal and the next match is weighed in turn.
 */
static void compress(deflater *state) {
    const size_t byte_count = state->input.byte_count;
    match pending = {.length = 0, .distance = 0};
    bool has_pending_byte = false;
    size_t position = 0;
    while (position < byte_count) {
        match current = {.length = 0, .distance = 0};
        if (pending.length < SXML_DEFLATE_MAXIMUM_LAZY_LENGTH) {
            const size_t chain_length =
                pending.length >= SXML_DEFLATE_GOOD_MATCH_LENGTH ? SXML_DEFLATE_MAXIMUM_CHAIN_LENGTH / 4
                                                                 : SXML_DEFLATE_MAXIMUM_CHAIN_LENGTH;
            current = find_longest_match(state, (match_search){.position = position, .chain_length = chain_length});
        }
        insert_position(state, position);
        if (pending.length >= SXML_DEFLATE_MINIMUM_MATCH_LENGTH && current.length <= pending.length) {
            const size_t match_end = position - 1 + pending.length;
            add_match(state, pending);
            for (size_t inserted = position + 1; inserted < match_end; inserted++) {
                insert_position(state, inserted);
            }
            position = match_end;
            pending = (match){.length = 0, .distance = 0};
            has_pending_byte = false;
            continue;
        }
        if (has_pending_byte) {
            add_literal(state, state->input.bytes[position - 1]);
        }
        has_pending_byte = true;
        pending = current;
        position++;
    }
    if (has_pending_byte) {
        add_literal(state, state->input.bytes[byte_count - 1]);
    }
    flush_block(state, (block_end){.is_final = true});
}

/* --------------------------------------------------------------- Memory */

/* The working memory: hash heads, the window's chains, the block's
   tokens, then room for the stream. */
typedef struct working_memory {
    size_t hash_heads[SXML_DEFLATE_HASH_ENTRY_COUNT];
    size_t previous_positions[SXML_DEFLATE_WINDOW_BYTE_COUNT];
    token tokens[SXML_DEFLATE_BLOCK_TOKEN_CAPACITY];
    uint8_t output[];
} working_memory;

/*
 * The room for the stream. A Huffman block is written only when smaller
 * than its bytes stored with 5 bytes per 65535; there are at most
 * input / SXML_DEFLATE_BLOCK_TOKEN_CAPACITY + 2 blocks, and a stored run
 * takes at most 6 bytes per 65535 bytes and one more chunk per block, so
 * 8 bytes per chunk and per block, and 64 more, are never exceeded.
 */
SXML_NODISCARD static sxml_status plan_output_room(size_t input_byte_count, size_t *output_byte_count) {
    const size_t block_count = input_byte_count / SXML_DEFLATE_BLOCK_TOKEN_CAPACITY + 2;
    const size_t chunk_count = input_byte_count / SXML_DEFLATE_STORED_BLOCK_MAXIMUM_BYTE_COUNT + block_count;
    size_t overhead = 0;
    sxml_status status = sxml_size_multiply(chunk_count + block_count, 8, &overhead);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(overhead, 64, &overhead);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(input_byte_count, overhead, output_byte_count);
    }
    return status;
}

sxml_status sxml_deflate(const sxml_deflate_request *request, sxml_memory_view *output) {
    if (request == NULL || output == NULL || (request->input.bytes == NULL && request->input.byte_count > 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t output_room = 0;
    size_t working_byte_count = 0;
    if (plan_output_room(request->input.byte_count, &output_room) != SXML_STATUS_OK ||
        sxml_size_add(sizeof(working_memory), output_room, &working_byte_count) != SXML_STATUS_OK) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        request->allocator,
        (sxml_allocation_request){.byte_count = working_byte_count, .alignment = alignof(working_memory)}, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    working_memory *working = memory;
    deflater state = {
        .input = request->input,
        .hash_heads = working->hash_heads,
        .previous_positions = working->previous_positions,
        .tokens = working->tokens,
        .writer = {.output = {.bytes = working->output, .byte_count = output_room}},
    };
    for (size_t hash_index = 0; hash_index < SXML_DEFLATE_HASH_ENTRY_COUNT; hash_index++) {
        state.hash_heads[hash_index] = no_position;
    }
    for (size_t window_index = 0; window_index < SXML_DEFLATE_WINDOW_BYTE_COUNT; window_index++) {
        state.previous_positions[window_index] = no_position;
    }
    compress(&state);
    if (state.writer.has_overflowed) {
        sxml_allocator_release(request->allocator, working);
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    /* The stream is copied out at its size, so the caller holds no more
       memory than the stream. */
    void *stream = NULL;
    status = sxml_allocator_allocate(
        request->allocator,
        (sxml_allocation_request){.byte_count = state.writer.byte_count, .alignment = alignof(max_align_t)}, &stream);
    if (status == SXML_STATUS_OK) {
        memcpy(stream, state.writer.output.bytes, state.writer.byte_count);
        *output = (sxml_memory_view){.bytes = stream, .byte_count = state.writer.byte_count};
    }
    sxml_allocator_release(request->allocator, working);
    return status;
}
