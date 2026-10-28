/*
 * Tests for the try/catch evaluator part (src/evaluator_try.c, D101,
 * D109) over hand-built IR.
 *
 * Golden file: tests/evaluator_machine_test.c (fixture, hand-built units,
 * allocation-failure loops), with the resumable bodies of
 * tests/evaluator_test.c.
 *
 * Specification: XQuery 4.0, section 4.20 "Try/Catch Expressions".
 *
 * Catch clauses and their name tests go into the unit through its adders
 * (sxml_ir_unit_add_name_tests, sxml_ir_unit_add_catch_clauses; D109).
 */
#include "evaluator_machine_internal.h"
#include "function_library_internal.h"
#include "ir_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

#define EXAMPLE_NAMESPACE_URI "http://example.com/errors"
#define UNIT_SOURCE_NAME "try-test"

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* ------------------------------------------------------------ Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    alignas(16) uint8_t counting_storage[256];
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
    sxml_dynamic_context *dynamic_context;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number,
                                  fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL};
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
            &(sxml_type_registry_configuration){.allocator = sxml_default_allocator(),
                                                .name_pool = test_fixture->name_pool,
                                                .maximum_registered_type_count = 0},
            &test_fixture->registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            fixture_allocator(test_fixture),
            &(sxml_item_heap_configuration){.allocator = fixture_allocator(test_fixture),
                                            .name_pool = test_fixture->name_pool,
                                            .type_registry = test_fixture->registry,
                                            .maximum_byte_count = 0},
            &test_fixture->item_heap);
    }
    if (status == SXML_STATUS_OK) {
        sxml_evaluator_configuration configuration;
        memset(&configuration, 0, sizeof(configuration));
        configuration.item_heap = test_fixture->item_heap;
        configuration.allocator = fixture_allocator(test_fixture);
        status = sxml_dynamic_context_create(fixture_allocator(test_fixture), &configuration,
                                             &test_fixture->dynamic_context);
    }
    return status;
}

/* Destroys the fixture; true when every block and cell was released. */
static bool fixture_destroy(fixture *test_fixture) {
    sxml_dynamic_context_destroy(test_fixture->dynamic_context);
    bool released = true;
    if (test_fixture->item_heap != NULL) {
        sxml_item_heap_clear_raised_error(test_fixture->item_heap);
        released = sxml_item_heap_read_counts(test_fixture->item_heap).live_cell_count == 0;
    }
    sxml_item_heap_destroy(test_fixture->item_heap);
    sxml_type_registry_destroy(test_fixture->registry);
    sxml_name_pool_destroy(test_fixture->name_pool);
    if (test_fixture->counting_allocator != NULL) {
        released = released &&
                   sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
                           .live_allocation_count == 0;
        sxml_counting_allocator_finalize(test_fixture->counting_allocator);
    }
    *test_fixture = (fixture){.counting_allocator = NULL};
    return released;
}

/* --------------------------------------------------------- Test rows */

static const sxml_static_error lazy_failure_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER("the lazy sequence failed"),
};

/* A generator whose first item is an error. */
static sxml_status next_failing_item(void *state, const sxml_generation_request *request,
                                     sxml_iteration_step *step) {
    (void)state;
    (void)step;
    return sxml_error_record_report(request->error_record, &lazy_failure_error);
}

static const sxml_sequence_generator_operations failing_generator_operations = {
    .next = next_failing_item,
    .release = NULL,
};

/* lazy-failure(): a lazy sequence that raises FOAR0002 when read. */
static sxml_status call_lazy_failure(const sxml_function_call *call, sxml_sequence *result) {
    return sxml_sequence_make_lazy(
        call->item_heap,
        (sxml_sequence_generator){.state = NULL, .operations = &failing_generator_operations},
        result);
}

/* call-action($action): calls $action without arguments (one native
   step), then finishes with its value. */
static sxml_status step_call_action(const sxml_native_step_request *request,
                                    sxml_native_step *step) {
    if (request->step_number == 0) {
        *step = (sxml_native_step){
            .step_kind = SXML_NATIVE_STEP_KIND_CALL,
            .result = sxml_sequence_empty(),
            .function_item = request->call->arguments.sequences[0].content,
            .arguments = {.sequences = NULL, .sequence_count = 0}};
        return SXML_STATUS_OK;
    }
    *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                               .result = request->call_result};
    return SXML_STATUS_OK;
}

/* fail-after-action($action): calls $action, then raises
   ex:LATE with a computed description and the action's value as
   $err:value. */
static sxml_status step_fail_after_action(const sxml_native_step_request *request,
                                          sxml_native_step *step) {
    if (request->step_number == 0) {
        return step_call_action(request, step);
    }
    const sxml_status status = sxml_item_heap_raise_error(
        request->call->item_heap,
        &(sxml_error_raise_request){
            .error_status = SXML_STATUS_DYNAMIC_ERROR,
            .code = {.namespace_uri = SXML_BYTE_VIEW_INITIALIZER(EXAMPLE_NAMESPACE_URI),
                     .local_name = SXML_BYTE_VIEW_INITIALIZER("LATE"),
                     .namespace_prefix = SXML_BYTE_VIEW_INITIALIZER("ex")},
            .description = SXML_BYTE_VIEW_INITIALIZER("late failure"),
            .has_value = true,
            .value = request->call_result,
            .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}},
        request->call->error_record);
    sxml_sequence_release(request->call_result);
    return status;
}

static const sxml_resumable_native_body call_action_body = {
    .step = step_call_action,
    .release_state = NULL,
    .state_byte_count = 0,
    .state_alignment = 1,
};

static const sxml_resumable_native_body fail_after_action_body = {
    .step = step_fail_after_action,
    .release_state = NULL,
    .state_byte_count = 0,
    .state_alignment = 1,
};

static const sxml_function_parameter action_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("action"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("fn() as item()*"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
};

static const sxml_native_function call_action_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("call-action"),
    .parameters = {action_parameters, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = NULL,
    .resumable_body = &call_action_body,
};

static const sxml_native_function fail_after_action_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("fail-after-action"),
    .parameters = {action_parameters, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = NULL,
    .resumable_body = &fail_after_action_body,
};

static const sxml_native_function lazy_failure_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("lazy-failure"),
    .parameters = {NULL, 0},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = call_lazy_failure,
    .resumable_body = NULL,
};

/* fn:error of the library (src/function_library_error.c). */
static const sxml_native_function *find_error_row(void) {
    const sxml_native_function_view view = sxml_function_library_error;
    for (size_t row_index = 0; row_index < view.native_function_count; row_index += 1) {
        if (sxml_byte_view_equals(view.native_functions[row_index].local_name,
                                  text_view("error"))) {
            return &view.native_functions[row_index];
        }
    }
    return NULL;
}

/* ------------------------------------------------------- Unit builder */

#define CLAUSE_TEST_LIMIT 3
#define TRY_CLAUSE_LIMIT 3
#define MAIN_SLOT_COUNT 4

/* A unit being built; the first failure is kept in status and every later
   step does nothing. */
typedef struct unit_builder {
    const fixture *test_fixture;
    sxml_ir_unit *unit;
    sxml_status status;
} unit_builder;

static sxml_ir_expression_index add_expression(unit_builder *builder,
                                               const sxml_ir_expression *expression) {
    sxml_ir_expression_index index = {0};
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_ir_unit_add_expression(builder->unit, expression, &index);
    }
    return index;
}

/* Adds value (owned: released here) as a constant. */
static sxml_ir_atom add_constant(unit_builder *builder, sxml_sequence value) {
    sxml_ir_constant_index index = {0};
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_ir_unit_add_constant(builder->unit, value, &index);
    }
    sxml_sequence_release(value);
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_CONSTANT, .constant = index};
}

static sxml_ir_atom slot_atom(uint32_t slot) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = {.value = slot}};
}

static sxml_ir_expression_index add_atom_expression(unit_builder *builder, sxml_ir_atom atom) {
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM;
    expression.atom = atom;
    return add_expression(builder, &expression);
}

static sxml_ir_expression_index add_integer_expression(unit_builder *builder,
                                                       int64_t integer) {
    return add_atom_expression(
        builder,
        add_constant(builder, sxml_sequence_from_item(sxml_atomic_item_from_integer(integer))));
}

/* "()". */
static sxml_ir_expression_index add_empty_expression(unit_builder *builder) {
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE;
    return add_expression(builder, &expression);
}

/* A RAISE of an err: error, from line 3, column 7. */
typedef struct raise_description {
    sxml_status error_status;
    const char *local_name;
} raise_description;

static sxml_ir_expression_index add_raise(unit_builder *builder,
                                          const raise_description *raise) {
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = SXML_IR_EXPRESSION_KIND_RAISE;
    expression.source_span = (sxml_ir_source_span){
        .byte_offset = 20, .byte_count = 5, .line_number = 3, .column_number = 7};
    expression.raise = (sxml_ir_raise){
        .error_status = raise->error_status,
        .error_code = {.namespace_uri = SXML_BYTE_VIEW_INITIALIZER(SXML_ERROR_NAMESPACE_URI),
                       .local_name = text_view(raise->local_name),
                       .namespace_prefix = SXML_BYTE_VIEW_INITIALIZER("err")},
        .description = SXML_BYTE_VIEW_INITIALIZER("raised by the test")};
    return add_expression(builder, &expression);
}

static sxml_ir_expression_index add_dynamic_error(unit_builder *builder,
                                                  const char *local_name) {
    return add_raise(builder, &(raise_description){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                                   .local_name = local_name});
}

/* A static call of row with atoms as arguments, from line 5, column 2. */
typedef struct call_description {
    const sxml_native_function *row;
    const sxml_ir_atom *atoms;
    size_t atom_count;
} call_description;

static sxml_ir_expression_index add_static_call(unit_builder *builder,
                                                const call_description *call) {
    sxml_ir_atom_list arguments = {.first_atom_index = 0, .atom_count = 0};
    if (builder->status == SXML_STATUS_OK && call->atom_count > 0) {
        builder->status = sxml_ir_unit_add_atoms(
            builder->unit, (sxml_ir_atom_view){.atoms = call->atoms, .atom_count = call->atom_count},
            &arguments);
    }
    if (builder->status == SXML_STATUS_OK && call->row == NULL) {
        builder->status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL;
    expression.source_span = (sxml_ir_source_span){
        .byte_offset = 40, .byte_count = 9, .line_number = 5, .column_number = 2};
    expression.static_call = (sxml_ir_static_call){
        .target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                   .native_function = call->row},
        .arguments = arguments};
    return add_expression(builder, &expression);
}

static sxml_ir_expression_index add_let(unit_builder *builder, const sxml_ir_let *let) {
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = SXML_IR_EXPRESSION_KIND_LET;
    expression.let = *let;
    return add_expression(builder, &expression);
}

/* An xs:QName constant Q{namespace_uri}local_name with prefix. */
typedef struct qname_description {
    const char *namespace_uri;
    const char *namespace_prefix;
    const char *local_name;
} qname_description;

static sxml_ir_atom add_qname_constant(unit_builder *builder,
                                       const qname_description *qname) {
    sxml_name_pool *name_pool = builder->test_fixture->name_pool;
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_name_identifier expanded_name = {0};
    sxml_namespace_prefix_identifier namespace_prefix = {0};
    sxml_item item = {.representation = 0};
    sxml_status status = builder->status;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(name_pool, text_view(qname->namespace_uri),
                                                     &namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = text_view(qname->local_name)},
            &expanded_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            name_pool, text_view(qname->namespace_prefix), &namespace_prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            builder->test_fixture->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                          .datum_details = {.qname = {.expanded_name = expanded_name,
                                                      .namespace_prefix = namespace_prefix}}}},
            &item);
    }
    builder->status = status;
    return add_constant(builder, status == SXML_STATUS_OK ? sxml_sequence_from_item(item)
                                                          : sxml_sequence_empty());
}

static sxml_ir_atom add_string_constant(unit_builder *builder, const char *text) {
    sxml_item item = {.representation = 0};
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_atomic_item_make(
            builder->test_fixture->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = text_view(text),
                                                       .codepoint_count = 0,
                                                       .is_ascii = false}}}},
            &item);
    }
    return add_constant(builder, builder->status == SXML_STATUS_OK
                                     ? sxml_sequence_from_item(item)
                                     : sxml_sequence_empty());
}

/* fn:error(Q{EXAMPLE_NAMESPACE_URI}local_name, description, value). */
typedef struct error_call_description {
    const char *local_name;
    const char *description;
    int64_t value;
} error_call_description;

static sxml_ir_expression_index add_error_call(unit_builder *builder,
                                               const error_call_description *error_call) {
    const sxml_ir_atom atoms[3] = {
        add_qname_constant(builder, &(qname_description){.namespace_uri = EXAMPLE_NAMESPACE_URI,
                                                         .namespace_prefix = "ex",
                                                         .local_name = error_call->local_name}),
        add_string_constant(builder, error_call->description),
        add_constant(builder,
                     sxml_sequence_from_item(sxml_atomic_item_from_integer(error_call->value))),
    };
    return add_static_call(builder,
                           &(call_description){.row = find_error_row(), .atoms = atoms,
                                               .atom_count = 3});
}

/* An IR function of no parameters with body, and the expression that
   makes its function item. */
static sxml_ir_expression_index add_function_definition(unit_builder *builder,
                                                        sxml_ir_expression_index body) {
    sxml_type_identifier signature = {0};
    if (builder->status == SXML_STATUS_OK) {
        sxml_type_description description;
        memset(&description, 0, sizeof(description));
        description.variety = SXML_TYPE_VARIETY_FUNCTION;
        description.variety_details.function_type = (sxml_function_type){
            .is_any_function = false,
            .parameter_types = {.sequence_types = NULL, .sequence_type_count = 0},
            .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
        builder->status = sxml_type_registry_register_item_type(
            builder->test_fixture->registry, &description, &signature);
    }
    sxml_ir_function_index function = {0};
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_ir_unit_add_function(
            builder->unit,
            &(sxml_ir_function){.signature = signature,
                                .parameter_count = 0,
                                .defining_function = {.value = 1},
                                .slot_count = 0,
                                .body = body},
            &function);
    }
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION;
    expression.function_definition = (sxml_ir_function_definition){.function = function};
    return add_expression(builder, &expression);
}

/* row(function() { body }), the function item in slot 4 of the main
   function. */
typedef struct action_call_description {
    const sxml_native_function *row;
    sxml_ir_expression_index action_body;
} action_call_description;

static sxml_ir_expression_index add_action_call(unit_builder *builder,
                                                const action_call_description *action_call) {
    const sxml_ir_expression_index definition =
        add_function_definition(builder, action_call->action_body);
    const sxml_ir_atom action = slot_atom(4);
    const sxml_ir_expression_index call = add_static_call(
        builder, &(call_description){.row = action_call->row, .atoms = &action, .atom_count = 1});
    return add_let(builder, &(sxml_ir_let){.variable = {.value = 4},
                                           .bound_expression = definition,
                                           .body = call});
}

/* Name tests. */

static sxml_name_test any_name_test(void) {
    return (sxml_name_test){.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME};
}

static sxml_name_test expanded_name_test(unit_builder *builder,
                                         const qname_description *qname) {
    sxml_name_pool *name_pool = builder->test_fixture->name_pool;
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_name_identifier expanded_name = {0};
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_name_pool_intern_namespace_uri(
            name_pool, text_view(qname->namespace_uri), &namespace_uri);
    }
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = text_view(qname->local_name)},
            &expanded_name);
    }
    return (sxml_name_test){.name_test_kind = SXML_NAME_TEST_KIND_EXPANDED_NAME,
                            .expanded_name = expanded_name};
}

static sxml_name_test error_name_test(unit_builder *builder, const char *local_name) {
    return expanded_name_test(builder, &(qname_description){
                                           .namespace_uri = SXML_ERROR_NAMESPACE_URI,
                                           .namespace_prefix = "err",
                                           .local_name = local_name});
}

static sxml_name_test namespace_wildcard_test(unit_builder *builder,
                                              const char *namespace_uri) {
    sxml_namespace_uri_identifier identifier = {.index = 0};
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_name_pool_intern_namespace_uri(
            builder->test_fixture->name_pool, text_view(namespace_uri), &identifier);
    }
    return (sxml_name_test){.name_test_kind = SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD,
                            .namespace_uri = identifier};
}

static sxml_name_test local_name_wildcard_test(const char *local_name) {
    return (sxml_name_test){.name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD,
                            .local_name = text_view(local_name)};
}

/* One catch clause: its tests, the slot of its error details and its
   handler. */
typedef struct clause_description {
    sxml_name_test error_tests[CLAUSE_TEST_LIMIT];
    uint32_t error_test_count;
    uint32_t error_details_slot;
    sxml_ir_expression_index handler;
} clause_description;

typedef struct try_description {
    sxml_ir_expression_index body;
    clause_description clauses[TRY_CLAUSE_LIMIT];
    uint32_t clause_count;
    sxml_ir_expression_index finally_body;
} try_description;

static sxml_ir_expression_index add_try(unit_builder *builder,
                                        const try_description *description) {
    sxml_ir_catch_clause clauses[TRY_CLAUSE_LIMIT];
    if (description->clause_count > TRY_CLAUSE_LIMIT) {
        builder->status = SXML_STATUS_CAPACITY_EXCEEDED;
        return (sxml_ir_expression_index){0};
    }
    for (uint32_t clause_index = 0; clause_index < description->clause_count;
         clause_index += 1) {
        const clause_description *clause = &description->clauses[clause_index];
        sxml_ir_name_test_list tests = {0};
        if (builder->status == SXML_STATUS_OK) {
            builder->status = sxml_ir_unit_add_name_tests(
                builder->unit,
                (sxml_ir_name_test_view){.name_tests = clause->error_tests,
                                         .name_test_count = clause->error_test_count},
                &tests);
        }
        clauses[clause_index] = (sxml_ir_catch_clause){
            .error_tests = tests,
            .error_details = {.value = clause->error_details_slot},
            .handler = clause->handler};
    }
    sxml_ir_catch_clause_list clause_list = {0};
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_ir_unit_add_catch_clauses(
            builder->unit,
            (sxml_ir_catch_clause_view){.catch_clauses = clauses,
                                        .catch_clause_count = description->clause_count},
            &clause_list);
    }
    sxml_ir_expression expression;
    memset(&expression, 0, sizeof(expression));
    expression.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH;
    expression.source_span = (sxml_ir_source_span){
        .byte_offset = 0, .byte_count = 60, .line_number = 1, .column_number = 1};
    expression.try_catch = (sxml_ir_try_catch){
        .body = description->body,
        .catch_clauses = clause_list,
        .finally_body = description->finally_body};
    return add_expression(builder, &expression);
}

/* try { body } catch tests { $slot 1 }: the handler returns the error
   details. */
static sxml_ir_expression_index add_try_returning_details(unit_builder *builder,
                                                          sxml_ir_expression_index body,
                                                          sxml_name_test test) {
    const sxml_ir_expression_index handler = add_atom_expression(builder, slot_atom(1));
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {test},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1,
                                .finally_body = {0}});
}

static sxml_status start_unit(const fixture *test_fixture, unit_builder *builder) {
    *builder = (unit_builder){.test_fixture = test_fixture,
                              .unit = NULL,
                              .status = SXML_STATUS_OK};
    builder->status = sxml_ir_unit_create(
        fixture_allocator(test_fixture),
        &(sxml_ir_unit_configuration){.allocator = fixture_allocator(test_fixture),
                                      .item_heap = test_fixture->item_heap,
                                      .source_name = text_view(UNIT_SOURCE_NAME),
                                      .maximum_expression_count = 0},
        &builder->unit);
    sxml_ir_function_index main_function = {0};
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_ir_unit_add_function(
            builder->unit, &(sxml_ir_function){.slot_count = MAIN_SLOT_COUNT}, &main_function);
    }
    return builder->status;
}

/* Sets the main function's body and seals; on failure releases the
   unit. */
static sxml_status finish_unit(unit_builder *builder, sxml_ir_expression_index body) {
    if (builder->status == SXML_STATUS_OK) {
        builder->status = sxml_ir_unit_replace_function(
            builder->unit,
            &(sxml_ir_function_replacement){
                .index = {.value = 1},
                .function = {.slot_count = MAIN_SLOT_COUNT, .body = body}});
    }
    if (builder->status != SXML_STATUS_OK) {
        sxml_ir_unit_release(builder->unit);
        builder->unit = NULL;
        return builder->status;
    }
    sxml_ir_unit_seal(builder->unit);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Running */

/* Builds the main function's body of a scenario. */
typedef sxml_ir_expression_index (*scenario_builder)(unit_builder *builder);

typedef struct scenario {
    scenario_builder build;
    /* True to evaluate without an error record (D109). */
    bool has_no_error_record;
    /* 0: no failure. */
    uint64_t failing_allocation_number;
    /* 0: no bound. */
    uint64_t maximum_evaluation_step_count;
} scenario;

/* A finished evaluation; the fixture and unit stay alive for checks. */
typedef struct scenario_run {
    fixture test_fixture;
    sxml_ir_unit *unit;
    sxml_status status;
    sxml_sequence value;
    sxml_error_record error_record;
} scenario_run;

/* Builds and evaluates the scenario; returns the failures of building. */
static sxml_status run_scenario(const scenario *test_scenario, scenario_run *run) {
    *run = (scenario_run){.unit = NULL,
                          .status = SXML_STATUS_OK,
                          .value = sxml_sequence_empty(),
                          .error_record = {.value = NULL}};
    sxml_status status =
        fixture_create(test_scenario->failing_allocation_number, &run->test_fixture);
    unit_builder builder = {.unit = NULL};
    if (status == SXML_STATUS_OK) {
        status = start_unit(&run->test_fixture, &builder);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_ir_expression_index body = test_scenario->build(&builder);
        status = finish_unit(&builder, body);
        run->unit = builder.unit;
    } else if (builder.unit != NULL) {
        sxml_ir_unit_release(builder.unit);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    run->test_fixture.dynamic_context->maximum_evaluation_step_count =
        test_scenario->maximum_evaluation_step_count;
    run->status = sxml_evaluator_evaluate(
        run->test_fixture.dynamic_context,
        &(sxml_evaluation_request){
            .unit = run->unit,
            .has_context_value = false,
            .context_value = sxml_sequence_empty(),
            .global_values = {.slots = NULL, .slot_count = 0},
            .static_base_uri = {.bytes = NULL, .byte_count = 0},
            .error_record = test_scenario->has_no_error_record ? NULL : &run->error_record},
        &run->value);
    return SXML_STATUS_OK;
}

/* Releases the run; true when every block and cell was released and the
   context no longer points at an error record of a try task. */
static bool finish_run(scenario_run *run) {
    const bool record_cleared = run->test_fixture.dynamic_context == NULL ||
                                run->test_fixture.dynamic_context->error_record == NULL;
    sxml_sequence_release(run->value);
    run->value = sxml_sequence_empty();
    sxml_ir_unit_release(run->unit);
    run->unit = NULL;
    return fixture_destroy(&run->test_fixture) && record_cleared;
}

/* ------------------------------------------------------------ Checks */

static bool is_integer(sxml_sequence value, int64_t expected) {
    uint64_t item_count = 0;
    if (sxml_sequence_count(value, NULL, &item_count) != SXML_STATUS_OK || item_count != 1) {
        return false;
    }
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    return sxml_sequence_get_item(value, (sxml_item_request){.item_index = 0}, &item) ==
               SXML_STATUS_OK &&
           sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_INTEGER &&
           datum.datum_details.integer == expected;
}

static bool is_string(sxml_sequence value, const char *expected) {
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    return sxml_sequence_get_item(value, (sxml_item_request){.item_index = 0}, &item) ==
               SXML_STATUS_OK &&
           sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_STRING &&
           sxml_byte_view_equals(datum.datum_details.string.utf8, text_view(expected));
}

/* True when value is the xs:QName {namespace_uri}local_name with the
   prefix. */
static bool is_qname(const fixture *test_fixture, sxml_sequence value,
                     const qname_description *expected) {
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    sxml_name_parts parts;
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view namespace_prefix = {.bytes = NULL, .byte_count = 0};
    return sxml_sequence_get_item(value, (sxml_item_request){.item_index = 0}, &item) ==
               SXML_STATUS_OK &&
           sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_QNAME &&
           sxml_name_pool_get_name_parts(test_fixture->name_pool,
                                         datum.datum_details.qname.expanded_name,
                                         &parts) == SXML_STATUS_OK &&
           sxml_name_pool_get_namespace_uri(test_fixture->name_pool, parts.namespace_uri,
                                            &namespace_uri) == SXML_STATUS_OK &&
           sxml_name_pool_get_namespace_prefix(test_fixture->name_pool,
                                               datum.datum_details.qname.namespace_prefix,
                                               &namespace_prefix) == SXML_STATUS_OK &&
           sxml_byte_view_equals(parts.local_name, text_view(expected->local_name)) &&
           sxml_byte_view_equals(namespace_uri, text_view(expected->namespace_uri)) &&
           sxml_byte_view_equals(namespace_prefix, text_view(expected->namespace_prefix));
}

/* The value of key in the map run->value, borrowed; the empty sequence
   when the key is absent or the value is not one map. */
static sxml_sequence map_entry(const scenario_run *run, const char *key) {
    sxml_item key_item = {.representation = 0};
    if (sxml_atomic_item_make(
            run->test_fixture.item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = text_view(key),
                                                       .codepoint_count = 0,
                                                       .is_ascii = false}}}},
            &key_item) != SXML_STATUS_OK) {
        return sxml_sequence_empty();
    }
    sxml_map_lookup lookup = {.is_found = false};
    const sxml_status status = sxml_map_find(run->value.content, key_item, &lookup);
    sxml_item_release(key_item);
    if (status != SXML_STATUS_OK || !lookup.is_found) {
        return sxml_sequence_empty();
    }
    return lookup.entry.value;
}

static bool map_has_size(const scenario_run *run, size_t expected) {
    size_t map_entry_count = 0;
    return sxml_map_get_size(run->value.content, &map_entry_count) == SXML_STATUS_OK &&
           map_entry_count == expected;
}

static bool is_empty(sxml_sequence value) {
    uint64_t item_count = 1;
    return sxml_sequence_count(value, NULL, &item_count) == SXML_STATUS_OK && item_count == 0;
}

/* True when the run failed with status and the err: code local_name. */
typedef struct expected_error {
    sxml_status status;
    const char *namespace_uri;
    const char *local_name;
} expected_error;

static bool failed_with(const scenario_run *run, const expected_error *expected) {
    return run->status == expected->status &&
           sxml_byte_view_equals(run->error_record.code.local_name,
                                 text_view(expected->local_name)) &&
           sxml_byte_view_equals(run->error_record.code.namespace_uri,
                                 text_view(expected->namespace_uri));
}

static bool failed_with_error_code(const scenario_run *run, const char *local_name) {
    return failed_with(run, &(expected_error){.status = SXML_STATUS_DYNAMIC_ERROR,
                                              .namespace_uri = SXML_ERROR_NAMESPACE_URI,
                                              .local_name = local_name});
}

/* Runs a scenario that must succeed with the integer expected. */
static bool evaluates_to_integer(scenario_builder build, int64_t expected) {
    scenario_run run;
    const bool has_run =
        run_scenario(&(scenario){.build = build}, &run) == SXML_STATUS_OK;
    const bool matches = has_run && run.status == SXML_STATUS_OK && is_integer(run.value, expected);
    return finish_run(&run) && matches;
}

/* Runs a scenario that must fail with an err: dynamic error. */
static bool fails_with_error_code(scenario_builder build, const char *local_name) {
    scenario_run run;
    const bool has_run =
        run_scenario(&(scenario){.build = build}, &run) == SXML_STATUS_OK;
    const bool matches = has_run && failed_with_error_code(&run, local_name);
    return finish_run(&run) && matches;
}

/* ------------------------------------------------------------ Scenarios */

/* try { 42 } catch * { 0 } */
static sxml_ir_expression_index build_value_without_error(unit_builder *builder) {
    const sxml_ir_expression_index body = add_integer_expression(builder, 42);
    const sxml_ir_expression_index handler = add_integer_expression(builder, 0);
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {any_name_test()},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* try { raise err:FOAR0001 } catch * { $err:map } */
static sxml_ir_expression_index build_catch_any(unit_builder *builder) {
    return add_try_returning_details(builder, add_dynamic_error(builder, "FOAR0001"),
                                     any_name_test());
}

/* try { raise err:FOAR0002 }
   catch err:XPTY0004 { 1 } catch err:FOAR0001 | err:FOAR0002 { 2 }
   catch * { 3 } */
static sxml_ir_expression_index build_first_matching_clause(unit_builder *builder) {
    const sxml_ir_expression_index body = add_dynamic_error(builder, "FOAR0002");
    const sxml_ir_expression_index first = add_integer_expression(builder, 1);
    const sxml_ir_expression_index second = add_integer_expression(builder, 2);
    const sxml_ir_expression_index third = add_integer_expression(builder, 3);
    return add_try(builder,
                   &(try_description){
                       .body = body,
                       .clauses = {{.error_tests = {error_name_test(builder, "XPTY0004")},
                                    .error_test_count = 1,
                                    .error_details_slot = 1,
                                    .handler = first},
                                   {.error_tests = {error_name_test(builder, "FOAR0001"),
                                                    error_name_test(builder, "FOAR0002")},
                                    .error_test_count = 2,
                                    .error_details_slot = 1,
                                    .handler = second},
                                   {.error_tests = {any_name_test()},
                                    .error_test_count = 1,
                                    .error_details_slot = 1,
                                    .handler = third}},
                       .clause_count = 3});
}

/* try { raise err:FOAR0001 } catch Q{err}* { 1 } */
static sxml_ir_expression_index build_namespace_wildcard(unit_builder *builder) {
    const sxml_ir_expression_index body = add_dynamic_error(builder, "FOAR0001");
    const sxml_ir_expression_index handler = add_integer_expression(builder, 1);
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {namespace_wildcard_test(
                                                 builder, SXML_ERROR_NAMESPACE_URI)},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* try { raise err:FOAR0001 } catch *:FOAR0001 { 1 } */
static sxml_ir_expression_index build_local_name_wildcard(unit_builder *builder) {
    const sxml_ir_expression_index body = add_dynamic_error(builder, "FOAR0001");
    const sxml_ir_expression_index handler = add_integer_expression(builder, 1);
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {local_name_wildcard_test(
                                                 "FOAR0001")},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* try { raise err:FOAR0001 }
   catch Q{example}* | *:FOAR0002 | ex:FOAR0001 { 1 }: nothing matches. */
static sxml_ir_expression_index build_unmatched_error(unit_builder *builder) {
    const sxml_ir_expression_index body = add_dynamic_error(builder, "FOAR0001");
    const sxml_ir_expression_index handler = add_integer_expression(builder, 1);
    return add_try(builder,
                   &(try_description){
                       .body = body,
                       .clauses = {{.error_tests = {namespace_wildcard_test(
                                                        builder, EXAMPLE_NAMESPACE_URI),
                                                    local_name_wildcard_test("FOAR0002"),
                                                    expanded_name_test(
                                                        builder,
                                                        &(qname_description){
                                                            .namespace_uri =
                                                                EXAMPLE_NAMESPACE_URI,
                                                            .namespace_prefix = "ex",
                                                            .local_name = "FOAR0001"})},
                                    .error_test_count = 3,
                                    .error_details_slot = 1,
                                    .handler = handler}},
                       .clause_count = 1});
}

/* try { error(ex:E1, "computed", 1) } catch Q{example}* {
     let $2 := try { error(ex:E2, "other", 2) } catch * { () }
     return $1 }
   The inner error replaces the heap's raised error (D41) before the outer
   details are read. */
static sxml_ir_expression_index build_error_function(unit_builder *builder) {
    const sxml_ir_expression_index body = add_error_call(
        builder, &(error_call_description){.local_name = "E1", .description = "computed",
                                           .value = 1});
    const sxml_ir_expression_index inner_body = add_error_call(
        builder,
        &(error_call_description){.local_name = "E2", .description = "other", .value = 2});
    const sxml_ir_expression_index inner_handler = add_empty_expression(builder);
    const sxml_ir_expression_index inner_try =
        add_try(builder, &(try_description){
                             .body = inner_body,
                             .clauses = {{.error_tests = {any_name_test()},
                                          .error_test_count = 1,
                                          .error_details_slot = 3,
                                          .handler = inner_handler}},
                             .clause_count = 1});
    const sxml_ir_expression_index handler = add_let(
        builder, &(sxml_ir_let){.variable = {.value = 2},
                                .bound_expression = inner_try,
                                .body = add_atom_expression(builder, slot_atom(1))});
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {namespace_wildcard_test(
                                                 builder, EXAMPLE_NAMESPACE_URI)},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* try { raise type error err:XPTY0004 } catch err:XPTY0004 { 4 } */
static sxml_ir_expression_index build_type_error(unit_builder *builder) {
    const sxml_ir_expression_index body = add_raise(
        builder, &(raise_description){.error_status = SXML_STATUS_TYPE_ERROR,
                                      .local_name = "XPTY0004"});
    const sxml_ir_expression_index handler = add_integer_expression(builder, 4);
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {error_name_test(builder,
                                                                             "XPTY0004")},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* try { raise err:FOAR0001 } catch * { raise err:FORG0001 } */
static sxml_ir_expression_index build_error_in_catch_clause(unit_builder *builder) {
    const sxml_ir_expression_index body = add_dynamic_error(builder, "FOAR0001");
    const sxml_ir_expression_index handler = add_dynamic_error(builder, "FORG0001");
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {any_name_test()},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* try { try { raise err:FOAR0001 } catch err:FORG0001 { 0 } }
   catch err:FOAR0001 { 7 } */
static sxml_ir_expression_index build_nested_try(unit_builder *builder) {
    const sxml_ir_expression_index inner_body = add_dynamic_error(builder, "FOAR0001");
    const sxml_ir_expression_index inner_handler = add_integer_expression(builder, 0);
    const sxml_ir_expression_index inner_try =
        add_try(builder, &(try_description){
                             .body = inner_body,
                             .clauses = {{.error_tests = {error_name_test(builder, "FORG0001")},
                                          .error_test_count = 1,
                                          .error_details_slot = 2,
                                          .handler = inner_handler}},
                             .clause_count = 1});
    const sxml_ir_expression_index handler = add_integer_expression(builder, 7);
    return add_try(builder, &(try_description){
                                .body = inner_try,
                                .clauses = {{.error_tests = {error_name_test(builder,
                                                                             "FOAR0001")},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* Finally clauses. A try with one optional catch * clause and a finally
   clause, each part chosen by its builder. */
typedef struct finally_description {
    sxml_ir_expression_index body;
    /* 0 for no catch clause. */
    sxml_ir_expression_index handler;
    sxml_ir_expression_index finally_body;
} finally_description;

static sxml_ir_expression_index add_try_finally(unit_builder *builder,
                                                const finally_description *description) {
    return add_try(builder, &(try_description){
                                .body = description->body,
                                .clauses = {{.error_tests = {any_name_test()},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = description->handler}},
                                .clause_count = description->handler.value == 0 ? 0 : 1,
                                .finally_body = description->finally_body});
}

/* try { 42 } finally { () } */
static sxml_ir_expression_index build_finally_after_value(unit_builder *builder) {
    return add_try_finally(builder, &(finally_description){
                                        .body = add_integer_expression(builder, 42),
                                        .handler = {0},
                                        .finally_body = add_empty_expression(builder)});
}

/* try { 42 } catch * { 0 } finally { raise err:FORG0001 } */
static sxml_ir_expression_index build_finally_error_replaces_value(unit_builder *builder) {
    return add_try_finally(builder,
                           &(finally_description){
                               .body = add_integer_expression(builder, 42),
                               .handler = add_integer_expression(builder, 0),
                               .finally_body = add_dynamic_error(builder, "FORG0001")});
}

/* try { 42 } finally { 1 } */
static sxml_ir_expression_index build_finally_with_value(unit_builder *builder) {
    return add_try_finally(builder, &(finally_description){
                                        .body = add_integer_expression(builder, 42),
                                        .handler = {0},
                                        .finally_body = add_integer_expression(builder, 1)});
}

/* try { raise err:FOAR0001 } catch * { 5 } finally { () } */
static sxml_ir_expression_index build_finally_after_catch(unit_builder *builder) {
    return add_try_finally(builder, &(finally_description){
                                        .body = add_dynamic_error(builder, "FOAR0001"),
                                        .handler = add_integer_expression(builder, 5),
                                        .finally_body = add_empty_expression(builder)});
}

/* try { raise err:FOAR0001 } catch * { 5 } finally { raise err:FORG0001 } */
static sxml_ir_expression_index build_finally_error_after_catch(unit_builder *builder) {
    return add_try_finally(builder,
                           &(finally_description){
                               .body = add_dynamic_error(builder, "FOAR0001"),
                               .handler = add_integer_expression(builder, 5),
                               .finally_body = add_dynamic_error(builder, "FORG0001")});
}

/* try { error(ex:E1, "computed", 1) } catch err:FORG0001 { 0 }
   finally { try { error(ex:E2, "other", 2) } catch * { () } }: the first
   error is raised again after the finally clause, although the finally
   clause raised (and caught) another one on the same heap. */
static sxml_ir_expression_index build_unmatched_error_after_finally(unit_builder *builder) {
    const sxml_ir_expression_index body = add_error_call(
        builder, &(error_call_description){.local_name = "E1", .description = "computed",
                                           .value = 1});
    const sxml_ir_expression_index handler = add_integer_expression(builder, 0);
    const sxml_ir_expression_index inner_body = add_error_call(
        builder,
        &(error_call_description){.local_name = "E2", .description = "other", .value = 2});
    const sxml_ir_expression_index finally_body =
        add_try(builder, &(try_description){
                             .body = inner_body,
                             .clauses = {{.error_tests = {any_name_test()},
                                          .error_test_count = 1,
                                          .error_details_slot = 2,
                                          .handler = add_empty_expression(builder)}},
                             .clause_count = 1});
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {error_name_test(builder,
                                                                             "FORG0001")},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1,
                                .finally_body = finally_body});
}

/* try { raise err:FOAR0001 } finally { raise err:FORG0001 } */
static sxml_ir_expression_index build_finally_error_replaces_error(unit_builder *builder) {
    return add_try_finally(builder,
                           &(finally_description){
                               .body = add_dynamic_error(builder, "FOAR0001"),
                               .handler = {0},
                               .finally_body = add_dynamic_error(builder, "FORG0001")});
}

/* try { raise err:FOAR0001 } catch * { raise err:FOCA0002 } finally { () } */
static sxml_ir_expression_index build_catch_error_after_finally(unit_builder *builder) {
    return add_try_finally(builder, &(finally_description){
                                        .body = add_dynamic_error(builder, "FOAR0001"),
                                        .handler = add_dynamic_error(builder, "FOCA0002"),
                                        .finally_body = add_empty_expression(builder)});
}

/* try { raise err:FOAR0001 } catch * { raise err:FOCA0002 }
   finally { raise err:FORG0001 } */
static sxml_ir_expression_index build_finally_error_replaces_catch_error(
    unit_builder *builder) {
    return add_try_finally(builder,
                           &(finally_description){
                               .body = add_dynamic_error(builder, "FOAR0001"),
                               .handler = add_dynamic_error(builder, "FOCA0002"),
                               .finally_body = add_dynamic_error(builder, "FORG0001")});
}

/* try { call-action(function() { raise err:FOAR0001 }) }
   catch err:FOAR0001 { 1 } */
static sxml_ir_expression_index build_error_in_called_function(unit_builder *builder) {
    const sxml_ir_expression_index body = add_action_call(
        builder, &(action_call_description){.row = &call_action_row,
                                            .action_body = add_dynamic_error(builder,
                                                                             "FOAR0001")});
    const sxml_ir_expression_index handler = add_integer_expression(builder, 1);
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {error_name_test(builder,
                                                                             "FOAR0001")},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* try { fail-after-action(function() { 7 }) } catch Q{example}* { $1 } */
static sxml_ir_expression_index build_error_in_native_step(unit_builder *builder) {
    const sxml_ir_expression_index body = add_action_call(
        builder, &(action_call_description){.row = &fail_after_action_row,
                                            .action_body = add_integer_expression(builder, 7)});
    return add_try_returning_details(builder, body,
                                     namespace_wildcard_test(builder, EXAMPLE_NAMESPACE_URI));
}

/* try { call-action(function() { 7 }) } catch * { 0 }, to be stopped by
   a step limit. */
static sxml_ir_expression_index build_long_body(unit_builder *builder) {
    const sxml_ir_expression_index body = add_action_call(
        builder, &(action_call_description){.row = &call_action_row,
                                            .action_body = add_integer_expression(builder, 7)});
    const sxml_ir_expression_index handler = add_integer_expression(builder, 0);
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {any_name_test()},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* try { raise err:FOAR0001 } catch * { try { call-action(function() { 7 })
   } catch * { 0 } }: a step limit stops the handler after the caught
   error. */
static sxml_ir_expression_index build_limit_after_caught_error(unit_builder *builder) {
    const sxml_ir_expression_index inner_body = add_action_call(
        builder, &(action_call_description){.row = &call_action_row,
                                            .action_body = add_integer_expression(builder, 7)});
    const sxml_ir_expression_index inner_try =
        add_try(builder, &(try_description){
                             .body = inner_body,
                             .clauses = {{.error_tests = {any_name_test()},
                                          .error_test_count = 1,
                                          .error_details_slot = 3,
                                          .handler = add_integer_expression(builder, 0)}},
                             .clause_count = 1});
    return add_try(builder, &(try_description){
                                .body = add_dynamic_error(builder, "FOAR0001"),
                                .clauses = {{.error_tests = {any_name_test()},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = inner_try}},
                                .clause_count = 1});
}

/* try { lazy-failure() } catch err:FOAR0002 { 1 } */
static sxml_ir_expression_index build_lazy_error(unit_builder *builder) {
    const sxml_ir_expression_index body = add_static_call(
        builder, &(call_description){.row = &lazy_failure_row, .atoms = NULL, .atom_count = 0});
    const sxml_ir_expression_index handler = add_integer_expression(builder, 1);
    return add_try(builder, &(try_description){
                                .body = body,
                                .clauses = {{.error_tests = {error_name_test(builder,
                                                                             "FOAR0002")},
                                             .error_test_count = 1,
                                             .error_details_slot = 1,
                                             .handler = handler}},
                                .clause_count = 1});
}

/* try { error(ex:E1, "computed", 1) } catch * { 5 } finally { () }: the
   scenario of the allocation-failure loop. */
static sxml_ir_expression_index build_everything(unit_builder *builder) {
    const sxml_ir_expression_index body = add_error_call(
        builder, &(error_call_description){.local_name = "E1", .description = "computed",
                                           .value = 1});
    return add_try_finally(builder, &(finally_description){
                                        .body = body,
                                        .handler = add_integer_expression(builder, 5),
                                        .finally_body = add_empty_expression(builder)});
}

/* ---------------------------------------------------------------- Tests */

static void test_value_without_error(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_value_without_error, 42));
}

static const qname_description foar0001_code = {.namespace_uri = SXML_ERROR_NAMESPACE_URI,
                                                .namespace_prefix = "err",
                                                .local_name = "FOAR0001"};

/* $err:code, description, module, line-number and column-number; no
   value entry (XQuery 4.0, "Try/Catch Expressions", $err:map). */
static void check_raise_details(sxml_test_state *state, const scenario_run *run) {
    SXML_TEST_EXPECT(state, run->status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, map_has_size(run, 5));
    SXML_TEST_EXPECT(state,
                     is_qname(&run->test_fixture, map_entry(run, "code"), &foar0001_code));
    SXML_TEST_EXPECT(state, is_string(map_entry(run, "description"), "raised by the test"));
    SXML_TEST_EXPECT(state, is_string(map_entry(run, "module"), UNIT_SOURCE_NAME));
    SXML_TEST_EXPECT(state, is_integer(map_entry(run, "line-number"), 3));
    SXML_TEST_EXPECT(state, is_integer(map_entry(run, "column-number"), 7));
    SXML_TEST_EXPECT(state, is_empty(map_entry(run, "value")));
    SXML_TEST_EXPECT(state, is_empty(map_entry(run, "stack-trace")));
    SXML_TEST_EXPECT(state, is_empty(map_entry(run, "additional")));
}

static void test_catch_any_binds_the_error_details(sxml_test_state *state) {
    scenario_run run;
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_catch_any}, &run) ==
                                 SXML_STATUS_OK);
    check_raise_details(state, &run);
    SXML_TEST_EXPECT(state, finish_run(&run));
}

static void test_first_matching_clause_wins(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_first_matching_clause, 2));
}

static void test_wildcards_match(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_namespace_wildcard, 1));
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_local_name_wildcard, 1));
}

static void test_unmatched_error_propagates(sxml_test_state *state) {
    scenario_run run;
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_unmatched_error}, &run) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failed_with_error_code(&run, "FOAR0001"));
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.location.line_number == 3);
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.location.column_number == 7);
    SXML_TEST_EXPECT(state, finish_run(&run));
}

static const qname_description e1_code = {.namespace_uri = EXAMPLE_NAMESPACE_URI,
                                          .namespace_prefix = "ex",
                                          .local_name = "E1"};

static void test_error_function_value_survives_later_errors(sxml_test_state *state) {
    scenario_run run;
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_error_function}, &run) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, is_qname(&run.test_fixture, map_entry(&run, "code"), &e1_code));
    SXML_TEST_EXPECT(state, is_string(map_entry(&run, "description"), "computed"));
    SXML_TEST_EXPECT(state, is_integer(map_entry(&run, "value"), 1));
    /* fn:error has no location of its own: the static call's. */
    SXML_TEST_EXPECT(state, is_integer(map_entry(&run, "line-number"), 5));
    SXML_TEST_EXPECT(state, is_integer(map_entry(&run, "column-number"), 2));
    SXML_TEST_EXPECT(state, finish_run(&run));
}

static void test_type_errors_are_caught(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_type_error, 4));
}

static void test_error_in_catch_clause_propagates(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, fails_with_error_code(build_error_in_catch_clause, "FORG0001"));
}

static void test_nested_try_passes_unmatched_errors_out(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_nested_try, 7));
}

static void test_finally_clauses(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_finally_after_value, 42));
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_finally_after_catch, 5));
    SXML_TEST_EXPECT(state,
                     fails_with_error_code(build_finally_error_replaces_value, "FORG0001"));
    SXML_TEST_EXPECT(state, fails_with_error_code(build_finally_error_after_catch, "FORG0001"));
    SXML_TEST_EXPECT(state,
                     fails_with_error_code(build_finally_error_replaces_error, "FORG0001"));
    SXML_TEST_EXPECT(state, fails_with_error_code(build_catch_error_after_finally, "FOCA0002"));
    SXML_TEST_EXPECT(state,
                     fails_with_error_code(build_finally_error_replaces_catch_error, "FORG0001"));
}

static void test_finally_must_yield_the_empty_sequence(sxml_test_state *state) {
    scenario_run run;
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_finally_with_value},
                                          &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failed_with(&run, &(expected_error){
                                                  .status = SXML_STATUS_TYPE_ERROR,
                                                  .namespace_uri = SXML_ERROR_NAMESPACE_URI,
                                                  .local_name = "XQTY0153"}));
    SXML_TEST_EXPECT(state, finish_run(&run));
}

static void test_unmatched_error_is_raised_again_after_finally(sxml_test_state *state) {
    scenario_run run;
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build =
                                                          build_unmatched_error_after_finally},
                                          &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failed_with(&run, &(expected_error){
                                                  .status = SXML_STATUS_DYNAMIC_ERROR,
                                                  .namespace_uri = EXAMPLE_NAMESPACE_URI,
                                                  .local_name = "E1"}));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(run.error_record.diagnostic.description,
                                                  text_view("computed")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(run.error_record.code.namespace_prefix,
                                                  text_view("ex")));
    SXML_TEST_EXPECT(state, run.error_record.value != NULL &&
                                is_integer(*run.error_record.value, 1));
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.location.line_number == 5);
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.location.column_number == 2);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(run.error_record.diagnostic.location.source_name,
                                                  text_view(UNIT_SOURCE_NAME)));
    SXML_TEST_EXPECT(state, finish_run(&run));
}

static void test_errors_in_resumable_native_calls(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_error_in_called_function, 1));
    scenario_run run;
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_error_in_native_step},
                                          &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, is_qname(&run.test_fixture, map_entry(&run, "code"),
                                     &(qname_description){.namespace_uri =
                                                              EXAMPLE_NAMESPACE_URI,
                                                          .namespace_prefix = "ex",
                                                          .local_name = "LATE"}));
    SXML_TEST_EXPECT(state, is_string(map_entry(&run, "description"), "late failure"));
    SXML_TEST_EXPECT(state, is_integer(map_entry(&run, "value"), 7));
    SXML_TEST_EXPECT(state, finish_run(&run));
}

static void test_lazy_errors_are_caught(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_lazy_error, 1));
}

static void test_limits_are_never_caught(sxml_test_state *state) {
    /* Without a limit the body finishes. */
    SXML_TEST_EXPECT(state, evaluates_to_integer(build_long_body, 7));
    for (uint64_t step_count = 1; step_count < 6; step_count += 1) {
        scenario_run run;
        SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_long_body,
                                                          .maximum_evaluation_step_count =
                                                              step_count},
                                              &run) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, run.status == SXML_STATUS_LIMIT_EXCEEDED);
        SXML_TEST_EXPECT(state, finish_run(&run));
    }
}

/* A caught error leaves the evaluation's error record (XQuery 4.0,
   "Try/Catch Expressions": the catch clause handles it), so a later
   failure that writes no record, a limit, does not carry its code,
   description or place (D310's stale description). */
static bool record_is_clear(const sxml_error_record *record) {
    return record->code.local_name.byte_count == 0 && record->value == NULL &&
           record->diagnostic.location.line_number == 0;
}

static void test_caught_errors_leave_the_record(sxml_test_state *state) {
    scenario_run run;
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_catch_any}, &run) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, record_is_clear(&run.error_record) &&
                                run.error_record.diagnostic.description.byte_count == 0);
    SXML_TEST_EXPECT(state, finish_run(&run));
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_finally_after_catch},
                                          &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, record_is_clear(&run.error_record));
    SXML_TEST_EXPECT(state, finish_run(&run));
    /* Every bound, from the first step until the evaluation finishes:
       one that stops it in the handler reports the limit alone. */
    bool has_finished = false;
    for (uint64_t step_count = 1; !has_finished && step_count < 200; step_count += 1) {
        SXML_TEST_REQUIRE(state,
                          run_scenario(&(scenario){.build = build_limit_after_caught_error,
                                                   .maximum_evaluation_step_count = step_count},
                                       &run) == SXML_STATUS_OK);
        has_finished = run.status == SXML_STATUS_OK;
        SXML_TEST_EXPECT(state, has_finished ? is_integer(run.value, 7)
                                             : run.status == SXML_STATUS_LIMIT_EXCEEDED);
        const sxml_byte_view description = run.error_record.diagnostic.description;
        SXML_TEST_EXPECT(state, record_is_clear(&run.error_record));
        /* The step limit's own description, never the caught error's. */
        SXML_TEST_EXPECT(state, has_finished ? description.byte_count == 0
                                             : description.byte_count > 16 &&
                                                   memcmp(description.bytes,
                                                          "the evaluation t", 16) == 0);
        SXML_TEST_EXPECT(state, finish_run(&run));
    }
    SXML_TEST_EXPECT(state, has_finished);
}

static void test_without_error_record(sxml_test_state *state) {
    scenario_run run;
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_catch_any,
                                                      .has_no_error_record = true},
                                          &run) == SXML_STATUS_OK);
    check_raise_details(state, &run);
    SXML_TEST_EXPECT(state, finish_run(&run));
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_unmatched_error,
                                                      .has_no_error_record = true},
                                          &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, finish_run(&run));
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build =
                                                          build_unmatched_error_after_finally,
                                                      .has_no_error_record = true},
                                          &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, finish_run(&run));
    SXML_TEST_REQUIRE(state, run_scenario(&(scenario){.build = build_error_in_native_step,
                                                      .has_no_error_record = true},
                                          &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, is_integer(map_entry(&run, "value"), 7));
    SXML_TEST_EXPECT(state, finish_run(&run));
}

/* Every allocation failure ends the evaluation with OUT_OF_MEMORY, never
   in a catch clause, and releases everything. */
typedef struct failure_loop {
    scenario_builder build;
    bool has_no_error_record;
} failure_loop;

static bool survives_every_allocation_failure(const failure_loop *loop) {
    bool succeeded = false;
    bool is_clean = true;
    for (uint64_t failing_allocation_number = 1;
         !succeeded && failing_allocation_number < 2000; failing_allocation_number += 1) {
        scenario_run run;
        const sxml_status status =
            run_scenario(&(scenario){.build = loop->build,
                                     .has_no_error_record = loop->has_no_error_record,
                                     .failing_allocation_number = failing_allocation_number},
                         &run);
        if (status == SXML_STATUS_OK) {
            succeeded = run.status == SXML_STATUS_OK && is_integer(run.value, 5);
            is_clean = is_clean && (succeeded || run.status == SXML_STATUS_OUT_OF_MEMORY);
        } else {
            is_clean = is_clean && status == SXML_STATUS_OUT_OF_MEMORY;
        }
        is_clean = finish_run(&run) && is_clean;
    }
    return succeeded && is_clean;
}

static void test_try_survives_every_allocation_failure(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, survives_every_allocation_failure(
                                &(failure_loop){.build = build_everything,
                                                .has_no_error_record = false}));
    SXML_TEST_EXPECT(state, survives_every_allocation_failure(
                                &(failure_loop){.build = build_everything,
                                                .has_no_error_record = true}));
}

static const sxml_test_case evaluator_try_test_cases[] = {
    {.name = "value_without_error", .function = test_value_without_error},
    {.name = "catch_any_binds_the_error_details",
     .function = test_catch_any_binds_the_error_details},
    {.name = "first_matching_clause_wins", .function = test_first_matching_clause_wins},
    {.name = "wildcards_match", .function = test_wildcards_match},
    {.name = "unmatched_error_propagates", .function = test_unmatched_error_propagates},
    {.name = "error_function_value_survives_later_errors",
     .function = test_error_function_value_survives_later_errors},
    {.name = "type_errors_are_caught", .function = test_type_errors_are_caught},
    {.name = "error_in_catch_clause_propagates",
     .function = test_error_in_catch_clause_propagates},
    {.name = "nested_try_passes_unmatched_errors_out",
     .function = test_nested_try_passes_unmatched_errors_out},
    {.name = "finally_clauses", .function = test_finally_clauses},
    {.name = "finally_must_yield_the_empty_sequence",
     .function = test_finally_must_yield_the_empty_sequence},
    {.name = "unmatched_error_is_raised_again_after_finally",
     .function = test_unmatched_error_is_raised_again_after_finally},
    {.name = "errors_in_resumable_native_calls",
     .function = test_errors_in_resumable_native_calls},
    {.name = "lazy_errors_are_caught", .function = test_lazy_errors_are_caught},
    {.name = "limits_are_never_caught", .function = test_limits_are_never_caught},
    {.name = "caught_errors_leave_the_record",
     .function = test_caught_errors_leave_the_record},
    {.name = "without_error_record", .function = test_without_error_record},
    {.name = "try_survives_every_allocation_failure",
     .function = test_try_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "evaluator_try",
            .cases = evaluator_try_test_cases,
            .case_count = SXML_TEST_COUNT(evaluator_try_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
