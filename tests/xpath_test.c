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
    /* Text resources (packet 2.12): lines ending in CR LF, CR and LF. */
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/docs/lines.txt"),
     .content = SXML_BYTE_VIEW_INITIALIZER("one\r\ntwo\rthree\n")},
    /* "hi" in UTF-16LE with a byte order mark. */
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/docs/utf16.txt"),
     .content = {.bytes = (const uint8_t *)"\xFF\xFEh\0i\0", .byte_count = 6}},
    /* Not UTF-8: a lone continuation byte. */
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/docs/latin1.txt"),
     .content = SXML_BYTE_VIEW_INITIALIZER("caf\xE9")},
    /* U+0001 is not an XML character. */
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/docs/control.txt"),
     .content = SXML_BYTE_VIEW_INITIALIZER("a\x01")},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("http://example.com/docs/data.json"),
     .content = SXML_BYTE_VIEW_INITIALIZER("{\"a\": [1, 2]}")},
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

/* Compiles and evaluates run->expression_text; writes the value, owned, to
   *result. The expression is destroyed before returning. */
static sxml_status evaluate_query(const fixture *test_fixture, query_run *run,
                                  sxml_sequence *result) {
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
    status = sxml_xpath_expression_evaluate(
        expression,
        &(sxml_xpath_evaluation_request){.dynamic_context = run->dynamic_context,
                                         .error_record = &run->error_record},
        result);
    sxml_xpath_expression_destroy(expression);
    return status;
}

/* Compiles and evaluates run->expression_text and writes the result by the
   adaptive method into the fixture's output buffer (cleared first). */
static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_sequence result = sxml_sequence_empty();
    sxml_status status = evaluate_query(test_fixture, run, &result);
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

/* The functions packet 1.23b added (D91), through the public API, with
   examples from F&O 4.0. */
static const slice_case library_cases[] = {
    SLICE_VALUE("math", "math:pi(), math:pow(2, 10), math:pow(-2, 3), math:pow(0, 0), "
                        "math:sqrt(-0e0), math:log(0), math:atan2(1, 0) * 2 eq math:pi(), "
                        "math:exp10(2), math:pow((), 2)",
                "3.141592653589793e0 1.024e3 -8.0e0 1.0e0 -0.0e0 xs:double(\"-INF\") true() "
                "1.0e2"),
    SLICE_VALUE("math-pow-parity", "math:pow(-1, 9007199254740993)", "-1.0e0"),
    SLICE_VALUE("qname-parts",
                "let $q := QName('http://a', 'p:l') return (local-name-from-QName($q), "
                "prefix-from-QName($q), namespace-uri-from-QName($q), expanded-QName($q), "
                "prefix-from-QName(QName('', 'l')), expanded-QName(QName('', 'l')))",
                "\"l\" \"p\" \"http://a\" \"Q{http://a}l\" \"Q{}l\""),
    SLICE_VALUE("in-scope-namespaces",
                "let $e := parse-xml('<z:a xmlns=\"http://one\" xmlns:z=\"http://two\"><b "
                "xmlns=\"\"/></z:a>')/* return (sort(in-scope-prefixes($e)), "
                "in-scope-namespaces($e)?z, namespace-uri-for-prefix('z', $e), "
                "namespace-uri-for-prefix((), $e), count(namespace-uri-for-prefix('', $e/b)))",
                "\"\" \"xml\" \"z\" \"http://two\" \"http://two\" \"http://one\" 0"),
    SLICE_VALUE("node-properties",
                "let $d := parse-xml('<a xml:base=\"http://b/c/\"><e/></a>') return "
                "(base-uri($d//e), has-children($d/a), has-children($d//e), "
                "count(document-uri($d)))",
                "\"http://b/c/\" true() false() 0"),
    SLICE_VALUE("strings",
                "translate('--aaa--', 'abc-', 'ABC'), substring-before-last('a.b.c', '.'), "
                "substring-after-last('a.b.c', '.'), codepoint-equal('a', 'a'), "
                "contains-token('red green blue ', ' green'), contains-token('a', ' ')",
                "\"AAA\" \"a.b\" \"c\" true() true() false()"),
    SLICE_VALUE("uri-escaping",
                "encode-for-uri('a b/~'), iri-to-uri('http://a/b c{}'), "
                "escape-html-uri('a b<é')",
                "\"a%20b%2F~\" \"http://a/b%20c%7B%7D\" \"a b<%C3%A9\""),
    SLICE_VALUE("slice", "slice(1 to 5, 2, 4), slice(1 to 5, -2), slice(1 to 5, step := -2)",
                "2 3 4 4 5 5 3 1"),
    SLICE_VALUE("sequences",
                "items-at(('a', 'b', 'c'), (3, 1, 7)), replicate((1, 2), 2), "
                "insert-separator(1 to 3, 0), foot(1 to 4), trunk(1 to 3), void(1 to 9), "
                "identity(7), unordered(8)",
                "\"c\" \"a\" 1 2 1 2 1 0 2 0 3 4 1 2 7 8"),
    SLICE_VALUE("comparisons",
                "compare('a', 'b'), compare(2, 1.0), compare(xs:double('NaN'), 1), "
                "atomic-equal(1, 1.0), all-equal((1, 1.0, 1e0)), all-different((1, 2, 1)), "
                "default-collation()",
                "-1 1 -1 true() true() false() "
                "\"http://www.w3.org/2005/xpath-functions/collation/codepoint\""),
    SLICE_ERROR("compare-families", "compare(1, 'a')", "XPTY0004"),
    SLICE_VALUE("more",
                "duplicate-values((1, 2, 1, 1.0, 3, 2)), "
                "dateTime(xs:date('1999-12-31Z'), xs:time('24:00:00')), "
                "trim-space('  a b  '), trim-space('  a ', {'side': 'start'}), "
                "parse-integer(' -1_000 '), parse-integer('FFFF FFFF', 16), "
                "parse-integer('zzzzzzzzzzzzzzzzzzzz', 36), "
                "let $d := parse-xml('<a><b/></a>') return "
                "(generate-id($d//b) = generate-id($d/a/b), generate-id($d//b) = generate-id($d/a))",
                "1 2 xs:dateTime(\"1999-12-31T00:00:00Z\") \"a b\" \"a \" -1000 4294967295 "
                "13367494538843734067838845976575 true() false()"),
    /* Packet 2.19: JNode ids (F&O 4.0, 14.1.11), decimal and double zero,
       a computed constructor in XPath 4.0 (section 4.13). */
    SLICE_VALUE("sweep-2.19",
                "let $j := jtree({'a': 1, 'b': [1, 2]}) return "
                "(generate-id($j/a) = generate-id($j/a), generate-id($j/a) = generate-id($j/b), "
                "matches(generate-id($j/b/*[2]), '^[A-Za-z][A-Za-z0-9]*$')), "
                "0.0 eq 0e0, count(distinct-values((0.0, -0e0))), string(element #e {'t'}), "
                "count(reverse(1 to 100000000000)), 1 = reverse(1 to 100000000000), "
                "reverse(reverse(1 to 3)), reverse(1 to 3)[2], "
                "parse-json('\"a\\u0000\"', {'fallback': fn($s) { 'F' }}), "
                "deep-equal(fn($x) { fn($y) { $x + $y } }, fn($a) { fn($b) { $a + $b } }), "
                "deep-equal(fn($x) { $x }, fn($x) { $x + 1 }), "
                "deep-equal(concat('a', ?), concat('a', ?)), "
                "deep-equal(fn($x) { fn($y) { $x + $y } }(1), fn($x) { fn($y) { $x + $y } }(2))",
                "true() false() true() true() 1 \"t\" 100000000000 true() 1 2 3 2 \"aF\" "
                "true() false() true() false()"),
    SLICE_ERROR("dateTime-timezones",
                "dateTime(xs:date('1999-12-31Z'), xs:time('12:00:00+01:00'))", "FORG0008"),
    SLICE_ERROR("parse-integer-radix", "parse-integer('12', 37)", "FORG0011"),
    SLICE_ERROR("parse-integer-digits", "parse-integer('19', 8)", "FORG0012"),
    SLICE_VALUE("durations",
                "years-from-duration(xs:yearMonthDuration('P20Y15M')), "
                "months-from-duration(xs:yearMonthDuration('-P20Y18M')), "
                "days-from-duration(xs:dayTimeDuration('P3DT55H')), "
                "hours-from-duration(xs:dayTimeDuration('-P3DT10H')), "
                "minutes-from-duration(xs:dayTimeDuration('-P5DT12H30M')), "
                "seconds-from-duration(xs:dayTimeDuration('P3DT10H12.5S')), "
                "seconds-from-duration(xs:dayTimeDuration('-PT256S'))",
                "21 -6 5 -10 -30 12.5 -16"),
};

static void test_library_functions_of_packet_1_23b(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(library_cases); case_index += 1) {
        const slice_case *test = &library_cases[case_index];
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

/* The static base URI is lexical (XPath 4.0, section 2.2.1; D91): a
   function item keeps that of its definition when another expression with
   another static base URI calls it after its own evaluation ended. */
static void test_static_base_uri_is_lexical(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    query_run definition = {
        .expression_text = TEXT("function() { static-base-uri(), count(doc('works.xml')//employee) }"),
        .static_context = {.static_base_uri = TEXT("http://example.com/docs/")}};
    sxml_sequence function_item = sxml_sequence_empty();
    SXML_TEST_REQUIRE(state, evaluate_query(&test_fixture, &definition, &function_item) ==
                                 SXML_STATUS_OK);
    const sxml_variable_declaration declaration = {
        .variable_name = fixture_name(&test_fixture, TEXT("f")),
        .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                          .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}};
    const sxml_variable_binding binding = {.variable_name = declaration.variable_name,
                                           .value = function_item};
    query_run caller = {
        .expression_text = TEXT("$f(), static-base-uri()"),
        .static_context = {.static_base_uri = TEXT("http://other.org/"),
                           .variable_declarations = {.variable_declarations = &declaration,
                                                     .variable_declaration_count = 1}},
        .dynamic_context = {.variable_bindings = {.variable_bindings = &binding,
                                                  .variable_binding_count = 1}}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &caller) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("\"http://example.com/docs/\" 2 "
                                                          "\"http://other.org/\"")));
    sxml_sequence_release(function_item);
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
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("false() true() false()")));
    /* fn:doc-available loads what it finds, so fn:doc gives the same node. */
    run.expression_text = TEXT("doc-available('staff.xml'), "
                               "doc('staff.xml') is doc('http://example.com/docs/staff.xml'), "
                               "doc-available('broken.xml'), doc-available(())");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("true() true() false() false()")));
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
                               TEXT("xs:dateTimeStamp(\"2026-10-16T19:30:45.5+02:00\") "
                                    "xs:dayTimeDuration(\"PT2H\") true()")));
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
    /* A range compared with one integer is decided from its bounds
       (D115), so it returns at once; a quantifier still steps. */
    run.expression_text = TEXT("some $x in 1 to 100000000000 satisfies $x = -1");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_LIMIT_EXCEEDED);
    run.expression_text = TEXT("-1 = -100000000000 to -1");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("true()")));
    /* Library functions that loop without tasks charge what they read
       (D91): joining, sorting, distinct values, deep equality, counting a
       lazy sequence, regular expressions. */
    static const sxml_byte_view charged_expressions[] = {
        SXML_BYTE_VIEW_INITIALIZER("string-length(string-join(1 to 200000))"),
        SXML_BYTE_VIEW_INITIALIZER("count(sort(1 to 200000))"),
        SXML_BYTE_VIEW_INITIALIZER("count(distinct-values(1 to 200000))"),
        SXML_BYTE_VIEW_INITIALIZER("deep-equal(1 to 200000, 1 to 200000)"),
        SXML_BYTE_VIEW_INITIALIZER("matches(string-join(1 to 30000), 'x')"),
    };
    for (size_t expression_index = 0; expression_index < SXML_TEST_COUNT(charged_expressions);
         expression_index += 1) {
        run.expression_text = charged_expressions[expression_index];
        SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_LIMIT_EXCEEDED);
    }
    run.expression_text = TEXT("string-length(string-join(1 to 2000)), count(sort(1 to 2000)), "
                               "string-length(string-join(1 to 30000)), "
                               "matches(string-join(1 to 2000), 'x')");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("6893 2000 138894 false()")));
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
            SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("true() 3 \"b\" 2 \"one\" \"two\" \"three\" 1.0e0")));
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return status;
}

/* ------------------------------------------------- Packet 2.12 resources */

/* Runs text with the static base URI of the fixture's resources and
   checks its adaptive output, or the error code it raises ("!CODE"). */
static void expect_resource_query(sxml_test_state *state, fixture *test_fixture,
                                  const char *const pair[2]) {
    query_run run = {.expression_text = {.bytes = (const uint8_t *)pair[0],
                                         .byte_count = strlen(pair[0])},
                     .static_context = {.static_base_uri = TEXT("http://example.com/docs/")}};
    const sxml_status status = run_query(test_fixture, &run);
    const char *expected = pair[1];
    if (expected[0] == '!') {
        SXML_TEST_EXPECT(state, status != SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, error_code_is(&run, (sxml_byte_view){
                                                        .bytes = (const uint8_t *)expected + 1,
                                                        .byte_count = strlen(expected) - 1}));
    } else {
        SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
        SXML_TEST_EXPECT(state, output_is(test_fixture, (sxml_byte_view){
                                                            .bytes = (const uint8_t *)expected,
                                                            .byte_count = strlen(expected)}));
    }
}

/* fn:unparsed-text and its relatives decode what the resolver serves
   (F&O 4.0, section 17.1). */
static void test_text_resources_decode(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    static const char *const cases[][2] = {
        {"string-to-codepoints(unparsed-text('lines.txt'))",
         "111 110 101 13 10 116 119 111 13 116 104 114 101 101 10"},
        {"string-to-codepoints(unparsed-text('lines.txt', {'normalize-newlines': true()}))",
         "111 110 101 10 116 119 111 10 116 104 114 101 101 10"},
        {"unparsed-text-lines('lines.txt')", "\"one\" \"two\" \"three\""},
        {"unparsed-text('utf16.txt'), unparsed-text('utf16.txt', 'UTF-16')", "\"hi\" \"hi\""},
        {"unparsed-text('utf16.txt', 'utf-8')", "!FOUT1190"},
        {"unparsed-text('latin1.txt')", "!FOUT1200"},
        {"unparsed-text('latin1.txt', 'iso-8859-1')", "\"caf\xC3\xA9\""},
        {"unparsed-text('latin1.txt', {'fallback': true()})", "\"caf\xEF\xBF\xBD\""},
        {"unparsed-text('control.txt')", "!FOUT1190"},
        {"unparsed-text('lines.txt', 'no-such-encoding')", "!FOUT1190"},
        {"unparsed-text('missing.txt')", "!FOUT1170"},
        {"unparsed-text('lines.txt', {'fallback': 'yes'})", "!XPTY0004"},
        {"unparsed-text(())", ""},
        {"unparsed-text-available('lines.txt'), unparsed-text-available('missing.txt'), "
         "unparsed-text-available('latin1.txt'), unparsed-text-available(())",
         "true() false() false() false()"},
        {"string(unparsed-binary('utf16.txt'))", "\"//5oAGkA\""},
        {"unparsed-binary('missing.txt')", "!FOUT1170"},
        {"json-doc('data.json')?a?2 eq 2", "true()"},
        {"json-doc('missing.json')", "!FOUT1170"},
        {"json-doc('lines.txt')", "!FOJS0001"},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases); case_index += 1) {
        expect_resource_query(state, &test_fixture, cases[case_index]);
    }
    /* Without a static base URI a relative URI cannot be made absolute. */
    query_run run = {.expression_text = TEXT("unparsed-text('lines.txt')")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("FOUT1170")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A resolver of two collections for collection_resolve. */
typedef struct collection_resolver_state {
    size_t resolve_count;
} collection_resolver_state;

/* Serves "http://example.com/c" (two integers), the default collection
   (one string) and the URI collection of "http://example.com/c". */
static sxml_status collection_resolve(void *state, const sxml_resource_request *request,
                                      sxml_resource *resource) {
    collection_resolver_state *resolver_state = state;
    resolver_state->resolve_count += 1;
    sxml_item_heap *item_heap = sxml_processor_get_item_heap(request->processor);
    if (request->resource_kind == SXML_RESOURCE_KIND_COLLECTION &&
        request->is_default_collection) {
        resource->resource_form = SXML_RESOURCE_FORM_SEQUENCE;
        sxml_item item = {.representation = 0};
        const sxml_status status = sxml_atomic_item_make(
            item_heap,
            &(sxml_atomic_item_request){
                .type = {.index = SXML_TYPE_XS_STRING},
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = TEXT("default")}}}},
            &item);
        resource->items = sxml_sequence_from_item(item);
        return status;
    }
    if (!sxml_byte_view_equals(request->absolute_uri, TEXT("http://example.com/c"))) {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        return SXML_STATUS_OK;
    }
    resource->resource_form = SXML_RESOURCE_FORM_SEQUENCE;
    if (request->resource_kind == SXML_RESOURCE_KIND_URI_COLLECTION) {
        sxml_item item = {.representation = 0};
        const sxml_status status = sxml_atomic_item_make(
            item_heap,
            &(sxml_atomic_item_request){
                .type = {.index = SXML_TYPE_XS_ANY_URI},
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = TEXT("http://example.com/a")}}}},
            &item);
        resource->items = sxml_sequence_from_item(item);
        return status;
    }
    return sxml_sequence_make_integer_range(
        item_heap, (sxml_integer_range){.first_integer = 1, .last_integer = 2},
        &resource->items);
}

static const sxml_resource_resolver_operations collection_resolver_operations = {
    .resolve = collection_resolve,
    .release = NULL,
};

/* fn:collection and fn:uri-collection read the resolver once per URI in
   an evaluation (F&O 4.0, sections 17.1.3 and 17.1.4). */
static void test_collections_come_from_the_resolver(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    collection_resolver_state resolver_state = {.resolve_count = 0};
    query_run run = {
        .expression_text = TEXT("collection('c'), collection('http://example.com/c'), "
                                "collection(), uri-collection('c') instance of xs:anyURI"),
        .static_context = {.static_base_uri = TEXT("http://example.com/")},
        .dynamic_context = {.resource_resolver = {.state = &resolver_state,
                                                  .operations =
                                                      &collection_resolver_operations}}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("1 2 1 2 \"default\" true()")));
    SXML_TEST_EXPECT(state, resolver_state.resolve_count == 3);
    run.expression_text = TEXT("collection('other')");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("FODC0002")));
    /* The table resolver has no collections. */
    run.dynamic_context = (sxml_dynamic_context_configuration){.has_context_value = false};
    run.expression_text = TEXT("uri-collection()");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("FODC0002")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* The environment variables and default language of the dynamic context
   (XPath 4.0, section 2.2.2). */
static void test_environment_variables_and_default_language(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    query_run run = {.expression_text = TEXT(
                         "available-environment-variables(), environment-variable('HOME'), "
                         "empty(environment-variable('PATH')), default-language()")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("true() \"en\"")));
    const sxml_environment_variable variables[] = {
        {.variable_name = TEXT("HOME"), .variable_value = TEXT("/home/x")},
        {.variable_name = TEXT("SHELL"), .variable_value = TEXT("sh")},
    };
    run.dynamic_context = (sxml_dynamic_context_configuration){
        .default_language = TEXT("fr-CA"),
        .environment_variables = {.environment_variables = variables,
                                  .environment_variable_count = 2}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture,
                                      TEXT("\"HOME\" \"SHELL\" \"/home/x\" true() \"fr-CA\"")));
    run.expression_text = TEXT("default-language() instance of xs:language");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("true()")));
    /* Breaches of the dynamic context's rules. */
    run.dynamic_context.default_language = TEXT("not a language");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    run.dynamic_context.default_language = TEXT("en");
    const sxml_environment_variable duplicates[] = {
        {.variable_name = TEXT("A"), .variable_value = TEXT("1")},
        {.variable_name = TEXT("A"), .variable_value = TEXT("2")},
    };
    run.dynamic_context.environment_variables = (sxml_environment_variable_view){
        .environment_variables = duplicates, .environment_variable_count = 2};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* fn:trace and fn:message reach the trace sink: the dynamic context's,
   else the processor's (null: discarded); sxml_stream_trace_sink writes
   one line per event (include/sxml/processor.h). */
static void test_trace_sink_receives_trace_and_message(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    sxml_output_buffer *trace_buffer = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_output_buffer_create(
                          sxml_default_allocator(),
                          &(sxml_output_buffer_configuration){
                              .allocator = sxml_default_allocator(), .maximum_byte_count = 0},
                          &trace_buffer) == SXML_STATUS_OK);
    const sxml_output_stream trace_stream = sxml_output_buffer_as_output_stream(trace_buffer);
    query_run run = {.expression_text = TEXT("trace((1, 'a'), 'label'), message(('x', 2))")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("1 \"a\"")));
    run.dynamic_context.trace_sink = sxml_stream_trace_sink(&trace_stream);
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("1 \"a\"")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(sxml_output_buffer_get_bytes(trace_buffer),
                                                  TEXT("label: 1 \"a\"\n\"x\" 2\n")));
    /* A failing sink stops the evaluation with its status. */
    sxml_output_buffer_destroy(trace_buffer);
    SXML_TEST_REQUIRE(state,
                      sxml_output_buffer_create(
                          sxml_default_allocator(),
                          &(sxml_output_buffer_configuration){
                              .allocator = sxml_default_allocator(), .maximum_byte_count = 2},
                          &trace_buffer) == SXML_STATUS_OK);
    const sxml_output_stream small_stream = sxml_output_buffer_as_output_stream(trace_buffer);
    run.dynamic_context.trace_sink = sxml_stream_trace_sink(&small_stream);
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_LIMIT_EXCEEDED);
    sxml_output_buffer_destroy(trace_buffer);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* Decimal formats and collations of the static context are checked by
   compilation (include/sxml/xpath.h, D104). */
static void test_static_context_checks_decimal_formats_and_collations(
    sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(&(fixture_options){.failing_allocation_number = 0},
                                            &test_fixture) == SXML_STATUS_OK);
    const sxml_decimal_format_property good_properties[] = {
        {.property_kind = SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR,
         .property_value = TEXT(",")},
        {.property_kind = SXML_DECIMAL_FORMAT_PROPERTY_GROUPING_SEPARATOR,
         .property_value = TEXT(".")},
        {.property_kind = SXML_DECIMAL_FORMAT_PROPERTY_PERCENT, .property_value = TEXT("%:pc")},
    };
    const sxml_decimal_format formats[] = {
        {.format_name = {.index = 0},
         .decimal_format_properties = {.decimal_format_properties = good_properties,
                                       .decimal_format_property_count = 3}},
        {.format_name = fixture_name(&test_fixture, TEXT("other")),
         .decimal_format_properties = {.decimal_format_properties = good_properties + 2,
                                       .decimal_format_property_count = 1}},
    };
    const sxml_collation_binding bindings[] = {
        {.collation_uri = TEXT("http://example.com/my-collation"),
         .target_collation_uri =
             TEXT("http://www.w3.org/2005/xpath-functions/collation/codepoint")},
    };
    query_run run = {
        .expression_text = TEXT("format-number(1234.5, '#.##0,0'), format-number(0.5, '0%', "
                                "'other'), default-collation(), compare('a', 'b')"),
        .static_context = {.decimal_formats = {.decimal_formats = formats,
                                               .decimal_format_count = 2},
                           .default_collation_uri = TEXT("http://example.com/my-collation"),
                           .collation_bindings = {.collation_bindings = bindings,
                                                  .collation_binding_count = 1}}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture,
                                      TEXT("\"1.234,5\" \"50pc\" "
                                           "\"http://example.com/my-collation\" -1")));
    run.expression_text = TEXT("format-number(1, '0', 'unknown')");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, error_code_is(&run, TEXT("FODF1280")));
    run.expression_text = TEXT("1");
    /* Markers that are not distinct. */
    const sxml_decimal_format_property clashing_properties[] = {
        {.property_kind = SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR,
         .property_value = TEXT(",")},
    };
    const sxml_decimal_format clashing[] = {
        {.format_name = {.index = 0},
         .decimal_format_properties = {.decimal_format_properties = clashing_properties,
                                       .decimal_format_property_count = 1}},
    };
    run.static_context.decimal_formats =
        (sxml_decimal_format_view){.decimal_formats = clashing, .decimal_format_count = 1};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    /* A kind twice; two formats of one name. */
    const sxml_decimal_format_property twice[] = {good_properties[2], good_properties[2]};
    const sxml_decimal_format repeated[] = {
        {.format_name = {.index = 0},
         .decimal_format_properties = {.decimal_format_properties = twice,
                                       .decimal_format_property_count = 2}},
    };
    run.static_context.decimal_formats =
        (sxml_decimal_format_view){.decimal_formats = repeated, .decimal_format_count = 1};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    const sxml_decimal_format same_names[] = {formats[1], formats[1]};
    run.static_context.decimal_formats =
        (sxml_decimal_format_view){.decimal_formats = same_names, .decimal_format_count = 2};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    run.static_context.decimal_formats =
        (sxml_decimal_format_view){.decimal_formats = NULL, .decimal_format_count = 0};
    /* Unknown collations. */
    run.static_context.default_collation_uri = TEXT("http://example.com/unknown");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    run.static_context.default_collation_uri = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    const sxml_collation_binding bad_bindings[] = {
        {.collation_uri = TEXT("http://example.com/x"),
         .target_collation_uri = TEXT("http://example.com/unknown")},
    };
    run.static_context.collation_bindings = (sxml_collation_binding_view){
        .collation_bindings = bad_bindings, .collation_binding_count = 1};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_compile_and_evaluate_survive_every_allocation_failure(
    sxml_test_state *state) {
    const sxml_byte_view text = TEXT(
        "let $f := function($x) { $x + 1 } return ($f(1) eq 2, "
        "fold-left(1 to 2, 0, op('+')), parse-json('[\"a\",\"b\"]')?2, "
        "count(doc('works.xml')//employee), unparsed-text-lines('lines.txt'), "
        "json-doc('data.json')?a?1)");
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
    {.name = "static_base_uri_is_lexical", .function = test_static_base_uri_is_lexical},
    {.name = "library_functions_of_packet_1_23b",
     .function = test_library_functions_of_packet_1_23b},
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
    {.name = "text_resources_decode", .function = test_text_resources_decode},
    {.name = "collections_come_from_the_resolver",
     .function = test_collections_come_from_the_resolver},
    {.name = "environment_variables_and_default_language",
     .function = test_environment_variables_and_default_language},
    {.name = "trace_sink_receives_trace_and_message",
     .function = test_trace_sink_receives_trace_and_message},
    {.name = "static_context_checks_decimal_formats_and_collations",
     .function = test_static_context_checks_decimal_formats_and_collations},
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
