/*
 * Lexical forms and canonical text (src/cast_internal.h): parsing the
 * lexical form of every primitive type into an atomic item, and writing the
 * canonical text of every atomic item.
 *
 * Golden file: src/memory.c.
 *
 * Parsing checks syntax only; the value space (days of the month, ranges
 * of the integer types, the patterns of xs:NCName and the other string
 * types) is checked by sxml_atomic_item_make, whose refusal becomes
 * FORG0001 here. Numbers that do not fit their C form are FODT0001,
 * FODT0002, FOCA0001 or FOCA0003, never wrapped.
 *
 * Specification: XSD 1.1 Part 2, section 3.3 (lexical spaces of the
 * primitive types) and appendix D (date/time lexical forms); F&O 4.0,
 * section 19.1.2 "Casting to xs:string and xs:untypedAtomic" (canonical
 * text, with the XPath rules for xs:float and xs:double) and section 19.2
 * "Casting from xs:string and xs:untypedAtomic".
 */
#include "cast_internal.h"

#include "sxml/name.h"
#include "sxml/string.h"
#include "xml_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- Errors */

static const sxml_static_error sxml_cast_lexical_form_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the value is not in the lexical space or value space of the target "
        "type"),
};

static const sxml_static_error sxml_cast_date_time_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODT0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the year is beyond the range this implementation supports"),
};

static const sxml_static_error sxml_cast_duration_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODT0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the duration is beyond the range this implementation supports"),
};

static const sxml_static_error sxml_cast_decimal_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCA0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the decimal has more digits than this implementation supports"),
};

static const sxml_static_error sxml_cast_integer_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCA0003"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the integer has more digits than this implementation supports"),
};

static const sxml_static_error sxml_cast_unbound_prefix_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FONS0004"),
    .description =
        SXML_BYTE_VIEW_INITIALIZER("no namespace is bound to the prefix"),
};

/* Reports error and returns its status. */
static sxml_status sxml_lexical_fail(const sxml_lexical_form_request *request,
                                     const sxml_static_error *error) {
    return sxml_error_record_report(request->error_record, error);
}

/* A parsed datum and the request it was parsed for. */
typedef struct sxml_parsed_datum {
    const sxml_lexical_form_request *request;
    sxml_atomic_datum datum;
} sxml_parsed_datum;

/* Makes the item; a datum outside the value space is FORG0001. */
static sxml_status sxml_lexical_make(sxml_item_heap *item_heap,
                                     sxml_parsed_datum parsed, sxml_item *item);

/* ------------------------------------------------------------- Cursors */

/* A position in a lexical form. */
typedef struct sxml_text_cursor {
    sxml_byte_view text;
    size_t byte_offset;
} sxml_text_cursor;

static bool sxml_cursor_is_at_end(const sxml_text_cursor *cursor) {
    return cursor->byte_offset == cursor->text.byte_count;
}

/* The next byte, or 0 at the end (0 never occurs in a lexical form we
   accept, so it never matches an expected byte). */
static uint8_t sxml_cursor_peek(const sxml_text_cursor *cursor) {
    return sxml_cursor_is_at_end(cursor)
               ? 0
               : cursor->text.bytes[cursor->byte_offset];
}

/* Consumes byte when it is next. */
static bool sxml_cursor_accept(sxml_text_cursor *cursor, uint8_t byte) {
    if (byte != 0 && sxml_cursor_peek(cursor) == byte) {
        cursor->byte_offset += 1;
        return true;
    }
    return false;
}

static bool sxml_is_decimal_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

/* The bytes of text from byte_offset on. The empty text may have no bytes
   at all (NULL), and a null pointer takes no offset, not even 0. */
static const uint8_t *sxml_text_bytes_at(sxml_byte_view text,
                                         size_t byte_offset) {
    return text.bytes == NULL ? NULL : text.bytes + byte_offset;
}

/* Consumes a run of decimal digits, possibly empty, and returns it. */
static sxml_byte_view sxml_cursor_read_digits(sxml_text_cursor *cursor) {
    size_t first_offset = cursor->byte_offset;
    while (sxml_is_decimal_digit(sxml_cursor_peek(cursor))) {
        cursor->byte_offset += 1;
    }
    return (sxml_byte_view){
        .bytes = sxml_text_bytes_at(cursor->text, first_offset),
        .byte_count = cursor->byte_offset - first_offset};
}

/* The value of digits, or false when it exceeds maximum. */
typedef struct sxml_digit_value_request {
    sxml_byte_view digits;
    uint64_t maximum;
} sxml_digit_value_request;

static bool sxml_digits_to_unsigned(sxml_digit_value_request request,
                                    uint64_t *value) {
    uint64_t accumulated = 0;
    for (size_t byte_index = 0; byte_index < request.digits.byte_count;
         byte_index += 1) {
        uint64_t digit = (uint64_t)(request.digits.bytes[byte_index] - '0');
        if (accumulated > (request.maximum - digit) / 10U) {
            return false;
        }
        accumulated = accumulated * 10U + digit;
    }
    *value = accumulated;
    return true;
}

/* A number of two digits, from 0 to 99. */
typedef struct sxml_two_digits {
    uint8_t value;
} sxml_two_digits;

/* Reads exactly two digits. */
static bool sxml_cursor_read_two_digits(sxml_text_cursor *cursor,
                                        sxml_two_digits *two_digits) {
    sxml_byte_view digits = sxml_cursor_read_digits(cursor);
    if (digits.byte_count != 2) {
        return false;
    }
    two_digits->value =
        (uint8_t)((digits.bytes[0] - '0') * 10 + (digits.bytes[1] - '0'));
    return true;
}

/* ------------------------------------------------------------- Booleans */

/* XSD 1.1 Part 2, section 3.3.2: "true", "false", "1", "0". */
static sxml_status sxml_parse_boolean(sxml_item_heap *item_heap,
                                      const sxml_lexical_form_request *request,
                                      sxml_item *item) {
    sxml_byte_view text = request->lexical_form;
    bool value = false;
    if (sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("true")) ||
        sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("1"))) {
        value = true;
    } else if (!sxml_byte_view_equals(text,
                                      SXML_BYTE_VIEW_FROM_LITERAL("false")) &&
               !sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("0"))) {
        return sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
    }
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN,
                               .datum_details = {.boolean = value}};
    return sxml_lexical_make(
        item_heap, (sxml_parsed_datum){.request = request, .datum = datum},
        item);
}

/* ------------------------------------------------- Decimals and integers */

static bool sxml_target_is_integer(sxml_atomic_target target) {
    return target.builtin_type >= SXML_TYPE_XS_INTEGER &&
           target.builtin_type <= SXML_TYPE_XS_POSITIVE_INTEGER;
}

/*
 * xs:decimal: (\+|-)?([0-9]+(\.[0-9]*)?|\.[0-9]+) (XSD 1.1 Part 2, section
 * 3.3.3); the integer types: (\+|-)?[0-9]+ (section 3.4.13).
 */
static sxml_status sxml_parse_decimal(sxml_item_heap *item_heap,
                                      const sxml_lexical_form_request *request,
                                      sxml_item *item) {
    bool is_integer_type = sxml_target_is_integer(request->target);
    sxml_text_cursor cursor = {.text = request->lexical_form, .byte_offset = 0};
    bool is_negative = sxml_cursor_accept(&cursor, '-');
    if (!is_negative) {
        (void)sxml_cursor_accept(&cursor, '+');
    }
    sxml_byte_view integer_digits = sxml_cursor_read_digits(&cursor);
    sxml_byte_view fraction_digits = {.bytes = NULL, .byte_count = 0};
    bool has_point = !is_integer_type && sxml_cursor_accept(&cursor, '.');
    if (has_point) {
        fraction_digits = sxml_cursor_read_digits(&cursor);
    }
    if (!sxml_cursor_is_at_end(&cursor) ||
        integer_digits.byte_count + fraction_digits.byte_count == 0) {
        return sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
    }
    if (is_integer_type) {
        /* The common case without libmpdec: the magnitude fits int64_t
           (INT64_MIN included). */
        uint64_t magnitude = 0;
        uint64_t maximum =
            is_negative ? (uint64_t)INT64_MAX + 1U : (uint64_t)INT64_MAX;
        if (sxml_digits_to_unsigned(
                (sxml_digit_value_request){.digits = integer_digits,
                                           .maximum = maximum},
                &magnitude)) {
            int64_t integer = 0;
            if (is_negative) {
                integer = magnitude == (uint64_t)INT64_MAX + 1U
                              ? INT64_MIN
                              : -(int64_t)magnitude;
            } else {
                integer = (int64_t)magnitude;
            }
            sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                                       .datum_details = {.integer = integer}};
            return sxml_lexical_make(
                item_heap,
                (sxml_parsed_datum){.request = request, .datum = datum}, item);
        }
    }
    sxml_decimal decimal;
    sxml_status status =
        sxml_decimal_parse(sxml_item_heap_get_decimal_configuration(item_heap),
                           request->lexical_form, &decimal);
    if (status == SXML_STATUS_MALFORMED_INPUT) {
        return sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
    }
    if (status == SXML_STATUS_LIMIT_EXCEEDED) {
        return sxml_lexical_fail(
            request, is_integer_type ? &sxml_cast_integer_overflow_error
                                     : &sxml_cast_decimal_overflow_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_atomic_item_make_taking_decimal(
        item_heap,
        &(sxml_decimal_item_request){.type = request->target.type,
                                     .decimal = &decimal},
        item);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&decimal);
        if (status == SXML_STATUS_INVALID_ARGUMENT) {
            return sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
        }
    }
    return status;
}

/* ------------------------------------------------- Binary floating point */

/*
 * Significant digits handed to strtod or strtof: as for decimals (D57),
 * the first 780 digits and a sticky digit for a nonzero rest round as all
 * digits would.
 */
#define SXML_CAST_CONVERSION_DIGIT_COUNT 780U

/* Sign, digits, sticky digit, "e", exponent sign and digits, terminator. */
#define SXML_CAST_CONVERSION_TEXT_CAPACITY                                     \
    (SXML_CAST_CONVERSION_DIGIT_COUNT + 32U)

/*
 * Exponents are clamped to this magnitude: beyond it every value is 0 or
 * an infinity in both binary formats, and the arithmetic stays in int64_t.
 */
static const int64_t sxml_cast_exponent_clamp = 100000;

/* What a float or double lexical form says. */
typedef struct sxml_binary_lexical_form {
    bool is_negative;
    bool is_nan;
    bool is_infinite;
    /* The mantissa digits without the point, and the power of ten of the
       last one. */
    sxml_byte_view integer_digits;
    sxml_byte_view fraction_digits;
    int64_t exponent;
} sxml_binary_lexical_form;

/*
 * (\+|-)?([0-9]+(\.[0-9]*)?|\.[0-9]+)([Ee](\+|-)?[0-9]+)?|(\+|-)?INF|NaN
 * (XSD 1.1 Part 2, sections 3.3.4 and 3.3.5).
 */
static bool sxml_split_binary_lexical_form(sxml_byte_view text,
                                           sxml_binary_lexical_form *form) {
    *form = (sxml_binary_lexical_form){.is_negative = false,
                                       .is_nan = false,
                                       .is_infinite = false,
                                       .integer_digits = {NULL, 0},
                                       .fraction_digits = {NULL, 0},
                                       .exponent = 0};
    if (sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("NaN"))) {
        form->is_nan = true;
        return true;
    }
    sxml_text_cursor cursor = {.text = text, .byte_offset = 0};
    form->is_negative = sxml_cursor_accept(&cursor, '-');
    if (!form->is_negative) {
        (void)sxml_cursor_accept(&cursor, '+');
    }
    sxml_byte_view rest = {.bytes = sxml_text_bytes_at(text, cursor.byte_offset),
                           .byte_count = text.byte_count - cursor.byte_offset};
    if (sxml_byte_view_equals(rest, SXML_BYTE_VIEW_FROM_LITERAL("INF"))) {
        form->is_infinite = true;
        return true;
    }
    form->integer_digits = sxml_cursor_read_digits(&cursor);
    if (sxml_cursor_accept(&cursor, '.')) {
        form->fraction_digits = sxml_cursor_read_digits(&cursor);
    }
    if (form->integer_digits.byte_count + form->fraction_digits.byte_count ==
        0) {
        return false;
    }
    if (sxml_cursor_accept(&cursor, 'e') || sxml_cursor_accept(&cursor, 'E')) {
        bool is_negative_exponent = sxml_cursor_accept(&cursor, '-');
        if (!is_negative_exponent) {
            (void)sxml_cursor_accept(&cursor, '+');
        }
        sxml_byte_view exponent_digits = sxml_cursor_read_digits(&cursor);
        if (exponent_digits.byte_count == 0) {
            return false;
        }
        uint64_t magnitude = 0;
        if (!sxml_digits_to_unsigned(
                (sxml_digit_value_request){
                    .digits = exponent_digits,
                    .maximum = (uint64_t)sxml_cast_exponent_clamp},
                &magnitude)) {
            magnitude = (uint64_t)sxml_cast_exponent_clamp;
        }
        form->exponent =
            is_negative_exponent ? -(int64_t)magnitude : (int64_t)magnitude;
    }
    return sxml_cursor_is_at_end(&cursor);
}

/* A text sink for conversion text, bounded by its capacity. */
typedef struct sxml_conversion_text {
    uint8_t bytes[SXML_CAST_CONVERSION_TEXT_CAPACITY];
    size_t byte_count;
} sxml_conversion_text;

static void sxml_conversion_text_put(sxml_conversion_text *text, uint8_t byte) {
    if (text->byte_count < sizeof(text->bytes)) {
        text->bytes[text->byte_count] = byte;
        text->byte_count += 1;
    }
}

/*
 * Writes the form as "[-]digitse[-]exponent" with a terminator: no decimal
 * point, so the text reads the same in every C locale (as D57). Returns
 * false when the mantissa is zero.
 */
static bool
sxml_write_binary_conversion_text(const sxml_binary_lexical_form *form,
                                  sxml_conversion_text *text) {
    text->byte_count = 0;
    if (form->is_negative) {
        sxml_conversion_text_put(text, '-');
    }
    int64_t exponent = form->exponent;
    if (exponent < -sxml_cast_exponent_clamp) {
        exponent = -sxml_cast_exponent_clamp;
    }
    size_t written_digit_count = 0;
    bool has_nonzero_rest = false;
    bool is_leading = true;
    for (size_t part_index = 0; part_index < 2; part_index += 1) {
        sxml_byte_view digits =
            part_index == 0 ? form->integer_digits : form->fraction_digits;
        for (size_t byte_index = 0; byte_index < digits.byte_count;
             byte_index += 1) {
            uint8_t digit = digits.bytes[byte_index];
            bool is_fraction_digit = part_index == 1;
            if (is_leading && digit == '0') {
                if (is_fraction_digit) {
                    exponent -= 1;
                }
                continue;
            }
            is_leading = false;
            if (written_digit_count < SXML_CAST_CONVERSION_DIGIT_COUNT) {
                sxml_conversion_text_put(text, digit);
                written_digit_count += 1;
                if (is_fraction_digit) {
                    exponent -= 1;
                }
            } else {
                has_nonzero_rest = has_nonzero_rest || digit != '0';
                if (!is_fraction_digit) {
                    exponent += 1;
                }
            }
            if (exponent < -2 * sxml_cast_exponent_clamp) {
                exponent = -2 * sxml_cast_exponent_clamp;
            }
        }
    }
    if (written_digit_count == 0) {
        return false;
    }
    if (has_nonzero_rest) {
        sxml_conversion_text_put(text, '1');
        exponent -= 1;
    }
    sxml_conversion_text_put(text, 'e');
    uint64_t exponent_magnitude = 0;
    if (exponent < 0) {
        sxml_conversion_text_put(text, '-');
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
        sxml_conversion_text_put(text, reversed_digits[reversed_digit_count]);
    }
    sxml_conversion_text_put(text, 0);
    return true;
}

static sxml_status
sxml_parse_binary_number(sxml_item_heap *item_heap,
                         const sxml_lexical_form_request *request,
                         sxml_item *item) {
    sxml_binary_lexical_form form;
    if (!sxml_split_binary_lexical_form(request->lexical_form, &form)) {
        return sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
    }
    double double_number = 0.0;
    float float_number = 0.0F;
    bool is_float = request->target.primitive_type == SXML_TYPE_XS_FLOAT;
    if (form.is_nan) {
        double_number = (double)NAN;
        float_number = NAN;
    } else if (form.is_infinite) {
        double_number = form.is_negative ? -(double)INFINITY : (double)INFINITY;
        float_number = form.is_negative ? -INFINITY : INFINITY;
    } else {
        sxml_conversion_text text;
        if (sxml_write_binary_conversion_text(&form, &text)) {
            /* strtod and strtof round correctly to nearest, give an
               infinity beyond the range and 0 below it; errno is not read.
               strtof rounds once, not through double. */
            if (is_float) {
                float_number = strtof((const char *)text.bytes, NULL);
            } else {
                double_number = strtod((const char *)text.bytes, NULL);
            }
        } else {
            double_number = form.is_negative ? -0.0 : 0.0;
            float_number = form.is_negative ? -0.0F : 0.0F;
        }
    }
    sxml_atomic_datum datum =
        is_float ? (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_FLOAT,
                                       .datum_details = {.float_number =
                                                             float_number}}
                 : (sxml_atomic_datum){
                       .datum_kind = SXML_DATUM_KIND_DOUBLE,
                       .datum_details = {.double_number = double_number}};
    return sxml_lexical_make(
        item_heap, (sxml_parsed_datum){.request = request, .datum = datum},
        item);
}

/* ------------------------------------------------------------ Durations */

/* Seconds per day, hour and minute. */
static const uint64_t sxml_seconds_per_day = 86400U;
static const uint64_t sxml_seconds_per_hour = 3600U;
static const uint64_t sxml_seconds_per_minute = 60U;

/* accumulated + value * factor within INT64_MAX. */
typedef struct sxml_checked_sum {
    uint64_t accumulated;
    uint64_t value;
    uint64_t factor;
} sxml_checked_sum;

static bool sxml_checked_sum_compute(sxml_checked_sum sum, uint64_t *total) {
    uint64_t limit = (uint64_t)INT64_MAX;
    if (sum.factor != 0 && sum.value > limit / sum.factor) {
        return false;
    }
    uint64_t product = sum.value * sum.factor;
    if (product > limit - sum.accumulated) {
        return false;
    }
    *total = sum.accumulated + product;
    return true;
}

/* Reads up to nine fraction digits as nanoseconds; more are truncated
   (D36). Returns false for an empty fraction. */
static bool sxml_cursor_read_nanoseconds(sxml_text_cursor *cursor,
                                         uint32_t *nanoseconds) {
    sxml_byte_view digits = sxml_cursor_read_digits(cursor);
    if (digits.byte_count == 0) {
        return false;
    }
    uint32_t value = 0;
    for (size_t digit_index = 0; digit_index < 9; digit_index += 1) {
        uint32_t digit = digit_index < digits.byte_count
                             ? (uint32_t)(digits.bytes[digit_index] - '0')
                             : 0U;
        value = value * 10U + digit;
    }
    *nanoseconds = value;
    return true;
}

/* The designators of a duration, in the order they must appear. */
typedef enum sxml_duration_designator {
    SXML_DURATION_DESIGNATOR_YEAR,
    SXML_DURATION_DESIGNATOR_MONTH,
    SXML_DURATION_DESIGNATOR_DAY,
    SXML_DURATION_DESIGNATOR_HOUR,
    SXML_DURATION_DESIGNATOR_MINUTE,
    SXML_DURATION_DESIGNATOR_SECOND,
    SXML_DURATION_DESIGNATOR_COUNT,
} sxml_duration_designator;

/* What a duration lexical form gives, before it is checked. */
typedef struct sxml_duration_parts {
    bool is_negative;
    uint64_t values[SXML_DURATION_DESIGNATOR_COUNT];
    bool is_present[SXML_DURATION_DESIGNATOR_COUNT];
    uint32_t nanoseconds;
} sxml_duration_parts;

/* The outcome of splitting a duration form. */
typedef enum sxml_duration_split {
    SXML_DURATION_SPLIT_OK,
    SXML_DURATION_SPLIT_MALFORMED,
    SXML_DURATION_SPLIT_OVERFLOW,
} sxml_duration_split;

/* A designator letter and whether it follows the "T". */
typedef struct sxml_designator_lookup {
    uint8_t byte;
    bool is_time_part;
} sxml_designator_lookup;

static bool sxml_designator_of(sxml_designator_lookup lookup,
                               sxml_duration_designator *designator);

/*
 * -?P(nY)?(nM)?(nD)?(T(nH)?(nM)?(n(.n)?S)?)? with at least one component,
 * and at least one after T (XSD 1.1 Part 2, section 3.3.6).
 */
static sxml_duration_split sxml_split_duration(sxml_byte_view text,
                                               sxml_duration_parts *parts) {
    *parts = (sxml_duration_parts){.is_negative = false, .nanoseconds = 0};
    sxml_text_cursor cursor = {.text = text, .byte_offset = 0};
    parts->is_negative = sxml_cursor_accept(&cursor, '-');
    if (!sxml_cursor_accept(&cursor, 'P')) {
        return SXML_DURATION_SPLIT_MALFORMED;
    }
    bool is_time_part = false;
    bool has_time_component = false;
    bool has_component = false;
    bool has_overflow = false;
    size_t next_designator = 0;
    while (!sxml_cursor_is_at_end(&cursor)) {
        if (sxml_cursor_accept(&cursor, 'T')) {
            if (is_time_part) {
                return SXML_DURATION_SPLIT_MALFORMED;
            }
            is_time_part = true;
            next_designator = SXML_DURATION_DESIGNATOR_HOUR;
            continue;
        }
        sxml_byte_view digits = sxml_cursor_read_digits(&cursor);
        if (digits.byte_count == 0) {
            return SXML_DURATION_SPLIT_MALFORMED;
        }
        uint32_t nanoseconds = 0;
        bool has_fraction = sxml_cursor_accept(&cursor, '.');
        if (has_fraction &&
            !sxml_cursor_read_nanoseconds(&cursor, &nanoseconds)) {
            return SXML_DURATION_SPLIT_MALFORMED;
        }
        sxml_duration_designator designator = SXML_DURATION_DESIGNATOR_COUNT;
        if (!sxml_designator_of(
                (sxml_designator_lookup){.byte = sxml_cursor_peek(&cursor),
                                         .is_time_part = is_time_part},
                &designator) ||
            (size_t)designator < next_designator ||
            (has_fraction && designator != SXML_DURATION_DESIGNATOR_SECOND)) {
            return SXML_DURATION_SPLIT_MALFORMED;
        }
        cursor.byte_offset += 1;
        uint64_t value = 0;
        if (!sxml_digits_to_unsigned(
                (sxml_digit_value_request){.digits = digits,
                                           .maximum = (uint64_t)INT64_MAX},
                &value)) {
            has_overflow = true;
        }
        parts->values[designator] = value;
        parts->is_present[designator] = true;
        if (designator == SXML_DURATION_DESIGNATOR_SECOND) {
            parts->nanoseconds = nanoseconds;
        }
        next_designator = (size_t)designator + 1U;
        has_component = true;
        has_time_component = has_time_component || is_time_part;
    }
    if (!has_component || (is_time_part && !has_time_component)) {
        return SXML_DURATION_SPLIT_MALFORMED;
    }
    return has_overflow ? SXML_DURATION_SPLIT_OVERFLOW : SXML_DURATION_SPLIT_OK;
}

static bool sxml_designator_of(sxml_designator_lookup lookup,
                               sxml_duration_designator *designator) {
    bool is_time_part = lookup.is_time_part;
    switch (lookup.byte) {
    case 'Y':
        *designator = SXML_DURATION_DESIGNATOR_YEAR;
        return !is_time_part;
    case 'M':
        *designator = is_time_part ? SXML_DURATION_DESIGNATOR_MINUTE
                                   : SXML_DURATION_DESIGNATOR_MONTH;
        return true;
    case 'D':
        *designator = SXML_DURATION_DESIGNATOR_DAY;
        return !is_time_part;
    case 'H':
        *designator = SXML_DURATION_DESIGNATOR_HOUR;
        return is_time_part;
    case 'S':
        *designator = SXML_DURATION_DESIGNATOR_SECOND;
        return is_time_part;
    default:
        return false;
    }
}

static sxml_status sxml_parse_duration(sxml_item_heap *item_heap,
                                       const sxml_lexical_form_request *request,
                                       sxml_item *item) {
    sxml_duration_parts parts;
    sxml_duration_split split =
        sxml_split_duration(request->lexical_form, &parts);
    bool has_year_month = parts.is_present[SXML_DURATION_DESIGNATOR_YEAR] ||
                          parts.is_present[SXML_DURATION_DESIGNATOR_MONTH];
    bool has_day_time = parts.is_present[SXML_DURATION_DESIGNATOR_DAY] ||
                        parts.is_present[SXML_DURATION_DESIGNATOR_HOUR] ||
                        parts.is_present[SXML_DURATION_DESIGNATOR_MINUTE] ||
                        parts.is_present[SXML_DURATION_DESIGNATOR_SECOND];
    /* The lexical spaces of the two derived types allow only their own
       components (XSD 1.1 Part 2, sections 3.4.26 and 3.4.27). */
    if (split == SXML_DURATION_SPLIT_MALFORMED ||
        (request->target.builtin_type == SXML_TYPE_XS_DAY_TIME_DURATION &&
         has_year_month) ||
        (request->target.builtin_type == SXML_TYPE_XS_YEAR_MONTH_DURATION &&
         has_day_time)) {
        return sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
    }
    uint64_t months = 0;
    uint64_t seconds = 0;
    bool fits =
        split == SXML_DURATION_SPLIT_OK &&
        sxml_checked_sum_compute(
            (sxml_checked_sum){
                .accumulated = parts.values[SXML_DURATION_DESIGNATOR_MONTH],
                .value = parts.values[SXML_DURATION_DESIGNATOR_YEAR],
                .factor = 12U},
            &months) &&
        sxml_checked_sum_compute(
            (sxml_checked_sum){
                .accumulated = parts.values[SXML_DURATION_DESIGNATOR_SECOND],
                .value = parts.values[SXML_DURATION_DESIGNATOR_DAY],
                .factor = sxml_seconds_per_day},
            &seconds) &&
        sxml_checked_sum_compute(
            (sxml_checked_sum){.accumulated = seconds,
                               .value =
                                   parts.values[SXML_DURATION_DESIGNATOR_HOUR],
                               .factor = sxml_seconds_per_hour},
            &seconds) &&
        sxml_checked_sum_compute(
            (sxml_checked_sum){
                .accumulated = seconds,
                .value = parts.values[SXML_DURATION_DESIGNATOR_MINUTE],
                .factor = sxml_seconds_per_minute},
            &seconds);
    if (!fits) {
        return sxml_lexical_fail(request, &sxml_cast_duration_overflow_error);
    }
    int64_t sign = parts.is_negative ? -1 : 1;
    sxml_atomic_datum datum = {
        .datum_kind = SXML_DATUM_KIND_DURATION,
        .datum_details = {
            .duration = {
                .months = sign * (int64_t)months,
                .seconds = sign * (int64_t)seconds,
                .nanoseconds = (int32_t)sign * (int32_t)parts.nanoseconds,
            }}};
    return sxml_lexical_make(
        item_heap, (sxml_parsed_datum){.request = request, .datum = datum},
        item);
}

/* ----------------------------------------------------------- Date/time */

/* Which parts a date/time lexical form has (XSD 1.1 Part 2, appendix D). */
typedef struct sxml_date_time_layout {
    bool has_year;
    /* "--" before the month of xs:gMonthDay and xs:gMonth, "---" before
       the day of xs:gDay. */
    bool has_month;
    bool has_day;
    bool has_time;
} sxml_date_time_layout;

static sxml_date_time_layout
sxml_date_time_layout_of(sxml_builtin_type primitive_type) {
    switch ((uint32_t)primitive_type) {
    case SXML_TYPE_XS_DATE_TIME:
        return (sxml_date_time_layout){true, true, true, true};
    case SXML_TYPE_XS_DATE:
        return (sxml_date_time_layout){true, true, true, false};
    case SXML_TYPE_XS_TIME:
        return (sxml_date_time_layout){false, false, false, true};
    case SXML_TYPE_XS_G_YEAR_MONTH:
        return (sxml_date_time_layout){true, true, false, false};
    case SXML_TYPE_XS_G_YEAR:
        return (sxml_date_time_layout){true, false, false, false};
    case SXML_TYPE_XS_G_MONTH_DAY:
        return (sxml_date_time_layout){false, true, true, false};
    case SXML_TYPE_XS_G_DAY:
        return (sxml_date_time_layout){false, false, true, false};
    case SXML_TYPE_XS_G_MONTH:
        return (sxml_date_time_layout){false, true, false, false};
    default:
        return (sxml_date_time_layout){false, false, false, false};
    }
}

/* The outcome of reading a date/time form. */
typedef enum sxml_date_time_split {
    SXML_DATE_TIME_SPLIT_OK,
    SXML_DATE_TIME_SPLIT_MALFORMED,
    SXML_DATE_TIME_SPLIT_OVERFLOW,
} sxml_date_time_split;

/* Years with more digits than this are beyond int64_t's comfortable
   range (the year arithmetic of date/time operators adds to it). */
enum { SXML_CAST_MAXIMUM_YEAR_DIGIT_COUNT = 16 };

/*
 * yearFrag: -?(([1-9][0-9]{3,})|(0[0-9]{3})) (XSD 1.1 Part 2, D.3.1):
 * "-0000" is in the lexical space and is year zero (yearFragValue; QT4
 * cbcl-cast-gYearMonth-004a, XSD saxon zone202).
 */
static sxml_date_time_split sxml_cursor_read_year(sxml_text_cursor *cursor,
                                                  int64_t *year) {
    bool is_negative = sxml_cursor_accept(cursor, '-');
    sxml_byte_view digits = sxml_cursor_read_digits(cursor);
    if (digits.byte_count < 4 ||
        (digits.byte_count > 4 && digits.bytes[0] == '0')) {
        return SXML_DATE_TIME_SPLIT_MALFORMED;
    }
    if (digits.byte_count > SXML_CAST_MAXIMUM_YEAR_DIGIT_COUNT) {
        return SXML_DATE_TIME_SPLIT_OVERFLOW;
    }
    uint64_t magnitude = 0;
    (void)sxml_digits_to_unsigned(
        (sxml_digit_value_request){.digits = digits,
                                   .maximum = (uint64_t)INT64_MAX},
        &magnitude);
    *year = is_negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return SXML_DATE_TIME_SPLIT_OK;
}

/* hh:mm:ss(.s+)? with 24:00:00 allowed (XSD 1.1 Part 2, D.3.2 to D.3.5). */
static bool sxml_cursor_read_time(sxml_text_cursor *cursor,
                                  sxml_date_time *date_time) {
    sxml_two_digits hour = {.value = 0};
    sxml_two_digits minute = {.value = 0};
    sxml_two_digits second = {.value = 0};
    if (!sxml_cursor_read_two_digits(cursor, &hour) ||
        !sxml_cursor_accept(cursor, ':') ||
        !sxml_cursor_read_two_digits(cursor, &minute) ||
        !sxml_cursor_accept(cursor, ':') ||
        !sxml_cursor_read_two_digits(cursor, &second)) {
        return false;
    }
    date_time->hour = hour.value;
    date_time->minute = minute.value;
    date_time->second = second.value;
    if (sxml_cursor_accept(cursor, '.') &&
        !sxml_cursor_read_nanoseconds(cursor, &date_time->nanosecond)) {
        return false;
    }
    if (date_time->minute > 59 || date_time->second > 59) {
        return false;
    }
    if (date_time->hour == 24) {
        return date_time->minute == 0 && date_time->second == 0 &&
               date_time->nanosecond == 0;
    }
    return date_time->hour <= 23;
}

/* (Z|(\+|-)hh:mm)? with hh:mm at most 14:00 (XSD 1.1 Part 2, D.3.7). */
static bool sxml_cursor_read_timezone(sxml_text_cursor *cursor,
                                      sxml_date_time *date_time) {
    if (sxml_cursor_is_at_end(cursor)) {
        return true;
    }
    date_time->has_timezone = true;
    if (sxml_cursor_accept(cursor, 'Z')) {
        date_time->timezone_offset_minutes = 0;
        return true;
    }
    bool is_negative = sxml_cursor_accept(cursor, '-');
    if (!is_negative && !sxml_cursor_accept(cursor, '+')) {
        return false;
    }
    sxml_two_digits hours = {.value = 0};
    sxml_two_digits minutes = {.value = 0};
    if (!sxml_cursor_read_two_digits(cursor, &hours) ||
        !sxml_cursor_accept(cursor, ':') ||
        !sxml_cursor_read_two_digits(cursor, &minutes) || minutes.value > 59 ||
        hours.value > 14 || (hours.value == 14 && minutes.value != 0)) {
        return false;
    }
    int32_t offset = (int32_t)hours.value * 60 + (int32_t)minutes.value;
    date_time->timezone_offset_minutes =
        (int16_t)(is_negative ? -offset : offset);
    return true;
}

static sxml_date_time_split sxml_split_date_time(sxml_text_cursor *cursor,
                                                 sxml_date_time_layout layout,
                                                 sxml_date_time *date_time) {
    if (layout.has_year) {
        sxml_date_time_split split =
            sxml_cursor_read_year(cursor, &date_time->year);
        if (split != SXML_DATE_TIME_SPLIT_OK) {
            return split;
        }
    } else if (layout.has_month || layout.has_day) {
        /* "--" before gMonthDay and gMonth, "---" before gDay. */
        if (!sxml_cursor_accept(cursor, '-') ||
            !sxml_cursor_accept(cursor, '-') ||
            (!layout.has_month && !sxml_cursor_accept(cursor, '-'))) {
            return SXML_DATE_TIME_SPLIT_MALFORMED;
        }
    }
    if (layout.has_month) {
        sxml_two_digits month = {.value = 0};
        if ((layout.has_year && !sxml_cursor_accept(cursor, '-')) ||
            !sxml_cursor_read_two_digits(cursor, &month)) {
            return SXML_DATE_TIME_SPLIT_MALFORMED;
        }
        date_time->month = month.value;
    }
    if (layout.has_day) {
        sxml_two_digits day = {.value = 0};
        if ((layout.has_month && !sxml_cursor_accept(cursor, '-')) ||
            !sxml_cursor_read_two_digits(cursor, &day)) {
            return SXML_DATE_TIME_SPLIT_MALFORMED;
        }
        date_time->day = day.value;
    }
    if (layout.has_time) {
        if ((layout.has_day && !sxml_cursor_accept(cursor, 'T')) ||
            !sxml_cursor_read_time(cursor, date_time)) {
            return SXML_DATE_TIME_SPLIT_MALFORMED;
        }
    }
    if (!sxml_cursor_read_timezone(cursor, date_time) ||
        !sxml_cursor_is_at_end(cursor)) {
        return SXML_DATE_TIME_SPLIT_MALFORMED;
    }
    return SXML_DATE_TIME_SPLIT_OK;
}

bool sxml_year_is_leap(int64_t year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

uint8_t sxml_days_in_month(int64_t year, uint8_t month) {
    static const uint8_t days_of_common_year[12] = {31, 28, 31, 30, 31, 30,
                                                    31, 31, 30, 31, 30, 31};
    if (month == 2 && sxml_year_is_leap(year)) {
        return 29;
    }
    return days_of_common_year[month - 1];
}

/*
 * 24:00:00 is 00:00:00 of the next day (XSD 1.1 Part 2, D.2.1; XDM 4.0,
 * section 5.4). Returns false for a day that does not exist (left to make
 * to refuse) or a year that would leave int64_t.
 */
static sxml_date_time_split
sxml_normalize_end_of_day(sxml_date_time_layout layout,
                          sxml_date_time *date_time) {
    if (date_time->hour != 24) {
        return SXML_DATE_TIME_SPLIT_OK;
    }
    date_time->hour = 0;
    if (!layout.has_day) {
        return SXML_DATE_TIME_SPLIT_OK;
    }
    if (date_time->month < 1 || date_time->month > 12 || date_time->day < 1 ||
        date_time->day >
            sxml_days_in_month(date_time->year, date_time->month)) {
        return SXML_DATE_TIME_SPLIT_MALFORMED;
    }
    date_time->day = (uint8_t)(date_time->day + 1);
    if (date_time->day >
        sxml_days_in_month(date_time->year, date_time->month)) {
        date_time->day = 1;
        date_time->month = (uint8_t)(date_time->month + 1);
        if (date_time->month > 12) {
            date_time->month = 1;
            if (date_time->year == INT64_MAX) {
                return SXML_DATE_TIME_SPLIT_OVERFLOW;
            }
            date_time->year += 1;
        }
    }
    return SXML_DATE_TIME_SPLIT_OK;
}

static sxml_status
sxml_parse_date_time(sxml_item_heap *item_heap,
                     const sxml_lexical_form_request *request,
                     sxml_item *item) {
    sxml_date_time_layout layout =
        sxml_date_time_layout_of(request->target.primitive_type);
    sxml_date_time date_time = {.year = 0,
                                .month = 0,
                                .day = 0,
                                .hour = 0,
                                .minute = 0,
                                .second = 0,
                                .nanosecond = 0,
                                .has_timezone = false,
                                .timezone_offset_minutes = 0};
    sxml_text_cursor cursor = {.text = request->lexical_form, .byte_offset = 0};
    sxml_date_time_split split =
        sxml_split_date_time(&cursor, layout, &date_time);
    if (split == SXML_DATE_TIME_SPLIT_OK) {
        split = sxml_normalize_end_of_day(layout, &date_time);
    }
    if (split == SXML_DATE_TIME_SPLIT_OVERFLOW) {
        return sxml_lexical_fail(request, &sxml_cast_date_time_overflow_error);
    }
    if (split != SXML_DATE_TIME_SPLIT_OK) {
        return sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
    }
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_DATE_TIME,
                               .datum_details = {.date_time = date_time}};
    return sxml_lexical_make(
        item_heap, (sxml_parsed_datum){.request = request, .datum = datum},
        item);
}

/* -------------------------------------------------------------- Binary */

static int sxml_hex_digit_value(uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

static int sxml_base64_digit_value(uint8_t byte) {
    if (byte >= 'A' && byte <= 'Z') {
        return byte - 'A';
    }
    if (byte >= 'a' && byte <= 'z') {
        return byte - 'a' + 26;
    }
    if (byte >= '0' && byte <= '9') {
        return byte - '0' + 52;
    }
    if (byte == '+') {
        return 62;
    }
    if (byte == '/') {
        return 63;
    }
    return -1;
}

/* Octets decoded into heap memory, released after the item is made. */
typedef struct sxml_decoded_octets {
    uint8_t *octets;
    size_t octet_count;
    size_t capacity;
} sxml_decoded_octets;

/*
 * xs:hexBinary: ([0-9a-fA-F]{2})* (XSD 1.1 Part 2, section 3.3.15). Writes
 * into octets, which has room for half the text.
 */
static bool sxml_decode_hex(sxml_byte_view text, sxml_decoded_octets *octets) {
    if (text.byte_count % 2 != 0) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 2) {
        int high = sxml_hex_digit_value(text.bytes[byte_index]);
        int low = sxml_hex_digit_value(text.bytes[byte_index + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        octets->octets[octets->octet_count] = (uint8_t)(high * 16 + low);
        octets->octet_count += 1;
    }
    return true;
}

/*
 * xs:base64Binary (XSD 1.1 Part 2, section 3.3.16): groups of four
 * characters of the base64 alphabet, single spaces allowed between them,
 * the last group padded with "=" or "==" where the bits it drops are 0.
 * Writes into octets, which has room for three quarters of the text.
 */
static bool sxml_decode_base64(sxml_byte_view text,
                               sxml_decoded_octets *octets) {
    uint32_t group = 0;
    size_t group_character_count = 0;
    size_t padding_count = 0;
    bool is_after_space = true;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        uint8_t byte = text.bytes[byte_index];
        if (byte == ' ') {
            if (is_after_space) {
                return false;
            }
            is_after_space = true;
            continue;
        }
        is_after_space = false;
        int value = 0;
        if (byte == '=') {
            /* "=" only in the last one or two places of the last group. */
            if (group_character_count < 2) {
                return false;
            }
            padding_count += 1;
        } else {
            value = sxml_base64_digit_value(byte);
            if (value < 0 || padding_count != 0) {
                return false;
            }
        }
        group = group * 64U + (uint32_t)value;
        group_character_count += 1;
        if (group_character_count == 4) {
            size_t octet_count = 3 - padding_count;
            /* The dropped bits must be 0: the B16 and B04 productions. */
            if ((padding_count == 1 && (group & 0xFFU) != 0) ||
                (padding_count == 2 && (group & 0xFFFFU) != 0)) {
                return false;
            }
            for (size_t octet_index = 0; octet_index < octet_count;
                 octet_index += 1) {
                octets->octets[octets->octet_count] =
                    (uint8_t)(group >> (16U - 8U * octet_index));
                octets->octet_count += 1;
            }
            group = 0;
            group_character_count = 0;
            if (padding_count != 0) {
                /* Nothing may follow a padded group. */
                for (size_t rest_index = byte_index + 1;
                     rest_index < text.byte_count; rest_index += 1) {
                    if (text.bytes[rest_index] != ' ') {
                        return false;
                    }
                }
                break;
            }
        }
    }
    return group_character_count == 0 &&
           (text.byte_count == 0 || text.bytes[text.byte_count - 1] != ' ');
}

static sxml_status sxml_parse_binary(sxml_item_heap *item_heap,
                                     const sxml_lexical_form_request *request,
                                     sxml_item *item) {
    bool is_hex = request->target.primitive_type == SXML_TYPE_XS_HEX_BINARY;
    sxml_byte_view text = request->lexical_form;
    size_t capacity = is_hex ? text.byte_count / 2 : text.byte_count / 4 * 3;
    sxml_decoded_octets octets = {
        .octets = NULL, .octet_count = 0, .capacity = capacity};
    if (capacity != 0) {
        void *memory = NULL;
        sxml_status status = sxml_item_heap_allocate_bytes(
            item_heap,
            (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        octets.octets = memory;
    }
    bool is_valid = capacity == 0 && text.byte_count != 0
                        ? false
                        : (is_hex ? sxml_decode_hex(text, &octets)
                                  : sxml_decode_base64(text, &octets));
    sxml_status status = SXML_STATUS_OK;
    if (is_valid) {
        sxml_atomic_datum datum = {
            .datum_kind = SXML_DATUM_KIND_BINARY,
            .datum_details = {.binary = {.bytes = octets.octets,
                                         .byte_count = octets.octet_count}}};
        status = sxml_lexical_make(
            item_heap, (sxml_parsed_datum){.request = request, .datum = datum},
            item);
    } else {
        status = sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
    }
    sxml_item_heap_release_bytes(
        item_heap,
        (sxml_heap_bytes){.memory = octets.octets, .byte_count = capacity});
    return status;
}

/* ------------------------------------------------------------- Strings */

static sxml_status sxml_parse_string(sxml_item_heap *item_heap,
                                     const sxml_lexical_form_request *request,
                                     sxml_item *item) {
    sxml_string string;
    if (sxml_string_from_utf8(request->lexical_form, &string) !=
        SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                               .datum_details = {.string = string}};
    return sxml_lexical_make(
        item_heap, (sxml_parsed_datum){.request = request, .datum = datum},
        item);
}

/* -------------------------------------------------------------- QNames */

/* A predeclared prefix (XPath 4.0, section 2.2.1 "Static Context",
   statically known namespaces). */
typedef struct sxml_predeclared_prefix {
    sxml_byte_view namespace_prefix;
    sxml_well_known_namespace namespace_uri;
} sxml_predeclared_prefix;

static const sxml_predeclared_prefix sxml_predeclared_prefixes[] = {
    {SXML_BYTE_VIEW_INITIALIZER("xml"), SXML_NAMESPACE_XML},
    {SXML_BYTE_VIEW_INITIALIZER("xs"), SXML_NAMESPACE_XML_SCHEMA},
    {SXML_BYTE_VIEW_INITIALIZER("xsi"), SXML_NAMESPACE_XML_SCHEMA_INSTANCE},
    {SXML_BYTE_VIEW_INITIALIZER("fn"), SXML_NAMESPACE_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("map"), SXML_NAMESPACE_MAP_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("array"), SXML_NAMESPACE_ARRAY_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("math"), SXML_NAMESPACE_MATH_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("err"), SXML_NAMESPACE_ERRORS},
};

/* A prefix to resolve against the bindings, then the predeclared ones. */
typedef struct sxml_prefix_lookup {
    sxml_byte_view namespace_prefix;
    sxml_namespace_prefix_identifier prefix_identifier;
    sxml_namespace_binding_view namespace_bindings;
} sxml_prefix_lookup;

static bool sxml_resolve_prefix(const sxml_prefix_lookup *lookup,
                                sxml_namespace_uri_identifier *namespace_uri) {
    for (size_t binding_index = 0;
         binding_index < lookup->namespace_bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding *binding =
            &lookup->namespace_bindings.namespace_bindings[binding_index];
        if (binding->namespace_prefix.index ==
            lookup->prefix_identifier.index) {
            *namespace_uri = binding->namespace_uri;
            /* A prefix bound to no namespace is undeclared. */
            return lookup->prefix_identifier.index == 0 ||
                   binding->namespace_uri.index != SXML_NAMESPACE_NONE;
        }
    }
    if (lookup->prefix_identifier.index == 0) {
        /* No default namespace for elements and types. */
        *namespace_uri =
            sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE);
        return true;
    }
    for (size_t prefix_index = 0;
         prefix_index < sizeof(sxml_predeclared_prefixes) /
                            sizeof(sxml_predeclared_prefixes[0]);
         prefix_index += 1) {
        if (sxml_byte_view_equals(
                lookup->namespace_prefix,
                sxml_predeclared_prefixes[prefix_index].namespace_prefix)) {
            *namespace_uri = sxml_well_known_namespace_identifier(
                sxml_predeclared_prefixes[prefix_index].namespace_uri);
            return true;
        }
    }
    return false;
}

/*
 * (NCName ":")? NCName (Namespaces in XML 1.0, section 4), the prefix
 * resolved with the statically known namespaces, an unprefixed name in the
 * default namespace for elements and types (F&O 4.0, section 19.1.9
 * "Casting to xs:QName and xs:NOTATION").
 */
static sxml_status sxml_parse_qname(sxml_item_heap *item_heap,
                                    const sxml_lexical_form_request *request,
                                    sxml_item *item) {
    sxml_byte_view text = request->lexical_form;
    sxml_byte_view namespace_prefix = {.bytes = text.bytes, .byte_count = 0};
    sxml_byte_view local_name = text;
    const uint8_t *colon =
        text.byte_count == 0 ? NULL : memchr(text.bytes, ':', text.byte_count);
    if (colon != NULL) {
        namespace_prefix.byte_count = (size_t)(colon - text.bytes);
        local_name = (sxml_byte_view){
            .bytes = colon + 1,
            .byte_count = text.byte_count - namespace_prefix.byte_count - 1};
        if (!sxml_xml_text_is_ncname(namespace_prefix)) {
            return sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
        }
    }
    if (!sxml_xml_text_is_ncname(local_name)) {
        return sxml_lexical_fail(request, &sxml_cast_lexical_form_error);
    }
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    sxml_namespace_prefix_identifier prefix_identifier = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_prefix(
        name_pool, namespace_prefix, &prefix_identifier);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    if (!sxml_resolve_prefix(
            &(sxml_prefix_lookup){.namespace_prefix = namespace_prefix,
                                  .prefix_identifier = prefix_identifier,
                                  .namespace_bindings =
                                      request->namespace_bindings},
            &namespace_uri)) {
        return sxml_lexical_fail(request, &sxml_cast_unbound_prefix_error);
    }
    sxml_name_identifier expanded_name = {.index = 0};
    status = sxml_name_pool_intern_name(
        name_pool,
        &(sxml_name_request){.namespace_uri = namespace_uri,
                             .local_name = local_name},
        &expanded_name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum = {
        .datum_kind = SXML_DATUM_KIND_QNAME,
        .datum_details = {.qname = {.expanded_name = expanded_name,
                                    .namespace_prefix = prefix_identifier}}};
    return sxml_lexical_make(
        item_heap, (sxml_parsed_datum){.request = request, .datum = datum},
        item);
}

/* ---------------------------------------------------------- Dispatch */

static sxml_status sxml_lexical_make(sxml_item_heap *item_heap,
                                     sxml_parsed_datum parsed,
                                     sxml_item *item) {
    sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){.type = parsed.request->target.type,
                                    .datum = parsed.datum},
        item);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        return sxml_lexical_fail(parsed.request, &sxml_cast_lexical_form_error);
    }
    return status;
}

sxml_status
sxml_cast_make_item_from_lexical_form(sxml_item_heap *item_heap,
                                      const sxml_lexical_form_request *request,
                                      sxml_item *item) {
    if (item_heap == NULL || request == NULL || item == NULL ||
        (request->lexical_form.bytes == NULL &&
         request->lexical_form.byte_count != 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    switch ((uint32_t)request->target.primitive_type) {
    case SXML_TYPE_XS_STRING:
    case SXML_TYPE_XS_UNTYPED_ATOMIC:
    case SXML_TYPE_XS_ANY_URI:
        return sxml_parse_string(item_heap, request, item);
    case SXML_TYPE_XS_BOOLEAN:
        return sxml_parse_boolean(item_heap, request, item);
    case SXML_TYPE_XS_DECIMAL:
        return sxml_parse_decimal(item_heap, request, item);
    case SXML_TYPE_XS_FLOAT:
    case SXML_TYPE_XS_DOUBLE:
        return sxml_parse_binary_number(item_heap, request, item);
    case SXML_TYPE_XS_DURATION:
        return sxml_parse_duration(item_heap, request, item);
    case SXML_TYPE_XS_DATE_TIME:
    case SXML_TYPE_XS_TIME:
    case SXML_TYPE_XS_DATE:
    case SXML_TYPE_XS_G_YEAR_MONTH:
    case SXML_TYPE_XS_G_YEAR:
    case SXML_TYPE_XS_G_MONTH_DAY:
    case SXML_TYPE_XS_G_DAY:
    case SXML_TYPE_XS_G_MONTH:
        return sxml_parse_date_time(item_heap, request, item);
    case SXML_TYPE_XS_HEX_BINARY:
    case SXML_TYPE_XS_BASE64_BINARY:
        return sxml_parse_binary(item_heap, request, item);
    case SXML_TYPE_XS_QNAME:
    case SXML_TYPE_XS_NOTATION:
        return sxml_parse_qname(item_heap, request, item);
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
}

/* ------------------------------------------------------ Canonical text */

/* Writes into output while it has room and counts every byte. */
typedef struct sxml_text_sink {
    sxml_memory_view output;
    size_t byte_count;
} sxml_text_sink;

static void sxml_sink_put(sxml_text_sink *sink, uint8_t byte) {
    if (sink->byte_count < sink->output.byte_count) {
        sink->output.bytes[sink->byte_count] = byte;
    }
    sink->byte_count += 1;
}

static void sxml_sink_put_bytes(sxml_text_sink *sink, sxml_byte_view bytes) {
    for (size_t byte_index = 0; byte_index < bytes.byte_count;
         byte_index += 1) {
        sxml_sink_put(sink, bytes.bytes[byte_index]);
    }
}

/* An unsigned number with at least minimum_digit_count digits. */
typedef struct sxml_padded_number {
    uint64_t value;
    size_t minimum_digit_count;
} sxml_padded_number;

static void sxml_sink_put_number(sxml_text_sink *sink,
                                 sxml_padded_number number) {
    uint8_t reversed_digits[24];
    size_t digit_count = 0;
    uint64_t value = number.value;
    do {
        reversed_digits[digit_count] = (uint8_t)('0' + value % 10U);
        digit_count += 1;
        value /= 10U;
    } while (value != 0);
    for (size_t padding = digit_count; padding < number.minimum_digit_count;
         padding += 1) {
        sxml_sink_put(sink, '0');
    }
    while (digit_count > 0) {
        digit_count -= 1;
        sxml_sink_put(sink, reversed_digits[digit_count]);
    }
}

/* The magnitude of a signed value, INT64_MIN included. */
static uint64_t sxml_magnitude(int64_t value) {
    return value < 0 ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;
}

static void sxml_sink_put_integer(sxml_text_sink *sink, int64_t integer) {
    if (integer < 0) {
        sxml_sink_put(sink, '-');
    }
    sxml_sink_put_number(sink,
                         (sxml_padded_number){.value = sxml_magnitude(integer),
                                              .minimum_digit_count = 1});
}

/* ".digits" without trailing zeros; nothing for 0 nanoseconds. */
static void sxml_sink_put_fraction(sxml_text_sink *sink, uint32_t nanoseconds) {
    if (nanoseconds == 0) {
        return;
    }
    uint32_t value = nanoseconds;
    size_t digit_count = 9;
    while (value % 10U == 0) {
        value /= 10U;
        digit_count -= 1;
    }
    sxml_sink_put(sink, '.');
    sxml_sink_put_number(
        sink, (sxml_padded_number){.value = value,
                                   .minimum_digit_count = digit_count});
}

/* "Z", or (+|-)hh:mm (F&O 4.0, section 19.1.2: a zero timezone is "Z"). */
static void sxml_sink_put_timezone(sxml_text_sink *sink,
                                   const sxml_date_time *date_time) {
    if (!date_time->has_timezone) {
        return;
    }
    int32_t offset = date_time->timezone_offset_minutes;
    if (offset == 0) {
        sxml_sink_put(sink, 'Z');
        return;
    }
    sxml_sink_put(sink, offset < 0 ? '-' : '+');
    uint32_t magnitude = (uint32_t)(offset < 0 ? -offset : offset);
    sxml_sink_put_number(sink, (sxml_padded_number){.value = magnitude / 60U,
                                                    .minimum_digit_count = 2});
    sxml_sink_put(sink, ':');
    sxml_sink_put_number(sink, (sxml_padded_number){.value = magnitude % 60U,
                                                    .minimum_digit_count = 2});
}

static void sxml_sink_put_two_digits(sxml_text_sink *sink, uint8_t value) {
    sxml_sink_put_number(
        sink, (sxml_padded_number){.value = value, .minimum_digit_count = 2});
}

/* A date/time datum and its primitive type. */
typedef struct sxml_date_time_text {
    sxml_builtin_type primitive_type;
    const sxml_date_time *date_time;
} sxml_date_time_text;

/* XSD 1.1 Part 2, appendix D.3 (canonical mappings). */
static void sxml_sink_put_date_time(sxml_text_sink *sink,
                                    sxml_date_time_text text) {
    sxml_date_time_layout layout =
        sxml_date_time_layout_of(text.primitive_type);
    const sxml_date_time *date_time = text.date_time;
    if (layout.has_year) {
        if (date_time->year < 0) {
            sxml_sink_put(sink, '-');
        }
        sxml_sink_put_number(
            sink, (sxml_padded_number){.value = sxml_magnitude(date_time->year),
                                       .minimum_digit_count = 4});
    } else if (layout.has_month || layout.has_day) {
        sxml_sink_put_bytes(sink, layout.has_month
                                      ? SXML_BYTE_VIEW_FROM_LITERAL("-")
                                      : SXML_BYTE_VIEW_FROM_LITERAL("--"));
    }
    if (layout.has_month) {
        sxml_sink_put(sink, '-');
        sxml_sink_put_two_digits(sink, date_time->month);
    }
    if (layout.has_day) {
        sxml_sink_put(sink, '-');
        sxml_sink_put_two_digits(sink, date_time->day);
    }
    if (layout.has_time) {
        if (layout.has_day) {
            sxml_sink_put(sink, 'T');
        }
        sxml_sink_put_two_digits(sink, date_time->hour);
        sxml_sink_put(sink, ':');
        sxml_sink_put_two_digits(sink, date_time->minute);
        sxml_sink_put(sink, ':');
        sxml_sink_put_two_digits(sink, date_time->second);
        sxml_sink_put_fraction(sink, date_time->nanosecond);
    }
    sxml_sink_put_timezone(sink, date_time);
}

/* A duration datum and the built-in type it is canonical for. */
typedef struct sxml_duration_text {
    sxml_builtin_type builtin_type;
    const sxml_duration *duration;
} sxml_duration_text;

/* A component with its designator, written when nonzero. */
typedef struct sxml_duration_component {
    uint64_t value;
    uint8_t designator;
} sxml_duration_component;

static void sxml_sink_put_component(sxml_text_sink *sink,
                                    sxml_duration_component component) {
    if (component.value != 0) {
        sxml_sink_put_number(sink,
                             (sxml_padded_number){.value = component.value,
                                                  .minimum_digit_count = 1});
        sxml_sink_put(sink, component.designator);
    }
}

/*
 * PnYnMnDTnHnMnS without zero components (XSD 1.1 Part 2, sections
 * 3.3.6.2, 3.4.26.2 and 3.4.27.2); zero is "PT0S", or "P0M" for
 * xs:yearMonthDuration.
 */
static void sxml_sink_put_duration(sxml_text_sink *sink,
                                   sxml_duration_text text) {
    const sxml_duration *duration = text.duration;
    bool is_negative = duration->months < 0 || duration->seconds < 0 ||
                       duration->nanoseconds < 0;
    uint64_t months = sxml_magnitude(duration->months);
    uint64_t seconds = sxml_magnitude(duration->seconds);
    uint32_t nanoseconds =
        (uint32_t)(duration->nanoseconds < 0 ? -duration->nanoseconds
                                             : duration->nanoseconds);
    if (months == 0 && seconds == 0 && nanoseconds == 0) {
        sxml_sink_put_bytes(sink, text.builtin_type ==
                                          SXML_TYPE_XS_YEAR_MONTH_DURATION
                                      ? SXML_BYTE_VIEW_FROM_LITERAL("P0M")
                                      : SXML_BYTE_VIEW_FROM_LITERAL("PT0S"));
        return;
    }
    if (is_negative) {
        sxml_sink_put(sink, '-');
    }
    sxml_sink_put(sink, 'P');
    sxml_sink_put_component(
        sink,
        (sxml_duration_component){.value = months / 12U, .designator = 'Y'});
    sxml_sink_put_component(
        sink,
        (sxml_duration_component){.value = months % 12U, .designator = 'M'});
    sxml_sink_put_component(
        sink, (sxml_duration_component){.value = seconds / sxml_seconds_per_day,
                                        .designator = 'D'});
    uint64_t day_seconds = seconds % sxml_seconds_per_day;
    if (day_seconds == 0 && nanoseconds == 0) {
        return;
    }
    sxml_sink_put(sink, 'T');
    sxml_sink_put_component(
        sink,
        (sxml_duration_component){.value = day_seconds / sxml_seconds_per_hour,
                                  .designator = 'H'});
    sxml_sink_put_component(
        sink,
        (sxml_duration_component){.value = day_seconds % sxml_seconds_per_hour /
                                           sxml_seconds_per_minute,
                                  .designator = 'M'});
    uint64_t minute_seconds = day_seconds % sxml_seconds_per_minute;
    if (minute_seconds != 0 || nanoseconds != 0) {
        sxml_sink_put_number(sink,
                             (sxml_padded_number){.value = minute_seconds,
                                                  .minimum_digit_count = 1});
        sxml_sink_put_fraction(sink, nanoseconds);
        sxml_sink_put(sink, 'S');
    }
}

static const uint8_t sxml_upper_hex_digits[16] = {'0', '1', '2', '3', '4', '5',
                                                  '6', '7', '8', '9', 'A', 'B',
                                                  'C', 'D', 'E', 'F'};

static const uint8_t sxml_base64_digits[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

/* Octets and the primitive type they are written for. */
typedef struct sxml_binary_text {
    bool is_hex;
    sxml_byte_view octets;
} sxml_binary_text;

/* Upper-case hex digits, or base64 without line breaks (XSD 1.1 Part 2,
   sections 3.3.15.2 and 3.3.16.2). */
static void sxml_sink_put_binary(sxml_text_sink *sink, sxml_binary_text text) {
    sxml_byte_view octets = text.octets;
    if (text.is_hex) {
        for (size_t octet_index = 0; octet_index < octets.byte_count;
             octet_index += 1) {
            uint8_t octet = octets.bytes[octet_index];
            sxml_sink_put(sink, sxml_upper_hex_digits[octet >> 4U]);
            sxml_sink_put(sink, sxml_upper_hex_digits[octet & 0x0FU]);
        }
        return;
    }
    for (size_t octet_index = 0; octet_index < octets.byte_count;
         octet_index += 3) {
        size_t remaining = octets.byte_count - octet_index;
        uint32_t group = (uint32_t)octets.bytes[octet_index] << 16U;
        if (remaining > 1) {
            group |= (uint32_t)octets.bytes[octet_index + 1] << 8U;
        }
        if (remaining > 2) {
            group |= octets.bytes[octet_index + 2];
        }
        sxml_sink_put(sink, sxml_base64_digits[(group >> 18U) & 0x3FU]);
        sxml_sink_put(sink, sxml_base64_digits[(group >> 12U) & 0x3FU]);
        sxml_sink_put(sink, remaining > 1
                                ? sxml_base64_digits[(group >> 6U) & 0x3FU]
                                : '=');
        sxml_sink_put(sink,
                      remaining > 2 ? sxml_base64_digits[group & 0x3FU] : '=');
    }
}

/* Room for the canonical decimal text of any double or float, from
   5.0E-324 (326 bytes) to 1.8E308 (309 bytes). */
#define SXML_CAST_DOUBLE_DECIMAL_TEXT_CAPACITY 400U

/* A binary floating point number to write. */
typedef struct sxml_binary_number_text {
    bool is_float;
    double double_number;
} sxml_binary_number_text;

/*
 * F&O 4.0, section 19.1.2 for xs:float and xs:double: the special values,
 * then the shortest decimal that reads back as the value (D57); written as
 * that decimal when it is from 1.0E-6 up to 1.0E6 (exclusive), otherwise
 * with one nonzero digit before the point, at least one digit after it, and
 * an exponent without "+" and leading zeros.
 */
static sxml_status sxml_sink_put_binary_number(sxml_item_heap *item_heap,
                                               sxml_text_sink *sink,
                                               sxml_binary_number_text text) {
    double value = text.double_number;
    if (isnan(value)) {
        sxml_sink_put_bytes(sink, SXML_BYTE_VIEW_FROM_LITERAL("NaN"));
        return SXML_STATUS_OK;
    }
    if (isinf(value)) {
        sxml_sink_put_bytes(sink, value < 0
                                      ? SXML_BYTE_VIEW_FROM_LITERAL("-INF")
                                      : SXML_BYTE_VIEW_FROM_LITERAL("INF"));
        return SXML_STATUS_OK;
    }
    if (value == 0.0) {
        sxml_sink_put_bytes(sink, signbit(value)
                                      ? SXML_BYTE_VIEW_FROM_LITERAL("-0")
                                      : SXML_BYTE_VIEW_FROM_LITERAL("0"));
        return SXML_STATUS_OK;
    }
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(item_heap);
    sxml_decimal decimal;
    sxml_status status =
        text.is_float
            ? sxml_decimal_from_float_shortest(configuration, (float)value,
                                               &decimal)
            : sxml_decimal_from_double_shortest(configuration, value, &decimal);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t decimal_bytes[SXML_CAST_DOUBLE_DECIMAL_TEXT_CAPACITY];
    sxml_byte_view decimal_text = {.bytes = NULL, .byte_count = 0};
    status = sxml_decimal_write_canonical_text(
        &decimal,
        (sxml_memory_view){.bytes = decimal_bytes,
                           .byte_count = sizeof(decimal_bytes)},
        &decimal_text);
    sxml_decimal_finalize(&decimal);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The significant digits and the power of ten of the first one. */
    size_t first_digit_offset = decimal_text.bytes[0] == '-' ? 1U : 0U;
    size_t point_offset = decimal_text.byte_count;
    const uint8_t *point =
        memchr(decimal_text.bytes, '.', decimal_text.byte_count);
    if (point != NULL) {
        point_offset = (size_t)(point - decimal_text.bytes);
    }
    uint8_t digits[SXML_CAST_DOUBLE_DECIMAL_TEXT_CAPACITY];
    size_t digit_count = 0;
    int64_t exponent = 0;
    bool has_digit = false;
    for (size_t byte_index = first_digit_offset;
         byte_index < decimal_text.byte_count; byte_index += 1) {
        uint8_t byte = decimal_text.bytes[byte_index];
        if (byte == '.' || (!has_digit && byte == '0')) {
            continue;
        }
        if (!has_digit) {
            exponent = byte_index < point_offset
                           ? (int64_t)(point_offset - byte_index) - 1
                           : -(int64_t)(byte_index - point_offset);
            has_digit = true;
        }
        digits[digit_count] = byte;
        digit_count += 1;
    }
    while (digit_count > 1 && digits[digit_count - 1] == '0') {
        digit_count -= 1;
    }
    if (exponent >= -6 && exponent < 6) {
        sxml_sink_put_bytes(sink, decimal_text);
        return SXML_STATUS_OK;
    }
    if (first_digit_offset != 0) {
        sxml_sink_put(sink, '-');
    }
    sxml_sink_put(sink, digits[0]);
    sxml_sink_put(sink, '.');
    if (digit_count == 1) {
        sxml_sink_put(sink, '0');
    } else {
        sxml_sink_put_bytes(sink,
                            (sxml_byte_view){.bytes = digits + 1,
                                             .byte_count = digit_count - 1});
    }
    sxml_sink_put(sink, 'E');
    sxml_sink_put_integer(sink, exponent);
    return SXML_STATUS_OK;
}

/* prefix:local, or local without a prefix (F&O 4.0, section 19.1.2). */
static sxml_status sxml_sink_put_qname(const sxml_item_heap *item_heap,
                                       sxml_text_sink *sink,
                                       const sxml_qname_datum *qname) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    sxml_name_parts parts;
    sxml_status status =
        sxml_name_pool_get_name_parts(name_pool, qname->expanded_name, &parts);
    sxml_byte_view namespace_prefix = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_prefix(
            name_pool, qname->namespace_prefix, &namespace_prefix);
    }
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (namespace_prefix.byte_count != 0) {
        sxml_sink_put_bytes(sink, namespace_prefix);
        sxml_sink_put(sink, ':');
    }
    sxml_sink_put_bytes(sink, parts.local_name);
    return SXML_STATUS_OK;
}

/* Writes the text of a datum whose text the item does not hold. */
static sxml_status sxml_sink_put_datum(sxml_item_heap *item_heap,
                                       sxml_text_sink *sink, sxml_item item) {
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_builtin_type primitive_type =
        (sxml_builtin_type)sxml_atomic_item_get_primitive_type(item).index;
    switch (datum.datum_kind) {
    case SXML_DATUM_KIND_BOOLEAN:
        sxml_sink_put_bytes(sink, datum.datum_details.boolean
                                      ? SXML_BYTE_VIEW_FROM_LITERAL("true")
                                      : SXML_BYTE_VIEW_FROM_LITERAL("false"));
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_INTEGER:
        sxml_sink_put_integer(sink, datum.datum_details.integer);
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_DECIMAL:
        sxml_sink_put_bytes(sink, datum.datum_details.decimal_text);
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_FLOAT:
        return sxml_sink_put_binary_number(
            item_heap, sink,
            (sxml_binary_number_text){
                .is_float = true,
                .double_number = (double)datum.datum_details.float_number});
    case SXML_DATUM_KIND_DOUBLE:
        return sxml_sink_put_binary_number(
            item_heap, sink,
            (sxml_binary_number_text){.is_float = false,
                                      .double_number =
                                          datum.datum_details.double_number});
    case SXML_DATUM_KIND_STRING:
        sxml_sink_put_bytes(sink, datum.datum_details.string.utf8);
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_QNAME:
        return sxml_sink_put_qname(item_heap, sink, &datum.datum_details.qname);
    case SXML_DATUM_KIND_DATE_TIME:
        sxml_sink_put_date_time(
            sink,
            (sxml_date_time_text){.primitive_type = primitive_type,
                                  .date_time = &datum.datum_details.date_time});
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_DURATION: {
        sxml_type_identifier annotation = sxml_item_get_type_annotation(item);
        sxml_builtin_type builtin_type =
            sxml_type_registry_is_derived_from(
                sxml_item_heap_get_type_registry(item_heap),
                (sxml_type_pair){.derived_type = annotation,
                                 .base_type = sxml_builtin_type_identifier(
                                     SXML_TYPE_XS_YEAR_MONTH_DURATION)})
                ? SXML_TYPE_XS_YEAR_MONTH_DURATION
                : SXML_TYPE_XS_DURATION;
        sxml_sink_put_duration(
            sink,
            (sxml_duration_text){.builtin_type = builtin_type,
                                 .duration = &datum.datum_details.duration});
        return SXML_STATUS_OK;
    }
    case SXML_DATUM_KIND_BINARY:
        sxml_sink_put_binary(
            sink, (sxml_binary_text){.is_hex = primitive_type ==
                                               SXML_TYPE_XS_HEX_BINARY,
                                     .octets = datum.datum_details.binary});
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

sxml_status
sxml_cast_write_canonical_text(sxml_item_heap *item_heap,
                               const sxml_canonical_text_request *request,
                               sxml_canonical_text *canonical_text) {
    if (item_heap == NULL || request == NULL || canonical_text == NULL ||
        !sxml_item_is_atomic(request->item) ||
        (request->output.bytes == NULL && request->output.byte_count != 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Strings and decimals hold their text: borrow it. */
    sxml_type_identifier primitive_type =
        sxml_atomic_item_get_primitive_type(request->item);
    if (sxml_item_get_representation(request->item) ==
            SXML_ITEM_REPRESENTATION_ATOMIC_CELL &&
        (primitive_type.index == SXML_TYPE_XS_STRING ||
         primitive_type.index == SXML_TYPE_XS_UNTYPED_ATOMIC ||
         primitive_type.index == SXML_TYPE_XS_ANY_URI ||
         primitive_type.index == SXML_TYPE_XS_DECIMAL)) {
        sxml_atomic_datum datum;
        sxml_status status = sxml_atomic_item_get_datum(request->item, &datum);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_byte_view text = datum.datum_kind == SXML_DATUM_KIND_STRING
                                  ? datum.datum_details.string.utf8
                                  : datum.datum_details.decimal_text;
        *canonical_text = (sxml_canonical_text){
            .text = text, .required_byte_count = text.byte_count};
        return SXML_STATUS_OK;
    }
    sxml_text_sink sink = {.output = request->output, .byte_count = 0};
    sxml_status status = sxml_sink_put_datum(item_heap, &sink, request->item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sink.byte_count > request->output.byte_count) {
        *canonical_text =
            (sxml_canonical_text){.text = {.bytes = NULL, .byte_count = 0},
                                  .required_byte_count = sink.byte_count};
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    *canonical_text = (sxml_canonical_text){
        .text = {.bytes = request->output.bytes, .byte_count = sink.byte_count},
        .required_byte_count = sink.byte_count};
    return SXML_STATUS_OK;
}
