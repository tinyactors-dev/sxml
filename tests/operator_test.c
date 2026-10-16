/*
 * Tests for the arithmetic and comparison operators
 * (src/operator_internal.h): the arithmetic and comparison lines of the
 * torture slice as unit cases, numeric promotion and integer overflow into
 * decimals, the NaN and division rules, durations, dates and times, the
 * xs:untypedAtomic rules of general comparisons (PR2218), the ordering of
 * every type family by value comparisons (PR2256), and allocation
 * failures.
 *
 * Imitates tests/memory_test.c (and the fixture of tests/cast_test.c).
 * Expected values follow XPath 4.0, sections 4.9 and 4.11, the examples of
 * F&O 4.0, sections 4.2, 8.5 and 9.8, and the QT4 test cases named at each
 * check.
 */
#include "cast_internal.h"
#include "operator_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xdm_internal.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

enum { SXML_TEST_OPERAND_CAPACITY = 64 };

typedef struct operator_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
    /* Operands made by the test, released by operator_fixture_destroy. */
    sxml_sequence made_values[SXML_TEST_OPERAND_CAPACITY];
    size_t made_value_count;
    /* The first failure of making an operand; operands made after it are
       the empty sequence. */
    sxml_status making_status;
} operator_fixture;

static sxml_status operator_fixture_create_with(sxml_allocator allocator,
                                                operator_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->making_status = SXML_STATUS_OK;
    sxml_status status = sxml_name_pool_create(
        sxml_default_allocator(),
        &(sxml_name_pool_configuration){.allocator = sxml_default_allocator(),
                                        .maximum_byte_count = 0},
        &fixture->name_pool);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_type_registry_create(sxml_default_allocator(),
                                      &(sxml_type_registry_configuration){
                                          .allocator = sxml_default_allocator(),
                                          .name_pool = fixture->name_pool,
                                          .maximum_registered_type_count = 0},
                                      &fixture->registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            sxml_default_allocator(),
            &(sxml_item_heap_configuration){.allocator = allocator,
                                            .name_pool = fixture->name_pool,
                                            .type_registry = fixture->registry,
                                            .maximum_byte_count = 0},
            &fixture->item_heap);
    }
    return status;
}

static sxml_status operator_fixture_create(operator_fixture *fixture) {
    return operator_fixture_create_with(sxml_default_allocator(), fixture);
}

static void operator_fixture_release_values(operator_fixture *fixture) {
    for (size_t value_index = 0; value_index < fixture->made_value_count;
         value_index += 1) {
        sxml_sequence_release(fixture->made_values[value_index]);
    }
    fixture->made_value_count = 0;
}

static void operator_fixture_destroy(operator_fixture *fixture) {
    operator_fixture_release_values(fixture);
    sxml_item_heap_clear_raised_error(fixture->item_heap);
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

static uint64_t live_cell_count(const operator_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
}

/* Keeps value for release at the end; the empty sequence when full. */
static sxml_sequence keep_value(operator_fixture *fixture,
                                sxml_sequence value) {
    if (fixture->made_value_count == SXML_TEST_OPERAND_CAPACITY) {
        sxml_sequence_release(value);
        fixture->making_status = SXML_STATUS_CAPACITY_EXCEEDED;
        return sxml_sequence_empty();
    }
    fixture->made_values[fixture->made_value_count] = value;
    fixture->made_value_count += 1;
    return value;
}

/* An operand: text cast to a built-in type, as the constructor function
   would make it (xs:int("-2147483648")). */
static sxml_sequence typed(operator_fixture *fixture,
                           sxml_builtin_type builtin_type, const char *text) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value_from_text(
        fixture->item_heap,
        &(sxml_text_cast_request){
            .text = sxml_byte_view_from_c_string(text),
            .target_type = sxml_builtin_type_identifier(builtin_type),
            .namespace_bindings = {.namespace_bindings = NULL,
                                   .namespace_binding_count = 0},
            .error_record = NULL},
        &value);
    if (status != SXML_STATUS_OK) {
        if (fixture->making_status == SXML_STATUS_OK) {
            fixture->making_status = status;
        }
        return sxml_sequence_empty();
    }
    return keep_value(fixture, value);
}

static sxml_sequence integer(int64_t value) {
    return sxml_sequence_from_item(sxml_atomic_item_from_integer(value));
}

static sxml_sequence double_number(double value) {
    return sxml_sequence_from_item(sxml_atomic_item_from_double(value));
}

static sxml_sequence decimal(operator_fixture *fixture, const char *text) {
    return typed(fixture, SXML_TYPE_XS_DECIMAL, text);
}

static sxml_sequence untyped(operator_fixture *fixture, const char *text) {
    return typed(fixture, SXML_TYPE_XS_UNTYPED_ATOMIC, text);
}

static sxml_sequence string(operator_fixture *fixture, const char *text) {
    return typed(fixture, SXML_TYPE_XS_STRING, text);
}

/* An xs:QName operand. */
static sxml_sequence qname(operator_fixture *fixture,
                           sxml_byte_view namespace_uri,
                           sxml_byte_view local_name) {
    sxml_namespace_uri_identifier uri = {0};
    sxml_name_identifier name = {0};
    sxml_item item;
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        fixture->name_pool, namespace_uri, &uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            fixture->name_pool,
            &(sxml_name_request){.namespace_uri = uri,
                                 .local_name = local_name},
            &name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            fixture->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum =
                    {.datum_kind = SXML_DATUM_KIND_QNAME,
                     .datum_details = {.qname = {.expanded_name = name,
                                                 .namespace_prefix = {0}}}}},
            &item);
    }
    if (status != SXML_STATUS_OK) {
        fixture->making_status = status;
        return sxml_sequence_empty();
    }
    return keep_value(fixture, sxml_sequence_from_item(item));
}

/* The sequence of the first items of values (each a single item). */
typedef struct value_list {
    sxml_sequence values[8];
    size_t value_count;
} value_list;

static sxml_sequence sequence_of(operator_fixture *fixture, value_list list) {
    sxml_item items[8];
    for (size_t value_index = 0; value_index < list.value_count;
         value_index += 1) {
        items[value_index] = list.values[value_index].content;
    }
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_sequence_make_vector(
        fixture->item_heap,
        (sxml_item_view){.items = items, .item_count = list.value_count},
        &value);
    if (status != SXML_STATUS_OK) {
        fixture->making_status = status;
        return sxml_sequence_empty();
    }
    return keep_value(fixture, value);
}

/* ------------------------------------------------------------ Outcomes */

/* What an operator gave: the status, the error code, the canonical text of
   the result ("()" for the empty sequence) and the result's annotation. */
typedef struct operator_outcome {
    sxml_status status;
    char error_code[16];
    char text[128];
    sxml_builtin_type result_type;
} operator_outcome;

static operator_outcome outcome_start(void) {
    return (operator_outcome){.status = SXML_STATUS_OK,
                              .error_code = {0},
                              .text = {0},
                              .result_type = SXML_TYPE_NONE};
}

static void outcome_set_error(operator_outcome *outcome,
                              const sxml_error_record *error_record) {
    size_t byte_count = error_record->code.local_name.byte_count;
    if (byte_count >= sizeof(outcome->error_code)) {
        byte_count = sizeof(outcome->error_code) - 1;
    }
    memcpy(outcome->error_code, error_record->code.local_name.bytes,
           byte_count);
    outcome->error_code[byte_count] = 0;
}

/* Describes result (owned) in outcome and releases it. */
static void outcome_describe(const operator_fixture *fixture,
                             operator_outcome *outcome, sxml_sequence result) {
    sxml_item item;
    sxml_status status = sxml_sequence_get_item(
        result, (sxml_item_request){.item_index = 0, .error_record = NULL},
        &item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        memcpy(outcome->text, "()", 3);
        return;
    }
    if (status == SXML_STATUS_OK) {
        sxml_canonical_text canonical;
        status = sxml_cast_write_canonical_text(
            fixture->item_heap,
            &(sxml_canonical_text_request){
                .item = item,
                .output = {.bytes = (uint8_t *)outcome->text,
                           .byte_count = sizeof(outcome->text) - 1}},
            &canonical);
        if (status == SXML_STATUS_OK) {
            memmove(outcome->text, canonical.text.bytes,
                    canonical.text.byte_count);
            outcome->text[canonical.text.byte_count] = 0;
            outcome->result_type =
                (sxml_builtin_type)sxml_item_get_type_annotation(item).index;
        }
    }
    if (status != SXML_STATUS_OK) {
        outcome->status = status;
    }
    sxml_sequence_release(result);
}

typedef struct arithmetic_case {
    sxml_arithmetic_operator arithmetic_operator;
    sxml_sequence left_operand;
    sxml_sequence right_operand;
} arithmetic_case;

/* The implicit timezone of the tests: -05:00, as in the F&O examples. */
static const int16_t implicit_timezone = -300;

static operator_outcome compute(const operator_fixture *fixture,
                                arithmetic_case test) {
    operator_outcome outcome = outcome_start();
    if (fixture->making_status != SXML_STATUS_OK) {
        outcome.status = fixture->making_status;
        return outcome;
    }
    sxml_error_record record;
    sxml_sequence result = sxml_sequence_empty();
    outcome.status = sxml_operator_make_arithmetic_value(
        fixture->item_heap,
        &(sxml_arithmetic_request){
            .arithmetic_operator = test.arithmetic_operator,
            .left_operand = test.left_operand,
            .right_operand = test.right_operand,
            .implicit_timezone_offset_minutes = implicit_timezone,
            .error_record = &record},
        &result);
    if (outcome.status == SXML_STATUS_OK) {
        outcome_describe(fixture, &outcome, result);
    } else if (sxml_status_has_error_record(outcome.status)) {
        outcome_set_error(&outcome, &record);
    }
    return outcome;
}

typedef struct unary_case {
    bool is_minus;
    sxml_sequence operand;
} unary_case;

static operator_outcome compute_unary(const operator_fixture *fixture,
                                      unary_case test) {
    operator_outcome outcome = outcome_start();
    sxml_error_record record;
    sxml_sequence result = sxml_sequence_empty();
    sxml_unary_arithmetic_request request = {.operand = test.operand,
                                             .error_record = &record};
    outcome.status = test.is_minus ? sxml_operator_make_unary_minus_value(
                                         fixture->item_heap, &request, &result)
                                   : sxml_operator_make_unary_plus_value(
                                         fixture->item_heap, &request, &result);
    if (outcome.status == SXML_STATUS_OK) {
        outcome_describe(fixture, &outcome, result);
    } else if (sxml_status_has_error_record(outcome.status)) {
        outcome_set_error(&outcome, &record);
    }
    return outcome;
}

typedef struct comparison_case {
    sxml_comparison_operator comparison_operator;
    sxml_sequence left_operand;
    sxml_sequence right_operand;
} comparison_case;

static sxml_comparison_request comparison_request(comparison_case test,
                                                  sxml_error_record *record) {
    return (sxml_comparison_request){
        .comparison_operator = test.comparison_operator,
        .left_operand = test.left_operand,
        .right_operand = test.right_operand,
        .implicit_timezone_offset_minutes = implicit_timezone,
        .namespace_bindings = {.namespace_bindings = NULL,
                               .namespace_binding_count = 0},
        .error_record = record};
}

static operator_outcome compare_values(const operator_fixture *fixture,
                                       comparison_case test) {
    operator_outcome outcome = outcome_start();
    if (fixture->making_status != SXML_STATUS_OK) {
        outcome.status = fixture->making_status;
        return outcome;
    }
    sxml_error_record record;
    sxml_comparison_request request = comparison_request(test, &record);
    sxml_sequence result = sxml_sequence_empty();
    outcome.status = sxml_operator_make_value_comparison(fixture->item_heap,
                                                         &request, &result);
    if (outcome.status == SXML_STATUS_OK) {
        outcome_describe(fixture, &outcome, result);
    } else if (sxml_status_has_error_record(outcome.status)) {
        outcome_set_error(&outcome, &record);
    }
    return outcome;
}

static operator_outcome compare_generally(const operator_fixture *fixture,
                                          comparison_case test) {
    operator_outcome outcome = outcome_start();
    if (fixture->making_status != SXML_STATUS_OK) {
        outcome.status = fixture->making_status;
        return outcome;
    }
    sxml_error_record record;
    sxml_comparison_request request = comparison_request(test, &record);
    sxml_item result;
    outcome.status = sxml_operator_make_general_comparison(fixture->item_heap,
                                                           &request, &result);
    if (outcome.status == SXML_STATUS_OK) {
        outcome_describe(fixture, &outcome, sxml_sequence_from_item(result));
    } else if (sxml_status_has_error_record(outcome.status)) {
        outcome_set_error(&outcome, &record);
    }
    return outcome;
}

static bool gives(operator_outcome outcome, const char *text,
                  sxml_builtin_type result_type) {
    return outcome.status == SXML_STATUS_OK &&
           strcmp(outcome.text, text) == 0 &&
           (result_type == SXML_TYPE_NONE ||
            outcome.result_type == result_type);
}

static bool is_true(operator_outcome outcome) {
    return gives(outcome, "true", SXML_TYPE_XS_BOOLEAN);
}

static bool is_false(operator_outcome outcome) {
    return gives(outcome, "false", SXML_TYPE_XS_BOOLEAN);
}

static bool is_empty(operator_outcome outcome) {
    return gives(outcome, "()", SXML_TYPE_NONE);
}

static bool raises(operator_outcome outcome, const char *code) {
    return sxml_status_has_error_record(outcome.status) &&
           strcmp(outcome.error_code, code) == 0;
}

static arithmetic_case arithmetic(sxml_arithmetic_operator arithmetic_operator,
                                  sxml_sequence left, sxml_sequence right) {
    return (arithmetic_case){.arithmetic_operator = arithmetic_operator,
                             .left_operand = left,
                             .right_operand = right};
}

static comparison_case comparison(sxml_comparison_operator comparison_operator,
                                  sxml_sequence left, sxml_sequence right) {
    return (comparison_case){.comparison_operator = comparison_operator,
                             .left_operand = left,
                             .right_operand = right};
}

/* ------------------------------------------------- Slice lines: numbers */

static void test_slice_arithmetic_lines(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    /* op-numeric-add K-NumericAdd-29: 1+1 eq 2. */
    operator_outcome sum =
        compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD, integer(1),
                                     integer(1)));
    SXML_TEST_EXPECT(state, gives(sum, "2", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state, is_true(compare_values(&fixture,
                                      comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                                 integer(2), integer(2)))));
    /* op-numeric-addint2args-1: xs:int + xs:int is an xs:integer beyond
       the xs:int range. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(
                  &fixture,
                  arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                             typed(&fixture, SXML_TYPE_XS_INT, "-1873914410"),
                             typed(&fixture, SXML_TYPE_XS_INT, "-273569238"))),
              "-2147483648", SXML_TYPE_XS_INTEGER));
    /* op-numeric-divide K-NumericDivide-24: float div float is xs:float. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                 typed(&fixture, SXML_TYPE_XS_FLOAT, "6"),
                                 typed(&fixture, SXML_TYPE_XS_FLOAT, "2"))),
              "3", SXML_TYPE_XS_FLOAT));
    /* op-numeric-divideint2args-1: integer div integer is xs:decimal. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(
                  &fixture,
                  arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                             typed(&fixture, SXML_TYPE_XS_INT, "-2147483648"),
                             typed(&fixture, SXML_TYPE_XS_INT, "-2147483648"))),
              "1", SXML_TYPE_XS_DECIMAL));
    /* op-numeric-dividepint2args-5: the second allowed result (D57). */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(
                          SXML_ARITHMETIC_OPERATOR_DIVIDE,
                          typed(&fixture, SXML_TYPE_XS_POSITIVE_INTEGER, "1"),
                          typed(&fixture, SXML_TYPE_XS_POSITIVE_INTEGER,
                                "999999999999999999"))),
              "0.000000000000000001000000000000000001", SXML_TYPE_XS_DECIMAL));
    /* op-numeric-integer-divide K-NumericIntegerDivide-17: double idiv
       double is xs:integer. */
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                        double_number(6), double_number(2))),
                     "3", SXML_TYPE_XS_INTEGER));
    /* op-numeric-integer-divideint2args-1. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(
                  &fixture,
                  arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                             typed(&fixture, SXML_TYPE_XS_INT, "-2147483648"),
                             typed(&fixture, SXML_TYPE_XS_INT, "-2147483648"))),
              "1", SXML_TYPE_XS_INTEGER));
    /* op-numeric-modint2args-1: the sign of the dividend. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(
                  &fixture,
                  arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                             typed(&fixture, SXML_TYPE_XS_INT, "-2147483648"),
                             typed(&fixture, SXML_TYPE_XS_INT, "2147483647"))),
              "-1", SXML_TYPE_XS_INTEGER));
    /* op-numeric-mod K2-NumericMod-9: NaN propagates, as xs:float. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                 typed(&fixture, SXML_TYPE_XS_FLOAT, "NaN"),
                                 integer(3))),
              "NaN", SXML_TYPE_XS_FLOAT));
    /* op-numeric-multiply K-NumericMultiply-2: decimal * decimal eq 12. */
    operator_outcome product = compute(
        &fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                             decimal(&fixture, "6"), decimal(&fixture, "2")));
    SXML_TEST_EXPECT(state, gives(product, "12", SXML_TYPE_XS_DECIMAL));
    SXML_TEST_EXPECT(
        state,
        is_true(compare_values(
            &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                 decimal(&fixture, "12"), integer(12)))));
    /* op-numeric-multiplydec2args-1: 18 digits stay exact. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                 decimal(&fixture, "-1.0000000000"),
                                 decimal(&fixture, "-999999999999999999"))),
              "999999999999999999", SXML_TYPE_XS_DECIMAL));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

/* ------------------------------------------- Slice lines: comparisons */

/* A generator that raises FOER0000 for its first item, as (error(), 3)
   does when its operand is read (K-ValCompTypeChecking-40). */
static sxml_status
raising_generator_next(void *state, const sxml_generation_request *request,
                       sxml_iteration_step *step) {
    (void)state;
    (void)step;
    static const sxml_static_error error = {
        .error_status = SXML_STATUS_DYNAMIC_ERROR,
        .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOER0000"),
        .description = SXML_BYTE_VIEW_INITIALIZER("fn:error()"),
    };
    return sxml_error_record_report(request->error_record, &error);
}

static const sxml_sequence_generator_operations raising_generator_operations = {
    .next = raising_generator_next,
    .release = NULL,
};

static void test_slice_comparison_lines(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    /* op-numeric-equal op-numeric-equalint2args-1. */
    SXML_TEST_EXPECT(
        state,
        is_true(compare_values(
            &fixture,
            comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                       typed(&fixture, SXML_TYPE_XS_INT, "-2147483648"),
                       typed(&fixture, SXML_TYPE_XS_INT, "-2147483648")))));
    /* op-numeric-equalulng2args-3. */
    SXML_TEST_EXPECT(
        state,
        is_false(compare_values(
            &fixture,
            comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                       typed(&fixture, SXML_TYPE_XS_UNSIGNED_LONG,
                             "184467440737095516"),
                       typed(&fixture, SXML_TYPE_XS_UNSIGNED_LONG, "0")))));
    /* op-string-equal K-StringEqual-1. */
    SXML_TEST_EXPECT(
        state, is_true(compare_values(&fixture,
                                      comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                                 string(&fixture, "equals"),
                                                 string(&fixture, "equals")))));
    /* K-ValCompTypeChecking-1: (1, 2, 3) eq 3 is XPTY0004. */
    sxml_sequence one_two_three = sequence_of(
        &fixture, (value_list){{integer(1), integer(2), integer(3)}, 3});
    SXML_TEST_EXPECT(
        state, raises(compare_values(&fixture,
                                     comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                                one_two_three, integer(3))),
                      "XPTY0004"));
    /* K-ValCompTypeChecking-40: the operand's error comes first. */
    sxml_sequence raising = sxml_sequence_empty();
    SXML_TEST_REQUIRE(
        state,
        sxml_sequence_make_lazy(
            fixture.item_heap,
            (sxml_sequence_generator){
                .state = NULL, .operations = &raising_generator_operations},
            &raising) == SXML_STATUS_OK);
    raising = keep_value(&fixture, raising);
    SXML_TEST_EXPECT(
        state, raises(compare_values(&fixture,
                                     comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                                raising, integer(3))),
                      "FOER0000"));
    /* value-comp-ne-string-15: an empty operand gives the empty
       sequence, not false. */
    SXML_TEST_EXPECT(
        state, is_empty(compare_values(
                   &fixture,
                   comparison(SXML_COMPARISON_OPERATOR_NOT_EQUAL,
                              sxml_sequence_empty(), string(&fixture, "")))));
    /* generalexpression1 and generalexpression199: () = (), () < (). */
    SXML_TEST_EXPECT(state,
                     is_false(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              sxml_sequence_empty(),
                                              sxml_sequence_empty()))));
    SXML_TEST_EXPECT(state,
                     is_false(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_LESS,
                                              sxml_sequence_empty(),
                                              sxml_sequence_empty()))));
    /* K-GenCompEq-40: 2 = (1, 2, 3). */
    SXML_TEST_EXPECT(state,
                     is_true(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              integer(2), one_two_three))));
    /* generalexpression92: untyped hours from two documents = 50000. */
    sxml_sequence hours = sequence_of(
        &fixture,
        (value_list){{untyped(&fixture, "40"), untyped(&fixture, "20")}, 2});
    SXML_TEST_EXPECT(state,
                     is_false(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              hours, integer(50000)))));
    /* generalexpression281: pairwise under <. */
    sxml_sequence limits = sequence_of(
        &fixture, (value_list){{integer(10000), integer(50000)}, 2});
    SXML_TEST_EXPECT(
        state, is_true(compare_generally(
                   &fixture,
                   comparison(SXML_COMPARISON_OPERATOR_LESS, hours, limits))));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Numeric arithmetic */

static void test_integers_overflow_into_decimals(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                           integer(INT64_MAX), integer(1))),
              "9223372036854775808", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           integer(INT64_MIN), integer(1))),
              "-9223372036854775809", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                 integer(4294967296), integer(4294967296))),
              "18446744073709551616", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                        integer(INT64_MIN), integer(-1))),
                     "9223372036854775808", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                           integer(INT64_MIN), integer(-1))),
              "0", SXML_TYPE_XS_INTEGER));
    /* Back into int64_t: the result is inline again. */
    sxml_sequence big =
        typed(&fixture, SXML_TYPE_XS_INTEGER, "9223372036854775808");
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           big, integer(1))),
              "9223372036854775807", SXML_TYPE_XS_INTEGER));
    /* F&O 4.0, op:numeric-integer-divide examples. */
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                        integer(10), integer(3))),
                     "3", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                        integer(3), integer(-2))),
                     "-1", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                        decimal(&fixture, "-3.5"), integer(3))),
                     "-1", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                        double_number(31), integer(7))),
                     "4", SXML_TYPE_XS_INTEGER));
    /* op:numeric-mod examples. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                           decimal(&fixture, "4.5"),
                                           decimal(&fixture, "1.2"))),
              "0.9", SXML_TYPE_XS_DECIMAL));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                        double_number(123), double_number(6))),
                     "3", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                           integer(6), integer(-2))),
              "0", SXML_TYPE_XS_INTEGER));
    /* -3 div 2 is the decimal -1.5 (XPath 4.0, section 4.9). */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                           integer(-3), integer(2))),
              "-1.5", SXML_TYPE_XS_DECIMAL));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

static void test_numbers_promote_to_a_common_type(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                           integer(1), double_number(1.5))),
              "2.5", SXML_TYPE_XS_DOUBLE));
    sxml_sequence one_and_a_half = typed(&fixture, SXML_TYPE_XS_FLOAT, "1.5");
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                                  integer(1), one_and_a_half)),
                     "2.5", SXML_TYPE_XS_FLOAT));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                                  decimal(&fixture, "0.25"),
                                                  one_and_a_half)),
                     "1.75", SXML_TYPE_XS_FLOAT));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                           one_and_a_half, double_number(2))),
              "3", SXML_TYPE_XS_DOUBLE));
    /* An xs:float quotient is rounded once to float. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                 typed(&fixture, SXML_TYPE_XS_FLOAT, "6"),
                                 typed(&fixture, SXML_TYPE_XS_FLOAT, "7"))),
              "0.85714287", SXML_TYPE_XS_FLOAT));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                        integer(1), decimal(&fixture, "0.5"))),
                     "1.5", SXML_TYPE_XS_DECIMAL));
    /* xs:untypedAtomic becomes xs:double; text that is no number is
       FORG0001; a string is XPTY0004. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                           untyped(&fixture, "3"), integer(1))),
              "4", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state, raises(compute(&fixture,
                              arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                         untyped(&fixture, "x"), integer(1))),
                      "FORG0001"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                            string(&fixture, "3"), integer(1))),
               "XPTY0004"));
    SXML_TEST_EXPECT(
        state,
        raises(
            compute(&fixture,
                    arithmetic(SXML_ARITHMETIC_OPERATOR_ADD, integer(1),
                               typed(&fixture, SXML_TYPE_XS_BOOLEAN, "true"))),
            "XPTY0004"));
    /* Empty operands give the empty sequence; two items are XPTY0004. */
    SXML_TEST_EXPECT(
        state, is_empty(compute(
                   &fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                        sxml_sequence_empty(), integer(1)))));
    SXML_TEST_EXPECT(
        state, is_empty(compute(
                   &fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                        integer(1), sxml_sequence_empty()))));
    sxml_sequence pair =
        sequence_of(&fixture, (value_list){{integer(1), integer(2)}, 2});
    SXML_TEST_EXPECT(
        state, raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                                   integer(1), pair)),
                      "XPTY0004"));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

static void test_division_and_special_values(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    /* FOAR0001 for integer and decimal division by zero. */
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                            integer(1), integer(0))),
               "FOAR0001"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture,
                       arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                  integer(1), integer(0))),
               "FOAR0001"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                            integer(1), integer(0))),
               "FOAR0001"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                            decimal(&fixture, "1.5"),
                                            decimal(&fixture, "0.0"))),
               "FOAR0001"));
    /* Doubles follow IEEE 754 (F&O 4.0, op:numeric-divide). */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                           double_number(1), integer(0))),
              "INF", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                           double_number(-1), integer(0))),
              "-INF", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                           double_number(0), integer(0))),
              "NaN", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                           double_number(1e308), integer(10))),
              "INF", SXML_TYPE_XS_DOUBLE));
    /* idiv: FOAR0001 for a zero divisor, FOAR0002 for NaN or an infinite
       dividend, 0 for an infinite divisor. */
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture,
                       arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                  double_number(1), double_number(0))),
               "FOAR0001"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture,
                       arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                  double_number(NAN), double_number(1))),
               "FOAR0002"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture,
                       arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                  double_number(INFINITY), double_number(1))),
               "FOAR0002"));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                 double_number(5), double_number(INFINITY))),
              "0", SXML_TYPE_XS_INTEGER));
    /* Large idiv results are exact xs:integer cells. */
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                        double_number(1e20), double_number(1))),
                     "100000000000000000000", SXML_TYPE_XS_INTEGER));
    /* mod of doubles is the exact remainder (IEEE fmod). */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                 double_number(1e300), double_number(7))),
              "1", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                 double_number(4.5), double_number(1.2))),
              "0.9000000000000001", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                        double_number(-7.5), double_number(2))),
                     "-1.5", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                        double_number(-4), double_number(2))),
                     "-0", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                 double_number(5), double_number(INFINITY))),
              "5", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                           double_number(5), double_number(0))),
              "NaN", SXML_TYPE_XS_DOUBLE));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

static void test_decimal_digit_limit_is_overflow(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    /* 10^10000 - 1 has the 10000 digits of D57; one more is too many.
       (Addition, not multiplication: libmpdec's large products overflow
       the 64 KiB WebAssembly stack, D72.) */
    static char digits[10001];
    memset(digits, '9', 10000);
    digits[10000] = 0;
    sxml_sequence largest = typed(&fixture, SXML_TYPE_XS_INTEGER, digits);
    SXML_TEST_EXPECT(
        state, raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                                   largest, integer(1))),
                      "FOAR0002"));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           largest, largest)),
              "0", SXML_TYPE_XS_INTEGER));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

static void test_unary_plus_and_minus(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, gives(compute_unary(
                         &fixture,
                         (unary_case){.is_minus = false,
                                      .operand = typed(
                                          &fixture, SXML_TYPE_XS_BYTE, "7")}),
                     "7", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state,
        gives(compute_unary(&fixture,
                            (unary_case){.is_minus = false,
                                         .operand = untyped(&fixture, "2")}),
              "2", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state,
        gives(compute_unary(&fixture,
                            (unary_case){.is_minus = true,
                                         .operand = decimal(&fixture, "1.25")}),
              "-1.25", SXML_TYPE_XS_DECIMAL));
    SXML_TEST_EXPECT(
        state, gives(compute_unary(&fixture,
                                   (unary_case){.is_minus = true,
                                                .operand = integer(INT64_MIN)}),
                     "9223372036854775808", SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(
        state, gives(compute_unary(&fixture,
                                   (unary_case){.is_minus = true,
                                                .operand = double_number(0)}),
                     "-0", SXML_TYPE_XS_DOUBLE));
    SXML_TEST_EXPECT(
        state,
        gives(compute_unary(
                  &fixture,
                  (unary_case){.is_minus = true,
                               .operand =
                                   typed(&fixture, SXML_TYPE_XS_FLOAT, "1.5")}),
              "-1.5", SXML_TYPE_XS_FLOAT));
    SXML_TEST_EXPECT(
        state, is_empty(compute_unary(
                   &fixture, (unary_case){.is_minus = true,
                                          .operand = sxml_sequence_empty()})));
    SXML_TEST_EXPECT(
        state,
        raises(compute_unary(&fixture,
                             (unary_case){.is_minus = true,
                                          .operand = string(&fixture, "1")}),
               "XPTY0004"));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

/* ----------------------------------------------------------- Durations */

static sxml_sequence year_month(operator_fixture *fixture, const char *text) {
    return typed(fixture, SXML_TYPE_XS_YEAR_MONTH_DURATION, text);
}

static sxml_sequence day_time(operator_fixture *fixture, const char *text) {
    return typed(fixture, SXML_TYPE_XS_DAY_TIME_DURATION, text);
}

static void test_duration_arithmetic(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    /* F&O 4.0, 8.5 examples. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                           year_month(&fixture, "P2Y11M"),
                                           year_month(&fixture, "P3Y3M"))),
              "P6Y2M", SXML_TYPE_XS_YEAR_MONTH_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           year_month(&fixture, "P2Y11M"),
                                           year_month(&fixture, "P3Y3M"))),
              "-P4M", SXML_TYPE_XS_YEAR_MONTH_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                           day_time(&fixture, "P2DT12H5M"),
                                           day_time(&fixture, "P5DT12H"))),
              "P8DT5M", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           day_time(&fixture, "P2DT12H"),
                                           day_time(&fixture, "P1DT10H30M"))),
              "P1DT1H30M", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           day_time(&fixture, "PT0.5S"),
                                           day_time(&fixture, "PT1.25S"))),
              "-PT0.75S", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                           year_month(&fixture, "P2Y11M"),
                                           decimal(&fixture, "2.3"))),
              "P6Y9M", SXML_TYPE_XS_YEAR_MONTH_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                           double_number(2.1),
                                           day_time(&fixture, "PT2H10M"))),
              "PT4H33M", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                           year_month(&fixture, "P2Y11M"),
                                           decimal(&fixture, "1.5"))),
              "P1Y11M", SXML_TYPE_XS_YEAR_MONTH_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                           day_time(&fixture, "P1DT2H30M10.5S"),
                                           decimal(&fixture, "1.5"))),
              "PT17H40M7S", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                           year_month(&fixture, "P3Y4M"),
                                           year_month(&fixture, "-P1Y4M"))),
              "-2.5", SXML_TYPE_XS_DECIMAL));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                           day_time(&fixture, "P2DT53M11S"),
                                           day_time(&fixture, "P1DT10H"))),
              "1.437834967320261437908496732026144", SXML_TYPE_XS_DECIMAL));
    /* Any xs:duration times a number (PR2628): months and seconds rounded
       apart, P1M1D * 0.5 is P1MT12H. */
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                        typed(&fixture, SXML_TYPE_XS_DURATION,
                                              "P1M1D"),
                                        decimal(&fixture, "0.5"))),
                     "P1MT12H", SXML_TYPE_XS_DURATION));
    /* NaN is FOCA0005, an infinite factor or a zero divisor FODT0002, an
       infinite divisor a zero-length duration. */
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                            year_month(&fixture, "P3Y3M"),
                                            double_number(NAN))),
               "FOCA0005"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                            day_time(&fixture, "P3D"),
                                            double_number(-INFINITY))),
               "FODT0002"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture,
                       arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                  day_time(&fixture, "P3D"), integer(0))),
               "FODT0002"));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                           year_month(&fixture, "P3Y36M"),
                                           double_number(INFINITY))),
              "P0M", SXML_TYPE_XS_YEAR_MONTH_DURATION));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE,
                                            year_month(&fixture, "P1Y"),
                                            year_month(&fixture, "P0M"))),
               "FOAR0001"));
    /* PT0.001S * 60000 * 60 is PT1H (K-DayTimeDurationMultiply-*). */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                                           day_time(&fixture, "PT0.001S"),
                                           integer(3600000))),
              "PT1H", SXML_TYPE_XS_DAY_TIME_DURATION));
    /* Combinations the operator table lacks. */
    SXML_TEST_EXPECT(
        state, raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                                   year_month(&fixture, "P1Y"),
                                                   day_time(&fixture, "P1D"))),
                      "XPTY0004"));
    SXML_TEST_EXPECT(
        state,
        raises(
            compute(&fixture,
                    arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                               typed(&fixture, SXML_TYPE_XS_DURATION, "P1Y"),
                               typed(&fixture, SXML_TYPE_XS_DURATION, "P1Y"))),
            "XPTY0004"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture,
                       arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                  day_time(&fixture, "P1D"), integer(2))),
               "XPTY0004"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture,
                       arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE, integer(2),
                                  day_time(&fixture, "P1D"))),
               "XPTY0004"));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

/* ------------------------------------------------------------- Dates */

static sxml_sequence date_time(operator_fixture *fixture, const char *text) {
    return typed(fixture, SXML_TYPE_XS_DATE_TIME, text);
}

static sxml_sequence date(operator_fixture *fixture, const char *text) {
    return typed(fixture, SXML_TYPE_XS_DATE, text);
}

static sxml_sequence time_of_day(operator_fixture *fixture, const char *text) {
    return typed(fixture, SXML_TYPE_XS_TIME, text);
}

static void test_date_and_time_arithmetic(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    /* F&O 4.0, 9.8 examples (implicit timezone -05:00). */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                 date_time(&fixture, "2000-10-30T06:12:00"),
                                 date_time(&fixture, "1999-11-28T09:00:00Z"))),
              "P337DT2H12M", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           date(&fixture, "2000-10-30"),
                                           date(&fixture, "1999-11-28"))),
              "P337D", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           date(&fixture, "2000-10-15-05:00"),
                                           date(&fixture, "2000-10-10+02:00"))),
              "P5DT7H", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                 time_of_day(&fixture, "11:12:00Z"),
                                 time_of_day(&fixture, "04:00:00-05:00"))),
              "PT2H12M", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                 time_of_day(&fixture, "24:00:00+01:00"),
                                 time_of_day(&fixture, "23:59:59.5+01:00"))),
              "-PT23H59M59.5S", SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                 date_time(&fixture, "2000-10-30T11:12:00"),
                                 year_month(&fixture, "P1Y2M"))),
              "2001-12-30T11:12:00", SXML_TYPE_XS_DATE_TIME));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                 day_time(&fixture, "P3DT1H15M"),
                                 date_time(&fixture, "2000-10-30T11:12:00"))),
              "2000-11-02T12:27:00", SXML_TYPE_XS_DATE_TIME));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                 date_time(&fixture, "2000-10-30T11:12:00"),
                                 year_month(&fixture, "P1Y2M"))),
              "1999-08-30T11:12:00", SXML_TYPE_XS_DATE_TIME));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                 date_time(&fixture, "2000-10-30T11:12:00"),
                                 day_time(&fixture, "P3DT1H15M"))),
              "2000-10-27T09:57:00", SXML_TYPE_XS_DATE_TIME));
    /* The day of a shorter month is its last day. */
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                                  date(&fixture, "2000-01-31"),
                                                  year_month(&fixture, "P1M"))),
                     "2000-02-29", SXML_TYPE_XS_DATE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           date(&fixture, "2000-02-29Z"),
                                           year_month(&fixture, "P1Y"))),
              "1999-02-28Z", SXML_TYPE_XS_DATE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                           date(&fixture, "2004-10-30Z"),
                                           day_time(&fixture, "P2DT2H30M0S"))),
              "2004-11-01Z", SXML_TYPE_XS_DATE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           date(&fixture, "2000-10-30"),
                                           day_time(&fixture, "P3DT1H15M"))),
              "2000-10-26", SXML_TYPE_XS_DATE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                           time_of_day(&fixture, "11:12:00"),
                                           day_time(&fixture, "P3DT1H15M"))),
              "12:27:00", SXML_TYPE_XS_TIME));
    SXML_TEST_EXPECT(
        state, gives(compute(&fixture,
                             arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                        time_of_day(&fixture, "08:20:00-05:00"),
                                        day_time(&fixture, "P23DT10H10M"))),
                     "22:10:00-05:00", SXML_TYPE_XS_TIME));
    /* Year 0 is 1 BCE, a leap year (XSD 1.1 numbering). */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           date(&fixture, "0000-03-01"),
                                           day_time(&fixture, "P1D"))),
              "0000-02-29", SXML_TYPE_XS_DATE));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           date(&fixture, "0001-01-01"),
                                           date(&fixture, "-0001-01-01"))),
              "P731D", SXML_TYPE_XS_DAY_TIME_DURATION));
    /* Fractions of seconds carry across midnight and the year. */
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture,
                      arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                 date_time(&fixture, "1999-12-31T23:59:59.75"),
                                 day_time(&fixture, "PT0.5S"))),
              "2000-01-01T00:00:00.25", SXML_TYPE_XS_DATE_TIME));
    /* Combinations the operator table lacks. */
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                            date(&fixture, "2000-01-01"),
                                            date(&fixture, "2000-01-01"))),
               "XPTY0004"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture,
                       arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                  date(&fixture, "2000-01-01"),
                                  date_time(&fixture, "2000-01-01T00:00:00"))),
               "XPTY0004"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                            time_of_day(&fixture, "10:00:00"),
                                            year_month(&fixture, "P1Y"))),
               "XPTY0004"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                            year_month(&fixture, "P1Y"),
                                            date(&fixture, "2000-01-01"))),
               "XPTY0004"));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

static void test_dates_overflow_at_the_year_limit(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item last_date;
    SXML_TEST_REQUIRE(
        state,
        sxml_atomic_item_make(
            fixture.item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DATE),
                .datum = {.datum_kind = SXML_DATUM_KIND_DATE_TIME,
                          .datum_details =
                              {.date_time = {.year = INT64_MAX,
                                             .month = 12,
                                             .day = 31,
                                             .hour = 0,
                                             .minute = 0,
                                             .second = 0,
                                             .nanosecond = 0,
                                             .has_timezone = false,
                                             .timezone_offset_minutes = 0}}}},
            &last_date) == SXML_STATUS_OK);
    sxml_sequence last =
        keep_value(&fixture, sxml_sequence_from_item(last_date));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD, last,
                                            year_month(&fixture, "P1M"))),
               "FODT0001"));
    SXML_TEST_EXPECT(
        state,
        raises(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD, last,
                                            day_time(&fixture, "P1D"))),
               "FODT0001"));
    SXML_TEST_EXPECT(
        state,
        gives(compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                           last, day_time(&fixture, "P364D"))),
              "9223372036854775807-01-01", SXML_TYPE_XS_DATE));
    /* The difference from year 1 has more seconds than int64_t holds. */
    SXML_TEST_EXPECT(
        state, raises(compute(&fixture,
                              arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                                         last, date(&fixture, "0001-01-01"))),
                      "FODT0002"));
    SXML_TEST_EXPECT(
        state, raises(compute(&fixture,
                              arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                         day_time(&fixture, "PT1S"),
                                         day_time(&fixture,
                                                  "PT9223372036854775807S"))),
                      "FODT0002"));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Value comparisons */

static void test_value_comparisons_order_every_family(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    /* Numbers of different types compare by exact value (PR2218):
       1.1 eq 1.1e0 is false. */
    SXML_TEST_EXPECT(state,
                     is_false(compare_values(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              decimal(&fixture, "1.1"),
                                              double_number(1.1)))));
    SXML_TEST_EXPECT(
        state, is_true(compare_values(&fixture,
                                      comparison(SXML_COMPARISON_OPERATOR_LESS,
                                                 decimal(&fixture, "1.1"),
                                                 double_number(1.1)))));
    /* NaN: false, except ne. */
    SXML_TEST_EXPECT(
        state,
        is_false(compare_values(
            &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                 double_number(NAN), double_number(NAN)))));
    SXML_TEST_EXPECT(
        state,
        is_true(compare_values(
            &fixture, comparison(SXML_COMPARISON_OPERATOR_NOT_EQUAL,
                                 double_number(NAN), double_number(NAN)))));
    SXML_TEST_EXPECT(
        state, is_false(compare_values(
                   &fixture, comparison(SXML_COMPARISON_OPERATOR_LESS_OR_EQUAL,
                                        double_number(NAN), integer(1)))));
    /* PR2256: durations, gYear and QNames are ordered. */
    SXML_TEST_EXPECT(
        state,
        is_true(compare_values(
            &fixture, comparison(SXML_COMPARISON_OPERATOR_LESS,
                                 typed(&fixture, SXML_TYPE_XS_DURATION, "P1Y"),
                                 year_month(&fixture, "P13M")))));
    SXML_TEST_EXPECT(
        state,
        is_true(compare_values(
            &fixture, comparison(SXML_COMPARISON_OPERATOR_GREATER,
                                 typed(&fixture, SXML_TYPE_XS_DURATION, "P1M"),
                                 day_time(&fixture, "P50D")))));
    SXML_TEST_EXPECT(
        state, is_true(compare_values(
                   &fixture,
                   comparison(SXML_COMPARISON_OPERATOR_LESS,
                              typed(&fixture, SXML_TYPE_XS_G_YEAR, "2020Z"),
                              typed(&fixture, SXML_TYPE_XS_G_YEAR, "2025")))));
    sxml_sequence xml_space = qname(
        &fixture,
        SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/XML/1998/namespace"),
        SXML_BYTE_VIEW_FROM_LITERAL("space"));
    sxml_sequence local_space =
        qname(&fixture, (sxml_byte_view){.bytes = NULL, .byte_count = 0},
              SXML_BYTE_VIEW_FROM_LITERAL("space"));
    sxml_sequence xml_id = qname(
        &fixture,
        SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/XML/1998/namespace"),
        SXML_BYTE_VIEW_FROM_LITERAL("id"));
    /* value-comp-QName-001: #xml:space gt #Q{}space. */
    SXML_TEST_EXPECT(state,
                     is_true(compare_values(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_GREATER,
                                              xml_space, local_space))));
    /* value-comp-QName-003 and general-comp-QName-002. */
    SXML_TEST_EXPECT(
        state, is_true(compare_values(&fixture,
                                      comparison(SXML_COMPARISON_OPERATOR_LESS,
                                                 xml_id, xml_space))));
    SXML_TEST_EXPECT(state,
                     is_true(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              xml_space, xml_space))));
    /* Binary across hexBinary and base64Binary (K-HexBinaryEQ-9). */
    SXML_TEST_EXPECT(
        state,
        is_true(compare_values(
            &fixture,
            comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                       typed(&fixture, SXML_TYPE_XS_HEX_BINARY, "0F"),
                       typed(&fixture, SXML_TYPE_XS_BASE64_BINARY, "Dw==")))));
    /* untypedAtomic compares as a string, so not with a number. */
    SXML_TEST_EXPECT(
        state, is_true(compare_values(&fixture,
                                      comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                                 untyped(&fixture, "a"),
                                                 string(&fixture, "a")))));
    SXML_TEST_EXPECT(
        state,
        raises(compare_values(&fixture,
                              comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                         untyped(&fixture, "1"), integer(1))),
               "XPTY0004"));
    SXML_TEST_EXPECT(
        state, raises(compare_values(&fixture,
                                     comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                                integer(1),
                                                date(&fixture, "2000-01-01"))),
                      "XPTY0004"));
    /* Dates without a timezone take the implicit one (-05:00). */
    SXML_TEST_EXPECT(
        state, is_true(compare_values(
                   &fixture,
                   comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                              date_time(&fixture, "2000-01-01T00:00:00"),
                              date_time(&fixture, "2000-01-01T05:00:00Z")))));
    SXML_TEST_EXPECT(
        state,
        is_true(compare_values(
            &fixture,
            comparison(SXML_COMPARISON_OPERATOR_GREATER_OR_EQUAL,
                       typed(&fixture, SXML_TYPE_XS_BOOLEAN, "true"),
                       typed(&fixture, SXML_TYPE_XS_BOOLEAN, "false")))));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

/* ----------------------------------------------- General comparisons */

static void test_general_comparisons_convert_untyped(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    /* PR2218: an untyped item is cast to the other number's type
       (general-comp-numeric-eq-001, -gt-003, GenCompEq-30 to -33). */
    SXML_TEST_EXPECT(
        state, is_false(compare_generally(
                   &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                        untyped(&fixture, "9007199254740993"),
                                        integer(9007199254740992)))));
    SXML_TEST_EXPECT(
        state,
        is_true(compare_generally(
            &fixture, comparison(SXML_COMPARISON_OPERATOR_GREATER,
                                 untyped(&fixture, "0.30000000000000001"),
                                 decimal(&fixture, "0.3")))));
    SXML_TEST_EXPECT(
        state, is_false(compare_generally(
                   &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                        integer(9007199254740993),
                                        double_number(9007199254740992.0)))));
    SXML_TEST_EXPECT(state,
                     is_false(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              double_number(0.1),
                                              decimal(&fixture, "0.1")))));
    sxml_sequence doubles = sequence_of(
        &fixture, (value_list){{double_number(1.2), double_number(1.3)}, 2});
    sxml_sequence decimals = sequence_of(
        &fixture,
        (value_list){{decimal(&fixture, "1.2"), decimal(&fixture, "1.3")}, 2});
    sxml_sequence mixed = sequence_of(
        &fixture, (value_list){{decimal(&fixture, "1.2"), double_number(1.2),
                                decimal(&fixture, "1.3")},
                               3});
    SXML_TEST_EXPECT(
        state, is_true(compare_generally(
                   &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                        untyped(&fixture, "1.2"), doubles))));
    SXML_TEST_EXPECT(
        state, is_true(compare_generally(
                   &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                        untyped(&fixture, "1.2"), decimals))));
    SXML_TEST_EXPECT(
        state, is_true(compare_generally(
                   &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                        untyped(&fixture, "1.2e0"), mixed))));
    SXML_TEST_EXPECT(
        state,
        is_false(compare_generally(
            &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                 untyped(&fixture, "1.2e0"), decimals))));
    /* The untyped item on the right is converted too. */
    SXML_TEST_EXPECT(
        state,
        is_true(compare_generally(
            &fixture, comparison(SXML_COMPARISON_OPERATOR_LESS, integer(1),
                                 untyped(&fixture, "1.5")))));
    /* GenCompEq-29: (1.1, 2.1, 3.1) = (NaN, 4.2) ! (. div 2). */
    sxml_sequence tenths =
        sequence_of(&fixture, (value_list){{decimal(&fixture, "1.1"),
                                            decimal(&fixture, "2.1"),
                                            decimal(&fixture, "3.1")},
                                           3});
    sxml_sequence halves = sequence_of(
        &fixture,
        (value_list){{double_number(NAN), decimal(&fixture, "2.1")}, 2});
    SXML_TEST_EXPECT(state,
                     is_true(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              tenths, halves))));
    SXML_TEST_EXPECT(
        state,
        is_true(compare_generally(
            &fixture, comparison(SXML_COMPARISON_OPERATOR_NOT_EQUAL,
                                 double_number(NAN), double_number(NAN)))));
    /* Two untyped items compare as strings: "2" and "2.0" differ
       (XPath 4.0, section 4.11.2 example). */
    SXML_TEST_EXPECT(state,
                     is_false(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              untyped(&fixture, "2"),
                                              untyped(&fixture, "2.0")))));
    SXML_TEST_EXPECT(state,
                     is_true(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              untyped(&fixture, "2"),
                                              decimal(&fixture, "2.0")))));
    /* Against other types the untyped item is cast to their primitive
       type; a failing cast is FORG0001. */
    SXML_TEST_EXPECT(state,
                     is_true(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              untyped(&fixture, " 2000-01-01 "),
                                              date(&fixture, "2000-01-01")))));
    SXML_TEST_EXPECT(state,
                     is_true(compare_generally(
                         &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                              untyped(&fixture, "P1D"),
                                              day_time(&fixture, "PT24H")))));
    SXML_TEST_EXPECT(
        state,
        raises(compare_generally(
                   &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                        untyped(&fixture, "abc"), integer(1))),
               "FORG0001"));
    SXML_TEST_EXPECT(
        state, raises(compare_generally(
                          &fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                               untyped(&fixture, "x"),
                                               date(&fixture, "2000-01-01"))),
                      "FORG0001"));
    /* Items that do not compare are XPTY0004. */
    SXML_TEST_EXPECT(
        state,
        raises(compare_generally(&fixture,
                                 comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                            integer(1), string(&fixture, "1"))),
               "XPTY0004"));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

static void test_non_atomic_operands_are_refused(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item array;
    SXML_TEST_REQUIRE(state,
                      sxml_array_make(fixture.item_heap,
                                      (sxml_sequence_view){.sequences = NULL,
                                                           .sequence_count = 0},
                                      &array) == SXML_STATUS_OK);
    sxml_sequence array_value =
        keep_value(&fixture, sxml_sequence_from_item(array));
    SXML_TEST_EXPECT(state,
                     compute(&fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                                  array_value, integer(1)))
                             .status == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(
        state,
        compare_values(&fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                            integer(1), array_value))
                .status == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(
        state,
        compare_generally(&fixture, comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                                               integer(1), array_value))
                .status == SXML_STATUS_INVALID_ARGUMENT);
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

/* --------------------------------------------------- GNode comparisons */

typedef struct node_case {
    sxml_node_comparison_operator node_comparison_operator;
    sxml_sequence left_operand;
    sxml_sequence right_operand;
} node_case;

static operator_outcome compare_nodes(const operator_fixture *fixture,
                                      node_case test) {
    operator_outcome outcome = outcome_start();
    if (fixture->making_status != SXML_STATUS_OK) {
        outcome.status = fixture->making_status;
        return outcome;
    }
    sxml_error_record record;
    sxml_sequence result = sxml_sequence_empty();
    outcome.status = sxml_operator_make_node_comparison(
        &(sxml_node_comparison_request){.node_comparison_operator =
                                            test.node_comparison_operator,
                                        .left_operand = test.left_operand,
                                        .right_operand = test.right_operand,
                                        .error_record = &record},
        &result);
    if (outcome.status == SXML_STATUS_OK) {
        outcome_describe(fixture, &outcome, result);
    } else if (sxml_status_has_error_record(outcome.status)) {
        outcome_set_error(&outcome, &record);
    }
    return outcome;
}

/* A child JNode of json_node, kept by the fixture. */
static sxml_sequence json_child(operator_fixture *fixture,
                                sxml_sequence json_node, uint64_t child_index) {
    sxml_item child = {.representation = 0};
    sxml_status status = sxml_json_node_make_child(
        fixture->item_heap,
        (sxml_json_child_request){.json_node = json_node.content,
                                  .child_index = child_index},
        &child);
    if (status != SXML_STATUS_OK) {
        fixture->making_status = status;
        return sxml_sequence_empty();
    }
    return keep_value(fixture, sxml_sequence_from_item(child));
}

static sxml_sequence json_root(operator_fixture *fixture, sxml_sequence value) {
    sxml_item root = {.representation = 0};
    sxml_status status =
        sxml_json_node_make_root(fixture->item_heap, value, &root);
    if (status != SXML_STATUS_OK) {
        fixture->making_status = status;
        return sxml_sequence_empty();
    }
    return keep_value(fixture, sxml_sequence_from_item(root));
}

static void test_node_comparisons(sxml_test_state *state) {
    operator_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      operator_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_sequence members[2] = {integer(1), integer(2)};
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_array_make(fixture.item_heap,
                                      (sxml_sequence_view){.sequences = members,
                                                           .sequence_count = 2},
                                      &array) == SXML_STATUS_OK);
    sxml_sequence array_value =
        keep_value(&fixture, sxml_sequence_from_item(array));
    sxml_sequence root = json_root(&fixture, array_value);
    sxml_sequence other_root = json_root(&fixture, array_value);
    sxml_sequence first = json_child(&fixture, root, 0);
    sxml_sequence first_again = json_child(&fixture, root, 0);
    sxml_sequence second = json_child(&fixture, root, 1);
    /* A child made twice is the same node (XDM 4.0, section 8.5). */
    SXML_TEST_EXPECT(state,
                     is_true(compare_nodes(
                         &fixture, (node_case){SXML_NODE_COMPARISON_OPERATOR_IS,
                                               first, first_again})));
    SXML_TEST_EXPECT(
        state, is_false(compare_nodes(
                   &fixture, (node_case){SXML_NODE_COMPARISON_OPERATOR_IS_NOT,
                                         first, first_again})));
    SXML_TEST_EXPECT(state,
                     is_false(compare_nodes(
                         &fixture, (node_case){SXML_NODE_COMPARISON_OPERATOR_IS,
                                               first, second})));
    SXML_TEST_EXPECT(
        state, is_true(compare_nodes(
                   &fixture, (node_case){SXML_NODE_COMPARISON_OPERATOR_PRECEDES,
                                         root, first})));
    SXML_TEST_EXPECT(
        state, is_true(compare_nodes(
                   &fixture, (node_case){SXML_NODE_COMPARISON_OPERATOR_FOLLOWS,
                                         second, first})));
    SXML_TEST_EXPECT(
        state,
        is_true(compare_nodes(
            &fixture, (node_case){SXML_NODE_COMPARISON_OPERATOR_PRECEDES_OR_IS,
                                  first, first_again})));
    SXML_TEST_EXPECT(
        state,
        is_false(compare_nodes(
            &fixture, (node_case){SXML_NODE_COMPARISON_OPERATOR_FOLLOWS_OR_IS,
                                  first, second})));
    /* Trees order by identifier: the root made later follows
       (nodeexpression28 allows either order, but it is stable). */
    SXML_TEST_EXPECT(
        state, is_true(compare_nodes(
                   &fixture, (node_case){SXML_NODE_COMPARISON_OPERATOR_PRECEDES,
                                         second, other_root})));
    SXML_TEST_EXPECT(state,
                     is_empty(compare_nodes(
                         &fixture, (node_case){SXML_NODE_COMPARISON_OPERATOR_IS,
                                               sxml_sequence_empty(), first})));
    SXML_TEST_EXPECT(
        state,
        raises(compare_nodes(&fixture,
                             (node_case){SXML_NODE_COMPARISON_OPERATOR_IS,
                                         first, integer(1)}),
               "XPTY0004"));
    sxml_sequence both =
        sequence_of(&fixture, (value_list){{first, second}, 2});
    SXML_TEST_EXPECT(
        state,
        raises(compare_nodes(
                   &fixture,
                   (node_case){SXML_NODE_COMPARISON_OPERATOR_IS, both, first}),
               "XPTY0004"));
    operator_fixture_release_values(&fixture);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    operator_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Allocation failures */

/* Runs operators that make cells; returns the first failure. */
static sxml_status run_allocating_operators(operator_fixture *fixture) {
    const operator_outcome outcomes[] = {
        compute(fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_ADD,
                                    integer(INT64_MAX), integer(1))),
        compute(fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_DIVIDE, integer(1),
                                    integer(3))),
        compute(fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_MODULO,
                                    double_number(1e300), double_number(7))),
        compute(fixture, arithmetic(SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE,
                                    double_number(1e20), double_number(3))),
        compute(fixture,
                arithmetic(
                    SXML_ARITHMETIC_OPERATOR_MULTIPLY,
                    typed(fixture, SXML_TYPE_XS_DAY_TIME_DURATION, "P1DT0.5S"),
                    double_number(1.5))),
        compute(fixture,
                arithmetic(SXML_ARITHMETIC_OPERATOR_SUBTRACT,
                           typed(fixture, SXML_TYPE_XS_DATE_TIME,
                                 "2000-10-30T06:12:00"),
                           typed(fixture, SXML_TYPE_XS_DATE, "1999-11-28"))),
        compute(
            fixture,
            arithmetic(
                SXML_ARITHMETIC_OPERATOR_ADD,
                typed(fixture, SXML_TYPE_XS_DATE_TIME, "2000-10-30T06:12:00"),
                typed(fixture, SXML_TYPE_XS_YEAR_MONTH_DURATION, "P1Y"))),
        compare_generally(
            fixture,
            comparison(SXML_COMPARISON_OPERATOR_EQUAL,
                       typed(fixture, SXML_TYPE_XS_UNTYPED_ATOMIC, "1.5"),
                       typed(fixture, SXML_TYPE_XS_DECIMAL, "1.5"))),
        compare_values(fixture,
                       comparison(SXML_COMPARISON_OPERATOR_LESS,
                                  typed(fixture, SXML_TYPE_XS_DECIMAL, "1.5"),
                                  double_number(2))),
    };
    for (size_t outcome_index = 0;
         outcome_index < sizeof(outcomes) / sizeof(outcomes[0]);
         outcome_index += 1) {
        sxml_status status = outcomes[outcome_index].status;
        if (status != SXML_STATUS_OK && status != SXML_STATUS_TYPE_ERROR) {
            return status;
        }
    }
    return fixture->making_status;
}

static void
test_operators_survive_every_allocation_failure(sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(
            state,
            sxml_counting_allocator_create(
                sxml_default_allocator(),
                &(sxml_counting_allocator_configuration){
                    .backing_allocator = sxml_default_allocator(),
                    .failing_allocation_number = failing_allocation_number},
                &counting_allocator) == SXML_STATUS_OK);
        operator_fixture fixture;
        SXML_TEST_REQUIRE(
            state, operator_fixture_create_with(
                       sxml_counting_allocator_as_allocator(counting_allocator),
                       &fixture) == SXML_STATUS_OK);
        sxml_status status = run_allocating_operators(&fixture);
        operator_fixture_release_values(&fixture);
        sxml_item_heap_counts counts =
            sxml_item_heap_read_counts(fixture.item_heap);
        SXML_TEST_EXPECT(state, counts.live_cell_count == 0);
        SXML_TEST_EXPECT(state, counts.live_byte_count == 0);
        operator_fixture_destroy(&fixture);
        sxml_allocation_counts allocation_counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_EXPECT(state, allocation_counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state,
                             allocation_counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 2000);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 10);
}

static const sxml_test_case operator_test_cases[] = {
    {.name = "slice_arithmetic_lines", .function = test_slice_arithmetic_lines},
    {.name = "slice_comparison_lines", .function = test_slice_comparison_lines},
    {.name = "integers_overflow_into_decimals",
     .function = test_integers_overflow_into_decimals},
    {.name = "numbers_promote_to_a_common_type",
     .function = test_numbers_promote_to_a_common_type},
    {.name = "division_and_special_values",
     .function = test_division_and_special_values},
    {.name = "decimal_digit_limit_is_overflow",
     .function = test_decimal_digit_limit_is_overflow},
    {.name = "unary_plus_and_minus", .function = test_unary_plus_and_minus},
    {.name = "duration_arithmetic", .function = test_duration_arithmetic},
    {.name = "date_and_time_arithmetic",
     .function = test_date_and_time_arithmetic},
    {.name = "dates_overflow_at_the_year_limit",
     .function = test_dates_overflow_at_the_year_limit},
    {.name = "value_comparisons_order_every_family",
     .function = test_value_comparisons_order_every_family},
    {.name = "general_comparisons_convert_untyped",
     .function = test_general_comparisons_convert_untyped},
    {.name = "node_comparisons", .function = test_node_comparisons},
    {.name = "non_atomic_operands_are_refused",
     .function = test_non_atomic_operands_are_refused},
    {.name = "operators_survive_every_allocation_failure",
     .function = test_operators_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "operator",
            .cases = operator_test_cases,
            .case_count = SXML_TEST_COUNT(operator_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
