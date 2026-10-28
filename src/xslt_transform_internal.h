#pragma once

#include <stdbool.h>

#include "evaluator_internal.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xslt.h"
#include "xpath_api_internal.h"
#include "xslt_ast_internal.h"

/*
 * A transformation run as a unit evaluation on a running machine
 * (glossary `unit_evaluation`; F&O 4.0, 18.3 fn:transform; packet 7.11).
 * Internal to sxml; defined in src/xslt.c, used by
 * src/xslt_transform_function.c.
 *
 * sxml_xslt_stylesheet_transform primes a transformation, evaluates its
 * unit with sxml_evaluate_unit and delivers the raw result. fn:transform
 * runs inside an evaluation, so it may not start another evaluation in C
 * (D71): it primes the transformation here, hands the evaluation request
 * to the machine as a native step's EVALUATE_UNIT, and delivers the value
 * the machine gives back. The request's extension carries the
 * transformation's result writer and function table to the code of the
 * unit (sxml_dynamic_context_get_unit_extension); the caller drops it
 * from the machine (sxml_dynamic_context_drop_unit_extension) before it
 * destroys the transformation.
 *
 * What the machine's evaluation does not give the inner transformation:
 * its own document loader (fn:doc shares the caller's documents and does
 * not strip them as the stylesheet asks), its own available documents and
 * its own trace sink.
 *
 * Thread safety: the processor's thread (D35).
 */

typedef struct sxml_xslt_machine_transformation sxml_xslt_machine_transformation;

/* A transformation to prime; borrowed for the life of the
   transformation made from it. */
typedef struct sxml_xslt_machine_transformation_request {
    const sxml_xslt_stylesheet *stylesheet;
    /* The rules of sxml_xslt_stylesheet_transform; its available
       documents are ignored by the machine's evaluation. */
    const sxml_xslt_transformation_request *transformation;
    /* xsl:message hands nothing to the trace sink. */
    bool discards_messages;
    /* xsl:assert is not evaluated. */
    bool disables_assertions;
} sxml_xslt_machine_transformation_request;

/*
 * Primes request's transformation (2.4.2) into *made, allocated with the
 * stylesheet processor's allocator. Returns SXML_STATUS_INVALID_ARGUMENT
 * as sxml_xslt_stylesheet_transform does, the errors of priming (XTDE0040,
 * XTDE0041, XTDE0045, ...) and the allocator's.
 */
SXML_NODISCARD sxml_status sxml_xslt_machine_transformation_create(
    const sxml_xslt_machine_transformation_request *request,
    sxml_xslt_machine_transformation **made);

/* The unit evaluation to ask the machine for; valid until destroy. */
const sxml_evaluation_request *sxml_xslt_machine_transformation_get_evaluation(
    const sxml_xslt_machine_transformation *running);

/* The extension the evaluation request lends the machine. */
const sxml_unit_evaluation_extension *sxml_xslt_machine_transformation_get_extension(
    const sxml_xslt_machine_transformation *running);

/* Delivers the raw result the machine gave as the request asks
   (sxml_xslt_stylesheet_transform's delivery, XTDE1490 included). */
SXML_NODISCARD sxml_status sxml_xslt_machine_transformation_deliver(
    sxml_xslt_machine_transformation *running, sxml_sequence raw_result,
    sxml_sequence *principal_result);

/* True when a result document became the principal result (26.2.2). */
bool sxml_xslt_machine_transformation_has_principal_document(
    const sxml_xslt_machine_transformation *running);

/* Releases what the transformation holds; accepts NULL. */
void sxml_xslt_machine_transformation_destroy(sxml_xslt_machine_transformation *running);

/* True when text is the memory of a module URI or source name of the
   stylesheet, which an error location may name. */
bool sxml_xslt_stylesheet_holds_text(const sxml_xslt_stylesheet *stylesheet, sxml_byte_view text);

/* ------------------------------------------------- Compiling a given AST */

/*
 * A stylesheet AST made by another front end than the stylesheet reader
 * (Schematron, src/schematron_build.c; D315), to compile as
 * sxml_xslt_stylesheet_create compiles what it reads: the in-scope
 * schemas (only those its xsl:import-schema declarations bring), lowering
 * for an apply-templates invocation in the default mode, the outputs. No
 * static parameters; constructs declared streamable are processed without
 * streaming (19.10's user option, D219).
 */
typedef struct sxml_xslt_ast_stylesheet_request {
    /* Kept by reference; must outlive the stylesheet. */
    sxml_processor *processor;
    /* Sealed; owned by the call: by the stylesheet made, destroyed when
       the call fails. */
    sxml_xslt_ast *ast;
    /* Receives the first error, located where the AST's nodes and
       expressions say; may be NULL. */
    sxml_error_record *error_record;
} sxml_xslt_ast_stylesheet_request;

/*
 * Compiles request->ast into a stylesheet allocated through allocator
 * (released with sxml_xslt_stylesheet_destroy), written to *stylesheet.
 * Returns the failures of sxml_xslt_stylesheet_create after reading, and
 * SXML_STATUS_INVALID_ARGUMENT for an AST that is not sealed.
 */
SXML_NODISCARD sxml_status sxml_xslt_stylesheet_create_from_ast(
    sxml_allocator allocator, const sxml_xslt_ast_stylesheet_request *request,
    sxml_xslt_stylesheet **stylesheet);

/* -------------------------------------------------- The optional feature */

/* The fn:transform row (src/xslt_transform_function.c), the library
   functions of sxml_xslt_transform_feature. */
extern const struct sxml_native_function_view sxml_xslt_transform_functions;
