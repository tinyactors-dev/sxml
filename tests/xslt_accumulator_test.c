/*
 * Tests for accumulators (src/xslt_accumulator.c, the rows of
 * src/function_library_xslt.c; D155, packet 4.14): pre- and post-descent
 * values with start and end rules, the last matching rule, the as type,
 * accumulators applicable by use-accumulators of the initial mode and on
 * other trees, names as strings and xs:QName, capture, the dynamic and
 * static errors (XTDE3340, XTDE3362, XTDE3400, XTTE3360, XTSE3300,
 * XTSE3350, XTSE3355, errors of the initial value raised again), and deep
 * and wide trees (an explicit stack, D71).
 *
 * Golden file: tests/xslt_key_test.c (read, lower, evaluate, write the
 * value by the adaptive method with items separated by a space,
 * "error:CODE" for an error).
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

/* A stylesheet of declarations. */
#define STYLESHEET(declarations)                                                                   \
    "<xsl:stylesheet version='4.0' " XSL_NAMESPACE ">" declarations "</xsl:stylesheet>"

static const char source_document[] =
    "<doc><chap><fig/><fig><fig/></fig></chap><chap><fig/>text</chap></doc>";

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

/* One transformation to run. */
typedef struct transform_request {
    const char *stylesheet;
    /* NULL for source_document. */
    const char *source;
    size_t source_byte_count;
    /* Applies templates even when there is an xsl:initial-template. */
    bool applies_templates;
    /* No global context item: the focus of the initial template is
       absent. */
    bool has_no_context_item;
} transform_request;

/* A unit to evaluate for a request. */
typedef struct unit_evaluation {
    sxml_processor *processor;
    transform_request request;
    sxml_ir_unit *unit;
} unit_evaluation;

/* Evaluates unit with the source's document node as the context item. */
static sxml_status evaluate_unit(unit_evaluation evaluation, transform_outcome *outcome) {
    sxml_processor *processor = evaluation.processor;
    const transform_request request = evaluation.request;
    sxml_ir_unit *unit = evaluation.unit;
    const sxml_byte_view source =
        request.source == NULL
            ? sxml_byte_view_from_c_string(source_document)
            : (sxml_byte_view){.bytes = (const uint8_t *)request.source,
                               .byte_count = request.source_byte_count};
    sxml_item document = {.representation = 0};
    sxml_status status = sxml_processor_parse_document(
        processor,
        &(sxml_xml_parse_request){.input = source,
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
                                       .has_context_value = !request.has_no_context_item,
                                       .context_value = request.has_no_context_item
                                                            ? sxml_sequence_empty()
                                                            : sxml_sequence_from_item(document),
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

/* Reads, lowers and evaluates the stylesheet; outcome->text is the value
   written, or "error:CODE" for an error with a code. */
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
                                          .applies_templates = request.applies_templates,
                                          .builds_result_document = false,
                                          .error_record = &outcome->error_record},
            &unit);
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = evaluate_unit(
            (unit_evaluation){.processor = processor, .request = request, .unit = unit}, outcome);
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

typedef struct case_list {
    const transform_case *cases;
    size_t case_count;
} case_list;

static bool check_cases(sxml_test_state *state, case_list list) {
    bool all_match = true;
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        transform_outcome outcome;
        run_transform((transform_request){.stylesheet = list.cases[case_index].stylesheet,
                                          .source = NULL,
                                          .source_byte_count = 0,
                                          .applies_templates = false},
                      &outcome);
        const bool matches = strcmp(outcome.text, list.cases[case_index].expected_text) == 0;
        if (!matches) {
            printf("case %zu: expected %s, got %s\n", case_index,
                   list.cases[case_index].expected_text, outcome.text);
        }
        all_match = all_match && matches;
    }
    SXML_TEST_EXPECT(state, all_match);
    return all_match;
}

#define CHECK(state, cases)                                                                        \
    check_cases((state), (case_list){.cases = (cases), .case_count = SXML_TEST_COUNT(cases)})


/* A mode that makes every accumulator applicable to the source. */
#define ALL_MODE "<xsl:mode use-accumulators='#all'/>"

/* 19.3, 19.4: start and end events in document order. */
static void test_values(sxml_test_state *state) {
    static const transform_case cases[] = {
        /* Numbering figures; a later chap resets it. */
        {STYLESHEET("<xsl:accumulator name='n' initial-value='0' as='xs:integer'>"
                    "<xsl:accumulator-rule match='chap' select='0'/>"
                    "<xsl:accumulator-rule match='fig' select='$value + 1'/>"
                    "</xsl:accumulator>" ALL_MODE
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='//fig/accumulator-before(\"n\")'/></xsl:template>"),
         "1 2 3 1"},
        /* Post-descent values: after the descendants' events. */
        {STYLESHEET("<xsl:accumulator name='n' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='$value + 1'/>"
                    "</xsl:accumulator>" ALL_MODE
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='//chap/accumulator-after(\"n\"), "
                    "accumulator-before(\"n\"), accumulator-after(\"n\"), "
                    "//text()/accumulator-before(\"n\")'/></xsl:template>"),
         "3 4 0 4 4"},
        /* End rules, a trace of the events; the last rule of a phase wins. */
        {STYLESHEET("<xsl:accumulator name='t' initial-value='\"\"'>"
                    "<xsl:accumulator-rule match='*' select='$value || \"(\" || name()'/>"
                    "<xsl:accumulator-rule match='chap' select='$value || \"[\"'/>"
                    "<xsl:accumulator-rule match='*' phase='end' select='$value || \")\"'/>"
                    "</xsl:accumulator>" ALL_MODE
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='accumulator-after(\"t\"), "
                    "(//fig)[2]/accumulator-after(\"t\")'/></xsl:template>"),
         "\"(doc[(fig)(fig(fig)))[(fig)))\" \"(doc[(fig)(fig(fig))\""},
        /* A sequence constructor; a QName as the name; a namespace. */
        {STYLESHEET("<xsl:accumulator name='p:c' initial-value='()' xmlns:p='urn:p'>"
                    "<xsl:accumulator-rule match='fig'>"
                    "<xsl:sequence select='$value, count(ancestor::*)'/>"
                    "</xsl:accumulator-rule></xsl:accumulator>" ALL_MODE
                    "<xsl:template match='/' xmlns:p='urn:p'>"
                    "<xsl:sequence select='accumulator-after(#p:c), "
                    "accumulator-before(\"Q{urn:p}c\")'/></xsl:template>"),
         "2 2 3 2"},
        /* Values of other trees: temporary trees, the source not restricted
           by the empty use-accumulators of an initial mode it is not in. */
        {STYLESHEET("<xsl:accumulator name='n' initial-value='0'>"
                    "<xsl:accumulator-rule match='x' select='$value + 1'/>"
                    "</xsl:accumulator>"
                    "<xsl:template match='/'>"
                    "<xsl:variable name='tree'><x/><x/><x/></xsl:variable>"
                    "<xsl:variable name='local'><a><x/></a></xsl:variable>"
                    "<xsl:sequence select='$tree/accumulator-after(\"n\"), "
                    "$local/a/accumulator-after(\"n\")'/>"
                    "</xsl:template>"),
         "3 1"},
        /* capture="yes": the rule sees a snapshot of the element. */
        {STYLESHEET("<xsl:accumulator name='c' initial-value='()'>"
                    "<xsl:accumulator-rule match='chap' phase='end' capture='yes' select='.'/>"
                    "</xsl:accumulator>" ALL_MODE
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='count(accumulator-after(\"c\")/fig), "
                    "accumulator-after(\"c\") is (//chap)[2]'/></xsl:template>"),
         "1 false()"},
        /* One accumulator reads another. */
        {STYLESHEET("<xsl:accumulator name='a' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='$value + 1'/></xsl:accumulator>"
                    "<xsl:accumulator name='b' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='$value + accumulator-before(\"a\")'/>"
                    "</xsl:accumulator>" ALL_MODE
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='accumulator-after(\"b\")'/></xsl:template>"),
         "10"},
    };
    CHECK(state, cases);
}

/* 19.2: use-accumulators of the initial mode. */
static void test_applicability(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:accumulator name='a' initial-value='1'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    "<xsl:accumulator name='b' initial-value='2'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    "<xsl:mode use-accumulators='b'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='accumulator-before(\"b\")'/></xsl:template>"),
         "2"},
        {STYLESHEET("<xsl:accumulator name='a' initial-value='1'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    "<xsl:accumulator name='b' initial-value='2'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    "<xsl:mode use-accumulators='b'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='accumulator-before(\"a\")'/></xsl:template>"),
         "error:XTDE3362"},
        /* Without an xsl:mode, no accumulator is applicable. */
        {STYLESHEET("<xsl:accumulator name='a' initial-value='1'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='accumulator-before(\"a\")'/></xsl:template>"),
         "error:XTDE3362"},
        {STYLESHEET("<xsl:accumulator name='a' initial-value='1'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    "<xsl:mode use-accumulators='a b'/>"
                    "<xsl:template match='/'/>"),
         "error:XTSE3300"},
    };
    CHECK(state, cases);
}

/* 19.1, 19.5, 19.6, 19.8, 19.9. */
static void test_errors(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:accumulator name='a' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    ALL_MODE "<xsl:template match='/'>"
                    "<xsl:sequence select='accumulator-before(\"b\")'/></xsl:template>"),
         "error:XTDE3340"},
        {STYLESHEET("<xsl:accumulator name='a' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    ALL_MODE "<xsl:template match='/'>"
                    "<xsl:sequence select='accumulator-before(\"a b\")'/></xsl:template>"),
         "error:XTDE3340"},
        {STYLESHEET("<xsl:accumulator name='a' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    ALL_MODE "<xsl:template match='/'>"
                    "<xsl:sequence select='(1 to 2) ! accumulator-before(\"a\")'/>"
                    "</xsl:template>"),
         "error:XTTE3360"},
        {STYLESHEET("<xsl:accumulator name='a' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    ALL_MODE "<xsl:template match='/'>"
                    "<xsl:sequence select='//@*/accumulator-before(\"a\"), "
                    "accumulator-before(\"a\")'/></xsl:template>"),
         "0"},
        /* A rule that reads its own accumulator. */
        {STYLESHEET("<xsl:accumulator name='a' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='$value + accumulator-before(\"a\")'/>"
                    "</xsl:accumulator>" ALL_MODE
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='accumulator-after(\"a\")'/></xsl:template>"),
         "error:XTDE3400"},
        /* The as type (XPTY0004). */
        {STYLESHEET("<xsl:accumulator name='a' initial-value='0' as='xs:integer'>"
                    "<xsl:accumulator-rule match='fig' select='\"x\"'/></xsl:accumulator>"
                    ALL_MODE "<xsl:template match='/'>"
                    "<xsl:sequence select='accumulator-after(\"a\")'/></xsl:template>"),
         "error:XPTY0004"},
        /* An error of the initial value is raised by every call. */
        {STYLESHEET("<xsl:accumulator name='a' initial-value='1 div 0'>"
                    "<xsl:accumulator-rule match='fig' select='$value'/></xsl:accumulator>"
                    ALL_MODE "<xsl:template match='/'>"
                    "<xsl:for-each select='//fig'><xsl:try>"
                    "<xsl:sequence select='accumulator-before(\"a\")'/>"
                    "<xsl:catch select='string($err:code)' "
                    "xmlns:err='http://www.w3.org/2005/xqt-errors'/></xsl:try></xsl:for-each>"
                    "</xsl:template>"),
         "\"err:FOAR0001\" \"err:FOAR0001\" \"err:FOAR0001\" \"err:FOAR0001\""},
        {STYLESHEET("<xsl:accumulator name='a' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='1'/></xsl:accumulator>"
                    "<xsl:accumulator name='a' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' select='2'/></xsl:accumulator>"
                    "<xsl:template match='/'/>"),
         "error:XTSE3350"},
        {STYLESHEET("<xsl:accumulator name='a' initial-value='0'>"
                    "<xsl:accumulator-rule match='fig' capture='yes' select='1'/>"
                    "</xsl:accumulator><xsl:template match='/'/>"),
         "error:XTSE3355"},
    };
    CHECK(state, cases);
}

/* A deep and a wide tree: the traversal keeps its stack in a sequence, not
   on the C stack (D71). */
#define LARGE_TREE_DEPTH 2000U
#define LARGE_TREE_WIDTH 20000U

static void test_large_trees(sxml_test_state *state) {
    static char source[LARGE_TREE_WIDTH * 4 + 64];
    const char *stylesheet = STYLESHEET(
        "<xsl:accumulator name='n' initial-value='0'>"
        "<xsl:accumulator-rule match='e' select='$value + 1'/>"
        "<xsl:accumulator-rule match='e' phase='end' select='$value + 10'/>"
        "</xsl:accumulator>" ALL_MODE
        "<xsl:template match='/'><xsl:sequence select='accumulator-after(\"n\")'/>"
        "</xsl:template>");
    size_t length = 0;
    for (size_t level = 0; level < LARGE_TREE_DEPTH; level += 1) {
        length += (size_t)snprintf(source + length, sizeof(source) - length, "<e>");
    }
    for (size_t level = 0; level < LARGE_TREE_DEPTH; level += 1) {
        length += (size_t)snprintf(source + length, sizeof(source) - length, "</e>");
    }
    transform_outcome outcome;
    run_transform((transform_request){.stylesheet = stylesheet,
                                      .source = source,
                                      .source_byte_count = length,
                                      .applies_templates = false},
                  &outcome);
    SXML_TEST_EXPECT(state, strcmp(outcome.text, "22000") == 0);
    length = (size_t)snprintf(source, sizeof(source), "<doc>");
    for (size_t item_index = 0; item_index < LARGE_TREE_WIDTH; item_index += 1) {
        length += (size_t)snprintf(source + length, sizeof(source) - length, "<e/>");
    }
    length += (size_t)snprintf(source + length, sizeof(source) - length, "</doc>");
    run_transform((transform_request){.stylesheet = stylesheet,
                                      .source = source,
                                      .source_byte_count = length,
                                      .applies_templates = false},
                  &outcome);
    SXML_TEST_EXPECT(state, strcmp(outcome.text, "220000") == 0);
}

static const sxml_test_case xslt_accumulator_test_cases[] = {
    {.name = "values", .function = test_values},
    {.name = "applicability", .function = test_applicability},
    {.name = "errors", .function = test_errors},
    {.name = "large_trees", .function = test_large_trees},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_accumulator",
            .cases = xslt_accumulator_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_accumulator_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
