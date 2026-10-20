/*
 * The lowering part `full_text` (src/xpath_lower_internal.h;
 * src/full_text_internal.h; docs/DECISIONS.md D103, D189, D214, D235;
 * packets 7.12a and 7.12d): what XQuery's lowering always links of XQuery
 * and XPath Full Text 3.0, kept small so that a program without the
 * optional feature full-text (sxml.wasm) carries little of it. XPath's
 * lowering links its rule only: XPath reaches the for score procedure
 * through the feature's operations.
 *
 * - the D162 place: without the feature, the first Full Text construct
 *   of a module is XPST0003 (a processor without Full Text does not
 *   accept its syntax); with it, the feature's static rules
 *   (src/full_text_lower.c);
 * - the rule for FT_CONTAINS_EXPRESSION, a procedure whose step is the
 *   feature's (sxml_full_text_lower_step);
 * - the internal native rows contains, whose body is the feature's
 *   matcher (src/full_text.c), and score;
 * - the score of a let score binding, which src/xquery_lower.c binds.
 *
 * Golden file: src/xquery_lower_update.c (internal rows called through
 * STATIC_CALL).
 *
 * Section numbers are of XQuery and XPath Full Text 3.0 (W3C
 * Recommendation, 24 November 2015).
 */
#include "full_text_internal.h"

#include "function_library_core_internal.h"
#include "ir_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"
#include "xpath_ast_internal.h"
#include "xpath_lower_internal.h"
#include "xpath_static_internal.h"

#include <math.h>
#include <stddef.h>

/* ================================================================= Rows */

static const sxml_static_error unsupported_error = {
    .error_status = SXML_STATUS_STATIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPST0003"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Full Text needs the optional feature full-text (XQuery and XPath Full Text 3.0)")};

static sxml_status call_contains(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    const sxml_full_text_operations *operations = sxml_processor_find_optional_feature(
        environment == NULL ? NULL : environment->processor, SXML_OPTIONAL_FEATURE_KIND_FULL_TEXT);
    if (operations == NULL) {
        return sxml_function_library_report(call, &unsupported_error);
    }
    return operations->contains(call, result);
}

static sxml_status call_score_contains(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    const sxml_full_text_operations *operations = sxml_processor_find_optional_feature(
        environment == NULL ? NULL : environment->processor, SXML_OPTIONAL_FEATURE_KIND_FULL_TEXT);
    if (operations == NULL) {
        return sxml_function_library_report(call, &unsupported_error);
    }
    return operations->score_contains(call, result);
}

/* The score of a let score binding (2.3.2; see full_text_internal.h). */
static sxml_status call_score(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence value = sxml_function_library_get_argument(call, 0);
    sxml_item first;
    sxml_status status = sxml_sequence_get_item(
        value, (sxml_item_request){.item_index = 0, .error_record = call->error_record}, &first);
    bool is_relevant = false;
    if (status == SXML_STATUS_OK) {
        is_relevant = true;
        sxml_atomic_datum datum;
        if (sxml_item_get_kind(first) == SXML_ITEM_KIND_ATOMIC &&
            sxml_atomic_item_get_datum(first, &datum) == SXML_STATUS_OK) {
            switch ((uint32_t)datum.datum_kind) {
            case SXML_DATUM_KIND_BOOLEAN:
                is_relevant = datum.datum_details.boolean;
                break;
            case SXML_DATUM_KIND_STRING:
                is_relevant = datum.datum_details.string.utf8.byte_count != 0;
                break;
            case SXML_DATUM_KIND_INTEGER:
                is_relevant = datum.datum_details.integer != 0;
                break;
            case SXML_DATUM_KIND_DOUBLE:
                is_relevant = datum.datum_details.double_number != 0.0 &&
                              !isnan(datum.datum_details.double_number);
                break;
            default:
                break;
            }
        }
    } else if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        status = SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_double(is_relevant ? 1.0 : 0.0));
    }
    return status;
}

static const sxml_function_parameter contains_parameters[] = {
    SXML_REQUIRED_PARAMETER("search", "item()*"),
    SXML_REQUIRED_PARAMETER("program", "array(*)"),
    SXML_REQUIRED_PARAMETER("ignore", "item()*"),
};

static const sxml_function_parameter score_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "item()*"),
};

const sxml_native_function sxml_full_text_contains_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    /* The processor of the evaluation names the feature. */
    .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("contains"),
    .parameters = {contains_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
    .call = call_contains,
    .resumable_body = NULL};

const sxml_native_function sxml_full_text_score_contains_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("score-contains"),
    .parameters = {contains_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:double"),
    .call = call_score_contains,
    .resumable_body = NULL};

const sxml_native_function sxml_full_text_score_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("score"),
    .parameters = {score_parameters, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:double"),
    .call = call_score,
    .resumable_body = NULL};

/* ========================================================= AST access */

static const sxml_ast_node *node_in(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_get_node(ast, node);
}

/* Whether kind is one of Full Text's (D214). */
static bool is_full_text_kind(sxml_ast_kind kind) {
    return kind >= SXML_AST_KIND_FT_OPTION_DECLARATION && kind <= SXML_AST_KIND_FT_EXTENSION_OPTION;
}

/* ========================================================= Lowering */

/* An expression bound in block. */
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

/* A STATIC_CALL of an internal row. */
typedef struct native_call {
    sxml_ast_node_index node;
    const sxml_native_function *native_function;
    sxml_ir_atom_view arguments;
} native_call;

static sxml_status make_native_call(sxml_xpath_lowerer *state, native_call request,
                                    sxml_ir_expression *expression) {
    sxml_ir_atom_list arguments = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(state->unit, request.arguments, &arguments);
    if (status == SXML_STATUS_OK) {
        *expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
            .static_call = {.target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                       .native_function = request.native_function},
                            .arguments = arguments}};
    }
    return status;
}

/* The expression inside any parentheses around node. */
static sxml_ast_node_index unwrapped(const sxml_ast *ast, sxml_ast_node_index node) {
    while (node.value != 0 &&
           node_in(ast, node)->kind == SXML_AST_KIND_PARENTHESIZED_EXPRESSION) {
        node = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = node, .field = SXML_AST_FIELD_EXPRESSION});
    }
    return node;
}

/* Whether the value of a let score binding is a contains expression
   (which then lowers to its score). */
static bool is_contains_value(const sxml_ast *ast, sxml_ast_node_index binding) {
    const sxml_ast_node_index value = unwrapped(
        ast, sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = binding,
                                                             .field = SXML_AST_FIELD_VALUE}));
    return value.value != 0 && node_in(ast, value)->kind == SXML_AST_KIND_FT_CONTAINS_EXPRESSION;
}

/* The score of "let score $s := E" (2.3.2): the score of E when E is a
   contains expression (its lowering gave score-contains), else score(E),
   bound in block. */
sxml_status sxml_xquery_bind_full_text_score(sxml_xpath_lowerer *lowerer,
                                             const sxml_xquery_full_text_score_request *request,
                                             sxml_ir_atom *atom) {
    if (is_contains_value(lowerer->ast, request->node)) {
        *atom = request->value;
        return SXML_STATUS_OK;
    }
    sxml_ir_expression expression;
    const sxml_status status = make_native_call(
        lowerer,
        (native_call){.node = request->node,
                      .native_function = &sxml_full_text_score_function,
                      .arguments = {.atoms = &request->value, .atom_count = 1}},
        &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_new_expression(
        lowerer,
        (expression_in_block){.block = request->block, .node = request->node,
                              .expression = expression},
        atom);
}


/* ========================================================= Static rules */

sxml_status sxml_xquery_check_full_text_rules(const sxml_xquery_full_text_rules_request *request) {
    const sxml_full_text_operations *operations = sxml_processor_find_optional_feature(
        request->processor, SXML_OPTIONAL_FEATURE_KIND_FULL_TEXT);
    if (operations != NULL) {
        return operations->check_rules(request);
    }
    /* A pre-order walk of every node by the links (D71) to the first Full
       Text construct. */
    const sxml_ast *ast = request->ast;
    sxml_ast_node_index node = ast->root;
    while (node.value != 0) {
        const sxml_ast_node *current = node_in(ast, node);
        if (is_full_text_kind(current->kind)) {
            return sxml_xpath_report_static_error(
                &(sxml_xpath_static_error){.ast = ast,
                                           .node = node,
                                           .code_local_name = unsupported_error.code_local_name,
                                           .description = unsupported_error.description},
                request->error_record);
        }
        if (current->first_child.value != 0) {
            node = current->first_child;
            continue;
        }
        while (node.value != 0 && node_in(ast, node)->next_sibling.value == 0) {
            node = node_in(ast, node)->parent;
        }
        if (node.value != 0) {
            node = node_in(ast, node)->next_sibling;
        }
    }
    return SXML_STATUS_OK;
}

/* ================================================== Scores of for items */

/* The predicate of the last step of a for binding's sequence that is a
   contains expression ("for $b score $s in $books[. contains text 'x']"),
   or 0: through parentheses, paths ("/" and "//", to their right) and
   filters (their predicates, last first). */
static sxml_ast_node_index scored_predicate(const sxml_ast *ast, sxml_ast_node_index sequence) {
    sxml_ast_node_index node = unwrapped(ast, sequence);
    while (node.value != 0) {
        const sxml_ast_node *current = node_in(ast, node);
        switch ((uint32_t)current->kind) {
        case SXML_AST_KIND_RELATIVE_PATH_EXPRESSION:
            node = unwrapped(ast, sxml_ast_find_child(ast, (sxml_ast_child_query){
                                                               .parent = node,
                                                               .field = SXML_AST_FIELD_RIGHT}));
            continue;
        case SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION:
            node = unwrapped(ast, sxml_ast_find_child(ast, (sxml_ast_child_query){
                                                               .parent = node,
                                                               .field = SXML_AST_FIELD_PATH}));
            continue;
        case SXML_AST_KIND_FILTER_EXPRESSION:
        case SXML_AST_KIND_AXIS_STEP: {
            for (sxml_ast_node_index predicate = sxml_ast_find_child(
                     ast, (sxml_ast_child_query){.parent = node, .field = SXML_AST_FIELD_PREDICATE});
                 predicate.value != 0; predicate = sxml_ast_find_next_with_same_field(ast, predicate)) {
                const sxml_ast_node_index condition = unwrapped(
                    ast, sxml_ast_find_child(ast, (sxml_ast_child_query){
                                                      .parent = predicate,
                                                      .field = SXML_AST_FIELD_EXPRESSION}));
                if (condition.value != 0 &&
                    node_in(ast, condition)->kind == SXML_AST_KIND_FT_CONTAINS_EXPRESSION) {
                    return condition;
                }
            }
            if (current->kind != SXML_AST_KIND_FILTER_EXPRESSION) {
                return (sxml_ast_node_index){0};
            }
            node = unwrapped(ast, sxml_ast_find_child(ast, (sxml_ast_child_query){
                                                               .parent = node,
                                                               .field = SXML_AST_FIELD_VALUE}));
            continue;
        }
        default:
            return (sxml_ast_node_index){0};
        }
    }
    return node;
}

typedef struct for_score_locals {
    sxml_ir_atom item;
    sxml_ast_node_index predicate;
    sxml_xpath_lowering_focused_result focused;
} for_score_locals;

static sxml_status continue_for_score(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame);

static const sxml_xpath_lowering_procedure for_score_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("full-text for score"),
    .step = continue_for_score,
    .locals_size = sizeof(for_score_locals)};

/* Step 0: the predicate lowered again, for its score, with a focus of its
   own (or 1 when the sequence has none); step 1: that body mapped over
   the item, which gives the item's score. */
static sxml_status continue_for_score(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *state = machine->state;
    for_score_locals *locals = frame->locals;
    sxml_ir_expression expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM};
    if (frame->step == 0 && locals->predicate.value != 0) {
        frame->step = 1;
        return sxml_xpath_lowering_call_focused_body(
            machine, (sxml_xpath_lowering_focused_call){.node = locals->predicate,
                                                        .result = &locals->focused});
    }
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        /* 2.3.1: any item is as relevant as any other. */
        status = sxml_xpath_lowerer_add_item_constant(state, sxml_atomic_item_from_double(1.0),
                                                      &expression.atom);
    } else {
        expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
            .map = {.input = locals->item,
                    .focus = locals->focused.focus,
                    .body = locals->focused.body,
                    .flags = 0}};
    }
    sxml_ir_expression_index index = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            state,
            (sxml_xpath_lowering_expression_to_add){.node = frame->node, .expression = expression},
            &index);
    }
    return status == SXML_STATUS_OK ? sxml_xpath_lowering_finish_frame(machine, frame, index)
                                    : status;
}

sxml_status sxml_xquery_call_full_text_for_score(sxml_xpath_lowering_machine *machine,
                                                 const sxml_xquery_full_text_for_score_request *request) {
    const sxml_ast *ast = machine->state->ast;
    const for_score_locals initial = {
        .item = request->item,
        .predicate = scored_predicate(
            ast, sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = request->binding,
                                                                 .field = SXML_AST_FIELD_SEQUENCE}))};
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &for_score_procedure,
            .node = request->binding,
            .block = request->block,
            .is_body = false,
            .destination = {.atom = request->score, .atom_block = request->block},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof initial}},
        &frame);
}

bool sxml_xquery_full_text_is_scored(const sxml_xpath_lowerer *lowerer,
                                     const sxml_xpath_lowering_frame *frame) {
    const sxml_ast_node_index parent = node_in(lowerer->ast, frame->node)->parent;
    sxml_ast_node_index binding = parent;
    while (binding.value != 0 &&
           node_in(lowerer->ast, binding)->kind == SXML_AST_KIND_PARENTHESIZED_EXPRESSION) {
        binding = node_in(lowerer->ast, binding)->parent;
    }
    if (binding.value != 0 &&
        node_in(lowerer->ast, binding)->kind == SXML_AST_KIND_LET_SCORE_BINDING &&
        is_contains_value(lowerer->ast, binding)) {
        return true;
    }
    /* Asked for by the for score procedure, through its focused body. */
    const sxml_xpath_lowering_frame *below = frame->below;
    return below != NULL && (below->procedure == &for_score_procedure ||
                             (below->below != NULL && below->below->procedure == &for_score_procedure));
}

/* ================================================================= Part */

/* The lowering of "E contains text S" is the feature's; without it the
   static rules have already refused the query. */
static sxml_status continue_contains(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    const sxml_full_text_operations *operations = sxml_processor_find_optional_feature(
        machine->state->static_context->processor, SXML_OPTIONAL_FEATURE_KIND_FULL_TEXT);
    if (operations == NULL) {
        return sxml_xpath_lowerer_report_static_error(
            machine->state, frame->node,
            SXML_BYTE_VIEW_FROM_LITERAL("XPST0003 Full Text needs the optional feature "
                                        "full-text (XQuery and XPath Full Text 3.0)"));
    }
    return operations->lower_step(machine, frame);
}

static const sxml_xpath_lowering_procedure contains_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("full-text contains"),
    .step = continue_contains,
    .locals_size = SXML_FULL_TEXT_LOWERING_LOCALS_SIZE};

static const sxml_xpath_lowering_rule full_text_rules[] = {
    {.kind = SXML_AST_KIND_FT_CONTAINS_EXPRESSION, .procedure = &contains_procedure},
};

/* XPath's too (D235): XPath parses Full Text only with the feature
   (grammars/xpath_full_text), so its D162 place is the grammar. */
const sxml_xpath_lowering_part sxml_xpath_lowering_part_full_text = {
    .languages = SXML_LANGUAGE_XPATH | SXML_LANGUAGE_XQUERY,
    .rules = full_text_rules,
    .rule_count = sizeof(full_text_rules) / sizeof(full_text_rules[0])};
