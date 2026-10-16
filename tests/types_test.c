/*
 * Tests for include/sxml/types.h: the type registry, derivation and the
 * subtype relation (docs/DECISIONS.md, D32 and D58).
 *
 * Imitates tests/memory_test.c and tests/name_test.c: lifecycle checks,
 * then behaviour, then allocation-failure and limit loops (D23). The
 * subtype cases are the examples of XPath 4.0, section 3.3, each named
 * after the rule it checks.
 */
#include "sxml/name.h"
#include "sxml/types.h"
#include "test_runner.h"

#include <stdalign.h>

/* Caller memory for a registry placed with initialize. */
#define TEST_STORAGE_ALIGNMENT 16
#define TEST_STORAGE_BYTE_COUNT 8192

typedef struct types_fixture {
    sxml_name_pool *name_pool;
    sxml_type_registry *registry;
} types_fixture;

static sxml_type_registry_configuration default_configuration(
    sxml_name_pool *name_pool) {
    return (sxml_type_registry_configuration){
        .allocator = sxml_default_allocator(),
        .name_pool = name_pool,
        .maximum_registered_type_count = 0,
    };
}

static sxml_name_pool *create_name_pool(void) {
    sxml_name_pool *name_pool = NULL;
    if (sxml_name_pool_create(sxml_default_allocator(),
                              &(sxml_name_pool_configuration){
                                  .allocator = sxml_default_allocator(),
                                  .maximum_byte_count = 0},
                              &name_pool) != SXML_STATUS_OK) {
        return NULL;
    }
    return name_pool;
}

/* A name pool and a registry with the default allocator. */
static bool types_fixture_create(types_fixture *fixture) {
    fixture->name_pool = create_name_pool();
    fixture->registry = NULL;
    if (fixture->name_pool == NULL) {
        return false;
    }
    sxml_type_registry_configuration configuration =
        default_configuration(fixture->name_pool);
    return sxml_type_registry_create(sxml_default_allocator(), &configuration,
                                     &fixture->registry) == SXML_STATUS_OK;
}

static void types_fixture_destroy(types_fixture *fixture) {
    sxml_type_registry_destroy(fixture->registry);
    sxml_name_pool_destroy(fixture->name_pool);
}

/* ------------------------------------------------------------ Builders */

static sxml_type_identifier builtin(sxml_builtin_type builtin_type) {
    return sxml_builtin_type_identifier(builtin_type);
}

static const sxml_type_identifier no_type = {.index = SXML_TYPE_NONE};

static sxml_sequence_type sequence(sxml_type_identifier item_type,
                                   sxml_occurrence occurrence) {
    return (sxml_sequence_type){.item_type = item_type,
                                .occurrence = occurrence};
}

static sxml_sequence_type exactly_one(sxml_type_identifier item_type) {
    return sequence(item_type, SXML_OCCURRENCE_EXACTLY_ONE);
}

static sxml_sequence_type any_sequence(void) {
    return sequence(builtin(SXML_TYPE_ITEM), SXML_OCCURRENCE_ZERO_OR_MORE);
}

static sxml_type_description item_type_description(sxml_type_variety variety) {
    return (sxml_type_description){
        .variety = variety,
        .type_name = {.index = 0},
        .base_type = {.index = 0},
        .primitive_type = {.index = 0},
        .is_builtin = false,
        .variety_details = {.list_item_type = {.index = 0}},
    };
}

/* Registers description; SXML_TYPE_NONE on failure. */
static sxml_type_identifier register_type(
    types_fixture *fixture, const sxml_type_description *description) {
    sxml_type_identifier type = {.index = 0};
    if (sxml_type_registry_register_item_type(fixture->registry, description,
                                              &type) != SXML_STATUS_OK) {
        return no_type;
    }
    return type;
}

typedef struct identifier_list {
    const sxml_type_identifier *identifiers;
    size_t identifier_count;
} identifier_list;

static sxml_type_identifier choice_of(types_fixture *fixture,
                                      identifier_list members) {
    sxml_type_description description =
        item_type_description(SXML_TYPE_VARIETY_CHOICE);
    description.variety_details.member_types = (sxml_type_identifier_view){
        .type_identifiers = members.identifiers,
        .type_identifier_count = members.identifier_count};
    return register_type(fixture, &description);
}

#define LIST_OF(array)                                                         \
    ((identifier_list){.identifiers = (array),                                 \
                       .identifier_count = sizeof(array) / sizeof((array)[0])})

static sxml_type_identifier enumeration_of(types_fixture *fixture,
                                           sxml_byte_view_list values) {
    sxml_type_description description =
        item_type_description(SXML_TYPE_VARIETY_ENUMERATION);
    description.variety_details.enumeration_values = values;
    return register_type(fixture, &description);
}

#define VALUES_OF(array)                                                       \
    ((sxml_byte_view_list){.byte_views = (array),                              \
                           .byte_view_count =                                  \
                               sizeof(array) / sizeof((array)[0])})

static sxml_type_identifier array_of(types_fixture *fixture,
                                     sxml_sequence_type member_type) {
    sxml_type_description description =
        item_type_description(SXML_TYPE_VARIETY_ARRAY);
    description.variety_details.array_type =
        (sxml_array_type){.is_any_array = false, .member_type = member_type};
    return register_type(fixture, &description);
}

typedef struct map_signature {
    sxml_type_identifier key_type;
    sxml_sequence_type value_type;
} map_signature;

static sxml_type_identifier map_of(types_fixture *fixture,
                                   map_signature signature) {
    sxml_type_description description =
        item_type_description(SXML_TYPE_VARIETY_MAP);
    description.variety_details.map_type =
        (sxml_map_type){.is_any_map = false,
                        .key_type = signature.key_type,
                        .value_type = signature.value_type};
    return register_type(fixture, &description);
}

typedef struct function_signature {
    const sxml_sequence_type *parameter_types;
    size_t parameter_count;
    sxml_sequence_type result_type;
} function_signature;

static sxml_type_identifier function_of(types_fixture *fixture,
                                        function_signature signature) {
    sxml_type_description description =
        item_type_description(SXML_TYPE_VARIETY_FUNCTION);
    description.variety_details.function_type = (sxml_function_type){
        .is_any_function = false,
        .parameter_types = {.sequence_types = signature.parameter_types,
                            .sequence_type_count = signature.parameter_count},
        .result_type = signature.result_type};
    return register_type(fixture, &description);
}

/* function(parameter) as result. */
typedef struct unary_signature {
    sxml_sequence_type parameter_type;
    sxml_sequence_type result_type;
} unary_signature;

static sxml_type_identifier unary_function_of(types_fixture *fixture,
                                              unary_signature signature) {
    return function_of(fixture,
                       (function_signature){.parameter_types =
                                                &signature.parameter_type,
                                            .parameter_count = 1,
                                            .result_type = signature.result_type});
}

static sxml_type_identifier record_of(types_fixture *fixture,
                                      sxml_record_field_view fields) {
    sxml_type_description description =
        item_type_description(SXML_TYPE_VARIETY_RECORD);
    description.variety_details.record_type =
        (sxml_record_type){.is_any_record = false,
                           .is_nominative = false,
                           .record_fields = fields};
    return register_type(fixture, &description);
}

#define FIELDS_OF(array)                                                       \
    ((sxml_record_field_view){.record_fields = (array),                        \
                              .record_field_count =                            \
                                  sizeof(array) / sizeof((array)[0])})

static sxml_name_identifier intern_name(types_fixture *fixture,
                                        sxml_name_parts parts) {
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_name(fixture->name_pool,
                                   &(sxml_name_request){
                                       .namespace_uri = parts.namespace_uri,
                                       .local_name = parts.local_name},
                                   &name) != SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return name;
}

static sxml_name_identifier local_name(types_fixture *fixture,
                                       sxml_byte_view text) {
    return intern_name(fixture,
                       (sxml_name_parts){.namespace_uri = {.index = 0},
                                         .local_name = text});
}

static sxml_name_test any_name_test(void) {
    return (sxml_name_test){.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME,
                            .expanded_name = {.index = 0},
                            .namespace_uri = {.index = 0},
                            .local_name = {.bytes = NULL, .byte_count = 0}};
}

static sxml_name_test exact_name_test(sxml_name_identifier name) {
    sxml_name_test name_test = any_name_test();
    name_test.name_test_kind = SXML_NAME_TEST_KIND_EXPANDED_NAME;
    name_test.expanded_name = name;
    return name_test;
}

static sxml_node_test node_test_of_kind(sxml_node_kind node_kind) {
    return (sxml_node_test){.node_kind = node_kind,
                            .name_test = any_name_test(),
                            .type_annotation = {.index = 0},
                            .allows_nilled = false,
                            .is_schema_declaration = false,
                            .document_element_test = {.index = 0}};
}

static sxml_type_identifier node_test_type(types_fixture *fixture,
                                           sxml_node_test node_test) {
    sxml_type_description description =
        item_type_description(SXML_TYPE_VARIETY_NODE_TEST);
    description.variety_details.node_test = node_test;
    return register_type(fixture, &description);
}

/* element(name, type?) or attribute(name, type). */
typedef struct named_node_test {
    sxml_node_kind node_kind;
    sxml_name_test name_test;
    sxml_type_identifier type_annotation;
    bool allows_nilled;
} named_node_test;

static sxml_type_identifier named_test(types_fixture *fixture,
                                       named_node_test test) {
    sxml_node_test node_test = node_test_of_kind(test.node_kind);
    node_test.name_test = test.name_test;
    node_test.type_annotation = test.type_annotation;
    node_test.allows_nilled = test.allows_nilled;
    return node_test_type(fixture, node_test);
}

/* element(local) without a type. */
static sxml_type_identifier element_named(types_fixture *fixture,
                                          sxml_byte_view name) {
    return named_test(fixture,
                      (named_node_test){
                          .node_kind = SXML_NODE_KIND_ELEMENT,
                          .name_test = exact_name_test(local_name(fixture, name)),
                          .type_annotation = no_type,
                          .allows_nilled = false});
}

static sxml_type_identifier document_of(types_fixture *fixture,
                                        sxml_type_identifier element_test) {
    sxml_node_test node_test = node_test_of_kind(SXML_NODE_KIND_DOCUMENT);
    node_test.document_element_test = element_test;
    return node_test_type(fixture, node_test);
}

static bool is_subtype(const types_fixture *fixture,
                       sxml_type_identifier derived_type,
                       sxml_type_identifier base_type) {
    return sxml_type_registry_is_subtype(
        fixture->registry,
        (sxml_type_pair){.derived_type = derived_type, .base_type = base_type});
}

static bool is_derived(const types_fixture *fixture,
                       sxml_builtin_type derived_type,
                       sxml_builtin_type base_type) {
    return sxml_type_registry_is_derived_from(
        fixture->registry, (sxml_type_pair){.derived_type = builtin(derived_type),
                                            .base_type = builtin(base_type)});
}

static bool is_sequence_subtype(const types_fixture *fixture,
                                sxml_sequence_type derived_type,
                                sxml_sequence_type base_type) {
    return sxml_type_registry_is_sequence_subtype(
        fixture->registry,
        (sxml_sequence_type_pair){.derived_type = derived_type,
                                  .base_type = base_type});
}

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ------------------------------------------------------------- Lifecycle */

static void test_memory_requirements_need_a_name_pool(sxml_test_state *state) {
    sxml_memory_requirements requirements = {.byte_count = 7, .alignment = 7};
    sxml_type_registry_configuration configuration = default_configuration(NULL);
    SXML_TEST_EXPECT(state, sxml_type_registry_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, requirements.byte_count == 7);
    sxml_name_pool *name_pool = create_name_pool();
    SXML_TEST_REQUIRE(state, name_pool != NULL);
    configuration = default_configuration(name_pool);
    SXML_TEST_EXPECT(state, sxml_type_registry_memory_requirements(
                                &configuration, &requirements) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, requirements.byte_count != 0);
    SXML_TEST_EXPECT(state, requirements.byte_count <= TEST_STORAGE_BYTE_COUNT);
    SXML_TEST_EXPECT(state, requirements.alignment <= TEST_STORAGE_ALIGNMENT);
    sxml_name_pool_destroy(name_pool);
}

static void test_initialize_checks_memory(sxml_test_state *state) {
    alignas(TEST_STORAGE_ALIGNMENT) static uint8_t storage[TEST_STORAGE_BYTE_COUNT];
    sxml_name_pool *name_pool = create_name_pool();
    SXML_TEST_REQUIRE(state, name_pool != NULL);
    sxml_type_registry_configuration configuration =
        default_configuration(name_pool);
    sxml_type_registry *registry = NULL;
    SXML_TEST_EXPECT(state, sxml_type_registry_initialize(
                                (sxml_memory_view){.bytes = storage,
                                                   .byte_count = 8},
                                &configuration,
                                &registry) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, sxml_type_registry_initialize(
                                (sxml_memory_view){.bytes = storage + 1,
                                                   .byte_count =
                                                       sizeof(storage) - 1},
                                &configuration,
                                &registry) == SXML_STATUS_MISALIGNED_MEMORY);
    SXML_TEST_EXPECT(state, registry == NULL);
    SXML_TEST_EXPECT(state, sxml_type_registry_initialize(
                                (sxml_memory_view){.bytes = storage,
                                                   .byte_count = sizeof(storage)},
                                &configuration,
                                &registry) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, registry != NULL);
    sxml_type_registry_finalize(registry);
    sxml_name_pool_destroy(name_pool);
}

static void test_finalize_and_destroy_accept_null(sxml_test_state *state) {
    sxml_type_registry_finalize(NULL);
    sxml_type_registry_destroy(NULL);
    SXML_TEST_EXPECT(state, true);
}

/* --------------------------------------------------------- Built-in types */

static void test_builtin_types_are_described(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_type_description description = item_type_description(
        SXML_TYPE_VARIETY_ANY_ITEM);
    for (uint32_t builtin_index = 1; builtin_index < SXML_TYPE_BUILTIN_COUNT;
         builtin_index += 1) {
        SXML_TEST_EXPECT(state, sxml_type_registry_describe_type(
                                    fixture.registry,
                                    (sxml_type_identifier){.index = builtin_index},
                                    &description) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, description.is_builtin);
        /* Every named built-in type is found by its name. */
        if (description.type_name.index != 0) {
            sxml_type_identifier found = {.index = 0};
            SXML_TEST_EXPECT(state, sxml_type_registry_find_type(
                                        fixture.registry, description.type_name,
                                        &found));
            SXML_TEST_EXPECT(state, found.index == builtin_index);
        }
    }
    SXML_TEST_EXPECT(state, sxml_type_registry_describe_type(
                                fixture.registry, no_type, &description) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_type_registry_describe_type(
                                fixture.registry,
                                builtin(SXML_TYPE_BUILTIN_COUNT),
                                &description) == SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state, sxml_type_registry_describe_type(
                                fixture.registry,
                                builtin(SXML_TYPE_FIRST_REGISTERED),
                                &description) == SXML_STATUS_INDEX_OUT_OF_RANGE);

    SXML_TEST_REQUIRE(state, sxml_type_registry_describe_type(
                                 fixture.registry, builtin(SXML_TYPE_XS_SHORT),
                                 &description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.variety == SXML_TYPE_VARIETY_ATOMIC);
    SXML_TEST_EXPECT(state, description.base_type.index == SXML_TYPE_XS_INT);
    SXML_TEST_EXPECT(state,
                     description.primitive_type.index == SXML_TYPE_XS_DECIMAL);
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    SXML_TEST_EXPECT(state, sxml_name_pool_get_name_parts(
                                fixture.name_pool, description.type_name,
                                &parts) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, parts.namespace_uri.index ==
                                SXML_NAMESPACE_XML_SCHEMA);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(parts.local_name,
                                                  TEXT("short")));

    SXML_TEST_REQUIRE(state, sxml_type_registry_describe_type(
                                 fixture.registry, builtin(SXML_TYPE_XS_NUMERIC),
                                 &description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.variety == SXML_TYPE_VARIETY_UNION);
    SXML_TEST_EXPECT(state, description.variety_details.member_types
                                    .type_identifier_count == 3);
    SXML_TEST_REQUIRE(state, sxml_type_registry_describe_type(
                                 fixture.registry, builtin(SXML_TYPE_MAP),
                                 &description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.type_name.index == 0);
    SXML_TEST_EXPECT(state, description.variety_details.map_type.is_any_map);
    types_fixture_destroy(&fixture);
}

static void test_find_type_by_name(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_name_identifier name = intern_name(
        &fixture,
        (sxml_name_parts){.namespace_uri = sxml_well_known_namespace_identifier(
                              SXML_NAMESPACE_XML_SCHEMA),
                          .local_name = TEXT("dateTimeStamp")});
    sxml_type_identifier type = {.index = 0};
    SXML_TEST_EXPECT(state,
                     sxml_type_registry_find_type(fixture.registry, name, &type));
    SXML_TEST_EXPECT(state, type.index == SXML_TYPE_XS_DATE_TIME_STAMP);
    /* The same local name in no namespace names no type. */
    type = builtin(SXML_TYPE_ITEM);
    SXML_TEST_EXPECT(state, !sxml_type_registry_find_type(
                                fixture.registry,
                                local_name(&fixture, TEXT("dateTimeStamp")),
                                &type));
    SXML_TEST_EXPECT(state, !sxml_type_registry_find_type(
                                fixture.registry,
                                (sxml_name_identifier){.index = 0}, &type));
    SXML_TEST_EXPECT(state, type.index == SXML_TYPE_ITEM);
    types_fixture_destroy(&fixture);
}

/* ------------------------------------------------------------- Derivation */

static void test_derivation_follows_base_types_and_unions(
    sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    SXML_TEST_EXPECT(state, is_derived(&fixture, SXML_TYPE_XS_SHORT,
                                       SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(state, is_derived(&fixture, SXML_TYPE_XS_BYTE,
                                       SXML_TYPE_XS_ANY_TYPE));
    SXML_TEST_EXPECT(state, is_derived(&fixture, SXML_TYPE_XS_ID,
                                       SXML_TYPE_XS_STRING));
    SXML_TEST_EXPECT(state, is_derived(&fixture, SXML_TYPE_XS_UNTYPED,
                                       SXML_TYPE_XS_ANY_TYPE));
    SXML_TEST_EXPECT(state, is_derived(&fixture, SXML_TYPE_XS_NMTOKENS,
                                       SXML_TYPE_XS_ANY_SIMPLE_TYPE));
    /* A union's transitive members derive from it. */
    SXML_TEST_EXPECT(state, is_derived(&fixture, SXML_TYPE_XS_DECIMAL,
                                       SXML_TYPE_XS_NUMERIC));
    SXML_TEST_EXPECT(state, is_derived(&fixture, SXML_TYPE_XS_UNSIGNED_BYTE,
                                       SXML_TYPE_XS_NUMERIC));
    SXML_TEST_EXPECT(state, is_derived(&fixture, SXML_TYPE_XS_DAY_TIME_DURATION,
                                       SXML_TYPE_XS_DAY_TIME_DURATION));
    SXML_TEST_EXPECT(state, !is_derived(&fixture, SXML_TYPE_XS_INTEGER,
                                        SXML_TYPE_XS_SHORT));
    SXML_TEST_EXPECT(state, !is_derived(&fixture, SXML_TYPE_XS_STRING,
                                        SXML_TYPE_XS_NUMERIC));
    SXML_TEST_EXPECT(state, !is_derived(&fixture, SXML_TYPE_XS_NUMERIC,
                                        SXML_TYPE_XS_DECIMAL));
    SXML_TEST_EXPECT(state, !is_derived(&fixture, SXML_TYPE_XS_INTEGER,
                                        SXML_TYPE_NONE));
    SXML_TEST_EXPECT(state, !sxml_type_registry_is_derived_from(
                                fixture.registry,
                                (sxml_type_pair){
                                    .derived_type = {.index = 600},
                                    .base_type = builtin(SXML_TYPE_XS_ANY_TYPE)}));
    types_fixture_destroy(&fixture);
}

/* ------------------------------------------- Subtyping: atomic, choice */

static void test_general_and_atomic_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    /* 3.3.2.1 */
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_XS_ERROR),
                                       builtin(SXML_TYPE_ELEMENT_NODE)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_ARRAY),
                                       builtin(SXML_TYPE_ITEM)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_XS_DATE),
                                       builtin(SXML_TYPE_XS_DATE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_ITEM),
                                        builtin(SXML_TYPE_XS_ERROR)));
    /* 3.3.2.3 */
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_XS_INTEGER),
                                       builtin(SXML_TYPE_XS_DECIMAL)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_XS_DECIMAL),
                                       builtin(SXML_TYPE_XS_NUMERIC)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_XS_NUMERIC),
                                       builtin(SXML_TYPE_XS_ANY_ATOMIC_TYPE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_XS_NUMERIC),
                                        builtin(SXML_TYPE_XS_DECIMAL)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_XS_DECIMAL),
                                        builtin(SXML_TYPE_XS_INTEGER)));
    /* Schema types that are not item types relate only to themselves. */
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_XS_UNTYPED),
                                        builtin(SXML_TYPE_XS_ANY_TYPE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_XS_STRING),
                                        builtin(SXML_TYPE_ELEMENT_NODE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, (sxml_type_identifier){.index = 99},
                                        builtin(SXML_TYPE_ITEM)));
    types_fixture_destroy(&fixture);
}

static void test_choice_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    /* (xs:int | xs:long) ⊆ (xs:decimal | xs:date) */
    const sxml_type_identifier int_or_long[] = {builtin(SXML_TYPE_XS_INT),
                                                builtin(SXML_TYPE_XS_LONG)};
    const sxml_type_identifier decimal_or_date[] = {
        builtin(SXML_TYPE_XS_DECIMAL), builtin(SXML_TYPE_XS_DATE)};
    sxml_type_identifier derived = choice_of(&fixture, LIST_OF(int_or_long));
    sxml_type_identifier base = choice_of(&fixture, LIST_OF(decimal_or_date));
    SXML_TEST_REQUIRE(state, derived.index >= SXML_TYPE_FIRST_REGISTERED);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, derived, base));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, base, derived));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, derived,
                                       builtin(SXML_TYPE_XS_INTEGER)));
    /* xs:int is not a subtype of (xs:negativeInteger |
       xs:nonNegativeInteger). */
    const sxml_type_identifier split_integers[] = {
        builtin(SXML_TYPE_XS_NEGATIVE_INTEGER),
        builtin(SXML_TYPE_XS_NON_NEGATIVE_INTEGER)};
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_XS_INT),
                                        choice_of(&fixture,
                                                  LIST_OF(split_integers))));
    /* xnode() is the choice of the seven node kinds. */
    const sxml_type_identifier node_kinds[] = {
        builtin(SXML_TYPE_ELEMENT_NODE),
        builtin(SXML_TYPE_ATTRIBUTE_NODE),
        builtin(SXML_TYPE_TEXT_NODE),
        builtin(SXML_TYPE_COMMENT_NODE),
        builtin(SXML_TYPE_PROCESSING_INSTRUCTION_NODE),
        builtin(SXML_TYPE_DOCUMENT_NODE),
        builtin(SXML_TYPE_NAMESPACE_NODE)};
    sxml_type_identifier all_kinds = choice_of(&fixture, LIST_OF(node_kinds));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_NODE),
                                       all_kinds));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_GENERIC_NODE),
                                        all_kinds));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, all_kinds,
                                       builtin(SXML_TYPE_NODE)));
    /* item() is (node() | xs:anyAtomicType | function(*)). */
    const sxml_type_identifier item_parts[] = {
        builtin(SXML_TYPE_FUNCTION), builtin(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
        builtin(SXML_TYPE_GENERIC_NODE)};
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_ITEM),
                                       choice_of(&fixture, LIST_OF(item_parts))));
    /* xs:anyAtomicType is the choice of the 20 primitive types. */
    sxml_type_identifier primitives[20];
    for (uint32_t primitive_index = 0; primitive_index < 20;
         primitive_index += 1) {
        primitives[primitive_index] = (sxml_type_identifier){
            .index = SXML_TYPE_XS_UNTYPED_ATOMIC + primitive_index};
    }
    sxml_type_identifier all_primitives =
        choice_of(&fixture, LIST_OF(primitives));
    SXML_TEST_EXPECT(state, is_subtype(&fixture,
                                       builtin(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
                                       all_primitives));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture,
                                        builtin(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
                                        choice_of(&fixture,
                                                  (identifier_list){
                                                      .identifiers = primitives,
                                                      .identifier_count = 19})));
    /* A union against a choice of its members (3.3.2.3). */
    const sxml_type_identifier numerics[] = {builtin(SXML_TYPE_XS_DOUBLE),
                                             builtin(SXML_TYPE_XS_FLOAT),
                                             builtin(SXML_TYPE_XS_DECIMAL)};
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_XS_NUMERIC),
                                       choice_of(&fixture, LIST_OF(numerics))));
    types_fixture_destroy(&fixture);
}

static void test_enumeration_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    const sxml_byte_view red_green[] = {TEXT("red"), TEXT("green")};
    const sxml_byte_view red_green_blue[] = {TEXT("red"), TEXT("green"),
                                             TEXT("blue")};
    const sxml_byte_view blue[] = {TEXT("blue")};
    sxml_type_identifier two = enumeration_of(&fixture, VALUES_OF(red_green));
    sxml_type_identifier three =
        enumeration_of(&fixture, VALUES_OF(red_green_blue));
    sxml_type_identifier only_blue = enumeration_of(&fixture, VALUES_OF(blue));
    SXML_TEST_REQUIRE(state, two.index != 0 && three.index != 0 &&
                                 only_blue.index != 0);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, two, three));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, three, two));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, two,
                                       builtin(SXML_TYPE_XS_STRING)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, two,
                                       builtin(SXML_TYPE_XS_ANY_ATOMIC_TYPE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, two,
                                        builtin(SXML_TYPE_XS_NCNAME)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_XS_STRING),
                                        two));
    const sxml_type_identifier split[] = {two, only_blue};
    SXML_TEST_EXPECT(state, is_subtype(&fixture, three,
                                       choice_of(&fixture, LIST_OF(split))));
    /* Values are copied and interned: the same list gives the same type. */
    const sxml_byte_view red_green_again[] = {TEXT("red"), TEXT("green")};
    SXML_TEST_EXPECT(state, enumeration_of(&fixture, VALUES_OF(red_green_again))
                                    .index == two.index);
    const sxml_byte_view duplicated[] = {TEXT("red"), TEXT("red")};
    SXML_TEST_EXPECT(state,
                     enumeration_of(&fixture, VALUES_OF(duplicated)).index == 0);
    types_fixture_destroy(&fixture);
}

/* --------------------------------------------------- Subtyping: nodes */

static void test_node_kind_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_COMMENT_NODE),
                                       builtin(SXML_TYPE_NODE)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_NODE),
                                       builtin(SXML_TYPE_GENERIC_NODE)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_JSON_NODE),
                                       builtin(SXML_TYPE_GENERIC_NODE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_JSON_NODE),
                                        builtin(SXML_TYPE_NODE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_GENERIC_NODE),
                                        builtin(SXML_TYPE_NODE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_TEXT_NODE),
                                        builtin(SXML_TYPE_COMMENT_NODE)));
    sxml_node_test pi_test =
        node_test_of_kind(SXML_NODE_KIND_PROCESSING_INSTRUCTION);
    pi_test.name_test = exact_name_test(local_name(&fixture, TEXT("pi")));
    sxml_type_identifier named_pi = node_test_type(&fixture, pi_test);
    SXML_TEST_REQUIRE(state, named_pi.index >= SXML_TYPE_FIRST_REGISTERED);
    SXML_TEST_EXPECT(state, is_subtype(
                                &fixture, named_pi,
                                builtin(SXML_TYPE_PROCESSING_INSTRUCTION_NODE)));
    SXML_TEST_EXPECT(state, !is_subtype(
                                &fixture,
                                builtin(SXML_TYPE_PROCESSING_INSTRUCTION_NODE),
                                named_pi));
    types_fixture_destroy(&fixture);
}

static void test_element_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_name_identifier title = local_name(&fixture, TEXT("title"));
    sxml_type_identifier element_title = element_named(&fixture, TEXT("title"));
    sxml_type_identifier element_heading =
        element_named(&fixture, TEXT("heading"));
    sxml_type_identifier title_as_string = named_test(
        &fixture, (named_node_test){.node_kind = SXML_NODE_KIND_ELEMENT,
                                    .name_test = exact_name_test(title),
                                    .type_annotation =
                                        builtin(SXML_TYPE_XS_STRING),
                                    .allows_nilled = false});
    sxml_type_identifier title_as_any_type_nillable = named_test(
        &fixture, (named_node_test){.node_kind = SXML_NODE_KIND_ELEMENT,
                                    .name_test = exact_name_test(title),
                                    .type_annotation =
                                        builtin(SXML_TYPE_XS_ANY_TYPE),
                                    .allows_nilled = true});
    const sxml_type_identifier title_or_heading_members[] = {element_title,
                                                             element_heading};
    sxml_type_identifier title_or_heading =
        choice_of(&fixture, LIST_OF(title_or_heading_members));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, element_title,
                                       builtin(SXML_TYPE_ELEMENT_NODE)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, title_or_heading,
                                       builtin(SXML_TYPE_ELEMENT_NODE)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, title_as_string,
                                       title_or_heading));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, element_title,
                                       title_as_any_type_nillable));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, element_title,
                                        title_as_string));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, element_heading,
                                        element_title));
    /* element(size, xs:integer) ⊆ element(size, xs:decimal), but
       element(size, xs:integer?) is not: it matches nilled elements. */
    sxml_name_identifier size = local_name(&fixture, TEXT("size"));
    named_node_test size_test = {.node_kind = SXML_NODE_KIND_ELEMENT,
                                 .name_test = exact_name_test(size),
                                 .type_annotation = builtin(SXML_TYPE_XS_INTEGER),
                                 .allows_nilled = false};
    sxml_type_identifier size_integer = named_test(&fixture, size_test);
    size_test.allows_nilled = true;
    sxml_type_identifier size_integer_nillable = named_test(&fixture, size_test);
    size_test.type_annotation = builtin(SXML_TYPE_XS_DECIMAL);
    sxml_type_identifier size_decimal_nillable = named_test(&fixture, size_test);
    size_test.allows_nilled = false;
    sxml_type_identifier size_decimal = named_test(&fixture, size_test);
    size_test.name_test = any_name_test();
    sxml_type_identifier any_decimal = named_test(&fixture, size_test);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, size_integer, size_decimal));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, size_integer, any_decimal));
    SXML_TEST_EXPECT(state,
                     is_subtype(&fixture, size_integer, size_decimal_nillable));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, size_integer_nillable,
                                       size_decimal_nillable));
    SXML_TEST_EXPECT(state,
                     !is_subtype(&fixture, size_integer_nillable, size_decimal));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, size_decimal, size_integer));
    types_fixture_destroy(&fixture);
}

static void test_wildcard_name_tests(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_namespace_uri_identifier my_namespace = {.index = 0};
    sxml_namespace_uri_identifier your_namespace = {.index = 0};
    SXML_TEST_REQUIRE(state, sxml_name_pool_intern_namespace_uri(
                                 fixture.name_pool, TEXT("urn:my"),
                                 &my_namespace) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_name_pool_intern_namespace_uri(
                                 fixture.name_pool, TEXT("urn:your"),
                                 &your_namespace) == SXML_STATUS_OK);
    sxml_name_identifier my_title = intern_name(
        &fixture, (sxml_name_parts){.namespace_uri = my_namespace,
                                    .local_name = TEXT("title")});
    sxml_name_identifier your_title = intern_name(
        &fixture, (sxml_name_parts){.namespace_uri = your_namespace,
                                    .local_name = TEXT("title")});
    named_node_test test = {.node_kind = SXML_NODE_KIND_ELEMENT,
                            .name_test = exact_name_test(my_title),
                            .type_annotation = no_type,
                            .allows_nilled = false};
    sxml_type_identifier element_my_title = named_test(&fixture, test);
    test.name_test = exact_name_test(your_title);
    sxml_type_identifier element_your_title = named_test(&fixture, test);
    test.name_test = any_name_test();
    test.name_test.name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD;
    test.name_test.local_name = TEXT("title");
    sxml_type_identifier element_any_title = named_test(&fixture, test);
    test.name_test = any_name_test();
    test.name_test.name_test_kind = SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD;
    test.name_test.namespace_uri = my_namespace;
    sxml_type_identifier element_my_any = named_test(&fixture, test);
    const sxml_type_identifier both_titles[] = {element_my_title,
                                                element_your_title};
    sxml_type_identifier my_or_your_title =
        choice_of(&fixture, LIST_OF(both_titles));
    SXML_TEST_REQUIRE(state, element_any_title.index != 0 &&
                                 element_my_any.index != 0);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, my_or_your_title,
                                       element_any_title));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, element_my_title,
                                       element_my_any));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, element_your_title,
                                        element_my_any));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, element_any_title,
                                        element_my_title));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, element_my_any,
                                        element_any_title));
    types_fixture_destroy(&fixture);
}

static void test_attribute_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_name_identifier code = local_name(&fixture, TEXT("code"));
    named_node_test test = {.node_kind = SXML_NODE_KIND_ATTRIBUTE,
                            .name_test = any_name_test(),
                            .type_annotation = builtin(SXML_TYPE_XS_ID),
                            .allows_nilled = false};
    sxml_type_identifier any_id = named_test(&fixture, test);
    test.type_annotation = builtin(SXML_TYPE_XS_STRING);
    sxml_type_identifier any_string = named_test(&fixture, test);
    test.name_test = exact_name_test(code);
    test.type_annotation = builtin(SXML_TYPE_XS_ANY_ATOMIC_TYPE);
    sxml_type_identifier code_any_atomic = named_test(&fixture, test);
    test.type_annotation = no_type;
    sxml_type_identifier code_untyped = named_test(&fixture, test);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, any_id, any_string));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, any_string, any_id));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, code_untyped, code_any_atomic));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, code_untyped,
                                       builtin(SXML_TYPE_ATTRIBUTE_NODE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, code_untyped, any_string));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, any_id,
                                        builtin(SXML_TYPE_ELEMENT_NODE)));
    types_fixture_destroy(&fixture);
}

static void test_document_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_type_identifier element_a = element_named(&fixture, TEXT("a"));
    sxml_type_identifier element_b = element_named(&fixture, TEXT("b"));
    sxml_type_identifier element_c = element_named(&fixture, TEXT("c"));
    const sxml_type_identifier a_or_b_members[] = {element_a, element_b};
    const sxml_type_identifier a_b_c_members[] = {element_a, element_b,
                                                  element_c};
    sxml_type_identifier document_a_or_b =
        document_of(&fixture, choice_of(&fixture, LIST_OF(a_or_b_members)));
    sxml_type_identifier document_a_b_c =
        document_of(&fixture, choice_of(&fixture, LIST_OF(a_b_c_members)));
    sxml_type_identifier document_a = document_of(&fixture, element_a);
    sxml_type_identifier document_b = document_of(&fixture, element_b);
    const sxml_type_identifier separate_members[] = {document_a, document_b};
    sxml_type_identifier separate =
        choice_of(&fixture, LIST_OF(separate_members));
    sxml_type_identifier document_any_element =
        document_of(&fixture, builtin(SXML_TYPE_ELEMENT_NODE));
    SXML_TEST_REQUIRE(state, document_a_or_b.index != 0 && separate.index != 0);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, document_a_or_b, separate));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, document_a_or_b,
                                       document_a_b_c));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, document_a_b_c, separate));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, document_a,
                                       document_any_element));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, document_any_element,
                                       builtin(SXML_TYPE_DOCUMENT_NODE)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, document_a_or_b,
                                       builtin(SXML_TYPE_NODE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture,
                                        builtin(SXML_TYPE_DOCUMENT_NODE),
                                        document_any_element));
    /* document-node() may only contain element tests. */
    SXML_TEST_EXPECT(state, document_of(&fixture, builtin(SXML_TYPE_TEXT_NODE))
                                    .index == 0);
    types_fixture_destroy(&fixture);
}

static sxml_type_identifier json_node_test_type(types_fixture *fixture,
                                                sxml_json_node_test test) {
    sxml_type_description description =
        item_type_description(SXML_TYPE_VARIETY_JSON_NODE_TEST);
    description.variety_details.json_node_test = test;
    return register_type(fixture, &description);
}

static void test_json_node_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_json_node_test test = {
        .key_constraint = SXML_JSON_KEY_CONSTRAINT_CONSTANT,
        .key_type = builtin(SXML_TYPE_XS_STRING),
        .key_lexical_form = TEXT("price"),
        .has_value_type = true,
        .value_type = exactly_one(builtin(SXML_TYPE_XS_INTEGER))};
    sxml_type_identifier price_integer = json_node_test_type(&fixture, test);
    test.has_value_type = false;
    sxml_type_identifier price_any = json_node_test_type(&fixture, test);
    test.key_constraint = SXML_JSON_KEY_CONSTRAINT_ANY;
    test.has_value_type = true;
    test.value_type = exactly_one(builtin(SXML_TYPE_XS_DECIMAL));
    sxml_type_identifier any_decimal = json_node_test_type(&fixture, test);
    test.key_constraint = SXML_JSON_KEY_CONSTRAINT_ROOT;
    test.has_value_type = false;
    sxml_type_identifier root = json_node_test_type(&fixture, test);
    test.key_constraint = SXML_JSON_KEY_CONSTRAINT_ANY;
    sxml_type_identifier any_json_node = json_node_test_type(&fixture, test);
    SXML_TEST_EXPECT(state, any_json_node.index == SXML_TYPE_JSON_NODE);
    SXML_TEST_REQUIRE(state, price_integer.index != 0 && root.index != 0);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, price_integer, any_decimal));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, price_integer, price_any));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, price_any,
                                       builtin(SXML_TYPE_JSON_NODE)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, root,
                                       builtin(SXML_TYPE_GENERIC_NODE)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, price_any, any_decimal));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, any_decimal, price_integer));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, root, price_any));
    types_fixture_destroy(&fixture);
}

/* ------------------------------------------- Subtyping: function items */

static void test_function_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_sequence_type integer = exactly_one(builtin(SXML_TYPE_XS_INTEGER));
    sxml_sequence_type long_type = exactly_one(builtin(SXML_TYPE_XS_LONG));
    sxml_sequence_type string = exactly_one(builtin(SXML_TYPE_XS_STRING));
    sxml_sequence_type identifier = exactly_one(builtin(SXML_TYPE_XS_ID));
    sxml_type_identifier integer_to_string = unary_function_of(
        &fixture, (unary_signature){.parameter_type = integer,
                                    .result_type = string});
    sxml_type_identifier long_to_string = unary_function_of(
        &fixture, (unary_signature){.parameter_type = long_type,
                                    .result_type = string});
    sxml_type_identifier integer_to_identifier = unary_function_of(
        &fixture, (unary_signature){.parameter_type = integer,
                                    .result_type = identifier});
    const sxml_sequence_type two_integers[] = {integer, integer};
    sxml_type_identifier binary = function_of(
        &fixture, (function_signature){.parameter_types = two_integers,
                                       .parameter_count = 2,
                                       .result_type = string});
    SXML_TEST_REQUIRE(state, integer_to_string.index != 0 && binary.index != 0);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, integer_to_string,
                                       builtin(SXML_TYPE_FUNCTION)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, integer_to_string,
                                       long_to_string));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, long_to_string,
                                        integer_to_string));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, integer_to_identifier,
                                       integer_to_string));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, integer_to_identifier,
                                       long_to_string));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, integer_to_string,
                                        integer_to_identifier));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, binary, integer_to_string));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_FUNCTION),
                                        integer_to_string));
    types_fixture_destroy(&fixture);
}

static void test_map_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_type_identifier long_to_item = map_of(
        &fixture, (map_signature){.key_type = builtin(SXML_TYPE_XS_LONG),
                                  .value_type =
                                      exactly_one(builtin(SXML_TYPE_ITEM))});
    sxml_type_identifier integer_to_items = map_of(
        &fixture,
        (map_signature){.key_type = builtin(SXML_TYPE_XS_INTEGER),
                        .value_type = sequence(builtin(SXML_TYPE_ITEM),
                                               SXML_OCCURRENCE_ONE_OR_MORE)});
    SXML_TEST_EXPECT(state, is_subtype(&fixture, long_to_item,
                                       integer_to_items));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, integer_to_items,
                                        long_to_item));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, long_to_item,
                                       builtin(SXML_TYPE_MAP)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, long_to_item,
                                       builtin(SXML_TYPE_FUNCTION)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_MAP),
                                        long_to_item));
    /* map-put-002-hof: map(xs:string, xs:integer+) ⊆
       function(xs:string) as item()*, through function(xs:anyAtomicType)
       as item()* and contravariance. */
    sxml_type_identifier string_to_integers = map_of(
        &fixture,
        (map_signature){.key_type = builtin(SXML_TYPE_XS_STRING),
                        .value_type = sequence(builtin(SXML_TYPE_XS_INTEGER),
                                               SXML_OCCURRENCE_ONE_OR_MORE)});
    sxml_type_identifier string_function = unary_function_of(
        &fixture,
        (unary_signature){.parameter_type =
                              exactly_one(builtin(SXML_TYPE_XS_STRING)),
                          .result_type = any_sequence()});
    SXML_TEST_REQUIRE(state, string_to_integers.index != 0 &&
                                 string_function.index != 0);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, string_to_integers,
                                       string_function));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_MAP),
                                       string_function));
    /* map(xs:int, node()) ⊆ function(xs:anyAtomicType) as node()?, not
       ... as node(): a missing key gives the empty sequence. */
    sxml_type_identifier int_to_node = map_of(
        &fixture,
        (map_signature){.key_type = builtin(SXML_TYPE_XS_INT),
                        .value_type = exactly_one(builtin(SXML_TYPE_GENERIC_NODE))});
    sxml_sequence_type any_atomic =
        exactly_one(builtin(SXML_TYPE_XS_ANY_ATOMIC_TYPE));
    sxml_type_identifier to_optional_node = unary_function_of(
        &fixture,
        (unary_signature){.parameter_type = any_atomic,
                          .result_type = sequence(builtin(SXML_TYPE_GENERIC_NODE),
                                                  SXML_OCCURRENCE_ZERO_OR_ONE)});
    sxml_type_identifier to_node = unary_function_of(
        &fixture,
        (unary_signature){.parameter_type = any_atomic,
                          .result_type =
                              exactly_one(builtin(SXML_TYPE_GENERIC_NODE))});
    SXML_TEST_EXPECT(state, is_subtype(&fixture, int_to_node, to_optional_node));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, int_to_node, to_node));
    /* Map keys must be generalized atomic types. */
    SXML_TEST_EXPECT(state,
                     map_of(&fixture,
                            (map_signature){.key_type =
                                                builtin(SXML_TYPE_ELEMENT_NODE),
                                            .value_type = any_sequence()})
                             .index == 0);
    types_fixture_destroy(&fixture);
}

static void test_array_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_type_identifier integers =
        array_of(&fixture, exactly_one(builtin(SXML_TYPE_XS_INTEGER)));
    sxml_type_identifier decimals = array_of(
        &fixture,
        sequence(builtin(SXML_TYPE_XS_DECIMAL), SXML_OCCURRENCE_ONE_OR_MORE));
    sxml_type_identifier strings =
        array_of(&fixture, exactly_one(builtin(SXML_TYPE_XS_STRING)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, integers, decimals));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, decimals, integers));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, integers,
                                       builtin(SXML_TYPE_ARRAY)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, integers,
                                       builtin(SXML_TYPE_FUNCTION)));
    sxml_sequence_type integer = exactly_one(builtin(SXML_TYPE_XS_INTEGER));
    SXML_TEST_EXPECT(state,
                     is_subtype(&fixture, builtin(SXML_TYPE_ARRAY),
                                unary_function_of(
                                    &fixture,
                                    (unary_signature){.parameter_type = integer,
                                                      .result_type =
                                                          any_sequence()})));
    SXML_TEST_EXPECT(
        state, is_subtype(&fixture, strings,
                          unary_function_of(
                              &fixture,
                              (unary_signature){
                                  .parameter_type = integer,
                                  .result_type = exactly_one(
                                      builtin(SXML_TYPE_XS_STRING))})));
    SXML_TEST_EXPECT(
        state, !is_subtype(&fixture, strings,
                           unary_function_of(
                               &fixture,
                               (unary_signature){
                                   .parameter_type = exactly_one(
                                       builtin(SXML_TYPE_XS_STRING)),
                                   .result_type = any_sequence()})));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, integers,
                                        builtin(SXML_TYPE_MAP)));
    types_fixture_destroy(&fixture);
}

static void test_record_subtype_rules(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_sequence_type double_type = exactly_one(builtin(SXML_TYPE_XS_DOUBLE));
    const sxml_record_field untyped_fields[] = {
        {.field_name = TEXT("x"), .field_type = any_sequence()},
        {.field_name = TEXT("y"), .field_type = any_sequence()}};
    const sxml_record_field double_fields[] = {
        {.field_name = TEXT("x"), .field_type = double_type},
        {.field_name = TEXT("y"), .field_type = double_type}};
    const sxml_record_field integer_fields[] = {
        {.field_name = TEXT("x"), .field_type = any_sequence()},
        {.field_name = TEXT("y"),
         .field_type = exactly_one(builtin(SXML_TYPE_XS_INTEGER))}};
    const sxml_record_field decimal_fields[] = {
        {.field_name = TEXT("y"),
         .field_type = exactly_one(builtin(SXML_TYPE_XS_DECIMAL))},
        {.field_name = TEXT("x"), .field_type = any_sequence()}};
    const sxml_record_field other_fields[] = {
        {.field_name = TEXT("x"), .field_type = any_sequence()},
        {.field_name = TEXT("z"), .field_type = any_sequence()}};
    sxml_type_identifier record_untyped =
        record_of(&fixture, FIELDS_OF(untyped_fields));
    sxml_type_identifier record_double =
        record_of(&fixture, FIELDS_OF(double_fields));
    sxml_type_identifier record_integer =
        record_of(&fixture, FIELDS_OF(integer_fields));
    sxml_type_identifier record_decimal =
        record_of(&fixture, FIELDS_OF(decimal_fields));
    sxml_type_identifier record_other =
        record_of(&fixture, FIELDS_OF(other_fields));
    SXML_TEST_REQUIRE(state, record_untyped.index != 0 &&
                                 record_decimal.index != 0);
    sxml_type_identifier string_to_items = map_of(
        &fixture, (map_signature){.key_type = builtin(SXML_TYPE_XS_STRING),
                                  .value_type = any_sequence()});
    sxml_type_identifier string_to_double = map_of(
        &fixture, (map_signature){.key_type = builtin(SXML_TYPE_XS_STRING),
                                  .value_type = double_type});
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_RECORD),
                                       string_to_items));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_RECORD),
                                       builtin(SXML_TYPE_MAP)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, record_untyped,
                                       string_to_items));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, record_double,
                                       string_to_double));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, record_untyped,
                                        string_to_double));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, record_integer,
                                       record_decimal));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, record_decimal,
                                        record_integer));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, record_integer,
                                       record_untyped));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, record_untyped,
                                        record_other));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, record_double,
                                       builtin(SXML_TYPE_RECORD)));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, builtin(SXML_TYPE_RECORD),
                                        record_untyped));
    SXML_TEST_EXPECT(state,
                     is_subtype(&fixture, record_double,
                                unary_function_of(
                                    &fixture,
                                    (unary_signature){
                                        .parameter_type =
                                            exactly_one(builtin(SXML_TYPE_XS_STRING)),
                                        .result_type = sequence(
                                            builtin(SXML_TYPE_XS_DOUBLE),
                                            SXML_OCCURRENCE_ZERO_OR_ONE)})));
    size_t field_index = 99;
    SXML_TEST_EXPECT(state, sxml_type_registry_find_record_field(
                                fixture.registry,
                                (sxml_record_field_query){
                                    .record_type = record_decimal,
                                    .field_name = TEXT("x")},
                                &field_index));
    SXML_TEST_EXPECT(state, field_index == 1);
    SXML_TEST_EXPECT(state, !sxml_type_registry_find_record_field(
                                fixture.registry,
                                (sxml_record_field_query){
                                    .record_type = record_decimal,
                                    .field_name = TEXT("z")},
                                &field_index));
    SXML_TEST_EXPECT(state, field_index == 1);
    const sxml_record_field repeated_fields[] = {
        {.field_name = TEXT("x"), .field_type = any_sequence()},
        {.field_name = TEXT("x"), .field_type = double_type}};
    SXML_TEST_EXPECT(state,
                     record_of(&fixture, FIELDS_OF(repeated_fields)).index == 0);
    types_fixture_destroy(&fixture);
}

static void test_nominative_record_types(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_name_identifier person_name = local_name(&fixture, TEXT("person"));
    sxml_type_identifier person = {.index = 0};
    SXML_TEST_REQUIRE(state, sxml_type_registry_declare_nominative_record_type(
                                 fixture.registry, person_name, &person) ==
                                 SXML_STATUS_OK);
    sxml_type_identifier duplicate = {.index = 0};
    SXML_TEST_EXPECT(state, sxml_type_registry_declare_nominative_record_type(
                                fixture.registry, person_name, &duplicate) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_type_identifier found = {.index = 0};
    SXML_TEST_EXPECT(state, sxml_type_registry_find_type(fixture.registry,
                                                         person_name, &found));
    SXML_TEST_EXPECT(state, found.index == person.index);
    /* A built-in type name cannot be reused. */
    sxml_type_description string_description =
        item_type_description(SXML_TYPE_VARIETY_ANY_ITEM);
    SXML_TEST_REQUIRE(state, sxml_type_registry_describe_type(
                                 fixture.registry, builtin(SXML_TYPE_XS_STRING),
                                 &string_description) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_type_registry_declare_nominative_record_type(
                                fixture.registry, string_description.type_name,
                                &duplicate) == SXML_STATUS_INVALID_ARGUMENT);

    /* person("first name" as xs:string, "middle name" as xs:string*,
       "last name" as xs:string, "parent" as person?): recursive. */
    sxml_sequence_type string = exactly_one(builtin(SXML_TYPE_XS_STRING));
    const sxml_record_field fields[] = {
        {.field_name = TEXT("first name"), .field_type = string},
        {.field_name = TEXT("middle name"),
         .field_type = sequence(builtin(SXML_TYPE_XS_STRING),
                                SXML_OCCURRENCE_ZERO_OR_MORE)},
        {.field_name = TEXT("last name"), .field_type = string},
        {.field_name = TEXT("parent"),
         .field_type = sequence(person, SXML_OCCURRENCE_ZERO_OR_ONE)}};
    sxml_record_definition definition = {.record_type = person,
                                         .record_fields = FIELDS_OF(fields)};
    SXML_TEST_REQUIRE(state, sxml_type_registry_define_record_fields(
                                 fixture.registry, &definition) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_type_registry_define_record_fields(
                                fixture.registry, &definition) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    sxml_type_description description =
        item_type_description(SXML_TYPE_VARIETY_ANY_ITEM);
    SXML_TEST_REQUIRE(state, sxml_type_registry_describe_type(
                                 fixture.registry, person, &description) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, description.variety == SXML_TYPE_VARIETY_RECORD);
    SXML_TEST_EXPECT(state, description.type_name.index == person_name.index);
    SXML_TEST_EXPECT(state, description.variety_details.record_type.is_nominative);
    SXML_TEST_EXPECT(state, description.variety_details.record_type
                                    .record_fields.record_field_count == 4);
    size_t field_index = 0;
    SXML_TEST_EXPECT(state, sxml_type_registry_find_record_field(
                                fixture.registry,
                                (sxml_record_field_query){
                                    .record_type = person,
                                    .field_name = TEXT("last name")},
                                &field_index));
    SXML_TEST_EXPECT(state, field_index == 2);

    /* The same fields, structurally: the nominative type is a subtype of
       it and of its supertypes, never the other way (3.3.2.10). */
    const sxml_record_field reordered[] = {fields[3], fields[2], fields[0],
                                           fields[1]};
    sxml_type_identifier structural = record_of(&fixture, FIELDS_OF(reordered));
    SXML_TEST_REQUIRE(state, structural.index != 0);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, person, person));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, person, structural));
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, structural, person));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, person,
                                       builtin(SXML_TYPE_RECORD)));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, person,
                                       builtin(SXML_TYPE_MAP)));
    sxml_type_identifier other = {.index = 0};
    SXML_TEST_REQUIRE(state, sxml_type_registry_declare_nominative_record_type(
                                 fixture.registry,
                                 local_name(&fixture, TEXT("other")),
                                 &other) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, person, other));
    /* Nominative record types are declared, not registered. */
    sxml_type_description nominative =
        item_type_description(SXML_TYPE_VARIETY_RECORD);
    nominative.variety_details.record_type.is_nominative = true;
    SXML_TEST_EXPECT(state, register_type(&fixture, &nominative).index == 0);
    /* Repeated field names are rejected and leave the type undefined. */
    const sxml_record_field repeated[] = {fields[0], fields[0]};
    SXML_TEST_EXPECT(state, sxml_type_registry_define_record_fields(
                                fixture.registry,
                                &(sxml_record_definition){
                                    .record_type = other,
                                    .record_fields = FIELDS_OF(repeated)}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_type_registry_define_record_fields(
                                fixture.registry,
                                &(sxml_record_definition){
                                    .record_type = other,
                                    .record_fields = FIELDS_OF(fields)}) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_type_registry_define_record_fields(
                                fixture.registry,
                                &(sxml_record_definition){
                                    .record_type = structural,
                                    .record_fields = FIELDS_OF(fields)}) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    types_fixture_destroy(&fixture);
}

/* ---------------------------------------------- Subtyping: sequences */

static void test_sequence_subtype_table(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_type_identifier integer = builtin(SXML_TYPE_XS_INTEGER);
    sxml_type_identifier decimal = builtin(SXML_TYPE_XS_DECIMAL);
    sxml_type_identifier error = builtin(SXML_TYPE_XS_ERROR);
    sxml_sequence_type empty = sequence(no_type, SXML_OCCURRENCE_ZERO);
    SXML_TEST_EXPECT(state, is_sequence_subtype(
                                &fixture, empty,
                                sequence(decimal, SXML_OCCURRENCE_ZERO_OR_ONE)));
    SXML_TEST_EXPECT(state, is_sequence_subtype(&fixture, empty, empty));
    SXML_TEST_EXPECT(state, is_sequence_subtype(
                                &fixture,
                                sequence(error, SXML_OCCURRENCE_ZERO_OR_MORE),
                                empty));
    SXML_TEST_EXPECT(state, !is_sequence_subtype(&fixture, empty,
                                                 exactly_one(decimal)));
    SXML_TEST_EXPECT(state, is_sequence_subtype(&fixture, exactly_one(error),
                                                exactly_one(integer)));
    SXML_TEST_EXPECT(state, !is_sequence_subtype(&fixture, exactly_one(integer),
                                                 exactly_one(error)));
    SXML_TEST_EXPECT(state, is_sequence_subtype(
                                &fixture, exactly_one(integer),
                                sequence(decimal, SXML_OCCURRENCE_ONE_OR_MORE)));
    SXML_TEST_EXPECT(state, is_sequence_subtype(
                                &fixture,
                                sequence(integer, SXML_OCCURRENCE_ONE_OR_MORE),
                                sequence(decimal, SXML_OCCURRENCE_ZERO_OR_MORE)));
    SXML_TEST_EXPECT(state, !is_sequence_subtype(
                                &fixture,
                                sequence(integer, SXML_OCCURRENCE_ZERO_OR_MORE),
                                sequence(decimal, SXML_OCCURRENCE_ONE_OR_MORE)));
    SXML_TEST_EXPECT(state, !is_sequence_subtype(
                                &fixture,
                                sequence(integer, SXML_OCCURRENCE_ZERO_OR_ONE),
                                exactly_one(decimal)));
    SXML_TEST_EXPECT(state, !is_sequence_subtype(
                                &fixture, exactly_one(decimal),
                                sequence(integer, SXML_OCCURRENCE_ZERO_OR_MORE)));
    SXML_TEST_EXPECT(state, !is_sequence_subtype(
                                &fixture, exactly_one((sxml_type_identifier){
                                              .index = 700}),
                                any_sequence()));
    types_fixture_destroy(&fixture);
}

/* ------------------------------------------------------------- Interning */

static void test_equal_descriptions_intern_to_one_type(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_type_identifier first =
        array_of(&fixture, exactly_one(builtin(SXML_TYPE_XS_STRING)));
    sxml_type_identifier second =
        array_of(&fixture, exactly_one(builtin(SXML_TYPE_XS_STRING)));
    SXML_TEST_EXPECT(state, first.index == SXML_TYPE_FIRST_REGISTERED);
    SXML_TEST_EXPECT(state, second.index == first.index);
    /* Built-in equivalents give the built-in identifier. */
    SXML_TEST_EXPECT(state, node_test_type(&fixture,
                                           node_test_of_kind(
                                               SXML_NODE_KIND_ELEMENT))
                                    .index == SXML_TYPE_ELEMENT_NODE);
    SXML_TEST_EXPECT(state, node_test_type(&fixture,
                                           node_test_of_kind(SXML_NODE_KIND_ANY))
                                    .index == SXML_TYPE_NODE);
    sxml_type_description any_map = item_type_description(SXML_TYPE_VARIETY_MAP);
    any_map.variety_details.map_type.is_any_map = true;
    any_map.variety_details.map_type.key_type = builtin(SXML_TYPE_ELEMENT_NODE);
    SXML_TEST_EXPECT(state, register_type(&fixture, &any_map).index ==
                                SXML_TYPE_MAP);
    sxml_type_description any_function =
        item_type_description(SXML_TYPE_VARIETY_FUNCTION);
    any_function.variety_details.function_type.is_any_function = true;
    SXML_TEST_EXPECT(state, register_type(&fixture, &any_function).index ==
                                SXML_TYPE_FUNCTION);
    /* (A) is A. */
    const sxml_type_identifier single[] = {first};
    SXML_TEST_EXPECT(state, choice_of(&fixture, LIST_OF(single)).index ==
                                first.index);
    /* Views are copied: the registered type outlives the caller's data. */
    sxml_type_description described =
        item_type_description(SXML_TYPE_VARIETY_ANY_ITEM);
    {
        sxml_sequence_type parameters[] = {
            exactly_one(builtin(SXML_TYPE_XS_DATE))};
        sxml_type_identifier function = function_of(
            &fixture, (function_signature){.parameter_types = parameters,
                                           .parameter_count = 1,
                                           .result_type = any_sequence()});
        parameters[0] = exactly_one(builtin(SXML_TYPE_XS_TIME));
        SXML_TEST_REQUIRE(state, sxml_type_registry_describe_type(
                                     fixture.registry, function, &described) ==
                                     SXML_STATUS_OK);
    }
    SXML_TEST_EXPECT(state, described.variety_details.function_type
                                    .parameter_types.sequence_types[0]
                                    .item_type.index == SXML_TYPE_XS_DATE);
    SXML_TEST_EXPECT(state, !described.is_builtin);
    types_fixture_destroy(&fixture);
}

static void test_invalid_descriptions_are_rejected(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_type_identifier type = {.index = 99};
    sxml_type_description atomic =
        item_type_description(SXML_TYPE_VARIETY_ATOMIC);
    SXML_TEST_EXPECT(state, sxml_type_registry_register_item_type(
                                fixture.registry, &atomic, &type) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, type.index == 99);
    /* Unissued identifiers, non-item types as members, empty choices. */
    SXML_TEST_EXPECT(state, array_of(&fixture, exactly_one((sxml_type_identifier){
                                                   .index = 513}))
                                    .index == 0);
    SXML_TEST_EXPECT(state,
                     array_of(&fixture, exactly_one(builtin(SXML_TYPE_XS_UNTYPED)))
                             .index == 0);
    SXML_TEST_EXPECT(state, choice_of(&fixture, (identifier_list){
                                                    .identifiers = NULL,
                                                    .identifier_count = 0})
                                    .index == 0);
    sxml_type_identifier bad_member[] = {builtin(SXML_TYPE_XS_NMTOKENS)};
    SXML_TEST_EXPECT(state, choice_of(&fixture, LIST_OF(bad_member)).index == 0);
    SXML_TEST_EXPECT(state, enumeration_of(&fixture, (sxml_byte_view_list){
                                                         .byte_views = NULL,
                                                         .byte_view_count = 0})
                                    .index == 0);
    /* Node tests: a text test with a name, an element test annotated with
       an item type. */
    sxml_node_test text_test = node_test_of_kind(SXML_NODE_KIND_TEXT);
    text_test.name_test = exact_name_test(local_name(&fixture, TEXT("t")));
    SXML_TEST_EXPECT(state, node_test_type(&fixture, text_test).index == 0);
    sxml_node_test element_test = node_test_of_kind(SXML_NODE_KIND_ELEMENT);
    element_test.type_annotation = builtin(SXML_TYPE_MAP);
    SXML_TEST_EXPECT(state, node_test_type(&fixture, element_test).index == 0);
    element_test.type_annotation = no_type;
    element_test.name_test = exact_name_test((sxml_name_identifier){.index = 9999});
    SXML_TEST_EXPECT(state, node_test_type(&fixture, element_test).index == 0);
    sxml_json_node_test json_test = {
        .key_constraint = SXML_JSON_KEY_CONSTRAINT_CONSTANT,
        .key_type = builtin(SXML_TYPE_XS_DATE),
        .key_lexical_form = TEXT("2020-01-01"),
        .has_value_type = false,
        .value_type = any_sequence()};
    SXML_TEST_EXPECT(state, json_node_test_type(&fixture, json_test).index == 0);
    types_fixture_destroy(&fixture);
}

/* -------------------------------------------------------- Limits, growth */

static void test_registry_without_allocator_only_knows_builtins(
    sxml_test_state *state) {
    sxml_name_pool *name_pool = create_name_pool();
    SXML_TEST_REQUIRE(state, name_pool != NULL);
    alignas(TEST_STORAGE_ALIGNMENT) static uint8_t storage[TEST_STORAGE_BYTE_COUNT];
    sxml_type_registry *registry = NULL;
    SXML_TEST_REQUIRE(state, sxml_type_registry_initialize(
                                 (sxml_memory_view){.bytes = storage,
                                                    .byte_count = sizeof(storage)},
                                 &(sxml_type_registry_configuration){
                                     .allocator = {.state = NULL,
                                                   .operations = NULL},
                                     .name_pool = name_pool,
                                     .maximum_registered_type_count = 0},
                                 &registry) == SXML_STATUS_OK);
    types_fixture fixture = {.name_pool = name_pool, .registry = registry};
    sxml_type_description array = item_type_description(SXML_TYPE_VARIETY_ARRAY);
    array.variety_details.array_type.member_type = any_sequence();
    sxml_type_identifier type = {.index = 0};
    SXML_TEST_EXPECT(state, sxml_type_registry_register_item_type(
                                registry, &array, &type) ==
                                SXML_STATUS_CAPACITY_EXCEEDED);
    array.variety_details.array_type.is_any_array = true;
    SXML_TEST_EXPECT(state, sxml_type_registry_register_item_type(
                                registry, &array, &type) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, type.index == SXML_TYPE_ARRAY);
    SXML_TEST_EXPECT(state, sxml_type_registry_declare_nominative_record_type(
                                registry, local_name(&fixture, TEXT("r")),
                                &type) == SXML_STATUS_CAPACITY_EXCEEDED);
    SXML_TEST_EXPECT(state, is_subtype(&fixture, builtin(SXML_TYPE_XS_BYTE),
                                       builtin(SXML_TYPE_XS_NUMERIC)));
    sxml_type_registry_finalize(registry);
    sxml_name_pool_destroy(name_pool);
}

static void test_registered_type_limit(sxml_test_state *state) {
    sxml_name_pool *name_pool = create_name_pool();
    SXML_TEST_REQUIRE(state, name_pool != NULL);
    sxml_type_registry_configuration configuration =
        default_configuration(name_pool);
    configuration.maximum_registered_type_count = 3;
    sxml_type_registry *registry = NULL;
    SXML_TEST_REQUIRE(state, sxml_type_registry_create(
                                 sxml_default_allocator(), &configuration,
                                 &registry) == SXML_STATUS_OK);
    types_fixture fixture = {.name_pool = name_pool, .registry = registry};
    sxml_type_identifier member = builtin(SXML_TYPE_XS_STRING);
    for (uint32_t depth = 0; depth < 3; depth += 1) {
        member = array_of(&fixture, exactly_one(member));
        SXML_TEST_EXPECT(state, member.index == SXML_TYPE_FIRST_REGISTERED + depth);
    }
    sxml_type_description array = item_type_description(SXML_TYPE_VARIETY_ARRAY);
    array.variety_details.array_type.member_type = exactly_one(member);
    sxml_type_identifier type = {.index = 0};
    SXML_TEST_EXPECT(state, sxml_type_registry_register_item_type(
                                registry, &array, &type) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    /* Registered types are still found. */
    array.variety_details.array_type.member_type =
        exactly_one(builtin(SXML_TYPE_XS_STRING));
    SXML_TEST_EXPECT(state, sxml_type_registry_register_item_type(
                                registry, &array, &type) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, type.index == SXML_TYPE_FIRST_REGISTERED);
    sxml_type_registry_destroy(registry);
    sxml_name_pool_destroy(name_pool);
}

/* Nested arrays array(array(...array(member)...)) of depth levels. */
typedef struct nesting_request {
    sxml_type_identifier member_type;
    uint32_t depth;
} nesting_request;

static sxml_type_identifier nested_arrays(types_fixture *fixture,
                                          nesting_request request) {
    sxml_type_identifier member = request.member_type;
    for (uint32_t level = 0; level < request.depth && member.index != 0;
         level += 1) {
        member = array_of(fixture, exactly_one(member));
    }
    return member;
}

static void test_many_types_and_deep_nesting(sxml_test_state *state) {
    types_fixture fixture = {.name_pool = NULL, .registry = NULL};
    SXML_TEST_REQUIRE(state, types_fixture_create(&fixture));
    sxml_type_identifier deep_integers = nested_arrays(
        &fixture, (nesting_request){.member_type = builtin(SXML_TYPE_XS_INTEGER),
                                    .depth = 1000});
    SXML_TEST_REQUIRE(state, deep_integers.index ==
                                 SXML_TYPE_FIRST_REGISTERED + 999);
    /* Interning still finds the first types after the tables grew. */
    SXML_TEST_EXPECT(state, nested_arrays(&fixture,
                                          (nesting_request){
                                              .member_type =
                                                  builtin(SXML_TYPE_XS_INTEGER),
                                              .depth = 1000})
                                    .index == deep_integers.index);
    sxml_type_identifier shallow_integers = nested_arrays(
        &fixture, (nesting_request){.member_type = builtin(SXML_TYPE_XS_INTEGER),
                                    .depth = 20});
    sxml_type_identifier shallow_decimals = nested_arrays(
        &fixture, (nesting_request){.member_type = builtin(SXML_TYPE_XS_DECIMAL),
                                    .depth = 20});
    sxml_type_identifier deep_decimals = nested_arrays(
        &fixture, (nesting_request){.member_type = builtin(SXML_TYPE_XS_DECIMAL),
                                    .depth = 1000});
    SXML_TEST_EXPECT(state, is_subtype(&fixture, shallow_integers,
                                       shallow_decimals));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, deep_integers, deep_integers));
    SXML_TEST_EXPECT(state, is_subtype(&fixture, deep_integers,
                                       builtin(SXML_TYPE_ARRAY)));
    /* Beyond the depth budget the answer is a conservative false (D58). */
    SXML_TEST_EXPECT(state, !is_subtype(&fixture, deep_integers, deep_decimals));
    types_fixture_destroy(&fixture);
}

/* --------------------------------------------------- Allocation failures */

/* Creates a registry through allocator, registers and declares types of
   every kind that copies data, checks them, destroys it. */
static sxml_status create_register_and_destroy(sxml_allocator allocator,
                                               sxml_name_pool *name_pool) {
    sxml_type_registry *registry = NULL;
    sxml_status status = sxml_type_registry_create(
        allocator,
        &(sxml_type_registry_configuration){.allocator = allocator,
                                            .name_pool = name_pool,
                                            .maximum_registered_type_count = 0},
        &registry);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_identifier member = builtin(SXML_TYPE_XS_STRING);
    for (uint32_t level = 0; level < 40 && status == SXML_STATUS_OK;
         level += 1) {
        sxml_type_description array =
            item_type_description(SXML_TYPE_VARIETY_ARRAY);
        array.variety_details.array_type.member_type = exactly_one(member);
        status = sxml_type_registry_register_item_type(registry, &array, &member);
    }
    const sxml_byte_view values[] = {TEXT("a"), TEXT("b"), TEXT("c")};
    sxml_type_identifier enumeration = {.index = 0};
    if (status == SXML_STATUS_OK) {
        sxml_type_description description =
            item_type_description(SXML_TYPE_VARIETY_ENUMERATION);
        description.variety_details.enumeration_values = VALUES_OF(values);
        status = sxml_type_registry_register_item_type(registry, &description,
                                                       &enumeration);
    }
    sxml_name_identifier name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = {.index = 0},
                                 .local_name = TEXT("point")},
            &name);
    }
    sxml_type_identifier point = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_declare_nominative_record_type(registry,
                                                                   name, &point);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_record_field fields[] = {
            {.field_name = TEXT("x"),
             .field_type = exactly_one(builtin(SXML_TYPE_XS_DOUBLE))},
            {.field_name = TEXT("y"),
             .field_type = exactly_one(builtin(SXML_TYPE_XS_DOUBLE))}};
        status = sxml_type_registry_define_record_fields(
            registry, &(sxml_record_definition){.record_type = point,
                                                .record_fields =
                                                    FIELDS_OF(fields)});
    }
    if (status == SXML_STATUS_OK &&
        !sxml_type_registry_is_subtype(
            registry, (sxml_type_pair){.derived_type = point,
                                       .base_type = builtin(SXML_TYPE_MAP)})) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_type_registry_destroy(registry);
    return status;
}

static void test_registry_survives_every_allocation_failure(
    sxml_test_state *state) {
    sxml_name_pool *name_pool = create_name_pool();
    SXML_TEST_REQUIRE(state, name_pool != NULL);
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
        sxml_status status = create_register_and_destroy(
            sxml_counting_allocator_as_allocator(counting_allocator), name_pool);
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
    /* The registry, entries, indexes, arena blocks. */
    SXML_TEST_EXPECT(state, failing_allocation_number > 4);
    sxml_name_pool_destroy(name_pool);
}

/* A failed registration leaves the registry usable; registering again
   succeeds and gives the next identifier. */
static void test_failed_registration_can_be_retried(sxml_test_state *state) {
    sxml_name_pool *name_pool = create_name_pool();
    SXML_TEST_REQUIRE(state, name_pool != NULL);
    for (uint64_t failing_allocation_number = 1; failing_allocation_number <= 4;
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
        alignas(TEST_STORAGE_ALIGNMENT) static uint8_t storage[TEST_STORAGE_BYTE_COUNT];
        sxml_type_registry *registry = NULL;
        SXML_TEST_REQUIRE(state, sxml_type_registry_initialize(
                                     (sxml_memory_view){
                                         .bytes = storage,
                                         .byte_count = sizeof(storage)},
                                     &(sxml_type_registry_configuration){
                                         .allocator =
                                             sxml_counting_allocator_as_allocator(
                                                 counting_allocator),
                                         .name_pool = name_pool,
                                         .maximum_registered_type_count = 0},
                                     &registry) == SXML_STATUS_OK);
        const sxml_record_field fields[] = {
            {.field_name = TEXT("x"), .field_type = any_sequence()}};
        sxml_type_description record =
            item_type_description(SXML_TYPE_VARIETY_RECORD);
        record.variety_details.record_type.record_fields = FIELDS_OF(fields);
        sxml_type_identifier type = {.index = 99};
        /* The first registration allocates entries, slots and an arena
           block. */
        sxml_status status =
            sxml_type_registry_register_item_type(registry, &record, &type);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY ||
                                    failing_allocation_number == 4);
        if (status != SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, type.index == 99);
            SXML_TEST_EXPECT(state, sxml_type_registry_register_item_type(
                                        registry, &record, &type) ==
                                        SXML_STATUS_OK);
        }
        SXML_TEST_EXPECT(state, type.index == SXML_TYPE_FIRST_REGISTERED);
        size_t field_index = 99;
        SXML_TEST_EXPECT(state, sxml_type_registry_find_record_field(
                                    registry,
                                    (sxml_record_field_query){
                                        .record_type = type,
                                        .field_name = TEXT("x")},
                                    &field_index));
        SXML_TEST_EXPECT(state, field_index == 0);
        sxml_type_registry_finalize(registry);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(counting_allocator);
        sxml_counting_allocator_finalize(counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
    }
    sxml_name_pool_destroy(name_pool);
}

static const sxml_test_case types_test_cases[] = {
    {.name = "memory_requirements_need_a_name_pool",
     .function = test_memory_requirements_need_a_name_pool},
    {.name = "initialize_checks_memory",
     .function = test_initialize_checks_memory},
    {.name = "finalize_and_destroy_accept_null",
     .function = test_finalize_and_destroy_accept_null},
    {.name = "builtin_types_are_described",
     .function = test_builtin_types_are_described},
    {.name = "find_type_by_name", .function = test_find_type_by_name},
    {.name = "derivation_follows_base_types_and_unions",
     .function = test_derivation_follows_base_types_and_unions},
    {.name = "general_and_atomic_subtype_rules",
     .function = test_general_and_atomic_subtype_rules},
    {.name = "choice_subtype_rules", .function = test_choice_subtype_rules},
    {.name = "enumeration_subtype_rules",
     .function = test_enumeration_subtype_rules},
    {.name = "node_kind_subtype_rules",
     .function = test_node_kind_subtype_rules},
    {.name = "element_subtype_rules", .function = test_element_subtype_rules},
    {.name = "wildcard_name_tests", .function = test_wildcard_name_tests},
    {.name = "attribute_subtype_rules",
     .function = test_attribute_subtype_rules},
    {.name = "document_subtype_rules", .function = test_document_subtype_rules},
    {.name = "json_node_subtype_rules",
     .function = test_json_node_subtype_rules},
    {.name = "function_subtype_rules", .function = test_function_subtype_rules},
    {.name = "map_subtype_rules", .function = test_map_subtype_rules},
    {.name = "array_subtype_rules", .function = test_array_subtype_rules},
    {.name = "record_subtype_rules", .function = test_record_subtype_rules},
    {.name = "nominative_record_types",
     .function = test_nominative_record_types},
    {.name = "sequence_subtype_table", .function = test_sequence_subtype_table},
    {.name = "equal_descriptions_intern_to_one_type",
     .function = test_equal_descriptions_intern_to_one_type},
    {.name = "invalid_descriptions_are_rejected",
     .function = test_invalid_descriptions_are_rejected},
    {.name = "registry_without_allocator_only_knows_builtins",
     .function = test_registry_without_allocator_only_knows_builtins},
    {.name = "registered_type_limit", .function = test_registered_type_limit},
    {.name = "many_types_and_deep_nesting",
     .function = test_many_types_and_deep_nesting},
    {.name = "registry_survives_every_allocation_failure",
     .function = test_registry_survives_every_allocation_failure},
    {.name = "failed_registration_can_be_retried",
     .function = test_failed_registration_can_be_retried},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "types",
            .cases = types_test_cases,
            .case_count = SXML_TEST_COUNT(types_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
