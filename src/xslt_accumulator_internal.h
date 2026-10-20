#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "sxml/status.h"
#include "xpath_lower_internal.h"
#include "xpath_static_internal.h"
#include "xslt_lower_internal.h"

/*
 * Accumulators (XSLT 4.0, section 19; docs/DECISIONS.md D155; packet
 * 4.14). Internal to sxml. Without streaming: streamable is read and
 * ignored, every tree is one in memory.
 *
 * An xslt accumulator (glossary) is lowered as templates are (D144): each
 * xsl:accumulator-rule is a match function (its pattern, D118, inside a
 * TRY_CATCH that makes every error false, 6.5) and a rule function of the
 * node and $value (its select or sequence constructor with the node as
 * singleton focus, the result coerced to the as type); the initial value
 * is a function of the tree's root. The traversal of a tree (19.4) is one
 * IR function per accumulator, lowered from an XPath text: a fold-left
 * over the tree's nodes in document order (attributes and namespaces are
 * not visited) whose state holds the value, an explicit stack of the
 * nodes whose end events are still to come (D71: nothing recurses) and
 * the accumulator values (glossary) built so far. Each node first closes
 * the nodes on the stack that are not its ancestors (their end events,
 * innermost first), then has its start event; the last rule in document
 * order whose phase and pattern fit gives the new value.
 *
 * fn:accumulator-before and fn:accumulator-after are stylesheet functions
 * of a function table put in front of every static context (as
 * src/xslt_lower_declaration.c puts its own, D150). They resolve the name
 * (accumulator-name: XTDE3340), check the context item (XTTE3360), and
 * read the values through accumulator-values, which builds them the first
 * time an evaluation asks for one accumulator and tree and keeps them in
 * the evaluation memo (D149): lazily, once per document. accumulator-values
 * raises XTDE3400 when a build asks for its own accumulator and XTDE3362
 * when the tree's accumulator scope (glossary) leaves it out: the main
 * function records, for the tree of the initial match selection, the
 * accumulators use-accumulators of the initial mode names (19.2); every
 * other tree has all of them.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* The internal rows (src/function_library_xslt.c): accumulator-name,
   accumulator-scope, accumulator-values, accumulator-link,
   accumulator-original, accumulator-streamed; in no namespace, in no
   part. */
extern const sxml_native_function_view sxml_xslt_accumulator_functions;

/* The accumulators of one stylesheet lowering (sxml_xslt_lowering's
   accumulators). */
typedef struct sxml_xslt_accumulator_table sxml_xslt_accumulator_table;

/* xsl:source-document (18.1), without streaming: fn:doc of href, the
   tree's accumulator scope from use-accumulators (19.2), the sequence
   constructor with the document node as singleton focus. */
extern const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_accumulator;

/*
 * Before anything of the stylesheet is lowered, after
 * sxml_xslt_declare_functions: reads the xsl:accumulator declarations (one
 * per name, the highest import precedence: XTSE3350, XTSE3355, XTSE3185)
 * and the use-accumulators attributes of xsl:mode (XTSE3300), makes the
 * table and puts it in front of the table of every static context in
 * static_contexts (the lowering's, writable). The table borrows lowering.
 * Returns SXML_STATUS_STATIC_ERROR with the error record filled, and the
 * failures of the allocator, the name pool and the function library;
 * *table is then NULL.
 */
SXML_NODISCARD sxml_status sxml_xslt_declare_accumulators(sxml_xslt_lowering *lowering,
                                                          sxml_xpath_static_context *static_contexts,
                                                          sxml_xslt_accumulator_table **table);

/*
 * After the templates and the main function: lowers the accumulator
 * functions expressions asked for, and with them every accumulator's
 * functions. Outside a lowering machine. Fails as lowering does. NULL is
 * allowed.
 */
SXML_NODISCARD sxml_status sxml_xslt_lower_accumulators(sxml_xslt_accumulator_table *table);

/* The accumulator scope of the initial match selection, to bind in a block
   of the main function before the invocation. */
typedef struct sxml_xslt_accumulator_scope_call {
    const sxml_xslt_accumulator_table *table;
    sxml_xslt_invocation invocation;
    sxml_xpath_lowering_block *block;
} sxml_xslt_accumulator_scope_call;

/* Binds, for an invocation that applies templates to the global context
   item (2.4.3), the record of the accumulators applicable to its tree: the
   use-accumulators of the initial mode's xsl:mode (none without one,
   19.2). Nothing for other invocations or a NULL table. */
SXML_NODISCARD sxml_status sxml_xslt_bind_accumulator_scope(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_accumulator_scope_call *call);

/* Whether the stylesheet declares an accumulator: only then do copies
   need links. */
bool sxml_xslt_has_accumulators(const sxml_xslt_lowering *lowering);

/* Copies that keep the accumulator values of what they copy (19.10). */
typedef struct sxml_xslt_accumulator_link_request {
    sxml_xpath_lowering_block *block;
    /* copies[n] is a copy of originals[n] (each item copied by itself). */
    sxml_ir_atom copies;
    sxml_ir_atom originals;
} sxml_xslt_accumulator_link_request;

/* Binds in request.block the copies, each node linked to the node it
   copies for the rest of the evaluation (accumulator-link), so the
   accumulator functions give a node of a copy the values of the node it
   copies: fn:copy-of, xsl:copy-of with copy-accumulators="yes". */
SXML_NODISCARD sxml_status sxml_xslt_bind_accumulator_link(
    sxml_xpath_lowerer *lowerer, sxml_xslt_accumulator_link_request request, sxml_ir_atom *atom);

/*
 * Accumulators computed while a document streams (XSLT 4.0, 18.2.9 and
 * 19.3; D231). The functions STREAM with OPENS_DOCUMENT calls as the
 * document's events pass (src/ir_internal.h, sxml_ir_stream_accumulators):
 * stream-initial-N($root) for the accumulators applicable to the document
 * (19.2: use-accumulators of xsl:source-document, or of the initial mode
 * for a streamed principal source; only those declared streamable="yes"),
 * and stream-start-0, stream-end-0 and stream-descend-0, which apply
 * start-K, end-K or descend-K (build-K's traversal of one subtree, whose
 * window holds it) of the accumulator numbered $number to its value. The
 * document calls them accumulator by accumulator, so a rule may read the
 * value another computed for the same node (an accumulator whose rule
 * reads one not computed yet is computed after the others). The values
 * of a streamed node are read through the row accumulator-streamed, which
 * asks the streamed document (src/evaluator_stream.c); fn:accumulator-
 * before and -after try it first when an accumulator is streamable.
 */
typedef struct sxml_xslt_streamed_accumulator_request {
    const sxml_xslt_lowering *lowering;
    /* The xsl:source-document whose use-accumulators names them; read
       when invocation is NULL. */
    sxml_xslt_ast_node_index scope;
    /* For a streamed principal source: the invocation, whose initial
       mode's xsl:mode names them; NULL otherwise. */
    const sxml_xslt_invocation *invocation;
} sxml_xslt_streamed_accumulator_request;

/* Declares the functions of request's streamed document and writes them
   to *functions; all 0 when no accumulator is applicable to it. Their
   bodies are lowered by sxml_xslt_lower_accumulators. Returns the
   allocator's and the lowerer's failures. */
SXML_NODISCARD sxml_status sxml_xslt_prepare_streamed_accumulators(
    const sxml_xslt_streamed_accumulator_request *request, sxml_ir_stream_accumulators *functions);

/* As sxml_xslt_prepare_streamed_accumulators, but all 0 when no
   expression calls fn:accumulator-before or -after: nothing would read
   the values. Defined in src/xslt_lower_stream.c. */
SXML_NODISCARD sxml_status sxml_xslt_stream_accumulators(
    const sxml_xslt_streamed_accumulator_request *request, sxml_ir_stream_accumulators *functions);

/* Releases the table; NULL is allowed. The static contexts must not be
   used for lowering afterwards. */
void sxml_xslt_release_accumulators(sxml_xslt_accumulator_table *table);
