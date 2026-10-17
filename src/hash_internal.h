#pragma once

/*
 * Hash algorithms for fn:hash (F&O 4.0, section 5.3.18 "fn:hash"; packet
 * 2.11). Internal to sxml; used by src/function_library_hash.c.
 *
 * Each algorithm reads a whole message, given as a byte view, and writes
 * its digest into a fixed buffer the caller owns: no allocation, no state
 * kept between calls, no recursion (D71; the BLAKE3 tree is merged with a
 * bounded stack of chaining values). Every function is safe to call from
 * any thread.
 *
 * - MD5: RFC 1321 (as updated by RFC 6151), 16 octets.
 * - SHA-1 and SHA-256: FIPS 180-4, sections 6.1 and 6.2; 20 and 32 octets.
 * - BLAKE3: the BLAKE3 specification (version 2020-01-09), section 2, the
 *   hash mode without key, 32 octets of output.
 * - CRC-32: IEEE 802.3 (reflected polynomial 0xEDB88320, initial value and
 *   final XOR 0xFFFFFFFF), the 32-bit value as 4 octets, most significant
 *   first, as fn:hash requires.
 */
#include "sxml/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The hash algorithms fn:hash supports (glossary: hash_algorithm). */
typedef enum sxml_hash_algorithm {
    SXML_HASH_ALGORITHM_MD5,
    SXML_HASH_ALGORITHM_SHA_1,
    SXML_HASH_ALGORITHM_SHA_256,
    SXML_HASH_ALGORITHM_BLAKE3,
    SXML_HASH_ALGORITHM_CRC_32,
} sxml_hash_algorithm;

/* The longest digest of any algorithm, in octets. */
#define SXML_DIGEST_BYTE_CAPACITY ((size_t)32)

/* The octets an algorithm returns (glossary: digest). A plain value:
   safe to copy byte for byte. */
typedef struct sxml_digest {
    uint8_t bytes[SXML_DIGEST_BYTE_CAPACITY];
    /* 16, 20, 32 or 4, by algorithm. */
    size_t byte_count;
} sxml_digest;

/* What to hash: the algorithm and the message, borrowed for the call. */
typedef struct sxml_hash_request {
    sxml_hash_algorithm algorithm;
    sxml_byte_view message;
} sxml_hash_request;

/*
 * Hashes request->message with request->algorithm and writes the digest
 * to *digest. Never fails: every message length is accepted (the message
 * length in bits is taken modulo 2^64, as the algorithms define it). The
 * work is linear in the message length.
 */
void sxml_hash_compute(const sxml_hash_request *request, sxml_digest *digest);

/* The outcome of looking an algorithm name up. */
typedef struct sxml_hash_algorithm_lookup {
    bool is_found;
    sxml_hash_algorithm algorithm;
} sxml_hash_algorithm_lookup;

/*
 * Looks up the algorithm an effective name stands for: "MD5", "SHA-1",
 * "SHA-256", "BLAKE3" or "CRC-32", compared byte for byte. The caller
 * computes the effective name first (fn:upper-case(fn:normalize-space(.))
 * in fn:hash). Writes is_found false for any other name.
 */
void sxml_hash_find_algorithm(sxml_byte_view effective_name,
                              sxml_hash_algorithm_lookup *lookup);
