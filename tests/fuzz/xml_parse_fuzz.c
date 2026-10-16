/*
 * Fuzz target: the XML parser (include/sxml/xml.h; packet 1.26).
 *
 * The input is one document. It is parsed into an arena tree, whose nodes
 * are then read back, and checked without a builder; both must give the
 * same status, since sxml_xml_parser_check "checks request->input as
 * sxml_xml_parser_parse does". Limits bound the work: entity expansion
 * (the "billion laughs", XML 1.0 section 4.4) and the item heap; nesting
 * depth is unbounded on purpose, since deep input must cost heap, not C
 * stack (D71).
 *
 * Golden file: tests/xml_test.c (test_documents_build_arena_trees).
 */
#include "fuzz_fixture.h"
#include "sxml/tree.h"
#include "sxml/xml.h"

#include <stdio.h>
#include <stdlib.h>

/* Bytes entity replacement may produce in one document. */
#define SXML_FUZZ_MAXIMUM_ENTITY_EXPANSION_BYTE_COUNT ((size_t)1 << 20U)

/* Reads every node of tree back, as a consumer of the tree would. */
static void read_tree(const sxml_fuzz_fixture *fixture, sxml_tree *tree) {
    sxml_tree_description description = {.node_count = 0};
    sxml_fuzz_check_status(sxml_tree_describe(tree, &description),
                           "sxml_tree_describe");
    for (uint32_t node_number = 0; node_number < description.node_count;
         node_number++) {
        const sxml_node_reference node = {.tree = tree,
                                          .node_index = {.index = node_number},
                                          .namespace_number = 0};
        sxml_node_description node_description = {.node_kind =
                                                      SXML_NODE_KIND_DOCUMENT};
        sxml_fuzz_check_status(sxml_node_describe(node, &node_description),
                               "sxml_node_describe");
        if (node_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            sxml_namespace_binding_view bindings = {.namespace_binding_count = 0};
            sxml_fuzz_check_status(
                sxml_node_get_in_scope_namespaces(node, &bindings),
                "sxml_node_get_in_scope_namespaces");
        } else if (node_description.node_kind != SXML_NODE_KIND_DOCUMENT) {
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            sxml_fuzz_check_status(sxml_node_get_content(node, &content),
                                   "sxml_node_get_content");
        }
    }
    sxml_item string_value = {.representation = 0};
    const sxml_status status = sxml_node_make_string_value(
        fixture->item_heap,
        (sxml_node_reference){.tree = tree,
                              .node_index = {.index = 0},
                              .namespace_number = 0},
        &string_value);
    sxml_fuzz_check_status(status, "sxml_node_make_string_value");
    if (status == SXML_STATUS_OK) {
        sxml_item_release(string_value);
    }
}

/* Parses input into an arena tree and returns the parser's status. */
static sxml_status parse_into_tree(const sxml_fuzz_fixture *fixture,
                                   sxml_xml_parser *parser,
                                   sxml_byte_view input) {
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(fixture);
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        allocator,
        &(sxml_arena_tree_configuration){
            .allocator = allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = fixture->item_heap,
            .keeps_locations = true,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = false,
            .error_record = NULL},
        &arena_tree);
    sxml_fuzz_check_status(status, "sxml_arena_tree_create");
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_OK;
    }
    sxml_diagnostic diagnostic = {.description = {.byte_count = 0}};
    status = sxml_xml_parser_parse(
        parser,
        &(sxml_xml_parse_request){
            .input = input,
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("fuzz.xml"),
            .document_uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///fuzz.xml"),
            .base_uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///fuzz.xml"),
            .diagnostic = &diagnostic},
        sxml_arena_tree_as_tree_builder(arena_tree));
    sxml_fuzz_check_status(status, "sxml_xml_parser_parse");
    if (status == SXML_STATUS_OK) {
        sxml_tree *tree = NULL;
        const sxml_status finish_status =
            sxml_arena_tree_finish(arena_tree, &tree);
        sxml_fuzz_check_status(finish_status, "sxml_arena_tree_finish");
        if (finish_status == SXML_STATUS_OK) {
            read_tree(fixture, tree);
            sxml_tree_release(tree);
        }
    }
    sxml_arena_tree_destroy(arena_tree);
    return status;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(
            &(sxml_fuzz_fixture_configuration){.has_function_library = false},
            &fixture) != SXML_STATUS_OK) {
        return 0;
    }
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(&fixture);
    sxml_xml_parser *parser = NULL;
    sxml_status status = sxml_xml_parser_create(
        allocator,
        &(sxml_xml_parser_configuration){
            .allocator = allocator,
            .name_pool = fixture.name_pool,
            .limits = {.maximum_depth = 0,
                       .maximum_entity_expansion_byte_count =
                           SXML_FUZZ_MAXIMUM_ENTITY_EXPANSION_BYTE_COUNT}},
        &parser);
    sxml_fuzz_check_status(status, "sxml_xml_parser_create");
    if (status == SXML_STATUS_OK) {
        const sxml_byte_view input = sxml_fuzz_input_view(data, size);
        const sxml_status parse_status =
            parse_into_tree(&fixture, parser, input);
        sxml_diagnostic diagnostic = {.description = {.byte_count = 0}};
        const sxml_status check_status = sxml_xml_parser_check(
            parser, &(sxml_xml_parse_request){.input = input,
                                              .diagnostic = &diagnostic});
        sxml_fuzz_check_status(check_status, "sxml_xml_parser_check");
        /* The item heap's limit may stop building but not checking. */
        if (parse_status != check_status &&
            parse_status != SXML_STATUS_LIMIT_EXCEEDED &&
            parse_status != SXML_STATUS_OUT_OF_MEMORY) {
            (void)fprintf(stderr, "parse and check disagree: %d and %d\n",
                          (int)parse_status, (int)check_status);
            abort();
        }
    }
    sxml_xml_parser_destroy(parser);
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
