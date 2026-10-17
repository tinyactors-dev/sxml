/*
 * Tests for include/sxml/xpath.h through the public API alone: slice
 * queries compiled, evaluated and written by the adaptive method, the
 * static context (namespaces, variables, static base URI), the dynamic
 * context (context value, bindings, available documents, the resolver,
 * the clock), nested evaluations, limits, sequence types given as text,
 * and allocation-failure loops on compile and evaluate (D23). Every case
 * checks for leaked cells and allocations.
 *
 * Golden file: tests/memory_test.c.
 */
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <stdalign.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* Levels of parentheses in default_limits_allow_deep_input. */
#define TEST_NESTING_DEPTH ((size_t)2000)

/* ---------------------------------------------------------------- Fixture */

/* 2026-10-16T12:30:45.5-05:00, implicit timezone +02:00. */
static const sxml_clock_reading fixture_clock_reading = {
    .current_date_time = {.year = 2026,
                          .month = 10,
                          .day = 16,
                          .hour = 12,
                          .minute = 30,
                          .second = 45,
                          .nanosecond = 500000000,
                          .has_timezone = true,
                          .timezone_offset_minutes = -300},
    .implicit_timezone_offset_minutes = 120,
};

static const char works_text[] =
    "<works><employee name='Jane Doe 1'><hours>40</hours><hours>20</hours>"
    "</employee><employee name='John Doe 2'><hours>30</hours></employee>"
    "</works>";

static const char staff_text[] = "<staff><employee><hours>10</hours></employee></staff>";

static const sxml_resource_entry fixture_resources[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/docs/works.xml"),
     .content = {.bytes = (const uint8_t *)works_text, .byte_count = sizeof(works_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/docs/staff.xml"),
     .content = {.bytes = (const uint8_t *)staff_text, .byte_count = sizeof(staff_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/docs/broken.xml"),
     .content = SXML_BYTE_VIEW_INITIALIZER("<a><b></a>")},
};

static const sxml_resource_entry_view fixture_resource_view = {
    .resource_entries = fixture_resources,
    .resource_entry_count = sizeof(fixture_resources) / sizeof(fixture_resources[0])};

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

typedef struct fixture_options {
    uint64_t failing_allocation_number;
    sxml_processor_limits limits;
} fixture_options;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(const fixture_options *options, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     options->failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .limits = options->limits,
            .resource_resolver = sxml_table_resource_resolver(&fixture_resource_view),
            .clock = sxml_fixed_clock(&fixture_clock_reading)},
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

static sxml_name_identifier fixture_name(const fixture *test_fixture, sxml_byte_view local_name) {
    sxml_name_identifier name = {.index = 0};
    if (sxml_name_pool_intern_name(sxml_processor_get_name_pool(test_fixture->processor),
                                   &(sxml_name_request){.namespace_uri = {.index = 0},
                                                        .local_name = local_name},
                                   &name) != SXML_STATUS_OK) {
        return (sxml_name_identifier){.index = 0};
    }
    return name;
}

/* An inline atomic item (no cell): an integer or a boolean. */
static sxml_item make_inline_item(const fixture *test_fixture, sxml_atomic_item_request request) {
    sxml_item item = {.representation = 0};
    if (sxml_atomic_item_make(sxml_processor_get_item_heap(test_fixture->processor), &request,
                              &item) != SXML_STATUS_OK) {
        return (sxml_item){.representation = 0};
    }
    return item;
}

/* --------------------------------------------------------------- Running */

/* One query to run and what it gives. */
typedef struct query_run {
    sxml_byte_view expression_text;
    sxml_static_context_configuration static_context;
    sxml_dynamic_context_configuration dynamic_context;
    /* Out: the error record of the failure. */
    sxml_error_record error_record;
} query_run;

/* Compiles and evaluates run->expression_text and writes the result by the
   adaptive method into the fixture's output buffer (cleared first). */
static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    sxml_output_buffer_clear(test_fixture->output_buffer);
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){.processor = test_fixture->processor,
                                               .expression_text = run->expression_text,
                                               .source_name = TEXT("test.xpath"),
                                               .static_context = run->static_context,
                                               .error_record = &run->error_record},
        &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xpath_expression_evaluate(
        expression,
        &(sxml_xpath_evaluation_request){.dynamic_context = run->dynamic_context,
                                         .error_record = &run->error_record},
        &result);
    sxml_xpath_expression_destroy(expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_serialize_sequence(
        sxml_processor_get_item_heap(test_fixture->processor),
        &(sxml_serialization_request){
            .value = result,
            .serialization_parameters = {.serialization_method =
                                             SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                         .has_item_separator = true,
                                         .item_separator = TEXT(" ")},
            .error_record = &run->error_record},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    sxml_sequence_release(result);
    return status;
}

static bool output_is(const fixture *test_fixture, sxml_byte_view expected) {
    return sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                 expected);
}

static bool error_code_is(const query_run *run, sxml_byte_view local_name) {
    return sxml_byte_view_equals(run->error_record.code.local_name, local_name);
}

/* ----------------------------------------------------------- Slice cases */

/* A slice query (tests/qt4/torture-slice.txt) and its adaptive output, or
   the error code it raises. */
typedef struct slice_case {
    const char *test_case_name;
    sxml_byte_view expression_text;
    sxml_byte_view expected_output;
    sxml_byte_view expected_error_code;
} slice_case;

#define SLICE_VALUE(name, text, output)                                                   \
    {.test_case_name = (name),                                                           \
     .expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                \
     .expected_output = SXML_BYTE_VIEW_INITIALIZER(output),                              \
     .expected_error_code = {.bytes = NULL, .byte_count = 0}}

#define SLICE_ERROR(name, text, code)                                                     \
    {.test_case_name = (name),                                                           \
     .expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                \
     .expected_output = {.bytes = NULL, .byte_count = 0},                                \
     .expected_error_code = SXML_BYTE_VIEW_INITIALIZER(code)}

static const slice_case slice_cases[] = {
    SLICE_VALUE("K-NumericAdd-29", "1+1 eq 2", "true()"),
    SLICE_VALUE("op-numeric-addint2args-1", "xs:int(\"-1873914410\") + xs:int(\"-273569238\")",
                "-2147483648"),
    SLICE_VALUE("op-numeric-divideint2args-1",
                "xs:int(\"-2147483648\") div xs:int(\"-2147483648\")", "1"),
    SLICE_VALUE("K2-NumericMod-9", "xs:float(\"NaN\") mod 3", "xs:float(\"NaN\")"),
    SLICE_ERROR("K-ValCompTypeChecking-1", "(1, 2, 3) eq 3", "XPTY0004"),
    SLICE_VALUE("fn-substring-25", "substring(\"\xF0\x90\x80\x81\", 1, 2)",
                "\"\xF0\x90\x80\x81\""),
    SLICE_VALUE("fn-tokenize-1a", "fn:tokenize(\"abba\", \".?\")", "\"\" \"\" \"\" \"\" \"\""),
    SLICE_VALUE("filterexpressionhc2", "((1 to 25)[. mod 2 eq 0])",
                "2 4 6 8 10 12 14 16 18 20 22 24"),
    SLICE_VALUE("bang-1", "(1 to 10)!(.*.)", "1 4 9 16 25 36 49 64 81 100"),
    SLICE_ERROR("K-QuantExprWithout-79", "some $foo in $foo satisfies 1", "XPST0008"),
    SLICE_VALUE("RangeExpr-408a", "count(100000000000 to 100000000003)", "4"),
    SLICE_VALUE("casthc31", "xs:dateTime(\"1999-05-31T13:20:00-05:00\") cast as xs:date",
                "xs:date(\"1999-05-31-05:00\")"),
    SLICE_ERROR("treat-as-1",
                "let $var := (100+200) div 2 return fn:concat($var treat as xs:string,\"a "
                "string\")",
                "XPDY0050"),
    SLICE_VALUE("inline-fn-004",
                "sum(for $i in 1 to 6 return let $add := function($x){$x + $i} return $add(4))",
                "45"),
    SLICE_ERROR("inline-fn-005", "(1 to 4) ! (let $add := function($x){$x + .} return $add(4))",
                "XPDY0002"),
    SLICE_VALUE("DynamicFunctionCall-143",
                "let $f := (abs#1, round#1, floor#1, ceiling#1) return $f(23.7)",
                "23.7 24 23 24"),
    SLICE_VALUE("ArrowPostfix-108", "let $usd := \"$\" => concat(?) return $usd(3)", "\"$3\""),
    SLICE_VALUE("for-each-402", "for-each(22 to 28, fn($it, $pos){$pos, $it})",
                "1 22 2 23 3 24 4 25 5 26 6 27 7 28"),
    SLICE_ERROR("fn-filter-003", "fn:filter( fn:boolean#1 )", "XPST0017"),
    SLICE_VALUE("fold-right-013", "fold-right(1 to 1000000, 0, function($a, $b){ $b + 1 })",
                "1000000"),
    SLICE_VALUE("fn-sort-22",
                "fn:sort( (map{\"key\":1, \"value\":89}, map{\"key\":6, \"value\":21}, "
                "map{\"key\":2, \"value\":33}), (), map:get(?, \"key\"))?value",
                "89 33 21"),
    SLICE_VALUE("map-merge-401",
                "map:merge(({17:0}, {29:0}, {'a':0}, {-234:0}, {86:0}, {445:0}, {101:0}, "
                "{'e':0})) => map:keys()",
                "17 29 \"a\" -234 86 445 101 \"e\""),
    SLICE_VALUE("UnaryLookup-023", "[['a', 'b', 'c'], ['b', 'c', 'd'], ['e', 'f', 'b']]!?*!?1",
                "\"a\" \"b\" \"e\""),
    SLICE_ERROR("array-subarray-312", "array:subarray([1,2,3], 2, 3)", "FOAY0001"),
    SLICE_VALUE("fn-parse-json-050",
                "parse-json('{\"a\":1, \"b\":2, \"a\":3}', map{'duplicates':'use-first'})",
                "{\"a\":1.0e0,\"b\":2.0e0}"),
    SLICE_VALUE("xml-to-json-015",
                "xml-to-json(json-to-xml('{\"A\":1, \"B\":2, \"C\":3, \"D\":4, \"E\":5}'))",
                "\"{\"\"A\"\":1,\"\"B\"\":2,\"\"C\"\":3,\"\"D\"\":4,\"\"E\"\":5}\""),
    SLICE_VALUE("parse-xml-012", "parse-xml(\"<a>foo</a>\") is parse-xml(\"<a>foo</a>\")",
                "false()"),
    SLICE_VALUE("fn-root-29", "let $j := jtree([1,2,3]) return $j is root($j/*[3])", "true()"),
    SLICE_VALUE("deep-equal-40-comments-001",
                "deep-equal(parse-xml('<a><!--foo--></a>'), parse-xml('<a><!--bar--></a>'))",
                "true()"),
    SLICE_ERROR("fn-error-1", "fn:error()", "FOER0000"),
};

static void test_slice_queries_round_trip(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(slice_cases); case_index += 1) {
        const slice_case *test = &slice_cases[case_index];
        fixture test_fixture;
        SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                                &test_fixture) == SXML_STATUS_OK);
        query_run run = {.expression_text = test->expression_text};
        const sxml_status status = run_query(&test_fixture, &run);
        if (test->expected_error_code.byte_count > 0) {
            SXML_TEST_EXPECT(state, sxml_status_has_error_record(status));
            SXML_TEST_EXPECT(state, error_code_is(&run, test->expected_error_code));
        } else {
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
            SXML_TEST_EXPECT(state, output_is(&test_fixture, test->expected_output));
        }
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
}

/* ----------------------------------------------------- Static context */

static void test_static_context_binds_namespaces_and_variables(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(test_fixture.processor);
    sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                      .namespace_uri = {.index = 0}};
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_prefix(name_pool, TEXT("p"),
                                                                   &binding.namespace_prefix) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_uri(name_pool, TEXT("urn:p"),
                                                                &binding.namespace_uri) ==
                                SXML_STATUS_OK);
    const sxml_variable_declaration declarations[] = {
        {.variable_name = fixture_name(&test_fixture, TEXT("x")),
         .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                           .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}},
        {.variable_name = fixture_name(&test_fixture, TEXT("unbound")),
         .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                           .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}},
    };
    const sxml_static_context_configuration static_context = {
        .namespace_bindings = {.namespace_bindings = &binding, .namespace_binding_count = 1},
        .static_base_uri = {.bytes = NULL, .byte_count = 0},
        .variable_declarations = {.variable_declarations = declarations,
                                  .variable_declaration_count = 2}};
    const sxml_variable_binding bindings[] = {
        {.variable_name = declarations[0].variable_name,
         .value = sxml_sequence_from_item(make_inline_item(
             &test_fixture,
             (sxml_atomic_item_request){
                 .type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                 .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                           .datum_details = {.integer = 41}}}))},
    };
    const sxml_dynamic_context_configuration dynamic_context = {
        .variable_bindings = {.variable_bindings = bindings, .variable_binding_count = 1}};
    query_run run = {.expression_text = TEXT("($x + 1, xs:QName('p:a'))"),
                     .static_context = static_context,
                     .dynamic_context = dynamic_context};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("42 #Q{urn:p}a")));
    /* A declared variable without a binding raises XPDY0002. */
    run.expression_text = TEXT("$unbound");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XPDY0002")));
    /* A bound value is coerced to the declared type (XPTY0004). */
    const sxml_variable_binding wrong_bindings[] = {
        {.variable_name = declarations[0].variable_name,
         .value = sxml_sequence_from_item(make_inline_item(
             &test_fixture,
             (sxml_atomic_item_request){
                 .type = sxml_builtin_type_identifier(SXML_TYPE_XS_BOOLEAN),
                 .datum = {.datum_kind = SXML_DATUM_KIND_BOOLEAN,
                           .datum_details = {.boolean = true}}}))},
    };
    run.expression_text = TEXT("$x");
    run.dynamic_context.variable_bindings =
        (sxml_variable_binding_view){.variable_bindings = wrong_bindings,
                                     .variable_binding_count = 1};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_TYPE_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XPTY0004")));
    /* An undeclared variable is a static error with a location. */
    run.expression_text = TEXT("1 + $y");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XPST0008")));
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.location.column_number == 5);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_static_base_uri_reaches_functions(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    query_run run = {
        .expression_text = TEXT("static-base-uri(), resolve-uri('../b/c.xml'), "
                                "resolve-uri('d', 'http://x.org/a/'), "
                                "count(doc('works.xml')//employee)"),
        .static_context = {.static_base_uri = TEXT("http://example.com/docs/index.xml")}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     output_is(&test_fixture,
                               TEXT("\"http://example.com/docs/index.xml\" "
                                    "\"http://example.com/b/c.xml\" \"http://x.org/a/d\" 2")));
    /* Without a static base URI, a relative URI cannot be resolved. */
    run.static_context.static_base_uri = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    run.expression_text = TEXT("resolve-uri('a')");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("FONS0005")));
    run.expression_text = TEXT("static-base-uri()");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("")));
    /* A static base URI must be absolute. */
    run.static_context.static_base_uri = TEXT("relative/");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ----------------------------------------------------- Dynamic context */

static void test_context_value_is_a_parsed_document(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state,
                      sxml_processor_parse_document(
                          test_fixture.processor,
                          &(sxml_xml_parse_request){
                              .input = {.bytes = (const uint8_t *)works_text,
                                        .byte_count = sizeof(works_text) - 1},
                              .source_name = TEXT("works.xml")},
                          &document) == SXML_STATUS_OK);
    query_run run = {.expression_text = TEXT("sum(//hours), /works/employee[2]/@name/string()"),
                     .dynamic_context = {.has_context_value = true,
                                         .context_value = sxml_sequence_from_item(document)}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("9.0e1 \"John Doe 2\"")));
    /* The same evaluation without a focus. */
    run.dynamic_context.has_context_value = false;
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XPDY0002")));
    sxml_item_release(document);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_doc_is_stable_and_resolves_through_the_resolver(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    /* fn-doc-6: two URIs, two documents; one URI, one node. */
    query_run run = {
        .expression_text = TEXT("doc('http://example.com/docs/works.xml') is "
                                "doc('http://example.com/docs/staff.xml'), "
                                "doc('works.xml') is doc('http://example.com/docs/works.xml'), "
                                "doc-available('missing.xml')"),
        .static_context = {.static_base_uri = TEXT("http://example.com/docs/")}};
    const sxml_status status = run_query(&test_fixture, &run);
    /* fn:doc-available is not in Phase 1's library: then only the first two. */
    if (status == SXML_STATUS_STATIC_ERROR) {
        run.expression_text = TEXT("doc('http://example.com/docs/works.xml') is "
                                   "doc('http://example.com/docs/staff.xml'), "
                                   "doc('works.xml') is doc('http://example.com/docs/works.xml')");
        SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("false() true()")));
    } else {
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("false() true() false()")));
    }
    run.expression_text = TEXT("doc('missing.xml')");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("FODC0002")));
    run.expression_text = TEXT("doc('broken.xml')");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("FODC0002")));
    run.expression_text = TEXT("parse-xml('<a>')");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("FODC0006")));
    /* The null resolver of the request means the processor's; another
       resolver replaces it. */
    static const sxml_resource_entry_view no_entries = {.resource_entries = NULL,
                                                        .resource_entry_count = 0};
    run.expression_text = TEXT("doc('works.xml')");
    run.dynamic_context.resource_resolver = sxml_table_resource_resolver(&no_entries);
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("FODC0002")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_available_documents_are_the_bound_nodes(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    sxml_item document = {.representation = 0};
    SXML_TEST_REQUIRE(state, sxml_processor_parse_document(
                                 test_fixture.processor,
                                 &(sxml_xml_parse_request){.input = TEXT("<local/>")},
                                 &document) == SXML_STATUS_OK);
    const sxml_available_document documents[] = {
        {.document_uri = TEXT("urn:local"), .document_node = document}};
    query_run run = {
        .expression_text = TEXT("doc('urn:local') is ., name(doc('urn:local')/*)"),
        .dynamic_context = {.has_context_value = true,
                            .context_value = sxml_sequence_from_item(document),
                            .available_documents = {.available_documents = documents,
                                                    .available_document_count = 1}}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("true() \"local\"")));
    /* Two available documents with one URI are not allowed. */
    const sxml_available_document duplicates[] = {
        {.document_uri = TEXT("urn:local"), .document_node = document},
        {.document_uri = TEXT("urn:local"), .document_node = document}};
    run.dynamic_context.available_documents =
        (sxml_available_document_view){.available_documents = duplicates,
                                       .available_document_count = 2};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_item_release(document);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_clock_is_read_once_per_evaluation(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    query_run run = {.expression_text = TEXT("current-dateTime(), implicit-timezone(), "
                                             "current-dateTime() eq current-dateTime()")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     output_is(&test_fixture,
                               TEXT("xs:dateTime(\"2026-10-16T19:30:45.5+02:00\") "
                                    "xs:duration(\"PT2H\") true()")));
    /* The evaluation's clock replaces the processor's. */
    const sxml_clock_reading utc_reading = {
        .current_date_time = {.year = 2000, .month = 1, .day = 1, .has_timezone = true},
        .implicit_timezone_offset_minutes = 0};
    run.expression_text = TEXT("current-date()");
    run.dynamic_context.clock = sxml_fixed_clock(&utc_reading);
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("xs:date(\"2000-01-01Z\")")));
    /* A reading outside the contract stops the evaluation. */
    const sxml_clock_reading bad_reading = {
        .current_date_time = {.year = 2000, .month = 2, .day = 30, .has_timezone = true},
        .implicit_timezone_offset_minutes = 0};
    run.dynamic_context.clock = sxml_fixed_clock(&bad_reading);
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ---------------------------------------------------- Nested evaluation */

/* A host function whose body evaluates another expression of the same
   processor with its argument as $n. */
typedef struct nested_function {
    const sxml_xpath_expression *expression;
    sxml_name_identifier variable_name;
} nested_function;

static sxml_status call_nested_function(void *state, const sxml_function_call *call,
                                        sxml_sequence *result) {
    const nested_function *function = state;
    const sxml_variable_binding binding = {.variable_name = function->variable_name,
                                           .value = call->arguments.sequences[0]};
    return sxml_xpath_expression_evaluate(
        function->expression,
        &(sxml_xpath_evaluation_request){
            .dynamic_context = {.variable_bindings = {.variable_bindings = &binding,
                                                      .variable_binding_count = 1}},
            .error_record = call->error_record},
        result);
}

static const sxml_function_body_operations nested_function_operations = {
    .call = call_nested_function, .retain = NULL, .release = NULL};

static void test_evaluations_nest(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    const sxml_allocator allocator = fixture_allocator(&test_fixture);
    const sxml_variable_declaration inner_declaration = {
        .variable_name = fixture_name(&test_fixture, TEXT("n")),
        .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                          .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}};
    sxml_xpath_expression *inner = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_xpath_expression_create(
                          allocator,
                          &(sxml_xpath_expression_configuration){
                              .processor = test_fixture.processor,
                              .expression_text = TEXT("$n * 10 + count(1 to $n)"),
                              .static_context = {.variable_declarations =
                                                     {.variable_declarations = &inner_declaration,
                                                      .variable_declaration_count = 1}}},
                          &inner) == SXML_STATUS_OK);
    sxml_sequence_type signature = {.item_type = {.index = 0},
                                    .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    SXML_TEST_EXPECT(state,
                     sxml_xpath_parse_sequence_type(
                         &(sxml_sequence_type_parse_request){
                             .processor = test_fixture.processor,
                             .sequence_type_text = TEXT("function(xs:integer) as xs:integer")},
                         &signature) == SXML_STATUS_OK);
    nested_function function = {.expression = inner,
                                .variable_name = inner_declaration.variable_name};
    sxml_item function_item = {.representation = 0};
    SXML_TEST_EXPECT(state,
                     sxml_function_item_make(
                         sxml_processor_get_item_heap(test_fixture.processor),
                         &(sxml_function_item_request){
                             .function_name = {.index = 0},
                             .signature = signature.item_type,
                             .function_annotations = {.function_annotations = NULL,
                                                      .function_annotation_count = 0},
                             .captured_values = {.sequences = NULL, .sequence_count = 0},
                             .body = {.state = &function,
                                      .operations = &nested_function_operations}},
                         &function_item) == SXML_STATUS_OK);
    const sxml_variable_declaration outer_declaration = {
        .variable_name = fixture_name(&test_fixture, TEXT("f")),
        .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                          .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    const sxml_variable_binding outer_binding = {.variable_name = outer_declaration.variable_name,
                                                 .value = sxml_sequence_from_item(function_item)};
    query_run run = {
        .expression_text = TEXT("for-each(1 to 3, $f), $f(4)"),
        .static_context = {.variable_declarations = {.variable_declarations = &outer_declaration,
                                                     .variable_declaration_count = 1}},
        .dynamic_context = {.variable_bindings = {.variable_bindings = &outer_binding,
                                                  .variable_binding_count = 1}}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("11 22 33 44")));
    sxml_item_release(function_item);
    sxml_xpath_expression_destroy(inner);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A function item in a result outlives its expression (D38, D51). */
static void test_function_items_outlive_their_expression(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    const sxml_allocator allocator = fixture_allocator(&test_fixture);
    sxml_xpath_expression *maker = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_xpath_expression_create(
                          allocator,
                          &(sxml_xpath_expression_configuration){
                              .processor = test_fixture.processor,
                              .expression_text = TEXT("let $k := 5 return function($x) { $x + $k }")},
                          &maker) == SXML_STATUS_OK);
    sxml_sequence closure = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_xpath_expression_evaluate(
                                maker, &(sxml_xpath_evaluation_request){.error_record = NULL},
                                &closure) == SXML_STATUS_OK);
    sxml_xpath_expression_destroy(maker);
    const sxml_variable_declaration declaration = {
        .variable_name = fixture_name(&test_fixture, TEXT("g")),
        .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                          .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    const sxml_variable_binding binding = {.variable_name = declaration.variable_name,
                                           .value = closure};
    query_run run = {
        .expression_text = TEXT("$g(37)"),
        .static_context = {.variable_declarations = {.variable_declarations = &declaration,
                                                     .variable_declaration_count = 1}},
        .dynamic_context = {.variable_bindings = {.variable_bindings = &binding,
                                                  .variable_binding_count = 1}}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("42")));
    sxml_sequence_release(closure);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------------- Limits */

static void test_limits_give_limit_exceeded(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.failing_allocation_number = 0,
                                                        .limits = {.maximum_expression_depth = 20,
                                                                   .maximum_evaluation_depth =
                                                                       50}},
                                     &test_fixture) == SXML_STATUS_OK);
    /* Deep recursion: a limit status, never a stack overflow. */
    query_run run = {.expression_text = TEXT(
                         "let $f := function($n, $self) { if ($n eq 0) then 0 else 1 + "
                         "$self($n - 1, $self) } return $f(100, $f)")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_LIMIT_EXCEEDED);
    run.expression_text = TEXT("let $f := function($n, $self) { if ($n eq 0) then 0 else 1 + "
                               "$self($n - 1, $self) } return $f(10, $f)");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("10")));
    run.expression_text = TEXT("((((((((((((((((((((((((((1))))))))))))))))))))))))))");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Work that allocates little is bounded too (D89, P9). */
static void test_step_bound_stops_long_evaluations(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state,
                      fixture_create(&(fixture_options){.failing_allocation_number = 0,
                                                        .limits = {.maximum_evaluation_step_count =
                                                                       100000}},
                                     &test_fixture) == SXML_STATUS_OK);
    query_run run = {.expression_text = TEXT("some $x in 1 to 100000000000 satisfies $x = 0")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.description.byte_count > 0);
    run.expression_text = TEXT("-1 = -100000000000 to -1");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_LIMIT_EXCEEDED);
    /* The count starts again with every evaluation. */
    run.expression_text = TEXT("sum((1 to 100) ! (. * 2))");
    for (int evaluation_number = 0; evaluation_number < 3; evaluation_number += 1) {
        SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("10100")));
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_default_limits_allow_deep_input(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    /* 2,000 levels of parentheses: within the default of 10,000 (D63). */
    static char text[2 * TEST_NESTING_DEPTH + 1];
    for (size_t index = 0; index < TEST_NESTING_DEPTH; index += 1) {
        text[index] = '(';
        text[2 * TEST_NESTING_DEPTH - index] = ')';
    }
    text[TEST_NESTING_DEPTH] = '7';
    query_run run = {.expression_text = {.bytes = (const uint8_t *)text,
                                         .byte_count = sizeof(text)}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("7")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------- Sequence types */

static void test_sequence_types_parse_from_text(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    sxml_sequence_type sequence_type = {.item_type = {.index = 0},
                                        .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    SXML_TEST_EXPECT(state, sxml_xpath_parse_sequence_type(
                                &(sxml_sequence_type_parse_request){
                                    .processor = test_fixture.processor,
                                    .sequence_type_text = TEXT("xs:integer+")},
                                &sequence_type) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sequence_type.item_type.index ==
                                sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER).index);
    SXML_TEST_EXPECT(state, sequence_type.occurrence == SXML_OCCURRENCE_ONE_OR_MORE);
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    SXML_TEST_EXPECT(state, sxml_xpath_parse_sequence_type(
                                &(sxml_sequence_type_parse_request){
                                    .processor = test_fixture.processor,
                                    .sequence_type_text = TEXT("q:t"),
                                    .error_record = &error_record},
                                &sequence_type) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name, TEXT("XPST0081")));
    SXML_TEST_EXPECT(state, sxml_xpath_parse_sequence_type(
                                &(sxml_sequence_type_parse_request){
                                    .processor = test_fixture.processor,
                                    .sequence_type_text = TEXT("xs:integer+("),
                                    .error_record = &error_record},
                                &sequence_type) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(error_record.code.local_name, TEXT("XPST0003")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------- Lifecycle */

static void test_expression_lifecycle_checks_its_arguments(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_xpath_expression_memory_requirements(
                                &(sxml_xpath_expression_configuration){.processor = NULL},
                                &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    const sxml_xpath_expression_configuration configuration = {
        .processor = test_fixture.processor, .expression_text = TEXT("1 to 3")};
    SXML_TEST_EXPECT(state, sxml_xpath_expression_memory_requirements(
                                &configuration, &requirements) == SXML_STATUS_OK);
    alignas(16) uint8_t storage[256];
    SXML_TEST_REQUIRE(state, requirements.byte_count <= sizeof(storage) &&
                                 requirements.alignment <= 16);
    sxml_xpath_expression *expression = NULL;
    SXML_TEST_EXPECT(state,
                     sxml_xpath_expression_initialize(
                         (sxml_memory_view){.bytes = storage,
                                            .byte_count = requirements.byte_count - 1},
                         &configuration, &expression) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, expression == NULL);
    SXML_TEST_EXPECT(state,
                     sxml_xpath_expression_initialize(
                         (sxml_memory_view){.bytes = storage, .byte_count = sizeof(storage)},
                         &configuration, &expression) == SXML_STATUS_OK);
    sxml_sequence result = sxml_sequence_empty();
    for (int evaluation_number = 0; evaluation_number < 3; evaluation_number += 1) {
        SXML_TEST_EXPECT(state, sxml_xpath_expression_evaluate(
                                    expression, &(sxml_xpath_evaluation_request){.error_record = NULL},
                                    &result) == SXML_STATUS_OK);
        uint64_t item_count = 0;
        SXML_TEST_EXPECT(state, sxml_sequence_count(result, NULL, &item_count) == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, item_count == 3);
        sxml_sequence_release(result);
    }
    sxml_xpath_expression_finalize(expression);
    sxml_xpath_expression_finalize(NULL);
    sxml_xpath_expression_destroy(NULL);
    /* Identifiers the processor did not issue. */
    const sxml_variable_declaration foreign = {
        .variable_name = {.index = 1000000},
        .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                          .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    SXML_TEST_EXPECT(state, sxml_xpath_expression_create(
                                fixture_allocator(&test_fixture),
                                &(sxml_xpath_expression_configuration){
                                    .processor = test_fixture.processor,
                                    .expression_text = TEXT("1"),
                                    .static_context = {.variable_declarations =
                                                           {.variable_declarations = &foreign,
                                                            .variable_declaration_count = 1}}},
                                &expression) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ---------------------------------------------- Allocation failure loops */

/* Runs one compile and evaluation with allocation number failing_number
   failing; returns the status and checks that nothing leaked. */
static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number,
                                    sxml_byte_view expression_text) {
    fixture test_fixture;
    sxml_status status = fixture_create(
        &(fixture_options){.failing_allocation_number = failing_number}, &test_fixture);
    if (status == SXML_STATUS_OK) {
        query_run run = {.expression_text = expression_text,
                         .static_context = {.static_base_uri =
                                                TEXT("http://example.com/docs/index.xml")}};
        status = run_query(&test_fixture, &run);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("true() 3 \"b\" 2")));
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_compile_and_evaluate_survive_every_allocation_failure(
    sxml_test_state *state) {
    const sxml_byte_view text = TEXT(
        "let $f := function($x) { $x + 1 } return ($f(1) eq 2, "
        "fold-left(1 to 2, 0, op('+')), parse-json('[\"a\",\"b\"]')?2, "
        "count(doc('works.xml')//employee))");
    sxml_status status = SXML_STATUS_OUT_OF_MEMORY;
    uint64_t failing_number = 1;
    for (; status != SXML_STATUS_OK && failing_number < 100000; failing_number += 1) {
        status = run_with_failure(state, failing_number, text);
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK || status == SXML_STATUS_OUT_OF_MEMORY);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            break;
        }
    }
    SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, failing_number > 10);
}

static const sxml_test_case xpath_test_cases[] = {
    {.name = "slice_queries_round_trip", .function = test_slice_queries_round_trip},
    {.name = "static_context_binds_namespaces_and_variables",
     .function = test_static_context_binds_namespaces_and_variables},
    {.name = "static_base_uri_reaches_functions",
     .function = test_static_base_uri_reaches_functions},
    {.name = "context_value_is_a_parsed_document",
     .function = test_context_value_is_a_parsed_document},
    {.name = "doc_is_stable_and_resolves_through_the_resolver",
     .function = test_doc_is_stable_and_resolves_through_the_resolver},
    {.name = "available_documents_are_the_bound_nodes",
     .function = test_available_documents_are_the_bound_nodes},
    {.name = "clock_is_read_once_per_evaluation",
     .function = test_clock_is_read_once_per_evaluation},
    {.name = "evaluations_nest", .function = test_evaluations_nest},
    {.name = "function_items_outlive_their_expression",
     .function = test_function_items_outlive_their_expression},
    {.name = "limits_give_limit_exceeded", .function = test_limits_give_limit_exceeded},
    {.name = "step_bound_stops_long_evaluations",
     .function = test_step_bound_stops_long_evaluations},
    {.name = "default_limits_allow_deep_input", .function = test_default_limits_allow_deep_input},
    {.name = "sequence_types_parse_from_text",
     .function = test_sequence_types_parse_from_text},
    {.name = "expression_lifecycle_checks_its_arguments",
     .function = test_expression_lifecycle_checks_its_arguments},
    {.name = "compile_and_evaluate_survive_every_allocation_failure",
     .function = test_compile_and_evaluate_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xpath",
            .cases = xpath_test_cases,
            .case_count = SXML_TEST_COUNT(xpath_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
