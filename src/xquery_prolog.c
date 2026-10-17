/*
 * Lowering an XQuery main module: its prolog's declared functions and
 * variables and its query body (src/xquery_lower_internal.h; D55, D101,
 * D135; packet 3.10), and the internal compile entry.
 *
 * Golden file: src/xquery_lower.c (its former entry points, which this
 * file now holds).
 *
 * The static context comes from src/xquery_static.c; every expression of
 * the module is lowered through the embedding entry (D103) with it, so the
 * core and the XQuery parts lower bodies and initializers as they lower
 * the query body. The unit holds:
 *
 * - function 1, the main function: the query body;
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
 * body orders the variables it reaches dependencies first; a cycle
 * through a variable is XQDY0054, raised statically (5.16: "If it is
 * discovered, during static analysis or during dynamic evaluation, that
 * such a cycle exists"). Each reached variable's global then gets, in
 * that order, an initializer function that calls its lowered initializer,
 * so the global part computes them by initializer index
 * (src/evaluator_global.c); variables the body never reaches get none and
 * are never computed. The walk keeps its path on an explicit stack (D71).
 *
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026)
 * unless another specification is named.
 */
#include "xquery_lower_internal.h"

#include "ir_pass_internal.h"
#include "sxml/memory.h"
#include "xpath_lower_internal.h"
#include "xquery_static_internal.h"

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

static sxml_ir_atom slot_atom(uint32_t slot_number) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT,
                          .slot = {.value = slot_number}};
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
    sxml_xpath_lowerer *lowerer;
    const sxml_ast *ast;
    sxml_xquery_static_context *context;
    sxml_ir_unit *unit;
    sxml_error_record *error_record;
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
    for (sxml_ast_node_index parameter =
             child_of(module->ast, scope.declaration, SXML_AST_FIELD_PARAMETER);
         status == SXML_STATUS_OK && parameter.value != 0 &&
         parameter_number <= scope.parameter_count;
         parameter = sxml_ast_find_next_with_same_field(module->ast, parameter)) {
        const sxml_ast_node_index variable =
            child_of(module->ast, parameter, SXML_AST_FIELD_VARIABLE);
        sxml_name_identifier name = {0};
        status = sxml_xpath_lowerer_resolve_variable_name(
            state, child_of(module->ast, variable, SXML_AST_FIELD_NAME), &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_push_variable(
                state, (sxml_xpath_lowering_scope_entry){.variable_name = name,
                                                         .function_level = 0,
                                                         .atom = slot_atom(parameter_number)});
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
    }
    return sxml_ir_unit_replace_function(module->unit, &replacement);
}

/* --------------------------------------------------- Declared functions */

/* The IR function of a shorter arity (5.18.3): the defaults of the
   parameters from arity on, each coerced to its parameter's type, and a
   call of the full arity. Defaults see no parameters and no focus but
   ":= .", which the core supplies at the call. */
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
    sxml_xpath_lowering_function_frame frame = {
        .function = {.function_index = index,
                     .function_level = 0,
                     .slot_count = request.arity,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT},
    };
    sxml_xpath_lowerer_enter_function(state, &frame);
    sxml_xpath_lowering_block block = {0};
    uint32_t parameter_number = 0;
    for (sxml_ast_node_index parameter =
             child_of(module->ast, function->declaration, SXML_AST_FIELD_PARAMETER);
         status == SXML_STATUS_OK && parameter.value != 0;
         parameter = sxml_ast_find_next_with_same_field(module->ast, parameter)) {
        parameter_number += 1;
        if (parameter_number <= request.arity) {
            arguments[parameter_number - 1] = slot_atom(parameter_number);
            continue;
        }
        const sxml_ast_node_index default_value =
            child_of(module->ast, parameter, SXML_AST_FIELD_DEFAULT);
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
    return sxml_ir_unit_replace_function(module->unit, &replacement);
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
        for (uint32_t arity = function->required_count;
             status == SXML_STATUS_OK && arity <= function->parameter_count; arity += 1) {
            const sxml_ir_function_index index = {
                .value = function->first_function.value + (arity - function->required_count)};
            lowered_region *region = &module->regions[function_node(module, index)];
            *region = begin_region(module->unit);
            if (arity < function->parameter_count) {
                status = lower_defaults(module,
                                        (default_request){.function = function, .arity = arity});
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

static sxml_status lower_initializers(module_lowering *module) {
    const sxml_xquery_static_context *context = module->context;
    sxml_status status = SXML_STATUS_OK;
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && variable_index < module->prolog_variable_count;
         variable_index += 1) {
        status = lower_initializer(
            module,
            &(initializer_request){
                .node = variable_index + 1,
                .declaration = context->variable_nodes[variable_index],
                .declared_type =
                    context->variable_declarations[context->host_variable_count + variable_index]
                        .declared_type,
                .focus = fixed_focus(module)});
    }
    if (status == SXML_STATUS_OK && context->context_value_declaration.value != 0) {
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
    const size_t host_count = module->context->host_variable_count;
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
   done), its place on the path, and the path with each node's next edge. */
typedef struct dependency_walk {
    uint8_t *colors;
    size_t *path_positions;
    uint32_t *path_nodes;
    size_t *path_edges;
    /* The variables in computation order. */
    uint32_t *order;
    size_t order_count;
} dependency_walk;

/* The declaration a variable node stands for. */
static sxml_ast_node_index declaration_of_node(const module_lowering *module, size_t node) {
    if (node == context_node(module)) {
        return module->context->context_value_declaration;
    }
    return module->context->variable_nodes[node - 1];
}

/* Walks from the query body; XQDY0054 for a cycle through a variable. */
static sxml_status walk_dependencies(module_lowering *module, const dependency_graph *graph,
                                     dependency_walk *walk) {
    size_t path_count = 1;
    walk->path_nodes[0] = 0;
    walk->path_edges[0] = graph->first_edges[0];
    walk->path_positions[0] = 0;
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
        if (walk->colors[target] == 0) {
            walk->colors[target] = 1;
            walk->path_positions[target] = path_count;
            walk->path_nodes[path_count] = (uint32_t)target;
            walk->path_edges[path_count] = graph->first_edges[target];
            path_count += 1;
            continue;
        }
        if (walk->colors[target] != 1) {
            continue;
        }
        /* A cycle: the path from target to the top. */
        for (size_t position = walk->path_positions[target]; position < path_count;
             position += 1) {
            if (is_variable_node(module, walk->path_nodes[position])) {
                return sxml_xpath_lowerer_report_static_error(
                    module->lowerer, declaration_of_node(module, walk->path_nodes[position]),
                    SXML_BYTE_VIEW_FROM_LITERAL(
                        "XQDY0054 the value of a variable depends on itself"));
            }
        }
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
            (void **)&walk.path_positions);
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
    sxml_xpath_lowerer_release_scratch(module->lowerer, walk.path_positions);
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
    const sxml_xquery_static_context *context = module->context;
    for (size_t declaration_index = 0;
         status == SXML_STATUS_OK && declaration_index < context->variable_declaration_count;
         declaration_index += 1) {
        const sxml_variable_declaration *declaration =
            &context->variable_declarations[declaration_index];
        const sxml_ir_function_index initializer =
            declaration_index < context->host_variable_count
                ? (sxml_ir_function_index){.value = 0}
                : ordered[declaration_index - context->host_variable_count + 1];
        sxml_ir_global_index global = {0};
        status = sxml_ir_unit_add_global_variable(
            module->unit,
            &(sxml_ir_global_variable){.variable_name = declaration->variable_name,
                                       .declared_type = declaration->declared_type,
                                       .initializer = initializer},
            &global);
    }
    if (status == SXML_STATUS_OK && module->has_context_global) {
        sxml_ir_global_index global = {0};
        status = sxml_ir_unit_add_global_variable(
            module->unit,
            &(sxml_ir_global_variable){.variable_name = {.index = 0},
                                       .declared_type = context->context_value_type,
                                       .initializer = ordered[context_node(module)]},
            &global);
    }
    sxml_xpath_lowerer_release_scratch(module->lowerer, ordered);
    sxml_xpath_lowerer_release_scratch(module->lowerer, order.nodes);
    return status;
}

/* ------------------------------------------------------------- Module */

/* True when the prolog makes initializers: a variable with a value or a
   default, or a context value declaration. */
static bool needs_context_global(const sxml_xquery_static_context *context) {
    if (context->context_value_declaration.value != 0) {
        return true;
    }
    for (size_t variable_index = 0;
         variable_index < context->variable_declaration_count - context->host_variable_count;
         variable_index += 1) {
        if (initializing_expression(context->ast, context->variable_nodes[variable_index]).value !=
            0) {
            return true;
        }
    }
    return false;
}

/* Lowers everything into module->unit, whose function 1 is the main
   function (see the file comment). */
static sxml_status lower_module(module_lowering *module, sxml_ast_node_index body) {
    const sxml_xquery_static_context *context = module->context;
    module->prolog_variable_count =
        context->variable_declaration_count - context->host_variable_count;
    for (size_t function_index = 0; function_index < context->declared_function_count;
         function_index += 1) {
        const sxml_xquery_declared_function *function =
            &context->declared_functions[function_index];
        module->declared_ir_function_count +=
            (size_t)(function->parameter_count - function->required_count) + 1;
    }
    module->node_count = module->prolog_variable_count + 2 + module->declared_ir_function_count;
    module->has_context_global = needs_context_global(context);
    module->context_global_number = (uint32_t)context->variable_declaration_count + 1;
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

/* The body of a main module; INVALID_ARGUMENT for a library module
   (packet 3.11). */
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

/* Lowers the module into new_unit with a lowerer over context. */
typedef struct lowering_run {
    const sxml_xquery_lowering_request *request;
    sxml_xquery_static_context *context;
    sxml_ast_node_index body;
} lowering_run;

static sxml_status run_lowering(const lowering_run *run, sxml_ir_unit *unit) {
    /* A plain struct on the stack is memory of the lowerer's size and
       alignment. */
    sxml_xpath_lowerer storage;
    sxml_xpath_lowerer *lowerer = NULL;
    sxml_status status = sxml_xpath_lowerer_initialize(
        (sxml_memory_view){.bytes = (uint8_t *)&storage, .byte_count = sizeof(storage)},
        &(sxml_xpath_lowerer_configuration){
            .static_context = &run->context->xpath,
            .unit = unit,
            .language = SXML_LANGUAGE_XQUERY,
            .parts = {.parts = NULL, .part_count = 0},
            .maximum_expression_depth = run->request->maximum_expression_depth,
            .error_record = run->request->error_record},
        &lowerer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    lowerer->ast = run->request->ast;
    module_lowering module = {.lowerer = lowerer,
                              .ast = run->request->ast,
                              .context = run->context,
                              .unit = unit,
                              .error_record = run->request->error_record};
    status = lower_module(&module, run->body);
    sxml_xpath_lowerer_finalize(lowerer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* For casts to xs:QName at run time (D84). */
    sxml_xpath_namespace_collection namespaces = {.bindings = NULL, .binding_count = 0};
    status = sxml_xpath_collect_namespaces(run->request->ast, &run->context->xpath, &namespaces);
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_set_static_namespaces(
            unit, (sxml_namespace_binding_view){.namespace_bindings = namespaces.bindings,
                                                .namespace_binding_count =
                                                    namespaces.binding_count});
        sxml_allocator_release(run->context->xpath.allocator, namespaces.bindings);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_set_static_base_uri(unit, run->context->xpath.static_base_uri);
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
    sxml_ir_unit *new_unit = NULL;
    status = sxml_ir_unit_create(
        host->allocator,
        &(sxml_ir_unit_configuration){.allocator = host->allocator,
                                      .item_heap = host->item_heap,
                                      .source_name = request->ast->source_name,
                                      .maximum_expression_count =
                                          request->maximum_expression_count},
        &new_unit);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = add_main_function(host, new_unit, request->ast);
    sxml_xquery_static_context context;
    bool has_context = false;
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_read_static_context(
            &(sxml_xquery_static_request){.ast = request->ast,
                                          .host_static_context = host,
                                          .unit = new_unit,
                                          .error_record = request->error_record},
            &context);
        has_context = status == SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        status = run_lowering(
            &(lowering_run){.request = request, .context = &context, .body = body}, new_unit);
    }
    if (has_context) {
        sxml_xquery_release_static_context(&context);
    }
    if (status != SXML_STATUS_OK) {
        sxml_ir_unit_release(new_unit);
        return status;
    }
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
    sxml_ir_unit *new_unit = NULL;
    status = sxml_xquery_lower(
        &(sxml_xquery_lowering_request){.ast = ast,
                                        .static_context = request->static_context,
                                        .maximum_expression_count =
                                            request->maximum_expression_count,
                                        .maximum_expression_depth =
                                            request->maximum_expression_depth,
                                        .error_record = request->error_record},
        &new_unit);
    sxml_ast_destroy(ast);
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
