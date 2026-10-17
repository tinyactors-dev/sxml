/*
 * Tests for src/decimal_internal.h: decimal arithmetic over libmpdec and
 * its decimal scope (D42, D49). Follows tests/memory_test.c: one case per
 * behaviour, and allocation-failure loops with the counting allocator.
 */
#include "decimal_internal.h"
#include "test_runner.h"

#include <float.h>
#include <math.h>
#include <stdalign.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__wasi__)
#include <pthread.h>
#endif

#define TEST_STORAGE_ALIGNMENT 16

/* ------------------------------------------------------------ Fixtures */

/* A counting allocator over the default allocator, placed on the stack. */
typedef struct counting_fixture {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[128];
    sxml_counting_allocator *counting_allocator;
} counting_fixture;

static sxml_status counting_fixture_initialize(
    counting_fixture *fixture, uint64_t failing_allocation_number) {
    return sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = fixture->storage,
                           .byte_count = sizeof(fixture->storage)},
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = failing_allocation_number,
        },
        &fixture->counting_allocator);
}

static sxml_allocation_counts counting_fixture_counts(
    const counting_fixture *fixture) {
    return sxml_counting_allocator_read_counts(fixture->counting_allocator);
}

static sxml_decimal_configuration configuration_of(sxml_allocator allocator) {
    return (sxml_decimal_configuration){
        .allocator = allocator,
        .maximum_digit_count = SXML_DECIMAL_DEFAULT_MAXIMUM_DIGIT_COUNT,
        .division_digit_count = SXML_DECIMAL_DEFAULT_DIVISION_DIGIT_COUNT,
    };
}

static sxml_decimal_configuration default_configuration(void) {
    return configuration_of(sxml_default_allocator());
}

/* An empty decimal, to fill or to detect an untouched output. */
static sxml_decimal empty_decimal(void) {
    sxml_decimal decimal;
    memset(&decimal, 0, sizeof(decimal));
    decimal.number.flags = MPD_STATIC | MPD_STATIC_DATA;
    return decimal;
}

static bool decimal_is_untouched(const sxml_decimal *decimal) {
    return decimal->number.data == NULL;
}

/* Parses text with the default configuration; false on failure. */
static bool parse(sxml_byte_view text, sxml_decimal *decimal) {
    sxml_decimal_configuration configuration = default_configuration();
    return sxml_decimal_parse(&configuration, text, decimal) == SXML_STATUS_OK;
}

static sxml_byte_view text_of(const char *c_string) {
    return sxml_byte_view_from_c_string(c_string);
}

/* A decimal and the canonical text it should have. */
typedef struct text_expectation {
    const sxml_decimal *decimal;
    sxml_byte_view expected_text;
} text_expectation;

/* True when the canonical text of the decimal is expected_text, and its
   byte count is reported alike. */
static bool has_text(text_expectation expectation) {
    uint8_t bytes[2048];
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (sxml_decimal_write_canonical_text(
            expectation.decimal,
            (sxml_memory_view){.bytes = bytes, .byte_count = sizeof(bytes)},
            &text) != SXML_STATUS_OK) {
        return false;
    }
    return sxml_decimal_canonical_text_byte_count(expectation.decimal) ==
               text.byte_count &&
           sxml_byte_view_equals(text, expectation.expected_text);
}

/* Parses text and checks its canonical text. */
static bool parses_to(sxml_byte_view text, sxml_byte_view expected_text) {
    sxml_decimal decimal = empty_decimal();
    if (!parse(text, &decimal)) {
        return false;
    }
    bool matches = has_text(
        (text_expectation){.decimal = &decimal, .expected_text = expected_text});
    sxml_decimal_finalize(&decimal);
    return matches;
}

/* The binary operations under test. */
typedef enum test_operation {
    SXML_TEST_OPERATION_ADD,
    SXML_TEST_OPERATION_SUBTRACT,
    SXML_TEST_OPERATION_MULTIPLY,
    SXML_TEST_OPERATION_DIVIDE,
    SXML_TEST_OPERATION_INTEGER_DIVIDE,
    SXML_TEST_OPERATION_MODULO,
} test_operation;

/* One binary operation to run. */
typedef struct operation_request {
    test_operation operation;
    sxml_decimal_operands operands;
} operation_request;

static sxml_status run_operation(const sxml_decimal_configuration *configuration,
                                 const operation_request *request,
                                 sxml_decimal *result) {
    const sxml_decimal_operands *operands = &request->operands;
    switch (request->operation) {
    case SXML_TEST_OPERATION_ADD:
        return sxml_decimal_add(configuration, operands, result);
    case SXML_TEST_OPERATION_SUBTRACT:
        return sxml_decimal_subtract(configuration, operands, result);
    case SXML_TEST_OPERATION_MULTIPLY:
        return sxml_decimal_multiply(configuration, operands, result);
    case SXML_TEST_OPERATION_DIVIDE:
        return sxml_decimal_divide(configuration, operands, result);
    case SXML_TEST_OPERATION_INTEGER_DIVIDE:
        return sxml_decimal_integer_divide(configuration, operands, result);
    case SXML_TEST_OPERATION_MODULO:
        return sxml_decimal_modulo(configuration, operands, result);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* One binary operation on two texts and its expected canonical text. */
typedef struct arithmetic_case {
    test_operation operation;
    const char *left_text;
    const char *right_text;
    const char *expected_text;
} arithmetic_case;

/* Runs one arithmetic case with configuration; true when it gives the
   expected text. */
static bool arithmetic_case_holds(
    const sxml_decimal_configuration *configuration,
    const arithmetic_case *test_case) {
    sxml_decimal left = empty_decimal();
    sxml_decimal right = empty_decimal();
    sxml_decimal result = empty_decimal();
    bool holds = false;
    if (sxml_decimal_parse(configuration, text_of(test_case->left_text),
                           &left) == SXML_STATUS_OK &&
        sxml_decimal_parse(configuration, text_of(test_case->right_text),
                           &right) == SXML_STATUS_OK &&
        run_operation(configuration,
                      &(operation_request){
                          .operation = test_case->operation,
                          .operands = {.left_operand = &left,
                                       .right_operand = &right},
                      },
                      &result) == SXML_STATUS_OK) {
        holds = has_text((text_expectation){
            .decimal = &result,
            .expected_text = text_of(test_case->expected_text)});
    }
    sxml_decimal_finalize(&result);
    sxml_decimal_finalize(&right);
    sxml_decimal_finalize(&left);
    return holds;
}

/* -------------------------------------------- Parsing and canonical text */

static void test_parse_gives_canonical_text(sxml_test_state *state) {
    static const struct {
        const char *input;
        const char *canonical;
    } cases[] = {
        {"0", "0"},
        {"-0.0", "0"},
        {"+1.50", "1.5"},
        {"007", "7"},
        {".5", "0.5"},
        {"5.", "5"},
        {"-0.000120", "-0.00012"},
        {"100", "100"},
        {"-12.340", "-12.34"},
        {"123456789012345678901234567890.1234567890123456789",
         "123456789012345678901234567890.1234567890123456789"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        SXML_TEST_EXPECT(state, parses_to(text_of(cases[case_index].input),
                                          text_of(cases[case_index].canonical)));
    }
}

static void test_parse_rejects_other_lexical_forms(sxml_test_state *state) {
    static const char *const inputs[] = {
        "", "+", "-", ".", "1e5", " 1", "1 ", "1..2", "NaN", "INF", "--1",
        "1,5", "+-1", "0x10", "1.2.3",
    };
    sxml_decimal_configuration configuration = default_configuration();
    for (size_t input_index = 0; input_index < SXML_TEST_COUNT(inputs);
         input_index += 1) {
        sxml_decimal decimal = empty_decimal();
        SXML_TEST_EXPECT(state, sxml_decimal_parse(&configuration,
                                                   text_of(inputs[input_index]),
                                                   &decimal) ==
                                    SXML_STATUS_MALFORMED_INPUT);
        SXML_TEST_EXPECT(state, decimal_is_untouched(&decimal));
    }
}

static void test_parse_respects_digit_limit(sxml_test_state *state) {
    sxml_decimal_configuration configuration = default_configuration();
    configuration.maximum_digit_count = 5;
    configuration.division_digit_count = 5;
    sxml_decimal decimal = empty_decimal();
    SXML_TEST_EXPECT(state, sxml_decimal_parse(&configuration, text_of("12345"),
                                               &decimal) == SXML_STATUS_OK);
    sxml_decimal_finalize(&decimal);
    /* Leading zeros and trailing fraction zeros are not digits of the
       value. */
    SXML_TEST_EXPECT(state,
                     sxml_decimal_parse(&configuration,
                                        text_of("000123.4500000000"),
                                        &decimal) == SXML_STATUS_OK);
    sxml_decimal_finalize(&decimal);
    SXML_TEST_EXPECT(state, sxml_decimal_parse(&configuration,
                                               text_of("123456"), &decimal) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    /* 0.00001 is written with six digits. */
    SXML_TEST_EXPECT(state, sxml_decimal_parse(&configuration,
                                               text_of("0.00001"), &decimal) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, decimal_is_untouched(&decimal));
}

static void test_write_canonical_text_checks_capacity(sxml_test_state *state) {
    sxml_decimal decimal = empty_decimal();
    SXML_TEST_REQUIRE(state, parse(text_of("-1.25"), &decimal));
    uint8_t bytes[5] = {0};
    sxml_byte_view text = {.bytes = NULL, .byte_count = 99};
    SXML_TEST_EXPECT(state, sxml_decimal_canonical_text_byte_count(&decimal) == 5);
    SXML_TEST_EXPECT(state,
                     sxml_decimal_write_canonical_text(
                         &decimal, (sxml_memory_view){.bytes = bytes, .byte_count = 4},
                         &text) == SXML_STATUS_CAPACITY_EXCEEDED);
    SXML_TEST_EXPECT(state, text.byte_count == 99 && bytes[0] == 0);
    SXML_TEST_EXPECT(state,
                     sxml_decimal_write_canonical_text(
                         &decimal, (sxml_memory_view){.bytes = bytes, .byte_count = 5},
                         &text) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                text, SXML_BYTE_VIEW_FROM_LITERAL("-1.25")));
    sxml_decimal_finalize(&decimal);
    /* Finalizing twice, and finalizing NULL, do nothing. */
    sxml_decimal_finalize(&decimal);
    sxml_decimal_finalize(NULL);
}

/* ---------------------------------------------------------- Arithmetic */

static void test_arithmetic_of_the_torture_slice(sxml_test_state *state) {
    static const arithmetic_case cases[] = {
        /* op-numeric-multiplydec2args-1 */
        {SXML_TEST_OPERATION_MULTIPLY, "-1.0000000000", "-999999999999999999",
         "999999999999999999"},
        /* K-NumericMultiply-2 */
        {SXML_TEST_OPERATION_MULTIPLY, "6", "2", "12"},
        /* op-numeric-dividepint2args-5: one of the allowed results. */
        {SXML_TEST_OPERATION_DIVIDE, "1", "999999999999999999",
         "0.000000000000000001000000000000000001"},
        /* op-numeric-divideint2args-1 */
        {SXML_TEST_OPERATION_DIVIDE, "-2147483648", "-2147483648", "1"},
        /* op-numeric-integer-divideint2args-1 */
        {SXML_TEST_OPERATION_INTEGER_DIVIDE, "-2147483648", "-2147483648", "1"},
        /* op-numeric-modint2args-1 */
        {SXML_TEST_OPERATION_MODULO, "-2147483648", "2147483647", "-1"},
        /* op-numeric-addint2args-1: beyond xs:int. */
        {SXML_TEST_OPERATION_ADD, "2147483647", "2147483647", "4294967294"},
    };
    sxml_decimal_configuration configuration = default_configuration();
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        SXML_TEST_EXPECT(state,
                         arithmetic_case_holds(&configuration, &cases[case_index]));
    }
}

static void test_arithmetic_follows_the_operators(sxml_test_state *state) {
    static const arithmetic_case cases[] = {
        {SXML_TEST_OPERATION_ADD, "0.1", "0.2", "0.3"},
        {SXML_TEST_OPERATION_ADD, "9223372036854775807", "1",
         "9223372036854775808"},
        {SXML_TEST_OPERATION_ADD, "-1.5", "1.5", "0"},
        {SXML_TEST_OPERATION_SUBTRACT, "1", "1.5", "-0.5"},
        {SXML_TEST_OPERATION_MULTIPLY, "18446744073709551616",
         "18446744073709551616", "340282366920938463463374607431768211456"},
        {SXML_TEST_OPERATION_MULTIPLY, "0.5", "0.5", "0.25"},
        {SXML_TEST_OPERATION_DIVIDE, "10", "4", "2.5"},
        {SXML_TEST_OPERATION_DIVIDE, "1", "3", "0.3333333333333333333333333333333333"},
        {SXML_TEST_OPERATION_DIVIDE, "2", "3", "0.6666666666666666666666666666666667"},
        {SXML_TEST_OPERATION_DIVIDE, "-1", "8", "-0.125"},
        /* Integers that divide exactly stay exact beyond 34 digits. */
        {SXML_TEST_OPERATION_DIVIDE, "1234567890123456789012345678901234567890",
         "10", "123456789012345678901234567890123456789"},
        {SXML_TEST_OPERATION_DIVIDE, "1", "0.001", "1000"},
        {SXML_TEST_OPERATION_INTEGER_DIVIDE, "7", "2", "3"},
        {SXML_TEST_OPERATION_INTEGER_DIVIDE, "-7", "2", "-3"},
        {SXML_TEST_OPERATION_INTEGER_DIVIDE, "7.5", "-2", "-3"},
        {SXML_TEST_OPERATION_MODULO, "5.5", "2", "1.5"},
        {SXML_TEST_OPERATION_MODULO, "-5", "3", "-2"},
        {SXML_TEST_OPERATION_MODULO, "5", "-3", "2"},
        {SXML_TEST_OPERATION_MODULO, "6", "3", "0"},
    };
    sxml_decimal_configuration configuration = default_configuration();
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        SXML_TEST_EXPECT(state,
                         arithmetic_case_holds(&configuration, &cases[case_index]));
    }
}

static void test_division_by_zero_is_invalid(sxml_test_state *state) {
    static const test_operation operations[] = {
        SXML_TEST_OPERATION_DIVIDE,
        SXML_TEST_OPERATION_INTEGER_DIVIDE,
        SXML_TEST_OPERATION_MODULO,
    };
    sxml_decimal one = empty_decimal();
    sxml_decimal zero = empty_decimal();
    SXML_TEST_REQUIRE(state, parse(text_of("1"), &one));
    SXML_TEST_REQUIRE(state, parse(text_of("-0.00"), &zero));
    sxml_decimal_configuration configuration = default_configuration();
    for (size_t operation_index = 0;
         operation_index < SXML_TEST_COUNT(operations); operation_index += 1) {
        sxml_decimal result = empty_decimal();
        SXML_TEST_EXPECT(
            state, run_operation(&configuration,
                                 &(operation_request){
                                     .operation = operations[operation_index],
                                     .operands = {.left_operand = &one,
                                                  .right_operand = &zero},
                                 },
                                 &result) == SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, decimal_is_untouched(&result));
    }
    sxml_decimal_finalize(&zero);
    sxml_decimal_finalize(&one);
}

static void test_results_beyond_digit_limit_are_refused(
    sxml_test_state *state) {
    sxml_decimal_configuration configuration = default_configuration();
    configuration.maximum_digit_count = 20;
    configuration.division_digit_count = 10;
    static const arithmetic_case refused_cases[] = {
        {SXML_TEST_OPERATION_MULTIPLY, "99999999999", "99999999999", ""},
        {SXML_TEST_OPERATION_ADD, "10000000000", "0.0000000001", ""},
        {SXML_TEST_OPERATION_DIVIDE, "10", "0.0000000000000000001", ""},
        {SXML_TEST_OPERATION_INTEGER_DIVIDE, "10000000000", "0.0000000001", ""},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(refused_cases);
         case_index += 1) {
        const arithmetic_case *test_case = &refused_cases[case_index];
        sxml_decimal left = empty_decimal();
        sxml_decimal right = empty_decimal();
        sxml_decimal result = empty_decimal();
        SXML_TEST_REQUIRE(state, sxml_decimal_parse(&configuration,
                                                    text_of(test_case->left_text),
                                                    &left) == SXML_STATUS_OK);
        SXML_TEST_REQUIRE(state, sxml_decimal_parse(&configuration,
                                                    text_of(test_case->right_text),
                                                    &right) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(
            state, run_operation(&configuration,
                                 &(operation_request){
                                     .operation = test_case->operation,
                                     .operands = {.left_operand = &left,
                                                  .right_operand = &right},
                                 },
                                 &result) == SXML_STATUS_LIMIT_EXCEEDED);
        SXML_TEST_EXPECT(state, decimal_is_untouched(&result));
        sxml_decimal_finalize(&right);
        sxml_decimal_finalize(&left);
    }
    /* Within the limit, the quotient has division_digit_count digits. */
    static const arithmetic_case accepted_case = {
        SXML_TEST_OPERATION_DIVIDE, "2", "3", "0.6666666667"};
    SXML_TEST_EXPECT(state, arithmetic_case_holds(&configuration, &accepted_case));
}

static void test_negate_absolute_and_copy(sxml_test_state *state) {
    sxml_decimal_configuration configuration = default_configuration();
    sxml_decimal value = empty_decimal();
    sxml_decimal negation = empty_decimal();
    sxml_decimal absolute_value = empty_decimal();
    sxml_decimal copy = empty_decimal();
    SXML_TEST_REQUIRE(state, parse(text_of("-12.50"), &value));
    SXML_TEST_EXPECT(state, sxml_decimal_negate(&configuration, &value,
                                                &negation) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_decimal_absolute(&configuration, &value,
                                                  &absolute_value) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_decimal_copy(&configuration, &value, &copy) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, has_text((text_expectation){
                                .decimal = &negation,
                                .expected_text = text_of("12.5")}));
    SXML_TEST_EXPECT(state, has_text((text_expectation){
                                .decimal = &absolute_value,
                                .expected_text = text_of("12.5")}));
    SXML_TEST_EXPECT(state, has_text((text_expectation){
                                .decimal = &copy,
                                .expected_text = text_of("-12.5")}));
    /* The copy owns its coefficient. */
    SXML_TEST_EXPECT(state, copy.number.data != value.number.data);
    sxml_decimal_finalize(&copy);
    sxml_decimal_finalize(&absolute_value);
    sxml_decimal_finalize(&negation);
    sxml_decimal_finalize(&value);
}

/* ------------------------------------------------------------ Rounding */

/* One rounding and its expected text. */
typedef struct rounding_case {
    const char *input;
    int64_t precision;
    sxml_rounding_mode rounding_mode;
    const char *expected_text;
} rounding_case;

static void test_round_follows_each_rounding_mode(sxml_test_state *state) {
    static const rounding_case cases[] = {
        {"2.5", 0, SXML_ROUNDING_MODE_HALF_TO_CEILING, "3"},
        {"-2.5", 0, SXML_ROUNDING_MODE_HALF_TO_CEILING, "-2"},
        {"2.5", 0, SXML_ROUNDING_MODE_HALF_TO_FLOOR, "2"},
        {"-2.5", 0, SXML_ROUNDING_MODE_HALF_TO_FLOOR, "-3"},
        {"2.5", 0, SXML_ROUNDING_MODE_HALF_TO_EVEN, "2"},
        {"3.5", 0, SXML_ROUNDING_MODE_HALF_TO_EVEN, "4"},
        {"2.5", 0, SXML_ROUNDING_MODE_HALF_TOWARD_ZERO, "2"},
        {"-2.5", 0, SXML_ROUNDING_MODE_HALF_AWAY_FROM_ZERO, "-3"},
        {"2.6", 0, SXML_ROUNDING_MODE_HALF_TOWARD_ZERO, "3"},
        {"-1.1", 0, SXML_ROUNDING_MODE_FLOOR, "-2"},
        {"-1.1", 0, SXML_ROUNDING_MODE_CEILING, "-1"},
        {"-1.9", 0, SXML_ROUNDING_MODE_TOWARD_ZERO, "-1"},
        {"1.1", 0, SXML_ROUNDING_MODE_AWAY_FROM_ZERO, "2"},
        {"1.23456", 2, SXML_ROUNDING_MODE_HALF_TO_CEILING, "1.23"},
        {"1.235", 2, SXML_ROUNDING_MODE_HALF_TO_EVEN, "1.24"},
        {"1250", -2, SXML_ROUNDING_MODE_HALF_TO_EVEN, "1200"},
        {"1251", -2, SXML_ROUNDING_MODE_HALF_TO_EVEN, "1300"},
        {"1.5", 3, SXML_ROUNDING_MODE_FLOOR, "1.5"},
        {"5", INT64_MIN, SXML_ROUNDING_MODE_HALF_TO_CEILING, "0"},
        {"5", INT64_MAX, SXML_ROUNDING_MODE_HALF_TO_CEILING, "5"},
        {"-0.4", 0, SXML_ROUNDING_MODE_HALF_TO_CEILING, "0"},
    };
    sxml_decimal_configuration configuration = default_configuration();
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        sxml_decimal value = empty_decimal();
        sxml_decimal rounded = empty_decimal();
        SXML_TEST_REQUIRE(state, parse(text_of(cases[case_index].input), &value));
        SXML_TEST_EXPECT(
            state, sxml_decimal_round(&configuration,
                                      &(sxml_decimal_rounding){
                                          .decimal = &value,
                                          .precision = cases[case_index].precision,
                                          .rounding_mode =
                                              cases[case_index].rounding_mode,
                                      },
                                      &rounded) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state,
                         has_text((text_expectation){
                             .decimal = &rounded,
                             .expected_text =
                                 text_of(cases[case_index].expected_text)}));
        sxml_decimal_finalize(&rounded);
        sxml_decimal_finalize(&value);
    }
}

static void test_round_refuses_results_beyond_limit(sxml_test_state *state) {
    sxml_decimal_configuration configuration = default_configuration();
    sxml_decimal value = empty_decimal();
    sxml_decimal rounded = empty_decimal();
    SXML_TEST_REQUIRE(state, parse(text_of("5"), &value));
    /* Up to the next 10^20000: twenty thousand and one digits. */
    SXML_TEST_EXPECT(state,
                     sxml_decimal_round(&configuration,
                                        &(sxml_decimal_rounding){
                                            .decimal = &value,
                                            .precision = -20000,
                                            .rounding_mode =
                                                SXML_ROUNDING_MODE_CEILING,
                                        },
                                        &rounded) == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, decimal_is_untouched(&rounded));
    sxml_decimal_finalize(&value);
}

/* ------------------------------------------- Division to a precision */

/* fn:divide-decimals($value, $divisor, $precision) and its results. */
typedef struct division_case {
    const char *value_text;
    const char *divisor_text;
    int64_t precision;
    const char *quotient_text;
    const char *remainder_text;
} division_case;

/* Divides with configuration; the status, and on success both texts. */
typedef struct division_outcome {
    sxml_status status;
    bool has_expected_texts;
} division_outcome;

static division_outcome divide_case(const sxml_decimal_configuration *configuration,
                                    const division_case *test_case) {
    sxml_decimal value = empty_decimal();
    sxml_decimal divisor = empty_decimal();
    division_outcome outcome = {.status = SXML_STATUS_INVALID_ARGUMENT,
                                .has_expected_texts = false};
    if (sxml_decimal_parse(configuration, text_of(test_case->value_text), &value) !=
            SXML_STATUS_OK ||
        sxml_decimal_parse(configuration, text_of(test_case->divisor_text), &divisor) !=
            SXML_STATUS_OK) {
        sxml_decimal_finalize(&value);
        sxml_decimal_finalize(&divisor);
        return outcome;
    }
    sxml_decimal_division division = {.quotient = empty_decimal(),
                                      .remainder = empty_decimal()};
    outcome.status = sxml_decimal_divide_to_precision(
        configuration,
        &(sxml_decimal_division_request){
            .operands = &(sxml_decimal_operands){.left_operand = &value,
                                                 .right_operand = &divisor},
            .precision = test_case->precision},
        &division);
    if (outcome.status == SXML_STATUS_OK) {
        outcome.has_expected_texts =
            has_text((text_expectation){
                .decimal = &division.quotient,
                .expected_text = text_of(test_case->quotient_text)}) &&
            has_text((text_expectation){
                .decimal = &division.remainder,
                .expected_text = text_of(test_case->remainder_text)});
        sxml_decimal_finalize(&division.quotient);
        sxml_decimal_finalize(&division.remainder);
    } else {
        outcome.has_expected_texts = decimal_is_untouched(&division.quotient) &&
                                     decimal_is_untouched(&division.remainder);
    }
    sxml_decimal_finalize(&value);
    sxml_decimal_finalize(&divisor);
    return outcome;
}

/* The examples of F&O 4.0, section 4.3.1 and QT4 fn-divide-decimals. */
static void test_divide_to_precision_follows_divide_decimals(sxml_test_state *state) {
    static const division_case cases[] = {
        {"120.6", "60.3", 4, "2", "0"},
        {"10", "3", 0, "3", "1"},
        {"10", "-3", 0, "-3", "1"},
        {"-10", "3", 0, "-3", "-1"},
        {"-10", "-3", 0, "3", "-1"},
        {"10", "3", 6, "3.333333", "0.000001"},
        {"100", "30", 0, "3", "10"},
        {"150862", "7", -3, "21000", "3862"},
        {"100", "30", 2, "3.33", "0.1"},
        {"1", "300", 2, "0", "1"},
        {"1000", "25", -1, "40", "0"},
        {"420", "10", -1, "40", "20"},
        {"100", "700", 6, "0.142857", "0.0001"},
        {"0", "7", 3, "0", "0"},
        {"0.5", "0.25", 0, "2", "0"},
        /* Beyond the digit limit, the precision is lowered to fit. */
        {"20", "10", 100000, "2", "0"},
        {"1.1", "100000", -100000000, "0", "1.1"},
        {"5", "3", INT64_MIN, "0", "5"},
    };
    sxml_decimal_configuration configuration = default_configuration();
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        division_outcome outcome = divide_case(&configuration, &cases[case_index]);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, outcome.has_expected_texts);
    }
}

static void test_divide_to_precision_lowers_precision_to_fit(sxml_test_state *state) {
    sxml_decimal_configuration configuration = configuration_of(sxml_default_allocator());
    configuration.maximum_digit_count = 12;
    configuration.division_digit_count = 12;
    static const division_case cases[] = {
        /* Up to twelve digits with "0." and room for the bounds. */
        {"10", "3", 100000, "3.333333333", "0.000000001"},
        {"1", "3", INT64_MAX, "0.3333333333", "0.0000000001"},
        {"123456789", "1", 50, "123456789", "0"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        division_outcome outcome = divide_case(&configuration, &cases[case_index]);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, outcome.has_expected_texts);
    }
    /* An integer quotient of more digits than the limit is refused. */
    division_outcome refused = divide_case(
        &configuration, &(division_case){"999999999999", "0.001", 0, "", ""});
    SXML_TEST_EXPECT(state, refused.status == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, refused.has_expected_texts);
    division_outcome by_zero =
        divide_case(&configuration, &(division_case){"29", "0", 18, "", ""});
    SXML_TEST_EXPECT(state, by_zero.status == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, by_zero.has_expected_texts);
}

/* --------------------------------------------- Comparison and predicates */

/* Two texts and their expected ordering. */
typedef struct comparison_case {
    const char *first_text;
    const char *second_text;
    sxml_ordering ordering;
} comparison_case;

static void test_compare_orders_by_value(sxml_test_state *state) {
    static const comparison_case cases[] = {
        {"1.0", "1", SXML_ORDERING_EQUAL},
        {"-0", "0.000", SXML_ORDERING_EQUAL},
        {"0.1", "0.11", SXML_ORDERING_LESS},
        {"-5", "3", SXML_ORDERING_LESS},
        {"100", "99.999999999999999999999", SXML_ORDERING_GREATER},
        {"-100", "-99.9", SXML_ORDERING_LESS},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        sxml_decimal first = empty_decimal();
        sxml_decimal second = empty_decimal();
        SXML_TEST_REQUIRE(state, parse(text_of(cases[case_index].first_text), &first));
        SXML_TEST_REQUIRE(state,
                          parse(text_of(cases[case_index].second_text), &second));
        /* Comparing needs no scope: run it outside every scope. */
        SXML_TEST_EXPECT(state, sxml_decimal_compare(&first, &second) ==
                                    cases[case_index].ordering);
        SXML_TEST_EXPECT(state, sxml_decimal_compare(&second, &first) ==
                                    (sxml_ordering)(-(int)cases[case_index].ordering));
        sxml_decimal_finalize(&second);
        sxml_decimal_finalize(&first);
    }
}

static void test_predicates_report_sign_zero_and_integrality(
    sxml_test_state *state) {
    sxml_decimal negative_zero = empty_decimal();
    sxml_decimal integral = empty_decimal();
    sxml_decimal fraction = empty_decimal();
    SXML_TEST_REQUIRE(state, parse(text_of("-0.0"), &negative_zero));
    SXML_TEST_REQUIRE(state, parse(text_of("2.000"), &integral));
    SXML_TEST_REQUIRE(state, parse(text_of("-2.5"), &fraction));
    SXML_TEST_EXPECT(state, sxml_decimal_is_zero(&negative_zero));
    SXML_TEST_EXPECT(state, !sxml_decimal_is_negative(&negative_zero));
    SXML_TEST_EXPECT(state, sxml_decimal_is_integer(&negative_zero));
    SXML_TEST_EXPECT(state, sxml_decimal_is_integer(&integral));
    SXML_TEST_EXPECT(state, !sxml_decimal_is_negative(&integral));
    SXML_TEST_EXPECT(state, !sxml_decimal_is_integer(&fraction));
    SXML_TEST_EXPECT(state, sxml_decimal_is_negative(&fraction));
    SXML_TEST_EXPECT(state, !sxml_decimal_is_zero(&fraction));
    sxml_decimal_finalize(&fraction);
    sxml_decimal_finalize(&integral);
    sxml_decimal_finalize(&negative_zero);
}

/* ------------------------------------------------------------- Integers */

static void test_integers_convert_both_ways(sxml_test_state *state) {
    sxml_decimal_configuration configuration = default_configuration();
    static const int64_t integers[] = {0, 1, -1, 42, INT64_MAX, INT64_MIN};
    for (size_t integer_index = 0; integer_index < SXML_TEST_COUNT(integers);
         integer_index += 1) {
        sxml_decimal decimal = empty_decimal();
        int64_t integer = 7;
        SXML_TEST_REQUIRE(state, sxml_decimal_from_integer(
                                     &configuration, integers[integer_index],
                                     &decimal) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_decimal_to_integer(&decimal, &integer) ==
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, integer == integers[integer_index]);
        sxml_decimal_finalize(&decimal);
    }
    sxml_decimal decimal = empty_decimal();
    SXML_TEST_REQUIRE(state, sxml_decimal_from_integer(&configuration, INT64_MIN,
                                                       &decimal) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, has_text((text_expectation){
                                .decimal = &decimal,
                                .expected_text = text_of("-9223372036854775808")}));
    sxml_decimal_finalize(&decimal);
    SXML_TEST_REQUIRE(state, sxml_decimal_from_unsigned_integer(
                                 &configuration, UINT64_MAX, &decimal) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, has_text((text_expectation){
                                .decimal = &decimal,
                                .expected_text = text_of("18446744073709551615")}));
    sxml_decimal_finalize(&decimal);
}

/* A text and the outcome of converting it to int64_t. */
typedef struct integer_case {
    const char *text;
    sxml_status status;
    int64_t integer;
} integer_case;

static void test_to_integer_needs_integral_value_in_range(
    sxml_test_state *state) {
    static const integer_case cases[] = {
        {"120.000", SXML_STATUS_OK, 120},
        {"-0.0", SXML_STATUS_OK, 0},
        {"9223372036854775807", SXML_STATUS_OK, INT64_MAX},
        {"-9223372036854775808", SXML_STATUS_OK, INT64_MIN},
        {"9223372036854775808", SXML_STATUS_ARITHMETIC_OVERFLOW, 0},
        {"-9223372036854775809", SXML_STATUS_ARITHMETIC_OVERFLOW, 0},
        {"100000000000000000000000000000", SXML_STATUS_ARITHMETIC_OVERFLOW, 0},
        {"1.5", SXML_STATUS_INVALID_ARGUMENT, 0},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        sxml_decimal decimal = empty_decimal();
        int64_t integer = 7;
        SXML_TEST_REQUIRE(state, parse(text_of(cases[case_index].text), &decimal));
        SXML_TEST_EXPECT(state, sxml_decimal_to_integer(&decimal, &integer) ==
                                    cases[case_index].status);
        SXML_TEST_EXPECT(state, integer == (cases[case_index].status ==
                                                    SXML_STATUS_OK
                                                ? cases[case_index].integer
                                                : 7));
        sxml_decimal_finalize(&decimal);
    }
    /* A positive exponent: 10 times 10 from a product. */
    sxml_decimal_configuration configuration = default_configuration();
    sxml_decimal quotient = empty_decimal();
    sxml_decimal one = empty_decimal();
    sxml_decimal hundredth = empty_decimal();
    SXML_TEST_REQUIRE(state, parse(text_of("1"), &one));
    SXML_TEST_REQUIRE(state, parse(text_of("0.01"), &hundredth));
    SXML_TEST_REQUIRE(state, sxml_decimal_divide(
                                 &configuration,
                                 &(sxml_decimal_operands){
                                     .left_operand = &one,
                                     .right_operand = &hundredth},
                                 &quotient) == SXML_STATUS_OK);
    int64_t integer = 0;
    SXML_TEST_EXPECT(state, sxml_decimal_to_integer(&quotient, &integer) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, integer == 100);
    sxml_decimal_finalize(&quotient);
    sxml_decimal_finalize(&hundredth);
    sxml_decimal_finalize(&one);
}

/* ------------------------------------------------ Binary floating point */

/* Parses text and converts it to double. */
static double double_of(sxml_byte_view text) {
    sxml_decimal decimal = empty_decimal();
    if (!parse(text, &decimal)) {
        return NAN;
    }
    double double_number = sxml_decimal_to_double(&decimal);
    sxml_decimal_finalize(&decimal);
    return double_number;
}

static void test_to_double_rounds_to_nearest(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, double_of(text_of("0.1")) == 0.1);
    SXML_TEST_EXPECT(state, double_of(text_of("-2.5")) == -2.5);
    SXML_TEST_EXPECT(state, double_of(text_of("-0.0")) == 0.0);
    SXML_TEST_EXPECT(state, double_of(text_of("123456789012345678901234567890")) ==
                                123456789012345678901234567890.0);
    /* 1 + 2^-53 lies halfway between 1 and the next double: a tie to even. */
    SXML_TEST_EXPECT(
        state, double_of(text_of(
                   "1.00000000000000011102230246251565404236316680908203125")) ==
                   1.0);
    SXML_TEST_EXPECT(
        state,
        double_of(text_of(
            "1.000000000000000111022302462515654042363166809082031250001")) ==
            1.0 + DBL_EPSILON);
    sxml_decimal_configuration configuration = default_configuration();
    uint8_t digits[1201];
    memset(digits, '0', sizeof(digits));
    digits[0] = '1';
    /* 10^1200 is beyond the double range: infinity. */
    sxml_decimal huge = empty_decimal();
    SXML_TEST_REQUIRE(state, sxml_decimal_parse(&configuration,
                                                (sxml_byte_view){
                                                    .bytes = digits,
                                                    .byte_count = sizeof(digits)},
                                                &huge) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, isinf(sxml_decimal_to_double(&huge)));
    sxml_decimal_finalize(&huge);
    /* More than 780 digits: the sticky digit keeps the rounding right. */
    digits[1] = '.';
    digits[800] = '1';
    SXML_TEST_REQUIRE(state, sxml_decimal_parse(&configuration,
                                                (sxml_byte_view){
                                                    .bytes = digits,
                                                    .byte_count = 801},
                                                &huge) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_decimal_to_double(&huge) == 1.0);
    SXML_TEST_EXPECT(state, sxml_decimal_to_float(&huge) == 1.0F);
    sxml_decimal_finalize(&huge);
    sxml_decimal tenth = empty_decimal();
    SXML_TEST_REQUIRE(state, parse(text_of("0.1"), &tenth));
    SXML_TEST_EXPECT(state, sxml_decimal_to_float(&tenth) == 0.1F);
    sxml_decimal_finalize(&tenth);
}

/* A double and the canonical text of a conversion of it. */
typedef struct double_case {
    double double_number;
    const char *expected_text;
} double_case;

static void test_from_double_shortest_gives_fewest_digits(
    sxml_test_state *state) {
    static const double_case cases[] = {
        {0.1, "0.1"},
        {-1.5, "-1.5"},
        {-0.0, "0"},
        {1e21, "1000000000000000000000"},
        {1.0 / 3.0, "0.3333333333333333"},
        {123456789.0, "123456789"},
    };
    sxml_decimal_configuration configuration = default_configuration();
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        sxml_decimal decimal = empty_decimal();
        SXML_TEST_REQUIRE(state, sxml_decimal_from_double_shortest(
                                     &configuration,
                                     cases[case_index].double_number,
                                     &decimal) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, has_text((text_expectation){
                                    .decimal = &decimal,
                                    .expected_text = text_of(
                                        cases[case_index].expected_text)}));
        sxml_decimal_finalize(&decimal);
    }
    /* DBL_MAX shortens to 17976931348623157 and 292 zeros. */
    sxml_decimal decimal = empty_decimal();
    SXML_TEST_REQUIRE(state, sxml_decimal_from_double_shortest(
                                 &configuration, DBL_MAX, &decimal) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_decimal_canonical_text_byte_count(&decimal) == 309);
    SXML_TEST_EXPECT(state, sxml_decimal_to_double(&decimal) == DBL_MAX);
    sxml_decimal_finalize(&decimal);
    /* The smallest subnormal, 4.9406564584124654e-324, shortens to 5e-324. */
    SXML_TEST_REQUIRE(state, sxml_decimal_from_double_shortest(
                                 &configuration, 4.9406564584124654e-324,
                                 &decimal) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_decimal_canonical_text_byte_count(&decimal) ==
                                2 + 323 + 1);
    SXML_TEST_EXPECT(state, sxml_decimal_to_double(&decimal) ==
                                4.9406564584124654e-324);
    sxml_decimal_finalize(&decimal);
    SXML_TEST_REQUIRE(state, sxml_decimal_from_float_shortest(
                                 &configuration, 0.1F, &decimal) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, has_text((text_expectation){
                                .decimal = &decimal,
                                .expected_text = text_of("0.1")}));
    sxml_decimal_finalize(&decimal);
}

static void test_from_double_exactly_keeps_every_digit(sxml_test_state *state) {
    static const double_case cases[] = {
        {0.1, "0.1000000000000000055511151231257827021181583404541015625"},
        {1180591620717411303424.0, "1180591620717411303424"},
        {-0.75, "-0.75"},
        {0.0, "0"},
    };
    sxml_decimal_configuration configuration = default_configuration();
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        sxml_decimal decimal = empty_decimal();
        SXML_TEST_REQUIRE(state, sxml_decimal_from_double_exactly(
                                     &configuration,
                                     cases[case_index].double_number,
                                     &decimal) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, has_text((text_expectation){
                                    .decimal = &decimal,
                                    .expected_text = text_of(
                                        cases[case_index].expected_text)}));
        sxml_decimal_finalize(&decimal);
    }
    /* The smallest subnormal has 751 significant digits, 1074 after the
       point. */
    sxml_decimal decimal = empty_decimal();
    SXML_TEST_REQUIRE(state, sxml_decimal_from_double_exactly(
                                 &configuration, 4.9406564584124654e-324,
                                 &decimal) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_decimal_canonical_text_byte_count(&decimal) ==
                                2 + 1074);
    sxml_decimal_finalize(&decimal);
}

static void test_from_double_refuses_nan_and_infinities(
    sxml_test_state *state) {
    static const double special_numbers[] = {NAN, INFINITY, -INFINITY};
    sxml_decimal_configuration configuration = default_configuration();
    for (size_t number_index = 0;
         number_index < SXML_TEST_COUNT(special_numbers); number_index += 1) {
        sxml_decimal decimal = empty_decimal();
        SXML_TEST_EXPECT(state, sxml_decimal_from_double_exactly(
                                    &configuration,
                                    special_numbers[number_index],
                                    &decimal) == SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, sxml_decimal_from_double_shortest(
                                    &configuration,
                                    special_numbers[number_index],
                                    &decimal) == SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, sxml_decimal_from_float_shortest(
                                    &configuration,
                                    (float)special_numbers[number_index],
                                    &decimal) == SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, decimal_is_untouched(&decimal));
    }
    /* 1e308 needs 309 digits: beyond a smaller limit (FOCA0001). */
    configuration.maximum_digit_count = 300;
    sxml_decimal decimal = empty_decimal();
    SXML_TEST_EXPECT(state, sxml_decimal_from_double_shortest(
                                &configuration, 1e308, &decimal) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, decimal_is_untouched(&decimal));
}

/* -------------------------------------------- Configuration and scope */

static void test_invalid_configuration_is_refused(sxml_test_state *state) {
    sxml_decimal_configuration configurations[4];
    for (size_t configuration_index = 0; configuration_index < 4;
         configuration_index += 1) {
        configurations[configuration_index] = default_configuration();
    }
    configurations[0].allocator = (sxml_allocator){.state = NULL,
                                                   .operations = NULL};
    configurations[1].maximum_digit_count = 0;
    configurations[2].division_digit_count = 0;
    configurations[3].division_digit_count =
        configurations[3].maximum_digit_count + 1;
    for (size_t configuration_index = 0; configuration_index < 4;
         configuration_index += 1) {
        sxml_decimal decimal = empty_decimal();
        SXML_TEST_EXPECT(state, sxml_decimal_parse(
                                    &configurations[configuration_index],
                                    text_of("1"), &decimal) ==
                                    SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, sxml_decimal_from_integer(
                                    &configurations[configuration_index], 1,
                                    &decimal) == SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, decimal_is_untouched(&decimal));
    }
}

static void test_scope_is_restored_after_each_operation(
    sxml_test_state *state) {
    /* Outside every operation the scope is closed: libmpdec cannot
       allocate. */
    SXML_TEST_EXPECT(state, mpd_qnew() == NULL);
    sxml_allocator outer_allocator = sxml_default_allocator();
    SXML_TEST_EXPECT(state, mpd_sxml_exchange_allocator_scope(
                                &outer_allocator) == NULL);
    sxml_decimal decimal = empty_decimal();
    SXML_TEST_EXPECT(state, parse(text_of("1.5"), &decimal));
    /* The operation nested inside the outer scope and restored it. */
    SXML_TEST_EXPECT(state, mpd_sxml_exchange_allocator_scope(NULL) ==
                                &outer_allocator);
    sxml_decimal_finalize(&decimal);
}

static void test_memory_comes_from_the_configured_allocator(
    sxml_test_state *state) {
    counting_fixture first_fixture = {.storage = {0}, .counting_allocator = NULL};
    counting_fixture second_fixture = {.storage = {0}, .counting_allocator = NULL};
    SXML_TEST_REQUIRE(state, counting_fixture_initialize(&first_fixture, 0) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, counting_fixture_initialize(&second_fixture, 0) ==
                                 SXML_STATUS_OK);
    sxml_decimal_configuration first_configuration = configuration_of(
        sxml_counting_allocator_as_allocator(first_fixture.counting_allocator));
    sxml_decimal_configuration second_configuration = configuration_of(
        sxml_counting_allocator_as_allocator(second_fixture.counting_allocator));
    sxml_decimal first = empty_decimal();
    sxml_decimal second = empty_decimal();
    SXML_TEST_REQUIRE(state, sxml_decimal_parse(&first_configuration,
                                                text_of("12345678901234567890123"),
                                                &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, counting_fixture_counts(&first_fixture)
                                    .live_allocation_count == 1);
    /* A copy made with the second configuration belongs to it; growing it
       takes memory from it too. */
    SXML_TEST_REQUIRE(state, sxml_decimal_multiply(
                                 &second_configuration,
                                 &(sxml_decimal_operands){
                                     .left_operand = &first,
                                     .right_operand = &first},
                                 &second) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, counting_fixture_counts(&first_fixture)
                                    .live_allocation_count == 1);
    SXML_TEST_EXPECT(state, counting_fixture_counts(&second_fixture)
                                    .live_allocation_count == 1);
    /* Finalizing needs no scope and returns memory to its own allocator. */
    sxml_decimal_finalize(&first);
    sxml_decimal_finalize(&second);
    sxml_allocation_counts first_counts = counting_fixture_counts(&first_fixture);
    sxml_allocation_counts second_counts =
        counting_fixture_counts(&second_fixture);
    SXML_TEST_EXPECT(state, first_counts.live_allocation_count == 0);
    SXML_TEST_EXPECT(state, second_counts.live_allocation_count == 0);
    SXML_TEST_EXPECT(state, first_counts.attempted_allocation_count > 0);
    SXML_TEST_EXPECT(state, second_counts.attempted_allocation_count > 0);
    sxml_counting_allocator_finalize(second_fixture.counting_allocator);
    sxml_counting_allocator_finalize(first_fixture.counting_allocator);
}

/* ------------------------------------------- Products at the digit limit */

/* 30000 nines squared have 60000 digits: on WebAssembly (9 digits a word)
   a power-of-two transform of 8192 words, which takes libmpdec's six-step
   path; 5000 and 10000 nines take its four-step path there. */
#define TEST_LARGE_OPERAND_DIGIT_COUNT 30000U
#define TEST_LARGE_PRODUCT_DIGIT_COUNT (2U * TEST_LARGE_OPERAND_DIGIT_COUNT)

/* Text storage of the large cases. Static: the WebAssembly stack has only
   64 KiB, which is what these cases check libmpdec respects (D74). */
typedef struct large_texts {
    uint8_t operand[TEST_LARGE_OPERAND_DIGIT_COUNT];
    uint8_t expected[TEST_LARGE_PRODUCT_DIGIT_COUNT];
    uint8_t written[TEST_LARGE_PRODUCT_DIGIT_COUNT + 8];
} large_texts;

static large_texts *large_texts_storage(void) {
    static large_texts texts;
    return &texts;
}

/* digit_count nines: 10^digit_count - 1. */
static sxml_byte_view nines_text(large_texts *texts, size_t digit_count) {
    memset(texts->operand, '9', digit_count);
    return (sxml_byte_view){.bytes = texts->operand, .byte_count = digit_count};
}

/* 1 followed by zero_count zeros: 10^zero_count. */
static sxml_byte_view power_of_ten_text(large_texts *texts, size_t zero_count) {
    texts->operand[0] = '1';
    memset(texts->operand + 1, '0', zero_count);
    return (sxml_byte_view){.bytes = texts->operand,
                            .byte_count = zero_count + 1};
}

/* The square of digit_count nines: (10^k - 1)^2 = 10^2k - 2*10^k + 1, that
   is k-1 nines, an eight, k-1 zeros and a one. */
static sxml_byte_view square_of_nines_text(large_texts *texts,
                                           size_t digit_count) {
    memset(texts->expected, '9', digit_count - 1);
    texts->expected[digit_count - 1] = '8';
    memset(texts->expected + digit_count, '0', digit_count - 1);
    texts->expected[2 * digit_count - 1] = '1';
    return (sxml_byte_view){.bytes = texts->expected,
                            .byte_count = 2 * digit_count};
}

/* True when the canonical text of decimal is expected_text; writes into
   the large storage, not onto the stack. */
static bool has_large_text(large_texts *texts, text_expectation expectation) {
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    return sxml_decimal_write_canonical_text(
               expectation.decimal,
               (sxml_memory_view){.bytes = texts->written,
                                  .byte_count = sizeof(texts->written)},
               &text) == SXML_STATUS_OK &&
           sxml_byte_view_equals(text, expectation.expected_text);
}

/* What square_and_divide_back found: the first failure, and whether every
   result it computed had the expected text. */
typedef struct square_outcome {
    sxml_status status;
    bool results_match;
} square_outcome;

/*
 * Squares digit_count nines with configuration, divides the square by the
 * nines again (sxml_decimal_divide and sxml_decimal_integer_divide) and
 * checks the three results.
 */
static square_outcome square_and_divide_back(
    const sxml_decimal_configuration *configuration, size_t digit_count) {
    large_texts *texts = large_texts_storage();
    sxml_decimal nines = empty_decimal();
    sxml_decimal square = empty_decimal();
    sxml_decimal quotient = empty_decimal();
    sxml_decimal integer_quotient = empty_decimal();
    bool results_match = true;
    sxml_status status = sxml_decimal_parse(
        configuration, nines_text(texts, digit_count), &nines);
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_multiply(
            configuration,
            &(sxml_decimal_operands){.left_operand = &nines,
                                     .right_operand = &nines},
            &square);
    }
    if (status == SXML_STATUS_OK &&
        !has_large_text(texts, (text_expectation){
                                   .decimal = &square,
                                   .expected_text = square_of_nines_text(
                                       texts, digit_count)})) {
        results_match = false;
    }
    sxml_decimal_operands back = {.left_operand = &square,
                                  .right_operand = &nines};
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_divide(configuration, &back, &quotient);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_integer_divide(configuration, &back,
                                             &integer_quotient);
    }
    if (status == SXML_STATUS_OK &&
        (!has_large_text(texts,
                         (text_expectation){
                             .decimal = &quotient,
                             .expected_text = nines_text(texts, digit_count)}) ||
         !has_large_text(texts, (text_expectation){
                                    .decimal = &integer_quotient,
                                    .expected_text =
                                        nines_text(texts, digit_count)}))) {
        results_match = false;
    }
    sxml_decimal_finalize(&integer_quotient);
    sxml_decimal_finalize(&quotient);
    sxml_decimal_finalize(&square);
    sxml_decimal_finalize(&nines);
    return (square_outcome){.status = status, .results_match = results_match};
}

/* True when squaring and dividing back succeeded with the right results. */
static bool square_and_divide_back_hold(
    const sxml_decimal_configuration *configuration, size_t digit_count) {
    square_outcome outcome =
        square_and_divide_back(configuration, digit_count);
    return outcome.status == SXML_STATUS_OK && outcome.results_match;
}

/* Multiplies two copies of operand_text; true when that exceeds the digit
   limit of configuration and leaves the result untouched. */
static bool square_exceeds_limit(
    const sxml_decimal_configuration *configuration,
    sxml_byte_view operand_text) {
    sxml_decimal operand = empty_decimal();
    sxml_decimal result = empty_decimal();
    bool exceeds = false;
    if (sxml_decimal_parse(configuration, operand_text, &operand) ==
        SXML_STATUS_OK) {
        exceeds = sxml_decimal_multiply(
                      configuration,
                      &(sxml_decimal_operands){.left_operand = &operand,
                                               .right_operand = &operand},
                      &result) == SXML_STATUS_LIMIT_EXCEEDED &&
                  decimal_is_untouched(&result);
    }
    sxml_decimal_finalize(&operand);
    return exceeds;
}

/*
 * libmpdec's long products (number-theoretic transforms) kept up to 128 KiB
 * of buffers on the stack and trapped on WebAssembly's 64 KiB stack
 * (D72, fixed by D74). These cases run there too.
 */
static void test_products_at_the_default_digit_limit(sxml_test_state *state) {
    sxml_decimal_configuration configuration = default_configuration();
    large_texts *texts = large_texts_storage();
    /* A 10000-digit product and its quotients: exact, within the limit. */
    SXML_TEST_EXPECT(state, square_and_divide_back_hold(&configuration, 5000));
    /* Operands of the largest size: the product is computed, then refused. */
    SXML_TEST_EXPECT(
        state,
        square_exceeds_limit(&configuration,
                             nines_text(texts, SXML_DECIMAL_DEFAULT_MAXIMUM_DIGIT_COUNT)));
    SXML_TEST_EXPECT(state, square_exceeds_limit(
                                &configuration, power_of_ten_text(texts, 6000)));
}

static void test_products_beyond_the_default_digit_limit(
    sxml_test_state *state) {
    sxml_decimal_configuration configuration = default_configuration();
    configuration.maximum_digit_count = TEST_LARGE_PRODUCT_DIGIT_COUNT;
    SXML_TEST_EXPECT(state, square_and_divide_back_hold(
                                &configuration, TEST_LARGE_OPERAND_DIGIT_COUNT));
}

/* The transform buffers come from the configured allocator: each failure
   of a long product is reported and leaks nothing. */
static void test_long_product_allocation_failures_are_reported_and_clean(
    sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
        SXML_TEST_REQUIRE(state, counting_fixture_initialize(
                                     &fixture, failing_allocation_number) ==
                                     SXML_STATUS_OK);
        sxml_decimal_configuration configuration = configuration_of(
            sxml_counting_allocator_as_allocator(fixture.counting_allocator));
        square_outcome outcome = square_and_divide_back(&configuration, 5000);
        sxml_status status = outcome.status;
        SXML_TEST_EXPECT(state, outcome.results_match);
        sxml_allocation_counts counts = counting_fixture_counts(&fixture);
        sxml_counting_allocator_finalize(fixture.counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, counts.failed_allocation_count == 1);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 10000);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 10);
}

/* ------------------------------------------------ Allocation failures */

/*
 * Runs every allocating function once with configuration and releases all
 * it made. Returns the first failure.
 */
static sxml_status use_every_allocating_function(
    const sxml_decimal_configuration *configuration) {
    sxml_decimal values[12];
    for (size_t value_index = 0; value_index < SXML_TEST_COUNT(values);
         value_index += 1) {
        values[value_index] = empty_decimal();
    }
    sxml_decimal_operands operands = {.left_operand = &values[0],
                                      .right_operand = &values[1]};
    sxml_status status = sxml_decimal_parse(
        configuration, text_of("123456789012345678901234567890.5"), &values[0]);
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_from_integer(configuration, -7, &values[1]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_multiply(configuration, &operands, &values[2]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_divide(configuration, &operands, &values[3]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_modulo(configuration, &operands, &values[4]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_add(configuration, &operands, &values[5]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_from_double_shortest(configuration, 0.1, &values[6]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_from_double_exactly(configuration, 1e-300,
                                                  &values[7]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_round(
            configuration,
            &(sxml_decimal_rounding){
                .decimal = &values[3],
                .precision = 3,
                .rounding_mode = SXML_ROUNDING_MODE_HALF_TO_EVEN,
            },
            &values[8]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_copy(configuration, &values[2], &values[9]);
    }
    if (status == SXML_STATUS_OK) {
        sxml_decimal_division division;
        status = sxml_decimal_divide_to_precision(
            configuration,
            &(sxml_decimal_division_request){.operands = &operands, .precision = 40},
            &division);
        if (status == SXML_STATUS_OK) {
            values[10] = division.quotient;
            values[11] = division.remainder;
        }
    }
    for (size_t value_index = 0; value_index < SXML_TEST_COUNT(values);
         value_index += 1) {
        sxml_decimal_finalize(&values[value_index]);
    }
    return status;
}

static void test_every_allocation_failure_is_reported_and_clean(
    sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
        SXML_TEST_REQUIRE(state, counting_fixture_initialize(
                                     &fixture, failing_allocation_number) ==
                                     SXML_STATUS_OK);
        sxml_decimal_configuration configuration = configuration_of(
            sxml_counting_allocator_as_allocator(fixture.counting_allocator));
        sxml_status status = use_every_allocating_function(&configuration);
        sxml_allocation_counts counts = counting_fixture_counts(&fixture);
        sxml_counting_allocator_finalize(fixture.counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, counts.failed_allocation_count == 1);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 1000);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 10);
}

/* ------------------------------------------------------------- Threads */

/* What one thread computes: its own allocator and its own result. */
typedef struct worker {
    counting_fixture fixture;
    uint32_t iteration_count;
    bool succeeded;
} worker;

/* Sums 1/7 + 2/7 + ... with the worker's allocator and checks the sum and
   that every allocation was released. */
static void run_worker(worker *work) {
    sxml_decimal_configuration configuration = configuration_of(
        sxml_counting_allocator_as_allocator(work->fixture.counting_allocator));
    bool succeeded = true;
    for (uint32_t iteration = 0; iteration < work->iteration_count;
         iteration += 1) {
        sxml_decimal seven = empty_decimal();
        sxml_decimal numerator = empty_decimal();
        sxml_decimal quotient = empty_decimal();
        sxml_decimal product = empty_decimal();
        int64_t integer = 0;
        succeeded = succeeded &&
                    sxml_decimal_from_integer(&configuration, 7, &seven) ==
                        SXML_STATUS_OK &&
                    sxml_decimal_from_integer(&configuration,
                                              (int64_t)iteration * 7,
                                              &numerator) == SXML_STATUS_OK &&
                    sxml_decimal_divide(&configuration,
                                        &(sxml_decimal_operands){
                                            .left_operand = &numerator,
                                            .right_operand = &seven},
                                        &quotient) == SXML_STATUS_OK &&
                    sxml_decimal_multiply(&configuration,
                                          &(sxml_decimal_operands){
                                              .left_operand = &quotient,
                                              .right_operand = &quotient},
                                          &product) == SXML_STATUS_OK &&
                    sxml_decimal_to_integer(&product, &integer) ==
                        SXML_STATUS_OK &&
                    integer == (int64_t)iteration * (int64_t)iteration;
        sxml_decimal_finalize(&product);
        sxml_decimal_finalize(&quotient);
        sxml_decimal_finalize(&numerator);
        sxml_decimal_finalize(&seven);
    }
    sxml_allocation_counts counts = counting_fixture_counts(&work->fixture);
    work->succeeded = succeeded && counts.live_allocation_count == 0 &&
                      counts.attempted_allocation_count >=
                          4U * (uint64_t)work->iteration_count;
}

#if defined(_WIN32)
static DWORD WINAPI run_worker_thread(LPVOID argument) {
    run_worker(argument);
    return 0;
}
#elif !defined(__wasi__)
static void *run_worker_thread(void *argument) {
    run_worker(argument);
    return NULL;
}
#endif

static void test_two_heaps_work_on_two_threads(sxml_test_state *state) {
    worker workers[2];
    for (size_t worker_index = 0; worker_index < 2; worker_index += 1) {
        workers[worker_index] = (worker){
            .fixture = {.storage = {0}, .counting_allocator = NULL},
            .iteration_count = 2000,
            .succeeded = false,
        };
        SXML_TEST_REQUIRE(state, counting_fixture_initialize(
                                     &workers[worker_index].fixture, 0) ==
                                     SXML_STATUS_OK);
    }
#if defined(_WIN32)
    HANDLE threads[2];
    for (size_t worker_index = 0; worker_index < 2; worker_index += 1) {
        threads[worker_index] = CreateThread(NULL, 0, run_worker_thread,
                                             &workers[worker_index], 0, NULL);
        SXML_TEST_REQUIRE(state, threads[worker_index] != NULL);
    }
    for (size_t worker_index = 0; worker_index < 2; worker_index += 1) {
        (void)WaitForSingleObject(threads[worker_index], INFINITE);
        (void)CloseHandle(threads[worker_index]);
    }
#elif !defined(__wasi__)
    pthread_t threads[2];
    for (size_t worker_index = 0; worker_index < 2; worker_index += 1) {
        SXML_TEST_REQUIRE(state, pthread_create(&threads[worker_index], NULL,
                                                run_worker_thread,
                                                &workers[worker_index]) == 0);
    }
    for (size_t worker_index = 0; worker_index < 2; worker_index += 1) {
        SXML_TEST_EXPECT(state, pthread_join(threads[worker_index], NULL) == 0);
    }
#else
    /* WebAssembly without threads (wasm32-wasip1): one after the other. */
    for (size_t worker_index = 0; worker_index < 2; worker_index += 1) {
        run_worker(&workers[worker_index]);
    }
#endif
    for (size_t worker_index = 0; worker_index < 2; worker_index += 1) {
        SXML_TEST_EXPECT(state, workers[worker_index].succeeded);
        sxml_counting_allocator_finalize(
            workers[worker_index].fixture.counting_allocator);
    }
}

static const sxml_test_case decimal_test_cases[] = {
    {.name = "parse_gives_canonical_text",
     .function = test_parse_gives_canonical_text},
    {.name = "parse_rejects_other_lexical_forms",
     .function = test_parse_rejects_other_lexical_forms},
    {.name = "parse_respects_digit_limit",
     .function = test_parse_respects_digit_limit},
    {.name = "write_canonical_text_checks_capacity",
     .function = test_write_canonical_text_checks_capacity},
    {.name = "arithmetic_of_the_torture_slice",
     .function = test_arithmetic_of_the_torture_slice},
    {.name = "arithmetic_follows_the_operators",
     .function = test_arithmetic_follows_the_operators},
    {.name = "division_by_zero_is_invalid",
     .function = test_division_by_zero_is_invalid},
    {.name = "results_beyond_digit_limit_are_refused",
     .function = test_results_beyond_digit_limit_are_refused},
    {.name = "negate_absolute_and_copy",
     .function = test_negate_absolute_and_copy},
    {.name = "round_follows_each_rounding_mode",
     .function = test_round_follows_each_rounding_mode},
    {.name = "round_refuses_results_beyond_limit",
     .function = test_round_refuses_results_beyond_limit},
    {.name = "divide_to_precision_follows_divide_decimals",
     .function = test_divide_to_precision_follows_divide_decimals},
    {.name = "divide_to_precision_lowers_precision_to_fit",
     .function = test_divide_to_precision_lowers_precision_to_fit},
    {.name = "compare_orders_by_value",
     .function = test_compare_orders_by_value},
    {.name = "predicates_report_sign_zero_and_integrality",
     .function = test_predicates_report_sign_zero_and_integrality},
    {.name = "integers_convert_both_ways",
     .function = test_integers_convert_both_ways},
    {.name = "to_integer_needs_integral_value_in_range",
     .function = test_to_integer_needs_integral_value_in_range},
    {.name = "to_double_rounds_to_nearest",
     .function = test_to_double_rounds_to_nearest},
    {.name = "from_double_shortest_gives_fewest_digits",
     .function = test_from_double_shortest_gives_fewest_digits},
    {.name = "from_double_exactly_keeps_every_digit",
     .function = test_from_double_exactly_keeps_every_digit},
    {.name = "from_double_refuses_nan_and_infinities",
     .function = test_from_double_refuses_nan_and_infinities},
    {.name = "invalid_configuration_is_refused",
     .function = test_invalid_configuration_is_refused},
    {.name = "scope_is_restored_after_each_operation",
     .function = test_scope_is_restored_after_each_operation},
    {.name = "memory_comes_from_the_configured_allocator",
     .function = test_memory_comes_from_the_configured_allocator},
    {.name = "products_at_the_default_digit_limit",
     .function = test_products_at_the_default_digit_limit},
    {.name = "products_beyond_the_default_digit_limit",
     .function = test_products_beyond_the_default_digit_limit},
    {.name = "every_allocation_failure_is_reported_and_clean",
     .function = test_every_allocation_failure_is_reported_and_clean},
    {.name = "long_product_allocation_failures_are_reported_and_clean",
     .function = test_long_product_allocation_failures_are_reported_and_clean},
    {.name = "two_heaps_work_on_two_threads",
     .function = test_two_heaps_work_on_two_threads},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "decimal",
            .cases = decimal_test_cases,
            .case_count = SXML_TEST_COUNT(decimal_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
