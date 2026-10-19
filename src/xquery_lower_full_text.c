/*
 * The XQuery lowering part `full_text` (src/xpath_lower_internal.h;
 * src/full_text_internal.h; docs/DECISIONS.md D103, D189, D214; packet
 * 7.12a): what XQuery's lowering always links of XQuery and XPath Full
 * Text 3.0, kept small so that a program without the optional feature
 * full-text (sxml.wasm) carries little of it:
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

/* The score of "let score $s := E" (2.3.2): score(E), bound in block. */
sxml_status sxml_xquery_bind_full_text_score(sxml_xpath_lowerer *lowerer,
                                             const sxml_xquery_full_text_score_request *request,
                                             sxml_ir_atom *atom) {
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

const sxml_xpath_lowering_part sxml_xpath_lowering_part_full_text = {
    .languages = SXML_LANGUAGE_XQUERY,
    .rules = full_text_rules,
    .rule_count = sizeof(full_text_rules) / sizeof(full_text_rules[0])};
