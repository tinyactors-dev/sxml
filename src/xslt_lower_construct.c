/*
 * XSLT 4.0 instructions that construct nodes, maps, arrays and sequences
 * (src/xslt_lower_internal.h; XSLT 4.0, sections 10.4.1, 11, 21, 22;
 * packet 4.6, D142): xsl:sequence, xsl:select, xsl:element, xsl:attribute, xsl:text,
 * xsl:value-of, xsl:comment, xsl:processing-instruction, xsl:namespace,
 * xsl:document, xsl:copy, xsl:copy-of, xsl:map, xsl:map-entry, xsl:record,
 * xsl:array, xsl:array-member.
 *
 * Golden file: src/xquery_lower_construct.c (computed constructors: a name
 * constant or atom, the content's atom, one IR constructor; frames whose
 * steps ask for their parts and return, D71).
 *
 * Each instruction is a procedure whose locals start with
 * sxml_xslt_instruction_locals; its result is bound in the sequence
 * constructor's block. The value of an instruction with a select attribute
 * or a sequence constructor (5.7) is the select expression's atom, or the
 * sequence constructor's: having both is the static error the instruction
 * names (XTSE0840, XTSE0870, XTSE0880, XTSE0910, XTSE0940, XTSE3185).
 * Simple content (5.7.2) gives the string of attributes, comments,
 * processing instructions, namespaces and text from xsl:value-of.
 *
 * Names: a name attribute without enclosed expressions is resolved while
 * lowering into an xs:QName constant (or the error is raised when the
 * instruction is evaluated: XTDE0820, XTDE0830, XTDE0850, XTDE0855,
 * XTDE0860); a computed name is an xs:string atom that the evaluator
 * resolves with the instruction's in-scope namespaces in the stylesheet
 * (name_namespaces, D124: with the default namespace for xsl:element,
 * without it for xsl:attribute), its XQuery error codes mapped to XSLT's.
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_lower_internal.h"

#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xml_internal.h"
#include "xslt_accumulator_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* ------------------------------------------------------------ AST reads */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

/* The attribute of kind on node; a zero attribute (kind NONE) when
   absent. */
typedef struct attribute_lookup {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_attribute_kind kind;
} attribute_lookup;

static const sxml_xslt_ast_attribute *find_attribute(const sxml_xslt_lowering *lowering,
                                                     attribute_lookup lookup) {
    return sxml_xslt_ast_get_attribute(
        lowering->ast,
        sxml_xslt_ast_find_attribute(lowering->ast, (sxml_xslt_ast_attribute_query){
                                                        .node = lookup.node, .kind = lookup.kind}));
}

static bool has_attribute(const sxml_xslt_lowering *lowering, attribute_lookup lookup) {
    return find_attribute(lowering, lookup)->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
}

/* True when node has children other than xsl:fallback (its content is
   not empty, 5.7). */
static bool has_content(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    for (sxml_xslt_ast_node_index child = node_of(lowering, node)->first_child; child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_FALLBACK) {
            return true;
        }
    }
    return false;
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

/* ---------------------------------------------------------- IR pieces */

static sxml_status bind(sxml_xpath_lowerer *lowerer, sxml_xslt_expression_to_bind request,
                        sxml_ir_atom *atom) {
    return sxml_xslt_lowering_bind_expression(lowerer, &request, atom);
}

/* Ends frame with atom as its value. */
static sxml_status finish_with_atom(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame, sxml_ir_atom atom) {
    sxml_ir_expression_index result = {0};
    const sxml_status status = sxml_xslt_lowering_add_expression(
        machine->state,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = atom},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

/* Ends frame with expression. */
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

static sxml_status add_string(sxml_xpath_lowerer *lowerer, sxml_byte_view text,
                              sxml_ir_atom *atom) {
    return sxml_xpath_lowerer_add_string_constant(lowerer, text, atom);
}

/* A library call of up to three atoms, bound in block. */
typedef struct call_request {
    sxml_xpath_lowering_block *block;
    sxml_well_known_namespace function_namespace;
    sxml_byte_view local_name;
    sxml_ir_atom_view arguments;
} call_request;

static sxml_status bind_call(sxml_xpath_lowerer *lowerer, call_request request,
                             sxml_ir_atom *atom) {
    return sxml_xslt_lowering_bind_library_call(
        lowerer,
        &(sxml_xslt_library_call){.block = request.block,
                                  .function_namespace = request.function_namespace,
                                  .local_name = request.local_name,
                                  .arguments = request.arguments},
        atom);
}

/* fn:replace($value, pattern, replacement) bound in block (static
   pattern and replacement). */
typedef struct replace_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
    sxml_byte_view pattern;
    sxml_byte_view replacement;
} replace_request;

static sxml_status bind_replace(sxml_xpath_lowerer *lowerer, replace_request request,
                                sxml_ir_atom *atom) {
    sxml_ir_atom arguments[3] = {request.value, {0}, {0}};
    sxml_status status = add_string(lowerer, request.pattern, &arguments[1]);
    if (status == SXML_STATUS_OK) {
        status = add_string(lowerer, request.replacement, &arguments[2]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_call(lowerer,
                     (call_request){.block = request.block,
                                    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("replace"),
                                    .arguments = {.atoms = arguments, .atom_count = 3}},
                     atom);
}

/* value instance of item_type with occurrence, bound in block. */
typedef struct instance_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
    sxml_builtin_type item_type;
    sxml_occurrence occurrence;
} instance_request;

static sxml_status bind_instance_of(sxml_xpath_lowerer *lowerer, instance_request request,
                                    sxml_ir_atom *atom) {
    return bind(
        lowerer,
        (sxml_xslt_expression_to_bind){
            .block = request.block,
            .expression =
                (sxml_ir_expression){
                    .expression_kind = SXML_IR_EXPRESSION_KIND_INSTANCE_OF,
                    .type_test = {.value = request.value,
                                  .sequence_type = {.item_type = sxml_builtin_type_identifier(
                                                        request.item_type),
                                                    .occurrence = request.occurrence}}}},
        atom);
}

/* A RAISE of an XSLT dynamic error, as an atom bound in block. */
typedef struct raise_request {
    sxml_xpath_lowering_block *block;
    sxml_byte_view code;
    sxml_byte_view description;
} raise_request;

static sxml_status bind_raise(sxml_xpath_lowerer *lowerer, raise_request request,
                              sxml_ir_atom *atom) {
    sxml_ir_expression_index raise = {0};
    const sxml_status status = sxml_xpath_lowerer_add_raise(
        lowerer,
        (sxml_xpath_lowering_raise_request){.node = no_ast_node,
                                            .error_status = SXML_STATUS_DYNAMIC_ERROR,
                                            .code_local_name = request.code,
                                            .description = request.description},
        &raise);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_bind_expression(
        lowerer,
        (sxml_xpath_lowering_expression_in_block){.block = request.block, .expression = raise},
        atom);
}

/* The atoms of a sequence as one SEQUENCE expression. */
static sxml_status add_atom_sequence(sxml_xpath_lowerer *lowerer, sxml_ir_atom_view atoms,
                                     sxml_ir_expression_index *index) {
    sxml_ir_atom_list list = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(lowerer->unit, atoms, &list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE, .sequence = list},
        index);
}

/* An ATOM expression of atom, as a constructor's content (D110: one
   enclosed expression). */
static sxml_status add_atom_content(sxml_xpath_lowerer *lowerer, sxml_ir_atom atom,
                                    sxml_ir_expression_index *index) {
    return sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = atom}, index);
}

/* The attribute nodes of node's use-attribute-sets in front of *content
   (10.2.3), bound in a block of their own so they are computed with the
   content; focus is the focus they see (the selected item of xsl:copy
   select, 11.9.1), NULL for the current one. */
typedef struct attribute_set_content {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    sxml_xpath_lowering_focus *focus;
} attribute_set_content;

static sxml_status prepend_attribute_sets(sxml_xpath_lowerer *lowerer,
                                          attribute_set_content request,
                                          sxml_ir_expression_index *content) {
    if (find_attribute(request.lowering,
                       (attribute_lookup){.node = request.node,
                                          .kind = SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS})
            ->kind == SXML_XSLT_AST_ATTRIBUTE_NONE) {
        return SXML_STATUS_OK;
    }
    sxml_xpath_lowering_focus *outer_focus = lowerer->function->focus;
    if (request.focus != NULL) {
        lowerer->function->focus = request.focus;
    }
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom atoms[2] = {{0}, {0}};
    sxml_status status = sxml_xslt_lowering_bind_attribute_sets(
        lowerer,
        &(sxml_xslt_attribute_set_call){
            .lowering = request.lowering, .node = request.node, .block = &block},
        &atoms[0]);
    lowerer->function->focus = outer_focus;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = &block, .expression = *content},
            &atoms[1]);
    }
    sxml_ir_atom_list list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(lowerer->unit,
                                        (sxml_ir_atom_view){.atoms = atoms, .atom_count = 2}, &list);
    }
    sxml_ir_expression_index sequence = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                 .sequence = list},
            &sequence);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = sequence}, content);
}

/* An expression of one branch: lowered into a block of its own. */
typedef struct branch {
    sxml_xpath_lowering_block block;
    sxml_ir_expression_index result;
} branch;

static sxml_status finish_branch(sxml_xpath_lowerer *lowerer, branch *built,
                                 sxml_ir_expression_index *expression) {
    return sxml_xpath_lowerer_finish_block(
        lowerer, (sxml_xpath_lowering_block_result){.block = built->block, .result = built->result},
        expression);
}

/* ---------------------------------------------------- Value and content */

/* What an instruction's value comes from. */
typedef struct value_request {
    const sxml_xslt_instruction_locals *head;
    sxml_xpath_lowering_frame *frame;
    sxml_ir_atom *atom;
    /* "XTSE0940 description" for a select attribute with content; NULL
       when the instruction has no select attribute. */
    const char *conflict_error;
    /* The block of a node constructor's content, which ends with atom
       (D166: its LETs are content LETs); NULL for the frame's block. */
    sxml_xpath_lowering_block *content_block;
} value_request;

/* Lowers the select attribute or the sequence constructor to *atom;
 *has_select tells which. */
typedef struct value_source {
    bool has_select;
} value_source;

static sxml_status start_value(sxml_xpath_lowering_machine *machine, value_request request,
                               value_source *source) {
    const sxml_xslt_lowering *lowering = request.head->lowering;
    sxml_xpath_lowering_block *block =
        request.content_block != NULL ? request.content_block : request.frame->block;
    const sxml_xslt_ast_attribute *select =
        find_attribute(lowering, (attribute_lookup){.node = request.head->node,
                                                    .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT});
    source->has_select = select->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
    if (source->has_select) {
        if (request.conflict_error != NULL && has_content(lowering, request.head->node)) {
            return sxml_xslt_lowering_report_static_error(
                lowering,
                (sxml_xslt_static_error){
                    .node = request.head->node,
                    .code_and_description = {.bytes = (const uint8_t *)request.conflict_error,
                                             .byte_count = strlen(request.conflict_error)}});
        }
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = block,
                                                  .atom = request.atom,
                                                  .focus = NULL});
    }
    return sxml_xslt_lowering_call_sequence_constructor(
        machine, &(sxml_xslt_sequence_constructor_call){
                     .lowering = lowering,
                     .first = node_of(lowering, request.head->node)->first_child,
                     .block = block,
                     .is_body = false,
                     .is_content = request.content_block != NULL,
                     .destination = {.expression = NULL,
                                     .atom = request.atom,
                                     .atom_block = block,
                                     .focused = NULL},
                     .focus = NULL});
}

/* Lowers the value template of attribute kind on the instruction to
   *atom, or writes the default constant there; *has_called false when
   nothing was pushed. */
typedef struct template_request {
    const sxml_xslt_instruction_locals *head;
    sxml_xpath_lowering_frame *frame;
    sxml_xslt_ast_attribute_kind kind;
    sxml_byte_view default_text;
    sxml_ir_atom *atom;
} template_request;

typedef struct template_call {
    bool has_called;
} template_call;

static sxml_status start_template_or_default(sxml_xpath_lowering_machine *machine,
                                             template_request request, template_call *call) {
    const sxml_xslt_lowering *lowering = request.head->lowering;
    const sxml_xslt_ast_attribute *attribute = find_attribute(
        lowering, (attribute_lookup){.node = request.head->node, .kind = request.kind});
    call->has_called = false;
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
        return add_string(machine->state, request.default_text, request.atom);
    }
    const fixed_text fixed = read_fixed_text(lowering, attribute);
    if (fixed.is_fixed) {
        return add_string(machine->state, fixed.text, request.atom);
    }
    call->has_called = true;
    return sxml_xslt_lowering_call_value_template(
        machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                              .expression = attribute->value.payload.expression,
                                              .block = request.frame->block,
                                              .atom = request.atom,
                                              .focus = NULL});
}

/* ------------------------------------------------------ xsl:sequence */

/* xsl:sequence (10.4.1): its select expression or sequence constructor. */
typedef struct sequence_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
} sequence_locals;

static sxml_status continue_sequence(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sequence_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        value_source source = {.has_select = false};
        return start_value(machine,
                           (value_request){.head = &locals->head,
                                           .frame = frame,
                                           .atom = &locals->value,
                                           .conflict_error =
                                               "XTSE3185 xsl:sequence has both a select attribute "
                                               "and content (XSLT 4.0, 10.4.1)"},
                           &source);
    }
    return finish_with_atom(machine, frame, locals->value);
}

static const sxml_xpath_lowering_procedure sequence_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:sequence"),
    .step = continue_sequence,
    .locals_size = sizeof(sequence_locals)};

/* xsl:select (10.4.2): the expression that is its text. */
static sxml_status continue_select(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sequence_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        return sxml_xslt_lowering_call_expression(
            machine,
            &(sxml_xslt_expression_call){
                .lowering = lowering,
                .expression = node_of(lowering, locals->head.node)->content.payload.expression,
                .block = frame->block,
                .atom = &locals->value,
                .focus = NULL});
    }
    return finish_with_atom(machine, frame, locals->value);
}

static const sxml_xpath_lowering_procedure select_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:select"),
    .step = continue_select,
    .locals_size = sizeof(sequence_locals)};

/* ----------------------------------------- Text, comments and the like */

/* What a simple-content instruction makes of its string. */
typedef enum text_instruction_kind {
    SXML_TEXT_INSTRUCTION_VALUE_OF,
    SXML_TEXT_INSTRUCTION_COMMENT,
    SXML_TEXT_INSTRUCTION_PROCESSING_INSTRUCTION,
    SXML_TEXT_INSTRUCTION_NAMESPACE,
} text_instruction_kind;

/* xsl:value-of and xsl:text (11.4.2, 11.4.3: its text node, or with
   select, 4.0, as xsl:value-of), xsl:comment
   (11.8), xsl:processing-instruction (11.6), xsl:namespace (11.7): a name
   (the last three), the value, then the constructor of the string. */
typedef struct text_instruction_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom name;
    sxml_ir_atom value;
    sxml_ir_atom separator;
    bool has_select;
    /* xsl:value-of and xsl:text: the cdata attribute (11.4.3), a constant
       answer, or computed into cdata when has_computed_cdata. */
    bool is_cdata_section;
    bool has_computed_cdata;
    sxml_ir_atom cdata;
} text_instruction_locals;

static text_instruction_kind text_instruction_kind_of(sxml_xslt_ast_kind kind) {
    switch ((uint32_t)kind) {
    case SXML_XSLT_AST_KIND_COMMENT:
        return SXML_TEXT_INSTRUCTION_COMMENT;
    case SXML_XSLT_AST_KIND_PROCESSING_INSTRUCTION:
        return SXML_TEXT_INSTRUCTION_PROCESSING_INSTRUCTION;
    case SXML_XSLT_AST_KIND_NAMESPACE:
        return SXML_TEXT_INSTRUCTION_NAMESPACE;
    default:
        return SXML_TEXT_INSTRUCTION_VALUE_OF;
    }
}

static const char *conflict_error_of(text_instruction_kind kind) {
    switch (kind) {
    case SXML_TEXT_INSTRUCTION_VALUE_OF:
        return "XTSE0870 xsl:value-of or xsl:text has both a select attribute and content "
               "(XSLT 4.0, 11.4.2)";
    case SXML_TEXT_INSTRUCTION_COMMENT:
        return "XTSE0940 xsl:comment has both a select attribute and content (XSLT 4.0, 11.8)";
    case SXML_TEXT_INSTRUCTION_PROCESSING_INSTRUCTION:
        return "XTSE0880 xsl:processing-instruction has both a select attribute and content "
               "(XSLT 4.0, 11.6)";
    case SXML_TEXT_INSTRUCTION_NAMESPACE:
        return "XTSE0910 xsl:namespace has both a select attribute and content (XSLT 4.0, "
               "11.7)";
    }
    return NULL;
}

/* The comment's string with a space after each hyphen followed by a
   hyphen or ending it (11.8): "--" twice (for runs of three and more),
   then a final "-". */
static sxml_status fix_comment_text(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_block *block,
                                    sxml_ir_atom *value) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t pass_index = 0; status == SXML_STATUS_OK && pass_index < 2; pass_index += 1) {
        status = bind_replace(lowerer,
                              (replace_request){.block = block,
                                                .value = *value,
                                                .pattern = SXML_BYTE_VIEW_FROM_LITERAL("--"),
                                                .replacement = SXML_BYTE_VIEW_FROM_LITERAL("- -")},
                              value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_replace(lowerer,
                        (replace_request){.block = block,
                                          .value = *value,
                                          .pattern = SXML_BYTE_VIEW_FROM_LITERAL("-$"),
                                          .replacement = SXML_BYTE_VIEW_FROM_LITERAL("- ")},
                        value);
}

/* 5.7.2, step 6, and 11.6: leading whitespace removed, "?>" made "? >". */
static sxml_status fix_instruction_text(sxml_xpath_lowerer *lowerer,
                                        sxml_xpath_lowering_block *block, sxml_ir_atom *value) {
    const sxml_status status =
        bind_replace(lowerer,
                     (replace_request){.block = block,
                                       .value = *value,
                                       .pattern = SXML_BYTE_VIEW_FROM_LITERAL("^\\s+"),
                                       .replacement = SXML_BYTE_VIEW_FROM_LITERAL("")},
                     value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_replace(lowerer,
                        (replace_request){.block = block,
                                          .value = *value,
                                          .pattern = SXML_BYTE_VIEW_FROM_LITERAL("\\?>"),
                                          .replacement = SXML_BYTE_VIEW_FROM_LITERAL("? >")},
                        value);
}

static const sxml_xslt_error_mapping processing_instruction_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XQDY0041"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0890"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "the name of xsl:processing-instruction is not an NCName (XSLT 4.0, 11.6)")},
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XQDY0064"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0890"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "the name of xsl:processing-instruction is xml in some case (XSLT 4.0, 11.6)")},
};

static const sxml_xslt_error_mapping namespace_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XQDY0074"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0920"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "the name of xsl:namespace is neither empty nor an NCName, or it is xmlns (XSLT 4.0, "
         "11.7)")},
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XQDY0101"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0925"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "xsl:namespace binds xml or xmlns, or another prefix to their namespaces (XSLT 4.0, "
         "11.7)")},
};

/* XTDE0930 (11.7): an xsl:namespace whose value is "". */
static sxml_status bind_checked_namespace_uri(sxml_xpath_lowerer *lowerer,
                                              sxml_xpath_lowering_block *block,
                                              sxml_ir_atom *value) {
    sxml_ir_atom is_empty = {0};
    sxml_status status =
        bind_call(lowerer,
                  (call_request){.block = block,
                                 .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string-length"),
                                 .arguments = {.atoms = value, .atom_count = 1}},
                  &is_empty);
    sxml_ir_atom zero = {0};
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_from_integer(0), &zero);
    }
    if (status == SXML_STATUS_OK) {
        status = bind(
            lowerer,
            (sxml_xslt_expression_to_bind){
                .block = block,
                .expression =
                    (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                                         .binary = {.operator= SXML_IR_BINARY_OPERATOR_VALUE_EQUAL,
                                                    .left_operand = is_empty,
                                                    .right_operand = zero}}},
            &is_empty);
    }
    sxml_ir_expression_index raise = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_raise(
            lowerer,
            (sxml_xpath_lowering_raise_request){
                .node = no_ast_node,
                .error_status = SXML_STATUS_DYNAMIC_ERROR,
                .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XTDE0930"),
                .description = SXML_BYTE_VIEW_FROM_LITERAL(
                    "the value of xsl:namespace is a zero-length string (XSLT 4.0, 11.7)")},
            &raise);
    }
    sxml_ir_expression_index kept = {0};
    if (status == SXML_STATUS_OK) {
        status = add_atom_content(lowerer, *value, &kept);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(lowerer,
                (sxml_xslt_expression_to_bind){
                    .block = block,
                    .expression =
                        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                             .conditional = {.condition = is_empty,
                                                             .then_branch = raise,
                                                             .else_branch = kept}}},
                value);
}

/* The text node of xsl:value-of or xsl:text: one asking for CDATA
   sections when the cdata attribute is yes (11.4.3), chosen at run time
   when the attribute is computed. */
static sxml_status finish_text_node(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame, sxml_ir_atom text) {
    sxml_xpath_lowerer *lowerer = machine->state;
    const text_instruction_locals *locals = frame->locals;
    const sxml_ir_expression plain = {.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                      .node_construction = {.value = text}};
    sxml_ir_expression cdata_section = plain;
    cdata_section.node_construction.construction_flags =
        (uint32_t)SXML_IR_CONSTRUCTION_CDATA_SECTION;
    if (!locals->has_computed_cdata) {
        return finish_with_expression(machine, frame,
                                      locals->is_cdata_section ? cdata_section : plain);
    }
    sxml_ir_atom is_yes = {0};
    sxml_status status = sxml_xslt_lowering_bind_boolean_value(
        lowerer, (sxml_xslt_boolean_value){.block = frame->block, .value = locals->cdata},
        &is_yes);
    sxml_ir_expression_index then_branch = {0};
    sxml_ir_expression_index else_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, cdata_section, &then_branch);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, plain, &else_branch);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(
        machine, frame,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                             .conditional = {.condition = is_yes,
                                             .then_branch = then_branch,
                                             .else_branch = else_branch}});
}

/* The cdata attribute of xsl:value-of and xsl:text (11.4.3): a fixed yes
   or no is decided here; a computed one is lowered to locals->cdata
   (*has_called true). */
static sxml_status start_cdata(sxml_xpath_lowering_machine *machine,
                               sxml_xpath_lowering_frame *frame, template_call *call) {
    text_instruction_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_attribute *attribute = find_attribute(
        lowering,
        (attribute_lookup){.node = locals->head.node, .kind = SXML_XSLT_AST_ATTRIBUTE_CDATA});
    call->has_called = false;
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
        return SXML_STATUS_OK;
    }
    const fixed_text fixed = read_fixed_text(lowering, attribute);
    if (fixed.is_fixed) {
        const sxml_xslt_boolean_text answer = sxml_xslt_lowering_read_boolean_text(fixed.text);
        if (!answer.is_valid) {
            return sxml_xslt_lowering_report_static_error(
                lowering, (sxml_xslt_static_error){
                              .node = locals->head.node,
                              .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XTSE0020 the cdata attribute is not yes or no (XSLT 4.0, "
                                  "11.4.3)")});
        }
        locals->is_cdata_section = answer.is_yes;
        return SXML_STATUS_OK;
    }
    call->has_called = true;
    locals->has_computed_cdata = true;
    return sxml_xslt_lowering_call_value_template(
        machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                              .expression = attribute->value.payload.expression,
                                              .block = frame->block,
                                              .atom = &locals->cdata,
                                              .focus = NULL});
}

/* The constructor of the instruction's string, as the frame's result. */
static sxml_status finish_text_instruction(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    text_instruction_locals *locals = frame->locals;
    const text_instruction_kind kind =
        text_instruction_kind_of(node_of(locals->head.lowering, locals->head.node)->kind);
    sxml_ir_atom text = {0};
    sxml_status status = sxml_xslt_lowering_bind_simple_content(
        lowerer,
        (sxml_xslt_simple_content_request){
            .block = frame->block, .value = locals->value, .separator = locals->separator},
        &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (kind) {
    case SXML_TEXT_INSTRUCTION_VALUE_OF:
        return finish_text_node(machine, frame, text);
    case SXML_TEXT_INSTRUCTION_COMMENT:
        status = fix_comment_text(lowerer, frame->block, &text);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return finish_with_expression(
            machine, frame,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
                                 .node_construction = {.value = text}});
    case SXML_TEXT_INSTRUCTION_PROCESSING_INSTRUCTION:
    case SXML_TEXT_INSTRUCTION_NAMESPACE:
        break;
    }
    const bool is_processing_instruction = kind == SXML_TEXT_INSTRUCTION_PROCESSING_INSTRUCTION;
    status = is_processing_instruction ? fix_instruction_text(lowerer, frame->block, &text)
                                       : bind_checked_namespace_uri(lowerer, frame->block, &text);
    sxml_ir_atom constructed = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_mapped_expression(
            lowerer,
            &(sxml_xslt_mapped_expression){
                .block = frame->block,
                .expression = {.expression_kind =
                                   is_processing_instruction
                                       ? SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR
                                       : SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
                               .node_construction = {.name = locals->name, .value = text}},
                .mappings = {.mappings = is_processing_instruction ? processing_instruction_mappings
                                                                   : namespace_mappings,
                             .mapping_count = 2}},
            &constructed);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, constructed);
}

static sxml_status continue_text_instruction(sxml_xpath_lowering_machine *machine,
                                             sxml_xpath_lowering_frame *frame) {
    text_instruction_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const text_instruction_kind kind =
        text_instruction_kind_of(node_of(lowering, locals->head.node)->kind);
    sxml_status status = SXML_STATUS_OK;
    template_call call = {.has_called = false};
    if (frame->step == 0) {
        frame->step = 1;
        if (kind == SXML_TEXT_INSTRUCTION_PROCESSING_INSTRUCTION ||
            kind == SXML_TEXT_INSTRUCTION_NAMESPACE) {
            status = start_template_or_default(
                machine,
                (template_request){.head = &locals->head,
                                   .frame = frame,
                                   .kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
                                   .default_text = {.bytes = NULL, .byte_count = 0},
                                   .atom = &locals->name},
                &call);
            if (status != SXML_STATUS_OK || call.has_called) {
                return status;
            }
        }
    }
    if (frame->step == 1) {
        frame->step = 2;
        value_source source = {.has_select = false};
        status = start_value(machine,
                             (value_request){.head = &locals->head,
                                             .frame = frame,
                                             .atom = &locals->value,
                                             .conflict_error = conflict_error_of(kind)},
                             &source);
        locals->has_select = source.has_select;
        return status;
    }
    if (frame->step == 2) {
        frame->step = 3;
        /* 5.7.2: a separator only for xsl:value-of and xsl:text (default
           " " with select, "" for content); otherwise a single space. */
        status = start_template_or_default(
            machine,
            (template_request){
                .head = &locals->head,
                .frame = frame,
                .kind = kind == SXML_TEXT_INSTRUCTION_VALUE_OF ? SXML_XSLT_AST_ATTRIBUTE_SEPARATOR
                                                               : SXML_XSLT_AST_ATTRIBUTE_NONE,
                .default_text = kind != SXML_TEXT_INSTRUCTION_VALUE_OF || locals->has_select
                                    ? SXML_BYTE_VIEW_FROM_LITERAL(" ")
                                    : SXML_BYTE_VIEW_FROM_LITERAL(""),
                .atom = &locals->separator},
            &call);
        if (status != SXML_STATUS_OK || call.has_called) {
            return status;
        }
    }
    if (frame->step == 3) {
        frame->step = 4;
        if (kind == SXML_TEXT_INSTRUCTION_VALUE_OF) {
            status = start_cdata(machine, frame, &call);
            if (status != SXML_STATUS_OK || call.has_called) {
                return status;
            }
        }
    }
    return finish_text_instruction(machine, frame);
}

static const sxml_xpath_lowering_procedure text_instruction_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt text instruction"),
    .step = continue_text_instruction,
    .locals_size = sizeof(text_instruction_locals)};

/* --------------------------------------------- Names of xsl:element and
   xsl:attribute */

/* A name to resolve while lowering. */
typedef struct name_resolution {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    sxml_byte_view lexical_name;
    /* The namespace attribute's fixed text; is_fixed false without one. */
    bool has_namespace;
    sxml_byte_view namespace_text;
    bool is_attribute;
} name_resolution;

/* The outcome: a QName, or the code of the error the instruction raises. */
typedef struct resolved_name {
    sxml_name_identifier expanded_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    const char *error_code;
} resolved_name;

static bool is_ncname(sxml_byte_view text) {
    return text.byte_count > 0 && sxml_xml_text_is_name(text) &&
           memchr(text.bytes, ':', text.byte_count) == NULL;
}

static sxml_status resolve_fixed_name(sxml_xpath_lowerer *lowerer, const name_resolution *request,
                                      resolved_name *name) {
    const char *const invalid_code = request->is_attribute ? "XTDE0850" : "XTDE0820";
    const sxml_byte_view lexical = sxml_xml_text_trim_whitespace(request->lexical_name);
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view local = lexical;
    const uint8_t *colon =
        lexical.byte_count > 0 ? memchr(lexical.bytes, ':', lexical.byte_count) : NULL;
    if (colon != NULL) {
        prefix =
            (sxml_byte_view){.bytes = lexical.bytes, .byte_count = (size_t)(colon - lexical.bytes)};
        local = (sxml_byte_view){.bytes = colon + 1,
                                 .byte_count = lexical.byte_count - prefix.byte_count - 1};
    }
    *name = (resolved_name){.expanded_name = {0}, .namespace_prefix = {0}, .error_code = NULL};
    if (!is_ncname(local) || (colon != NULL && !is_ncname(prefix))) {
        name->error_code = invalid_code;
        return SXML_STATUS_OK;
    }
    sxml_namespace_uri_identifier uri = {.index = SXML_NAMESPACE_NONE};
    sxml_status status = SXML_STATUS_OK;
    if (request->has_namespace) {
        status = sxml_name_pool_intern_namespace_uri(
            lowerer->name_pool, sxml_xml_text_trim_whitespace(request->namespace_text), &uri);
        if (uri.index == SXML_NAMESPACE_NONE) {
            /* A zero-length namespace: no namespace, the prefix dropped. */
            prefix = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        }
    } else {
        if (request->is_attribute && colon == NULL &&
            sxml_byte_view_equals(local, SXML_BYTE_VIEW_FROM_LITERAL("xmlns"))) {
            name->error_code = "XTDE0855";
            return SXML_STATUS_OK;
        }
        sxml_namespace_prefix_identifier prefix_identifier = {.index = 0};
        status =
            sxml_name_pool_intern_namespace_prefix(lowerer->name_pool, prefix, &prefix_identifier);
        sxml_namespace_binding_view in_scope = {.namespace_bindings = NULL,
                                                .namespace_binding_count = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_get_element_namespaces(request->lowering, request->node,
                                                               &in_scope);
        }
        /* 11.2, 11.3: an unprefixed element name is in the default
           namespace, an unprefixed attribute name in none. */
        bool is_bound = colon == NULL;
        for (size_t binding_index = 0;
             status == SXML_STATUS_OK && binding_index < in_scope.namespace_binding_count;
             binding_index += 1) {
            const sxml_namespace_binding binding = in_scope.namespace_bindings[binding_index];
            if (binding.namespace_prefix.index == prefix_identifier.index &&
                (colon != NULL || !request->is_attribute)) {
                uri = binding.namespace_uri;
                is_bound = true;
            }
        }
        if (status == SXML_STATUS_OK && colon != NULL &&
            sxml_byte_view_equals(prefix, SXML_BYTE_VIEW_FROM_LITERAL("xml"))) {
            uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML);
            is_bound = true;
        }
        if (status == SXML_STATUS_OK && !is_bound) {
            name->error_code = request->is_attribute ? "XTDE0860" : "XTDE0830";
            return SXML_STATUS_OK;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            lowerer->name_pool, &(sxml_name_request){.namespace_uri = uri, .local_name = local},
            &name->expanded_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(lowerer->name_pool, prefix,
                                                        &name->namespace_prefix);
    }
    return status;
}

/* The name atom of a constructor whose name and namespace attributes are
   fixed: an xs:QName constant, or a RAISE of the instruction's error. */
typedef struct fixed_name_request {
    name_resolution resolution;
    sxml_xpath_lowering_block *block;
} fixed_name_request;

static sxml_status bind_fixed_name(sxml_xpath_lowerer *lowerer, const fixed_name_request *request,
                                   sxml_ir_atom *atom) {
    resolved_name name = {.expanded_name = {0}, .namespace_prefix = {0}, .error_code = NULL};
    sxml_status status = resolve_fixed_name(lowerer, &request->resolution, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (name.error_code != NULL) {
        return bind_raise(
            lowerer,
            (raise_request){.block = request->block,
                            .code = {.bytes = (const uint8_t *)name.error_code, .byte_count = 8},
                            .description = SXML_BYTE_VIEW_FROM_LITERAL(
                                "the name of xsl:element or xsl:attribute is not "
                                "a lexical QName with a declared prefix (XSLT 4.0, "
                                "11.2, 11.3)")},
            atom);
    }
    sxml_item item = {.representation = 0};
    status = sxml_atomic_item_make(
        lowerer->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = name.expanded_name,
                                                  .namespace_prefix = name.namespace_prefix}}}},
        &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_item_constant(lowerer, item, atom);
}

/* The in-scope namespaces a computed name resolves with: the
   instruction's in the stylesheet, without the default namespace for an
   attribute (11.3). Owned memory 3 holds a filtered copy. */
typedef struct name_namespaces_request {
    const sxml_xslt_instruction_locals *head;
    sxml_xpath_lowering_frame *frame;
    bool is_attribute;
} name_namespaces_request;

static sxml_status add_name_namespaces(sxml_xpath_lowering_machine *machine,
                                       name_namespaces_request request,
                                       sxml_ir_namespace_binding_list *list) {
    sxml_namespace_binding_view in_scope = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status = sxml_xslt_lowering_get_element_namespaces(request.head->lowering,
                                                                   request.head->node, &in_scope);
    if (status != SXML_STATUS_OK || in_scope.namespace_binding_count == 0) {
        return status;
    }
    status = sxml_xpath_lowering_allocate_owned(
        machine, request.frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 3,
            .scratch = {.element_count = in_scope.namespace_binding_count,
                        .element_size = sizeof(sxml_namespace_binding),
                        .element_alignment = alignof(sxml_namespace_binding)}});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_namespace_binding *kept = request.frame->owned_memory[3];
    size_t kept_count = 0;
    for (size_t binding_index = 0; binding_index < in_scope.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = in_scope.namespace_bindings[binding_index];
        if (binding.namespace_uri.index == SXML_NAMESPACE_XML ||
            (request.is_attribute && binding.namespace_prefix.index == 0)) {
            continue;
        }
        kept[kept_count] = binding;
        kept_count += 1;
    }
    if (kept_count == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_ir_unit_add_namespace_bindings(
        machine->state->unit,
        (sxml_namespace_binding_view){.namespace_bindings = kept,
                                      .namespace_binding_count = kept_count},
        list);
}

static const sxml_xslt_error_mapping element_name_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XQDY0074"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0820"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "the name of xsl:element is not a lexical QName with a declared prefix (XSLT 4.0, "
         "11.2)")},
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("FOCA0002"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0820"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "the name of xsl:element is not a lexical QName (XSLT 4.0, 11.2)")},
};

static const sxml_xslt_error_mapping attribute_name_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XQDY0074"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0850"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "the name of xsl:attribute is not a lexical QName with a declared prefix (XSLT 4.0, "
         "11.3)")},
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XQDY0044"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0855"),
     .description =
         SXML_BYTE_VIEW_INITIALIZER("the name of xsl:attribute is xmlns (XSLT 4.0, 11.3)")},
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("FOCA0002"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0850"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "the name of xsl:attribute is not a lexical QName (XSLT 4.0, 11.3)")},
};

/* -------------------------------------------- xsl:element, xsl:attribute */

/* xsl:element (11.2) and xsl:attribute (11.3): the name, the namespace,
   then the content (and separator), then the constructor. */
typedef struct named_constructor_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom name;
    sxml_ir_atom namespace_uri;
    sxml_ir_atom value;
    sxml_ir_atom separator;
    bool is_name_fixed;
    bool has_select;
    sxml_ir_namespace_binding_list name_namespaces;
    /* xsl:element: the block its content is computed in, inside the
       element constructor (D166). */
    sxml_xpath_lowering_block content_block;
} named_constructor_locals;

/* An ATOM of value at the end of content_block: a node constructor's
   content expression (D166). */
typedef struct block_content {
    const sxml_xpath_lowering_block *content_block;
    sxml_ir_atom value;
} block_content;

static sxml_status add_block_content(sxml_xpath_lowerer *lowerer, block_content request,
                                     sxml_ir_expression_index *content) {
    sxml_ir_expression_index result = {0};
    const sxml_status status = add_atom_content(lowerer, request.value, &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        lowerer,
        (sxml_xpath_lowering_block_result){.block = *request.content_block, .result = result},
        content);
}

static bool is_attribute_instruction(const named_constructor_locals *locals) {
    return node_of(locals->head.lowering, locals->head.node)->kind == SXML_XSLT_AST_KIND_ATTRIBUTE;
}

/* Step 0: a fixed name (with a fixed namespace) at once; else the name's
   template. */
static sxml_status start_name(sxml_xpath_lowering_machine *machine,
                              sxml_xpath_lowering_frame *frame, template_call *call) {
    named_constructor_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_attribute *name =
        find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                    .kind = SXML_XSLT_AST_ATTRIBUTE_NAME});
    const sxml_xslt_ast_attribute *namespace_attribute =
        find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                    .kind = SXML_XSLT_AST_ATTRIBUTE_NAMESPACE});
    const fixed_text fixed_name = read_fixed_text(lowering, name);
    const bool has_namespace = namespace_attribute->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
    const fixed_text fixed_namespace = read_fixed_text(lowering, namespace_attribute);
    call->has_called = false;
    if (fixed_name.is_fixed && (!has_namespace || fixed_namespace.is_fixed)) {
        locals->is_name_fixed = true;
        return bind_fixed_name(
            machine->state,
            &(fixed_name_request){.resolution = {.lowering = lowering,
                                                 .node = locals->head.node,
                                                 .lexical_name = fixed_name.text,
                                                 .has_namespace = has_namespace,
                                                 .namespace_text = fixed_namespace.text,
                                                 .is_attribute = is_attribute_instruction(locals)},
                                  .block = frame->block},
            &locals->name);
    }
    return start_template_or_default(
        machine,
        (template_request){.head = &locals->head,
                           .frame = frame,
                           .kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
                           .default_text = {.bytes = NULL, .byte_count = 0},
                           .atom = &locals->name},
        call);
}

/* With a namespace attribute, the lexical name's local part when the
   namespace is "" (11.2, 11.3: then the name is in no namespace and its
   prefix is dropped), the name itself otherwise. */
static sxml_status bind_name_for_namespace(sxml_xpath_lowerer *lowerer,
                                           sxml_xpath_lowering_block *block,
                                           named_constructor_locals *locals) {
    sxml_ir_atom length = {0};
    sxml_status status =
        bind_call(lowerer,
                  (call_request){.block = block,
                                 .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string-length"),
                                 .arguments = {.atoms = &locals->namespace_uri, .atom_count = 1}},
                  &length);
    sxml_ir_atom zero = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_from_integer(0),
                                                      &zero);
    }
    sxml_ir_atom is_empty = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (sxml_xslt_expression_to_bind){
                          .block = block,
                          .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                                         .binary = {.operator= SXML_IR_BINARY_OPERATOR_VALUE_EQUAL,
                                                    .left_operand = length,
                                                    .right_operand = zero}}},
                      &is_empty);
    }
    branch local_part = {.block = {.first = {0}, .open_let = {0}}, .result = {0}};
    sxml_ir_atom local = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_replace(lowerer,
                              (replace_request){.block = &local_part.block,
                                                .value = locals->name,
                                                .pattern = SXML_BYTE_VIEW_FROM_LITERAL("^[^:]*:"),
                                                .replacement = SXML_BYTE_VIEW_FROM_LITERAL("")},
                              &local);
    }
    if (status == SXML_STATUS_OK) {
        status = add_atom_content(lowerer, local, &local_part.result);
    }
    sxml_ir_expression_index then_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, &local_part, &then_branch);
    }
    sxml_ir_expression_index else_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = add_atom_content(lowerer, locals->name, &else_branch);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(lowerer,
                (sxml_xslt_expression_to_bind){
                    .block = block,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                   .conditional = {.condition = is_empty,
                                                   .then_branch = then_branch,
                                                   .else_branch = else_branch}}},
                &locals->name);
}

/* The computed name: an xs:string resolved by the evaluator, or with a
   namespace, fn:QName($namespace, $name). */
static sxml_status bind_computed_name(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame) {
    named_constructor_locals *locals = frame->locals;
    const bool has_namespace = has_attribute(
        locals->head.lowering,
        (attribute_lookup){.node = locals->head.node, .kind = SXML_XSLT_AST_ATTRIBUTE_NAMESPACE});
    if (!has_namespace) {
        return add_name_namespaces(
            machine,
            (name_namespaces_request){.head = &locals->head,
                                      .frame = frame,
                                      .is_attribute = is_attribute_instruction(locals)},
            &locals->name_namespaces);
    }
    const sxml_status status = bind_name_for_namespace(machine->state, frame->block, locals);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_atom arguments[2] = {locals->namespace_uri, locals->name};
    return bind_call(machine->state,
                     (call_request){.block = frame->block,
                                    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("QName"),
                                    .arguments = {.atoms = arguments, .atom_count = 2}},
                     &locals->name);
}

static sxml_status finish_named_constructor(sxml_xpath_lowering_machine *machine,
                                            sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    named_constructor_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const bool is_attribute = is_attribute_instruction(locals);
    sxml_status status = SXML_STATUS_OK;
    if (!locals->is_name_fixed) {
        status = bind_computed_name(machine, frame);
    }
    sxml_ir_expression expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_NONE};
    if (status == SXML_STATUS_OK && is_attribute) {
        sxml_ir_atom text = {0};
        status = sxml_xslt_lowering_bind_simple_content(
            lowerer,
            (sxml_xslt_simple_content_request){
                .block = frame->block, .value = locals->value, .separator = locals->separator},
            &text);
        expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
            .node_construction = {
                .name = locals->name, .value = text, .name_namespaces = locals->name_namespaces}};
    } else if (status == SXML_STATUS_OK) {
        sxml_ir_expression_index content = {0};
        status = add_block_content(
            lowerer,
            (block_content){.content_block = &locals->content_block, .value = locals->value},
            &content);
        if (status == SXML_STATUS_OK) {
            /* 11.2: the attribute sets' attributes come first. */
            status = prepend_attribute_sets(
                lowerer,
                (attribute_set_content){
                    .lowering = lowering, .node = locals->head.node, .focus = NULL},
                &content);
        }
        uint32_t flags = (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES;
        const sxml_xslt_ast_attribute *inherit = find_attribute(
            lowering, (attribute_lookup){.node = locals->head.node,
                                         .kind = SXML_XSLT_AST_ATTRIBUTE_INHERIT_NAMESPACES});
        if (inherit->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
            !inherit->value.payload.is_true) {
            flags |= (uint32_t)SXML_IR_CONSTRUCTION_NO_INHERIT_NAMESPACES;
        }
        expression =
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
                                 .node_construction = {.name = locals->name,
                                                       .content = content,
                                                       .name_namespaces = locals->name_namespaces,
                                                       .construction_flags = flags,
                                                       .module_number =
                                                           sxml_xslt_lowering_module_of_node(
                                                               lowering, locals->head.node)}};
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (locals->is_name_fixed) {
        return finish_with_expression(machine, frame, expression);
    }
    sxml_ir_atom constructed = {0};
    status = sxml_xslt_lowering_bind_mapped_expression(
        lowerer,
        &(sxml_xslt_mapped_expression){
            .block = frame->block,
            .expression = expression,
            .mappings = {.mappings = is_attribute ? attribute_name_mappings : element_name_mappings,
                         .mapping_count = is_attribute ? 3U : 2U}},
        &constructed);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, constructed);
}

static sxml_status continue_named_constructor(sxml_xpath_lowering_machine *machine,
                                              sxml_xpath_lowering_frame *frame) {
    named_constructor_locals *locals = frame->locals;
    template_call call = {.has_called = false};
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        frame->step = 1;
        status = start_name(machine, frame, &call);
        if (status != SXML_STATUS_OK || call.has_called) {
            return status;
        }
    }
    if (frame->step == 1) {
        frame->step = 2;
        if (!locals->is_name_fixed) {
            status = start_template_or_default(
                machine,
                (template_request){.head = &locals->head,
                                   .frame = frame,
                                   .kind = SXML_XSLT_AST_ATTRIBUTE_NAMESPACE,
                                   .default_text = {.bytes = NULL, .byte_count = 0},
                                   .atom = &locals->namespace_uri},
                &call);
            if (status != SXML_STATUS_OK || call.has_called) {
                return status;
            }
        }
    }
    if (frame->step == 2) {
        frame->step = 3;
        value_source source = {.has_select = false};
        status = start_value(
            machine,
            (value_request){.head = &locals->head,
                            .frame = frame,
                            .atom = &locals->value,
                            .conflict_error = is_attribute_instruction(locals)
                                                  ? "XTSE0840 xsl:attribute has both a select "
                                                    "attribute and content (XSLT 4.0, 11.3)"
                                                  : NULL,
                            .content_block = is_attribute_instruction(locals)
                                                 ? NULL
                                                 : &locals->content_block},
            &source);
        locals->has_select = source.has_select;
        return status;
    }
    if (frame->step == 3) {
        frame->step = 4;
        if (is_attribute_instruction(locals)) {
            status = start_template_or_default(
                machine,
                (template_request){.head = &locals->head,
                                   .frame = frame,
                                   .kind = SXML_XSLT_AST_ATTRIBUTE_SEPARATOR,
                                   .default_text = locals->has_select
                                                       ? SXML_BYTE_VIEW_FROM_LITERAL(" ")
                                                       : SXML_BYTE_VIEW_FROM_LITERAL(""),
                                   .atom = &locals->separator},
                &call);
            if (status != SXML_STATUS_OK || call.has_called) {
                return status;
            }
        }
    }
    return finish_named_constructor(machine, frame);
}

static const sxml_xpath_lowering_procedure named_constructor_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:element or xsl:attribute"),
    .step = continue_named_constructor,
    .locals_size = sizeof(named_constructor_locals)};

/* ------------------------------------------------------- xsl:document */

/* xsl:document (11.5): a document constructor of the content. */
typedef struct document_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
    /* The block the content is computed in, inside the constructor
       (D166). */
    sxml_xpath_lowering_block content_block;
} document_locals;

static sxml_status continue_document(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    document_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        value_source source = {.has_select = false};
        return start_value(machine,
                           (value_request){.head = &locals->head,
                                           .frame = frame,
                                           .atom = &locals->value,
                                           .conflict_error = NULL,
                                           .content_block = &locals->content_block},
                           &source);
    }
    sxml_ir_expression_index content = {0};
    const sxml_status status =
        add_block_content(
        machine->state,
        (block_content){.content_block = &locals->content_block, .value = locals->value},
        &content);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(
        machine, frame,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                             .node_construction = {.content = content,
                                                   .construction_flags =
                                                       (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES,
                                                   .module_number =
                                                       sxml_xslt_lowering_module_of_node(
                                                           locals->head.lowering,
                                                           locals->head.node)}});
}

static const sxml_xpath_lowering_procedure document_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:document"),
    .step = continue_document,
    .locals_size = sizeof(document_locals)};

/* ------------------------------------------------------- xsl:copy-of */

/* The copy flags of copy-namespaces="no" (11.9.1, 11.9.2). */
static uint32_t copy_flags_of(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    uint32_t flags = (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES;
    const sxml_xslt_ast_attribute *copy_namespaces = find_attribute(
        lowering,
        (attribute_lookup){.node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_COPY_NAMESPACES});
    if (copy_namespaces->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
        !copy_namespaces->value.payload.is_true) {
        flags |= (uint32_t)SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES;
    }
    const sxml_xslt_ast_attribute *inherit = find_attribute(
        lowering,
        (attribute_lookup){.node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_INHERIT_NAMESPACES});
    if (inherit->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
        !inherit->value.payload.is_true) {
        flags |= (uint32_t)SXML_IR_CONSTRUCTION_NO_INHERIT_NAMESPACES;
    }
    return flags;
}

/* The deep copy of item (11.9.2): an element or document by SHALLOW_COPY
   with its attributes and children as content (each copied in full by the
   constructor), another node by SHALLOW_COPY, which copies it whole;
   another item as it is. */
typedef struct deep_copy_request {
    sxml_ir_atom item;
    uint32_t flags;
} deep_copy_request;

static sxml_status add_parent_copy(sxml_xpath_lowerer *lowerer, deep_copy_request request,
                                   sxml_ir_expression_index *expression) {
    branch built = {.block = {.first = {0}, .open_let = {0}}, .result = {0}};
    sxml_ir_atom parts[2] = {{0}, {0}};
    sxml_status status = bind(
        lowerer,
        (sxml_xslt_expression_to_bind){
            .block = &built.block,
            .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_STEP,
                                               .step = {.origin = request.item,
                                                        .axis = SXML_AXIS_ATTRIBUTE,
                                                        .node_test = sxml_builtin_type_identifier(
                                                            SXML_TYPE_ATTRIBUTE_NODE)}}},
        &parts[0]);
    if (status == SXML_STATUS_OK) {
        status =
            bind(lowerer,
                 (sxml_xslt_expression_to_bind){
                     .block = &built.block,
                     .expression =
                         (sxml_ir_expression){
                             .expression_kind = SXML_IR_EXPRESSION_KIND_STEP,
                             .step = {.origin = request.item,
                                      .axis = SXML_AXIS_CHILD,
                                      .node_test = sxml_builtin_type_identifier(SXML_TYPE_NODE)}}},
                 &parts[1]);
    }
    sxml_ir_expression_index content = {0};
    if (status == SXML_STATUS_OK) {
        status = add_atom_sequence(lowerer, (sxml_ir_atom_view){.atoms = parts, .atom_count = 2},
                                   &content);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
                                 .node_construction = {.value = request.item,
                                                       .content = content,
                                                       .construction_flags = request.flags}},
            &built.result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_branch(lowerer, &built, expression);
}

static sxml_status add_other_copy(sxml_xpath_lowerer *lowerer, deep_copy_request request,
                                  sxml_ir_expression_index *expression) {
    branch built = {.block = {.first = {0}, .open_let = {0}}, .result = {0}};
    sxml_ir_atom is_node = {0};
    sxml_status status =
        bind_instance_of(lowerer,
                         (instance_request){.block = &built.block,
                                            .value = request.item,
                                            .item_type = SXML_TYPE_NODE,
                                            .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                         &is_node);
    sxml_ir_expression_index empty_content = {0};
    if (status == SXML_STATUS_OK) {
        status = add_atom_sequence(lowerer, (sxml_ir_atom_view){.atoms = NULL, .atom_count = 0},
                                   &empty_content);
    }
    sxml_ir_expression_index copied = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
                                 .node_construction = {.value = request.item,
                                                       .content = empty_content,
                                                       .construction_flags = request.flags}},
            &copied);
    }
    sxml_ir_expression_index kept = {0};
    if (status == SXML_STATUS_OK) {
        status = add_atom_content(lowerer, request.item, &kept);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                .conditional = {.condition = is_node, .then_branch = copied, .else_branch = kept}},
            &built.result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_branch(lowerer, &built, expression);
}

/* Binds the copies of value's items in block. */
typedef struct copy_of_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
    uint32_t flags;
} copy_of_request;

static sxml_status bind_copy_of(sxml_xpath_lowerer *lowerer, copy_of_request request,
                                sxml_ir_atom *atom) {
    sxml_ir_slot item_slot = {0};
    sxml_status status = sxml_xpath_lowerer_add_slot(lowerer, &item_slot);
    const sxml_ir_atom item = {.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = item_slot};
    const deep_copy_request copy = {.item = item, .flags = request.flags};
    /* if (. instance of element()) then copy with children
       else if (. instance of document-node()) then copy with children
       else copy or keep */
    branch body = {.block = {.first = {0}, .open_let = {0}}, .result = {0}};
    branch not_element = {.block = {.first = {0}, .open_let = {0}}, .result = {0}};
    sxml_ir_atom is_element = {0};
    sxml_ir_atom is_document = {0};
    sxml_ir_expression_index element_copy = {0};
    sxml_ir_expression_index document_copy = {0};
    sxml_ir_expression_index other_copy = {0};
    sxml_ir_expression_index else_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_instance_of(lowerer,
                                  (instance_request){.block = &body.block,
                                                     .value = item,
                                                     .item_type = SXML_TYPE_ELEMENT_NODE,
                                                     .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                                  &is_element);
    }
    if (status == SXML_STATUS_OK) {
        status = add_parent_copy(lowerer, copy, &element_copy);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_instance_of(lowerer,
                                  (instance_request){.block = &not_element.block,
                                                     .value = item,
                                                     .item_type = SXML_TYPE_DOCUMENT_NODE,
                                                     .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                                  &is_document);
    }
    if (status == SXML_STATUS_OK) {
        status = add_parent_copy(lowerer, copy, &document_copy);
    }
    if (status == SXML_STATUS_OK) {
        status = add_other_copy(lowerer, copy, &other_copy);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                 .conditional = {.condition = is_document,
                                                 .then_branch = document_copy,
                                                 .else_branch = other_copy}},
            &not_element.result);
    }
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, &not_element, &else_branch);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                 .conditional = {.condition = is_element,
                                                 .then_branch = element_copy,
                                                 .else_branch = else_branch}},
            &body.result);
    }
    sxml_ir_expression_index body_expression = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, &body, &body_expression);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(
        lowerer,
        (sxml_xslt_expression_to_bind){
            .block = request.block,
            .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                                               .map = {.input = request.value,
                                                       .focus = {.context_value = item_slot},
                                                       .body = body_expression,
                                                       .flags = 0}}},
        atom);
}

sxml_status sxml_xslt_lowering_bind_copy_of(sxml_xpath_lowerer *lowerer,
                                            sxml_xslt_copy_request request, sxml_ir_atom *atom) {
    return bind_copy_of(lowerer,
                        (copy_of_request){.block = request.block,
                                          .value = request.value,
                                          .flags = request.construction_flags},
                        atom);
}

/* xsl:copy-of (11.9.2). */
typedef struct copy_of_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
} copy_of_locals;

static sxml_status continue_copy_of(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame) {
    copy_of_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        value_source source = {.has_select = false};
        return start_value(machine,
                           (value_request){.head = &locals->head,
                                           .frame = frame,
                                           .atom = &locals->value,
                                           .conflict_error = NULL},
                           &source);
    }
    sxml_ir_atom copies = {0};
    sxml_status status =
        bind_copy_of(machine->state,
                     (copy_of_request){.block = frame->block,
                                       .value = locals->value,
                                       .flags = copy_flags_of(lowering, locals->head.node)},
                     &copies);
    const sxml_xslt_ast_attribute *copy_accumulators = find_attribute(
        lowering, (attribute_lookup){.node = locals->head.node,
                                     .kind = SXML_XSLT_AST_ATTRIBUTE_COPY_ACCUMULATORS});
    if (status == SXML_STATUS_OK && sxml_xslt_has_accumulators(lowering) &&
        copy_accumulators->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
        copy_accumulators->value.payload.is_true) {
        /* 19.10: the copies keep the accumulator values (packet 4.14). */
        status = sxml_xslt_bind_accumulator_link(
            machine->state,
            (sxml_xslt_accumulator_link_request){
                .block = frame->block, .copies = copies, .originals = locals->value},
            &copies);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, copies);
}

static const sxml_xpath_lowering_procedure copy_of_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:copy-of"),
    .step = continue_copy_of,
    .locals_size = sizeof(copy_of_locals)};

/* --------------------------------------------------------- xsl:copy */

/* xsl:copy (11.9.1): SHALLOW_COPY of the context item, or (4.0) of the
   selected item with it as the focus of the content; the content is a body
   of its own, which the evaluator runs only for an element or document. */
typedef struct copy_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom item;
    sxml_ir_expression_index content;
    sxml_xpath_lowering_focus selected_focus;
} copy_locals;

static sxml_status continue_copy(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    copy_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_attribute *select =
        find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                    .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT});
    const bool has_select = select->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
    if (frame->step == 0) {
        frame->step = 1;
        if (has_select) {
            return sxml_xslt_lowering_call_expression(
                machine,
                &(sxml_xslt_expression_call){.lowering = lowering,
                                             .expression = select->value.payload.expression,
                                             .block = frame->block,
                                             .atom = &locals->item,
                                             .focus = NULL});
        }
        const sxml_status status = sxml_xpath_lowerer_read_focus(
            lowerer,
            (sxml_xpath_lowering_focus_read){.block = frame->block,
                                             .node = no_ast_node,
                                             .part = SXML_XPATH_LOWERING_FOCUS_PART_VALUE},
            &locals->item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (frame->step == 1) {
        frame->step = 2;
        locals->selected_focus = (sxml_xpath_lowering_focus){
            .kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED, .fixed_value = locals->item};
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = sxml_xslt_lowering_first_constructor_child(lowering->ast,
                                                                             locals->head.node),
                         .block = NULL,
                         .is_body = true,
                         .destination = {.expression = &locals->content,
                                         .atom = NULL,
                                         .atom_block = NULL,
                                         .focused = NULL},
                         .focus = has_select ? &locals->selected_focus : NULL});
    }
    /* 11.9.1: the attribute sets' attributes come first, with the focus
       of the content. */
    const sxml_status status = prepend_attribute_sets(
        lowerer,
        (attribute_set_content){.lowering = lowering,
                                .node = locals->head.node,
                                .focus = has_select ? &locals->selected_focus : NULL},
        &locals->content);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(
        machine, frame,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
                             .node_construction = {.value = locals->item,
                                                   .content = locals->content,
                                                   .construction_flags = copy_flags_of(
                                                       lowering, locals->head.node)}});
}

static const sxml_xpath_lowering_procedure copy_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:copy"),
    .step = continue_copy,
    .locals_size = sizeof(copy_locals)};

/* ------------------------------------------------ Maps and records */

static const sxml_xslt_error_mapping map_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XQDY0137"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE3365"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "two maps of xsl:map or xsl:record have one key (XSLT 4.0, 21.1.2)")},
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
     .raised_status = SXML_STATUS_TYPE_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTTE3375"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "the content of xsl:map or xsl:record is not a sequence of maps (XSLT 4.0, 21.1)")},
};

/* xsl:map (21.1), xsl:record (4.0): a map constructor of the record's
   fields and the content's maps, merged; with [xsl:]duplicates,
   map:merge with that option (21.1.2). */
typedef struct map_locals {
    sxml_xslt_instruction_locals head;
    /* The next attribute offset to look at (xsl:record fields). */
    uint32_t attribute_offset;
    sxml_xslt_ast_attribute_index pending_field;
    /* One entry per field, then the content (owned memory 0). */
    sxml_ir_map_entry *entries;
    size_t entry_count;
    sxml_ir_atom content;
    sxml_ir_atom duplicates;
} map_locals;

/* The duplicates attribute: DUPLICATES (xsl:map, xsl:-prefixed on
   xsl:record). */
static const sxml_xslt_ast_attribute *duplicates_of(const map_locals *locals) {
    return find_attribute(
        locals->head.lowering,
        (attribute_lookup){.node = locals->head.node, .kind = SXML_XSLT_AST_ATTRIBUTE_DUPLICATES});
}

/* Adds the pending field's entry: its local name as key. */
static sxml_status add_field_entry(sxml_xpath_lowerer *lowerer, map_locals *locals,
                                   sxml_ir_atom value) {
    const sxml_xslt_ast_attribute *field =
        sxml_xslt_ast_get_attribute(locals->head.lowering->ast, locals->pending_field);
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status =
        sxml_name_pool_get_name_parts(lowerer->name_pool, field->attribute_name, &parts);
    sxml_ir_atom key = {0};
    if (status == SXML_STATUS_OK) {
        status = add_string(lowerer, parts.local_name, &key);
    }
    if (status == SXML_STATUS_OK) {
        locals->entries[locals->entry_count] = (sxml_ir_map_entry){.key = key, .value = value};
        locals->entry_count += 1;
    }
    locals->pending_field = (sxml_xslt_ast_attribute_index){.value = 0};
    return status;
}

static sxml_status finish_map(sxml_xpath_lowering_machine *machine,
                              sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    map_locals *locals = frame->locals;
    const bool has_duplicates = duplicates_of(locals)->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
    sxml_ir_map_entry_list entries = {0};
    sxml_ir_atom maps = locals->content;
    sxml_status status = SXML_STATUS_OK;
    if (has_duplicates) {
        /* map:merge((fields, content), {'duplicates': $duplicates}). */
        sxml_ir_atom fields = {0};
        status = sxml_ir_unit_add_map_entries(
            lowerer->unit,
            (sxml_ir_map_entry_view){.map_entries = locals->entries,
                                     .map_entry_count = locals->entry_count},
            &entries);
        if (status == SXML_STATUS_OK) {
            status = bind(
                lowerer,
                (sxml_xslt_expression_to_bind){
                    .block = frame->block,
                    .expression = (sxml_ir_expression){.expression_kind =
                                                           SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                                                       .map_constructor = {.entries = entries}}},
                &fields);
        }
        sxml_ir_expression_index both = {0};
        const sxml_ir_atom parts[2] = {fields, locals->content};
        if (status == SXML_STATUS_OK) {
            status = add_atom_sequence(lowerer,
                                       (sxml_ir_atom_view){.atoms = parts, .atom_count = 2}, &both);
        }
        if (status == SXML_STATUS_OK) {
            status =
                sxml_xpath_lowerer_bind_expression(lowerer,
                                                   (sxml_xpath_lowering_expression_in_block){
                                                       .block = frame->block, .expression = both},
                                                   &maps);
        }
        sxml_ir_map_entry option = {.key = {0}, .value = locals->duplicates};
        if (status == SXML_STATUS_OK) {
            status = add_string(lowerer, SXML_BYTE_VIEW_FROM_LITERAL("duplicates"), &option.key);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_add_map_entries(
                lowerer->unit,
                (sxml_ir_map_entry_view){.map_entries = &option, .map_entry_count = 1}, &entries);
        }
        sxml_ir_atom options = {0};
        if (status == SXML_STATUS_OK) {
            status = bind(
                lowerer,
                (sxml_xslt_expression_to_bind){
                    .block = frame->block,
                    .expression = (sxml_ir_expression){.expression_kind =
                                                           SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                                                       .map_constructor = {.entries = entries}}},
                &options);
        }
        const sxml_ir_atom arguments[2] = {maps, options};
        sxml_ir_atom merged = {0};
        if (status == SXML_STATUS_OK) {
            status = bind_call(lowerer,
                               (call_request){.block = frame->block,
                                              .function_namespace = SXML_NAMESPACE_MAP_FUNCTIONS,
                                              .local_name = SXML_BYTE_VIEW_FROM_LITERAL("merge"),
                                              .arguments = {.atoms = arguments, .atom_count = 2}},
                               &merged);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return finish_with_atom(machine, frame, merged);
    }
    /* A key with no value: the entries of the content's maps are added
       (src/ir_internal.h, sxml_ir_map_entry). */
    locals->entries[locals->entry_count] =
        (sxml_ir_map_entry){.key = locals->content, .value = {0}};
    locals->entry_count += 1;
    status = sxml_ir_unit_add_map_entries(
        lowerer->unit,
        (sxml_ir_map_entry_view){.map_entries = locals->entries,
                                 .map_entry_count = locals->entry_count},
        &entries);
    sxml_ir_atom map = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_mapped_expression(
            lowerer,
            &(sxml_xslt_mapped_expression){
                .block = frame->block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                               .map_constructor = {.entries = entries}},
                .mappings = {.mappings = map_mappings, .mapping_count = 2}},
            &map);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, map);
}

/* Starts the next field of xsl:record; *has_started false when none is
   left. */
static sxml_status start_next_field(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame, attribute_lookup *started) {
    map_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_attribute_list list = node_of(lowering, locals->head.node)->attributes;
    started->kind = SXML_XSLT_AST_ATTRIBUTE_NONE;
    while (locals->attribute_offset < list.attribute_count) {
        const sxml_xslt_ast_attribute_index index = {.value = list.first_attribute_index +
                                                              locals->attribute_offset};
        locals->attribute_offset += 1;
        const sxml_xslt_ast_attribute *attribute =
            sxml_xslt_ast_get_attribute(lowering->ast, index);
        if (attribute->kind != SXML_XSLT_AST_ATTRIBUTE_RECORD_FIELD) {
            continue;
        }
        locals->pending_field = index;
        started->kind = SXML_XSLT_AST_ATTRIBUTE_RECORD_FIELD;
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = attribute->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->content,
                                                  .focus = NULL});
    }
    return SXML_STATUS_OK;
}

static sxml_status continue_map(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    map_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_xslt_ast_node *node = node_of(lowering, locals->head.node);
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = (size_t)node->attributes.attribute_count + 1U,
                            .element_size = sizeof(sxml_ir_map_entry),
                            .element_alignment = alignof(sxml_ir_map_entry)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->entries = frame->owned_memory[0];
    } else if (frame->step == 1 && locals->pending_field.value != 0) {
        status = add_field_entry(lowerer, locals, locals->content);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (frame->step == 1) {
        attribute_lookup started = {.node = {0}, .kind = SXML_XSLT_AST_ATTRIBUTE_NONE};
        status = start_next_field(machine, frame, &started);
        if (status != SXML_STATUS_OK || started.kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
            return status;
        }
        frame->step = 2;
        value_source source = {.has_select = false};
        return start_value(machine,
                           (value_request){.head = &locals->head,
                                           .frame = frame,
                                           .atom = &locals->content,
                                           .conflict_error = NULL},
                           &source);
    }
    if (frame->step == 2) {
        frame->step = 3;
        const sxml_xslt_ast_attribute *duplicates = duplicates_of(locals);
        if (duplicates->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
            return sxml_xslt_lowering_call_expression(
                machine,
                &(sxml_xslt_expression_call){.lowering = lowering,
                                             .expression = duplicates->value.payload.expression,
                                             .block = frame->block,
                                             .atom = &locals->duplicates,
                                             .focus = NULL});
        }
    }
    return finish_map(machine, frame);
}

static const sxml_xpath_lowering_procedure map_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:map or xsl:record"),
    .step = continue_map,
    .locals_size = sizeof(map_locals)};

/* xsl:map-entry (21.1.1): a map of one entry. */
typedef struct map_entry_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom key;
    sxml_ir_atom value;
} map_entry_locals;

static sxml_status continue_map_entry(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    map_entry_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_xslt_ast_attribute *key =
            find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                        .kind = SXML_XSLT_AST_ATTRIBUTE_KEY});
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = key->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->key,
                                                  .focus = NULL});
    }
    if (frame->step == 1) {
        frame->step = 2;
        value_source source = {.has_select = false};
        return start_value(machine,
                           (value_request){.head = &locals->head,
                                           .frame = frame,
                                           .atom = &locals->value,
                                           .conflict_error =
                                               "XTSE3185 xsl:map-entry has both a select "
                                               "attribute and content (XSLT 4.0, 21.1.1)"},
                           &source);
    }
    const sxml_ir_map_entry entry = {.key = locals->key, .value = locals->value};
    sxml_ir_map_entry_list entries = {0};
    const sxml_status status = sxml_ir_unit_add_map_entries(
        lowerer->unit, (sxml_ir_map_entry_view){.map_entries = &entry, .map_entry_count = 1},
        &entries);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(
        machine, frame,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                             .map_constructor = {.entries = entries}});
}

static const sxml_xpath_lowering_procedure map_entry_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:map-entry"),
    .step = continue_map_entry,
    .locals_size = sizeof(map_entry_locals)};

/* ------------------------------------------------------------ Arrays */

/* The JNode a member value is held in (22.1): itself when it is one
   JNode (is_member_test), else fn:jtree of it. */
typedef struct member_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
} member_request;

static sxml_status bind_member(sxml_xpath_lowerer *lowerer, member_request request,
                               sxml_ir_atom *atom) {
    sxml_ir_atom is_json_node = {0};
    sxml_status status =
        bind_instance_of(lowerer,
                         (instance_request){.block = request.block,
                                            .value = request.value,
                                            .item_type = SXML_TYPE_JSON_NODE,
                                            .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                         &is_json_node);
    branch wrapped = {.block = {.first = {0}, .open_let = {0}}, .result = {0}};
    sxml_ir_atom tree = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (call_request){.block = &wrapped.block,
                                          .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("jtree"),
                                          .arguments = {.atoms = &request.value, .atom_count = 1}},
                           &tree);
    }
    if (status == SXML_STATUS_OK) {
        status = add_atom_content(lowerer, tree, &wrapped.result);
    }
    sxml_ir_expression_index else_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, &wrapped, &else_branch);
    }
    sxml_ir_expression_index then_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = add_atom_content(lowerer, request.value, &then_branch);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(lowerer,
                (sxml_xslt_expression_to_bind){
                    .block = request.block,
                    .expression =
                        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                             .conditional = {.condition = is_json_node,
                                                             .then_branch = then_branch,
                                                             .else_branch = else_branch}}},
                atom);
}

/* xsl:array (22.1): array:of-members over one JNode per member: without
   for-each, each item of the value (a JNode as it is, any other item in
   fn:jtree); with for-each, the value computed for each item of it as the
   focus. */
typedef struct array_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom for_each;
    sxml_ir_atom value;
    /* The members' body and the focus of the for-each items. */
    sxml_xpath_lowering_block body_block;
    sxml_xpath_lowering_focus item_focus;
} array_locals;

/* Binds the JNodes of the members of value (no for-each). */
static sxml_status bind_item_members(sxml_xpath_lowerer *lowerer, member_request request,
                                     sxml_ir_atom *atom) {
    sxml_ir_slot item_slot = {0};
    sxml_status status = sxml_xpath_lowerer_add_slot(lowerer, &item_slot);
    branch body = {.block = {.first = {0}, .open_let = {0}}, .result = {0}};
    sxml_ir_atom member = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_member(
            lowerer,
            (member_request){.block = &body.block,
                             .value = {.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = item_slot}},
            &member);
    }
    if (status == SXML_STATUS_OK) {
        status = add_atom_content(lowerer, member, &body.result);
    }
    sxml_ir_expression_index body_expression = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, &body, &body_expression);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(
        lowerer,
        (sxml_xslt_expression_to_bind){
            .block = request.block,
            .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                                               .map = {.input = request.value,
                                                       .focus = {.context_value = item_slot},
                                                       .body = body_expression,
                                                       .flags = 0}}},
        atom);
}

static sxml_status finish_array(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    array_locals *locals = frame->locals;
    const bool has_for_each = has_attribute(
        locals->head.lowering,
        (attribute_lookup){.node = locals->head.node, .kind = SXML_XSLT_AST_ATTRIBUTE_FOR_EACH});
    sxml_ir_atom members = {0};
    sxml_status status = SXML_STATUS_OK;
    if (has_for_each) {
        branch body = {.block = locals->body_block, .result = {0}};
        sxml_ir_atom member = {0};
        status = bind_member(
            lowerer, (member_request){.block = &body.block, .value = locals->value}, &member);
        if (status == SXML_STATUS_OK) {
            status = add_atom_content(lowerer, member, &body.result);
        }
        sxml_ir_expression_index body_expression = {0};
        if (status == SXML_STATUS_OK) {
            status = finish_branch(lowerer, &body, &body_expression);
        }
        if (status == SXML_STATUS_OK) {
            status =
                bind(lowerer,
                     (sxml_xslt_expression_to_bind){
                         .block = frame->block,
                         .expression =
                             (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                                                  .map = {.input = locals->for_each,
                                                          .focus = locals->item_focus.slots,
                                                          .body = body_expression,
                                                          .flags = 0}}},
                     &members);
        }
    } else {
        status = bind_item_members(
            lowerer, (member_request){.block = frame->block, .value = locals->value}, &members);
    }
    sxml_ir_atom array = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (call_request){.block = frame->block,
                                          .function_namespace = SXML_NAMESPACE_ARRAY_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("of-members"),
                                          .arguments = {.atoms = &members, .atom_count = 1}},
                           &array);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, array);
}

static sxml_status continue_array(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    array_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_attribute *for_each =
        find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                    .kind = SXML_XSLT_AST_ATTRIBUTE_FOR_EACH});
    const bool has_for_each = for_each->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
    if (frame->step == 0) {
        frame->step = 1;
        if (has_for_each) {
            return sxml_xslt_lowering_call_expression(
                machine,
                &(sxml_xslt_expression_call){.lowering = lowering,
                                             .expression = for_each->value.payload.expression,
                                             .block = frame->block,
                                             .atom = &locals->for_each,
                                             .focus = NULL});
        }
    }
    if (frame->step == 1) {
        frame->step = 2;
        const sxml_xslt_ast_attribute *select =
            find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                        .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT});
        if (has_for_each) {
            /* Each member is computed in a body of its own whose focus is
               the for-each item (22.1). */
            locals->item_focus =
                (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
            locals->body_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
            if (select->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
                return sxml_xslt_lowering_call_expression(
                    machine,
                    &(sxml_xslt_expression_call){.lowering = lowering,
                                                 .expression = select->value.payload.expression,
                                                 .block = &locals->body_block,
                                                 .atom = &locals->value,
                                                 .focus = &locals->item_focus});
            }
            return sxml_xslt_lowering_call_sequence_constructor(
                machine, &(sxml_xslt_sequence_constructor_call){
                             .lowering = lowering,
                             .first = node_of(lowering, locals->head.node)->first_child,
                             .block = &locals->body_block,
                             .is_body = false,
                             .destination = {.expression = NULL,
                                             .atom = &locals->value,
                                             .atom_block = &locals->body_block,
                                             .focused = NULL},
                             .focus = &locals->item_focus});
        }
        value_source source = {.has_select = false};
        return start_value(machine,
                           (value_request){.head = &locals->head,
                                           .frame = frame,
                                           .atom = &locals->value,
                                           .conflict_error =
                                               "XTSE3185 xsl:array has both a select attribute "
                                               "and content (XSLT 4.0, 22.1)"},
                           &source);
    }
    return finish_array(machine, frame);
}

static const sxml_xpath_lowering_procedure array_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:array"),
    .step = continue_array,
    .locals_size = sizeof(array_locals)};

/* xsl:array-member (22.1): fn:jtree of its value, a root JNode whose
   ·jvalue· is the value. */
typedef struct array_member_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
} array_member_locals;

static sxml_status continue_array_member(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame) {
    array_member_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        value_source source = {.has_select = false};
        return start_value(machine,
                           (value_request){.head = &locals->head,
                                           .frame = frame,
                                           .atom = &locals->value,
                                           .conflict_error =
                                               "XTSE3185 xsl:array-member has both a select "
                                               "attribute and content (XSLT 4.0, 22.1)"},
                           &source);
    }
    sxml_ir_atom tree = {0};
    const sxml_status status =
        bind_call(machine->state,
                  (call_request){.block = frame->block,
                                 .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("jtree"),
                                 .arguments = {.atoms = &locals->value, .atom_count = 1}},
                  &tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, tree);
}

static const sxml_xpath_lowering_procedure array_member_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:array-member"),
    .step = continue_array_member,
    .locals_size = sizeof(array_member_locals)};

/* ================================================================ Rules */

static const sxml_xslt_instruction_rule construct_rules[] = {
    {.kind = SXML_XSLT_AST_KIND_SEQUENCE, .procedure = &sequence_procedure},
    {.kind = SXML_XSLT_AST_KIND_SELECT, .procedure = &select_procedure},
    {.kind = SXML_XSLT_AST_KIND_ELEMENT, .procedure = &named_constructor_procedure},
    {.kind = SXML_XSLT_AST_KIND_ATTRIBUTE, .procedure = &named_constructor_procedure},
    {.kind = SXML_XSLT_AST_KIND_TEXT, .procedure = &text_instruction_procedure},
    {.kind = SXML_XSLT_AST_KIND_VALUE_OF, .procedure = &text_instruction_procedure},
    {.kind = SXML_XSLT_AST_KIND_COMMENT, .procedure = &text_instruction_procedure},
    {.kind = SXML_XSLT_AST_KIND_PROCESSING_INSTRUCTION, .procedure = &text_instruction_procedure},
    {.kind = SXML_XSLT_AST_KIND_NAMESPACE, .procedure = &text_instruction_procedure},
    {.kind = SXML_XSLT_AST_KIND_DOCUMENT, .procedure = &document_procedure},
    {.kind = SXML_XSLT_AST_KIND_COPY, .procedure = &copy_procedure},
    {.kind = SXML_XSLT_AST_KIND_COPY_OF, .procedure = &copy_of_procedure},
    {.kind = SXML_XSLT_AST_KIND_MAP, .procedure = &map_procedure},
    {.kind = SXML_XSLT_AST_KIND_RECORD, .procedure = &map_procedure},
    {.kind = SXML_XSLT_AST_KIND_MAP_ENTRY, .procedure = &map_entry_procedure},
    {.kind = SXML_XSLT_AST_KIND_ARRAY, .procedure = &array_procedure},
    {.kind = SXML_XSLT_AST_KIND_ARRAY_MEMBER, .procedure = &array_member_procedure},
};

const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_construct = {
    .rules = construct_rules, .rule_count = sizeof(construct_rules) / sizeof(construct_rules[0])};
