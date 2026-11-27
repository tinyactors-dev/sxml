/*
 * Tests for the typed values of schema-informed EXI streams
 * (src/exi_datatype.c; EXI 1.0, 7.1 and 7.2): which texts are values of a
 * datatype, the text a value decodes to, and the bits between, for each
 * representation, with integers past 64 bits and date-times with
 * timezones. The forms are the oracle's (EXIficient 1.0.4).
 */
#include "exi_internal.h"
#include "test_runner.h"

#include <string.h>

/* Grammars that hold the one datatype tested (index 0). */
typedef struct test_datatype {
    sxml_exi_schema_grammars grammars;
    sxml_exi_datatype datatype;
} test_datatype;

static void make_datatype(test_datatype *test, sxml_exi_datatype datatype) {
    test->datatype = datatype;
    test->grammars = (sxml_exi_schema_grammars){.allocator = sxml_default_allocator(),
                                                .datatypes = &test->datatype,
                                                .datatype_count = 1,
                                                .text = {.allocator = sxml_default_allocator()}};
}

static sxml_byte_view text_of(const char *text) {
    return (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = strlen(text)};
}

static bool buffer_is(const sxml_exi_buffer *buffer, const char *expected) {
    const size_t length = strlen(expected);
    return buffer->byte_count == length && (length == 0 || memcmp(buffer->bytes, expected, length) == 0);
}

/* The canonical text of text in the datatype; "" with is_valid false for
   no value. */
typedef struct test_canonical {
    sxml_exi_buffer buffer;
    bool is_valid;
} test_canonical;

static void canonicalize(const test_datatype *test, const char *text, test_canonical *canonical) {
    canonical->buffer = (sxml_exi_buffer){.allocator = sxml_default_allocator()};
    sxml_exi_canonical_output output = {.buffer = &canonical->buffer};
    const sxml_status status = sxml_exi_canonicalize_value(
        &test->grammars, (sxml_exi_value_text){.datatype = 0, .text = text_of(text)}, &output);
    canonical->is_valid = status == SXML_STATUS_OK && output.is_valid;
}

/* Writes text bit-packed, reads it back: expected is what it decodes to. */
static bool round_trips(const test_datatype *test, const char *text, const char *expected) {
    sxml_exi_bit_writer writer = {.allocator = sxml_default_allocator()};
    sxml_exi_validity validity = {0};
    sxml_status status = sxml_exi_write_typed_value(&(sxml_exi_typed_write){.grammars = &test->grammars, .writer = &writer},
                                                    (sxml_exi_value_text){.datatype = 0, .text = text_of(text)}, &validity);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_writer_align(&writer);
    }
    sxml_exi_buffer output = {.allocator = sxml_default_allocator()};
    sxml_exi_bit_reader reader = {.input = sxml_exi_writer_bytes(&writer)};
    if (status == SXML_STATUS_OK && validity.is_valid) {
        status = sxml_exi_read_typed_value(
            &(sxml_exi_typed_read){.grammars = &test->grammars, .reader = &reader, .output = &output}, 0);
    }
    const bool is_expected = status == SXML_STATUS_OK && validity.is_valid && buffer_is(&output, expected);
    sxml_exi_buffer_release(&output);
    sxml_exi_writer_release(&writer);
    return is_expected;
}

/* The bytes text writes to, bit-packed and padded. */
static bool writes_bytes(const test_datatype *test, const char *text, sxml_byte_view expected) {
    sxml_exi_bit_writer writer = {.allocator = sxml_default_allocator()};
    sxml_exi_validity validity = {0};
    sxml_status status = sxml_exi_write_typed_value(&(sxml_exi_typed_write){.grammars = &test->grammars, .writer = &writer},
                                                    (sxml_exi_value_text){.datatype = 0, .text = text_of(text)}, &validity);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_writer_align(&writer);
    }
    const sxml_byte_view bytes = sxml_exi_writer_bytes(&writer);
    const bool is_expected = status == SXML_STATUS_OK && validity.is_valid && bytes.byte_count == expected.byte_count &&
                             memcmp(bytes.bytes, expected.bytes, expected.byte_count) == 0;
    sxml_exi_writer_release(&writer);
    return is_expected;
}

static bool canonical_is(const test_datatype *test, const char *text, const char *expected) {
    test_canonical canonical = {0};
    canonicalize(test, text, &canonical);
    const bool is_expected = canonical.is_valid && buffer_is(&canonical.buffer, expected);
    sxml_exi_buffer_release(&canonical.buffer);
    return is_expected;
}

static bool is_invalid(const test_datatype *test, const char *text) {
    test_canonical canonical = {0};
    canonicalize(test, text, &canonical);
    sxml_exi_buffer_release(&canonical.buffer);
    return !canonical.is_valid;
}

/* ------------------------------------------------------------ Decimal */

static void test_decimal_texts_are_the_oracle_s(sxml_test_state *state) {
    test_datatype test = {0};
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_DECIMAL});
    SXML_TEST_EXPECT(state, canonical_is(&test, "123", "123.0"));
    SXML_TEST_EXPECT(state, canonical_is(&test, " -0.50 ", "-0.5"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "+.05", "0.05"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "-0.0", "0.0"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "007.100", "7.1"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "5."));
    SXML_TEST_EXPECT(state, is_invalid(&test, "1e5"));
    SXML_TEST_EXPECT(state, is_invalid(&test, ""));
    SXML_TEST_EXPECT(state, round_trips(&test, "-12345678901234567890123.000456", "-12345678901234567890123.000456"));
    /* 7.1.3: sign 0, integral 1, fraction digits reversed: 5 from ".5". */
    static const uint8_t half[] = {0x00, 0x82, 0x80};
    SXML_TEST_EXPECT(state, writes_bytes(&test, "1.5", (sxml_byte_view){.bytes = half, .byte_count = sizeof half}));
}

/* -------------------------------------------------------------- Float */

static void test_float_texts_are_mantissa_and_exponent(sxml_test_state *state) {
    test_datatype test = {0};
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_FLOAT});
    SXML_TEST_EXPECT(state, canonical_is(&test, "5.55", "555E-2"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "100", "1E2"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "1.50e3", "15E2"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "-0", "0E0"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "INF", "INF"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "-INF", "-INF"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "NaN", "NaN"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "1E16384"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "abc"));
    SXML_TEST_EXPECT(state, round_trips(&test, "-9.25E-7", "-925E-9"));
    SXML_TEST_EXPECT(state, round_trips(&test, "NaN", "NaN"));
}

/* ------------------------------------------------------------ Integer */

static void test_integers_have_any_number_of_digits(sxml_test_state *state) {
    test_datatype test = {0};
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_INTEGER});
    SXML_TEST_EXPECT(state, canonical_is(&test, "+0042", "42"));
    SXML_TEST_EXPECT(state, canonical_is(&test, "-0", "0"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "4.2"));
    SXML_TEST_EXPECT(state, round_trips(&test, "123456789012345678901234567890", "123456789012345678901234567890"));
    SXML_TEST_EXPECT(state, round_trips(&test, "-123456789012345678901234567890", "-123456789012345678901234567890"));
    SXML_TEST_EXPECT(state, round_trips(&test, "-18446744073709551616", "-18446744073709551616"));
    /* 7.1.5: -1 is sign 1 then magnitude less one, 0. */
    static const uint8_t minus_one[] = {0x80, 0x00};
    SXML_TEST_EXPECT(state, writes_bytes(&test, "-1", (sxml_byte_view){.bytes = minus_one, .byte_count = sizeof minus_one}));
}

static void test_unsigned_integers_refuse_negatives(sxml_test_state *state) {
    test_datatype test = {0};
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_UNSIGNED_INTEGER});
    SXML_TEST_EXPECT(state, is_invalid(&test, "-1"));
    SXML_TEST_EXPECT(state, round_trips(&test, "0", "0"));
    /* 7.1.6: 300 is 0xAC 0x02. */
    static const uint8_t three_hundred[] = {0xAC, 0x02};
    SXML_TEST_EXPECT(state, writes_bytes(&test, "300", (sxml_byte_view){.bytes = three_hundred, .byte_count = sizeof three_hundred}));
    SXML_TEST_EXPECT(state, round_trips(&test, "99999999999999999999999999999999", "99999999999999999999999999999999"));
}

static void test_bounded_integers_are_offsets(sxml_test_state *state) {
    sxml_exi_buffer lower = {.allocator = sxml_default_allocator()};
    sxml_exi_integer_range range = {.lower_bound = &lower};
    SXML_TEST_REQUIRE(state, sxml_exi_integer_range_of(&(sxml_exi_integer_bounds){.minimum_inclusive = text_of("-128"),
                                                                                  .maximum_inclusive = text_of("127")},
                                                       &range) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, range.datatype_kind == SXML_EXI_DATATYPE_BOUNDED_INTEGER);
    SXML_TEST_EXPECT(state, range.value_count == 256);
    SXML_TEST_EXPECT(state, buffer_is(&lower, "-128"));
    /* Exclusive bounds, and bounds past 64 bits. */
    SXML_TEST_REQUIRE(state, sxml_exi_integer_range_of(&(sxml_exi_integer_bounds){.minimum_exclusive = text_of("100000000000000000000"),
                                                                                  .maximum_exclusive = text_of("100000000000000000010")},
                                                       &range) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, range.datatype_kind == SXML_EXI_DATATYPE_BOUNDED_INTEGER);
    SXML_TEST_EXPECT(state, range.value_count == 9);
    SXML_TEST_EXPECT(state, buffer_is(&lower, "100000000000000000001"));
    /* 4097 values: no n-bit integer; a lower bound of 0: unsigned. */
    SXML_TEST_REQUIRE(state, sxml_exi_integer_range_of(&(sxml_exi_integer_bounds){.minimum_inclusive = text_of("0"),
                                                                                  .maximum_inclusive = text_of("4096")},
                                                       &range) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, range.datatype_kind == SXML_EXI_DATATYPE_UNSIGNED_INTEGER);
    SXML_TEST_REQUIRE(state, sxml_exi_integer_range_of(&(sxml_exi_integer_bounds){.maximum_inclusive = text_of("0")}, &range) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, range.datatype_kind == SXML_EXI_DATATYPE_INTEGER);
    sxml_exi_buffer_release(&lower);

    test_datatype test = {0};
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_BOUNDED_INTEGER,
                                             .bit_count = 8,
                                             .value_count = 256,
                                             .lower_bound = {.offset = 0, .byte_count = 4}});
    SXML_TEST_REQUIRE(state, sxml_exi_buffer_append(&test.grammars.text, text_of("-128")) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, is_invalid(&test, "128"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "-129"));
    SXML_TEST_EXPECT(state, round_trips(&test, "-128", "-128"));
    SXML_TEST_EXPECT(state, round_trips(&test, "+5", "5"));
    /* 7.1.9: 5 - (-128) = 133 in eight bits. */
    static const uint8_t offset[] = {133};
    SXML_TEST_EXPECT(state, writes_bytes(&test, "5", (sxml_byte_view){.bytes = offset, .byte_count = sizeof offset}));
    sxml_exi_buffer_release(&test.grammars.text);
}

/* ---------------------------------------------------------- Date-Time */

static void test_date_times_keep_their_fields(sxml_test_state *state) {
    test_datatype test = {0};
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_DATE_TIME,
                                             .date_time_kind = SXML_EXI_DATE_TIME_DATE_TIME});
    SXML_TEST_EXPECT(state, round_trips(&test, "1985-04-12T10:30:00", "1985-04-12T10:30:00"));
    SXML_TEST_EXPECT(state, round_trips(&test, "2000-03-23T07:41:34.0500-05:00", "2000-03-23T07:41:34.05-05:00"));
    SXML_TEST_EXPECT(state, round_trips(&test, "-0044-03-15T12:00:00Z", "-0044-03-15T12:00:00Z"));
    SXML_TEST_EXPECT(state, round_trips(&test, "1999-12-31T24:00:00+14:00", "2000-01-01T00:00:00+14:00"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "1999-12-31"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "1999-12-31T10:00:00+0500"));

    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_DATE_TIME,
                                             .date_time_kind = SXML_EXI_DATE_TIME_TIME});
    SXML_TEST_EXPECT(state, round_trips(&test, "13:20:00.000", "13:20:00"));
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_DATE_TIME,
                                             .date_time_kind = SXML_EXI_DATE_TIME_G_DAY});
    SXML_TEST_EXPECT(state, round_trips(&test, "---05Z", "---05Z"));
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_DATE_TIME,
                                             .date_time_kind = SXML_EXI_DATE_TIME_G_MONTH});
    SXML_TEST_EXPECT(state, round_trips(&test, "--11--", "--11"));
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_DATE_TIME,
                                             .date_time_kind = SXML_EXI_DATE_TIME_G_YEAR_MONTH});
    SXML_TEST_EXPECT(state, round_trips(&test, "2001-07-09:30", "2001-07-09:30"));
}

/* ----------------------------------------------- Binary and Boolean */

static void test_binary_values_are_their_bytes(sxml_test_state *state) {
    test_datatype test = {0};
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_BASE64_BINARY});
    SXML_TEST_EXPECT(state, round_trips(&test, " AQID\n BA== ", "AQIDBA=="));
    SXML_TEST_EXPECT(state, is_invalid(&test, "AQI"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "A==="));
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_HEX_BINARY});
    SXML_TEST_EXPECT(state, round_trips(&test, "0a1B", "0A1B"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "0a1"));
}

static void test_booleans_with_and_without_patterns(sxml_test_state *state) {
    test_datatype test = {0};
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_BOOLEAN});
    SXML_TEST_EXPECT(state, round_trips(&test, " 1 ", "true"));
    SXML_TEST_EXPECT(state, round_trips(&test, "false", "false"));
    SXML_TEST_EXPECT(state, is_invalid(&test, "yes"));
    make_datatype(&test, (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_PATTERN_BOOLEAN});
    SXML_TEST_EXPECT(state, round_trips(&test, "0", "0"));
    SXML_TEST_EXPECT(state, round_trips(&test, "true", "true"));
}

/* --------------------------------------------------------- Whitespace */

static void test_whitespace_facets(sxml_test_state *state) {
    sxml_exi_buffer output = {.allocator = sxml_default_allocator()};
    SXML_TEST_REQUIRE(state, sxml_exi_apply_whitespace(
                                 (sxml_exi_whitespace_request){.whitespace = SXML_EXI_WHITESPACE_COLLAPSE, .text = text_of(" a \t\n b  ")},
                                 &output) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, buffer_is(&output, "a b"));
    SXML_TEST_REQUIRE(state, sxml_exi_apply_whitespace(
                                 (sxml_exi_whitespace_request){.whitespace = SXML_EXI_WHITESPACE_REPLACE, .text = text_of("a\tb\n")},
                                 &output) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, buffer_is(&output, "a b "));
    size_t offset = 0;
    sxml_byte_view item = {0};
    const sxml_byte_view list = text_of("  one\ttwo  ");
    SXML_TEST_EXPECT(state, sxml_exi_next_list_item(list, &offset, &item) && item.byte_count == 3);
    SXML_TEST_EXPECT(state, sxml_exi_next_list_item(list, &offset, &item) && item.byte_count == 3);
    SXML_TEST_EXPECT(state, !sxml_exi_next_list_item(list, &offset, &item));
    sxml_exi_buffer_release(&output);
}

static const sxml_test_case exi_datatype_test_cases[] = {
    {.name = "decimal_texts_are_the_oracle_s", .function = test_decimal_texts_are_the_oracle_s},
    {.name = "float_texts_are_mantissa_and_exponent", .function = test_float_texts_are_mantissa_and_exponent},
    {.name = "integers_have_any_number_of_digits", .function = test_integers_have_any_number_of_digits},
    {.name = "unsigned_integers_refuse_negatives", .function = test_unsigned_integers_refuse_negatives},
    {.name = "bounded_integers_are_offsets", .function = test_bounded_integers_are_offsets},
    {.name = "date_times_keep_their_fields", .function = test_date_times_keep_their_fields},
    {.name = "binary_values_are_their_bytes", .function = test_binary_values_are_their_bytes},
    {.name = "booleans_with_and_without_patterns", .function = test_booleans_with_and_without_patterns},
    {.name = "whitespace_facets", .function = test_whitespace_facets},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "exi_datatype",
            .cases = exi_datatype_test_cases,
            .case_count = SXML_TEST_COUNT(exi_datatype_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
