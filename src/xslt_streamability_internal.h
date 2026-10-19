#pragma once

#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/xpath.h"
#include "xslt_ast_internal.h"

/*
 * Streamability analysis of XSLT 4.0 (packet 7.6a, D183, D212). Internal
 * to sxml.
 *
 * A static pass over a sealed stylesheet AST (src/xslt_ast_internal.h)
 * and the XPath ASTs of its expressions, patterns and value templates. It
 * gives each construct a posture, a sweep and a U-type (glossary
 * `posture`, `sweep`, `u_type`) by the rules of XSLT 4.0, chapter 19
 * "Streamability" (W3C QT4 CG draft; the rules are those of XSLT 3.0,
 * chapter 19, which the 4.0 streaming specification keeps), and decides
 * whether each declared-streamable construct (glossary
 * `declared_streamable`, 19.10) is guaranteed-streamable:
 * - a template rule applicable to a mode declared with streamable="yes"
 *   (6.6.4 "Streamable Templates");
 * - xsl:source-document with streamable="yes" (18.1);
 * - xsl:function with a streamability category other than unclassified
 *   (19.8.5 "Classifying Stylesheet Functions");
 * - xsl:attribute-set with streamable="yes" (10.2.3);
 * - xsl:accumulator with streamable="yes" (18.2.9);
 * - xsl:merge-source with streamable="yes" (15.4 "Streamable Merging").
 * The first one that is not is the static error XTSE3430 (19.10), unless
 * the stylesheet's configuration asks to process it without streaming
 * instead (the user option of 19.10, falls_back_to_unstreamed, D219).
 *
 * Streamed evaluation (packet 7.6b, D219) reads the sweeps of single
 * constructs (sxml_xslt_assess_sweep below) to decide what it streams;
 * the verdicts here only raise XTSE3430.
 *
 * How it walks (no C recursion, D71): a machine with an explicit stack of
 * frames in allocator memory, one frame per construct being assessed, in
 * the style of the lowering machine (src/xpath_lower_internal.h). A frame
 * that needs the posture and sweep of an operand pushes a frame for it
 * and is stepped again when that frame has its result. The operands of
 * constructs that follow the general streamability rules (19.8.1) are
 * listed on a second stack and combined one at a time.
 *
 * Where sxml cannot tell (a function it does not classify, an XPath 4.0
 * construct chapter 19 has no rule for, a limit reached), the assessment
 * is marked uncertain, and an uncertain construct is never reported: a
 * processor may stream more than the rules guarantee (19.10), and sxml
 * never rejects a stylesheet on a guess.
 */

typedef struct sxml_xslt_streamability_request {
    /* A sealed AST, borrowed for the call. */
    const sxml_xslt_ast *ast;
    /* For the machine's stacks, released before the call returns. */
    sxml_allocator allocator;
    /* The pool of the AST's names; read only. */
    const sxml_name_pool *name_pool;
    /* Most frames on the machine's stack while one construct is assessed;
       0 means 100,000. A construct that would need more is uncertain. */
    uint32_t maximum_frame_count;
    /* Receives XTSE3430 and the location of the construct; may be NULL. */
    sxml_error_record *error_record;
} sxml_xslt_streamability_request;

/*
 * Checks every declared-streamable construct of request->ast (see above).
 * Returns SXML_STATUS_OK when each is guaranteed-streamable or uncertain,
 * SXML_STATUS_STATIC_ERROR with XTSE3430 in the error record for the
 * first that is not, SXML_STATUS_OUT_OF_MEMORY when the stacks cannot
 * grow, and SXML_STATUS_INVALID_ARGUMENT for a missing or unsealed AST.
 */
SXML_NODISCARD sxml_status
sxml_xslt_check_streamability(const sxml_xslt_streamability_request *request);

/*
 * The sweep of one construct in a striding focus (glossary `sweep`, 19.6),
 * for the lowering of streamed evaluation (src/xslt_lower_stream.c,
 * D219): a motionless construct reads nothing of its context item's
 * content, so it runs in a shallow stream window.
 */
typedef enum sxml_xslt_stream_sweep {
    SXML_XSLT_STREAM_SWEEP_MOTIONLESS,
    SXML_XSLT_STREAM_SWEEP_CONSUMING,
    SXML_XSLT_STREAM_SWEEP_FREE_RANGING,
    /* sxml has no rule for it (see above). */
    SXML_XSLT_STREAM_SWEEP_UNCERTAIN,
} sxml_xslt_stream_sweep;

typedef struct sxml_xslt_sweep_query {
    /* A sealed AST, borrowed for the call. */
    const sxml_xslt_ast *ast;
    sxml_allocator allocator;
    const sxml_name_pool *name_pool;
    /* The construct: an instruction of ast, or, when expression_ast is not
       NULL, the node expression_node of that XPath AST of ast (in
       static_context). */
    sxml_xslt_ast_node_index instruction;
    /* With an instruction: its sequence constructor (its children that
       are instructions) instead of the instruction. */
    bool assesses_content;
    const sxml_ast *expression_ast;
    sxml_ast_node_index expression_node;
    sxml_xslt_ast_static_context_index static_context;
    /* The context item is a streamed document node; an element otherwise. */
    bool is_document_focus;
    /* A motionless construct whose value is streamed nodes (not grounded,
       19.4) is reported as consuming: what consumes its value reads them
       (the content of a literal result element absorbs them). */
    bool requires_grounded;
} sxml_xslt_sweep_query;

/* Writes the sweep of query's construct, with the context item striding,
   to *found. Returns SXML_STATUS_INVALID_ARGUMENT for a missing or
   unsealed AST and the allocator's failures. */
SXML_NODISCARD sxml_status sxml_xslt_assess_sweep(const sxml_xslt_sweep_query *query,
                                                 sxml_xslt_stream_sweep *found);
