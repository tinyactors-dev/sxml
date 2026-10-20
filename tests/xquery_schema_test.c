/*
 * Tests for schema-aware XPath and XQuery (packet 5.11, D174) through the
 * public API alone: in-scope schemas of the host's static context, import
 * schema, validate, schema-element and schema-attribute, casts and
 * constructor functions of schema types, fn:xsd-validator and the
 * xsd-validation option of fn:parse-xml, with the error codes of each.
 * Every case checks for leaked cells and allocations.
 *
 * Golden file: tests/xquery_test.c.
 */
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/serialize.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "sxml/xquery.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ---------------------------------------------------------------- Fixture */

static const char schema_text[] =
    "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema' xmlns:s='urn:s'"
    " targetNamespace='urn:s' elementFormDefault='qualified'>"
    "<xs:simpleType name='size'><xs:restriction base='xs:integer'>"
    "<xs:maxInclusive value='10'/></xs:restriction></xs:simpleType>"
    "<xs:simpleType name='sizes'><xs:list itemType='s:size'/></xs:simpleType>"
    "<xs:element name='hat'><xs:complexType><xs:sequence>"
    "<xs:element name='label' type='xs:string' minOccurs='0'/></xs:sequence>"
    "<xs:attribute name='size' type='s:size'/></xs:complexType></xs:element>"
    "<xs:element name='cap' substitutionGroup='s:hat'/>"
    "<xs:element name='hood' substitutionGroup='s:hat' abstract='true'/>"
    "<xs:attribute name='colour' type='xs:string'/>"
    "</xs:schema>";

static const sxml_resource_entry fixture_resources[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/s.xsd"),
     .content = {.bytes = (const uint8_t *)schema_text, .byte_count = sizeof(schema_text) - 1}},
};

static const sxml_resource_entry_view fixture_resource_view = {
    .resource_entries = fixture_resources,
    .resource_entry_count = sizeof(fixture_resources) / sizeof(fixture_resources[0])};

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number = 0},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    /* fn:xsd-validator compiles its own schemas (options, hints) with the
       builtin-schemas feature's compiler (D225, D278). */
    const sxml_optional_feature feature = sxml_builtin_schemas_feature();
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = sxml_table_resource_resolver(&fixture_resource_view),
            .optional_features = {.optional_features = &feature, .optional_feature_count = 1}},
        &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    return status;
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
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && sxml_counting_allocator_read_counts(
                                         test_fixture->counting_allocator)
                                             .live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

/* --------------------------------------------------------------- Running */

/* One query (XQuery, or XPath when is_xpath) and its static context. */
typedef struct query_run {
    sxml_byte_view expression_text;
    bool is_xpath;
    sxml_static_context_configuration static_context;
    /* Out: the error record of the failure. */
    sxml_error_record error_record;
} query_run;

/* Serializes value as text items joined by spaces into the output
   buffer (the xml method's sequence normalization). */
static sxml_status serialize_value(const fixture *test_fixture, query_run *run,
                                   sxml_sequence value) {
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof parameters);
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
    parameters.omit_xml_declaration = SXML_SERIALIZATION_FLAG_YES;
    return sxml_serialize_sequence(
        sxml_processor_get_item_heap(test_fixture->processor),
        &(sxml_serialization_request){.value = value,
                                      .serialization_parameters = parameters,
                                      .error_record = &run->error_record},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
}

static sxml_status run_xquery(const fixture *test_fixture, query_run *run,
                              sxml_sequence *result) {
    sxml_xquery_query *query = NULL;
    sxml_status status = sxml_xquery_query_create(
        fixture_allocator(test_fixture),
        &(sxml_xquery_query_configuration){.processor = test_fixture->processor,
                                           .expression_text = run->expression_text,
                                           .source_name = TEXT("test.xq"),
                                           .static_context = run->static_context,
                                           .error_record = &run->error_record},
        &query);
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_query_evaluate(
            query, &(sxml_xquery_evaluation_request){.error_record = &run->error_record}, result);
    }
    if (status == SXML_STATUS_OK) {
        status = serialize_value(test_fixture, run, *result);
        sxml_sequence_release(*result);
    }
    sxml_xquery_query_destroy(query);
    return status;
}

static sxml_status run_xpath(const fixture *test_fixture, query_run *run, sxml_sequence *result) {
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){.processor = test_fixture->processor,
                                               .expression_text = run->expression_text,
                                               .source_name = TEXT("test.xp"),
                                               .static_context = run->static_context,
                                               .error_record = &run->error_record},
        &expression);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression, &(sxml_xpath_evaluation_request){.error_record = &run->error_record},
            result);
    }
    if (status == SXML_STATUS_OK) {
        status = serialize_value(test_fixture, run, *result);
        sxml_sequence_release(*result);
    }
    sxml_xpath_expression_destroy(expression);
    return status;
}

/* Compiles, evaluates and serializes run's query into the output buffer
   (cleared first). */
static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_sequence result = sxml_sequence_empty();
    return run->is_xpath ? run_xpath(test_fixture, run, &result)
                         : run_xquery(test_fixture, run, &result);
}

/* ---------------------------------------------------------- Query cases */

/* A query and its serialized result, or the error code it raises. */
typedef struct query_case {
    const char *query;
    const char *expected_output;
    const char *expected_code;
} query_case;

#define IMPORT "import schema namespace s = 'urn:s' at 'http://example.com/s.xsd'; "

static const query_case xquery_cases[] = {
    /* Casts and constructor functions of schema types (XPath 4.0, 4.5.1.3,
       5.11 of XQuery 4.0). */
    {IMPORT "8 cast as s:size", "8", NULL},
    {IMPORT "11 castable as s:size", "false", NULL},
    {IMPORT "s:size(3) instance of xs:integer", "true", NULL},
    {IMPORT "count(s:sizes('1 2 3'))", "3", NULL},
    {IMPORT "s:size(12)", NULL, "FORG0001"},
    /* A union with a list member is not an item type (3.2.3). */
    {IMPORT "1 instance of s:sizes", NULL, "XPST0051"},
    /* Validation (4.25). */
    {IMPORT "validate { <s:hat size='3'/> } instance of schema-element(s:hat)", "true", NULL},
    {IMPORT "data((validate { <s:hat size='3'/> })/@size) instance of s:size", "true", NULL},
    {IMPORT "validate { <s:cap/> } instance of schema-element(s:hat)", "true", NULL},
    {IMPORT "<s:cap/> instance of schema-element(s:hat)", "false", NULL},
    {IMPORT "validate { <s:hat size='30'/> }", NULL, "XQDY0027"},
    {IMPORT "validate { <s:shoe/> }", NULL, "XQDY0084"},
    {IMPORT "validate lax { <s:shoe/> } instance of element(*, xs:anyType)", "true", NULL},
    {IMPORT "validate { 1 }", NULL, "XQTY0030"},
    {IMPORT "validate { document { <s:hat/>, <s:hat/> } }", NULL, "XQDY0061"},
    {IMPORT "validate type s:no-such-type { <s:hat/> }", NULL, "XQST0104"},
    {IMPORT "validate type xs:untyped { validate { <s:hat size='3'/> } } instance of "
            "element(*, xs:untyped)",
     "true", NULL},
    {"validate lax { <a xmlns:xs='http://www.w3.org/2001/XMLSchema' xsi:type='xs:integer'>42</a> "
     "} instance of element(a, xs:integer)",
     "true", NULL},
    /* xsi:type resolves with the element's in-scope namespaces only (cvc-elt.4.1;
       QT4 validateexpr-34, D271). */
    {"validate lax { <a xsi:type='xs:integer'>42</a> }", NULL, "XQDY0027"},
    /* xml:id is of type ID without the xml namespace's schema (xml:id
       1.0, 4), so an IDREF may name it (D275). */
    {"let $d := validate lax { document { <r><e xml:id=' i1 '/><f xmlns:xs='http://www.w3.org/"
     "2001/XMLSchema' xsi:type='xs:IDREF'>i1</f></r> } } "
     "return (data($d//e/@xml:id) instance of xs:ID, name(id('i1', $d)), name(idref('i1', $d)))",
     "true e f", NULL},
    {IMPORT "validate { attribute s:colour { 'red' } }", NULL, "XQTY0030"},
    /* schema-element and schema-attribute (3.2.7.3, 3.2.7.5). */
    {IMPORT "() instance of schema-element(s:shoe)", NULL, "XPST0008"},
    {IMPORT "() instance of schema-attribute(s:colour)", "false", NULL},
    {IMPORT "validate { <s:hood/> }", NULL, "XQDY0027"},
    /* Import errors (5.11). */
    {"import schema namespace t = 'urn:t'; 1", NULL, "XQST0059"},
    {"import schema namespace s = 'urn:s' at 'http://example.com/s.xsd'; "
     "import schema namespace t = 'urn:s'; 1",
     NULL, "XQST0058"},
    {"import schema namespace s = 'urn:t' at 'http://example.com/s.xsd'; 1", NULL, "XQST0059"},
    {"import schema namespace s = 'urn:s' at 'http://example.com/none.xsd', "
     "'http://example.com/s.xsd'; 8 cast as s:size",
     "8", NULL},
    {"import schema default element namespace 'urn:s' at 'http://example.com/s.xsd'; "
     "validate { <hat/> } instance of schema-element(hat)",
     "true", NULL},
    /* fn:xsd-validator (F&O 4.0, 17.2.5). */
    {IMPORT "xsd-validator()(<s:hat size='3'/>)?is-valid", "true", NULL},
    {IMPORT "xsd-validator()(<s:hat size='30'/>)?is-valid", "false", NULL},
    {IMPORT "exists(xsd-validator({'return-error-details': true()})"
            "(<s:hat size='30'/>)?error-details)",
     "true", NULL},
    {IMPORT "data(xsd-validator({'type': xs:QName('s:size')})(attribute a { 7 })?typed-node) "
            "instance of s:size",
     "true", NULL},
    {IMPORT "xsd-validator()(())", "", NULL},
    {IMPORT "xsd-validator({'validation-mode': 'loose'})", NULL, "XPTY0004"},
    /* use-xsi-schema-location (17.2.5; D278): the target's hints are read
       through the resolver; without the option they are not; a hint that
       does not compile is an invalidity, not an error. */
    {"xsd-validator({'use-xsi-schema-location': true()})(<hat xmlns='urn:s' size='3' "
     "xsi:schemaLocation='urn:s http://example.com/s.xsd'/>)?is-valid",
     "true", NULL},
    {"xsd-validator({'use-xsi-schema-location': true()})(<hat xmlns='urn:s' size='30' "
     "xsi:schemaLocation='urn:s http://example.com/s.xsd'/>)?is-valid",
     "false", NULL},
    {"xsd-validator()(<hat xmlns='urn:s' size='3' "
     "xsi:schemaLocation='urn:s http://example.com/s.xsd'/>)?is-valid",
     "false", NULL},
    {"xsd-validator({'use-xsi-schema-location': true(), 'return-error-details': true()})"
     "(<hat xmlns='urn:x' xsi:schemaLocation='urn:x http://example.com/s.xsd'/>)"
     "?error-details?rule",
     "schema_reference", NULL},
    /* fn:parse-xml's use-xsi-schema-location (17.2.1; D278). */
    {"data(parse-xml('<hat xmlns=\"urn:s\" size=\"4\" xmlns:xsi=\"http://www.w3.org/2001/"
     "XMLSchema-instance\" xsi:schemaLocation=\"urn:s http://example.com/s.xsd\"/>', "
     "{'xsd-validation': 'strict', 'use-xsi-schema-location': true()})/*/@size) "
     "instance of xs:integer",
     "true", NULL},
    /* The xsd-validation option of fn:parse-xml (17.2.1). */
    {IMPORT "data(parse-xml('<hat xmlns=\"urn:s\" size=\"4\"/>', {'xsd-validation': 'strict'})"
            "/s:hat/@size) instance of s:size",
     "true", NULL},
    {IMPORT "parse-xml('<hat xmlns=\"urn:s\" size=\"40\"/>', {'xsd-validation': 'strict'})", NULL,
     "FODC0014"},
    {IMPORT "parse-xml('<a/>', {'xsd-validation': 'maybe'})", NULL, "FODC0008"},
};

static void test_xquery_is_schema_aware(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(xquery_cases); case_index += 1) {
        const query_case *query = &xquery_cases[case_index];
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(&test_fixture) == SXML_STATUS_OK);
        query_run run = {
            .expression_text = {.bytes = (const uint8_t *)query->query,
                                .byte_count = strlen(query->query)},
            .is_xpath = false};
        const sxml_status status = run_query(&test_fixture, &run);
        bool matches = false;
        if (query->expected_code == NULL) {
            const sxml_byte_view expected = {.bytes = (const uint8_t *)query->expected_output,
                                             .byte_count = strlen(query->expected_output)};
            matches = status == SXML_STATUS_OK &&
                      sxml_byte_view_equals(
                          sxml_output_buffer_get_bytes(test_fixture.output_buffer), expected);
        } else {
            matches = status != SXML_STATUS_OK &&
                      sxml_byte_view_equals(
                          run.error_record.code.local_name,
                          (sxml_byte_view){.bytes = (const uint8_t *)query->expected_code,
                                           .byte_count = strlen(query->expected_code)});
        }
        if (!matches) {
            const sxml_byte_view output = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
            (void)fprintf(stderr, "query %s: status %d, code %.*s, output %.*s\n", query->query,
                           (int)status, (int)run.error_record.code.local_name.byte_count,
                           (const char *)run.error_record.code.local_name.bytes,
                           (int)output.byte_count, (const char *)output.bytes);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
}

/* ------------------------------------------------ The host's schemas */

/* The XPath static context names a schema the host compiled: its types,
   declarations and substitution groups are in scope (XPath 4.0, 2.2.1),
   and an XQuery import of its namespace takes it from there. */
static void test_host_schemas_are_in_scope(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&test_fixture) == SXML_STATUS_OK);
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof error_record);
    sxml_schema *schema = NULL;
    SXML_TEST_REQUIRE(
        state,
        sxml_schema_create(
            fixture_allocator(&test_fixture),
            &(sxml_schema_configuration){
                .processor = test_fixture.processor,
                .schema_documents = {.schema_documents =
                                         &(sxml_schema_document){
                                             .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                             .document_text = {.bytes = (const uint8_t *)schema_text,
                                                               .byte_count =
                                                                   sizeof(schema_text) - 1},
                                             .document_uri = TEXT("http://example.com/s.xsd")},
                                     .schema_document_count = 1},
                .error_record = &error_record},
            &schema) == SXML_STATUS_OK);
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(test_fixture.processor);
    sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                      .namespace_uri = {.index = 0}};
    SXML_TEST_REQUIRE(state, sxml_name_pool_intern_namespace_prefix(name_pool, TEXT("s"),
                                                                    &binding.namespace_prefix) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_name_pool_intern_namespace_uri(name_pool, TEXT("urn:s"),
                                                                 &binding.namespace_uri) ==
                                 SXML_STATUS_OK);
    const sxml_schema *const schemas[1] = {schema};
    const sxml_static_context_configuration static_context = {
        .namespace_bindings = {.namespace_bindings = &binding, .namespace_binding_count = 1},
        .in_scope_schemas = {.schemas = schemas, .schema_count = 1}};
    static const struct {
        const char *expression;
        bool is_xpath;
        const char *expected_output;
    } cases[] = {
        {"s:size(4) instance of s:size", true, "true"},
        {"function-lookup(xs:QName('s:size'), 1)(5) instance of s:size", true, "true"},
        {"() instance of schema-element(s:cap)", true, "false"},
        {"xsd-validator()(<s:hat size='3'/>)?is-valid", false, "true"},
        {"import schema namespace s = 'urn:s'; 9 cast as s:size", false, "9"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        query_run run = {
            .expression_text = {.bytes = (const uint8_t *)cases[case_index].expression,
                                .byte_count = strlen(cases[case_index].expression)},
            .is_xpath = cases[case_index].is_xpath,
            .static_context = static_context};
        const sxml_status status = run_query(&test_fixture, &run);
        const sxml_byte_view expected = {
            .bytes = (const uint8_t *)cases[case_index].expected_output,
            .byte_count = strlen(cases[case_index].expected_output)};
        const bool matches =
            status == SXML_STATUS_OK &&
            sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                  expected);
        if (!matches) {
            (void)fprintf(stderr, "expression %s: status %d, code %.*s\n",
                           cases[case_index].expression, (int)status,
                           (int)run.error_record.code.local_name.byte_count,
                           (const char *)run.error_record.code.local_name.bytes);
        }
        SXML_TEST_EXPECT(state, matches);
    }
    /* A schema of another processor is an invalid configuration. */
    fixture other_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&other_fixture) == SXML_STATUS_OK);
    query_run run = {.expression_text = TEXT("1"),
                     .is_xpath = true,
                     .static_context = {.in_scope_schemas = {.schemas = schemas,
                                                             .schema_count = 1}}};
    SXML_TEST_EXPECT(state, run_query(&other_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&other_fixture));
    sxml_schema_destroy(schema);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static const sxml_test_case xquery_schema_test_cases[] = {
    {.name = "xquery_is_schema_aware", .function = test_xquery_is_schema_aware},
    {.name = "host_schemas_are_in_scope", .function = test_host_schemas_are_in_scope},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery_schema",
            .cases = xquery_schema_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_schema_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
