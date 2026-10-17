/*
 * Tests for include/sxml/xquery.h through the public API alone: queries
 * of the XQ40 test sets compiled, evaluated and serialized with the
 * parameters their prologs declare; output declarations over a parameter
 * document and their errors; external variables and the context value;
 * host declarations against the prolog; library modules; the lifecycle's
 * argument checks; and an allocation-failure loop on compile (D23). Every
 * case checks for leaked cells and allocations.
 *
 * Golden file: tests/xpath_test.c.
 */
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "sxml/xquery.h"
#include "test_runner.h"

#include <stdalign.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ---------------------------------------------------------------- Fixture */

static const char parameters_text[] =
    "<output:serialization-parameters "
    "xmlns:output='http://www.w3.org/2010/xslt-xquery-serialization'>"
    "<output:method value='xml'/><output:indent value='yes'/>"
    "<output:omit-xml-declaration value='no'/></output:serialization-parameters>";

static const char invalid_parameters_text[] =
    "<output:serialization-parameters "
    "xmlns:output='http://www.w3.org/2010/xslt-xquery-serialization'>"
    "<output:indent value='perhaps'/></output:serialization-parameters>";

static const sxml_resource_entry fixture_resources[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/q/parameters.xml"),
     .content = {.bytes = (const uint8_t *)parameters_text,
                 .byte_count = sizeof(parameters_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/q/invalid.xml"),
     .content = {.bytes = (const uint8_t *)invalid_parameters_text,
                 .byte_count = sizeof(invalid_parameters_text) - 1}},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/q/broken.xml"),
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

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
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
            .resource_resolver = sxml_table_resource_resolver(&fixture_resource_view)},
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

static sxml_item make_integer(const fixture *test_fixture, int64_t value) {
    sxml_item item = {.representation = 0};
    if (sxml_atomic_item_make(
            sxml_processor_get_item_heap(test_fixture->processor),
            &(sxml_atomic_item_request){.type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                                        .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                                                  .datum_details = {.integer = value}}},
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
    /* Out: the error record of the failure, and the declared parameters. */
    sxml_error_record error_record;
    sxml_serialization_parameters serialization_parameters;
} query_run;

/* Compiles and evaluates run->expression_text and serializes the result
   with the query's serialization parameters into the fixture's output
   buffer (cleared first). */
static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xquery_query *query = NULL;
    sxml_status status = sxml_xquery_query_create(
        fixture_allocator(test_fixture),
        &(sxml_xquery_query_configuration){.processor = test_fixture->processor,
                                           .expression_text = run->expression_text,
                                           .source_name = TEXT("test.xq"),
                                           .static_context = run->static_context,
                                           .error_record = &run->error_record},
        &query);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    run->serialization_parameters = sxml_xquery_query_get_serialization_parameters(query);
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xquery_query_evaluate(
        query,
        &(sxml_xquery_evaluation_request){.dynamic_context = run->dynamic_context,
                                          .error_record = &run->error_record},
        &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(test_fixture->processor),
            &(sxml_serialization_request){.value = result,
                                          .serialization_parameters =
                                              run->serialization_parameters,
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

/* ---------------------------------------------------------- Query cases */

/* A query in the manner of an XQ40 test case (named after it) and its
   output by the query's own serialization parameters, or the error code
   it raises. */
typedef struct query_case {
    const char *test_case_name;
    sxml_byte_view expression_text;
    sxml_byte_view expected_output;
    sxml_byte_view expected_error_code;
} query_case;

#define QUERY_VALUE(name, text, output)                                                   \
    {.test_case_name = (name),                                                           \
     .expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                \
     .expected_output = SXML_BYTE_VIEW_INITIALIZER(output),                              \
     .expected_error_code = {.bytes = NULL, .byte_count = 0}}

#define QUERY_ERROR(name, text, code)                                                     \
    {.test_case_name = (name),                                                           \
     .expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                \
     .expected_output = {.bytes = NULL, .byte_count = 0},                                \
     .expected_error_code = SXML_BYTE_VIEW_INITIALIZER(code)}

static const query_case query_cases[] = {
    QUERY_VALUE("FLWOR-1", "for $x in 1 to 3 let $y := $x * $x return $y", "1 4 9"),
    QUERY_VALUE("FLWOR-order", "for $x in (3, 1, 2) order by $x descending return $x", "3 2 1"),
    QUERY_VALUE("FLWOR-where-count",
                "for $x in 1 to 10 where $x mod 3 eq 0 count $n return $n * 100 + $x",
                "103 206 309"),
    QUERY_VALUE("group-by", "for $x in (1, 2, 3, 4) group by $odd := $x mod 2 order by $odd "
                            "return sum($x)",
                "6 4"),
    QUERY_VALUE("tumbling-window",
                "for tumbling window $w in 1 to 7 start at $s when $s mod 3 eq 1 "
                "return <w>{$w}</w>",
                "<w>1 2 3</w><w>4 5 6</w><w>7</w>"),
    QUERY_VALUE("typeswitch-1",
                "typeswitch (3.5) case xs:integer return 'int' case xs:decimal return 'dec' "
                "default return 'other'",
                "dec"),
    QUERY_VALUE("switch-1", "switch (2) case 1 return 'one' case 2 return 'two' default return "
                            "'many'",
                "two"),
    QUERY_VALUE("try-catch-1", "try { 1 div 0 } catch err:FOAR0001 { 'caught' }", "caught"),
    QUERY_VALUE("constructor-1", "<a b='{1 + 1}'>{'text', <c/>}</a>",
                "<a b=\"2\">text<c/></a>"),
    QUERY_VALUE("constructor-computed", "element e { attribute x { 'y' }, text { 'z' } }",
                "<e x=\"y\">z</e>"),
    QUERY_VALUE("string-constructor", "let $n := 3 return ``[n=`{$n}`!]``", "n=3!"),
    QUERY_VALUE("VarDecl-1", "declare variable $x := 7; declare variable $y := $x * 6; $y",
                "42"),
    QUERY_VALUE("FunctionDecl-1",
                "declare function local:fact($n as xs:integer) as xs:integer { if ($n le 1) "
                "then 1 else $n * local:fact($n - 1) }; local:fact(10)",
                "3628800"),
    QUERY_VALUE("FunctionDecl-defaults",
                "declare function local:greet($name, $greeting := 'Hello') { $greeting || ', ' "
                "|| $name }; local:greet('you'), local:greet('me', 'Hi')",
                "Hello, you Hi, me"),
    QUERY_VALUE("NamespaceDecl-1",
                "declare namespace p = 'http://example.com/p'; <p:a/> ! namespace-uri(.)",
                "http://example.com/p"),
    QUERY_VALUE("DefaultNamespaceDecl-1",
                "declare default element namespace 'http://example.com/d'; <a/>",
                "<a xmlns=\"http://example.com/d\"/>"),
    QUERY_VALUE("BoundarySpaceDecl-1", "declare boundary-space preserve; <a> <b/> </a>",
                "<a> <b/> </a>"),
    QUERY_VALUE("ContextValueDecl-1", "declare context value := 5; . * 2", "10"),
    QUERY_VALUE("item-type-decl",
                "declare namespace my = 'http://my'; declare type my:int as xs:integer; 3 "
                "instance of my:int",
                "true"),
    QUERY_ERROR("VarDecl-circular", "declare variable $a := $b; declare variable $b := $a; $a",
                "XQDY0054"),
    QUERY_ERROR("FunctionDecl-duplicate",
                "declare function local:f() { 1 }; declare function local:f() { 2 }; 1",
                "XQST0034"),
    QUERY_ERROR("Prolog-syntax", "declare variable $x := ; 1", "XPST0003"),
};

static void test_queries_round_trip(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < sizeof(query_cases) / sizeof(query_cases[0]);
         case_index += 1) {
        const query_case *current = &query_cases[case_index];
        query_run run = {.expression_text = current->expression_text};
        const sxml_status status = run_query(&test_fixture, &run);
        const bool passed = current->expected_error_code.byte_count > 0
                                ? status != SXML_STATUS_OK &&
                                      error_code_is(&run, current->expected_error_code)
                                : status == SXML_STATUS_OK &&
                                      output_is(&test_fixture, current->expected_output);
        (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                              .expression =
                                                                  current->test_case_name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------- Output declarations */

static void test_output_declarations_give_serialization_parameters(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    /* No declaration: XQuery's defaults (5.22.1), method xml. */
    query_run run = {.expression_text = TEXT("<a/>")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.serialization_parameters.serialization_method ==
                                SXML_SERIALIZATION_METHOD_XML);
    SXML_TEST_EXPECT(state, run.serialization_parameters.indent == SXML_SERIALIZATION_FLAG_DEFAULT);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("<a/>")));
    /* The predeclared output prefix and a rebound one. */
    run.expression_text = TEXT(
        "declare namespace s = 'http://www.w3.org/2010/xslt-xquery-serialization'; "
        "declare option output:method 'json'; declare option output:indent 'no'; "
        "declare option s:item-separator '|'; map { 'a': [1, true()] }");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.serialization_parameters.serialization_method ==
                                SXML_SERIALIZATION_METHOD_JSON);
    SXML_TEST_EXPECT(state, run.serialization_parameters.indent == SXML_SERIALIZATION_FLAG_NO);
    SXML_TEST_EXPECT(state, run.serialization_parameters.has_item_separator);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("{\"a\":[1,true]}")));
    /* Text and HTML methods; options outside the serialization namespace
       and unprefixed ones (the XQuery namespace) are ignored. */
    run.expression_text = TEXT("declare option output:method 'text'; declare option "
                               "local:anything 'x'; declare option unknown 'y'; <a>x<b>y</b></a>");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("xy")));
    run.expression_text = TEXT("declare option output:method 'html'; declare option "
                               "output:html-version '5'; <p>a<br/></p>");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.serialization_parameters.serialization_method ==
                                SXML_SERIALIZATION_METHOD_HTML);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("<p>a<br></p>")));
    /* QName lists take the default element namespace (D106). */
    run.expression_text = TEXT("declare default element namespace 'http://example.com/d'; "
                               "declare option output:cdata-section-elements 'a'; <a>x</a>");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     run.serialization_parameters.cdata_section_elements.element_name_count == 1);
    SXML_TEST_EXPECT(state, output_is(&test_fixture,
                                      TEXT("<a xmlns=\"http://example.com/d\"><![CDATA[x]]></a>")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_parameter_document_is_under_declarations(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    query_run run = {
        .expression_text = TEXT("declare option output:parameter-document 'parameters.xml'; "
                                "declare option output:indent 'no'; <a><b/></a>"),
        .static_context = {.static_base_uri = TEXT("http://example.com/q/query.xq")}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    /* indent from the declaration, omit-xml-declaration from the document. */
    SXML_TEST_EXPECT(state, run.serialization_parameters.indent == SXML_SERIALIZATION_FLAG_NO);
    SXML_TEST_EXPECT(state, run.serialization_parameters.omit_xml_declaration ==
                                SXML_SERIALIZATION_FLAG_NO);
    SXML_TEST_EXPECT(state,
                     output_is(&test_fixture, TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                                   "<a><b/></a>")));
    /* A base URI declaration moves the document's base. */
    run.static_context.static_base_uri = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    run.expression_text = TEXT("declare base-uri 'http://example.com/q/'; declare option "
                               "output:parameter-document 'parameters.xml'; 1");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.serialization_parameters.indent == SXML_SERIALIZATION_FLAG_YES);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_output_declaration_errors_are_static(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static const char *const cases[][2] = {
        {"declare option output:indent 'perhaps'; 1", "SEPM0016"},
        {"declare option output:method 'pdf'; 1", "SEPM0016"},
        {"declare option output:standalone 'maybe'; 1", "SEPM0016"},
        {"declare option output:encoding 'EBCDIC'; 1", "SESU0007"},
        {"declare option output:no-such-parameter 'x'; 1", "XQST0109"},
        {"declare option output:use-character-maps 'x'; 1", "XQST0109"},
        {"declare option output:indent 'yes'; declare option output:indent 'no'; 1", "XQST0110"},
        {"declare option output:parameter-document 'missing.xml'; 1", "XQST0119"},
        {"declare option output:parameter-document 'broken.xml'; 1", "XQST0119"},
        {"declare option output:parameter-document 'invalid.xml'; 1", "XQST0115"},
    };
    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); case_index += 1) {
        query_run run = {
            .expression_text = sxml_byte_view_from_c_string(cases[case_index][0]),
            .static_context = {.static_base_uri = TEXT("http://example.com/q/query.xq")}};
        const sxml_status status = run_query(&test_fixture, &run);
        (void)sxml_test_record_check(
            state,
            (sxml_test_check){
                .passed = status == SXML_STATUS_STATIC_ERROR &&
                          error_code_is(&run, sxml_byte_view_from_c_string(cases[case_index][1])) &&
                          run.error_record.diagnostic.location.line_number == 1,
                .expression = cases[case_index][0],
                .file_name = __FILE__,
                .line_number = __LINE__});
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------- Variables and context */

static void test_external_variables_and_context_value(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_variable_binding bindings[] = {
        {.variable_name = fixture_name(&test_fixture, TEXT("x")),
         .value = sxml_sequence_from_item(make_integer(&test_fixture, 20))},
    };
    query_run run = {
        .expression_text = TEXT("declare variable $x external; declare variable $y external := "
                                "1; declare context value as xs:integer external; $x + $y + ."),
        .dynamic_context = {.has_context_value = true,
                            .context_value =
                                sxml_sequence_from_item(make_integer(&test_fixture, 300)),
                            .variable_bindings = {.variable_bindings = bindings,
                                                  .variable_binding_count = 1}}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("321")));
    /* Without the context value: XPDY0002; of the wrong type: XPTY0004. */
    run.dynamic_context.has_context_value = false;
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XPDY0002")));
    run.expression_text = TEXT("declare variable $z external; $z");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XPDY0002")));
    /* A host declaration is in scope; declaring it again is XQST0049. */
    const sxml_variable_declaration declarations[] = {
        {.variable_name = bindings[0].variable_name,
         .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                           .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}},
    };
    run.static_context.variable_declarations = (sxml_variable_declaration_view){
        .variable_declarations = declarations, .variable_declaration_count = 1};
    run.expression_text = TEXT("$x * 2");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("40")));
    run.expression_text = TEXT("declare variable $x := 1; $x");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XQST0049")));
    sxml_sequence_release(bindings[0].value);
    sxml_sequence_release(run.dynamic_context.context_value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_library_module_is_not_a_query(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    query_run run = {.expression_text = TEXT("module namespace m = 'http://example.com/m'; "
                                             "declare function m:f() { 1 };")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("XPST0003")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------------ Lifecycle */

static void test_query_lifecycle_checks_its_arguments(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    SXML_TEST_EXPECT(state, sxml_xquery_query_memory_requirements(
                                &(sxml_xquery_query_configuration){.processor = NULL},
                                &requirements) == SXML_STATUS_INVALID_ARGUMENT);
    const sxml_xquery_query_configuration configuration = {
        .processor = test_fixture.processor, .expression_text = TEXT("1 + 1")};
    SXML_TEST_EXPECT(state, sxml_xquery_query_memory_requirements(&configuration, &requirements) ==
                                SXML_STATUS_OK);
    /* Initialize on caller memory. */
    alignas(max_align_t) uint8_t memory[512];
    SXML_TEST_EXPECT(state, requirements.byte_count <= sizeof(memory));
    sxml_xquery_query *query = NULL;
    SXML_TEST_EXPECT(state, sxml_xquery_query_initialize(
                                (sxml_memory_view){.bytes = memory, .byte_count = 1},
                                &configuration, &query) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state,
                     sxml_xquery_query_initialize(
                         (sxml_memory_view){.bytes = memory, .byte_count = sizeof(memory)},
                         &configuration, &query) == SXML_STATUS_OK);
    sxml_sequence result = sxml_sequence_empty();
    SXML_TEST_EXPECT(state, sxml_xquery_query_evaluate(query, NULL, &result) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_xquery_query_evaluate(query, &(sxml_xquery_evaluation_request){.error_record = NULL},
                                                &result) == SXML_STATUS_OK);
    sxml_sequence_release(result);
    sxml_xquery_query_finalize(query);
    sxml_xquery_query_finalize(NULL);
    sxml_xquery_query_destroy(NULL);
    /* Identifiers from no processor's pool. */
    const sxml_namespace_binding foreign[] = {
        {.namespace_prefix = {.index = 100000}, .namespace_uri = {.index = 100000}}};
    sxml_xquery_query_configuration invalid = configuration;
    invalid.static_context.namespace_bindings =
        (sxml_namespace_binding_view){.namespace_bindings = foreign, .namespace_binding_count = 1};
    SXML_TEST_EXPECT(state, sxml_xquery_query_create(fixture_allocator(&test_fixture), &invalid,
                                                     &query) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Compiles and runs one query with allocation failing_number failing. */
static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(failing_number, &test_fixture);
    if (status == SXML_STATUS_OK) {
        query_run run = {
            .expression_text = TEXT(
                "declare namespace p = 'http://example.com/p'; "
                "declare option output:parameter-document 'parameters.xml'; "
                "declare option output:indent 'no'; "
                "declare variable $n := 3; "
                "declare function local:twice($x) { 2 * $x }; "
                "<p:r>{for $i in 1 to $n return local:twice($i)}</p:r>"),
            .static_context = {.static_base_uri = TEXT("http://example.com/q/query.xq")}};
        status = run_query(&test_fixture, &run);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, output_is(&test_fixture,
                                              TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                                   "<p:r xmlns:p=\"http://example.com/p\">2 4 "
                                                   "6</p:r>")));
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

static void test_compile_survives_every_allocation_failure(sxml_test_state *state) {
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

static const sxml_test_case xquery_test_cases[] = {
    {.name = "queries_round_trip", .function = test_queries_round_trip},
    {.name = "output_declarations_give_serialization_parameters",
     .function = test_output_declarations_give_serialization_parameters},
    {.name = "parameter_document_is_under_declarations",
     .function = test_parameter_document_is_under_declarations},
    {.name = "output_declaration_errors_are_static",
     .function = test_output_declaration_errors_are_static},
    {.name = "external_variables_and_context_value",
     .function = test_external_variables_and_context_value},
    {.name = "library_module_is_not_a_query", .function = test_library_module_is_not_a_query},
    {.name = "query_lifecycle_checks_its_arguments",
     .function = test_query_lifecycle_checks_its_arguments},
    {.name = "compile_survives_every_allocation_failure",
     .function = test_compile_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery",
            .cases = xquery_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
