#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir_internal.h"
#include "sxml/status.h"
#include "sxml/types.h"
#include "sxml/view.h"

/*
 * The optimizer seam (docs/DECISIONS.md, D46, D55). Internal to sxml.
 *
 * A pass is an IR-to-IR function over one unit, run in a fixed list
 * between lowering and evaluation, before the unit is sealed:
 *
 *     front end -> AST -> static analysis and lowering -> passes -> seal
 *     -> evaluation
 *
 * Phase 1 has one pass, the verifier, which changes nothing. Rewrites
 * (constant folding, type-based simplification, dropping DOCUMENT_ORDER on
 * single steps, inlining) come later, each as a new pass with tests that
 * compare results with and without it.
 *
 * Rules for passes:
 * - A pass may add expressions, atoms, constants and functions to the
 *   unit and replace expressions and functions in place
 *   (sxml_ir_unit_replace_expression); it never removes anything, so
 *   indices held elsewhere stay valid. Replaced or unreachable
 *   expressions are dead weight until the unit is released.
 * - The result must satisfy every invariant the verifier checks, so the
 *   pass list can run the verifier after each pass.
 * - A pass must not evaluate calls of native functions marked
 *   SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT or _IS_NONDETERMINISTIC.
 * - A pass that finds an error that must be raised (a type error proved
 *   statically) rewrites the expression to RAISE rather than failing, unless
 *   the specification requires a static error.
 *
 * Thread safety: a pass runs on the thread of the unit's item heap (D35).
 */

/* What a pass receives. */
typedef struct sxml_ir_pass_request {
    /* The unit to check or rewrite, not yet sealed. */
    sxml_ir_unit *unit;
    /* The registry of the unit's types (the item heap's). */
    sxml_type_registry *type_registry;
    /* Receives a static or type error the specification requires to be
       raised statically; may be NULL. */
    sxml_error_record *error_record;
    /* Receives what is wrong when a pass finds a unit that breaks an
       invariant (an error in sxml itself); may be NULL. */
    sxml_diagnostic *diagnostic;
} sxml_ir_pass_request;

/*
 * Runs one pass. Returns SXML_STATUS_OK, SXML_STATUS_STATIC_ERROR or
 * SXML_STATUS_TYPE_ERROR with the error record filled,
 * SXML_STATUS_INVALID_ARGUMENT with the diagnostic filled for a unit that
 * breaks an invariant, or the unit's and allocator's failures.
 */
typedef sxml_status (*sxml_ir_pass_function)(
    const sxml_ir_pass_request *request);

/* A pass: a name for diagnostics and tests, and its function. */
typedef struct sxml_ir_pass {
    /* ASCII, such as "verify". */
    sxml_byte_view pass_name;
    sxml_ir_pass_function run;
} sxml_ir_pass;

typedef struct sxml_ir_pass_view {
    const sxml_ir_pass *passes;
    size_t pass_count;
} sxml_ir_pass_view;

/*
 * The verifier (src/ir_verify.c, packet 1.12). Checks, and changes
 * nothing:
 * - every index (expression, function, constant, global, list) is in
 *   range, and every expression kind and enumeration value is known;
 *   reserved kinds are rejected until the evaluator implements them;
 * - the expressions of each function body form a tree: every expression
 *   is reached at most once, from one function;
 * - every slot read is at most the function's slot_count and is bound
 *   where it is read: a parameter, a captured variable, or a slot bound by
 *   an enclosing LET, MAP, FILTER, SORT or QUANTIFIED, or a focus slot of
 *   the main function; no slot is bound twice on one path;
 * - a function's captured_variables are atoms bound at its
 *   FUNCTION_DEFINITION in defining_function, and every function except
 *   main and top-level ones has exactly one FUNCTION_DEFINITION;
 * - parameter_count matches the arity of signature, a registered function
 *   type; every function target names a native row whose arity range
 *   includes the call's, an existing IR function of that arity, or an
 *   atomic constructor type;
 * - type identifiers are registered in request->type_registry.
 * Returns SXML_STATUS_INVALID_ARGUMENT with request->diagnostic describing
 * the first violation (with the expression's source span).
 */
SXML_NODISCARD sxml_status sxml_ir_verify(const sxml_ir_pass_request *request);

/*
 * The hoist pass (src/ir_loop_passes.c, D224): moves the LETs at the start
 * of a loop's body (a MAP without flags, a FILTER's predicate) whose bound
 * expressions are plain (make no nodes or function items, call nothing
 * nondeterministic) and read nothing the loop binds out of the loop,
 * computed once, and only when the loop's input is not empty. LETs whose
 * bound expression is a LET are flattened first. Returns the allocator's
 * failures.
 */
SXML_NODISCARD sxml_status sxml_ir_hoist_invariant_lets(const sxml_ir_pass_request *request);

/*
 * The filter-index pass (src/ir_loop_passes.c, D224): gives a FILTER
 * whose predicate needs "key = probe", "key eq probe" or "key is probe"
 * to be true, with a key that reads only the focus value and a probe that
 * reads no focus, a filter index (sxml_ir_filter_index): copies of the
 * expressions of both operands. Changes nothing else. A unit at its
 * expression limit keeps the filters indexed so far. Returns the
 * allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_ir_index_filters(const sxml_ir_pass_request *request);

/*
 * The descendant pass (src/ir_loop_passes.c, glossary `descendant_path`):
 * rewrites "E//T[P]..." as "E/descendant::T[P]..." where the step is to
 * the children, T selects XNodes only and every predicate selects by the
 * item alone (it reads neither the position nor the size and its value is
 * no number), and the intermediate values are read once: one FILTER over
 * the descendants instead of one per node below E. Returns the
 * allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_ir_shorten_descendant_paths(const sxml_ir_pass_request *request);

/*
 * The global-read pass (src/ir_global_read.c, D234): puts a GLOBAL_READ of
 * each global with an initializer that an expression reads as one of its
 * own atoms in the expression's place, the expression moved into its
 * body, so globals are computed on first read. Returns the allocator's
 * failures and SXML_STATUS_LIMIT_EXCEEDED at the unit's expression limit.
 */
SXML_NODISCARD sxml_status sxml_ir_read_globals_lazily(const sxml_ir_pass_request *request);

/*
 * The passes compile runs, in order (src/ir.c, packet 1.12):
 * {"verify", sxml_ir_verify}, {"descendant", sxml_ir_shorten_descendant_paths},
 * {"filter-index", sxml_ir_index_filters},
 * {"hoist", sxml_ir_hoist_invariant_lets} (D224), {"global-read",
 * sxml_ir_read_globals_lazily} (D234): filter indexes first, while index
 * keys still read globals and not the slots hoisting binds; global reads
 * last, so the loop passes see globals as atoms.
 */
extern const sxml_ir_pass_view sxml_ir_standard_passes;

/* A list of passes to run over one unit. */
typedef struct sxml_ir_pass_list_request {
    sxml_ir_pass_request pass_request;
    sxml_ir_pass_view passes;
    /* Runs sxml_ir_verify after every pass that is not itself the
       verifier: on in debug builds and tests, to name the pass that broke
       an invariant. */
    bool verifies_after_each_pass;
} sxml_ir_pass_list_request;

/*
 * Runs request->passes in order over request->pass_request.unit and stops
 * at the first failure, which it returns; the diagnostic then names the
 * pass. Defined in src/ir.c (packet 1.12).
 */
SXML_NODISCARD sxml_status sxml_ir_run_passes(
    const sxml_ir_pass_list_request *request);
