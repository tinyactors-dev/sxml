/*
 * Tests for the XQuery lowering part (src/xquery_lower.c,
 * src/xquery_lower_internal.h; D55, D103, packet 3.7).
 *
 * Golden file: tests/xpath_static_test.c. Queries are compiled with
 * sxml_xquery_compile against the function table of a processor and the
 * unit is compared with golden text in the form of
 * sxml_ir_unit_write_text; static errors are checked by code. Compiling
 * goes through a counting allocator, which must get every block back, also
 * when any one allocation fails.
 */
#include "processor_internal.h"
#include "sxml/processor.h"
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

static sxml_status fixture_create(uint64_t failing_allocation_number,
                                  fixture *test_fixture) {
    *test_fixture = (fixture){.processor = NULL, .counting_allocator = NULL};
    sxml_status status = sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = test_fixture->counting_storage,
                           .byte_count = sizeof(test_fixture->counting_storage)},
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = failing_allocation_number},
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
    const sxml_allocation_counts counts =
        sxml_counting_allocator_read_counts(test_fixture->counting_allocator);
    return counts.live_allocation_count == 0;
}

/* What compiling one query gave. */
typedef struct compile_outcome {
    sxml_status status;
    sxml_error_record error_record;
    char text[8192];
} compile_outcome;

static void compile_query(fixture *test_fixture, const char *query,
                          compile_outcome *outcome) {
    sxml_processor *processor = test_fixture->processor;
    const sxml_xpath_static_context static_context = {
        .item_heap = processor->item_heap,
        .allocator = sxml_counting_allocator_as_allocator(test_fixture->counting_allocator),
        .function_table = sxml_processor_get_function_table(processor)};
    memset(&outcome->error_record, 0, sizeof(outcome->error_record));
    outcome->text[0] = '\0';
    sxml_ir_unit *unit = NULL;
    outcome->status = sxml_xquery_compile(
        &(sxml_xpath_compile_request){
            .expression_text = {.bytes = (const uint8_t *)query,
                                .byte_count = strlen(query)},
            .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xq"),
            .static_context = &static_context,
            .error_record = &outcome->error_record},
        &unit);
    if (outcome->status != SXML_STATUS_OK) {
        return;
    }
    size_t byte_count = 0;
    const sxml_status status = sxml_ir_unit_write_text(
        unit,
        (sxml_memory_view){.bytes = (uint8_t *)outcome->text,
                           .byte_count = sizeof(outcome->text) - 1},
        &byte_count);
    if (status == SXML_STATUS_OK) {
        outcome->text[byte_count] = '\0';
    } else {
        outcome->status = status;
    }
    sxml_ir_unit_release(unit);
}

/* ------------------------------------------------------- Golden text */

typedef struct golden_case {
    const char *query;
    const char *expected_text;
} golden_case;

static const golden_case golden_cases[] = {
    {"for $x at $i in (1, 2, 3) let $y := $x * 2 where $y gt 2 order by $y descending return $i",
     "(function 1 main (parameters 0) (captures) (slots 15)\n"
     "  (let $1 (sequence 1 2 3)\n"
     "    (let $6 (map $1 (focus $2 $3 _)\n"
     "              (let $4 (binary multiply $2 2)\n"
     "                (let $5 (binary value-greater $4 2)\n"
     "                  (conditional $5\n"
     "                    (array-constructor square $2 $3 $4)\n"
     "                    (sequence)))))\n"
     "      (let $11 (sort $6 (focus $7 _ _)\n"
     "                 (key descending empty-least\n"
     "                   (let $8 (lookup $7 1)\n"
     "                     (let $9 (lookup $7 2)\n"
     "                       (let $10 (lookup $7 3)\n"
     "                         $10)))))\n"
     "        (map $11 (focus $12 _ _)\n"
     "          (let $13 (lookup $12 1)\n"
     "            (let $14 (lookup $12 2)\n"
     "              (let $15 (lookup $12 3)\n"
     "                $14))))))))\n"},
    {"for $x allowing empty at $i in () return ($x, $i)",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (let $1 (sequence)\n"
     "    (map $1 (focus $2 $3 _) allowing-empty\n"
     "      (sequence $2 $3))))\n"},
    {"for $x in (5, 6) count $c return $c",
     "(function 1 main (parameters 0) (captures) (slots 6)\n"
     "  (let $1 (sequence 5 6)\n"
     "    (let $3 (map $1 (focus $2 _ _)\n"
     "              (array-constructor square $2))\n"
     "      (map $3 (focus $4 $5 _)\n"
     "        (let $6 (lookup $4 1)\n"
     "          $5)))))\n"},
    {"for $x in 1 to 10 while $x lt 3 return $x",
     "(function 1 main (parameters 0) (captures) (slots 8)\n"
     "  (let $1 (binary range 1 10)\n"
     "    (let $3 (map $1 (focus $2 _ _)\n"
     "              (array-constructor square $2))\n"
     "      (let $4 (function-definition 2)\n"
     "        (let $5 (coerce $4 function(item(), xs:integer) as xs:boolean? function-argument)\n"
     "          (let $6 (static-call fn:take-while#2 $3 $5)\n"
     "            (map $6 (focus $7 _ _)\n"
     "              (let $8 (lookup $7 1)\n"
     "                $8))))))))\n"
     "(function 2 anonymous (parameters 2) (captures) (slots 7)\n"
     "  (let $6 (lookup $1 1)\n"
     "    (let $7 (binary value-less $6 3)\n"
     "      (conditional $7\n"
     "        true()\n"
     "        false()))))\n"},
    {"for $x in (3, 1) order by $x empty greatest collation 'http://www.w3.org/2005/xpath-functions/collation/codepoint' count $n return $n",
     "(function 1 main (parameters 0) (captures) (slots 12)\n"
     "  (let $1 (sequence 3 1)\n"
     "    (let $3 (map $1 (focus $2 _ _)\n"
     "              (array-constructor square $2))\n"
     "      (let $6 (sort $3 (focus $4 _ _)\n"
     "                (key ascending empty-greatest\n"
     "                  (let $5 (lookup $4 1)\n"
     "                    $5)))\n"
     "        (let $9 (map $6 (focus $7 _ _)\n"
     "                  (let $8 (lookup $7 1)\n"
     "                    (array-constructor square $8)))\n"
     "          (map $9 (focus $10 $11 _)\n"
     "            (let $12 (lookup $10 1)\n"
     "              $11)))))))\n"},
    {"typeswitch (1) case $s as xs:string | xs:anyURI return $s case xs:integer return 2 default $d return $d",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (let $3 (let $2 (instance-of 1 xs:string)\n"
     "            (conditional $2\n"
     "              true()\n"
     "              (instance-of 1 xs:anyURI)))\n"
     "    (conditional $3\n"
     "      1\n"
     "      (let $1 (instance-of 1 xs:integer)\n"
     "        (conditional $1\n"
     "          2\n"
     "          1)))))\n"},
    {"switch (1) case 1 case 2 return 'a' default return 'b'",
     "(function 1 main (parameters 0) (captures) (slots 13)\n"
     "  (let $1 (static-call fn:data#1 1)\n"
     "    (let $2 (coerce $1 xs:anyAtomicType? variable)\n"
     "      (let $3 (static-call fn:empty#1 $2)\n"
     "        (let $13 (let $12 (let $8 (static-call fn:data#1 1)\n"
     "                            (let $9 (static-call fn:exists#1 $8)\n"
     "                              (conditional $9\n"
     "                                (quantified some $10 $8\n"
     "                                  (let $11 (static-call fn:deep-equal#2 $2 $10)\n"
     "                                    $11))\n"
     "                                $3)))\n"
     "                   (conditional $12\n"
     "                     true()\n"
     "                     (let $4 (static-call fn:data#1 2)\n"
     "                       (let $5 (static-call fn:exists#1 $4)\n"
     "                         (conditional $5\n"
     "                           (quantified some $6 $4\n"
     "                             (let $7 (static-call fn:deep-equal#2 $2 $6)\n"
     "                               $7))\n"
     "                           $3)))))\n"
     "          (conditional $13\n"
     "            \"a\"\n"
     "            \"b\"))))))\n"},
    {"switch () case 1 eq 2 return 'a' default return 'b'",
     "(function 1 main (parameters 0) (captures) (slots 6)\n"
     "  (let $6 (let $1 (binary value-equal 1 2)\n"
     "            (let $2 (static-call fn:data#1 $1)\n"
     "              (let $3 (static-call fn:exists#1 $2)\n"
     "                (conditional $3\n"
     "                  (quantified some $4 $2\n"
     "                    (let $5 (static-call fn:deep-equal#2 true() $4)\n"
     "                      $5))\n"
     "                  false()))))\n"
     "    (conditional $6\n"
     "      \"a\"\n"
     "      \"b\")))\n"},
    {"try { 1 div 0 } catch err:FOAR0001 | *:X { $err:code } catch * { $err:map } finally { () }",
     "(function 1 main (parameters 0) (captures) (slots 3)\n"
     "  (try-catch\n"
     "    (binary divide 1 0)\n"
     "    (catch (err:FOAR0001 *:X) $1\n"
     "      (let $2 (lookup $1 \"code\")\n"
     "        $2))\n"
     "    (catch (*) $3\n"
     "      $3)\n"
     "    (finally\n"
     "      (sequence))))\n"},
    {"``[a`{1, 2}`b`{}`]``",
     "(function 1 main (parameters 0) (captures) (slots 5)\n"
     "  (let $1 (sequence 1 2)\n"
     "    (let $2 (coerce $1 xs:anyAtomicType* function-argument)\n"
     "      (let $3 (static-call fn:string-join#2 $2 \" \")\n"
     "        (let $4 (sequence \"a\" $3 \"b\")\n"
     "          (let $5 (coerce $4 xs:anyAtomicType* function-argument)\n"
     "            (static-call fn:string-join#2 $5 \"\")))))))\n"},
    {"ordered { (# Q{urn:x}p c #) { 1 } }, unordered { 2 }",
     "(function 1 main (parameters 0) (captures) (slots 0)\n"
     "  (sequence 1 2))\n"},
    {"for $x in 1 trace $x return $x",
     "(function 1 main (parameters 0) (captures) (slots 2)\n"
     "  (map 1 (focus $1 _ _)\n"
     "    (let $2 (conditional false()\n"
     "              $1\n"
     "              (sequence))\n"
     "      $1)))\n"},
};

static void test_queries_match_golden_text(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(golden_cases);
         case_index += 1) {
        static fixture test_fixture;
        static compile_outcome outcome;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        compile_query(&test_fixture, golden_cases[case_index].query, &outcome);
        const bool matches =
            outcome.status == SXML_STATUS_OK &&
            strcmp(outcome.text, golden_cases[case_index].expected_text) == 0;
        if (!matches) {
            (void)fprintf(stderr, "query %s\nstatus %d %.*s\ngot:\n%s\n",
                          golden_cases[case_index].query, (int)outcome.status,
                          (int)outcome.error_record.diagnostic.description.byte_count,
                          (const char *)outcome.error_record.diagnostic.description.bytes,
                          outcome.text);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
        fixture_destroy(&test_fixture);
    }
}

/* ------------------------------------------------------ Static errors */

typedef struct error_case {
    const char *query;
    sxml_status status;
    /* The error code's local name; NULL for a status without one. */
    const char *code;
} error_case;

static const error_case error_cases[] = {
    {"validate { <a/> }", SXML_STATUS_STATIC_ERROR, "XQST0075"},
    {"for $x in 1 order by $x collation 'urn:unknown' return $x",
     SXML_STATUS_STATIC_ERROR, "XQST0076"},
    {"(# Q{urn:x}p #) { }", SXML_STATUS_STATIC_ERROR, "XQST0079"},
    {"(# u:p #) { 1 }", SXML_STATUS_STATIC_ERROR, "XPST0081"},
    {"for $x at $x in 1 return $x", SXML_STATUS_STATIC_ERROR, "XQST0089"},
    {"for $x in 1 count $c return $d", SXML_STATUS_STATIC_ERROR, "XPST0008"},
    {"for $x in 1 while $y return $x", SXML_STATUS_STATIC_ERROR, "XPST0008"},
    {"typeswitch (1) case $a as xs:string return $a default return $a + $b",
     SXML_STATUS_STATIC_ERROR, "XPST0008"},
    {"try { 1 } catch u:e { 2 }", SXML_STATUS_STATIC_ERROR, "XPST0081"},
    {"module namespace m = 'urn:m';", SXML_STATUS_INVALID_ARGUMENT, NULL},
};

static void test_static_errors_have_codes(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(error_cases);
         case_index += 1) {
        static fixture test_fixture;
        static compile_outcome outcome;
        SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
        const error_case *test_case = &error_cases[case_index];
        compile_query(&test_fixture, test_case->query, &outcome);
        const sxml_byte_view code = outcome.error_record.code.local_name;
        const bool matches =
            outcome.status == test_case->status &&
            (test_case->code == NULL ||
             (code.byte_count == strlen(test_case->code) &&
              memcmp(code.bytes, test_case->code, code.byte_count) == 0));
        if (!matches) {
            (void)fprintf(stderr, "query %s: status %d code %.*s\n", test_case->query,
                          (int)outcome.status, (int)code.byte_count,
                          (const char *)code.bytes);
        }
        SXML_TEST_EXPECT(state, matches);
        SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
        fixture_destroy(&test_fixture);
    }
}

/* -------------------------------------------------- Allocation failure */

static void test_compile_survives_every_allocation_failure(sxml_test_state *state) {
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(golden_cases);
         case_index += 1) {
        uint64_t failing_allocation_number = 1;
        for (;;) {
            static fixture test_fixture;
            static compile_outcome outcome;
            SXML_TEST_REQUIRE(state, fixture_create(failing_allocation_number,
                                                    &test_fixture) == SXML_STATUS_OK);
            compile_query(&test_fixture, golden_cases[case_index].query, &outcome);
            SXML_TEST_EXPECT(state, fixture_released_everything(&test_fixture));
            fixture_destroy(&test_fixture);
            if (outcome.status == SXML_STATUS_OK) {
                break;
            }
            SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OUT_OF_MEMORY);
            if (outcome.status != SXML_STATUS_OUT_OF_MEMORY) {
                (void)fprintf(stderr, "query %s allocation %llu: status %d\n",
                              golden_cases[case_index].query,
                              (unsigned long long)failing_allocation_number,
                              (int)outcome.status);
                break;
            }
            failing_allocation_number += 1;
            SXML_TEST_REQUIRE(state, failing_allocation_number < 20000);
        }
    }
}

static const sxml_test_case xquery_lower_test_cases[] = {
    {.name = "queries_match_golden_text", .function = test_queries_match_golden_text},
    {.name = "static_errors_have_codes", .function = test_static_errors_have_codes},
    {.name = "compile_survives_every_allocation_failure",
     .function = test_compile_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery_lower",
            .cases = xquery_lower_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_lower_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
