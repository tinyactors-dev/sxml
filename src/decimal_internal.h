/*
 * Decimal arithmetic: xs:decimal values, and xs:integer values beyond
 * int64_t, as libmpdec numbers (D8, D36, D42, D49).
 *
 * Internal to the library; the item heap (src/xdm_atomic.c) keeps an
 * sxml_decimal in a cell, and casting and the arithmetic operators compute
 * with the functions below. Every function that may allocate takes a
 * configuration whose allocator receives the memory of the result, through
 * the decimal scope of D42: libmpdec's allocation hooks are patched to use
 * the allocator the current operation names, so two item heaps with their
 * own allocators work on two threads at once (D35).
 *
 * Specification: F&O 4.0, "Arithmetic operators on numeric values"
 * (precision of xs:decimal results) and "Casting".
 */
#pragma once

#include "mpdecimal.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/string.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Default bound on the digits of a decimal: 10000 digits are far beyond any
 * xs:decimal a query writes (XSD 1.1 Part 2, section 5.4, requires 16) and
 * keep the work of one multiplication or division small.
 */
#define SXML_DECIMAL_DEFAULT_MAXIMUM_DIGIT_COUNT 10000U

/*
 * Default number of significant digits of a quotient that does not
 * terminate: 34, the precision of IEEE 754 decimal128.
 */
#define SXML_DECIMAL_DEFAULT_DIVISION_DIGIT_COUNT 34U

/*
 * How decimal operations allocate and how far results may grow. A plain
 * value; the item heap keeps one.
 */
typedef struct sxml_decimal_configuration {
    /* Receives the coefficient memory of every result, and the scratch
       memory of the operation. Not the null allocator. Each result records
       it, so finalizing a decimal needs no configuration; it must outlive
       every decimal made with it. */
    sxml_allocator allocator;
    /* The most digits a result may have, counting every digit of its
       canonical text (integer and fraction digits); 1 or more. A larger
       result is SXML_STATUS_LIMIT_EXCEEDED, which the operators report as
       FOAR0002 and casting as FOCA0001 or FOCA0003. Bounds the work on
       untrusted input. */
    uint32_t maximum_digit_count;
    /* The significant digits of a quotient (sxml_decimal_divide), 1 to
       maximum_digit_count. F&O 4.0, op:numeric-divide,
       leaves the precision of xs:decimal division implementation-defined.
       A quotient is computed to the larger of this count and the digit
       counts of the operands together, so that dividing integers that
       divide exactly stays exact in practice. */
    uint32_t division_digit_count;
} sxml_decimal_configuration;

/*
 * A decimal: a sign, a coefficient and a power of ten, never NaN or
 * infinite. A plain value with private fields that owns its coefficient
 * memory: made by the functions below, which write a new decimal to their
 * output, and released with sxml_decimal_finalize. It may be moved byte for
 * byte (the coefficient is not inside it) but not copied: use
 * sxml_decimal_copy. The value -0 can arise and is equal to 0 everywhere.
 *
 * Thread safety: a decimal is used by one thread at a time; decimals with
 * different allocators may be used on different threads at once.
 */
typedef struct sxml_decimal {
    /* A libmpdec number whose struct is static (MPD_STATIC: it lives
       here) and whose coefficient is dynamic, from the allocator. */
    mpd_t number;
} sxml_decimal;

/* The two operands of a binary operation, borrowed for the call. */
typedef struct sxml_decimal_operands {
    const sxml_decimal *left_operand;
    const sxml_decimal *right_operand;
} sxml_decimal_operands;

/* The $rounding-mode values of fn:round (F&O 4.0, fn:round). */
typedef enum sxml_rounding_mode {
    /* Toward negative infinity. */
    SXML_ROUNDING_MODE_FLOOR,
    /* Toward positive infinity. */
    SXML_ROUNDING_MODE_CEILING,
    /* Toward zero (truncation). */
    SXML_ROUNDING_MODE_TOWARD_ZERO,
    /* Away from zero. */
    SXML_ROUNDING_MODE_AWAY_FROM_ZERO,
    /* To the nearest, a tie toward negative infinity. */
    SXML_ROUNDING_MODE_HALF_TO_FLOOR,
    /* To the nearest, a tie toward positive infinity (fn:round's default). */
    SXML_ROUNDING_MODE_HALF_TO_CEILING,
    /* To the nearest, a tie toward zero. */
    SXML_ROUNDING_MODE_HALF_TOWARD_ZERO,
    /* To the nearest, a tie away from zero. */
    SXML_ROUNDING_MODE_HALF_AWAY_FROM_ZERO,
    /* To the nearest, a tie to an even last digit
       (fn:round-half-to-even). */
    SXML_ROUNDING_MODE_HALF_TO_EVEN,
} sxml_rounding_mode;

/* A decimal to round, as fn:round($value, $precision, $rounding-mode). */
typedef struct sxml_decimal_rounding {
    /* Borrowed for the call. */
    const sxml_decimal *decimal;
    /* Digits kept after the decimal point; a negative precision rounds to
       a multiple of a power of ten (-2 rounds to hundreds). */
    int64_t precision;
    sxml_rounding_mode rounding_mode;
} sxml_decimal_rounding;

/*
 * Common contract of the functions below that take a configuration and
 * write a decimal to their last parameter: on success the output is a new
 * decimal the caller finalizes; on failure the output is untouched. They
 * return SXML_STATUS_INVALID_ARGUMENT for a configuration with the null
 * allocator or digit counts outside their ranges, SXML_STATUS_OUT_OF_MEMORY
 * (or the allocator's own failure) when memory runs out, and
 * SXML_STATUS_LIMIT_EXCEEDED for a result with more than
 * maximum_digit_count digits.
 */

/*
 * Parses text, the lexical form of xs:decimal (XSD 1.1 Part 2, section
 * 3.3.3: an optional sign, digits with an optional ".", at least one
 * digit; no exponent and no whitespace). Every xs:integer lexical form is
 * one. Returns SXML_STATUS_MALFORMED_INPUT for any other text (casting
 * raises FORG0001).
 */
SXML_NODISCARD sxml_status sxml_decimal_parse(
    const sxml_decimal_configuration *configuration, sxml_byte_view text,
    sxml_decimal *decimal);

/* Makes the decimal equal to integer. */
SXML_NODISCARD sxml_status sxml_decimal_from_integer(
    const sxml_decimal_configuration *configuration, int64_t integer,
    sxml_decimal *decimal);

/* Makes the decimal equal to unsigned_integer (xs:unsignedLong values). */
SXML_NODISCARD sxml_status sxml_decimal_from_unsigned_integer(
    const sxml_decimal_configuration *configuration, uint64_t unsigned_integer,
    sxml_decimal *decimal);

/*
 * Makes the decimal exactly equal to double_number: every finite double is
 * a decimal of at most 767 significant digits. For comparing numbers by
 * exact magnitude (fn:atomic-equal, F&O 4.0, section 2.2.1). Returns
 * SXML_STATUS_INVALID_ARGUMENT for NaN and the infinities.
 */
SXML_NODISCARD sxml_status sxml_decimal_from_double_exactly(
    const sxml_decimal_configuration *configuration, double double_number,
    sxml_decimal *decimal);

/*
 * Makes the decimal with the fewest significant digits (at most 17) that
 * converts back to double_number, the nearest such decimal when several
 * have that many digits. For casting xs:double to xs:decimal (F&O 4.0,
 * "Casting to xs:decimal"): 0.1e0 gives 0.1, not the exact binary value. Returns
 * SXML_STATUS_INVALID_ARGUMENT for NaN and the infinities (casting raises
 * FOCA0002).
 */
SXML_NODISCARD sxml_status sxml_decimal_from_double_shortest(
    const sxml_decimal_configuration *configuration, double double_number,
    sxml_decimal *decimal);

/*
 * As sxml_decimal_from_double_shortest for an xs:float: the fewest digits
 * (at most 9) that convert back to float_number.
 */
SXML_NODISCARD sxml_status sxml_decimal_from_float_shortest(
    const sxml_decimal_configuration *configuration, float float_number,
    sxml_decimal *decimal);

/* Makes the decimal equal to source. */
SXML_NODISCARD sxml_status sxml_decimal_copy(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal *source, sxml_decimal *copy);

/*
 * Releases the coefficient of decimal through the allocator it was made
 * with and leaves decimal empty, so finalizing twice is harmless. Does
 * nothing when decimal is NULL. Needs no decimal scope.
 */
void sxml_decimal_finalize(sxml_decimal *decimal);

/*
 * Writes integer when decimal is integral and within int64_t. Returns
 * SXML_STATUS_INVALID_ARGUMENT when it has a fraction (round first) and
 * SXML_STATUS_ARITHMETIC_OVERFLOW when it is outside int64_t; *integer is
 * then untouched.
 */
SXML_NODISCARD sxml_status sxml_decimal_to_integer(const sxml_decimal *decimal,
                                                   int64_t *integer);

/*
 * The double nearest to decimal, a tie to even (IEEE 754 round to
 * nearest); infinity beyond the double range, as casting xs:decimal to
 * xs:double requires (F&O 4.0, "Casting to xs:double"). Never fails.
 */
double sxml_decimal_to_double(const sxml_decimal *decimal);

/* As sxml_decimal_to_double for xs:float, rounded once. */
float sxml_decimal_to_float(const sxml_decimal *decimal);

/*
 * The arithmetic of F&O 4.0, "Arithmetic operators on numeric values", on xs:decimal operands. Sum,
 * difference and product are exact (op:numeric-add, op:numeric-subtract,
 * op:numeric-multiply).
 */
SXML_NODISCARD sxml_status sxml_decimal_add(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operands *operands, sxml_decimal *sum);

SXML_NODISCARD sxml_status sxml_decimal_subtract(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operands *operands, sxml_decimal *difference);

SXML_NODISCARD sxml_status sxml_decimal_multiply(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operands *operands, sxml_decimal *product);

/*
 * left_operand divided by right_operand (op:numeric-divide), exact when the
 * quotient fits in the precision chosen by the configuration
 * (division_digit_count), otherwise rounded to it, a tie to even. Returns
 * SXML_STATUS_INVALID_ARGUMENT when right_operand is zero (the operator
 * raises FOAR0001).
 */
SXML_NODISCARD sxml_status sxml_decimal_divide(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operands *operands, sxml_decimal *quotient);

/*
 * The integer quotient, truncated toward zero (op:numeric-integer-divide).
 * Returns SXML_STATUS_INVALID_ARGUMENT when right_operand is zero
 * (FOAR0001).
 */
SXML_NODISCARD sxml_status sxml_decimal_integer_divide(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operands *operands, sxml_decimal *quotient);

/*
 * The remainder left - (left idiv right) * right, with the sign of
 * left_operand (op:numeric-mod). Returns SXML_STATUS_INVALID_ARGUMENT when
 * right_operand is zero (FOAR0001).
 */
SXML_NODISCARD sxml_status sxml_decimal_modulo(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operands *operands, sxml_decimal *remainder);

/* A division to a number of decimal places, as fn:divide-decimals($value,
   $divisor, $precision). */
typedef struct sxml_decimal_division_request {
    /* left_operand is $value, right_operand $divisor; borrowed. */
    const sxml_decimal_operands *operands;
    /* Decimal places of the quotient; a negative precision makes it a
       multiple of a power of ten (-2: of hundreds). */
    int64_t precision;
} sxml_decimal_division_request;

/* The two results of sxml_decimal_divide_to_precision; both new decimals
   the caller finalizes. */
typedef struct sxml_decimal_division {
    sxml_decimal quotient;
    sxml_decimal remainder;
} sxml_decimal_division;

/*
 * fn:divide-decimals (F&O 4.0, section 4.3.1): the quotient is the
 * multiple of ten to the power -precision furthest from zero whose
 * product with the divisor is at most the value in magnitude, with the
 * sign of value / divisor; the remainder is exactly value - quotient *
 * divisor (so it has the sign of the value). A precision beyond what the
 * digit limit allows is lowered to the largest that fits ("adjusted to the
 * nearest value supported"); one below every digit of the quotient gives
 * the quotient 0. Returns SXML_STATUS_INVALID_ARGUMENT when right_operand
 * is zero (FOAR0001) and SXML_STATUS_LIMIT_EXCEEDED when even the integer
 * part of the quotient has more than maximum_digit_count digits.
 */
SXML_NODISCARD sxml_status sxml_decimal_divide_to_precision(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_division_request *request,
    sxml_decimal_division *division);

/* -decimal (op:numeric-unary-minus). */
SXML_NODISCARD sxml_status sxml_decimal_negate(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal *decimal, sxml_decimal *negation);

/* The absolute value (fn:abs). */
SXML_NODISCARD sxml_status sxml_decimal_absolute(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal *decimal, sxml_decimal *absolute_value);

/*
 * Rounds as fn:round (F&O 4.0) with the given precision and
 * rounding mode; fn:floor and fn:ceiling are precision 0 with
 * SXML_ROUNDING_MODE_FLOOR and SXML_ROUNDING_MODE_CEILING, and
 * fn:round-half-to-even is SXML_ROUNDING_MODE_HALF_TO_EVEN. A decimal with
 * no more fraction digits than precision is returned unchanged.
 */
SXML_NODISCARD sxml_status sxml_decimal_round(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_rounding *rounding, sxml_decimal *rounded);

/* Orders first and second by value (op:numeric-less-than and
   op:numeric-equal); -0 equals 0. Never fails, needs no scope. */
sxml_ordering sxml_decimal_compare(const sxml_decimal *first,
                                   const sxml_decimal *second);

/* True when decimal is 0 (or -0). */
bool sxml_decimal_is_zero(const sxml_decimal *decimal);

/* True when decimal is below 0; false for -0. */
bool sxml_decimal_is_negative(const sxml_decimal *decimal);

/* True when decimal has no fraction (its value is an xs:integer). */
bool sxml_decimal_is_integer(const sxml_decimal *decimal);

/*
 * The byte count of the canonical text of decimal: what casting it to
 * xs:string gives (F&O 4.0, "Casting to xs:string and xs:untypedAtomic"): an optional "-", the integer
 * digits without leading zeros (at least one), and only when the value is
 * not integral a "." and the fraction digits without trailing zeros. No
 * exponent, no "+", and 0 for -0.
 */
size_t sxml_decimal_canonical_text_byte_count(const sxml_decimal *decimal);

/*
 * Writes the canonical text of decimal (ASCII, so UTF-8) to the start of
 * output and writes the written bytes, borrowed from output, to *text.
 * Returns SXML_STATUS_CAPACITY_EXCEEDED, writing nothing, when output is
 * smaller than sxml_decimal_canonical_text_byte_count. Needs no scope.
 */
SXML_NODISCARD sxml_status sxml_decimal_write_canonical_text(
    const sxml_decimal *decimal, sxml_memory_view output,
    sxml_byte_view *text);
