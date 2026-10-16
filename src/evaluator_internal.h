#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * The evaluator (docs/DECISIONS.md, D38, D46, D55). Internal to sxml.
 *
 * Walks the IR of a sealed unit (src/ir_internal.h) and computes its value.
 * No bytecode (PLAN.md non-goal). Defined in src/evaluator.c,
 * src/evaluator_path.c and src/evaluator_function.c (packet 1.18), except
 * where a declaration names another file.
 *
 * Frames. Each call of an IR function takes slot_count slots from the
 * frame stack of the dynamic context and gives them back when it returns;
 * slots hold owned sequences, released when the frame ends. Function items
 * keep captured values, never frames (D38), so nothing outlives its frame.
 *
 * Depth. Every nested expression evaluation and every call adds one to the
 * evaluation depth (glossary `evaluation_depth`); past
 * maximum_evaluation_depth the evaluation stops with
 * SXML_STATUS_LIMIT_EXCEEDED, so deep recursion never overflows the C
 * stack (D51). Native functions that loop (fn:fold-right over a million
 * items) iterate and do not add depth per item.
 *
 * Globals. Global variables live in a table of the dynamic context for the
 * unit being evaluated. A FUNCTION_DEFINITION whose function reads a
 * global variable captures it like a local variable (lowering adds it to
 * captured_variables), so a function item that escapes its evaluation and
 * is called from another one sees the values of the evaluation that made
 * it.
 *
 * Thread safety: a dynamic context is used by the thread of its item heap
 * (D35).
 */

/* ------------------------------------------------------------- Slots */

typedef enum sxml_evaluator_slot_state {
    /* No value: an absent focus or an external variable without a
       binding; reading it raises XPDY0002 (XPath 4.0, section 2.2.2). */
    SXML_EVALUATOR_SLOT_STATE_ABSENT,
    /* A global variable whose initializer is running; reading it raises
       XQDY0054 (a circular definition). */
    SXML_EVALUATOR_SLOT_STATE_BEING_COMPUTED,
    SXML_EVALUATOR_SLOT_STATE_BOUND,
} sxml_evaluator_slot_state;

/* One variable's place: an owned value when bound. */
typedef struct sxml_evaluator_slot {
    sxml_sequence value;
    sxml_evaluator_slot_state slot_state;
} sxml_evaluator_slot;

typedef struct sxml_evaluator_slot_view {
    const sxml_evaluator_slot *slots;
    size_t slot_count;
} sxml_evaluator_slot_view;

/* The slots of every active frame, innermost last. */
typedef struct sxml_evaluator_frame_stack {
    sxml_evaluator_slot *slots;
    size_t slot_count_in_use;
    size_t slot_capacity;
} sxml_evaluator_frame_stack;

/* ----------------------------------------------------- Document loader */

/* A document fn:doc asks for (F&O 4.0, "fn:doc"). */
typedef struct sxml_document_request {
    /* The argument of fn:doc, as written. */
    sxml_byte_view uri_reference;
    /* The static base URI to resolve it against; may be empty. */
    sxml_byte_view base_uri;
    sxml_error_record *error_record;
} sxml_document_request;

/* Bytes fn:parse-xml parses into a new document (F&O 4.0,
   "fn:parse-xml"). */
typedef struct sxml_document_parse_request {
    sxml_byte_view xml_bytes;
    /* The base URI of the new document node; may be empty. */
    sxml_byte_view base_uri;
    sxml_error_record *error_record;
} sxml_document_parse_request;

/*
 * The operations of a document loader. Implemented by the processor
 * (src/processor.c, packet 1.23) over the available documents of the
 * dynamic context configuration, its resource resolver and its XML limits;
 * native functions and their tests only see this interface.
 */
typedef struct sxml_document_loader_operations {
    /*
     * Writes the document node for request->uri_reference, owned, to
     * *document_node: an available document, or what the resolver gives.
     * The same absolute URI gives the same node within an evaluation
     * (F&O 4.0, "fn:doc": stable). Returns SXML_STATUS_DYNAMIC_ERROR with
     * FODC0002 (not available or not well-formed) or FODC0005 (an invalid
     * URI), and the resolver's and allocator's failures.
     */
    sxml_status (*load_document)(void *state,
                                 const sxml_document_request *request,
                                 sxml_item *document_node);
    /*
     * Parses request->xml_bytes into a new tree with the processor's
     * limits and writes its document node, owned, to *document_node; a new
     * tree on every call. Returns SXML_STATUS_DYNAMIC_ERROR with FODC0006
     * for bytes that are not a well-formed document, and the allocator's
     * failures.
     */
    sxml_status (*parse_document)(void *state,
                                  const sxml_document_parse_request *request,
                                  sxml_item *document_node);
} sxml_document_loader_operations;

/* The document loader interface, passed by value. */
typedef struct sxml_document_loader {
    void *state;
    const sxml_document_loader_operations *operations;
} sxml_document_loader;

/* ---------------------------------------------------- Dynamic context */

/* How a dynamic context, the evaluator's state, is set up (the public
   sxml_dynamic_context_configuration of sxml/xpath.h is what the host
   gives one evaluation; the processor turns it into this and a request). */
typedef struct sxml_evaluator_configuration {
    /* Values are made in it; must outlive the dynamic context. */
    sxml_item_heap *item_heap;
    /* Memory for frames and the global table. Must not be the null
       allocator; must outlive the dynamic context. */
    sxml_allocator allocator;
    /* Borrowed; state must outlive the dynamic context. A loader whose
       operations is NULL makes fn:doc raise FODC0002 and fn:parse-xml
       FODC0006. */
    sxml_document_loader document_loader;
    /* Read at most once per evaluation (D51). The null clock makes
       fn:current-dateTime fail with SXML_STATUS_UNSUPPORTED_FEATURE. */
    sxml_clock clock;
    /* What fn:function-lookup finds (F&O 4.0, "fn:function-lookup"): the
       same table static analysis used. Borrowed; state must outlive the
       dynamic context. A table whose operations is NULL finds nothing. */
    sxml_function_table function_table;
    /* Greater than 0: the processor resolves "0 means sxml's default". */
    uint32_t maximum_evaluation_depth;
    /* Most frame slots in use at once; 0 means no bound beyond the
       allocator's. */
    size_t maximum_frame_slot_count;
} sxml_evaluator_configuration;

/*
 * The dynamic context of an evaluation (XPath 4.0, section 2.2.2), as
 * native function bodies receive it through sxml_function_call (the
 * substrate declares it opaque, D38). The focus is not here: it lives in
 * slots (D46). Only the evaluator writes the fields; native bodies read
 * them through the functions below.
 */
struct sxml_dynamic_context {
    sxml_item_heap *item_heap;
    sxml_allocator allocator;
    sxml_document_loader document_loader;
    sxml_clock clock;
    sxml_function_table function_table;
    bool has_clock_reading;
    sxml_clock_reading clock_reading;
    uint32_t evaluation_depth;
    uint32_t maximum_evaluation_depth;
    size_t maximum_frame_slot_count;
    sxml_evaluator_frame_stack frame_stack;
    /* The unit being evaluated and its global variables, slot n - 1 for
       global n. Set by sxml_evaluator_evaluate for its duration. */
    const sxml_ir_unit *unit;
    sxml_evaluator_slot *global_slots;
    size_t global_slot_count;
    /* Receives the error of the evaluation running; may be NULL. */
    sxml_error_record *error_record;
    /* Reserved for XSLT (Phase 4): the current mode and template rule, for
       xsl:next-match, xsl:apply-imports and current-mode(). */
    sxml_ir_mode_index current_mode;
    uint32_t current_template_rule_index;
};

/* Writes sizeof(sxml_dynamic_context) and its alignment. */
SXML_NODISCARD sxml_status sxml_dynamic_context_memory_requirements(
    const sxml_evaluator_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places a dynamic context with an empty frame stack in memory. Returns
   SXML_STATUS_INVALID_ARGUMENT for an invalid configuration. */
SXML_NODISCARD sxml_status sxml_dynamic_context_initialize(
    sxml_memory_view memory,
    const sxml_evaluator_configuration *configuration,
    sxml_dynamic_context **dynamic_context);

/* Releases the frame stack and global table; accepts NULL. */
void sxml_dynamic_context_finalize(sxml_dynamic_context *dynamic_context);

SXML_NODISCARD sxml_status sxml_dynamic_context_create(
    sxml_allocator allocator,
    const sxml_evaluator_configuration *configuration,
    sxml_dynamic_context **dynamic_context);

void sxml_dynamic_context_destroy(sxml_dynamic_context *dynamic_context);

/*
 * Writes the clock reading of the evaluation to *reading, reading the clock
 * on first use (fn:current-dateTime, fn:implicit-timezone). Returns the
 * failures of sxml_clock_read.
 */
SXML_NODISCARD sxml_status sxml_dynamic_context_read_clock(
    sxml_dynamic_context *dynamic_context, sxml_clock_reading *reading);

/* Calls the document loader's load_document (fn:doc). */
SXML_NODISCARD sxml_status sxml_dynamic_context_load_document(
    sxml_dynamic_context *dynamic_context,
    const sxml_document_request *request, sxml_item *document_node);

/* Calls the document loader's parse_document (fn:parse-xml). */
SXML_NODISCARD sxml_status sxml_dynamic_context_parse_document(
    sxml_dynamic_context *dynamic_context,
    const sxml_document_parse_request *request, sxml_item *document_node);

/* ----------------------------------------------------------- Evaluation */

/* One evaluation of a unit's main function. */
typedef struct sxml_evaluation_request {
    /* Sealed. Retained for the evaluation. */
    sxml_ir_unit *unit;
    /* The context value; when has_context_value is false, the main
       function's focus slots are absent. A value of one item is the
       context item; position and size are 1 (XPath 4.0, section 2.2.2). */
    bool has_context_value;
    sxml_sequence context_value;
    /* One slot per global variable of the unit, in index order (slot n - 1
       for global n): BOUND for an external variable with a value, which is
       coerced to its declared type (XPTY0004); ABSENT otherwise, which
       raises XPDY0002 for an external variable and runs the initializer
       for a declared one. Borrowed; values are retained. */
    sxml_evaluator_slot_view global_values;
    /* Receives the evaluation's error; may be NULL. */
    sxml_error_record *error_record;
} sxml_evaluation_request;

/*
 * Evaluates request->unit's main function and writes its value, owned, to
 * *result. The result is complete: lazy sequences in it have been read, so
 * reading it raises nothing (D51).
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR or SXML_STATUS_TYPE_ERROR with the
 * error record filled (code, description, value, the source span of the
 * expression that raised it), SXML_STATUS_LIMIT_EXCEEDED at
 * maximum_evaluation_depth or maximum_frame_slot_count, the document
 * loader's and clock's failures, and the allocator's. On failure every
 * value the evaluation made is released.
 */
SXML_NODISCARD sxml_status sxml_evaluator_evaluate(
    sxml_dynamic_context *dynamic_context,
    const sxml_evaluation_request *request, sxml_sequence *result);

/*
 * The function body operations of every function item made from an IR
 * function (FUNCTION_DEFINITION, FUNCTION_REFERENCE to an IR function): the
 * state is the sealed unit's sxml_ir_function, cast from const; retain and
 * release retain and release its unit. call needs a dynamic context and
 * returns SXML_STATUS_INVALID_ARGUMENT without one (D38).
 */
extern const sxml_function_body_operations sxml_evaluator_function_body_operations;

/* ------------------------------------------- Services for native bodies */

/* Why a value is coerced; see sxml_ir_coercion_role. */
typedef struct sxml_coercion_request {
    sxml_sequence value;
    sxml_sequence_type sequence_type;
    sxml_ir_coercion_role role;
    sxml_error_record *error_record;
} sxml_coercion_request;

/*
 * Applies the coercion rules (XPath 4.0, section 3.4: atomization,
 * untypedAtomic casting, numeric and URI promotion, function coercion) to
 * request->value and writes the coerced value, owned, to *result. Returns
 * SXML_STATUS_TYPE_ERROR (XPTY0004, or the role's code) when the value
 * cannot be coerced, and the heap's failures.
 */
SXML_NODISCARD sxml_status sxml_evaluator_coerce(
    sxml_dynamic_context *dynamic_context,
    const sxml_coercion_request *request, sxml_sequence *result);

/*
 * Calls call->function_item as a dynamic function call does (XPath 4.0,
 * section 4.5.3): coerces call->arguments to the parameter types of its
 * signature, then sxml_function_item_call. What higher-order native
 * functions (fn:filter, fn:fold-left, fn:sort with a key) use. Returns
 * SXML_STATUS_TYPE_ERROR (XPTY0004) for a wrong arity or an argument that
 * cannot be coerced, and the body's failures.
 */
SXML_NODISCARD sxml_status sxml_evaluator_call_function_item(
    const sxml_function_call *call, sxml_sequence *result);

/* The effective boolean value of a sequence (XPath 4.0, section 2.5.4). */
typedef struct sxml_effective_boolean_value {
    bool is_true;
} sxml_effective_boolean_value;

/*
 * Writes the effective boolean value of sequence to
 * *effective_boolean_value (fn:boolean, predicates, conditions). Returns
 * SXML_STATUS_TYPE_ERROR (FORG0006) for a sequence that has none, with
 * error_record (which may be NULL) filled, and a lazy sequence's failures.
 */
SXML_NODISCARD sxml_status sxml_evaluator_compute_effective_boolean_value(
    sxml_sequence sequence, sxml_error_record *error_record,
    sxml_effective_boolean_value *effective_boolean_value);
