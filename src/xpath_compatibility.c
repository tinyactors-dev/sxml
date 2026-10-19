/*
 * The conversions of XPath 1.0 compatibility mode (XPath 4.0, section
 * 2.2.1; src/xpath_compatibility_internal.h, packet 7.4, D197): internal
 * native rows that src/xpath_lower.c calls where the static context has
 * the mode. Each row is ordinary IR to the evaluator.
 *
 * Golden file: src/xslt_simple_content.c (an internal row in no function
 * table).
 */
#include "xpath_compatibility_internal.h"

#include "cast_internal.h"
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"

#include <math.h>
#include <string.h>

static const sxml_static_error no_string_value_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOTY0014"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "XPath 1.0 compatibility mode converts the argument by fn:string, and a function "
        "item, map or array has no string value (XPath 4.0, section 3.4.1)"),
};

static sxml_sequence not_a_number(void) {
    return sxml_sequence_from_item(sxml_atomic_item_from_double(NAN));
}

static sxml_builtin_type primitive_type_of(sxml_item item) {
    return (sxml_builtin_type)sxml_atomic_item_get_primitive_type(item).index;
}

/* The first item of value, borrowed; has_item false when it is empty. */
static sxml_status read_first_item(const sxml_function_call *call, sxml_sequence value,
                                   sxml_optional_item *first) {
    *first = (sxml_optional_item){.has_item = false, .item = {.representation = 0}};
    const sxml_status status = sxml_sequence_get_item(
        value, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
        &first->item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        return SXML_STATUS_OK;
    }
    first->has_item = status == SXML_STATUS_OK;
    return status;
}

static sxml_status atomize(const sxml_function_call *call, sxml_sequence value,
                           sxml_sequence *atoms) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_evaluator_atomize(
        call->dynamic_context,
        &(sxml_atomization_request){.value = value, .error_record = call->error_record}, atoms);
}

/* fn:number of an atomic item (F&O 4.0, fn:number): the item cast to
   xs:double, NaN when the cast fails. Written, owned, to *result. */
static sxml_status number_of_atomic(const sxml_function_call *call, sxml_item item,
                                    sxml_sequence *result) {
    sxml_sequence cast = sxml_sequence_empty();
    const sxml_status status = sxml_cast_make_value(
        call->item_heap,
        &(sxml_cast_request){
            .value = sxml_sequence_from_item(item),
            .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_DOUBLE),
            .allows_empty = true,
            .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
            .error_record = NULL},
        &cast);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        *result = not_a_number();
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        *result = cast;
    }
    return status;
}

/* fn:number of one item: its first atomized item, NaN for none. */
static sxml_status number_of_item(const sxml_function_call *call, sxml_item item,
                                  sxml_sequence *result) {
    if (sxml_item_is_atomic(item)) {
        return number_of_atomic(call, item, result);
    }
    sxml_sequence atoms = sxml_sequence_empty();
    sxml_status status = atomize(call, sxml_sequence_from_item(item), &atoms);
    sxml_optional_item first = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = read_first_item(call, atoms, &first);
    }
    if (status == SXML_STATUS_OK) {
        if (first.has_item) {
            status = number_of_atomic(call, first.item, result);
        } else {
            *result = not_a_number();
        }
    }
    sxml_sequence_release(atoms);
    return status;
}

/* fn:string of one item (F&O 4.0, fn:string): the string value of a
   node, an atomic item cast to xs:string. */
static sxml_status string_of_item(const sxml_function_call *call, sxml_item item,
                                  sxml_sequence *result) {
    if (sxml_item_get_kind(item) == SXML_ITEM_KIND_NODE) {
        sxml_node_reference node;
        sxml_status status = sxml_item_get_node(item, &node);
        sxml_item string_value = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_node_make_string_value(call->item_heap, node, &string_value);
        }
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(string_value);
        }
        return status;
    }
    if (!sxml_item_is_atomic(item)) {
        return sxml_function_library_report(call, &no_string_value_error);
    }
    if (sxml_item_get_type_annotation(item).index == SXML_TYPE_XS_STRING) {
        sxml_item_retain(item);
        *result = sxml_sequence_from_item(item);
        return SXML_STATUS_OK;
    }
    return sxml_cast_make_value(
        call->item_heap,
        &(sxml_cast_request){
            .value = sxml_sequence_from_item(item),
            .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .allows_empty = true,
            .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
            .error_record = call->error_record},
        result);
}

/* ------------------------------------------------------- Arguments */

/* What 3.4.1 makes of an argument: its first item, then fn:string or
   fn:number of it; an optional parameter keeps the empty sequence. */
typedef enum argument_conversion {
    SXML_ARGUMENT_CONVERSION_FIRST_ITEM,
    SXML_ARGUMENT_CONVERSION_STRING,
    SXML_ARGUMENT_CONVERSION_DOUBLE,
} argument_conversion;

typedef struct argument_rule {
    argument_conversion conversion;
    bool keeps_empty;
} argument_rule;

static sxml_status convert_argument(const sxml_function_call *call, argument_rule rule,
                                    sxml_sequence *result) {
    sxml_optional_item first = {.has_item = false};
    const sxml_status status =
        read_first_item(call, sxml_function_library_get_argument(call, 0), &first);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!first.has_item) {
        /* fn:string(()) is "", fn:number(()) NaN; () is an instance of
           xs:string? and xs:double?, so those keep it. */
        if (rule.keeps_empty || rule.conversion == SXML_ARGUMENT_CONVERSION_FIRST_ITEM) {
            *result = sxml_sequence_empty();
            return SXML_STATUS_OK;
        }
        if (rule.conversion == SXML_ARGUMENT_CONVERSION_DOUBLE) {
            *result = not_a_number();
            return SXML_STATUS_OK;
        }
        return sxml_function_library_make_string(
            call->item_heap, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, result);
    }
    switch (rule.conversion) {
    case SXML_ARGUMENT_CONVERSION_STRING:
        return string_of_item(call, first.item, result);
    case SXML_ARGUMENT_CONVERSION_DOUBLE:
        return number_of_item(call, first.item, result);
    case SXML_ARGUMENT_CONVERSION_FIRST_ITEM:
        break;
    }
    sxml_item_retain(first.item);
    *result = sxml_sequence_from_item(first.item);
    return SXML_STATUS_OK;
}

static sxml_status call_first_item(const sxml_function_call *call, sxml_sequence *result) {
    return convert_argument(
        call, (argument_rule){.conversion = SXML_ARGUMENT_CONVERSION_FIRST_ITEM, .keeps_empty = true},
        result);
}

static sxml_status call_string(const sxml_function_call *call, sxml_sequence *result) {
    return convert_argument(
        call, (argument_rule){.conversion = SXML_ARGUMENT_CONVERSION_STRING, .keeps_empty = false},
        result);
}

static sxml_status call_optional_string(const sxml_function_call *call, sxml_sequence *result) {
    return convert_argument(
        call, (argument_rule){.conversion = SXML_ARGUMENT_CONVERSION_STRING, .keeps_empty = true},
        result);
}

static sxml_status call_double(const sxml_function_call *call, sxml_sequence *result) {
    return convert_argument(
        call, (argument_rule){.conversion = SXML_ARGUMENT_CONVERSION_DOUBLE, .keeps_empty = false},
        result);
}

static sxml_status call_optional_double(const sxml_function_call *call, sxml_sequence *result) {
    return convert_argument(
        call, (argument_rule){.conversion = SXML_ARGUMENT_CONVERSION_DOUBLE, .keeps_empty = true},
        result);
}

static const sxml_function_parameter value_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "item()*"),
};

#define SXML_COMPATIBLE_ROW(name_text, result_text, body)                    \
    {.function_namespace = SXML_NAMESPACE_NONE,                              \
     .properties = 0,                                                        \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                    \
     .parameters = {value_parameters, 1},                                    \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                 \
     .call = (body),                                                         \
     .resumable_body = NULL}

const sxml_native_function sxml_xpath_compatible_first_item_function =
    SXML_COMPATIBLE_ROW("compatible-first-item", "item()?", call_first_item);
const sxml_native_function sxml_xpath_compatible_string_function =
    SXML_COMPATIBLE_ROW("compatible-string", "xs:string", call_string);
const sxml_native_function sxml_xpath_compatible_optional_string_function =
    SXML_COMPATIBLE_ROW("compatible-optional-string", "xs:string?", call_optional_string);
const sxml_native_function sxml_xpath_compatible_double_function =
    SXML_COMPATIBLE_ROW("compatible-double", "xs:double", call_double);
const sxml_native_function sxml_xpath_compatible_optional_double_function =
    SXML_COMPATIBLE_ROW("compatible-optional-double", "xs:double?", call_optional_double);

/* -------------------------------------------- Arithmetic operands */

/* XPath 4.0, 4.9, the mode true: the atomized operand's first item, NaN
   for none; xs:boolean, xs:string, xs:decimal, xs:float and
   xs:untypedAtomic become xs:double by fn:number. */
static sxml_status call_operand(const sxml_function_call *call, sxml_sequence *result) {
    sxml_sequence atoms = sxml_sequence_empty();
    sxml_status status = atomize(call, sxml_function_library_get_argument(call, 0), &atoms);
    sxml_optional_item first = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = read_first_item(call, atoms, &first);
    }
    if (status == SXML_STATUS_OK && !first.has_item) {
        *result = not_a_number();
    } else if (status == SXML_STATUS_OK) {
        const sxml_builtin_type type = primitive_type_of(first.item);
        if (type == SXML_TYPE_XS_BOOLEAN || type == SXML_TYPE_XS_STRING ||
            type == SXML_TYPE_XS_DECIMAL || type == SXML_TYPE_XS_FLOAT ||
            type == SXML_TYPE_XS_UNTYPED_ATOMIC) {
            status = number_of_atomic(call, first.item, result);
        } else {
            sxml_item_retain(first.item);
            *result = sxml_sequence_from_item(first.item);
        }
    }
    sxml_sequence_release(atoms);
    return status;
}

const sxml_native_function sxml_xpath_compatible_operand_function =
    SXML_COMPATIBLE_ROW("compatible-operand", "xs:anyAtomicType", call_operand);

/* ------------------------------------------- General comparisons */

typedef struct operator_name {
    sxml_byte_view name;
    sxml_comparison_operator comparison_operator;
} operator_name;

static const operator_name operator_names[] = {
    {SXML_BYTE_VIEW_INITIALIZER("eq"), SXML_COMPARISON_OPERATOR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("ne"), SXML_COMPARISON_OPERATOR_NOT_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("lt"), SXML_COMPARISON_OPERATOR_LESS},
    {SXML_BYTE_VIEW_INITIALIZER("le"), SXML_COMPARISON_OPERATOR_LESS_OR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("gt"), SXML_COMPARISON_OPERATOR_GREATER},
    {SXML_BYTE_VIEW_INITIALIZER("ge"), SXML_COMPARISON_OPERATOR_GREATER_OR_EQUAL},
};

/* True when value is a single atomic item of type xs:boolean. */
static sxml_status is_single_boolean(const sxml_function_call *call, sxml_sequence value,
                                     bool *is_boolean) {
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(value, call->error_record, &item_count);
    sxml_optional_item first = {.has_item = false};
    if (status == SXML_STATUS_OK && item_count == 1) {
        status = read_first_item(call, value, &first);
    }
    *is_boolean = status == SXML_STATUS_OK && first.has_item && sxml_item_is_atomic(first.item) &&
                  primitive_type_of(first.item) == SXML_TYPE_XS_BOOLEAN;
    return status;
}

/* Which atomized items a part of an operand takes. */
typedef enum operand_part {
    SXML_OPERAND_PART_ALL,
    SXML_OPERAND_PART_NUMERIC,
    SXML_OPERAND_PART_OTHER,
} operand_part;

/* The items of atoms in part, each as fn:number makes it when
   as_numbers, written, owned, to *result. */
typedef struct operand_selection {
    sxml_sequence atoms;
    operand_part part;
    bool as_numbers;
} operand_selection;

static sxml_status select_operand(const sxml_function_call *call, operand_selection selection,
                                  sxml_sequence *result) {
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(selection.atoms);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        const bool is_numeric = sxml_atomic_item_is_numeric(step.item);
        if ((selection.part == SXML_OPERAND_PART_NUMERIC && !is_numeric) ||
            (selection.part == SXML_OPERAND_PART_OTHER && is_numeric)) {
            continue;
        }
        if (!selection.as_numbers) {
            status = sxml_sequence_builder_append_item(builder, step.item);
            continue;
        }
        sxml_sequence number = sxml_sequence_empty();
        status = number_of_atomic(call, step.item, &number);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(builder, number, call->error_record);
        }
        sxml_sequence_release(number);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* One general comparison of two parts, by XPath 4.0's rules for the mode
   false; *is_true set when it holds. */
typedef struct part_comparison {
    sxml_comparison_operator comparison_operator;
    operand_selection left;
    operand_selection right;
    int16_t timezone_offset_minutes;
} part_comparison;

static sxml_status compare_parts(const sxml_function_call *call,
                                 const part_comparison *comparison, bool *is_true) {
    sxml_sequence operands[2] = {sxml_sequence_empty(), sxml_sequence_empty()};
    sxml_status status = select_operand(call, comparison->left, &operands[0]);
    if (status == SXML_STATUS_OK) {
        status = select_operand(call, comparison->right, &operands[1]);
    }
    sxml_item outcome = sxml_atomic_item_false();
    if (status == SXML_STATUS_OK) {
        status = sxml_operator_make_general_comparison(
            call->item_heap,
            &(sxml_comparison_request){
                .comparison_operator = comparison->comparison_operator,
                .left_operand = operands[0],
                .right_operand = operands[1],
                .implicit_timezone_offset_minutes = comparison->timezone_offset_minutes,
                .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
                .error_record = call->error_record},
            &outcome);
    }
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(outcome, &datum);
    }
    *is_true = status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_BOOLEAN &&
               datum.datum_details.boolean;
    sxml_sequence_release(operands[0]);
    sxml_sequence_release(operands[1]);
    return status;
}

/* The operands, after the xs:boolean rule, atomized. */
typedef struct comparison_operands {
    sxml_sequence left;
    sxml_sequence right;
} comparison_operands;

/* 4.11.2, rule 1: when one operand is a single xs:boolean, the other
   becomes its effective boolean value; then both are atomized (rule 2). */
static sxml_status prepare_operands(const sxml_function_call *call,
                                    comparison_operands *operands) {
    sxml_sequence raw[2] = {sxml_function_library_get_argument(call, 0),
                            sxml_function_library_get_argument(call, 1)};
    bool is_boolean[2] = {false, false};
    sxml_status status = is_single_boolean(call, raw[0], &is_boolean[0]);
    if (status == SXML_STATUS_OK) {
        status = is_single_boolean(call, raw[1], &is_boolean[1]);
    }
    for (size_t side_index = 0; status == SXML_STATUS_OK && side_index < 2; side_index += 1) {
        if (is_boolean[1 - side_index] && !is_boolean[side_index]) {
            sxml_effective_boolean_value value = {.is_true = false};
            status = sxml_evaluator_compute_effective_boolean_value(raw[side_index], call->error_record,
                                                                    &value);
            raw[side_index] = sxml_sequence_from_item(value.is_true ? sxml_atomic_item_true()
                                                              : sxml_atomic_item_false());
        }
    }
    if (status == SXML_STATUS_OK) {
        status = atomize(call, raw[0], &operands->left);
    }
    if (status == SXML_STATUS_OK) {
        status = atomize(call, raw[1], &operands->right);
    }
    return status;
}

/*
 * XPath 4.0, 4.11.2, the mode true. For <, <=, > and >= every item becomes
 * an xs:double by fn:number. For = and !=, a pair with a numeric item
 * compares both as fn:number makes them, and other pairs by the rules of
 * the mode false (strings, or a cast of xs:untypedAtomic): the pairs are
 * all left items with the numeric right ones, the numeric left items with
 * the other right ones, and the other left items with the other right
 * ones.
 */
static sxml_status call_comparison(const sxml_function_call *call, sxml_sequence *result) {
    sxml_string operator_text = {0};
    sxml_status status = sxml_function_library_read_string_argument(call, 2, &operator_text);
    sxml_comparison_operator comparison_operator = SXML_COMPARISON_OPERATOR_EQUAL;
    bool is_known = false;
    for (size_t name_index = 0; status == SXML_STATUS_OK &&
                                name_index < sizeof(operator_names) / sizeof(operator_names[0]);
         name_index += 1) {
        if (sxml_byte_view_equals(operator_text.utf8, operator_names[name_index].name)) {
            comparison_operator = operator_names[name_index].comparison_operator;
            is_known = true;
        }
    }
    if (status == SXML_STATUS_OK && !is_known) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    comparison_operands operands = {.left = sxml_sequence_empty(),
                                    .right = sxml_sequence_empty()};
    if (status == SXML_STATUS_OK) {
        status = prepare_operands(call, &operands);
    }
    int16_t timezone_offset_minutes = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_items(call, operands.left);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_items(call, operands.right);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_read_implicit_timezone(call->dynamic_context,
                                                             &timezone_offset_minutes);
    }
    const bool is_equality = comparison_operator == SXML_COMPARISON_OPERATOR_EQUAL ||
                             comparison_operator == SXML_COMPARISON_OPERATOR_NOT_EQUAL;
    const part_comparison comparisons[3] = {
        {.comparison_operator = comparison_operator,
         .left = {.atoms = operands.left, .part = SXML_OPERAND_PART_ALL, .as_numbers = true},
         .right = {.atoms = operands.right,
                   .part = is_equality ? SXML_OPERAND_PART_NUMERIC : SXML_OPERAND_PART_ALL,
                   .as_numbers = true},
         .timezone_offset_minutes = timezone_offset_minutes},
        {.comparison_operator = comparison_operator,
         .left = {.atoms = operands.left, .part = SXML_OPERAND_PART_NUMERIC, .as_numbers = true},
         .right = {.atoms = operands.right, .part = SXML_OPERAND_PART_OTHER, .as_numbers = true},
         .timezone_offset_minutes = timezone_offset_minutes},
        {.comparison_operator = comparison_operator,
         .left = {.atoms = operands.left, .part = SXML_OPERAND_PART_OTHER, .as_numbers = false},
         .right = {.atoms = operands.right, .part = SXML_OPERAND_PART_OTHER, .as_numbers = false},
         .timezone_offset_minutes = timezone_offset_minutes}};
    const size_t comparison_count = is_equality ? 3U : 1U;
    bool is_true = false;
    for (size_t comparison_index = 0;
         status == SXML_STATUS_OK && !is_true && comparison_index < comparison_count;
         comparison_index += 1) {
        status = compare_parts(call, &comparisons[comparison_index], &is_true);
    }
    sxml_sequence_release(operands.left);
    sxml_sequence_release(operands.right);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_true});
    }
    return status;
}

static const sxml_function_parameter comparison_parameters[] = {
    SXML_REQUIRED_PARAMETER("left", "item()*"),
    SXML_REQUIRED_PARAMETER("right", "item()*"),
    SXML_REQUIRED_PARAMETER("operator", "xs:string"),
};

const sxml_native_function sxml_xpath_compatible_comparison_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("compatible-comparison"),
    .parameters = {comparison_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
    .call = call_comparison,
    .resumable_body = NULL};
