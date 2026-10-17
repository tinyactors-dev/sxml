#pragma once

#include <stdint.h>

#include "ir_internal.h"
#include "sxml/status.h"
#include "xpath_ast_internal.h"
#include "xpath_static_internal.h"

/*
 * Lowering XQuery 4.0 expressions (docs/DECISIONS.md, D55, D103, D111,
 * D121; packet 3.7). Internal to sxml.
 *
 * src/xquery_lower.c is the lowering part `xquery`
 * (sxml_xpath_lowering_part_xquery, src/xpath_lower_internal.h): for the
 * XQuery language it lowers FLWOR expressions with every clause but group
 * by and the windows (packet 3.8), typeswitch, switch, try/catch,
 * ordered and unordered expressions, extension expressions, validate
 * (XQST0075, D97) and string constructors. The core lowers everything
 * XPath 4.0 has; node constructors are packet 3.9's part.
 *
 * FLWOR expressions (XQuery 4.0, section 4.13). The clauses between two
 * tuple stream clauses (order by, count, while; group by and the windows
 * of 3.8) are a clause segment: for is a MAP whose focus value and
 * position slots are the variable and its position, let a LET, where a
 * CONDITIONAL, so the tuple stream is never a value. A segment that ends
 * at a tuple stream clause returns one tuple array per tuple: a square
 * ARRAY_CONSTRUCTOR with one member per live variable bound to a slot
 * (the tuple layout; constants and globals are not packed). The tuple
 * stream clause works on the sequence of tuple arrays and a MAP over its
 * result unpacks the members with LOOKUP into slots before the next
 * segment:
 *
 * - order by: SORT over the tuple arrays; each key is lowered with the
 *   members unpacked from the SORT's focus value. SORT is stable, so
 *   "stable" changes nothing.
 * - count: the position slot of the unpacking MAP.
 * - while: fn:take-while over the tuple arrays with a closure of the
 *   tuple array and its position that unpacks the members and returns
 *   fn:boolean of the condition, so no condition is evaluated after the
 *   first false one.
 * - trace: the value is lowered (static errors are raised) but not
 *   evaluated; trace output is implementation-defined (4.13.10).
 *
 * For `for $x at $i in $in let $y := $x * 2 where $y gt 2 order by $y
 * descending return $i` ($in global 1) the dump is that of
 * src/ir_internal.h, "Lowering XQuery 4.0".
 *
 * typeswitch and switch (4.21.2, 4.18) are CONDITIONAL chains over
 * INSTANCE_OF and over fn:deep-equal of the atomized values, each case's
 * tests in the else branch of the case before (so no case operand after
 * the effective case is evaluated, 2.6.5).
 *
 * Errors: as sxml_xpath_lower (src/xpath_static_internal.h).
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* An XQuery main module to lower: an AST whose root is a MODULE node. */
typedef struct sxml_xquery_lowering_request {
    const sxml_ast *ast;
    /* As sxml_xpath_lowering_request: namespaces, external variables
       (global variables of the unit, in order), the function table, the
       static base URI. */
    const sxml_xpath_static_context *static_context;
    /* sxml_ir_unit_configuration.maximum_expression_count. */
    uint32_t maximum_expression_count;
    /* As sxml_xpath_lowering_request.maximum_expression_depth. */
    uint32_t maximum_expression_depth;
    sxml_error_record *error_record;
} sxml_xquery_lowering_request;

/*
 * Lowers the body of request->ast's main module into a new unit with the
 * lowering parts of SXML_LANGUAGE_XQUERY, as sxml_xpath_lower does for
 * XPath, and writes it, owned, unsealed and with no pass run, to *unit.
 *
 * The prolog is packet 3.10's: a main module whose prolog declares
 * anything gives SXML_STATUS_UNSUPPORTED_FEATURE, a library module
 * SXML_STATUS_INVALID_ARGUMENT. Otherwise returns the failures of
 * sxml_xpath_lower.
 */
SXML_NODISCARD sxml_status sxml_xquery_lower(
    const sxml_xquery_lowering_request *request, sxml_ir_unit **unit);

/*
 * Parses request->expression_text with sxml_xquery_build_ast, lowers it
 * with sxml_xquery_lower, runs sxml_ir_standard_passes and seals, as
 * sxml_xpath_compile does: the internal entry tests and drivers use until
 * the XQuery API (packet 3.12).
 */
SXML_NODISCARD sxml_status sxml_xquery_compile(
    const sxml_xpath_compile_request *request, sxml_ir_unit **unit);
