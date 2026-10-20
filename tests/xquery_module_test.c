/*
 * Tests for library modules and module import (XQuery 4.0, 5.2, 5.12;
 * docs/DECISIONS.md, D106, D148; packet 3.11) through the public API: a
 * resolver serves modules by location hint or, for an import without
 * hints, by namespace, and counts its requests. Imports of functions,
 * variables and item types, %private, the namespace rule of 5.12.2,
 * non-transitive imports, cycles, a module reached twice, external
 * variables of a library module, the static errors, an error located in
 * a library module, and an allocation-failure loop (D23). Every case
 * checks for leaked cells and allocations.
 *
 * Golden file: tests/xquery_test.c.
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
    MODULE("urn:a", "http://example.com/m/a.xq",
           "module namespace a = 'urn:a'; "
           "declare variable $a:base := 10; "
           "declare %private variable $a:hidden := 1; "
           "declare function a:add($x) { $x + $a:base + a:helper() }; "
           "declare %private function a:helper() { $a:hidden }; "
           "declare type a:small as xs:integer;"),
    /* Imports b, which is not visible to a's importers. */
    MODULE("urn:c", "http://example.com/m/c.xq",
           "module namespace c = 'urn:c'; "
           "import module namespace b = 'urn:b' at 'b.xq'; "
           "declare function c:twice($x) { b:double($x) };"),
    MODULE("urn:b", "http://example.com/m/b.xq",
           "module namespace b = 'urn:b'; declare function b:double($x) { 2 * $x };"),
    /* A cycle: even and odd call each other across modules. */
    MODULE("urn:even", "http://example.com/m/even.xq",
           "module namespace e = 'urn:even'; "
           "import module namespace o = 'urn:odd' at 'odd.xq'; "
           "declare function e:is-even($n) { if ($n eq 0) then true() else o:is-odd($n - 1) };"),
    MODULE("urn:odd", "http://example.com/m/odd.xq",
           "module namespace o = 'urn:odd'; "
           "import module namespace e = 'urn:even' at 'even.xq'; "
           "declare function o:is-odd($n) { if ($n eq 0) then false() else e:is-even($n - 1) };"),
    /* A static context of its own: decimal formats, default collation
       and namespaces its functions read when they run (5.2, D275). */
    MODULE("urn:own", "http://example.com/m/own.xq",
           "module namespace w = 'urn:own'; "
           "declare default collation "
           "'http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive'; "
           "declare decimal-format w:de decimal-separator = ',' grouping-separator = '.'; "
           "declare function w:show($n) { format-number($n, '#.###,##', 'w:de') }; "
           "declare function w:same($a, $b) { $a eq $b and compare($a, $b) eq 0 }; "
           "declare function w:base() { static-base-uri#0 };"),
    /* Two modules of one namespace (5.12.2). */
    MODULE("urn:two", "http://example.com/m/two-1.xq",
           "module namespace t = 'urn:two'; declare function t:one() { 1 };"),
    MODULE("urn:two", "http://example.com/m/two-2.xq",
           "module namespace t = 'urn:two'; declare function t:two() { 2 };"),
    /* Found by namespace only (an import without hints). */
    MODULE("urn:nohint", "",
           "module namespace n = 'urn:nohint'; declare variable $n:value external := 'default';"),
    /* One node made once, read through two importers. */
    MODULE("urn:node", "http://example.com/m/node.xq",
           "module namespace n = 'urn:node'; declare variable $n:node := <n/>;"),
    MODULE("urn:reader", "http://example.com/m/reader.xq",
           "module namespace r = 'urn:reader'; "
           "import module namespace n = 'urn:node' at 'node.xq'; "
           "declare function r:node() { $n:node };"),
    /* Variables whose values depend on each other across modules. */
    MODULE("urn:loop", "http://example.com/m/loop.xq",
           "module namespace l = 'urn:loop'; "
           "import module namespace m = 'urn:main-side' at 'side.xq'; "
           "declare variable $l:x := $m:y;"),
    MODULE("urn:main-side", "http://example.com/m/side.xq",
           "module namespace m = 'urn:main-side'; "
           "import module namespace l = 'urn:loop' at 'loop.xq'; "
           "declare variable $m:y := $l:x;"),
    /* Errors. */
    MODULE("urn:wrong", "http://example.com/m/wrong.xq",
           "module namespace w = 'urn:not-wrong'; declare function w:f() { 1 };"),
    MODULE("urn:main", "http://example.com/m/main.xq", "1 + 1"),
    MODULE("urn:outside", "http://example.com/m/outside.xq",
           "module namespace o = 'urn:outside'; declare function local:f() { 1 };"),
    MODULE("urn:output", "http://example.com/m/output.xq",
           "module namespace p = 'urn:output'; declare option output:method 'text';"),
    MODULE("urn:collide", "http://example.com/m/collide.xq",
           "module namespace k = 'urn:collide'; declare variable $k:v := 1; "
           "declare function k:f() { 1 };"),
    MODULE("urn:broken", "http://example.com/m/broken.xq",
           "module namespace x = 'urn:broken';\ndeclare function x:f() { 1 + };"),
};

#define SERVED_MODULE_COUNT (sizeof(served_modules) / sizeof(served_modules[0]))

/* The test resolver's state: how many requests it answered. */
typedef struct module_resolver {
    size_t request_count;
} module_resolver;

/* An import of urn:two without hints: the locations of both of its
   modules, as a SEQUENCE of xs:anyURI items (include/sxml/processor.h). */
static sxml_status list_modules(const sxml_resource_request *request, sxml_resource *resource) {
    static const sxml_byte_view locations[2] = {
        SXML_BYTE_VIEW_INITIALIZER("http://example.com/m/two-1.xq"),
        SXML_BYTE_VIEW_INITIALIZER("http://example.com/m/two-2.xq")};
    sxml_item_heap *item_heap = sxml_processor_get_item_heap(request->processor);
    sxml_item items[2] = {{.representation = 0}, {.representation = 0}};
    sxml_status status = SXML_STATUS_OK;
    for (size_t location_index = 0; status == SXML_STATUS_OK && location_index < 2;
         location_index += 1) {
        sxml_string string = {.utf8 = locations[location_index],
                              .codepoint_count = 0,
                              .is_ascii = true};
        status = sxml_string_from_utf8(locations[location_index], &string);
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_make(
                item_heap,
                &(sxml_atomic_item_request){.type = {.index = SXML_TYPE_XS_ANY_URI},
                                            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                                                      .datum_details = {.string = string}}},
                &items[location_index]);
        }
    }
    sxml_sequence listed = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            item_heap, (sxml_item_view){.items = items, .item_count = 2}, &listed);
    }
    sxml_item_release(items[0]);
    sxml_item_release(items[1]);
    if (status == SXML_STATUS_OK) {
        resource->resource_form = SXML_RESOURCE_FORM_SEQUENCE;
        resource->items = listed;
    }
    return status;
}

static sxml_status resolve_module(void *state, const sxml_resource_request *request,
                                  sxml_resource *resource) {
    module_resolver *resolver = state;
    if (request->resource_kind != SXML_RESOURCE_KIND_XQUERY_MODULE) {
        return SXML_STATUS_OK;
    }
    resolver->request_count += 1;
    if (request->uri_reference.byte_count == 0 &&
        sxml_byte_view_equals(request->target_namespace_uri, TEXT("urn:two"))) {
        return list_modules(request, resource);
    }
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

static const query_case query_cases[] = {
    /* Functions, variables and item types of an imported module, its
       private ones used inside it. */
    QUERY_VALUE("function-and-variable",
                "import module namespace a = 'urn:a' at 'a.xq'; a:add(5), $a:base",
                "16 10"),
    QUERY_VALUE("own-static-context",
                "import module namespace w = 'urn:own' at 'own.xq'; "
                "w:show(1234.5), w:same('a', 'A'), 'a' eq 'A'",
                "1.234,5 true false"),
    /* A reference to a context-dependent function keeps its module's
       static context (XPath 4.0, 4.5.4; D275). */
    QUERY_VALUE("context-dependent-reference",
                "import module namespace w = 'urn:own' at 'own.xq'; "
                "w:base()(), static-base-uri#0() = w:base()()",
                "http://example.com/m/own.xq false"),
    QUERY_VALUE("item-type", "import module namespace a = 'urn:a' at 'a.xq'; 3 instance of a:small",
                "true"),
    QUERY_VALUE("prefix-of-the-main-module",
                "import module 'urn:a' at 'a.xq'; declare namespace x = 'urn:a'; x:add(1)", "12"),
    QUERY_VALUE("function-reference",
                "import module namespace a = 'urn:a' at 'a.xq'; (1, 2) ! a:add#1(.)", "12 13"),
    /* The modules a module imports serve it, not its importers (5.12). */
    QUERY_VALUE("import-of-an-import",
                "import module namespace c = 'urn:c' at 'c.xq'; c:twice(21)", "42"),
    QUERY_ERROR("imports-are-not-transitive",
                "import module namespace c = 'urn:c' at 'c.xq'; declare namespace b = 'urn:b'; "
                "b:double(1)",
                "XPST0017"),
    QUERY_ERROR("private-function", "import module namespace a = 'urn:a' at 'a.xq'; a:helper()",
                "XPST0017"),
    QUERY_ERROR("private-variable", "import module namespace a = 'urn:a' at 'a.xq'; $a:hidden",
                "XPST0008"),
    /* Cyclic imports (5.12.4). */
    QUERY_VALUE("cycle", "import module namespace e = 'urn:even' at 'even.xq'; e:is-even(10), "
                         "e:is-even(7)",
                "true false"),
    /* Every module of an imported namespace (5.12.2). */
    QUERY_VALUE("two-modules-of-one-namespace",
                "import module namespace t = 'urn:two' at 'two-1.xq', 'two-2.xq'; t:one() + "
                "t:two()",
                "3"),
    /* Without hints, a resolver may list a namespace's modules. */
    QUERY_VALUE("listed-modules-of-one-namespace",
                "import module namespace t = 'urn:two'; t:one() + t:two()", "3"),
    QUERY_VALUE("import-without-hints", "import module namespace n = 'urn:nohint'; $n:value",
                "default"),
    /* One module reached twice is one module: its variable is one node. */
    QUERY_VALUE("module-reached-twice",
                "import module namespace n = 'urn:node' at 'node.xq'; import module namespace r "
                "= 'urn:reader' at 'reader.xq'; $n:node is r:node()",
                "true"),
    QUERY_ERROR("variables-depend-on-each-other-across-modules",
                "import module namespace l = 'urn:loop' at 'loop.xq'; $l:x", "XQDY0054"),
    /* Static errors. */
    QUERY_ERROR("no-module", "import module namespace z = 'urn:none' at 'none.xq'; 1",
                "XQST0059"),
    QUERY_ERROR("no-module-without-hints", "import module namespace z = 'urn:none'; 1",
                "XQST0059"),
    QUERY_ERROR("wrong-namespace", "import module namespace w = 'urn:wrong' at 'wrong.xq'; 1",
                "XQST0059"),
    QUERY_ERROR("main-module-delivered",
                "import module namespace w = 'urn:main' at 'main.xq'; 1", "XQST0059"),
    QUERY_ERROR("empty-namespace", "import module namespace w = '' at 'a.xq'; 1", "XQST0088"),
    QUERY_ERROR("namespace-imported-twice",
                "import module namespace a = 'urn:a' at 'a.xq'; import module namespace b = "
                "'urn:a'; 1",
                "XQST0047"),
    QUERY_ERROR("xmlns-prefix", "import module namespace xmlns = 'urn:a' at 'a.xq'; 1",
                "XQST0070"),
    QUERY_ERROR("prefix-bound-twice",
                "declare namespace a = 'urn:x'; import module namespace a = 'urn:a' at 'a.xq'; 1",
                "XQST0033"),
    QUERY_ERROR("public-name-outside-the-target-namespace",
                "import module namespace o = 'urn:outside' at 'outside.xq'; 1", "XQST0048"),
    QUERY_ERROR("output-declaration-in-a-library-module",
                "import module namespace p = 'urn:output' at 'output.xq'; 1", "XQST0108"),
    QUERY_ERROR("variable-declared-and-imported",
                "import module namespace k = 'urn:collide' at 'collide.xq'; declare variable "
                "$k:v := 2; 1",
                "XQST0049"),
    QUERY_ERROR("function-declared-and-imported",
                "import module namespace k = 'urn:collide' at 'collide.xq'; declare function "
                "k:f() { 2 }; 1",
                "XQST0034"),
    QUERY_ERROR("syntax-error-in-a-library-module",
                "import module namespace x = 'urn:broken' at 'broken.xq'; 1", "XPST0003"),
};

static void test_modules_import(sxml_test_state *state) {
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
                                                              .expression = current->name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* An import without hints asks once, with an empty URI reference; a hint
   asks once per hint; a module of a namespace already loaded needs no
   request without hints. */
static void test_resolver_requests(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    query_run run = {.expression_text = TEXT("import module namespace n = 'urn:nohint'; 1")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_fixture.resolver.request_count == 1);
    test_fixture.resolver.request_count = 0;
    run.expression_text =
        TEXT("import module namespace t = 'urn:two' at 'two-1.xq', 'two-2.xq'; 1");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_fixture.resolver.request_count == 2);
    /* even imports odd, which imports even again: three requests, two
       modules. */
    test_fixture.resolver.request_count = 0;
    run.expression_text = TEXT("import module namespace e = 'urn:even' at 'even.xq'; 1");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_fixture.resolver.request_count == 3);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A library module's external variable is bound by the host's binding
   (include/sxml/xquery.h). */
static void test_library_external_variable_is_bound(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(test_fixture.processor);
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_name_identifier name = {.index = 0};
    SXML_TEST_EXPECT(state, sxml_name_pool_intern_namespace_uri(name_pool, TEXT("urn:nohint"),
                                                                &namespace_uri) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state,
                     sxml_name_pool_intern_name(
                         name_pool,
                         &(sxml_name_request){.namespace_uri = namespace_uri,
                                              .local_name = TEXT("value")},
                         &name) == SXML_STATUS_OK);
    const sxml_variable_binding bindings[] = {
        {.variable_name = name, .value = sxml_sequence_from_item(sxml_atomic_item_from_integer(7))},
    };
    query_run run = {
        .expression_text = TEXT("import module namespace n = 'urn:nohint'; $n:value"),
        .dynamic_context = {.variable_bindings = {.variable_bindings = bindings,
                                                  .variable_binding_count = 1}}};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("7")));
    sxml_sequence_release(bindings[0].value);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A static error in a library module is located in it: its module URI
   is the source name. */
static void test_error_in_a_library_module_names_it(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    query_run run = {
        .expression_text = TEXT("import module namespace x = 'urn:broken' at 'broken.xq'; 1")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(run.error_record.diagnostic.location.source_name,
                                                  TEXT("http://example.com/m/broken.xq")));
    SXML_TEST_EXPECT(state, run.error_record.diagnostic.location.line_number == 2);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------- Allocation failures */

static sxml_status run_with_failure(sxml_test_state *state, uint64_t failing_number) {
    fixture test_fixture;
    sxml_status status = fixture_create(failing_number, &test_fixture);
    if (status == SXML_STATUS_OK) {
        query_run run = {
            .expression_text = TEXT("import module namespace e = 'urn:even' at 'even.xq'; "
                                    "import module namespace a = 'urn:a' at 'a.xq'; "
                                    "e:is-even(4), a:add(1)")};
        status = run_query(&test_fixture, &run);
        if (status == SXML_STATUS_OK) {
            SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("true 12")));
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

static const sxml_test_case xquery_module_test_cases[] = {
    {.name = "modules_import", .function = test_modules_import},
    {.name = "resolver_requests", .function = test_resolver_requests},
    {.name = "library_external_variable_is_bound",
     .function = test_library_external_variable_is_bound},
    {.name = "error_in_a_library_module_names_it",
     .function = test_error_in_a_library_module_names_it},
    {.name = "compile_survives_every_allocation_failure",
     .function = test_compile_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery_module",
            .cases = xquery_module_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_module_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
