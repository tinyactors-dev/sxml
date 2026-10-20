/*
 * Tests for schema-aware XSLT (packet 5.12, D179) through the public API
 * alone: xsl:import-schema with a schema-location and an inline
 * xs:schema, the validation and type attributes of the instructions and
 * literal result elements, default-validation, input-type-annotations,
 * type-available and is-schema-aware, the host's schemas, with the error
 * codes of each. Every case checks for leaked cells and allocations.
 *
 * Golden file: tests/xquery_schema_test.c.
 */
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/serialize.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "sxml/xslt.h"
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
    "<xs:complexType name='hat-type'><xs:sequence>"
    "<xs:element name='label' type='xs:string' minOccurs='0'/></xs:sequence>"
    "<xs:attribute name='size' type='s:size'/></xs:complexType>"
    "<xs:element name='hat' type='s:hat-type'/>"
    "<xs:attribute name='colour' type='xs:string'/>"
    "</xs:schema>";

static const sxml_resource_entry fixture_resources[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/x/s.xsd"),
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
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = sxml_table_resource_resolver(&fixture_resource_view)},
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

/* One stylesheet, called at xsl:initial-template, with the host's
   schemas and a global context item (none when source is NONE). */
typedef struct transformation_run {
    sxml_byte_view stylesheet_text;
    sxml_schema_view host_schemas;
    sxml_item source;
    /* Out: the error record of the failure. */
    sxml_error_record error_record;
} transformation_run;

/* Compiles and runs run's stylesheet (base URI
   http://example.com/x/main.xsl); the serialized result goes to the
   output buffer (cleared first). */
static sxml_status run_transformation(const fixture *test_fixture, transformation_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xslt_stylesheet *stylesheet = NULL;
    sxml_status status = sxml_xslt_stylesheet_create(
        fixture_allocator(test_fixture),
        &(sxml_xslt_stylesheet_configuration){
            .processor = test_fixture->processor,
            .principal_module_text = run->stylesheet_text,
            .static_context = {.static_base_uri = TEXT("http://example.com/x/main.xsl"),
                               .in_scope_schemas = run->host_schemas},
            .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
            .error_record = &run->error_record},
        &stylesheet);
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context;
        memset(&dynamic_context, 0, sizeof(dynamic_context));
        dynamic_context.has_context_value = run->source.representation != 0;
        dynamic_context.context_value = run->source.representation != 0
                                            ? sxml_sequence_from_item(run->source)
                                            : sxml_sequence_empty();
        sxml_sequence result = sxml_sequence_empty();
        status = sxml_xslt_stylesheet_transform(
            stylesheet,
            &(sxml_xslt_transformation_request){
                .dynamic_context = dynamic_context,
                .base_output_uri = TEXT("http://example.com/out/main.xml"),
                .output_stream = sxml_output_buffer_as_output_stream(test_fixture->output_buffer),
                .error_record = &run->error_record,
                .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED},
            &result);
        sxml_sequence_release(result);
    }
    sxml_xslt_stylesheet_destroy(stylesheet);
    return status;
}

/* --------------------------------------------------------------- Cases */

/* A stylesheet body (inside xsl:stylesheet, after the schema import) and
   its serialized output, or the error code it raises. */
typedef struct transformation_case {
    const char *body;
    const char *expected_output;
    const char *expected_code;
} transformation_case;

#define STYLESHEET_START                                                                       \
    "<xsl:stylesheet version='3.0' xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"           \
    " xmlns:xs='http://www.w3.org/2001/XMLSchema' xmlns:s='urn:s'"                             \
    " exclude-result-prefixes='#all'>"                                                         \
    "<xsl:output method='text'/>"
#define IMPORT "<xsl:import-schema namespace='urn:s' schema-location='s.xsd'/>"
#define MAIN(content) "<xsl:template name='xsl:initial-template'>" content "</xsl:template>"

static const transformation_case transformation_cases[] = {
    /* 3.13: a schema-location, and the types it imports. */
    {IMPORT MAIN("<xsl:value-of select='s:size(4) instance of s:size'/>"), "true", NULL},
    {IMPORT MAIN("<xsl:value-of select=\"type-available('s:size'), type-available('s:shoe')\"/>"),
     "true false", NULL},
    {MAIN("<xsl:value-of select=\"system-property('xsl:is-schema-aware')\"/>"), "yes", NULL},
    /* An inline schema (3.13), its namespace from its targetNamespace. */
    {"<xsl:import-schema><xs:schema targetNamespace='urn:t'>"
     "<xs:simpleType name='yes-no'><xs:restriction base='xs:string'>"
     "<xs:enumeration value='yes'/><xs:enumeration value='no'/></xs:restriction>"
     "</xs:simpleType></xs:schema></xsl:import-schema>"
     "<xsl:variable name='v' as='Q{urn:t}yes-no' select=\"Q{urn:t}yes-no('yes')\"/>" MAIN(
         "<xsl:value-of select='$v'/>"),
     "yes", NULL},
    {"<xsl:import-schema namespace='urn:u'><xs:schema targetNamespace='urn:t'/>"
     "</xsl:import-schema>" MAIN("<x/>"),
     NULL, "XTSE0215"},
    {"<xsl:import-schema schema-location='s.xsd'><xs:schema targetNamespace='urn:t'/>"
     "</xsl:import-schema>" MAIN("<x/>"),
     NULL, "XTSE0215"},
    {"<xsl:import-schema><xs:schema><xs:element name='a' type='no-type'/></xs:schema>"
     "</xsl:import-schema>" MAIN("<x/>"),
     NULL, "XTSE0220"},
    /* 3.13.1: a schema-role names the role of an import, else XTSE4045. */
    {"<xsl:import-schema role='r' namespace='urn:s' schema-location='s.xsd'/>" MAIN(
         "<x xsl:schema-role='r'>a</x>"),
     "a", NULL},
    {IMPORT MAIN("<x xsl:schema-role='r'/>"), NULL, "XTSE4045"},
    /* A location the resolver does not deliver is no error (3.13). */
    {"<xsl:import-schema namespace='urn:v' schema-location='none.xsd'/>" MAIN("<x/>"), "", NULL},
    /* Validation of constructed nodes (25.1). */
    {IMPORT MAIN("<xsl:variable name='h'><xsl:element name='s:hat' validation='strict'>"
                 "<xsl:attribute name='size' select='3'/></xsl:element></xsl:variable>"
                 "<xsl:value-of select='$h/s:hat instance of element(*, s:hat-type), "
                 "data($h/s:hat/@size) instance of s:size'/>"),
     "true true", NULL},
    {IMPORT MAIN("<xsl:element name='s:hat' validation='strict'>"
                 "<xsl:attribute name='size' select='30'/></xsl:element>"),
     NULL, "XTTE1510"},
    {IMPORT MAIN("<xsl:element name='s:shoe' validation='strict'/>"), NULL, "XTTE1512"},
    {IMPORT MAIN("<xsl:variable name='e'><xsl:element name='s:shoe' validation='lax'/>"
                 "</xsl:variable><xsl:value-of select='exists($e/s:shoe)'/>"),
     "true", NULL},
    {IMPORT MAIN("<xsl:variable name='a' as='attribute()'><xsl:attribute name='size' "
                 "type='s:size' select='7'/></xsl:variable>"
                 "<xsl:value-of select='data($a) instance of s:size'/>"),
     "true", NULL},
    {IMPORT MAIN("<xsl:attribute name='size' type='s:size' select='70'/>"), NULL, "XTTE1540"},
    {IMPORT MAIN("<xsl:attribute name='size' type='s:hat-type' select='7'/>"), NULL,
     "XTSE1530"},
    {IMPORT MAIN("<xsl:element name='a' type='s:shoe-type'/>"), NULL, "XTSE1520"},
    {IMPORT MAIN("<xsl:element name='a' type='xs:integer' validation='strict'/>"), NULL,
     "XTSE1505"},
    /* A literal result element's xsl:type (11.1.2). */
    {IMPORT MAIN("<xsl:variable name='h'><s:hat xsl:type='s:hat-type' size='2'/></xsl:variable>"
                 "<xsl:value-of select='$h/* instance of element(*, s:hat-type)'/>"),
     "true", NULL},
    /* Typed modes (6.9.4, packet 4.21): a leading element name is
       schema-element() in a lax or strict mode; strict needs a
       declaration (XTSE3105), lax reads an undeclared name as a name. */
    {IMPORT "<xsl:mode typed='lax'/><xsl:template match='s:hat'>T</xsl:template>"
            "<xsl:template match='*'>O</xsl:template>"
            MAIN("<xsl:variable name='h'><s:hat xsl:validation='strict' size='2'/>"
                 "<s:hat xsl:type='xs:string'/></xsl:variable><xsl:apply-templates select='$h/*'/>"),
     "TO", NULL},
    {IMPORT "<xsl:mode typed='strict'/><xsl:template match='s:nothing'/>" MAIN("<x/>"), NULL,
     "XTSE3105"},
    {IMPORT "<xsl:mode typed='lax'/><xsl:template match='s:nothing'/>" MAIN("x"), "x", NULL},
    /* Documents (25.2). */
    {IMPORT MAIN("<xsl:variable name='d' as='document-node()'><xsl:document validation='strict'>"
                 "<s:hat/></xsl:document></xsl:variable>"
                 "<xsl:value-of select='$d instance of document-node(schema-element(s:hat))'/>"),
     "true", NULL},
    {IMPORT MAIN("<xsl:document validation='strict'><s:hat/><s:hat/></xsl:document>"), NULL,
     "XTTE1550"},
    /* copy-of keeps or strips annotations (25.1.1). */
    {IMPORT MAIN("<xsl:variable name='h'><s:hat xsl:validation='strict' size='2'/></xsl:variable>"
                 "<xsl:variable name='kept'><xsl:copy-of select='$h/*' validation='preserve'/>"
                 "</xsl:variable><xsl:variable name='stripped'><xsl:copy-of select='$h/*'/>"
                 "</xsl:variable><xsl:value-of select='$kept/* instance of element(*, s:hat-type),"
                 " $stripped/* instance of element(*, xs:untyped)'/>"),
     "true true", NULL},
    /* XTTE0950 (11.9.1.1, 11.9.2): namespace-sensitive content copied
       with validation="preserve" without its namespaces or its parent;
       fn:copy-of preserves (20.6.1). */
#define QNAME_SCHEMA                                                                               \
    "<xsl:import-schema><xs:schema targetNamespace='urn:q'><xs:element name='e'>"                  \
    "<xs:complexType><xs:attribute name='q' type='xs:QName'/></xs:complexType>"                    \
    "</xs:element></xs:schema></xsl:import-schema>"                                                \
    "<xsl:variable name='x' as='element()'><e xmlns='urn:q' xmlns:p='urn:p' q='p:v' "              \
    "xsl:validation='strict'/></xsl:variable>"
    {QNAME_SCHEMA MAIN("<xsl:copy-of select='$x' validation='preserve'/>"), "", NULL},
    {QNAME_SCHEMA MAIN("<xsl:copy-of select='$x' copy-namespaces='no' validation='preserve'/>"),
     NULL, "XTTE0950"},
    {QNAME_SCHEMA MAIN("<a><xsl:copy-of select='$x/@q' validation='preserve'/></a>"), NULL,
     "XTTE0950"},
    {QNAME_SCHEMA MAIN("<a><xsl:copy-of select='$x/@q'/></a>"), "", NULL},
    {QNAME_SCHEMA MAIN("<a><xsl:sequence select='copy-of($x/@q)'/></a>"), NULL, "XTTE0950"},
#undef QNAME_SCHEMA
    /* default-validation="preserve" (3.6.1). */
    {IMPORT "<xsl:variable name='h'><s:hat xsl:validation='strict' size='2'/></xsl:variable>"
            "<xsl:template name='xsl:initial-template' default-validation='preserve'>"
            "<xsl:variable name='kept'><xsl:copy-of select='$h/*'/></xsl:variable>"
            "<xsl:value-of select='$kept/* instance of element(*, s:hat-type)'/></xsl:template>",
     "true", NULL},
};

static void test_transformations_are_schema_aware(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(transformation_cases);
         case_index += 1) {
        const transformation_case *transformation = &transformation_cases[case_index];
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(&test_fixture) == SXML_STATUS_OK);
        char text[4096];
        const int written = snprintf(text, sizeof text, "%s%s</xsl:stylesheet>", STYLESHEET_START,
                                     transformation->body);
        SXML_TEST_REQUIRE(state, written > 0 && (size_t)written < sizeof text);
        transformation_run run = {
            .stylesheet_text = {.bytes = (const uint8_t *)text, .byte_count = (size_t)written},
            .host_schemas = {.schemas = NULL, .schema_count = 0},
            .source = {.representation = 0}};
        const sxml_status status = run_transformation(&test_fixture, &run);
        bool matches = false;
        if (transformation->expected_code == NULL) {
            const sxml_byte_view expected = {
                .bytes = (const uint8_t *)transformation->expected_output,
                .byte_count = strlen(transformation->expected_output)};
            matches = status == SXML_STATUS_OK &&
                      sxml_byte_view_equals(
                          sxml_output_buffer_get_bytes(test_fixture.output_buffer), expected);
        } else {
            matches = status != SXML_STATUS_OK &&
                      sxml_byte_view_equals(
                          run.error_record.code.local_name,
                          (sxml_byte_view){.bytes = (const uint8_t *)transformation->expected_code,
                                           .byte_count = strlen(transformation->expected_code)});
        }
        if (!matches) {
            const sxml_byte_view output = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
            (void)fprintf(stderr, "case %zu: status %d, code %.*s: %.*s, output %.*s\n",
                          case_index, (int)status,
                          (int)run.error_record.code.local_name.byte_count,
                          (const char *)run.error_record.code.local_name.bytes,
                          (int)run.error_record.diagnostic.description.byte_count,
                          (const char *)run.error_record.diagnostic.description.bytes,
                          (int)output.byte_count, (const char *)output.bytes);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
}

/* ------------------------------------------------ The host's schemas */

/* A source the host validated against its schema: the stylesheet sees
   the host's schema (xslt.h, D179), so its types are the annotations';
   input-type-annotations="strip" removes them (4.1). */
static void test_host_schemas_type_the_source(sxml_test_state *state) {
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
                                             .document_uri = TEXT("http://example.com/x/s.xsd")},
                                     .schema_document_count = 1},
                .error_record = &error_record},
            &schema) == SXML_STATUS_OK);
    sxml_item parsed = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_processor_parse_document(
                          test_fixture.processor,
                          &(sxml_xml_parse_request){
                              .input = TEXT("<hat xmlns='urn:s' size='3'/>"),
                              .source_name = TEXT("source.xml"),
                              .document_uri = TEXT("http://example.com/x/source.xml"),
                              .base_uri = TEXT("http://example.com/x/source.xml")},
                          &parsed) == SXML_STATUS_OK);
    sxml_item validated = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_schema_validate_node(
                                 schema,
                                 &(sxml_schema_validation_request){
                                     .node = parsed,
                                     .validation_mode = SXML_VALIDATION_MODE_STRICT,
                                     .governing_type = {.index = SXML_TYPE_NONE},
                                     .error_record = &error_record},
                                 &validated) == SXML_STATUS_OK);
    sxml_item_release(parsed);
    const sxml_schema *const schemas[1] = {schema};
    static const struct {
        const char *stylesheet;
        const char *expected_output;
    } cases[] = {
        {STYLESHEET_START IMPORT MAIN("<xsl:value-of select='s:hat instance of element(*, "
                                      "s:hat-type), data(s:hat/@size) instance of s:size'/>")
             "</xsl:stylesheet>",
         "true true"},
        {"<xsl:stylesheet version='3.0' xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"
         " xmlns:s='urn:s' input-type-annotations='strip'><xsl:output method='text'/>"
         "<xsl:template name='xsl:initial-template'><xsl:value-of select='s:hat instance of element(*, "
         "Q{http://www.w3.org/2001/XMLSchema}untyped)'/></xsl:template></xsl:stylesheet>",
         "true"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        transformation_run run = {
            .stylesheet_text = {.bytes = (const uint8_t *)cases[case_index].stylesheet,
                                .byte_count = strlen(cases[case_index].stylesheet)},
            .host_schemas = {.schemas = schemas, .schema_count = 1},
            .source = validated};
        const sxml_status status = run_transformation(&test_fixture, &run);
        const sxml_byte_view expected = {
            .bytes = (const uint8_t *)cases[case_index].expected_output,
            .byte_count = strlen(cases[case_index].expected_output)};
        const bool matches =
            status == SXML_STATUS_OK &&
            sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture.output_buffer),
                                  expected);
        if (!matches) {
            (void)fprintf(stderr, "host case %zu: status %d, code %.*s\n", case_index,
                          (int)status, (int)run.error_record.code.local_name.byte_count,
                          (const char *)run.error_record.code.local_name.bytes);
        }
        SXML_TEST_EXPECT(state, matches);
    }
    sxml_item_release(validated);
    sxml_schema_destroy(schema);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static const sxml_test_case xslt_schema_test_cases[] = {
    {.name = "transformations_are_schema_aware", .function = test_transformations_are_schema_aware},
    {.name = "host_schemas_type_the_source", .function = test_host_schemas_type_the_source},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_schema",
            .cases = xslt_schema_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_schema_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
