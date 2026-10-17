#pragma once

#include "sxml/memory.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/xdm.h"
#include "sxml/xquery.h"

/*
 * TEMPORARY (packet 3.13, D134): the QT4 driver's XQuery adapter (glossary:
 * qt4_xquery_adapter). Until include/sxml/xquery.h is implemented (packet
 * 3.12) the driver compiles XQuery main modules through the internal
 * sxml_xquery_compile (src/xquery_lower_internal.h, D126) and evaluates
 * them as src/xpath.c evaluates an XPath expression. Each function here
 * has the signature of the xquery.h function of the same name without
 * "qt4_", and takes that header's structs, so 3.12 replaces the adapter by
 * deleting this file and xquery_adapter.c and dropping "qt4_" from the
 * calls.
 *
 * What it does not do, until 3.10 to 3.12: prologs (a main module that
 * declares anything is SXML_STATUS_UNSUPPORTED_FEATURE), library modules,
 * output declarations. No configuration is validated: the driver passes
 * what the public XPath API has validated already.
 *
 * Thread safety: the processor's.
 */

typedef struct sxml_qt4_xquery_query sxml_qt4_xquery_query;

SXML_NODISCARD sxml_status sxml_qt4_xquery_query_memory_requirements(
    const sxml_xquery_query_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Compiles configuration->expression_text in memory; returns
   SXML_STATUS_INVALID_ARGUMENT for a NULL processor and the failures of
   sxml_xquery_compile (static errors with the error record filled). */
SXML_NODISCARD sxml_status sxml_qt4_xquery_query_initialize(
    sxml_memory_view memory,
    const sxml_xquery_query_configuration *configuration,
    sxml_qt4_xquery_query **query);

/* Accepts NULL. */
void sxml_qt4_xquery_query_finalize(sxml_qt4_xquery_query *query);

SXML_NODISCARD sxml_status sxml_qt4_xquery_query_create(
    sxml_allocator allocator,
    const sxml_xquery_query_configuration *configuration,
    sxml_qt4_xquery_query **query);

/* Accepts NULL. */
void sxml_qt4_xquery_query_destroy(sxml_qt4_xquery_query *query);

/* Evaluates the query as sxml_xpath_expression_evaluate evaluates an
   expression: bindings bind the globals by name, the result is owned. */
SXML_NODISCARD sxml_status sxml_qt4_xquery_query_evaluate(
    const sxml_qt4_xquery_query *query,
    const sxml_xquery_evaluation_request *request, sxml_sequence *result);

/* XQuery's defaults (method xml; D106): the adapter reads no output
   declarations. */
sxml_serialization_parameters sxml_qt4_xquery_query_get_serialization_parameters(
    const sxml_qt4_xquery_query *query);
