/*
 * Tests for the HTML and text output methods (src/serialize_html.c, the
 * HTML rules of src/serialize_xml.c, src/serialize_text.c; Serialization
 * 4.0, sections 7 and 8; D128): void elements, raw text, boolean
 * attributes, escaping in attributes, processing instructions, the
 * document type declaration and the meta element, XML islands, versions
 * before HTML5 (SERE0014, SERE0015), and the text method's string value
 * without escaping.
 *
 * Golden file: tests/serialize_xml_test.c (fixture, expected texts, "!CODE"
 * for an expected error, the allocation-failure loop). Expected texts
 * follow the QT4 method-html and method-text test sets.
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

#define HTML_METHOD .serialization_method = SXML_SERIALIZATION_METHOD_HTML
#define TEXT_METHOD .serialization_method = SXML_SERIALIZATION_METHOD_TEXT
/* No meta element, so that expected texts stay short. */
#define NO_META .include_content_type = SXML_SERIALIZATION_FLAG_NO

/* ------------------------------------------------------------ Elements */

static void test_elements(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* Section 7.1: void elements have no end tag, other empty elements
       a start and an end tag, never an empty-element tag. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<p><br/>a<BR/><p/></p>",
                                                            .parameters = {HTML_METHOD, NO_META}},
                                           "<p><br>a<BR><p></p></p>"));
    /* HTML 4: isindex and basefont are empty, source is not. */
    SXML_TEST_EXPECT(
        state,
        document_gives(current,
                       &(document_case){.xml_text = "<p><isindex/><source/></p>",
                                        .parameters = {HTML_METHOD, NO_META, .has_version = true,
                                                       .version = SXML_BYTE_VIEW_FROM_LITERAL(
                                                           "4.0")}},
                       "<p><isindex><source></source></p>"));
    /* XML islands keep the XML syntax; with HTML5 an element in the XHTML
       namespace is an HTML element, unprefixed (7.1). */
    SXML_TEST_EXPECT(
        state,
        document_gives(current,
                       &(document_case){.xml_text = "<div><x:i xmlns:x='urn:x'/>"
                                                    "<h:br xmlns:h='http://www.w3.org/1999/xhtml'/>"
                                                    "</div>",
                                        .parameters = {HTML_METHOD, NO_META}},
                       "<div><x:i xmlns:x=\"urn:x\"/>"
                       "<br xmlns=\"http://www.w3.org/1999/xhtml\"></div>"));
    /* Raw text: script and style content and their descendants' attributes
       are not escaped (7.1). */
    SXML_TEST_EXPECT(state,
                     document_gives(current,
                                    &(document_case){
                                        .xml_text = "<p><script>if (a &lt; b &amp;&amp; c) x()"
                                                    "</script><style>p&gt;a{}</style>a&lt;b</p>",
                                        .parameters = {HTML_METHOD, NO_META}},
                                    "<p><script>if (a < b && c) x()</script>"
                                    "<style>p>a{}</style>a&lt;b</p>"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ---------------------------------------------------------- Attributes */

static void test_attributes(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* Section 7.2: boolean attributes minimized when the value is the
       name; "<" and "&{" stay; '"' is a reference. */
    SXML_TEST_EXPECT(
        state,
        document_gives(current,
                       &(document_case){.xml_text = "<OPTION selected='SELECTED' value='a&lt;b'"
                                                    " title='&amp;{x}; &amp;y &quot;'/>",
                                        .parameters = {HTML_METHOD, NO_META}},
                       "<OPTION selected value=\"a<b\" title=\"&{x}; &amp;y &quot;\"></OPTION>"));
    /* Only boolean attributes; checked="no" stays. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){
                                               .xml_text = "<input checked='no' name='name'/>",
                                               .parameters = {HTML_METHOD, NO_META}},
                                           "<input checked=\"no\" name=\"name\">"));
    /* escape-uri-attributes (7.5.13): non-ASCII in href %-escaped. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<a href='\xC3\xA9 x'/>",
                                                            .parameters = {HTML_METHOD, NO_META}},
                                           "<a href=\"%C3%A9 x\"></a>"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------- Comments, PIs, controls, doctype */

static void test_documents(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    const sxml_serialization_parameters html4 = {
        HTML_METHOD, NO_META, .has_html_version = true,
        .html_version = SXML_BYTE_VIEW_FROM_LITERAL("4.01")};
    /* Section 7.4: processing instructions end with ">" before HTML5 and
       are comments with HTML5, "--" split. */
    SXML_TEST_EXPECT(state,
                     document_gives(current,
                                    &(document_case){.xml_text = "<p><?pi a--b?></p>",
                                                     .parameters = {HTML_METHOD, NO_META}},
                                    "<p><!--?pi a- -b?--></p>"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<p><?pi a?></p>",
                                                            .parameters = html4},
                                           "<p><?pi a></p>"));
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<p><?pi a>b?></p>",
                                                            .parameters = html4},
                                           "!SERE0015"));
    /* Section 7.3: control characters are SERE0014 before HTML5 and
       references with HTML5. */
    sxml_sequence control = sxml_sequence_from_item(make_string(current, "a\x01"));
    SXML_TEST_EXPECT(state, serialization_gives(
                                current, &(serialization_case){.value = control, .parameters = html4},
                                text_view("!SERE0014")));
    SXML_TEST_EXPECT(state,
                     serialization_gives(current,
                                         &(serialization_case){.value = control,
                                                               .parameters = {HTML_METHOD}},
                                         text_view("a&#x1;")));
    sxml_sequence_release(control);
    /* Section 7.5.8: <!DOCTYPE html> before an html element with HTML5;
       PUBLIC and SYSTEM identifiers otherwise. 7.5.14: the meta element as
       head's first child, an old one dropped. */
    SXML_TEST_EXPECT(
        state,
        document_gives(current,
                       &(document_case){.xml_text = "<html><head><meta charset='x'/><title>t"
                                                    "</title></head></html>",
                                        .parameters = {HTML_METHOD}},
                       "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>t</title>"
                       "</head></html>"));
    SXML_TEST_EXPECT(
        state,
        document_gives(current,
                       &(document_case){
                           .xml_text = "<html><head/></html>",
                           .parameters = {HTML_METHOD, .has_version = true,
                                          .version = SXML_BYTE_VIEW_FROM_LITERAL("4.0"),
                                          .has_doctype_public = true,
                                          .doctype_public = SXML_BYTE_VIEW_FROM_LITERAL(
                                              "-//W3C//DTD HTML 4.0//EN"),
                                          .has_doctype_system = true,
                                          .doctype_system = SXML_BYTE_VIEW_FROM_LITERAL("s.dtd")}},
                       "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.0//EN\" \"s.dtd\"><html><head>"
                       "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">"
                       "</head></html>"));
    /* Indentation (7.5.3): block elements indented, inline ones not. */
    SXML_TEST_EXPECT(state,
                     document_gives(current,
                                    &(document_case){
                                        .xml_text = "<html><body><div><p>a<b>b</b></p></div>"
                                                    "</body></html>",
                                        .parameters = {HTML_METHOD, NO_META,
                                                       .indent = SXML_SERIALIZATION_FLAG_YES}},
                                    "<!DOCTYPE html>\n<html>\n  <body>\n    <div>\n"
                                    "      <p>a<b>b</b></p>\n    </div>\n  </body>\n</html>"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------- The text method */

static void test_text_method(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* Section 8: the string value of the result tree, nothing escaped,
       comments and processing instructions left out. */
    SXML_TEST_EXPECT(state, document_gives(current,
                                           &(document_case){.xml_text = "<a>x&lt;<!--c--><?p q?>"
                                                                        "<b k='v'>&amp;y</b></a>",
                                                            .parameters = {TEXT_METHOD}},
                                           "x<&y"));
    /* Controls are written as they are (QT4 serialize-xml-169); an
       unencodable character is SERE0008 (8.1.1); line-ending applies. */
    sxml_item items[2] = {make_string(current, "\x01\n"), make_string(current, "\xC3\xA9")};
    sxml_sequence value = make_sequence(current, items, 2);
    SXML_TEST_EXPECT(state,
                     serialization_gives(
                         current,
                         &(serialization_case){
                             .value = value,
                             .parameters = {TEXT_METHOD, .line_ending =
                                                             SXML_LINE_ENDING_CARRIAGE_RETURN_LINE_FEED}},
                         text_view("\x01\r\n \xC3\xA9")));
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){
                                    .value = value,
                                    .parameters = {TEXT_METHOD, .character_encoding =
                                                                    SXML_CHARACTER_ENCODING_US_ASCII}},
                                text_view("!SERE0008")));
    /* Character maps apply (section 12). */
    const sxml_character_mapping mappings[] = {
        {.codepoint = 0xE9U, .replacement_text = SXML_BYTE_VIEW_FROM_LITERAL("e'")}};
    SXML_TEST_EXPECT(state,
                     serialization_gives(
                         current,
                         &(serialization_case){
                             .value = value,
                             .parameters = {TEXT_METHOD,
                                            .use_character_maps = {.character_mappings = mappings,
                                                                   .character_mapping_count = 1}}},
                         text_view("\x01\n e'")));
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------- Allocation failures */

static void test_allocation_failures(sxml_test_state *state) {
    bool has_succeeded = false;
    for (uint64_t failing = 1; failing < 400 && !has_succeeded; failing += 1) {
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(failing, &test_fixture) == SXML_STATUS_OK);
        sxml_item document = {.representation = 0};
        sxml_status status = parse_document(
            &test_fixture, "<html><head/><body><script>a&lt;b</script><br/></body></html>",
            &document);
        if (status == SXML_STATUS_OK) {
            sxml_error_record error_record = {.value = NULL};
            status = sxml_serialize_sequence(
                test_fixture.item_heap,
                &(sxml_serialization_request){
                    .value = sxml_sequence_from_item(document),
                    .serialization_parameters = {HTML_METHOD,
                                                 .indent = SXML_SERIALIZATION_FLAG_YES},
                    .error_record = &error_record},
                sxml_output_buffer_as_output_stream(test_fixture.output_buffer));
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
            has_succeeded = status == SXML_STATUS_OK;
            sxml_item_release(document);
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

static const sxml_test_case serialize_html_test_cases[] = {
    {.name = "elements", .function = test_elements},
    {.name = "attributes", .function = test_attributes},
    {.name = "documents", .function = test_documents},
    {.name = "text_method", .function = test_text_method},
    {.name = "allocation_failures", .function = test_allocation_failures},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "serialize_html",
            .cases = serialize_html_test_cases,
            .case_count = SXML_TEST_COUNT(serialize_html_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
