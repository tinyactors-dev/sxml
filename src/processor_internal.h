#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "evaluator_internal.h"
#include "function_library_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

/*
 * The processor's internals (include/sxml/processor.h; docs/DECISIONS.md,
 * D30, D51, D88), shared by src/processor.c and src/xpath.c. Internal to
 * sxml.
 *
 * The processor owns, in the memory it was given, its name pool, type
 * registry, item heap and the function library over every part of
 * src/function_library_*.c (D55, D78), so a compiled expression and the
 * evaluations of it find them here.
 *
 * Every evaluation brings its own document loader (src/evaluator_internal.h,
 * "Document loader"): an sxml_processor_document_loader, made from the
 * dynamic context configuration, that serves fn:doc from the available
 * documents, then from the resource resolver, and keeps what it loaded
 * until the evaluation ends, so fn:doc is stable (F&O 4.0, "fn:doc").
 *
 * Thread safety: the thread of the processor (D35).
 */

struct sxml_processor {
    /* The configuration's allocator: every object and value of the
       processor is made through it. */
    sxml_allocator allocator;
    /* The allocator of sxml_processor_create, which destroy frees the
       processor with; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    sxml_processor_limits limits;
    sxml_resource_resolver resource_resolver;
    /* Never the null clock: a null configuration clock becomes
       sxml_system_clock(). */
    sxml_clock clock;
    /* In the processor's memory. */
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    sxml_item_heap *item_heap;
    sxml_function_library *function_library;
};

/* The function table of the processor's library, for compilation and for
   fn:function-lookup. */
sxml_function_table sxml_processor_get_function_table(
    const sxml_processor *processor);

/* The evaluation depth the processor's limits give, with 0 resolved to
   SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_DEPTH. */
uint32_t sxml_processor_get_maximum_evaluation_depth(
    const sxml_processor *processor);

/* The step bound the processor's limits give, with 0 resolved to
   SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_STEP_COUNT (D89). */
uint64_t sxml_processor_get_maximum_evaluation_step_count(
    const sxml_processor *processor);

/* The expression depth the processor's limits give, with 0 resolved to
   SXML_XPATH_DEFAULT_MAXIMUM_EXPRESSION_DEPTH (src/xpath_static_internal.h). */
uint32_t sxml_processor_get_maximum_expression_depth(
    const sxml_processor *processor);

/* ----------------------------------------------------- Document loader */

/* One document fn:doc loaded in this evaluation, by the URI it was asked
   for (absolute when the static base URI allowed resolving it). */
typedef struct sxml_processor_loaded_document {
    /* Allocated from the processor's allocator. */
    uint8_t *uri_bytes;
    size_t uri_byte_count;
    /* Owned. */
    sxml_item document_node;
} sxml_processor_loaded_document;

/* What one evaluation's document loader serves. Views are borrowed and must
   outlive the loader. */
typedef struct sxml_processor_document_loader_configuration {
    sxml_processor *processor;
    /* The evaluation's resolver: the dynamic context's, or the
       processor's when that is the null resolver. */
    sxml_resource_resolver resource_resolver;
    sxml_available_document_view available_documents;
    /* The static base URI of the expression; empty when absent. */
    sxml_byte_view static_base_uri;
} sxml_processor_document_loader_configuration;

/* The loader of one evaluation. Its fields are private to
   src/processor.c. */
typedef struct sxml_processor_document_loader {
    sxml_processor_document_loader_configuration configuration;
    sxml_processor_loaded_document *loaded_documents;
    size_t loaded_document_count;
    size_t loaded_document_capacity;
} sxml_processor_document_loader;

/* Writes sizeof(sxml_processor_document_loader) and its alignment. Returns
   SXML_STATUS_INVALID_ARGUMENT without a processor. */
SXML_NODISCARD sxml_status sxml_processor_document_loader_memory_requirements(
    const sxml_processor_document_loader_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a loader that has loaded nothing in memory. It allocates as it
 * loads, from the processor's allocator. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration and the memory
 * checks of the conventions.
 */
SXML_NODISCARD sxml_status sxml_processor_document_loader_initialize(
    sxml_memory_view memory,
    const sxml_processor_document_loader_configuration *configuration,
    sxml_processor_document_loader **loader);

/* Releases the documents it loaded; accepts NULL. */
void sxml_processor_document_loader_finalize(
    sxml_processor_document_loader *loader);

/* The loader as the evaluator's interface; keeps a reference to loader. */
sxml_document_loader sxml_processor_document_loader_as_document_loader(
    sxml_processor_document_loader *loader);
