/*
 * Tests for function items: making, describing and calling them, captured
 * values and body state released with the item, and maps and arrays called
 * as functions (include/sxml/xdm.h, src/xdm_function.c; D33, D38, D60).
 *
 * Imitates tests/memory_test.c and tests/xdm_test.c (fixture, D23 failure
 * loop). Behaviour follows XDM 4.0, section 8.1, XPath 4.0, section 4.5.3
 * (dynamic function calls) and F&O 4.0, map:get and array:get.
 */
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"

#include <string.h>

typedef struct function_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
    /* function(xs:integer) as xs:integer */
    sxml_type_identifier unary_signature;
    /* function(item()*, item()*) as item()* */
    sxml_type_identifier binary_signature;
} function_fixture;

static sxml_sequence_type exactly_one(sxml_builtin_type builtin_type) {
    return (sxml_sequence_type){.item_type =
                                    sxml_builtin_type_identifier(builtin_type),
                                .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
}

static sxml_sequence_type any_sequence(void) {
    return (sxml_sequence_type){
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
}

typedef struct signature_request {
    const sxml_sequence_type *parameter_types;
    size_t parameter_count;
    sxml_sequence_type result_type;
} signature_request;

static sxml_status register_signature(sxml_type_registry *registry,
                                      signature_request request,
                                      sxml_type_identifier *signature) {
    sxml_type_description description = {
        .variety = SXML_TYPE_VARIETY_FUNCTION,
        .type_name = {.index = 0},
        .base_type = {.index = 0},
        .primitive_type = {.index = 0},
        .is_builtin = false,
        .variety_details = {.list_item_type = {.index = 0}},
    };
    description.variety_details.function_type = (sxml_function_type){
        .is_any_function = false,
        .parameter_types = {.sequence_types = request.parameter_types,
                            .sequence_type_count = request.parameter_count},
        .result_type = request.result_type};
    return sxml_type_registry_register_item_type(registry, &description,
                                                 signature);
}

static sxml_status function_fixture_create_with(sxml_allocator allocator,
                                                function_fixture *fixture) {
    *fixture = (function_fixture){.name_pool = NULL,
                                  .registry = NULL,
                                  .item_heap = NULL,
                                  .unary_signature = {.index = 0},
                                  .binary_signature = {.index = 0}};
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
    const sxml_sequence_type integer_type = exactly_one(SXML_TYPE_XS_INTEGER);
    if (status == SXML_STATUS_OK) {
        status = register_signature(
            fixture->registry,
            (signature_request){.parameter_types = &integer_type,
                                .parameter_count = 1,
                                .result_type = integer_type},
            &fixture->unary_signature);
    }
    const sxml_sequence_type two_sequences[2] = {any_sequence(),
                                                 any_sequence()};
    if (status == SXML_STATUS_OK) {
        status = register_signature(
            fixture->registry,
            (signature_request){.parameter_types = two_sequences,
                                .parameter_count = 2,
                                .result_type = any_sequence()},
            &fixture->binary_signature);
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

static sxml_status function_fixture_create(function_fixture *fixture) {
    return function_fixture_create_with(sxml_default_allocator(), fixture);
}

static void function_fixture_destroy(function_fixture *fixture) {
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

static uint64_t live_cell_count(const function_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
}

static sxml_status make_string(sxml_item_heap *item_heap, sxml_byte_view text,
                               sxml_item *item) {
    sxml_string string;
    sxml_status status = sxml_string_from_utf8(text, &string);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = string}}},
        item);
}

/* The integer of a single-integer sequence; -1 for anything else. */
static int64_t sequence_integer(sxml_sequence sequence) {
    if (sxml_sequence_get_representation(sequence) !=
        SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        return -1;
    }
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(sequence.content, &datum) !=
            SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_INTEGER) {
        return -1;
    }
    return datum.datum_details.integer;
}

static bool error_code_is(const sxml_error_record *error_record,
                          sxml_byte_view local_name) {
    return error_record->code.local_name.byte_count == local_name.byte_count &&
           memcmp(error_record->code.local_name.bytes, local_name.bytes,
                  local_name.byte_count) == 0;
}

/* ------------------------------------------------------- A native body */

/* The state of the test body: how often it was retained and released. */
typedef struct adder_state {
    size_t retain_count;
    size_t release_count;
    size_t call_count;
} adder_state;

static void adder_retain(void *state) {
    ((adder_state *)state)->retain_count += 1;
}

static void adder_release(void *state) {
    ((adder_state *)state)->release_count += 1;
}

/* Adds the first captured value (an integer) to the argument. */
static sxml_status adder_call(void *state, const sxml_function_call *call,
                              sxml_sequence *result) {
    ((adder_state *)state)->call_count += 1;
    sxml_function_item_description description;
    sxml_status status =
        sxml_function_item_describe(call->function_item, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    int64_t addend = sequence_integer(description.captured_values.sequences[0]);
    int64_t argument = sequence_integer(call->arguments.sequences[0]);
    *result = sxml_sequence_from_item(
        sxml_atomic_item_from_integer(addend + argument));
    return SXML_STATUS_OK;
}

static const sxml_function_body_operations adder_operations = {
    .call = adder_call,
    .retain = adder_retain,
    .release = adder_release,
};

/* -------------------------------------------------------- Function items */

static void test_make_describe_and_call(sxml_test_state *state) {
    function_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      function_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item text = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string(fixture.item_heap,
                                         SXML_BYTE_VIEW_FROM_LITERAL("kept"),
                                         &text) == SXML_STATUS_OK);
    const sxml_sequence captured[2] = {
        sxml_sequence_from_item(sxml_atomic_item_from_integer(40)),
        sxml_sequence_from_item(text)};
    const sxml_item annotation_values[1] = {sxml_atomic_item_from_integer(1)};
    const sxml_function_annotation annotations[1] = {
        {.annotation_name = {.index = 7},
         .annotation_values = {.items = annotation_values, .item_count = 1}}};
    adder_state adder = {
        .retain_count = 0, .release_count = 0, .call_count = 0};
    sxml_item function = {.representation = 0};
    SXML_TEST_REQUIRE(
        state,
        sxml_function_item_make(
            fixture.item_heap,
            &(sxml_function_item_request){
                .function_name = {.index = 3},
                .signature = fixture.unary_signature,
                .function_annotations = {.function_annotations = annotations,
                                         .function_annotation_count = 1},
                .captured_values = {.sequences = captured, .sequence_count = 2},
                .body = {.state = &adder, .operations = &adder_operations}},
            &function) == SXML_STATUS_OK);
    /* The function item retains the string; the caller's reference can
       go, and the body's state was retained once. */
    sxml_item_release(text);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 2);
    SXML_TEST_EXPECT(state, adder.retain_count == 1);
    SXML_TEST_EXPECT(state,
                     sxml_item_get_kind(function) == SXML_ITEM_KIND_FUNCTION);
    SXML_TEST_EXPECT(state, sxml_item_is_function_item(function));
    SXML_TEST_EXPECT(state, sxml_item_get_type_annotation(function).index ==
                                fixture.unary_signature.index);
    sxml_function_item_description description;
    SXML_TEST_REQUIRE(state, sxml_function_item_describe(
                                 function, &description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.function_name.index == 3);
    SXML_TEST_EXPECT(state, description.signature.index ==
                                fixture.unary_signature.index);
    SXML_TEST_EXPECT(state, description.arity == 1);
    SXML_TEST_EXPECT(
        state,
        description.function_annotations.function_annotations == annotations &&
            description.function_annotations.function_annotation_count == 1);
    SXML_TEST_EXPECT(state, description.captured_values.sequence_count == 2);
    SXML_TEST_EXPECT(
        state,
        sequence_integer(description.captured_values.sequences[0]) == 40);
    SXML_TEST_EXPECT(state,
                     description.body.state == &adder &&
                         description.body.operations == &adder_operations);
    const sxml_sequence arguments[1] = {
        sxml_sequence_from_item(sxml_atomic_item_from_integer(2))};
    sxml_sequence result = sxml_sequence_empty();
    SXML_TEST_REQUIRE(
        state, sxml_function_item_call(
                   &(sxml_function_call){.function_item = function,
                                         .arguments = {.sequences = arguments,
                                                       .sequence_count = 1},
                                         .dynamic_context = NULL,
                                         .item_heap = fixture.item_heap,
                                         .error_record = NULL},
                   &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sequence_integer(result) == 42);
    SXML_TEST_EXPECT(state, adder.call_count == 1);
    sxml_sequence_release(result);
    /* Captures and body state go with the item. */
    sxml_item_retain(function);
    sxml_item_release(function);
    SXML_TEST_EXPECT(state, adder.release_count == 0);
    sxml_item_release(function);
    SXML_TEST_EXPECT(state, adder.release_count == 1);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(
        state,
        sxml_item_heap_read_counts(fixture.item_heap).live_byte_count == 0);
    function_fixture_destroy(&fixture);
}

/* Built-in body that needs no retain or release (static state). */
static sxml_status constant_call(void *state, const sxml_function_call *call,
                                 sxml_sequence *result) {
    (void)state;
    (void)call;
    *result = sxml_sequence_from_item(sxml_atomic_item_true());
    return SXML_STATUS_OK;
}

static const sxml_function_body_operations constant_operations = {
    .call = constant_call,
    .retain = NULL,
    .release = NULL,
};

static void test_requests_are_checked(sxml_test_state *state) {
    function_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      function_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_function_item_request request = {
        .function_name = {.index = 0},
        .signature = fixture.binary_signature,
        .function_annotations = {.function_annotations = NULL,
                                 .function_annotation_count = 0},
        .captured_values = {.sequences = NULL, .sequence_count = 0},
        .body = {.state = NULL, .operations = &constant_operations}};
    sxml_item function = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_function_item_make(fixture.item_heap, &request,
                                              &function) == SXML_STATUS_OK);
    sxml_function_item_description description;
    SXML_TEST_EXPECT(state, sxml_function_item_describe(
                                function, &description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.arity == 2);
    SXML_TEST_EXPECT(state, description.function_name.index == 0);
    sxml_item_release(function);
    /* function(*) has no arity, and xs:integer is no function type. */
    request.signature = sxml_builtin_type_identifier(SXML_TYPE_FUNCTION);
    SXML_TEST_EXPECT(state, sxml_function_item_make(fixture.item_heap, &request,
                                                    &function) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    request.signature = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER);
    SXML_TEST_EXPECT(state, sxml_function_item_make(fixture.item_heap, &request,
                                                    &function) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    request.signature = (sxml_type_identifier){.index = 60000};
    SXML_TEST_EXPECT(state, sxml_function_item_make(fixture.item_heap, &request,
                                                    &function) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    /* A body needs a call operation. */
    request.signature = fixture.unary_signature;
    request.body.operations = NULL;
    SXML_TEST_EXPECT(state, sxml_function_item_make(fixture.item_heap, &request,
                                                    &function) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    const sxml_function_body_operations no_call = {
        .call = NULL, .retain = NULL, .release = NULL};
    request.body.operations = &no_call;
    SXML_TEST_EXPECT(state, sxml_function_item_make(fixture.item_heap, &request,
                                                    &function) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    /* Only function items can be described or called. */
    const sxml_item integer = sxml_atomic_item_from_integer(1);
    SXML_TEST_EXPECT(state,
                     sxml_function_item_describe(integer, &description) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    sxml_sequence result = sxml_sequence_empty();
    SXML_TEST_EXPECT(
        state, sxml_function_item_call(
                   &(sxml_function_call){
                       .function_item = integer,
                       .arguments = {.sequences = NULL, .sequence_count = 0},
                       .dynamic_context = NULL,
                       .item_heap = fixture.item_heap,
                       .error_record = NULL},
                   &result) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    function_fixture_destroy(&fixture);
}

static void test_wrong_argument_count_is_a_type_error(sxml_test_state *state) {
    function_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      function_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item function = {.representation = 0};
    SXML_TEST_REQUIRE(
        state,
        sxml_function_item_make(
            fixture.item_heap,
            &(sxml_function_item_request){
                .function_name = {.index = 0},
                .signature = fixture.binary_signature,
                .function_annotations = {.function_annotations = NULL,
                                         .function_annotation_count = 0},
                .captured_values = {.sequences = NULL, .sequence_count = 0},
                .body = {.state = NULL, .operations = &constant_operations}},
            &function) == SXML_STATUS_OK);
    const sxml_sequence arguments[3] = {
        sxml_sequence_empty(), sxml_sequence_empty(), sxml_sequence_empty()};
    sxml_error_record error_record;
    sxml_sequence result = sxml_sequence_empty();
    SXML_TEST_EXPECT(
        state, sxml_function_item_call(
                   &(sxml_function_call){.function_item = function,
                                         .arguments = {.sequences = arguments,
                                                       .sequence_count = 3},
                                         .dynamic_context = NULL,
                                         .item_heap = fixture.item_heap,
                                         .error_record = &error_record},
                   &result) == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(
        state,
        error_code_is(&error_record, SXML_BYTE_VIEW_FROM_LITERAL("XPTY0004")));
    /* Without an error record the status is the same. */
    SXML_TEST_EXPECT(
        state, sxml_function_item_call(
                   &(sxml_function_call){.function_item = function,
                                         .arguments = {.sequences = arguments,
                                                       .sequence_count = 1},
                                         .dynamic_context = NULL,
                                         .item_heap = fixture.item_heap,
                                         .error_record = NULL},
                   &result) == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(
        state, sxml_function_item_call(
                   &(sxml_function_call){.function_item = function,
                                         .arguments = {.sequences = arguments,
                                                       .sequence_count = 2},
                                         .dynamic_context = NULL,
                                         .item_heap = fixture.item_heap,
                                         .error_record = NULL},
                   &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(result) ==
                                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM);
    sxml_item_release(function);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    function_fixture_destroy(&fixture);
}

/* ------------------------------------------ Maps and arrays as functions */

/* Calls collection with one argument; writes the error record. */
typedef struct collection_call {
    sxml_item collection;
    sxml_sequence argument;
    sxml_error_record *error_record;
} collection_call;

static sxml_status call_collection(const function_fixture *fixture,
                                   const collection_call *call,
                                   sxml_sequence *result) {
    return sxml_function_item_call(
        &(sxml_function_call){
            .function_item = call->collection,
            .arguments = {.sequences = &call->argument, .sequence_count = 1},
            .dynamic_context = NULL,
            .item_heap = fixture->item_heap,
            .error_record = call->error_record},
        result);
}

static sxml_sequence integer_argument(int64_t integer) {
    return sxml_sequence_from_item(sxml_atomic_item_from_integer(integer));
}

static void test_arrays_are_called_as_array_get(sxml_test_state *state) {
    function_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      function_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item text = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string(fixture.item_heap,
                                         SXML_BYTE_VIEW_FROM_LITERAL("two"),
                                         &text) == SXML_STATUS_OK);
    const sxml_sequence members[3] = {integer_argument(10),
                                      sxml_sequence_from_item(text),
                                      integer_argument(30)};
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_array_make(fixture.item_heap,
                                      (sxml_sequence_view){.sequences = members,
                                                           .sequence_count = 3},
                                      &array) == SXML_STATUS_OK);
    sxml_item_release(text);
    sxml_function_item_description description;
    SXML_TEST_REQUIRE(state, sxml_function_item_describe(array, &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.arity == 1);
    SXML_TEST_EXPECT(state, description.signature.index == SXML_TYPE_ARRAY);
    SXML_TEST_EXPECT(state, description.function_name.index == 0);
    SXML_TEST_EXPECT(state, description.captured_values.sequence_count == 0);
    SXML_TEST_EXPECT(state, description.body.operations != NULL &&
                                description.body.operations->call != NULL);
    /* Positions count from 1 (F&O 4.0, array:get). */
    sxml_sequence result = sxml_sequence_empty();
    SXML_TEST_REQUIRE(
        state,
        call_collection(&fixture,
                        &(collection_call){.collection = array,
                                           .argument = integer_argument(3),
                                           .error_record = NULL},
                        &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sequence_integer(result) == 30);
    sxml_sequence_release(result);
    /* A member in a cell is returned owned. */
    SXML_TEST_REQUIRE(
        state,
        call_collection(&fixture,
                        &(collection_call){.collection = array,
                                           .argument = integer_argument(2),
                                           .error_record = NULL},
                        &result) == SXML_STATUS_OK);
    sxml_item_release(array);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 1);
    sxml_sequence_release(result);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_REQUIRE(state,
                      sxml_array_make(fixture.item_heap,
                                      (sxml_sequence_view){.sequences = members,
                                                           .sequence_count = 1},
                                      &array) == SXML_STATUS_OK);
    /* Out of bounds: FOAY0001, also beyond int64_t. */
    sxml_error_record error_record;
    const int64_t outside[3] = {0, 2, -1};
    for (size_t outside_index = 0; outside_index < 3; outside_index += 1) {
        SXML_TEST_EXPECT(
            state,
            call_collection(&fixture,
                            &(collection_call){.collection = array,
                                               .argument = integer_argument(
                                                   outside[outside_index]),
                                               .error_record = &error_record},
                            &result) == SXML_STATUS_DYNAMIC_ERROR);
        SXML_TEST_EXPECT(
            state, error_code_is(&error_record,
                                 SXML_BYTE_VIEW_FROM_LITERAL("FOAY0001")));
    }
    sxml_item large = {.representation = 0};
    SXML_TEST_REQUIRE(
        state,
        sxml_atomic_item_make(
            fixture.item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                .datum = {.datum_kind = SXML_DATUM_KIND_DECIMAL,
                          .datum_details = {.decimal_text =
                                                SXML_BYTE_VIEW_FROM_LITERAL(
                                                    "100000000000000000000")}}},
            &large) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, call_collection(&fixture,
                               &(collection_call){
                                   .collection = array,
                                   .argument = sxml_sequence_from_item(large),
                                   .error_record = &error_record},
                               &result) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(
        state,
        error_code_is(&error_record, SXML_BYTE_VIEW_FROM_LITERAL("FOAY0001")));
    sxml_item_release(large);
    /* Not an integer: XPTY0004 (coercion is the caller's, but the body
       checks). */
    SXML_TEST_EXPECT(
        state, call_collection(
                   &fixture,
                   &(collection_call){.collection = array,
                                      .argument = sxml_sequence_from_item(
                                          sxml_atomic_item_from_double(1.0)),
                                      .error_record = &error_record},
                   &result) == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(
        state,
        error_code_is(&error_record, SXML_BYTE_VIEW_FROM_LITERAL("XPTY0004")));
    SXML_TEST_EXPECT(
        state,
        call_collection(&fixture,
                        &(collection_call){.collection = array,
                                           .argument = sxml_sequence_empty(),
                                           .error_record = &error_record},
                        &result) == SXML_STATUS_TYPE_ERROR);
    /* Arity 1. */
    SXML_TEST_EXPECT(
        state, sxml_function_item_call(
                   &(sxml_function_call){
                       .function_item = array,
                       .arguments = {.sequences = members, .sequence_count = 2},
                       .dynamic_context = NULL,
                       .item_heap = fixture.item_heap,
                       .error_record = NULL},
                   &result) == SXML_STATUS_TYPE_ERROR);
    sxml_item_release(array);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    function_fixture_destroy(&fixture);
}

static void test_maps_are_called_as_map_get(sxml_test_state *state) {
    function_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      function_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item key = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string(fixture.item_heap,
                                         SXML_BYTE_VIEW_FROM_LITERAL("a"),
                                         &key) == SXML_STATUS_OK);
    sxml_item value = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_string(fixture.item_heap,
                                         SXML_BYTE_VIEW_FROM_LITERAL("value"),
                                         &value) == SXML_STATUS_OK);
    const sxml_map_entry entries[2] = {
        {.key = key, .value = sxml_sequence_from_item(value)},
        {.key = sxml_atomic_item_from_integer(1),
         .value = integer_argument(11)}};
    sxml_item map = {.representation = 0};
    SXML_TEST_REQUIRE(
        state,
        sxml_map_make(
            fixture.item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries, .map_entry_count = 2},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                .error_record = NULL},
            &map) == SXML_STATUS_OK);
    sxml_item_release(value);
    sxml_function_item_description description;
    SXML_TEST_REQUIRE(state, sxml_function_item_describe(map, &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.arity == 1);
    SXML_TEST_EXPECT(state, description.signature.index ==
                                sxml_item_get_type_annotation(map).index);
    /* Keys compare by atomic-equal: 1.0e0 finds the key 1. */
    sxml_sequence result = sxml_sequence_empty();
    SXML_TEST_REQUIRE(
        state, call_collection(
                   &fixture,
                   &(collection_call){.collection = map,
                                      .argument = sxml_sequence_from_item(
                                          sxml_atomic_item_from_double(1.0)),
                                      .error_record = NULL},
                   &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sequence_integer(result) == 11);
    sxml_sequence_release(result);
    /* The value of a string key, owned by the caller. */
    SXML_TEST_REQUIRE(
        state, call_collection(
                   &fixture,
                   &(collection_call){.collection = map,
                                      .argument = sxml_sequence_from_item(key),
                                      .error_record = NULL},
                   &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(result) ==
                                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM);
    sxml_item_release(key);
    sxml_item_release(map);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 1);
    sxml_sequence_release(result);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_REQUIRE(
        state, sxml_map_make(
                   fixture.item_heap,
                   &(sxml_map_request){
                       .map_entries = {.map_entries = &entries[1],
                                       .map_entry_count = 1},
                       .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                       .error_record = NULL},
                   &map) == SXML_STATUS_OK);
    /* An absent key gives the empty sequence. */
    SXML_TEST_REQUIRE(
        state,
        call_collection(&fixture,
                        &(collection_call){.collection = map,
                                           .argument = integer_argument(2),
                                           .error_record = NULL},
                        &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(result) ==
                                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    /* The key must be one atomic item. */
    sxml_error_record error_record;
    SXML_TEST_EXPECT(
        state, call_collection(
                   &fixture,
                   &(collection_call){.collection = map,
                                      .argument = sxml_sequence_from_item(map),
                                      .error_record = &error_record},
                   &result) == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(
        state,
        error_code_is(&error_record, SXML_BYTE_VIEW_FROM_LITERAL("XPTY0004")));
    sxml_item_release(map);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    function_fixture_destroy(&fixture);
}

/* ------------------------------------------------------------ Failures */

/* Makes a function item capturing three strings and calls it; any failure
   is returned after everything made is released. */
static sxml_status
make_function_with_three_strings(const function_fixture *fixture,
                                 adder_state *adder) {
    sxml_item texts[3];
    size_t made_count = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && made_count < 3) {
        status = make_string(fixture->item_heap,
                             SXML_BYTE_VIEW_FROM_LITERAL("captured"),
                             &texts[made_count]);
        if (status == SXML_STATUS_OK) {
            made_count += 1;
        }
    }
    sxml_sequence captured[4] = {integer_argument(1), sxml_sequence_empty(),
                                 sxml_sequence_empty(), sxml_sequence_empty()};
    for (size_t text_index = 0; text_index < made_count; text_index += 1) {
        captured[text_index + 1] = sxml_sequence_from_item(texts[text_index]);
    }
    sxml_item function = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_item_make(
            fixture->item_heap,
            &(sxml_function_item_request){
                .function_name = {.index = 0},
                .signature = fixture->unary_signature,
                .function_annotations = {.function_annotations = NULL,
                                         .function_annotation_count = 0},
                .captured_values = {.sequences = captured, .sequence_count = 4},
                .body = {.state = adder, .operations = &adder_operations}},
            &function);
    }
    for (size_t text_index = 0; text_index < made_count; text_index += 1) {
        sxml_item_release(texts[text_index]);
    }
    if (status == SXML_STATUS_OK) {
        sxml_sequence result = sxml_sequence_empty();
        const sxml_sequence argument = integer_argument(1);
        status = sxml_function_item_call(
            &(sxml_function_call){
                .function_item = function,
                .arguments = {.sequences = &argument, .sequence_count = 1},
                .dynamic_context = NULL,
                .item_heap = fixture->item_heap,
                .error_record = NULL},
            &result);
        sxml_sequence_release(result);
        sxml_item_release(function);
    }
    return status;
}

/* D23: fail each heap allocation in turn; every failure leaks nothing and
   leaves the body's state balanced. */
static void
test_make_survives_every_allocation_failure(sxml_test_state *state) {
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
        function_fixture fixture;
        SXML_TEST_REQUIRE(
            state, function_fixture_create_with(
                       sxml_counting_allocator_as_allocator(counting_allocator),
                       &fixture) == SXML_STATUS_OK);
        adder_state adder = {
            .retain_count = 0, .release_count = 0, .call_count = 0};
        sxml_status status = make_function_with_three_strings(&fixture, &adder);
        SXML_TEST_EXPECT(state, adder.retain_count == adder.release_count);
        SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
        function_fixture_destroy(&fixture);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            SXML_TEST_EXPECT(state, adder.call_count == 1);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 100);
    }
    /* Three strings and the function cell. */
    SXML_TEST_EXPECT(state, failing_allocation_number == 5);
}

static const sxml_test_case xdm_function_test_cases[] = {
    {.name = "make_describe_and_call", .function = test_make_describe_and_call},
    {.name = "requests_are_checked", .function = test_requests_are_checked},
    {.name = "wrong_argument_count_is_a_type_error",
     .function = test_wrong_argument_count_is_a_type_error},
    {.name = "arrays_are_called_as_array_get",
     .function = test_arrays_are_called_as_array_get},
    {.name = "maps_are_called_as_map_get",
     .function = test_maps_are_called_as_map_get},
    {.name = "make_survives_every_allocation_failure",
     .function = test_make_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xdm_function",
            .cases = xdm_function_test_cases,
            .case_count = SXML_TEST_COUNT(xdm_function_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
