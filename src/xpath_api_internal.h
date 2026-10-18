#pragma once

#include <stdbool.h>

#include "function_library_internal.h"
#include "ir_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "xpath_static_internal.h"

/*
 * What the public XPath API (src/xpath.c) shares with the XQuery API
 * (src/xquery.c) and the XSLT API (src/xslt.c; docs/DECISIONS.md, D106,
 * D146, D157): the checks of the host's static and dynamic contexts and
 * the evaluation of a sealed unit, so an XQuery query and an XSLT
 * transformation are compiled against the same static context and
 * evaluated by the same code as an XPath expression. Internal to sxml.
 *
 * Thread safety: the thread of the processor (D35).
 */

/* True when the identifiers of context are the processor's (name pool,
   type registry) and its decimal formats, collations and static base URI
   follow include/sxml/xpath.h. */
bool sxml_static_context_configuration_is_valid(
    const sxml_processor *processor, const sxml_static_context_configuration *context);

/* The static context compilation starts from: context's views over the
   processor's item heap, allocator and function table. */
sxml_xpath_static_context sxml_processor_static_context(
    sxml_processor *processor, const sxml_static_context_configuration *context);

/* True when context follows the rules of include/sxml/xpath.h (distinct
   variable and document names, document nodes, an xs:language, distinct
   environment variable names). */
bool sxml_dynamic_context_configuration_is_valid(
    const sxml_dynamic_context_configuration *context);

/* One evaluation of a sealed unit. Borrowed for the call. */
typedef struct sxml_unit_evaluation {
    sxml_processor *processor;
    /* Sealed. */
    sxml_ir_unit *unit;
    /* Used when the unit has none of its own; may be empty. */
    sxml_byte_view static_base_uri;
    /* Checked by the caller (sxml_dynamic_context_configuration_is_valid). */
    const sxml_dynamic_context_configuration *dynamic_context;
    sxml_error_record *error_record;
    /* What a language adds to the evaluation; NULL for none (XPath,
       XQuery). Borrowed for the call. */
    const struct sxml_unit_evaluation_extension *extension;
} sxml_unit_evaluation;

/* What XSLT adds to an evaluation (src/xslt.c). Each zero member keeps
   what XPath and XQuery get. */
typedef struct sxml_unit_evaluation_extension {
    /* The dynamic context's function table (fn:function-lookup,
       function-available); the null table means the processor's. */
    sxml_function_table function_table;
    /* Applied to every document the evaluation's loader gets from the
       resolver (XSLT 4.0, 4.2.2: whitespace stripping). */
    sxml_document_preparation document_preparation;
    /* The transformation's result writer (src/xslt_output_internal.h). */
    struct sxml_xslt_result_writer *result_writer;
} sxml_unit_evaluation_extension;

/*
 * Evaluates evaluation->unit as sxml_xpath_expression_evaluate does: each
 * global variable gets the binding of its name (the context global, name
 * 0, none), the evaluation makes its own document loader and dynamic
 * context and releases them before returning, and the result is written,
 * owned, to *result. Returns the statuses of
 * sxml_xpath_expression_evaluate; on failure *result is untouched.
 */
SXML_NODISCARD sxml_status sxml_evaluate_unit(const sxml_unit_evaluation *evaluation,
                                              sxml_sequence *result);
