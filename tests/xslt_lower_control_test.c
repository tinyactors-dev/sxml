/*
 * Tests for XSLT control flow and variables (src/xslt_lower_control.c;
 * D145, packet 4.7): xsl:if, choose, switch, for-each, iterate, variable,
 * try/catch, where-populated, on-empty, on-non-empty, message, assert,
 * fork and evaluate; global variables and parameters.
 *
 * Golden file: tests/xslt_lower_test.c (read, lower around the initial
 * template, evaluate, write the value by the adaptive method with items
 * separated by a space, "error:CODE" for an error).
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
    "<xsl:stylesheet version='4.0' " XSL_NAMESPACE                                                 \
    " xmlns:err='http://www.w3.org/2005/xqt-errors' exclude-result-prefixes='err'>"                \
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
    /* No host binds a parameter: every global starts absent. */
    static sxml_evaluator_slot absent_globals[64];
    for (size_t slot_index = 0; slot_index < SXML_TEST_COUNT(absent_globals); slot_index += 1) {
        absent_globals[slot_index] = (sxml_evaluator_slot){
            .value = sxml_sequence_empty(), .slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT};
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){
                .unit = unit,
                .has_context_value = true,
                .context_value = sxml_sequence_from_item(document),
                .global_values = {.slots = absent_globals,
                                  .slot_count = unit->global_variable_count > 1
                                                    ? unit->global_variable_count - 1
                                                    : 0},
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

/* 8.1 to 8.3: xsl:if, xsl:choose, xsl:switch. */
static void test_conditionals(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:if test='//item'>yes</xsl:if><xsl:if test='//none'>no</xsl:if>"), "yes"},
        {STYLESHEET("<xsl:if test='1 = 1' then='\"a\"' else='\"b\"'/>"
                    "<xsl:if test='1 = 2' then='\"a\"' else='\"b\"'/>"),
         "\"a\" \"b\""},
        {STYLESHEET("<xsl:if test='1' then='2'>x</xsl:if>"), "error:XTSE3185"},
        {STYLESHEET("<xsl:if test='(1, 2)'>x</xsl:if>"), "error:FORG0006"},
        {STYLESHEET("<xsl:choose><xsl:when test='false()'>a</xsl:when>"
                    "<xsl:when test='true()' select='\"b\"'/>"
                    "<xsl:when test='error()'>c</xsl:when></xsl:choose>"),
         "\"b\""},
        {STYLESHEET("<xsl:choose><xsl:when test='false()'>a</xsl:when>"
                    "<xsl:otherwise>o</xsl:otherwise></xsl:choose>|"
                    "<xsl:choose><xsl:when test='false()'>a</xsl:when></xsl:choose>"),
         "o |"},
        {STYLESHEET("<xsl:choose><xsl:when test='1' select='1'>x</xsl:when></xsl:choose>"),
         "error:XTSE3185"},
        {STYLESHEET("<xsl:for-each select='1 to 4'><xsl:switch select='. mod 3'>"
                    "<xsl:when test='0' select='\"zero\"'/><xsl:when test='1, 2' select='.'/>"
                    "</xsl:switch></xsl:for-each>"),
         "1 2 \"zero\" 4"},
        {STYLESHEET("<xsl:switch select='\"b\"'><xsl:when test='\"a\"'>A</xsl:when>"
                    "<xsl:otherwise>O</xsl:otherwise></xsl:switch>"),
         "O"},
        {STYLESHEET("<xsl:switch select='(1, 2)'><xsl:when test='1'>A</xsl:when></xsl:switch>"),
         "error:XPTY0004"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 7.1, 9: xsl:for-each and local variables. */
static void test_for_each_and_variables(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:for-each select='//item'><i p='{position()}/{last()}'>"
                    "<xsl:value-of select='@n'/></i></xsl:for-each>"),
         "<i p=\"1/2\">1</i> <i p=\"2/2\">2</i>"},
        {STYLESHEET("<a><xsl:for-each select='1 to 3' separator=', '>"
                    "<xsl:sequence select='. * 2'/></xsl:for-each></a>"),
         "<a>2, 4, 6</a>"},
        {STYLESHEET("<xsl:variable name='x' select='2'/><xsl:variable name='y' select='$x + 1'/>"
                    "<xsl:sequence select='$x * $y'/>"),
         "6"},
        {STYLESHEET("<xsl:variable name='x'/><xsl:sequence select='string-length($x), "
                    "$x instance of xs:string'/>"),
         "0 true()"},
        {STYLESHEET("<xsl:variable name='d'><a/>t</xsl:variable>"
                    "<xsl:sequence select='$d instance of document-node(), count($d/node())'/>"),
         "true() 2"},
        {STYLESHEET("<xsl:variable name='s' as='xs:integer*'><xsl:sequence select='1 to 3'/>"
                    "</xsl:variable><xsl:sequence select='sum($s)'/>"),
         "6"},
        {STYLESHEET("<xsl:variable name='m'><xsl:map/></xsl:variable>"
                    "<xsl:sequence select='$m instance of map(*)'/>"),
         "true()"},
        {STYLESHEET("<xsl:variable name='e' as='xs:integer?'/><xsl:sequence select='count($e)'/>"),
         "0"},
        {STYLESHEET("<xsl:variable name='t' as='xs:integer' select='\"x\"'/>"
                    "<xsl:sequence select='$t'/>"),
         "error:XTTE0570"},
        {STYLESHEET("<xsl:variable name='v' select='1'>x</xsl:variable>"), "error:XTSE0620"},
        /* 9.9: shadowing, and the scope ends with the sequence constructor. */
        {STYLESHEET("<xsl:variable name='v' select='1'/><a><xsl:variable name='v' select='2'/>"
                    "<xsl:value-of select='$v'/></a><xsl:sequence select='$v'/>"),
         "<a>2</a> 1"},
        {STYLESHEET("<xsl:for-each select='1 to 2'><xsl:variable name='i' select='. * 10'/>"
                    "<xsl:for-each select='1 to 2'><xsl:sequence select='$i + .'/>"
                    "</xsl:for-each></xsl:for-each>"),
         "11 12 21 22"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 7.2: xsl:iterate, its parameters, xsl:next-iteration, xsl:break and
   xsl:on-completion. */
static void test_iterate(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:iterate select='1 to 5'><xsl:param name='total' select='0'/>"
                    "<xsl:sequence select='$total + .'/>"
                    "<xsl:next-iteration><xsl:with-param name='total' select='$total + .'/>"
                    "</xsl:next-iteration></xsl:iterate>"),
         "1 3 6 10 15"},
        {STYLESHEET("<xsl:iterate select='1 to 5'><xsl:param name='total' select='0'/>"
                    "<xsl:on-completion select='\"total\", $total'/>"
                    "<xsl:next-iteration><xsl:with-param name='total' select='$total + .'/>"
                    "</xsl:next-iteration></xsl:iterate>"),
         "\"total\" 15"},
        {STYLESHEET("<xsl:iterate select='1 to 10'><xsl:param name='n' select='0'/>"
                    "<xsl:on-completion>done</xsl:on-completion>"
                    "<xsl:choose><xsl:when test='. gt 3'><xsl:break select='\"stop\", $n'/>"
                    "</xsl:when><xsl:otherwise><xsl:sequence select='.'/>"
                    "<xsl:next-iteration><xsl:with-param name='n' select='$n + 1'/>"
                    "</xsl:next-iteration></xsl:otherwise></xsl:choose></xsl:iterate>"),
         "1 2 3 \"stop\" 3"},
        {STYLESHEET("<xsl:iterate select='()'><xsl:param name='p' select='7'/>"
                    "<xsl:on-completion select='$p'/><x/></xsl:iterate>"),
         "7"},
        {STYLESHEET("<xsl:iterate select='//item'><xsl:sequence select='position(), last(), "
                    "string(@n)'/></xsl:iterate>"),
         "1 2 \"1\" 2 2 \"2\""},
        {STYLESHEET("<xsl:variable name='k' select='100'/><xsl:iterate select='1 to 2'>"
                    "<xsl:variable name='d' select='. + $k'/><xsl:sequence select='$d'/>"
                    "</xsl:iterate>"),
         "101 102"},
        {STYLESHEET("<xsl:iterate select='1 to 3'><xsl:param name='p' as='xs:integer' "
                    "select='0'/><xsl:next-iteration><xsl:with-param name='p' select='\"x\"'/>"
                    "</xsl:next-iteration></xsl:iterate>"),
         "error:XTTE0590"},
        {STYLESHEET("<xsl:iterate select='1'><xsl:next-iteration>"
                    "<xsl:with-param name='q' select='1'/></xsl:next-iteration></xsl:iterate>"),
         "error:XTSE3130"},
        /* 7.2: XTSE3120 outside a tail position; xsl:if, xsl:choose and
           xsl:try bodies in tail position may hold them. */
        {STYLESHEET("<xsl:iterate select='1'><xsl:if test='true()'><xsl:break/><x/></xsl:if>"
                    "</xsl:iterate>"),
         "error:XTSE3120"},
        {STYLESHEET("<xsl:break/>"), "error:XTSE3120"},
        {STYLESHEET("<xsl:iterate select='1 to 3'><xsl:try><xsl:sequence select='.'/>"
                    "<xsl:if test='. = 2'><xsl:break/></xsl:if>"
                    "<xsl:catch><xsl:break/></xsl:catch></xsl:try></xsl:iterate>"),
         "1 2"},
        /* Nested iterations, and a sum over many items: a loop, no
           recursion (D71). */
        {STYLESHEET("<xsl:iterate select='1 to 2'><xsl:iterate select='1 to 2'>"
                    "<xsl:param name='p' select='0'/><xsl:sequence select='$p'/>"
                    "<xsl:next-iteration><xsl:with-param name='p' select='$p + 1'/>"
                    "</xsl:next-iteration></xsl:iterate></xsl:iterate>"),
         "0 1 0 1"},
        {STYLESHEET("<xsl:iterate select='1 to 100000'><xsl:param name='s' select='0'/>"
                    "<xsl:on-completion select='$s'/><xsl:next-iteration>"
                    "<xsl:with-param name='s' select='$s + .'/></xsl:next-iteration>"
                    "</xsl:iterate>"),
         "5000050000"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 8.4: xsl:try and xsl:catch with the $err: variables. */
static void test_try(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:try select='1 div 0'><xsl:catch select='\"caught\"'/></xsl:try>"),
         "\"caught\""},
        {STYLESHEET("<xsl:try><xsl:sequence select='error(xs:QName(\"err:X\"), \"d\")'/>"
                    "<xsl:catch errors='err:Y'>y</xsl:catch>"
                    "<xsl:catch errors='err:X' select='local-name-from-QName($err:code), "
                    "$err:description'/></xsl:try>"),
         "\"X\" \"d\""},
        {STYLESHEET("<xsl:try select='1'><xsl:catch>c</xsl:catch></xsl:try>"), "1"},
        {STYLESHEET("<xsl:try select='1 div 0'><xsl:catch errors='err:FOAR0002'>c</xsl:catch>"
                    "</xsl:try>"),
         "error:FOAR0001"},
        {STYLESHEET("<xsl:try select='1'><x/><xsl:catch/></xsl:try>"), "error:XTSE3140"},
        {STYLESHEET("<xsl:try select='1 div 0'><xsl:catch errors='*:FOAR0001'>"
                    "<xsl:sequence select='map:size($err:map) gt 0'/></xsl:catch></xsl:try>"),
         "true()"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 8.5: xsl:where-populated, xsl:on-empty, xsl:on-non-empty. */
static void test_conditional_content(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:where-populated><a/><b>x</b><xsl:sequence select='\"\", 1, map{}'/>"
                    "</xsl:where-populated>"),
         "<b>x</b> 1"},
        {STYLESHEET("<ul><xsl:for-each select='//none'><li/></xsl:for-each>"
                    "<xsl:on-empty>none</xsl:on-empty></ul>"),
         "<ul>none</ul>"},
        {STYLESHEET("<ul><xsl:on-non-empty><h/></xsl:on-non-empty>"
                    "<xsl:for-each select='//item'><li/></xsl:for-each>"
                    "<xsl:on-empty>none</xsl:on-empty></ul>"),
         "<ul><h/><li/><li/></ul>"},
        {STYLESHEET("<ul><xsl:on-non-empty><h/></xsl:on-non-empty><xsl:sequence select='\"\"'/>"
                    "</ul>"),
         "<ul/>"},
        {STYLESHEET("<xsl:on-empty select='\"only\"'/>"), "\"only\""},
        {STYLESHEET("<a><xsl:on-empty select='1'/><b/></a>"), "error:XTSE0010"},
        {STYLESHEET("<a><xsl:on-empty select='1'/><xsl:on-non-empty select='2'/></a>"),
         "error:XTSE0010"},
        /* 9.9: each xsl:on-non-empty sees the variables before it. */
        {STYLESHEET("<a><xsl:variable name='x' select='1'/><xsl:on-non-empty select='$x'/>"
                    "<b/><xsl:variable name='x' select='2'/><xsl:on-non-empty select='$x'/></a>"),
         "<a>1<b/>2</a>"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 23, 16.1, 10.5: xsl:message, xsl:assert, xsl:fork, xsl:evaluate. */
static void test_messages_and_others(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:message>note</xsl:message><a/>"), "<a/>"},
        {STYLESHEET("<xsl:message terminate='yes' select='\"stop\"'/><a/>"), "error:XTMM9000"},
        {STYLESHEET("<xsl:message terminate='{1 = 1}'>x</xsl:message>"), "error:XTMM9000"},
        {STYLESHEET("<xsl:message terminate='{1 = 2}'>x</xsl:message>ok"), "ok"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE " xmlns:my='urn:my'>"
         "<xsl:template name='xsl:initial-template'><xsl:try>"
         "<xsl:message terminate='yes' error-code='my:stop'>m</xsl:message>"
         "<xsl:catch select='namespace-uri-from-QName($err:code), string($err:value)'/>"
         "</xsl:try></xsl:template></xsl:stylesheet>",
         "\"urn:my\" \"m\""},
        {STYLESHEET("<xsl:message terminate='maybe'/>"), "error:XTSE0020"},
        /* 23.1: an EQName code, a computed one with the instruction's
           namespaces, an invalid one (the default), an attribute at the
           top of the body (no failure). */
        {STYLESHEET("<xsl:try><xsl:message terminate='yes' error-code='Q{{urn:q}}e'/>"
                    "<xsl:catch select='string($err:code), namespace-uri-from-QName($err:code)'/>"
                    "</xsl:try>"),
         "\"e\" \"urn:q\""},
        {STYLESHEET("<a xmlns:p='urn:p'><xsl:try><xsl:message terminate='yes' "
                    "error-code='{\"p\"}:x'/><xsl:catch select='namespace-uri-from-QName("
                    "$err:code)'/></xsl:try></a>"),
         "<a xmlns:p=\"urn:p\">urn:p</a>"},
        {STYLESHEET("<xsl:message terminate='yes' error-code='1 2'/>"), "error:XTMM9000"},
        {STYLESHEET("<xsl:message select='//@n'/>ok"), "ok"},
        {STYLESHEET("<xsl:assert test='1 = 1'>fine</xsl:assert>ok"), "ok"},
        {STYLESHEET("<xsl:assert test='1 = 2'>bad</xsl:assert>"), "error:XTMM9001"},
        {STYLESHEET("<xsl:fork><xsl:sequence select='1'/><xsl:sequence select='2'/></xsl:fork>"),
         "1 2"},
        {STYLESHEET("<xsl:evaluate xpath='\"1\"'/>"), "error:XTDE3175"},
        {STYLESHEET("<xsl:evaluate xpath='\"1\"'><xsl:fallback>f</xsl:fallback></xsl:evaluate>"),
         "f"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 9.5, 9.11: global variables and parameters. */
static void test_global_variables(sxml_test_state *state) {
    static const transform_case cases[] = {
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"
         "<xsl:variable name='b' select='$a * 2'/><xsl:variable name='a' select='21'/>"
         "<xsl:template name='xsl:initial-template'><xsl:sequence select='$b'/></xsl:template>"
         "</xsl:stylesheet>",
         "42"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"
         "<xsl:variable name='n' select='count(//item)'/><xsl:param name='p' select='$n + 1'/>"
         "<xsl:template name='xsl:initial-template'><xsl:sequence select='$n, $p'/>"
         "<xsl:iterate select='1 to 2'><xsl:sequence select='. + $n'/></xsl:iterate>"
         "</xsl:template></xsl:stylesheet>",
         "2 3 3 4"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"
         "<xsl:variable name='d'><a/></xsl:variable>"
         "<xsl:template name='xsl:initial-template'><xsl:copy-of select='$d/a'/></xsl:template>"
         "</xsl:stylesheet>",
         "<a/>"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"
         "<xsl:variable name='a' select='$b'/><xsl:variable name='b' select='$a'/>"
         "<xsl:template name='xsl:initial-template'>x</xsl:template></xsl:stylesheet>",
         "error:XTDE0640"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"
         "<xsl:variable name='a' select='$a + 1'/>"
         "<xsl:template name='xsl:initial-template'>x</xsl:template></xsl:stylesheet>",
         "error:XPST0008"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"
         "<xsl:variable name='a' select='1'/><xsl:variable name='a' select='2'/>"
         "<xsl:template name='xsl:initial-template'>x</xsl:template></xsl:stylesheet>",
         "error:XTSE0630"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"
         "<xsl:param name='r' required='yes'/>"
         "<xsl:template name='xsl:initial-template'>x</xsl:template></xsl:stylesheet>",
         "error:XTDE0050"},
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"
         "<xsl:variable name='t' as='xs:integer' select='\"x\"'/>"
         "<xsl:template name='xsl:initial-template'><xsl:sequence select='$t'/></xsl:template>"
         "</xsl:stylesheet>",
         "error:XTTE0570"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

static const sxml_test_case xslt_lower_control_test_cases[] = {
    {.name = "conditionals", .function = test_conditionals},
    {.name = "for_each_and_variables", .function = test_for_each_and_variables},
    {.name = "iterate", .function = test_iterate},
    {.name = "try", .function = test_try},
    {.name = "conditional_content", .function = test_conditional_content},
    {.name = "messages_and_others", .function = test_messages_and_others},
    {.name = "global_variables", .function = test_global_variables},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_lower_control",
            .cases = xslt_lower_control_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_lower_control_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
