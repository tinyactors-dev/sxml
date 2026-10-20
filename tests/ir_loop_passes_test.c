/*
 * Tests for filter indexes (src/ir_loop_passes.c, src/evaluator_index.c;
 * docs/DECISIONS.md D224; packet perf-1), hoisting, and globals computed
 * on first read (src/ir_global_read.c, D234): which FILTERs and where clauses
 * get one, and that a FILTER with one gives the values and errors it gives
 * without: inputs of 16 items or more, filtered three times or more, so
 * the index is remembered, built and probed.
 *
 * Golden file: tests/xquery_flwor_test.c (fixture, golden IR text, values
 * written by the adaptive method, allocation-failure loops). Every case
 * checks that the counting allocator got every block back.
 */
#include "evaluator_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "test_runner.h"
#include "xquery_lower_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ Fixture */

typedef struct fixture {
    sxml_processor *processor;
    sxml_counting_allocator *counting_allocator;
    alignas(16) uint8_t counting_storage[256];
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.processor = NULL, .counting_allocator = NULL};
    sxml_status status = sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = test_fixture->counting_storage,
                           .byte_count = sizeof(test_fixture->counting_storage)},
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_create(
            sxml_default_allocator(),
            &(sxml_processor_configuration){.allocator = sxml_default_allocator()},
            &test_fixture->processor);
    }
    return status;
}

static void fixture_destroy(fixture *test_fixture) {
    sxml_processor_destroy(test_fixture->processor);
    sxml_counting_allocator_finalize(test_fixture->counting_allocator);
}

static bool fixture_released_everything(const fixture *test_fixture) {
    return sxml_counting_allocator_read_counts(test_fixture->counting_allocator)
               .live_allocation_count == 0;
}

/* What compiling (and evaluating) one query gave. */
typedef struct query_outcome {
    sxml_status status;
    sxml_error_record error_record;
    /* The compiled unit, owned; NULL after a failure. */
    sxml_ir_unit *unit;
    char text[8192];
} query_outcome;

/* Compiles query into outcome->unit. */
static sxml_status compile_query(fixture *test_fixture, const char *query, query_outcome *outcome) {
    sxml_processor *processor = test_fixture->processor;
    const sxml_xpath_static_context static_context = {
        .item_heap = processor->item_heap,
        .allocator = fixture_allocator(test_fixture),
        .function_table = sxml_processor_get_function_table(processor)};
    memset(&outcome->error_record, 0, sizeof(outcome->error_record));
    outcome->text[0] = '\0';
    outcome->unit = NULL;
    return sxml_xquery_compile(
        &(sxml_xpath_compile_request){
            .expression_text = {.bytes = (const uint8_t *)query, .byte_count = strlen(query)},
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xq"),
            .static_context = &static_context,
            .error_record = &outcome->error_record},
        &outcome->unit);
}

/* Compiles query and writes the unit's IR text to outcome. */
static void write_query_text(fixture *test_fixture, const char *query, query_outcome *outcome) {
    outcome->status = compile_query(test_fixture, query, outcome);
    sxml_ir_unit *unit = outcome->unit;
    if (outcome->status != SXML_STATUS_OK) {
        return;
    }
    size_t byte_count = 0;
    outcome->status =
        sxml_ir_unit_write_text(unit,
                                (sxml_memory_view){.bytes = (uint8_t *)outcome->text,
                                                   .byte_count = sizeof(outcome->text) - 1},
                                &byte_count);
    if (outcome->status == SXML_STATUS_OK) {
        outcome->text[byte_count] = '\0';
    }
    sxml_ir_unit_release(unit);
}

/* Writes value by the adaptive method into outcome->text. */
static sxml_status serialize_value(fixture *test_fixture, sxml_sequence value,
                                   query_outcome *outcome) {
    sxml_output_buffer *output_buffer = NULL;
    sxml_status status = sxml_output_buffer_create(
        fixture_allocator(test_fixture),
        &(sxml_output_buffer_configuration){.allocator = fixture_allocator(test_fixture),
                                            .maximum_byte_count = sizeof(outcome->text) - 1},
        &output_buffer);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            test_fixture->processor->item_heap,
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

/* Compiles and evaluates query; outcome->text is the value written, or
   "error:CODE" for an error with a code. */
static void evaluate_query(fixture *test_fixture, const char *query, query_outcome *outcome) {
    outcome->status = compile_query(test_fixture, query, outcome);
    sxml_ir_unit *unit = outcome->unit;
    sxml_dynamic_context *dynamic_context = NULL;
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_dynamic_context_create(
            fixture_allocator(test_fixture),
            &(sxml_evaluator_configuration){
                .item_heap = test_fixture->processor->item_heap,
                .allocator = fixture_allocator(test_fixture),
                .function_table = sxml_processor_get_function_table(test_fixture->processor)},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    /* Every global ABSENT: computed by its initializer on first read. */
    static sxml_evaluator_slot global_slots[8];
    memset(global_slots, 0, sizeof(global_slots));
    const size_t global_count =
        unit != NULL && unit->global_variable_count > 1 ? unit->global_variable_count - 1 : 0;
    if (outcome->status == SXML_STATUS_OK && global_count > SXML_TEST_COUNT(global_slots)) {
        outcome->status = SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit,
                                       .has_context_value = false,
                                       .context_value = sxml_sequence_empty(),
                                       .global_values = {.slots = global_slots,
                                                         .slot_count = global_count},
                                       .static_base_uri = {.bytes = NULL, .byte_count = 0},
                                       .error_record = &outcome->error_record},
            &value);
    }
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = serialize_value(test_fixture, value, outcome);
    }
    if (outcome->status != SXML_STATUS_OK && sxml_status_has_error_record(outcome->status)) {
        const sxml_byte_view code = outcome->error_record.code.local_name;
        (void)snprintf(outcome->text, sizeof(outcome->text), "error:%.*s", (int)code.byte_count,
                       (const char *)code.bytes);
    }
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_ir_unit_release(unit);
    sxml_item_heap_clear_raised_error(test_fixture->processor->item_heap);
}

/* ------------------------------------------------------- Golden text */

typedef struct golden_case {
    const char *query;
    const char *expected_text;
} golden_case;

static const golden_case golden_cases[] = {
    /* The key reads the focus value, the probe a global. */
    {"declare variable $c external; declare variable $v external; $c[@k = $v]",
     "(function 1 main (parameters 0) (captures) (slots 2)\n"
     "  (filter $global:1 (focus $1 _ _)\n"
     "    (let $2 (step $1 attribute attribute(k))\n"
     "      (binary general-equal $2 $global:2))\n"
     "    (index general-equal\n"
     "      (let $2 (step $1 attribute attribute(k))\n"
     "        $2)\n"
     "      $global:2)))\n"},
    /* Hoisting: the invariant LET leaves the loop, behind a guard. */
    {"declare variable $c external; declare variable $v external; "
     "for $x in $c let $y := $v + 1 return $x + $y",
     "(function 1 main (parameters 0) (captures) (slots 4)\n"
     "  (let $3 (quantified some $4 $global:1\n"
     "            true())\n"
     "    (let $2 (conditional $3\n"
     "              (binary add $global:2 1)\n"
     "              (sequence))\n"
     "      (map $global:1 (focus $1 _ _)\n"
     "        (binary add $1 $2)))))\n"},
    /* Hoisting from a quantified condition (D229): the variable is what
       the loop binds. */
    {"declare variable $c external; declare variable $v external; "
     "some $x in $c satisfies $x = $v + 1",
     "(function 1 main (parameters 0) (captures) (slots 4)\n"
     "  (let $3 (quantified some $4 $global:1\n"
     "            true())\n"
     "    (let $2 (conditional $3\n"
     "              (binary add $global:2 1)\n"
     "              (sequence))\n"
     "      (quantified some $1 $global:1\n"
     "        (binary general-equal $1 $2)))))\n"},
    /* A global with an initializer is read through a GLOBAL_READ (D234). */
    {"declare variable $v := 3; $v + 1",
     "(function 1 main (parameters 0) (captures) (slots 1) (focus $1 _ _)\n"
     "  (global-read $global:1\n"
     "    (binary add $global:1 1)))\n"
     "(function 2 anonymous (parameters 0) (captures) (slots 0)\n"
     "  3)\n"
     "(function 3 anonymous (parameters 0) (captures) (slots 0)\n"
     "  (static-call function:2#0))\n"},
    /* A where clause: the MAP runs over the FILTER of its input. */
    {"declare variable $c external; declare variable $v external; "
     "for $x in $c where $x/@k = $v return $x",
     "(function 1 main (parameters 0) (captures) (slots 6)\n"
     "  (let $6 (filter $global:1 (focus $1 _ _)\n"
     "            (let $3 (map $1 (focus $2 _ _)\n"
     "                      (step $2 attribute attribute(k)))\n"
     "              (let $4 (document-order $3)\n"
     "                (let $5 (binary general-equal $4 $global:2)\n"
     "                  (conditional $5\n"
     "                    true()\n"
     "                    false()))))\n"
     "            (index general-equal\n"
     "              (let $3 (map $1 (focus $2 _ _)\n"
     "                        (step $2 attribute attribute(k)))\n"
     "                (let $4 (document-order $3)\n"
     "                  $4))\n"
     "              $global:2))\n"
     "    (map $6 (focus $1 _ _)\n"
     "      (let $3 (map $1 (focus $2 _ _)\n"
     "                (step $2 attribute attribute(k)))\n"
     "        (let $4 (document-order $3)\n"
     "          (let $5 (binary general-equal $4 $global:2)\n"
     "            (conditional $5\n"
     "              $1\n"
     "              (sequence))))))))\n"},
};

static void test_queries_match_golden_text(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(golden_cases); case_index += 1) {
        static fixture test_fixture;
        static query_outcome outcome;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        write_query_text(&test_fixture, golden_cases[case_index].query, &outcome);
        const bool matches = outcome.status == SXML_STATUS_OK &&
                             strcmp(outcome.text, golden_cases[case_index].expected_text) == 0;
        if (!matches) {
            (void)fprintf(stderr, "query %s\nstatus %d\ngot:\n%s\n",
                          golden_cases[case_index].query, (int)outcome.status, outcome.text);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
        fixture_destroy(&test_fixture);
    }
}

/* ------------------------------------------------------ Which filters */

typedef struct shape_case {
    const char *query;
    bool is_indexed;
} shape_case;

#define SHAPE_PROLOG                                                                 \
    "declare variable $c external; declare variable $v external; "                   \
    "declare function local:plain($n) { $n/@k }; "                                   \
    "declare function local:making($n) { <a>{$n/@k}</a> }; "

static const shape_case shape_cases[] = {
    {SHAPE_PROLOG "$c[@k = $v]", true},
    {SHAPE_PROLOG "$c[$v = @k]", true},
    {SHAPE_PROLOG "$c[@k eq 'x']", true},
    {SHAPE_PROLOG "$c[. is $v]", true},
    /* A conjunct of "and", on either side. */
    {SHAPE_PROLOG "$c[@a and @k = $v]", true},
    {SHAPE_PROLOG "$c[@k = $v and @a]", true},
    /* An IR function whose body makes no nodes. */
    {SHAPE_PROLOG "$c[local:plain(.) = $v]", true},
    /* Not a necessary conjunct. */
    {SHAPE_PROLOG "$c[@a or @k = $v]", false},
    {SHAPE_PROLOG "$c[not(@k = $v)]", false},
    /* Not equality. */
    {SHAPE_PROLOG "$c[@k > $v]", false},
    {SHAPE_PROLOG "$c[@k != $v]", false},
    /* Both operands read the focus, or neither does. */
    {SHAPE_PROLOG "$c[@k = @j]", false},
    {SHAPE_PROLOG "$c[$v = 'x']", false},
    /* The position or size is read. */
    {SHAPE_PROLOG "$c[@k = $v and position() > 1]", false},
    {SHAPE_PROLOG "$c[@k = last()]", false},
    /* The key reads a variable bound outside the predicate. */
    {SHAPE_PROLOG "for $x in (1, 2) return $c[@k = $v and @j = $x]", true},
    {SHAPE_PROLOG "for $x in (1, 2) return $c[(@k, $x) = $v]", false},
    /* Nodes are made in the key or the probe. */
    {SHAPE_PROLOG "$c[local:making(.) = $v]", false},
    {SHAPE_PROLOG "$c[@k = <a>{$v}</a>]", false},
    /* A hoisted LET reads a slot of the guard. */
    {SHAPE_PROLOG "for $x in $c let $y := $v + 1 return $x + $y", false},
    /* A where clause of a for clause becomes an indexed FILTER. */
    {SHAPE_PROLOG "for $x in $c where $x/@k = $v return $x/@n", true},
    {SHAPE_PROLOG "for $x at $p in $c where $x/@k = $v return $p", false},
};

static void test_filters_are_indexed_by_shape(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(shape_cases); case_index += 1) {
        static fixture test_fixture;
        static query_outcome outcome;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        const shape_case *test_case = &shape_cases[case_index];
        write_query_text(&test_fixture, test_case->query, &outcome);
        const bool is_indexed = strstr(outcome.text, "(index ") != NULL;
        const bool matches = outcome.status == SXML_STATUS_OK && is_indexed == test_case->is_indexed;
        if (!matches) {
            (void)fprintf(stderr, "query %s\nstatus %d, indexed %d\n%s\n", test_case->query,
                          (int)outcome.status, (int)is_indexed, outcome.text);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
        fixture_destroy(&test_fixture);
    }
}

/* ---------------------------------------------------------- Values */

typedef struct value_case {
    const char *query;
    /* The value written by the adaptive method, or "error:CODE". */
    const char *expected;
} value_case;

static const value_case value_cases[] = {
    /* "=": untyped keys, string probes, the items in input order. */
    {"let $s := (1 to 40) ! <e k='{. mod 5}' n='{.}'/> "
     "return for $i in 0 to 3 return string-join($s[@k = string($i)]/@n, ',')",
     "\"5,10,15,20,25,30,35,40\" \"1,6,11,16,21,26,31,36\" "
     "\"2,7,12,17,22,27,32,37\" \"3,8,13,18,23,28,33,38\""},
    /* Keys of several values, probes of several values, an item once. */
    {"let $s := (1 to 30) ! <e t='a{. mod 3} b{. mod 2}'/> "
     "return for $t in ('a1', 'b0', 'zz', 'a1') return count($s[tokenize(@t) = $t])",
     "10 15 0 10"},
    {"let $s := (1 to 30) ! <e k='{. mod 4}'/> "
     "return for $i in 1 to 3 return count($s[@k = (string($i), '0', string($i))])",
     "15 15 14"},
    /* A numeric probe: untyped keys compare as numbers (4.11.2). */
    {"let $s := (1 to 20) ! <e n='{.}.0'/> "
     "return for $i in 1 to 3 return count($s[@n = $i])",
     "1 1 1"},
    /* Numeric keys and probes (D229): integers, decimals and doubles. */
    {"let $s := (1 to 40) ! <e n='{. mod 7}'/> "
     "return for $i in (0, 3, 3.0, 3e0, 9) return count($s[xs:integer(@n) = $i])",
     "5 6 6 6 0"},
    {"let $s := (1 to 30) ! xs:decimal(. div 4) "
     "return for $p in (0.5, 1, 2.25e0, 0.3) return count($s[. eq $p])",
     "1 1 1 0"},
    /* Integers beyond 2^53 share doubles: the predicate decides. */
    {"let $s := (1 to 20) ! (9007199254740992 + .) "
     "return for $p in (9007199254740993, 9007199254740994, 1) return count($s[. = $p])",
     "1 1 0"},
    /* Floats compare with decimals by exact value: 0.1 is not
       xs:float(0.1). NaN is not indexed: the same values without. */
    {"let $s := ((1 to 20) ! xs:float(. div 8), xs:float(0.1)) "
     "return for $p in (0.25, 0.1, xs:float(0.1), 7) return count($s[. = $p])",
     "1 0 1 0"},
    {"let $s := ((1 to 20), xs:double('NaN')) "
     "return for $p in (xs:double('NaN'), 5, 5) return count($s[. = $p])",
     "0 1 1"},
    /* Numeric keys with a string probe are XPTY0004, as without. */
    {"let $s := (1 to 20) ! <e n='{.}'/> "
     "return for $i in (1, 'a') return count($s[xs:integer(@n) = $i])",
     "error:XPTY0004"},
    /* A key of no value never compares. */
    {"let $s := (1 to 20) ! <e>{if (. mod 2 = 0) then attribute k {.} else ()}</e> "
     "return for $i in (2, 3, 4, 2) return count($s[@k eq string($i)])",
     "1 0 1 1"},
    /* "eq" of a key of two values is XPTY0004, as without an index. */
    {"let $s := ((1 to 20) ! <e><k>{.}</k></e>, <e><k>1</k><k>2</k></e>) "
     "return for $i in 1 to 3 return count($s[k eq string($i)])",
     "error:XPTY0004"},
    /* "is". */
    {"let $s := (1 to 30) ! <e/> "
     "return for $i in (3, 7, 50, 3) return count($s[. is $s[$i]])",
     "1 1 0 1"},
    {"let $s := ((1 to 30) ! <e/>, 'x') "
     "return for $i in 1 to 3 return count($s[. is $s[$i]])",
     "error:XPTY0004"},
    /* A key that raises an error raises it, a guarded one does not. */
    {"let $s := ((1 to 20) ! <e v='{.}'/>, <e v='abc'/>) "
     "return for $i in 1 to 3 return count($s[xs:integer(@v) = $i])",
     "error:FORG0001"},
    {"let $s := ((1 to 20) ! <e v='{.}'/>, <e v='abc'/>) "
     "return for $i in 1 to 3 return count($s[@v castable as xs:integer and xs:integer(@v) = $i])",
     "1 1 1"},
    /* A probe that raises an error raises it. */
    {"let $s := (1 to 20) ! <e k='{.}'/> "
     "return for $i in 1 to 3 return count($s[@k = xs:integer('x' || $i)])",
     "error:FORG0001"},
    /* Keys and probes of types that do not compare are XPTY0004. */
    {"let $s := (1 to 20) ! <e n='{.}'/> "
     "return for $i in 1 to 3 return count($s[xs:integer(@n) = 'a'])",
     "error:XPTY0004"},
    /* A collation that is not the codepoint collation. */
    {"declare default collation "
     "'http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive'; "
     "let $s := (1 to 20) ! <e k='K{.}'/> "
     "return for $i in 1 to 3 return count($s[@k = 'k' || $i])",
     "1 1 1"},
    /* An input that changes every time. */
    {"for $i in 1 to 4 return count(((1 to 20 + $i) ! <e k='{. mod 2}'/>)[@k = '1'])",
     "11 11 12 12"},
    /* Hoisting: an invariant LET runs once, and not for an empty input. */
    {"for $x in () let $y := error() return $y", ""},
    {"for $x in (1, 2) let $y := error() return $x", "error:FOER0000"},
    {"let $v := 4 return for $x in 1 to 3 let $y := $v * 2 return $x + $y", "9 10 11"},
    {"let $v := 4 return (1 to 12)[. > $v + 5]", "10 11 12"},
    {"for $x at $i in (5, 6) let $y := $i * 2 let $z := $x + 1 return $y + $z", "8 11"},
    {"let $s := (1 to 3) ! <e n='{.}'/> return "
     "for $p in $s let $l := for $q in $s/@n where $q > $p/@n return $q return count($l)",
     "2 1 0"},
    /* An indexed FILTER tests its predicate on the candidates only: none
       here once the index is used (the third input), so the error of the
       invariant LET, which the predicate never runs then, is not raised
       (nothing is hoisted from an indexed FILTER). */
    {"let $s := (1 to 20) ! <e k='{.}'/> return for $i in 1 to 4 return "
     "count($s[let $y := (if ($i = 3) then error() else true()) return @k = 'none' and $y])",
     "0 0 0 0"},
    /* Hoisting from quantified conditions: once, not for an empty input,
       the first error as without. */
    {"let $n := 3 return (some $x in (1 to 5) satisfies $x = $n * 2 - 1, "
     "every $x in (1 to 5) satisfies $x < $n + 3, some $x in (1 to 5) satisfies $x > $n * 9)",
     "true() true() false()"},
    {"every $x in () satisfies (let $y := error() return $y)", "true()"},
    {"some $x in (1, 2) satisfies (let $y := error() return $x = 1)", "error:FOER0000"},
    /* Globals computed on first read (D234): one never read is never
       computed, one read in a loop once; an initializer's error is caught
       inside it, not around the read; a dynamic cycle is XQDY0054. */
    {"declare variable $e := error(); if (count(1 to 3) = 2) then $e else 5", "5"},
    {"declare variable $x := (1 to 3) ! (. * 2); for $i in 1 to 3 return $x[$i] + $x[1]",
     "4 6 8"},
    {"declare variable $e := error(xs:QName('err:X')); try { $e } catch * { 'c' }",
     "error:X"},
    {"declare variable $e := try { error(xs:QName('err:X')) } catch * { 'c' }; ($e, $e)",
     "\"c\" \"c\""},
    {"declare variable $f := function() { $g }; declare variable $g := $f(); $g",
     "error:XQDY0054"},
    /* A where clause. */
    {"let $s := (1 to 40) ! <e k='{. mod 4}' n='{.}'/> "
     "return for $i in 0 to 2 return "
     "string-join(for $e in $s where $e/@k = string($i) and $e/@n > 30 return $e/@n, ',')",
     "\"32,36,40\" \"33,37\" \"34,38\""},
};

static void test_queries_give_values(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(value_cases); case_index += 1) {
        static fixture test_fixture;
        static query_outcome outcome;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        const value_case *test_case = &value_cases[case_index];
        evaluate_query(&test_fixture, test_case->query, &outcome);
        const bool matches =
            (outcome.status == SXML_STATUS_OK || sxml_status_has_error_record(outcome.status)) &&
            strcmp(outcome.text, test_case->expected) == 0;
        if (!matches) {
            (void)fprintf(stderr, "query %s\nstatus %d, got %s\nexpected %s\n", test_case->query,
                          (int)outcome.status, outcome.text, test_case->expected);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
        fixture_destroy(&test_fixture);
    }
}

/* -------------------------------------------------- Allocation failure */

/* Each value case again, failing each allocation in turn until one run
   succeeds: every failure is OUT_OF_MEMORY and releases everything. */
static void test_queries_survive_every_allocation_failure(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(value_cases); case_index += 1) {
        const value_case *test_case = &value_cases[case_index];
        uint64_t failing_allocation_number = 1;
        for (;;) {
            static fixture test_fixture;
            static query_outcome outcome;
            SXML_TEST_REQUIRE(state, fixture_create(failing_allocation_number, &test_fixture) ==
                                         SXML_STATUS_OK);
            evaluate_query(&test_fixture, test_case->query, &outcome);
            SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
            fixture_destroy(&test_fixture);
            if (strcmp(outcome.text, test_case->expected) == 0) {
                break;
            }
            const bool is_out_of_memory = outcome.status == SXML_STATUS_OUT_OF_MEMORY;
            SXML_TEST_EXPECT(state, is_out_of_memory);
            if (!is_out_of_memory) {
                (void)fprintf(stderr, "query %s allocation %llu: status %d %s\n", test_case->query,
                              (unsigned long long)failing_allocation_number, (int)outcome.status,
                              outcome.text);
                break;
            }
            failing_allocation_number += 1;
            SXML_TEST_REQUIRE(state, failing_allocation_number < 100000);
        }
    }
}

static const sxml_test_case ir_loop_passes_test_cases[] = {
    {.name = "queries_match_golden_text", .function = test_queries_match_golden_text},
    {.name = "filters_are_indexed_by_shape", .function = test_filters_are_indexed_by_shape},
    {.name = "queries_give_values", .function = test_queries_give_values},
    {.name = "queries_survive_every_allocation_failure",
     .function = test_queries_survive_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "ir_loop_passes",
            .cases = ir_loop_passes_test_cases,
            .case_count = SXML_TEST_COUNT(ir_loop_passes_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
