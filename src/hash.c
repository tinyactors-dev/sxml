/*
 * Hash algorithms for fn:hash (src/hash_internal.h; F&O 4.0, section
 * 5.3.18 "fn:hash"; packet 2.11).
 *
 * Golden file: src/memory.c (layout, naming, comments). MD5, SHA-1 and
 * SHA-256 share one Merkle-Damgard driver: the 64-octet blocks of the
 * message, then the padded tail with the message length in bits (RFC 1321,
 * section 3.1-3.2; FIPS 180-4, section 5.1.1). BLAKE3 hashes 1024-octet
 * chunks and merges their chaining values with a stack bounded by the
 * height of the tree (BLAKE3 specification, section 5.1.2), so nothing
 * recurses (D71).
 */
#include "hash_internal.h"

#include "sxml/memory.h"

#include <string.h>

/* ------------------------------------------------------------ Helpers */

#define SXML_HASH_BLOCK_BYTE_COUNT ((size_t)64)

static uint32_t rotate_left(uint32_t word, unsigned bit_count) {
    return (word << bit_count) | (word >> (32U - bit_count));
}

static uint32_t rotate_right(uint32_t word, unsigned bit_count) {
    return (word >> bit_count) | (word << (32U - bit_count));
}

static uint32_t read_little_endian(sxml_byte_view block, size_t byte_offset) {
    return (uint32_t)block.bytes[byte_offset] | ((uint32_t)block.bytes[byte_offset + 1] << 8U) |
           ((uint32_t)block.bytes[byte_offset + 2] << 16U) |
           ((uint32_t)block.bytes[byte_offset + 3] << 24U);
}

static uint32_t read_big_endian(sxml_byte_view block, size_t byte_offset) {
    return ((uint32_t)block.bytes[byte_offset] << 24U) |
           ((uint32_t)block.bytes[byte_offset + 1] << 16U) |
           ((uint32_t)block.bytes[byte_offset + 2] << 8U) | (uint32_t)block.bytes[byte_offset + 3];
}

/* Where a word goes in a digest. */
typedef struct word_placement {
    size_t byte_offset;
    uint32_t word;
} word_placement;

static void write_little_endian(sxml_digest *digest, word_placement placement) {
    for (size_t byte_index = 0; byte_index < 4; byte_index += 1) {
        digest->bytes[placement.byte_offset + byte_index] =
            (uint8_t)(placement.word >> (8U * byte_index));
    }
}

static void write_big_endian(sxml_digest *digest, word_placement placement) {
    for (size_t byte_index = 0; byte_index < 4; byte_index += 1) {
        digest->bytes[placement.byte_offset + byte_index] =
            (uint8_t)(placement.word >> (8U * (3U - byte_index)));
    }
}

/* The chaining state of MD5 (4 words), SHA-1 (5) or SHA-256 (8). */
typedef struct chaining_state {
    uint32_t words[8];
} chaining_state;

/* ----------------------------------------------------------------- MD5 */

/* RFC 1321, section 3.4: T[i] = floor(2^32 * abs(sin(i + 1))). */
static const uint32_t md5_sines[64] = {
    0xD76AA478U, 0xE8C7B756U, 0x242070DBU, 0xC1BDCEEEU, 0xF57C0FAFU, 0x4787C62AU, 0xA8304613U,
    0xFD469501U, 0x698098D8U, 0x8B44F7AFU, 0xFFFF5BB1U, 0x895CD7BEU, 0x6B901122U, 0xFD987193U,
    0xA679438EU, 0x49B40821U, 0xF61E2562U, 0xC040B340U, 0x265E5A51U, 0xE9B6C7AAU, 0xD62F105DU,
    0x02441453U, 0xD8A1E681U, 0xE7D3FBC8U, 0x21E1CDE6U, 0xC33707D6U, 0xF4D50D87U, 0x455A14EDU,
    0xA9E3E905U, 0xFCEFA3F8U, 0x676F02D9U, 0x8D2A4C8AU, 0xFFFA3942U, 0x8771F681U, 0x6D9D6122U,
    0xFDE5380CU, 0xA4BEEA44U, 0x4BDECFA9U, 0xF6BB4B60U, 0xBEBFBC70U, 0x289B7EC6U, 0xEAA127FAU,
    0xD4EF3085U, 0x04881D05U, 0xD9D4D039U, 0xE6DB99E5U, 0x1FA27CF8U, 0xC4AC5665U, 0xF4292244U,
    0x432AFF97U, 0xAB9423A7U, 0xFC93A039U, 0x655B59C3U, 0x8F0CCC92U, 0xFFEFF47DU, 0x85845DD1U,
    0x6FA87E4FU, 0xFE2CE6E0U, 0xA3014314U, 0x4E0811A1U, 0xF7537E82U, 0xBD3AF235U, 0x2AD7D2BBU,
    0xEB86D391U,
};

/* The rotation of each step, four per round (RFC 1321, section 3.4). */
static const unsigned md5_rotations[4][4] = {
    {7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};

/* One 64-octet block (RFC 1321, section 3.4). */
static void compress_md5(chaining_state *state, sxml_byte_view block) {
    uint32_t message_words[16];
    for (size_t word_index = 0; word_index < 16; word_index += 1) {
        message_words[word_index] = read_little_endian(block, 4 * word_index);
    }
    uint32_t first = state->words[0];
    uint32_t second = state->words[1];
    uint32_t third = state->words[2];
    uint32_t fourth = state->words[3];
    for (size_t step_index = 0; step_index < 64; step_index += 1) {
        const size_t round_index = step_index / 16;
        uint32_t mixed = 0;
        size_t word_index = 0;
        switch (round_index) {
        case 0:
            mixed = (second & third) | (~second & fourth);
            word_index = step_index;
            break;
        case 1:
            mixed = (fourth & second) | (~fourth & third);
            word_index = (5 * step_index + 1) % 16;
            break;
        case 2:
            mixed = second ^ third ^ fourth;
            word_index = (3 * step_index + 5) % 16;
            break;
        default:
            mixed = third ^ (second | ~fourth);
            word_index = (7 * step_index) % 16;
            break;
        }
        mixed += first + md5_sines[step_index] + message_words[word_index];
        first = fourth;
        fourth = third;
        third = second;
        second += rotate_left(mixed, md5_rotations[round_index][step_index % 4]);
    }
    state->words[0] += first;
    state->words[1] += second;
    state->words[2] += third;
    state->words[3] += fourth;
}

/* --------------------------------------------------------------- SHA-1 */

/* One 64-octet block (FIPS 180-4, section 6.1.2). */
static void compress_sha_1(chaining_state *state, sxml_byte_view block) {
    uint32_t schedule[80];
    for (size_t word_index = 0; word_index < 16; word_index += 1) {
        schedule[word_index] = read_big_endian(block, 4 * word_index);
    }
    for (size_t word_index = 16; word_index < 80; word_index += 1) {
        schedule[word_index] =
            rotate_left(schedule[word_index - 3] ^ schedule[word_index - 8] ^
                            schedule[word_index - 14] ^ schedule[word_index - 16],
                        1);
    }
    uint32_t working[5];
    memcpy(working, state->words, sizeof(working));
    for (size_t step_index = 0; step_index < 80; step_index += 1) {
        uint32_t function_value = 0;
        uint32_t constant = 0;
        if (step_index < 20) {
            function_value = (working[1] & working[2]) | (~working[1] & working[3]);
            constant = 0x5A827999U;
        } else if (step_index < 40) {
            function_value = working[1] ^ working[2] ^ working[3];
            constant = 0x6ED9EBA1U;
        } else if (step_index < 60) {
            function_value =
                (working[1] & working[2]) | (working[1] & working[3]) | (working[2] & working[3]);
            constant = 0x8F1BBCDCU;
        } else {
            function_value = working[1] ^ working[2] ^ working[3];
            constant = 0xCA62C1D6U;
        }
        const uint32_t next = rotate_left(working[0], 5) + function_value + working[4] + constant +
                              schedule[step_index];
        working[4] = working[3];
        working[3] = working[2];
        working[2] = rotate_left(working[1], 30);
        working[1] = working[0];
        working[0] = next;
    }
    for (size_t word_index = 0; word_index < 5; word_index += 1) {
        state->words[word_index] += working[word_index];
    }
}

/* ------------------------------------------------------------- SHA-256 */

/* FIPS 180-4, section 4.2.2: the first 32 bits of the fractional parts of
   the cube roots of the first 64 primes. */
static const uint32_t sha_256_constants[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
    0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
    0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
    0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
    0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
    0xC67178F2U,
};

/* FIPS 180-4, section 5.3.3; also the BLAKE3 IV (BLAKE3, section 2.2). */
static const uint32_t sha_256_initial_words[8] = {0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U,
                                                  0xA54FF53AU, 0x510E527FU, 0x9B05688CU,
                                                  0x1F83D9ABU, 0x5BE0CD19U};

/* One 64-octet block (FIPS 180-4, section 6.2.2). */
static void compress_sha_256(chaining_state *state, sxml_byte_view block) {
    uint32_t schedule[64];
    for (size_t word_index = 0; word_index < 16; word_index += 1) {
        schedule[word_index] = read_big_endian(block, 4 * word_index);
    }
    for (size_t word_index = 16; word_index < 64; word_index += 1) {
        const uint32_t early = schedule[word_index - 15];
        const uint32_t late = schedule[word_index - 2];
        const uint32_t small_sigma_0 =
            rotate_right(early, 7) ^ rotate_right(early, 18) ^ (early >> 3U);
        const uint32_t small_sigma_1 =
            rotate_right(late, 17) ^ rotate_right(late, 19) ^ (late >> 10U);
        schedule[word_index] =
            small_sigma_1 + schedule[word_index - 7] + small_sigma_0 + schedule[word_index - 16];
    }
    uint32_t working[8];
    memcpy(working, state->words, sizeof(working));
    for (size_t step_index = 0; step_index < 64; step_index += 1) {
        const uint32_t big_sigma_1 =
            rotate_right(working[4], 6) ^ rotate_right(working[4], 11) ^ rotate_right(working[4], 25);
        const uint32_t choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
        const uint32_t first_temporary = working[7] + big_sigma_1 + choice +
                                         sha_256_constants[step_index] + schedule[step_index];
        const uint32_t big_sigma_0 =
            rotate_right(working[0], 2) ^ rotate_right(working[0], 13) ^ rotate_right(working[0], 22);
        const uint32_t majority =
            (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
        const uint32_t second_temporary = big_sigma_0 + majority;
        working[7] = working[6];
        working[6] = working[5];
        working[5] = working[4];
        working[4] = working[3] + first_temporary;
        working[3] = working[2];
        working[2] = working[1];
        working[1] = working[0];
        working[0] = first_temporary + second_temporary;
    }
    for (size_t word_index = 0; word_index < 8; word_index += 1) {
        state->words[word_index] += working[word_index];
    }
}

/* ------------------------------------------------- Merkle-Damgard driver */

static void compress_block(sxml_hash_algorithm algorithm, chaining_state *state,
                           sxml_byte_view block) {
    switch (algorithm) {
    case SXML_HASH_ALGORITHM_MD5:
        compress_md5(state, block);
        return;
    case SXML_HASH_ALGORITHM_SHA_1:
        compress_sha_1(state, block);
        return;
    case SXML_HASH_ALGORITHM_SHA_256:
        compress_sha_256(state, block);
        return;
    case SXML_HASH_ALGORITHM_BLAKE3:
    case SXML_HASH_ALGORITHM_CRC_32:
        return;
    }
}

/*
 * Every block of the message, then the padding: 0x80, zeros, and the
 * length in bits modulo 2^64 as the last 8 octets, little-endian for MD5
 * (RFC 1321, sections 3.1 and 3.2), big-endian for SHA (FIPS 180-4,
 * section 5.1.1).
 */
static void hash_blocks(sxml_hash_algorithm algorithm, sxml_byte_view message,
                        chaining_state *state) {
    const size_t full_block_count = message.byte_count / SXML_HASH_BLOCK_BYTE_COUNT;
    for (size_t block_index = 0; block_index < full_block_count; block_index += 1) {
        compress_block(algorithm, state,
                       (sxml_byte_view){.bytes = message.bytes +
                                                 block_index * SXML_HASH_BLOCK_BYTE_COUNT,
                                        .byte_count = SXML_HASH_BLOCK_BYTE_COUNT});
    }
    uint8_t tail[2 * SXML_HASH_BLOCK_BYTE_COUNT];
    memset(tail, 0, sizeof(tail));
    const size_t remaining_byte_count = message.byte_count % SXML_HASH_BLOCK_BYTE_COUNT;
    if (remaining_byte_count > 0) {
        memcpy(tail, message.bytes + full_block_count * SXML_HASH_BLOCK_BYTE_COUNT,
               remaining_byte_count);
    }
    tail[remaining_byte_count] = 0x80;
    const size_t tail_byte_count = remaining_byte_count < SXML_HASH_BLOCK_BYTE_COUNT - 8
                                       ? SXML_HASH_BLOCK_BYTE_COUNT
                                       : 2 * SXML_HASH_BLOCK_BYTE_COUNT;
    const uint64_t bit_count = (uint64_t)message.byte_count * 8U;
    for (size_t byte_index = 0; byte_index < 8; byte_index += 1) {
        const unsigned shift = (unsigned)(8 * byte_index);
        const size_t position = algorithm == SXML_HASH_ALGORITHM_MD5
                                    ? tail_byte_count - 8 + byte_index
                                    : tail_byte_count - 1 - byte_index;
        tail[position] = (uint8_t)(bit_count >> shift);
    }
    for (size_t byte_offset = 0; byte_offset < tail_byte_count;
         byte_offset += SXML_HASH_BLOCK_BYTE_COUNT) {
        compress_block(algorithm, state,
                       (sxml_byte_view){.bytes = tail + byte_offset,
                                        .byte_count = SXML_HASH_BLOCK_BYTE_COUNT});
    }
}

/* -------------------------------------------------------------- BLAKE3 */

#define SXML_BLAKE3_CHUNK_BYTE_COUNT ((size_t)1024)
/* A chunk count below 2^64 gives a tree at most 54 levels high above
   chunks of 2^10 octets (BLAKE3, section 5.1.2). */
#define SXML_BLAKE3_STACK_CAPACITY ((size_t)54)

/* Domain flags (BLAKE3, section 2.1, table 3). */
enum {
    SXML_BLAKE3_CHUNK_START = 1U,
    SXML_BLAKE3_CHUNK_END = 2U,
    SXML_BLAKE3_PARENT = 4U,
    SXML_BLAKE3_ROOT = 8U,
};

/* The message word permutation (BLAKE3, section 2.2, table 2). */
static const uint8_t blake3_permutation[16] = {2, 6,  3,  10, 7,  0,  4,  13,
                                               1, 11, 12, 5,  9,  14, 15, 8};

typedef struct blake3_chaining_value {
    uint32_t words[8];
} blake3_chaining_value;

/* The input of one compression (BLAKE3, section 2.2). */
typedef struct blake3_compression {
    blake3_chaining_value chaining_value;
    uint32_t block_words[16];
    uint64_t counter;
    uint32_t block_byte_count;
    uint32_t flags;
} blake3_compression;

/* The quarter-round G on state columns or diagonals. */
typedef struct blake3_mix {
    size_t positions[4];
    uint32_t first_word;
    uint32_t second_word;
} blake3_mix;

static void blake3_mix_state(uint32_t *state, blake3_mix mix) {
    uint32_t *const first = &state[mix.positions[0]];
    uint32_t *const second = &state[mix.positions[1]];
    uint32_t *const third = &state[mix.positions[2]];
    uint32_t *const fourth = &state[mix.positions[3]];
    *first = *first + *second + mix.first_word;
    *fourth = rotate_right(*fourth ^ *first, 16);
    *third = *third + *fourth;
    *second = rotate_right(*second ^ *third, 12);
    *first = *first + *second + mix.second_word;
    *fourth = rotate_right(*fourth ^ *first, 8);
    *third = *third + *fourth;
    *second = rotate_right(*second ^ *third, 7);
}

/* The columns, then the diagonals, of one round. */
static const size_t blake3_mix_positions[8][4] = {
    {0, 4, 8, 12}, {1, 5, 9, 13}, {2, 6, 10, 14}, {3, 7, 11, 15},
    {0, 5, 10, 15}, {1, 6, 11, 12}, {2, 7, 8, 13}, {3, 4, 9, 14}};

/* The compression function: seven rounds, then the state's two halves
   folded (BLAKE3, section 2.2). Writes all 16 output words. */
static void blake3_compress(const blake3_compression *compression, uint32_t *output_words) {
    uint32_t state[16];
    memcpy(state, compression->chaining_value.words, sizeof(compression->chaining_value.words));
    memcpy(&state[8], sha_256_initial_words, 4 * sizeof(uint32_t));
    state[12] = (uint32_t)compression->counter;
    state[13] = (uint32_t)(compression->counter >> 32U);
    state[14] = compression->block_byte_count;
    state[15] = compression->flags;
    uint32_t message_words[16];
    memcpy(message_words, compression->block_words, sizeof(message_words));
    for (size_t round_index = 0; round_index < 7; round_index += 1) {
        for (size_t mix_index = 0; mix_index < 8; mix_index += 1) {
            blake3_mix mix = {.first_word = message_words[2 * mix_index],
                              .second_word = message_words[2 * mix_index + 1]};
            memcpy(mix.positions, blake3_mix_positions[mix_index], sizeof(mix.positions));
            blake3_mix_state(state, mix);
        }
        uint32_t permuted[16];
        for (size_t word_index = 0; word_index < 16; word_index += 1) {
            permuted[word_index] = message_words[blake3_permutation[word_index]];
        }
        memcpy(message_words, permuted, sizeof(message_words));
    }
    for (size_t word_index = 0; word_index < 8; word_index += 1) {
        output_words[word_index] = state[word_index] ^ state[word_index + 8];
        output_words[word_index + 8] =
            state[word_index + 8] ^ compression->chaining_value.words[word_index];
    }
}

/* Loads up to 64 octets as little-endian words, zero-padded. */
static void blake3_load_block(sxml_byte_view block, uint32_t *block_words) {
    uint8_t padded[SXML_HASH_BLOCK_BYTE_COUNT];
    memset(padded, 0, sizeof(padded));
    if (block.bytes != NULL && block.byte_count > 0) {
        memcpy(padded, block.bytes, block.byte_count);
    }
    const sxml_byte_view padded_view = {.bytes = padded, .byte_count = sizeof(padded)};
    for (size_t word_index = 0; word_index < 16; word_index += 1) {
        block_words[word_index] = read_little_endian(padded_view, 4 * word_index);
    }
}

/* Which chunk, by index, and its octets (at most 1024). */
typedef struct blake3_chunk {
    sxml_byte_view chunk_bytes;
    uint64_t chunk_index;
} blake3_chunk;

/* The compression that ends a chunk, its chaining value not yet taken
   (BLAKE3, section 2.4): every block before the last is compressed into
   the chaining value; the last one, possibly short or empty, waits. */
static void blake3_chunk_output(blake3_chunk chunk, blake3_compression *output) {
    blake3_chaining_value chaining_value;
    memcpy(chaining_value.words, sha_256_initial_words, sizeof(chaining_value.words));
    const size_t block_count =
        chunk.chunk_bytes.byte_count == 0
            ? 1
            : (chunk.chunk_bytes.byte_count + SXML_HASH_BLOCK_BYTE_COUNT - 1) /
                  SXML_HASH_BLOCK_BYTE_COUNT;
    for (size_t block_index = 0; block_index < block_count; block_index += 1) {
        const size_t byte_offset = block_index * SXML_HASH_BLOCK_BYTE_COUNT;
        const size_t remaining = chunk.chunk_bytes.byte_count - byte_offset;
        const sxml_byte_view block = {
            .bytes = chunk.chunk_bytes.bytes == NULL ? NULL : chunk.chunk_bytes.bytes + byte_offset,
            .byte_count =
                remaining < SXML_HASH_BLOCK_BYTE_COUNT ? remaining : SXML_HASH_BLOCK_BYTE_COUNT};
        blake3_compression compression = {.chaining_value = chaining_value,
                                          .counter = chunk.chunk_index,
                                          .block_byte_count = (uint32_t)block.byte_count,
                                          .flags = block_index == 0 ? SXML_BLAKE3_CHUNK_START : 0U};
        blake3_load_block(block, compression.block_words);
        if (block_index + 1 == block_count) {
            compression.flags |= SXML_BLAKE3_CHUNK_END;
            *output = compression;
            return;
        }
        uint32_t output_words[16];
        blake3_compress(&compression, output_words);
        memcpy(chaining_value.words, output_words, sizeof(chaining_value.words));
    }
}

static blake3_chaining_value blake3_take_chaining_value(const blake3_compression *compression) {
    uint32_t output_words[16];
    blake3_compress(compression, output_words);
    blake3_chaining_value chaining_value;
    memcpy(chaining_value.words, output_words, sizeof(chaining_value.words));
    return chaining_value;
}

/* The two children of a parent node. */
typedef struct blake3_children {
    blake3_chaining_value left;
    blake3_chaining_value right;
} blake3_children;

/* The compression of a parent node (BLAKE3, section 2.5). */
static blake3_compression blake3_parent_output(const blake3_children *children) {
    blake3_compression compression = {.counter = 0,
                                      .block_byte_count = (uint32_t)SXML_HASH_BLOCK_BYTE_COUNT,
                                      .flags = SXML_BLAKE3_PARENT};
    memcpy(compression.chaining_value.words, sha_256_initial_words,
           sizeof(compression.chaining_value.words));
    memcpy(compression.block_words, children->left.words, sizeof(children->left.words));
    memcpy(&compression.block_words[8], children->right.words, sizeof(children->right.words));
    return compression;
}

/* The chaining values of complete subtrees, left to right. */
typedef struct blake3_stack {
    blake3_chaining_value entries[SXML_BLAKE3_STACK_CAPACITY];
    size_t entry_count;
} blake3_stack;

static void hash_blake3(sxml_byte_view message, sxml_digest *digest) {
    blake3_stack stack = {.entry_count = 0};
    const size_t chunk_count =
        message.byte_count == 0
            ? 1
            : (message.byte_count + SXML_BLAKE3_CHUNK_BYTE_COUNT - 1) / SXML_BLAKE3_CHUNK_BYTE_COUNT;
    blake3_compression output;
    memset(&output, 0, sizeof(output));
    for (size_t chunk_index = 0; chunk_index < chunk_count; chunk_index += 1) {
        const size_t byte_offset = chunk_index * SXML_BLAKE3_CHUNK_BYTE_COUNT;
        const size_t remaining = message.byte_count - byte_offset;
        const blake3_chunk chunk = {
            .chunk_bytes = {.bytes = message.bytes == NULL ? NULL : message.bytes + byte_offset,
                            .byte_count = remaining < SXML_BLAKE3_CHUNK_BYTE_COUNT
                                              ? remaining
                                              : SXML_BLAKE3_CHUNK_BYTE_COUNT},
            .chunk_index = (uint64_t)chunk_index};
        blake3_chunk_output(chunk, &output);
        if (chunk_index + 1 == chunk_count) {
            break;
        }
        /* A complete chunk that is not the last: merge it with the
           subtrees it completes, one per trailing zero bit of the number
           of chunks so far (BLAKE3, section 5.1.2). */
        blake3_chaining_value chaining_value = blake3_take_chaining_value(&output);
        uint64_t completed_chunk_count = (uint64_t)chunk_index + 1;
        while ((completed_chunk_count & 1U) == 0 && stack.entry_count > 0) {
            stack.entry_count -= 1;
            const blake3_children children = {.left = stack.entries[stack.entry_count],
                                              .right = chaining_value};
            const blake3_compression parent = blake3_parent_output(&children);
            chaining_value = blake3_take_chaining_value(&parent);
            completed_chunk_count >>= 1U;
        }
        stack.entries[stack.entry_count] = chaining_value;
        stack.entry_count += 1;
    }
    /* The last chunk's output joins every subtree on the stack, right to
       left; the final compression is the root. */
    while (stack.entry_count > 0) {
        stack.entry_count -= 1;
        const blake3_children children = {.left = stack.entries[stack.entry_count],
                                          .right = blake3_take_chaining_value(&output)};
        output = blake3_parent_output(&children);
    }
    output.counter = 0;
    output.flags |= SXML_BLAKE3_ROOT;
    uint32_t output_words[16];
    blake3_compress(&output, output_words);
    for (size_t word_index = 0; word_index < 8; word_index += 1) {
        write_little_endian(digest,
                            (word_placement){.byte_offset = 4 * word_index,
                                             .word = output_words[word_index]});
    }
    digest->byte_count = 32;
}

/* -------------------------------------------------------------- CRC-32 */

/* IEEE 802.3, bit by bit over the reflected polynomial. */
static uint32_t compute_crc_32(sxml_byte_view message) {
    uint32_t remainder = 0xFFFFFFFFU;
    for (size_t byte_index = 0; byte_index < message.byte_count; byte_index += 1) {
        remainder ^= message.bytes[byte_index];
        for (size_t bit_index = 0; bit_index < 8; bit_index += 1) {
            const uint32_t mask = 0U - (remainder & 1U);
            remainder = (remainder >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~remainder;
}

/* --------------------------------------------------------------- Entry */

void sxml_hash_compute(const sxml_hash_request *request, sxml_digest *digest) {
    memset(digest, 0, sizeof(*digest));
    chaining_state state;
    memset(&state, 0, sizeof(state));
    switch (request->algorithm) {
    case SXML_HASH_ALGORITHM_MD5:
        /* RFC 1321, section 3.3. */
        state.words[0] = 0x67452301U;
        state.words[1] = 0xEFCDAB89U;
        state.words[2] = 0x98BADCFEU;
        state.words[3] = 0x10325476U;
        hash_blocks(request->algorithm, request->message, &state);
        for (size_t word_index = 0; word_index < 4; word_index += 1) {
            write_little_endian(digest, (word_placement){.byte_offset = 4 * word_index,
                                                         .word = state.words[word_index]});
        }
        digest->byte_count = 16;
        return;
    case SXML_HASH_ALGORITHM_SHA_1:
        /* FIPS 180-4, section 5.3.1. */
        state.words[0] = 0x67452301U;
        state.words[1] = 0xEFCDAB89U;
        state.words[2] = 0x98BADCFEU;
        state.words[3] = 0x10325476U;
        state.words[4] = 0xC3D2E1F0U;
        hash_blocks(request->algorithm, request->message, &state);
        for (size_t word_index = 0; word_index < 5; word_index += 1) {
            write_big_endian(digest, (word_placement){.byte_offset = 4 * word_index,
                                                      .word = state.words[word_index]});
        }
        digest->byte_count = 20;
        return;
    case SXML_HASH_ALGORITHM_SHA_256:
        memcpy(state.words, sha_256_initial_words, sizeof(state.words));
        hash_blocks(request->algorithm, request->message, &state);
        for (size_t word_index = 0; word_index < 8; word_index += 1) {
            write_big_endian(digest, (word_placement){.byte_offset = 4 * word_index,
                                                      .word = state.words[word_index]});
        }
        digest->byte_count = 32;
        return;
    case SXML_HASH_ALGORITHM_BLAKE3:
        hash_blake3(request->message, digest);
        return;
    case SXML_HASH_ALGORITHM_CRC_32:
        write_big_endian(digest, (word_placement){.byte_offset = 0,
                                                  .word = compute_crc_32(request->message)});
        digest->byte_count = 4;
        return;
    }
}

/* The names of fn:hash (F&O 4.0, section 5.3.18), in enumeration order. */
static const sxml_byte_view hash_algorithm_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("MD5"), SXML_BYTE_VIEW_INITIALIZER("SHA-1"),
    SXML_BYTE_VIEW_INITIALIZER("SHA-256"), SXML_BYTE_VIEW_INITIALIZER("BLAKE3"),
    SXML_BYTE_VIEW_INITIALIZER("CRC-32")};

void sxml_hash_find_algorithm(sxml_byte_view effective_name,
                              sxml_hash_algorithm_lookup *lookup) {
    *lookup = (sxml_hash_algorithm_lookup){.is_found = false,
                                           .algorithm = SXML_HASH_ALGORITHM_MD5};
    for (size_t name_index = 0;
         name_index < sizeof(hash_algorithm_names) / sizeof(hash_algorithm_names[0]);
         name_index += 1) {
        if (sxml_byte_view_equals(effective_name, hash_algorithm_names[name_index])) {
            *lookup = (sxml_hash_algorithm_lookup){.is_found = true,
                                                   .algorithm = (sxml_hash_algorithm)name_index};
            return;
        }
    }
}
