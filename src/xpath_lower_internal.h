#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_parts_internal.h"
#include "ir_internal.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "xpath_ast_internal.h"
#include "xpath_static_internal.h"

/*
 * The lowering machine and lowering parts (docs/DECISIONS.md, D46, D63,
 * D71, D103). Internal to sxml.
 *
 * Lowering (src/xpath_lower.c) translates an AST into IR expressions of a
 * unit (src/ir_internal.h). It never recurses on the C stack (D71): each
 * construct that lowers parts of itself is a lowering procedure, run by a
 * lowering frame in allocator memory on the lowering machine's stack. A
 * procedure's step function either finishes its frame, delivering its
 * result, or asks for a part (sxml_xpath_lowering_call_expression,
 * _call_atom, _call_body, _call_focused_body, _call_embedded,
 * _push_procedure) and returns; the machine runs the frame on top until it
 * finishes, then the one below it again. A step never runs a machine
 * itself: sxml_xpath_lowerer_run_procedure and
 * sxml_xpath_lowerer_lower_embedded are for callers outside one.
 *
 * Administrative normal form (D46): every operand of an IR expression is
 * an atom, so a part that is not a literal, a variable or the focus is
 * computed first and bound by a LET in a lowering block. Binding appends a
 * LET whose body is filled later, so the LETs nest in evaluation order;
 * branches, loop bodies and function bodies are blocks of their own.
 *
 * Lowering parts. The core (src/xpath_lower.c) lowers XPath 4.0. A
 * lowering part is a table of AST kinds and the procedure or leaf
 * function lowering each, in a file of its own that defines
 *
 *     const sxml_xpath_lowering_part sxml_xpath_lowering_part_<part> = ...;
 *
 * CMake (cmake/SxmlParts.cmake) reads every src/<name>_lower.c and
 * src/<name>_lower_<part>.c for that definition and generates the
 * registry, sxml_xpath_lowering_registered_parts(), so XQuery (packets 3.7 to 3.10) and XSLT (4.5 to
 * 4.11) add files, not lines. CMake also reads the definition's
 * .languages and generates one list per language,
 * sxml_xpath_lowering_<language>_parts(); each language's entry point
 * names its own list, so a module that offers only XPath links no
 * XQuery-only part (D227). The lowering of a kind is the rule of the
 * first part, in registry order, that serves the lowerer's language and
 * lists the kind; then the core's. A part may list kinds the core lowers
 * (XQuery's FLWOR clauses) and serve only its language; no two registered
 * parts may list the same kind for a common language (a unit test checks
 * it). A part may have no rules and only procedures that its own code
 * pushes (XSLT instructions over the stylesheet AST of packet 4.2,
 * D102).
 *
 * The embedding entry. sxml_xpath_lowering_call_embedded (inside a
 * machine) and sxml_xpath_lowerer_lower_embedded (outside) lower one XPath
 * AST inside the caller's scope: into the current function, with the
 * variables the caller pushed (sxml_xpath_lowerer_push_variable) in scope
 * as their slots, a focus the caller chooses or the current one, and the
 * AST's own static context (namespaces, the in-scope functions of an XSLT
 * stylesheet through its function table), as XSLT attributes need.
 *
 * Errors: as sxml_xpath_lower (src/xpath_static_internal.h). After a
 * failure the lowerer is fit only for finalize: its scope, focus and AST
 * may be those of the construct that failed.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* ------------------------------------------------------------- State */

/* The lowering parts a lowerer uses (see "Parts" below). */
typedef struct sxml_xpath_lowering_registered_part
    sxml_xpath_lowering_registered_part;

typedef struct sxml_xpath_lowering_part_view {
    const sxml_xpath_lowering_registered_part *parts;
    size_t part_count;
} sxml_xpath_lowering_part_view;

/* A variable in scope: innermost last. */
typedef struct sxml_xpath_lowering_scope_entry {
    sxml_name_identifier variable_name;
    /* The level of the function that binds it: 0 for the main function,
       one more for each inline function inside. */
    uint32_t function_level;
    /* Where its value is, in that function's frame. */
    sxml_ir_atom atom;
} sxml_xpath_lowering_scope_entry;

/* A free variable of an inline function: a captured slot, or the
   constant a let bound it to. */
typedef struct sxml_xpath_lowering_capture_entry {
    sxml_name_identifier variable_name;
    sxml_ir_atom atom;
} sxml_xpath_lowering_capture_entry;

typedef enum sxml_xpath_lowering_focus_kind {
    /* No focus: an inline function body (reading it raises XPDY0002,
       XPath 4.0, section 4.5.6). */
    SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT,
    /* Slots made on first read: the main function, MAP and FILTER. */
    SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
    /* A fixed value, position and size 1: a focus function, "->". */
    SXML_XPATH_LOWERING_FOCUS_KIND_FIXED,
} sxml_xpath_lowering_focus_kind;

/* The focus of the function being lowered. */
typedef struct sxml_xpath_lowering_focus {
    sxml_xpath_lowering_focus_kind kind;
    /* SLOTS: the slots read so far; a part never read stays 0. */
    sxml_ir_focus slots;
    /* SLOTS: the slots of the parts read so far, also of preset slots
       (a default function's focus, glossary `default_function`). */
    sxml_ir_focus parts_read;
    /* FIXED: the context value. */
    sxml_ir_atom fixed_value;
} sxml_xpath_lowering_focus;

/* The IR function being lowered. */
typedef struct sxml_xpath_lowering_function {
    sxml_ir_function_index function_index;
    uint32_t function_level;
    /* Slots used so far; sxml_xpath_lowerer_add_slot adds one. */
    uint32_t slot_count;
    sxml_xpath_lowering_focus *focus;
    /* The free variables of an inline function and their slots. */
    sxml_xpath_lowering_capture_entry *captures;
    size_t capture_count;
} sxml_xpath_lowering_function;

/*
 * A lowering override (glossary `lowering_override`, D223): one node of
 * one AST that a procedure of the caller lowers instead of the rule of the
 * node's kind, wherever the node is lowered. The procedure's frame is
 * pushed as the kind's frame would be (the node, block, body and
 * destination of the call), its locals starting with
 * sxml_xpath_lowering_override_locals. XSLT's streamed paths use it
 * (src/xslt_lower_stream.c): a path inside an instruction's expression
 * lowers to the STREAM chain that selects its nodes as their events pass.
 */
typedef struct sxml_xpath_lowering_procedure sxml_xpath_lowering_procedure;

typedef struct sxml_xpath_lowering_override {
    /* Borrowed; compared by address. */
    const sxml_ast *ast;
    sxml_ast_node_index node;
    const sxml_xpath_lowering_procedure *procedure;
    /* Copied into the locals: the caller's state and a number of its own. */
    const void *state;
    size_t number;
} sxml_xpath_lowering_override;

/* What the locals of an override's procedure start with. */
typedef struct sxml_xpath_lowering_override_locals {
    const void *state;
    size_t number;
} sxml_xpath_lowering_override_locals;

typedef struct sxml_xpath_lowering_override_view {
    const sxml_xpath_lowering_override *overrides;
    size_t override_count;
} sxml_xpath_lowering_override_view;

/*
 * The state of one lowering into one unit. Parts read its fields; they
 * change them only through the functions below. Every pointer is
 * borrowed: the static context, unit and error record outlive the
 * lowerer.
 */
typedef struct sxml_xpath_lowerer {
    /* The AST being lowered; NULL until an embedding gives one. */
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
    sxml_error_record *error_record;
    sxml_ir_unit *unit;
    /* The static context's allocator: scratch arrays and frames. */
    sxml_allocator allocator;
    /* The allocator of sxml_xpath_lowerer_create; null when initialized in
       caller memory. */
    sxml_allocator owning_allocator;
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    sxml_item_heap *item_heap;
    /* The language whose parts apply. */
    sxml_language language;
    sxml_xpath_lowering_part_view parts;
    sxml_xpath_lowering_scope_entry *scope_entries;
    size_t scope_entry_count;
    size_t scope_entry_capacity;
    /* The function being lowered; NULL outside one. */
    sxml_xpath_lowering_function *function;
    /* Per node of ast, whether its subtree has a variable reference; made
       for the first inline function. */
    bool *has_variable_reference;
    /* The focus and function the innermost embedding started with (what
       SXML_XPATH_LOWERING_FOCUS_PART_CURRENT_ITEM reads); NULL outside
       one. */
    sxml_xpath_lowering_focus *embedding_focus;
    const sxml_xpath_lowering_function *embedding_function;
    /* Whether a machine of this lowerer runs (sxml_xpath_lowerer_run_procedure
       or sxml_xpath_lowerer_lower_embedded): a step may not start another
       (D71, D161). */
    bool is_running;
    /* Levels of maximum_expression_depth in use. */
    uint32_t depth;
    uint32_t maximum_depth;
    /* The lowering overrides in effect (see above); borrowed, set and
       kept up to date by the caller that owns them; empty for none. */
    sxml_xpath_lowering_override_view overrides;
    /* Constants made once per unit; index 0 until first needed. */
    sxml_ir_constant_index true_constant;
    sxml_ir_constant_index false_constant;
    sxml_ir_constant_index one_constant;
} sxml_xpath_lowerer;

/* The LETs bound while lowering one expression; see the file comment. */
typedef struct sxml_xpath_lowering_block {
    sxml_ir_expression_index first;
    sxml_ir_expression_index open_let;
} sxml_xpath_lowering_block;

/* An AST node to lower and the block its LETs go to. */
typedef struct sxml_xpath_lowering_node_in_block {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index node;
} sxml_xpath_lowering_node_in_block;

/* ------------------------------------------------------- The lowerer */

typedef struct sxml_xpath_lowerer_configuration {
    /* Borrowed for the lowerer's life: its item heap and allocator serve
       every embedding. */
    const sxml_xpath_static_context *static_context;
    /* The unit lowering adds to; borrowed for the lowerer's life. */
    sxml_ir_unit *unit;
    /* Whose lowering parts apply; 0 means SXML_LANGUAGE_XPATH. */
    sxml_language language;
    /* The parts to use: the language's list,
       sxml_xpath_lowering_<language>_parts() (D227); parts NULL means
       sxml_xpath_lowering_xpath_parts(). Borrowed for the lowerer's
       life. */
    sxml_xpath_lowering_part_view parts;
    /* As sxml_xpath_lowering_request.maximum_expression_depth. */
    uint32_t maximum_expression_depth;
    /* Where static errors go; may be NULL. */
    sxml_error_record *error_record;
} sxml_xpath_lowerer_configuration;

/* Size and alignment of a lowerer. Returns SXML_STATUS_INVALID_ARGUMENT
   for a configuration without a static context or unit, or with a
   language that is not one sxml_language. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_memory_requirements(
    const sxml_xpath_lowerer_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places a lowerer in memory (no function, empty scope, no AST). Returns
   the failures of memory_requirements, SXML_STATUS_INSUFFICIENT_MEMORY,
   SXML_STATUS_MISALIGNED_MEMORY, and SXML_STATUS_INVALID_ARGUMENT for an
   invalid static context (as sxml_xpath_lower). */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_initialize(
    sxml_memory_view memory, const sxml_xpath_lowerer_configuration *configuration,
    sxml_xpath_lowerer **lowerer);

/* Releases the lowerer's scratch arrays, not its memory. NULL is
   allowed. */
void sxml_xpath_lowerer_finalize(sxml_xpath_lowerer *lowerer);

/* As initialize, in memory from allocator, which destroy releases. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_create(
    sxml_allocator allocator, const sxml_xpath_lowerer_configuration *configuration,
    sxml_xpath_lowerer **lowerer);

/* Finalizes and releases a created lowerer. NULL is allowed. */
void sxml_xpath_lowerer_destroy(sxml_xpath_lowerer *lowerer);

/* -------------------------------------------------- Building blocks */

/* code_and_description is "CODE description": an eight-character error
   code, a space and the description, static text. Fills the error record
   at node of the lowerer's AST and returns SXML_STATUS_STATIC_ERROR. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_report_static_error(
    const sxml_xpath_lowerer *lowerer, sxml_ast_node_index node,
    sxml_byte_view code_and_description);

/* Fills the diagnostic (static text) at node and returns
   SXML_STATUS_UNSUPPORTED_FEATURE. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_report_unsupported(
    const sxml_xpath_lowerer *lowerer, sxml_ast_node_index node,
    sxml_byte_view description);

/* An array of element_count elements (at least one), allocated with the
   lowerer's allocator; release it with sxml_xpath_lowerer_release_scratch. */
typedef struct sxml_xpath_lowering_scratch_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} sxml_xpath_lowering_scratch_request;

SXML_NODISCARD sxml_status sxml_xpath_lowerer_allocate_scratch(
    const sxml_xpath_lowerer *lowerer,
    sxml_xpath_lowering_scratch_request request, void **memory);

/* NULL is allowed. */
void sxml_xpath_lowerer_release_scratch(const sxml_xpath_lowerer *lowerer,
                                        void *memory);

/* An expression to add, with the source span of node. */
typedef struct sxml_xpath_lowering_expression_to_add {
    sxml_ast_node_index node;
    sxml_ir_expression expression;
} sxml_xpath_lowering_expression_to_add;

SXML_NODISCARD sxml_status sxml_xpath_lowerer_add_expression(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_expression_to_add request,
    sxml_ir_expression_index *index);

/* A new slot of the current function. SXML_STATUS_LIMIT_EXCEEDED when
   none is left. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_add_slot(
    sxml_xpath_lowerer *lowerer, sxml_ir_slot *slot);

/* Binds expression in block (a LET to a new slot, whose atom it writes)
   unless it is an atom expression, whose atom it writes. */
typedef struct sxml_xpath_lowering_expression_in_block {
    sxml_xpath_lowering_block *block;
    sxml_ir_expression_index expression;
} sxml_xpath_lowering_expression_in_block;

SXML_NODISCARD sxml_status sxml_xpath_lowerer_bind_expression(
    sxml_xpath_lowerer *lowerer,
    sxml_xpath_lowering_expression_in_block request, sxml_ir_atom *atom);

/* Ends a block with result: the outermost LET, or result itself. */
typedef struct sxml_xpath_lowering_block_result {
    sxml_xpath_lowering_block block;
    sxml_ir_expression_index result;
} sxml_xpath_lowering_block_result;

SXML_NODISCARD sxml_status sxml_xpath_lowerer_finish_block(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_block_result request,
    sxml_ir_expression_index *expression);

/* One item of content just lowered into block (D166): the block's open
   LET before its lowering began, and the item's atom. */
typedef struct sxml_xpath_lowering_content_item {
    sxml_xpath_lowering_block *block;
    sxml_ir_expression_index open_let_before;
    sxml_ir_atom atom;
} sxml_xpath_lowering_content_item;

/* Whether marking content LETs may go on after an item. */
typedef struct sxml_xpath_lowering_content_mark {
    /* False for an item that has no LET of its own (a constant, a
       variable bound before) and is not the empty sequence: the items
       after it must not be marked, as their values would be delivered
       before its value. */
    bool goes_on;
} sxml_xpath_lowering_content_mark;

/*
 * Marks the LET made for item as a content LET (glossary `content_let`,
 * sxml_ir_let.is_content): the block's open LET when it was made after
 * open_let_before and binds item.atom. The caller marks the items of a
 * content in order, from the first, while mark->goes_on stays true, and
 * only when its block ends with a result that reads them (directly or
 * through LETs of SEQUENCEs of them) before any other atom and nowhere
 * else (a sequence constructor's atoms, a literal result element's
 * content).
 */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_mark_content_let(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_content_item item,
    sxml_xpath_lowering_content_mark *mark);

/* "()" at node. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_add_empty_sequence(
    sxml_xpath_lowerer *lowerer, sxml_ast_node_index node,
    sxml_ir_expression_index *expression);

/* Adds item, owned, as a constant of the unit and writes its atom; the
   item is released either way. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_add_item_constant(
    sxml_xpath_lowerer *lowerer, sxml_item item, sxml_ir_atom *atom);

/* An xs:string constant of UTF-8 text (copied). */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_add_string_constant(
    sxml_xpath_lowerer *lowerer, sxml_byte_view text, sxml_ir_atom *atom);

/* A RAISE of an error known statically (err namespace). */
typedef struct sxml_xpath_lowering_raise_request {
    sxml_ast_node_index node;
    sxml_status error_status;
    /* Static ASCII, such as "XPDY0002". */
    sxml_byte_view code_local_name;
    /* Static text. */
    sxml_byte_view description;
} sxml_xpath_lowering_raise_request;

SXML_NODISCARD sxml_status sxml_xpath_lowerer_add_raise(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_raise_request request,
    sxml_ir_expression_index *expression);

/* ---------------------------------------------------------- Variables */

/* Puts entry in scope, innermost. Entries go out of scope when the
   function or the construct that pushed them ends
   (sxml_xpath_lowerer_leave_function, sxml_xpath_lowerer_pop_variables). */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_push_variable(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_scope_entry entry);

/* XSLT (26.3.1): clears the current output URI in the function being
   lowered, for the rest of it: fn:current-output-uri() there is the
   empty sequence, and current-output-uri#0 returns it. For stylesheet
   functions, global variables and parameters, and patterns. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_clear_output_uri(sxml_xpath_lowerer *lowerer);

/* Takes the entries pushed after the first entry_count out of scope. */
void sxml_xpath_lowerer_pop_variables(sxml_xpath_lowerer *lowerer,
                                      size_t entry_count);

/* The expanded name of a variable name node of the lowerer's AST (no
   default namespace; XPST0081). */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_resolve_variable_name(
    sxml_xpath_lowerer *lowerer, sxml_ast_node_index name_node,
    sxml_name_identifier *name);

/* A variable of the AST (a VARIABLE_NAME_AND_TYPE node) bound to value,
   coerced in block when declared with a type, then pushed on the
   scope. */
typedef struct sxml_xpath_lowering_typed_variable {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index variable;
    sxml_ir_atom value;
} sxml_xpath_lowering_typed_variable;

SXML_NODISCARD sxml_status sxml_xpath_lowerer_push_typed_variable(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_typed_variable request);

/* -------------------------------------------------------------- Focus */

typedef enum sxml_xpath_lowering_focus_part {
    SXML_XPATH_LOWERING_FOCUS_PART_VALUE,
    SXML_XPATH_LOWERING_FOCUS_PART_POSITION,
    SXML_XPATH_LOWERING_FOCUS_PART_SIZE,
    /* XSLT's current item (glossary `current_item`, D149): the context
       value of the focus the innermost embedding started with, the
       current focus outside an embedding. */
    SXML_XPATH_LOWERING_FOCUS_PART_CURRENT_ITEM,
} sxml_xpath_lowering_focus_part;

/* One part of the current function's focus, read at node. */
typedef struct sxml_xpath_lowering_focus_read {
    /* Where a RAISE of XPDY0002 is bound when the focus is absent. */
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index node;
    sxml_xpath_lowering_focus_part part;
} sxml_xpath_lowering_focus_read;

/* Writes the atom of the focus part, making its slot on first read. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_read_focus(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_focus_read request,
    sxml_ir_atom *atom);

/* The %annotations of owner (an inline function, a function declaration;
   XQuery 4.0, 5.15): names by the annotation namespace rule, values the
   literals' items, copied into the unit (what an IR function's
   function_annotations borrows). Returns the unit's failures. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_read_annotations(
    sxml_xpath_lowerer *lowerer, sxml_ast_node_index owner,
    sxml_function_annotation_view *annotations);

/* A call of a default function (glossary `default_function`) at node
   with the parts of the current focus its IR function's focus names,
   the empty sequence for the others, bound in block. */
typedef struct sxml_xpath_lowering_default_call {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index node;
    uint32_t default_function_number;
} sxml_xpath_lowering_default_call;

/* Writes the atom of the default's value. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_call_default_function(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_default_call request,
    sxml_ir_atom *atom);

/* -------------------------------------------------------------- Types */

/* sxml_xpath_resolve_sequence_type and _item_type for a node of the
   lowerer's AST in its static context. */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_resolve_sequence_type(
    sxml_xpath_lowerer *lowerer, sxml_ast_node_index node,
    sxml_sequence_type *sequence_type);

SXML_NODISCARD sxml_status sxml_xpath_lowerer_resolve_item_type(
    sxml_xpath_lowerer *lowerer, sxml_ast_node_index node,
    sxml_type_identifier *item_type);

/* The node test of an axis step of another AST, as lowering the step
   would register it (a name test gets the axis's principal node kind):
   for the stream points of src/xslt_lower_stream.c (D219). */
typedef struct sxml_xpath_step_test_request {
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
    /* The step's test field (0 for none: node()). */
    sxml_ast_node_index test;
    sxml_axis axis;
} sxml_xpath_step_test_request;

SXML_NODISCARD sxml_status sxml_xpath_lowerer_resolve_step_test(
    sxml_xpath_lowerer *lowerer, const sxml_xpath_step_test_request *request,
    sxml_type_identifier *node_test);

/* A value coerced to a type (XPath 4.0, section 3.4), bound in block; the
   value itself when the type accepts everything or a constant already
   matches. */
typedef struct sxml_xpath_lowering_coercion_request {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom value;
    sxml_sequence_type sequence_type;
    sxml_ir_coercion_role role;
} sxml_xpath_lowering_coercion_request;

SXML_NODISCARD sxml_status sxml_xpath_lowerer_coerce_atom(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_coercion_request request,
    sxml_ir_atom *atom);

/* Registers function(parameter_types) as result_type, the signature of
   an IR function. */
typedef struct sxml_xpath_lowering_signature_request {
    sxml_sequence_type_view parameter_types;
    sxml_sequence_type result_type;
} sxml_xpath_lowering_signature_request;

SXML_NODISCARD sxml_status sxml_xpath_lowerer_register_signature(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_signature_request request,
    sxml_type_identifier *signature);

/* ---------------------------------------------------------- Functions */

/* An IR function being lowered, in memory of the caller that enters it
   (a frame's locals or the C stack of an entry point): its state, its
   focus, and the lowerer's function and scope to go back to. */
typedef struct sxml_xpath_lowering_function_frame {
    /* Set by the caller before entering: index, level, captures. */
    sxml_xpath_lowering_function function;
    /* Set by the caller before entering. */
    sxml_xpath_lowering_focus focus;
    sxml_xpath_lowering_function *outer_function;
    size_t outer_scope_entry_count;
} sxml_xpath_lowering_function_frame;

/* Makes frame's function the current one, with its focus; frame must stay
   where it is until leave. */
void sxml_xpath_lowerer_enter_function(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_function_frame *frame);

/* Goes back to the function and scope of before enter; frame->function
   then holds the slot count and frame->focus the slots read. */
void sxml_xpath_lowerer_leave_function(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_function_frame *frame);

/* -------------------------------------------------------- The machine */

/* Scratch arrays a frame owns, released when it is popped. */
#define SXML_XPATH_LOWERING_OWNED_MEMORY_COUNT 4U

/* A body and the focus slots it read (sxml_xpath_lowering_call_focused_body). */
typedef struct sxml_xpath_lowering_focused_result {
    sxml_ir_expression_index body;
    sxml_ir_focus focus;
} sxml_xpath_lowering_focused_result;

/* Where a frame's result goes: an expression index, or the result bound
   in atom_block and its atom (atom not NULL), or (a focused body) a body
   and its focus. */
typedef struct sxml_xpath_lowering_destination {
    sxml_ir_expression_index *expression;
    sxml_ir_atom *atom;
    sxml_xpath_lowering_block *atom_block;
    sxml_xpath_lowering_focused_result *focused;
} sxml_xpath_lowering_destination;

typedef struct sxml_xpath_lowering_frame sxml_xpath_lowering_frame;

/*
 * One running procedure. Frames are allocated one by one and never move,
 * so a part's result is written through a pointer into the frame that
 * asked for it, and blocks and foci of a frame may be pointed to by the
 * frames above it.
 */
struct sxml_xpath_lowering_frame {
    /* The frame that pushed this one; NULL for the first. */
    sxml_xpath_lowering_frame *below;
    const sxml_xpath_lowering_procedure *procedure;
    /* Which step comes next; 0 for the first. The step function keeps
       it. */
    uint32_t step;
    /* Levels of maximum_expression_depth this frame holds. */
    uint32_t depth_units;
    /* Set by sxml_xpath_lowering_finish_frame; the machine then pops the
       frame. */
    bool is_finished;
    /* The AST node it lowers (of the lowerer's AST when the frame
       started); 0 for a procedure without one. */
    sxml_ast_node_index node;
    /* The block the procedure's LETs go to: the caller's, or own_block
       when the frame lowers a body. */
    sxml_xpath_lowering_block *block;
    sxml_xpath_lowering_block own_block;
    bool is_body;
    sxml_xpath_lowering_destination destination;
    /* Released when the frame is popped (sxml_xpath_lowering_allocate_owned). */
    void *owned_memory[SXML_XPATH_LOWERING_OWNED_MEMORY_COUNT];
    /* The procedure's locals (locals_size bytes, aligned for any type),
       right after the frame in its allocation; zeroed, then
       initial_locals. */
    void *locals;
};

typedef struct sxml_xpath_lowering_machine {
    sxml_xpath_lowerer *state;
    sxml_xpath_lowering_frame *top;
} sxml_xpath_lowering_machine;

/* One step of a procedure: returns to the machine, never runs one
   (D71). */
typedef sxml_status (*sxml_xpath_lowering_step)(
    sxml_xpath_lowering_machine *machine, sxml_xpath_lowering_frame *frame);

/* A construct lowered by frames. Static const data. */
struct sxml_xpath_lowering_procedure {
    /* For diagnostics and tests. */
    sxml_byte_view procedure_name;
    sxml_xpath_lowering_step step;
    /* sizeof its locals. */
    size_t locals_size;
};

/* A part to lower: node, for destination, in block or as a body (block
   then NULL). */
typedef struct sxml_xpath_lowering_expression_call {
    sxml_ast_node_index node;
    sxml_xpath_lowering_block *block;
    bool is_body;
    sxml_xpath_lowering_destination destination;
} sxml_xpath_lowering_expression_call;

/* Lowers call.node for call.destination: at once when it needs no frame
   (a literal, a variable, "."), else by pushing its frame, which the
   machine runs after the calling step returns. */
SXML_NODISCARD sxml_status sxml_xpath_lowering_call_expression(
    sxml_xpath_lowering_machine *machine,
    sxml_xpath_lowering_expression_call call);

/* An operand to lower to an atom in block: literals, variables and "." at
   once, other expressions bound in block. */
typedef struct sxml_xpath_lowering_atom_call {
    sxml_ast_node_index node;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
} sxml_xpath_lowering_atom_call;

SXML_NODISCARD sxml_status sxml_xpath_lowering_call_atom(
    sxml_xpath_lowering_machine *machine, sxml_xpath_lowering_atom_call call);

/* Lowers node as a block of its own (a branch, a body). */
SXML_NODISCARD sxml_status sxml_xpath_lowering_call_body(
    sxml_xpath_lowering_machine *machine, sxml_ast_node_index node,
    sxml_ir_expression_index *expression);

/* Lowers node as a body with a new focus of kind SLOTS and writes the
   body and the slots it read to *result. */
typedef struct sxml_xpath_lowering_focused_call {
    sxml_ast_node_index node;
    /* Slots already chosen; others are made on first read. */
    sxml_ir_focus preset_slots;
    sxml_xpath_lowering_focused_result *result;
} sxml_xpath_lowering_focused_call;

SXML_NODISCARD sxml_status sxml_xpath_lowering_call_focused_body(
    sxml_xpath_lowering_machine *machine, sxml_xpath_lowering_focused_call call);

/* A procedure to push as a frame of its own. */
typedef struct sxml_xpath_lowering_procedure_call {
    const sxml_xpath_lowering_procedure *procedure;
    /* The frame's node; may be 0. */
    sxml_ast_node_index node;
    /* Where its LETs go; NULL when is_body. */
    sxml_xpath_lowering_block *block;
    bool is_body;
    sxml_xpath_lowering_destination destination;
    /* Copied to the start of the locals (at most locals_size bytes); may
       be empty. Borrowed for the call. */
    sxml_byte_view initial_locals;
} sxml_xpath_lowering_procedure_call;

/* Pushes call->procedure's frame, holding one level of
   maximum_expression_depth, and writes it to *frame. Returns
   SXML_STATUS_LIMIT_EXCEEDED beyond the depth,
   SXML_STATUS_INVALID_ARGUMENT for a call without a procedure or step,
   with initial locals larger than the locals, or neither a block nor a
   body, and the allocator's failures. */
SXML_NODISCARD sxml_status sxml_xpath_lowering_push_procedure(
    sxml_xpath_lowering_machine *machine,
    const sxml_xpath_lowering_procedure_call *call,
    sxml_xpath_lowering_frame **frame);

/* Ends frame with result, delivered to its destination (after its own
   block's LETs when it lowered a body); the machine pops it. */
SXML_NODISCARD sxml_status sxml_xpath_lowering_finish_frame(
    sxml_xpath_lowering_machine *machine, sxml_xpath_lowering_frame *frame,
    sxml_ir_expression_index result);

/* Allocates a scratch array that frame owns, as owned_memory[memory_index]
   (which must be NULL). */
typedef struct sxml_xpath_lowering_owned_request {
    size_t memory_index;
    sxml_xpath_lowering_scratch_request scratch;
} sxml_xpath_lowering_owned_request;

SXML_NODISCARD sxml_status sxml_xpath_lowering_allocate_owned(
    sxml_xpath_lowering_machine *machine, sxml_xpath_lowering_frame *frame,
    sxml_xpath_lowering_owned_request request);

/* Pushes call->procedure's frame (sxml_xpath_lowering_push_procedure) and
   runs the machine until it is finished: for callers outside a machine,
   never from a step (SXML_STATUS_INVALID_ARGUMENT while a machine of the
   lowerer runs, D161). */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_run_procedure(
    sxml_xpath_lowerer *lowerer, const sxml_xpath_lowering_procedure_call *call);

/* ---------------------------------------------------------- Embedding */

/* One XPath AST to lower inside the caller's scope. */
typedef struct sxml_xpath_lowering_embedding {
    /* Borrowed until the expression is lowered. */
    const sxml_ast *ast;
    /* The expression; 0 for the AST's root (an XPATH node). */
    sxml_ast_node_index node;
    /* The AST's static context (namespaces, function table, base URI);
       NULL keeps the lowerer's current one. Its item heap and allocator
       must be the lowerer's. Its variable declarations are the unit's
       global variables, in order. */
    const sxml_xpath_static_context *static_context;
    /* The focus while the expression is lowered; NULL keeps the current
       function's. A SLOTS focus gets the slots its expression reads. */
    sxml_xpath_lowering_focus *focus;
} sxml_xpath_lowering_embedding;

/* An embedding lowered for destination, in block or as a body. */
typedef struct sxml_xpath_lowering_embedded_call {
    sxml_xpath_lowering_embedding embedding;
    sxml_xpath_lowering_block *block;
    bool is_body;
    sxml_xpath_lowering_destination destination;
} sxml_xpath_lowering_embedded_call;

/* From a step: pushes a frame that lowers the embedding into the current
   function, then gives the lowerer back its AST, static context and
   focus. Returns SXML_STATUS_INVALID_ARGUMENT outside a function, for a
   node beyond the AST, a static context of another heap or allocator or
   an invalid one, or neither a block nor a body, and the allocator's
   failures. */
SXML_NODISCARD sxml_status sxml_xpath_lowering_call_embedded(
    sxml_xpath_lowering_machine *machine,
    const sxml_xpath_lowering_embedded_call *call);

/* Outside a machine: lowers the embedding as a body of the current
   function and writes it to *expression. Fails as call_embedded and as
   lowering does, and with SXML_STATUS_INVALID_ARGUMENT while a machine of
   the lowerer runs (D161). */
SXML_NODISCARD sxml_status sxml_xpath_lowerer_lower_embedded(
    sxml_xpath_lowerer *lowerer, const sxml_xpath_lowering_embedding *embedding,
    sxml_ir_expression_index *expression);

/* ---------------------------------------------------------- Parts */

/* Lowers node at once, binding what it needs in request.block. */
typedef sxml_status (*sxml_xpath_lowering_leaf)(
    sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_node_in_block request,
    sxml_ir_expression_index *expression);

/* How a part lowers one AST kind: exactly one of procedure and leaf. */
typedef struct sxml_xpath_lowering_rule {
    sxml_ast_kind kind;
    /* Pushed as a frame for the node, holding its levels of
       maximum_expression_depth. */
    const sxml_xpath_lowering_procedure *procedure;
    /* Called at once. */
    sxml_xpath_lowering_leaf leaf;
} sxml_xpath_lowering_rule;

/* A lowering part (glossary `lowering_part`). Static const data. */
typedef struct sxml_xpath_lowering_part {
    /* The languages whose lowerers use it. */
    sxml_language_set languages;
    const sxml_xpath_lowering_rule *rules;
    size_t rule_count;
} sxml_xpath_lowering_part;

/* A part of the registry. */
struct sxml_xpath_lowering_registered_part {
    /* The <part> of its definition, for diagnostics and tests. */
    sxml_byte_view part_name;
    const sxml_xpath_lowering_part *part;
    /* Its languages as CMake read them from the definition's .languages;
       a unit test checks that they agree. */
    sxml_language_set languages;
};

/* Every registered lowering part, sorted by part name (static data).
   Defined in the generated lowering_parts.c. Only tests and checks use it:
   it links every part. */
sxml_xpath_lowering_part_view sxml_xpath_lowering_registered_parts(void);

/* The registered lowering parts serving one language, sorted by part name
   (static data; D227). Each is defined in a generated unit of its own,
   lowering_parts_<language>.c, so naming one links only its parts:
   sxml_xpath_lower uses XPath's, XQuery's and XSLT's compilations theirs. */
const sxml_xpath_lowering_part_view *sxml_xpath_lowering_xpath_parts(void);
const sxml_xpath_lowering_part_view *sxml_xpath_lowering_xquery_parts(void);
const sxml_xpath_lowering_part_view *sxml_xpath_lowering_xslt_parts(void);
