/*
 * Tests for the arena tree and its tree builder (include/sxml/tree.h;
 * D40, D67): lifecycle in caller and allocated memory, the event grammar,
 * text merging, duplicate attributes, namespace fixup, limits, locations,
 * unparsed entities, self-destruction and allocation failures.
 *
 * Imitates tests/memory_test.c: lifecycle checks, then behaviour, then
 * allocation-failure and limit loops (D23).
 */
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "tree_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

#define TEST_STORAGE_ALIGNMENT 16
#define TEST_STORAGE_BYTE_COUNT 4096
/* initial_memory for the tree without an allocator. */
#define TEST_INITIAL_BYTE_COUNT 16384
/* Attributes of one element, beyond the scan limit of the arena tree. */
#define TEST_ATTRIBUTE_COUNT 300
/* Children of the wide tree, enough for many node segments. */
#define TEST_CHILD_COUNT 40000

/* ------------------------------------------------------------ Fixture */

typedef struct tree_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} tree_fixture;

static sxml_status tree_fixture_create(tree_fixture *fixture) {
    *fixture = (tree_fixture){
        .name_pool = NULL, .registry = NULL, .item_heap = NULL};
    sxml_status status =
        sxml_name_pool_create(sxml_default_allocator(),
                              &(sxml_name_pool_configuration){
                                  .allocator = sxml_default_allocator(),
                                  .maximum_byte_count = 0},
                              &fixture->name_pool);
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_create(
            sxml_default_allocator(),
            &(sxml_type_registry_configuration){
                .allocator = sxml_default_allocator(),
                .name_pool = fixture->name_pool,
                .maximum_registered_type_count = 0},
            &fixture->registry);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_create(
            sxml_default_allocator(),
            &(sxml_item_heap_configuration){
                .allocator = sxml_default_allocator(),
                .name_pool = fixture->name_pool,
                .type_registry = fixture->registry,
                .maximum_byte_count = 0},
            &fixture->item_heap);
    }
    return status;
}

static void tree_fixture_destroy(tree_fixture *fixture) {
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

static sxml_arena_tree_configuration arena_configuration(
    const tree_fixture *fixture) {
    return (sxml_arena_tree_configuration){
        .allocator = sxml_default_allocator(),
        .initial_memory = {.bytes = NULL, .byte_count = 0},
        .item_heap = fixture->item_heap,
        .maximum_node_count = 0,
        .maximum_depth = 0,
        .maximum_text_byte_count = 0,
        .keeps_locations = false,
        .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
        .is_destroyed_when_unreferenced = false,
        .error_record = NULL,
    };
}

/* --------------------------------------------------------- Event writer */

/* Sends events to a builder, keeping the first failure. */
typedef struct event_writer {
    const tree_fixture *fixture;
    sxml_tree_builder builder;
    sxml_status status;
} event_writer;

/* A name as text: namespace URI ("" for none), prefix ("" for none),
   local name. */
typedef struct test_name {
    const char *namespace_uri;
    const char *prefix;
    const char *local_name;
} test_name;

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

static sxml_name_identifier intern_name(event_writer *writer,
                                        test_name name) {
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_name_identifier identifier = {.index = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_uri(
            writer->fixture->name_pool, text_view(name.namespace_uri),
            &namespace_uri);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_name(
            writer->fixture->name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = text_view(name.local_name)},
            &identifier);
    }
    return identifier;
}

static sxml_namespace_prefix_identifier intern_prefix(event_writer *writer,
                                                      const char *prefix) {
    sxml_namespace_prefix_identifier identifier = {.index = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_prefix(
            writer->fixture->name_pool, text_view(prefix), &identifier);
    }
    return identifier;
}

static const sxml_source_location no_location = {
    .source_name = {.bytes = NULL, .byte_count = 0},
    .line_number = 0,
    .column_number = 0,
    .byte_offset = 0};

static void write_start_document(event_writer *writer) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->start_document(
            writer->builder.state,
            &(sxml_document_start_event){
                .document_uri = text_view("urn:document"),
                .base_uri = text_view("http://example.com/"),
                .location = no_location});
    }
}

static void write_end_document(event_writer *writer) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status =
            writer->builder.operations->end_document(writer->builder.state);
    }
}

static void write_start_element(event_writer *writer, test_name name) {
    sxml_name_identifier element_name = intern_name(writer, name);
    sxml_namespace_prefix_identifier prefix =
        intern_prefix(writer, name.prefix);
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->start_element(
            writer->builder.state,
            &(sxml_element_start_event){
                .element_name = element_name,
                .namespace_prefix = prefix,
                .type_annotation = {.index = SXML_TYPE_NONE},
                .is_id = false,
                .is_idrefs = false,
                .is_nilled = false,
                .location = no_location});
    }
}

static void write_element(event_writer *writer, const char *local_name) {
    write_start_element(writer, (test_name){.namespace_uri = "",
                                            .prefix = "",
                                            .local_name = local_name});
}

static void write_end_element(event_writer *writer) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status =
            writer->builder.operations->end_element(writer->builder.state);
    }
}

static void write_namespace(event_writer *writer, const char *prefix,
                            const char *namespace_uri) {
    sxml_namespace_prefix_identifier prefix_identifier =
        intern_prefix(writer, prefix);
    sxml_namespace_uri_identifier uri_identifier = {.index = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_uri(
            writer->fixture->name_pool, text_view(namespace_uri),
            &uri_identifier);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->add_namespace_binding(
            writer->builder.state,
            &(sxml_namespace_binding){.namespace_prefix = prefix_identifier,
                                      .namespace_uri = uri_identifier});
    }
}

static void write_attribute(event_writer *writer, test_name name,
                            const char *value) {
    sxml_name_identifier attribute_name = intern_name(writer, name);
    sxml_namespace_prefix_identifier prefix =
        intern_prefix(writer, name.prefix);
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->add_attribute(
            writer->builder.state,
            &(sxml_attribute_event){
                .attribute_name = attribute_name,
                .namespace_prefix = prefix,
                .attribute_value = text_view(value),
                .type_annotation = {.index = SXML_TYPE_NONE},
                .is_id = false,
                .is_idrefs = false,
                .location = no_location});
    }
}

static void write_plain_attribute(event_writer *writer,
                                  const char *local_name, const char *value) {
    write_attribute(writer,
                    (test_name){.namespace_uri = "",
                                .prefix = "",
                                .local_name = local_name},
                    value);
}

static void write_text(event_writer *writer, const char *text) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->add_text(
            writer->builder.state,
            &(sxml_text_event){.text = text_view(text),
                               .location = no_location});
    }
}

static void write_comment(event_writer *writer, const char *comment) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->add_comment(
            writer->builder.state,
            &(sxml_comment_event){.comment = text_view(comment),
                                  .location = no_location});
    }
}

static void write_processing_instruction(event_writer *writer,
                                         const char *target,
                                         const char *data) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->add_processing_instruction(
            writer->builder.state,
            &(sxml_processing_instruction_event){.target = text_view(target),
                                                 .data = text_view(data),
                                                 .location = no_location});
    }
}

/* ------------------------------------------------------------ Helpers */

/* An arena tree and, once finished, its tree. */
typedef struct built_tree {
    sxml_arena_tree *arena_tree;
    sxml_tree *tree;
} built_tree;

static void built_tree_destroy(built_tree *built) {
    sxml_tree_release(built->tree);
    sxml_arena_tree_destroy(built->arena_tree);
}

static sxml_status built_tree_create(
    const sxml_arena_tree_configuration *configuration, built_tree *built) {
    *built = (built_tree){.arena_tree = NULL, .tree = NULL};
    return sxml_arena_tree_create(sxml_default_allocator(), configuration,
                                  &built->arena_tree);
}

static event_writer writer_for(const tree_fixture *fixture,
                               const built_tree *built) {
    return (event_writer){
        .fixture = fixture,
        .builder = sxml_arena_tree_as_tree_builder(built->arena_tree),
        .status = SXML_STATUS_OK};
}

static sxml_node_reference node_of(const built_tree *built,
                                   uint32_t node_number) {
    return (sxml_node_reference){.tree = built->tree,
                                 .node_index = {.index = node_number},
                                 .namespace_number = 0};
}

static sxml_node_description describe(const built_tree *built,
                                      uint32_t node_number) {
    sxml_node_description description = {.node_kind = SXML_NODE_KIND_ANY};
    if (sxml_node_describe(node_of(built, node_number), &description) !=
        SXML_STATUS_OK) {
        description.node_kind = SXML_NODE_KIND_ANY;
    }
    return description;
}

static bool content_is(const built_tree *built, uint32_t node_number,
                       const char *expected) {
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    return sxml_node_get_content(node_of(built, node_number), &content) ==
               SXML_STATUS_OK &&
           sxml_byte_view_equals(content, text_view(expected));
}

static bool prefix_is(const tree_fixture *fixture,
                      sxml_namespace_prefix_identifier prefix,
                      const char *expected) {
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    return sxml_name_pool_get_namespace_prefix(fixture->name_pool, prefix,
                                               &text) == SXML_STATUS_OK &&
           sxml_byte_view_equals(text, text_view(expected));
}

/* A prefix and the URI it should be bound to on an element. */
typedef struct expected_binding {
    const char *prefix;
    /* NULL: the prefix must not be bound. */
    const char *namespace_uri;
} expected_binding;

static bool element_binds(const tree_fixture *fixture,
                          sxml_node_reference element,
                          expected_binding expected) {
    sxml_namespace_binding_view bindings;
    if (sxml_node_get_in_scope_namespaces(element, &bindings) !=
        SXML_STATUS_OK) {
        return false;
    }
    for (size_t binding_index = 0;
         binding_index < bindings.namespace_binding_count; binding_index++) {
        sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
        if (!prefix_is(fixture, binding.namespace_prefix, expected.prefix)) {
            continue;
        }
        sxml_byte_view namespace_uri;
        return expected.namespace_uri != NULL &&
               sxml_name_pool_get_namespace_uri(
                   fixture->name_pool, binding.namespace_uri, &namespace_uri) ==
                   SXML_STATUS_OK &&
               sxml_byte_view_equals(namespace_uri,
                                     text_view(expected.namespace_uri));
    }
    return expected.namespace_uri == NULL;
}

/* ------------------------------------------------------------ Lifecycle */

static void test_arena_tree_rejects_invalid_configuration(
    sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    sxml_memory_requirements requirements = {.byte_count = 3, .alignment = 1};
    configuration.item_heap = NULL;
    SXML_TEST_EXPECT(state, sxml_arena_tree_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    configuration = arena_configuration(&fixture);
    configuration.initial_memory =
        (sxml_memory_view){.bytes = NULL, .byte_count = 16};
    SXML_TEST_EXPECT(state, sxml_arena_tree_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, requirements.byte_count == 3);
    /* Self-destruction needs memory the arena tree can give back. */
    configuration = arena_configuration(&fixture);
    configuration.is_destroyed_when_unreferenced = true;
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[TEST_STORAGE_BYTE_COUNT];
    sxml_arena_tree *arena_tree = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_arena_tree_initialize(
                         (sxml_memory_view){.bytes = storage,
                                            .byte_count = sizeof(storage)},
                         &configuration,
                         &arena_tree) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, arena_tree == NULL);
    tree_fixture_destroy(&fixture);
}

static void test_arena_tree_initializes_in_caller_memory(
    sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    sxml_memory_requirements requirements;
    SXML_TEST_REQUIRE(state, sxml_arena_tree_memory_requirements(
                                 &configuration, &requirements) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      requirements.byte_count <= TEST_STORAGE_BYTE_COUNT);
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[TEST_STORAGE_BYTE_COUNT];
    sxml_arena_tree *arena_tree = NULL;
    SXML_TEST_EXPECT(state, sxml_arena_tree_initialize(
                                (sxml_memory_view){
                                    .bytes = storage,
                                    .byte_count = requirements.byte_count - 1},
                                &configuration, &arena_tree) ==
                                SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, sxml_arena_tree_initialize(
                                (sxml_memory_view){
                                    .bytes = storage + 1,
                                    .byte_count = requirements.byte_count},
                                &configuration, &arena_tree) ==
                                SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_initialize(
                                 (sxml_memory_view){
                                     .bytes = storage,
                                     .byte_count = requirements.byte_count},
                                 &configuration,
                                 &arena_tree) == SXML_STATUS_OK);
    built_tree built = {.arena_tree = arena_tree, .tree = NULL};
    event_writer writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    write_end_element(&writer);
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(arena_tree, &built.tree) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, describe(&built, 0).node_kind ==
                                SXML_NODE_KIND_ELEMENT);
    sxml_tree_release(built.tree);
    sxml_arena_tree_finalize(arena_tree);
    sxml_arena_tree_finalize(NULL);
    sxml_arena_tree_destroy(NULL);
    tree_fixture_destroy(&fixture);
}

/* With the null allocator, the tree lives in initial_memory alone. */
static void test_arena_tree_works_in_initial_memory(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    alignas(TEST_STORAGE_ALIGNMENT) static uint8_t initial[TEST_INITIAL_BYTE_COUNT];
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    configuration.allocator = (sxml_allocator){.state = NULL, .operations = NULL};
    configuration.initial_memory =
        (sxml_memory_view){.bytes = initial, .byte_count = TEST_INITIAL_BYTE_COUNT};
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    write_plain_attribute(&writer, "name", "value");
    write_text(&writer, "small");
    write_end_element(&writer);
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                    &built.tree) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, content_is(&built, 2, "small"));
    built_tree_destroy(&built);
    /* Growth beyond initial_memory fails without an allocator. */
    configuration.initial_memory.byte_count = 64;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_CAPACITY_EXCEEDED);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* ----------------------------------------------------------- Building */

static void test_adjacent_text_merges_and_empty_text_vanishes(
    sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_start_document(&writer);
    write_element(&writer, "root");
    write_text(&writer, "");
    write_element(&writer, "empty");
    write_end_element(&writer);
    write_text(&writer, "ab");
    write_text(&writer, "");
    write_text(&writer, "cd");
    write_comment(&writer, "between");
    write_text(&writer, "ef");
    write_end_element(&writer);
    write_end_document(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                    &built.tree) ==
                                 SXML_STATUS_OK);
    /* XDM 4.0, section 7.5.7.1: #doc, root, empty, "abcd", comment, "ef". */
    SXML_TEST_EXPECT(state, sxml_tree_get_node_count(built.tree) == 6);
    SXML_TEST_EXPECT(state, describe(&built, 3).node_kind ==
                                SXML_NODE_KIND_TEXT);
    SXML_TEST_EXPECT(state, content_is(&built, 3, "abcd"));
    SXML_TEST_EXPECT(state, content_is(&built, 4, "between"));
    SXML_TEST_EXPECT(state, content_is(&built, 5, "ef"));
    sxml_node_description root = describe(&built, 1);
    SXML_TEST_EXPECT(state, root.first_child.index == 2 &&
                                root.last_child.index == 5 &&
                                root.last_descendant.index == 5);
    sxml_node_description text = describe(&built, 3);
    SXML_TEST_EXPECT(state, text.has_previous_sibling &&
                                text.previous_sibling.index == 2 &&
                                text.has_next_sibling &&
                                text.next_sibling.index == 4);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* The root may be any node kind (XDM 4.0, section 6: parentless nodes). */
static void test_parentless_nodes_are_roots(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    for (int root_kind = 0; root_kind < 5; root_kind++) {
        built_tree built;
        SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                     SXML_STATUS_OK);
        event_writer writer = writer_for(&fixture, &built);
        sxml_node_kind expected_kind = SXML_NODE_KIND_TEXT;
        switch (root_kind) {
        case 0:
            write_text(&writer, "one ");
            write_text(&writer, "text");
            break;
        case 1:
            write_comment(&writer, "one");
            expected_kind = SXML_NODE_KIND_COMMENT;
            break;
        case 2:
            write_plain_attribute(&writer, "alone", "one");
            expected_kind = SXML_NODE_KIND_ATTRIBUTE;
            break;
        case 3:
            write_processing_instruction(&writer, "target", "one");
            expected_kind = SXML_NODE_KIND_PROCESSING_INSTRUCTION;
            break;
        default:
            write_element(&writer, "one");
            write_end_element(&writer);
            expected_kind = SXML_NODE_KIND_ELEMENT;
            break;
        }
        SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
        /* Nothing may follow a complete parentless root. */
        write_comment(&writer, "second root");
        SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                        &built.tree) ==
                                     SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_tree_get_node_count(built.tree) == 1);
        sxml_node_description root = describe(&built, 0);
        SXML_TEST_EXPECT(state, root.node_kind == expected_kind);
        SXML_TEST_EXPECT(state, !root.has_parent);
        if (root_kind == 0) {
            SXML_TEST_EXPECT(state, content_is(&built, 0, "one text"));
        }
        built_tree_destroy(&built);
    }
    /* Only empty text makes a parentless zero-length text node (XDM 4.0,
       section 7.5.7.1; D163). */
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_text(&writer, "");
    write_text(&writer, "");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_arena_tree_finish(built.arena_tree,
                                                   &built.tree) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, built.tree != NULL);
    sxml_node_description empty_text;
    const sxml_node_reference root = {
        .tree = built.tree, .node_index = {.index = 0}, .namespace_number = 0};
    SXML_TEST_EXPECT(state, sxml_node_describe(root, &empty_text) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, empty_text.node_kind == SXML_NODE_KIND_TEXT &&
                                !empty_text.has_parent);
    sxml_byte_view content = {.bytes = NULL, .byte_count = 1};
    SXML_TEST_EXPECT(state, sxml_node_get_content(root, &content) == SXML_STATUS_OK &&
                                content.byte_count == 0);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* A parentless root takes the configured base URI (D98); a document keeps
   the one of its start event. */
/* A text node made of events of which one asks for CDATA sections keeps
   the request (glossary cdata_section; D163); others do not have it. */
static void test_text_keeps_cdata_requests(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) == SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_element(&writer, "a");
    write_text(&writer, "plain ");
    if (writer.status == SXML_STATUS_OK) {
        writer.status = writer.builder.operations->add_text(
            writer.builder.state, &(sxml_text_event){.text = text_view("<cdata>"),
                                                     .location = no_location,
                                                     .is_cdata_section = true});
    }
    write_element(&writer, "b");
    write_text(&writer, "other");
    write_end_element(&writer);
    write_end_element(&writer);
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_arena_tree_finish(built.arena_tree, &built.tree) == SXML_STATUS_OK);
    /* a, its text, b, b's text. */
    const bool expected[4] = {false, true, false, false};
    for (uint32_t node_index = 0; node_index < 4; node_index += 1) {
        sxml_node_description description;
        const sxml_node_reference node = {
            .tree = built.tree, .node_index = {.index = node_index}, .namespace_number = 0};
        SXML_TEST_EXPECT(state, sxml_node_describe(node, &description) == SXML_STATUS_OK &&
                                    description.is_cdata_section == expected[node_index]);
    }
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* Text whose output escaping is disabled is a text node of its own
   inside a parent (the property belongs to each character, XSLT 4.0,
   section 26.1.8; D188), and copies keep the mark. */
static void test_text_keeps_escaping_marks_apart(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) == SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_element(&writer, "a");
    write_text(&writer, "<");
    for (size_t event_index = 0; event_index < 2 && writer.status == SXML_STATUS_OK;
         event_index += 1) {
        writer.status = writer.builder.operations->add_text(
            writer.builder.state, &(sxml_text_event){.text = text_view("&lt;"),
                                                     .location = no_location,
                                                     .is_cdata_section = false,
                                                     .disables_output_escaping = true});
    }
    write_text(&writer, "!");
    write_end_element(&writer);
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_arena_tree_finish(built.arena_tree, &built.tree) == SXML_STATUS_OK);
    /* a, "<", "&lt;&lt;" (one node of two events), "!". */
    const bool expected[4] = {false, false, true, false};
    const char *const contents[4] = {"", "<", "&lt;&lt;", "!"};
    for (uint32_t node_index = 1; node_index < 4; node_index += 1) {
        sxml_node_description description;
        const sxml_node_reference node = {
            .tree = built.tree, .node_index = {.index = node_index}, .namespace_number = 0};
        sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
        SXML_TEST_EXPECT(state, sxml_node_describe(node, &description) == SXML_STATUS_OK &&
                                    description.node_kind == SXML_NODE_KIND_TEXT &&
                                    description.disables_output_escaping ==
                                        expected[node_index]);
        SXML_TEST_EXPECT(state,
                         sxml_node_get_content(node, &content) == SXML_STATUS_OK &&
                             sxml_byte_view_equals(content, text_view(contents[node_index])));
    }
    built_tree copy;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &copy) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state,
        sxml_tree_builder_copy_node(
            sxml_arena_tree_as_tree_builder(copy.arena_tree),
            &(sxml_copy_request){
                .node = {.tree = built.tree, .node_index = {.index = 2}, .namespace_number = 0},
                .preserves_namespaces = true,
                .inherits_namespaces = true,
                .preserves_type_annotations = true}) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_arena_tree_finish(copy.arena_tree, &copy.tree) == SXML_STATUS_OK);
    sxml_node_description copied;
    SXML_TEST_EXPECT(
        state,
        sxml_node_describe((sxml_node_reference){.tree = copy.tree,
                                                 .node_index = {.index = 0},
                                                 .namespace_number = 0},
                           &copied) == SXML_STATUS_OK &&
            copied.disables_output_escaping);
    built_tree_destroy(&copy);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

static void test_roots_take_the_configured_base_uri(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    configuration.base_uri = text_view("http://example.com/static/");
    for (size_t root_index = 0; root_index < 2; root_index += 1) {
        built_tree built;
        SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                     SXML_STATUS_OK);
        event_writer writer = writer_for(&fixture, &built);
        if (root_index == 0) {
            write_element(&writer, "constructed");
            write_end_element(&writer);
        } else {
            write_start_document(&writer);
            write_element(&writer, "parsed");
            write_end_element(&writer);
            write_end_document(&writer);
        }
        SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
        SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                        &built.tree) ==
                                     SXML_STATUS_OK);
        sxml_tree_description description;
        SXML_TEST_REQUIRE(state, sxml_tree_describe(built.tree, &description) ==
                                     SXML_STATUS_OK);
        SXML_TEST_EXPECT(
            state, sxml_byte_view_equals(
                       description.base_uri,
                       text_view(root_index == 0 ? "http://example.com/static/"
                                                 : "http://example.com/")));
        built_tree_destroy(&built);
    }
    tree_fixture_destroy(&fixture);
}

/* A namespace binding sent first makes a parentless namespace node, the
   tree's root (D98); copying it gives the element being built a binding. */
static void test_parentless_namespace_nodes_are_stored(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_error_record error_record = {.value = NULL};
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    configuration.error_record = &error_record;
    built_tree namespace_tree;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &namespace_tree) ==
                                 SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &namespace_tree);
    write_namespace(&writer, "p", "urn:p");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
    write_comment(&writer, "second root");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(namespace_tree.arena_tree,
                                                    &namespace_tree.tree) ==
                                 SXML_STATUS_OK);
    sxml_node_description root = describe(&namespace_tree, 0);
    SXML_TEST_EXPECT(state, root.node_kind == SXML_NODE_KIND_NAMESPACE);
    SXML_TEST_EXPECT(state, !root.has_parent);
    SXML_TEST_EXPECT(state, content_is(&namespace_tree, 0, "urn:p"));
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(fixture.name_pool,
                                                          root.node_name,
                                                          &parts) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(parts.local_name, text_view("p")));

    built_tree element_tree;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &element_tree) ==
                                 SXML_STATUS_OK);
    writer = writer_for(&fixture, &element_tree);
    write_element(&writer, "holder");
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_tree_builder_copy_node(
                                writer.builder,
                                &(sxml_copy_request){
                                    .node = node_of(&namespace_tree, 0),
                                    .preserves_namespaces = true,
                                    .inherits_namespaces = true,
                                    .preserves_type_annotations = true}) ==
                                SXML_STATUS_OK);
    write_end_element(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(element_tree.arena_tree,
                                                    &element_tree.tree) ==
                                 SXML_STATUS_OK);
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    SXML_TEST_REQUIRE(state, sxml_node_get_in_scope_namespaces(
                                 node_of(&element_tree, 0), &bindings) ==
                                 SXML_STATUS_OK);
    bool has_p = false;
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        has_p = has_p || prefix_is(&fixture,
                                   bindings.namespace_bindings[binding_index]
                                       .namespace_prefix,
                                   "p");
    }
    SXML_TEST_EXPECT(state, has_p);
    built_tree_destroy(&element_tree);
    built_tree_destroy(&namespace_tree);

    /* An empty namespace URI cannot make a namespace node (XQDY0101). */
    built_tree empty_tree;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &empty_tree) ==
                                 SXML_STATUS_OK);
    writer = writer_for(&fixture, &empty_tree);
    write_namespace(&writer, "p", "");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name,
                                                  text_view("XQDY0101")));
    built_tree_destroy(&empty_tree);
    tree_fixture_destroy(&fixture);
}

static void test_events_out_of_order_are_rejected(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_arena_tree_finish(built.arena_tree,
                                                   &built.tree) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    event_writer writer = writer_for(&fixture, &built);
    write_end_element(&writer);
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_INVALID_ARGUMENT);
    writer = writer_for(&fixture, &built);
    write_end_document(&writer);
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_INVALID_ARGUMENT);
    writer = writer_for(&fixture, &built);
    write_start_document(&writer);
    write_plain_attribute(&writer, "outside", "element");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_INVALID_ARGUMENT);
    writer = writer_for(&fixture, &built);
    write_namespace(&writer, "p", "urn:p");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_INVALID_ARGUMENT);
    writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    write_text(&writer, "content");
    write_plain_attribute(&writer, "late", "attribute");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_INVALID_ARGUMENT);
    writer = writer_for(&fixture, &built);
    write_end_document(&writer);
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_arena_tree_finish(built.arena_tree,
                                                   &built.tree) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    writer = writer_for(&fixture, &built);
    write_end_element(&writer);
    write_end_document(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                    &built.tree) ==
                                 SXML_STATUS_OK);
    /* After finish the builder takes nothing more. */
    writer = writer_for(&fixture, &built);
    write_text(&writer, "after");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_INVALID_ARGUMENT);
    sxml_tree *second = NULL;
    SXML_TEST_EXPECT(state, sxml_arena_tree_finish(built.arena_tree, &second) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, second == NULL);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* ------------------------------------------------------- Attributes */

static void test_duplicate_attributes_follow_the_policy(
    sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_error_record error_record = {.value = NULL};
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    configuration.error_record = &error_record;
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    write_plain_attribute(&writer, "first", "1");
    write_plain_attribute(&writer, "first", "2");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name,
                                                  text_view("XQDY0025")));
    built_tree_destroy(&built);
    /* REPLACE keeps the first position and takes the later value (XSLT
       4.0, section 5.7.1). */
    configuration.duplicate_attribute_policy =
        SXML_DUPLICATE_ATTRIBUTE_POLICY_REPLACE;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    write_plain_attribute(&writer, "first", "1");
    write_plain_attribute(&writer, "second", "2");
    write_plain_attribute(&writer, "first", "3");
    write_end_element(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                    &built.tree) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, describe(&built, 0).attribute_count == 2);
    SXML_TEST_EXPECT(state, content_is(&built, 1, "3"));
    SXML_TEST_EXPECT(state, content_is(&built, 2, "2"));
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* Many attributes switch the duplicate check to a hash index. */
static void test_many_attributes_are_checked_by_index(
    sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    for (int policy_number = 0; policy_number < 2; policy_number++) {
        sxml_arena_tree_configuration configuration =
            arena_configuration(&fixture);
        configuration.duplicate_attribute_policy =
            policy_number == 0 ? SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT
                               : SXML_DUPLICATE_ATTRIBUTE_POLICY_REPLACE;
        built_tree built;
        SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                     SXML_STATUS_OK);
        event_writer writer = writer_for(&fixture, &built);
        write_element(&writer, "root");
        char name[16];
        for (int attribute_number = 0; attribute_number < TEST_ATTRIBUTE_COUNT;
             attribute_number++) {
            (void)snprintf(name, sizeof(name), "a%d", attribute_number);
            write_plain_attribute(&writer, name, "v");
        }
        SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
        write_plain_attribute(&writer, "a150", "again");
        if (policy_number == 0) {
            SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_DYNAMIC_ERROR);
        } else {
            write_end_element(&writer);
            SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
            SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                            &built.tree) ==
                                         SXML_STATUS_OK);
            SXML_TEST_EXPECT(state, describe(&built, 0).attribute_count ==
                                        TEST_ATTRIBUTE_COUNT);
            SXML_TEST_EXPECT(state, content_is(&built, 151, "again"));
        }
        built_tree_destroy(&built);
    }
    tree_fixture_destroy(&fixture);
}

/* ---------------------------------------------------- Namespace fixup */

static void test_namespace_fixup_binds_name_prefixes(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    /* 0: <p:root> with p unbound: fixup binds p (XSLT 4.0, 5.7.3). */
    write_start_element(&writer, (test_name){.namespace_uri = "urn:p",
                                             .prefix = "p",
                                             .local_name = "root"});
    write_namespace(&writer, "", "urn:default");
    /* 1: an attribute in a namespace without a prefix gets one. */
    write_attribute(&writer,
                    (test_name){.namespace_uri = "urn:q",
                                .prefix = "",
                                .local_name = "unprefixed"},
                    "1");
    /* 2: an attribute whose prefix is the element's, for another URI. */
    write_attribute(&writer,
                    (test_name){.namespace_uri = "urn:other",
                                .prefix = "p",
                                .local_name = "clash"},
                    "2");
    /* 3: xml:lang needs no binding. */
    write_attribute(&writer,
                    (test_name){.namespace_uri =
                                    "http://www.w3.org/XML/1998/namespace",
                                .prefix = "xml",
                                .local_name = "lang"},
                    "en");
    write_namespace(&writer, "xml", "http://www.w3.org/XML/1998/namespace");
    /* 4: a child in no namespace undeclares the default namespace. */
    write_element(&writer, "plain");
    /* 5: a grandchild that declares nothing shares 4's list. */
    write_element(&writer, "inner");
    write_end_element(&writer);
    write_end_element(&writer);
    /* 6: a child redeclaring an inherited prefix for its own name. */
    write_start_element(&writer, (test_name){.namespace_uri = "urn:new",
                                             .prefix = "p",
                                             .local_name = "rebound"});
    write_end_element(&writer);
    write_end_element(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                    &built.tree) ==
                                 SXML_STATUS_OK);
    sxml_node_reference root = node_of(&built, 0);
    SXML_TEST_EXPECT(state, prefix_is(&fixture, describe(&built, 0)
                                                    .namespace_prefix,
                                      "p"));
    SXML_TEST_EXPECT(state, element_binds(&fixture, root,
                                          (expected_binding){
                                              .prefix = "p",
                                              .namespace_uri = "urn:p"}));
    SXML_TEST_EXPECT(state, element_binds(&fixture, root,
                                          (expected_binding){
                                              .prefix = "",
                                              .namespace_uri = "urn:default"}));
    SXML_TEST_EXPECT(state, element_binds(&fixture, root,
                                          (expected_binding){
                                              .prefix = "xml",
                                              .namespace_uri =
                                                  "http://www.w3.org/XML/1998/"
                                                  "namespace"}));
    SXML_TEST_EXPECT(state, prefix_is(&fixture, describe(&built, 1)
                                                    .namespace_prefix,
                                      "ns0"));
    SXML_TEST_EXPECT(state, element_binds(&fixture, root,
                                          (expected_binding){
                                              .prefix = "ns0",
                                              .namespace_uri = "urn:q"}));
    SXML_TEST_EXPECT(state, prefix_is(&fixture, describe(&built, 2)
                                                    .namespace_prefix,
                                      "ns1"));
    SXML_TEST_EXPECT(state, element_binds(&fixture, root,
                                          (expected_binding){
                                              .prefix = "ns1",
                                              .namespace_uri = "urn:other"}));
    SXML_TEST_EXPECT(state, prefix_is(&fixture, describe(&built, 3)
                                                    .namespace_prefix,
                                      "xml"));
    sxml_node_reference plain = node_of(&built, 4);
    SXML_TEST_EXPECT(state, describe(&built, 4).node_kind ==
                                SXML_NODE_KIND_ELEMENT);
    SXML_TEST_EXPECT(state, element_binds(&fixture, plain,
                                          (expected_binding){
                                              .prefix = "",
                                              .namespace_uri = NULL}));
    SXML_TEST_EXPECT(state, element_binds(&fixture, plain,
                                          (expected_binding){
                                              .prefix = "p",
                                              .namespace_uri = "urn:p"}));
    sxml_namespace_binding_view plain_bindings;
    sxml_namespace_binding_view inner_bindings;
    SXML_TEST_REQUIRE(state, sxml_node_get_in_scope_namespaces(
                                 plain, &plain_bindings) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_node_get_in_scope_namespaces(
                                 node_of(&built, 5), &inner_bindings) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, plain_bindings.namespace_bindings ==
                                inner_bindings.namespace_bindings);
    SXML_TEST_EXPECT(state, element_binds(&fixture, node_of(&built, 6),
                                          (expected_binding){
                                              .prefix = "p",
                                              .namespace_uri = "urn:new"}));
    SXML_TEST_EXPECT(state, prefix_is(&fixture, describe(&built, 6)
                                                    .namespace_prefix,
                                      "p"));
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

static void test_conflicting_bindings_raise_errors(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_error_record error_record = {.value = NULL};
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    configuration.error_record = &error_record;
    typedef struct conflict_case {
        const char *prefix;
        const char *namespace_uri;
        const char *second_uri;
        const char *code;
    } conflict_case;
    static const conflict_case conflict_cases[] = {
        {"p", "urn:one", "urn:two", "XQDY0102"},
        {"xmlns", "urn:one", NULL, "XQDY0101"},
        {"xml", "urn:one", NULL, "XQDY0101"},
        {"other", "http://www.w3.org/XML/1998/namespace", NULL, "XQDY0101"},
        {"p", "http://www.w3.org/2000/xmlns/", NULL, "XQDY0101"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(conflict_cases);
         case_index++) {
        const conflict_case *conflict = &conflict_cases[case_index];
        built_tree built;
        SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                     SXML_STATUS_OK);
        event_writer writer = writer_for(&fixture, &built);
        write_element(&writer, "root");
        write_namespace(&writer, conflict->prefix, conflict->namespace_uri);
        if (conflict->second_uri != NULL) {
            SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
            /* The same binding twice is fine. */
            write_namespace(&writer, conflict->prefix, conflict->namespace_uri);
            SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
            write_namespace(&writer, conflict->prefix, conflict->second_uri);
        }
        SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_DYNAMIC_ERROR);
        SXML_TEST_EXPECT(state,
                         sxml_byte_view_equals(error_record.code.local_name,
                                               text_view(conflict->code)));
        built_tree_destroy(&built);
    }
    /* An element in no namespace that declares a default namespace. */
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    write_namespace(&writer, "", "urn:default");
    write_end_element(&writer);
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name,
                                                  text_view("XQDY0102")));
    built_tree_destroy(&built);
    /* An attribute in the xmlns namespace (XQDY0044). */
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    write_attribute(&writer,
                    (test_name){.namespace_uri = "http://www.w3.org/2000/xmlns/",
                                .prefix = "xmlns",
                                .local_name = "p"},
                    "urn:p");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name,
                                                  text_view("XQDY0044")));
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* ---------------------------------------------------------- Limits */

static void test_limits_stop_untrusted_input(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    configuration.maximum_node_count = 3;
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    write_element(&writer, "one");
    write_end_element(&writer);
    write_comment(&writer, "two");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
    write_text(&writer, "three");
    write_end_element(&writer);
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_LIMIT_EXCEEDED);
    built_tree_destroy(&built);

    configuration = arena_configuration(&fixture);
    configuration.maximum_depth = 2;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    writer = writer_for(&fixture, &built);
    write_start_document(&writer);
    write_element(&writer, "one");
    write_element(&writer, "two");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
    write_element(&writer, "three");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_LIMIT_EXCEEDED);
    built_tree_destroy(&built);

    configuration = arena_configuration(&fixture);
    configuration.maximum_text_byte_count = 10;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    writer = writer_for(&fixture, &built);
    write_element(&writer, "root");
    write_plain_attribute(&writer, "name", "12345");
    write_text(&writer, "12345");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_OK);
    write_text(&writer, "6");
    SXML_TEST_EXPECT(state, writer.status == SXML_STATUS_LIMIT_EXCEEDED);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* ---------------------------------------------- Locations and entities */

static void test_locations_are_kept_on_request(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    configuration.keeps_locations = true;
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    char source_name[] = "input.xml";
    sxml_tree_builder builder = sxml_arena_tree_as_tree_builder(built.arena_tree);
    event_writer writer = writer_for(&fixture, &built);
    sxml_name_identifier element_name =
        intern_name(&writer, (test_name){.namespace_uri = "",
                                         .prefix = "",
                                         .local_name = "located"});
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, builder.operations->start_element(
                                 builder.state,
                                 &(sxml_element_start_event){
                                     .element_name = element_name,
                                     .namespace_prefix = {.index = 0},
                                     .type_annotation = {.index = 0},
                                     .location = {.source_name = text_view(
                                                      source_name),
                                                  .line_number = 3,
                                                  .column_number = 5,
                                                  .byte_offset = 40}}) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, builder.operations->add_text(
                                 builder.state,
                                 &(sxml_text_event){
                                     .text = text_view("located text"),
                                     .location = {.source_name = text_view(
                                                      source_name),
                                                  .line_number = 3,
                                                  .column_number = 15,
                                                  .byte_offset = 50}}) ==
                                 SXML_STATUS_OK);
    /* Event views are borrowed for the call only. */
    source_name[0] = 'X';
    SXML_TEST_REQUIRE(state, builder.operations->end_element(builder.state) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                    &built.tree) ==
                                 SXML_STATUS_OK);
    sxml_source_location location = {.line_number = 0};
    SXML_TEST_REQUIRE(state, built.tree->operations->get_location(
                                 built.tree->state, (sxml_node_index){.index = 1},
                                 &location) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, location.line_number == 3 &&
                                location.column_number == 15 &&
                                location.byte_offset == 50);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(location.source_name,
                                                  text_view("input.xml")));
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

static void test_unparsed_entities_are_found(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_start_document(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    sxml_tree_builder builder = writer.builder;
    SXML_TEST_EXPECT(state, builder.operations->add_unparsed_entity(
                                builder.state,
                                &(sxml_unparsed_entity){
                                    .entity_name = text_view("picture"),
                                    .system_identifier = text_view("pic.gif"),
                                    .public_identifier = text_view("")}) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, builder.operations->add_unparsed_entity(
                                builder.state,
                                &(sxml_unparsed_entity){
                                    .entity_name = text_view("sound"),
                                    .system_identifier = text_view("sound.wav"),
                                    .public_identifier = text_view("-//sound")}) ==
                                SXML_STATUS_OK);
    write_element(&writer, "root");
    write_end_element(&writer);
    /* Entities come before the content. */
    SXML_TEST_EXPECT(state, builder.operations->add_unparsed_entity(
                                builder.state,
                                &(sxml_unparsed_entity){
                                    .entity_name = text_view("late"),
                                    .system_identifier = text_view("late"),
                                    .public_identifier = text_view("")}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    write_end_document(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree,
                                                    &built.tree) ==
                                 SXML_STATUS_OK);
    sxml_unparsed_entity entity = {.entity_name = text_view("picture")};
    SXML_TEST_REQUIRE(state, built.tree->operations->find_unparsed_entity(
                                 built.tree->state, &entity) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(entity.system_identifier,
                                                  text_view("pic.gif")));
    entity = (sxml_unparsed_entity){.entity_name = text_view("missing")};
    SXML_TEST_EXPECT(state, built.tree->operations->find_unparsed_entity(
                                built.tree->state, &entity) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    /* Listed in the order of the declarations (D210). */
    SXML_TEST_REQUIRE(state, built.tree->operations->get_unparsed_entity(
                                 built.tree->state, 0, &entity) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(entity.entity_name, text_view("picture")));
    SXML_TEST_REQUIRE(state, built.tree->operations->get_unparsed_entity(
                                 built.tree->state, 1, &entity) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(entity.public_identifier,
                                                  text_view("-//sound")));
    SXML_TEST_EXPECT(state, built.tree->operations->get_unparsed_entity(
                                built.tree->state, 2, &entity) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    /* A copy of the document node keeps them (XSLT 4.0, 11.9.2). */
    built_tree copied;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &copied) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(
        state, sxml_tree_builder_copy_node(
                   sxml_arena_tree_as_tree_builder(copied.arena_tree),
                   &(sxml_copy_request){.node = {.tree = built.tree,
                                                 .node_index = {.index = 0},
                                                 .namespace_number = 0},
                                        .preserves_namespaces = true,
                                        .inherits_namespaces = true,
                                        .preserves_type_annotations = true}) ==
                   SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(copied.arena_tree, &copied.tree) ==
                                 SXML_STATUS_OK);
    entity = (sxml_unparsed_entity){.entity_name = text_view("sound")};
    SXML_TEST_REQUIRE(state, copied.tree->operations->find_unparsed_entity(
                                 copied.tree->state, &entity) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(entity.system_identifier,
                                                  text_view("sound.wav")));
    SXML_TEST_REQUIRE(state, copied.tree->operations->get_unparsed_entity(
                                 copied.tree->state, 0, &entity) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(entity.entity_name, text_view("picture")));
    built_tree_destroy(&copied);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* An element at the top of an external entity keeps the entity's URI
   (D210); other elements have none. */
static void test_entity_base_uris_are_kept(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) == SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_start_document(&writer);
    write_element(&writer, "root");
    const sxml_name_identifier name = intern_name(
        &writer, (test_name){.namespace_uri = "", .prefix = "", .local_name = "chapter"});
    if (writer.status == SXML_STATUS_OK) {
        writer.status = writer.builder.operations->start_element(
            writer.builder.state,
            &(sxml_element_start_event){.element_name = name,
                                        .location = no_location,
                                        .entity_base_uri =
                                            text_view("http://example.com/dir/chapter.xml")});
    }
    write_element(&writer, "para");
    write_end_element(&writer);
    write_end_element(&writer);
    write_end_element(&writer);
    write_end_document(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree, &built.tree) ==
                                 SXML_STATUS_OK);
    sxml_byte_view base_uri = text_view("unset");
    SXML_TEST_REQUIRE(state, built.tree->operations->get_entity_base_uri(
                                 built.tree->state, (sxml_node_index){.index = 2},
                                 &base_uri) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                base_uri, text_view("http://example.com/dir/chapter.xml")));
    SXML_TEST_REQUIRE(state, built.tree->operations->get_entity_base_uri(
                                 built.tree->state, (sxml_node_index){.index = 3},
                                 &base_uri) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, base_uri.byte_count == 0);
    /* dm:base-uri of the descendant is the entity's (XDM 4.0, 5.2). */
    sxml_sequence value = sxml_sequence_empty();
    SXML_TEST_REQUIRE(
        state, sxml_node_make_base_uri(
                   fixture.item_heap,
                   &(sxml_node_value_request){.node = {.tree = built.tree,
                                                       .node_index = {.index = 3},
                                                       .namespace_number = 0},
                                              .error_record = NULL},
                   &value) == SXML_STATUS_OK);
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof datum);
    SXML_TEST_REQUIRE(state, sxml_sequence_get_item(value, (sxml_item_request){.item_index = 0},
                                                    &item) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(datum.datum_details.string.utf8,
                                                  text_view("http://example.com/dir/chapter.xml")));
    sxml_sequence_release(value);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* set_element_identity revises the open element's is-id and is-idrefs
   (include/sxml/tree.h, D263); outside an open element it is refused. */
static void test_element_identity_is_revised(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) == SXML_STATUS_OK);
    event_writer writer = writer_for(&fixture, &built);
    write_start_document(&writer);
    write_element(&writer, "root");
    write_element(&writer, "key");
    write_text(&writer, "k1");
    if (writer.status == SXML_STATUS_OK) {
        writer.status = writer.builder.operations->set_element_identity(
            writer.builder.state,
            &(sxml_element_identity_event){.is_id = true, .is_idrefs = false});
    }
    write_end_element(&writer);
    write_end_element(&writer);
    SXML_TEST_EXPECT(state, writer.builder.operations->set_element_identity(
                                writer.builder.state,
                                &(sxml_element_identity_event){.is_id = true,
                                                               .is_idrefs = true}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    write_end_document(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(built.arena_tree, &built.tree) ==
                                 SXML_STATUS_OK);
    sxml_node_description description;
    SXML_TEST_REQUIRE(state, sxml_node_describe((sxml_node_reference){.tree = built.tree,
                                                                      .node_index = {.index = 2},
                                                                      .namespace_number = 0},
                                                &description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.is_id && !description.is_idrefs);
    SXML_TEST_REQUIRE(state, sxml_node_describe((sxml_node_reference){.tree = built.tree,
                                                                      .node_index = {.index = 1},
                                                                      .namespace_number = 0},
                                                &description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !description.is_id);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Growth and failure */

/* Builds <root> with child_count children, each with an attribute and a
   text child; the status of the first failure. */
static sxml_status build_wide_tree(const tree_fixture *fixture,
                                   built_tree *built, int child_count) {
    event_writer writer = writer_for(fixture, built);
    write_start_document(&writer);
    write_start_element(&writer, (test_name){.namespace_uri = "urn:wide",
                                             .prefix = "w",
                                             .local_name = "root"});
    for (int child_number = 0; child_number < child_count; child_number++) {
        write_element(&writer, "child");
        write_plain_attribute(&writer, "number", "value of the attribute");
        write_text(&writer, "text of the child, long enough to fill blocks");
        write_end_element(&writer);
    }
    write_end_element(&writer);
    write_end_document(&writer);
    if (writer.status != SXML_STATUS_OK) {
        return writer.status;
    }
    return sxml_arena_tree_finish(built->arena_tree, &built->tree);
}

static void test_large_trees_grow_in_segments(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree built;
    SXML_TEST_REQUIRE(state, built_tree_create(&configuration, &built) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, build_wide_tree(&fixture, &built, TEST_CHILD_COUNT) ==
                                 SXML_STATUS_OK);
    uint32_t node_count = sxml_tree_get_node_count(built.tree);
    SXML_TEST_EXPECT(state, node_count == 2 + 3 * TEST_CHILD_COUNT);
    sxml_node_description last = describe(&built, node_count - 1);
    SXML_TEST_EXPECT(state, last.node_kind == SXML_NODE_KIND_TEXT);
    SXML_TEST_EXPECT(state, last.has_parent &&
                                last.parent.index == node_count - 3);
    SXML_TEST_EXPECT(state, content_is(&built, node_count - 2,
                                       "value of the attribute"));
    sxml_node_description root = describe(&built, 1);
    SXML_TEST_EXPECT(state, root.last_child.index == node_count - 3);
    SXML_TEST_EXPECT(state, root.last_descendant.index == node_count - 1);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* D23: fail each allocation in turn; every failure is reported and leaks
   nothing. */
static void test_building_survives_every_allocation_failure(
    sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    uint64_t failing_allocation_number = 1;
    for (;;) {
        sxml_counting_allocator *counting = NULL;
        SXML_TEST_REQUIRE(state,
                          sxml_counting_allocator_create(
                              sxml_default_allocator(),
                              &(sxml_counting_allocator_configuration){
                                  .backing_allocator = sxml_default_allocator(),
                                  .failing_allocation_number =
                                      failing_allocation_number},
                              &counting) == SXML_STATUS_OK);
        sxml_arena_tree_configuration configuration =
            arena_configuration(&fixture);
        configuration.allocator = sxml_counting_allocator_as_allocator(counting);
        configuration.keeps_locations = true;
        configuration.is_destroyed_when_unreferenced = true;
        built_tree built = {.arena_tree = NULL, .tree = NULL};
        sxml_status status = sxml_arena_tree_create(
            sxml_counting_allocator_as_allocator(counting), &configuration,
            &built.arena_tree);
        if (status == SXML_STATUS_OK) {
            status = build_wide_tree(&fixture, &built, 2000);
        }
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            /* The last reference destroys the arena tree. */
            sxml_tree_release(built.tree);
        } else {
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
            sxml_arena_tree_destroy(built.arena_tree);
        }
        SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(counting)
                                        .live_allocation_count == 0);
        sxml_counting_allocator_destroy(counting);
        if (status == SXML_STATUS_OK) {
            break;
        }
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 500);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 5);
    tree_fixture_destroy(&fixture);
}

static const sxml_test_case arena_tree_test_cases[] = {
    {.name = "arena_tree_rejects_invalid_configuration",
     .function = test_arena_tree_rejects_invalid_configuration},
    {.name = "arena_tree_initializes_in_caller_memory",
     .function = test_arena_tree_initializes_in_caller_memory},
    {.name = "arena_tree_works_in_initial_memory",
     .function = test_arena_tree_works_in_initial_memory},
    {.name = "adjacent_text_merges_and_empty_text_vanishes",
     .function = test_adjacent_text_merges_and_empty_text_vanishes},
    {.name = "parentless_nodes_are_roots",
     .function = test_parentless_nodes_are_roots},
    {.name = "text_keeps_cdata_requests", .function = test_text_keeps_cdata_requests},
    {.name = "text_keeps_escaping_marks_apart", .function = test_text_keeps_escaping_marks_apart},
    {.name = "roots_take_the_configured_base_uri",
     .function = test_roots_take_the_configured_base_uri},
    {.name = "parentless_namespace_nodes_are_stored",
     .function = test_parentless_namespace_nodes_are_stored},
    {.name = "events_out_of_order_are_rejected",
     .function = test_events_out_of_order_are_rejected},
    {.name = "duplicate_attributes_follow_the_policy",
     .function = test_duplicate_attributes_follow_the_policy},
    {.name = "many_attributes_are_checked_by_index",
     .function = test_many_attributes_are_checked_by_index},
    {.name = "namespace_fixup_binds_name_prefixes",
     .function = test_namespace_fixup_binds_name_prefixes},
    {.name = "conflicting_bindings_raise_errors",
     .function = test_conflicting_bindings_raise_errors},
    {.name = "limits_stop_untrusted_input",
     .function = test_limits_stop_untrusted_input},
    {.name = "locations_are_kept_on_request",
     .function = test_locations_are_kept_on_request},
    {.name = "unparsed_entities_are_found",
     .function = test_unparsed_entities_are_found},
    {.name = "entity_base_uris_are_kept", .function = test_entity_base_uris_are_kept},
    {.name = "element_identity_is_revised", .function = test_element_identity_is_revised},
    {.name = "large_trees_grow_in_segments",
     .function = test_large_trees_grow_in_segments},
    {.name = "building_survives_every_allocation_failure",
     .function = test_building_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "arena_tree",
            .cases = arena_tree_test_cases,
            .case_count = SXML_TEST_COUNT(arena_tree_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
