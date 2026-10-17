/*
 * The numeric part of the function library (packet 1.20, D78): fn:abs,
 * fn:ceiling, fn:floor, fn:round, fn:round-half-to-even, fn:number,
 * fn:is-NaN, fn:sum, fn:avg, fn:max, fn:min, fn:divide-decimals and
 * fn:random-number-generator (F&O 4.0, sections 4.3, 4.4, 4.5, 4.7 and
 * 14.4).
 *
 * Golden file: src/memory.c (layout and comments); rows as in
 * src/function_library_internal.h.
 *
 * Rounding computes with decimals (D57): integers and xs:decimal values
 * exactly, xs:float and xs:double by their exact binary value (F&O 4.0,
 * fn:round: "the value is rounded as an xs:double"), converted back with
 * one rounding, so round(2.5e0) is 3 and round(2.675e0, 2) is 2.67.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

static const sxml_static_error incompatible_types_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The values are not all numbers, or not all durations of one kind, or "
        "not comparable (F&O 4.0, fn:sum, fn:avg, fn:max, fn:min)"),
};

/* -------------------------------------------------------------- Numbers */

/* The four numeric types a result can have (F&O 4.0, section 4.4: a
   derived type gives its base numeric type). */
typedef enum numeric_kind {
    SXML_NUMERIC_KIND_INTEGER,
    SXML_NUMERIC_KIND_DECIMAL,
    SXML_NUMERIC_KIND_FLOAT,
    SXML_NUMERIC_KIND_DOUBLE,
} numeric_kind;

static numeric_kind kind_of_number(const sxml_item_heap *item_heap, sxml_item item) {
    switch (sxml_atomic_item_get_primitive_type(item).index) {
    case SXML_TYPE_XS_FLOAT:
        return SXML_NUMERIC_KIND_FLOAT;
    case SXML_TYPE_XS_DOUBLE:
        return SXML_NUMERIC_KIND_DOUBLE;
    default:
        break;
    }
    return sxml_operator_item_has_type(
               item_heap, (sxml_type_test){.item = item, .type = SXML_TYPE_XS_INTEGER})
               ? SXML_NUMERIC_KIND_INTEGER
               : SXML_NUMERIC_KIND_DECIMAL;
}

/* What to do to a number. */
typedef enum numeric_operation {
    SXML_NUMERIC_OPERATION_ABSOLUTE,
    SXML_NUMERIC_OPERATION_ROUND,
} numeric_operation;

typedef struct numeric_request {
    numeric_operation operation;
    /* For SXML_NUMERIC_OPERATION_ROUND (floor and ceiling are rounding modes
       at precision 0). */
    sxml_rounding_mode rounding_mode;
    int64_t precision;
} numeric_request;

/* The decimal of an integer or decimal item, made or copied. */
static sxml_status read_decimal(const sxml_decimal_configuration *configuration,
                                sxml_item item, sxml_decimal *decimal) {
    const sxml_decimal *held = sxml_atomic_item_get_decimal(item);
    if (held != NULL) {
        return sxml_decimal_copy(configuration, held, decimal);
    }
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_INTEGER) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_decimal_from_integer(configuration, datum.datum_details.integer, decimal);
}

/* A request and the decimal (borrowed) it applies to. */
typedef struct decimal_operation {
    const numeric_request *request;
    const sxml_decimal *decimal;
} decimal_operation;

/* Applies the request to the decimal, writing a new decimal. */
static sxml_status apply_to_decimal(const sxml_decimal_configuration *configuration,
                                    decimal_operation operation, sxml_decimal *output) {
    const numeric_request *request = operation.request;
    const sxml_decimal *decimal = operation.decimal;
    if (request->operation == SXML_NUMERIC_OPERATION_ABSOLUTE) {
        return sxml_decimal_absolute(configuration, decimal, output);
    }
    return sxml_decimal_round(configuration,
                              &(sxml_decimal_rounding){.decimal = decimal,
                                                       .precision = request->precision,
                                                       .rounding_mode = request->rounding_mode},
                              output);
}

/* A number and what to do to it. */
typedef struct numeric_operand {
    sxml_item item;
    const numeric_request *request;
} numeric_operand;

/* An integer or decimal item: computed exactly (F&O 4.0, sections 4.4.1 to
   4.4.5). */
static sxml_status apply_to_exact(sxml_item_heap *item_heap, numeric_operand operand,
                                  sxml_item *result) {
    const sxml_item item = operand.item;
    const numeric_request *request = operand.request;
    const numeric_kind kind = kind_of_number(item_heap, item);
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        const int64_t integer = datum.datum_details.integer;
        if (request->operation == SXML_NUMERIC_OPERATION_ABSOLUTE && integer != INT64_MIN) {
            *result = sxml_atomic_item_from_integer(integer < 0 ? -integer : integer);
            return SXML_STATUS_OK;
        }
        if (request->operation == SXML_NUMERIC_OPERATION_ROUND && request->precision >= 0) {
            *result = sxml_atomic_item_from_integer(integer);
            return SXML_STATUS_OK;
        }
    }
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(item_heap);
    sxml_decimal decimal;
    status = read_decimal(configuration, item, &decimal);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal output;
    status = apply_to_decimal(configuration,
                              (decimal_operation){.request = request, .decimal = &decimal},
                              &output);
    sxml_decimal_finalize(&decimal);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_atomic_item_make_taking_decimal(
        item_heap,
        &(sxml_decimal_item_request){
            .type = sxml_builtin_type_identifier(kind == SXML_NUMERIC_KIND_INTEGER
                                                     ? SXML_TYPE_XS_INTEGER
                                                     : SXML_TYPE_XS_DECIMAL),
            .decimal = &output},
        result);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&output);
    }
    return status;
}

/* An xs:float or xs:double value, rounded by its exact value. */
static sxml_status apply_to_binary(sxml_item_heap *item_heap, numeric_operand operand,
                                   sxml_item *result) {
    const sxml_item item = operand.item;
    const numeric_request *request = operand.request;
    const bool is_float = kind_of_number(item_heap, item) == SXML_NUMERIC_KIND_FLOAT;
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const double value = is_float ? (double)datum.datum_details.float_number
                                  : datum.datum_details.double_number;
    double computed = value;
    if (request->operation == SXML_NUMERIC_OPERATION_ABSOLUTE) {
        /* abs(-0e0) is 0e0, abs(-INF) is INF, NaN stays NaN. */
        computed = signbit(value) ? -value : value;
    } else if (isfinite(value) && value != 0.0 &&
               !(fabs(value) >= 9007199254740992.0 && request->precision >= 0)) {
        const sxml_decimal_configuration *configuration =
            sxml_item_heap_get_decimal_configuration(item_heap);
        sxml_decimal decimal;
        /* The exact binary value is rounded (QT4 fn-round-472:
           round(1e-2, 2, 'away-from-zero') is 0.02, since 1e-2 is a little
           above 0.01). */
        status = sxml_decimal_from_double_exactly(configuration, value, &decimal);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_decimal output;
        status = apply_to_decimal(configuration,
                                  (decimal_operation){.request = request, .decimal = &decimal},
                                  &output);
        sxml_decimal_finalize(&decimal);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        computed = is_float ? (double)sxml_decimal_to_float(&output)
                            : sxml_decimal_to_double(&output);
        sxml_decimal_finalize(&output);
        /* A negative value that rounds to zero gives negative zero (F&O
           4.0, fn:round, fn:ceiling). */
        if (computed == 0.0 && value < 0.0) {
            computed = -0.0;
        }
    }
    if (is_float) {
        return sxml_atomic_item_make(
            item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_FLOAT),
                .datum = {.datum_kind = SXML_DATUM_KIND_FLOAT,
                          .datum_details = {.float_number = (float)computed}}},
            result);
    }
    *result = sxml_atomic_item_from_double(computed);
    return SXML_STATUS_OK;
}

/* Runs request on the first argument of call: empty gives empty. */
static sxml_status call_numeric(const sxml_function_call *call,
                                const numeric_request *request, sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!value.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    const numeric_kind kind = kind_of_number(call->item_heap, value.item);
    sxml_item item = {.representation = 0};
    if (kind == SXML_NUMERIC_KIND_FLOAT || kind == SXML_NUMERIC_KIND_DOUBLE) {
        status = apply_to_binary(call->item_heap,
                                 (numeric_operand){.item = value.item, .request = request}, &item);
    } else {
        status = apply_to_exact(call->item_heap,
                                (numeric_operand){.item = value.item, .request = request}, &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:abs (F&O 4.0, section 4.4.1). */
static sxml_status call_abs(const sxml_function_call *call, sxml_sequence *result) {
    return call_numeric(call,
                        &(numeric_request){.operation = SXML_NUMERIC_OPERATION_ABSOLUTE,
                                           .rounding_mode = SXML_ROUNDING_MODE_FLOOR,
                                           .precision = 0},
                        result);
}

/* fn:ceiling (F&O 4.0, section 4.4.2). */
static sxml_status call_ceiling(const sxml_function_call *call, sxml_sequence *result) {
    return call_numeric(call,
                        &(numeric_request){.operation = SXML_NUMERIC_OPERATION_ROUND,
                                           .rounding_mode = SXML_ROUNDING_MODE_CEILING,
                                           .precision = 0},
                        result);
}

/* fn:floor (F&O 4.0, section 4.4.3). */
static sxml_status call_floor(const sxml_function_call *call, sxml_sequence *result) {
    return call_numeric(call,
                        &(numeric_request){.operation = SXML_NUMERIC_OPERATION_ROUND,
                                           .rounding_mode = SXML_ROUNDING_MODE_FLOOR,
                                           .precision = 0},
                        result);
}

/* Reads $precision (xs:integer?, default 0), the argument at
   argument_index; values beyond the digit limits are clamped, which gives
   the same result. */
static sxml_status read_precision(const sxml_function_call *call, size_t argument_index,
                                  int64_t *precision) {
    sxml_optional_item argument;
    sxml_status status =
        sxml_function_library_read_optional_item(call, argument_index, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *precision = 0;
    if (!argument.has_item) {
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(argument.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const int64_t limit = 1000000;
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        const int64_t value = datum.datum_details.integer;
        *precision = value > limit ? limit : (value < -limit ? -limit : value);
    } else {
        const bool is_negative = datum.datum_details.decimal_text.byte_count > 0 &&
                                 datum.datum_details.decimal_text.bytes[0] == '-';
        *precision = is_negative ? -limit : limit;
    }
    return SXML_STATUS_OK;
}

/* The $mode values of fn:round (F&O 4.0, section 4.4.4). */
typedef struct rounding_mode_name {
    sxml_byte_view mode_name;
    sxml_rounding_mode rounding_mode;
} rounding_mode_name;

static const rounding_mode_name rounding_mode_names[] = {
    {SXML_BYTE_VIEW_INITIALIZER("floor"), SXML_ROUNDING_MODE_FLOOR},
    {SXML_BYTE_VIEW_INITIALIZER("ceiling"), SXML_ROUNDING_MODE_CEILING},
    {SXML_BYTE_VIEW_INITIALIZER("toward-zero"), SXML_ROUNDING_MODE_TOWARD_ZERO},
    {SXML_BYTE_VIEW_INITIALIZER("away-from-zero"), SXML_ROUNDING_MODE_AWAY_FROM_ZERO},
    {SXML_BYTE_VIEW_INITIALIZER("half-to-floor"), SXML_ROUNDING_MODE_HALF_TO_FLOOR},
    {SXML_BYTE_VIEW_INITIALIZER("half-to-ceiling"), SXML_ROUNDING_MODE_HALF_TO_CEILING},
    {SXML_BYTE_VIEW_INITIALIZER("half-toward-zero"), SXML_ROUNDING_MODE_HALF_TOWARD_ZERO},
    {SXML_BYTE_VIEW_INITIALIZER("half-away-from-zero"),
     SXML_ROUNDING_MODE_HALF_AWAY_FROM_ZERO},
    {SXML_BYTE_VIEW_INITIALIZER("half-to-even"), SXML_ROUNDING_MODE_HALF_TO_EVEN},
};

static const sxml_static_error unknown_rounding_mode_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The rounding mode is not one of the values fn:round accepts (F&O 4.0, "
        "section 4.4.4)"),
};

/* fn:round($value, $precision := 0, $mode := 'half-to-ceiling'). */
static sxml_status call_round(const sxml_function_call *call, sxml_sequence *result) {
    numeric_request request = {.operation = SXML_NUMERIC_OPERATION_ROUND,
                               .rounding_mode = SXML_ROUNDING_MODE_HALF_TO_CEILING,
                               .precision = 0};
    sxml_status status = read_precision(call, 1, &request.precision);
    sxml_string mode = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 2, &mode);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (mode.utf8.byte_count > 0) {
        bool is_known = false;
        for (size_t mode_index = 0;
             mode_index < sizeof(rounding_mode_names) / sizeof(rounding_mode_names[0]);
             mode_index += 1) {
            if (sxml_byte_view_equals(mode.utf8, rounding_mode_names[mode_index].mode_name)) {
                request.rounding_mode = rounding_mode_names[mode_index].rounding_mode;
                is_known = true;
            }
        }
        if (!is_known) {
            return sxml_function_library_report(call, &unknown_rounding_mode_error);
        }
    }
    return call_numeric(call, &request, result);
}

/* fn:round-half-to-even($value, $precision := 0) (F&O 4.0, section
   4.4.5). */
static sxml_status call_round_half_to_even(const sxml_function_call *call,
                                           sxml_sequence *result) {
    numeric_request request = {.operation = SXML_NUMERIC_OPERATION_ROUND,
                               .rounding_mode = SXML_ROUNDING_MODE_HALF_TO_EVEN,
                               .precision = 0};
    sxml_status status = read_precision(call, 1, &request.precision);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return call_numeric(call, &request, result);
}

/* fn:number($value := .) (F&O 4.0, section 4.5.1): the value cast to
   xs:double, NaN when it is empty or does not cast. */
static sxml_status call_number(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!value.has_item) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_double(NAN));
        return SXML_STATUS_OK;
    }
    sxml_sequence cast = sxml_sequence_empty();
    status = sxml_cast_make_value(
        call->item_heap,
        &(sxml_cast_request){
            .value = sxml_sequence_from_item(value.item),
            .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_DOUBLE),
            .allows_empty = true,
            .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
            .error_record = NULL},
        &cast);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_double(NAN));
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        *result = cast;
    }
    return status;
}

/* fn:is-NaN($value) (F&O 4.0, section 4.5.2): true only for the xs:float
   and xs:double NaN. */
static sxml_status call_is_nan(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum;
    bool is_not_a_number = false;
    if (value.has_item && sxml_atomic_item_get_datum(value.item, &datum) == SXML_STATUS_OK) {
        is_not_a_number = (datum.datum_kind == SXML_DATUM_KIND_DOUBLE &&
                           isnan(datum.datum_details.double_number)) ||
                          (datum.datum_kind == SXML_DATUM_KIND_FLOAT &&
                           isnan(datum.datum_details.float_number));
    }
    *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_not_a_number});
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Aggregates */

/* The families fn:sum and fn:avg accept (F&O 4.0, section 14.4.5). */
typedef enum aggregate_family {
    SXML_AGGREGATE_FAMILY_NONE,
    SXML_AGGREGATE_FAMILY_NUMERIC,
    SXML_AGGREGATE_FAMILY_YEAR_MONTH_DURATION,
    SXML_AGGREGATE_FAMILY_DAY_TIME_DURATION,
} aggregate_family;

static aggregate_family family_of(const sxml_item_heap *item_heap, sxml_item item) {
    if (sxml_atomic_item_is_numeric(item)) {
        return SXML_AGGREGATE_FAMILY_NUMERIC;
    }
    if (sxml_operator_item_has_type(
            item_heap, (sxml_type_test){.item = item, .type = SXML_TYPE_XS_YEAR_MONTH_DURATION})) {
        return SXML_AGGREGATE_FAMILY_YEAR_MONTH_DURATION;
    }
    if (sxml_operator_item_has_type(
            item_heap, (sxml_type_test){.item = item, .type = SXML_TYPE_XS_DAY_TIME_DURATION})) {
        return SXML_AGGREGATE_FAMILY_DAY_TIME_DURATION;
    }
    return SXML_AGGREGATE_FAMILY_NONE;
}

/* An xs:untypedAtomic item becomes xs:double (F&O 4.0, fn:sum, fn:max);
   other items are retained as they are. */
static sxml_status promote_untyped(const sxml_function_call *call, sxml_item item,
                                   sxml_item *promoted) {
    if (sxml_atomic_item_get_primitive_type(item).index != SXML_TYPE_XS_UNTYPED_ATOMIC) {
        sxml_item_retain(item);
        *promoted = item;
        return SXML_STATUS_OK;
    }
    return sxml_operator_cast_item(
        call->item_heap,
        &(sxml_operator_cast){.item = item,
                              .target_type = SXML_TYPE_XS_DOUBLE,
                              .namespace_bindings = {.namespace_bindings = NULL,
                                                     .namespace_binding_count = 0},
                              .error_record = call->error_record},
        promoted);
}

/* The running sum of fn:sum and fn:avg. */
typedef struct aggregate_sum {
    sxml_sequence total;
    aggregate_family family;
    uint64_t item_count;
} aggregate_sum;

/* Adds every item of values to *sum (owned total). */
static sxml_status add_values(const sxml_function_call *call, sxml_sequence values,
                              aggregate_sum *sum) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(values);
    for (;;) {
        sxml_iteration_step step;
        sxml_status status =
            sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!step.has_item) {
            return SXML_STATUS_OK;
        }
        sxml_item item = {.representation = 0};
        status = promote_untyped(call, step.item, &item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const aggregate_family family = family_of(call->item_heap, item);
        if (family == SXML_AGGREGATE_FAMILY_NONE ||
            (sum->item_count > 0 && family != sum->family)) {
            sxml_item_release(item);
            return sxml_function_library_report(call, &incompatible_types_error);
        }
        sum->family = family;
        sum->item_count += 1;
        if (sum->item_count == 1) {
            sum->total = sxml_sequence_from_item(item);
            continue;
        }
        sxml_sequence added = sxml_sequence_empty();
        status = sxml_operator_make_arithmetic_value(
            call->item_heap,
            &(sxml_arithmetic_request){.arithmetic_operator = SXML_ARITHMETIC_OPERATOR_ADD,
                                       .left_operand = sum->total,
                                       .right_operand = sxml_sequence_from_item(item),
                                       .implicit_timezone_offset_minutes = 0,
                                       .error_record = call->error_record},
            &added);
        sxml_item_release(item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_sequence_release(sum->total);
        sum->total = added;
    }
}

/* fn:sum($values, $zero := 0) (F&O 4.0, section 14.4.5). */
static sxml_status call_sum(const sxml_function_call *call, sxml_sequence *result) {
    aggregate_sum sum = {.total = sxml_sequence_empty(),
                         .family = SXML_AGGREGATE_FAMILY_NONE,
                         .item_count = 0};
    sxml_status status =
        add_values(call, sxml_function_library_get_argument(call, 0), &sum);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(sum.total);
        return status;
    }
    if (sum.item_count > 0) {
        *result = sum.total;
        return SXML_STATUS_OK;
    }
    if (call->arguments.sequence_count < 2) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer(0));
        return SXML_STATUS_OK;
    }
    const sxml_sequence zero = call->arguments.sequences[1];
    sxml_sequence_retain(zero);
    *result = zero;
    return SXML_STATUS_OK;
}

/* fn:avg($values) (F&O 4.0, section 14.4.1): the sum divided by the
   count. */
static sxml_status call_avg(const sxml_function_call *call, sxml_sequence *result) {
    aggregate_sum sum = {.total = sxml_sequence_empty(),
                         .family = SXML_AGGREGATE_FAMILY_NONE,
                         .item_count = 0};
    sxml_status status =
        add_values(call, sxml_function_library_get_argument(call, 0), &sum);
    if (status != SXML_STATUS_OK || sum.item_count == 0) {
        sxml_sequence_release(sum.total);
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_empty();
        }
        return status;
    }
    sxml_sequence quotient = sxml_sequence_empty();
    status = sxml_operator_make_arithmetic_value(
        call->item_heap,
        &(sxml_arithmetic_request){
            .arithmetic_operator = SXML_ARITHMETIC_OPERATOR_DIVIDE,
            .left_operand = sum.total,
            .right_operand = sxml_sequence_from_item(
                sxml_atomic_item_from_integer((int64_t)sum.item_count)),
            .implicit_timezone_offset_minutes = 0,
            .error_record = call->error_record},
        &quotient);
    sxml_sequence_release(sum.total);
    if (status == SXML_STATUS_OK) {
        *result = quotient;
    }
    return status;
}

/* Which extreme fn:max and fn:min look for. */
typedef enum extreme_kind {
    SXML_EXTREME_KIND_MAXIMUM,
    SXML_EXTREME_KIND_MINIMUM,
} extreme_kind;

static bool is_nan(sxml_item item) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK) {
        return false;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_DOUBLE) {
        return isnan(datum.datum_details.double_number);
    }
    if (datum.datum_kind == SXML_DATUM_KIND_FLOAT) {
        return isnan(datum.datum_details.float_number);
    }
    return false;
}

/* The search of fn:max or fn:min. */
typedef struct extreme_search {
    extreme_kind kind;
    /* Owned. */
    sxml_item best;
    bool has_best;
    /* The first NaN, owned: the result whenever there is one. */
    sxml_item first_nan;
    bool has_nan;
    bool has_incompatible_items;
    /* The collation strings compare under (D116). */
    sxml_collation collation;
} extreme_search;

/* Takes candidate (owned) into the search. */
static sxml_status consider(const sxml_function_call *call, extreme_search *search,
                            sxml_item candidate) {
    if (is_nan(candidate)) {
        if (!search->has_nan) {
            search->first_nan = candidate;
            search->has_nan = true;
        } else {
            sxml_item_release(candidate);
        }
        return SXML_STATUS_OK;
    }
    if (!search->has_best) {
        search->best = candidate;
        search->has_best = true;
        return SXML_STATUS_OK;
    }
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    int16_t implicit_timezone = 0;
    sxml_status status = SXML_STATUS_OK;
    if (call->dynamic_context != NULL) {
        status = sxml_dynamic_context_read_implicit_timezone(call->dynamic_context,
                                                             &implicit_timezone);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_compare_collated(
            call->item_heap,
            &(sxml_collated_comparison){
                .collation = &search->collation,
                .comparison = {.first = candidate,
                               .second = search->best,
                               .implicit_timezone_offset_minutes = implicit_timezone,
                               .error_record = NULL,
                               .name_pool = sxml_item_heap_get_name_pool(call->item_heap)}},
            &ordering);
    }
    if (status == SXML_STATUS_TYPE_ERROR) {
        search->has_incompatible_items = true;
        status = SXML_STATUS_OK;
    }
    const bool is_better = search->kind == SXML_EXTREME_KIND_MAXIMUM
                               ? ordering == SXML_ORDERING_GREATER
                               : ordering == SXML_ORDERING_LESS;
    if (status == SXML_STATUS_OK && is_better) {
        sxml_item_release(search->best);
        search->best = candidate;
    } else {
        sxml_item_release(candidate);
    }
    return status;
}

/* fn:max and fn:min (F&O 4.0, sections 14.4.3 and 14.4.4): the item
   itself, without numeric promotion, ordered by fn:compare (so durations
   and QNames order too, PR2256), strings under the collation (D116,
   through the one resolver). Items are taken in order: xs:untypedAtomic
   is cast to xs:double (FORG0001), a NaN ends the search and is the result,
   and two items fn:compare does not order are FORG0006 (QT4
   K-SeqMAXFunc-21a, -22, -23a, -24). */
static sxml_status call_extreme(const sxml_function_call *call, extreme_kind kind,
                                sxml_sequence *result) {
    sxml_collation collation;
    sxml_status status = sxml_function_library_resolve_collation(call, 1, &collation);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    extreme_search search = {.kind = kind,
                             .best = {.representation = 0},
                             .has_best = false,
                             .first_nan = {.representation = 0},
                             .has_nan = false,
                             .has_incompatible_items = false,
                             .collation = collation};
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    for (;;) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_item candidate = {.representation = 0};
        status = promote_untyped(call, step.item, &candidate);
        if (status == SXML_STATUS_OK) {
            status = consider(call, &search, candidate);
        }
        if (status != SXML_STATUS_OK || search.has_nan || search.has_incompatible_items) {
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        if (search.has_nan) {
            sxml_item_retain(search.first_nan);
            *result = sxml_sequence_from_item(search.first_nan);
        } else if (search.has_incompatible_items) {
            status = sxml_function_library_report(call, &incompatible_types_error);
        } else if (search.has_best) {
            sxml_item_retain(search.best);
            *result = sxml_sequence_from_item(search.best);
        } else {
            *result = sxml_sequence_empty();
        }
    }
    sxml_item_release(search.best);
    sxml_item_release(search.first_nan);
    return status;
}

static sxml_status call_max(const sxml_function_call *call, sxml_sequence *result) {
    return call_extreme(call, SXML_EXTREME_KIND_MAXIMUM, result);
}

static sxml_status call_min(const sxml_function_call *call, sxml_sequence *result) {
    return call_extreme(call, SXML_EXTREME_KIND_MINIMUM, result);
}

/* ------------------------------------------------- Random numbers */

/*
 * fn:random-number-generator (F&O 4.0, section 4.11.2): a record with
 * "number" (an xs:double in [0, 1)), "next" (a function giving the next
 * generator), "permute" (a function giving a random permutation of a
 * sequence) and "take" (a function giving the next $count numbers), in
 * an fn:random-number-generator-record (D133). The generator is
 * SplitMix64 (Steele, Lea and Flood, 2014):
 * its 64-bit state is kept as the captured xs:integer of the two function
 * items, so equal seeds give equal results within one heap.
 */

static uint64_t next_random_state(uint64_t state) {
    return state + UINT64_C(0x9E3779B97F4A7C15);
}

static uint64_t random_output(uint64_t state) {
    uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
    mixed = (mixed ^ (mixed >> 27U)) * UINT64_C(0x94D049BB133111EB);
    return mixed ^ (mixed >> 31U);
}

static sxml_status make_generator(const sxml_function_call *call, uint64_t state,
                                  sxml_sequence *result);

/* The state a "next" or "permute" function item captured. */
static sxml_status read_captured_state(const sxml_function_call *call, uint64_t *state) {
    sxml_function_item_description description;
    sxml_status status = sxml_function_item_describe(call->function_item, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.captured_values.sequence_count != 1) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(description.captured_values.sequences[0].content,
                                        &datum);
    if (status != SXML_STATUS_OK || datum.datum_kind != SXML_DATUM_KIND_INTEGER) {
        return status != SXML_STATUS_OK ? status : SXML_STATUS_INVALID_ARGUMENT;
    }
    *state = (uint64_t)datum.datum_details.integer;
    return SXML_STATUS_OK;
}

/* "next": the generator of the following state. */
static sxml_status call_random_next(const sxml_function_call *call,
                                    sxml_sequence *result) {
    uint64_t state = 0;
    sxml_status status = read_captured_state(call, &state);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return make_generator(call, next_random_state(state), result);
}

/* "permute": a Fisher-Yates shuffle driven by the generator's state. */
static sxml_status call_random_permute(const sxml_function_call *call,
                                       sxml_sequence *result) {
    uint64_t state = 0;
    sxml_status status = read_captured_state(call, &state);
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(input, call->error_record, &item_count);
    }
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = item_count > SIZE_MAX ? SXML_STATUS_ARITHMETIC_OVERFLOW
                                       : sxml_size_multiply((size_t)item_count + 1,
                                                            sizeof(sxml_item), &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            call->item_heap,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_item)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item *items = memory;
    for (uint64_t item_index = 0; status == SXML_STATUS_OK && item_index < item_count;
         item_index += 1) {
        status = sxml_sequence_get_item(
            input,
            (sxml_item_request){.item_index = item_index, .error_record = call->error_record},
            &items[item_index]);
    }
    for (uint64_t item_index = item_count; status == SXML_STATUS_OK && item_index > 1;
         item_index -= 1) {
        state = next_random_state(state);
        const uint64_t chosen = random_output(state) % item_index;
        const sxml_item swapped = items[item_index - 1];
        items[item_index - 1] = items[chosen];
        items[chosen] = swapped;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            call->item_heap,
            (sxml_item_view){.items = items, .item_count = (size_t)item_count}, result);
    }
    sxml_item_heap_release_bytes(call->item_heap,
                                 (sxml_heap_bytes){.memory = memory, .byte_count = byte_count});
    return status;
}

static double random_number(uint64_t state) {
    return (double)(random_output(state) >> 11U) * 0x1.0p-53;
}

/* "take": the numbers of this generator and the ones after it (F&O 4.0,
   fn:random-number-generator, the take entry). */
static sxml_status call_random_take(const sxml_function_call *call,
                                    sxml_sequence *result) {
    uint64_t state = 0;
    sxml_status status = read_captured_state(call, &state);
    sxml_optional_item count = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 0, &count);
    }
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                               .datum_details = {.integer = 0}};
    if (status == SXML_STATUS_OK && count.has_item) {
        status = sxml_atomic_item_get_datum(count.item, &datum);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* An integer beyond int64_t asks for more numbers than memory holds. */
    const int64_t number_count = datum.datum_kind == SXML_DATUM_KIND_INTEGER
                                     ? datum.datum_details.integer
                                     : (datum.datum_details.decimal_text.bytes[0] == '-'
                                            ? 0
                                            : INT64_MAX);
    sxml_sequence_builder *builder = NULL;
    status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    for (int64_t number_index = 0; status == SXML_STATUS_OK && number_index < number_count;
         number_index += 1) {
        status = sxml_sequence_builder_append_item(
            builder, sxml_atomic_item_from_double(random_number(state)));
        state = next_random_state(state);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

static const sxml_function_parameter take_parameters[] = {
    SXML_REQUIRED_PARAMETER("count", "xs:nonNegativeInteger")};

static const sxml_native_function random_take_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("random-number-generator-take"),
    .parameters = {take_parameters, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:double*"),
    .call = call_random_take,
    .resumable_body = NULL};

static const sxml_native_function random_next_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("random-number-generator-next"),
    .parameters = {NULL, 0},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("fn:random-number-generator-record"),
    .call = call_random_next,
    .resumable_body = NULL};

static const sxml_function_parameter permute_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*")};

static const sxml_native_function random_permute_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("random-number-generator-permute"),
    .parameters = {permute_parameters, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = call_random_permute,
    .resumable_body = NULL};

/* Makes an anonymous function item over row capturing the state. */
typedef struct generator_function_request {
    const sxml_native_function *row;
    uint64_t state;
    sxml_type_identifier signature;
} generator_function_request;

static sxml_status make_generator_function(const sxml_function_call *call,
                                           const generator_function_request *request,
                                           sxml_item *item) {
    const sxml_sequence captured =
        sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)request->state));
    return sxml_function_item_make(
        call->item_heap,
        &(sxml_function_item_request){
            .function_name = {.index = 0},
            .signature = request->signature,
            .function_annotations = {.function_annotations = NULL,
                                     .function_annotation_count = 0},
            .captured_values = {.sequences = &captured, .sequence_count = 1},
            .body = {.state = (void *)request->row,
                     .operations = &sxml_native_function_body_operations}},
        item);
}

/* The fields of fn:random-number-generator-record after "number", in
   record order, and the rows of their function items. */
static const sxml_native_function *const generator_function_rows[] = {
    &random_next_row, &random_permute_row, &random_take_row};

#define SXML_GENERATOR_FIELD_COUNT ((size_t)4)

/* The fn:random-number-generator-record of state (F&O 4.0, section
   4.11.1, D133): each function item's signature is the item type of its
   field, so ?next gives fn() as fn:random-number-generator-record and
   ?take checks xs:nonNegativeInteger. */
static sxml_status make_generator(const sxml_function_call *call, uint64_t state,
                                  sxml_sequence *result) {
    sxml_type_identifier record_type = {.index = SXML_TYPE_NONE};
    sxml_status status = sxml_function_library_find_record_type(
        call->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("random-number-generator-record"),
        &record_type);
    sxml_type_description description = {.variety = SXML_TYPE_VARIETY_ANY_ITEM};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_describe_type(
            sxml_item_heap_get_type_registry(call->item_heap), record_type, &description);
    }
    const sxml_record_field_view fields = description.variety_details.record_type.record_fields;
    if (status == SXML_STATUS_OK && (description.variety != SXML_TYPE_VARIETY_RECORD ||
                                     fields.record_field_count != SXML_GENERATOR_FIELD_COUNT)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence field_values[SXML_GENERATOR_FIELD_COUNT];
    field_values[0] = sxml_sequence_from_item(sxml_atomic_item_from_double(random_number(state)));
    size_t field_count = 1;
    while (status == SXML_STATUS_OK && field_count < SXML_GENERATOR_FIELD_COUNT) {
        sxml_item function_item = {.representation = 0};
        status = make_generator_function(
            call,
            &(generator_function_request){
                .row = generator_function_rows[field_count - 1],
                .state = state,
                .signature = fields.record_fields[field_count].field_type.item_type},
            &function_item);
        if (status == SXML_STATUS_OK) {
            field_values[field_count] = sxml_sequence_from_item(function_item);
            field_count += 1;
        }
    }
    sxml_item record = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_record_make(
            call->item_heap,
            &(sxml_record_request){.record_type = record_type,
                                   .field_values = {.sequences = field_values,
                                                    .sequence_count = field_count}},
            &record);
    }
    for (size_t field_index = 0; field_index < field_count; field_index += 1) {
        sxml_sequence_release(field_values[field_index]);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(record);
    }
    return status;
}

/* fn:random-number-generator($seed := ()): the seed's hash, or for no
   seed the current dateTime of the evaluation when there is a clock. */
static sxml_status call_random_number_generator(const sxml_function_call *call,
                                                sxml_sequence *result) {
    sxml_optional_item seed;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &seed);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint64_t state = UINT64_C(0x2545F4914F6CDD1D);
    if (seed.has_item) {
        state ^= sxml_atomic_item_hash(seed.item);
    } else if (call->dynamic_context != NULL) {
        sxml_clock_reading reading;
        if (sxml_dynamic_context_read_clock(call->dynamic_context, &reading) ==
            SXML_STATUS_OK) {
            const sxml_date_time now = reading.current_date_time;
            state ^= ((uint64_t)now.year << 40U) ^ ((uint64_t)now.month << 36U) ^
                     ((uint64_t)now.day << 30U) ^ ((uint64_t)now.hour << 24U) ^
                     ((uint64_t)now.minute << 18U) ^ ((uint64_t)now.second << 12U) ^
                     (uint64_t)now.nanosecond;
        }
    }
    return make_generator(call, random_output(state), result);
}

/* ------------------------------------------------------ fn:divide-decimals */

static const sxml_static_error division_by_zero_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The divisor is zero (F&O 4.0, fn:divide-decimals)"),
};

static const sxml_static_error quotient_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The quotient has more digits than the processor keeps (F&O 4.0, "
        "fn:divide-decimals)"),
};

/* Reads the xs:decimal argument at argument_index (an xs:integer is one)
   into a new decimal. */
static sxml_status read_decimal_argument(const sxml_function_call *call,
                                         size_t argument_index, sxml_decimal *decimal) {
    sxml_optional_item argument;
    sxml_status status =
        sxml_function_library_read_optional_item(call, argument_index, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Coercion to xs:decimal (exactly one) leaves an item. */
    if (!argument.has_item) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return read_decimal(sxml_item_heap_get_decimal_configuration(call->item_heap),
                        argument.item, decimal);
}

/* Makes the fn:division-record { "quotient": ..., "remainder": ... }
   (F&O 4.0, section 4.3.2), taking both decimals of division in every
   case. */
static sxml_status make_division_record(const sxml_function_call *call,
                                        sxml_decimal_division *division,
                                        sxml_sequence *result) {
    sxml_item quotient = {.representation = 0};
    sxml_status status = sxml_atomic_item_make_taking_decimal(
        call->item_heap,
        &(sxml_decimal_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL),
            .decimal = &division->quotient},
        &quotient);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&division->quotient);
    }
    sxml_item remainder = {.representation = 0};
    const sxml_status remainder_status = sxml_atomic_item_make_taking_decimal(
        call->item_heap,
        &(sxml_decimal_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL),
            .decimal = &division->remainder},
        &remainder);
    if (remainder_status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&division->remainder);
        if (status == SXML_STATUS_OK) {
            status = remainder_status;
        }
    }
    const sxml_sequence field_values[2] = {sxml_sequence_from_item(quotient),
                                           sxml_sequence_from_item(remainder)};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_record(
            call,
            &(sxml_builtin_record_request){
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("division-record"),
                .field_values = {.sequences = field_values, .sequence_count = 2}},
            result);
    }
    sxml_sequence_release(field_values[0]);
    sxml_sequence_release(field_values[1]);
    return status;
}

/* fn:divide-decimals($value, $divisor, $precision := 0) (F&O 4.0, section
   4.3.1). The result is an fn:division-record (D133). */
static sxml_status call_divide_decimals(const sxml_function_call *call,
                                        sxml_sequence *result) {
    int64_t precision = 0;
    sxml_status status = read_precision(call, 2, &precision);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal value;
    status = read_decimal_argument(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal divisor;
    status = read_decimal_argument(call, 1, &divisor);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&value);
        return status;
    }
    sxml_decimal_division division;
    status = sxml_decimal_divide_to_precision(
        sxml_item_heap_get_decimal_configuration(call->item_heap),
        &(sxml_decimal_division_request){
            .operands = &(sxml_decimal_operands){.left_operand = &value,
                                                 .right_operand = &divisor},
            .precision = precision},
        &division);
    const bool divisor_is_zero = sxml_decimal_is_zero(&divisor);
    sxml_decimal_finalize(&value);
    sxml_decimal_finalize(&divisor);
    if (status == SXML_STATUS_INVALID_ARGUMENT && divisor_is_zero) {
        return sxml_function_library_report(call, &division_by_zero_error);
    }
    if (status == SXML_STATUS_LIMIT_EXCEEDED) {
        return sxml_function_library_report(call, &quotient_overflow_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return make_division_record(call, &division, result);
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter numeric_value_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:numeric?")};
static const sxml_function_parameter round_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:numeric?"),
    SXML_OPTIONAL_PARAMETER("precision", "xs:integer?"),
    SXML_OPTIONAL_PARAMETER("mode", "xs:string?")};
static const sxml_function_parameter round_half_to_even_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:numeric?"),
    SXML_OPTIONAL_PARAMETER("precision", "xs:integer?")};
static const sxml_function_parameter number_parameters[] = {
    SXML_CONTEXT_PARAMETER("value", "xs:anyAtomicType?")};
static const sxml_function_parameter is_nan_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:anyAtomicType")};
static const sxml_function_parameter sum_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:anyAtomicType*"),
    SXML_OPTIONAL_PARAMETER("zero", "xs:anyAtomicType?")};
static const sxml_function_parameter avg_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:anyAtomicType*")};
static const sxml_function_parameter extreme_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:anyAtomicType*"),
    SXML_OPTIONAL_PARAMETER("collation", "xs:string?")};
static const sxml_function_parameter divide_decimals_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:decimal"),
    SXML_REQUIRED_PARAMETER("divisor", "xs:decimal"),
    SXML_OPTIONAL_PARAMETER("precision", "xs:integer?")};
static const sxml_function_parameter random_parameters[] = {
    SXML_OPTIONAL_PARAMETER("seed", "xs:anyAtomicType?")};

static const sxml_native_function numeric_functions[] = {
    SXML_FUNCTION_ROW("abs", numeric_value_parameters, "xs:numeric?", 0, call_abs),
    SXML_FUNCTION_ROW("ceiling", numeric_value_parameters, "xs:numeric?", 0, call_ceiling),
    SXML_FUNCTION_ROW("floor", numeric_value_parameters, "xs:numeric?", 0, call_floor),
    SXML_FUNCTION_ROW("round", round_parameters, "xs:numeric?", 0, call_round),
    SXML_FUNCTION_ROW("round-half-to-even", round_half_to_even_parameters, "xs:numeric?",
                      0, call_round_half_to_even),
    SXML_FUNCTION_ROW("number", number_parameters, "xs:double", 0, call_number),
    SXML_FUNCTION_ROW("divide-decimals", divide_decimals_parameters,
                      "fn:division-record", 0,
                      call_divide_decimals),
    SXML_FUNCTION_ROW("is-NaN", is_nan_parameters, "xs:boolean", 0, call_is_nan),
    SXML_FUNCTION_ROW("sum", sum_parameters, "xs:anyAtomicType?", 0, call_sum),
    SXML_FUNCTION_ROW("avg", avg_parameters, "xs:anyAtomicType?", 0, call_avg),
    SXML_FUNCTION_ROW("max", extreme_parameters, "xs:anyAtomicType?", 0, call_max),
    SXML_FUNCTION_ROW("min", extreme_parameters, "xs:anyAtomicType?", 0, call_min),
    SXML_FUNCTION_ROW("random-number-generator", random_parameters,
                      "fn:random-number-generator-record",
                      SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC |
                          SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
                      call_random_number_generator),
};

const sxml_native_function_view sxml_function_library_numeric =
    SXML_LIBRARY_PART(numeric_functions);
