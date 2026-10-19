/*
 * Tests for Invisible XML (src/ixml_internal.h; packet 7.18, D215):
 * fn:invisible-xml through a processor with and without the feature (F&O
 * 4.0, 17.4), grammars in ixml notation and in XML form, the static
 * errors of the Invisible XML Specification, marks, renaming and
 * insertions ("Constructing XML"), ambiguity and failure ("Parsing"), the
 * dynamic errors D02 to D07, long inputs (no recursion, D71), and an
 * allocation-failure loop (D23). Every case checks for leaks.
 *
 * Golden file: tests/html_test.c.
 */
#include "ixml_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static sxml_counting_allocator *make_counting_allocator(uint64_t failing_number) {
    sxml_counting_allocator *counting_allocator = NULL;
    if (sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                     .failing_allocation_number = failing_number},
            &counting_allocator) != SXML_STATUS_OK) {
        return NULL;
    }
    return counting_allocator;
}

static uint64_t live_allocations(sxml_counting_allocator *counting_allocator) {
    return sxml_counting_allocator_read_counts(counting_allocator).live_allocation_count;
}

/* A processor with or without the feature, and an output buffer. */
typedef struct ixml_fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    sxml_error_record error_record;
} ixml_fixture;

typedef struct ixml_fixture_options {
    bool has_feature;
    uint64_t failing_number;
} ixml_fixture_options;

static sxml_status ixml_fixture_create(ixml_fixture *fixture, ixml_fixture_options options) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->counting_allocator = make_counting_allocator(options.failing_number);
    if (fixture->counting_allocator == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(fixture->counting_allocator);
    const sxml_optional_feature feature = sxml_invisible_xml_feature();
    sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .optional_features = {.optional_features = &feature,
                                  .optional_feature_count = options.has_feature ? 1U : 0U}},
        &fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &fixture->output_buffer);
    }
    return status;
}

/* Destroys the fixture; true when nothing leaked. */
static bool ixml_fixture_destroy(ixml_fixture *fixture) {
    bool has_no_leak = true;
    if (fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(fixture->processor);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(fixture->processor);
    sxml_output_buffer_destroy(fixture->output_buffer);
    if (fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && live_allocations(fixture->counting_allocator) == 0;
        sxml_counting_allocator_destroy(fixture->counting_allocator);
    }
    return has_no_leak;
}

/* Evaluates expression_text and writes its value by the adaptive method
   into the fixture's output buffer. */
static sxml_status run_expression(ixml_fixture *fixture, sxml_byte_view expression_text) {
    memset(&fixture->error_record, 0, sizeof(fixture->error_record));
    sxml_output_buffer_clear(fixture->output_buffer);
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(fixture->counting_allocator);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        allocator,
        &(sxml_xpath_expression_configuration){
            .processor = fixture->processor,
            .expression_text = expression_text,
            .source_name = TEXT("test.xpath"),
            .static_context = {.static_base_uri = TEXT("http://example.com/")},
            .error_record = &fixture->error_record},
        &expression);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression, &(sxml_xpath_evaluation_request){.error_record = &fixture->error_record},
            &result);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(fixture->processor),
            &(sxml_serialization_request){
                .value = result,
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                             .has_item_separator = true,
                                             .item_separator = TEXT(" ")},
                .error_record = &fixture->error_record},
            sxml_output_buffer_as_output_stream(fixture->output_buffer));
    }
    sxml_sequence_release(result);
    sxml_xpath_expression_destroy(expression);
    return status;
}

/* An expression and its adaptive output, or the error code it raises. */
typedef struct expression_case {
    sxml_byte_view expression_text;
    sxml_byte_view output;
    sxml_byte_view error_code;
} expression_case;

#define EXPRESSION_VALUE(text, output_text)                                                       \
    {.expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                         \
     .output = SXML_BYTE_VIEW_INITIALIZER(output_text),                                           \
     .error_code = {.bytes = NULL, .byte_count = 0}}

#define EXPRESSION_ERROR(text, code)                                                              \
    {.expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                         \
     .output = {.bytes = NULL, .byte_count = 0},                                                  \
     .error_code = SXML_BYTE_VIEW_INITIALIZER(code)}

static const expression_case expression_cases[] = {
    /* The Specification, "How it works" (the date grammar of F&O 4.0's
       example). */
    EXPRESSION_VALUE("invisible-xml(\"date: year, -'-', month, -'-', day. year: d, d, d, d. "
                     "month: '0', d; '1', ['0'; '1'; '2']. day: ['0'-'2'], d; '3', ['0'; '1']. "
                     "-d: ['0'-'9'].\")('2023-10-31')",
                     "<date><year>2023</year><month>10</month><day>31</day></date>"),
    /* No grammar: the grammar of grammars, whose result is the XML
       form. */
    EXPRESSION_VALUE("invisible-xml()(\"{c} s: ^a, -[L; 'x'-'z'], +#41. a>b = ~[#a]?.\")",
                     "<ixml><comment>c</comment><rule name=\"s\"><alt><nonterminal mark=\"^\" "
                     "name=\"a\"/><inclusion tmark=\"-\"><member code=\"L\"/><member "
                     "from=\"x\" to=\"z\"/></inclusion><insertion hex=\"41\"/></alt></rule>"
                     "<rule name=\"a\" alias=\"b\"><alt><option><exclusion><member hex=\"a\"/>"
                     "</exclusion></option></alt></rule></ixml>"),
    /* The same grammar in XML form parses the same. */
    EXPRESSION_VALUE("let $grammar := \"list: item++-',', @size. item: ['a'-'z']+. @size: "
                     "+'n'.\" return deep-equal(invisible-xml($grammar)('ab,c'), "
                     "invisible-xml(invisible-xml()($grammar)/*)('ab,c'))",
                     "true()"),
    /* "Constructing XML": the Specification's example of marks and
       renaming. */
    EXPRESSION_VALUE("invisible-xml(\"expr: open, -arith, @close, -';'. @open: '('. close: ')'. "
                     "arith: left, op, ^right>second. left>first: operand. -right: operand. "
                     "-operand: name; -number. @name: ['a'-'z']. @number: ['0'-'9']. -op: sign. "
                     "@sign>operator: '+'; '-'.\")('(a+1);')",
                     "<expr open=\"(\" operator=\"+\" close=\")\"><first name=\"a\"/>"
                     "<second>1</second></expr>"),
    /* "Insertions". */
    EXPRESSION_VALUE("invisible-xml(\"data: value++-',', @source. source: +'ixml'. value: pos; "
                     "neg. -pos: +'+', digit+. -neg: +'-', -'(', digit+, -')'. -digit: "
                     "['0'-'9'].\")('100,(300)')",
                     "<data source=\"ixml\"><value>+100</value><value>-300</value></data>"),
    /* "Parsing": ambiguity, and a nullable nonterminal used twice. */
    EXPRESSION_VALUE("invisible-xml(\"expr: e. e: e, '+', e; 'i'.\")('i+i+i')/*/@*:state/string()",
                     "\"ambiguous\""),
    EXPRESSION_VALUE("invisible-xml(\"s: a, a. a: 'x'?.\")('x')/*/@*:state/string()",
                     "\"ambiguous\""),
    EXPRESSION_VALUE("invisible-xml(\"s: 'a'*; 'b'+.\")('aaa')", "<s>aaa</s>"),
    EXPRESSION_VALUE("invisible-xml(\"s: a**',', '.'. a: 'x'.\")('.')", "<s>.</s>"),
    EXPRESSION_VALUE("invisible-xml(\"s: a**',', '.'. a: 'x'.\")('x,x.')",
                     "<s><a>x</a>,<a>x</a>.</s>"),
    /* A failure says where. */
    EXPRESSION_VALUE("invisible-xml(\"s: 'a', ['0'-'9'].\")('a\nb')!(*/@*:state/string(), "
                     "*/line/string(), */column/string(), */expected/string())",
                     "\"failed\" \"1\" \"2\" \"[\"\"0\"\"-\"\"9\"\"]\""),
    EXPRESSION_ERROR("invisible-xml(\"s: 'a'.\", {'fail-on-error': true()})('b')", "FOIX0002"),
    /* Line ends are normalized (the Specification, "Parsing"). */
    EXPRESSION_VALUE("invisible-xml(\"s: 'a', #a, 'b', #a, 'c'.\")('a' || "
                     "codepoints-to-string((13, 10)) || 'b' || codepoints-to-string(13) || 'c')",
                     "<s>a\nb\nc</s>"),
    /* "Prolog": an unknown version is used as 1.1. */
    EXPRESSION_VALUE("invisible-xml(\"ixml version '9'. s: 'a'.\")('a')/*/@*!string()",
                     "\"version-mismatch\" \"1.1\""),
    EXPRESSION_VALUE("count(invisible-xml(\"ixml version '1.0'. s: 'a'.\")('a')/*/@*)", "0"),
    /* Static errors (FOIX0001). */
    EXPRESSION_ERROR("invisible-xml('s: t.')", "FOIX0001"),
    EXPRESSION_ERROR("invisible-xml(\"s: 'a'. s: 'b'.\")", "FOIX0001"),
    EXPRESSION_ERROR("invisible-xml(\"s: #110000.\")", "FOIX0001"),
    EXPRESSION_ERROR("invisible-xml(\"s: #fffe.\")", "FOIX0001"),
    EXPRESSION_ERROR("invisible-xml(\"s: ['z'-'a'].\")", "FOIX0001"),
    EXPRESSION_ERROR("invisible-xml(\"s: [Xx].\")", "FOIX0001"),
    EXPRESSION_ERROR("invisible-xml('s = (')", "FOIX0001"),
    EXPRESSION_ERROR("invisible-xml(parse-xml('<ixml><rule name=\"s\"/></ixml>')/*)", "FOIX0001"),
    EXPRESSION_ERROR("invisible-xml(parse-xml('<ixml><rule name=\"s\"><alt><bad/></alt></rule>"
                     "</ixml>')/*)",
                     "FOIX0001"),
    /* Elements in a namespace are not part of the XML form. */
    EXPRESSION_VALUE("invisible-xml(parse-xml('<ixml><x:y xmlns:x=\"x\"/><rule name=\"s\">"
                     "<alt><literal string=\"a\"/></alt></rule></ixml>')/*)('a')",
                     "<s>a</s>"),
    /* Dynamic errors (FOIX0002). */
    EXPRESSION_ERROR("invisible-xml(\"s: @a, @a. @a: 'x'.\")('xx')", "FOIX0002"),
    EXPRESSION_ERROR("invisible-xml(parse-xml('<ixml><rule name=\"1a\"><alt><literal "
                     "string=\"x\"/></alt></rule></ixml>')/*)('x')",
                     "FOIX0002"),
    EXPRESSION_ERROR("invisible-xml(\"s: +#1.\")('')", "FOIX0002"),
    EXPRESSION_ERROR("invisible-xml(\"@s: 'x'.\")('x')", "FOIX0002"),
    EXPRESSION_ERROR("invisible-xml(\"-s: 'x'.\")('x')", "FOIX0002"),
    EXPRESSION_ERROR("invisible-xml(\"-s: a, a. a: 'x'.\")('xx')", "FOIX0002"),
    EXPRESSION_ERROR("invisible-xml(\"s: @xmlns. @xmlns: 'x'.\")('x')", "FOIX0002"),
    EXPRESSION_VALUE("invisible-xml(\"-s: -' ', a, -' '. a: 'x'.\")(' x ')", "<a>x</a>"),
    /* Long inputs: left and right recursion, no recursion in C. */
    EXPRESSION_VALUE("string-length(invisible-xml(\"s: 'a'*.\")(string-join((1 to 100000)!'a')))",
                     "100000"),
    EXPRESSION_VALUE("string-length(invisible-xml(\"s: r. -r: 'a', r; .\")(string-join((1 to "
                     "300)!'a')))",
                     "300"),
    /* The options (F&O 4.0, 1.7). */
    EXPRESSION_ERROR("invisible-xml(\"s: 'a'.\", {'fail-on-error': 'maybe'})", "XPTY0004"),
    EXPRESSION_VALUE("system-properties()?(#supports-invisible-xml)", "true()"),
};

static void test_function_parses(sxml_test_state *state) {
    ixml_fixture fixture;
    SXML_TEST_REQUIRE(state, ixml_fixture_create(&fixture, (ixml_fixture_options){
                                                               .has_feature = true,
                                                               .failing_number = 0}) ==
                                 SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(expression_cases); case_index += 1) {
        const expression_case *expression = &expression_cases[case_index];
        const sxml_status status = run_expression(&fixture, expression->expression_text);
        if (expression->error_code.byte_count == 0) {
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
            const bool is_equal = sxml_byte_view_equals(
                sxml_output_buffer_get_bytes(fixture.output_buffer), expression->output);
            SXML_TEST_EXPECT(state, is_equal);
        } else {
            SXML_TEST_EXPECT(state, status != SXML_STATUS_OK);
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(fixture.error_record.code.local_name,
                                                          expression->error_code));
        }
    }
    SXML_TEST_EXPECT(state, ixml_fixture_destroy(&fixture));
}

/* Without the feature the function is absent (D191). */
static void test_function_is_absent_without_the_feature(sxml_test_state *state) {
    ixml_fixture fixture;
    SXML_TEST_REQUIRE(state, ixml_fixture_create(&fixture, (ixml_fixture_options){
                                                               .has_feature = false,
                                                               .failing_number = 0}) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run_expression(&fixture, TEXT("invisible-xml()")) != SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(fixture.error_record.code.local_name, TEXT("XPST0017")));
    SXML_TEST_EXPECT(state, run_expression(&fixture,
                                           TEXT("system-properties()?(#supports-invisible-xml)")) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_output_buffer_get_bytes(fixture.output_buffer), TEXT("false()")));
    SXML_TEST_EXPECT(state, ixml_fixture_destroy(&fixture));
}

/* The table of the grammar of grammars compiles, and its parse of the
   grammar text it came from gives its own entries back. */
static void test_grammar_of_grammars_compiles(sxml_test_state *state) {
    sxml_counting_allocator *counting_allocator = make_counting_allocator(0);
    SXML_TEST_REQUIRE(state, counting_allocator != NULL);
    const sxml_allocator allocator = sxml_counting_allocator_as_allocator(counting_allocator);
    sxml_ixml_grammar_builder builder;
    sxml_ixml_grammar_builder_initialize(
        &builder,
        &(sxml_ixml_grammar_builder_configuration){.allocator = allocator, .name_pool = NULL});
    SXML_TEST_EXPECT(state, sxml_ixml_grammar_builder_read_entries(
                                &builder, sxml_ixml_grammar_of_grammars) == SXML_STATUS_OK);
    sxml_ixml_grammar grammar;
    sxml_ixml_grammar_outcome outcome = {.error = SXML_IXML_GRAMMAR_ERROR_FORM};
    SXML_TEST_EXPECT(state, sxml_ixml_grammar_builder_finish(&builder, &grammar, &outcome) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, outcome.error == SXML_IXML_GRAMMAR_ERROR_NONE);
    sxml_ixml_grammar_builder_finalize(&builder);
    /* "ixml", the first rule, is the root. */
    const sxml_ixml_nonterminal *root = sxml_xml_array_at(&grammar.nonterminals, 0);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_ixml_grammar_text(&grammar, root->name),
                                                  TEXT("ixml")));
    static const uint32_t input[] = {'s', ':', ' ', '\'', 'a', '\'', '.'};
    sxml_ixml_parse_result result = sxml_ixml_parse_result_empty(allocator);
    SXML_TEST_EXPECT(state, sxml_ixml_parse(&(sxml_ixml_parse_request){
                                                .grammar = &grammar,
                                                .input = {.codepoints = input,
                                                          .codepoint_count = SXML_TEST_COUNT(input)},
                                                .step_meter = {.state = NULL, .operations = NULL}},
                                            &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, result.is_sentence);
    SXML_TEST_EXPECT(state, !result.is_ambiguous);
    sxml_ixml_parse_result_finalize(&result);
    result = sxml_ixml_parse_result_empty(allocator);
    SXML_TEST_EXPECT(state, sxml_ixml_parse(&(sxml_ixml_parse_request){
                                                .grammar = &grammar,
                                                .input = {.codepoints = input,
                                                          .codepoint_count = 3},
                                                .step_meter = {.state = NULL, .operations = NULL}},
                                            &result) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !result.is_sentence);
    SXML_TEST_EXPECT(state, result.failure_position == 3);
    SXML_TEST_EXPECT(state, result.expected_terminals.element_count > 0);
    sxml_ixml_parse_result_finalize(&result);
    sxml_ixml_grammar_finalize(&grammar);
    SXML_TEST_EXPECT(state, live_allocations(counting_allocator) == 0);
    sxml_counting_allocator_destroy(counting_allocator);
}

static void test_function_survives_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    for (uint64_t failing_number = 1; failing_number < 100000 && status != SXML_STATUS_OK;
         failing_number += 1) {
        ixml_fixture fixture;
        status = ixml_fixture_create(&fixture, (ixml_fixture_options){
                                                   .has_feature = true,
                                                   .failing_number = failing_number});
        if (status == SXML_STATUS_OK) {
            status = run_expression(
                &fixture, TEXT("invisible-xml(\"s: a++',', @n. a: ['a'-'z']+. @n: +'1'.\")('ab,c'),"
                               " invisible-xml(\"s: 'a'.\")('b')/*/@*:state/string()"));
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                        sxml_output_buffer_get_bytes(fixture.output_buffer),
                                        TEXT("<s n=\"1\"><a>ab</a>,<a>c</a></s> \"failed\"")));
        }
        SXML_TEST_EXPECT(state, ixml_fixture_destroy(&fixture));
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
}

static const sxml_test_case ixml_test_cases[] = {
    {.name = "function_parses", .function = test_function_parses},
    {.name = "function_is_absent_without_the_feature",
     .function = test_function_is_absent_without_the_feature},
    {.name = "grammar_of_grammars_compiles", .function = test_grammar_of_grammars_compiles},
    {.name = "function_survives_every_allocation_failure",
     .function = test_function_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "ixml",
            .cases = ixml_test_cases,
            .case_count = SXML_TEST_COUNT(ixml_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
