/*
 * Tests for node items and their accessors (string value, typed value,
 * base URI) and for JNodes with the generic node operations
 * (include/sxml/xdm.h; src/xdm_node.c, src/xdm_json_node.c; D33, D39, D60).
 *
 * Imitates tests/xdm_function_test.c and tests/tree_test.c (fixture, event
 * writer, D23 failure loop). Behaviour follows XDM 4.0, sections 6.2, 7 and
 * 8.5, XML Base, section 4.2, and RFC 3986, section 5.4 (the examples of
 * reference resolution). The QT4 cases op-union-406 and fn-root-29 are
 * written out as unit cases.
 */
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "tree_internal.h"

#include <string.h>

/* Arrays nested in the deep JTree case. */
#define TEST_NESTING_DEPTH 10000

/* ------------------------------------------------------------ Fixture */

typedef struct node_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} node_fixture;

static sxml_status node_fixture_create_with(sxml_allocator allocator,
                                            node_fixture *fixture) {
    *fixture = (node_fixture){
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
            &(sxml_item_heap_configuration){.allocator = allocator,
                                            .name_pool = fixture->name_pool,
                                            .type_registry = fixture->registry,
                                            .maximum_byte_count = 0},
            &fixture->item_heap);
    }
    return status;
}

static sxml_status node_fixture_create(node_fixture *fixture) {
    return node_fixture_create_with(sxml_default_allocator(), fixture);
}

static void node_fixture_destroy(node_fixture *fixture) {
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

static uint64_t live_cell_count(const node_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
}

static uint64_t live_byte_count(const node_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_byte_count;
}

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* --------------------------------------------------------- Event writer */

/* Sends events to an arena tree, keeping the first failure. */
typedef struct event_writer {
    const node_fixture *fixture;
    sxml_tree_builder builder;
    sxml_status status;
} event_writer;

static const sxml_source_location no_location = {
    .source_name = {.bytes = NULL, .byte_count = 0},
    .line_number = 0,
    .column_number = 0,
    .byte_offset = 0};

static void write_start_document(event_writer *writer, const char *base_uri) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->start_document(
            writer->builder.state,
            &(sxml_document_start_event){.document_uri = text_view(""),
                                         .base_uri = text_view(base_uri),
                                         .location = no_location});
    }
}

static void write_end_document(event_writer *writer) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status =
            writer->builder.operations->end_document(writer->builder.state);
    }
}

static void write_element(event_writer *writer, const char *local_name) {
    sxml_name_identifier element_name = {.index = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_name(
            writer->fixture->name_pool,
            &(sxml_name_request){.namespace_uri = {.index = 0},
                                 .local_name = text_view(local_name)},
            &element_name);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->start_element(
            writer->builder.state,
            &(sxml_element_start_event){
                .element_name = element_name,
                .namespace_prefix = {.index = 0},
                .type_annotation = {.index = SXML_TYPE_NONE},
                .is_id = false,
                .is_idrefs = false,
                .is_nilled = false,
                .location = no_location});
    }
}

static void write_end_element(event_writer *writer) {
    if (writer->status == SXML_STATUS_OK) {
        writer->status =
            writer->builder.operations->end_element(writer->builder.state);
    }
}

static void write_namespace(event_writer *writer, const char *prefix,
                            const char *namespace_uri) {
    sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                      .namespace_uri = {.index = 0}};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_prefix(
            writer->fixture->name_pool, text_view(prefix),
            &binding.namespace_prefix);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_uri(
            writer->fixture->name_pool, text_view(namespace_uri),
            &binding.namespace_uri);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->add_namespace_binding(
            writer->builder.state, &binding);
    }
}

/* An attribute: in the XML namespace with prefix xml when is_xml, else in
   no namespace. */
typedef struct test_attribute {
    bool is_xml;
    const char *local_name;
    const char *value;
} test_attribute;

static void write_attribute(event_writer *writer, test_attribute attribute) {
    sxml_name_identifier attribute_name = {.index = 0};
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_name(
            writer->fixture->name_pool,
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(
                    attribute.is_xml ? SXML_NAMESPACE_XML
                                     : SXML_NAMESPACE_NONE),
                .local_name = text_view(attribute.local_name)},
            &attribute_name);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = sxml_name_pool_intern_namespace_prefix(
            writer->fixture->name_pool,
            text_view(attribute.is_xml ? "xml" : ""), &prefix);
    }
    if (writer->status == SXML_STATUS_OK) {
        writer->status = writer->builder.operations->add_attribute(
            writer->builder.state,
            &(sxml_attribute_event){
                .attribute_name = attribute_name,
                .namespace_prefix = prefix,
                .attribute_value = text_view(attribute.value),
                .type_annotation = {.index = SXML_TYPE_NONE},
                .is_id = false,
                .is_idrefs = false,
                .location = no_location});
    }
}

static void write_xml_base(event_writer *writer, const char *value) {
    write_attribute(writer, (test_attribute){.is_xml = true,
                                             .local_name = "base",
                                             .value = value});
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

/* ------------------------------------------------------------- Documents */

/* An arena tree and the tree it finished. */
typedef struct built_tree {
    sxml_arena_tree *arena_tree;
    sxml_tree *tree;
} built_tree;

static void built_tree_destroy(built_tree *built) {
    sxml_tree_release(built->tree);
    sxml_arena_tree_destroy(built->arena_tree);
}

static sxml_status built_tree_start(const node_fixture *fixture,
                                    built_tree *built,
                                    event_writer *writer) {
    *built = (built_tree){.arena_tree = NULL, .tree = NULL};
    sxml_status status = sxml_arena_tree_create(
        sxml_default_allocator(),
        &(sxml_arena_tree_configuration){
            .allocator = sxml_default_allocator(),
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = fixture->item_heap,
            .maximum_node_count = 0,
            .maximum_depth = 0,
            .maximum_text_byte_count = 0,
            .keeps_locations = false,
            .duplicate_attribute_policy =
                SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = false,
            .error_record = NULL},
        &built->arena_tree);
    *writer = (event_writer){
        .fixture = fixture,
        .builder = status == SXML_STATUS_OK
                       ? sxml_arena_tree_as_tree_builder(built->arena_tree)
                       : (sxml_tree_builder){.state = NULL,
                                             .operations = NULL},
        .status = status};
    return status;
}

static sxml_status built_tree_finish(built_tree *built,
                                     const event_writer *writer) {
    sxml_status status = writer->status;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(built->arena_tree, &built->tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(built->arena_tree);
        *built = (built_tree){.arena_tree = NULL, .tree = NULL};
    }
    return status;
}

/*
 * The fixed document, with node numbers:
 *
 *   0 #doc (base URI http://example.com/dir/doc.xml)
 *   1   <r xmlns:p="urn:p" xml:base="sub/">       2 @xml:base
 *   3     he
 *   4     <b xml:base="../other/x.xml" n="v">     5 @xml:base, 6 @n
 *   7       ll
 *   8       <c/></b>
 *   9     o
 *  10     <!--note-->
 *  11     <?target data?></r>
 */
#define TEST_DOCUMENT_NODE 0
#define TEST_R_ELEMENT 1
#define TEST_R_BASE_ATTRIBUTE 2
#define TEST_HE_TEXT 3
#define TEST_B_ELEMENT 4
#define TEST_N_ATTRIBUTE 6
#define TEST_LL_TEXT 7
#define TEST_C_ELEMENT 8
#define TEST_NOTE_COMMENT 10
#define TEST_TARGET_INSTRUCTION 11

static sxml_status build_fixed_document(const node_fixture *fixture,
                                        built_tree *built) {
    event_writer writer;
    if (built_tree_start(fixture, built, &writer) != SXML_STATUS_OK) {
        return writer.status;
    }
    write_start_document(&writer, "http://example.com/dir/doc.xml");
    write_element(&writer, "r");
    write_namespace(&writer, "p", "urn:p");
    write_xml_base(&writer, "sub/");
    write_text(&writer, "he");
    write_element(&writer, "b");
    write_xml_base(&writer, "../other/x.xml");
    write_attribute(&writer, (test_attribute){
                                 .is_xml = false, .local_name = "n",
                                 .value = "v"});
    write_text(&writer, "ll");
    write_element(&writer, "c");
    write_end_element(&writer);
    write_end_element(&writer);
    write_text(&writer, "o");
    write_comment(&writer, "note");
    write_processing_instruction(&writer, "target", "data");
    write_end_element(&writer);
    write_end_document(&writer);
    return built_tree_finish(built, &writer);
}

/* A document whose element <e> carries xml:base="reference" (none when
   reference is NULL), under a document with base_uri. */
typedef struct base_document_request {
    const char *base_uri;
    const char *reference;
} base_document_request;

static sxml_status build_base_document(const node_fixture *fixture,
                                       base_document_request request,
                                       built_tree *built) {
    event_writer writer;
    if (built_tree_start(fixture, built, &writer) != SXML_STATUS_OK) {
        return writer.status;
    }
    write_start_document(&writer, request.base_uri);
    write_element(&writer, "e");
    if (request.reference != NULL) {
        write_xml_base(&writer, request.reference);
    }
    write_end_element(&writer);
    write_end_document(&writer);
    return built_tree_finish(built, &writer);
}

static sxml_node_reference node_at(const built_tree *built,
                                   uint32_t node_number) {
    return sxml_node_reference_of_stored_node(
        (sxml_stored_node_request){.tree = built->tree,
                                   .node_number = node_number});
}

/* ------------------------------------------------------------ Checking */

/* True when item is an atomic item of type with text as its string. */
typedef struct expected_text {
    sxml_builtin_type type;
    const char *text;
} expected_text;

static bool item_has_text(sxml_item item, expected_text expected) {
    sxml_atomic_datum datum;
    if (sxml_item_get_type_annotation(item).index != (uint32_t)expected.type ||
        sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return false;
    }
    sxml_byte_view utf8 = datum.datum_details.string.utf8;
    sxml_byte_view wanted = text_view(expected.text);
    return utf8.byte_count == wanted.byte_count &&
           (wanted.byte_count == 0 ||
            memcmp(utf8.bytes, wanted.bytes, wanted.byte_count) == 0);
}

static bool sequence_has_text(sxml_sequence sequence, expected_text expected) {
    return sxml_sequence_get_representation(sequence) ==
               SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
           item_has_text(sequence.content, expected);
}

static bool sequence_is_empty(sxml_sequence sequence) {
    return sxml_sequence_get_representation(sequence) ==
           SXML_SEQUENCE_REPRESENTATION_EMPTY;
}

/* ----------------------------------------------------------- Node items */

static void test_node_items_hold_their_tree(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    size_t references_before = built.tree->reference_count;
    sxml_node_reference namespace_node = {
        .tree = built.tree,
        .node_index = {.index = TEST_R_ELEMENT},
        .namespace_number = 1};
    sxml_item item = sxml_node_item_make(namespace_node);
    SXML_TEST_EXPECT(state,
                     built.tree->reference_count == references_before + 1);
    SXML_TEST_EXPECT(state, sxml_item_get_kind(item) == SXML_ITEM_KIND_NODE);
    SXML_TEST_EXPECT(state, sxml_item_is_generic_node(item));
    SXML_TEST_EXPECT(state, !sxml_item_is_function_item(item));
    sxml_node_reference node = {.tree = NULL};
    SXML_TEST_EXPECT(state, sxml_item_get_node(item, &node) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_node_is_same_node(node, namespace_node));
    sxml_item_retain(item);
    SXML_TEST_EXPECT(state,
                     built.tree->reference_count == references_before + 2);
    sxml_item_release(item);
    sxml_item_release(item);
    SXML_TEST_EXPECT(state, built.tree->reference_count == references_before);

    /* Only XNodes have a node reference. */
    SXML_TEST_EXPECT(state,
                     sxml_item_get_node(sxml_atomic_item_from_integer(1),
                                        &node) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    sxml_item root = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_json_node_make_root(
                                 fixture.item_heap, sxml_sequence_empty(),
                                 &root) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_item_get_kind(root) ==
                                SXML_ITEM_KIND_JSON_NODE);
    SXML_TEST_EXPECT(state, sxml_item_get_node(root, &node) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_item_release(root);
    built_tree_destroy(&built);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

/* -------------------------------------------------------- String values */

typedef struct string_value_case {
    uint32_t node_number;
    uint16_t namespace_number;
    const char *string_value;
    /* The type of the typed value. */
    sxml_builtin_type typed_value_type;
} string_value_case;

static const string_value_case string_value_cases[] = {
    {TEST_DOCUMENT_NODE, 0, "hello", SXML_TYPE_XS_UNTYPED_ATOMIC},
    {TEST_R_ELEMENT, 0, "hello", SXML_TYPE_XS_UNTYPED_ATOMIC},
    {TEST_R_BASE_ATTRIBUTE, 0, "sub/", SXML_TYPE_XS_UNTYPED_ATOMIC},
    {TEST_HE_TEXT, 0, "he", SXML_TYPE_XS_UNTYPED_ATOMIC},
    {TEST_B_ELEMENT, 0, "ll", SXML_TYPE_XS_UNTYPED_ATOMIC},
    {TEST_N_ATTRIBUTE, 0, "v", SXML_TYPE_XS_UNTYPED_ATOMIC},
    {TEST_C_ELEMENT, 0, "", SXML_TYPE_XS_UNTYPED_ATOMIC},
    {TEST_NOTE_COMMENT, 0, "note", SXML_TYPE_XS_STRING},
    {TEST_TARGET_INSTRUCTION, 0, "data", SXML_TYPE_XS_STRING},
};

static void test_string_and_typed_values(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    for (size_t case_index = 0;
         case_index < SXML_TEST_COUNT(string_value_cases); case_index += 1) {
        const string_value_case *test_case = &string_value_cases[case_index];
        sxml_node_reference node = node_at(&built, test_case->node_number);
        sxml_item string_value = {.representation = 0};
        SXML_TEST_EXPECT(state, sxml_node_make_string_value(
                                    fixture.item_heap, node, &string_value) ==
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, item_has_text(string_value,
                                              (expected_text){
                                                  .type = SXML_TYPE_XS_STRING,
                                                  .text =
                                                      test_case->string_value}));
        sxml_item_release(string_value);
        sxml_sequence typed_value = sxml_sequence_empty();
        SXML_TEST_EXPECT(
            state, sxml_node_make_typed_value(
                       fixture.item_heap,
                       &(sxml_node_value_request){.node = node,
                                                  .error_record = NULL},
                       &typed_value) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(
            state, sequence_has_text(typed_value,
                                     (expected_text){
                                         .type = test_case->typed_value_type,
                                         .text = test_case->string_value}));
        sxml_sequence_release(typed_value);
    }

    /* A namespace node: its URI, as xs:string (XDM 4.0, section 7.5). The
       xml binding comes first or second; find p's. */
    sxml_namespace_binding_view bindings;
    SXML_TEST_REQUIRE(state, sxml_node_get_in_scope_namespaces(
                                 node_at(&built, TEST_R_ELEMENT), &bindings) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, bindings.namespace_binding_count == 2);
    uint16_t namespace_number = 1;
    sxml_byte_view uri = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_REQUIRE(state, sxml_name_pool_get_namespace_uri(
                                 fixture.name_pool,
                                 bindings.namespace_bindings[0].namespace_uri,
                                 &uri) == SXML_STATUS_OK);
    if (uri.byte_count != 5) {
        namespace_number = 2;
    }
    sxml_node_reference namespace_node = {
        .tree = built.tree,
        .node_index = {.index = TEST_R_ELEMENT},
        .namespace_number = namespace_number};
    sxml_item string_value = {.representation = 0};
    SXML_TEST_EXPECT(state, sxml_node_make_string_value(
                                fixture.item_heap, namespace_node,
                                &string_value) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item_has_text(string_value,
                                          (expected_text){
                                              .type = SXML_TYPE_XS_STRING,
                                              .text = "urn:p"}));
    sxml_item_release(string_value);
    sxml_sequence typed_value = sxml_sequence_empty();
    SXML_TEST_EXPECT(
        state, sxml_node_make_typed_value(
                   fixture.item_heap,
                   &(sxml_node_value_request){.node = namespace_node,
                                              .error_record = NULL},
                   &typed_value) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sequence_has_text(
                                typed_value,
                                (expected_text){.type = SXML_TYPE_XS_STRING,
                                                .text = "urn:p"}));
    sxml_sequence_release(typed_value);

    /* A node index the tree does not have. */
    SXML_TEST_EXPECT(state, sxml_node_make_string_value(
                                fixture.item_heap, node_at(&built, 99),
                                &string_value) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    built_tree_destroy(&built);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Validated typed value */

/*
 * A host tree of two nodes, an element annotated xs:integer and its text
 * "42", whose get_typed_value gives 42 or fails as told.
 */
typedef struct host_tree_state {
    sxml_name_pool *name_pool;
    sxml_name_identifier element_name;
    bool fails;
    size_t typed_value_call_count;
} host_tree_state;

static sxml_status host_describe_tree(void *state,
                                      sxml_tree_description *description) {
    const host_tree_state *host = state;
    *description = (sxml_tree_description){
        .node_count = 2,
        .document_uri = {.bytes = NULL, .byte_count = 0},
        .base_uri = {.bytes = NULL, .byte_count = 0},
        .name_pool = host->name_pool};
    return SXML_STATUS_OK;
}

static sxml_status host_describe_node(void *state, sxml_node_index node_index,
                                      sxml_node_description *description) {
    const host_tree_state *host = state;
    bool is_element = node_index.index == 0;
    *description = (sxml_node_description){
        .node_kind = is_element ? SXML_NODE_KIND_ELEMENT : SXML_NODE_KIND_TEXT,
        .node_name = is_element ? host->element_name
                                : (sxml_name_identifier){.index = 0},
        .namespace_prefix = {.index = 0},
        .has_parent = !is_element,
        .parent = {.index = 0},
        .last_descendant = {.index = 1},
        .attribute_count = 0,
        .has_children = is_element,
        .first_child = {.index = 1},
        .last_child = {.index = 1},
        .has_previous_sibling = false,
        .previous_sibling = {.index = 0},
        .has_next_sibling = false,
        .next_sibling = {.index = 0},
        .type_annotation = sxml_builtin_type_identifier(
            is_element ? SXML_TYPE_XS_INTEGER : SXML_TYPE_NONE),
        .is_id = false,
        .is_idrefs = false,
        .is_nilled = false};
    return SXML_STATUS_OK;
}

static sxml_status host_get_node_content(void *state,
                                         sxml_node_index node_index,
                                         sxml_byte_view *content) {
    (void)state;
    if (node_index.index != 1) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *content = SXML_BYTE_VIEW_FROM_LITERAL("42");
    return SXML_STATUS_OK;
}

static sxml_status host_get_in_scope_namespaces(
    void *state, sxml_node_index element,
    sxml_namespace_binding_view *namespace_bindings) {
    (void)state;
    (void)element;
    *namespace_bindings = (sxml_namespace_binding_view){
        .namespace_bindings = NULL, .namespace_binding_count = 0};
    return SXML_STATUS_OK;
}

static sxml_status
host_get_typed_value(void *state, const sxml_typed_value_request *request,
                     sxml_sequence *typed_value) {
    host_tree_state *host = state;
    host->typed_value_call_count += 1;
    if (host->fails || request->node_index.index != 0) {
        return SXML_STATUS_TYPE_ERROR;
    }
    *typed_value =
        sxml_sequence_from_item(sxml_atomic_item_from_integer(42));
    return SXML_STATUS_OK;
}

static const sxml_tree_operations host_tree_operations = {
    .describe_tree = host_describe_tree,
    .describe_node = host_describe_node,
    .get_node_content = host_get_node_content,
    .get_in_scope_namespaces = host_get_in_scope_namespaces,
    .get_typed_value = host_get_typed_value,
    .get_location = NULL,
    .find_unparsed_entity = NULL,
    .find_element_by_id = NULL,
};

static void test_validated_typed_values_come_from_the_tree(
    sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    host_tree_state host = {.name_pool = fixture.name_pool,
                            .element_name = {.index = 0},
                            .fails = false,
                            .typed_value_call_count = 0};
    SXML_TEST_REQUIRE(state, sxml_name_pool_intern_name(
                                 fixture.name_pool,
                                 &(sxml_name_request){
                                     .namespace_uri = {.index = 0},
                                     .local_name = text_view("number")},
                                 &host.element_name) == SXML_STATUS_OK);
    sxml_tree *tree = NULL;
    SXML_TEST_REQUIRE(
        state,
        sxml_tree_create(sxml_default_allocator(),
                         &(sxml_tree_configuration){
                             .state = &host,
                             .operations = &host_tree_operations,
                             .item_heap = fixture.item_heap,
                             .owner = {.state = NULL, .operations = NULL}},
                         &tree) == SXML_STATUS_OK);
    sxml_node_reference element = {
        .tree = tree, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_sequence typed_value = sxml_sequence_empty();
    SXML_TEST_EXPECT(state,
                     sxml_node_make_typed_value(
                         fixture.item_heap,
                         &(sxml_node_value_request){.node = element,
                                                    .error_record = NULL},
                         &typed_value) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, host.typed_value_call_count == 1);
    SXML_TEST_EXPECT(state,
                     sxml_atomic_item_is_atomic_equal(
                         typed_value.content, sxml_atomic_item_from_integer(42)));
    sxml_sequence_release(typed_value);

    /* The tree's failure (FOTY0012, say) comes through unchanged. */
    host.fails = true;
    typed_value = sxml_sequence_empty();
    SXML_TEST_EXPECT(state,
                     sxml_node_make_typed_value(
                         fixture.item_heap,
                         &(sxml_node_value_request){.node = element,
                                                    .error_record = NULL},
                         &typed_value) == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(state, sequence_is_empty(typed_value));

    /* The text child is untyped: derived, the tree is not asked. */
    SXML_TEST_EXPECT(
        state, sxml_node_make_typed_value(
                   fixture.item_heap,
                   &(sxml_node_value_request){
                       .node = {.tree = tree,
                                .node_index = {.index = 1},
                                .namespace_number = 0},
                       .error_record = NULL},
                   &typed_value) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, host.typed_value_call_count == 2);
    SXML_TEST_EXPECT(state, sequence_has_text(
                                typed_value,
                                (expected_text){
                                    .type = SXML_TYPE_XS_UNTYPED_ATOMIC,
                                    .text = "42"}));
    sxml_sequence_release(typed_value);
    /* The string value of the element needs no typed value either. */
    sxml_item string_value = {.representation = 0};
    SXML_TEST_EXPECT(state, sxml_node_make_string_value(
                                fixture.item_heap, element, &string_value) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item_has_text(string_value,
                                          (expected_text){
                                              .type = SXML_TYPE_XS_STRING,
                                              .text = "42"}));
    sxml_item_release(string_value);
    sxml_tree_destroy(tree);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

/* ------------------------------------------------------------ Base URIs */

typedef struct base_uri_case {
    uint32_t node_number;
    /* NULL for the empty sequence. */
    const char *base_uri;
} base_uri_case;

static const base_uri_case base_uri_cases[] = {
    {TEST_DOCUMENT_NODE, "http://example.com/dir/doc.xml"},
    {TEST_R_ELEMENT, "http://example.com/dir/sub/"},
    {TEST_R_BASE_ATTRIBUTE, "http://example.com/dir/sub/"},
    {TEST_HE_TEXT, "http://example.com/dir/sub/"},
    {TEST_B_ELEMENT, "http://example.com/dir/other/x.xml"},
    {TEST_N_ATTRIBUTE, "http://example.com/dir/other/x.xml"},
    {TEST_LL_TEXT, "http://example.com/dir/other/x.xml"},
    {TEST_C_ELEMENT, "http://example.com/dir/other/x.xml"},
    {TEST_NOTE_COMMENT, "http://example.com/dir/sub/"},
    {TEST_TARGET_INSTRUCTION, "http://example.com/dir/sub/"},
};

/* Makes the base URI of node and checks it against expected (NULL for the
   empty sequence). */
typedef struct base_uri_check {
    const node_fixture *fixture;
    sxml_node_reference node;
    const char *expected;
} base_uri_check;

static bool base_uri_is(base_uri_check check) {
    sxml_sequence base_uri = sxml_sequence_empty();
    sxml_status status = sxml_node_make_base_uri(
        check.fixture->item_heap,
        &(sxml_node_value_request){.node = check.node, .error_record = NULL},
        &base_uri);
    bool is_expected =
        status == SXML_STATUS_OK &&
        (check.expected == NULL
             ? sequence_is_empty(base_uri)
             : sequence_has_text(base_uri,
                                 (expected_text){.type = SXML_TYPE_XS_ANY_URI,
                                                 .text = check.expected}));
    sxml_sequence_release(base_uri);
    return is_expected;
}

static void test_base_uris_follow_xml_base(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(base_uri_cases);
         case_index += 1) {
        const base_uri_case *test_case = &base_uri_cases[case_index];
        SXML_TEST_EXPECT(state, base_uri_is((base_uri_check){
                                    .fixture = &fixture,
                                    .node = node_at(&built,
                                                    test_case->node_number),
                                    .expected = test_case->base_uri}));
    }
    /* A namespace node has none (XDM 4.0, section 7.5.4). */
    SXML_TEST_EXPECT(state, base_uri_is((base_uri_check){
                                .fixture = &fixture,
                                .node = {.tree = built.tree,
                                         .node_index = {.index = TEST_R_ELEMENT},
                                         .namespace_number = 1},
                                .expected = NULL}));
    built_tree_destroy(&built);

    /* Without a base URI: none, or the xml:base value as written. */
    SXML_TEST_REQUIRE(state, build_base_document(
                                 &fixture,
                                 (base_document_request){.base_uri = "",
                                                         .reference = NULL},
                                 &built) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, base_uri_is((base_uri_check){
                                .fixture = &fixture,
                                .node = node_at(&built, 1),
                                .expected = NULL}));
    SXML_TEST_EXPECT(state, base_uri_is((base_uri_check){
                                .fixture = &fixture,
                                .node = node_at(&built, 0),
                                .expected = NULL}));
    built_tree_destroy(&built);
    SXML_TEST_REQUIRE(state, build_base_document(
                                 &fixture,
                                 (base_document_request){
                                     .base_uri = "", .reference = "rel/x"},
                                 &built) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, base_uri_is((base_uri_check){
                                .fixture = &fixture,
                                .node = node_at(&built, 1),
                                .expected = "rel/x"}));
    built_tree_destroy(&built);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

/* RFC 3986, section 5.4: the reference and its resolution against
   http://a/b/c/d;p?q. */
typedef struct resolution_example {
    const char *reference;
    const char *target_uri;
} resolution_example;

static const resolution_example resolution_examples[] = {
    /* Section 5.4.1, normal examples. */
    {"g:h", "g:h"},
    {"g", "http://a/b/c/g"},
    {"./g", "http://a/b/c/g"},
    {"g/", "http://a/b/c/g/"},
    {"/g", "http://a/g"},
    {"//g", "http://g"},
    {"?y", "http://a/b/c/d;p?y"},
    {"g?y", "http://a/b/c/g?y"},
    {"#s", "http://a/b/c/d;p?q#s"},
    {"g#s", "http://a/b/c/g#s"},
    {"g?y#s", "http://a/b/c/g?y#s"},
    {";x", "http://a/b/c/;x"},
    {"g;x", "http://a/b/c/g;x"},
    {"g;x?y#s", "http://a/b/c/g;x?y#s"},
    {"", "http://a/b/c/d;p?q"},
    {".", "http://a/b/c/"},
    {"./", "http://a/b/c/"},
    {"..", "http://a/b/"},
    {"../", "http://a/b/"},
    {"../g", "http://a/b/g"},
    {"../..", "http://a/"},
    {"../../", "http://a/"},
    {"../../g", "http://a/g"},
    /* Section 5.4.2, abnormal examples (strict parser). */
    {"../../../g", "http://a/g"},
    {"../../../../g", "http://a/g"},
    {"/./g", "http://a/g"},
    {"/../g", "http://a/g"},
    {"g.", "http://a/b/c/g."},
    {".g", "http://a/b/c/.g"},
    {"g..", "http://a/b/c/g.."},
    {"..g", "http://a/b/c/..g"},
    {"./../g", "http://a/b/g"},
    {"./g/.", "http://a/b/c/g/"},
    {"g/./h", "http://a/b/c/g/h"},
    {"g/../h", "http://a/b/c/h"},
    {"g;x=1/./y", "http://a/b/c/g;x=1/y"},
    {"g;x=1/../y", "http://a/b/c/y"},
    {"g?y/./x", "http://a/b/c/g?y/./x"},
    {"g?y/../x", "http://a/b/c/g?y/../x"},
    {"g#s/./x", "http://a/b/c/g#s/./x"},
    {"g#s/../x", "http://a/b/c/g#s/../x"},
    {"http:g", "http:g"},
};

static void test_reference_resolution_examples(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    for (size_t example_index = 0;
         example_index < SXML_TEST_COUNT(resolution_examples);
         example_index += 1) {
        const resolution_example *example =
            &resolution_examples[example_index];
        built_tree built;
        SXML_TEST_REQUIRE(state, build_base_document(
                                     &fixture,
                                     (base_document_request){
                                         .base_uri = "http://a/b/c/d;p?q",
                                         .reference = example->reference},
                                     &built) == SXML_STATUS_OK);
        bool is_resolved = base_uri_is((base_uri_check){
            .fixture = &fixture,
            .node = node_at(&built, 1),
            .expected = example->target_uri});
        if (!is_resolved) {
            /* Names the failing example in the report. */
            SXML_TEST_EXPECT(state, example->reference == NULL);
        }
        built_tree_destroy(&built);
    }
    /* A base with an authority and no path gets "/" before the merged
       path (RFC 3986, section 5.2.3). */
    built_tree built;
    SXML_TEST_REQUIRE(state, build_base_document(
                                 &fixture,
                                 (base_document_request){
                                     .base_uri = "http://a", .reference = "g"},
                                 &built) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, base_uri_is((base_uri_check){
                                .fixture = &fixture,
                                .node = node_at(&built, 1),
                                .expected = "http://a/g"}));
    built_tree_destroy(&built);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

/* --------------------------------------------------------------- JNodes */

static sxml_status make_string(sxml_item_heap *item_heap, const char *text,
                               sxml_item *item) {
    sxml_string string;
    sxml_status status = sxml_string_from_utf8(text_view(text), &string);
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

/* A map of two entries, first_key: first_value, second_key: second_value,
   integers as values. */
typedef struct two_entry_map {
    const char *first_key;
    int64_t first_value;
    const char *second_key;
    int64_t second_value;
} two_entry_map;

static sxml_status make_two_entry_map(sxml_item_heap *item_heap,
                                      two_entry_map description,
                                      sxml_item *map) {
    sxml_item first_key = {.representation = 0};
    sxml_item second_key = {.representation = 0};
    sxml_status status =
        make_string(item_heap, description.first_key, &first_key);
    if (status == SXML_STATUS_OK) {
        status = make_string(item_heap, description.second_key, &second_key);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_map_entry entries[2] = {
            {.key = first_key,
             .value = sxml_sequence_from_item(
                 sxml_atomic_item_from_integer(description.first_value))},
            {.key = second_key,
             .value = sxml_sequence_from_item(
                 sxml_atomic_item_from_integer(description.second_value))},
        };
        status = sxml_map_make(
            item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries, .map_entry_count = 2},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                .error_record = NULL},
            map);
    }
    sxml_item_release(first_key);
    sxml_item_release(second_key);
    return status;
}

/* [1, 2, 3] */
static sxml_status make_small_array(sxml_item_heap *item_heap,
                                    sxml_item *array) {
    const sxml_sequence members[3] = {
        sxml_sequence_from_item(sxml_atomic_item_from_integer(1)),
        sxml_sequence_from_item(sxml_atomic_item_from_integer(2)),
        sxml_sequence_from_item(sxml_atomic_item_from_integer(3)),
    };
    return sxml_array_make(
        item_heap,
        (sxml_sequence_view){.sequences = members, .sequence_count = 3},
        array);
}

static sxml_status make_root_over_item(sxml_item_heap *item_heap,
                                       sxml_item value, sxml_item *root) {
    return sxml_json_node_make_root(item_heap, sxml_sequence_from_item(value),
                                    root);
}

static sxml_item make_child_or_none(sxml_item_heap *item_heap,
                                    sxml_json_child_request request) {
    sxml_item child = {.representation = 0};
    if (sxml_json_node_make_child(item_heap, request, &child) !=
        SXML_STATUS_OK) {
        return (sxml_item){.representation = 0};
    }
    return child;
}

static bool is_same(sxml_item first, sxml_item second) {
    return sxml_generic_node_is_same_node(
        (sxml_item_pair){.first = first, .second = second});
}

/* The document order of two generic nodes, compared with expected. */
typedef struct expected_order {
    sxml_item first;
    sxml_item second;
    sxml_ordering ordering;
} expected_order;

static bool is_in_order(expected_order expected) {
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    return sxml_generic_node_compare_document_order(
               (sxml_item_pair){.first = expected.first,
                                .second = expected.second},
               &ordering) == SXML_STATUS_OK &&
           ordering == expected.ordering;
}

static void test_json_roots_describe_their_value(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item map = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_two_entry_map(fixture.item_heap,
                                                (two_entry_map){
                                                    .first_key = "a",
                                                    .first_value = 1,
                                                    .second_key = "b",
                                                    .second_value = 2},
                                                &map) == SXML_STATUS_OK);
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_small_array(fixture.item_heap, &array) ==
                                 SXML_STATUS_OK);
    sxml_sequence three_integers = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_sequence_make_integer_range(
                                 fixture.item_heap,
                                 (sxml_integer_range){.first_integer = 1,
                                                      .last_integer = 3},
                                 &three_integers) == SXML_STATUS_OK);
    typedef struct root_case {
        sxml_sequence value;
        uint64_t child_count;
    } root_case;
    const root_case root_cases[] = {
        {sxml_sequence_from_item(map), 2},
        {sxml_sequence_from_item(array), 3},
        {three_integers, 3},
        {sxml_sequence_from_item(sxml_atomic_item_from_integer(7)), 0},
        {sxml_sequence_empty(), 0},
    };
    sxml_tree_identifier previous_tree = {.value = 0};
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(root_cases);
         case_index += 1) {
        sxml_item root = {.representation = 0};
        SXML_TEST_REQUIRE(state, sxml_json_node_make_root(
                                     fixture.item_heap,
                                     root_cases[case_index].value, &root) ==
                                     SXML_STATUS_OK);
        sxml_json_node_description description;
        SXML_TEST_REQUIRE(state, sxml_json_node_describe(root, &description) ==
                                     SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, !description.has_parent);
        SXML_TEST_EXPECT(state, description.json_key.representation == 0);
        SXML_TEST_EXPECT(state, description.json_value.content.payload.bits ==
                                    root_cases[case_index]
                                        .value.content.payload.bits);
        SXML_TEST_EXPECT(state, description.child_count ==
                                    root_cases[case_index].child_count);
        SXML_TEST_EXPECT(state, description.json_tree.value >
                                    previous_tree.value);
        previous_tree = description.json_tree;
        /* Past the last child. */
        sxml_item child = {.representation = 0};
        SXML_TEST_EXPECT(state,
                         sxml_json_node_make_child(
                             fixture.item_heap,
                             (sxml_json_child_request){
                                 .json_node = root,
                                 .child_index = description.child_count},
                             &child) == SXML_STATUS_INDEX_OUT_OF_RANGE);
        sxml_item_release(root);
    }
    /* Not JNodes. */
    sxml_json_node_description description;
    SXML_TEST_EXPECT(state, sxml_json_node_describe(map, &description) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_item child = {.representation = 0};
    SXML_TEST_EXPECT(state, sxml_json_node_make_child(
                                fixture.item_heap,
                                (sxml_json_child_request){.json_node = array,
                                                          .child_index = 0},
                                &child) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_item_release(map);
    sxml_item_release(array);
    sxml_sequence_release(three_integers);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

static void test_json_children_have_keys_and_values(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item map = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_two_entry_map(fixture.item_heap,
                                                (two_entry_map){
                                                    .first_key = "a",
                                                    .first_value = 1,
                                                    .second_key = "b",
                                                    .second_value = 2},
                                                &map) == SXML_STATUS_OK);
    sxml_item root = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_root_over_item(fixture.item_heap, map,
                                                 &root) == SXML_STATUS_OK);
    sxml_item second = make_child_or_none(
        fixture.item_heap,
        (sxml_json_child_request){.json_node = root, .child_index = 1});
    sxml_json_node_description description;
    SXML_TEST_REQUIRE(state, sxml_json_node_describe(second, &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.has_parent);
    SXML_TEST_EXPECT(state, is_same(description.parent, root));
    SXML_TEST_EXPECT(state, item_has_text(description.json_key,
                                          (expected_text){
                                              .type = SXML_TYPE_XS_STRING,
                                              .text = "b"}));
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_atomic_equal(
                                description.json_value.content,
                                sxml_atomic_item_from_integer(2)));
    SXML_TEST_EXPECT(state, description.child_index == 1);
    SXML_TEST_EXPECT(state, description.child_count == 0);
    /* The child keeps its parent and the map alive. */
    sxml_item_release(root);
    sxml_item_release(map);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) > 0);
    sxml_item_release(second);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);

    /* Under a sequence: each item, keyed by its position. */
    sxml_sequence three_integers = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, sxml_sequence_make_integer_range(
                                 fixture.item_heap,
                                 (sxml_integer_range){.first_integer = 10,
                                                      .last_integer = 12},
                                 &three_integers) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_json_node_make_root(fixture.item_heap,
                                                      three_integers,
                                                      &root) == SXML_STATUS_OK);
    sxml_sequence_release(three_integers);
    sxml_item last = make_child_or_none(
        fixture.item_heap,
        (sxml_json_child_request){.json_node = root, .child_index = 2});
    SXML_TEST_REQUIRE(state, sxml_json_node_describe(last, &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_atomic_equal(
                                description.json_key,
                                sxml_atomic_item_from_integer(3)));
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_atomic_equal(
                                description.json_value.content,
                                sxml_atomic_item_from_integer(12)));
    sxml_item_release(last);
    sxml_item_release(root);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

/* QT4 op-union-406: jtree({"a":1, "b":2}) union jtree({"b":2, "a":1}) has
   two nodes. */
static void test_op_union_406(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item first_map = {.representation = 0};
    sxml_item second_map = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_two_entry_map(fixture.item_heap,
                                                (two_entry_map){
                                                    .first_key = "a",
                                                    .first_value = 1,
                                                    .second_key = "b",
                                                    .second_value = 2},
                                                &first_map) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, make_two_entry_map(fixture.item_heap,
                                                (two_entry_map){
                                                    .first_key = "b",
                                                    .first_value = 2,
                                                    .second_key = "a",
                                                    .second_value = 1},
                                                &second_map) ==
                                 SXML_STATUS_OK);
    sxml_item first = {.representation = 0};
    sxml_item second = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_root_over_item(fixture.item_heap, first_map,
                                                 &first) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, make_root_over_item(fixture.item_heap, second_map,
                                                 &second) == SXML_STATUS_OK);
    /* Union keeps distinct nodes in document order: two. */
    SXML_TEST_EXPECT(state, !is_same(first, second));
    SXML_TEST_EXPECT(state, is_same(first, first));
    SXML_TEST_EXPECT(state, is_in_order((expected_order){.first = first, .second = second, .ordering = SXML_ORDERING_LESS}));
    SXML_TEST_EXPECT(state, is_in_order((expected_order){.first = second, .second = first, .ordering = SXML_ORDERING_GREATER}));
    /* jtree applied twice to one map makes two JTrees too. */
    sxml_item again = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_root_over_item(fixture.item_heap, first_map,
                                                 &again) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !is_same(first, again));
    sxml_item_release(again);
    sxml_item_release(first);
    sxml_item_release(second);
    sxml_item_release(first_map);
    sxml_item_release(second_map);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

/* QT4 fn-root-29: $j is the root of the third child of $j, for
   $j := jtree([1,2,3]). */
static void test_fn_root_29(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_small_array(fixture.item_heap, &array) ==
                                 SXML_STATUS_OK);
    sxml_item json_tree = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_root_over_item(fixture.item_heap, array,
                                                 &json_tree) == SXML_STATUS_OK);
    sxml_item_release(array);
    sxml_item third = make_child_or_none(
        fixture.item_heap,
        (sxml_json_child_request){.json_node = json_tree, .child_index = 2});
    /* root(): follow ·jparent· to the node that has none. */
    sxml_item root = third;
    sxml_json_node_description description;
    SXML_TEST_REQUIRE(state, sxml_json_node_describe(root, &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_atomic_equal(
                                description.json_key,
                                sxml_atomic_item_from_integer(3)));
    while (description.has_parent) {
        root = description.parent;
        SXML_TEST_REQUIRE(state, sxml_json_node_describe(
                                     root, &description) == SXML_STATUS_OK);
    }
    SXML_TEST_EXPECT(state, is_same(json_tree, root));
    SXML_TEST_EXPECT(state, !is_same(json_tree, third));
    /* The same child made again is the same node, in one place. */
    sxml_item third_again = make_child_or_none(
        fixture.item_heap,
        (sxml_json_child_request){.json_node = json_tree, .child_index = 2});
    SXML_TEST_EXPECT(state, third_again.payload.cell != third.payload.cell);
    SXML_TEST_EXPECT(state, is_same(third, third_again));
    SXML_TEST_EXPECT(state, is_in_order((expected_order){.first = third, .second = third_again, .ordering = SXML_ORDERING_EQUAL}));
    sxml_item_release(third_again);
    sxml_item_release(third);
    sxml_item_release(json_tree);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

static void test_generic_nodes_in_document_order(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    built_tree built;
    SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                 SXML_STATUS_OK);
    /* {"a": [1, 2, 3], "b": 2}, made by putting the array under "a". */
    sxml_item map = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_two_entry_map(fixture.item_heap,
                                                (two_entry_map){
                                                    .first_key = "a",
                                                    .first_value = 1,
                                                    .second_key = "b",
                                                    .second_value = 2},
                                                &map) == SXML_STATUS_OK);
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_small_array(fixture.item_heap, &array) ==
                                 SXML_STATUS_OK);
    sxml_map_entry first_entry;
    SXML_TEST_REQUIRE(state, sxml_map_get_entry(map, 0, &first_entry) ==
                                 SXML_STATUS_OK);
    sxml_item nested_map = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_map_put(fixture.item_heap,
                                          &(sxml_map_update){
                                              .map = map,
                                              .key = first_entry.key,
                                              .value = sxml_sequence_from_item(
                                                  array)},
                                          &nested_map) == SXML_STATUS_OK);
    sxml_item_release(array);
    sxml_item_release(map);
    sxml_item root = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_root_over_item(fixture.item_heap, nested_map,
                                                 &root) == SXML_STATUS_OK);
    sxml_item_release(nested_map);
    sxml_item key_a = make_child_or_none(
        fixture.item_heap,
        (sxml_json_child_request){.json_node = root, .child_index = 0});
    sxml_item key_b = make_child_or_none(
        fixture.item_heap,
        (sxml_json_child_request){.json_node = root, .child_index = 1});
    sxml_item member_one = make_child_or_none(
        fixture.item_heap,
        (sxml_json_child_request){.json_node = key_a, .child_index = 0});
    sxml_item member_three = make_child_or_none(
        fixture.item_heap,
        (sxml_json_child_request){.json_node = key_a, .child_index = 2});
    /* Preorder: root, a, a/1, a/3, b. */
    const sxml_item in_order[] = {root, key_a, member_one, member_three,
                                  key_b};
    for (size_t first_index = 0; first_index < SXML_TEST_COUNT(in_order);
         first_index += 1) {
        for (size_t second_index = 0; second_index < SXML_TEST_COUNT(in_order);
             second_index += 1) {
            sxml_ordering expected =
                first_index < second_index
                    ? SXML_ORDERING_LESS
                    : (first_index > second_index ? SXML_ORDERING_GREATER
                                                  : SXML_ORDERING_EQUAL);
            SXML_TEST_EXPECT(state, is_in_order((expected_order){.first = in_order[first_index], .second = in_order[second_index], .ordering = expected}));
            SXML_TEST_EXPECT(state, is_same(in_order[first_index],
                                            in_order[second_index]) ==
                                        (first_index == second_index));
        }
    }
    /* XNodes and JNodes: by tree identifier; the document came first. */
    sxml_item element = sxml_node_item_make(node_at(&built, TEST_R_ELEMENT));
    sxml_item text = sxml_node_item_make(node_at(&built, TEST_HE_TEXT));
    SXML_TEST_EXPECT(state, is_in_order((expected_order){.first = element, .second = member_one, .ordering = SXML_ORDERING_LESS}));
    SXML_TEST_EXPECT(state, is_in_order((expected_order){.first = root, .second = text, .ordering = SXML_ORDERING_GREATER}));
    SXML_TEST_EXPECT(state, is_in_order((expected_order){.first = element, .second = text, .ordering = SXML_ORDERING_LESS}));
    SXML_TEST_EXPECT(state, !is_same(element, root));
    SXML_TEST_EXPECT(state, !is_same(element, text));
    sxml_item element_again = sxml_node_item_make(node_at(&built, TEST_R_ELEMENT));
    SXML_TEST_EXPECT(state, is_same(element, element_again));
    /* Not generic nodes. */
    sxml_item integer = sxml_atomic_item_from_integer(1);
    SXML_TEST_EXPECT(state, !is_same(integer, integer));
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    SXML_TEST_EXPECT(state, sxml_generic_node_compare_document_order(
                                (sxml_item_pair){.first = root,
                                                 .second = integer},
                                &ordering) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_item_release(element_again);
    sxml_item_release(element);
    sxml_item_release(text);
    sxml_item_release(member_three);
    sxml_item_release(member_one);
    sxml_item_release(key_b);
    sxml_item_release(key_a);
    sxml_item_release(root);
    built_tree_destroy(&built);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

/* A JTree TEST_NESTING_DEPTH arrays deep: navigation to the bottom, order
   against the root, and release of the chain in constant stack. */
static void test_deep_json_trees(sxml_test_state *state) {
    node_fixture fixture;
    SXML_TEST_REQUIRE(state, node_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item value = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_array_make(fixture.item_heap,
                                             (sxml_sequence_view){
                                                 .sequences = NULL,
                                                 .sequence_count = 0},
                                             &value) == SXML_STATUS_OK);
    for (size_t level = 0; level < TEST_NESTING_DEPTH; level += 1) {
        const sxml_sequence member = sxml_sequence_from_item(value);
        sxml_item outer = {.representation = 0};
        SXML_TEST_REQUIRE(state, sxml_array_make(fixture.item_heap,
                                                 (sxml_sequence_view){
                                                     .sequences = &member,
                                                     .sequence_count = 1},
                                                 &outer) == SXML_STATUS_OK);
        sxml_item_release(value);
        value = outer;
    }
    sxml_item root = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_root_over_item(fixture.item_heap, value,
                                                 &root) == SXML_STATUS_OK);
    sxml_item_release(value);
    sxml_item bottom = root;
    sxml_item_retain(bottom);
    for (size_t level = 0; level < TEST_NESTING_DEPTH; level += 1) {
        sxml_item child = make_child_or_none(
            fixture.item_heap,
            (sxml_json_child_request){.json_node = bottom, .child_index = 0});
        SXML_TEST_REQUIRE(state, child.representation != 0);
        sxml_item_release(bottom);
        bottom = child;
    }
    sxml_json_node_description description;
    SXML_TEST_REQUIRE(state, sxml_json_node_describe(bottom, &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.child_count == 0);
    SXML_TEST_EXPECT(state, is_in_order((expected_order){.first = root, .second = bottom, .ordering = SXML_ORDERING_LESS}));
    SXML_TEST_EXPECT(state, is_in_order((expected_order){.first = bottom, .second = root, .ordering = SXML_ORDERING_GREATER}));
    sxml_item_release(root);
    sxml_item_release(bottom);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    node_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Allocation failures */

/* Every accessor that allocates, once; the first failure is returned and
   everything made is released. */
static sxml_status use_every_accessor(const node_fixture *fixture,
                                      const built_tree *built) {
    sxml_item string_value = {.representation = 0};
    sxml_status status = sxml_node_make_string_value(
        fixture->item_heap, node_at(built, TEST_R_ELEMENT), &string_value);
    sxml_item_release(string_value);
    sxml_sequence base_uri = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_node_make_base_uri(
            fixture->item_heap,
            &(sxml_node_value_request){.node = node_at(built, TEST_C_ELEMENT),
                                       .error_record = NULL},
            &base_uri);
    }
    sxml_sequence_release(base_uri);
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = make_two_entry_map(fixture->item_heap,
                                    (two_entry_map){.first_key = "a",
                                                    .first_value = 1,
                                                    .second_key = "b",
                                                    .second_value = 2},
                                    &map);
    }
    sxml_item root = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = make_root_over_item(fixture->item_heap, map, &root);
    }
    sxml_item child = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_json_node_make_child(
            fixture->item_heap,
            (sxml_json_child_request){.json_node = root, .child_index = 1},
            &child);
    }
    sxml_item_release(child);
    sxml_item_release(root);
    sxml_item_release(map);
    return status;
}

static void
test_accessors_survive_every_allocation_failure(sxml_test_state *state) {
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
        node_fixture fixture;
        SXML_TEST_REQUIRE(
            state, node_fixture_create_with(
                       sxml_counting_allocator_as_allocator(counting_allocator),
                       &fixture) == SXML_STATUS_OK);
        built_tree built;
        SXML_TEST_REQUIRE(state, build_fixed_document(&fixture, &built) ==
                                     SXML_STATUS_OK);
        sxml_status status = use_every_accessor(&fixture, &built);
        built_tree_destroy(&built);
        SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
        SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
        node_fixture_destroy(&fixture);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 100);
    }
    /* String value: concatenation and item; base URI: the xml:base list,
       two resolutions and the item; two keys, the map, root and child. */
    SXML_TEST_EXPECT(state, failing_allocation_number == 12);
}

static const sxml_test_case xdm_node_test_cases[] = {
    {.name = "node_items_hold_their_tree",
     .function = test_node_items_hold_their_tree},
    {.name = "string_and_typed_values",
     .function = test_string_and_typed_values},
    {.name = "validated_typed_values_come_from_the_tree",
     .function = test_validated_typed_values_come_from_the_tree},
    {.name = "base_uris_follow_xml_base",
     .function = test_base_uris_follow_xml_base},
    {.name = "reference_resolution_examples",
     .function = test_reference_resolution_examples},
    {.name = "json_roots_describe_their_value",
     .function = test_json_roots_describe_their_value},
    {.name = "json_children_have_keys_and_values",
     .function = test_json_children_have_keys_and_values},
    {.name = "op_union_406", .function = test_op_union_406},
    {.name = "fn_root_29", .function = test_fn_root_29},
    {.name = "generic_nodes_in_document_order",
     .function = test_generic_nodes_in_document_order},
    {.name = "deep_json_trees", .function = test_deep_json_trees},
    {.name = "accessors_survive_every_allocation_failure",
     .function = test_accessors_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xdm_node",
            .cases = xdm_node_test_cases,
            .case_count = SXML_TEST_COUNT(xdm_node_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
