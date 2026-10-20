#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir_internal.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * The evaluator (docs/DECISIONS.md, D38, D46, D55). Internal to sxml.
 *
 * Walks the IR of a sealed unit (src/ir_internal.h) and computes its value.
 * No bytecode (PLAN.md non-goal). Defined in src/evaluator.c,
 * src/evaluator_path.c and src/evaluator_function.c (packet 1.18), except
 * where a declaration names another file. The task types, the machine API
 * and the evaluator parts that evaluate the XQuery and XSLT kinds are in
 * src/evaluator_machine_internal.h (D101).
 *
 * No recursion (D71, D75). The evaluator is an explicit-stack machine:
 * evaluating an expression that contains a nested expression (the bound
 * expression of a LET, the body of a MAP, a function body) pushes an
 * evaluator task for it on the task stack of the dynamic context, in
 * allocator memory, and the machine's one loop runs the top task until the
 * stack is empty. When a task finishes, its value goes to the task below,
 * which resumes. A call of an IR function, of a function item of any kind,
 * and every step of a resumable native body (src/function_library_
 * internal.h) is a task too, so user recursion, fn:fold-right and nested
 * closures cost heap, never C stack. A LET body, a branch of a
 * CONDITIONAL and a call in tail position replace their task instead of
 * pushing one, and a tail call reuses its caller's frame (glossary
 * `tail_call`), so loops written as tail recursion run in constant memory.
 * Only leaf work (operators, casts, axis steps, atomization, matching a
 * type) runs as plain C calls, and those walk nested values with explicit
 * stacks of their own.
 *
 * Lazy sequences. The evaluator makes no lazy sequence whose generator
 * would evaluate IR or call a function item: MAP, FILTER, paths and calls
 * build their values completely (Phase 1), so reading a sequence never
 * re-enters the evaluator. Native bodies may make lazy sequences only with
 * generators that call no function item.
 *
 * Frames. Each call of an IR function takes slot_count slots from the
 * frame stack of the dynamic context and gives them back when it returns;
 * slots hold owned sequences, released when the frame ends (a slot bound
 * again, as a MAP's focus slots are for each item, releases its old
 * value). Function items keep captured values, never frames (D38), so
 * nothing outlives its frame.
 *
 * Depth. The evaluation depth (glossary `evaluation_depth`) is the number
 * of calls in progress: IR function calls and resumable native calls; a
 * tail call does not add one. Past maximum_evaluation_depth the evaluation
 * stops with SXML_STATUS_LIMIT_EXCEEDED, and past maximum_frame_slot_count
 * too: deep recursion is a limit status, never a stack overflow (D51). The
 * tasks between two calls are bounded by the nesting of the function's IR,
 * so the depth bounds the task stack as well.
 *
 * Reentry. sxml_evaluator_evaluate, sxml_evaluator_call_function_item and
 * the call operations of IR function bodies run the machine on a dynamic
 * context that is not running already; on a running one they return
 * SXML_STATUS_INVALID_ARGUMENT. Inside an evaluation the machine
 * recognizes the function bodies it knows (IR, native rows, coerced
 * functions) and calls them through tasks; other bodies (maps, arrays, host
 * functions) are leaves and are called directly.
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

/* The global table of one unit in one evaluation (glossary
   `global_table`): slot n - 1 for global n. */
typedef struct sxml_evaluator_global_table {
    /* Retained while the table lives. */
    sxml_ir_unit *unit;
    /* Allocated with the dynamic context's allocator; NULL for a unit
       without globals. */
    sxml_evaluator_slot *slots;
    size_t slot_count;
    /* What the language of the unit evaluation adds to the code of the
       unit (src/xpath_api_internal.h; fn:transform's transformation, packet
       7.11); NULL for none. Borrowed from the native body that asked for
       the unit evaluation, which drops it before it ends
       (sxml_dynamic_context_drop_unit_extension). */
    const struct sxml_unit_evaluation_extension *extension;
} sxml_evaluator_global_table;

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
    /* fn:doc without trust-external (F&O 4.0, 17.1.1): an external DTD or
       entity the document refers to is FODC0016 instead of being read
       (D199). Other callers (XSLT's fn:document) leave it false and read
       them through the resolver. */
    bool blocks_external_resources;
    /* dtd-validation=true: FODC0007 when the document is not valid against
       its DTD. */
    bool validates_dtd;
    /* strip-space="none" (F&O 4.0, 17.1.1): the document as parsed,
       without the host's document preparation (XSLT's xsl:strip-space,
       4.2.2). The loader keeps only prepared documents, so with a
       preparation each such call reads a new tree (D274). */
    bool skips_preparation;
    /* The document preparation to apply (the caller's static context's
       package, sxml_ir_static_context.package_number); 0 for the default. The
       loader keeps one document per URI and preparation. */
    uint32_t preparation_number;
} sxml_document_request;

/* Bytes fn:parse-xml parses into a new document (F&O 4.0,
   "fn:parse-xml"). */
typedef struct sxml_document_parse_request {
    sxml_byte_view xml_bytes;
    /* The base URI of the new document node; may be empty. */
    sxml_byte_view base_uri;
    sxml_error_record *error_record;
    /* As in sxml_document_request (fn:parse-xml's options). */
    bool blocks_external_resources;
    bool validates_dtd;
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
    /* Most calls in progress at once; 0 means
       SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_DEPTH (the processor
       resolves the public "0 means sxml's default" to it). */
    uint32_t maximum_evaluation_depth;
    /* Most frame slots in use at once; 0 means no bound beyond the
       allocator's. */
    size_t maximum_frame_slot_count;
    /* Most evaluation steps of one evaluation (glossary
       `evaluation_step`; D89): every task the machine runs is one, and
       leaf work charges what it reads (sxml_dynamic_context_charge_steps).
       0 means no bound (the processor resolves its public "0 means sxml's
       default" to SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_STEP_COUNT). */
    uint64_t maximum_evaluation_step_count;
} sxml_evaluator_configuration;

/* The step bound the processor uses when its limits give 0: 10^8 steps,
   5 to 15 seconds of work at the 7 to 20 million steps a second measured
   in D259 (XMark-All takes 12 million), so a query that would run for
   hours stops in seconds (D89, D259). The test drivers set their own
   bounds (D166, D178). */
#define SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_STEP_COUNT 100000000U

/* The steps each element of a streamed document grants the evaluation
   that reads it (sxml_dynamic_context_grant_steps, D295): ten times what
   the streamed identity takes (about 50 an element, its attributes and
   text), so a stream of any length that does a few times the identity's
   work per element runs within the default bound, and a loop that reads
   no more input stops at most 500 steps per element read after the bound.
   Evaluations of trees are unchanged. */
#define SXML_EVALUATOR_STREAMED_ELEMENT_STEP_COUNT 500U

/* The depth limit when the configuration gives 0. Each level costs some
   hundred bytes of allocator memory, not C stack. */
#define SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_DEPTH 1000000U

/* One entry of the task stack; defined in src/evaluator_machine_internal.h
   (D101). */
typedef struct sxml_evaluator_task sxml_evaluator_task;

/* The tasks of the running evaluation, innermost last. */
typedef struct sxml_evaluator_task_stack {
    sxml_evaluator_task *tasks;
    size_t task_count;
    size_t task_capacity;
} sxml_evaluator_task_stack;

/*
 * What an evaluation memo entry was computed for (D148): a computation the
 * caller names (an address of its static data, so callers cannot collide,
 * plus a number of its own, such as the key of an xsl:key) and a subject
 * item. Subjects are the same when they are one item, the same node
 * (sxml_generic_node_is_same_node), atomic items of one type annotation
 * that are atomic-equal, or arrays of as many members whose items are the
 * same in that sense, nested functions by identity (the arguments of a
 * cached xsl:function call, D163).
 */
typedef struct sxml_evaluation_memo_key {
    const void *computation;
    uint64_t computation_number;
    sxml_item subject;
} sxml_evaluation_memo_key;

/* One value of the evaluation memo; the evaluator owns the subject and the
   value it keeps. */
typedef struct sxml_evaluation_memo_entry {
    sxml_evaluation_memo_key key;
    sxml_sequence value;
} sxml_evaluation_memo_entry;

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
    /* The allocator of sxml_dynamic_context_create, which destroy frees
       the context with; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    sxml_document_loader document_loader;
    sxml_clock clock;
    sxml_function_table function_table;
    bool has_clock_reading;
    sxml_clock_reading clock_reading;
    uint32_t evaluation_depth;
    uint32_t maximum_evaluation_depth;
    size_t maximum_frame_slot_count;
    /* Steps taken by the evaluation running, and the bound (0: none). */
    uint64_t evaluation_step_count;
    uint64_t maximum_evaluation_step_count;
    sxml_evaluator_frame_stack frame_stack;
    sxml_evaluator_task_stack task_stack;
    /* True while the machine runs on this context (see "Reentry"). */
    bool is_running;
    /* The value of the task that finished last, owned, waiting for the
       task below it. */
    bool has_task_value;
    sxml_sequence task_value;
    /* The status a catching task was handed and has not taken yet
       (src/evaluator_machine_internal.h, "Catching"); SXML_STATUS_OK when
       none. */
    sxml_status caught_status;
    /* The unit being evaluated. Set by sxml_evaluator_evaluate for its
       duration. */
    const sxml_ir_unit *unit;
    /* The global tables of the evaluation running: the unit's first, then
       one for each unit a native step evaluated on the machine
       (fn:load-xquery-module, D154), kept until the evaluation ends so
       the functions of that unit read its globals. A task reads the
       table of its function's unit. Allocated with allocator. */
    sxml_evaluator_global_table *global_tables;
    size_t global_table_count;
    size_t global_table_capacity;
    /* Receives the error of the evaluation running; may be NULL. */
    sxml_error_record *error_record;
    /* The static base URI of the unit being evaluated (XPath 4.0, section
       2.2.1), borrowed from the request; empty when absent or outside
       sxml_evaluator_evaluate. */
    sxml_byte_view static_base_uri;
    /* Reserved for XSLT (Phase 4): the current mode and template rule, for
       xsl:next-match, xsl:apply-imports and current-mode(). */
    sxml_ir_mode_index current_mode;
    uint32_t current_template_rule_index;
    /* The evaluation memo (glossary `evaluation_memo`, D148): values
       computed once per evaluation, released when it ends. */
    sxml_evaluation_memo_entry *memo_entries;
    size_t memo_entry_count;
    size_t memo_entry_capacity;
    /* An open-addressing index of the entries by key hash: entry number
       plus one, 0 for a free place; a power of two, at least twice the
       entries (D163). */
    size_t *memo_index;
    size_t memo_index_capacity;
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

/* Releases the frame stack, the task stack and the global table; accepts
   NULL. */
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

/*
 * Writes the implicit timezone of the evaluation to *offset_minutes: the
 * clock's (read on first use, as sxml_dynamic_context_read_clock), or UTC
 * when the clock is the null clock. Returns the clock's failures.
 */
SXML_NODISCARD sxml_status sxml_dynamic_context_read_implicit_timezone(
    sxml_dynamic_context *dynamic_context, int16_t *offset_minutes);

/*
 * The static base URI of the code running (fn:static-base-uri,
 * fn:resolve-uri, fn:doc, fn:parse-xml, fn:json-to-xml), borrowed; empty
 * when absent (XPath 4.0, section 2.2.1). Lexical (D91): of the innermost
 * task that runs IR, the module its STATIC_CALL or node constructor names
 * (D163), else its function's module (D148), else its unit's, when that
 * unit has one set (sxml_ir_unit_set_static_base_uri); otherwise the
 * request's. So a function item called after its evaluation ended, or
 * inside another expression, sees the static base URI of its own
 * definition.
 */
sxml_byte_view sxml_dynamic_context_get_static_base_uri(
    const sxml_dynamic_context *dynamic_context);

/*
 * The decimal formats and collations (XPath 4.0, section 2.2.1) of the code
 * running, read lexically as the static base URI is: those of the module
 * the innermost STATIC_CALL or node constructor names (D163), else the
 * running unit's (unit->static_context); NULL outside an evaluation.
 * Borrowed.
 */
const sxml_ir_static_context *sxml_dynamic_context_get_static_context(
    const sxml_dynamic_context *dynamic_context);

/*
 * The statically known namespaces beyond the predeclared ones of the code
 * running, which names given as strings and casts to xs:QName resolve
 * with (D84), found as sxml_dynamic_context_get_static_context finds its
 * context. Empty outside an evaluation. Borrowed.
 */
sxml_namespace_binding_view sxml_dynamic_context_get_static_namespaces(
    const sxml_dynamic_context *dynamic_context);

/*
 * The unit whose code is running, for the parts of its static context
 * that functions read lexically, as the static base URI (XPath 4.0,
 * section 2.2.1): the decimal formats and collations
 * (unit->static_context) and the static namespaces. The unit of the
 * innermost task that runs IR, else the unit being evaluated; NULL
 * outside an evaluation. Borrowed.
 */
const sxml_ir_unit *sxml_dynamic_context_get_running_unit(
    const sxml_dynamic_context *dynamic_context);

/*
 * The extension of the unit evaluation whose code is running (glossary
 * `unit_evaluation`): the extension of the global table of
 * sxml_dynamic_context_get_running_unit's unit, which a unit evaluation
 * a native step asked for sets (sxml_evaluation_request.extension; a
 * unit evaluation that gives none inherits the extension of the code that
 * asked for it). NULL when the code runs in no such unit evaluation, or
 * outside an evaluation: the evaluation's own extension is then the one
 * its document loader carries (src/xpath_api_internal.h). Borrowed.
 */
const struct sxml_unit_evaluation_extension *sxml_dynamic_context_get_unit_extension(
    const sxml_dynamic_context *dynamic_context);

/* Clears extension from every global table that holds it: the native
   body that lent it calls this before releasing it. */
void sxml_dynamic_context_drop_unit_extension(
    sxml_dynamic_context *dynamic_context,
    const struct sxml_unit_evaluation_extension *extension);

/*
 * Charges step_count evaluation steps to the evaluation running (D89):
 * leaf work that reads many items without running a task (a general
 * comparison of two long sequences) calls it before the work. Returns
 * SXML_STATUS_LIMIT_EXCEEDED, with the error record's diagnostic filled,
 * when the steps pass maximum_evaluation_step_count.
 */
SXML_NODISCARD sxml_status sxml_dynamic_context_charge_steps(
    sxml_dynamic_context *dynamic_context, uint64_t step_count);

/*
 * Gives the evaluation running step_count more steps before
 * maximum_evaluation_step_count stops it (D295): a streamed document
 * grants SXML_EVALUATOR_STREAMED_ELEMENT_STEP_COUNT for each element its
 * parse reads, so the bound of a streamed evaluation grows with its input
 * while a loop that reads nothing stops as before. Does nothing without a
 * bound.
 */
void sxml_dynamic_context_grant_steps(sxml_dynamic_context *dynamic_context,
                                      uint64_t step_count);

/*
 * The evaluation memo (glossary `evaluation_memo`, D148): values that a
 * native body computes once per evaluation and per subject node, such as
 * the index of an xsl:key for one document (XSLT 4.0, 20.2). Entries live
 * until the evaluation (sxml_evaluator_evaluate or
 * sxml_evaluator_call_function_item) ends; one evaluation's memo is never
 * seen by another, so a value may depend on the evaluation's documents.
 *
 * Writes the value stored for key to *value, borrowed until the
 * evaluation ends, and returns true; false when there is none.
 */
bool sxml_dynamic_context_find_memo(const sxml_dynamic_context *dynamic_context,
                                    const sxml_evaluation_memo_key *key, sxml_sequence *value);

/*
 * Stores entry (the subject and value retained) for the rest of the
 * evaluation; an entry with the same key is replaced. Returns the
 * allocator's failures, with nothing stored.
 */
SXML_NODISCARD sxml_status sxml_dynamic_context_store_memo(
    sxml_dynamic_context *dynamic_context, const sxml_evaluation_memo_entry *entry);

/*
 * The accumulator values of a node of a streamed document (XSLT 4.0,
 * 18.2.9; D231), for the row accumulator-streamed: the streamed documents
 * of the running STREAM tasks whose accumulators are computed as their
 * events pass hold the values of the nodes their windows hold now. Values
 * are maps from accumulator numbers (from 1) to values, borrowed until
 * the task stack changes.
 */
typedef struct sxml_streamed_accumulator_values {
    /* False for a node no such document holds; the rest is then unset. */
    bool is_streamed;
    /* The values after the node's start event and after its end event;
       no item while that event has not been read, or for descended. */
    sxml_item before;
    sxml_item after;
    /* For a node of a window that holds a subtree: the map from numbers to
       {"b": map, "a": map} of the values before and after its nodes, by
       fn:generate-id (src/xslt_accumulator.c, descend-K); else no item. */
    sxml_item descended;
    /* The error of the accumulator rule that failed in the document, which
       a value it left out raises when read (XSLT 4.0, 19.5); NULL when
       none failed. */
    const sxml_error_raise_request *failure;
    /* The document's values now: a map whose keys are the numbers of the
       accumulators applicable to it (19.2). */
    sxml_item applicable;
    /* While the document computes the node's values, accumulator by
       accumulator: set to true to have the one computing wait until the
       others are done, when its rule reads a value not computed yet; NULL
       otherwise. */
    bool *waiting;
} sxml_streamed_accumulator_values;

/* Writes the values of node to *values. Defined in
   src/evaluator_stream.c; never fails but on an invalid argument. */
SXML_NODISCARD sxml_status sxml_evaluator_stream_find_accumulator_values(
    sxml_dynamic_context *dynamic_context, sxml_item node, sxml_streamed_accumulator_values *values);

/* What a streamed open of a document finds (XSLT 4.0, 18.2,
   fn:stream-available). */
typedef enum sxml_stream_availability {
    /* The evaluation has no processor's document loader to open it. */
    SXML_STREAM_AVAILABILITY_UNKNOWN,
    /* Retrieved, with a well-formed prolog and document element start tag
       (or delivered by the resolver as a node). */
    SXML_STREAM_AVAILABILITY_AVAILABLE,
    /* fn:doc's errors, or not well-formed that far. */
    SXML_STREAM_AVAILABILITY_UNAVAILABLE
} sxml_stream_availability;

/* Opens the document at uri_reference (resolved against the static base
   URI) as xsl:source-document streamable="yes" would, reads it to the end
   of the document element's start tag, closes it and writes what it found
   to *availability. Defined in src/evaluator_stream.c; fails only for the
   resolver's and allocator's failures. */
SXML_NODISCARD sxml_status sxml_evaluator_stream_check_available(
    sxml_dynamic_context *dynamic_context, sxml_byte_view uri_reference,
    sxml_stream_availability *availability);

/* SXML_STATUS_OK when the running unit may access external resources;
   else FODC0016 in error_record (F&O 4.0, 18.2: the functions of a module
   fn:load-xquery-module loaded without trust are untrusted). */
SXML_NODISCARD sxml_status sxml_dynamic_context_check_trust(
    const sxml_dynamic_context *dynamic_context, sxml_error_record *error_record);

/* Calls the document loader's load_document (fn:doc), after
   sxml_dynamic_context_check_trust. */
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
    /* The static base URI (XPath 4.0, section 2.2.1), an absolute URI, or
       empty when absent, for units that do not carry their own (D91).
       Borrowed for the call. */
    sxml_byte_view static_base_uri;
    /* Receives the evaluation's error; may be NULL. */
    sxml_error_record *error_record;
    /* A unit evaluation a native step asks for (EVALUATE_UNIT): what its
       language adds to the unit's code (src/xpath_api_internal.h), or NULL
       to inherit the asking code's. Borrowed until the native body is
       released, which drops it first. sxml_evaluator_evaluate ignores
       it. */
    const struct sxml_unit_evaluation_extension *extension;
} sxml_evaluation_request;

/*
 * Evaluates request->unit's main function and writes its value, owned, to
 * *result. The result is complete: lazy sequences in it have been read, so
 * reading it raises nothing (D51).
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR or SXML_STATUS_TYPE_ERROR with the
 * error record filled (code, description, value, the source span of the
 * expression that raised it), SXML_STATUS_LIMIT_EXCEEDED at
 * maximum_evaluation_depth or maximum_frame_slot_count,
 * SXML_STATUS_UNSUPPORTED_FEATURE for the IR kinds no evaluator part
 * evaluates yet and for global variables with an initializer while the
 * global part does not compute them (src/evaluator_machine_internal.h), the document loader's
 * and clock's failures, the allocator's, and SXML_STATUS_INVALID_ARGUMENT
 * for an unsealed unit or a dynamic context that is running already. On
 * failure every value the evaluation made is released.
 */
SXML_NODISCARD sxml_status sxml_evaluator_evaluate(
    sxml_dynamic_context *dynamic_context,
    const sxml_evaluation_request *request, sxml_sequence *result);

/*
 * The function body operations of every function item made from an IR
 * function (FUNCTION_DEFINITION, FUNCTION_REFERENCE to an IR function): the
 * state is the sealed unit's sxml_ir_function, cast from const; retain and
 * release retain and release its unit. call needs a dynamic context that
 * is not running, and returns SXML_STATUS_INVALID_ARGUMENT otherwise (D38;
 * see "Reentry").
 */
extern const sxml_function_body_operations sxml_evaluator_function_body_operations;

/*
 * The body of a function item made by function coercion (XPath 4.0, section
 * 3.4.4): state NULL, one captured value, the wrapped function item, whose
 * arity may be lower than the signature's. A call coerces the arguments to
 * the item's own signature (as every dynamic call does), calls the wrapped
 * item with as many leading arguments as it takes (which it coerces to its
 * parameter types) and coerces the result to the signature's result type
 * (XPTY0004). call
 * follows the rules of sxml_evaluator_function_body_operations.
 */
extern const sxml_function_body_operations
    sxml_evaluator_coerced_function_body_operations;

/*
 * The body of a function item made from an xs: constructor function
 * (FUNCTION_REFERENCE with a constructor target, such as xs:integer#1):
 * state NULL, one captured value, an xs:integer item holding the target
 * type's identifier. A call casts its argument, allowing the empty
 * sequence (XPath 4.0, section 4.5.1.3); a leaf, it needs no dynamic
 * context.
 */
extern const sxml_function_body_operations
    sxml_evaluator_constructor_body_operations;

/*
 * The body of a run-time closure (glossary `run_time_closure`, D287): a
 * function item that calls the function item it wraps at the place where
 * it was made, so a context-dependent function (XPath 4.0, 4.5.4; F&O
 * 4.0, 1.7) keeps that place's static context (its module, D275) and, when
 * it takes the caller's focus, that place's focus. The state is the IR
 * function of the place (retain and release retain and release its unit);
 * the captured values are the wrapped item, the place's expression index
 * as an xs:integer and, for a row that takes the caller's focus and was
 * made with one, the three parts of that focus. The machine calls the
 * wrapped item in a CALL task of the place (src/evaluator.c), so
 * sxml_dynamic_context_get_static_context and its kin read the place's
 * module; call follows the rules of sxml_evaluator_function_body_operations.
 */
extern const sxml_function_body_operations
    sxml_evaluator_run_time_closure_body_operations;

/* A run-time closure to make. */
typedef struct sxml_run_time_closure_request {
    /* The function item to wrap; borrowed. */
    sxml_item function_item;
    /* The caller's focus (context value, position, size; borrowed) for a
       native row that takes it, or NULL. */
    const sxml_sequence *caller_focus;
} sxml_run_time_closure_request;

/*
 * Writes, owned, to *item the run-time closure of request->function_item
 * at the place of the innermost task that runs IR (as
 * sxml_dynamic_context_get_static_context finds it), or the function item
 * itself, retained, when no task runs IR. Returns the heap's failures.
 * Defined in src/evaluator.c.
 */
SXML_NODISCARD sxml_status sxml_evaluator_make_run_time_closure(
    sxml_dynamic_context *dynamic_context, const sxml_run_time_closure_request *request,
    sxml_item *item);

/*
 * sxml:run-time-closure($function) with the caller's focus (glossary
 * `caller_focus`): the run-time closure of $function at the place of the
 * static call. Lowering calls it for a named reference to a context-
 * dependent native function (XPath 4.0, 4.5.4); no library lists it.
 * Defined in src/evaluator_function.c.
 */
extern const sxml_native_function sxml_evaluator_run_time_closure_function;

/* ------------------------------------------- Services for native bodies */

/* Why a value is coerced; see sxml_ir_coercion_role. */
typedef struct sxml_coercion_request {
    sxml_sequence value;
    sxml_sequence_type sequence_type;
    sxml_ir_coercion_role role;
    sxml_error_record *error_record;
    /* True for the rules of a composite cast (glossary `composite_cast`;
       XPath 4.0, section 4.19.2.4) instead of the coercion rules: an item
       that does not match its item type is cast to it ("J cast as T"),
       or XPTY0004 when the type is not a cast target; a record keeps only
       the entries of its fields; duplicate map keys are XQDY0137. */
    bool applies_cast_rules;
    /* Under applies_cast_rules: the statically known namespaces for casts
       to xs:QName (see sxml_cast_request). Borrowed for the call. */
    sxml_namespace_binding_view namespace_bindings;
} sxml_coercion_request;

/*
 * Applies the coercion rules (XPath 4.0, section 3.4: atomization,
 * untypedAtomic casting, numeric and URI promotion, function coercion), or
 * the cast rules when request->applies_cast_rules, to request->value and
 * writes the coerced value, owned, to *result. Returns
 * SXML_STATUS_TYPE_ERROR (XPTY0004, or the role's code) when the value
 * cannot be coerced, the errors of the casts it makes, and the heap's
 * failures.
 */
SXML_NODISCARD sxml_status sxml_evaluator_coerce(
    sxml_dynamic_context *dynamic_context,
    const sxml_coercion_request *request, sxml_sequence *result);

/*
 * Calls call->function_item as a dynamic function call does (XPath 4.0,
 * section 4.5.3) from outside an evaluation: coerces call->arguments to the
 * parameter types of its signature, then runs the machine on
 * call->dynamic_context, which must not be running (see "Reentry").
 * Higher-order native functions do not use this: they are resumable bodies
 * and ask the evaluator for their calls (D75). Writes the result, complete,
 * to *result. Returns SXML_STATUS_TYPE_ERROR (XPTY0004) for a wrong arity
 * or an argument that cannot be coerced, SXML_STATUS_INVALID_ARGUMENT
 * without a dynamic context or on a running one, and the body's failures.
 */
SXML_NODISCARD sxml_status sxml_evaluator_call_function_item(
    const sxml_function_call *call, sxml_sequence *result);

/* A value to atomize. */
typedef struct sxml_atomization_request {
    sxml_sequence value;
    sxml_error_record *error_record;
} sxml_atomization_request;

/*
 * Atomizes request->value (XPath 4.0, section 2.5.3; glossary
 * `atomization`) and writes the atomic items, owned, to *result: atomic
 * items as they are, the typed value of each XNode, the atomized
 * ·jvalue· of each JNode, the atomized members of each array, at any
 * depth (walked with an explicit stack). A value of atomic items only is
 * returned as it is (retained). Returns SXML_STATUS_TYPE_ERROR (FOTY0013
 * for a map or another function item, FOTY0012 for a node without a typed
 * value), a lazy sequence's failures, and the heap's and allocator's.
 */
SXML_NODISCARD sxml_status sxml_evaluator_atomize(
    sxml_dynamic_context *dynamic_context,
    const sxml_atomization_request *request, sxml_sequence *result);

/* A value to match against a sequence type. */
typedef struct sxml_type_match_request {
    sxml_sequence value;
    sxml_sequence_type sequence_type;
    /* Receives a lazy sequence's error; may be NULL. */
    sxml_error_record *error_record;
} sxml_type_match_request;

/* The outcome of matching. */
typedef struct sxml_type_match {
    bool is_match;
} sxml_type_match;

/*
 * Writes to *match whether request->value is an instance of
 * request->sequence_type (XPath 4.0, section 3.1.2 "Sequence Type
 * Matching", "instance of"): the occurrence, then every item against the
 * item type: atomic and union types by derivation, enumerations, node
 * tests, JNode tests (keys and value types), function types by signature,
 * maps, arrays and records by their entries and members at any depth, and
 * choice types by their alternatives, on an explicit stack of type match
 * frames. Returns a lazy sequence's failures and the heap's and
 * allocator's; *match is then untouched.
 */
SXML_NODISCARD sxml_status sxml_evaluator_match_sequence_type(
    sxml_dynamic_context *dynamic_context,
    const sxml_type_match_request *request, sxml_type_match *match);

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

/* ------------------------------------- Between src/evaluator*.c files */

/* An array in allocator memory to grow (task stack, frame stack, work
   lists); defined in src/evaluator.c. */
typedef struct sxml_evaluator_growth {
    /* In/out: the array, NULL before its first growth, and its capacity
       in elements. */
    void **elements;
    size_t *element_capacity;
    /* Elements in use, which are kept. */
    size_t element_count;
    /* The array grows, doubling, to hold at least this many. */
    size_t required_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_evaluator_growth;

/* Grows *growth->elements; returns the allocator's failures and
   SXML_STATUS_ARITHMETIC_OVERFLOW, leaving the array as it was. */
SXML_NODISCARD sxml_status sxml_evaluator_grow(sxml_allocator allocator,
                                              const sxml_evaluator_growth *growth);

/* Two positions to compare while sorting. */
typedef struct sxml_index_pair {
    size_t first_index;
    size_t second_index;
} sxml_index_pair;

/* Orders the elements at two positions; a failure stops the sort. */
typedef sxml_status (*sxml_index_comparison)(void *state, sxml_index_pair pair,
                                             sxml_ordering *ordering);

/* Positions to sort (src/evaluator_path.c). */
typedef struct sxml_index_sort_request {
    /* In/out: the positions, put into order. */
    size_t *indices;
    size_t index_count;
    sxml_index_comparison compare;
    void *compare_state;
} sxml_index_sort_request;

/* A stable, bottom-up merge sort (no recursion) with scratch memory from
   allocator. Returns the comparison's and allocator's failures. */
SXML_NODISCARD sxml_status sxml_evaluator_sort_indices(
    sxml_allocator allocator, const sxml_index_sort_request *request);

/* One axis step (src/evaluator_path.c). */
typedef struct sxml_step_request {
    /* The origin: generic nodes (XPTY0004 for anything else). */
    sxml_sequence origin;
    sxml_axis axis;
    /* A registered item type (sxml_ir_step). */
    sxml_type_identifier node_test;
    /* sxml_ir_step.has_qualified_name_test and is_xnode_type_test. */
    bool has_qualified_name_test;
    bool is_xnode_type_test;
    sxml_error_record *error_record;
    /* For a forward axis from an XNode: at most this many nodes of each
       origin, its first in document order (an item demand, D259); 0 for
       all of them. */
    uint64_t maximum_node_count;
} sxml_step_request;

/*
 * The nodes of request->axis from each origin node that match the node
 * test, in axis order for one origin (XPath 4.0, section 4.6.4), in
 * document order without duplicates for several, written, owned, to
 * *result. From a JNode the axes run over the JTree, a name test N selects
 * the JNodes with key "N" (section 4.6.5) and jnode() tests are matched
 * with sxml_evaluator_match_sequence_type. Returns SXML_STATUS_TYPE_ERROR
 * (XPTY0004), and the tree's, heap's and allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_evaluator_make_step_value(
    sxml_dynamic_context *dynamic_context, const sxml_step_request *request,
    sxml_sequence *result);

/* A value whose generic nodes to put into document order. */
typedef struct sxml_document_order_request {
    sxml_sequence input;
    sxml_error_record *error_record;
} sxml_document_order_request;

/*
 * The input sorted into document order without duplicates when it holds
 * only generic nodes, unchanged when it holds none (XPath 4.0, section
 * 4.6.3), written, owned, to *result. Returns SXML_STATUS_TYPE_ERROR
 * (XPTY0018) for a mix, and the heap's and allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_evaluator_make_document_order_value(
    sxml_dynamic_context *dynamic_context,
    const sxml_document_order_request *request, sxml_sequence *result);

/* union, intersect, except (XPath 4.0, section 4.8.4). */
typedef enum sxml_node_set_operator {
    SXML_NODE_SET_OPERATOR_UNION,
    SXML_NODE_SET_OPERATOR_INTERSECT,
    SXML_NODE_SET_OPERATOR_EXCEPT,
} sxml_node_set_operator;

typedef struct sxml_node_set_request {
    sxml_node_set_operator node_set_operator;
    sxml_sequence left_operand;
    sxml_sequence right_operand;
    sxml_error_record *error_record;
} sxml_node_set_request;

/* The operator's nodes in document order without duplicates, owned, in
   *result. Returns SXML_STATUS_TYPE_ERROR (XPTY0004) for an item that is
   not a generic node, and the heap's and allocator's failures. */
SXML_NODISCARD sxml_status sxml_evaluator_make_node_set_value(
    sxml_dynamic_context *dynamic_context, const sxml_node_set_request *request,
    sxml_sequence *result);

/* An XNode and a node test of the type registry. */
typedef struct sxml_node_test_request {
    sxml_node_reference node;
    const sxml_node_test *node_test;
} sxml_node_test_request;

/* Writes to *match whether request->node matches the node test (kind,
   name, type annotation, nilled, document element; XPath 4.0, section
   3.2.7). Returns the tree's and name pool's failures. */
SXML_NODISCARD sxml_status sxml_evaluator_test_node(
    const sxml_item_heap *item_heap, const sxml_node_test_request *request,
    sxml_type_match *match);

/* A FUNCTION_DEFINITION to make (src/evaluator_function.c). */
typedef struct sxml_function_definition_request {
    const sxml_ir_function *function;
    /* The values of function->captured_variables, in order; retained. */
    sxml_sequence_view captured_values;
} sxml_function_definition_request;

/* Makes the function item, owned, in *item: the function's name,
   signature and annotations, an IR body. Returns the heap's failures. */
SXML_NODISCARD sxml_status sxml_evaluator_make_function_definition(
    sxml_dynamic_context *dynamic_context,
    const sxml_function_definition_request *request, sxml_item *item);

/* A FUNCTION_REFERENCE to make. */
typedef struct sxml_function_reference_request {
    const sxml_ir_unit *unit;
    const sxml_ir_function_reference *function_reference;
} sxml_function_reference_request;

/* Makes the function item, owned, in *item (XPath 4.0, section 4.5.4): a
   native row's body, an IR function's body or a constructor body. Returns
   the name pool's and heap's failures. */
SXML_NODISCARD sxml_status sxml_evaluator_make_function_reference(
    sxml_dynamic_context *dynamic_context,
    const sxml_function_reference_request *request, sxml_item *item);

/* A function of the calling unit to find by name and arity. */
typedef struct sxml_unit_function_query {
    sxml_name_identifier function_name;
    size_t arity;
    /* The caller's focus (context value, position, size; borrowed), which
       a function that takes it captures (an XQuery declared function's
       shorter arity, whose focus slots follow its parameters); NULL when
       the caller has none, and the function then reads an absent focus
       (XPDY0002). */
    const sxml_sequence *caller_focus;
} sxml_unit_function_query;

/* fn:function-lookup's search of the unit of the task that calls it (F&O
   4.0, 13.1.1: the "named function definitions" of the dynamic context):
   a top-level IR function of the name and arity, such as an XQuery
   declared function or an XSLT stylesheet function, made a function item
   as a named function reference makes it (capturing query.caller_focus
   when the function's focus is the three slots after its parameters,
   XQuery 4.0, 5.18.3; F&O 4.0, 13.1.1, PR 2941), in *item; item->representation
   stays 0 when there is none. Returns the heap's failures. */
SXML_NODISCARD sxml_status sxml_evaluator_find_unit_function(
    sxml_dynamic_context *dynamic_context, sxml_unit_function_query query, sxml_item *item);

/* The parameter and result types of a function item's signature. */
typedef struct sxml_call_signature {
    /* False for a map or array (called with one atomic key or position)
       and for function(*), whose arguments are not coerced. */
    bool has_function_type;
    sxml_sequence_type_view parameter_types;
    sxml_sequence_type result_type;
} sxml_call_signature;

/* Writes the signature of function_item to *signature, borrowed from the
   type registry. Returns SXML_STATUS_TYPE_ERROR (XPTY0004) when
   function_item is not a function item. */
SXML_NODISCARD sxml_status sxml_evaluator_describe_call_signature(
    sxml_dynamic_context *dynamic_context, sxml_item function_item,
    sxml_call_signature *signature);

/* A fold's question (packet 2.6, D115): does every later call return the
   accumulator unchanged? */
typedef struct sxml_fixpoint_question {
    /* The fold's function item, borrowed. */
    sxml_item function_item;
    /* The accumulator the next call would get, borrowed. */
    sxml_sequence accumulator;
    /* Its argument position: 0 (fold-left) or 1 (fold-right). */
    size_t accumulator_index;
} sxml_fixpoint_question;

typedef struct sxml_fixpoint_answer {
    /* True only when it is certain that function_item called with
       accumulator in that position returns accumulator, whatever the
       other argument. */
    bool is_fixpoint;
} sxml_fixpoint_answer;

/*
 * Answers question for an inline function (also behind function coercion
 * to item()* parameters and result) whose body is "if (C) then $acc else
 * E" or "if (C) then E else $acc", C a value or general comparison of
 * numeric constants, captured variables and $acc: C is evaluated here
 * for accumulator. Every other function is not a fixpoint. Defined in
 * src/evaluator_function.c; never raises (a failing C answers false).
 */
SXML_NODISCARD sxml_status sxml_evaluator_test_fixpoint(
    sxml_dynamic_context *dynamic_context, const sxml_fixpoint_question *question,
    sxml_fixpoint_answer *answer);
