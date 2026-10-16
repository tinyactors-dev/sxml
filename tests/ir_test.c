/*
 * Tests for the IR unit, its text form, the verifier and the pass list
 * (src/ir.c, src/ir_verify.c; src/ir_internal.h, src/ir_pass_internal.h;
 * D46, D55, D63).
 *
 * Golden file: tests/memory_test.c: lifecycle checks, then behaviour, then
 * allocation-failure loops (D23). Units are built by hand here; lowering
 * builds them in tests/xpath_static_test.c.
 */
#include "ir_internal.h"
#include "ir_pass_internal.h"
#include "sxml/name.h"
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
    SXML_TEST_EXPECT(state, sxml_ir_standard_passes.pass_count == 1);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_ir_standard_passes.passes[0].pass_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("verify")));
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
            SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR;
        replacement.expression.node_construction =
            (sxml_ir_node_construction){.value = slot(1)};
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
