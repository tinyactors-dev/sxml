/*
 * Tests for XSLT backwards compatible behaviour and XPath 1.0
 * compatibility mode (packet 7.4, D197; XSLT 4.0, 3.8.1; XPath 4.0,
 * sections 2.2.1, 3.4.1, 4.9 and 4.11.2) through the public API alone:
 * XPath expressions compiled with has_xpath_1_compatibility_mode, and
 * stylesheets whose elements have an effective version of 1.0 next to
 * elements of 2.0 and later; the 1.0 rules of xsl:value-of, attribute
 * value templates, xsl:number, xsl:sort and keys; XTDE1425 for extension
 * functions; and an allocation-failure loop (D23). Every case checks for
 * leaked cells and allocations.
 *
 * Golden file: tests/xslt_test.c.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "sxml/xslt.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

#define XSL_NAMESPACE "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"

/* A stylesheet of version 1.0 whose template "main" makes out. */
#define VERSION_1(declarations, body)                                                     \
    "<xsl:stylesheet version='1.0' " XSL_NAMESPACE ">"                                  \
    "<xsl:output method='xml' omit-xml-declaration='yes'/>" declarations                \
    "<xsl:template name='main'><out>" body "</out></xsl:template></xsl:stylesheet>"

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    status = sxml_processor_create(
        allocator, &(sxml_processor_configuration){.allocator = allocator},
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

static bool output_is(const fixture *test_fixture, sxml_byte_view expected) {
    return sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                 expected);
}

/* ------------------------------------------------------------------ XPath */

/* An expression evaluated without a context value, its value serialized
   by the adaptive method into the fixture's buffer (cleared first). */
typedef struct expression_run {
    sxml_byte_view expression_text;
    bool has_xpath_1_compatibility_mode;
    sxml_error_record error_record;
} expression_run;

static sxml_status run_expression(fixture *test_fixture, expression_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){
            .processor = test_fixture->processor,
            .expression_text = run->expression_text,
            .source_name = TEXT("compatibility"),
            .static_context = {.has_xpath_1_compatibility_mode =
                                   run->has_xpath_1_compatibility_mode},
            .error_record = &run->error_record},
        &expression);
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        sxml_xpath_evaluation_request request;
        memset(&request, 0, sizeof(request));
        request.error_record = &run->error_record;
        status = sxml_xpath_expression_evaluate(expression, &request, &value);
    }
    if (status == SXML_STATUS_OK) {
        sxml_serialization_parameters parameters;
        memset(&parameters, 0, sizeof(parameters));
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_ADAPTIVE;
        parameters.has_item_separator = true;
        parameters.item_separator = TEXT(" ");
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(test_fixture->processor),
            &(sxml_serialization_request){.value = value,
                                          .serialization_parameters = parameters,
                                          .error_record = &run->error_record},
            sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    }
    sxml_sequence_release(value);
    sxml_xpath_expression_destroy(expression);
    return status;
}

/* An expression and what it gives with the mode false and true: its
   adaptive output, or "error:CODE". */
typedef struct mode_case {
    const char *expression_text;
    const char *without_mode;
    const char *with_mode;
} mode_case;

static const mode_case mode_cases[] = {
    /* 3.4.1: the first item, then fn:string or fn:number. */
    {"substring-before(123456789, 5)", "error:XPTY0004", "\"1234\""},
    {"string-length((\"ab\", \"cde\"))", "error:XPTY0004", "2"},
    {"concat((\"a\", \"b\"), (\"c\", \"d\"))", "\"abcd\"", "\"ac\""},
    {"math:sqrt(\"16\")", "error:XPTY0004", "4.0e0"},
    {"string-length(())", "0", "0"},
    /* 3.4.1 applies to xs:double only, not xs:numeric (F&O 4.0, 2.1.13). */
    {"abs(\"2\")", "error:XPTY0004", "error:XPTY0004"},
    /* 4.9: the first item, xs:double by fn:number, NaN for (). */
    {"\"2\" + 3", "error:XPTY0004", "5.0e0"},
    {"(1, 2) * 2", "error:XPTY0004", "2.0e0"},
    {"() + 1", "", "NaN"},
    {"true() + 1", "error:XPTY0004", "2.0e0"},
    {"-\"3\"", "error:XPTY0004", "-3.0e0"},
    {"1 + 1", "2", "2.0e0"},
    /* 4.11.2: the xs:boolean rule (the effective boolean value of (1, 2)
       is FORG0006), numbers, strings. */
    {"'' = true()", "error:XPTY0004", "false()"},
    {"(1, 2) = true()", "error:XPTY0004", "error:FORG0006"},
    {"'abc' = 1", "error:XPTY0004", "false()"},
    {"'1.0' = 1", "error:XPTY0004", "true()"},
    {"'b' < 'c'", "true()", "false()"},
    {"('x', 2) = ('y', 2.0)", "error:XPTY0004", "true()"},
    {"'x' != 'x'", "false()", "false()"},
    /* 4.8.2: the operands of "to" are coerced, so they keep their first
       items. */
    {"count((1, 2) to (3, 4))", "error:XPTY0004", "3"},
};

static void test_xpath_1_compatibility_mode(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < sizeof(mode_cases) / sizeof(mode_cases[0]);
         case_index += 1) {
        const mode_case *current = &mode_cases[case_index];
        for (size_t mode_index = 0; mode_index < 2; mode_index += 1) {
            expression_run run = {
                .expression_text = sxml_byte_view_from_c_string(current->expression_text),
                .has_xpath_1_compatibility_mode = mode_index == 1};
            const sxml_status status = run_expression(&test_fixture, &run);
            const char *expected = mode_index == 1 ? current->with_mode : current->without_mode;
            const bool expects_error = strncmp(expected, "error:", 6) == 0;
            const bool passed =
                expects_error
                    ? status != SXML_STATUS_OK &&
                          sxml_byte_view_equals(run.error_record.code.local_name,
                                                sxml_byte_view_from_c_string(expected + 6))
                    : status == SXML_STATUS_OK &&
                          output_is(&test_fixture, sxml_byte_view_from_c_string(expected));
            (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                                  .expression =
                                                                      current->expression_text,
                                                                  .file_name = __FILE__,
                                                                  .line_number = __LINE__});
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------------ XSLT */

/* Compiles stylesheet_text, calls its template "main" with source as the
   global context item (none when empty) and serializes the principal
   result into the fixture's buffer (cleared first). */
typedef struct transformation_run {
    sxml_byte_view stylesheet_text;
    sxml_byte_view source_text;
    sxml_error_record error_record;
} transformation_run;

static sxml_status run_transformation(fixture *test_fixture, transformation_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xslt_stylesheet *stylesheet = NULL;
    sxml_status status = sxml_xslt_stylesheet_create(
        fixture_allocator(test_fixture),
        &(sxml_xslt_stylesheet_configuration){
            .processor = test_fixture->processor,
            .principal_module_text = run->stylesheet_text,
            .static_context = {.static_base_uri = TEXT("http://example.com/x/main.xsl")},
            .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
            .error_record = &run->error_record},
        &stylesheet);
    sxml_item source = {.representation = 0};
    const bool has_source = run->source_text.byte_count > 0;
    if (status == SXML_STATUS_OK && has_source) {
        status = sxml_processor_parse_document(
            test_fixture->processor,
            &(sxml_xml_parse_request){.input = run->source_text,
                                      .source_name = TEXT("source.xml"),
                                      .document_uri = TEXT("http://example.com/x/source.xml"),
                                      .base_uri = TEXT("http://example.com/x/source.xml"),
                                      .encoding_name = {.bytes = NULL, .byte_count = 0},
                                      .diagnostic = NULL},
            &source);
    }
    sxml_name_identifier main_name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            sxml_processor_get_name_pool(test_fixture->processor),
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = TEXT("main")},
            &main_name);
    }
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context;
        memset(&dynamic_context, 0, sizeof(dynamic_context));
        dynamic_context.has_context_value = has_source;
        dynamic_context.context_value =
            has_source ? sxml_sequence_from_item(source) : sxml_sequence_empty();
        sxml_sequence principal_result = sxml_sequence_empty();
        status = sxml_xslt_stylesheet_transform(
            stylesheet,
            &(sxml_xslt_transformation_request){
                .dynamic_context = dynamic_context,
                .base_output_uri = TEXT("http://example.com/out/main.xml"),
                .output_definition = NULL,
                .output_stream = sxml_output_buffer_as_output_stream(test_fixture->output_buffer),
                .error_record = &run->error_record,
                .initial_template_name = main_name,
                .invocation_kind = SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED},
            &principal_result);
        sxml_sequence_release(principal_result);
    }
    if (has_source && source.representation != 0) {
        sxml_item_release(source);
    }
    sxml_xslt_stylesheet_destroy(stylesheet);
    return status;
}

/* A stylesheet, its source, and its output or "error:CODE". */
typedef struct behaviour_case {
    const char *test_case_name;
    const char *stylesheet_text;
    const char *source_text;
    const char *expected;
} behaviour_case;

#define SOURCE "<doc><v>3</v><v>1</v><v>2</v><w>b a</w></doc>"

static const behaviour_case behaviour_cases[] = {
    /* 11.4.2: xsl:value-of keeps the first item, not with a separator. */
    {"value-of", VERSION_1("", "<xsl:value-of select='//v'/>"), SOURCE, "<out>3</out>"},
    {"value-of-separator", VERSION_1("", "<xsl:value-of select='//v' separator=','/>"), SOURCE,
     "<out>3,1,2</out>"},
    {"value-of-2.0", VERSION_1("", "<xsl:value-of select='//v' version='2.0'/>"), SOURCE,
     "<out>3 1 2</out>"},
    {"value-of-empty", VERSION_1("", "<xsl:value-of select='//none'/>"), SOURCE, "<out/>"},
    /* 5.6.1: an attribute value template's enclosed expressions. */
    {"attribute-value-template", VERSION_1("", "<a b='{//v}-{//none}'/>"), SOURCE,
     "<out><a b=\"3-\"/></out>"},
    /* Expressions of the element: comparisons and arithmetic. */
    {"comparison", VERSION_1("", "<xsl:value-of select=\"//v = '2' and //w = 1\"/>"), SOURCE,
     "<out>false</out>"},
    {"arithmetic", VERSION_1("", "<xsl:value-of select='//v + 1'/>"), SOURCE, "<out>4</out>"},
    {"function-arguments", VERSION_1("", "<xsl:value-of select='string-length(//v)'/>"), SOURCE,
     "<out>1</out>"},
    /* 12.2: the first item, NaN for a value without an integer. */
    {"number", VERSION_1("", "<xsl:number value='//v'/>"), SOURCE, "<out>3</out>"},
    {"number-nan", VERSION_1("", "<xsl:number value=\"'apples'\" format='(1)'/>"), SOURCE,
     "<out>(NaN)</out>"},
    {"number-empty", VERSION_1("", "<xsl:number value='//none'/>"), SOURCE, "<out>NaN</out>"},
    /* 13.1.2: a sort key keeps its first item. */
    {"sort", VERSION_1("", "<xsl:for-each select='//v'><xsl:sort select='(., 0)'/>"
                           "<xsl:value-of select='.'/></xsl:for-each>"),
     SOURCE, "<out>123</out>"},
    /* 20.2.2: a key in backwards compatible mode compares strings: the
       number 2.0 is the string "2". */
    {"key", VERSION_1("<xsl:key name='k' match='v' use='.'/>",
                      "<xsl:value-of select=\"count(key('k', 2.0))\"/>"
                      "<xsl:value-of select=\"count(key('k', '2'))\"/>"),
     SOURCE, "<out>11</out>"},
    /* 3.8.1, 24.1.2: an unknown extension function raises XTDE1425 when
       called, and function-available says false. */
    {"extension-function-not-called",
     VERSION_1("", "<xsl:if test=\"function-available('e:f')\" xmlns:e='urn:e'>"
                   "<xsl:value-of select='e:f()'/></xsl:if>"),
     SOURCE, "<out/>"},
    {"extension-function-called", VERSION_1("", "<xsl:value-of select='e:f()' xmlns:e='urn:e'/>"),
     SOURCE, "error:XTDE1425"},
    {"supports-backwards-compatibility",
     VERSION_1("", "<xsl:value-of select=\"system-property('xsl:supports-backwards-"
                   "compatibility')\"/>"),
     SOURCE, "<out>yes</out>"},
    /* 3.8.1: a text value template's expressions are evaluated without
       XPath 1.0 compatibility mode (D206): the product is an xs:integer
       there, an xs:double for xsl:value-of. */
    {"text-value-template",
     VERSION_1("", "<t xsl:expand-text='yes'>{1000000 * 1000000}</t>"
                   "<xsl:value-of select='1000000 * 1000000'/>"),
     SOURCE, "<out><t>1000000000000</t>1.0E12</out>"},
    /* EXSLT Common (D206): exsl:node-set returns a temporary tree as it
       is, function-available knows it, exsl:object-type names the XPath
       1.0 types. */
    {"exsl-node-set",
     VERSION_1("<xsl:variable name='t'><i>a</i><i>b</i></xsl:variable>",
               "<xsl:value-of select=\"count(exsl:node-set($t)/i)\" "
               "xmlns:exsl='http://exslt.org/common'/>"),
     SOURCE, "<out>2</out>"},
    {"exsl-function-available",
     VERSION_1("", "<xsl:value-of select=\"function-available('exsl:node-set')\" "
                   "xmlns:exsl='http://exslt.org/common'/>"),
     SOURCE, "<out>true</out>"},
    {"exsl-object-type",
     VERSION_1("", "<xsl:value-of xmlns:exsl='http://exslt.org/common' select=\""
                   "concat(exsl:object-type('a'), ' ', exsl:object-type(1), ' ', "
                   "exsl:object-type(true()), ' ', exsl:object-type(//v), ' ', "
                   "exsl:object-type(/doc/none))\"/>"),
     SOURCE, "<out>string number boolean node-set node-set</out>"},
    /* exsl:document is xsl:result-document in an extension namespace,
       and a literal result element outside one (D206). */
    {"exsl-document-available",
     VERSION_1("", "<xsl:value-of select=\"element-available('exsl:document')\" "
                   "xmlns:exsl='http://exslt.org/common'/>"),
     SOURCE, "<out>true</out>"},
    {"exsl-document-principal",
     "<xsl:stylesheet version='1.0' " XSL_NAMESPACE " xmlns:exsl='http://exslt.org/common' "
     "extension-element-prefixes='exsl'><xsl:template name='main'>"
     "<exsl:document method='text'><i>x</i></exsl:document></xsl:template></xsl:stylesheet>",
     SOURCE, "x"},
    {"exsl-document-literal",
     VERSION_1("", "<e:document xmlns:e='http://exslt.org/common'/>"), SOURCE,
     "<out><e:document xmlns:e=\"http://exslt.org/common\"/></out>"},
};

static void test_xslt_1_behaviour(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < sizeof(behaviour_cases) / sizeof(behaviour_cases[0]);
         case_index += 1) {
        const behaviour_case *current = &behaviour_cases[case_index];
        transformation_run run = {
            .stylesheet_text = sxml_byte_view_from_c_string(current->stylesheet_text),
            .source_text = sxml_byte_view_from_c_string(current->source_text)};
        const sxml_status status = run_transformation(&test_fixture, &run);
        const bool expects_error = strncmp(current->expected, "error:", 6) == 0;
        const bool passed =
            expects_error
                ? status != SXML_STATUS_OK &&
                      sxml_byte_view_equals(run.error_record.code.local_name,
                                            sxml_byte_view_from_c_string(current->expected + 6))
                : status == SXML_STATUS_OK &&
                      output_is(&test_fixture, sxml_byte_view_from_c_string(current->expected));
        (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                              .expression = current->test_case_name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Compiles and runs one 1.0 stylesheet with allocation failing_number
   failing. */
static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(failing_number, &test_fixture);
    if (status == SXML_STATUS_OK) {
        transformation_run run = {
            .stylesheet_text = TEXT(VERSION_1(
                "", "<a b='{//v}'><xsl:value-of select=\"//v + '1' = 4\"/>"
                    "<xsl:number value='//v'/></a>")),
            .source_text = TEXT(SOURCE)};
        status = run_transformation(&test_fixture, &run);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state,
                             output_is(&test_fixture, TEXT("<out><a b=\"3\">true3</a></out>")));
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_behaviour_survives_every_allocation_failure(sxml_test_state *state) {
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

static const sxml_test_case xslt_compatibility_test_cases[] = {
    {.name = "xpath_1_compatibility_mode", .function = test_xpath_1_compatibility_mode},
    {.name = "xslt_1_behaviour", .function = test_xslt_1_behaviour},
    {.name = "behaviour_survives_every_allocation_failure",
     .function = test_behaviour_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_compatibility",
            .cases = xslt_compatibility_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_compatibility_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
