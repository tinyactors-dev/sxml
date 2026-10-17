/*
 * Tests for XSLT lowering (src/xslt_lower.c, src/xslt_lower_construct.c,
 * src/xslt_simple_content.c; D142, packet 4.6): sequence constructors and
 * the instructions that construct nodes, maps and arrays.
 *
 * Golden file: tests/xquery_lower_construct_test.c (compile, evaluate,
 * write the value by the adaptive method with items separated by a space,
 * "error:CODE" for an error). Stylesheets are read with
 * sxml_xslt_read_stylesheet and lowered around their initial template
 * (xsl:initial-template, or the template of a simplified module) with
 * sxml_xslt_lower_initial_template; the global context item is the
 * document node of a small source document.
 */
#include "evaluator_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "test_runner.h"
#include "xslt_lower_internal.h"
#include "xslt_read_internal.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

#define XSL_NAMESPACE "xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"

/* Wraps a template body in a stylesheet whose initial template it is. */
#define STYLESHEET(body)                                                                           \
    "<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"                                             \
    "<xsl:template name='xsl:initial-template'>" body "</xsl:template></xsl:stylesheet>"

static const char source_document[] =
    "<doc xmlns:s='urn:s'><item n='1'>one</item><item n='2'>two<s:b/></item>"
    "<!--c--><?p q?></doc>";

/* What compiling and evaluating one stylesheet gave. */
typedef struct transform_outcome {
    sxml_status status;
    sxml_error_record error_record;
    char text[4096];
} transform_outcome;

/* Writes value by the adaptive method into outcome->text. */
static sxml_status serialize_value(sxml_processor *processor, sxml_sequence value,
                                   transform_outcome *outcome) {
    sxml_output_buffer *output_buffer = NULL;
    sxml_status status = sxml_output_buffer_create(
        sxml_default_allocator(),
        &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                            .maximum_byte_count = sizeof(outcome->text) - 1},
        &output_buffer);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            processor->item_heap,
            &(sxml_serialization_request){
                .value = value,
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                             .has_item_separator = true,
                                             .item_separator = SXML_BYTE_VIEW_FROM_LITERAL(" ")},
                .error_record = &outcome->error_record},
            sxml_output_buffer_as_output_stream(output_buffer));
    }
    if (status == SXML_STATUS_OK) {
        const sxml_byte_view bytes = sxml_output_buffer_get_bytes(output_buffer);
        memcpy(outcome->text, bytes.bytes, bytes.byte_count);
        outcome->text[bytes.byte_count] = '\0';
    }
    sxml_output_buffer_destroy(output_buffer);
    return status;
}

/* Evaluates unit with the source document as the context item. */
static sxml_status evaluate_unit(sxml_processor *processor, sxml_ir_unit *unit,
                                 transform_outcome *outcome) {
    sxml_item document = {.representation = 0};
    sxml_status status = sxml_processor_parse_document(
        processor,
        &(sxml_xml_parse_request){.input = sxml_byte_view_from_c_string(source_document),
                                  .source_name = SXML_BYTE_VIEW_FROM_LITERAL("source.xml")},
        &document);
    sxml_dynamic_context *dynamic_context = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_create(
            sxml_default_allocator(),
            &(sxml_evaluator_configuration){.item_heap = processor->item_heap,
                                            .allocator = processor->allocator,
                                            .function_table =
                                                sxml_processor_get_function_table(processor)},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit,
                                       .has_context_value = true,
                                       .context_value = sxml_sequence_from_item(document),
                                       .global_values = {.slots = NULL, .slot_count = 0},
                                       .static_base_uri = {.bytes = NULL, .byte_count = 0},
                                       .error_record = &outcome->error_record},
            &value);
    }
    if (status == SXML_STATUS_OK) {
        status = serialize_value(processor, value, outcome);
    }
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_item_release(document);
    return status;
}

typedef struct transform_request {
    const char *stylesheet;
    bool builds_result_document;
    /* Writes the unit's IR instead of evaluating it. */
    bool writes_ir;
} transform_request;

/* Reads, lowers and evaluates (or writes) the stylesheet; outcome->text is
   the value written, or "error:CODE" for an error with a code. */
static void run_transform(transform_request request, transform_outcome *outcome) {
    memset(outcome, 0, sizeof(*outcome));
    sxml_processor *processor = NULL;
    outcome->status = sxml_processor_create(
        sxml_default_allocator(),
        &(sxml_processor_configuration){.allocator = sxml_default_allocator()}, &processor);
    sxml_xslt_ast *ast = NULL;
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_xslt_read_stylesheet(
            &(sxml_xslt_read_request){
                .processor = processor,
                .principal_module_text = sxml_byte_view_from_c_string(request.stylesheet),
                .principal_module_uri = SXML_BYTE_VIEW_FROM_LITERAL("file:///test.xsl"),
                .error_record = &outcome->error_record},
            &ast);
    }
    sxml_ir_unit *unit = NULL;
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_xslt_lower_initial_template(
            &(sxml_xslt_lowering_request){.ast = ast,
                                          .processor = processor,
                                          .template_node = {.value = 0},
                                          .builds_result_document = request.builds_result_document,
                                          .error_record = &outcome->error_record},
            &unit);
    }
    if (outcome->status == SXML_STATUS_OK && request.writes_ir) {
        size_t byte_count = 0;
        outcome->status =
            sxml_ir_unit_write_text(unit,
                                    (sxml_memory_view){.bytes = (uint8_t *)outcome->text,
                                                       .byte_count = sizeof(outcome->text) - 1},
                                    &byte_count);
        if (outcome->status == SXML_STATUS_OK) {
            outcome->text[byte_count] = '\0';
        }
    } else if (outcome->status == SXML_STATUS_OK) {
        outcome->status = evaluate_unit(processor, unit, outcome);
    }
    if (outcome->status != SXML_STATUS_OK) {
        if (sxml_status_has_error_record(outcome->status)) {
            const sxml_byte_view code = outcome->error_record.code.local_name;
            (void)snprintf(outcome->text, sizeof(outcome->text), "error:%.*s", (int)code.byte_count,
                           (const char *)code.bytes);
        } else {
            const sxml_byte_view description = outcome->error_record.diagnostic.description;
            (void)snprintf(outcome->text, sizeof(outcome->text), "status:%s %.*s",
                           (const char *)sxml_status_name(outcome->status).bytes,
                           (int)description.byte_count, (const char *)description.bytes);
        }
    }
    sxml_ir_unit_release(unit);
    sxml_xslt_ast_destroy(ast);
    if (processor != NULL) {
        sxml_item_heap_clear_raised_error(processor->item_heap);
    }
    sxml_processor_destroy(processor);
}

/* ---------------------------------------------------------- The cases */

typedef struct transform_case {
    const char *stylesheet;
    const char *expected_text;
} transform_case;

/* Cases to check, and how their result is delivered. */
typedef struct case_list {
    const transform_case *cases;
    size_t case_count;
    bool builds_result_document;
} case_list;

static bool check_cases(sxml_test_state *state, case_list list) {
    const transform_case *cases = list.cases;
    const bool builds_result_document = list.builds_result_document;
    bool all_match = true;
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        transform_outcome outcome;
        run_transform((transform_request){.stylesheet = cases[case_index].stylesheet,
                                          .builds_result_document = builds_result_document,
                                          .writes_ir = false},
                      &outcome);
        if (strcmp(outcome.text, cases[case_index].expected_text) != 0) {
            printf("case %zu: %s\n  expected: %s\n  got:      %s\n", case_index,
                   cases[case_index].stylesheet, cases[case_index].expected_text, outcome.text);
            all_match = false;
        }
    }
    SXML_TEST_EXPECT(state, all_match);
    return all_match;
}

/* 11.1: literal result elements, attribute value templates, namespace
   nodes, exclude-result-prefixes, xsl:namespace-alias. */
static void test_literal_result_elements(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<a b='x{1+1}y'>t<c/></a>"), "<a b=\"x2y\">t<c/></a>"},
        {STYLESHEET("<a b='{{lit}}' c='{()}' d=''/>"), "<a b=\"{lit}\" c=\"\" d=\"\"/>"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE
         " xmlns:p='urn:p' xmlns:q='urn:q' exclude-result-prefixes='q'>"
         "<xsl:template name='xsl:initial-template'><p:a q:b='1'/></xsl:template>"
         "</xsl:stylesheet>",
         "<p:a xmlns:p=\"urn:p\" xmlns:q=\"urn:q\" q:b=\"1\"/>"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE
         " xmlns:p='urn:p' xmlns:q='urn:q' exclude-result-prefixes='#all'>"
         "<xsl:template name='xsl:initial-template'><a/></xsl:template></xsl:stylesheet>",
         "<a/>"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE
         " xmlns:axsl='urn:alias' xmlns:out='http://www.w3.org/1999/XSL/Transform'>"
         "<xsl:namespace-alias stylesheet-prefix='axsl' result-prefix='out'/>"
         "<xsl:template name='xsl:initial-template'><axsl:template match='/'/></xsl:template>"
         "</xsl:stylesheet>",
         /* 11.1.4: a namespace node whose URI is a result URI is copied,
            excluded or not. */
         "<out:template xmlns:xsl=\"http://www.w3.org/1999/XSL/Transform\" "
         "xmlns:out=\"http://www.w3.org/1999/XSL/Transform\" match=\"/\"/>"},
        /* 5.7.1: adjacent atomic values joined with spaces across
           instructions; text nodes are not separated. */
        {STYLESHEET("<a><xsl:sequence select='1'/><xsl:sequence select='2, 3'/>x"
                    "<xsl:sequence select='4'/></a>"),
         "<a>1 2 3x4</a>"},
        /* A simplified stylesheet module (3.7). */
        {"<out xsl:version='4.0' " XSL_NAMESPACE "><xsl:value-of select='count(//item)'/></out>",
         "<out>2</out>"},
        {STYLESHEET("<a xsl:inherit-namespaces='no' xmlns:p='urn:p'>"
                    "<b/></a>"),
         "<a xmlns:p=\"urn:p\"><b/></a>"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 5.6: value templates; 5.7.2: simple content merges adjacent text
   nodes before joining (D120's deviation, fixed). */
static void test_value_templates(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<a b='{//item/text()}' c='{1, 2}' d='{//item}'/>"),
         "<a b=\"onetwo\" c=\"1 2\" d=\"one two\"/>"},
        {"<xsl:stylesheet version='4.0' expand-text='yes' " XSL_NAMESPACE ">"
         "<xsl:template name='xsl:initial-template'><a>n={count(//item)} {{x}}</a>"
         "</xsl:template></xsl:stylesheet>",
         "<a>n=2 {x}</a>"},
        {STYLESHEET("<a b='{//item/@n}'/>"), "<a b=\"1 2\"/>"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 11.2 to 11.8: xsl:element, xsl:attribute, xsl:text, xsl:value-of,
   xsl:comment, xsl:processing-instruction, xsl:namespace. */
static void test_node_instructions(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:element name='e'><xsl:attribute name='a' select='1, 2'/>"
                    "<xsl:attribute name='b'>x<xsl:value-of select='3'/></xsl:attribute>"
                    "</xsl:element>"),
         "<e a=\"1 2\" b=\"x3\"/>"},
        {STYLESHEET("<xsl:element name='{concat(\"p:\", \"e\")}' namespace='urn:p'/>"),
         "<p:e xmlns:p=\"urn:p\"/>"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE " xmlns:p='urn:p'>"
         "<xsl:template name='xsl:initial-template'><xsl:element name='p:e'>"
         "<xsl:attribute name='p:a'>v</xsl:attribute></xsl:element>"
         "<xsl:element name='{\"p:f\"}'/></xsl:template></xsl:stylesheet>",
         "<p:e xmlns:p=\"urn:p\" p:a=\"v\"/> <p:f xmlns:p=\"urn:p\"/>"},
        {STYLESHEET("<xsl:element name='x:e'/>"), "error:XTDE0830"},
        {STYLESHEET("<xsl:element name='1e'/>"), "error:XTDE0820"},
        {STYLESHEET("<xsl:element name='{\"1e\"}'/>"), "error:XTDE0820"},
        {STYLESHEET("<xsl:attribute name='xmlns'>v</xsl:attribute>"), "error:XTDE0855"},
        {STYLESHEET("<xsl:attribute name='a' select='1'>x</xsl:attribute>"), "error:XTSE0840"},
        {STYLESHEET("<a><xsl:text>  t </xsl:text><xsl:text/></a>"), "<a>  t </a>"},
        {STYLESHEET("<a><xsl:value-of select='1 to 3' separator='-'/>|"
                    "<xsl:value-of select='//item'/>|<xsl:value-of>a<b>c</b></xsl:value-of></a>"),
         "<a>1-2-3|one two|ac</a>"},
        {STYLESHEET("<a><xsl:comment>x--y-</xsl:comment>"
                    "<xsl:processing-instruction name='p' select='\"  v?>w\"'/></a>"),
         "<a><!--x- -y- --><?p v? >w?></a>"},
        {STYLESHEET("<xsl:processing-instruction name='xml'/>"), "error:XTDE0890"},
        {STYLESHEET("<a><xsl:namespace name='p' select='\"urn:p\"'/><p:b xmlns:p='urn:p'/></a>"),
         "<a xmlns:p=\"urn:p\"><p:b/></a>"},
        {STYLESHEET("<xsl:namespace name='p' select='\"\"'/>"), "error:XTDE0930"},
        {STYLESHEET("<xsl:comment select='1'>x</xsl:comment>"), "error:XTSE0940"},
        /* 11.3: an empty namespace drops the prefix of a computed name. */
        {STYLESHEET("<a><xsl:attribute name='{\"p:x\"}' namespace='{\"\"}'>v</xsl:attribute>"
                    "<xsl:attribute name='{\"p:y\"}' namespace='urn:p'>w</xsl:attribute></a>"),
         "<a xmlns:p=\"urn:p\" x=\"v\" p:y=\"w\"/>"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 11.5, 11.9: xsl:document, xsl:copy, xsl:copy-of. */
static void test_copies(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:copy-of select='//item[2], 1, //comment(), //@n'/>"),
         "<item xmlns:s=\"urn:s\" n=\"2\">two<s:b/></item> 1 <!--c--> n=\"1\" n=\"2\""},
        {STYLESHEET("<xsl:copy-of select='//item[2]' copy-namespaces='no'/>"),
         "<item n=\"2\">two<s:b xmlns:s=\"urn:s\"/></item>"},
        {STYLESHEET("<xsl:copy select='//item[1]'><xsl:attribute name='m' select='@n'/>"
                    "<xsl:value-of select='.'/></xsl:copy>"),
         "<item xmlns:s=\"urn:s\" m=\"1\">one</item>"},
        {STYLESHEET("<xsl:copy select='//item[1]/@n'><ignored/></xsl:copy>"), "n=\"1\""},
        {STYLESHEET("<xsl:copy select='//item'/>"), "error:XTTE3180"},
        {STYLESHEET("<xsl:copy><x/></xsl:copy>"), "<x/>"},
        {STYLESHEET("<xsl:document><a/>t</xsl:document>"), "<a/>t"},
        {STYLESHEET("<xsl:sequence select='count(.)'/><xsl:sequence>text</xsl:sequence>"),
         "1 text"},
        {STYLESHEET("<xsl:sequence select='1'>x</xsl:sequence>"), "error:XTSE3185"},
        /* 10.4.2 (4.0): xsl:select's text is its expression. */
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE "><xsl:template name='xsl:initial-template'>"
         "<xsl:select>1 + 1, 'x'</xsl:select></xsl:template></xsl:stylesheet>",
         "2 \"x\""},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 21, 22 and xsl:record (4.0): maps, records and arrays. */
static void test_maps_and_arrays(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:map><xsl:map-entry key='\"a\"' select='1'/>"
                    "<xsl:map-entry key='\"b\"'>x</xsl:map-entry></xsl:map>"),
         /* The content is a text node. */
         "{\"a\":1,\"b\":x}"},
        {STYLESHEET("<xsl:map><xsl:map-entry key='1' select='1'/>"
                    "<xsl:map-entry key='1' select='2'/></xsl:map>"),
         "error:XTDE3365"},
        {STYLESHEET("<xsl:map><xsl:sequence select='1'/></xsl:map>"), "error:XTTE3375"},
        {STYLESHEET("<xsl:map duplicates='\"use-last\"'><xsl:map-entry key='1' select='1'/>"
                    "<xsl:map-entry key='1' select='2'/></xsl:map>"),
         "{1:2}"},
        {STYLESHEET("<xsl:record a='1' _b='\"two\"' version='3.1'/>"),
         "{\"a\":1,\"_b\":\"two\",\"version\":3.1}"},
        {STYLESHEET("<xsl:record/>"), "{}"},
        {STYLESHEET("<xsl:record x='1' xsl:duplicates='\"use-first\"'>"
                    "<xsl:map-entry key='\"x\"' select='2'/></xsl:record>"),
         "{\"x\":1}"},
        {STYLESHEET("<xsl:array select='1, 2'/>"), "[1,2]"},
        {STYLESHEET("<xsl:array><xsl:array-member select='1, 2'/><xsl:sequence select='3'/>"
                    "<xsl:array-member/></xsl:array>"),
         "[(1,2),3,()]"},
        {STYLESHEET("<xsl:array for-each='1 to 3' select='., . * 10'/>"), "[(1,10),(2,20),(3,30)]"},
        {STYLESHEET("<xsl:array for-each='(\"a\", \"b\")'><xsl:value-of select='position()'/>"
                    "</xsl:array>"),
         "[1,2]"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 24.3, 3.9: fallback; an instruction of a later packet. */
static void test_fallback(sxml_test_state *state) {
    static const transform_case cases[] = {
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE " xmlns:e='urn:e'"
         " extension-element-prefixes='e'>"
         "<xsl:template name='xsl:initial-template'><e:x><xsl:fallback>f</xsl:fallback>"
         "</e:x><e:y/></xsl:template></xsl:stylesheet>",
         "error:XTDE1450"},
        {"<xsl:stylesheet version='5.0' " XSL_NAMESPACE ">"
         "<xsl:template name='xsl:initial-template'><a><xsl:future>"
         "<xsl:fallback>f</xsl:fallback></xsl:future></a></xsl:template></xsl:stylesheet>",
         "<a>f</a>"},
        {STYLESHEET("<xsl:sequence select='1'><xsl:fallback>f</xsl:fallback></xsl:sequence>"), "1"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 2.4: the principal result is built into a document node. */
static void test_result_document(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<a/><xsl:sequence select='1, 2'/>"), "<a/>1 2"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = true});
}

/* The IR of a literal result element: one element constructor whose
   content is its attribute's constructor and the sequence constructor, in
   the template function the main function calls when there is a global
   context item (D144); its variant for an absent focus and the unit's
   built-in template follow. */
static void test_golden_ir(sxml_test_state *state) {
    transform_outcome outcome;
    run_transform((transform_request){.stylesheet = STYLESHEET("<a b='{.}'>t</a>"),
                                      .builds_result_document = false,
                                      .writes_ir = true},
                  &outcome);
    static const char expected[] =
        "(function 1 main (parameters 0) (captures) (slots 6) (focus $1 _ _)\n"
        "  (let $4 (try-catch\n"
        "            (let $2 (static-call fn:exists#1 $1)\n"
        "              $2)\n"
        "            (catch (err:XPDY0002) $3\n"
        "              false()))\n"
        "    (let $6 (conditional $4\n"
        "              (static-call function:2#5 $1 1 1 map-item() map-item())\n"
        "              (let $5 (sequence)\n"
        "                (static-call function:3#5 $5 0 0 map-item() map-item())))\n"
        "      $6)))\n"
        "(function 2 anonymous (parameters 5) (captures) (slots 11) (focus $1 $2 $3)\n"
        "  (let $6 (static-call Q{}merge-text-nodes#1 $1)\n"
        "    (let $7 (coerce $6 xs:anyAtomicType* function-argument)\n"
        "      (let $8 (static-call fn:string-join#2 $7 \" \")\n"
        "        (let $9 (attribute-constructor #b $8)\n"
        "          (let $10 (text-constructor _ \"t\")\n"
        "            (let $11 (element-constructor #a _ xslt-rules\n"
        "                       (sequence $9 $10))\n"
        "              $11)))))))\n";
    const bool matches = strncmp(outcome.text, expected, sizeof(expected) - 1) == 0;
    if (!matches) {
        printf("got:\n%s\n", outcome.text);
    }
    SXML_TEST_EXPECT(state, matches);
}

static const sxml_test_case xslt_lower_test_cases[] = {
    {.name = "literal_result_elements", .function = test_literal_result_elements},
    {.name = "value_templates", .function = test_value_templates},
    {.name = "node_instructions", .function = test_node_instructions},
    {.name = "copies", .function = test_copies},
    {.name = "maps_and_arrays", .function = test_maps_and_arrays},
    {.name = "fallback", .function = test_fallback},
    {.name = "result_document", .function = test_result_document},
    {.name = "golden_ir", .function = test_golden_ir},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_lower",
            .cases = xslt_lower_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_lower_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
