/*
 * Tests for src/hash_internal.h (packet 2.11): MD5, SHA-1, SHA-256, BLAKE3
 * and CRC-32 digests of short messages, of messages around the block and
 * chunk sizes, and of the long messages of the QT4 test set fn-hash
 * ("ABC" 50,000 times; the octet 0xC0 50,000 times), plus the algorithm
 * names. The functions allocate nothing, so there is no allocation-failure
 * loop.
 *
 * Golden file: tests/memory_test.c (case table, runner). Expected digests
 * come from RFC 1321 (appendix A.5), FIPS 180-4 examples, the BLAKE3
 * reference implementation, F&O 4.0 (fn:hash examples) and QT4 fn-hash.
 */
#include "hash_internal.h"
#include "sxml/memory.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

/* Long messages, built once per case in a static-sized buffer. */
#define LONG_MESSAGE_BYTE_CAPACITY ((size_t)150000)

typedef struct long_message {
    uint8_t bytes[LONG_MESSAGE_BYTE_CAPACITY];
    size_t byte_count;
} long_message;

static long_message test_message;

/* Writes the digest as upper-case hexadecimal into text (capacity 65). */
typedef struct digest_text {
    char characters[2 * SXML_DIGEST_BYTE_CAPACITY + 1];
} digest_text;

static digest_text format_digest(const sxml_digest *digest) {
    static const char hexadecimal_digits[] = "0123456789ABCDEF";
    digest_text text;
    memset(&text, 0, sizeof(text));
    for (size_t byte_index = 0; byte_index < digest->byte_count; byte_index += 1) {
        text.characters[2 * byte_index] = hexadecimal_digits[digest->bytes[byte_index] >> 4U];
        text.characters[2 * byte_index + 1] = hexadecimal_digits[digest->bytes[byte_index] & 0x0FU];
    }
    return text;
}

/* One algorithm, one message, the expected digest in hexadecimal. */
typedef struct digest_case {
    sxml_hash_algorithm algorithm;
    sxml_byte_view message;
    const char *expected_digest;
} digest_case;

static bool digest_holds(const digest_case *test_case) {
    sxml_digest digest;
    sxml_hash_compute(
        &(sxml_hash_request){.algorithm = test_case->algorithm, .message = test_case->message},
        &digest);
    const digest_text text = format_digest(&digest);
    const bool holds = strcmp(text.characters, test_case->expected_digest) == 0;
    if (!holds) {
        (void)fprintf(stderr, "algorithm %d, %zu octets: %s, expected %s\n",
                      (int)test_case->algorithm, test_case->message.byte_count, text.characters,
                      test_case->expected_digest);
    }
    return holds;
}

static void expect_digests(sxml_test_state *state, const digest_case *cases, size_t case_count) {
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        SXML_TEST_EXPECT(state, digest_holds(&cases[case_index]));
    }
}

#define MESSAGE(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view empty_message = {.bytes = NULL, .byte_count = 0};

static void test_short_messages(sxml_test_state *state) {
    const digest_case cases[] = {
        {SXML_HASH_ALGORITHM_MD5, empty_message, "D41D8CD98F00B204E9800998ECF8427E"},
        {SXML_HASH_ALGORITHM_MD5, MESSAGE("abc"), "900150983CD24FB0D6963F7D28E17F72"},
        {SXML_HASH_ALGORITHM_MD5, MESSAGE("ABC"), "902FBDD2B1DF0C4F70B4A5D23525E932"},
        {SXML_HASH_ALGORITHM_MD5, MESSAGE("jamboree"), "CF7711FF42E1B9F2DCF1374E5825F940"},
        {SXML_HASH_ALGORITHM_MD5,
         MESSAGE("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
         "57EDF4A22BE3C955AC49DA2E2107B67A"},
        {SXML_HASH_ALGORITHM_SHA_1, empty_message, "DA39A3EE5E6B4B0D3255BFEF95601890AFD80709"},
        {SXML_HASH_ALGORITHM_SHA_1, MESSAGE("ABC"), "3C01BDBB26F358BAB27F267924AA2C9A03FCFDB8"},
        {SXML_HASH_ALGORITHM_SHA_1, MESSAGE("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
         "84983E441C3BD26EBAAE4AA1F95129E5E54670F1"},
        {SXML_HASH_ALGORITHM_SHA_256, empty_message,
         "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"},
        {SXML_HASH_ALGORITHM_SHA_256, MESSAGE("ABC"),
         "B5D4045C3F466FA91FE2CC6ABE79232A1A57CDF104F7A26E716E0A1E2789DF78"},
        {SXML_HASH_ALGORITHM_SHA_256,
         MESSAGE("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
         "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1"},
        {SXML_HASH_ALGORITHM_BLAKE3, empty_message,
         "AF1349B9F5F9A1A6A0404DEA36DCC9499BCB25C9ADC112B7CC9A93CAE41F3262"},
        {SXML_HASH_ALGORITHM_BLAKE3, MESSAGE("ABC"),
         "D1717274597CF0289694F75D96D444B992A096F1AFD8E7BBFA6EBB1D360FEDFC"},
        {SXML_HASH_ALGORITHM_BLAKE3, MESSAGE("jamboree"),
         "12089B2F37AFB7FDED7DD038D06E0FE647BD7E41BCEBB688B5ABECF6A13489C0"},
        {SXML_HASH_ALGORITHM_CRC_32, empty_message, "00000000"},
        {SXML_HASH_ALGORITHM_CRC_32, MESSAGE("123456789"), "CBF43926"},
    };
    expect_digests(state, cases, SXML_TEST_COUNT(cases));
}

/* "ABC" repeated, then the octet 0xC0 repeated, 50,000 times each
   (QT4 fn-hash, long string and long hexBinary cases). */
static sxml_byte_view fill_repeated(sxml_byte_view unit, size_t repeat_count) {
    test_message.byte_count = 0;
    for (size_t repeat_index = 0; repeat_index < repeat_count; repeat_index += 1) {
        memcpy(test_message.bytes + test_message.byte_count, unit.bytes, unit.byte_count);
        test_message.byte_count += unit.byte_count;
    }
    return (sxml_byte_view){.bytes = test_message.bytes, .byte_count = test_message.byte_count};
}

static void test_long_messages(sxml_test_state *state) {
    const sxml_byte_view letters = fill_repeated(MESSAGE("ABC"), 50000);
    const digest_case letter_cases[] = {
        {SXML_HASH_ALGORITHM_MD5, letters, "1077E3D55103C24FECF6EC6BBCE72D8C"},
        {SXML_HASH_ALGORITHM_SHA_1, letters, "E33C20D696C61A9E416C91F15CB3DB8BDC706506"},
        {SXML_HASH_ALGORITHM_SHA_256, letters,
         "4AC2D916EE1F85EF12145D5A9EF6CB2DE4203147BB55D1A5367025BB1896751B"},
        {SXML_HASH_ALGORITHM_BLAKE3, letters,
         "C2BE0DAC22D8EB919BCFBD6C619356AA62318245FA8B90F60533470658DF5F7C"},
        {SXML_HASH_ALGORITHM_CRC_32, letters, "06496EAB"},
    };
    expect_digests(state, letter_cases, SXML_TEST_COUNT(letter_cases));
    static const uint8_t octet = 0xC0;
    const sxml_byte_view octets =
        fill_repeated((sxml_byte_view){.bytes = &octet, .byte_count = 1}, 50000);
    const digest_case octet_cases[] = {
        {SXML_HASH_ALGORITHM_MD5, octets, "E9D5AC69547693456306FA81C1F9508D"},
        {SXML_HASH_ALGORITHM_SHA_1, octets, "44642E7231D6B89543BF2DB37791DA8F4A052044"},
        {SXML_HASH_ALGORITHM_SHA_256, octets,
         "EA85B9DA3D406717BA817DF0BA9F543DDEC363C437A0515AC5BE74DE2C1F5CB5"},
        {SXML_HASH_ALGORITHM_BLAKE3, octets,
         "E09F7FB6D5B60ADA282145FEC92304EC5A0EB2C9FC0808455D28B3EB50E8AFC1"},
        {SXML_HASH_ALGORITHM_CRC_32, octets, "7C7F56C3"},
    };
    expect_digests(state, octet_cases, SXML_TEST_COUNT(octet_cases));
}

/* The BLAKE3 reference test vectors: input octet i is i mod 251, around
   the block (64) and chunk (1024) sizes; the first 32 octets of the
   expected hash (BLAKE3 repository, test_vectors.json). */
static void test_blake3_boundaries(sxml_test_state *state) {
    for (size_t byte_index = 0; byte_index < 3073; byte_index += 1) {
        test_message.bytes[byte_index] = (uint8_t)(byte_index % 251);
    }
    typedef struct boundary_case {
        size_t byte_count;
        const char *expected_digest;
    } boundary_case;
    static const boundary_case cases[] = {
        {1, "2D3ADEDFF11B61F14C886E35AFA036736DCD87A74D27B5C1510225D0F592E213"},
        {63, "E9BC37A594DAAD83BE9470DF7F7B3798297C3D834CE80BA85D6E207627B7DB7B"},
        {64, "4EED7141EA4A5CD4B788606BD23F46E212AF9CACEBACDC7D1F4C6DC7F2511B98"},
        {65, "DE1E5FA0BE70DF6D2BE8FFFD0E99CEAA8EB6E8C93A63F2D8D1C30ECB6B263DEE"},
        {1023, "10108970EEDA3EB932BAAC1428C7A2163B0E924C9A9E25B35BBA72B28F70BD11"},
        {1024, "42214739F095A406F3FC83DEB889744AC00DF831C10DAA55189B5D121C855AF7"},
        {1025, "D00278AE47EB27B34FAECF67B4FE263F82D5412916C1FFD97C8CB7FB814B8444"},
        {2048, "E776B6028C7CD22A4D0BA182A8BF62205D2EF576467E838ED6F2529B85FBA24A"},
        {2049, "5F4D72F40D7A5F82B15CA2B2E44B1DE3C2EF86C426C95C1AF0B6879522563030"},
        {3072, "B98CB0FF3623BE03326B373DE6B9095218513E64F1EE2EDD2525C7AD1E5CFFD2"},
        {3073, "7124B49501012F81CC7F11CA069EC9226CECB8A2C850CFE644E327D22D3E1CD3"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        const digest_case test_case = {
            .algorithm = SXML_HASH_ALGORITHM_BLAKE3,
            .message = {.bytes = test_message.bytes, .byte_count = cases[case_index].byte_count},
            .expected_digest = cases[case_index].expected_digest};
        SXML_TEST_EXPECT(state, digest_holds(&test_case));
    }
}

static void test_algorithm_names(sxml_test_state *state) {
    sxml_hash_algorithm_lookup lookup;
    sxml_hash_find_algorithm(MESSAGE("SHA-256"), &lookup);
    SXML_TEST_EXPECT(state, lookup.is_found && lookup.algorithm == SXML_HASH_ALGORITHM_SHA_256);
    sxml_hash_find_algorithm(MESSAGE("CRC-32"), &lookup);
    SXML_TEST_EXPECT(state, lookup.is_found && lookup.algorithm == SXML_HASH_ALGORITHM_CRC_32);
    sxml_hash_find_algorithm(MESSAGE("BLAKE3"), &lookup);
    SXML_TEST_EXPECT(state, lookup.is_found && lookup.algorithm == SXML_HASH_ALGORITHM_BLAKE3);
    sxml_hash_find_algorithm(MESSAGE("md5"), &lookup);
    SXML_TEST_EXPECT(state, !lookup.is_found);
    sxml_hash_find_algorithm(empty_message, &lookup);
    SXML_TEST_EXPECT(state, !lookup.is_found);
}

static const sxml_test_case hash_test_cases[] = {
    {.name = "short_messages", .function = test_short_messages},
    {.name = "long_messages", .function = test_long_messages},
    {.name = "blake3_boundaries", .function = test_blake3_boundaries},
    {.name = "algorithm_names", .function = test_algorithm_names},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "hash",
            .cases = hash_test_cases,
            .case_count = SXML_TEST_COUNT(hash_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
