/*
 * Tests for include/sxml/processor.h through the public API: the clocks,
 * the table resolver, the processor's lifecycle in caller memory and
 * through an allocator (with an allocation-failure loop, D23), and
 * sxml_processor_parse_document. Every case checks for leaks.
 *
 * Golden file: tests/memory_test.c.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/tree.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <stdalign.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ----------------------------------------------------------------- Clock */

static void test_system_clock_reads_a_valid_instant(sxml_test_state *state) {
    sxml_clock_reading reading;
    memset(&reading, 0, sizeof(reading));
    const sxml_status status = sxml_clock_read(sxml_system_clock(), &reading);
    /* A platform without a clock may say so. */
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_UNSUPPORTED_FEATURE);
    if (status == SXML_STATUS_OK) {
        SXML_TEST_EXPECT(state, reading.current_date_time.year >= 2024);
        SXML_TEST_EXPECT(state, reading.current_date_time.has_timezone);
        SXML_TEST_EXPECT(state, reading.implicit_timezone_offset_minutes == 0);
    }
}

static void test_fixed_clock_reads_its_reading(sxml_test_state *state) {
    const sxml_clock_reading fixed = {
        .current_date_time = {.year = 2000, .month = 2, .day = 29, .hour = 23, .minute = 59,
                              .second = 59, .nanosecond = 999999999, .has_timezone = true,
                              .timezone_offset_minutes = -840},
        .implicit_timezone_offset_minutes = 840};
    sxml_clock_reading reading;
    memset(&reading, 0, sizeof(reading));
    SXML_TEST_EXPECT(state, sxml_clock_read(sxml_fixed_clock(&fixed), &reading) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, reading.current_date_time.year == 2000 &&
                                reading.current_date_time.day == 29 &&
                                reading.current_date_time.nanosecond == 999999999 &&
                                reading.current_date_time.timezone_offset_minutes == -840);
    SXML_TEST_EXPECT(state, reading.implicit_timezone_offset_minutes == 840);
}

static void test_clock_read_rejects_readings_outside_the_contract(sxml_test_state *state) {
    static const sxml_clock_reading bad_readings[] = {
        /* No timezone. */
        {.current_date_time = {.year = 2000, .month = 1, .day = 1, .has_timezone = false}},
        /* 1900 is not a leap year. */
        {.current_date_time = {.year = 1900, .month = 2, .day = 29, .has_timezone = true}},
        {.current_date_time = {.year = 2000, .month = 13, .day = 1, .has_timezone = true}},
        {.current_date_time = {.year = 2000, .month = 1, .day = 1, .hour = 24,
                               .has_timezone = true}},
        {.current_date_time = {.year = 2000, .month = 1, .day = 1, .nanosecond = 1000000000,
                               .has_timezone = true}},
        {.current_date_time = {.year = 2000, .month = 1, .day = 1, .has_timezone = true,
                               .timezone_offset_minutes = 841}},
        {.current_date_time = {.year = 2000, .month = 1, .day = 1, .has_timezone = true},
         .implicit_timezone_offset_minutes = -841},
    };
    for (size_t reading_index = 0; reading_index < SXML_TEST_COUNT(bad_readings);
         reading_index += 1) {
        sxml_clock_reading reading = {.implicit_timezone_offset_minutes = 7};
        SXML_TEST_EXPECT(state, sxml_clock_read(sxml_fixed_clock(&bad_readings[reading_index]),
                                                &reading) == SXML_STATUS_INVALID_ARGUMENT);
        /* Untouched on failure. */
        SXML_TEST_EXPECT(state, reading.implicit_timezone_offset_minutes == 7);
    }
    sxml_clock_reading reading;
    SXML_TEST_EXPECT(state, sxml_clock_read((sxml_clock){.state = NULL, .operations = NULL},
                                            &reading) == SXML_STATUS_INVALID_ARGUMENT);
}

/* ------------------------------------------------------ Resource resolver */

static void test_table_resolver_serves_its_entries(sxml_test_state *state) {
    const sxml_resource_entry entries[] = {
        {.uri = TEXT("urn:a"), .content = TEXT("<a/>")},
        {.uri = TEXT("b.xml"), .content = TEXT("<b/>")},
        {.uri = TEXT("urn:a"), .content = TEXT("<second/>")},
    };
    const sxml_resource_entry_view view = {.resource_entries = entries,
                                           .resource_entry_count = 3};
    const sxml_resource_resolver resolver = sxml_table_resource_resolver(&view);
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    SXML_TEST_EXPECT(state, resolver.operations->resolve(
                                resolver.state,
                                &(sxml_resource_request){
                                    .resource_kind = SXML_RESOURCE_KIND_XML_DOCUMENT,
                                    .uri_reference = TEXT("a"),
                                    .absolute_uri = TEXT("urn:a")},
                                &resource) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, resource.resource_form == SXML_RESOURCE_FORM_BYTES);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(resource.content, TEXT("<a/>")));
    /* Without an absolute URI, the reference is the key. */
    memset(&resource, 0, sizeof(resource));
    SXML_TEST_EXPECT(state, resolver.operations->resolve(
                                resolver.state,
                                &(sxml_resource_request){.uri_reference = TEXT("b.xml")},
                                &resource) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(resource.content, TEXT("<b/>")));
    memset(&resource, 0, sizeof(resource));
    SXML_TEST_EXPECT(state, resolver.operations->resolve(
                                resolver.state,
                                &(sxml_resource_request){.uri_reference = TEXT("c.xml")},
                                &resource) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, resource.resource_form == SXML_RESOURCE_FORM_NONE);
}

/* ------------------------------------------------------------- Processor */

static sxml_processor_configuration processor_configuration(sxml_allocator allocator) {
    return (sxml_processor_configuration){
        .allocator = allocator,
        .limits = {.maximum_name_pool_byte_count = 0},
        .resource_resolver = {.state = NULL, .operations = NULL},
        .clock = {.state = NULL, .operations = NULL}};
}

static void test_processor_lives_in_caller_memory(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = NULL;
    SXML_TEST_REQUIRE(state, sxml_counting_allocator_create(
                                 sxml_default_allocator(),
                                 &(sxml_counting_allocator_configuration){
                                     .backing_allocator = sxml_default_allocator(),
                                     .failing_allocation_number = 0},
                                 &counting_allocator) == SXML_STATUS_OK);
    const sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting_allocator);
    const sxml_processor_configuration configuration = processor_configuration(allocator);
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_processor_memory_requirements(&configuration, &requirements) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, requirements.byte_count > 0);
    const sxml_processor_configuration null_allocator =
        processor_configuration((sxml_allocator){.state = NULL, .operations = NULL});
    SXML_TEST_EXPECT(state, sxml_processor_memory_requirements(&null_allocator, &requirements) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    void *block = NULL;
    SXML_TEST_REQUIRE(state, sxml_allocator_allocate(
                                 sxml_default_allocator(),
                                 (sxml_allocation_request){.byte_count = requirements.byte_count + 64,
                                                           .alignment = 64},
                                 &block) == SXML_STATUS_OK);
    uint8_t *bytes = block;
    sxml_processor *processor = NULL;
    SXML_TEST_EXPECT(state, sxml_processor_initialize(
                                (sxml_memory_view){.bytes = bytes,
                                                   .byte_count = requirements.byte_count - 1},
                                &configuration, &processor) == SXML_STATUS_INSUFFICIENT_MEMORY);
    if (requirements.alignment > 1) {
        SXML_TEST_EXPECT(state, sxml_processor_initialize(
                                    (sxml_memory_view){.bytes = bytes + 1,
                                                       .byte_count = requirements.byte_count + 8},
                                    &configuration, &processor) == SXML_STATUS_MISALIGNED_MEMORY);
    }
    SXML_TEST_EXPECT(state, processor == NULL);
    SXML_TEST_EXPECT(state, sxml_processor_initialize(
                                (sxml_memory_view){.bytes = bytes,
                                                   .byte_count = requirements.byte_count},
                                &configuration, &processor) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_processor_get_item_heap(processor) != NULL);
    SXML_TEST_EXPECT(state, sxml_processor_get_name_pool(processor) != NULL);
    SXML_TEST_EXPECT(state, sxml_processor_get_type_registry(processor) != NULL);
    sxml_processor_finalize(processor);
    sxml_processor_finalize(NULL);
    sxml_processor_destroy(NULL);
    sxml_allocator_release(sxml_default_allocator(), block);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(counting_allocator)
                                    .live_allocation_count == 0);
    sxml_counting_allocator_destroy(counting_allocator);
}

/* Creates a processor and parses a document with allocation number
   failing_number failing; checks that nothing leaked. */
static sxml_status parse_with_failure(sxml_test_state *state, uint64_t failing_number) {
    sxml_counting_allocator *counting_allocator = NULL;
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number = failing_number},
        &counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting_allocator);
    const sxml_processor_configuration configuration = processor_configuration(allocator);
    sxml_processor *processor = NULL;
    status = sxml_processor_create(allocator, &configuration, &processor);
    sxml_item document = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_document(
            processor,
            &(sxml_xml_parse_request){.input = TEXT("<a x='1'><b>text</b><!--c--></a>"),
                                      .document_uri = TEXT("urn:doc")},
            &document);
    }
    if (status == SXML_STATUS_OK) {
        sxml_node_reference node;
        sxml_node_description description;
        SXML_TEST_EXPECT(state, sxml_item_get_node(document, &node) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, sxml_node_describe(node, &description) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, description.node_kind == SXML_NODE_KIND_DOCUMENT);
        sxml_item_release(document);
    }
    if (processor != NULL) {
        SXML_TEST_EXPECT(state, sxml_item_heap_read_counts(sxml_processor_get_item_heap(processor))
                                        .live_cell_count == 0);
    }
    sxml_processor_destroy(processor);
    SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(counting_allocator)
                                    .live_allocation_count == 0);
    sxml_counting_allocator_destroy(counting_allocator);
    return status;
}

static void test_processor_survives_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 100000; failing_number += 1) {
        status = parse_with_failure(state, failing_number);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 5);
}

static void test_parse_document_reports_malformed_input(sxml_test_state *state) {
    sxml_processor *processor = NULL;
    SXML_TEST_REQUIRE(state, sxml_processor_create(sxml_default_allocator(),
                                                   &(sxml_processor_configuration){
                                                       .allocator = sxml_default_allocator(),
                                                       .limits = {.xml_limits = {.maximum_depth = 2}}},
                                                   &processor) == SXML_STATUS_OK);
    sxml_diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof(diagnostic));
    sxml_item document = {.representation = 0};
    SXML_TEST_EXPECT(state, sxml_processor_parse_document(
                                processor,
                                &(sxml_xml_parse_request){.input = TEXT("<a>\n<b></a>"),
                                                          .diagnostic = &diagnostic},
                                &document) == SXML_STATUS_MALFORMED_INPUT);
    SXML_TEST_EXPECT(state, diagnostic.location.line_number == 2);
    SXML_TEST_EXPECT(state, diagnostic.description.byte_count > 0);
    /* The processor's XML limits apply. */
    SXML_TEST_EXPECT(state, sxml_processor_parse_document(
                                processor, &(sxml_xml_parse_request){.input = TEXT("<a><b><c/></b></a>")},
                                &document) == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, sxml_item_heap_read_counts(sxml_processor_get_item_heap(processor))
                                    .live_cell_count == 0);
    sxml_processor_destroy(processor);
}

/* ------------------------------------------------------ Source locations */

/* What one location check works with: a processor whose table resolver
   serves urn:doc. */
typedef struct location_fixture {
    sxml_test_state *state;
    sxml_processor *processor;
} location_fixture;

static const sxml_resource_entry location_entries[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:doc"),
     .content = SXML_BYTE_VIEW_INITIALIZER("<a>\n  <b/>\n</a>")},
};
static const sxml_resource_entry_view location_entry_view = {
    .resource_entries = location_entries, .resource_entry_count = 1};

/* The line of the node item is, 0 when its document kept no locations. */
static uint32_t inner_element_line(sxml_test_state *state, sxml_item item) {
    sxml_node_reference node = {.tree = NULL};
    SXML_TEST_EXPECT(state, sxml_item_get_node(item, &node) == SXML_STATUS_OK);
    sxml_source_location location = {.line_number = 99};
    SXML_TEST_EXPECT(state, sxml_node_get_location(node, &location) == SXML_STATUS_OK);
    return location.line_number;
}

/* Parses <a>\n  <b/>\n</a> with sxml_processor_parse_document and
   returns the line of its document node (1 when kept, 0 when not). */
static uint32_t parsed_document_line(const location_fixture *fixture) {
    sxml_item document = {.representation = 0};
    SXML_TEST_EXPECT(fixture->state,
                     sxml_processor_parse_document(
                         fixture->processor,
                         &(sxml_xml_parse_request){.input = location_entries[0].content,
                                                   .source_name = TEXT("doc.xml")},
                         &document) == SXML_STATUS_OK);
    const uint32_t line = inner_element_line(fixture->state, document);
    sxml_item_release(document);
    return line;
}

/* Compiles text and returns it; NULL on failure (the test then fails). */
static sxml_xpath_expression *compile_expression(const location_fixture *fixture,
                                                 sxml_byte_view text) {
    sxml_xpath_expression *expression = NULL;
    SXML_TEST_EXPECT(fixture->state,
                     sxml_xpath_expression_create(
                         sxml_default_allocator(),
                         &(sxml_xpath_expression_configuration){.processor = fixture->processor,
                                                                .expression_text = text},
                         &expression) == SXML_STATUS_OK);
    return expression;
}

/* Evaluates text, whose result is one node of urn:doc, and returns that
   node's line (0 when its document kept no locations). */
static uint32_t evaluated_node_line(const location_fixture *fixture, sxml_byte_view text) {
    sxml_xpath_expression *expression = compile_expression(fixture, text);
    sxml_sequence result = sxml_sequence_empty();
    uint32_t line = 0;
    if (expression != NULL &&
        sxml_xpath_expression_evaluate(expression,
                                       &(sxml_xpath_evaluation_request){.error_record = NULL},
                                       &result) == SXML_STATUS_OK) {
        sxml_item item = {.representation = 0};
        SXML_TEST_EXPECT(fixture->state,
                         sxml_sequence_get_item(result,
                                                (sxml_item_request){.item_index = 0,
                                                                    .error_record = NULL},
                                                &item) == SXML_STATUS_OK);
        line = inner_element_line(fixture->state, item);
        sxml_sequence_release(result);
    } else {
        SXML_TEST_EXPECT(fixture->state, false);
    }
    sxml_xpath_expression_destroy(expression);
    return line;
}

static sxml_processor *make_location_processor(sxml_test_state *state,
                                               sxml_source_location_policy policy) {
    sxml_processor *processor = NULL;
    SXML_TEST_EXPECT(state, sxml_processor_create(
                                sxml_default_allocator(),
                                &(sxml_processor_configuration){
                                    .allocator = sxml_default_allocator(),
                                    .resource_resolver =
                                        sxml_table_resource_resolver(&location_entry_view),
                                    .source_location_policy = policy},
                                &processor) == SXML_STATUS_OK);
    return processor;
}

/* D313, D318: AUTOMATIC keeps a host's document parsed before any program
   is compiled; after that the programs decide, and fn:doc follows the
   program it is evaluated for. */
static void test_automatic_locations_follow_the_programs(sxml_test_state *state) {
    location_fixture fixture = {
        .state = state,
        .processor = make_location_processor(state, SXML_SOURCE_LOCATION_POLICY_AUTOMATIC)};
    SXML_TEST_REQUIRE(state, fixture.processor != NULL);
    SXML_TEST_EXPECT(state, parsed_document_line(&fixture) == 1);
    /* A program that cannot ask: fn:doc and later parses keep nothing. */
    SXML_TEST_EXPECT(state, evaluated_node_line(&fixture, TEXT("doc('urn:doc')/a/b")) == 0);
    SXML_TEST_EXPECT(state, parsed_document_line(&fixture) == 0);
    /* One that calls fn:location: its documents keep them. */
    SXML_TEST_EXPECT(state, evaluated_node_line(
                                &fixture, TEXT("doc('urn:doc')/a/b[exists(location(.))]")) == 2);
    /* So do the host's from now on; fn:doc still follows the program it
       is evaluated for. */
    SXML_TEST_EXPECT(state, parsed_document_line(&fixture) == 1);
    SXML_TEST_EXPECT(state, evaluated_node_line(&fixture, TEXT("doc('urn:doc')/a/b")) == 0);
    /* function-lookup can reach fn:location. */
    SXML_TEST_EXPECT(
        state, evaluated_node_line(
                   &fixture, TEXT("doc('urn:doc')/a/b[function-lookup(xs:QName('fn:true'), 0)()]")) ==
                   2);
    sxml_processor_destroy(fixture.processor);
}

static void test_always_and_never_locations(sxml_test_state *state) {
    location_fixture fixture = {
        .state = state,
        .processor = make_location_processor(state, SXML_SOURCE_LOCATION_POLICY_ALWAYS)};
    SXML_TEST_REQUIRE(state, fixture.processor != NULL);
    sxml_xpath_expression *expression = compile_expression(&fixture, TEXT("count(//b)"));
    SXML_TEST_EXPECT(state, parsed_document_line(&fixture) == 1);
    SXML_TEST_EXPECT(state, evaluated_node_line(&fixture, TEXT("doc('urn:doc')/a/b")) == 2);
    sxml_xpath_expression_destroy(expression);
    sxml_processor_destroy(fixture.processor);
    fixture.processor = make_location_processor(state, SXML_SOURCE_LOCATION_POLICY_NEVER);
    SXML_TEST_REQUIRE(state, fixture.processor != NULL);
    SXML_TEST_EXPECT(state, parsed_document_line(&fixture) == 0);
    SXML_TEST_EXPECT(state, evaluated_node_line(
                                &fixture, TEXT("doc('urn:doc')/a/b[empty(location(.))]")) == 0);
    sxml_processor_destroy(fixture.processor);
    /* A value outside the enumeration. */
    sxml_processor *processor = NULL;
    SXML_TEST_EXPECT(state, sxml_processor_create(
                                sxml_default_allocator(),
                                &(sxml_processor_configuration){
                                    .allocator = sxml_default_allocator(),
                                    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                                    .source_location_policy = (sxml_source_location_policy)7},
                                &processor) == SXML_STATUS_INVALID_ARGUMENT);
}

static const sxml_test_case processor_test_cases[] = {
    {.name = "system_clock_reads_a_valid_instant",
     .function = test_system_clock_reads_a_valid_instant},
    {.name = "fixed_clock_reads_its_reading", .function = test_fixed_clock_reads_its_reading},
    {.name = "clock_read_rejects_readings_outside_the_contract",
     .function = test_clock_read_rejects_readings_outside_the_contract},
    {.name = "table_resolver_serves_its_entries",
     .function = test_table_resolver_serves_its_entries},
    {.name = "processor_lives_in_caller_memory",
     .function = test_processor_lives_in_caller_memory},
    {.name = "processor_survives_every_allocation_failure",
     .function = test_processor_survives_every_allocation_failure},
    {.name = "parse_document_reports_malformed_input",
     .function = test_parse_document_reports_malformed_input},
    {.name = "automatic_locations_follow_the_programs",
     .function = test_automatic_locations_follow_the_programs},
    {.name = "always_and_never_locations", .function = test_always_and_never_locations},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "processor",
            .cases = processor_test_cases,
            .case_count = SXML_TEST_COUNT(processor_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
