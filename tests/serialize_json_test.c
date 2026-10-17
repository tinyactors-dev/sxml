/*
 * Tests for the JSON and CSV output methods and the parameter reader
 * (src/serialize_json.c, src/serialize_text.c, src/serialize_parameters.c;
 * Serialization 4.0, sections 3.1, 9 and 10; F&O 4.0, 17.2.3; D128): JSON
 * values, escaping, numbers, duplicate keys, JSON Lines, canonical JSON,
 * nodes by json-node-output-method; CSV rows, quoting and headers; options
 * maps and parameter documents with their errors.
 *
 * Golden file: tests/serialize_xml_test.c (fixture, "!CODE" for an
 * expected error, allocation-failure loop). Expected texts follow the QT4
 * fn-serialize, method-json and method-csv test sets.
 */
#include "sxml/serialize.h"

#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"
#include "test_runner.h"

#include <math.h>
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


#define JSON_METHOD .serialization_method = SXML_SERIALIZATION_METHOD_JSON
#define CSV_METHOD .serialization_method = SXML_SERIALIZATION_METHOD_CSV

/* The array of members, each one item (taken over). */
static sxml_item make_array(fixture *test_fixture, const sxml_item *items, size_t item_count) {
    sxml_sequence members[8];
    for (size_t item_index = 0; item_index < item_count && item_index < 8; item_index += 1) {
        members[item_index] = sxml_sequence_from_item(items[item_index]);
    }
    sxml_item array = sxml_atomic_item_false();
    if (sxml_array_make(test_fixture->item_heap,
                        (sxml_sequence_view){.sequences = members, .sequence_count = item_count},
                        &array) != SXML_STATUS_OK) {
        array = sxml_atomic_item_false();
    }
    for (size_t item_index = 0; item_index < item_count; item_index += 1) {
        sxml_item_release(items[item_index]);
    }
    return array;
}

/* The map of keys to one-item values (all taken over). */
typedef struct map_case {
    const sxml_item *keys;
    const sxml_item *values;
    size_t entry_count;
} map_case;

static sxml_item make_map(fixture *test_fixture, const map_case *test_case) {
    sxml_map_entry entries[8];
    for (size_t entry_index = 0; entry_index < test_case->entry_count; entry_index += 1) {
        entries[entry_index] = (sxml_map_entry){
            .key = test_case->keys[entry_index],
            .value = sxml_sequence_from_item(test_case->values[entry_index])};
    }
    sxml_item map = sxml_atomic_item_false();
    if (sxml_map_make(test_fixture->item_heap,
                      &(sxml_map_request){
                          .map_entries = {.map_entries = entries,
                                          .map_entry_count = test_case->entry_count},
                          .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                          .error_record = NULL},
                      &map) != SXML_STATUS_OK) {
        map = sxml_atomic_item_false();
    }
    for (size_t entry_index = 0; entry_index < test_case->entry_count; entry_index += 1) {
        sxml_item_release(test_case->keys[entry_index]);
        sxml_item_release(test_case->values[entry_index]);
    }
    return map;
}

static sxml_item make_decimal(fixture *test_fixture, const char *text) {
    sxml_item item = sxml_atomic_item_false();
    if (sxml_atomic_item_make(
            test_fixture->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL),
                .datum = {.datum_kind = SXML_DATUM_KIND_DECIMAL,
                          .datum_details = {.decimal_text = text_view(text)}}},
            &item) != SXML_STATUS_OK) {
        return sxml_atomic_item_false();
    }
    return item;
}

static sxml_item make_qname(fixture *test_fixture, const char *namespace_uri,
                            const char *local_name) {
    sxml_namespace_uri_identifier uri = {.index = 0};
    sxml_name_identifier name = {.index = 0};
    sxml_item item = sxml_atomic_item_false();
    if (sxml_name_pool_intern_namespace_uri(test_fixture->name_pool, text_view(namespace_uri),
                                            &uri) != SXML_STATUS_OK ||
        sxml_name_pool_intern_name(
            test_fixture->name_pool,
            &(sxml_name_request){.namespace_uri = uri, .local_name = text_view(local_name)},
            &name) != SXML_STATUS_OK ||
        sxml_atomic_item_make(
            test_fixture->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                          .datum_details = {.qname = {.expanded_name = name,
                                                      .namespace_prefix = {.index = 0}}}}},
            &item) != SXML_STATUS_OK) {
        return sxml_atomic_item_false();
    }
    return item;
}

/* ------------------------------------------------------------ JSON */

static void test_json_values(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    const sxml_serialization_parameters json = {JSON_METHOD};
    /* Section 9.1: the empty sequence is null; more than one item is
       SERE0023. */
    SXML_TEST_EXPECT(state, serialization_gives(current,
                                                &(serialization_case){
                                                    .value = sxml_sequence_empty(),
                                                    .parameters = json},
                                                text_view("null")));
    sxml_item pair[2] = {sxml_atomic_item_from_integer(1), sxml_atomic_item_from_integer(2)};
    sxml_sequence two = make_sequence(current, pair, 2);
    SXML_TEST_EXPECT(state, serialization_gives(current,
                                                &(serialization_case){.value = two,
                                                                      .parameters = json},
                                                text_view("!SERE0023")));
    /* JSON Lines (9.2): one text per line. */
    SXML_TEST_EXPECT(
        state,
        serialization_gives(current,
                            &(serialization_case){
                                .value = two,
                                .parameters = {JSON_METHOD,
                                               .json_lines = SXML_SERIALIZATION_FLAG_YES}},
                            text_view("1\n2")));
    sxml_sequence_release(two);
    /* Arrays, maps in entry order, strings escaped, numbers, booleans. */
    sxml_item members[4] = {make_string(current, "a\"\\/\n\x01\xC2\x85"), make_decimal(current, "1.50"),
                            sxml_atomic_item_from_double(1e23), sxml_atomic_item_true()};
    sxml_item keys[2] = {make_string(current, "z"), make_string(current, "a")};
    sxml_item values[2] = {make_array(current, members, 4), sxml_atomic_item_from_double(NAN)};
    sxml_sequence value = sxml_sequence_from_item(
        make_map(current, &(map_case){.keys = keys, .values = values, .entry_count = 2}));
    SXML_TEST_EXPECT(state,
                     serialization_gives(current,
                                         &(serialization_case){.value = value, .parameters = json},
                                         text_view("{\"z\":[\"a\\\"\\\\\\/\\n\\u0001\\u0085\",1.5,"
                                                   "1.0E23,true],\"a\":null}")));
    /* escape-solidus no; indentation. */
    SXML_TEST_EXPECT(
        state,
        serialization_gives(
            current,
            &(serialization_case){.value = value,
                                  .parameters = {JSON_METHOD,
                                                 .escape_solidus = SXML_SERIALIZATION_FLAG_NO,
                                                 .indent = SXML_SERIALIZATION_FLAG_YES}},
            text_view("{\n  \"z\": [\n    \"a\\\"\\\\/\\n\\u0001\\u0085\",\n    1.5,\n    1.0E23,\n"
                      "    true\n  ],\n  \"a\": null\n}")));
    /* Canonical JSON (RFC 8785): sorted keys, ECMAScript numbers, NaN is
       SERE0020. */
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){
                                    .value = value,
                                    .parameters = {JSON_METHOD,
                                                   .canonical = SXML_SERIALIZATION_FLAG_YES}},
                                text_view("!SERE0020")));
    sxml_sequence_release(value);
    sxml_item numbers[4] = {sxml_atomic_item_from_double(1e23), sxml_atomic_item_from_double(1e-7),
                            sxml_atomic_item_from_double(333333333.3333332),
                            sxml_atomic_item_from_double(-0.000001)};
    sxml_item canonical_keys[3] = {make_string(current, "\xE2\x8D\xBA"), make_string(current, "b"),
                                   make_string(current, "\xF0\x9F\x98\x80")};
    sxml_item canonical_values[3] = {sxml_atomic_item_from_integer(1),
                                     make_array(current, numbers, 4),
                                     sxml_atomic_item_from_integer(3)};
    value = sxml_sequence_from_item(make_map(
        current, &(map_case){.keys = canonical_keys, .values = canonical_values, .entry_count = 3}));
    SXML_TEST_EXPECT(
        state,
        serialization_gives(current,
                            &(serialization_case){
                                .value = value,
                                .parameters = {JSON_METHOD,
                                               .canonical = SXML_SERIALIZATION_FLAG_YES}},
                            text_view("{\"b\":[1e+23,1e-7,333333333.3333332,-0.000001],"
                                      "\"\xE2\x8D\xBA\":1,\"\xF0\x9F\x98\x80\":3}")));
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_json_keys_and_nodes(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* SERE0022: two keys with the same string value, unless
       allow-duplicate-names (9.3.11). */
    sxml_item keys[2] = {make_qname(current, "", "foo"), make_string(current, "foo")};
    sxml_item values[2] = {sxml_atomic_item_from_integer(1), sxml_atomic_item_from_integer(2)};
    sxml_sequence value = sxml_sequence_from_item(
        make_map(current, &(map_case){.keys = keys, .values = values, .entry_count = 2}));
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){.value = value, .parameters = {JSON_METHOD}},
                                text_view("!SERE0022")));
    SXML_TEST_EXPECT(
        state,
        serialization_gives(
            current,
            &(serialization_case){
                .value = value,
                .parameters = {JSON_METHOD,
                               .allow_duplicate_names = SXML_SERIALIZATION_FLAG_YES}},
            text_view("{\"foo\":1,\"foo\":2}")));
    sxml_sequence_release(value);
    /* Nodes become strings by json-node-output-method (9.1, 9.3.12). */
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, parse_document(current, "<a>t<b/></a>", &document) == SXML_STATUS_OK);
    sxml_item item_of_document[1] = {document};
    value = sxml_sequence_from_item(make_array(current, item_of_document, 1));
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){.value = value, .parameters = {JSON_METHOD}},
                                text_view("[\"<a>t<b\\/><\\/a>\"]")));
    SXML_TEST_EXPECT(
        state,
        serialization_gives(current,
                            &(serialization_case){
                                .value = value,
                                .parameters = {JSON_METHOD, .json_node_output_method =
                                                                SXML_JSON_NODE_OUTPUT_METHOD_TEXT}},
                            text_view("[\"t\"]")));
    /* Characters the encoding lacks are \u escapes (9.1). */
    sxml_sequence clef = sxml_sequence_from_item(make_string(current, "\xF0\x9D\x84\x9E"));
    SXML_TEST_EXPECT(
        state,
        serialization_gives(current,
                            &(serialization_case){
                                .value = clef,
                                .parameters = {JSON_METHOD, .character_encoding =
                                                                SXML_CHARACTER_ENCODING_ISO_8859_1}},
                            text_view("\"\\uD834\\uDD1E\"")));
    sxml_sequence_release(clef);
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------- CSV */

static void test_csv(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* Section 10.2: rows of fields, quoted where needed, quotes doubled. */
    sxml_item first_row[2] = {make_string(current, "a;b"), make_string(current, "c\"d")};
    sxml_item second_row[2] = {sxml_atomic_item_from_integer(1), make_string(current, "x\r\ny")};
    sxml_item rows[2] = {make_array(current, first_row, 2), make_array(current, second_row, 2)};
    sxml_sequence value = make_sequence(current, rows, 2);
    SXML_TEST_EXPECT(state, serialization_gives(current,
                                                &(serialization_case){
                                                    .value = value,
                                                    .parameters = {CSV_METHOD,
                                                                   .csv_separator_codepoint = ';'}},
                                                text_view("\"a;b\";\"c\"\"d\"\n1;\"x\ny\"\n")));
    SXML_TEST_EXPECT(state,
                     serialization_gives(current,
                                         &(serialization_case){
                                             .value = value,
                                             .parameters = {CSV_METHOD,
                                                            .csv_separator_codepoint = '"'}},
                                         text_view("!SEPM0016")));
    /* A parsed CSV structure with csv-header. */
    sxml_item header[1] = {make_string(current, "h")};
    sxml_sequence columns = make_sequence(current, header, 1);
    sxml_map_entry entries[2] = {{.key = make_string(current, "columns"), .value = columns},
                                 {.key = make_string(current, "rows"), .value = value}};
    sxml_item structure = sxml_atomic_item_false();
    SXML_TEST_REQUIRE(state,
                      sxml_map_make(current->item_heap,
                                    &(sxml_map_request){
                                        .map_entries = {.map_entries = entries,
                                                        .map_entry_count = 2},
                                        .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT},
                                    &structure) == SXML_STATUS_OK);
    sxml_item_release(entries[0].key);
    sxml_item_release(entries[1].key);
    sxml_sequence_release(columns);
    sxml_sequence_release(value);
    value = sxml_sequence_from_item(structure);
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){
                                    .value = value,
                                    .parameters = {CSV_METHOD,
                                                   .csv_header = SXML_SERIALIZATION_FLAG_YES}},
                                text_view("h\na;b,\"c\"\"d\"\n1,\"x\ny\"\n")));
    sxml_sequence_release(value);
    /* Anything else is SERE0025. */
    sxml_sequence text = sxml_sequence_from_item(make_string(current, "a"));
    SXML_TEST_EXPECT(state, serialization_gives(
                                current,
                                &(serialization_case){.value = text, .parameters = {CSV_METHOD}},
                                text_view("!SERE0025")));
    sxml_sequence_release(text);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------- Parameter reader */

/* Reads options with a new reader; writes the error code, if any, to
   code (a buffer of 16 bytes). */
typedef struct read_case {
    sxml_serialization_parameter_reader *reader;
    sxml_sequence options;
    sxml_serialization_parameters *parameters;
    char *code;
} read_case;

static sxml_status read_options(const read_case *test_case) {
    sxml_error_record error_record = {.value = NULL};
    const sxml_status status = sxml_serialization_parameter_reader_read(
            test_case->reader,
            &(sxml_serialization_parameter_read_request){.options = test_case->options,
                                                         .error_record = &error_record},
            test_case->parameters);
    test_case->code[0] = '\0';
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        (void)snprintf(test_case->code, 16, "%.*s", (int)error_record.code.local_name.byte_count,
                       (const char *)error_record.code.local_name.bytes);
    }
    return status;
}

static sxml_serialization_parameter_reader *make_reader(fixture *test_fixture) {
    sxml_serialization_parameter_reader *reader = NULL;
    if (sxml_serialization_parameter_reader_create(
            fixture_allocator(test_fixture),
            &(sxml_serialization_parameter_reader_configuration){
                .allocator = fixture_allocator(test_fixture), .item_heap = test_fixture->item_heap},
            &reader) != SXML_STATUS_OK) {
        return NULL;
    }
    return reader;
}

static void test_parameter_maps(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    char code[16];
    sxml_serialization_parameters parameters;
    sxml_serialization_parameter_reader *reader = make_reader(current);
    SXML_TEST_REQUIRE(state, reader != NULL);
    /* The empty sequence: method xml and defaults (F&O 4.0, 17.2.3). */
    SXML_TEST_EXPECT(state, read_options(&(read_case){.reader = reader, .options = sxml_sequence_empty(),
                                                               .parameters = &parameters,
                                                               .code = code}) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, parameters.serialization_method == SXML_SERIALIZATION_METHOD_XML);
    sxml_item character_key[1] = {make_string(current, "$")};
    sxml_item character_value[1] = {make_string(current, "\xC2\xA3")};
    sxml_item keys[5] = {make_string(current, "method"), make_string(current, "indent"),
                         make_string(current, "use-character-maps"),
                         make_string(current, "indent-unit"),
                         make_string(current, "cdata-section-elements")};
    sxml_item values[5] = {
        make_string(current, "json"), sxml_atomic_item_true(),
        make_map(current,
                 &(map_case){.keys = character_key, .values = character_value, .entry_count = 1}),
        make_string(current, "\\t"), make_qname(current, "urn:x", "c")};
    sxml_sequence options = sxml_sequence_from_item(
        make_map(current, &(map_case){.keys = keys, .values = values, .entry_count = 5}));
    SXML_TEST_EXPECT(state, read_options(&(read_case){.reader = reader, .options = options,
                                                               .parameters = &parameters,
                                                               .code = code}) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, parameters.serialization_method == SXML_SERIALIZATION_METHOD_JSON);
    SXML_TEST_EXPECT(state, parameters.indent == SXML_SERIALIZATION_FLAG_YES);
    SXML_TEST_EXPECT(state, parameters.use_character_maps.character_mapping_count == 1);
    SXML_TEST_EXPECT(state, parameters.has_indent_unit &&
                                sxml_byte_view_equals(parameters.indent_unit, text_view("\t")));
    SXML_TEST_EXPECT(state, parameters.cdata_section_elements.element_name_count == 1);
    sxml_sequence_release(options);
    /* Errors: a wrong type (XPTY0004), an unknown name (XPTY0004 in 4.0),
       a bad value (SEPM0016), an unknown encoding (SESU0007). */
    const struct {
        const char *key;
        const char *value;
        const char *code;
    } errors[] = {
        {"indent", "yes", "XPTY0004"},
        {"xindent", "yes", "XPTY0004"},
        {"method", "jsonx", "SEPM0016"},
        {"line-ending", "\\n\\n", "SEPM0016"},
        {"csv-separator", "ab", "SEPM0016"},
        {"encoding", "EBCDIC", "SESU0007"},
    };
    for (size_t error_index = 0; error_index < sizeof(errors) / sizeof(errors[0]);
         error_index += 1) {
        sxml_item error_keys[1] = {make_string(current, errors[error_index].key)};
        sxml_item error_values[1] = {make_string(current, errors[error_index].value)};
        options = sxml_sequence_from_item(make_map(
            current, &(map_case){.keys = error_keys, .values = error_values, .entry_count = 1}));
        SXML_TEST_EXPECT(state, read_options(&(read_case){.reader = reader, .options = options,
                                                                   .parameters = &parameters,
                                                                   .code = code}) !=
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, strcmp(code, errors[error_index].code) == 0);
        sxml_sequence_release(options);
    }
    sxml_serialization_parameter_reader_destroy(reader);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_parameter_documents(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    char code[16];
    sxml_serialization_parameters parameters;
    sxml_serialization_parameter_reader *reader = make_reader(current);
    SXML_TEST_REQUIRE(state, reader != NULL);
    /* Serialization 4.0, 3.1; codes of the QT4 params-NNN documents. */
    const struct {
        const char *document;
        const char *code;
    } documents[] = {
        {"<o:serialization-parameters xmlns:o='http://www.w3.org/2010/xslt-xquery-serialization'"
         " xmlns='urn:d' xmlns:b='urn:b'><o:method value='html'/><o:indent value=' yes '/>"
         "<o:cdata-section-elements value='h b:f'/><o:use-character-maps>"
         "<o:character-map character='$' map-string='&#xA3;'/></o:use-character-maps>"
         "<x:any xmlns:x='urn:x' value='1'/></o:serialization-parameters>",
         ""},
        {"<o:serialization-parameters xmlns:o='http://www.w3.org/2010/xslt-xquery-serialization'>"
         "<o:indent value='maybe'/></o:serialization-parameters>",
         "SEPM0017"},
        {"<o:serialization-parameters xmlns:o='http://www.w3.org/2010/xslt-xquery-serialization'>"
         "<o:indent value='yes' value2='no'/></o:serialization-parameters>",
         "SEPM0017"},
        {"<o:serialization-parameters xmlns:o='http://www.w3.org/2010/xslt-xquery-serialization'>"
         "<o:outdent value='yes'/></o:serialization-parameters>",
         "SEPM0017"},
        {"<o:serialization-parameters xmlns:o='http://www.w3.org/2010/xslt-xquery-serialization'>"
         "<o:indent value='yes'/><o:indent value='no'/></o:serialization-parameters>",
         "SEPM0019"},
        {"<o:serialization-parameters xmlns:o='http://www.w3.org/2010/xslt-xquery-serialization'>"
         "<o:use-character-maps><o:character-map character='$' map-string='a'/>"
         "<o:character-map character='$' map-string='b'/></o:use-character-maps>"
         "</o:serialization-parameters>",
         "SEPM0018"},
        {"<o:serialization-parameters xmlns:o='http://www.w3.org/2010/xslt-xquery-serialization/x'/>",
         "XPTY0004"},
    };
    for (size_t document_index = 0; document_index < sizeof(documents) / sizeof(documents[0]);
         document_index += 1) {
        sxml_item document = {.representation = 0};
        SXML_TEST_REQUIRE(state, parse_document(current, documents[document_index].document,
                                                &document) == SXML_STATUS_OK);
        sxml_node_reference node = {.tree = NULL};
        SXML_TEST_REQUIRE(state, sxml_item_get_node(document, &node) == SXML_STATUS_OK);
        node.node_index.index = 1;
        sxml_item element = sxml_node_item_make(node);
        const sxml_status status =
            read_options(&(read_case){.reader = reader, .options = sxml_sequence_from_item(element),
                                               .parameters = &parameters,
                                               .code = code});
        SXML_TEST_EXPECT(state, strcmp(code, documents[document_index].code) == 0);
        if (documents[document_index].code[0] == '\0') {
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
            SXML_TEST_EXPECT(state,
                             parameters.serialization_method == SXML_SERIALIZATION_METHOD_HTML);
            SXML_TEST_EXPECT(state, parameters.indent == SXML_SERIALIZATION_FLAG_YES);
            SXML_TEST_EXPECT(state, parameters.cdata_section_elements.element_name_count == 2);
            SXML_TEST_EXPECT(state, parameters.use_character_maps.character_mapping_count == 1 &&
                                        parameters.use_character_maps.character_mappings[0]
                                                .codepoint == '$');
        }
        sxml_item_release(element);
        sxml_item_release(document);
    }
    sxml_serialization_parameter_reader_destroy(reader);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------- Allocation failures */

static void test_allocation_failures(sxml_test_state *state) {
    bool has_succeeded = false;
    for (uint64_t failing = 1; failing < 400 && !has_succeeded; failing += 1) {
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(failing, &test_fixture) == SXML_STATUS_OK);
        fixture *current = &test_fixture;
        sxml_item document = {.representation = 0};
        sxml_status status = parse_document(current, "<a>x</a>", &document);
        if (status == SXML_STATUS_OK) {
            sxml_item members[2] = {document, make_string(current, "s")};
            sxml_item keys[2] = {sxml_atomic_item_from_integer(1), make_string(current, "k")};
            sxml_item values[2] = {make_array(current, members, 2), sxml_atomic_item_true()};
            sxml_sequence value = sxml_sequence_from_item(
                make_map(current, &(map_case){.keys = keys, .values = values, .entry_count = 2}));
            sxml_error_record error_record = {.value = NULL};
            status = sxml_serialize_sequence(
                current->item_heap,
                &(sxml_serialization_request){
                    .value = value,
                    .serialization_parameters = {JSON_METHOD,
                                                 .canonical = SXML_SERIALIZATION_FLAG_YES},
                    .error_record = &error_record},
                sxml_output_buffer_as_output_stream(current->output_buffer));
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY ||
                                        status == SXML_STATUS_INVALID_ARGUMENT);
            has_succeeded =
                status == SXML_STATUS_OK &&
                sxml_byte_view_equals(sxml_output_buffer_get_bytes(current->output_buffer),
                                      text_view("{\"1\":[\"<a>x</a>\",\"s\"],\"k\":true}"));
            sxml_sequence_release(value);
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

static const sxml_test_case serialize_json_test_cases[] = {
    {.name = "json_values", .function = test_json_values},
    {.name = "json_keys_and_nodes", .function = test_json_keys_and_nodes},
    {.name = "csv", .function = test_csv},
    {.name = "parameter_maps", .function = test_parameter_maps},
    {.name = "parameter_documents", .function = test_parameter_documents},
    {.name = "allocation_failures", .function = test_allocation_failures},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "serialize_json",
            .cases = serialize_json_test_cases,
            .case_count = SXML_TEST_COUNT(serialize_json_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
