#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "evaluator_internal.h"
#include "function_library_internal.h"
#include "function_library_parts_internal.h"
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
 * running machine loop; below a task with stops_catching set (a global's
 * computation, D234), not among the tasks of its function's unit (an
 * error of an initializer is caught inside the initializer, not around
 * the read: XSLT 4.0, 2.4.2 and 8.3), but in a unit evaluation's asker.
 *
 * Delivering. A node constructor whose content is one expression makes
 * its part task a receiving task (glossary `receiving_task`, D166): it
 * opens its construction, sets its receive function and pushes the
 * content as a delivering task (glossary `delivering_task`). When a
 * delivering task finishes, its value goes to the receive function of
 * the nearest receiving task below it, and the task below gets the empty
 * sequence. Delivering passes on only where a value would reach the
 * receiver unchanged: a LET's body and a conditional's branch (which
 * replace their task), the bound expression of a content LET
 * (sxml_ir_let.is_content), a MAP's body (each iteration's value is
 * delivered as it is made; not for a path's MAP, whose body may select
 * JNodes), the body of a function a delivering call enters, and the
 * template calls of a delivering APPLY_TEMPLATES. So content is added
 * as it is made, and a constructor in a delivering task builds into the
 * construction of its receiver instead of making a tree that would be
 * copied (src/evaluator_construct.c). A try/catch body never delivers, so
 * no caught error leaves part of its content behind.
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
    /* The static call's item demand (D259): the body may stop once it has
       made that many items; 0 for every item. */
    uint64_t item_demand;
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
    /* A MAP or FILTER whose value is read for its first item_demand items
       only (glossary `item_demand`, D259) stops once its builder has
       item_count >= item_demand; 0 when it makes every item, and then
       item_count is not kept. */
    uint64_t item_demand;
    uint64_t item_count;
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

/* Adds value (owned, released on every path) to the construction in
   state, the part state of a receiving task (see "Delivering"). Never
   pushes a task or runs the machine. */
typedef sxml_status (*sxml_evaluator_part_receive_function)(
    sxml_dynamic_context *dynamic_context, void *state, sxml_sequence value);

/* PART: the step function and the part's state, owned by the task. */
typedef struct sxml_evaluator_part_task_state {
    sxml_evaluator_part_step_function step;
    /* NULL when state needs no release. */
    sxml_evaluator_part_release_function release_state;
    /* Set while the task is a receiving task; NULL otherwise. */
    sxml_evaluator_part_receive_function receive;
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
    /* True for a task no catching task below catches the failures of
       (see "Catching"). */
    bool stops_catching;
    /* True for a delivering task (see "Delivering"). */
    bool delivers_content;
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
    /* The languages whose programs it evaluates, as CMake read them from
       the file's
           const sxml_language_set sxml_evaluator_part_<part>_languages = ...;
       (as a library part's, D101); all three when the file has none. */
    sxml_language_set languages;
} sxml_evaluator_registered_part;

typedef struct sxml_evaluator_registered_part_view {
    const sxml_evaluator_registered_part *parts;
    size_t part_count;
} sxml_evaluator_registered_part_view;

/* Every registered evaluator part, sorted by part name. Defined in the
   generated evaluator_parts.c. */
sxml_evaluator_registered_part_view sxml_evaluator_registered_parts(void);

/* The registered evaluator parts serving one language, sorted by part
   name (static data; D227). Each is defined in a generated unit of its
   own, evaluator_parts_<language>.c, so naming one links only its parts. */
const sxml_evaluator_registered_part_view *sxml_evaluator_xpath_parts(void);
const sxml_evaluator_registered_part_view *sxml_evaluator_xquery_parts(void);
const sxml_evaluator_registered_part_view *sxml_evaluator_xslt_parts(void);

/*
 * The step function of the one part of parts that claims kind; NULL when
 * no part or more than one claims it. The machine looks among the parts of
 * the unit whose code it runs (sxml_ir_unit_configuration.evaluator_parts,
 * XPath's when NULL; D227). Defined in src/evaluator.c.
 */
sxml_evaluator_part_step_function sxml_evaluator_find_kind_handler(
    const sxml_evaluator_registered_part_view *parts, sxml_ir_expression_kind kind);

/* ------------------------------------------------------- Machine API */

/* The task on top of the running machine; valid until the next push. */
sxml_evaluator_task *sxml_evaluator_machine_top(
    sxml_dynamic_context *dynamic_context);

/* Pushes an EXPRESSION task for expression of the top task's function, in
   its frame. Returns the allocator's failures. */
SXML_NODISCARD sxml_status sxml_evaluator_machine_push_expression(
    sxml_dynamic_context *dynamic_context, sxml_ir_expression_index expression);

/* As sxml_evaluator_machine_push_expression, as a delivering task: the
   top task must be a receiving task or a delivering one. */
SXML_NODISCARD sxml_status sxml_evaluator_machine_push_content(
    sxml_dynamic_context *dynamic_context, sxml_ir_expression_index expression);

/* The part state of the receiving task the top task delivers into, or
   NULL when the top task is not a delivering task. */
void *sxml_evaluator_machine_find_receiver(sxml_dynamic_context *dynamic_context);

/* Adds value (owned) to the construction the top task delivers into: for
   a delivering part task that adds values of its own (a separator).
   SXML_STATUS_INVALID_ARGUMENT when the top task does not deliver. */
SXML_NODISCARD sxml_status sxml_evaluator_machine_deliver(
    sxml_dynamic_context *dynamic_context, sxml_sequence value);

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
 * takes it with sxml_evaluator_machine_take_value; the value of a
 * delivering task goes to its receiver instead, and the task below gets
 * the empty sequence (see "Delivering"). Returns the receiver's failures
 * (a content error such as XQTY0024, the allocator's).
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

/* A predicate's value and the position of the item it tests. */
typedef struct sxml_predicate_value {
    /* Borrowed. */
    sxml_sequence value;
    uint64_t position;
} sxml_predicate_value;

/* Writes to *selects whether the value of a predicate selects the item at
   position (XPath 4.0, 4.4.1): a numeric value by equality with the
   position, anything else by its effective boolean value. Raises FORG0006
   as a FILTER does. For STREAM (src/evaluator_stream.c). */
SXML_NODISCARD sxml_status sxml_evaluator_machine_test_predicate(
    sxml_dynamic_context *dynamic_context, const sxml_predicate_value *predicate, bool *selects);

/* ------------------------------------------------------ Filter indexes */

/* A FILTER with a filter index (D224) and its input. */
typedef struct sxml_filter_index_request {
    /* In its unit: the memo keeps the FILTER's index under its address. */
    const sxml_ir_filter *filter;
    /* Borrowed; the task retains it. */
    sxml_sequence input;
} sxml_filter_index_request;

/*
 * Pushes the filter index task (src/evaluator_index.c) above the FILTER's
 * task on top, in its function and frame. The task finishes with the
 * candidates: the items of the input, in order, whose index key compares
 * with the index probe's value, which are the only items the predicate
 * can select; or with the input itself when the index cannot say. Its
 * task binds the FILTER's focus value slot. Returns the allocator's
 * failures.
 */
SXML_NODISCARD sxml_status sxml_evaluator_start_filter_index(
    sxml_dynamic_context *dynamic_context, const sxml_filter_index_request *request);

/* --------------------------------------------------- Global variables */

/*
 * Called by sxml_evaluator_evaluate, and for a unit evaluation (D154),
 * after the main function's frame, RETURN task and body task are pushed
 * (the top task is the body's): binds the context global (name 0) of the
 * global table at table_index to the host's context value (D135). The
 * other globals are computed on first read (GLOBAL_READ, D234). Defined in
 * src/evaluator_global.c.
 */
SXML_NODISCARD sxml_status sxml_evaluator_bind_context_global(
    sxml_dynamic_context *dynamic_context, size_t table_index);

/* A global of the global table at table_index, by number (from 1). */
typedef struct sxml_evaluator_global_request {
    size_t table_index;
    uint32_t global_number;
} sxml_evaluator_global_request;

/*
 * Pushes a PART task, in the top task's function and frame, that computes
 * the global of request, which has an initializer and is ABSENT: it is
 * BEING_COMPUTED while its initializer's CALL runs, then BOUND to the
 * value, and the task finishes with the empty sequence; ABSENT again when
 * the task is released before (the initializer failed). For the machine's
 * GLOBAL_READ (D234). Defined in src/evaluator_global.c.
 */
SXML_NODISCARD sxml_status sxml_evaluator_compute_global(
    sxml_dynamic_context *dynamic_context, sxml_evaluator_global_request request);

/* The global table of unit in the evaluation running, or NULL when it
   has none (glossary `global_table`). Valid until a table is added.
   Defined in src/evaluator.c. */
sxml_evaluator_global_table *sxml_evaluator_find_global_table(
    sxml_dynamic_context *dynamic_context, const sxml_ir_unit *unit);
