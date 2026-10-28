/*
 * Tests for the host shim (bindings/host/sxml_host.h; docs/GLOSSARY.md,
 * "host_shim"; D238), driven as a host binding drives it: a processor with
 * a resolver (a state and a function), parse, compile once and evaluate
 * with a context value and a variable, serialize by the adaptive method
 * and with fn:serialize options, a resolver that fails or has nothing, an
 * XQuery query, and the whole XPath run under every allocation failure
 * (D23). Every case ends with the processor's live allocation count 0.
 *
 * Golden file: bindings/javascript/test/package.test.ts, whose first cases
 * these follow in C, and tests/csv_test.c (fixture, failure loop).
 */
#include "sxml_host.h"

#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "test_runner.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ---------------------------------------------------------------- Resolver */

/* What a test's resolver answers and what it was asked. */
typedef struct resolver_state {
    /* The status resolve returns; SXML_STATUS_OK to answer. */
    sxml_status answer_status;
    /* Answers every request with document_text, unless it is empty. */
    sxml_byte_view document_text;
    /* The last request: kind and absolute URI (copied). */
    uint32_t request_count;
    uint32_t last_resource_kind;
    char last_absolute_uri[128];
} resolver_state;

/* Answers with a copy of the state's document in the requesting
   processor's memory, as a host binding does. */
static uint32_t resolve_resource(void *state, const sxml_host_resource_request *request,
                                 sxml_host_resource *resource) {
    resolver_state *resolver = state;
    resolver->request_count += 1;
    resolver->last_resource_kind = request->resource_kind;
    const size_t uri_byte_count = request->absolute_uri.byte_count;
    if (uri_byte_count < sizeof(resolver->last_absolute_uri)) {
        if (uri_byte_count > 0) {
            memcpy(resolver->last_absolute_uri, request->absolute_uri.bytes, uri_byte_count);
        }
        resolver->last_absolute_uri[uri_byte_count] = '\0';
    }
    const sxml_byte_view text = resolver->document_text;
    if (text.byte_count > 0) {
        uint8_t *bytes = sxml_host_allocate(request->processor, text.byte_count);
        if (bytes == NULL) {
            return SXML_STATUS_OUT_OF_MEMORY;
        }
        memcpy(bytes, text.bytes, text.byte_count);
        /* Answered also when failing: the shim releases the bytes. */
        resource->resource_form = SXML_RESOURCE_FORM_BYTES;
        resource->content = (sxml_byte_view){.bytes = bytes, .byte_count = text.byte_count};
    }
    return (uint32_t)resolver->answer_status;
}

static sxml_host_processor *make_processor(resolver_state *resolver,
                                           uint32_t failing_allocation_number) {
    return sxml_host_processor_create(&(sxml_host_processor_configuration){
        .failing_allocation_number = failing_allocation_number,
        .feature_mask = UINT32_MAX,
        .resolver = {.state = resolver, .resolve = resolve_resource}});
}

static bool output_is(const sxml_host_processor *processor, const char *expected) {
    const sxml_byte_view *output = sxml_host_output_of(processor);
    const size_t expected_byte_count = strlen(expected);
    return output->byte_count == expected_byte_count &&
           (expected_byte_count == 0 || memcmp(output->bytes, expected, expected_byte_count) == 0);
}

static bool error_code_is(const sxml_host_processor *processor, const char *expected) {
    const sxml_byte_view code = sxml_host_error_of(processor)->code_local_name;
    const size_t expected_byte_count = strlen(expected);
    return code.byte_count == expected_byte_count &&
           memcmp(code.bytes, expected, expected_byte_count) == 0;
}

/* --------------------------------------------------------------- Scenario */

/* The XPath run the cases share, as text the adaptive method writes. */
typedef struct xpath_outcome {
    /* The first failing status, or SXML_STATUS_OK. */
    sxml_status status;
    bool adaptive_output_matches;
    bool xml_output_matches;
} xpath_outcome;

/* The values and expressions one run holds, all released at its end. */
typedef struct xpath_handles {
    sxml_host_value *book;
    sxml_host_value *suffix;
    sxml_host_expression *expression;
    sxml_host_value *result;
    sxml_host_expression *options_expression;
    sxml_host_value *options;
    sxml_host_expression *title_expression;
    sxml_host_value *title;
} xpath_handles;

static void release_handles(const xpath_handles *handles) {
    sxml_host_value_release(handles->title);
    sxml_host_expression_destroy(handles->title_expression);
    sxml_host_value_release(handles->options);
    sxml_host_expression_destroy(handles->options_expression);
    sxml_host_value_release(handles->result);
    sxml_host_expression_destroy(handles->expression);
    sxml_host_value_release(handles->suffix);
    sxml_host_value_release(handles->book);
}

static sxml_status failure_of(const sxml_host_processor *processor) {
    const uint32_t status = sxml_host_error_of(processor)->status;
    return status == SXML_STATUS_OK ? SXML_STATUS_OUT_OF_MEMORY : (sxml_status)status;
}

/* Compiles text without namespaces or variables. */
static sxml_host_expression *compile_plain(sxml_host_processor *processor,
                                           sxml_byte_view text) {
    return sxml_host_compile(processor,
                             &(sxml_host_compile_request){.expression_text = text,
                                                          .static_base_uri = TEXT(""),
                                                          .namespaces = NULL,
                                                          .namespace_count = 0,
                                                          .variables = NULL,
                                                          .variable_count = 0});
}

static sxml_host_value *evaluate_plain(sxml_host_expression *expression,
                                       const sxml_host_value *context) {
    return sxml_host_evaluate(expression,
                              &(sxml_host_evaluate_request){.context_value = context,
                                                            .variable_values = NULL,
                                                            .variable_value_count = 0});
}

/* Parse, compile and evaluate with a variable and fn:doc through the
   resolver, then serialize by the adaptive method and as XML. */
static sxml_status run_xpath_steps(sxml_host_processor *processor, xpath_handles *handles,
                                   xpath_outcome *outcome) {
    handles->book = sxml_host_parse(
        processor,
        &(sxml_host_parse_request){
            .input = TEXT("<book><title>XPath</title><year>2026</year></book>"),
            .document_uri = TEXT("http://example.org/book.xml")});
    if (handles->book == NULL) {
        return failure_of(processor);
    }
    handles->suffix = sxml_host_string(processor, &TEXT("from C"));
    if (handles->suffix == NULL) {
        return failure_of(processor);
    }
    const sxml_host_variable variable = {.namespace_uri = TEXT(""), .local_name = TEXT("suffix")};
    handles->expression = sxml_host_compile(
        processor,
        &(sxml_host_compile_request){
            .expression_text = TEXT("//title/string(), xs:integer(//year) + 1, "
                                    "doc('note.xml')/note/string(), $suffix"),
            .static_base_uri = TEXT("http://example.org/"),
            .namespaces = NULL,
            .namespace_count = 0,
            .variables = &variable,
            .variable_count = 1});
    if (handles->expression == NULL) {
        return failure_of(processor);
    }
    const sxml_host_value *variable_values[] = {handles->suffix};
    handles->result = sxml_host_evaluate(
        handles->expression, &(sxml_host_evaluate_request){.context_value = handles->book,
                                                           .variable_values = variable_values,
                                                           .variable_value_count = 1});
    if (handles->result == NULL) {
        return failure_of(processor);
    }
    uint32_t status = sxml_host_serialize(
        processor, &(sxml_host_serialize_request){.value = handles->result, .options = NULL});
    if (status != SXML_STATUS_OK) {
        return (sxml_status)status;
    }
    outcome->adaptive_output_matches =
        output_is(processor, "\"XPath\"\n2027\n\"resolved\"\n\"from C\"");
    handles->options_expression =
        compile_plain(processor, TEXT("map { 'method': 'xml', 'omit-xml-declaration': true() }"));
    if (handles->options_expression == NULL) {
        return failure_of(processor);
    }
    handles->options = evaluate_plain(handles->options_expression, NULL);
    if (handles->options == NULL) {
        return failure_of(processor);
    }
    handles->title_expression = compile_plain(processor, TEXT("//title"));
    if (handles->title_expression == NULL) {
        return failure_of(processor);
    }
    handles->title = evaluate_plain(handles->title_expression, handles->book);
    if (handles->title == NULL) {
        return failure_of(processor);
    }
    status = sxml_host_serialize(processor, &(sxml_host_serialize_request){
                                                .value = handles->title,
                                                .options = handles->options});
    if (status != SXML_STATUS_OK) {
        return (sxml_status)status;
    }
    outcome->xml_output_matches = output_is(processor, "<title>XPath</title>");
    return SXML_STATUS_OK;
}

static xpath_outcome run_xpath(sxml_host_processor *processor) {
    xpath_outcome outcome = {.status = SXML_STATUS_OK,
                             .adaptive_output_matches = false,
                             .xml_output_matches = false};
    xpath_handles handles;
    memset(&handles, 0, sizeof(handles));
    outcome.status = run_xpath_steps(processor, &handles, &outcome);
    release_handles(&handles);
    return outcome;
}

static resolver_state note_resolver(void) {
    resolver_state resolver;
    memset(&resolver, 0, sizeof(resolver));
    resolver.answer_status = SXML_STATUS_OK;
    resolver.document_text = TEXT("<note>resolved</note>");
    return resolver;
}

/* ------------------------------------------------------------------ Cases */

static void test_xpath_through_a_resolver(sxml_test_state *state) {
    resolver_state resolver = note_resolver();
    sxml_host_processor *processor = make_processor(&resolver, 0);
    SXML_TEST_REQUIRE(state, processor != NULL);
    const xpath_outcome outcome = run_xpath(processor);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, outcome.adaptive_output_matches);
    SXML_TEST_EXPECT(state, outcome.xml_output_matches);
    SXML_TEST_EXPECT(state, resolver.request_count == 1);
    SXML_TEST_EXPECT(state, resolver.last_resource_kind == SXML_RESOURCE_KIND_XML_DOCUMENT);
    SXML_TEST_EXPECT(state,
                     strcmp(resolver.last_absolute_uri, "http://example.org/note.xml") == 0);
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

static void test_resolver_failure_stops_the_call(sxml_test_state *state) {
    resolver_state resolver = note_resolver();
    resolver.answer_status = SXML_STATUS_INPUT_OUTPUT_FAILED;
    sxml_host_processor *processor = make_processor(&resolver, 0);
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_host_expression *expression = compile_plain(processor, TEXT("doc('a.xml')"));
    SXML_TEST_REQUIRE(state, expression != NULL);
    sxml_host_value *result = evaluate_plain(expression, NULL);
    SXML_TEST_EXPECT(state, result == NULL);
    SXML_TEST_EXPECT(state,
                     sxml_host_error_of(processor)->status == SXML_STATUS_INPUT_OUTPUT_FAILED);
    SXML_TEST_EXPECT(state, resolver.request_count == 1);
    sxml_host_value_release(result);
    sxml_host_expression_destroy(expression);
    /* The bytes the failing resolver answered with were released. */
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

static void test_missing_resource_is_fodc0002(sxml_test_state *state) {
    resolver_state resolver = note_resolver();
    resolver.document_text = TEXT("");
    sxml_host_processor *processor = make_processor(&resolver, 0);
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_host_expression *expression = compile_plain(processor, TEXT("doc('missing.xml')"));
    SXML_TEST_REQUIRE(state, expression != NULL);
    sxml_host_value *result = evaluate_plain(expression, NULL);
    SXML_TEST_EXPECT(state, result == NULL);
    /* F&O 4.0, "fn:doc": FODC0002 when the resource cannot be retrieved. */
    SXML_TEST_EXPECT(state, error_code_is(processor, "FODC0002"));
    sxml_host_value_release(result);
    sxml_host_expression_destroy(expression);
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

static void test_no_resolver_function_has_no_resources(sxml_test_state *state) {
    sxml_host_processor *processor = sxml_host_processor_create(
        &(sxml_host_processor_configuration){.failing_allocation_number = 0,
                                             .feature_mask = 0,
                                             .resolver = {.state = NULL, .resolve = NULL}});
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_host_expression *expression = compile_plain(processor, TEXT("doc-available('a.xml')"));
    SXML_TEST_REQUIRE(state, expression != NULL);
    sxml_host_value *result = evaluate_plain(expression, NULL);
    SXML_TEST_REQUIRE(state, result != NULL);
    SXML_TEST_EXPECT(state, sxml_host_serialize(processor, &(sxml_host_serialize_request){
                                                               .value = result,
                                                               .options = NULL}) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(processor, "false()"));
    sxml_host_value_release(result);
    sxml_host_expression_destroy(expression);
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

static void test_features_are_those_of_the_full_link(sxml_test_state *state) {
    const uint32_t feature_count = sxml_host_feature_count();
    SXML_TEST_EXPECT(state, feature_count > 0);
    SXML_TEST_EXPECT(state, sxml_host_feature_name_byte_count(0) > 0);
    SXML_TEST_EXPECT(state, sxml_host_feature_name_bytes(feature_count) == NULL);
    SXML_TEST_EXPECT(state, sxml_host_feature_name_byte_count(feature_count) == 0);
    SXML_TEST_EXPECT(state, sxml_host_version_text()[0] != '\0');
}

static void test_xquery_compile_evaluate_serialize(sxml_test_state *state) {
    resolver_state resolver = note_resolver();
    sxml_host_processor *processor = make_processor(&resolver, 0);
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_host_query *query = sxml_host_query_compile(
        processor,
        &(sxml_host_query_compile_request){
            .query_text = TEXT("declare option output:method 'text'; "
                               "string-join((1 to 3) ! string(), '-')"),
            .static_base_uri = TEXT(""),
            .namespaces = NULL,
            .namespace_count = 0});
    SXML_TEST_REQUIRE(state, query != NULL);
    SXML_TEST_EXPECT(state, sxml_host_query_is_updating(query) == 0);
    sxml_host_query_evaluate_request request = {
        .context_value = NULL,
        .variables = {.bindings = NULL, .binding_count = 0},
        .accepts_put_documents = 0,
        .side_results = {.side_results = NULL, .side_result_count = 0}};
    sxml_host_value *result = sxml_host_query_evaluate(query, &request);
    SXML_TEST_EXPECT(state, result != NULL);
    SXML_TEST_EXPECT(state, request.side_results.side_result_count == 0);
    if (result != NULL) {
        SXML_TEST_EXPECT(state, sxml_host_query_serialize(query, result) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, output_is(processor, "1-2-3"));
    }
    sxml_host_value_release(result);
    sxml_host_query_destroy(query);
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

/* -------------------------------------------------------------- Schematron */

/* True when the processor's output contains text. */
static bool output_contains(const sxml_host_processor *processor, const char *text) {
    const sxml_byte_view *output = sxml_host_output_of(processor);
    const size_t text_byte_count = strlen(text);
    for (size_t offset = 0; offset + text_byte_count <= output->byte_count; offset += 1) {
        if (memcmp(output->bytes + offset, text, text_byte_count) == 0) {
            return true;
        }
    }
    return false;
}

static void test_schematron_compile_describe_validate(sxml_test_state *state) {
    resolver_state resolver = note_resolver();
    sxml_host_processor *processor = make_processor(&resolver, 0);
    SXML_TEST_REQUIRE(state, processor != NULL);
    /* Errors: every one a side result, its code with its prefix. */
    sxml_host_schematron *broken = sxml_host_schematron_create(processor);
    SXML_TEST_REQUIRE(state, broken != NULL);
    sxml_host_schematron_compile_request broken_request = {
        .text = TEXT("<sch:schema xmlns:sch='http://purl.oclc.org/dsdl/schematron'>\n"
                     "<sch:pattern><sch:rule context='a['><sch:assert test='1 +'>x</sch:assert>"
                     "</sch:rule></sch:pattern></sch:schema>"),
        .uri = TEXT("http://example.com/broken.sch")};
    SXML_TEST_EXPECT(state, sxml_host_schematron_compile(broken, &broken_request) ==
                                SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, broken_request.side_results.side_result_count == 2);
    if (broken_request.side_results.side_result_count == 2) {
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(broken_request.side_results.side_results[0].code,
                                                      TEXT("err:XTSE0340")));
        SXML_TEST_EXPECT(state, broken_request.side_results.side_results[1].line_number == 2);
    }
    sxml_host_schematron_destroy(broken);
    /* A schema, described, then a validation of text. */
    sxml_host_schematron *schematron = sxml_host_schematron_create(processor);
    SXML_TEST_REQUIRE(state, schematron != NULL);
    const sxml_host_schematron_parameter limit = {.name = TEXT("limit"), .value = TEXT("1")};
    sxml_host_schematron_compile_request request = {
        .text = TEXT("<sch:schema xmlns:sch='http://purl.oclc.org/dsdl/schematron' "
                     "schematronEdition='2025'>\n"
                     "<sch:title>Items</sch:title><sch:param name='limit' value='5'/>\n"
                     "<sch:pattern id='p'><sch:rule context='item'>\n"
                     "<sch:assert test='@n &lt;= $limit' id='small'>\"<sch:value-of "
                     "select='@n'/>\" is large.</sch:assert>\n"
                     "</sch:rule></sch:pattern></sch:schema>"),
        .uri = TEXT("http://example.com/items.sch"),
        .parameters = &limit,
        .parameter_count = 1};
    SXML_TEST_EXPECT(state, sxml_host_schematron_compile(schematron, &request) == SXML_STATUS_OK);
    sxml_host_schematron_description description;
    sxml_host_schematron_describe(schematron, &description);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(description.title, TEXT("Items")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(description.schematron_edition, TEXT("2025")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(description.phase, TEXT("#ALL")));
    const sxml_host_schematron_validate_request validation = {
        .node = NULL,
        .text = TEXT("<doc>\n<item n='1'/>\n<item n='2'/></doc>"),
        .document_uri = TEXT("http://example.com/doc.xml"),
        .reports_fired_rules = 0,
        .writes_svrl = 1};
    SXML_TEST_EXPECT(state, sxml_host_schematron_validate(schematron, &validation) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_contains(processor, "{\"findings\":[{\"kind\":\"failed-assert\","
                                                       "\"id\":\"small\""));
    SXML_TEST_EXPECT(state, output_contains(processor, "\"message\":\"\\\"2\\\" is large.\""));
    SXML_TEST_EXPECT(state, output_contains(processor, "\"lineNumber\":3"));
    SXML_TEST_EXPECT(state, output_contains(processor, "\"schemaLineNumber\":4"));
    SXML_TEST_EXPECT(state, output_contains(processor, "\"valid\":false,\"phase\":\"#ALL\""));
    SXML_TEST_EXPECT(state, output_contains(processor, "\"svrl\":\"<?xml version=\\\"1.0\\\""));
    sxml_host_schematron_destroy(schematron);
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

static void test_stylesheet_compile_reports_every_error(sxml_test_state *state) {
    resolver_state resolver = note_resolver();
    sxml_host_processor *processor = make_processor(&resolver, 0);
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_host_stylesheet *stylesheet = sxml_host_stylesheet_compile(
        processor,
        &(sxml_host_stylesheet_compile_request){
            .stylesheet_text = TEXT(
                "<xsl:stylesheet version='3.0' xmlns:xsl='http://www.w3.org/1999/XSL/Transform'>\n"
                "<xsl:template match='a'><xsl:value-of select='1 +'/></xsl:template>\n"
                "<xsl:template match='b'><xsl:value-of select='2 *'/></xsl:template>\n"
                "</xsl:stylesheet>"),
            .static_base_uri = TEXT("http://example.com/errors.xsl"),
            .static_parameters = {.bindings = NULL, .binding_count = 0}});
    SXML_TEST_EXPECT(state, stylesheet == NULL);
    SXML_TEST_EXPECT(state, error_code_is(processor, "XPST0003"));
    const sxml_host_side_result_list *reports = sxml_host_compile_reports_of(processor);
    SXML_TEST_EXPECT(state, reports->side_result_count == 2);
    if (reports->side_result_count == 2) {
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(reports->side_results[0].code,
                                                      TEXT("err:XPST0003")));
        SXML_TEST_EXPECT(state, reports->side_results[0].line_number == 2);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(reports->side_results[1].code,
                                                      TEXT("err:XPST0003")));
        SXML_TEST_EXPECT(state, reports->side_results[1].line_number == 3);
    }
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

/* ----------------------------------------------------------------- Streams */

/* What a test's output writer received (D294). */
typedef struct written_output {
    uint8_t bytes[256];
    size_t byte_count;
    size_t chunk_count;
    size_t largest_chunk_byte_count;
    /* Fails the write with this number (counted from 1); 0 fails none. */
    size_t failing_write_number;
} written_output;

static uint32_t write_output(void *state, const sxml_host_output_event *event) {
    written_output *output = state;
    output->chunk_count += 1;
    if (output->chunk_count == output->failing_write_number) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    const sxml_byte_view bytes = event->bytes;
    if (event->output_event_kind == SXML_HOST_OUTPUT_EVENT_KIND_BYTES &&
        output->byte_count + bytes.byte_count <= sizeof(output->bytes)) {
        memcpy(output->bytes + output->byte_count, bytes.bytes, bytes.byte_count);
        output->byte_count += bytes.byte_count;
        if (bytes.byte_count > output->largest_chunk_byte_count) {
            output->largest_chunk_byte_count = bytes.byte_count;
        }
    }
    return SXML_STATUS_OK;
}

/* A query serialized through an output writer arrives in chunks of at
   most the chunk size, and the processor's output stays empty; a failing
   write stops the serialization with its status. */
static void test_output_writer_receives_chunks(sxml_test_state *state) {
    resolver_state resolver = note_resolver();
    sxml_host_processor *processor = make_processor(&resolver, 0);
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_host_expression *expression =
        compile_plain(processor, TEXT("string-join((1 to 20) ! string(), '-')"));
    SXML_TEST_REQUIRE(state, expression != NULL);
    sxml_host_value *result = evaluate_plain(expression, NULL);
    SXML_TEST_REQUIRE(state, result != NULL);
    written_output output;
    memset(&output, 0, sizeof(output));
    SXML_TEST_EXPECT(state, sxml_host_set_output(
                                processor, &(sxml_host_output_configuration){
                                               .writer = {.state = &output, .write = write_output},
                                               .chunk_byte_count = 4}) == SXML_STATUS_OK);
    const sxml_host_serialize_request request = {.value = result, .options = NULL};
    SXML_TEST_EXPECT(state, sxml_host_serialize(processor, &request) == SXML_STATUS_OK);
    const char *expected = "\"1-2-3-4-5-6-7-8-9-10-11-12-13-14-15-16-17-18-19-20\"";
    SXML_TEST_EXPECT(state, output.byte_count == strlen(expected) &&
                                memcmp(output.bytes, expected, output.byte_count) == 0);
    SXML_TEST_EXPECT(state, output.largest_chunk_byte_count == 4);
    SXML_TEST_EXPECT(state, output.chunk_count == (strlen(expected) + 3) / 4);
    SXML_TEST_EXPECT(state, output_is(processor, ""));
    memset(&output, 0, sizeof(output));
    output.failing_write_number = 2;
    SXML_TEST_EXPECT(state,
                     sxml_host_serialize(processor, &request) == SXML_STATUS_INPUT_OUTPUT_FAILED);
    SXML_TEST_EXPECT(state, output.chunk_count == 2);
    /* Without a writer, the output is the processor's again. */
    SXML_TEST_EXPECT(state, sxml_host_set_output(processor, &(sxml_host_output_configuration){
                                                                .writer = {.state = NULL,
                                                                           .write = NULL},
                                                                .chunk_byte_count = 0}) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_host_serialize(processor, &request) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(processor, expected));
    sxml_host_value_release(result);
    sxml_host_expression_destroy(expression);
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

/* A test's byte reader: a text read three bytes at a time. */
typedef struct chunk_reader_state {
    sxml_byte_view text;
    size_t offset;
    uint32_t read_count;
    uint32_t release_count;
    uint32_t released_identifier;
} chunk_reader_state;

static uint32_t read_chunk(void *state, sxml_host_read_request *request) {
    chunk_reader_state *reader = state;
    reader->read_count += 1;
    size_t count = reader->text.byte_count - reader->offset;
    count = count < 3 ? count : 3;
    count = count < request->buffer.byte_count ? count : request->buffer.byte_count;
    memcpy(request->buffer.bytes, reader->text.bytes + reader->offset, count);
    reader->offset += count;
    request->byte_count = (uint32_t)count;
    request->is_end = reader->offset == reader->text.byte_count ? 1 : 0;
    return SXML_STATUS_OK;
}

static void release_chunk_reader(void *state, uint32_t reader_identifier) {
    chunk_reader_state *reader = state;
    reader->release_count += 1;
    reader->released_identifier = reader_identifier;
}

static uint32_t resolve_with_reader(void *state, const sxml_host_resource_request *request,
                                    sxml_host_resource *resource) {
    (void)state;
    (void)request;
    resource->resource_form = SXML_RESOURCE_FORM_BYTE_READER;
    resource->reader_identifier = 7;
    return SXML_STATUS_OK;
}

/* fn:doc reads a document a resolver delivers through the byte reader,
   which is released once; without a byte reader it is not available. */
static void test_byte_reader_delivers_a_document(sxml_test_state *state) {
    chunk_reader_state reader = {.text = TEXT("<a><b>1</b><b>2</b><b>3</b></a>")};
    sxml_host_processor *processor = sxml_host_processor_create(&(sxml_host_processor_configuration){
        .failing_allocation_number = 0,
        .feature_mask = UINT32_MAX,
        .resolver = {.state = NULL, .resolve = resolve_with_reader}});
    SXML_TEST_REQUIRE(state, processor != NULL);
    sxml_host_expression *expression = compile_plain(processor, TEXT("string-join(doc('a.xml')//b)"));
    SXML_TEST_REQUIRE(state, expression != NULL);
    sxml_host_value *missing = evaluate_plain(expression, NULL);
    SXML_TEST_EXPECT(state, missing == NULL && error_code_is(processor, "FODC0002"));
    SXML_TEST_EXPECT(state, sxml_host_set_byte_reader(
                                processor, &(sxml_host_byte_reader){
                                               .state = &reader,
                                               .read = read_chunk,
                                               .release = release_chunk_reader}) ==
                                SXML_STATUS_OK);
    sxml_host_value *result = evaluate_plain(expression, NULL);
    SXML_TEST_REQUIRE(state, result != NULL);
    const sxml_host_serialize_request request = {.value = result, .options = NULL};
    SXML_TEST_EXPECT(state, sxml_host_serialize(processor, &request) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(processor, "\"123\""));
    SXML_TEST_EXPECT(state, reader.read_count >= 11);
    SXML_TEST_EXPECT(state, reader.release_count == 1 && reader.released_identifier == 7);
    sxml_host_value_release(missing);
    sxml_host_value_release(result);
    sxml_host_expression_destroy(expression);
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

/* sxml_host_string takes XML characters only (XML 1.1, production [2]
   Char): U+0000, U+FFFE and U+FFFF are INVALID_ARGUMENT, malformed UTF-8
   is INVALID_UTF8, and the characters around them cross intact. */
static void test_string_refuses_characters_xml_does_not_allow(sxml_test_state *state) {
    sxml_host_processor *processor = make_processor(NULL, 0);
    SXML_TEST_REQUIRE(state, processor != NULL);
    const sxml_byte_view refused[] = {
        {.bytes = (const uint8_t *)"a\0b", .byte_count = 3},
        TEXT("a\xEF\xBF\xBE"),
        TEXT("\xEF\xBF\xBF"),
    };
    for (size_t text_index = 0; text_index < SXML_TEST_COUNT(refused); text_index += 1) {
        SXML_TEST_EXPECT(state, sxml_host_string(processor, &refused[text_index]) == NULL);
        SXML_TEST_EXPECT(state,
                         sxml_host_error_of(processor)->status == SXML_STATUS_INVALID_ARGUMENT);
    }
    SXML_TEST_EXPECT(state, sxml_host_string(processor, &TEXT("\xC0\x80")) == NULL);
    SXML_TEST_EXPECT(state, sxml_host_error_of(processor)->status == SXML_STATUS_INVALID_UTF8);
    /* U+0001, U+FFFD and U+1F600 are XML 1.1 characters. */
    sxml_host_value *control = sxml_host_string(processor, &TEXT("\x01"));
    SXML_TEST_EXPECT(state, control != NULL);
    sxml_host_value_release(control);
    sxml_host_value *kept = sxml_host_string(processor, &TEXT("\xEF\xBF\xBD\xF0\x9F\x98\x80"));
    SXML_TEST_REQUIRE(state, kept != NULL);
    SXML_TEST_EXPECT(state, sxml_host_serialize(processor, &(sxml_host_serialize_request){
                                                               .value = kept,
                                                               .options = NULL}) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(processor, "\"\xEF\xBF\xBD\xF0\x9F\x98\x80\""));
    sxml_host_value_release(kept);
    SXML_TEST_EXPECT(state, sxml_host_processor_destroy(processor) == 0);
}

/* The XPath run with each allocation failing in turn (D23): every run
   fails cleanly or succeeds, and none leaks. */
static void test_xpath_under_every_allocation_failure(sxml_test_state *state) {
    bool succeeded = false;
    for (uint32_t failing_number = 1; !succeeded && failing_number < 100000;
         failing_number += 1) {
        resolver_state resolver = note_resolver();
        sxml_host_processor *processor = make_processor(&resolver, failing_number);
        if (processor == NULL) {
            continue;
        }
        const xpath_outcome outcome = run_xpath(processor);
        succeeded = outcome.status == SXML_STATUS_OK;
        if (succeeded) {
            SXML_TEST_EXPECT(state, outcome.adaptive_output_matches);
            SXML_TEST_EXPECT(state, outcome.xml_output_matches);
        }
        const uint32_t leaked_allocation_count = sxml_host_processor_destroy(processor);
        SXML_TEST_EXPECT(state, leaked_allocation_count == 0);
        if (leaked_allocation_count != 0) {
            return;
        }
    }
    SXML_TEST_EXPECT(state, succeeded);
}

static const sxml_test_case host_shim_test_cases[] = {
    {.name = "xpath_through_a_resolver", .function = test_xpath_through_a_resolver},
    {.name = "resolver_failure_stops_the_call",
     .function = test_resolver_failure_stops_the_call},
    {.name = "missing_resource_is_fodc0002", .function = test_missing_resource_is_fodc0002},
    {.name = "no_resolver_function_has_no_resources",
     .function = test_no_resolver_function_has_no_resources},
    {.name = "features_are_those_of_the_full_link",
     .function = test_features_are_those_of_the_full_link},
    {.name = "xquery_compile_evaluate_serialize",
     .function = test_xquery_compile_evaluate_serialize},
    {.name = "string_refuses_characters_xml_does_not_allow",
     .function = test_string_refuses_characters_xml_does_not_allow},
    {.name = "xpath_under_every_allocation_failure",
     .function = test_xpath_under_every_allocation_failure},
    {.name = "output_writer_receives_chunks", .function = test_output_writer_receives_chunks},
    {.name = "byte_reader_delivers_a_document",
     .function = test_byte_reader_delivers_a_document},
    {.name = "stylesheet_compile_reports_every_error",
     .function = test_stylesheet_compile_reports_every_error},
    {.name = "schematron_compile_describe_validate",
     .function = test_schematron_compile_describe_validate},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){.name = "host_shim",
                           .cases = host_shim_test_cases,
                           .case_count = SXML_TEST_COUNT(host_shim_test_cases)},
        (sxml_test_command_line){.argument_count = argument_count, .arguments = arguments});
}
