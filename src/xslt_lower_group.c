/*
 * XSLT 4.0 sorting and grouping (src/xslt_lower_internal.h; XSLT 4.0,
 * sections 13 and 14; packet 4.9, D152): sort key specifications under
 * xsl:for-each, xsl:apply-templates, xsl:perform-sort and
 * xsl:for-each-group, xsl:perform-sort, xsl:for-each-group.
 *
 * Golden file: src/xslt_lower_control.c (instructions as procedures whose
 * steps ask the machine for their parts and return, D71; branches in
 * blocks of their own).
 *
 * Nothing new runs: a sort key specification is the evaluator's SORT, one
 * key per xsl:sort (composite, its direction and collation computed when
 * evaluated, D152), the same machinery XQuery's order by uses; grouping is
 * a MAP over the population that makes one grouping pair per item, the
 * internal row of src/xslt_group.c over the pairs (for group-by the
 * XQuery group-by row, D137), an optional SORT of the groups and a MAP
 * over them whose body is the sequence constructor. The glue between them
 * (data-type conversions, checks of attribute values computed at run
 * time, the atomized grouping keys) is short XPath texts lowered through
 * the embedding entry with their inputs as variables in a namespace no
 * stylesheet can write, as src/xslt_lower_template.c lowers built-in
 * templates.
 *
 * The current group and grouping key are variables in the XSLT namespace
 * (Q{XSLT}current-group, Q{XSLT}current-grouping-key) in scope in the
 * sequence constructor and the xsl:sort keys of xsl:for-each-group;
 * fn:current-group() and fn:current-grouping-key() read them (src/
 * xpath_lower.c, glossary `context_function`): lexically, so a template
 * called from the body sees no current group (XTDE1061 there).
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_lower_internal.h"

#include "function_library_core_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xslt_group_internal.h"
#include "xslt_pattern_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* The namespace of the variables the XPath texts below read. */
#define GROUP_VARIABLE "$Q{urn:x-sxml:group-variable}"

/* ============================================================ AST reads */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

static sxml_xslt_ast_node_index next_of(const sxml_xslt_lowering *lowering,
                                        sxml_xslt_ast_node_index node) {
    return node_of(lowering, node)->next_sibling;
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
        child = next_of(lowering, child);
    }
    return child;
}

/* The next sibling of node of its kind; 0 when none. */
static sxml_xslt_ast_node_index next_like(const sxml_xslt_lowering *lowering,
                                          sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_kind kind = node_of(lowering, node)->kind;
    sxml_xslt_ast_node_index child = next_of(lowering, node);
    while (child.value != 0 && node_of(lowering, child)->kind != kind) {
        child = next_of(lowering, child);
    }
    return child;
}

/* The number of children of node of kind. */
static size_t count_children(const sxml_xslt_lowering *lowering, child_query query) {
    size_t count = 0;
    for (sxml_xslt_ast_node_index child = first_child_of(lowering, query); child.value != 0;
         child = next_like(lowering, child)) {
        count += 1;
    }
    return count;
}

bool sxml_xslt_lowering_has_sort(const sxml_xslt_lowering *lowering,
                                 sxml_xslt_ast_node_index node) {
    return first_child_of(lowering, (child_query){.node = node, .kind = SXML_XSLT_AST_KIND_SORT})
               .value != 0;
}

/* An attribute value template's text when it has no enclosed expression:
   is_fixed and the text (empty for ""); is_fixed false otherwise. */
typedef struct fixed_text {
    bool is_fixed;
    sxml_byte_view text;
} fixed_text;

static fixed_text read_fixed_text(const sxml_xslt_lowering *lowering,
                                  const sxml_xslt_ast_attribute *attribute) {
    const fixed_text computed = {.is_fixed = false, .text = {.bytes = NULL, .byte_count = 0}};
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
        return computed;
    }
    const sxml_ast *ast =
        sxml_xslt_ast_get_expression(lowering->ast, attribute->value.payload.expression)->ast;
    if (ast == NULL) {
        return computed;
    }
    const sxml_ast_node_index body = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = ast->root, .field = SXML_AST_FIELD_BODY});
    const sxml_ast_node_index part = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = body, .field = SXML_AST_FIELD_PART});
    if (part.value == 0) {
        return (fixed_text){.is_fixed = true, .text = {.bytes = NULL, .byte_count = 0}};
    }
    const sxml_ast_node *part_node = sxml_ast_get_node(ast, part);
    if (part_node->kind != SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART ||
        sxml_ast_find_next_with_same_field(ast, part).value != 0) {
        return computed;
    }
    return (fixed_text){.is_fixed = true, .text = sxml_ast_get_text(ast, part_node->payload.text)};
}

static bool is_xml_whitespace(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* text without leading and trailing XML whitespace. */
static sxml_byte_view trim_whitespace(sxml_byte_view text) {
    size_t start = 0;
    size_t end = text.byte_count;
    while (start < end && is_xml_whitespace(text.bytes[start])) {
        start += 1;
    }
    while (end > start && is_xml_whitespace(text.bytes[end - 1])) {
        end -= 1;
    }
    return (sxml_byte_view){.bytes = text.bytes == NULL ? NULL : text.bytes + start,
                            .byte_count = end - start};
}

static bool text_equals(sxml_byte_view text, sxml_byte_view literal) {
    return sxml_byte_view_equals(trim_whitespace(text), literal);
}

/* Whether text, trimmed, is the string literal. */
#define TEXT_IS(text, literal) text_equals((text), SXML_BYTE_VIEW_FROM_LITERAL(literal))

static bool is_ascii_letter(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

static bool is_ascii_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

/* Whether text is an xs:language or empty: [a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*
   (XSD 1.1 Part 2, 3.4.3; xsl:sort lang, 13.1.3). */
static bool is_language(sxml_byte_view text) {
    text = trim_whitespace(text);
    size_t run = 0;
    bool is_first_run = true;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '-') {
            if (run == 0) {
                return false;
            }
            run = 0;
            is_first_run = false;
            continue;
        }
        if (!is_ascii_letter(byte) && (is_first_run || !is_ascii_digit(byte))) {
            return false;
        }
        run += 1;
        if (run > 8) {
            return false;
        }
    }
    return text.byte_count == 0 || run > 0;
}

/* Whether text is an xs:boolean's lexical form (stable, 13.1). */
static bool is_boolean_text(sxml_byte_view text) {
    return TEXT_IS(text, "yes") || TEXT_IS(text, "no") || TEXT_IS(text, "true") ||
           TEXT_IS(text, "false") || TEXT_IS(text, "1") || TEXT_IS(text, "0");
}

/* Whether text names a data type other than text and number: an EQName
   with a namespace (13.1.2: implementation-defined, here no conversion). */
static bool is_data_type_name(sxml_byte_view text) {
    text = trim_whitespace(text);
    if (text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{') {
        return true;
    }
    return memchr(text.bytes == NULL ? "" : (const void *)text.bytes, ':', text.byte_count) != NULL;
}

/* =========================================================== IR pieces */

static sxml_status add_expression(sxml_xpath_lowerer *lowerer, sxml_ir_expression expression,
                                  sxml_ir_expression_index *index) {
    return sxml_xpath_lowerer_add_expression(
        lowerer,
        (sxml_xpath_lowering_expression_to_add){.node = no_ast_node, .expression = expression},
        index);
}

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

static sxml_ir_atom slot_atom(sxml_ir_slot slot) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = slot};
}

/* Ends frame with atom as its value. */
static sxml_status finish_with_atom(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame, sxml_ir_atom atom) {
    sxml_ir_expression_index result = {0};
    const sxml_status status = add_expression(machine->state, atom_expression(atom), &result);
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
    const sxml_status status = add_expression(lowerer, atom_expression(branch.value), &result);
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

/* LOOKUP of member member_number of an array atom, bound in block. */
typedef struct member_read {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom array;
    int64_t member_number;
} member_read;

static sxml_status bind_member(sxml_xpath_lowerer *lowerer, member_read request,
                               sxml_ir_atom *atom) {
    sxml_ir_atom number = {0};
    const sxml_status status = sxml_xpath_lowerer_add_item_constant(
        lowerer, sxml_atomic_item_from_integer(request.member_number), &number);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(lowerer,
                (binding){.block = request.block,
                          .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LOOKUP,
                                         .lookup = {.value = request.array, .key = number}}},
                atom);
}

static const sxml_xslt_error_mapping sort_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE1030"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "Two sort key values are not comparable (XSLT 4.0, 13.1.2)")},
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("FOCH0002"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE1035"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "The collation of xsl:sort is not one sxml recognizes (XSLT 4.0, 13.1.3)")},
};

static const sxml_xslt_error_mapping grouping_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("FOCH0002"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE1110"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "The collation of xsl:for-each-group is not one sxml recognizes (XSLT 4.0, 14.1)")},
};

/* Written by a step that may push a part: whether it did. */
typedef struct step_outcome {
    bool is_pushed;
} step_outcome;

/* Written by add_fixed_collation: whether the collation is a constant. */
typedef struct collation_fixing {
    bool is_fixed;
} collation_fixing;

/* =============================================================== Scope */

/* Puts the variable Q{XSLT}local_name in scope with atom's value. */
static sxml_status push_xslt_variable(sxml_xpath_lowerer *lowerer, sxml_byte_view local_name,
                                      sxml_ir_atom atom) {
    sxml_name_identifier name = {0};
    const sxml_status status = sxml_name_pool_intern_name(
        lowerer->name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_XSLT},
                             .local_name = local_name},
        &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_push_variable(
        lowerer, (sxml_xpath_lowering_scope_entry){
                     .variable_name = name,
                     .function_level = lowerer->function->function_level,
                     .atom = atom});
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

/* One variable of an XPath text: Q{urn:x-sxml:group-variable}local_name. */
typedef struct text_variable {
    const char *local_name;
    sxml_ir_atom atom;
} text_variable;

/* An XPath text of this file to lower in block with its variables. */
typedef struct text_locals {
    const sxml_xslt_lowering *lowering;
    const char *text;
    text_variable variables[TEXT_VARIABLE_MAXIMUM_COUNT];
    size_t variable_count;
    sxml_xpath_lowering_block *block;
    size_t scope_mark;
    sxml_ir_atom value;
} text_locals;

static sxml_status continue_text(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    text_locals *locals = frame->locals;
    if (frame->step == 1) {
        sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
        return finish_with_atom(machine, frame, locals->value);
    }
    frame->step = 1;
    sxml_ast *ast = NULL;
    sxml_status status =
        parse_kept(locals->lowering, sxml_byte_view_from_c_string(locals->text), &ast);
    sxml_namespace_uri_identifier namespace_uri = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            lowerer->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("urn:x-sxml:group-variable"),
            &namespace_uri);
    }
    locals->scope_mark = lowerer->scope_entry_count;
    for (size_t variable_index = 0; status == SXML_STATUS_OK && variable_index < locals->variable_count;
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
    /* The text names only fn functions and EQNames: the lowerer's static
       context serves. */
    return sxml_xpath_lowering_call_embedded(
        machine, &(sxml_xpath_lowering_embedded_call){
                     .embedding = {.ast = ast,
                                   .node = {.value = 0},
                                   .static_context = NULL,
                                   .focus = NULL},
                     .block = locals->block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = &locals->value,
                                     .atom_block = locals->block,
                                     .focused = NULL}});
}

static const sxml_xpath_lowering_procedure text_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("XSLT grouping text"),
    .step = continue_text,
    .locals_size = sizeof(text_locals)};

/* From a step: lowers text with variables in block, the value to *atom. */
typedef struct text_call {
    const sxml_xslt_lowering *lowering;
    const char *text;
    const text_variable *variables;
    size_t variable_count;
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

#define XTDE0030_CODE "QName('http://www.w3.org/2005/xqt-errors', 'XTDE0030')"

/* 13.1: order is "ascending" or "descending" (XTDE0030). */
static const char order_text[] =
    "let $o := normalize-space(" GROUP_VARIABLE "order) return "
    "if ($o = ('ascending', 'descending')) then $o else error(" XTDE0030_CODE
    ", 'The order of xsl:sort is not ascending or descending (XSLT 4.0, 13.1)')";

/* 13.1.3: the collation lang and case-order ask for (a UCA collation with
   fallback, D116), or none (the codepoint collation). */
static const char collation_text[] =
    "let $l := normalize-space(" GROUP_VARIABLE "lang), "
    "$c := normalize-space(" GROUP_VARIABLE "case) return "
    "if (not($l = '' or matches($l, '^[a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*$'))) then error(" XTDE0030_CODE
    ", 'The lang of xsl:sort is not a language (XSLT 4.0, 13.1.3)') "
    "else if (not($c = ('', 'upper-first', 'lower-first'))) then error(" XTDE0030_CODE
    ", 'The case-order of xsl:sort is not upper-first or lower-first (XSLT 4.0, 13.1.3)') "
    "else if ($l = '' and $c = '') then 'http://www.w3.org/2005/xpath-functions/collation/codepoint' "
    "else string-join(('http://www.w3.org/2013/collation/UCA?fallback=yes', "
    "(if ($l ne '') then 'lang=' || $l else ()), "
    "(if ($c eq 'upper-first') then 'caseFirst=upper' "
    "else if ($c eq 'lower-first') then 'caseFirst=lower' else ())), ';')";

/* 13.1.2: data-type="number" and "text" convert the atomized key. */
static const char number_key_text[] = "data(" GROUP_VARIABLE "key) ! number(.)";
static const char string_key_text[] = "data(" GROUP_VARIABLE "key) ! string(.)";
static const char data_type_key_text[] =
    "let $t := normalize-space(" GROUP_VARIABLE "type) return "
    "if ($t eq 'number') then data(" GROUP_VARIABLE "key) ! number(.) "
    "else if ($t eq 'text') then data(" GROUP_VARIABLE "key) ! string(.) "
    "else if (contains($t, ':') or starts-with($t, 'Q{')) then " GROUP_VARIABLE "key "
    "else error(" XTDE0030_CODE ", 'The data-type of xsl:sort is not text, number or a QName "
    "(XSLT 4.0, 13.1.2)')";

/* 14.1: grouping keys, atomized with xs:untypedAtomic cast to xs:string;
   for group-by without composite, the distinct ones. */
#define ATOMIZED_KEY_TEXT                                                                          \
    "(data(" GROUP_VARIABLE "key) ! (if (. instance of "                                           \
    "Q{http://www.w3.org/2001/XMLSchema}untypedAtomic) then string(.) else .))"
static const char grouping_key_text[] = ATOMIZED_KEY_TEXT;
static const char distinct_key_text[] =
    "let $c := " GROUP_VARIABLE "collation return "
    "if ($c eq '') then distinct-values(" ATOMIZED_KEY_TEXT ") "
    "else distinct-values(" ATOMIZED_KEY_TEXT ", $c)";

/* ========================================================== Group focus */

/* The bindings of one group array in block: its current group, grouping
   key and initial item, and a focus of the initial item with the
   position and size of slots (14.1, 13.1.1). */
typedef struct group_view {
    sxml_ir_atom group;
    sxml_ir_atom key;
    sxml_ir_atom initial_item;
    sxml_xpath_lowering_focus focus;
} group_view;

typedef struct group_view_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_focus slots;
} group_view_request;

static sxml_status bind_group_view(const sxml_xslt_lowering *lowering, group_view_request request,
                                   group_view *view) {
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    const sxml_ir_atom group_array = slot_atom(request.slots.context_value);
    sxml_status status = bind_member(
        lowerer, (member_read){.block = request.block, .array = group_array, .member_number = 1},
        &view->group);
    if (status == SXML_STATUS_OK) {
        status = bind_member(
            lowerer,
            (member_read){.block = request.block, .array = group_array, .member_number = 2},
            &view->key);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_library_call(
            lowerer,
            &(sxml_xslt_library_call){.block = request.block,
                                      .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                      .local_name = SXML_BYTE_VIEW_FROM_LITERAL("head"),
                                      .arguments = {.atoms = &view->group, .atom_count = 1}},
            &view->initial_item);
    }
    if (status == SXML_STATUS_OK && view->initial_item.atom_kind != SXML_IR_ATOM_KIND_SLOT) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        view->focus = (sxml_xpath_lowering_focus){
            .kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
            .slots = {.context_value = view->initial_item.slot,
                      .context_position = request.slots.context_position,
                      .context_size = request.slots.context_size}};
    }
    return status;
}

/* Puts the current group and the current grouping key in scope; without
   has_key the grouping key is absent (bound to NONE, 14.2.2), hiding an
   outer one. */
typedef struct group_scope {
    const group_view *view;
    bool has_key;
} group_scope;

static sxml_status push_group_scope(sxml_xpath_lowerer *lowerer, group_scope scope) {
    sxml_status status = push_xslt_variable(
        lowerer, SXML_BYTE_VIEW_FROM_LITERAL("current-group"), scope.view->group);
    if (status == SXML_STATUS_OK) {
        status = push_xslt_variable(
            lowerer, SXML_BYTE_VIEW_FROM_LITERAL("current-grouping-key"),
            scope.has_key ? scope.view->key : (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE});
    }
    return status;
}

/* Three fresh slots as a focus. */
static sxml_status add_focus_slots(sxml_xpath_lowerer *lowerer, sxml_ir_focus *slots) {
    sxml_status status = sxml_xpath_lowerer_add_slot(lowerer, &slots->context_value);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(lowerer, &slots->context_position);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(lowerer, &slots->context_size);
    }
    return status;
}

/* ============================================================== xsl:sort */

/* The attribute value templates of xsl:sort, evaluated with the focus of
   the instruction (13.1). */
typedef enum sort_attribute {
    SXML_XSLT_SORT_ATTRIBUTE_ORDER,
    SXML_XSLT_SORT_ATTRIBUTE_COLLATION,
    SXML_XSLT_SORT_ATTRIBUTE_LANG,
    SXML_XSLT_SORT_ATTRIBUTE_CASE_ORDER,
    SXML_XSLT_SORT_ATTRIBUTE_DATA_TYPE,
    SXML_XSLT_SORT_ATTRIBUTE_COUNT,
} sort_attribute;

static const sxml_xslt_ast_attribute_kind sort_attribute_kinds[SXML_XSLT_SORT_ATTRIBUTE_COUNT] = {
    [SXML_XSLT_SORT_ATTRIBUTE_ORDER] = SXML_XSLT_AST_ATTRIBUTE_ORDER,
    [SXML_XSLT_SORT_ATTRIBUTE_COLLATION] = SXML_XSLT_AST_ATTRIBUTE_COLLATION,
    [SXML_XSLT_SORT_ATTRIBUTE_LANG] = SXML_XSLT_AST_ATTRIBUTE_LANG,
    [SXML_XSLT_SORT_ATTRIBUTE_CASE_ORDER] = SXML_XSLT_AST_ATTRIBUTE_CASE_ORDER,
    [SXML_XSLT_SORT_ATTRIBUTE_DATA_TYPE] = SXML_XSLT_AST_ATTRIBUTE_DATA_TYPE,
};

/* How a key is converted (13.1.2). */
typedef enum data_type_conversion {
    SXML_XSLT_DATA_TYPE_CONVERSION_NONE,
    SXML_XSLT_DATA_TYPE_CONVERSION_NUMBER,
    SXML_XSLT_DATA_TYPE_CONVERSION_TEXT,
    /* Chosen when evaluated, from the data-type value. */
    SXML_XSLT_DATA_TYPE_CONVERSION_COMPUTED,
} data_type_conversion;

typedef enum sort_phase {
    SXML_XSLT_SORT_PHASE_START,
    SXML_XSLT_SORT_PHASE_ATTRIBUTES,
    SXML_XSLT_SORT_PHASE_ORDER,
    SXML_XSLT_SORT_PHASE_COLLATION,
    SXML_XSLT_SORT_PHASE_KEY,
    SXML_XSLT_SORT_PHASE_CONVERSION,
    SXML_XSLT_SORT_PHASE_KEY_DONE,
} sort_phase;

typedef struct sort_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom input;
    bool sorts_groups;
    uint32_t tuple_key_offset;
    sort_phase phase;
    /* The xsl:sort being lowered and its attribute values. */
    sxml_xslt_ast_node_index sort;
    uint32_t attribute_index;
    bool has_attributes[SXML_XSLT_SORT_ATTRIBUTE_COUNT];
    sxml_ir_atom attribute_values[SXML_XSLT_SORT_ATTRIBUTE_COUNT];
    sxml_ir_sort_direction direction;
    sxml_ir_atom order;
    sxml_ir_atom collation;
    data_type_conversion conversion;
    /* The SORT's focus: slots made on first read, or (sorting groups) the
       group array's slots, with view's focus for the key. */
    sxml_xpath_lowering_focus item_focus;
    group_view view;
    sxml_xpath_lowering_block key_block;
    sxml_ir_atom key;
    size_t scope_mark;
    /* The keys so far (owned_memory[0]). */
    sxml_ir_sort_key *sort_keys;
    size_t sort_key_count;
} sort_locals;

/* 13.1: the static rules of a sort key specification. */
static sxml_status check_sorts(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                               size_t *sort_count) {
    *sort_count = 0;
    const bool is_merge = node_of(lowering, node)->kind == SXML_XSLT_AST_KIND_MERGE_SOURCE;
    for (sxml_xslt_ast_node_index sort_node = first_child_of(
             lowering, (child_query){.node = node,
                                     .kind = is_merge ? SXML_XSLT_AST_KIND_MERGE_KEY
                                                      : SXML_XSLT_AST_KIND_SORT});
         sort_node.value != 0; sort_node = next_like(lowering, sort_node)) {
        const sxml_xslt_ast_attribute *stable =
            find_attribute(lowering, sort_node, SXML_XSLT_AST_ATTRIBUTE_STABLE);
        if (*sort_count > 0 && stable->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
            return sxml_xslt_lowering_report_static_error(
                lowering, (sxml_xslt_static_error){
                              .node = sort_node,
                              .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XTSE1017 only the first xsl:sort may have a stable "
                                  "attribute (XSLT 4.0, 13.1)")});
        }
        if (has_attribute(lowering, sort_node, SXML_XSLT_AST_ATTRIBUTE_SELECT) &&
            node_of(lowering, sort_node)->first_child.value != 0) {
            return sxml_xslt_lowering_report_static_error(
                lowering,
                (sxml_xslt_static_error){
                    .node = sort_node,
                    .code_and_description =
                        is_merge ? SXML_BYTE_VIEW_FROM_LITERAL(
                                       "XTSE3200 an xsl:merge-key with a select attribute has "
                                       "content (XSLT 4.0, 15.4)")
                                 : SXML_BYTE_VIEW_FROM_LITERAL(
                                       "XTSE1015 an xsl:sort with a select attribute has content "
                                       "(XSLT 4.0, 13.1)")});
        }
        const fixed_text stable_text = read_fixed_text(lowering, stable);
        const fixed_text order =
            read_fixed_text(lowering, find_attribute(lowering, sort_node, SXML_XSLT_AST_ATTRIBUTE_ORDER));
        const fixed_text lang =
            read_fixed_text(lowering, find_attribute(lowering, sort_node, SXML_XSLT_AST_ATTRIBUTE_LANG));
        const fixed_text case_order = read_fixed_text(
            lowering, find_attribute(lowering, sort_node, SXML_XSLT_AST_ATTRIBUTE_CASE_ORDER));
        const fixed_text data_type = read_fixed_text(
            lowering, find_attribute(lowering, sort_node, SXML_XSLT_AST_ATTRIBUTE_DATA_TYPE));
        const bool is_invalid =
            (stable_text.is_fixed && !is_boolean_text(stable_text.text)) ||
            (order.is_fixed && !TEXT_IS(order.text, "ascending") &&
             !TEXT_IS(order.text, "descending")) ||
            (lang.is_fixed && !is_language(lang.text)) ||
            (case_order.is_fixed && !TEXT_IS(case_order.text, "upper-first") &&
             !TEXT_IS(case_order.text, "lower-first")) ||
            (data_type.is_fixed && !TEXT_IS(data_type.text, "text") &&
             !TEXT_IS(data_type.text, "number") && !is_data_type_name(data_type.text));
        if (is_invalid) {
            /* 5.6.1: an attribute value template without enclosed
               expressions is checked statically. */
            return sxml_xslt_lowering_report_static_error(
                lowering, (sxml_xslt_static_error){
                              .node = sort_node,
                              .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XTSE0020 an attribute of xsl:sort has a value it does not "
                                  "permit (XSLT 4.0, 13.1)")});
        }
        *sort_count += 1;
    }
    return SXML_STATUS_OK;
}

static sxml_status start_sort(sxml_xpath_lowering_machine *machine,
                              sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    sort_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    size_t sort_count = 0;
    sxml_status status = check_sorts(lowering, locals->head.node, &sort_count);
    if (status == SXML_STATUS_OK && sort_count == 0) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = sort_count,
                            .element_size = sizeof(sxml_ir_sort_key),
                            .element_alignment = alignof(sxml_ir_sort_key)}});
    }
    locals->item_focus = (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
    if (status == SXML_STATUS_OK && locals->sorts_groups) {
        status = add_focus_slots(lowerer, &locals->item_focus.slots);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->sort_keys = frame->owned_memory[0];
    locals->sort = first_child_of(
        lowering,
        (child_query){.node = locals->head.node,
                      .kind = locals->tuple_key_offset != 0 ? SXML_XSLT_AST_KIND_MERGE_KEY
                                                            : SXML_XSLT_AST_KIND_SORT});
    locals->phase = SXML_XSLT_SORT_PHASE_ATTRIBUTES;
    return SXML_STATUS_OK;
}

/* Lowers the next present attribute value template; false when none is
   left. */
static sxml_status lower_next_sort_attribute(sxml_xpath_lowering_machine *machine,
                                             sxml_xpath_lowering_frame *frame,
                                             step_outcome *outcome) {
    sort_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    outcome->is_pushed = false;
    while (locals->attribute_index < SXML_XSLT_SORT_ATTRIBUTE_COUNT) {
        const uint32_t attribute_index = locals->attribute_index;
        locals->attribute_index += 1;
        const sxml_xslt_ast_attribute *attribute =
            find_attribute(lowering, locals->sort, sort_attribute_kinds[attribute_index]);
        locals->has_attributes[attribute_index] =
            attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE;
        if (locals->has_attributes[attribute_index]) {
            outcome->is_pushed = true;
            return sxml_xslt_lowering_call_value_template(
                machine,
                &(sxml_xslt_expression_call){.lowering = lowering,
                                             .expression = attribute->value.payload.expression,
                                             .block = frame->block,
                                             .atom = &locals->attribute_values[attribute_index],
                                             .focus = NULL});
        }
    }
    return SXML_STATUS_OK;
}

/* Reads the fixed attributes: direction and conversion. */
static void read_fixed_sort_attributes(sort_locals *locals) {
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const fixed_text order =
        read_fixed_text(lowering, find_attribute(lowering, locals->sort, SXML_XSLT_AST_ATTRIBUTE_ORDER));
    const fixed_text data_type = read_fixed_text(
        lowering, find_attribute(lowering, locals->sort, SXML_XSLT_AST_ATTRIBUTE_DATA_TYPE));
    locals->direction = order.is_fixed && TEXT_IS(order.text, "descending")
                            ? SXML_IR_SORT_DIRECTION_DESCENDING
                            : SXML_IR_SORT_DIRECTION_ASCENDING;
    locals->conversion = SXML_XSLT_DATA_TYPE_CONVERSION_NONE;
    if (locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_DATA_TYPE]) {
        locals->conversion = !data_type.is_fixed                  ? SXML_XSLT_DATA_TYPE_CONVERSION_COMPUTED
                             : TEXT_IS(data_type.text, "number") ? SXML_XSLT_DATA_TYPE_CONVERSION_NUMBER
                             : TEXT_IS(data_type.text, "text")   ? SXML_XSLT_DATA_TYPE_CONVERSION_TEXT
                                                                     : SXML_XSLT_DATA_TYPE_CONVERSION_NONE;
    }
}

/* A collation URI being written into fixed memory. */
typedef struct collation_uri_text {
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
    bool is_truncated;
} collation_uri_text;

static void append_text(collation_uri_text *text, sxml_byte_view part) {
    if (part.byte_count > text->byte_capacity - text->byte_count) {
        text->is_truncated = true;
        return;
    }
    for (size_t byte_index = 0; byte_index < part.byte_count; byte_index += 1) {
        text->bytes[text->byte_count + byte_index] = part.bytes[byte_index];
    }
    text->byte_count += part.byte_count;
}

/* The collation lang and case-order fix while lowering, as a constant;
   fixing->is_fixed false when one is computed. */
static sxml_status add_fixed_collation(const sort_locals *locals, collation_fixing *fixing,
                                       sxml_ir_atom *atom) {
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const fixed_text lang =
        read_fixed_text(lowering, find_attribute(lowering, locals->sort, SXML_XSLT_AST_ATTRIBUTE_LANG));
    const fixed_text case_order = read_fixed_text(
        lowering, find_attribute(lowering, locals->sort, SXML_XSLT_AST_ATTRIBUTE_CASE_ORDER));
    fixing->is_fixed =
        (!locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_LANG] || lang.is_fixed) &&
        (!locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_CASE_ORDER] || case_order.is_fixed);
    *atom = (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
    if (!fixing->is_fixed) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view language =
        locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_LANG] ? trim_whitespace(lang.text)
                                                    : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    const bool is_upper_first = locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_CASE_ORDER] &&
                                TEXT_IS(case_order.text, "upper-first");
    const bool is_lower_first = locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_CASE_ORDER] &&
                                TEXT_IS(case_order.text, "lower-first");
    if (language.byte_count == 0 && !is_upper_first && !is_lower_first) {
        /* 13.1.3: the implementation-defined default, the codepoint
           collation. */
        return SXML_STATUS_OK;
    }
    /* "http://www.w3.org/2013/collation/UCA?fallback=yes;lang=L;caseFirst=C" */
    uint8_t uri[128];
    collation_uri_text text = {.bytes = uri, .byte_count = 0, .byte_capacity = sizeof(uri)};
    append_text(&text, sxml_byte_view_from_c_string(
                           "http://www.w3.org/2013/collation/UCA?fallback=yes"));
    if (language.byte_count > 0) {
        append_text(&text, sxml_byte_view_from_c_string(";lang="));
        append_text(&text, language);
    }
    if (is_upper_first || is_lower_first) {
        append_text(&text, sxml_byte_view_from_c_string(is_upper_first ? ";caseFirst=upper"
                                                                        : ";caseFirst=lower"));
    }
    if (text.is_truncated) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const size_t byte_count = text.byte_count;
    return sxml_xpath_lowerer_add_string_constant(
        lowering->lowerer, (sxml_byte_view){.bytes = uri, .byte_count = byte_count}, atom);
}

/* Starts the key's block and lowers its value: select, content or "."
   (13.1.1). */
static sxml_status lower_sort_key(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    sxml_xpath_lowerer *lowerer = machine->state;
    sort_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    outcome->is_pushed = false;
    locals->key_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    locals->scope_mark = lowerer->scope_entry_count;
    sxml_xpath_lowering_focus *focus = &locals->item_focus;
    if (locals->sorts_groups) {
        /* 13.1.1 with 14.1: the initial item of each group is the context
           item, and the group the current group. */
        sxml_status status = bind_group_view(
            lowering, (group_view_request){.block = &locals->key_block, .slots = locals->item_focus.slots},
            &locals->view);
        if (status == SXML_STATUS_OK) {
            status = push_group_scope(lowerer, (group_scope){.view = &locals->view,
                                                             .has_key = !has_attribute(
                                                                 lowering, locals->head.node,
                                                                 SXML_XSLT_AST_ATTRIBUTE_GROUP_STARTING_WITH) &&
                                                                 !has_attribute(
                                                                 lowering, locals->head.node,
                                                                 SXML_XSLT_AST_ATTRIBUTE_GROUP_ENDING_WITH)});
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        focus = &locals->view.focus;
    }
    if (locals->tuple_key_offset != 0) {
        /* 15.4: merge keys are computed already, members of the tuple. */
        sxml_ir_atom tuple = {0};
        const sxml_status status = read_focus_value(
            lowerer, (focus_value_read){.focus = focus, .block = &locals->key_block}, &tuple);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return bind_member(lowerer,
                           (member_read){.block = &locals->key_block,
                                         .array = tuple,
                                         .member_number = (int64_t)locals->tuple_key_offset +
                                                          (int64_t)locals->sort_key_count},
                           &locals->key);
    }
    const sxml_xslt_ast_attribute *select =
        find_attribute(lowering, locals->sort, SXML_XSLT_AST_ATTRIBUTE_SELECT);
    if (select->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = &locals->key_block,
                                                  .atom = &locals->key,
                                                  .focus = focus});
    }
    const sxml_xslt_ast_node_index content = node_of(lowering, locals->sort)->first_child;
    if (content.value != 0) {
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = content,
                         .block = &locals->key_block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &locals->key,
                                         .atom_block = &locals->key_block,
                                         .focused = NULL},
                         .focus = focus});
    }
    return read_focus_value(lowerer, (focus_value_read){.focus = focus, .block = &locals->key_block},
                            &locals->key);
}

/* Converts the key as data-type says. */
static sxml_status convert_sort_key(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    sort_locals *locals = frame->locals;
    outcome->is_pushed = locals->conversion != SXML_XSLT_DATA_TYPE_CONVERSION_NONE &&
                         locals->tuple_key_offset == 0;
    if (!outcome->is_pushed) {
        return SXML_STATUS_OK;
    }
    const text_variable variables[2] = {
        {.local_name = "key", .atom = locals->key},
        {.local_name = "type", .atom = locals->attribute_values[SXML_XSLT_SORT_ATTRIBUTE_DATA_TYPE]}};
    const char *text = locals->conversion == SXML_XSLT_DATA_TYPE_CONVERSION_NUMBER ? number_key_text
                       : locals->conversion == SXML_XSLT_DATA_TYPE_CONVERSION_TEXT ? string_key_text
                                                                         : data_type_key_text;
    return call_text(machine,
                     &(text_call){.lowering = locals->head.lowering,
                                  .text = text,
                                  .variables = variables,
                                  .variable_count =
                                      locals->conversion == SXML_XSLT_DATA_TYPE_CONVERSION_COMPUTED ? 2 : 1,
                                  .block = &locals->key_block,
                                  .atom = &locals->key});
}

/* The key is lowered: records it and goes on with the next xsl:sort. */
static sxml_status finish_sort_key(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    sort_locals *locals = frame->locals;
    sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
    sxml_ir_expression_index key = {0};
    const sxml_status status = finish_branch(
        lowerer, (branch_result){.block = &locals->key_block, .value = locals->key}, &key);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* 13.1.2: composite keys, the empty sequence first; 13.1.1: the
       direction reverses the whole order. */
    locals->sort_keys[locals->sort_key_count] = (sxml_ir_sort_key){
        .key = key,
        .direction = locals->direction,
        .empty_order = SXML_IR_EMPTY_ORDER_LEAST,
        .collation = locals->collation,
        .order = locals->order,
        .is_composite = true};
    locals->sort_key_count += 1;
    locals->sort = next_like(locals->head.lowering, locals->sort);
    locals->attribute_index = 0;
    locals->order = (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
    locals->collation = (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
    locals->phase = SXML_XSLT_SORT_PHASE_ATTRIBUTES;
    return SXML_STATUS_OK;
}

/* Every key is lowered: the SORT, its type errors XSLT's. */
static sxml_status finish_sort(sxml_xpath_lowering_machine *machine,
                               sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    sort_locals *locals = frame->locals;
    sxml_ir_sort_key_list keys = {0};
    sxml_status status = sxml_ir_unit_add_sort_keys(
        lowerer->unit,
        (sxml_ir_sort_key_view){.sort_keys = locals->sort_keys,
                                .sort_key_count = locals->sort_key_count},
        &keys);
    sxml_ir_atom sorted = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_mapped_expression(
            lowerer,
            &(sxml_xslt_mapped_expression){
                .block = frame->block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SORT,
                               .sort = {.input = locals->input,
                                        .focus = locals->item_focus.slots,
                                        .keys = keys}},
                .mappings = {.mappings = sort_mappings,
                             .mapping_count = sizeof(sort_mappings) / sizeof(sort_mappings[0])}},
            &sorted);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, sorted);
}

static sxml_status continue_sort(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sort_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        step_outcome pushed = {.is_pushed = false};
        switch (locals->phase) {
        case SXML_XSLT_SORT_PHASE_START:
            status = start_sort(machine, frame);
            break;
        case SXML_XSLT_SORT_PHASE_ATTRIBUTES:
            if (locals->sort.value == 0) {
                return finish_sort(machine, frame);
            }
            status = lower_next_sort_attribute(machine, frame, &pushed);
            if (status == SXML_STATUS_OK && !pushed.is_pushed) {
                read_fixed_sort_attributes(locals);
                locals->phase = SXML_XSLT_SORT_PHASE_ORDER;
            }
            break;
        case SXML_XSLT_SORT_PHASE_ORDER: {
            locals->phase = SXML_XSLT_SORT_PHASE_COLLATION;
            const fixed_text order = read_fixed_text(
                lowering, find_attribute(lowering, locals->sort, SXML_XSLT_AST_ATTRIBUTE_ORDER));
            if (locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_ORDER] && !order.is_fixed) {
                const text_variable variable = {
                    .local_name = "order",
                    .atom = locals->attribute_values[SXML_XSLT_SORT_ATTRIBUTE_ORDER]};
                pushed.is_pushed = true;
                status = call_text(machine, &(text_call){.lowering = lowering,
                                                         .text = order_text,
                                                         .variables = &variable,
                                                         .variable_count = 1,
                                                         .block = frame->block,
                                                         .atom = &locals->order});
            }
            break;
        }
        case SXML_XSLT_SORT_PHASE_COLLATION: {
            locals->phase = SXML_XSLT_SORT_PHASE_KEY;
            if (locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_COLLATION]) {
                /* 13.1.3: lang and case-order are then ignored. */
                locals->collation = locals->attribute_values[SXML_XSLT_SORT_ATTRIBUTE_COLLATION];
                break;
            }
            collation_fixing fixing = {.is_fixed = false};
            status = add_fixed_collation(locals, &fixing, &locals->collation);
            if (status == SXML_STATUS_OK && !fixing.is_fixed) {
                sxml_ir_atom empty_text = {0};
                status = sxml_xpath_lowerer_add_string_constant(
                    machine->state, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &empty_text);
                const text_variable variables[2] = {
                    {.local_name = "lang",
                     .atom = locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_LANG]
                                 ? locals->attribute_values[SXML_XSLT_SORT_ATTRIBUTE_LANG]
                                 : empty_text},
                    {.local_name = "case",
                     .atom = locals->has_attributes[SXML_XSLT_SORT_ATTRIBUTE_CASE_ORDER]
                                 ? locals->attribute_values[SXML_XSLT_SORT_ATTRIBUTE_CASE_ORDER]
                                 : empty_text}};
                if (status == SXML_STATUS_OK) {
                    pushed.is_pushed = true;
                    status = call_text(machine, &(text_call){.lowering = lowering,
                                                             .text = collation_text,
                                                             .variables = variables,
                                                             .variable_count = 2,
                                                             .block = frame->block,
                                                             .atom = &locals->collation});
                }
            }
            break;
        }
        case SXML_XSLT_SORT_PHASE_KEY:
            locals->phase = SXML_XSLT_SORT_PHASE_CONVERSION;
            status = lower_sort_key(machine, frame, &pushed);
            break;
        case SXML_XSLT_SORT_PHASE_CONVERSION:
            locals->phase = SXML_XSLT_SORT_PHASE_KEY_DONE;
            status = convert_sort_key(machine, frame, &pushed);
            break;
        case SXML_XSLT_SORT_PHASE_KEY_DONE:
            status = finish_sort_key(machine, frame);
            break;
        }
        if (status != SXML_STATUS_OK || pushed.is_pushed) {
            return status;
        }
    }
}

static const sxml_xpath_lowering_procedure sort_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:sort"),
    .step = continue_sort,
    .locals_size = sizeof(sort_locals)};

sxml_status sxml_xslt_lowering_call_sort(sxml_xpath_lowering_machine *machine,
                                         const sxml_xslt_sort_call *call) {
    const sort_locals initial = {
        .head = {.lowering = call->lowering, .node = call->node},
        .input = call->input,
        .sorts_groups = call->sorts_groups,
        .tuple_key_offset = call->tuple_key_offset,
        .phase = SXML_XSLT_SORT_PHASE_START,
        .order = {.atom_kind = SXML_IR_ATOM_KIND_NONE},
        .collation = {.atom_kind = SXML_IR_ATOM_KIND_NONE}};
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &sort_procedure,
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

/* ====================================================== xsl:perform-sort */

/* xsl:perform-sort (13.2): the select value or the sequence constructor
   (XTSE1040 for both), sorted. */
typedef struct perform_sort_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom input;
    sxml_ir_atom sorted;
} perform_sort_locals;

static sxml_status continue_perform_sort(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame) {
    perform_sort_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index node = locals->head.node;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_xslt_ast_attribute *select =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT);
        sxml_xslt_ast_node_index content =
            sxml_xslt_lowering_first_constructor_child(lowering->ast, node);
        while (content.value != 0 && node_of(lowering, content)->kind == SXML_XSLT_AST_KIND_FALLBACK) {
            content = next_of(lowering, content);
        }
        if (select->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
            if (content.value != 0) {
                return sxml_xslt_lowering_report_static_error(
                    lowering, (sxml_xslt_static_error){
                                  .node = node,
                                  .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                      "XTSE1040 an xsl:perform-sort with a select attribute "
                                      "has content (XSLT 4.0, 13.2)")});
            }
            return sxml_xslt_lowering_call_expression(
                machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                      .expression = select->value.payload.expression,
                                                      .block = frame->block,
                                                      .atom = &locals->input,
                                                      .focus = NULL});
        }
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = sxml_xslt_lowering_first_constructor_child(lowering->ast, node),
                         .block = frame->block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &locals->input,
                                         .atom_block = frame->block,
                                         .focused = NULL},
                         .focus = NULL});
    }
    if (frame->step == 1) {
        frame->step = 2;
        return sxml_xslt_lowering_call_sort(
            machine, &(sxml_xslt_sort_call){.lowering = lowering,
                                            .node = node,
                                            .input = locals->input,
                                            .sorts_groups = false,
                                            .block = frame->block,
                                            .atom = &locals->sorted});
    }
    return finish_with_atom(machine, frame, locals->sorted);
}

static const sxml_xpath_lowering_procedure perform_sort_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:perform-sort"),
    .step = continue_perform_sort,
    .locals_size = sizeof(perform_sort_locals)};

/* ==================================================== xsl:for-each-group */

/* The attributes that choose how groups form (14.1, XTSE1080). */
static const sxml_xslt_ast_attribute_kind grouping_attribute_kinds[] = {
    SXML_XSLT_AST_ATTRIBUTE_GROUP_BY,          SXML_XSLT_AST_ATTRIBUTE_GROUP_ADJACENT,
    SXML_XSLT_AST_ATTRIBUTE_GROUP_STARTING_WITH, SXML_XSLT_AST_ATTRIBUTE_GROUP_ENDING_WITH,
    SXML_XSLT_AST_ATTRIBUTE_SPLIT_WHEN,        SXML_XSLT_AST_ATTRIBUTE_MERGE_WHEN,
};

#define GROUPING_ATTRIBUTE_COUNT                                                                   \
    (sizeof(grouping_attribute_kinds) / sizeof(grouping_attribute_kinds[0]))

typedef struct group_locals {
    sxml_xslt_instruction_locals head;
    sxml_xslt_grouping_method method;
    const sxml_xslt_ast_attribute *grouping;
    sxml_ir_atom population;
    sxml_ir_atom collation;
    /* The MAP that makes the grouping pairs. */
    sxml_xpath_lowering_focus item_focus;
    sxml_xpath_lowering_block pair_block;
    sxml_xpath_lowering_focus match_focus;
    sxml_xpath_lowering_block match_block;
    sxml_ir_atom key;
    sxml_ir_atom groups;
    /* The MAP over the groups. */
    sxml_ir_focus group_slots;
    group_view view;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom body;
    size_t scope_mark;
} group_locals;

/* 14.1: XTSE1080, XTSE1090; the method. */
static sxml_status read_grouping(group_locals *locals) {
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index node = locals->head.node;
    size_t present_count = 0;
    sxml_xslt_ast_attribute_kind present = SXML_XSLT_AST_ATTRIBUTE_NONE;
    for (size_t kind_index = 0; kind_index < GROUPING_ATTRIBUTE_COUNT; kind_index += 1) {
        if (has_attribute(lowering, node, grouping_attribute_kinds[kind_index])) {
            present_count += 1;
            present = grouping_attribute_kinds[kind_index];
        }
    }
    if (present_count != 1) {
        return sxml_xslt_lowering_report_static_error(
            lowering, (sxml_xslt_static_error){
                          .node = node,
                          .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE1080 xsl:for-each-group needs exactly one of group-by, "
                              "group-adjacent, group-starting-with, group-ending-with, "
                              "split-when and merge-when (XSLT 4.0, 14.1)")});
    }
    const bool is_keyed = present == SXML_XSLT_AST_ATTRIBUTE_GROUP_BY ||
                          present == SXML_XSLT_AST_ATTRIBUTE_GROUP_ADJACENT;
    if (!is_keyed && (has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COLLATION) ||
                      has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COMPOSITE))) {
        return sxml_xslt_lowering_report_static_error(
            lowering, (sxml_xslt_static_error){
                          .node = node,
                          .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE1090 collation and composite need group-by or group-adjacent "
                              "(XSLT 4.0, 14.1)")});
    }
    if (present == SXML_XSLT_AST_ATTRIBUTE_SPLIT_WHEN ||
        present == SXML_XSLT_AST_ATTRIBUTE_MERGE_WHEN) {
        return sxml_xslt_lowering_report_unsupported(
            lowering, (sxml_xslt_static_error){
                          .node = node,
                          .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                              "split-when and merge-when are not lowered yet")});
    }
    const sxml_xslt_ast_attribute *composite =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COMPOSITE);
    const bool is_composite = composite->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
                              composite->value.payload.is_true;
    locals->grouping = find_attribute(lowering, node, present);
    locals->method =
        present == SXML_XSLT_AST_ATTRIBUTE_GROUP_BY
            ? (is_composite ? SXML_XSLT_GROUPING_METHOD_BY_COMPOSITE : SXML_XSLT_GROUPING_METHOD_BY)
        : present == SXML_XSLT_AST_ATTRIBUTE_GROUP_ADJACENT
            ? (is_composite ? SXML_XSLT_GROUPING_METHOD_ADJACENT_COMPOSITE
                            : SXML_XSLT_GROUPING_METHOD_ADJACENT)
        : present == SXML_XSLT_AST_ATTRIBUTE_GROUP_STARTING_WITH
            ? SXML_XSLT_GROUPING_METHOD_STARTING_WITH
            : SXML_XSLT_GROUPING_METHOD_ENDING_WITH;
    return SXML_STATUS_OK;
}

static bool is_keyed_method(sxml_xslt_grouping_method method) {
    return method != SXML_XSLT_GROUPING_METHOD_STARTING_WITH &&
           method != SXML_XSLT_GROUPING_METHOD_ENDING_WITH;
}

/* The pattern's match expression for the item of the pair being made,
   lowered in match_block with the item as fixed focus (14.1, D118). */
static sxml_status lower_match(sxml_xpath_lowering_machine *machine,
                               sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    group_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_expression *pattern =
        sxml_xslt_ast_get_expression(lowering->ast, locals->grouping->value.payload.expression);
    if (pattern->ast == NULL || pattern->static_context.value == 0 ||
        pattern->static_context.value >= lowering->static_context_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xpath_static_context *context = &lowering->static_contexts[pattern->static_context.value];
    sxml_ir_atom item = {0};
    sxml_status status = read_focus_value(
        lowerer, (focus_value_read){.focus = &locals->item_focus, .block = &locals->pair_block},
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
    locals->match_focus =
        (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED, .fixed_value = item};
    locals->match_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    return sxml_xpath_lowering_call_embedded(
        machine, &(sxml_xpath_lowering_embedded_call){
                     .embedding = {.ast = match,
                                   .node = {.value = 0},
                                   .static_context = context,
                                   .focus = &locals->match_focus},
                     .block = &locals->match_block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = &locals->key,
                                     .atom_block = &locals->match_block,
                                     .focused = NULL}});
}

/* 6.5: an error while matching is no match: the match block inside a
   TRY_CATCH whose one clause gives false. */
static sxml_status bind_guarded_match(sxml_xpath_lowerer *lowerer, group_locals *locals) {
    sxml_ir_expression_index body = {0};
    sxml_status status = finish_branch(
        lowerer, (branch_result){.block = &locals->match_block, .value = locals->key}, &body);
    sxml_ir_expression guarded = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_make_guarded_match(lowerer, body, &guarded);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(lowerer, (binding){.block = &locals->pair_block, .expression = guarded},
                &locals->key);
}

/* The pairs (MAP over the population) and the row's groups. */
static sxml_status bind_groups(sxml_xpath_lowerer *lowerer, group_locals *locals,
                               sxml_xpath_lowering_block *block) {
    sxml_ir_atom item = {0};
    sxml_status status = read_focus_value(
        lowerer, (focus_value_read){.focus = &locals->item_focus, .block = &locals->pair_block},
        &item);
    const sxml_ir_atom members[2] = {item, locals->key};
    sxml_ir_atom_list member_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit, (sxml_ir_atom_view){.atoms = members, .atom_count = 2}, &member_list);
    }
    sxml_ir_atom pair = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = &locals->pair_block,
                                .expression = {.expression_kind =
                                                   SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
                                               .array_constructor = {.array_form =
                                                                         SXML_IR_ARRAY_FORM_SQUARE,
                                                                     .members = member_list}}},
                      &pair);
    }
    sxml_ir_expression_index pair_body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, (branch_result){.block = &locals->pair_block, .value = pair},
                               &pair_body);
    }
    /* The collation's errors are XSLT's here too (fn:distinct-values). */
    const sxml_xslt_error_mapping_view mappings = {
        .mappings = grouping_mappings,
        .mapping_count = sizeof(grouping_mappings) / sizeof(grouping_mappings[0])};
    sxml_ir_atom pairs = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_mapped_expression(
            lowerer,
            &(sxml_xslt_mapped_expression){
                .block = block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                               .map = {.input = locals->population,
                                       .focus = locals->item_focus.slots,
                                       .body = pair_body,
                                       .flags = 0}},
                .mappings = mappings},
            &pairs);
    }
    sxml_ir_atom arguments[3] = {pairs, {0}, locals->collation};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(
            lowerer, sxml_atomic_item_from_integer((int64_t)locals->method), &arguments[1]);
    }
    sxml_ir_atom_list argument_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit, (sxml_ir_atom_view){.atoms = arguments, .atom_count = 3}, &argument_list);
    }
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
                                                          &sxml_xslt_for_each_group_function},
                                           .arguments = argument_list}},
            .mappings = mappings},
        &locals->groups);
}

/* The body: the sequence constructor once per group (14.1). */
static sxml_status lower_group_body(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    group_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = add_focus_slots(lowerer, &locals->group_slots);
    locals->body_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    if (status == SXML_STATUS_OK) {
        status = bind_group_view(
            lowering, (group_view_request){.block = &locals->body_block, .slots = locals->group_slots},
            &locals->view);
    }
    locals->scope_mark = lowerer->scope_entry_count;
    if (status == SXML_STATUS_OK) {
        status = push_group_scope(
            lowerer,
            (group_scope){.view = &locals->view, .has_key = is_keyed_method(locals->method)});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_call_sequence_constructor(
        machine, &(sxml_xslt_sequence_constructor_call){
                     .lowering = lowering,
                     .first = sxml_xslt_lowering_first_constructor_child(lowering->ast,
                                                                         locals->head.node),
                     .block = &locals->body_block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = &locals->body,
                                     .atom_block = &locals->body_block,
                                     .focused = NULL},
                     .focus = &locals->view.focus});
}

static sxml_status finish_group(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    group_locals *locals = frame->locals;
    sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
    sxml_ir_expression_index body = {0};
    sxml_status status = finish_branch(
        lowerer, (branch_result){.block = &locals->body_block, .value = locals->body}, &body);
    sxml_ir_expression_index map = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(lowerer,
                                (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                                                     .map = {.input = locals->groups,
                                                             .focus = locals->group_slots,
                                                             .body = body,
                                                             .flags = 0}},
                                &map);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, map);
}

typedef enum group_step {
    SXML_XSLT_GROUP_STEP_SELECT,
    SXML_XSLT_GROUP_STEP_COLLATION,
    SXML_XSLT_GROUP_STEP_KEY,
    SXML_XSLT_GROUP_STEP_KEY_VALUE,
    SXML_XSLT_GROUP_STEP_GROUPS,
    SXML_XSLT_GROUP_STEP_BODY,
    SXML_XSLT_GROUP_STEP_FINISH,
} group_step;

/* One step of xsl:for-each-group; outcome->is_pushed when it asked for a part. */
static sxml_status step_group(sxml_xpath_lowering_machine *machine,
                              sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    sxml_xpath_lowerer *lowerer = machine->state;
    group_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index node = locals->head.node;
    sxml_status status = SXML_STATUS_OK;
    outcome->is_pushed = true;
    switch ((group_step)frame->step) {
    case SXML_XSLT_GROUP_STEP_SELECT: {
        frame->step = SXML_XSLT_GROUP_STEP_COLLATION;
        status = read_grouping(locals);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_xslt_ast_attribute *select =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT);
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->population,
                                                  .focus = NULL});
    }
    case SXML_XSLT_GROUP_STEP_COLLATION: {
        frame->step = SXML_XSLT_GROUP_STEP_KEY;
        const sxml_xslt_ast_attribute *collation =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_COLLATION);
        if (collation->value.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
            return sxml_xslt_lowering_call_value_template(
                machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                      .expression =
                                                          collation->value.payload.expression,
                                                      .block = frame->block,
                                                      .atom = &locals->collation,
                                                      .focus = NULL});
        }
        /* 14.1: the default collation. */
        outcome->is_pushed = false;
        return sxml_xpath_lowerer_add_string_constant(
            lowerer, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &locals->collation);
    }
    case SXML_XSLT_GROUP_STEP_KEY:
        frame->step = SXML_XSLT_GROUP_STEP_KEY_VALUE;
        locals->item_focus =
            (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
        locals->pair_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
        if (!is_keyed_method(locals->method)) {
            return lower_match(machine, frame);
        }
        /* 14.1: the key with the item, its position and the population's
           size as the focus. */
        return sxml_xslt_lowering_call_expression(
            machine,
            &(sxml_xslt_expression_call){.lowering = lowering,
                                         .expression = locals->grouping->value.payload.expression,
                                         .block = &locals->pair_block,
                                         .atom = &locals->key,
                                         .focus = &locals->item_focus});
    case SXML_XSLT_GROUP_STEP_KEY_VALUE: {
        frame->step = SXML_XSLT_GROUP_STEP_GROUPS;
        if (!is_keyed_method(locals->method)) {
            outcome->is_pushed = false;
            return bind_guarded_match(lowerer, locals);
        }
        const text_variable variables[2] = {{.local_name = "key", .atom = locals->key},
                                            {.local_name = "collation", .atom = locals->collation}};
        const bool is_distinct = locals->method == SXML_XSLT_GROUPING_METHOD_BY;
        return call_text(machine, &(text_call){.lowering = lowering,
                                               .text = is_distinct ? distinct_key_text
                                                                   : grouping_key_text,
                                               .variables = variables,
                                               .variable_count = is_distinct ? 2 : 1,
                                               .block = &locals->pair_block,
                                               .atom = &locals->key});
    }
    case SXML_XSLT_GROUP_STEP_GROUPS:
        frame->step = SXML_XSLT_GROUP_STEP_BODY;
        status = bind_groups(lowerer, locals, frame->block);
        if (status != SXML_STATUS_OK || !sxml_xslt_lowering_has_sort(lowering, node)) {
            outcome->is_pushed = false;
            return status;
        }
        /* 14.3: the order of first appearance unless sorted. */
        return sxml_xslt_lowering_call_sort(
            machine, &(sxml_xslt_sort_call){.lowering = lowering,
                                            .node = node,
                                            .input = locals->groups,
                                            .sorts_groups = true,
                                            .block = frame->block,
                                            .atom = &locals->groups});
    case SXML_XSLT_GROUP_STEP_BODY:
        frame->step = SXML_XSLT_GROUP_STEP_FINISH;
        return lower_group_body(machine, frame);
    case SXML_XSLT_GROUP_STEP_FINISH:
        return finish_group(machine, frame);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static sxml_status continue_group(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    for (;;) {
        step_outcome pushed = {.is_pushed = true};
        const sxml_status status = step_group(machine, frame, &pushed);
        if (status != SXML_STATUS_OK || pushed.is_pushed) {
            return status;
        }
    }
}

static const sxml_xpath_lowering_procedure group_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:for-each-group"),
    .step = continue_group,
    .locals_size = sizeof(group_locals)};

/* ============================================================= xsl:merge */

/* 15.4 to 15.6: the merge-action's view of a group. */
static const char merge_items_text[] = GROUP_VARIABLE "tuples ! ?2";
static const char merge_key_array_text[] =
    "Q{http://www.w3.org/2005/xpath-functions/array}subarray(head(" GROUP_VARIABLE "tuples), 3)";
static const char merge_sources_text[] =
    "Q{http://www.w3.org/2005/xpath-functions/map}merge(for $i in 1 to count(" GROUP_VARIABLE
    "names) return Q{http://www.w3.org/2005/xpath-functions/map}entry("
    GROUP_VARIABLE "names[$i], " GROUP_VARIABLE "tuples[?1 eq $i] ! ?2))";
/* 15.3: the documents for-each-source names, relative to the base URI. */
static const char merge_documents_text[] =
    GROUP_VARIABLE "uris ! doc(resolve-uri(., " GROUP_VARIABLE "base))";

typedef enum merge_phase {
    SXML_XSLT_MERGE_PHASE_START,
    SXML_XSLT_MERGE_PHASE_KEY_ATTRIBUTES,
    SXML_XSLT_MERGE_PHASE_ANCHORS,
    SXML_XSLT_MERGE_PHASE_ANCHOR_DOCUMENTS,
    SXML_XSLT_MERGE_PHASE_SELECT,
    SXML_XSLT_MERGE_PHASE_KEY,
    SXML_XSLT_MERGE_PHASE_KEY_VALUE,
    SXML_XSLT_MERGE_PHASE_TUPLES,
    SXML_XSLT_MERGE_PHASE_SORT,
    SXML_XSLT_MERGE_PHASE_GROUPS,
    SXML_XSLT_MERGE_PHASE_ITEMS,
    SXML_XSLT_MERGE_PHASE_KEY_ARRAY,
    SXML_XSLT_MERGE_PHASE_SOURCES,
    SXML_XSLT_MERGE_PHASE_ACTION,
    SXML_XSLT_MERGE_PHASE_FINISH,
} merge_phase;

/* The members of a merge tuple before its keys: [source number, item,
   keys...]. */
#define MERGE_TUPLE_KEY_OFFSET 3U

typedef struct merge_locals {
    sxml_xslt_instruction_locals head;
    merge_phase phase;
    sxml_xslt_ast_node_index first_source;
    sxml_xslt_ast_node_index source;
    size_t source_index;
    size_t source_count;
    size_t key_count;
    sxml_xslt_ast_node_index key;
    size_t key_index;
    /* The merge source being lowered. */
    bool has_anchors;
    sxml_ir_atom anchors;
    sxml_xpath_lowering_focus anchor_focus;
    sxml_xpath_lowering_block anchor_block;
    sxml_ir_atom input;
    sxml_xpath_lowering_focus item_focus;
    sxml_xpath_lowering_focus key_focus;
    sxml_xpath_lowering_block item_block;
    sxml_ir_atom value;
    /* Owned: the data-type values of the source's keys (owned_memory[0]),
       a tuple's members (owned_memory[1]), each source's tuples
       (owned_memory[2]), the source names (owned_memory[3]). */
    sxml_ir_atom *data_types;
    sxml_ir_atom *members;
    sxml_ir_atom *source_tuples;
    sxml_ir_atom *source_names;
    sxml_ir_atom sorted;
    bool has_collation;
    sxml_ir_atom collation;
    sxml_ir_atom groups;
    /* The MAP over the groups. */
    sxml_ir_focus group_slots;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom tuples;
    sxml_ir_atom merge_key;
    sxml_ir_atom items;
    sxml_ir_atom initial_item;
    sxml_ir_atom key_array;
    sxml_ir_atom sources;
    sxml_xpath_lowering_focus body_focus;
    sxml_ir_atom body;
    size_t scope_mark;
} merge_locals;

/* 15.3, 15.4: XTSE2200, XTSE3195, XTSE3190; the counts. */
static sxml_status check_merge(const sxml_xslt_lowering *lowering, merge_locals *locals) {
    const sxml_xslt_ast_node_index node = locals->head.node;
    locals->first_source =
        first_child_of(lowering, (child_query){.node = node, .kind = SXML_XSLT_AST_KIND_MERGE_SOURCE});
    locals->source_count =
        count_children(lowering, (child_query){.node = node, .kind = SXML_XSLT_AST_KIND_MERGE_SOURCE});
    locals->key_count = count_children(
        lowering,
        (child_query){.node = locals->first_source, .kind = SXML_XSLT_AST_KIND_MERGE_KEY});
    for (sxml_xslt_ast_node_index source = locals->first_source; source.value != 0;
         source = next_like(lowering, source)) {
        if (count_children(lowering, (child_query){.node = source,
                                                   .kind = SXML_XSLT_AST_KIND_MERGE_KEY}) !=
            locals->key_count) {
            return sxml_xslt_lowering_report_static_error(
                lowering, (sxml_xslt_static_error){
                              .node = source,
                              .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XTSE2200 the merge sources have different numbers of merge "
                                  "keys (XSLT 4.0, 15.4)")});
        }
        if (has_attribute(lowering, source, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_ITEM) &&
            has_attribute(lowering, source, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE)) {
            return sxml_xslt_lowering_report_static_error(
                lowering, (sxml_xslt_static_error){
                              .node = source,
                              .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XTSE3195 for-each-item and for-each-source together "
                                  "(XSLT 4.0, 15.3)")});
        }
        const sxml_xslt_ast_attribute *name =
            find_attribute(lowering, source, SXML_XSLT_AST_ATTRIBUTE_NAME);
        for (sxml_xslt_ast_node_index other = next_like(lowering, source);
             name->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME && other.value != 0;
             other = next_like(lowering, other)) {
            const sxml_xslt_ast_attribute *other_name =
                find_attribute(lowering, other, SXML_XSLT_AST_ATTRIBUTE_NAME);
            if (other_name->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME &&
                other_name->value.payload.name.index == name->value.payload.name.index) {
                return sxml_xslt_lowering_report_static_error(
                    lowering, (sxml_xslt_static_error){
                                  .node = other,
                                  .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                      "XTSE3190 two merge sources have one name "
                                      "(XSLT 4.0, 15.3)")});
            }
        }
    }
    if (locals->source_count == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t sort_count = 0;
    for (sxml_xslt_ast_node_index source = locals->first_source; source.value != 0;
         source = next_like(lowering, source)) {
        const sxml_status status = check_sorts(lowering, source, &sort_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* Allocates the owned arrays and the source names. */
static sxml_status start_merge(sxml_xpath_lowering_machine *machine,
                               sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    merge_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = check_merge(lowering, locals);
    const size_t counts[4] = {locals->key_count, MERGE_TUPLE_KEY_OFFSET - 1 + locals->key_count,
                              locals->source_count, locals->source_count};
    for (size_t memory_index = 0; status == SXML_STATUS_OK && memory_index < 4;
         memory_index += 1) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = memory_index,
                .scratch = {.element_count = counts[memory_index],
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->data_types = frame->owned_memory[0];
    locals->members = frame->owned_memory[1];
    locals->source_tuples = frame->owned_memory[2];
    locals->source_names = frame->owned_memory[3];
    size_t source_index = 0;
    for (sxml_xslt_ast_node_index source = locals->first_source;
         status == SXML_STATUS_OK && source.value != 0; source = next_like(lowering, source)) {
        /* 15.3: an unnamed source gets a name no other has. */
        const sxml_xslt_ast_attribute *name =
            find_attribute(lowering, source, SXML_XSLT_AST_ATTRIBUTE_NAME);
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("#unnamed")};
        if (name->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME) {
            status = sxml_name_pool_get_name_parts(lowerer->name_pool, name->value.payload.name,
                                                   &parts);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_string_constant(lowerer, parts.local_name,
                                                            &locals->source_names[source_index]);
        }
        source_index += 1;
    }
    locals->source = locals->first_source;
    locals->source_index = 0;
    return status;
}

/* Lowers the next data-type attribute value template of the source's
   keys (with the instruction's focus, 15.4). */
static sxml_status lower_merge_key_attribute(sxml_xpath_lowering_machine *machine,
                                             sxml_xpath_lowering_frame *frame,
                                             step_outcome *outcome) {
    merge_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    outcome->is_pushed = false;
    while (locals->key.value != 0) {
        const sxml_xslt_ast_attribute *data_type =
            find_attribute(lowering, locals->key, SXML_XSLT_AST_ATTRIBUTE_DATA_TYPE);
        const size_t key_index = locals->key_index;
        locals->data_types[key_index] = (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
        locals->key = next_like(lowering, locals->key);
        locals->key_index += 1;
        if (data_type->value.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
            outcome->is_pushed = true;
            return sxml_xslt_lowering_call_value_template(
                machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                      .expression =
                                                          data_type->value.payload.expression,
                                                      .block = frame->block,
                                                      .atom = &locals->data_types[key_index],
                                                      .focus = NULL});
        }
    }
    return SXML_STATUS_OK;
}

/* The block the source's input is lowered in: the anchor MAP's body, or
   the instruction's block. */
static sxml_xpath_lowering_block *source_block(sxml_xpath_lowering_frame *frame) {
    merge_locals *locals = frame->locals;
    return locals->has_anchors ? &locals->anchor_block : frame->block;
}

/* The key of key_index for the item: select, content or "." with a
   singleton focus (15.4). */
static sxml_status lower_merge_key(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    sxml_xpath_lowerer *lowerer = machine->state;
    merge_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    outcome->is_pushed = false;
    const sxml_xslt_ast_attribute *select =
        find_attribute(lowering, locals->key, SXML_XSLT_AST_ATTRIBUTE_SELECT);
    if (select->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = &locals->item_block,
                                                  .atom = &locals->value,
                                                  .focus = &locals->key_focus});
    }
    const sxml_xslt_ast_node_index content = node_of(lowering, locals->key)->first_child;
    if (content.value != 0) {
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = content,
                         .block = &locals->item_block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &locals->value,
                                         .atom_block = &locals->item_block,
                                         .focused = NULL},
                         .focus = &locals->key_focus});
    }
    locals->value = locals->key_focus.fixed_value;
    (void)lowerer;
    return SXML_STATUS_OK;
}

/* The key atomized (xs:untypedAtomic as xs:string) and converted as
   data-type says (13.1.2). */
static sxml_status convert_merge_key(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    merge_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const fixed_text data_type = read_fixed_text(
        lowering, find_attribute(lowering, locals->key, SXML_XSLT_AST_ATTRIBUTE_DATA_TYPE));
    const bool has_data_type =
        locals->data_types[locals->key_index].atom_kind != SXML_IR_ATOM_KIND_NONE;
    const char *text = !has_data_type ? grouping_key_text
                       : !data_type.is_fixed                  ? data_type_key_text
                       : TEXT_IS(data_type.text, "number") ? number_key_text
                       : TEXT_IS(data_type.text, "text")   ? string_key_text
                                                               : grouping_key_text;
    const text_variable variables[2] = {
        {.local_name = "key", .atom = locals->value},
        {.local_name = "type", .atom = locals->data_types[locals->key_index]}};
    return call_text(machine,
                     &(text_call){.lowering = lowering,
                                  .text = text,
                                  .variables = variables,
                                  .variable_count = text == data_type_key_text ? 2 : 1,
                                  .block = &locals->item_block,
                                  .atom = &locals->members[MERGE_TUPLE_KEY_OFFSET - 1 +
                                                           locals->key_index]});
}

/* The tuple of the item, then the MAPs: the source's tuples. */
static sxml_status bind_source_tuples(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_frame *frame) {
    merge_locals *locals = frame->locals;
    sxml_status status = sxml_xpath_lowerer_add_item_constant(
        lowerer, sxml_atomic_item_from_integer((int64_t)locals->source_index + 1),
        &locals->members[0]);
    locals->members[1] = locals->key_focus.fixed_value;
    sxml_ir_atom_list member_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit,
            (sxml_ir_atom_view){.atoms = locals->members,
                                .atom_count = MERGE_TUPLE_KEY_OFFSET - 1 + locals->key_count},
            &member_list);
    }
    sxml_ir_atom tuple = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = &locals->item_block,
                                .expression = {.expression_kind =
                                                   SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
                                               .array_constructor = {.array_form =
                                                                         SXML_IR_ARRAY_FORM_SQUARE,
                                                                     .members = member_list}}},
                      &tuple);
    }
    sxml_ir_expression_index item_body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, (branch_result){.block = &locals->item_block, .value = tuple},
                               &item_body);
    }
    sxml_ir_atom tuples = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = source_block(frame),
                                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                                               .map = {.input = locals->input,
                                                       .focus = locals->item_focus.slots,
                                                       .body = item_body,
                                                       .flags = 0}}},
                      &tuples);
    }
    if (status == SXML_STATUS_OK && locals->has_anchors) {
        sxml_ir_expression_index anchor_body = {0};
        status = finish_branch(
            lowerer, (branch_result){.block = &locals->anchor_block, .value = tuples}, &anchor_body);
        if (status == SXML_STATUS_OK) {
            status = bind(lowerer,
                          (binding){.block = frame->block,
                                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                                                   .map = {.input = locals->anchors,
                                                           .focus = locals->anchor_focus.slots,
                                                           .body = anchor_body,
                                                           .flags = 0}}},
                          &tuples);
        }
    }
    locals->source_tuples[locals->source_index] = tuples;
    return status;
}

/* All tuples, merged: sorted by the keys, then runs of equal keys. */
static sxml_status bind_merge_groups(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_frame *frame) {
    merge_locals *locals = frame->locals;
    sxml_xpath_lowering_focus tuple_focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
    sxml_xpath_lowering_block pair_block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom tuple = {0};
    sxml_status status = read_focus_value(
        lowerer, (focus_value_read){.focus = &tuple_focus, .block = &pair_block}, &tuple);
    for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < locals->key_count;
         key_index += 1) {
        status = bind_member(lowerer,
                             (member_read){.block = &pair_block,
                                           .array = tuple,
                                           .member_number =
                                               (int64_t)(MERGE_TUPLE_KEY_OFFSET + key_index)},
                             &locals->members[key_index]);
    }
    sxml_ir_expression keys = {0};
    sxml_ir_atom_list key_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit, (sxml_ir_atom_view){.atoms = locals->members, .atom_count = locals->key_count},
            &key_list);
        keys = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                    .sequence = key_list};
    }
    sxml_ir_atom key = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer, (binding){.block = &pair_block, .expression = keys}, &key);
    }
    const sxml_ir_atom pair_members[2] = {tuple, key};
    sxml_ir_atom_list pair_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit, (sxml_ir_atom_view){.atoms = pair_members, .atom_count = 2}, &pair_list);
    }
    sxml_ir_atom pair = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = &pair_block,
                                .expression = {.expression_kind =
                                                   SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
                                               .array_constructor = {.array_form =
                                                                         SXML_IR_ARRAY_FORM_SQUARE,
                                                                     .members = pair_list}}},
                      &pair);
    }
    sxml_ir_expression_index pair_body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, (branch_result){.block = &pair_block, .value = pair},
                               &pair_body);
    }
    sxml_ir_atom pairs = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = frame->block,
                                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                                               .map = {.input = locals->sorted,
                                                       .focus = tuple_focus.slots,
                                                       .body = pair_body,
                                                       .flags = 0}}},
                      &pairs);
    }
    sxml_ir_atom arguments[3] = {pairs, {0}, locals->collation};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(
            lowerer,
            sxml_atomic_item_from_integer((int64_t)SXML_XSLT_GROUPING_METHOD_ADJACENT_COMPOSITE),
            &arguments[1]);
    }
    sxml_ir_atom_list argument_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit, (sxml_ir_atom_view){.atoms = arguments, .atom_count = 3}, &argument_list);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(lowerer,
                (binding){.block = frame->block,
                          .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                                         .static_call = {.target = {.target_kind =
                                                                        SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                                    .native_function =
                                                                        &sxml_xslt_for_each_group_function},
                                                         .arguments = argument_list}}},
                &locals->groups);
}

/* One step of xsl:merge; outcome->is_pushed when it asked for a part. */
static sxml_status step_merge(sxml_xpath_lowering_machine *machine,
                              sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    sxml_xpath_lowerer *lowerer = machine->state;
    merge_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = SXML_STATUS_OK;
    outcome->is_pushed = false;
    switch (locals->phase) {
    case SXML_XSLT_MERGE_PHASE_START:
        locals->phase = SXML_XSLT_MERGE_PHASE_KEY_ATTRIBUTES;
        status = start_merge(machine, frame);
        locals->key = first_child_of(
            lowering, (child_query){.node = locals->source, .kind = SXML_XSLT_AST_KIND_MERGE_KEY});
        locals->key_index = 0;
        return status;
    case SXML_XSLT_MERGE_PHASE_KEY_ATTRIBUTES:
        status = lower_merge_key_attribute(machine, frame, outcome);
        if (status == SXML_STATUS_OK && !outcome->is_pushed) {
            locals->phase = SXML_XSLT_MERGE_PHASE_ANCHORS;
            locals->key_index = 0;
        }
        return status;
    case SXML_XSLT_MERGE_PHASE_ANCHORS: {
        locals->phase = SXML_XSLT_MERGE_PHASE_ANCHOR_DOCUMENTS;
        const sxml_xslt_ast_attribute *item =
            find_attribute(lowering, locals->source, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_ITEM);
        const sxml_xslt_ast_attribute *source =
            find_attribute(lowering, locals->source, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE);
        const sxml_xslt_ast_attribute *anchors =
            item->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION ? item : source;
        locals->has_anchors = anchors->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION;
        if (!locals->has_anchors) {
            return SXML_STATUS_OK;
        }
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = anchors->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->anchors,
                                                  .focus = NULL});
    }
    case SXML_XSLT_MERGE_PHASE_ANCHOR_DOCUMENTS: {
        locals->phase = SXML_XSLT_MERGE_PHASE_SELECT;
        if (!has_attribute(lowering, locals->source, SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE)) {
            return SXML_STATUS_OK;
        }
        const sxml_xslt_ast_static_context_index context_index =
            node_of(lowering, locals->source)->static_context;
        const sxml_byte_view base_uri =
            context_index.value < lowering->static_context_count
                ? lowering->static_contexts[context_index.value].static_base_uri
                : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        sxml_ir_atom base = {0};
        status = sxml_xpath_lowerer_add_string_constant(lowerer, base_uri, &base);
        const text_variable variables[2] = {{.local_name = "uris", .atom = locals->anchors},
                                            {.local_name = "base", .atom = base}};
        if (status == SXML_STATUS_OK) {
            outcome->is_pushed = true;
            status = call_text(machine, &(text_call){.lowering = lowering,
                                                     .text = merge_documents_text,
                                                     .variables = variables,
                                                     .variable_count = 2,
                                                     .block = frame->block,
                                                     .atom = &locals->anchors});
        }
        return status;
    }
    case SXML_XSLT_MERGE_PHASE_SELECT: {
        locals->phase = SXML_XSLT_MERGE_PHASE_KEY;
        locals->anchor_focus =
            (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
        locals->anchor_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
        const sxml_xslt_ast_attribute *select =
            find_attribute(lowering, locals->source, SXML_XSLT_AST_ATTRIBUTE_SELECT);
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_expression(
            machine,
            &(sxml_xslt_expression_call){.lowering = lowering,
                                         .expression = select->value.payload.expression,
                                         .block = source_block(frame),
                                         .atom = &locals->input,
                                         .focus = locals->has_anchors ? &locals->anchor_focus : NULL});
    }
    case SXML_XSLT_MERGE_PHASE_KEY:
        if (locals->key_index == 0) {
            /* The item MAP's body: the item as a singleton focus (15.4). */
            locals->item_focus =
                (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
            locals->item_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
            sxml_ir_atom item = {0};
            status = read_focus_value(
                lowerer, (focus_value_read){.focus = &locals->item_focus, .block = &locals->item_block},
                &item);
            locals->key_focus = (sxml_xpath_lowering_focus){
                .kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED, .fixed_value = item};
            locals->key = first_child_of(
                lowering, (child_query){.node = locals->source, .kind = SXML_XSLT_AST_KIND_MERGE_KEY});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        if (locals->key.value == 0) {
            locals->phase = SXML_XSLT_MERGE_PHASE_TUPLES;
            return SXML_STATUS_OK;
        }
        locals->phase = SXML_XSLT_MERGE_PHASE_KEY_VALUE;
        return lower_merge_key(machine, frame, outcome);
    case SXML_XSLT_MERGE_PHASE_KEY_VALUE:
        locals->phase = SXML_XSLT_MERGE_PHASE_KEY;
        outcome->is_pushed = true;
        status = convert_merge_key(machine, frame);
        locals->key = next_like(lowering, locals->key);
        locals->key_index += 1;
        return status;
    case SXML_XSLT_MERGE_PHASE_TUPLES:
        status = bind_source_tuples(lowerer, frame);
        locals->source = next_like(lowering, locals->source);
        locals->source_index += 1;
        locals->key = first_child_of(
            lowering, (child_query){.node = locals->source, .kind = SXML_XSLT_AST_KIND_MERGE_KEY});
        locals->key_index = 0;
        locals->phase =
            locals->source.value != 0 ? SXML_XSLT_MERGE_PHASE_KEY_ATTRIBUTES : SXML_XSLT_MERGE_PHASE_SORT;
        return status;
    case SXML_XSLT_MERGE_PHASE_SORT: {
        locals->phase = SXML_XSLT_MERGE_PHASE_GROUPS;
        sxml_ir_expression all = {0};
        sxml_ir_atom_list list = {0};
        status = sxml_ir_unit_add_atoms(
            lowerer->unit,
            (sxml_ir_atom_view){.atoms = locals->source_tuples, .atom_count = locals->source_count},
            &list);
        all = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                   .sequence = list};
        sxml_ir_atom tuples = {0};
        if (status == SXML_STATUS_OK) {
            status = bind(lowerer, (binding){.block = frame->block, .expression = all}, &tuples);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* 15.6: the groups in key order; the tuples in source, anchor and
           input order (a stable sort). */
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_sort(
            machine, &(sxml_xslt_sort_call){.lowering = lowering,
                                            .node = locals->first_source,
                                            .input = tuples,
                                            .sorts_groups = false,
                                            .tuple_key_offset = MERGE_TUPLE_KEY_OFFSET,
                                            .block = frame->block,
                                            .atom = &locals->sorted});
    }
    case SXML_XSLT_MERGE_PHASE_GROUPS: {
        locals->phase = SXML_XSLT_MERGE_PHASE_ITEMS;
        const sxml_xslt_ast_node_index first_key = first_child_of(
            lowering,
            (child_query){.node = locals->first_source, .kind = SXML_XSLT_AST_KIND_MERGE_KEY});
        const sxml_xslt_ast_attribute *collation =
            find_attribute(lowering, first_key, SXML_XSLT_AST_ATTRIBUTE_COLLATION);
        if (collation->value.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE &&
            !locals->has_collation) {
            /* Equal keys under the first key's collation. */
            locals->phase = SXML_XSLT_MERGE_PHASE_GROUPS;
            locals->has_collation = true;
            outcome->is_pushed = true;
            return sxml_xslt_lowering_call_value_template(
                machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                      .expression =
                                                          collation->value.payload.expression,
                                                      .block = frame->block,
                                                      .atom = &locals->collation,
                                                      .focus = NULL});
        }
        if (!locals->has_collation) {
            status = sxml_xpath_lowerer_add_string_constant(
                lowerer, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &locals->collation);
        }
        if (status == SXML_STATUS_OK) {
            status = bind_merge_groups(lowerer, frame);
        }
        if (status == SXML_STATUS_OK) {
            status = add_focus_slots(lowerer, &locals->group_slots);
        }
        locals->body_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
        const sxml_ir_atom group_array = slot_atom(locals->group_slots.context_value);
        if (status == SXML_STATUS_OK) {
            status = bind_member(
                lowerer,
                (member_read){.block = &locals->body_block, .array = group_array, .member_number = 1},
                &locals->tuples);
        }
        if (status == SXML_STATUS_OK) {
            status = bind_member(
                lowerer,
                (member_read){.block = &locals->body_block, .array = group_array, .member_number = 2},
                &locals->merge_key);
        }
        return status;
    }
    case SXML_XSLT_MERGE_PHASE_ITEMS: {
        locals->phase = SXML_XSLT_MERGE_PHASE_KEY_ARRAY;
        const text_variable variable = {.local_name = "tuples", .atom = locals->tuples};
        outcome->is_pushed = true;
        return call_text(machine, &(text_call){.lowering = lowering,
                                               .text = merge_items_text,
                                               .variables = &variable,
                                               .variable_count = 1,
                                               .block = &locals->body_block,
                                               .atom = &locals->items});
    }
    case SXML_XSLT_MERGE_PHASE_KEY_ARRAY: {
        locals->phase = SXML_XSLT_MERGE_PHASE_SOURCES;
        const text_variable variable = {.local_name = "tuples", .atom = locals->tuples};
        outcome->is_pushed = true;
        return call_text(machine, &(text_call){.lowering = lowering,
                                               .text = merge_key_array_text,
                                               .variables = &variable,
                                               .variable_count = 1,
                                               .block = &locals->body_block,
                                               .atom = &locals->key_array});
    }
    case SXML_XSLT_MERGE_PHASE_SOURCES: {
        locals->phase = SXML_XSLT_MERGE_PHASE_ACTION;
        sxml_ir_atom_list list = {0};
        status = sxml_ir_unit_add_atoms(
            lowerer->unit,
            (sxml_ir_atom_view){.atoms = locals->source_names, .atom_count = locals->source_count},
            &list);
        sxml_ir_atom names = {0};
        if (status == SXML_STATUS_OK) {
            status = bind(lowerer,
                          (binding){.block = &locals->body_block,
                                    .expression = {.expression_kind =
                                                       SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                                   .sequence = list}},
                          &names);
        }
        const text_variable variables[2] = {{.local_name = "tuples", .atom = locals->tuples},
                                            {.local_name = "names", .atom = names}};
        if (status == SXML_STATUS_OK) {
            outcome->is_pushed = true;
            status = call_text(machine, &(text_call){.lowering = lowering,
                                                     .text = merge_sources_text,
                                                     .variables = variables,
                                                     .variable_count = 2,
                                                     .block = &locals->body_block,
                                                     .atom = &locals->sources});
        }
        return status;
    }
    case SXML_XSLT_MERGE_PHASE_ACTION: {
        locals->phase = SXML_XSLT_MERGE_PHASE_FINISH;
        status = sxml_xslt_lowering_bind_library_call(
            lowerer,
            &(sxml_xslt_library_call){.block = &locals->body_block,
                                      .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                      .local_name = SXML_BYTE_VIEW_FROM_LITERAL("head"),
                                      .arguments = {.atoms = &locals->items, .atom_count = 1}},
            &locals->initial_item);
        if (status == SXML_STATUS_OK && locals->initial_item.atom_kind != SXML_IR_ATOM_KIND_SLOT) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        locals->scope_mark = lowerer->scope_entry_count;
        if (status == SXML_STATUS_OK) {
            status = push_xslt_variable(lowerer, SXML_BYTE_VIEW_FROM_LITERAL("current-merge-group"),
                                         locals->items);
        }
        if (status == SXML_STATUS_OK) {
            status = push_xslt_variable(lowerer, SXML_BYTE_VIEW_FROM_LITERAL("current-merge-key"),
                                         locals->merge_key);
        }
        if (status == SXML_STATUS_OK) {
            status = push_xslt_variable(
                lowerer, SXML_BYTE_VIEW_FROM_LITERAL("current-merge-key-array"), locals->key_array);
        }
        if (status == SXML_STATUS_OK) {
            status = push_xslt_variable(
                lowerer, SXML_BYTE_VIEW_FROM_LITERAL("current-merge-sources"), locals->sources);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* 15.6: the initial item of the group, its position and the
           number of groups. */
        locals->body_focus = (sxml_xpath_lowering_focus){
            .kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
            .slots = {.context_value = locals->initial_item.slot,
                      .context_position = locals->group_slots.context_position,
                      .context_size = locals->group_slots.context_size}};
        const sxml_xslt_ast_node_index action = first_child_of(
            lowering,
            (child_query){.node = locals->head.node, .kind = SXML_XSLT_AST_KIND_MERGE_ACTION});
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = node_of(lowering, action)->first_child,
                         .block = &locals->body_block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &locals->body,
                                         .atom_block = &locals->body_block,
                                         .focused = NULL},
                         .focus = &locals->body_focus});
    }
    case SXML_XSLT_MERGE_PHASE_FINISH: {
        outcome->is_pushed = true;
        sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
        sxml_ir_expression_index body = {0};
        status = finish_branch(
            lowerer, (branch_result){.block = &locals->body_block, .value = locals->body}, &body);
        sxml_ir_expression_index map = {0};
        if (status == SXML_STATUS_OK) {
            status = add_expression(lowerer,
                                    (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                                                         .map = {.input = locals->groups,
                                                                 .focus = locals->group_slots,
                                                                 .body = body,
                                                                 .flags = 0}},
                                    &map);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowering_finish_frame(machine, frame, map);
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static sxml_status continue_merge(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    for (;;) {
        step_outcome pushed = {.is_pushed = false};
        const sxml_status status = step_merge(machine, frame, &pushed);
        if (status != SXML_STATUS_OK || pushed.is_pushed) {
            return status;
        }
    }
}

static const sxml_xpath_lowering_procedure merge_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:merge"),
    .step = continue_merge,
    .locals_size = sizeof(merge_locals)};

/* ================================================================ Rules */

static const sxml_xslt_instruction_rule group_rules[] = {
    {.kind = SXML_XSLT_AST_KIND_PERFORM_SORT, .procedure = &perform_sort_procedure},
    {.kind = SXML_XSLT_AST_KIND_FOR_EACH_GROUP, .procedure = &group_procedure},
    {.kind = SXML_XSLT_AST_KIND_MERGE, .procedure = &merge_procedure},
};

const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_group = {
    .rules = group_rules, .rule_count = sizeof(group_rules) / sizeof(group_rules[0])};
