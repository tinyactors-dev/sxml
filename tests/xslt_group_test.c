/*
 * Tests for XSLT sorting and grouping (src/xslt_lower_group.c,
 * src/xslt_group.c; D152, packet 4.9): xsl:sort under xsl:for-each,
 * xsl:apply-templates, xsl:perform-sort and xsl:for-each-group;
 * xsl:for-each-group with group-by, group-adjacent, group-starting-with
 * and group-ending-with; current-group and current-grouping-key;
 * xsl:merge.
 *
 * Golden file: tests/xslt_lower_control_test.c (read, lower around the
 * initial template, evaluate, write the value by the adaptive method with
 * items separated by a space, "error:CODE" for an error).
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
        if (bytes.byte_count > 0) {
            memcpy(outcome->text, bytes.bytes, bytes.byte_count);
        }
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

/* 13.1, 13.3: xsl:sort under xsl:for-each and xsl:apply-templates. */
static void test_sort(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:for-each select='(3, 1, 2)'><xsl:sort/><xsl:sequence select='.'/>"
                    "</xsl:for-each>"),
         "1 2 3"},
        {STYLESHEET("<xsl:for-each select='(3, 1, 2)'><xsl:sort order='descending'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "3 2 1"},
        {STYLESHEET("<xsl:for-each select='(\"10\", \"9\", \"100\")'><xsl:sort/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "\"10\" \"100\" \"9\""},
        {STYLESHEET("<xsl:for-each select='(\"10\", \"9\", \"100\")'>"
                    "<xsl:sort data-type='number'/><xsl:sequence select='.'/></xsl:for-each>"),
         "\"9\" \"10\" \"100\""},
        {STYLESHEET("<xsl:for-each select='(10, 9, 100)'><xsl:sort data-type='text'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "10 100 9"},
        /* Major and minor keys; a stable sort keeps equal items in order. */
        {STYLESHEET("<xsl:for-each select='(\"b2\", \"a2\", \"b1\", \"a1\", \"a2\")'>"
                    "<xsl:sort select='substring(., 1, 1)'/>"
                    "<xsl:sort select='substring(., 2)' order='descending'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "\"a2\" \"a2\" \"a1\" \"b2\" \"b1\""},
        /* 13.1.2: composite keys item by item, () first; descending
           reverses the whole order, () last. */
        {STYLESHEET("<xsl:for-each select='//item'><xsl:sort select='@n' order='descending'/>"
                    "<xsl:value-of select='.'/></xsl:for-each>"),
         "two one"},
        {STYLESHEET("<xsl:variable name='v' select='[(2, 1), (1, 3), (), 1]'/>"
                    "<xsl:for-each select='1 to 4'><xsl:sort select='array:get($v, .)' "
                    "xmlns:array='http://www.w3.org/2005/xpath-functions/array'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "3 4 2 1"},
        {STYLESHEET("<xsl:variable name='v' select='[(2, 1), (1, 3), (), 1]'/>"
                    "<xsl:for-each select='1 to 4'><xsl:sort select='array:get($v, .)' "
                    "order='descending' "
                    "xmlns:array='http://www.w3.org/2005/xpath-functions/array'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "1 2 4 3"},
        /* The focus of a key: position() and last() of the input. */
        {STYLESHEET("<xsl:for-each select='(\"a\", \"b\", \"c\")'>"
                    "<xsl:sort select='last() - position()'/><xsl:sequence select='.'/>"
                    "</xsl:for-each>"),
         "\"c\" \"b\" \"a\""},
        /* Attribute value templates, with the instruction's focus. */
        {STYLESHEET("<xsl:variable name='o' select='\"descending\"'/>"
                    "<xsl:for-each select='(1, 3, 2)'><xsl:sort order='{$o}'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "3 2 1"},
        {STYLESHEET("<xsl:variable name='t' select='\"number\"'/>"
                    "<xsl:for-each select='(\"10\", \"9\")'><xsl:sort data-type='{$t}'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "\"9\" \"10\""},
        {STYLESHEET("<xsl:variable name='o' select='\"up\"'/>"
                    "<xsl:for-each select='(1, 3, 2)'><xsl:sort order='{$o}'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "error:XTDE0030"},
        /* 13.1.3: collations and case-order. */
        {STYLESHEET("<xsl:for-each select='(\"b\", \"B\", \"a\", \"A\")'><xsl:sort/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "\"A\" \"B\" \"a\" \"b\""},
        {STYLESHEET("<xsl:for-each select='(\"b\", \"B\", \"a\", \"A\")'>"
                    "<xsl:sort case-order='upper-first'/><xsl:sequence select='.'/>"
                    "</xsl:for-each>"),
         "\"A\" \"a\" \"B\" \"b\""},
        {STYLESHEET("<xsl:for-each select='(\"b\", \"B\", \"a\", \"A\")'>"
                    "<xsl:sort lang='en' case-order='lower-first'/><xsl:sequence select='.'/>"
                    "</xsl:for-each>"),
         "\"a\" \"A\" \"b\" \"B\""},
        {STYLESHEET("<xsl:for-each select='(\"b\", \"B\", \"a\")'>"
                    "<xsl:sort collation='http://www.w3.org/2005/xpath-functions/collation/"
                    "html-ascii-case-insensitive'/><xsl:sequence select='.'/></xsl:for-each>"),
         "\"a\" \"b\" \"B\""},
        {STYLESHEET("<xsl:for-each select='(\"b\", \"a\")'><xsl:sort collation='urn:none'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "error:XTDE1035"},
        {STYLESHEET("<xsl:for-each select='(1, \"a\")'><xsl:sort/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "error:XTDE1030"},
        /* Static rules. */
        {STYLESHEET("<xsl:for-each select='1'><xsl:sort stable='maybe'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "error:XTSE0020"},
        {STYLESHEET("<xsl:for-each select='1'><xsl:sort lang=\"'de'\"/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "error:XTSE0020"},
        {STYLESHEET("<xsl:for-each select='1'><xsl:sort/><xsl:sort stable='yes'/>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "error:XTSE1017"},
        {STYLESHEET("<xsl:for-each select='1'><xsl:sort select='.'>x</xsl:sort>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "error:XTSE1015"},
        /* A key from content. */
        {STYLESHEET("<xsl:for-each select='(\"x2\", \"y1\")'>"
                    "<xsl:sort><xsl:value-of select='substring(., 2)'/></xsl:sort>"
                    "<xsl:sequence select='.'/></xsl:for-each>"),
         "\"y1\" \"x2\""},
        /* Under xsl:apply-templates. */
        {"<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">"
         "<xsl:template name='xsl:initial-template'>"
         "<xsl:apply-templates select='//item'><xsl:sort select='@n' data-type='number' "
         "order='descending'/></xsl:apply-templates></xsl:template>"
         "<xsl:template match='item'><xsl:sequence select='position(), string(.)'/>"
         "</xsl:template></xsl:stylesheet>",
         "1 \"two\" 2 \"one\""},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 13.2: xsl:perform-sort. */
static void test_perform_sort(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:perform-sort select='(3, 1, 2)'><xsl:sort/></xsl:perform-sort>"),
         "1 2 3"},
        {STYLESHEET("<xsl:perform-sort><xsl:sort select='string-length()'/>"
                    "<xsl:sequence select='\"ccc\", \"a\", \"bb\"'/></xsl:perform-sort>"),
         "\"a\" \"bb\" \"ccc\""},
        {STYLESHEET("<xsl:perform-sort><xsl:sort/></xsl:perform-sort>"), ""},
        {STYLESHEET("<xsl:perform-sort select='1'><xsl:sort/>x</xsl:perform-sort>"),
         "error:XTSE1040"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 14.1: group-by and group-adjacent. */
static void test_grouping_keys(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:for-each-group select='(3, 1, 3, 2, 1)' group-by='.'>"
                    "<xsl:sequence select='[current-grouping-key(), current-group()]'/>"
                    "</xsl:for-each-group>"),
         "[3,(3,3)] [1,(1,1)] [2,2]"},
        /* An item in several groups, once per distinct key. */
        {STYLESHEET("<xsl:for-each-group select='(\"ab\", \"bc\", \"bb\")' "
                    "group-by='string-to-codepoints(.) ! codepoints-to-string(.)'>"
                    "<xsl:sequence select='current-grouping-key() || \":\" || "
                    "string-join(current-group(), \",\")'/></xsl:for-each-group>"),
         "\"a:ab\" \"b:ab,bc,bb\" \"c:bc\""},
        /* Untyped keys are strings; xs:integer and xs:decimal are equal. */
        {STYLESHEET("<xsl:for-each-group select='//item' group-by='@n'>"
                    "<xsl:sequence select='current-grouping-key() instance of xs:string'/>"
                    "</xsl:for-each-group>"),
         "true() true()"},
        {STYLESHEET("<xsl:for-each-group select='(1, 1.0, 2)' group-by='.'>"
                    "<xsl:sequence select='count(current-group())'/></xsl:for-each-group>"),
         "2 1"},
        {STYLESHEET("<xsl:for-each-group select='(\"a\", \"A\", \"b\")' group-by='.' "
                    "collation='http://www.w3.org/2005/xpath-functions/collation/"
                    "html-ascii-case-insensitive'>"
                    "<xsl:sequence select='string-join(current-group())'/>"
                    "</xsl:for-each-group>"),
         "\"aA\" \"b\""},
        {STYLESHEET("<xsl:for-each-group select='(\"a\", \"b\")' group-by='.' "
                    "collation='urn:none'><xsl:sequence select='.'/></xsl:for-each-group>"),
         "error:XTDE1110"},
        /* Composite keys. */
        {STYLESHEET("<xsl:for-each-group select='(\"a1\", \"a2\", \"a1\", \"b\")' "
                    "group-by='substring(., 1, 1), substring(., 2)[.]' composite='yes'>"
                    "<xsl:sequence select='[current-grouping-key(), count(current-group())]'/>"
                    "</xsl:for-each-group>"),
         "[(\"a\",\"1\"),2] [(\"a\",\"2\"),1] [\"b\",1]"},
        {STYLESHEET("<xsl:for-each-group select='(1, 1, 2, 1)' group-adjacent='.'>"
                    "<xsl:sequence select='[position(), last(), current-group()]'/>"
                    "</xsl:for-each-group>"),
         "[1,3,(1,1)] [2,3,2] [3,3,1]"},
        {STYLESHEET("<xsl:for-each-group select='(1, 2)' group-adjacent='(., .)'>"
                    "<xsl:sequence select='.'/></xsl:for-each-group>"),
         "error:XTTE1100"},
        {STYLESHEET("<xsl:for-each-group select='(1, 2, 3)' group-adjacent='(. idiv 2, 0)' "
                    "composite='yes'><xsl:sequence select='[current-grouping-key()]'/>"
                    "</xsl:for-each-group>"),
         "[(0,0)] [(1,0)]"},
        /* The context item is the initial item. */
        {STYLESHEET("<xsl:for-each-group select='(\"x1\", \"y1\", \"z2\")' "
                    "group-by='substring(., 2)'><xsl:sequence select='.'/>"
                    "</xsl:for-each-group>"),
         "\"x1\" \"z2\""},
        {STYLESHEET("<xsl:for-each-group select='()' group-by='.'>x</xsl:for-each-group>"),
         ""},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 14.1: group-starting-with, group-ending-with; 14.2, 14.3. */
static void test_grouping_patterns(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:for-each-group select='(\"h\", 1, 2, \"h\", 3)' "
                    "group-starting-with='.[. instance of xs:string]'>"
                    "<xsl:sequence select='[current-group()]'/></xsl:for-each-group>"),
         "[(\"h\",1,2)] [(\"h\",3)]"},
        {STYLESHEET("<xsl:for-each-group select='(1, 2, \"e\", 3, \"e\")' "
                    "group-ending-with='.[. instance of xs:string]'>"
                    "<xsl:sequence select='[current-group()]'/></xsl:for-each-group>"),
         "[(1,2,\"e\")] [(3,\"e\")]"},
        {STYLESHEET("<xsl:for-each-group select='//doc/node()' group-starting-with='item'>"
                    "<xsl:sequence select='count(current-group())'/></xsl:for-each-group>"),
         "1 3"},
        /* An error while matching is no match (6.5). */
        {STYLESHEET("<xsl:for-each-group select='(1, 2, 3)' "
                    "group-starting-with='.[xs:integer(\"x\")]'>"
                    "<xsl:sequence select='count(current-group())'/></xsl:for-each-group>"),
         "3"},
        /* The grouping key is absent with patterns, also inside another
           group; the current group is absent outside. */
        {STYLESHEET("<xsl:for-each-group select='1' group-by='.'>"
                    "<xsl:for-each-group select='1' group-starting-with='*'>"
                    "<xsl:sequence select='current-grouping-key()'/></xsl:for-each-group>"
                    "</xsl:for-each-group>"),
         "error:XTDE1071"},
        {STYLESHEET("<xsl:sequence select='count(current-group())'/>"), "error:XTDE1061"},
        {STYLESHEET("<xsl:for-each-group select='1' group-by='.'>"
                    "<xsl:sequence select='(function() { current-group() })()'/>"
                    "</xsl:for-each-group>"),
         "error:XTDE1061"},
        {STYLESHEET("<xsl:variable name='f' as='function(*)*'>"
                    "<xsl:for-each-group select='(1, 2)' group-by='.'>"
                    "<xsl:sequence select='current-group#0'/></xsl:for-each-group>"
                    "</xsl:variable><xsl:sequence select='$f ! .()'/>"),
         "1 2"},
        /* Sorted groups: the key sees the group's initial item and the
           current group (14.3, 13.1.1). */
        {STYLESHEET("<xsl:for-each-group select='(\"b\", \"a\", \"b\", \"c\", \"b\")' "
                    "group-by='.'><xsl:sort select='count(current-group())' "
                    "order='descending'/><xsl:sort select='.'/>"
                    "<xsl:sequence select='.'/></xsl:for-each-group>"),
         "\"b\" \"a\" \"c\""},
        {STYLESHEET("<xsl:for-each-group select='1' group-by='.' group-adjacent='.'>"
                    "x</xsl:for-each-group>"),
         "error:XTSE1080"},
        {STYLESHEET("<xsl:for-each-group select='1' group-starting-with='*' composite='yes'>"
                    "x</xsl:for-each-group>"),
         "error:XTSE1090"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* 15: xsl:merge. */
static void test_merge(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:merge>"
                    "<xsl:merge-source name='a' select='(1, 3, 5)'>"
                    "<xsl:merge-key select='.'/></xsl:merge-source>"
                    "<xsl:merge-source name='b' select='(2, 3, 4)'>"
                    "<xsl:merge-key select='.'/></xsl:merge-source>"
                    "<xsl:merge-action><xsl:sequence select='[current-merge-key(), "
                    "count(current-merge-group(\"a\")), current-merge-group(), position()]'/>"
                    "</xsl:merge-action></xsl:merge>"),
         "[1,1,1,1] [2,0,2,2] [3,1,(3,3),3] [4,0,4,4] [5,1,5,5]"},
        /* for-each-item: one input per anchor; descending keys. */
        {STYLESHEET("<xsl:merge><xsl:merge-source for-each-item='([3, 1], [4, 2])' "
                    "select='?*'><xsl:merge-key select='.' order='descending'/>"
                    "</xsl:merge-source><xsl:merge-action>"
                    "<xsl:sequence select='., current-merge-key-array()'/>"
                    "</xsl:merge-action></xsl:merge>"),
         "4 [4] 3 [3] 2 [2] 1 [1]"},
        {STYLESHEET("<xsl:sequence select='current-merge-group()'/>"), "error:XTDE3480"},
        {STYLESHEET("<xsl:sequence select='current-merge-key()'/>"), "error:XTDE3510"},
        {STYLESHEET("<xsl:merge><xsl:merge-source select='1'><xsl:merge-key/>"
                    "</xsl:merge-source><xsl:merge-source select='1'><xsl:merge-key/>"
                    "<xsl:merge-key/></xsl:merge-source><xsl:merge-action/></xsl:merge>"),
         "error:XTSE2200"},
        {STYLESHEET("<xsl:merge><xsl:merge-source name='a' select='1'><xsl:merge-key/>"
                    "</xsl:merge-source><xsl:merge-source name='a' select='1'><xsl:merge-key/>"
                    "</xsl:merge-source><xsl:merge-action/></xsl:merge>"),
         "error:XTSE3190"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

/* No recursion (D71): a long population and many groups. */
static void test_large_input(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:variable name='g'><xsl:for-each-group select='1 to 100000' "
                    "group-adjacent='. idiv 2'><xsl:sequence select='count(current-group())'/>"
                    "</xsl:for-each-group></xsl:variable>"
                    "<xsl:sequence select='count(tokenize($g))'/>"),
         "50001"},
        {STYLESHEET("<xsl:variable name='s' as='xs:integer*'>"
                    "<xsl:perform-sort select='reverse(1 to 100000)'><xsl:sort/>"
                    "</xsl:perform-sort></xsl:variable><xsl:sequence select='$s[1], $s[last()]'/>"),
         "1 100000"},
    };
    check_cases(state, (case_list){.cases = cases,
                                   .case_count = SXML_TEST_COUNT(cases),
                                   .builds_result_document = false});
}

static const sxml_test_case xslt_group_test_cases[] = {
    {.name = "sort", .function = test_sort},
    {.name = "perform_sort", .function = test_perform_sort},
    {.name = "grouping_keys", .function = test_grouping_keys},
    {.name = "grouping_patterns", .function = test_grouping_patterns},
    {.name = "merge", .function = test_merge},
    {.name = "large_input", .function = test_large_input},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_group",
            .cases = xslt_group_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_group_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
