#pragma once

/*
 * What every fuzz target shares (packet 1.26): a name pool, type registry
 * and item heap behind a counting allocator, made fresh for each input so
 * that one input cannot hide a leak of another, and a leak check that
 * stops the fuzzer when an input leaves a block or a cell behind.
 *
 * Golden file: the fixture of tests/evaluator_test.c (itself after
 * tests/memory_test.c).
 */
#include "function_library_internal.h"
#include "ir_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* libFuzzer's entry point. Its name and its two parameters are fixed by
   libFuzzer (an external callback signature, docs/C_GUIDELINES.md). */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size); // NOLINT(readability-identifier-naming)

/* The bound on the item heap of every target: inputs may make values,
   not exhaust the machine. */
#define SXML_FUZZ_MAXIMUM_HEAP_BYTE_COUNT ((size_t)16 << 20U)

typedef struct sxml_fuzz_fixture {
    sxml_counting_allocator *counting_allocator;
    alignas(16) uint8_t counting_storage[256];
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    sxml_item_heap *item_heap;
    /* The function library of every part written so far (D78, D79), and
       the statically known functions of the XPath targets: its table. */
    sxml_function_library *function_library;
    sxml_function_table function_table;
} sxml_fuzz_fixture;

/* What a target needs beyond the heap. */
typedef struct sxml_fuzz_fixture_configuration {
    /* Make the function library and its table (the XPath targets). */
    bool has_function_library;
} sxml_fuzz_fixture_configuration;

/* Makes the fixture. A failure, which no input can cause, stops the
   process. */
sxml_status
sxml_fuzz_fixture_create(const sxml_fuzz_fixture_configuration *configuration,
                         sxml_fuzz_fixture *fixture);

/* The counting allocator, which every object of a target allocates
   through. */
sxml_allocator sxml_fuzz_fixture_allocator(const sxml_fuzz_fixture *fixture);

/*
 * Checks that every cell and block was released (raised error values are
 * cleared first), then destroys the fixture. A leak is reported on stderr
 * and stops the process, which libFuzzer records as a crash with the
 * input.
 */
void sxml_fuzz_fixture_destroy_checking_leaks(sxml_fuzz_fixture *fixture);

/* Stops the process with a message when status is not one an input may
   cause: a status that means sxml was called wrongly or is broken. */
void sxml_fuzz_check_status(sxml_status status, const char *operation_name);

/* The external variables every XPath target declares, as item()*: $x and
   $doc. */
#define SXML_FUZZ_VARIABLE_COUNT 2U

/* Bounds of compilation for the XPath targets. */
#define SXML_FUZZ_MAXIMUM_EXPRESSION_COUNT 100000U
#define SXML_FUZZ_MAXIMUM_EXPRESSION_DEPTH 2000U

/*
 * Compiles expression_text against a static context with the
 * predeclared namespaces, the prefix p bound to urn:p, and the variables
 * $x and $doc (global variables 1 and 2), and writes the sealed unit,
 * owned, to *unit. Returns the failures of sxml_xpath_compile; a failure
 * that an expression may not cause stops the process.
 */
sxml_status sxml_fuzz_compile_xpath(const sxml_fuzz_fixture *fixture,
                                    sxml_byte_view expression_text,
                                    sxml_ir_unit **unit);

/* The input as a byte view. */
sxml_byte_view sxml_fuzz_input_view(const uint8_t *data, size_t size);

/*
 * Compiles text as an XQuery main module on processor (static base URI
 * urn:fuzz/), evaluates it and serializes the result as its prolog asks
 * into a buffer of at most 1 MB, all with the fixture's allocator. A
 * failure that a query may not cause stops the process (the XQuery
 * targets; golden: tests/xquery_test.c, run_query).
 */
void sxml_fuzz_run_xquery(const sxml_fuzz_fixture *fixture, sxml_processor *processor,
                          sxml_byte_view text);

/* Checks that no cell of processor's item heap is live (raised errors
   cleared first), then destroys processor; a leak stops the process. */
void sxml_fuzz_processor_destroy_checking_leaks(sxml_processor *processor);
