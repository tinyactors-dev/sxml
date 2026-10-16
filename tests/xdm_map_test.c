/*
 * Tests for maps and records (include/sxml/xdm.h, src/xdm_map.c; D37, D61):
 * entry order, duplicate key policies, lookup by fn:atomic-equal, put and
 * remove on entry blocks, persistent maps and records, and failure paths.
 *
 * Imitates tests/memory_test.c and tests/xdm_atomic_test.c. Cases from the
 * QT4 test set are named after their test: map-merge-401 and -402 (entry
 * order), map-get-014 and -015 (NaN keys), map-put-011 (keys that compare
 * equal), map-put-024 and -025 (put keeps positions; new keys go last).
 */
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"

#include <math.h>
#include <string.h>

typedef struct map_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} map_fixture;

static sxml_status map_fixture_create_with(sxml_allocator allocator,
                                           map_fixture *fixture) {
    *fixture =
        (map_fixture){.name_pool = NULL, .registry = NULL, .item_heap = NULL};
    sxml_status status = sxml_name_pool_create(
        sxml_default_allocator(),
        &(sxml_name_pool_configuration){.allocator = sxml_default_allocator(),
                                        .maximum_byte_count = 0},
        &fixture->name_pool);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_type_registry_create(sxml_default_allocator(),
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

static sxml_status map_fixture_create(map_fixture *fixture) {
    return map_fixture_create_with(sxml_default_allocator(), fixture);
}

static void map_fixture_destroy(map_fixture *fixture) {
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

static uint64_t live_cell_count(const map_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
}

static uint64_t live_byte_count(const map_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_byte_count;
}

/* -------------------------------------------------------------- Helpers */

static sxml_item integer_key(int64_t integer) {
    return sxml_atomic_item_from_integer(integer);
}

/* A text to make an item of some string type from. */
typedef struct text_item_request {
    sxml_builtin_type type;
    sxml_byte_view text;
} text_item_request;

static sxml_status make_text_item(const map_fixture *fixture,
                                  text_item_request request, sxml_item *item) {
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                          .codepoint_count = 0,
                          .is_ascii = false};
    sxml_status status = sxml_string_from_utf8(request.text, &string);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_atomic_item_make(
        fixture->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(request.type),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = string}}},
        item);
}

static sxml_status make_string(const map_fixture *fixture, sxml_byte_view text,
                               sxml_item *item) {
    return make_text_item(
        fixture, (text_item_request){.type = SXML_TYPE_XS_STRING, .text = text},
        item);
}

static sxml_sequence value_of(sxml_item item) {
    return sxml_sequence_from_item(item);
}

/* True when item is an atomic item with the string value text. */
static bool has_text(sxml_item item, sxml_byte_view text) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return false;
    }
    sxml_byte_view utf8 = datum.datum_details.string.utf8;
    return utf8.byte_count == text.byte_count &&
           (text.byte_count == 0 ||
            memcmp(utf8.bytes, text.bytes, text.byte_count) == 0);
}

/* True when item is an inline integer equal to integer. */
static bool has_integer(sxml_item item, int64_t integer) {
    sxml_atomic_datum datum;
    return sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_INTEGER &&
           datum.datum_details.integer == integer;
}

/* The single item of a sequence; a non-item when it has none. */
static sxml_item single_item(sxml_sequence sequence) {
    sxml_item item = {.representation = 0};
    if (sxml_sequence_get_item(sequence, (sxml_item_request){.item_index = 0},
                               &item) != SXML_STATUS_OK) {
        return (sxml_item){.representation = 0};
    }
    return item;
}

static sxml_status make_map(const map_fixture *fixture,
                            const sxml_map_request *request, sxml_item *map) {
    return sxml_map_make(fixture->item_heap, request, map);
}

static sxml_map_request entries_request(const sxml_map_entry *entries,
                                        size_t entry_count) {
    return (sxml_map_request){
        .map_entries = {.map_entries = entries, .map_entry_count = entry_count},
        .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
        .error_record = NULL};
}

static size_t size_of(sxml_item map) {
    size_t entry_count = 0;
    if (sxml_map_get_size(map, &entry_count) != SXML_STATUS_OK) {
        return SIZE_MAX;
    }
    return entry_count;
}

/* The key at entry_index; a non-item when there is none. */
static sxml_item key_at(sxml_item map, size_t entry_index) {
    sxml_map_entry entry;
    if (sxml_map_get_entry(map, entry_index, &entry) != SXML_STATUS_OK) {
        return (sxml_item){.representation = 0};
    }
    return entry.key;
}

static sxml_map_lookup find_key(sxml_item map, sxml_item key) {
    sxml_map_lookup lookup = {.is_found = false,
                              .entry_index = 0,
                              .entry = {.key = {.representation = 0},
                                        .value = {{.representation = 0}}}};
    if (sxml_map_find(map, key, &lookup) != SXML_STATUS_OK) {
        lookup.is_found = false;
    }
    return lookup;
}

/* One key and value to put. */
typedef struct put_step {
    sxml_item key;
    sxml_sequence value;
} put_step;

/* Puts step into *map, replacing *map (released) on success. */
static sxml_status put_into(const map_fixture *fixture, put_step step,
                            sxml_item *map) {
    sxml_item new_map = {.representation = 0};
    sxml_status status = sxml_map_put(
        fixture->item_heap,
        &(sxml_map_update){.map = *map, .key = step.key, .value = step.value},
        &new_map);
    if (status == SXML_STATUS_OK) {
        sxml_item_release(*map);
        *map = new_map;
    }
    return status;
}

static sxml_status remove_from(const map_fixture *fixture, sxml_item key,
                               sxml_item *map) {
    sxml_item new_map = {.representation = 0};
    sxml_status status = sxml_map_remove(
        fixture->item_heap,
        &(sxml_map_update){
            .map = *map, .key = key, .value = {{.representation = 0}}},
        &new_map);
    if (status == SXML_STATUS_OK) {
        sxml_item_release(*map);
        *map = new_map;
    }
    return status;
}

static sxml_status make_empty_map(const map_fixture *fixture, sxml_item *map) {
    return make_map(
        fixture,
        &(sxml_map_request){
            .map_entries = {.map_entries = NULL, .map_entry_count = 0},
            .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
            .error_record = NULL},
        map);
}

/* ---------------------------------------------------------------- Cases */

static void test_maps_keep_entry_order(sxml_test_state *state) {
    map_fixture fixture;
    SXML_TEST_REQUIRE(state, map_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item key_a = {.representation = 0};
    sxml_item key_e = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      make_string(&fixture, SXML_BYTE_VIEW_FROM_LITERAL("a"),
                                  &key_a) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      make_string(&fixture, SXML_BYTE_VIEW_FROM_LITERAL("e"),
                                  &key_e) == SXML_STATUS_OK);
    /* map-merge-401: 17, 29, 'a', -234, 86, 445, 101, 'e'. */
    const sxml_map_entry entries[] = {
        {integer_key(17), value_of(integer_key(0))},
        {integer_key(29), value_of(integer_key(0))},
        {key_a, value_of(integer_key(0))},
        {integer_key(-234), value_of(integer_key(0))},
        {integer_key(86), value_of(integer_key(0))},
        {integer_key(445), value_of(integer_key(0))},
        {integer_key(101), value_of(integer_key(0))},
        {key_e, value_of(integer_key(0))},
    };
    sxml_item map = {.representation = 0};
    SXML_TEST_REQUIRE(
        state,
        make_map(
            &fixture,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries,
                                .map_entry_count = SXML_TEST_COUNT(entries)},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                .error_record = NULL},
            &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_item_get_kind(map) == SXML_ITEM_KIND_MAP);
    SXML_TEST_EXPECT(state, sxml_item_is_function_item(map));
    SXML_TEST_EXPECT(state,
                     sxml_item_get_type_annotation(map).index == SXML_TYPE_MAP);
    SXML_TEST_EXPECT(state, size_of(map) == SXML_TEST_COUNT(entries));
    for (size_t entry_index = 0; entry_index < SXML_TEST_COUNT(entries);
         entry_index += 1) {
        SXML_TEST_EXPECT(
            state, sxml_atomic_item_is_atomic_equal(key_at(map, entry_index),
                                                    entries[entry_index].key));
        sxml_map_lookup lookup = find_key(map, entries[entry_index].key);
        SXML_TEST_EXPECT(state, lookup.is_found);
        SXML_TEST_EXPECT(state, lookup.entry_index == entry_index);
    }
    /* map-merge-402: the same keys put one by one into an empty map. */
    sxml_item put_map = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      make_empty_map(&fixture, &put_map) == SXML_STATUS_OK);
    for (size_t entry_index = 0; entry_index < SXML_TEST_COUNT(entries);
         entry_index += 1) {
        SXML_TEST_REQUIRE(
            state, put_into(&fixture,
                            (put_step){.key = entries[entry_index].key,
                                       .value = entries[entry_index].value},
                            &put_map) == SXML_STATUS_OK);
    }
    for (size_t entry_index = 0; entry_index < SXML_TEST_COUNT(entries);
         entry_index += 1) {
        SXML_TEST_EXPECT(
            state, sxml_atomic_item_is_atomic_equal(
                       key_at(put_map, entry_index), entries[entry_index].key));
    }
    sxml_map_entry entry;
    SXML_TEST_EXPECT(
        state, sxml_map_get_entry(map, SXML_TEST_COUNT(entries), &entry) ==
                   SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_map_get_entry(key_a, 0, &entry) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    size_t entry_count = 0;
    SXML_TEST_EXPECT(state, sxml_map_get_size(integer_key(1), &entry_count) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_map_lookup lookup;
    SXML_TEST_EXPECT(state, sxml_map_find(key_a, key_a, &lookup) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_item_release(put_map);
    sxml_item_release(map);
    sxml_item_release(key_a);
    sxml_item_release(key_e);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    map_fixture_destroy(&fixture);
}

static void test_duplicate_key_policies(sxml_test_state *state) {
    map_fixture fixture;
    SXML_TEST_REQUIRE(state, map_fixture_create(&fixture) == SXML_STATUS_OK);
    /* Small (linear search) and large (hash index) maps alike. */
    const size_t entry_counts[] = {4, 40};
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(entry_counts);
         case_index += 1) {
        size_t entry_count = entry_counts[case_index];
        sxml_map_entry entries[40];
        for (size_t entry_index = 0; entry_index < entry_count;
             entry_index += 1) {
            entries[entry_index] = (sxml_map_entry){
                .key = integer_key((int64_t)entry_index),
                .value = value_of(integer_key((int64_t)entry_index))};
        }
        /* The last entry repeats key 1 as xs:double 1.0e0, atomic-equal. */
        entries[entry_count - 1].key = sxml_atomic_item_from_double(1.0);
        sxml_error_record error_record;
        memset(&error_record, 0, sizeof(error_record));
        sxml_map_request request = entries_request(entries, entry_count);
        request.error_record = &error_record;
        sxml_item map = {.representation = 0};
        SXML_TEST_EXPECT(state, make_map(&fixture, &request, &map) ==
                                    SXML_STATUS_DYNAMIC_ERROR);
        SXML_TEST_EXPECT(state, error_record.code.local_name.byte_count == 8 &&
                                    memcmp(error_record.code.local_name.bytes,
                                           "XQDY0137", 8) == 0);
        SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);

        request.duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST;
        SXML_TEST_REQUIRE(state,
                          make_map(&fixture, &request, &map) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, size_of(map) == entry_count - 1);
        sxml_map_lookup lookup = find_key(map, sxml_atomic_item_from_double(1));
        SXML_TEST_EXPECT(state, lookup.is_found && lookup.entry_index == 1);
        SXML_TEST_EXPECT(state, has_integer(lookup.entry.key, 1));
        SXML_TEST_EXPECT(state,
                         has_integer(single_item(lookup.entry.value), 1));
        sxml_item_release(map);

        request.duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_LAST;
        SXML_TEST_REQUIRE(state,
                          make_map(&fixture, &request, &map) == SXML_STATUS_OK);
        lookup = find_key(map, integer_key(1));
        SXML_TEST_EXPECT(state, lookup.is_found && lookup.entry_index == 1);
        SXML_TEST_EXPECT(state, has_integer(single_item(lookup.entry.value),
                                            (int64_t)entry_count - 1));
        sxml_item_release(map);

        request.duplicate_key_policy = (sxml_duplicate_key_policy)7;
        SXML_TEST_EXPECT(state, make_map(&fixture, &request, &map) ==
                                    SXML_STATUS_INVALID_ARGUMENT);
    }
    /* Keys must be atomic. */
    sxml_item inner = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      make_empty_map(&fixture, &inner) == SXML_STATUS_OK);
    const sxml_map_entry bad_entries[] = {{inner, value_of(inner)}};
    sxml_item map = {.representation = 0};
    sxml_map_request bad_request = entries_request(bad_entries, 1);
    SXML_TEST_EXPECT(state, make_map(&fixture, &bad_request, &map) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_item_release(inner);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    map_fixture_destroy(&fixture);
}

static void test_keys_compare_by_atomic_equal(sxml_test_state *state) {
    map_fixture fixture;
    SXML_TEST_REQUIRE(state, map_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item weds = {.representation = 0};
    SXML_TEST_REQUIRE(
        state,
        make_text_item(&fixture,
                       (text_item_request){
                           .type = SXML_TYPE_XS_ANY_URI,
                           .text = SXML_BYTE_VIEW_FROM_LITERAL("urn:weds")},
                       &weds) == SXML_STATUS_OK);
    /* map-get-014: no NaN in the map, NaN not found. */
    const sxml_map_entry week[] = {
        {integer_key(1), value_of(integer_key(10))},
        {integer_key(2), value_of(integer_key(20))},
        {integer_key(3), value_of(integer_key(30))},
        {weds, value_of(integer_key(40))},
        {integer_key(5), value_of(integer_key(50))},
        {integer_key(6), value_of(integer_key(60))},
        {integer_key(7), value_of(integer_key(70))},
    };
    sxml_map_request request = entries_request(week, SXML_TEST_COUNT(week));
    sxml_item map = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      make_map(&fixture, &request, &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, !find_key(map, sxml_atomic_item_from_double(NAN)).is_found);
    /* xs:string "urn:weds" is atomic-equal to the xs:anyURI key. */
    sxml_item weds_string = {.representation = 0};
    SXML_TEST_REQUIRE(
        state, make_string(&fixture, SXML_BYTE_VIEW_FROM_LITERAL("urn:weds"),
                           &weds_string) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, find_key(map, weds_string).entry_index == 3);
    SXML_TEST_EXPECT(
        state,
        find_key(map, sxml_atomic_item_from_double(3.0)).entry_index == 2);
    SXML_TEST_EXPECT(
        state, !find_key(map, sxml_atomic_item_from_double(3.5)).is_found);
    SXML_TEST_EXPECT(state, !find_key(map, sxml_atomic_item_true()).is_found);
    sxml_item_release(map);

    /* map-get-015: NaN as a key finds NaN, in small and indexed maps. */
    sxml_map_entry with_nan[20];
    for (size_t entry_index = 0; entry_index < SXML_TEST_COUNT(with_nan);
         entry_index += 1) {
        with_nan[entry_index] = (sxml_map_entry){
            .key = integer_key((int64_t)entry_index),
            .value = value_of(integer_key((int64_t)entry_index))};
    }
    with_nan[3].key = sxml_atomic_item_from_double(NAN);
    const size_t map_sizes[] = {7, 20};
    for (size_t size_index = 0; size_index < SXML_TEST_COUNT(map_sizes);
         size_index += 1) {
        request = entries_request(with_nan, map_sizes[size_index]);
        SXML_TEST_REQUIRE(state,
                          make_map(&fixture, &request, &map) == SXML_STATUS_OK);
        sxml_map_lookup lookup =
            find_key(map, sxml_atomic_item_from_double(NAN));
        SXML_TEST_EXPECT(state, lookup.is_found && lookup.entry_index == 3);
        sxml_item_release(map);
    }
    /* map-put-011: xs:float 3.0 replaces the value of key 3, keeping one
       entry; map-put-022-like: double NaN and float NaN are one key. */
    SXML_TEST_REQUIRE(state, make_empty_map(&fixture, &map) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      put_into(&fixture,
                               (put_step){.key = integer_key(3),
                                          .value = value_of(integer_key(1))},
                               &map) == SXML_STATUS_OK);
    sxml_item float_three = {.representation = 0};
    SXML_TEST_REQUIRE(
        state, sxml_atomic_item_make(
                   fixture.item_heap,
                   &(sxml_atomic_item_request){
                       .type = sxml_builtin_type_identifier(SXML_TYPE_XS_FLOAT),
                       .datum = {.datum_kind = SXML_DATUM_KIND_FLOAT,
                                 .datum_details = {.float_number = 3.0F}}},
                   &float_three) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      put_into(&fixture,
                               (put_step){.key = float_three,
                                          .value = value_of(integer_key(2))},
                               &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, size_of(map) == 1);
    SXML_TEST_EXPECT(
        state,
        has_integer(single_item(find_key(map, integer_key(3)).entry.value), 2));
    sxml_item_release(map);
    sxml_item_release(weds);
    sxml_item_release(weds_string);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    map_fixture_destroy(&fixture);
}

/* {"x":5,"r":1,"i":8,"t":9}, then put, put new and remove. */
static void test_put_keeps_positions(sxml_test_state *state) {
    map_fixture fixture;
    SXML_TEST_REQUIRE(state, map_fixture_create(&fixture) == SXML_STATUS_OK);
    static const char *const names[] = {"x", "r", "i", "t", "z"};
    sxml_item keys[SXML_TEST_COUNT(names)];
    for (size_t key_index = 0; key_index < SXML_TEST_COUNT(names);
         key_index += 1) {
        SXML_TEST_REQUIRE(
            state, make_string(&fixture,
                               (sxml_byte_view){
                                   .bytes = (const uint8_t *)names[key_index],
                                   .byte_count = 1},
                               &keys[key_index]) == SXML_STATUS_OK);
    }
    const sxml_map_entry entries[] = {
        {keys[0], value_of(integer_key(5))},
        {keys[1], value_of(integer_key(1))},
        {keys[2], value_of(integer_key(8))},
        {keys[3], value_of(integer_key(9))},
    };
    sxml_map_request request =
        entries_request(entries, SXML_TEST_COUNT(entries));
    sxml_item original = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_map(&fixture, &request, &original) ==
                                 SXML_STATUS_OK);
    /* map-put-025: put "i" keeps x, r, i, t. */
    sxml_item map = original;
    sxml_item_retain(map);
    SXML_TEST_REQUIRE(
        state,
        put_into(&fixture,
                 (put_step){.key = keys[2], .value = value_of(integer_key(0))},
                 &map) == SXML_STATUS_OK);
    for (size_t key_index = 0; key_index < 4; key_index += 1) {
        SXML_TEST_EXPECT(state, key_at(map, key_index).payload.cell ==
                                    keys[key_index].payload.cell);
    }
    SXML_TEST_EXPECT(
        state, has_integer(single_item(find_key(map, keys[2]).entry.value), 0));
    /* The original is unchanged (values are immutable). */
    SXML_TEST_EXPECT(
        state,
        has_integer(single_item(find_key(original, keys[2]).entry.value), 8));
    /* map-put-024: put "z" goes last. */
    SXML_TEST_REQUIRE(
        state,
        put_into(&fixture,
                 (put_step){.key = keys[4], .value = value_of(integer_key(4))},
                 &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, size_of(map) == 5);
    SXML_TEST_EXPECT(state,
                     key_at(map, 4).payload.cell == keys[4].payload.cell);
    /* Remove keeps the order of the rest; removing an absent key gives the
       same map. */
    SXML_TEST_REQUIRE(state,
                      remove_from(&fixture, keys[1], &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, size_of(map) == 4);
    SXML_TEST_EXPECT(state,
                     key_at(map, 1).payload.cell == keys[2].payload.cell);
    sxml_item before = map;
    SXML_TEST_REQUIRE(state,
                      remove_from(&fixture, keys[1], &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, map.payload.cell == before.payload.cell);
    /* Errors: a non-atomic key, a non-map. */
    sxml_item result = {.representation = 0};
    SXML_TEST_EXPECT(
        state, sxml_map_put(fixture.item_heap,
                            &(sxml_map_update){
                                .map = map, .key = map, .value = value_of(map)},
                            &result) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_map_remove(fixture.item_heap,
                                     &(sxml_map_update){.map = keys[0],
                                                        .key = keys[0],
                                                        .value = value_of(map)},
                                     &result) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_item_release(map);
    sxml_item_release(original);
    for (size_t key_index = 0; key_index < SXML_TEST_COUNT(names);
         key_index += 1) {
        sxml_item_release(keys[key_index]);
    }
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    map_fixture_destroy(&fixture);
}

/* Checks that map holds the integer keys of expected, in order, with the
   key times 10 as value. */
typedef struct expected_keys {
    const int64_t *keys;
    size_t key_count;
} expected_keys;

static bool map_has_keys_in_order(sxml_item map, expected_keys expected) {
    if (size_of(map) != expected.key_count) {
        return false;
    }
    for (size_t entry_index = 0; entry_index < expected.key_count;
         entry_index += 1) {
        sxml_map_entry entry;
        if (sxml_map_get_entry(map, entry_index, &entry) != SXML_STATUS_OK ||
            !has_integer(entry.key, expected.keys[entry_index]) ||
            !has_integer(single_item(entry.value),
                         expected.keys[entry_index] * 10)) {
            return false;
        }
        sxml_map_lookup lookup =
            find_key(map, integer_key(expected.keys[entry_index]));
        if (!lookup.is_found || lookup.entry_index != entry_index) {
            return false;
        }
    }
    return true;
}

#define PERSISTENT_KEY_COUNT 3000

static void test_persistent_maps_scale(sxml_test_state *state) {
    map_fixture fixture;
    SXML_TEST_REQUIRE(state, map_fixture_create(&fixture) == SXML_STATUS_OK);
    static int64_t keys[PERSISTENT_KEY_COUNT];
    sxml_item map = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_empty_map(&fixture, &map) == SXML_STATUS_OK);
    sxml_item halfway = {.representation = 0};
    /* Keys in a scrambled order, so hashes and stamps disagree. */
    for (size_t key_index = 0; key_index < PERSISTENT_KEY_COUNT;
         key_index += 1) {
        keys[key_index] = (int64_t)((key_index * 7919) % PERSISTENT_KEY_COUNT);
        SXML_TEST_REQUIRE(
            state, put_into(&fixture,
                            (put_step){.key = integer_key(keys[key_index]),
                                       .value = value_of(
                                           integer_key(keys[key_index] * 10))},
                            &map) == SXML_STATUS_OK);
        if (key_index == PERSISTENT_KEY_COUNT / 2 - 1) {
            halfway = map;
            sxml_item_retain(halfway);
        }
    }
    SXML_TEST_EXPECT(
        state, map_has_keys_in_order(
                   map, (expected_keys){.keys = keys,
                                        .key_count = PERSISTENT_KEY_COUNT}));
    SXML_TEST_EXPECT(
        state,
        map_has_keys_in_order(
            halfway, (expected_keys){.keys = keys,
                                     .key_count = PERSISTENT_KEY_COUNT / 2}));
    SXML_TEST_EXPECT(
        state, !find_key(halfway, integer_key(keys[PERSISTENT_KEY_COUNT - 1]))
                    .is_found);
    /* Remove every other key, then put the first one back: it goes last. */
    static int64_t remaining[PERSISTENT_KEY_COUNT / 2 + 1];
    size_t remaining_count = 0;
    for (size_t key_index = 0; key_index < PERSISTENT_KEY_COUNT;
         key_index += 1) {
        if (key_index % 2 == 0) {
            SXML_TEST_REQUIRE(state, remove_from(&fixture,
                                                 integer_key(keys[key_index]),
                                                 &map) == SXML_STATUS_OK);
        } else {
            remaining[remaining_count] = keys[key_index];
            remaining_count += 1;
        }
    }
    SXML_TEST_REQUIRE(
        state,
        put_into(&fixture,
                 (put_step){.key = integer_key(keys[0]),
                            .value = value_of(integer_key(keys[0] * 10))},
                 &map) == SXML_STATUS_OK);
    remaining[remaining_count] = keys[0];
    remaining_count += 1;
    SXML_TEST_EXPECT(state,
                     map_has_keys_in_order(
                         map, (expected_keys){.keys = remaining,
                                              .key_count = remaining_count}));
    /* Replacing values keeps the order. */
    SXML_TEST_REQUIRE(
        state,
        put_into(&fixture,
                 (put_step){.key = integer_key(remaining[5]),
                            .value = value_of(integer_key(remaining[5] * 10))},
                 &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     map_has_keys_in_order(
                         map, (expected_keys){.keys = remaining,
                                              .key_count = remaining_count}));
    sxml_item_release(halfway);
    sxml_item_release(map);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    map_fixture_destroy(&fixture);
}

static void test_large_blocks_convert_on_change(sxml_test_state *state) {
    map_fixture fixture;
    SXML_TEST_REQUIRE(state, map_fixture_create(&fixture) == SXML_STATUS_OK);
    int64_t keys[100];
    sxml_map_entry entries[100];
    for (size_t entry_index = 0; entry_index < 100; entry_index += 1) {
        keys[entry_index] = 1000 - (int64_t)entry_index;
        entries[entry_index] = (sxml_map_entry){
            .key = integer_key(keys[entry_index]),
            .value = value_of(integer_key(keys[entry_index] * 10))};
    }
    sxml_map_request request = entries_request(entries, 100);
    sxml_item block = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      make_map(&fixture, &request, &block) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, map_has_keys_in_order(
                   block, (expected_keys){.keys = keys, .key_count = 100}));
    sxml_item map = block;
    sxml_item_retain(map);
    SXML_TEST_REQUIRE(state, remove_from(&fixture, integer_key(keys[0]),
                                         &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, map_has_keys_in_order(
                   map, (expected_keys){.keys = keys + 1, .key_count = 99}));
    sxml_item_release(map);
    map = block;
    sxml_item_retain(map);
    SXML_TEST_REQUIRE(
        state,
        put_into(&fixture,
                 (put_step){.key = integer_key(keys[50]),
                            .value = value_of(integer_key(keys[50] * 10))},
                 &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     map_has_keys_in_order(
                         map, (expected_keys){.keys = keys, .key_count = 100}));
    /* Shrink a persistent map to nothing. */
    for (size_t entry_index = 0; entry_index < 100; entry_index += 1) {
        SXML_TEST_REQUIRE(state,
                          remove_from(&fixture, integer_key(keys[entry_index]),
                                      &map) == SXML_STATUS_OK);
    }
    SXML_TEST_EXPECT(state, size_of(map) == 0);
    SXML_TEST_EXPECT(state, !find_key(map, integer_key(keys[0])).is_found);
    sxml_item_release(map);
    sxml_item_release(block);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    map_fixture_destroy(&fixture);
}

/* Registers record(name as xs:string, size as xs:integer, note). */
static sxml_type_identifier register_record_type(map_fixture *fixture) {
    const sxml_record_field fields[] = {
        {SXML_BYTE_VIEW_FROM_LITERAL("name"),
         {sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
          SXML_OCCURRENCE_EXACTLY_ONE}},
        {SXML_BYTE_VIEW_FROM_LITERAL("size"),
         {sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
          SXML_OCCURRENCE_EXACTLY_ONE}},
        {SXML_BYTE_VIEW_FROM_LITERAL("note"),
         {sxml_builtin_type_identifier(SXML_TYPE_ITEM),
          SXML_OCCURRENCE_ZERO_OR_MORE}},
    };
    sxml_type_description description = {
        .variety = SXML_TYPE_VARIETY_RECORD,
        .type_name = {.index = 0},
        .base_type = {.index = 0},
        .primitive_type = {.index = 0},
        .is_builtin = false,
        .variety_details = {
            .record_type = {.is_any_record = false,
                            .is_nominative = false,
                            .record_fields = {.record_fields = fields,
                                              .record_field_count =
                                                  SXML_TEST_COUNT(fields)}}}};
    sxml_type_identifier type = sxml_builtin_type_identifier(SXML_TYPE_NONE);
    if (sxml_type_registry_register_item_type(fixture->registry, &description,
                                              &type) != SXML_STATUS_OK) {
        return sxml_builtin_type_identifier(SXML_TYPE_NONE);
    }
    return type;
}

static void test_records_share_their_shape(sxml_test_state *state) {
    map_fixture fixture;
    SXML_TEST_REQUIRE(state, map_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_type_identifier record_type = register_record_type(&fixture);
    SXML_TEST_REQUIRE(state, record_type.index != SXML_TYPE_NONE);
    sxml_item name = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      make_string(&fixture, SXML_BYTE_VIEW_FROM_LITERAL("box"),
                                  &name) == SXML_STATUS_OK);
    const sxml_sequence values[] = {
        value_of(name), value_of(integer_key(3)), {{.representation = 0}}};
    sxml_item record = {.representation = 0};
    SXML_TEST_REQUIRE(
        state, sxml_record_make(fixture.item_heap,
                                &(sxml_record_request){
                                    .record_type = record_type,
                                    .field_values = {.sequences = values,
                                                     .sequence_count = 3}},
                                &record) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_item_get_kind(record) == SXML_ITEM_KIND_MAP);
    SXML_TEST_EXPECT(state, sxml_item_get_type_annotation(record).index ==
                                record_type.index);
    SXML_TEST_EXPECT(state, size_of(record) == 3);
    sxml_sequence value = {{.representation = 0}};
    SXML_TEST_EXPECT(state, sxml_record_get_field_value(record, 1, &value) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, has_integer(single_item(value), 3));
    SXML_TEST_EXPECT(state, sxml_record_get_field_value(record, 3, &value) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_record_get_field_value(name, 0, &value) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    /* Keys are xs:string in field order (dm:make-record); an absent-able
       field is still an entry, with the empty sequence. */
    SXML_TEST_EXPECT(state, has_text(key_at(record, 0),
                                     SXML_BYTE_VIEW_FROM_LITERAL("name")));
    SXML_TEST_EXPECT(state, has_text(key_at(record, 2),
                                     SXML_BYTE_VIEW_FROM_LITERAL("note")));
    sxml_item size_key = {.representation = 0};
    SXML_TEST_REQUIRE(
        state, make_text_item(&fixture,
                              (text_item_request){
                                  .type = SXML_TYPE_XS_UNTYPED_ATOMIC,
                                  .text = SXML_BYTE_VIEW_FROM_LITERAL("size")},
                              &size_key) == SXML_STATUS_OK);
    sxml_map_lookup lookup = find_key(record, size_key);
    SXML_TEST_EXPECT(state, lookup.is_found && lookup.entry_index == 1);
    SXML_TEST_EXPECT(
        state, has_text(lookup.entry.key, SXML_BYTE_VIEW_FROM_LITERAL("size")));
    SXML_TEST_EXPECT(state, !find_key(record, integer_key(1)).is_found);
    SXML_TEST_EXPECT(state, !find_key(record, name).is_found);

    /* map:put on a record gives a plain map, in both directions. */
    sxml_item map = record;
    sxml_item_retain(map);
    SXML_TEST_REQUIRE(
        state,
        put_into(&fixture,
                 (put_step){.key = size_key, .value = value_of(integer_key(4))},
                 &map) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_item_get_type_annotation(map).index == SXML_TYPE_MAP);
    SXML_TEST_EXPECT(state, size_of(map) == 3);
    SXML_TEST_EXPECT(state, sxml_record_get_field_value(map, 0, &value) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(
        state,
        has_integer(single_item(find_key(map, size_key).entry.value), 4));
    SXML_TEST_REQUIRE(state, remove_from(&fixture, key_at(record, 0), &map) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, size_of(map) == 2);
    SXML_TEST_EXPECT(
        state, has_text(key_at(map, 0), SXML_BYTE_VIEW_FROM_LITERAL("size")));
    sxml_item_release(map);
    map = record;
    sxml_item_retain(map);
    SXML_TEST_REQUIRE(state, remove_from(&fixture, integer_key(1), &map) ==
                                 SXML_STATUS_OK);
    sxml_item_release(map);

    /* Wrong field counts and types that are not record types. */
    SXML_TEST_EXPECT(
        state, sxml_record_make(fixture.item_heap,
                                &(sxml_record_request){
                                    .record_type = record_type,
                                    .field_values = {.sequences = values,
                                                     .sequence_count = 2}},
                                &map) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(
        state,
        sxml_record_make(
            fixture.item_heap,
            &(sxml_record_request){
                .record_type = sxml_builtin_type_identifier(SXML_TYPE_RECORD),
                .field_values = {.sequences = values, .sequence_count = 0}},
            &map) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(
        state,
        sxml_record_make(
            fixture.item_heap,
            &(sxml_record_request){
                .record_type =
                    sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .field_values = {.sequences = values, .sequence_count = 0}},
            &map) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_item_release(record);
    sxml_item_release(size_key);
    sxml_item_release(name);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    map_fixture_destroy(&fixture);
}

/* Makes, changes and reads maps of every variety; releases everything. */
static sxml_status exercise_maps(map_fixture *fixture) {
    sxml_map_entry entries[20];
    for (size_t entry_index = 0; entry_index < 20; entry_index += 1) {
        entries[entry_index] = (sxml_map_entry){
            .key = integer_key((int64_t)entry_index),
            .value = value_of(integer_key((int64_t)entry_index))};
    }
    sxml_item key = {.representation = 0};
    sxml_status status =
        make_string(fixture, SXML_BYTE_VIEW_FROM_LITERAL("size"), &key);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    entries[0].value = value_of(key);
    sxml_item map = {.representation = 0};
    sxml_map_request request = entries_request(entries, 20);
    status = make_map(fixture, &request, &map);
    for (int64_t step_number = 0; status == SXML_STATUS_OK && step_number < 12;
         step_number += 1) {
        status = put_into(fixture,
                          (put_step){.key = integer_key(100 + step_number),
                                     .value = value_of(key)},
                          &map);
    }
    if (status == SXML_STATUS_OK) {
        status = remove_from(fixture, integer_key(3), &map);
    }
    sxml_item small = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        request = entries_request(entries, 5);
        status = make_map(fixture, &request, &small);
    }
    if (status == SXML_STATUS_OK) {
        status = put_into(
            fixture, (put_step){.key = key, .value = value_of(key)}, &small);
    }
    sxml_item record = {.representation = 0};
    sxml_type_identifier record_type = register_record_type(fixture);
    const sxml_sequence values[] = {value_of(key), value_of(integer_key(3)),
                                    value_of(map)};
    if (status == SXML_STATUS_OK) {
        status = sxml_record_make(
            fixture->item_heap,
            &(sxml_record_request){
                .record_type = record_type,
                .field_values = {.sequences = values, .sequence_count = 3}},
            &record);
    }
    sxml_map_lookup lookup;
    if (status == SXML_STATUS_OK) {
        status = sxml_map_find(record, key, &lookup);
    }
    if (status == SXML_STATUS_OK) {
        sxml_item_retain(record);
        sxml_item changed = record;
        status = put_into(
            fixture,
            (put_step){.key = integer_key(1), .value = value_of(small)},
            &changed);
        sxml_item_release(changed);
    }
    sxml_item_release(record);
    sxml_item_release(small);
    sxml_item_release(map);
    sxml_item_release(key);
    return status;
}

static void
test_changes_survive_every_allocation_failure(sxml_test_state *state) {
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
        map_fixture fixture;
        SXML_TEST_REQUIRE(
            state, map_fixture_create_with(
                       sxml_counting_allocator_as_allocator(counting_allocator),
                       &fixture) == SXML_STATUS_OK);
        sxml_status status = exercise_maps(&fixture);
        sxml_item_heap_counts counts =
            sxml_item_heap_read_counts(fixture.item_heap);
        SXML_TEST_EXPECT(state, counts.live_cell_count == 0);
        SXML_TEST_EXPECT(state, counts.live_byte_count == 0);
        map_fixture_destroy(&fixture);
        sxml_allocation_counts allocation_counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_destroy(counting_allocator);
        SXML_TEST_EXPECT(state, allocation_counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state,
                             allocation_counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 2000);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 50);
}

static void test_byte_limit_is_kept(sxml_test_state *state) {
    map_fixture fixture;
    SXML_TEST_REQUIRE(state, map_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item_heap *limited_heap = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_item_heap_create(sxml_default_allocator(),
                                     &(sxml_item_heap_configuration){
                                         .allocator = sxml_default_allocator(),
                                         .name_pool = fixture.name_pool,
                                         .type_registry = fixture.registry,
                                         .maximum_byte_count = 4096},
                                     &limited_heap) == SXML_STATUS_OK);
    sxml_item_heap *unlimited_heap = fixture.item_heap;
    fixture.item_heap = limited_heap;
    sxml_item map = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_empty_map(&fixture, &map) == SXML_STATUS_OK);
    sxml_status status = SXML_STATUS_OK;
    for (int64_t step_number = 0;
         status == SXML_STATUS_OK && step_number < 1000; step_number += 1) {
        status =
            put_into(&fixture,
                     (put_step){.key = integer_key(step_number),
                                .value = value_of(integer_key(step_number))},
                     &map);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_LIMIT_EXCEEDED);
    sxml_item_release(map);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(state, live_byte_count(&fixture) == 0);
    sxml_item_heap_destroy(limited_heap);
    fixture.item_heap = unlimited_heap;
    map_fixture_destroy(&fixture);
}

static const sxml_test_case xdm_map_test_cases[] = {
    {.name = "maps_keep_entry_order", .function = test_maps_keep_entry_order},
    {.name = "duplicate_key_policies", .function = test_duplicate_key_policies},
    {.name = "keys_compare_by_atomic_equal",
     .function = test_keys_compare_by_atomic_equal},
    {.name = "put_keeps_positions", .function = test_put_keeps_positions},
    {.name = "persistent_maps_scale", .function = test_persistent_maps_scale},
    {.name = "large_blocks_convert_on_change",
     .function = test_large_blocks_convert_on_change},
    {.name = "records_share_their_shape",
     .function = test_records_share_their_shape},
    {.name = "changes_survive_every_allocation_failure",
     .function = test_changes_survive_every_allocation_failure},
    {.name = "byte_limit_is_kept", .function = test_byte_limit_is_kept},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xdm_map",
            .cases = xdm_map_test_cases,
            .case_count = SXML_TEST_COUNT(xdm_map_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
