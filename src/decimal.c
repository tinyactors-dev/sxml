/*
 * Decimal arithmetic over libmpdec (src/decimal_internal.h; D8, D36, D42,
 * D49).
 *
 * Every operation that may allocate opens a decimal scope: it names the
 * configuration's allocator to the patched libmpdec allocation hooks
 * (third_party/mpdecimal/libmpdec/mpalloc.c) and restores the previous
 * scope before returning. Results are computed into a local decimal and
 * moved to the output only on success, so outputs are untouched on failure.
 * libmpdec contexts never trap, so its process-global trap handler is never
 * called (D42).
 */
#include "decimal_internal.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- Constants */

/* Digits in one coefficient word of libmpdec: 19 on 64-bit targets, 9 on
   32-bit ones (WebAssembly). */
static const uint64_t sxml_decimal_word_digit_count = MPD_RDIGITS;

/* The largest precision libmpdec supports: about 10^18 on 64-bit targets,
   425000000 on 32-bit ones, where it bounds maximum_digit_count. */
static const uint64_t sxml_decimal_maximum_precision = (uint64_t)MPD_MAX_PREC;

/* 10^0 to 10^19, enough for the digits of one coefficient word. */
static const uint64_t sxml_decimal_powers_of_ten[] = {
    1ULL,
    10ULL,
    100ULL,
    1000ULL,
    10000ULL,
    100000ULL,
    1000000ULL,
    10000000ULL,
    100000000ULL,
    1000000000ULL,
    10000000000ULL,
    100000000000ULL,
    1000000000000ULL,
    10000000000000ULL,
    100000000000000ULL,
    1000000000000000ULL,
    10000000000000000ULL,
    100000000000000000ULL,
    1000000000000000000ULL,
    10000000000000000000ULL,
};

/*
 * Precision for computing the exact value of a double: 2^-1074 times a
 * 53-bit significand has 767 significant digits, 2^971 times one has 309.
 */
static const mpd_ssize_t sxml_decimal_exact_double_precision = 800;

/*
 * Digits of a decimal handed to strtod or strtof. Every value halfway
 * between two doubles has at most 767 significant digits, so the first 780
 * digits followed by a nonzero sticky digit for the rest round exactly as
 * all digits would.
 */
#define SXML_DECIMAL_CONVERSION_DIGIT_COUNT 780

/* Sign, digits, sticky digit, "e", exponent sign and digits, terminator. */
#define SXML_DECIMAL_CONVERSION_TEXT_CAPACITY                                \
    (SXML_DECIMAL_CONVERSION_DIGIT_COUNT + 32)

/* ------------------------------------------------------------ Scope */

/* Opens the decimal scope of configuration (D42); returns the scope to
   restore with sxml_decimal_leave_scope. */
static const sxml_allocator *sxml_decimal_enter_scope(
    const sxml_decimal_configuration *configuration) {
    return mpd_sxml_exchange_allocator_scope(&configuration->allocator);
}

static void sxml_decimal_leave_scope(const sxml_allocator *previous_scope) {
    (void)mpd_sxml_exchange_allocator_scope(previous_scope);
}

static bool sxml_decimal_configuration_is_valid(
    const sxml_decimal_configuration *configuration) {
    return !sxml_allocator_is_null(configuration->allocator) &&
           configuration->maximum_digit_count >= 1 &&
           (uint64_t)configuration->maximum_digit_count <=
               sxml_decimal_maximum_precision &&
           configuration->division_digit_count >= 1 &&
           configuration->division_digit_count <=
               configuration->maximum_digit_count;
}

/*
 * A libmpdec context that never traps, with the widest exponent range and
 * the given precision (significant digits of a result).
 */
static mpd_context_t sxml_decimal_context(mpd_ssize_t precision) {
    return (mpd_context_t){
        .prec = precision,
        .emax = MPD_MAX_EMAX,
        .emin = MPD_MIN_EMIN,
        .traps = 0,
        .status = 0,
        .newtrap = 0,
        .round = MPD_ROUND_HALF_EVEN,
        .clamp = 0,
        .allcr = 1,
    };
}

static mpd_context_t sxml_decimal_limit_context(
    const sxml_decimal_configuration *configuration) {
    return sxml_decimal_context(
        (mpd_ssize_t)configuration->maximum_digit_count);
}

/*
 * Makes decimal an empty libmpdec number: a static struct with a dynamic
 * coefficient of the minimum size. Call inside a scope.
 */
static sxml_status sxml_decimal_start(sxml_decimal *decimal) {
    mpd_uint_t *words =
        mpd_alloc((mpd_size_t)MPD_MINALLOC, sizeof(mpd_uint_t));
    if (words == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    decimal->number = (mpd_t){0};
    decimal->number.flags = MPD_STATIC;
    decimal->number.alloc = MPD_MINALLOC;
    decimal->number.data = words;
    return SXML_STATUS_OK;
}

void sxml_decimal_finalize(sxml_decimal *decimal) {
    if (decimal == NULL) {
        return;
    }
    mpd_del(&decimal->number);
    /* Static data with no words: a second finalize frees nothing. */
    decimal->number = (mpd_t){0};
    decimal->number.flags = MPD_STATIC | MPD_STATIC_DATA;
}

/* Maps the status flags of a libmpdec operation to a status. */
static sxml_status sxml_decimal_status_from_flags(uint32_t flags) {
    if ((flags & MPD_Malloc_error) != 0) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    if ((flags & (MPD_Division_impossible | MPD_Overflow)) != 0) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if ((flags & (MPD_Invalid_operation | MPD_Division_by_zero |
                  MPD_Division_undefined | MPD_Conversion_syntax)) != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Canonical layout */

/* The digit of the coefficient at position, counted from the least
   significant digit, 0 to 9. */
static uint8_t sxml_decimal_digit_at(const mpd_t *number, uint64_t position) {
    mpd_uint_t word =
        number->data[(size_t)(position / sxml_decimal_word_digit_count)];
    uint64_t power =
        sxml_decimal_powers_of_ten[(size_t)(position %
                                            sxml_decimal_word_digit_count)];
    return (uint8_t)(((uint64_t)word / power) % 10U);
}

/*
 * The value of a number written as its canonical text needs: the
 * coefficient digits from the most significant down to lowest_position
 * (trailing zeros of a fraction left out) times 10^exponent.
 */
typedef struct sxml_decimal_layout {
    bool is_zero;
    bool is_negative;
    /* Position of the lowest coefficient digit written. */
    uint64_t lowest_position;
    /* Number of coefficient digits written. */
    uint64_t significant_digit_count;
    /* Power of ten of the lowest written digit. */
    int64_t exponent;
} sxml_decimal_layout;

static sxml_decimal_layout sxml_decimal_lay_out(const mpd_t *number) {
    if (mpd_iszero(number)) {
        return (sxml_decimal_layout){
            .is_zero = true,
            .is_negative = false,
            .lowest_position = 0,
            .significant_digit_count = 1,
            .exponent = 0,
        };
    }
    uint64_t digit_count = (uint64_t)number->digits;
    int64_t exponent = (int64_t)number->exp;
    uint64_t dropped_digit_count = 0;
    if (exponent < 0) {
        /* Trailing zeros of the fraction are not part of the canonical
           form (F&O 4.0, "Casting to xs:string and xs:untypedAtomic"). */
        uint64_t trailing_zero_count = (uint64_t)mpd_trail_zeros(number);
        uint64_t fraction_digit_count = (uint64_t)(-exponent);
        dropped_digit_count = trailing_zero_count < fraction_digit_count
                                  ? trailing_zero_count
                                  : fraction_digit_count;
    }
    return (sxml_decimal_layout){
        .is_zero = false,
        .is_negative = mpd_isnegative(number) != 0,
        .lowest_position = dropped_digit_count,
        .significant_digit_count = digit_count - dropped_digit_count,
        .exponent = exponent + (int64_t)dropped_digit_count,
    };
}

/* Digits of the canonical text, integer and fraction digits together. */
static uint64_t sxml_decimal_layout_digit_count(sxml_decimal_layout layout) {
    if (layout.exponent >= 0) {
        return layout.significant_digit_count + (uint64_t)layout.exponent;
    }
    uint64_t fraction_digit_count = (uint64_t)(-layout.exponent);
    if (layout.significant_digit_count > fraction_digit_count) {
        return layout.significant_digit_count;
    }
    /* "0." and the fraction. */
    return fraction_digit_count + 1;
}

/* Where canonical text goes: bytes NULL only counts. */
typedef struct sxml_decimal_text_sink {
    uint8_t *bytes;
    size_t byte_count;
} sxml_decimal_text_sink;

static void sxml_decimal_sink_put(sxml_decimal_text_sink *sink,
                                  uint8_t byte) {
    if (sink->bytes != NULL) {
        sink->bytes[sink->byte_count] = byte;
    }
    sink->byte_count += 1;
}

/* A range of coefficient digits to write, most significant first. */
typedef struct sxml_decimal_digit_range {
    /* One past the highest position. */
    uint64_t end_position;
    uint64_t lowest_position;
} sxml_decimal_digit_range;

static void sxml_decimal_sink_put_digits(sxml_decimal_text_sink *sink,
                                         const mpd_t *number,
                                         sxml_decimal_digit_range range) {
    for (uint64_t position = range.end_position;
         position > range.lowest_position; position -= 1) {
        sxml_decimal_sink_put(
            sink, (uint8_t)('0' + sxml_decimal_digit_at(number, position - 1)));
    }
}

static void sxml_decimal_sink_put_zeros(sxml_decimal_text_sink *sink,
                                        uint64_t zero_count) {
    for (uint64_t index = 0; index < zero_count; index += 1) {
        sxml_decimal_sink_put(sink, '0');
    }
}

/* Emits the canonical text of number (F&O 4.0, "Casting to xs:string and
   xs:untypedAtomic": no exponent, no trailing fraction zeros). */
static void sxml_decimal_emit_canonical_text(const mpd_t *number,
                                             sxml_decimal_text_sink *sink) {
    sxml_decimal_layout layout = sxml_decimal_lay_out(number);
    if (layout.is_zero) {
        sxml_decimal_sink_put(sink, '0');
        return;
    }
    if (layout.is_negative) {
        sxml_decimal_sink_put(sink, '-');
    }
    uint64_t end_position =
        layout.lowest_position + layout.significant_digit_count;
    if (layout.exponent >= 0) {
        sxml_decimal_sink_put_digits(
            sink, number,
            (sxml_decimal_digit_range){
                .end_position = end_position,
                .lowest_position = layout.lowest_position,
            });
        sxml_decimal_sink_put_zeros(sink, (uint64_t)layout.exponent);
        return;
    }
    uint64_t fraction_digit_count = (uint64_t)(-layout.exponent);
    if (layout.significant_digit_count > fraction_digit_count) {
        uint64_t point_position = layout.lowest_position + fraction_digit_count;
        sxml_decimal_sink_put_digits(
            sink, number,
            (sxml_decimal_digit_range){
                .end_position = end_position,
                .lowest_position = point_position,
            });
        sxml_decimal_sink_put(sink, '.');
        sxml_decimal_sink_put_digits(
            sink, number,
            (sxml_decimal_digit_range){
                .end_position = point_position,
                .lowest_position = layout.lowest_position,
            });
        return;
    }
    sxml_decimal_sink_put(sink, '0');
    sxml_decimal_sink_put(sink, '.');
    sxml_decimal_sink_put_zeros(
        sink, fraction_digit_count - layout.significant_digit_count);
    sxml_decimal_sink_put_digits(
        sink, number,
        (sxml_decimal_digit_range){
            .end_position = end_position,
            .lowest_position = layout.lowest_position,
        });
}

size_t sxml_decimal_canonical_text_byte_count(const sxml_decimal *decimal) {
    sxml_decimal_text_sink sink = {.bytes = NULL, .byte_count = 0};
    sxml_decimal_emit_canonical_text(&decimal->number, &sink);
    return sink.byte_count;
}

sxml_status sxml_decimal_write_canonical_text(const sxml_decimal *decimal,
                                              sxml_memory_view output,
                                              sxml_byte_view *text) {
    size_t byte_count = sxml_decimal_canonical_text_byte_count(decimal);
    if (output.byte_count < byte_count || output.bytes == NULL) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    sxml_decimal_text_sink sink = {.bytes = output.bytes, .byte_count = 0};
    sxml_decimal_emit_canonical_text(&decimal->number, &sink);
    *text = (sxml_byte_view){.bytes = output.bytes, .byte_count = byte_count};
    return SXML_STATUS_OK;
}

/*
 * Finishes an operation: maps flags, then checks the configured limit on
 * the digits of the result.
 */
static sxml_status sxml_decimal_check_result(
    const sxml_decimal_configuration *configuration, const mpd_t *number,
    uint32_t flags) {
    sxml_status status = sxml_decimal_status_from_flags(flags);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal_layout layout = sxml_decimal_lay_out(number);
    if (sxml_decimal_layout_digit_count(layout) >
        (uint64_t)configuration->maximum_digit_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Parsing */

static bool sxml_decimal_is_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

/* The parts of a decimal lexical form, as byte ranges of the text. */
typedef struct sxml_decimal_lexical_form {
    bool is_negative;
    /* Integer digits without leading zeros. */
    sxml_byte_view integer_digits;
    /* Fraction digits without trailing zeros. */
    sxml_byte_view fraction_digits;
} sxml_decimal_lexical_form;

/*
 * Splits text by the xs:decimal lexical space (XSD 1.1 Part 2, section
 * 3.3.3.1): (\+|-)?([0-9]+(\.[0-9]*)?|\.[0-9]+).
 */
static sxml_status sxml_decimal_split_lexical_form(
    sxml_byte_view text, sxml_decimal_lexical_form *form) {
    size_t offset = 0;
    bool is_negative = false;
    if (offset < text.byte_count &&
        (text.bytes[offset] == '+' || text.bytes[offset] == '-')) {
        is_negative = text.bytes[offset] == '-';
        offset += 1;
    }
    size_t integer_start = offset;
    while (offset < text.byte_count && sxml_decimal_is_digit(text.bytes[offset])) {
        offset += 1;
    }
    size_t integer_end = offset;
    size_t fraction_start = offset;
    size_t fraction_end = offset;
    if (offset < text.byte_count && text.bytes[offset] == '.') {
        offset += 1;
        fraction_start = offset;
        while (offset < text.byte_count &&
               sxml_decimal_is_digit(text.bytes[offset])) {
            offset += 1;
        }
        fraction_end = offset;
    }
    if (offset != text.byte_count ||
        (integer_end == integer_start && fraction_end == fraction_start)) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    while (integer_start < integer_end && text.bytes[integer_start] == '0') {
        integer_start += 1;
    }
    while (fraction_end > fraction_start &&
           text.bytes[fraction_end - 1] == '0') {
        fraction_end -= 1;
    }
    *form = (sxml_decimal_lexical_form){
        .is_negative = is_negative,
        .integer_digits =
            {
                .bytes = text.bytes + integer_start,
                .byte_count = integer_end - integer_start,
            },
        .fraction_digits =
            {
                .bytes = text.bytes + fraction_start,
                .byte_count = fraction_end - fraction_start,
            },
    };
    return SXML_STATUS_OK;
}

/* Appends view to scratch (in/out); scratch has room. */
static void sxml_decimal_append(sxml_memory_view *scratch, size_t *used,
                                sxml_byte_view view) {
    if (view.byte_count != 0) {
        memcpy(scratch->bytes + *used, view.bytes, view.byte_count);
        *used += view.byte_count;
    }
}

/*
 * Parses form with libmpdec into decimal (an empty number). libmpdec reads
 * a NUL-terminated string, so the significant digits are copied into
 * scratch memory from the configuration's allocator first.
 */
static sxml_status sxml_decimal_parse_form(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_lexical_form *form, sxml_decimal *decimal) {
    /* Sign, "0", ".", digits and the terminator. */
    size_t scratch_byte_count = form->integer_digits.byte_count +
                                form->fraction_digits.byte_count + 4;
    void *scratch_memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        configuration->allocator,
        (sxml_allocation_request){.byte_count = scratch_byte_count,
                                  .alignment = 1},
        &scratch_memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_memory_view scratch = {.bytes = scratch_memory,
                                .byte_count = scratch_byte_count};
    size_t used = 0;
    sxml_decimal_append(&scratch, &used,
                        form->is_negative ? SXML_BYTE_VIEW_FROM_LITERAL("-")
                                          : SXML_BYTE_VIEW_FROM_LITERAL(""));
    sxml_decimal_append(&scratch, &used,
                        form->integer_digits.byte_count == 0
                            ? SXML_BYTE_VIEW_FROM_LITERAL("0")
                            : form->integer_digits);
    if (form->fraction_digits.byte_count != 0) {
        sxml_decimal_append(&scratch, &used, SXML_BYTE_VIEW_FROM_LITERAL("."));
        sxml_decimal_append(&scratch, &used, form->fraction_digits);
    }
    scratch.bytes[used] = 0;
    mpd_context_t context = sxml_decimal_limit_context(configuration);
    uint32_t flags = 0;
    mpd_qset_string(&decimal->number, (const char *)scratch.bytes, &context,
                    &flags);
    sxml_allocator_release(configuration->allocator, scratch_memory);
    return sxml_decimal_check_result(configuration, &decimal->number, flags);
}

sxml_status sxml_decimal_parse(const sxml_decimal_configuration *configuration,
                               sxml_byte_view text, sxml_decimal *decimal) {
    if (!sxml_decimal_configuration_is_valid(configuration) ||
        (text.bytes == NULL && text.byte_count != 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_decimal_lexical_form form;
    sxml_status status = sxml_decimal_split_lexical_form(text, &form);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The same count as sxml_decimal_layout_digit_count, checked before
       any memory is taken for an over-long text. */
    size_t digit_count =
        form.integer_digits.byte_count != 0
            ? form.integer_digits.byte_count + form.fraction_digits.byte_count
            : form.fraction_digits.byte_count + 1;
    if (digit_count > (size_t)configuration->maximum_digit_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const sxml_allocator *previous_scope =
        sxml_decimal_enter_scope(configuration);
    sxml_decimal result;
    status = sxml_decimal_start(&result);
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_parse_form(configuration, &form, &result);
        if (status != SXML_STATUS_OK) {
            sxml_decimal_finalize(&result);
        }
    }
    sxml_decimal_leave_scope(previous_scope);
    if (status == SXML_STATUS_OK) {
        *decimal = result;
    }
    return status;
}

/* ----------------------------------------------------------- Integers */

/* Which C integer a decimal is made from. */
typedef struct sxml_decimal_integer_source {
    bool is_unsigned;
    int64_t integer;
    uint64_t unsigned_integer;
} sxml_decimal_integer_source;

static sxml_status sxml_decimal_from_integer_source(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_integer_source *source, sxml_decimal *decimal) {
    if (!sxml_decimal_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_allocator *previous_scope =
        sxml_decimal_enter_scope(configuration);
    sxml_decimal result;
    sxml_status status = sxml_decimal_start(&result);
    if (status == SXML_STATUS_OK) {
        /* 20 digits always fit: the precision only bounds the check. */
        mpd_context_t context = sxml_decimal_context(MPD_MAX_PREC);
        uint32_t flags = 0;
        if (source->is_unsigned) {
            mpd_qset_u64(&result.number, source->unsigned_integer, &context,
                         &flags);
        } else {
            mpd_qset_i64(&result.number, source->integer, &context, &flags);
        }
        status = sxml_decimal_check_result(configuration, &result.number, flags);
        if (status != SXML_STATUS_OK) {
            sxml_decimal_finalize(&result);
        }
    }
    sxml_decimal_leave_scope(previous_scope);
    if (status == SXML_STATUS_OK) {
        *decimal = result;
    }
    return status;
}

sxml_status sxml_decimal_from_integer(
    const sxml_decimal_configuration *configuration, int64_t integer,
    sxml_decimal *decimal) {
    return sxml_decimal_from_integer_source(
        configuration,
        &(sxml_decimal_integer_source){
            .is_unsigned = false, .integer = integer, .unsigned_integer = 0},
        decimal);
}

sxml_status sxml_decimal_from_unsigned_integer(
    const sxml_decimal_configuration *configuration, uint64_t unsigned_integer,
    sxml_decimal *decimal) {
    return sxml_decimal_from_integer_source(
        configuration,
        &(sxml_decimal_integer_source){.is_unsigned = true,
                                       .integer = 0,
                                       .unsigned_integer = unsigned_integer},
        decimal);
}

sxml_status sxml_decimal_to_integer(const sxml_decimal *decimal,
                                    int64_t *integer) {
    const mpd_t *number = &decimal->number;
    if (mpd_iszero(number)) {
        *integer = 0;
        return SXML_STATUS_OK;
    }
    if (!mpd_isinteger(number)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    bool is_negative = mpd_isnegative(number) != 0;
    /* The magnitude of INT64_MIN is one more than INT64_MAX. */
    uint64_t limit = is_negative ? (uint64_t)INT64_MAX + 1U : (uint64_t)INT64_MAX;
    int64_t exponent = (int64_t)number->exp;
    /* For an integral value with a negative exponent, the lowest -exponent
       digits are zeros. */
    uint64_t lowest_position = exponent < 0 ? (uint64_t)(-exponent) : 0;
    uint64_t magnitude = 0;
    for (uint64_t position = (uint64_t)number->digits;
         position > lowest_position; position -= 1) {
        uint8_t digit = sxml_decimal_digit_at(number, position - 1);
        if (magnitude > (limit - digit) / 10U) {
            return SXML_STATUS_ARITHMETIC_OVERFLOW;
        }
        magnitude = magnitude * 10U + digit;
    }
    for (int64_t zero_index = 0; zero_index < exponent; zero_index += 1) {
        if (magnitude > limit / 10U) {
            return SXML_STATUS_ARITHMETIC_OVERFLOW;
        }
        magnitude *= 10U;
    }
    if (is_negative) {
        /* Two's complement: the magnitude fits uint64_t and its negation
           is the value; computed without signed overflow. */
        *integer = magnitude == (uint64_t)INT64_MAX + 1U
                       ? INT64_MIN
                       : -(int64_t)magnitude;
    } else {
        *integer = (int64_t)magnitude;
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------- Binary floating point */

/*
 * Writes number as "[-]digitsE[-]exponent" with a terminator for strtod or
 * strtof. No decimal point: the text reads the same in every C locale.
 */
static void sxml_decimal_write_conversion_text(const mpd_t *number,
                                               sxml_memory_view text) {
    sxml_decimal_text_sink sink = {.bytes = text.bytes, .byte_count = 0};
    if (mpd_isnegative(number)) {
        sxml_decimal_sink_put(&sink, '-');
    }
    uint64_t digit_count = (uint64_t)number->digits;
    int64_t exponent = (int64_t)number->exp;
    uint64_t written_digit_count = digit_count;
    if (digit_count > SXML_DECIMAL_CONVERSION_DIGIT_COUNT) {
        written_digit_count = SXML_DECIMAL_CONVERSION_DIGIT_COUNT;
    }
    uint64_t lowest_position = digit_count - written_digit_count;
    sxml_decimal_sink_put_digits(
        &sink, number,
        (sxml_decimal_digit_range){.end_position = digit_count,
                                   .lowest_position = lowest_position});
    exponent += (int64_t)lowest_position;
    bool has_nonzero_rest = lowest_position != 0 &&
                            (uint64_t)mpd_trail_zeros(number) < lowest_position;
    if (has_nonzero_rest) {
        sxml_decimal_sink_put(&sink, '1');
        exponent -= 1;
    }
    sxml_decimal_sink_put(&sink, 'e');
    uint64_t exponent_magnitude = 0;
    if (exponent < 0) {
        sxml_decimal_sink_put(&sink, '-');
        exponent_magnitude = (uint64_t)(-exponent);
    } else {
        exponent_magnitude = (uint64_t)exponent;
    }
    uint8_t reversed_digits[24];
    size_t reversed_digit_count = 0;
    do {
        reversed_digits[reversed_digit_count] =
            (uint8_t)('0' + exponent_magnitude % 10U);
        reversed_digit_count += 1;
        exponent_magnitude /= 10U;
    } while (exponent_magnitude != 0);
    while (reversed_digit_count > 0) {
        reversed_digit_count -= 1;
        sxml_decimal_sink_put(&sink, reversed_digits[reversed_digit_count]);
    }
    sxml_decimal_sink_put(&sink, 0);
}

/*
 * The exact powers of ten of a double: 10^22 is the largest whose value
 * fits the 53-bit significand.
 */
static const double sxml_exact_powers_of_ten[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

bool sxml_scaled_integer_to_double(sxml_scaled_integer scaled, double *number) {
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
    const int64_t power_count = (int64_t)(sizeof(sxml_exact_powers_of_ten) /
                                          sizeof(sxml_exact_powers_of_ten[0]));
    if (scaled.mantissa == 0 || scaled.mantissa >= SXML_EXACT_DOUBLE_MANTISSA_LIMIT ||
        scaled.power <= -power_count || scaled.power >= power_count) {
        return false;
    }
    double magnitude = (double)scaled.mantissa;
    if (scaled.power < 0) {
        magnitude /= sxml_exact_powers_of_ten[-scaled.power];
    } else {
        magnitude *= sxml_exact_powers_of_ten[scaled.power];
    }
    *number = scaled.is_negative ? -magnitude : magnitude;
    return true;
#else
    (void)scaled;
    (void)number;
    return false;
#endif
}

/* The decimal as a scaled integer, when its coefficient is below
   SXML_EXACT_DOUBLE_MANTISSA_LIMIT (15 digits fit one word of 10^19 or
   two of 10^9); false for larger coefficients and special values. */
static bool sxml_decimal_get_scaled_integer(const mpd_t *number,
                                            sxml_scaled_integer *scaled) {
    if (mpd_isspecial(number) || number->digits > 15) {
        return false;
    }
    uint64_t mantissa = 0;
    for (mpd_ssize_t word_index = number->len; word_index > 0; word_index -= 1) {
        mantissa = mantissa * (uint64_t)MPD_RADIX + (uint64_t)number->data[word_index - 1];
    }
    *scaled = (sxml_scaled_integer){.is_negative = mpd_isnegative(number) != 0,
                                    .mantissa = mantissa,
                                    .power = (int64_t)number->exp};
    return true;
}

double sxml_decimal_to_double(const sxml_decimal *decimal) {
    if (mpd_iszero(&decimal->number)) {
        return 0.0;
    }
    sxml_scaled_integer scaled = {.is_negative = false, .mantissa = 0, .power = 0};
    double fast_number = 0.0;
    if (sxml_decimal_get_scaled_integer(&decimal->number, &scaled) &&
        sxml_scaled_integer_to_double(scaled, &fast_number)) {
        return fast_number;
    }
    uint8_t text[SXML_DECIMAL_CONVERSION_TEXT_CAPACITY];
    sxml_decimal_write_conversion_text(
        &decimal->number,
        (sxml_memory_view){.bytes = text, .byte_count = sizeof(text)});
    /* strtod rounds correctly to nearest and gives HUGE_VAL beyond the
       range; errno is not read. */
    return strtod((const char *)text, NULL);
}

float sxml_decimal_to_float(const sxml_decimal *decimal) {
    if (mpd_iszero(&decimal->number)) {
        return 0.0F;
    }
    uint8_t text[SXML_DECIMAL_CONVERSION_TEXT_CAPACITY];
    sxml_decimal_write_conversion_text(
        &decimal->number,
        (sxml_memory_view){.bytes = text, .byte_count = sizeof(text)});
    return strtof((const char *)text, NULL);
}

/* An IEEE 754 binary64 value split as (-1)^sign * significand * 2^power. */
typedef struct sxml_decimal_binary_parts {
    bool is_finite;
    bool is_negative;
    uint64_t significand;
    int64_t power_of_two;
} sxml_decimal_binary_parts;

static sxml_decimal_binary_parts sxml_decimal_split_double(
    double double_number) {
    uint64_t bits = 0;
    memcpy(&bits, &double_number, sizeof(bits));
    uint64_t biased_exponent = (bits >> 52U) & 0x7FFU;
    uint64_t fraction = bits & ((1ULL << 52U) - 1U);
    sxml_decimal_binary_parts parts = {
        .is_finite = biased_exponent != 0x7FFU,
        .is_negative = (bits >> 63U) != 0,
        .significand = fraction,
        .power_of_two = -1074,
    };
    if (biased_exponent != 0) {
        /* A normal number: the implicit leading bit. */
        parts.significand = fraction | (1ULL << 52U);
        parts.power_of_two = (int64_t)biased_exponent - 1075;
    }
    return parts;
}

/*
 * Sets result (an empty number, in a scope) to base^exponent exactly, by
 * libmpdec's integer power at the exact-double precision.
 */
typedef struct sxml_decimal_power {
    uint32_t base;
    int64_t exponent;
} sxml_decimal_power;

static sxml_status sxml_decimal_compute_power(sxml_decimal_power power,
                                              mpd_t *result) {
    mpd_context_t context =
        sxml_decimal_context(sxml_decimal_exact_double_precision);
    uint32_t flags = 0;
    sxml_decimal base;
    sxml_decimal exponent;
    sxml_status status = sxml_decimal_start(&base);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_decimal_start(&exponent);
    if (status == SXML_STATUS_OK) {
        mpd_qset_u32(&base.number, power.base, &context, &flags);
        mpd_qset_i64(&exponent.number, power.exponent, &context, &flags);
        mpd_qpow(result, &base.number, &exponent.number, &context, &flags);
        status = sxml_decimal_status_from_flags(flags);
        if (status == SXML_STATUS_OK && (flags & MPD_Inexact) != 0) {
            /* Unreachable: the precision covers every double. */
            status = SXML_STATUS_LIMIT_EXCEEDED;
        }
        sxml_decimal_finalize(&exponent);
    }
    sxml_decimal_finalize(&base);
    return status;
}

/*
 * Sets result (an empty number, in a scope) to the exact value of a finite
 * double: significand * 2^power, which for a negative power is
 * significand * 5^-power * 10^power.
 */
static sxml_status sxml_decimal_compute_exact_double(
    sxml_decimal_binary_parts parts, mpd_t *result) {
    mpd_context_t context =
        sxml_decimal_context(sxml_decimal_exact_double_precision);
    uint32_t flags = 0;
    mpd_qset_u64(result, parts.significand, &context, &flags);
    sxml_status status = sxml_decimal_status_from_flags(flags);
    if (status != SXML_STATUS_OK || parts.significand == 0 ||
        parts.power_of_two == 0) {
        return status;
    }
    sxml_decimal factor;
    status = sxml_decimal_start(&factor);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool is_negative_power = parts.power_of_two < 0;
    status = sxml_decimal_compute_power(
        (sxml_decimal_power){
            .base = is_negative_power ? 5U : 2U,
            .exponent =
                is_negative_power ? -parts.power_of_two : parts.power_of_two,
        },
        &factor.number);
    if (status == SXML_STATUS_OK) {
        mpd_qmul(result, result, &factor.number, &context, &flags);
        status = sxml_decimal_status_from_flags(flags);
        if (status == SXML_STATUS_OK && is_negative_power) {
            /* The coefficient stays; only the power of ten moves. */
            result->exp += (mpd_ssize_t)parts.power_of_two;
        }
    }
    sxml_decimal_finalize(&factor);
    return status;
}

/* Which conversion from binary floating point to make. */
typedef enum sxml_decimal_binary_conversion {
    SXML_DECIMAL_BINARY_CONVERSION_EXACT,
    SXML_DECIMAL_BINARY_CONVERSION_SHORTEST_DOUBLE,
    SXML_DECIMAL_BINARY_CONVERSION_SHORTEST_FLOAT,
} sxml_decimal_binary_conversion;

typedef struct sxml_decimal_binary_request {
    sxml_decimal_binary_conversion conversion;
    double double_number;
} sxml_decimal_binary_request;

/* True when candidate converts back to the requested number. */
static bool sxml_decimal_converts_back(const sxml_decimal *candidate,
                                       const sxml_decimal_binary_request *request) {
    switch (request->conversion) {
    case SXML_DECIMAL_BINARY_CONVERSION_EXACT:
        return true;
    case SXML_DECIMAL_BINARY_CONVERSION_SHORTEST_DOUBLE:
        return sxml_decimal_to_double(candidate) == request->double_number;
    case SXML_DECIMAL_BINARY_CONVERSION_SHORTEST_FLOAT:
        return sxml_decimal_to_float(candidate) ==
               (float)request->double_number;
    }
    return true;
}

/*
 * Rounds exact (in a scope) to the fewest significant digits that convert
 * back, nearest first (a tie to even), into result, an empty number; for
 * the exact conversion, copies it.
 */
static sxml_status sxml_decimal_shorten(
    const sxml_decimal *exact, const sxml_decimal_binary_request *request,
    sxml_decimal *result) {
    if (request->conversion == SXML_DECIMAL_BINARY_CONVERSION_EXACT) {
        uint32_t flags = 0;
        (void)mpd_qcopy(&result->number, &exact->number, &flags);
        return sxml_decimal_status_from_flags(flags);
    }
    mpd_ssize_t maximum_precision =
        request->conversion == SXML_DECIMAL_BINARY_CONVERSION_SHORTEST_FLOAT
            ? 9
            : 17;
    for (mpd_ssize_t precision = 1; precision <= maximum_precision;
         precision += 1) {
        mpd_context_t context = sxml_decimal_context(precision);
        uint32_t flags = 0;
        mpd_qplus(&result->number, &exact->number, &context, &flags);
        sxml_status status = sxml_decimal_status_from_flags(flags);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (sxml_decimal_converts_back(result, request)) {
            return SXML_STATUS_OK;
        }
    }
    /* Unreachable: 17 (9) digits always convert back (IEEE 754-2019,
       section 5.12.2). Keep the exact value then. */
    uint32_t flags = 0;
    (void)mpd_qcopy(&result->number, &exact->number, &flags);
    return sxml_decimal_status_from_flags(flags);
}

static sxml_status sxml_decimal_from_binary(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_binary_request *request, sxml_decimal *decimal) {
    if (!sxml_decimal_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_decimal_binary_parts parts =
        sxml_decimal_split_double(request->double_number);
    if (!parts.is_finite) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_allocator *previous_scope =
        sxml_decimal_enter_scope(configuration);
    sxml_decimal exact;
    sxml_decimal result;
    sxml_status status = sxml_decimal_start(&exact);
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_compute_exact_double(parts, &exact.number);
        if (status == SXML_STATUS_OK && parts.is_negative &&
            parts.significand != 0) {
            mpd_set_sign(&exact.number, MPD_NEG);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_decimal_start(&result);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_decimal_shorten(&exact, request, &result);
            if (status == SXML_STATUS_OK) {
                status = sxml_decimal_check_result(configuration,
                                                   &result.number, 0);
            }
            if (status != SXML_STATUS_OK) {
                sxml_decimal_finalize(&result);
            }
        }
        sxml_decimal_finalize(&exact);
    }
    sxml_decimal_leave_scope(previous_scope);
    if (status == SXML_STATUS_OK) {
        *decimal = result;
    }
    return status;
}

sxml_status sxml_decimal_from_double_exactly(
    const sxml_decimal_configuration *configuration, double double_number,
    sxml_decimal *decimal) {
    return sxml_decimal_from_binary(
        configuration,
        &(sxml_decimal_binary_request){
            .conversion = SXML_DECIMAL_BINARY_CONVERSION_EXACT,
            .double_number = double_number,
        },
        decimal);
}

sxml_status sxml_decimal_from_double_shortest(
    const sxml_decimal_configuration *configuration, double double_number,
    sxml_decimal *decimal) {
    return sxml_decimal_from_binary(
        configuration,
        &(sxml_decimal_binary_request){
            .conversion = SXML_DECIMAL_BINARY_CONVERSION_SHORTEST_DOUBLE,
            .double_number = double_number,
        },
        decimal);
}

sxml_status sxml_decimal_from_float_shortest(
    const sxml_decimal_configuration *configuration, float float_number,
    sxml_decimal *decimal) {
    return sxml_decimal_from_binary(
        configuration,
        &(sxml_decimal_binary_request){
            .conversion = SXML_DECIMAL_BINARY_CONVERSION_SHORTEST_FLOAT,
            .double_number = (double)float_number,
        },
        decimal);
}

/* ---------------------------------------------------------- Arithmetic */

/* The operations sxml_decimal_compute runs. */
typedef enum sxml_decimal_operation {
    SXML_DECIMAL_OPERATION_COPY,
    SXML_DECIMAL_OPERATION_NEGATE,
    SXML_DECIMAL_OPERATION_ABSOLUTE,
    SXML_DECIMAL_OPERATION_ADD,
    SXML_DECIMAL_OPERATION_SUBTRACT,
    SXML_DECIMAL_OPERATION_MULTIPLY,
    SXML_DECIMAL_OPERATION_DIVIDE,
    SXML_DECIMAL_OPERATION_INTEGER_DIVIDE,
    SXML_DECIMAL_OPERATION_MODULO,
} sxml_decimal_operation;

/* One operation; the unary ones read only left_operand and pass it as
   right_operand too, so that neither is ever NULL. */
typedef struct sxml_decimal_operation_request {
    sxml_decimal_operation operation;
    const sxml_decimal *left_operand;
    const sxml_decimal *right_operand;
} sxml_decimal_operation_request;

static bool sxml_decimal_operation_divides(sxml_decimal_operation operation) {
    return operation == SXML_DECIMAL_OPERATION_DIVIDE ||
           operation == SXML_DECIMAL_OPERATION_INTEGER_DIVIDE ||
           operation == SXML_DECIMAL_OPERATION_MODULO;
}

/*
 * The precision of a quotient: division_digit_count, or more when the
 * operands have more digits together, at most maximum_digit_count.
 */
static mpd_ssize_t sxml_decimal_division_precision(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operation_request *request) {
    uint64_t operand_digit_count =
        (uint64_t)request->left_operand->number.digits +
        (uint64_t)request->right_operand->number.digits;
    uint64_t precision = configuration->division_digit_count;
    if (operand_digit_count > precision) {
        precision = operand_digit_count;
    }
    if (precision > configuration->maximum_digit_count) {
        precision = configuration->maximum_digit_count;
    }
    return (mpd_ssize_t)precision;
}

/* Runs request into result (an empty number, in a scope) and returns the
   libmpdec flags. */
static uint32_t sxml_decimal_run_operation(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operation_request *request, mpd_t *result) {
    mpd_context_t context = sxml_decimal_limit_context(configuration);
    uint32_t flags = 0;
    const mpd_t *left = &request->left_operand->number;
    const mpd_t *right = &request->right_operand->number;
    switch (request->operation) {
    case SXML_DECIMAL_OPERATION_COPY:
        (void)mpd_qcopy(result, left, &flags);
        break;
    case SXML_DECIMAL_OPERATION_NEGATE:
        mpd_qminus(result, left, &context, &flags);
        break;
    case SXML_DECIMAL_OPERATION_ABSOLUTE:
        mpd_qabs(result, left, &context, &flags);
        break;
    case SXML_DECIMAL_OPERATION_ADD:
        mpd_qadd(result, left, right, &context, &flags);
        break;
    case SXML_DECIMAL_OPERATION_SUBTRACT:
        mpd_qsub(result, left, right, &context, &flags);
        break;
    case SXML_DECIMAL_OPERATION_MULTIPLY:
        mpd_qmul(result, left, right, &context, &flags);
        break;
    case SXML_DECIMAL_OPERATION_DIVIDE:
        context.prec = sxml_decimal_division_precision(configuration, request);
        mpd_qdiv(result, left, right, &context, &flags);
        /* Rounding a quotient is the configured policy, not a failure. */
        flags &= ~(uint32_t)MPD_Inexact;
        break;
    case SXML_DECIMAL_OPERATION_INTEGER_DIVIDE:
        /* Truncates toward zero (F&O 4.0, op:numeric-integer-divide). */
        mpd_qdivint(result, left, right, &context, &flags);
        break;
    case SXML_DECIMAL_OPERATION_MODULO:
        /* The remainder of truncating division, with the sign of the
           dividend (F&O 4.0, op:numeric-mod). */
        mpd_qrem(result, left, right, &context, &flags);
        break;
    }
    return flags;
}

static sxml_status sxml_decimal_compute(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operation_request *request, sxml_decimal *output) {
    if (!sxml_decimal_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (sxml_decimal_operation_divides(request->operation) &&
        mpd_iszero(&request->right_operand->number)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_allocator *previous_scope =
        sxml_decimal_enter_scope(configuration);
    sxml_decimal result;
    sxml_status status = sxml_decimal_start(&result);
    if (status == SXML_STATUS_OK) {
        uint32_t flags =
            sxml_decimal_run_operation(configuration, request, &result.number);
        /* Every result but a quotient is exact: a rounded one has more
           digits than the limit allows. */
        if ((flags & MPD_Inexact) != 0) {
            flags |= MPD_Overflow;
        }
        status = sxml_decimal_check_result(configuration, &result.number, flags);
        if (status != SXML_STATUS_OK) {
            sxml_decimal_finalize(&result);
        }
    }
    sxml_decimal_leave_scope(previous_scope);
    if (status == SXML_STATUS_OK) {
        *output = result;
    }
    return status;
}

/* Runs a binary operation on operands. */
typedef struct sxml_decimal_binary_operation {
    sxml_decimal_operation operation;
    const sxml_decimal_operands *operands;
} sxml_decimal_binary_operation;

static sxml_status sxml_decimal_compute_binary(
    const sxml_decimal_configuration *configuration,
    sxml_decimal_binary_operation binary_operation, sxml_decimal *output) {
    return sxml_decimal_compute(
        configuration,
        &(sxml_decimal_operation_request){
            .operation = binary_operation.operation,
            .left_operand = binary_operation.operands->left_operand,
            .right_operand = binary_operation.operands->right_operand,
        },
        output);
}

sxml_status sxml_decimal_copy(const sxml_decimal_configuration *configuration,
                              const sxml_decimal *source, sxml_decimal *copy) {
    return sxml_decimal_compute(
        configuration,
        &(sxml_decimal_operation_request){
            .operation = SXML_DECIMAL_OPERATION_COPY,
            .left_operand = source,
            .right_operand = source,
        },
        copy);
}

sxml_status sxml_decimal_negate(const sxml_decimal_configuration *configuration,
                                const sxml_decimal *decimal,
                                sxml_decimal *negation) {
    return sxml_decimal_compute(
        configuration,
        &(sxml_decimal_operation_request){
            .operation = SXML_DECIMAL_OPERATION_NEGATE,
            .left_operand = decimal,
            .right_operand = decimal,
        },
        negation);
}

sxml_status sxml_decimal_absolute(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal *decimal, sxml_decimal *absolute_value) {
    return sxml_decimal_compute(
        configuration,
        &(sxml_decimal_operation_request){
            .operation = SXML_DECIMAL_OPERATION_ABSOLUTE,
            .left_operand = decimal,
            .right_operand = decimal,
        },
        absolute_value);
}

sxml_status sxml_decimal_add(const sxml_decimal_configuration *configuration,
                             const sxml_decimal_operands *operands,
                             sxml_decimal *sum) {
    return sxml_decimal_compute_binary(
        configuration,
        (sxml_decimal_binary_operation){
            .operation = SXML_DECIMAL_OPERATION_ADD, .operands = operands},
        sum);
}

sxml_status sxml_decimal_subtract(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operands *operands, sxml_decimal *difference) {
    return sxml_decimal_compute_binary(
        configuration,
        (sxml_decimal_binary_operation){
            .operation = SXML_DECIMAL_OPERATION_SUBTRACT, .operands = operands},
        difference);
}

sxml_status sxml_decimal_multiply(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operands *operands, sxml_decimal *product) {
    return sxml_decimal_compute_binary(
        configuration,
        (sxml_decimal_binary_operation){
            .operation = SXML_DECIMAL_OPERATION_MULTIPLY, .operands = operands},
        product);
}

sxml_status sxml_decimal_divide(const sxml_decimal_configuration *configuration,
                                const sxml_decimal_operands *operands,
                                sxml_decimal *quotient) {
    return sxml_decimal_compute_binary(
        configuration,
        (sxml_decimal_binary_operation){
            .operation = SXML_DECIMAL_OPERATION_DIVIDE, .operands = operands},
        quotient);
}

sxml_status sxml_decimal_integer_divide(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_operands *operands, sxml_decimal *quotient) {
    return sxml_decimal_compute_binary(
        configuration,
        (sxml_decimal_binary_operation){
            .operation = SXML_DECIMAL_OPERATION_INTEGER_DIVIDE,
            .operands = operands},
        quotient);
}

sxml_status sxml_decimal_modulo(const sxml_decimal_configuration *configuration,
                                const sxml_decimal_operands *operands,
                                sxml_decimal *remainder) {
    return sxml_decimal_compute_binary(
        configuration,
        (sxml_decimal_binary_operation){
            .operation = SXML_DECIMAL_OPERATION_MODULO, .operands = operands},
        remainder);
}

/* --------------------------------------------- Division to a precision */

/*
 * The precision fn:divide-decimals computes with (F&O 4.0, section 4.3.1:
 * a precision outside the supported range "should be adjusted to the
 * nearest value supported"). quotient_integer_digit_count bounds the
 * integer digits of |value / divisor|.
 *
 * Upward: the quotient has at most quotient_integer_digit_count + precision
 * digits and the remainder, below |divisor| * 10^-precision in magnitude
 * and no larger than |value|, at most the canonical digits of the divisor
 * plus precision, or those of the value; one more for "0.". A positive
 * precision that does not fit is lowered until both fit, to 0 at least.
 * Downward: 10^-precision above |value / divisor| already gives the
 * quotient 0, so a lower precision changes nothing.
 */
typedef struct sxml_decimal_precision_bounds {
    int64_t precision;
    int64_t quotient_integer_digit_count;
    int64_t divisor_digit_count;
} sxml_decimal_precision_bounds;

static int64_t sxml_decimal_supported_precision(
    const sxml_decimal_configuration *configuration,
    sxml_decimal_precision_bounds bounds) {
    const int64_t maximum_digit_count = (int64_t)configuration->maximum_digit_count;
    int64_t highest = maximum_digit_count - 1 - bounds.quotient_integer_digit_count;
    const int64_t remainder_highest =
        maximum_digit_count - 1 - bounds.divisor_digit_count;
    if (remainder_highest < highest) {
        highest = remainder_highest;
    }
    if (highest < 0) {
        highest = 0;
    }
    const int64_t lowest = -bounds.quotient_integer_digit_count - 1;
    if (bounds.precision > highest) {
        return highest;
    }
    return bounds.precision < lowest ? lowest : bounds.precision;
}

/*
 * Runs the division in a scope: the divisor's power of ten is shifted by
 * -precision (exact, no digits move), so that the integer quotient and
 * remainder of truncating division (mpd_qdivint, mpd_qrem) are the
 * quotient times 10^precision and the remainder fn:divide-decimals asks
 * for: value - trunc(value / (divisor * 10^-precision)) * divisor *
 * 10^-precision.
 */
static sxml_status sxml_decimal_run_division(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_division_request *request,
    sxml_decimal_division *division) {
    const mpd_t *value = &request->operands->left_operand->number;
    const mpd_t *divisor = &request->operands->right_operand->number;
    int64_t quotient_integer_digit_count = 0;
    if (!mpd_iszero(value)) {
        quotient_integer_digit_count =
            (int64_t)mpd_adjexp(value) - (int64_t)mpd_adjexp(divisor) + 1;
        if (quotient_integer_digit_count < 0) {
            quotient_integer_digit_count = 0;
        }
    }
    const int64_t precision = sxml_decimal_supported_precision(
        configuration,
        (sxml_decimal_precision_bounds){
            .precision = request->precision,
            .quotient_integer_digit_count = quotient_integer_digit_count,
            .divisor_digit_count = (int64_t)sxml_decimal_layout_digit_count(
                sxml_decimal_lay_out(divisor)),
        });
    /* A borrowed view of the divisor with another exponent: read only. */
    mpd_t shifted_divisor = *divisor;
    shifted_divisor.flags =
        (uint8_t)((divisor->flags & ~(uint8_t)(MPD_DATAFLAGS)) | MPD_STATIC |
                  MPD_CONST_DATA);
    shifted_divisor.exp = (mpd_ssize_t)((int64_t)divisor->exp - precision);
    sxml_decimal quotient;
    sxml_status status = sxml_decimal_start(&quotient);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal remainder;
    status = sxml_decimal_start(&remainder);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&quotient);
        return status;
    }
    mpd_context_t context = sxml_decimal_limit_context(configuration);
    uint32_t flags = 0;
    mpd_qdivint(&quotient.number, value, &shifted_divisor, &context, &flags);
    mpd_qrem(&remainder.number, value, &shifted_divisor, &context, &flags);
    /* Both are exact: anything inexact did not fit the limit. */
    if ((flags & MPD_Inexact) != 0) {
        flags |= MPD_Overflow;
    }
    status = sxml_decimal_status_from_flags(flags);
    if (status == SXML_STATUS_OK && !mpd_iszero(&quotient.number)) {
        quotient.number.exp =
            (mpd_ssize_t)((int64_t)quotient.number.exp - precision);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_check_result(configuration, &quotient.number, 0);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_check_result(configuration, &remainder.number, 0);
    }
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&quotient);
        sxml_decimal_finalize(&remainder);
        return status;
    }
    division->quotient = quotient;
    division->remainder = remainder;
    return SXML_STATUS_OK;
}

sxml_status sxml_decimal_divide_to_precision(
    const sxml_decimal_configuration *configuration,
    const sxml_decimal_division_request *request,
    sxml_decimal_division *division) {
    if (!sxml_decimal_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (mpd_iszero(&request->operands->right_operand->number)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_allocator *previous_scope =
        sxml_decimal_enter_scope(configuration);
    sxml_decimal_division result;
    sxml_status status =
        sxml_decimal_run_division(configuration, request, &result);
    sxml_decimal_leave_scope(previous_scope);
    if (status == SXML_STATUS_OK) {
        *division = result;
    }
    return status;
}

/* ------------------------------------------------------------ Rounding */

/* The libmpdec rounding of mode for a value of the given sign (fn:round:
   the half-to-floor and half-to-ceiling modes depend on the sign). */
typedef struct sxml_decimal_signed_mode {
    sxml_rounding_mode rounding_mode;
    bool is_negative;
} sxml_decimal_signed_mode;

static int sxml_decimal_libmpdec_rounding(sxml_decimal_signed_mode mode) {
    switch (mode.rounding_mode) {
    case SXML_ROUNDING_MODE_FLOOR:
        return MPD_ROUND_FLOOR;
    case SXML_ROUNDING_MODE_CEILING:
        return MPD_ROUND_CEILING;
    case SXML_ROUNDING_MODE_TOWARD_ZERO:
        return MPD_ROUND_DOWN;
    case SXML_ROUNDING_MODE_AWAY_FROM_ZERO:
        return MPD_ROUND_UP;
    case SXML_ROUNDING_MODE_HALF_TO_FLOOR:
        return mode.is_negative ? MPD_ROUND_HALF_UP : MPD_ROUND_HALF_DOWN;
    case SXML_ROUNDING_MODE_HALF_TO_CEILING:
        return mode.is_negative ? MPD_ROUND_HALF_DOWN : MPD_ROUND_HALF_UP;
    case SXML_ROUNDING_MODE_HALF_TOWARD_ZERO:
        return MPD_ROUND_HALF_DOWN;
    case SXML_ROUNDING_MODE_HALF_AWAY_FROM_ZERO:
        return MPD_ROUND_HALF_UP;
    case SXML_ROUNDING_MODE_HALF_TO_EVEN:
        return MPD_ROUND_HALF_EVEN;
    }
    return -1;
}

sxml_status sxml_decimal_round(const sxml_decimal_configuration *configuration,
                               const sxml_decimal_rounding *rounding,
                               sxml_decimal *rounded) {
    if (!sxml_decimal_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    int rounding_rule = sxml_decimal_libmpdec_rounding(
        (sxml_decimal_signed_mode){
            .rounding_mode = rounding->rounding_mode,
            .is_negative = sxml_decimal_is_negative(rounding->decimal),
        });
    if (rounding_rule < 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const mpd_t *value = &rounding->decimal->number;
    sxml_decimal_layout layout = sxml_decimal_lay_out(value);
    int64_t fraction_digit_count = layout.exponent < 0 ? -layout.exponent : 0;
    if (rounding->precision >= fraction_digit_count) {
        /* Nothing to round (fn:round returns the value unchanged). */
        return sxml_decimal_copy(configuration, rounding->decimal, rounded);
    }
    /* Every value has fewer than maximum_digit_count + 1 integer digits,
       so rounding to a higher power of ten gives the same result. */
    int64_t lowest_precision = -(int64_t)configuration->maximum_digit_count - 1;
    int64_t precision = rounding->precision < lowest_precision
                            ? lowest_precision
                            : rounding->precision;
    const sxml_allocator *previous_scope =
        sxml_decimal_enter_scope(configuration);
    sxml_decimal result;
    sxml_status status = sxml_decimal_start(&result);
    if (status == SXML_STATUS_OK) {
        mpd_context_t context = sxml_decimal_limit_context(configuration);
        context.round = rounding_rule;
        uint32_t flags = 0;
        mpd_qrescale(&result.number, value, (mpd_ssize_t)(-precision), &context,
                     &flags);
        /* Rounding may be inexact; a result with more digits than the
           precision is reported as invalid by libmpdec. */
        if ((flags & MPD_Invalid_operation) != 0) {
            flags = (flags & ~(uint32_t)MPD_Invalid_operation) | MPD_Overflow;
        }
        status = sxml_decimal_check_result(configuration, &result.number, flags);
        if (status != SXML_STATUS_OK) {
            sxml_decimal_finalize(&result);
        }
    }
    sxml_decimal_leave_scope(previous_scope);
    if (status == SXML_STATUS_OK) {
        *rounded = result;
    }
    return status;
}

/* ---------------------------------------------------------- Predicates */

sxml_ordering sxml_decimal_compare(const sxml_decimal *first,
                                   const sxml_decimal *second) {
    uint32_t flags = 0;
    /* Comparing finite numbers allocates nothing. */
    int comparison = mpd_qcmp(&first->number, &second->number, &flags);
    if (comparison < 0) {
        return SXML_ORDERING_LESS;
    }
    return comparison == 0 ? SXML_ORDERING_EQUAL : SXML_ORDERING_GREATER;
}

bool sxml_decimal_is_zero(const sxml_decimal *decimal) {
    return mpd_iszero(&decimal->number) != 0;
}

bool sxml_decimal_is_negative(const sxml_decimal *decimal) {
    return mpd_isnegative(&decimal->number) != 0 &&
           mpd_iszero(&decimal->number) == 0;
}

bool sxml_decimal_is_integer(const sxml_decimal *decimal) {
    return mpd_isinteger(&decimal->number) != 0;
}
