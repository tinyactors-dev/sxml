/*
 * Tests for the IR unit, its text form, the verifier and the pass list
 * (src/ir.c, src/ir_verify.c; src/ir_internal.h, src/ir_pass_internal.h;
 * D46, D55, D63).
 *
 * Golden file: tests/memory_test.c: lifecycle checks, then behaviour, then
 * allocation-failure loops (D23). Units are built by hand here; lowering
 * builds them in tests/xpath_static_test.c. The last section checks the
 * lowering seam (src/xpath_lower_internal.h, D103): probe lowering parts,
 * the embedding entry and procedures run from outside a machine. The
 * XQuery payloads (try/catch clauses, name tests, constructor namespaces
 * and flags; D109, D110) have a hand-built unit of their own.
 */
#include "ir_internal.h"
#include "ir_pass_internal.h"
#include "xpath_lower_internal.h"
#include "sxml/name.h"
#include "sxml/string.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

typedef struct fixture {
    alignas(16) uint8_t counting_storage[256];
    sxml_counting_allocator *counting_allocator;
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} fixture;

static sxml_status fixture_create(uint64_t failing_allocation_number,
                                  fixture *test_fixture) {
    test_fixture->counting_allocator = NULL;
    test_fixture->name_pool = NULL;
    test_fixture->registry = NULL;
    test_fixture->item_heap = NULL;
    sxml_status status = sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = test_fixture->counting_storage,
                           .byte_count = sizeof(test_fixture->counting_storage)},
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_create(
            sxml_default_allocator(),
            &(sxml_name_pool_configuration){.allocator = sxml_default_allocator(),
                                            .maximum_byte_count = 0},
            &test_fixture->name_pool);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_create(
            sxml_default_allocator(),
            &(sxml_type_registry_configuration){
                .allocator = sxml_default_allocator(),
                .name_pool = test_fixture->name_pool,
                .maximum_registered_type_count = 0},
            &test_fixture->registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            sxml_default_allocator(),
            &(sxml_item_heap_configuration){
                .allocator = sxml_default_allocator(),
                .name_pool = test_fixture->name_pool,
                .type_registry = test_fixture->registry,
                .maximum_byte_count = 0},
            &test_fixture->item_heap);
    }
    return status;
}

static void fixture_destroy(fixture *test_fixture) {
    sxml_item_heap_destroy(test_fixture->item_heap);
    sxml_type_registry_destroy(test_fixture->registry);
    sxml_name_pool_destroy(test_fixture->name_pool);
    sxml_counting_allocator_finalize(test_fixture->counting_allocator);
}

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static uint64_t live_allocations(const fixture *test_fixture) {
    return sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
        .live_allocation_count;
}

static uint64_t live_cells(const fixture *test_fixture) {
    return sxml_item_heap_read_counts(test_fixture->item_heap).live_cell_count;
}

static sxml_ir_unit_configuration unit_configuration(const fixture *test_fixture) {
    return (sxml_ir_unit_configuration){
        .allocator = fixture_allocator(test_fixture),
        .item_heap = test_fixture->item_heap,
        .source_name = SXML_BYTE_VIEW_FROM_LITERAL("unit.xpath"),
        .maximum_expression_count = 0};
}

/* ------------------------------------------------------- Building units */

static sxml_ir_atom slot(uint32_t number) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT,
                          .slot = {.value = number}};
}

static sxml_ir_atom constant(sxml_ir_constant_index index) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_CONSTANT,
                          .constant = index};
}

static sxml_status add(sxml_ir_unit *unit, sxml_ir_expression expression,
                       sxml_ir_expression_index *index) {
    return sxml_ir_unit_add_expression(unit, &expression, index);
}

static sxml_status register_signature(const fixture *test_fixture,
                                      uint32_t parameter_count,
                                      sxml_type_identifier *signature) {
    const sxml_sequence_type any = {
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    const sxml_sequence_type parameters[2] = {any, any};
    return sxml_type_registry_register_item_type(
        test_fixture->registry,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {
                                    .is_any_function = false,
                                    .parameter_types = {.sequence_types = parameters,
                                                        .sequence_type_count =
                                                            parameter_count},
                                    .result_type = any}}},
        signature);
}

/* The indices of the example unit, for tests that change it. */
typedef struct example_unit {
    sxml_ir_unit *unit;
    sxml_ir_expression_index main_let;
    sxml_ir_expression_index dynamic_call;
    sxml_ir_expression_index body_let;
    sxml_ir_expression_index raise;
    sxml_ir_expression_index addition;
    sxml_ir_function_index inner_function;
} example_unit;

/*
 * Builds Example 1 of src/ir_internal.h, the IR of QT4 inline-fn-005,
 * `let $f := function($x){$x + .} return $f(4)`, with its function bodies
 * in the order lowering makes them.
 */
static sxml_status build_example_unit(const fixture *test_fixture,
                                      example_unit *example) {
    const sxml_ir_unit_configuration configuration =
        unit_configuration(test_fixture);
    sxml_ir_unit *unit = NULL;
    sxml_status status =
        sxml_ir_unit_create(fixture_allocator(test_fixture), &configuration, &unit);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_identifier main_signature = {0};
    sxml_type_identifier inner_signature = {0};
    sxml_ir_function_index main_function = {0};
    sxml_ir_function_index inner_function = {0};
    sxml_ir_constant_index four = {0};
    sxml_ir_atom_list arguments = {0};
    example_unit built = {.unit = unit};
    status = register_signature(test_fixture, 0, &main_signature);
    if (status == SXML_STATUS_OK) {
        status = register_signature(test_fixture, 1, &inner_signature);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            unit, &(sxml_ir_function){.signature = main_signature},
            &main_function);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            unit,
            &(sxml_ir_function){.signature = inner_signature,
                                .parameter_count = 1,
                                .defining_function = main_function,
                                .slot_count = 2},
            &inner_function);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_constant(
            unit, sxml_sequence_from_item(sxml_atomic_item_from_integer(4)),
            &four);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_ir_atom call_arguments[] = {constant(four)};
        status = sxml_ir_unit_add_atoms(
            unit, (sxml_ir_atom_view){.atoms = call_arguments, .atom_count = 1},
            &arguments);
    }
    sxml_ir_expression_index definition = {0};
    if (status == SXML_STATUS_OK) {
        status = add(unit,
                     (sxml_ir_expression){
                         .expression_kind =
                             SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION,
                         .function_definition = {.function = inner_function}},
                     &definition);
    }
    if (status == SXML_STATUS_OK) {
        status = add(unit,
                     (sxml_ir_expression){
                         .expression_kind = SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL,
                         .dynamic_call = {.function_item = slot(1),
                                          .arguments = arguments}},
                     &built.dynamic_call);
    }
    if (status == SXML_STATUS_OK) {
        status = add(unit,
                     (sxml_ir_expression){
                         .expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                         .let = {.variable = {.value = 1},
                                 .bound_expression = definition,
                                 .body = built.dynamic_call}},
                     &built.main_let);
    }
    if (status == SXML_STATUS_OK) {
        status = add(
            unit,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_RAISE,
                .raise = {.error_status = SXML_STATUS_DYNAMIC_ERROR,
                          .error_code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(
                                             SXML_ERROR_NAMESPACE_URI),
                                         .local_name =
                                             SXML_BYTE_VIEW_FROM_LITERAL("XPDY0002"),
                                         .namespace_prefix =
                                             SXML_BYTE_VIEW_FROM_LITERAL("err")},
                          .description = SXML_BYTE_VIEW_FROM_LITERAL(
                              "the focus is absent in a function body")}},
            &built.raise);
    }
    if (status == SXML_STATUS_OK) {
        status = add(unit,
                     (sxml_ir_expression){
                         .expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                         .source_span = {.byte_offset = 26,
                                         .byte_count = 6,
                                         .line_number = 1,
                                         .column_number = 27},
                         .binary = {.operator = SXML_IR_BINARY_OPERATOR_ADD,
                                    .left_operand = slot(1),
                                    .right_operand = slot(2)}},
                     &built.addition);
    }
    if (status == SXML_STATUS_OK) {
        status = add(unit,
                     (sxml_ir_expression){
                         .expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                         .let = {.variable = {.value = 2},
                                 .bound_expression = built.raise,
                                 .body = built.addition}},
                     &built.body_let);
    }
    if (status == SXML_STATUS_OK) {
        sxml_ir_function_replacement replacement = {
            .index = main_function,
            .function = *sxml_ir_unit_get_function(unit, main_function)};
        replacement.function.body = built.main_let;
        replacement.function.slot_count = 1;
        status = sxml_ir_unit_replace_function(unit, &replacement);
    }
    if (status == SXML_STATUS_OK) {
        sxml_ir_function_replacement replacement = {
            .index = inner_function,
            .function = *sxml_ir_unit_get_function(unit, inner_function)};
        replacement.function.body = built.body_let;
        status = sxml_ir_unit_replace_function(unit, &replacement);
    }
    if (status != SXML_STATUS_OK) {
        sxml_ir_unit_release(unit);
        return status;
    }
    built.inner_function = inner_function;
    *example = built;
    return SXML_STATUS_OK;
}

static const char example_text[] =
    "(function 1 main (parameters 0) (captures) (slots 1)\n"
    "  (let $1 (function-definition 2)\n"
    "    (dynamic-call $1 4)))\n"
    "(function 2 anonymous (parameters 1) (captures) (slots 2)\n"
    "  (let $2 (raise #err:XPDY0002 \"the focus is absent in a function "
    "body\")\n"
    "    (binary add $1 $2)))\n";

static sxml_status verify(const fixture *test_fixture, sxml_ir_unit *unit,
                          sxml_diagnostic *diagnostic) {
    return sxml_ir_verify(&(sxml_ir_pass_request){
        .unit = unit,
        .type_registry = test_fixture->registry,
        .error_record = NULL,
        .diagnostic = diagnostic});
}

/* ------------------------------------------------------------ Lifecycle */

static void test_unit_initializes_in_caller_memory_and_copies_source_name(
    sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    uint8_t name_bytes[] = {'q', '.', 'x', 'p'};
    sxml_ir_unit_configuration configuration = unit_configuration(&test_fixture);
    configuration.source_name =
        (sxml_byte_view){.bytes = name_bytes, .byte_count = sizeof(name_bytes)};
    sxml_memory_requirements requirements = {0};
    SXML_TEST_REQUIRE(state, sxml_ir_unit_memory_requirements(
                                 &configuration, &requirements) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, requirements.byte_count >= sizeof(sxml_ir_unit) + 4);
    alignas(16) static uint8_t storage[2048];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(storage));
    sxml_ir_unit *unit = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_ir_unit_initialize(
                         (sxml_memory_view){.bytes = storage,
                                            .byte_count =
                                                requirements.byte_count - 1},
                         &configuration, &unit) ==
                         SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state,
                     sxml_ir_unit_initialize(
                         (sxml_memory_view){.bytes = storage + 1,
                                            .byte_count = requirements.byte_count},
                         &configuration, &unit) == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, unit == NULL);
    SXML_TEST_REQUIRE(state,
                      sxml_ir_unit_initialize(
                          (sxml_memory_view){.bytes = storage,
                                             .byte_count = requirements.byte_count},
                          &configuration, &unit) == SXML_STATUS_OK);
    name_bytes[0] = 'z';
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(unit->source_name,
                                           SXML_BYTE_VIEW_FROM_LITERAL("q.xp")));
    SXML_TEST_EXPECT(state, unit->reference_count == 1);
    sxml_ir_constant_index index = {0};
    SXML_TEST_EXPECT(state,
                     sxml_ir_unit_add_constant(
                         unit, sxml_sequence_from_item(sxml_atomic_item_true()),
                         &index) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, index.value == 1);
    sxml_ir_unit_retain(unit);
    sxml_ir_unit_release(unit);
    SXML_TEST_EXPECT(state, unit->reference_count == 1);
    /* The last release of an initialized unit finalizes it. */
    sxml_ir_unit_release(unit);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    sxml_ir_unit_finalize(NULL);
    sxml_ir_unit_destroy(NULL);
    sxml_ir_unit_release(NULL);
    fixture_destroy(&test_fixture);
}

static void test_unit_rejects_invalid_configuration(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_memory_requirements requirements = {0};
    sxml_ir_unit_configuration configuration = unit_configuration(&test_fixture);
    configuration.allocator = (sxml_allocator){0};
    SXML_TEST_EXPECT(state, sxml_ir_unit_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    configuration = unit_configuration(&test_fixture);
    configuration.item_heap = NULL;
    sxml_ir_unit *unit = NULL;
    SXML_TEST_EXPECT(state, sxml_ir_unit_create(sxml_default_allocator(),
                                                &configuration, &unit) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, unit == NULL);
    fixture_destroy(&test_fixture);
}

static void test_unit_keeps_constants_until_its_last_release(
    sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_ir_unit_configuration configuration =
        unit_configuration(&test_fixture);
    sxml_ir_unit *unit = NULL;
    SXML_TEST_REQUIRE(state, sxml_ir_unit_create(fixture_allocator(&test_fixture),
                                                 &configuration,
                                                 &unit) == SXML_STATUS_OK);
    sxml_string string = {0};
    SXML_TEST_REQUIRE(state, sxml_string_from_utf8(
                                 SXML_BYTE_VIEW_FROM_LITERAL("a string in a cell"),
                                 &string) == SXML_STATUS_OK);
    sxml_item item = {0};
    SXML_TEST_REQUIRE(
        state,
        sxml_atomic_item_make(
            test_fixture.item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = string}}},
            &item) == SXML_STATUS_OK);
    const sxml_sequence value = sxml_sequence_from_item(item);
    sxml_ir_constant_index index = {0};
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_constant(unit, value, &index) ==
                                SXML_STATUS_OK);
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, live_cells(&test_fixture) == 1);
    sxml_ir_unit_retain(unit);
    sxml_ir_unit_release(unit);
    SXML_TEST_EXPECT(state, live_cells(&test_fixture) == 1);
    sxml_ir_unit_release(unit);
    SXML_TEST_EXPECT(state, live_cells(&test_fixture) == 0);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

static void test_accessors_give_zero_entries_out_of_range(
    sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_ir_unit_configuration configuration =
        unit_configuration(&test_fixture);
    sxml_ir_unit *unit = NULL;
    SXML_TEST_REQUIRE(state, sxml_ir_unit_create(sxml_default_allocator(),
                                                 &configuration,
                                                 &unit) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_ir_unit_get_expression(
                         unit, (sxml_ir_expression_index){.value = 3})
                             ->expression_kind == SXML_IR_EXPRESSION_KIND_NONE);
    SXML_TEST_EXPECT(state, sxml_ir_unit_get_function(
                                unit, (sxml_ir_function_index){.value = 1})
                                    ->body.value == 0);
    SXML_TEST_EXPECT(state,
                     sxml_ir_unit_get_atoms(
                         unit, (sxml_ir_atom_list){.first_atom_index = 1,
                                                   .atom_count = 2})
                             .atom_count == 0);
    SXML_TEST_EXPECT(state,
                     sxml_sequence_get_representation(sxml_ir_unit_get_constant(
                         unit, (sxml_ir_constant_index){.value = 9})) ==
                         SXML_SEQUENCE_REPRESENTATION_EMPTY);
    const sxml_ir_atom atoms[] = {slot(1), slot(2), slot(3)};
    sxml_ir_atom_list list = {0};
    SXML_TEST_REQUIRE(state, sxml_ir_unit_add_atoms(
                                 unit, (sxml_ir_atom_view){.atoms = atoms,
                                                           .atom_count = 3},
                                 &list) == SXML_STATUS_OK);
    /* Entry 0 is reserved: the first list starts at 1. */
    SXML_TEST_EXPECT(state, list.first_atom_index == 1 && list.atom_count == 3);
    const sxml_ir_atom_view stored = sxml_ir_unit_get_atoms(unit, list);
    SXML_TEST_REQUIRE(state, stored.atom_count == 3 && stored.atoms != NULL);
    SXML_TEST_EXPECT(state, stored.atoms[2].slot.value == 3);
    sxml_ir_unit_destroy(unit);
    fixture_destroy(&test_fixture);
}

static void test_sealed_unit_and_limits_refuse_additions(
    sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_ir_unit_configuration configuration = unit_configuration(&test_fixture);
    configuration.maximum_expression_count = 2;
    sxml_ir_unit *unit = NULL;
    SXML_TEST_REQUIRE(state, sxml_ir_unit_create(fixture_allocator(&test_fixture),
                                                 &configuration,
                                                 &unit) == SXML_STATUS_OK);
    const sxml_ir_expression atom_expression = {
        .expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = slot(1)};
    sxml_ir_expression_index index = {0};
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_expression(unit, &atom_expression,
                                                        &index) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, index.value == 1);
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_expression(unit, &atom_expression,
                                                        &index) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_expression(unit, &atom_expression,
                                                        &index) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, index.value == 2);
    const sxml_ir_expression no_kind = {
        .expression_kind = SXML_IR_EXPRESSION_KIND_NONE};
    SXML_TEST_EXPECT(state, sxml_ir_unit_replace_expression(
                                unit, &(sxml_ir_expression_replacement){
                                          .index = index, .expression = no_kind}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    /* The static namespaces (D84): set once, read back. */
    const sxml_namespace_binding bindings[2] = {
        {.namespace_prefix = {.index = 0}, .namespace_uri = {.index = SXML_NAMESPACE_XML}},
        {.namespace_prefix = {.index = 1}, .namespace_uri = {.index = SXML_NAMESPACE_NONE}}};
    SXML_TEST_EXPECT(state, sxml_ir_unit_get_static_namespaces(unit).namespace_binding_count ==
                                0);
    SXML_TEST_EXPECT(state, sxml_ir_unit_set_static_namespaces(
                                unit, (sxml_namespace_binding_view){
                                          .namespace_bindings = bindings,
                                          .namespace_binding_count = 2}) == SXML_STATUS_OK);
    const sxml_namespace_binding_view read_back = sxml_ir_unit_get_static_namespaces(unit);
    SXML_TEST_EXPECT(state, read_back.namespace_binding_count == 2 &&
                                read_back.namespace_bindings[0].namespace_uri.index ==
                                    SXML_NAMESPACE_XML &&
                                read_back.namespace_bindings[1].namespace_prefix.index == 1);
    SXML_TEST_EXPECT(state, sxml_ir_unit_set_static_namespaces(
                                unit, (sxml_namespace_binding_view){
                                          .namespace_bindings = bindings,
                                          .namespace_binding_count = 1}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_ir_unit_seal(unit);
    sxml_ir_function_index function = {0};
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_function(
                                unit, &(sxml_ir_function){0}, &function) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_ir_unit_replace_expression(
                                unit, &(sxml_ir_expression_replacement){
                                          .index = index,
                                          .expression = atom_expression}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_ir_unit_release(unit);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

/* ------------------------------------------------------------ Text form */

static void test_example_unit_writes_its_text_form(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    example_unit example = {0};
    SXML_TEST_REQUIRE(state, build_example_unit(&test_fixture, &example) ==
                                 SXML_STATUS_OK);
    char text[1024];
    size_t byte_count = 0;
    SXML_TEST_EXPECT(state, sxml_ir_unit_write_text(
                                example.unit,
                                (sxml_memory_view){.bytes = (uint8_t *)text,
                                                   .byte_count = sizeof(text)},
                                &byte_count) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, byte_count == sizeof(example_text) - 1);
    SXML_TEST_EXPECT(state, memcmp(text, example_text, sizeof(example_text) - 1) ==
                                0);
    /* Too small: the byte count needed is written, the output holds a
       prefix. */
    char small[10];
    size_t needed = 0;
    SXML_TEST_EXPECT(state, sxml_ir_unit_write_text(
                                example.unit,
                                (sxml_memory_view){.bytes = (uint8_t *)small,
                                                   .byte_count = sizeof(small)},
                                &needed) == SXML_STATUS_CAPACITY_EXCEEDED);
    SXML_TEST_EXPECT(state, needed == sizeof(example_text) - 1);
    SXML_TEST_EXPECT(state, memcmp(small, example_text, sizeof(small)) == 0);
    sxml_ir_unit_release(example.unit);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

/* ------------------------------------------------------------- Verifier */

static void test_verifier_accepts_the_example_unit(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    example_unit example = {0};
    SXML_TEST_REQUIRE(state, build_example_unit(&test_fixture, &example) ==
                                 SXML_STATUS_OK);
    sxml_diagnostic diagnostic = {0};
    SXML_TEST_EXPECT(state, verify(&test_fixture, example.unit, &diagnostic) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, diagnostic.description.byte_count == 0);
    SXML_TEST_EXPECT(state, sxml_ir_run_passes(&(sxml_ir_pass_list_request){
                                .pass_request = {.unit = example.unit,
                                                 .type_registry =
                                                     test_fixture.registry},
                                .passes = sxml_ir_standard_passes,
                                .verifies_after_each_pass = true}) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_ir_standard_passes.pass_count == 4);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_ir_standard_passes.passes[0].pass_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("verify")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_ir_standard_passes.passes[1].pass_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("filter-index")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_ir_standard_passes.passes[2].pass_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("hoist")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_ir_standard_passes.passes[3].pass_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("global-read")));
    sxml_ir_unit_release(example.unit);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

/* One way to break the example unit, and what the verifier must say. */
typedef enum breakage {
    SXML_TEST_BREAKAGE_UNBOUND_SLOT,
    SXML_TEST_BREAKAGE_SLOT_BEYOND_FRAME,
    SXML_TEST_BREAKAGE_SHARED_EXPRESSION,
    SXML_TEST_BREAKAGE_RESERVED_KIND,
    SXML_TEST_BREAKAGE_BOUND_TWICE,
    SXML_TEST_BREAKAGE_UNDEFINED_FUNCTION,
    SXML_TEST_BREAKAGE_WRONG_SIGNATURE,
    SXML_TEST_BREAKAGE_BAD_CONSTANT,
    SXML_TEST_BREAKAGE_UNKNOWN_OPERATOR,
} breakage;

static sxml_status break_unit(example_unit *example, breakage kind) {
    sxml_ir_unit *unit = example->unit;
    sxml_ir_expression_replacement replacement = {
        .index = example->addition,
        .expression = *sxml_ir_unit_get_expression(unit, example->addition)};
    switch (kind) {
    case SXML_TEST_BREAKAGE_UNBOUND_SLOT: {
        /* Main gets a second slot, which nothing binds, and reads it. */
        sxml_ir_function_replacement function_replacement = {
            .index = {.value = 1},
            .function = *sxml_ir_unit_get_function(
                unit, (sxml_ir_function_index){.value = 1})};
        function_replacement.function.slot_count = 2;
        sxml_status status =
            sxml_ir_unit_replace_function(unit, &function_replacement);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        replacement.index = example->dynamic_call;
        replacement.expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = slot(2)};
        break;
    }
    case SXML_TEST_BREAKAGE_SLOT_BEYOND_FRAME:
        replacement.expression.binary.right_operand = slot(7);
        break;
    case SXML_TEST_BREAKAGE_SHARED_EXPRESSION:
        /* The inner LET's body is also main's LET body. */
        replacement.index = example->main_let;
        replacement.expression = *sxml_ir_unit_get_expression(unit, example->main_let);
        replacement.expression.let.body = example->addition;
        break;
    case SXML_TEST_BREAKAGE_RESERVED_KIND:
        replacement.expression.expression_kind =
            SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES;
        replacement.expression.apply_templates =
            (sxml_ir_apply_templates){.select = slot(1)};
        break;
    case SXML_TEST_BREAKAGE_BOUND_TWICE:
        replacement.index = example->body_let;
        replacement.expression = *sxml_ir_unit_get_expression(unit, example->body_let);
        replacement.expression.let.variable = (sxml_ir_slot){.value = 1};
        break;
    case SXML_TEST_BREAKAGE_UNDEFINED_FUNCTION:
        /* Function 2 is never defined if main's definition goes. */
        replacement.index = example->main_let;
        replacement.expression = *sxml_ir_unit_get_expression(unit, example->main_let);
        {
            const sxml_ir_expression_index definition =
                replacement.expression.let.bound_expression;
            sxml_ir_expression_replacement atom_replacement = {
                .index = definition,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                               .atom = constant((sxml_ir_constant_index){.value = 1})}};
            return sxml_ir_unit_replace_expression(unit, &atom_replacement);
        }
    case SXML_TEST_BREAKAGE_WRONG_SIGNATURE: {
        sxml_ir_function_replacement function_replacement = {
            .index = example->inner_function,
            .function = *sxml_ir_unit_get_function(unit, example->inner_function)};
        function_replacement.function.signature =
            sxml_builtin_type_identifier(SXML_TYPE_FUNCTION);
        return sxml_ir_unit_replace_function(unit, &function_replacement);
    }
    case SXML_TEST_BREAKAGE_BAD_CONSTANT:
        replacement.expression.binary.left_operand =
            constant((sxml_ir_constant_index){.value = 5});
        break;
    case SXML_TEST_BREAKAGE_UNKNOWN_OPERATOR:
        replacement.expression.binary.operator = (sxml_ir_binary_operator)99;
        break;
    }
    return sxml_ir_unit_replace_expression(unit, &replacement);
}

static void test_verifier_names_each_broken_invariant(sxml_test_state *state) {
    static const breakage breakages[] = {
        SXML_TEST_BREAKAGE_UNBOUND_SLOT,       SXML_TEST_BREAKAGE_SLOT_BEYOND_FRAME,
        SXML_TEST_BREAKAGE_SHARED_EXPRESSION,  SXML_TEST_BREAKAGE_RESERVED_KIND,
        SXML_TEST_BREAKAGE_BOUND_TWICE,        SXML_TEST_BREAKAGE_UNDEFINED_FUNCTION,
        SXML_TEST_BREAKAGE_WRONG_SIGNATURE,    SXML_TEST_BREAKAGE_BAD_CONSTANT,
        SXML_TEST_BREAKAGE_UNKNOWN_OPERATOR,
    };
    for (size_t breakage_index = 0;
         breakage_index < sizeof(breakages) / sizeof(breakages[0]);
         breakage_index += 1) {
        fixture test_fixture;
        SXML_TEST_REQUIRE(state,
                          fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        example_unit example = {0};
        SXML_TEST_REQUIRE(state, build_example_unit(&test_fixture, &example) ==
                                     SXML_STATUS_OK);
        SXML_TEST_REQUIRE(state, break_unit(&example, breakages[breakage_index]) ==
                                     SXML_STATUS_OK);
        sxml_diagnostic diagnostic = {0};
        const sxml_status status = verify(&test_fixture, example.unit, &diagnostic);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, diagnostic.description.byte_count > 0);
        SXML_TEST_EXPECT(state,
                         sxml_byte_view_equals(diagnostic.location.source_name,
                                               SXML_BYTE_VIEW_FROM_LITERAL(
                                                   "unit.xpath")));
        if (status != SXML_STATUS_INVALID_ARGUMENT) {
            (void)fprintf(stderr, "breakage %zu not found\n", breakage_index);
        }
        sxml_ir_unit_release(example.unit);
        SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
        fixture_destroy(&test_fixture);
    }
}

static void test_verifier_reports_the_span_of_the_broken_expression(
    sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    example_unit example = {0};
    SXML_TEST_REQUIRE(state, build_example_unit(&test_fixture, &example) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, break_unit(&example, SXML_TEST_BREAKAGE_SLOT_BEYOND_FRAME) ==
                                 SXML_STATUS_OK);
    sxml_diagnostic diagnostic = {0};
    SXML_TEST_EXPECT(state, verify(&test_fixture, example.unit, &diagnostic) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, diagnostic.location.line_number == 1);
    SXML_TEST_EXPECT(state, diagnostic.location.column_number == 27);
    SXML_TEST_EXPECT(state, diagnostic.location.byte_offset == 26);
    sxml_ir_unit_release(example.unit);
    fixture_destroy(&test_fixture);
}

/* A pass that breaks the unit: run with verification after each pass, the
   list stops there. */
static sxml_status run_breaking_pass(const sxml_ir_pass_request *request) {
    const sxml_ir_function *main_function = sxml_ir_unit_get_function(
        request->unit, (sxml_ir_function_index){.value = 1});
    sxml_ir_function_replacement replacement = {.index = {.value = 1},
                                                .function = *main_function};
    replacement.function.slot_count = 0;
    return sxml_ir_unit_replace_function(request->unit, &replacement);
}

static void test_pass_list_verifies_after_each_pass(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    example_unit example = {0};
    SXML_TEST_REQUIRE(state, build_example_unit(&test_fixture, &example) ==
                                 SXML_STATUS_OK);
    const sxml_ir_pass passes[] = {
        {.pass_name = SXML_BYTE_VIEW_INITIALIZER("break"),
         .run = run_breaking_pass},
    };
    sxml_diagnostic diagnostic = {0};
    SXML_TEST_EXPECT(state,
                     sxml_ir_run_passes(&(sxml_ir_pass_list_request){
                         .pass_request = {.unit = example.unit,
                                          .type_registry = test_fixture.registry,
                                          .diagnostic = &diagnostic},
                         .passes = {.passes = passes, .pass_count = 1},
                         .verifies_after_each_pass = false}) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_ir_run_passes(&(sxml_ir_pass_list_request){
                         .pass_request = {.unit = example.unit,
                                          .type_registry = test_fixture.registry,
                                          .diagnostic = &diagnostic},
                         .passes = {.passes = passes, .pass_count = 1},
                         .verifies_after_each_pass = true}) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, diagnostic.description.byte_count > 0);
    sxml_ir_unit_release(example.unit);
    fixture_destroy(&test_fixture);
}

/* ------------------------------------------------------ XQuery payloads */

/* The indices of the XQuery example unit, for tests that break it. */
typedef struct xquery_unit {
    sxml_ir_unit *unit;
    sxml_ir_expression_index try_catch;
    sxml_ir_expression_index element;
    sxml_ir_expression_index content;
    sxml_ir_expression_index finally_body;
} xquery_unit;

/* Each step of building the XQuery example unit does nothing once status
   is not OK. */
typedef struct xquery_writer {
    const fixture *test_fixture;
    sxml_ir_unit *unit;
    sxml_status status;
} xquery_writer;

static sxml_ir_expression_index xquery_add(xquery_writer *writer,
                                           sxml_ir_expression expression) {
    sxml_ir_expression_index index = {0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = add(writer->unit, expression, &index);
    }
    return index;
}

static sxml_ir_atom xquery_constant(xquery_writer *writer, sxml_sequence value) {
    sxml_ir_constant_index index = {0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_ir_unit_add_constant(writer->unit, value, &index);
    }
    sxml_sequence_release(value);
    return constant(index);
}

static sxml_ir_atom xquery_string(xquery_writer *writer, const char *text) {
    sxml_string string = {0};
    sxml_item item = {0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_string_from_utf8(sxml_byte_view_from_c_string(text), &string);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_atomic_item_make(
            writer->test_fixture->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = string}}},
            &item);
    }
    if (writer->status != SXML_STATUS_OK) {
        return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
    }
    return xquery_constant(writer, sxml_sequence_from_item(item));
}

/* The binding prefix="uri", interned. */
static sxml_namespace_binding xquery_binding(xquery_writer *writer,
                                             const char *prefix,
                                             const char *uri) {
    sxml_namespace_binding binding = {0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_prefix(
            writer->test_fixture->name_pool, sxml_byte_view_from_c_string(prefix),
            &binding.namespace_prefix);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_uri(
            writer->test_fixture->name_pool, sxml_byte_view_from_c_string(uri),
            &binding.namespace_uri);
    }
    return binding;
}

static sxml_ir_namespace_binding_list xquery_bindings(
    xquery_writer *writer, sxml_namespace_binding_view bindings) {
    sxml_ir_namespace_binding_list list = {0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status =
            sxml_ir_unit_add_namespace_bindings(writer->unit, bindings, &list);
    }
    return list;
}

/* The clauses of the example: `catch err:FOAR0001 | *:local` reading
   $err:code, and `catch *`. The local name comes from a buffer that is
   overwritten after the add, which the unit must not notice. */
static sxml_ir_catch_clause_list xquery_clauses(xquery_writer *writer) {
    char local_name[] = "local";
    sxml_name_identifier code = {0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_name(
            writer->test_fixture->name_pool,
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_ERRORS),
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FOAR0001")},
            &code);
    }
    const sxml_name_test tests[] = {
        {.name_test_kind = SXML_NAME_TEST_KIND_EXPANDED_NAME, .expanded_name = code},
        {.name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD,
         .local_name = {.bytes = (const uint8_t *)local_name,
                        .byte_count = sizeof(local_name) - 1}},
        {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME},
    };
    sxml_ir_name_test_list first_tests = {0};
    sxml_ir_name_test_list second_tests = {0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_ir_unit_add_name_tests(
            writer->unit, (sxml_ir_name_test_view){.name_tests = tests, .name_test_count = 2},
            &first_tests);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_ir_unit_add_name_tests(
            writer->unit,
            (sxml_ir_name_test_view){.name_tests = tests + 2, .name_test_count = 1},
            &second_tests);
    }
    memset(local_name, 'x', sizeof(local_name) - 1);
    const sxml_ir_expression_index lookup = xquery_add(
        writer, (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_LOOKUP,
                                     .lookup = {.value = slot(1),
                                                .key = xquery_string(writer, "code")}});
    const sxml_ir_expression_index empty = xquery_add(
        writer, (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE});
    const sxml_ir_catch_clause clauses[] = {
        {.error_tests = first_tests, .error_details = {.value = 1}, .handler = lookup},
        {.error_tests = second_tests, .error_details = {.value = 1}, .handler = empty},
    };
    sxml_ir_catch_clause_list list = {0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_ir_unit_add_catch_clauses(
            writer->unit,
            (sxml_ir_catch_clause_view){.catch_clauses = clauses, .catch_clause_count = 2},
            &list);
    }
    return list;
}

/*
 * Builds the unit of
 *
 *     let $v := try { 1 div 0 } catch err:FOAR0001 | *:local { $err:code }
 *               catch * { () } finally { () }
 *     return <p:a xmlns:p="urn:p" xmlns="urn:d">{$v}</p:a>
 *
 * with the element's name as the string "p:a" resolved with name
 * namespaces, and the flags no-preserve-namespaces and xslt-rules.
 */
static sxml_status build_xquery_unit(const fixture *test_fixture, xquery_unit *example) {
    const sxml_ir_unit_configuration configuration = unit_configuration(test_fixture);
    xquery_writer writer = {.test_fixture = test_fixture, .unit = NULL};
    writer.status =
        sxml_ir_unit_create(fixture_allocator(test_fixture), &configuration, &writer.unit);
    if (writer.status != SXML_STATUS_OK) {
        return writer.status;
    }
    sxml_type_identifier signature = {0};
    sxml_ir_function_index main_function = {0};
    writer.status = register_signature(test_fixture, 0, &signature);
    if (writer.status == SXML_STATUS_OK) {
        writer.status = sxml_ir_unit_add_function(
            writer.unit, &(sxml_ir_function){.signature = signature, .slot_count = 2},
            &main_function);
    }
    xquery_unit built = {.unit = writer.unit};
    const sxml_ir_expression_index division = xquery_add(
        &writer,
        (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
            .binary = {.operator = SXML_IR_BINARY_OPERATOR_DIVIDE,
                       .left_operand = xquery_constant(
                           &writer, sxml_sequence_from_item(sxml_atomic_item_from_integer(1))),
                       .right_operand = xquery_constant(
                           &writer, sxml_sequence_from_item(sxml_atomic_item_from_integer(0)))}});
    const sxml_ir_catch_clause_list clauses = xquery_clauses(&writer);
    built.finally_body = xquery_add(
        &writer, (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE});
    built.try_catch = xquery_add(
        &writer, (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH,
                                      .try_catch = {.body = division,
                                                    .catch_clauses = clauses,
                                                    .finally_body = built.finally_body}});
    const sxml_namespace_binding written[] = {
        xquery_binding(&writer, "p", "urn:p"), xquery_binding(&writer, "", "urn:d")};
    const sxml_ir_namespace_binding_list written_list = xquery_bindings(
        &writer, (sxml_namespace_binding_view){.namespace_bindings = written,
                                               .namespace_binding_count = 2});
    const sxml_ir_namespace_binding_list name_list = xquery_bindings(
        &writer, (sxml_namespace_binding_view){.namespace_bindings = written,
                                               .namespace_binding_count = 1});
    sxml_ir_atom_list content_atoms = {0};
    if (writer.status == SXML_STATUS_OK) {
        const sxml_ir_atom atoms[] = {slot(2)};
        writer.status = sxml_ir_unit_add_atoms(
            writer.unit, (sxml_ir_atom_view){.atoms = atoms, .atom_count = 1}, &content_atoms);
    }
    built.content = xquery_add(
        &writer, (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                      .sequence = content_atoms});
    built.element = xquery_add(
        &writer,
        (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
            .node_construction = {.name = xquery_string(&writer, "p:a"),
                                  .content = built.content,
                                  .namespace_bindings = written_list,
                                  .name_namespaces = name_list,
                                  .construction_flags =
                                      (uint32_t)SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES |
                                      (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES}});
    const sxml_ir_expression_index body = xquery_add(
        &writer, (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                                      .let = {.variable = {.value = 2},
                                              .bound_expression = built.try_catch,
                                              .body = built.element}});
    if (writer.status == SXML_STATUS_OK) {
        sxml_ir_function_replacement replacement = {
            .index = main_function,
            .function = *sxml_ir_unit_get_function(writer.unit, main_function)};
        replacement.function.body = body;
        writer.status = sxml_ir_unit_replace_function(writer.unit, &replacement);
    }
    if (writer.status != SXML_STATUS_OK) {
        sxml_ir_unit_release(writer.unit);
        return writer.status;
    }
    *example = built;
    return SXML_STATUS_OK;
}

static const char xquery_text[] =
    "(function 1 main (parameters 0) (captures) (slots 2)\n"
    "  (let $2 (try-catch\n"
    "            (binary divide 1 0)\n"
    "            (catch (err:FOAR0001 *:local) $1\n"
    "              (lookup $1 \"code\"))\n"
    "            (catch (*) $1\n"
    "              (sequence))\n"
    "            (finally\n"
    "              (sequence)))\n"
    "    (element-constructor \"p:a\" _ (namespaces p=\"urn:p\" \"\"=\"urn:d\") "
    "(name-namespaces p=\"urn:p\") no-preserve-namespaces xslt-rules\n"
    "      (sequence $2))))\n";

static void test_adders_copy_name_tests_clauses_and_bindings(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_ir_unit_configuration configuration = unit_configuration(&test_fixture);
    sxml_ir_unit *unit = NULL;
    SXML_TEST_REQUIRE(state, sxml_ir_unit_create(fixture_allocator(&test_fixture),
                                                 &configuration, &unit) == SXML_STATUS_OK);
    /* Empty views give empty lists. */
    sxml_ir_name_test_list tests = {.first_name_test_index = 7, .name_test_count = 7};
    sxml_ir_catch_clause_list clauses = {.first_catch_clause_index = 7};
    sxml_ir_namespace_binding_list bindings = {.first_namespace_binding_index = 7};
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_name_tests(unit, (sxml_ir_name_test_view){0},
                                                        &tests) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, tests.first_name_test_index == 0 && tests.name_test_count == 0);
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_catch_clauses(
                                unit, (sxml_ir_catch_clause_view){0}, &clauses) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, clauses.first_catch_clause_index == 0);
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_namespace_bindings(
                                unit, (sxml_namespace_binding_view){0}, &bindings) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, bindings.first_namespace_binding_index == 0);
    /* A wildcard's local name is the pool's copy; an empty one is
       refused and leaves the unit as it was. */
    char local_name[] = "name";
    const sxml_name_test wildcard[] = {
        {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME,
         .local_name = SXML_BYTE_VIEW_FROM_LITERAL("ignored")},
        {.name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD,
         .local_name = {.bytes = (const uint8_t *)local_name, .byte_count = 4}}};
    SXML_TEST_REQUIRE(state, sxml_ir_unit_add_name_tests(
                                 unit, (sxml_ir_name_test_view){.name_tests = wildcard,
                                                                .name_test_count = 2},
                                 &tests) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, tests.first_name_test_index == 1 && tests.name_test_count == 2);
    memset(local_name, 'x', 4);
    const sxml_ir_name_test_view stored = sxml_ir_unit_get_name_tests(unit, tests);
    SXML_TEST_REQUIRE(state, stored.name_test_count == 2);
    SXML_TEST_EXPECT(state, stored.name_tests[0].local_name.byte_count == 0);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(stored.name_tests[1].local_name,
                                                  SXML_BYTE_VIEW_FROM_LITERAL("name")));
    const sxml_name_test empty_wildcard = {
        .name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD};
    sxml_ir_name_test_list refused = tests;
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_name_tests(
                                unit, (sxml_ir_name_test_view){.name_tests = &empty_wildcard,
                                                               .name_test_count = 1},
                                &refused) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, refused.first_name_test_index == 1 && unit->name_test_count == 3);
    const sxml_ir_catch_clause clause = {.error_tests = tests,
                                         .error_details = {.value = 4},
                                         .handler = {.value = 5}};
    SXML_TEST_REQUIRE(state, sxml_ir_unit_add_catch_clauses(
                                 unit, (sxml_ir_catch_clause_view){.catch_clauses = &clause,
                                                                   .catch_clause_count = 1},
                                 &clauses) == SXML_STATUS_OK);
    const sxml_ir_catch_clause_view stored_clauses =
        sxml_ir_unit_get_catch_clauses(unit, clauses);
    SXML_TEST_EXPECT(state, stored_clauses.catch_clause_count == 1 &&
                                stored_clauses.catch_clauses[0].error_details.value == 4 &&
                                stored_clauses.catch_clauses[0].handler.value == 5);
    /* Static namespaces and constructor runs share one array. */
    const sxml_namespace_binding pairs[2] = {
        {.namespace_prefix = {.index = 0}, .namespace_uri = {.index = SXML_NAMESPACE_XML}},
        {.namespace_prefix = {.index = 1}, .namespace_uri = {.index = SXML_NAMESPACE_NONE}}};
    SXML_TEST_REQUIRE(state, sxml_ir_unit_set_static_namespaces(
                                 unit, (sxml_namespace_binding_view){
                                           .namespace_bindings = pairs,
                                           .namespace_binding_count = 1}) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_ir_unit_add_namespace_bindings(
                                 unit, (sxml_namespace_binding_view){
                                           .namespace_bindings = pairs,
                                           .namespace_binding_count = 2},
                                 &bindings) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, bindings.first_namespace_binding_index == 2 &&
                                bindings.namespace_binding_count == 2);
    SXML_TEST_EXPECT(state, sxml_ir_unit_get_static_namespaces(unit).namespace_binding_count ==
                                1);
    const sxml_namespace_binding_view run = sxml_ir_unit_get_namespace_bindings(unit, bindings);
    SXML_TEST_REQUIRE(state, run.namespace_binding_count == 2 && run.namespace_bindings != NULL);
    SXML_TEST_EXPECT(state, run.namespace_bindings[1].namespace_prefix.index == 1);
    /* Out of range: empty. */
    SXML_TEST_EXPECT(state, sxml_ir_unit_get_name_tests(
                                unit, (sxml_ir_name_test_list){.first_name_test_index = 2,
                                                               .name_test_count = 5})
                                    .name_test_count == 0);
    SXML_TEST_EXPECT(state,
                     sxml_ir_unit_get_catch_clauses(
                         unit, (sxml_ir_catch_clause_list){.first_catch_clause_index = 9,
                                                           .catch_clause_count = 1})
                             .catch_clause_count == 0);
    SXML_TEST_EXPECT(state, sxml_ir_unit_get_namespace_bindings(
                                unit, (sxml_ir_namespace_binding_list){
                                          .first_namespace_binding_index = 3,
                                          .namespace_binding_count = 2})
                                    .namespace_binding_count == 0);
    sxml_ir_unit_seal(unit);
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_name_tests(unit, (sxml_ir_name_test_view){0},
                                                        &tests) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_catch_clauses(
                                unit, (sxml_ir_catch_clause_view){.catch_clauses = &clause,
                                                                  .catch_clause_count = 1},
                                &clauses) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_ir_unit_add_namespace_bindings(
                                unit, (sxml_namespace_binding_view){0}, &bindings) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_ir_unit_release(unit);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

static void test_xquery_unit_writes_its_text_and_verifies(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    xquery_unit example = {0};
    SXML_TEST_REQUIRE(state, build_xquery_unit(&test_fixture, &example) == SXML_STATUS_OK);
    char text[1024];
    size_t byte_count = 0;
    SXML_TEST_EXPECT(state, sxml_ir_unit_write_text(
                                example.unit,
                                (sxml_memory_view){.bytes = (uint8_t *)text,
                                                   .byte_count = sizeof(text)},
                                &byte_count) == SXML_STATUS_OK);
    const bool is_expected = byte_count == sizeof(xquery_text) - 1 &&
                             memcmp(text, xquery_text, sizeof(xquery_text) - 1) == 0;
    SXML_TEST_EXPECT(state, is_expected);
    if (!is_expected && byte_count < sizeof(text)) {
        (void)fprintf(stderr, "%.*s", (int)byte_count, text);
    }
    sxml_diagnostic diagnostic = {0};
    SXML_TEST_EXPECT(state, verify(&test_fixture, example.unit, &diagnostic) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, diagnostic.description.byte_count == 0);
    sxml_ir_unit_release(example.unit);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

/* One way to break the XQuery example unit. */
typedef enum xquery_breakage {
    SXML_TEST_XQUERY_BREAKAGE_CLAUSE_WITHOUT_SLOT,
    SXML_TEST_XQUERY_BREAKAGE_CLAUSE_WITHOUT_TESTS,
    SXML_TEST_XQUERY_BREAKAGE_NOTHING_TO_CATCH_OR_FINISH,
    SXML_TEST_XQUERY_BREAKAGE_DETAILS_READ_OUTSIDE_HANDLER,
    SXML_TEST_XQUERY_BREAKAGE_UNKNOWN_FLAG,
    SXML_TEST_XQUERY_BREAKAGE_BINDINGS_OUT_OF_RANGE,
    SXML_TEST_XQUERY_BREAKAGE_FIELD_OF_ANOTHER_KIND,
    SXML_TEST_XQUERY_BREAKAGE_DOCUMENT_WITH_NAME,
} xquery_breakage;

static sxml_status break_xquery_unit(const xquery_unit *example, xquery_breakage kind) {
    sxml_ir_unit *unit = example->unit;
    sxml_ir_expression_replacement replacement = {
        .index = example->try_catch,
        .expression = *sxml_ir_unit_get_expression(unit, example->try_catch)};
    sxml_ir_node_construction *construction = &replacement.expression.node_construction;
    const sxml_ir_catch_clause_view clauses =
        sxml_ir_unit_get_catch_clauses(unit, replacement.expression.try_catch.catch_clauses);
    if (clauses.catch_clause_count == 0 || clauses.catch_clauses == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_ir_catch_clause first_clause = clauses.catch_clauses[0];
    sxml_status status = SXML_STATUS_OK;
    switch (kind) {
    case SXML_TEST_XQUERY_BREAKAGE_CLAUSE_WITHOUT_SLOT:
    case SXML_TEST_XQUERY_BREAKAGE_CLAUSE_WITHOUT_TESTS: {
        sxml_ir_catch_clause clause = first_clause;
        if (kind == SXML_TEST_XQUERY_BREAKAGE_CLAUSE_WITHOUT_SLOT) {
            clause.error_details = (sxml_ir_slot){.value = 0};
        } else {
            clause.error_tests = (sxml_ir_name_test_list){0};
        }
        status = sxml_ir_unit_add_catch_clauses(
            unit, (sxml_ir_catch_clause_view){.catch_clauses = &clause, .catch_clause_count = 1},
            &replacement.expression.try_catch.catch_clauses);
        break;
    }
    case SXML_TEST_XQUERY_BREAKAGE_NOTHING_TO_CATCH_OR_FINISH:
        /* The handlers and finally body become unreachable, which is
           allowed; a try with neither clauses nor finally is not. */
        replacement.expression.try_catch.catch_clauses = (sxml_ir_catch_clause_list){0};
        replacement.expression.try_catch.finally_body = (sxml_ir_expression_index){0};
        break;
    case SXML_TEST_XQUERY_BREAKAGE_DETAILS_READ_OUTSIDE_HANDLER:
        /* The finally clause reads the first clause's details slot. */
        replacement.index = example->finally_body;
        replacement.expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = slot(1)};
        break;
    case SXML_TEST_XQUERY_BREAKAGE_UNKNOWN_FLAG:
    case SXML_TEST_XQUERY_BREAKAGE_BINDINGS_OUT_OF_RANGE:
    case SXML_TEST_XQUERY_BREAKAGE_FIELD_OF_ANOTHER_KIND:
    case SXML_TEST_XQUERY_BREAKAGE_DOCUMENT_WITH_NAME:
        replacement.index = example->element;
        replacement.expression = *sxml_ir_unit_get_expression(unit, example->element);
        if (kind == SXML_TEST_XQUERY_BREAKAGE_UNKNOWN_FLAG) {
            construction->construction_flags |= 1U << 9U;
        } else if (kind == SXML_TEST_XQUERY_BREAKAGE_BINDINGS_OUT_OF_RANGE) {
            construction->name_namespaces.first_namespace_binding_index = 40;
        } else if (kind == SXML_TEST_XQUERY_BREAKAGE_FIELD_OF_ANOTHER_KIND) {
            /* A text constructor has no namespace bindings or content. */
            replacement.expression.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR;
            construction->name = (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
            construction->content = (sxml_ir_expression_index){0};
            construction->construction_flags = 0;
            construction->name_namespaces = (sxml_ir_namespace_binding_list){0};
        } else {
            /* A document constructor has no name. */
            replacement.expression.expression_kind =
                SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR;
            construction->namespace_bindings = (sxml_ir_namespace_binding_list){0};
            construction->name_namespaces = (sxml_ir_namespace_binding_list){0};
        }
        break;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_expression(unit, &replacement);
    }
    return status;
}

static void test_verifier_checks_try_catch_and_constructors(sxml_test_state *state) {
    static const xquery_breakage breakages[] = {
        SXML_TEST_XQUERY_BREAKAGE_CLAUSE_WITHOUT_SLOT,
        SXML_TEST_XQUERY_BREAKAGE_CLAUSE_WITHOUT_TESTS,
        SXML_TEST_XQUERY_BREAKAGE_NOTHING_TO_CATCH_OR_FINISH,
        SXML_TEST_XQUERY_BREAKAGE_DETAILS_READ_OUTSIDE_HANDLER,
        SXML_TEST_XQUERY_BREAKAGE_UNKNOWN_FLAG,
        SXML_TEST_XQUERY_BREAKAGE_BINDINGS_OUT_OF_RANGE,
        SXML_TEST_XQUERY_BREAKAGE_FIELD_OF_ANOTHER_KIND,
        SXML_TEST_XQUERY_BREAKAGE_DOCUMENT_WITH_NAME,
    };
    for (size_t breakage_index = 0; breakage_index < SXML_TEST_COUNT(breakages);
         breakage_index += 1) {
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        xquery_unit example = {0};
        SXML_TEST_REQUIRE(state, build_xquery_unit(&test_fixture, &example) == SXML_STATUS_OK);
        SXML_TEST_REQUIRE(state, break_xquery_unit(&example, breakages[breakage_index]) ==
                                     SXML_STATUS_OK);
        sxml_diagnostic diagnostic = {0};
        const sxml_status status = verify(&test_fixture, example.unit, &diagnostic);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, diagnostic.description.byte_count > 0);
        if (status != SXML_STATUS_INVALID_ARGUMENT) {
            (void)fprintf(stderr, "XQuery breakage %zu not found\n", breakage_index);
        }
        sxml_ir_unit_release(example.unit);
        SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
        fixture_destroy(&test_fixture);
    }
}

static void test_xquery_unit_survives_every_allocation_failure(sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(failing_allocation_number, &test_fixture) ==
                                     SXML_STATUS_OK);
        xquery_unit example = {0};
        sxml_status status = build_xquery_unit(&test_fixture, &example);
        if (status == SXML_STATUS_OK) {
            status = verify(&test_fixture, example.unit, NULL);
        }
        if (status == SXML_STATUS_OK) {
            char text[1024];
            size_t byte_count = 0;
            status = sxml_ir_unit_write_text(
                example.unit,
                (sxml_memory_view){.bytes = (uint8_t *)text, .byte_count = sizeof(text)},
                &byte_count);
        }
        sxml_ir_unit_release(example.unit);
        const sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(test_fixture.counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        SXML_TEST_EXPECT(state, live_cells(&test_fixture) == 0);
        fixture_destroy(&test_fixture);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 100);
    }
    /* The unit and its eight arrays, the verifier's tables and stack, the
       printer's stack. */
    SXML_TEST_EXPECT(state, failing_allocation_number > 10);
}

/* -------------------------------------------------- Allocation failures */

static void test_building_and_verifying_survive_every_allocation_failure(
    sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(failing_allocation_number,
                                                &test_fixture) == SXML_STATUS_OK);
        example_unit example = {0};
        sxml_status status = build_example_unit(&test_fixture, &example);
        if (status == SXML_STATUS_OK) {
            status = verify(&test_fixture, example.unit, NULL);
        }
        if (status == SXML_STATUS_OK) {
            char text[512];
            size_t byte_count = 0;
            status = sxml_ir_unit_write_text(
                example.unit,
                (sxml_memory_view){.bytes = (uint8_t *)text,
                                   .byte_count = sizeof(text)},
                &byte_count);
        }
        sxml_ir_unit_release(example.unit);
        const sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(test_fixture.counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        SXML_TEST_EXPECT(state, live_cells(&test_fixture) == 0);
        fixture_destroy(&test_fixture);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, counts.failed_allocation_count == 1);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 100);
    }
    /* The unit, five arrays, the verifier's three tables and its task
       stack, the printer's task stack. */
    SXML_TEST_EXPECT(state, failing_allocation_number > 8);
}

/* ----------------------------------------------------- Lowering seam */

/* An XPath AST of text, made with the default allocator. */
static sxml_status build_ast(const char *expression_text, sxml_ast **ast) {
    return sxml_xpath_build_ast(
        &(sxml_ast_build_request){
            .expression_text = sxml_byte_view_from_c_string(expression_text),
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("probe.xpath"),
            .allocator = sxml_default_allocator(),
            .maximum_expression_depth = 0,
            .error_record = NULL},
        ast);
}

/* A unit, a lowerer over it and its main function, entered. */
typedef struct main_function {
    sxml_xpath_static_context static_context;
    sxml_ir_unit *unit;
    sxml_xpath_lowerer *lowerer;
    sxml_ir_function_index function_index;
    sxml_xpath_lowering_function_frame frame;
    sxml_error_record error_record;
} main_function;

typedef struct main_request {
    /* For lower_main: the main function's body. */
    const char *expression_text;
    sxml_xpath_lowering_part_view parts;
    sxml_language language;
    uint32_t maximum_expression_depth;
} main_request;

/* Makes the unit and lowerer and enters the main function, which reads
   its focus from slots; seam must not move until end_main. */
static sxml_status begin_main(const fixture *test_fixture, main_request request,
                              main_function *seam) {
    seam->static_context = (sxml_xpath_static_context){
        .item_heap = test_fixture->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .namespace_bindings = {.namespace_bindings = NULL,
                               .namespace_binding_count = 0},
        .variable_declarations = {.variable_declarations = NULL,
                                  .variable_declaration_count = 0},
        .function_table = {.state = NULL, .operations = NULL},
        .static_base_uri = {.bytes = NULL, .byte_count = 0}};
    seam->error_record = (sxml_error_record){0};
    seam->unit = NULL;
    seam->lowerer = NULL;
    const sxml_ir_unit_configuration configuration =
        unit_configuration(test_fixture);
    sxml_status status =
        sxml_ir_unit_create(fixture_allocator(test_fixture), &configuration,
                            &seam->unit);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_create(
            fixture_allocator(test_fixture),
            &(sxml_xpath_lowerer_configuration){
                .static_context = &seam->static_context,
                .unit = seam->unit,
                .language = request.language,
                .parts = request.parts,
                .maximum_expression_depth = request.maximum_expression_depth,
                .error_record = &seam->error_record},
            &seam->lowerer);
    }
    sxml_type_identifier signature = {0};
    if (status == SXML_STATUS_OK) {
        status = register_signature(test_fixture, 0, &signature);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            seam->unit, &(sxml_ir_function){.signature = signature},
            &seam->function_index);
    }
    if (status == SXML_STATUS_OK) {
        seam->frame = (sxml_xpath_lowering_function_frame){
            .function = {.function_index = seam->function_index,
                         .function_level = 0,
                         .slot_count = 0,
                         .captures = NULL,
                         .capture_count = 0},
            .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS}};
        sxml_xpath_lowerer_enter_function(seam->lowerer, &seam->frame);
    }
    return status;
}

/* Leaves the main function and gives it body. */
static sxml_status end_main(main_function *seam, sxml_ir_expression_index body) {
    sxml_xpath_lowerer_leave_function(seam->lowerer, &seam->frame);
    sxml_ir_function_replacement replacement = {
        .index = seam->function_index,
        .function = *sxml_ir_unit_get_function(seam->unit, seam->function_index)};
    replacement.function.body = body;
    replacement.function.slot_count = seam->frame.function.slot_count;
    replacement.function.focus = seam->frame.focus.slots;
    return sxml_ir_unit_replace_function(seam->unit, &replacement);
}

static void release_main(main_function *seam) {
    sxml_xpath_lowerer_destroy(seam->lowerer);
    sxml_ir_unit_release(seam->unit);
}

/* Whether unit's text is expected_text. */
static bool unit_text_is(const sxml_ir_unit *unit, const char *expected_text) {
    char text[1024];
    size_t byte_count = 0;
    if (sxml_ir_unit_write_text(unit,
                                (sxml_memory_view){.bytes = (uint8_t *)text,
                                                   .byte_count = sizeof(text)},
                                &byte_count) != SXML_STATUS_OK) {
        return false;
    }
    const bool is_equal = byte_count == strlen(expected_text) &&
                          memcmp(text, expected_text, byte_count) == 0;
    if (!is_equal) {
        (void)fprintf(stderr, "unit text:\n%.*s", (int)byte_count, text);
    }
    return is_equal;
}

/* Probe procedure for "text { E }": E lowered to an atom, then cast to
   xs:string. */
typedef struct probe_text_locals {
    sxml_ir_atom content;
} probe_text_locals;

static sxml_status continue_probe_text(sxml_xpath_lowering_machine *machine,
                                       sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    probe_text_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return sxml_xpath_lowering_call_atom(
            machine,
            (sxml_xpath_lowering_atom_call){
                .node = sxml_ast_find_child(
                    lowerer->ast,
                    (sxml_ast_child_query){.parent = frame->node,
                                           .field = SXML_AST_FIELD_CONTENT}),
                .block = frame->block,
                .atom = &locals->content});
    }
    sxml_ir_expression_index cast = {0};
    const sxml_status status = sxml_xpath_lowerer_add_expression(
        lowerer,
        (sxml_xpath_lowering_expression_to_add){
            .node = frame->node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_CAST,
                           .cast = {.value = locals->content,
                                    .target_type = sxml_builtin_type_identifier(
                                        SXML_TYPE_XS_STRING),
                                    .allows_empty = false}}},
        &cast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, cast);
}

static const sxml_xpath_lowering_procedure probe_text_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("probe text"),
    .step = continue_probe_text,
    .locals_size = sizeof(probe_text_locals)};

/* Probe leaf for "comment { E }": the string "probe", E not lowered. */
static sxml_status lower_probe_comment(sxml_xpath_lowerer *lowerer,
                                       sxml_xpath_lowering_node_in_block request,
                                       sxml_ir_expression_index *expression) {
    sxml_ir_atom text = {0};
    const sxml_status status = sxml_xpath_lowerer_add_string_constant(
        lowerer, SXML_BYTE_VIEW_FROM_LITERAL("probe"), &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_expression(
        lowerer,
        (sxml_xpath_lowering_expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                           .atom = text}},
        expression);
}

static const sxml_xpath_lowering_rule probe_rules[] = {
    {.kind = SXML_AST_KIND_COMPUTED_TEXT_CONSTRUCTOR,
     .procedure = &probe_text_procedure,
     .leaf = NULL},
    {.kind = SXML_AST_KIND_COMPUTED_COMMENT_CONSTRUCTOR,
     .procedure = NULL,
     .leaf = lower_probe_comment},
};

static const sxml_xpath_lowering_part probe_part = {
    .languages = SXML_LANGUAGE_XPATH,
    .rules = probe_rules,
    .rule_count = SXML_TEST_COUNT(probe_rules)};

/* An XQuery-only part that claims the same kind: skipped for XPath. */
static const sxml_xpath_lowering_rule xquery_probe_rules[] = {
    {.kind = SXML_AST_KIND_COMPUTED_TEXT_CONSTRUCTOR,
     .procedure = NULL,
     .leaf = lower_probe_comment},
};

static const sxml_xpath_lowering_part xquery_probe_part = {
    .languages = SXML_LANGUAGE_XQUERY,
    .rules = xquery_probe_rules,
    .rule_count = SXML_TEST_COUNT(xquery_probe_rules)};

static const sxml_xpath_lowering_registered_part probe_parts[] = {
    {.part_name = SXML_BYTE_VIEW_INITIALIZER("xquery_probe"),
     .part = &xquery_probe_part},
    {.part_name = SXML_BYTE_VIEW_INITIALIZER("probe"), .part = &probe_part},
};

static const sxml_xpath_lowering_part_view probe_part_view = {
    .parts = probe_parts, .part_count = SXML_TEST_COUNT(probe_parts)};

/* Lowers text as the main function's body with request's parts. */
static sxml_status lower_main(const fixture *test_fixture, main_request request,
                              main_function *seam) {
    *seam = (main_function){.unit = NULL, .lowerer = NULL};
    sxml_ast *ast = NULL;
    sxml_status status = build_ast(request.expression_text, &ast);
    if (status == SXML_STATUS_OK) {
        status = begin_main(test_fixture, request, seam);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_lower_embedded(
            seam->lowerer,
            &(sxml_xpath_lowering_embedding){.ast = ast,
                                             .node = {.value = 0},
                                             .static_context = NULL,
                                             .focus = NULL},
            &body);
    }
    if (status == SXML_STATUS_OK) {
        status = end_main(seam, body);
    }
    sxml_ast_destroy(ast);
    return status;
}

static const char probe_text[] =
    "(function 1 main (parameters 0) (captures) (slots 2)\n"
    "  (let $1 (binary add 1 2)\n"
    "    (let $2 (cast $1 xs:string)\n"
    "      (sequence $2 \"probe\"))))\n";

static void test_lowering_part_lowers_its_kinds(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    main_function seam;
    SXML_TEST_EXPECT(
        state, lower_main(&test_fixture,
                          (main_request){.expression_text =
                                             "text { 1 + 2 }, comment { 3 }",
                                         .parts = probe_part_view,
                                         .language = SXML_LANGUAGE_XPATH},
                          &seam) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, unit_text_is(seam.unit, probe_text));
    SXML_TEST_EXPECT(state, verify(&test_fixture, seam.unit, NULL) ==
                                SXML_STATUS_OK);
    release_main(&seam);
    /* For XQuery the first part claims "text { }". */
    SXML_TEST_EXPECT(
        state, lower_main(&test_fixture,
                          (main_request){.expression_text = "text { 1 + 2 }",
                                         .parts = probe_part_view,
                                         .language = SXML_LANGUAGE_XQUERY},
                          &seam) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, unit_text_is(
                                seam.unit,
                                "(function 1 main (parameters 0) "
                                "(captures) (slots 0)\n  \"probe\")\n"));
    release_main(&seam);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    SXML_TEST_EXPECT(state, live_cells(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

static void test_kinds_no_part_lowers_are_the_cores(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    main_function seam;
    /* With only a part for XQuery, the core lowers XPath's computed text
       constructor, which it reports as not lowered (the registered part
       `construct` serves every language since packet 2.19). */
    const sxml_xpath_lowering_part_view xquery_probe_only = {.parts = probe_parts,
                                                             .part_count = 1};
    SXML_TEST_EXPECT(state,
                     lower_main(&test_fixture,
                                (main_request){.expression_text = "text { 1 }",
                                               .parts = xquery_probe_only,
                                               .language = SXML_LANGUAGE_XPATH},
                                &seam) == SXML_STATUS_UNSUPPORTED_FEATURE);
    SXML_TEST_EXPECT(state, seam.error_record.diagnostic.location.column_number ==
                                1);
    release_main(&seam);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

/* Each registered part has a name and rules of one kind per language:
   no two parts lower the same kind for a language both serve. */
static void test_registered_parts_claim_each_kind_once(sxml_test_state *state) {
    const sxml_xpath_lowering_part_view parts =
        sxml_xpath_lowering_registered_parts();
    for (size_t part_index = 0; part_index < parts.part_count; part_index += 1) {
        const sxml_xpath_lowering_part *part = parts.parts[part_index].part;
        SXML_TEST_REQUIRE(state, part != NULL);
        SXML_TEST_EXPECT(state, parts.parts[part_index].part_name.byte_count > 0);
        SXML_TEST_EXPECT(state, (part->languages & ~SXML_LANGUAGE_SET_ALL) == 0);
        for (size_t rule_index = 0; rule_index < part->rule_count;
             rule_index += 1) {
            const sxml_xpath_lowering_rule *rule = &part->rules[rule_index];
            SXML_TEST_EXPECT(state,
                             (rule->procedure == NULL) != (rule->leaf == NULL));
            for (size_t other_index = 0; other_index <= part_index;
                 other_index += 1) {
                const sxml_xpath_lowering_part *other =
                    parts.parts[other_index].part;
                const size_t other_rule_count =
                    other_index == part_index ? rule_index : other->rule_count;
                for (size_t other_rule = 0; other_rule < other_rule_count;
                     other_rule += 1) {
                    SXML_TEST_EXPECT(
                        state, other->rules[other_rule].kind != rule->kind ||
                                   (other->languages & part->languages) == 0);
                }
            }
        }
    }
}

/* "$x + ." inside a caller's scope: $x is the caller's slot, the focus a
   fixed value of the caller's. */
static sxml_status lower_in_scope(const fixture *test_fixture,
                                  main_function *seam) {
    *seam = (main_function){.unit = NULL, .lowerer = NULL};
    sxml_ast *ast = NULL;
    sxml_status status = build_ast("$x + .", &ast);
    if (status == SXML_STATUS_OK) {
        status =
            begin_main(test_fixture,
                       (main_request){.language = SXML_LANGUAGE_XPATH}, seam);
    }
    sxml_name_identifier name = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            test_fixture->name_pool,
            &(sxml_name_request){.namespace_uri = {.index = 0},
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("x")},
            &name);
    }
    sxml_ir_slot variable_slot = {0};
    sxml_ir_slot focus_slot = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(seam->lowerer, &variable_slot);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(seam->lowerer, &focus_slot);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_push_variable(
            seam->lowerer,
            (sxml_xpath_lowering_scope_entry){.variable_name = name,
                                              .function_level = 0,
                                              .atom = slot(variable_slot.value)});
    }
    sxml_xpath_lowering_focus focus = {
        .kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED,
        .fixed_value = slot(focus_slot.value)};
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_lower_embedded(
            seam->lowerer,
            &(sxml_xpath_lowering_embedding){.ast = ast,
                                             .node = {.value = 0},
                                             .static_context = NULL,
                                             .focus = &focus},
            &body);
    }
    if (status == SXML_STATUS_OK) {
        /* The lowerer has its own focus and no AST again. */
        status = seam->lowerer->ast == NULL &&
                         seam->lowerer->function->focus == &seam->frame.focus
                     ? end_main(seam, body)
                     : SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ast_destroy(ast);
    return status;
}

static void test_embedding_lowers_in_the_callers_scope(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    main_function seam;
    SXML_TEST_EXPECT(state, lower_in_scope(&test_fixture, &seam) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     unit_text_is(seam.unit,
                                  "(function 1 main (parameters 0) "
                                  "(captures) (slots 2)\n"
                                  "  (binary add $1 $2))\n"));
    release_main(&seam);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

/* A procedure pushed from outside a machine that embeds an AST from its
   initial locals in its own block, as XSLT instruction lowering does. */
typedef struct probe_outer_locals {
    const sxml_ast *ast;
    sxml_ir_atom value;
} probe_outer_locals;

static sxml_status continue_probe_outer(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    probe_outer_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return sxml_xpath_lowering_call_embedded(
            machine,
            &(sxml_xpath_lowering_embedded_call){
                .embedding = {.ast = locals->ast,
                              .node = {.value = 0},
                              .static_context = NULL,
                              .focus = NULL},
                .block = frame->block,
                .is_body = false,
                .destination = {.atom = &locals->value,
                                .atom_block = frame->block}});
    }
    sxml_ir_expression_index result = {0};
    const sxml_status status = sxml_xpath_lowerer_add_expression(
        machine->state,
        (sxml_xpath_lowering_expression_to_add){
            .node = {.value = 0},
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                           .atom = locals->value}},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

static const sxml_xpath_lowering_procedure probe_outer_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("probe outer"),
    .step = continue_probe_outer,
    .locals_size = sizeof(probe_outer_locals)};

static sxml_status run_outer_procedure(const fixture *test_fixture,
                                       main_function *seam) {
    *seam = (main_function){.unit = NULL, .lowerer = NULL};
    sxml_ast *ast = NULL;
    sxml_status status = build_ast("(1 to 3) ! (. * 2)", &ast);
    if (status == SXML_STATUS_OK) {
        status =
            begin_main(test_fixture,
                       (main_request){.language = SXML_LANGUAGE_XPATH}, seam);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        const probe_outer_locals initial = {.ast = ast, .value = {0}};
        status = sxml_xpath_lowerer_run_procedure(
            seam->lowerer,
            &(sxml_xpath_lowering_procedure_call){
                .procedure = &probe_outer_procedure,
                .node = {.value = 0},
                .block = NULL,
                .is_body = true,
                .destination = {.expression = &body},
                .initial_locals = {.bytes = (const uint8_t *)&initial,
                                   .byte_count = sizeof(initial)}});
    }
    if (status == SXML_STATUS_OK) {
        status = end_main(seam, body);
    }
    sxml_ast_destroy(ast);
    return status;
}

static const char outer_text[] =
    "(function 1 main (parameters 0) (captures) (slots 3)\n"
    "  (let $1 (binary range 1 3)\n"
    "    (let $3 (map $1 (focus $2 _ _)\n"
    "              (binary multiply $2 2))\n"
    "      $3)))\n";

static void test_procedures_run_from_outside_embed_asts(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    main_function seam;
    SXML_TEST_EXPECT(state, run_outer_procedure(&test_fixture, &seam) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, unit_text_is(seam.unit, outer_text));
    release_main(&seam);
    /* Nesting beyond maximum_expression_depth: the procedure holds one
       level, the embedded expression needs more. */
    sxml_ast *ast = NULL;
    SXML_TEST_REQUIRE(state, build_ast("1", &ast) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      begin_main(&test_fixture,
                                 (main_request){.language = SXML_LANGUAGE_XPATH,
                                                .maximum_expression_depth = 1},
                                 &seam) == SXML_STATUS_OK);
    sxml_ir_expression_index body = {0};
    const probe_outer_locals initial = {.ast = ast, .value = {0}};
    SXML_TEST_EXPECT(state,
                     sxml_xpath_lowerer_run_procedure(
                         seam.lowerer,
                         &(sxml_xpath_lowering_procedure_call){
                             .procedure = &probe_outer_procedure,
                             .is_body = true,
                             .destination = {.expression = &body},
                             .initial_locals = {.bytes = (const uint8_t *)&initial,
                                                .byte_count = sizeof(initial)}}) ==
                         SXML_STATUS_LIMIT_EXCEEDED);
    /* Initial locals larger than the locals. */
    const uint8_t too_many[sizeof(probe_outer_locals) + 1] = {0};
    SXML_TEST_EXPECT(state,
                     sxml_xpath_lowerer_run_procedure(
                         seam.lowerer,
                         &(sxml_xpath_lowering_procedure_call){
                             .procedure = &probe_outer_procedure,
                             .is_body = true,
                             .destination = {.expression = &body},
                             .initial_locals = {.bytes = too_many,
                                                .byte_count = sizeof(too_many)}}) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    release_main(&seam);
    sxml_ast_destroy(ast);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

static void test_lowerer_lifecycle_checks_its_configuration(
    sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    main_function seam;
    SXML_TEST_REQUIRE(
        state, begin_main(&test_fixture,
                          (main_request){.language = SXML_LANGUAGE_XPATH},
                          &seam) == SXML_STATUS_OK);
    sxml_xpath_lowerer_configuration configuration = {
        .static_context = &seam.static_context, .unit = seam.unit};
    sxml_memory_requirements requirements = {0};
    SXML_TEST_EXPECT(state, sxml_xpath_lowerer_memory_requirements(
                                &configuration, &requirements) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, requirements.byte_count == sizeof(sxml_xpath_lowerer));
    alignas(sxml_xpath_lowerer) uint8_t memory[sizeof(sxml_xpath_lowerer) + 1];
    sxml_xpath_lowerer *lowerer = NULL;
    SXML_TEST_EXPECT(state, sxml_xpath_lowerer_initialize(
                                (sxml_memory_view){.bytes = memory,
                                                   .byte_count = sizeof(memory) - 2},
                                &configuration,
                                &lowerer) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, sxml_xpath_lowerer_initialize(
                                (sxml_memory_view){.bytes = memory + 1,
                                                   .byte_count = sizeof(memory) - 1},
                                &configuration,
                                &lowerer) == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, lowerer == NULL);
    SXML_TEST_EXPECT(state, sxml_xpath_lowerer_initialize(
                                (sxml_memory_view){.bytes = memory,
                                                   .byte_count = sizeof(memory)},
                                &configuration, &lowerer) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, lowerer != NULL && lowerer->language ==
                                                   SXML_LANGUAGE_XPATH);
    sxml_xpath_lowerer_finalize(lowerer);
    sxml_xpath_lowerer_finalize(NULL);
    sxml_xpath_lowerer_destroy(NULL);
    configuration.language = SXML_LANGUAGE_XSLT;
    configuration.unit = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_xpath_lowerer_create(fixture_allocator(&test_fixture),
                                               &configuration, &lowerer) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    /* An embedding with another item heap's static context. */
    sxml_xpath_static_context other_context = seam.static_context;
    other_context.item_heap = NULL;
    sxml_ast *ast = NULL;
    SXML_TEST_REQUIRE(state, build_ast("1", &ast) == SXML_STATUS_OK);
    sxml_ir_expression_index body = {0};
    SXML_TEST_EXPECT(state, sxml_xpath_lowerer_lower_embedded(
                                seam.lowerer,
                                &(sxml_xpath_lowering_embedding){
                                    .ast = ast, .static_context = &other_context},
                                &body) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_ast_destroy(ast);
    release_main(&seam);
    SXML_TEST_EXPECT(state, live_allocations(&test_fixture) == 0);
    fixture_destroy(&test_fixture);
}

/* What one run of the seam's cases needs: a probe part, an embedding in
   scope, an outer procedure. */
static sxml_status run_seam_cases(const fixture *test_fixture) {
    main_function seam;
    sxml_status status = lower_main(
        test_fixture,
        (main_request){.expression_text = "text { 1 + 2 }, comment { 3 }",
                       .parts = probe_part_view,
                       .language = SXML_LANGUAGE_XPATH},
        &seam);
    release_main(&seam);
    if (status == SXML_STATUS_OK) {
        status = lower_in_scope(test_fixture, &seam);
        release_main(&seam);
    }
    if (status == SXML_STATUS_OK) {
        status = run_outer_procedure(test_fixture, &seam);
        release_main(&seam);
    }
    return status;
}

static void test_lowering_seam_survives_every_allocation_failure(
    sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(failing_allocation_number,
                                                &test_fixture) == SXML_STATUS_OK);
        const sxml_status status = run_seam_cases(&test_fixture);
        const sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(test_fixture.counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        SXML_TEST_EXPECT(state, live_cells(&test_fixture) == 0);
        fixture_destroy(&test_fixture);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, counts.failed_allocation_count == 1);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 400);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 20);
}

static const sxml_test_case ir_test_cases[] = {
    {.name = "unit_initializes_in_caller_memory_and_copies_source_name",
     .function = test_unit_initializes_in_caller_memory_and_copies_source_name},
    {.name = "unit_rejects_invalid_configuration",
     .function = test_unit_rejects_invalid_configuration},
    {.name = "unit_keeps_constants_until_its_last_release",
     .function = test_unit_keeps_constants_until_its_last_release},
    {.name = "accessors_give_zero_entries_out_of_range",
     .function = test_accessors_give_zero_entries_out_of_range},
    {.name = "sealed_unit_and_limits_refuse_additions",
     .function = test_sealed_unit_and_limits_refuse_additions},
    {.name = "example_unit_writes_its_text_form",
     .function = test_example_unit_writes_its_text_form},
    {.name = "verifier_accepts_the_example_unit",
     .function = test_verifier_accepts_the_example_unit},
    {.name = "verifier_names_each_broken_invariant",
     .function = test_verifier_names_each_broken_invariant},
    {.name = "verifier_reports_the_span_of_the_broken_expression",
     .function = test_verifier_reports_the_span_of_the_broken_expression},
    {.name = "pass_list_verifies_after_each_pass",
     .function = test_pass_list_verifies_after_each_pass},
    {.name = "building_and_verifying_survive_every_allocation_failure",
     .function = test_building_and_verifying_survive_every_allocation_failure},
    {.name = "adders_copy_name_tests_clauses_and_bindings",
     .function = test_adders_copy_name_tests_clauses_and_bindings},
    {.name = "xquery_unit_writes_its_text_and_verifies",
     .function = test_xquery_unit_writes_its_text_and_verifies},
    {.name = "verifier_checks_try_catch_and_constructors",
     .function = test_verifier_checks_try_catch_and_constructors},
    {.name = "xquery_unit_survives_every_allocation_failure",
     .function = test_xquery_unit_survives_every_allocation_failure},
    {.name = "lowering_part_lowers_its_kinds",
     .function = test_lowering_part_lowers_its_kinds},
    {.name = "kinds_no_part_lowers_are_the_cores",
     .function = test_kinds_no_part_lowers_are_the_cores},
    {.name = "registered_parts_claim_each_kind_once",
     .function = test_registered_parts_claim_each_kind_once},
    {.name = "embedding_lowers_in_the_callers_scope",
     .function = test_embedding_lowers_in_the_callers_scope},
    {.name = "procedures_run_from_outside_embed_asts",
     .function = test_procedures_run_from_outside_embed_asts},
    {.name = "lowerer_lifecycle_checks_its_configuration",
     .function = test_lowerer_lifecycle_checks_its_configuration},
    {.name = "lowering_seam_survives_every_allocation_failure",
     .function = test_lowering_seam_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "ir",
            .cases = ir_test_cases,
            .case_count = SXML_TEST_COUNT(ir_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
