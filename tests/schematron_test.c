/*
 * Unit tests of Schematron (include/sxml/schematron.h; D315, D323): the
 * placeholder scan and its expansion segments, compile errors through the
 * report sink with their locations (several in one compile), reports with
 * the schema's and the instance's locations, the constructs of the 2025
 * edition, SVRL, configured parameters, inclusions through the resolver,
 * and the lifecycle under the counting allocator. The conformance suite
 * of tests/schematron runs in tests/schematron/driver.
 *
 * Golden file: tests/xslt_test.c (fixture, report sink, allocation
 * failures).
 */
#include "schematron_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/schematron.h"
#include "test_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)
#define SCHEMA_URI "http://example.com/s/schema.sch"
#define SCHEMA_START                                                                               \
    "<sch:schema xmlns:sch='http://purl.oclc.org/dsdl/schematron' queryBinding='xslt3'"            \
    " schematronEdition='2025'>\n"

/* Prints text to standard error when SXML_TEST_VERBOSE is set, to see
   what a failing case received. */
static void note(sxml_test_state *state, const char *text) {
    (void)state;
    if (getenv("SXML_TEST_VERBOSE") != NULL) {
        (void)fprintf(stderr, "%s\n", text);
    }
}

/* ------------------------------------------------------------- Fixture */

static const char included_text[] =
    "<sch:rule xmlns:sch='http://purl.oclc.org/dsdl/schematron' context='item'>\n"
    "  <sch:assert test='@n' id='has-n'>An item has n.</sch:assert>\n"
    "</sch:rule>";

static const char library_text[] =
    "<sch:library xmlns:sch='http://purl.oclc.org/dsdl/schematron'>\n"
    "  <sch:pattern id='from-library'>\n"
    "    <sch:rule context='doc'><sch:report test='true()' id='library'>Library.</sch:report>"
    "</sch:rule>\n"
    "  </sch:pattern>\n"
    "</sch:library>";

static const char other_text[] = "<entries>\n  <entry ok='1'/>\n  <entry/>\n</entries>";

static const sxml_resource_entry fixture_resources[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/s/other.xml"),
     .content = {.bytes = (const uint8_t *)other_text, .byte_count = sizeof(other_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/s/rule.sch"),
     .content = {.bytes = (const uint8_t *)included_text, .byte_count = sizeof(included_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/s/library.sch"),
     .content = {.bytes = (const uint8_t *)library_text, .byte_count = sizeof(library_text) - 1}},
};

static const sxml_resource_entry_view fixture_resource_view = {
    .resource_entries = fixture_resources,
    .resource_entry_count = sizeof(fixture_resources) / sizeof(fixture_resources[0])};

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL};
    sxml_status status =
        sxml_counting_allocator_create(sxml_default_allocator(),
                                       &(sxml_counting_allocator_configuration){
                                           .backing_allocator = sxml_default_allocator(),
                                           .failing_allocation_number = failing_allocation_number},
                                       &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    return sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){.allocator = allocator,
                                        .resource_resolver =
                                            sxml_table_resource_resolver(&fixture_resource_view)},
        &test_fixture->processor);
}

/* Destroys the fixture; true when nothing leaked. */
static bool fixture_destroy(fixture *test_fixture) {
    bool has_no_leak = true;
    if (test_fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(test_fixture->processor);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak =
            has_no_leak && sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
                                   .live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

/* ------------------------------------------------------------- Reports */

enum { SXML_TEST_MAXIMUM_LINE_COUNT = 32, SXML_TEST_LINE_BYTE_COUNT = 256 };

/* What a sink received, one line each: compile errors as
   "PREFIX:CODE LINE:COLUMN", reports as "KIND ID PATH LINE @SCHEMA-LINE
   MESSAGE". */
typedef struct received_lines {
    char lines[SXML_TEST_MAXIMUM_LINE_COUNT][SXML_TEST_LINE_BYTE_COUNT];
    size_t line_count;
} received_lines;

static sxml_status receive_compile_error(void *state, const sxml_schema_report *report) {
    received_lines *received = state;
    if (received->line_count < SXML_TEST_MAXIMUM_LINE_COUNT) {
        (void)snprintf(received->lines[received->line_count], SXML_TEST_LINE_BYTE_COUNT,
                       "%.*s:%.*s %u:%u", (int)report->code.namespace_prefix.byte_count,
                       (const char *)report->code.namespace_prefix.bytes,
                       (int)report->code.local_name.byte_count,
                       (const char *)report->code.local_name.bytes, report->location.line_number,
                       report->location.column_number);
        received->line_count += 1;
    }
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations compile_sink_operations = {
    .receive = receive_compile_error};

static sxml_status receive_report(void *state, const sxml_schematron_report *report) {
    received_lines *received = state;
    static const char *const kinds[] = {"failed-assert", "successful-report", "fired-rule"};
    if (received->line_count < SXML_TEST_MAXIMUM_LINE_COUNT) {
        (void)snprintf(received->lines[received->line_count], SXML_TEST_LINE_BYTE_COUNT,
                       "%s %.*s %.*s %u @%u %s%.*s%s%.*s", kinds[report->report_kind],
                       (int)report->id.byte_count, (const char *)report->id.bytes,
                       (int)report->node_path.byte_count, (const char *)report->node_path.bytes,
                       report->node_location.line_number, report->schema_location.line_number,
                       report->is_error ? "error " : "", (int)report->message.byte_count,
                       (const char *)report->message.bytes,
                       report->severity.byte_count > 0 ? " severity=" : "",
                       (int)report->severity.byte_count, (const char *)report->severity.bytes);
        received->line_count += 1;
    }
    return SXML_STATUS_OK;
}

static const sxml_schematron_report_sink_operations report_sink_operations = {.receive =
                                                                                  receive_report};

static bool has_line(const received_lines *received, const char *line) {
    for (size_t line_index = 0; line_index < received->line_count; line_index += 1) {
        if (strcmp(received->lines[line_index], line) == 0) {
            return true;
        }
    }
    return false;
}

/* A schema to compile. */
typedef struct compile_request {
    const char *text;
    const char *phase_id;
    sxml_schematron_parameter_view parameters;
    /* Receives the compile errors; NULL for the null sink. */
    received_lines *errors;
} compile_request;

static sxml_status compile(const fixture *test_fixture, const compile_request *request,
                           sxml_schematron **schematron) {
    received_lines *errors = request->errors;
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    const sxml_status status = sxml_schematron_create(
        fixture_allocator(test_fixture),
        &(sxml_schematron_configuration){
            .processor = test_fixture->processor,
            .document_source = SXML_SCHEMATRON_DOCUMENT_SOURCE_TEXT,
            .document_text = sxml_byte_view_from_c_string(request->text),
            .document_uri = TEXT(SCHEMA_URI),
            .phase_id = request->phase_id != NULL
                            ? sxml_byte_view_from_c_string(request->phase_id)
                            : (sxml_byte_view){.bytes = NULL, .byte_count = 0},
            .parameters = request->parameters,
            .report_sink = {.state = errors,
                            .operations = errors != NULL ? &compile_sink_operations : NULL},
            .error_record = &record},
        schematron);
    if (status != SXML_STATUS_OK && errors == NULL && getenv("SXML_TEST_VERBOSE") != NULL) {
        (void)fprintf(
            stderr, "compile: %.*s %.*s at %u:%u\n", (int)record.code.local_name.byte_count,
            (const char *)record.code.local_name.bytes,
            (int)record.diagnostic.description.byte_count,
            (const char *)record.diagnostic.description.bytes,
            record.diagnostic.location.line_number, record.diagnostic.location.column_number);
    }
    return status;
}

/* An instance to validate, with what to collect. */
typedef struct validation_request {
    const sxml_schematron *schematron;
    const char *instance_text;
    /* Receives the reports. */
    received_lines *reports;
    sxml_output_stream svrl_output_stream;
    bool reports_fired_rules;
} validation_request;

static sxml_status validate(const fixture *test_fixture, const validation_request *request,
                            sxml_schematron_outcome *outcome) {
    const sxml_schematron *schematron = request->schematron;
    received_lines *reports = request->reports;
    sxml_item instance;
    sxml_status status = sxml_processor_parse_document(
        test_fixture->processor,
        &(sxml_xml_parse_request){.input = sxml_byte_view_from_c_string(request->instance_text),
                                  .source_name = TEXT("instance.xml"),
                                  .document_uri = TEXT("http://example.com/s/instance.xml"),
                                  .base_uri = TEXT("http://example.com/s/instance.xml")},
        &instance);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    status = sxml_schematron_validate_node(
        schematron,
        &(sxml_schematron_validation_request){
            .node = instance,
            .report_sink = {.state = reports, .operations = &report_sink_operations},
            .svrl_output_stream = request->svrl_output_stream,
            .error_record = &record,
            .reports_fired_rules = request->reports_fired_rules},
        outcome);
    if (status != SXML_STATUS_OK && getenv("SXML_TEST_VERBOSE") != NULL) {
        (void)fprintf(
            stderr, "validate: %.*s %.*s at %u:%u\n", (int)record.code.local_name.byte_count,
            (const char *)record.code.local_name.bytes,
            (int)record.diagnostic.description.byte_count,
            (const char *)record.diagnostic.description.bytes,
            record.diagnostic.location.line_number, record.diagnostic.location.column_number);
    }
    sxml_item_release(instance);
    return status;
}

/* --------------------------------------------------------- Placeholders */

static void test_placeholders_replace_whole_name_tokens(sxml_test_state *state) {
    const sxml_schematron_parameter_value schema_values[] = {
        {.name = TEXT("limit"), .value = TEXT("10")},
        {.name = TEXT("part"), .value = TEXT("$limit")},
    };
    const sxml_schematron_parameter_value instance_values[] = {
        {.name = TEXT("AMOUNT"), .value = TEXT("@amount + $limit")},
    };
    sxml_schematron_expansion expansion;
    SXML_TEST_EXPECT(state,
                     sxml_schematron_expand(
                         sxml_default_allocator(),
                         &(sxml_schematron_expansion_request){
                             .text = TEXT("$AMOUNT gt $AMOUNTX and $part and $"),
                             .instance_parameters = {.values = instance_values, .value_count = 1},
                             .schema_parameters = {.values = schema_values, .value_count = 2},
                             .renamed_variables = {.values = NULL, .value_count = 0}},
                         &expansion) == SXML_STATUS_OK);
    /* An instance value is scanned once for schema parameters; a schema
       parameter's value is not scanned again; $AMOUNTX is not AMOUNT. */
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(sxml_schematron_buffer_text(&expansion.text),
                                           TEXT("@amount + 10 gt $AMOUNTX and $limit and $")));
    SXML_TEST_EXPECT(state, expansion.has_placeholder);
    /* "+" at offset 8 is in AMOUNT's value at 8; "10" at 10 in limit's;
       "gt" at 13 back in the attribute, after "$AMOUNT" (offset 8). */
    sxml_schematron_expansion_origin origin = sxml_schematron_expansion_origin_of(&expansion, 8);
    SXML_TEST_EXPECT(state, origin.parameter == &instance_values[0] && origin.source_offset == 8);
    origin = sxml_schematron_expansion_origin_of(&expansion, 10);
    SXML_TEST_EXPECT(state, origin.parameter == &schema_values[0] && origin.source_offset == 0);
    origin = sxml_schematron_expansion_origin_of(&expansion, 13);
    SXML_TEST_EXPECT(state, origin.parameter == NULL && origin.source_offset == 8);
    sxml_schematron_expansion_release(&expansion);
}

/* ------------------------------------------------------------ Compiling */

static void test_compile_reports_every_error_with_its_location(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char schema[] = SCHEMA_START
        "  <sch:param name='limit' value='1 +'/>\n"
        "  <sch:pattern>\n"
        "    <sch:rule context='item['>\n"
        "      <sch:assert test='@n'>n</sch:assert>\n"
        "    </sch:rule>\n"
        "    <sch:rule context='doc'>\n"
        "      <sch:assert test='@n ='>n</sch:assert>\n"
        "      <sch:bogus/>\n"
        "    </sch:rule>\n"
        "  </sch:pattern>\n"
        "  <sch:pattern abstract='true' id='a'>\n"
        "    <sch:rule context='$c'><sch:assert test='true()'>t</sch:assert></sch:rule>\n"
        "  </sch:pattern>\n"
        "  <sch:pattern is-a='a'><sch:param name='c' value='x['/></sch:pattern>\n"
        "  <xsl:function xmlns:xsl='http://www.w3.org/1999/XSL/Transform' name='Q{f}g'>"
        "<xsl:sequence select='1 *'/></xsl:function>\n"
        "  <sch:pattern><sch:rule context='doc'><sch:assert test='$limit'>l</sch:assert>"
        "</sch:rule></sch:pattern>\n"
        "</sch:schema>";
    received_lines errors;
    memset(&errors, 0, sizeof(errors));
    sxml_schematron *schematron = NULL;
    SXML_TEST_EXPECT(state,
                     compile(&test_fixture, &(compile_request){.text = schema, .errors = &errors},
                             &schematron) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, schematron == NULL);
    /* Each error at its place: the schema parameter's value (2), the
       rule context (4, at its unclosed "["), the test (8), the unknown
       element (9), the instance's parameter value (15, at its "["), the
       foreign function (16). */
    SXML_TEST_EXPECT(state, errors.line_count == 6);
    SXML_TEST_EXPECT(state, has_line(&errors, "schematron:syntax 9:7"));
    SXML_TEST_EXPECT(state, has_line(&errors, "err:XTSE0340 4:20"));
    SXML_TEST_EXPECT(state, has_line(&errors, "err:XPST0003 8:23"));
    SXML_TEST_EXPECT(state, has_line(&errors, "err:XTSE0340 15:47"));
    SXML_TEST_EXPECT(state, has_line(&errors, "err:XPST0003 16:96"));
    SXML_TEST_EXPECT(state, has_line(&errors, "err:XPST0003 2:30"));
    for (size_t line_index = 0; line_index < errors.line_count; line_index += 1) {
        note(state, errors.lines[line_index]);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_compile_refuses_unsupported_query_bindings(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char *const bindings[] = {"xquery3", "unknown", "XSLT3", "xpath31", "xslt4"};
    static const sxml_status expected[] = {SXML_STATUS_STATIC_ERROR, SXML_STATUS_STATIC_ERROR,
                                           SXML_STATUS_OK, SXML_STATUS_OK, SXML_STATUS_OK};
    for (size_t binding_index = 0; binding_index < 5; binding_index += 1) {
        char schema[512];
        (void)snprintf(schema, sizeof(schema),
                 "<sch:schema xmlns:sch='http://purl.oclc.org/dsdl/schematron' "
                 "queryBinding='%s'>\n<sch:pattern><sch:rule context='/doc'>"
                 "<sch:assert test='true()'>t</sch:assert></sch:rule></sch:pattern>"
                 "</sch:schema>",
                 bindings[binding_index]);
        received_lines errors;
        memset(&errors, 0, sizeof(errors));
        sxml_schematron *schematron = NULL;
        SXML_TEST_EXPECT(state, compile(&test_fixture,
                                        &(compile_request){.text = schema, .errors = &errors},
                                        &schematron) == expected[binding_index]);
        for (size_t line_index = 0; line_index < errors.line_count; line_index += 1) {
            note(state, errors.lines[line_index]);
        }
        if (expected[binding_index] != SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, has_line(&errors, "schematron:query-binding 1:62"));
        }
        sxml_schematron_destroy(schematron);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_configured_parameters_replace_schema_parameters(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char schema[] =
        SCHEMA_START "  <sch:param name='limit' value='2'/>\n"
                     "  <sch:pattern><sch:rule context='item'>\n"
                     "    <sch:assert test='@n &lt;= $limit' id='within'>Within.</sch:assert>\n"
                     "  </sch:rule></sch:pattern>\n"
                     "</sch:schema>";
    const sxml_schematron_parameter limit = {.name = TEXT("limit"), .value = TEXT("3")};
    sxml_schematron *schematron = NULL;
    SXML_TEST_EXPECT(state, compile(&test_fixture,
                                    &(compile_request){
                                        .text = schema,
                                        .parameters = {.parameters = &limit, .parameter_count = 1},
                                        .errors = NULL},
                                    &schematron) == SXML_STATUS_OK);
    received_lines reports;
    memset(&reports, 0, sizeof(reports));
    sxml_schematron_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    SXML_TEST_EXPECT(state, validate(&test_fixture,
                                     &(validation_request){
                                         .instance_text = "<doc><item n='3'/><item n='4'/></doc>",
                                         .schematron = schematron,
                                         .reports = &reports},
                                     &outcome) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, outcome.finding_count == 1);
    SXML_TEST_EXPECT(state, has_line(&reports, "failed-assert within /Q{}doc[1]/Q{}item[2] 1 @4 "
                                               "error Within."));
    sxml_schematron_destroy(schematron);
    /* A name that is no schema parameter. */
    const sxml_schematron_parameter unknown = {.name = TEXT("other"), .value = TEXT("1")};
    received_lines errors;
    memset(&errors, 0, sizeof(errors));
    SXML_TEST_EXPECT(state, compile(&test_fixture,
                                    &(compile_request){.text = schema,
                                                       .parameters = {.parameters = &unknown,
                                                                      .parameter_count = 1},
                                                       .errors = &errors},
                                    &schematron) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, errors.line_count == 1 &&
                                strncmp(errors.lines[0], "schematron:parameter", 20) == 0);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ----------------------------------------------------------- Validating */

static void test_reports_carry_both_locations(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char schema[] = SCHEMA_START
        "  <sch:extends href='library.sch'/>\n"
        "  <sch:pattern id='p'>\n"
        "    <sch:let name='count' value='count(//item)'/>\n"
        "    <sch:rule context='doc'>\n"
        "      <sch:report test='$count = 2' role='info' id='two'>Two <sch:name/> items, "
        "<sch:value-of select='$count'/>.</sch:report>\n"
        "    </sch:rule>\n"
        "    <sch:include href='rule.sch'/>\n"
        "  </sch:pattern>\n"
        "</sch:schema>";
    sxml_schematron *schematron = NULL;
    SXML_TEST_EXPECT(state,
                     compile(&test_fixture, &(compile_request){.text = schema, .errors = NULL},
                             &schematron) == SXML_STATUS_OK);
    received_lines reports;
    memset(&reports, 0, sizeof(reports));
    sxml_schematron_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    SXML_TEST_EXPECT(state,
                     validate(&test_fixture,
                              &(validation_request){.instance_text = "<doc>\n  <item n='1'/>\n"
                                                                     "  <item/>\n</doc>",
                                                    .reports_fired_rules = true,
                                                    .schematron = schematron,
                                                    .reports = &reports},
                              &outcome) == SXML_STATUS_OK);
    for (size_t line_index = 0; line_index < reports.line_count; line_index += 1) {
        note(state, reports.lines[line_index]);
    }
    /* The included rule's assert is in rule.sch, line 2; the report's at
       line 8 of the schema, with role info, so not an error. */
    SXML_TEST_EXPECT(state, has_line(&reports, "failed-assert has-n /Q{}doc[1]/Q{}item[2] 3 @2 "
                                               "error An item has n."));
    SXML_TEST_EXPECT(state,
                     has_line(&reports, "successful-report two /Q{}doc[1] 1 @6 Two doc items, 2."));
    SXML_TEST_EXPECT(
        state, has_line(&reports, "successful-report library /Q{}doc[1] 1 @3 error Library."));
    SXML_TEST_EXPECT(state, has_line(&reports, "fired-rule  /Q{}doc[1] 1 @5 "));
    SXML_TEST_EXPECT(state, outcome.finding_count == 3 && outcome.error_finding_count == 2);
    sxml_schematron_destroy(schematron);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Each report's document URI and node line, "URI LINE". */
static sxml_status receive_document(void *state, const sxml_schematron_report *report) {
    received_lines *received = state;
    if (received->line_count < SXML_TEST_MAXIMUM_LINE_COUNT) {
        (void)snprintf(received->lines[received->line_count], SXML_TEST_LINE_BYTE_COUNT, "%.*s %u",
                       (int)report->document_uri.byte_count,
                       (const char *)report->document_uri.bytes, report->node_location.line_number);
        received->line_count += 1;
    }
    return SXML_STATUS_OK;
}

static const sxml_schematron_report_sink_operations document_sink_operations = {
    .receive = receive_document};

/* A finding names the document its node is in: a subordinate document of
   a pattern with documents (5.4.12) or the instance (D331). */
static void test_reports_name_their_document(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char schema[] = SCHEMA_START
        "  <sch:pattern id='other' documents=\"'other.xml'\">\n"
        "    <sch:rule context='entry'><sch:assert test='@ok'>Not ok.</sch:assert></sch:rule>\n"
        "  </sch:pattern>\n"
        "  <sch:pattern id='instance'>\n"
        "    <sch:rule context='item'><sch:assert test='@n'>No n.</sch:assert></sch:rule>\n"
        "  </sch:pattern>\n"
        "</sch:schema>";
    sxml_schematron *schematron = NULL;
    SXML_TEST_EXPECT(state,
                     compile(&test_fixture, &(compile_request){.text = schema, .errors = NULL},
                             &schematron) == SXML_STATUS_OK);
    sxml_item instance;
    SXML_TEST_REQUIRE(
        state, sxml_processor_parse_document(
                   test_fixture.processor,
                   &(sxml_xml_parse_request){
                       .input = TEXT("<doc>\n  <item/>\n</doc>"),
                       .source_name = TEXT("instance.xml"),
                       .document_uri = TEXT("http://example.com/s/instance.xml"),
                       .base_uri = TEXT("http://example.com/s/instance.xml")},
                   &instance) == SXML_STATUS_OK);
    received_lines reports;
    memset(&reports, 0, sizeof(reports));
    sxml_schematron_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    SXML_TEST_EXPECT(state,
                     sxml_schematron_validate_node(
                         schematron,
                         &(sxml_schematron_validation_request){
                             .node = instance,
                             .report_sink = {.state = &reports,
                                             .operations = &document_sink_operations},
                             .svrl_output_stream = {.state = NULL, .operations = NULL},
                             .error_record = NULL,
                             .reports_fired_rules = false},
                         &outcome) == SXML_STATUS_OK);
    for (size_t line_index = 0; line_index < reports.line_count; line_index += 1) {
        note(state, reports.lines[line_index]);
    }
    SXML_TEST_EXPECT(state, reports.line_count == 2);
    SXML_TEST_EXPECT(state, has_line(&reports, "http://example.com/s/other.xml 3"));
    SXML_TEST_EXPECT(state, has_line(&reports, "http://example.com/s/instance.xml 2"));
    sxml_item_release(instance);
    sxml_schematron_destroy(schematron);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* The known deviations of real rule sets compile, each occurrence a
   schematron:compatibility warning with its place; anything else stays
   an error; the null sink drops warnings (D331). */
static void test_compatibility_warnings(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char schema[] = SCHEMA_START
        "  <sch:pattern id='globals'>\n"
        "    <sch:let name='limit' value='2'/>\n"
        "  </sch:pattern>\n"
        "  <sch:pattern abstract='true' id='bounded'>\n"
        "    <sch:rule context='$element'><sch:assert test='number(@n) le $limit'>Too big.</sch:assert>"
        "</sch:rule>\n"
        "  </sch:pattern>\n"
        "  <sch:pattern is-a='bounded' id='items'>\n"
        "    <sch:param name='element' value='item'/>\n"
        "    <sch:param name='element' value='other'/>\n"
        "  </sch:pattern>\n"
        "  <sch:pattern id='late'>\n"
        "    <sch:rule context='doc'>\n"
        "      <sch:assert test='$count gt 0'>Empty.</sch:assert>\n"
        "      <sch:let name='count' value='count(item)'/>\n"
        "    </sch:rule>\n"
        "  </sch:pattern>\n"
        "</sch:schema>";
    received_lines warnings;
    memset(&warnings, 0, sizeof(warnings));
    sxml_schematron *schematron = NULL;
    SXML_TEST_EXPECT(state, compile(&test_fixture, &(compile_request){.text = schema, .errors = &warnings},
                                    &schematron) == SXML_STATUS_OK);
    for (size_t line_index = 0; line_index < warnings.line_count; line_index += 1) {
        note(state, warnings.lines[line_index]);
    }
    SXML_TEST_EXPECT(state, warnings.line_count == 3);
    SXML_TEST_EXPECT(state, has_line(&warnings, "schematron:compatibility 10:5"));
    SXML_TEST_EXPECT(state, has_line(&warnings, "schematron:compatibility 15:7"));
    SXML_TEST_EXPECT(state, has_line(&warnings, "schematron:compatibility 6:61"));
    received_lines reports;
    memset(&reports, 0, sizeof(reports));
    sxml_schematron_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    SXML_TEST_EXPECT(state,
                     validate(&test_fixture,
                              &(validation_request){.instance_text = "<doc><item n='1'/><item n='3'/></doc>",
                                                    .schematron = schematron,
                                                    .reports = &reports},
                              &outcome) == SXML_STATUS_OK);
    for (size_t line_index = 0; line_index < reports.line_count; line_index += 1) {
        note(state, reports.lines[line_index]);
    }
    /* The first parameter wins (item), the shared let is 2. */
    SXML_TEST_EXPECT(state, reports.line_count == 1 &&
                                has_line(&reports, "failed-assert  /Q{}doc[1]/Q{}item[2] 1 @6 error Too big."));
    sxml_schematron_destroy(schematron);
    schematron = NULL;
    /* The null sink: compiles all the same. */
    SXML_TEST_EXPECT(state, compile(&test_fixture, &(compile_request){.text = schema, .errors = NULL},
                                    &schematron) == SXML_STATUS_OK);
    sxml_schematron_destroy(schematron);
    schematron = NULL;
    /* A let after a rule, in its pattern, stays an error. */
    static const char wrong[] = SCHEMA_START
        "  <sch:pattern>\n"
        "    <sch:rule context='doc'><sch:assert test='true()'>Never.</sch:assert></sch:rule>\n"
        "    <sch:let name='late' value='1'/>\n"
        "  </sch:pattern>\n"
        "</sch:schema>";
    received_lines errors;
    memset(&errors, 0, sizeof(errors));
    SXML_TEST_EXPECT(state, compile(&test_fixture, &(compile_request){.text = wrong, .errors = &errors},
                                    &schematron) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, has_line(&errors, "schematron:syntax 4:5"));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_dynamic_error_names_its_assertion(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char schema[] =
        SCHEMA_START "  <sch:pattern><sch:rule context='item'>\n"
                     "    <sch:assert test='zero-or-one(../item)'>One.</sch:assert>\n"
                     "  </sch:rule></sch:pattern>\n"
                     "</sch:schema>";
    sxml_schematron *schematron = NULL;
    SXML_TEST_EXPECT(state,
                     compile(&test_fixture, &(compile_request){.text = schema, .errors = NULL},
                             &schematron) == SXML_STATUS_OK);
    sxml_item instance;
    SXML_TEST_EXPECT(state,
                     sxml_processor_parse_document(
                         test_fixture.processor,
                         &(sxml_xml_parse_request){.input = TEXT("<doc><item/><item/></doc>")},
                         &instance) == SXML_STATUS_OK);
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    sxml_schematron_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    SXML_TEST_EXPECT(
        state, sxml_schematron_validate_node(
                   schematron,
                   &(sxml_schematron_validation_request){.node = instance, .error_record = &record},
                   &outcome) != SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(record.code.local_name, TEXT("FORG0003")));
    SXML_TEST_EXPECT(state, record.diagnostic.location.line_number == 3);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(record.diagnostic.location.source_name, TEXT(SCHEMA_URI)));
    sxml_item_release(instance);
    sxml_schematron_destroy(schematron);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* The 2025 constructs: group, visit-each, severity, a dynamic role, let
   with as, an expression rule context, phase from, #ANY with when, the
   schematronEdition reported. */
static const char edition_schema[] = SCHEMA_START
    "  <sch:ns prefix='xs' uri='http://www.w3.org/2001/XMLSchema'/>\n"
    "  <sch:let name='kind' as='xs:string' value=\"'checked'\"/>\n"
    "  <sch:phase id='lists' from='//list' when='exists(//list)'><sch:active pattern='g'/>"
    "</sch:phase>\n"
    "  <sch:phase id='other'><sch:active pattern='e'/></sch:phase>\n"
    "  <sch:group id='g'>\n"
    "    <sch:rule context='list' visit-each='entry'>\n"
    "      <sch:assert test='@v' severity='warning' role='{$kind}'>Entry has v.</sch:assert>\n"
    "    </sch:rule>\n"
    "    <sch:rule context='list'>\n"
    "      <sch:report test='true()' id='group-goes-on'>Groups go on.</sch:report>\n"
    "    </sch:rule>\n"
    "  </sch:group>\n"
    "  <sch:pattern id='e'>\n"
    "    <sch:rule context='(//entry)[1]'>\n"
    "      <sch:report test='true()' id='first'>First.</sch:report>\n"
    "    </sch:rule>\n"
    "  </sch:pattern>\n"
    "</sch:schema>";

static void test_edition_2025_constructs(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char *const instances[] = {"<doc><list><entry v='1'/><entry/></list></doc>",
                                            "<doc><entry/></doc>"};
    static const char *const phases[] = {"#ANY", "#ALL"};
    for (size_t phase_index = 0; phase_index < 2; phase_index += 1) {
        sxml_schematron *schematron = NULL;
        received_lines errors;
        memset(&errors, 0, sizeof(errors));
        SXML_TEST_EXPECT(state, compile(&test_fixture,
                                        &(compile_request){.text = edition_schema,
                                                           .phase_id = phases[phase_index],
                                                           .errors = &errors},
                                        &schematron) == SXML_STATUS_OK);
        for (size_t line_index = 0; line_index < errors.line_count; line_index += 1) {
            note(state, errors.lines[line_index]);
        }
        if (schematron == NULL) {
            continue;
        }
        sxml_schematron_description description;
        sxml_schematron_describe(schematron, &description);
        SXML_TEST_EXPECT(state,
                         sxml_byte_view_equals(description.schematron_edition, TEXT("2025")));
        SXML_TEST_EXPECT(state,
                         sxml_byte_view_equals(description.phase_id,
                                               sxml_byte_view_from_c_string(phases[phase_index])));
        for (size_t instance_index = 0; instance_index < 2; instance_index += 1) {
            received_lines reports;
            memset(&reports, 0, sizeof(reports));
            sxml_schematron_outcome outcome;
            memset(&outcome, 0, sizeof(outcome));
            SXML_TEST_EXPECT(
                state, validate(&test_fixture,
                                &(validation_request){.instance_text = instances[instance_index],
                                                      .schematron = schematron,
                                                      .reports = &reports},
                                &outcome) == SXML_STATUS_OK);
            for (size_t line_index = 0; line_index < reports.line_count; line_index += 1) {
                note(state, reports.lines[line_index]);
            }
            const bool chooses_lists = phase_index == 0 && instance_index == 0;
            if (phase_index == 0) {
                /* #ANY: "lists" when the instance has a list, else every
                   pattern. */
                SXML_TEST_EXPECT(
                    state, sxml_byte_view_equals(outcome.phase_id,
                                                 chooses_lists ? TEXT("lists") : TEXT("#ALL")));
            }
            if (instance_index == 0) {
                SXML_TEST_EXPECT(state, has_line(&reports, "failed-assert  "
                                                           "/Q{}doc[1]/Q{}list[1]/Q{}entry[2] 1 @8 "
                                                           "Entry has v. severity=warning"));
                SXML_TEST_EXPECT(state, has_line(&reports, "successful-report group-goes-on "
                                                           "/Q{}doc[1]/Q{}list[1] 1 @11 error "
                                                           "Groups go on."));
                SXML_TEST_EXPECT(state, has_line(&reports, "successful-report first "
                                                           "/Q{}doc[1]/Q{}list[1]/Q{}entry[1] 1 "
                                                           "@16 error First.") == !chooses_lists);
            } else {
                SXML_TEST_EXPECT(state,
                                 has_line(&reports, "successful-report first "
                                                    "/Q{}doc[1]/Q{}entry[1] 1 @16 error First."));
            }
        }
        sxml_schematron_destroy(schematron);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------- Pattern walk */

/* One schematron pattern per node kind a rule context may match: a
   pattern walk selects attributes, text, comments and processing
   instructions only for patterns whose rules may match them (perf-14),
   so each must still reach its nodes, and the element-only pattern the
   nested elements. */
static const char walk_schema[] = SCHEMA_START
    "  <sch:pattern id='elements'><sch:rule context='item'>"
    "<sch:report test='true()' id='element'>E</sch:report></sch:rule></sch:pattern>\n"
    "  <sch:pattern id='attributes'><sch:rule context='item'>"
    "<sch:report test='false()' id='never'>-</sch:report></sch:rule><sch:rule context='@n'>"
    "<sch:report test='true()' id='attribute'>A</sch:report></sch:rule></sch:pattern>\n"
    "  <sch:pattern id='texts'><sch:rule context='text()'>"
    "<sch:report test='true()' id='text'>T</sch:report></sch:rule></sch:pattern>\n"
    "  <sch:pattern id='comments'><sch:rule context='doc/(comment() | item)/comment()'>"
    "<sch:report test='true()' id='comment'>C</sch:report></sch:rule></sch:pattern>\n"
    "  <sch:pattern id='instructions'><sch:rule context='processing-instruction(p)'>"
    "<sch:report test='true()' id='instruction'>P</sch:report></sch:rule></sch:pattern>\n"
    "  <sch:pattern id='nodes'><sch:rule context='item/node() except item/*'>"
    "<sch:report test='true()' id='node'>N</sch:report></sch:rule></sch:pattern>\n"
    "  <sch:pattern id='expressions'><sch:rule context='if (true()) then //comment() else ()'>"
    "<sch:report test='true()' id='expression'>X</sch:report></sch:rule></sch:pattern>\n"
    "</sch:schema>";

static void test_pattern_walk_reaches_every_kind_a_rule_matches(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_schematron *schematron = NULL;
    SXML_TEST_EXPECT(state, compile(&test_fixture, &(compile_request){.text = walk_schema}, &schematron) ==
                                SXML_STATUS_OK);
    if (schematron != NULL) {
        received_lines reports;
        memset(&reports, 0, sizeof(reports));
        sxml_schematron_outcome outcome;
        memset(&outcome, 0, sizeof(outcome));
        SXML_TEST_EXPECT(state, validate(&test_fixture,
                                         &(validation_request){
                                             .instance_text = "<doc><item n='1'>t<!--c--><?p x?><item/></item></doc>",
                                             .schematron = schematron,
                                             .reports = &reports},
                                         &outcome) == SXML_STATUS_OK);
        for (size_t line_index = 0; line_index < reports.line_count; line_index += 1) {
            note(state, reports.lines[line_index]);
        }
        static const char *const expected[] = {
            "successful-report element /Q{}doc[1]/Q{}item[1] 1 @2 error E",
            "successful-report element /Q{}doc[1]/Q{}item[1]/Q{}item[1] 1 @2 error E",
            "successful-report attribute /Q{}doc[1]/Q{}item[1]/@n 1 @3 error A",
            "successful-report text /Q{}doc[1]/Q{}item[1]/text()[1] 1 @4 error T",
            "successful-report comment /Q{}doc[1]/Q{}item[1]/comment()[1] 1 @5 error C",
            "successful-report instruction /Q{}doc[1]/Q{}item[1]/processing-instruction(p)[1] 1 @6 error P",
            "successful-report node /Q{}doc[1]/Q{}item[1]/text()[1] 1 @7 error N",
            "successful-report node /Q{}doc[1]/Q{}item[1]/comment()[1] 1 @7 error N",
            "successful-report node /Q{}doc[1]/Q{}item[1]/processing-instruction(p)[1] 1 @7 error N",
            "successful-report expression /Q{}doc[1]/Q{}item[1]/comment()[1] 1 @8 error X"};
        const size_t expected_count = sizeof(expected) / sizeof(expected[0]);
        SXML_TEST_EXPECT(state, reports.line_count == expected_count);
        for (size_t line_index = 0; line_index < expected_count; line_index += 1) {
            SXML_TEST_EXPECT(state, line_index < reports.line_count &&
                                        strcmp(reports.lines[line_index], expected[line_index]) == 0);
        }
        sxml_schematron_destroy(schematron);
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* SVRL into a buffer. */
typedef struct text_sink {
    char bytes[8192];
    size_t byte_count;
} text_sink;

static sxml_status write_text(void *state, sxml_byte_view bytes) {
    text_sink *sink = state;
    if (sink->byte_count + bytes.byte_count >= sizeof(sink->bytes)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    memcpy(sink->bytes + sink->byte_count, bytes.bytes, bytes.byte_count);
    sink->byte_count += bytes.byte_count;
    sink->bytes[sink->byte_count] = '\0';
    return SXML_STATUS_OK;
}

static const sxml_output_stream_operations text_sink_operations = {.write = write_text};

static void test_svrl_lists_patterns_rules_and_findings(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_schematron *schematron = NULL;
    SXML_TEST_EXPECT(state, compile(&test_fixture,
                                    &(compile_request){
                                        .text = edition_schema, .phase_id = "#ALL", .errors = NULL},
                                    &schematron) == SXML_STATUS_OK);
    static text_sink svrl;
    svrl.byte_count = 0;
    received_lines reports;
    memset(&reports, 0, sizeof(reports));
    sxml_schematron_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    SXML_TEST_EXPECT(
        state,
        validate(&test_fixture,
                 &(validation_request){
                     .instance_text = "<doc><list><entry/></list></doc>",
                     .svrl_output_stream = {.state = &svrl, .operations = &text_sink_operations},
                     .schematron = schematron,
                     .reports = &reports},
                 &outcome) == SXML_STATUS_OK);
    note(state, svrl.bytes);
    SXML_TEST_EXPECT(state, strstr(svrl.bytes, "<svrl:schematron-output") != NULL);
    SXML_TEST_EXPECT(state, strstr(svrl.bytes, "schematronEdition=\"2025\"") != NULL);
    SXML_TEST_EXPECT(state, strstr(svrl.bytes, "<svrl:active-group id=\"g\"") != NULL);
    SXML_TEST_EXPECT(state, strstr(svrl.bytes, "<svrl:active-pattern id=\"e\"") != NULL);
    SXML_TEST_EXPECT(state, strstr(svrl.bytes, "visit-each=\"entry\"") != NULL);
    SXML_TEST_EXPECT(state, strstr(svrl.bytes, "severity=\"warning\"") != NULL);
    SXML_TEST_EXPECT(state, strstr(svrl.bytes, "role=\"checked\"") != NULL);
    sxml_schematron_destroy(schematron);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------ Lifecycle */

static void test_lifecycle_in_caller_memory(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_schematron_memory_requirements(
                                &(sxml_schematron_configuration){.processor = NULL},
                                &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    const sxml_schematron_configuration configuration = {
        .processor = test_fixture.processor,
        .document_source = SXML_SCHEMATRON_DOCUMENT_SOURCE_TEXT,
        .document_text = sxml_byte_view_from_c_string(edition_schema)};
    SXML_TEST_EXPECT(state, sxml_schematron_memory_requirements(&configuration, &requirements) ==
                                SXML_STATUS_OK);
    static _Alignas(16) uint8_t memory[4096];
    SXML_TEST_EXPECT(state,
                     requirements.byte_count <= sizeof(memory) && requirements.alignment <= 16);
    sxml_schematron *schematron = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_schematron_initialize(
                         (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
                         &configuration, &schematron) == SXML_STATUS_OK);
    sxml_schematron_finalize(schematron);
    sxml_schematron_destroy(NULL);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A schema with a variable, a group, visit-each and a value-of, small
   enough to fail each of its allocations in turn. */
static const char small_schema[] =
    SCHEMA_START "  <sch:let name='limit' value='1'/>\n"
                 "  <sch:group>\n"
                 "    <sch:rule context='list' visit-each='entry'>\n"
                 "      <sch:assert test='@v'>Entry <sch:value-of select='$limit'/>.</sch:assert>\n"
                 "    </sch:rule>\n"
                 "  </sch:group>\n"
                 "</sch:schema>";

/* Compiles and validates with allocation failing_number failing. */
static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(failing_number, &test_fixture);
    sxml_schematron *schematron = NULL;
    if (status == SXML_STATUS_OK) {
        status = compile(&test_fixture, &(compile_request){.text = small_schema, .errors = NULL},
                         &schematron);
    }
    if (status == SXML_STATUS_OK) {
        received_lines reports;
        memset(&reports, 0, sizeof(reports));
        sxml_schematron_outcome outcome;
        memset(&outcome, 0, sizeof(outcome));
        status = validate(
            &test_fixture,
            &(validation_request){.instance_text = "<doc><list><entry v='1'/><entry/></list></doc>",
                                  .schematron = schematron,
                                  .reports = &reports},
            &outcome);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, outcome.finding_count == 1);
        }
    }
    sxml_schematron_destroy(schematron);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_survives_every_allocation_failure(sxml_test_state *state) {
    uint64_t failing_number = 1;
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    for (; failing_number < 100000 && status != SXML_STATUS_OK; failing_number += 1) {
        status = run_with_failure(state, failing_number);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            note(state, (const char *)sxml_status_name(status).bytes);
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
    char count_text[64];
    (void)snprintf(count_text, sizeof(count_text), "allocations: %llu",
                   (unsigned long long)failing_number);
    note(state, count_text);
}

static const sxml_test_case schematron_test_cases[] = {
    {.name = "placeholders_replace_whole_name_tokens",
     .function = test_placeholders_replace_whole_name_tokens},
    {.name = "compile_reports_every_error_with_its_location",
     .function = test_compile_reports_every_error_with_its_location},
    {.name = "compile_refuses_unsupported_query_bindings",
     .function = test_compile_refuses_unsupported_query_bindings},
    {.name = "configured_parameters_replace_schema_parameters",
     .function = test_configured_parameters_replace_schema_parameters},
    {.name = "reports_carry_both_locations", .function = test_reports_carry_both_locations},
    {.name = "reports_name_their_document", .function = test_reports_name_their_document},
    {.name = "compatibility_warnings", .function = test_compatibility_warnings},
    {.name = "dynamic_error_names_its_assertion",
     .function = test_dynamic_error_names_its_assertion},
    {.name = "edition_2025_constructs", .function = test_edition_2025_constructs},
    {.name = "pattern_walk_reaches_every_kind_a_rule_matches",
     .function = test_pattern_walk_reaches_every_kind_a_rule_matches},
    {.name = "svrl_lists_patterns_rules_and_findings",
     .function = test_svrl_lists_patterns_rules_and_findings},
    {.name = "lifecycle_in_caller_memory", .function = test_lifecycle_in_caller_memory},
    {.name = "survives_every_allocation_failure",
     .function = test_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "schematron",
            .cases = schematron_test_cases,
            .case_count = SXML_TEST_COUNT(schematron_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
