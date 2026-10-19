/*
 * Tests for include/sxml/name.h: the name pool (docs/DECISIONS.md, D31).
 *
 * Imitates tests/memory_test.c: lifecycle checks, then behaviour, then one
 * allocation-failure loop per growing function (D23) and limit checks.
 */
#include "name_internal.h"
#include "sxml/name.h"
#include "test_runner.h"

#include <stdalign.h>

/* Caller memory for objects placed with initialize. */
#define TEST_STORAGE_ALIGNMENT 16
#define TEST_STORAGE_BYTE_COUNT 512

/* Enough names to grow every table and index several times. */
#define TEST_MANY_NAME_COUNT 3000U

typedef struct name_fixture {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[TEST_STORAGE_BYTE_COUNT];
    sxml_name_pool *name_pool;
} name_fixture;

static sxml_name_pool_configuration default_configuration(void) {
    return (sxml_name_pool_configuration){
        .allocator = sxml_default_allocator(),
        .maximum_byte_count = 0,
    };
}

static sxml_status name_fixture_initialize(
    name_fixture *fixture, const sxml_name_pool_configuration *configuration) {
    return sxml_name_pool_initialize(
        (sxml_memory_view){.bytes = fixture->storage,
                           .byte_count = sizeof(fixture->storage)},
        configuration, &fixture->name_pool);
}

static sxml_name_identifier intern_name_or_zero(
    sxml_name_pool *name_pool, const sxml_name_request *request) {
    sxml_name_identifier identifier = {.index = 0};
    if (sxml_name_pool_intern_name(name_pool, request, &identifier) !=
        SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return identifier;
}

/* Room for "name-" and the decimal digits of any uint32_t. */
#define TEST_NUMBERED_TEXT_CAPACITY 32

/* Writes "name-<number>" into text and returns a view of it. */
static sxml_byte_view numbered_text(uint32_t number,
                                    char text[TEST_NUMBERED_TEXT_CAPACITY]) {
    static const char stem[] = "name-";
    size_t byte_count = sizeof(stem) - 1;
    for (size_t byte_index = 0; byte_index < byte_count; byte_index += 1) {
        text[byte_index] = stem[byte_index];
    }
    char digits[16];
    size_t digit_count = 0;
    do {
        digits[digit_count] = (char)('0' + (number % 10U));
        digit_count += 1;
        number /= 10U;
    } while (number != 0);
    while (digit_count != 0) {
        digit_count -= 1;
        text[byte_count] = digits[digit_count];
        byte_count += 1;
    }
    return (sxml_byte_view){.bytes = (const uint8_t *)text,
                            .byte_count = byte_count};
}

/* ------------------------------------------------------------- Lifecycle */

static void test_memory_requirements_fit_storage(sxml_test_state *state) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 0};
    sxml_name_pool_configuration configuration = default_configuration();
    SXML_TEST_EXPECT(state, sxml_name_pool_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, requirements.byte_count != 0);
    SXML_TEST_EXPECT(state, requirements.byte_count <= TEST_STORAGE_BYTE_COUNT);
    SXML_TEST_EXPECT(state, requirements.alignment <= TEST_STORAGE_ALIGNMENT);
}

static void test_null_allocator_is_invalid(sxml_test_state *state) {
    sxml_name_pool_configuration configuration = {
        .allocator = {.state = NULL, .operations = NULL},
        .maximum_byte_count = 0,
    };
    sxml_memory_requirements requirements = {.byte_count = 7, .alignment = 7};
    SXML_TEST_EXPECT(state, sxml_name_pool_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, requirements.byte_count == 7);
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    SXML_TEST_EXPECT(state, name_fixture_initialize(&fixture, &configuration) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture.name_pool == NULL);
}

static void test_initialize_checks_memory(sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = default_configuration();
    sxml_status status = sxml_name_pool_initialize(
        (sxml_memory_view){.bytes = fixture.storage, .byte_count = 8},
        &configuration, &fixture.name_pool);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_INSUFFICIENT_MEMORY);
    status = sxml_name_pool_initialize(
        (sxml_memory_view){.bytes = fixture.storage + 1,
                           .byte_count = sizeof(fixture.storage) - 1},
        &configuration, &fixture.name_pool);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, fixture.name_pool == NULL);
}

static void test_finalize_and_destroy_accept_null(sxml_test_state *state) {
    sxml_name_pool_finalize(NULL);
    sxml_name_pool_destroy(NULL);
    SXML_TEST_EXPECT(state, true);
}

static void test_create_and_destroy_release_everything(
    sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t counting_storage[128] = {0};
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_counting_allocator_initialize(
                   (sxml_memory_view){.bytes = counting_storage,
                                      .byte_count = sizeof(counting_storage)},
                   &(sxml_counting_allocator_configuration){
                       .backing_allocator = sxml_default_allocator(),
                       .failing_allocation_number = 0},
                   &counting_allocator) == SXML_STATUS_OK);
    sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(counting_allocator);
    sxml_name_pool *name_pool = NULL;
    sxml_status status = sxml_name_pool_create(
        allocator,
        &(sxml_name_pool_configuration){.allocator = allocator,
                                        .maximum_byte_count = 0},
        &name_pool);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    /* Initializing allocates nothing beyond the pool itself. */
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(
                                counting_allocator)
                                .live_allocation_count == 1);
    sxml_name_identifier identifier = intern_name_or_zero(
        name_pool,
        &(sxml_name_request){
            .namespace_uri = {.index = SXML_NAMESPACE_NONE},
            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("a")});
    SXML_TEST_EXPECT(state, identifier.index == 1);
    sxml_name_pool_destroy(name_pool);
    sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(counting_allocator);
    sxml_counting_allocator_finalize(counting_allocator);
    SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
}

/* -------------------------------------------------- Namespace URIs */

typedef struct well_known_case {
    sxml_well_known_namespace namespace_value;
    const char *namespace_uri;
} well_known_case;

static void test_well_known_namespaces_have_fixed_identifiers(
    sxml_test_state *state) {
    static const well_known_case cases[] = {
        {SXML_NAMESPACE_XML, "http://www.w3.org/XML/1998/namespace"},
        {SXML_NAMESPACE_XMLNS, "http://www.w3.org/2000/xmlns/"},
        {SXML_NAMESPACE_XML_SCHEMA, "http://www.w3.org/2001/XMLSchema"},
        {SXML_NAMESPACE_XML_SCHEMA_INSTANCE,
         "http://www.w3.org/2001/XMLSchema-instance"},
        {SXML_NAMESPACE_FUNCTIONS, "http://www.w3.org/2005/xpath-functions"},
        {SXML_NAMESPACE_MAP_FUNCTIONS,
         "http://www.w3.org/2005/xpath-functions/map"},
        {SXML_NAMESPACE_ARRAY_FUNCTIONS,
         "http://www.w3.org/2005/xpath-functions/array"},
        {SXML_NAMESPACE_MATH_FUNCTIONS,
         "http://www.w3.org/2005/xpath-functions/math"},
        {SXML_NAMESPACE_ERRORS, "http://www.w3.org/2005/xqt-errors"},
        {SXML_NAMESPACE_LOCAL_FUNCTIONS,
         "http://www.w3.org/2005/xquery-local-functions"},
        {SXML_NAMESPACE_SERIALIZATION,
         "http://www.w3.org/2010/xslt-xquery-serialization"},
        {SXML_NAMESPACE_XSLT, "http://www.w3.org/1999/XSL/Transform"},
        {SXML_NAMESPACE_EXPATH_BINARY, "http://expath.org/ns/binary"},
        {SXML_NAMESPACE_EXPATH_FILE, "http://expath.org/ns/file"},
        {SXML_NAMESPACE_EXSLT_COMMON, "http://exslt.org/common"},
    };
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = default_configuration();
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, SXML_TEST_COUNT(cases) + 1 ==
                                SXML_NAMESPACE_WELL_KNOWN_COUNT);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        sxml_byte_view text =
            sxml_byte_view_from_c_string(cases[case_index].namespace_uri);
        sxml_namespace_uri_identifier expected =
            sxml_well_known_namespace_identifier(
                cases[case_index].namespace_value);
        sxml_namespace_uri_identifier found = {.index = 99};
        SXML_TEST_EXPECT(state, sxml_name_pool_find_namespace_uri(
                                    fixture.name_pool, text, &found));
        SXML_TEST_EXPECT(state, found.index == expected.index);
        sxml_namespace_uri_identifier interned = {.index = 99};
        SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_uri(
                                    fixture.name_pool, text, &interned) ==
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, interned.index == expected.index);
        sxml_byte_view stored = {.bytes = NULL, .byte_count = 0};
        SXML_TEST_EXPECT(state, sxml_name_pool_get_namespace_uri(
                                    fixture.name_pool, expected, &stored) ==
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(stored, text));
    }
    /* None of them took memory. */
    SXML_TEST_EXPECT(state, sxml_name_pool_namespace_uri_limit(
                                fixture.name_pool) ==
                                SXML_NAMESPACE_WELL_KNOWN_COUNT);
    sxml_name_pool_finalize(fixture.name_pool);
}

static void test_empty_namespace_uri_is_no_namespace(sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = default_configuration();
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    sxml_namespace_uri_identifier identifier = {.index = 99};
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_uri(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL(""),
                                &identifier) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, identifier.index == SXML_NAMESPACE_NONE);
    identifier.index = 99;
    SXML_TEST_EXPECT(state, sxml_name_pool_find_namespace_uri(
                                fixture.name_pool,
                                (sxml_byte_view){.bytes = NULL,
                                                 .byte_count = 0},
                                &identifier));
    SXML_TEST_EXPECT(state, identifier.index == SXML_NAMESPACE_NONE);
    sxml_byte_view text = SXML_BYTE_VIEW_FROM_LITERAL("x");
    SXML_TEST_EXPECT(state, sxml_name_pool_get_namespace_uri(
                                fixture.name_pool, identifier, &text) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, text.byte_count == 0);
    sxml_name_pool_finalize(fixture.name_pool);
}

static void test_namespace_uris_intern_to_one_identifier(
    sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = default_configuration();
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    sxml_namespace_uri_identifier unknown = {.index = 99};
    SXML_TEST_EXPECT(state, !sxml_name_pool_find_namespace_uri(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL("urn:a"),
                                &unknown));
    SXML_TEST_EXPECT(state, unknown.index == 99);
    sxml_namespace_uri_identifier first = {.index = 0};
    sxml_namespace_uri_identifier second = {.index = 0};
    sxml_namespace_uri_identifier again = {.index = 0};
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_uri(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL("urn:a"),
                                &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_uri(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL("urn:b"),
                                &second) == SXML_STATUS_OK);
    /* A copy with other storage must find the same entry. */
    char copy[] = "urn:a";
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_uri(
                                fixture.name_pool,
                                sxml_byte_view_from_c_string(copy),
                                &again) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, first.index == SXML_NAMESPACE_WELL_KNOWN_COUNT);
    SXML_TEST_EXPECT(state, second.index == first.index + 1);
    SXML_TEST_EXPECT(state, again.index == first.index);
    SXML_TEST_EXPECT(state, sxml_name_pool_find_namespace_uri(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL("urn:b"),
                                &unknown));
    SXML_TEST_EXPECT(state, unknown.index == second.index);
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    SXML_TEST_EXPECT(state, sxml_name_pool_get_namespace_uri(
                                fixture.name_pool, first, &text) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                text, SXML_BYTE_VIEW_FROM_LITERAL("urn:a")));
    /* The pool copied the text. */
    SXML_TEST_EXPECT(state, text.bytes != (const uint8_t *)copy);
    SXML_TEST_EXPECT(state, sxml_name_pool_get_namespace_uri(
                                fixture.name_pool,
                                (sxml_namespace_uri_identifier){
                                    .index = second.index + 1},
                                &text) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                text, SXML_BYTE_VIEW_FROM_LITERAL("urn:a")));
    sxml_name_pool_finalize(fixture.name_pool);
}

/* -------------------------------------------------------------- Prefixes */

static void test_prefixes_intern_to_one_identifier(sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = default_configuration();
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    sxml_namespace_prefix_identifier none = {.index = 99};
    sxml_namespace_prefix_identifier first = {.index = 0};
    sxml_namespace_prefix_identifier second = {.index = 0};
    sxml_namespace_prefix_identifier again = {.index = 0};
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_prefix(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL(""),
                                &none) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, none.index == 0);
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_prefix(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL("xs"),
                                &first) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_prefix(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL("fn"),
                                &second) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_prefix(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL("xs"),
                                &again) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, first.index == 1);
    SXML_TEST_EXPECT(state, second.index == 2);
    SXML_TEST_EXPECT(state, again.index == 1);
    SXML_TEST_EXPECT(state, sxml_name_pool_namespace_prefix_limit(
                                fixture.name_pool) == 3);
    sxml_byte_view text = SXML_BYTE_VIEW_FROM_LITERAL("x");
    SXML_TEST_EXPECT(state, sxml_name_pool_get_namespace_prefix(
                                fixture.name_pool, none, &text) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, text.byte_count == 0);
    SXML_TEST_EXPECT(state, sxml_name_pool_get_namespace_prefix(
                                fixture.name_pool, second, &text) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                text, SXML_BYTE_VIEW_FROM_LITERAL("fn")));
    SXML_TEST_EXPECT(state, sxml_name_pool_get_namespace_prefix(
                                fixture.name_pool,
                                (sxml_namespace_prefix_identifier){.index = 3},
                                &text) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                text, SXML_BYTE_VIEW_FROM_LITERAL("fn")));
    sxml_name_pool_finalize(fixture.name_pool);
}

/* ----------------------------------------------------------------- Names */

static void test_names_are_namespace_and_local_name(sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = default_configuration();
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    sxml_byte_view local_name = SXML_BYTE_VIEW_FROM_LITERAL("string");
    sxml_name_identifier unprefixed = intern_name_or_zero(
        fixture.name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = local_name});
    sxml_name_identifier in_schema = intern_name_or_zero(
        fixture.name_pool,
        &(sxml_name_request){
            .namespace_uri = sxml_well_known_namespace_identifier(
                SXML_NAMESPACE_XML_SCHEMA),
            .local_name = local_name});
    sxml_name_identifier in_functions = intern_name_or_zero(
        fixture.name_pool,
        &(sxml_name_request){
            .namespace_uri = sxml_well_known_namespace_identifier(
                SXML_NAMESPACE_FUNCTIONS),
            .local_name = local_name});
    sxml_name_identifier schema_again = intern_name_or_zero(
        fixture.name_pool,
        &(sxml_name_request){
            .namespace_uri = sxml_well_known_namespace_identifier(
                SXML_NAMESPACE_XML_SCHEMA),
            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string")});
    /* XDM 4.0 section 5.6: same namespace URI and local name, same name. */
    SXML_TEST_EXPECT(state, unprefixed.index == 1);
    SXML_TEST_EXPECT(state, in_schema.index == 2);
    SXML_TEST_EXPECT(state, in_functions.index == 3);
    SXML_TEST_EXPECT(state, schema_again.index == in_schema.index);
    SXML_TEST_EXPECT(state, sxml_name_pool_name_limit(fixture.name_pool) == 4);
    sxml_name_parts parts = {.namespace_uri = {.index = 99},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(
                                fixture.name_pool, in_functions, &parts) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     parts.namespace_uri.index == SXML_NAMESPACE_FUNCTIONS);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(parts.local_name, local_name));
    SXML_TEST_EXPECT(state, parts.local_name.bytes != local_name.bytes);
    sxml_name_pool_finalize(fixture.name_pool);
}

static void test_intern_name_rejects_invalid_requests(sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = default_configuration();
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    sxml_name_identifier identifier = {.index = 99};
    SXML_TEST_EXPECT(state,
                     sxml_name_pool_intern_name(
                         fixture.name_pool,
                         &(sxml_name_request){
                             .namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("")},
                         &identifier) == SXML_STATUS_INVALID_ARGUMENT);
    /* The first interned URI would get this identifier; none is issued. */
    SXML_TEST_EXPECT(state,
                     sxml_name_pool_intern_name(
                         fixture.name_pool,
                         &(sxml_name_request){
                             .namespace_uri =
                                 {.index = SXML_NAMESPACE_WELL_KNOWN_COUNT},
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("a")},
                         &identifier) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, identifier.index == 99);
    SXML_TEST_EXPECT(state, sxml_name_pool_name_limit(fixture.name_pool) == 1);
    sxml_name_pool_finalize(fixture.name_pool);
}

static void test_get_name_parts_rejects_unissued_identifiers(
    sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = default_configuration();
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    sxml_name_parts parts = {.namespace_uri = {.index = 99},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(
                                fixture.name_pool,
                                (sxml_name_identifier){.index = 0},
                                &parts) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(
                                fixture.name_pool,
                                (sxml_name_identifier){.index = 1},
                                &parts) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(
                                fixture.name_pool,
                                (sxml_name_identifier){.index = UINT32_MAX},
                                &parts) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, parts.namespace_uri.index == 99);
    sxml_name_pool_finalize(fixture.name_pool);
}

static void test_find_name_never_interns(sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t counting_storage[128] = {0};
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_REQUIRE(
        state, sxml_counting_allocator_initialize(
                   (sxml_memory_view){.bytes = counting_storage,
                                      .byte_count = sizeof(counting_storage)},
                   &(sxml_counting_allocator_configuration){
                       .backing_allocator = sxml_default_allocator(),
                       .failing_allocation_number = 0},
                   &counting_allocator) == SXML_STATUS_OK);
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = {
        .allocator = sxml_counting_allocator_as_allocator(counting_allocator),
        .maximum_byte_count = 0,
    };
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    sxml_name_request request = {
        .namespace_uri = {.index = SXML_NAMESPACE_XML},
        .local_name = SXML_BYTE_VIEW_FROM_LITERAL("lang")};
    sxml_name_identifier identifier = {.index = 99};
    /* An empty pool, then a pool holding other names. */
    SXML_TEST_EXPECT(state, !sxml_name_pool_find_name(fixture.name_pool,
                                                      &request, &identifier));
    sxml_name_identifier other = intern_name_or_zero(
        fixture.name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("lang")});
    SXML_TEST_EXPECT(state, other.index == 1);
    uint64_t attempts_before =
        sxml_counting_allocator_read_counts(counting_allocator)
            .attempted_allocation_count;
    SXML_TEST_EXPECT(state, !sxml_name_pool_find_name(fixture.name_pool,
                                                      &request, &identifier));
    SXML_TEST_EXPECT(state, !sxml_name_pool_find_name(
                                fixture.name_pool,
                                &(sxml_name_request){
                                    .namespace_uri = {.index = 5000},
                                    .local_name = request.local_name},
                                &identifier));
    SXML_TEST_EXPECT(state, !sxml_name_pool_find_name(
                                fixture.name_pool,
                                &(sxml_name_request){
                                    .namespace_uri = {.index = 0},
                                    .local_name = {.bytes = NULL,
                                                   .byte_count = 0}},
                                &identifier));
    SXML_TEST_EXPECT(state, !sxml_name_pool_find_namespace_uri(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL("urn:x"),
                                &(sxml_namespace_uri_identifier){.index = 0}));
    SXML_TEST_EXPECT(state, identifier.index == 99);
    SXML_TEST_EXPECT(state,
                     sxml_counting_allocator_read_counts(counting_allocator)
                             .attempted_allocation_count == attempts_before);
    sxml_name_identifier interned =
        intern_name_or_zero(fixture.name_pool, &request);
    SXML_TEST_EXPECT(state, sxml_name_pool_find_name(fixture.name_pool,
                                                     &request, &identifier));
    SXML_TEST_EXPECT(state, identifier.index == interned.index);
    sxml_name_pool_finalize(fixture.name_pool);
    sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(counting_allocator);
    sxml_counting_allocator_finalize(counting_allocator);
    SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
}

/* Interns TEST_MANY_NAME_COUNT names, URIs and prefixes, returning a status. */
static sxml_status intern_many(sxml_name_pool *name_pool) {
    for (uint32_t number = 0; number < TEST_MANY_NAME_COUNT; number += 1) {
        char text[TEST_NUMBERED_TEXT_CAPACITY];
        sxml_byte_view view = numbered_text(number, text);
        sxml_namespace_uri_identifier namespace_uri = {.index = 0};
        sxml_namespace_prefix_identifier prefix = {.index = 0};
        sxml_name_identifier name = {.index = 0};
        sxml_status status =
            sxml_name_pool_intern_namespace_uri(name_pool, view, &namespace_uri);
        if (status == SXML_STATUS_OK) {
            status =
                sxml_name_pool_intern_namespace_prefix(name_pool, view, &prefix);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_name(
                name_pool,
                &(sxml_name_request){.namespace_uri = namespace_uri,
                                     .local_name = view},
                &name);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

static void test_many_names_survive_growth(sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = default_configuration();
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, intern_many(fixture.name_pool) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_name_pool_name_limit(fixture.name_pool) ==
                                TEST_MANY_NAME_COUNT + 1);
    SXML_TEST_EXPECT(state, sxml_name_pool_namespace_uri_limit(
                                fixture.name_pool) ==
                                TEST_MANY_NAME_COUNT +
                                    SXML_NAMESPACE_WELL_KNOWN_COUNT);
    for (uint32_t number = 0; number < TEST_MANY_NAME_COUNT; number += 1) {
        char text[TEST_NUMBERED_TEXT_CAPACITY];
        sxml_byte_view view = numbered_text(number, text);
        sxml_namespace_uri_identifier namespace_uri = {.index = 0};
        SXML_TEST_REQUIRE(state, sxml_name_pool_find_namespace_uri(
                                     fixture.name_pool, view, &namespace_uri));
        SXML_TEST_EXPECT(state, namespace_uri.index ==
                                    number + SXML_NAMESPACE_WELL_KNOWN_COUNT);
        sxml_name_identifier name = {.index = 0};
        SXML_TEST_REQUIRE(state, sxml_name_pool_find_name(
                                     fixture.name_pool,
                                     &(sxml_name_request){
                                         .namespace_uri = namespace_uri,
                                         .local_name = view},
                                     &name));
        SXML_TEST_EXPECT(state, name.index == number + 1);
        sxml_name_parts parts = {
            .namespace_uri = {.index = 0},
            .local_name = {.bytes = NULL, .byte_count = 0}};
        SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(
                                    fixture.name_pool, name, &parts) ==
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, parts.namespace_uri.index ==
                                    namespace_uri.index);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(parts.local_name, view));
        sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
        SXML_TEST_EXPECT(state, sxml_name_pool_get_namespace_prefix(
                                    fixture.name_pool,
                                    (sxml_namespace_prefix_identifier){
                                        .index = number + 1},
                                    &prefix) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(prefix, view));
    }
    /* Interning again gives the same identifiers and adds nothing. */
    SXML_TEST_EXPECT(state, intern_many(fixture.name_pool) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_name_pool_name_limit(fixture.name_pool) ==
                                TEST_MANY_NAME_COUNT + 1);
    sxml_name_pool_finalize(fixture.name_pool);
}

/* ------------------------------------------------------------- Limits */

static void test_byte_limit_stops_growth_and_keeps_names(
    sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = {
        .allocator = sxml_default_allocator(),
        .maximum_byte_count = 8192,
    };
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    sxml_status status = intern_many(fixture.name_pool);
    SXML_TEST_EXPECT(state, status == SXML_STATUS_LIMIT_EXCEEDED);
    uint32_t name_limit = sxml_name_pool_name_limit(fixture.name_pool);
    SXML_TEST_EXPECT(state, name_limit > 1);
    SXML_TEST_EXPECT(state, name_limit < TEST_MANY_NAME_COUNT);
    /* Everything interned before the limit is still there. */
    for (uint32_t index = 1; index < name_limit; index += 1) {
        sxml_name_parts parts = {
            .namespace_uri = {.index = 0},
            .local_name = {.bytes = NULL, .byte_count = 0}};
        SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(
                                    fixture.name_pool,
                                    (sxml_name_identifier){.index = index},
                                    &parts) == SXML_STATUS_OK);
        char text[TEST_NUMBERED_TEXT_CAPACITY];
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    parts.local_name,
                                    numbered_text(index - 1, text)));
    }
    /* Names already interned can still be interned and found. */
    char text[TEST_NUMBERED_TEXT_CAPACITY];
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_uri(
                                fixture.name_pool, numbered_text(0, text),
                                &namespace_uri) == SXML_STATUS_OK);
    sxml_name_identifier name = {.index = 0};
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_name(
                                fixture.name_pool,
                                &(sxml_name_request){
                                    .namespace_uri = namespace_uri,
                                    .local_name = numbered_text(0, text)},
                                &name) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, name.index == 1);
    /* The well-known namespaces need no memory. */
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_uri(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL(
                                    "http://www.w3.org/2001/XMLSchema"),
                                &namespace_uri) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA);
    sxml_name_pool_finalize(fixture.name_pool);
}

static void test_tiny_byte_limit_fails_first_intern(sxml_test_state *state) {
    name_fixture fixture = {.storage = {0}, .name_pool = NULL};
    sxml_name_pool_configuration configuration = {
        .allocator = sxml_default_allocator(),
        .maximum_byte_count = 16,
    };
    SXML_TEST_REQUIRE(state, name_fixture_initialize(&fixture, &configuration) ==
                                 SXML_STATUS_OK);
    sxml_namespace_prefix_identifier prefix = {.index = 99};
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_prefix(
                                fixture.name_pool,
                                SXML_BYTE_VIEW_FROM_LITERAL("p"),
                                &prefix) == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, prefix.index == 99);
    SXML_TEST_EXPECT(state, sxml_name_pool_namespace_prefix_limit(
                                fixture.name_pool) == 1);
    sxml_name_pool_finalize(fixture.name_pool);
}

/* --------------------------------------------------- Allocation failures */

/* Creates a pool through allocator, interns many names, destroys it. */
static sxml_status create_intern_and_destroy(sxml_allocator allocator) {
    sxml_name_pool *name_pool = NULL;
    sxml_status status = sxml_name_pool_create(
        allocator,
        &(sxml_name_pool_configuration){.allocator = allocator,
                                        .maximum_byte_count = 0},
        &name_pool);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* A failed intern must leave the pool usable: retry once, then check
       that every name made before the failure is still found. */
    status = intern_many(name_pool);
    if (status == SXML_STATUS_OK) {
        sxml_name_identifier identifier = {.index = 0};
        char text[TEST_NUMBERED_TEXT_CAPACITY];
        sxml_namespace_uri_identifier namespace_uri = {.index = 0};
        bool found =
            sxml_name_pool_find_namespace_uri(
                name_pool, numbered_text(TEST_MANY_NAME_COUNT - 1, text),
                &namespace_uri) &&
            sxml_name_pool_find_name(
                name_pool,
                &(sxml_name_request){
                    .namespace_uri = namespace_uri,
                    .local_name =
                        numbered_text(TEST_MANY_NAME_COUNT - 1, text)},
                &identifier);
        if (!found || identifier.index != TEST_MANY_NAME_COUNT) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    sxml_name_pool_destroy(name_pool);
    return status;
}

static void test_name_pool_survives_every_allocation_failure(
    sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        alignas(TEST_STORAGE_ALIGNMENT) uint8_t counting_storage[128] = {0};
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(
            state,
            sxml_counting_allocator_initialize(
                (sxml_memory_view){.bytes = counting_storage,
                                   .byte_count = sizeof(counting_storage)},
                &(sxml_counting_allocator_configuration){
                    .backing_allocator = sxml_default_allocator(),
                    .failing_allocation_number = failing_allocation_number},
                &counting_allocator) == SXML_STATUS_OK);
        sxml_status status = create_intern_and_destroy(
            sxml_counting_allocator_as_allocator(counting_allocator));
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_finalize(counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, counts.failed_allocation_count == 0);
            break;
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, counts.failed_allocation_count == 1);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 1000);
    }
    /* The pool, then table growth and text blocks. */
    SXML_TEST_EXPECT(state, failing_allocation_number > 10);
}

/* A failed intern leaves the pool as it was; interning again succeeds. */
static void test_failed_intern_can_be_retried(sxml_test_state *state) {
    for (uint64_t failing_allocation_number = 1; failing_allocation_number <= 3;
         failing_allocation_number += 1) {
        alignas(TEST_STORAGE_ALIGNMENT) uint8_t counting_storage[128] = {0};
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(
            state,
            sxml_counting_allocator_initialize(
                (sxml_memory_view){.bytes = counting_storage,
                                   .byte_count = sizeof(counting_storage)},
                &(sxml_counting_allocator_configuration){
                    .backing_allocator = sxml_default_allocator(),
                    .failing_allocation_number = failing_allocation_number},
                &counting_allocator) == SXML_STATUS_OK);
        name_fixture fixture = {.storage = {0}, .name_pool = NULL};
        sxml_name_pool_configuration configuration = {
            .allocator =
                sxml_counting_allocator_as_allocator(counting_allocator),
            .maximum_byte_count = 0,
        };
        SXML_TEST_REQUIRE(state, name_fixture_initialize(
                                     &fixture, &configuration) ==
                                     SXML_STATUS_OK);
        sxml_name_request request = {
            .namespace_uri = {.index = SXML_NAMESPACE_NONE},
            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("item")};
        /* The first intern allocates entries, slots and a text block. */
        sxml_name_identifier identifier = {.index = 99};
        SXML_TEST_EXPECT(state, sxml_name_pool_intern_name(
                                    fixture.name_pool, &request,
                                    &identifier) == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, identifier.index == 99);
        SXML_TEST_EXPECT(state, !sxml_name_pool_find_name(
                                    fixture.name_pool, &request, &identifier));
        SXML_TEST_EXPECT(state, sxml_name_pool_intern_name(
                                    fixture.name_pool, &request,
                                    &identifier) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, identifier.index == 1);
        sxml_name_pool_finalize(fixture.name_pool);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_finalize(counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
    }
}

static const sxml_test_case name_test_cases[] = {
    {.name = "memory_requirements_fit_storage",
     .function = test_memory_requirements_fit_storage},
    {.name = "null_allocator_is_invalid",
     .function = test_null_allocator_is_invalid},
    {.name = "initialize_checks_memory",
     .function = test_initialize_checks_memory},
    {.name = "finalize_and_destroy_accept_null",
     .function = test_finalize_and_destroy_accept_null},
    {.name = "create_and_destroy_release_everything",
     .function = test_create_and_destroy_release_everything},
    {.name = "well_known_namespaces_have_fixed_identifiers",
     .function = test_well_known_namespaces_have_fixed_identifiers},
    {.name = "empty_namespace_uri_is_no_namespace",
     .function = test_empty_namespace_uri_is_no_namespace},
    {.name = "namespace_uris_intern_to_one_identifier",
     .function = test_namespace_uris_intern_to_one_identifier},
    {.name = "prefixes_intern_to_one_identifier",
     .function = test_prefixes_intern_to_one_identifier},
    {.name = "names_are_namespace_and_local_name",
     .function = test_names_are_namespace_and_local_name},
    {.name = "intern_name_rejects_invalid_requests",
     .function = test_intern_name_rejects_invalid_requests},
    {.name = "get_name_parts_rejects_unissued_identifiers",
     .function = test_get_name_parts_rejects_unissued_identifiers},
    {.name = "find_name_never_interns",
     .function = test_find_name_never_interns},
    {.name = "many_names_survive_growth",
     .function = test_many_names_survive_growth},
    {.name = "byte_limit_stops_growth_and_keeps_names",
     .function = test_byte_limit_stops_growth_and_keeps_names},
    {.name = "tiny_byte_limit_fails_first_intern",
     .function = test_tiny_byte_limit_fails_first_intern},
    {.name = "name_pool_survives_every_allocation_failure",
     .function = test_name_pool_survives_every_allocation_failure},
    {.name = "failed_intern_can_be_retried",
     .function = test_failed_intern_can_be_retried},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "name",
            .cases = name_test_cases,
            .case_count = SXML_TEST_COUNT(name_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
