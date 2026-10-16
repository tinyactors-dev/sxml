/*
 * Tests for the evaluator (src/evaluator.c, src/evaluator_path.c,
 * src/evaluator_function.c, src/evaluator_internal.h; D46, D55, D71, D75).
 *
 * Golden file: tests/memory_test.c (fixture, lifecycle, allocation-failure
 * loops), with the function table and compilation of
 * tests/xpath_static_test.c. Expressions are compiled with
 * sxml_xpath_compile against a test function table of a few rows whose
 * bodies live here (the function library comes with packets 1.20 and 1.21;
 * fn:for-each and fn:fold-left are written here as resumable bodies, D75),
 * evaluated, and their values written as text: atomic items in canonical
 * form, separated by spaces. Errors are checked by code. Every case checks
 * that the item heap's cells and the allocator's blocks are all released.
 * Slice queries are named by their QT4 test case.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    alignas(16) uint8_t counting_storage[256];
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} fixture;

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

static bool fixture_released_everything(const fixture *test_fixture) {
    sxml_item_heap_clear_raised_error(test_fixture->item_heap);
    const bool is_clean =
        sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
                .live_allocation_count == 0 &&
        sxml_item_heap_read_counts(test_fixture->item_heap).live_cell_count == 0;
    if (!is_clean) {
        (void)fprintf(
            stderr, "leak: %llu blocks, %llu cells\n",
            (unsigned long long)sxml_counting_allocator_read_counts(
                test_fixture->counting_allocator)
                .live_allocation_count,
            (unsigned long long)sxml_item_heap_read_counts(test_fixture->item_heap)
                .live_cell_count);
    }
    return is_clean;
}

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* ------------------------------------------------- Native test bodies */

static const sxml_static_error test_error_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOER0000"),
    .description = SXML_BYTE_VIEW_INITIALIZER("fn:error() was called"),
};

static sxml_status call_true(const sxml_function_call *call,
                             sxml_sequence *result) {
    (void)call;
    *result = sxml_sequence_from_item(sxml_atomic_item_true());
    return SXML_STATUS_OK;
}

static sxml_status call_false(const sxml_function_call *call,
                              sxml_sequence *result) {
    (void)call;
    *result = sxml_sequence_from_item(sxml_atomic_item_false());
    return SXML_STATUS_OK;
}

static sxml_status call_error(const sxml_function_call *call,
                              sxml_sequence *result) {
    (void)result;
    return sxml_error_record_report(call->error_record, &test_error_error);
}

static sxml_status call_count(const sxml_function_call *call,
                              sxml_sequence *result) {
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(call->arguments.sequences[0],
                                             call->error_record, &item_count);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(
            sxml_atomic_item_from_integer((int64_t)item_count));
    }
    return status;
}

/* fn:root: the parent chain of an XNode or a JNode (F&O 4.0, fn:root;
   D70: a JNode's root is found through its parents). */
static sxml_status call_root(const sxml_function_call *call,
                             sxml_sequence *result) {
    const sxml_sequence argument = call->arguments.sequences[0];
    if (sxml_sequence_get_representation(argument) ==
        SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_item item = argument.content;
    if (sxml_item_get_kind(item) == SXML_ITEM_KIND_JSON_NODE) {
        for (;;) {
            sxml_json_node_description description;
            sxml_status status = sxml_json_node_describe(item, &description);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (!description.has_parent) {
                break;
            }
            item = description.parent;
        }
        sxml_item_retain(item);
        *result = sxml_sequence_from_item(item);
        return SXML_STATUS_OK;
    }
    sxml_node_reference node;
    sxml_status status = sxml_item_get_node(item, &node);
    while (status == SXML_STATUS_OK) {
        sxml_node_description description;
        status = sxml_node_describe(node, &description);
        if (status != SXML_STATUS_OK || !description.has_parent) {
            break;
        }
        node = (sxml_node_reference){.tree = node.tree,
                                     .node_index = description.parent,
                                     .namespace_number = 0};
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_node_item_make(node));
    }
    return status;
}

static sxml_status call_jtree(const sxml_function_call *call,
                              sxml_sequence *result) {
    sxml_item json_node = {.representation = 0};
    sxml_status status = sxml_json_node_make_root(
        call->item_heap, call->arguments.sequences[0], &json_node);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(json_node);
    }
    return status;
}

/* fn:concat over atomized arguments, as text. */
static sxml_status call_concat(const sxml_function_call *call,
                               sxml_sequence *result) {
    char text[512];
    size_t byte_count = 0;
    for (size_t argument_index = 0;
         argument_index < call->arguments.sequence_count; argument_index += 1) {
        const sxml_sequence argument = call->arguments.sequences[argument_index];
        if (sxml_sequence_get_representation(argument) ==
            SXML_SEQUENCE_REPRESENTATION_EMPTY) {
            continue;
        }
        uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
        sxml_canonical_text canonical;
        sxml_status status = sxml_cast_write_canonical_text(
            call->item_heap,
            &(sxml_canonical_text_request){
                .item = argument.content,
                .output = {.bytes = output, .byte_count = sizeof(output)}},
            &canonical);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (byte_count + canonical.text.byte_count > sizeof(text)) {
            return SXML_STATUS_CAPACITY_EXCEEDED;
        }
        memcpy(text + byte_count, canonical.text.bytes, canonical.text.byte_count);
        byte_count += canonical.text.byte_count;
    }
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = {.bytes = (const uint8_t *)text,
                                                            .byte_count = byte_count},
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/*
 * fn:for-each($input, $action) as a resumable body (D75): one native step
 * per item, each asking the evaluator to call $action with the item and
 * its position.
 */
typedef struct for_each_state {
    sxml_sequence_builder *builder;
    sxml_sequence_iterator iterator;
    uint64_t position;
    sxml_sequence call_arguments[2];
} for_each_state;

static void release_for_each_state(void *state) {
    for_each_state *for_each = state;
    sxml_sequence_builder_destroy(for_each->builder);
    sxml_sequence_release(for_each->call_arguments[0]);
    sxml_sequence_release(for_each->call_arguments[1]);
}

static sxml_status step_for_each(const sxml_native_step_request *request,
                                 sxml_native_step *step) {
    for_each_state *state = request->state;
    const sxml_function_call *call = request->call;
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        status = sxml_sequence_builder_create(
            sxml_default_allocator(),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap},
            &state->builder);
        state->iterator = sxml_sequence_iterator_start(call->arguments.sequences[0]);
    } else {
        status = sxml_sequence_builder_append_sequence(
            state->builder, request->call_result, call->error_record);
        sxml_sequence_release(request->call_result);
        sxml_sequence_release(state->call_arguments[0]);
        sxml_sequence_release(state->call_arguments[1]);
        state->call_arguments[0] = sxml_sequence_empty();
        state->call_arguments[1] = sxml_sequence_empty();
    }
    sxml_iteration_step item_step;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_iterator_next(&state->iterator, call->error_record,
                                             &item_step);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!item_step.has_item) {
        sxml_sequence value = sxml_sequence_empty();
        status = sxml_sequence_builder_finish(state->builder, &value);
        if (status == SXML_STATUS_OK) {
            *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                                       .result = value};
        }
        return status;
    }
    state->position += 1;
    sxml_item_retain(item_step.item);
    state->call_arguments[0] = sxml_sequence_from_item(item_step.item);
    state->call_arguments[1] = sxml_sequence_from_item(
        sxml_atomic_item_from_integer((int64_t)state->position));
    *step = (sxml_native_step){
        .step_kind = SXML_NATIVE_STEP_KIND_CALL,
        .result = sxml_sequence_empty(),
        .function_item = call->arguments.sequences[1].content,
        .arguments = {.sequences = state->call_arguments, .sequence_count = 2}};
    return SXML_STATUS_OK;
}

static const sxml_resumable_native_body for_each_body = {
    .step = step_for_each,
    .release_state = release_for_each_state,
    .state_byte_count = sizeof(for_each_state),
    .state_alignment = alignof(for_each_state),
};

/* fn:fold-left($input, $zero, $action): the accumulator lives in the
   state between steps. */
typedef struct fold_left_state {
    uint64_t next_index;
    sxml_sequence accumulator;
    sxml_sequence call_arguments[2];
} fold_left_state;

static void release_fold_left_state(void *state) {
    fold_left_state *fold = state;
    sxml_sequence_release(fold->accumulator);
    sxml_sequence_release(fold->call_arguments[0]);
    sxml_sequence_release(fold->call_arguments[1]);
}

static sxml_status step_fold_left(const sxml_native_step_request *request,
                                  sxml_native_step *step) {
    fold_left_state *state = request->state;
    const sxml_function_call *call = request->call;
    if (request->step_number == 0) {
        state->accumulator = call->arguments.sequences[1];
        sxml_sequence_retain(state->accumulator);
    } else {
        sxml_sequence_release(state->accumulator);
        state->accumulator = request->call_result;
        sxml_sequence_release(state->call_arguments[0]);
        sxml_sequence_release(state->call_arguments[1]);
        state->call_arguments[0] = sxml_sequence_empty();
        state->call_arguments[1] = sxml_sequence_empty();
    }
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        call->arguments.sequences[0],
        (sxml_item_request){.item_index = state->next_index,
                            .error_record = call->error_record},
        &item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                                   .result = state->accumulator};
        state->accumulator = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->next_index += 1;
    sxml_sequence_retain(state->accumulator);
    state->call_arguments[0] = state->accumulator;
    sxml_item_retain(item);
    state->call_arguments[1] = sxml_sequence_from_item(item);
    *step = (sxml_native_step){
        .step_kind = SXML_NATIVE_STEP_KIND_CALL,
        .result = sxml_sequence_empty(),
        .function_item = call->arguments.sequences[2].content,
        .arguments = {.sequences = state->call_arguments, .sequence_count = 2}};
    return SXML_STATUS_OK;
}

static const sxml_resumable_native_body fold_left_body = {
    .step = step_fold_left,
    .release_state = release_fold_left_state,
    .state_byte_count = sizeof(fold_left_state),
    .state_alignment = alignof(fold_left_state),
};

/* --------------------------------------------------- Function table */

#define PARAMETER(name_text, type_text)                                       \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                 \
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER(type_text),                 \
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED}

static const sxml_function_parameter input_parameters[] = {
    PARAMETER("input", "item()*")};
static const sxml_function_parameter node_parameters[] = {
    PARAMETER("node", "node()?")};
static const sxml_function_parameter jtree_parameters[] = {
    PARAMETER("input", "item()?")};
static const sxml_function_parameter concat_parameters[] = {
    PARAMETER("values", "xs:anyAtomicType*")};
static const sxml_function_parameter for_each_parameters[] = {
    PARAMETER("input", "item()*"),
    PARAMETER("action", "fn(item(), xs:integer) as item()*")};
static const sxml_function_parameter fold_left_parameters[] = {
    PARAMETER("input", "item()*"), PARAMETER("zero", "item()*"),
    PARAMETER("action", "fn(item()*, item()) as item()*")};

#define ROW(name_text, parameter_array, result_text, property_bits, body)    \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                          \
     .properties = (property_bits),                                           \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {(parameter_array),                                        \
                    sizeof(parameter_array) / sizeof((parameter_array)[0])},  \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                  \
     .call = (body),                                                          \
     .resumable_body = NULL}

#define NULLARY_ROW(name_text, result_text, body)                             \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                          \
     .properties = 0,                                                         \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {NULL, 0},                                                 \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                  \
     .call = (body),                                                          \
     .resumable_body = NULL}

#define RESUMABLE_ROW(name_text, parameter_array, body)                       \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                          \
     .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,                 \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                     \
     .parameters = {(parameter_array),                                        \
                    sizeof(parameter_array) / sizeof((parameter_array)[0])},  \
     .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),                    \
     .call = NULL,                                                            \
     .resumable_body = &(body)}

static const sxml_native_function test_rows[] = {
    NULLARY_ROW("true", "xs:boolean", call_true),
    NULLARY_ROW("false", "xs:boolean", call_false),
    NULLARY_ROW("error", "item()*", call_error),
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("position"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:integer"),
     .call = NULL,
     .resumable_body = NULL},
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = SXML_NATIVE_FUNCTION_READS_CONTEXT_SIZE,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("last"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:integer"),
     .call = NULL,
     .resumable_body = NULL},
    ROW("count", input_parameters, "xs:integer", 0, call_count),
    ROW("root", node_parameters, "node()?", 0, call_root),
    ROW("jtree", jtree_parameters, "jnode()?", 0, call_jtree),
    ROW("concat", concat_parameters, "xs:string", SXML_NATIVE_FUNCTION_IS_VARIADIC,
        call_concat),
    RESUMABLE_ROW("for-each", for_each_parameters, for_each_body),
    RESUMABLE_ROW("fold-left", fold_left_parameters, fold_left_body),
};

#define TEST_ROW_COUNT (sizeof(test_rows) / sizeof(test_rows[0]))
#define TEST_PARAMETER_LIMIT 12

typedef struct test_table {
    sxml_item_heap *item_heap;
    sxml_allocator allocator;
    sxml_name_identifier row_names[TEST_ROW_COUNT];
    bool is_resolved[TEST_ROW_COUNT];
    sxml_sequence_type parameter_types[TEST_ROW_COUNT][TEST_PARAMETER_LIMIT];
    sxml_sequence_type result_types[TEST_ROW_COUNT];
    sxml_sequence_type constructor_parameter_type;
} test_table;

/* Resolves type text the way the function library does (D55). */
static sxml_status resolve_type_text(const test_table *table,
                                     sxml_byte_view type_text,
                                     sxml_sequence_type *sequence_type) {
    sxml_ast *ast = NULL;
    sxml_status status = sxml_xpath_build_sequence_type_ast(
        &(sxml_ast_build_request){.expression_text = type_text,
                                  .allocator = table->allocator},
        &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xpath_static_context static_context = {
        .item_heap = table->item_heap, .allocator = table->allocator};
    status = sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = ast,
                                   .type_node = ast->root,
                                   .static_context = &static_context,
                                   .error_record = NULL},
        sequence_type);
    sxml_ast_destroy(ast);
    return status;
}

static size_t row_parameter_count(size_t row_index) {
    return test_rows[row_index].parameters.function_parameter_count;
}

static sxml_status resolve_row(test_table *table, size_t row_index) {
    if (table->is_resolved[row_index]) {
        return SXML_STATUS_OK;
    }
    const sxml_native_function *row = &test_rows[row_index];
    const size_t parameter_count = row_parameter_count(row_index);
    sxml_status status = SXML_STATUS_OK;
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_count > 0 &&
         parameter_index < TEST_PARAMETER_LIMIT;
         parameter_index += 1) {
        const size_t source_index = parameter_index < parameter_count
                                        ? parameter_index
                                        : parameter_count - 1;
        status = resolve_type_text(
            table, row->parameters.function_parameters[source_index].parameter_type,
            &table->parameter_types[row_index][parameter_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_type_text(table, row->result_type,
                                   &table->result_types[row_index]);
    }
    if (status == SXML_STATUS_OK) {
        table->is_resolved[row_index] = true;
    }
    return status;
}

static bool row_accepts(size_t row_index, size_t arity) {
    const size_t parameter_count = row_parameter_count(row_index);
    if (arity > TEST_PARAMETER_LIMIT) {
        return false;
    }
    if ((test_rows[row_index].properties & SXML_NATIVE_FUNCTION_IS_VARIADIC) != 0) {
        return true;
    }
    return arity == parameter_count;
}

static sxml_status register_signature(const test_table *table,
                                      const sxml_function_entry *entry,
                                      sxml_type_identifier *signature) {
    return sxml_type_registry_register_item_type(
        sxml_item_heap_get_type_registry(table->item_heap),
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {.is_any_function = false,
                                                  .parameter_types =
                                                      entry->parameter_types,
                                                  .result_type =
                                                      entry->result_type}}},
        signature);
}

static sxml_status find_constructor(test_table *table,
                                    const sxml_function_query *query,
                                    sxml_function_entry *entry) {
    sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(table->item_heap);
    sxml_type_identifier type = {0};
    sxml_type_description description = {0};
    if (query->arity != 1 ||
        !sxml_type_registry_find_type(registry, query->function_name, &type) ||
        sxml_type_registry_describe_type(registry, type, &description) !=
            SXML_STATUS_OK ||
        (description.variety != SXML_TYPE_VARIETY_ATOMIC &&
         description.variety != SXML_TYPE_VARIETY_LIST) ||
        type.index == SXML_TYPE_XS_ANY_ATOMIC_TYPE ||
        type.index == SXML_TYPE_XS_NOTATION) {
        return SXML_STATUS_OK;
    }
    sxml_function_entry found = {
        .entry_kind = SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR,
        .constructor_type = type,
        .parameter_types = {.sequence_types = &table->constructor_parameter_type,
                            .sequence_type_count = 1},
        .result_type = {.item_type = type, .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE}};
    sxml_status status = register_signature(table, &found, &found.signature);
    if (status == SXML_STATUS_OK) {
        *entry = found;
    }
    return status;
}

static sxml_status find_test_function(void *state,
                                      const sxml_function_query *query,
                                      sxml_function_entry *entry) {
    test_table *table = state;
    *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    sxml_name_parts parts = {0};
    if (sxml_name_pool_get_name_parts(
            sxml_item_heap_get_name_pool(table->item_heap), query->function_name,
            &parts) == SXML_STATUS_OK &&
        parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA) {
        return find_constructor(table, query, entry);
    }
    for (size_t row_index = 0; row_index < TEST_ROW_COUNT; row_index += 1) {
        if (table->row_names[row_index].index != query->function_name.index ||
            !row_accepts(row_index, query->arity)) {
            continue;
        }
        sxml_status status = resolve_row(table, row_index);
        sxml_function_entry found = {
            .entry_kind = SXML_FUNCTION_ENTRY_KIND_NATIVE,
            .native_function = &test_rows[row_index],
            .parameters = test_rows[row_index].parameters,
            .parameter_types = {.sequence_types = table->parameter_types[row_index],
                                .sequence_type_count = query->arity},
            .result_type = table->result_types[row_index]};
        if (status == SXML_STATUS_OK) {
            status = register_signature(table, &found, &found.signature);
        }
        if (status == SXML_STATUS_OK) {
            *entry = found;
        }
        return status;
    }
    return SXML_STATUS_OK;
}

static const sxml_function_table_operations test_table_operations = {
    .find_function = find_test_function,
};

static sxml_status test_table_initialize(const fixture *test_fixture,
                                         test_table *table) {
    memset(table, 0, sizeof(*table));
    table->item_heap = test_fixture->item_heap;
    table->allocator = sxml_default_allocator();
    table->constructor_parameter_type = (sxml_sequence_type){
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
    for (size_t row_index = 0; row_index < TEST_ROW_COUNT; row_index += 1) {
        sxml_status status = sxml_name_pool_intern_name(
            test_fixture->name_pool,
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(
                    test_rows[row_index].function_namespace),
                .local_name = test_rows[row_index].local_name},
            &table->row_names[row_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------- Mini documents */

static const sxml_source_location no_location = {
    .source_name = {.bytes = NULL, .byte_count = 0},
    .line_number = 0,
    .column_number = 0,
    .byte_offset = 0};

/* Interns a name in no namespace. */
static sxml_status intern_local_name(const fixture *test_fixture,
                                     sxml_byte_view local_name,
                                     sxml_name_identifier *name) {
    return sxml_name_pool_intern_name(
        test_fixture->name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = local_name},
        name);
}

/* Reads a name of letters and digits at text[*position]. */
static sxml_byte_view read_name(const char *text, size_t *position) {
    const size_t start = *position;
    while ((text[*position] >= 'a' && text[*position] <= 'z') ||
           (text[*position] >= 'A' && text[*position] <= 'Z') ||
           (text[*position] >= '0' && text[*position] <= '9')) {
        *position += 1;
    }
    return (sxml_byte_view){.bytes = (const uint8_t *)text + start,
                            .byte_count = *position - start};
}

/* Sends the events of a tiny XML subset (elements, attributes in double
   quotes, text; no entities) to builder, in one loop. */
static sxml_status send_mini_xml(const fixture *test_fixture,
                                 sxml_tree_builder builder, const char *text) {
    sxml_status status = builder.operations->start_document(
        builder.state, &(sxml_document_start_event){.document_uri = text_view(""),
                                                    .base_uri = text_view(""),
                                                    .location = no_location});
    size_t position = 0;
    while (status == SXML_STATUS_OK && text[position] != '\0') {
        if (text[position] != '<') {
            const size_t start = position;
            while (text[position] != '\0' && text[position] != '<') {
                position += 1;
            }
            status = builder.operations->add_text(
                builder.state,
                &(sxml_text_event){.text = {.bytes = (const uint8_t *)text + start,
                                            .byte_count = position - start},
                                   .location = no_location});
            continue;
        }
        position += 1;
        if (text[position] == '/') {
            while (text[position] != '>') {
                position += 1;
            }
            position += 1;
            status = builder.operations->end_element(builder.state);
            continue;
        }
        sxml_name_identifier element_name = {.index = 0};
        status = intern_local_name(test_fixture, read_name(text, &position),
                                   &element_name);
        if (status == SXML_STATUS_OK) {
            status = builder.operations->start_element(
                builder.state, &(sxml_element_start_event){
                                   .element_name = element_name,
                                   .namespace_prefix = {.index = 0},
                                   .type_annotation = {.index = SXML_TYPE_NONE},
                                   .location = no_location});
        }
        while (status == SXML_STATUS_OK && text[position] == ' ') {
            position += 1;
            sxml_name_identifier attribute_name = {.index = 0};
            status = intern_local_name(test_fixture, read_name(text, &position),
                                       &attribute_name);
            position += 2;
            const size_t value_start = position;
            while (text[position] != '"') {
                position += 1;
            }
            if (status == SXML_STATUS_OK) {
                status = builder.operations->add_attribute(
                    builder.state,
                    &(sxml_attribute_event){
                        .attribute_name = attribute_name,
                        .namespace_prefix = {.index = 0},
                        .attribute_value = {.bytes = (const uint8_t *)text + value_start,
                                            .byte_count = position - value_start},
                        .type_annotation = {.index = SXML_TYPE_NONE},
                        .location = no_location});
            }
            position += 1;
        }
        if (status == SXML_STATUS_OK && text[position] == '/') {
            position += 1;
            status = builder.operations->end_element(builder.state);
        }
        position += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    return status;
}

/* Builds a document from mini XML; *document is its document node,
   owned (the arena tree destroys itself when the last node item goes). */
static sxml_status build_document(const fixture *test_fixture, const char *text,
                                  sxml_item *document) {
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        fixture_allocator(test_fixture),
        &(sxml_arena_tree_configuration){
            .allocator = fixture_allocator(test_fixture),
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = test_fixture->item_heap,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = NULL},
        &arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = send_mini_xml(test_fixture, sxml_arena_tree_as_tree_builder(arena_tree),
                           text);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document = sxml_node_item_make((sxml_node_reference){
        .tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    /* The node item holds its own reference. */
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Evaluating */

#define CASE_VARIABLE_LIMIT 2

/* An expression with optional external variables (in no namespace) and
   context value. */
typedef struct evaluation_case {
    const char *expression_text;
    const char *variable_names[CASE_VARIABLE_LIMIT];
    sxml_sequence variable_values[CASE_VARIABLE_LIMIT];
    bool has_context_value;
    sxml_sequence context_value;
    uint32_t maximum_evaluation_depth;
} evaluation_case;

/* What evaluating gave: the status, the error code and location, and the
   value as text. */
typedef struct evaluation_outcome {
    sxml_status status;
    char code[32];
    uint32_t line_number;
    uint32_t column_number;
    char text[2048];
} evaluation_outcome;

/* Appends text to the outcome's text (cut at its capacity). */
static void outcome_append(evaluation_outcome *outcome, sxml_byte_view text) {
    const size_t used = strlen(outcome->text);
    size_t byte_count = text.byte_count;
    if (used + byte_count + 1 > sizeof(outcome->text)) {
        byte_count = sizeof(outcome->text) - used - 1;
    }
    memcpy(outcome->text + used, text.bytes, byte_count);
    outcome->text[used + byte_count] = '\0';
}

/* Writes value as text: atomic items canonically, other items by kind
   (nodes by local name). */
static sxml_status write_value(const fixture *test_fixture, sxml_sequence value,
                               evaluation_outcome *outcome) {
    outcome->text[0] = '\0';
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    bool is_first = true;
    for (;;) {
        sxml_iteration_step step;
        sxml_status status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!step.has_item) {
            return SXML_STATUS_OK;
        }
        if (!is_first) {
            outcome_append(outcome, text_view(" "));
        }
        is_first = false;
        switch (sxml_item_get_kind(step.item)) {
        case SXML_ITEM_KIND_ATOMIC: {
            uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
            sxml_canonical_text canonical;
            status = sxml_cast_write_canonical_text(
                test_fixture->item_heap,
                &(sxml_canonical_text_request){
                    .item = step.item,
                    .output = {.bytes = output, .byte_count = sizeof(output)}},
                &canonical);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            outcome_append(outcome, canonical.text);
            break;
        }
        case SXML_ITEM_KIND_NODE: {
            sxml_node_reference node;
            sxml_node_description description;
            sxml_name_parts parts = {.local_name = {.bytes = NULL, .byte_count = 0}};
            status = sxml_item_get_node(step.item, &node);
            if (status == SXML_STATUS_OK) {
                status = sxml_node_describe(node, &description);
            }
            if (status == SXML_STATUS_OK && description.node_name.index != 0) {
                status = sxml_name_pool_get_name_parts(
                    test_fixture->name_pool, description.node_name, &parts);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            outcome_append(outcome, parts.local_name.byte_count == 0
                                        ? text_view("#node")
                                        : parts.local_name);
            break;
        }
        case SXML_ITEM_KIND_JSON_NODE:
            outcome_append(outcome, text_view("#jnode"));
            break;
        case SXML_ITEM_KIND_FUNCTION:
            outcome_append(outcome, text_view("#function"));
            break;
        case SXML_ITEM_KIND_MAP:
            outcome_append(outcome, text_view("#map"));
            break;
        case SXML_ITEM_KIND_ARRAY:
            outcome_append(outcome, text_view("#array"));
            break;
        }
    }
}

/* Compiles and evaluates test_case in test_fixture, with table. */
typedef struct evaluation_environment {
    fixture *test_fixture;
    test_table *table;
} evaluation_environment;

static void evaluate_with(evaluation_environment environment,
                          const evaluation_case *test_case,
                          evaluation_outcome *outcome) {
    fixture *test_fixture = environment.test_fixture;
    *outcome = (evaluation_outcome){.status = SXML_STATUS_OK};
    sxml_variable_declaration declarations[CASE_VARIABLE_LIMIT];
    sxml_evaluator_slot global_values[CASE_VARIABLE_LIMIT];
    size_t declaration_count = 0;
    while (declaration_count < CASE_VARIABLE_LIMIT &&
           test_case->variable_names[declaration_count] != NULL) {
        sxml_variable_declaration *declaration = &declarations[declaration_count];
        outcome->status = intern_local_name(
            test_fixture, text_view(test_case->variable_names[declaration_count]),
            &declaration->variable_name);
        if (outcome->status != SXML_STATUS_OK) {
            return;
        }
        declaration->declared_type = (sxml_sequence_type){
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
        global_values[declaration_count] = (sxml_evaluator_slot){
            .value = test_case->variable_values[declaration_count],
            .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
        declaration_count += 1;
    }
    const sxml_xpath_static_context static_context = {
        .item_heap = test_fixture->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
        .variable_declarations = {.variable_declarations = declarations,
                                  .variable_declaration_count = declaration_count},
        .function_table = {.state = environment.table,
                           .operations = &test_table_operations},
    };
    sxml_error_record error_record = {.value = NULL};
    sxml_ir_unit *unit = NULL;
    outcome->status = sxml_xpath_compile(
        &(sxml_xpath_compile_request){
            .expression_text = text_view(test_case->expression_text),
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xpath"),
            .static_context = &static_context,
            .error_record = &error_record},
        &unit);
    sxml_dynamic_context *dynamic_context = NULL;
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_dynamic_context_create(
            fixture_allocator(test_fixture),
            &(sxml_evaluator_configuration){
                .item_heap = test_fixture->item_heap,
                .allocator = fixture_allocator(test_fixture),
                .function_table = static_context.function_table,
                .maximum_evaluation_depth = test_case->maximum_evaluation_depth},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){
                .unit = unit,
                .has_context_value = test_case->has_context_value,
                .context_value = test_case->context_value,
                .global_values = {.slots = global_values,
                                  .slot_count = declaration_count},
                .error_record = &error_record},
            &value);
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = write_value(test_fixture, value, outcome);
    } else if (sxml_status_has_error_record(outcome->status)) {
        const size_t byte_count =
            error_record.code.local_name.byte_count < sizeof(outcome->code) - 1
                ? error_record.code.local_name.byte_count
                : sizeof(outcome->code) - 1;
        memcpy(outcome->code, error_record.code.local_name.bytes, byte_count);
        outcome->code[byte_count] = '\0';
        outcome->line_number = error_record.diagnostic.location.line_number;
        outcome->column_number = error_record.diagnostic.location.column_number;
    }
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_ir_unit_release(unit);
}

/* A fixture and table for one case. Static: the 64 KiB stack of the
   WebAssembly tests is not for test buffers. */
typedef struct evaluation_fixture {
    fixture test_fixture;
    test_table table;
    evaluation_outcome outcome;
} evaluation_fixture;

static evaluation_fixture shared_fixture;

static bool evaluation_fixture_start(evaluation_fixture *evaluation) {
    if (fixture_create(0, &evaluation->test_fixture) != SXML_STATUS_OK ||
        test_table_initialize(&evaluation->test_fixture, &evaluation->table) !=
            SXML_STATUS_OK) {
        fixture_destroy(&evaluation->test_fixture);
        return false;
    }
    return true;
}

static evaluation_environment evaluation_fixture_environment(
    evaluation_fixture *evaluation) {
    return (evaluation_environment){.test_fixture = &evaluation->test_fixture,
                                    .table = &evaluation->table};
}

/* True when test_case gives expected_text and nothing leaks. */
static bool case_evaluates_to(const evaluation_case *test_case,
                              const char *expected_text) {
    evaluation_fixture *evaluation = &shared_fixture;
    if (!evaluation_fixture_start(evaluation)) {
        return false;
    }
    evaluate_with(evaluation_fixture_environment(evaluation), test_case,
                  &evaluation->outcome);
    const bool is_equal = evaluation->outcome.status == SXML_STATUS_OK &&
                          strcmp(evaluation->outcome.text, expected_text) == 0;
    if (!is_equal) {
        (void)fprintf(stderr, "for %s\n  status %d %s, expected \"%s\", got \"%s\"\n",
                      test_case->expression_text, (int)evaluation->outcome.status,
                      evaluation->outcome.code, expected_text,
                      evaluation->outcome.text);
    }
    const bool is_clean = fixture_released_everything(&evaluation->test_fixture);
    fixture_destroy(&evaluation->test_fixture);
    return is_equal && is_clean;
}

static bool evaluates_to(const char *expression_text, const char *expected_text) {
    return case_evaluates_to(&(evaluation_case){.expression_text = expression_text},
                             expected_text);
}

/* True when test_case fails with status and the error code, and nothing
   leaks. */
typedef struct expected_failure {
    sxml_status status;
    const char *code;
} expected_failure;

static bool case_fails_with(const evaluation_case *test_case,
                            expected_failure expected) {
    evaluation_fixture *evaluation = &shared_fixture;
    if (!evaluation_fixture_start(evaluation)) {
        return false;
    }
    evaluate_with(evaluation_fixture_environment(evaluation), test_case,
                  &evaluation->outcome);
    const bool is_expected =
        evaluation->outcome.status == expected.status &&
        (expected.code == NULL || strcmp(evaluation->outcome.code, expected.code) == 0);
    if (!is_expected) {
        (void)fprintf(stderr, "for %s\n  expected %d %s, got %d %s \"%s\"\n",
                      test_case->expression_text, (int)expected.status,
                      expected.code == NULL ? "" : expected.code,
                      (int)evaluation->outcome.status, evaluation->outcome.code,
                      evaluation->outcome.text);
    }
    const bool is_clean = fixture_released_everything(&evaluation->test_fixture);
    fixture_destroy(&evaluation->test_fixture);
    return is_expected && is_clean;
}

static bool fails_with(const char *expression_text, const char *code) {
    const bool is_type_error = strncmp(code, "XPTY", 4) == 0 ||
                               strncmp(code, "FOTY", 4) == 0 ||
                               strncmp(code, "FORG0006", 8) == 0;
    return case_fails_with(
        &(evaluation_case){.expression_text = expression_text},
        (expected_failure){.status = is_type_error ? SXML_STATUS_TYPE_ERROR
                                                   : SXML_STATUS_DYNAMIC_ERROR,
                           .code = code});
}

/* ---------------------------------------------------------- Lifecycle */

static void test_dynamic_context_lifecycle(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_evaluator_configuration configuration = {
        .item_heap = test_fixture.item_heap,
        .allocator = fixture_allocator(&test_fixture),
        .maximum_evaluation_depth = 0};
    sxml_memory_requirements requirements;
    SXML_TEST_EXPECT(state, sxml_dynamic_context_memory_requirements(
                                &configuration, &requirements) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, requirements.byte_count == sizeof(sxml_dynamic_context));
    const sxml_evaluator_configuration without_heap = {
        .item_heap = NULL, .allocator = fixture_allocator(&test_fixture)};
    SXML_TEST_EXPECT(state, sxml_dynamic_context_memory_requirements(
                                &without_heap, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);

    alignas(16) static uint8_t memory[sizeof(sxml_dynamic_context) + 16];
    sxml_dynamic_context *context = NULL;
    SXML_TEST_EXPECT(state, sxml_dynamic_context_initialize(
                                (sxml_memory_view){.bytes = memory, .byte_count = 8},
                                &configuration,
                                &context) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state,
                     sxml_dynamic_context_initialize(
                         (sxml_memory_view){.bytes = memory + 1,
                                            .byte_count = sizeof(memory) - 1},
                         &configuration, &context) == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_REQUIRE(state, sxml_dynamic_context_initialize(
                                 (sxml_memory_view){.bytes = memory,
                                                    .byte_count = sizeof(memory)},
                                 &configuration, &context) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, context->maximum_evaluation_depth ==
                                SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_DEPTH);
    int16_t timezone = 1;
    SXML_TEST_EXPECT(state, sxml_dynamic_context_read_implicit_timezone(
                                context, &timezone) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, timezone == 0);
    sxml_clock_reading reading;
    SXML_TEST_EXPECT(state, sxml_dynamic_context_read_clock(context, &reading) ==
                                SXML_STATUS_UNSUPPORTED_FEATURE);
    sxml_dynamic_context_finalize(context);
    sxml_dynamic_context_finalize(NULL);

    SXML_TEST_REQUIRE(state, sxml_dynamic_context_create(
                                 fixture_allocator(&test_fixture), &configuration,
                                 &context) == SXML_STATUS_OK);
    sxml_dynamic_context_destroy(context);
    sxml_dynamic_context_destroy(NULL);
    SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
    fixture_destroy(&test_fixture);
}

/* -------------------------------------------------- Operators (slice) */

static void test_arithmetic_and_comparison_slice_lines(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("1+1 eq 2", "true"));  /* K-NumericAdd-29 */
    SXML_TEST_EXPECT(state, evaluates_to(
                                "xs:int(\"-1873914410\") + xs:int(\"-273569238\")",
                                "-2147483648"));  /* op-numeric-addint2args-1 */
    SXML_TEST_EXPECT(state,
                     evaluates_to("(xs:float(6) div xs:float(2)) instance of xs:float",
                                  "true"));  /* K-NumericDivide-24 */
    SXML_TEST_EXPECT(state, evaluates_to(
                                "xs:int(\"-2147483648\") div xs:int(\"-2147483648\")",
                                "1"));  /* op-numeric-divideint2args-1 */
    SXML_TEST_EXPECT(
        state,
        evaluates_to("(xs:double(6) idiv xs:double(2)) instance of xs:integer",
                     "true"));  /* K-NumericIntegerDivide-17 */
    SXML_TEST_EXPECT(state, evaluates_to(
                                "xs:int(\"-2147483648\") mod xs:int(\"2147483647\")",
                                "-1"));  /* op-numeric-modint2args-1 */
    SXML_TEST_EXPECT(state, evaluates_to("xs:float(\"NaN\") mod 3",
                                         "NaN"));  /* K2-NumericMod-9 */
    SXML_TEST_EXPECT(state, evaluates_to("xs:decimal(6) * xs:decimal(2) eq 12",
                                         "true"));  /* K-NumericMultiply-2 */
    SXML_TEST_EXPECT(state, evaluates_to("xs:unsignedLong(\"184467440737095516\") eq "
                                         "xs:unsignedLong(\"0\")",
                                         "false"));  /* op-numeric-equalulng2args-3 */
    SXML_TEST_EXPECT(state, evaluates_to("'equals' eq 'equals'",
                                         "true"));  /* K-StringEqual-1 */
    SXML_TEST_EXPECT(state, fails_with("(1, 2, 3) eq 3",
                                       "XPTY0004"));  /* K-ValCompTypeChecking-1 */
    SXML_TEST_EXPECT(state, fails_with("(error(), 3) eq 3",
                                       "FOER0000"));  /* K-ValCompTypeChecking-40 */
    SXML_TEST_EXPECT(state, evaluates_to("() = ()", "false"));  /* generalexpression1 */
    SXML_TEST_EXPECT(state, evaluates_to("2 = (1, 2, 3)", "true"));  /* K-GenCompEq-40 */
    SXML_TEST_EXPECT(state, evaluates_to("() < ()", "false"));  /* generalexpression199 */
    SXML_TEST_EXPECT(state, evaluates_to("-(3) + +4 || 'x' || ()", "1x"));
    SXML_TEST_EXPECT(state, fails_with("(1, 2) || 'x'", "XPTY0004"));
}

/* ------------------------------------------- Sequences, filters, maps */

static void test_filters_maps_and_ranges(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("((1 to 25)[. mod 2 eq 0])",
                                         "2 4 6 8 10 12 14 16 18 20 22 24"));
    SXML_TEST_EXPECT(state, evaluates_to("(0, 1, 2)[1] eq 0", "true"));  /* K-FilterExpr-24 */
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 10)!(.*.)",
                                         "1 4 9 16 25 36 49 64 81 100"));  /* bang-1 */
    SXML_TEST_EXPECT(state, evaluates_to("(10, 1 to 4)", "10 1 2 3 4"));  /* rangeExpr-1 */
    SXML_TEST_EXPECT(state, evaluates_to("(-33 to -33)[1]", "-33"));  /* RangeExpr-402b */
    SXML_TEST_EXPECT(state, evaluates_to("count(100000000000 to 100000000003)",
                                         "4"));  /* RangeExpr-408a */
    SXML_TEST_EXPECT(state,
                     fails_with("((28446744073709551616 to 28446744073709551620)!last())[1]",
                                "FOAR0002"));  /* rangeExpr-31 */
    SXML_TEST_EXPECT(state, fails_with("count(-1000000000000000000003 to "
                                       "-1000000000000000000000)",
                                       "FOAR0002"));  /* RangeExpr-411a */
    /* A general comparison does not walk a range to look for dates
       (found by tests/fuzz/xpath_evaluate_fuzz.c, which timed out). */
    SXML_TEST_EXPECT(state, evaluates_to("1 = 1 to 100000000000", "true"));
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 100000000000) = 1", "true"));
    SXML_TEST_EXPECT(state, evaluates_to("(5 to 1)", ""));
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 10)[last()]", "10"));
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 10)[position() = (2, 4)]", "2 4"));
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 10)[2.0]", "2"));
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 10)[2.5]", ""));
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 3)[true()]", "1 2 3"));
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 3)[0]", ""));
    SXML_TEST_EXPECT(state, evaluates_to("('a', 'b')[.]", "a b"));
    SXML_TEST_EXPECT(state, evaluates_to("(1 to 3) ! (., last())", "1 3 2 3 3 3"));
    SXML_TEST_EXPECT(state, fails_with("(1 to 3)[(1, 2)]", "FORG0006"));
    SXML_TEST_EXPECT(state, evaluates_to("some $x in (1, 2) satisfies $x + $x = 3",
                                         "false"));  /* quantExpr-1 */
    SXML_TEST_EXPECT(state, evaluates_to("every $x in (1, 2) satisfies $x < 3", "true"));
    SXML_TEST_EXPECT(state,
                     evaluates_to("some $x in (1, 2), $y in (3, 4) satisfies $x + $y = 6",
                                  "true"));
    SXML_TEST_EXPECT(state, evaluates_to("if (()) then 1 else 2", "2"));
    SXML_TEST_EXPECT(state, evaluates_to("1 and 0 or 'x'", "true"));
    SXML_TEST_EXPECT(state, evaluates_to("let $a := 1, $b := $a + 1 return ($b, $a)",
                                         "2 1"));
    SXML_TEST_EXPECT(state, evaluates_to("for $i in 1 to 3 return $i * 10", "10 20 30"));
    SXML_TEST_EXPECT(state, fails_with("if ((1, 2)) then 1 else 2", "FORG0006"));
}

/* ---------------------------------------------------- Casts and types */

static void test_cast_slice_lines(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("(5 + 5) cast as xs:integer", "10"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:double(1267.43233E12) cast as xs:string",
                                         "1.26743233E15"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:integer(12678967543233) cast as xs:decimal",
                                         "12678967543233"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:dateTime(\"1999-05-31T13:20:00-05:00\") "
                                         "cast as xs:date",
                                         "1999-05-31-05:00"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:untypedAtomic(\"1e-5\") cast as xs:float",
                                         "0.00001"));
    SXML_TEST_EXPECT(state, fails_with("xs:untypedAtomic(\"true\") cast as xs:decimal",
                                       "FORG0001"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:untypedAtomic(\"true\") cast as xs:boolean",
                                         "true"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:string(\"NaN\") cast as xs:float", "NaN"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:decimal(\"-1.1234\") cast as "
                                         "xs:untypedAtomic",
                                         "-1.1234"));
    SXML_TEST_EXPECT(state, fails_with("xs:gDay(\"---31\") cast as xs:duration",
                                       "XPTY0004"));
    SXML_TEST_EXPECT(state, fails_with("xs:date(\"2002-02-30\")", "FORG0001"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:duration(\"P1Y2M3DT10H30M\") cast as "
                                         "xs:duration eq xs:duration(\"P1Y2M3DT10H30M\")",
                                         "true"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:date(\"2004-10-13\") cast as xs:gMonthDay "
                                         "ne xs:gMonthDay(\"--11-13\")",
                                         "true"));
    SXML_TEST_EXPECT(state, fails_with("xs:negativeInteger(xs:double(\"INF\"))",
                                       "FOCA0002"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:untypedAtomic(\"NaN\") castable as xs:float",
                                         "true"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:untypedAtomic(\"-0.0E0\") castable as "
                                         "xs:decimal",
                                         "false"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:float(\"5.4321E-100\") castable as "
                                         "xs:hexBinary",
                                         "false"));
    SXML_TEST_EXPECT(state, evaluates_to("() cast as xs:integer?", ""));
    SXML_TEST_EXPECT(state, evaluates_to("xs:integer#1('12') + 1", "13"));
}

static void test_treat_and_instance_of(sxml_test_state *state) {
    SXML_TEST_EXPECT(state,
                     fails_with("let $var := (100+200) div 2 return "
                                "fn:concat($var treat as xs:string,\"a string\")",
                                "XPDY0050"));  /* treat-as-1 */
    SXML_TEST_EXPECT(state, evaluates_to("let $var := -100 return "
                                         "($var cast as xs:float treat as xs:float)",
                                         "-100"));
    SXML_TEST_EXPECT(state, evaluates_to("xs:dateTime(\"2002-04-02T12:00:00Z\") "
                                         "instance of xs:dateTime",
                                         "true"));  /* instanceof1 */
    SXML_TEST_EXPECT(state, evaluates_to("xs:anyURI(\"http://www.example.com\") "
                                         "instance of xs:float",
                                         "false"));  /* instanceof71 */
    SXML_TEST_EXPECT(state, evaluates_to("1 instance of xs:integer+",
                                         "true"));  /* K-SeqExprInstanceOf-6 */
    SXML_TEST_EXPECT(state, evaluates_to("([] instance of array(*), [] instance of "
                                         "array(empty-sequence()), [] instance of "
                                         "array(xs:string))",
                                         "true true true"));  /* SquareArray-001 */
    SXML_TEST_EXPECT(state, evaluates_to("([1, 'a'] instance of array(xs:integer), "
                                         "[1, 2] instance of array(xs:integer), "
                                         "[[1], [2, 3]] instance of array(array(xs:integer+)))",
                                         "false true true"));
    SXML_TEST_EXPECT(state, evaluates_to("(map{'foo': 1 to 5} instance of "
                                         "map(xs:string, xs:integer+), map{'foo': ()} "
                                         "instance of map(xs:string, xs:integer+))",
                                         "true false"));
    SXML_TEST_EXPECT(state, evaluates_to("(1, 'a') instance of (xs:integer | xs:string)+",
                                         "true"));
    SXML_TEST_EXPECT(state, evaluates_to("1.5 instance of (xs:integer | xs:string)",
                                         "false"));
    SXML_TEST_EXPECT(state, evaluates_to("(1 instance of xs:numeric, 'a' instance of "
                                         "enum('a', 'b'), 'c' instance of enum('a'))",
                                         "true true false"));
    SXML_TEST_EXPECT(state, evaluates_to("(() instance of xs:integer?, (1, 2) instance "
                                         "of xs:integer?, () instance of empty-sequence())",
                                         "true false true"));
    SXML_TEST_EXPECT(state, evaluates_to("(function($a) {$a} instance of "
                                         "function(item()*) as item()*, count#1 instance of "
                                         "function(*), 1 instance of function(*))",
                                         "true true false"));
}

/* ------------------------------------------------- Functions (slice) */

static void test_inline_function_slice_lines(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("let $f := function($x){$x+1} return $f(2)",
                                         "3"));  /* inline-fn-001 */
    SXML_TEST_EXPECT(state,
                     fails_with("(1 to 4) ! (let $add := function($x){$x + .} return "
                                "$add(4))",
                                "XPDY0002"));  /* inline-fn-005 */
    SXML_TEST_EXPECT(state, evaluates_to("let $outer := function($x) { let $inner := "
                                         "function($y) {$y + $x} return $inner(5) } "
                                         "return $outer(3)",
                                         "8"));  /* inline-fn-008 */
    SXML_TEST_EXPECT(state, evaluates_to("let $add := function($x as xs:integer, $y as "
                                         "xs:integer){$x + $y} return $add(3,4)",
                                         "7"));  /* inline-fn-010 */
    SXML_TEST_EXPECT(state, fails_with("let $add := function($x as xs:integer, $y as "
                                       "xs:integer){$x + $y} return $add(3,4.2)",
                                       "XPTY0004"));  /* inline-fn-011 */
    SXML_TEST_EXPECT(state, evaluates_to("let $add := function($x, $y) as xs:double "
                                         "{$x + $y} let $r := $add(3, 4) return ($r, $r "
                                         "instance of xs:double)",
                                         "7 true"));  /* inline-fn-013 */
    SXML_TEST_EXPECT(state, evaluates_to("let $weirdo := function($x, $f) {if ($x=0) "
                                         "then 0 else $f($x - 1, $f)} return "
                                         "$weirdo(12, $weirdo)",
                                         "0"));  /* inline-fn-026 */
    SXML_TEST_EXPECT(state, evaluates_to("let $calc := map{ \"+\" : function($a, $b) "
                                         "{$a + $b}, \"-\" : function($a, $b) {$a - $b}, "
                                         "\"*\" : function($a, $b) {$a * $b}, \"/\" : "
                                         "function($a, $b) {$a div $b}} return "
                                         "$calc(\"*\")(2,3)",
                                         "6"));  /* inline-fn-027 */
    SXML_TEST_EXPECT(state, evaluates_to("let $array:= ['a','b','c','d','e'] return "
                                         "3=>$array()",
                                         "c"));  /* ArrowPostfix-022 */
    SXML_TEST_EXPECT(state, evaluates_to("\"Tu\" => {\"Mo\": \"Monday\", \"Tu\": "
                                         "\"Tuesday\"}()",
                                         "Tuesday"));  /* ArrowExpr-407 */
    SXML_TEST_EXPECT(state, evaluates_to("for $i in 1 to 3 return let $add := "
                                         "function($x){$x + $i} return $add(4)",
                                         "5 6 7"));  /* inline-fn-004, without sum */
    SXML_TEST_EXPECT(state, evaluates_to("let $f := (count#1, true#0) return "
                                         "($f[1]((1, 2)), $f[2]())",
                                         "2 true"));
    SXML_TEST_EXPECT(state, fails_with("let $f := function($a) {$a} return $f(1, 2)",
                                       "XPTY0004"));
    SXML_TEST_EXPECT(state, fails_with("let $f := 1 return $f(1)", "XPTY0004"));
    SXML_TEST_EXPECT(state, fails_with("[1, 2](3)", "FOAY0001"));
    SXML_TEST_EXPECT(state, evaluates_to("map{1: 'a'}(1.0)", "a"));
    SXML_TEST_EXPECT(state, evaluates_to("concat#3('a', 1, ())", "a1"));
}

/* Function coercion (XPath 4.0, section 3.4.4): a function passed where a
   typed function is required is wrapped, and its arguments and result are
   coerced on each call. */
static void test_function_coercion(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("let $apply := function($f as "
                                         "function(xs:integer) as xs:double) {$f(2)} "
                                         "return $apply(function($x) {$x + 1}) instance "
                                         "of xs:double",
                                         "true"));
    SXML_TEST_EXPECT(state, fails_with("let $apply := function($f as "
                                       "function(xs:integer) as xs:integer) {$f(2)} "
                                       "return $apply(function($x) {'a'})",
                                       "XPTY0004"));
    SXML_TEST_EXPECT(state, fails_with("let $apply := function($f as "
                                       "function(xs:integer) as item()*) {$f(2)} return "
                                       "$apply(function($x, $y) {$x})",
                                       "XPTY0004"));
    SXML_TEST_EXPECT(state, evaluates_to("let $apply := function($f as "
                                         "function(xs:integer, xs:integer) as item()*) "
                                         "{$f(2, 3)} return $apply(function($x) {$x * 10})",
                                         "20"));
    SXML_TEST_EXPECT(state, evaluates_to("let $f := function($x as xs:double) {$x} "
                                         "return $f(1) instance of xs:double",
                                         "true"));
    SXML_TEST_EXPECT(state, evaluates_to("let $f := function($x as xs:string) {$x} "
                                         "return $f(xs:untypedAtomic('u')) instance of "
                                         "xs:string",
                                         "true"));
    SXML_TEST_EXPECT(state, evaluates_to("let $f := function($x as xs:positiveInteger) "
                                         "{$x instance of xs:positiveInteger} return $f(3)",
                                         "true"));
    SXML_TEST_EXPECT(state, fails_with("let $f := function($x as xs:positiveInteger) "
                                       "{$x} return $f(0)",
                                       "XPTY0004"));
    SXML_TEST_EXPECT(state, fails_with("let $f := function($x as xs:QName) {$x} return "
                                       "$f(xs:untypedAtomic('a'))",
                                       "XPTY0117"));
}

/* ------------------------------------------------- Maps and arrays */

static void test_lookups_and_constructors(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("(['a', 'b'], ['c', 'd'])[.?1 eq 'c'] ! ?*",
                                         "c d"));  /* Lookup-001 */
    SXML_TEST_EXPECT(state, fails_with("['a', 'b', 'c', 'd']?(2 to 5)",
                                       "FOAY0001"));  /* Lookup-028 */
    SXML_TEST_EXPECT(state, evaluates_to("map{'decimal':true(), 'integer':true(), "
                                         "'polygon':[11,22,33]}?polygon?2",
                                         "22"));  /* Lookup-159 */
    SXML_TEST_EXPECT(state, evaluates_to("(['A', 'B', 'C'], ['D', 'E'])?2",
                                         "B E"));  /* Lookup-225 */
    SXML_TEST_EXPECT(state, evaluates_to("(['a', 'b'], ['c', 'd'])[?1 eq 'c'] ! ?2",
                                         "d"));  /* UnaryLookup-001 */
    SXML_TEST_EXPECT(state, evaluates_to("[['a', 'b', 'c'], ['b', 'c', 'd'], ['e', 'f', "
                                         "'b']]!?*!?1",
                                         "a b e"));  /* UnaryLookup-023 */
    SXML_TEST_EXPECT(state, evaluates_to("map{1: 'x', 2: 'y'}?*", "x y"));
    SXML_TEST_EXPECT(state, evaluates_to("map{1: 'x', 2: 'y'}?(2, 3, 1)", "y x"));
    SXML_TEST_EXPECT(state, fails_with("(1, 2)?1", "XPTY0004"));
    SXML_TEST_EXPECT(state, fails_with("['a']?('x')", "XPTY0004"));
    SXML_TEST_EXPECT(state, fails_with("map{1: 'a', 1.0: 'b'}", "XQDY0137"));
    SXML_TEST_EXPECT(state, fails_with("map{(1, 2): 'a'}", "XPTY0004"));
    SXML_TEST_EXPECT(state, evaluates_to("array{(), ('A', 'B', 'C'), 'D'}?*",
                                         "A B C D"));
    SXML_TEST_EXPECT(state, evaluates_to("count([(), ('A', 'B', 'C'), 'D']?*)", "4"));
    SXML_TEST_EXPECT(state, evaluates_to("[(), ('A', 'B', 'C'), 'D']?2", "A B C"));
    SXML_TEST_EXPECT(state, evaluates_to("[[1, 2], 3] = 3", "true"));
    SXML_TEST_EXPECT(state, fails_with("map{1: 2} + 1", "FOTY0013"));
    SXML_TEST_EXPECT(state, fails_with("function($a) {$a} + 1", "FOTY0013"));
}

/* --------------------------------------------------------------- Paths */

static const char works_document[] =
    "<works><employee name=\"Jane Doe 1\"><hours>40</hours><hours>20</hours>"
    "</employee><employee name=\"John Doe 2\"><hours>30</hours><hours>20</hours>"
    "</employee><employee name=\"Jane Doe 3\"><hours>12</hours></employee>"
    "<employee name=\"John Doe 4\"><hours>80</hours><center/></employee></works>";

/* Evaluates expression_text with the works document as context value and
   as $doc into shared_fixture.outcome; false when something leaked or the
   document could not be built. */
static bool run_path_case(const char *expression_text) {
    evaluation_fixture *evaluation = &shared_fixture;
    if (!evaluation_fixture_start(evaluation)) {
        return false;
    }
    sxml_item document = {.representation = 0};
    bool is_built = build_document(&evaluation->test_fixture, works_document,
                                   &document) == SXML_STATUS_OK;
    if (is_built) {
        const sxml_sequence value = sxml_sequence_from_item(document);
        evaluate_with(evaluation_fixture_environment(evaluation),
                      &(evaluation_case){.expression_text = expression_text,
                                         .variable_names = {"doc"},
                                         .variable_values = {value},
                                         .has_context_value = true,
                                         .context_value = value},
                      &evaluation->outcome);
        sxml_item_release(document);
    }
    const bool is_clean = fixture_released_everything(&evaluation->test_fixture);
    fixture_destroy(&evaluation->test_fixture);
    return is_built && is_clean;
}

static bool path_evaluates_to(const char *expression_text, const char *expected_text) {
    const bool is_clean = run_path_case(expression_text);
    const evaluation_outcome *outcome = &shared_fixture.outcome;
    const bool is_equal = outcome->status == SXML_STATUS_OK &&
                          strcmp(outcome->text, expected_text) == 0;
    if (!is_equal) {
        (void)fprintf(stderr, "for %s\n  status %d %s, expected \"%s\", got \"%s\"\n",
                      expression_text, (int)outcome->status, outcome->code,
                      expected_text, outcome->text);
    }
    return is_clean && is_equal;
}

static bool path_fails_with(const char *expression_text, const char *code) {
    const bool is_clean = run_path_case(expression_text);
    const evaluation_outcome *outcome = &shared_fixture.outcome;
    const bool is_expected = sxml_status_has_error_record(outcome->status) &&
                             strcmp(outcome->code, code) == 0;
    if (!is_expected) {
        (void)fprintf(stderr, "for %s\n  expected %s, got %d %s\n", expression_text,
                      code, (int)outcome->status, outcome->code);
    }
    return is_clean && is_expected;
}

static void test_paths_in_document_order(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, path_evaluates_to("/works/employee[2]/hours[2] + 0", "20"));
    SXML_TEST_EXPECT(state, path_evaluates_to("/ works ! employee[2] ! hours[2] + 1",
                                              "21"));  /* bang-7, number() as + */
    SXML_TEST_EXPECT(state, path_evaluates_to("/ works ! employee[4] ! "
                                              "preceding-sibling::*[1] ! (@name || '')",
                                              "Jane Doe 3"));  /* bang-13 */
    SXML_TEST_EXPECT(state, path_evaluates_to("count(//hours)", "6"));
    SXML_TEST_EXPECT(state, path_evaluates_to("/count(works)", "1"));  /* PathExpr-21 */
    SXML_TEST_EXPECT(state, path_evaluates_to("count(//center/child::*)",
                                              "0"));  /* Axes001-1 */
    SXML_TEST_EXPECT(state, path_evaluates_to("count(//center/descendant::south)",
                                              "0"));  /* Axes032-1 */
    SXML_TEST_EXPECT(state, path_evaluates_to("count(.[5 * /])", "0"));  /* PathExpr-1 */
    SXML_TEST_EXPECT(state, path_evaluates_to("//employee[hours = 20] ! (@name || '')",
                                              "Jane Doe 1 John Doe 2"));
    SXML_TEST_EXPECT(state, path_evaluates_to("(//hours)[last()] + 0", "80"));
    SXML_TEST_EXPECT(state, path_evaluates_to("//hours[1] = (40, 30, 12, 80)",
                                              "true"));
    SXML_TEST_EXPECT(state, path_evaluates_to("(//hours)[1]/ancestor::*",
                                              "works employee"));
    SXML_TEST_EXPECT(state, path_evaluates_to("(//hours)[1]/ancestor-or-self::*[1]",
                                              "hours"));
    SXML_TEST_EXPECT(state, path_evaluates_to("(//hours)[3]/preceding::*",
                                              "employee hours hours"));
    SXML_TEST_EXPECT(state, path_evaluates_to("count((//hours)[3]/following::node())",
                                              "9"));
    SXML_TEST_EXPECT(state, path_evaluates_to("count(//employee/.. | /works)", "1"));
    SXML_TEST_EXPECT(state, path_evaluates_to("count(/works/employee | //employee)",
                                              "4"));
    SXML_TEST_EXPECT(state, path_evaluates_to("count(//employee intersect "
                                              "//employee[hours = 20])",
                                              "2"));
    SXML_TEST_EXPECT(state, path_evaluates_to("(//employee except //employee[hours = "
                                              "20]) ! (@name || '')",
                                              "Jane Doe 3 John Doe 4"));
    SXML_TEST_EXPECT(state, path_evaluates_to("(/works/employee[1]/hours[1], "
                                              "/works/employee[4]/hours[1]) < (10000, "
                                              "50000)",
                                              "true"));  /* generalexpression281 */
    SXML_TEST_EXPECT(state, path_evaluates_to("((//hours)[1] << (//hours)[2], /works "
                                              "is $doc/works, (//hours)[1] >> /works)",
                                              "true true true"));
    /* Name tests on the namespace axis (P4, D77): the prefix. */
    SXML_TEST_EXPECT(state, path_evaluates_to("(count(/works/namespace::xml), "
                                              "count(/works/namespace::other), "
                                              "/works/namespace::xml || '')",
                                              "1 0 http://www.w3.org/XML/1998/namespace"));
    SXML_TEST_EXPECT(state, path_evaluates_to("(//employee)[1]/@name instance of "
                                              "attribute(name)",
                                              "true"));
}

static void test_path_errors(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, fails_with("(1, 2)/a", "XPTY0019"));  /* P5, D76 */
    SXML_TEST_EXPECT(state, fails_with("(1, 2)/(. + 1)", "XPTY0019"));
    SXML_TEST_EXPECT(state, fails_with("(1, 2)//a", "XPTY0019"));
    SXML_TEST_EXPECT(state, fails_with("(1, 2) ! child::a", "XPTY0020"));
    SXML_TEST_EXPECT(state, fails_with("child::a", "XPDY0002"));
    SXML_TEST_EXPECT(state, fails_with("(1, 2) union (1, 2)", "XPTY0004"));  /* K2-SeqUnion-5 */
    SXML_TEST_EXPECT(state, fails_with("/works", "XPDY0002"));
}

static void test_path_errors_with_document(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, path_fails_with("count(/works/employee/(@name, 1))",
                                            "XPTY0018"));
    SXML_TEST_EXPECT(state, path_fails_with("/works/employee/hours + 1", "XPTY0004"));
    SXML_TEST_EXPECT(state, path_fails_with("(/works treat as element(other))",
                                            "XPDY0050"));
    SXML_TEST_EXPECT(state, path_evaluates_to("(/works treat as element(works)) ! "
                                              "count(employee)",
                                              "4"));
}

/* ---------------------------------------------------------------- JNodes */

static void test_json_nodes(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("let $j := jtree([1,2,3]) return $j is "
                                         "root($j/*[3])",
                                         "true"));  /* fn-root-29 */
    SXML_TEST_EXPECT(state, evaluates_to("let $in1 := jtree({\"a\":1, \"b\":2}) let $in2 "
                                         ":= jtree({\"b\":2, \"a\":1}) return count($in1 "
                                         "union $in2)",
                                         "2"));  /* op-union-406 */
    SXML_TEST_EXPECT(state, evaluates_to("jtree([1, 2, 3])/*[3] + 0", "3"));
    SXML_TEST_EXPECT(state, evaluates_to("jtree({'a': 1, 'b': [10, 20]})/b/*[2] + 0 = 20",
                                         "true"));
    SXML_TEST_EXPECT(state, evaluates_to("count(jtree({'a': 1, 'b': [10, 20]})//*)", "4"));
    SXML_TEST_EXPECT(state, evaluates_to("let $j := jtree({'a': 1, 'b': [10, 20]}) "
                                         "return ($j/b/*[2]/.. is $j/b, count($j/b/*[2]/"
                                         "ancestor::*), $j/a/following-sibling::* is "
                                         "$j/b, count($j/b/*[2]/preceding::*))",
                                         "true 2 true 2"));
    SXML_TEST_EXPECT(state, evaluates_to("let $j := jtree([[1, 2], 3]) return "
                                         "($j/*[1]/*[2] << $j/*[2], $j/*[1] is "
                                         "$j/*[1])",
                                         "true true"));
    SXML_TEST_EXPECT(state, evaluates_to("jtree({'a': 1})/a instance of jnode(*, "
                                         "xs:integer)",
                                         "true"));
}

/* ----------------------------------------------------------- Errors */

static void test_errors_have_code_and_location(sxml_test_state *state) {
    evaluation_fixture *evaluation = &shared_fixture;
    SXML_TEST_REQUIRE(state, evaluation_fixture_start(evaluation));
    evaluate_with(evaluation_fixture_environment(evaluation),
                  &(evaluation_case){.expression_text = "1 +\n  ('a' * 2)"},
                  &evaluation->outcome);
    SXML_TEST_EXPECT(state, evaluation->outcome.status == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(state, strcmp(evaluation->outcome.code, "XPTY0004") == 0);
    SXML_TEST_EXPECT(state, evaluation->outcome.line_number == 2);
    SXML_TEST_EXPECT(state, evaluation->outcome.column_number >= 3);
    SXML_TEST_EXPECT(state, fixture_released_everything(&evaluation->test_fixture));
    fixture_destroy(&evaluation->test_fixture);
    SXML_TEST_EXPECT(state, fails_with("error()", "FOER0000"));  /* fn-error-1 */
    SXML_TEST_EXPECT(state, fails_with("if (error()) then true() else true()",
                                       "FOER0000"));  /* cbcl-error-017 */
    SXML_TEST_EXPECT(state, case_fails_with(
                                &(evaluation_case){.expression_text = "$undeclared"},
                                (expected_failure){.status = SXML_STATUS_STATIC_ERROR,
                                                   .code = "XPST0008"}));
}

/* -------------------------------------------- Resumable native bodies */

static void test_resumable_native_bodies(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, evaluates_to("for-each(1 to 5, function($x, $i) {$x * $i})",
                                         "1 4 9 16 25"));
    SXML_TEST_EXPECT(state, evaluates_to("for-each((\"aa\", \"bb\"), concat#1)",
                                         "aa bb"));  /* for-each-001, lower arity */
    SXML_TEST_EXPECT(state, evaluates_to("for-each(22 to 24, fn($it, $pos){$pos, $it})",
                                         "1 22 2 23 3 24"));  /* for-each-402 */
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 5, 0, function($a, $b) "
                                         "{ $a + $b })",
                                         "15"));  /* fold-left-001 */
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 5, (), function($a, $b) "
                                         "{($b, $a)})",
                                         "5 4 3 2 1"));  /* fold-left-005 */
    SXML_TEST_EXPECT(state, fails_with("fold-left(1 to 5, 1, function($a, $b) as "
                                       "element(foo) { $a + $b })",
                                       "XPTY0004"));  /* fold-left-011 */
    SXML_TEST_EXPECT(state, fails_with("fold-left(1 to 3, 0, function($a, $b) "
                                       "{ $a + 'x' })",
                                       "XPTY0004"));
    /* A million calls of a function item from one native body, in constant
       C stack: each is a task, not a C call. */
    SXML_TEST_EXPECT(state, evaluates_to("fold-left(1 to 100000, 0, function($a, $b) "
                                         "{ $a + $b })",
                                         "5000050000"));
    /* Recursion through a native body: user function, native step, user
       function, ... costs tasks, not C stack. */
    SXML_TEST_EXPECT(state, evaluates_to("let $f := function($f, $n) { if ($n = 0) then "
                                         "0 else for-each(1, function($x, $i) { $f($f, $n "
                                         "- 1) + 1 }) } return $f($f, 20000)",
                                         "20000"));
    SXML_TEST_EXPECT(state, evaluates_to("for-each(1 to 3, function($x, $i) { "
                                         "for-each(1 to $x, function($y, $j) { $x * 10 + "
                                         "$y }) })",
                                         "11 21 22 31 32 33"));
}

/* -------------------------------------------------- Recursion depth */

static const char counting_recursion[] =
    "let $f := function($f, $n) { if ($n = 0) then 0 else 1 + $f($f, $n - 1) } "
    "return $f($f, $depth)";

static void test_deep_recursion_reaches_the_limit(sxml_test_state *state) {
    /* 100,000 nested calls (not tail calls: "1 +" waits for each) need heap
       for tasks and frames, not C stack, also under WebAssembly. */
    SXML_TEST_EXPECT(state, case_evaluates_to(
                                &(evaluation_case){
                                    .expression_text = counting_recursion,
                                    .variable_names = {"depth"},
                                    .variable_values = {sxml_sequence_from_item(
                                        sxml_atomic_item_from_integer(100000))},
                                    .maximum_evaluation_depth = 100010},
                                "100000"));
    /* Beyond the configured depth: a limit status, not a stack overflow. */
    SXML_TEST_EXPECT(state, case_fails_with(
                                &(evaluation_case){
                                    .expression_text = counting_recursion,
                                    .variable_names = {"depth"},
                                    .variable_values = {sxml_sequence_from_item(
                                        sxml_atomic_item_from_integer(100020))},
                                    .maximum_evaluation_depth = 100010},
                                (expected_failure){.status = SXML_STATUS_LIMIT_EXCEEDED,
                                                   .code = NULL}));
}

static void test_tail_calls_reuse_their_frame(sxml_test_state *state) {
    /* inline-fn-026's shape: the recursive call is the function's value, so
       it replaces its caller's frame; depth 10 is enough. */
    SXML_TEST_EXPECT(state, case_evaluates_to(
                                &(evaluation_case){
                                    .expression_text =
                                        "let $f := function($f, $n, $sum) { if ($n = 0) "
                                        "then $sum else $f($f, $n - 1, $sum + $n) } "
                                        "return $f($f, 200000, 0)",
                                    .maximum_evaluation_depth = 10},
                                "20000100000"));
}

/* ---------------------------------------- Variables and context value */

static void test_variables_and_context_value(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, case_evaluates_to(
                                &(evaluation_case){
                                    .expression_text = "$x + 1, $y",
                                    .variable_names = {"x", "y"},
                                    .variable_values = {sxml_sequence_from_item(
                                                            sxml_atomic_item_from_integer(41)),
                                                        sxml_sequence_empty()}},
                                "42"));
    SXML_TEST_EXPECT(state, case_evaluates_to(
                                &(evaluation_case){
                                    .expression_text = ". * 2, position(), last()",
                                    .has_context_value = true,
                                    .context_value = sxml_sequence_from_item(
                                        sxml_atomic_item_from_integer(21))},
                                "42 1 1"));
    SXML_TEST_EXPECT(state, fails_with(". + 1", "XPDY0002"));
    SXML_TEST_EXPECT(state, fails_with("let $f := function() { position() } return $f()",
                                       "XPDY0002"));
}

/* ---------------------------------------------- Calls from outside */

/* A function item made by one evaluation and called afterwards, from
   outside any evaluation (sxml_evaluator_call_function_item). */
static void test_function_item_outlives_its_evaluation(sxml_test_state *state) {
    evaluation_fixture *evaluation = &shared_fixture;
    SXML_TEST_REQUIRE(state, evaluation_fixture_start(evaluation));
    fixture *test_fixture = &evaluation->test_fixture;
    const sxml_xpath_static_context static_context = {
        .item_heap = test_fixture->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .function_table = {.state = &evaluation->table,
                           .operations = &test_table_operations}};
    sxml_ir_unit *unit = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_xpath_compile(
                   &(sxml_xpath_compile_request){
                       .expression_text =
                           text_view("let $k := 10 return function($x) { $x * $k }"),
                       .static_context = &static_context},
                   &unit) == SXML_STATUS_OK);
    sxml_dynamic_context *context = NULL;
    SXML_TEST_REQUIRE(state, sxml_dynamic_context_create(
                                 fixture_allocator(test_fixture),
                                 &(sxml_evaluator_configuration){
                                     .item_heap = test_fixture->item_heap,
                                     .allocator = fixture_allocator(test_fixture)},
                                 &context) == SXML_STATUS_OK);
    sxml_sequence function_value = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_evaluator_evaluate(
                                context, &(sxml_evaluation_request){.unit = unit},
                                &function_value) == SXML_STATUS_OK);
    /* The function item keeps the unit alive (D38, D55). */
    sxml_ir_unit_release(unit);
    const sxml_sequence argument =
        sxml_sequence_from_item(sxml_atomic_item_from_integer(4));
    sxml_sequence result = sxml_sequence_empty();
    sxml_error_record error_record = {.value = NULL};
    SXML_TEST_EXPECT(state, sxml_evaluator_call_function_item(
                                &(sxml_function_call){
                                    .function_item = function_value.content,
                                    .arguments = {.sequences = &argument,
                                                  .sequence_count = 1},
                                    .dynamic_context = context,
                                    .item_heap = test_fixture->item_heap,
                                    .error_record = &error_record},
                                &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_sequence_get_representation(result) ==
                                    SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
                                result.content.payload.integer == 40);
    /* Through the public call too: the body runs the machine. */
    sxml_sequence second = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_function_item_call(
                                &(sxml_function_call){
                                    .function_item = function_value.content,
                                    .arguments = {.sequences = &argument,
                                                  .sequence_count = 1},
                                    .dynamic_context = context,
                                    .item_heap = test_fixture->item_heap,
                                    .error_record = &error_record},
                                &second) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, second.content.payload.integer == 40);
    /* Without a dynamic context the body refuses (D38). */
    SXML_TEST_EXPECT(state, sxml_function_item_call(
                                &(sxml_function_call){
                                    .function_item = function_value.content,
                                    .arguments = {.sequences = &argument,
                                                  .sequence_count = 1},
                                    .dynamic_context = NULL,
                                    .item_heap = test_fixture->item_heap,
                                    .error_record = &error_record},
                                &second) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_sequence_release(result);
    sxml_sequence_release(function_value);
    sxml_dynamic_context_destroy(context);
    SXML_TEST_EXPECT(state, fixture_released_everything(test_fixture));
    fixture_destroy(test_fixture);
}

/* ------------------------------------------------ Allocation failures */

/* Every allocation of compiling and evaluating fails in turn: each run
   fails cleanly with OUT_OF_MEMORY and releases everything, until one
   succeeds. */
static void test_evaluation_survives_every_allocation_failure(sxml_test_state *state) {
    static const char expression_text[] =
        "let $f := function($x as xs:integer) { [$x, map{'k': $x * 2}] } "
        "return for-each((1 to 5)[. mod 2 = 1], function($y, $i) { $f($y)?2?k "
        "|| '-' || $i })";
    uint64_t failing_allocation_number = 1;
    for (;;) {
        evaluation_fixture *evaluation = &shared_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(failing_allocation_number,
                                                &evaluation->test_fixture) ==
                                     SXML_STATUS_OK);
        SXML_TEST_REQUIRE(state, test_table_initialize(&evaluation->test_fixture,
                                                       &evaluation->table) ==
                                     SXML_STATUS_OK);
        evaluate_with(evaluation_fixture_environment(evaluation),
                      &(evaluation_case){.expression_text = expression_text},
                      &evaluation->outcome);
        const sxml_allocation_counts counts = sxml_counting_allocator_read_counts(
            evaluation->test_fixture.counting_allocator);
        SXML_TEST_EXPECT(state, fixture_released_everything(&evaluation->test_fixture));
        fixture_destroy(&evaluation->test_fixture);
        if (counts.failed_allocation_count == 0) {
            SXML_TEST_EXPECT(state, evaluation->outcome.status == SXML_STATUS_OK);
            SXML_TEST_EXPECT(state, strcmp(evaluation->outcome.text, "2-1 6-2 10-3") == 0);
            break;
        }
        if (evaluation->outcome.status != SXML_STATUS_OUT_OF_MEMORY) {
            (void)fprintf(stderr, "allocation %llu: status %d\n",
                          (unsigned long long)failing_allocation_number,
                          (int)evaluation->outcome.status);
        }
        SXML_TEST_EXPECT(state, evaluation->outcome.status == SXML_STATUS_OUT_OF_MEMORY);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 20000);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 20);
}

/* ------------------------------------------------------------------ Main */

static const sxml_test_case evaluator_test_cases[] = {
    {.name = "dynamic_context_lifecycle", .function = test_dynamic_context_lifecycle},
    {.name = "arithmetic_and_comparison_slice_lines",
     .function = test_arithmetic_and_comparison_slice_lines},
    {.name = "filters_maps_and_ranges", .function = test_filters_maps_and_ranges},
    {.name = "cast_slice_lines", .function = test_cast_slice_lines},
    {.name = "treat_and_instance_of", .function = test_treat_and_instance_of},
    {.name = "inline_function_slice_lines", .function = test_inline_function_slice_lines},
    {.name = "function_coercion", .function = test_function_coercion},
    {.name = "lookups_and_constructors", .function = test_lookups_and_constructors},
    {.name = "paths_in_document_order", .function = test_paths_in_document_order},
    {.name = "path_errors", .function = test_path_errors},
    {.name = "path_errors_with_document", .function = test_path_errors_with_document},
    {.name = "json_nodes", .function = test_json_nodes},
    {.name = "errors_have_code_and_location",
     .function = test_errors_have_code_and_location},
    {.name = "resumable_native_bodies", .function = test_resumable_native_bodies},
    {.name = "deep_recursion_reaches_the_limit",
     .function = test_deep_recursion_reaches_the_limit},
    {.name = "tail_calls_reuse_their_frame", .function = test_tail_calls_reuse_their_frame},
    {.name = "variables_and_context_value", .function = test_variables_and_context_value},
    {.name = "function_item_outlives_its_evaluation",
     .function = test_function_item_outlives_its_evaluation},
    {.name = "evaluation_survives_every_allocation_failure",
     .function = test_evaluation_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "evaluator",
            .cases = evaluator_test_cases,
            .case_count = SXML_TEST_COUNT(evaluator_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
