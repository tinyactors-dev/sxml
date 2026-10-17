#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * The native function table (docs/DECISIONS.md, D38, D46, D55). Internal
 * to sxml.
 *
 * Functions written in C (the F&O 4.0 library) are described by rows of
 * static const data, one row array per source file:
 *
 *     src/function_library_numeric.c      packet 1.20
 *     src/function_library_string.c       packet 1.20
 *     src/function_library_sequence.c     packet 1.20
 *     src/function_library_node.c         packet 1.20
 *     src/function_library_context.c      packet 1.20
 *     src/function_library_error.c        packet 1.20
 *     src/function_library_higher_order.c packet 1.21
 *     src/function_library_map.c          packet 1.21
 *     src/function_library_array.c        packet 1.21
 *     src/function_library_json.c         packet 1.22
 *     src/function_library_uri.c          packet 1.23
 *     src/function_library_math.c         packet 1.23b
 *     src/function_library_qname.c        packet 1.23b
 *
 * Each file defines one sxml_native_function_view below over its rows, so
 * no file lists another file's functions. src/function_library.c (packet
 * 1.20) implements sxml_function_library: an index over whichever parts
 * its configuration names, which resolves the type text of a row the first
 * time the row is found and serves static analysis through the
 * sxml_function_table interface. The processor (packet 1.23) names all
 * parts; unit tests name the parts they test. xs: constructor functions
 * have no rows: the library answers them from the type registry.
 *
 * A row's types are written as text in the F&O 4.0 spelling, so a row
 * reads like the specification's signature:
 *
 *     static const sxml_function_parameter filter_parameters[] = {
 *         {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("input"),
 *          .parameter_type = SXML_BYTE_VIEW_INITIALIZER("item()*")},
 *         {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("predicate"),
 *          .parameter_type = SXML_BYTE_VIEW_INITIALIZER(
 *              "fn(item(), xs:integer) as xs:boolean?")},
 *     };
 *     static const sxml_native_function higher_order_functions[] = {
 *         {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
 *          .local_name = SXML_BYTE_VIEW_INITIALIZER("filter"),
 *          .parameters = {filter_parameters, 2},
 *          .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
 *          .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
 *          .resumable_body = &filter_body},
 *     };
 *
 * Type text is parsed with the XPath front end
 * (sxml_xpath_build_sequence_type_ast, src/xpath_ast_internal.h) and
 * resolved by static analysis (packet 1.12) with the prefixes xs, fn, map,
 * array, math and err bound; the resolved types are cached per library,
 * since rows are constant.
 *
 * Thread safety: rows are static const data, safe from any thread. A
 * library is used by the thread of its item heap (D35).
 */

/* -------------------------------------------------------------- Rows */

/* How a parameter that a call leaves out gets its value. */
typedef enum sxml_parameter_default {
    /* The parameter is required: every arity includes it. */
    SXML_PARAMETER_DEFAULT_REQUIRED,
    /* "$p := ." (F&O 4.0, section 1.5 "Function signatures"): the caller
       passes the context value. Lowering supplies it in a static call
       (raising XPDY0002 when the focus is absent), and a function item of
       the shorter arity captures the focus when it is made (XPath 4.0,
       section 4.5.4 "Named Function References"). */
    SXML_PARAMETER_DEFAULT_CONTEXT_VALUE,
    /* Any other default ("()", "\"\"", the default collation, ...): the
       body receives fewer arguments and supplies the value itself. */
    SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY,
} sxml_parameter_default;

/*
 * One parameter of a function: for keyword arguments (XPath 4.0, section
 * 4.5.1.1), coercion and arity. Also used by function tables of other
 * kinds, such as XQuery declared functions (Phase 3).
 */
typedef struct sxml_function_parameter {
    /* The NCName of the parameter, without "$". */
    sxml_byte_view parameter_name;
    /* An XPath 4.0 SequenceType as text, F&O spelling with the default
       prefixes above. */
    sxml_byte_view parameter_type;
    sxml_parameter_default parameter_default;
} sxml_function_parameter;

typedef struct sxml_function_parameter_view {
    const sxml_function_parameter *function_parameters;
    size_t function_parameter_count;
} sxml_function_parameter_view;

/*
 * Properties of a native function (bits of sxml_native_function.properties)
 * that lowering, the optimizer and the evaluator need to know.
 */
typedef enum sxml_native_function_property {
    /* The last parameter repeats: any arity from the required count up
       (fn:concat, F&O 4.0, section 5.4.1). */
    SXML_NATIVE_FUNCTION_IS_VARIADIC = 1U << 0U,
    /* The body reads the dynamic context beyond its arguments: the clock,
       documents, the implicit timezone (F&O 4.0, section 1.7 "Properties
       of functions": context-dependent). A pass must not evaluate a call
       at compile time. */
    SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT = 1U << 1U,
    /* Two calls with the same arguments may give different results
       (nondeterministic, F&O 4.0, section 1.7), such as
       fn:random-number-generator without a seed. */
    SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC = 1U << 2U,
    /* The body calls function items it receives (higher-order). Such a
       row has a resumable_body (see "Resumable native bodies" below): it
       asks the evaluator to make each call and never calls a function item
       itself, so the evaluator is never re-entered on the C stack (D71,
       D75). It needs a dynamic context. */
    SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS = 1U << 3U,
    /* fn:position: lowering reads the context position slot instead of
       calling the row; call is NULL (XPath 4.0, section 2.2.2). */
    SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION = 1U << 4U,
    /* fn:last: lowering reads the context size slot; call is NULL. */
    SXML_NATIVE_FUNCTION_READS_CONTEXT_SIZE = 1U << 5U,
} sxml_native_function_property;

/*
 * The C body of a native function. call->arguments has as many sequences
 * as the arity of the call, already coerced to the parameter types
 * (XPath 4.0, section 3.4); parameters with SXML_PARAMETER_DEFAULT_
 * SUPPLIED_BY_BODY beyond that arity are the body's to fill.
 * call->function_item is a zero item (not an item) for a static call and
 * the function item for a dynamic one; native rows capture nothing, so they
 * do not read it. Writes the result, owned, to *result; raises errors
 * through sxml_item_heap_raise_error or with a static error record.
 */
typedef sxml_status (*sxml_native_function_call)(
    const sxml_function_call *call, sxml_sequence *result);

/* ------------------------------------------------ Resumable native bodies */

/*
 * A native function that calls function items (fn:for-each, fn:filter,
 * fn:fold-left, fn:fold-right, fn:sort with a key, fn:apply, map:for-each,
 * array:for-each, fn:partial-apply's result, ...) is written as a
 * resumable body: a step function that the evaluator runs again and again
 * (D71, D75). Each native step either finishes with the value of the call
 * or asks the evaluator to call one function item with some arguments; the
 * evaluator makes that call on its own task stack (coercing the arguments
 * as a dynamic call does, XPath 4.0, section 4.5.3) and runs the next step
 * with the result. Nothing calls back into the evaluator on the C stack,
 * so fold-right over a million items needs no C stack per item.
 *
 * The evaluator gives every call of such a body state_byte_count bytes of
 * state, zeroed before the first step, at state_alignment, and calls
 * release_state once after the last step, whether the steps finished, a
 * step failed or the requested call failed, so the state may hold owned
 * values (an accumulator, a sequence builder).
 *
 *     typedef struct fold_left_state {
 *         uint64_t next_index;
 *         sxml_sequence accumulator;          // owned
 *         sxml_sequence call_arguments[2];    // owned while a call runs
 *     } fold_left_state;
 *
 *     static sxml_status step_fold_left(const sxml_native_step_request *request,
 *                                       sxml_native_step *step) {
 *         fold_left_state *state = request->state;
 *         if (request->step_number == 0) {
 *             state->accumulator = request->call->arguments.sequences[1];
 *             sxml_sequence_retain(state->accumulator);
 *         } else {
 *             release call_arguments; replace accumulator with
 *             request->call_result (owned: taken over)
 *         }
 *         read item next_index of the input; at the end:
 *             *step = (sxml_native_step){.step_kind = FINISH,
 *                                        .result = accumulator};
 *             state->accumulator = sxml_sequence_empty();
 *         otherwise:
 *             call_arguments = {accumulator retained, item retained};
 *             *step = (sxml_native_step){.step_kind = CALL,
 *                 .function_item = arguments[2]'s item,
 *                 .arguments = {state->call_arguments, 2}};
 *     }
 */

/* What a native step asks for. */
typedef enum sxml_native_step_kind {
    /* The call is complete: result holds its value. */
    SXML_NATIVE_STEP_KIND_FINISH,
    /* Call function_item with arguments, then run the next step with the
       result. */
    SXML_NATIVE_STEP_KIND_CALL,
} sxml_native_step_kind;

/* The outcome of one native step. */
typedef struct sxml_native_step {
    sxml_native_step_kind step_kind;
    /* FINISH: the value of the call, owned; the evaluator takes it. */
    sxml_sequence result;
    /* CALL: the function item and its arguments, borrowed (from the
       state or the call's arguments) until the next step runs; the
       evaluator retains what it keeps. */
    sxml_item function_item;
    sxml_sequence_view arguments;
} sxml_native_step;

/* What one native step receives. */
typedef struct sxml_native_step_request {
    /* The call of the native function, as sxml_native_function_call
       receives it (arguments coerced, dynamic context, heap, error
       record). Borrowed for every step of the call. */
    const sxml_function_call *call;
    /* state_byte_count bytes owned by the evaluator, zeroed before step
       0 and kept between steps. */
    void *state;
    /* 0 for the first step, then 1, 2, ... */
    uint64_t step_number;
    /* From the second step on: the value of the call the previous step
       asked for, owned: the step takes it over (keeps or releases it).
       Empty in the first step. */
    sxml_sequence call_result;
} sxml_native_step_request;

/*
 * Runs one native step and writes its outcome to *step. Fails as a native
 * function call does (errors raised through the heap or a static error
 * record); after a failure no further step runs, and release_state still
 * does.
 */
typedef sxml_status (*sxml_native_step_function)(
    const sxml_native_step_request *request, sxml_native_step *step);

/* A resumable body: the step function and the state it needs. */
typedef struct sxml_resumable_native_body {
    sxml_native_step_function step;
    /* Releases what the state holds; called once after the last step of
       every call. May be NULL when the state holds nothing to release. */
    void (*release_state)(void *state);
    /* Bytes and alignment of the state; byte count 0 for no state. */
    size_t state_byte_count;
    size_t state_alignment;
} sxml_resumable_native_body;

/* One function implemented in C: name, arities, signature, body. */
typedef struct sxml_native_function {
    /* fn, map, array or math; the namespace of the function's name. */
    sxml_well_known_namespace function_namespace;
    /* Bits of sxml_native_function_property. Next to function_namespace
       so the struct has no padding (D63). */
    uint32_t properties;
    /* An NCName, such as "string-join". */
    sxml_byte_view local_name;
    /* In order. The arities are from the count of required parameters up
       to function_parameter_count, or without bound when variadic. */
    sxml_function_parameter_view parameters;
    /* An XPath 4.0 SequenceType as text. */
    sxml_byte_view result_type;
    /* The body, for a row that calls no function items. NULL with
       SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION or _SIZE, and for a row
       with a resumable_body. */
    sxml_native_function_call call;
    /* The body, for a row with SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS
       (static const data); NULL otherwise. A row has call or
       resumable_body, never both. */
    const sxml_resumable_native_body *resumable_body;
} sxml_native_function;

typedef struct sxml_native_function_view {
    const sxml_native_function *native_functions;
    size_t native_function_count;
} sxml_native_function_view;

/*
 * The parts of the library, one per source file (see above). Each is
 * defined by the packet that owns its file; a part that is not yet written
 * is simply not named by any configuration, so nothing links to it.
 */
extern const sxml_native_function_view sxml_function_library_numeric;
extern const sxml_native_function_view sxml_function_library_string;
extern const sxml_native_function_view sxml_function_library_sequence;
extern const sxml_native_function_view sxml_function_library_node;
extern const sxml_native_function_view sxml_function_library_context;
extern const sxml_native_function_view sxml_function_library_error;
extern const sxml_native_function_view sxml_function_library_higher_order;
extern const sxml_native_function_view sxml_function_library_map;
extern const sxml_native_function_view sxml_function_library_array;
extern const sxml_native_function_view sxml_function_library_json;
/* fn:static-base-uri and fn:resolve-uri (src/function_library_uri.c,
   packet 1.23, D88). */
extern const sxml_native_function_view sxml_function_library_uri;
/* The math namespace (src/function_library_math.c, packet 1.23b, D91). */
extern const sxml_native_function_view sxml_function_library_math;
/* QNames and in-scope namespaces: fn:namespace-uri-from-QName,
   fn:local-name-from-QName, fn:prefix-from-QName, fn:expanded-QName,
   fn:in-scope-prefixes, fn:in-scope-namespaces, fn:namespace-uri-for-prefix
   (src/function_library_qname.c, packet 1.23b, D91). */
extern const sxml_native_function_view sxml_function_library_qname;

/*
 * The operations every function item made from a native row uses (a named
 * function reference such as fn:abs#1): state is the row, cast from const
 * and never written; retain and release are NULL because rows are static.
 * call runs row->call, or, for a row with a resumable_body, runs its steps
 * on the evaluator of call->dynamic_context, which must not be evaluating
 * already (SXML_STATUS_INVALID_ARGUMENT; within an evaluation the
 * evaluator recognizes these operations and drives the steps itself).
 * Defined in src/evaluator_function.c (packet 1.18, D75), because the
 * evaluator dispatches on it.
 */
extern const sxml_function_body_operations sxml_native_function_body_operations;

/* ------------------------------------------------------ Function tables */

/* A function to find: the name as resolved by static analysis, and the
   number of arguments of the call or reference. */
typedef struct sxml_function_query {
    sxml_name_identifier function_name;
    size_t arity;
} sxml_function_query;

/* What a function table found. */
typedef enum sxml_function_entry_kind {
    /* No function has this name and arity (XPST0017 for the caller). */
    SXML_FUNCTION_ENTRY_KIND_NONE,
    /* A native row. */
    SXML_FUNCTION_ENTRY_KIND_NATIVE,
    /* An xs: constructor function: a cast to constructor_type that
       accepts the empty sequence (XPath 4.0, section 4.5.1.3, "Constructor
       Functions"). */
    SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR,
    /* A function of the unit being compiled (an XQuery declared function,
       an XSLT xsl:function; Phase 3 and 4): ir_function_number is its
       sxml_ir_function_index value. */
    SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION,
} sxml_function_entry_kind;

/*
 * A function found by name and arity, with its signature resolved for that
 * arity. Views are borrowed from the table and valid as long as it is.
 */
typedef struct sxml_function_entry {
    sxml_function_entry_kind entry_kind;
    /* For SXML_FUNCTION_ENTRY_KIND_NATIVE. */
    const sxml_native_function *native_function;
    /* For SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR: the target atomic type. */
    sxml_type_identifier constructor_type;
    /* For SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION. */
    uint32_t ir_function_number;
    /* All parameters, including those beyond the arity (for keyword
       arguments and defaults). */
    sxml_function_parameter_view parameters;
    /* The registered function type for this arity: what a function item
       made from the entry carries as its signature. */
    sxml_type_identifier signature;
    /* arity entries, resolved: what lowering coerces arguments to. */
    sxml_sequence_type_view parameter_types;
    sxml_sequence_type result_type;
} sxml_function_entry;

/* The operations of a function table. Tables are static const. */
typedef struct sxml_function_table_operations {
    /*
     * Writes what the table holds for query to *entry, with entry_kind
     * SXML_FUNCTION_ENTRY_KIND_NONE when nothing matches. Returns another
     * status only when resolving the entry's types fails (the type
     * registry's or allocator's failures); *entry is then untouched.
     */
    sxml_status (*find_function)(void *state, const sxml_function_query *query,
                                 sxml_function_entry *entry);
} sxml_function_table_operations;

/*
 * How static analysis finds functions by name and arity (XPath 4.0,
 * section 2.2.1, "statically known function definitions"). An interface so
 * that tests of static analysis bring their own table and XQuery can put
 * its declared functions in front of the library.
 */
typedef struct sxml_function_table {
    void *state;
    const sxml_function_table_operations *operations;
} sxml_function_table;

/* Calls table.operations->find_function. */
SXML_NODISCARD sxml_status sxml_function_table_find(
    sxml_function_table table, const sxml_function_query *query,
    sxml_function_entry *entry);

/* ------------------------------------------------------ Function library */

/*
 * The per-processor index over native function parts. Defined in
 * src/function_library.c (packet 1.20).
 */
typedef struct sxml_function_library sxml_function_library;

typedef struct sxml_native_function_view_list {
    const sxml_native_function_view *const *native_function_views;
    size_t native_function_view_count;
} sxml_native_function_view_list;

typedef struct sxml_function_library_configuration {
    /* Memory for the index and the resolved types. Must not be the null
       allocator; must outlive the library. */
    sxml_allocator allocator;
    /* Kept by reference: names are interned in its name pool and types
       registered in its type registry. Must outlive the library. */
    sxml_item_heap *item_heap;
    /* The parts to serve, borrowed; must outlive the library. A name and
       arity served by two rows is SXML_STATUS_INVALID_ARGUMENT. */
    sxml_native_function_view_list parts;
} sxml_function_library_configuration;

SXML_NODISCARD sxml_status sxml_function_library_memory_requirements(
    const sxml_function_library_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a library in memory and indexes the names of every row of the
 * parts (interning them in the name pool). Types are resolved later, when
 * a row is first found. Returns SXML_STATUS_INVALID_ARGUMENT for an invalid
 * configuration, the memory checks of the conventions, and the failures of
 * the name pool and allocator.
 */
SXML_NODISCARD sxml_status sxml_function_library_initialize(
    sxml_memory_view memory,
    const sxml_function_library_configuration *configuration,
    sxml_function_library **library);

void sxml_function_library_finalize(sxml_function_library *library);

SXML_NODISCARD sxml_status sxml_function_library_create(
    sxml_allocator allocator,
    const sxml_function_library_configuration *configuration,
    sxml_function_library **library);

void sxml_function_library_destroy(sxml_function_library *library);

/*
 * The library as a function table. It finds native rows and xs:
 * constructor functions; a type text that does not parse or resolve is an
 * error in sxml itself, reported as SXML_STATUS_INVALID_ARGUMENT. The
 * table keeps a reference to library.
 */
sxml_function_table sxml_function_library_as_table(
    sxml_function_library *library);
