/*
 * Tests for the AST lifecycle and builder functions of
 * src/xpath_ast_internal.h (src/xpath_ast.c), and for the parser part seam
 * of the front end (D111) with test parts over grammars/xpath. The front
 * end that fills an AST from XPath text is tested in
 * tests/xpath_parse_test.c.
 *
 * Golden file: tests/memory_test.c.
 */
#include "test_runner.h"
#include "xpath_ast_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* Defined by the generated parser of grammars/xpath (D12). */
const struct TSLanguage *
tree_sitter_xpath(void); // NOLINT(readability-identifier-naming)

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
    sxml_ast_node_request instruction =
        leaf_request(SXML_AST_KIND_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR);
    instruction.payload.processing_instruction.content =
        (sxml_ast_text){.byte_offset = 0, .byte_count = 3};
    SXML_TEST_EXPECT(state,
                     sxml_ast_add_node(fixture.ast, &instruction, &node) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    sxml_ast_node_request qualified_name = leaf_request(SXML_AST_KIND_QNAME);
    qualified_name.payload.name.local_name =
        (sxml_ast_text){.byte_offset = 1, .byte_count = 1};
    SXML_TEST_EXPECT(state,
                     sxml_ast_add_node(fixture.ast, &qualified_name, &node) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    sxml_ast_node_request element_text =
        leaf_request(SXML_AST_KIND_DIRECT_ELEMENT_TEXT);
    element_text.payload.text =
        (sxml_ast_text){.byte_offset = 0, .byte_count = 2};
    SXML_TEST_EXPECT(state,
                     sxml_ast_add_node(fixture.ast, &element_text, &node) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, node.value == 0);
    SXML_TEST_EXPECT(state, fixture.ast->node_count == 1);
}

static void test_add_node_takes_xquery_payloads(sxml_test_state *state) {
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_REQUIRE(
        state, ast_fixture_initialize(&fixture, configuration_of(4, 16)) ==
                   SXML_STATUS_OK);
    sxml_ast *ast = fixture.ast;
    sxml_ast_text target = {.byte_offset = 0, .byte_count = 0};
    sxml_ast_text content = {.byte_offset = 0, .byte_count = 0};
    SXML_TEST_REQUIRE(state, sxml_ast_append_text(
                                 ast, SXML_BYTE_VIEW_FROM_LITERAL("pi"),
                                 &target) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_ast_append_text(
                                 ast, SXML_BYTE_VIEW_FROM_LITERAL("data"),
                                 &content) == SXML_STATUS_OK);
    sxml_ast_node_request instruction =
        leaf_request(SXML_AST_KIND_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR);
    instruction.payload.processing_instruction =
        (sxml_ast_processing_instruction){.target = target,
                                          .content = content};
    sxml_ast_node_index node = {.value = 0};
    SXML_TEST_REQUIRE(state, sxml_ast_add_node(ast, &instruction, &node) ==
                                 SXML_STATUS_OK);
    const sxml_ast_processing_instruction stored =
        sxml_ast_get_node(ast, node)->payload.processing_instruction;
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_ast_get_text(ast, stored.target),
                                SXML_BYTE_VIEW_FROM_LITERAL("pi")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_ast_get_text(ast, stored.content),
                                SXML_BYTE_VIEW_FROM_LITERAL("data")));
}

static void test_add_flags_marks_nodes(sxml_test_state *state) {
    ast_fixture fixture = {.storage = {0}, .ast = NULL};
    SXML_TEST_REQUIRE(
        state, ast_fixture_initialize(&fixture, configuration_of(4, 0)) ==
                   SXML_STATUS_OK);
    sxml_ast *ast = fixture.ast;
    sxml_ast_node_request text_request =
        leaf_request(SXML_AST_KIND_DIRECT_ELEMENT_TEXT);
    text_request.flags = (uint32_t)SXML_AST_FLAG_FIXED;
    sxml_ast_node_index node = {.value = 0};
    SXML_TEST_REQUIRE(state, sxml_ast_add_node(ast, &text_request, &node) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state,
        sxml_ast_add_flags(
            ast, (sxml_ast_flags_request){
                     .node = node,
                     .flags = (uint32_t)SXML_AST_FLAG_BOUNDARY_WHITESPACE}) ==
            SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_ast_get_node(ast, node)->flags ==
                                ((uint32_t)SXML_AST_FLAG_FIXED |
                                 (uint32_t)SXML_AST_FLAG_BOUNDARY_WHITESPACE));
    SXML_TEST_EXPECT(state, sxml_ast_add_flags(
                                ast, (sxml_ast_flags_request){
                                         .node = {.value = 0}, .flags = 1}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_ast_add_flags(
                                ast, (sxml_ast_flags_request){
                                         .node = {.value = 2}, .flags = 1}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
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

/* ------------------------------------------------------ Parser parts */

static sxml_ast_build_request build_request_of(sxml_byte_view text,
                                               sxml_error_record *record) {
    return (sxml_ast_build_request){
        .expression_text = text,
        .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xq"),
        .allocator = sxml_default_allocator(),
        .maximum_expression_depth = 0,
        .error_record = record,
    };
}

/* A part that adds nothing: the XPath grammar's root, no tables, no hooks. */
static const sxml_ast_parser_part inert_part = {
    .language = tree_sitter_xpath,
    .root_node_type = SXML_BYTE_VIEW_INITIALIZER("xpath"),
    .node_types = {.entries = NULL, .entry_count = 0},
    .fields = {.entries = NULL, .entry_count = 0},
    .unnamed_children = {.entries = NULL, .entry_count = 0},
    .syntax_error_description = {.bytes = NULL, .byte_count = 0},
    .missing_token_description = {.bytes = NULL, .byte_count = 0},
    .apply_token = NULL,
    .describe_payload = NULL,
    .finish_node = NULL,
};

/* Claims the operator of an additive expression and makes it MINUS. */
static bool test_apply_token(sxml_ast_parser_token token,
                             sxml_ast_node_request *request) {
    if (request->kind != SXML_AST_KIND_ADDITIVE_EXPRESSION ||
        !sxml_byte_view_equals(token.field_name,
                               SXML_BYTE_VIEW_FROM_LITERAL("operator"))) {
        return false;
    }
    request->detail.operator= SXML_AST_OPERATOR_MINUS;
    return true;
}

/* Describes string literals itself: the payload is always "PART". */
static sxml_status test_describe_payload(sxml_ast_parser_node node,
                                         sxml_ast_node_request *request,
                                         sxml_ast_payload_outcome *outcome) {
    if (request->kind != SXML_AST_KIND_STRING_LITERAL) {
        return SXML_STATUS_OK;
    }
    sxml_ast_text text = {.byte_offset = 0, .byte_count = 0};
    const sxml_status status =
        sxml_ast_append_text(sxml_ast_front_end_get_ast(node.front_end),
                             SXML_BYTE_VIEW_FROM_LITERAL("PART"), &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    request->payload.text = text;
    outcome->is_described = true;
    return SXML_STATUS_OK;
}

/*
 * Marks each complete node STABLE after checking that its children are
 * complete; raises XQST0118 at the integer literal 13.
 */
static sxml_status test_finish_node(sxml_ast_front_end *front_end,
                                    sxml_ast_node_index node) {
    sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    const sxml_ast_node *finished = sxml_ast_get_node(ast, node);
    for (sxml_ast_node_index child = finished->first_child; child.value != 0;
         child = sxml_ast_get_node(ast, child)->next_sibling) {
        if ((sxml_ast_get_node(ast, child)->flags &
             (uint32_t)SXML_AST_FLAG_STABLE) == 0) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    if (finished->kind == SXML_AST_KIND_INTEGER_LITERAL &&
        sxml_byte_view_equals(
            sxml_ast_get_text(ast, finished->payload.text),
            SXML_BYTE_VIEW_FROM_LITERAL("13"))) {
        return sxml_ast_front_end_raise(
            front_end,
            (sxml_ast_static_error){
                .error_code = SXML_BYTE_VIEW_FROM_LITERAL("XQST0118"),
                .description = SXML_BYTE_VIEW_FROM_LITERAL("thirteen"),
                .byte_offset = finished->source_span.byte_offset});
    }
    return sxml_ast_add_flags(
        ast, (sxml_ast_flags_request){.node = node,
                                      .flags = (uint32_t)SXML_AST_FLAG_STABLE});
}

static const sxml_ast_parser_part hooked_part = {
    .language = tree_sitter_xpath,
    .root_node_type = SXML_BYTE_VIEW_INITIALIZER("xpath"),
    .node_types = {.entries = NULL, .entry_count = 0},
    .fields = {.entries = NULL, .entry_count = 0},
    .unnamed_children = {.entries = NULL, .entry_count = 0},
    .syntax_error_description = SXML_BYTE_VIEW_INITIALIZER("not test syntax"),
    .missing_token_description = {.bytes = NULL, .byte_count = 0},
    .apply_token = test_apply_token,
    .describe_payload = test_describe_payload,
    .finish_node = test_finish_node,
};

/* True when both ASTs have the same nodes and the same text. */
static bool asts_are_equal(const sxml_ast *left, const sxml_ast *right) {
    return left->node_count == right->node_count &&
           left->root.value == right->root.value &&
           left->text_byte_count == right->text_byte_count &&
           memcmp(left->nodes, right->nodes,
                  left->node_count * sizeof(sxml_ast_node)) == 0 &&
           (left->text_byte_count == 0 ||
            memcmp(left->text_bytes, right->text_bytes,
                   left->text_byte_count) == 0);
}

static void test_inert_part_builds_the_xpath_ast(sxml_test_state *state) {
    static const char *const expressions[] = {
        "for $x at $i in (1 to 3) return $x * 2",
        "/p:a[@b = \"c\"\"\"]//d ! string(), 'it''s'",
        "map { 'k': [1, 2] }?k?* => sum() otherwise jnode(())",
        "`x{1}y{{`, -0x1_F, 1.5e3, fn($a as xs:integer) { $a }(1)",
    };
    for (size_t expression_index = 0;
         expression_index < sizeof expressions / sizeof expressions[0];
         expression_index += 1U) {
        const sxml_ast_build_request request = build_request_of(
            sxml_byte_view_from_c_string(expressions[expression_index]),
            NULL);
        sxml_ast *expected = NULL;
        sxml_ast *built = NULL;
        SXML_TEST_REQUIRE(state, sxml_xpath_build_ast(&request, &expected) ==
                                     SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_ast_build_with_parser_part(
                                    &request, &inert_part, &built) ==
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state,
                         built != NULL && asts_are_equal(expected, built));
        sxml_ast_destroy(built);
        sxml_ast_destroy(expected);
    }
}

static void test_part_hooks_run_in_order(sxml_test_state *state) {
    const sxml_ast_build_request request = build_request_of(
        SXML_BYTE_VIEW_FROM_LITERAL("'abcdefgh' , 1 + (2, 3)"), NULL);
    sxml_ast *ast = NULL;
    SXML_TEST_REQUIRE(state, sxml_ast_build_with_parser_part(
                                 &request, &hooked_part, &ast) ==
                                 SXML_STATUS_OK);
    size_t literal_count = 0;
    size_t additive_count = 0;
    for (uint32_t node_index = 1; node_index < ast->node_count;
         node_index += 1U) {
        const sxml_ast_node *node = sxml_ast_get_node(
            ast, (sxml_ast_node_index){.value = node_index});
        /* finish_node ran once for every node, the root included. */
        SXML_TEST_EXPECT(state,
                         (node->flags & (uint32_t)SXML_AST_FLAG_STABLE) != 0);
        if (node->kind == SXML_AST_KIND_STRING_LITERAL) {
            literal_count += 1U;
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                        sxml_ast_get_text(ast, node->payload.text),
                                        SXML_BYTE_VIEW_FROM_LITERAL("PART")));
        }
        if (node->kind == SXML_AST_KIND_ADDITIVE_EXPRESSION) {
            additive_count += 1U;
            SXML_TEST_EXPECT(state, node->detail.operator==
                                        SXML_AST_OPERATOR_MINUS);
        }
    }
    SXML_TEST_EXPECT(state, literal_count == 1);
    SXML_TEST_EXPECT(state, additive_count == 1);
    SXML_TEST_EXPECT(state, sxml_ast_get_node(ast, ast->root)->kind ==
                                SXML_AST_KIND_XPATH);
    sxml_ast_destroy(ast);
}

static void test_part_hook_raises_its_code(sxml_test_state *state) {
    sxml_error_record record;
    memset(&record, 0, sizeof record);
    const sxml_ast_build_request request =
        build_request_of(SXML_BYTE_VIEW_FROM_LITERAL("1,\n  13"), &record);
    sxml_ast *ast = NULL;
    SXML_TEST_EXPECT(state, sxml_ast_build_with_parser_part(
                                &request, &hooked_part, &ast) ==
                                SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, ast == NULL);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.code.local_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("XQST0118")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.code.namespace_uri,
                                SXML_BYTE_VIEW_FROM_LITERAL(
                                    SXML_ERROR_NAMESPACE_URI)));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.diagnostic.description,
                                SXML_BYTE_VIEW_FROM_LITERAL("thirteen")));
    SXML_TEST_EXPECT(state, record.diagnostic.location.line_number == 2);
    SXML_TEST_EXPECT(state, record.diagnostic.location.column_number == 3);
    SXML_TEST_EXPECT(state, record.diagnostic.location.byte_offset == 5);
}

static void test_part_syntax_errors(sxml_test_state *state) {
    sxml_error_record record;
    memset(&record, 0, sizeof record);
    const sxml_ast_build_request request =
        build_request_of(SXML_BYTE_VIEW_FROM_LITERAL("1 +* 2"), &record);
    sxml_ast *ast = NULL;
    SXML_TEST_EXPECT(state, sxml_ast_build_with_parser_part(
                                &request, &hooked_part, &ast) ==
                                SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.code.local_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("XPST0003")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.diagnostic.description,
                                SXML_BYTE_VIEW_FROM_LITERAL("not test syntax")));
    /* A root of another node type is XPST0003 too. */
    sxml_ast_parser_part other_root = inert_part;
    other_root.root_node_type = SXML_BYTE_VIEW_FROM_LITERAL("module");
    const sxml_ast_build_request valid =
        build_request_of(SXML_BYTE_VIEW_FROM_LITERAL("1"), &record);
    SXML_TEST_EXPECT(state, sxml_ast_build_with_parser_part(
                                &valid, &other_root, &ast) ==
                                SXML_STATUS_STATIC_ERROR);
    sxml_ast_parser_part no_language = inert_part;
    no_language.language = NULL;
    SXML_TEST_EXPECT(state, sxml_ast_build_with_parser_part(
                                &valid, &no_language, &ast) ==
                                SXML_STATUS_UNSUPPORTED_FEATURE);
    SXML_TEST_EXPECT(state, sxml_ast_build_with_parser_part(&valid, NULL,
                                                            &ast) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, ast == NULL);
}

/* ------------------------------------------------------- Copies and memo */

static void test_copy_is_the_original_with_its_source_name(
    sxml_test_state *state) {
    const sxml_ast_build_request request = build_request_of(
        SXML_BYTE_VIEW_FROM_LITERAL("for $x in (1, 'a''b') return $x[. eq 2]"),
        NULL);
    sxml_ast *original = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_xpath_build_ast(&request, &original) == SXML_STATUS_OK);
    sxml_ast *copy = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_ast_copy(&(sxml_ast_copy_request){
                                 .original = original,
                                 .allocator = sxml_default_allocator(),
                                 .source_name =
                                     SXML_BYTE_VIEW_FROM_LITERAL("other.xsl")},
                             &copy) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, asts_are_equal(original, copy));
    SXML_TEST_EXPECT(state, copy->node_capacity == original->node_capacity);
    SXML_TEST_EXPECT(state,
                     copy->text_byte_capacity == original->text_byte_capacity);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                copy->source_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("other.xsl")));
    sxml_ast_destroy(copy);
    sxml_ast_destroy(original);
}

/* A memo build of text through build, compared with a direct build. */
typedef struct memo_case {
    sxml_ast_build_function build;
    const char *text;
} memo_case;

static void test_memo_parses_each_text_once(sxml_test_state *state) {
    static const memo_case cases[] = {
        {.build = sxml_xpath_build_ast, .text = "1 + 2"},
        {.build = sxml_xpath_build_ast, .text = "@a = 'b'"},
        {.build = sxml_xpath_build_ast, .text = "1 + 2"},
        {.build = sxml_xslt_pattern_build_ast, .text = "a/b"},
        {.build = sxml_xpath_build_ast, .text = "a/b"},
        {.build = sxml_xpath_build_sequence_type_ast, .text = "xs:integer*"},
        {.build = sxml_xpath_build_ast, .text = "@a = 'b'"},
        {.build = sxml_xslt_pattern_build_ast, .text = "a/b"},
    };
    sxml_ast_memo memo = sxml_ast_memo_empty(sxml_default_allocator());
    for (size_t case_index = 0; case_index < sizeof cases / sizeof cases[0];
         case_index += 1U) {
        const sxml_ast_build_request request = build_request_of(
            sxml_byte_view_from_c_string(cases[case_index].text), NULL);
        sxml_ast *expected = NULL;
        sxml_ast *memoized = NULL;
        SXML_TEST_REQUIRE(state, cases[case_index].build(&request, &expected) ==
                                     SXML_STATUS_OK);
        SXML_TEST_EXPECT(state,
                         sxml_ast_memo_build(
                             &memo,
                             &(sxml_ast_memo_request){
                                 .build = cases[case_index].build,
                                 .build_request = &request},
                             &memoized) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, memoized != NULL &&
                                    asts_are_equal(expected, memoized) &&
                                    memoized->node_capacity ==
                                        expected->node_capacity &&
                                    sxml_byte_view_equals(memoized->source_name,
                                                          request.source_name));
        sxml_ast_destroy(memoized);
        sxml_ast_destroy(expected);
    }
    /* The same text by another front end is another entry. */
    SXML_TEST_EXPECT(state, memo.used_count == 5);
    /* Other limits are another entry too. */
    sxml_ast_build_request bounded =
        build_request_of(SXML_BYTE_VIEW_FROM_LITERAL("1 + 2"), NULL);
    bounded.maximum_expression_depth = 10;
    sxml_ast *ast = NULL;
    SXML_TEST_EXPECT(state, sxml_ast_memo_build(
                                &memo,
                                &(sxml_ast_memo_request){
                                    .build = sxml_xpath_build_ast,
                                    .build_request = &bounded},
                                &ast) == SXML_STATUS_OK);
    sxml_ast_destroy(ast);
    SXML_TEST_EXPECT(state, memo.used_count == 6);
    sxml_ast_memo_release(&memo);
    SXML_TEST_EXPECT(state, memo.used_count == 0 && memo.slot_count == 0);
}

static void test_memo_keeps_no_error(sxml_test_state *state) {
    sxml_ast_memo memo = sxml_ast_memo_empty(sxml_default_allocator());
    for (int attempt = 0; attempt < 2; attempt += 1) {
        sxml_error_record record;
        memset(&record, 0, sizeof record);
        const sxml_ast_build_request request =
            build_request_of(SXML_BYTE_VIEW_FROM_LITERAL("1 +"), &record);
        sxml_ast *ast = NULL;
        SXML_TEST_EXPECT(state, sxml_ast_memo_build(
                                    &memo,
                                    &(sxml_ast_memo_request){
                                        .build = sxml_xpath_build_ast,
                                        .build_request = &request},
                                    &ast) == SXML_STATUS_STATIC_ERROR);
        SXML_TEST_EXPECT(state, ast == NULL);
        /* Each use reports the error itself. */
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    record.code.local_name,
                                    SXML_BYTE_VIEW_FROM_LITERAL("XPST0003")));
    }
    SXML_TEST_EXPECT(state, memo.used_count == 0);
    sxml_ast_memo_release(&memo);
}

static void test_memo_grows_and_survives_every_allocation_failure(
    sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
        SXML_TEST_REQUIRE(state, counting_fixture_initialize(
                                     &fixture, failing_allocation_number) ==
                                     SXML_STATUS_OK);
        const sxml_allocator allocator =
            sxml_counting_allocator_as_allocator(fixture.counting_allocator);
        sxml_ast_memo memo = sxml_ast_memo_empty(allocator);
        sxml_status status = SXML_STATUS_OK;
        /* 40 texts, each twice: the table grows past its first 64 slots. */
        char text[16];
        for (int pass_number = 0; pass_number < 2 && status == SXML_STATUS_OK;
             pass_number += 1) {
            for (int text_number = 0;
                 text_number < 40 && status == SXML_STATUS_OK;
                 text_number += 1) {
                const int length = snprintf(text, sizeof text, "%d + .", text_number);
                sxml_ast_build_request request = build_request_of(
                    (sxml_byte_view){.bytes = (const uint8_t *)text,
                                     .byte_count = (size_t)length},
                    NULL);
                request.allocator = allocator;
                sxml_ast *ast = NULL;
                status = sxml_ast_memo_build(
                    &memo,
                    &(sxml_ast_memo_request){.build = sxml_xpath_build_ast,
                                             .build_request = &request},
                    &ast);
                SXML_TEST_EXPECT(state, (status == SXML_STATUS_OK) == (ast != NULL));
                sxml_ast_destroy(ast);
            }
        }
        const size_t used_count = memo.used_count;
        sxml_ast_memo_release(&memo);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(fixture.counting_allocator);
        sxml_counting_allocator_finalize(fixture.counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, used_count == 40);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 1000);
    }
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
    {.name = "add_node_takes_xquery_payloads",
     .function = test_add_node_takes_xquery_payloads},
    {.name = "add_flags_marks_nodes", .function = test_add_flags_marks_nodes},
    {.name = "append_child_links_in_order",
     .function = test_append_child_links_in_order},
    {.name = "append_child_rejects_invalid_links",
     .function = test_append_child_rejects_invalid_links},
    {.name = "append_text_starts_and_extends_texts",
     .function = test_append_text_starts_and_extends_texts},
    {.name = "source_location_names_source",
     .function = test_source_location_names_source},
    {.name = "inert_part_builds_the_xpath_ast",
     .function = test_inert_part_builds_the_xpath_ast},
    {.name = "part_hooks_run_in_order",
     .function = test_part_hooks_run_in_order},
    {.name = "part_hook_raises_its_code",
     .function = test_part_hook_raises_its_code},
    {.name = "part_syntax_errors", .function = test_part_syntax_errors},
    {.name = "copy_is_the_original_with_its_source_name",
     .function = test_copy_is_the_original_with_its_source_name},
    {.name = "memo_parses_each_text_once",
     .function = test_memo_parses_each_text_once},
    {.name = "memo_keeps_no_error", .function = test_memo_keeps_no_error},
    {.name = "memo_grows_and_survives_every_allocation_failure",
     .function = test_memo_grows_and_survives_every_allocation_failure},
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
