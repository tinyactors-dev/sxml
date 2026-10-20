/*
 * XSLT 4.0 numbering and regular expressions (src/xslt_lower_internal.h;
 * XSLT 4.0, sections 12 and 17; packet 4.11, D153): xsl:number and
 * xsl:analyze-string.
 *
 * Golden file: src/xslt_lower_group.c (instructions as procedures whose
 * steps ask the machine for their parts and return, D71; the glue as
 * short XPath texts with their inputs as variables; a pattern's match
 * expression lowered inline with the item as fixed focus, an error while
 * matching being no match, 6.5).
 *
 * Nothing new runs. xsl:number is the formal definition of 12.3 written
 * as XPath texts: the selected node, the nodes the count pattern is tried
 * on (the ancestors-or-self and their preceding siblings, or for
 * level="any" the preceding nodes and the ancestors-or-self), a FILTER of
 * them by the count pattern's match expression (D118), the same for the
 * from pattern, and the place marker computed from those sets in one
 * text per level. A value attribute gives the place marker directly
 * (12.2). start-at re-bases it (12.1), and the internal row
 * format-place-marker (src/xslt_number_internal.h) formats it with the
 * picture engine of fn:format-integer (D119); the result is a text node.
 *
 * xsl:analyze-string is fn:matching-segments (D114, the one regex
 * engine, D105) with the non-matching segments between the matches put
 * in by an XPath text, then a MAP over the segments whose body is one of
 * the two children, with the segment's string as context item and its
 * position among the segments (17.1). The current captured groups are
 * the variable Q{XSLT}regex-groups, in scope in xsl:matching-substring
 * (the segment's groups with group 0, the match) and in
 * xsl:non-matching-substring (the empty map); fn:regex-groups and
 * fn:regex-group read it lexically (glossary `context_function`), so a
 * template called from the instruction sees no captured groups.
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_lower_internal.h"

#include "function_library_core_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xslt_number_internal.h"
#include "xslt_pattern_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* The namespace of the variables the XPath texts below read. */
#define NUMBER_VARIABLE "$Q{urn:x-sxml:number-variable}"
#define NUMBER_VARIABLE_NAMESPACE "urn:x-sxml:number-variable"

/* ============================================================ AST reads */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

static const sxml_xslt_ast_attribute *find_attribute(const sxml_xslt_lowering *lowering,
                                                     sxml_xslt_ast_node_index node,
                                                     sxml_xslt_ast_attribute_kind kind) {
    return sxml_xslt_ast_get_attribute(
        lowering->ast, sxml_xslt_ast_find_attribute(
                           lowering->ast, (sxml_xslt_ast_attribute_query){.node = node, .kind = kind}));
}

static bool has_attribute(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                          sxml_xslt_ast_attribute_kind kind) {
    return find_attribute(lowering, node, kind)->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
}

/* The first child of node of kind; 0 when none. */
typedef struct child_query {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_kind kind;
} child_query;

static sxml_xslt_ast_node_index first_child_of(const sxml_xslt_lowering *lowering,
                                               child_query query) {
    sxml_xslt_ast_node_index child = node_of(lowering, query.node)->first_child;
    while (child.value != 0 && node_of(lowering, child)->kind != query.kind) {
        child = node_of(lowering, child)->next_sibling;
    }
    return child;
}

/* =========================================================== IR pieces */

/* Adds expression and binds it in block. */
typedef struct binding {
    sxml_xpath_lowering_block *block;
    sxml_ir_expression expression;
} binding;

static sxml_status bind(sxml_xpath_lowerer *lowerer, binding request, sxml_ir_atom *atom) {
    return sxml_xslt_lowering_bind_expression(
        lowerer,
        &(sxml_xslt_expression_to_bind){.block = request.block, .expression = request.expression},
        atom);
}

static sxml_ir_expression atom_expression(sxml_ir_atom atom) {
    return (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = atom};
}

static sxml_status add_empty_atom(sxml_xpath_lowerer *lowerer, sxml_ir_atom *atom) {
    sxml_ir_constant_index constant = {0};
    const sxml_status status =
        sxml_ir_unit_add_constant(lowerer->unit, sxml_sequence_empty(), &constant);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *atom = (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_CONSTANT, .constant = constant};
    return SXML_STATUS_OK;
}

/* Ends frame with expression as its value. */
static sxml_status finish_with_expression(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame,
                                          sxml_ir_expression expression) {
    sxml_ir_expression_index result = {0};
    const sxml_status status = sxml_xslt_lowering_add_expression(machine->state, expression, &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

/* The block ended with an ATOM expression of value. */
typedef struct branch_result {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
} branch_result;

static sxml_status finish_branch(sxml_xpath_lowerer *lowerer, branch_result branch,
                                 sxml_ir_expression_index *expression) {
    sxml_ir_expression_index result = {0};
    const sxml_status status = sxml_xslt_lowering_add_expression(lowerer, atom_expression(branch.value), &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        lowerer, (sxml_xpath_lowering_block_result){.block = *branch.block, .result = result},
        expression);
}

/* The focus value of focus, read in block. */
typedef struct focus_value_read {
    sxml_xpath_lowering_focus *focus;
    sxml_xpath_lowering_block *block;
} focus_value_read;

static sxml_status read_focus_value(sxml_xpath_lowerer *lowerer, focus_value_read request,
                                    sxml_ir_atom *atom) {
    sxml_xpath_lowering_focus *outer_focus = lowerer->function->focus;
    lowerer->function->focus = request.focus;
    const sxml_status status = sxml_xpath_lowerer_read_focus(
        lowerer,
        (sxml_xpath_lowering_focus_read){.block = request.block,
                                         .node = no_ast_node,
                                         .part = SXML_XPATH_LOWERING_FOCUS_PART_VALUE},
        atom);
    lowerer->function->focus = outer_focus;
    return status;
}

/* A slot bound to value in block (a LET), for a focus to name. */
static sxml_status bind_in_slot(sxml_xpath_lowerer *lowerer, binding request, sxml_ir_slot *slot) {
    sxml_ir_slot made = {0};
    sxml_status status = sxml_xpath_lowerer_add_slot(lowerer, &made);
    sxml_ir_expression_index bound = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, request.expression, &bound);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            atom_expression((sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = made}),
            &body);
    }
    sxml_ir_atom atom = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = request.block,
                                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                                               .let = {.variable = made,
                                                       .bound_expression = bound,
                                                       .body = body}}},
                      &atom);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (atom.atom_kind != SXML_IR_ATOM_KIND_SLOT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *slot = atom.slot;
    return SXML_STATUS_OK;
}

/* =============================================================== Texts */

/* Parses text (kept until the lowering ends). */
static sxml_status parse_kept(const sxml_xslt_lowering *lowering, sxml_byte_view text,
                              sxml_ast **ast) {
    sxml_ast *made = NULL;
    const sxml_status status = sxml_xpath_build_ast(
        &(sxml_ast_build_request){.expression_text = text,
                                  .source_name = {.bytes = NULL, .byte_count = 0},
                                  .allocator = lowering->lowerer->allocator,
                                  .maximum_expression_depth = 0,
                                  .error_record = lowering->error_record},
        &made);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *ast = made;
    return sxml_xslt_lowering_keep_ast(lowering, made);
}

#define TEXT_VARIABLE_MAXIMUM_COUNT 3U

/* One variable of an XPath text: Q{urn:x-sxml:number-variable}local_name. */
typedef struct text_variable {
    const char *local_name;
    sxml_ir_atom atom;
} text_variable;

/* An XPath text of this file to lower in a block of its own with its
   variables, then bound in block, inside a TRY_CATCH that raises XSLT's
   codes when mappings has any. */
typedef struct text_locals {
    const sxml_xslt_lowering *lowering;
    const char *text;
    text_variable variables[TEXT_VARIABLE_MAXIMUM_COUNT];
    size_t variable_count;
    sxml_xslt_error_mapping_view mappings;
    sxml_xpath_lowering_block *block;
    sxml_xpath_lowering_block text_block;
    size_t scope_mark;
    sxml_ir_atom value;
} text_locals;

/* The value of the text block, bound in the caller's block. */
static sxml_status bind_text_value(sxml_xpath_lowerer *lowerer, text_locals *locals,
                                   sxml_ir_atom *atom) {
    sxml_ir_expression_index body = {0};
    sxml_status status = finish_branch(
        lowerer, (branch_result){.block = &locals->text_block, .value = locals->value}, &body);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (locals->mappings.mapping_count == 0) {
        return sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = locals->block, .expression = body},
            atom);
    }
    /* let $v := (the text) return $v, as one expression to map. */
    sxml_ir_slot value = {0};
    status = sxml_xpath_lowerer_add_slot(lowerer, &value);
    sxml_ir_expression_index read = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            atom_expression((sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = value}),
            &read);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_bind_mapped_expression(
        lowerer,
        &(sxml_xslt_mapped_expression){
            .block = locals->block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                           .let = {.variable = value, .bound_expression = body, .body = read}},
            .mappings = locals->mappings},
        atom);
}

static sxml_status continue_text(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    text_locals *locals = frame->locals;
    if (frame->step == 1) {
        sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
        sxml_ir_atom atom = {0};
        const sxml_status status = bind_text_value(lowerer, locals, &atom);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return finish_with_expression(machine, frame, atom_expression(atom));
    }
    frame->step = 1;
    sxml_ast *ast = NULL;
    sxml_status status =
        parse_kept(locals->lowering, sxml_byte_view_from_c_string(locals->text), &ast);
    sxml_namespace_uri_identifier namespace_uri = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            lowerer->name_pool, SXML_BYTE_VIEW_FROM_LITERAL(NUMBER_VARIABLE_NAMESPACE),
            &namespace_uri);
    }
    locals->scope_mark = lowerer->scope_entry_count;
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && variable_index < locals->variable_count;
         variable_index += 1) {
        sxml_name_identifier name = {0};
        status = sxml_name_pool_intern_name(
            lowerer->name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = sxml_byte_view_from_c_string(
                                     locals->variables[variable_index].local_name)},
            &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_push_variable(
                lowerer, (sxml_xpath_lowering_scope_entry){
                             .variable_name = name,
                             .function_level = lowerer->function->function_level,
                             .atom = locals->variables[variable_index].atom});
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->text_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    /* The texts name only fn functions and EQNames: the lowerer's static
       context serves; "." is the instruction's focus. */
    return sxml_xpath_lowering_call_embedded(
        machine, &(sxml_xpath_lowering_embedded_call){
                     .embedding = {.ast = ast,
                                   .node = {.value = 0},
                                   .static_context = NULL,
                                   .focus = NULL},
                     .block = &locals->text_block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = &locals->value,
                                     .atom_block = &locals->text_block,
                                     .focused = NULL}});
}

static const sxml_xpath_lowering_procedure text_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("XSLT numbering text"),
    .step = continue_text,
    .locals_size = sizeof(text_locals)};

/* From a step: lowers text with variables in block, the value to *atom. */
typedef struct text_call {
    const sxml_xslt_lowering *lowering;
    const char *text;
    const text_variable *variables;
    size_t variable_count;
    sxml_xslt_error_mapping_view mappings;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
} text_call;

static sxml_status call_text(sxml_xpath_lowering_machine *machine, const text_call *call) {
    if (call->variable_count > TEXT_VARIABLE_MAXIMUM_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    text_locals initial = {.lowering = call->lowering,
                           .text = call->text,
                           .variable_count = call->variable_count,
                           .mappings = call->mappings,
                           .block = call->block};
    for (size_t variable_index = 0; variable_index < call->variable_count; variable_index += 1) {
        initial.variables[variable_index] = call->variables[variable_index];
    }
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &text_procedure,
            .node = no_ast_node,
            .block = call->block,
            .is_body = false,
            .destination = {.expression = NULL,
                            .atom = call->atom,
                            .atom_block = call->block,
                            .focused = NULL},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof(initial)}},
        &frame);
}

static const sxml_xslt_error_mapping_view no_mappings = {.mappings = NULL, .mapping_count = 0};

/* ============================================================ Patterns */

/*
 * The items of an input that match a pattern (D118), as a FILTER whose
 * predicate is the match expression with the item as fixed focus, inside
 * a TRY_CATCH whose one clause gives false (6.5: an error while matching
 * is no match). Lowered in two steps: start pushes the match expression,
 * finish binds the FILTER.
 */
typedef struct pattern_filter {
    sxml_xpath_lowering_focus item_focus;
    sxml_xpath_lowering_focus match_focus;
    sxml_xpath_lowering_block predicate_block;
    sxml_xpath_lowering_block match_block;
    sxml_ir_atom matched;
    /* The scope before the pattern, which clears the dynamic context
       (6.5, 17.2.2, 26.3.1). */
    size_t scope_mark;
} pattern_filter;

typedef struct pattern_filter_start {
    const sxml_xslt_lowering *lowering;
    const sxml_xslt_ast_attribute *pattern;
    pattern_filter *filter;
} pattern_filter_start;

static sxml_status start_pattern_filter(sxml_xpath_lowering_machine *machine,
                                        pattern_filter_start request) {
    sxml_xpath_lowerer *lowerer = machine->state;
    const sxml_xslt_lowering *lowering = request.lowering;
    pattern_filter *filter = request.filter;
    const sxml_xslt_ast_expression *pattern =
        sxml_xslt_ast_get_expression(lowering->ast, request.pattern->value.payload.expression);
    if (pattern->ast == NULL || pattern->static_context.value == 0 ||
        pattern->static_context.value >= lowering->static_context_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xpath_static_context *context =
        &lowering->static_contexts[pattern->static_context.value];
    filter->item_focus = (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
    filter->predicate_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    filter->match_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    sxml_ir_atom item = {0};
    sxml_status status = read_focus_value(
        lowerer,
        (focus_value_read){.focus = &filter->item_focus, .block = &filter->predicate_block},
        &item);
    sxml_ast *match = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_build_match_expression(
            &(sxml_xslt_pattern_match_expression_request){.pattern_ast = pattern->ast,
                                                          .static_context = context,
                                                          .allocator = lowerer->allocator,
                                                          .error_record = lowering->error_record},
            &match);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_keep_ast(lowering, match);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    filter->match_focus =
        (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED, .fixed_value = item};
    /* 17.2.2, 26.3.1: no captured groups and no current output URI in a
       pattern. */
    filter->scope_mark = lowerer->scope_entry_count;
    status = sxml_xpath_lowerer_clear_output_uri(lowerer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_call_embedded(
        machine, &(sxml_xpath_lowering_embedded_call){
                     .embedding = {.ast = match,
                                   .node = {.value = 0},
                                   .static_context = context,
                                   .focus = &filter->match_focus},
                     .block = &filter->match_block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = &filter->matched,
                                     .atom_block = &filter->match_block,
                                     .focused = NULL}});
}

/* The guarded match in the predicate block, the FILTER of input in block. */
typedef struct pattern_filter_finish {
    pattern_filter *filter;
    sxml_ir_atom input;
    sxml_xpath_lowering_block *block;
} pattern_filter_finish;

static sxml_status finish_pattern_filter(sxml_xpath_lowerer *lowerer,
                                         pattern_filter_finish request, sxml_ir_atom *atom) {
    pattern_filter *filter = request.filter;
    sxml_xpath_lowerer_pop_variables(lowerer, filter->scope_mark);
    sxml_ir_expression_index body = {0};
    sxml_status status = finish_branch(
        lowerer, (branch_result){.block = &filter->match_block, .value = filter->matched}, &body);
    sxml_ir_expression guarded_match = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_make_guarded_match(lowerer, body, &guarded_match);
    }
    sxml_ir_atom guarded = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = &filter->predicate_block, .expression = guarded_match},
                      &guarded);
    }
    sxml_ir_expression_index predicate = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(
            lowerer, (branch_result){.block = &filter->predicate_block, .value = guarded},
            &predicate);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(lowerer,
                (binding){.block = request.block,
                          .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_FILTER,
                                         .filter = {.input = request.input,
                                                    .focus = filter->item_focus.slots,
                                                    .predicate = predicate}}},
                atom);
}

/* ========================================================== xsl:number */

#define ERROR_CODE(code) "QName('http://www.w3.org/2005/xqt-errors', '" code "')"

/* 12.2: each atomized value as xs:integer(round(number($V))), XTDE0980
   for NaN, infinity and negative numbers; xs:decimal values (xs:integer
   among them) are rounded exactly. */
static const char value_numbers_text[] =
    "data(" NUMBER_VARIABLE "value) ! (let $r := if (. instance of xs:decimal) then round(.) "
    "else round(number(.)) return if (not($r instance of xs:decimal) and "
    "(string($r) = ('NaN', 'INF', '-INF'))) then error(" ERROR_CODE("XTDE0980") ", "
    "'The value of xsl:number cannot be converted to an integer (XSLT 4.0, 12.2)') "
    "else if ($r lt 0) then error(" ERROR_CODE("XTDE0980") ", "
    "'The value of xsl:number is negative (XSLT 4.0, 12.2)') else xs:integer($r))";

/* 12.2 with XSLT 1.0 behaviour (3.8.1): the first atomized item only, NaN
   for none; a value that cannot be converted to an integer gives NaN, the
   marker of the string "NaN" in the formatted result, not XTDE0980. */
static const char compatible_value_numbers_text[] =
    "let $v := data(" NUMBER_VARIABLE "value)[1], $r := if (empty($v)) then xs:double('NaN') "
    "else if ($v instance of xs:decimal) then round($v) else round(number($v)) "
    "return if (not($r instance of xs:decimal) and (string($r) = ('NaN', 'INF', '-INF'))) "
    "then xs:double('NaN') else if ($r lt 0) then error(" ERROR_CODE("XTDE0980") ", "
    "'The value of xsl:number is negative (XSLT 4.0, 12.2)') else xs:integer($r)";

/* The formatted result for the NaN marker: "NaN" between the format's
   prefix and suffix, its leading and trailing characters that are not
   alphanumeric (12.4); () when the place marker has no NaN. */
static const char compatible_not_a_number_text[] =
    "if (" NUMBER_VARIABLE "numbers instance of xs:double) then "
    "let $f := " NUMBER_VARIABLE "format return "
    "replace($f, '^([^\\p{L}\\p{N}]*).*$', '$1', 's') || 'NaN' || "
    "(if (matches($f, '[\\p{L}\\p{N}]')) then "
    "replace($f, '^.*[\\p{L}\\p{N}]([^\\p{L}\\p{N}]*)$', '$1', 's') else '') else ()";

static const char compatible_integers_text[] =
    NUMBER_VARIABLE "numbers[. instance of xs:integer]";

static const char compatible_choice_text[] =
    "(" NUMBER_VARIABLE "nan, " NUMBER_VARIABLE "formatted)[1]";

/* 12.3: the selected node: the context item, or the select value. */
static const char context_node_text[] =
    "if (. instance of node()) then . else error(" ERROR_CODE("XTTE0990") ", "
    "'xsl:number without value or select is evaluated with a context item that is not a node "
    "(XSLT 4.0, 12.3)')";

static const sxml_xslt_error_mapping absent_focus_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
     .raised_status = SXML_STATUS_TYPE_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTTE0990"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "xsl:number without value or select is evaluated without a context item (XSLT 4.0, "
         "12.3)")},
};

static const char selected_node_text[] =
    "if (count(" NUMBER_VARIABLE "select) eq 1 and " NUMBER_VARIABLE "select instance of node()) "
    "then " NUMBER_VARIABLE "select else error(" ERROR_CODE("XTTE1000") ", "
    "'The select attribute of xsl:number does not give a single node (XSLT 4.0, 12.3)')";

/* The nodes the count pattern is tried on: for level single and
   multiple, the ancestors-or-self and their preceding siblings (the
   formal definition tries no others); for any, the preceding nodes and
   the ancestors-or-self. The from pattern is tried on the
   ancestors-or-self, or for any on the same nodes as count. */
static const char sibling_candidates_text[] =
    NUMBER_VARIABLE "node/(ancestor-or-self::node() | "
                    "ancestor-or-self::node()/preceding-sibling::node())";
static const char ancestor_candidates_text[] = NUMBER_VARIABLE "node/ancestor-or-self::node()";
static const char any_candidates_text[] =
    NUMBER_VARIABLE "node/(preceding::node() | ancestor-or-self::node())";

/* 12.3: without count, the nodes of the selected node's kind and, if it
   has one, expanded name. */
static const char default_count_text[] =
    "let $s := " NUMBER_VARIABLE "node return " NUMBER_VARIABLE "candidates["
    "if ($s instance of element()) then (self::element() and node-name(.) eq node-name($s)) "
    "else if ($s instance of attribute()) then (self::attribute() and node-name(.) eq "
    "node-name($s)) "
    "else if ($s instance of text()) then self::text() "
    "else if ($s instance of comment()) then self::comment() "
    "else if ($s instance of processing-instruction()) then (self::processing-instruction() "
    "and node-name(.) eq node-name($s)) "
    "else if ($s instance of namespace-node()) then (self::namespace-node() and "
    "string(node-name(.)) eq string(node-name($s))) "
    "else self::document-node()]";

/*
 * 12.3, for each level, with $counted the candidates matching count and
 * $from those matching from (() without a from pattern): matches-from is
 * also true for the root of the tree, so $F is the innermost (any: the
 * last) of $from and the root. A counted node's preceding siblings that
 * match count are the counted candidates with its parent before it.
 */
static const char single_level_text[] =
    "let $s := " NUMBER_VARIABLE "node, $c := " NUMBER_VARIABLE "counted, "
    "$ancestors := $s/ancestor-or-self::node(), "
    "$f := (" NUMBER_VARIABLE "from | root($s))[last()], "
    "$a := ($c intersect $ancestors)[last()][ancestor-or-self::node()[. is $f]] "
    "return if (empty($a)) then () else 1 + count($c[. << $a][.. is $a/..])";

static const char multiple_level_text[] =
    "let $s := " NUMBER_VARIABLE "node, $c := " NUMBER_VARIABLE "counted, "
    "$f := (" NUMBER_VARIABLE "from | root($s))[last()] "
    "return for $a in ($c intersect $s/ancestor-or-self::node())"
    "[ancestor-or-self::node()[. is $f]] "
    "return 1 + count($c[. << $a][.. is $a/..])";

static const char any_level_text[] =
    "let $f := (" NUMBER_VARIABLE "from | root(" NUMBER_VARIABLE "node))[last()], "
    "$a := " NUMBER_VARIABLE "counted[. is $f or . >> $f] "
    "return if (empty($a)) then () else count($a)";

/* 12.1: the place marker re-based by start-at. */
static const char start_at_text[] =
    "let $t := normalize-space(" NUMBER_VARIABLE "start) return "
    "if (not(matches($t, '^-?[0-9]+( -?[0-9]+)*$'))) then error(" ERROR_CODE("XTDE0030") ", "
    "'The start-at attribute of xsl:number is not a list of integers (XSLT 4.0, 12.1)') "
    "else let $starts := tokenize($t) ! xs:integer(.) return "
    "for $n at $i in " NUMBER_VARIABLE "numbers return $n + ($starts[$i], $starts[last()])[1] - 1";

/* The number to string conversion attributes (12.4), in the order of the
   row's parameters after $numbers. */
static const sxml_xslt_ast_attribute_kind conversion_attribute_kinds[] = {
    SXML_XSLT_AST_ATTRIBUTE_FORMAT,       SXML_XSLT_AST_ATTRIBUTE_GROUPING_SEPARATOR,
    SXML_XSLT_AST_ATTRIBUTE_GROUPING_SIZE, SXML_XSLT_AST_ATTRIBUTE_ORDINAL,
    SXML_XSLT_AST_ATTRIBUTE_LANG,          SXML_XSLT_AST_ATTRIBUTE_LETTER_VALUE,
};

#define CONVERSION_ATTRIBUTE_COUNT \
    (sizeof(conversion_attribute_kinds) / sizeof(conversion_attribute_kinds[0]))

/* The steps of xsl:number (frame->step). */
typedef enum number_step {
    SXML_NUMBER_STEP_START,
    SXML_NUMBER_STEP_VALUE,
    SXML_NUMBER_STEP_SELECTED_NODE,
    SXML_NUMBER_STEP_CANDIDATES,
    SXML_NUMBER_STEP_COUNT,
    SXML_NUMBER_STEP_COUNTED,
    SXML_NUMBER_STEP_FROM_CANDIDATES,
    SXML_NUMBER_STEP_FROM,
    SXML_NUMBER_STEP_FROM_MATCHED,
    SXML_NUMBER_STEP_PLACE_MARKER,
    SXML_NUMBER_STEP_CONVERSION,
    SXML_NUMBER_STEP_START_AT,
    /* XSLT 1.0 behaviour with a value attribute: the NaN marker's text,
       then the integers to format, then the choice of the two texts. */
    SXML_NUMBER_STEP_NOT_A_NUMBER,
    SXML_NUMBER_STEP_INTEGERS,
    SXML_NUMBER_STEP_FORMAT,
    SXML_NUMBER_STEP_CHOICE,
} number_step;

typedef struct number_locals {
    sxml_xslt_instruction_locals head;
    sxml_xslt_ast_keyword level;
    /* The select or value attribute's value. */
    sxml_ir_atom selected;
    sxml_ir_atom node;
    sxml_ir_atom candidates;
    sxml_ir_atom counted;
    sxml_ir_atom from_candidates;
    sxml_ir_atom from;
    sxml_ir_atom numbers;
    sxml_ir_atom start_at;
    /* The conversion attributes' values ("1" for an absent format, ()
       for the others). */
    sxml_ir_atom conversions[CONVERSION_ATTRIBUTE_COUNT];
    size_t conversion_index;
    pattern_filter filter;
    /* XSLT 1.0 behaviour with a value attribute (12.2): the text for the
       NaN marker, or (), and the formatted place marker. */
    bool is_compatible_value;
    sxml_ir_atom not_a_number_text;
    sxml_ir_atom formatted;
} number_locals;


/* 12, XTSE0975: value excludes select, level, count and from. */
static sxml_status check_number(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    if (has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_VALUE) &&
        (has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT) ||
         has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_LEVEL) ||
         has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COUNT) ||
         has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_FROM))) {
        return sxml_xslt_lowering_report_static_error(
            lowering, (sxml_xslt_static_error){
                          .node = node,
                          .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE0975 xsl:number has a value attribute and one of select, "
                              "level, count and from (XSLT 4.0, 12)")});
    }
    return SXML_STATUS_OK;
}

/* Pushes the text with up to three variables, bound in the frame's block. */
typedef struct number_text {
    const char *text;
    text_variable variables[TEXT_VARIABLE_MAXIMUM_COUNT];
    size_t variable_count;
    sxml_ir_atom *atom;
} number_text;

static sxml_status call_number_text(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame, const number_text *text) {
    const number_locals *locals = frame->locals;
    return call_text(machine, &(text_call){.lowering = locals->head.lowering,
                                           .text = text->text,
                                           .variables = text->variables,
                                           .variable_count = text->variable_count,
                                           .mappings = no_mappings,
                                           .block = frame->block,
                                           .atom = text->atom});
}

/* An attribute's expression lowered to an atom in the frame's block. */
typedef struct attribute_lowering {
    sxml_xslt_ast_attribute_kind kind;
    sxml_ir_atom *atom;
} attribute_lowering;

static sxml_status call_attribute(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame, attribute_lowering request) {
    const number_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_attribute *attribute =
        find_attribute(lowering, locals->head.node, request.kind);
    const sxml_xslt_expression_call call = {.lowering = lowering,
                                            .expression = attribute->value.payload.expression,
                                            .block = frame->block,
                                            .atom = request.atom,
                                            .focus = NULL};
    if (attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
        return sxml_xslt_lowering_call_value_template(machine, &call);
    }
    return sxml_xslt_lowering_call_expression(machine, &call);
}

/* The steps that find the place marker from the selected node (12.3). */
static sxml_status step_position(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    number_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index node = locals->head.node;
    const bool is_any = locals->level == SXML_XSLT_AST_KEYWORD_ANY;
    switch ((number_step)frame->step) {
    case SXML_NUMBER_STEP_SELECTED_NODE:
        frame->step = SXML_NUMBER_STEP_CANDIDATES;
        if (has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT)) {
            return call_number_text(
                machine, frame,
                &(number_text){.text = selected_node_text,
                               .variables = {{.local_name = "select", .atom = locals->selected}},
                               .variable_count = 1,
                               .atom = &locals->node});
        }
        /* 12.3: XTTE0990 also when the focus is absent. */
        return call_text(
            machine,
            &(text_call){.lowering = lowering,
                         .text = context_node_text,
                         .variables = NULL,
                         .variable_count = 0,
                         .mappings = {.mappings = absent_focus_mappings,
                                      .mapping_count = sizeof(absent_focus_mappings) /
                                                       sizeof(absent_focus_mappings[0])},
                         .block = frame->block,
                         .atom = &locals->node});
    case SXML_NUMBER_STEP_CANDIDATES:
        frame->step = SXML_NUMBER_STEP_COUNT;
        return call_number_text(
            machine, frame,
            &(number_text){.text = is_any ? any_candidates_text : sibling_candidates_text,
                           .variables = {{.local_name = "node", .atom = locals->node}},
                           .variable_count = 1,
                           .atom = &locals->candidates});
    case SXML_NUMBER_STEP_COUNT: {
        const sxml_xslt_ast_attribute *count =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COUNT);
        if (count->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
            frame->step = SXML_NUMBER_STEP_COUNTED;
            return start_pattern_filter(
                machine, (pattern_filter_start){
                             .lowering = lowering, .pattern = count, .filter = &locals->filter});
        }
        frame->step = SXML_NUMBER_STEP_FROM_CANDIDATES;
        return call_number_text(
            machine, frame,
            &(number_text){.text = default_count_text,
                           .variables = {{.local_name = "node", .atom = locals->node},
                                         {.local_name = "candidates", .atom = locals->candidates}},
                           .variable_count = 2,
                           .atom = &locals->counted});
    }
    case SXML_NUMBER_STEP_COUNTED: {
        frame->step = SXML_NUMBER_STEP_FROM_CANDIDATES;
        return finish_pattern_filter(machine->state,
                                     (pattern_filter_finish){.filter = &locals->filter,
                                                             .input = locals->candidates,
                                                             .block = frame->block},
                                     &locals->counted);
    }
    case SXML_NUMBER_STEP_FROM_CANDIDATES:
        if (!has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_FROM)) {
            frame->step = SXML_NUMBER_STEP_PLACE_MARKER;
            return add_empty_atom(machine->state, &locals->from);
        }
        frame->step = SXML_NUMBER_STEP_FROM;
        if (is_any) {
            locals->from_candidates = locals->candidates;
            return SXML_STATUS_OK;
        }
        return call_number_text(
            machine, frame,
            &(number_text){.text = ancestor_candidates_text,
                           .variables = {{.local_name = "node", .atom = locals->node}},
                           .variable_count = 1,
                           .atom = &locals->from_candidates});
    case SXML_NUMBER_STEP_FROM:
        frame->step = SXML_NUMBER_STEP_FROM_MATCHED;
        return start_pattern_filter(
            machine, (pattern_filter_start){
                         .lowering = lowering,
                         .pattern = find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_FROM),
                         .filter = &locals->filter});
    case SXML_NUMBER_STEP_FROM_MATCHED:
        frame->step = SXML_NUMBER_STEP_PLACE_MARKER;
        return finish_pattern_filter(machine->state,
                                     (pattern_filter_finish){.filter = &locals->filter,
                                                             .input = locals->from_candidates,
                                                             .block = frame->block},
                                     &locals->from);
    case SXML_NUMBER_STEP_PLACE_MARKER: {
        frame->step = SXML_NUMBER_STEP_CONVERSION;
        const char *text = locals->level == SXML_XSLT_AST_KEYWORD_MULTIPLE ? multiple_level_text
                           : is_any                                        ? any_level_text
                                                                           : single_level_text;
        return call_number_text(
            machine, frame,
            &(number_text){.text = text,
                           .variables = {{.local_name = "node", .atom = locals->node},
                                         {.local_name = "counted", .atom = locals->counted},
                                         {.local_name = "from", .atom = locals->from}},
                           .variable_count = 3,
                           .atom = &locals->numbers});
    }
    case SXML_NUMBER_STEP_START:
    case SXML_NUMBER_STEP_VALUE:
    case SXML_NUMBER_STEP_CONVERSION:
    case SXML_NUMBER_STEP_START_AT:
    case SXML_NUMBER_STEP_NOT_A_NUMBER:
    case SXML_NUMBER_STEP_INTEGERS:
    case SXML_NUMBER_STEP_FORMAT:
    case SXML_NUMBER_STEP_CHOICE:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The text node of the formatted place marker (12.4), as the frame's
   result. */
static sxml_status finish_number(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    number_locals *locals = frame->locals;
    sxml_ir_atom arguments[CONVERSION_ATTRIBUTE_COUNT + 1];
    arguments[0] = locals->numbers;
    memcpy(&arguments[1], locals->conversions, sizeof(locals->conversions));
    sxml_ir_atom_list argument_list = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        lowerer->unit,
        (sxml_ir_atom_view){.atoms = arguments, .atom_count = CONVERSION_ATTRIBUTE_COUNT + 1},
        &argument_list);
    sxml_ir_atom text = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = frame->block,
                                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                                               .static_call =
                                                   {.target = {.target_kind =
                                                                   SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                               .native_function =
                                                                   &sxml_xslt_format_place_marker_function},
                                                    .arguments = argument_list}}},
                      &text);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (locals->is_compatible_value) {
        frame->step = SXML_NUMBER_STEP_CHOICE;
        return call_number_text(
            machine, frame,
            &(number_text){.text = compatible_choice_text,
                           .variables = {{.local_name = "nan", .atom = locals->not_a_number_text},
                                         {.local_name = "formatted", .atom = text}},
                           .variable_count = 2,
                           .atom = &locals->formatted});
    }
    return finish_with_expression(
        machine, frame,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                             .node_construction = {.value = text}});
}

/* After the place marker and start-at: with XSLT 1.0 behaviour and a
   value attribute, the NaN marker's text first (12.2), else the format. */
static sxml_status format_numbers(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    number_locals *locals = frame->locals;
    if (!locals->is_compatible_value) {
        frame->step = SXML_NUMBER_STEP_FORMAT;
        return finish_number(machine, frame);
    }
    frame->step = SXML_NUMBER_STEP_INTEGERS;
    return call_number_text(
        machine, frame,
        &(number_text){.text = compatible_not_a_number_text,
                       .variables = {{.local_name = "numbers", .atom = locals->numbers},
                                     {.local_name = "format", .atom = locals->conversions[0]}},
                       .variable_count = 2,
                       .atom = &locals->not_a_number_text});
}

static sxml_status continue_number(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    number_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index node = locals->head.node;
    sxml_status status = SXML_STATUS_OK;
    switch ((number_step)frame->step) {
    case SXML_NUMBER_STEP_START: {
        status = check_number(lowering, node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_xslt_ast_attribute *level =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_LEVEL);
        locals->level = level->kind == SXML_XSLT_AST_ATTRIBUTE_NONE
                            ? SXML_XSLT_AST_KEYWORD_SINGLE
                            : level->value.payload.keyword;
        if (has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_VALUE)) {
            frame->step = SXML_NUMBER_STEP_VALUE;
            return call_attribute(machine, frame,
                                  (attribute_lowering){.kind = SXML_XSLT_AST_ATTRIBUTE_VALUE,
                                                       .atom = &locals->selected});
        }
        frame->step = SXML_NUMBER_STEP_SELECTED_NODE;
        if (has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT)) {
            return call_attribute(machine, frame,
                                  (attribute_lowering){.kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                       .atom = &locals->selected});
        }
        return step_position(machine, frame);
    }
    case SXML_NUMBER_STEP_VALUE:
        frame->step = SXML_NUMBER_STEP_CONVERSION;
        locals->is_compatible_value = sxml_xslt_lowering_has_xslt_1_behaviour(lowering, node);
        return call_number_text(
            machine, frame,
            &(number_text){.text = locals->is_compatible_value ? compatible_value_numbers_text
                                                               : value_numbers_text,
                           .variables = {{.local_name = "value", .atom = locals->selected}},
                           .variable_count = 1,
                           .atom = &locals->numbers});
    case SXML_NUMBER_STEP_SELECTED_NODE:
    case SXML_NUMBER_STEP_CANDIDATES:
    case SXML_NUMBER_STEP_COUNT:
    case SXML_NUMBER_STEP_COUNTED:
    case SXML_NUMBER_STEP_FROM_CANDIDATES:
    case SXML_NUMBER_STEP_FROM:
    case SXML_NUMBER_STEP_FROM_MATCHED:
    case SXML_NUMBER_STEP_PLACE_MARKER:
        return step_position(machine, frame);
    case SXML_NUMBER_STEP_CONVERSION:
        /* One attribute per step: each value template is a frame. */
        while (locals->conversion_index < CONVERSION_ATTRIBUTE_COUNT) {
            const size_t index = locals->conversion_index;
            locals->conversion_index += 1;
            const sxml_xslt_ast_attribute_kind kind = conversion_attribute_kinds[index];
            if (has_attribute(lowering, node, kind)) {
                return call_attribute(
                    machine, frame,
                    (attribute_lowering){.kind = kind, .atom = &locals->conversions[index]});
            }
            status = kind == SXML_XSLT_AST_ATTRIBUTE_FORMAT
                         ? sxml_xpath_lowerer_add_string_constant(
                               lowerer, SXML_BYTE_VIEW_FROM_LITERAL("1"),
                               &locals->conversions[index])
                         : add_empty_atom(lowerer, &locals->conversions[index]);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        frame->step = SXML_NUMBER_STEP_START_AT;
        if (has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_START_AT)) {
            return call_attribute(machine, frame,
                                  (attribute_lowering){.kind = SXML_XSLT_AST_ATTRIBUTE_START_AT,
                                                       .atom = &locals->start_at});
        }
        return format_numbers(machine, frame);
    case SXML_NUMBER_STEP_START_AT:
        frame->step = SXML_NUMBER_STEP_NOT_A_NUMBER;
        return call_number_text(
            machine, frame,
            &(number_text){.text = start_at_text,
                           .variables = {{.local_name = "start", .atom = locals->start_at},
                                         {.local_name = "numbers", .atom = locals->numbers}},
                           .variable_count = 2,
                           .atom = &locals->numbers});
    case SXML_NUMBER_STEP_NOT_A_NUMBER:
        return format_numbers(machine, frame);
    case SXML_NUMBER_STEP_INTEGERS:
        frame->step = SXML_NUMBER_STEP_FORMAT;
        return call_number_text(
            machine, frame,
            &(number_text){.text = compatible_integers_text,
                           .variables = {{.local_name = "numbers", .atom = locals->numbers}},
                           .variable_count = 1,
                           .atom = &locals->numbers});
    case SXML_NUMBER_STEP_FORMAT:
        return finish_number(machine, frame);
    case SXML_NUMBER_STEP_CHOICE:
        return finish_with_expression(
            machine, frame,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                 .node_construction = {.value = locals->formatted}});
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_xpath_lowering_procedure number_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:number"),
    .step = continue_number,
    .locals_size = sizeof(number_locals)};

/* ================================================== xsl:analyze-string */

/*
 * 17.1: the select value coerced to xs:string? (() as ""); its segments in
 * order come from the row analyze-segments (src/xslt_number_internal.h):
 * a non-matching segment as its xs:string and a matching one as its
 * fn:matching-segment-record (substring, position, groups). The regex and
 * flags errors of the row are XSLT's (XTDE1140, XTDE1145).
 */
static const char input_text[] =
    "string(let $value as xs:string? := " NUMBER_VARIABLE "select return $value)";

static const sxml_xslt_error_mapping regex_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("FORX0002"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE1140"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "The regex of xsl:analyze-string is not a valid regular expression (XSLT 4.0, 17.1)")},
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("FORX0001"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE1145"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "The flags of xsl:analyze-string are not valid (XSLT 4.0, 17.1)")},
};

/* The string of a segment, and whether it matched. */
static const char segment_string_text[] =
    "if (" NUMBER_VARIABLE "segment instance of xs:string) then " NUMBER_VARIABLE "segment "
    "else " NUMBER_VARIABLE "segment?substring";
static const char segment_matches_text[] =
    "not(" NUMBER_VARIABLE "segment instance of xs:string)";

/* 17.2.1: the current captured groups, group 0 (the match) first. */
static const char captured_groups_text[] =
    "Q{http://www.w3.org/2005/xpath-functions/map}merge(("
    "Q{http://www.w3.org/2005/xpath-functions/map}entry(0, map{'value': " NUMBER_VARIABLE
    "segment?substring, 'position': " NUMBER_VARIABLE "segment?position, 'nr': 0}), "
    NUMBER_VARIABLE "segment?groups))";

/* The steps of xsl:analyze-string (frame->step). */
typedef enum analyze_step {
    SXML_ANALYZE_STEP_START,
    SXML_ANALYZE_STEP_REGEX,
    SXML_ANALYZE_STEP_FLAGS,
    SXML_ANALYZE_STEP_SEGMENTS,
    SXML_ANALYZE_STEP_SEGMENT_STRING,
    SXML_ANALYZE_STEP_SEGMENT_MATCHES,
    SXML_ANALYZE_STEP_CAPTURED_GROUPS,
    SXML_ANALYZE_STEP_MATCHING,
    SXML_ANALYZE_STEP_NON_MATCHING,
    SXML_ANALYZE_STEP_FINISH,
} analyze_step;

typedef struct analyze_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom select;
    sxml_ir_atom regex;
    sxml_ir_atom flags;
    sxml_ir_atom input;
    sxml_ir_atom segments;
    /* The MAP's focus (the segment) and the children's: the segment's
       string, with the MAP's position and size. */
    sxml_xpath_lowering_focus segment_focus;
    sxml_xpath_lowering_focus string_focus;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom segment;
    sxml_ir_atom matches;
    sxml_ir_atom string;
    sxml_ir_atom captured_groups;
    size_t scope_mark;
    sxml_xpath_lowering_block branch_blocks[2];
    sxml_ir_atom branch_values[2];
} analyze_locals;

/* 17.1: XTSE1130, XTSE1132, XTSE3185. */
static sxml_status check_analyze_string(const sxml_xslt_lowering *lowering,
                                        sxml_xslt_ast_node_index node) {
    const char *error = NULL;
    const bool has_regex = has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_REGEX);
    const bool has_compiled = has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COMPILED_REGEX);
    if (first_child_of(lowering, (child_query){.node = node,
                                               .kind = SXML_XSLT_AST_KIND_MATCHING_SUBSTRING})
                .value == 0 &&
        first_child_of(lowering, (child_query){.node = node,
                                               .kind = SXML_XSLT_AST_KIND_NON_MATCHING_SUBSTRING})
                .value == 0) {
        error = "XTSE1130 xsl:analyze-string has neither xsl:matching-substring nor "
                "xsl:non-matching-substring (XSLT 4.0, 17.1)";
    } else if (has_regex == has_compiled ||
               (has_compiled && has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_FLAGS))) {
        error = "XTSE1132 xsl:analyze-string needs either regex (and flags) or compiled-regex "
                "(XSLT 4.0, 17.1)";
    }
    for (sxml_xslt_ast_node_index child = node_of(lowering, node)->first_child;
         error == NULL && child.value != 0; child = node_of(lowering, child)->next_sibling) {
        const sxml_xslt_ast_kind kind = node_of(lowering, child)->kind;
        if ((kind == SXML_XSLT_AST_KIND_MATCHING_SUBSTRING ||
             kind == SXML_XSLT_AST_KIND_NON_MATCHING_SUBSTRING) &&
            has_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_SELECT) &&
            node_of(lowering, child)->first_child.value != 0) {
            error = "XTSE3185 xsl:matching-substring or xsl:non-matching-substring has both a "
                    "select attribute and content (XSLT 4.0, 17.1)";
        }
    }
    if (error == NULL) {
        return SXML_STATUS_OK;
    }
    return sxml_xslt_lowering_report_static_error(
        lowering, (sxml_xslt_static_error){.node = node,
                                           .code_and_description =
                                               sxml_byte_view_from_c_string(error)});
}

/* The segments: analyze-segments($input, $regex, $flags), its errors
   mapped, bound in block. */
static sxml_status bind_segments(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_block *block,
                                 analyze_locals *locals) {
    const sxml_ir_atom arguments[3] = {locals->input, locals->regex, locals->flags};
    sxml_ir_atom_list argument_list = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(
        lowerer->unit, (sxml_ir_atom_view){.atoms = arguments, .atom_count = 3}, &argument_list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_bind_mapped_expression(
        lowerer,
        &(sxml_xslt_mapped_expression){
            .block = block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                           .static_call = {.target = {.target_kind =
                                                          SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                      .native_function =
                                                          &sxml_xslt_analyze_segments_function},
                                           .arguments = argument_list}},
            .mappings = {.mappings = regex_mappings,
                         .mapping_count = sizeof(regex_mappings) / sizeof(regex_mappings[0])}},
        &locals->segments);
}

/* Pushes a text with one variable, the segment or the select value. */
typedef struct analyze_text {
    const char *text;
    text_variable variables[TEXT_VARIABLE_MAXIMUM_COUNT];
    size_t variable_count;
    sxml_xslt_error_mapping_view mappings;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
} analyze_text;

static sxml_status call_analyze_text(sxml_xpath_lowering_machine *machine,
                                     const analyze_locals *locals, const analyze_text *text) {
    return call_text(machine, &(text_call){.lowering = locals->head.lowering,
                                           .text = text->text,
                                           .variables = text->variables,
                                           .variable_count = text->variable_count,
                                           .mappings = text->mappings,
                                           .block = text->block,
                                           .atom = text->atom});
}

/* The name of the current captured groups' variable, Q{XSLT}regex-groups
   (glossary `context_variable`). */
static sxml_status intern_captured_groups_name(sxml_xpath_lowerer *lowerer,
                                               sxml_name_identifier *name) {
    sxml_namespace_uri_identifier namespace_uri = {0};
    const sxml_status status = sxml_name_pool_intern_namespace_uri(
        lowerer->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/1999/XSL/Transform"),
        &namespace_uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_pool_intern_name(
        lowerer->name_pool,
        &(sxml_name_request){.namespace_uri = namespace_uri,
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("regex-groups")},
        name);
}

/* One entry an instruction puts in the tunnel parameters it passes. */
typedef struct tunnel_entry {
    sxml_name_identifier name;
    sxml_ir_atom value;
} tunnel_entry;

static sxml_status put_tunnel_entry(sxml_xpath_lowerer *lowerer,
                                    const sxml_xslt_dynamic_context_pass *pass,
                                    tunnel_entry entry) {
    sxml_item key = {.representation = 0};
    sxml_status status = sxml_atomic_item_make(
        lowerer->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = entry.name,
                                                  .namespace_prefix = {.index = 0}}}}},
        &key);
    sxml_ir_atom key_atom = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(lowerer, key, &key_atom);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_atom arguments[3] = {*pass->tunnel_parameters, key_atom, entry.value};
    return sxml_xslt_lowering_bind_library_call(
        lowerer,
        &(sxml_xslt_library_call){.block = pass->block,
                                  .function_namespace = SXML_NAMESPACE_MAP_FUNCTIONS,
                                  .local_name = SXML_BYTE_VIEW_FROM_LITERAL("put"),
                                  .arguments = {.atoms = arguments, .atom_count = 3}},
        pass->tunnel_parameters);
}

sxml_status sxml_xslt_lowering_pass_dynamic_context(sxml_xpath_lowerer *lowerer,
                                                    const sxml_xslt_dynamic_context_pass *pass) {
    sxml_name_identifier name = {0};
    sxml_status status = intern_captured_groups_name(lowerer, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Only an instruction of the function being lowered sets them; a
       template function passes on what it received unchanged. */
    sxml_xpath_context_variable_lookup lookup;
    sxml_xpath_lowerer_find_context_variable(lowerer, name, &lookup);
    if (lookup.is_in_scope) {
        status = put_tunnel_entry(lowerer, pass,
                                  (tunnel_entry){.name = name, .value = lookup.atom});
    }
    if (status != SXML_STATUS_OK || !sxml_xpath_lowerer_is_output_uri_cleared(lowerer)) {
        return status;
    }
    /* 26.3.1: the current output URI stays cleared in the templates a
       stylesheet function or a global calls (fn:current-output-uri reads
       the mark, src/xpath_lower.c). */
    sxml_name_identifier output_uri_name = {0};
    status = sxml_name_pool_intern_name(
        lowerer->name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_XSLT},
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("current-output-uri")},
        &output_uri_name);
    sxml_ir_atom mark = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_true(), &mark);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return put_tunnel_entry(lowerer, pass,
                            (tunnel_entry){.name = output_uri_name, .value = mark});
}

/* Puts the current captured groups (the variable Q{XSLT}regex-groups,
   glossary `context_function`) in scope with value. */
static sxml_status push_captured_groups(sxml_xpath_lowerer *lowerer, sxml_ir_atom value) {
    sxml_name_identifier name = {0};
    const sxml_status status = intern_captured_groups_name(lowerer, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_push_variable(
        lowerer, (sxml_xpath_lowering_scope_entry){
                     .variable_name = name,
                     .function_level = lowerer->function->function_level,
                     .atom = value});
}

/* Lowers the child of kind (its select or its sequence constructor) in
   its branch block (which the caller started) with the string focus; ()
   when the child is absent. */
typedef struct branch_lowering {
    sxml_xslt_ast_kind kind;
    size_t branch_index;
} branch_lowering;

static sxml_status lower_branch(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame, branch_lowering request) {
    analyze_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_xpath_lowering_block *block = &locals->branch_blocks[request.branch_index];
    sxml_ir_atom *value = &locals->branch_values[request.branch_index];
    const sxml_xslt_ast_node_index child =
        first_child_of(lowering, (child_query){.node = locals->head.node, .kind = request.kind});
    if (child.value == 0) {
        return add_empty_atom(machine->state, value);
    }
    const sxml_xslt_ast_attribute *select =
        find_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_SELECT);
    if (select->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = block,
                                                  .atom = value,
                                                  .focus = &locals->string_focus});
    }
    return sxml_xslt_lowering_call_sequence_constructor(
        machine,
        &(sxml_xslt_sequence_constructor_call){
            .lowering = lowering,
            .first = sxml_xslt_lowering_first_constructor_child(lowering->ast, child),
            .block = block,
            .is_body = false,
            .destination = {.expression = NULL, .atom = value, .atom_block = block, .focused = NULL},
            .focus = &locals->string_focus});
}

/* The MAP over the segments, as the frame's result. */
static sxml_status finish_analyze_string(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    analyze_locals *locals = frame->locals;
    sxml_ir_expression_index branches[2] = {{0}, {0}};
    sxml_status status = SXML_STATUS_OK;
    for (size_t branch_index = 0; status == SXML_STATUS_OK && branch_index < 2;
         branch_index += 1) {
        status = finish_branch(lowerer,
                               (branch_result){.block = &locals->branch_blocks[branch_index],
                                               .value = locals->branch_values[branch_index]},
                               &branches[branch_index]);
    }
    sxml_ir_atom chosen = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = &locals->body_block,
                                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                               .conditional = {.condition = locals->matches,
                                                               .then_branch = branches[0],
                                                               .else_branch = branches[1]}}},
                      &chosen);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, (branch_result){.block = &locals->body_block, .value = chosen},
                               &body);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The children's position and size are the MAP's (17.1). */
    const sxml_ir_focus focus = {.context_value = locals->segment_focus.slots.context_value,
                                 .context_position = locals->string_focus.slots.context_position,
                                 .context_size = locals->string_focus.slots.context_size};
    return finish_with_expression(
        machine, frame,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                             .map = {.input = locals->segments,
                                     .focus = focus,
                                     .body = body,
                                     .flags = 0}});
}

static sxml_status continue_analyze_string(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    analyze_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index node = locals->head.node;
    sxml_status status = SXML_STATUS_OK;
    switch ((analyze_step)frame->step) {
    case SXML_ANALYZE_STEP_START: {
        status = check_analyze_string(lowering, node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COMPILED_REGEX)) {
            return sxml_xslt_lowering_report_unsupported(
                lowering, (sxml_xslt_static_error){
                              .node = node,
                              .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                  "the compiled-regex attribute of xsl:analyze-string")});
        }
        frame->step = SXML_ANALYZE_STEP_REGEX;
        const sxml_xslt_ast_attribute *select =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT);
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->select,
                                                  .focus = NULL});
    }
    case SXML_ANALYZE_STEP_REGEX: {
        frame->step = SXML_ANALYZE_STEP_FLAGS;
        const sxml_xslt_ast_attribute *regex =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_REGEX);
        return sxml_xslt_lowering_call_value_template(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = regex->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->regex,
                                                  .focus = NULL});
    }
    case SXML_ANALYZE_STEP_FLAGS: {
        frame->step = SXML_ANALYZE_STEP_SEGMENTS;
        const sxml_xslt_ast_attribute *flags =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_FLAGS);
        if (flags->kind == SXML_XSLT_AST_ATTRIBUTE_NONE) {
            return sxml_xpath_lowerer_add_string_constant(
                lowerer, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &locals->flags);
        }
        return sxml_xslt_lowering_call_value_template(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = flags->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->flags,
                                                  .focus = NULL});
    }
    case SXML_ANALYZE_STEP_SEGMENTS:
        frame->step = SXML_ANALYZE_STEP_SEGMENT_STRING;
        return call_analyze_text(
            machine, locals,
            &(analyze_text){.text = input_text,
                            .variables = {{.local_name = "select", .atom = locals->select}},
                            .variable_count = 1,
                            .mappings = no_mappings,
                            .block = frame->block,
                            .atom = &locals->input});
    case SXML_ANALYZE_STEP_SEGMENT_STRING:
        frame->step = SXML_ANALYZE_STEP_SEGMENT_MATCHES;
        status = bind_segments(lowerer, frame->block, locals);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->segment_focus =
            (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
        locals->body_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
        status = read_focus_value(
            lowerer,
            (focus_value_read){.focus = &locals->segment_focus, .block = &locals->body_block},
            &locals->segment);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return call_analyze_text(
            machine, locals,
            &(analyze_text){.text = segment_string_text,
                            .variables = {{.local_name = "segment", .atom = locals->segment}},
                            .variable_count = 1,
                            .mappings = no_mappings,
                            .block = &locals->body_block,
                            .atom = &locals->string});
    case SXML_ANALYZE_STEP_SEGMENT_MATCHES: {
        frame->step = SXML_ANALYZE_STEP_CAPTURED_GROUPS;
        /* The children's context item, in a slot of its own. */
        sxml_ir_slot string_slot = {0};
        status = bind_in_slot(lowerer,
                              (binding){.block = &locals->body_block,
                                        .expression = atom_expression(locals->string)},
                              &string_slot);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->string_focus = (sxml_xpath_lowering_focus){
            .kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
            .slots = {.context_value = string_slot,
                      .context_position = {.value = 0},
                      .context_size = {.value = 0}}};
        return call_analyze_text(
            machine, locals,
            &(analyze_text){.text = segment_matches_text,
                            .variables = {{.local_name = "segment", .atom = locals->segment}},
                            .variable_count = 1,
                            .mappings = no_mappings,
                            .block = &locals->body_block,
                            .atom = &locals->matches});
    }
    case SXML_ANALYZE_STEP_CAPTURED_GROUPS:
        frame->step = SXML_ANALYZE_STEP_MATCHING;
        locals->branch_blocks[0] = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
        return call_analyze_text(
            machine, locals,
            &(analyze_text){.text = captured_groups_text,
                            .variables = {{.local_name = "segment", .atom = locals->segment}},
                            .variable_count = 1,
                            .mappings = no_mappings,
                            .block = &locals->branch_blocks[0],
                            .atom = &locals->captured_groups});
    case SXML_ANALYZE_STEP_MATCHING: {
        frame->step = SXML_ANALYZE_STEP_NON_MATCHING;
        locals->scope_mark = lowerer->scope_entry_count;
        status = push_captured_groups(lowerer, locals->captured_groups);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return lower_branch(machine, frame,
                            (branch_lowering){.kind = SXML_XSLT_AST_KIND_MATCHING_SUBSTRING,
                                              .branch_index = 0});
    }
    case SXML_ANALYZE_STEP_NON_MATCHING: {
        frame->step = SXML_ANALYZE_STEP_FINISH;
        sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
        /* 17.2.1: no captured groups while a non-matching segment is
           processed. */
        sxml_ir_atom no_groups = {0};
        status = add_empty_atom(lowerer, &no_groups);
        if (status == SXML_STATUS_OK) {
            status = push_captured_groups(lowerer, no_groups);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->branch_blocks[1] = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
        return lower_branch(machine, frame,
                            (branch_lowering){.kind = SXML_XSLT_AST_KIND_NON_MATCHING_SUBSTRING,
                                              .branch_index = 1});
    }
    case SXML_ANALYZE_STEP_FINISH:
        sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
        return finish_analyze_string(machine, frame);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_xpath_lowering_procedure analyze_string_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:analyze-string"),
    .step = continue_analyze_string,
    .locals_size = sizeof(analyze_locals)};

/* ================================================================ Rules */

static const sxml_xslt_instruction_rule number_rules[] = {
    {.kind = SXML_XSLT_AST_KIND_NUMBER, .procedure = &number_procedure},
    {.kind = SXML_XSLT_AST_KIND_ANALYZE_STRING, .procedure = &analyze_string_procedure},
};

const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_number = {
    .rules = number_rules, .rule_count = sizeof(number_rules) / sizeof(number_rules[0])};
