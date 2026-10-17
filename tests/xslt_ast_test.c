/*
 * Unit tests for src/xslt_ast.c: the stylesheet AST's lifecycle, builder,
 * import precedence and text form (src/xslt_ast_internal.h, D102).
 *
 * Golden file: tests/ir_test.c (a unit built by hand, its text form
 * compared with the expected text).
 */
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"
#include "xpath_ast_internal.h"
#include "xslt_ast_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* A processor, a document to stand for every module, and an AST. */
typedef struct test_fixture {
    sxml_processor *processor;
    sxml_item document;
    sxml_xslt_ast *ast;
} test_fixture;

static bool fixture_open(test_fixture *fixture, const char *document_text) {
    const sxml_allocator allocator = sxml_default_allocator();
    memset(fixture, 0, sizeof(*fixture));
    if (sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                              &fixture->processor) != SXML_STATUS_OK) {
        return false;
    }
    if (sxml_processor_parse_document(
            fixture->processor,
            &(sxml_xml_parse_request){.input = sxml_byte_view_from_c_string(document_text)},
            &fixture->document) != SXML_STATUS_OK) {
        return false;
    }
    return sxml_xslt_ast_create(
               allocator,
               &(sxml_xslt_ast_configuration){
                   .allocator = allocator,
                   .item_heap = sxml_processor_get_item_heap(fixture->processor),
                   .maximum_module_count = 0,
                   .maximum_node_count = 0},
               &fixture->ast) == SXML_STATUS_OK;
}

static void fixture_close(test_fixture *fixture) {
    sxml_xslt_ast_destroy(fixture->ast);
    sxml_item_release(fixture->document);
    sxml_processor_destroy(fixture->processor);
}

static sxml_name_identifier intern_name(const test_fixture *fixture, const char *local_name) {
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_name(sxml_processor_get_name_pool(fixture->processor),
                                   &(sxml_name_request){.namespace_uri = {.index = 0},
                                                        .local_name = sxml_byte_view_from_c_string(
                                                            local_name)},
                                   &name) != SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return name;
}

typedef struct module_addition {
    sxml_xslt_ast_module_relation relation;
    uint32_t parent_module;
    uint32_t referencing_node;
    const char *uri;
} module_addition;

static sxml_status add_module(test_fixture *fixture, module_addition addition,
                              sxml_xslt_ast_module_index *module) {
    return sxml_xslt_ast_add_module(
        fixture->ast,
        &(sxml_xslt_ast_module_request){
            .relation = addition.relation,
            .parent_module = {.value = addition.parent_module},
            .referencing_node = {.value = addition.referencing_node},
            .document = fixture->document,
            .module_uri = sxml_byte_view_from_c_string(addition.uri)},
        module);
}

typedef struct node_addition {
    sxml_xslt_ast_kind kind;
    uint32_t flags;
    uint32_t module;
    uint32_t parent;
    uint32_t static_context;
    uint32_t line_number;
} node_addition;

static sxml_status add_node(test_fixture *fixture, node_addition addition,
                            sxml_xslt_ast_node_index *node) {
    sxml_status status = sxml_xslt_ast_add_node(
        fixture->ast,
        &(sxml_xslt_ast_node_request){
            .kind = addition.kind,
            .flags = addition.flags,
            .module = {.value = addition.module},
            .source_node = {.index = 0},
            .static_context = {.value = addition.static_context},
            .element_name = {.index = 0},
            .namespace_prefix = {.index = 0},
            .content = {.kind = SXML_XSLT_AST_VALUE_KIND_NONE},
            .source_span = {.byte_offset = 0,
                            .byte_count = 0,
                            .line_number = addition.line_number,
                            .column_number = addition.line_number == 0 ? 0U : 3U}},
        node);
    if (status == SXML_STATUS_OK && addition.parent != 0) {
        status = sxml_xslt_ast_append_child(
            fixture->ast, (sxml_xslt_ast_child_request){.parent = {.value = addition.parent},
                                                        .child = *node});
    }
    return status;
}

/* Includes and imports mixed: precedence is the post-order of the import
   tree, and an included module shares its includer's (3.10.2, 3.10.3). */
static void test_import_precedence(sxml_test_state *state) {
    test_fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    SXML_TEST_REQUIRE(state, fixture_open(&fixture, "<a/>"));
    sxml_xslt_ast_module_index module = {.value = 0};
    sxml_xslt_ast_node_index node = {.value = 0};
    /* 1 includes 2, 2 imports 3, 1 imports 4, 4 includes 5. */
    SXML_TEST_REQUIRE(state,
                      add_module(&fixture,
                                 (module_addition){.relation =
                                                       SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL,
                                                   .uri = "one.xsl"},
                                 &module) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                                      .module = 1},
                                      &node) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_INCLUDE,
                                                      .flags = SXML_XSLT_AST_NODE_FLAG_DECLARATION,
                                                      .module = 1,
                                                      .parent = 1},
                                      &node) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      add_module(&fixture,
                                 (module_addition){.relation =
                                                       SXML_XSLT_AST_MODULE_RELATION_INCLUDED,
                                                   .parent_module = 1,
                                                   .referencing_node = 2,
                                                   .uri = "two.xsl"},
                                 &module) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_xslt_ast_get_node(fixture.ast, node)->included_module.value == 2);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                                      .module = 2},
                                      &node) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_IMPORT,
                                                      .flags = SXML_XSLT_AST_NODE_FLAG_DECLARATION,
                                                      .module = 2,
                                                      .parent = 3},
                                      &node) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      add_module(&fixture,
                                 (module_addition){.relation =
                                                       SXML_XSLT_AST_MODULE_RELATION_IMPORTED,
                                                   .parent_module = 2,
                                                   .referencing_node = 4,
                                                   .uri = "three.xsl"},
                                 &module) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                                      .module = 3},
                                      &node) == SXML_STATUS_OK);
    /* Module 3 is done; module 1 is on the path of the last module. */
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_IMPORT,
                                                      .flags = SXML_XSLT_AST_NODE_FLAG_DECLARATION,
                                                      .module = 1,
                                                      .parent = 1},
                                      &node) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      add_module(&fixture,
                                 (module_addition){.relation =
                                                       SXML_XSLT_AST_MODULE_RELATION_IMPORTED,
                                                   .parent_module = 1,
                                                   .referencing_node = node.value,
                                                   .uri = "four.xsl"},
                                 &module) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                                      .module = 4},
                                      &node) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_INCLUDE,
                                                      .flags = SXML_XSLT_AST_NODE_FLAG_DECLARATION,
                                                      .module = 4,
                                                      .parent = node.value},
                                      &node) == SXML_STATUS_OK);
    /* Module 2 is not on the path of module 4: refused. */
    SXML_TEST_EXPECT(state,
                     add_module(&fixture,
                                (module_addition){.relation =
                                                      SXML_XSLT_AST_MODULE_RELATION_INCLUDED,
                                                  .parent_module = 2,
                                                  .referencing_node = 4,
                                                  .uri = "late.xsl"},
                                &module) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_REQUIRE(state,
                      add_module(&fixture,
                                 (module_addition){.relation =
                                                       SXML_XSLT_AST_MODULE_RELATION_INCLUDED,
                                                   .parent_module = 4,
                                                   .referencing_node = node.value,
                                                   .uri = "five.xsl"},
                                 &module) == SXML_STATUS_OK);
    sxml_xslt_ast_seal(fixture.ast);
    static const uint32_t expected_precedences[] = {0, 3, 3, 1, 2, 2};
    for (uint32_t module_number = 1; module_number <= 5; module_number += 1) {
        SXML_TEST_EXPECT(state,
                         sxml_xslt_ast_get_module(fixture.ast, (sxml_xslt_ast_module_index){
                                                                   .value = module_number})
                                 ->import_precedence == expected_precedences[module_number]);
    }
    SXML_TEST_EXPECT(state, sxml_xslt_ast_get_module(fixture.ast,
                                                     (sxml_xslt_ast_module_index){.value = 3})
                                    ->nesting_depth == 2);
    /* Declarations in the order they were added, numbered from 1. */
    SXML_TEST_EXPECT(state, sxml_xslt_ast_get_declaration(fixture.ast, 1).value == 2);
    SXML_TEST_EXPECT(state, sxml_xslt_ast_get_declaration(fixture.ast, 2).value == 4);
    SXML_TEST_EXPECT(state, sxml_xslt_ast_get_declaration(fixture.ast, 5).value == 0);
    /* Sealed: nothing grows. */
    SXML_TEST_EXPECT(state, add_node(&fixture,
                                     (node_addition){.kind = SXML_XSLT_AST_KIND_TEMPLATE,
                                                     .module = 1},
                                     &node) == SXML_STATUS_INVALID_ARGUMENT);
    fixture_close(&fixture);
}

/* The text form of a small stylesheet built by hand. */
static void test_text_form(sxml_test_state *state) {
    test_fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    SXML_TEST_REQUIRE(state,
                      fixture_open(&fixture, "<xsl:stylesheet "
                                             "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'>"
                                             "<xsl:template match='item'/></xsl:stylesheet>"));
    sxml_xslt_ast *ast = fixture.ast;
    sxml_xslt_ast_module_index module = {.value = 0};
    SXML_TEST_REQUIRE(state,
                      add_module(&fixture,
                                 (module_addition){.relation =
                                                       SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL,
                                                   .uri = "main.xsl"},
                                 &module) == SXML_STATUS_OK);
    sxml_namespace_uri_identifier xslt_namespace = {.index = SXML_NAMESPACE_XSLT};
    sxml_namespace_prefix_identifier xsl_prefix = {.index = 0};
    SXML_TEST_REQUIRE(state, sxml_name_pool_intern_namespace_prefix(
                                 sxml_processor_get_name_pool(fixture.processor),
                                 SXML_BYTE_VIEW_FROM_LITERAL("xsl"),
                                 &xsl_prefix) == SXML_STATUS_OK);
    const sxml_namespace_binding binding = {.namespace_prefix = xsl_prefix,
                                            .namespace_uri = xslt_namespace};
    sxml_xslt_ast_static_context_index context = {.value = 0};
    SXML_TEST_REQUIRE(
        state,
        sxml_xslt_ast_add_static_context(
            ast,
            &(sxml_xslt_ast_static_context_request){
                .parent = {.value = 0},
                .namespace_bindings = {.namespace_bindings = &binding,
                                       .namespace_binding_count = 1},
                .default_collation = {.bytes = NULL, .byte_count = 0},
                .default_mode = {.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED},
                .default_validation = SXML_XSLT_AST_KEYWORD_STRIP,
                .expands_text = false,
                .version = 3.0,
                .excluded_namespaces = {.namespace_uris = &xslt_namespace,
                                        .namespace_uri_count = 1},
                .extension_namespaces = {.namespace_uris = NULL, .namespace_uri_count = 0},
                .static_base_uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///main.xsl")},
            &context) == SXML_STATUS_OK);
    sxml_xslt_ast_node_index root = {.value = 0};
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                                      .module = 1,
                                                      .static_context = 1,
                                                      .line_number = 1},
                                      &root) == SXML_STATUS_OK);
    sxml_xslt_ast_text version_text = {.byte_offset = 0, .byte_count = 0};
    SXML_TEST_REQUIRE(state, sxml_xslt_ast_append_text(ast, SXML_BYTE_VIEW_FROM_LITERAL("3."),
                                                       &version_text) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_xslt_ast_append_text(ast, SXML_BYTE_VIEW_FROM_LITERAL("0"),
                                                       &version_text) == SXML_STATUS_OK);
    sxml_xslt_ast_attribute_index attribute = {.value = 0};
    SXML_TEST_REQUIRE(
        state, sxml_xslt_ast_add_attribute(
                   ast,
                   &(sxml_xslt_ast_attribute_request){
                       .node = root,
                       .kind = SXML_XSLT_AST_ATTRIBUTE_VERSION,
                       .attribute_name = intern_name(&fixture, "version"),
                       .value = {.kind = SXML_XSLT_AST_VALUE_KIND_TEXT,
                                 .payload = {.text = version_text}},
                       .source_span = {.line_number = 1, .column_number = 17}},
                   &attribute) == SXML_STATUS_OK);
    sxml_xslt_ast_node_index template_node = {.value = 0};
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_TEMPLATE,
                                                      .flags = SXML_XSLT_AST_NODE_FLAG_DECLARATION,
                                                      .module = 1,
                                                      .parent = root.value,
                                                      .static_context = 1,
                                                      .line_number = 2},
                                      &template_node) == SXML_STATUS_OK);
    /* Attributes go only to the node added last. */
    SXML_TEST_EXPECT(state,
                     sxml_xslt_ast_add_attribute(
                         ast,
                         &(sxml_xslt_ast_attribute_request){
                             .node = root,
                             .kind = SXML_XSLT_AST_ATTRIBUTE_ID,
                             .value = {.kind = SXML_XSLT_AST_VALUE_KIND_TEXT}},
                         &attribute) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_ast *pattern = NULL;
    SXML_TEST_REQUIRE(state, sxml_xslt_pattern_build_ast(
                                 &(sxml_ast_build_request){
                                     .expression_text = SXML_BYTE_VIEW_FROM_LITERAL("item"),
                                     .source_name = SXML_BYTE_VIEW_FROM_LITERAL("main.xsl"),
                                     .allocator = sxml_default_allocator()},
                                 &pattern) == SXML_STATUS_OK);
    sxml_xslt_ast_expression_index expression = {.value = 0};
    SXML_TEST_REQUIRE(state, sxml_xslt_ast_take_expression(
                                 ast,
                                 &(sxml_xslt_ast_expression_request){
                                     .kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                                     .expression_ast = pattern,
                                     .static_context = context,
                                     .node = template_node},
                                 &expression) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(
        state, sxml_xslt_ast_add_attribute(
                   ast,
                   &(sxml_xslt_ast_attribute_request){
                       .node = template_node,
                       .kind = SXML_XSLT_AST_ATTRIBUTE_PRIORITY,
                       .attribute_name = intern_name(&fixture, "priority"),
                       .value = {.kind = SXML_XSLT_AST_VALUE_KIND_NUMBER,
                                 .payload = {.number = -0.25}},
                       .source_span = {.line_number = 2, .column_number = 30}},
                   &attribute) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(
        state, sxml_xslt_ast_add_attribute(
                   ast,
                   &(sxml_xslt_ast_attribute_request){
                       .node = template_node,
                       .kind = SXML_XSLT_AST_ATTRIBUTE_MATCH,
                       .attribute_name = intern_name(&fixture, "match"),
                       .value = {.kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                                 .payload = {.expression = expression}},
                       .source_span = {.line_number = 2, .column_number = 17}},
                   &attribute) == SXML_STATUS_OK);
    const sxml_xslt_ast_name_token tokens[] = {
        {.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_NAME, .name = intern_name(&fixture, "list")},
        {.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT},
    };
    sxml_xslt_ast_name_token_list token_list = {.first_name_token_index = 0,
                                                .name_token_count = 0};
    SXML_TEST_REQUIRE(state, sxml_xslt_ast_add_name_tokens(
                                 ast,
                                 (sxml_xslt_ast_name_token_view){.name_tokens = tokens,
                                                                 .name_token_count = 2},
                                 &token_list) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(
        state, sxml_xslt_ast_add_attribute(
                   ast,
                   &(sxml_xslt_ast_attribute_request){
                       .node = template_node,
                       .kind = SXML_XSLT_AST_ATTRIBUTE_MODE,
                       .attribute_name = intern_name(&fixture, "mode"),
                       .value = {.kind = SXML_XSLT_AST_VALUE_KIND_NAME_LIST,
                                 .payload = {.name_tokens = token_list}}},
                   &attribute) == SXML_STATUS_OK);
    sxml_xslt_ast_node_index text_node = {.value = 0};
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_TEXT_NODE,
                                                      .module = 1,
                                                      .parent = template_node.value,
                                                      .static_context = 1,
                                                      .line_number = 3},
                                      &text_node) == SXML_STATUS_OK);
    sxml_xslt_ast_seal(ast);
    SXML_TEST_EXPECT(state, sxml_xslt_ast_find_attribute(
                                ast, (sxml_xslt_ast_attribute_query){
                                         .node = template_node,
                                         .kind = SXML_XSLT_AST_ATTRIBUTE_MATCH})
                                    .value == 3);
    SXML_TEST_EXPECT(state, sxml_xslt_ast_find_attribute(
                                ast, (sxml_xslt_ast_attribute_query){
                                         .node = template_node,
                                         .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT})
                                    .value == 0);
    static const char expected[] =
        "module 1 principal precedence 1 root 1\n"
        "  uri \"main.xsl\"\n"
        "context 1 parent 0 version 3.0 expand-text no\n"
        "  namespaces (xsl = http://www.w3.org/1999/XSL/Transform)\n"
        "  xpath-default-namespace none default-collation codepoint\n"
        "  default-mode #unnamed default-validation strip\n"
        "  excluded (http://www.w3.org/1999/XSL/Transform) extension ()\n"
        "  base-uri \"file:///main.xsl\"\n"
        "declarations (node 2)\n"
        "node 1 STYLESHEET module 1 context 1 at 1:3\n"
        "  attribute VERSION text \"3.0\" at 1:17\n"
        "  node 2 TEMPLATE declaration 1 at 2:3\n"
        "    attribute PRIORITY number -0.25 at 2:30\n"
        "    attribute MATCH pattern 1 at 2:17\n"
        "    attribute MODE names (list, #default)\n"
        "    node 3 TEXT_NODE at 3:3\n";
    char output[2048];
    size_t byte_count = 0;
    SXML_TEST_REQUIRE(state, sxml_xslt_ast_write_text(
                                 ast,
                                 (sxml_memory_view){.bytes = (uint8_t *)output,
                                                    .byte_count = sizeof(output)},
                                 &byte_count) == SXML_STATUS_OK);
    const bool matches = byte_count == sizeof(expected) - 1 &&
                         memcmp(output, expected, byte_count) == 0;
    if (!matches) {
        printf("got:\n%.*s\n", (int)byte_count, output);
    }
    SXML_TEST_EXPECT(state, matches);
    /* A short output holds a prefix and the count of the whole. */
    size_t short_count = 0;
    SXML_TEST_EXPECT(state, sxml_xslt_ast_write_text(
                                ast, (sxml_memory_view){.bytes = (uint8_t *)output, .byte_count = 8},
                                &short_count) == SXML_STATUS_CAPACITY_EXCEEDED);
    SXML_TEST_EXPECT(state, short_count == byte_count);
    fixture_close(&fixture);
}

/* Requests that break the rules are refused and change nothing. */
static void test_requests_are_checked(sxml_test_state *state) {
    test_fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    SXML_TEST_REQUIRE(state, fixture_open(&fixture, "<a/>"));
    sxml_xslt_ast_module_index module = {.value = 0};
    sxml_xslt_ast_node_index node = {.value = 0};
    /* The first module must be the principal one. */
    SXML_TEST_EXPECT(state,
                     add_module(&fixture,
                                (module_addition){.relation =
                                                      SXML_XSLT_AST_MODULE_RELATION_INCLUDED,
                                                  .uri = "x.xsl"},
                                &module) == SXML_STATUS_INVALID_ARGUMENT);
    /* A node needs a module. */
    SXML_TEST_EXPECT(state, add_node(&fixture,
                                     (node_addition){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                                     .module = 1},
                                     &node) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_REQUIRE(state,
                      add_module(&fixture,
                                 (module_addition){.relation =
                                                       SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL,
                                                   .uri = "x.xsl"},
                                 &module) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, add_node(&fixture,
                                     (node_addition){.kind = SXML_XSLT_AST_KIND_NONE, .module = 1},
                                     &node) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, add_node(&fixture,
                                     (node_addition){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                                     .module = 1,
                                                     .static_context = 4},
                                     &node) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                                      .module = 1},
                                      &node) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_xslt_ast_get_module(fixture.ast, module)->root.value == 1);
    /* A module request naming a node that is no xsl:include. */
    SXML_TEST_EXPECT(state,
                     add_module(&fixture,
                                (module_addition){.relation =
                                                      SXML_XSLT_AST_MODULE_RELATION_INCLUDED,
                                                  .parent_module = 1,
                                                  .referencing_node = 1,
                                                  .uri = "y.xsl"},
                                &module) == SXML_STATUS_INVALID_ARGUMENT);
    /* A child with a parent, or the parent itself. */
    sxml_xslt_ast_node_index child = {.value = 0};
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_TEMPLATE,
                                                      .module = 1,
                                                      .parent = 1},
                                      &child) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_xslt_ast_append_child(
                                fixture.ast, (sxml_xslt_ast_child_request){
                                                 .parent = node, .child = child}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_xslt_ast_append_child(
                         fixture.ast, (sxml_xslt_ast_child_request){.parent = node, .child = node}) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    /* Text extends only the text that ends the store. */
    sxml_xslt_ast_text first = {.byte_offset = 0, .byte_count = 0};
    sxml_xslt_ast_text second = {.byte_offset = 0, .byte_count = 0};
    SXML_TEST_REQUIRE(state, sxml_xslt_ast_append_text(fixture.ast,
                                                       SXML_BYTE_VIEW_FROM_LITERAL("ab"),
                                                       &first) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_xslt_ast_append_text(fixture.ast,
                                                       SXML_BYTE_VIEW_FROM_LITERAL("cd"),
                                                       &second) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_xslt_ast_append_text(fixture.ast, SXML_BYTE_VIEW_FROM_LITERAL("e"),
                                                      &first) == SXML_STATUS_INVALID_ARGUMENT);
    const sxml_byte_view second_bytes = sxml_xslt_ast_get_text(fixture.ast, second);
    SXML_TEST_EXPECT(state, second_bytes.byte_count == 2 &&
                                memcmp(second_bytes.bytes, "cd", 2) == 0);
    /* Accessors give the zero entries out of range. */
    SXML_TEST_EXPECT(state, sxml_xslt_ast_get_node(fixture.ast, (sxml_xslt_ast_node_index){
                                                                    .value = 99})
                                    ->kind == SXML_XSLT_AST_KIND_NONE);
    SXML_TEST_EXPECT(state, sxml_xslt_ast_get_expression(fixture.ast,
                                                         (sxml_xslt_ast_expression_index){
                                                             .value = 1})
                                    ->ast == NULL);
    fixture_close(&fixture);
}

/* The bounds of the configuration. */
static void test_limits(sxml_test_state *state) {
    const sxml_allocator allocator = sxml_default_allocator();
    test_fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    SXML_TEST_REQUIRE(state, fixture_open(&fixture, "<a/>"));
    sxml_xslt_ast_destroy(fixture.ast);
    fixture.ast = NULL;
    /* Caller memory, one module and two nodes plus attributes. */
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    const sxml_xslt_ast_configuration configuration = {
        .allocator = allocator,
        .item_heap = sxml_processor_get_item_heap(fixture.processor),
        .maximum_module_count = 1,
        .maximum_node_count = 2};
    SXML_TEST_REQUIRE(state, sxml_xslt_ast_memory_requirements(&configuration, &requirements) ==
                                 SXML_STATUS_OK);
    alignas(max_align_t) static uint8_t memory[sizeof(sxml_xslt_ast) + 64];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(memory));
    sxml_xslt_ast *ast = NULL;
    SXML_TEST_EXPECT(state, sxml_xslt_ast_initialize(
                                (sxml_memory_view){.bytes = memory, .byte_count = 8},
                                &configuration, &ast) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_REQUIRE(state, sxml_xslt_ast_initialize(
                                 (sxml_memory_view){.bytes = memory, .byte_count = sizeof(memory)},
                                 &configuration, &ast) == SXML_STATUS_OK);
    fixture.ast = ast;
    sxml_xslt_ast_module_index module = {.value = 0};
    sxml_xslt_ast_node_index node = {.value = 0};
    SXML_TEST_REQUIRE(state,
                      add_module(&fixture,
                                 (module_addition){.relation =
                                                       SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL,
                                                   .uri = "x.xsl"},
                                 &module) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                                      .module = 1},
                                      &node) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, add_node(&fixture,
                                      (node_addition){.kind = SXML_XSLT_AST_KIND_INCLUDE,
                                                      .module = 1,
                                                      .parent = 1},
                                      &node) == SXML_STATUS_OK);
    sxml_xslt_ast_attribute_index attribute = {.value = 0};
    SXML_TEST_EXPECT(state, sxml_xslt_ast_add_attribute(
                                ast,
                                &(sxml_xslt_ast_attribute_request){
                                    .node = node,
                                    .kind = SXML_XSLT_AST_ATTRIBUTE_HREF,
                                    .value = {.kind = SXML_XSLT_AST_VALUE_KIND_TEXT}},
                                &attribute) == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state,
                     add_module(&fixture,
                                (module_addition){.relation =
                                                      SXML_XSLT_AST_MODULE_RELATION_INCLUDED,
                                                  .parent_module = 1,
                                                  .referencing_node = 2,
                                                  .uri = "y.xsl"},
                                &module) == SXML_STATUS_LIMIT_EXCEEDED);
    sxml_xslt_ast_finalize(ast);
    fixture.ast = NULL;
    fixture_close(&fixture);
}

static const sxml_test_case xslt_ast_test_cases[] = {
    {.name = "import_precedence", .function = test_import_precedence},
    {.name = "text_form", .function = test_text_form},
    {.name = "requests_are_checked", .function = test_requests_are_checked},
    {.name = "limits", .function = test_limits},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_ast",
            .cases = xslt_ast_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_ast_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
