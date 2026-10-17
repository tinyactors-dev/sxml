/*
 * Lowering group by and window clauses (src/xquery_flwor_internal.h; D55,
 * D126; packet 3.8).
 *
 * Golden file: src/xquery_lower.c. A window clause and each of its
 * condition closures is a lowering procedure whose steps ask the machine
 * for their parts and return (D71: no C recursion).
 *
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026).
 */
#include "xquery_flwor_internal.h"

#include "collation_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"

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
                                    sxml_ast_node_index parent, sxml_ast_field field) {
    return sxml_ast_find_child(state->ast,
                               (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ast_node_index next_of(const sxml_xpath_lowerer *state,
                                   sxml_ast_node_index node) {
    return sxml_ast_find_next_with_same_field(state->ast, node);
}

static sxml_byte_view text_of(const sxml_xpath_lowerer *state, sxml_ast_node_index node) {
    return sxml_ast_get_text(state->ast, node_of(state, node)->payload.text);
}

static sxml_ir_source_span span_of(const sxml_xpath_lowerer *state, sxml_ast_node_index node) {
    const sxml_ast_source_span span = node_of(state, node)->source_span;
    return (sxml_ir_source_span){.byte_offset = span.byte_offset,
                                 .byte_count = span.byte_count,
                                 .line_number = span.line_number,
                                 .column_number = span.column_number};
}

/* ----------------------------------------------------------- IR helpers */

static sxml_ir_atom slot_atom(sxml_ir_slot slot) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = slot};
}

typedef struct expression_in_block {
    sxml_xpath_lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_expression expression;
} expression_in_block;

/* Adds an expression and binds it in block. */
static sxml_status bind_new_expression(sxml_xpath_lowerer *state,
                                       expression_in_block request, sxml_ir_atom *atom) {
    sxml_ir_expression_index index = {0};
    sxml_status status = sxml_xpath_lowerer_add_expression(
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

/* An expression that is just atom. */
typedef struct atom_at {
    sxml_ast_node_index node;
    sxml_ir_atom atom;
} atom_at;

static sxml_status add_atom_expression(sxml_xpath_lowerer *state, atom_at request,
                                       sxml_ir_expression_index *index) {
    return sxml_xpath_lowerer_add_expression(
        state,
        (sxml_xpath_lowering_expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = request.atom}},
        index);
}

/* A STATIC_CALL of an internal native row, bound in block. The arguments
   already have the row's types, so none is coerced. */
typedef struct native_call {
    sxml_ast_node_index node;
    const sxml_native_function *native_function;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom_view arguments;
} native_call;

static sxml_status bind_native_call(sxml_xpath_lowerer *state, native_call request,
                                    sxml_ir_atom *atom) {
    sxml_ir_atom_list arguments = {0};
    const sxml_status status =
        sxml_ir_unit_add_atoms(state->unit, request.arguments, &arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_new_expression(
        state,
        (expression_in_block){
            .block = request.block,
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                           .static_call = {.target = {.target_kind =
                                                          SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                      .native_function =
                                                          request.native_function},
                                           .arguments = arguments}}},
        atom);
}

/* The SEQUENCE of atoms, bound in block. */
typedef struct atom_sequence {
    sxml_ast_node_index node;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom_view atoms;
} atom_sequence;

static sxml_status bind_atom_sequence(sxml_xpath_lowerer *state, atom_sequence request,
                                      sxml_ir_atom *atom) {
    sxml_ir_atom_list atoms = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(state->unit, request.atoms, &atoms);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_new_expression(
        state,
        (expression_in_block){.block = request.block,
                              .node = request.node,
                              .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                             .sequence = atoms}},
        atom);
}

/* The name of the variable a name node declares. */
static sxml_status resolve_name(sxml_xpath_lowerer *state, sxml_ast_node_index name_node,
                                sxml_name_identifier *name) {
    return sxml_xpath_lowerer_resolve_variable_name(state, name_node, name);
}

/* ============================================================ group by */

/* The innermost scope entry of name from first_entry on; NULL for none. */
typedef struct scope_query {
    size_t first_entry;
    sxml_name_identifier name;
} scope_query;

static const sxml_xpath_lowering_scope_entry *find_own_variable(const sxml_xpath_lowerer *state,
                                                                scope_query query) {
    for (size_t entry_number = state->scope_entry_count; entry_number > query.first_entry;
         entry_number -= 1) {
        const sxml_xpath_lowering_scope_entry *entry = &state->scope_entries[entry_number - 1];
        if (entry->variable_name.index == query.name.index) {
            return entry;
        }
    }
    return NULL;
}

sxml_status sxml_xquery_bind_grouping_keys(sxml_xpath_lowerer *lowerer,
                                           sxml_xquery_grouping_keys request) {
    sxml_xpath_lowerer *state = lowerer;
    /* 4.13.8: the grouping key is the atomized value of the grouping
       variable; more than one item is XPTY0004. */
    const sxml_sequence_type key_type = {
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index specification =
             child_of(state, request.clause, SXML_AST_FIELD_SPECIFICATION);
         status == SXML_STATUS_OK && specification.value != 0;
         specification = next_of(state, specification)) {
        const sxml_ast_node_index name_node = child_of(state, specification, SXML_AST_FIELD_NAME);
        sxml_name_identifier name = {0};
        status = resolve_name(state, name_node, &name);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const sxml_xpath_lowering_scope_entry *entry = find_own_variable(
            state, (scope_query){.first_entry = request.scope_mark, .name = name});
        if (entry == NULL) {
            return sxml_xpath_lowerer_report_static_error(
                state, name_node,
                SXML_BYTE_VIEW_FROM_LITERAL("XQST0094 the grouping variable is not a "
                                            "variable of the FLWOR expression"));
        }
        sxml_ir_atom key = {0};
        status = sxml_xpath_lowerer_coerce_atom(
            state, (sxml_xpath_lowering_coercion_request){.block = request.block,
                                                          .node = specification,
                                                          .value = entry->atom,
                                                          .sequence_type = key_type,
                                                          .role = SXML_IR_COERCION_ROLE_VARIABLE},
            &key);
        /* "If a type declaration is present and the resulting atomized
           value is not an instance of the specified type, a type error is
           raised [err:XPTY0004]": the key coerced to the type again. */
        const sxml_ast_node_index declaration =
            child_of(state, specification, SXML_AST_FIELD_TYPE);
        if (status == SXML_STATUS_OK && declaration.value != 0) {
            sxml_sequence_type declared_type = {0};
            status = sxml_xpath_lowerer_resolve_sequence_type(
                state, child_of(state, declaration, SXML_AST_FIELD_TYPE), &declared_type);
            if (status == SXML_STATUS_OK) {
                status = sxml_xpath_lowerer_coerce_atom(
                    state,
                    (sxml_xpath_lowering_coercion_request){.block = request.block,
                                                           .node = specification,
                                                           .value = key,
                                                           .sequence_type = declared_type,
                                                           .role = SXML_IR_COERCION_ROLE_VARIABLE},
                    &key);
            }
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_push_variable(
                state,
                (sxml_xpath_lowering_scope_entry){.variable_name = name,
                                                  .function_level = state->function->function_level,
                                                  .atom = key});
        }
    }
    return status;
}

/* The collation of a grouping specification as a string constant: the
   resolved URI, or "" for the default collation. A URI sxml does not know
   is XQST0076 (4.13.8). */
static sxml_status resolve_grouping_collation(sxml_xpath_lowerer *state,
                                              sxml_ast_node_index specification,
                                              sxml_ir_atom *collation) {
    const sxml_ast_node_index literal = child_of(state, specification, SXML_AST_FIELD_COLLATION);
    if (literal.value == 0) {
        return sxml_xpath_lowerer_add_string_constant(state, (sxml_byte_view){0}, collation);
    }
    sxml_uri_resolution resolution = {.uri = {0}, .memory = NULL};
    sxml_status status = sxml_uri_resolve(
        &(sxml_uri_resolution_request){.reference = text_of(state, literal),
                                       .base_uri = state->static_context->static_base_uri,
                                       .allocator = state->allocator},
        &resolution);
    if (status == SXML_STATUS_OUT_OF_MEMORY) {
        return status;
    }
    sxml_collation found = sxml_collation_codepoint();
    if (status == SXML_STATUS_OK && sxml_collation_find(resolution.uri, &found)) {
        status = sxml_xpath_lowerer_add_string_constant(state, resolution.uri, collation);
    } else {
        status = sxml_xpath_lowerer_report_static_error(
            state, literal,
            SXML_BYTE_VIEW_FROM_LITERAL(
                "XQST0076 the collation of the grouping specification is not known"));
    }
    sxml_allocator_release(state->allocator, resolution.memory);
    return status;
}

/* The member of the layout that holds name (the innermost one); 0 for
   none. */
static uint32_t find_member_number(sxml_xquery_tuple_layout layout, sxml_name_identifier name) {
    uint32_t member_number = 0;
    for (size_t member_index = 0; member_index < layout.member_count; member_index += 1) {
        if (layout.members[member_index].variable_name.index == name.index) {
            member_number = layout.members[member_index].member_number;
        }
    }
    return member_number;
}

sxml_status sxml_xquery_lower_group_by(sxml_xpath_lowerer *lowerer,
                                       const sxml_xquery_grouping *request,
                                       sxml_ir_atom *tuples) {
    sxml_xpath_lowerer *state = lowerer;
    size_t specification_count = 0;
    for (sxml_ast_node_index specification =
             child_of(state, request->clause, SXML_AST_FIELD_SPECIFICATION);
         specification.value != 0; specification = next_of(state, specification)) {
        specification_count += 1;
    }
    void *memory = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        state,
        (sxml_xpath_lowering_scratch_request){.element_count = 2 * specification_count,
                                              .element_size = sizeof(sxml_ir_atom),
                                              .element_alignment = alignof(sxml_ir_atom)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_atom *keys = memory;
    sxml_ir_atom *collations = keys + specification_count;
    size_t key_index = 0;
    for (sxml_ast_node_index specification =
             child_of(state, request->clause, SXML_AST_FIELD_SPECIFICATION);
         status == SXML_STATUS_OK && specification.value != 0;
         specification = next_of(state, specification)) {
        sxml_name_identifier name = {0};
        status = resolve_name(state, child_of(state, specification, SXML_AST_FIELD_NAME), &name);
        const uint32_t member_number = find_member_number(request->layout, name);
        if (status == SXML_STATUS_OK && member_number == 0) {
            /* sxml_xquery_bind_grouping_keys made every key a member. */
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_item_constant(
                state, sxml_atomic_item_from_integer(member_number), &keys[key_index]);
        }
        if (status == SXML_STATUS_OK) {
            status = resolve_grouping_collation(state, specification, &collations[key_index]);
        }
        key_index += 1;
    }
    sxml_ir_atom arguments[3] = {*tuples, {0}, {0}};
    if (status == SXML_STATUS_OK) {
        status = bind_atom_sequence(
            state,
            (atom_sequence){.node = request->clause,
                            .block = request->block,
                            .atoms = {.atoms = keys, .atom_count = specification_count}},
            &arguments[1]);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_atom_sequence(
            state,
            (atom_sequence){.node = request->clause,
                            .block = request->block,
                            .atoms = {.atoms = collations, .atom_count = specification_count}},
            &arguments[2]);
    }
    sxml_xpath_lowerer_release_scratch(state, memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_native_call(state,
                            (native_call){.node = request->clause,
                                          .native_function = &sxml_xquery_group_by_function,
                                          .block = request->block,
                                          .arguments = {.atoms = arguments, .atom_count = 3}},
                            tuples);
}

/* =================================================== condition closures */

/* Focus parts a condition may read from the clause's focus. */
#define FOCUS_PART_COUNT 3U

/*
 * A condition closure (src/xquery_flwor_internal.h). The closure captures
 * every variable the condition references that is visible and is neither
 * a parameter nor a member of the tuple layout (more than it needs when an
 * inner binding hides an outer name, which costs a slot) and, when
 * lowering read it, the clause's focus parts, in three slots after the
 * captures bound by LETs at the closure's start (D126).
 */
typedef struct closure_locals {
    sxml_ast_node_index clause;
    sxml_ast_node_index condition;
    uint32_t parameter_count;
    sxml_ast_node_index parameter_names[SXML_XQUERY_CLOSURE_MAXIMUM_PARAMETER_COUNT];
    sxml_sequence_type parameter_types[SXML_XQUERY_CLOSURE_MAXIMUM_PARAMETER_COUNT];
    sxml_sequence_type result_type;
    sxml_xquery_tuple_layout tuple_layout;
    bool is_boolean;
    sxml_ir_function_index closure;
    sxml_xpath_lowering_function_frame function_frame;
    /* The captures' atoms in the clause's function (owned_memory[0]) and
       the captures (owned_memory[1]). */
    sxml_ir_atom *outer_atoms;
    size_t captured_count;
    sxml_xpath_lowering_block closure_block;
    sxml_ir_atom condition_value;
} closure_locals;

enum {
    SXML_XQUERY_CLOSURE_STEP_BEGIN = 0,
    SXML_XQUERY_CLOSURE_STEP_FINISH = 1,
};

/* The atom variable_name has in the current function, or NONE when it is
   not in scope (then lowering the reference reports XPST0008). */
static sxml_ir_atom find_visible_variable(const sxml_xpath_lowerer *state,
                                          sxml_name_identifier variable_name) {
    const sxml_xpath_lowering_function *function = state->function;
    bool is_found = false;
    bool is_outer = false;
    sxml_ir_atom atom = {.atom_kind = SXML_IR_ATOM_KIND_NONE};
    for (size_t entry_number = state->scope_entry_count; entry_number > 0; entry_number -= 1) {
        const sxml_xpath_lowering_scope_entry *entry = &state->scope_entries[entry_number - 1];
        if (entry->variable_name.index == variable_name.index) {
            is_found = true;
            is_outer = entry->function_level != function->function_level;
            atom = entry->atom;
            break;
        }
    }
    if (!is_found) {
        const sxml_variable_declaration_view declarations =
            state->static_context->variable_declarations;
        for (size_t declaration_index = 0;
             declaration_index < declarations.variable_declaration_count;
             declaration_index += 1) {
            if (declarations.variable_declarations[declaration_index].variable_name.index ==
                variable_name.index) {
                is_found = true;
                is_outer = function->function_level != 0;
                atom = (sxml_ir_atom){
                    .atom_kind = SXML_IR_ATOM_KIND_GLOBAL_VARIABLE,
                    .global_variable = {.value = (uint32_t)declaration_index + 1U}};
                break;
            }
        }
    }
    if (is_found && is_outer) {
        for (size_t capture_index = 0; capture_index < function->capture_count;
             capture_index += 1) {
            if (function->captures[capture_index].variable_name.index == variable_name.index) {
                return function->captures[capture_index].atom;
            }
        }
        return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
    }
    return atom;
}

/* The node after node in a preorder walk of root's subtree; 0 after the
   last. Walks the links, so it needs no stack. */
static sxml_ast_node_index next_in_subtree(const sxml_xpath_lowerer *state,
                                           sxml_ast_node_index root, sxml_ast_node_index node) {
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

/* Whether name is one of the closure's parameters. */
typedef struct parameter_query {
    const closure_locals *locals;
    sxml_name_identifier name;
} parameter_query;

static sxml_status is_parameter_name(sxml_xpath_lowerer *state, parameter_query query,
                                     bool *is_parameter) {
    *is_parameter = false;
    const closure_locals *locals = query.locals;
    for (uint32_t parameter_index = 0; parameter_index < locals->parameter_count;
         parameter_index += 1) {
        if (locals->parameter_names[parameter_index].value == 0) {
            continue;
        }
        sxml_name_identifier parameter = {0};
        const sxml_status status =
            resolve_name(state, locals->parameter_names[parameter_index], &parameter);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *is_parameter = *is_parameter || parameter.index == query.name.index;
    }
    return SXML_STATUS_OK;
}

/* The captures of the closure from slot parameter_count + 1 on and their
   atoms in the clause's function. */
static sxml_status find_captures(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    closure_locals *locals = frame->locals;
    const sxml_ast_node_index condition = locals->condition;
    size_t reference_count = 0;
    for (sxml_ast_node_index walked = condition; walked.value != 0;
         walked = next_in_subtree(state, condition, walked)) {
        if (node_of(state, walked)->kind == SXML_AST_KIND_VARIABLE_REFERENCE) {
            reference_count += 1;
        }
    }
    const size_t capacity = reference_count + FOCUS_PART_COUNT;
    sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 0,
            .scratch = {.element_count = capacity,
                        .element_size = sizeof(sxml_ir_atom),
                        .element_alignment = alignof(sxml_ir_atom)}});
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 1,
                .scratch = {.element_count = capacity,
                            .element_size = sizeof(sxml_xpath_lowering_capture_entry),
                            .element_alignment = alignof(sxml_xpath_lowering_capture_entry)}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->outer_atoms = frame->owned_memory[0];
    sxml_xpath_lowering_capture_entry *captures = frame->owned_memory[1];
    size_t capture_count = 0;
    size_t captured_count = 0;
    for (sxml_ast_node_index walked = condition; status == SXML_STATUS_OK && walked.value != 0;
         walked = next_in_subtree(state, condition, walked)) {
        if (node_of(state, walked)->kind != SXML_AST_KIND_VARIABLE_REFERENCE) {
            continue;
        }
        sxml_name_identifier name = {0};
        status = resolve_name(state, child_of(state, walked, SXML_AST_FIELD_NAME), &name);
        bool is_known = false;
        if (status == SXML_STATUS_OK) {
            status = is_parameter_name(state, (parameter_query){.locals = locals, .name = name}, &is_known);
        }
        for (size_t member_index = 0; member_index < locals->tuple_layout.member_count;
             member_index += 1) {
            is_known = is_known ||
                       locals->tuple_layout.members[member_index].variable_name.index == name.index;
        }
        for (size_t capture_index = 0; capture_index < capture_count; capture_index += 1) {
            is_known = is_known || captures[capture_index].variable_name.index == name.index;
        }
        if (status != SXML_STATUS_OK || is_known) {
            continue;
        }
        const sxml_ir_atom outer = find_visible_variable(state, name);
        if (outer.atom_kind == SXML_IR_ATOM_KIND_NONE) {
            continue;
        }
        if (outer.atom_kind == SXML_IR_ATOM_KIND_CONSTANT) {
            /* A constant needs no capture: the closure reads it directly. */
            captures[capture_count] =
                (sxml_xpath_lowering_capture_entry){.variable_name = name, .atom = outer};
        } else {
            captures[capture_count] = (sxml_xpath_lowering_capture_entry){
                .variable_name = name,
                .atom = slot_atom((sxml_ir_slot){
                    .value = locals->parameter_count + (uint32_t)captured_count + 1U})};
            locals->outer_atoms[captured_count] = outer;
            captured_count += 1;
        }
        capture_count += 1;
    }
    locals->captured_count = captured_count;
    locals->function_frame.function.captures = captures;
    locals->function_frame.function.capture_count = capture_count;
    return status;
}

static sxml_status begin_closure(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    closure_locals *locals = frame->locals;
    locals->function_frame = (sxml_xpath_lowering_function_frame){0};
    sxml_status status = find_captures(machine, frame);
    sxml_type_identifier signature = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_register_signature(
            state,
            (sxml_xpath_lowering_signature_request){
                .parameter_types = {.sequence_types = locals->parameter_types,
                                    .sequence_type_count = locals->parameter_count},
                .result_type = locals->result_type},
            &signature);
    }
    const uint32_t slot_count =
        locals->parameter_count + (uint32_t)locals->captured_count + FOCUS_PART_COUNT;
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            state->unit,
            &(sxml_ir_function){.function_name = {.index = 0},
                                .signature = signature,
                                .parameter_count = locals->parameter_count,
                                .captured_variables = {0},
                                .defining_function = state->function->function_index,
                                .slot_count = slot_count,
                                .body = {.value = 0},
                                .source_span = span_of(state, locals->clause)},
            &locals->closure);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->function_frame.function.function_index = locals->closure;
    locals->function_frame.function.function_level = state->function->function_level + 1U;
    locals->function_frame.function.slot_count = slot_count;
    locals->function_frame.focus =
        (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
    sxml_xpath_lowerer_enter_function(state, &locals->function_frame);
    for (uint32_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < locals->parameter_count;
         parameter_index += 1) {
        if (locals->parameter_names[parameter_index].value == 0) {
            continue;
        }
        sxml_name_identifier name = {0};
        status = resolve_name(state, locals->parameter_names[parameter_index], &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_push_variable(
                state, (sxml_xpath_lowering_scope_entry){
                           .variable_name = name,
                           .function_level = state->function->function_level,
                           .atom = slot_atom((sxml_ir_slot){.value = parameter_index + 1U})});
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->closure_block = (sxml_xpath_lowering_block){{0}, {0}};
    status = sxml_xquery_unpack_tuple(
        state, &(sxml_xquery_tuple_unpacking){.block = &locals->closure_block,
                                              .node = locals->clause,
                                              .tuple = slot_atom((sxml_ir_slot){.value = 1}),
                                              .layout = locals->tuple_layout});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = SXML_XQUERY_CLOSURE_STEP_FINISH;
    return sxml_xpath_lowering_call_atom(
        machine, (sxml_xpath_lowering_atom_call){.node = locals->condition,
                                                 .block = &locals->closure_block,
                                                 .atom = &locals->condition_value});
}

/* The closure's body: the condition, after a LET for each focus part it
   read from the captured slot that holds it. */
static sxml_status finish_closure_body(sxml_xpath_lowerer *state, closure_locals *locals,
                                       sxml_ir_expression_index *body) {
    sxml_ir_expression_index result = {0};
    sxml_status status = add_atom_expression(
        state, (atom_at){.node = locals->clause, .atom = locals->condition_value}, &result);
    if (status == SXML_STATUS_OK && locals->is_boolean) {
        /* The effective boolean value, as fn:boolean (XPath 4.0, 2.5.4). */
        sxml_ir_atom truth[2] = {{0}, {0}};
        sxml_ir_expression_index branches[2] = {{0}, {0}};
        status = sxml_xpath_lowerer_add_item_constant(state, sxml_atomic_item_true(), &truth[0]);
        if (status == SXML_STATUS_OK) {
            status =
                sxml_xpath_lowerer_add_item_constant(state, sxml_atomic_item_false(), &truth[1]);
        }
        for (size_t branch_index = 0; status == SXML_STATUS_OK && branch_index < 2;
             branch_index += 1) {
            status = add_atom_expression(
                state, (atom_at){.node = locals->clause, .atom = truth[branch_index]},
                &branches[branch_index]);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_expression(
                state,
                (sxml_xpath_lowering_expression_to_add){
                    .node = locals->clause,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                   .conditional = {.condition = locals->condition_value,
                                                   .then_branch = branches[0],
                                                   .else_branch = branches[1]}}},
                &result);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            state,
            (sxml_xpath_lowering_block_result){.block = locals->closure_block, .result = result},
            body);
    }
    const sxml_ir_focus read = locals->function_frame.focus.slots;
    const sxml_ir_slot parts[FOCUS_PART_COUNT] = {read.context_value, read.context_position,
                                                  read.context_size};
    uint32_t captured_part_count = 0;
    for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < FOCUS_PART_COUNT;
         part_index += 1) {
        if (parts[part_index].value == 0) {
            continue;
        }
        captured_part_count += 1;
        sxml_ir_expression_index captured = {0};
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = locals->clause,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                               .atom = slot_atom((sxml_ir_slot){
                                   .value = locals->parameter_count +
                                            (uint32_t)locals->captured_count +
                                            captured_part_count})}},
            &captured);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_expression(
                state,
                (sxml_xpath_lowering_expression_to_add){
                    .node = locals->clause,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                                   .let = {.variable = parts[part_index],
                                           .bound_expression = captured,
                                           .body = *body}}},
                body);
        }
    }
    return status;
}

static sxml_status finish_closure(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    closure_locals *locals = frame->locals;
    sxml_ir_expression_index body = {0};
    sxml_status status = finish_closure_body(state, locals, &body);
    sxml_xpath_lowerer_leave_function(state, &locals->function_frame);
    const sxml_ir_focus read = locals->function_frame.focus.slots;
    const sxml_xpath_lowering_focus_part part_names[FOCUS_PART_COUNT] = {
        SXML_XPATH_LOWERING_FOCUS_PART_VALUE, SXML_XPATH_LOWERING_FOCUS_PART_POSITION,
        SXML_XPATH_LOWERING_FOCUS_PART_SIZE};
    const sxml_ir_slot parts[FOCUS_PART_COUNT] = {read.context_value, read.context_position,
                                                  read.context_size};
    size_t outer_count = locals->captured_count;
    for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < FOCUS_PART_COUNT;
         part_index += 1) {
        if (parts[part_index].value == 0) {
            continue;
        }
        status = sxml_xpath_lowerer_read_focus(
            state,
            (sxml_xpath_lowering_focus_read){
                .block = frame->block, .node = locals->clause, .part = part_names[part_index]},
            &locals->outer_atoms[outer_count]);
        outer_count += 1;
    }
    sxml_ir_atom_list captured = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            state->unit,
            (sxml_ir_atom_view){.atoms = locals->outer_atoms, .atom_count = outer_count},
            &captured);
    }
    if (status == SXML_STATUS_OK) {
        sxml_ir_function_replacement replacement = {
            .index = locals->closure,
            .function = *sxml_ir_unit_get_function(state->unit, locals->closure)};
        replacement.function.body = body;
        replacement.function.captured_variables = captured;
        replacement.function.slot_count = locals->function_frame.function.slot_count;
        status = sxml_ir_unit_replace_function(state->unit, &replacement);
    }
    sxml_ir_expression_index definition = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = locals->clause,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION,
                               .function_definition = {.function = locals->closure}}},
            &definition);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, definition);
}

static sxml_status continue_closure(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame) {
    return frame->step == SXML_XQUERY_CLOSURE_STEP_BEGIN ? begin_closure(machine, frame)
                                             : finish_closure(machine, frame);
}

static const sxml_xpath_lowering_procedure closure_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery condition closure"),
    .step = continue_closure,
    .locals_size = sizeof(closure_locals)};

sxml_status sxml_xquery_push_condition_closure(sxml_xpath_lowering_machine *machine,
                                               const sxml_xquery_condition_closure *request) {
    if (request->parameter_count > SXML_XQUERY_CLOSURE_MAXIMUM_PARAMETER_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    closure_locals initial = {.clause = request->clause,
                              .condition = request->condition,
                              .parameter_count = request->parameter_count,
                              .result_type = request->result_type,
                              .tuple_layout = request->tuple_layout,
                              .is_boolean = request->is_boolean};
    for (uint32_t parameter_index = 0; parameter_index < request->parameter_count;
         parameter_index += 1) {
        initial.parameter_names[parameter_index] =
            request->parameter_names == NULL ? (sxml_ast_node_index){.value = 0}
                                             : request->parameter_names[parameter_index];
        initial.parameter_types[parameter_index] = request->parameter_types[parameter_index];
    }
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &closure_procedure,
            .node = request->condition,
            .block = request->block,
            .is_body = false,
            .destination = {.atom = request->closure, .atom_block = request->block},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof(initial)}},
        &frame);
}

/* ============================================================= windows */

/* The variables of a window clause in window array member order: the
   window variable's name node, then the start's and the end's current,
   position, previous and next (0 when not declared). */
typedef struct window_names {
    sxml_ast_node_index name_nodes[9];
} window_names;

static window_names find_window_names(const sxml_xpath_lowerer *state,
                                      sxml_ast_node_index clause) {
    window_names names = {{{0}}};
    names.name_nodes[0] =
        child_of(state, child_of(state, clause, SXML_AST_FIELD_VARIABLE), SXML_AST_FIELD_NAME);
    const sxml_ast_node_index conditions[2] = {child_of(state, clause, SXML_AST_FIELD_START),
                                               child_of(state, clause, SXML_AST_FIELD_END)};
    for (size_t condition_index = 0; condition_index < 2; condition_index += 1) {
        const sxml_ast_node_index condition = conditions[condition_index];
        if (condition.value == 0) {
            continue;
        }
        const size_t first = 1 + 4 * condition_index;
        names.name_nodes[first] = child_of(state, condition, SXML_AST_FIELD_CURRENT);
        const sxml_ast_node_index positional = child_of(state, condition, SXML_AST_FIELD_POSITION);
        names.name_nodes[first + 1] =
            positional.value == 0 ? positional : child_of(state, positional, SXML_AST_FIELD_NAME);
        names.name_nodes[first + 2] = child_of(state, condition, SXML_AST_FIELD_PREVIOUS);
        names.name_nodes[first + 3] = child_of(state, condition, SXML_AST_FIELD_NEXT);
    }
    return names;
}

/* 4.13.4: XQST0103 when two variables of the clause have one name. */
static sxml_status check_window_names(sxml_xpath_lowerer *state, const window_names *names) {
    sxml_name_identifier resolved[9] = {{0}};
    for (size_t name_index = 0; name_index < 9; name_index += 1) {
        if (names->name_nodes[name_index].value == 0) {
            continue;
        }
        const sxml_status status =
            resolve_name(state, names->name_nodes[name_index], &resolved[name_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        for (size_t earlier_index = 0; earlier_index < name_index; earlier_index += 1) {
            if (names->name_nodes[earlier_index].value != 0 &&
                resolved[earlier_index].index == resolved[name_index].index) {
                return sxml_xpath_lowerer_report_static_error(
                    state, names->name_nodes[name_index],
                    SXML_BYTE_VIEW_FROM_LITERAL(
                        "XQST0103 two variables of the window clause have the same name"));
            }
        }
    }
    return SXML_STATUS_OK;
}

typedef struct window_locals {
    sxml_xquery_segment_rest segment;
    window_names names;
    sxml_ir_atom sequence;
    sxml_ir_atom start_closure;
    sxml_ir_atom end_closure;
    sxml_ir_atom windows;
    sxml_ir_slot item;
    sxml_xpath_lowering_block body_block;
    size_t scope_mark;
    sxml_ir_expression_index rest;
} window_locals;

enum {
    SXML_XQUERY_WINDOW_STEP_BEGIN = 0,
    SXML_XQUERY_WINDOW_STEP_START_CLOSURE = 1,
    SXML_XQUERY_WINDOW_STEP_END_CLOSURE = 2,
    SXML_XQUERY_WINDOW_STEP_BODY = 3,
    SXML_XQUERY_WINDOW_STEP_FINISH = 4,
};

/* Pushes the condition closure of a start (or end) condition, its atom
   to *closure; the empty sequence without "when". */
typedef struct closure_request {
    sxml_xpath_lowering_frame *frame;
    /* 0 for the start condition, 1 for the end condition. */
    size_t condition_index;
    sxml_ir_atom *closure;
} closure_request;

static sxml_status push_closure(sxml_xpath_lowering_machine *machine, closure_request request) {
    sxml_xpath_lowerer *state = machine->state;
    sxml_xpath_lowering_frame *frame = request.frame;
    window_locals *locals = frame->locals;
    const sxml_ast_node_index clause = locals->segment.clause;
    const sxml_ast_node_index condition_clause = child_of(
        state, clause, request.condition_index == 0 ? SXML_AST_FIELD_START : SXML_AST_FIELD_END);
    const sxml_ast_node_index condition =
        condition_clause.value == 0
            ? condition_clause
            : child_of(state, condition_clause, SXML_AST_FIELD_CONDITION);
    if (condition.value == 0) {
        sxml_ir_expression_index empty = {0};
        const sxml_status status = sxml_xpath_lowerer_add_empty_sequence(state, clause, &empty);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowerer_bind_expression(
            state,
            (sxml_xpath_lowering_expression_in_block){.block = frame->block, .expression = empty},
            request.closure);
    }
    sxml_ast_node_index parameter_names[SXML_XQUERY_WINDOW_END_PARAMETER_COUNT] = {{0}};
    sxml_sequence_type parameter_types[SXML_XQUERY_WINDOW_END_PARAMETER_COUNT];
    const uint32_t parameter_count = request.condition_index == 0
                                         ? SXML_XQUERY_WINDOW_START_PARAMETER_COUNT
                                         : SXML_XQUERY_WINDOW_END_PARAMETER_COUNT;
    for (uint32_t parameter_index = 0; parameter_index < parameter_count; parameter_index += 1) {
        parameter_names[parameter_index] = locals->names.name_nodes[parameter_index + 1];
        parameter_types[parameter_index] = (sxml_sequence_type){
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    }
    return sxml_xquery_push_condition_closure(
        machine,
        &(sxml_xquery_condition_closure){
            .clause = clause,
            .condition = condition,
            .parameter_count = parameter_count,
            .parameter_names = parameter_names,
            .parameter_types = parameter_types,
            .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
            .tuple_layout = {.members = NULL, .member_count = 0},
            .is_boolean = false,
            .block = frame->block,
            .closure = request.closure});
}

/* The window call, the MAP's item slot and the window variables in its
   body block, then the rest of the segment there (4.13.4.3). */
static sxml_status begin_window_body(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    window_locals *locals = frame->locals;
    const sxml_ast_node_index clause = locals->segment.clause;
    const sxml_ast_node_index end = child_of(state, clause, SXML_AST_FIELD_END);
    uint32_t kind = 0;
    if (node_of(state, clause)->kind == SXML_AST_KIND_SLIDING_WINDOW_CLAUSE) {
        kind |= (uint32_t)SXML_XQUERY_WINDOW_SLIDING;
    }
    if (end.value != 0) {
        kind |= (uint32_t)SXML_XQUERY_WINDOW_HAS_END;
        if ((node_of(state, end)->flags & SXML_AST_FLAG_ONLY) != 0) {
            kind |= (uint32_t)SXML_XQUERY_WINDOW_ONLY_END;
        }
    }
    sxml_ir_atom arguments[4] = {locals->sequence, locals->start_closure, locals->end_closure,
                                 {0}};
    sxml_status status = sxml_xpath_lowerer_add_item_constant(
        state, sxml_atomic_item_from_integer((int64_t)kind), &arguments[3]);
    if (status == SXML_STATUS_OK) {
        status = bind_native_call(state,
                                  (native_call){.node = clause,
                                                .native_function = &sxml_xquery_window_function,
                                                .block = frame->block,
                                                .arguments = {.atoms = arguments, .atom_count = 4}},
                                  &locals->windows);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(state, &locals->item);
    }
    locals->scope_mark = state->scope_entry_count;
    locals->body_block = (sxml_xpath_lowering_block){{0}, {0}};
    for (uint32_t member_index = 0; status == SXML_STATUS_OK && member_index < 9;
         member_index += 1) {
        const sxml_ast_node_index name_node = locals->names.name_nodes[member_index];
        if (name_node.value == 0) {
            continue;
        }
        sxml_ir_atom key = {0};
        sxml_ir_atom value = {0};
        status = sxml_xpath_lowerer_add_item_constant(
            state, sxml_atomic_item_from_integer((int64_t)member_index + 1), &key);
        if (status == SXML_STATUS_OK) {
            status = bind_new_expression(
                state,
                (expression_in_block){
                    .block = &locals->body_block,
                    .node = name_node,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LOOKUP,
                                   .lookup = {.value = slot_atom(locals->item), .key = key}}},
                &value);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (member_index == 0) {
            /* The window variable may declare a type (4.13.4). */
            status = sxml_xpath_lowerer_push_typed_variable(
                state, (sxml_xpath_lowering_typed_variable){
                           .block = &locals->body_block,
                           .variable = child_of(state, clause, SXML_AST_FIELD_VARIABLE),
                           .value = value});
            continue;
        }
        sxml_name_identifier name = {0};
        status = resolve_name(state, name_node, &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_push_variable(
                state,
                (sxml_xpath_lowering_scope_entry){.variable_name = name,
                                                  .function_level = state->function->function_level,
                                                  .atom = value});
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = SXML_XQUERY_WINDOW_STEP_FINISH;
    return sxml_xquery_push_segment_rest(
        machine, &(sxml_xquery_segment_rest){.flwor_frame = locals->segment.flwor_frame,
                                             .clause = clause,
                                             .block = &locals->body_block,
                                             .rest = &locals->rest});
}

static sxml_status finish_window(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    window_locals *locals = frame->locals;
    sxml_ir_expression_index body = {0};
    sxml_status status = sxml_xpath_lowerer_finish_block(
        state,
        (sxml_xpath_lowering_block_result){.block = locals->body_block, .result = locals->rest},
        &body);
    sxml_xpath_lowerer_pop_variables(state, locals->scope_mark);
    sxml_ir_expression_index map = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = locals->segment.clause,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                               .map = {.input = locals->windows,
                                       .focus = {.context_value = locals->item},
                                       .body = body,
                                       .flags = 0}}},
            &map);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, map);
}

static sxml_status continue_window(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    window_locals *locals = frame->locals;
    switch (frame->step) {
    case SXML_XQUERY_WINDOW_STEP_BEGIN: {
        locals->names = find_window_names(state, locals->segment.clause);
        const sxml_status status = check_window_names(state, &locals->names);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* The binding sequence once, outside the conditions' scope. */
        frame->step = SXML_XQUERY_WINDOW_STEP_START_CLOSURE;
        return sxml_xpath_lowering_call_atom(
            machine, (sxml_xpath_lowering_atom_call){
                         .node = child_of(state, locals->segment.clause, SXML_AST_FIELD_SEQUENCE),
                         .block = frame->block,
                         .atom = &locals->sequence});
    }
    case SXML_XQUERY_WINDOW_STEP_START_CLOSURE:
        frame->step = SXML_XQUERY_WINDOW_STEP_END_CLOSURE;
        return push_closure(machine, (closure_request){.frame = frame,
                                                       .condition_index = 0,
                                                       .closure = &locals->start_closure});
    case SXML_XQUERY_WINDOW_STEP_END_CLOSURE:
        frame->step = SXML_XQUERY_WINDOW_STEP_BODY;
        return push_closure(machine, (closure_request){.frame = frame,
                                                       .condition_index = 1,
                                                       .closure = &locals->end_closure});
    case SXML_XQUERY_WINDOW_STEP_BODY:
        return begin_window_body(machine, frame);
    default:
        return finish_window(machine, frame);
    }
}

static const sxml_xpath_lowering_procedure window_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xquery window clause"),
    .step = continue_window,
    .locals_size = sizeof(window_locals)};

sxml_status sxml_xquery_push_window_clause(sxml_xpath_lowering_machine *machine,
                                           const sxml_xquery_segment_rest *request) {
    const window_locals initial = {.segment = *request};
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &window_procedure,
            .node = request->clause,
            .block = request->block,
            .is_body = false,
            .destination = {.expression = request->rest},
            .initial_locals = {.bytes = (const uint8_t *)&initial,
                               .byte_count = sizeof(initial)}},
        &frame);
}
