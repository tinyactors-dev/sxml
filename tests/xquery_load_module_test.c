/*
 * Tests for fn:load-xquery-module (F&O 4.0, 18.2; docs/DECISIONS.md,
 * D154; packet 3.14) through the public API: a module found by its
 * namespace or given as content, its public variables and functions in the
 * module record (each arity of a function with defaults, private
 * declarations left out), values for external variables and the context
 * value, functions of the loaded module reading its globals after the
 * load, a module loaded twice, the FOQM errors, a dynamic error raised as
 * it is, the function called from XPath and from XSLT (the library part
 * xquery_module serves every language, D272), and an allocation-failure
 * loop (D23). Every case checks for leaked cells and allocations.
 *
 * Golden file: tests/xquery_module_test.c.
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

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* --------------------------------------------------------------- Modules */

/* One module the test resolver serves. */
typedef struct served_module {
    sxml_byte_view namespace_uri;
    /* The absolute location hint that finds it; empty: only by
       namespace. */
    sxml_byte_view location;
    sxml_byte_view content;
} served_module;

#define MODULE(namespace_text, location_text, content_text)                                  \
    {.namespace_uri = SXML_BYTE_VIEW_INITIALIZER(namespace_text),                            \
     .location = SXML_BYTE_VIEW_INITIALIZER(location_text),                                  \
     .content = SXML_BYTE_VIEW_INITIALIZER(content_text)}

static const served_module served_modules[] = {
    MODULE("urn:lib", "",
           "module namespace l = 'urn:lib'; "
           "declare variable $l:base := 10; "
           "declare %private variable $l:hidden := 1; "
           "declare variable $l:input external; "
           "declare variable $l:counted as xs:integer external := 0; "
           "declare function l:add($x, $y := 1) { $x + $y + $l:base + l:helper() }; "
           "declare %private function l:helper() { $l:hidden }; "
           "declare function l:input() { $l:input };"),
    MODULE("urn:context", "",
           "module namespace c = 'urn:context'; "
           "declare context value as xs:integer external; "
           "declare variable $c:double := . * 2;"),
    MODULE("urn:failing", "",
           "module namespace f = 'urn:failing'; declare variable $f:bad := 1 div 0;"),
    MODULE("urn:broken", "", "module namespace b = 'urn:broken'; declare function b:f() { 1 + };"),
};

#define SERVED_MODULE_COUNT (sizeof(served_modules) / sizeof(served_modules[0]))

/* The test resolver's state: how many requests it answered. */
typedef struct module_resolver {
    size_t request_count;
} module_resolver;

static sxml_status resolve_module(void *state, const sxml_resource_request *request,
                                  sxml_resource *resource) {
    module_resolver *resolver = state;
    if (request->resource_kind != SXML_RESOURCE_KIND_XQUERY_MODULE) {
        return SXML_STATUS_OK;
    }
    resolver->request_count += 1;
    for (size_t module_index = 0; module_index < SERVED_MODULE_COUNT; module_index += 1) {
        const served_module *module = &served_modules[module_index];
        const bool is_match =
            request->uri_reference.byte_count == 0
                ? sxml_byte_view_equals(module->namespace_uri, request->target_namespace_uri)
                : sxml_byte_view_equals(module->location, request->absolute_uri);
        if (is_match) {
            resource->resource_form = SXML_RESOURCE_FORM_BYTES;
            resource->content = module->content;
            resource->resource_uri = module->location.byte_count > 0 ? module->location
                                                                     : module->namespace_uri;
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_OK;
}

static const sxml_resource_resolver_operations module_resolver_operations = {
    .resolve = resolve_module, .release = NULL};

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    module_resolver resolver;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

/* test_fixture must not move while the processor lives: the resolver
   points to it. */
static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL,
                              .processor = NULL,
                              .output_buffer = NULL,
                              .resolver = {.request_count = 0}};
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
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = {.state = &test_fixture->resolver,
                                  .operations = &module_resolver_operations}},
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

#define LOAD_LIB                                                                          \
    "let $m := load-xquery-module('urn:lib', {'variables': {QName('urn:lib', 'input'): "  \
    "'in'}}) "

static const query_case query_cases[] = {
    /* 18.1: public variables, and each arity of a public function. */
    QUERY_VALUE("variables", LOAD_LIB "return sort(map:keys($m?variables) ! local-name-from-QName(.))",
                "base counted input"),
    QUERY_VALUE("variable-value", LOAD_LIB "return $m?variables(QName('urn:lib', 'base'))", "10"),
    QUERY_VALUE("arities",
                LOAD_LIB "return sort(map:keys($m?functions(QName('urn:lib', 'add'))))", "1 2"),
    QUERY_VALUE("private-functions-left-out",
                LOAD_LIB "return map:keys($m?functions) ! local-name-from-QName(.) => sort()",
                "add input"),
    /* The functions read the loaded module's globals when called. */
    QUERY_VALUE("call", LOAD_LIB "return $m?functions(QName('urn:lib', 'add'))?1(5)", "17"),
    QUERY_VALUE("call-with-both-arities",
                LOAD_LIB "let $add := $m?functions(QName('urn:lib', 'add')) "
                         "return $add?2(5, 2) + $add?1(0)",
                "30"),
    QUERY_VALUE("external-variable",
                LOAD_LIB "return $m?functions(QName('urn:lib', 'input'))?0()", "in"),
    QUERY_VALUE("loaded-twice",
                LOAD_LIB "let $n := load-xquery-module('urn:lib', {'variables': "
                         "{QName('urn:lib', 'input'): 'again'}}) "
                         "return ($m?functions(QName('urn:lib', 'input'))?0(), "
                         "$n?functions(QName('urn:lib', 'input'))?0())",
                "in again"),
    QUERY_VALUE("content",
                "load-xquery-module('urn:dyn', {'content': \"module namespace d = 'urn:dyn'; "
                "declare variable $d:value := 2 + 2;\"})?variables(QName('urn:dyn', 'value'))",
                "4"),
    QUERY_VALUE("context-value",
                "load-xquery-module('urn:context', {'context-value': 21})"
                "?variables(QName('urn:context', 'double'))",
                "42"),
    /* Errors (18.2). */
    QUERY_ERROR("no-namespace", "load-xquery-module('')", "FOQM0001"),
    QUERY_ERROR("no-module", "load-xquery-module('urn:none')", "FOQM0002"),
    QUERY_ERROR("static-error", "load-xquery-module('urn:broken')", "FOQM0003"),
    QUERY_ERROR("wrong-variable-type",
                "load-xquery-module('urn:lib', {'variables': {QName('urn:lib', 'input'): 1, "
                "QName('urn:lib', 'counted'): 'x'}})",
                "FOQM0005"),
    QUERY_ERROR("both-context-options",
                "load-xquery-module('urn:context', {'context-value': 1, 'context-item': 1})",
                "FOQM0005"),
    QUERY_ERROR("later-version", "load-xquery-module('urn:lib', {'xquery-version': 4.1})",
                "FOQM0006"),
    QUERY_ERROR("absent-external-variable",
                "load-xquery-module('urn:lib')?variables(QName('urn:lib', 'input'))", "XPDY0002"),
    QUERY_ERROR("dynamic-error-as-it-is", "load-xquery-module('urn:failing')", "FOAR0001"),
};

static void test_modules_load(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < sizeof(query_cases) / sizeof(query_cases[0]);
         case_index += 1) {
        const query_case *current = &query_cases[case_index];
        fixture test_fixture;
        sxml_status status = fixture_create(0, &test_fixture);
        query_run run = {.expression_text = current->expression_text};
        if (status == SXML_STATUS_OK) {
            status = run_query(&test_fixture, &run);
        }
        const bool passed = current->expected_error_code.byte_count > 0
                                ? status != SXML_STATUS_OK &&
                                      error_code_is(&run, current->expected_error_code)
                                : status == SXML_STATUS_OK &&
                                      output_is(&test_fixture, current->expected_output);
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

/* ------------------------------------------------------- XPath and XSLT */

/* Evaluates expression_text as XPath without a context value and
   serializes the value (method xml) into the fixture's buffer. */
static sxml_status run_xpath(fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){
            .processor = test_fixture->processor,
            .expression_text = run->expression_text,
            .source_name = TEXT("main.xpath"),
            .static_context = {.static_base_uri = TEXT("http://example.com/m/")},
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
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
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

/* Compiles expression_text as a stylesheet, calls its template "main"
   and serializes the principal result into the fixture's buffer. */
static sxml_status run_xslt(fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xslt_stylesheet *stylesheet = NULL;
    sxml_status status = sxml_xslt_stylesheet_create(
        fixture_allocator(test_fixture),
        &(sxml_xslt_stylesheet_configuration){
            .processor = test_fixture->processor,
            .principal_module_text = run->expression_text,
            .static_context = {.static_base_uri = TEXT("http://example.com/m/main.xsl")},
            .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
            .error_record = &run->error_record},
        &stylesheet);
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
    sxml_xslt_stylesheet_destroy(stylesheet);
    return status;
}

#define LIBRARY_CALL                                                                       \
    "load-xquery-module('urn:lib', {'variables': {QName('urn:lib', 'input'): 'in'}})"     \
    "?functions(QName('urn:lib', 'add'))?1(5)"

#define STYLESHEET(body)                                                                   \
    "<xsl:stylesheet version='3.0' xmlns:xsl='http://www.w3.org/1999/XSL/Transform'>"     \
    "<xsl:output method='text'/><xsl:template name='main'>" body                          \
    "</xsl:template></xsl:stylesheet>"

/* F&O 4.0, 18.2 in the other host languages (D272): XPath and XSLT find
   fn:load-xquery-module, load the module through the processor's
   resolver, and raise its errors. */
static void test_other_languages_load(sxml_test_state *state) {
    static const struct {
        const char *name;
        bool is_stylesheet;
        sxml_byte_view text;
        sxml_byte_view expected_output;
        sxml_byte_view expected_error_code;
    } cases[] = {
        {"xpath-call", false, SXML_BYTE_VIEW_INITIALIZER(LIBRARY_CALL),
         SXML_BYTE_VIEW_INITIALIZER("17"), {.bytes = NULL, .byte_count = 0}},
        {"xpath-function-lookup", false,
         SXML_BYTE_VIEW_INITIALIZER("exists(function-lookup(QName("
                                    "'http://www.w3.org/2005/xpath-functions', "
                                    "'load-xquery-module'), 2))"),
         SXML_BYTE_VIEW_INITIALIZER("true"), {.bytes = NULL, .byte_count = 0}},
        {"xpath-no-module", false, SXML_BYTE_VIEW_INITIALIZER("load-xquery-module('urn:none')"),
         {.bytes = NULL, .byte_count = 0}, SXML_BYTE_VIEW_INITIALIZER("FOQM0002")},
        {"xslt-call", true,
         SXML_BYTE_VIEW_INITIALIZER(STYLESHEET("<xsl:value-of select=\"" LIBRARY_CALL "\"/>")),
         SXML_BYTE_VIEW_INITIALIZER("17"), {.bytes = NULL, .byte_count = 0}},
        {"xslt-variable", true,
         SXML_BYTE_VIEW_INITIALIZER(STYLESHEET(
             "<xsl:value-of select=\"load-xquery-module('urn:lib', {'variables': "
             "{QName('urn:lib', 'input'): 1}})?variables(QName('urn:lib', 'base'))\"/>")),
         SXML_BYTE_VIEW_INITIALIZER("10"), {.bytes = NULL, .byte_count = 0}},
        {"xslt-function-available", true,
         SXML_BYTE_VIEW_INITIALIZER(STYLESHEET(
             "<xsl:value-of select=\"function-available('load-xquery-module', 1)\"/>")),
         SXML_BYTE_VIEW_INITIALIZER("true"), {.bytes = NULL, .byte_count = 0}},
        {"xslt-static-error", true,
         SXML_BYTE_VIEW_INITIALIZER(
             STYLESHEET("<xsl:value-of select=\"load-xquery-module('urn:broken')\"/>")),
         {.bytes = NULL, .byte_count = 0}, SXML_BYTE_VIEW_INITIALIZER("FOQM0003")},
    };
    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); case_index += 1) {
        fixture test_fixture;
        sxml_status status = fixture_create(0, &test_fixture);
        query_run run = {.expression_text = cases[case_index].text};
        if (status == SXML_STATUS_OK) {
            status = cases[case_index].is_stylesheet ? run_xslt(&test_fixture, &run)
                                                     : run_xpath(&test_fixture, &run);
        }
        const bool passed =
            cases[case_index].expected_error_code.byte_count > 0
                ? status != SXML_STATUS_OK &&
                      error_code_is(&run, cases[case_index].expected_error_code)
                : status == SXML_STATUS_OK &&
                      output_is(&test_fixture, cases[case_index].expected_output);
        (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                              .expression = cases[case_index].name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
        (void)sxml_test_record_check(
            state, (sxml_test_check){.passed = fixture_destroy(&test_fixture),
                                     .expression = cases[case_index].name,
                                     .file_name = __FILE__,
                                     .line_number = __LINE__});
    }
}

static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(failing_number, &test_fixture);
    if (status == SXML_STATUS_OK) {
        query_run run = {.expression_text = TEXT(LOAD_LIB
                                                 "return $m?functions(QName('urn:lib', "
                                                 "'add'))?2(1, 2)")};
        status = run_query(&test_fixture, &run);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("14")));
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_load_survives_every_allocation_failure(sxml_test_state *state) {
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 100000; failing_number += 1) {
        status = run_with_failure(state, failing_number);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case xquery_load_module_test_cases[] = {
    {.name = "modules_load", .function = test_modules_load},
    {.name = "other_languages_load", .function = test_other_languages_load},
    {.name = "load_survives_every_allocation_failure",
     .function = test_load_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery_load_module",
            .cases = xquery_load_module_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_load_module_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
