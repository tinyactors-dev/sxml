/*
 * Unit tests for src/schema_simple_type.c, the compile stage SIMPLE_TYPES
 * (src/schema_internal.h, D167, D171): simple type definitions built by
 * hand in the components, as the read stage would write them, then
 * checked, registered and cast to. The simple types of the three schemas
 * in text form of src/schema_internal.h come first (the purchase order's
 * quantity and SKU, the message schema's Percent with its fixed facet and
 * assertion); then the constraints of XSD 1.1 Part 1, 3.16.6 and Part 2,
 * 4.1.5 and 4.3, one schema each.
 *
 * Golden file: tests/schema_components_test.c (components built by hand).
 */
#include "cast_internal.h"
#include "processor_internal.h"
#include "schema_internal.h"
#include "sxml/processor.h"
#include "test_runner.h"
#include "xdm_internal.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- Fixture */

/* The constraint names a compilation reported, space-separated. */
typedef struct report_log {
    char names[1024];
    size_t name_count;
} report_log;

static sxml_status log_report(void *state, const sxml_schema_report *report) {
    report_log *log = state;
    size_t used = strlen(log->names);
    size_t byte_count = report->code.local_name.byte_count;
    if (used + byte_count + 2 < sizeof(log->names)) {
        if (used != 0) {
            log->names[used] = ' ';
            used += 1;
        }
        memcpy(log->names + used, report->code.local_name.bytes, byte_count);
        log->names[used + byte_count] = 0;
    }
    log->name_count += 1;
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations log_operations = {.receive = log_report};

/* A processor, components with the built-in ones, and one namespace
   context with xs and p bound. */
typedef struct test_fixture {
    sxml_processor *processor;
    sxml_schema_components *components;
    sxml_schema_namespace_context_index context;
    report_log log;
    sxml_schema_configuration configuration;
    sxml_schema_compilation compilation;
} test_fixture;

static bool fixture_open(test_fixture *fixture) {
    const sxml_allocator allocator = sxml_default_allocator();
    memset(fixture, 0, sizeof(*fixture));
    if (sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                              &fixture->processor) != SXML_STATUS_OK ||
        sxml_schema_components_create(
            allocator, &(sxml_schema_components_configuration){.processor = fixture->processor},
            &fixture->components) != SXML_STATUS_OK ||
        sxml_schema_components_add_builtin_components(fixture->components) != SXML_STATUS_OK) {
        return false;
    }
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(fixture->processor);
    sxml_namespace_binding bindings[2];
    if (sxml_name_pool_intern_namespace_prefix(name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xs"),
                                               &bindings[0].namespace_prefix) != SXML_STATUS_OK ||
        sxml_name_pool_intern_namespace_uri(
            name_pool, SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/2001/XMLSchema"),
            &bindings[0].namespace_uri) != SXML_STATUS_OK ||
        sxml_name_pool_intern_namespace_prefix(name_pool, SXML_BYTE_VIEW_FROM_LITERAL("p"),
                                               &bindings[1].namespace_prefix) != SXML_STATUS_OK ||
        sxml_name_pool_intern_namespace_uri(name_pool,
                                            SXML_BYTE_VIEW_FROM_LITERAL("urn:example:p"),
                                            &bindings[1].namespace_uri) != SXML_STATUS_OK) {
        return false;
    }
    sxml_schema_namespace_context context = {.default_namespace = {.index = 0}};
    if (sxml_schema_components_add_namespace_bindings(
            fixture->components,
            (sxml_namespace_binding_view){.namespace_bindings = bindings,
                                          .namespace_binding_count = 2},
            &context.namespace_bindings) != SXML_STATUS_OK ||
        sxml_schema_components_add_entries(
            fixture->components,
            &(sxml_schema_entry_request){
                .table = SXML_SCHEMA_TABLE_NAMESPACE_CONTEXT, .entries = &context, .entry_count = 1},
            &fixture->context.value) != SXML_STATUS_OK) {
        return false;
    }
    fixture->configuration = (sxml_schema_configuration){
        .processor = fixture->processor,
        .report_sink = {.state = &fixture->log, .operations = &log_operations},
        .error_record = NULL};
    fixture->compilation = (sxml_schema_compilation){.configuration = &fixture->configuration,
                                                     .components = fixture->components,
                                                     .error_count = 0};
    return true;
}

static void fixture_close(test_fixture *fixture) {
    sxml_schema_components_destroy(fixture->components);
    sxml_processor_destroy(fixture->processor);
}

static sxml_status compile(test_fixture *fixture) {
    return sxml_schema_compile_simple_types(&fixture->compilation);
}

static bool reported(const test_fixture *fixture, const char *names) {
    return strcmp(fixture->log.names, names) == 0;
}

/* --------------------------------------------------------------- Builders */

static uint32_t add_entry(test_fixture *fixture, sxml_schema_table table, const void *entry) {
    uint32_t index = 0;
    if (sxml_schema_components_add_entries(
            fixture->components,
            &(sxml_schema_entry_request){.table = table, .entries = entry, .entry_count = 1},
            &index) != SXML_STATUS_OK) {
        return 0;
    }
    return index;
}

static sxml_schema_text add_text(test_fixture *fixture, const char *text) {
    sxml_schema_text added = {.byte_offset = 0, .byte_count = 0};
    if (sxml_schema_components_add_text(fixture->components, sxml_byte_view_from_c_string(text),
                                        &added) != SXML_STATUS_OK) {
        return (sxml_schema_text){.byte_offset = 0, .byte_count = 0};
    }
    return added;
}

/* A list of up to four indexes; 0 ends it. */
typedef struct index_list {
    uint32_t indexes[4];
} index_list;

static sxml_schema_list add_list(test_fixture *fixture, index_list list) {
    size_t count = 0;
    while (count < 4 && list.indexes[count] != 0) {
        count += 1;
    }
    sxml_schema_list added = {.first_entry_index = 0, .entry_count = 0};
    if (sxml_schema_components_add_list(
            fixture->components,
            (sxml_schema_index_view){.indexes = list.indexes, .index_count = count},
            &added) != SXML_STATUS_OK) {
        return (sxml_schema_list){.first_entry_index = 0, .entry_count = 0};
    }
    return added;
}

typedef struct facet_request {
    sxml_facet_kind facet_kind;
    const char *value;
    bool is_fixed;
} facet_request;

static uint32_t add_facet(test_fixture *fixture, facet_request request) {
    sxml_schema_facet facet = {.facet_kind = request.facet_kind,
                               .position = {.line_number = 7, .column_number = 7},
                               .value = add_text(fixture, request.value),
                               .namespace_context = fixture->context,
                               .is_fixed = request.is_fixed};
    return add_entry(fixture, SXML_SCHEMA_TABLE_FACET, &facet);
}

/* A simple type definition as READ and RESOLVE write it. */
typedef struct simple_type_request {
    sxml_schema_derivation derivation_method;
    /* RESTRICTION: the base; LIST: the item type; UNION: unused. */
    uint32_t base_or_item;
    sxml_schema_list facets;
    sxml_schema_list member_types;
    sxml_schema_derivation_set final_derivations;
    sxml_schema_list assertions;
} simple_type_request;

static sxml_schema_type_definition simple_type_of(simple_type_request request) {
    sxml_schema_type_definition definition = {
        .variety = SXML_TYPE_VARIETY_ATOMIC,
        .position = {.document = {.value = 0}, .line_number = 5, .column_number = 3},
        .derivation_method = request.derivation_method,
        .final_derivations = request.final_derivations,
        .facets = request.facets,
        .assertions = request.assertions,
    };
    if (request.derivation_method == SXML_SCHEMA_DERIVATION_LIST) {
        definition.variety = SXML_TYPE_VARIETY_LIST;
        definition.base_type.value = SXML_TYPE_XS_ANY_SIMPLE_TYPE;
        definition.item_type.value = request.base_or_item;
    } else if (request.derivation_method == SXML_SCHEMA_DERIVATION_UNION) {
        definition.variety = SXML_TYPE_VARIETY_UNION;
        definition.base_type.value = SXML_TYPE_XS_ANY_SIMPLE_TYPE;
        definition.member_types = request.member_types;
    } else {
        /* The read stage does not know a restriction's variety; any simple
           one stands for it until SIMPLE_TYPES (D171). */
        definition.variety = SXML_TYPE_VARIETY_ANY_SIMPLE;
        definition.base_type.value = request.base_or_item;
    }
    return definition;
}

static uint32_t add_simple_type(test_fixture *fixture, simple_type_request request) {
    sxml_schema_type_definition definition = simple_type_of(request);
    return add_entry(fixture, SXML_SCHEMA_TABLE_TYPE_DEFINITION, &definition);
}

/* Reserves a type definition, to fill after the types it refers to. */
static uint32_t reserve_type(test_fixture *fixture) {
    sxml_schema_type_definition definition = {.variety = SXML_TYPE_VARIETY_ATOMIC};
    return add_entry(fixture, SXML_SCHEMA_TABLE_TYPE_DEFINITION, &definition);
}

static void define_type(test_fixture *fixture, uint32_t index, simple_type_request request) {
    sxml_schema_type_definition *entry = sxml_schema_components_edit_entry(
        fixture->components,
        (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = index});
    if (entry != NULL) {
        *entry = simple_type_of(request);
    }
}

static const sxml_schema_type_definition *type_of(const test_fixture *fixture, uint32_t index) {
    return sxml_schema_get_type_definition(fixture->components,
                                           (sxml_schema_type_definition_index){.value = index});
}

static sxml_schema_list one_facet(test_fixture *fixture, facet_request request) {
    return add_list(fixture, (index_list){{add_facet(fixture, request)}});
}

/* -------------------------------------------------------------- Casting */

/* Whether text casts to the registered type of definition index; with
   constraint_name, whether it fails with FORG0001 naming it. */
typedef struct cast_expectation {
    uint32_t type_index;
    const char *text;
    const char *constraint_name;
} cast_expectation;

static bool cast_gives(const test_fixture *fixture, cast_expectation expectation) {
    sxml_item_heap *item_heap = sxml_processor_get_item_heap(fixture->processor);
    sxml_error_record error_record;
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value_from_text(
        item_heap,
        &(sxml_text_cast_request){
            .text = sxml_byte_view_from_c_string(expectation.text),
            .target_type = type_of(fixture, expectation.type_index)->registered_type,
            .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
            .error_record = &error_record},
        &value);
    if (status == SXML_STATUS_OK) {
        sxml_sequence_release(value);
        return expectation.constraint_name == NULL;
    }
    if (expectation.constraint_name == NULL || status != SXML_STATUS_DYNAMIC_ERROR) {
        return false;
    }
    size_t name_length = strlen(expectation.constraint_name);
    sxml_byte_view description = error_record.diagnostic.description;
    return description.byte_count >= name_length &&
           memcmp(description.bytes, expectation.constraint_name, name_length) == 0;
}

/* The registered facets of a type, "kind=value" separated by spaces. */
static bool registered_facets_are(const test_fixture *fixture, uint32_t type_index,
                                  const char *expected) {
    static const char *const kind_names[] = {
        "length",       "minLength",    "maxLength",    "pattern",     "enumeration",
        "whiteSpace",   "maxInclusive", "maxExclusive", "minExclusive", "minInclusive",
        "totalDigits",  "fractionDigits", "explicitTimezone"};
    sxml_type_description description;
    if (sxml_type_registry_describe_type(sxml_processor_get_type_registry(fixture->processor),
                                         type_of(fixture, type_index)->registered_type,
                                         &description) != SXML_STATUS_OK) {
        return false;
    }
    char text[256] = {0};
    size_t used = 0;
    for (size_t facet_index = 0; facet_index < description.facets.facet_count; facet_index++) {
        const sxml_facet *facet = &description.facets.facets[facet_index];
        int written = snprintf(text + used, sizeof(text) - used, "%s%s=%.*s",
                               facet_index == 0 ? "" : " ", kind_names[facet->facet_kind],
                               (int)facet->value.byte_count, (const char *)facet->value.bytes);
        if (written < 0 || (size_t)written >= sizeof(text) - used) {
            return false;
        }
        used += (size_t)written;
    }
    return strcmp(text, expected) == 0;
}

/* ------------------------------------------- The schemas in text form */

/* Example 1 (src/schema_internal.h): quantity's anonymous restriction of
   xsd:positiveInteger and SKU; the fixed country attribute. */
static void test_purchase_order_simple_types(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    uint32_t quantity = add_simple_type(
        &fixture, (simple_type_request){
                      .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                      .base_or_item = SXML_TYPE_XS_POSITIVE_INTEGER,
                      .facets = one_facet(&fixture, (facet_request){SXML_FACET_KIND_MAX_EXCLUSIVE,
                                                                    "100", false})});
    uint32_t sku = add_simple_type(
        &fixture,
        (simple_type_request){.derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                              .base_or_item = SXML_TYPE_XS_STRING,
                              .facets = one_facet(&fixture, (facet_request){SXML_FACET_KIND_PATTERN,
                                                                            "\\d{3}-[A-Z]{2}",
                                                                            false})});
    SXML_TEST_EXPECT(state, quantity == 56 && sku == 57);
    sxml_schema_attribute_declaration country = {
        .scope_variety = SXML_SCHEMA_SCOPE_LOCAL,
        .type_definition = {.value = SXML_TYPE_XS_NMTOKEN}};
    uint32_t country_index = add_entry(&fixture, SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION, &country);
    sxml_schema_attribute_use country_use = {
        .use_kind = SXML_SCHEMA_ATTRIBUTE_USE_OPTIONAL,
        .attribute_declaration = {.value = country_index},
        .value_constraint = {.variety = SXML_SCHEMA_VALUE_CONSTRAINT_FIXED,
                             .lexical_form = add_text(&fixture, "US"),
                             .namespace_context = fixture.context}};
    (void)add_entry(&fixture, SXML_SCHEMA_TABLE_ATTRIBUTE_USE, &country_use);
    SXML_TEST_REQUIRE(state, compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, reported(&fixture, ""));
    const sxml_schema_type_definition *quantity_type = type_of(&fixture, quantity);
    SXML_TEST_EXPECT(state, quantity_type->variety == SXML_TYPE_VARIETY_ATOMIC);
    SXML_TEST_EXPECT(state, quantity_type->primitive_type.value == SXML_TYPE_XS_DECIMAL);
    SXML_TEST_EXPECT(state, quantity_type->registered_type.index >= SXML_TYPE_FIRST_REGISTERED);
    SXML_TEST_EXPECT(state, type_of(&fixture, sku)->primitive_type.value == SXML_TYPE_XS_STRING);
    SXML_TEST_EXPECT(state, registered_facets_are(&fixture, quantity, "maxExclusive=100"));
    SXML_TEST_EXPECT(state, registered_facets_are(&fixture, sku, "pattern=\\d{3}-[A-Z]{2}"));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){quantity, " 99 ", NULL}));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){quantity, "100",
                                                                    "cvc-maxExclusive-valid"}));
    SXML_TEST_EXPECT(state, !cast_gives(&fixture, (cast_expectation){quantity, "0", NULL}));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){sku, "926-AA", NULL}));
    SXML_TEST_EXPECT(state,
                     cast_gives(&fixture, (cast_expectation){sku, "926-A", "cvc-pattern-valid"}));
    fixture_close(&fixture);
}

/* Example 3: Percent, minInclusive 0, maxInclusive 100 fixed, and an
   assertion that stays in the components. */
static void test_message_percent(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    sxml_schema_assertion assertion = {.position = {.line_number = 9}};
    uint32_t assertion_index = add_entry(&fixture, SXML_SCHEMA_TABLE_ASSERTION, &assertion);
    uint32_t percent = add_simple_type(
        &fixture,
        (simple_type_request){
            .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
            .base_or_item = SXML_TYPE_XS_DECIMAL,
            .facets = add_list(
                &fixture,
                (index_list){{add_facet(&fixture, (facet_request){SXML_FACET_KIND_MIN_INCLUSIVE,
                                                                  "0", false}),
                              add_facet(&fixture, (facet_request){SXML_FACET_KIND_MAX_INCLUSIVE,
                                                                  "100.0", true})}}),
            .assertions = add_list(&fixture, (index_list){{assertion_index}})});
    /* Restrictions of it: one valid, one changing the fixed facet, one
       above it. */
    uint32_t discount = add_simple_type(
        &fixture, (simple_type_request){
                      .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                      .base_or_item = percent,
                      .facets = one_facet(&fixture, (facet_request){SXML_FACET_KIND_MIN_INCLUSIVE,
                                                                    "10", false})});
    uint32_t lower_maximum = add_simple_type(
        &fixture, (simple_type_request){
                      .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                      .base_or_item = percent,
                      .facets = one_facet(&fixture, (facet_request){SXML_FACET_KIND_MAX_INCLUSIVE,
                                                                    "50", false})});
    uint32_t higher_maximum = add_simple_type(
        &fixture, (simple_type_request){
                      .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                      .base_or_item = discount,
                      .facets = one_facet(&fixture, (facet_request){SXML_FACET_KIND_MAX_EXCLUSIVE,
                                                                    "200", false})});
    SXML_TEST_REQUIRE(state, compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, reported(&fixture, "maxInclusive-valid-restriction "
                                               "maxExclusive-valid-restriction"));
    /* Canonical values in the base type (types.h): 100.0 is 100. */
    SXML_TEST_EXPECT(state,
                     registered_facets_are(&fixture, percent, "minInclusive=0 maxInclusive=100"));
    sxml_schema_index_view assertions =
        sxml_schema_get_list(fixture.components, type_of(&fixture, discount)->assertions);
    SXML_TEST_EXPECT(state, assertions.index_count == 1 && assertions.indexes[0] == assertion_index);
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){discount, "55", NULL}));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){discount, "5",
                                                                    "cvc-minInclusive-valid"}));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){discount, "100.5",
                                                                    "cvc-maxInclusive-valid"}));
    SXML_TEST_EXPECT(state,
                     type_of(&fixture, lower_maximum)->registered_type.index == SXML_TYPE_NONE);
    SXML_TEST_EXPECT(state,
                     type_of(&fixture, higher_maximum)->registered_type.index == SXML_TYPE_NONE);
    fixture_close(&fixture);
}

/* The packet's unit test, through the stage: "5" to a restriction of
   xs:integer with minInclusive 10 is FORG0001, cvc-minInclusive-valid. */
static void test_registered_restriction_casts(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    uint32_t at_least_ten = add_simple_type(
        &fixture, (simple_type_request){
                      .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                      .base_or_item = SXML_TYPE_XS_INTEGER,
                      .facets = one_facet(&fixture, (facet_request){SXML_FACET_KIND_MIN_INCLUSIVE,
                                                                    "+010", false})});
    SXML_TEST_REQUIRE(state, compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, reported(&fixture, ""));
    SXML_TEST_EXPECT(state, registered_facets_are(&fixture, at_least_ten, "minInclusive=10"));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){at_least_ten, "5",
                                                                    "cvc-minInclusive-valid"}));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){at_least_ten, "10", NULL}));
    fixture_close(&fixture);
}

/* -------------------------------------------- Lists, unions and order */

static void test_lists_unions_and_derivation_order(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* Index order is not derivation order: the list (56) and the
       restriction of it (57) come before their item type (58). */
    uint32_t codes = reserve_type(&fixture);
    uint32_t two_codes = reserve_type(&fixture);
    uint32_t code = add_simple_type(
        &fixture, (simple_type_request){
                      .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                      .base_or_item = SXML_TYPE_XS_TOKEN,
                      .facets = one_facet(&fixture, (facet_request){SXML_FACET_KIND_LENGTH, "2",
                                                                    false})});
    define_type(&fixture, codes,
                (simple_type_request){.derivation_method = SXML_SCHEMA_DERIVATION_LIST,
                                      .base_or_item = code});
    define_type(&fixture, two_codes,
                (simple_type_request){
                    .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                    .base_or_item = codes,
                    .facets = add_list(
                        &fixture,
                        (index_list){{add_facet(&fixture, (facet_request){SXML_FACET_KIND_MAX_LENGTH,
                                                                          "2", false}),
                                      add_facet(&fixture,
                                                (facet_request){SXML_FACET_KIND_ENUMERATION,
                                                                " ab  cd ", false})}})});
    /* A union of the code and xs:date, restricted by a pattern. */
    uint32_t code_or_date = add_simple_type(
        &fixture,
        (simple_type_request){.derivation_method = SXML_SCHEMA_DERIVATION_UNION,
                              .member_types = add_list(&fixture, (index_list){{code, SXML_TYPE_XS_DATE}})});
    uint32_t short_code_or_date = add_simple_type(
        &fixture, (simple_type_request){
                      .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                      .base_or_item = code_or_date,
                      .facets = one_facet(&fixture, (facet_request){SXML_FACET_KIND_PATTERN,
                                                                    "[a-z]{2}|2000-.*", false})});
    SXML_TEST_REQUIRE(state, compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, reported(&fixture, ""));
    SXML_TEST_EXPECT(state, type_of(&fixture, codes)->variety == SXML_TYPE_VARIETY_LIST);
    SXML_TEST_EXPECT(state, type_of(&fixture, two_codes)->variety == SXML_TYPE_VARIETY_LIST);
    SXML_TEST_EXPECT(state, type_of(&fixture, two_codes)->item_type.value == code);
    SXML_TEST_EXPECT(state, type_of(&fixture, short_code_or_date)->variety ==
                                SXML_TYPE_VARIETY_UNION);
    /* The enumeration value is canonical in the list type. */
    SXML_TEST_EXPECT(state, registered_facets_are(&fixture, two_codes,
                                                  "maxLength=2 enumeration=ab cd"));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){codes, "", NULL}));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){codes, "ab cd ef", NULL}));
    SXML_TEST_EXPECT(state,
                     cast_gives(&fixture, (cast_expectation){codes, "ab c", "cvc-length-valid"}));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){two_codes, "ab  cd", NULL}));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){two_codes, "cd ab",
                                                                    "cvc-enumeration-valid"}));
    SXML_TEST_EXPECT(state,
                     cast_gives(&fixture, (cast_expectation){short_code_or_date, "2000-01-01", NULL}));
    SXML_TEST_EXPECT(state, cast_gives(&fixture, (cast_expectation){short_code_or_date,
                                                                    "1999-01-01",
                                                                    "cvc-pattern-valid"}));
    fixture_close(&fixture);
}

/* ------------------------------------------------------- Constraints */

/* One schema whose one simple type breaks a constraint. */
typedef struct broken_type {
    sxml_schema_derivation derivation_method;
    uint32_t base_or_item;
    facet_request facets[2];
    const char *constraint_name;
} broken_type;

static bool reports_broken_type(broken_type broken) {
    test_fixture fixture;
    if (!fixture_open(&fixture)) {
        return false;
    }
    index_list facets = {{0}};
    for (size_t facet_index = 0; facet_index < 2 && broken.facets[facet_index].value != NULL;
         facet_index++) {
        facets.indexes[facet_index] = add_facet(&fixture, broken.facets[facet_index]);
    }
    uint32_t type_index = add_simple_type(
        &fixture, (simple_type_request){.derivation_method = broken.derivation_method,
                                        .base_or_item = broken.base_or_item,
                                        .facets = add_list(&fixture, facets)});
    bool is_reported = compile(&fixture) == SXML_STATUS_OK &&
                       reported(&fixture, broken.constraint_name) &&
                       type_of(&fixture, type_index)->registered_type.index == SXML_TYPE_NONE;
    if (!is_reported) {
        printf("    reported \"%s\", expected \"%s\"\n", fixture.log.names,
               broken.constraint_name);
    }
    fixture_close(&fixture);
    return is_reported;
}

static void test_facet_constraints(sxml_test_state *state) {
    const sxml_schema_derivation restriction = SXML_SCHEMA_DERIVATION_RESTRICTION;
    const broken_type cases[] = {
        {restriction, SXML_TYPE_XS_DECIMAL, {{SXML_FACET_KIND_LENGTH, "1", false}},
         "cos-applicable-facets"},
        {restriction, SXML_TYPE_XS_STRING, {{SXML_FACET_KIND_TOTAL_DIGITS, "1", false}},
         "cos-applicable-facets"},
        {restriction, SXML_TYPE_XS_BOOLEAN, {{SXML_FACET_KIND_ENUMERATION, "true", false}},
         "cos-applicable-facets"},
        {restriction,
         SXML_TYPE_XS_STRING,
         {{SXML_FACET_KIND_MAX_LENGTH, "1", false}, {SXML_FACET_KIND_MAX_LENGTH, "2", false}},
         "st-props-correct.4"},
        {restriction, SXML_TYPE_XS_STRING, {{SXML_FACET_KIND_LENGTH, "-1", false}},
         "length-valid-restriction"},
        {restriction, SXML_TYPE_XS_DECIMAL, {{SXML_FACET_KIND_TOTAL_DIGITS, "0", false}},
         "totalDigits-valid-restriction"},
        {restriction, SXML_TYPE_XS_STRING, {{SXML_FACET_KIND_WHITESPACE, "trim", false}},
         "whiteSpace-valid-restriction"},
        {restriction, SXML_TYPE_XS_STRING, {{SXML_FACET_KIND_PATTERN, "(?:a)", false}},
         "cos-st-restricts.1.3.2"},
        {restriction, SXML_TYPE_XS_INT, {{SXML_FACET_KIND_ENUMERATION, "x", false}},
         "enumeration-valid-restriction"},
        {restriction, SXML_TYPE_XS_BYTE, {{SXML_FACET_KIND_MAX_INCLUSIVE, "200", false}},
         "maxInclusive-valid-restriction"},
        /* Facets against built-in ones: whiteSpace of xs:token, the
           fixed fractionDigits 0 of xs:integer, minLength 1 of
           xs:NMTOKENS, the timezone of xs:dateTimeStamp. */
        {restriction, SXML_TYPE_XS_TOKEN, {{SXML_FACET_KIND_WHITESPACE, "replace", false}},
         "whiteSpace-valid-restriction"},
        {restriction, SXML_TYPE_XS_DATE, {{SXML_FACET_KIND_WHITESPACE, "preserve", false}},
         "whiteSpace-valid-restriction"},
        {restriction, SXML_TYPE_XS_INTEGER, {{SXML_FACET_KIND_FRACTION_DIGITS, "1", false}},
         "fractionDigits-valid-restriction"},
        {restriction, SXML_TYPE_XS_NMTOKENS, {{SXML_FACET_KIND_MIN_LENGTH, "0", false}},
         "minLength-valid-restriction"},
        {restriction,
         SXML_TYPE_XS_DATE_TIME_STAMP,
         {{SXML_FACET_KIND_EXPLICIT_TIMEZONE, "optional", false}},
         "timezone-valid-restriction"},
        /* The consistency of one step. */
        {restriction,
         SXML_TYPE_XS_DECIMAL,
         {{SXML_FACET_KIND_MIN_INCLUSIVE, "5", false}, {SXML_FACET_KIND_MAX_INCLUSIVE, "4", false}},
         "minInclusive-less-than-equal-to-maxInclusive"},
        {restriction,
         SXML_TYPE_XS_DECIMAL,
         {{SXML_FACET_KIND_MIN_INCLUSIVE, "5", false}, {SXML_FACET_KIND_MAX_EXCLUSIVE, "5", false}},
         "minInclusive-less-than-maxExclusive"},
        {restriction,
         SXML_TYPE_XS_DECIMAL,
         {{SXML_FACET_KIND_MAX_INCLUSIVE, "5", false}, {SXML_FACET_KIND_MAX_EXCLUSIVE, "6", false}},
         "maxInclusive-maxExclusive"},
        {restriction,
         SXML_TYPE_XS_STRING,
         {{SXML_FACET_KIND_MIN_LENGTH, "3", false}, {SXML_FACET_KIND_MAX_LENGTH, "2", false}},
         "minLength-less-than-equal-to-maxLength"},
        {restriction,
         SXML_TYPE_XS_STRING,
         {{SXML_FACET_KIND_LENGTH, "3", false}, {SXML_FACET_KIND_MIN_LENGTH, "2", false}},
         "length-minLength-maxLength.1"},
        {restriction,
         SXML_TYPE_XS_DECIMAL,
         {{SXML_FACET_KIND_TOTAL_DIGITS, "2", false}, {SXML_FACET_KIND_FRACTION_DIGITS, "3", false}},
         "fractionDigits-totalDigits"},
        /* Variety and base (Part 1, 3.16.6). */
        {restriction, SXML_TYPE_XS_ANY_SIMPLE_TYPE, {{0}}, "cos-st-restricts.1.1"},
        {restriction, SXML_TYPE_XS_ANY_ATOMIC_TYPE, {{0}}, "cos-st-restricts.1.1"},
        {restriction, SXML_TYPE_XS_ANY_TYPE, {{0}}, "cos-st-restricts.1.1"},
        {SXML_SCHEMA_DERIVATION_LIST, SXML_TYPE_XS_NMTOKENS, {{0}}, "cos-st-restricts.2.1"},
        {SXML_SCHEMA_DERIVATION_LIST, SXML_TYPE_XS_ANY_ATOMIC_TYPE, {{0}}, "cos-st-restricts.2.1"},
        {SXML_SCHEMA_DERIVATION_LIST,
         SXML_TYPE_XS_STRING,
         {{SXML_FACET_KIND_LENGTH, "1", false}},
         "cos-st-restricts.2.2.1.2"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index++) {
        bool is_reported = reports_broken_type(cases[case_index]);
        SXML_TEST_EXPECT(state, is_reported);
        if (!is_reported) {
            SXML_TEST_EXPECT(state, cases[case_index].constraint_name == NULL);
        }
    }
}

/* Final, cycles and value constraints. */
static void test_final_cycles_and_value_constraints(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    uint32_t final_type = add_simple_type(
        &fixture, (simple_type_request){.derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                                        .base_or_item = SXML_TYPE_XS_STRING,
                                        .final_derivations = SXML_SCHEMA_DERIVATION_RESTRICTION |
                                                             SXML_SCHEMA_DERIVATION_UNION});
    uint32_t restricted = add_simple_type(
        &fixture, (simple_type_request){.derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                                        .base_or_item = final_type});
    uint32_t joined = add_simple_type(
        &fixture,
        (simple_type_request){.derivation_method = SXML_SCHEMA_DERIVATION_UNION,
                              .member_types = add_list(&fixture, (index_list){{final_type}})});
    /* Two restrictions of each other, and one made from them. */
    uint32_t first = reserve_type(&fixture);
    uint32_t second = add_simple_type(
        &fixture, (simple_type_request){.derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                                        .base_or_item = first});
    define_type(&fixture, first,
                (simple_type_request){.derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
                                      .base_or_item = second});
    uint32_t after_cycle = add_simple_type(
        &fixture, (simple_type_request){.derivation_method = SXML_SCHEMA_DERIVATION_LIST,
                                        .base_or_item = second});
    sxml_schema_attribute_declaration size = {
        .type_definition = {.value = SXML_TYPE_XS_INTEGER},
        .value_constraint = {.variety = SXML_SCHEMA_VALUE_CONSTRAINT_FIXED,
                             .lexical_form = add_text(&fixture, " 12 "),
                             .namespace_context = fixture.context}};
    uint32_t size_index = add_entry(&fixture, SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION, &size);
    sxml_schema_attribute_declaration bad = {
        .type_definition = {.value = SXML_TYPE_XS_DATE},
        .value_constraint = {.variety = SXML_SCHEMA_VALUE_CONSTRAINT_DEFAULT,
                             .lexical_form = add_text(&fixture, "yesterday"),
                             .namespace_context = fixture.context}};
    (void)add_entry(&fixture, SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION, &bad);
    sxml_schema_attribute_use uses[] = {
        {.attribute_declaration = {.value = size_index},
         .value_constraint = {.variety = SXML_SCHEMA_VALUE_CONSTRAINT_FIXED,
                              .lexical_form = add_text(&fixture, "012"),
                              .namespace_context = fixture.context}},
        {.attribute_declaration = {.value = size_index},
         .value_constraint = {.variety = SXML_SCHEMA_VALUE_CONSTRAINT_FIXED,
                              .lexical_form = add_text(&fixture, "13"),
                              .namespace_context = fixture.context}},
        {.attribute_declaration = {.value = size_index},
         .value_constraint = {.variety = SXML_SCHEMA_VALUE_CONSTRAINT_DEFAULT,
                              .lexical_form = add_text(&fixture, "12"),
                              .namespace_context = fixture.context}},
    };
    for (size_t use_index = 0; use_index < SXML_TEST_COUNT(uses); use_index++) {
        (void)add_entry(&fixture, SXML_SCHEMA_TABLE_ATTRIBUTE_USE, &uses[use_index]);
    }
    SXML_TEST_REQUIRE(state, compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, reported(&fixture, "st-props-correct.3 cos-st-restricts.3.2.1.1 "
                                               "st-props-correct.2 a-props-correct.2 "
                                               "au-props-correct.2 au-props-correct.2"));
    SXML_TEST_EXPECT(state, type_of(&fixture, final_type)->registered_type.index != SXML_TYPE_NONE);
    SXML_TEST_EXPECT(state, type_of(&fixture, restricted)->registered_type.index == SXML_TYPE_NONE);
    SXML_TEST_EXPECT(state, type_of(&fixture, joined)->registered_type.index == SXML_TYPE_NONE);
    SXML_TEST_EXPECT(state, type_of(&fixture, after_cycle)->registered_type.index == SXML_TYPE_NONE);
    fixture_close(&fixture);
}

/* With the null sink the first error stops the stage. */
static void test_null_sink_stops_at_the_first_error(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    sxml_error_record error_record;
    fixture.configuration.report_sink = (sxml_schema_report_sink){.state = NULL, .operations = NULL};
    fixture.configuration.error_record = &error_record;
    for (int type_count = 0; type_count < 2; type_count++) {
        (void)add_simple_type(&fixture,
                              (simple_type_request){.derivation_method =
                                                        SXML_SCHEMA_DERIVATION_RESTRICTION,
                                                    .base_or_item = SXML_TYPE_XS_ANY_SIMPLE_TYPE});
    }
    SXML_TEST_EXPECT(state, compile(&fixture) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, fixture.compilation.error_count == 1);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name,
                                                  SXML_BYTE_VIEW_FROM_LITERAL(
                                                      "cos-st-restricts.1.1")));
    fixture_close(&fixture);
}

static const sxml_test_case schema_simple_type_test_cases[] = {
    {.name = "purchase_order_simple_types", .function = test_purchase_order_simple_types},
    {.name = "message_percent", .function = test_message_percent},
    {.name = "registered_restriction_casts", .function = test_registered_restriction_casts},
    {.name = "lists_unions_and_derivation_order",
     .function = test_lists_unions_and_derivation_order},
    {.name = "facet_constraints", .function = test_facet_constraints},
    {.name = "final_cycles_and_value_constraints",
     .function = test_final_cycles_and_value_constraints},
    {.name = "null_sink_stops_at_the_first_error",
     .function = test_null_sink_stops_at_the_first_error},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "schema_simple_type",
            .cases = schema_simple_type_test_cases,
            .case_count = SXML_TEST_COUNT(schema_simple_type_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
