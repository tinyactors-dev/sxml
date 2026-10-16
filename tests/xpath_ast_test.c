/*
 * Tests for the AST lifecycle and builder functions of
 * src/xpath_ast_internal.h (src/xpath_ast.c). The front end that fills an
 * AST from XPath text is tested in tests/xpath_parse_test.c.
 *
 * Golden file: tests/memory_test.c.
 */
#include "test_runner.h"
#include "xpath_ast_internal.h"

#include <stdalign.h>

/* Caller memory for objects placed with initialize; 16 suits every type. */
#define TEST_STORAGE_ALIGNMENT 16

static sxml_ast_configuration configuration_of(size_t node_capacity,
                                               size_t text_byte_capacity) {
    return (sxml_ast_configuration){
        .node_capacity = node_capacity,
        .text_byte_capacity = text_byte_capacity,
        .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xpath"),
    };
}

static sxml_ast_node_request leaf_request(sxml_ast_kind kind) {
    return (sxml_ast_node_request){
        .kind = kind,
        .detail =
            {
                .operator= SXML_AST_OPERATOR_NONE },
                .flags = 0,
                .source_span = {.byte_offset = 0,
                                .byte_count = 1,
                                .line_number = 1,
                                .column_number = 1},
                .payload = {.text = {.byte_offset = 0, .byte_count = 0}},
        };
}

/* An AST placed in storage; storage must stay valid while it is used. */
typedef struct ast_fixture {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[1024];
    sxml_ast *ast;
} ast_fixture;

static sxml_status
ast_fixture_initialize(ast_fixture *fixture,
                       sxml_ast_configuration configuration) {
    return sxml_ast_initialize(
        (sxml_memory_view){.bytes = fixture->storage,
                           .byte_count = sizeof(fixture->storage)},
        &configuration, &fixture->ast);
}

/* ------------------------------------------------------------ Lifecycle */

static void test_requirements_grow_with_capacities(sxml_test_state *state) {
    sxml_memory_requirements small = {.byte_count = 0, .alignment = 0};
    sxml_memory_requirements large = {.byte_count = 0, .alignment = 0};
    sxml_ast_configuration small_configuration = configuration_of(1, 1);
    sxml_ast_configuration large_configuration = configuration_of(10, 100);
    SXML_TEST_REQUIRE(state,
                      sxml_ast_memory_requirements(&small_configuration,
                                                   &small) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_ast_memory_requirements(&large_configuration,
                                                   &large) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, small.alignment >= alignof(sxml_ast_node));
    /* The header, two nodes (one is the sentinel) and one text byte. */
    SXML_TEST_EXPECT(state, small.byte_count >= sizeof(sxml_ast) +
                                                    2 * sizeof(sxml_ast_node) +
                                                    1);
    SXML_TEST_EXPECT(state, large.byte_count >= small.byte_count +
                                                    9 * sizeof(sxml_ast_node) +
                                                    99);
}

static void
test_requirements_reject_capacities_above_maximum(sxml_test_state *state) {
    sxml_memory_requirements requirements = {.byte_count = 7, .alignment = 1};
    sxml_ast_configuration too_many_nodes =
        configuration_of((size_t)UINT32_MAX, 1);
    SXML_TEST_EXPECT(
        state, sxml_ast_memory_requirements(&too_many_nodes, &requirements) ==
                   SXML_STATUS_INVALID_ARGUMENT);
    if (sizeof(size_t) > sizeof(uint32_t)) {
        sxml_ast_configuration too_much_text =
            configuration_of(1, (size_t)UINT32_MAX + 1U);
        SXML_TEST_EXPECT(state, sxml_ast_memory_requirements(&too_much_text,
                                                             &requirements) ==
                                    SXML_STATUS_INVALID_ARGUMENT);
    }
    SXML_TEST_EXPECT(state, requirements.byte_count == 7);
}

static void test_initialize_places_empty_ast(sxml_test_state *state) {
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_REQUIRE(
        state, ast_fixture_initialize(&fixture, configuration_of(4, 16)) ==
                   SXML_STATUS_OK);
    const sxml_ast *ast = fixture.ast;
    SXML_TEST_EXPECT(state, (const void *)ast == (const void *)fixture.storage);
    SXML_TEST_EXPECT(state, ast->node_count == 1);
    SXML_TEST_EXPECT(state, ast->node_capacity == 4);
    SXML_TEST_EXPECT(state, ast->text_byte_capacity == 16);
    SXML_TEST_EXPECT(state, ast->root.value == 0);
    SXML_TEST_EXPECT(state, sxml_allocator_is_null(ast->allocator));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                ast->source_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("test.xpath")));
    SXML_TEST_EXPECT(state, sxml_ast_get_node(ast, ast->root)->kind ==
                                SXML_AST_KIND_NONE);
    sxml_ast_finalize(fixture.ast);
    sxml_ast_finalize(NULL);
}

static void test_initialize_checks_memory(sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[64] = {0};
    sxml_ast *ast = NULL;
    sxml_ast_configuration configuration = configuration_of(8, 8);
    SXML_TEST_EXPECT(
        state, sxml_ast_initialize(
                   (sxml_memory_view){.bytes = storage, .byte_count = 16},
                   &configuration, &ast) == SXML_STATUS_INSUFFICIENT_MEMORY);
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_EXPECT(
        state,
        sxml_ast_initialize(
            (sxml_memory_view){.bytes = fixture.storage + 1,
                               .byte_count = sizeof(fixture.storage) - 1},
            &configuration, &ast) == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, ast == NULL);
}

static void test_create_and_destroy_through_allocator(sxml_test_state *state) {
    sxml_ast *ast = NULL;
    sxml_ast_configuration configuration = configuration_of(100, 1000);
    SXML_TEST_REQUIRE(state,
                      sxml_ast_create(sxml_default_allocator(), &configuration,
                                      &ast) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !sxml_allocator_is_null(ast->allocator));
    SXML_TEST_EXPECT(state, ast->node_capacity == 100);
    sxml_ast_destroy(ast);
    sxml_ast_destroy(NULL);
}

/* A counting allocator over the default allocator, placed on the stack. */
typedef struct counting_fixture {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[128];
    sxml_counting_allocator *counting_allocator;
} counting_fixture;

static sxml_status
counting_fixture_initialize(counting_fixture *fixture,
                            uint64_t failing_allocation_number) {
    return sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = fixture->storage,
                           .byte_count = sizeof(fixture->storage)},
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = failing_allocation_number,
        },
        &fixture->counting_allocator);
}

static void
test_create_survives_every_allocation_failure(sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
        SXML_TEST_REQUIRE(state, counting_fixture_initialize(
                                     &fixture, failing_allocation_number) ==
                                     SXML_STATUS_OK);
        sxml_ast *ast = NULL;
        sxml_ast_configuration configuration = configuration_of(3, 3);
        sxml_status status = sxml_ast_create(
            sxml_counting_allocator_as_allocator(fixture.counting_allocator),
            &configuration, &ast);
        sxml_ast_destroy(ast);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(fixture.counting_allocator);
        sxml_counting_allocator_finalize(fixture.counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, ast == NULL);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 10);
    }
    /* One block holds the whole AST. */
    SXML_TEST_EXPECT(state, failing_allocation_number == 2);
}

/* --------------------------------------------------------------- Builder */

static void test_add_node_fills_capacity(sxml_test_state *state) {
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_REQUIRE(
        state, ast_fixture_initialize(&fixture, configuration_of(2, 0)) ==
                   SXML_STATUS_OK);
    sxml_ast *ast = fixture.ast;
    sxml_ast_node_request request =
        leaf_request(SXML_AST_KIND_CONTEXT_VALUE_REFERENCE);
    request.source_span.byte_offset = 5;
    sxml_ast_node_index first = {.value = 0};
    sxml_ast_node_index second = {.value = 0};
    sxml_ast_node_index third = {.value = 99};
    SXML_TEST_EXPECT(state, sxml_ast_add_node(ast, &request, &first) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_ast_add_node(ast, &request, &second) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_ast_add_node(ast, &request, &third) ==
                                SXML_STATUS_CAPACITY_EXCEEDED);
    SXML_TEST_EXPECT(state, first.value == 1);
    SXML_TEST_EXPECT(state, second.value == 2);
    SXML_TEST_EXPECT(state, third.value == 99);
    const sxml_ast_node *node = sxml_ast_get_node(ast, second);
    SXML_TEST_EXPECT(state,
                     node->kind == SXML_AST_KIND_CONTEXT_VALUE_REFERENCE);
    SXML_TEST_EXPECT(state, node->source_span.byte_offset == 5);
    SXML_TEST_EXPECT(state, node->parent.value == 0);
    /* An index not in use reads as the sentinel. */
    SXML_TEST_EXPECT(
        state,
        sxml_ast_get_node(ast, (sxml_ast_node_index){.value = 3})->kind ==
            SXML_AST_KIND_NONE);
}

static void test_add_node_rejects_invalid_requests(sxml_test_state *state) {
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_REQUIRE(
        state, ast_fixture_initialize(&fixture, configuration_of(4, 4)) ==
                   SXML_STATUS_OK);
    sxml_ast_node_index node = {.value = 0};
    sxml_ast_node_request none = leaf_request(SXML_AST_KIND_NONE);
    SXML_TEST_EXPECT(state, sxml_ast_add_node(fixture.ast, &none, &node) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_ast_node_request literal = leaf_request(SXML_AST_KIND_STRING_LITERAL);
    literal.payload.text = (sxml_ast_text){.byte_offset = 0, .byte_count = 1};
    SXML_TEST_EXPECT(state, sxml_ast_add_node(fixture.ast, &literal, &node) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_ast_node_request name = leaf_request(SXML_AST_KIND_NCNAME);
    name.payload.name.namespace_uri =
        (sxml_ast_text){.byte_offset = 2, .byte_count = 0};
    SXML_TEST_EXPECT(state, sxml_ast_add_node(fixture.ast, &name, &node) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, node.value == 0);
    SXML_TEST_EXPECT(state, fixture.ast->node_count == 1);
}

static void test_append_child_links_in_order(sxml_test_state *state) {
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_REQUIRE(
        state, ast_fixture_initialize(&fixture, configuration_of(4, 0)) ==
                   SXML_STATUS_OK);
    sxml_ast *ast = fixture.ast;
    sxml_ast_node_request list_request =
        leaf_request(SXML_AST_KIND_ARGUMENT_LIST);
    sxml_ast_node_request item_request =
        leaf_request(SXML_AST_KIND_CONTEXT_VALUE_REFERENCE);
    sxml_ast_node_index list = {.value = 0};
    sxml_ast_node_index first = {.value = 0};
    sxml_ast_node_index other = {.value = 0};
    sxml_ast_node_index second = {.value = 0};
    SXML_TEST_REQUIRE(state, sxml_ast_add_node(ast, &list_request, &list) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_ast_add_node(ast, &item_request, &first) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_ast_add_node(ast, &item_request, &other) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_ast_add_node(ast, &item_request, &second) ==
                                 SXML_STATUS_OK);
    const sxml_ast_field fields[] = {
        SXML_AST_FIELD_ARGUMENT, SXML_AST_FIELD_NAME, SXML_AST_FIELD_ARGUMENT};
    const sxml_ast_node_index children[] = {first, other, second};
    for (size_t child_index = 0; child_index < 3; child_index += 1) {
        SXML_TEST_EXPECT(
            state, sxml_ast_append_child(ast,
                                         (sxml_ast_child_request){
                                             .parent = list,
                                             .field = fields[child_index],
                                             .child = children[child_index]}) ==
                       SXML_STATUS_OK);
    }
    sxml_ast_node_index argument = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = list,
                                    .field = SXML_AST_FIELD_ARGUMENT});
    SXML_TEST_EXPECT(state, argument.value == first.value);
    argument = sxml_ast_find_next_with_same_field(ast, argument);
    SXML_TEST_EXPECT(state, argument.value == second.value);
    argument = sxml_ast_find_next_with_same_field(ast, argument);
    SXML_TEST_EXPECT(state, argument.value == 0);
    SXML_TEST_EXPECT(
        state, sxml_ast_find_child(
                   ast, (sxml_ast_child_query){.parent = list,
                                               .field = SXML_AST_FIELD_NAME})
                       .value == other.value);
    SXML_TEST_EXPECT(
        state, sxml_ast_find_child(
                   ast, (sxml_ast_child_query){.parent = list,
                                               .field = SXML_AST_FIELD_TYPE})
                       .value == 0);
    SXML_TEST_EXPECT(state, sxml_ast_get_node(ast, second)->parent.value ==
                                list.value);
    SXML_TEST_EXPECT(state, sxml_ast_get_node(ast, list)->last_child.value ==
                                second.value);
}

static void test_append_child_rejects_invalid_links(sxml_test_state *state) {
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_REQUIRE(
        state, ast_fixture_initialize(&fixture, configuration_of(4, 0)) ==
                   SXML_STATUS_OK);
    sxml_ast *ast = fixture.ast;
    sxml_ast_node_request request = leaf_request(SXML_AST_KIND_PREDICATE);
    sxml_ast_node_index parent = {.value = 0};
    sxml_ast_node_index child = {.value = 0};
    SXML_TEST_REQUIRE(state, sxml_ast_add_node(ast, &request, &parent) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_ast_add_node(ast, &request, &child) ==
                                 SXML_STATUS_OK);
    const sxml_ast_child_request invalid_requests[] = {
        {.parent = {.value = 0}, .field = SXML_AST_FIELD_ITEM, .child = child},
        {.parent = parent, .field = SXML_AST_FIELD_ITEM, .child = {.value = 0}},
        {.parent = parent, .field = SXML_AST_FIELD_ITEM, .child = {.value = 7}},
        {.parent = parent, .field = SXML_AST_FIELD_ITEM, .child = parent},
        {.parent = parent, .field = SXML_AST_FIELD_NONE, .child = child},
    };
    for (size_t request_index = 0; request_index < 5; request_index += 1) {
        SXML_TEST_EXPECT(state, sxml_ast_append_child(
                                    ast, invalid_requests[request_index]) ==
                                    SXML_STATUS_INVALID_ARGUMENT);
    }
    const sxml_ast_child_request valid = {
        .parent = parent, .field = SXML_AST_FIELD_ITEM, .child = child};
    SXML_TEST_EXPECT(state,
                     sxml_ast_append_child(ast, valid) == SXML_STATUS_OK);
    /* A child has at most one parent. */
    SXML_TEST_EXPECT(state, sxml_ast_append_child(ast, valid) ==
                                SXML_STATUS_INVALID_ARGUMENT);
}

static void test_append_text_starts_and_extends_texts(sxml_test_state *state) {
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_REQUIRE(
        state, ast_fixture_initialize(&fixture, configuration_of(1, 8)) ==
                   SXML_STATUS_OK);
    sxml_ast *ast = fixture.ast;
    sxml_ast_text first = {.byte_offset = 0, .byte_count = 0};
    sxml_ast_text second = {.byte_offset = 0, .byte_count = 0};
    SXML_TEST_EXPECT(
        state, sxml_ast_append_text(ast, SXML_BYTE_VIEW_FROM_LITERAL("ab"),
                                    &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_ast_append_text(ast, SXML_BYTE_VIEW_FROM_LITERAL("c"),
                                          &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, sxml_ast_append_text(ast, SXML_BYTE_VIEW_FROM_LITERAL("xyz"),
                                    &second) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(sxml_ast_get_text(ast, first),
                                           SXML_BYTE_VIEW_FROM_LITERAL("abc")));
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(sxml_ast_get_text(ast, second),
                                           SXML_BYTE_VIEW_FROM_LITERAL("xyz")));
    /* first no longer ends the store. */
    SXML_TEST_EXPECT(
        state, sxml_ast_append_text(ast, SXML_BYTE_VIEW_FROM_LITERAL("d"),
                                    &first) == SXML_STATUS_INVALID_ARGUMENT);
    /* Six of eight bytes are used. */
    SXML_TEST_EXPECT(
        state, sxml_ast_append_text(ast, SXML_BYTE_VIEW_FROM_LITERAL("123"),
                                    &second) == SXML_STATUS_CAPACITY_EXCEEDED);
    SXML_TEST_EXPECT(
        state, sxml_ast_append_text(ast, SXML_BYTE_VIEW_FROM_LITERAL("12"),
                                    &second) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, second.byte_count == 5);
    SXML_TEST_EXPECT(state,
                     sxml_ast_get_text(ast, (sxml_ast_text){.byte_offset = 7,
                                                            .byte_count = 2})
                             .byte_count == 0);
}

static void test_source_location_names_source(sxml_test_state *state) {
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_REQUIRE(
        state, ast_fixture_initialize(&fixture, configuration_of(1, 0)) ==
                   SXML_STATUS_OK);
    sxml_ast_node_request request =
        leaf_request(SXML_AST_KIND_CONTEXT_VALUE_REFERENCE);
    request.source_span = (sxml_ast_source_span){.byte_offset = 12,
                                                 .byte_count = 1,
                                                 .line_number = 2,
                                                 .column_number = 4};
    sxml_ast_node_index node = {.value = 0};
    SXML_TEST_REQUIRE(state, sxml_ast_add_node(fixture.ast, &request, &node) ==
                                 SXML_STATUS_OK);
    sxml_source_location location =
        sxml_ast_get_source_location(fixture.ast, node);
    SXML_TEST_EXPECT(state, location.line_number == 2);
    SXML_TEST_EXPECT(state, location.column_number == 4);
    SXML_TEST_EXPECT(state, location.byte_offset == 12);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                location.source_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("test.xpath")));
}

static const sxml_test_case xpath_ast_test_cases[] = {
    {.name = "requirements_grow_with_capacities",
     .function = test_requirements_grow_with_capacities},
    {.name = "requirements_reject_capacities_above_maximum",
     .function = test_requirements_reject_capacities_above_maximum},
    {.name = "initialize_places_empty_ast",
     .function = test_initialize_places_empty_ast},
    {.name = "initialize_checks_memory",
     .function = test_initialize_checks_memory},
    {.name = "create_and_destroy_through_allocator",
     .function = test_create_and_destroy_through_allocator},
    {.name = "create_survives_every_allocation_failure",
     .function = test_create_survives_every_allocation_failure},
    {.name = "add_node_fills_capacity",
     .function = test_add_node_fills_capacity},
    {.name = "add_node_rejects_invalid_requests",
     .function = test_add_node_rejects_invalid_requests},
    {.name = "append_child_links_in_order",
     .function = test_append_child_links_in_order},
    {.name = "append_child_rejects_invalid_links",
     .function = test_append_child_rejects_invalid_links},
    {.name = "append_text_starts_and_extends_texts",
     .function = test_append_text_starts_and_extends_texts},
    {.name = "source_location_names_source",
     .function = test_source_location_names_source},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xpath_ast",
            .cases = xpath_ast_test_cases,
            .case_count = SXML_TEST_COUNT(xpath_ast_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
