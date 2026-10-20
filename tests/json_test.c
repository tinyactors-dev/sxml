/*
 * Tests for JSON texts and the JSON part of the function library
 * (src/json.c, src/function_library_json.c; D82): parsing into values and
 * into the XML representation of JSON, writing that representation back,
 * fn:parse-json, fn:json-to-xml, fn:xml-to-json, fn:jtree and
 * fn:parse-xml, with the JSON slice lines of tests/qt4/torture-slice.txt.
 *
 * Golden file: tests/function_library_test.c (fixture, evaluation,
 * allocation-failure loops), itself after tests/memory_test.c. Values are
 * written with the adaptive output method (src/serialize.c), so a map is
 * {"a":1.0e0} and a string "x". Every case checks that the heap's cells and
 * the allocator's blocks are all released.
 */
#include "evaluator_internal.h"
#include "function_library_internal.h"
#include "json_internal.h"
#include "xdm_internal.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* "\u" of a JSON escape sequence, split so no tool or compiler reads it
   as a universal character name. */
#define ESCAPE_U "\\" "u"

/* ------------------------------------------------------------ Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    alignas(16) uint8_t counting_storage[256];
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
    sxml_function_library *library;
    sxml_output_buffer *output_buffer;
} fixture;

static const sxml_native_function_view *const all_parts[] = {
    &sxml_function_library_numeric,  &sxml_function_library_string,
    &sxml_function_library_sequence, &sxml_function_library_node,
    &sxml_function_library_context,  &sxml_function_library_error,
    &sxml_function_library_higher_order, &sxml_function_library_map,
    &sxml_function_library_array,    &sxml_function_library_json,
};

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
        status = sxml_function_library_create(
            sxml_default_allocator(),
            &(sxml_function_library_configuration){
                .allocator = sxml_default_allocator(),
                .item_heap = test_fixture->item_heap,
                .parts = {.native_function_views = all_parts,
                          .native_function_view_count = SXML_TEST_COUNT(all_parts)}},
            &test_fixture->library);
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
    sxml_function_library_destroy(test_fixture->library);
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

/* The adaptive serialization of value, in the fixture's buffer. */
static sxml_status serialize(fixture *test_fixture, sxml_sequence value) {
    sxml_output_buffer_clear(test_fixture->output_buffer);
    return sxml_serialize_sequence(
        test_fixture->item_heap,
        &(sxml_serialization_request){
            .value = value,
            .serialization_parameters = {.has_item_separator = true,
                                         .item_separator = text_view(" ")}},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
}

static bool buffer_holds(const fixture *test_fixture, const char *expected_text) {
    const sxml_byte_view bytes = sxml_output_buffer_get_bytes(test_fixture->output_buffer);
    const bool is_equal = sxml_byte_view_equals(bytes, text_view(expected_text));
    if (!is_equal) {
        (void)fprintf(stderr, "  expected \"%s\"\n  got      \"%.*s\"\n", expected_text,
                      (int)bytes.byte_count, (const char *)bytes.bytes);
    }
    return is_equal;
}

/* The error code of a record, as C text. */
typedef struct error_code_text {
    char text[32];
} error_code_text;

static error_code_text read_code(const sxml_error_record *error_record) {
    error_code_text code = {.text = {0}};
    const size_t byte_count = error_record->code.local_name.byte_count < sizeof(code.text) - 1
                                  ? error_record->code.local_name.byte_count
                                  : sizeof(code.text) - 1;
    if (byte_count > 0) {
        memcpy(code.text, error_record->code.local_name.bytes, byte_count);
    }
    return code;
}

/* -------------------------------------------- Parsing into XDM values */

static const sxml_json_parse_options default_options = {
    .duplicates_policy = SXML_JSON_DUPLICATES_POLICY_USE_FIRST,
    .keeps_escaped_form = false,
    .number_format = SXML_JSON_NUMBER_FORMAT_DOUBLE,
    .null_value = {.content = {.representation = 0}}};

/* A JSON text and how to parse it; expected is the adaptive form of the
   value, or "!CODE" for an error, or "!LIMIT" for LIMIT_EXCEEDED. */
typedef struct parse_case {
    const char *json_text;
    sxml_json_parse_options options;
    uint32_t maximum_depth;
} parse_case;

static bool parses_to(const parse_case *test_case, const char *expected_text) {
    fixture test_fixture;
    if (fixture_create(0, &test_fixture) != SXML_STATUS_OK) {
        (void)fixture_destroy(&test_fixture);
        return false;
    }
    sxml_error_record error_record = {.value = NULL};
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_json_make_value(
        test_fixture.item_heap,
        &(sxml_json_text_request){.json_text = text_view(test_case->json_text),
                                  .options = test_case->options,
                                  .allocator = fixture_allocator(&test_fixture),
                                  .maximum_depth = test_case->maximum_depth,
                                  .error_record = &error_record},
        &value);
    bool is_expected = false;
    if (expected_text[0] == '!') {
        is_expected = strcmp(expected_text, "!LIMIT") == 0
                          ? status == SXML_STATUS_LIMIT_EXCEEDED
                          : status == SXML_STATUS_DYNAMIC_ERROR &&
                                strcmp(read_code(&error_record).text, expected_text + 1) == 0;
    } else if (status == SXML_STATUS_OK) {
        status = serialize(&test_fixture, value);
        is_expected = status == SXML_STATUS_OK && buffer_holds(&test_fixture, expected_text);
    }
    if (!is_expected) {
        (void)fprintf(stderr, "  for %s: status %d %s\n", test_case->json_text, (int)status,
                      read_code(&error_record).text);
    }
    sxml_sequence_release(value);
    return fixture_destroy(&test_fixture) && is_expected;
}

static bool parses_by_default(const char *json_text, const char *expected_text) {
    return parses_to(&(parse_case){.json_text = json_text, .options = default_options},
                     expected_text);
}

static void test_parse_values(sxml_test_state *state) {
    SXML_TEST_EXPECT(state, parses_by_default("{}", "{}"));
    SXML_TEST_EXPECT(state, parses_by_default("[1,2,3, \"abc\", \"def\", true, false, null]",
                                              "[1.0e0,2.0e0,3.0e0,\"abc\",\"def\",true(),"
                                              "false(),()]"));
    SXML_TEST_EXPECT(state, parses_by_default(" {\"x\" : {\"b\": [], \"a\": {}}, \"y\":-0.5e1}\n",
                                              "{\"x\":{\"b\":[],\"a\":{}},\"y\":-5.0e0}"));
    SXML_TEST_EXPECT(state, parses_by_default("null", ""));
    SXML_TEST_EXPECT(state, parses_by_default("\"\"", "\"\""));
    SXML_TEST_EXPECT(state, parses_by_default("\xEF\xBB\xBF[0]", "[0.0e0]"));
    /* Out of range numbers become infinities (F&O 4.0, 17.4.4, note). */
    SXML_TEST_EXPECT(state, parses_by_default("[1e400, -1e400]",
                                              "[INF,-INF]"));
}

static void test_parse_strings(sxml_test_state *state) {
    /* Escape sequences, a surrogate pair (U+1D11E), unpaired surrogates and
       U+0000 as U+FFFD (F&O 4.0, 17.4.3). */
    SXML_TEST_EXPECT(state,
                     parses_by_default("[\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t" ESCAPE_U "0041\"]",
                                       "[\"a\"\"b\\c/d\b\f\n\r\tA\"]"));
    SXML_TEST_EXPECT(state, parses_by_default("[\"" ESCAPE_U "D834" ESCAPE_U "dD1E\"]",
                                              "[\"\xF0\x9D\x84\x9E\"]"));
    SXML_TEST_EXPECT(state, parses_by_default("[\"" ESCAPE_U "D834x" ESCAPE_U "DD1E" ESCAPE_U
                                              "0000\"]",
                                              "[\"\xEF\xBF\xBDx\xEF\xBF\xBD\xEF\xBF\xBD\"]"));
    SXML_TEST_EXPECT(state, parses_by_default("[\"" ESCAPE_U "D834" ESCAPE_U "D834" ESCAPE_U
                                              "DD1E\"]",
                                              "[\"\xEF\xBF\xBD\xF0\x9D\x84\x9E\"]"));
    /* Escaped form: special characters stay escapes, others are decoded
       (option escape). */
    sxml_json_parse_options escaped = default_options;
    escaped.keeps_escaped_form = true;
    SXML_TEST_EXPECT(state,
                     parses_to(&(parse_case){.json_text = "[\"\\\\" ESCAPE_U "0001" ESCAPE_U
                                                          "0041\\n\\/" ESCAPE_U "d800\xC2\x80\"]",
                                             .options = escaped},
                               "[\"\\\\" ESCAPE_U "0001A\\n/" ESCAPE_U "D800" ESCAPE_U "0080\"]"));
}

/* The escapes a fallback is called for: not permitted, surrogates paired
   as parsing pairs them, escaped backslashes skipped. */
static void test_find_unpermitted_escapes(sxml_test_state *state) {
    const sxml_byte_view text = text_view("[\"\\\\u0000 " ESCAPE_U "D834" ESCAPE_U
                                          "DD1E\\n" ESCAPE_U "dd1e" ESCAPE_U "FFFE" ESCAPE_U
                                          "0041\"]");
    sxml_json_unpermitted_escape escape = sxml_json_find_unpermitted_escape(
        (sxml_json_escape_search){.json_text = text, .start_offset = 0});
    SXML_TEST_EXPECT(state, escape.is_found && escape.codepoint == 0xDD1EU &&
                                escape.escape_sequence.byte_count == 6 &&
                                memcmp(escape.escape_sequence.bytes, ESCAPE_U "dd1e", 6) == 0);
    escape = sxml_json_find_unpermitted_escape(
        (sxml_json_escape_search){.json_text = text, .start_offset = escape.next_offset});
    SXML_TEST_EXPECT(state, escape.is_found && escape.codepoint == 0xFFFEU);
    escape = sxml_json_find_unpermitted_escape(
        (sxml_json_escape_search){.json_text = text, .start_offset = escape.next_offset});
    SXML_TEST_EXPECT(state, !escape.is_found && escape.next_offset == text.byte_count);
    /* A replacement for a codepoint is inserted for each of its escapes. */
    const sxml_json_replacement replacement = {
        .codepoint = 0xFFFEU, .replacement_text = SXML_BYTE_VIEW_FROM_LITERAL("<>")};
    sxml_json_parse_options replacing = default_options;
    replacing.replacements =
        (sxml_json_replacement_view){.replacements = &replacement, .replacement_count = 1};
    SXML_TEST_EXPECT(state, parses_to(&(parse_case){.json_text = "[\"" ESCAPE_U "FFFEa" ESCAPE_U
                                                                 "fffe" ESCAPE_U "FFFF\"]",
                                                    .options = replacing},
                                      "[\"<>a<>\xEF\xBF\xBD\"]"));
}

static void test_parse_numbers(sxml_test_state *state) {
    sxml_json_parse_options decimal = default_options;
    decimal.number_format = SXML_JSON_NUMBER_FORMAT_DECIMAL;
    SXML_TEST_EXPECT(state, parses_to(&(parse_case){.json_text = "[1, 2.01, 1.00, -0, 1e2, "
                                                                 "12345678901234567890123]",
                                                    .options = decimal},
                                      "[1,2.01,1,0,100,12345678901234567890123]"));
    SXML_TEST_EXPECT(state, parses_to(&(parse_case){.json_text = "[1e400]", .options = decimal},
                                      "!FOCA0002"));
    sxml_json_parse_options adaptive = default_options;
    adaptive.number_format = SXML_JSON_NUMBER_FORMAT_ADAPTIVE;
    SXML_TEST_EXPECT(state, parses_to(&(parse_case){.json_text = "[1, 2.5, 3E1]",
                                                    .options = adaptive},
                                      "[1,2.5,3.0e1]"));
}

static void test_parse_duplicates_and_null(sxml_test_state *state) {
    const char *text = "{\"a\":1, \"b\":2, \"a\":3}";
    SXML_TEST_EXPECT(state, parses_by_default(text, "{\"a\":1.0e0,\"b\":2.0e0}"));
    sxml_json_parse_options options = default_options;
    options.duplicates_policy = SXML_JSON_DUPLICATES_POLICY_USE_LAST;
    SXML_TEST_EXPECT(state, parses_to(&(parse_case){.json_text = text, .options = options},
                                      "{\"a\":3.0e0,\"b\":2.0e0}"));
    options.duplicates_policy = SXML_JSON_DUPLICATES_POLICY_REJECT;
    SXML_TEST_EXPECT(state,
                     parses_to(&(parse_case){.json_text = text, .options = options}, "!FOJS0003"));
    /* Keys equal after decoding are duplicates. */
    SXML_TEST_EXPECT(state, parses_to(&(parse_case){.json_text = "{\"a\":1,\"" ESCAPE_U
                                                                 "0061\":2}",
                                                    .options = options},
                                      "!FOJS0003"));
    options = default_options;
    options.null_value = sxml_sequence_from_item(sxml_atomic_item_from_integer(0));
    SXML_TEST_EXPECT(state, parses_to(&(parse_case){.json_text = "[null, {\"n\": null}]",
                                                    .options = options},
                                      "[0,{\"n\":0}]"));
}

static void test_parse_errors(sxml_test_state *state) {
    static const char *const invalid_texts[] = {
        "",      " ",        "[\"abc]",  "[1.234f0]", "1.e2",   "01",       "[1,]",
        "{\"a\" 1}", "\"a\tb\"", "+1",   "[.5]",      "nul",    "truex",    "{\"a\":1,}",
        "\"\\x\"", "[1] 2", "{1:2}", "[1 2]", "TRUE", "[-]", "1e",
        "\"\x01\"",
    };
    for (size_t text_index = 0; text_index < SXML_TEST_COUNT(invalid_texts); text_index += 1) {
        const bool is_refused = parses_by_default(invalid_texts[text_index], "!FOJS0001");
        (void)sxml_test_record_check(
            state, (sxml_test_check){.passed = is_refused,
                                     .expression = invalid_texts[text_index],
                                     .file_name = __FILE__,
                                     .line_number = (uint32_t)__LINE__});
    }
    SXML_TEST_EXPECT(state, parses_by_default("\"" ESCAPE_U "12\"", "!FOJS0001"));
    /* U+0000 in the text and the error's place in the description. */
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char with_nul[] = "[1,\n\x00]";
    sxml_error_record error_record = {.value = NULL};
    sxml_sequence value = sxml_sequence_empty();
    SXML_TEST_EXPECT(state,
                     sxml_json_make_value(test_fixture.item_heap,
                                          &(sxml_json_text_request){
                                              .json_text = {.bytes = (const uint8_t *)with_nul,
                                                            .byte_count = sizeof(with_nul) - 1},
                                              .options = default_options,
                                              .allocator = fixture_allocator(&test_fixture),
                                              .error_record = &error_record},
                                          &value) == SXML_STATUS_DYNAMIC_ERROR &&
                         strcmp(read_code(&error_record).text, "FOJS0001") == 0);
    SXML_TEST_EXPECT(state,
                     sxml_json_make_value(test_fixture.item_heap,
                                          &(sxml_json_text_request){
                                              .json_text = text_view("{\n  \"\xC3\xA9\": tru }"),
                                              .options = default_options,
                                              .allocator = fixture_allocator(&test_fixture),
                                              .error_record = &error_record},
                                          &value) == SXML_STATUS_DYNAMIC_ERROR);
    const sxml_byte_view description = error_record.diagnostic.description;
    char description_text[256] = {0};
    if (description.byte_count < sizeof(description_text)) {
        memcpy(description_text, description.bytes, description.byte_count);
    }
    SXML_TEST_EXPECT(state, strstr(description_text, "line 2, column 8") != NULL);
    SXML_TEST_EXPECT(state,
                     sxml_json_make_value(test_fixture.item_heap,
                                          &(sxml_json_text_request){
                                              .json_text = text_view("[]"),
                                              .options = default_options,
                                              .allocator = {.state = NULL, .operations = NULL}},
                                          &value) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Deep nesting costs heap, not C stack (D71); a limit bounds it. */
static void test_parse_depth(sxml_test_state *state) {
    enum { SXML_TEST_DEPTH = 100000 };
    /* Static: the 64 KiB stack of the WebAssembly tests is not for it. */
    static char text[2 * SXML_TEST_DEPTH + 1];
    memset(text, '[', SXML_TEST_DEPTH);
    memset(text + (size_t)SXML_TEST_DEPTH, ']', SXML_TEST_DEPTH);
    text[(size_t)2 * SXML_TEST_DEPTH] = '\0';
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_sequence value = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_json_make_value(test_fixture.item_heap,
                                                 &(sxml_json_text_request){
                                                     .json_text = text_view(text),
                                                     .options = default_options,
                                                     .allocator = fixture_allocator(&test_fixture)},
                                                 &value) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, serialize(&test_fixture, value) == SXML_STATUS_OK &&
                                sxml_byte_view_equals(sxml_output_buffer_get_bytes(
                                                          test_fixture.output_buffer),
                                                      text_view(text)));
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    SXML_TEST_EXPECT(state, parses_to(&(parse_case){.json_text = "[[[1]]]",
                                                    .options = default_options,
                                                    .maximum_depth = 3},
                                      "[[[1.0e0]]]"));
    SXML_TEST_EXPECT(state, parses_to(&(parse_case){.json_text = "[[[{}]]]",
                                                    .options = default_options,
                                                    .maximum_depth = 3},
                                      "!LIMIT"));
}

/* Every allocation failure of a parse leaks nothing. */
static void test_parse_survives_every_allocation_failure(sxml_test_state *state) {
    static const char text[] = "{\"a\": [1, \"two\", {\"b\": null, \"c\": [true]}], "
                               "\"d\": \"" ESCAPE_U "00e9\", \"a\": 3}";
    bool has_succeeded = false;
    for (uint64_t failing_number = 1; !has_succeeded && failing_number < 2000;
         failing_number += 1) {
        fixture test_fixture;
        if (fixture_create(failing_number, &test_fixture) != SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
            continue;
        }
        sxml_sequence value = sxml_sequence_empty();
        const sxml_status status = sxml_json_make_value(
            test_fixture.item_heap,
            &(sxml_json_text_request){.json_text = text_view(text),
                                      .options = default_options,
                                      .allocator = fixture_allocator(&test_fixture)},
            &value);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        has_succeeded = status == SXML_STATUS_OK;
        sxml_sequence_release(value);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

/* --------------------------------------- The XML representation of JSON */

/* Parses json_text into a document node of the XML representation. */
static sxml_status build_representation(fixture *test_fixture, const parse_case *test_case,
                                        sxml_item *document) {
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
    sxml_error_record error_record = {.value = NULL};
    status = builder.operations->start_document(builder.state, &(sxml_document_start_event){0});
    if (status == SXML_STATUS_OK) {
        status = sxml_json_send_xml_events(
            test_fixture->item_heap,
            &(sxml_json_text_request){.json_text = text_view(test_case->json_text),
                                      .options = test_case->options,
                                      .allocator = fixture_allocator(test_fixture),
                                      .maximum_depth = test_case->maximum_depth,
                                      .error_record = &error_record},
            builder);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
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

static sxml_json_parse_options xml_options(sxml_json_duplicates_policy policy) {
    return (sxml_json_parse_options){.duplicates_policy = policy,
                                     .keeps_escaped_form = false,
                                     .number_format = SXML_JSON_NUMBER_FORMAT_DOUBLE,
                                     .null_value = {.content = {.representation = 0}}};
}

static sxml_json_parse_options escaped_xml_options(sxml_json_duplicates_policy policy) {
    sxml_json_parse_options options = xml_options(policy);
    options.keeps_escaped_form = true;
    return options;
}

/* The XML of test_case's representation, or "!CODE". */
static bool converts_to(const parse_case *test_case, const char *expected_text) {
    fixture test_fixture;
    if (fixture_create(0, &test_fixture) != SXML_STATUS_OK) {
        (void)fixture_destroy(&test_fixture);
        return false;
    }
    sxml_item document = {.representation = 0};
    sxml_status status = build_representation(&test_fixture, test_case, &document);
    bool is_expected = false;
    if (expected_text[0] == '!') {
        is_expected = status == SXML_STATUS_DYNAMIC_ERROR;
    } else if (status == SXML_STATUS_OK) {
        status = serialize(&test_fixture, sxml_sequence_from_item(document));
        is_expected = status == SXML_STATUS_OK && buffer_holds(&test_fixture, expected_text);
    }
    if (!is_expected) {
        (void)fprintf(stderr, "  for %s: status %d\n", test_case->json_text, (int)status);
    }
    sxml_item_release(document);
    return fixture_destroy(&test_fixture) && is_expected;
}

#define FN_XMLNS " xmlns=\"http://www.w3.org/2005/xpath-functions\""

static void test_json_to_xml_events(sxml_test_state *state) {
    const sxml_json_parse_options retain = xml_options(SXML_JSON_DUPLICATES_POLICY_RETAIN);
    SXML_TEST_EXPECT(state,
                     converts_to(&(parse_case){.json_text = "{ \"x\": 1, \"y\": [ 3, 4, 5 ] }",
                                               .options = retain},
                                 "<map" FN_XMLNS "><number key=\"x\">1</number><array key=\"y\">"
                                 "<number>3</number><number>4</number><number>5</number></array>"
                                 "</map>"));
    SXML_TEST_EXPECT(state, converts_to(&(parse_case){.json_text = "[\"abcd\", true, false, null, "
                                                                   "-1.5E+3, {}]",
                                                      .options = retain},
                                        "<array" FN_XMLNS "><string>abcd</string><boolean>true"
                                        "</boolean><boolean>false</boolean><null/><number>-1.5E+3"
                                        "</number><map/></array>"));
    SXML_TEST_EXPECT(state, converts_to(&(parse_case){.json_text = "\"\"", .options = retain},
                                        "<string" FN_XMLNS "/>"));
    /* F&O 4.0, 17.4.6, examples: unescaped and escaped forms. */
    const char *escapes = "{ \"x\": \"\\\\\", \"y\": \"" ESCAPE_U "0025\" }";
    SXML_TEST_EXPECT(state, converts_to(&(parse_case){.json_text = escapes, .options = retain},
                                        "<map" FN_XMLNS "><string key=\"x\">\\</string>"
                                        "<string key=\"y\">%</string></map>"));
    SXML_TEST_EXPECT(state,
                     converts_to(&(parse_case){.json_text = escapes,
                                               .options = escaped_xml_options(SXML_JSON_DUPLICATES_POLICY_RETAIN)},
                                 "<map" FN_XMLNS "><string key=\"x\" escaped=\"true\">\\\\</string>"
                                 "<string key=\"y\">%</string></map>"));
    SXML_TEST_EXPECT(state,
                     converts_to(&(parse_case){.json_text = "{\"a\\nb\": 1}",
                                               .options = escaped_xml_options(SXML_JSON_DUPLICATES_POLICY_RETAIN)},
                                 "<map" FN_XMLNS "><number key=\"a\\nb\" escaped-key=\"true\">1"
                                 "</number></map>"));
    /* Duplicates: retained, the first kept (its value skipped whole), or
       FOJS0003. */
    const char *duplicates = "{\"a\": 1, \"a\": {\"b\": [2]}, \"c\": {\"a\": 3}}";
    SXML_TEST_EXPECT(state, converts_to(&(parse_case){.json_text = duplicates, .options = retain},
                                        "<map" FN_XMLNS "><number key=\"a\">1</number><map key=\"a\">"
                                        "<array key=\"b\"><number>2</number></array></map>"
                                        "<map key=\"c\"><number key=\"a\">3</number></map></map>"));
    SXML_TEST_EXPECT(state,
                     converts_to(&(parse_case){.json_text = duplicates,
                                               .options = xml_options(SXML_JSON_DUPLICATES_POLICY_USE_FIRST)},
                                 "<map" FN_XMLNS "><number key=\"a\">1</number>"
                                 "<map key=\"c\"><number key=\"a\">3</number></map></map>"));
    SXML_TEST_EXPECT(state,
                     converts_to(&(parse_case){.json_text = duplicates,
                                               .options = xml_options(SXML_JSON_DUPLICATES_POLICY_REJECT)},
                                 "!FOJS0003"));
    SXML_TEST_EXPECT(state, converts_to(&(parse_case){.json_text = "[1,]", .options = retain},
                                        "!FOJS0001"));
}

/* ----------------------------------------------------- Writing back */

/* Parses xml_text into a document node (the test's own loader too). */
typedef struct xml_source {
    fixture *test_fixture;
    sxml_byte_view xml_text;
} xml_source;

static sxml_status parse_xml_text(xml_source source, sxml_item *document) {
    fixture *test_fixture = source.test_fixture;
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
        status = sxml_xml_parser_parse(parser, &(sxml_xml_parse_request){.input = source.xml_text},
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

/* The JSON text xml-to-json writes for xml_text, or "!CODE". */
typedef struct writing_case {
    const char *xml_text;
    bool keeps_solidus;
} writing_case;

static bool writes_json(writing_case test_case, const char *expected_text) {
    fixture test_fixture;
    if (fixture_create(0, &test_fixture) != SXML_STATUS_OK) {
        (void)fixture_destroy(&test_fixture);
        return false;
    }
    sxml_item document = {.representation = 0};
    sxml_status status = parse_xml_text(
        (xml_source){.test_fixture = &test_fixture, .xml_text = text_view(test_case.xml_text)},
        &document);
    sxml_node_reference node = {.tree = NULL};
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(document, &node);
    }
    sxml_error_record error_record = {.value = NULL};
    if (status == SXML_STATUS_OK) {
        sxml_output_buffer_clear(test_fixture.output_buffer);
        status = sxml_json_write_xml_representation(
            test_fixture.item_heap,
            &(sxml_json_writing_request){.node = node,
                                         .escapes_solidus = !test_case.keeps_solidus,
                                         .allocator = fixture_allocator(&test_fixture),
                                         .error_record = &error_record},
            sxml_output_buffer_as_output_stream(test_fixture.output_buffer));
    }
    bool is_expected = false;
    if (expected_text[0] == '!') {
        is_expected = status == SXML_STATUS_DYNAMIC_ERROR &&
                      strcmp(read_code(&error_record).text, expected_text + 1) == 0;
    } else {
        is_expected = status == SXML_STATUS_OK && buffer_holds(&test_fixture, expected_text);
    }
    if (!is_expected) {
        (void)fprintf(stderr, "  for %s: status %d %s\n", test_case.xml_text, (int)status,
                      read_code(&error_record).text);
    }
    sxml_item_release(document);
    return fixture_destroy(&test_fixture) && is_expected;
}

static bool writes_by_default(const char *xml_text, const char *expected_text) {
    return writes_json((writing_case){.xml_text = xml_text, .keeps_solidus = false},
                       expected_text);
}

#define FN_ELEMENT(name) "<" name FN_XMLNS

static void test_xml_to_json_writing(sxml_test_state *state) {
    /* F&O 4.0, 17.4.7, examples. */
    SXML_TEST_EXPECT(state, writes_by_default(FN_ELEMENT("array") "><number>1</number><string>is"
                                                                  "</string><boolean>1</boolean>"
                                                                  "</array>",
                                              "[1,\"is\",true]"));
    SXML_TEST_EXPECT(state, writes_by_default(FN_ELEMENT("map") "><number key=\"Sunday\">1</number>"
                                                                "<number key=\"Monday\">2</number>"
                                                                "</map>",
                                              "{\"Sunday\":1,\"Monday\":2}"));
    SXML_TEST_EXPECT(state, writes_by_default(FN_ELEMENT("array") "><number>10</number><number>17e2"
                                                                  "</number><number> +0005 "
                                                                  "</number><number>.5</number>"
                                                                  "<number>-5.</number></array>",
                                              "[10,17e2,5,0.5,-5.0]"));
    /* Whitespace, comments and foreign attributes are ignored; the top
       element's key too. */
    SXML_TEST_EXPECT(state,
                     writes_by_default("<f:map xmlns:f=\"http://www.w3.org/2005/xpath-functions\" "
                                       "xmlns:o=\"urn:o\" key=\"top\" o:x=\"1\">\n <!--c--> "
                                       "<f:null key=\"n\"/> <f:boolean key=\"b\"> false "
                                       "</f:boolean></f:map>",
                                       "{\"n\":null,\"b\":false}"));
    /* Strings: escaping without and with escaped="true". */
    SXML_TEST_EXPECT(state,
                     writes_by_default(FN_ELEMENT("string") ">a\\b\"c/&#x9;&#xA;&#x7F;\xC2\x80"
                                                            "</string>",
                                       "\"a\\\\b\\\"c\\/\\t\\n" ESCAPE_U "007F" ESCAPE_U "0080\""));
    SXML_TEST_EXPECT(state, writes_json((writing_case){.xml_text = FN_ELEMENT("string") ">a/b"
                                                                                       "</string>",
                                                       .keeps_solidus = true},
                                        "\"a/b\""));
    SXML_TEST_EXPECT(state, writes_by_default(FN_ELEMENT("string") " escaped=\"true\">\\n\\" ESCAPE_U
                                                                   "00e9\"</string>",
                                              "\"\\n\\" ESCAPE_U "00e9\\\"\""));
    SXML_TEST_EXPECT(state, writes_by_default(FN_ELEMENT("string") " escaped=\"1\">\\x</string>",
                                              "!FOJS0007"));
    SXML_TEST_EXPECT(state, writes_by_default(FN_ELEMENT("string") " escaped=\"true\">" ESCAPE_U
                                                                   "12</string>",
                                              "!FOJS0007"));
    /* Not the XML representation of JSON: FOJS0006. */
    static const char *const invalid_texts[] = {
        FN_ELEMENT("number") ">-INF</number>",
        FN_ELEMENT("number") ">NaN</number>",
        FN_ELEMENT("number") ">1e</number>",
        FN_ELEMENT("number") ">.</number>",
        FN_ELEMENT("boolean") ">yes</boolean>",
        FN_ELEMENT("object") "/>",
        "<map/>",
        FN_ELEMENT("map") "><number>1</number></map>",
        FN_ELEMENT("array") "><number key=\"a\">1</number></array>",
        FN_ELEMENT("array") ">x<null/></array>",
        FN_ELEMENT("null") ">x</null>",
        FN_ELEMENT("string") "><null/></string>",
        FN_ELEMENT("number") " escaped=\"true\">1</number>",
        FN_ELEMENT("null") " other=\"1\"/>",
        FN_ELEMENT("map") "><null key=\"a\"/><null key=\"a\"/></map>",
        FN_ELEMENT("map") "><null key=\"\\n\" escaped-key=\"true\"/><null key=\"&#xA;\"/></map>",
        FN_ELEMENT("map") "><null escaped-key=\"true\"/></map>",
    };
    for (size_t text_index = 0; text_index < SXML_TEST_COUNT(invalid_texts); text_index += 1) {
        const bool is_refused = writes_by_default(invalid_texts[text_index], "!FOJS0006");
        (void)sxml_test_record_check(
            state, (sxml_test_check){.passed = is_refused,
                                     .expression = invalid_texts[text_index],
                                     .file_name = __FILE__,
                                     .line_number = (uint32_t)__LINE__});
    }
    /* The same key in two different maps is no duplicate. */
    SXML_TEST_EXPECT(state, writes_by_default(FN_ELEMENT("map") "><map key=\"a\"><null key=\"a\"/>"
                                                                "</map><null key=\"b\"/></map>",
                                              "{\"a\":{\"a\":null},\"b\":null}"));
}

/* ------------------------------------------------- Function library */

static const sxml_static_error test_unavailable_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER("The test loader has no documents"),
};

static const sxml_static_error test_not_well_formed_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER("Not a well-formed XML document"),
};

static sxml_status load_test_document(void *state, const sxml_document_request *request,
                                      sxml_item *document_node) {
    (void)state;
    (void)document_node;
    return sxml_error_record_report(request->error_record, &test_unavailable_error);
}

/* A loader double for fn:parse-xml: the XML parser into a new arena
   tree (the processor's loader is packet 1.23's). */
static sxml_status parse_test_document(void *state,
                                       const sxml_document_parse_request *request,
                                       sxml_item *document_node) {
    const sxml_status status = parse_xml_text(
        (xml_source){.test_fixture = state, .xml_text = request->xml_bytes}, document_node);
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8) {
        return sxml_error_record_report(request->error_record, &test_not_well_formed_error);
    }
    return status;
}

static const sxml_document_loader_operations test_loader_operations = {
    .load_document = load_test_document,
    .parse_document = parse_test_document,
};

/* Compiles and evaluates expression_text; on success the adaptive form of
   the value is in the fixture's buffer. */
static sxml_status evaluate(fixture *test_fixture, const char *expression_text,
                            sxml_error_record *error_record) {
    const sxml_xpath_static_context static_context = {
        .item_heap = test_fixture->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .function_table = sxml_function_library_as_table(test_fixture->library),
    };
    sxml_ir_unit *unit = NULL;
    sxml_status status = sxml_xpath_compile(
        &(sxml_xpath_compile_request){.expression_text = text_view(expression_text),
                                      .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xpath"),
                                      .static_context = &static_context,
                                      .error_record = error_record},
        &unit);
    sxml_dynamic_context *dynamic_context = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_create(
            fixture_allocator(test_fixture),
            &(sxml_evaluator_configuration){
                .item_heap = test_fixture->item_heap,
                .allocator = fixture_allocator(test_fixture),
                .document_loader = {.state = test_fixture, .operations = &test_loader_operations},
                .function_table = static_context.function_table},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit, .error_record = error_record}, &value);
    }
    if (status == SXML_STATUS_OK) {
        status = serialize(test_fixture, value);
    }
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_ir_unit_release(unit);
    return status;
}

/* True when expression_text gives expected_text in adaptive form, or
   "!CODE" for an error, and nothing leaks. */
static bool evaluates_to(const char *expression_text, const char *expected_text) {
    fixture test_fixture;
    if (fixture_create(0, &test_fixture) != SXML_STATUS_OK) {
        (void)fixture_destroy(&test_fixture);
        return false;
    }
    sxml_error_record error_record = {.value = NULL};
    const sxml_status status = evaluate(&test_fixture, expression_text, &error_record);
    bool is_expected = false;
    if (expected_text[0] == '!') {
        is_expected = sxml_status_has_error_record(status) &&
                      strcmp(read_code(&error_record).text, expected_text + 1) == 0;
    } else {
        is_expected = status == SXML_STATUS_OK && buffer_holds(&test_fixture, expected_text);
    }
    if (!is_expected) {
        (void)fprintf(stderr, "  for %s: status %d %s\n", expression_text, (int)status,
                      sxml_status_has_error_record(status) ? read_code(&error_record).text : "");
    }
    return fixture_destroy(&test_fixture) && is_expected;
}

/* An expression and what it gives. */
typedef struct expression_case {
    const char *expression_text;
    const char *expected_text;
} expression_case;

static void expect_all(sxml_test_state *state, const expression_case *cases,
                       size_t case_count) {
    for (size_t case_index = 0; case_index < case_count; case_index += 1) {
        const bool passed =
            evaluates_to(cases[case_index].expression_text, cases[case_index].expected_text);
        (void)sxml_test_record_check(
            state, (sxml_test_check){.passed = passed,
                                     .expression = cases[case_index].expression_text,
                                     .file_name = __FILE__,
                                     .line_number = (uint32_t)__LINE__});
    }
}

/* The XInclude namespace, for the parse-xml cases. */
#define XINCLUDE "http://www.w3.org/2001/XInclude"

/* The JSON lines of tests/qt4/torture-slice.txt, named by test case. */
static const expression_case slice_cases[] = {
    /* fn-parse-json-001 */
    {"parse-json(\"{}\")", "{}"},
    /* fn-parse-json-017 */
    {"let $result := parse-json('[1,2,3, \"abc\", \"def\", true, false, null]') return "
     "($result instance of array(*), count($result), array:size($result) eq 8, $result?1 eq 1, "
     "$result?4 eq \"abc\", $result?6 eq true(), $result?7 eq false(), $result?8 => empty())",
     "true() 1 true() true() true() true() true() true()"},
    /* fn-parse-json-021 */
    {"let $result := parse-json('[{\"x\":12,\"y\":5}, {\"x\":13,\"y\":6}]') return "
     "(array:size($result) = 2, $result?2?y = 6)",
     "true() true()"},
    /* fn-parse-json-023 */
    {"let $result := parse-json('[0.123]') return ($result?1 = 0.123e0, $result?1 instance of "
     "xs:double)",
     "true() true()"},
    /* fn-parse-json-034 */
    {"let $result := parse-json('[\"" ESCAPE_U "D834" ESCAPE_U "dD1E\"]') return ($result?1 = "
     "codepoints-to-string(119070), $result?1 instance of xs:string)",
     "true() true()"},
    /* fn-parse-json-040 */
    {"parse-json('null')", ""},
    /* fn-parse-json-050 */
    {"parse-json('{\"a\":1, \"b\":2, \"a\":3}', map{'duplicates':'use-first'})",
     "{\"a\":1.0e0,\"b\":2.0e0}"},
    /* fn-parse-json-052 */
    {"deep-equal(parse-json('{\"a\":1, \"b\":2, \"a\":3}', map{}), map{\"a\":1, \"b\":2})",
     "true()"},
    /* fn-parse-json-709 */
    {"let $result := '{\"a\":1, \"b\":2.01}' => parse-json({ 'number-format': 'decimal'}) "
     "return ($result?a = 1, $result?b = 2.01, $result?a instance of xs:integer, $result?b "
     "instance of xs:decimal)",
     "true() true() true() true()"},
    /* fn-parse-json-740 */
    {"'[\"a\", null, \"b\"]' => parse-json(map { 'null': ()})", "[\"a\",(),\"b\"]"},
    /* fn-parse-json-908 */
    {"parse-json('[\"abc]')", "!FOJS0001"},
    /* fn-parse-json-934 */
    {"parse-json('[1.234f0]')", "!FOJS0001"},
    /* fn-json-to-xml-empty */
    {"local-name(json-to-xml('\"\"', ())/*)", "\"string\""},
    /* json-to-xml-error-033 */
    {"fn:json-to-xml('1.e2')", "!FOJS0001"},
    /* xml-to-json-012 */
    {"xml-to-json(json-to-xml('[\"A\", \"B\", \"C\", \"D\", \"E\"]'))",
     "\"[\"\"A\"\",\"\"B\"\",\"\"C\"\",\"\"D\"\",\"\"E\"\"]\""},
    /* xml-to-json-015 */
    {"xml-to-json(json-to-xml('{\"A\":1, \"B\":2, \"C\":3, \"D\":4, \"E\":5}')) eq "
     "'{\"A\":1,\"B\":2,\"C\":3,\"D\":4,\"E\":5}'",
     "true()"},
    /* xml-to-json-017 */
    {"xml-to-json(json-to-xml('\"\\\\\\/\\\"\\r\\t\\n" ESCAPE_U "0020\"')) eq "
     "'\"\\\\\\/\\\"\\r\\t\\n \"'",
     "true()"},
    /* xml-to-json-403 */
    {"parse-xml(\"<fn:number xmlns:fn='http://www.w3.org/2005/xpath-functions'>-INF</fn:number>\") "
     "=> xml-to-json()",
     "!FOJS0006"},
    /* op-union-406 */
    {"let $in1 := jtree({\"a\":1, \"b\":2}) let $in2 := jtree({\"b\":2, \"a\":1}) return "
     "count($in1 union $in2)",
     "2"},
    /* parse-xml-012 (either answer is allowed; sxml makes a new tree) */
    {"parse-xml(\"<a>foo</a>\") is parse-xml(\"<a>foo</a>\")", "false()"},
    /* parse-xml-014 */
    {"parse-xml(\"<a>foo</a>\") instance of document-node()", "true()"},
    /* fn-root-29 */
    {"let $j := jtree([1,2,3]) return $j is root($j/*[3])", "true()"},
};

static void test_slice_lines(sxml_test_state *state) {
    expect_all(state, slice_cases, SXML_TEST_COUNT(slice_cases));
}

static const expression_case function_cases[] = {
    {"parse-json(())", ""},
    {"parse-json('[1, 2]', {'number-format': 'adaptive', 'liberal': true(), 'escape': false()})",
     "[1,2]"},
    {"parse-json('{\"a\":1, \"a\":2}', {'duplicates': 'reject'})", "!FOJS0003"},
    {"parse-json('[]', {'duplicates': 'retain'})", "!FOJS0005"},
    {"parse-json('[]', {'number-format': 'binary'})", "!FOJS0005"},
    {"parse-json('[]', {'unknown': 1})", "!XPTY0004"},
    {"parse-json('[]', {'escape': 'yes'})", "!XPTY0004"},
    {"parse-json('[]', {'null': (1, 2)})", "!XPTY0004"},
    {"parse-json('[]', {xs:QName('fn:x'): 1})", "[]"},
    {"parse-json('[]', {'escape': true(), 'fallback': fn($s) { $s }})", "!FOJS0005"},
    {"parse-json('[\"a\\\\b\"]', {'escape': true()})", "[\"a\\\\b\"]"},
    /* Option fallback (F&O 4.0, 17.4.4): called once per character that is
       not permitted, with its escape sequence; a surrogate pair is no such
       character, and two-character escapes never are. */
    {"parse-json('[\"" ESCAPE_U "0000\", \"" ESCAPE_U "D800" ESCAPE_U "DC00" ESCAPE_U
     "DEAD\\b\", \"" ESCAPE_U "dead\"]', {'fallback': fn($s) { '[' || $s || ']' }})",
     "[\"[" ESCAPE_U "0000]\",\"\xF0\x90\x80\x80[" ESCAPE_U "DEAD]\b\",\"[" ESCAPE_U
     "DEAD]\"]"},
    {"json-to-xml('\"" ESCAPE_U "FFFF\"', {'fallback': fn($s) { 'x' }})/*/string()", "\"x\""},
    {"parse-json('\"" ESCAPE_U "FFFF\"', {'fallback': substring#2})", "!XPTY0004"},
    {"parse-json('\"" ESCAPE_U "FFFF\"', {'fallback': fn($s) { () }})", "!XPTY0004"},
    {"parse-json('\"" ESCAPE_U "FFFF\"', {'fallback': fn($s) { error() }})", "!FOER0000"},
    {"parse-json('{\"x\": null}', {'null': #fn:null})", "{\"x\":#fn:null}"},
    {"json-to-xml(())", ""},
    {"json-to-xml('{\"a\":1, \"a\":2}', {'duplicates': 'use-last'})", "!FOJS0005"},
    /* validate=true types the result by F&O 4.0, D.2, which needs a
       processor (this fixture has none: FOJS0004); duplicates are
       rejected then, and retain is FOJS0005 (17.4.6). */
    {"json-to-xml('[1]', {'validate': true()})", "!FOJS0004"},
    {"json-to-xml('{\"a\":1, \"a\":2}', {'validate': true()})", "!FOJS0003"},
    {"json-to-xml('{}', {'validate': true(), 'duplicates': 'retain'})", "!FOJS0005"},
    {"json-to-xml('{\"a\": [true, null]}')",
     "<map" FN_XMLNS "><array key=\"a\"><boolean>true</boolean><null/></array></map>"},
    {"json-to-xml('[1]')/*/*/string()", "\"1\""},
    {"xml-to-json(())", ""},
    {"xml-to-json(json-to-xml('{\"x\": \"a/b\"}'), {'escape-solidus': false(), 'indent': true()})",
     "\"{\n  \"\"x\"\": \"\"a/b\"\"\n}\""},
    /* indent: a member per line, two spaces per level; empty maps and
       arrays stay on one line. */
    {"xml-to-json(json-to-xml('{\"a\": [1, {}]}'), {'indent': true()})",
     "\"{\n  \"\"a\"\": [\n    1,\n    {}\n  ]\n}\""},
    {"xml-to-json(json-to-xml('\"" ESCAPE_U "0001\"', {'escape': true()}))",
     "\"\"\"" ESCAPE_U "0001\"\"\""},
    {"jtree(())", "jtree(())"},
    {"jtree({'a': [4, 5]})/a", "jnode(\"a\":[4,5])"},
    {"parse-xml(())", ""},
    {"parse-xml('<a x=\"1\"><b/></a>')", "<a x=\"1\"><b/></a>"},
    {"parse-xml('<a>')", "!FODC0006"},
    /* The option reaches the loader (D199), which validates; this test's
       loader does not: tests/xpath_test.c checks the processor's. */
    {"parse-xml('<a/>', {'dtd-validation': true()})", "<a/>"},
    /* XInclude (src/xinclude_internal.h): this test's loader has no
       documents, so an include reads its fallback or fails (FODC0002);
       without trust-external an include is FODC0016. */
    {"parse-xml('<a/>', {'xinclude': true()})", "<a/>"},
    {"parse-xml('<a xmlns:xi=\"" XINCLUDE "\"><xi:include href=\"b.xml\"/></a>', "
     "{'xinclude': true()})",
     "!FODC0016"},
    {"parse-xml('<a xmlns:xi=\"" XINCLUDE "\"><xi:include href=\"b.xml\"><xi:fallback>"
     "<c/>t</xi:fallback></xi:include></a>', {'xinclude': true(), 'trust-external': true()})",
     "<a xmlns:xi=\"" XINCLUDE "\"><c/>t</a>"},
    {"parse-xml('<a xmlns:xi=\"" XINCLUDE "\"><xi:include href=\"b.xml\"/></a>', "
     "{'xinclude': true(), 'trust-external': true()})",
     "!FODC0002"},
    {"parse-xml('<a xmlns:xi=\"" XINCLUDE "\"><xi:fallback/></a>', "
     "{'xinclude': true(), 'trust-external': true()})",
     "!FODC0002"},
    {"parse-xml('<a xmlns:xi=\"" XINCLUDE "\"><xi:include href=\"b.xml\" parse=\"c\"/></a>', "
     "{'xinclude': true(), 'trust-external': true()})",
     "!FODC0002"},
    {"parse-xml('<a xmlns:xi=\"" XINCLUDE "\"><xi:include href=\"b.xml\"/></a>', "
     "{'xinclude': false()})/a/*/name()",
     "\"xi:include\""},
    /* F&O 4.0, fn:parse-xml: strip-space="all" drops whitespace-only text. */
    {"parse-xml('<a> <b/> </a>', {'strip-space': 'all'})", "<a><b/></a>"},
    {"parse-xml('<a/>', {'strip-space': 'some'})", "!XPTY0004"},
    {"parse-xml('<a/>', {'xsd-validation': 'strict'})", "!FODC0014"},
    {"parse-xml('<a/>', {'base-uri': 'http://example.com/', 'retain-location': true()})",
     "<a/>"},
    {"parse-xml(xs:hexBinary('3C612F3E'))", "<a/>"},
    {"parse-xml(1)", "!XPTY0004"},
    {"fn:exists#1", "fn:exists#1"},
};

static void test_functions(sxml_test_state *state) {
    expect_all(state, function_cases, SXML_TEST_COUNT(function_cases));
}

/* Every allocation failure of an evaluation that uses all five functions
   leaks nothing. */
static void test_functions_survive_every_allocation_failure(sxml_test_state *state) {
    static const char expression_text[] =
        "(parse-json('{\"a\": [1, {\"b\": null}]}')?a?2, xml-to-json(json-to-xml('[\"x\", 1]')), "
        "count(jtree([1, 2])/*), parse-xml('<a/>'), "
        "parse-json('\"" ESCAPE_U "0000" ESCAPE_U "FFFF" ESCAPE_U "0000\"', {'fallback': fn($s) { 'f' }}))";
    bool has_succeeded = false;
    for (uint64_t failing_number = 1; !has_succeeded && failing_number < 20000;
         failing_number += 1) {
        fixture test_fixture;
        if (fixture_create(failing_number, &test_fixture) != SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
            continue;
        }
        sxml_error_record error_record = {.value = NULL};
        const sxml_status status = evaluate(&test_fixture, expression_text, &error_record);
        has_succeeded = status == SXML_STATUS_OK;
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (has_succeeded) {
            SXML_TEST_EXPECT(state, buffer_holds(&test_fixture,
                                                 "{\"b\":()} \"[\"\"x\"\",1]\" 2 <a/> \"fff\""));
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

static const sxml_test_case json_test_cases[] = {
    {.name = "parse_values", .function = test_parse_values},
    {.name = "parse_strings", .function = test_parse_strings},
    {.name = "find_unpermitted_escapes", .function = test_find_unpermitted_escapes},
    {.name = "parse_numbers", .function = test_parse_numbers},
    {.name = "parse_duplicates_and_null", .function = test_parse_duplicates_and_null},
    {.name = "parse_errors", .function = test_parse_errors},
    {.name = "parse_depth", .function = test_parse_depth},
    {.name = "parse_survives_every_allocation_failure",
     .function = test_parse_survives_every_allocation_failure},
    {.name = "json_to_xml_events", .function = test_json_to_xml_events},
    {.name = "xml_to_json_writing", .function = test_xml_to_json_writing},
    {.name = "slice_lines", .function = test_slice_lines},
    {.name = "functions", .function = test_functions},
    {.name = "functions_survive_every_allocation_failure",
     .function = test_functions_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "json",
            .cases = json_test_cases,
            .case_count = SXML_TEST_COUNT(json_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
