#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "evaluator_internal.h"
#include "function_library_internal.h"
#include "ir_internal.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"

/*
 * The evaluator machine and its parts (docs/DECISIONS.md, D55, D71, D75,
 * D101). Internal to sxml.
 *
 * The machine (src/evaluator.c) runs the task stack of a dynamic context
 * in one loop (see src/evaluator_internal.h). It evaluates the XPath
 * expression kinds itself. Every other IR expression kind is evaluated by
 * an evaluator part (glossary `evaluator_part`): a file
 * src/evaluator_<part>.c that defines
 *
 *     const sxml_evaluator_part sxml_evaluator_part_<part> = {...};
 *
 * a table of the kinds it evaluates and their step functions. CMake
 * (cmake/SxmlParts.cmake) generates the registry from those files, so a
 * part that starts to evaluate a kind edits only its own file. The parts
 * of Phase 2 are stubs that return SXML_STATUS_UNSUPPORTED_FEATURE:
 *
 *     src/evaluator_construct.c  the seven constructors, SHALLOW_COPY (3.5)
 *     src/evaluator_try.c        TRY_CATCH (3.6)
 *     src/evaluator_template.c   APPLY_TEMPLATES (4.x)
 *     src/evaluator_global.c     global initializers (3.10, D135)
 *
 * A kind that no part claims, or that two parts claim, evaluates to
 * SXML_STATUS_UNSUPPORTED_FEATURE.
 *
 * Part tasks. When the machine meets an expression of a part's kind, the
 * EXPRESSION task on top becomes a PART task (glossary `part_task`) whose
 * step function is the kind's, with no state and phase 0, and the machine
 * calls that function each time the task is on top. A step function does
 * one step and returns to the loop: it may push tasks (a nested
 * expression, another part task) and return, and resumes when their value
 * arrives; it ends by handing a value down with
 * sxml_evaluator_machine_finish. It never runs the machine itself (no C
 * recursion, D71: sxml_evaluator_evaluate and
 * sxml_evaluator_call_function_item refuse a running context). Pointers to
 * tasks are invalid after a push.
 *
 * Catching. A task with catches_errors set is a catching task (glossary
 * `catching_task`). When a task above it fails with
 * SXML_STATUS_DYNAMIC_ERROR or SXML_STATUS_TYPE_ERROR, the machine puts the
 * source span into the error record, releases every task above the
 * innermost catching task (frames, arguments, native call states, part
 * states), clears its catches_errors, stores the status in
 * caught_status and runs it again. The catching task reads the error from
 * the evaluation's error record (which may be NULL when the caller passed
 * none) and takes the status with sxml_evaluator_machine_take_caught_status.
 * Its own failure is never handed to itself, and no other status is ever
 * caught (limits, memory, unsupported features, invalid arguments: XQuery
 * 4.0, "Try/Catch Expressions", catches dynamic and type
 * errors only). Catching tasks are looked for only above the base of the
 * running machine loop.
 *
 * Thread safety: as the dynamic context (D35).
 */

/* ---------------------------------------------------------------- Tasks */

typedef enum sxml_evaluator_task_kind {
    /* An IR expression of the task's function, evaluated in its frame. */
    SXML_EVALUATOR_TASK_KIND_EXPRESSION,
    /* A call with owned arguments: of function_item, or of callee (a
       static call of an IR function, arguments coerced by lowering). */
    SXML_EVALUATOR_TASK_KIND_CALL,
    /* An IR function call in progress: owns the frame at frame_base and
       passes its body's value up. */
    SXML_EVALUATOR_TASK_KIND_RETURN,
    /* A call of a resumable native body in progress. */
    SXML_EVALUATOR_TASK_KIND_NATIVE_STEP,
    /* Coerces the value of the task above to sequence_type (the result of
       a coerced function). */
    SXML_EVALUATOR_TASK_KIND_COERCE_RESULT,
    /* A task of an evaluator part: details.part says how it steps. */
    SXML_EVALUATOR_TASK_KIND_PART,
} sxml_evaluator_task_kind;

/* A call of a resumable native body: its call record, arguments and state,
   in one block of allocator memory, so the record stays put while the task
   stack grows. */
typedef struct sxml_native_call_record {
    sxml_function_call call;
    const sxml_native_function *row;
    /* Owned. */
    sxml_sequence *arguments;
    size_t argument_count;
    void *state;
    uint64_t step_number;
} sxml_native_call_record;

/* The state of a SORT expression: its items, borrowed from input, and the
   atomized key values, owned, row by row. */
typedef struct sxml_sort_state {
    sxml_item *items;
    size_t item_count;
    size_t key_count;
    sxml_sequence *key_values;
    size_t next_value_index;
} sxml_sort_state;

/* Loops: MAP, FILTER, QUANTIFIED, SORT. */
typedef struct sxml_loop_state {
    /* Owned. */
    sxml_sequence input;
    sxml_sequence_iterator iterator;
    /* Borrowed from input: the item the body or predicate runs for. */
    sxml_item current_item;
    uint64_t position;
    uint64_t size;
    /* Owned; made on first need. */
    sxml_sequence_builder *builder;
    sxml_sort_state *sort_state;
} sxml_loop_state;

/* Calls: CALL, NATIVE_STEP. */
typedef struct sxml_call_state {
    /* Owned; a zero item for a static call. */
    sxml_item function_item;
    const sxml_ir_function *callee;
    const sxml_native_function *native_function;
    /* CALL: owned arguments in allocator memory. */
    sxml_sequence *arguments;
    size_t argument_count;
    /* NATIVE_STEP: the call record. */
    sxml_native_call_record *native_call;
} sxml_call_state;

/*
 * One step of a part task, which is on top of the task stack: task points
 * at it (valid until the next push). Returns SXML_STATUS_OK after pushing a
 * task, after finishing, or after changing its own phase or state to step
 * again; a failure ends the task (and is caught or ends the evaluation).
 */
typedef sxml_status (*sxml_evaluator_part_step_function)(
    sxml_dynamic_context *dynamic_context, sxml_evaluator_task *task);

/* Releases the state of a part task when the task ends, normally or by
   unwinding; state is never NULL. */
typedef void (*sxml_evaluator_part_release_function)(
    sxml_dynamic_context *dynamic_context, void *state);

/* PART: the step function and the part's state, owned by the task. */
typedef struct sxml_evaluator_part_task_state {
    sxml_evaluator_part_step_function step;
    /* NULL when state needs no release. */
    sxml_evaluator_part_release_function release_state;
    /* The part's, in allocator memory or NULL. */
    void *state;
} sxml_evaluator_part_task_state;

struct sxml_evaluator_task {
    sxml_evaluator_task_kind task_kind;
    /* Free for the task's step function: 0 when the task starts. */
    uint32_t phase;
    /* The function whose frame the task reads (EXPRESSION, CALL, PART) or
       owns (RETURN); NULL for a call from outside an evaluation. */
    const sxml_ir_function *function;
    size_t frame_base;
    /* EXPRESSION and PART: what it evaluates; others: where they come
       from, for error locations (0 when unknown). */
    sxml_ir_expression_index expression;
    /* True for a task that counts in the evaluation depth. */
    bool counts_depth;
    /* True for a catching task (see "Catching"). */
    bool catches_errors;
    union {
        sxml_loop_state loop;
        sxml_call_state call;
        struct {
            sxml_sequence_type sequence_type;
            sxml_ir_coercion_role role;
        } coercion;
        sxml_evaluator_part_task_state part;
    } details;
};

/* ---------------------------------------------------------------- Parts */

/* One IR expression kind an evaluator part evaluates. */
typedef struct sxml_evaluator_kind_handler {
    sxml_ir_expression_kind expression_kind;
    /* The step function of the part task the expression's task becomes. */
    sxml_evaluator_part_step_function step;
} sxml_evaluator_kind_handler;

/* What one src/evaluator_<part>.c defines as sxml_evaluator_part_<part>. */
typedef struct sxml_evaluator_part {
    const sxml_evaluator_kind_handler *kind_handlers;
    size_t kind_handler_count;
} sxml_evaluator_part;

/* A registered evaluator part. */
typedef struct sxml_evaluator_registered_part {
    /* The <part> of its file name, for diagnostics and tests. */
    sxml_byte_view part_name;
    const sxml_evaluator_part *part;
} sxml_evaluator_registered_part;

typedef struct sxml_evaluator_registered_part_view {
    const sxml_evaluator_registered_part *parts;
    size_t part_count;
} sxml_evaluator_registered_part_view;

/* Every registered evaluator part, sorted by part name. Defined in the
   generated evaluator_parts.c. */
sxml_evaluator_registered_part_view sxml_evaluator_registered_parts(void);

/*
 * The step function of the one registered part that claims kind; NULL when
 * no part or more than one claims it. Defined in src/evaluator.c.
 */
sxml_evaluator_part_step_function sxml_evaluator_find_kind_handler(
    sxml_ir_expression_kind kind);

/* ------------------------------------------------------- Machine API */

/* The task on top of the running machine; valid until the next push. */
sxml_evaluator_task *sxml_evaluator_machine_top(
    sxml_dynamic_context *dynamic_context);

/* Pushes an EXPRESSION task for expression of the top task's function, in
   its frame. Returns the allocator's failures. */
SXML_NODISCARD sxml_status sxml_evaluator_machine_push_expression(
    sxml_dynamic_context *dynamic_context, sxml_ir_expression_index expression);

/*
 * Pushes a PART task in the top task's function and frame: part_state's
 * step runs it, phase 0. The task owns part_state->state from the call on:
 * on failure it is released at once.
 */
SXML_NODISCARD sxml_status sxml_evaluator_machine_push_part_task(
    sxml_dynamic_context *dynamic_context,
    const sxml_evaluator_part_task_state *part_state);

/*
 * Ends the top task: releases what it holds (its frame for RETURN, its
 * part state for PART) and hands value, owned, to the task below, which
 * takes it with sxml_evaluator_machine_take_value. Always SXML_STATUS_OK.
 */
SXML_NODISCARD sxml_status sxml_evaluator_machine_finish(
    sxml_dynamic_context *dynamic_context, sxml_sequence value);

/* Takes the value the task above finished with, owned (the empty sequence
   when none waits). */
sxml_sequence sxml_evaluator_machine_take_value(
    sxml_dynamic_context *dynamic_context);

/* Takes the status a catching task was handed (see "Catching") and clears
   it; SXML_STATUS_OK when none waits. */
sxml_status sxml_evaluator_machine_take_caught_status(
    sxml_dynamic_context *dynamic_context);

/* Reads atom in the top task's frame: borrowed, valid until that place
   changes. Raises XPDY0002 for an absent slot. */
SXML_NODISCARD sxml_status sxml_evaluator_machine_read_atom(
    sxml_dynamic_context *dynamic_context, sxml_ir_atom atom,
    sxml_sequence *value);

/* Binds slot of the top task's frame to value, owned, releasing what it
   held; slot 0 releases value. */
void sxml_evaluator_machine_bind_slot(sxml_dynamic_context *dynamic_context,
                                      sxml_ir_slot slot, sxml_sequence value);

/* Reports error into the evaluation's error record and returns its status:
   what a step function returns to raise a dynamic or type error. */
SXML_NODISCARD sxml_status sxml_evaluator_machine_raise(
    sxml_dynamic_context *dynamic_context, const sxml_static_error *error);

/* --------------------------------------------------- Global variables */

/*
 * Called by sxml_evaluator_evaluate, and for a unit evaluation (D154),
 * after the main function's frame, RETURN task and body task are pushed,
 * so tasks it pushes run before the body (the top task is the body's):
 * starts computing the global variables of the global table at
 * table_index with an initializer that the request left ABSENT (XQuery
 * 4.0, "Variable Declaration") in the order of their initializers'
 * indices, and binds the context global (name 0) to the host's context
 * value (D135). Defined in src/evaluator_global.c.
 */
SXML_NODISCARD sxml_status sxml_evaluator_start_global_initializers(
    sxml_dynamic_context *dynamic_context, size_t table_index);

/* The global table of unit in the evaluation running, or NULL when it
   has none (glossary `global_table`). Valid until a table is added.
   Defined in src/evaluator.c. */
sxml_evaluator_global_table *sxml_evaluator_find_global_table(
    sxml_dynamic_context *dynamic_context, const sxml_ir_unit *unit);
