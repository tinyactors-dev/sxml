/*
 * Tests for include/sxml/string.h: strict UTF-8 validation, construction of
 * strings, codepoint access, iteration and the codepoint collation.
 *
 * The ill-formed inputs follow Unicode 16.0, section 3.9, table 3-7, and the
 * examples of section 3.9.6 "U+FFFD Substitution of Maximal Subparts".
 */
#include "sxml/string.h"
#include "test_runner.h"

/* A byte view over an array of bytes, which may contain NUL. */
#define TEST_BYTES(...)                                                       \
    ((sxml_byte_view){                                                        \
        .bytes = (const uint8_t[]){__VA_ARGS__},                              \
        .byte_count = sizeof((const uint8_t[]){__VA_ARGS__}),                 \
    })

/* An output value that a failing call must leave in place. */
#define TEST_UNTOUCHED_CODEPOINT 0xDEADU

/* A validation that differs from every result the tests expect. */
static const sxml_utf8_validation test_unset_validation = {
    .valid_byte_count = 99,
    .codepoint_count = 99,
    .is_ascii = false,
};

/* Validates utf8, requires success and returns the string. */
static sxml_string test_string_from(sxml_test_state *state,
                                    sxml_byte_view utf8) {
    sxml_string string = {
        .utf8 = {.bytes = NULL, .byte_count = 0},
        .codepoint_count = 0,
        .is_ascii = false,
    };
    sxml_status status = sxml_string_from_utf8(utf8, &string);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    return string;
}

/* Expects utf8 to be rejected after valid_byte_count well-formed bytes. */
static void test_expect_rejected_at(sxml_test_state *state,
                                    sxml_byte_view utf8,
                                    size_t valid_byte_count) {
    sxml_utf8_validation validation = test_unset_validation;
    sxml_status status = sxml_utf8_validate(utf8, &validation);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_UTF8);
    SXML_TEST_EXPECT(state, validation.valid_byte_count == valid_byte_count);
}

/* ---------------------------------------------------------- Validation */

static void test_validate_accepts_empty_input(sxml_test_state *state) {
    sxml_utf8_validation validation = test_unset_validation;
    sxml_byte_view empty = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(state,
                     sxml_utf8_validate(empty, &validation) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, validation.valid_byte_count == 0);
    SXML_TEST_EXPECT(state, validation.codepoint_count == 0);
    SXML_TEST_EXPECT(state, validation.is_ascii);
}

static void test_validate_counts_ascii(sxml_test_state *state) {
    sxml_utf8_validation validation = test_unset_validation;
    sxml_status status = sxml_utf8_validate(
        SXML_BYTE_VIEW_FROM_LITERAL("hello, world"), &validation);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, validation.valid_byte_count == 12);
    SXML_TEST_EXPECT(state, validation.codepoint_count == 12);
    SXML_TEST_EXPECT(state, validation.is_ascii);
}

static void test_validate_accepts_nul_and_delete(sxml_test_state *state) {
    sxml_utf8_validation validation = test_unset_validation;
    sxml_status status =
        sxml_utf8_validate(TEST_BYTES(0x00, 0x7F, 0x00), &validation);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, validation.codepoint_count == 3);
    SXML_TEST_EXPECT(state, validation.is_ascii);
}

static void test_validate_counts_every_encoded_length(
    sxml_test_state *state) {
    /* U+0041, U+00E9, U+20AC, U+1F600: one, two, three and four bytes. */
    sxml_utf8_validation validation = test_unset_validation;
    sxml_status status = sxml_utf8_validate(
        TEST_BYTES(0x41, 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80),
        &validation);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, validation.valid_byte_count == 10);
    SXML_TEST_EXPECT(state, validation.codepoint_count == 4);
    SXML_TEST_EXPECT(state, !validation.is_ascii);
}

static void test_validate_accepts_boundaries_of_each_range(
    sxml_test_state *state) {
    /* The first and last codepoint of every row of table 3-7. */
    sxml_byte_view boundaries[] = {
        TEST_BYTES(0xC2, 0x80),             /* U+0080 */
        TEST_BYTES(0xDF, 0xBF),             /* U+07FF */
        TEST_BYTES(0xE0, 0xA0, 0x80),       /* U+0800 */
        TEST_BYTES(0xE0, 0xBF, 0xBF),       /* U+0FFF */
        TEST_BYTES(0xE1, 0x80, 0x80),       /* U+1000 */
        TEST_BYTES(0xEC, 0xBF, 0xBF),       /* U+CFFF */
        TEST_BYTES(0xED, 0x80, 0x80),       /* U+D000 */
        TEST_BYTES(0xED, 0x9F, 0xBF),       /* U+D7FF */
        TEST_BYTES(0xEE, 0x80, 0x80),       /* U+E000 */
        TEST_BYTES(0xEF, 0xBF, 0xBF),       /* U+FFFF */
        TEST_BYTES(0xF0, 0x90, 0x80, 0x80), /* U+10000 */
        TEST_BYTES(0xF0, 0xBF, 0xBF, 0xBF), /* U+3FFFF */
        TEST_BYTES(0xF1, 0x80, 0x80, 0x80), /* U+40000 */
        TEST_BYTES(0xF3, 0xBF, 0xBF, 0xBF), /* U+FFFFF */
        TEST_BYTES(0xF4, 0x80, 0x80, 0x80), /* U+100000 */
        TEST_BYTES(0xF4, 0x8F, 0xBF, 0xBF), /* U+10FFFF */
    };
    for (size_t boundary_index = 0;
         boundary_index < SXML_TEST_COUNT(boundaries); boundary_index++) {
        sxml_utf8_validation validation = test_unset_validation;
        sxml_byte_view utf8 = boundaries[boundary_index];
        SXML_TEST_EXPECT(state, sxml_utf8_validate(utf8, &validation) ==
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, validation.valid_byte_count == utf8.byte_count);
        SXML_TEST_EXPECT(state, validation.codepoint_count == 1);
        SXML_TEST_EXPECT(state, !validation.is_ascii);
    }
}

static void test_validate_rejects_overlong_forms(sxml_test_state *state) {
    test_expect_rejected_at(state, TEST_BYTES(0xC0, 0x80), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xC1, 0xBF), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xE0, 0x80, 0x80), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xE0, 0x9F, 0xBF), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xF0, 0x80, 0x80, 0x80), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xF0, 0x8F, 0xBF, 0xBF), 0);
}

static void test_validate_rejects_surrogates(sxml_test_state *state) {
    test_expect_rejected_at(state, TEST_BYTES(0xED, 0xA0, 0x80), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xED, 0xBF, 0xBF), 0);
    /* A CESU-8 pair for U+10000 is two surrogates, not one codepoint. */
    test_expect_rejected_at(
        state, TEST_BYTES(0x61, 0xED, 0xA0, 0x80, 0xED, 0xB0, 0x80), 1);
}

static void test_validate_rejects_codepoints_above_maximum(
    sxml_test_state *state) {
    test_expect_rejected_at(state, TEST_BYTES(0xF4, 0x90, 0x80, 0x80), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xF5, 0x80, 0x80, 0x80), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xF7, 0xBF, 0xBF, 0xBF), 0);
    /* Former five- and six-byte forms (RFC 3629, section 3). */
    test_expect_rejected_at(state, TEST_BYTES(0xF8, 0x88, 0x80, 0x80, 0x80),
                            0);
    test_expect_rejected_at(
        state, TEST_BYTES(0xFC, 0x84, 0x80, 0x80, 0x80, 0x80), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xFE), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xFF), 0);
}

static void test_validate_rejects_stray_continuation_bytes(
    sxml_test_state *state) {
    test_expect_rejected_at(state, TEST_BYTES(0x80), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xBF), 0);
    test_expect_rejected_at(state, TEST_BYTES(0x61, 0xC3, 0xA9, 0xA9), 3);
}

static void test_validate_rejects_truncated_sequences(
    sxml_test_state *state) {
    test_expect_rejected_at(state, TEST_BYTES(0xC3), 0);
    test_expect_rejected_at(state, TEST_BYTES(0x61, 0xE2, 0x82), 1);
    test_expect_rejected_at(state, TEST_BYTES(0xF0, 0x9F, 0x98), 0);
    /* A truncated encoding followed by an ASCII byte (section 3.9.6). */
    test_expect_rejected_at(state, TEST_BYTES(0xE2, 0x82, 0x41), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xF0, 0x9F, 0x41, 0x41), 0);
    test_expect_rejected_at(state, TEST_BYTES(0xC3, 0xC3, 0xA9), 0);
}

static void test_validate_describes_valid_prefix(sxml_test_state *state) {
    /* "aé€" then a stray continuation byte. */
    sxml_utf8_validation validation = test_unset_validation;
    sxml_status status = sxml_utf8_validate(
        TEST_BYTES(0x61, 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0x80, 0x61),
        &validation);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_UTF8);
    SXML_TEST_EXPECT(state, validation.valid_byte_count == 6);
    SXML_TEST_EXPECT(state, validation.codepoint_count == 3);
    SXML_TEST_EXPECT(state, !validation.is_ascii);
    status = sxml_utf8_validate(TEST_BYTES(0x61, 0x62, 0xFF), &validation);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_UTF8);
    SXML_TEST_EXPECT(state, validation.valid_byte_count == 2);
    SXML_TEST_EXPECT(state, validation.codepoint_count == 2);
    SXML_TEST_EXPECT(state, validation.is_ascii);
}

static void test_validate_accepts_every_scalar_value(sxml_test_state *state) {
    /* Every codepoint except surrogates, encoded by hand (Unicode 16.0,
       table 3-6), must pass; this cross-checks table 3-7. */
    bool all_accepted = true;
    for (uint32_t codepoint = 0; codepoint <= 0x10FFFFU; codepoint++) {
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
            continue;
        }
        uint8_t encoded[4] = {0};
        size_t encoded_byte_count = 0;
        if (codepoint < 0x80U) {
            encoded[0] = (uint8_t)codepoint;
            encoded_byte_count = 1;
        } else if (codepoint < 0x800U) {
            encoded[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
            encoded[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            encoded_byte_count = 2;
        } else if (codepoint < 0x10000U) {
            encoded[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
            encoded[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
            encoded[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            encoded_byte_count = 3;
        } else {
            encoded[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
            encoded[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
            encoded[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
            encoded[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            encoded_byte_count = 4;
        }
        sxml_string string = {
            .utf8 = {.bytes = NULL, .byte_count = 0},
            .codepoint_count = 0,
            .is_ascii = false,
        };
        sxml_status status = sxml_string_from_utf8(
            (sxml_byte_view){.bytes = encoded,
                             .byte_count = encoded_byte_count},
            &string);
        uint32_t decoded = TEST_UNTOUCHED_CODEPOINT;
        if (status != SXML_STATUS_OK || string.codepoint_count != 1 ||
            sxml_string_codepoint_at(string, 0, &decoded) != SXML_STATUS_OK ||
            decoded != codepoint) {
            all_accepted = false;
            break;
        }
    }
    SXML_TEST_EXPECT(state, all_accepted);
}

static void test_validate_rejects_every_ill_formed_pair(
    sxml_test_state *state) {
    /* For every lead byte and second byte, the pair followed by enough
       continuation bytes is accepted exactly where table 3-7 says. */
    size_t mismatch_count = 0;
    for (uint32_t lead_byte = 0xC0U; lead_byte <= 0xFFU; lead_byte++) {
        for (uint32_t second_byte = 0; second_byte <= 0xFFU; second_byte++) {
            uint8_t bytes[4] = {(uint8_t)lead_byte, (uint8_t)second_byte, 0x80U,
                                0x80U};
            size_t byte_count = lead_byte >= 0xF0U   ? 4
                                : lead_byte >= 0xE0U ? 3
                                                     : 2;
            uint32_t second_minimum = lead_byte == 0xE0U   ? 0xA0U
                                      : lead_byte == 0xF0U ? 0x90U
                                                           : 0x80U;
            uint32_t second_maximum = lead_byte == 0xEDU   ? 0x9FU
                                      : lead_byte == 0xF4U ? 0x8FU
                                                           : 0xBFU;
            bool expected_valid = lead_byte >= 0xC2U && lead_byte <= 0xF4U &&
                                  second_byte >= second_minimum &&
                                  second_byte <= second_maximum;
            sxml_utf8_validation validation = test_unset_validation;
            sxml_status status = sxml_utf8_validate(
                (sxml_byte_view){.bytes = bytes, .byte_count = byte_count},
                &validation);
            if ((status == SXML_STATUS_OK) != expected_valid) {
                mismatch_count++;
            }
        }
    }
    SXML_TEST_EXPECT(state, mismatch_count == 0);
}

/* ---------------------------------------------------------- Construction */

static void test_from_utf8_borrows_bytes_and_counts(sxml_test_state *state) {
    sxml_byte_view utf8 = TEST_BYTES(0x61, 0xC3, 0xA9, 0x62);
    sxml_string string = test_string_from(state, utf8);
    SXML_TEST_EXPECT(state, string.utf8.bytes == utf8.bytes);
    SXML_TEST_EXPECT(state, string.utf8.byte_count == 4);
    SXML_TEST_EXPECT(state, string.codepoint_count == 3);
    SXML_TEST_EXPECT(state, !string.is_ascii);
    sxml_string ascii =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL("abc"));
    SXML_TEST_EXPECT(state, ascii.codepoint_count == 3);
    SXML_TEST_EXPECT(state, ascii.is_ascii);
}

static void test_from_utf8_rejects_and_keeps_output(sxml_test_state *state) {
    static const uint8_t kept_bytes[] = {'k'};
    sxml_string string = {
        .utf8 = {.bytes = kept_bytes, .byte_count = 1},
        .codepoint_count = 1,
        .is_ascii = true,
    };
    sxml_status status =
        sxml_string_from_utf8(TEST_BYTES(0x61, 0xED, 0xA0, 0x80), &string);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_UTF8);
    SXML_TEST_EXPECT(state, string.utf8.bytes == kept_bytes);
    SXML_TEST_EXPECT(state, string.utf8.byte_count == 1);
    SXML_TEST_EXPECT(state, string.codepoint_count == 1);
    SXML_TEST_EXPECT(state, string.is_ascii);
}

static void test_from_utf8_accepts_empty_input(sxml_test_state *state) {
    sxml_string string = test_string_from(
        state, (sxml_byte_view){.bytes = NULL, .byte_count = 0});
    SXML_TEST_EXPECT(state, string.codepoint_count == 0);
    SXML_TEST_EXPECT(state, string.is_ascii);
}

/* ------------------------------------------------------ Codepoint access */

static void test_codepoint_at_reads_ascii(sxml_test_state *state) {
    sxml_string string =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL("xyz"));
    uint32_t codepoint = TEST_UNTOUCHED_CODEPOINT;
    SXML_TEST_EXPECT(state, sxml_string_codepoint_at(string, 0, &codepoint) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, codepoint == 'x');
    SXML_TEST_EXPECT(state, sxml_string_codepoint_at(string, 2, &codepoint) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, codepoint == 'z');
}

static void test_codepoint_at_decodes_every_encoded_length(
    sxml_test_state *state) {
    sxml_string string = test_string_from(
        state,
        TEST_BYTES(0x41, 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80,
                   0xF4, 0x8F, 0xBF, 0xBF));
    static const uint32_t expected[] = {0x41U, 0xE9U, 0x20ACU, 0x1F600U,
                                        0x10FFFFU};
    SXML_TEST_REQUIRE(state, string.codepoint_count == 5);
    for (size_t codepoint_index = 0; codepoint_index < 5; codepoint_index++) {
        uint32_t codepoint = TEST_UNTOUCHED_CODEPOINT;
        SXML_TEST_EXPECT(state, sxml_string_codepoint_at(
                                    string, codepoint_index, &codepoint) ==
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, codepoint == expected[codepoint_index]);
    }
}

static void test_codepoint_at_rejects_index_and_keeps_output(
    sxml_test_state *state) {
    sxml_string ascii =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL("ab"));
    sxml_string non_ascii = test_string_from(state, TEST_BYTES(0xC3, 0xA9));
    sxml_string empty = {
        .utf8 = {.bytes = NULL, .byte_count = 0},
        .codepoint_count = 0,
        .is_ascii = false,
    };
    uint32_t codepoint = TEST_UNTOUCHED_CODEPOINT;
    SXML_TEST_EXPECT(state, sxml_string_codepoint_at(ascii, 2, &codepoint) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state,
                     sxml_string_codepoint_at(non_ascii, 1, &codepoint) ==
                         SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state,
                     sxml_string_codepoint_at(non_ascii, SIZE_MAX,
                                              &codepoint) ==
                         SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_string_codepoint_at(empty, 0, &codepoint) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, codepoint == TEST_UNTOUCHED_CODEPOINT);
}

static void test_codepoint_at_works_without_ascii_flag(
    sxml_test_state *state) {
    /* is_ascii false makes no claim: the slow path must agree. */
    sxml_string string = {
        .utf8 = SXML_BYTE_VIEW_FROM_LITERAL("abc"),
        .codepoint_count = 3,
        .is_ascii = false,
    };
    uint32_t codepoint = TEST_UNTOUCHED_CODEPOINT;
    SXML_TEST_EXPECT(state, sxml_string_codepoint_at(string, 2, &codepoint) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, codepoint == 'c');
}

static void test_codepoint_at_stays_within_inconsistent_string(
    sxml_test_state *state) {
    /* Results are unspecified, but no read may leave the bytes; the
       sanitizers check that. */
    sxml_string overcounted = {
        .utf8 = TEST_BYTES(0xC3, 0xA9),
        .codepoint_count = 5,
        .is_ascii = false,
    };
    sxml_string false_ascii = {
        .utf8 = TEST_BYTES(0x61),
        .codepoint_count = 4,
        .is_ascii = true,
    };
    sxml_string truncated = {
        .utf8 = TEST_BYTES(0xF0, 0x9F),
        .codepoint_count = 1,
        .is_ascii = false,
    };
    uint32_t codepoint = TEST_UNTOUCHED_CODEPOINT;
    SXML_TEST_EXPECT(state,
                     sxml_string_codepoint_at(overcounted, 3, &codepoint) ==
                         SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state,
                     sxml_string_codepoint_at(false_ascii, 3, &codepoint) ==
                         SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_string_codepoint_at(truncated, 0,
                                                     &codepoint) ==
                                SXML_STATUS_OK);
}

/* ------------------------------------------------------------- Iteration */

static void test_iterator_walks_codepoints_in_order(sxml_test_state *state) {
    sxml_string string = test_string_from(
        state, TEST_BYTES(0x61, 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F,
                          0x98, 0x80));
    static const uint32_t expected[] = {0x61U, 0xE9U, 0x20ACU, 0x1F600U};
    static const size_t expected_offsets[] = {1, 3, 6, 10};
    sxml_codepoint_iterator iterator = {.string = string, .byte_offset = 0};
    for (size_t codepoint_index = 0; codepoint_index < 4; codepoint_index++) {
        uint32_t codepoint = TEST_UNTOUCHED_CODEPOINT;
        SXML_TEST_REQUIRE(state,
                          sxml_codepoint_iterator_next(&iterator, &codepoint));
        SXML_TEST_EXPECT(state, codepoint == expected[codepoint_index]);
        SXML_TEST_EXPECT(state, iterator.byte_offset ==
                                    expected_offsets[codepoint_index]);
    }
    uint32_t codepoint = TEST_UNTOUCHED_CODEPOINT;
    SXML_TEST_EXPECT(state, !sxml_codepoint_iterator_next(&iterator,
                                                          &codepoint));
    SXML_TEST_EXPECT(state, !sxml_codepoint_iterator_next(&iterator,
                                                          &codepoint));
    SXML_TEST_EXPECT(state, codepoint == TEST_UNTOUCHED_CODEPOINT);
    SXML_TEST_EXPECT(state, iterator.byte_offset == 10);
}

static void test_iterator_over_empty_string_ends_at_once(
    sxml_test_state *state) {
    sxml_string empty = {
        .utf8 = {.bytes = NULL, .byte_count = 0},
        .codepoint_count = 0,
        .is_ascii = false,
    };
    sxml_codepoint_iterator iterator = {.string = empty, .byte_offset = 0};
    uint32_t codepoint = TEST_UNTOUCHED_CODEPOINT;
    SXML_TEST_EXPECT(state, !sxml_codepoint_iterator_next(&iterator,
                                                          &codepoint));
    SXML_TEST_EXPECT(state, codepoint == TEST_UNTOUCHED_CODEPOINT);
}

static void test_iterator_counts_match_validation(sxml_test_state *state) {
    sxml_string string = test_string_from(
        state, TEST_BYTES(0xE6, 0x97, 0xA5, 0xE6, 0x9C, 0xAC, 0x20, 0xC3,
                          0xBC, 0xF0, 0x90, 0x8D, 0x88));
    sxml_codepoint_iterator iterator = {.string = string, .byte_offset = 0};
    uint32_t codepoint = 0;
    size_t codepoint_count = 0;
    while (sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        codepoint_count++;
    }
    SXML_TEST_EXPECT(state, codepoint_count == string.codepoint_count);
    SXML_TEST_EXPECT(state, codepoint_count == 5);
    SXML_TEST_EXPECT(state, codepoint == 0x10348U);
}

static void test_iterator_stays_within_truncated_bytes(
    sxml_test_state *state) {
    /* Ill-formed bytes give unspecified codepoints but the walk ends at the
       last byte; the sanitizers check that nothing past it is read. */
    sxml_string truncated = {
        .utf8 = TEST_BYTES(0x80, 0xE2, 0x82),
        .codepoint_count = 2,
        .is_ascii = false,
    };
    sxml_codepoint_iterator iterator = {.string = truncated,
                                        .byte_offset = 0};
    uint32_t codepoint = 0;
    size_t step_count = 0;
    while (sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        step_count++;
    }
    SXML_TEST_EXPECT(state, step_count == 2);
    SXML_TEST_EXPECT(state, iterator.byte_offset == 3);
}

/* ------------------------------------------------------------ Comparison */

static void test_compare_orders_by_codepoint(sxml_test_state *state) {
    sxml_string apple =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL("apple"));
    sxml_string apricot =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL("apricot"));
    SXML_TEST_EXPECT(state,
                     sxml_string_compare(apple, apricot) == SXML_ORDERING_LESS);
    SXML_TEST_EXPECT(state, sxml_string_compare(apricot, apple) ==
                                SXML_ORDERING_GREATER);
    SXML_TEST_EXPECT(state,
                     sxml_string_compare(apple, apple) == SXML_ORDERING_EQUAL);
    /* Uppercase letters come before lowercase ones: no case folding. */
    sxml_string upper =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL("Zebra"));
    SXML_TEST_EXPECT(state,
                     sxml_string_compare(upper, apple) == SXML_ORDERING_LESS);
}

static void test_compare_puts_proper_prefix_first(sxml_test_state *state) {
    sxml_string prefix =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL("ab"));
    sxml_string longer =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL("abc"));
    sxml_string empty = {
        .utf8 = {.bytes = NULL, .byte_count = 0},
        .codepoint_count = 0,
        .is_ascii = false,
    };
    SXML_TEST_EXPECT(state,
                     sxml_string_compare(prefix, longer) == SXML_ORDERING_LESS);
    SXML_TEST_EXPECT(state, sxml_string_compare(longer, prefix) ==
                                SXML_ORDERING_GREATER);
    SXML_TEST_EXPECT(state,
                     sxml_string_compare(empty, prefix) == SXML_ORDERING_LESS);
    SXML_TEST_EXPECT(state,
                     sxml_string_compare(empty, empty) == SXML_ORDERING_EQUAL);
}

static void test_compare_orders_across_encoded_lengths(
    sxml_test_state *state) {
    /* Ascending codepoints U+007F, U+0080, U+07FF, U+0800, U+FFFF,
       U+10000, U+10FFFF; U+FFFD < U+10000 even though a UTF-16 order would
       put a surrogate-pair codepoint before U+FFFD. */
    sxml_string ascending[] = {
        test_string_from(state, TEST_BYTES(0x7F)),
        test_string_from(state, TEST_BYTES(0xC2, 0x80)),
        test_string_from(state, TEST_BYTES(0xDF, 0xBF)),
        test_string_from(state, TEST_BYTES(0xE0, 0xA0, 0x80)),
        test_string_from(state, TEST_BYTES(0xEF, 0xBF, 0xBD)),
        test_string_from(state, TEST_BYTES(0xF0, 0x90, 0x80, 0x80)),
        test_string_from(state, TEST_BYTES(0xF4, 0x8F, 0xBF, 0xBF)),
    };
    for (size_t first_index = 0; first_index < SXML_TEST_COUNT(ascending);
         first_index++) {
        for (size_t second_index = 0;
             second_index < SXML_TEST_COUNT(ascending); second_index++) {
            sxml_ordering expected = first_index < second_index
                                         ? SXML_ORDERING_LESS
                                     : first_index == second_index
                                         ? SXML_ORDERING_EQUAL
                                         : SXML_ORDERING_GREATER;
            SXML_TEST_EXPECT(state, sxml_string_compare(
                                        ascending[first_index],
                                        ascending[second_index]) == expected);
        }
    }
}

static void test_equals_compares_codepoints(sxml_test_state *state) {
    static const uint8_t other_bytes[] = {0x63, 0x61, 0x66, 0xC3, 0xA9};
    sxml_string cafe = test_string_from(
        state, TEST_BYTES(0x63, 0x61, 0x66, 0xC3, 0xA9));
    sxml_string same = test_string_from(
        state, (sxml_byte_view){.bytes = other_bytes, .byte_count = 5});
    sxml_string plain =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL("cafe"));
    sxml_string empty = {
        .utf8 = {.bytes = NULL, .byte_count = 0},
        .codepoint_count = 0,
        .is_ascii = false,
    };
    sxml_string empty_literal =
        test_string_from(state, SXML_BYTE_VIEW_FROM_LITERAL(""));
    SXML_TEST_EXPECT(state, sxml_string_equals(cafe, same));
    SXML_TEST_EXPECT(state, !sxml_string_equals(cafe, plain));
    SXML_TEST_EXPECT(state, !sxml_string_equals(plain, empty));
    SXML_TEST_EXPECT(state, sxml_string_equals(empty, empty_literal));
}

static const sxml_test_case string_test_cases[] = {
    {.name = "validate_accepts_empty_input",
     .function = test_validate_accepts_empty_input},
    {.name = "validate_counts_ascii", .function = test_validate_counts_ascii},
    {.name = "validate_accepts_nul_and_delete",
     .function = test_validate_accepts_nul_and_delete},
    {.name = "validate_counts_every_encoded_length",
     .function = test_validate_counts_every_encoded_length},
    {.name = "validate_accepts_boundaries_of_each_range",
     .function = test_validate_accepts_boundaries_of_each_range},
    {.name = "validate_rejects_overlong_forms",
     .function = test_validate_rejects_overlong_forms},
    {.name = "validate_rejects_surrogates",
     .function = test_validate_rejects_surrogates},
    {.name = "validate_rejects_codepoints_above_maximum",
     .function = test_validate_rejects_codepoints_above_maximum},
    {.name = "validate_rejects_stray_continuation_bytes",
     .function = test_validate_rejects_stray_continuation_bytes},
    {.name = "validate_rejects_truncated_sequences",
     .function = test_validate_rejects_truncated_sequences},
    {.name = "validate_describes_valid_prefix",
     .function = test_validate_describes_valid_prefix},
    {.name = "validate_accepts_every_scalar_value",
     .function = test_validate_accepts_every_scalar_value},
    {.name = "validate_rejects_every_ill_formed_pair",
     .function = test_validate_rejects_every_ill_formed_pair},
    {.name = "from_utf8_borrows_bytes_and_counts",
     .function = test_from_utf8_borrows_bytes_and_counts},
    {.name = "from_utf8_rejects_and_keeps_output",
     .function = test_from_utf8_rejects_and_keeps_output},
    {.name = "from_utf8_accepts_empty_input",
     .function = test_from_utf8_accepts_empty_input},
    {.name = "codepoint_at_reads_ascii",
     .function = test_codepoint_at_reads_ascii},
    {.name = "codepoint_at_decodes_every_encoded_length",
     .function = test_codepoint_at_decodes_every_encoded_length},
    {.name = "codepoint_at_rejects_index_and_keeps_output",
     .function = test_codepoint_at_rejects_index_and_keeps_output},
    {.name = "codepoint_at_works_without_ascii_flag",
     .function = test_codepoint_at_works_without_ascii_flag},
    {.name = "codepoint_at_stays_within_inconsistent_string",
     .function = test_codepoint_at_stays_within_inconsistent_string},
    {.name = "iterator_walks_codepoints_in_order",
     .function = test_iterator_walks_codepoints_in_order},
    {.name = "iterator_over_empty_string_ends_at_once",
     .function = test_iterator_over_empty_string_ends_at_once},
    {.name = "iterator_counts_match_validation",
     .function = test_iterator_counts_match_validation},
    {.name = "iterator_stays_within_truncated_bytes",
     .function = test_iterator_stays_within_truncated_bytes},
    {.name = "compare_orders_by_codepoint",
     .function = test_compare_orders_by_codepoint},
    {.name = "compare_puts_proper_prefix_first",
     .function = test_compare_puts_proper_prefix_first},
    {.name = "compare_orders_across_encoded_lengths",
     .function = test_compare_orders_across_encoded_lengths},
    {.name = "equals_compares_codepoints",
     .function = test_equals_compares_codepoints},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "string",
            .cases = string_test_cases,
            .case_count = SXML_TEST_COUNT(string_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
