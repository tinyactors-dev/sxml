/*
 * Unit tests for src/schema_complex_type.c and src/schema_content_model.c:
 * the COMPLEX_TYPES stage on components built by hand (the three schemas
 * of src/schema_internal.h's text form, and small cases of our own), the
 * content models it compiles, their checks and the matcher.
 *
 * Golden file: tests/schema_components_test.c (components built by hand
 * and read back), tests/regex_test.c (a compiled program run on inputs).
 */
#include "processor_internal.h"
#include "schema_internal.h"
#include "sxml/processor.h"
#include "test_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- Fixture */

typedef struct test_fixture {
    sxml_processor *processor;
    sxml_schema_components *components;
    sxml_namespace_uri_identifier target_namespace;
    sxml_error_record error_record;
    uint32_t maximum_state_count;
} test_fixture;

static bool fixture_open(test_fixture *fixture) {
    const sxml_allocator allocator = sxml_default_allocator();
    memset(fixture, 0, sizeof(*fixture));
    if (sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                              &fixture->processor) != SXML_STATUS_OK) {
        return false;
    }
    if (sxml_schema_components_create(
            allocator, &(sxml_schema_components_configuration){.processor = fixture->processor},
            &fixture->components) != SXML_STATUS_OK) {
        return false;
    }
    return sxml_schema_components_add_builtin_components(fixture->components) == SXML_STATUS_OK;
}

static void fixture_close(test_fixture *fixture) {
    sxml_schema_components_destroy(fixture->components);
    sxml_processor_destroy(fixture->processor);
}

/* Binds prefix to namespace_uri for the text form and makes it the
   namespace of the names fixture_name gives. */
typedef struct namespace_request {
    const char *prefix;
    const char *namespace_uri;
} namespace_request;

static void fixture_bind(test_fixture *fixture, namespace_request request) {
    sxml_name_pool *name_pool = fixture->processor->name_pool;
    sxml_namespace_binding binding = {.namespace_prefix = {0}, .namespace_uri = {0}};
    if (sxml_name_pool_intern_namespace_uri(name_pool,
                                            sxml_byte_view_from_c_string(request.namespace_uri),
                                            &binding.namespace_uri) != SXML_STATUS_OK ||
        sxml_name_pool_intern_namespace_prefix(name_pool,
                                               sxml_byte_view_from_c_string(request.prefix),
                                               &binding.namespace_prefix) != SXML_STATUS_OK) {
        return;
    }
    sxml_schema_namespace_binding_list list = {.first_binding_index = 0, .binding_count = 0};
    if (sxml_schema_components_add_namespace_bindings(
            fixture->components,
            (sxml_namespace_binding_view){.namespace_bindings = &binding,
                                          .namespace_binding_count = 1},
            &list) != SXML_STATUS_OK) {
        return;
    }
    fixture->target_namespace = binding.namespace_uri;
}

static sxml_name_identifier fixture_name(const test_fixture *fixture, const char *local_name) {
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_name(
            fixture->processor->name_pool,
            &(sxml_name_request){.namespace_uri = fixture->target_namespace,
                                 .local_name = sxml_byte_view_from_c_string(local_name)},
            &name) != SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return name;
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

typedef struct index_list {
    uint32_t indexes[8];
    size_t index_count;
} index_list;

static sxml_schema_list fixture_list(test_fixture *fixture, index_list indexes) {
    sxml_schema_list list = {.first_entry_index = 0, .entry_count = 0};
    if (sxml_schema_components_add_list(
            fixture->components,
            (sxml_schema_index_view){.indexes = indexes.indexes,
                                     .index_count = indexes.index_count},
            &list) != SXML_STATUS_OK) {
        return (sxml_schema_list){0, 0};
    }
    return list;
}

/* --- Components by hand: what READ and RESOLVE would write. --- */

typedef struct element_request {
    const char *local_name;
    uint32_t type_definition;
    bool is_global;
    bool is_abstract;
} element_request;

static uint32_t add_element(test_fixture *fixture, element_request request) {
    const sxml_schema_element_declaration declaration = {
        .element_name = fixture_name(fixture, request.local_name),
        .scope_variety = request.is_global ? SXML_SCHEMA_SCOPE_GLOBAL : SXML_SCHEMA_SCOPE_LOCAL,
        .type_definition = {.value = request.type_definition},
        .is_abstract = request.is_abstract};
    const uint32_t index = add_entry(fixture, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, &declaration);
    if (request.is_global) {
        uint32_t existing = 0;
        if (sxml_schema_components_enter_name(
                fixture->components,
                &(sxml_schema_name_entry){.symbol_space =
                                              SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                          .name = declaration.element_name,
                                          .component = index},
                &existing) != SXML_STATUS_OK) {
            return 0;
        }
    }
    return index;
}

static sxml_schema_element_declaration *edit_element(test_fixture *fixture, uint32_t index) {
    return sxml_schema_components_edit_entry(
        fixture->components,
        (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, .index = index});
}

typedef struct particle_request {
    uint64_t minimum_occurrences;
    uint64_t maximum_occurrences;
    sxml_schema_term_kind term_kind;
    uint32_t term;
} particle_request;

static uint32_t add_particle(test_fixture *fixture, particle_request request) {
    const sxml_schema_particle particle = {.position = {.line_number = 1, .column_number = 1},
                                           .minimum_occurrences = request.minimum_occurrences,
                                           .maximum_occurrences = request.maximum_occurrences,
                                           .term_kind = request.term_kind,
                                           .term = request.term};
    return add_entry(fixture, SXML_SCHEMA_TABLE_PARTICLE, &particle);
}

/* A particle exactly once of an element declaration. */
static uint32_t add_element_particle(test_fixture *fixture, uint32_t element) {
    return add_particle(fixture,
                        (particle_request){1, 1, SXML_SCHEMA_TERM_ELEMENT_DECLARATION, element});
}

typedef struct group_request {
    sxml_schema_compositor compositor;
    index_list particles;
} group_request;

static uint32_t add_group(test_fixture *fixture, group_request request) {
    const sxml_schema_model_group group = {.compositor = request.compositor,
                                           .particles = fixture_list(fixture, request.particles)};
    return add_entry(fixture, SXML_SCHEMA_TABLE_MODEL_GROUP, &group);
}

typedef struct complex_type_request {
    const char *local_name;
    uint32_t base_type;
    sxml_schema_derivation derivation_method;
    uint32_t explicit_content;
    bool is_mixed;
} complex_type_request;

static uint32_t add_complex_type(test_fixture *fixture, complex_type_request request) {
    const sxml_schema_type_definition definition = {
        .variety = SXML_TYPE_VARIETY_COMPLEX,
        .type_name = request.local_name != NULL ? fixture_name(fixture, request.local_name)
                                                : (sxml_name_identifier){0},
        .position = {.line_number = 1, .column_number = 1},
        .base_type = {.value = request.base_type != 0 ? request.base_type : SXML_TYPE_XS_ANY_TYPE},
        .derivation_method = request.derivation_method != SXML_SCHEMA_DERIVATION_NONE
                                 ? request.derivation_method
                                 : SXML_SCHEMA_DERIVATION_RESTRICTION,
        .explicit_content = {.value = request.explicit_content},
        .is_mixed = request.is_mixed};
    return add_entry(fixture, SXML_SCHEMA_TABLE_TYPE_DEFINITION, &definition);
}

static sxml_schema_type_definition *edit_type(test_fixture *fixture, uint32_t index) {
    return sxml_schema_components_edit_entry(
        fixture->components,
        (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = index});
}

/* Runs COMPLEX_TYPES with the null report sink. */
static sxml_status fixture_compile(test_fixture *fixture) {
    const sxml_schema_configuration configuration = {.processor = fixture->processor,
                                                     .maximum_content_model_state_count =
                                                         fixture->maximum_state_count,
                                                     .error_record = &fixture->error_record};
    sxml_schema_compilation compilation = {.configuration = &configuration,
                                           .components = fixture->components};
    return sxml_schema_compile_complex_types(&compilation);
}

static bool error_code_is(const test_fixture *fixture, const char *code) {
    const sxml_byte_view local_name = fixture->error_record.code.local_name;
    return local_name.byte_count == strlen(code) &&
           memcmp(local_name.bytes, code, local_name.byte_count) == 0;
}

/* The text form of a type's content model against expected. */
typedef struct text_check {
    uint32_t type_definition;
    const char *expected_text;
} text_check;

static bool content_model_text_is(const test_fixture *fixture, text_check check) {
    const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
        fixture->components, (sxml_schema_type_definition_index){.value = check.type_definition});
    if (definition->content_model == NULL) {
        printf("type %u has no content model\n", check.type_definition);
        return false;
    }
    uint8_t text[4096];
    size_t byte_count = 0;
    if (sxml_schema_content_model_write_text(
            fixture->components,
            &(sxml_schema_content_model_text_request){
                .content_model = definition->content_model,
                .output = {.bytes = text, .byte_count = sizeof(text)}},
            &byte_count) != SXML_STATUS_OK) {
        return false;
    }
    const bool is_equal = byte_count == strlen(check.expected_text) &&
                          memcmp(text, check.expected_text, byte_count) == 0;
    if (!is_equal) {
        printf("expected:\n%s\nactual:\n%.*s\n", check.expected_text, (int)byte_count,
               (const char *)text);
    }
    return is_equal;
}

/* --- The matcher on a sequence of names. --- */

#define MAXIMUM_TEST_COUNTERS 16U

typedef struct match_run {
    uint32_t type_definition;
    /* Local names, space separated; "*" repeats the previous one up to
       the count after it, as "a*5". */
    const char *children;
} match_run;

/* True when the children match and the content may end there. */
static bool content_accepts(test_fixture *fixture, match_run run) {
    const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
        fixture->components, (sxml_schema_type_definition_index){.value = run.type_definition});
    if (definition->content_model == NULL) {
        return false;
    }
    uint64_t counter_values[MAXIMUM_TEST_COUNTERS];
    sxml_schema_content_model_cursor cursor = {
        .content_model = definition->content_model,
        .counter_values = counter_values,
        .counter_count = sxml_schema_content_model_get_counter_count(definition->content_model)};
    if (cursor.counter_count > MAXIMUM_TEST_COUNTERS) {
        return false;
    }
    sxml_schema_content_model_start(&cursor);
    const char *next = run.children;
    while (*next != '\0') {
        while (*next == ' ') {
            next += 1;
        }
        if (*next == '\0') {
            break;
        }
        char local_name[64];
        size_t length = 0;
        while (next[length] != '\0' && next[length] != ' ' && next[length] != '*' &&
               length + 1 < sizeof(local_name)) {
            local_name[length] = next[length];
            length += 1;
        }
        local_name[length] = '\0';
        next += length;
        unsigned long repeat_count = 1;
        if (*next == '*') {
            char *end = NULL;
            repeat_count = strtoul(next + 1, &end, 10);
            next = end;
        }
        for (unsigned long repeat = 0; repeat < repeat_count; repeat += 1) {
            sxml_schema_content_model_match match = {.match_kind = SXML_SCHEMA_MATCH_NONE};
            if (sxml_schema_content_model_step(
                    fixture->components,
                    &(sxml_schema_content_model_step_request){
                        .cursor = &cursor, .element_name = fixture_name(fixture, local_name)},
                    &match) != SXML_STATUS_OK ||
                match.match_kind == SXML_SCHEMA_MATCH_NONE) {
                return false;
            }
        }
    }
    return sxml_schema_content_model_can_end(&cursor);
}

/* ------------------------------------------- The header's three schemas */

#define UNBOUNDED SXML_SCHEMA_UNBOUNDED
#define ELEMENT SXML_SCHEMA_TERM_ELEMENT_DECLARATION
#define GROUP SXML_SCHEMA_TERM_MODEL_GROUP
#define WILDCARD SXML_SCHEMA_TERM_WILDCARD
#define SEQUENCE SXML_SCHEMA_COMPOSITOR_SEQUENCE
#define CHOICE SXML_SCHEMA_COMPOSITOR_CHOICE

/* Example 1: the purchase order, numbered as the reader numbers it. */
static void test_purchase_order(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* Types 56 to 59 (60 and 61 are simple: built-ins stand in). */
    SXML_TEST_REQUIRE(
        state, add_complex_type(&fixture,
                                (complex_type_request){"PurchaseOrderType", 0, 0, 3, false}) == 56);
    SXML_TEST_REQUIRE(state, add_complex_type(&fixture, (complex_type_request){"USAddress", 0, 0, 8,
                                                                               false}) == 57);
    SXML_TEST_REQUIRE(
        state, add_complex_type(&fixture, (complex_type_request){"Items", 0, 0, 12, false}) == 58);
    SXML_TEST_REQUIRE(
        state, add_complex_type(&fixture, (complex_type_request){NULL, 0, 0, 14, false}) == 59);
    const struct {
        const char *local_name;
        uint32_t type_definition;
        bool is_global;
    } elements[] = {{"purchaseOrder", 56, true},
                    {"comment", SXML_TYPE_XS_STRING, true},
                    {"shipTo", 57, false},
                    {"billTo", 57, false},
                    {"items", 58, false},
                    {"name", SXML_TYPE_XS_STRING, false},
                    {"street", SXML_TYPE_XS_STRING, false},
                    {"zip", SXML_TYPE_XS_DECIMAL, false},
                    {"item", 59, false},
                    {"productName", SXML_TYPE_XS_STRING, false},
                    {"quantity", SXML_TYPE_XS_POSITIVE_INTEGER, false}};
    for (size_t element = 0; element < sizeof(elements) / sizeof(elements[0]); element += 1) {
        SXML_TEST_REQUIRE(state,
                          add_element(&fixture, (element_request){elements[element].local_name,
                                                                  elements[element].type_definition,
                                                                  elements[element].is_global,
                                                                  false}) == element + 1);
    }
    /* Particles 3 to 17, model groups 2 to 5. */
    SXML_TEST_REQUIRE(state, add_particle(&fixture, (particle_request){1, 1, GROUP, 2}) == 3);
    add_element_particle(&fixture, 3);
    add_element_particle(&fixture, 4);
    add_particle(&fixture, (particle_request){0, 1, ELEMENT, 2});
    add_element_particle(&fixture, 5);
    add_particle(&fixture, (particle_request){1, 1, GROUP, 3});
    add_element_particle(&fixture, 6);
    add_element_particle(&fixture, 7);
    add_element_particle(&fixture, 8);
    add_particle(&fixture, (particle_request){1, 1, GROUP, 4});
    add_particle(&fixture, (particle_request){0, UNBOUNDED, ELEMENT, 9});
    add_particle(&fixture, (particle_request){1, 1, GROUP, 5});
    add_element_particle(&fixture, 10);
    add_element_particle(&fixture, 11);
    SXML_TEST_REQUIRE(state, add_particle(&fixture, (particle_request){0, 1, ELEMENT, 2}) == 17);
    SXML_TEST_REQUIRE(state,
                      add_group(&fixture, (group_request){SEQUENCE, {{4, 5, 6, 7}, 4}}) == 2);
    add_group(&fixture, (group_request){SEQUENCE, {{9, 10, 11}, 3}});
    add_group(&fixture, (group_request){SEQUENCE, {{13}, 1}});
    SXML_TEST_REQUIRE(state,
                      add_group(&fixture, (group_request){SEQUENCE, {{15, 16, 17}, 3}}) == 5);

    SXML_TEST_REQUIRE(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state,
        content_model_text_is(
            &fixture, (text_check){56, "content-model of type 56 states 5 counters 0\n"
                                       "  state 1: shipTo element 3 -> 2\n"
                                       "  state 2: billTo element 4 -> 3\n"
                                       "  state 3: comment element 2 -> 4, items element 5 -> 5\n"
                                       "  state 4: items element 5 -> 5\n"
                                       "  state 5 final\n"}));
    SXML_TEST_EXPECT(state,
                     content_model_text_is(
                         &fixture, (text_check){57, "content-model of type 57 states 4 counters 0\n"
                                                    "  state 1: name element 6 -> 2\n"
                                                    "  state 2: street element 7 -> 3\n"
                                                    "  state 3: zip element 8 -> 4\n"
                                                    "  state 4 final\n"}));
    SXML_TEST_EXPECT(state,
                     content_model_text_is(
                         &fixture, (text_check){58, "content-model of type 58 states 2 counters 0\n"
                                                    "  state 1 final: item element 9 -> 2\n"
                                                    "  state 2 final: item element 9 -> 2\n"}));
    SXML_TEST_EXPECT(state,
                     content_model_text_is(
                         &fixture, (text_check){59, "content-model of type 59 states 4 counters 0\n"
                                                    "  state 1: productName element 10 -> 2\n"
                                                    "  state 2: quantity element 11 -> 3\n"
                                                    "  state 3 final: comment element 2 -> 4\n"
                                                    "  state 4 final\n"}));
    /* xs:anyType's content (3.4.7): any element, laxly, any number. */
    SXML_TEST_EXPECT(
        state, content_model_text_is(&fixture,
                                     (text_check){1, "content-model of type 1 states 2 counters 0\n"
                                                     "  state 1 final: wildcard 1 -> 2\n"
                                                     "  state 2 final: wildcard 1 -> 2\n"}));
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){1, "x y shipTo"}));
    /* Properties and registration. */
    const sxml_schema_type_definition *purchase_order = sxml_schema_get_type_definition(
        fixture.components, (sxml_schema_type_definition_index){.value = 56});
    SXML_TEST_EXPECT(state, purchase_order->content_type_variety ==
                                SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY);
    SXML_TEST_EXPECT(state, purchase_order->particle.value == 3);
    SXML_TEST_EXPECT(state, purchase_order->registered_type.index != SXML_TYPE_NONE);
    SXML_TEST_EXPECT(state,
                     content_accepts(&fixture, (match_run){56, "shipTo billTo comment items"}));
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){56, "shipTo billTo items"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){56, "shipTo items"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){56, "shipTo billTo"}));
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){58, "item*40"}));
    fixture_close(&fixture);
}

/* Example 2: a recursive substitution group. */
static void test_substitution_group(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    fixture_bind(&fixture, (namespace_request){"e", "urn:example:expression"});
    SXML_TEST_REQUIRE(state, add_complex_type(&fixture, (complex_type_request){"Expression", 0, 0,
                                                                               0, false}) == 56);
    SXML_TEST_REQUIRE(
        state, add_complex_type(&fixture, (complex_type_request){"Operation", 56,
                                                                 SXML_SCHEMA_DERIVATION_EXTENSION,
                                                                 3, false}) == 57);
    SXML_TEST_REQUIRE(
        state, add_complex_type(&fixture,
                                (complex_type_request){NULL, 56, SXML_SCHEMA_DERIVATION_EXTENSION,
                                                       0, false}) == 58);
    edit_type(&fixture, 56)->is_abstract = true;
    SXML_TEST_REQUIRE(state,
                      add_element(&fixture, (element_request){"expression", 56, true, true}) == 1);
    add_element(&fixture, (element_request){"number", 58, true, false});
    add_element(&fixture, (element_request){"sum", 57, true, false});
    add_element(&fixture, (element_request){"product", 57, true, false});
    SXML_TEST_REQUIRE(state,
                      add_element(&fixture, (element_request){"difference", 57, true, false}) == 5);
    edit_element(&fixture, 1)->substitution_group_members =
        fixture_list(&fixture, (index_list){{2, 3, 4, 5}, 4});
    edit_element(&fixture, 2)->substitution_group_affiliations =
        fixture_list(&fixture, (index_list){{1}, 1});
    edit_element(&fixture, 3)->substitution_group_affiliations =
        fixture_list(&fixture, (index_list){{1}, 1});
    edit_element(&fixture, 3)->substitution_group_members =
        fixture_list(&fixture, (index_list){{5}, 1});
    edit_element(&fixture, 4)->substitution_group_affiliations =
        fixture_list(&fixture, (index_list){{1}, 1});
    edit_element(&fixture, 5)->substitution_group_affiliations =
        fixture_list(&fixture, (index_list){{3}, 1});
    SXML_TEST_REQUIRE(state, add_particle(&fixture, (particle_request){1, 1, GROUP, 2}) == 3);
    add_particle(&fixture, (particle_request){1, UNBOUNDED, ELEMENT, 1});
    SXML_TEST_REQUIRE(state, add_group(&fixture, (group_request){SEQUENCE, {{4}, 1}}) == 2);

    SXML_TEST_REQUIRE(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, content_model_text_is(
                   &fixture, (text_check){57, "content-model of type 57 states 2 counters 0\n"
                                              "  state 1: e:expression element 1 -> 2\n"
                                              "  state 2 final: e:expression element 1 -> 2\n"}));
    const sxml_schema_type_definition *expression = sxml_schema_get_type_definition(
        fixture.components, (sxml_schema_type_definition_index){.value = 56});
    SXML_TEST_EXPECT(state, expression->content_type_variety == SXML_CONTENT_TYPE_VARIETY_EMPTY);
    /* A child e:difference takes element 1's transition and gives 5. */
    const sxml_schema_type_definition *operation = sxml_schema_get_type_definition(
        fixture.components, (sxml_schema_type_definition_index){.value = 57});
    sxml_schema_content_model_cursor cursor = {
        .content_model = operation->content_model, .counter_values = NULL, .counter_count = 0};
    sxml_schema_content_model_start(&cursor);
    sxml_schema_content_model_match match = {.match_kind = SXML_SCHEMA_MATCH_NONE};
    SXML_TEST_EXPECT(
        state, sxml_schema_content_model_step(
                   fixture.components,
                   &(sxml_schema_content_model_step_request){
                       .cursor = &cursor, .element_name = fixture_name(&fixture, "difference")},
                   &match) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, match.match_kind == SXML_SCHEMA_MATCH_ELEMENT_DECLARATION);
    SXML_TEST_EXPECT(state, match.element_declaration.value == 5);
    SXML_TEST_EXPECT(state, sxml_schema_content_model_can_end(&cursor));
    SXML_TEST_EXPECT(state,
                     content_accepts(&fixture, (match_run){57, "number sum product difference"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){57, ""}));
    /* Blocking substitution leaves the head alone. */
    edit_element(&fixture, 1)->disallowed_substitutions = SXML_SCHEMA_DERIVATION_SUBSTITUTION;
    edit_type(&fixture, 57)->content_model = NULL;
    SXML_TEST_REQUIRE(
        state,
        sxml_schema_compile_content_model(
            &(sxml_schema_compilation){
                .configuration = &(sxml_schema_configuration){.processor = fixture.processor},
                .components = fixture.components},
            (sxml_schema_type_definition_index){.value = 57}) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){57, "difference"}));
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){57, "expression"}));
    fixture_close(&fixture);
}

/* Example 3: open content and a counter. */
static void test_open_content_and_counter(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    fixture_bind(&fixture, (namespace_request){"m", "urn:example:message"});
    /* Type 56 is the simple type Percent: decimal stands in. */
    const sxml_schema_type_definition percent = {.variety = SXML_TYPE_VARIETY_ATOMIC,
                                                 .type_name = fixture_name(&fixture, "Percent"),
                                                 .base_type = {SXML_TYPE_XS_DECIMAL},
                                                 .derivation_method =
                                                     SXML_SCHEMA_DERIVATION_RESTRICTION,
                                                 .registered_type = {SXML_TYPE_XS_DECIMAL}};
    SXML_TEST_REQUIRE(state,
                      add_entry(&fixture, SXML_SCHEMA_TABLE_TYPE_DEFINITION, &percent) == 56);
    SXML_TEST_REQUIRE(
        state, add_complex_type(&fixture, (complex_type_request){"Message", 0, 0, 3, false}) == 57);
    SXML_TEST_REQUIRE(
        state, add_complex_type(&fixture, (complex_type_request){"Urgent", 57,
                                                                 SXML_SCHEMA_DERIVATION_EXTENSION,
                                                                 6, false}) == 58);
    add_element(&fixture, (element_request){"subject", SXML_TYPE_XS_STRING, false, false});
    add_element(&fixture, (element_request){"line", SXML_TYPE_XS_STRING, false, false});
    add_element(&fixture, (element_request){"deadline", SXML_TYPE_XS_DATE_TIME, false, false});
    SXML_TEST_REQUIRE(state,
                      add_element(&fixture, (element_request){"message", 57, true, false}) == 4);
    SXML_TEST_REQUIRE(state, add_particle(&fixture, (particle_request){1, 1, GROUP, 2}) == 3);
    add_element_particle(&fixture, 1);
    add_particle(&fixture, (particle_request){1, 1000, ELEMENT, 2});
    add_particle(&fixture, (particle_request){1, 1, GROUP, 3});
    SXML_TEST_REQUIRE(state, add_element_particle(&fixture, 3) == 7);
    add_group(&fixture, (group_request){SEQUENCE, {{4, 5}, 2}});
    SXML_TEST_REQUIRE(state, add_group(&fixture, (group_request){SEQUENCE, {{7}, 1}}) == 3);
    /* Wildcard 3: ##other, skip; Message's open content. */
    const sxml_schema_wildcard other = {
        .namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT,
        .namespaces = fixture_list(&fixture, (index_list){{fixture.target_namespace.index, 0}, 2}),
        .process_contents = SXML_SCHEMA_PROCESS_CONTENTS_SKIP};
    SXML_TEST_REQUIRE(state, add_entry(&fixture, SXML_SCHEMA_TABLE_WILDCARD, &other) == 3);
    edit_type(&fixture, 57)->open_content_mode = SXML_SCHEMA_OPEN_CONTENT_SUFFIX;
    edit_type(&fixture, 57)->open_content_wildcard.value = 3;

    SXML_TEST_REQUIRE(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(
        state, content_model_text_is(
                   &fixture, (text_check){57, "content-model of type 57 states 4 counters 1\n"
                                              "  counter 1 1..1000\n"
                                              "  state 1: m:subject element 1 -> 2\n"
                                              "  state 2: m:line element 2 -> 3 [counter 1 = 1]\n"
                                              "  state 3 final if counter 1 >= 1: m:line element 2 "
                                              "-> 3 [counter 1 < 1000, counter 1 + 1]\n"
                                              "  state 4 final suffix\n"
                                              "  open-content suffix wildcard 3 -> 4\n"}));
    SXML_TEST_EXPECT(
        state, content_model_text_is(
                   &fixture,
                   (text_check){58, "content-model of type 58 states 5 counters 1\n"
                                    "  counter 1 1..1000\n"
                                    "  state 1: m:subject element 1 -> 2\n"
                                    "  state 2: m:line element 2 -> 3 [counter 1 = 1]\n"
                                    "  state 3: m:line element 2 -> 3 [counter 1 < 1000, counter 1 "
                                    "+ 1], m:deadline element 3 -> 4 [counter 1 >= 1]\n"
                                    "  state 4 final\n"
                                    "  state 5 final suffix\n"
                                    "  open-content suffix wildcard 3 -> 5\n"}));
    /* COMPLEX_TYPES synthesized particle 8 and model group 4 for Urgent. */
    const sxml_schema_type_definition *urgent = sxml_schema_get_type_definition(
        fixture.components, (sxml_schema_type_definition_index){.value = 58});
    SXML_TEST_EXPECT(state, urgent->particle.value == 8);
    SXML_TEST_EXPECT(state, urgent->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX);
    SXML_TEST_EXPECT(state, urgent->open_content_wildcard.value == 3);
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){57, "subject line*1000"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){57, "subject line*1001"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){57, "subject"}));
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){58, "subject line*3 deadline"}));
    /* Suffix open content: an element of another namespace once the
       content may end, and then only those. */
    const sxml_namespace_uri_identifier message_namespace = fixture.target_namespace;
    fixture_bind(&fixture, (namespace_request){"o", "urn:example:other"});
    const sxml_name_identifier other_name = fixture_name(&fixture, "note");
    fixture.target_namespace = message_namespace;
    uint64_t counter_values[1] = {0};
    const sxml_schema_type_definition *message = sxml_schema_get_type_definition(
        fixture.components, (sxml_schema_type_definition_index){.value = 57});
    sxml_schema_content_model_cursor cursor = {.content_model = message->content_model,
                                               .counter_values = counter_values,
                                               .counter_count = 1};
    sxml_schema_content_model_start(&cursor);
    const char *const names[] = {"subject", "line", NULL, NULL, "line"};
    const sxml_schema_match_kind expected[] = {
        SXML_SCHEMA_MATCH_ELEMENT_DECLARATION, SXML_SCHEMA_MATCH_ELEMENT_DECLARATION,
        SXML_SCHEMA_MATCH_WILDCARD, SXML_SCHEMA_MATCH_WILDCARD, SXML_SCHEMA_MATCH_NONE};
    for (size_t child = 0; child < sizeof(names) / sizeof(names[0]); child += 1) {
        sxml_schema_content_model_match match = {.match_kind = SXML_SCHEMA_MATCH_NONE};
        SXML_TEST_EXPECT(state, sxml_schema_content_model_step(
                                    fixture.components,
                                    &(sxml_schema_content_model_step_request){
                                        .cursor = &cursor,
                                        .element_name = names[child] != NULL
                                                            ? fixture_name(&fixture, names[child])
                                                            : other_name},
                                    &match) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, match.match_kind == expected[child]);
    }
    SXML_TEST_EXPECT(state, cursor.state == 4);
    sxml_schema_content_model_match expected_terms[4];
    sxml_schema_expected_term_buffer buffer = {.terms = expected_terms, .term_capacity = 4};
    sxml_schema_content_model_list_expected(fixture.components, &cursor, &buffer);
    SXML_TEST_EXPECT(state, buffer.term_count == 1 && expected_terms[0].wildcard.value == 3);
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){58, "subject line*3"}));
    fixture_close(&fixture);
}

/* ---------------------------------------------------------- Own cases */

/* A type whose content is one model group of element particles, each
   of its own element (a, b, c, ... by position). */
typedef struct simple_content_request {
    sxml_schema_compositor compositor;
    /* minOccurs and maxOccurs of each particle; elements are named by
       names[particle]. */
    const char *names[6];
    uint64_t minimum_occurrences[6];
    uint64_t maximum_occurrences[6];
    size_t particle_count;
} simple_content_request;

static uint32_t add_model_type(test_fixture *fixture, simple_content_request request) {
    index_list particles = {.index_count = request.particle_count};
    for (size_t entry = 0; entry < request.particle_count; entry += 1) {
        uint32_t element = 0;
        if (sxml_schema_components_find_name(
                fixture->components,
                (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                       .name = fixture_name(fixture, request.names[entry])},
                &element) == false) {
            element = add_element(
                fixture, (element_request){request.names[entry], SXML_TYPE_XS_STRING, true, false});
        }
        particles.indexes[entry] = add_particle(
            fixture, (particle_request){request.minimum_occurrences[entry],
                                        request.maximum_occurrences[entry], ELEMENT, element});
    }
    const uint32_t group = add_group(fixture, (group_request){request.compositor, particles});
    const uint32_t particle = add_particle(fixture, (particle_request){1, 1, GROUP, group});
    return add_complex_type(fixture, (complex_type_request){NULL, 0, 0, particle, false});
}

static void test_unique_particle_attribution(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* (a?, a) is ambiguous; (a{2}, a) is not (counts decide). */
    add_model_type(&fixture, (simple_content_request){SEQUENCE, {"a", "a"}, {0, 1}, {1, 1}, 2});
    SXML_TEST_EXPECT(state, fixture_compile(&fixture) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&fixture, "cos-nonambig"));
    fixture_close(&fixture);

    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    const uint32_t counted =
        add_model_type(&fixture, (simple_content_request){SEQUENCE, {"a", "a"}, {2, 1}, {2, 1}, 2});
    const uint32_t large = add_model_type(
        &fixture, (simple_content_request){SEQUENCE, {"a", "b"}, {20, 1}, {20, 1}, 2});
    SXML_TEST_REQUIRE(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){counted, "a a a"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){counted, "a a"}));
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){large, "a*20 b"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){large, "a*19 b"}));
    fixture_close(&fixture);

    /* (a | a): two particles of one declaration compete. */
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    add_model_type(&fixture, (simple_content_request){CHOICE, {"a", "a"}, {1, 1}, {1, 1}, 2});
    SXML_TEST_EXPECT(state, fixture_compile(&fixture) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&fixture, "cos-nonambig"));
    fixture_close(&fixture);

    /* (a | any): an element and a wildcard may compete; the element wins. */
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    const uint32_t element =
        add_element(&fixture, (element_request){"a", SXML_TYPE_XS_STRING, true, false});
    const uint32_t element_particle = add_element_particle(&fixture, element);
    const uint32_t wildcard_particle =
        add_particle(&fixture, (particle_request){1, 1, WILDCARD, 1});
    const uint32_t group =
        add_group(&fixture, (group_request){CHOICE, {{element_particle, wildcard_particle}, 2}});
    const uint32_t type = add_complex_type(
        &fixture,
        (complex_type_request){
            NULL, 0, 0, add_particle(&fixture, (particle_request){1, 1, GROUP, group}), false});
    SXML_TEST_REQUIRE(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
        fixture.components, (sxml_schema_type_definition_index){type});
    sxml_schema_content_model_cursor cursor = {.content_model = definition->content_model};
    sxml_schema_content_model_start(&cursor);
    sxml_schema_content_model_match match = {.match_kind = SXML_SCHEMA_MATCH_NONE};
    SXML_TEST_EXPECT(state, sxml_schema_content_model_step(
                                fixture.components,
                                &(sxml_schema_content_model_step_request){
                                    .cursor = &cursor, .element_name = fixture_name(&fixture, "a")},
                                &match) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, match.match_kind == SXML_SCHEMA_MATCH_ELEMENT_DECLARATION);
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){type, "zzz"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){type, "a a"}));
    fixture_close(&fixture);
}

static void test_counters(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* (a{1,20}){1,20}: the counters cannot tell which repeats, so the
       inner particle is expanded; 400 a's and no more. */
    const uint32_t element =
        add_element(&fixture, (element_request){"a", SXML_TYPE_XS_STRING, true, false});
    const uint32_t inner = add_particle(&fixture, (particle_request){1, 20, ELEMENT, element});
    const uint32_t group = add_group(&fixture, (group_request){SEQUENCE, {{inner}, 1}});
    const uint32_t outer = add_particle(&fixture, (particle_request){1, 20, GROUP, group});
    const uint32_t nested =
        add_complex_type(&fixture, (complex_type_request){NULL, 0, 0, outer, false});
    /* (b{2,5000}, c)* with an unbounded outer repetition. */
    const uint32_t b_element =
        add_element(&fixture, (element_request){"b", SXML_TYPE_XS_STRING, true, false});
    const uint32_t c_element =
        add_element(&fixture, (element_request){"c", SXML_TYPE_XS_STRING, true, false});
    const uint32_t b_particle =
        add_particle(&fixture, (particle_request){2, 5000, ELEMENT, b_element});
    const uint32_t c_particle = add_element_particle(&fixture, c_element);
    const uint32_t pair =
        add_group(&fixture, (group_request){SEQUENCE, {{b_particle, c_particle}, 2}});
    const uint32_t repeated = add_complex_type(
        &fixture,
        (complex_type_request){
            NULL, 0, 0, add_particle(&fixture, (particle_request){0, UNBOUNDED, GROUP, pair}),
            false});
    SXML_TEST_REQUIRE(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){nested, "a"}));
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){nested, "a*400"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){nested, "a*401"}));
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){repeated, ""}));
    SXML_TEST_EXPECT(state,
                     content_accepts(&fixture, (match_run){repeated, "b*2 c b*5000 c b*3 c"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){repeated, "b c"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){repeated, "b*5001 c"}));
    fixture_close(&fixture);

    /* A state limit: counting rescues (a{3}, b{3}, c{3}) at 8 states, a
       choice of six names needs seven. */
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    fixture.maximum_state_count = 8;
    const uint32_t rescued = add_model_type(
        &fixture, (simple_content_request){SEQUENCE, {"a", "b", "c"}, {3, 3, 3}, {3, 3, 3}, 3});
    SXML_TEST_EXPECT(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){rescued, "a*3 b*3 c*3"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){rescued, "a*3 b*2 c*3"}));
    fixture_close(&fixture);
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    fixture.maximum_state_count = 6;
    add_model_type(
        &fixture,
        (simple_content_request){
            CHOICE, {"a", "b", "c", "d", "e", "f"}, {1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1}, 6});
    SXML_TEST_EXPECT(state, fixture_compile(&fixture) == SXML_STATUS_LIMIT_EXCEEDED);
    fixture_close(&fixture);
}

static void test_all_groups(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    const uint32_t type = add_model_type(
        &fixture, (simple_content_request){
                      SXML_SCHEMA_COMPOSITOR_ALL, {"a", "b", "c"}, {1, 0, 1}, {1, 1, 3}, 3});
    SXML_TEST_REQUIRE(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){type, "a c"}));
    SXML_TEST_EXPECT(state, content_accepts(&fixture, (match_run){type, "c b c a c"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){type, "c b c a c c"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){type, "a a c"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){type, "a b"}));
    SXML_TEST_EXPECT(state, !content_accepts(&fixture, (match_run){type, ""}));
    fixture_close(&fixture);

    /* An all group inside a sequence (cos-all-limited). */
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    const uint32_t element =
        add_element(&fixture, (element_request){"a", SXML_TYPE_XS_STRING, true, false});
    const uint32_t all =
        add_group(&fixture, (group_request){SXML_SCHEMA_COMPOSITOR_ALL,
                                            {{add_element_particle(&fixture, element)}, 1}});
    const uint32_t sequence = add_group(
        &fixture,
        (group_request){SEQUENCE,
                        {{add_particle(&fixture, (particle_request){1, 1, GROUP, all})}, 1}});
    add_complex_type(
        &fixture,
        (complex_type_request){
            NULL, 0, 0, add_particle(&fixture, (particle_request){1, 1, GROUP, sequence}), false});
    SXML_TEST_EXPECT(state, fixture_compile(&fixture) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&fixture, "cos-all-limited.1.2"));
    fixture_close(&fixture);
}

static void test_restriction(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* Base (a, b?, c*); (a, c{0,3}) restricts it, (a, b, b) does not. */
    const uint32_t base = add_model_type(
        &fixture,
        (simple_content_request){SEQUENCE, {"a", "b", "c"}, {1, 0, 0}, {1, 1, UNBOUNDED}, 3});
    const uint32_t valid =
        add_model_type(&fixture, (simple_content_request){SEQUENCE, {"a", "c"}, {1, 0}, {1, 3}, 2});
    edit_type(&fixture, valid)->base_type.value = base;
    SXML_TEST_REQUIRE(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    fixture_close(&fixture);

    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    const uint32_t other_base = add_model_type(
        &fixture,
        (simple_content_request){SEQUENCE, {"a", "b", "c"}, {1, 0, 0}, {1, 1, UNBOUNDED}, 3});
    const uint32_t invalid =
        add_model_type(&fixture, (simple_content_request){SEQUENCE, {"a", "b"}, {1, 2}, {1, 2}, 2});
    edit_type(&fixture, invalid)->base_type.value = other_base;
    SXML_TEST_EXPECT(state, fixture_compile(&fixture) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&fixture, "cos-content-act-restrict"));
    fixture_close(&fixture);
}

static void test_element_declarations(sxml_test_state *state) {
    test_fixture fixture;
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    /* e-props-correct.2: "ten" is no xs:integer. */
    const uint32_t element =
        add_element(&fixture, (element_request){"count", SXML_TYPE_XS_INTEGER, true, false});
    sxml_schema_text lexical_form = {.byte_offset = 0, .byte_count = 0};
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_text(fixture.components,
                                                             sxml_byte_view_from_c_string("ten"),
                                                             &lexical_form) == SXML_STATUS_OK);
    edit_element(&fixture, element)->value_constraint = (sxml_schema_value_constraint){
        .variety = SXML_SCHEMA_VALUE_CONSTRAINT_DEFAULT, .lexical_form = lexical_form};
    SXML_TEST_EXPECT(state, fixture_compile(&fixture) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&fixture, "e-props-correct.2"));
    fixture_close(&fixture);

    /* "10" is; a member whose type is not derived from its head's is not
       (e-props-correct.4). */
    SXML_TEST_REQUIRE(state, fixture_open(&fixture));
    const uint32_t valid =
        add_element(&fixture, (element_request){"count", SXML_TYPE_XS_INTEGER, true, false});
    SXML_TEST_REQUIRE(state, sxml_schema_components_add_text(fixture.components,
                                                             sxml_byte_view_from_c_string("10"),
                                                             &lexical_form) == SXML_STATUS_OK);
    edit_element(&fixture, valid)->value_constraint = (sxml_schema_value_constraint){
        .variety = SXML_SCHEMA_VALUE_CONSTRAINT_FIXED, .lexical_form = lexical_form};
    SXML_TEST_EXPECT(state, fixture_compile(&fixture) == SXML_STATUS_OK);
    const uint32_t member =
        add_element(&fixture, (element_request){"text", SXML_TYPE_XS_STRING, true, false});
    edit_element(&fixture, member)->substitution_group_affiliations =
        fixture_list(&fixture, (index_list){{valid}, 1});
    SXML_TEST_EXPECT(state, fixture_compile(&fixture) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&fixture, "e-props-correct.4"));
    fixture_close(&fixture);
}

static const sxml_test_case schema_content_model_test_cases[] = {
    {.name = "purchase_order", .function = test_purchase_order},
    {.name = "substitution_group", .function = test_substitution_group},
    {.name = "open_content_and_counter", .function = test_open_content_and_counter},
    {.name = "unique_particle_attribution", .function = test_unique_particle_attribution},
    {.name = "counters", .function = test_counters},
    {.name = "all_groups", .function = test_all_groups},
    {.name = "restriction", .function = test_restriction},
    {.name = "element_declarations", .function = test_element_declarations},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "schema_content_model",
            .cases = schema_content_model_test_cases,
            .case_count = SXML_TEST_COUNT(schema_content_model_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
