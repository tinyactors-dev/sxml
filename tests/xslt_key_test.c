/*
 * Tests for keys and the XSLT functions (src/xslt_key.c,
 * src/function_library_xslt.c; D149, packet 4.10): xsl:key and fn:key
 * (several declarations, composite keys, collations, numbers, a top node,
 * keys in patterns, errors), fn:current in expressions and patterns,
 * fn:copy-of, fn:system-property, fn:unparsed-entity-uri, and patterns
 * whose predicates are tested without selecting the step again.
 *
 * Golden file: tests/evaluator_template_test.c (read, lower, evaluate,
 * write the value by the adaptive method with items separated by a
 * space, "error:CODE" for an error).
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
    "<doc><item n='1'>one</item><item n='2'>two<b>bold</b></item><!--c--><?p q?></doc>";

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

static const char key_document[] =
    "<doc><city name='Pittsfield' state='VT' n='1'/><city name='Bristol' state='NH' n='2.0'/>"
    "<city name='Manchester' state='VT' n='3'/><city name='Concord' state='nh' n='4'/>"
    "<region><city name='Hanover' state='NH' n='5'/></region></doc>";

static bool check_key_cases(sxml_test_state *state, case_list list) {
    bool all_match = true;
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        transform_outcome outcome;
        run_transform((transform_request){.stylesheet = list.cases[case_index].stylesheet,
                                          .source = key_document,
                                          .source_byte_count = sizeof(key_document) - 1,
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

#define CHECK_KEYS(state, cases)                                                                   \
    check_key_cases((state), (case_list){.cases = (cases), .case_count = SXML_TEST_COUNT(cases)})

/* 20.2: xsl:key and fn:key. */
static void test_keys(sxml_test_state *state) {
    static const transform_case cases[] = {
        /* Nodes in document order, without duplicates. */
        {STYLESHEET("<xsl:key name='by-state' match='city' use='@state'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='key(\"by-state\", (\"VT\", \"NH\", \"VT\"))/@name/string()'/>"
                    "</xsl:template>"),
         "\"Pittsfield\" \"Bristol\" \"Manchester\" \"Hanover\""},
        /* Two declarations of one name; a key used in a predicate. */
        {STYLESHEET("<xsl:key name='k' match='city[@state=\"VT\"]' use='@name'/>"
                    "<xsl:key name='k' match='region/city' use='@name'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='count(key(\"k\", (\"Hanover\", \"Manchester\", "
                    "\"Bristol\"))), //city[key(\"k\", @name)]/@n/string()'/>"
                    "</xsl:template>"),
         "2 \"1\" \"3\" \"5\""},
        /* Numbers are compared as eq does: 2 eq 2.0e0. */
        {STYLESHEET("<xsl:key name='n' match='city' use='number(@n)'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='key(\"n\", (2, 3.0))/@name/string()'/>"
                    "</xsl:template>"),
         "\"Bristol\" \"Manchester\""},
        /* A composite key (20.2.1). */
        {STYLESHEET("<xsl:key name='c' match='city' use='@state, @n' composite='yes'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='key(\"c\", (\"VT\", \"3\"))/@name/string(), "
                    "count(key(\"c\", \"VT\"))'/>"
                    "</xsl:template>"),
         "\"Manchester\" 0"},
        /* A collation. */
        {STYLESHEET("<xsl:key name='s' match='city' use='@state' "
                    "collation='http://www.w3.org/2005/xpath-functions/collation/"
                    "html-ascii-case-insensitive'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='key(\"s\", \"nh\")/@name/string()'/>"
                    "</xsl:template>"),
         "\"Bristol\" \"Concord\" \"Hanover\""},
        /* fn:key#3: only the descendants of $top. */
        {STYLESHEET("<xsl:key name='by-state' match='city' use='@state'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='key(\"by-state\", \"NH\", //region)/@name/string()'/>"
                    "</xsl:template>"),
         "\"Hanover\""},
        /* The use value of a sequence constructor; a key in a pattern. */
        {STYLESHEET("<xsl:key name='u' match='city'><xsl:sequence select='lower-case(@state)'/>"
                    "</xsl:key>"
                    "<xsl:template match='/'><xsl:apply-templates select='//city'/></xsl:template>"
                    "<xsl:template match='key(\"u\", \"nh\")'>"
                    "<xsl:sequence select='string(@n)'/></xsl:template>"
                    "<xsl:template match='city'/>"),
         "\"2.0\" \"4\" \"5\""},
        /* A pattern that matches namespace nodes (6.3): each element has
           the xml namespace node. */
        {STYLESHEET("<xsl:key name='ns' match='namespace-node()' use='name()'/>"
                    "<xsl:template match='/'>"
                    "<xsl:sequence select='count(key(\"ns\", \"xml\")) eq count(//*), "
                    "key(\"ns\", \"xml\")[1]/../name()'/>"
                    "</xsl:template>"),
         "true() \"doc\""},
        /* Declarations of one name that differ in collation or composite. */
        {STYLESHEET("<xsl:key name='d' match='city' use='@n'/>"
                    "<xsl:key name='d' match='region' use='@n' "
                    "collation='http://www.w3.org/2005/xpath-functions/collation/"
                    "html-ascii-case-insensitive'/>"
                    "<xsl:template match='/'><xsl:sequence select='key(\"d\", 1)'/>"
                    "</xsl:template>"),
         "error:XTSE1220"},
        {STYLESHEET("<xsl:key name='d' match='city' use='@n'/>"
                    "<xsl:key name='d' match='region' use='@n' composite='yes'/>"
                    "<xsl:template match='/'><xsl:sequence select='key(\"d\", 1)'/>"
                    "</xsl:template>"),
         "error:XTSE1222"},
        {STYLESHEET("<xsl:key name='u' match='city' use='@n' collation='http://unknown/'/>"
                    "<xsl:template match='/'><xsl:sequence select='1'/></xsl:template>"),
         "error:XTSE1210"},
        /* Errors: an undeclared key name; use and content. */
        {STYLESHEET("<xsl:template match='/'><xsl:sequence select='key(\"none\", 1)'/>"
                    "</xsl:template>"),
         "error:XTDE1260"},
        {STYLESHEET("<xsl:key name='b' match='city' use='@n'><xsl:sequence select='1'/></xsl:key>"
                    "<xsl:template match='/'><xsl:sequence select='key(\"b\", 1)'/>"
                    "</xsl:template>"),
         "error:XTSE1205"},
    };
    CHECK_KEYS(state, cases);
}

/* 20.3.1, 20.6.1, 24.2.1, 20.2.3. */
static void test_functions(sxml_test_state *state) {
    static const transform_case cases[] = {
        /* current() is the item the outermost expression started with. */
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item'>"
                    "<xsl:sequence select='count(//item[@n le current()/@n])'/></xsl:template>"),
         "1 2"},
        /* In a pattern, the item being matched (D118). */
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item[current()/@n = 2]'>two</xsl:template>"
                    "<xsl:template match='item'>other</xsl:template>"),
         "other two"},
        /* A deep copy has no parent. */
        {STYLESHEET("<xsl:template match='/'>"
                    "<xsl:sequence select='count(copy-of(//b)/..), copy-of(//item[1])/string(), "
                    "copy-of(3)'/></xsl:template>"),
         "0 \"one\" 3"},
        /* A snapshot keeps copies of the ancestors (20.6.2). */
        {STYLESHEET("<xsl:template match='/'>"
                    "<xsl:sequence select='snapshot(//b)/ancestor::*/name(), "
                    "count(snapshot(//b)/../../*), snapshot(//item[1]/@n)/../name(), "
                    "snapshot(//b) is //b'/></xsl:template>"),
         "\"doc\" \"item\" 1 \"item\" false()"},
        /* A namespace node's snapshot is its new parent's (20.6.2). */
        {STYLESHEET("<xsl:template match='/'>"
                    "<xsl:sequence select='snapshot(/doc/namespace::xml)/../name(), "
                    "snapshot(/doc/namespace::xml) instance of namespace-node()'/>"
                    "</xsl:template>"),
         "\"doc\" true()"},
        {STYLESHEET("<xsl:template match='/'>"
                    "<xsl:sequence select='system-property(\"xsl:version\"), "
                    "system-property(\"xsl:vendor\"), system-property(\"none\")'/>"
                    "</xsl:template>"),
         "\"4.0\" \"sxml\" \"\""},
        {STYLESHEET("<xsl:template match='/'>"
                    "<xsl:sequence select='string(unparsed-entity-uri(\"e\")), "
                    "unparsed-entity-public-id(\"e\")'/></xsl:template>"),
         "\"\" \"\""},
    };
    CHECK(state, cases);
}

/* 6.3.2.3: predicates that never count positions are tested on the item;
   positional ones still count among the nodes selected. */
static void test_pattern_predicates(sxml_test_state *state) {
    static const transform_case cases[] = {
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item[@n = \"2\"]'>A</xsl:template>"
                    "<xsl:template match='doc/item[b]'>B</xsl:template>"
                    "<xsl:template match='item[1]'>C</xsl:template>"),
         "C B"},
        {STYLESHEET("<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                    "<xsl:template match='item[position() = last()]'>last</xsl:template>"
                    "<xsl:template match='//item[not(b)]'>no-b</xsl:template>"),
         "no-b last"},
    };
    CHECK(state, cases);
}

/* Many rules with predicates over many siblings: linear, not quadratic,
   in the number of siblings (D149; packet 4.8's match-242). */
static void test_many_siblings(sxml_test_state *state) {
    static char source[2000 * 24 + 64];
    size_t length = 0;
    length += (size_t)snprintf(source + length, sizeof(source) - length, "<doc>");
    for (size_t item_index = 0; item_index < 2000; item_index += 1) {
        length += (size_t)snprintf(source + length, sizeof(source) - length,
                                   "<item n='%zu'/>", item_index % 7);
    }
    length += (size_t)snprintf(source + length, sizeof(source) - length, "</doc>");
    transform_outcome outcome;
    run_transform(
        (transform_request){
            .stylesheet = STYLESHEET(
                "<xsl:template match='/'><xsl:apply-templates select='//item'/></xsl:template>"
                "<xsl:template match='item[@n = \"1\"]'/>"
                "<xsl:template match='item[@n = \"2\"]'/>"
                "<xsl:template match='item[@n = \"3\"]'/>"
                "<xsl:template match='item[@n = \"4\"]'/>"
                "<xsl:template match='item[@n = \"5\"]'/>"
                "<xsl:template match='item[@n = \"6\"]'>6</xsl:template>"
                "<xsl:template match='item'/>"),
            .source = source,
            .source_byte_count = length,
            .applies_templates = false},
        &outcome);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strlen(outcome.text) > 0);
}

static const sxml_test_case xslt_key_test_cases[] = {
    {.name = "keys", .function = test_keys},
    {.name = "functions", .function = test_functions},
    {.name = "pattern_predicates", .function = test_pattern_predicates},
    {.name = "many_siblings", .function = test_many_siblings},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_key",
            .cases = xslt_key_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_key_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
