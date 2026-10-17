/*
 * Tests for the XML and XHTML output methods (src/serialize_xml.c,
 * src/serialize.c; Serialization 4.0, sections 2, 4, 5, 6 and 12; D120):
 * sequence normalization, escaping, encodings, line endings, indentation,
 * CDATA sections, declarations, namespaces, character maps, Unicode
 * normalization, Canonical XML and the rules of XHTML.
 *
 * Golden file: tests/serialize_test.c (fixture, buffer comparison,
 * allocation-failure loop). Documents are parsed with the XML parser into
 * arena trees; expected texts follow the QT4 method-xml and method-xhtml
 * test sets where those decide the form. An expected text "!CODE" means
 * the serialization raises CODE.
 */
#include "sxml/serialize.h"

#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"

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
    sxml_output_buffer *output_buffer;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
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
            sxml_default_allocator(),
            &(sxml_item_heap_configuration){.allocator = fixture_allocator(test_fixture),
                                            .name_pool = test_fixture->name_pool,
                                            .type_registry = test_fixture->registry,
                                            .maximum_byte_count = 0},
            &test_fixture->item_heap);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    return status;
}

/* Destroys the fixture; true when every cell and block was released. */
static bool fixture_destroy(fixture *test_fixture) {
    bool is_clean = true;
    if (test_fixture->item_heap != NULL) {
        sxml_item_heap_clear_raised_error(test_fixture->item_heap);
        const uint64_t live_cell_count =
            sxml_item_heap_read_counts(test_fixture->item_heap).live_cell_count;
        if (live_cell_count != 0) {
            (void)fprintf(stderr, "leak: %llu cells\n", (unsigned long long)live_cell_count);
        }
        is_clean = live_cell_count == 0;
    }
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    sxml_item_heap_destroy(test_fixture->item_heap);
    sxml_type_registry_destroy(test_fixture->registry);
    sxml_name_pool_destroy(test_fixture->name_pool);
    if (test_fixture->counting_allocator != NULL) {
        const uint64_t live_allocation_count =
            sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
                .live_allocation_count;
        if (live_allocation_count != 0) {
            (void)fprintf(stderr, "leak: %llu blocks\n",
                          (unsigned long long)live_allocation_count);
        }
        is_clean = is_clean && live_allocation_count == 0;
    }
    sxml_counting_allocator_finalize(test_fixture->counting_allocator);
    return is_clean;
}

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* ------------------------------------------------------- Making values */

/* Parses xml_text into a document node item (owned). */
static sxml_status parse_document(fixture *test_fixture, const char *xml_text,
                                  sxml_item *document) {
    sxml_xml_parser *parser = NULL;
    sxml_status status = sxml_xml_parser_create(
        fixture_allocator(test_fixture),
        &(sxml_xml_parser_configuration){.allocator = fixture_allocator(test_fixture),
                                         .name_pool = test_fixture->name_pool},
        &parser);
    sxml_arena_tree *arena_tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_create(
            fixture_allocator(test_fixture),
            &(sxml_arena_tree_configuration){.allocator = fixture_allocator(test_fixture),
                                             .item_heap = test_fixture->item_heap,
                                             .duplicate_attribute_policy =
                                                 SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                                             .is_destroyed_when_unreferenced = true},
            &arena_tree);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(parser,
                                       &(sxml_xml_parse_request){.input = text_view(xml_text)},
                                       sxml_arena_tree_as_tree_builder(arena_tree));
    }
    sxml_xml_parser_destroy(parser);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* A node item (owned) of the tree of document, by node index. */
static sxml_item node_of(sxml_item document, uint32_t node_index) {
    sxml_node_reference node = {.tree = NULL};
    if (sxml_item_get_node(document, &node) != SXML_STATUS_OK) {
        return sxml_atomic_item_false();
    }
    node.node_index.index = node_index;
    return sxml_node_item_make(node);
}

static sxml_item make_string(fixture *test_fixture, const char *text) {
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    sxml_item item = {.representation = 0};
    if (sxml_string_from_utf8(text_view(text), &string) != SXML_STATUS_OK ||
        sxml_atomic_item_make(
            test_fixture->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING, .datum_details = {.string = string}}},
            &item) != SXML_STATUS_OK) {
        return sxml_atomic_item_false();
    }
    return item;
}

/* The sequence of items (owned: taken over). */
static sxml_sequence make_sequence(fixture *test_fixture, const sxml_item *items,
                                   size_t item_count) {
    sxml_sequence sequence = sxml_sequence_empty();
    const sxml_status status = sxml_sequence_make_vector(
        test_fixture->item_heap, (sxml_item_view){.items = items, .item_count = item_count},
        &sequence);
    for (size_t item_index = 0; item_index < item_count; item_index += 1) {
        sxml_item_release(items[item_index]);
    }
    return status == SXML_STATUS_OK ? sequence : sxml_sequence_empty();
}

static sxml_name_identifier intern_name(fixture *test_fixture, const char *namespace_uri,
                                        const char *local_name) {
    sxml_namespace_uri_identifier uri = {.index = 0};
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_namespace_uri(test_fixture->name_pool, text_view(namespace_uri),
                                            &uri) != SXML_STATUS_OK ||
        sxml_name_pool_intern_name(
            test_fixture->name_pool,
            &(sxml_name_request){.namespace_uri = uri, .local_name = text_view(local_name)},
            &name) != SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return name;
}

/* --------------------------------------------------------- Serializing */

/* Serializes value (borrowed) and compares the bytes with expected, or
   the error code with the code after "!" in expected. */
typedef struct serialization_case {
    sxml_sequence value;
    sxml_serialization_parameters parameters;
} serialization_case;

static bool serialization_gives(fixture *test_fixture, const serialization_case *test_case,
                                sxml_byte_view expected) {
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_error_record error_record = {.value = NULL};
    const sxml_status status = sxml_serialize_sequence(
        test_fixture->item_heap,
        &(sxml_serialization_request){.value = test_case->value,
                                      .serialization_parameters = test_case->parameters,
                                      .error_record = &error_record},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    const sxml_byte_view bytes = sxml_output_buffer_get_bytes(test_fixture->output_buffer);
    if (expected.byte_count > 0 && expected.bytes[0] == '!') {
        const sxml_byte_view code = {.bytes = expected.bytes + 1,
                                     .byte_count = expected.byte_count - 1};
        const bool is_expected_error = status == SXML_STATUS_DYNAMIC_ERROR &&
                                       sxml_byte_view_equals(error_record.code.local_name, code);
        if (!is_expected_error) {
            (void)fprintf(stderr, "expected error %.*s, got status %d (%.*s), output \"%.*s\"\n",
                          (int)code.byte_count, (const char *)code.bytes, (int)status,
                          status == SXML_STATUS_DYNAMIC_ERROR
                              ? (int)error_record.code.local_name.byte_count
                              : 0,
                          status == SXML_STATUS_DYNAMIC_ERROR
                              ? (const char *)error_record.code.local_name.bytes
                              : "",
                          (int)bytes.byte_count, (const char *)bytes.bytes);
        }
        return is_expected_error;
    }
    const bool is_equal = status == SXML_STATUS_OK && sxml_byte_view_equals(bytes, expected);
    if (!is_equal) {
        (void)fprintf(stderr, "status %d (%.*s): expected \"%.*s\", got \"%.*s\"\n", (int)status,
                      status == SXML_STATUS_DYNAMIC_ERROR
                          ? (int)error_record.code.local_name.byte_count
                          : 0,
                      status == SXML_STATUS_DYNAMIC_ERROR
                          ? (const char *)error_record.code.local_name.bytes
                          : "",
                      (int)expected.byte_count, (const char *)expected.bytes,
                      (int)bytes.byte_count, (const char *)bytes.bytes);
    }
    return is_equal;
}

/* A document to parse and serialize with parameters. */
typedef struct document_case {
    const char *xml_text;
    sxml_serialization_parameters parameters;
} document_case;

static bool document_gives(fixture *test_fixture, const document_case *test_case,
                           const char *expected) {
    sxml_item document = {.representation = 0};
    if (parse_document(test_fixture, test_case->xml_text, &document) != SXML_STATUS_OK) {
        (void)fprintf(stderr, "cannot parse %s\n", test_case->xml_text);
        return false;
    }
    const sxml_sequence value = sxml_sequence_from_item(document);
    const bool is_expected = serialization_gives(
        test_fixture, &(serialization_case){.value = value, .parameters = test_case->parameters},
        text_view(expected));
    sxml_item_release(document);
    return is_expected;
}

/* The XML method with the defaults of fn:serialize. */
#define XML_METHOD .serialization_method = SXML_SERIALIZATION_METHOD_XML
#define XHTML_METHOD .serialization_method = SXML_SERIALIZATION_METHOD_XHTML

/* --------------------------------------------- Sequence normalization */

static void test_sequence_normalization(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, parse_document(current, "<doc><e/>t</doc>", &document) ==
                                 SXML_STATUS_OK);
    /* Serialization 4.0, section 2: adjacent atomic items with a space,
       nodes copied, document nodes replaced by their children. */
    sxml_item_retain(document);
    sxml_sequence mixed = make_sequence(
        current,
        (const sxml_item[]){sxml_atomic_item_from_integer(1), sxml_atomic_item_from_integer(2),
                            make_string(current, "x"), node_of(document, 2),
                            sxml_atomic_item_from_integer(3), document},
        6);
    SXML_TEST_EXPECT(state, serialization_gives(current,
                                                &(serialization_case){.value = mixed,
                                                                      .parameters = {XML_METHOD}},
                                                text_view("1 2 x<e/>3<doc><e/>t</doc>")));
    /* With an item separator between every two items (K2-Serialization-14,
       Serialization-xml-25). */
    SXML_TEST_EXPECT(state,
                     serialization_gives(current,
                                         &(serialization_case){
                                             .value = mixed,
                                             .parameters = {XML_METHOD, .has_item_separator = true,
                                                            .item_separator = text_view("|")}},
                                         text_view("1|2|x|<e/>|3|<doc><e/>t</doc>")));
    sxml_sequence_release(mixed);
    /* Arrays are flattened (Serialization-xml-01). */
    const sxml_sequence members[] = {
        sxml_sequence_from_item(sxml_atomic_item_from_integer(1)),
        make_sequence(current,
                      (const sxml_item[]){sxml_atomic_item_from_integer(2),
                                          sxml_atomic_item_from_integer(3)},
                      2)};
    sxml_item array = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_array_make(current->item_heap,
                                             (sxml_sequence_view){.sequences = members,
                                                                  .sequence_count = 2},
                                             &array) == SXML_STATUS_OK);
    sxml_sequence_release(members[1]);
    sxml_sequence array_value = sxml_sequence_from_item(array);
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){.value = array_value,
                                                      .parameters = {XML_METHOD,
                                                                     .has_item_separator = true,
                                                                     .item_separator =
                                                                         text_view("|")}},
                                text_view("1|2|3")));
    sxml_sequence_release(array_value);
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){.value = sxml_sequence_empty(),
                                                      .parameters = {XML_METHOD}},
                                text_view("")));
    /* SENR0001 for attributes, maps (Serialization-xml-02) and functions. */
    sxml_item attribute_document = {.representation = 0};
    SXML_TEST_REQUIRE(state, parse_document(current, "<a b='1'/>", &attribute_document) ==
                                 SXML_STATUS_OK);
    sxml_sequence attribute = sxml_sequence_from_item(node_of(attribute_document, 2));
    SXML_TEST_EXPECT(state, serialization_gives(current,
                                                &(serialization_case){.value = attribute,
                                                                      .parameters = {XML_METHOD}},
                                                text_view("!SENR0001")));
    sxml_sequence_release(attribute);
    sxml_item map = {.representation = 0};
    SXML_TEST_REQUIRE(
        state, sxml_map_make(current->item_heap,
                             &(sxml_map_request){.map_entries = {.map_entries = NULL,
                                                                 .map_entry_count = 0},
                                                 .duplicate_key_policy =
                                                     SXML_DUPLICATE_KEY_POLICY_REJECT,
                                                 .error_record = NULL},
                             &map) == SXML_STATUS_OK);
    sxml_sequence map_value = sxml_sequence_from_item(map);
    SXML_TEST_EXPECT(state, serialization_gives(current,
                                                &(serialization_case){.value = map_value,
                                                                      .parameters = {XML_METHOD}},
                                                text_view("!SENR0001")));
    sxml_sequence_release(map_value);
    sxml_item_release(attribute_document);
    sxml_item_release(document);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------- Escaping, encodings */

static void test_escaping(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* Section 5: &, <, > and CR, NEL, LINE SEPARATOR in text; also ", TAB,
       LF in attributes (K2-Serialization-5, -6). */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<a b='&quot;&#9;&#10;&#xD;&lt;'>"
                                                           "&amp;&lt;&gt;&#xD;\xC2\x85\xE2\x80\xA8"
                                                           "</a>",
                                               .parameters = {XML_METHOD}},
                                           "<a b=\"&quot;&#x9;&#xA;&#xD;&lt;\">&amp;&lt;&gt;"
                                           "&#xD;&#x85;&#x2028;</a>"));
    /* Control characters are references (K2-Serialization-9, -10,
       Serialization-xml-27, -30); C0 controls only with version 1.1, else
       SERE0006 (5.1.2, QT4 serialize-xml-162). */
    sxml_sequence controls =
        sxml_sequence_from_item(make_string(current, "\x01x\x7F\xC2\x80\xC2\x9F"));
    SXML_TEST_EXPECT(state,
                     serialization_gives(
                         current,
                         &(serialization_case){
                             .value = controls,
                             .parameters = {XML_METHOD, .has_version = true,
                                            .version = SXML_BYTE_VIEW_FROM_LITERAL("1.1")}},
                         text_view("&#x1;x&#x7F;&#x80;&#x9F;")));
    SXML_TEST_EXPECT(state, serialization_gives(current,
                                                &(serialization_case){.value = controls,
                                                                      .parameters = {XML_METHOD}},
                                                text_view("!SERE0006")));
    sxml_sequence_release(controls);
    /* Comments and processing instructions are not escaped. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text =
                                                                "<a><!--<&>--><?pi x<y?><?q?></a>",
                                                            .parameters = {XML_METHOD}},
                                           "<a><!--<&>--><?pi x<y?><?q?></a>"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_encodings(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* Section 5.1.3: a reference where XML allows one, SERE0008 elsewhere. */
    const sxml_serialization_parameters ascii = {
        XML_METHOD, .character_encoding = SXML_CHARACTER_ENCODING_US_ASCII};
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<a b='\xC3\xA9'>"
                                                                        "\xC3\xA9\xF0\x9F\x98\x80"
                                                                        "</a>",
                                                            .parameters = ascii},
                                           "<a b=\"&#xE9;\">&#xE9;&#x1F600;</a>"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<a><!--\xC3\xA9--></a>",
                                                            .parameters = ascii},
                                           "!SERE0008"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<\xC3\xA9/>",
                                                            .parameters = ascii},
                                           "!SERE0008"));
    SXML_TEST_EXPECT(state,
                     document_gives(current,
                                    &(document_case){.xml_text = "<a>\xC3\xA9\xE2\x82\xAC</a>",
                                                     .parameters = {XML_METHOD,
                                                                    .character_encoding =
                                                                        SXML_CHARACTER_ENCODING_ISO_8859_1,
                                                                    .omit_xml_declaration =
                                                                        SXML_SERIALIZATION_FLAG_NO}},
                                    "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a>\xE9&#x20AC;</a>"));
    /* UTF-16, big endian, with a byte order mark and a surrogate pair. */
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, parse_document(current, "<a>\xF0\x9F\x98\x80</a>", &document) ==
                                 SXML_STATUS_OK);
    sxml_sequence value = sxml_sequence_from_item(document);
    static const uint8_t utf16[] = {0xFE, 0xFF, 0x00, '<',  0x00, 'a',  0x00, '>',  0xD8, 0x3D,
                                    0xDE, 0x00, 0x00, '<',  0x00, '/',  0x00, 'a',  0x00, '>'};
    SXML_TEST_EXPECT(state,
                     serialization_gives(
                         current,
                         &(serialization_case){
                             .value = value,
                             .parameters = {XML_METHOD,
                                            .character_encoding = SXML_CHARACTER_ENCODING_UTF_16,
                                            .byte_order_mark = SXML_SERIALIZATION_FLAG_YES}},
                         (sxml_byte_view){.bytes = utf16, .byte_count = sizeof(utf16)}));
    static const uint8_t utf8[] = {0xEF, 0xBB, 0xBF, '<', 'a', '>', 0xF0, 0x9F,
                                   0x98, 0x80, '<',  '/', 'a', '>'};
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){
                                    .value = value,
                                    .parameters = {XML_METHOD,
                                                   .byte_order_mark = SXML_SERIALIZATION_FLAG_YES}},
                                (sxml_byte_view){.bytes = utf8, .byte_count = sizeof(utf8)}));
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_line_endings(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* Section 5.1.7: every newline written, also in comments, becomes the
       line ending; attribute newlines stay references (Serialization-xml-16,
       -19, -21). */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<out a='x&#10;y'>x\ny<!--a\nb--></out>",
                                               .parameters =
                                                   {XML_METHOD,
                                                    .line_ending =
                                                        SXML_LINE_ENDING_CARRIAGE_RETURN_LINE_FEED}},
                                           "<out a=\"x&#xA;y\">x\r\ny<!--a\r\nb--></out>"));
    /* Serialization-xml-17. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<out><in/></out>",
                                               .parameters = {XML_METHOD,
                                                              .indent = SXML_SERIALIZATION_FLAG_YES,
                                                              .has_indent_unit = true,
                                                              .indent_unit = text_view(""),
                                                              .line_ending =
                                                                  SXML_LINE_ENDING_CARRIAGE_RETURN}},
                                           "<out>\r<in/>\r</out>"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------- Indentation */

static void test_indentation(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    const sxml_serialization_parameters indent = {XML_METHOD,
                                                  .indent = SXML_SERIALIZATION_FLAG_YES};
    /* Serialization-xml-09: two spaces by default; whitespace-only text is
       replaced. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<out> <mid><in/><!--c--></mid></out>",
                                               .parameters = indent},
                                           "<out>\n  <mid>\n    <in/>\n    <!--c-->\n  </mid>\n"
                                           "</out>"));
    /* Serialization-xml-06: a tab as the unit. */
    sxml_serialization_parameters tab = indent;
    tab.has_indent_unit = true;
    tab.indent_unit = text_view("\t");
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<out><in/></out>",
                                                            .parameters = tab},
                                           "<out>\n\t<in/>\n</out>"));
    /* Mixed content and xml:space="preserve" are left alone
       (K2-Serialization-40). */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<t><p>a<b><c/></b>d</p><a xml:space="
                                                           "'preserve'> <x/></a><b/></t>",
                                               .parameters = indent},
                                           "<t>\n  <p>a<b><c/></b>d</p>\n  <a xml:space=\""
                                           "preserve\"> <x/></a>\n  <b/>\n</t>"));
    /* suppress-indentation covers the whole content (K2-Serialization-26). */
    sxml_serialization_parameters suppress = indent;
    const sxml_name_identifier para = intern_name(current, "", "para");
    suppress.suppress_indentation =
        (sxml_element_name_view){.element_names = &para, .element_name_count = 1};
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<c><para><b><B/></b><i/></para></c>",
                                               .parameters = suppress},
                                           "<c>\n  <para><b><B/></b><i/></para>\n</c>"));
    /* With an XML declaration; indent-attributes (Serialization-xml-12). */
    sxml_serialization_parameters attributes = indent;
    attributes.indent_attributes = SXML_SERIALIZATION_FLAG_YES;
    attributes.omit_xml_declaration = SXML_SERIALIZATION_FLAG_NO;
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<out a='1' b='2'><in/></out>",
                                                            .parameters = attributes},
                                           "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                           "<out a=\"1\"\n  b=\"2\">\n  <in/>\n</out>"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------ CDATA */

static void test_cdata_sections(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    const sxml_name_identifier names[] = {intern_name(current, "", "b"),
                                          intern_name(current, "urn:p", "b")};
    const sxml_serialization_parameters cdata = {
        XML_METHOD,
        .cdata_section_elements = {.element_names = names, .element_name_count = 2}};
    /* K2-Serialization-30, -33, -34. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<a xmlns:p='urn:p'><b>bold <!--c--> "
                                                           "x</b><p:b>&lt;[[]]&gt;</p:b><i>i</i></a>",
                                               .parameters = cdata},
                                           "<a xmlns:p=\"urn:p\"><b><![CDATA[bold ]]><!--c-->"
                                           "<![CDATA[ x]]></b><p:b><![CDATA[<[[]]]]><![CDATA[>]]>"
                                           "</p:b><i>i</i></a>"));
    /* K2-Serialization-35: a character the encoding lacks closes the
       section. */
    sxml_serialization_parameters ascii = cdata;
    ascii.character_encoding = SXML_CHARACTER_ENCODING_US_ASCII;
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text =
                                                                "<a><b>bold\xC2\xA0" "as</b></a>",
                                                            .parameters = ascii},
                                           "<a><b><![CDATA[bold]]>&#xA0;<![CDATA[as]]></b></a>"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------- Declarations and doctype */

static void test_declarations(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    const sxml_serialization_flag omits_not = SXML_SERIALIZATION_FLAG_NO;
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<a/>",
                                               .parameters = {XML_METHOD,
                                                              .omit_xml_declaration = omits_not,
                                                              .standalone = SXML_STANDALONE_YES}},
                                           "<?xml version=\"1.0\" encoding=\"UTF-8\" "
                                           "standalone=\"yes\"?><a/>"));
    /* Section 5.1.9: SEPM0009; 5.1.11: SEPM0010; 5.1.2: SESU0013. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<a/>",
                                               .parameters = {XML_METHOD,
                                                              .standalone = SXML_STANDALONE_NO}},
                                           "!SEPM0009"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<a/>",
                                               .parameters = {XML_METHOD,
                                                              .undeclare_prefixes =
                                                                  SXML_SERIALIZATION_FLAG_YES}},
                                           "!SEPM0010"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<a/>",
                                               .parameters = {XML_METHOD, .has_version = true,
                                                              .version = text_view("2.0")}},
                                           "!SESU0013"));
    /* Section 5.1.10. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<!--c--><p:a xmlns:p='urn:p'/>",
                                               .parameters = {XML_METHOD, .has_doctype_system = true,
                                                              .doctype_system = text_view("a.dtd"),
                                                              .has_doctype_public = true,
                                                              .doctype_public = text_view("-//X//EN")}},
                                           "<!--c--><!DOCTYPE p:a PUBLIC \"-//X//EN\" \"a.dtd\">"
                                           "<p:a xmlns:p=\"urn:p\"/>"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<a/>",
                                               .parameters = {XML_METHOD, .has_doctype_system = true,
                                                              .doctype_system = text_view("say \"x\"")}},
                                           "<!DOCTYPE a SYSTEM 'say \"x\"'><a/>"));
    /* SEPM0004: doctype-system with more than one element. */
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, parse_document(current, "<a/>", &document) == SXML_STATUS_OK);
    sxml_item_retain(document);
    sxml_item_retain(document);
    sxml_sequence two =
        make_sequence(current, (const sxml_item[]){document, document}, 2);
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){
                                    .value = two,
                                    .parameters = {XML_METHOD, .has_doctype_system = true,
                                                   .doctype_system = text_view("a.dtd")}},
                                text_view("!SEPM0004")));
    sxml_sequence_release(two);
    sxml_item_release(document);
    /* SEPM0016 for values the parameters do not permit (section 3). */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<a/>",
                                               .parameters = {XML_METHOD, .has_indent_unit = true,
                                                              .indent_unit = text_view(" \t")}},
                                           "!SEPM0016"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<a/>",
                                               .parameters = {XML_METHOD, .has_doctype_system = true,
                                                              .doctype_system = text_view("'\"")}},
                                           "!SEPM0016"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------- Namespaces */

/* Builds <p:chapter xmlns:p="urn:p"><section/></p:chapter> with section
   not inheriting p (XQuery copy-namespaces no-inherit). */
static sxml_status build_undeclaring_tree(fixture *test_fixture, sxml_item *element) {
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        fixture_allocator(test_fixture),
        &(sxml_arena_tree_configuration){.allocator = fixture_allocator(test_fixture),
                                         .item_heap = test_fixture->item_heap,
                                         .duplicate_attribute_policy =
                                             SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                                         .is_destroyed_when_unreferenced = true},
        &arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_tree_builder builder = sxml_arena_tree_as_tree_builder(arena_tree);
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    sxml_namespace_uri_identifier uri = {.index = 0};
    status = sxml_name_pool_intern_namespace_prefix(test_fixture->name_pool, text_view("p"),
                                                    &prefix);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(test_fixture->name_pool, text_view("urn:p"),
                                                     &uri);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->start_element(
            builder.state,
            &(sxml_element_start_event){.element_name = intern_name(test_fixture, "urn:p",
                                                                    "chapter"),
                                        .namespace_prefix = prefix});
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->add_namespace_binding(
            builder.state,
            &(sxml_namespace_binding){.namespace_prefix = prefix, .namespace_uri = uri});
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->start_element(
            builder.state,
            &(sxml_element_start_event){.element_name = intern_name(test_fixture, "", "section"),
                                        .ignores_parent_namespaces = true});
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_element(builder.state);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_element(builder.state);
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *element = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

static void test_namespaces(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* Declarations only where the written parent lacks them; xmlns=""
       where a default goes out of scope (section 5). */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<p:a xmlns:p='urn:p' p:x='1'><b "
                                                           "xmlns='urn:d'><c xmlns=''/><p:d/></b>"
                                                           "</p:a>",
                                               .parameters = {XML_METHOD}},
                                           "<p:a xmlns:p=\"urn:p\" p:x=\"1\"><b xmlns=\"urn:d\">"
                                           "<c xmlns=\"\"/><p:d/></b></p:a>"));
    /* Undeclaring prefixes needs XML 1.1 (K2-Serialization-20, -21). */
    sxml_item chapter = {.representation = 0};
    SXML_TEST_REQUIRE(state, build_undeclaring_tree(current, &chapter) == SXML_STATUS_OK);
    sxml_sequence value = sxml_sequence_from_item(chapter);
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){.value = value,
                                                      .parameters = {XML_METHOD}},
                                text_view("<p:chapter xmlns:p=\"urn:p\"><section/></p:chapter>")));
    SXML_TEST_EXPECT(
        state, serialization_gives(
                   current,
                   &(serialization_case){
                       .value = value,
                       .parameters = {XML_METHOD, .has_version = true,
                                      .version = text_view("1.1"),
                                      .undeclare_prefixes = SXML_SERIALIZATION_FLAG_YES}},
                   text_view("<p:chapter xmlns:p=\"urn:p\"><section xmlns:p=\"\"/></p:chapter>")));
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------- Character maps, normalization */

static void test_character_maps_and_normalization(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* Section 12 (Serialization-xml-03): the replacement is written as it
       is, in text and attributes, not in comments. */
    const sxml_character_mapping mappings[] = {
        {.codepoint = 'c', .replacement_text = text_view("CCC")},
        {.codepoint = 'a', .replacement_text = text_view("<&>")},
    };
    const sxml_serialization_parameters mapped = {
        XML_METHOD,
        .use_character_maps = {.character_mappings = mappings, .character_mapping_count = 2}};
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<out att='abc'>XabcX<!--abc--></out>",
                                               .parameters = mapped},
                                           "<out att=\"<&>bCCC\">X<&>bCCCX<!--abc--></out>"));
    /* A codepoint twice is an invalid argument. */
    const sxml_character_mapping twice[] = {
        {.codepoint = 'c', .replacement_text = text_view("1")},
        {.codepoint = 'c', .replacement_text = text_view("2")},
    };
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, parse_document(current, "<a/>", &document) == SXML_STATUS_OK);
    sxml_sequence value = sxml_sequence_from_item(document);
    SXML_TEST_EXPECT(state, sxml_serialize_sequence(
                                current->item_heap,
                                &(sxml_serialization_request){
                                    .value = value,
                                    .serialization_parameters =
                                        {XML_METHOD,
                                         .use_character_maps = {.character_mappings = twice,
                                                                .character_mapping_count = 2}}},
                                sxml_output_buffer_as_output_stream(current->output_buffer)) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_sequence_release(value);
    /* Section 5.1.12: NFC composes e + COMBINING ACUTE ACCENT; NFD
       decomposes; fully-normalized refuses a leading composing
       character. */
    SXML_TEST_EXPECT(state,
                     document_gives(current,
                                    &(document_case){.xml_text = "<a b='e\xCC\x81'>e\xCC\x81</a>",
                                                     .parameters = {XML_METHOD,
                                                                    .normalization_form =
                                                                        SXML_NORMALIZATION_FORM_NFC}},
                                    "<a b=\"\xC3\xA9\">\xC3\xA9</a>"));
    SXML_TEST_EXPECT(state,
                     document_gives(current,
                                    &(document_case){.xml_text = "<a>\xC3\xA9</a>",
                                                     .parameters = {XML_METHOD,
                                                                    .normalization_form =
                                                                        SXML_NORMALIZATION_FORM_NFD}},
                                    "<a>e\xCC\x81</a>"));
    SXML_TEST_EXPECT(
        state, document_gives(current,
                              &(document_case){.xml_text = "<a><b/>\xCC\x81x</a>",
                                               .parameters = {XML_METHOD,
                                                              .normalization_form =
                                                                  SXML_NORMALIZATION_FORM_FULLY_NORMALIZED}},
                              "!SERE0012"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------- Canonical XML */

static void test_canonical(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    const sxml_serialization_parameters canonical = {
        XML_METHOD, .canonical = SXML_SERIALIZATION_FLAG_YES,
        .omit_xml_declaration = SXML_SERIALIZATION_FLAG_NO,
        .indent = SXML_SERIALIZATION_FLAG_YES};
    /* Canonical XML 1.1, sections 2.2 and 2.3: no declaration, empty
       elements as pairs, declarations then attributes in order, line feeds
       around the nodes outside the document element. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<!--c-->\n<e xmlns:b='urn:b' "
                                                           "xmlns:a='urn:a' z='1' b:y='2' a:x='3' "
                                                           "a='&#9;\"'><f>&#xD;&gt;</f></e><?p?>",
                                               .parameters = canonical},
                                           "<!--c-->\n<e xmlns:a=\"urn:a\" xmlns:b=\"urn:b\" a=\""
                                           "&#x9;&quot;\" z=\"1\" a:x=\"3\" b:y=\"2\"><f>&#xD;&gt;"
                                           "</f></e>\n<?p?>"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<e xmlns='rel'/>",
                                                            .parameters = canonical},
                                           "!SERE0024"));
    sxml_sequence text = sxml_sequence_from_item(make_string(current, "x"));
    SXML_TEST_EXPECT(state, serialization_gives(
                                current, &(serialization_case){.value = text, .parameters = canonical},
                                text_view("!SERE0024")));
    sxml_sequence_release(text);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------------- XHTML */

static void test_xhtml(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    const sxml_serialization_parameters html4 = {XHTML_METHOD, .has_html_version = true,
                                                 .html_version = text_view("4.0")};
    /* Serialization-xhtml-1, -1a, -23: minimized only when expected to be
       empty, with a space before "/>" before HTML5. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<html xmlns='http://www.w3.org/1999/"
                                                           "xhtml'><body><br/><p/><frame/></body>"
                                                           "</html>",
                                               .parameters = html4},
                                           "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
                                           "<br /><p></p><frame /></body></html>"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<html><br/></html>",
                                                            .parameters = html4},
                                           "<html><br></br></html>"));
    /* HTML5 (the default): a doctype, void elements, basefont not void
       (Serialization-xhtml-71, -72). */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<html xmlns='http://www.w3.org/1999/"
                                                           "xhtml'><body><br/><basefont/></body>"
                                                           "</html>",
                                               .parameters = {XHTML_METHOD}},
                                           "<!DOCTYPE html><html xmlns=\"http://www.w3.org/1999/"
                                           "xhtml\"><body><br/><basefont></basefont></body>"
                                           "</html>"));
    /* Prefix normalization (Serialization-xhtml-50, -51, -74, -75). */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<h:html xmlns:h='http://www.w3.org/"
                                                           "1999/xhtml'><h:body><s:svg xmlns:s="
                                                           "'http://www.w3.org/2000/svg'><s:r/>"
                                                           "</s:svg><x/></h:body></h:html>",
                                               .parameters = {XHTML_METHOD}},
                                           "<!DOCTYPE html><html xmlns=\"http://www.w3.org/1999/"
                                           "xhtml\"><body><svg xmlns=\"http://www.w3.org/2000/svg"
                                           "\"><r></r></svg><x xmlns=\"\"></x></body></html>"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<o:h3 xmlns:o='urn:o' xmlns:x='http:"
                                                           "//www.w3.org/1999/xhtml'><o:i "
                                                           "x:id='i'/></o:h3>",
                                               .parameters = {XHTML_METHOD}},
                                           "<o:h3 xmlns:o=\"urn:o\"><o:i xmlns:x=\"http://www.w3."
                                           "org/1999/xhtml\" x:id=\"i\"></o:i></o:h3>"));
    /* include-content-type (Serialization-xhtml-33, -34, -76). */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<html xmlns='http://www.w3.org/1999/"
                                                           "xhtml'><head><meta http-equiv=' "
                                                           "content-type' content='x'/><title>t"
                                                           "</title></head></html>",
                                               .parameters = html4},
                                           "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
                                           "<meta http-equiv=\"Content-Type\" content=\"text/html; "
                                           "charset=UTF-8\" /><title>t</title></head></html>"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<html><head><meta charset='latin1'/>"
                                                           "</head></html>",
                                               .parameters = {XHTML_METHOD}},
                                           "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"/>"
                                           "</head></html>"));
    /* escape-uri-attributes (section 6.1.17): non-ASCII %-escaped in a
       URI attribute only. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<html><a href='\xC3\xA9 x&amp;' "
                                                           "title='\xC3\xA9'/></html>",
                                               .parameters = {XHTML_METHOD}},
                                           "<!DOCTYPE html><html><a href=\"%C3%A9 x&amp;\" "
                                           "title=\"\xC3\xA9\"></a></html>"));
    /* Indentation: block elements indented, inline and mixed content left
       alone (Serialization-xhtml-59, -65). */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<html><body><ul><li><p>One</p></li>"
                                                           "</ul><p>a<a>t</a>z</p></body></html>",
                                               .parameters = {XHTML_METHOD,
                                                              .indent = SXML_SERIALIZATION_FLAG_YES,
                                                              .include_content_type =
                                                                  SXML_SERIALIZATION_FLAG_NO}},
                                           "<!DOCTYPE html>\n<html>\n  <body>\n    <ul>\n      "
                                           "<li>\n        <p>One</p>\n      </li>\n    </ul>\n"
                                           "    <p>a<a>t</a>z</p>\n  </body>\n</html>"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------ Failures */

/* 20,000 nested elements: the writer's stacks are heap memory (D71). */
static void test_deep_nesting(sxml_test_state *state) {
    static const size_t depth = 20000;
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static char deep_text[20000 * 7 + 1];
    size_t length = 0;
    for (size_t level = 0; level < depth; level += 1) {
        memcpy(deep_text + length, "<a>", 3);
        length += 3;
    }
    for (size_t level = 0; level < depth; level += 1) {
        memcpy(deep_text + length, "</a>", 4);
        length += 4;
    }
    deep_text[length] = '\0';
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, parse_document(&test_fixture, deep_text, &document) == SXML_STATUS_OK);
    const sxml_sequence value = sxml_sequence_from_item(document);
    /* The innermost element is written as <a/>. */
    deep_text[3 * (depth - 1) + 2] = '/';
    deep_text[3 * (depth - 1) + 3] = '>';
    memmove(deep_text + 3 * depth + 1, deep_text + 3 * depth + 4,
            length - 3 * depth - 4 + 1);
    SXML_TEST_EXPECT(state, serialization_gives(
                                &test_fixture,
                                &(serialization_case){.value = value, .parameters = {XML_METHOD}},
                                text_view(deep_text)));
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_failures(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, parse_document(&test_fixture, "<a/>", &document) == SXML_STATUS_OK);
    sxml_sequence value = sxml_sequence_from_item(document);
    /* What C can say and XDM cannot: an invalid argument. The out-of-range
       enumeration values are the point of the test. */
    const sxml_name_identifier zero = {.index = 0};
    const sxml_serialization_parameters invalid[] = {
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        {XML_METHOD, .indent = (sxml_serialization_flag)7},
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        {XML_METHOD, .character_encoding = (sxml_character_encoding)9},
        {XML_METHOD, .cdata_section_elements = {.element_names = &zero, .element_name_count = 1}},
        {XML_METHOD, .has_media_type = true, .media_type = {.bytes = NULL, .byte_count = 2}},
    };
    for (size_t case_index = 0; case_index < sizeof(invalid) / sizeof(invalid[0]);
         case_index += 1) {
        SXML_TEST_EXPECT(state, sxml_serialize_sequence(
                                    test_fixture.item_heap,
                                    &(sxml_serialization_request){
                                        .value = value,
                                        .serialization_parameters = invalid[case_index]},
                                    sxml_output_buffer_as_output_stream(
                                        test_fixture.output_buffer)) ==
                                    SXML_STATUS_INVALID_ARGUMENT);
    }
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    /* Every allocation failure is reported and leaks nothing: parse and
       serialize with the n-th allocation failing until nothing fails. */
    bool has_succeeded = false;
    for (uint64_t failing = 1; failing < 2000 && !has_succeeded; failing += 1) {
        SXML_TEST_REQUIRE(state, fixture_create(failing, &test_fixture) == SXML_STATUS_OK);
        sxml_item tree = {.representation = 0};
        sxml_status status = parse_document(
            &test_fixture,
            "<p:a xmlns:p='urn:p'><b c='\xC3\xA9'>e\xCC\x81 x<d/></b><!--x--></p:a>", &tree);
        if (status == SXML_STATUS_OK) {
            sxml_error_record error_record = {.value = NULL};
            sxml_output_buffer_clear(test_fixture.output_buffer);
            status = sxml_serialize_sequence(
                test_fixture.item_heap,
                &(sxml_serialization_request){
                    .value = sxml_sequence_from_item(tree),
                    .serialization_parameters = {XML_METHOD,
                                                 .indent = SXML_SERIALIZATION_FLAG_YES,
                                                 .normalization_form = SXML_NORMALIZATION_FORM_NFC,
                                                 .has_item_separator = true,
                                                 .item_separator = text_view("-")},
                    .error_record = &error_record},
                sxml_output_buffer_as_output_stream(test_fixture.output_buffer));
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
            has_succeeded =
                status == SXML_STATUS_OK &&
                sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                      text_view("<p:a xmlns:p=\"urn:p\">\n  <b c=\"\xC3\xA9\">"
                                                "\xC3\xA9 x<d/></b>\n  <!--x-->\n</p:a>"));
            sxml_item_release(tree);
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

static const sxml_test_case serialize_xml_test_cases[] = {
    {.name = "sequence_normalization", .function = test_sequence_normalization},
    {.name = "escaping", .function = test_escaping},
    {.name = "encodings", .function = test_encodings},
    {.name = "line_endings", .function = test_line_endings},
    {.name = "indentation", .function = test_indentation},
    {.name = "cdata_sections", .function = test_cdata_sections},
    {.name = "declarations", .function = test_declarations},
    {.name = "namespaces", .function = test_namespaces},
    {.name = "character_maps_and_normalization",
     .function = test_character_maps_and_normalization},
    {.name = "canonical", .function = test_canonical},
    {.name = "xhtml", .function = test_xhtml},
    {.name = "deep_nesting", .function = test_deep_nesting},
    {.name = "failures", .function = test_failures},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "serialize_xml",
            .cases = serialize_xml_test_cases,
            .case_count = SXML_TEST_COUNT(serialize_xml_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
