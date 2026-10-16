/*
 * Tests for atomic items: making them in every primitive and built-in
 * derived type, reading their datum, fn:atomic-equal, the hash and
 * fn:compare (include/sxml/xdm.h, src/xdm_internal.h; D34, D36, D60).
 *
 * Imitates tests/memory_test.c. The equality and ordering cases follow
 * F&O 4.0, section 2.2.1 (fn:atomic-equal) and the QT4 test set fn-compare
 * (compare-duration-*, compare-gYear-*, compare-QName-*, compare-binary-*).
 */
#include "decimal_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xdm_internal.h"

#include <math.h>
#include <string.h>

typedef struct atomic_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
} atomic_fixture;

static sxml_status atomic_fixture_create_with(sxml_allocator allocator,
                                              atomic_fixture *fixture) {
    *fixture = (atomic_fixture){
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
                .allocator = allocator,
                .name_pool = fixture->name_pool,
                .type_registry = fixture->registry,
                .maximum_byte_count = 0},
            &fixture->item_heap);
    }
    return status;
}

static sxml_status atomic_fixture_create(atomic_fixture *fixture) {
    return atomic_fixture_create_with(sxml_default_allocator(), fixture);
}

static void atomic_fixture_destroy(atomic_fixture *fixture) {
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

static uint64_t live_cell_count(const atomic_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
}

static sxml_type_identifier builtin(sxml_builtin_type builtin_type) {
    return sxml_builtin_type_identifier(builtin_type);
}

/* ------------------------------------------------------- Datum builders */

static sxml_atomic_datum integer_datum(int64_t integer) {
    return (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_INTEGER,
                               .datum_details = {.integer = integer}};
}

static sxml_atomic_datum decimal_datum(sxml_byte_view text) {
    return (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_DECIMAL,
                               .datum_details = {.decimal_text = text}};
}

static sxml_atomic_datum string_datum(sxml_byte_view text) {
    sxml_string string = {.utf8 = text, .codepoint_count = 0,
                          .is_ascii = false};
    /* Ill-formed text keeps the zero counts, for the make to reject. */
    if (sxml_string_from_utf8(text, &string) != SXML_STATUS_OK) {
        string.codepoint_count = 0;
    }
    return (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_STRING,
                               .datum_details = {.string = string}};
}

static sxml_atomic_datum date_time_datum(sxml_date_time date_time) {
    return (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_DATE_TIME,
                               .datum_details = {.date_time = date_time}};
}

static sxml_atomic_datum duration_datum(sxml_duration duration) {
    return (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_DURATION,
                               .datum_details = {.duration = duration}};
}

static sxml_atomic_datum binary_datum(sxml_byte_view octets) {
    return (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_BINARY,
                               .datum_details = {.binary = octets}};
}

static sxml_atomic_datum qname_datum(sxml_name_identifier name,
                                     sxml_namespace_prefix_identifier prefix) {
    return (sxml_atomic_datum){
        .datum_kind = SXML_DATUM_KIND_QNAME,
        .datum_details = {.qname = {.expanded_name = name,
                                    .namespace_prefix = prefix}}};
}

/* A request to make one atomic item. */
typedef struct make_request {
    sxml_builtin_type type;
    sxml_atomic_datum datum;
} make_request;

static sxml_status make_item(const atomic_fixture *fixture,
                             make_request request, sxml_item *item) {
    return sxml_atomic_item_make(
        fixture->item_heap,
        &(sxml_atomic_item_request){.type = builtin(request.type),
                                    .datum = request.datum},
        item);
}

/* True when the item can be made; releases it. */
static bool can_make(const atomic_fixture *fixture, make_request request) {
    sxml_item item = {.representation = 0};
    if (make_item(fixture, request, &item) != SXML_STATUS_OK) {
        return false;
    }
    sxml_item_release(item);
    return true;
}

static sxml_item float_item(float float_number) {
    sxml_item item = sxml_atomic_item_from_double(0.0);
    item.representation = SXML_ITEM_REPRESENTATION_FLOAT;
    item.flags = SXML_TYPE_XS_FLOAT;
    item.type_or_node_index = SXML_TYPE_XS_FLOAT;
    item.payload.bits = 0;
    item.payload.float_number = float_number;
    return item;
}

static sxml_date_time date_time_value(sxml_date_time date_time) {
    return date_time;
}

/* ------------------------------------------------------- Inline items */

static void test_inline_items_read_back(sxml_test_state *state) {
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(
                                sxml_atomic_item_from_integer(-5), &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, datum.datum_kind == SXML_DATUM_KIND_INTEGER);
    SXML_TEST_EXPECT(state, datum.datum_details.integer == -5);
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(
                                sxml_atomic_item_from_double(2.5), &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, datum.datum_kind == SXML_DATUM_KIND_DOUBLE);
    SXML_TEST_EXPECT(state, datum.datum_details.double_number == 2.5);
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(
                                sxml_atomic_item_true(), &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, datum.datum_kind == SXML_DATUM_KIND_BOOLEAN);
    SXML_TEST_EXPECT(state, datum.datum_details.boolean);
    SXML_TEST_EXPECT(state, sxml_item_get_type_annotation(
                                sxml_atomic_item_false())
                                .index == SXML_TYPE_XS_BOOLEAN);
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_primitive_type(
                                sxml_atomic_item_from_integer(1))
                                .index == SXML_TYPE_XS_DECIMAL);
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_numeric(
                                sxml_atomic_item_from_double(1.0)));
    SXML_TEST_EXPECT(state,
                     !sxml_atomic_item_is_numeric(sxml_atomic_item_true()));
    sxml_item not_an_item = {.representation = 0};
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(not_an_item, &datum) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_primitive_type(not_an_item)
                                .index == SXML_TYPE_NONE);
}

/* ------------------------------------------------------------- Making */

static void test_integer_types_keep_their_range(sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item item = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_item(&fixture,
                                       (make_request){SXML_TYPE_XS_BYTE,
                                                      integer_datum(127)},
                                       &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_item_get_type_annotation(item).index ==
                         SXML_TYPE_XS_BYTE);
    SXML_TEST_EXPECT(state, item.representation ==
                                SXML_ITEM_REPRESENTATION_INTEGER);
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_BYTE,
                                                     integer_datum(128)}));
    SXML_TEST_EXPECT(state,
                     !can_make(&fixture,
                               (make_request){SXML_TYPE_XS_UNSIGNED_BYTE,
                                              integer_datum(-1)}));
    SXML_TEST_EXPECT(state,
                     !can_make(&fixture,
                               (make_request){SXML_TYPE_XS_POSITIVE_INTEGER,
                                              integer_datum(0)}));
    SXML_TEST_EXPECT(state,
                     can_make(&fixture,
                              (make_request){SXML_TYPE_XS_NON_POSITIVE_INTEGER,
                                             integer_datum(0)}));
    SXML_TEST_EXPECT(state,
                     !can_make(&fixture,
                               (make_request){SXML_TYPE_XS_NEGATIVE_INTEGER,
                                              integer_datum(0)}));
    /* Decimal text is accepted for integer types when it is integral. */
    SXML_TEST_REQUIRE(state,
                      make_item(&fixture,
                                (make_request){SXML_TYPE_XS_INTEGER,
                                               decimal_datum(
                                                   SXML_BYTE_VIEW_FROM_LITERAL(
                                                       "-0042.000"))},
                                &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item.representation ==
                                SXML_ITEM_REPRESENTATION_INTEGER);
    SXML_TEST_EXPECT(state, item.payload.integer == -42);
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_INTEGER,
                                                     decimal_datum(
                                                         SXML_BYTE_VIEW_FROM_LITERAL(
                                                             "1.5"))}));
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

static void test_big_integers_live_in_cells(sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_byte_view big_text =
        SXML_BYTE_VIEW_FROM_LITERAL("123456789012345678901234567890");
    sxml_item item = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_item(&fixture,
                                       (make_request){SXML_TYPE_XS_INTEGER,
                                                      decimal_datum(big_text)},
                                       &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item.representation ==
                                SXML_ITEM_REPRESENTATION_ATOMIC_CELL);
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_decimal(item) != NULL);
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(item, &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, datum.datum_kind == SXML_DATUM_KIND_DECIMAL);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                datum.datum_details.decimal_text, big_text));
    sxml_item_release(item);
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_LONG,
                                                     decimal_datum(big_text)}));
    SXML_TEST_EXPECT(state,
                     can_make(&fixture,
                              (make_request){SXML_TYPE_XS_UNSIGNED_LONG,
                                             decimal_datum(
                                                 SXML_BYTE_VIEW_FROM_LITERAL(
                                                     "18446744073709551615"))}));
    SXML_TEST_EXPECT(state,
                     !can_make(&fixture,
                               (make_request){SXML_TYPE_XS_UNSIGNED_LONG,
                                              decimal_datum(
                                                  SXML_BYTE_VIEW_FROM_LITERAL(
                                                      "18446744073709551616"))}));
    SXML_TEST_EXPECT(state,
                     !can_make(&fixture,
                               (make_request){
                                   SXML_TYPE_XS_NON_NEGATIVE_INTEGER,
                                   decimal_datum(SXML_BYTE_VIEW_FROM_LITERAL(
                                       "-123456789012345678901234567890"))}));
    SXML_TEST_EXPECT(state,
                     can_make(&fixture,
                              (make_request){
                                  SXML_TYPE_XS_NEGATIVE_INTEGER,
                                  decimal_datum(SXML_BYTE_VIEW_FROM_LITERAL(
                                      "-123456789012345678901234567890"))}));
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

static void test_decimals_are_cells_with_canonical_text(
    sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item item = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      make_item(&fixture,
                                (make_request){SXML_TYPE_XS_DECIMAL,
                                               decimal_datum(
                                                   SXML_BYTE_VIEW_FROM_LITERAL(
                                                       "+001.500"))},
                                &item) == SXML_STATUS_OK);
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(item, &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                datum.datum_details.decimal_text,
                                SXML_BYTE_VIEW_FROM_LITERAL("1.5")));
    sxml_item_release(item);
    /* xs:decimal is a cell even for an integral value (D36). */
    SXML_TEST_REQUIRE(state, make_item(&fixture,
                                       (make_request){SXML_TYPE_XS_DECIMAL,
                                                      integer_datum(3)},
                                       &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item.representation ==
                                SXML_ITEM_REPRESENTATION_ATOMIC_CELL);
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(item, &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                datum.datum_details.decimal_text,
                                SXML_BYTE_VIEW_FROM_LITERAL("3")));
    sxml_item_release(item);
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_DECIMAL,
                                                     decimal_datum(
                                                         SXML_BYTE_VIEW_FROM_LITERAL(
                                                             "1e5"))}));
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

static void test_decimals_taken_from_operators(sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(fixture.item_heap);
    sxml_decimal decimal;
    SXML_TEST_REQUIRE(state, sxml_decimal_parse(configuration,
                                                SXML_BYTE_VIEW_FROM_LITERAL(
                                                    "2.25"),
                                                &decimal) == SXML_STATUS_OK);
    sxml_item item = {.representation = 0};
    /* An integer type rejects a fraction and leaves the decimal. */
    SXML_TEST_EXPECT(state,
                     sxml_atomic_item_make_taking_decimal(
                         fixture.item_heap,
                         &(sxml_decimal_item_request){
                             .type = builtin(SXML_TYPE_XS_INTEGER),
                             .decimal = &decimal},
                         &item) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_REQUIRE(state,
                      sxml_atomic_item_make_taking_decimal(
                          fixture.item_heap,
                          &(sxml_decimal_item_request){
                              .type = builtin(SXML_TYPE_XS_DECIMAL),
                              .decimal = &decimal},
                          &item) == SXML_STATUS_OK);
    const sxml_decimal *kept = sxml_atomic_item_get_decimal(item);
    SXML_TEST_REQUIRE(state, kept != NULL);
    SXML_TEST_EXPECT(state, sxml_decimal_to_double(kept) == 2.25);
    sxml_item_release(item);
    SXML_TEST_REQUIRE(state, sxml_decimal_from_integer(configuration, 7,
                                                       &decimal) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_atomic_item_make_taking_decimal(
                          fixture.item_heap,
                          &(sxml_decimal_item_request){
                              .type = builtin(SXML_TYPE_XS_SHORT),
                              .decimal = &decimal},
                          &item) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, item.representation ==
                                SXML_ITEM_REPRESENTATION_INTEGER);
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_decimal(item) == NULL);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    SXML_TEST_EXPECT(
        state, sxml_item_heap_read_counts(fixture.item_heap).live_byte_count ==
                   0);
    atomic_fixture_destroy(&fixture);
}

static void test_strings_check_their_type(sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item item = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      make_item(&fixture,
                                (make_request){SXML_TYPE_XS_NCNAME,
                                               string_datum(
                                                   SXML_BYTE_VIEW_FROM_LITERAL(
                                                       "na\xC3\xAFve"))},
                                &item) == SXML_STATUS_OK);
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(item, &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, datum.datum_kind == SXML_DATUM_KIND_STRING);
    SXML_TEST_EXPECT(state, datum.datum_details.string.codepoint_count == 5);
    SXML_TEST_EXPECT(state, !datum.datum_details.string.is_ascii);
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_primitive_type(item).index ==
                                SXML_TYPE_XS_STRING);
    sxml_item_release(item);
    typedef struct string_case {
        sxml_byte_view text;
        sxml_builtin_type type;
        bool is_valid;
    } string_case;
    const string_case cases[] = {
        {SXML_BYTE_VIEW_INITIALIZER("a:b"), SXML_TYPE_XS_NCNAME, false},
        {SXML_BYTE_VIEW_INITIALIZER("1a"), SXML_TYPE_XS_NCNAME, false},
        {SXML_BYTE_VIEW_INITIALIZER(""), SXML_TYPE_XS_NCNAME, false},
        {SXML_BYTE_VIEW_INITIALIZER("a:b"), SXML_TYPE_XS_NAME, true},
        {SXML_BYTE_VIEW_INITIALIZER("1a.-"), SXML_TYPE_XS_NMTOKEN, true},
        {SXML_BYTE_VIEW_INITIALIZER("a b"), SXML_TYPE_XS_NMTOKEN, false},
        {SXML_BYTE_VIEW_INITIALIZER("id1"), SXML_TYPE_XS_ID, true},
        {SXML_BYTE_VIEW_INITIALIZER("a b"), SXML_TYPE_XS_TOKEN, true},
        {SXML_BYTE_VIEW_INITIALIZER("a  b"), SXML_TYPE_XS_TOKEN, false},
        {SXML_BYTE_VIEW_INITIALIZER(" a"), SXML_TYPE_XS_TOKEN, false},
        {SXML_BYTE_VIEW_INITIALIZER(" a "), SXML_TYPE_XS_NORMALIZED_STRING,
         true},
        {SXML_BYTE_VIEW_INITIALIZER("a\tb"), SXML_TYPE_XS_NORMALIZED_STRING,
         false},
        {SXML_BYTE_VIEW_INITIALIZER("en-US"), SXML_TYPE_XS_LANGUAGE, true},
        {SXML_BYTE_VIEW_INITIALIZER("en-"), SXML_TYPE_XS_LANGUAGE, false},
        {SXML_BYTE_VIEW_INITIALIZER("toolonglang"), SXML_TYPE_XS_LANGUAGE,
         false},
        {SXML_BYTE_VIEW_INITIALIZER("a b"), SXML_TYPE_XS_ANY_URI, true},
        {SXML_BYTE_VIEW_INITIALIZER("\xFF"), SXML_TYPE_XS_STRING, false},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        sxml_atomic_datum text_datum = string_datum(cases[case_index].text);
        /* Ill-formed UTF-8 is passed as it is, to be rejected. */
        text_datum.datum_details.string.utf8 = cases[case_index].text;
        SXML_TEST_EXPECT(state, can_make(&fixture,
                                         (make_request){cases[case_index].type,
                                                        text_datum}) ==
                                    cases[case_index].is_valid);
    }
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

static void test_types_and_datum_kinds_must_match(sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_STRING,
                                                     integer_datum(1)}));
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_ANY_ATOMIC_TYPE,
                                                     integer_datum(1)}));
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_NUMERIC,
                                                     integer_datum(1)}));
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_ELEMENT_NODE,
                                                     integer_datum(1)}));
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_NONE,
                                                     integer_datum(1)}));
    SXML_TEST_EXPECT(state,
                     !can_make(&fixture,
                               (make_request){
                                   SXML_TYPE_XS_QNAME,
                                   qname_datum((sxml_name_identifier){0},
                                               (sxml_namespace_prefix_identifier){
                                                   0})}));
    sxml_atomic_datum float_datum = {
        .datum_kind = SXML_DATUM_KIND_FLOAT,
        .datum_details = {.float_number = 1.5F}};
    sxml_item item = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      make_item(&fixture,
                                (make_request){SXML_TYPE_XS_FLOAT, float_datum},
                                &item) == SXML_STATUS_OK);
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(item, &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, datum.datum_kind == SXML_DATUM_KIND_FLOAT);
    SXML_TEST_EXPECT(state, datum.datum_details.float_number == 1.5F);
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_DOUBLE,
                                                     float_datum}));
    atomic_fixture_destroy(&fixture);
}

static void test_dates_and_durations_check_their_fields(
    sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_date_time noon = {.year = 2000, .month = 2, .day = 29, .hour = 12,
                           .minute = 0, .second = 0, .nanosecond = 0,
                           .has_timezone = true,
                           .timezone_offset_minutes = 60};
    sxml_item item = {.representation = 0};
    SXML_TEST_REQUIRE(state, make_item(&fixture,
                                       (make_request){SXML_TYPE_XS_DATE_TIME,
                                                      date_time_datum(noon)},
                                       &item) == SXML_STATUS_OK);
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(item, &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, datum.datum_kind == SXML_DATUM_KIND_DATE_TIME);
    SXML_TEST_EXPECT(state, datum.datum_details.date_time.day == 29);
    SXML_TEST_EXPECT(state,
                     datum.datum_details.date_time.timezone_offset_minutes ==
                         60);
    sxml_item_release(item);
    sxml_date_time not_leap = noon;
    not_leap.year = 1900;
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_DATE_TIME,
                                                     date_time_datum(
                                                         not_leap)}));
    sxml_date_time no_timezone = noon;
    no_timezone.has_timezone = false;
    no_timezone.timezone_offset_minutes = 0;
    SXML_TEST_EXPECT(state,
                     !can_make(&fixture,
                               (make_request){SXML_TYPE_XS_DATE_TIME_STAMP,
                                              date_time_datum(no_timezone)}));
    SXML_TEST_EXPECT(state,
                     can_make(&fixture,
                              (make_request){SXML_TYPE_XS_DATE_TIME_STAMP,
                                             date_time_datum(noon)}));
    /* xs:date has no time; xs:gMonthDay allows --02-29. */
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_DATE,
                                                     date_time_datum(noon)}));
    sxml_date_time month_day = {.year = 0, .month = 2, .day = 29};
    SXML_TEST_EXPECT(state, can_make(&fixture,
                                     (make_request){SXML_TYPE_XS_G_MONTH_DAY,
                                                    date_time_datum(
                                                        month_day)}));
    sxml_date_time bad_offset = noon;
    bad_offset.timezone_offset_minutes = 841;
    SXML_TEST_EXPECT(state, !can_make(&fixture,
                                      (make_request){SXML_TYPE_XS_DATE_TIME,
                                                     date_time_datum(
                                                         bad_offset)}));
    SXML_TEST_EXPECT(
        state,
        can_make(&fixture,
                 (make_request){SXML_TYPE_XS_DURATION,
                                duration_datum((sxml_duration){
                                    .months = -1, .seconds = -5,
                                    .nanoseconds = -1})}));
    SXML_TEST_EXPECT(
        state, !can_make(&fixture,
                         (make_request){SXML_TYPE_XS_DURATION,
                                        duration_datum((sxml_duration){
                                            .months = -1, .seconds = 5,
                                            .nanoseconds = 0})}));
    SXML_TEST_EXPECT(
        state,
        !can_make(&fixture,
                  (make_request){SXML_TYPE_XS_DAY_TIME_DURATION,
                                 duration_datum((sxml_duration){
                                     .months = 1, .seconds = 0,
                                     .nanoseconds = 0})}));
    SXML_TEST_EXPECT(
        state,
        !can_make(&fixture,
                  (make_request){SXML_TYPE_XS_YEAR_MONTH_DURATION,
                                 duration_datum((sxml_duration){
                                     .months = 1, .seconds = 1,
                                     .nanoseconds = 0})}));
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

/* ---------------------------------------------------- Equality and hash */

/* Two items that must be atomic-equal and hash alike. */
static bool are_equal_with_equal_hashes(sxml_item first, sxml_item second) {
    return sxml_atomic_item_is_atomic_equal(first, second) &&
           sxml_atomic_item_is_atomic_equal(second, first) &&
           sxml_atomic_item_hash(first) == sxml_atomic_item_hash(second);
}

static bool are_unequal(sxml_item first, sxml_item second) {
    return !sxml_atomic_item_is_atomic_equal(first, second) &&
           !sxml_atomic_item_is_atomic_equal(second, first);
}

static sxml_item make_or_false(const atomic_fixture *fixture,
                               make_request request) {
    sxml_item item = sxml_atomic_item_false();
    (void)make_item(fixture, request, &item);
    return item;
}

static sxml_item decimal_item(const atomic_fixture *fixture,
                              sxml_byte_view text) {
    return make_or_false(fixture, (make_request){SXML_TYPE_XS_DECIMAL,
                                                 decimal_datum(text)});
}

/* The packet's done-when: equal numerics of different types hash alike. */
static void test_equal_numbers_of_different_types_hash_alike(
    sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_item integer_one = sxml_atomic_item_from_integer(1);
    sxml_item decimal_one =
        decimal_item(&fixture, SXML_BYTE_VIEW_FROM_LITERAL("1.0"));
    sxml_item double_one = sxml_atomic_item_from_double(1.0);
    sxml_item float_one = float_item(1.0F);
    sxml_item short_one = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_SHORT, integer_datum(1)});
    const sxml_item ones[] = {integer_one, decimal_one, double_one, float_one,
                              short_one};
    for (size_t first_index = 0; first_index < SXML_TEST_COUNT(ones);
         first_index += 1) {
        for (size_t second_index = 0; second_index < SXML_TEST_COUNT(ones);
             second_index += 1) {
            SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(
                                        ones[first_index], ones[second_index]));
        }
    }
    /* 0.5 is exactly a double; 0.1 is not. */
    sxml_item decimal_half =
        decimal_item(&fixture, SXML_BYTE_VIEW_FROM_LITERAL("0.5"));
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(
                                decimal_half, sxml_atomic_item_from_double(0.5)));
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(decimal_half,
                                                        float_item(0.5F)));
    sxml_item decimal_tenth =
        decimal_item(&fixture, SXML_BYTE_VIEW_FROM_LITERAL("0.1"));
    SXML_TEST_EXPECT(state, are_unequal(decimal_tenth,
                                        sxml_atomic_item_from_double(0.1)));
    SXML_TEST_EXPECT(state, are_unequal(sxml_atomic_item_from_double(0.1),
                                        float_item(0.1F)));
    /* The exact value of the double 0.1. */
    sxml_item exact_tenth = decimal_item(
        &fixture, SXML_BYTE_VIEW_FROM_LITERAL(
                      "0.1000000000000000055511151231257827021181583404541015"
                      "625"));
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(
                                exact_tenth, sxml_atomic_item_from_double(0.1)));
    /* 2^53 + 1 is no double, but equals the decimal with its digits. */
    sxml_item odd_integer = sxml_atomic_item_from_integer(9007199254740993);
    SXML_TEST_EXPECT(state, are_unequal(odd_integer, sxml_atomic_item_from_double(
                                                         9007199254740992.0)));
    sxml_item odd_decimal = decimal_item(
        &fixture, SXML_BYTE_VIEW_FROM_LITERAL("9007199254740993"));
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(odd_integer,
                                                        odd_decimal));
    /* 2^64, beyond int64_t, is exactly a double. */
    sxml_item two_to_64 = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_INTEGER,
                       decimal_datum(SXML_BYTE_VIEW_FROM_LITERAL(
                           "18446744073709551616"))});
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(
                                two_to_64,
                                sxml_atomic_item_from_double(
                                    18446744073709551616.0)));
    SXML_TEST_EXPECT(state, are_unequal(two_to_64,
                                        sxml_atomic_item_from_integer(INT64_MAX)));
    SXML_TEST_EXPECT(state, are_unequal(sxml_atomic_item_from_integer(INT64_MAX),
                                        sxml_atomic_item_from_double(
                                            9223372036854775807.0)));
    /* NaN equals NaN; +0 equals -0 (F&O 4.0, section 2.2.1). */
    double nan = (double)NAN;
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(
                                sxml_atomic_item_from_double(nan),
                                float_item((float)nan)));
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(
                                sxml_atomic_item_from_double(-0.0),
                                sxml_atomic_item_from_integer(0)));
    SXML_TEST_EXPECT(state, are_unequal(sxml_atomic_item_from_double(nan),
                                        sxml_atomic_item_from_double(1.0)));
    SXML_TEST_EXPECT(state, are_unequal(integer_one, sxml_atomic_item_true()));
    const sxml_item cells[] = {decimal_one, short_one, decimal_half,
                               decimal_tenth, exact_tenth, two_to_64,
                               odd_decimal};
    for (size_t cell_index = 0; cell_index < SXML_TEST_COUNT(cells);
         cell_index += 1) {
        sxml_item_release(cells[cell_index]);
    }
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

static void test_other_types_compare_within_their_family(
    sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_byte_view text = SXML_BYTE_VIEW_FROM_LITERAL("a");
    sxml_item string_a = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_STRING, string_datum(text)});
    sxml_item untyped_a = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_UNTYPED_ATOMIC, string_datum(text)});
    sxml_item uri_a = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_ANY_URI, string_datum(text)});
    sxml_item string_b = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_STRING,
                                 string_datum(SXML_BYTE_VIEW_FROM_LITERAL("b"))});
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(string_a, untyped_a));
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(string_a, uri_a));
    SXML_TEST_EXPECT(state, are_unequal(string_a, string_b));
    SXML_TEST_EXPECT(state, are_unequal(string_a,
                                        sxml_atomic_item_from_integer(1)));
    /* hexBinary and base64Binary compare by octets. */
    static const uint8_t octet_a[] = {0x41};
    sxml_byte_view octets = {.bytes = octet_a, .byte_count = 1};
    sxml_item hex = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_HEX_BINARY, binary_datum(octets)});
    sxml_item base64 = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_BASE64_BINARY, binary_datum(octets)});
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(hex, base64));
    SXML_TEST_EXPECT(state, are_unequal(hex, string_a));
    /* QNames: the prefix takes no part. */
    sxml_name_identifier name = {0};
    SXML_TEST_REQUIRE(state, sxml_name_pool_intern_name(
                                 fixture.name_pool,
                                 &(sxml_name_request){
                                     .namespace_uri = {.index = 0},
                                     .local_name = text},
                                 &name) == SXML_STATUS_OK);
    sxml_item qname_p = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_QNAME,
                       qname_datum(name, (sxml_namespace_prefix_identifier){1})});
    sxml_item qname_q = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_QNAME,
                       qname_datum(name, (sxml_namespace_prefix_identifier){2})});
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(qname_p, qname_q));
    sxml_atomic_datum datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(qname_q, &datum) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, datum.datum_details.qname.namespace_prefix.index ==
                                2);
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(
                                sxml_atomic_item_true(),
                                sxml_atomic_item_true()));
    SXML_TEST_EXPECT(state, are_unequal(sxml_atomic_item_true(),
                                        sxml_atomic_item_false()));
    /* Durations: P1Y equals P12M across duration types. */
    sxml_item year = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_DURATION,
                                 duration_datum((sxml_duration){12, 0, 0})});
    sxml_item months = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_YEAR_MONTH_DURATION,
                                 duration_datum((sxml_duration){12, 0, 0})});
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(year, months));
    const sxml_item cells[] = {string_a, untyped_a, uri_a, string_b, hex,
                               base64, year, months};
    for (size_t cell_index = 0; cell_index < SXML_TEST_COUNT(cells);
         cell_index += 1) {
        sxml_item_release(cells[cell_index]);
    }
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

static void test_date_times_are_equal_as_instants(sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_date_time utc_noon = {.year = 2000, .month = 1, .day = 1,
                               .hour = 12, .has_timezone = true,
                               .timezone_offset_minutes = 0};
    sxml_date_time paris_one = utc_noon;
    paris_one.hour = 13;
    paris_one.timezone_offset_minutes = 60;
    /* 1999-12-31T23:00:00-13:00 is 2000-01-01T12:00:00Z: the year rolls. */
    sxml_date_time pacific = date_time_value((sxml_date_time){
        .year = 1999, .month = 12, .day = 31, .hour = 23,
        .has_timezone = true, .timezone_offset_minutes = -780});
    sxml_date_time local_noon = utc_noon;
    local_noon.has_timezone = false;
    sxml_item first = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_DATE_TIME, date_time_datum(utc_noon)});
    sxml_item second = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_DATE_TIME, date_time_datum(paris_one)});
    sxml_item third = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_DATE_TIME, date_time_datum(pacific)});
    sxml_item local = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_DATE_TIME, date_time_datum(local_noon)});
    sxml_date_time date_only = {.year = 2000, .month = 1, .day = 1,
                                .has_timezone = true};
    sxml_item date = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_DATE, date_time_datum(date_only)});
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(first, second));
    SXML_TEST_EXPECT(state, are_equal_with_equal_hashes(first, third));
    /* With and without a timezone are never equal (F&O 4.0, 2.2.1). */
    SXML_TEST_EXPECT(state, are_unequal(first, local));
    SXML_TEST_EXPECT(state, are_unequal(first, date));
    const sxml_item cells[] = {first, second, third, local, date};
    for (size_t cell_index = 0; cell_index < SXML_TEST_COUNT(cells);
         cell_index += 1) {
        sxml_item_release(cells[cell_index]);
    }
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

/* -------------------------------------------------------------- Compare */

typedef struct ordering_question {
    sxml_item first;
    sxml_item second;
    int16_t implicit_timezone_offset_minutes;
} ordering_question;

/* The ordering, or 2 when compare fails. */
static int order_in(const sxml_item_heap *item_heap,
                    ordering_question question) {
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    sxml_status status = sxml_atomic_item_compare_in_heap(
        item_heap,
        &(sxml_atomic_comparison){
            .first = question.first,
            .second = question.second,
            .implicit_timezone_offset_minutes =
                question.implicit_timezone_offset_minutes,
            .error_record = NULL},
        &ordering);
    return status == SXML_STATUS_OK ? (int)ordering : 2;
}

static int order(sxml_item first, sxml_item second) {
    return order_in(NULL, (ordering_question){first, second, 0});
}

static void test_numbers_order_by_exact_value(sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    double nan = (double)NAN;
    double infinity = (double)INFINITY;
    sxml_item decimal_tenth =
        decimal_item(&fixture, SXML_BYTE_VIEW_FROM_LITERAL("0.1"));
    sxml_item big = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_INTEGER,
                       decimal_datum(SXML_BYTE_VIEW_FROM_LITERAL(
                           "-99999999999999999999999"))});
    /* The double 0.1 is a little more than the decimal 0.1. */
    SXML_TEST_EXPECT(state, order(decimal_tenth,
                                  sxml_atomic_item_from_double(0.1)) == -1);
    SXML_TEST_EXPECT(state, order(sxml_atomic_item_from_double(0.1),
                                  decimal_tenth) == 1);
    SXML_TEST_EXPECT(state, order(sxml_atomic_item_from_integer(
                                      9007199254740993),
                                  sxml_atomic_item_from_double(
                                      9007199254740992.0)) == 1);
    SXML_TEST_EXPECT(state, order(sxml_atomic_item_from_integer(2),
                                  sxml_atomic_item_from_double(2.5)) == -1);
    SXML_TEST_EXPECT(state, order(sxml_atomic_item_from_integer(-2),
                                  sxml_atomic_item_from_double(-2.5)) == 1);
    SXML_TEST_EXPECT(state, order(sxml_atomic_item_from_double(nan),
                                  sxml_atomic_item_from_double(-infinity)) ==
                                -1);
    SXML_TEST_EXPECT(state, order(sxml_atomic_item_from_double(nan),
                                  float_item((float)nan)) == 0);
    SXML_TEST_EXPECT(state, order(decimal_tenth,
                                  sxml_atomic_item_from_double(nan)) == 1);
    SXML_TEST_EXPECT(state, order(big, sxml_atomic_item_from_integer(
                                           INT64_MIN)) == -1);
    SXML_TEST_EXPECT(state, order(big, sxml_atomic_item_from_double(
                                           -infinity)) == 1);
    SXML_TEST_EXPECT(state, order(big, decimal_tenth) == -1);
    SXML_TEST_EXPECT(state, order(sxml_atomic_item_from_integer(INT64_MAX),
                                  sxml_atomic_item_from_double(
                                      9223372036854775808.0)) == -1);
    sxml_item_release(decimal_tenth);
    sxml_item_release(big);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

static void test_compare_follows_fn_compare(sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    /* compare-duration-01, -04, -05, -06, -08. */
    sxml_item year = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_DURATION,
                                 duration_datum((sxml_duration){12, 0, 0})});
    sxml_item thirteen_months = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_DURATION,
                                 duration_datum((sxml_duration){13, 0, 0})});
    sxml_item year_and_day = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_DURATION,
                       duration_datum((sxml_duration){12, 86400, 0})});
    sxml_item two_years = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_DURATION,
                                 duration_datum((sxml_duration){24, 0, 0})});
    sxml_item thousand_days = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_DURATION,
                       duration_datum((sxml_duration){0, 86400000, 0})});
    sxml_item minute = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_DURATION,
                                 duration_datum((sxml_duration){0, 60, 0})});
    sxml_item almost_minute = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_DURATION,
                       duration_datum((sxml_duration){0, 59, 999000000})});
    SXML_TEST_EXPECT(state, order(year, thirteen_months) == -1);
    SXML_TEST_EXPECT(state, order(year, year_and_day) == -1);
    SXML_TEST_EXPECT(state, order(two_years, thousand_days) == 1);
    SXML_TEST_EXPECT(state, order(minute, almost_minute) == 1);
    /* compare-gYear-03: 2005+02:00 starts after 2005+03:00. */
    sxml_item year_plus_two = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_G_YEAR,
                       date_time_datum((sxml_date_time){
                           .year = 2005, .has_timezone = true,
                           .timezone_offset_minutes = 120})});
    sxml_item year_plus_three = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_G_YEAR,
                       date_time_datum((sxml_date_time){
                           .year = 2005, .has_timezone = true,
                           .timezone_offset_minutes = 180})});
    sxml_item year_local = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_G_YEAR,
                                 date_time_datum((sxml_date_time){
                                     .year = 2005})});
    SXML_TEST_EXPECT(state, order(year_plus_two, year_plus_three) == 1);
    /* Without a timezone the implicit one applies. */
    SXML_TEST_EXPECT(state, order_in(NULL, (ordering_question){
                                               year_local, year_plus_three,
                                               180}) == 0);
    SXML_TEST_EXPECT(state, order_in(NULL, (ordering_question){
                                               year_local, year_plus_three,
                                               0}) == 1);
    /* compare-binary-07, -08. */
    static const uint8_t octet_a[] = {0x41};
    sxml_item hex_empty = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_HEX_BINARY,
                       binary_datum((sxml_byte_view){.bytes = NULL,
                                                     .byte_count = 0})});
    sxml_item base64_a = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_BASE64_BINARY,
                       binary_datum((sxml_byte_view){.bytes = octet_a,
                                                     .byte_count = 1})});
    SXML_TEST_EXPECT(state, order(hex_empty, base64_a) == -1);
    SXML_TEST_EXPECT(state, order(sxml_atomic_item_false(),
                                  sxml_atomic_item_true()) == -1);
    sxml_item string_b = make_or_false(
        &fixture, (make_request){SXML_TYPE_XS_STRING,
                                 string_datum(SXML_BYTE_VIEW_FROM_LITERAL("b"))});
    sxml_item untyped_ab = make_or_false(
        &fixture,
        (make_request){SXML_TYPE_XS_UNTYPED_ATOMIC,
                       string_datum(SXML_BYTE_VIEW_FROM_LITERAL("ab"))});
    SXML_TEST_EXPECT(state, order(untyped_ab, string_b) == -1);
    /* compare-error-02: different families raise XPTY0004. */
    sxml_error_record record;
    sxml_ordering ordering = SXML_ORDERING_LESS;
    SXML_TEST_EXPECT(state,
                     sxml_atomic_item_compare(
                         &(sxml_atomic_comparison){
                             .first = sxml_atomic_item_from_integer(1),
                             .second = string_b,
                             .implicit_timezone_offset_minutes = 0,
                             .error_record = &record},
                         &ordering) == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                record.code.local_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("XPTY0004")));
    SXML_TEST_EXPECT(state, ordering == SXML_ORDERING_LESS);
    SXML_TEST_EXPECT(state, order(year, minute) == 1);
    SXML_TEST_EXPECT(state, order(year, year_plus_two) == 2);
    const sxml_item cells[] = {year, thirteen_months, year_and_day,
                               two_years, thousand_days, minute,
                               almost_minute, year_plus_two, year_plus_three,
                               year_local, hex_empty, base64_a, string_b,
                               untyped_ab};
    for (size_t cell_index = 0; cell_index < SXML_TEST_COUNT(cells);
         cell_index += 1) {
        sxml_item_release(cells[cell_index]);
    }
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    atomic_fixture_destroy(&fixture);
}

static sxml_item qname_item(const atomic_fixture *fixture,
                            sxml_byte_view namespace_uri,
                            sxml_byte_view local_name) {
    sxml_namespace_uri_identifier uri = {0};
    sxml_name_identifier name = {0};
    if (sxml_name_pool_intern_namespace_uri(fixture->name_pool, namespace_uri,
                                            &uri) != SXML_STATUS_OK ||
        sxml_name_pool_intern_name(
            fixture->name_pool,
            &(sxml_name_request){.namespace_uri = uri,
                                 .local_name = local_name},
            &name) != SXML_STATUS_OK) {
        return sxml_atomic_item_false();
    }
    return make_or_false(
        fixture,
        (make_request){SXML_TYPE_XS_QNAME,
                       qname_datum(name, (sxml_namespace_prefix_identifier){0})});
}

static void test_qnames_order_by_uri_then_local_name(sxml_test_state *state) {
    atomic_fixture fixture;
    SXML_TEST_REQUIRE(state, atomic_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_byte_view no_namespace = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view example = SXML_BYTE_VIEW_FROM_LITERAL("http://example.com/");
    sxml_byte_view example_ns =
        SXML_BYTE_VIEW_FROM_LITERAL("http://example.com/ns");
    sxml_item alpha =
        qname_item(&fixture, no_namespace, SXML_BYTE_VIEW_FROM_LITERAL("alpha"));
    sxml_item beta =
        qname_item(&fixture, no_namespace, SXML_BYTE_VIEW_FROM_LITERAL("beta"));
    sxml_item example_alpha =
        qname_item(&fixture, example, SXML_BYTE_VIEW_FROM_LITERAL("alpha"));
    sxml_item example_beta =
        qname_item(&fixture, example, SXML_BYTE_VIEW_FROM_LITERAL("beta"));
    sxml_item example_ns_alpha =
        qname_item(&fixture, example_ns, SXML_BYTE_VIEW_FROM_LITERAL("alpha"));
    const sxml_item_heap *item_heap = fixture.item_heap;
    /* compare-QName-01 to -04. */
    SXML_TEST_EXPECT(state, order_in(item_heap, (ordering_question){
                                                    alpha, beta, 0}) == -1);
    SXML_TEST_EXPECT(state, order_in(item_heap, (ordering_question){
                                                    alpha, example_alpha,
                                                    0}) == -1);
    SXML_TEST_EXPECT(state,
                     order_in(item_heap, (ordering_question){
                                             example_ns_alpha, example_alpha,
                                             0}) == 1);
    SXML_TEST_EXPECT(state,
                     order_in(item_heap, (ordering_question){
                                             example_ns_alpha, example_beta,
                                             0}) == 1);
    SXML_TEST_EXPECT(state, order_in(item_heap, (ordering_question){
                                                    alpha, alpha, 0}) == 0);
    /* The public function orders them with the comparison's name pool
       (D68), and without one cannot order different names. */
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    SXML_TEST_EXPECT(state, sxml_atomic_item_compare(
                                &(sxml_atomic_comparison){
                                    .first = example_beta,
                                    .second = example_ns_alpha,
                                    .implicit_timezone_offset_minutes = 0,
                                    .error_record = NULL,
                                    .name_pool = fixture.name_pool},
                                &ordering) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, ordering == SXML_ORDERING_LESS);
    SXML_TEST_EXPECT(state, sxml_atomic_item_compare(
                                &(sxml_atomic_comparison){
                                    .first = alpha,
                                    .second = beta,
                                    .implicit_timezone_offset_minutes = 0,
                                    .error_record = NULL,
                                    .name_pool = NULL},
                                &ordering) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, ordering == SXML_ORDERING_LESS);
    SXML_TEST_EXPECT(state, order(alpha, alpha) == 0);
    atomic_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Allocation failures */

/* Makes one item of each kind of cell and compares them; returns the
   first failure after releasing everything. */
static sxml_status make_one_of_each_cell(const atomic_fixture *fixture) {
    const make_request requests[] = {
        {SXML_TYPE_XS_DECIMAL,
         decimal_datum(SXML_BYTE_VIEW_FROM_LITERAL(
             "12345678901234567890123456789.123456789"))},
        {SXML_TYPE_XS_INTEGER,
         decimal_datum(SXML_BYTE_VIEW_FROM_LITERAL(
             "-1234567890123456789012345678901234567890"))},
        {SXML_TYPE_XS_STRING,
         string_datum(SXML_BYTE_VIEW_FROM_LITERAL("some text"))},
        {SXML_TYPE_XS_DATE,
         date_time_datum((sxml_date_time){.year = 1999, .month = 12,
                                          .day = 31})},
        {SXML_TYPE_XS_DAY_TIME_DURATION,
         duration_datum((sxml_duration){0, 3600, 0})},
    };
    sxml_item items[SXML_TEST_COUNT(requests)];
    size_t made_count = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && made_count < SXML_TEST_COUNT(requests)) {
        status = make_item(fixture, requests[made_count], &items[made_count]);
        if (status == SXML_STATUS_OK) {
            made_count += 1;
        }
    }
    if (status == SXML_STATUS_OK &&
        (!sxml_atomic_item_is_atomic_equal(items[0], items[0]) ||
         sxml_atomic_item_hash(items[1]) != sxml_atomic_item_hash(items[1]))) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    for (size_t item_index = 0; item_index < made_count; item_index += 1) {
        sxml_item_release(items[item_index]);
    }
    return status;
}

static void test_making_survives_every_allocation_failure(
    sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(state,
                          sxml_counting_allocator_create(
                              sxml_default_allocator(),
                              &(sxml_counting_allocator_configuration){
                                  .backing_allocator = sxml_default_allocator(),
                                  .failing_allocation_number =
                                      failing_allocation_number},
                              &counting_allocator) == SXML_STATUS_OK);
        atomic_fixture fixture;
        SXML_TEST_REQUIRE(
            state, atomic_fixture_create_with(
                       sxml_counting_allocator_as_allocator(counting_allocator),
                       &fixture) == SXML_STATUS_OK);
        sxml_status status = make_one_of_each_cell(&fixture);
        sxml_item_heap_counts counts =
            sxml_item_heap_read_counts(fixture.item_heap);
        SXML_TEST_EXPECT(state, counts.live_cell_count == 0);
        SXML_TEST_EXPECT(state, counts.live_byte_count == 0);
        atomic_fixture_destroy(&fixture);
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
        SXML_TEST_REQUIRE(state, failing_allocation_number < 200);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 5);
}

static const sxml_test_case xdm_atomic_test_cases[] = {
    {.name = "inline_items_read_back",
     .function = test_inline_items_read_back},
    {.name = "integer_types_keep_their_range",
     .function = test_integer_types_keep_their_range},
    {.name = "big_integers_live_in_cells",
     .function = test_big_integers_live_in_cells},
    {.name = "decimals_are_cells_with_canonical_text",
     .function = test_decimals_are_cells_with_canonical_text},
    {.name = "decimals_taken_from_operators",
     .function = test_decimals_taken_from_operators},
    {.name = "strings_check_their_type",
     .function = test_strings_check_their_type},
    {.name = "types_and_datum_kinds_must_match",
     .function = test_types_and_datum_kinds_must_match},
    {.name = "dates_and_durations_check_their_fields",
     .function = test_dates_and_durations_check_their_fields},
    {.name = "equal_numbers_of_different_types_hash_alike",
     .function = test_equal_numbers_of_different_types_hash_alike},
    {.name = "other_types_compare_within_their_family",
     .function = test_other_types_compare_within_their_family},
    {.name = "date_times_are_equal_as_instants",
     .function = test_date_times_are_equal_as_instants},
    {.name = "numbers_order_by_exact_value",
     .function = test_numbers_order_by_exact_value},
    {.name = "compare_follows_fn_compare",
     .function = test_compare_follows_fn_compare},
    {.name = "qnames_order_by_uri_then_local_name",
     .function = test_qnames_order_by_uri_then_local_name},
    {.name = "making_survives_every_allocation_failure",
     .function = test_making_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xdm_atomic",
            .cases = xdm_atomic_test_cases,
            .case_count = SXML_TEST_COUNT(xdm_atomic_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
