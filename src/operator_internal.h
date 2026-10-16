#pragma once

/*
 * Arithmetic and comparison operators (XPath 4.0, section 4.9 "Arithmetic
 * Expressions" and section 4.11 "Comparison Expressions"; F&O 4.0, the op:
 * functions of sections 4.2, 4.3, 8.5 and 9.8; docs/DECISIONS.md D69).
 *
 * Users: the evaluator (BINARY and UNARY expressions of the IR), the
 * function library (fn:sum, fn:avg, fn:op) and later XQuery and XSLT,
 * which lower into the same IR.
 *
 * Operands are atomized sequences, borrowed: atomization belongs to the
 * evaluator (XPath 4.0, section 2.5.3), which hands over atomic items
 * only; a lazy sequence is read as far as needed. Results are owned.
 *
 * Files
 * -----
 * - src/operator.c: reading single operands, numeric promotion and the
 *   numeric operators (op:numeric-add to op:numeric-unary-minus), and the
 *   dispatch of arithmetic by operand types.
 * - src/operator_temporal.c: arithmetic on durations, dates and times.
 * - src/operator_compare.c: value, general and GNode comparisons.
 *
 * Errors are reported through the request's error record (which may be
 * NULL) with the codes of XPath 4.0 and F&O 4.0: XPTY0004 (an operand of
 * more than one item, operand types the operator does not accept, or
 * items that do not compare), FORG0001 (an xs:untypedAtomic operand that
 * does not cast), FOAR0001 (division by zero), FOAR0002 (numeric overflow,
 * also of the heap's decimal digit limit, and idiv of NaN or infinity),
 * FODT0001 (a date beyond the supported years), FODT0002 (duration
 * overflow), FOCA0005 (a duration and NaN). Outputs are untouched on
 * failure (D20).
 *
 * Thread safety: one thread per item heap (D35).
 */
#include "sxml/tree.h"
#include "xdm_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ----------------------------------------------------------- Arithmetic */

/* The binary arithmetic operators (XPath 4.0, section 4.9). */
typedef enum sxml_arithmetic_operator {
    /* "+": op:numeric-add, durations and dates (op:add-*). */
    SXML_ARITHMETIC_OPERATOR_ADD,
    /* "-": op:numeric-subtract, op:subtract-dates and the like. */
    SXML_ARITHMETIC_OPERATOR_SUBTRACT,
    /* "*", "×": op:numeric-multiply, op:multiply-duration-by-number. */
    SXML_ARITHMETIC_OPERATOR_MULTIPLY,
    /* "div", "÷": op:numeric-divide (xs:decimal for two integers),
       op:divide-duration-by-number, ratios of durations. */
    SXML_ARITHMETIC_OPERATOR_DIVIDE,
    /* "idiv": op:numeric-integer-divide, always xs:integer. */
    SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
    /* "mod": op:numeric-mod. */
    SXML_ARITHMETIC_OPERATOR_MODULO,
} sxml_arithmetic_operator;

/* A binary arithmetic expression to compute. */
typedef struct sxml_arithmetic_request {
    sxml_arithmetic_operator arithmetic_operator;
    /* Atomized operands, borrowed: atomic items only. */
    sxml_sequence left_operand;
    sxml_sequence right_operand;
    /* The implicit timezone of the dynamic context, minutes east of UTC,
       for subtracting dates and times without a timezone (F&O 4.0,
       op:subtract-dateTimes). */
    int16_t implicit_timezone_offset_minutes;
    /* Receives the error; may be NULL. */
    sxml_error_record *error_record;
} sxml_arithmetic_request;

/*
 * Computes request->left_operand op request->right_operand (XPath 4.0,
 * section 4.9) and writes the value, owned, to *result: the empty sequence
 * when an operand is empty, otherwise one atomic item.
 *
 * Each operand must be empty or one item (XPTY0004); xs:untypedAtomic is
 * cast to xs:double (FORG0001). Two numbers of different primitive types
 * are promoted: to xs:double if either is one, else to xs:float if either
 * is one, else both stay xs:decimal. xs:integer arithmetic stays exact:
 * results beyond int64_t become decimal cells (D36). Every other
 * combination of types is the one of the operator table of section 4.9, or
 * XPTY0004.
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR or SXML_STATUS_TYPE_ERROR with the
 * error record filled (see above), SXML_STATUS_INVALID_ARGUMENT for an
 * operand item that is not atomic, a lazy operand's failures, and the
 * heap's failures.
 */
SXML_NODISCARD sxml_status sxml_operator_make_arithmetic_value(
    sxml_item_heap *item_heap, const sxml_arithmetic_request *request,
    sxml_sequence *result);

/* A unary arithmetic expression to compute. */
typedef struct sxml_unary_arithmetic_request {
    /* The atomized operand, borrowed. */
    sxml_sequence operand;
    /* Receives the error; may be NULL. */
    sxml_error_record *error_record;
} sxml_unary_arithmetic_request;

/*
 * "+A" (op:numeric-unary-plus): the operand's value as an xs:integer,
 * xs:decimal, xs:float or xs:double, so +xs:byte(1) is xs:integer 1;
 * xs:untypedAtomic is cast to xs:double (F&O 4.0, op:numeric-unary-plus
 * note). Writes it, owned, to *result; the empty sequence for an empty
 * operand. Failures as for sxml_operator_make_arithmetic_value; a
 * non-numeric operand is XPTY0004.
 */
SXML_NODISCARD sxml_status sxml_operator_make_unary_plus_value(
    sxml_item_heap *item_heap, const sxml_unary_arithmetic_request *request,
    sxml_sequence *result);

/*
 * "-A" (op:numeric-unary-minus): the negation, -0.0e0 for 0.0e0, and an
 * exact decimal cell for -(-9223372036854775808). Otherwise as
 * sxml_operator_make_unary_plus_value.
 */
SXML_NODISCARD sxml_status sxml_operator_make_unary_minus_value(
    sxml_item_heap *item_heap, const sxml_unary_arithmetic_request *request,
    sxml_sequence *result);

/* ---------------------------------------------------------- Comparisons */

/* What a comparison tests: eq/=, ne/!=, lt/<, le/<=, gt/>, ge/>=. */
typedef enum sxml_comparison_operator {
    SXML_COMPARISON_OPERATOR_EQUAL,
    SXML_COMPARISON_OPERATOR_NOT_EQUAL,
    SXML_COMPARISON_OPERATOR_LESS,
    SXML_COMPARISON_OPERATOR_LESS_OR_EQUAL,
    SXML_COMPARISON_OPERATOR_GREATER,
    SXML_COMPARISON_OPERATOR_GREATER_OR_EQUAL,
} sxml_comparison_operator;

/* A value or general comparison to compute. */
typedef struct sxml_comparison_request {
    sxml_comparison_operator comparison_operator;
    /* Atomized operands, borrowed: atomic items only. */
    sxml_sequence left_operand;
    sxml_sequence right_operand;
    /* For date/time values without a timezone (F&O 4.0, fn:compare). */
    int16_t implicit_timezone_offset_minutes;
    /* The statically known namespaces, for a general comparison that casts
       xs:untypedAtomic to xs:QName (as sxml_cast_request); borrowed. */
    sxml_namespace_binding_view namespace_bindings;
    /* Receives the error; may be NULL. */
    sxml_error_record *error_record;
} sxml_comparison_request;

/*
 * A value comparison (XPath 4.0, section 4.11.1): the empty sequence when
 * an operand is empty, XPTY0004 when one has more than one item; for two
 * numbers of which one is NaN, false (true for ne); otherwise fn:compare
 * with the codepoint collation decides (xs:untypedAtomic compares as a
 * string, numbers by exact value, every type family ordered, PR2218 and
 * PR2256). Writes the xs:boolean or empty sequence, owned, to *result.
 *
 * Returns SXML_STATUS_TYPE_ERROR (XPTY0004) for operands of different
 * type families, SXML_STATUS_INVALID_ARGUMENT for a non-atomic item, and a
 * lazy operand's failures.
 */
SXML_NODISCARD sxml_status sxml_operator_make_value_comparison(
    sxml_item_heap *item_heap, const sxml_comparison_request *request,
    sxml_sequence *result);

/*
 * A general comparison (XPath 4.0, section 4.11.2), XPath 1.0
 * compatibility mode false: true when some item of the left operand and
 * some item of the right one have the relationship, compared as value
 * comparisons after converting xs:untypedAtomic: two such items compare as
 * strings; one is cast to the primitive type of the other item (for a
 * number, failing that to xs:double). Items are taken in order, left
 * outer, and the first error ends the comparison. Writes the xs:boolean
 * item, owned, to *result.
 *
 * Returns SXML_STATUS_TYPE_ERROR (XPTY0004) for a pair that does not
 * compare, SXML_STATUS_DYNAMIC_ERROR (FORG0001) for a cast that fails,
 * SXML_STATUS_INVALID_ARGUMENT for a non-atomic item, a lazy operand's
 * failures and the heap's.
 */
SXML_NODISCARD sxml_status sxml_operator_make_general_comparison(
    sxml_item_heap *item_heap, const sxml_comparison_request *request,
    sxml_item *result);

/* What a GNode comparison tests (XPath 4.0, section 4.11.3). */
typedef enum sxml_node_comparison_operator {
    /* "is". */
    SXML_NODE_COMPARISON_OPERATOR_IS,
    /* "is-not". */
    SXML_NODE_COMPARISON_OPERATOR_IS_NOT,
    /* "<<", "precedes". */
    SXML_NODE_COMPARISON_OPERATOR_PRECEDES,
    /* ">>", "follows". */
    SXML_NODE_COMPARISON_OPERATOR_FOLLOWS,
    SXML_NODE_COMPARISON_OPERATOR_PRECEDES_OR_IS,
    SXML_NODE_COMPARISON_OPERATOR_FOLLOWS_OR_IS,
} sxml_node_comparison_operator;

/* A GNode comparison to compute. */
typedef struct sxml_node_comparison_request {
    sxml_node_comparison_operator node_comparison_operator;
    /* The operands as evaluated (not atomized), borrowed. */
    sxml_sequence left_operand;
    sxml_sequence right_operand;
    /* Receives the error; may be NULL. */
    sxml_error_record *error_record;
} sxml_node_comparison_request;

/*
 * A GNode comparison (XPath 4.0, section 4.11.3) of XNodes or JNodes by
 * identity (sxml_generic_node_is_same_node) or document order
 * (sxml_generic_node_compare_document_order): the empty sequence when an
 * operand is empty, otherwise an xs:boolean, written, owned, to *result.
 *
 * Returns SXML_STATUS_TYPE_ERROR (XPTY0004) for an operand of more than one
 * item or an item that is not a GNode, and a lazy operand's failures.
 */
SXML_NODISCARD sxml_status sxml_operator_make_node_comparison(
    const sxml_node_comparison_request *request, sxml_sequence *result);

/* ------------------------------------------ Between src/operator*.c files */

/* The outcome of reading an operand that must be empty or one item. */
typedef struct sxml_single_operand {
    bool has_item;
    /* Borrowed from the operand sequence. */
    sxml_item item;
} sxml_single_operand;

/*
 * Reads operand: no item, or its one atomic item. Returns
 * SXML_STATUS_TYPE_ERROR (XPTY0004) for two or more items,
 * SXML_STATUS_INVALID_ARGUMENT for an item that is not atomic, and a lazy
 * sequence's failures, with error_record (may be NULL) filled.
 */
SXML_NODISCARD sxml_status sxml_operator_read_single_operand(
    sxml_sequence operand, sxml_error_record *error_record,
    sxml_single_operand *single_operand);

/* An item to cast to a built-in atomic type, as "cast as". */
typedef struct sxml_operator_cast {
    sxml_item item;
    sxml_builtin_type target_type;
    sxml_namespace_binding_view namespace_bindings;
    sxml_error_record *error_record;
} sxml_operator_cast;

/*
 * Casts cast->item to cast->target_type (src/cast_internal.h) and writes the
 * item, owned, to *item. Failures as for sxml_cast_make_value.
 */
SXML_NODISCARD sxml_status sxml_operator_cast_item(
    sxml_item_heap *item_heap, const sxml_operator_cast *cast, sxml_item *item);

/* An item and a built-in type to test its annotation against. */
typedef struct sxml_type_test {
    sxml_item item;
    sxml_builtin_type type;
} sxml_type_test;

/* True when test.item's type annotation is test.type or derived from it
   (through the heap's type registry). */
bool sxml_operator_item_has_type(const sxml_item_heap *item_heap,
                                 sxml_type_test test);

/*
 * The arithmetic of durations, dates and times (F&O 4.0, sections 8.5 and
 * 9.8) for two single atomic items, at least one of them a duration, date
 * or time; called by sxml_operator_make_arithmetic_value. Writes the item,
 * owned, to *result. Returns SXML_STATUS_TYPE_ERROR (XPTY0004) for a
 * combination the operator table of XPath 4.0, section 4.9, lacks.
 */
typedef struct sxml_temporal_arithmetic {
    sxml_arithmetic_operator arithmetic_operator;
    sxml_item left_item;
    sxml_item right_item;
    int16_t implicit_timezone_offset_minutes;
    sxml_error_record *error_record;
} sxml_temporal_arithmetic;

SXML_NODISCARD sxml_status sxml_operator_make_temporal_value(
    sxml_item_heap *item_heap, const sxml_temporal_arithmetic *arithmetic,
    sxml_item *result);
