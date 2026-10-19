/*
 * The XQuery lowering part `update` (src/xpath_lower_internal.h;
 * docs/DECISIONS.md D103, D187; packet 7.7a): the expressions of the
 * XQuery Update Facility 3.0. Every one lowers to IR the evaluator
 * already has (src/xquery_update_internal.h):
 *
 * - insert, delete, replace and rename (5.4.1 to 5.4.4): their operands
 *   as atoms, then a STATIC_CALL of the internal native row that makes
 *   the update primitives. "replace value of" first makes its value a
 *   TEXT_CONSTRUCTOR (5.4.3.2: the value is computed "as in a text node
 *   constructor");
 * - copy-modify (5.4.5): per copy clause a LET of copy(E), the variable
 *   in scope of the later clauses and of modify; then
 *   modify-copies((copies), M), whose array the return clause reads, each
 *   variable bound to its member by LOOKUP;
 * - an updating function call (5.3): a DYNAMIC_CALL.
 *
 * For `copy $c := $d modify delete node $c/a return $c`:
 *
 *     (let $1 (static-call copy $d)
 *       (let $5 (let $2 (map $1 (focus $3 _ _) (step $3 child element(a)))
 *                 (let $4 (document-order $2)
 *                   (static-call delete $4)))
 *         (let $6 (static-call modify-copies $1 $5)
 *           (let $7 (lookup $6 1) $7))))
 *
 * transform-with (5.4.6) is not lowered yet: no QT4 test uses it.
 *
 * Golden file: src/xquery_lower_construct.c (procedures as frames whose
 * steps ask the machine for their parts, D71).
 *
 * Section numbers are of the XQuery Update Facility 3.0 (W3C
 * Recommendation, 24 January 2017).
 */
#include "ir_internal.h"
#include "sxml/xdm.h"
#include "xpath_ast_internal.h"
#include "xpath_lower_internal.h"
#include "xquery_update_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

/* ----------------------------------------------------------- AST access */

static const sxml_ast_node *node_of(const sxml_xpath_lowerer *state, sxml_ast_node_index node) {
    return sxml_ast_get_node(state->ast, node);
}

static sxml_ast_node_index child_of(const sxml_xpath_lowerer *state, sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(state->ast,
                               (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ast_node_index next_of(const sxml_xpath_lowerer *state, sxml_ast_node_index node) {
    return sxml_ast_find_next_with_same_field(state->ast, node);
}

/* ------------------------------------------------------------ Helpers */

/* An expression of node to bind in block. */
typedef struct expression_in_block {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_expression expression;
} expression_in_block;

static sxml_status bind_new_expression(sxml_xpath_lowerer *state, expression_in_block request,
                                       sxml_ir_atom *atom) {
    sxml_ir_expression_index index = {0};
    const sxml_status status = sxml_xpath_lowerer_add_expression(
        state,
        (sxml_xpath_lowering_expression_to_add){.node = request.node,
                                                .expression = request.expression},
        &index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_bind_expression(
        state,
        (sxml_xpath_lowering_expression_in_block){.block = request.block, .expression = index},
        atom);
}

/* A STATIC_CALL of an internal native row. */
typedef struct native_call {
    sxml_ast_node_index node;
    const sxml_native_function *native_function;
    sxml_ir_atom_view arguments;
} native_call;

static sxml_status make_native_call(sxml_xpath_lowerer *state, native_call request,
                                    sxml_ir_expression *expression) {
    sxml_ir_atom_list arguments = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(state->unit, request.arguments, &arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *expression = (sxml_ir_expression){
        .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
        .static_call = {.target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                   .native_function = request.native_function},
                        .arguments = arguments}};
    return SXML_STATUS_OK;
}

/* A native call to bind in block. */
typedef struct native_call_in_block {
    sxml_xpath_lowering_block *block;
    native_call call;
} native_call_in_block;

static sxml_status bind_native_call(sxml_xpath_lowerer *state, native_call_in_block request,
                                    sxml_ir_atom *atom) {
    sxml_ir_expression expression;
    const sxml_status status = make_native_call(state, request.call, &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_new_expression(state,
                               (expression_in_block){.block = request.block,
                                                     .node = request.call.node,
                                                     .expression = expression},
                               atom);
}

/* Finishes frame with a STATIC_CALL of the row. */
static sxml_status finish_with_native_call(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame,
                                           native_call request) {
    sxml_ir_expression expression;
    sxml_status status = make_native_call(machine->state, request, &expression);
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            machine->state,
            (sxml_xpath_lowering_expression_to_add){.node = frame->node, .expression = expression},
            &result);
    }
    return status == SXML_STATUS_OK ? sxml_xpath_lowering_finish_frame(machine, frame, result)
                                    : status;
}

/* =================================================== Basic updates */

/* The most operands an update expression has. */
#define UPDATE_MAXIMUM_OPERAND_COUNT 3U

/* An update expression's operands, lowered one per step, and the row. */
typedef struct update_locals {
    /* The variant constant first, where the row takes one. */
    sxml_ir_atom arguments[UPDATE_MAXIMUM_OPERAND_COUNT];
    size_t argument_count;
    sxml_ast_node_index operands[UPDATE_MAXIMUM_OPERAND_COUNT];
    size_t operand_count;
    size_t next_operand;
    const sxml_native_function *native_function;
} update_locals;

/* The variant of insert (src/xquery_update_internal.h, insert). */
static int64_t insert_variant(sxml_ast_keyword keyword) {
    switch (keyword) {
    case SXML_AST_KEYWORD_FIRST:
        return 1;
    case SXML_AST_KEYWORD_LAST:
        return 2;
    case SXML_AST_KEYWORD_BEFORE:
        return 3;
    case SXML_AST_KEYWORD_AFTER:
        return 4;
    case SXML_AST_KEYWORD_INTO:
    case SXML_AST_KEYWORD_NONE:
    case SXML_AST_KEYWORD_PRESERVE:
    case SXML_AST_KEYWORD_STRIP:
    case SXML_AST_KEYWORD_ORDERED:
    case SXML_AST_KEYWORD_UNORDERED:
    case SXML_AST_KEYWORD_GREATEST:
    case SXML_AST_KEYWORD_LEAST:
    case SXML_AST_KEYWORD_ELEMENT:
    case SXML_AST_KEYWORD_FUNCTION:
    case SXML_AST_KEYWORD_VALUE:
    case SXML_AST_KEYWORD_ITEM:
    case SXML_AST_KEYWORD_LAX:
    case SXML_AST_KEYWORD_STRICT:
    case SXML_AST_KEYWORD_SKIP:
        break;
    }
    return 0;
}

/* Fills the locals from the node: which row, the variant, the operands in
   the row's order. */
static sxml_status plan_update(sxml_xpath_lowerer *state, sxml_ast_node_index node,
                               update_locals *locals) {
    const sxml_ast_node *update = node_of(state, node);
    int64_t variant = -1;
    switch ((uint32_t)update->kind) {
    case SXML_AST_KIND_INSERT_EXPRESSION:
        locals->native_function = &sxml_xquery_insert_function;
        variant = insert_variant(update->detail.keyword);
        locals->operands[0] = child_of(state, node, SXML_AST_FIELD_SOURCE);
        locals->operands[1] = child_of(state, node, SXML_AST_FIELD_TARGET);
        locals->operand_count = 2;
        break;
    case SXML_AST_KIND_DELETE_EXPRESSION:
        locals->native_function = &sxml_xquery_delete_function;
        locals->operands[0] = child_of(state, node, SXML_AST_FIELD_TARGET);
        locals->operand_count = 1;
        break;
    case SXML_AST_KIND_REPLACE_EXPRESSION:
        locals->native_function = &sxml_xquery_replace_function;
        variant = (update->flags & SXML_AST_FLAG_VALUE_OF) != 0 ? 1 : 0;
        locals->operands[0] = child_of(state, node, SXML_AST_FIELD_TARGET);
        locals->operands[1] = child_of(state, node, SXML_AST_FIELD_VALUE);
        locals->operand_count = 2;
        break;
    case SXML_AST_KIND_RENAME_EXPRESSION:
        locals->native_function = &sxml_xquery_rename_function;
        locals->operands[0] = child_of(state, node, SXML_AST_FIELD_TARGET);
        locals->operands[1] = child_of(state, node, SXML_AST_FIELD_NAME_EXPRESSION);
        locals->operand_count = 2;
        break;
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (variant < 0) {
        return SXML_STATUS_OK;
    }
    locals->argument_count = 1;
    return sxml_xpath_lowerer_add_item_constant(state, sxml_atomic_item_from_integer(variant),
                                                &locals->arguments[0]);
}

/* "replace value of": the new value as a text node (5.4.3.2). */
static sxml_status bind_text_of_value(sxml_xpath_lowerer *state, sxml_xpath_lowering_frame *frame,
                                      sxml_ir_atom *value) {
    sxml_ir_node_construction construction;
    memset(&construction, 0, sizeof construction);
    construction.value = *value;
    return bind_new_expression(
        state,
        (expression_in_block){.block = frame->block,
                              .node = frame->node,
                              .expression = {.expression_kind =
                                                 SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                             .node_construction = construction}},
        value);
}

static sxml_status continue_update(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    update_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_status status = plan_update(state, frame->node, locals);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (locals->next_operand < locals->operand_count) {
        sxml_ir_atom *atom = &locals->arguments[locals->argument_count];
        locals->argument_count += 1;
        locals->next_operand += 1;
        return sxml_xpath_lowering_call_atom(
            machine,
            (sxml_xpath_lowering_atom_call){.node = locals->operands[locals->next_operand - 1],
                                            .block = frame->block,
                                            .atom = atom});
    }
    const sxml_ast_node *update = node_of(state, frame->node);
    if (update->kind == SXML_AST_KIND_REPLACE_EXPRESSION &&
        (update->flags & SXML_AST_FLAG_VALUE_OF) != 0) {
        const sxml_status status =
            bind_text_of_value(state, frame, &locals->arguments[locals->argument_count - 1]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return finish_with_native_call(
        machine, frame,
        (native_call){.node = frame->node,
                      .native_function = locals->native_function,
                      .arguments = {.atoms = locals->arguments,
                                    .atom_count = locals->argument_count}});
}

static const sxml_xpath_lowering_procedure update_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery update expression"),
    .step = continue_update,
    .locals_size = sizeof(update_locals)};

/* ======================================================= copy-modify */

typedef struct copy_modify_locals {
    /* The copies, one atom per copy clause (owned_memory[0]). */
    sxml_ir_atom *copies;
    size_t copy_count;
    size_t copy_index;
    sxml_ast_node_index binding;
    sxml_ir_atom value;
    sxml_ir_atom modify;
    sxml_ir_expression_index result;
    size_t scope_mark;
} copy_modify_locals;

enum {
    SXML_COPY_MODIFY_STEP_BEGIN = 0,
    SXML_COPY_MODIFY_STEP_COPY = 1,
    SXML_COPY_MODIFY_STEP_MODIFY = 2,
    SXML_COPY_MODIFY_STEP_FINISH = 3,
};

static sxml_status push_copy_variable(sxml_xpath_lowerer *state, sxml_ast_node_index binding,
                                      sxml_ir_atom atom) {
    sxml_name_identifier name = {0};
    const sxml_status status = sxml_xpath_lowerer_resolve_variable_name(
        state, child_of(state, binding, SXML_AST_FIELD_NAME), &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_push_variable(
        state, (sxml_xpath_lowering_scope_entry){.variable_name = name,
                                                 .function_level = state->function->function_level,
                                                 .atom = atom});
}

/* The copies made, modify-copies over them and the modify clause's
   updates, the variables rebound to the array's members (5.4.5), then
   the return clause. */
static sxml_status lower_return_clause(sxml_xpath_lowering_machine *machine,
                                       sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    copy_modify_locals *locals = frame->locals;
    sxml_ir_atom copies = {0};
    sxml_ir_atom_list copy_list = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        state->unit, (sxml_ir_atom_view){.atoms = locals->copies, .atom_count = locals->copy_count},
        &copy_list);
    if (status == SXML_STATUS_OK) {
        status = bind_new_expression(
            state, (expression_in_block){.block = frame->block,
                                         .node = frame->node,
                                         .expression = {.expression_kind =
                                                            SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                                        .sequence = copy_list}},
            &copies);
    }
    sxml_ir_atom modified = {0};
    if (status == SXML_STATUS_OK) {
        const sxml_ir_atom arguments[2] = {copies, locals->modify};
        status = bind_native_call(
            state,
            (native_call_in_block){
                .block = frame->block,
                .call = {.node = frame->node,
                         .native_function = &sxml_xquery_modify_copies_function,
                         .arguments = {.atoms = arguments, .atom_count = 2}}},
            &modified);
    }
    sxml_ast_node_index binding = child_of(state, frame->node, SXML_AST_FIELD_BINDING);
    for (size_t member_number = 1; status == SXML_STATUS_OK && binding.value != 0;
         member_number += 1, binding = next_of(state, binding)) {
        sxml_ir_atom key = {0};
        status = sxml_xpath_lowerer_add_item_constant(
            state, sxml_atomic_item_from_integer((int64_t)member_number), &key);
        sxml_ir_atom member = {0};
        if (status == SXML_STATUS_OK) {
            status = bind_new_expression(
                state, (expression_in_block){.block = frame->block,
                                             .node = binding,
                                             .expression = {.expression_kind =
                                                                SXML_IR_EXPRESSION_KIND_LOOKUP,
                                                            .lookup = {.value = modified,
                                                                       .key = key}}},
                &member);
        }
        if (status == SXML_STATUS_OK) {
            status = push_copy_variable(state, binding, member);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = SXML_COPY_MODIFY_STEP_FINISH;
    return sxml_xpath_lowering_call_expression(
        machine, (sxml_xpath_lowering_expression_call){
                     .node = child_of(state, frame->node, SXML_AST_FIELD_RETURN),
                     .block = frame->block,
                     .is_body = false,
                     .destination = {.expression = &locals->result}});
}

static sxml_status continue_copy_modify(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    copy_modify_locals *locals = frame->locals;
    sxml_status status = SXML_STATUS_OK;
    switch (frame->step) {
    case SXML_COPY_MODIFY_STEP_BEGIN:
        locals->scope_mark = state->scope_entry_count;
        for (sxml_ast_node_index binding = child_of(state, frame->node, SXML_AST_FIELD_BINDING);
             binding.value != 0; binding = next_of(state, binding)) {
            locals->copy_count += 1;
        }
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = locals->copy_count,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->copies = frame->owned_memory[0];
        locals->binding = child_of(state, frame->node, SXML_AST_FIELD_BINDING);
        break;
    case SXML_COPY_MODIFY_STEP_COPY: {
        /* The source just lowered: copy it and put its variable in scope
           of the later clauses and of modify. */
        const sxml_ir_atom value = locals->value;
        status = bind_native_call(
            state,
            (native_call_in_block){.block = frame->block,
                                   .call = {.node = locals->binding,
                                            .native_function = &sxml_xquery_copy_function,
                                            .arguments = {.atoms = &value, .atom_count = 1}}},
            &locals->copies[locals->copy_index]);
        if (status == SXML_STATUS_OK) {
            status = push_copy_variable(state, locals->binding, locals->copies[locals->copy_index]);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->copy_index += 1;
        locals->binding = next_of(state, locals->binding);
        break;
    }
    case SXML_COPY_MODIFY_STEP_MODIFY:
        /* The copy variables of the modify clause give way to those of
           the return clause. */
        state->scope_entry_count = locals->scope_mark;
        return lower_return_clause(machine, frame);
    default:
        state->scope_entry_count = locals->scope_mark;
        return sxml_xpath_lowering_finish_frame(machine, frame, locals->result);
    }
    if (locals->binding.value != 0) {
        frame->step = SXML_COPY_MODIFY_STEP_COPY;
        return sxml_xpath_lowering_call_atom(
            machine,
            (sxml_xpath_lowering_atom_call){
                .node = child_of(state, locals->binding, SXML_AST_FIELD_VALUE),
                .block = frame->block,
                .atom = &locals->value});
    }
    frame->step = SXML_COPY_MODIFY_STEP_MODIFY;
    return sxml_xpath_lowering_call_atom(
        machine, (sxml_xpath_lowering_atom_call){
                     .node = child_of(state, frame->node, SXML_AST_FIELD_MODIFY),
                     .block = frame->block,
                     .atom = &locals->modify});
}

static const sxml_xpath_lowering_procedure copy_modify_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery copy-modify"),
    .step = continue_copy_modify,
    .locals_size = sizeof(copy_modify_locals)};

/* ============================================ Updating function calls */

typedef struct updating_call_locals {
    sxml_ir_atom function_item;
    /* One atom per argument (owned_memory[0]). */
    sxml_ir_atom *arguments;
    size_t argument_count;
    size_t argument_index;
    sxml_ast_node_index argument;
} updating_call_locals;

/* "invoke updating F(args)" (5.3): a dynamic call of F. */
static sxml_status continue_updating_call(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    updating_call_locals *locals = frame->locals;
    const sxml_ast_node_index arguments = child_of(state, frame->node, SXML_AST_FIELD_ARGUMENTS);
    if (frame->step == 0) {
        frame->step = 1;
        for (sxml_ast_node_index argument = child_of(state, arguments, SXML_AST_FIELD_ARGUMENT);
             argument.value != 0; argument = next_of(state, argument)) {
            if (node_of(state, argument)->kind == SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
                return sxml_xpath_lowerer_report_static_error(
                    state, argument,
                    SXML_BYTE_VIEW_FROM_LITERAL("XPST0003 an updating function call has an "
                                                "argument placeholder"));
            }
            locals->argument_count += 1;
        }
        const sxml_status status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = locals->argument_count + 1,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->arguments = frame->owned_memory[0];
        locals->argument = child_of(state, arguments, SXML_AST_FIELD_ARGUMENT);
        return sxml_xpath_lowering_call_atom(
            machine, (sxml_xpath_lowering_atom_call){
                         .node = child_of(state, frame->node, SXML_AST_FIELD_FUNCTION),
                         .block = frame->block,
                         .atom = &locals->function_item});
    }
    if (locals->argument.value != 0) {
        sxml_ir_atom *atom = &locals->arguments[locals->argument_index];
        const sxml_ast_node_index argument = locals->argument;
        locals->argument_index += 1;
        locals->argument = next_of(state, argument);
        return sxml_xpath_lowering_call_atom(
            machine,
            (sxml_xpath_lowering_atom_call){.node = argument, .block = frame->block, .atom = atom});
    }
    sxml_ir_atom_list argument_list = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        state->unit,
        (sxml_ir_atom_view){.atoms = locals->arguments, .atom_count = locals->argument_count},
        &argument_list);
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = frame->node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL,
                               .dynamic_call = {.function_item = locals->function_item,
                                                .arguments = argument_list}}},
            &result);
    }
    return status == SXML_STATUS_OK ? sxml_xpath_lowering_finish_frame(machine, frame, result)
                                    : status;
}

static const sxml_xpath_lowering_procedure updating_call_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery updating function call"),
    .step = continue_updating_call,
    .locals_size = sizeof(updating_call_locals)};

/* ====================================================== transform-with */

static sxml_status lower_transform_with(sxml_xpath_lowerer *state,
                                        sxml_xpath_lowering_node_in_block request,
                                        sxml_ir_expression_index *expression) {
    (void)expression;
    return sxml_xpath_lowerer_report_unsupported(
        state, request.node,
        SXML_BYTE_VIEW_FROM_LITERAL("the transform-with expression (XQuery Update Facility "
                                    "3.0, 5.4.6) is not supported yet"));
}

/* ================================================================= Part */

static const sxml_xpath_lowering_rule update_rules[] = {
    {.kind = SXML_AST_KIND_INSERT_EXPRESSION, .procedure = &update_procedure},
    {.kind = SXML_AST_KIND_DELETE_EXPRESSION, .procedure = &update_procedure},
    {.kind = SXML_AST_KIND_REPLACE_EXPRESSION, .procedure = &update_procedure},
    {.kind = SXML_AST_KIND_RENAME_EXPRESSION, .procedure = &update_procedure},
    {.kind = SXML_AST_KIND_COPY_MODIFY_EXPRESSION, .procedure = &copy_modify_procedure},
    {.kind = SXML_AST_KIND_UPDATING_FUNCTION_CALL, .procedure = &updating_call_procedure},
    {.kind = SXML_AST_KIND_TRANSFORM_WITH_EXPRESSION, .leaf = lower_transform_with},
};

const sxml_xpath_lowering_part sxml_xpath_lowering_part_update = {
    .languages = SXML_LANGUAGE_XQUERY,
    .rules = update_rules,
    .rule_count = sizeof(update_rules) / sizeof(update_rules[0])};
