/*
 * Tests for casting (src/cast_internal.h): the cast lines of the torture
 * slice as unit cases, the lexical forms and canonical text of every
 * primitive type, the integer types with their facets, xs:NMTOKENS,
 * unions, enumerations, castable, and allocation failures.
 *
 * Imitates tests/memory_test.c (and the fixture of tests/xdm_atomic_test.c).
 * Expected values follow F&O 4.0, section 19 "Casting", and the QT4 test
 * sets prod-CastExpr and prod-CastableExpr named at each slice case.
 */
#include "cast_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "types_internal.h"
#include "xdm_internal.h"

#include <string.h>

typedef struct cast_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
    sxml_item_heap *item_heap;
    /* "p" bound to http://example.com/p. */
    sxml_namespace_binding bindings[2];
    sxml_namespace_binding_view namespace_bindings;
} cast_fixture;

static sxml_status cast_fixture_create_with(sxml_allocator allocator,
                                            cast_fixture *fixture) {
    *fixture =
        (cast_fixture){.name_pool = NULL, .registry = NULL, .item_heap = NULL};
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
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            fixture->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("p"),
            &fixture->bindings[0].namespace_prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            fixture->name_pool,
            SXML_BYTE_VIEW_FROM_LITERAL("http://example.com/p"),
            &fixture->bindings[0].namespace_uri);
    }
    fixture->namespace_bindings = (sxml_namespace_binding_view){
        .namespace_bindings = fixture->bindings, .namespace_binding_count = 1};
    return status;
}

static sxml_status cast_fixture_create(cast_fixture *fixture) {
    return cast_fixture_create_with(sxml_default_allocator(), fixture);
}

static void cast_fixture_destroy(cast_fixture *fixture) {
    sxml_item_heap_destroy(fixture->item_heap);
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

static uint64_t live_cell_count(const cast_fixture *fixture) {
    return sxml_item_heap_read_counts(fixture->item_heap).live_cell_count;
}

static sxml_type_identifier builtin(sxml_builtin_type builtin_type) {
    return sxml_builtin_type_identifier(builtin_type);
}

static sxml_byte_view text_view(const char *text) {
    return sxml_byte_view_from_c_string(text);
}

/* ------------------------------------------------------------ Running */

/* One cast: text made into a source item of source_type (text cast as an
   xs:string when source_type is SXML_TYPE_NONE), then cast to target. */
typedef struct cast_case {
    const char *source_text;
    sxml_builtin_type source_type;
    sxml_type_identifier target_type;
} cast_case;

/* What a cast gave: the status, the error code, the canonical text of the
   result (items separated by spaces) and the result's annotation. */
typedef struct cast_outcome {
    sxml_status status;
    char error_code[16];
    char text[512];
    uint64_t item_count;
    sxml_type_identifier result_type;
} cast_outcome;

static void outcome_set_error(cast_outcome *outcome,
                              const sxml_error_record *error_record) {
    size_t byte_count = error_record->code.local_name.byte_count;
    if (byte_count >= sizeof(outcome->error_code)) {
        byte_count = sizeof(outcome->error_code) - 1;
    }
    memcpy(outcome->error_code, error_record->code.local_name.bytes,
           byte_count);
    outcome->error_code[byte_count] = 0;
}

/* Appends the canonical text of item to outcome->text. */
static sxml_status outcome_append_item(const cast_fixture *fixture,
                                       cast_outcome *outcome, sxml_item item) {
    size_t used = strlen(outcome->text);
    if (used != 0 && used + 1 < sizeof(outcome->text)) {
        outcome->text[used] = ' ';
        used += 1;
    }
    sxml_canonical_text canonical;
    sxml_status status = sxml_cast_write_canonical_text(
        fixture->item_heap,
        &(sxml_canonical_text_request){
            .item = item,
            .output = {.bytes = (uint8_t *)outcome->text + used,
                       .byte_count = sizeof(outcome->text) - used - 1}},
        &canonical);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memmove(outcome->text + used, canonical.text.bytes,
            canonical.text.byte_count);
    outcome->text[used + canonical.text.byte_count] = 0;
    outcome->result_type = sxml_item_get_type_annotation(item);
    return SXML_STATUS_OK;
}

static void outcome_describe(const cast_fixture *fixture, cast_outcome *outcome,
                             sxml_sequence result) {
    sxml_status status =
        sxml_sequence_count(result, NULL, &outcome->item_count);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(result);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        status = outcome_append_item(fixture, outcome, step.item);
    }
    if (status != SXML_STATUS_OK) {
        outcome->status = status;
    }
}

/* Makes the source item of a case; writes has_item false for text. */
typedef struct cast_source {
    bool has_item;
    sxml_item item;
} cast_source;

static sxml_status make_source(const cast_fixture *fixture, cast_case test,
                               cast_source *source) {
    *source = (cast_source){.has_item = false, .item = {.representation = 0}};
    if (test.source_type == SXML_TYPE_NONE) {
        return SXML_STATUS_OK;
    }
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value_from_text(
        fixture->item_heap,
        &(sxml_text_cast_request){.text = text_view(test.source_text),
                                  .target_type = builtin(test.source_type),
                                  .namespace_bindings =
                                      fixture->namespace_bindings,
                                  .error_record = NULL},
        &value);
    if (status == SXML_STATUS_OK) {
        *source = (cast_source){.has_item = true, .item = value.content};
    }
    return status;
}

static cast_outcome run_cast(const cast_fixture *fixture, cast_case test) {
    cast_outcome outcome = {.status = SXML_STATUS_OK,
                            .error_code = {0},
                            .text = {0},
                            .item_count = 0,
                            .result_type = {.index = 0}};
    cast_source source;
    sxml_status status = make_source(fixture, test, &source);
    if (status != SXML_STATUS_OK) {
        outcome.status = SXML_STATUS_INVALID_ARGUMENT;
        memcpy(outcome.error_code, "SOURCE", sizeof("SOURCE"));
        return outcome;
    }
    sxml_error_record error_record;
    sxml_sequence result = sxml_sequence_empty();
    if (source.has_item) {
        status = sxml_cast_make_value(
            fixture->item_heap,
            &(sxml_cast_request){.value = sxml_sequence_from_item(source.item),
                                 .target_type = test.target_type,
                                 .allows_empty = false,
                                 .namespace_bindings =
                                     fixture->namespace_bindings,
                                 .error_record = &error_record},
            &result);
        sxml_item_release(source.item);
    } else {
        status = sxml_cast_make_value_from_text(
            fixture->item_heap,
            &(sxml_text_cast_request){.text = text_view(test.source_text),
                                      .target_type = test.target_type,
                                      .namespace_bindings =
                                          fixture->namespace_bindings,
                                      .error_record = &error_record},
            &result);
    }
    outcome.status = status;
    if (status == SXML_STATUS_DYNAMIC_ERROR ||
        status == SXML_STATUS_TYPE_ERROR ||
        status == SXML_STATUS_STATIC_ERROR) {
        outcome_set_error(&outcome, &error_record);
    } else if (status == SXML_STATUS_OK) {
        outcome_describe(fixture, &outcome, result);
        sxml_sequence_release(result);
    }
    return outcome;
}

/* A case and the canonical text expected from it. */
typedef struct text_expectation {
    cast_case test;
    const char *expected_text;
} text_expectation;

static bool casts_to_text(const cast_fixture *fixture,
                          text_expectation expectation) {
    cast_outcome outcome = run_cast(fixture, expectation.test);
    return outcome.status == SXML_STATUS_OK &&
           strcmp(outcome.text, expectation.expected_text) == 0;
}

/* A case and the error code expected from it. */
typedef struct error_expectation {
    cast_case test;
    const char *error_code;
} error_expectation;

static bool fails_with(const cast_fixture *fixture,
                       error_expectation expectation) {
    cast_outcome outcome = run_cast(fixture, expectation.test);
    return outcome.status != SXML_STATUS_OK &&
           strcmp(outcome.error_code, expectation.error_code) == 0;
}

/* 1 castable, 0 not castable, -1 an error. */
static int castable(const cast_fixture *fixture, cast_case test) {
    cast_source source;
    if (make_source(fixture, test, &source) != SXML_STATUS_OK ||
        !source.has_item) {
        return -1;
    }
    sxml_castability castability = {.is_castable = false};
    sxml_status status = sxml_cast_test_castability(
        fixture->item_heap,
        &(sxml_cast_request){.value = sxml_sequence_from_item(source.item),
                             .target_type = test.target_type,
                             .allows_empty = false,
                             .namespace_bindings = fixture->namespace_bindings,
                             .error_record = NULL},
        &castability);
    sxml_item_release(source.item);
    if (status != SXML_STATUS_OK) {
        return -1;
    }
    return castability.is_castable ? 1 : 0;
}

/* Shorthands for cases. */
static cast_case from_text(const char *text, sxml_builtin_type target) {
    return (cast_case){.source_text = text,
                       .source_type = SXML_TYPE_NONE,
                       .target_type = builtin(target)};
}

/* A source type and a target type. */
typedef struct type_pair {
    sxml_builtin_type source;
    sxml_builtin_type target;
} type_pair;

static cast_case from_item(const char *text, type_pair types) {
    return (cast_case){.source_text = text,
                       .source_type = types.source,
                       .target_type = builtin(types.target)};
}

#define EXPECT_TEXT(state, fixture, cast_value, expected)                      \
    SXML_TEST_EXPECT(                                                          \
        state, casts_to_text(fixture,                                          \
                             (text_expectation){.test = (cast_value),          \
                                                .expected_text = (expected)}))

#define EXPECT_ERROR(state, fixture, cast_value, code)                         \
    SXML_TEST_EXPECT(                                                          \
        state, fails_with(fixture, (error_expectation){.test = (cast_value),   \
                                                       .error_code = (code)}))

/* ------------------------------------------------------ The slice lines */

static void test_slice_cast_lines(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    /* casthc1: (5 + 5) cast as xs:integer. */
    cast_outcome outcome =
        run_cast(&fixture, from_item("10", (type_pair){SXML_TYPE_XS_INTEGER,
                                                       SXML_TYPE_XS_INTEGER}));
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         strcmp(outcome.text, "10") == 0 &&
                         outcome.result_type.index == SXML_TYPE_XS_INTEGER);
    /* casthc13: xs:double(1267.43233E12) cast as xs:string. */
    EXPECT_TEXT(state, &fixture,
                from_item("1267.43233E12", (type_pair){SXML_TYPE_XS_DOUBLE,
                                                       SXML_TYPE_XS_STRING}),
                "1.26743233E15");
    /* casthc25: xs:integer(12678967543233) cast as xs:decimal. */
    outcome = run_cast(&fixture, from_item("12678967543233",
                                           (type_pair){SXML_TYPE_XS_INTEGER,
                                                       SXML_TYPE_XS_DECIMAL}));
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         strcmp(outcome.text, "12678967543233") == 0 &&
                         outcome.result_type.index == SXML_TYPE_XS_DECIMAL);
    /* casthc31: xs:dateTime("1999-05-31T13:20:00-05:00") cast as xs:date. */
    EXPECT_TEXT(
        state, &fixture,
        from_item("1999-05-31T13:20:00-05:00",
                  (type_pair){SXML_TYPE_XS_DATE_TIME, SXML_TYPE_XS_DATE}),
        "1999-05-31-05:00");
    /* CastAs009: xs:untypedAtomic("1e-5") cast as xs:float. */
    EXPECT_TEXT(state, &fixture,
                from_item("1e-5", (type_pair){SXML_TYPE_XS_UNTYPED_ATOMIC,
                                              SXML_TYPE_XS_FLOAT}),
                "0.00001");
    /* CastAs021: xs:untypedAtomic("true") cast as xs:decimal. */
    EXPECT_ERROR(state, &fixture,
                 from_item("true", (type_pair){SXML_TYPE_XS_UNTYPED_ATOMIC,
                                               SXML_TYPE_XS_DECIMAL}),
                 "FORG0001");
    /* CastAs072: xs:untypedAtomic("true") cast as xs:boolean. */
    EXPECT_TEXT(state, &fixture,
                from_item("true", (type_pair){SXML_TYPE_XS_UNTYPED_ATOMIC,
                                              SXML_TYPE_XS_BOOLEAN}),
                "true");
    /* CastAs090: xs:string("NaN") cast as xs:float. */
    EXPECT_TEXT(
        state, &fixture,
        from_item("NaN", (type_pair){SXML_TYPE_XS_STRING, SXML_TYPE_XS_FLOAT}),
        "NaN");
    /* CastAs234: xs:decimal("-1.1234") cast as xs:untypedAtomic. */
    outcome =
        run_cast(&fixture, from_item("-1.1234",
                                     (type_pair){SXML_TYPE_XS_DECIMAL,
                                                 SXML_TYPE_XS_UNTYPED_ATOMIC}));
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK &&
                                strcmp(outcome.text, "-1.1234") == 0 &&
                                outcome.result_type.index ==
                                    SXML_TYPE_XS_UNTYPED_ATOMIC);
    /* CastAs510: xs:gDay("---31") cast as xs:duration. */
    EXPECT_ERROR(state, &fixture,
                 from_item("---31", (type_pair){SXML_TYPE_XS_G_DAY,
                                                SXML_TYPE_XS_DURATION}),
                 "XPTY0004");
    /* K-SeqExprCast-379: xs:date("2002-02-30"). */
    EXPECT_ERROR(state, &fixture, from_text("2002-02-30", SXML_TYPE_XS_DATE),
                 "FORG0001");
    /* K-SeqExprCast-932: xs:date("2004-10-13") cast as xs:gMonthDay. */
    EXPECT_TEXT(state, &fixture,
                from_item("2004-10-13", (type_pair){SXML_TYPE_XS_DATE,
                                                    SXML_TYPE_XS_G_MONTH_DAY}),
                "--10-13");
    /* K2-SeqExprCast-269: xs:negativeInteger(xs:double("INF")). */
    EXPECT_ERROR(state, &fixture,
                 from_item("INF", (type_pair){SXML_TYPE_XS_DOUBLE,
                                              SXML_TYPE_XS_NEGATIVE_INTEGER}),
                 "FOCA0002");
    /* CastableAs008, CastableAs017, CastableAs197. */
    SXML_TEST_EXPECT(
        state,
        castable(&fixture,
                 from_item("NaN", (type_pair){SXML_TYPE_XS_UNTYPED_ATOMIC,
                                              SXML_TYPE_XS_FLOAT})) == 1);
    SXML_TEST_EXPECT(
        state,
        castable(&fixture,
                 from_item("-0.0E0", (type_pair){SXML_TYPE_XS_UNTYPED_ATOMIC,
                                                 SXML_TYPE_XS_DECIMAL})) == 0);
    SXML_TEST_EXPECT(
        state, castable(&fixture,
                        from_item("5.4321E-100",
                                  (type_pair){SXML_TYPE_XS_FLOAT,
                                              SXML_TYPE_XS_HEX_BINARY})) == 0);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* K-SeqExprCast-656: a duration cast to xs:duration keeps its value. */
static void test_duration_cast_to_itself_is_equal(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_sequence original = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state,
                      sxml_cast_make_value_from_text(
                          fixture.item_heap,
                          &(sxml_text_cast_request){
                              .text = text_view("P1Y2M3DT10H30M"),
                              .target_type = builtin(SXML_TYPE_XS_DURATION),
                              .namespace_bindings = {NULL, 0},
                              .error_record = NULL},
                          &original) == SXML_STATUS_OK);
    sxml_sequence cast = sxml_sequence_empty();
    SXML_TEST_EXPECT(
        state,
        sxml_cast_make_value(
            fixture.item_heap,
            &(sxml_cast_request){.value = original,
                                 .target_type = builtin(SXML_TYPE_XS_DURATION),
                                 .allows_empty = false,
                                 .namespace_bindings = {NULL, 0},
                                 .error_record = NULL},
            &cast) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_atomic_item_is_atomic_equal(original.content,
                                                             cast.content));
    sxml_sequence_release(cast);
    sxml_sequence_release(original);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* ------------------------------------------------------------- Numbers */

static void test_doubles_and_floats_write_xpath_text(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    static const struct {
        const char *source;
        const char *expected;
    } doubles[] = {
        {"1e6", "1.0E6"},
        {"999999", "999999"},
        {"0.000001", "0.000001"},
        {"1e-7", "1.0E-7"},
        {"-0", "-0"},
        {"0", "0"},
        {"0.1", "0.1"},
        {"1e300", "1.0E300"},
        {"INF", "INF"},
        {"-INF", "-INF"},
        {"+INF", "INF"},
        {"NaN", "NaN"},
        {"-1.5e10", "-1.5E10"},
        {"123456.5", "123456.5"},
        {".5", "0.5"},
        {"5.", "5"},
        {"4.9e-324", "5.0E-324"},
        {"1.7976931348623157e308", "1.7976931348623157E308"},
        {"1e400", "INF"},
        {"-1e-400", "-0"},
        {"0.30000000000000004", "0.30000000000000004"},
        /* The edges of Clinger's fast path (src/cast_lexical.c) and just
           beyond them, where strtod answers. */
        {"999999999999999", "9.99999999999999E14"},
        {"1000000000000000", "1.0E15"},
        {"1e22", "1.0E22"},
        {"1e23", "1.0E23"},
        {"1e-22", "1.0E-22"},
        {"1.5e-23", "1.5E-23"},
        {"-0.000", "-0"},
        {"-12.50e1", "-125"},
        {"0.1e-21", "1.0E-22"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(doubles);
         case_index += 1) {
        EXPECT_TEXT(state, &fixture,
                    from_text(doubles[case_index].source, SXML_TYPE_XS_DOUBLE),
                    doubles[case_index].expected);
    }
    EXPECT_TEXT(state, &fixture, from_text("3.4028235E38", SXML_TYPE_XS_FLOAT),
                "3.4028235E38");
    EXPECT_TEXT(state, &fixture, from_text("0.1", SXML_TYPE_XS_FLOAT), "0.1");
    EXPECT_TEXT(state, &fixture, from_text("1e39", SXML_TYPE_XS_FLOAT), "INF");
    EXPECT_TEXT(state, &fixture, from_text("16777217", SXML_TYPE_XS_FLOAT),
                "1.6777216E7");
    const char *malformed[] = {"",    "e5",  "1e",  "1.2.3", "inf",
                               "nan", "- 1", "1e+", "0x10",  "1 0"};
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(malformed);
         case_index += 1) {
        EXPECT_ERROR(state, &fixture,
                     from_text(malformed[case_index], SXML_TYPE_XS_DOUBLE),
                     "FORG0001");
    }
    /* Whitespace is collapsed first (F&O 4.0, section 19.2). */
    EXPECT_TEXT(state, &fixture, from_text(" \t12.5\n", SXML_TYPE_XS_DOUBLE),
                "12.5");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_decimals_and_integers(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    EXPECT_TEXT(state, &fixture, from_text("001.500", SXML_TYPE_XS_DECIMAL),
                "1.5");
    EXPECT_TEXT(state, &fixture, from_text("-0.0", SXML_TYPE_XS_DECIMAL), "0");
    EXPECT_TEXT(state, &fixture, from_text("+.5", SXML_TYPE_XS_DECIMAL), "0.5");
    EXPECT_ERROR(state, &fixture, from_text("1e5", SXML_TYPE_XS_DECIMAL),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture, from_text(".", SXML_TYPE_XS_DECIMAL),
                 "FORG0001");
    EXPECT_TEXT(state, &fixture, from_text("+007", SXML_TYPE_XS_INTEGER), "7");
    EXPECT_TEXT(state, &fixture,
                from_text("-9223372036854775808", SXML_TYPE_XS_INTEGER),
                "-9223372036854775808");
    EXPECT_TEXT(
        state, &fixture,
        from_text("123456789012345678901234567890", SXML_TYPE_XS_INTEGER),
        "123456789012345678901234567890");
    EXPECT_ERROR(state, &fixture, from_text("1.0", SXML_TYPE_XS_INTEGER),
                 "FORG0001");
    /* The facets of the integer types (XSD 1.1 Part 2, section 3.4). */
    EXPECT_ERROR(state, &fixture, from_text("300", SXML_TYPE_XS_BYTE),
                 "FORG0001");
    EXPECT_TEXT(state, &fixture, from_text("-128", SXML_TYPE_XS_BYTE), "-128");
    EXPECT_ERROR(state, &fixture, from_text("0", SXML_TYPE_XS_POSITIVE_INTEGER),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture, from_text("1", SXML_TYPE_XS_NEGATIVE_INTEGER),
                 "FORG0001");
    EXPECT_TEXT(state, &fixture,
                from_text("18446744073709551615", SXML_TYPE_XS_UNSIGNED_LONG),
                "18446744073709551615");
    EXPECT_ERROR(state, &fixture,
                 from_text("18446744073709551616", SXML_TYPE_XS_UNSIGNED_LONG),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture, from_text("-1", SXML_TYPE_XS_UNSIGNED_INT),
                 "FORG0001");
    /* Numbers to integers truncate (F&O 4.0, section 19.1.3). */
    EXPECT_TEXT(
        state, &fixture,
        from_item("3.7", (type_pair){SXML_TYPE_XS_DECIMAL, SXML_TYPE_XS_SHORT}),
        "3");
    EXPECT_TEXT(
        state, &fixture,
        from_item("-3.7", (type_pair){SXML_TYPE_XS_DOUBLE, SXML_TYPE_XS_INT}),
        "-3");
    EXPECT_TEXT(state, &fixture,
                from_item("1e20", (type_pair){SXML_TYPE_XS_DOUBLE,
                                              SXML_TYPE_XS_INTEGER}),
                "100000000000000000000");
    EXPECT_ERROR(
        state, &fixture,
        from_item("1e10", (type_pair){SXML_TYPE_XS_DOUBLE, SXML_TYPE_XS_INT}),
        "FORG0001");
    EXPECT_ERROR(
        state, &fixture,
        from_item("NaN", (type_pair){SXML_TYPE_XS_FLOAT, SXML_TYPE_XS_INTEGER}),
        "FOCA0002");
    EXPECT_ERROR(state, &fixture,
                 from_item("-INF", (type_pair){SXML_TYPE_XS_DOUBLE,
                                               SXML_TYPE_XS_DECIMAL}),
                 "FOCA0002");
    EXPECT_TEXT(state, &fixture,
                from_item("0.1", (type_pair){SXML_TYPE_XS_DOUBLE,
                                             SXML_TYPE_XS_DECIMAL}),
                "0.1");
    EXPECT_TEXT(state, &fixture,
                from_item("1e-5", (type_pair){SXML_TYPE_XS_FLOAT,
                                              SXML_TYPE_XS_DECIMAL}),
                "0.00001");
    EXPECT_TEXT(state, &fixture,
                from_item("2.5", (type_pair){SXML_TYPE_XS_DECIMAL,
                                             SXML_TYPE_XS_DOUBLE}),
                "2.5");
    EXPECT_TEXT(state, &fixture,
                from_item("16777217", (type_pair){SXML_TYPE_XS_INTEGER,
                                                  SXML_TYPE_XS_FLOAT}),
                "1.6777216E7");
    /* Booleans (F&O 4.0, sections 19.1.3 and 19.1.6). */
    EXPECT_TEXT(state, &fixture,
                from_item("true", (type_pair){SXML_TYPE_XS_BOOLEAN,
                                              SXML_TYPE_XS_INTEGER}),
                "1");
    EXPECT_TEXT(state, &fixture,
                from_item("NaN", (type_pair){SXML_TYPE_XS_DOUBLE,
                                             SXML_TYPE_XS_BOOLEAN}),
                "false");
    EXPECT_TEXT(state, &fixture,
                from_item("-0.5", (type_pair){SXML_TYPE_XS_DECIMAL,
                                              SXML_TYPE_XS_BOOLEAN}),
                "true");
    EXPECT_TEXT(state, &fixture, from_text(" 1 ", SXML_TYPE_XS_BOOLEAN),
                "true");
    EXPECT_ERROR(state, &fixture, from_text("TRUE", SXML_TYPE_XS_BOOLEAN),
                 "FORG0001");
    EXPECT_TEXT(state, &fixture, from_text(" 12 ", SXML_TYPE_XS_INTEGER), "12");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_decimal_digit_limit(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    /* More digits than SXML_DECIMAL_DEFAULT_MAXIMUM_DIGIT_COUNT. */
    static char digits[SXML_DECIMAL_DEFAULT_MAXIMUM_DIGIT_COUNT + 8];
    memset(digits, '7', sizeof(digits) - 1);
    digits[sizeof(digits) - 1] = 0;
    EXPECT_ERROR(state, &fixture, from_text(digits, SXML_TYPE_XS_INTEGER),
                 "FOCA0003");
    EXPECT_ERROR(state, &fixture, from_text(digits, SXML_TYPE_XS_DECIMAL),
                 "FOCA0001");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* ------------------------------------------------------ Dates and times */

static void test_date_time_lexical_forms(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    static const struct {
        const char *source;
        sxml_builtin_type type;
        const char *expected;
    } forms[] = {
        {"2002-10-10T12:00:00.5000Z", SXML_TYPE_XS_DATE_TIME,
         "2002-10-10T12:00:00.5Z"},
        {"1999-12-31T24:00:00", SXML_TYPE_XS_DATE_TIME, "2000-01-01T00:00:00"},
        {"2000-02-28T24:00:00+01:00", SXML_TYPE_XS_DATE_TIME,
         "2000-02-29T00:00:00+01:00"},
        {"2002-10-10T12:00:00+00:00", SXML_TYPE_XS_DATE_TIME,
         "2002-10-10T12:00:00Z"},
        {"2002-10-10T12:00:00-14:00", SXML_TYPE_XS_DATE_TIME,
         "2002-10-10T12:00:00-14:00"},
        {"2002-10-10T12:00:00.123456789123", SXML_TYPE_XS_DATE_TIME,
         "2002-10-10T12:00:00.123456789"},
        {"24:00:00", SXML_TYPE_XS_TIME, "00:00:00"},
        {"13:20:00.001-05:00", SXML_TYPE_XS_TIME, "13:20:00.001-05:00"},
        {"-0001-01-01", SXML_TYPE_XS_DATE, "-0001-01-01"},
        {"0000-01-01", SXML_TYPE_XS_DATE, "0000-01-01"},
        {"12345-01-01Z", SXML_TYPE_XS_DATE, "12345-01-01Z"},
        {"2001-12", SXML_TYPE_XS_G_YEAR_MONTH, "2001-12"},
        {"2001+02:00", SXML_TYPE_XS_G_YEAR, "2001+02:00"},
        {"--02-29", SXML_TYPE_XS_G_MONTH_DAY, "--02-29"},
        {"---01Z", SXML_TYPE_XS_G_DAY, "---01Z"},
        {"--12", SXML_TYPE_XS_G_MONTH, "--12"},
        {"2002-10-10T12:00:00Z", SXML_TYPE_XS_DATE_TIME_STAMP,
         "2002-10-10T12:00:00Z"},
        /* XSD 1.1 Part 2, D.3.1: "-0000" is year zero. */
        {"-0000-01-01", SXML_TYPE_XS_DATE, "0000-01-01"},
        {"-0000-05", SXML_TYPE_XS_G_YEAR_MONTH, "0000-05"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(forms);
         case_index += 1) {
        EXPECT_TEXT(state, &fixture,
                    from_text(forms[case_index].source, forms[case_index].type),
                    forms[case_index].expected);
    }
    static const struct {
        const char *source;
        sxml_builtin_type type;
    } malformed[] = {
        {"2002-10-10T12:00:00+14:01", SXML_TYPE_XS_DATE_TIME},
        {"2002-10-10T24:00:01", SXML_TYPE_XS_DATE_TIME},
        {"2002-10-10T12:60:00", SXML_TYPE_XS_DATE_TIME},
        {"2002-10-10 12:00:00", SXML_TYPE_XS_DATE_TIME},
        {"2002-10-10T12:00", SXML_TYPE_XS_DATE_TIME},
        {"2002-10-10T12:00:00.", SXML_TYPE_XS_DATE_TIME},
        {"2002-10-10T12:00:00z", SXML_TYPE_XS_DATE_TIME},
        {"2001-02-29T24:00:00", SXML_TYPE_XS_DATE_TIME},
        {"01234-01-01", SXML_TYPE_XS_DATE},
        {"999-01-01", SXML_TYPE_XS_DATE},
        {"2001-13-01", SXML_TYPE_XS_DATE},
        {"2001-1-01", SXML_TYPE_XS_DATE},
        {"--02-30", SXML_TYPE_XS_G_MONTH_DAY},
        {"--13", SXML_TYPE_XS_G_MONTH},
        {"---32", SXML_TYPE_XS_G_DAY},
        {"--01", SXML_TYPE_XS_G_DAY},
        {"2002-10-10T12:00:00", SXML_TYPE_XS_DATE_TIME_STAMP},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(malformed);
         case_index += 1) {
        EXPECT_ERROR(
            state, &fixture,
            from_text(malformed[case_index].source, malformed[case_index].type),
            "FORG0001");
    }
    EXPECT_ERROR(state, &fixture,
                 from_text("12345678901234567-01-01", SXML_TYPE_XS_DATE),
                 "FODT0001");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_date_time_casts_keep_components(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    const char *date_time = "2004-10-13T08:15:30.25+05:30";
    static const struct {
        sxml_builtin_type type;
        const char *expected;
    } targets[] = {
        {SXML_TYPE_XS_TIME, "08:15:30.25+05:30"},
        {SXML_TYPE_XS_G_YEAR_MONTH, "2004-10+05:30"},
        {SXML_TYPE_XS_G_YEAR, "2004+05:30"},
        {SXML_TYPE_XS_G_MONTH_DAY, "--10-13+05:30"},
        {SXML_TYPE_XS_G_DAY, "---13+05:30"},
        {SXML_TYPE_XS_G_MONTH, "--10+05:30"},
        {SXML_TYPE_XS_DATE_TIME_STAMP, "2004-10-13T08:15:30.25+05:30"},
        {SXML_TYPE_XS_STRING, "2004-10-13T08:15:30.25+05:30"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(targets);
         case_index += 1) {
        EXPECT_TEXT(state, &fixture,
                    from_item(date_time, (type_pair){SXML_TYPE_XS_DATE_TIME,
                                                     targets[case_index].type}),
                    targets[case_index].expected);
    }
    EXPECT_TEXT(state, &fixture,
                from_item("2004-10-13Z", (type_pair){SXML_TYPE_XS_DATE,
                                                     SXML_TYPE_XS_DATE_TIME}),
                "2004-10-13T00:00:00Z");
    EXPECT_ERROR(state, &fixture,
                 from_item("2004-10-13",
                           (type_pair){SXML_TYPE_XS_DATE, SXML_TYPE_XS_TIME}),
                 "XPTY0004");
    EXPECT_ERROR(
        state, &fixture,
        from_item("2004", (type_pair){SXML_TYPE_XS_G_YEAR, SXML_TYPE_XS_DATE}),
        "XPTY0004");
    EXPECT_ERROR(state, &fixture,
                 from_item("2004-10-13T00:00:00",
                           (type_pair){SXML_TYPE_XS_DATE_TIME,
                                       SXML_TYPE_XS_DATE_TIME_STAMP}),
                 "FORG0001");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* ------------------------------------------------------------ Durations */

static void test_duration_lexical_forms(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    static const struct {
        const char *source;
        sxml_builtin_type type;
        const char *expected;
    } forms[] = {
        {"P1Y2M3DT10H30M", SXML_TYPE_XS_DURATION, "P1Y2M3DT10H30M"},
        {"PT0S", SXML_TYPE_XS_DURATION, "PT0S"},
        {"P0D", SXML_TYPE_XS_DURATION, "PT0S"},
        {"-P1Y", SXML_TYPE_XS_DURATION, "-P1Y"},
        {"PT1.50S", SXML_TYPE_XS_DURATION, "PT1.5S"},
        {"PT36H", SXML_TYPE_XS_DAY_TIME_DURATION, "P1DT12H"},
        {"PT0.000000001S", SXML_TYPE_XS_DAY_TIME_DURATION, "PT0.000000001S"},
        {"-PT90M", SXML_TYPE_XS_DAY_TIME_DURATION, "-PT1H30M"},
        {"P13M", SXML_TYPE_XS_YEAR_MONTH_DURATION, "P1Y1M"},
        {"P0Y", SXML_TYPE_XS_YEAR_MONTH_DURATION, "P0M"},
        {"P0D", SXML_TYPE_XS_DAY_TIME_DURATION, "PT0S"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(forms);
         case_index += 1) {
        EXPECT_TEXT(state, &fixture,
                    from_text(forms[case_index].source, forms[case_index].type),
                    forms[case_index].expected);
    }
    static const struct {
        const char *source;
        sxml_builtin_type type;
    } malformed[] = {
        {"P", SXML_TYPE_XS_DURATION},
        {"P1DT", SXML_TYPE_XS_DURATION},
        {"PT", SXML_TYPE_XS_DURATION},
        {"P1M1Y", SXML_TYPE_XS_DURATION},
        {"P1.5Y", SXML_TYPE_XS_DURATION},
        {"P1S", SXML_TYPE_XS_DURATION},
        {"PT1D", SXML_TYPE_XS_DURATION},
        {"+P1Y", SXML_TYPE_XS_DURATION},
        {"P-1Y", SXML_TYPE_XS_DURATION},
        {"PT1.S", SXML_TYPE_XS_DURATION},
        {"P1Y", SXML_TYPE_XS_DAY_TIME_DURATION},
        {"P1D", SXML_TYPE_XS_YEAR_MONTH_DURATION},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(malformed);
         case_index += 1) {
        EXPECT_ERROR(
            state, &fixture,
            from_text(malformed[case_index].source, malformed[case_index].type),
            "FORG0001");
    }
    EXPECT_ERROR(state, &fixture,
                 from_text("P99999999999999999999Y", SXML_TYPE_XS_DURATION),
                 "FODT0002");
    EXPECT_ERROR(state, &fixture,
                 from_text("P999999999999999999Y", SXML_TYPE_XS_DURATION),
                 "FODT0002");
    /* Casting between the duration types keeps the part the target has
       (F&O 4.0, section 19.1.4). */
    EXPECT_TEXT(
        state, &fixture,
        from_item("P1Y2M3DT4H", (type_pair){SXML_TYPE_XS_DURATION,
                                            SXML_TYPE_XS_YEAR_MONTH_DURATION}),
        "P1Y2M");
    EXPECT_TEXT(
        state, &fixture,
        from_item("P1Y2M3DT4H", (type_pair){SXML_TYPE_XS_DURATION,
                                            SXML_TYPE_XS_DAY_TIME_DURATION}),
        "P3DT4H");
    EXPECT_TEXT(state, &fixture,
                from_item("P1Y", (type_pair){SXML_TYPE_XS_DURATION,
                                             SXML_TYPE_XS_DAY_TIME_DURATION}),
                "PT0S");
    EXPECT_TEXT(state, &fixture,
                from_item("PT1M", (type_pair){SXML_TYPE_XS_DAY_TIME_DURATION,
                                              SXML_TYPE_XS_UNTYPED_ATOMIC}),
                "PT1M");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* ------------------------------------------- Binary, strings and names */

static void test_binary_lexical_forms(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    EXPECT_TEXT(state, &fixture, from_text("0fb7", SXML_TYPE_XS_HEX_BINARY),
                "0FB7");
    EXPECT_TEXT(state, &fixture, from_text("", SXML_TYPE_XS_HEX_BINARY), "");
    EXPECT_ERROR(state, &fixture, from_text("0fb", SXML_TYPE_XS_HEX_BINARY),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture, from_text("0g", SXML_TYPE_XS_HEX_BINARY),
                 "FORG0001");
    EXPECT_TEXT(state, &fixture, from_text("AQID", SXML_TYPE_XS_BASE64_BINARY),
                "AQID");
    EXPECT_TEXT(state, &fixture,
                from_text(" A Q I D ", SXML_TYPE_XS_BASE64_BINARY), "AQID");
    EXPECT_TEXT(state, &fixture, from_text("AQ= =", SXML_TYPE_XS_BASE64_BINARY),
                "AQ==");
    EXPECT_ERROR(state, &fixture, from_text("AR==", SXML_TYPE_XS_BASE64_BINARY),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture, from_text("AQI", SXML_TYPE_XS_BASE64_BINARY),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture,
                 from_text("AQ==AQID", SXML_TYPE_XS_BASE64_BINARY), "FORG0001");
    EXPECT_ERROR(state, &fixture, from_text("A===", SXML_TYPE_XS_BASE64_BINARY),
                 "FORG0001");
    EXPECT_TEXT(state, &fixture,
                from_item("AQID", (type_pair){SXML_TYPE_XS_BASE64_BINARY,
                                              SXML_TYPE_XS_HEX_BINARY}),
                "010203");
    EXPECT_TEXT(state, &fixture,
                from_item("01", (type_pair){SXML_TYPE_XS_HEX_BINARY,
                                            SXML_TYPE_XS_BASE64_BINARY}),
                "AQ==");
    EXPECT_TEXT(state, &fixture,
                from_item("0102", (type_pair){SXML_TYPE_XS_HEX_BINARY,
                                              SXML_TYPE_XS_BASE64_BINARY}),
                "AQI=");
    EXPECT_ERROR(state, &fixture,
                 from_item("01", (type_pair){SXML_TYPE_XS_HEX_BINARY,
                                             SXML_TYPE_XS_INTEGER}),
                 "XPTY0004");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_strings_follow_their_whitespace_facet(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    EXPECT_TEXT(state, &fixture, from_text(" a\tb ", SXML_TYPE_XS_STRING),
                " a\tb ");
    EXPECT_TEXT(state, &fixture,
                from_text(" a\tb ", SXML_TYPE_XS_NORMALIZED_STRING), " a b ");
    EXPECT_TEXT(state, &fixture, from_text("  a \n  b  ", SXML_TYPE_XS_TOKEN),
                "a b");
    EXPECT_TEXT(state, &fixture, from_text(" en-GB ", SXML_TYPE_XS_LANGUAGE),
                "en-GB");
    EXPECT_ERROR(state, &fixture, from_text("12", SXML_TYPE_XS_NCNAME),
                 "FORG0001");
    EXPECT_ERROR(
        state, &fixture,
        from_item("12", (type_pair){SXML_TYPE_XS_INTEGER, SXML_TYPE_XS_NCNAME}),
        "FORG0001");
    EXPECT_TEXT(state, &fixture,
                from_item("12", (type_pair){SXML_TYPE_XS_INTEGER,
                                            SXML_TYPE_XS_NMTOKEN}),
                "12");
    EXPECT_TEXT(state, &fixture,
                from_text(" http://example.com/a b ", SXML_TYPE_XS_ANY_URI),
                "http://example.com/a b");
    EXPECT_ERROR(
        state, &fixture,
        from_item("1", (type_pair){SXML_TYPE_XS_INTEGER, SXML_TYPE_XS_ANY_URI}),
        "XPTY0004");
    EXPECT_TEXT(state, &fixture,
                from_item("http://a", (type_pair){SXML_TYPE_XS_ANY_URI,
                                                  SXML_TYPE_XS_STRING}),
                "http://a");
    /* A string type source goes through the lexical form, so an NCName
       casts to a number. */
    EXPECT_TEXT(
        state, &fixture,
        from_item("x", (type_pair){SXML_TYPE_XS_NCNAME, SXML_TYPE_XS_STRING}),
        "x");
    EXPECT_TEXT(state, &fixture,
                from_item("12", (type_pair){SXML_TYPE_XS_NMTOKEN,
                                            SXML_TYPE_XS_INTEGER}),
                "12");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_qnames_resolve_their_prefix(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    EXPECT_TEXT(state, &fixture, from_text(" p:local ", SXML_TYPE_XS_QNAME),
                "p:local");
    EXPECT_TEXT(state, &fixture, from_text("xs:integer", SXML_TYPE_XS_QNAME),
                "xs:integer");
    EXPECT_TEXT(state, &fixture, from_text("local", SXML_TYPE_XS_QNAME),
                "local");
    EXPECT_ERROR(state, &fixture, from_text("q:local", SXML_TYPE_XS_QNAME),
                 "FONS0004");
    EXPECT_ERROR(state, &fixture, from_text("a:b:c", SXML_TYPE_XS_QNAME),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture, from_text("1a", SXML_TYPE_XS_QNAME),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture, from_text(":a", SXML_TYPE_XS_QNAME),
                 "FORG0001");
    /* The expanded name uses the binding. */
    sxml_sequence value = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state,
                      sxml_cast_make_value_from_text(
                          fixture.item_heap,
                          &(sxml_text_cast_request){
                              .text = text_view("p:local"),
                              .target_type = builtin(SXML_TYPE_XS_QNAME),
                              .namespace_bindings = fixture.namespace_bindings,
                              .error_record = NULL},
                          &value) == SXML_STATUS_OK);
    sxml_atomic_datum datum;
    SXML_TEST_REQUIRE(state, sxml_atomic_item_get_datum(
                                 value.content, &datum) == SXML_STATUS_OK);
    sxml_name_parts parts;
    SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(
                                fixture.name_pool,
                                datum.datum_details.qname.expanded_name,
                                &parts) == SXML_STATUS_OK &&
                                parts.namespace_uri.index ==
                                    fixture.bindings[0].namespace_uri.index);
    /* QName to string and to xs:untypedAtomic. */
    EXPECT_TEXT(state, &fixture,
                from_item("p:local", (type_pair){SXML_TYPE_XS_QNAME,
                                                 SXML_TYPE_XS_UNTYPED_ATOMIC}),
                "p:local");
    sxml_sequence_release(value);
    /* An unprefixed name takes the default namespace for elements and
       types: a binding with prefix 0. */
    fixture.bindings[1] = (sxml_namespace_binding){
        .namespace_prefix = {.index = 0},
        .namespace_uri = fixture.bindings[0].namespace_uri};
    fixture.namespace_bindings.namespace_binding_count = 2;
    SXML_TEST_REQUIRE(state,
                      sxml_cast_make_value_from_text(
                          fixture.item_heap,
                          &(sxml_text_cast_request){
                              .text = text_view("local"),
                              .target_type = builtin(SXML_TYPE_XS_QNAME),
                              .namespace_bindings = fixture.namespace_bindings,
                              .error_record = NULL},
                          &value) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_atomic_item_get_datum(value.content, &datum) ==
                                    SXML_STATUS_OK &&
                                sxml_name_pool_get_name_parts(
                                    fixture.name_pool,
                                    datum.datum_details.qname.expanded_name,
                                    &parts) == SXML_STATUS_OK &&
                                parts.namespace_uri.index ==
                                    fixture.bindings[0].namespace_uri.index);
    sxml_sequence_release(value);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* ------------------------------------------------ Lists, unions, choices */

static void test_nmtokens_split_into_items(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    cast_outcome outcome =
        run_cast(&fixture, from_text("  a b\t\tc ", SXML_TYPE_XS_NMTOKENS));
    SXML_TEST_EXPECT(
        state, outcome.status == SXML_STATUS_OK && outcome.item_count == 3 &&
                   strcmp(outcome.text, "a b c") == 0 &&
                   outcome.result_type.index == SXML_TYPE_XS_NMTOKEN);
    outcome = run_cast(&fixture, from_text("x", SXML_TYPE_XS_NMTOKENS));
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK &&
                                outcome.item_count == 1);
    EXPECT_ERROR(state, &fixture, from_text("   ", SXML_TYPE_XS_NMTOKENS),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture, from_text("a b,c d", SXML_TYPE_XS_NMTOKENS),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture,
                 from_item("1", (type_pair){SXML_TYPE_XS_INTEGER,
                                            SXML_TYPE_XS_NMTOKENS}),
                 "XPTY0004");
    outcome = run_cast(
        &fixture, from_item("a1 b2", (type_pair){SXML_TYPE_XS_UNTYPED_ATOMIC,
                                                 SXML_TYPE_XS_IDREFS}));
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK &&
                                outcome.item_count == 2);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_unions_try_members_in_order(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    /* xs:numeric is xs:double | xs:float | xs:decimal. */
    cast_outcome outcome =
        run_cast(&fixture, from_text("12", SXML_TYPE_XS_NUMERIC));
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         outcome.result_type.index == SXML_TYPE_XS_DOUBLE &&
                         strcmp(outcome.text, "12") == 0);
    /* An instance of a member is returned unchanged. */
    outcome = run_cast(
        &fixture,
        from_item("5", (type_pair){SXML_TYPE_XS_SHORT, SXML_TYPE_XS_NUMERIC}));
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         outcome.result_type.index == SXML_TYPE_XS_SHORT);
    outcome = run_cast(&fixture,
                       from_item("true", (type_pair){SXML_TYPE_XS_BOOLEAN,
                                                     SXML_TYPE_XS_NUMERIC}));
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         outcome.result_type.index == SXML_TYPE_XS_DOUBLE &&
                         strcmp(outcome.text, "1") == 0);
    EXPECT_ERROR(state, &fixture, from_text("x", SXML_TYPE_XS_NUMERIC),
                 "FORG0001");
    EXPECT_ERROR(state, &fixture,
                 from_item("2001", (type_pair){SXML_TYPE_XS_G_YEAR,
                                               SXML_TYPE_XS_NUMERIC}),
                 "XPTY0004");
    /* xs:error has no members (QT4 xs/error.xml: FORG0001). */
    EXPECT_ERROR(
        state, &fixture,
        from_item("1", (type_pair){SXML_TYPE_XS_INTEGER, SXML_TYPE_XS_ERROR}),
        "FORG0001");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static sxml_status register_enumeration(cast_fixture *fixture,
                                        sxml_byte_view_list values,
                                        sxml_type_identifier *type) {
    return sxml_type_registry_register_item_type(
        fixture->registry,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_ENUMERATION,
            .variety_details = {.enumeration_values = values}},
        type);
}

static void test_enumerations_and_choices(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    static const sxml_byte_view letters[] = {
        SXML_BYTE_VIEW_INITIALIZER("a"), SXML_BYTE_VIEW_INITIALIZER("b"),
        SXML_BYTE_VIEW_INITIALIZER("a1234")};
    sxml_type_identifier enumeration = {.index = 0};
    SXML_TEST_REQUIRE(
        state, register_enumeration(&fixture,
                                    (sxml_byte_view_list){.byte_views = letters,
                                                          .byte_view_count = 3},
                                    &enumeration) == SXML_STATUS_OK);
    /* EnumerationType tests: "a" cast as enum("a", "b", ...). */
    cast_outcome outcome =
        run_cast(&fixture, (cast_case){.source_text = "a",
                                       .source_type = SXML_TYPE_XS_STRING,
                                       .target_type = enumeration});
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         strcmp(outcome.text, "a") == 0 &&
                         outcome.result_type.index == SXML_TYPE_XS_STRING);
    EXPECT_ERROR(state, &fixture,
                 ((cast_case){.source_text = "g",
                              .source_type = SXML_TYPE_XS_STRING,
                              .target_type = enumeration}),
                 "FORG0001");
    SXML_TEST_EXPECT(
        state,
        castable(&fixture, (cast_case){.source_text = "a1234",
                                       .source_type = SXML_TYPE_XS_NCNAME,
                                       .target_type = enumeration}) == 1);
    SXML_TEST_EXPECT(
        state,
        castable(&fixture, (cast_case){.source_text = "A1234",
                                       .source_type = SXML_TYPE_XS_NCNAME,
                                       .target_type = enumeration}) == 0);
    /* (enum("a") | xs:integer): a choice tries its alternatives. */
    sxml_type_identifier members[2] = {enumeration,
                                       builtin(SXML_TYPE_XS_INTEGER)};
    sxml_type_identifier choice = {.index = 0};
    SXML_TEST_REQUIRE(
        state, sxml_type_registry_register_item_type(
                   fixture.registry,
                   &(sxml_type_description){
                       .variety = SXML_TYPE_VARIETY_CHOICE,
                       .variety_details =
                           {.member_types = {.type_identifiers = members,
                                             .type_identifier_count = 2}}},
                   &choice) == SXML_STATUS_OK);
    outcome = run_cast(&fixture, (cast_case){.source_text = " 42 ",
                                             .source_type = SXML_TYPE_NONE,
                                             .target_type = choice});
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         outcome.result_type.index == SXML_TYPE_XS_INTEGER &&
                         strcmp(outcome.text, "42") == 0);
    outcome = run_cast(&fixture, (cast_case){.source_text = "b",
                                             .source_type = SXML_TYPE_NONE,
                                             .target_type = choice});
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         outcome.result_type.index == SXML_TYPE_XS_STRING);
    EXPECT_ERROR(state, &fixture,
                 ((cast_case){.source_text = "z",
                              .source_type = SXML_TYPE_NONE,
                              .target_type = choice}),
                 "FORG0001");
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* (xs:boolean | (xs:boolean | ... (xs:boolean | xs:integer))), depth
   choices deep; SXML_TYPE_NONE when a registration fails. */
static sxml_type_identifier nested_choice(cast_fixture *fixture,
                                          uint32_t depth) {
    sxml_type_identifier type = builtin(SXML_TYPE_XS_INTEGER);
    for (uint32_t level = 0; level < depth && type.index != 0; level += 1) {
        sxml_type_identifier members[2] = {builtin(SXML_TYPE_XS_BOOLEAN),
                                           type};
        type = (sxml_type_identifier){.index = 0};
        if (sxml_type_registry_register_item_type(
                fixture->registry,
                &(sxml_type_description){
                    .variety = SXML_TYPE_VARIETY_CHOICE,
                    .variety_details =
                        {.member_types = {.type_identifiers = members,
                                          .type_identifier_count = 2}}},
                &type) != SXML_STATUS_OK) {
            type = (sxml_type_identifier){.index = 0};
        }
    }
    return type;
}

/* Nested targets are cast on an explicit stack (D71), at most 32 deep
   (D66); deeper ones give LIMIT_EXCEEDED, without using the C stack. */
static void test_nested_targets_are_bounded(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_type_identifier deepest_allowed = nested_choice(&fixture, 32);
    sxml_type_identifier too_deep = nested_choice(&fixture, 33);
    sxml_type_identifier very_deep = nested_choice(&fixture, 10000);
    SXML_TEST_REQUIRE(state, deepest_allowed.index != 0 &&
                                 too_deep.index != 0 && very_deep.index != 0);
    cast_outcome outcome = run_cast(
        &fixture, (cast_case){.source_text = "42",
                              .source_type = SXML_TYPE_NONE,
                              .target_type = deepest_allowed});
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         outcome.result_type.index == SXML_TYPE_XS_INTEGER &&
                         strcmp(outcome.text, "42") == 0);
    EXPECT_ERROR(state, &fixture,
                 ((cast_case){.source_text = "x",
                              .source_type = SXML_TYPE_NONE,
                              .target_type = deepest_allowed}),
                 "FORG0001");
    outcome = run_cast(&fixture,
                       (cast_case){.source_text = "42",
                                   .source_type = SXML_TYPE_NONE,
                                   .target_type = too_deep});
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_LIMIT_EXCEEDED);
    outcome = run_cast(&fixture,
                       (cast_case){.source_text = "42",
                                   .source_type = SXML_TYPE_NONE,
                                   .target_type = very_deep});
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_LIMIT_EXCEEDED);
    /* The first member succeeds before the depth matters. */
    outcome = run_cast(&fixture,
                       (cast_case){.source_text = "true",
                                   .source_type = SXML_TYPE_NONE,
                                   .target_type = very_deep});
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         outcome.result_type.index == SXML_TYPE_XS_BOOLEAN);
    /* An instance of an atomic member is returned unchanged. */
    outcome = run_cast(&fixture,
                       (cast_case){.source_text = "1",
                                   .source_type = SXML_TYPE_XS_BOOLEAN,
                                   .target_type = very_deep});
    SXML_TEST_EXPECT(state,
                     outcome.status == SXML_STATUS_OK &&
                         outcome.result_type.index == SXML_TYPE_XS_BOOLEAN);
    /* The limit is not an answer of castable. */
    SXML_TEST_EXPECT(
        state, castable(&fixture, (cast_case){.source_text = "42",
                                              .source_type = SXML_TYPE_XS_STRING,
                                              .target_type = very_deep}) == -1);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* ------------------------------------------- Operands and target types */

static void test_operands_and_targets_are_checked(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_error_record error_record;
    sxml_sequence result =
        sxml_sequence_from_item(sxml_atomic_item_from_integer(9));
    /* The empty sequence: empty with "?", XPTY0004 without. */
    SXML_TEST_EXPECT(
        state,
        sxml_cast_make_value(
            fixture.item_heap,
            &(sxml_cast_request){.value = sxml_sequence_empty(),
                                 .target_type = builtin(SXML_TYPE_XS_INTEGER),
                                 .allows_empty = true,
                                 .namespace_bindings = {NULL, 0},
                                 .error_record = &error_record},
            &result) == SXML_STATUS_OK &&
            sxml_sequence_get_representation(result) ==
                SXML_SEQUENCE_REPRESENTATION_EMPTY);
    SXML_TEST_EXPECT(
        state,
        sxml_cast_make_value(
            fixture.item_heap,
            &(sxml_cast_request){.value = sxml_sequence_empty(),
                                 .target_type = builtin(SXML_TYPE_XS_INTEGER),
                                 .allows_empty = false,
                                 .namespace_bindings = {NULL, 0},
                                 .error_record = &error_record},
            &result) == SXML_STATUS_TYPE_ERROR);
    /* Two items. */
    sxml_item pair[2] = {sxml_atomic_item_from_integer(1),
                         sxml_atomic_item_from_integer(2)};
    sxml_sequence two = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state,
                      sxml_sequence_make_vector(
                          fixture.item_heap,
                          (sxml_item_view){.items = pair, .item_count = 2},
                          &two) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_cast_make_value(
                                fixture.item_heap,
                                &(sxml_cast_request){
                                    .value = two,
                                    .target_type = builtin(SXML_TYPE_XS_STRING),
                                    .allows_empty = true,
                                    .namespace_bindings = {NULL, 0},
                                    .error_record = &error_record},
                                &result) == SXML_STATUS_TYPE_ERROR);
    sxml_castability castability = {.is_castable = true};
    SXML_TEST_EXPECT(state, sxml_cast_test_castability(
                                fixture.item_heap,
                                &(sxml_cast_request){
                                    .value = two,
                                    .target_type = builtin(SXML_TYPE_XS_STRING),
                                    .allows_empty = true,
                                    .namespace_bindings = {NULL, 0},
                                    .error_record = &error_record},
                                &castability) == SXML_STATUS_OK &&
                                !castability.is_castable);
    sxml_sequence_release(two);
    /* Target types that cannot be cast to: XPST0080. */
    const sxml_builtin_type refused[] = {SXML_TYPE_XS_NOTATION,
                                         SXML_TYPE_XS_ANY_ATOMIC_TYPE,
                                         SXML_TYPE_XS_ANY_SIMPLE_TYPE,
                                         SXML_TYPE_ITEM,
                                         SXML_TYPE_NODE,
                                         SXML_TYPE_MAP};
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(refused);
         case_index += 1) {
        EXPECT_ERROR(state, &fixture, from_text("x", refused[case_index]),
                     "XPST0080");
        SXML_TEST_EXPECT(
            state,
            castable(&fixture,
                     from_item("x", (type_pair){SXML_TYPE_XS_STRING,
                                                refused[case_index]})) == -1);
    }
    /* Text must be well-formed UTF-8. */
    SXML_TEST_EXPECT(state, sxml_cast_make_value_from_text(
                                fixture.item_heap,
                                &(sxml_text_cast_request){
                                    .text = {.bytes = (const uint8_t *)"\xff",
                                             .byte_count = 1},
                                    .target_type = builtin(SXML_TYPE_XS_STRING),
                                    .namespace_bindings = {NULL, 0},
                                    .error_record = NULL},
                                &result) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_canonical_text_reports_its_size(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    uint8_t small[4];
    sxml_canonical_text canonical;
    SXML_TEST_EXPECT(
        state, sxml_cast_write_canonical_text(
                   fixture.item_heap,
                   &(sxml_canonical_text_request){
                       .item = sxml_atomic_item_from_integer(123456),
                       .output = {.bytes = small, .byte_count = sizeof(small)}},
                   &canonical) == SXML_STATUS_CAPACITY_EXCEEDED &&
                   canonical.required_byte_count == 6);
    SXML_TEST_EXPECT(
        state, sxml_cast_write_canonical_text(
                   fixture.item_heap,
                   &(sxml_canonical_text_request){
                       .item = {.representation = 0},
                       .output = {.bytes = small, .byte_count = sizeof(small)}},
                   &canonical) == SXML_STATUS_INVALID_ARGUMENT);
    /* Strings are borrowed, not copied. */
    sxml_sequence value = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state,
                      sxml_cast_make_value_from_text(
                          fixture.item_heap,
                          &(sxml_text_cast_request){
                              .text = text_view("a longer string than four"),
                              .target_type = builtin(SXML_TYPE_XS_STRING),
                              .namespace_bindings = {NULL, 0},
                              .error_record = NULL},
                          &value) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, sxml_cast_write_canonical_text(
                   fixture.item_heap,
                   &(sxml_canonical_text_request){
                       .item = value.content,
                       .output = {.bytes = small, .byte_count = sizeof(small)}},
                   &canonical) == SXML_STATUS_OK &&
                   canonical.text.byte_count == 25);
    sxml_sequence_release(value);
    /* Long binary values go through heap scratch when cast to string. */
    static char hex[2 * SXML_CAST_CANONICAL_TEXT_CAPACITY + 3];
    memset(hex, 'a', sizeof(hex) - 1);
    hex[sizeof(hex) - 1] = 0;
    cast_outcome outcome =
        run_cast(&fixture, from_item(hex, (type_pair){SXML_TYPE_XS_HEX_BINARY,
                                                      SXML_TYPE_XS_STRING}));
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK &&
                                strlen(outcome.text) == sizeof(hex) - 1 &&
                                outcome.text[0] == 'A');
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* ------------------------------------------------- Allocation failures */

/* Runs casts that take each kind of memory; returns the first failure. */
static sxml_status run_allocating_casts(const cast_fixture *fixture) {
    static const struct {
        const char *source;
        sxml_builtin_type type;
    } casts[] = {
        {"  a  b  c ", SXML_TYPE_XS_NMTOKENS},
        {"AQIDBAUG", SXML_TYPE_XS_BASE64_BINARY},
        {" 1.25 ", SXML_TYPE_XS_DECIMAL},
        {"123456789012345678901234567890", SXML_TYPE_XS_INTEGER},
        {"2004-10-13T08:15:30Z", SXML_TYPE_XS_DATE_TIME},
        {"p:name", SXML_TYPE_XS_QNAME},
        {"12", SXML_TYPE_XS_NUMERIC},
    };
    for (size_t cast_index = 0; cast_index < SXML_TEST_COUNT(casts);
         cast_index += 1) {
        sxml_sequence value = sxml_sequence_empty();
        sxml_status status = sxml_cast_make_value_from_text(
            fixture->item_heap,
            &(sxml_text_cast_request){
                .text = text_view(casts[cast_index].source),
                .target_type = builtin(casts[cast_index].type),
                .namespace_bindings = fixture->namespace_bindings,
                .error_record = NULL},
            &value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_sequence string_value = sxml_sequence_empty();
        status = sxml_cast_make_value(
            fixture->item_heap,
            &(sxml_cast_request){.value = value,
                                 .target_type = builtin(SXML_TYPE_XS_STRING),
                                 .allows_empty = true,
                                 .namespace_bindings = {NULL, 0},
                                 .error_record = NULL},
            &string_value);
        /* A list value is more than one item: XPTY0004 is expected. */
        if (status == SXML_STATUS_TYPE_ERROR) {
            status = SXML_STATUS_OK;
        }
        sxml_sequence_release(string_value);
        sxml_sequence_release(value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_item one_tenth = sxml_atomic_item_from_double(0.1);
    sxml_sequence decimal = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value(
        fixture->item_heap,
        &(sxml_cast_request){.value = sxml_sequence_from_item(one_tenth),
                             .target_type = builtin(SXML_TYPE_XS_DECIMAL),
                             .allows_empty = false,
                             .namespace_bindings = {NULL, 0},
                             .error_record = NULL},
        &decimal);
    sxml_sequence_release(decimal);
    return status;
}

static void
test_casts_survive_every_allocation_failure(sxml_test_state *state) {
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
        cast_fixture fixture;
        SXML_TEST_REQUIRE(
            state, cast_fixture_create_with(
                       sxml_counting_allocator_as_allocator(counting_allocator),
                       &fixture) == SXML_STATUS_OK);
        sxml_status status = run_allocating_casts(&fixture);
        sxml_item_heap_counts counts =
            sxml_item_heap_read_counts(fixture.item_heap);
        SXML_TEST_EXPECT(state, counts.live_cell_count == 0);
        SXML_TEST_EXPECT(state, counts.live_byte_count == 0);
        cast_fixture_destroy(&fixture);
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
        SXML_TEST_REQUIRE(state, failing_allocation_number < 500);
    }
    SXML_TEST_EXPECT(state, failing_allocation_number > 10);
}

/* The empty text without bytes (NULL), as an empty string literal gives
   it: FORG0001 for every type whose lexical space lacks "", and no offset
   applied to the null pointer on the way (found by
   tests/fuzz/xpath_evaluate_fuzz.c, packet 1.26, on xs:int("")). */
static void test_empty_text_without_bytes(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    static const sxml_builtin_type targets[] = {
        SXML_TYPE_XS_INT,      SXML_TYPE_XS_DECIMAL, SXML_TYPE_XS_DOUBLE,
        SXML_TYPE_XS_FLOAT,    SXML_TYPE_XS_DATE,    SXML_TYPE_XS_DATE_TIME,
        SXML_TYPE_XS_DURATION, SXML_TYPE_XS_G_YEAR,  SXML_TYPE_XS_BOOLEAN};
    for (size_t target_index = 0;
         target_index < sizeof(targets) / sizeof(targets[0]); target_index++) {
        sxml_error_record error_record = {.value = NULL};
        sxml_sequence value = sxml_sequence_empty();
        SXML_TEST_EXPECT(
            state, sxml_cast_make_value_from_text(
                       fixture.item_heap,
                       &(sxml_text_cast_request){
                           .text = {.bytes = NULL, .byte_count = 0},
                           .target_type = builtin(targets[target_index]),
                           .namespace_bindings = {NULL, 0},
                           .error_record = &error_record},
                       &value) == SXML_STATUS_DYNAMIC_ERROR);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    error_record.code.local_name,
                                    SXML_BYTE_VIEW_FROM_LITERAL("FORG0001")));
    }
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

/* ------------------------------------------- Schema types and facets */

/* Registers a restriction of base with facets (D165), as schema
   compilation does; SXML_TYPE_NONE on failure. */
typedef struct restriction_request {
    sxml_type_variety variety;
    sxml_type_identifier base_type;
    const sxml_facet *facets;
    size_t facet_count;
    /* LIST: the item type; UNION: the members. */
    sxml_type_identifier list_item_type;
    sxml_type_identifier_view member_types;
} restriction_request;

static sxml_type_identifier register_restriction(const cast_fixture *fixture,
                                                 restriction_request request) {
    sxml_type_description description = {
        .variety = request.variety,
        .base_type = request.base_type,
        .facets = {.facets = request.facets, .facet_count = request.facet_count},
    };
    if (request.variety == SXML_TYPE_VARIETY_LIST) {
        description.variety_details.list_item_type = request.list_item_type;
    } else if (request.variety == SXML_TYPE_VARIETY_UNION) {
        description.variety_details.member_types = request.member_types;
    }
    sxml_type_identifier type = {.index = SXML_TYPE_NONE};
    if (sxml_type_registry_register_schema_type(fixture->registry, &description,
                                                &type) != SXML_STATUS_OK) {
        return (sxml_type_identifier){.index = SXML_TYPE_NONE};
    }
    return type;
}

static sxml_type_identifier register_atomic(const cast_fixture *fixture,
                                            sxml_type_identifier base,
                                            sxml_facet facet) {
    return register_restriction(
        fixture, (restriction_request){.variety = SXML_TYPE_VARIETY_ATOMIC,
                                       .base_type = base,
                                       .facets = &facet,
                                       .facet_count = 1});
}

static sxml_facet facet_of(sxml_facet_kind facet_kind, const char *value) {
    return (sxml_facet){.facet_kind = facet_kind, .value = text_view(value)};
}

static cast_case text_to(const char *text, sxml_type_identifier target) {
    return (cast_case){.source_text = text,
                       .source_type = SXML_TYPE_NONE,
                       .target_type = target};
}

/* Whether casting text to target fails with FORG0001 whose description
   starts with constraint_name. */
typedef struct facet_failure {
    const char *text;
    sxml_type_identifier target;
    const char *constraint_name;
} facet_failure;

static bool fails_facet(const cast_fixture *fixture, facet_failure failure) {
    sxml_error_record error_record;
    sxml_sequence result = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value_from_text(
        fixture->item_heap,
        &(sxml_text_cast_request){.text = text_view(failure.text),
                                  .target_type = failure.target,
                                  .namespace_bindings = fixture->namespace_bindings,
                                  .error_record = &error_record},
        &result);
    if (status == SXML_STATUS_OK) {
        sxml_sequence_release(result);
        return false;
    }
    size_t name_length = strlen(failure.constraint_name);
    sxml_byte_view description = error_record.diagnostic.description;
    return status == SXML_STATUS_DYNAMIC_ERROR &&
           sxml_byte_view_equals(error_record.code.local_name, text_view("FORG0001")) &&
           description.byte_count > name_length &&
           memcmp(description.bytes, failure.constraint_name, name_length) == 0;
}

/* The unit test the packet names: "5" to a restriction of xs:integer
   with minInclusive 10 (XSD 1.1 Part 2, 4.3.10.4). */
static void test_facets_of_schema_types_raise_forg0001(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_type_identifier at_least_ten =
        register_atomic(&fixture, builtin(SXML_TYPE_XS_INTEGER),
                        facet_of(SXML_FACET_KIND_MIN_INCLUSIVE, "10"));
    SXML_TEST_REQUIRE(state, at_least_ten.index != SXML_TYPE_NONE);
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"5", at_least_ten,
                                                                  "cvc-minInclusive-valid"}));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to(" 10 ", at_least_ten), "10"}));
    cast_outcome outcome = run_cast(&fixture, text_to("12", at_least_ten));
    SXML_TEST_EXPECT(state, outcome.result_type.index == at_least_ten.index);
    /* From another type than a string: an xs:double source. */
    SXML_TEST_EXPECT(state,
                     fails_with(&fixture, (error_expectation){
                                              {"9.5", SXML_TYPE_XS_DOUBLE, at_least_ten},
                                              "FORG0001"}));
    SXML_TEST_EXPECT(state, castable(&fixture, (cast_case){"11.5", SXML_TYPE_XS_DOUBLE,
                                                           at_least_ten}) == 1);
    SXML_TEST_EXPECT(state, castable(&fixture, (cast_case){"1", SXML_TYPE_XS_DOUBLE,
                                                           at_least_ten}) == 0);
    /* A restriction of the restriction keeps the base's facets. */
    sxml_type_identifier ten_to_twenty = register_atomic(
        &fixture, at_least_ten, facet_of(SXML_FACET_KIND_MAX_EXCLUSIVE, "20"));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"20", ten_to_twenty,
                                                                  "cvc-maxExclusive-valid"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"9", ten_to_twenty,
                                                                  "cvc-minInclusive-valid"}));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("019", ten_to_twenty), "19"}));
    /* The built-in type's own value space still applies. */
    sxml_type_identifier small_byte = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_BYTE), facet_of(SXML_FACET_KIND_MIN_INCLUSIVE, "0"));
    SXML_TEST_EXPECT(state, fails_with(&fixture, (error_expectation){
                                                     text_to("300", small_byte), "FORG0001"}));
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_string_facets(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    /* SKU of the purchase order (XML Schema Part 0, 2.1). */
    sxml_type_identifier sku = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_STRING),
        facet_of(SXML_FACET_KIND_PATTERN, "\\d{3}-[A-Z]{2}"));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("926-AA", sku), "926-AA"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"926-AAA", sku,
                                                                  "cvc-pattern-valid"}));
    /* Two patterns of one step are alternatives; steps are conjunctions. */
    const sxml_facet patterns[] = {facet_of(SXML_FACET_KIND_PATTERN, "a+"),
                                   facet_of(SXML_FACET_KIND_PATTERN, "b+")};
    sxml_type_identifier a_or_b = register_restriction(
        &fixture, (restriction_request){.variety = SXML_TYPE_VARIETY_ATOMIC,
                                        .base_type = builtin(SXML_TYPE_XS_STRING),
                                        .facets = patterns,
                                        .facet_count = 2});
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("bb", a_or_b), "bb"}));
    sxml_type_identifier short_a_or_b = register_atomic(
        &fixture, a_or_b, facet_of(SXML_FACET_KIND_PATTERN, ".{1,2}"));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"aaa", short_a_or_b,
                                                                  "cvc-pattern-valid"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"ab", short_a_or_b,
                                                                  "cvc-pattern-valid"}));
    /* Lengths in characters. */
    sxml_type_identifier three = register_atomic(&fixture, builtin(SXML_TYPE_XS_STRING),
                                                 facet_of(SXML_FACET_KIND_LENGTH, "3"));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("\xC3\xA9t\xC3\xA9", three),
                                                        "\xC3\xA9t\xC3\xA9"}));
    SXML_TEST_EXPECT(state,
                     fails_facet(&fixture, (facet_failure){"ab", three, "cvc-length-valid"}));
    sxml_type_identifier at_most_two = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_TOKEN), facet_of(SXML_FACET_KIND_MAX_LENGTH, "2"));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("  ab  ", at_most_two), "ab"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"abc", at_most_two,
                                                                  "cvc-maxLength-valid"}));
    sxml_type_identifier two_octets = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_HEX_BINARY), facet_of(SXML_FACET_KIND_MIN_LENGTH, "2"));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("0aFF", two_octets), "0AFF"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"0a", two_octets,
                                                                  "cvc-minLength-valid"}));
    /* Enumeration and whiteSpace: a collapsed string. */
    const sxml_facet colours[] = {facet_of(SXML_FACET_KIND_WHITESPACE, "collapse"),
                                  facet_of(SXML_FACET_KIND_ENUMERATION, "red"),
                                  facet_of(SXML_FACET_KIND_ENUMERATION, "dark blue")};
    sxml_type_identifier colour = register_restriction(
        &fixture, (restriction_request){.variety = SXML_TYPE_VARIETY_ATOMIC,
                                        .base_type = builtin(SXML_TYPE_XS_STRING),
                                        .facets = colours,
                                        .facet_count = 3});
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to(" dark \n blue ", colour),
                                                        "dark blue"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"green", colour,
                                                                  "cvc-enumeration-valid"}));
    /* A QName enumeration value is an EQName. */
    sxml_type_identifier qname_choice = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_QNAME),
        facet_of(SXML_FACET_KIND_ENUMERATION, "Q{http://example.com/p}a"));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("p:a", qname_choice), "p:a"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"p:b", qname_choice,
                                                                  "cvc-enumeration-valid"}));
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_number_and_time_facets(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    const sxml_facet money_facets[] = {facet_of(SXML_FACET_KIND_TOTAL_DIGITS, "5"),
                                       facet_of(SXML_FACET_KIND_FRACTION_DIGITS, "2")};
    sxml_type_identifier money = register_restriction(
        &fixture, (restriction_request){.variety = SXML_TYPE_VARIETY_ATOMIC,
                                        .base_type = builtin(SXML_TYPE_XS_DECIMAL),
                                        .facets = money_facets,
                                        .facet_count = 2});
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("00123.4500", money), "123.45"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"1234.56", money,
                                                                  "cvc-totalDigits-valid"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"1.234", money,
                                                                  "cvc-fractionDigits-valid"}));
    /* NaN is incomparable: no bound holds for it (XSD 1.1 Part 2, 3.3.4). */
    sxml_type_identifier small_double = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_DOUBLE), facet_of(SXML_FACET_KIND_MAX_INCLUSIVE, "1.0E2"));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("-INF", small_double), "-INF"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"NaN", small_double,
                                                                  "cvc-maxInclusive-valid"}));
    /* Dates: a value without a timezone is less only if it is so at
       -14:00 and +14:00 (3.3.6.2). */
    sxml_type_identifier before_noon = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_DATE_TIME),
        facet_of(SXML_FACET_KIND_MAX_EXCLUSIVE, "2000-01-01T12:00:00Z"));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("1999-12-31T12:00:00", before_noon),
                                                        "1999-12-31T12:00:00"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"2000-01-01T06:00:00",
                                                                  before_noon,
                                                                  "cvc-maxExclusive-valid"}));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("2000-01-01T13:00:00+02:00",
                                                                before_noon),
                                                        "2000-01-01T13:00:00+02:00"}));
    sxml_type_identifier stamped = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_DATE),
        facet_of(SXML_FACET_KIND_EXPLICIT_TIMEZONE, "required"));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"2000-01-01", stamped,
                                                                  "cvc-explicitTimezone-valid"}));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("2000-01-01Z", stamped),
                                                        "2000-01-01Z"}));
    /* Durations order partially: P1M and P30D are incomparable (3.3.7). */
    sxml_type_identifier under_a_month = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_DURATION), facet_of(SXML_FACET_KIND_MAX_EXCLUSIVE, "P1M"));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("P27D", under_a_month), "P27D"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"P30D", under_a_month,
                                                                  "cvc-maxExclusive-valid"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"P32D", under_a_month,
                                                                  "cvc-maxExclusive-valid"}));
    sxml_type_identifier over_a_month = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_DURATION), facet_of(SXML_FACET_KIND_MIN_INCLUSIVE, "P1M"));
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("P32D", over_a_month), "P32D"}));
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static void test_list_and_union_facets(sxml_test_state *state) {
    cast_fixture fixture;
    SXML_TEST_REQUIRE(state, cast_fixture_create(&fixture) == SXML_STATUS_OK);
    sxml_type_identifier positive = register_atomic(
        &fixture, builtin(SXML_TYPE_XS_INTEGER), facet_of(SXML_FACET_KIND_MIN_INCLUSIVE, "1"));
    /* A list made by xs:list may be empty; its items keep their facets. */
    sxml_type_identifier numbers = register_restriction(
        &fixture, (restriction_request){.variety = SXML_TYPE_VARIETY_LIST,
                                        .base_type = builtin(SXML_TYPE_XS_ANY_SIMPLE_TYPE),
                                        .list_item_type = positive});
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to(" 1  02 3 ", numbers), "1 2 3"}));
    cast_outcome empty = run_cast(&fixture, text_to("  ", numbers));
    SXML_TEST_EXPECT(state, empty.status == SXML_STATUS_OK && empty.item_count == 0);
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"1 0", numbers,
                                                                  "cvc-minInclusive-valid"}));
    /* Its restriction counts items. */
    const sxml_facet pair_facets[] = {facet_of(SXML_FACET_KIND_LENGTH, "2"),
                                      facet_of(SXML_FACET_KIND_ENUMERATION, "1 2"),
                                      facet_of(SXML_FACET_KIND_ENUMERATION, "3 4")};
    sxml_type_identifier pair = register_restriction(
        &fixture, (restriction_request){.variety = SXML_TYPE_VARIETY_LIST,
                                        .base_type = numbers,
                                        .facets = pair_facets,
                                        .facet_count = 3,
                                        .list_item_type = positive});
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("03 4", pair), "3 4"}));
    SXML_TEST_EXPECT(state,
                     fails_facet(&fixture, (facet_failure){"1 2 3", pair, "cvc-length-valid"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"2 1", pair,
                                                                  "cvc-enumeration-valid"}));
    SXML_TEST_EXPECT(state,
                     fails_facet(&fixture, (facet_failure){"", pair, "cvc-length-valid"}));
    /* A union restricted by a pattern on its lexical form. */
    sxml_type_identifier members[] = {builtin(SXML_TYPE_XS_INTEGER),
                                      builtin(SXML_TYPE_XS_DATE)};
    sxml_type_identifier number_or_date = register_restriction(
        &fixture,
        (restriction_request){.variety = SXML_TYPE_VARIETY_UNION,
                              .base_type = builtin(SXML_TYPE_XS_ANY_SIMPLE_TYPE),
                              .member_types = {.type_identifiers = members,
                                               .type_identifier_count = 2}});
    const sxml_facet short_facet = facet_of(SXML_FACET_KIND_PATTERN, "[0-9]{1,3}|.{10}");
    sxml_type_identifier short_number_or_date = register_restriction(
        &fixture,
        (restriction_request){.variety = SXML_TYPE_VARIETY_UNION,
                              .base_type = number_or_date,
                              .facets = &short_facet,
                              .facet_count = 1,
                              .member_types = {.type_identifiers = members,
                                               .type_identifier_count = 2}});
    cast_outcome as_date = run_cast(&fixture, text_to("2000-01-02", short_number_or_date));
    SXML_TEST_EXPECT(state, as_date.status == SXML_STATUS_OK &&
                                as_date.result_type.index == SXML_TYPE_XS_DATE);
    SXML_TEST_EXPECT(state, casts_to_text(&fixture, (text_expectation){
                                                        text_to("007", short_number_or_date),
                                                        "7"}));
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"1234", short_number_or_date,
                                                                  "cvc-pattern-valid"}));
    const sxml_facet one_facet = facet_of(SXML_FACET_KIND_ENUMERATION, "1");
    sxml_type_identifier just_one = register_restriction(
        &fixture,
        (restriction_request){.variety = SXML_TYPE_VARIETY_UNION,
                              .base_type = number_or_date,
                              .facets = &one_facet,
                              .facet_count = 1,
                              .member_types = {.type_identifiers = members,
                                               .type_identifier_count = 2}});
    SXML_TEST_EXPECT(state, castable(&fixture, (cast_case){"1", SXML_TYPE_XS_INTEGER,
                                                           just_one}) == 1);
    SXML_TEST_EXPECT(state, castable(&fixture, (cast_case){"2", SXML_TYPE_XS_INTEGER,
                                                           just_one}) == 0);
    SXML_TEST_EXPECT(state, fails_facet(&fixture, (facet_failure){"2000-01-02", just_one,
                                                                  "cvc-enumeration-valid"}));
    SXML_TEST_EXPECT(state, live_cell_count(&fixture) == 0);
    cast_fixture_destroy(&fixture);
}

static const sxml_test_case cast_test_cases[] = {
    {.name = "facets_of_schema_types_raise_forg0001",
     .function = test_facets_of_schema_types_raise_forg0001},
    {.name = "string_facets", .function = test_string_facets},
    {.name = "number_and_time_facets", .function = test_number_and_time_facets},
    {.name = "list_and_union_facets", .function = test_list_and_union_facets},
    {.name = "slice_cast_lines", .function = test_slice_cast_lines},
    {.name = "duration_cast_to_itself_is_equal",
     .function = test_duration_cast_to_itself_is_equal},
    {.name = "doubles_and_floats_write_xpath_text",
     .function = test_doubles_and_floats_write_xpath_text},
    {.name = "decimals_and_integers", .function = test_decimals_and_integers},
    {.name = "decimal_digit_limit", .function = test_decimal_digit_limit},
    {.name = "date_time_lexical_forms",
     .function = test_date_time_lexical_forms},
    {.name = "date_time_casts_keep_components",
     .function = test_date_time_casts_keep_components},
    {.name = "duration_lexical_forms", .function = test_duration_lexical_forms},
    {.name = "binary_lexical_forms", .function = test_binary_lexical_forms},
    {.name = "strings_follow_their_whitespace_facet",
     .function = test_strings_follow_their_whitespace_facet},
    {.name = "qnames_resolve_their_prefix",
     .function = test_qnames_resolve_their_prefix},
    {.name = "nmtokens_split_into_items",
     .function = test_nmtokens_split_into_items},
    {.name = "unions_try_members_in_order",
     .function = test_unions_try_members_in_order},
    {.name = "enumerations_and_choices",
     .function = test_enumerations_and_choices},
    {.name = "nested_targets_are_bounded",
     .function = test_nested_targets_are_bounded},
    {.name = "operands_and_targets_are_checked",
     .function = test_operands_and_targets_are_checked},
    {.name = "canonical_text_reports_its_size",
     .function = test_canonical_text_reports_its_size},
    {.name = "casts_survive_every_allocation_failure",
     .function = test_casts_survive_every_allocation_failure},
    {.name = "empty_text_without_bytes",
     .function = test_empty_text_without_bytes},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "cast",
            .cases = cast_test_cases,
            .case_count = SXML_TEST_COUNT(cast_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
