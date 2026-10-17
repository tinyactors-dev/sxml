/*
 * The math part of the function library (packet 1.23b, D91): the
 * trigonometric and exponential functions of the math namespace (F&O 4.0,
 * section 4.10): math:pi, math:e, math:exp, math:exp10, math:log,
 * math:log10, math:pow, math:sqrt, math:sin, math:cos, math:tan,
 * math:asin, math:acos, math:atan, math:atan2, math:sinh, math:cosh and
 * math:tanh.
 *
 * Golden file: src/function_library_numeric.c (rows, double results); rows
 * as in src/function_library_internal.h.
 *
 * F&O 4.0 defines these functions by the IEEE 754-2019 operations of the
 * same names on 64-bit binary floating point values. C17's <math.h>
 * functions are those operations (Annex F), including the special cases
 * (log(0) is -INF, sqrt(-0) is -0, pow(-1, INF) is 1), so each body is the
 * C function. Arguments are already coerced to xs:double.
 */
#include "cast_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"

#include <math.h>

/* ------------------------------------------------------------- Constants */

/* math:pi() as xs:double (F&O 4.0, section 4.10.1). */
static sxml_status call_pi(const sxml_function_call *call, sxml_sequence *result) {
    (void)call;
    *result = sxml_sequence_from_item(sxml_atomic_item_from_double(3.141592653589793));
    return SXML_STATUS_OK;
}

/* math:e() as xs:double (F&O 4.0, section 4.10.2). */
static sxml_status call_e(const sxml_function_call *call, sxml_sequence *result) {
    (void)call;
    *result = sxml_sequence_from_item(sxml_atomic_item_from_double(2.718281828459045));
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Unary functions */

/* Which IEEE operation a unary function applies. */
typedef enum math_operation {
    SXML_MATH_OPERATION_EXP,
    SXML_MATH_OPERATION_EXP10,
    SXML_MATH_OPERATION_LOG,
    SXML_MATH_OPERATION_LOG10,
    SXML_MATH_OPERATION_SQRT,
    SXML_MATH_OPERATION_SIN,
    SXML_MATH_OPERATION_COS,
    SXML_MATH_OPERATION_TAN,
    SXML_MATH_OPERATION_ASIN,
    SXML_MATH_OPERATION_ACOS,
    SXML_MATH_OPERATION_ATAN,
    SXML_MATH_OPERATION_SINH,
    SXML_MATH_OPERATION_COSH,
    SXML_MATH_OPERATION_TANH,
} math_operation;

static double apply_operation(math_operation operation, double value) {
    switch (operation) {
    case SXML_MATH_OPERATION_EXP:
        return exp(value);
    case SXML_MATH_OPERATION_EXP10:
        /* IEEE exp10: pow(10, x), exact for integral x in range. */
        return pow(10.0, value);
    case SXML_MATH_OPERATION_LOG:
        return log(value);
    case SXML_MATH_OPERATION_LOG10:
        return log10(value);
    case SXML_MATH_OPERATION_SQRT:
        return sqrt(value);
    case SXML_MATH_OPERATION_SIN:
        return sin(value);
    case SXML_MATH_OPERATION_COS:
        return cos(value);
    case SXML_MATH_OPERATION_TAN:
        return tan(value);
    case SXML_MATH_OPERATION_ASIN:
        return asin(value);
    case SXML_MATH_OPERATION_ACOS:
        return acos(value);
    case SXML_MATH_OPERATION_ATAN:
        return atan(value);
    case SXML_MATH_OPERATION_SINH:
        return sinh(value);
    case SXML_MATH_OPERATION_COSH:
        return cosh(value);
    case SXML_MATH_OPERATION_TANH:
        return tanh(value);
    }
    return NAN;
}

/*
 * A function of the form math:f($value as xs:double?) as xs:double?: the
 * empty sequence for the empty sequence (F&O 4.0, section 4.10).
 */
static sxml_status call_unary(const sxml_function_call *call, math_operation operation,
                              sxml_sequence *result) {
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!value.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(value.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_DOUBLE) {
        /* Coercion to xs:double happened before the call. */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *result = sxml_sequence_from_item(sxml_atomic_item_from_double(
        apply_operation(operation, datum.datum_details.double_number)));
    return SXML_STATUS_OK;
}

#define SXML_MATH_UNARY_BODY(function_name, operation)                       \
    static sxml_status function_name(const sxml_function_call *call,          \
                                     sxml_sequence *result) {                 \
        return call_unary(call, (operation), result);                         \
    }

SXML_MATH_UNARY_BODY(call_exp, SXML_MATH_OPERATION_EXP)
SXML_MATH_UNARY_BODY(call_exp10, SXML_MATH_OPERATION_EXP10)
SXML_MATH_UNARY_BODY(call_log, SXML_MATH_OPERATION_LOG)
SXML_MATH_UNARY_BODY(call_log10, SXML_MATH_OPERATION_LOG10)
SXML_MATH_UNARY_BODY(call_sqrt, SXML_MATH_OPERATION_SQRT)
SXML_MATH_UNARY_BODY(call_sin, SXML_MATH_OPERATION_SIN)
SXML_MATH_UNARY_BODY(call_cos, SXML_MATH_OPERATION_COS)
SXML_MATH_UNARY_BODY(call_tan, SXML_MATH_OPERATION_TAN)
SXML_MATH_UNARY_BODY(call_asin, SXML_MATH_OPERATION_ASIN)
SXML_MATH_UNARY_BODY(call_acos, SXML_MATH_OPERATION_ACOS)
SXML_MATH_UNARY_BODY(call_atan, SXML_MATH_OPERATION_ATAN)
SXML_MATH_UNARY_BODY(call_sinh, SXML_MATH_OPERATION_SINH)
SXML_MATH_UNARY_BODY(call_cosh, SXML_MATH_OPERATION_COSH)
SXML_MATH_UNARY_BODY(call_tanh, SXML_MATH_OPERATION_TANH)

/* ------------------------------------------------------------- Two arguments */

/* Reads an xs:double argument (coerced, exactly one item). */
static sxml_status read_double(const sxml_function_call *call, size_t argument_index,
                               double *value) {
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum;
    status = argument.has_item ? sxml_atomic_item_get_datum(argument.item, &datum)
                               : SXML_STATUS_INVALID_ARGUMENT;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_DOUBLE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *value = datum.datum_details.double_number;
    return SXML_STATUS_OK;
}

/* math:atan2($y as xs:double, $x as xs:double) as xs:double (F&O 4.0,
   section 4.10.13): IEEE atan2. */
static sxml_status call_atan2(const sxml_function_call *call, sxml_sequence *result) {
    double y_value = 0.0;
    double x_value = 0.0;
    sxml_status status = read_double(call, 0, &y_value);
    if (status == SXML_STATUS_OK) {
        status = read_double(call, 1, &x_value);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_double(atan2(y_value, x_value)));
    }
    return status;
}

/* The exponent of math:pow: an integer (with its parity, which pown needs
   beyond 2^53) or a double. */
typedef struct power_exponent {
    bool is_integer;
    bool is_odd;
    double value;
} power_exponent;

/* Reads $y as xs:numeric: an integer stays an integer (pown); a decimal,
   float or double is cast to xs:double (pow). */
static sxml_status read_exponent(const sxml_function_call *call, power_exponent *exponent) {
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &argument);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = argument.has_item ? sxml_atomic_item_get_datum(argument.item, &datum)
                                   : SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_integer = sxml_operator_item_has_type(
        call->item_heap, (sxml_type_test){.item = argument.item, .type = SXML_TYPE_XS_INTEGER});
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        const int64_t integer = datum.datum_details.integer;
        *exponent = (power_exponent){
            .is_integer = true, .is_odd = (integer % 2) != 0, .value = (double)integer};
        return SXML_STATUS_OK;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_DOUBLE) {
        *exponent = (power_exponent){
            .is_integer = false, .is_odd = false, .value = datum.datum_details.double_number};
        return SXML_STATUS_OK;
    }
    sxml_sequence cast = sxml_sequence_empty();
    status = sxml_cast_make_value(
        call->item_heap,
        &(sxml_cast_request){
            .value = sxml_sequence_from_item(argument.item),
            .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_DOUBLE),
            .allows_empty = false,
            .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
            .error_record = call->error_record},
        &cast);
    sxml_item double_item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            cast, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
            &double_item);
    }
    sxml_atomic_datum double_datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(double_item, &double_datum);
    }
    if (status == SXML_STATUS_OK) {
        bool is_odd = false;
        if (is_integer && datum.datum_kind == SXML_DATUM_KIND_DECIMAL) {
            /* An integer beyond int64_t: its parity is its last digit's. */
            const sxml_byte_view digits = datum.datum_details.decimal_text;
            is_odd = digits.byte_count > 0 &&
                     ((digits.bytes[digits.byte_count - 1] - '0') % 2) != 0;
        }
        *exponent = (power_exponent){.is_integer = is_integer,
                                     .is_odd = is_odd,
                                     .value = double_datum.datum_details.double_number};
    }
    sxml_sequence_release(cast);
    return status;
}

/*
 * math:pow($x as xs:double?, $y as xs:numeric) as xs:double? (F&O 4.0,
 * section 4.10.9): IEEE pown for an integer $y, IEEE pow otherwise. C's
 * pow agrees with pown whenever the integer is exact as a double; beyond
 * 2^53 the magnitude is the same and the sign follows the parity.
 */
static sxml_status call_pow(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item base;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &base);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!base.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    double base_value = 0.0;
    power_exponent exponent = {.is_integer = false, .is_odd = false, .value = 0.0};
    status = read_double(call, 0, &base_value);
    if (status == SXML_STATUS_OK) {
        status = read_exponent(call, &exponent);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    double power = 0.0;
    if (exponent.is_integer) {
        /* pown(x, 0) is 1 even for NaN; pown of a negative base takes the
           sign of the parity. */
        power = exponent.value == 0.0 ? 1.0 : pow(fabs(base_value), exponent.value);
        if (signbit(base_value) && exponent.is_odd) {
            power = -power;
        }
    } else {
        power = pow(base_value, exponent.value);
    }
    *result = sxml_sequence_from_item(sxml_atomic_item_from_double(power));
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------------ Rows */

/* A row in the math namespace. */
#define SXML_MATH_ROW(name_text, parameter_array, result_text, body)          \
    {.function_namespace = SXML_NAMESPACE_MATH_FUNCTIONS,                     \
     .properties = 0,                                                         \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {(parameter_array),                                        \
                    sizeof(parameter_array) / sizeof((parameter_array)[0])},  \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                  \
     .call = (body),                                                          \
     .resumable_body = NULL}

#define SXML_MATH_NULLARY_ROW(name_text, body)                                \
    {.function_namespace = SXML_NAMESPACE_MATH_FUNCTIONS,                     \
     .properties = 0,                                                         \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {NULL, 0},                                                 \
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:double"),                  \
     .call = (body),                                                          \
     .resumable_body = NULL}

static const sxml_function_parameter value_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:double?")};
/* math:sin, math:cos and math:tan name their argument $radians (F&O 4.0,
   sections 4.10.4, 4.10.14 and 4.10.17). */
static const sxml_function_parameter radians_parameters[] = {
    SXML_REQUIRED_PARAMETER("radians", "xs:double?")};
static const sxml_function_parameter pow_parameters[] = {
    SXML_REQUIRED_PARAMETER("x", "xs:double?"), SXML_REQUIRED_PARAMETER("y", "xs:numeric")};
static const sxml_function_parameter atan2_parameters[] = {
    SXML_REQUIRED_PARAMETER("y", "xs:double"), SXML_REQUIRED_PARAMETER("x", "xs:double")};

static const sxml_native_function math_functions[] = {
    SXML_MATH_NULLARY_ROW("pi", call_pi),
    SXML_MATH_NULLARY_ROW("e", call_e),
    SXML_MATH_ROW("exp", value_parameters, "xs:double?", call_exp),
    SXML_MATH_ROW("exp10", value_parameters, "xs:double?", call_exp10),
    SXML_MATH_ROW("log", value_parameters, "xs:double?", call_log),
    SXML_MATH_ROW("log10", value_parameters, "xs:double?", call_log10),
    SXML_MATH_ROW("pow", pow_parameters, "xs:double?", call_pow),
    SXML_MATH_ROW("sqrt", value_parameters, "xs:double?", call_sqrt),
    SXML_MATH_ROW("sin", radians_parameters, "xs:double?", call_sin),
    SXML_MATH_ROW("cos", radians_parameters, "xs:double?", call_cos),
    SXML_MATH_ROW("tan", radians_parameters, "xs:double?", call_tan),
    SXML_MATH_ROW("asin", value_parameters, "xs:double?", call_asin),
    SXML_MATH_ROW("acos", value_parameters, "xs:double?", call_acos),
    SXML_MATH_ROW("atan", value_parameters, "xs:double?", call_atan),
    SXML_MATH_ROW("atan2", atan2_parameters, "xs:double", call_atan2),
    SXML_MATH_ROW("sinh", value_parameters, "xs:double?", call_sinh),
    SXML_MATH_ROW("cosh", value_parameters, "xs:double?", call_cosh),
    SXML_MATH_ROW("tanh", value_parameters, "xs:double?", call_tanh),
};

const sxml_native_function_view sxml_function_library_math = SXML_LIBRARY_PART(math_functions);
