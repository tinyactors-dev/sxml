/*
 * Arithmetic operators (src/operator_internal.h): single operands,
 * numeric promotion, the numeric operators, unary plus and minus, and the
 * dispatch to the arithmetic of durations, dates and times
 * (src/operator_temporal.c).
 *
 * Golden file: src/memory.c (and src/cast.c for the error tables).
 *
 * Numbers are computed in their promoted type (XPath 4.0, section 4.9):
 * xs:double and xs:float in IEEE 754 arithmetic (an xs:float operation is
 * computed in double and rounded once, which is exact for +, -, * and /
 * because double has more than twice float's precision), xs:integer in
 * int64_t while it fits and in libmpdec beyond (D36), xs:decimal in
 * libmpdec with the heap's decimal configuration (D57). idiv and mod of
 * binary floating point numbers are computed exactly through decimals when
 * the int64_t shortcut does not apply, so no libm is needed.
 *
 * Specification: XPath 4.0, section 4.9 "Arithmetic Expressions"; F&O 4.0,
 * section 4.2 "Arithmetic operators on numeric values".
 */
#include "operator_internal.h"

#include "cast_internal.h"

#include <math.h>

/* --------------------------------------------------------------- Errors */

static const sxml_static_error sxml_operator_cardinality_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an operand of an arithmetic expression or value comparison must be "
        "empty or one item (XPath 4.0, sections 4.9 and 4.11.1)"),
};

static const sxml_static_error sxml_operator_operand_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the operator is not defined for operands of these types (XPath "
        "4.0, section 4.9, binary and unary arithmetic operators)"),
};

static const sxml_static_error sxml_operator_division_by_zero_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "division by zero (F&O 4.0, op:numeric-divide, "
        "op:numeric-integer-divide, op:numeric-mod)"),
};

static const sxml_static_error sxml_operator_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "numeric overflow: the result has more digits than the processor "
        "keeps (F&O 4.0, section 4.2)"),
};

static const sxml_static_error sxml_operator_integer_divide_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "idiv of NaN, or of an infinite dividend (F&O 4.0, "
        "op:numeric-integer-divide)"),
};

/* ------------------------------------------------------ Shared helpers */

sxml_status
sxml_operator_read_single_operand(sxml_sequence operand,
                                  sxml_error_record *error_record,
                                  sxml_single_operand *single_operand) {
    sxml_item first_item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        operand,
        (sxml_item_request){.item_index = 0, .error_record = error_record},
        &first_item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        *single_operand = (sxml_single_operand){.has_item = false,
                                                .item = {.representation = 0}};
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item second_item = {.representation = 0};
    status = sxml_sequence_get_item(
        operand,
        (sxml_item_request){.item_index = 1, .error_record = error_record},
        &second_item);
    if (status == SXML_STATUS_OK) {
        return sxml_error_record_report(error_record,
                                        &sxml_operator_cardinality_error);
    }
    if (status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
        return status;
    }
    if (!sxml_item_is_atomic(first_item)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *single_operand =
        (sxml_single_operand){.has_item = true, .item = first_item};
    return SXML_STATUS_OK;
}

sxml_status sxml_operator_cast_item(sxml_item_heap *item_heap,
                                    const sxml_operator_cast *cast,
                                    sxml_item *item) {
    sxml_sequence result = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value(
        item_heap,
        &(sxml_cast_request){
            .value = sxml_sequence_from_item(cast->item),
            .target_type = sxml_builtin_type_identifier(cast->target_type),
            .allows_empty = false,
            .namespace_bindings = cast->namespace_bindings,
            .error_record = cast->error_record},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* An atomic target gives exactly one item. */
    *item = result.content;
    return SXML_STATUS_OK;
}

bool sxml_operator_item_has_type(const sxml_item_heap *item_heap,
                                 sxml_type_test test) {
    sxml_type_identifier annotation = sxml_item_get_type_annotation(test.item);
    if (annotation.index == (uint32_t)test.type) {
        return true;
    }
    return sxml_type_registry_is_subtype(
        sxml_item_heap_get_type_registry(item_heap),
        (sxml_type_pair){.derived_type = annotation,
                         .base_type = sxml_builtin_type_identifier(test.type)});
}

/* ------------------------------------------------------------- Numbers */

/*
 * A numeric operand: its type after promotion (SXML_TYPE_XS_INTEGER,
 * _DECIMAL, _FLOAT or _DOUBLE) and its value in the member that type
 * uses. An xs:integer is either inline (decimal NULL) or a decimal cell's
 * decimal; an xs:decimal always the decimal of its cell.
 */
typedef struct sxml_number_operand {
    sxml_builtin_type numeric_type;
    int64_t integer;
    /* Borrowed from the item's cell. */
    const sxml_decimal *decimal;
    float float_number;
    double double_number;
} sxml_number_operand;

/* The numeric operand of a numeric item, in its own type. */
static sxml_number_operand
sxml_number_operand_of(const sxml_item_heap *item_heap, sxml_item item) {
    sxml_number_operand number = {
        .numeric_type = SXML_TYPE_XS_DOUBLE,
        .integer = 0,
        .decimal = NULL,
        .float_number = 0.0F,
        .double_number = 0.0,
    };
    switch (sxml_item_get_representation(item)) {
    case SXML_ITEM_REPRESENTATION_INTEGER:
        number.numeric_type = SXML_TYPE_XS_INTEGER;
        number.integer = item.payload.integer;
        return number;
    case SXML_ITEM_REPRESENTATION_FLOAT:
        number.numeric_type = SXML_TYPE_XS_FLOAT;
        number.float_number = item.payload.float_number;
        return number;
    case SXML_ITEM_REPRESENTATION_DOUBLE:
        number.double_number = item.payload.double_number;
        return number;
    case SXML_ITEM_REPRESENTATION_ATOMIC_CELL:
        number.decimal = sxml_atomic_item_get_decimal(item);
        number.numeric_type =
            sxml_operator_item_has_type(
                item_heap,
                (sxml_type_test){.item = item, .type = SXML_TYPE_XS_INTEGER})
                ? SXML_TYPE_XS_INTEGER
                : SXML_TYPE_XS_DECIMAL;
        return number;
    case SXML_ITEM_REPRESENTATION_NONE:
    case SXML_ITEM_REPRESENTATION_BOOLEAN:
    case SXML_ITEM_REPRESENTATION_QNAME:
    case SXML_ITEM_REPRESENTATION_NODE:
    case SXML_ITEM_REPRESENTATION_JSON_NODE_CELL:
    case SXML_ITEM_REPRESENTATION_FUNCTION_CELL:
    case SXML_ITEM_REPRESENTATION_MAP_CELL:
    case SXML_ITEM_REPRESENTATION_ARRAY_CELL:
    case SXML_ITEM_REPRESENTATION_VECTOR_CELL:
    case SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL:
    case SXML_ITEM_REPRESENTATION_CONCATENATION_CELL:
    case SXML_ITEM_REPRESENTATION_LAZY_CELL:
        break;
    }
    return number;
}

/* The order of promotion (XPath 4.0, section 4.9): integer, decimal,
   float, double. */
static int sxml_numeric_type_rank(sxml_builtin_type numeric_type) {
    if (numeric_type == SXML_TYPE_XS_INTEGER) {
        return 0;
    }
    if (numeric_type == SXML_TYPE_XS_DECIMAL) {
        return 1;
    }
    return numeric_type == SXML_TYPE_XS_FLOAT ? 2 : 3;
}

static double sxml_number_to_double(sxml_number_operand number) {
    switch (sxml_numeric_type_rank(number.numeric_type)) {
    case 0:
    case 1:
        return number.decimal != NULL ? sxml_decimal_to_double(number.decimal)
                                      : (double)number.integer;
    case 2:
        return (double)number.float_number;
    default:
        return number.double_number;
    }
}

/* Only called for integers and decimals: floats and doubles do not
   become xs:float. */
static float sxml_number_to_float(sxml_number_operand number) {
    if (number.numeric_type == SXML_TYPE_XS_FLOAT) {
        return number.float_number;
    }
    return number.decimal != NULL ? sxml_decimal_to_float(number.decimal)
                                  : (float)number.integer;
}

/* The two operands of a numeric operation, both of one promoted type. */
typedef struct sxml_number_operands {
    sxml_number_operand left_operand;
    sxml_number_operand right_operand;
} sxml_number_operands;

/*
 * Promotes both operands to their common type: xs:double if either is,
 * else xs:float if either is, else xs:decimal if either is, else
 * xs:integer (XPath 4.0, section 4.9).
 */
static sxml_number_operands
sxml_numbers_promote(sxml_number_operands operands) {
    int left_rank = sxml_numeric_type_rank(operands.left_operand.numeric_type);
    int right_rank =
        sxml_numeric_type_rank(operands.right_operand.numeric_type);
    int common_rank = left_rank > right_rank ? left_rank : right_rank;
    sxml_number_operand *both[2] = {&operands.left_operand,
                                    &operands.right_operand};
    for (size_t operand_index = 0; operand_index < 2; operand_index += 1) {
        sxml_number_operand *number = both[operand_index];
        if (common_rank == 3) {
            number->double_number = sxml_number_to_double(*number);
            number->numeric_type = SXML_TYPE_XS_DOUBLE;
        } else if (common_rank == 2) {
            number->float_number = sxml_number_to_float(*number);
            number->numeric_type = SXML_TYPE_XS_FLOAT;
        } else if (common_rank == 1) {
            number->numeric_type = SXML_TYPE_XS_DECIMAL;
        }
    }
    return operands;
}

/* ------------------------------------------------ Checked int64 helpers */

typedef struct sxml_integer_operands {
    int64_t left_operand;
    int64_t right_operand;
} sxml_integer_operands;

/* True, with *sum written, when the sum fits in int64_t. */
static bool sxml_integer_add_fits(sxml_integer_operands operands,
                                  int64_t *sum) {
    int64_t left = operands.left_operand;
    int64_t right = operands.right_operand;
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
        return false;
    }
    *sum = left + right;
    return true;
}

static bool sxml_integer_subtract_fits(sxml_integer_operands operands,
                                       int64_t *difference) {
    int64_t left = operands.left_operand;
    int64_t right = operands.right_operand;
    if ((right < 0 && left > INT64_MAX + right) ||
        (right > 0 && left < INT64_MIN + right)) {
        return false;
    }
    *difference = left - right;
    return true;
}

static bool sxml_integer_multiply_fits(sxml_integer_operands operands,
                                       int64_t *product) {
    int64_t left = operands.left_operand;
    int64_t right = operands.right_operand;
    if (left == 0 || right == 0) {
        *product = 0;
        return true;
    }
    bool overflows = false;
    if (left > 0) {
        overflows =
            right > 0 ? left > INT64_MAX / right : right < INT64_MIN / left;
    } else {
        overflows =
            right > 0 ? left < INT64_MIN / right : right < INT64_MAX / left;
    }
    if (overflows) {
        return false;
    }
    *product = left * right;
    return true;
}

/* ---------------------------------------------- Decimal and integer path */

/* What a decimal computation needs besides its operands. */
typedef struct sxml_decimal_computation {
    sxml_arithmetic_operator arithmetic_operator;
    /* The annotation of the result: xs:integer or xs:decimal. */
    sxml_builtin_type result_type;
    /* Both of type xs:integer or xs:decimal; borrowed. */
    const sxml_number_operands *operands;
    sxml_error_record *error_record;
} sxml_decimal_computation;

/*
 * The decimal of an integer or decimal operand: borrowed from its cell, or
 * made from an inline integer into storage, which then needs finalizing.
 */
typedef struct sxml_owned_decimal {
    const sxml_decimal *decimal;
    bool has_storage;
    sxml_decimal storage;
} sxml_owned_decimal;

static sxml_status
sxml_number_get_decimal(const sxml_decimal_configuration *configuration,
                        const sxml_number_operand *number,
                        sxml_owned_decimal *owned_decimal) {
    owned_decimal->has_storage = false;
    owned_decimal->decimal = number->decimal;
    if (number->decimal != NULL) {
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_decimal_from_integer(
        configuration, number->integer, &owned_decimal->storage);
    if (status == SXML_STATUS_OK) {
        owned_decimal->has_storage = true;
        owned_decimal->decimal = &owned_decimal->storage;
    }
    return status;
}

static void sxml_owned_decimal_finalize(sxml_owned_decimal *owned_decimal) {
    if (owned_decimal->has_storage) {
        sxml_decimal_finalize(&owned_decimal->storage);
        owned_decimal->has_storage = false;
    }
}

/* Maps a failure of the decimal functions to the operator's error. */
static sxml_status sxml_decimal_failure(sxml_status status,
                                        sxml_error_record *error_record) {
    if (status == SXML_STATUS_LIMIT_EXCEEDED) {
        /* The digit limit of D57 (or the heap's byte limit through the
           heap allocator) is the processor's precision: FOAR0002. */
        return sxml_error_record_report(error_record,
                                        &sxml_operator_overflow_error);
    }
    return status;
}

/* An operator applied to two decimals. */
typedef struct sxml_decimal_operation {
    sxml_arithmetic_operator arithmetic_operator;
    sxml_decimal_operands operands;
} sxml_decimal_operation;

/* Runs the decimal function of an operator on two decimals. */
static sxml_status
sxml_decimal_apply(const sxml_decimal_configuration *configuration,
                   const sxml_decimal_operation *operation,
                   sxml_decimal *value) {
    const sxml_decimal_operands *operands = &operation->operands;
    switch (operation->arithmetic_operator) {
    case SXML_ARITHMETIC_OPERATOR_ADD:
        return sxml_decimal_add(configuration, operands, value);
    case SXML_ARITHMETIC_OPERATOR_SUBTRACT:
        return sxml_decimal_subtract(configuration, operands, value);
    case SXML_ARITHMETIC_OPERATOR_MULTIPLY:
        return sxml_decimal_multiply(configuration, operands, value);
    case SXML_ARITHMETIC_OPERATOR_DIVIDE:
        return sxml_decimal_divide(configuration, operands, value);
    case SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE:
        return sxml_decimal_integer_divide(configuration, operands, value);
    case SXML_ARITHMETIC_OPERATOR_MODULO:
        return sxml_decimal_modulo(configuration, operands, value);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Makes the item of a computed decimal, which it takes on success. */
static sxml_status sxml_decimal_result_make(sxml_item_heap *item_heap,
                                            sxml_decimal_item_request request,
                                            sxml_item *result) {
    sxml_status status =
        sxml_atomic_item_make_taking_decimal(item_heap, &request, result);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(request.decimal);
    }
    return status;
}

/* An integer or decimal operation in libmpdec (D36, D57). */
static sxml_status
sxml_decimal_compute(sxml_item_heap *item_heap,
                     const sxml_decimal_computation *computation,
                     sxml_item *result) {
    const sxml_number_operands *operands = computation->operands;
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(item_heap);
    sxml_owned_decimal left_decimal;
    sxml_owned_decimal right_decimal;
    sxml_status status = sxml_number_get_decimal(
        configuration, &operands->left_operand, &left_decimal);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_number_get_decimal(configuration, &operands->right_operand,
                                     &right_decimal);
    if (status != SXML_STATUS_OK) {
        sxml_owned_decimal_finalize(&left_decimal);
        return status;
    }
    sxml_arithmetic_operator arithmetic_operator =
        computation->arithmetic_operator;
    bool divides =
        arithmetic_operator == SXML_ARITHMETIC_OPERATOR_DIVIDE ||
        arithmetic_operator == SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE ||
        arithmetic_operator == SXML_ARITHMETIC_OPERATOR_MODULO;
    sxml_decimal value;
    if (divides && sxml_decimal_is_zero(right_decimal.decimal)) {
        /* F&O 4.0, op:numeric-divide and following: FOAR0001. */
        status = sxml_error_record_report(
            computation->error_record, &sxml_operator_division_by_zero_error);
    } else {
        status = sxml_decimal_apply(
            configuration,
            &(sxml_decimal_operation){
                .arithmetic_operator = arithmetic_operator,
                .operands = {.left_operand = left_decimal.decimal,
                             .right_operand = right_decimal.decimal}},
            &value);
        if (status != SXML_STATUS_OK) {
            status = sxml_decimal_failure(status, computation->error_record);
        }
    }
    sxml_owned_decimal_finalize(&left_decimal);
    sxml_owned_decimal_finalize(&right_decimal);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_decimal_result_make(
        item_heap,
        (sxml_decimal_item_request){
            .type = sxml_builtin_type_identifier(computation->result_type),
            .decimal = &value},
        result);
}

/*
 * xs:integer operands held inline: the int64_t result when it fits.
 * Writes has_result false when the result needs libmpdec (overflow, and
 * every division, whose result is an xs:decimal cell).
 */
typedef struct sxml_inline_integer_result {
    bool has_result;
    int64_t integer;
} sxml_inline_integer_result;

static sxml_inline_integer_result
sxml_inline_integers_compute(sxml_arithmetic_operator arithmetic_operator,
                             sxml_integer_operands operands) {
    sxml_inline_integer_result outcome = {.has_result = false, .integer = 0};
    switch (arithmetic_operator) {
    case SXML_ARITHMETIC_OPERATOR_ADD:
        outcome.has_result = sxml_integer_add_fits(operands, &outcome.integer);
        break;
    case SXML_ARITHMETIC_OPERATOR_SUBTRACT:
        outcome.has_result =
            sxml_integer_subtract_fits(operands, &outcome.integer);
        break;
    case SXML_ARITHMETIC_OPERATOR_MULTIPLY:
        outcome.has_result =
            sxml_integer_multiply_fits(operands, &outcome.integer);
        break;
    case SXML_ARITHMETIC_OPERATOR_DIVIDE:
        break;
    case SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE:
        /* C division truncates toward zero, as op:numeric-integer-divide
           does; INT64_MIN idiv -1 overflows. */
        if (operands.right_operand != 0 &&
            !(operands.left_operand == INT64_MIN &&
              operands.right_operand == -1)) {
            outcome.has_result = true;
            outcome.integer = operands.left_operand / operands.right_operand;
        }
        break;
    case SXML_ARITHMETIC_OPERATOR_MODULO:
        /* The remainder takes the sign of the dividend in C as in
           op:numeric-mod; x mod -1 is 0 (and INT64_MIN % -1 is undefined
           in C). */
        if (operands.right_operand == -1) {
            outcome.has_result = true;
            outcome.integer = 0;
        } else if (operands.right_operand != 0) {
            outcome.has_result = true;
            outcome.integer = operands.left_operand % operands.right_operand;
        }
        break;
    }
    return outcome;
}

/* ------------------------------------------------- Binary floating point */

/* 2^53: integers up to it in magnitude are exact doubles. */
static const double sxml_two_to_the_53 = 9007199254740992.0;

/* True, with *integer written, for an integral double within 2^53. */
static bool sxml_double_to_small_integer(double double_number,
                                         int64_t *integer) {
    if (!(double_number >= -sxml_two_to_the_53 &&
          double_number <= sxml_two_to_the_53)) {
        return false;
    }
    int64_t truncated = (int64_t)double_number;
    if ((double)truncated != double_number) {
        return false;
    }
    *integer = truncated;
    return true;
}

/* A double operation whose result is a double: idiv and mod need
   exactness (computed through decimals beyond the int64_t shortcut). */
typedef struct sxml_double_computation {
    sxml_arithmetic_operator arithmetic_operator;
    double left_operand;
    double right_operand;
    sxml_error_record *error_record;
} sxml_double_computation;

/*
 * The exact a idiv b or a mod b of two finite doubles, b not zero,
 * through decimals: *result is an xs:integer item for idiv, and for mod
 * the double of the exact remainder (which is exactly representable, the
 * IEEE 754 fmod).
 */
static sxml_status
sxml_doubles_divide_exactly(sxml_item_heap *item_heap,
                            const sxml_double_computation *computation,
                            sxml_item *result) {
    int64_t left_integer = 0;
    int64_t right_integer = 0;
    if (sxml_double_to_small_integer(computation->left_operand,
                                     &left_integer) &&
        sxml_double_to_small_integer(computation->right_operand,
                                     &right_integer)) {
        if (computation->arithmetic_operator ==
            SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE) {
            *result =
                sxml_atomic_item_from_integer(left_integer / right_integer);
            return SXML_STATUS_OK;
        }
        int64_t remainder = left_integer % right_integer;
        /* A zero remainder keeps the sign of the dividend (-0.0e0). */
        *result = sxml_atomic_item_from_double(
            remainder == 0 ? computation->left_operand * 0.0
                           : (double)remainder);
        return SXML_STATUS_OK;
    }
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(item_heap);
    sxml_decimal left_decimal;
    sxml_decimal right_decimal;
    sxml_decimal value;
    sxml_status status = sxml_decimal_from_double_exactly(
        configuration, computation->left_operand, &left_decimal);
    if (status != SXML_STATUS_OK) {
        return sxml_decimal_failure(status, computation->error_record);
    }
    status = sxml_decimal_from_double_exactly(
        configuration, computation->right_operand, &right_decimal);
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_apply(
            configuration,
            &(sxml_decimal_operation){
                .arithmetic_operator = computation->arithmetic_operator,
                .operands = {.left_operand = &left_decimal,
                             .right_operand = &right_decimal}},
            &value);
        sxml_decimal_finalize(&right_decimal);
    }
    sxml_decimal_finalize(&left_decimal);
    if (status != SXML_STATUS_OK) {
        return sxml_decimal_failure(status, computation->error_record);
    }
    if (computation->arithmetic_operator ==
        SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE) {
        return sxml_decimal_result_make(
            item_heap,
            (sxml_decimal_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                .decimal = &value},
            result);
    }
    double remainder = sxml_decimal_is_zero(&value)
                           ? computation->left_operand * 0.0
                           : sxml_decimal_to_double(&value);
    sxml_decimal_finalize(&value);
    *result = sxml_atomic_item_from_double(remainder);
    return SXML_STATUS_OK;
}

/*
 * The double result of a double operation (F&O 4.0, sections 4.2.1 to
 * 4.2.6, IEEE 754 for +, -, *, div), or for idiv an xs:integer item.
 * *is_integer_result tells which: the caller makes xs:float from a double
 * result of float operands.
 */
typedef struct sxml_double_outcome {
    bool is_item;
    sxml_item item;
    double double_number;
} sxml_double_outcome;

static sxml_status
sxml_doubles_compute(sxml_item_heap *item_heap,
                     const sxml_double_computation *computation,
                     sxml_double_outcome *outcome) {
    double left = computation->left_operand;
    double right = computation->right_operand;
    *outcome = (sxml_double_outcome){
        .is_item = false, .item = {.representation = 0}, .double_number = 0.0};
    switch (computation->arithmetic_operator) {
    case SXML_ARITHMETIC_OPERATOR_ADD:
        outcome->double_number = left + right;
        return SXML_STATUS_OK;
    case SXML_ARITHMETIC_OPERATOR_SUBTRACT:
        outcome->double_number = left - right;
        return SXML_STATUS_OK;
    case SXML_ARITHMETIC_OPERATOR_MULTIPLY:
        outcome->double_number = left * right;
        return SXML_STATUS_OK;
    case SXML_ARITHMETIC_OPERATOR_DIVIDE:
        outcome->double_number = left / right;
        return SXML_STATUS_OK;
    case SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE:
        /* F&O 4.0, op:numeric-integer-divide. */
        if (isnan(left) || isnan(right) || isinf(left)) {
            return sxml_error_record_report(
                computation->error_record, &sxml_operator_integer_divide_error);
        }
        if (right == 0.0) {
            return sxml_error_record_report(
                computation->error_record,
                &sxml_operator_division_by_zero_error);
        }
        outcome->is_item = true;
        if (isinf(right)) {
            outcome->item = sxml_atomic_item_from_integer(0);
            return SXML_STATUS_OK;
        }
        return sxml_doubles_divide_exactly(item_heap, computation,
                                           &outcome->item);
    case SXML_ARITHMETIC_OPERATOR_MODULO:
        /* F&O 4.0, op:numeric-mod: NaN for NaN, an infinite dividend or a
           zero divisor; the dividend for an infinite divisor. */
        if (isnan(left) || isnan(right) || isinf(left) || right == 0.0) {
            outcome->double_number = (double)NAN;
            return SXML_STATUS_OK;
        }
        if (isinf(right) || left == 0.0) {
            outcome->double_number = left;
            return SXML_STATUS_OK;
        }
        {
            sxml_item remainder = {.representation = 0};
            sxml_status status =
                sxml_doubles_divide_exactly(item_heap, computation, &remainder);
            if (status == SXML_STATUS_OK) {
                outcome->double_number = remainder.payload.double_number;
            }
            return status;
        }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Makes an xs:float item (inline: needs no heap memory). */
static sxml_status sxml_float_item_make(sxml_item_heap *item_heap,
                                        float float_number, sxml_item *item) {
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_FLOAT),
            .datum = {.datum_kind = SXML_DATUM_KIND_FLOAT,
                      .datum_details = {.float_number = float_number}}},
        item);
}

/* -------------------------------------------------------- Numeric dispatch */

typedef struct sxml_numeric_arithmetic {
    sxml_arithmetic_operator arithmetic_operator;
    sxml_item left_item;
    sxml_item right_item;
    sxml_error_record *error_record;
} sxml_numeric_arithmetic;

/* op:numeric-add to op:numeric-mod on two numeric items. */
static sxml_status
sxml_numbers_compute(sxml_item_heap *item_heap,
                     const sxml_numeric_arithmetic *arithmetic,
                     sxml_item *result) {
    sxml_number_operands operands = sxml_numbers_promote((sxml_number_operands){
        .left_operand =
            sxml_number_operand_of(item_heap, arithmetic->left_item),
        .right_operand =
            sxml_number_operand_of(item_heap, arithmetic->right_item)});
    sxml_builtin_type numeric_type = operands.left_operand.numeric_type;
    if (numeric_type == SXML_TYPE_XS_DOUBLE ||
        numeric_type == SXML_TYPE_XS_FLOAT) {
        bool is_float = numeric_type == SXML_TYPE_XS_FLOAT;
        sxml_double_computation computation = {
            .arithmetic_operator = arithmetic->arithmetic_operator,
            .left_operand = is_float
                                ? (double)operands.left_operand.float_number
                                : operands.left_operand.double_number,
            .right_operand = is_float
                                 ? (double)operands.right_operand.float_number
                                 : operands.right_operand.double_number,
            .error_record = arithmetic->error_record,
        };
        sxml_double_outcome outcome;
        sxml_status status =
            sxml_doubles_compute(item_heap, &computation, &outcome);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (outcome.is_item) {
            *result = outcome.item;
            return SXML_STATUS_OK;
        }
        if (is_float) {
            /* One rounding of the exact double result (see the top). */
            return sxml_float_item_make(item_heap, (float)outcome.double_number,
                                        result);
        }
        *result = sxml_atomic_item_from_double(outcome.double_number);
        return SXML_STATUS_OK;
    }
    bool are_integers =
        numeric_type == SXML_TYPE_XS_INTEGER &&
        operands.right_operand.numeric_type == SXML_TYPE_XS_INTEGER;
    if (are_integers && operands.left_operand.decimal == NULL &&
        operands.right_operand.decimal == NULL) {
        sxml_inline_integer_result inline_result = sxml_inline_integers_compute(
            arithmetic->arithmetic_operator,
            (sxml_integer_operands){
                .left_operand = operands.left_operand.integer,
                .right_operand = operands.right_operand.integer});
        if (inline_result.has_result) {
            *result = sxml_atomic_item_from_integer(inline_result.integer);
            return SXML_STATUS_OK;
        }
    }
    /* The result type (F&O 4.0, section 4.2): xs:integer for two integers
       except div, which gives xs:decimal; always xs:integer for idiv. */
    sxml_builtin_type result_type = SXML_TYPE_XS_DECIMAL;
    if (arithmetic->arithmetic_operator ==
            SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE ||
        (are_integers &&
         arithmetic->arithmetic_operator != SXML_ARITHMETIC_OPERATOR_DIVIDE)) {
        result_type = SXML_TYPE_XS_INTEGER;
    }
    return sxml_decimal_compute(
        item_heap,
        &(sxml_decimal_computation){.arithmetic_operator =
                                        arithmetic->arithmetic_operator,
                                    .result_type = result_type,
                                    .operands = &operands,
                                    .error_record = arithmetic->error_record},
        result);
}

/* ---------------------------------------------------------- Operands */

/* An operand after the casting of xs:untypedAtomic to xs:double. */
typedef struct sxml_arithmetic_operand {
    bool has_item;
    sxml_item item;
    /* True when item was made by the cast and must be released. */
    bool is_owned;
} sxml_arithmetic_operand;

typedef struct sxml_operand_reading {
    sxml_sequence operand;
    sxml_error_record *error_record;
} sxml_operand_reading;

/* Reads an operand and casts xs:untypedAtomic to xs:double (XPath 4.0,
   section 4.9; FORG0001 when the cast fails). */
static sxml_status
sxml_arithmetic_operand_read(sxml_item_heap *item_heap,
                             sxml_operand_reading reading,
                             sxml_arithmetic_operand *arithmetic_operand) {
    sxml_single_operand single_operand = {.has_item = false,
                                          .item = {.representation = 0}};
    sxml_status status = sxml_operator_read_single_operand(
        reading.operand, reading.error_record, &single_operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *arithmetic_operand =
        (sxml_arithmetic_operand){.has_item = single_operand.has_item,
                                  .item = single_operand.item,
                                  .is_owned = false};
    if (!single_operand.has_item ||
        sxml_atomic_item_get_primitive_type(single_operand.item).index !=
            (uint32_t)SXML_TYPE_XS_UNTYPED_ATOMIC) {
        return SXML_STATUS_OK;
    }
    sxml_item double_item = {.representation = 0};
    status = sxml_operator_cast_item(
        item_heap,
        &(sxml_operator_cast){
            .item = single_operand.item,
            .target_type = SXML_TYPE_XS_DOUBLE,
            .namespace_bindings = {.namespace_bindings = NULL,
                                   .namespace_binding_count = 0},
            .error_record = reading.error_record},
        &double_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    arithmetic_operand->item = double_item;
    arithmetic_operand->is_owned = true;
    return SXML_STATUS_OK;
}

static void
sxml_arithmetic_operand_release(sxml_arithmetic_operand *arithmetic_operand) {
    if (arithmetic_operand->is_owned) {
        sxml_item_release(arithmetic_operand->item);
        arithmetic_operand->is_owned = false;
    }
}

/* ---------------------------------------------------------- Interface */

sxml_status
sxml_operator_make_arithmetic_value(sxml_item_heap *item_heap,
                                    const sxml_arithmetic_request *request,
                                    sxml_sequence *result) {
    sxml_arithmetic_operand left_operand = {
        .has_item = false, .item = {.representation = 0}, .is_owned = false};
    sxml_status status = sxml_arithmetic_operand_read(
        item_heap,
        (sxml_operand_reading){.operand = request->left_operand,
                               .error_record = request->error_record},
        &left_operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!left_operand.has_item) {
        /* An empty operand gives the empty sequence (XPath 4.0, 4.9). */
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_arithmetic_operand right_operand = {
        .has_item = false, .item = {.representation = 0}, .is_owned = false};
    status = sxml_arithmetic_operand_read(
        item_heap,
        (sxml_operand_reading){.operand = request->right_operand,
                               .error_record = request->error_record},
        &right_operand);
    if (status != SXML_STATUS_OK) {
        sxml_arithmetic_operand_release(&left_operand);
        return status;
    }
    if (!right_operand.has_item) {
        sxml_arithmetic_operand_release(&left_operand);
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_item value = {.representation = 0};
    if (sxml_atomic_item_is_numeric(left_operand.item) &&
        sxml_atomic_item_is_numeric(right_operand.item)) {
        status = sxml_numbers_compute(
            item_heap,
            &(sxml_numeric_arithmetic){.arithmetic_operator =
                                           request->arithmetic_operator,
                                       .left_item = left_operand.item,
                                       .right_item = right_operand.item,
                                       .error_record = request->error_record},
            &value);
    } else {
        status = sxml_operator_make_temporal_value(
            item_heap,
            &(sxml_temporal_arithmetic){
                .arithmetic_operator = request->arithmetic_operator,
                .left_item = left_operand.item,
                .right_item = right_operand.item,
                .implicit_timezone_offset_minutes =
                    request->implicit_timezone_offset_minutes,
                .error_record = request->error_record},
            &value);
    }
    sxml_arithmetic_operand_release(&left_operand);
    sxml_arithmetic_operand_release(&right_operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = sxml_sequence_from_item(value);
    return SXML_STATUS_OK;
}

/* Reads the operand of a unary operator: empty, or a numeric item. */
static sxml_status
sxml_unary_operand_read(sxml_item_heap *item_heap,
                        const sxml_unary_arithmetic_request *request,
                        sxml_arithmetic_operand *arithmetic_operand) {
    sxml_status status = sxml_arithmetic_operand_read(
        item_heap,
        (sxml_operand_reading){.operand = request->operand,
                               .error_record = request->error_record},
        arithmetic_operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (arithmetic_operand->has_item &&
        !sxml_atomic_item_is_numeric(arithmetic_operand->item)) {
        sxml_arithmetic_operand_release(arithmetic_operand);
        return sxml_error_record_report(request->error_record,
                                        &sxml_operator_operand_type_error);
    }
    return SXML_STATUS_OK;
}

/* The item of a number with the annotation of its numeric type. */
static sxml_status sxml_number_item_make(sxml_item_heap *item_heap,
                                         sxml_number_operand number,
                                         sxml_item *item) {
    switch (sxml_numeric_type_rank(number.numeric_type)) {
    case 0:
    case 1:
        if (number.decimal == NULL) {
            *item = sxml_atomic_item_from_integer(number.integer);
            return SXML_STATUS_OK;
        }
        {
            sxml_decimal copy;
            sxml_status status = sxml_decimal_copy(
                sxml_item_heap_get_decimal_configuration(item_heap),
                number.decimal, &copy);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            return sxml_decimal_result_make(
                item_heap,
                (sxml_decimal_item_request){
                    .type = sxml_builtin_type_identifier(number.numeric_type),
                    .decimal = &copy},
                item);
        }
    case 2:
        return sxml_float_item_make(item_heap, number.float_number, item);
    default:
        *item = sxml_atomic_item_from_double(number.double_number);
        return SXML_STATUS_OK;
    }
}

sxml_status sxml_operator_make_unary_plus_value(
    sxml_item_heap *item_heap, const sxml_unary_arithmetic_request *request,
    sxml_sequence *result) {
    sxml_arithmetic_operand operand = {
        .has_item = false, .item = {.representation = 0}, .is_owned = false};
    sxml_status status = sxml_unary_operand_read(item_heap, request, &operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!operand.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_item value = {.representation = 0};
    sxml_number_operand number =
        sxml_number_operand_of(item_heap, operand.item);
    sxml_type_identifier annotation =
        sxml_item_get_type_annotation(operand.item);
    if (annotation.index == (uint32_t)number.numeric_type) {
        /* Already of its numeric type: the item itself (F&O 4.0,
           op:numeric-unary-plus). */
        sxml_item_retain(operand.item);
        value = operand.item;
    } else {
        status = sxml_number_item_make(item_heap, number, &value);
    }
    sxml_arithmetic_operand_release(&operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = sxml_sequence_from_item(value);
    return SXML_STATUS_OK;
}

sxml_status sxml_operator_make_unary_minus_value(
    sxml_item_heap *item_heap, const sxml_unary_arithmetic_request *request,
    sxml_sequence *result) {
    sxml_arithmetic_operand operand = {
        .has_item = false, .item = {.representation = 0}, .is_owned = false};
    sxml_status status = sxml_unary_operand_read(item_heap, request, &operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!operand.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_number_operand number =
        sxml_number_operand_of(item_heap, operand.item);
    sxml_item value = {.representation = 0};
    switch (sxml_numeric_type_rank(number.numeric_type)) {
    case 0:
    case 1:
        if (number.decimal == NULL && number.integer != INT64_MIN) {
            value = sxml_atomic_item_from_integer(-number.integer);
            break;
        }
        {
            /* F&O 4.0, op:numeric-unary-minus, exact in libmpdec. */
            const sxml_decimal_configuration *configuration =
                sxml_item_heap_get_decimal_configuration(item_heap);
            sxml_owned_decimal owned_decimal;
            sxml_decimal negation;
            status =
                sxml_number_get_decimal(configuration, &number, &owned_decimal);
            if (status == SXML_STATUS_OK) {
                status = sxml_decimal_negate(configuration,
                                             owned_decimal.decimal, &negation);
                sxml_owned_decimal_finalize(&owned_decimal);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_decimal_result_make(
                    item_heap,
                    (sxml_decimal_item_request){
                        .type =
                            sxml_builtin_type_identifier(number.numeric_type),
                        .decimal = &negation},
                    &value);
            } else {
                status = sxml_decimal_failure(status, request->error_record);
            }
        }
        break;
    case 2:
        status = sxml_float_item_make(item_heap, -number.float_number, &value);
        break;
    default:
        value = sxml_atomic_item_from_double(-number.double_number);
        break;
    }
    sxml_arithmetic_operand_release(&operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = sxml_sequence_from_item(value);
    return SXML_STATUS_OK;
}
