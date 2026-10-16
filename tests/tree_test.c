/*
 * Tests for the tree wrapper, node accessors, document order, axes and
 * sxml_tree_builder_copy_node (include/sxml/tree.h; D39, D40, D67), and
 * for node items keeping their tree alive (D33, D60).
 *
 * Imitates tests/memory_test.c: lifecycle checks, then behaviour. The axes
 * are checked against a table of expected results on one fixed document.
 */
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "tree_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

#define TEST_STORAGE_ALIGNMENT 16
#define TEST_STORAGE_BYTE_COUNT 1024
#define TEST_TEXT_BYTE_COUNT 512
/* Elements nested in the deep-tree case. */
#define TEST_NESTING_DEPTH 100000

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

/* ------------------------------------------------------- Fixed document */

/* An arena tree and the tree it finished. */
typedef struct built_tree {
    sxml_arena_tree *arena_tree;
    sxml_tree *tree;
} built_tree;

static void built_tree_destroy(built_tree *built) {
    sxml_tree_release(built->tree);
    sxml_arena_tree_destroy(built->arena_tree);
}

/*
 * The fixed document, with node numbers:
 *
 *   0 #doc
 *   1   <doc xmlns:p="urn:p">
 *   2     <a id="1" x="2">        3 @id, 4 @x
 *   5       <b/>
 *   6       t1
 *   7       <c>
 *   8         <d/></c></a>
 *   9     <!--#c-->
 *  10     <e>
 *  11       <?pi data?></e></doc>
 */
static sxml_status build_fixed_document(const tree_fixture *fixture,
                                        built_tree *built) {
    *built = (built_tree){.arena_tree = NULL, .tree = NULL};
    sxml_arena_tree_configuration configuration = arena_configuration(fixture);
    sxml_status status = sxml_arena_tree_create(
        sxml_default_allocator(), &configuration, &built->arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    event_writer writer = {
        .fixture = fixture,
        .builder = sxml_arena_tree_as_tree_builder(built->arena_tree),
        .status = SXML_STATUS_OK};
    write_start_document(&writer);
    write_element(&writer, "doc");
    write_namespace(&writer, "p", "urn:p");
    write_element(&writer, "a");
    write_plain_attribute(&writer, "id", "1");
    write_plain_attribute(&writer, "x", "2");
    write_element(&writer, "b");
    write_end_element(&writer);
    write_text(&writer, "t");
    write_text(&writer, "1");
    write_element(&writer, "c");
    write_element(&writer, "d");
    write_end_element(&writer);
    write_end_element(&writer);
    write_end_element(&writer);
    write_comment(&writer, "#c");
    write_element(&writer, "e");
    write_processing_instruction(&writer, "pi", "data");
    write_end_element(&writer);
    write_end_element(&writer);
    write_end_document(&writer);
    if (writer.status != SXML_STATUS_OK) {
        return writer.status;
    }
    return sxml_arena_tree_finish(built->arena_tree, &built->tree);
}

static const char *const fixed_document_labels[] = {
    "#doc", "doc", "a", "@id", "@x", "b", "t1", "c", "d", "#c", "e", "#pi",
};

static sxml_node_reference fixed_node(const built_tree *built,
                                      uint32_t node_number) {
    return (sxml_node_reference){.tree = built->tree,
                                 .node_index = {.index = node_number},
                                 .namespace_number = 0};
}

/* Appends the label of node to text (space separated). */
static void append_label(char *text, sxml_node_reference node) {
    char label[16];
    if (node.namespace_number != 0) {
        (void)snprintf(label, sizeof(label), "ns%u",
                       (unsigned)node.namespace_number);
    } else if (node.node_index.index < SXML_TEST_COUNT(fixed_document_labels)) {
        (void)snprintf(label, sizeof(label), "%s",
                       fixed_document_labels[node.node_index.index]);
    } else {
        (void)snprintf(label, sizeof(label), "?");
    }
    if (text[0] != '\0') {
        (void)strncat(text, " ", TEST_TEXT_BYTE_COUNT - strlen(text) - 1);
    }
    (void)strncat(text, label, TEST_TEXT_BYTE_COUNT - strlen(text) - 1);
}

/* Walks an axis and writes its nodes' labels to text. */
static sxml_status walk_axis(sxml_axis_request request, char *text) {
    text[0] = '\0';
    sxml_axis_iterator iterator;
    sxml_status status = sxml_axis_iterator_start(request, &iterator);
    while (status == SXML_STATUS_OK) {
        sxml_node_iteration_step step = {.has_node = false};
        status = sxml_axis_iterator_next(&iterator, &step);
        if (status != SXML_STATUS_OK || !step.has_node) {
            break;
        }
        append_label(text, step.node);
    }
    return status;
}

/* ------------------------------------------------------------ The table */

typedef struct axis_case {
    uint32_t origin;
    uint16_t namespace_number;
    sxml_axis axis;
    const char *expected;
} axis_case;

/* XPath 4.0, section 4.7.5: forward axes in document order, reverse axes
   in reverse document order. */
static const axis_case axis_cases[] = {
    {2, 0, SXML_AXIS_SELF, "a"},
    {2, 0, SXML_AXIS_CHILD, "b t1 c"},
    {2, 0, SXML_AXIS_DESCENDANT, "b t1 c d"},
    {2, 0, SXML_AXIS_DESCENDANT_OR_SELF, "a b t1 c d"},
    {2, 0, SXML_AXIS_ATTRIBUTE, "@id @x"},
    {2, 0, SXML_AXIS_NAMESPACE, "ns1 ns2"},
    {2, 0, SXML_AXIS_FOLLOWING_SIBLING, "#c e"},
    {2, 0, SXML_AXIS_FOLLOWING_SIBLING_OR_SELF, "a #c e"},
    {2, 0, SXML_AXIS_FOLLOWING, "#c e #pi"},
    {2, 0, SXML_AXIS_FOLLOWING_OR_SELF, "a #c e #pi"},
    {2, 0, SXML_AXIS_PARENT, "doc"},
    {2, 0, SXML_AXIS_ANCESTOR, "doc #doc"},
    {2, 0, SXML_AXIS_ANCESTOR_OR_SELF, "a doc #doc"},
    {2, 0, SXML_AXIS_PRECEDING_SIBLING, ""},
    {2, 0, SXML_AXIS_PRECEDING_SIBLING_OR_SELF, "a"},
    {2, 0, SXML_AXIS_PRECEDING, ""},
    {2, 0, SXML_AXIS_PRECEDING_OR_SELF, "a"},
    {8, 0, SXML_AXIS_PRECEDING, "t1 b"},
    {8, 0, SXML_AXIS_PRECEDING_OR_SELF, "d t1 b"},
    {8, 0, SXML_AXIS_ANCESTOR, "c a doc #doc"},
    {8, 0, SXML_AXIS_FOLLOWING, "#c e #pi"},
    {8, 0, SXML_AXIS_CHILD, ""},
    {8, 0, SXML_AXIS_DESCENDANT, ""},
    {5, 0, SXML_AXIS_FOLLOWING_SIBLING, "t1 c"},
    {5, 0, SXML_AXIS_FOLLOWING, "t1 c d #c e #pi"},
    {7, 0, SXML_AXIS_PRECEDING_SIBLING, "t1 b"},
    {10, 0, SXML_AXIS_PRECEDING_SIBLING, "#c a"},
    {10, 0, SXML_AXIS_PRECEDING_SIBLING_OR_SELF, "e #c a"},
    {11, 0, SXML_AXIS_PRECEDING, "#c d c t1 b a"},
    {11, 0, SXML_AXIS_PRECEDING_OR_SELF, "#pi #c d c t1 b a"},
    {11, 0, SXML_AXIS_FOLLOWING, ""},
    {11, 0, SXML_AXIS_ANCESTOR_OR_SELF, "#pi e doc #doc"},
    {4, 0, SXML_AXIS_PARENT, "a"},
    {4, 0, SXML_AXIS_SELF, "@x"},
    {4, 0, SXML_AXIS_CHILD, ""},
    {4, 0, SXML_AXIS_ATTRIBUTE, ""},
    {4, 0, SXML_AXIS_DESCENDANT_OR_SELF, "@x"},
    {4, 0, SXML_AXIS_FOLLOWING_SIBLING, ""},
    {4, 0, SXML_AXIS_FOLLOWING_SIBLING_OR_SELF, "@x"},
    {4, 0, SXML_AXIS_PRECEDING_SIBLING_OR_SELF, "@x"},
    {4, 0, SXML_AXIS_FOLLOWING, "b t1 c d #c e #pi"},
    {4, 0, SXML_AXIS_FOLLOWING_OR_SELF, "@x b t1 c d #c e #pi"},
    {4, 0, SXML_AXIS_PRECEDING, ""},
    {4, 0, SXML_AXIS_ANCESTOR_OR_SELF, "@x a doc #doc"},
    {9, 0, SXML_AXIS_PRECEDING, "d c t1 b a"},
    {2, 2, SXML_AXIS_SELF, "ns2"},
    {2, 2, SXML_AXIS_PARENT, "a"},
    {2, 2, SXML_AXIS_ANCESTOR, "a doc #doc"},
    {2, 2, SXML_AXIS_CHILD, ""},
    {2, 2, SXML_AXIS_ATTRIBUTE, ""},
    {2, 2, SXML_AXIS_NAMESPACE, ""},
    {2, 2, SXML_AXIS_DESCENDANT_OR_SELF, "ns2"},
    {2, 2, SXML_AXIS_FOLLOWING, "b t1 c d #c e #pi"},
    {2, 2, SXML_AXIS_PRECEDING, ""},
    {2, 2, SXML_AXIS_FOLLOWING_SIBLING_OR_SELF, "ns2"},
    {0, 0, SXML_AXIS_DESCENDANT, "doc a b t1 c d #c e #pi"},
    {0, 0, SXML_AXIS_CHILD, "doc"},
    {0, 0, SXML_AXIS_PARENT, ""},
    {0, 0, SXML_AXIS_NAMESPACE, ""},
    {0, 0, SXML_AXIS_ATTRIBUTE, ""},
    {0, 0, SXML_AXIS_FOLLOWING, ""},
    {0, 0, SXML_AXIS_PRECEDING, ""},
    {1, 0, SXML_AXIS_NAMESPACE, "ns1 ns2"},
    {1, 0, SXML_AXIS_CHILD, "a #c e"},
};

static void test_axes_match_table(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_tree_get_node_count(built.tree) == 12);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(axis_cases);
         case_index++) {
        const axis_case *axis_case_entry = &axis_cases[case_index];
        sxml_node_reference origin =
            fixed_node(&built, axis_case_entry->origin);
        origin.namespace_number = axis_case_entry->namespace_number;
        char text[TEST_TEXT_BYTE_COUNT];
        sxml_status status = walk_axis(
            (sxml_axis_request){.origin = origin,
                                .axis = axis_case_entry->axis},
            text);
        bool matches = status == SXML_STATUS_OK &&
                       strcmp(text, axis_case_entry->expected) == 0;
        if (!matches) {
            sxml_byte_view status_name = sxml_status_name(status);
            printf("axis case %zu: expected \"%s\", got \"%s\" (%.*s)\n",
                   case_index, axis_case_entry->expected, text,
                   (int)status_name.byte_count,
                   (const char *)status_name.bytes);
        }
        SXML_TEST_EXPECT(state, matches);
    }
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

static void test_axis_iterator_checks_origin(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    sxml_axis_iterator iterator = {.has_next_node = false};
    SXML_TEST_EXPECT(state, sxml_axis_iterator_start(
                                (sxml_axis_request){
                                    .origin = fixed_node(&built, 12),
                                    .axis = SXML_AXIS_SELF},
                                &iterator) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    sxml_node_reference namespace_node = fixed_node(&built, 2);
    namespace_node.namespace_number = 3;
    SXML_TEST_EXPECT(state, sxml_axis_iterator_start(
                                (sxml_axis_request){.origin = namespace_node,
                                                    .axis = SXML_AXIS_SELF},
                                &iterator) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    namespace_node = fixed_node(&built, 3);
    namespace_node.namespace_number = 1;
    SXML_TEST_EXPECT(state, sxml_axis_iterator_start(
                                (sxml_axis_request){.origin = namespace_node,
                                                    .axis = SXML_AXIS_PARENT},
                                &iterator) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, !iterator.has_next_node);
    /* An iterator at its end stays there. */
    SXML_TEST_REQUIRE(state, sxml_axis_iterator_start(
                                 (sxml_axis_request){
                                     .origin = fixed_node(&built, 0),
                                     .axis = SXML_AXIS_PARENT},
                                 &iterator) == SXML_STATUS_OK);
    sxml_node_iteration_step step = {.has_node = true};
    SXML_TEST_EXPECT(state, sxml_axis_iterator_next(&iterator, &step) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !step.has_node);
    step.has_node = true;
    SXML_TEST_EXPECT(state, sxml_axis_iterator_next(&iterator, &step) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !step.has_node);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* ------------------------------------------------------- Node accessors */

static void test_nodes_describe_themselves(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    sxml_node_description description;
    SXML_TEST_REQUIRE(state, sxml_node_describe(fixed_node(&built, 2),
                                                &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.node_kind == SXML_NODE_KIND_ELEMENT);
    SXML_TEST_EXPECT(state, description.has_parent &&
                                description.parent.index == 1);
    SXML_TEST_EXPECT(state, description.attribute_count == 2);
    SXML_TEST_EXPECT(state, description.last_descendant.index == 8);
    SXML_TEST_EXPECT(state, description.has_children &&
                                description.first_child.index == 5 &&
                                description.last_child.index == 7);
    SXML_TEST_EXPECT(state, description.type_annotation.index ==
                                SXML_TYPE_XS_UNTYPED);
    SXML_TEST_REQUIRE(state, sxml_node_describe(fixed_node(&built, 4),
                                                &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.node_kind == SXML_NODE_KIND_ATTRIBUTE);
    SXML_TEST_EXPECT(state, description.type_annotation.index ==
                                SXML_TYPE_XS_UNTYPED_ATOMIC);
    SXML_TEST_EXPECT(state, !description.has_next_sibling &&
                                !description.has_previous_sibling);
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(state, sxml_node_get_content(fixed_node(&built, 4),
                                                  &content) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(content, text_view("2")));
    SXML_TEST_EXPECT(state, sxml_node_get_content(fixed_node(&built, 6),
                                                  &content) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(content, text_view("t1")));
    SXML_TEST_EXPECT(state, sxml_node_get_content(fixed_node(&built, 11),
                                                  &content) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(content, text_view("data")));
    SXML_TEST_EXPECT(state, sxml_node_get_content(fixed_node(&built, 1),
                                                  &content) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_node_get_content(fixed_node(&built, 99),
                                                  &content) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    /* The processing instruction's name is its target (XDM 4.0, 7.5.6). */
    SXML_TEST_REQUIRE(state, sxml_node_describe(fixed_node(&built, 11),
                                                &description) ==
                                 SXML_STATUS_OK);
    sxml_name_parts parts;
    SXML_TEST_REQUIRE(state, sxml_name_pool_get_name_parts(
                                 fixture.name_pool, description.node_name,
                                 &parts) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(parts.local_name, text_view("pi")));
    sxml_tree_description tree_description;
    SXML_TEST_REQUIRE(state, sxml_tree_describe(built.tree,
                                                &tree_description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, tree_description.node_count == 12);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                tree_description.document_uri,
                                text_view("urn:document")));
    SXML_TEST_EXPECT(state, tree_description.name_pool == fixture.name_pool);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

static void test_namespace_nodes_are_made_on_request(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    sxml_namespace_binding_view bindings;
    SXML_TEST_REQUIRE(state, sxml_node_get_in_scope_namespaces(
                                 fixed_node(&built, 2), &bindings) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, bindings.namespace_binding_count == 2);
    /* a declares nothing, so it shares doc's list (D40). */
    sxml_namespace_binding_view parent_bindings;
    SXML_TEST_REQUIRE(state, sxml_node_get_in_scope_namespaces(
                                 fixed_node(&built, 1), &parent_bindings) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, parent_bindings.namespace_bindings ==
                                bindings.namespace_bindings);
    SXML_TEST_EXPECT(state, sxml_node_get_in_scope_namespaces(
                                fixed_node(&built, 3), &bindings) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_node_reference namespace_node = fixed_node(&built, 2);
    namespace_node.namespace_number = 2;
    SXML_TEST_EXPECT(state, sxml_node_get_in_scope_namespaces(
                                namespace_node, &bindings) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_node_description description;
    SXML_TEST_REQUIRE(state, sxml_node_describe(namespace_node,
                                                &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.node_kind == SXML_NODE_KIND_NAMESPACE);
    SXML_TEST_EXPECT(state, description.has_parent &&
                                description.parent.index == 2);
    SXML_TEST_EXPECT(state, !description.has_children &&
                                !description.has_next_sibling);
    sxml_name_parts parts;
    SXML_TEST_REQUIRE(state, sxml_name_pool_get_name_parts(
                                 fixture.name_pool, description.node_name,
                                 &parts) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, parts.namespace_uri.index == SXML_NAMESPACE_NONE);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(parts.local_name, text_view("p")));
    sxml_byte_view content;
    SXML_TEST_REQUIRE(state, sxml_node_get_content(namespace_node, &content) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(content, text_view("urn:p")));
    namespace_node.namespace_number = 1;
    SXML_TEST_REQUIRE(state, sxml_node_get_content(namespace_node, &content) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                content, text_view(
                                             "http://www.w3.org/XML/1998/"
                                             "namespace")));
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

static void test_document_order_within_and_across_trees(
    sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree first;
    built_tree second;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &first) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &second) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_tree_get_identifier(first.tree).value <
                                sxml_tree_get_identifier(second.tree).value);
    sxml_node_reference element = fixed_node(&first, 2);
    sxml_node_reference namespace_node = element;
    namespace_node.namespace_number = 1;
    sxml_node_reference attribute = fixed_node(&first, 3);
    SXML_TEST_EXPECT(state, sxml_node_compare_document_order(
                                (sxml_node_pair){.first = element,
                                                 .second = namespace_node}) ==
                                SXML_ORDERING_LESS);
    SXML_TEST_EXPECT(state, sxml_node_compare_document_order(
                                (sxml_node_pair){.first = attribute,
                                                 .second = namespace_node}) ==
                                SXML_ORDERING_GREATER);
    SXML_TEST_EXPECT(state, sxml_node_compare_document_order(
                                (sxml_node_pair){.first = element,
                                                 .second = element}) ==
                                SXML_ORDERING_EQUAL);
    SXML_TEST_EXPECT(state,
                     sxml_node_compare_document_order((sxml_node_pair){
                         .first = fixed_node(&second, 0),
                         .second = fixed_node(&first, 11)}) ==
                         SXML_ORDERING_GREATER);
    SXML_TEST_EXPECT(state, sxml_node_is_same_node(element, element));
    SXML_TEST_EXPECT(state, !sxml_node_is_same_node(element, namespace_node));
    SXML_TEST_EXPECT(state,
                     !sxml_node_is_same_node(element, fixed_node(&second, 2)));
    built_tree_destroy(&first);
    built_tree_destroy(&second);
    tree_fixture_destroy(&fixture);
}

/* -------------------------------------------------------- Tree wrapper */

/* A test double: a tree of one text node, counting owner releases. */
typedef struct single_node_state {
    sxml_name_pool *name_pool;
    uint32_t node_count;
    uint32_t release_count;
} single_node_state;

static sxml_status single_node_describe_tree(
    void *state, sxml_tree_description *description) {
    const single_node_state *single = state;
    *description = (sxml_tree_description){
        .node_count = single->node_count,
        .document_uri = {.bytes = NULL, .byte_count = 0},
        .base_uri = {.bytes = NULL, .byte_count = 0},
        .name_pool = single->name_pool};
    return SXML_STATUS_OK;
}

static sxml_status single_node_describe_node(
    void *state, sxml_node_index node_index,
    sxml_node_description *description) {
    (void)state;
    *description = (sxml_node_description){
        .node_kind = SXML_NODE_KIND_TEXT,
        .last_descendant = node_index,
        .type_annotation = {.index = SXML_TYPE_NONE}};
    return SXML_STATUS_OK;
}

static sxml_status single_node_get_content(void *state,
                                           sxml_node_index node_index,
                                           sxml_byte_view *content) {
    (void)state;
    (void)node_index;
    *content = text_view("single");
    return SXML_STATUS_OK;
}

static sxml_status single_node_get_in_scope_namespaces(
    void *state, sxml_node_index element,
    sxml_namespace_binding_view *namespace_bindings) {
    (void)state;
    (void)element;
    (void)namespace_bindings;
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_tree_operations single_node_operations = {
    .describe_tree = single_node_describe_tree,
    .describe_node = single_node_describe_node,
    .get_node_content = single_node_get_content,
    .get_in_scope_namespaces = single_node_get_in_scope_namespaces,
    .get_typed_value = NULL,
    .get_location = NULL,
    .find_unparsed_entity = NULL,
    .find_element_by_id = NULL,
};

static void single_node_release(void *state, sxml_tree *tree) {
    (void)tree;
    single_node_state *single = state;
    single->release_count += 1;
}

static const sxml_tree_owner_operations single_node_owner_operations = {
    .release = single_node_release,
};

static void test_tree_rejects_invalid_configuration(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    single_node_state single = {
        .name_pool = fixture.name_pool, .node_count = 1, .release_count = 0};
    sxml_tree_configuration configuration = {
        .state = &single,
        .operations = NULL,
        .item_heap = fixture.item_heap,
        .owner = {.state = NULL, .operations = NULL}};
    sxml_memory_requirements requirements = {.byte_count = 7,
                                             .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_tree_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    configuration.operations = &single_node_operations;
    configuration.item_heap = NULL;
    SXML_TEST_EXPECT(state, sxml_tree_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, requirements.byte_count == 7);
    configuration.item_heap = fixture.item_heap;
    sxml_tree *tree = NULL;
    /* A tree of no nodes, or with another name pool, is refused. */
    single.node_count = 0;
    SXML_TEST_EXPECT(state, sxml_tree_create(sxml_default_allocator(),
                                             &configuration, &tree) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    single.node_count = 1;
    single.name_pool = NULL;
    SXML_TEST_EXPECT(state, sxml_tree_create(sxml_default_allocator(),
                                             &configuration, &tree) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, tree == NULL);
    /* No identifier is used by a failed initialize. */
    SXML_TEST_EXPECT(state, sxml_item_heap_read_counts(fixture.item_heap)
                                    .issued_tree_identifier_count == 0);
    tree_fixture_destroy(&fixture);
}

static void test_tree_initializes_in_caller_memory(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    single_node_state single = {
        .name_pool = fixture.name_pool, .node_count = 1, .release_count = 0};
    const sxml_tree_configuration configuration = {
        .state = &single,
        .operations = &single_node_operations,
        .item_heap = fixture.item_heap,
        .owner = {.state = &single,
                  .operations = &single_node_owner_operations}};
    sxml_memory_requirements requirements;
    SXML_TEST_REQUIRE(state, sxml_tree_memory_requirements(
                                 &configuration, &requirements) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      requirements.byte_count <= TEST_STORAGE_BYTE_COUNT);
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[TEST_STORAGE_BYTE_COUNT];
    sxml_tree *tree = NULL;
    SXML_TEST_EXPECT(state, sxml_tree_initialize(
                                (sxml_memory_view){
                                    .bytes = storage,
                                    .byte_count = requirements.byte_count - 1},
                                &configuration, &tree) ==
                                SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, sxml_tree_initialize(
                                (sxml_memory_view){
                                    .bytes = storage + 1,
                                    .byte_count = requirements.byte_count},
                                &configuration, &tree) ==
                                SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_REQUIRE(state, sxml_tree_initialize(
                                 (sxml_memory_view){
                                     .bytes = storage,
                                     .byte_count = requirements.byte_count},
                                 &configuration, &tree) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_tree_get_identifier(tree).value == 1);
    sxml_byte_view content;
    SXML_TEST_EXPECT(state, sxml_node_get_content(
                                (sxml_node_reference){
                                    .tree = tree,
                                    .node_index = {.index = 0},
                                    .namespace_number = 0},
                                &content) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(content, text_view("single")));
    /* The owner hears of the last release once (D39). */
    sxml_tree_retain(tree);
    sxml_tree_release(tree);
    SXML_TEST_EXPECT(state, single.release_count == 0);
    sxml_tree_release(tree);
    SXML_TEST_EXPECT(state, single.release_count == 1);
    sxml_tree_release(tree);
    SXML_TEST_EXPECT(state, single.release_count == 1);
    sxml_tree_finalize(tree);
    sxml_tree_finalize(NULL);
    sxml_tree_destroy(NULL);
    sxml_tree_retain(NULL);
    sxml_tree_release(NULL);
    tree_fixture_destroy(&fixture);
}

static void test_immortal_trees_ignore_counting(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    single_node_state single = {
        .name_pool = fixture.name_pool, .node_count = 1, .release_count = 0};
    const sxml_tree_configuration configuration = {
        .state = &single,
        .operations = &single_node_operations,
        .item_heap = fixture.item_heap,
        .owner = {.state = &single,
                  .operations = &single_node_owner_operations}};
    sxml_tree *tree = NULL;
    SXML_TEST_REQUIRE(state, sxml_tree_create(sxml_default_allocator(),
                                              &configuration, &tree) ==
                                 SXML_STATUS_OK);
    /* A count that would overflow becomes immortal (tree.h). */
    tree->reference_count = SXML_TREE_IMMORTAL_REFERENCE_COUNT - 1;
    sxml_tree_retain(tree);
    SXML_TEST_EXPECT(state, tree->reference_count ==
                                SXML_TREE_IMMORTAL_REFERENCE_COUNT);
    sxml_tree_release(tree);
    sxml_tree_retain(tree);
    SXML_TEST_EXPECT(state, tree->reference_count ==
                                SXML_TREE_IMMORTAL_REFERENCE_COUNT);
    SXML_TEST_EXPECT(state, single.release_count == 0);
    sxml_tree_destroy(tree);
    tree_fixture_destroy(&fixture);
}

/* ------------------------------------------------------- Node items */

/* A node item as packet 1.13 will make it (src/xdm_internal.h layout). */
static sxml_item node_item_of(sxml_node_reference node) {
    sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NODE,
                      .flags = 0,
                      .namespace_number = node.namespace_number,
                      .type_or_node_index = node.node_index.index,
                      .payload = {.bits = 0}};
    item.payload.tree = node.tree;
    return item;
}

static void test_node_item_keeps_its_tree_alive(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_counting_allocator *counting = NULL;
    SXML_TEST_REQUIRE(state, sxml_counting_allocator_create(
                                 sxml_default_allocator(),
                                 &(sxml_counting_allocator_configuration){
                                     .backing_allocator =
                                         sxml_default_allocator(),
                                     .failing_allocation_number = 0},
                                 &counting) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    configuration.allocator = sxml_counting_allocator_as_allocator(counting);
    configuration.is_destroyed_when_unreferenced = true;
    sxml_arena_tree *arena_tree = NULL;
    SXML_TEST_REQUIRE(state, sxml_arena_tree_create(
                                 sxml_counting_allocator_as_allocator(counting),
                                 &configuration, &arena_tree) ==
                                 SXML_STATUS_OK);
    event_writer writer = {
        .fixture = &fixture,
        .builder = sxml_arena_tree_as_tree_builder(arena_tree),
        .status = SXML_STATUS_OK};
    write_element(&writer, "kept");
    write_text(&writer, "alive");
    write_end_element(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    sxml_tree *tree = NULL;
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(arena_tree, &tree) ==
                                 SXML_STATUS_OK);
    sxml_item item = node_item_of((sxml_node_reference){
        .tree = tree, .node_index = {.index = 1}, .namespace_number = 0});
    sxml_item_retain(item);
    /* The builder's reference goes; the item's keeps the tree. */
    sxml_tree_release(tree);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(counting)
                                    .live_allocation_count > 0);
    sxml_byte_view content;
    SXML_TEST_EXPECT(state, sxml_node_get_content(
                                (sxml_node_reference){
                                    .tree = item.payload.tree,
                                    .node_index = {.index = 1},
                                    .namespace_number = 0},
                                &content) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(content, text_view("alive")));
    /* The last reference destroys the arena tree and all its memory. */
    sxml_item_release(item);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(counting)
                                    .live_allocation_count == 0);
    sxml_counting_allocator_destroy(counting);
    tree_fixture_destroy(&fixture);
}

/* ------------------------------------------------------------ Copying */

/* Writes a readable form of the subtree of node into text. */
static sxml_status dump_node(const tree_fixture *fixture,
                             sxml_node_reference node, char *text);

static void dump_append(char *text, const char *piece) {
    (void)strncat(text, piece, TEST_TEXT_BYTE_COUNT - strlen(text) - 1);
}

static void dump_append_view(char *text, sxml_byte_view view) {
    size_t room = TEST_TEXT_BYTE_COUNT - strlen(text) - 1;
    size_t byte_count = view.byte_count < room ? view.byte_count : room;
    if (byte_count != 0) {
        (void)strncat(text, (const char *)view.bytes, byte_count);
    }
}

static sxml_status dump_name(const tree_fixture *fixture,
                             const sxml_node_description *description,
                             char *text) {
    sxml_byte_view prefix;
    sxml_status status = sxml_name_pool_get_namespace_prefix(
        fixture->name_pool, description->namespace_prefix, &prefix);
    sxml_name_parts parts;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(
            fixture->name_pool, description->node_name, &parts);
    }
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(
            fixture->name_pool, parts.namespace_uri, &namespace_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (prefix.byte_count != 0) {
        dump_append_view(text, prefix);
        dump_append(text, ":");
    }
    dump_append_view(text, parts.local_name);
    if (namespace_uri.byte_count != 0) {
        dump_append(text, "{");
        dump_append_view(text, namespace_uri);
        dump_append(text, "}");
    }
    return SXML_STATUS_OK;
}

static sxml_status dump_namespaces(const tree_fixture *fixture,
                                   sxml_node_reference element, char *text) {
    sxml_namespace_binding_view bindings;
    sxml_status status = sxml_node_get_in_scope_namespaces(element, &bindings);
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK &&
         binding_index < bindings.namespace_binding_count;
         binding_index++) {
        sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
        if (binding.namespace_uri.index == SXML_NAMESPACE_XML) {
            continue;
        }
        sxml_byte_view prefix;
        sxml_byte_view namespace_uri;
        status = sxml_name_pool_get_namespace_prefix(
            fixture->name_pool, binding.namespace_prefix, &prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(
                fixture->name_pool, binding.namespace_uri, &namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            dump_append(text, " xmlns");
            if (prefix.byte_count != 0) {
                dump_append(text, ":");
                dump_append_view(text, prefix);
            }
            dump_append(text, "=");
            dump_append_view(text, namespace_uri);
        }
    }
    return status;
}

/* A document or element being dumped: the attribute pass (0), then the
   child pass (1). */
typedef struct dump_frame {
    sxml_node_reference node;
    sxml_axis_iterator iterator;
    bool is_element;
    int pass_number;
} dump_frame;

/* The documents and elements being dumped, outermost first: an explicit
   stack instead of recursion (D71); test trees are shallow. */
#define TEST_DUMP_MAXIMUM_DEPTH 64

typedef struct dump_stack {
    dump_frame frames[TEST_DUMP_MAXIMUM_DEPTH];
    size_t frame_count;
} dump_stack;

/* A node to start dumping onto a stack. */
typedef struct dump_entry {
    dump_stack *stack;
    sxml_node_reference node;
} dump_entry;

/* Dumps all of a leaf node, or opens a document or element and pushes its
   frame, which then lists its attributes. */
static sxml_status dump_enter(const tree_fixture *fixture,
                              const dump_entry *entry, char *text) {
    sxml_node_reference node = entry->node;
    sxml_node_description description;
    sxml_status status = sxml_node_describe(node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    switch (description.node_kind) {
    case SXML_NODE_KIND_DOCUMENT:
    case SXML_NODE_KIND_ELEMENT: {
        dump_stack *stack = entry->stack;
        if (stack->frame_count == TEST_DUMP_MAXIMUM_DEPTH) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        bool is_element = description.node_kind == SXML_NODE_KIND_ELEMENT;
        dump_frame *frame = &stack->frames[stack->frame_count];
        stack->frame_count += 1;
        frame->node = node;
        frame->is_element = is_element;
        frame->pass_number = 0;
        dump_append(text, is_element ? "<" : "[doc ");
        if (is_element) {
            status = dump_name(fixture, &description, text);
            if (status == SXML_STATUS_OK) {
                status = dump_namespaces(fixture, node, text);
            }
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_axis_iterator_start(
                (sxml_axis_request){.origin = node,
                                    .axis = SXML_AXIS_ATTRIBUTE},
                &frame->iterator);
        }
        return status;
    }
    case SXML_NODE_KIND_ATTRIBUTE:
        status = dump_name(fixture, &description, text);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_get_content(node, &content);
        }
        dump_append(text, "=\"");
        dump_append_view(text, content);
        dump_append(text, "\"");
        return status;
    case SXML_NODE_KIND_TEXT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
    case SXML_NODE_KIND_NAMESPACE:
    case SXML_NODE_KIND_ANY:
        status = sxml_node_get_content(node, &content);
        dump_append(text, description.node_kind == SXML_NODE_KIND_TEXT
                              ? "'"
                              : "(");
        dump_append_view(text, content);
        dump_append(text, description.node_kind == SXML_NODE_KIND_TEXT
                              ? "'"
                              : ")");
        return status;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static sxml_status dump_node(const tree_fixture *fixture,
                             sxml_node_reference node, char *text) {
    dump_stack stack = {.frame_count = 0};
    sxml_status status =
        dump_enter(fixture, &(dump_entry){.stack = &stack, .node = node}, text);
    while (status == SXML_STATUS_OK && stack.frame_count > 0) {
        dump_frame *frame = &stack.frames[stack.frame_count - 1];
        sxml_node_iteration_step step;
        status = sxml_axis_iterator_next(&frame->iterator, &step);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (step.has_node) {
            if (frame->pass_number == 0) {
                dump_append(text, " ");
            }
            status = dump_enter(
                fixture, &(dump_entry){.stack = &stack, .node = step.node},
                text);
        } else if (frame->pass_number == 0) {
            frame->pass_number = 1;
            dump_append(text, frame->is_element ? ">" : "");
            status = sxml_axis_iterator_start(
                (sxml_axis_request){.origin = frame->node,
                                    .axis = SXML_AXIS_CHILD},
                &frame->iterator);
        } else {
            dump_append(text, frame->is_element ? "</>" : "]");
            stack.frame_count -= 1;
        }
    }
    /* After a failure, close what is open, as the dump always does. */
    while (stack.frame_count > 0) {
        stack.frame_count -= 1;
        dump_append(text, stack.frames[stack.frame_count].is_element ? "</>"
                                                                    : "]");
    }
    return status;
}

/* Copies node into a new arena tree and dumps the copy's root. */
static sxml_status copy_and_dump(const tree_fixture *fixture,
                                 const sxml_copy_request *request,
                                 char *text) {
    text[0] = '\0';
    sxml_arena_tree_configuration configuration = arena_configuration(fixture);
    built_tree copy = {.arena_tree = NULL, .tree = NULL};
    sxml_status status = sxml_arena_tree_create(
        sxml_default_allocator(), &configuration, &copy.arena_tree);
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_builder_copy_node(
            sxml_arena_tree_as_tree_builder(copy.arena_tree), request);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(copy.arena_tree, &copy.tree);
    }
    if (status == SXML_STATUS_OK) {
        status = dump_node(fixture,
                           (sxml_node_reference){.tree = copy.tree,
                                                 .node_index = {.index = 0},
                                                 .namespace_number = 0},
                           text);
    }
    built_tree_destroy(&copy);
    return status;
}

static sxml_copy_request copy_request_of(sxml_node_reference node) {
    return (sxml_copy_request){.node = node,
                               .preserves_namespaces = true,
                               .inherits_namespaces = true,
                               .preserves_type_annotations = true};
}

static void test_copy_node_rebuilds_subtrees(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    char original[TEST_TEXT_BYTE_COUNT] = "";
    SXML_TEST_REQUIRE(state, dump_node(&fixture, fixed_node(&built, 0),
                                       original) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     strcmp(original,
                            "[doc <doc xmlns:p=urn:p><a xmlns:p=urn:p "
                            "id=\"1\" x=\"2\"><b xmlns:p=urn:p></>'t1'<c "
                            "xmlns:p=urn:p><d xmlns:p=urn:p></></></>(#c)<e "
                            "xmlns:p=urn:p>(data)</></>]") == 0);
    char text[TEST_TEXT_BYTE_COUNT];
    sxml_copy_request request = copy_request_of(fixed_node(&built, 0));
    SXML_TEST_REQUIRE(state, copy_and_dump(&fixture, &request, text) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(text, original) == 0);
    /* An element keeps its in-scope namespaces with preserve... */
    request = copy_request_of(fixed_node(&built, 2));
    SXML_TEST_REQUIRE(state, copy_and_dump(&fixture, &request, text) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     strcmp(text, "<a xmlns:p=urn:p id=\"1\" x=\"2\"><b "
                                  "xmlns:p=urn:p></>'t1'<c xmlns:p=urn:p><d "
                                  "xmlns:p=urn:p></></></>") == 0);
    /* ...and only the namespaces its names use without (XQuery 4.0,
       copy-namespaces no-preserve). */
    request.preserves_namespaces = false;
    SXML_TEST_REQUIRE(state, copy_and_dump(&fixture, &request, text) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(text, "<a id=\"1\" x=\"2\"><b></>'t1'<c><d>"
                                         "</></></>") == 0);
    request = copy_request_of(fixed_node(&built, 11));
    SXML_TEST_REQUIRE(state, copy_and_dump(&fixture, &request, text) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(text, "(data)") == 0);
    request = copy_request_of(fixed_node(&built, 4));
    SXML_TEST_REQUIRE(state, copy_and_dump(&fixture, &request, text) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(text, "x=\"2\"") == 0);
    request = copy_request_of(fixed_node(&built, 99));
    SXML_TEST_EXPECT(state, copy_and_dump(&fixture, &request, text) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

static void test_copy_node_copies_namespace_nodes(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree copy = {.arena_tree = NULL, .tree = NULL};
    SXML_TEST_REQUIRE(state, sxml_arena_tree_create(sxml_default_allocator(),
                                                    &configuration,
                                                    &copy.arena_tree) ==
                                 SXML_STATUS_OK);
    event_writer writer = {
        .fixture = &fixture,
        .builder = sxml_arena_tree_as_tree_builder(copy.arena_tree),
        .status = SXML_STATUS_OK};
    write_element(&writer, "holder");
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    sxml_node_reference namespace_node = fixed_node(&built, 1);
    namespace_node.namespace_number = 2;
    sxml_copy_request request = copy_request_of(namespace_node);
    SXML_TEST_EXPECT(state, sxml_tree_builder_copy_node(writer.builder,
                                                        &request) ==
                                SXML_STATUS_OK);
    request = copy_request_of(fixed_node(&built, 3));
    SXML_TEST_EXPECT(state, sxml_tree_builder_copy_node(writer.builder,
                                                        &request) ==
                                SXML_STATUS_OK);
    write_end_element(&writer);
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(copy.arena_tree,
                                                    &copy.tree) ==
                                 SXML_STATUS_OK);
    char text[TEST_TEXT_BYTE_COUNT] = "";
    SXML_TEST_REQUIRE(state,
                      dump_node(&fixture,
                                (sxml_node_reference){
                                    .tree = copy.tree,
                                    .node_index = {.index = 0},
                                    .namespace_number = 0},
                                text) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     strcmp(text, "<holder xmlns:p=urn:p id=\"1\"></>") == 0);
    built_tree_destroy(&copy);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* Copies element a of the fixed document into <holder xmlns:h="urn:h">
   and dumps the result. */
static sxml_status copy_into_holder(const tree_fixture *fixture,
                                    const sxml_copy_request *request,
                                    char *text) {
    text[0] = '\0';
    sxml_arena_tree_configuration configuration = arena_configuration(fixture);
    built_tree copy = {.arena_tree = NULL, .tree = NULL};
    sxml_status status = sxml_arena_tree_create(
        sxml_default_allocator(), &configuration, &copy.arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    event_writer writer = {
        .fixture = fixture,
        .builder = sxml_arena_tree_as_tree_builder(copy.arena_tree),
        .status = SXML_STATUS_OK};
    write_element(&writer, "holder");
    write_namespace(&writer, "h", "urn:h");
    status = writer.status;
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_builder_copy_node(writer.builder, request);
    }
    if (status == SXML_STATUS_OK) {
        write_end_element(&writer);
        status = writer.status;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(copy.arena_tree, &copy.tree);
    }
    if (status == SXML_STATUS_OK) {
        status = dump_node(fixture,
                           (sxml_node_reference){.tree = copy.tree,
                                                 .node_index = {.index = 0},
                                                 .namespace_number = 0},
                           text);
    }
    built_tree_destroy(&copy);
    return status;
}

/* copy-namespaces inherit and no-inherit (XQuery 4.0, section 5.6.7): only
   the top element of the copy is cut off from the new parent's bindings,
   and its descendants then inherit from it (D69). */
static void test_copy_node_honours_no_inherit(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    char text[TEST_TEXT_BYTE_COUNT];
    sxml_copy_request request = copy_request_of(fixed_node(&built, 2));
    SXML_TEST_REQUIRE(state, copy_into_holder(&fixture, &request, text) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strstr(text, "<a xmlns:h=urn:h") != NULL);
    request.inherits_namespaces = false;
    SXML_TEST_REQUIRE(state, copy_into_holder(&fixture, &request, text) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     strcmp(text, "<holder xmlns:h=urn:h><a xmlns:p=urn:p "
                                  "id=\"1\" x=\"2\"><b xmlns:p=urn:p></>'t1'"
                                  "<c xmlns:p=urn:p><d xmlns:p=urn:p></></></>"
                                  "</>") == 0);
    built_tree_destroy(&built);
    tree_fixture_destroy(&fixture);
}

/* Nested 10^5 deep: copying and walking use constant C stack. */
static void test_deep_trees_copy_and_walk_iteratively(sxml_test_state *state) {
    tree_fixture fixture;
    SXML_TEST_REQUIRE(state, tree_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_arena_tree_configuration configuration = arena_configuration(&fixture);
    built_tree deep = {.arena_tree = NULL, .tree = NULL};
    SXML_TEST_REQUIRE(state, sxml_arena_tree_create(sxml_default_allocator(),
                                                    &configuration,
                                                    &deep.arena_tree) ==
                                 SXML_STATUS_OK);
    event_writer writer = {
        .fixture = &fixture,
        .builder = sxml_arena_tree_as_tree_builder(deep.arena_tree),
        .status = SXML_STATUS_OK};
    for (int level = 0; level < TEST_NESTING_DEPTH; level++) {
        write_element(&writer, "n");
    }
    write_text(&writer, "leaf");
    for (int level = 0; level < TEST_NESTING_DEPTH; level++) {
        write_end_element(&writer);
    }
    SXML_TEST_REQUIRE(state, writer.status == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(deep.arena_tree,
                                                    &deep.tree) ==
                                 SXML_STATUS_OK);
    built_tree copy = {.arena_tree = NULL, .tree = NULL};
    SXML_TEST_REQUIRE(state, sxml_arena_tree_create(sxml_default_allocator(),
                                                    &configuration,
                                                    &copy.arena_tree) ==
                                 SXML_STATUS_OK);
    sxml_copy_request request = copy_request_of(
        (sxml_node_reference){.tree = deep.tree,
                              .node_index = {.index = 0},
                              .namespace_number = 0});
    SXML_TEST_EXPECT(state, sxml_tree_builder_copy_node(
                                sxml_arena_tree_as_tree_builder(
                                    copy.arena_tree),
                                &request) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_arena_tree_finish(copy.arena_tree,
                                                    &copy.tree) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_tree_get_node_count(copy.tree) ==
                                TEST_NESTING_DEPTH + 1);
    sxml_axis_iterator iterator;
    SXML_TEST_REQUIRE(state,
                      sxml_axis_iterator_start(
                          (sxml_axis_request){
                              .origin = {.tree = copy.tree,
                                         .node_index = {.index =
                                                            TEST_NESTING_DEPTH},
                                         .namespace_number = 0},
                              .axis = SXML_AXIS_ANCESTOR},
                          &iterator) == SXML_STATUS_OK);
    uint32_t ancestor_count = 0;
    for (;;) {
        sxml_node_iteration_step step;
        if (sxml_axis_iterator_next(&iterator, &step) != SXML_STATUS_OK ||
            !step.has_node) {
            break;
        }
        ancestor_count += 1;
    }
    SXML_TEST_EXPECT(state, ancestor_count == TEST_NESTING_DEPTH);
    built_tree_destroy(&copy);
    built_tree_destroy(&deep);
    tree_fixture_destroy(&fixture);
}

static const sxml_test_case tree_test_cases[] = {
    {.name = "axes_match_table", .function = test_axes_match_table},
    {.name = "axis_iterator_checks_origin",
     .function = test_axis_iterator_checks_origin},
    {.name = "nodes_describe_themselves",
     .function = test_nodes_describe_themselves},
    {.name = "namespace_nodes_are_made_on_request",
     .function = test_namespace_nodes_are_made_on_request},
    {.name = "document_order_within_and_across_trees",
     .function = test_document_order_within_and_across_trees},
    {.name = "tree_rejects_invalid_configuration",
     .function = test_tree_rejects_invalid_configuration},
    {.name = "tree_initializes_in_caller_memory",
     .function = test_tree_initializes_in_caller_memory},
    {.name = "immortal_trees_ignore_counting",
     .function = test_immortal_trees_ignore_counting},
    {.name = "node_item_keeps_its_tree_alive",
     .function = test_node_item_keeps_its_tree_alive},
    {.name = "copy_node_rebuilds_subtrees",
     .function = test_copy_node_rebuilds_subtrees},
    {.name = "copy_node_copies_namespace_nodes",
     .function = test_copy_node_copies_namespace_nodes},
    {.name = "copy_node_honours_no_inherit",
     .function = test_copy_node_honours_no_inherit},
    {.name = "deep_trees_copy_and_walk_iteratively",
     .function = test_deep_trees_copy_and_walk_iteratively},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "tree",
            .cases = tree_test_cases,
            .case_count = SXML_TEST_COUNT(tree_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
