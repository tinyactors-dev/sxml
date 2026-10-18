/*
 * Unit tests for src/schema_components.c: the schema components' tables,
 * name index, blocks, built-in components and error reports
 * (src/schema_internal.h, D167). The last case builds part of example 2
 * of the header by hand, as packets 5.5 and 5.6 build components before
 * the reader exists.
 *
 * Golden file: tests/xslt_ast_test.c (a store built by hand and read
 * back through its accessors).
 */
#include "processor_internal.h"
#include "schema_internal.h"
#include "sxml/processor.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

/* A processor and its schema components, with the built-in ones. */
typedef struct test_fixture {
    sxml_processor *processor;
    sxml_schema_components *components;
} test_fixture;

static bool fixture_open(test_fixture *fixture) {
    const sxml_allocator allocator = sxml_default_allocator();
    memset(fixture, 0, sizeof(*fixture));
    if (sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                              &fixture->processor) != SXML_STATUS_OK) {
        return false;
    }
    if (sxml_schema_components_create(
            allocator,
            &(sxml_schema_components_configuration){.processor = fixture->processor},
            &fixture->components) != SXML_STATUS_OK) {
        return false;
    }
    return sxml_schema_components_add_builtin_components(fixture->components) ==
           SXML_STATUS_OK;
}

static void fixture_close(test_fixture *fixture) {
    sxml_schema_components_destroy(fixture->components);
    sxml_processor_destroy(fixture->processor);
}

typedef struct name_request {
    sxml_well_known_namespace namespace_uri;
    const char *local_name;
} name_request;

static sxml_name_identifier intern_name(const test_fixture *fixture, name_request request) {
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_name(
            sxml_processor_get_name_pool(fixture->processor),
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(request.namespace_uri),
                .local_name = sxml_byte_view_from_c_string(request.local_name)},
            &name) != SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return name;
}

static uint32_t find_global(const test_fixture *fixture, sxml_schema_symbol_space symbol_space,
                            name_request request) {
    uint32_t component = 0;
    if (!sxml_schema_components_find_name(
            fixture->components,
            (sxml_schema_name_key){.symbol_space = symbol_space,
                                   .name = intern_name(fixture, request)},
            &component)) {
        return 0;
    }
    return component;
}

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

static void test_builtin_components(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    const sxml_schema_components *components = fixture.components;

    /* Type definitions 1 to 54 are the registry's built-ins. */
    const sxml_schema_type_definition *string_type = sxml_schema_get_type_definition(
        components, (sxml_schema_type_definition_index){.value = SXML_TYPE_XS_STRING});
    SXML_TEST_EXPECT(state, string_type->is_builtin);
    SXML_TEST_EXPECT(state, string_type->variety == SXML_TYPE_VARIETY_ATOMIC);
    SXML_TEST_EXPECT(state, string_type->registered_type.index == SXML_TYPE_XS_STRING);
    SXML_TEST_EXPECT(state, string_type->base_type.value == SXML_TYPE_XS_ANY_ATOMIC_TYPE);
    SXML_TEST_EXPECT(state, string_type->primitive_type.value == SXML_TYPE_XS_STRING);
    SXML_TEST_EXPECT(state,
                     find_global(&fixture, SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                 (name_request){SXML_NAMESPACE_XML_SCHEMA, "string"}) ==
                         SXML_TYPE_XS_STRING);

    /* xs:anyType: its own base, mixed, the wildcard content of 3.4.7. */
    const sxml_schema_type_definition *any_type = sxml_schema_get_type_definition(
        components, (sxml_schema_type_definition_index){.value = SXML_TYPE_XS_ANY_TYPE});
    SXML_TEST_EXPECT(state, any_type->variety == SXML_TYPE_VARIETY_COMPLEX);
    SXML_TEST_EXPECT(state, any_type->base_type.value == SXML_TYPE_XS_ANY_TYPE);
    SXML_TEST_EXPECT(state, any_type->content_type_variety == SXML_CONTENT_TYPE_VARIETY_MIXED);
    const sxml_schema_particle *outer_particle =
        sxml_schema_get_particle(components, any_type->particle);
    SXML_TEST_EXPECT(state, outer_particle->term_kind == SXML_SCHEMA_TERM_MODEL_GROUP);
    const sxml_schema_model_group *model_group = sxml_schema_get_model_group(
        components, (sxml_schema_model_group_index){.value = outer_particle->term});
    const sxml_schema_index_view particles = sxml_schema_get_list(components, model_group->particles);
    SXML_TEST_REQUIRE(state, particles.index_count == 1);
    const sxml_schema_particle *inner_particle = sxml_schema_get_particle(
        components, (sxml_schema_particle_index){.value = particles.indexes[0]});
    SXML_TEST_EXPECT(state, inner_particle->term_kind == SXML_SCHEMA_TERM_WILDCARD);
    SXML_TEST_EXPECT(state, inner_particle->maximum_occurrences == SXML_SCHEMA_UNBOUNDED);
    const sxml_schema_wildcard *wildcard = sxml_schema_get_wildcard(
        components, (sxml_schema_wildcard_index){.value = inner_particle->term});
    SXML_TEST_EXPECT(state, wildcard->process_contents == SXML_SCHEMA_PROCESS_CONTENTS_LAX);

    /* xs:numeric is a union of three; XDM's types have no XSD name. */
    const sxml_schema_type_definition *numeric_type = sxml_schema_get_type_definition(
        components, (sxml_schema_type_definition_index){.value = SXML_TYPE_XS_NUMERIC});
    SXML_TEST_EXPECT(state,
                     sxml_schema_get_list(components, numeric_type->member_types).index_count ==
                         3);
    SXML_TEST_EXPECT(state,
                     find_global(&fixture, SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                 (name_request){SXML_NAMESPACE_XML_SCHEMA, "untyped"}) == 0);
    SXML_TEST_EXPECT(state,
                     find_global(&fixture, SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                 (name_request){SXML_NAMESPACE_XML_SCHEMA, "numeric"}) == 0);

    /* Type 55 and the xsi: attribute declarations (3.2.7). */
    const sxml_schema_type_definition *location_type = sxml_schema_get_type_definition(
        components, (sxml_schema_type_definition_index){.value = 55});
    SXML_TEST_EXPECT(state, location_type->variety == SXML_TYPE_VARIETY_LIST);
    SXML_TEST_EXPECT(state, !location_type->is_builtin);
    SXML_TEST_EXPECT(state, location_type->item_type.value == SXML_TYPE_XS_ANY_URI);
    SXML_TEST_EXPECT(state,
                     find_global(&fixture, SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
                                 (name_request){SXML_NAMESPACE_XML_SCHEMA_INSTANCE, "nil"}) ==
                         2);
    const sxml_schema_attribute_declaration *location_attribute =
        sxml_schema_get_attribute_declaration(
            components, (sxml_schema_attribute_declaration_index){.value = 3});
    SXML_TEST_EXPECT(state, location_attribute->type_definition.value == 55);

    /* What a schema adds comes after the built-in components. */
    const sxml_schema_type_definition anonymous_type = {.variety = SXML_TYPE_VARIETY_COMPLEX};
    SXML_TEST_EXPECT(state, add_entry(&fixture, SXML_SCHEMA_TABLE_TYPE_DEFINITION,
                                      &anonymous_type) == 56);
    const sxml_schema_particle particle = {.minimum_occurrences = 1};
    SXML_TEST_EXPECT(state, add_entry(&fixture, SXML_SCHEMA_TABLE_PARTICLE, &particle) == 3);
    const sxml_schema_element_declaration declaration = {.is_nillable = true};
    SXML_TEST_EXPECT(state, add_entry(&fixture, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION,
                                      &declaration) == 1);
    SXML_TEST_EXPECT(state, sxml_schema_components_add_builtin_components(
                                fixture.components) == SXML_STATUS_INVALID_ARGUMENT);
    fixture_close(&fixture);
}

static void test_tables_lists_and_texts(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    sxml_schema_components *components = fixture.components;

    /* Index 0 and indexes not in use read as zero entries. */
    SXML_TEST_EXPECT(state, sxml_schema_get_facet(components, (sxml_schema_facet_index){.value = 0})
                                    ->facet_kind == SXML_FACET_KIND_LENGTH);
    SXML_TEST_EXPECT(state, sxml_schema_get_identity_constraint(
                                components, (sxml_schema_identity_constraint_index){.value = 7})
                                    ->constraint_name.index == 0);

    sxml_schema_text text = {.byte_offset = 0, .byte_count = 0};
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_text(
                                 components, sxml_byte_view_from_c_string("\\d{3}-[A-Z]{2}"),
                                 &text) == SXML_STATUS_OK);
    const sxml_schema_facet facet = {.facet_kind = SXML_FACET_KIND_PATTERN, .value = text};
    const uint32_t facet_index = add_entry(&fixture, SXML_SCHEMA_TABLE_FACET, &facet);
    SXML_TEST_REQUIRE(state, facet_index == 1);
    const sxml_byte_view pattern = sxml_schema_get_text(
        components,
        sxml_schema_get_facet(components, (sxml_schema_facet_index){.value = facet_index})->value);
    SXML_TEST_EXPECT(state, pattern.byte_count == 14 &&
                                memcmp(pattern.bytes, "\\d{3}-[A-Z]{2}", 14) == 0);

    /* Many entries: tables grow and keep what they hold. */
    for (uint32_t particle_number = 0; particle_number < 1000; particle_number += 1) {
        const sxml_schema_particle particle = {.minimum_occurrences = particle_number,
                                               .maximum_occurrences = particle_number + 1U};
        SXML_TEST_REQUIRE(state, add_entry(&fixture, SXML_SCHEMA_TABLE_PARTICLE, &particle) ==
                                     particle_number + 3U);
    }
    SXML_TEST_EXPECT(state, sxml_schema_get_particle(components,
                                                     (sxml_schema_particle_index){.value = 503})
                                    ->minimum_occurrences == 500);

    /* A stage writes properties in place. */
    sxml_schema_particle *edited = sxml_schema_components_edit_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_PARTICLE, .index = 503});
    SXML_TEST_REQUIRE(state, edited != NULL);
    edited->maximum_occurrences = SXML_SCHEMA_UNBOUNDED;
    SXML_TEST_EXPECT(state, sxml_schema_get_particle(components,
                                                     (sxml_schema_particle_index){.value = 503})
                                    ->maximum_occurrences == SXML_SCHEMA_UNBOUNDED);
    SXML_TEST_EXPECT(state, sxml_schema_components_edit_entry(
                                components, (sxml_schema_entry_key){
                                                .table = SXML_SCHEMA_TABLE_PARTICLE,
                                                .index = 0}) == NULL);

    const uint32_t particle_indexes[] = {3, 4, 5};
    sxml_schema_list list = {.first_entry_index = 0, .entry_count = 0};
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_list(
                                 components,
                                 (sxml_schema_index_view){.indexes = particle_indexes,
                                                          .index_count = 3},
                                 &list) == SXML_STATUS_OK);
    const sxml_schema_index_view entries = sxml_schema_get_list(components, list);
    SXML_TEST_EXPECT(state, entries.index_count == 3 && entries.indexes[2] == 5);
    sxml_schema_list empty_list = {.first_entry_index = 9, .entry_count = 9};
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_list(
                                 components,
                                 (sxml_schema_index_view){.indexes = NULL, .index_count = 0},
                                 &empty_list) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, empty_list.entry_count == 0 &&
                                sxml_schema_get_list(components, empty_list).index_count == 0);

    const sxml_namespace_binding bindings[] = {
        {.namespace_prefix = {.index = 0},
         .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML_SCHEMA)}};
    sxml_schema_namespace_binding_list binding_list = {.first_binding_index = 0,
                                                       .binding_count = 0};
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_namespace_bindings(
                                 components,
                                 (sxml_namespace_binding_view){.namespace_bindings = bindings,
                                                               .namespace_binding_count = 1},
                                 &binding_list) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_schema_get_namespace_bindings(components, binding_list)
                                    .namespace_bindings[0]
                                    .namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA);

    /* Blocks never move and go with the components. */
    void *block = NULL;
    SXML_TEST_REQUIRE(state, sxml_schema_components_allocate_block(
                                 components,
                                 (sxml_allocation_request){.byte_count = 100, .alignment = 16},
                                 &block) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, ((uintptr_t)block % 16U) == 0);
    memset(block, 0xA5, 100);
    SXML_TEST_EXPECT(state, sxml_schema_components_allocate_block(
                                components,
                                (sxml_allocation_request){.byte_count = 8, .alignment = 3},
                                &block) == SXML_STATUS_INVALID_ARGUMENT);

    /* Sealed components do not grow. */
    sxml_schema_components_seal(components);
    SXML_TEST_EXPECT(state, add_entry(&fixture, SXML_SCHEMA_TABLE_FACET, &facet) == 0);
    SXML_TEST_EXPECT(state, sxml_schema_components_edit_entry(
                                components, (sxml_schema_entry_key){
                                                .table = SXML_SCHEMA_TABLE_PARTICLE,
                                                .index = 3}) == NULL);
    fixture_close(&fixture);
}

static void test_name_index(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    sxml_schema_components *components = fixture.components;
    char local_name[16];
    for (uint32_t name_number = 0; name_number < 500; name_number += 1) {
        (void)snprintf(local_name, sizeof(local_name), "e%u", (unsigned)name_number);
        uint32_t existing_component = 99;
        SXML_TEST_REQUIRE(
            state, sxml_schema_components_enter_name(
                       components,
                       &(sxml_schema_name_entry){
                           .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                           .name = intern_name(&fixture, (name_request){SXML_NAMESPACE_NONE,
                                                                        local_name}),
                           .component = name_number + 1U},
                       &existing_component) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, existing_component == 0);
    }
    SXML_TEST_EXPECT(state, find_global(&fixture, SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                        (name_request){SXML_NAMESPACE_NONE, "e321"}) == 322);
    /* Symbol spaces are distinct (2.6). */
    SXML_TEST_EXPECT(state, find_global(&fixture, SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                        (name_request){SXML_NAMESPACE_NONE, "e321"}) == 0);

    const sxml_name_identifier seventh_name =
        intern_name(&fixture, (name_request){SXML_NAMESPACE_NONE, "seventh_name"});
    uint32_t existing_component = 0;
    SXML_TEST_REQUIRE(state, sxml_schema_components_enter_name(
                                 components,
                                 &(sxml_schema_name_entry){
                                     .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                     .name = seventh_name,
                                     .component = 1000},
                                 &existing_component) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, existing_component == 8);
    SXML_TEST_REQUIRE(state, sxml_schema_components_replace_name(
                                 components,
                                 &(sxml_schema_name_entry){
                                     .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                     .name = seventh_name,
                                     .component = 1000}) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, find_global(&fixture, SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                        (name_request){SXML_NAMESPACE_NONE, "seventh_name"}) == 1000);
    SXML_TEST_EXPECT(state, sxml_schema_components_replace_name(
                                components,
                                &(sxml_schema_name_entry){
                                    .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_MODEL_GROUP_DEFINITION,
                                    .name = seventh_name,
                                    .component = 1}) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, sxml_schema_components_enter_name(
                                components,
                                &(sxml_schema_name_entry){
                                    .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_NONE,
                                    .name = seventh_name,
                                    .component = 1},
                                &existing_component) == SXML_STATUS_INVALID_ARGUMENT);
    fixture_close(&fixture);
}

/* A report sink that counts and keeps the last constraint name. */
typedef struct counting_sink {
    size_t report_count;
    char last_code[64];
} counting_sink;

static sxml_status receive_report(void *state, const sxml_schema_report *report) {
    counting_sink *sink = state;
    sink->report_count += 1;
    const size_t byte_count = report->code.local_name.byte_count < sizeof(sink->last_code) - 1U
                                  ? report->code.local_name.byte_count
                                  : sizeof(sink->last_code) - 1U;
    memcpy(sink->last_code, report->code.local_name.bytes, byte_count);
    sink->last_code[byte_count] = '\0';
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations counting_sink_operations = {
    .receive = receive_report,
};

static void test_reports(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    sxml_schema_text uri = {.byte_offset = 0, .byte_count = 0};
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_text(
                                 fixture.components, sxml_byte_view_from_c_string("file:/po.xsd"),
                                 &uri) == SXML_STATUS_OK);
    const sxml_schema_document_entry document = {
        .document_uri = uri, .configured_uri = sxml_byte_view_from_c_string("po.xsd")};
    SXML_TEST_REQUIRE(state, add_entry(&fixture, SXML_SCHEMA_TABLE_DOCUMENT, &document) == 1);
    const sxml_schema_report_request request = {
        .constraint_name = SXML_BYTE_VIEW_FROM_LITERAL("src-resolve"),
        .description = SXML_BYTE_VIEW_FROM_LITERAL("a QName names no component"),
        .detailed_description = SXML_BYTE_VIEW_FROM_LITERAL("no type named 'USAddress'"),
        .position = {.document = {.value = 1}, .line_number = 11, .column_number = 7}};

    /* The null sink: the first error stops, the record has it. */
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    const sxml_schema_configuration null_sink_configuration = {
        .processor = fixture.processor, .error_record = &error_record};
    sxml_schema_compilation compilation = {.configuration = &null_sink_configuration,
                                           .components = fixture.components,
                                           .error_count = 0};
    SXML_TEST_EXPECT(state, sxml_schema_compilation_report(&compilation, &request) ==
                                SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, compilation.error_count == 1);
    SXML_TEST_EXPECT(state, error_record.code.namespace_uri.byte_count == 0);
    SXML_TEST_EXPECT(state, error_record.code.local_name.byte_count == 11 &&
                                memcmp(error_record.code.local_name.bytes, "src-resolve", 11) == 0);
    SXML_TEST_EXPECT(state, error_record.diagnostic.location.line_number == 11);
    SXML_TEST_EXPECT(state, error_record.diagnostic.location.source_name.byte_count == 6);

    /* A sink receives every error; the record keeps the first. */
    counting_sink sink = {.report_count = 0, .last_code = {0}};
    memset(&error_record, 0, sizeof(error_record));
    const sxml_schema_configuration sink_configuration = {
        .processor = fixture.processor,
        .report_sink = {.state = &sink, .operations = &counting_sink_operations},
        .error_record = &error_record};
    compilation = (sxml_schema_compilation){.configuration = &sink_configuration,
                                            .components = fixture.components,
                                            .error_count = 0};
    SXML_TEST_EXPECT(state, sxml_schema_compilation_report(&compilation, &request) ==
                                SXML_STATUS_OK);
    const sxml_schema_report_request second_request = {
        .constraint_name = SXML_BYTE_VIEW_FROM_LITERAL("cos-nonambig"),
        .description = SXML_BYTE_VIEW_FROM_LITERAL("two particles compete"),
        .detailed_description = {.bytes = NULL, .byte_count = 0},
        .position = {.document = {.value = 1}, .line_number = 12, .column_number = 3}};
    SXML_TEST_EXPECT(state, sxml_schema_compilation_report(&compilation, &second_request) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sink.report_count == 2 && compilation.error_count == 2);
    SXML_TEST_EXPECT(state, strcmp(sink.last_code, "cos-nonambig") == 0);
    SXML_TEST_EXPECT(state, error_record.diagnostic.location.line_number == 11);
    fixture_close(&fixture);
}

/* Example 2 of src/schema_internal.h, lines 5 to 16 and 27 to 28, as
   RESOLVE leaves them: Expression, Operation, expression and sum. */
static void test_components_by_hand(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    sxml_schema_components *components = fixture.components;
    sxml_namespace_uri_identifier expression_namespace = {.index = 0};
    SXML_TEST_REQUIRE(state, sxml_name_pool_intern_namespace_uri(
                                 sxml_processor_get_name_pool(fixture.processor),
                                 sxml_byte_view_from_c_string("urn:example:expression"),
                                 &expression_namespace) == SXML_STATUS_OK);
    sxml_name_identifier names[4] = {{0}, {0}, {0}, {0}};
    const char *const local_names[4] = {"Expression", "Operation", "expression", "sum"};
    for (size_t name_index = 0; name_index < 4; name_index += 1) {
        SXML_TEST_REQUIRE(
            state, sxml_name_pool_intern_name(
                       sxml_processor_get_name_pool(fixture.processor),
                       &(sxml_name_request){.namespace_uri = expression_namespace,
                                            .local_name = sxml_byte_view_from_c_string(
                                                local_names[name_index])},
                       &names[name_index]) == SXML_STATUS_OK);
    }
    const sxml_schema_type_definition expression_type = {
        .variety = SXML_TYPE_VARIETY_COMPLEX,
        .type_name = names[0],
        .is_abstract = true,
        .base_type = {.value = SXML_TYPE_XS_ANY_TYPE},
        .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
        .content_type_variety = SXML_CONTENT_TYPE_VARIETY_EMPTY};
    SXML_TEST_REQUIRE(state, add_entry(&fixture, SXML_SCHEMA_TABLE_TYPE_DEFINITION,
                                       &expression_type) == 56);
    const sxml_schema_element_declaration head = {
        .element_name = names[2],
        .scope_variety = SXML_SCHEMA_SCOPE_GLOBAL,
        .type_definition = {.value = 56},
        .is_abstract = true};
    SXML_TEST_REQUIRE(state,
                      add_entry(&fixture, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, &head) == 1);
    const sxml_schema_particle head_particle = {
        .minimum_occurrences = 1,
        .maximum_occurrences = SXML_SCHEMA_UNBOUNDED,
        .term_kind = SXML_SCHEMA_TERM_ELEMENT_DECLARATION,
        .term = 1};
    SXML_TEST_REQUIRE(state,
                      add_entry(&fixture, SXML_SCHEMA_TABLE_PARTICLE, &head_particle) == 3);
    const uint32_t head_particle_index = 3;
    sxml_schema_model_group sequence = {.compositor = SXML_SCHEMA_COMPOSITOR_SEQUENCE};
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_list(
                                 components,
                                 (sxml_schema_index_view){.indexes = &head_particle_index,
                                                          .index_count = 1},
                                 &sequence.particles) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, add_entry(&fixture, SXML_SCHEMA_TABLE_MODEL_GROUP, &sequence) == 2);
    const sxml_schema_particle content_particle = {
        .minimum_occurrences = 1,
        .maximum_occurrences = 1,
        .term_kind = SXML_SCHEMA_TERM_MODEL_GROUP,
        .term = 2};
    SXML_TEST_REQUIRE(state,
                      add_entry(&fixture, SXML_SCHEMA_TABLE_PARTICLE, &content_particle) == 4);
    const sxml_schema_type_definition operation_type = {
        .variety = SXML_TYPE_VARIETY_COMPLEX,
        .type_name = names[1],
        .base_type = {.value = 56},
        .derivation_method = SXML_SCHEMA_DERIVATION_EXTENSION,
        .explicit_content = {.value = 4},
        .content_type_variety = SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY,
        .particle = {.value = 4}};
    SXML_TEST_REQUIRE(state, add_entry(&fixture, SXML_SCHEMA_TABLE_TYPE_DEFINITION,
                                       &operation_type) == 57);
    const uint32_t head_index = 1;
    sxml_schema_element_declaration member = {
        .element_name = names[3],
        .scope_variety = SXML_SCHEMA_SCOPE_GLOBAL,
        .type_definition = {.value = 57}};
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_list(
                                 components,
                                 (sxml_schema_index_view){.indexes = &head_index,
                                                          .index_count = 1},
                                 &member.substitution_group_affiliations) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      add_entry(&fixture, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, &member) == 2);
    /* RESOLVE's substitution group members, written in place. */
    const uint32_t member_index = 2;
    sxml_schema_element_declaration *edited_head = sxml_schema_components_edit_entry(
        components,
        (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, .index = 1});
    SXML_TEST_REQUIRE(state, edited_head != NULL);
    sxml_schema_list members = {.first_entry_index = 0, .entry_count = 0};
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_list(
                                 components,
                                 (sxml_schema_index_view){.indexes = &member_index,
                                                          .index_count = 1},
                                 &members) == SXML_STATUS_OK);
    edited_head->substitution_group_members = members;

    /* Walk Operation's content to the head and its member. */
    const sxml_schema_type_definition *operation = sxml_schema_get_type_definition(
        components, (sxml_schema_type_definition_index){.value = 57});
    const sxml_schema_particle *particle = sxml_schema_get_particle(components, operation->particle);
    const sxml_schema_model_group *group = sxml_schema_get_model_group(
        components, (sxml_schema_model_group_index){.value = particle->term});
    const sxml_schema_particle *element_particle = sxml_schema_get_particle(
        components, (sxml_schema_particle_index){
                        .value = sxml_schema_get_list(components, group->particles).indexes[0]});
    const sxml_schema_element_declaration *found_head = sxml_schema_get_element_declaration(
        components, (sxml_schema_element_declaration_index){.value = element_particle->term});
    SXML_TEST_EXPECT(state, found_head->element_name.index == names[2].index);
    const sxml_schema_index_view found_members =
        sxml_schema_get_list(components, found_head->substitution_group_members);
    SXML_TEST_REQUIRE(state, found_members.index_count == 1);
    const sxml_schema_element_declaration *found_member = sxml_schema_get_element_declaration(
        components, (sxml_schema_element_declaration_index){.value = found_members.indexes[0]});
    SXML_TEST_EXPECT(state, found_member->type_definition.value == 57);
    SXML_TEST_EXPECT(
        state,
        sxml_schema_get_type_definition(components, found_member->type_definition)->base_type.value ==
            56);
    fixture_close(&fixture);
}

static const sxml_test_case schema_components_test_cases[] = {
    {.name = "builtin_components", .function = test_builtin_components},
    {.name = "tables_lists_and_texts", .function = test_tables_lists_and_texts},
    {.name = "name_index", .function = test_name_index},
    {.name = "reports", .function = test_reports},
    {.name = "components_by_hand", .function = test_components_by_hand},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "schema_components",
            .cases = schema_components_test_cases,
            .case_count = SXML_TEST_COUNT(schema_components_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
