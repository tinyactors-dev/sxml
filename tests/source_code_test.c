/*
 * Tests for source code as XML (src/source_code_internal.h; packets 7.22,
 * 7.23 and 7.26, D285, D288, D307): tree-sitter:parse and
 * tree-sitter:languages through a processor with and without the feature,
 * registered languages and their checks, locals and injections, the shape of the document (elements of named
 * nodes, fields, tokens as text or sxml:token, ERROR and missing nodes,
 * the string value is the source), highlights and their predicates,
 * positions in characters, deep nesting (no recursion, D71), and an
 * allocation-failure loop (D23). Every case checks for leaks.
 *
 * Golden file: tests/ixml_test.c.
 */
#include "source_code_internal.h"
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
typedef struct source_code_fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    sxml_error_record error_record;
} source_code_fixture;

typedef struct source_code_fixture_options {
    bool has_feature;
    uint64_t failing_number;
    /* Registered languages (D288); the empty view for none. */
    sxml_source_code_language_view languages;
} source_code_fixture_options;

static sxml_status source_code_fixture_create(source_code_fixture *fixture, source_code_fixture_options options) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->counting_allocator = make_counting_allocator(options.failing_number);
    if (fixture->counting_allocator == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    const sxml_allocator allocator =
        sxml_counting_allocator_as_allocator(fixture->counting_allocator);
    const sxml_optional_feature feature = sxml_source_code_feature();
    sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .optional_features = {.optional_features = &feature,
                                  .optional_feature_count = options.has_feature ? 1U : 0U},
            .source_code_languages = options.languages},
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
static bool source_code_fixture_destroy(source_code_fixture *fixture) {
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
static sxml_status run_expression(source_code_fixture *fixture, sxml_byte_view expression_text) {
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
    /* Named nodes are elements, tokens text; fields are attributes. */
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('[1, true]', 'json', {'highlights': false()})",
                     "<document xmlns:sxml=\"urn:sxml\"><array>[<number>1</number>, <true>true</true>]</array>"
                     "</document>"),
    /* Highlights: tokens with a capture become sxml:token. */
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('{\"a\": null}', 'json')", "<document xmlns:sxml=\"urn:sxml\"><object>{<pair><string field=\"key\" "
                     "highlight=\"string\">\"<string_content>a</string_content>\"</string>: <null "
                     "field=\"value\" highlight=\"constant.builtin\">null</null></pair>}</object>"
                     "</document>"),
    /* The string value is the source, whitespace and comments included. */
    EXPRESSION_VALUE("let $text := '  /* c */ function f(a) {\n  return a; } \n' return "
                     "string(Q{urn:sxml:tree-sitter}parse($text, 'javascript')) eq $text",
                     "true()"),
    /* A predicate (#match?): constructors start with a capital. */
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('new Thing(); thing();', 'javascript')"
                     "//identifier/@highlight/string()",
                     "\"constructor\" \"function\""),
    /* Positions count characters from 1. */
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('[\"\u00e9\", 1]', 'json', {'positions': true()})"
                     "//number/@*/string()",
                     "\"number\" \"1\" \"7\" \"1\" \"8\""),
    /* Errors and missing tokens are in the tree, and the text stays. */
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('[1, 2', 'json', {'highlights': false()})",
                     "<document xmlns:sxml=\"urn:sxml\"><array>[<number>1</number>, <number>2</number>"
                     "<sxml:token missing=\"true\" kind=\"]\"/></array></document>"),
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('{1 2}', 'python', {'highlights': false()})",
                     "<module xmlns:sxml=\"urn:sxml\"><set>{<integer>1</integer> <ERROR><integer>2</integer>"
                     "</ERROR>}</set></module>"),
    /* The empty sequence and the empty string. */
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse((), 'json')", ""),
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('', 'c')", "<translation_unit xmlns:sxml=\"urn:sxml\"/>"),
    EXPRESSION_ERROR("Q{urn:sxml:tree-sitter}parse('x', 'cobol')", "unknown-language"),
    EXPRESSION_ERROR("Q{urn:sxml:tree-sitter}parse('x', 'json', {'positions': 'yes'})", "XPTY0004"),
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}languages()",
                     "\"c\" \"css\" \"go\" \"html\" \"javascript\" \"json\" \"markdown\" "
                     "\"markdown_inline\" \"python\" \"tsx\" \"typescript\""),
    /* Locals: a reference takes its definition's highlight (D288). */
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('function f(a) { return a; }', 'javascript')"
                     "//return_statement/identifier/@highlight/string()",
                     "\"variable.parameter\""),
    /* Injections: Markdown's inline grammar, HTML's script; the text stays. */
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('Some *text*.', 'markdown', {'highlights': false()})",
                     "<document xmlns:sxml=\"urn:sxml\"><section><paragraph><inline><inline "
                     "language=\"markdown_inline\">Some <emphasis><emphasis_delimiter>*"
                     "</emphasis_delimiter>text<emphasis_delimiter>*</emphasis_delimiter>"
                     "</emphasis>.</inline></inline></paragraph></section></document>"),
    EXPRESSION_VALUE("Q{urn:sxml:tree-sitter}parse('<script>f(1)</script>', 'html', "
                     "{'highlights': false()})//program/@language/string()",
                     "\"javascript\""),
    EXPRESSION_VALUE("let $code := '> a *b*\n> c' return "
                     "string(Q{urn:sxml:tree-sitter}parse($code, 'markdown')) = $code",
                     "true()"),
    EXPRESSION_VALUE("count(Q{urn:sxml:tree-sitter}parse('Some *text*.', 'markdown', "
                     "{'injections': false()})//emphasis)",
                     "0"),
    /* Deep nesting costs heap, not C stack (D71). */
    EXPRESSION_VALUE("count(Q{urn:sxml:tree-sitter}parse(string-join((1 to 10000) ! '[') || "
                     "string-join((1 to 10000) ! ']'), 'json')//array)",
                     "10000"),
};

/* Every built-in language has a grammar the runtime accepts and queries
   that compile against it. */
static void test_languages_have_grammars_and_queries(sxml_test_state *state) {
    const sxml_source_code_language_set no_registered = {
        .registered_languages = {.languages = NULL, .language_count = 0}};
    for (size_t language_index = 0;
         language_index < sxml_source_code_builtin_languages.language_count;
         language_index += 1) {
        const sxml_source_code_language language =
            sxml_source_code_builtin_language_at(language_index);
        sxml_source_code_language found;
        SXML_TEST_EXPECT(state, sxml_source_code_find_language(
                                    &no_registered, language.language_name, &found) &&
                                    found.grammar == language.grammar);
        sxml_source_code_language_report report;
        SXML_TEST_EXPECT(state,
                         sxml_source_code_check_language(&language, &report) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, report.problem == SXML_SOURCE_CODE_LANGUAGE_PROBLEM_NONE);
    }
    sxml_source_code_language found;
    SXML_TEST_EXPECT(state,
                     !sxml_source_code_find_language(&no_registered, TEXT("cobol"), &found));
}

/* The built-in JSON grammar, as a host would register one. */
static sxml_source_code_language json_language(sxml_byte_view name, sxml_byte_view highlights) {
    const sxml_source_code_language_set no_registered = {
        .registered_languages = {.languages = NULL, .language_count = 0}};
    sxml_source_code_language json;
    memset(&json, 0, sizeof(json));
    (void)sxml_source_code_find_language(&no_registered, TEXT("json"), &json);
    return (sxml_source_code_language){.language_name = name,
                                       .grammar = json.grammar,
                                       .highlights_query = highlights,
                                       .locals_query = TEXT(""),
                                       .injections_query = TEXT(""),
                                       .injection_regex = TEXT("^(mine|my-json)$")};
}

/* sxml_source_code_check_language reports names, ABIs and queries. */
static void test_check_reports_problems(sxml_test_state *state) {
    sxml_source_code_language_report report;
    sxml_source_code_language language = json_language(TEXT("mine"), TEXT("(number) @number"));
    SXML_TEST_EXPECT(state, sxml_source_code_check_language(&language, &report) == SXML_STATUS_OK);
    language.language_name = TEXT("no spaces");
    SXML_TEST_EXPECT(state, sxml_source_code_check_language(&language, &report) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, report.problem == SXML_SOURCE_CODE_LANGUAGE_PROBLEM_NAME);
    language = json_language(TEXT("mine"), TEXT("(number) @number\n(no_such_node) @x"));
    SXML_TEST_EXPECT(state, sxml_source_code_check_language(&language, &report) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, report.problem == SXML_SOURCE_CODE_LANGUAGE_PROBLEM_HIGHLIGHTS_QUERY &&
                                report.byte_offset == 18);
    language = json_language(TEXT("mine"), TEXT(""));
    language.injections_query = TEXT("(((");
    SXML_TEST_EXPECT(state, sxml_source_code_check_language(&language, &report) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, report.problem == SXML_SOURCE_CODE_LANGUAGE_PROBLEM_INJECTIONS_QUERY);
    language.grammar = NULL;
    SXML_TEST_EXPECT(state, sxml_source_code_check_language(&language, &report) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, report.problem == SXML_SOURCE_CODE_LANGUAGE_PROBLEM_ABI &&
                                report.abi_version == 0);
}

/* Registered languages: listed, parsed with their queries, replacing a
   built-in one of the same name; bad configurations are refused; a query
   that does not compile is tree-sitter:invalid-query (D288, D307). */
static void test_registered_languages(sxml_test_state *state) {
    const sxml_source_code_language languages[] = {
        json_language(TEXT("mine"), TEXT("(number) @mine.number")),
        json_language(TEXT("json"), TEXT("(true) @mine.true")),
        json_language(TEXT("broken"), TEXT("(no_such_node) @x")),
    };
    source_code_fixture fixture;
    SXML_TEST_REQUIRE(state,
                      source_code_fixture_create(
                          &fixture, (source_code_fixture_options){
                                        .has_feature = true,
                                        .failing_number = 0,
                                        .languages = {.languages = languages,
                                                      .language_count = 3}}) == SXML_STATUS_OK);
    const struct {
        sxml_byte_view expression_text;
        sxml_byte_view output;
    } cases[] = {
        {TEXT("Q{urn:sxml:tree-sitter}languages()[. = ('broken', 'json', 'mine')]"),
         TEXT("\"broken\" \"json\" \"mine\"")},
        {TEXT("Q{urn:sxml:tree-sitter}parse('[1, true]', 'mine')//@highlight/string()"),
         TEXT("\"mine.number\"")},
        {TEXT("Q{urn:sxml:tree-sitter}parse('[1, true]', 'json')//@highlight/string()"),
         TEXT("\"mine.true\"")},
        {TEXT("Q{urn:sxml:tree-sitter}parse('[1, true]', 'broken', {'highlights': false()})//number/string()"),
         TEXT("\"1\"")},
        /* An injection names a language by its injection regex. */
        {TEXT("Q{urn:sxml:tree-sitter}parse('```my-json\n[2]\n```\n', 'markdown')"
              "//*[@language]/(@language, .//@highlight)/string()"),
         TEXT("\"mine\" \"mine.number\"")},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        SXML_TEST_EXPECT(state, run_expression(&fixture, cases[case_index].expression_text) ==
                                    SXML_STATUS_OK);
        SXML_TEST_EXPECT(state,
                         sxml_byte_view_equals(sxml_output_buffer_get_bytes(fixture.output_buffer),
                                               cases[case_index].output));
    }
    SXML_TEST_EXPECT(state, run_expression(&fixture,
                                           TEXT("Q{urn:sxml:tree-sitter}parse('1', 'broken')")) !=
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(fixture.error_record.code.local_name,
                                                  TEXT("invalid-query")));
    /* The functions' errors are in their namespace (D307). */
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(fixture.error_record.code.namespace_uri,
                                                  TEXT("urn:sxml:tree-sitter")));
    SXML_TEST_EXPECT(state, source_code_fixture_destroy(&fixture));
    /* Two languages of one name, a bad name, no grammar, no array. */
    const sxml_source_code_language twice[] = {languages[0], languages[0]};
    sxml_source_code_language unnamed = languages[0];
    unnamed.language_name = TEXT("");
    sxml_source_code_language without_grammar = languages[0];
    without_grammar.grammar = NULL;
    const sxml_source_code_language_view invalid_views[] = {
        {.languages = twice, .language_count = 2},
        {.languages = &unnamed, .language_count = 1},
        {.languages = &without_grammar, .language_count = 1},
        {.languages = NULL, .language_count = 1},
    };
    for (size_t view_index = 0; view_index < SXML_TEST_COUNT(invalid_views); view_index += 1) {
        SXML_TEST_EXPECT(state, source_code_fixture_create(
                                    &fixture, (source_code_fixture_options){
                                                  .has_feature = true,
                                                  .failing_number = 0,
                                                  .languages = invalid_views[view_index]}) ==
                                    SXML_STATUS_INVALID_ARGUMENT);
        SXML_TEST_EXPECT(state, source_code_fixture_destroy(&fixture));
    }
}

static void test_function_parses(sxml_test_state *state) {
    source_code_fixture fixture;
    SXML_TEST_REQUIRE(state, source_code_fixture_create(&fixture, (source_code_fixture_options){
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
    SXML_TEST_EXPECT(state, source_code_fixture_destroy(&fixture));
}

/* Without the feature the function is absent (D191). */
static void test_function_is_absent_without_the_feature(sxml_test_state *state) {
    source_code_fixture fixture;
    SXML_TEST_REQUIRE(state, source_code_fixture_create(&fixture, (source_code_fixture_options){
                                                               .has_feature = false,
                                                               .failing_number = 0}) ==
                                 SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run_expression(&fixture, TEXT("Q{urn:sxml:tree-sitter}languages()")) !=
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(fixture.error_record.code.local_name, TEXT("XPST0017")));
    SXML_TEST_EXPECT(state, source_code_fixture_destroy(&fixture));
}

static void test_function_survives_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    for (uint64_t failing_number = 1; failing_number < 100000 && status != SXML_STATUS_OK;
         failing_number += 1) {
        source_code_fixture fixture;
        status = source_code_fixture_create(&fixture, (source_code_fixture_options){
                                                   .has_feature = true,
                                                   .failing_number = failing_number});
        if (status == SXML_STATUS_OK) {
            status = run_expression(
                &fixture, TEXT("Q{urn:sxml:tree-sitter}parse('f(\"a\") // x', 'javascript', "
                               "{'positions': true()})//string/@column/string()"));
        }
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                        sxml_output_buffer_get_bytes(fixture.output_buffer),
                                        TEXT("\"3\"")));
        }
        SXML_TEST_EXPECT(state, source_code_fixture_destroy(&fixture));
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
}

static const sxml_test_case source_code_test_cases[] = {
    {.name = "function_parses", .function = test_function_parses},
    {.name = "function_is_absent_without_the_feature",
     .function = test_function_is_absent_without_the_feature},
    {.name = "languages_have_grammars_and_queries",
     .function = test_languages_have_grammars_and_queries},
    {.name = "check_reports_problems", .function = test_check_reports_problems},
    {.name = "registered_languages", .function = test_registered_languages},
    {.name = "function_survives_every_allocation_failure",
     .function = test_function_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "source_code",
            .cases = source_code_test_cases,
            .case_count = SXML_TEST_COUNT(source_code_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
