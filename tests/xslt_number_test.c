/*
 * Tests for XSLT numbering and regular expressions (src/xslt_lower_number.c,
 * the format-place-marker row of src/function_library_format.c, the xsl:number
 * format strings of src/format.c and fn:regex-group in
 * src/function_library_xslt.c; D153, packet 4.11): xsl:number with value,
 * select, the three levels, count and from patterns, format tokens,
 * grouping, ordinals, lang, letter-value and start-at; xsl:analyze-string
 * with its two children, fn:regex-group and fn:regex-groups.
 *
 * Golden file: tests/xslt_lower_control_test.c (read, lower around the
 * initial template, evaluate, write the value by the adaptive method with
 * items separated by a space, "error:CODE" for an error).
 */
#include "evaluator_internal.h"
#include "format_internal.h"
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
/* XSLT predeclares no prefix but xml (5.3.1; D261). */
#define XS_NAMESPACE "xmlns:xs='http://www.w3.org/2001/XMLSchema' exclude-result-prefixes='xs'"

/* Wraps templates in a stylesheet whose initial template is body. */
#define STYLESHEET_WITH(templates, body)                                                           \
    "<xsl:stylesheet version='4.0' " XSL_NAMESPACE " " XS_NAMESPACE ">" templates                                   \
    "<xsl:template name='xsl:initial-template'>" body "</xsl:template></xsl:stylesheet>"

#define STYLESHEET(body) STYLESHEET_WITH("", body)

/* Chapters, sections and notes to number. */
static const char source_document[] =
    "<doc><title>T</title>"
    "<chapter><title>A</title><note>n1</note>"
    "<section><title>A1</title><note>n2</note><note>n3</note></section>"
    "<section><title>A2</title><note>n4</note></section></chapter>"
    "<chapter><title>B</title>"
    "<section><title>B1</title><note>n5</note></section></chapter></doc>";

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
    static sxml_evaluator_slot absent_globals[16];
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

/* Reads, lowers and evaluates the stylesheet; outcome->text is the value
   written, or "error:CODE" for an error with a code. */
static void run_transform(const char *stylesheet, transform_outcome *outcome) {
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
                .principal_module_text = sxml_byte_view_from_c_string(stylesheet),
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
                                          .builds_result_document = false,
                                          .error_record = &outcome->error_record},
            &unit);
    }
    if (outcome->status == SXML_STATUS_OK) {
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

typedef struct transform_case {
    const char *stylesheet;
    const char *expected_text;
} transform_case;

typedef struct case_list {
    const transform_case *cases;
    size_t case_count;
} case_list;

static bool check_cases(sxml_test_state *state, case_list list) {
    bool all_match = true;
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        transform_outcome outcome;
        run_transform(list.cases[case_index].stylesheet, &outcome);
        if (strcmp(outcome.text, list.cases[case_index].expected_text) != 0) {
            printf("case %zu: %s\n  expected: %s\n  got:      %s\n", case_index,
                   list.cases[case_index].stylesheet, list.cases[case_index].expected_text,
                   outcome.text);
            all_match = false;
        }
    }
    SXML_TEST_EXPECT(state, all_match);
    return all_match;
}

/* ---------------------------------------------------- Format strings */

/* 12.4: prefix, separators, suffix and the token of each number. */
static void test_format_strings(sxml_test_state *state) {
    const sxml_byte_view format = SXML_BYTE_VIEW_FROM_LITERAL("(A-001(i)");
    const sxml_number_format_affixes affixes = sxml_number_format_find_affixes(format);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(affixes.prefix, SXML_BYTE_VIEW_FROM_LITERAL("(")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(affixes.suffix, SXML_BYTE_VIEW_FROM_LITERAL(")")));
    static const struct {
        size_t number_index;
        const char *separator;
        const char *format_token;
    } parts[] = {{0, ".", "A"}, {1, "-", "001"}, {2, "(", "i"}, {5, "(", "i"}};
    for (size_t part_index = 0; part_index < SXML_TEST_COUNT(parts); part_index += 1) {
        const sxml_number_format_part part = sxml_number_format_find_part(
            (sxml_number_format_query){.format = format,
                                       .number_index = parts[part_index].number_index});
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(part.format_token,
                                                      sxml_byte_view_from_c_string(
                                                          parts[part_index].format_token)));
        if (parts[part_index].number_index > 0) {
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(part.separator,
                                                          sxml_byte_view_from_c_string(
                                                              parts[part_index].separator)));
        }
    }
    /* No format token: the one other run is prefix and suffix, the token
       is "1"; one token: numbers after the first are separated by ".". */
    const sxml_number_format_affixes only = sxml_number_format_find_affixes(
        SXML_BYTE_VIEW_FROM_LITERAL("--"));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(only.prefix, SXML_BYTE_VIEW_FROM_LITERAL("--")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(only.suffix, SXML_BYTE_VIEW_FROM_LITERAL("--")));
    const sxml_number_format_part fallback = sxml_number_format_find_part(
        (sxml_number_format_query){.format = SXML_BYTE_VIEW_FROM_LITERAL("--"), .number_index = 1});
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(fallback.format_token,
                                                  SXML_BYTE_VIEW_FROM_LITERAL("1")));
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(fallback.separator, SXML_BYTE_VIEW_FROM_LITERAL(".")));
    const sxml_number_format_affixes token = sxml_number_format_find_affixes(
        SXML_BYTE_VIEW_FROM_LITERAL("a"));
    SXML_TEST_EXPECT(state, token.prefix.byte_count == 0 && token.suffix.byte_count == 0);
}

/* ---------------------------------------------------------- xsl:number */

/* 12.2, 12.4: the value attribute and the conversion attributes. */
static void test_number_value(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<n><xsl:number value='3'/></n>"), "<n>3</n>"},
        {STYLESHEET("<n><xsl:number value='2.5, 3.5, \"4.4\"'/></n>"), "<n>3.4.4</n>"},
        {STYLESHEET("<n><xsl:number value='5, 13, 7' format='A-001(i)'/></n>"),
         "<n>E-013(vii)</n>"},
        {STYLESHEET("<n><xsl:number value='1, 2, 3' format='(1)'/></n>"), "<n>(1.2.3)</n>"},
        {STYLESHEET("<n><xsl:number value='1234567' grouping-separator=',' "
                    "grouping-size='3'/></n>"),
         "<n>1,234,567</n>"},
        {STYLESHEET("<n><xsl:number value='1234567' grouping-separator='.'/></n>"),
         "<n>1234567</n>"},
        {STYLESHEET("<n><xsl:number value='12' format='0001' grouping-separator=' ' "
                    "grouping-size='2'/></n>"),
         "<n>00 12</n>"},
        {STYLESHEET("<n><xsl:number value='21' format='w' ordinal='yes'/>|"
                    "<xsl:number value='22' ordinal='yes'/>|"
                    "<xsl:number value='4' format='W' ordinal='no'/></n>"),
         "<n>twenty-first|22nd|FOUR</n>"},
        {STYLESHEET("<n><xsl:number value='28' format='a'/>|<xsl:number value='1999' "
                    "format='I'/>|<xsl:number value='0' format='a'/></n>"),
         "<n>ab|MCMXCIX|0</n>"},
        {STYLESHEET("<n><xsl:number value='122' format='Ww'/></n>"),
         "<n>One Hundred and Twenty Two</n>"},
        {STYLESHEET("<n><xsl:number value='1, 1, 1, 1' start-at='3 0 0'/></n>"), "<n>3.0.0.0</n>"},
        {STYLESHEET("<n><xsl:number value='()' format='[1]'/></n>"), "<n>[]</n>"},
        {STYLESHEET("<n><xsl:number value='7' lang='en-GB' letter-value='alphabetic'/></n>"),
         "<n>7</n>"},
        {STYLESHEET("<xsl:number value='-1'/>"), "error:XTDE0980"},
        {STYLESHEET("<xsl:number value='\"x\"'/>"), "error:XTDE0980"},
        {STYLESHEET("<xsl:number value='1' grouping-size='x' grouping-separator=','/>"),
         "error:XTDE0030"},
        {STYLESHEET("<xsl:number value='1' letter-value='odd'/>"), "error:XTDE0030"},
        {STYLESHEET("<xsl:number value='1' start-at='1..2'/>"), "error:XTDE0030"},
        {STYLESHEET("<xsl:number value='1' level='any'/>"), "error:XTSE0975"},
    };
    check_cases(state, (case_list){.cases = cases, .case_count = SXML_TEST_COUNT(cases)});
}

/* 12.3: numbering by position, in each level, with count and from. */
static void test_number_position(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:for-each select='//note'><xsl:number/></xsl:for-each>"),
         "1 1 2 1 1"},
        {STYLESHEET("<xsl:for-each select='//note'><xsl:number level='any'/></xsl:for-each>"),
         "1 2 3 4 5"},
        {STYLESHEET("<xsl:for-each select='//note'><xsl:number level='any' from='chapter'/>"
                    "</xsl:for-each>"),
         "1 2 3 4 1"},
        {STYLESHEET("<xsl:for-each select='//section/title'>"
                    "<xsl:number level='multiple' count='chapter|section' format='A.1'/>"
                    "</xsl:for-each>"),
         "A.1 A.2 B.1"},
        {STYLESHEET("<xsl:for-each select='//note'><xsl:number level='multiple' "
                    "count='chapter|section|note' from='chapter'/></xsl:for-each>"),
         "1.1 1.2.1 1.2.2 1.3.1 2.1.1"},
        {STYLESHEET("<xsl:for-each select='//title'><xsl:number count='section'/>,"
                    "</xsl:for-each>"),
         /* An uncounted title's number is a zero-length text node (D163). */
         " ,  , 1 , 2 ,  , 1 ,"},
        {STYLESHEET("<xsl:number select='(//note)[3]' count='note|section' level='single'/>"),
         "2"},
        {STYLESHEET("<xsl:number select='//chapter[2]' count='*'/>"), "3"},
        {STYLESHEET("<xsl:for-each select='//note'><xsl:number count='note[. ne \"n2\"]'/>"
                    "</xsl:for-each>"),
         /* n2 is not counted: its number is "", a zero-length text node (D163). */
         "1  1 1 1"},
        /* An error while matching is no match (6.5). */
        {STYLESHEET("<xsl:for-each select='//note'><xsl:number level='any' "
                    "count='note[xs:integer(.) gt 0]' format='[1]'/></xsl:for-each>"),
         "[] [] [] [] []"},
        {STYLESHEET("<xsl:number select='//note'/>"), "error:XTTE1000"},
        {STYLESHEET("<xsl:for-each select='1'><xsl:number/></xsl:for-each>"), "error:XTTE0990"},
        {STYLESHEET_WITH("<xsl:template match='note'><xsl:number level='any' format='(a)'/>"
                         "</xsl:template>",
                         "<xsl:apply-templates select='//note'/>"),
         "(a) (b) (c) (d) (e)"},
    };
    check_cases(state, (case_list){.cases = cases, .case_count = SXML_TEST_COUNT(cases)});
}

/* ------------------------------------------------- xsl:analyze-string */

/* 17.1, 17.2. */
static void test_analyze_string(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<s><xsl:analyze-string select='\"energex\"' regex='e'>"
                    "<xsl:matching-substring>[<xsl:value-of select='.'/>]</xsl:matching-substring>"
                    "<xsl:non-matching-substring><xsl:value-of select='.'/>"
                    "</xsl:non-matching-substring></xsl:analyze-string></s>"),
         "<s>[e]n[e]rg[e]x</s>"},
        {STYLESHEET("<xsl:analyze-string select='\"a1b22\"' regex='[0-9]+'>"
                    "<xsl:matching-substring select='position() || \"/\" || last() || \":\" || .'/>"
                    "<xsl:non-matching-substring select='\"(\" || . || \")\"'/>"
                    "</xsl:analyze-string>"),
         "\"(a)\" \"2/4:1\" \"(b)\" \"4/4:22\""},
        {STYLESHEET("<xsl:analyze-string select='\"23 March 2002\"' "
                    "regex='([0-9]+)\\s+([A-Z][a-z]+)\\s+(?&lt;year&gt;[0-9]+)'>"
                    "<xsl:matching-substring><xsl:sequence select='regex-group(3), regex-group(1), "
                    "regex-group(\"year\"), regex-group(0), regex-group(9), "
                    "regex-groups()?year?value'/></xsl:matching-substring>"
                    "</xsl:analyze-string>"),
         "\"2002\" \"23\" \"2002\" \"23 March 2002\" \"\" \"2002\""},
        /* No captured groups outside xsl:matching-substring. */
        {STYLESHEET("<xsl:analyze-string select='\"ab\"' regex='(a)'>"
                    "<xsl:non-matching-substring select='\"[\" || regex-group(1) || \"]\", "
                    "exists(regex-groups()?*)'/></xsl:analyze-string>"
                    "<xsl:sequence select='regex-group(1) = \"\"'/>"),
         "\"[]\" false() true()"},
        {STYLESHEET("<xsl:analyze-string select='\"Hello\"' regex='L+' flags='i'>"
                    "<xsl:matching-substring><m><xsl:value-of select='.'/></m>"
                    "</xsl:matching-substring></xsl:analyze-string>"),
         "<m>ll</m>"},
        {STYLESHEET("<xsl:analyze-string select='()' regex='a'>"
                    "<xsl:non-matching-substring>x</xsl:non-matching-substring>"
                    "</xsl:analyze-string>|"),
         "|"},
        {STYLESHEET("<xsl:analyze-string select='22' regex='2'><xsl:matching-substring/>"
                    "</xsl:analyze-string>"),
         "error:XPTY0004"},
        {STYLESHEET("<xsl:analyze-string select='\"a\"' regex='('><xsl:matching-substring/>"
                    "</xsl:analyze-string>"),
         "error:XTDE1140"},
        {STYLESHEET("<xsl:analyze-string select='\"a\"' regex='a' flags='z'>"
                    "<xsl:matching-substring/></xsl:analyze-string>"),
         "error:XTDE1145"},
        {STYLESHEET("<xsl:analyze-string select='\"a\"' regex='a'/>"), "error:XTSE1130"},
        {STYLESHEET("<xsl:analyze-string select='\"a\"' regex='a'>"
                    "<xsl:matching-substring select='1'>x</xsl:matching-substring>"
                    "</xsl:analyze-string>"),
         "error:XTSE3185"},
    };
    check_cases(state, (case_list){.cases = cases, .case_count = SXML_TEST_COUNT(cases)});
}

static const sxml_test_case xslt_number_test_cases[] = {
    {.name = "format_strings", .function = test_format_strings},
    {.name = "number_value", .function = test_number_value},
    {.name = "number_position", .function = test_number_position},
    {.name = "analyze_string", .function = test_analyze_string},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_number",
            .cases = xslt_number_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_number_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
