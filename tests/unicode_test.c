/*
 * Tests for src/unicode_internal.h: combining classes, decompositions and
 * composites, the normalization forms (with lines of Unicode 16.0
 * NormalizationTest.txt), grapheme clusters (with lines of
 * GraphemeBreakTest.txt and the examples of F&O 4.0, fn:graphemes) and
 * character reference names. The whole of both test files was run against
 * this code once (packet 2.4, docs/LOG.md); they are not vendored.
 *
 * Golden file: tests/regex_test.c.
 */
#include "test_runner.h"
#include "unicode_internal.h"

#include <string.h>

/* ---------------------------------------------------------------- Helpers */

static sxml_string test_string(const char *text) {
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                          .codepoint_count = 0,
                          .is_ascii = true};
    /* Test texts are well-formed UTF-8; a failure leaves the empty string
       and the test's expectations fail. */
    if (sxml_string_from_utf8(sxml_byte_view_from_c_string(text), &string) !=
        SXML_STATUS_OK) {
        string.codepoint_count = 0;
    }
    return string;
}

/* A normalization of text through a counting allocator. */
typedef struct test_normalization {
    const char *input_text;
    sxml_unicode_normalization_form normalization_form;
} test_normalization;

/* True when normalizing gives expected_text and leaves nothing
   allocated. */
static bool test_normalizes_to(test_normalization normalization,
                               const char *expected_text) {
    sxml_counting_allocator *counting_allocator = NULL;
    if (sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){
                .backing_allocator = sxml_default_allocator(),
                .failing_allocation_number = 0,
            },
            &counting_allocator) != SXML_STATUS_OK) {
        return false;
    }
    sxml_unicode_normalized_text text = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_unicode_normalize(
        &(sxml_unicode_normalization_request){
            .allocator = sxml_counting_allocator_as_allocator(counting_allocator),
            .normalization_form = normalization.normalization_form,
            .input = test_string(normalization.input_text)},
        &text);
    bool is_expected =
        status == SXML_STATUS_OK &&
        sxml_byte_view_equals(sxml_unicode_normalized_text_view(&text),
                              sxml_byte_view_from_c_string(expected_text));
    sxml_unicode_normalized_text_release(&text);
    is_expected = is_expected && sxml_counting_allocator_read_counts(counting_allocator)
                                         .live_allocation_count == 0;
    sxml_counting_allocator_destroy(counting_allocator);
    return is_expected;
}

/* The five forms of one NormalizationTest.txt line: source, NFC, NFD,
   NFKC, NFKD (columns c1 to c5). */
typedef struct test_normalization_line {
    const char *source_text;
    const char *nfc_text;
    const char *nfd_text;
    const char *nfkc_text;
    const char *nfkd_text;
} test_normalization_line;

static bool test_line_holds(test_normalization_line line) {
    return test_normalizes_to(
               (test_normalization){.input_text = line.source_text,
                                    .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFC},
               line.nfc_text) &&
           test_normalizes_to(
               (test_normalization){.input_text = line.source_text,
                                    .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFD},
               line.nfd_text) &&
           test_normalizes_to(
               (test_normalization){.input_text = line.source_text,
                                    .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFKC},
               line.nfkc_text) &&
           test_normalizes_to(
               (test_normalization){.input_text = line.source_text,
                                    .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFKD},
               line.nfkd_text);
}

/* The grapheme clusters of text joined with "|". */
static bool test_clusters_are(const char *text, const char *expected_text) {
    const sxml_string string = test_string(text);
    char joined[256];
    size_t joined_count = 0;
    sxml_grapheme_cluster_iterator iterator = {.string = string, .byte_offset = 0};
    sxml_byte_range cluster = {.byte_offset = 0, .byte_count = 0};
    while (sxml_grapheme_cluster_iterator_next(&iterator, &cluster)) {
        if (cluster.byte_count == 0 || joined_count + cluster.byte_count + 1 >= sizeof(joined)) {
            return false;
        }
        if (joined_count > 0) {
            joined[joined_count] = '|';
            joined_count += 1;
        }
        memcpy(&joined[joined_count], &string.utf8.bytes[cluster.byte_offset],
               cluster.byte_count);
        joined_count += cluster.byte_count;
    }
    joined[joined_count] = '\0';
    return strcmp(joined, expected_text) == 0;
}

/* Text being built in a fixed array. */
typedef struct test_text {
    char bytes[512];
    size_t byte_count;
} test_text;

/* Appends piece (NUL-terminated) to text; drops what does not fit, so
   the test's expectations fail. */
static void test_append(test_text *text, const char *piece) {
    const size_t piece_count = strlen(piece);
    if (text->byte_count + piece_count < sizeof(text->bytes)) {
        memcpy(&text->bytes[text->byte_count], piece, piece_count);
        text->byte_count += piece_count;
    }
    text->bytes[text->byte_count] = '\0';
}

/* ------------------------------------------------------------- Properties */

static void test_combining_classes(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, sxml_unicode_get_combining_class('a') == 0);
    SXML_TEST_EXPECT(state, sxml_unicode_get_combining_class(0x0301) == 230);
    SXML_TEST_EXPECT(state, sxml_unicode_get_combining_class(0x0316) == 220);
    SXML_TEST_EXPECT(state, sxml_unicode_get_combining_class(0x05B0) == 10);
    SXML_TEST_EXPECT(state, sxml_unicode_get_combining_class(0x094D) == 9);
    SXML_TEST_EXPECT(state, sxml_unicode_get_combining_class(0x1D16D) == 226);
    SXML_TEST_EXPECT(state, sxml_unicode_get_combining_class(0x10FFFF) == 0);
}

static bool test_decomposes_to(sxml_unicode_decomposition_request request,
                               const uint32_t *expected, size_t expected_count) {
    sxml_unicode_decomposition decomposition = {.codepoint_count = 0};
    sxml_unicode_decompose(request, &decomposition);
    return decomposition.codepoint_count == expected_count &&
           memcmp(decomposition.codepoints, expected, expected_count * sizeof(uint32_t)) == 0;
}

static void test_decompositions(sxml_test_state *state) {
    static const uint32_t a_ring[] = {0x41, 0x30A};
    static const uint32_t long_s_dot[] = {0x17F, 0x307};
    static const uint32_t s_dot[] = {0x73, 0x307};
    static const uint32_t hangul_lv[] = {0x1100, 0x1161};
    static const uint32_t gak[] = {0x1100, 0x1161, 0x11A8};
    static const uint32_t plain[] = {0x41};
    const sxml_unicode_decomposition_kind canonical = SXML_UNICODE_DECOMPOSITION_KIND_CANONICAL;
    const sxml_unicode_decomposition_kind compatibility =
        SXML_UNICODE_DECOMPOSITION_KIND_COMPATIBILITY;
    SXML_TEST_EXPECT(state, test_decomposes_to(
                                (sxml_unicode_decomposition_request){
                                    .codepoint = 0xC5, .decomposition_kind = canonical},
                                a_ring, 2));
    /* U+212B ANGSTROM SIGN decomposes through U+00C5. */
    SXML_TEST_EXPECT(state, test_decomposes_to(
                                (sxml_unicode_decomposition_request){
                                    .codepoint = 0x212B, .decomposition_kind = canonical},
                                a_ring, 2));
    SXML_TEST_EXPECT(state, test_decomposes_to(
                                (sxml_unicode_decomposition_request){
                                    .codepoint = 0x1E9B, .decomposition_kind = canonical},
                                long_s_dot, 2));
    SXML_TEST_EXPECT(state, test_decomposes_to(
                                (sxml_unicode_decomposition_request){
                                    .codepoint = 0x1E9B, .decomposition_kind = compatibility},
                                s_dot, 2));
    SXML_TEST_EXPECT(state, test_decomposes_to(
                                (sxml_unicode_decomposition_request){
                                    .codepoint = 0xAC00, .decomposition_kind = compatibility},
                                hangul_lv, 2));
    SXML_TEST_EXPECT(state, test_decomposes_to(
                                (sxml_unicode_decomposition_request){
                                    .codepoint = 0xAC01, .decomposition_kind = canonical},
                                gak, 3));
    SXML_TEST_EXPECT(state, test_decomposes_to(
                                (sxml_unicode_decomposition_request){
                                    .codepoint = 0x41, .decomposition_kind = compatibility},
                                plain, 1));
    sxml_unicode_decomposition longest = {.codepoint_count = 0};
    sxml_unicode_decompose(
        (sxml_unicode_decomposition_request){.codepoint = 0xFDFA,
                                             .decomposition_kind = compatibility},
        &longest);
    SXML_TEST_EXPECT(state,
                     longest.codepoint_count == SXML_UNICODE_MAXIMUM_DECOMPOSITION_LENGTH);
    sxml_unicode_decompose(
        (sxml_unicode_decomposition_request){.codepoint = 0xFDFA,
                                             .decomposition_kind = canonical},
        &longest);
    SXML_TEST_EXPECT(state, longest.codepoint_count == 1 && longest.codepoints[0] == 0xFDFA);
}

static void test_composites(sxml_test_state *state) {
    uint32_t composite = 0;
    SXML_TEST_EXPECT(state, sxml_unicode_compose((sxml_unicode_codepoint_pair){
                                                     .first_codepoint = 0x41,
                                                     .second_codepoint = 0x30A},
                                                 &composite) &&
                                composite == 0xC5);
    SXML_TEST_EXPECT(state, sxml_unicode_compose((sxml_unicode_codepoint_pair){
                                                     .first_codepoint = 0x1100,
                                                     .second_codepoint = 0x1161},
                                                 &composite) &&
                                composite == 0xAC00);
    SXML_TEST_EXPECT(state, sxml_unicode_compose((sxml_unicode_codepoint_pair){
                                                     .first_codepoint = 0xAC00,
                                                     .second_codepoint = 0x11A8},
                                                 &composite) &&
                                composite == 0xAC01);
    composite = 7;
    /* U+0958 is a composition exclusion; U+AC01 already has a trailing
       consonant; U+11A7 is no trailing consonant. */
    SXML_TEST_EXPECT(state, !sxml_unicode_compose((sxml_unicode_codepoint_pair){
                                                      .first_codepoint = 0x915,
                                                      .second_codepoint = 0x93C},
                                                  &composite));
    SXML_TEST_EXPECT(state, !sxml_unicode_compose((sxml_unicode_codepoint_pair){
                                                      .first_codepoint = 0xAC01,
                                                      .second_codepoint = 0x11A8},
                                                  &composite));
    SXML_TEST_EXPECT(state, !sxml_unicode_compose((sxml_unicode_codepoint_pair){
                                                      .first_codepoint = 0xAC00,
                                                      .second_codepoint = 0x11A7},
                                                  &composite));
    SXML_TEST_EXPECT(state, composite == 7);
}

static void test_composing_characters(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, sxml_unicode_is_composing_character(0x0301));
    SXML_TEST_EXPECT(state, sxml_unicode_is_composing_character(0x0B57));
    SXML_TEST_EXPECT(state, sxml_unicode_is_composing_character(0x1161));
    SXML_TEST_EXPECT(state, sxml_unicode_is_composing_character(0x11A8));
    SXML_TEST_EXPECT(state, !sxml_unicode_is_composing_character('a'));
    SXML_TEST_EXPECT(state, !sxml_unicode_is_composing_character(0x1100));
    SXML_TEST_EXPECT(state, !sxml_unicode_is_composing_character(0x11A7));
}

/* ---------------------------------------------------------- Normalization */

static void test_normalization_test_lines(sxml_test_state *state) {
    /* NormalizationTest.txt, part 0 and part 1. */
    static const test_normalization_line lines[] = {
        /* 1E0A; 1E0A; 0044 0307; 1E0A; 0044 0307 */
        {"\u1E0A", "\u1E0A", "D\u0307", "\u1E0A", "D\u0307"},
        /* 1E0C 0307; 1E0C 0307; 0044 0323 0307; ... */
        {"\u1E0A\u0323", "\u1E0C\u0307", "D\u0323\u0307", "\u1E0C\u0307", "D\u0323\u0307"},
        {"D\u0307\u0323", "\u1E0C\u0307", "D\u0323\u0307", "\u1E0C\u0307", "D\u0323\u0307"},
        /* 1E9B 0323: long s with dot above and dot below. */
        {"\u1E9B\u0323", "\u1E9B\u0323", "\u017F\u0323\u0307", "\u1E69", "s\u0323\u0307"},
        /* FB01 LATIN SMALL LIGATURE FI. */
        {"\uFB01", "\uFB01", "\uFB01", "fi", "fi"},
        /* Hangul: AC00 11A8 composes to AC01. */
        {"\uAC00\u11A8", "\uAC01", "\u1100\u1161\u11A8", "\uAC01", "\u1100\u1161\u11A8"},
        /* U+0958 is excluded from composition. */
        {"\u0958", "\u0915\u093C", "\u0915\u093C", "\u0915\u093C", "\u0915\u093C"},
        /* A blocked mark: 0061 0305 0301 (both class 230). */
        {"a\u0305\u0301", "a\u0305\u0301", "a\u0305\u0301", "a\u0305\u0301", "a\u0305\u0301"},
        /* Unassigned and private use codepoints are left alone. */
        {"\U000E0080\uE000", "\U000E0080\uE000", "\U000E0080\uE000", "\U000E0080\uE000",
         "\U000E0080\uE000"},
        {"", "", "", "", ""},
        {"plain ASCII", "plain ASCII", "plain ASCII", "plain ASCII", "plain ASCII"},
    };
    for (size_t line_index = 0; line_index < SXML_TEST_COUNT(lines); line_index += 1) {
        SXML_TEST_EXPECT(state, test_line_holds(lines[line_index]));
    }
}

/* Runs of more than sixteen non-starters take the counting sort. */
static void test_long_runs_of_marks(sxml_test_state *state) {
    test_text input = {.byte_count = 0};
    test_text decomposed = {.byte_count = 0};
    test_text composed = {.byte_count = 0};
    test_append(&input, "a");
    test_append(&decomposed, "a");
    test_append(&composed, "\u00E1");
    for (size_t pair_index = 0; pair_index < 20; pair_index += 1) {
        test_append(&input, "\u0301\u0316");
    }
    for (size_t mark_index = 0; mark_index < 20; mark_index += 1) {
        test_append(&decomposed, "\u0316");
        test_append(&composed, "\u0316");
    }
    for (size_t mark_index = 0; mark_index < 20; mark_index += 1) {
        test_append(&decomposed, "\u0301");
        if (mark_index > 0) {
            test_append(&composed, "\u0301");
        }
    }
    SXML_TEST_EXPECT(state, test_normalizes_to(
                                (test_normalization){
                                    .input_text = input.bytes,
                                    .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFD},
                                decomposed.bytes));
    SXML_TEST_EXPECT(state, test_normalizes_to(
                                (test_normalization){
                                    .input_text = input.bytes,
                                    .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFC},
                                composed.bytes));
}

static void test_fully_normalized(sxml_test_state *state) {
    const sxml_unicode_normalization_form fully =
        SXML_UNICODE_NORMALIZATION_FORM_FULLY_NORMALIZED;
    SXML_TEST_EXPECT(state, test_normalizes_to((test_normalization){.input_text = "\u0301a",
                                                                    .normalization_form = fully},
                                               " \u0301a"));
    SXML_TEST_EXPECT(state, test_normalizes_to((test_normalization){.input_text = "\u0B57",
                                                                    .normalization_form = fully},
                                               " \u0B57"));
    SXML_TEST_EXPECT(state, test_normalizes_to((test_normalization){.input_text = "e\u0301",
                                                                    .normalization_form = fully},
                                               "\u00E9"));
    SXML_TEST_EXPECT(state, test_normalizes_to((test_normalization){.input_text = "abc",
                                                                    .normalization_form = fully},
                                               "abc"));
}

static void test_invalid_requests(sxml_test_state *state) {
    sxml_unicode_normalized_text text = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(state,
                     sxml_unicode_normalize(
                         &(sxml_unicode_normalization_request){
                             .allocator = {.state = NULL, .operations = NULL},
                             .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFC,
                             .input = test_string("\u00E9")},
                         &text) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, text.bytes == NULL && text.byte_count == 0);
}

/* Normalizes with every allocation failing in turn (D23). */
static void test_allocation_failures_leave_nothing_behind(sxml_test_state *state) {
    test_text input = {.byte_count = 0};
    test_append(&input, "\uFDFA");
    for (size_t pair_index = 0; pair_index < 40; pair_index += 1) {
        test_append(&input, "\u0301\u0316");
    }
    for (uint64_t failing_allocation_number = 1;; failing_allocation_number++) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(state,
                          sxml_counting_allocator_create(
                              sxml_default_allocator(),
                              &(sxml_counting_allocator_configuration){
                                  .backing_allocator = sxml_default_allocator(),
                                  .failing_allocation_number = failing_allocation_number,
                              },
                              &counting_allocator) == SXML_STATUS_OK);
        sxml_unicode_normalized_text text = {.bytes = NULL, .byte_count = 0};
        const sxml_status status = sxml_unicode_normalize(
            &(sxml_unicode_normalization_request){
                .allocator = sxml_counting_allocator_as_allocator(counting_allocator),
                .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFKC,
                .input = test_string(input.bytes)},
            &text);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || text.bytes == NULL);
        sxml_unicode_normalized_text_release(&text);
        SXML_TEST_EXPECT(
            state,
            sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count == 0);
        sxml_counting_allocator_destroy(counting_allocator);
        if (status == SXML_STATUS_OK) {
            break;
        }
        SXML_TEST_REQUIRE(state, failing_allocation_number < 100);
    }
}

/* ------------------------------------------------------ Grapheme clusters */

static void test_grapheme_properties(sxml_test_state *state) {
    sxml_unicode_grapheme_properties properties;
    sxml_unicode_get_grapheme_properties(0x0D, &properties);
    SXML_TEST_EXPECT(state, properties.grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_CR);
    sxml_unicode_get_grapheme_properties(0xAC00, &properties);
    SXML_TEST_EXPECT(state, properties.grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_LV);
    sxml_unicode_get_grapheme_properties(0xAC01, &properties);
    SXML_TEST_EXPECT(state, properties.grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_LVT);
    sxml_unicode_get_grapheme_properties(0x094D, &properties);
    SXML_TEST_EXPECT(state, properties.conjunct_break == SXML_UNICODE_CONJUNCT_BREAK_LINKER);
    sxml_unicode_get_grapheme_properties(0x0915, &properties);
    SXML_TEST_EXPECT(state, properties.conjunct_break == SXML_UNICODE_CONJUNCT_BREAK_CONSONANT);
    sxml_unicode_get_grapheme_properties(0x1F476, &properties);
    SXML_TEST_EXPECT(state, properties.is_extended_pictographic);
    sxml_unicode_get_grapheme_properties('a', &properties);
    SXML_TEST_EXPECT(state, properties.grapheme_break == SXML_UNICODE_GRAPHEME_BREAK_OTHER &&
                                !properties.is_extended_pictographic &&
                                properties.conjunct_break == SXML_UNICODE_CONJUNCT_BREAK_NONE);
}

static void test_fn_graphemes_examples(sxml_test_state *state) {
    /* F&O 4.0, fn:graphemes, examples. */
    SXML_TEST_EXPECT(state, test_clusters_are("a\u0308b", "a\u0308|b"));
    SXML_TEST_EXPECT(state, test_clusters_are("", ""));
    SXML_TEST_EXPECT(state, test_clusters_are("\r\n", "\r\n"));
    SXML_TEST_EXPECT(state, test_clusters_are("\U0001F476\u200D\U0001F6D1",
                                              "\U0001F476\u200D\U0001F6D1"));
    SXML_TEST_EXPECT(state, test_clusters_are("\u0915\u0924", "\u0915|\u0924"));
    SXML_TEST_EXPECT(state, test_clusters_are("\u0915\u093C\u200D\u094D\u0924",
                                              "\u0915\u093C\u200D\u094D\u0924"));
}

static void test_grapheme_break_test_lines(sxml_test_state *state) {
    /* GraphemeBreakTest.txt (Unicode 16.0). */
    /* GB4, GB5: controls. */
    SXML_TEST_EXPECT(state, test_clusters_are("a\n\u0308", "a|\n|\u0308"));
    SXML_TEST_EXPECT(state, test_clusters_are("\r\r\n", "\r|\r\n"));
    /* GB6 to GB8: Hangul. */
    SXML_TEST_EXPECT(state, test_clusters_are("\u1100\u1161\u11A8\u1100", "\u1100\u1161\u11A8|\u1100"));
    SXML_TEST_EXPECT(state, test_clusters_are("\uAC00\u11A8\u11A8", "\uAC00\u11A8\u11A8"));
    SXML_TEST_EXPECT(state, test_clusters_are("\uAC01\u1161", "\uAC01|\u1161"));
    /* GB9a, GB9b: spacing mark, prepend. */
    SXML_TEST_EXPECT(state, test_clusters_are("\u0915\u0903", "\u0915\u0903"));
    SXML_TEST_EXPECT(state, test_clusters_are("\u0600a", "\u0600a"));
    SXML_TEST_EXPECT(state, test_clusters_are("\u0600\n", "\u0600|\n"));
    /* GB11: emoji ZWJ sequences, but not after a plain letter. */
    SXML_TEST_EXPECT(state, test_clusters_are("\u2701\u200D\u2701", "\u2701\u200D\u2701"));
    SXML_TEST_EXPECT(state, test_clusters_are("a\u200D\u2701", "a\u200D|\u2701"));
    SXML_TEST_EXPECT(state, test_clusters_are("\U0001F6D1\u0308\u200D\U0001F6D1",
                                              "\U0001F6D1\u0308\u200D\U0001F6D1"));
    /* GB12, GB13: regional indicators in pairs. */
    SXML_TEST_EXPECT(state, test_clusters_are("\U0001F1E6\U0001F1E7\U0001F1E8b",
                                              "\U0001F1E6\U0001F1E7|\U0001F1E8|b"));
    SXML_TEST_EXPECT(state, test_clusters_are("a\U0001F1E6\U0001F1E7\U0001F1E8\U0001F1E9",
                                              "a|\U0001F1E6\U0001F1E7|\U0001F1E8\U0001F1E9"));
    /* GB9c: a linker joins consonants; a linker without a consonant
       before it does not. */
    SXML_TEST_EXPECT(state, test_clusters_are("\u0915\u094D\u0924", "\u0915\u094D\u0924"));
    SXML_TEST_EXPECT(state, test_clusters_are("\u0915\u094D\u094D\u0924",
                                              "\u0915\u094D\u094D\u0924"));
    SXML_TEST_EXPECT(state, test_clusters_are("a\u094D\u0924", "a\u094D|\u0924"));
}

/* ------------------------------------------- Character reference names */

static bool test_reference_is(const char *name, uint32_t first_codepoint,
                              uint32_t second_codepoint) {
    sxml_unicode_character_reference_value value = {.codepoints = {0, 0},
                                                    .codepoint_count = 0};
    if (!sxml_unicode_find_character_reference(sxml_byte_view_from_c_string(name), &value)) {
        return false;
    }
    return value.codepoints[0] == first_codepoint &&
           value.codepoint_count == (second_codepoint == 0 ? 1U : 2U) &&
           (second_codepoint == 0 || value.codepoints[1] == second_codepoint);
}

static void test_character_references(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, test_reference_is("nbsp", 0xA0, 0));
    SXML_TEST_EXPECT(state, test_reference_is("pi", 0x3C0, 0));
    SXML_TEST_EXPECT(state, test_reference_is("NotEqualTilde", 0x2242, 0x338));
    SXML_TEST_EXPECT(state, test_reference_is("NewLine", 0x0A, 0));
    SXML_TEST_EXPECT(state, test_reference_is("AElig", 0xC6, 0));
    SXML_TEST_EXPECT(state, test_reference_is("zwnj", 0x200C, 0));
    SXML_TEST_EXPECT(state, test_reference_is("Aacute", 0xC1, 0));
    SXML_TEST_EXPECT(state, test_reference_is("aacute", 0xE1, 0));
    sxml_unicode_character_reference_value value = {.codepoints = {7, 7},
                                                    .codepoint_count = 7};
    static const char *const unknown_names[] = {"NBSP", "", "nbsp;", "&nbsp", "Unknown",
                                                "nbs", "nbspx", "zzzz", "AAAA"};
    for (size_t name_index = 0; name_index < SXML_TEST_COUNT(unknown_names); name_index += 1) {
        SXML_TEST_EXPECT(state, !sxml_unicode_find_character_reference(
                                    sxml_byte_view_from_c_string(unknown_names[name_index]),
                                    &value));
    }
    SXML_TEST_EXPECT(state, value.codepoint_count == 7);
}

/* ------------------------------------------------------------------ Suite */

static const sxml_test_case unicode_test_cases[] = {
    {.name = "combining_classes", .function = test_combining_classes},
    {.name = "decompositions", .function = test_decompositions},
    {.name = "composites", .function = test_composites},
    {.name = "composing_characters", .function = test_composing_characters},
    {.name = "normalization_test_lines", .function = test_normalization_test_lines},
    {.name = "long_runs_of_marks", .function = test_long_runs_of_marks},
    {.name = "fully_normalized", .function = test_fully_normalized},
    {.name = "invalid_requests", .function = test_invalid_requests},
    {.name = "allocation_failures_leave_nothing_behind",
     .function = test_allocation_failures_leave_nothing_behind},
    {.name = "grapheme_properties", .function = test_grapheme_properties},
    {.name = "fn_graphemes_examples", .function = test_fn_graphemes_examples},
    {.name = "grapheme_break_test_lines", .function = test_grapheme_break_test_lines},
    {.name = "character_references", .function = test_character_references},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "unicode",
            .cases = unicode_test_cases,
            .case_count = SXML_TEST_COUNT(unicode_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
