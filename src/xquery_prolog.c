/*
 * Lowering an XQuery main module: its prolog's declared functions and
 * variables and its query body (src/xquery_lower_internal.h; D55, D101,
 * D135; packet 3.10), with the library modules it imports (D148, packet
 * 3.11), and the internal compile entry.
 *
 * Library modules (src/xquery_module_internal.h) lower into the same
 * unit: every module's static context is read in the reading order, so
 * all declared functions are IR functions from 2 on and each module's
 * prolog variables are globals numbered after the host's, then linked to
 * its imports; each library module's functions and initializers are
 * lowered with a lowerer of its own. The dependency graph below spans
 * every module, so one walk orders all variables and finds cycles through
 * imports (XQDY0054).
 *
 * Golden file: src/xquery_lower.c (its former entry points, which this
 * file now holds).
 *
 * The static context comes from src/xquery_static.c; every expression of
 * the module is lowered through the embedding entry (D103) with it, so the
 * core and the XQuery parts lower bodies and initializers as they lower
 * the query body. The unit holds:
 *
 * - function 1, the main function: the query body, annotated %updating
 *   when the body is an updating expression (XQuery Update Facility 3.0,
 *   D187);
 * - the declared functions, one IR function per arity of each arity
 *   range (5.18): the full arity has the body, its result coerced to the
 *   declared type; a shorter arity computes the defaults of the parameters
 *   it lacks and calls the full one;
 * - one initializer function of arity 0 per variable with a value or a
 *   default (5.16), and for the context value declaration (5.17), its
 *   value coerced to the declared type;
 * - global variables: the host's, the prolog's, then the context global
 *   (glossary) when the prolog has initializers or a context value
 *   declaration. The query body and every variable initializer then read
 *   the focus as the fixed focus of the context global (5.17: "a fixed
 *   focus is created ... for the evaluation of the QueryBody ... and for
 *   the initializing expression of every variable declaration"), whose
 *   value the global part takes from the main function's focus slot or
 *   computes.
 *
 * Computation order (5.16: a variable "depends on" another when computing
 * it reads the other). After lowering, the expressions, atoms and map
 * entries each body and initializer added are scanned for the global
 * variables they read and the declared functions they call or reference,
 * a static superset of what they read when run. A walk from the query
 * body orders the variables it reaches dependencies first; a cycle of
 * that superset is no error by itself: a variable depends on another
 * only when computing it reads the other (5.16), which the evaluator
 * finds, raising XQDY0054 for a global being computed (D263, replacing
 * the static check). Each reached variable's global then gets, in
 * that order, an initializer function that calls its lowered initializer,
 * so the global part computes them by initializer index
 * (src/evaluator_global.c); variables the body never reaches get none and
 * are never computed. The walk keeps its path on an explicit stack (D71).
 *
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026)
 * unless another specification is named.
 */
#include "xquery_lower_internal.h"

#include "evaluator_machine_internal.h"
#include "full_text_internal.h"
#include "ir_pass_internal.h"
#include "sxml/memory.h"
#include "xpath_lower_internal.h"
#include "xquery_load_module_internal.h"
#include "xquery_module_internal.h"
#include "xquery_static_internal.h"
#include "xquery_update_internal.h"

#include <stdalign.h>
#include <string.h>

/* ----------------------------------------------------------- AST access */

static const sxml_ast_node *node_of(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_get_node(ast, node);
}

static sxml_ast_node_index child_of(const sxml_ast *ast, sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ir_source_span span_of(const sxml_ast *ast, sxml_ast_node_index node) {
    const sxml_ast_source_span span = node_of(ast, node)->source_span;
    return (sxml_ir_source_span){.byte_offset = span.byte_offset,
                                 .byte_count = span.byte_count,
                                 .line_number = span.line_number,
                                 .column_number = span.column_number};
}

static sxml_ir_atom global_atom(uint32_t global_number) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_GLOBAL_VARIABLE,
                          .global_variable = {.value = global_number}};
}

static bool accepts_everything(sxml_sequence_type sequence_type) {
    return sequence_type.item_type.index == sxml_builtin_type_identifier(SXML_TYPE_ITEM).index &&
           sequence_type.occurrence == SXML_OCCURRENCE_ZERO_OR_MORE;
}

/* ------------------------------------------------------------ Regions */

/* The entries of the unit's arrays one lowering added. */
typedef struct lowered_region {
    uint32_t first_expression;
    uint32_t end_expression;
    uint32_t first_atom;
    uint32_t end_atom;
    uint32_t first_map_entry;
    uint32_t end_map_entry;
} lowered_region;

static lowered_region begin_region(const sxml_ir_unit *unit) {
    return (lowered_region){.first_expression = unit->expression_count,
                            .first_atom = unit->atom_count,
                            .first_map_entry = unit->map_entry_count};
}

static void end_region(const sxml_ir_unit *unit, lowered_region *region) {
    region->end_expression = unit->expression_count;
    region->end_atom = unit->atom_count;
    region->end_map_entry = unit->map_entry_count;
}

/* ------------------------------------------------------------- Module */

/*
 * The nodes of the dependency graph: 0 the query body, 1 to P the prolog
 * variables, P + 1 the context global, then one per IR function of the
 * declared functions (in function index order from function 2).
 */
typedef struct module_lowering {
    /* The module being lowered: its lowerer, AST and static context. */
    sxml_xpath_lowerer *lowerer;
    const sxml_ast *ast;
    sxml_xquery_static_context *context;
    /* The main module's lowerer, which also allocates the scratch arrays
       of the whole unit. */
    sxml_xpath_lowerer *main_lowerer;
    sxml_ir_unit *unit;
    sxml_error_record *error_record;
    /* The module set and the static context of each module (D148). */
    const sxml_xquery_module_set *set;
    sxml_xquery_static_context *contexts;
    /* The host's variables are globals 1 to host_variable_count; prolog
       variables of every module follow, prolog_variable_count of them. */
    size_t host_variable_count;
    /* Prolog variables, the context global, declared IR functions. */
    size_t prolog_variable_count;
    size_t declared_ir_function_count;
    size_t node_count;
    /* Per node: what lowering it added, and for variables (and the context
       global) their initializer function, 0 for none. */
    lowered_region *regions;
    sxml_ir_function_index *initializers;
    /* Whether the context global and the fixed focus are used. */
    bool has_context_global;
    uint32_t context_global_number;
} module_lowering;

static size_t context_node(const module_lowering *module) {
    return module->prolog_variable_count + 1;
}

static size_t function_node(const module_lowering *module, sxml_ir_function_index function) {
    return module->prolog_variable_count + 2 + (function.value - 2);
}

static bool is_variable_node(const module_lowering *module, size_t node) {
    return node >= 1 && node <= module->prolog_variable_count + 1;
}

/* The focus of the query body and of variable initializers. */
static sxml_xpath_lowering_focus fixed_focus(const module_lowering *module) {
    if (module->has_context_global) {
        return (sxml_xpath_lowering_focus){
            .kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED,
            .fixed_value = global_atom(module->context_global_number)};
    }
    return (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
}


/* Puts the first parameter_count parameters of a FUNCTION_DECLARATION in
   scope as slots 1 onwards. */
typedef struct parameter_scope {
    sxml_ast_node_index declaration;
    uint32_t parameter_count;
} parameter_scope;

static sxml_status push_parameters(module_lowering *module, parameter_scope scope) {
    sxml_xpath_lowerer *state = module->lowerer;
    sxml_status status = SXML_STATUS_OK;
    uint32_t parameter_number = 1;
    for (sxml_ast_node_index parameter = sxml_xquery_first_parameter(module->ast, scope.declaration);
         status == SXML_STATUS_OK && parameter.value != 0 &&
         parameter_number <= scope.parameter_count;
         parameter = sxml_xquery_next_parameter(module->ast, parameter)) {
        sxml_name_identifier name = {0};
        if (node_of(module->ast, parameter)->kind == SXML_AST_KIND_EXTENDED_FIELD_DECLARATION) {
            /* A record constructor's parameter: the field's name, in no
               namespace (5.20.2). */
            status = sxml_name_pool_intern_name(
                state->name_pool,
                &(sxml_name_request){
                    .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE),
                    .local_name = sxml_xquery_field_name(module->ast, parameter)},
                &name);
        } else {
            status = sxml_xpath_lowerer_resolve_variable_name(
                state, sxml_xquery_parameter_name(module->ast, parameter), &name);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_push_variable(
                state, (sxml_xpath_lowering_scope_entry){.variable_name = name,
                                                         .function_level = 0,
                                                         .atom = sxml_ir_slot_atom((sxml_ir_slot){.value = parameter_number})});
        }
        parameter_number += 1;
    }
    return status;
}

/* One expression lowered as the body of an IR function. */
typedef struct body_request {
    sxml_ir_function_index function;
    sxml_ast_node_index node;
    sxml_xpath_lowering_focus focus;
    /* A FUNCTION_DECLARATION whose first parameter_count parameters are
       slots 1 onwards; 0 for none. */
    parameter_scope parameters;
    /* The declared type the value is coerced to, and why. */
    sxml_sequence_type result_type;
    sxml_ir_coercion_role role;
    /* For the main function: reserve a focus slot for the host's context
       value, which the global part copies to the context global. */
    bool reserves_context_slot;
} body_request;

/* Lowers request->node as the body of request->function, coerced to
   request->result_type, and replaces the function. */
static sxml_status lower_body(module_lowering *module, const body_request *request) {
    sxml_xpath_lowerer *state = module->lowerer;
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = request->function,
                     .function_level = 0,
                     .slot_count = request->parameters.parameter_count,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = request->focus,
    };
    sxml_xpath_lowerer_enter_function(state, &frame);
    sxml_status status = SXML_STATUS_OK;
    sxml_ir_slot context_slot = {0};
    if (request->reserves_context_slot) {
        status = sxml_xpath_lowerer_add_slot(state, &context_slot);
    }
    if (status == SXML_STATUS_OK && request->parameters.declaration.value != 0) {
        status = push_parameters(module, request->parameters);
    }
    sxml_ir_expression_index lowered = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_lower_embedded(
            state,
            &(sxml_xpath_lowering_embedding){
                .ast = module->ast, .node = request->node, .static_context = NULL, .focus = NULL},
            &lowered);
    }
    sxml_xpath_lowering_block block = {0};
    if (status == SXML_STATUS_OK && !accepts_everything(request->result_type)) {
        /* The declared type is checked by coercion (5.16, 5.17, 5.18). */
        sxml_ir_atom value = {0};
        status = sxml_xpath_lowerer_bind_expression(
            state,
            (sxml_xpath_lowering_expression_in_block){.block = &block, .expression = lowered},
            &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_expression(
                state,
                (sxml_xpath_lowering_expression_to_add){
                    .node = request->node,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_COERCE,
                                   .coercion = {.value = value,
                                                .sequence_type = request->result_type,
                                                .role = request->role}}},
                &lowered);
        }
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            state, (sxml_xpath_lowering_block_result){.block = block, .result = lowered}, &body);
    }
    sxml_xpath_lowerer_leave_function(state, &frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = request->function,
        .function = *sxml_ir_unit_get_function(module->unit, request->function)};
    replacement.function.body = body;
    replacement.function.slot_count = frame.function.slot_count;
    if (request->function.value == 1) {
        replacement.function.focus = request->reserves_context_slot
                                         ? (sxml_ir_focus){.context_value = context_slot}
                                         : frame.focus.slots;
    } else if (request->focus.kind == SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS) {
        /* A default function: the parts of the focus it reads, which its
           callers pass (glossary `default_function`). */
        replacement.function.focus = frame.focus.parts_read;
    }
    return sxml_ir_unit_replace_function(module->unit, &replacement);
}

/* --------------------------------------------------- Declared functions */

/* The IR function of a shorter arity (5.18.3): the defaults of the
   parameters from arity on, each coerced to its parameter's type, and a
   call of the full arity. Defaults see no parameters; a declared
   function's are calls of its default functions, a record constructor's
   are lowered here. The function's focus is the three slots after its
   parameters, which fn:function-lookup fills with its caller's focus
   (F&O 4.0, 13.1.1, PR 2941; sxml_evaluator_find_unit_function); a
   function item made otherwise leaves them absent, so a default that
   reads the focus raises XPDY0002 (a static call or a reference that
   needs the focus calls the defaults itself, with its own). */
typedef struct default_request {
    const sxml_xquery_declared_function *function;
    uint32_t arity;
} default_request;

static sxml_status lower_defaults(module_lowering *module, default_request request) {
    sxml_xpath_lowerer *state = module->lowerer;
    const sxml_xquery_declared_function *function = request.function;
    const sxml_ir_function_index index = {
        .value = function->first_function.value + (request.arity - function->required_count)};
    const sxml_ir_function_index full = {
        .value = function->first_function.value +
                 (function->parameter_count - function->required_count)};
    sxml_ir_atom *arguments = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        state,
        (sxml_xpath_lowering_scratch_request){.element_count = function->parameter_count,
                                              .element_size = sizeof(sxml_ir_atom),
                                              .element_alignment = alignof(sxml_ir_atom)},
        (void **)&arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_focus caller_focus = {.context_value = {.value = request.arity + 1U},
                                        .context_position = {.value = request.arity + 2U},
                                        .context_size = {.value = request.arity + 3U}};
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = index,
                     .function_level = 0,
                     .slot_count = request.arity + 3U,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS, .slots = caller_focus},
    };
    sxml_xpath_lowerer_enter_function(state, &frame);
    sxml_xpath_lowering_block block = {0};
    uint32_t parameter_number = 0;
    for (sxml_ast_node_index parameter =
             sxml_xquery_first_parameter(module->ast, function->declaration);
         status == SXML_STATUS_OK && parameter.value != 0;
         parameter = sxml_xquery_next_parameter(module->ast, parameter)) {
        parameter_number += 1;
        if (parameter_number <= request.arity) {
            arguments[parameter_number - 1] = sxml_ir_slot_atom((sxml_ir_slot){.value = parameter_number});
            continue;
        }
        const sxml_ast_node_index default_value =
            child_of(module->ast, parameter, SXML_AST_FIELD_DEFAULT);
        const sxml_function_parameter *declared =
            &module->context->function_parameters[function->first_parameter + parameter_number - 1];
        if (declared->parameter_default == SXML_PARAMETER_DEFAULT_EXPRESSION) {
            /* The default function, with this function's focus. */
            status = sxml_xpath_lowerer_call_default_function(
                state,
                (sxml_xpath_lowering_default_call){
                    .block = &block,
                    .node = default_value,
                    .default_function_number = declared->default_function_number},
                &arguments[parameter_number - 1]);
            continue;
        }
        sxml_ir_expression_index lowered = {0};
        status = sxml_xpath_lowerer_lower_embedded(
            state,
            &(sxml_xpath_lowering_embedding){
                .ast = module->ast, .node = default_value, .static_context = NULL, .focus = NULL},
            &lowered);
        sxml_ir_atom value = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_bind_expression(
                state,
                (sxml_xpath_lowering_expression_in_block){.block = &block, .expression = lowered},
                &value);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_coerce_atom(
                state,
                (sxml_xpath_lowering_coercion_request){
                    .block = &block,
                    .node = default_value,
                    .value = value,
                    .sequence_type = module->context->parameter_types[function->first_parameter +
                                                                      parameter_number - 1],
                    .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
                &arguments[parameter_number - 1]);
        }
    }
    sxml_ir_atom_list argument_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            module->unit,
            (sxml_ir_atom_view){.atoms = arguments, .atom_count = function->parameter_count},
            &argument_list);
    }
    sxml_ir_expression_index call = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = function->declaration,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                               .static_call = {.target = {.target_kind =
                                                              SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                                          .function = full},
                                               .arguments = argument_list}}},
            &call);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            state, (sxml_xpath_lowering_block_result){.block = block, .result = call}, &body);
    }
    sxml_xpath_lowerer_leave_function(state, &frame);
    sxml_xpath_lowerer_release_scratch(state, arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = index, .function = *sxml_ir_unit_get_function(module->unit, index)};
    replacement.function.body = body;
    replacement.function.slot_count = frame.function.slot_count;
    replacement.function.focus = caller_focus;
    return sxml_ir_unit_replace_function(module->unit, &replacement);
}

/* The full arity of a record's constructor (5.20.2): a map of every
   field, the parameters' values and the %static fields' defaults, coerced
   to the record type. */
static sxml_status lower_constructor(module_lowering *module,
                                     const sxml_xquery_declared_function *function) {
    sxml_xpath_lowerer *state = module->lowerer;
    const sxml_ast *ast = module->ast;
    const sxml_ir_function_index index = {
        .value = function->first_function.value +
                 (function->parameter_count - function->required_count)};
    size_t field_count = 0;
    for (sxml_ast_node_index field = child_of(ast, function->declaration, SXML_AST_FIELD_FIELD);
         field.value != 0; field = sxml_ast_find_next_with_same_field(ast, field)) {
        field_count += 1;
    }
    sxml_ir_map_entry *entries = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        state,
        (sxml_xpath_lowering_scratch_request){.element_count = field_count,
                                              .element_size = sizeof(sxml_ir_map_entry),
                                              .element_alignment = alignof(sxml_ir_map_entry)},
        (void **)&entries);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = index,
                     .function_level = 0,
                     .slot_count = function->parameter_count,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT},
    };
    sxml_xpath_lowerer_enter_function(state, &frame);
    sxml_xpath_lowering_block block = {0};
    uint32_t parameter_number = 0;
    size_t entry_index = 0;
    for (sxml_ast_node_index field = child_of(ast, function->declaration, SXML_AST_FIELD_FIELD);
         status == SXML_STATUS_OK && field.value != 0;
         field = sxml_ast_find_next_with_same_field(ast, field)) {
        status = sxml_xpath_lowerer_add_string_constant(
            state, sxml_xquery_field_name(ast, field), &entries[entry_index].key);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if ((node_of(ast, field)->flags & SXML_AST_FLAG_STATIC) == 0) {
            parameter_number += 1;
            entries[entry_index].value = sxml_ir_slot_atom((sxml_ir_slot){.value = parameter_number});
        } else {
            const sxml_ast_node_index default_value = child_of(ast, field, SXML_AST_FIELD_DEFAULT);
            sxml_ir_expression_index lowered = {0};
            if (default_value.value != 0) {
                status = sxml_xpath_lowerer_lower_embedded(
                    state,
                    &(sxml_xpath_lowering_embedding){
                        .ast = ast, .node = default_value, .static_context = NULL, .focus = NULL},
                    &lowered);
            } else {
                status = sxml_xpath_lowerer_add_empty_sequence(state, field, &lowered);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xpath_lowerer_bind_expression(
                    state,
                    (sxml_xpath_lowering_expression_in_block){.block = &block,
                                                              .expression = lowered},
                    &entries[entry_index].value);
            }
        }
        entry_index += 1;
    }
    sxml_ir_map_entry_list entry_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_map_entries(
            module->unit,
            (sxml_ir_map_entry_view){.map_entries = entries, .map_entry_count = field_count},
            &entry_list);
    }
    sxml_ir_expression_index map = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = function->declaration,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                               .map_constructor = {.entries = entry_list}}},
            &map);
    }
    sxml_ir_atom value = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_bind_expression(
            state, (sxml_xpath_lowering_expression_in_block){.block = &block, .expression = map},
            &value);
    }
    sxml_ir_expression_index coerced = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){
                .node = function->declaration,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_COERCE,
                               .coercion = {.value = value,
                                            .sequence_type = function->result_type,
                                            .role = SXML_IR_COERCION_ROLE_FUNCTION_RESULT}}},
            &coerced);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            state, (sxml_xpath_lowering_block_result){.block = block, .result = coerced}, &body);
    }
    sxml_xpath_lowerer_leave_function(state, &frame);
    sxml_xpath_lowerer_release_scratch(state, entries);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = index, .function = *sxml_ir_unit_get_function(module->unit, index)};
    replacement.function.body = body;
    replacement.function.slot_count = frame.function.slot_count;
    return sxml_ir_unit_replace_function(module->unit, &replacement);
}

/* Lowers the default functions of every declared function of the current
   module (glossary `default_function`): each default, coerced to its
   parameter's type, with the caller's focus in slots 1 to 3 (5.18.3: the
   focus "is the focus of the construct that invokes the function"); the
   function's focus becomes the parts it reads. */
static sxml_status lower_default_functions(module_lowering *module) {
    const sxml_xquery_static_context *context = module->context;
    sxml_status status = SXML_STATUS_OK;
    for (size_t function_index = 0;
         status == SXML_STATUS_OK && function_index < context->declared_function_count;
         function_index += 1) {
        const sxml_xquery_declared_function *function =
            &context->declared_functions[function_index];
        uint32_t parameter_index = 0;
        for (sxml_ast_node_index parameter =
                 sxml_xquery_first_parameter(module->ast, function->declaration);
             status == SXML_STATUS_OK && parameter.value != 0;
             parameter = sxml_xquery_next_parameter(module->ast, parameter)) {
            const size_t position = function->first_parameter + parameter_index;
            parameter_index += 1;
            const sxml_function_parameter *declared = &context->function_parameters[position];
            if (declared->parameter_default != SXML_PARAMETER_DEFAULT_EXPRESSION) {
                continue;
            }
            const sxml_ir_function_index index = {.value = declared->default_function_number};
            lowered_region *region = &module->regions[function_node(module, index)];
            *region = begin_region(module->unit);
            status = lower_body(
                module,
                &(body_request){
                    .function = index,
                    .node = child_of(module->ast, parameter, SXML_AST_FIELD_DEFAULT),
                    .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
                              .slots = {.context_value = {.value = 1},
                                        .context_position = {.value = 2},
                                        .context_size = {.value = 3}}},
                    .parameters = {.declaration = {.value = 0}, .parameter_count = 3},
                    .result_type = context->parameter_types[position],
                    .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
                    .reserves_context_slot = false});
            end_region(module->unit, region);
        }
    }
    return status;
}

/* Whether function is a default function of some module of the set. */
static bool is_default_function(const module_lowering *module, sxml_ir_function_index function) {
    for (size_t module_index = 0; module_index < module->set->module_count; module_index += 1) {
        const sxml_xquery_static_context *context = &module->contexts[module_index];
        for (size_t function_index = 0; function_index < context->declared_function_count;
             function_index += 1) {
            const sxml_xquery_declared_function *declared =
                &context->declared_functions[function_index];
            for (uint32_t parameter_index = 0; parameter_index < declared->parameter_count;
                 parameter_index += 1) {
                if (context->function_parameters[declared->first_parameter + parameter_index]
                        .default_function_number == function.value) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Where close_default_focus stands: whether a round added a part. */
typedef struct focus_closure {
    bool is_changed;
} focus_closure;

/* Adds to caller's focus the parts it passes on, in slots 1 to 3, to the
   default functions it calls that read them; closure->is_changed is set
   when one was added. */
static sxml_status add_passed_parts(module_lowering *module, sxml_ir_function_index caller,
                                    focus_closure *closure) {
    const lowered_region *region = &module->regions[function_node(module, caller)];
    sxml_ir_focus focus = sxml_ir_unit_get_function(module->unit, caller)->focus;
    bool is_added = false;
    for (uint32_t expression_index = region->first_expression;
         expression_index < region->end_expression; expression_index += 1) {
        const sxml_ir_expression *expression = sxml_ir_unit_get_expression(
            module->unit, (sxml_ir_expression_index){.value = expression_index});
        if (expression->expression_kind != SXML_IR_EXPRESSION_KIND_STATIC_CALL ||
            expression->static_call.target.target_kind !=
                SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION ||
            expression->static_call.arguments.atom_count != 3 ||
            !is_default_function(module, expression->static_call.target.function)) {
            continue;
        }
        const sxml_ir_focus callee =
            sxml_ir_unit_get_function(module->unit, expression->static_call.target.function)
                ->focus;
        const uint32_t callee_parts[3] = {callee.context_value.value,
                                          callee.context_position.value,
                                          callee.context_size.value};
        sxml_ir_slot *caller_parts[3] = {&focus.context_value, &focus.context_position,
                                         &focus.context_size};
        for (size_t part_index = 0; part_index < 3; part_index += 1) {
            const sxml_ir_atom argument =
                module->unit->atoms[expression->static_call.arguments.first_atom_index +
                                    part_index];
            if (callee_parts[part_index] == 0 || argument.atom_kind != SXML_IR_ATOM_KIND_SLOT ||
                argument.slot.value == 0 || argument.slot.value > 3 ||
                caller_parts[argument.slot.value - 1]->value != 0) {
                continue;
            }
            caller_parts[argument.slot.value - 1]->value = argument.slot.value;
            is_added = true;
        }
    }
    if (!is_added) {
        return SXML_STATUS_OK;
    }
    closure->is_changed = true;
    sxml_ir_function_replacement replacement = {
        .index = caller, .function = *sxml_ir_unit_get_function(module->unit, caller)};
    replacement.function.focus = focus;
    return sxml_ir_unit_replace_function(module->unit, &replacement);
}

/* After every module's default functions are lowered: a default that
   calls another reads the parts it passes to it (glossary
   `default_function`), until nothing changes. */
static sxml_status close_default_focus(module_lowering *module) {
    sxml_status status = SXML_STATUS_OK;
    focus_closure closure = {.is_changed = true};
    while (status == SXML_STATUS_OK && closure.is_changed) {
        closure.is_changed = false;
        for (size_t module_index = 0; module_index < module->set->module_count;
             module_index += 1) {
            const sxml_xquery_static_context *context = &module->contexts[module_index];
            for (size_t function_index = 0; function_index < context->declared_function_count;
                 function_index += 1) {
                const sxml_xquery_declared_function *declared =
                    &context->declared_functions[function_index];
                for (uint32_t parameter_index = 0;
                     status == SXML_STATUS_OK && parameter_index < declared->parameter_count;
                     parameter_index += 1) {
                    const uint32_t number =
                        context->function_parameters[declared->first_parameter + parameter_index]
                            .default_function_number;
                    if (number != 0) {
                        status = add_passed_parts(
                            module, (sxml_ir_function_index){.value = number}, &closure);
                    }
                }
            }
        }
    }
    return status;
}

/* Lowers every IR function of every declared function, recording each
   one's region. */
static sxml_status lower_declared_functions(module_lowering *module) {
    const sxml_xquery_static_context *context = module->context;
    sxml_status status = SXML_STATUS_OK;
    for (size_t function_index = 0;
         status == SXML_STATUS_OK && function_index < context->declared_function_count;
         function_index += 1) {
        const sxml_xquery_declared_function *function =
            &context->declared_functions[function_index];
        /* 5.18: the function's %annotations, which its function items
           carry (F&O 4.0, fn:function-annotations). */
        sxml_function_annotation_view annotations = {.function_annotations = NULL,
                                                     .function_annotation_count = 0};
        if (function->record_type.index == 0) {
            status = sxml_xpath_lowerer_read_annotations(module->lowerer, function->declaration,
                                                         &annotations);
        }
        for (uint32_t arity = function->required_count;
             status == SXML_STATUS_OK && arity <= function->parameter_count; arity += 1) {
            const sxml_ir_function_index index = {
                .value = function->first_function.value + (arity - function->required_count)};
            if (annotations.function_annotation_count != 0) {
                sxml_ir_function_replacement replacement = {
                    .index = index, .function = *sxml_ir_unit_get_function(module->unit, index)};
                replacement.function.function_annotations = annotations;
                status = sxml_ir_unit_replace_function(module->unit, &replacement);
                if (status != SXML_STATUS_OK) {
                    break;
                }
            }
            lowered_region *region = &module->regions[function_node(module, index)];
            *region = begin_region(module->unit);
            if (arity < function->parameter_count) {
                status = lower_defaults(module,
                                        (default_request){.function = function, .arity = arity});
            } else if (function->record_type.index != 0) {
                status = lower_constructor(module, function);
            } else {
                status = lower_body(
                    module,
                    &(body_request){
                        .function = index,
                        .node = child_of(module->ast, function->declaration, SXML_AST_FIELD_BODY),
                        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT},
                        .parameters = {.declaration = function->declaration,
                                       .parameter_count = arity},
                        .result_type = function->result_type,
                        .role = SXML_IR_COERCION_ROLE_FUNCTION_RESULT,
                        .reserves_context_slot = false});
            }
            end_region(module->unit, region);
        }
    }
    return status;
}

/* --------------------------------------------------------- Initializers */

/* The expression that gives a variable or the context value its value:
   the value, or with "external" the default; 0 for none. */
static sxml_ast_node_index initializing_expression(const sxml_ast *ast,
                                                   sxml_ast_node_index declaration) {
    if ((node_of(ast, declaration)->flags & SXML_AST_FLAG_EXTERNAL) != 0) {
        return child_of(ast, declaration, SXML_AST_FIELD_DEFAULT);
    }
    return child_of(ast, declaration, SXML_AST_FIELD_VALUE);
}

/* An initializer: an IR function of arity 0 returning its declared type. */
typedef struct initializer_request {
    size_t node;
    sxml_ast_node_index declaration;
    sxml_sequence_type declared_type;
    sxml_xpath_lowering_focus focus;
} initializer_request;

static sxml_status lower_initializer(module_lowering *module, const initializer_request *request) {
    const sxml_ast_node_index expression = initializing_expression(module->ast, request->declaration);
    if (expression.value == 0) {
        return SXML_STATUS_OK;
    }
    sxml_type_identifier signature = {0};
    sxml_status status = sxml_xpath_lowerer_register_signature(
        module->lowerer,
        (sxml_xpath_lowering_signature_request){
            .parameter_types = {.sequence_types = NULL, .sequence_type_count = 0},
            .result_type = request->declared_type},
        &signature);
    sxml_ir_function_index function = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            module->unit,
            &(sxml_ir_function){.signature = signature,
                                .source_span = span_of(module->ast, request->declaration)},
            &function);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    module->initializers[request->node] = function;
    lowered_region *region = &module->regions[request->node];
    *region = begin_region(module->unit);
    status = lower_body(module, &(body_request){.function = function,
                                               .node = expression,
                                               .focus = request->focus,
                                               .parameters = {.declaration = {0},
                                                              .parameter_count = 0},
                                               .result_type = request->declared_type,
                                               .role = SXML_IR_COERCION_ROLE_VARIABLE,
                                               .reserves_context_slot = false});
    end_region(module->unit, region);
    return status;
}

/* The initializers of the current module's variables, and of the main
   module's context value declaration (a library module's only states a
   type, 5.17). */
static sxml_status lower_initializers(module_lowering *module) {
    const sxml_xquery_static_context *context = module->context;
    sxml_status status = SXML_STATUS_OK;
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && variable_index < context->prolog_variable_count;
         variable_index += 1) {
        const size_t position = context->host_variable_count + variable_index;
        status = lower_initializer(
            module,
            &(initializer_request){
                .node = context->global_numbers[position] - module->host_variable_count,
                .declaration = context->variable_nodes[variable_index],
                .declared_type = context->variable_declarations[position].declared_type,
                .focus = fixed_focus(module)});
    }
    if (status == SXML_STATUS_OK && context == &module->contexts[0] &&
        context->context_value_declaration.value != 0) {
        /* The context value's own initializer has no focus (5.17). */
        status = lower_initializer(
            module, &(initializer_request){.node = context_node(module),
                                           .declaration = context->context_value_declaration,
                                           .declared_type = context->context_value_type,
                                           .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT}});
    }
    return status;
}

/* ------------------------------------------------------- Dependencies */

/* Edges of the dependency graph, counted (targets NULL) or written. */
typedef struct edge_sink {
    const module_lowering *module;
    uint32_t *targets;
    size_t edge_count;
} edge_sink;

static void add_edge(edge_sink *sink, size_t target) {
    if (target == SIZE_MAX) {
        return;
    }
    if (sink->targets != NULL) {
        sink->targets[sink->edge_count] = (uint32_t)target;
    }
    sink->edge_count += 1;
}

/* The node a read of atom depends on; SIZE_MAX for none. */
static size_t node_of_atom(const module_lowering *module, sxml_ir_atom atom) {
    if (atom.atom_kind != SXML_IR_ATOM_KIND_GLOBAL_VARIABLE) {
        return SIZE_MAX;
    }
    const uint32_t global_number = atom.global_variable.value;
    if (module->has_context_global && global_number == module->context_global_number) {
        return context_node(module);
    }
    const size_t host_count = module->host_variable_count;
    if (global_number > host_count && global_number <= host_count + module->prolog_variable_count) {
        return global_number - host_count;
    }
    return SIZE_MAX;
}

/* The node a call or reference of target depends on; SIZE_MAX for none. */
static size_t node_of_target(const module_lowering *module, sxml_ir_function_target target) {
    if (target.target_kind != SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION ||
        target.function.value < 2 ||
        target.function.value >= 2 + module->declared_ir_function_count) {
        return SIZE_MAX;
    }
    return function_node(module, target.function);
}

/* The atoms an expression holds itself (atom lists and map entries are
   scanned as runs of the unit's arrays) and the function it calls. */
static void scan_expression(edge_sink *sink, const sxml_ir_expression *expression) {
    const module_lowering *module = sink->module;
    switch (expression->expression_kind) {
    case SXML_IR_EXPRESSION_KIND_ATOM:
        add_edge(sink, node_of_atom(module, expression->atom));
        return;
    case SXML_IR_EXPRESSION_KIND_CONDITIONAL:
        add_edge(sink, node_of_atom(module, expression->conditional.condition));
        return;
    case SXML_IR_EXPRESSION_KIND_BINARY:
        add_edge(sink, node_of_atom(module, expression->binary.left_operand));
        add_edge(sink, node_of_atom(module, expression->binary.right_operand));
        return;
    case SXML_IR_EXPRESSION_KIND_UNARY:
        add_edge(sink, node_of_atom(module, expression->unary.operand));
        return;
    case SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE:
        add_edge(sink, node_of_target(module, expression->function_reference.target));
        return;
    case SXML_IR_EXPRESSION_KIND_STATIC_CALL:
        add_edge(sink, node_of_target(module, expression->static_call.target));
        return;
    case SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL:
        add_edge(sink, node_of_atom(module, expression->dynamic_call.function_item));
        return;
    case SXML_IR_EXPRESSION_KIND_LOOKUP:
        add_edge(sink, node_of_atom(module, expression->lookup.value));
        add_edge(sink, node_of_atom(module, expression->lookup.key));
        return;
    case SXML_IR_EXPRESSION_KIND_MAP:
        add_edge(sink, node_of_atom(module, expression->map.input));
        return;
    case SXML_IR_EXPRESSION_KIND_FILTER:
        add_edge(sink, node_of_atom(module, expression->filter.input));
        return;
    case SXML_IR_EXPRESSION_KIND_QUANTIFIED:
        add_edge(sink, node_of_atom(module, expression->quantified.input));
        return;
    case SXML_IR_EXPRESSION_KIND_SORT:
        add_edge(sink, node_of_atom(module, expression->sort.input));
        return;
    case SXML_IR_EXPRESSION_KIND_STEP:
        add_edge(sink, node_of_atom(module, expression->step.origin));
        return;
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER:
        add_edge(sink, node_of_atom(module, expression->document_order.input));
        return;
    case SXML_IR_EXPRESSION_KIND_CAST:
    case SXML_IR_EXPRESSION_KIND_CASTABLE:
        add_edge(sink, node_of_atom(module, expression->cast.value));
        return;
    case SXML_IR_EXPRESSION_KIND_INSTANCE_OF:
    case SXML_IR_EXPRESSION_KIND_TREAT:
        add_edge(sink, node_of_atom(module, expression->type_test.value));
        return;
    case SXML_IR_EXPRESSION_KIND_COERCE:
        add_edge(sink, node_of_atom(module, expression->coercion.value));
        return;
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_SHALLOW_COPY:
        add_edge(sink, node_of_atom(module, expression->node_construction.name));
        add_edge(sink, node_of_atom(module, expression->node_construction.value));
        return;
    case SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES:
        add_edge(sink, node_of_atom(module, expression->apply_templates.select));
        return;
    case SXML_IR_EXPRESSION_KIND_STREAM:
        add_edge(sink, node_of_atom(module, expression->stream.origin));
        return;
    case SXML_IR_EXPRESSION_KIND_MERGE:
        /* XSLT only (packet 7.6g): no XQuery global reads its streams. */
        add_edge(sink, node_of_atom(module, expression->merge.tuples));
        return;
    case SXML_IR_EXPRESSION_KIND_GLOBAL_READ:
        /* Added by the global-read pass, after this scan (D234). */
        add_edge(sink, node_of_atom(module, (sxml_ir_atom){
                                                .atom_kind = SXML_IR_ATOM_KIND_GLOBAL_VARIABLE,
                                                .global_variable = expression->global_read.global}));
        return;
    case SXML_IR_EXPRESSION_KIND_NONE:
    case SXML_IR_EXPRESSION_KIND_LET:
    case SXML_IR_EXPRESSION_KIND_SEQUENCE:
    case SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION:
    case SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_RAISE:
    case SXML_IR_EXPRESSION_KIND_TRY_CATCH:
    case SXML_IR_EXPRESSION_KIND_COUNT:
        return;
    }
}

/* Every edge out of region. */
static void scan_region(edge_sink *sink, const lowered_region *region) {
    const sxml_ir_unit *unit = sink->module->unit;
    for (uint32_t expression_index = region->first_expression;
         expression_index < region->end_expression; expression_index += 1) {
        scan_expression(sink, sxml_ir_unit_get_expression(
                                  unit, (sxml_ir_expression_index){.value = expression_index}));
    }
    for (uint32_t atom_index = region->first_atom; atom_index < region->end_atom;
         atom_index += 1) {
        add_edge(sink, node_of_atom(sink->module, unit->atoms[atom_index]));
    }
    for (uint32_t entry_index = region->first_map_entry; entry_index < region->end_map_entry;
         entry_index += 1) {
        add_edge(sink, node_of_atom(sink->module, unit->map_entries[entry_index].key));
        add_edge(sink, node_of_atom(sink->module, unit->map_entries[entry_index].value));
    }
}

/* The graph as adjacency lists: node n's targets are
   targets[first_edges[n] .. first_edges[n + 1]). */
typedef struct dependency_graph {
    size_t *first_edges;
    uint32_t *targets;
} dependency_graph;

/* The walk's state: per node its color (0 not seen, 1 on the path, 2
   done), and the path with each node's next edge. */
typedef struct dependency_walk {
    uint8_t *colors;
    uint32_t *path_nodes;
    size_t *path_edges;
    /* The variables in computation order. */
    uint32_t *order;
    size_t order_count;
} dependency_walk;


/* Walks from the query body, ordering the variables it reaches
   dependencies first; a cycle is left to the evaluator (5.16). */
static sxml_status walk_dependencies(module_lowering *module, const dependency_graph *graph,
                                     dependency_walk *walk) {
    size_t path_count = 1;
    walk->path_nodes[0] = 0;
    walk->path_edges[0] = graph->first_edges[0];
    walk->colors[0] = 1;
    while (path_count > 0) {
        const size_t top = walk->path_nodes[path_count - 1];
        if (walk->path_edges[path_count - 1] == graph->first_edges[top + 1]) {
            walk->colors[top] = 2;
            path_count -= 1;
            if (is_variable_node(module, top)) {
                walk->order[walk->order_count] = (uint32_t)top;
                walk->order_count += 1;
            }
            continue;
        }
        const size_t target = graph->targets[walk->path_edges[path_count - 1]];
        walk->path_edges[path_count - 1] += 1;
        /* A variable read in its own initializer is a cycle only if the
           read runs while the variable is computed (5.16): a closure that
           reads its own variable is not one (D135 (c)), and a read that
           does run is BEING_COMPUTED, XQDY0054, in the evaluator. */
        if (target == top) {
            continue;
        }
        if (walk->colors[target] == 0) {
            walk->colors[target] = 1;
            walk->path_nodes[path_count] = (uint32_t)target;
            walk->path_edges[path_count] = graph->first_edges[target];
            path_count += 1;
            continue;
        }
        /* A cycle (colors[target] == 1) is a static superset of what runs:
           a variable depends on another only if the read happens while it
           is computed (5.16), which the evaluator finds (XQDY0054 for a
           global BEING_COMPUTED, src/evaluator_global.c). The walk goes
           on. */
    }
    return SXML_STATUS_OK;
}

/* Scratch memory of element_count elements, released by the caller. */
static sxml_status allocate_zeroed(module_lowering *module,
                                   sxml_xpath_lowering_scratch_request request, void **memory) {
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(module->lowerer, request, memory);
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, (request.element_count == 0 ? 1 : request.element_count) *
                               request.element_size);
    }
    return status;
}

/* The computation order of the variables the query body reaches. */
typedef struct computation_order {
    uint32_t *nodes;
    size_t node_count;
} computation_order;

static sxml_status order_variables(module_lowering *module, computation_order *order) {
    const size_t node_count = module->node_count;
    edge_sink sink = {.module = module, .targets = NULL, .edge_count = 0};
    dependency_graph graph = {0};
    sxml_status status = allocate_zeroed(
        module,
        (sxml_xpath_lowering_scratch_request){.element_count = node_count + 1,
                                              .element_size = sizeof(size_t),
                                              .element_alignment = alignof(size_t)},
        (void **)&graph.first_edges);
    for (size_t node_index = 0; status == SXML_STATUS_OK && node_index < node_count;
         node_index += 1) {
        graph.first_edges[node_index] = sink.edge_count;
        scan_region(&sink, &module->regions[node_index]);
    }
    if (status == SXML_STATUS_OK) {
        graph.first_edges[node_count] = sink.edge_count;
        status = allocate_zeroed(
            module,
            (sxml_xpath_lowering_scratch_request){.element_count = sink.edge_count,
                                                  .element_size = sizeof(uint32_t),
                                                  .element_alignment = alignof(uint32_t)},
            (void **)&graph.targets);
    }
    if (status == SXML_STATUS_OK) {
        sink = (edge_sink){.module = module, .targets = graph.targets, .edge_count = 0};
        for (size_t node_index = 0; node_index < node_count; node_index += 1) {
            scan_region(&sink, &module->regions[node_index]);
        }
    }
    dependency_walk walk = {0};
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            module,
            (sxml_xpath_lowering_scratch_request){.element_count = node_count,
                                                  .element_size = sizeof(uint8_t),
                                                  .element_alignment = 1},
            (void **)&walk.colors);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            module,
            (sxml_xpath_lowering_scratch_request){.element_count = node_count,
                                                  .element_size = sizeof(size_t),
                                                  .element_alignment = alignof(size_t)},
            (void **)&walk.path_edges);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            module,
            (sxml_xpath_lowering_scratch_request){.element_count = node_count,
                                                  .element_size = sizeof(uint32_t),
                                                  .element_alignment = alignof(uint32_t)},
            (void **)&walk.path_nodes);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            module,
            (sxml_xpath_lowering_scratch_request){.element_count = node_count,
                                                  .element_size = sizeof(uint32_t),
                                                  .element_alignment = alignof(uint32_t)},
            (void **)&walk.order);
    }
    if (status == SXML_STATUS_OK) {
        status = walk_dependencies(module, &graph, &walk);
    }
    sxml_xpath_lowerer_release_scratch(module->lowerer, graph.first_edges);
    sxml_xpath_lowerer_release_scratch(module->lowerer, graph.targets);
    sxml_xpath_lowerer_release_scratch(module->lowerer, walk.colors);
    sxml_xpath_lowerer_release_scratch(module->lowerer, walk.path_edges);
    sxml_xpath_lowerer_release_scratch(module->lowerer, walk.path_nodes);
    if (status != SXML_STATUS_OK) {
        sxml_xpath_lowerer_release_scratch(module->lowerer, walk.order);
        return status;
    }
    *order = (computation_order){.nodes = walk.order, .node_count = walk.order_count};
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Globals */

/* An IR function of arity 0 that calls initializer: added in computation
   order, so the global part computes by index (src/evaluator_global.c). */
static sxml_status add_ordered_initializer(module_lowering *module,
                                           sxml_ir_function_index initializer,
                                           sxml_ir_function_index *ordered) {
    const sxml_ir_function *lowered = sxml_ir_unit_get_function(module->unit, initializer);
    sxml_ir_expression_index call = {0};
    sxml_status status = sxml_ir_unit_add_expression(
        module->unit,
        &(sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
            .source_span = lowered->source_span,
            .static_call = {.target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                       .function = initializer},
                            .arguments = {.first_atom_index = 0, .atom_count = 0}}},
        &call);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_ir_unit_add_function(
        module->unit,
        &(sxml_ir_function){.signature = lowered->signature,
                            .body = call,
                            .source_span = lowered->source_span},
        ordered);
}

/* The unit's global variables: the host's, the prolog's, the context
   global; reached variables with the initializer functions of their
   computation order. */
static sxml_status add_globals(module_lowering *module) {
    computation_order order = {0};
    sxml_status status = order_variables(module, &order);
    sxml_ir_function_index *ordered = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            module,
            (sxml_xpath_lowering_scratch_request){.element_count = module->node_count,
                                                  .element_size = sizeof(sxml_ir_function_index),
                                                  .element_alignment =
                                                      alignof(sxml_ir_function_index)},
            (void **)&ordered);
    }
    for (size_t order_index = 0; status == SXML_STATUS_OK && order_index < order.node_count;
         order_index += 1) {
        const uint32_t node = order.nodes[order_index];
        if (module->initializers[node].value != 0) {
            status = add_ordered_initializer(module, module->initializers[node], &ordered[node]);
        }
    }
    /* In global number order: the host's (the main module's first
       declarations), then each module's prolog variables, whose numbers
       follow the reading order (D148). */
    const sxml_xquery_static_context *main_context = &module->contexts[0];
    for (size_t host_index = 0;
         status == SXML_STATUS_OK && host_index < main_context->host_variable_count;
         host_index += 1) {
        const sxml_variable_declaration *declaration =
            &main_context->variable_declarations[host_index];
        sxml_ir_global_index global = {0};
        status = sxml_ir_unit_add_global_variable(
            module->unit,
            &(sxml_ir_global_variable){.variable_name = declaration->variable_name,
                                       .declared_type = declaration->declared_type,
                                       .initializer = {.value = 0}},
            &global);
    }
    for (size_t order_index = 0; status == SXML_STATUS_OK && order_index < module->set->module_count;
         order_index += 1) {
        const sxml_xquery_static_context *context =
            &module->contexts[module->set->reading_order[order_index]];
        for (size_t variable_index = 0;
             status == SXML_STATUS_OK && variable_index < context->prolog_variable_count;
             variable_index += 1) {
            const size_t position = context->host_variable_count + variable_index;
            const sxml_variable_declaration *declaration = &context->variable_declarations[position];
            sxml_ir_global_index global = {0};
            status = sxml_ir_unit_add_global_variable(
                module->unit,
                &(sxml_ir_global_variable){
                    .variable_name = declaration->variable_name,
                    .declared_type = declaration->declared_type,
                    .initializer =
                        ordered[context->global_numbers[position] - module->host_variable_count]},
                &global);
        }
    }
    /* The context global: the main module's context value declaration
       (item()* without one), whose type the context value is coerced to.
       Then one more global of name 0, without initializer, per library
       module that declares the context value: the value, after that
       coercion, must match each one's type without further coercion
       (5.17, XPTY0004; the evaluator checks them when it binds the
       context global, fn:load-xquery-module when it loads, F&O 4.0,
       18.2). */
    if (status == SXML_STATUS_OK && module->has_context_global) {
        sxml_ir_global_index global = {0};
        status = sxml_ir_unit_add_global_variable(
            module->unit,
            &(sxml_ir_global_variable){.variable_name = {.index = 0},
                                       .declared_type = main_context->context_value_type,
                                       .initializer = ordered[context_node(module)]},
            &global);
    }
    for (size_t module_index = 1; status == SXML_STATUS_OK && module->has_context_global &&
                                  module_index < module->set->module_count;
         module_index += 1) {
        const sxml_xquery_static_context *library = &module->contexts[module_index];
        if (library->context_value_declaration.value == 0) {
            continue;
        }
        sxml_ir_global_index global = {0};
        status = sxml_ir_unit_add_global_variable(
            module->unit,
            &(sxml_ir_global_variable){.variable_name = {.index = 0},
                                       .declared_type = library->context_value_type,
                                       .initializer = {.value = 0}},
            &global);
    }
    sxml_xpath_lowerer_release_scratch(module->lowerer, ordered);
    sxml_xpath_lowerer_release_scratch(module->lowerer, order.nodes);
    return status;
}

/* ------------------------------------------------------------- Module */

/* True when some prolog makes initializers or states the context value's
   type: a variable with a value or a default, or a context value
   declaration. */
static bool needs_context_global(const module_lowering *module) {
    for (size_t module_index = 0; module_index < module->set->module_count; module_index += 1) {
        const sxml_xquery_static_context *context = &module->contexts[module_index];
        if (context->context_value_declaration.value != 0) {
            return true;
        }
        for (size_t variable_index = 0; variable_index < context->prolog_variable_count;
             variable_index += 1) {
            if (initializing_expression(context->ast, context->variable_nodes[variable_index])
                    .value != 0) {
                return true;
            }
        }
    }
    return false;
}

/* What lower_library_modules lowers: the default functions, which come
   first in every module so that calls know which focus parts they read,
   or the other declared functions and the initializers. */
typedef enum library_phase {
    SXML_LIBRARY_PHASE_DEFAULT_FUNCTIONS,
    SXML_LIBRARY_PHASE_FUNCTIONS_AND_INITIALIZERS,
} library_phase;

/* Marks the functions of the unit from first_function on, and the
   declared functions of the current module, as the module's (its static
   base URI and source name, glossary `ir_module`). */
typedef struct module_functions {
    uint32_t module_number;
    uint32_t first_function;
} module_functions;

static sxml_status mark_module_function(module_lowering *module, uint32_t module_number,
                                        sxml_ir_function_index index) {
    sxml_ir_function_replacement replacement = {
        .index = index, .function = *sxml_ir_unit_get_function(module->unit, index)};
    replacement.function.module_number = module_number;
    return sxml_ir_unit_replace_function(module->unit, &replacement);
}

static sxml_status mark_module_functions(module_lowering *module, module_functions request) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t function_number = request.first_function;
         status == SXML_STATUS_OK && function_number < module->unit->function_count;
         function_number += 1) {
        status = mark_module_function(module, request.module_number,
                                      (sxml_ir_function_index){.value = function_number});
    }
    const sxml_xquery_static_context *context = module->context;
    for (size_t function_index = 0;
         status == SXML_STATUS_OK && function_index < context->declared_function_count;
         function_index += 1) {
        const sxml_xquery_declared_function *declared = &context->declared_functions[function_index];
        const uint32_t ir_function_count =
            declared->parameter_count - declared->required_count + 1U +
            declared->default_function_count;
        for (uint32_t offset = 0; status == SXML_STATUS_OK && offset < ir_function_count;
             offset += 1) {
            status = mark_module_function(
                module, request.module_number,
                (sxml_ir_function_index){.value = declared->first_function.value + offset});
        }
    }
    return status;
}

/* Lowers phase of every library module, each with a lowerer over its own
   static context and AST. */
static sxml_status lower_library_modules(module_lowering *module,
                                         const sxml_xquery_lowering_request *request,
                                         library_phase phase) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t module_index = 1; status == SXML_STATUS_OK && module_index < module->set->module_count;
         module_index += 1) {
        /* A plain struct on the stack is memory of the lowerer's size and
           alignment. */
        sxml_xpath_lowerer storage;
        sxml_xpath_lowerer *lowerer = NULL;
        sxml_xquery_static_context *context = &module->contexts[module_index];
        status = sxml_xpath_lowerer_initialize(
            (sxml_memory_view){.bytes = (uint8_t *)&storage, .byte_count = sizeof(storage)},
            &(sxml_xpath_lowerer_configuration){
                .static_context = &context->xpath,
                .unit = module->unit,
                .language = SXML_LANGUAGE_XQUERY,
                .parts = *sxml_xpath_lowering_xquery_parts(),
                .maximum_expression_depth = request->maximum_expression_depth,
                .error_record = request->error_record},
            &lowerer);
        if (status != SXML_STATUS_OK) {
            break;
        }
        lowerer->ast = context->ast;
        module->lowerer = lowerer;
        module->ast = context->ast;
        module->context = context;
        const uint32_t first_function = module->unit->function_count;
        if (phase == SXML_LIBRARY_PHASE_DEFAULT_FUNCTIONS) {
            /* The module's static base URI and source name, module number
               module_index (D148). */
            uint32_t module_number = 0;
            status = sxml_ir_unit_add_module(
                module->unit,
                &(sxml_ir_module){.source_name = module->set->modules[module_index].module_uri,
                                  .static_base_uri = context->xpath.static_base_uri},
                &module_number);
            if (status == SXML_STATUS_OK && module_number != module_index) {
                status = SXML_STATUS_INVALID_ARGUMENT;
            }
            if (status == SXML_STATUS_OK) {
                status = lower_default_functions(module);
            }
        } else {
            status = lower_declared_functions(module);
            if (status == SXML_STATUS_OK) {
                status = lower_initializers(module);
            }
        }
        if (status == SXML_STATUS_OK) {
            status = mark_module_functions(
                module, (module_functions){.module_number = (uint32_t)module_index,
                                           .first_function = first_function});
        }
        sxml_xpath_lowerer_finalize(lowerer);
    }
    module->lowerer = module->main_lowerer;
    module->ast = module->contexts[0].ast;
    module->context = &module->contexts[0];
    return status;
}

/* Lowers everything into module->unit, whose function 1 is the main
   function (see the file comment). */
static sxml_status lower_module(module_lowering *module,
                                const sxml_xquery_lowering_request *request,
                                sxml_ast_node_index body) {
    const sxml_xquery_static_context *context = &module->contexts[0];
    module->host_variable_count = context->host_variable_count;
    for (size_t module_index = 0; module_index < module->set->module_count; module_index += 1) {
        const sxml_xquery_static_context *each = &module->contexts[module_index];
        module->prolog_variable_count += each->prolog_variable_count;
        for (size_t function_index = 0; function_index < each->declared_function_count;
             function_index += 1) {
            const sxml_xquery_declared_function *function = &each->declared_functions[function_index];
            module->declared_ir_function_count +=
                (size_t)(function->parameter_count - function->required_count) + 1 +
                function->default_function_count;
        }
    }
    module->node_count = module->prolog_variable_count + 2 + module->declared_ir_function_count;
    module->has_context_global = needs_context_global(module);
    module->context_global_number =
        (uint32_t)(module->host_variable_count + module->prolog_variable_count) + 1;
    sxml_status status = allocate_zeroed(
        module,
        (sxml_xpath_lowering_scratch_request){.element_count = module->node_count,
                                              .element_size = sizeof(lowered_region),
                                              .element_alignment = alignof(lowered_region)},
        (void **)&module->regions);
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            module,
            (sxml_xpath_lowering_scratch_request){.element_count = module->node_count,
                                                  .element_size = sizeof(sxml_ir_function_index),
                                                  .element_alignment =
                                                      alignof(sxml_ir_function_index)},
            (void **)&module->initializers);
    }
    if (status == SXML_STATUS_OK) {
        status = lower_library_modules(module, request, SXML_LIBRARY_PHASE_DEFAULT_FUNCTIONS);
    }
    if (status == SXML_STATUS_OK) {
        status = lower_default_functions(module);
    }
    if (status == SXML_STATUS_OK) {
        status = close_default_focus(module);
    }
    if (status == SXML_STATUS_OK) {
        status =
            lower_library_modules(module, request, SXML_LIBRARY_PHASE_FUNCTIONS_AND_INITIALIZERS);
    }
    if (status == SXML_STATUS_OK) {
        status = lower_declared_functions(module);
    }
    if (status == SXML_STATUS_OK) {
        status = lower_initializers(module);
    }
    if (status == SXML_STATUS_OK) {
        /* 5.17: with a context value declaration that is not external, the
           host's context value is not used. */
        const bool takes_host_value =
            module->has_context_global &&
            (context->context_value_declaration.value == 0 ||
             (node_of(module->ast, context->context_value_declaration)->flags &
              SXML_AST_FLAG_EXTERNAL) != 0);
        module->regions[0] = begin_region(module->unit);
        status = lower_body(module, &(body_request){.function = {.value = 1},
                                                   .node = body,
                                                   .focus = fixed_focus(module),
                                                   .parameters = {.declaration = {0},
                                                                  .parameter_count = 0},
                                                   .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                                                                   .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
                                                   .role = SXML_IR_COERCION_ROLE_VARIABLE,
                                                   .reserves_context_slot = takes_host_value});
        end_region(module->unit, &module->regions[0]);
    }
    if (status == SXML_STATUS_OK) {
        status = add_globals(module);
    }
    sxml_xpath_lowerer_release_scratch(module->lowerer, module->regions);
    sxml_xpath_lowerer_release_scratch(module->lowerer, module->initializers);
    return status;
}

/* ===================================================== Entry points */

/* The body of a main module; INVALID_ARGUMENT for a library module,
   which is compiled only as an import. */
static sxml_status find_main_body(const sxml_ast *ast, sxml_ast_node_index *body) {
    const sxml_ast_node_index module = child_of(ast, ast->root, SXML_AST_FIELD_MODULE);
    if (node_of(ast, module)->kind != SXML_AST_KIND_MAIN_MODULE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *body = child_of(ast, module, SXML_AST_FIELD_BODY);
    return SXML_STATUS_OK;
}

/* Adds function 1, the main function, with no body yet. */
static sxml_status add_main_function(const sxml_xpath_static_context *context,
                                     sxml_ir_unit *unit, const sxml_ast *ast) {
    sxml_type_identifier signature = {0};
    sxml_status status = sxml_type_registry_register_item_type(
        sxml_item_heap_get_type_registry(context->item_heap),
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {
                                    .is_any_function = false,
                                    .parameter_types = {.sequence_types = NULL,
                                                        .sequence_type_count = 0},
                                    .result_type = {.item_type = sxml_builtin_type_identifier(
                                                        SXML_TYPE_ITEM),
                                                    .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}}}},
        &signature);
    sxml_ir_function_index main_function = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            unit, &(sxml_ir_function){.signature = signature, .source_span = span_of(ast, ast->root)},
            &main_function);
    }
    return status;
}

/* What lowering a module set works with: the request, the set, a
   static context per module, the main module's body. */
typedef struct lowering_run {
    const sxml_xquery_lowering_request *request;
    const sxml_xquery_module_set *set;
    sxml_xquery_static_context *contexts;
    sxml_ast_node_index body;
} lowering_run;

static sxml_status run_lowering(const lowering_run *run, sxml_ir_unit *unit) {
    sxml_xquery_static_context *main_context = &run->contexts[0];
    /* A plain struct on the stack is memory of the lowerer's size and
       alignment. */
    sxml_xpath_lowerer storage;
    sxml_xpath_lowerer *lowerer = NULL;
    sxml_status status = sxml_xpath_lowerer_initialize(
        (sxml_memory_view){.bytes = (uint8_t *)&storage, .byte_count = sizeof(storage)},
        &(sxml_xpath_lowerer_configuration){
            .static_context = &main_context->xpath,
            .unit = unit,
            .language = SXML_LANGUAGE_XQUERY,
            .parts = *sxml_xpath_lowering_xquery_parts(),
            .maximum_expression_depth = run->request->maximum_expression_depth,
            .error_record = run->request->error_record},
        &lowerer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    lowerer->ast = run->request->ast;
    module_lowering module = {.lowerer = lowerer,
                              .ast = run->request->ast,
                              .context = main_context,
                              .main_lowerer = lowerer,
                              .unit = unit,
                              .error_record = run->request->error_record,
                              .set = run->set,
                              .contexts = run->contexts};
    status = lower_module(&module, run->request, run->body);
    sxml_xpath_lowerer_finalize(lowerer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* For casts to xs:QName at run time (D84). */
    sxml_xpath_namespace_collection namespaces = {.bindings = NULL, .binding_count = 0};
    status = sxml_xpath_collect_namespaces(run->request->ast, &main_context->xpath, &namespaces);
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_set_static_namespaces(
            unit, (sxml_namespace_binding_view){.namespace_bindings = namespaces.bindings,
                                                .namespace_binding_count =
                                                    namespaces.binding_count});
        sxml_allocator_release(main_context->xpath.allocator, namespaces.bindings);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_set_static_base_uri(unit, main_context->xpath.static_base_uri);
    }
    if (status == SXML_STATUS_OK) {
        /* Decimal formats and collations, read lexically (D131). */
        status = sxml_ir_unit_set_static_context(unit, &main_context->xpath.unit_static_context);
    }
    return status;
}

/* The static contexts of a module set, one per module (by module index),
   the hosts library modules start from, and the imported contexts each
   module sees. Allocated with the host's allocator. */
typedef struct module_contexts {
    sxml_xquery_static_context *contexts;
    sxml_xpath_static_context *hosts;
    const sxml_xquery_static_context **imports;
    /* How many contexts were read, so release knows. */
    bool *is_read;
} module_contexts;

static void release_module_contexts(const sxml_xpath_static_context *host,
                                    const sxml_xquery_module_set *set, module_contexts *contexts) {
    for (size_t module_index = 0; contexts->is_read != NULL && module_index < set->module_count;
         module_index += 1) {
        if (contexts->is_read[module_index]) {
            sxml_xquery_release_static_context(&contexts->contexts[module_index]);
        }
    }
    sxml_allocator_release(host->allocator, contexts->contexts);
    sxml_allocator_release(host->allocator, contexts->hosts);
    sxml_allocator_release(host->allocator, (void *)contexts->imports);
    sxml_allocator_release(host->allocator, contexts->is_read);
}

/* Zeroed memory for count elements of a size. */
typedef struct context_array {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} context_array;

static sxml_status allocate_context_array(const sxml_xpath_static_context *host,
                                          context_array array, void **memory) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(array.element_count == 0 ? 1 : array.element_count,
                                            array.element_size, &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            host->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = array.element_alignment},
            memory);
    }
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, byte_count);
    }
    return status;
}

/* What reading the module set's static contexts works with. */
typedef struct context_reading {
    const sxml_xquery_lowering_request *request;
    const sxml_xquery_module_set *set;
    sxml_ir_unit *unit;
} context_reading;

/*
 * Reads every module's static context in the reading order, numbering
 * the prolog variables after the host's in that order, then links each
 * to its imports (D148). A library module starts from XQuery's default
 * static context: no host namespaces or variables, its module URI as
 * static base URI, the host's functions and collations.
 */
static sxml_status read_module_contexts(const context_reading *reading,
                                        module_contexts *contexts) {
    const sxml_xpath_static_context *host = reading->request->static_context;
    const sxml_xquery_module_set *set = reading->set;
    sxml_status status = allocate_context_array(
        host,
        (context_array){.element_count = set->module_count,
                        .element_size = sizeof(sxml_xquery_static_context),
                        .element_alignment = alignof(sxml_xquery_static_context)},
        (void **)&contexts->contexts);
    if (status == SXML_STATUS_OK) {
        status = allocate_context_array(
            host,
            (context_array){.element_count = set->module_count,
                            .element_size = sizeof(sxml_xpath_static_context),
                            .element_alignment = alignof(sxml_xpath_static_context)},
            (void **)&contexts->hosts);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_context_array(
            host,
            (context_array){.element_count = set->imported_module_count,
                            .element_size = sizeof(sxml_xquery_static_context *),
                            .element_alignment = alignof(sxml_xquery_static_context *)},
            (void **)&contexts->imports);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_context_array(
            host,
            (context_array){.element_count = set->module_count,
                            .element_size = sizeof(bool),
                            .element_alignment = alignof(bool)},
            (void **)&contexts->is_read);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t import_index = 0; import_index < set->imported_module_count; import_index += 1) {
        contexts->imports[import_index] = &contexts->contexts[set->imported_modules[import_index]];
    }
    uint32_t global_number_base = (uint32_t)host->variable_declarations.variable_declaration_count;
    for (size_t order_index = 0; status == SXML_STATUS_OK && order_index < set->module_count;
         order_index += 1) {
        const size_t module_index = set->reading_order[order_index];
        const sxml_xquery_module *module = &set->modules[module_index];
        sxml_xpath_static_context *module_host = &contexts->hosts[module_index];
        *module_host = *host;
        if (module_index != 0) {
            module_host->namespace_bindings =
                (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                              .namespace_binding_count = 0};
            module_host->variable_declarations =
                (sxml_variable_declaration_view){.variable_declarations = NULL,
                                                 .variable_declaration_count = 0};
            module_host->global_numbers = NULL;
            module_host->static_base_uri = module->module_uri;
            module_host->named_item_types =
                (sxml_xpath_named_item_type_view){.named_item_types = NULL,
                                                  .named_item_type_count = 0};
        }
        status = sxml_xquery_read_static_context(
            &(sxml_xquery_static_request){
                .ast = module->ast,
                .host_static_context = module_host,
                .unit = reading->unit,
                .error_record = reading->request->error_record,
                .target_namespace = module->target_namespace,
                .global_number_base = global_number_base,
                .imported_modules = {.static_contexts =
                                         &contexts->imports[module->first_imported_module],
                                     .static_context_count = module->imported_module_count}},
            &contexts->contexts[module_index]);
        if (status == SXML_STATUS_OK) {
            contexts->is_read[module_index] = true;
            global_number_base += (uint32_t)contexts->contexts[module_index].prolog_variable_count;
        }
    }
    for (size_t module_index = 0; status == SXML_STATUS_OK && module_index < set->module_count;
         module_index += 1) {
        const sxml_xquery_module *module = &set->modules[module_index];
        status = sxml_xquery_link_static_context(
            &(sxml_xquery_link_request){
                .imports = {.static_contexts = &contexts->imports[module->first_imported_module],
                            .static_context_count = module->imported_module_count},
                .error_record = reading->request->error_record},
            &contexts->contexts[module_index]);
    }
    return status;
}

/* The XQuery Update Facility's rules on updating expressions in every
   module (src/xquery_update_internal.h, D187); *body_is_updating for the
   main module's body. */
typedef struct update_rules_check {
    const sxml_xquery_lowering_request *request;
    const module_contexts *contexts;
    size_t module_count;
} update_rules_check;

/* The static rules of Full Text (src/full_text_internal.h, D214) for
   every module of the query. */
static sxml_status check_full_text_rules(update_rules_check check) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t module_index = 0; status == SXML_STATUS_OK && module_index < check.module_count;
         module_index += 1) {
        status = sxml_xquery_check_full_text_rules(&(sxml_xquery_full_text_rules_request){
            .ast = check.contexts->contexts[module_index].ast,
            .processor = check.request->processor != NULL
                             ? check.request->processor
                             : check.request->static_context->processor,
            .error_record = check.request->error_record});
    }
    return status;
}

static sxml_status check_update_rules(update_rules_check check, bool *body_is_updating) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t module_index = 0; status == SXML_STATUS_OK && module_index < check.module_count;
         module_index += 1) {
        bool is_updating = false;
        status = sxml_xquery_check_update_rules(
            &(sxml_xquery_update_rules_request){.context = &check.contexts->contexts[module_index],
                                                .error_record = check.request->error_record},
            &is_updating);
        if (module_index == 0) {
            *body_is_updating = is_updating;
        }
    }
    return status;
}

sxml_status sxml_xquery_lower(const sxml_xquery_lowering_request *request, sxml_ir_unit **unit) {
    if (request == NULL || request->ast == NULL || request->static_context == NULL ||
        sxml_ast_get_node(request->ast, request->ast->root)->kind != SXML_AST_KIND_MODULE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ast_node_index body = {0};
    sxml_status status = find_main_body(request->ast, &body);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xpath_static_context *host = request->static_context;
    sxml_xquery_module_set set = {.allocator = host->allocator};
    status = sxml_xquery_load_modules(
        &(sxml_xquery_module_loading){.main_ast = request->ast,
                                      .item_heap = host->item_heap,
                                      .allocator = host->allocator,
                                      .resource_resolver = request->resource_resolver,
                                      .processor = request->processor,
                                      .static_base_uri = host->static_base_uri,
                                      .maximum_expression_depth = request->maximum_expression_depth,
                                      .error_record = request->error_record},
        &set);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_unit *new_unit = NULL;
    status = sxml_ir_unit_create(
        host->allocator,
        &(sxml_ir_unit_configuration){.allocator = host->allocator,
                                      .item_heap = host->item_heap,
                                      .source_name = request->ast->source_name,
                                      .maximum_expression_count =
                                          request->maximum_expression_count,
                                      .evaluator_parts = sxml_evaluator_xquery_parts()},
        &new_unit);
    if (status != SXML_STATUS_OK) {
        sxml_xquery_release_modules(&set);
        return status;
    }
    status = add_main_function(host, new_unit, request->ast);
    module_contexts contexts = {.contexts = NULL, .hosts = NULL, .imports = NULL, .is_read = NULL};
    if (status == SXML_STATUS_OK) {
        status = read_module_contexts(
            &(context_reading){.request = request, .set = &set, .unit = new_unit}, &contexts);
    }
    bool body_is_updating = false;
    if (status == SXML_STATUS_OK) {
        status = check_full_text_rules((update_rules_check){
            .request = request, .contexts = &contexts, .module_count = set.module_count});
    }
    if (status == SXML_STATUS_OK) {
        status = check_update_rules(
            (update_rules_check){
                .request = request, .contexts = &contexts, .module_count = set.module_count},
            &body_is_updating);
    }
    if (status == SXML_STATUS_OK) {
        status = run_lowering(&(lowering_run){.request = request,
                                              .set = &set,
                                              .contexts = contexts.contexts,
                                              .body = body},
                              new_unit);
    }
    if (status == SXML_STATUS_OK && body_is_updating) {
        /* An updating query (D187): its value is a pending update list. */
        status = sxml_xquery_mark_updating_function(&(sxml_xquery_updating_mark){
            .unit = new_unit, .function_index = {.value = 1}});
    }
    release_module_contexts(host, &set, &contexts);
    sxml_xquery_release_modules(&set);
    if (status != SXML_STATUS_OK) {
        sxml_ir_unit_release(new_unit);
        return status;
    }
    *unit = new_unit;
    return SXML_STATUS_OK;
}

sxml_status sxml_xquery_compile_module(const sxml_xquery_lowering_request *request,
                                       sxml_ir_unit **unit) {
    if (request == NULL || request->static_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* XQuery's own functions after the host's (fn:put,
       src/xquery_load_module_internal.h, D154). */
    sxml_xquery_function_table functions = {.host_table = {.state = NULL, .operations = NULL},
                                            .library = NULL};
    sxml_status status =
        sxml_xquery_function_table_initialize(&functions, request->static_context);
    sxml_xpath_static_context static_context = *request->static_context;
    static_context.function_table = sxml_xquery_function_table_as_table(&functions);
    sxml_xquery_lowering_request lowering = *request;
    lowering.static_context = &static_context;
    sxml_ir_unit *new_unit = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_lower(&lowering, &new_unit);
    }
    sxml_xquery_function_table_finalize(&functions);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_diagnostic diagnostic = {0};
    status = sxml_ir_run_passes(&(sxml_ir_pass_list_request){
        .pass_request = {.unit = new_unit,
                         .type_registry =
                             sxml_item_heap_get_type_registry(request->static_context->item_heap),
                         .error_record = request->error_record,
                         .diagnostic = &diagnostic},
        .passes = sxml_ir_standard_passes,
        .verifies_after_each_pass = false});
    if (status != SXML_STATUS_OK) {
        if (status == SXML_STATUS_INVALID_ARGUMENT && request->error_record != NULL) {
            request->error_record->diagnostic = diagnostic;
        }
        sxml_ir_unit_release(new_unit);
        return status;
    }
    sxml_ir_unit_seal(new_unit);
    *unit = new_unit;
    return SXML_STATUS_OK;
}

sxml_status sxml_xquery_compile(const sxml_xpath_compile_request *request, sxml_ir_unit **unit) {
    if (request == NULL || request->static_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ast *ast = NULL;
    sxml_status status = sxml_xquery_build_ast(
        &(sxml_ast_build_request){.expression_text = request->expression_text,
                                  .source_name = request->source_name,
                                  .allocator = request->static_context->allocator,
                                  .maximum_expression_depth = request->maximum_expression_depth,
                                  .error_record = request->error_record},
        &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_xquery_compile_module(
        &(sxml_xquery_lowering_request){.ast = ast,
                                        .static_context = request->static_context,
                                        .maximum_expression_count =
                                            request->maximum_expression_count,
                                        .maximum_expression_depth =
                                            request->maximum_expression_depth,
                                        .error_record = request->error_record,
                                        .resource_resolver = {.state = NULL, .operations = NULL},
                                        .processor = NULL},
        unit);
    sxml_ast_destroy(ast);
    return status;
}
