/*
 * The XQuery 4.0 lowering part (src/xquery_lower_internal.h; D55, D103,
 * D111, packet 3.7). The module's entry points, sxml_xquery_lower and
 * sxml_xquery_compile, are in src/xquery_prolog.c (packet 3.10).
 *
 * Golden file: src/xpath_lower.c. Every construct that lowers parts of
 * itself is a lowering procedure whose step functions ask the machine for
 * those parts and return (D71: no C recursion); FLWOR clauses are one
 * frame per binding or clause, as the core's bindings are.
 *
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026)
 * unless another specification is named.
 */
#include "xquery_lower_internal.h"

#include "full_text_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"
#include "xpath_lower_internal.h"
#include "xquery_flwor_internal.h"
#include "schema_scope_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ----------------------------------------------------------- AST access */

static const sxml_ast_node *node_of(const sxml_xpath_lowerer *state,
                                    sxml_ast_node_index node) {
    return sxml_ast_get_node(state->ast, node);
}

static sxml_ast_node_index child_of(const sxml_xpath_lowerer *state,
                                    sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(
        state->ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ast_node_index next_of(const sxml_xpath_lowerer *state,
                                   sxml_ast_node_index node) {
    return sxml_ast_find_next_with_same_field(state->ast, node);
}

static sxml_byte_view text_of(const sxml_xpath_lowerer *state,
                              sxml_ast_node_index node) {
    return sxml_ast_get_text(state->ast, node_of(state, node)->payload.text);
}

/* The node after node in a preorder walk of root's subtree; 0 after the
   last. Walks the links, so it needs no stack. */
static sxml_ast_node_index next_in_subtree(const sxml_xpath_lowerer *state,
                                           sxml_ast_node_index root,
                                           sxml_ast_node_index node) {
    const sxml_ast_node *current = node_of(state, node);
    if (current->first_child.value != 0) {
        return current->first_child;
    }
    sxml_ast_node_index walked = node;
    while (walked.value != root.value) {
        const sxml_ast_node *walked_node = node_of(state, walked);
        if (walked_node->next_sibling.value != 0) {
            return walked_node->next_sibling;
        }
        walked = walked_node->parent;
    }
    return (sxml_ast_node_index){.value = 0};
}

static size_t count_children(const sxml_xpath_lowerer *state,
                             sxml_ast_node_index parent, sxml_ast_field field) {
    size_t child_count = 0;
    for (sxml_ast_node_index child = child_of(state, parent, field);
         child.value != 0; child = next_of(state, child)) {
        child_count += 1;
    }
    return child_count;
}

/* An expression of the unit at node. */
typedef struct expression_at {
    sxml_ast_node_index node;
    sxml_ir_expression expression;
} expression_at;

static sxml_status add_new_expression(sxml_xpath_lowerer *state,
                                      expression_at request,
                                      sxml_ir_expression_index *index) {
    return sxml_xpath_lowerer_add_expression(
        state,
        (sxml_xpath_lowering_expression_to_add){.node = request.node,
                                                .expression = request.expression},
        index);
}

typedef struct expression_in_block {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_expression expression;
} expression_in_block;

/* Adds an expression and binds it in block. */
static sxml_status bind_new_expression(sxml_xpath_lowerer *state,
                                       expression_in_block request,
                                       sxml_ir_atom *atom) {
    sxml_ir_expression_index index = {0};
    sxml_status status =
        add_new_expression(state, (expression_at){.node = request.node, .expression = request.expression}, &index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_bind_expression(
        state,
        (sxml_xpath_lowering_expression_in_block){.block = request.block,
                                                  .expression = index},
        atom);
}

/* An expression that is just atom. */
typedef struct atom_at {
    sxml_ast_node_index node;
    sxml_ir_atom atom;
} atom_at;

static sxml_status add_atom_expression(sxml_xpath_lowerer *state, atom_at request,
                                       sxml_ir_expression_index *index) {
    return add_new_expression(
        state,
        (expression_at){.node = request.node,
                        .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                                       .atom = request.atom}},
        index);
}

/* Ends block with an expression that is just atom. */
typedef struct atom_block_result {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom atom;
} atom_block_result;

static sxml_status finish_atom_block(sxml_xpath_lowerer *state,
                                     atom_block_result request,
                                     sxml_ir_expression_index *expression) {
    sxml_ir_expression_index result = {0};
    sxml_status status =
        add_atom_expression(state, (atom_at){.node = request.node, .atom = request.atom}, &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        state,
        (sxml_xpath_lowering_block_result){.block = *request.block,
                                           .result = result},
        expression);
}

static sxml_status add_true_atom(sxml_xpath_lowerer *state, sxml_ir_atom *atom) {
    return sxml_xpath_lowerer_add_item_constant(state, sxml_atomic_item_true(), atom);
}

static sxml_status add_false_atom(sxml_xpath_lowerer *state, sxml_ir_atom *atom) {
    return sxml_xpath_lowerer_add_item_constant(state, sxml_atomic_item_false(), atom);
}

/* A CONDITIONAL on condition. */
typedef struct conditional_request {
    sxml_ast_node_index node;
    sxml_ir_atom condition;
    sxml_ir_expression_index then_branch;
    sxml_ir_expression_index else_branch;
} conditional_request;

static sxml_status add_conditional(sxml_xpath_lowerer *state,
                                   conditional_request request,
                                   sxml_ir_expression_index *expression) {
    return add_new_expression(state, (expression_at){.node = request.node, .expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
            .conditional = {.condition = request.condition,
                            .then_branch = request.then_branch,
                            .else_branch = request.else_branch}}}, expression);
}

/* Puts a variable of the current function in scope. */
static sxml_status push_scope_variable(sxml_xpath_lowerer *state,
                                       sxml_name_identifier variable_name,
                                       sxml_ir_atom atom) {
    return sxml_xpath_lowerer_push_variable(
        state,
        (sxml_xpath_lowering_scope_entry){
            .variable_name = variable_name,
            .function_level = state->function->function_level,
            .atom = atom});
}

/* The sequence type of an optional TYPE_DECLARATION; item()* without. */
static sxml_status resolve_declared_type(sxml_xpath_lowerer *state,
                                         sxml_ast_node_index declaration,
                                         sxml_sequence_type *sequence_type) {
    if (declaration.value == 0) {
        *sequence_type = (sxml_sequence_type){
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
        return SXML_STATUS_OK;
    }
    return sxml_xpath_lowerer_resolve_sequence_type(
        state, child_of(state, declaration, SXML_AST_FIELD_TYPE), sequence_type);
}

/* ------------------------------------------------------ Library calls */

/* The most arguments a library call made by lowering has. */
#define LIBRARY_CALL_MAXIMUM_ARGUMENT_COUNT 3U

/* A call of a library function lowering makes (fn:boolean, fn:data,
   fn:deep-equal, fn:take-while, array:split, map:entries, ...), found by
   local name in its namespace and arity. */
typedef struct library_call {
    sxml_ast_node_index node;
    sxml_well_known_namespace function_namespace;
    sxml_byte_view local_name;
    /* Where argument coercions are bound. */
    sxml_xpath_lowering_block *block;
    sxml_ir_atom_view arguments;
} library_call;

static sxml_ir_function_target target_of(const sxml_function_entry *entry) {
    switch (entry->entry_kind) {
    case SXML_FUNCTION_ENTRY_KIND_NATIVE:
        return (sxml_ir_function_target){
            .target_kind = SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
            .native_function = entry->native_function};
    case SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION:
        return (sxml_ir_function_target){
            .target_kind = SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
            .function = {.value = entry->ir_function_number}};
    case SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR:
    case SXML_FUNCTION_ENTRY_KIND_FUNCTION_ITEM:
    case SXML_FUNCTION_ENTRY_KIND_NONE:
        break;
    }
    return (sxml_ir_function_target){.target_kind =
                                         SXML_IR_FUNCTION_TARGET_KIND_NONE};
}

/* The STATIC_CALL, its arguments coerced to the parameter types (XPath
   4.0, section 3.4) in request->block. */
static sxml_status add_library_call(sxml_xpath_lowerer *state,
                                    const library_call *request,
                                    sxml_ir_expression_index *expression) {
    const size_t argument_count = request->arguments.atom_count;
    if (argument_count > LIBRARY_CALL_MAXIMUM_ARGUMENT_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_name_identifier function_name = {.index = 0};
    (void)sxml_name_pool_find_name(
        state->name_pool,
        &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                 request->function_namespace),
                             .local_name = request->local_name},
        &function_name);
    sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    sxml_status status = SXML_STATUS_OK;
    if (function_name.index != 0) {
        status = sxml_xpath_find_function(
            state->static_context->function_table,
            &(sxml_function_query){.function_name = function_name,
                                   .arity = argument_count},
            &entry);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_function_target target = target_of(&entry);
    if (target.target_kind == SXML_IR_FUNCTION_TARGET_KIND_NONE) {
        return sxml_xpath_lowerer_report_unsupported(
            state, request->node,
            SXML_BYTE_VIEW_FROM_LITERAL(
                "the function table lacks a function the XQuery lowering "
                "calls (such as fn:boolean, fn:data, fn:deep-equal, "
                "fn:take-while or fn:string-join)"));
    }
    sxml_ir_atom atoms[LIBRARY_CALL_MAXIMUM_ARGUMENT_COUNT] = {{0}, {0}, {0}};
    for (size_t argument_index = 0;
         status == SXML_STATUS_OK && argument_index < argument_count;
         argument_index += 1) {
        atoms[argument_index] = request->arguments.atoms[argument_index];
        if (argument_index < entry.parameter_types.sequence_type_count) {
            status = sxml_xpath_lowerer_coerce_atom(
                state,
                (sxml_xpath_lowering_coercion_request){
                    .block = request->block,
                    .node = request->node,
                    .value = request->arguments.atoms[argument_index],
                    .sequence_type =
                        entry.parameter_types.sequence_types[argument_index],
                    .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
                &atoms[argument_index]);
        }
    }
    sxml_ir_atom_list arguments = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            state->unit,
            (sxml_ir_atom_view){.atoms = atoms, .atom_count = argument_count},
            &arguments);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_new_expression(state, (expression_at){.node = request->node, .expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
            .static_call = {.target = target, .arguments = arguments}}}, expression);
}

/* The call, bound in request->block. */
static sxml_status bind_library_call(sxml_xpath_lowerer *state,
                                     const library_call *request,
                                     sxml_ir_atom *atom) {
    sxml_ir_expression_index expression = {0};
    sxml_status status = add_library_call(state, request, &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_bind_expression(
        state,
        (sxml_xpath_lowering_expression_in_block){.block = request->block,
                                                  .expression = expression},
        atom);
}

/* fn:local_name(argument) bound in block. */
typedef struct unary_library_call {
    sxml_ast_node_index node;
    sxml_well_known_namespace function_namespace;
    sxml_byte_view local_name;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom argument;
} unary_library_call;

static sxml_status bind_unary_library_call(sxml_xpath_lowerer *state,
                                           unary_library_call request,
                                           sxml_ir_atom *atom) {
    return bind_library_call(
        state,
        &(library_call){.node = request.node,
                        .function_namespace = request.function_namespace,
                        .local_name = request.local_name,
                        .block = request.block,
                        .arguments = {.atoms = &request.argument, .atom_count = 1}},
        atom);
}

/* ============================================================ FLWOR */

/* One variable of a tuple array's layout (src/xquery_flwor_internal.h). */
typedef sxml_xquery_tuple_member tuple_member;

/* Where a clause segment starts: a clause and, in a for or let clause,
   one of its bindings (in a group by clause one of its grouping
   specifications, then binding 0 for the grouping itself). Clause 0 is
   the return clause. */
typedef struct clause_position {
    sxml_ast_node_index clause;
    sxml_ast_node_index binding;
} clause_position;

static bool clause_has_bindings(const sxml_xpath_lowerer *state,
                                sxml_ast_node_index clause) {
    const sxml_ast_kind kind = node_of(state, clause)->kind;
    return kind == SXML_AST_KIND_FOR_CLAUSE || kind == SXML_AST_KIND_LET_CLAUSE ||
           kind == SXML_AST_KIND_GROUP_BY_CLAUSE;
}

static clause_position clause_start(const sxml_xpath_lowerer *state,
                                    sxml_ast_node_index clause) {
    return (clause_position){
        .clause = clause,
        .binding = !clause_has_bindings(state, clause)
                       ? (sxml_ast_node_index){.value = 0}
                   : node_of(state, clause)->kind == SXML_AST_KIND_GROUP_BY_CLAUSE
                       ? child_of(state, clause, SXML_AST_FIELD_SPECIFICATION)
                       : child_of(state, clause, SXML_AST_FIELD_BINDING)};
}

/* The position after position: the next binding of the clause, else the
   next clause. */
static clause_position next_position(const sxml_xpath_lowerer *state,
                                     clause_position position) {
    if (position.binding.value != 0) {
        const sxml_ast_node_index binding = next_of(state, position.binding);
        if (binding.value != 0 ||
            node_of(state, position.clause)->kind == SXML_AST_KIND_GROUP_BY_CLAUSE) {
            return (clause_position){.clause = position.clause, .binding = binding};
        }
    }
    return clause_start(state, next_of(state, position.clause));
}

/* Clauses that need the tuple stream as a value (glossary
   `tuple_stream_clause`). */
static bool is_tuple_stream_clause(sxml_ast_kind kind) {
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_ORDER_BY_CLAUSE:
    case SXML_AST_KIND_COUNT_CLAUSE:
    case SXML_AST_KIND_WHILE_CLAUSE:
    case SXML_AST_KIND_GROUP_BY_CLAUSE:
        return true;
    default:
        return false;
    }
}

/* Parameters of the while clause's closure: the tuple and its
   position. */
#define WHILE_PARAMETER_COUNT 2U

/* The locals of a FLWOR frame. */
typedef struct flwor_locals {
    /* The scope at the start of the FLWOR: the variables after it are its
       own. */
    size_t scope_mark;
    /* The layout of the last segment's tuple arrays (owned_memory[0]). */
    tuple_member *members;
    size_t member_count;
    /* The tuple stream clause the last segment stopped at; 0 when it
       ended with the return clause. */
    sxml_ast_node_index stop_clause;
    sxml_ir_expression_index segment_result;
    /* The tuple stream clause being lowered and its tuple arrays. */
    sxml_ast_node_index stream_clause;
    sxml_ir_atom tuples;
    /* The MAP that unpacks the tuple arrays before the segment being
       lowered. */
    bool is_unpacking;
    sxml_ir_slot unpacking_item;
    sxml_ir_slot unpacking_position;
    sxml_xpath_lowering_block unpacking_block;
    /* order by: the keys so far (owned_memory[1]). */
    sxml_ir_sort_key *sort_keys;
    size_t sort_key_count;
    sxml_ast_node_index specification;
    sxml_ir_slot sort_item;
    sxml_xpath_lowering_block key_block;
    sxml_ir_expression_index key;
    /* while: the condition closure. */
    sxml_ir_atom closure;
} flwor_locals;

enum {
    SXML_XQUERY_FLWOR_STEP_START = 0,
    SXML_XQUERY_FLWOR_STEP_SEGMENT = 1,
    SXML_XQUERY_FLWOR_STEP_SORT_KEY = 2,
    SXML_XQUERY_FLWOR_STEP_WHILE_CONDITION = 3,
};

/* The locals of a clause frame: one binding or clause of a segment. */
typedef struct clause_locals {
    /* The FLWOR's frame, below this one. */
    sxml_xpath_lowering_frame *flwor_frame;
    clause_position position;
    sxml_ir_atom value;
    sxml_ir_slot item;
    sxml_ir_slot position_slot;
    size_t scope_mark;
    /* A for clause's MAP body or a where clause's then branch. */
    sxml_xpath_lowering_block body_block;
    sxml_ir_expression_index rest;
    /* A trace clause's value. */
    sxml_ir_expression_index traced;
    /* A for binding's score (Full Text 3.0, 2.3.1). */
    sxml_ir_atom score;
} clause_locals;

enum {
    SXML_XQUERY_CLAUSE_STEP_BEGIN = 0,
    SXML_XQUERY_CLAUSE_STEP_FOR_BODY = 1,
    SXML_XQUERY_CLAUSE_STEP_FOR_END = 2,
    SXML_XQUERY_CLAUSE_STEP_LET_BODY = 3,
    SXML_XQUERY_CLAUSE_STEP_LET_END = 4,
    SXML_XQUERY_CLAUSE_STEP_WHERE_BODY = 5,
    SXML_XQUERY_CLAUSE_STEP_WHERE_END = 6,
    SXML_XQUERY_CLAUSE_STEP_TRACE_BODY = 7,
    SXML_XQUERY_CLAUSE_STEP_FINISH = 8,
    SXML_XQUERY_CLAUSE_STEP_FOR_SCORE = 9,
};

static sxml_status continue_flwor(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame);
static sxml_status continue_clause(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame);

static const sxml_xpath_lowering_procedure flwor_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery flwor"),
    .step = continue_flwor,
    .locals_size = sizeof(flwor_locals)};

static const sxml_xpath_lowering_procedure clause_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery clause"),
    .step = continue_clause,
    .locals_size = sizeof(clause_locals)};

/* Pushes the clause frame lowering from position on, in block, whose
   result goes to *rest. */
typedef struct clause_frame_request {
    sxml_xpath_lowering_frame *flwor_frame;
    clause_position position;
    sxml_xpath_lowering_block *block;
    sxml_ir_expression_index *rest;
} clause_frame_request;

static sxml_status push_clause_frame(sxml_xpath_lowering_machine *machine,
                                     clause_frame_request request) {
    const clause_locals initial = {.flwor_frame = request.flwor_frame,
                                   .position = request.position};
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &clause_procedure,
            .node = request.position.clause.value != 0 ? request.position.clause
                                                       : request.flwor_frame->node,
            .block = request.block,
            .is_body = false,
            .destination = {.expression = request.rest},
            .initial_locals = {.bytes = (const uint8_t *)&initial,
                               .byte_count = sizeof(initial)}},
        &frame);
}

/* Pushes the frame of the rest of the segment after the current
   position, in block. */
static sxml_status push_rest(sxml_xpath_lowering_machine *machine,
                             sxml_xpath_lowering_frame *frame,
                             sxml_xpath_lowering_block *block) {
    clause_locals *locals = frame->locals;
    return push_clause_frame(
        machine,
        (clause_frame_request){.flwor_frame = locals->flwor_frame,
                               .position = next_position(machine->state,
                                                         locals->position),
                               .block = block,
                               .rest = &locals->rest});
}

/* ----------------------------------------------------- Tuple arrays */

/* Whether a variable reference or grouping specification from clause on
   (its later sibling clauses and the return clause too) names
   variable_name: a variable a group by does not pack when none does, since
   its grouped value (4.13.8: every tuple's value concatenated) is never
   read. Conservative: a reference to an inner variable of the name counts. */
typedef struct later_reference_query {
    sxml_ast_node_index clause;
    sxml_name_identifier variable_name;
} later_reference_query;

/* What is_referenced_later found. */
typedef struct later_reference {
    bool is_referenced;
} later_reference;

static sxml_status is_referenced_later(sxml_xpath_lowerer *state, later_reference_query query,
                                       later_reference *found) {
    found->is_referenced = false;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index sibling = query.clause;
         status == SXML_STATUS_OK && !found->is_referenced && sibling.value != 0;
         sibling = node_of(state, sibling)->next_sibling) {
        for (sxml_ast_node_index walked = sibling;
             status == SXML_STATUS_OK && !found->is_referenced && walked.value != 0;
             walked = next_in_subtree(state, sibling, walked)) {
            const sxml_ast_kind kind = node_of(state, walked)->kind;
            if (kind != SXML_AST_KIND_VARIABLE_REFERENCE &&
                kind != SXML_AST_KIND_GROUPING_SPECIFICATION) {
                continue;
            }
            sxml_name_identifier name = {0};
            status = sxml_xpath_lowerer_resolve_variable_name(
                state, child_of(state, walked, SXML_AST_FIELD_NAME), &name);
            found->is_referenced =
                status == SXML_STATUS_OK && name.index == query.variable_name.index;
        }
    }
    return status;
}

/*
 * Ends a segment at a tuple stream clause: the tuple array of the live
 * variables of the FLWOR (the innermost of each name), whose layout goes
 * to the FLWOR's frame. A group by leaves out the variables nothing reads
 * after it (is_referenced_later).
 */
static sxml_status finish_with_tuple_array(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    sxml_xpath_lowering_frame *flwor_frame = locals->flwor_frame;
    flwor_locals *flwor = flwor_frame->locals;
    const size_t first_entry = flwor->scope_mark;
    const size_t entry_count = state->scope_entry_count - first_entry;
    /* A group by packs every variable it keeps (src/xquery_flwor_internal.h). */
    const bool packs_every_variable =
        node_of(state, locals->position.clause)->kind == SXML_AST_KIND_GROUP_BY_CLAUSE;
    sxml_xpath_lowerer_release_scratch(state, flwor_frame->owned_memory[0]);
    flwor_frame->owned_memory[0] = NULL;
    flwor->members = NULL;
    flwor->member_count = 0;
    sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, flwor_frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 0,
            .scratch = {.element_count = entry_count == 0 ? 1 : entry_count,
                        .element_size = sizeof(tuple_member),
                        .element_alignment = alignof(tuple_member)}});
    void *atom_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_allocate_scratch(
            state,
            (sxml_xpath_lowering_scratch_request){
                .element_count = entry_count == 0 ? 1 : entry_count,
                .element_size = sizeof(sxml_ir_atom),
                .element_alignment = alignof(sxml_ir_atom)},
            &atom_memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    tuple_member *members = flwor_frame->owned_memory[0];
    sxml_ir_atom *atoms = atom_memory;
    size_t member_count = 0;
    uint32_t packed_count = 0;
    for (size_t entry_index = first_entry;
         entry_index < state->scope_entry_count; entry_index += 1) {
        const sxml_xpath_lowering_scope_entry *entry =
            &state->scope_entries[entry_index];
        bool is_occluded = false;
        for (size_t later_index = entry_index + 1;
             later_index < state->scope_entry_count; later_index += 1) {
            if (state->scope_entries[later_index].variable_name.index ==
                entry->variable_name.index) {
                is_occluded = true;
                break;
            }
        }
        later_reference read = {.is_referenced = true};
        if (!is_occluded && packs_every_variable) {
            status = is_referenced_later(
                state,
                (later_reference_query){.clause = locals->position.clause,
                                        .variable_name = entry->variable_name},
                &read);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (is_occluded || !read.is_referenced) {
            continue;
        }
        tuple_member member = {.variable_name = entry->variable_name,
                               .member_number = 0,
                               .atom = entry->atom};
        if (packs_every_variable || entry->atom.atom_kind == SXML_IR_ATOM_KIND_SLOT) {
            atoms[packed_count] = entry->atom;
            packed_count += 1;
            member.member_number = packed_count;
        }
        members[member_count] = member;
        member_count += 1;
    }
    if (status != SXML_STATUS_OK) {
        sxml_xpath_lowerer_release_scratch(state, atom_memory);
        return status;
    }
    flwor->members = members;
    flwor->member_count = member_count;
    flwor->stop_clause = locals->position.clause;
    sxml_ir_atom_list member_atoms = {0};
    status = sxml_ir_unit_add_atoms(
        state->unit, (sxml_ir_atom_view){.atoms = atoms, .atom_count = packed_count},
        &member_atoms);
    sxml_xpath_lowerer_release_scratch(state, atom_memory);
    sxml_ir_expression_index array = {0};
    if (status == SXML_STATUS_OK) {
        status = add_new_expression(state, (expression_at){.node = locals->position.clause, .expression = (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
                .array_constructor = {.array_form = SXML_IR_ARRAY_FORM_SQUARE,
                                      .members = member_atoms}}}, &array);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, array);
}

sxml_status sxml_xquery_unpack_tuple(sxml_xpath_lowerer *lowerer,
                                     const sxml_xquery_tuple_unpacking *request) {
    sxml_xpath_lowerer *state = lowerer;
    sxml_status status = SXML_STATUS_OK;
    for (size_t member_index = 0;
         status == SXML_STATUS_OK && member_index < request->layout.member_count;
         member_index += 1) {
        const tuple_member *member = &request->layout.members[member_index];
        sxml_ir_atom value = member->atom;
        if (member->member_number != 0) {
            sxml_ir_atom key = {0};
            status = sxml_xpath_lowerer_add_item_constant(
                state, sxml_atomic_item_from_integer(member->member_number), &key);
            if (status == SXML_STATUS_OK) {
                status = bind_new_expression(
                    state,
                    (expression_in_block){
                        .block = request->block,
                        .node = request->node,
                        .expression = {.expression_kind =
                                           SXML_IR_EXPRESSION_KIND_LOOKUP,
                                       .lookup = {.value = request->tuple,
                                                  .key = key}}},
                    &value);
            }
        }
        if (status == SXML_STATUS_OK) {
            status = push_scope_variable(state, member->variable_name, value);
        }
    }
    return status;
}

/* ------------------------------------------------------ for and let */

/*
 * The binding collection of "for member" and "for key/value" (XPath 4.0,
 * section 4.13.1, as the core lowers it, D113): the members of each array
 * as single-member arrays (array:split) or the entries of each map as
 * single-entry maps (map:entries), so the position counts across all.
 */
static sxml_status expand_binding_collection(sxml_xpath_lowerer *state,
                                             sxml_xpath_lowering_frame *frame) {
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index binding = locals->position.binding;
    const bool is_member =
        node_of(state, binding)->kind == SXML_AST_KIND_FOR_MEMBER_BINDING;
    sxml_ir_slot container = {0};
    sxml_status status = sxml_xpath_lowerer_add_slot(state, &container);
    sxml_xpath_lowering_block body_block = {{0}, {0}};
    sxml_ir_atom parts = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_unary_library_call(
            state,
            (unary_library_call){
                .node = binding,
                .function_namespace = is_member ? SXML_NAMESPACE_ARRAY_FUNCTIONS
                                                : SXML_NAMESPACE_MAP_FUNCTIONS,
                .local_name = is_member ? SXML_BYTE_VIEW_FROM_LITERAL("split")
                                        : SXML_BYTE_VIEW_FROM_LITERAL("entries"),
                .block = &body_block,
                .argument = sxml_ir_slot_atom(container)},
            &parts);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_atom_block(
            state,
            (atom_block_result){.block = &body_block, .node = binding, .atom = parts},
            &body);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_new_expression(
        state,
        (expression_in_block){
            .block = frame->block,
            .node = binding,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                           .map = {.input = locals->value,
                                   .focus = {.context_value = container},
                                   .body = body,
                                   .flags = 0}}},
        &locals->value);
}

/* Whether a VARIABLE_NAME_AND_TYPE or a name node declares name. */
typedef struct declared_name_query {
    sxml_ast_node_index name_node;
    sxml_name_identifier name;
} declared_name_query;

typedef struct declared_name {
    bool is_declared;
} declared_name;

static sxml_status declares_name(sxml_xpath_lowerer *state,
                                 declared_name_query query, declared_name *result) {
    sxml_name_identifier declared = {.index = 0};
    sxml_status status =
        sxml_xpath_lowerer_resolve_variable_name(state, query.name_node, &declared);
    if (status == SXML_STATUS_OK) {
        result->is_declared = declared.index == query.name.index;
    }
    return status;
}

/* The range variables of a for binding, in the body block: the item, a
   member (item?1) or an entry's key (map:keys) and value (map:items). */
static sxml_status push_range_variables(sxml_xpath_lowerer *state,
                                        sxml_xpath_lowering_frame *frame) {
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index binding = locals->position.binding;
    const sxml_ir_atom item = sxml_ir_slot_atom(locals->item);
    switch ((uint32_t)node_of(state, binding)->kind) {
    case SXML_AST_KIND_FOR_MEMBER_BINDING: {
        sxml_ir_atom one = {0};
        sxml_ir_atom member = {0};
        sxml_status status = sxml_xpath_lowerer_add_item_constant(
            state, sxml_atomic_item_from_integer(1), &one);
        if (status == SXML_STATUS_OK) {
            status = bind_new_expression(
                state,
                (expression_in_block){
                    .block = &locals->body_block,
                    .node = binding,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LOOKUP,
                                   .lookup = {.value = item, .key = one}}},
                &member);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowerer_push_typed_variable(
            state, (sxml_xpath_lowering_typed_variable){
                       .block = &locals->body_block,
                       .variable = child_of(state, binding, SXML_AST_FIELD_VARIABLE),
                       .value = member});
    }
    case SXML_AST_KIND_FOR_ENTRY_BINDING: {
        const sxml_ast_node_index key =
            child_of(state, child_of(state, binding, SXML_AST_FIELD_KEY),
                     SXML_AST_FIELD_VARIABLE);
        const sxml_ast_node_index value =
            child_of(state, child_of(state, binding, SXML_AST_FIELD_VALUE),
                     SXML_AST_FIELD_VARIABLE);
        sxml_status status = SXML_STATUS_OK;
        if (key.value != 0 && value.value != 0) {
            sxml_name_identifier key_name = {0};
            declared_name same = {.is_declared = false};
            status = sxml_xpath_lowerer_resolve_variable_name(
                state, child_of(state, key, SXML_AST_FIELD_NAME), &key_name);
            if (status == SXML_STATUS_OK) {
                status = declares_name(state, (declared_name_query){.name_node = child_of(state, value, SXML_AST_FIELD_NAME), .name = key_name}, &same);
            }
            if (status == SXML_STATUS_OK && same.is_declared) {
                /* XPath 4.0, section 4.13.1: distinct key and value names. */
                return sxml_xpath_lowerer_report_static_error(
                    state, value,
                    SXML_BYTE_VIEW_FROM_LITERAL(
                        "XQST0089 the key and value variables have the same name"));
            }
        }
        const sxml_ast_node_index variables[2] = {key, value};
        const sxml_byte_view functions[2] = {SXML_BYTE_VIEW_FROM_LITERAL("keys"),
                                             SXML_BYTE_VIEW_FROM_LITERAL("items")};
        for (size_t variable_index = 0; status == SXML_STATUS_OK && variable_index < 2;
             variable_index += 1) {
            if (variables[variable_index].value == 0) {
                continue;
            }
            sxml_ir_atom part = {0};
            status = bind_unary_library_call(
                state,
                (unary_library_call){.node = variables[variable_index],
                                     .function_namespace = SXML_NAMESPACE_MAP_FUNCTIONS,
                                     .local_name = functions[variable_index],
                                     .block = &locals->body_block,
                                     .argument = item},
                &part);
            if (status == SXML_STATUS_OK) {
                status = sxml_xpath_lowerer_push_typed_variable(
                    state,
                    (sxml_xpath_lowering_typed_variable){
                        .block = &locals->body_block,
                        .variable = variables[variable_index],
                        .value = part});
            }
        }
        return status;
    }
    default:
        return sxml_xpath_lowerer_push_typed_variable(
            state, (sxml_xpath_lowering_typed_variable){
                       .block = &locals->body_block,
                       .variable = child_of(state, binding, SXML_AST_FIELD_VARIABLE),
                       .value = item});
    }
}

/* "at $i" (4.13.2): XQST0089 when it is named like a range variable. */
static sxml_status push_position_variable(sxml_xpath_lowerer *state,
                                          sxml_xpath_lowering_frame *frame,
                                          sxml_ast_node_index positional) {
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index binding = locals->position.binding;
    sxml_name_identifier position_name = {0};
    sxml_status status = sxml_xpath_lowerer_resolve_variable_name(
        state, child_of(state, positional, SXML_AST_FIELD_NAME), &position_name);
    const sxml_ast_node_index range_variables[3] = {
        child_of(state, binding, SXML_AST_FIELD_VARIABLE),
        child_of(state, child_of(state, binding, SXML_AST_FIELD_KEY),
                 SXML_AST_FIELD_VARIABLE),
        child_of(state, child_of(state, binding, SXML_AST_FIELD_VALUE),
                 SXML_AST_FIELD_VARIABLE)};
    declared_name same = {.is_declared = false};
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && !same.is_declared && variable_index < 3;
         variable_index += 1) {
        if (range_variables[variable_index].value != 0) {
            status = declares_name(state, (declared_name_query){.name_node = child_of(state, range_variables[variable_index], SXML_AST_FIELD_NAME), .name = position_name}, &same);
        }
    }
    if (status == SXML_STATUS_OK && same.is_declared) {
        return sxml_xpath_lowerer_report_static_error(
            state, positional,
            SXML_BYTE_VIEW_FROM_LITERAL(
                "XQST0089 the position variable has the name of a range variable"));
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return push_scope_variable(state, position_name, sxml_ir_slot_atom(locals->position_slot));
}

/* "score $s" of a for binding (Full Text 3.0, 2.3.1), its score lowered
   (src/xquery_lower_full_text.c; D214). */
static sxml_status push_score_variable(sxml_xpath_lowerer *state, sxml_ast_node_index score,
                                       sxml_ir_atom value) {
    sxml_name_identifier score_name = {0};
    const sxml_status status = sxml_xpath_lowerer_resolve_variable_name(
        state, child_of(state, score, SXML_AST_FIELD_NAME), &score_name);
    return status == SXML_STATUS_OK ? push_scope_variable(state, score_name, value) : status;
}

/* A for binding's variables, its slots made (and its score lowered): the
   range, position and score variables in the MAP's body block, then the
   rest of the segment there. */
static sxml_status bind_for_variables(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index binding = locals->position.binding;
    const sxml_ast_node_index positional = child_of(state, binding, SXML_AST_FIELD_POSITION);
    sxml_status status = push_range_variables(state, frame);
    if (status == SXML_STATUS_OK && positional.value != 0) {
        status = push_position_variable(state, frame, positional);
    }
    const sxml_ast_node_index score = child_of(state, binding, SXML_AST_FIELD_SCORE);
    if (status == SXML_STATUS_OK && score.value != 0) {
        status = push_score_variable(state, score, locals->score);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = SXML_XQUERY_CLAUSE_STEP_FOR_END;
    return push_rest(machine, frame, &locals->body_block);
}

/* A for binding, its sequence lowered: its slots, its score (before its
   variables are in scope: the score lowers a predicate of the sequence
   again), then its variables. */
static sxml_status begin_for_body(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index binding = locals->position.binding;
    const sxml_ast_node_index positional =
        child_of(state, binding, SXML_AST_FIELD_POSITION);
    const sxml_ast_kind kind = node_of(state, binding)->kind;
    sxml_status status = SXML_STATUS_OK;
    if (kind == SXML_AST_KIND_FOR_MEMBER_BINDING ||
        kind == SXML_AST_KIND_FOR_ENTRY_BINDING) {
        status = expand_binding_collection(state, frame);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(state, &locals->item);
    }
    if (status == SXML_STATUS_OK && positional.value != 0) {
        status = sxml_xpath_lowerer_add_slot(state, &locals->position_slot);
    }
    locals->scope_mark = state->scope_entry_count;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (child_of(state, binding, SXML_AST_FIELD_SCORE).value != 0) {
        frame->step = SXML_XQUERY_CLAUSE_STEP_FOR_SCORE;
        return sxml_xquery_call_full_text_for_score(
            machine, &(sxml_xquery_full_text_for_score_request){
                         .binding = binding,
                         .item = sxml_ir_slot_atom(locals->item),
                         .block = &locals->body_block,
                         .score = &locals->score});
    }
    return bind_for_variables(machine, frame);
}

/* The MAP of a for binding (4.13.2; "allowing empty" is
   SXML_IR_MAP_ALLOWING_EMPTY). */
static sxml_status finish_for(sxml_xpath_lowering_machine *machine,
                              sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index binding = locals->position.binding;
    sxml_ir_expression_index body = {0};
    sxml_status status = sxml_xpath_lowerer_finish_block(
        state,
        (sxml_xpath_lowering_block_result){.block = locals->body_block,
                                           .result = locals->rest},
        &body);
    state->scope_entry_count = locals->scope_mark;
    sxml_ir_expression_index map = {0};
    if (status == SXML_STATUS_OK) {
        status = add_new_expression(state, (expression_at){.node = binding, .expression = (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                .map = {.input = locals->value,
                        .focus = {.context_value = locals->item,
                                  .context_position = locals->position_slot},
                        .body = body,
                        .flags = (node_of(state, binding)->flags &
                                  SXML_AST_FLAG_ALLOWING_EMPTY) != 0
                                     ? (uint32_t)SXML_IR_MAP_ALLOWING_EMPTY
                                     : 0U}}}, &map);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, map);
}

/* The value of variable number variable_number (from 1) of a
   destructuring let over the coerced value (XPath 4.0, section 4.13.2, as
   the core lowers it, D113). */
typedef struct destructured_part {
    sxml_xpath_lowering_frame *frame;
    sxml_ast_node_index variable;
    uint32_t variable_number;
    bool is_last;
    /* A map binding's declared record type with named fields; 0 for none. */
    sxml_type_identifier record_type;
} destructured_part;

static sxml_status bind_destructured_part(sxml_xpath_lowerer *state,
                                          destructured_part request,
                                          sxml_ir_atom *atom) {
    sxml_xpath_lowering_frame *frame = request.frame;
    const clause_locals *locals = frame->locals;
    const sxml_ast_kind kind = node_of(state, locals->position.binding)->kind;
    sxml_ir_atom key = {0};
    sxml_status status = SXML_STATUS_OK;
    if (kind == SXML_AST_KIND_LET_MAP_BINDING) {
        const sxml_ast_node *name = node_of(
            state, child_of(state, request.variable, SXML_AST_FIELD_NAME));
        const sxml_byte_view local_name =
            sxml_ast_get_text(state->ast, name->payload.name.local_name);
        size_t field_index = 0;
        if (request.record_type.index != 0 &&
            !sxml_type_registry_find_record_field(
                state->type_registry,
                (sxml_record_field_query){.record_type = request.record_type,
                                          .field_name = local_name},
                &field_index)) {
            /* A record type without the field is a type error. */
            sxml_ir_expression_index raise = {0};
            status = sxml_xpath_lowerer_add_raise(
                state,
                (sxml_xpath_lowering_raise_request){
                    .node = request.variable,
                    .error_status = SXML_STATUS_TYPE_ERROR,
                    .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XPTY0004"),
                    .description = SXML_BYTE_VIEW_FROM_LITERAL(
                        "the record type has no field of the variable's name")},
                &raise);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            return sxml_xpath_lowerer_bind_expression(
                state,
                (sxml_xpath_lowering_expression_in_block){.block = frame->block,
                                                          .expression = raise},
                atom);
        }
        status = sxml_xpath_lowerer_add_string_constant(state, local_name, &key);
    } else {
        status = sxml_xpath_lowerer_add_item_constant(
            state, sxml_atomic_item_from_integer(request.variable_number), &key);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (kind != SXML_AST_KIND_LET_SEQUENCE_BINDING) {
        return bind_new_expression(
            state,
            (expression_in_block){
                .block = frame->block,
                .node = request.variable,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LOOKUP,
                               .lookup = {.value = locals->value, .key = key}}},
            atom);
    }
    const sxml_ir_atom arguments[2] = {locals->value, key};
    return bind_library_call(
        state,
        &(library_call){.node = request.variable,
                        .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                        .local_name = request.is_last
                                          ? SXML_BYTE_VIEW_FROM_LITERAL("subsequence")
                                          : SXML_BYTE_VIEW_FROM_LITERAL("items-at"),
                        .block = frame->block,
                        .arguments = {.atoms = arguments, .atom_count = 2}},
        atom);
}

/* "let $(...)", "let $[...]" and "let ${...}": the value coerced to the
   declared type (item()*, or one array(*) or map(*)), then each variable
   bound to its part in order. */
static sxml_status push_destructured_variables(sxml_xpath_lowerer *state,
                                               sxml_xpath_lowering_frame *frame) {
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index binding = locals->position.binding;
    const sxml_ast_kind kind = node_of(state, binding)->kind;
    const sxml_ast_node_index declaration =
        child_of(state, binding, SXML_AST_FIELD_TYPE);
    sxml_sequence_type declared_type = {
        .item_type = sxml_builtin_type_identifier(
            kind == SXML_AST_KIND_LET_ARRAY_BINDING ? SXML_TYPE_ARRAY
            : kind == SXML_AST_KIND_LET_MAP_BINDING ? SXML_TYPE_MAP
                                                    : SXML_TYPE_ITEM),
        .occurrence = kind == SXML_AST_KIND_LET_SEQUENCE_BINDING
                          ? SXML_OCCURRENCE_ZERO_OR_MORE
                          : SXML_OCCURRENCE_EXACTLY_ONE};
    const sxml_sequence_type single_type = declared_type;
    sxml_status status = SXML_STATUS_OK;
    if (declaration.value != 0) {
        status = resolve_declared_type(state, declaration, &declared_type);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_coerce_atom(
            state,
            (sxml_xpath_lowering_coercion_request){
                .block = frame->block,
                .node = binding,
                .value = locals->value,
                .sequence_type = declared_type,
                .role = SXML_IR_COERCION_ROLE_VARIABLE},
            &locals->value);
    }
    if (status == SXML_STATUS_OK && declaration.value != 0 &&
        kind != SXML_AST_KIND_LET_SEQUENCE_BINDING) {
        status = sxml_xpath_lowerer_coerce_atom(
            state,
            (sxml_xpath_lowering_coercion_request){
                .block = frame->block,
                .node = binding,
                .value = locals->value,
                .sequence_type = single_type,
                .role = SXML_IR_COERCION_ROLE_VARIABLE},
            &locals->value);
    }
    sxml_type_identifier record_type = {.index = 0};
    sxml_type_description description = {0};
    if (status == SXML_STATUS_OK && declaration.value != 0 &&
        kind == SXML_AST_KIND_LET_MAP_BINDING &&
        sxml_type_registry_describe_type(state->type_registry,
                                         declared_type.item_type,
                                         &description) == SXML_STATUS_OK &&
        description.variety == SXML_TYPE_VARIETY_RECORD &&
        !description.variety_details.record_type.is_any_record) {
        record_type = declared_type.item_type;
    }
    uint32_t variable_number = 0;
    for (sxml_ast_node_index variable =
             child_of(state, binding, SXML_AST_FIELD_VARIABLE);
         status == SXML_STATUS_OK && variable.value != 0;
         variable = next_of(state, variable)) {
        if (variable_number == UINT32_MAX - 1U) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        variable_number += 1;
        sxml_ir_atom value = {0};
        status = bind_destructured_part(
            state,
            (destructured_part){.frame = frame,
                                .variable = variable,
                                .variable_number = variable_number,
                                .is_last = next_of(state, variable).value == 0,
                                .record_type = record_type},
            &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_push_typed_variable(
                state, (sxml_xpath_lowering_typed_variable){.block = frame->block,
                                                            .variable = variable,
                                                            .value = value});
        }
    }
    return status;
}

/* ---------------------------------------------------- Clause frames */

/* The return clause (4.13.11) at the end of the last segment. */
static sxml_status begin_return(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    flwor_locals *flwor = locals->flwor_frame->locals;
    flwor->stop_clause = (sxml_ast_node_index){.value = 0};
    frame->step = SXML_XQUERY_CLAUSE_STEP_FINISH;
    return sxml_xpath_lowering_call_expression(
        machine,
        (sxml_xpath_lowering_expression_call){
            .node = child_of(state,
                             child_of(state, locals->flwor_frame->node,
                                      SXML_AST_FIELD_RETURN),
                             SXML_AST_FIELD_VALUE),
            .block = frame->block,
            .is_body = false,
            .destination = {.expression = &locals->rest}});
}

static sxml_status begin_clause(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index clause = locals->position.clause;
    if (clause.value == 0) {
        return begin_return(machine, frame);
    }
    const sxml_ast_kind kind = node_of(state, clause)->kind;
    if (is_tuple_stream_clause(kind) && locals->position.binding.value == 0) {
        if (kind == SXML_AST_KIND_GROUP_BY_CLAUSE) {
            const flwor_locals *flwor = locals->flwor_frame->locals;
            const sxml_status status = sxml_xquery_bind_grouping_keys(
                state, (sxml_xquery_grouping_keys){.clause = clause,
                                                   .block = frame->block,
                                                   .scope_mark = flwor->scope_mark});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        return finish_with_tuple_array(machine, frame);
    }
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_FOR_CLAUSE:
        /* 4.13.2: the binding sequence once, then a MAP over it. */
        frame->step = SXML_XQUERY_CLAUSE_STEP_FOR_BODY;
        return sxml_xpath_lowering_call_atom(
            machine, (sxml_xpath_lowering_atom_call){
                         .node = child_of(state, locals->position.binding,
                                          SXML_AST_FIELD_SEQUENCE),
                         .block = frame->block,
                         .atom = &locals->value});
    case SXML_AST_KIND_LET_CLAUSE:
        /* 4.13.3: the value bound once. */
        frame->step = SXML_XQUERY_CLAUSE_STEP_LET_BODY;
        return sxml_xpath_lowering_call_atom(
            machine, (sxml_xpath_lowering_atom_call){
                         .node = child_of(state, locals->position.binding,
                                          SXML_AST_FIELD_VALUE),
                         .block = frame->block,
                         .atom = &locals->value});
    case SXML_AST_KIND_GROUP_BY_CLAUSE:
        /* 4.13.8: a grouping specification with a value is a let. */
        if (child_of(state, locals->position.binding, SXML_AST_FIELD_VALUE).value == 0) {
            frame->step = SXML_XQUERY_CLAUSE_STEP_FINISH;
            return push_rest(machine, frame, frame->block);
        }
        frame->step = SXML_XQUERY_CLAUSE_STEP_LET_BODY;
        return sxml_xpath_lowering_call_atom(
            machine, (sxml_xpath_lowering_atom_call){
                         .node = child_of(state, locals->position.binding,
                                          SXML_AST_FIELD_VALUE),
                         .block = frame->block,
                         .atom = &locals->value});
    case SXML_AST_KIND_TUMBLING_WINDOW_CLAUSE:
    case SXML_AST_KIND_SLIDING_WINDOW_CLAUSE:
        /* 4.13.4.3: like a for clause, src/xquery_lower_window.c. */
        frame->step = SXML_XQUERY_CLAUSE_STEP_FINISH;
        return sxml_xquery_push_window_clause(
            machine, &(sxml_xquery_segment_rest){.flwor_frame = locals->flwor_frame,
                                                 .clause = clause,
                                                 .block = frame->block,
                                                 .rest = &locals->rest});
    case SXML_AST_KIND_WHERE_CLAUSE:
        /* 4.13.5: the tuples whose condition's effective boolean value is
           true. */
        frame->step = SXML_XQUERY_CLAUSE_STEP_WHERE_BODY;
        return sxml_xpath_lowering_call_atom(
            machine, (sxml_xpath_lowering_atom_call){
                         .node = child_of(state, clause, SXML_AST_FIELD_CONDITION),
                         .block = frame->block,
                         .atom = &locals->value});
    case SXML_AST_KIND_TRACE_CLAUSE:
        /* 4.13.10: no effect on the tuple stream; its output is
           implementation-defined and its errors must not fail the FLWOR,
           so sxml lowers the value (static errors stand) but never
           evaluates it. */
        frame->step = SXML_XQUERY_CLAUSE_STEP_TRACE_BODY;
        return sxml_xpath_lowering_call_body(
            machine, child_of(state, clause, SXML_AST_FIELD_VALUE), &locals->traced);
    default:
        return sxml_xpath_lowerer_report_unsupported(
            state, clause,
            SXML_BYTE_VIEW_FROM_LITERAL("this FLWOR clause is not lowered"));
    }
}

static sxml_status continue_let(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    locals->scope_mark = state->scope_entry_count;
    const sxml_ast_node_index binding = locals->position.binding;
    const sxml_ast_kind kind = node_of(state, binding)->kind;
    if (kind == SXML_AST_KIND_LET_SCORE_BINDING) {
        /* "let score $s := E" (Full Text 3.0, 2.3.2): $s is the score of
           E (src/full_text_internal.h). */
        sxml_name_identifier score_name = {0};
        sxml_status status = sxml_xpath_lowerer_resolve_variable_name(
            state, child_of(state, binding, SXML_AST_FIELD_NAME), &score_name);
        sxml_ir_atom score = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xquery_bind_full_text_score(
                state,
                &(sxml_xquery_full_text_score_request){
                    .block = frame->block, .node = binding, .value = locals->value},
                &score);
        }
        if (status == SXML_STATUS_OK) {
            status = push_scope_variable(state, score_name, score);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->step = SXML_XQUERY_CLAUSE_STEP_LET_END;
        return push_rest(machine, frame, frame->block);
    }
    /* A grouping specification has the name and type a variable has. */
    const sxml_status status =
        kind == SXML_AST_KIND_LET_VALUE_BINDING ||
                kind == SXML_AST_KIND_GROUPING_SPECIFICATION
            ? sxml_xpath_lowerer_push_typed_variable(
                  state,
                  (sxml_xpath_lowering_typed_variable){
                      .block = frame->block,
                      .variable = kind == SXML_AST_KIND_GROUPING_SPECIFICATION
                                      ? binding
                                      : child_of(state, binding, SXML_AST_FIELD_VARIABLE),
                      .value = locals->value})
            : push_destructured_variables(state, frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = SXML_XQUERY_CLAUSE_STEP_LET_END;
    return push_rest(machine, frame, frame->block);
}

static sxml_status finish_where(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index clause = locals->position.clause;
    sxml_ir_expression_index then_branch = {0};
    sxml_status status = sxml_xpath_lowerer_finish_block(
        state,
        (sxml_xpath_lowering_block_result){.block = locals->body_block,
                                           .result = locals->rest},
        &then_branch);
    sxml_ir_expression_index else_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_empty_sequence(state, clause, &else_branch);
    }
    sxml_ir_expression_index conditional = {0};
    if (status == SXML_STATUS_OK) {
        status = add_conditional(state,
                                 (conditional_request){.node = clause,
                                                       .condition = locals->value,
                                                       .then_branch = then_branch,
                                                       .else_branch = else_branch},
                                 &conditional);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, conditional);
}

/* A trace clause's value as the never-taken branch of a CONDITIONAL bound
   in the segment's block. */
static sxml_status continue_trace(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    const sxml_ast_node_index clause = locals->position.clause;
    sxml_ir_atom never = {0};
    sxml_status status = add_false_atom(state, &never);
    sxml_ir_expression_index else_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_empty_sequence(state, clause, &else_branch);
    }
    sxml_ir_atom unused = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_new_expression(
            state,
            (expression_in_block){
                .block = frame->block,
                .node = clause,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                               .conditional = {.condition = never,
                                               .then_branch = locals->traced,
                                               .else_branch = else_branch}}},
            &unused);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = SXML_XQUERY_CLAUSE_STEP_FINISH;
    return push_rest(machine, frame, frame->block);
}

static sxml_status continue_clause(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    clause_locals *locals = frame->locals;
    switch (frame->step) {
    case SXML_XQUERY_CLAUSE_STEP_BEGIN:
        return begin_clause(machine, frame);
    case SXML_XQUERY_CLAUSE_STEP_FOR_BODY:
        return begin_for_body(machine, frame);
    case SXML_XQUERY_CLAUSE_STEP_FOR_SCORE:
        return bind_for_variables(machine, frame);
    case SXML_XQUERY_CLAUSE_STEP_FOR_END:
        return finish_for(machine, frame);
    case SXML_XQUERY_CLAUSE_STEP_LET_BODY:
        return continue_let(machine, frame);
    case SXML_XQUERY_CLAUSE_STEP_LET_END:
        state->scope_entry_count = locals->scope_mark;
        return sxml_xpath_lowering_finish_frame(machine, frame, locals->rest);
    case SXML_XQUERY_CLAUSE_STEP_WHERE_BODY:
        frame->step = SXML_XQUERY_CLAUSE_STEP_WHERE_END;
        return push_rest(machine, frame, &locals->body_block);
    case SXML_XQUERY_CLAUSE_STEP_WHERE_END:
        return finish_where(machine, frame);
    case SXML_XQUERY_CLAUSE_STEP_TRACE_BODY:
        return continue_trace(machine, frame);
    default:
        return sxml_xpath_lowering_finish_frame(machine, frame, locals->rest);
    }
}

/* ----------------------------------------------- Tuple stream clauses */

/* Lowers the segment from position on in block, its result to the
   FLWOR's segment_result. */
static sxml_status push_segment(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame,
                                clause_position position) {
    flwor_locals *locals = frame->locals;
    frame->step = SXML_XQUERY_FLWOR_STEP_SEGMENT;
    return push_clause_frame(
        machine,
        (clause_frame_request){.flwor_frame = frame,
                               .position = position,
                               .block = locals->is_unpacking ? &locals->unpacking_block
                                                             : frame->block,
                               .rest = &locals->segment_result});
}

/* After a tuple stream clause: a MAP over the tuple arrays unpacks them
   (with the position as the count clause's variable) before the next
   segment. */
static sxml_status open_unpacking(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame,
                                  sxml_ast_node_index count_name) {
    sxml_xpath_lowerer *state = machine->state;
    flwor_locals *locals = frame->locals;
    locals->unpacking_position = (sxml_ir_slot){.value = 0};
    locals->unpacking_block = (sxml_xpath_lowering_block){{0}, {0}};
    sxml_status status = sxml_xpath_lowerer_add_slot(state, &locals->unpacking_item);
    if (status == SXML_STATUS_OK && count_name.value != 0) {
        status = sxml_xpath_lowerer_add_slot(state, &locals->unpacking_position);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_unpack_tuple(
            state, &(sxml_xquery_tuple_unpacking){
                       .block = &locals->unpacking_block,
                       .node = locals->stream_clause,
                       .tuple = sxml_ir_slot_atom(locals->unpacking_item),
                       .layout = {.members = locals->members,
                                  .member_count = locals->member_count}});
    }
    if (status == SXML_STATUS_OK && count_name.value != 0) {
        /* 4.13.7: the position of the tuple in the stream, from 1. */
        sxml_name_identifier name = {0};
        status = sxml_xpath_lowerer_resolve_variable_name(state, count_name, &name);
        if (status == SXML_STATUS_OK) {
            status = push_scope_variable(state, name,
                                         sxml_ir_slot_atom(locals->unpacking_position));
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->is_unpacking = true;
    return push_segment(machine, frame,
                        clause_start(state, next_of(state, locals->stream_clause)));
}

/* The collation URI of an order specification as a constant; NONE for the
   codepoint collation. Relative URIs are resolved against the static base
   URI; a collation sxml does not know is XQST0076 (4.13.9). */
static sxml_status resolve_order_collation(sxml_xpath_lowerer *state,
                                           sxml_ast_node_index specification,
                                           sxml_ir_atom *collation) {
    const sxml_ast_node_index literal =
        child_of(state, specification, SXML_AST_FIELD_COLLATION);
    *collation = (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
    if (literal.value == 0) {
        /* The default collation of the static context: the codepoint
           collation until the prolog (3.10) and D116's default reach
           lowering. */
        return SXML_STATUS_OK;
    }
    sxml_uri_resolution resolution = {.uri = {0}, .memory = NULL};
    sxml_status status = sxml_uri_resolve(
        &(sxml_uri_resolution_request){.reference = text_of(state, literal),
                                       .base_uri = state->static_context->static_base_uri,
                                       .allocator = state->allocator},
        &resolution);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    static const sxml_byte_view codepoint = SXML_BYTE_VIEW_INITIALIZER(
        "http://www.w3.org/2005/xpath-functions/collation/codepoint");
    static const sxml_byte_view html_ascii = SXML_BYTE_VIEW_INITIALIZER(
        "http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive");
    static const sxml_byte_view uca =
        SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2013/collation/UCA");
    const sxml_byte_view uri = resolution.uri;
    const bool is_uca =
        uri.byte_count >= uca.byte_count &&
        memcmp(uri.bytes, uca.bytes, uca.byte_count) == 0 &&
        (uri.byte_count == uca.byte_count || uri.bytes[uca.byte_count] == '?');
    if (sxml_byte_view_equals(uri, codepoint)) {
        status = SXML_STATUS_OK;
    } else if (is_uca || sxml_byte_view_equals(uri, html_ascii)) {
        status = sxml_xpath_lowerer_add_string_constant(state, uri, collation);
    } else {
        status = sxml_xpath_lowerer_report_static_error(
            state, literal,
            SXML_BYTE_VIEW_FROM_LITERAL(
                "XQST0076 the collation of the order specification is not known"));
    }
    sxml_allocator_release(state->allocator, resolution.memory);
    return status;
}

/* The next order specification's key, lowered with the members unpacked
   from the SORT's focus value; after the last, the SORT. */
static sxml_status lower_next_sort_key(sxml_xpath_lowering_machine *machine,
                                       sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    flwor_locals *locals = frame->locals;
    if (locals->specification.value == 0) {
        sxml_ir_sort_key_list keys = {0};
        sxml_status status = sxml_ir_unit_add_sort_keys(
            state->unit,
            (sxml_ir_sort_key_view){.sort_keys = locals->sort_keys,
                                    .sort_key_count = locals->sort_key_count},
            &keys);
        if (status == SXML_STATUS_OK) {
            status = bind_new_expression(
                state,
                (expression_in_block){
                    .block = frame->block,
                    .node = locals->stream_clause,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SORT,
                                   .sort = {.input = locals->tuples,
                                            .focus = {.context_value =
                                                          locals->sort_item},
                                            .keys = keys}}},
                &locals->tuples);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return open_unpacking(machine, frame, (sxml_ast_node_index){.value = 0});
    }
    locals->key_block = (sxml_xpath_lowering_block){{0}, {0}};
    sxml_status status = sxml_xquery_unpack_tuple(
        state, &(sxml_xquery_tuple_unpacking){
                   .block = &locals->key_block,
                   .node = locals->specification,
                   .tuple = sxml_ir_slot_atom(locals->sort_item),
                   .layout = {.members = locals->members, .member_count = locals->member_count}});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = SXML_XQUERY_FLWOR_STEP_SORT_KEY;
    return sxml_xpath_lowering_call_expression(
        machine,
        (sxml_xpath_lowering_expression_call){
            .node = child_of(state, locals->specification, SXML_AST_FIELD_KEY),
            .block = &locals->key_block,
            .is_body = false,
            .destination = {.expression = &locals->key}});
}

/* order by (4.13.9): SORT over the tuple arrays, one key per order
   specification. The evaluator's SORT atomizes each key (XPTY0004 for
   more than one item), orders empty keys by the empty order, compares
   with fn:compare semantics and is stable. */
static sxml_status begin_order_by(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    flwor_locals *locals = frame->locals;
    const size_t specification_count = count_children(
        state, locals->stream_clause, SXML_AST_FIELD_SPECIFICATION);
    sxml_xpath_lowerer_release_scratch(state, frame->owned_memory[1]);
    frame->owned_memory[1] = NULL;
    locals->sort_keys = NULL;
    locals->sort_key_count = 0;
    sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 1,
            .scratch = {.element_count =
                            specification_count == 0 ? 1 : specification_count,
                        .element_size = sizeof(sxml_ir_sort_key),
                        .element_alignment = alignof(sxml_ir_sort_key)}});
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(state, &locals->sort_item);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->sort_keys = frame->owned_memory[1];
    locals->specification =
        child_of(state, locals->stream_clause, SXML_AST_FIELD_SPECIFICATION);
    return lower_next_sort_key(machine, frame);
}

/* The empty order of an order spec: as written, else the prolog's
   default order declaration (5.8), else empty least. */
static sxml_ir_empty_order empty_order_of(const sxml_xpath_lowerer *state, uint32_t flags) {
    if ((flags & SXML_AST_FLAG_EMPTY_GREATEST) != 0) {
        return SXML_IR_EMPTY_ORDER_GREATEST;
    }
    if ((flags & SXML_AST_FLAG_EMPTY_LEAST) != 0) {
        return SXML_IR_EMPTY_ORDER_LEAST;
    }
    const sxml_ast_node_index root = state->ast->root;
    const sxml_ast_node_index module = child_of(state, root, SXML_AST_FIELD_MODULE);
    const sxml_ast_node_index prolog = child_of(state, module, SXML_AST_FIELD_PROLOG);
    for (sxml_ast_node_index declaration = child_of(state, prolog, SXML_AST_FIELD_DECLARATION);
         prolog.value != 0 && declaration.value != 0; declaration = next_of(state, declaration)) {
        if (node_of(state, declaration)->kind == SXML_AST_KIND_EMPTY_ORDER_DECLARATION) {
            return node_of(state, declaration)->detail.keyword == SXML_AST_KEYWORD_GREATEST
                       ? SXML_IR_EMPTY_ORDER_GREATEST
                       : SXML_IR_EMPTY_ORDER_LEAST;
        }
    }
    return SXML_IR_EMPTY_ORDER_LEAST;
}

static sxml_status finish_sort_key(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    flwor_locals *locals = frame->locals;
    sxml_ir_expression_index key = {0};
    sxml_status status = sxml_xpath_lowerer_finish_block(
        state,
        (sxml_xpath_lowering_block_result){.block = locals->key_block,
                                           .result = locals->key},
        &key);
    state->scope_entry_count = locals->scope_mark;
    sxml_ir_atom collation = {0};
    if (status == SXML_STATUS_OK) {
        status = resolve_order_collation(state, locals->specification, &collation);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t flags = node_of(state, locals->specification)->flags;
    locals->sort_keys[locals->sort_key_count] = (sxml_ir_sort_key){
        .key = key,
        .direction = (flags & SXML_AST_FLAG_DESCENDING) != 0
                         ? SXML_IR_SORT_DIRECTION_DESCENDING
                         : SXML_IR_SORT_DIRECTION_ASCENDING,
        .empty_order = empty_order_of(state, flags),
        .collation = collation};
    locals->sort_key_count += 1;
    locals->specification = next_of(state, locals->specification);
    return lower_next_sort_key(machine, frame);
}

/* ------------------------------------------------------------ while */

/*
 * while (4.13.6): fn:take-while over the tuple arrays with a condition
 * closure fn($tuple, $position) { boolean(C) } whose body unpacks the
 * tuple (src/xquery_flwor_internal.h), so that no condition after the
 * first false one is evaluated.
 */
static sxml_status begin_while(sxml_xpath_lowering_machine *machine,
                               sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    flwor_locals *locals = frame->locals;
    const sxml_sequence_type parameter_types[WHILE_PARAMETER_COUNT] = {
        {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
         .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
        {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
         .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}};
    frame->step = SXML_XQUERY_FLWOR_STEP_WHILE_CONDITION;
    return sxml_xquery_push_condition_closure(
        machine,
        &(sxml_xquery_condition_closure){
            .clause = locals->stream_clause,
            .condition = child_of(state, locals->stream_clause, SXML_AST_FIELD_CONDITION),
            .parameter_count = WHILE_PARAMETER_COUNT,
            .parameter_names = NULL,
            .parameter_types = parameter_types,
            .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_BOOLEAN),
                            .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
            .tuple_layout = {.members = locals->members, .member_count = locals->member_count},
            .is_boolean = true,
            .block = frame->block,
            .closure = &locals->closure});
}

static sxml_status finish_while(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    flwor_locals *locals = frame->locals;
    const sxml_ir_atom arguments[2] = {locals->tuples, locals->closure};
    const sxml_status status = bind_library_call(
        state,
        &(library_call){.node = locals->stream_clause,
                        .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                        .local_name = SXML_BYTE_VIEW_FROM_LITERAL("take-while"),
                        .block = frame->block,
                        .arguments = {.atoms = arguments, .atom_count = 2}},
        &locals->tuples);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return open_unpacking(machine, frame, (sxml_ast_node_index){.value = 0});
}

/* ------------------------------------------------------ FLWOR frame */

/* A segment has ended: close the unpacking MAP around it, then go on with
   the tuple stream clause it stopped at, or finish. */
static sxml_status finish_segment(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    flwor_locals *locals = frame->locals;
    sxml_ir_expression_index result = locals->segment_result;
    sxml_status status = SXML_STATUS_OK;
    if (locals->is_unpacking) {
        locals->is_unpacking = false;
        state->scope_entry_count = locals->scope_mark;
        sxml_ir_expression_index body = {0};
        status = sxml_xpath_lowerer_finish_block(
            state,
            (sxml_xpath_lowering_block_result){.block = locals->unpacking_block,
                                               .result = result},
            &body);
        if (status == SXML_STATUS_OK) {
            status = add_new_expression(state, (expression_at){.node = locals->stream_clause, .expression = (sxml_ir_expression){
                    .expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                    .map = {.input = locals->tuples,
                            .focus = {.context_value = locals->unpacking_item,
                                      .context_position = locals->unpacking_position},
                            .body = body,
                            .flags = 0}}}, &result);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (locals->stop_clause.value == 0) {
        return sxml_xpath_lowering_finish_frame(machine, frame, result);
    }
    locals->stream_clause = locals->stop_clause;
    status = sxml_xpath_lowerer_bind_expression(
        state,
        (sxml_xpath_lowering_expression_in_block){.block = frame->block,
                                                  .expression = result},
        &locals->tuples);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch ((uint32_t)node_of(state, locals->stream_clause)->kind) {
    case SXML_AST_KIND_COUNT_CLAUSE:
        return open_unpacking(machine, frame,
                              child_of(state, locals->stream_clause, SXML_AST_FIELD_NAME));
    case SXML_AST_KIND_ORDER_BY_CLAUSE:
        return begin_order_by(machine, frame);
    case SXML_AST_KIND_WHILE_CLAUSE:
        return begin_while(machine, frame);
    case SXML_AST_KIND_GROUP_BY_CLAUSE:
        status = sxml_xquery_lower_group_by(
            state,
            &(sxml_xquery_grouping){.clause = locals->stream_clause,
                                    .layout = {.members = locals->members,
                                               .member_count = locals->member_count},
                                    .block = frame->block},
            &locals->tuples);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return open_unpacking(machine, frame, (sxml_ast_node_index){.value = 0});
    default:
        return sxml_xpath_lowerer_report_unsupported(
            state, locals->stream_clause,
            SXML_BYTE_VIEW_FROM_LITERAL("this tuple stream clause is not lowered"));
    }
}

sxml_status sxml_xquery_push_segment_rest(sxml_xpath_lowering_machine *machine,
                                          const sxml_xquery_segment_rest *request) {
    return push_clause_frame(
        machine,
        (clause_frame_request){
            .flwor_frame = request->flwor_frame,
            .position = next_position(machine->state,
                                      clause_start(machine->state, request->clause)),
            .block = request->block,
            .rest = request->rest});
}

/* A FLWOR expression (4.13): segments and tuple stream clauses in turn. */
static sxml_status continue_flwor(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    flwor_locals *locals = frame->locals;
    switch (frame->step) {
    case SXML_XQUERY_FLWOR_STEP_START:
        locals->scope_mark = state->scope_entry_count;
        return push_segment(
            machine, frame,
            clause_start(state, child_of(state, frame->node, SXML_AST_FIELD_CLAUSE)));
    case SXML_XQUERY_FLWOR_STEP_SEGMENT:
        return finish_segment(machine, frame);
    case SXML_XQUERY_FLWOR_STEP_SORT_KEY:
        return finish_sort_key(machine, frame);
    default:
        return finish_while(machine, frame);
    }
}

/* ======================================================= typeswitch */

typedef struct typeswitch_locals {
    sxml_ir_atom operand;
    /* The bodies of the case clauses and, last, the default clause
       (owned_memory[0]). */
    sxml_ir_expression_index *bodies;
    size_t case_count;
    size_t case_index;
    sxml_ast_node_index case_clause;
    size_t scope_mark;
} typeswitch_locals;

/* Puts a case or default clause's variable in scope, bound to the
   operand (4.21.2). */
static sxml_status push_case_variable(sxml_xpath_lowerer *state,
                                      sxml_ast_node_index clause,
                                      sxml_ir_atom operand) {
    const sxml_ast_node_index name_node = child_of(state, clause, SXML_AST_FIELD_VARIABLE);
    if (name_node.value == 0) {
        return SXML_STATUS_OK;
    }
    sxml_name_identifier name = {0};
    sxml_status status = sxml_xpath_lowerer_resolve_variable_name(state, name_node, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return push_scope_variable(state, name, operand);
}

/* The test of a case clause: operand instance of T1, or of T2, ...
   (a CONDITIONAL chain whose else branches test the next type). */
typedef struct type_test_request {
    sxml_ast_node_index clause;
    sxml_ir_atom operand;
} type_test_request;

static sxml_status add_type_tests(sxml_xpath_lowerer *state,
                                  type_test_request request,
                                  sxml_ir_expression_index *test) {
    const sxml_ast_node_index clause = request.clause;
    const sxml_ir_atom operand = request.operand;
    const size_t type_count = count_children(state, clause, SXML_AST_FIELD_TYPE);
    sxml_ir_expression_index rest = {0};
    sxml_status status = SXML_STATUS_OK;
    for (size_t type_number = type_count; status == SXML_STATUS_OK && type_number > 0;
         type_number -= 1) {
        sxml_ast_node_index type_node = child_of(state, clause, SXML_AST_FIELD_TYPE);
        for (size_t skipped = 1; skipped < type_number; skipped += 1) {
            type_node = next_of(state, type_node);
        }
        sxml_sequence_type sequence_type = {0};
        status = sxml_xpath_lowerer_resolve_sequence_type(state, type_node, &sequence_type);
        sxml_ir_expression_index instance_of = {0};
        if (status == SXML_STATUS_OK) {
            status = add_new_expression(state, (expression_at){.node = type_node, .expression = (sxml_ir_expression){
                    .expression_kind = SXML_IR_EXPRESSION_KIND_INSTANCE_OF,
                    .type_test = {.value = operand, .sequence_type = sequence_type}}}, &instance_of);
        }
        if (status != SXML_STATUS_OK || type_number == type_count) {
            rest = instance_of;
            continue;
        }
        sxml_xpath_lowering_block block = {{0}, {0}};
        sxml_ir_atom is_instance = {0};
        status = sxml_xpath_lowerer_bind_expression(
            state,
            (sxml_xpath_lowering_expression_in_block){.block = &block,
                                                      .expression = instance_of},
            &is_instance);
        sxml_ir_atom true_atom = {0};
        if (status == SXML_STATUS_OK) {
            status = add_true_atom(state, &true_atom);
        }
        sxml_ir_expression_index then_branch = {0};
        if (status == SXML_STATUS_OK) {
            status = add_atom_expression(state, (atom_at){.node = type_node, .atom = true_atom}, &then_branch);
        }
        sxml_ir_expression_index conditional = {0};
        if (status == SXML_STATUS_OK) {
            status = add_conditional(state,
                                     (conditional_request){.node = type_node,
                                                           .condition = is_instance,
                                                           .then_branch = then_branch,
                                                           .else_branch = rest},
                                     &conditional);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_finish_block(
                state,
                (sxml_xpath_lowering_block_result){.block = block,
                                                   .result = conditional},
                &rest);
        }
    }
    *test = rest;
    return status;
}

/* A test expression, its body and the rest of the chain as one link:
   [let $m := test] CONDITIONAL($m, body, rest). */
typedef struct chain_link {
    sxml_ast_node_index node;
    sxml_ir_expression_index test;
    sxml_ir_expression_index body;
    sxml_ir_expression_index rest;
} chain_link;

static sxml_status add_chain_link(sxml_xpath_lowerer *state, chain_link request,
                                  sxml_ir_expression_index *link) {
    sxml_xpath_lowering_block block = {{0}, {0}};
    sxml_ir_atom matches = {0};
    sxml_status status = sxml_xpath_lowerer_bind_expression(
        state,
        (sxml_xpath_lowering_expression_in_block){.block = &block,
                                                  .expression = request.test},
        &matches);
    sxml_ir_expression_index conditional = {0};
    if (status == SXML_STATUS_OK) {
        status = add_conditional(state,
                                 (conditional_request){.node = request.node,
                                                       .condition = matches,
                                                       .then_branch = request.body,
                                                       .else_branch = request.rest},
                                 &conditional);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        state,
        (sxml_xpath_lowering_block_result){.block = block, .result = conditional},
        link);
}

/* The CONDITIONAL chain from the last case clause to the first. */
static sxml_status finish_typeswitch(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    typeswitch_locals *locals = frame->locals;
    sxml_ir_expression_index rest = locals->bodies[locals->case_count];
    sxml_status status = SXML_STATUS_OK;
    for (size_t case_number = locals->case_count;
         status == SXML_STATUS_OK && case_number > 0; case_number -= 1) {
        sxml_ast_node_index clause = child_of(state, frame->node, SXML_AST_FIELD_CASE);
        for (size_t skipped = 1; skipped < case_number; skipped += 1) {
            clause = next_of(state, clause);
        }
        sxml_ir_expression_index test = {0};
        status = add_type_tests(state, (type_test_request){.clause = clause, .operand = locals->operand}, &test);
        if (status == SXML_STATUS_OK) {
            status = add_chain_link(state,
                                    (chain_link){.node = clause,
                                                 .test = test,
                                                 .body = locals->bodies[case_number - 1],
                                                 .rest = rest},
                                    &rest);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, rest);
}

/* typeswitch (4.21.2): the operand once, then each case clause's body as a
   branch with its variable bound to the operand. */
static sxml_status continue_typeswitch(sxml_xpath_lowering_machine *machine,
                                       sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    typeswitch_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    if (frame->step == 0) {
        frame->step = 1;
        return sxml_xpath_lowering_call_atom(
            machine,
            (sxml_xpath_lowering_atom_call){
                .node = child_of(state, node, SXML_AST_FIELD_OPERAND),
                .block = frame->block,
                .atom = &locals->operand});
    }
    if (frame->step == 1) {
        locals->case_count = count_children(state, node, SXML_AST_FIELD_CASE);
        const sxml_status status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = locals->case_count + 1,
                            .element_size = sizeof(sxml_ir_expression_index),
                            .element_alignment = alignof(sxml_ir_expression_index)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->bodies = frame->owned_memory[0];
        locals->case_clause = child_of(state, node, SXML_AST_FIELD_CASE);
        locals->scope_mark = state->scope_entry_count;
    } else {
        state->scope_entry_count = locals->scope_mark;
        if (locals->case_index > locals->case_count) {
            return finish_typeswitch(machine, frame);
        }
    }
    frame->step = 2;
    const sxml_ast_node_index clause =
        locals->case_index < locals->case_count
            ? locals->case_clause
            : child_of(state, node, SXML_AST_FIELD_DEFAULT);
    sxml_status status = push_case_variable(state, clause, locals->operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_expression_index *body = &locals->bodies[locals->case_index];
    locals->case_index += 1;
    locals->case_clause = next_of(state, locals->case_clause);
    return sxml_xpath_lowering_call_body(
        machine, child_of(state, clause, SXML_AST_FIELD_VALUE), body);
}

/* =========================================================== switch */

typedef struct switch_locals {
    sxml_ir_atom comparand;
    /* The switch value (4.18) and whether it is empty. */
    sxml_ir_atom switch_value;
    sxml_ir_atom is_empty;
    /* Per case clause its operands, then its return value, in order;
       last the default (owned_memory[0]). */
    sxml_ir_expression_index *parts;
    size_t part_count;
    size_t part_index;
    sxml_ast_node_index case_clause;
    sxml_ast_node_index operand;
} switch_locals;

/* Whether one case operand matches (4.18): its value atomized; empty
   matches an empty switch value; otherwise some item is contextually
   equal to the switch value (fn:deep-equal of two atomic items, with the
   default collation). */
typedef struct operand_test {
    sxml_ast_node_index node;
    sxml_ir_expression_index operand;
    sxml_ir_atom switch_value;
    sxml_ir_atom is_empty;
} operand_test;

static sxml_status add_operand_test(sxml_xpath_lowerer *state, operand_test request,
                                    sxml_ir_expression_index *test) {
    sxml_xpath_lowering_block block = {{0}, {0}};
    sxml_ir_atom value = {0};
    sxml_status status = sxml_xpath_lowerer_bind_expression(
        state,
        (sxml_xpath_lowering_expression_in_block){.block = &block,
                                                  .expression = request.operand},
        &value);
    sxml_ir_atom case_value = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_unary_library_call(
            state,
            (unary_library_call){.node = request.node,
                                 .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("data"),
                                 .block = &block,
                                 .argument = value},
            &case_value);
    }
    sxml_ir_atom exists = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_unary_library_call(
            state,
            (unary_library_call){.node = request.node,
                                 .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("exists"),
                                 .block = &block,
                                 .argument = case_value},
            &exists);
    }
    sxml_ir_slot item = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(state, &item);
    }
    sxml_xpath_lowering_block condition_block = {{0}, {0}};
    sxml_ir_atom is_equal = {0};
    if (status == SXML_STATUS_OK) {
        const sxml_ir_atom arguments[2] = {request.switch_value, sxml_ir_slot_atom(item)};
        status = bind_library_call(
            state,
            &(library_call){.node = request.node,
                            .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("deep-equal"),
                            .block = &condition_block,
                            .arguments = {.atoms = arguments, .atom_count = 2}},
            &is_equal);
    }
    sxml_ir_expression_index condition = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_atom_block(state,
                                   (atom_block_result){.block = &condition_block,
                                                       .node = request.node,
                                                       .atom = is_equal},
                                   &condition);
    }
    sxml_ir_expression_index some = {0};
    if (status == SXML_STATUS_OK) {
        status = add_new_expression(state, (expression_at){.node = request.node, .expression = (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_QUANTIFIED,
                .quantified = {.quantifier = SXML_IR_QUANTIFIER_SOME,
                               .input = case_value,
                               .variable = item,
                               .condition = condition}}}, &some);
    }
    sxml_ir_expression_index empty = {0};
    if (status == SXML_STATUS_OK) {
        status = add_atom_expression(state, (atom_at){.node = request.node, .atom = request.is_empty}, &empty);
    }
    sxml_ir_expression_index conditional = {0};
    if (status == SXML_STATUS_OK) {
        status = add_conditional(state,
                                 (conditional_request){.node = request.node,
                                                       .condition = exists,
                                                       .then_branch = some,
                                                       .else_branch = empty},
                                 &conditional);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        state,
        (sxml_xpath_lowering_block_result){.block = block, .result = conditional},
        test);
}

/* A case clause's test: its first operand matches, or its second, ... */
typedef struct case_test {
    sxml_ast_node_index clause;
    sxml_ir_atom switch_value;
    sxml_ir_atom is_empty;
    /* The clause's operand bodies, operand_count of them. */
    const sxml_ir_expression_index *operands;
    size_t operand_count;
} case_test;

static sxml_status add_case_test(sxml_xpath_lowerer *state, case_test request,
                                 sxml_ir_expression_index *test) {
    sxml_ir_expression_index rest = {0};
    sxml_status status = SXML_STATUS_OK;
    for (size_t operand_number = request.operand_count;
         status == SXML_STATUS_OK && operand_number > 0; operand_number -= 1) {
        sxml_ir_expression_index operand = {0};
        status = add_operand_test(state,
                                  (operand_test){.node = request.clause,
                                                 .operand =
                                                     request.operands[operand_number - 1],
                                                 .switch_value = request.switch_value,
                                                 .is_empty = request.is_empty},
                                  &operand);
        if (status != SXML_STATUS_OK || operand_number == request.operand_count) {
            rest = operand;
            continue;
        }
        sxml_ir_atom true_atom = {0};
        status = add_true_atom(state, &true_atom);
        sxml_ir_expression_index then_branch = {0};
        if (status == SXML_STATUS_OK) {
            status = add_atom_expression(state, (atom_at){.node = request.clause, .atom = true_atom}, &then_branch);
        }
        if (status == SXML_STATUS_OK) {
            status = add_chain_link(state,
                                    (chain_link){.node = request.clause,
                                                 .test = operand,
                                                 .body = then_branch,
                                                 .rest = rest},
                                    &rest);
        }
    }
    *test = rest;
    return status;
}

static sxml_status finish_switch(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    switch_locals *locals = frame->locals;
    sxml_ir_expression_index rest = locals->parts[locals->part_count - 1];
    size_t part_end = locals->part_count - 1;
    sxml_status status = SXML_STATUS_OK;
    const size_t case_count = count_children(state, frame->node, SXML_AST_FIELD_CASE);
    for (size_t case_number = case_count; status == SXML_STATUS_OK && case_number > 0;
         case_number -= 1) {
        sxml_ast_node_index clause = child_of(state, frame->node, SXML_AST_FIELD_CASE);
        for (size_t skipped = 1; skipped < case_number; skipped += 1) {
            clause = next_of(state, clause);
        }
        const size_t operand_count = count_children(state, clause, SXML_AST_FIELD_OPERAND);
        const size_t first_part = part_end - operand_count - 1;
        sxml_ir_expression_index test = {0};
        status = add_case_test(state,
                               (case_test){.clause = clause,
                                           .switch_value = locals->switch_value,
                                           .is_empty = locals->is_empty,
                                           .operands = &locals->parts[first_part],
                                           .operand_count = operand_count},
                               &test);
        if (status == SXML_STATUS_OK) {
            status = add_chain_link(state,
                                    (chain_link){.node = clause,
                                                 .test = test,
                                                 .body = locals->parts[part_end - 1],
                                                 .rest = rest},
                                    &rest);
        }
        part_end = first_part;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, rest);
}

/* The switch value: the comparand atomized, at most one item (XPTY0004),
   or true() without a comparand (4.18). */
static sxml_status bind_switch_value(sxml_xpath_lowerer *state,
                                     sxml_xpath_lowering_frame *frame) {
    switch_locals *locals = frame->locals;
    const sxml_ast_node_index comparand =
        child_of(state, frame->node, SXML_AST_FIELD_COMPARAND);
    if (comparand.value == 0) {
        sxml_status status = add_true_atom(state, &locals->switch_value);
        if (status == SXML_STATUS_OK) {
            status = add_false_atom(state, &locals->is_empty);
        }
        return status;
    }
    sxml_ir_atom atomized = {0};
    sxml_status status = bind_unary_library_call(
        state,
        (unary_library_call){.node = comparand,
                             .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("data"),
                             .block = frame->block,
                             .argument = locals->comparand},
        &atomized);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_coerce_atom(
            state,
            (sxml_xpath_lowering_coercion_request){
                .block = frame->block,
                .node = comparand,
                .value = atomized,
                .sequence_type = {.item_type = sxml_builtin_type_identifier(
                                      SXML_TYPE_XS_ANY_ATOMIC_TYPE),
                                  .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE},
                .role = SXML_IR_COERCION_ROLE_VARIABLE},
            &locals->switch_value);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_unary_library_call(
            state,
            (unary_library_call){.node = comparand,
                                 .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("empty"),
                                 .block = frame->block,
                                 .argument = locals->switch_value},
            &locals->is_empty);
    }
    return status;
}

/* switch (4.18): the switch value once, then every case operand and
   return value and the default as bodies, chained by finish_switch. */
static sxml_status continue_switch(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    switch_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_ast_node_index comparand =
            child_of(state, node, SXML_AST_FIELD_COMPARAND);
        if (comparand.value != 0) {
            return sxml_xpath_lowering_call_atom(
                machine, (sxml_xpath_lowering_atom_call){.node = comparand,
                                                         .block = frame->block,
                                                         .atom = &locals->comparand});
        }
    }
    if (frame->step == 1) {
        sxml_status status = bind_switch_value(state, frame);
        size_t part_count = 1;
        for (sxml_ast_node_index clause = child_of(state, node, SXML_AST_FIELD_CASE);
             clause.value != 0; clause = next_of(state, clause)) {
            part_count += count_children(state, clause, SXML_AST_FIELD_OPERAND) + 1;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowering_allocate_owned(
                machine, frame,
                (sxml_xpath_lowering_owned_request){
                    .memory_index = 0,
                    .scratch = {.element_count = part_count,
                                .element_size = sizeof(sxml_ir_expression_index),
                                .element_alignment =
                                    alignof(sxml_ir_expression_index)}});
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->parts = frame->owned_memory[0];
        locals->part_count = part_count;
        locals->case_clause = child_of(state, node, SXML_AST_FIELD_CASE);
        locals->operand = child_of(state, locals->case_clause, SXML_AST_FIELD_OPERAND);
        frame->step = 2;
    }
    if (locals->part_index == locals->part_count) {
        return finish_switch(machine, frame);
    }
    sxml_ast_node_index part = {0};
    if (locals->case_clause.value == 0) {
        part = child_of(state, node, SXML_AST_FIELD_DEFAULT);
    } else if (locals->operand.value != 0) {
        part = locals->operand;
        locals->operand = next_of(state, locals->operand);
    } else {
        part = child_of(state, locals->case_clause, SXML_AST_FIELD_VALUE);
        locals->case_clause = next_of(state, locals->case_clause);
        locals->operand = child_of(state, locals->case_clause, SXML_AST_FIELD_OPERAND);
    }
    sxml_ir_expression_index *body = &locals->parts[locals->part_index];
    locals->part_index += 1;
    return sxml_xpath_lowering_call_body(machine, part, body);
}

/* ================================================ Other expressions */

/* ordered { E }, unordered { E } (4.15) and extension expressions
   (4.26): the content's value. */
typedef struct content_locals {
    sxml_ir_expression_index result;
} content_locals;

/* Checks the pragmas of an extension expression: their names resolve
   (XPST0081); sxml recognizes no pragma namespace, so every pragma is
   ignored, and one with no expression is XQST0079. */
static sxml_status check_pragmas(sxml_xpath_lowerer *state,
                                 sxml_ast_node_index node) {
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index pragma = child_of(state, node, SXML_AST_FIELD_PRAGMA);
         status == SXML_STATUS_OK && pragma.value != 0;
         pragma = next_of(state, pragma)) {
        sxml_xpath_resolved_name resolved = {0};
        status = sxml_xpath_resolve_name(
            &(sxml_xpath_name_request){
                .ast = state->ast,
                .name_node = child_of(state, pragma, SXML_AST_FIELD_NAME),
                .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                .static_context = state->static_context,
                .error_record = state->error_record},
            &resolved);
    }
    if (status == SXML_STATUS_OK &&
        child_of(state, node, SXML_AST_FIELD_EXPRESSION).value == 0) {
        return sxml_xpath_lowerer_report_static_error(
            state, node,
            SXML_BYTE_VIEW_FROM_LITERAL(
                "XQST0079 an extension expression whose pragmas are all ignored "
                "needs an expression"));
    }
    return status;
}

static sxml_status continue_content(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    content_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    if (frame->step != 0) {
        return sxml_xpath_lowering_finish_frame(machine, frame, locals->result);
    }
    frame->step = 1;
    sxml_ast_node_index content = {0};
    if (node_of(state, node)->kind == SXML_AST_KIND_EXTENSION_EXPRESSION) {
        const sxml_status status = check_pragmas(state, node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        content = child_of(state, node, SXML_AST_FIELD_EXPRESSION);
    } else {
        /* The ordering mode only allows another order of nodes, which
           sxml never uses. */
        content = child_of(state, node, SXML_AST_FIELD_CONTENT);
    }
    return sxml_xpath_lowering_call_expression(
        machine,
        (sxml_xpath_lowering_expression_call){.node = content,
                                              .block = frame->block,
                                              .is_body = false,
                                              .destination = {.expression =
                                                                  &locals->result}});
}

/* ========================================================== validate */

/*
 * validate (4.25; D174): the validate row of src/schema_scope_internal.h
 * over the operand, the mode and, for "validate type T", T as an xs:QName
 * constant; T must be a type of the in-scope schema definitions
 * (XQST0104). The row finds the schema at run time through the static
 * context's in-scope schemas.
 */
typedef struct validate_locals {
    sxml_ir_atom operand;
} validate_locals;

/* The xs:QName constant of "validate type T"'s T, after XQST0104. */
static sxml_status add_validation_type(sxml_xpath_lowerer *state, sxml_ast_node_index type_node,
                                       sxml_ir_atom *atom) {
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = state->ast,
                                   .name_node = type_node,
                                   .name_role = SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE,
                                   .static_context = state->static_context,
                                   .error_record = state->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (resolved.is_any_namespace) {
        resolved.namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML_SCHEMA);
    }
    sxml_name_identifier type_name = {0};
    sxml_type_identifier type = {0};
    status = sxml_name_pool_intern_name(
        state->name_pool,
        &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                             .local_name = resolved.local_name},
        &type_name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!sxml_xpath_find_named_type(state->static_context, type_name, &type)) {
        return sxml_xpath_lowerer_report_static_error(
            state, type_node,
            SXML_BYTE_VIEW_FROM_LITERAL(
                "XQST0104 the type of a validate expression is not an in-scope schema type"));
    }
    sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    status = sxml_atomic_item_make(
        state->item_heap,
        &(sxml_atomic_item_request){
            .type = {.index = SXML_TYPE_XS_QNAME},
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = type_name,
                                                  .namespace_prefix = {.index = 0}}}}},
        &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_item_constant(state, item, atom);
}

static sxml_status continue_validate(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    validate_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return sxml_xpath_lowering_call_atom(
            machine,
            (sxml_xpath_lowering_atom_call){
                .node = child_of(state, frame->node, SXML_AST_FIELD_EXPRESSION),
                .block = frame->block,
                .atom = &locals->operand});
    }
    const sxml_ast_node_index type_node = child_of(state, frame->node, SXML_AST_FIELD_TYPE);
    sxml_schema_validate_mode mode = SXML_SCHEMA_VALIDATE_MODE_STRICT;
    if (type_node.value != 0) {
        mode = SXML_SCHEMA_VALIDATE_MODE_TYPE;
    } else if (node_of(state, frame->node)->detail.keyword == SXML_AST_KEYWORD_LAX) {
        mode = SXML_SCHEMA_VALIDATE_MODE_LAX;
    }
    sxml_ir_atom arguments[3] = {locals->operand, {0}, {0}};
    sxml_status status = sxml_xpath_lowerer_add_item_constant(
        state, sxml_atomic_item_from_integer((int64_t)mode), &arguments[1]);
    if (status == SXML_STATUS_OK && type_node.value != 0) {
        status = add_validation_type(state, type_node, &arguments[2]);
    } else if (status == SXML_STATUS_OK) {
        sxml_ir_expression_index empty = {0};
        status = sxml_xpath_lowerer_add_empty_sequence(state, frame->node, &empty);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_bind_expression(
                state,
                (sxml_xpath_lowering_expression_in_block){.block = frame->block,
                                                          .expression = empty},
                &arguments[2]);
        }
    }
    sxml_ir_atom_list argument_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            state->unit, (sxml_ir_atom_view){.atoms = arguments, .atom_count = 3},
            &argument_list);
    }
    sxml_ir_expression_index call = {0};
    if (status == SXML_STATUS_OK) {
        status = add_new_expression(
            state,
            (expression_at){
                .node = frame->node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                               .static_call = {.target = {.target_kind =
                                                              SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                          .native_function =
                                                              &sxml_schema_validate_function},
                                               .arguments = argument_list}}},
            &call);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, call);
}

/* String constructors (4.9.3): each run of characters a string, each
   interpolation string-join(E, " ") ("`{}`" is ignored), all joined. */
typedef struct string_constructor_locals {
    /* One atom per part (owned_memory[0]). */
    sxml_ir_atom *atoms;
    size_t atom_count;
    sxml_ast_node_index part;
    sxml_ir_atom value;
} string_constructor_locals;

static sxml_status join_string_parts(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    string_constructor_locals *locals = frame->locals;
    sxml_ir_atom_list atoms = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        state->unit,
        (sxml_ir_atom_view){.atoms = locals->atoms, .atom_count = locals->atom_count},
        &atoms);
    sxml_ir_atom parts = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_new_expression(
            state,
            (expression_in_block){
                .block = frame->block,
                .node = frame->node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                               .sequence = atoms}},
            &parts);
    }
    sxml_ir_atom separator = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_string_constant(
            state, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &separator);
    }
    sxml_ir_expression_index joined = {0};
    if (status == SXML_STATUS_OK) {
        const sxml_ir_atom arguments[2] = {parts, separator};
        status = add_library_call(
            state,
            &(library_call){.node = frame->node,
                            .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string-join"),
                            .block = frame->block,
                            .arguments = {.atoms = arguments, .atom_count = 2}},
            &joined);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, joined);
}

static sxml_status continue_string_constructor(sxml_xpath_lowering_machine *machine,
                                               sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    string_constructor_locals *locals = frame->locals;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        const size_t part_count = count_children(state, frame->node, SXML_AST_FIELD_PART);
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = part_count == 0 ? 1 : part_count,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
        locals->atoms = frame->owned_memory[0];
        locals->part = child_of(state, frame->node, SXML_AST_FIELD_PART);
        frame->step = 1;
    } else {
        /* An interpolation's value has been lowered. */
        sxml_ir_atom separator = {0};
        status = sxml_xpath_lowerer_add_string_constant(
            state, SXML_BYTE_VIEW_FROM_LITERAL(" "), &separator);
        if (status == SXML_STATUS_OK) {
            const sxml_ir_atom arguments[2] = {locals->value, separator};
            status = bind_library_call(
                state,
                &(library_call){.node = frame->node,
                                .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string-join"),
                                .block = frame->block,
                                .arguments = {.atoms = arguments, .atom_count = 2}},
                &locals->atoms[locals->atom_count]);
        }
        locals->atom_count += 1;
    }
    while (status == SXML_STATUS_OK && locals->part.value != 0) {
        const sxml_ast_node_index part = locals->part;
        locals->part = next_of(state, part);
        if (node_of(state, part)->kind == SXML_AST_KIND_STRING_CONSTRUCTOR_CHARACTERS) {
            status = sxml_xpath_lowerer_add_string_constant(
                state, text_of(state, part), &locals->atoms[locals->atom_count]);
            locals->atom_count += 1;
            continue;
        }
        const sxml_ast_node_index expression =
            child_of(state, part, SXML_AST_FIELD_EXPRESSION);
        if (expression.value == 0) {
            continue;
        }
        return sxml_xpath_lowering_call_atom(
            machine, (sxml_xpath_lowering_atom_call){.node = expression,
                                                     .block = frame->block,
                                                     .atom = &locals->value});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return join_string_parts(machine, frame);
}

/* ======================================================== try/catch */

/* The variables a catch clause declares (4.20), in the err namespace:
   all but stack-trace and additional are entries of $err:map (D109),
   which is the clause's error details slot; those two are always (). */
#define ERROR_VARIABLE_COUNT 9U

static const sxml_byte_view error_variable_names[ERROR_VARIABLE_COUNT] = {
    SXML_BYTE_VIEW_INITIALIZER("code"),          SXML_BYTE_VIEW_INITIALIZER("description"),
    SXML_BYTE_VIEW_INITIALIZER("value"),         SXML_BYTE_VIEW_INITIALIZER("module"),
    SXML_BYTE_VIEW_INITIALIZER("line-number"),   SXML_BYTE_VIEW_INITIALIZER("column-number"),
    SXML_BYTE_VIEW_INITIALIZER("stack-trace"),   SXML_BYTE_VIEW_INITIALIZER("additional"),
    SXML_BYTE_VIEW_INITIALIZER("map")};

/* Indexes into error_variable_names. */
enum {
    SXML_XQUERY_ERROR_VARIABLE_STACK_TRACE = 6,
    SXML_XQUERY_ERROR_VARIABLE_ADDITIONAL = 7,
    SXML_XQUERY_ERROR_VARIABLE_MAP = 8,
};

typedef struct try_catch_locals {
    sxml_ir_expression_index body;
    /* One per catch clause (owned_memory[0]). */
    sxml_ir_catch_clause *catch_clauses;
    size_t catch_clause_count;
    sxml_ast_node_index catch_clause;
    size_t scope_mark;
    sxml_xpath_lowering_block handler_block;
    sxml_ir_expression_index handler;
    sxml_ir_expression_index finally_body;
} try_catch_locals;

enum {
    SXML_XQUERY_TRY_STEP_START = 0,
    SXML_XQUERY_TRY_STEP_BODY = 1,
    SXML_XQUERY_TRY_STEP_HANDLER = 2,
    SXML_XQUERY_TRY_STEP_FINALLY = 3,
};

/* One name test of a catch clause (4.20: EQName or Wildcard); unprefixed
   names are expanded with the default element namespace, as in XQuery
   3.1. */
static sxml_status resolve_error_test(sxml_xpath_lowerer *state,
                                      sxml_ast_node_index test_node,
                                      sxml_name_test *name_test) {
    const sxml_ast_node *test = node_of(state, test_node);
    if (test->kind == SXML_AST_KIND_WILDCARD &&
        test->detail.name_test_kind == SXML_NAME_TEST_KIND_ANY_NAME) {
        *name_test = (sxml_name_test){.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME};
        return SXML_STATUS_OK;
    }
    if (test->kind == SXML_AST_KIND_WILDCARD &&
        test->detail.name_test_kind == SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD) {
        *name_test = (sxml_name_test){
            .name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD,
            .local_name = sxml_ast_get_text(state->ast, test->payload.name.local_name)};
        return SXML_STATUS_OK;
    }
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = state->ast,
                                   .name_node = test_node,
                                   .name_role = SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE,
                                   .static_context = state->static_context,
                                   .error_record = state->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (test->kind == SXML_AST_KIND_WILDCARD) {
        *name_test = (sxml_name_test){
            .name_test_kind = SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD,
            .namespace_uri = resolved.namespace_uri};
        return SXML_STATUS_OK;
    }
    sxml_name_identifier name = {0};
    status = sxml_name_pool_intern_name(
        state->name_pool,
        &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                             .local_name = resolved.local_name},
        &name);
    if (status == SXML_STATUS_OK) {
        *name_test = (sxml_name_test){.name_test_kind = SXML_NAME_TEST_KIND_EXPANDED_NAME,
                                      .expanded_name = name};
    }
    return status;
}

static sxml_status add_error_tests(sxml_xpath_lowering_machine *machine,
                                   sxml_ast_node_index clause,
                                   sxml_ir_name_test_list *list) {
    sxml_xpath_lowerer *state = machine->state;
    const sxml_ast_node_index errors = child_of(state, clause, SXML_AST_FIELD_ERRORS);
    const size_t test_count = count_children(state, errors, SXML_AST_FIELD_TEST);
    void *memory = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        state,
        (sxml_xpath_lowering_scratch_request){
            .element_count = test_count == 0 ? 1 : test_count,
            .element_size = sizeof(sxml_name_test),
            .element_alignment = alignof(sxml_name_test)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_test *tests = memory;
    size_t test_index = 0;
    for (sxml_ast_node_index test_node = child_of(state, errors, SXML_AST_FIELD_TEST);
         status == SXML_STATUS_OK && test_node.value != 0;
         test_node = next_of(state, test_node)) {
        status = resolve_error_test(state, test_node, &tests[test_index]);
        test_index += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_name_tests(
            state->unit,
            (sxml_ir_name_test_view){.name_tests = tests, .name_test_count = test_count},
            list);
    }
    sxml_xpath_lowerer_release_scratch(state, memory);
    return status;
}

/* Puts the $err: variables the handler references in scope, read from the
   error details slot in the handler's block. */
static sxml_status push_error_variables(sxml_xpath_lowerer *state,
                                        sxml_ast_node_index handler,
                                        try_catch_locals *locals) {
    const sxml_ir_slot details = locals->catch_clauses[locals->catch_clause_count].error_details;
    sxml_name_identifier names[ERROR_VARIABLE_COUNT] = {{0}};
    bool is_referenced[ERROR_VARIABLE_COUNT] = {false};
    sxml_status status = SXML_STATUS_OK;
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && variable_index < ERROR_VARIABLE_COUNT;
         variable_index += 1) {
        status = sxml_name_pool_intern_name(
            state->name_pool,
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_ERRORS),
                .local_name = error_variable_names[variable_index]},
            &names[variable_index]);
    }
    for (sxml_ast_node_index walked = handler;
         status == SXML_STATUS_OK && walked.value != 0;
         walked = next_in_subtree(state, handler, walked)) {
        if (node_of(state, walked)->kind != SXML_AST_KIND_VARIABLE_REFERENCE) {
            continue;
        }
        sxml_name_identifier name = {0};
        status = sxml_xpath_lowerer_resolve_variable_name(
            state, child_of(state, walked, SXML_AST_FIELD_NAME), &name);
        for (size_t variable_index = 0; variable_index < ERROR_VARIABLE_COUNT;
             variable_index += 1) {
            is_referenced[variable_index] = is_referenced[variable_index] ||
                                            names[variable_index].index == name.index;
        }
    }
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && variable_index < ERROR_VARIABLE_COUNT;
         variable_index += 1) {
        if (!is_referenced[variable_index]) {
            continue;
        }
        sxml_ir_atom value = {.atom_kind = SXML_IR_ATOM_KIND_NONE};
        if (variable_index == SXML_XQUERY_ERROR_VARIABLE_MAP) {
            value = sxml_ir_slot_atom(details);
        } else if (variable_index == SXML_XQUERY_ERROR_VARIABLE_STACK_TRACE ||
                   variable_index == SXML_XQUERY_ERROR_VARIABLE_ADDITIONAL) {
            sxml_ir_expression_index empty = {0};
            status = sxml_xpath_lowerer_add_empty_sequence(state, handler, &empty);
            if (status == SXML_STATUS_OK) {
                status = sxml_xpath_lowerer_bind_expression(
                    state,
                    (sxml_xpath_lowering_expression_in_block){
                        .block = &locals->handler_block, .expression = empty},
                    &value);
            }
        } else {
            sxml_ir_atom key = {0};
            status = sxml_xpath_lowerer_add_string_constant(
                state, error_variable_names[variable_index], &key);
            if (status == SXML_STATUS_OK) {
                status = bind_new_expression(
                    state,
                    (expression_in_block){
                        .block = &locals->handler_block,
                        .node = handler,
                        .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LOOKUP,
                                       .lookup = {.value = sxml_ir_slot_atom(details), .key = key}}},
                    &value);
            }
        }
        if (status == SXML_STATUS_OK) {
            status = push_scope_variable(state, names[variable_index], value);
        }
    }
    return status;
}

/* The next catch clause's handler, or the finally clause, or the
   TRY_CATCH. */
static sxml_status lower_next_catch_clause(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    try_catch_locals *locals = frame->locals;
    const sxml_ast_node_index clause = locals->catch_clause;
    if (clause.value == 0) {
        const sxml_ast_node_index finally_clause =
            child_of(state, frame->node, SXML_AST_FIELD_FINALLY);
        frame->step = SXML_XQUERY_TRY_STEP_FINALLY;
        if (finally_clause.value != 0 && locals->finally_body.value == 0) {
            return sxml_xpath_lowering_call_body(
                machine, child_of(state, finally_clause, SXML_AST_FIELD_BODY),
                &locals->finally_body);
        }
        sxml_ir_catch_clause_list clauses = {0};
        sxml_status status = sxml_ir_unit_add_catch_clauses(
            state->unit,
            (sxml_ir_catch_clause_view){.catch_clauses = locals->catch_clauses,
                                        .catch_clause_count = locals->catch_clause_count},
            &clauses);
        sxml_ir_expression_index try_catch = {0};
        if (status == SXML_STATUS_OK) {
            status = add_new_expression(
                state,
                (expression_at){
                    .node = frame->node,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH,
                                   .try_catch = {.body = locals->body,
                                                 .catch_clauses = clauses,
                                                 .finally_body = locals->finally_body}}},
                &try_catch);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowering_finish_frame(machine, frame, try_catch);
    }
    sxml_ir_catch_clause *catch_clause = &locals->catch_clauses[locals->catch_clause_count];
    sxml_status status = add_error_tests(machine, clause, &catch_clause->error_tests);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(state, &catch_clause->error_details);
    }
    const sxml_ast_node_index handler = child_of(state, clause, SXML_AST_FIELD_BODY);
    locals->handler_block = (sxml_xpath_lowering_block){{0}, {0}};
    locals->scope_mark = state->scope_entry_count;
    if (status == SXML_STATUS_OK) {
        status = push_error_variables(state, handler, locals);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = SXML_XQUERY_TRY_STEP_HANDLER;
    return sxml_xpath_lowering_call_expression(
        machine, (sxml_xpath_lowering_expression_call){
                     .node = handler,
                     .block = &locals->handler_block,
                     .is_body = false,
                     .destination = {.expression = &locals->handler}});
}

/* try/catch (4.20): the TRY_CATCH kind (D109); the evaluator catches
   dynamic and type errors of the body and runs finally. */
static sxml_status continue_try_catch(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    try_catch_locals *locals = frame->locals;
    switch (frame->step) {
    case SXML_XQUERY_TRY_STEP_START:
        frame->step = SXML_XQUERY_TRY_STEP_BODY;
        return sxml_xpath_lowering_call_body(
            machine, child_of(state, frame->node, SXML_AST_FIELD_BODY), &locals->body);
    case SXML_XQUERY_TRY_STEP_BODY: {
        const size_t clause_count = count_children(state, frame->node, SXML_AST_FIELD_CATCH);
        const sxml_status status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = clause_count == 0 ? 1 : clause_count,
                            .element_size = sizeof(sxml_ir_catch_clause),
                            .element_alignment = alignof(sxml_ir_catch_clause)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->catch_clauses = frame->owned_memory[0];
        locals->catch_clause = child_of(state, frame->node, SXML_AST_FIELD_CATCH);
        return lower_next_catch_clause(machine, frame);
    }
    case SXML_XQUERY_TRY_STEP_HANDLER: {
        sxml_ir_catch_clause *catch_clause =
            &locals->catch_clauses[locals->catch_clause_count];
        const sxml_status status = sxml_xpath_lowerer_finish_block(
            state,
            (sxml_xpath_lowering_block_result){.block = locals->handler_block,
                                               .result = locals->handler},
            &catch_clause->handler);
        state->scope_entry_count = locals->scope_mark;
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->catch_clause_count += 1;
        locals->catch_clause = next_of(state, locals->catch_clause);
        return lower_next_catch_clause(machine, frame);
    }
    default:
        return lower_next_catch_clause(machine, frame);
    }
}

/* ============================================================= Part */

static const sxml_xpath_lowering_procedure typeswitch_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery typeswitch"),
    .step = continue_typeswitch,
    .locals_size = sizeof(typeswitch_locals)};

static const sxml_xpath_lowering_procedure switch_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery switch"),
    .step = continue_switch,
    .locals_size = sizeof(switch_locals)};

static const sxml_xpath_lowering_procedure content_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery content"),
    .step = continue_content,
    .locals_size = sizeof(content_locals)};

static const sxml_xpath_lowering_procedure string_constructor_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery string constructor"),
    .step = continue_string_constructor,
    .locals_size = sizeof(string_constructor_locals)};

static const sxml_xpath_lowering_procedure validate_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery validate"),
    .step = continue_validate,
    .locals_size = sizeof(validate_locals)};

static const sxml_xpath_lowering_procedure try_catch_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery try catch"),
    .step = continue_try_catch,
    .locals_size = sizeof(try_catch_locals)};

static const sxml_xpath_lowering_rule xquery_rules[] = {
    {.kind = SXML_AST_KIND_TRY_CATCH_EXPRESSION, .procedure = &try_catch_procedure},
    {.kind = SXML_AST_KIND_FLWOR_EXPRESSION, .procedure = &flwor_procedure},
    {.kind = SXML_AST_KIND_TYPESWITCH_EXPRESSION, .procedure = &typeswitch_procedure},
    {.kind = SXML_AST_KIND_SWITCH_EXPRESSION, .procedure = &switch_procedure},
    {.kind = SXML_AST_KIND_ORDERED_EXPRESSION, .procedure = &content_procedure},
    {.kind = SXML_AST_KIND_UNORDERED_EXPRESSION, .procedure = &content_procedure},
    {.kind = SXML_AST_KIND_EXTENSION_EXPRESSION, .procedure = &content_procedure},
    {.kind = SXML_AST_KIND_STRING_CONSTRUCTOR,
     .procedure = &string_constructor_procedure},
    {.kind = SXML_AST_KIND_VALIDATE_EXPRESSION, .procedure = &validate_procedure},
};

const sxml_xpath_lowering_part sxml_xpath_lowering_part_xquery = {
    .languages = SXML_LANGUAGE_XQUERY,
    .rules = xquery_rules,
    .rule_count = sizeof(xquery_rules) / sizeof(xquery_rules[0])};
