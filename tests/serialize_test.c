/*
 * Tests for serialization (src/serialize.c, include/sxml/serialize.h; D51,
 * D82): the output stream wrapper, the output buffer, and the adaptive
 * output method of Serialization 4.0, section 11, on every kind of item.
 *
 * Golden file: tests/memory_test.c (lifecycle, allocation-failure loops).
 * Values are made with the substrate (src/xdm*.c, the arena tree), written
 * into an output buffer and compared with the expected text; every case
 * checks that the heap's cells and the allocator's blocks are released.
 */
#include "sxml/serialize.h"

#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
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

/* Serializes value with the default parameters into the fixture's buffer. */
static sxml_status serialize(fixture *test_fixture, sxml_sequence value) {
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_error_record error_record = {.value = NULL};
    return sxml_serialize_sequence(
        test_fixture->item_heap,
        &(sxml_serialization_request){
            .value = value,
            .serialization_parameters = {.serialization_method =
                                             SXML_SERIALIZATION_METHOD_ADAPTIVE},
            .error_record = &error_record},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
}

/* True when the buffer holds expected_text; prints both otherwise. */
static bool buffer_holds(const fixture *test_fixture, const char *expected_text) {
    const sxml_byte_view bytes = sxml_output_buffer_get_bytes(test_fixture->output_buffer);
    const bool is_equal = sxml_byte_view_equals(bytes, text_view(expected_text));
    if (!is_equal) {
        (void)fprintf(stderr, "expected \"%s\", got \"%.*s\"\n", expected_text,
                      (int)bytes.byte_count, (const char *)bytes.bytes);
    }
    return is_equal;
}

/* Serializes value (owned: released here) and compares. */
static bool serializes_to(fixture *test_fixture, sxml_sequence value,
                          const char *expected_text) {
    const sxml_status status = serialize(test_fixture, value);
    sxml_sequence_release(value);
    return status == SXML_STATUS_OK && buffer_holds(test_fixture, expected_text);
}

/* -------------------------------------------------------- Making values */

static sxml_item make_atomic(fixture *test_fixture, sxml_builtin_type type,
                             sxml_atomic_datum datum) {
    sxml_item item = {.representation = 0};
    if (sxml_atomic_item_make(test_fixture->item_heap,
                              &(sxml_atomic_item_request){
                                  .type = sxml_builtin_type_identifier(type), .datum = datum},
                              &item) != SXML_STATUS_OK) {
        return sxml_atomic_item_false();
    }
    return item;
}

static sxml_item make_text_item(fixture *test_fixture, sxml_builtin_type type,
                                const char *text) {
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    if (sxml_string_from_utf8(text_view(text), &string) != SXML_STATUS_OK) {
        return sxml_atomic_item_false();
    }
    return make_atomic(test_fixture, type,
                       (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_STRING,
                                           .datum_details = {.string = string}});
}

static sxml_item make_string(fixture *test_fixture, const char *text) {
    return make_text_item(test_fixture, SXML_TYPE_XS_STRING, text);
}

static sxml_item make_qname(fixture *test_fixture, sxml_byte_view namespace_uri,
                            const char *local_name) {
    sxml_namespace_uri_identifier uri = {.index = 0};
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_namespace_uri(test_fixture->name_pool, namespace_uri, &uri) !=
            SXML_STATUS_OK ||
        sxml_name_pool_intern_name(
            test_fixture->name_pool,
            &(sxml_name_request){.namespace_uri = uri, .local_name = text_view(local_name)},
            &name) != SXML_STATUS_OK) {
        return sxml_atomic_item_false();
    }
    return make_atomic(test_fixture, SXML_TYPE_XS_QNAME,
                       (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_QNAME,
                                           .datum_details = {.qname = {.expanded_name = name}}});
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

/* The array of members (owned: taken over). */
static sxml_item make_array(fixture *test_fixture, const sxml_sequence *members,
                            size_t member_count) {
    sxml_item array = {.representation = 0};
    const sxml_status status = sxml_array_make(
        test_fixture->item_heap,
        (sxml_sequence_view){.sequences = members, .sequence_count = member_count}, &array);
    for (size_t member_index = 0; member_index < member_count; member_index += 1) {
        sxml_sequence_release(members[member_index]);
    }
    return status == SXML_STATUS_OK ? array : sxml_atomic_item_false();
}

/* The map of entries (keys and values owned: taken over). */
static sxml_item make_map(fixture *test_fixture, const sxml_map_entry *entries,
                          size_t entry_count) {
    sxml_item map = {.representation = 0};
    const sxml_status status = sxml_map_make(
        test_fixture->item_heap,
        &(sxml_map_request){.map_entries = {.map_entries = entries, .map_entry_count = entry_count},
                            .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                            .error_record = NULL},
        &map);
    for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
        sxml_item_release(entries[entry_index].key);
        sxml_sequence_release(entries[entry_index].value);
    }
    return status == SXML_STATUS_OK ? map : sxml_atomic_item_false();
}

/* --------------------------------------------------------- Output buffer */

static void test_output_buffer_lifecycle(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_output_buffer_configuration configuration = {
        .allocator = fixture_allocator(&test_fixture), .maximum_byte_count = 0};
    sxml_memory_requirements requirements;
    SXML_TEST_EXPECT(state, sxml_output_buffer_memory_requirements(
                                &configuration, &requirements) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_output_buffer_memory_requirements(
                         &(sxml_output_buffer_configuration){
                             .allocator = {.state = NULL, .operations = NULL}},
                         &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    alignas(16) uint8_t storage[256];
    sxml_output_buffer *buffer = NULL;
    SXML_TEST_EXPECT(state, sxml_output_buffer_initialize(
                                (sxml_memory_view){.bytes = storage, .byte_count = 1},
                                &configuration, &buffer) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_REQUIRE(state,
                      sxml_output_buffer_initialize(
                          (sxml_memory_view){.bytes = storage, .byte_count = sizeof(storage)},
                          &configuration, &buffer) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_output_buffer_get_bytes(buffer).byte_count == 0);
    const sxml_output_stream stream = sxml_output_buffer_as_output_stream(buffer);
    SXML_TEST_EXPECT(state, sxml_output_stream_write(stream, text_view("abc")) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_output_stream_write(stream, text_view("")) == SXML_STATUS_OK);
    /* Growing past the first block keeps what was written. */
    char long_text[700];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    SXML_TEST_EXPECT(state, sxml_output_stream_write(stream, text_view(long_text)) ==
                                SXML_STATUS_OK);
    const sxml_byte_view bytes = sxml_output_buffer_get_bytes(buffer);
    SXML_TEST_EXPECT(state, bytes.byte_count == 3 + sizeof(long_text) - 1 &&
                                memcmp(bytes.bytes, "abcxx", 5) == 0);
    sxml_output_buffer_clear(buffer);
    SXML_TEST_EXPECT(state, sxml_output_buffer_get_bytes(buffer).byte_count == 0);
    SXML_TEST_EXPECT(state, sxml_output_stream_write(stream, text_view("z")) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_output_buffer_get_bytes(buffer),
                                                  text_view("z")));
    sxml_output_buffer_finalize(buffer);
    sxml_output_buffer_finalize(NULL);
    sxml_output_buffer_destroy(NULL);
    SXML_TEST_EXPECT(state, sxml_output_stream_write(
                                (sxml_output_stream){.state = NULL, .operations = NULL},
                                text_view("a")) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_output_buffer_limits_and_failures(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_output_buffer *buffer = NULL;
    SXML_TEST_REQUIRE(state, sxml_output_buffer_create(
                                 fixture_allocator(&test_fixture),
                                 &(sxml_output_buffer_configuration){
                                     .allocator = fixture_allocator(&test_fixture),
                                     .maximum_byte_count = 4},
                                 &buffer) == SXML_STATUS_OK);
    const sxml_output_stream stream = sxml_output_buffer_as_output_stream(buffer);
    SXML_TEST_EXPECT(state, sxml_output_stream_write(stream, text_view("abc")) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_output_stream_write(stream, text_view("de")) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_output_buffer_get_bytes(buffer),
                                                  text_view("abc")));
    sxml_output_buffer_destroy(buffer);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    /* Every allocation failure: create fails cleanly, a write leaves the
       buffer as it was. */
    bool has_succeeded = false;
    for (uint64_t failing_number = 1; !has_succeeded && failing_number < 10;
         failing_number += 1) {
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        sxml_counting_allocator *failing = NULL;
        alignas(16) uint8_t failing_storage[256];
        SXML_TEST_REQUIRE(state, sxml_counting_allocator_initialize(
                                     (sxml_memory_view){.bytes = failing_storage,
                                                        .byte_count = sizeof(failing_storage)},
                                     &(sxml_counting_allocator_configuration){
                                         .backing_allocator = sxml_default_allocator(),
                                         .failing_allocation_number = failing_number},
                                     &failing) == SXML_STATUS_OK);
        const sxml_allocator allocator = sxml_counting_allocator_as_allocator(failing);
        sxml_status status = sxml_output_buffer_create(
            allocator,
            &(sxml_output_buffer_configuration){.allocator = allocator, .maximum_byte_count = 0},
            &buffer);
        if (status == SXML_STATUS_OK) {
            status = sxml_output_stream_write(sxml_output_buffer_as_output_stream(buffer),
                                              text_view("hello"));
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK ||
                                        sxml_output_buffer_get_bytes(buffer).byte_count == 0);
            sxml_output_buffer_destroy(buffer);
        }
        has_succeeded = status == SXML_STATUS_OK;
        SXML_TEST_EXPECT(state, has_succeeded || status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state,
                         sxml_counting_allocator_read_counts(failing).live_allocation_count == 0);
        sxml_counting_allocator_finalize(failing);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
}

/* --------------------------------------------------------- Atomic items */

static void test_atomic_items(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(sxml_atomic_item_true()),
                                          "true()"));
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(sxml_atomic_item_false()),
                                          "false()"));
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(make_string(current, "a\"b'c")),
                                          "\"a\"\"b'c\""));
    SXML_TEST_EXPECT(
        state, serializes_to(current,
                             sxml_sequence_from_item(
                                 make_text_item(current, SXML_TYPE_XS_UNTYPED_ATOMIC, "u")),
                             "\"u\""));
    SXML_TEST_EXPECT(
        state, serializes_to(current,
                             sxml_sequence_from_item(
                                 make_text_item(current, SXML_TYPE_XS_ANY_URI, "http://e/")),
                             "\"http://e/\""));
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(
                                                 sxml_atomic_item_from_integer(-42)),
                                          "-42"));
    SXML_TEST_EXPECT(
        state,
        serializes_to(current,
                      sxml_sequence_from_item(make_atomic(
                          current, SXML_TYPE_XS_DECIMAL,
                          (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_DECIMAL,
                                              .datum_details = {.decimal_text =
                                                                    text_view("1.50")}})),
                      "1.5"));
    /* Doubles keep an exponent so their type shows. */
    const struct {
        double value;
        const char *expected_text;
    } doubles[] = {
        {1.0, "1.0e0"},       {0.123, "1.23e-1"},   {123456789.0, "1.23456789e8"},
        {1e300, "1.0e300"},   {-2.5e-7, "-2.5e-7"}, {0.0, "0.0e0"},
        {-0.0, "-0.0e0"},     {1e6, "1.0e6"},       {100.0, "1.0e2"},
    };
    for (size_t double_index = 0; double_index < SXML_TEST_COUNT(doubles); double_index += 1) {
        SXML_TEST_EXPECT(state,
                         serializes_to(current,
                                       sxml_sequence_from_item(sxml_atomic_item_from_double(
                                           doubles[double_index].value)),
                                       doubles[double_index].expected_text));
    }
    SXML_TEST_EXPECT(state, serializes_to(current,
                                          sxml_sequence_from_item(
                                              sxml_atomic_item_from_double((double)INFINITY)),
                                          "xs:double(\"INF\")"));
    SXML_TEST_EXPECT(
        state,
        serializes_to(current,
                      sxml_sequence_from_item(make_atomic(
                          current, SXML_TYPE_XS_FLOAT,
                          (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_FLOAT,
                                              .datum_details = {.float_number = 1.5F}})),
                      "xs:float(\"1.5\")"));
    SXML_TEST_EXPECT(
        state,
        serializes_to(current,
                      sxml_sequence_from_item(make_atomic(
                          current, SXML_TYPE_XS_DATE,
                          (sxml_atomic_datum){
                              .datum_kind = SXML_DATUM_KIND_DATE_TIME,
                              .datum_details = {.date_time = {.year = 2015, .month = 7,
                                                              .day = 17}}})),
                      "xs:date(\"2015-07-17\")"));
    const uint8_t octets[] = {0x0A, 0xFF};
    SXML_TEST_EXPECT(
        state,
        serializes_to(current,
                      sxml_sequence_from_item(make_atomic(
                          current, SXML_TYPE_XS_HEX_BINARY,
                          (sxml_atomic_datum){
                              .datum_kind = SXML_DATUM_KIND_BINARY,
                              .datum_details = {.binary = {.bytes = octets, .byte_count = 2}}})),
                      "xs:hexBinary(\"0AFF\")"));
    SXML_TEST_EXPECT(state,
                     serializes_to(current,
                                   sxml_sequence_from_item(make_qname(
                                       current, text_view("http://www.w3.org/2005/xpath-functions"),
                                       "true")),
                                   "#fn:true"));
    SXML_TEST_EXPECT(state, serializes_to(current,
                                          sxml_sequence_from_item(
                                              make_qname(current, text_view(""), "local")),
                                          "#local"));
    SXML_TEST_EXPECT(state, serializes_to(current,
                                          sxml_sequence_from_item(
                                              make_qname(current, text_view("http://e.com/"), "x")),
                                          "#Q{http://e.com/}x"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------- Sequences, arrays and maps */

static void test_sequences_arrays_and_maps(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_empty(), ""));
    const sxml_item three[] = {sxml_atomic_item_from_integer(1), make_string(current, "x"),
                               sxml_atomic_item_true()};
    SXML_TEST_EXPECT(state, serializes_to(current, make_sequence(current, three, 3), "1\n\"x\"\ntrue()"));
    /* A chosen item separator. */
    sxml_sequence pair = make_sequence(
        current, (const sxml_item[]){sxml_atomic_item_from_integer(1), sxml_atomic_item_from_integer(2)},
        2);
    sxml_output_buffer_clear(current->output_buffer);
    SXML_TEST_EXPECT(state,
                     sxml_serialize_sequence(
                         current->item_heap,
                         &(sxml_serialization_request){
                             .value = pair,
                             .serialization_parameters = {.has_item_separator = true,
                                                          .item_separator = text_view(", ")}},
                         sxml_output_buffer_as_output_stream(current->output_buffer)) == SXML_STATUS_OK &&
                         buffer_holds(current, "1, 2"));
    const uint8_t bad_utf8[] = {0xC3};
    SXML_TEST_EXPECT(state,
                     sxml_serialize_sequence(
                         current->item_heap,
                         &(sxml_serialization_request){
                             .value = pair,
                             .serialization_parameters = {
                                 .has_item_separator = true,
                                 .item_separator = {.bytes = bad_utf8, .byte_count = 1}}},
                         sxml_output_buffer_as_output_stream(current->output_buffer)) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    sxml_sequence_release(pair);
    /* [1, (), (2, 3), "x", []] */
    const sxml_sequence members[] = {
        sxml_sequence_from_item(sxml_atomic_item_from_integer(1)),
        sxml_sequence_empty(),
        make_sequence(current,
                      (const sxml_item[]){sxml_atomic_item_from_integer(2),
                                          sxml_atomic_item_from_integer(3)},
                      2),
        sxml_sequence_from_item(make_string(current, "x")),
        sxml_sequence_from_item(make_array(current, NULL, 0)),
    };
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(make_array(current, members, 5)),
                                          "[1,(),(2,3),\"x\",[]]"));
    /* {"b": [true()], "a": 1, 2: {}} keeps entry order. */
    const sxml_map_entry entries[] = {
        {.key = make_string(current, "b"),
         .value = sxml_sequence_from_item(make_array(
             current, (const sxml_sequence[]){sxml_sequence_from_item(sxml_atomic_item_true())}, 1))},
        {.key = make_string(current, "a"), .value = sxml_sequence_from_item(sxml_atomic_item_from_integer(1))},
        {.key = sxml_atomic_item_from_integer(2),
         .value = sxml_sequence_from_item(make_map(current, NULL, 0))},
    };
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(make_map(current, entries, 3)),
                                          "{\"b\":[true()],\"a\":1,2:{}}"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------------- JNodes */

static void test_json_nodes(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    /* jtree({"a": [4, 5]}) and its child (Serialization 4.0, section 11). */
    const sxml_map_entry entry = {
        .key = make_string(current, "a"),
        .value = sxml_sequence_from_item(make_array(
            current,
            (const sxml_sequence[]){sxml_sequence_from_item(sxml_atomic_item_from_integer(4)),
                                    sxml_sequence_from_item(sxml_atomic_item_from_integer(5))},
            2))};
    const sxml_item map = make_map(current, &entry, 1);
    sxml_item root = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_json_node_make_root(current->item_heap, sxml_sequence_from_item(map),
                                                      &root) == SXML_STATUS_OK);
    sxml_item_release(map);
    sxml_item child = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_json_node_make_child(
                                 current->item_heap,
                                 (sxml_json_child_request){.json_node = root, .child_index = 0},
                                 &child) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(child), "jnode(\"a\":[4,5])"));
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(root),
                                          "jtree({\"a\":[4,5]})"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ---------------------------------------------------------------- Nodes */

/* Builds <p:a xmlns:p="urn:p" xmlns="urn:d" x="1&quot;">t&lt;<b xmlns="">
   <!--c--><?pi d?></b><p:c/></p:a> as a document. */
static sxml_status build_document(fixture *current, sxml_item *document) {
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        sxml_default_allocator(),
        &(sxml_arena_tree_configuration){.allocator = sxml_default_allocator(),
                                         .item_heap = current->item_heap,
                                         .duplicate_attribute_policy =
                                             SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                                         .is_destroyed_when_unreferenced = true},
        &arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_tree_builder builder = sxml_arena_tree_as_tree_builder(arena_tree);
    sxml_namespace_uri_identifier p_uri = {0};
    sxml_namespace_uri_identifier d_uri = {0};
    sxml_namespace_prefix_identifier p_prefix = {0};
    sxml_name_identifier a_name = {0};
    sxml_name_identifier b_name = {0};
    sxml_name_identifier c_name = {0};
    sxml_name_identifier x_name = {0};
    sxml_name_identifier pi_name = {0};
    status = sxml_name_pool_intern_namespace_uri(current->name_pool, text_view("urn:p"), &p_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(current->name_pool, text_view("urn:d"), &d_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(current->name_pool, text_view("p"), &p_prefix);
    }
    const struct {
        sxml_namespace_uri_identifier uri;
        const char *local_name;
        sxml_name_identifier *name;
    } names[] = {{p_uri, "a", &a_name},
                 {{0}, "b", &b_name},
                 {p_uri, "c", &c_name},
                 {{0}, "x", &x_name},
                 {{0}, "pi", &pi_name}};
    for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < 5; name_index += 1) {
        status = sxml_name_pool_intern_name(
            current->name_pool,
            &(sxml_name_request){.namespace_uri = names[name_index].uri,
                                 .local_name = text_view(names[name_index].local_name)},
            names[name_index].name);
    }
    const sxml_tree_builder_operations *operations = builder.operations;
    if (status == SXML_STATUS_OK) {
        status = operations->start_document(builder.state, &(sxml_document_start_event){0});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->start_element(
            builder.state,
            &(sxml_element_start_event){.element_name = a_name, .namespace_prefix = p_prefix});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->add_namespace_binding(
            builder.state,
            &(sxml_namespace_binding){.namespace_prefix = p_prefix, .namespace_uri = p_uri});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->add_namespace_binding(
            builder.state, &(sxml_namespace_binding){.namespace_prefix = {0}, .namespace_uri = d_uri});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->add_attribute(
            builder.state,
            &(sxml_attribute_event){.attribute_name = x_name, .attribute_value = text_view("1\"<\t")});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->add_text(builder.state, &(sxml_text_event){.text = text_view("t<&>")});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->start_element(builder.state,
                                           &(sxml_element_start_event){.element_name = b_name});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->add_namespace_binding(
            builder.state, &(sxml_namespace_binding){.namespace_prefix = {0}, .namespace_uri = {0}});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->add_comment(builder.state,
                                         &(sxml_comment_event){.comment = text_view("c")});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->add_processing_instruction(
            builder.state,
            &(sxml_processing_instruction_event){.target = text_view("pi"), .data = text_view("d")});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->end_element(builder.state);
    }
    if (status == SXML_STATUS_OK) {
        status = operations->start_element(
            builder.state,
            &(sxml_element_start_event){.element_name = c_name, .namespace_prefix = p_prefix});
    }
    if (status == SXML_STATUS_OK) {
        status = operations->end_element(builder.state);
    }
    if (status == SXML_STATUS_OK) {
        status = operations->end_element(builder.state);
    }
    if (status == SXML_STATUS_OK) {
        status = operations->end_document(builder.state);
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

/* The node at node_index of document's tree, owned. */
static sxml_item node_at(sxml_item document, uint32_t node_index, uint16_t namespace_number) {
    sxml_node_reference node;
    if (sxml_item_get_node(document, &node) != SXML_STATUS_OK) {
        return sxml_atomic_item_false();
    }
    node.node_index.index = node_index;
    node.namespace_number = namespace_number;
    return sxml_node_item_make(node);
}

static void test_nodes(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, build_document(current, &document) == SXML_STATUS_OK);
    /* Node indices: 0 document, 1 p:a, 2 @x, 3 text, 4 b, 5 comment, 6 PI,
       7 p:c. */
    static const char document_text[] =
        "<p:a xmlns:p=\"urn:p\" xmlns=\"urn:d\" x=\"1&quot;&lt;&#x9;\">t&lt;&amp;&gt;"
        "<b xmlns=\"\"><!--c--><?pi d?></b><p:c/></p:a>";
    sxml_item_retain(document);
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(document), document_text));
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(node_at(document, 1, 0)),
                                          document_text));
    /* An element below the top declares its in-scope namespaces, inherited
       ones included (they are its namespace nodes). */
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(node_at(document, 7, 0)),
                                          "<p:c xmlns:p=\"urn:p\" xmlns=\"urn:d\"/>"));
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(node_at(document, 4, 0)),
                                          "<b xmlns:p=\"urn:p\"><!--c--><?pi d?></b>"));
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(node_at(document, 2, 0)),
                                          "x=\"1&quot;&lt;&#x9;\""));
    SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(node_at(document, 3, 0)),
                                          "t&lt;&amp;&gt;"));
    /* Namespace nodes of p:a: some order of xml, p and the default. */
    sxml_namespace_binding_view bindings;
    sxml_node_reference element;
    SXML_TEST_REQUIRE(state, sxml_item_get_node(document, &element) == SXML_STATUS_OK);
    element.node_index.index = 1;
    SXML_TEST_REQUIRE(state, sxml_node_get_in_scope_namespaces(element, &bindings) ==
                                 SXML_STATUS_OK);
    bool has_p = false;
    for (size_t number = 1; number <= bindings.namespace_binding_count; number += 1) {
        const sxml_item namespace_node = node_at(document, 1, (uint16_t)number);
        if (serialize(current, sxml_sequence_from_item(namespace_node)) == SXML_STATUS_OK &&
            sxml_byte_view_equals(sxml_output_buffer_get_bytes(current->output_buffer),
                                  text_view("xmlns:p=\"urn:p\""))) {
            has_p = true;
        }
        sxml_item_release(namespace_node);
    }
    SXML_TEST_EXPECT(state, has_p);
    sxml_item_release(document);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A parentless namespace node is stored as the root of its own tree (D98):
   its name is the prefix, its content the URI. */
static void test_stored_namespace_nodes(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    const char *const prefixes[] = {"p", ""};
    const char *const expected_texts[] = {"xmlns:p=\"urn:a&amp;b\"", "xmlns=\"urn:a&amp;b\""};
    for (size_t case_index = 0; case_index < 2; case_index += 1) {
        sxml_arena_tree *arena_tree = NULL;
        SXML_TEST_REQUIRE(state, sxml_arena_tree_create(
                                     sxml_default_allocator(),
                                     &(sxml_arena_tree_configuration){
                                         .allocator = sxml_default_allocator(),
                                         .item_heap = current->item_heap,
                                         .duplicate_attribute_policy =
                                             SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                                         .is_destroyed_when_unreferenced = true},
                                     &arena_tree) == SXML_STATUS_OK);
        sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                          .namespace_uri = {.index = 0}};
        sxml_status status = SXML_STATUS_OK;
        if (prefixes[case_index][0] != '\0') {
            status = sxml_name_pool_intern_namespace_prefix(
                current->name_pool, text_view(prefixes[case_index]), &binding.namespace_prefix);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_uri(current->name_pool, text_view("urn:a&b"),
                                                         &binding.namespace_uri);
        }
        const sxml_tree_builder builder = sxml_arena_tree_as_tree_builder(arena_tree);
        if (status == SXML_STATUS_OK) {
            status = builder.operations->add_namespace_binding(builder.state, &binding);
        }
        sxml_tree *tree = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_arena_tree_finish(arena_tree, &tree);
        }
        if (status != SXML_STATUS_OK) {
            sxml_arena_tree_destroy(arena_tree);
        }
        SXML_TEST_REQUIRE(state, status == SXML_STATUS_OK);
        const sxml_item namespace_node = sxml_node_item_make(
            (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
        sxml_tree_release(tree);
        SXML_TEST_EXPECT(state, serializes_to(current, sxml_sequence_from_item(namespace_node),
                                              expected_texts[case_index]));
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------- Depth and failures */

/* 100,000 nested arrays: the serializer's stack is heap memory (D71). */
static void test_deep_nesting(sxml_test_state *state) {
    enum { SXML_TEST_DEPTH = 100000 };
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    fixture *current = &test_fixture;
    sxml_item array = make_array(current, NULL, 0);
    for (uint32_t level = 1; level < SXML_TEST_DEPTH; level += 1) {
        const sxml_sequence member = sxml_sequence_from_item(array);
        array = make_array(current, &member, 1);
    }
    SXML_TEST_EXPECT(state, serialize(current, sxml_sequence_from_item(array)) == SXML_STATUS_OK);
    const sxml_byte_view bytes = sxml_output_buffer_get_bytes(current->output_buffer);
    SXML_TEST_EXPECT(state, bytes.byte_count == (size_t)2 * SXML_TEST_DEPTH && bytes.bytes[0] == '[' &&
                                bytes.bytes[bytes.byte_count - 1] == ']');
    sxml_item_release(array);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static sxml_status refuse_write(void *state, sxml_byte_view bytes) {
    (void)state;
    (void)bytes;
    return SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_output_stream_operations refusing_operations = {.write = refuse_write};

/* A nested value under every allocation failure, and a refusing stream. */
static void test_failures(sxml_test_state *state) {
    fixture test_fixture;
    bool has_succeeded = false;
    for (uint64_t failing_number = 1; !has_succeeded && failing_number < 200;
         failing_number += 1) {
        SXML_TEST_REQUIRE(state, fixture_create(failing_number, &test_fixture) == SXML_STATUS_OK);
        fixture *current = &test_fixture;
        /* [[[1]], {"k": [2]}] */
        sxml_item inner = make_array(
            current, (const sxml_sequence[]){sxml_sequence_from_item(sxml_atomic_item_from_integer(1))},
            1);
        sxml_item middle = make_array(current, (const sxml_sequence[]){sxml_sequence_from_item(inner)}, 1);
        const sxml_map_entry entry = {
            .key = make_string(current, "k"),
            .value = sxml_sequence_from_item(make_array(
                current,
                (const sxml_sequence[]){sxml_sequence_from_item(sxml_atomic_item_from_integer(2))},
                1))};
        const sxml_item map = make_map(current, &entry, 1);
        const sxml_item outer = make_array(
            current, (const sxml_sequence[]){sxml_sequence_from_item(middle), sxml_sequence_from_item(map)},
            2);
        const sxml_status status = serialize(current, sxml_sequence_from_item(outer));
        /* Quietly: earlier runs made false() where a make failed. */
        has_succeeded = status == SXML_STATUS_OK &&
                         sxml_byte_view_equals(sxml_output_buffer_get_bytes(current->output_buffer),
                                               text_view("[[[1]],{\"k\":[2]}]"));
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY ||
                                    status == SXML_STATUS_INVALID_ARGUMENT);
        sxml_item_release(outer);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
    SXML_TEST_EXPECT(state, has_succeeded);
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_serialize_sequence(
                         test_fixture.item_heap,
                         &(sxml_serialization_request){
                             .value = sxml_sequence_from_item(sxml_atomic_item_true())},
                         (sxml_output_stream){.state = NULL, .operations = &refusing_operations}) ==
                         SXML_STATUS_INPUT_OUTPUT_FAILED);
    SXML_TEST_EXPECT(state,
                     sxml_serialize_sequence(
                         test_fixture.item_heap,
                         &(sxml_serialization_request){
                             .value = sxml_sequence_from_item(sxml_atomic_item_true())},
                         (sxml_output_stream){.state = NULL, .operations = NULL}) ==
                         SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static const sxml_test_case serialize_test_cases[] = {
    {.name = "output_buffer_lifecycle", .function = test_output_buffer_lifecycle},
    {.name = "output_buffer_limits_and_failures",
     .function = test_output_buffer_limits_and_failures},
    {.name = "atomic_items", .function = test_atomic_items},
    {.name = "sequences_arrays_and_maps", .function = test_sequences_arrays_and_maps},
    {.name = "json_nodes", .function = test_json_nodes},
    {.name = "nodes", .function = test_nodes},
    {.name = "stored_namespace_nodes", .function = test_stored_namespace_nodes},
    {.name = "deep_nesting", .function = test_deep_nesting},
    {.name = "failures", .function = test_failures},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "serialize",
            .cases = serialize_test_cases,
            .case_count = SXML_TEST_COUNT(serialize_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
