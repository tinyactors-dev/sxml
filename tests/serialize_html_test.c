/*
 * Tests for the HTML and text output methods (src/serialize_html.c, the
 * HTML rules of src/serialize_xml.c, src/serialize_text.c; Serialization
 * 4.0, sections 7 and 8; D128): void elements, raw text, boolean
 * attributes, escaping in attributes, processing instructions, the
 * document type declaration and the meta element, XML islands, versions
 * before HTML5 (SERE0014, SERE0015), the text method's string value
 * without escaping, and the streamed serialization of the HTML and XHTML
 * methods (D297): byte for byte what the tree gives, with text events
 * split at every character.
 *
 * Golden file: tests/serialize_xml_test.c (fixture, expected texts, "!CODE"
 * for an expected error, the allocation-failure loop). Expected texts
 * follow the QT4 method-html and method-text test sets.
 */
#include "sxml/serialize.h"

#include "serialize_internal.h"
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

/* ------------------------------------------- Streamed serialization */

/* A tree builder that forwards its events to target, each text event as
   one event per character: event borders inside script and style
   elements, around multi-byte characters and in attribute-free text. */
typedef struct splitting_builder {
    sxml_tree_builder target;
} splitting_builder;

static splitting_builder *splitting_from_state(void *state) {
    return state;
}

static sxml_status split_start_document(void *state, const sxml_document_start_event *event) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    return target.operations->start_document(target.state, event);
}

static sxml_status split_end_document(void *state) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    return target.operations->end_document(target.state);
}

static sxml_status split_start_element(void *state, const sxml_element_start_event *event) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    return target.operations->start_element(target.state, event);
}

static sxml_status split_namespace_binding(void *state, const sxml_namespace_binding *binding) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    return target.operations->add_namespace_binding(target.state, binding);
}

static sxml_status split_attribute(void *state, const sxml_attribute_event *event) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    return target.operations->add_attribute(target.state, event);
}

static sxml_status split_end_element(void *state) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    return target.operations->end_element(target.state);
}

static sxml_status split_text(void *state, const sxml_text_event *event) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    sxml_status status = SXML_STATUS_OK;
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < event->text.byte_count) {
        size_t character_byte_count = 1;
        while (offset + character_byte_count < event->text.byte_count &&
               (event->text.bytes[offset + character_byte_count] & 0xC0U) == 0x80U) {
            character_byte_count += 1;
        }
        sxml_text_event part = *event;
        part.text = (sxml_byte_view){.bytes = event->text.bytes + offset,
                                     .byte_count = character_byte_count};
        status = target.operations->add_text(target.state, &part);
        offset += character_byte_count;
    }
    return status;
}

static sxml_status split_comment(void *state, const sxml_comment_event *event) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    return target.operations->add_comment(target.state, event);
}

static sxml_status split_processing_instruction(void *state,
                                                const sxml_processing_instruction_event *event) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    return target.operations->add_processing_instruction(target.state, event);
}

static sxml_status split_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    const sxml_tree_builder target = splitting_from_state(state)->target;
    return target.operations->add_unparsed_entity(target.state, entity);
}

static const sxml_tree_builder_operations splitting_operations = {
    .start_document = split_start_document,
    .end_document = split_end_document,
    .start_element = split_start_element,
    .add_namespace_binding = split_namespace_binding,
    .add_attribute = split_attribute,
    .end_element = split_end_element,
    .add_text = split_text,
    .add_comment = split_comment,
    .add_processing_instruction = split_processing_instruction,
    .add_unparsed_entity = split_unparsed_entity,
    .set_element_identity = NULL,
};

/* Serializes document's events, split, through a streamed serialization
   into output; the error's code goes to *error_record. */
typedef struct streamed_run {
    sxml_item document;
    const sxml_serialization_parameters *parameters;
    sxml_output_buffer *output;
} streamed_run;

static sxml_status run_streamed(fixture *test_fixture, const streamed_run *run,
                                sxml_error_record *error_record) {
    sxml_streamed_serialization *serialization = NULL;
    sxml_status status = sxml_streamed_serialization_create(
        fixture_allocator(test_fixture),
        &(sxml_streamed_serialization_request){
            .item_heap = test_fixture->item_heap,
            .parameters = run->parameters,
            .output_stream = sxml_output_buffer_as_output_stream(run->output),
            .error_record = error_record},
        &serialization);
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(run->document, &node);
    }
    if (status == SXML_STATUS_OK) {
        splitting_builder splitting = {
            .target = sxml_streamed_serialization_as_tree_builder(serialization)};
        status = sxml_tree_builder_copy_node(
            (sxml_tree_builder){.state = &splitting, .operations = &splitting_operations},
            &(sxml_copy_request){.node = node,
                                 .preserves_namespaces = true,
                                 .inherits_namespaces = true,
                                 .preserves_type_annotations = true});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_streamed_serialization_finish(serialization);
    }
    sxml_streamed_serialization_destroy(serialization);
    return status;
}

/* Whether the streamed serialization of xml_text with parameters gives
   the bytes, or the error code, of the tree's serialization. */
typedef struct streamed_case {
    const char *xml_text;
    const sxml_serialization_parameters *parameters;
} streamed_case;

static bool streamed_equals_tree(fixture *test_fixture, const streamed_case *test_case) {
    sxml_item document = {.representation = 0};
    if (parse_document(test_fixture, test_case->xml_text, &document) != SXML_STATUS_OK) {
        (void)fprintf(stderr, "cannot parse %s\n", test_case->xml_text);
        return false;
    }
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_error_record tree_error = {.value = NULL};
    const sxml_status tree_status = sxml_serialize_sequence(
        test_fixture->item_heap,
        &(sxml_serialization_request){.value = sxml_sequence_from_item(document),
                                      .serialization_parameters = *test_case->parameters,
                                      .error_record = &tree_error},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    sxml_output_buffer *streamed = NULL;
    sxml_status status = sxml_output_buffer_create(
        sxml_default_allocator(),
        &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                            .maximum_byte_count = 0},
        &streamed);
    sxml_error_record streamed_error = {.value = NULL};
    if (status == SXML_STATUS_OK) {
        status = run_streamed(test_fixture,
                              &(streamed_run){.document = document,
                                              .parameters = test_case->parameters,
                                              .output = streamed},
                              &streamed_error);
    }
    const sxml_byte_view expected = sxml_output_buffer_get_bytes(test_fixture->output_buffer);
    const sxml_byte_view got =
        streamed == NULL ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                         : sxml_output_buffer_get_bytes(streamed);
    bool is_equal = status == tree_status;
    if (is_equal && status == SXML_STATUS_OK) {
        is_equal = sxml_byte_view_equals(got, expected);
    } else if (is_equal && status == SXML_STATUS_DYNAMIC_ERROR) {
        is_equal = sxml_byte_view_equals(streamed_error.code.local_name,
                                         tree_error.code.local_name);
    }
    if (!is_equal) {
        (void)fprintf(stderr, "%s: tree status %d \"%.*s\", streamed status %d \"%.*s\"\n",
                      test_case->xml_text, (int)tree_status, (int)expected.byte_count,
                      (const char *)expected.bytes, (int)status, (int)got.byte_count,
                      (const char *)got.bytes);
    }
    sxml_output_buffer_destroy(streamed);
    sxml_item_release(document);
    return is_equal;
}

#define XHTML_METHOD .serialization_method = SXML_SERIALIZATION_METHOD_XHTML

/* Documents for the HTML rules: void and raw text elements (raw text in
   events of one character), boolean and URI attributes, the meta element
   added and one with content replaced, an empty head, comments and
   processing instructions, the top level before the first element, XML
   islands, the XHTML, SVG and MathML namespaces with prefixes an
   ancestor's attribute keeps. */
static const char *const streamed_documents[] = {
    "<html><head><meta charset='x'/><title>t&amp;u</title></head><body><p>a<br/>b"
    "<script>if (a &lt; b &amp;&amp; c) x('\xC3\xA9\xE2\x82\xAC')</script>"
    "<style>p&gt;a{}</style><input checked='checked' name='n'/>"
    "<a href='\xC3\xA9 x?a=1&amp;b'>\xE2\x82\xAC\xF0\x9F\x98\x80</a><!--c--><?pi a--b?></p>"
    "</body></html>",
    "<html><head/></html>",
    "<html><HEAD><meta http-equiv=' Content-Type ' content='x'><b>in</b>t</meta>"
    "<META CHARSET='y'/><link href='\xC3\xA9'/></HEAD><body/></html>",
    "<!--c--><?pi d?>\n<HTML><body><SCRIPT>a&lt;<b>\xC3\xA9</b>&amp;</SCRIPT></body></HTML>"
    "<!--after-->",
    "<html xmlns='http://www.w3.org/1999/xhtml'><head/><body><br/><p/>"
    "<svg:svg xmlns:svg='http://www.w3.org/2000/svg'><svg:g/></svg:svg>"
    "<m:math xmlns:m='http://www.w3.org/1998/Math/MathML'/><x:i xmlns:x='urn:x'/></body></html>",
    "<h:html xmlns:h='http://www.w3.org/1999/xhtml' h:a='1'><h:body><h:p xmlns:h2="
    "'http://www.w3.org/1999/xhtml'><h:br/></h:p></h:body></h:html>",
    "<div><pre>\n a\xC2\x80</pre><textarea>&lt;x&gt;</textarea><option selected='SELECTED'/>"
    "<img src='a b.png' alt='&quot;'/></div>",
};

static void test_streamed_matches_tree(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_character_mapping mappings[] = {
        {.codepoint = 0xE9U, .replacement_text = SXML_BYTE_VIEW_FROM_LITERAL("&eacute;")}};
    const sxml_serialization_parameters parameter_sets[] = {
        {HTML_METHOD},
        {HTML_METHOD, NO_META},
        {HTML_METHOD, .has_html_version = true,
         .html_version = SXML_BYTE_VIEW_FROM_LITERAL("4.01")},
        {HTML_METHOD, .has_version = true, .version = SXML_BYTE_VIEW_FROM_LITERAL("4.0"),
         .has_doctype_public = true,
         .doctype_public = SXML_BYTE_VIEW_FROM_LITERAL("-//W3C//DTD HTML 4.0//EN"),
         .has_doctype_system = true, .doctype_system = SXML_BYTE_VIEW_FROM_LITERAL("s\".dtd")},
        {HTML_METHOD, .has_doctype_system = true,
         .doctype_system = SXML_BYTE_VIEW_FROM_LITERAL("about:legacy-compat")},
        {HTML_METHOD, .escape_uri_attributes = SXML_SERIALIZATION_FLAG_NO,
         .character_encoding = SXML_CHARACTER_ENCODING_US_ASCII},
        {HTML_METHOD, .normalization_form = SXML_NORMALIZATION_FORM_NFD,
         .use_character_maps = {.character_mappings = mappings, .character_mapping_count = 1}},
        {XHTML_METHOD},
        {XHTML_METHOD, .omit_xml_declaration = SXML_SERIALIZATION_FLAG_NO,
         .has_html_version = true, .html_version = SXML_BYTE_VIEW_FROM_LITERAL("1.0")},
        {XHTML_METHOD, .include_content_type = SXML_SERIALIZATION_FLAG_NO,
         .character_encoding = SXML_CHARACTER_ENCODING_US_ASCII},
    };
    for (size_t parameter_index = 0; parameter_index < SXML_TEST_COUNT(parameter_sets);
         parameter_index += 1) {
        SXML_TEST_EXPECT(state, sxml_serialization_streams(&parameter_sets[parameter_index]));
        for (size_t document_index = 0; document_index < SXML_TEST_COUNT(streamed_documents);
             document_index += 1) {
            const bool is_equal = streamed_equals_tree(
                &test_fixture,
                &(streamed_case){.xml_text = streamed_documents[document_index],
                                 .parameters = &parameter_sets[parameter_index]});
            if (!is_equal) {
                (void)fprintf(stderr, "  parameter set %zu\n", parameter_index);
            }
            SXML_TEST_EXPECT(state, is_equal);
        }
    }
    /* Indentation looks ahead (whether a later child is text or inline
       decides the whitespace before the first); XHTML's document type
       declaration and Canonical XML need the whole tree (D297). */
    const sxml_serialization_parameters built_sets[] = {
        {HTML_METHOD, .indent = SXML_SERIALIZATION_FLAG_YES},
        {XHTML_METHOD, .indent = SXML_SERIALIZATION_FLAG_YES},
        {XHTML_METHOD, .has_doctype_system = true,
         .doctype_system = SXML_BYTE_VIEW_FROM_LITERAL("s.dtd")},
        {XHTML_METHOD, .canonical = SXML_SERIALIZATION_FLAG_YES},
    };
    for (size_t parameter_index = 0; parameter_index < SXML_TEST_COUNT(built_sets);
         parameter_index += 1) {
        SXML_TEST_EXPECT(state, !sxml_serialization_streams(&built_sets[parameter_index]));
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Every allocation of the streamed HTML serialization may fail: the
   status says so and nothing leaks. */
static void test_streamed_allocation_failures(sxml_test_state *state) {
    bool has_succeeded = false;
    const sxml_serialization_parameters parameters = {HTML_METHOD};
    for (uint64_t failing = 1; failing < 600 && !has_succeeded; failing += 1) {
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(failing, &test_fixture) == SXML_STATUS_OK);
        sxml_item document = {.representation = 0};
        sxml_status status = parse_document(&test_fixture, streamed_documents[0], &document);
        if (status == SXML_STATUS_OK) {
            sxml_error_record error_record = {.value = NULL};
            status = run_streamed(&test_fixture,
                                  &(streamed_run){.document = document,
                                                  .parameters = &parameters,
                                                  .output = test_fixture.output_buffer},
                                  &error_record);
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
            has_succeeded = status == SXML_STATUS_OK;
            sxml_item_release(document);
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
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
    {.name = "streamed_matches_tree", .function = test_streamed_matches_tree},
    {.name = "streamed_allocation_failures", .function = test_streamed_allocation_failures},
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
