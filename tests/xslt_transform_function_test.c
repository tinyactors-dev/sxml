/*
 * Tests for fn:transform (F&O 4.0, 18.3; docs/DECISIONS.md, D203; packet
 * 7.11) through the public API, from XQuery with the optional feature
 * sxml_xslt_transform_feature(): a stylesheet given as text, as a node
 * and by location, the invocations (apply-templates, call-template,
 * call-function), the parameters, the delivery formats, result documents
 * in the result map, post-process, a function item that outlives the
 * transformation, a transformation inside a transformation, the FOXT
 * errors, the stylesheet's own errors, the function's absence without the
 * feature, and an allocation-failure loop (D23). Every case checks for
 * leaked cells and allocations.
 *
 * Golden file: tests/xquery_load_module_test.c.
 */
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "sxml/xquery.h"
#include "sxml/xslt.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ------------------------------------------------------------- Resolver */

/* The stylesheet the resolver serves at http://example.com/m/served.xsl,
   as a document (fn:transform reads a location as fn:doc does) and as a
   module. */
static const sxml_byte_view served_stylesheet = SXML_BYTE_VIEW_INITIALIZER(
    "<xsl:stylesheet xmlns:xsl='http://www.w3.org/1999/XSL/Transform' version='3.0'>"
    "<xsl:template match='/'><served><xsl:value-of select='.'/></served></xsl:template>"
    "<xsl:template name='main'><base><xsl:value-of select='static-base-uri()'/></base>"
    "</xsl:template></xsl:stylesheet>");

static sxml_status resolve_document(void *state, const sxml_resource_request *request,
                                    sxml_resource *resource) {
    (void)state;
    if ((request->resource_kind == SXML_RESOURCE_KIND_XML_DOCUMENT ||
         request->resource_kind == SXML_RESOURCE_KIND_XSLT_MODULE) &&
        sxml_byte_view_equals(request->absolute_uri, TEXT("http://example.com/m/served.xsl"))) {
        resource->resource_form = SXML_RESOURCE_FORM_BYTES;
        resource->content = served_stylesheet;
        resource->resource_uri = request->absolute_uri;
    }
    return SXML_STATUS_OK;
}

static const sxml_resource_resolver_operations document_resolver_operations = {
    .resolve = resolve_document, .release = NULL};

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    bool has_feature;
    sxml_optional_feature feature;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

typedef struct fixture_request {
    uint64_t failing_allocation_number;
    bool has_feature;
} fixture_request;

static sxml_status fixture_create(fixture_request request, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL,
                              .processor = NULL,
                              .output_buffer = NULL,
                              .has_feature = request.has_feature,
                              .feature = sxml_xslt_transform_feature()};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     request.failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = {.state = NULL, .operations = &document_resolver_operations},
            .optional_features = {.optional_features = &test_fixture->feature,
                                  .optional_feature_count = request.has_feature ? 1U : 0U}},
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

/* One query to run and what it gives. */
typedef struct query_run {
    sxml_byte_view expression_text;
    sxml_dynamic_context_configuration dynamic_context;
    /* Out: the error record of the failure. */
    sxml_error_record error_record;
} query_run;

/* Compiles run->expression_text with the static base URI
   http://example.com/m/, evaluates it and serializes the result (method
   xml) into the fixture's output buffer (cleared first). */
static sxml_status run_query(fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xquery_query *query = NULL;
    sxml_status status = sxml_xquery_query_create(
        fixture_allocator(test_fixture),
        &(sxml_xquery_query_configuration){
            .processor = test_fixture->processor,
            .expression_text = run->expression_text,
            .source_name = TEXT("main.xq"),
            .static_context = {.static_base_uri = TEXT("http://example.com/m/")},
            .error_record = &run->error_record},
        &query);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xquery_query_evaluate(
        query,
        &(sxml_xquery_evaluation_request){.dynamic_context = run->dynamic_context,
                                          .error_record = &run->error_record},
        &result);
    if (status == SXML_STATUS_OK) {
        sxml_serialization_parameters parameters;
        memset(&parameters, 0, sizeof(parameters));
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(test_fixture->processor),
            &(sxml_serialization_request){.value = result,
                                          .serialization_parameters = parameters,
                                          .error_record = &run->error_record},
            sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
        sxml_sequence_release(result);
    }
    sxml_xquery_query_destroy(query);
    return status;
}

static bool output_is(const fixture *test_fixture, sxml_byte_view expected) {
    return sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                 expected);
}

static bool error_code_is(const query_run *run, sxml_byte_view local_name) {
    return sxml_byte_view_equals(run->error_record.code.local_name, local_name);
}

/* ------------------------------------------------------------ Query cases */

/* A main module and its output, or the error code it raises. */
typedef struct query_case {
    const char *name;
    sxml_byte_view expression_text;
    sxml_byte_view expected_output;
    sxml_byte_view expected_error_code;
} query_case;

#define QUERY_VALUE(name_text, text, output)                                              \
    {.name = (name_text),                                                                \
     .expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                \
     .expected_output = SXML_BYTE_VIEW_INITIALIZER(output),                              \
     .expected_error_code = {.bytes = NULL, .byte_count = 0}}

#define QUERY_ERROR(name_text, text, code)                                                \
    {.name = (name_text),                                                                \
     .expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                \
     .expected_output = {.bytes = NULL, .byte_count = 0},                                \
     .expected_error_code = SXML_BYTE_VIEW_INITIALIZER(code)}

#define STYLESHEET_START                                                                  \
    "<xsl:stylesheet xmlns:xsl='http://www.w3.org/1999/XSL/Transform' "                   \
    "xmlns:xs='http://www.w3.org/2001/XMLSchema' xmlns:f='urn:f' "                    \
    "exclude-result-prefixes='#all' version='3.0'>"
#define STYLESHEET_END "</xsl:stylesheet>"

#define COPY_STYLESHEET                                                                   \
    "\"" STYLESHEET_START "<xsl:template match='/'><out><xsl:value-of select='.'/></out>"   \
    "</xsl:template>" STYLESHEET_END "\""

static const query_case query_cases[] = {
    /* 18.3: stylesheet-text and source-node; the principal result is a
       document under "output". */
    QUERY_VALUE("text-and-source",
                "transform({'stylesheet-text': " COPY_STYLESHEET
                ", 'source-node': parse-xml('<doc>this</doc>')})?output",
                "<out>this</out>"),
    QUERY_VALUE("result-map-keys",
                "map:keys(transform({'stylesheet-text': " COPY_STYLESHEET
                ", 'source-node': parse-xml('<doc/>')}))",
                "output"),
    QUERY_VALUE("source-element",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:template match='doc'><e><xsl:value-of select='name(.)'/></e></xsl:template>"
                STYLESHEET_END "\", 'source-node': parse-xml('<r><doc/></r>')/r/doc})?output",
                "<e>doc</e>"),
    QUERY_VALUE("stylesheet-node",
                "transform({'stylesheet-node': parse-xml(" COPY_STYLESHEET
                "), 'source-node': parse-xml('<doc>node</doc>')})?output",
                "<out>node</out>"),
    QUERY_VALUE("stylesheet-location",
                "transform({'stylesheet-location': 'served.xsl', "
                "'source-node': parse-xml('<doc>far</doc>')})?output",
                "<served>far</served>"),
    QUERY_VALUE("location-base-uri",
                "transform({'stylesheet-location': 'served.xsl', "
                "'initial-template': QName('', 'main')})?output",
                "<base>http://example.com/m/served.xsl</base>"),
    /* Delivery formats. */
    QUERY_VALUE("serialized",
                "transform({'stylesheet-text': " COPY_STYLESHEET
                ", 'source-node': parse-xml('<doc>s</doc>'), 'delivery-format': 'serialized', "
                "'serialization-params': {'omit-xml-declaration': true()}})?output",
                "&lt;out&gt;s&lt;/out&gt;"),
    QUERY_VALUE("raw",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:template match='/' as='xs:integer'><xsl:sequence select='count(//*)'/>"
                "</xsl:template>" STYLESHEET_END
                "\", 'source-node': parse-xml('<a><b/><c/></a>'), 'delivery-format': 'raw'})"
                "?output instance of xs:integer",
                "true"),
    /* Invocations and parameters (2.4). */
    QUERY_VALUE("initial-template-and-parameters",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:param name='p'/><xsl:template name='main'><xsl:param name='t'/>"
                "<xsl:param name='u' tunnel='yes'/>"
                "<r><xsl:value-of select='$p, $t, $u'/></r></xsl:template>" STYLESHEET_END
                "\", 'initial-template': QName('', 'main'), "
                "'stylesheet-params': {QName('', 'p'): 1}, "
                "'template-params': {QName('', 't'): 2}, "
                "'tunnel-params': {QName('', 'u'): 3}})?output",
                "<r>1 2 3</r>"),
    QUERY_VALUE("initial-function",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:function name='f:range' visibility='public' as='xs:integer*'>"
                "<xsl:param name='from'/><xsl:param name='to'/>"
                "<xsl:sequence select='$from to $to'/></xsl:function>" STYLESHEET_END
                "\", 'initial-function': QName('urn:f', 'range'), 'function-params': [2, 4], "
                "'delivery-format': 'raw'})?output",
                "2 3 4"),
    QUERY_VALUE("initial-match-selection",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:template match='.' as='xs:integer'><xsl:sequence select='. * .'/>"
                "</xsl:template>" STYLESHEET_END
                "\", 'initial-match-selection': 1 to 3, 'delivery-format': 'raw'})?output",
                "1 4 9"),
    QUERY_VALUE("static-params",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:param name='s' static='yes' select='0'/>"
                "<xsl:template name='xsl:initial-template'><s><xsl:value-of select='$s'/></s>"
                "</xsl:template>" STYLESHEET_END "\", 'static-params': {QName('', 's'): 7}})?output",
                "<s>7</s>"),
    QUERY_VALUE("unnamed-mode",
                "transform({'stylesheet-text': \"<xsl:stylesheet "
                "xmlns:xsl='http://www.w3.org/1999/XSL/Transform' version='3.0' default-mode='m'>"
                "<xsl:template match='/' mode='m'><m/></xsl:template>"
                "<xsl:template match='/' mode='#unnamed'><unnamed/></xsl:template>" STYLESHEET_END
                "\", 'source-node': parse-xml('<doc/>'), "
                "'initial-mode': QName('http://www.w3.org/1999/XSL/Transform', 'unnamed')})?output",
                "<unnamed/>"),
    QUERY_VALUE("joined-cdata-section-elements",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:output cdata-section-elements='a'/>"
                "<xsl:template name='xsl:initial-template'><r><a>1</a><b>2</b></r>"
                "</xsl:template>" STYLESHEET_END "\", 'delivery-format': 'serialized', "
                "'serialization-params': {'cdata-section-elements': QName('', 'b'), "
                "'omit-xml-declaration': true()}})?output",
                "&lt;r&gt;&lt;a&gt;&lt;![CDATA[1]]&gt;&lt;/a&gt;&lt;b&gt;&lt;![CDATA[2]]&gt;&lt;/b&gt;"
                "&lt;/r&gt;"),
    /* Result documents (26.2) and post-process. */
    QUERY_VALUE("principal-result-document",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:template name='xsl:initial-template'>"
                "<xsl:result-document><p/></xsl:result-document>"
                "<xsl:result-document href='s.xml'><s/></xsl:result-document>"
                "</xsl:template>" STYLESHEET_END "\"})?output",
                "<p/>"),
    QUERY_VALUE("result-documents",
                "let $r := transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:template name='xsl:initial-template'>"
                "<xsl:for-each select='1 to 2'><xsl:result-document href='d{.}.xml'>"
                "<d><xsl:value-of select='.'/></d></xsl:result-document></xsl:for-each>"
                "<main/></xsl:template>" STYLESHEET_END
                "\", 'base-output-uri': 'http://example.com/out/main.xml'}) "
                "return (sort(map:keys($r)), $r('http://example.com/out/d2.xml'))",
                "http://example.com/out/d1.xml http://example.com/out/d2.xml "
                "http://example.com/out/main.xml<d>2</d>"),
    QUERY_VALUE("post-process",
                "transform({'stylesheet-text': " COPY_STYLESHEET
                ", 'source-node': parse-xml('<doc>5</doc>'), "
                "'post-process': fn($key, $value) { $key || '=' || $value }})?output",
                "output=5"),
    /* A function item made by the transformation, called after it. */
    QUERY_VALUE("function-outlives",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:variable name='g' select='7'/>"
                "<xsl:template name='xsl:initial-template'>"
                "<xsl:sequence select='function($x) { $x + $g }'/></xsl:template>" STYLESHEET_END
                "\", 'delivery-format': 'raw'})?output(1)",
                "8"),
    /* A transformation inside a transformation: the inner one's result
       documents and result writer are its own. */
    QUERY_VALUE("nested",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:param name='inner'/>"
                "<xsl:template name='xsl:initial-template'>"
                "<outer><xsl:copy-of select=\"\"transform(map{'stylesheet-text': $inner})?output\"\"/>"
                "</outer></xsl:template>" STYLESHEET_END
                "\", 'stylesheet-params': {QName('', 'inner'): \"" STYLESHEET_START
                "<xsl:template name='xsl:initial-template'><inner/></xsl:template>" STYLESHEET_END
                "\"}})?output",
                "<outer><inner/></outer>"),
    /* Errors (18.3). */
    QUERY_ERROR("no-stylesheet", "transform({})", "FOXT0002"),
    QUERY_ERROR("two-stylesheets",
                "transform({'stylesheet-text': " COPY_STYLESHEET
                ", 'stylesheet-location': 'served.xsl'})",
                "FOXT0002"),
    QUERY_ERROR("unknown-delivery",
                "transform({'stylesheet-text': " COPY_STYLESHEET ", 'delivery-format': 'doc'})",
                "FOXT0002"),
    QUERY_ERROR("missing-location", "transform({'stylesheet-location': 'none.xsl'})",
                "FOXT0002"),
    QUERY_ERROR("later-version",
                "transform({'stylesheet-text': " COPY_STYLESHEET ", 'xslt-version': 5.0})",
                "FOXT0001"),
    QUERY_ERROR("requested-vendor",
                "transform({'stylesheet-text': " COPY_STYLESHEET ", 'requested-properties': "
                "{QName('http://www.w3.org/1999/XSL/Transform', 'vendor'): 'Other'}})",
                "FOXT0001"),
    QUERY_ERROR("unknown-option",
                "transform({'stylesheet-text': " COPY_STYLESHEET ", 'debug': true()})",
                "XPTY0004"),
    QUERY_ERROR("not-well-formed", "transform({'stylesheet-text': '<xsl:stylesheet'})",
                "FOXT0003"),
    QUERY_ERROR("static-error-keeps-its-code",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:template/>" STYLESHEET_END "\", 'source-node': parse-xml('<a/>')})",
                "XTSE0500"),
    QUERY_ERROR("no-initial-template",
                "transform({'stylesheet-text': " COPY_STYLESHEET "})", "XTDE0040"),
    QUERY_ERROR("dynamic-error-keeps-its-code",
                "transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:template name='xsl:initial-template'><xsl:sequence select='1 div 0'/>"
                "</xsl:template>" STYLESHEET_END "\"})",
                "FOAR0001"),
    QUERY_VALUE("caught",
                "try { transform({'stylesheet-text': \"" STYLESHEET_START
                "<xsl:template name='xsl:initial-template'>"
                "<xsl:sequence select=\"\"error(QName('urn:e', 'E'))\"\"/>"
                "</xsl:template>" STYLESHEET_END
                "\"}) } catch * { local-name-from-QName($err:code) }",
                "E"),
};

static void test_transformations_run(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < sizeof(query_cases) / sizeof(query_cases[0]);
         case_index += 1) {
        const query_case *current = &query_cases[case_index];
        fixture test_fixture;
        sxml_status status = fixture_create(
            (fixture_request){.failing_allocation_number = 0, .has_feature = true}, &test_fixture);
        query_run run = {.expression_text = current->expression_text};
        if (status == SXML_STATUS_OK) {
            status = run_query(&test_fixture, &run);
        }
        const bool passed = current->expected_error_code.byte_count > 0
                                ? status != SXML_STATUS_OK &&
                                      error_code_is(&run, current->expected_error_code)
                                : status == SXML_STATUS_OK &&
                                      output_is(&test_fixture, current->expected_output);
        if (!passed) {
            sxml_byte_view output = sxml_output_buffer_get_bytes(test_fixture.output_buffer);
            (void)fprintf(stderr, "%s: status %d, code %.*s, output %.*s\n", current->name,
                          (int)status, (int)run.error_record.code.local_name.byte_count,
                          (const char *)run.error_record.code.local_name.bytes,
                          (int)output.byte_count, (const char *)output.bytes);
        }
        (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                              .expression = current->name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
        (void)sxml_test_record_check(
            state, (sxml_test_check){.passed = fixture_destroy(&test_fixture),
                                     .expression = current->name,
                                     .file_name = __FILE__,
                                     .line_number = __LINE__});
    }
}

/* Without the feature the function is absent (D191). */
static void test_function_is_absent_without_the_feature(sxml_test_state *state) {
    fixture test_fixture;
    sxml_status status = fixture_create(
        (fixture_request){.failing_allocation_number = 0, .has_feature = false}, &test_fixture);
    query_run run = {.expression_text = TEXT("transform({})")};
    if (status == SXML_STATUS_OK) {
        status = run_query(&test_fixture, &run);
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XPST0017")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(
        (fixture_request){.failing_allocation_number = failing_number, .has_feature = true},
        &test_fixture);
    if (status == SXML_STATUS_OK) {
        query_run run = {.expression_text = TEXT(
                             "let $r := transform({'stylesheet-text': \"" STYLESHEET_START
                             "<xsl:template match='/'><xsl:result-document href='x.xml'><x/>"
                             "</xsl:result-document><out><xsl:value-of select='.'/></out>"
                             "</xsl:template>" STYLESHEET_END
                             "\", 'source-node': parse-xml('<doc>9</doc>'), "
                             "'base-output-uri': 'http://example.com/o.xml', "
                             "'delivery-format': 'serialized'}) "
                             "return count(map:keys($r))")};
        status = run_query(&test_fixture, &run);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("2")));
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_transform_survives_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 200000; failing_number += 1) {
        status = run_with_failure(state, failing_number);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case xslt_transform_function_test_cases[] = {
    {.name = "transformations_run", .function = test_transformations_run},
    {.name = "function_is_absent_without_the_feature",
     .function = test_function_is_absent_without_the_feature},
    {.name = "transform_survives_every_allocation_failure",
     .function = test_transform_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_transform_function",
            .cases = xslt_transform_function_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_transform_function_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
