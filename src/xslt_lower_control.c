/*
 * XSLT 4.0 control flow and variables (src/xslt_lower_internal.h; XSLT
 * 4.0, sections 7, 8, 9, 10.5, 16.1, 23; packet 4.7, D145): xsl:if,
 * xsl:choose, xsl:switch, xsl:for-each, xsl:iterate with xsl:param,
 * xsl:next-iteration, xsl:break and xsl:on-completion, local xsl:variable,
 * xsl:try and xsl:catch, xsl:where-populated, xsl:on-empty,
 * xsl:on-non-empty, xsl:message, xsl:assert, xsl:fork and xsl:evaluate.
 *
 * Golden file: src/xslt_lower_construct.c (instructions as procedures
 * whose steps ask the machine for their parts and return, D71; atoms in
 * owned memory; branches in blocks of their own).
 *
 * Everything lowers to kinds XQuery already uses (D126): xsl:if, choose
 * and switch are CONDITIONAL chains whose later tests sit in the earlier
 * else branches; xsl:for-each is a MAP whose body is the sequence
 * constructor with the item as its focus; xsl:variable binds its value in
 * the sequence constructor's block (a LET) and puts it in scope until the
 * sequence constructor ends (D142); xsl:try is TRY_CATCH with the $err:
 * variables as LOOKUPs of the error details (D109); xsl:message and
 * xsl:assert call fn:message and fn:error. xsl:iterate is fn:scan over
 * the input with a closure, so it runs as a loop of the evaluator, never
 * as recursion (D71): see "Iteration" below.
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_lower_internal.h"

#include "function_library_core_internal.h"
#include "processor_internal.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* ============================================================ AST reads */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

static sxml_xslt_ast_kind kind_of(const sxml_xslt_lowering *lowering,
                                  sxml_xslt_ast_node_index node) {
    return node_of(lowering, node)->kind;
}

static sxml_xslt_ast_node_index next_of(const sxml_xslt_lowering *lowering,
                                        sxml_xslt_ast_node_index node) {
    return node_of(lowering, node)->next_sibling;
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

/* True when node has children other than xsl:fallback and xsl:catch (its
   content is not empty, 5.7; xsl:catch only under xsl:try). */
static bool has_content(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    for (sxml_xslt_ast_node_index child = node_of(lowering, node)->first_child; child.value != 0;
         child = next_of(lowering, child)) {
        const sxml_xslt_ast_kind kind = kind_of(lowering, child);
        if (kind != SXML_XSLT_AST_KIND_FALLBACK && kind != SXML_XSLT_AST_KIND_CATCH) {
            return true;
        }
    }
    return false;
}

/* The number of children of kind. */
typedef struct child_count_query {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_kind kind;
} child_count_query;

static size_t count_children(const sxml_xslt_lowering *lowering, child_count_query query) {
    size_t count = 0;
    for (sxml_xslt_ast_node_index child = node_of(lowering, query.node)->first_child;
         child.value != 0; child = next_of(lowering, child)) {
        count += kind_of(lowering, child) == query.kind ? 1U : 0U;
    }
    return count;
}

/* The NAME attribute's name of a variable-binding element. */
static sxml_name_identifier binding_name_of(const sxml_xslt_lowering *lowering,
                                            sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *name = find_attribute(
        lowering, (attribute_lookup){.node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_NAME});
    return name->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME ? name->value.payload.name
                                                             : (sxml_name_identifier){.index = 0};
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

static bool text_equals(sxml_byte_view text, sxml_byte_view literal) {
    return text.byte_count == literal.byte_count &&
           (literal.byte_count == 0 || memcmp(text.bytes, literal.bytes, literal.byte_count) == 0);
}

/* =========================================================== IR pieces */

/* Whether a step pushed a frame (or found a value to lower). */
typedef struct step_outcome {
    bool is_pushed;
} step_outcome;

/* Whether an item is empty under a rule (8.5). */
typedef struct emptiness_answer {
    bool is_empty;
} emptiness_answer;

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

/* Ends frame with atom as its value. */
static sxml_status finish_with_atom(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame, sxml_ir_atom atom) {
    return finish_with_expression(machine, frame, atom_expression(atom));
}

/* The empty sequence as a constant. */
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

/* Ends frame with the empty sequence (an instruction without a value). */
static sxml_status finish_with_empty(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_ir_atom empty = {0};
    const sxml_status status = add_empty_atom(machine->state, &empty);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, empty);
}

static sxml_status add_integer(sxml_xpath_lowerer *lowerer, int64_t integer, sxml_ir_atom *atom) {
    return sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_from_integer(integer),
                                                atom);
}

static sxml_status add_string(sxml_xpath_lowerer *lowerer, sxml_byte_view text,
                              sxml_ir_atom *atom) {
    return sxml_xpath_lowerer_add_string_constant(lowerer, text, atom);
}

/* An xs:QName constant of name (no prefix). */
/* The prefix an xs:QName of name is shown with: err for a name in the
   namespace of error codes (XTMM9000 is reported as err:XTMM9000, as the
   errors of the function library are), no prefix otherwise. */
static sxml_status qname_prefix(sxml_name_pool *name_pool, sxml_name_identifier name,
                                sxml_namespace_prefix_identifier *prefix) {
    prefix->index = 0;
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    const sxml_status status = sxml_name_pool_get_name_parts(name_pool, name, &parts);
    if (status != SXML_STATUS_OK ||
        parts.namespace_uri.index !=
            sxml_well_known_namespace_identifier(SXML_NAMESPACE_ERRORS).index) {
        return status;
    }
    return sxml_name_pool_intern_namespace_prefix(name_pool, SXML_BYTE_VIEW_FROM_LITERAL("err"),
                                                  prefix);
}

/* An xs:QName item of name, owned. */
static sxml_status make_qname_item(sxml_item_heap *item_heap, sxml_name_identifier name,
                                   sxml_item *item) {
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    const sxml_status status =
        qname_prefix(sxml_item_heap_get_name_pool(item_heap), name, &prefix);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = name,
                                                  .namespace_prefix = prefix}}}},
        item);
}

static sxml_status add_qname(sxml_xpath_lowerer *lowerer, sxml_name_identifier name,
                             sxml_ir_atom *atom) {
    sxml_item item = {.representation = 0};
    const sxml_status status = make_qname_item(lowerer->item_heap, name, &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_item_constant(lowerer, item, atom);
}

/* The atoms of a sequence as one SEQUENCE expression. */
static sxml_status make_sequence(sxml_xpath_lowerer *lowerer, sxml_ir_atom_view atoms,
                                 sxml_ir_expression *expression) {
    sxml_ir_atom_list list = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(lowerer->unit, atoms, &list);
    *expression =
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE, .sequence = list};
    return status;
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

/* A call of one of this file's internal native rows, bound in block. */
typedef struct native_request {
    sxml_xpath_lowering_block *block;
    const sxml_native_function *native_function;
    sxml_ir_atom argument;
} native_request;

static sxml_status bind_native(sxml_xpath_lowerer *lowerer, native_request request,
                               sxml_ir_atom *atom) {
    sxml_ir_atom_list arguments = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(
        lowerer->unit, (sxml_ir_atom_view){.atoms = &request.argument, .atom_count = 1},
        &arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(
        lowerer,
        (binding){
            .block = request.block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                           .static_call = {.target = {.target_kind =
                                                          SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                      .native_function = request.native_function},
                                           .arguments = arguments}}},
        atom);
}

/* value?key (a LOOKUP), bound in block. */
typedef struct lookup_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
    sxml_ir_atom key;
} lookup_request;

static sxml_status bind_lookup(sxml_xpath_lowerer *lowerer, lookup_request request,
                               sxml_ir_atom *atom) {
    return bind(lowerer,
                (binding){.block = request.block,
                          .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LOOKUP,
                                         .lookup = {.value = request.value, .key = request.key}}},
                atom);
}

/* value?position for an integer position. */
typedef struct member_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom array;
    int64_t position;
} member_request;

static sxml_status bind_member(sxml_xpath_lowerer *lowerer, member_request request,
                               sxml_ir_atom *atom) {
    sxml_ir_atom key = {0};
    const sxml_status status = add_integer(lowerer, request.position, &key);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_lookup(lowerer,
                       (lookup_request){.block = request.block, .value = request.array, .key = key},
                       atom);
}

/* "[a, b, ...]": one member per atom, bound in block. */
typedef struct array_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom_view members;
} array_request;

static sxml_status bind_array(sxml_xpath_lowerer *lowerer, array_request request,
                              sxml_ir_atom *atom) {
    sxml_ir_atom_list members = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(lowerer->unit, request.members, &members);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(
        lowerer,
        (binding){.block = request.block,
                  .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
                                 .array_constructor = {.array_form = SXML_IR_ARRAY_FORM_SQUARE,
                                                       .members = members}}},
        atom);
}

/* A RAISE of an XSLT error known statically, as an expression. */
typedef struct raise_request {
    sxml_status error_status;
    sxml_byte_view code;
    sxml_byte_view description;
} raise_request;

static sxml_status add_raise(sxml_xpath_lowerer *lowerer, raise_request request,
                             sxml_ir_expression_index *expression) {
    return sxml_xpath_lowerer_add_raise(
        lowerer,
        (sxml_xpath_lowering_raise_request){.node = no_ast_node,
                                            .error_status = request.error_status,
                                            .code_local_name = request.code,
                                            .description = request.description},
        expression);
}

/* An expression of one branch: lowered into a block of its own. */
typedef struct branch_result {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
} branch_result;

/* The block ended with an ATOM expression of value. */
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

/* "if (condition) then then_expression else else_expression" from two
   finished branches. */
typedef struct conditional_parts {
    sxml_ir_atom condition;
    sxml_ir_expression_index then_branch;
    sxml_ir_expression_index else_branch;
} conditional_parts;

static sxml_ir_expression conditional_expression(conditional_parts parts) {
    return (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                .conditional = {.condition = parts.condition,
                                                .then_branch = parts.then_branch,
                                                .else_branch = parts.else_branch}};
}

static sxml_status report_static(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                                 sxml_byte_view code_and_description) {
    return sxml_xslt_lowering_report_static_error(
        lowering,
        (sxml_xslt_static_error){.node = node, .code_and_description = code_and_description});
}

/* ======================================================== Types */

/* The XSLT code of a variable's coercion error (9.3): XTTE0570 where the
   evaluator raises XPTY0004. */
static const sxml_xslt_error_mapping variable_type_mapping[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
     .raised_status = SXML_STATUS_TYPE_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTTE0570"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "the value of the variable does not match its required type (XSLT 4.0, 9.3)")},
};

/* How a variable-binding element's value is coerced to its type. */
typedef enum binding_role {
    /* xsl:variable: XTTE0570. */
    SXML_XSLT_BINDING_ROLE_VARIABLE,
    /* xsl:param and xsl:with-param of xsl:iterate: XTTE0590. */
    SXML_XSLT_BINDING_ROLE_PARAMETER,
} binding_role;

/* value coerced to type in block (9.3), with role's error code. */
typedef struct coercion_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
    sxml_sequence_type sequence_type;
} coercion_request;

typedef struct coercion_call {
    coercion_request request;
    binding_role role;
} coercion_call;

static sxml_status bind_coercion(sxml_xpath_lowerer *lowerer, coercion_call call,
                                 sxml_ir_atom *atom) {
    const coercion_request request = call.request;
    if (call.role == SXML_XSLT_BINDING_ROLE_PARAMETER) {
        return sxml_xpath_lowerer_coerce_atom(lowerer,
                                              (sxml_xpath_lowering_coercion_request){
                                                  .block = request.block,
                                                  .node = no_ast_node,
                                                  .value = request.value,
                                                  .sequence_type = request.sequence_type,
                                                  .role = SXML_IR_COERCION_ROLE_TEMPLATE_PARAMETER},
                                              atom);
    }
    return sxml_xslt_lowering_bind_mapped_expression(
        lowerer,
        &(sxml_xslt_mapped_expression){
            .block = request.block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_COERCE,
                           .coercion = {.value = request.value,
                                        .sequence_type = request.sequence_type,
                                        .role = SXML_IR_COERCION_ROLE_VARIABLE}},
            .mappings = {.mappings = variable_type_mapping, .mapping_count = 1}},
        atom);
}

/* ================================================== Select or content */

/* The value of an instruction or variable-binding element: its select
   expression or its sequence constructor, lowered to *atom in block. */
typedef struct value_options value_options;

/* What start_value did. */
typedef struct value_source {
    bool has_select;
    bool has_content;
    /* Neither: *atom is not written and nothing was pushed. */
    bool has_value;
} value_source;

/* conflict_error ("XTSE3185 ...", static text) is reported for a select
   attribute with content; NULL allows both (xsl:message: the select
   value, then the content's). focus is the focus while the value is
   lowered; NULL keeps the current one. */
struct value_options {
    const char *conflict_error;
    sxml_xpath_lowering_focus *focus;
};

typedef struct value_request {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
    value_options options;
    /* True when atom is the content its block ends with (D166:
       sxml_xslt_sequence_constructor_call.is_content). */
    bool is_content;
} value_request;

static sxml_status start_value(sxml_xpath_lowering_machine *machine, value_request request,
                               value_source *source) {
    const value_options options = request.options;
    const sxml_xslt_lowering *lowering = request.lowering;
    const sxml_xslt_ast_attribute *select = find_attribute(
        lowering, (attribute_lookup){.node = request.node, .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT});
    source->has_select = select->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
    source->has_content = has_content(lowering, request.node);
    source->has_value = source->has_select || source->has_content;
    if (source->has_select) {
        if (options.conflict_error != NULL && source->has_content) {
            return report_static(lowering, request.node,
                                 (sxml_byte_view){.bytes = (const uint8_t *)options.conflict_error,
                                                  .byte_count = strlen(options.conflict_error)});
        }
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = request.block,
                                                  .atom = request.atom,
                                                  .focus = options.focus});
    }
    if (!source->has_content) {
        return SXML_STATUS_OK;
    }
    return sxml_xslt_lowering_call_sequence_constructor(
        machine,
        &(sxml_xslt_sequence_constructor_call){
            .lowering = lowering,
            .first = sxml_xslt_lowering_first_constructor_child(lowering->ast, request.node),
            .block = request.block,
            .is_body = false,
            .is_content = request.is_content,
            .destination = {.expression = NULL,
                            .atom = request.atom,
                            .atom_block = request.block,
                            .focused = NULL},
            .focus = options.focus});
}

/* The sequence constructor of node alone (its select ignored). */
static sxml_status start_content(sxml_xpath_lowering_machine *machine, value_request request) {
    return sxml_xslt_lowering_call_sequence_constructor(
        machine,
        &(sxml_xslt_sequence_constructor_call){.lowering = request.lowering,
                                               .first = sxml_xslt_lowering_first_constructor_child(
                                                   request.lowering->ast, request.node),
                                               .block = request.block,
                                               .is_body = false,
                                               .destination = {.expression = NULL,
                                                               .atom = request.atom,
                                                               .atom_block = request.block,
                                                               .focused = NULL},
                                               .focus = NULL});
}

/* ================================================= Variable bindings */

/*
 * The value of a variable-binding element (9.3): xsl:variable, and
 * xsl:param and xsl:with-param of xsl:iterate. The select expression's
 * value; else, for content without "as" and without a disqualifying
 * child (9.4: xsl:map, xsl:map-entry, xsl:array, xsl:array-member,
 * xsl:select), a document node of the content (5.7.1, XSLT rules); else
 * the content's value; with neither, "" without "as" and () with it.
 * Then coerced to "as", and for a parameter also to required_type.
 */
typedef struct binding_value_locals {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    binding_role role;
    /* The iterate parameter's type a with-param value must also match. */
    bool has_required_type;
    sxml_sequence_type required_type;
    value_source source;
    sxml_ir_atom value;
    /* For an implicit document node (9.4): the block its content is
       computed in, inside the document constructor (D166). */
    bool makes_document;
    sxml_xpath_lowering_block content_block;
} binding_value_locals;

static bool has_disqualifying_child(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index node) {
    for (sxml_xslt_ast_node_index child = node_of(lowering, node)->first_child; child.value != 0;
         child = next_of(lowering, child)) {
        switch ((uint32_t)kind_of(lowering, child)) {
        case SXML_XSLT_AST_KIND_MAP:
        case SXML_XSLT_AST_KIND_MAP_ENTRY:
        case SXML_XSLT_AST_KIND_ARRAY:
        case SXML_XSLT_AST_KIND_ARRAY_MEMBER:
        case SXML_XSLT_AST_KIND_SELECT:
            return true;
        default:
            break;
        }
    }
    return false;
}

static sxml_status finish_binding_value(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    binding_value_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->lowering;
    sxml_xslt_declared_type type = {.has_type = false};
    sxml_status status = sxml_xslt_lowering_resolve_declared_type(lowering, locals->node, &type);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_atom value = locals->value;
    if (!locals->source.has_value) {
        status =
            type.has_type
                ? add_empty_atom(lowerer, &value)
                : add_string(lowerer, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &value);
    } else if (locals->makes_document) {
        /* 9.4: an implicit document node. */
        sxml_ir_expression_index result = {0};
        status = sxml_xslt_lowering_add_expression(lowerer, atom_expression(value), &result);
        sxml_ir_expression_index content = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_finish_block(
                lowerer,
                (sxml_xpath_lowering_block_result){.block = locals->content_block,
                                                   .result = result},
                &content);
        }
        if (status == SXML_STATUS_OK) {
            status = bind(
                lowerer,
                (binding){
                    .block = frame->block,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                                   .node_construction = {.content = content,
                                                         /* 25.2: as preserve (D179). */
                                                         .construction_flags =
                                                             (uint32_t)
                                                                 SXML_IR_CONSTRUCTION_XSLT_RULES |
                                                             (uint32_t)
                                                                 SXML_IR_CONSTRUCTION_PRESERVES_TYPES,
                                                         .module_number =
                                                             sxml_xslt_lowering_module_of_node(
                                                                 lowering, locals->node)}}},
                &value);
        }
    }
    if (status == SXML_STATUS_OK && type.has_type) {
        status = bind_coercion(lowerer,
                               (coercion_call){.request = {.block = frame->block,
                                                           .value = value,
                                                           .sequence_type = type.sequence_type},
                                               .role = locals->role},
                               &value);
    }
    if (status == SXML_STATUS_OK && locals->has_required_type) {
        status = bind_coercion(lowerer,
                               (coercion_call){.request = {.block = frame->block,
                                                           .value = value,
                                                           .sequence_type = locals->required_type},
                                               .role = SXML_XSLT_BINDING_ROLE_PARAMETER},
                               &value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, value);
}

static sxml_status continue_binding_value(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    binding_value_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        sxml_xslt_declared_type type = {.has_type = false};
        sxml_status status =
            sxml_xslt_lowering_resolve_declared_type(locals->lowering, locals->node, &type);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const bool has_select =
            find_attribute(locals->lowering,
                           (attribute_lookup){.node = locals->node,
                                              .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT})
                ->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
        locals->makes_document = !has_select && !type.has_type &&
                                 has_content(locals->lowering, locals->node) &&
                                 !has_disqualifying_child(locals->lowering, locals->node);
        status = start_value(
            machine,
            (value_request){.lowering = locals->lowering,
                            .node = locals->node,
                            .block = locals->makes_document ? &locals->content_block
                                                            : frame->block,
                            .is_content = locals->makes_document,
                            .atom = &locals->value,
                            .options = {.conflict_error = "XTSE0620 a variable-binding element "
                                                          "has both a select attribute and "
                                                          "content (XSLT 4.0, 9.3)",
                                        .focus = NULL}},
            &locals->source);
        if (status != SXML_STATUS_OK || locals->source.has_value) {
            return status;
        }
    }
    return finish_binding_value(machine, frame);
}

static const sxml_xpath_lowering_procedure binding_value_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt variable-binding value"),
    .step = continue_binding_value,
    .locals_size = sizeof(binding_value_locals)};

/* Pushes the frame lowering node's value to *atom in block. */
typedef struct binding_value_call {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    binding_role role;
    /* NULL for none. */
    const sxml_sequence_type *required_type;
} binding_value_call;

typedef struct binding_destination {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
} binding_destination;

static sxml_status call_binding_value(sxml_xpath_lowering_machine *machine, binding_value_call call,
                                      binding_destination destination) {
    binding_value_locals initial = {.lowering = call.lowering,
                                    .node = call.node,
                                    .role = call.role,
                                    .has_required_type = call.required_type != NULL};
    if (call.required_type != NULL) {
        initial.required_type = *call.required_type;
    }
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &binding_value_procedure,
            .node = no_ast_node,
            .block = destination.block,
            .is_body = false,
            .destination = {.expression = NULL,
                            .atom = destination.atom,
                            .atom_block = destination.block,
                            .focused = NULL},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof(initial)}},
        &frame);
}

/* xsl:variable in a sequence constructor (9.1, 9.8): its value bound in
   the sequence constructor's block and the name in scope for the
   following siblings and their descendants (9.9) until the sequence
   constructor ends. The instruction's own value is (). */
typedef struct variable_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
} variable_locals;

static sxml_status continue_variable(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    variable_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return call_binding_value(
            machine,
            (binding_value_call){.lowering = locals->head.lowering,
                                 .node = locals->head.node,
                                 .role = SXML_XSLT_BINDING_ROLE_VARIABLE,
                                 .required_type = NULL},
            (binding_destination){.block = frame->block, .atom = &locals->value});
    }
    const sxml_status status = sxml_xpath_lowerer_push_variable(
        lowerer, (sxml_xpath_lowering_scope_entry){
                     .variable_name = binding_name_of(locals->head.lowering, locals->head.node),
                     .function_level = lowerer->function->function_level,
                     .atom = locals->value});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_empty(machine, frame);
}

static const sxml_xpath_lowering_procedure variable_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:variable"),
    .step = continue_variable,
    .locals_size = sizeof(variable_locals)};

/* =============================================================== xsl:if */

/* xsl:if (8.1): CONDITIONAL on the test, then the then attribute or the
   content, else the else attribute or (). */
typedef struct if_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom test;
    sxml_xpath_lowering_block then_block;
    sxml_ir_atom then_value;
    bool has_then;
    sxml_xpath_lowering_block else_block;
    sxml_ir_atom else_value;
    bool has_else;
} if_locals;

/* Lowers expression attribute kind of the instruction to *atom in block;
 outcome->is_pushed false when it is absent. */
typedef struct attribute_value_request {
    const sxml_xslt_instruction_locals *head;
    sxml_xslt_ast_attribute_kind kind;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
} attribute_value_request;

static sxml_status start_attribute_value(sxml_xpath_lowering_machine *machine,
                                         attribute_value_request request, step_outcome *outcome) {
    const sxml_xslt_ast_attribute *attribute =
        find_attribute(request.head->lowering,
                       (attribute_lookup){.node = request.head->node, .kind = request.kind});
    outcome->is_pushed = attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION;
    if (!outcome->is_pushed) {
        return SXML_STATUS_OK;
    }
    return sxml_xslt_lowering_call_expression(
        machine, &(sxml_xslt_expression_call){.lowering = request.head->lowering,
                                              .expression = attribute->value.payload.expression,
                                              .block = request.block,
                                              .atom = request.atom,
                                              .focus = NULL});
}

/* The value of a branch, () when it has none. */
typedef struct branch_value {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
    bool has_value;
} branch_value;

static sxml_status finish_branch_value(sxml_xpath_lowerer *lowerer, branch_value branch,
                                       sxml_ir_expression_index *expression) {
    sxml_ir_atom value = branch.value;
    if (!branch.has_value) {
        const sxml_status status = add_empty_atom(lowerer, &value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return finish_branch(lowerer, (branch_result){.block = branch.block, .value = value},
                         expression);
}

static sxml_status continue_if(sxml_xpath_lowering_machine *machine,
                               sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    if_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    step_outcome outcome = {.is_pushed = false};
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_xslt_ast_attribute *test =
            find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                        .kind = SXML_XSLT_AST_ATTRIBUTE_TEST});
        if (has_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                       .kind = SXML_XSLT_AST_ATTRIBUTE_THEN}) &&
            has_content(lowering, locals->head.node)) {
            return report_static(lowering, locals->head.node,
                                 SXML_BYTE_VIEW_FROM_LITERAL(
                                     "XTSE3185 xsl:if has both a then attribute and content (XSLT "
                                     "4.0, 8.1)"));
        }
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = test->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->test,
                                                  .focus = NULL});
    }
    if (frame->step == 1) {
        frame->step = 2;
        const sxml_status status =
            start_attribute_value(machine,
                                  (attribute_value_request){.head = &locals->head,
                                                            .kind = SXML_XSLT_AST_ATTRIBUTE_THEN,
                                                            .block = &locals->then_block,
                                                            .atom = &locals->then_value},
                                  &outcome);
        locals->has_then = outcome.is_pushed;
        if (status != SXML_STATUS_OK || outcome.is_pushed) {
            return status;
        }
        locals->has_then = has_content(lowering, locals->head.node);
        if (locals->has_then) {
            return start_content(machine, (value_request){.lowering = lowering,
                                                          .node = locals->head.node,
                                                          .block = &locals->then_block,
                                                          .atom = &locals->then_value});
        }
    }
    if (frame->step == 2) {
        frame->step = 3;
        const sxml_status status =
            start_attribute_value(machine,
                                  (attribute_value_request){.head = &locals->head,
                                                            .kind = SXML_XSLT_AST_ATTRIBUTE_ELSE,
                                                            .block = &locals->else_block,
                                                            .atom = &locals->else_value},
                                  &outcome);
        locals->has_else = outcome.is_pushed;
        if (status != SXML_STATUS_OK || outcome.is_pushed) {
            return status;
        }
    }
    conditional_parts parts = {.condition = locals->test};
    sxml_status status = finish_branch_value(lowerer,
                                             (branch_value){.block = &locals->then_block,
                                                            .value = locals->then_value,
                                                            .has_value = locals->has_then},
                                             &parts.then_branch);
    if (status == SXML_STATUS_OK) {
        status = finish_branch_value(lowerer,
                                     (branch_value){.block = &locals->else_block,
                                                    .value = locals->else_value,
                                                    .has_value = locals->has_else},
                                     &parts.else_branch);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(machine, frame, conditional_expression(parts));
}

static const sxml_xpath_lowering_procedure if_procedure = {.procedure_name =
                                                               SXML_BYTE_VIEW_INITIALIZER("xsl:if"),
                                                           .step = continue_if,
                                                           .locals_size = sizeof(if_locals)};

/* ===================================================== choose and switch */

/*
 * xsl:choose (8.2) and xsl:switch (8.3): a CONDITIONAL per xsl:when, the
 * test of each later one lowered in the else branch of the one before
 * (tests after the selected branch are not evaluated), the last else
 * branch xsl:otherwise or (). For xsl:switch the selector is the select
 * value coerced to xs:anyAtomicType (one item), and a when is satisfied
 * when its test value, coerced to xs:anyAtomicType*, "=" the selector.
 */
typedef struct when_branch {
    /* Where the test is lowered: the else branch of the previous when. */
    sxml_xpath_lowering_block test_block;
    sxml_ir_atom test;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom body;
    bool has_body;
} when_branch;

typedef struct choose_locals {
    sxml_xslt_instruction_locals head;
    bool is_switch;
    sxml_ir_atom selector;
    /* when_count + 1 branches (owned memory 0); the last holds
       xsl:otherwise in its test block and body. */
    when_branch *branches;
    size_t when_count;
    size_t branch_index;
    sxml_xslt_ast_node_index next_child;
    /* The xsl:when whose test was lowered last. */
    sxml_xslt_ast_node_index current_when;
    sxml_xslt_ast_node_index otherwise;
} choose_locals;

enum {
    SXML_XSLT_CHOOSE_STEP_START = 0,
    SXML_XSLT_CHOOSE_STEP_SELECTOR = 1,
    SXML_XSLT_CHOOSE_STEP_TEST = 2,
    SXML_XSLT_CHOOSE_STEP_BODY = 3,
    SXML_XSLT_CHOOSE_STEP_OTHERWISE = 4,
    SXML_XSLT_CHOOSE_STEP_FINISH = 5,
};

/* The block the test of branch index goes to. */
static sxml_xpath_lowering_block *test_block_of(sxml_xpath_lowering_frame *frame,
                                                size_t branch_index) {
    choose_locals *locals = frame->locals;
    return branch_index == 0 ? frame->block : &locals->branches[branch_index].test_block;
}

static sxml_status start_choose(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    choose_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    locals->is_switch = kind_of(lowering, locals->head.node) == SXML_XSLT_AST_KIND_SWITCH;
    locals->when_count = count_children(
        lowering, (child_count_query){.node = locals->head.node, .kind = SXML_XSLT_AST_KIND_WHEN});
    for (sxml_xslt_ast_node_index child = node_of(lowering, locals->head.node)->first_child;
         child.value != 0; child = next_of(lowering, child)) {
        if (kind_of(lowering, child) == SXML_XSLT_AST_KIND_OTHERWISE) {
            locals->otherwise = child;
        }
    }
    const sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 0,
            .scratch = {.element_count = locals->when_count + 1,
                        .element_size = sizeof(when_branch),
                        .element_alignment = alignof(when_branch)}});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->branches = frame->owned_memory[0];
    memset(locals->branches, 0, (locals->when_count + 1) * sizeof(when_branch));
    locals->next_child = node_of(lowering, locals->head.node)->first_child;
    if (!locals->is_switch) {
        frame->step = SXML_XSLT_CHOOSE_STEP_TEST;
        return SXML_STATUS_OK;
    }
    frame->step = SXML_XSLT_CHOOSE_STEP_SELECTOR;
    const sxml_xslt_ast_attribute *select =
        find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                    .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT});
    return sxml_xslt_lowering_call_expression(
        machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                              .expression = select->value.payload.expression,
                                              .block = frame->block,
                                              .atom = &locals->selector,
                                              .focus = NULL});
}

/* The next xsl:when child; 0 after the last. */
static sxml_xslt_ast_node_index take_next_when(choose_locals *locals) {
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    while (locals->next_child.value != 0) {
        const sxml_xslt_ast_node_index child = locals->next_child;
        locals->next_child = next_of(lowering, child);
        if (kind_of(lowering, child) == SXML_XSLT_AST_KIND_WHEN) {
            return child;
        }
    }
    return (sxml_xslt_ast_node_index){.value = 0};
}

/* For xsl:switch, the test value compared with the selector (8.3). */
static sxml_status compare_with_selector(sxml_xpath_lowerer *lowerer, choose_locals *locals,
                                         sxml_xpath_lowering_block *block) {
    when_branch *branch = &locals->branches[locals->branch_index];
    sxml_ir_atom atomized = {0};
    const sxml_status status = sxml_xpath_lowerer_coerce_atom(
        lowerer,
        (sxml_xpath_lowering_coercion_request){
            .block = block,
            .node = no_ast_node,
            .value = branch->test,
            .sequence_type = {.item_type =
                                  sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
                              .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
            .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
        &atomized);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(
        lowerer,
        (binding){.block = block,
                  .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                                 .binary = {.operator= SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL,
                                            .left_operand = atomized,
                                            .right_operand = locals->selector}}},
        &branch->test);
}

static sxml_status finish_choose(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    choose_locals *locals = frame->locals;
    const size_t when_count = locals->when_count;
    when_branch *last = &locals->branches[when_count];
    sxml_ir_expression_index rest = {0};
    sxml_status status = finish_branch_value(lowerer,
                                             (branch_value){.block = &last->test_block,
                                                            .value = last->body,
                                                            .has_value = last->has_body},
                                             &rest);
    for (size_t branch_number = when_count; status == SXML_STATUS_OK && branch_number > 0;
         branch_number -= 1) {
        when_branch *branch = &locals->branches[branch_number - 1];
        conditional_parts parts = {.condition = branch->test, .else_branch = rest};
        status = finish_branch_value(lowerer,
                                     (branch_value){.block = &branch->body_block,
                                                    .value = branch->body,
                                                    .has_value = branch->has_body},
                                     &parts.then_branch);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (branch_number == 1) {
            return finish_with_expression(machine, frame, conditional_expression(parts));
        }
        sxml_ir_expression_index conditional = {0};
        status = sxml_xslt_lowering_add_expression(lowerer, conditional_expression(parts), &conditional);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_finish_block(
                lowerer,
                (sxml_xpath_lowering_block_result){.block = branch->test_block,
                                                   .result = conditional},
                &rest);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* No xsl:when (the reader requires one): the otherwise value. */
    return sxml_xpath_lowering_finish_frame(machine, frame, rest);
}

static sxml_status continue_choose(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    choose_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == SXML_XSLT_CHOOSE_STEP_START) {
        status = start_choose(machine, frame);
        if (status != SXML_STATUS_OK || frame->step == SXML_XSLT_CHOOSE_STEP_SELECTOR) {
            return status;
        }
    }
    if (frame->step == SXML_XSLT_CHOOSE_STEP_SELECTOR) {
        /* 8.3: the selector is one atomic item (coercion rules). */
        status = sxml_xpath_lowerer_coerce_atom(
            lowerer,
            (sxml_xpath_lowering_coercion_request){
                .block = frame->block,
                .node = no_ast_node,
                .value = locals->selector,
                .sequence_type = {.item_type =
                                      sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
                                  .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
            &locals->selector);
        frame->step = SXML_XSLT_CHOOSE_STEP_TEST;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (;;) {
        when_branch *branch = &locals->branches[locals->branch_index];
        switch (frame->step) {
        case SXML_XSLT_CHOOSE_STEP_TEST: {
            const sxml_xslt_ast_node_index when = take_next_when(locals);
            if (when.value == 0) {
                frame->step = SXML_XSLT_CHOOSE_STEP_OTHERWISE;
                break;
            }
            frame->step = SXML_XSLT_CHOOSE_STEP_BODY;
            const sxml_xslt_ast_attribute *test = find_attribute(
                lowering, (attribute_lookup){.node = when, .kind = SXML_XSLT_AST_ATTRIBUTE_TEST});
            locals->current_when = when;
            return sxml_xslt_lowering_call_expression(
                machine,
                &(sxml_xslt_expression_call){.lowering = lowering,
                                             .expression = test->value.payload.expression,
                                             .block = test_block_of(frame, locals->branch_index),
                                             .atom = &branch->test,
                                             .focus = NULL});
        }
        case SXML_XSLT_CHOOSE_STEP_BODY: {
            if (locals->is_switch) {
                status = compare_with_selector(lowerer, locals,
                                               test_block_of(frame, locals->branch_index));
                if (status != SXML_STATUS_OK) {
                    return status;
                }
            }
            locals->branch_index += 1;
            frame->step = SXML_XSLT_CHOOSE_STEP_TEST;
            value_source source = {.has_value = false};
            status =
                start_value(machine,
                            (value_request){.lowering = lowering,
                                            .node = locals->current_when,
                                            .block = &branch->body_block,
                                            .atom = &branch->body,
                                            .options = {.conflict_error =
                                                            "XTSE3185 xsl:when has both a select "
                                                            "attribute and content (XSLT 4.0, 8.2)",
                                                        .focus = NULL}},
                            &source);
            branch->has_body = source.has_value;
            if (status != SXML_STATUS_OK || source.has_value) {
                return status;
            }
            break;
        }
        case SXML_XSLT_CHOOSE_STEP_OTHERWISE: {
            frame->step = SXML_XSLT_CHOOSE_STEP_FINISH;
            if (locals->otherwise.value == 0) {
                break;
            }
            when_branch *last = &locals->branches[locals->when_count];
            value_source source = {.has_value = false};
            status = start_value(
                machine,
                (value_request){.lowering = lowering,
                                .node = locals->otherwise,
                                .block = &last->test_block,
                                .atom = &last->body,
                                .options = {.conflict_error =
                                                "XTSE3185 xsl:otherwise has both a select "
                                                "attribute and content (XSLT 4.0, 8.2)",
                                            .focus = NULL}},
                &source);
            last->has_body = source.has_value;
            if (status != SXML_STATUS_OK || source.has_value) {
                return status;
            }
            break;
        }
        default:
            return finish_choose(machine, frame);
        }
    }
}

static const sxml_xpath_lowering_procedure choose_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:choose"),
    .step = continue_choose,
    .locals_size = sizeof(choose_locals)};

/* ========================================================== xsl:for-each */

/* xsl:for-each (7.1): a MAP over the select value whose body is the
   sequence constructor with the item, its position and the size as the
   focus; with a separator (7.1.1), a text node of its value after the
   result of each item but the last. With xsl:sort children the input is
   sorted first (13.3, src/xslt_lower_group.c). */
typedef struct for_each_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom input;
    bool is_sorted;
    sxml_ir_atom separator;
    bool has_separator;
    sxml_xpath_lowering_focus item_focus;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom body;
} for_each_locals;

/* In the body: (body, if (position() lt last()) then text{separator}
   else ()). */
static sxml_status add_separator(sxml_xpath_lowerer *lowerer, for_each_locals *locals) {
    sxml_xpath_lowering_focus *outer_focus = lowerer->function->focus;
    lowerer->function->focus = &locals->item_focus;
    sxml_ir_atom position = {0};
    sxml_ir_atom size = {0};
    sxml_status status = sxml_xpath_lowerer_read_focus(
        lowerer,
        (sxml_xpath_lowering_focus_read){.block = &locals->body_block,
                                         .node = no_ast_node,
                                         .part = SXML_XPATH_LOWERING_FOCUS_PART_POSITION},
        &position);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_read_focus(
            lowerer,
            (sxml_xpath_lowering_focus_read){.block = &locals->body_block,
                                             .node = no_ast_node,
                                             .part = SXML_XPATH_LOWERING_FOCUS_PART_SIZE},
            &size);
    }
    lowerer->function->focus = outer_focus;
    sxml_ir_atom is_not_last = {0};
    if (status == SXML_STATUS_OK) {
        status =
            bind(lowerer,
                 (binding){.block = &locals->body_block,
                           .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                                          .binary = {.operator= SXML_IR_BINARY_OPERATOR_VALUE_LESS,
                                                     .left_operand = position,
                                                     .right_operand = size}}},
                 &is_not_last);
    }
    conditional_parts parts = {.condition = is_not_last};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                 .node_construction = {.value = locals->separator}},
            &parts.then_branch);
    }
    sxml_ir_expression empty = {0};
    if (status == SXML_STATUS_OK) {
        status =
            make_sequence(lowerer, (sxml_ir_atom_view){.atoms = NULL, .atom_count = 0}, &empty);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, empty, &parts.else_branch);
    }
    sxml_ir_atom separator_node = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(
            lowerer,
            (binding){.block = &locals->body_block, .expression = conditional_expression(parts)},
            &separator_node);
    }
    const sxml_ir_atom parts_of_body[2] = {locals->body, separator_node};
    sxml_ir_expression sequence = {0};
    if (status == SXML_STATUS_OK) {
        status = make_sequence(
            lowerer, (sxml_ir_atom_view){.atoms = parts_of_body, .atom_count = 2}, &sequence);
    }
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer, (binding){.block = &locals->body_block, .expression = sequence},
                      &locals->body);
    }
    return status;
}

static sxml_status finish_for_each(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    for_each_locals *locals = frame->locals;
    sxml_status status = SXML_STATUS_OK;
    if (locals->has_separator) {
        status = add_separator(lowerer, locals);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(
            lowerer, (branch_result){.block = &locals->body_block, .value = locals->body}, &body);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(
        machine, frame,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                             .map = {.input = locals->input,
                                     .focus = locals->item_focus.slots,
                                     .body = body,
                                     .flags = 0}});
}

static sxml_status continue_for_each(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    for_each_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_xslt_ast_attribute *select =
            find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                        .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT});
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->input,
                                                  .focus = NULL});
    }
    if (frame->step == 1 && !locals->is_sorted &&
        sxml_xslt_lowering_has_sort(lowering, locals->head.node)) {
        locals->is_sorted = true;
        return sxml_xslt_lowering_call_sort(
            machine, &(sxml_xslt_sort_call){.lowering = lowering,
                                            .node = locals->head.node,
                                            .input = locals->input,
                                            .sorts_groups = false,
                                            .block = frame->block,
                                            .atom = &locals->input});
    }
    if (frame->step == 1) {
        frame->step = 2;
        const sxml_xslt_ast_attribute *separator =
            find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                        .kind = SXML_XSLT_AST_ATTRIBUTE_SEPARATOR});
        locals->has_separator = separator->value.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE;
        if (locals->has_separator) {
            return sxml_xslt_lowering_call_value_template(
                machine,
                &(sxml_xslt_expression_call){.lowering = lowering,
                                             .expression = separator->value.payload.expression,
                                             .block = frame->block,
                                             .atom = &locals->separator,
                                             .focus = NULL});
        }
    }
    if (frame->step == 2) {
        frame->step = 3;
        locals->item_focus =
            (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS};
        locals->body_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
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
                         .focus = &locals->item_focus});
    }
    return finish_for_each(machine, frame);
}

static const sxml_xpath_lowering_procedure for_each_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:for-each"),
    .step = continue_for_each,
    .locals_size = sizeof(for_each_locals)};

/* ============================================================= xsl:try */

/*
 * xsl:try (8.4): TRY_CATCH (D109, D124) of the select value or the
 * sequence constructor (the children before the first xsl:catch; the
 * catches give nothing there). One clause per xsl:catch, its errors
 * attribute (default "*") as name tests; in its handler the $err:
 * variables are in scope: code, description, value, module, line-number
 * and column-number are LOOKUPs of the error details map, stack-trace
 * and additional are (), map is the map itself (D109).
 */
typedef struct catch_variable {
    const char *local_name;
    /* A LOOKUP of the details map; else () (or the map for "map"). */
    bool is_entry;
} catch_variable;

static const catch_variable catch_variables[] = {
    {.local_name = "code", .is_entry = true},
    {.local_name = "description", .is_entry = true},
    {.local_name = "value", .is_entry = true},
    {.local_name = "module", .is_entry = true},
    {.local_name = "line-number", .is_entry = true},
    {.local_name = "column-number", .is_entry = true},
    {.local_name = "stack-trace", .is_entry = false},
    {.local_name = "additional", .is_entry = false},
    {.local_name = "map", .is_entry = false},
};

#define CATCH_VARIABLE_COUNT (sizeof(catch_variables) / sizeof(catch_variables[0]))

typedef struct catch_branch {
    sxml_xslt_ast_node_index node;
    sxml_ir_catch_clause clause;
    sxml_xpath_lowering_block block;
    sxml_ir_atom value;
    bool has_value;
} catch_branch;

typedef struct try_locals {
    sxml_xslt_instruction_locals head;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom body;
    bool has_body;
    /* One per xsl:catch (owned memory 0). */
    catch_branch *catches;
    size_t catch_count;
    size_t catch_index;
    size_t scope_mark;
} try_locals;

/* The name tests of an errors attribute (owned memory 1 while they are
   copied into the unit). */
static sxml_status add_error_tests(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame, catch_branch *branch) {
    sxml_xpath_lowerer *lowerer = machine->state;
    try_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_attribute *errors = find_attribute(
        lowering, (attribute_lookup){.node = branch->node, .kind = SXML_XSLT_AST_ATTRIBUTE_ERRORS});
    const sxml_xslt_ast_name_token_view tokens =
        errors->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME_LIST
            ? sxml_xslt_ast_get_name_tokens(lowering->ast, errors->value.payload.name_tokens)
            : (sxml_xslt_ast_name_token_view){.name_tokens = NULL, .name_token_count = 0};
    const sxml_name_test any_name = {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME,
                                     .expanded_name = {.index = 0},
                                     .namespace_uri = {.index = 0},
                                     .local_name = {.bytes = NULL, .byte_count = 0}};
    if (tokens.name_token_count == 0) {
        return sxml_ir_unit_add_name_tests(
            lowerer->unit, (sxml_ir_name_test_view){.name_tests = &any_name, .name_test_count = 1},
            &branch->clause.error_tests);
    }
    if (frame->owned_memory[1] != NULL) {
        sxml_xpath_lowerer_release_scratch(lowerer, frame->owned_memory[1]);
        frame->owned_memory[1] = NULL;
    }
    sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 1,
            .scratch = {.element_count = tokens.name_token_count,
                        .element_size = sizeof(sxml_name_test),
                        .element_alignment = alignof(sxml_name_test)}});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_test *tests = frame->owned_memory[1];
    for (size_t token_index = 0; status == SXML_STATUS_OK && token_index < tokens.name_token_count;
         token_index += 1) {
        const sxml_xslt_ast_name_token *token = &tokens.name_tokens[token_index];
        tests[token_index] = any_name;
        switch ((uint32_t)token->kind) {
        case SXML_XSLT_AST_NAME_TOKEN_KIND_NAME:
            tests[token_index].name_test_kind = SXML_NAME_TEST_KIND_EXPANDED_NAME;
            tests[token_index].expanded_name = token->name;
            break;
        case SXML_XSLT_AST_NAME_TOKEN_KIND_NAMESPACE_WILDCARD:
            tests[token_index].name_test_kind = SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD;
            tests[token_index].namespace_uri = token->namespace_uri;
            break;
        case SXML_XSLT_AST_NAME_TOKEN_KIND_LOCAL_NAME_WILDCARD: {
            sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                     .local_name = {.bytes = NULL, .byte_count = 0}};
            status = sxml_name_pool_get_name_parts(lowerer->name_pool, token->name, &parts);
            tests[token_index].name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD;
            tests[token_index].local_name = parts.local_name;
            break;
        }
        default:
            break;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_ir_unit_add_name_tests(
        lowerer->unit,
        (sxml_ir_name_test_view){.name_tests = tests, .name_test_count = tokens.name_token_count},
        &branch->clause.error_tests);
}

/* The $err: variables of a catch clause, bound in its block and pushed. */
static sxml_status push_catch_variables(sxml_xpath_lowerer *lowerer, catch_branch *branch) {
    const sxml_ir_atom details = sxml_ir_slot_atom(branch->clause.error_details);
    sxml_status status = SXML_STATUS_OK;
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && variable_index < CATCH_VARIABLE_COUNT; variable_index += 1) {
        const catch_variable *variable = &catch_variables[variable_index];
        const sxml_byte_view local_name = {.bytes = (const uint8_t *)variable->local_name,
                                           .byte_count = strlen(variable->local_name)};
        sxml_name_identifier name = {.index = 0};
        status = sxml_name_pool_intern_name(
            lowerer->name_pool,
            &(sxml_name_request){.namespace_uri =
                                     sxml_well_known_namespace_identifier(SXML_NAMESPACE_ERRORS),
                                 .local_name = local_name},
            &name);
        sxml_ir_atom value = details;
        if (status == SXML_STATUS_OK && variable->is_entry) {
            sxml_ir_atom key = {0};
            status = add_string(lowerer, local_name, &key);
            if (status == SXML_STATUS_OK) {
                status = bind_lookup(
                    lowerer,
                    (lookup_request){.block = &branch->block, .value = details, .key = key},
                    &value);
            }
        } else if (status == SXML_STATUS_OK &&
                   !text_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("map"))) {
            status = add_empty_atom(lowerer, &value);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_push_variable(
                lowerer, (sxml_xpath_lowering_scope_entry){.variable_name = name,
                                                           .function_level =
                                                               lowerer->function->function_level,
                                                           .atom = value});
        }
    }
    return status;
}

static sxml_status start_try(sxml_xpath_lowering_machine *machine,
                             sxml_xpath_lowering_frame *frame) {
    try_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    locals->catch_count = count_children(
        lowering, (child_count_query){.node = locals->head.node, .kind = SXML_XSLT_AST_KIND_CATCH});
    if (locals->catch_count > 0) {
        const sxml_status status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = locals->catch_count,
                            .element_size = sizeof(catch_branch),
                            .element_alignment = alignof(catch_branch)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->catches = frame->owned_memory[0];
        memset(locals->catches, 0, locals->catch_count * sizeof(catch_branch));
        size_t catch_index = 0;
        for (sxml_xslt_ast_node_index child = node_of(lowering, locals->head.node)->first_child;
             child.value != 0; child = next_of(lowering, child)) {
            if (kind_of(lowering, child) == SXML_XSLT_AST_KIND_CATCH) {
                locals->catches[catch_index].node = child;
                catch_index += 1;
            }
        }
    }
    value_source source = {.has_value = false};
    const sxml_status status = start_value(
        machine,
        (value_request){.lowering = lowering,
                        .node = locals->head.node,
                        .block = &locals->body_block,
                        .atom = &locals->body,
                        .options = {.conflict_error =
                                        "XTSE3140 xsl:try has both a select attribute and "
                                        "content other than xsl:catch (XSLT 4.0, 8.4)",
                                    .focus = NULL}},
        &source);
    locals->has_body = source.has_value;
    return status;
}

static sxml_status finish_try(sxml_xpath_lowering_machine *machine,
                              sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    try_locals *locals = frame->locals;
    sxml_ir_expression_index body = {0};
    sxml_status status = finish_branch_value(lowerer,
                                             (branch_value){.block = &locals->body_block,
                                                            .value = locals->body,
                                                            .has_value = locals->has_body},
                                             &body);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (locals->catch_count == 0) {
        /* The reader requires an xsl:catch (XTSE0010); without one the
           value is the body's. */
        return sxml_xpath_lowering_finish_frame(machine, frame, body);
    }
    /* The clauses, contiguous (owned memory 2). */
    status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 2,
            .scratch = {.element_count = locals->catch_count,
                        .element_size = sizeof(sxml_ir_catch_clause),
                        .element_alignment = alignof(sxml_ir_catch_clause)}});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_catch_clause *clauses = frame->owned_memory[2];
    for (size_t catch_index = 0; catch_index < locals->catch_count; catch_index += 1) {
        clauses[catch_index] = locals->catches[catch_index].clause;
    }
    sxml_ir_catch_clause_list clause_list = {0};
    status = sxml_ir_unit_add_catch_clauses(
        lowerer->unit,
        (sxml_ir_catch_clause_view){.catch_clauses = clauses,
                                    .catch_clause_count = locals->catch_count},
        &clause_list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(
        machine, frame,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH,
                             .try_catch = {.body = body,
                                           .catch_clauses = clause_list,
                                           .finally_body = {.value = 0}}});
}

static sxml_status continue_try(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    try_locals *locals = frame->locals;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        frame->step = 1;
        status = start_try(machine, frame);
        if (status != SXML_STATUS_OK || locals->has_body) {
            return status;
        }
    }
    if (frame->step == 2) {
        /* After a catch's value: its handler. */
        catch_branch *done = &locals->catches[locals->catch_index];
        sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
        status = finish_branch_value(lowerer,
                                     (branch_value){.block = &done->block,
                                                    .value = done->value,
                                                    .has_value = done->has_value},
                                     &done->clause.handler);
        locals->catch_index += 1;
    }
    while (status == SXML_STATUS_OK && locals->catch_index < locals->catch_count) {
        catch_branch *branch = &locals->catches[locals->catch_index];
        status = add_error_tests(machine, frame, branch);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_slot(lowerer, &branch->clause.error_details);
        }
        locals->scope_mark = lowerer->scope_entry_count;
        if (status == SXML_STATUS_OK) {
            status = push_catch_variables(lowerer, branch);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->step = 2;
        value_source source = {.has_value = false};
        status = start_value(
            machine,
            (value_request){.lowering = locals->head.lowering,
                            .node = branch->node,
                            .block = &branch->block,
                            .atom = &branch->value,
                            .options = {.conflict_error =
                                            "XTSE3150 xsl:catch has both a select attribute "
                                            "and content (XSLT 4.0, 8.4)",
                                        .focus = NULL}},
            &source);
        branch->has_value = source.has_value;
        if (status != SXML_STATUS_OK || source.has_value) {
            return status;
        }
        sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
        status = finish_branch_value(
            lowerer,
            (branch_value){.block = &branch->block, .value = branch->value, .has_value = false},
            &branch->clause.handler);
        locals->catch_index += 1;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_try(machine, frame);
}

static const sxml_xpath_lowering_procedure try_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:try"),
    .step = continue_try,
    .locals_size = sizeof(try_locals)};

/* xsl:catch inside the sequence constructor of xsl:try, and other
   elements that give nothing where they stand: (). */
static sxml_status continue_nothing(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame) {
    return finish_with_empty(machine, frame);
}

static const sxml_xpath_lowering_procedure nothing_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt element without a value"),
    .step = continue_nothing,
    .locals_size = sizeof(sxml_xslt_instruction_locals)};

/* ================================================= Vacuous and deemed empty */

/*
 * The two internal native rows of 8.5 (in no function table, called by
 * STATIC_CALL, as merge-text-nodes): vacuous($value as item()*) as
 * xs:boolean, true when every item is vacuous (8.5.2: a zero-length text
 * node, a document node without children, an atomic item whose string is
 * zero-length, an array whose flattened members are all vacuous); and
 * remove-deemed-empty($value as item()*) as item()*, the items that are
 * not deemed empty (8.5.1: a document or element node without children,
 * another node whose string value is zero-length, a zero-length atomic
 * item, an empty map, an array whose flattened members are all deemed
 * empty). Arrays are flattened with an explicit stack (D71).
 */
typedef enum emptiness_rule {
    SXML_XSLT_EMPTINESS_RULE_VACUOUS,
    SXML_XSLT_EMPTINESS_RULE_DEEMED_EMPTY,
} emptiness_rule;

/* Whether one item that is not an array is empty under rule. */
typedef struct emptiness_question {
    sxml_item item;
    emptiness_rule rule;
} emptiness_question;

static sxml_status is_simple_item_empty(emptiness_question question, emptiness_answer *answer) {
    answer->is_empty = false;
    const sxml_item item = question.item;
    switch (sxml_item_get_kind(item)) {
    case SXML_ITEM_KIND_ATOMIC: {
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof(datum));
        const sxml_status status = sxml_atomic_item_get_datum(item, &datum);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        answer->is_empty = (datum.datum_kind == SXML_DATUM_KIND_STRING &&
                            datum.datum_details.string.utf8.byte_count == 0) ||
                           (datum.datum_kind == SXML_DATUM_KIND_BINARY &&
                            datum.datum_details.binary.byte_count == 0);
        return SXML_STATUS_OK;
    }
    case SXML_ITEM_KIND_MAP: {
        if (question.rule != SXML_XSLT_EMPTINESS_RULE_DEEMED_EMPTY) {
            return SXML_STATUS_OK;
        }
        size_t entry_count = 0;
        const sxml_status status = sxml_map_get_size(item, &entry_count);
        answer->is_empty = entry_count == 0;
        return status;
    }
    case SXML_ITEM_KIND_NODE: {
        sxml_node_reference node = {.tree = NULL, .node_index = {0}, .namespace_number = 0};
        sxml_status status = sxml_item_get_node(item, &node);
        sxml_node_description description;
        memset(&description, 0, sizeof(description));
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(node, &description);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (description.node_kind == SXML_NODE_KIND_DOCUMENT) {
            answer->is_empty = !description.has_children;
            return SXML_STATUS_OK;
        }
        if (description.node_kind == SXML_NODE_KIND_ELEMENT) {
            answer->is_empty =
                question.rule == SXML_XSLT_EMPTINESS_RULE_DEEMED_EMPTY && !description.has_children;
            return SXML_STATUS_OK;
        }
        if (question.rule == SXML_XSLT_EMPTINESS_RULE_VACUOUS &&
            description.node_kind != SXML_NODE_KIND_TEXT) {
            return SXML_STATUS_OK;
        }
        sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_get_content(node, &content);
        answer->is_empty = content.byte_count == 0;
        return status;
    }
    case SXML_ITEM_KIND_JSON_NODE:
    case SXML_ITEM_KIND_FUNCTION:
    case SXML_ITEM_KIND_ARRAY:
    default:
        return SXML_STATUS_OK;
    }
}

/* One array being flattened: the array and its next member. */
typedef struct flattening_entry {
    sxml_item array;
    size_t member_index;
    size_t member_count;
    /* The member's items being looked at. */
    sxml_sequence_iterator iterator;
    bool has_iterator;
} flattening_entry;

/* An explicit stack of arrays, in allocator memory. */
typedef struct flattening_stack {
    sxml_allocator allocator;
    flattening_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
} flattening_stack;

static sxml_status push_array(flattening_stack *stack, sxml_item array) {
    if (stack->entry_count == stack->entry_capacity) {
        const size_t capacity = stack->entry_capacity == 0 ? 8U : stack->entry_capacity * 2U;
        void *memory = NULL;
        sxml_status status = sxml_allocator_allocate(
            stack->allocator,
            (sxml_allocation_request){.byte_count = capacity * sizeof(flattening_entry),
                                      .alignment = alignof(flattening_entry)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (stack->entry_count > 0) {
            memcpy(memory, stack->entries, stack->entry_count * sizeof(flattening_entry));
        }
        sxml_allocator_release(stack->allocator, stack->entries);
        stack->entries = memory;
        stack->entry_capacity = capacity;
    }
    size_t member_count = 0;
    const sxml_status status = sxml_array_get_size(array, &member_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    stack->entries[stack->entry_count] = (flattening_entry){
        .array = array, .member_index = 0, .member_count = member_count, .has_iterator = false};
    stack->entry_count += 1;
    return SXML_STATUS_OK;
}

/* Whether an array is empty under rule: every flattened member is. */
typedef struct array_question {
    const sxml_function_call *call;
    sxml_item array;
    emptiness_rule rule;
} array_question;

static sxml_status is_array_empty(array_question question, emptiness_answer *answer) {
    flattening_stack stack = {.allocator = sxml_item_heap_as_allocator(question.call->item_heap),
                              .entries = NULL,
                              .entry_count = 0,
                              .entry_capacity = 0};
    answer->is_empty = true;
    sxml_status status = push_array(&stack, question.array);
    while (status == SXML_STATUS_OK && answer->is_empty && stack.entry_count > 0) {
        flattening_entry *top = &stack.entries[stack.entry_count - 1];
        if (!top->has_iterator) {
            if (top->member_index == top->member_count) {
                stack.entry_count -= 1;
                continue;
            }
            sxml_sequence member = sxml_sequence_empty();
            status = sxml_array_get_member(top->array, top->member_index, &member);
            top->member_index += 1;
            top->iterator = sxml_sequence_iterator_start(member);
            top->has_iterator = true;
            continue;
        }
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&top->iterator, question.call->error_record, &step);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!step.has_item) {
            top->has_iterator = false;
            continue;
        }
        if (sxml_item_get_kind(step.item) == SXML_ITEM_KIND_ARRAY) {
            status = push_array(&stack, step.item);
            continue;
        }
        emptiness_answer member = {.is_empty = false};
        status = is_simple_item_empty(
            (emptiness_question){.item = step.item, .rule = question.rule}, &member);
        answer->is_empty = answer->is_empty && member.is_empty;
    }
    sxml_allocator_release(stack.allocator, stack.entries);
    return status;
}

static sxml_status is_item_empty(const sxml_function_call *call, emptiness_question question,
                                 emptiness_answer *answer) {
    if (sxml_item_get_kind(question.item) == SXML_ITEM_KIND_ARRAY) {
        return is_array_empty(
            (array_question){.call = call, .array = question.item, .rule = question.rule}, answer);
    }
    return is_simple_item_empty(question, answer);
}

static sxml_status call_vacuous(const sxml_function_call *call, sxml_sequence *result) {
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    emptiness_answer answer = {.is_empty = true};
    sxml_status status = SXML_STATUS_OK;
    while (answer.is_empty) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        status = is_item_empty(
            call, (emptiness_question){.item = step.item, .rule = SXML_XSLT_EMPTINESS_RULE_VACUOUS},
            &answer);
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = sxml_sequence_from_item(answer.is_empty ? sxml_atomic_item_true()
                                                      : sxml_atomic_item_false());
    return SXML_STATUS_OK;
}

static sxml_status call_remove_deemed_empty(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        allocator, &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        emptiness_answer answer = {.is_empty = false};
        status = is_item_empty(
            call,
            (emptiness_question){.item = step.item, .rule = SXML_XSLT_EMPTINESS_RULE_DEEMED_EMPTY},
            &answer);
        if (status == SXML_STATUS_OK && !answer.is_empty) {
            status = sxml_sequence_builder_append_item(builder, step.item);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

static const sxml_function_parameter emptiness_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "item()*"),
};

static const sxml_native_function vacuous_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("vacuous"),
    .parameters = {emptiness_parameters, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:boolean"),
    .call = call_vacuous,
    .resumable_body = NULL};

static const sxml_native_function remove_deemed_empty_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("remove-deemed-empty"),
    .parameters = {emptiness_parameters, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = call_remove_deemed_empty,
    .resumable_body = NULL};

/* ================================================== xsl:where-populated */

/* xsl:where-populated (8.5.1): the content without the items deemed
   empty. */
typedef struct where_populated_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
} where_populated_locals;

static sxml_status continue_where_populated(sxml_xpath_lowering_machine *machine,
                                            sxml_xpath_lowering_frame *frame) {
    where_populated_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return start_content(machine, (value_request){.lowering = locals->head.lowering,
                                                      .node = locals->head.node,
                                                      .block = frame->block,
                                                      .atom = &locals->value});
    }
    sxml_ir_atom populated = {0};
    const sxml_status status =
        bind_native(machine->state,
                    (native_request){.block = frame->block,
                                     .native_function = &remove_deemed_empty_function,
                                     .argument = locals->value},
                    &populated);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_atom(machine, frame, populated);
}

static const sxml_xpath_lowering_procedure where_populated_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:where-populated"),
    .step = continue_where_populated,
    .locals_size = sizeof(where_populated_locals)};

/* ============================================== on-empty and on-non-empty */

/*
 * A sequence constructor with xsl:on-empty or xsl:on-non-empty children
 * (8.5.2, 8.5.3). R, the SEQUENCE of the other children's atoms, is bound
 * in the block; then "if (vacuous(R)) then E else N": E is xsl:on-empty's
 * value (R itself without one), N the children in order with each
 * xsl:on-non-empty's value in its place. The values are lowered in their
 * branches, so only the one chosen is evaluated.
 */
typedef struct conditional_content_locals {
    sxml_xslt_conditional_content_call call;
    sxml_ir_atom ordinary;
    sxml_ir_atom is_vacuous;
    sxml_xpath_lowering_block empty_block;
    sxml_ir_atom empty_value;
    bool has_empty_value;
    sxml_xpath_lowering_block populated_block;
    /* Atoms of the populated branch, one per child with a value (owned
       memory 0), and how far the walk is. */
    sxml_ir_atom *populated_atoms;
    size_t populated_count;
    size_t ordinary_index;
    sxml_xslt_ast_node_index next_child;
    /* The variables of the children after an xsl:on-non-empty, taken out
       of scope while its value is lowered (owned memory 1). */
    sxml_xpath_lowering_scope_entry *hidden_entries;
    size_t hidden_count;
    size_t variable_count;
} conditional_content_locals;

enum {
    SXML_XSLT_CONDITIONAL_STEP_START = 0,
    SXML_XSLT_CONDITIONAL_STEP_POPULATED = 1,
};

static sxml_status start_conditional_content(sxml_xpath_lowering_machine *machine,
                                             sxml_xpath_lowering_frame *frame,
                                             step_outcome *outcome) {
    sxml_xpath_lowerer *lowerer = machine->state;
    conditional_content_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->call.lowering;
    size_t child_count = 0;
    sxml_xslt_ast_node_index on_empty = {.value = 0};
    for (sxml_xslt_ast_node_index child = locals->call.first; child.value != 0;
         child = next_of(lowering, child)) {
        child_count += 1;
        const sxml_xslt_ast_kind kind = kind_of(lowering, child);
        if (on_empty.value != 0 && kind != SXML_XSLT_AST_KIND_FALLBACK &&
            kind != SXML_XSLT_AST_KIND_CATCH) {
            /* 8.5.2: only xsl:fallback and non-instructions may follow the
               one xsl:on-empty. */
            return report_static(lowering, on_empty,
                                 SXML_BYTE_VIEW_FROM_LITERAL(
                                     "XTSE0010 xsl:on-empty is followed by an instruction, a text "
                                     "node or a literal result element (XSLT 4.0, 8.5.2)"));
        }
        if (kind == SXML_XSLT_AST_KIND_ON_EMPTY) {
            on_empty = child;
        }
    }
    sxml_ir_expression ordinary = {0};
    sxml_status status = make_sequence(lowerer, locals->call.atoms, &ordinary);
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer, (binding){.block = frame->block, .expression = ordinary},
                      &locals->ordinary);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_native(lowerer,
                             (native_request){.block = frame->block,
                                              .native_function = &vacuous_function,
                                              .argument = locals->ordinary},
                             &locals->is_vacuous);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = child_count,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->populated_atoms = frame->owned_memory[0];
    locals->next_child = locals->call.first;
    frame->step = SXML_XSLT_CONDITIONAL_STEP_POPULATED;
    outcome->is_pushed = false;
    if (on_empty.value == 0) {
        return SXML_STATUS_OK;
    }
    value_source source = {.has_value = false};
    status = start_value(
        machine,
        (value_request){.lowering = lowering,
                        .node = on_empty,
                        .block = &locals->empty_block,
                        .atom = &locals->empty_value,
                        .options = {.conflict_error =
                                        "XTSE3185 xsl:on-empty has both a select attribute "
                                        "and content (XSLT 4.0, 8.5.2)",
                                    .focus = NULL}},
        &source);
    locals->has_empty_value = true;
    outcome->is_pushed = source.has_value;
    if (status == SXML_STATUS_OK && !source.has_value) {
        status = add_empty_atom(lowerer, &locals->empty_value);
    }
    return status;
}

/* Walks the children for the populated branch until an xsl:on-non-empty
   is pushed (outcome->is_pushed) or the walk ends. */
/* Puts the variables hidden for an xsl:on-non-empty back in scope. */
static sxml_status restore_hidden(sxml_xpath_lowerer *lowerer, conditional_content_locals *locals) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < locals->hidden_count;
         entry_index += 1) {
        status = sxml_xpath_lowerer_push_variable(lowerer, locals->hidden_entries[entry_index]);
    }
    locals->hidden_count = 0;
    return status;
}

/* 9.9: an xsl:on-non-empty sees the variables of the children before it
   only; the later ones are taken out of scope (and kept to put back). */
static sxml_status hide_later_variables(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    conditional_content_locals *locals = frame->locals;
    const size_t visible = locals->call.scope_mark + locals->variable_count;
    if (lowerer->scope_entry_count <= visible) {
        return SXML_STATUS_OK;
    }
    const size_t hidden_count = lowerer->scope_entry_count - visible;
    if (locals->hidden_entries == NULL) {
        const sxml_status status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 1,
                .scratch = {.element_count = hidden_count,
                            .element_size = sizeof(sxml_xpath_lowering_scope_entry),
                            .element_alignment = alignof(sxml_xpath_lowering_scope_entry)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->hidden_entries = frame->owned_memory[1];
    }
    memcpy(locals->hidden_entries, lowerer->scope_entries + visible,
           hidden_count * sizeof(sxml_xpath_lowering_scope_entry));
    locals->hidden_count = hidden_count;
    sxml_xpath_lowerer_pop_variables(lowerer, visible);
    return SXML_STATUS_OK;
}

static sxml_status walk_populated(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    conditional_content_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->call.lowering;
    outcome->is_pushed = false;
    sxml_status status = restore_hidden(machine->state, locals);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    while (locals->next_child.value != 0) {
        const sxml_xslt_ast_node_index child = locals->next_child;
        locals->next_child = next_of(lowering, child);
        const sxml_xslt_ast_kind kind = kind_of(lowering, child);
        if (kind == SXML_XSLT_AST_KIND_FALLBACK || kind == SXML_XSLT_AST_KIND_ON_EMPTY) {
            continue;
        }
        if (kind == SXML_XSLT_AST_KIND_VARIABLE) {
            locals->variable_count += 1;
        }
        if (kind != SXML_XSLT_AST_KIND_ON_NON_EMPTY) {
            if (locals->ordinary_index < locals->call.atoms.atom_count) {
                locals->populated_atoms[locals->populated_count] =
                    locals->call.atoms.atoms[locals->ordinary_index];
                locals->populated_count += 1;
            }
            locals->ordinary_index += 1;
            continue;
        }
        sxml_ir_atom *atom = &locals->populated_atoms[locals->populated_count];
        locals->populated_count += 1;
        status = hide_later_variables(machine, frame);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        value_source source = {.has_value = false};
        status = start_value(
            machine,
            (value_request){.lowering = lowering,
                            .node = child,
                            .block = &locals->populated_block,
                            .atom = atom,
                            .options = {.conflict_error =
                                            "XTSE3185 xsl:on-non-empty has both a select "
                                            "attribute and content (XSLT 4.0, 8.5.3)",
                                        .focus = NULL}},
            &source);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (source.has_value) {
            outcome->is_pushed = true;
            return SXML_STATUS_OK;
        }
        locals->populated_count -= 1;
        status = restore_hidden(machine->state, locals);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status finish_conditional_content(sxml_xpath_lowering_machine *machine,
                                              sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    conditional_content_locals *locals = frame->locals;
    sxml_ir_expression populated = {0};
    sxml_status status = make_sequence(lowerer,
                                       (sxml_ir_atom_view){.atoms = locals->populated_atoms,
                                                           .atom_count = locals->populated_count},
                                       &populated);
    sxml_ir_atom populated_value = {0};
    if (status == SXML_STATUS_OK) {
        status =
            bind(lowerer, (binding){.block = &locals->populated_block, .expression = populated},
                 &populated_value);
    }
    conditional_parts parts = {.condition = locals->is_vacuous};
    if (status == SXML_STATUS_OK) {
        status =
            finish_branch(lowerer,
                          (branch_result){.block = &locals->empty_block,
                                          .value = locals->has_empty_value ? locals->empty_value
                                                                           : locals->ordinary},
                          &parts.then_branch);
    }
    if (status == SXML_STATUS_OK) {
        status = finish_branch(
            lowerer, (branch_result){.block = &locals->populated_block, .value = populated_value},
            &parts.else_branch);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(machine, frame, conditional_expression(parts));
}

static sxml_status continue_conditional_content(sxml_xpath_lowering_machine *machine,
                                                sxml_xpath_lowering_frame *frame) {
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == SXML_XSLT_CONDITIONAL_STEP_START) {
        step_outcome outcome = {.is_pushed = false};
        status = start_conditional_content(machine, frame, &outcome);
        if (status != SXML_STATUS_OK || outcome.is_pushed) {
            return status;
        }
    }
    if (frame->step == SXML_XSLT_CONDITIONAL_STEP_POPULATED) {
        step_outcome outcome = {.is_pushed = false};
        status = walk_populated(machine, frame, &outcome);
        if (status != SXML_STATUS_OK || outcome.is_pushed) {
            return status;
        }
    }
    return finish_conditional_content(machine, frame);
}

static const sxml_xpath_lowering_procedure conditional_content_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt conditional content"),
    .step = continue_conditional_content,
    .locals_size = sizeof(conditional_content_locals)};

sxml_status
sxml_xslt_lowering_call_conditional_content(sxml_xpath_lowering_machine *machine,
                                            const sxml_xslt_conditional_content_call *call) {
    const conditional_content_locals initial = {.call = *call};
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &conditional_content_procedure,
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

/* ===================================================== message and assert */

/*
 * xsl:message (23.1): the select value and then the content's, as a
 * document node (the message body), handed to fn:message (the trace
 * sink). With terminate "yes" (an attribute value template), fn:error
 * then raises the error-code (default err:XTMM9000) with the body as its
 * value. xsl:assert (23.2): when the test's effective boolean value is
 * false, fn:error raises the error-code (default err:XTMM9001) with the
 * body; assertions are always enabled.
 */
typedef struct message_locals {
    sxml_xslt_instruction_locals head;
    bool is_assert;
    sxml_ir_atom test;
    sxml_xpath_lowering_block body_block;
    sxml_ir_atom select;
    bool has_select;
    sxml_ir_atom content;
    bool has_content;
    sxml_ir_atom terminate;
    sxml_ir_atom error_code;
    /* The error-code value template has enclosed expressions: its string
       is cast to xs:QName when evaluated. */
    bool is_computed_code;
} message_locals;

enum {
    SXML_XSLT_MESSAGE_STEP_START = 0,
    SXML_XSLT_MESSAGE_STEP_SELECT = 1,
    SXML_XSLT_MESSAGE_STEP_CONTENT = 2,
    SXML_XSLT_MESSAGE_STEP_TERMINATE = 3,
    SXML_XSLT_MESSAGE_STEP_COMPARE_TERMINATE = 4,
    SXML_XSLT_MESSAGE_STEP_ERROR_CODE = 5,
    SXML_XSLT_MESSAGE_STEP_RESOLVE_CODE = 6,
    SXML_XSLT_MESSAGE_STEP_FINISH = 7,
};

/* The block the body goes to: the else branch of an assertion, the
   caller's block for a message. */
static sxml_xpath_lowering_block *message_block(sxml_xpath_lowering_frame *frame) {
    message_locals *locals = frame->locals;
    return locals->is_assert ? &locals->body_block : frame->block;
}

/* An error code written as text (23.1): "Q{uri}local", "prefix:local"
   or "local" (no namespace), parsed without the namespaces; is_valid
   false for anything else, which means the default code. */
typedef struct error_code_text {
    bool is_valid;
    bool has_uri;
    sxml_byte_view namespace_uri;
    bool has_prefix;
    sxml_byte_view prefix;
    sxml_byte_view local_name;
} error_code_text;

static bool is_name_byte(uint8_t byte) {
    return byte != ':' && byte != '{' && byte != '}' && byte != ' ' && byte != '\t' &&
           byte != '\n' && byte != '\r';
}

static bool is_name_text(sxml_byte_view text) {
    if (text.byte_count == 0 || (text.bytes[0] >= '0' && text.bytes[0] <= '9') ||
        text.bytes[0] == '-' || text.bytes[0] == '.') {
        return false;
    }
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (!is_name_byte(text.bytes[byte_index])) {
            return false;
        }
    }
    return true;
}

static error_code_text parse_error_code(sxml_byte_view text) {
    const sxml_byte_view trimmed = sxml_xml_text_trim_whitespace(text);
    error_code_text parsed = {.is_valid = false};
    if (trimmed.byte_count > 2 && trimmed.bytes[0] == 'Q' && trimmed.bytes[1] == '{') {
        size_t close = 2;
        while (close < trimmed.byte_count && trimmed.bytes[close] != '}') {
            close += 1;
        }
        if (close == trimmed.byte_count) {
            return parsed;
        }
        parsed.has_uri = true;
        parsed.namespace_uri =
            (sxml_byte_view){.bytes = trimmed.bytes + 2, .byte_count = close - 2};
        parsed.local_name = (sxml_byte_view){.bytes = trimmed.bytes + close + 1,
                                             .byte_count = trimmed.byte_count - close - 1};
    } else {
        size_t colon = 0;
        while (colon < trimmed.byte_count && trimmed.bytes[colon] != ':') {
            colon += 1;
        }
        parsed.local_name = trimmed;
        if (colon < trimmed.byte_count) {
            parsed.has_prefix = true;
            parsed.prefix = (sxml_byte_view){.bytes = trimmed.bytes, .byte_count = colon};
            parsed.local_name = (sxml_byte_view){.bytes = trimmed.bytes + colon + 1,
                                                 .byte_count = trimmed.byte_count - colon - 1};
            if (!is_name_text(parsed.prefix)) {
                return parsed;
            }
        }
    }
    parsed.is_valid = is_name_text(parsed.local_name);
    return parsed;
}

/* The prefix's URI among bindings; false when it is not bound. */
typedef struct prefix_lookup {
    sxml_name_pool *name_pool;
    sxml_namespace_binding_view bindings;
} prefix_lookup;

static bool find_prefix_uri(prefix_lookup lookup, sxml_byte_view prefix,
                            sxml_namespace_uri_identifier *namespace_uri) {
    for (size_t binding_index = 0; binding_index < lookup.bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding namespace_binding =
            lookup.bindings.namespace_bindings[binding_index];
        sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
        if (namespace_binding.namespace_prefix.index != 0 &&
            sxml_name_pool_get_namespace_prefix(
                lookup.name_pool, namespace_binding.namespace_prefix, &text) == SXML_STATUS_OK &&
            text.byte_count == prefix.byte_count &&
            memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0) {
            *namespace_uri = namespace_binding.namespace_uri;
            return true;
        }
    }
    return false;
}

/* The expanded name of an error code, or default_name when it is not a
   valid EQName (23.1) or its prefix is not bound. */
typedef struct error_code_request {
    prefix_lookup namespaces;
    sxml_byte_view text;
    sxml_name_identifier default_name;
} error_code_request;

static sxml_status resolve_error_code(error_code_request request, sxml_name_identifier *name) {
    const error_code_text parsed = parse_error_code(request.text);
    *name = request.default_name;
    if (!parsed.is_valid) {
        return SXML_STATUS_OK;
    }
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    sxml_status status = SXML_STATUS_OK;
    if (parsed.has_uri) {
        status = sxml_name_pool_intern_namespace_uri(request.namespaces.name_pool,
                                                     parsed.namespace_uri, &namespace_uri);
    } else if (parsed.has_prefix &&
               !find_prefix_uri(request.namespaces, parsed.prefix, &namespace_uri)) {
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_pool_intern_name(
        request.namespaces.name_pool,
        &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = parsed.local_name},
        name);
}

/* The default error code of the instruction: err:XTMM9000 or (assert)
   err:XTMM9001. */
static sxml_status default_error_code(sxml_name_pool *name_pool, const message_locals *locals,
                                      sxml_name_identifier *name) {
    const bool is_assert = locals->is_assert;
    return sxml_name_pool_intern_name(
        name_pool,
        &(sxml_name_request){.namespace_uri =
                                 sxml_well_known_namespace_identifier(SXML_NAMESPACE_ERRORS),
                             .local_name = is_assert ? SXML_BYTE_VIEW_FROM_LITERAL("XTMM9001")
                                                     : SXML_BYTE_VIEW_FROM_LITERAL("XTMM9000")},
        name);
}

/* The string of an atomic item (an xs:string or xs:QName argument). */
static sxml_status read_datum(sxml_optional_item argument, sxml_atomic_datum *datum) {
    memset(datum, 0, sizeof(*datum));
    if (!argument.has_item || sxml_item_get_kind(argument.item) != SXML_ITEM_KIND_ATOMIC) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_atomic_item_get_datum(argument.item, datum);
}

/*
 * The internal native row error-code($code as item()*, $namespaces as
 * item()*, $default as item()*) as xs:QName: the value of a computed
 * error-code (an xs:string) resolved with the instruction's namespaces
 * ($namespaces, a map from prefix to URI), else $default.
 */
static sxml_status call_error_code(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item arguments[3] = {
        {.has_item = false}, {.has_item = false}, {.has_item = false}};
    sxml_status status = SXML_STATUS_OK;
    for (size_t argument_index = 0; status == SXML_STATUS_OK && argument_index < 3;
         argument_index += 1) {
        status = sxml_function_library_read_optional_item(call, argument_index,
                                                          &arguments[argument_index]);
    }
    sxml_atomic_datum code = {0};
    sxml_atomic_datum default_code = {0};
    if (status == SXML_STATUS_OK) {
        status = read_datum(arguments[0], &code);
    }
    if (status == SXML_STATUS_OK) {
        status = read_datum(arguments[2], &default_code);
    }
    if (status != SXML_STATUS_OK || code.datum_kind != SXML_DATUM_KIND_STRING ||
        default_code.datum_kind != SXML_DATUM_KIND_QNAME || !arguments[1].has_item) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    const error_code_text parsed = parse_error_code(code.datum_details.string.utf8);
    sxml_name_identifier name = default_code.datum_details.qname.expanded_name;
    if (parsed.is_valid && parsed.has_prefix) {
        /* The prefix's URI from the map. */
        sxml_item key = {.representation = 0};
        status =
            sxml_atomic_item_make(call->item_heap,
                                  &(sxml_atomic_item_request){
                                      .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                                      .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                                                .datum_details = {.string = {.utf8 = parsed.prefix,
                                                                             .codepoint_count = 0,
                                                                             .is_ascii = false}}}},
                                  &key);
        sxml_map_lookup lookup;
        memset(&lookup, 0, sizeof(lookup));
        if (status == SXML_STATUS_OK) {
            status = sxml_map_find(arguments[1].item, key, &lookup);
            sxml_item_release(key);
        }
        sxml_item uri_item = {.representation = 0};
        if (status == SXML_STATUS_OK && lookup.is_found &&
            sxml_sequence_get_item(
                lookup.entry.value,
                (sxml_item_request){.item_index = 0, .error_record = call->error_record},
                &uri_item) == SXML_STATUS_OK) {
            sxml_atomic_datum uri = {0};
            sxml_namespace_uri_identifier namespace_uri = {.index = 0};
            status = sxml_atomic_item_get_datum(uri_item, &uri);
            if (status == SXML_STATUS_OK) {
                status = sxml_name_pool_intern_namespace_uri(
                    name_pool, uri.datum_details.string.utf8, &namespace_uri);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_name_pool_intern_name(
                    name_pool,
                    &(sxml_name_request){.namespace_uri = namespace_uri,
                                         .local_name = parsed.local_name},
                    &name);
            }
        }
    } else if (parsed.is_valid) {
        status = resolve_error_code(
            (error_code_request){.namespaces = {.name_pool = name_pool,
                                                .bindings = {.namespace_bindings = NULL,
                                                             .namespace_binding_count = 0}},
                                 .text = code.datum_details.string.utf8,
                                 .default_name = name},
            &name);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = make_qname_item(call->item_heap, name, &item);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = sxml_sequence_from_item(item);
    return SXML_STATUS_OK;
}

/*
 * The internal native row xslt-message($body as item()*, $terminating as
 * item()*, $code as item()*) as empty-sequence(): hands the body to the
 * trace sink as an XSLT_MESSAGE event (include/sxml/processor.h) with
 * is_terminating ($terminating, an xs:boolean) and the error code
 * ($code, an xs:QName). The null sink, and a call outside the processor's
 * evaluations, discard it.
 */
static sxml_status call_xslt_message(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const sxml_processor_document_loader *loader =
        sxml_processor_document_loader_from_dynamic_context(call->dynamic_context);
    if (loader == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_optional_item terminating = {.has_item = false};
    sxml_optional_item code = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &terminating);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 2, &code);
    }
    sxml_atomic_datum terminating_datum = {0};
    sxml_atomic_datum code_datum = {0};
    if (status == SXML_STATUS_OK) {
        status = read_datum(terminating, &terminating_datum);
    }
    if (status == SXML_STATUS_OK) {
        status = read_datum(code, &code_datum);
    }
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(
            name_pool, code_datum.datum_details.qname.expanded_name, &parts);
    }
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri, &namespace_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_processor_document_loader_trace(
        loader,
        &(sxml_trace_event){.trace_event_kind = SXML_TRACE_EVENT_KIND_XSLT_MESSAGE,
                            .label = {.bytes = NULL, .byte_count = 0},
                            .value = sxml_function_library_get_argument(call, 0),
                            .is_terminating = terminating_datum.datum_details.boolean,
                            .error_code = {.namespace_uri = namespace_uri,
                                           .local_name = parts.local_name,
                                           .namespace_prefix = {.bytes = NULL, .byte_count = 0}},
                            .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                         .line_number = 0,
                                         .column_number = 0,
                                         .byte_offset = 0},
                            .processor = NULL});
}

static const sxml_function_parameter message_parameters[] = {
    SXML_REQUIRED_PARAMETER("body", "item()*"),
    SXML_REQUIRED_PARAMETER("terminating", "item()*"),
    SXML_REQUIRED_PARAMETER("code", "item()*"),
};

static const sxml_native_function error_code_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("error-code"),
    .parameters = {message_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:QName"),
    .call = call_error_code,
    .resumable_body = NULL};

static const sxml_native_function xslt_message_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("xslt-message"),
    .parameters = {message_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("empty-sequence()"),
    .call = call_xslt_message,
    .resumable_body = NULL};

/* A call of a native row of three arguments, bound in block. */
typedef struct native_call_request {
    sxml_xpath_lowering_block *block;
    const sxml_native_function *native_function;
    const sxml_ir_atom *arguments;
} native_call_request;

static sxml_status bind_native_call(sxml_xpath_lowerer *lowerer, native_call_request request,
                                    sxml_ir_atom *atom) {
    sxml_ir_atom_list arguments = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(
        lowerer->unit, (sxml_ir_atom_view){.atoms = request.arguments, .atom_count = 3},
        &arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(
        lowerer,
        (binding){
            .block = request.block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                           .static_call = {.target = {.target_kind =
                                                          SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                      .native_function = request.native_function},
                                           .arguments = arguments}}},
        atom);
}

/* The instruction's in-scope namespaces as a map constant from prefix to
   URI, for a computed error code. */
static sxml_status add_namespace_map(sxml_xpath_lowerer *lowerer, const message_locals *locals,
                                     sxml_ir_atom *atom) {
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_namespace_binding_view bindings = sxml_xslt_ast_get_namespace_bindings(
        lowering->ast, node_of(lowering, locals->head.node)->static_context);
    const size_t entry_count = bindings.namespace_binding_count;
    void *memory = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (entry_count > 0) {
        status = sxml_xpath_lowerer_allocate_scratch(
            lowerer,
            (sxml_xpath_lowering_scratch_request){.element_count = entry_count * 2U,
                                                  .element_size = sizeof(sxml_item),
                                                  .element_alignment = alignof(sxml_item)},
            &memory);
    }
    sxml_item *items = memory;
    size_t made = 0;
    for (size_t binding_index = 0; status == SXML_STATUS_OK && binding_index < entry_count;
         binding_index += 1) {
        const sxml_namespace_binding namespace_binding = bindings.namespace_bindings[binding_index];
        sxml_byte_view texts[2] = {{.bytes = NULL, .byte_count = 0},
                                   {.bytes = NULL, .byte_count = 0}};
        status = sxml_name_pool_get_namespace_prefix(lowerer->name_pool,
                                                     namespace_binding.namespace_prefix, &texts[0]);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(lowerer->name_pool,
                                                      namespace_binding.namespace_uri, &texts[1]);
        }
        for (size_t text_index = 0; status == SXML_STATUS_OK && text_index < 2; text_index += 1) {
            status = sxml_atomic_item_make(
                lowerer->item_heap,
                &(sxml_atomic_item_request){
                    .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                    .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                              .datum_details = {.string = {.utf8 = texts[text_index],
                                                           .codepoint_count = 0,
                                                           .is_ascii = false}}}},
                &items[made]);
            made += status == SXML_STATUS_OK ? 1U : 0U;
        }
    }
    void *entry_memory = NULL;
    if (status == SXML_STATUS_OK && entry_count > 0) {
        status = sxml_xpath_lowerer_allocate_scratch(
            lowerer,
            (sxml_xpath_lowering_scratch_request){.element_count = entry_count,
                                                  .element_size = sizeof(sxml_map_entry),
                                                  .element_alignment = alignof(sxml_map_entry)},
            &entry_memory);
    }
    sxml_map_entry *entries = entry_memory;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        entries[entry_index] =
            (sxml_map_entry){.key = items[entry_index * 2U],
                             .value = sxml_sequence_from_item(items[entry_index * 2U + 1U])};
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            lowerer->item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries, .map_entry_count = entry_count},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                .error_record = NULL},
            &map);
    }
    for (size_t item_index = 0; item_index < made; item_index += 1) {
        sxml_item_release(items[item_index]);
    }
    sxml_xpath_lowerer_release_scratch(lowerer, memory);
    sxml_xpath_lowerer_release_scratch(lowerer, entry_memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_item_constant(lowerer, map, atom);
}

/* The error-code atom: a QName constant, or (outcome->is_pushed) the value
   template's string, resolved by the error-code row when evaluated. */
static sxml_status start_error_code(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    sxml_xpath_lowerer *lowerer = machine->state;
    message_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    outcome->is_pushed = false;
    const sxml_xslt_ast_attribute *error_code =
        find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                    .kind = SXML_XSLT_AST_ATTRIBUTE_ERROR_CODE});
    const fixed_text fixed = read_fixed_text(lowering, error_code);
    if (error_code->kind != SXML_XSLT_AST_ATTRIBUTE_NONE && !fixed.is_fixed) {
        outcome->is_pushed = true;
        locals->is_computed_code = true;
        return sxml_xslt_lowering_call_value_template(
            machine,
            &(sxml_xslt_expression_call){.lowering = lowering,
                                         .expression = error_code->value.payload.expression,
                                         .block = message_block(frame),
                                         .atom = &locals->error_code,
                                         .focus = NULL});
    }
    sxml_name_identifier name = {.index = 0};
    sxml_status status = default_error_code(lowerer->name_pool, locals, &name);
    if (status == SXML_STATUS_OK && error_code->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
        status = resolve_error_code(
            (error_code_request){
                .namespaces = {.name_pool = lowerer->name_pool,
                               .bindings = sxml_xslt_ast_get_namespace_bindings(
                                   lowering->ast,
                                   node_of(lowering, locals->head.node)->static_context)},
                .text = fixed.text,
                .default_name = name},
            &name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_qname(lowerer, name, &locals->error_code);
}

/* A computed error code: error-code(string, namespaces, default). */
static sxml_status bind_computed_error_code(sxml_xpath_lowerer *lowerer,
                                            sxml_xpath_lowering_frame *frame) {
    message_locals *locals = frame->locals;
    sxml_ir_atom arguments[3] = {locals->error_code, {0}, {0}};
    sxml_status status = add_namespace_map(lowerer, locals, &arguments[1]);
    sxml_name_identifier default_name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = default_error_code(lowerer->name_pool, locals, &default_name);
    }
    if (status == SXML_STATUS_OK) {
        status = add_qname(lowerer, default_name, &arguments[2]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_native_call(lowerer,
                            (native_call_request){.block = message_block(frame),
                                                  .native_function = &error_code_function,
                                                  .arguments = arguments},
                            &locals->error_code);
}

/* terminate (23.1): "yes", "true" or "1" terminate; a value template with
   enclosed expressions is compared when evaluated (outcome->is_pushed). */
static sxml_status start_terminate(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    sxml_xpath_lowerer *lowerer = machine->state;
    message_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    outcome->is_pushed = false;
    const sxml_xslt_ast_attribute *terminate =
        find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                    .kind = SXML_XSLT_AST_ATTRIBUTE_TERMINATE});
    const bool is_absent = terminate->kind == SXML_XSLT_AST_ATTRIBUTE_NONE;
    const fixed_text fixed = read_fixed_text(lowering, terminate);
    if (!is_absent && !fixed.is_fixed) {
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_value_template(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = terminate->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->terminate,
                                                  .focus = NULL});
    }
    const sxml_xslt_boolean_text answer = sxml_xslt_lowering_read_boolean_text(fixed.text);
    const bool is_yes = !is_absent && answer.is_yes;
    const bool is_no = is_absent || (answer.is_valid && !answer.is_yes);
    if (!is_yes && !is_no) {
        return report_static(lowering, locals->head.node,
                             SXML_BYTE_VIEW_FROM_LITERAL(
                                 "XTSE0020 the terminate attribute of xsl:message is not yes or no "
                                 "(XSLT 4.0, 23.1)"));
    }
    return sxml_xpath_lowerer_add_item_constant(
        lowerer, is_yes ? sxml_atomic_item_true() : sxml_atomic_item_false(), &locals->terminate);
}

/* A computed terminate value: normalize-space(.) = ("yes", "true", "1"). */
static sxml_status compare_terminate(sxml_xpath_lowerer *lowerer,
                                     sxml_xpath_lowering_frame *frame) {
    message_locals *locals = frame->locals;
    return sxml_xslt_lowering_bind_boolean_value(
        lowerer, (sxml_xslt_boolean_value){.block = frame->block, .value = locals->terminate},
        &locals->terminate);
}

/* The message body (23.1): a document node of the select value (as by
   xsl:copy-of) and the content's, bound in the message block. A body
   that cannot be built (an attribute at the top, 5.7.1) gives an empty
   document: the transformation does not fail for it. */
static const sxml_name_test any_error_test = {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME,
                                              .expanded_name = {.index = 0},
                                              .namespace_uri = {.index = 0},
                                              .local_name = {.bytes = NULL, .byte_count = 0}};

static sxml_status bind_message_body(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_frame *frame,
                                     sxml_ir_atom *body) {
    message_locals *locals = frame->locals;
    sxml_ir_atom parts[2] = {{0}, {0}};
    size_t part_count = 0;
    if (locals->has_select) {
        parts[part_count] = locals->select;
        part_count += 1;
    }
    if (locals->has_content) {
        parts[part_count] = locals->content;
        part_count += 1;
    }
    sxml_ir_expression sequence = {0};
    sxml_status status = make_sequence(
        lowerer, (sxml_ir_atom_view){.atoms = parts, .atom_count = part_count}, &sequence);
    sxml_ir_expression_index content = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, sequence, &content);
    }
    sxml_ir_expression_index document = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                .node_construction = {.content = content,
                                      .construction_flags =
                                          (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES}},
            &document);
    }
    sxml_ir_catch_clause clause;
    memset(&clause, 0, sizeof(clause));
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_name_tests(
            lowerer->unit,
            (sxml_ir_name_test_view){.name_tests = &any_error_test, .name_test_count = 1},
            &clause.error_tests);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(lowerer, &clause.error_details);
    }
    sxml_ir_expression empty = {0};
    sxml_ir_expression_index empty_content = {0};
    if (status == SXML_STATUS_OK) {
        status =
            make_sequence(lowerer, (sxml_ir_atom_view){.atoms = NULL, .atom_count = 0}, &empty);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, empty, &empty_content);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(
            lowerer,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                .node_construction = {.content = empty_content,
                                      .construction_flags =
                                          (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES}},
            &clause.handler);
    }
    sxml_ir_catch_clause_list clauses = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_catch_clauses(
            lowerer->unit,
            (sxml_ir_catch_clause_view){.catch_clauses = &clause, .catch_clause_count = 1},
            &clauses);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(lowerer,
                (binding){.block = message_block(frame),
                          .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH,
                                         .try_catch = {.body = document,
                                                       .catch_clauses = clauses,
                                                       .finally_body = {.value = 0}}}},
                body);
}

/* xslt-message(body, terminating, code), bound in block. */
typedef struct message_call {
    const message_locals *locals;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom body;
    sxml_ir_atom terminating;
} message_call;

static sxml_status bind_message_call(sxml_xpath_lowerer *lowerer, message_call call,
                                     sxml_ir_atom *sent) {
    const sxml_ir_atom arguments[3] = {call.body, call.terminating, call.locals->error_code};
    return bind_native_call(lowerer,
                            (native_call_request){.block = call.block,
                                                  .native_function = &xslt_message_function,
                                                  .arguments = arguments},
                            sent);
}

/* fn:error(code, string(body), body), bound in block. */
typedef struct error_call {
    const message_locals *locals;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom body;
} error_call;

static sxml_status bind_message_error(sxml_xpath_lowerer *lowerer, error_call call,
                                      sxml_ir_atom *raised) {
    sxml_ir_atom description = {0};
    sxml_status status =
        bind_call(lowerer,
                  (call_request){.block = call.block,
                                 .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string"),
                                 .arguments = {.atoms = &call.body, .atom_count = 1}},
                  &description);
    const sxml_ir_atom arguments[3] = {call.locals->error_code, description, call.body};
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_call(lowerer,
                     (call_request){.block = call.block,
                                    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("error"),
                                    .arguments = {.atoms = arguments, .atom_count = 3}},
                     raised);
}

/* A message: (xslt-message(body, terminate, code), if (terminate) then
   error else ()). An assertion: if (test) then () else
   (xslt-message(body, true(), code), error). */
static sxml_status finish_message(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    message_locals *locals = frame->locals;
    sxml_ir_atom body = {0};
    sxml_status status = bind_message_body(lowerer, frame, &body);
    sxml_ir_atom terminating = locals->terminate;
    if (status == SXML_STATUS_OK && locals->is_assert) {
        status =
            sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_true(), &terminating);
    }
    sxml_ir_atom sent = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_message_call(lowerer,
                                   (message_call){.locals = locals,
                                                  .block = message_block(frame),
                                                  .body = body,
                                                  .terminating = terminating},
                                   &sent);
    }
    sxml_ir_atom raised = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_message_error(
            lowerer, (error_call){.locals = locals, .block = &locals->body_block, .body = body},
            &raised);
    }
    sxml_ir_atom empty = {0};
    if (status == SXML_STATUS_OK) {
        status = add_empty_atom(lowerer, &empty);
    }
    sxml_xpath_lowering_block empty_block = {.first = {0}, .open_let = {0}};
    sxml_ir_expression_index quiet = {0};
    sxml_ir_expression_index failing = {0};
    if (status == SXML_STATUS_OK) {
        status =
            finish_branch(lowerer, (branch_result){.block = &empty_block, .value = empty}, &quiet);
    }
    if (status == SXML_STATUS_OK) {
        status = finish_branch(
            lowerer, (branch_result){.block = &locals->body_block, .value = raised}, &failing);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (locals->is_assert) {
        /* The body and the message were bound in the failing branch. */
        return finish_with_expression(
            machine, frame,
            conditional_expression((conditional_parts){
                .condition = locals->test, .then_branch = quiet, .else_branch = failing}));
    }
    sxml_ir_atom termination = {0};
    status = bind(
        lowerer,
        (binding){
            .block = frame->block,
            .expression = conditional_expression((conditional_parts){
                .condition = locals->terminate, .then_branch = failing, .else_branch = quiet})},
        &termination);
    const sxml_ir_atom results[2] = {sent, termination};
    sxml_ir_expression result = {0};
    if (status == SXML_STATUS_OK) {
        status =
            make_sequence(lowerer, (sxml_ir_atom_view){.atoms = results, .atom_count = 2}, &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(machine, frame, result);
}

/* One step of a message or assertion: lowers the next part, or finishes.
 outcome->is_pushed when a frame was pushed. */
static sxml_status step_message(sxml_xpath_lowering_machine *machine,
                                sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    message_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    outcome->is_pushed = false;
    switch (frame->step) {
    case SXML_XSLT_MESSAGE_STEP_START: {
        frame->step = SXML_XSLT_MESSAGE_STEP_SELECT;
        locals->is_assert = kind_of(lowering, locals->head.node) == SXML_XSLT_AST_KIND_ASSERT;
        if (!locals->is_assert) {
            return SXML_STATUS_OK;
        }
        const sxml_xslt_ast_attribute *test =
            find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                        .kind = SXML_XSLT_AST_ATTRIBUTE_TEST});
        outcome->is_pushed = true;
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = test->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->test,
                                                  .focus = NULL});
    }
    case SXML_XSLT_MESSAGE_STEP_SELECT: {
        frame->step = SXML_XSLT_MESSAGE_STEP_CONTENT;
        const sxml_status status =
            start_attribute_value(machine,
                                  (attribute_value_request){.head = &locals->head,
                                                            .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                            .block = message_block(frame),
                                                            .atom = &locals->select},
                                  outcome);
        locals->has_select = outcome->is_pushed;
        return status;
    }
    case SXML_XSLT_MESSAGE_STEP_CONTENT:
        frame->step = locals->is_assert ? SXML_XSLT_MESSAGE_STEP_ERROR_CODE
                                        : SXML_XSLT_MESSAGE_STEP_TERMINATE;
        locals->has_content = has_content(lowering, locals->head.node);
        outcome->is_pushed = locals->has_content;
        if (!locals->has_content) {
            return SXML_STATUS_OK;
        }
        return start_content(machine, (value_request){.lowering = lowering,
                                                      .node = locals->head.node,
                                                      .block = message_block(frame),
                                                      .atom = &locals->content});
    case SXML_XSLT_MESSAGE_STEP_TERMINATE: {
        const sxml_status status = start_terminate(machine, frame, outcome);
        frame->step = outcome->is_pushed ? SXML_XSLT_MESSAGE_STEP_COMPARE_TERMINATE
                                         : SXML_XSLT_MESSAGE_STEP_ERROR_CODE;
        return status;
    }
    case SXML_XSLT_MESSAGE_STEP_COMPARE_TERMINATE:
        frame->step = SXML_XSLT_MESSAGE_STEP_ERROR_CODE;
        return compare_terminate(machine->state, frame);
    case SXML_XSLT_MESSAGE_STEP_ERROR_CODE:
        frame->step = SXML_XSLT_MESSAGE_STEP_RESOLVE_CODE;
        return start_error_code(machine, frame, outcome);
    case SXML_XSLT_MESSAGE_STEP_RESOLVE_CODE:
        frame->step = SXML_XSLT_MESSAGE_STEP_FINISH;
        return locals->is_computed_code ? bind_computed_error_code(machine->state, frame)
                                        : SXML_STATUS_OK;
    default:
        outcome->is_pushed = true;
        return finish_message(machine, frame);
    }
}

static sxml_status continue_message(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame) {
    step_outcome outcome = {.is_pushed = false};
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && !outcome.is_pushed) {
        status = step_message(machine, frame, &outcome);
    }
    return status;
}

static const sxml_xpath_lowering_procedure message_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:message"),
    .step = continue_message,
    .locals_size = sizeof(message_locals)};

/* ===================================================== fork and evaluate */

/* xsl:fork (16.1): its xsl:sequence (or xsl:for-each-group) children in
   order, as a sequence constructor. */
typedef struct content_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
} content_locals;

static sxml_status continue_fork(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    content_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return start_content(machine, (value_request){.lowering = locals->head.lowering,
                                                      .node = locals->head.node,
                                                      .block = frame->block,
                                                      .atom = &locals->value});
    }
    return finish_with_atom(machine, frame, locals->value);
}

static const sxml_xpath_lowering_procedure fork_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:fork"),
    .step = continue_fork,
    .locals_size = sizeof(content_locals)};

/* xsl:evaluate (10.5): sxml has no dynamic evaluation (the feature is
   not claimed, D117), so the xsl:fallback children are its value, or it
   raises XTDE3175 (dynamic evaluation disabled) when evaluated. */
typedef struct evaluate_locals {
    sxml_xslt_instruction_locals head;
    sxml_xslt_ast_node_index next;
    sxml_ir_atom *atoms;
    size_t atom_count;
} evaluate_locals;

static sxml_status continue_evaluate(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    evaluate_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        const size_t fallback_count =
            count_children(lowering, (child_count_query){.node = locals->head.node,
                                                         .kind = SXML_XSLT_AST_KIND_FALLBACK});
        if (fallback_count == 0) {
            sxml_ir_expression_index raise = {0};
            const sxml_status status = add_raise(
                lowerer,
                (raise_request){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                .code = SXML_BYTE_VIEW_FROM_LITERAL("XTDE3175"),
                                .description = SXML_BYTE_VIEW_FROM_LITERAL(
                                    "xsl:evaluate is evaluated, and sxml does not support "
                                    "dynamic evaluation (XSLT 4.0, 10.5)")},
                &raise);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            return sxml_xpath_lowering_finish_frame(machine, frame, raise);
        }
        const sxml_status status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = fallback_count,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->atoms = frame->owned_memory[0];
        locals->next = node_of(lowering, locals->head.node)->first_child;
    }
    while (locals->next.value != 0) {
        const sxml_xslt_ast_node_index child = locals->next;
        locals->next = next_of(lowering, child);
        if (kind_of(lowering, child) != SXML_XSLT_AST_KIND_FALLBACK) {
            continue;
        }
        sxml_ir_atom *atom = &locals->atoms[locals->atom_count];
        locals->atom_count += 1;
        return start_content(
            machine, (value_request){
                         .lowering = lowering, .node = child, .block = frame->block, .atom = atom});
    }
    sxml_ir_expression sequence = {0};
    const sxml_status status = make_sequence(
        lowerer, (sxml_ir_atom_view){.atoms = locals->atoms, .atom_count = locals->atom_count},
        &sequence);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(machine, frame, sequence);
}

static const sxml_xpath_lowering_procedure evaluate_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:evaluate"),
    .step = continue_evaluate,
    .locals_size = sizeof(evaluate_locals)};

/* ============================================================ Iteration */

/*
 * xsl:iterate (7.2) runs as fn:scan(input, initial state, closure): the
 * evaluator's loop calls the closure once per item (D71). A state is an
 * array [control, output, parameter values...]: control 0 continues, 1
 * means a break happened; output is what the iteration gave.
 *
 * The closure fn($state, $item, $position) is lowered here with every
 * variable visible at the xsl:iterate captured (overcapture is harmless,
 * D126) and, when the body reads last(), the input's size as one more
 * capture. Its body:
 *
 *     if ($state?1) then array:put($state, 2, ())        (after a break)
 *     else let $p1 := $state?3, ... (the parameters in scope)
 *          let $r := BODY       (focus: $item, $position, the size)
 *          let $last := foot($r)
 *          return if ($last instance of map(*) and map:contains($last, K))
 *                 then let $a := $last?K
 *                      return array:put($a, 2, (trunk($r), $a?2))
 *                 else array:put($state, 2, $r)
 *
 * xsl:next-iteration and xsl:break stand in tail position (7.2), so the
 * marker they give is the last item of the body's value: a map whose one
 * key K is the xs:QName xsl:iterate (Q{http://www.w3.org/1999/XSL/Transform}
 * iterate), and whose value is the next state without output: [0, (), new
 * parameter values...] (parameters without xsl:with-param keep theirs),
 * or [1, the break's value]. The result of xsl:iterate is the outputs of
 * the states in order, then, when no break happened, xsl:on-completion's
 * value with the final parameter values in scope and the focus absent.
 */
typedef struct iterate_parameter {
    sxml_xslt_ast_node_index node;
    sxml_name_identifier name;
    sxml_sequence_type sequence_type;
    sxml_ir_atom initial_value;
    /* Its value inside the closure (a LOOKUP of the state). */
    sxml_ir_atom current_value;
} iterate_parameter;

/* The closure's slots: its three parameters, then the captures. */
#define ITERATE_STATE_SLOT 1U
#define ITERATE_ITEM_SLOT 2U
#define ITERATE_POSITION_SLOT 3U
#define ITERATE_PARAMETER_COUNT 3U

typedef struct iterate_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom input;
    /* One per xsl:param (owned memory 0). */
    iterate_parameter *parameters;
    size_t parameter_count;
    size_t parameter_index;
    sxml_ir_atom initial_state;
    /* The closure being lowered. */
    sxml_ir_function_index closure;
    sxml_xpath_lowering_function_frame function_frame;
    /* Captures (owned memory 1) and their atoms in the outer function
       (owned memory 2), with one more for the size. */
    sxml_ir_atom *outer_atoms;
    size_t captured_count;
    sxml_xpath_lowering_block closure_block;
    sxml_ir_atom control;
    sxml_xpath_lowering_block running_block;
    sxml_xpath_lowering_focus body_focus;
    sxml_ir_atom body;
    sxml_ir_atom states;
    sxml_ir_atom final_state;
    sxml_ir_atom outputs;
    sxml_xpath_lowering_block completion_block;
    sxml_ir_atom completion;
    bool has_completion;
    sxml_xpath_lowering_focus absent_focus;
    size_t scope_mark;
} iterate_locals;

enum {
    SXML_XSLT_ITERATE_STEP_START = 0,
    SXML_XSLT_ITERATE_STEP_PARAMETERS = 1,
    SXML_XSLT_ITERATE_STEP_BODY = 2,
    SXML_XSLT_ITERATE_STEP_COMPLETION = 3,
    SXML_XSLT_ITERATE_STEP_FINISH = 4,
};

static const sxml_xpath_lowering_procedure iterate_procedure;

/* K, the key of the iteration marker. */
static sxml_status add_marker_key(sxml_xpath_lowerer *lowerer, sxml_ir_atom *atom) {
    sxml_name_identifier name = {.index = 0};
    const sxml_status status = sxml_name_pool_intern_name(
        lowerer->name_pool,
        &(sxml_name_request){.namespace_uri =
                                 sxml_well_known_namespace_identifier(SXML_NAMESPACE_XSLT),
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("iterate")},
        &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_qname(lowerer, name, atom);
}

/* The atom of a visible variable in the current function, as lowering a
   reference to it would find it; NONE when it is not reachable. */
static sxml_ir_atom find_visible_atom(const sxml_xpath_lowerer *lowerer,
                                      const sxml_xpath_lowering_scope_entry *entry) {
    const sxml_xpath_lowering_function *function = lowerer->function;
    if (entry->function_level == function->function_level ||
        entry->atom.atom_kind == SXML_IR_ATOM_KIND_CONSTANT) {
        return entry->atom;
    }
    for (size_t capture_index = 0; capture_index < function->capture_count; capture_index += 1) {
        if (function->captures[capture_index].variable_name.index == entry->variable_name.index) {
            return function->captures[capture_index].atom;
        }
    }
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
}

/* The closure's captures: every variable in scope (innermost of each
   name) and every global variable declared, then the size. */
static sxml_status find_iterate_captures(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    iterate_locals *locals = frame->locals;
    const sxml_variable_declaration_view declarations = {
        .variable_declarations = locals->head.lowering->global_declarations,
        .variable_declaration_count = locals->head.lowering->global_count};
    const size_t capacity =
        lowerer->scope_entry_count + declarations.variable_declaration_count + 1U;
    sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 1,
            .scratch = {.element_count = capacity,
                        .element_size = sizeof(sxml_xpath_lowering_capture_entry),
                        .element_alignment = alignof(sxml_xpath_lowering_capture_entry)}});
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 2,
                .scratch = {.element_count = capacity,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xpath_lowering_capture_entry *captures = frame->owned_memory[1];
    locals->outer_atoms = frame->owned_memory[2];
    size_t capture_count = 0;
    size_t captured_count = 0;
    const size_t candidate_count =
        lowerer->scope_entry_count + declarations.variable_declaration_count;
    for (size_t candidate = 0; candidate < candidate_count; candidate += 1) {
        sxml_xpath_lowering_scope_entry entry = {0};
        if (candidate < lowerer->scope_entry_count) {
            entry = lowerer->scope_entries[lowerer->scope_entry_count - 1 - candidate];
        } else {
            const size_t declaration_index = candidate - lowerer->scope_entry_count;
            entry = (sxml_xpath_lowering_scope_entry){
                .variable_name =
                    declarations.variable_declarations[declaration_index].variable_name,
                .function_level = 0,
                .atom = {.atom_kind = SXML_IR_ATOM_KIND_GLOBAL_VARIABLE,
                         .global_variable = {.value = (uint32_t)declaration_index + 1U}}};
        }
        bool is_known = false;
        for (size_t capture_index = 0; capture_index < capture_count; capture_index += 1) {
            is_known = is_known ||
                       captures[capture_index].variable_name.index == entry.variable_name.index;
        }
        const sxml_ir_atom outer = find_visible_atom(lowerer, &entry);
        if (is_known || outer.atom_kind == SXML_IR_ATOM_KIND_NONE) {
            continue;
        }
        if (outer.atom_kind == SXML_IR_ATOM_KIND_CONSTANT) {
            captures[capture_count] = (sxml_xpath_lowering_capture_entry){
                .variable_name = entry.variable_name, .atom = outer};
        } else {
            captures[capture_count] = (sxml_xpath_lowering_capture_entry){
                .variable_name = entry.variable_name,
                .atom = sxml_ir_slot_atom((sxml_ir_slot){.value = ITERATE_PARAMETER_COUNT +
                                                          (uint32_t)captured_count + 1U})};
            locals->outer_atoms[captured_count] = outer;
            captured_count += 1;
        }
        capture_count += 1;
    }
    locals->captured_count = captured_count;
    locals->function_frame.function.captures = captures;
    locals->function_frame.function.capture_count = capture_count;
    return SXML_STATUS_OK;
}

/* The slot the closure keeps the input's size in: after the captures. */
static sxml_ir_slot size_capture_slot(const iterate_locals *locals) {
    return (sxml_ir_slot){.value = ITERATE_PARAMETER_COUNT + (uint32_t)locals->captured_count + 1U};
}

static sxml_status start_iterate(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    iterate_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    locals->parameter_count = count_children(
        lowering, (child_count_query){.node = locals->head.node, .kind = SXML_XSLT_AST_KIND_PARAM});
    if (locals->parameter_count > 0) {
        const sxml_status status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = locals->parameter_count,
                            .element_size = sizeof(iterate_parameter),
                            .element_alignment = alignof(iterate_parameter)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->parameters = frame->owned_memory[0];
        memset(locals->parameters, 0, locals->parameter_count * sizeof(iterate_parameter));
    }
    size_t parameter_index = 0;
    for (sxml_xslt_ast_node_index child = node_of(lowering, locals->head.node)->first_child;
         child.value != 0; child = next_of(lowering, child)) {
        if (kind_of(lowering, child) != SXML_XSLT_AST_KIND_PARAM) {
            continue;
        }
        iterate_parameter *parameter = &locals->parameters[parameter_index];
        parameter->node = child;
        parameter->name = binding_name_of(lowering, child);
        for (size_t earlier = 0; earlier < parameter_index; earlier += 1) {
            if (locals->parameters[earlier].name.index == parameter->name.index) {
                return report_static(
                    lowering, child,
                    SXML_BYTE_VIEW_FROM_LITERAL(
                        "XTSE0580 two parameters of xsl:iterate have the same name "
                        "(XSLT 4.0, 9.2)"));
            }
        }
        sxml_xslt_declared_type type = {.has_type = false};
        const sxml_status status = sxml_xslt_lowering_resolve_declared_type(lowering, child, &type);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        parameter->sequence_type = type.sequence_type;
        parameter_index += 1;
    }
    const sxml_xslt_ast_attribute *select =
        find_attribute(lowering, (attribute_lookup){.node = locals->head.node,
                                                    .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT});
    return sxml_xslt_lowering_call_expression(
        machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                              .expression = select->value.payload.expression,
                                              .block = frame->block,
                                              .atom = &locals->input,
                                              .focus = NULL});
}

/* [0, (), the initial values...], bound in the iterate's block. */
static sxml_status bind_initial_state(sxml_xpath_lowerer *lowerer,
                                      sxml_xpath_lowering_frame *frame) {
    iterate_locals *locals = frame->locals;
    void *memory = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = locals->parameter_count + 2U,
                                              .element_size = sizeof(sxml_ir_atom),
                                              .element_alignment = alignof(sxml_ir_atom)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_atom *members = memory;
    status = add_integer(lowerer, 0, &members[0]);
    if (status == SXML_STATUS_OK) {
        status = add_empty_atom(lowerer, &members[1]);
    }
    for (size_t parameter_index = 0; parameter_index < locals->parameter_count;
         parameter_index += 1) {
        members[parameter_index + 2U] = locals->parameters[parameter_index].initial_value;
    }
    if (status == SXML_STATUS_OK) {
        status =
            bind_array(lowerer,
                       (array_request){.block = frame->block,
                                       .members = {.atoms = members,
                                                   .atom_count = locals->parameter_count + 2U}},
                       &locals->initial_state);
    }
    sxml_xpath_lowerer_release_scratch(lowerer, memory);
    return status;
}

/* Puts each parameter in scope as state?(k + 2), bound in block. */
typedef struct parameter_unpacking {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom state;
} parameter_unpacking;

static sxml_status unpack_parameters(sxml_xpath_lowerer *lowerer, iterate_locals *locals,
                                     parameter_unpacking request) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < locals->parameter_count;
         parameter_index += 1) {
        iterate_parameter *parameter = &locals->parameters[parameter_index];
        status = bind_member(lowerer,
                             (member_request){.block = request.block,
                                              .array = request.state,
                                              .position = (int64_t)parameter_index + 3},
                             &parameter->current_value);
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_push_variable(
                lowerer, (sxml_xpath_lowering_scope_entry){.variable_name = parameter->name,
                                                           .function_level =
                                                               lowerer->function->function_level,
                                                           .atom = parameter->current_value});
        }
    }
    return status;
}

/* Adds the closure and enters it, then pushes the body. */
static sxml_status start_closure(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    iterate_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    locals->function_frame = (sxml_xpath_lowering_function_frame){0};
    sxml_status status = find_iterate_captures(machine, frame);
    const sxml_sequence_type parameter_types[ITERATE_PARAMETER_COUNT] = {
        {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
         .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
        {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
         .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
        {.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
         .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}};
    sxml_type_identifier signature = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_register_signature(
            lowerer,
            (sxml_xpath_lowering_signature_request){
                .parameter_types = {.sequence_types = parameter_types,
                                    .sequence_type_count = ITERATE_PARAMETER_COUNT},
                .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                                .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}},
            &signature);
    }
    const uint32_t slot_count = size_capture_slot(locals).value;
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            lowerer->unit,
            &(sxml_ir_function){.function_name = {.index = 0},
                                .signature = signature,
                                .parameter_count = ITERATE_PARAMETER_COUNT,
                                .captured_variables = {0},
                                .defining_function = lowerer->function->function_index,
                                .slot_count = slot_count,
                                .body = {.value = 0}},
            &locals->closure);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->function_frame.function.function_index = locals->closure;
    locals->function_frame.function.function_level = lowerer->function->function_level + 1U;
    locals->function_frame.function.slot_count = slot_count;
    locals->function_frame.focus =
        (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT};
    sxml_xpath_lowerer_enter_function(lowerer, &locals->function_frame);
    const sxml_ir_atom state = sxml_ir_slot_atom((sxml_ir_slot){.value = ITERATE_STATE_SLOT});
    locals->closure_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    locals->running_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    status = bind_member(
        lowerer, (member_request){.block = &locals->closure_block, .array = state, .position = 1},
        &locals->control);
    if (status == SXML_STATUS_OK) {
        status = unpack_parameters(
            lowerer, locals,
            (parameter_unpacking){.block = &locals->running_block, .state = state});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->body_focus =
        (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
                                    .slots = {.context_value = {.value = ITERATE_ITEM_SLOT},
                                              .context_position = {.value = ITERATE_POSITION_SLOT},
                                              .context_size = {.value = 0}}};
    return sxml_xslt_lowering_call_sequence_constructor(
        machine,
        &(sxml_xslt_sequence_constructor_call){
            .lowering = lowering,
            .first = sxml_xslt_lowering_first_constructor_child(lowering->ast, locals->head.node),
            .block = &locals->running_block,
            .is_body = false,
            .destination = {.expression = NULL,
                            .atom = &locals->body,
                            .atom_block = &locals->running_block,
                            .focused = NULL},
            .focus = &locals->body_focus});
}

/* In the running block: the next state from the body's value. */
static sxml_status bind_next_state(sxml_xpath_lowerer *lowerer, iterate_locals *locals,
                                   sxml_ir_atom *next_state) {
    sxml_xpath_lowering_block *block = &locals->running_block;
    const sxml_ir_atom state = sxml_ir_slot_atom((sxml_ir_slot){.value = ITERATE_STATE_SLOT});
    sxml_ir_atom key = {0};
    sxml_status status = add_marker_key(lowerer, &key);
    sxml_ir_atom last = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (call_request){.block = block,
                                          .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("foot"),
                                          .arguments = {.atoms = &locals->body, .atom_count = 1}},
                           &last);
    }
    sxml_ir_atom is_map = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(
            lowerer,
            (binding){
                .block = block,
                .expression =
                    {.expression_kind = SXML_IR_EXPRESSION_KIND_INSTANCE_OF,
                     .type_test = {.value = last,
                                   .sequence_type = {.item_type = sxml_builtin_type_identifier(
                                                         SXML_TYPE_MAP),
                                                     .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}}}},
            &is_map);
    }
    /* map:contains($last, K) only for a map. */
    sxml_xpath_lowering_block contains_block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom contains = {0};
    const sxml_ir_atom contains_arguments[2] = {last, key};
    if (status == SXML_STATUS_OK) {
        status =
            bind_call(lowerer,
                      (call_request){.block = &contains_block,
                                     .function_namespace = SXML_NAMESPACE_MAP_FUNCTIONS,
                                     .local_name = SXML_BYTE_VIEW_FROM_LITERAL("contains"),
                                     .arguments = {.atoms = contains_arguments, .atom_count = 2}},
                      &contains);
    }
    conditional_parts marker_test = {.condition = is_map};
    if (status == SXML_STATUS_OK) {
        status =
            finish_branch(lowerer, (branch_result){.block = &contains_block, .value = contains},
                          &marker_test.then_branch);
    }
    sxml_ir_atom false_atom = {0};
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_false(), &false_atom);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, atom_expression(false_atom), &marker_test.else_branch);
    }
    sxml_ir_atom is_marker = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = block, .expression = conditional_expression(marker_test)},
                      &is_marker);
    }
    /* With a marker: array:put($a, 2, (trunk($r), $a?2)). */
    sxml_xpath_lowering_block marker_block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom marked = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_lookup(
            lowerer, (lookup_request){.block = &marker_block, .value = last, .key = key}, &marked);
    }
    sxml_ir_atom trunk = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (call_request){.block = &marker_block,
                                          .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("trunk"),
                                          .arguments = {.atoms = &locals->body, .atom_count = 1}},
                           &trunk);
    }
    sxml_ir_atom extra = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_member(
            lowerer, (member_request){.block = &marker_block, .array = marked, .position = 2},
            &extra);
    }
    const sxml_ir_atom outputs[2] = {trunk, extra};
    sxml_ir_expression output_sequence = {0};
    sxml_ir_atom output = {0};
    if (status == SXML_STATUS_OK) {
        status = make_sequence(lowerer, (sxml_ir_atom_view){.atoms = outputs, .atom_count = 2},
                               &output_sequence);
    }
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer, (binding){.block = &marker_block, .expression = output_sequence},
                      &output);
    }
    sxml_ir_atom two = {0};
    if (status == SXML_STATUS_OK) {
        status = add_integer(lowerer, 2, &two);
    }
    const sxml_ir_atom marker_put[3] = {marked, two, output};
    sxml_ir_atom from_marker = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (call_request){.block = &marker_block,
                                          .function_namespace = SXML_NAMESPACE_ARRAY_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("put"),
                                          .arguments = {.atoms = marker_put, .atom_count = 3}},
                           &from_marker);
    }
    /* Without: array:put($state, 2, $r). */
    sxml_xpath_lowering_block plain_block = {.first = {0}, .open_let = {0}};
    const sxml_ir_atom plain_put[3] = {state, two, locals->body};
    sxml_ir_atom unchanged = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (call_request){.block = &plain_block,
                                          .function_namespace = SXML_NAMESPACE_ARRAY_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("put"),
                                          .arguments = {.atoms = plain_put, .atom_count = 3}},
                           &unchanged);
    }
    conditional_parts choice = {.condition = is_marker};
    if (status == SXML_STATUS_OK) {
        status =
            finish_branch(lowerer, (branch_result){.block = &marker_block, .value = from_marker},
                          &choice.then_branch);
    }
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, (branch_result){.block = &plain_block, .value = unchanged},
                               &choice.else_branch);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind(lowerer, (binding){.block = block, .expression = conditional_expression(choice)},
                next_state);
}

/* The closure's body, then the function item bound in the iterate's
   block. */
static sxml_status finish_closure(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    iterate_locals *locals = frame->locals;
    const sxml_ir_atom state = sxml_ir_slot_atom((sxml_ir_slot){.value = ITERATE_STATE_SLOT});
    sxml_ir_atom next_state = {0};
    sxml_status status = bind_next_state(lowerer, locals, &next_state);
    conditional_parts choice = {.condition = locals->control};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(
            lowerer, (branch_result){.block = &locals->running_block, .value = next_state},
            &choice.else_branch);
    }
    /* After a break: array:put($state, 2, ()). */
    sxml_xpath_lowering_block stopped_block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom arguments[3] = {state, {0}, {0}};
    if (status == SXML_STATUS_OK) {
        status = add_integer(lowerer, 2, &arguments[1]);
    }
    if (status == SXML_STATUS_OK) {
        status = add_empty_atom(lowerer, &arguments[2]);
    }
    sxml_ir_atom stopped = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (call_request){.block = &stopped_block,
                                          .function_namespace = SXML_NAMESPACE_ARRAY_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("put"),
                                          .arguments = {.atoms = arguments, .atom_count = 3}},
                           &stopped);
    }
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, (branch_result){.block = &stopped_block, .value = stopped},
                               &choice.then_branch);
    }
    sxml_ir_expression_index conditional = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_add_expression(lowerer, conditional_expression(choice), &conditional);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer,
            (sxml_xpath_lowering_block_result){.block = locals->closure_block,
                                               .result = conditional},
            &body);
    }
    const sxml_ir_slot read_size = locals->body_focus.slots.context_size;
    if (status == SXML_STATUS_OK && read_size.value != 0) {
        /* last(): the size, captured, copied to the slot the body reads. */
        sxml_ir_expression_index captured = {0};
        status = sxml_xslt_lowering_add_expression(lowerer, atom_expression(sxml_ir_slot_atom(size_capture_slot(locals))),
                                &captured);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_add_expression(
                lowerer,
                (sxml_ir_expression){
                    .expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                    .let = {.variable = read_size, .bound_expression = captured, .body = body}},
                &body);
        }
    }
    sxml_xpath_lowerer_leave_function(lowerer, &locals->function_frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The size in the outer function: count(input) when read, else (). */
    sxml_ir_atom *size = &locals->outer_atoms[locals->captured_count];
    if (read_size.value != 0) {
        status = bind_call(lowerer,
                           (call_request){.block = frame->block,
                                          .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("count"),
                                          .arguments = {.atoms = &locals->input, .atom_count = 1}},
                           size);
    } else {
        status = add_empty_atom(lowerer, size);
    }
    sxml_ir_atom_list captured = {0};
    if (status == SXML_STATUS_OK) {
        status =
            sxml_ir_unit_add_atoms(lowerer->unit,
                                   (sxml_ir_atom_view){.atoms = locals->outer_atoms,
                                                       .atom_count = locals->captured_count + 1U},
                                   &captured);
    }
    if (status == SXML_STATUS_OK) {
        sxml_ir_function_replacement replacement = {
            .index = locals->closure,
            .function = *sxml_ir_unit_get_function(lowerer->unit, locals->closure)};
        replacement.function.body = body;
        replacement.function.captured_variables = captured;
        replacement.function.slot_count = locals->function_frame.function.slot_count;
        status = sxml_ir_unit_replace_function(lowerer->unit, &replacement);
    }
    sxml_ir_atom closure = {0};
    if (status == SXML_STATUS_OK) {
        status = bind(
            lowerer,
            (binding){.block = frame->block,
                      .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION,
                                     .function_definition = {.function = locals->closure}}},
            &closure);
    }
    const sxml_ir_atom scan_arguments[3] = {locals->input, locals->initial_state, closure};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (call_request){.block = frame->block,
                                          .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("scan"),
                                          .arguments = {.atoms = scan_arguments, .atom_count = 3}},
                           &locals->states);
    }
    return status;
}

/* The outputs of the states, and the final state, bound in the iterate's
   block. */
static sxml_status bind_outputs(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_frame *frame) {
    iterate_locals *locals = frame->locals;
    sxml_ir_slot item = {0};
    sxml_status status = sxml_xpath_lowerer_add_slot(lowerer, &item);
    sxml_xpath_lowering_block map_block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom state = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_member(
            lowerer, (member_request){.block = &map_block, .array = sxml_ir_slot_atom(item), .position = 1},
            &state);
    }
    sxml_ir_atom output = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_member(
            lowerer, (member_request){.block = &map_block, .array = state, .position = 2}, &output);
    }
    sxml_ir_expression_index map_body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_branch(lowerer, (branch_result){.block = &map_block, .value = output},
                               &map_body);
    }
    if (status == SXML_STATUS_OK) {
        status = bind(lowerer,
                      (binding){.block = frame->block,
                                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                                               .map = {.input = locals->states,
                                                       .focus = {.context_value = item},
                                                       .body = map_body,
                                                       .flags = 0}}},
                      &locals->outputs);
    }
    sxml_ir_atom last = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (call_request){.block = frame->block,
                                          .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                          .local_name = SXML_BYTE_VIEW_FROM_LITERAL("foot"),
                                          .arguments = {.atoms = &locals->states, .atom_count = 1}},
                           &last);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_member(lowerer,
                             (member_request){.block = frame->block, .array = last, .position = 1},
                             &locals->final_state);
    }
    return status;
}

/* xsl:on-completion with the final parameter values in scope and the
   focus absent; outcome->is_pushed when its value is being lowered. */
static sxml_status start_completion(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame, step_outcome *outcome) {
    sxml_xpath_lowerer *lowerer = machine->state;
    iterate_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    outcome->is_pushed = false;
    sxml_xslt_ast_node_index completion = {.value = 0};
    for (sxml_xslt_ast_node_index child = node_of(lowering, locals->head.node)->first_child;
         child.value != 0; child = next_of(lowering, child)) {
        if (kind_of(lowering, child) == SXML_XSLT_AST_KIND_ON_COMPLETION) {
            completion = child;
        }
    }
    if (completion.value == 0) {
        return SXML_STATUS_OK;
    }
    locals->has_completion = true;
    locals->completion_block = (sxml_xpath_lowering_block){.first = {0}, .open_let = {0}};
    locals->scope_mark = lowerer->scope_entry_count;
    sxml_status status = unpack_parameters(
        lowerer, locals,
        (parameter_unpacking){.block = &locals->completion_block, .state = locals->final_state});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->absent_focus =
        (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT};
    value_source source = {.has_value = false};
    status =
        start_value(machine,
                    (value_request){.lowering = lowering,
                                    .node = completion,
                                    .block = &locals->completion_block,
                                    .atom = &locals->completion,
                                    .options = {.conflict_error =
                                                    "XTSE3125 xsl:on-completion has both a select "
                                                    "attribute and content (XSLT 4.0, 7.2)",
                                                .focus = &locals->absent_focus}},
                    &source);
    outcome->is_pushed = source.has_value;
    if (status == SXML_STATUS_OK && !source.has_value) {
        status = add_empty_atom(lowerer, &locals->completion);
    }
    return status;
}

static sxml_status finish_iterate(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    iterate_locals *locals = frame->locals;
    sxml_ir_atom results[2] = {locals->outputs, {0}};
    size_t result_count = 1;
    sxml_status status = SXML_STATUS_OK;
    if (locals->has_completion) {
        sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_mark);
        sxml_ir_atom control = {0};
        status = bind_member(
            lowerer,
            (member_request){.block = frame->block, .array = locals->final_state, .position = 1},
            &control);
        conditional_parts parts = {.condition = control};
        sxml_xpath_lowering_block stopped_block = {.first = {0}, .open_let = {0}};
        sxml_ir_atom empty = {0};
        if (status == SXML_STATUS_OK) {
            status = add_empty_atom(lowerer, &empty);
        }
        if (status == SXML_STATUS_OK) {
            status =
                finish_branch(lowerer, (branch_result){.block = &stopped_block, .value = empty},
                              &parts.then_branch);
        }
        if (status == SXML_STATUS_OK) {
            status = finish_branch(
                lowerer,
                (branch_result){.block = &locals->completion_block, .value = locals->completion},
                &parts.else_branch);
        }
        if (status == SXML_STATUS_OK) {
            status =
                bind(lowerer,
                     (binding){.block = frame->block, .expression = conditional_expression(parts)},
                     &results[1]);
            result_count = 2;
        }
    }
    sxml_ir_expression result = {0};
    if (status == SXML_STATUS_OK) {
        status = make_sequence(
            lowerer, (sxml_ir_atom_view){.atoms = results, .atom_count = result_count}, &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(machine, frame, result);
}

static sxml_status continue_iterate(sxml_xpath_lowering_machine *machine,
                                    sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    iterate_locals *locals = frame->locals;
    sxml_status status = SXML_STATUS_OK;
    switch (frame->step) {
    case SXML_XSLT_ITERATE_STEP_START:
        frame->step = SXML_XSLT_ITERATE_STEP_PARAMETERS;
        return start_iterate(machine, frame);
    case SXML_XSLT_ITERATE_STEP_PARAMETERS:
        if (locals->parameter_index < locals->parameter_count) {
            /* 7.2: initial values in the focus of the select expression. */
            iterate_parameter *parameter = &locals->parameters[locals->parameter_index];
            locals->parameter_index += 1;
            return call_binding_value(
                machine,
                (binding_value_call){.lowering = locals->head.lowering,
                                     .node = parameter->node,
                                     .role = SXML_XSLT_BINDING_ROLE_PARAMETER,
                                     .required_type = NULL},
                (binding_destination){.block = frame->block, .atom = &parameter->initial_value});
        }
        frame->step = SXML_XSLT_ITERATE_STEP_BODY;
        status = bind_initial_state(lowerer, frame);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return start_closure(machine, frame);
    case SXML_XSLT_ITERATE_STEP_BODY: {
        frame->step = SXML_XSLT_ITERATE_STEP_COMPLETION;
        status = finish_closure(machine, frame);
        if (status == SXML_STATUS_OK) {
            status = bind_outputs(lowerer, frame);
        }
        step_outcome outcome = {.is_pushed = false};
        if (status == SXML_STATUS_OK) {
            status = start_completion(machine, frame, &outcome);
        }
        if (status != SXML_STATUS_OK || outcome.is_pushed) {
            return status;
        }
        return finish_iterate(machine, frame);
    }
    default:
        return finish_iterate(machine, frame);
    }
}

static const sxml_xpath_lowering_procedure iterate_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:iterate"),
    .step = continue_iterate,
    .locals_size = sizeof(iterate_locals)};

/* 7.2: whether node is in a tail position of the body of the innermost
   xsl:iterate around it: the last instruction (xsl:fallback aside, and
   xsl:catch after the body of an xsl:try) of a sequence constructor that
   is the iterate's, or the body of an xsl:if, xsl:when, xsl:otherwise,
   xsl:try or xsl:catch that is itself in a tail position. */
static bool is_in_tail_position(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    sxml_xslt_ast_node_index current = node;
    for (;;) {
        const sxml_xslt_ast_node_index parent = node_of(lowering, current)->parent;
        if (parent.value == 0) {
            return false;
        }
        const sxml_xslt_ast_kind parent_kind = kind_of(lowering, parent);
        for (sxml_xslt_ast_node_index sibling = next_of(lowering, current); sibling.value != 0;
             sibling = next_of(lowering, sibling)) {
            const sxml_xslt_ast_kind kind = kind_of(lowering, sibling);
            if (kind != SXML_XSLT_AST_KIND_FALLBACK &&
                !(kind == SXML_XSLT_AST_KIND_CATCH && parent_kind == SXML_XSLT_AST_KIND_TRY)) {
                return false;
            }
        }
        switch ((uint32_t)parent_kind) {
        case SXML_XSLT_AST_KIND_ITERATE:
            return true;
        case SXML_XSLT_AST_KIND_IF:
        case SXML_XSLT_AST_KIND_TRY:
            current = parent;
            break;
        case SXML_XSLT_AST_KIND_CATCH:
        case SXML_XSLT_AST_KIND_WHEN:
        case SXML_XSLT_AST_KIND_OTHERWISE:
            /* The xsl:try, xsl:choose or xsl:switch around it. */
            current = node_of(lowering, parent)->parent;
            break;
        default:
            return false;
        }
    }
}

/* The frame of the innermost xsl:iterate below frame; NULL outside one
   (XTSE3120). */
static iterate_locals *find_iterate(sxml_xpath_lowering_frame *frame) {
    for (sxml_xpath_lowering_frame *below = frame->below; below != NULL; below = below->below) {
        if (below->procedure == &iterate_procedure) {
            return below->locals;
        }
    }
    return NULL;
}

/*
 * xsl:next-iteration (7.2): the marker {K: [0, (), values...]}, each
 * parameter's value from its xsl:with-param (coerced to its own "as" and
 * to the parameter's type, XTTE0590) or the current one.
 * xsl:break: {K: [1, the break's value]}.
 */
typedef struct next_iteration_locals {
    sxml_xslt_instruction_locals head;
    iterate_locals *iterate;
    /* parameter_count + 2 members (owned memory 0). */
    sxml_ir_atom *members;
    sxml_xslt_ast_node_index next_child;
    sxml_ir_atom break_value;
} next_iteration_locals;

/* The marker of members, as the frame's value. */
typedef struct marker_request {
    sxml_xpath_lowering_frame *frame;
    sxml_ir_atom_view members;
} marker_request;

static sxml_status finish_with_marker(sxml_xpath_lowering_machine *machine,
                                      marker_request request) {
    sxml_xpath_lowerer *lowerer = machine->state;
    sxml_ir_atom array = {0};
    sxml_status status = bind_array(
        lowerer, (array_request){.block = request.frame->block, .members = request.members},
        &array);
    sxml_ir_map_entry entry = {.key = {0}, .value = array};
    if (status == SXML_STATUS_OK) {
        status = add_marker_key(lowerer, &entry.key);
    }
    sxml_ir_map_entry_list entries = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_map_entries(
            lowerer->unit, (sxml_ir_map_entry_view){.map_entries = &entry, .map_entry_count = 1},
            &entries);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_with_expression(
        machine, request.frame,
        (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                             .map_constructor = {.entries = entries}});
}

static sxml_status start_next_iteration(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    next_iteration_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    locals->iterate = find_iterate(frame);
    if (locals->iterate == NULL || !is_in_tail_position(lowering, locals->head.node)) {
        return report_static(
            lowering, locals->head.node,
            SXML_BYTE_VIEW_FROM_LITERAL("XTSE3120 xsl:next-iteration or xsl:break is not in a tail "
                                        "position of an xsl:iterate (XSLT 4.0, 7.2)"));
    }
    const size_t parameter_count = locals->iterate->parameter_count;
    sxml_status status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 0,
            .scratch = {.element_count = parameter_count + 2U,
                        .element_size = sizeof(sxml_ir_atom),
                        .element_alignment = alignof(sxml_ir_atom)}});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->members = frame->owned_memory[0];
    const bool is_break = kind_of(lowering, locals->head.node) == SXML_XSLT_AST_KIND_BREAK;
    status = add_integer(lowerer, is_break ? 1 : 0, &locals->members[0]);
    if (status == SXML_STATUS_OK) {
        status = add_empty_atom(lowerer, &locals->members[1]);
    }
    for (size_t parameter_index = 0; parameter_index < parameter_count; parameter_index += 1) {
        locals->members[parameter_index + 2U] =
            locals->iterate->parameters[parameter_index].current_value;
    }
    /* 7.2: XTSE3130 for a name that is no parameter, XTSE0670 for two
       with one name. */
    for (sxml_xslt_ast_node_index child = node_of(lowering, locals->head.node)->first_child;
         status == SXML_STATUS_OK && child.value != 0; child = next_of(lowering, child)) {
        if (kind_of(lowering, child) != SXML_XSLT_AST_KIND_WITH_PARAM) {
            continue;
        }
        const sxml_name_identifier name = binding_name_of(lowering, child);
        bool is_parameter = false;
        for (size_t parameter_index = 0; parameter_index < parameter_count; parameter_index += 1) {
            is_parameter = is_parameter ||
                           locals->iterate->parameters[parameter_index].name.index == name.index;
        }
        if (!is_parameter) {
            return report_static(
                lowering, child,
                SXML_BYTE_VIEW_FROM_LITERAL(
                    "XTSE3130 xsl:with-param names no parameter of the xsl:iterate "
                    "(XSLT 4.0, 7.2)"));
        }
        for (sxml_xslt_ast_node_index earlier = node_of(lowering, locals->head.node)->first_child;
             earlier.value != child.value; earlier = next_of(lowering, earlier)) {
            if (kind_of(lowering, earlier) == SXML_XSLT_AST_KIND_WITH_PARAM &&
                binding_name_of(lowering, earlier).index == name.index) {
                return report_static(lowering, child,
                                     SXML_BYTE_VIEW_FROM_LITERAL(
                                         "XTSE0670 two xsl:with-param elements have the same name "
                                         "(XSLT 4.0, 9.10)"));
            }
        }
    }
    locals->next_child = node_of(lowering, locals->head.node)->first_child;
    return status;
}

static sxml_status continue_next_iteration(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame) {
    next_iteration_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_status status = start_next_iteration(machine, frame);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    while (locals->next_child.value != 0) {
        const sxml_xslt_ast_node_index child = locals->next_child;
        locals->next_child = next_of(lowering, child);
        if (kind_of(lowering, child) != SXML_XSLT_AST_KIND_WITH_PARAM) {
            continue;
        }
        const sxml_name_identifier name = binding_name_of(lowering, child);
        for (size_t parameter_index = 0; parameter_index < locals->iterate->parameter_count;
             parameter_index += 1) {
            iterate_parameter *parameter = &locals->iterate->parameters[parameter_index];
            if (parameter->name.index != name.index) {
                continue;
            }
            return call_binding_value(
                machine,
                (binding_value_call){.lowering = lowering,
                                     .node = child,
                                     .role = SXML_XSLT_BINDING_ROLE_PARAMETER,
                                     .required_type = &parameter->sequence_type},
                (binding_destination){.block = frame->block,
                                      .atom = &locals->members[parameter_index + 2U]});
        }
    }
    return finish_with_marker(
        machine,
        (marker_request){.frame = frame,
                         .members = {.atoms = locals->members,
                                     .atom_count = locals->iterate->parameter_count + 2U}});
}

static const sxml_xpath_lowering_procedure next_iteration_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:next-iteration"),
    .step = continue_next_iteration,
    .locals_size = sizeof(next_iteration_locals)};

static sxml_status continue_break(sxml_xpath_lowering_machine *machine,
                                  sxml_xpath_lowering_frame *frame) {
    next_iteration_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        sxml_status status = start_next_iteration(machine, frame);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        value_source source = {.has_value = false};
        status = start_value(
            machine,
            (value_request){.lowering = locals->head.lowering,
                            .node = locals->head.node,
                            .block = frame->block,
                            .atom = &locals->members[1],
                            .options = {.conflict_error =
                                            "XTSE3125 xsl:break has both a select attribute "
                                            "and content (XSLT 4.0, 7.2)",
                                        .focus = NULL}},
            &source);
        if (status != SXML_STATUS_OK || source.has_value) {
            return status;
        }
    }
    return finish_with_marker(
        machine,
        (marker_request){.frame = frame, .members = {.atoms = locals->members, .atom_count = 2}});
}

static const sxml_xpath_lowering_procedure break_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:break"),
    .step = continue_break,
    .locals_size = sizeof(next_iteration_locals)};

/* ===================================================== Global variables */

/*
 * Global variables and parameters (9.5): one global of the unit per name,
 * the declaration of highest import precedence (XTSE0630 for two of that
 * precedence), declared to every expression's static context so a
 * reference is a GLOBAL_VARIABLE atom; then the context global (name 0)
 * that holds the global context item (D135). Each initializer is an IR
 * function of arity 0 whose focus is the context global, lowered by the
 * variable-binding procedure; a required xsl:param raises XTDE0050 (no
 * value is supplied yet, packet 4.15). The evaluator computes the globals
 * before the main function in the order of their initializer functions'
 * indices (src/evaluator_global.c), so a function that calls each
 * initializer is added per global in dependency order (references in the
 * expressions of its declaration); a cycle is XTDE0640 (9.11).
 */

/* A global variable's name in the package that declares it (3.4.5:
   packages have globals of their own). */
typedef struct global_key {
    sxml_name_identifier name;
    uint32_t package;
} global_key;

/* The global a name declares in a package; global_count when none. */
static size_t find_global(const sxml_xslt_lowering *lowering, global_key key) {
    for (size_t global_index = 0; global_index < lowering->global_count; global_index += 1) {
        if (lowering->global_declarations[global_index].variable_name.index == key.name.index &&
            sxml_xslt_package_of_node(lowering, lowering->global_nodes[global_index]) ==
                key.package) {
            return global_index;
        }
    }
    return lowering->global_count;
}

/* The global a reference to name reads in an AST static context: the
   one its XPath static context declares (per package, below);
   global_count when none. */
typedef struct global_reference {
    sxml_xslt_ast_static_context_index static_context;
    sxml_name_identifier name;
} global_reference;

static size_t find_referenced_global(const sxml_xslt_lowering *lowering,
                                     global_reference reference) {
    if (reference.static_context.value == 0 ||
        reference.static_context.value >= lowering->static_context_count) {
        return lowering->global_count;
    }
    const sxml_xpath_static_context *context =
        &lowering->static_contexts[reference.static_context.value];
    const sxml_variable_declaration_view declarations = context->variable_declarations;
    for (size_t index = 0; index < declarations.variable_declaration_count; index += 1) {
        if (declarations.variable_declarations[index].variable_name.index ==
            reference.name.index) {
            const uint32_t number = sxml_xpath_global_number(context, index);
            return number >= 1U && number <= lowering->global_count ? (size_t)number - 1U
                                                                    : lowering->global_count;
        }
    }
    return lowering->global_count;
}

static bool is_global_declaration(const sxml_xslt_lowering *lowering,
                                  sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_kind kind = kind_of(lowering, node);
    return kind == SXML_XSLT_AST_KIND_VARIABLE || kind == SXML_XSLT_AST_KIND_PARAM;
}

/* An xsl:variable or xsl:param that overrides one of a used package. */
static bool has_overriding_variable(const sxml_xslt_lowering *lowering) {
    for (size_t global_index = 0; global_index < lowering->global_count; global_index += 1) {
        if (sxml_xslt_package_overriding_ancestor_of(lowering, lowering->global_nodes[global_index])
                .value != 0) {
            return true;
        }
    }
    return false;
}

/* The globals one package sees (3.4.5.5): one per variable name it binds,
   each to the global of the declaration it binds to, plus $xsl:original
   in an overriding variable (3.4.5.4). */
typedef struct package_view {
    uint32_t package;
    /* 0, or the overriding variable whose xsl:original is added. */
    sxml_xslt_ast_node_index overriding;
} package_view;

static sxml_status make_package_view(const sxml_xslt_lowering *lowering, package_view view,
                                     sxml_xpath_static_context *static_context) {
    const size_t capacity = lowering->global_count + 1U;
    void *memory = NULL;
    sxml_status status = sxml_xslt_package_allocate(
        lowering,
        (sxml_xslt_package_memory_request){.element_count = capacity,
                                           .element_size = sizeof(sxml_variable_declaration),
                                           .element_alignment = alignof(sxml_variable_declaration)},
        &memory);
    sxml_variable_declaration *declarations = memory;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_package_allocate(
            lowering,
            (sxml_xslt_package_memory_request){.element_count = capacity,
                                               .element_size = sizeof(uint32_t),
                                               .element_alignment = alignof(uint32_t)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *numbers = memory;
    size_t count = 0;
    for (size_t global_index = 0; global_index < lowering->global_count; global_index += 1) {
        const sxml_name_identifier name = lowering->global_declarations[global_index].variable_name;
        bool is_seen = false;
        for (size_t earlier = 0; earlier < count && !is_seen; earlier += 1) {
            is_seen = declarations[earlier].variable_name.index == name.index;
        }
        if (is_seen) {
            continue;
        }
        const sxml_xslt_component_binding bound = sxml_xslt_package_bind(
            lowering, &(sxml_xslt_component_reference){
                          .component_kind = SXML_XSLT_COMPONENT_KIND_VARIABLE,
                          .component_name = name,
                          .arity = 0,
                          .package = view.package});
        for (size_t target = 0; target < lowering->global_count; target += 1) {
            if (bound.declaration.value != 0 &&
                lowering->global_nodes[target].value == bound.declaration.value) {
                declarations[count] = lowering->global_declarations[target];
                numbers[count] = (uint32_t)target + 1U;
                count += 1;
                break;
            }
        }
    }
    if (view.overriding.value != 0) {
        const sxml_xslt_component_binding original =
            sxml_xslt_package_bind_original(lowering, view.overriding);
        sxml_name_identifier original_name = {.index = 0};
        status = sxml_name_pool_intern_name(
            lowering->lowerer->name_pool,
            &(sxml_name_request){.namespace_uri =
                                     sxml_well_known_namespace_identifier(SXML_NAMESPACE_XSLT),
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("original")},
            &original_name);
        for (size_t target = 0; status == SXML_STATUS_OK && target < lowering->global_count;
             target += 1) {
            /* XTSE3075 for $xsl:original of an abstract variable: left
               undeclared, XPST0008 (3.4.5.4). */
            if (original.declaration.value != 0 && !original.is_abstract &&
                lowering->global_nodes[target].value == original.declaration.value) {
                declarations[count] = lowering->global_declarations[target];
                declarations[count].variable_name = original_name;
                numbers[count] = (uint32_t)target + 1U;
                count += 1;
                break;
            }
        }
    }
    static_context->variable_declarations = (sxml_variable_declaration_view){
        .variable_declarations = declarations, .variable_declaration_count = count};
    static_context->global_numbers = numbers;
    return status;
}

static sxml_status declare_package_views(const sxml_xslt_lowering *lowering,
                                         sxml_xpath_static_context *static_contexts) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t context_index = 1;
         status == SXML_STATUS_OK && context_index < lowering->static_context_count;
         context_index += 1) {
        const sxml_xslt_ast_static_context_index context = {.value = context_index};
        const sxml_xslt_ast_node_index overriding =
            sxml_xslt_package_overriding_declaration_of(lowering, context);
        const sxml_xslt_ast_kind overriding_kind = node_of(lowering, overriding)->kind;
        const bool is_variable = overriding_kind == SXML_XSLT_AST_KIND_VARIABLE ||
                                 overriding_kind == SXML_XSLT_AST_KIND_PARAM;
        const uint32_t package = sxml_xslt_package_of_static_context(lowering, context);
        /* Contexts of one package without xsl:original share a view: the
           first one made. */
        bool is_shared = false;
        for (uint32_t earlier = 1; !is_variable && earlier < context_index && !is_shared;
             earlier += 1) {
            const sxml_xslt_ast_static_context_index other = {.value = earlier};
            const sxml_xslt_ast_kind other_kind =
                node_of(lowering, sxml_xslt_package_overriding_declaration_of(lowering, other))
                    ->kind;
            if (sxml_xslt_package_of_static_context(lowering, other) == package &&
                other_kind != SXML_XSLT_AST_KIND_VARIABLE &&
                other_kind != SXML_XSLT_AST_KIND_PARAM) {
                static_contexts[context_index].variable_declarations =
                    static_contexts[earlier].variable_declarations;
                static_contexts[context_index].global_numbers =
                    static_contexts[earlier].global_numbers;
                is_shared = true;
            }
        }
        if (!is_shared) {
            status = make_package_view(
                lowering,
                (package_view){.package = package,
                               .overriding = is_variable ? overriding
                                                         : (sxml_xslt_ast_node_index){.value = 0}},
                &static_contexts[context_index]);
        }
    }
    return status;
}

sxml_status
sxml_xslt_lowering_declare_global_variables(sxml_xslt_lowering *lowering,
                                            sxml_xpath_static_context *static_contexts) {
    const sxml_xslt_ast *ast = lowering->ast;
    size_t capacity = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        capacity +=
            is_global_declaration(lowering, sxml_xslt_ast_get_declaration(ast, order)) ? 1U : 0U;
    }
    if (capacity == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_xpath_lowerer *lowerer = lowering->lowerer;
    void *memory = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = capacity,
                                              .element_size = sizeof(sxml_variable_declaration),
                                              .element_alignment =
                                                  alignof(sxml_variable_declaration)},
        &memory);
    lowering->global_declarations = memory;
    /* A failed allocation leaves memory as it was. */
    memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_allocate_scratch(
            lowerer,
            (sxml_xpath_lowering_scratch_request){.element_count = capacity,
                                                  .element_size = sizeof(sxml_xslt_ast_node_index),
                                                  .element_alignment =
                                                      alignof(sxml_xslt_ast_node_index)},
            &memory);
        lowering->global_nodes = memory;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (!is_global_declaration(lowering, node)) {
            continue;
        }
        const sxml_name_identifier name = binding_name_of(lowering, node);
        const uint32_t precedence =
            sxml_xslt_ast_get_module(ast, node_of(lowering, node)->module)->import_precedence;
        const uint32_t package = sxml_xslt_package_of_node(lowering, node);
        const size_t existing = find_global(lowering, (global_key){.name = name, .package = package});
        if (existing == lowering->global_count) {
            lowering->global_declarations[existing] = (sxml_variable_declaration){
                .variable_name = name,
                .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                                  .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
            lowering->global_nodes[existing] = node;
            lowering->global_count += 1;
            continue;
        }
        const sxml_xslt_ast_node_index other = lowering->global_nodes[existing];
        const uint32_t other_precedence =
            sxml_xslt_ast_get_module(ast, node_of(lowering, other)->module)->import_precedence;
        if (precedence > other_precedence) {
            lowering->global_nodes[existing] = node;
        } else if (precedence == other_precedence) {
            /* 9.5: unless one of higher precedence exists, which an
               earlier or later declaration of the list would be. */
            bool has_higher = false;
            for (uint32_t later = 1; sxml_xslt_ast_get_declaration(ast, later).value != 0;
                 later += 1) {
                const sxml_xslt_ast_node_index candidate =
                    sxml_xslt_ast_get_declaration(ast, later);
                has_higher = has_higher ||
                             (is_global_declaration(lowering, candidate) &&
                              binding_name_of(lowering, candidate).index == name.index &&
                              sxml_xslt_package_of_node(lowering, candidate) == package &&
                              sxml_xslt_ast_get_module(ast, node_of(lowering, candidate)->module)
                                      ->import_precedence > precedence);
            }
            if (!has_higher) {
                return report_static(
                    lowering, node,
                    SXML_BYTE_VIEW_FROM_LITERAL(
                        "XTSE0630 two global variables or parameters of one import "
                        "precedence have the same name (XSLT 4.0, 9.5)"));
            }
        }
    }
    if (sxml_xslt_package_count(lowering) > 1U || has_overriding_variable(lowering)) {
        return declare_package_views(lowering, static_contexts);
    }
    const sxml_variable_declaration_view declarations = {
        .variable_declarations = lowering->global_declarations,
        .variable_declaration_count = lowering->global_count};
    for (uint32_t context_index = 1; context_index < lowering->static_context_count;
         context_index += 1) {
        static_contexts[context_index].variable_declarations = declarations;
    }
    return SXML_STATUS_OK;
}

void sxml_xslt_lowering_release_global_variables(sxml_xslt_lowering *lowering) {
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, lowering->global_declarations);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, lowering->global_nodes);
    lowering->global_declarations = NULL;
    lowering->global_nodes = NULL;
    lowering->global_count = 0;
}

/* The dependencies of the globals: global i reads global targets[k] for
   k from first_edges[i] to first_edges[i + 1]. */
typedef struct dependency_graph {
    size_t *first_edges;
    size_t *targets;
    size_t edge_count;
} dependency_graph;

/* The global whose declaration holds node; global_count for none. */
static size_t owning_global(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    sxml_xslt_ast_node_index current = node;
    while (current.value != 0 &&
           (node_of(lowering, current)->flags & SXML_XSLT_AST_NODE_FLAG_DECLARATION) == 0) {
        current = node_of(lowering, current)->parent;
    }
    for (size_t global_index = 0; global_index < lowering->global_count; global_index += 1) {
        if (current.value != 0 && lowering->global_nodes[global_index].value == current.value) {
            return global_index;
        }
    }
    return lowering->global_count;
}

/* Calls visit for each global an expression's variable references name
   (counting when targets is NULL). */
typedef struct reference_walk {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_expression_index expression;
} reference_walk;

typedef struct edge_sink {
    size_t *targets;
    size_t edge_count;
} edge_sink;

static sxml_status walk_references(reference_walk walk, edge_sink *sink) {
    const sxml_xslt_lowering *lowering = walk.lowering;
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, walk.expression);
    const sxml_ast *ast = expression->ast;
    if (ast == NULL || expression->static_context.value == 0 ||
        expression->static_context.value >= lowering->static_context_count) {
        return SXML_STATUS_OK;
    }
    sxml_name_pool *name_pool = lowering->lowerer->name_pool;
    for (uint32_t node_number = 1; node_number < ast->node_count; node_number += 1) {
        const sxml_ast_node_index node = {.value = node_number};
        if (sxml_ast_get_node(ast, node)->kind != SXML_AST_KIND_VARIABLE_REFERENCE) {
            continue;
        }
        sxml_xpath_resolved_name resolved = {.namespace_uri = {.index = 0},
                                             .local_name = {.bytes = NULL, .byte_count = 0}};
        const sxml_status status = sxml_xpath_resolve_name(
            &(sxml_xpath_name_request){
                .ast = ast,
                .name_node = sxml_ast_find_child(
                    ast, (sxml_ast_child_query){.parent = node, .field = SXML_AST_FIELD_NAME}),
                .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                .static_context = &lowering->static_contexts[expression->static_context.value],
                .error_record = NULL},
            &resolved);
        if (status != SXML_STATUS_OK) {
            /* Lowering the reference reports it. */
            continue;
        }
        sxml_name_identifier name = {.index = 0};
        if (!sxml_name_pool_find_name(name_pool,
                                      &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                                                           .local_name = resolved.local_name},
                                      &name)) {
            continue;
        }
        const size_t target = find_referenced_global(
            lowering, (global_reference){.static_context = expression->static_context, .name = name});
        if (target == lowering->global_count) {
            continue;
        }
        if (sink->targets != NULL) {
            sink->targets[sink->edge_count] = target;
        }
        sink->edge_count += 1;
    }
    return SXML_STATUS_OK;
}

/* Fills the graph in two passes over the AST's expressions: counts, then
   edges, grouped by owning global. */
static sxml_status build_dependency_graph(const sxml_xslt_lowering *lowering,
                                          dependency_graph *graph) {
    const sxml_xpath_lowerer *lowerer = lowering->lowerer;
    const size_t global_count = lowering->global_count;
    void *memory = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = global_count + 1U,
                                              .element_size = sizeof(size_t),
                                              .element_alignment = alignof(size_t)},
        &memory);
    graph->first_edges = memory;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(graph->first_edges, 0, (global_count + 1U) * sizeof(size_t));
    const uint32_t expression_count = lowering->ast->expression_count;
    /* Pass 1: the edges of each global, counted in first_edges[i + 1]. */
    for (uint32_t expression = 1; status == SXML_STATUS_OK && expression < expression_count;
         expression += 1) {
        const sxml_xslt_ast_expression_index index = {.value = expression};
        const size_t owner =
            owning_global(lowering, sxml_xslt_ast_get_expression(lowering->ast, index)->node);
        if (owner == global_count) {
            continue;
        }
        edge_sink counter = {.targets = NULL, .edge_count = 0};
        status =
            walk_references((reference_walk){.lowering = lowering, .expression = index}, &counter);
        graph->first_edges[owner + 1U] += counter.edge_count;
    }
    for (size_t global_index = 0; global_index < global_count; global_index += 1) {
        graph->first_edges[global_index + 1U] += graph->first_edges[global_index];
    }
    graph->edge_count = graph->first_edges[global_count];
    if (status != SXML_STATUS_OK || graph->edge_count == 0) {
        return status;
    }
    status = sxml_xpath_lowerer_allocate_scratch(
        lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = graph->edge_count,
                                              .element_size = sizeof(size_t),
                                              .element_alignment = alignof(size_t)},
        &memory);
    graph->targets = memory;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Pass 2: written at each owner's next free edge (first_edges[i]
       moves to the end of its run, then is moved back). */
    for (uint32_t expression = 1; status == SXML_STATUS_OK && expression < expression_count;
         expression += 1) {
        const sxml_xslt_ast_expression_index index = {.value = expression};
        const size_t owner =
            owning_global(lowering, sxml_xslt_ast_get_expression(lowering->ast, index)->node);
        if (owner == global_count) {
            continue;
        }
        edge_sink sink = {.targets = graph->targets + graph->first_edges[owner], .edge_count = 0};
        status =
            walk_references((reference_walk){.lowering = lowering, .expression = index}, &sink);
        graph->first_edges[owner] += sink.edge_count;
    }
    for (size_t global_number = global_count; global_number > 0; global_number -= 1) {
        graph->first_edges[global_number] = graph->first_edges[global_number - 1U];
    }
    graph->first_edges[0] = 0;
    return status;
}

/* The globals in dependency order (each after those it reads), by a
   depth-first walk on an explicit stack (D71); XTDE0640 for a cycle. */
typedef enum walk_color {
    SXML_XSLT_WALK_COLOR_NEW = 0,
    SXML_XSLT_WALK_COLOR_OPEN = 1,
    SXML_XSLT_WALK_COLOR_DONE = 2,
} walk_color;

typedef struct walk_frame {
    size_t global_index;
    size_t next_edge;
} walk_frame;

typedef struct order_request {
    const sxml_xslt_lowering *lowering;
    const dependency_graph *graph;
} order_request;

static sxml_status order_globals(order_request request, size_t *order) {
    const sxml_xslt_lowering *lowering = request.lowering;
    const dependency_graph *graph = request.graph;
    const size_t global_count = lowering->global_count;
    void *colors_memory = NULL;
    void *stack_memory = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        lowering->lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = global_count,
                                              .element_size = sizeof(uint8_t),
                                              .element_alignment = alignof(uint8_t)},
        &colors_memory);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_allocate_scratch(
            lowering->lowerer,
            (sxml_xpath_lowering_scratch_request){.element_count = global_count,
                                                  .element_size = sizeof(walk_frame),
                                                  .element_alignment = alignof(walk_frame)},
            &stack_memory);
    }
    uint8_t *colors = colors_memory;
    walk_frame *stack = stack_memory;
    if (status == SXML_STATUS_OK) {
        memset(colors, 0, global_count);
    }
    size_t order_count = 0;
    for (size_t root_index = 0; status == SXML_STATUS_OK && root_index < global_count;
         root_index += 1) {
        if (colors[root_index] != SXML_XSLT_WALK_COLOR_NEW) {
            continue;
        }
        size_t depth = 1;
        stack[0] =
            (walk_frame){.global_index = root_index, .next_edge = graph->first_edges[root_index]};
        colors[root_index] = SXML_XSLT_WALK_COLOR_OPEN;
        while (status == SXML_STATUS_OK && depth > 0) {
            walk_frame *top = &stack[depth - 1U];
            if (graph->targets == NULL ||
                top->next_edge == graph->first_edges[top->global_index + 1U]) {
                colors[top->global_index] = SXML_XSLT_WALK_COLOR_DONE;
                order[order_count] = top->global_index;
                order_count += 1;
                depth -= 1;
                continue;
            }
            const size_t target = graph->targets[top->next_edge];
            top->next_edge += 1;
            if (target == top->global_index) {
                /* 9.9: a global variable is not in scope in its own
                   declaration (nor is one it overrides). */
                status = report_static(lowering, lowering->global_nodes[target],
                                       SXML_BYTE_VIEW_FROM_LITERAL(
                                           "XPST0008 a global variable refers to itself (XSLT 4.0, "
                                           "9.9)"));
            } else if (colors[target] == SXML_XSLT_WALK_COLOR_OPEN) {
                status =
                    report_static(lowering, lowering->global_nodes[target],
                                  SXML_BYTE_VIEW_FROM_LITERAL(
                                      "XTDE0640 a global variable depends on itself (XSLT 4.0, "
                                      "9.11)"));
            } else if (colors[target] == SXML_XSLT_WALK_COLOR_NEW) {
                colors[target] = SXML_XSLT_WALK_COLOR_OPEN;
                stack[depth] =
                    (walk_frame){.global_index = target, .next_edge = graph->first_edges[target]};
                depth += 1;
            }
        }
    }
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, colors_memory);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, stack_memory);
    return status;
}

/* The initializer of one global: an IR function of arity 0. */
typedef struct initializer_request {
    const sxml_xslt_lowering *lowering;
    size_t global_index;
} initializer_request;

static sxml_status lower_initializer(initializer_request request,
                                     sxml_ir_function_index *initializer) {
    const sxml_xslt_lowering *lowering = request.lowering;
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    const sxml_xslt_ast_node_index node = lowering->global_nodes[request.global_index];
    sxml_type_identifier signature = {0};
    sxml_status status = sxml_xpath_lowerer_register_signature(
        lowerer,
        (sxml_xpath_lowering_signature_request){
            .parameter_types = {.sequence_types = NULL, .sequence_type_count = 0},
            .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}},
        &signature);
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            lowerer->unit, &(sxml_ir_function){.signature = signature}, initializer);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xpath_lowering_function_frame function_frame = {
        .function = {.function_index = *initializer,
                     .function_level = 0,
                     .slot_count = 0,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {
            .kind = SXML_XPATH_LOWERING_FOCUS_KIND_FIXED,
            .fixed_value = {.atom_kind = SXML_IR_ATOM_KIND_GLOBAL_VARIABLE,
                            .global_variable = {.value = (uint32_t)lowering->global_count + 1U}}}};
    if (sxml_xslt_package_of_node(lowering, node) != 1U) {
        /* 2.3.2: a library package's globals have no global context item
           (XPDY0002 when they read one). */
        function_frame.focus =
            (sxml_xpath_lowering_focus){.kind = SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT};
    }
    sxml_xpath_lowerer_enter_function(lowerer, &function_frame);
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom value = {0};
    const bool is_parameter = kind_of(lowering, node) == SXML_XSLT_AST_KIND_PARAM;
    const sxml_xslt_ast_attribute *required = find_attribute(
        lowering, (attribute_lookup){.node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_REQUIRED});
    sxml_ir_expression_index result = {0};
    if (sxml_xslt_package_is_abstract(lowering, node)) {
        status = add_raise(lowerer,
                           (raise_request){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                           .code = SXML_BYTE_VIEW_FROM_LITERAL("XTDE3052"),
                                           .description = SXML_BYTE_VIEW_FROM_LITERAL(
                                               "an abstract variable no package overrides is "
                                               "read (XSLT 4.0, 3.4.5.2)")},
                           &result);
    } else if (is_parameter && required->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
        required->value.payload.is_true) {
        status = add_raise(lowerer,
                           (raise_request){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                           .code = SXML_BYTE_VIEW_FROM_LITERAL("XTDE0050"),
                                           .description = SXML_BYTE_VIEW_FROM_LITERAL(
                                               "no value is supplied for a required stylesheet "
                                               "parameter (XSLT 4.0, 9.5)")},
                           &result);
    } else {
        const binding_value_locals initial = {.lowering = lowering,
                                              .node = node,
                                              .role = is_parameter
                                                          ? SXML_XSLT_BINDING_ROLE_PARAMETER
                                                          : SXML_XSLT_BINDING_ROLE_VARIABLE};
        status = sxml_xpath_lowerer_run_procedure(
            lowerer, &(sxml_xpath_lowering_procedure_call){
                         .procedure = &binding_value_procedure,
                         .node = no_ast_node,
                         .block = &block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &value,
                                         .atom_block = &block,
                                         .focused = NULL},
                         .initial_locals = {.bytes = (const uint8_t *)&initial,
                                            .byte_count = sizeof(initial)}});
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_add_expression(lowerer, atom_expression(value), &result);
        }
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = result}, &body);
    }
    sxml_xpath_lowerer_leave_function(lowerer, &function_frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = *initializer, .function = *sxml_ir_unit_get_function(lowerer->unit, *initializer)};
    replacement.function.body = body;
    replacement.function.slot_count = function_frame.function.slot_count;
    return sxml_ir_unit_replace_function(lowerer->unit, &replacement);
}

/* A function of arity 0 that calls initializer: added in computation
   order (golden: src/xquery_prolog.c, add_ordered_initializer). */
static sxml_status add_ordered_initializer(sxml_xpath_lowerer *lowerer,
                                           sxml_ir_function_index initializer,
                                           sxml_ir_function_index *ordered) {
    const sxml_ir_function *lowered = sxml_ir_unit_get_function(lowerer->unit, initializer);
    const sxml_type_identifier signature = lowered->signature;
    sxml_ir_expression_index call = {0};
    const sxml_status status = sxml_xslt_lowering_add_expression(
        lowerer,
        (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
            .static_call = {.target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                       .function = initializer},
                            .arguments = {.first_atom_index = 0, .atom_count = 0}}},
        &call);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_ir_unit_add_function(
        lowerer->unit, &(sxml_ir_function){.signature = signature, .body = call}, ordered);
}

/* ------------------------------------- Dependencies through components */

/*
 * A global variable's initializer may read other globals through the
 * components it calls: stylesheet functions, named templates, template
 * rules (apply-templates), attribute sets (packet 4.12, D150). The order
 * of computation therefore follows a graph whose nodes are the
 * declarations (one more node stands for "every template rule") and
 * whose edges are the references in each declaration's expressions and
 * instructions: globals read, functions called or referenced by name (by
 * name: every arity), templates called, rules applied, attribute sets
 * used. Its strongly connected components (Tarjan's algorithm on an
 * explicit stack, D71) come out dependencies first, and the globals in
 * them in that order are the computation order. Only the direct cycles
 * between globals are static errors (above); a cycle through a component
 * may never happen, so it is left to the evaluator (BEING_COMPUTED).
 */

/* The graph over declarations: node n is declaration n + 1, node
   declaration_count is every template rule. */
typedef struct component_graph {
    size_t node_count;
    /* Edges from node n are targets[first_edges[n] .. first_edges[n + 1]). */
    size_t *first_edges;
    size_t *targets;
    /* Collected edges (sources and targets) before they are grouped. */
    size_t *edge_sources;
    size_t *edge_targets;
    size_t edge_count;
    size_t edge_capacity;
    /* For each AST node, the declaration order of the declaration it is
       (0 for a node that is none). */
    uint32_t *declaration_orders;
} component_graph;

static void release_component_graph(const sxml_xslt_lowering *lowering, component_graph *graph) {
    const sxml_xpath_lowerer *lowerer = lowering->lowerer;
    sxml_xpath_lowerer_release_scratch(lowerer, graph->first_edges);
    sxml_xpath_lowerer_release_scratch(lowerer, graph->targets);
    sxml_xpath_lowerer_release_scratch(lowerer, graph->edge_sources);
    sxml_xpath_lowerer_release_scratch(lowerer, graph->edge_targets);
    sxml_xpath_lowerer_release_scratch(lowerer, graph->declaration_orders);
}

static sxml_status allocate_sizes(const sxml_xslt_lowering *lowering, size_t count, size_t **sizes) {
    void *memory = NULL;
    const sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        lowering->lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = count == 0 ? 1 : count,
                                              .element_size = sizeof(size_t),
                                              .element_alignment = alignof(size_t)},
        &memory);
    *sizes = memory;
    return status;
}

/* One edge, from the declaration holding node to target. */
typedef struct component_edge {
    size_t source;
    size_t target;
} component_edge;

static sxml_status add_component_edge(const sxml_xslt_lowering *lowering, component_graph *graph,
                                      component_edge edge) {
    if (graph->edge_count == graph->edge_capacity) {
        const size_t capacity = graph->edge_capacity == 0 ? 64 : graph->edge_capacity * 2;
        size_t *sources = NULL;
        size_t *targets = NULL;
        sxml_status status = allocate_sizes(lowering, capacity, &sources);
        if (status == SXML_STATUS_OK) {
            status = allocate_sizes(lowering, capacity, &targets);
        }
        if (status != SXML_STATUS_OK) {
            sxml_xpath_lowerer_release_scratch(lowering->lowerer, sources);
            return status;
        }
        if (graph->edge_count > 0) {
            memcpy(sources, graph->edge_sources, graph->edge_count * sizeof(size_t));
            memcpy(targets, graph->edge_targets, graph->edge_count * sizeof(size_t));
        }
        sxml_xpath_lowerer_release_scratch(lowering->lowerer, graph->edge_sources);
        sxml_xpath_lowerer_release_scratch(lowering->lowerer, graph->edge_targets);
        graph->edge_sources = sources;
        graph->edge_targets = targets;
        graph->edge_capacity = capacity;
    }
    graph->edge_sources[graph->edge_count] = edge.source;
    graph->edge_targets[graph->edge_count] = edge.target;
    graph->edge_count += 1;
    return SXML_STATUS_OK;
}

/* The graph node of the declaration holding node; node_count for none. */
static size_t holding_declaration(const sxml_xslt_lowering *lowering, const component_graph *graph,
                                  sxml_xslt_ast_node_index node) {
    sxml_xslt_ast_node_index current = node;
    while (current.value != 0 &&
           (node_of(lowering, current)->flags & SXML_XSLT_AST_NODE_FLAG_DECLARATION) == 0) {
        current = node_of(lowering, current)->parent;
    }
    if (current.value == 0 || current.value >= lowering->ast->node_count ||
        graph->declaration_orders[current.value] == 0) {
        return graph->node_count;
    }
    return graph->declaration_orders[current.value] - 1U;
}

/* Edges from source to every declaration of kind named name (every
   template rule when kind is TEMPLATE and the name is 0). */
typedef struct named_target {
    size_t source;
    sxml_xslt_ast_kind kind;
    sxml_name_identifier name;
} named_target;

static sxml_status add_named_edges(const sxml_xslt_lowering *lowering, component_graph *graph,
                                   named_target target) {
    const sxml_xslt_ast *ast = lowering->ast;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, declaration)->kind != target.kind) {
            continue;
        }
        const bool matches =
            target.name.index == 0
                ? find_attribute(lowering, (attribute_lookup){.node = declaration, .kind = SXML_XSLT_AST_ATTRIBUTE_MATCH})->kind !=
                      SXML_XSLT_AST_ATTRIBUTE_NONE
                : binding_name_of(lowering, declaration).index == target.name.index;
        if (matches) {
            status = add_component_edge(lowering, graph,
                                        (component_edge){.source = target.source, .target = order - 1U});
        }
    }
    return status;
}

/* A name node of an XPath AST that names a function (a call, a named
   function reference, an arrow's function). */
static bool is_function_name(const sxml_ast *ast, const sxml_ast_node *node) {
    if (node->kind != SXML_AST_KIND_NCNAME && node->kind != SXML_AST_KIND_PREFIXED_NAME &&
        node->kind != SXML_AST_KIND_URI_QUALIFIED_NAME) {
        return false;
    }
    if (node->field != SXML_AST_FIELD_NAME && node->field != SXML_AST_FIELD_FUNCTION) {
        return false;
    }
    const sxml_ast_kind parent_kind = sxml_ast_get_node(ast, node->parent)->kind;
    return parent_kind == SXML_AST_KIND_FUNCTION_CALL ||
           parent_kind == SXML_AST_KIND_NAMED_FUNCTION_REFERENCE ||
           parent_kind == SXML_AST_KIND_ARROW_EXPRESSION;
}

/* The edges of one expression: globals read and functions named. */
static sxml_status add_expression_edges(const sxml_xslt_lowering *lowering, component_graph *graph,
                                        sxml_xslt_ast_expression_index index) {
    const sxml_xslt_ast_expression *expression = sxml_xslt_ast_get_expression(lowering->ast, index);
    const sxml_ast *ast = expression->ast;
    const size_t source = holding_declaration(lowering, graph, expression->node);
    if (ast == NULL || source == graph->node_count || expression->static_context.value == 0 ||
        expression->static_context.value >= lowering->static_context_count) {
        return SXML_STATUS_OK;
    }
    const sxml_xpath_static_context *context =
        &lowering->static_contexts[expression->static_context.value];
    sxml_name_pool *name_pool = lowering->lowerer->name_pool;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t node_number = 1; status == SXML_STATUS_OK && node_number < ast->node_count;
         node_number += 1) {
        const sxml_ast_node_index node = {.value = node_number};
        const sxml_ast_node *ast_node = sxml_ast_get_node(ast, node);
        const bool is_reference = ast_node->kind == SXML_AST_KIND_VARIABLE_REFERENCE;
        if (!is_reference && !is_function_name(ast, ast_node)) {
            continue;
        }
        sxml_xpath_resolved_name resolved = {.namespace_uri = {.index = 0},
                                             .local_name = {.bytes = NULL, .byte_count = 0}};
        if (sxml_xpath_resolve_name(
                &(sxml_xpath_name_request){
                    .ast = ast,
                    .name_node = is_reference
                                     ? sxml_ast_find_child(ast, (sxml_ast_child_query){
                                                                    .parent = node,
                                                                    .field = SXML_AST_FIELD_NAME})
                                     : node,
                    .name_role = is_reference ? SXML_XPATH_NAME_ROLE_NO_NAMESPACE
                                              : SXML_XPATH_NAME_ROLE_FUNCTION,
                    .static_context = context,
                    .error_record = NULL},
                &resolved) != SXML_STATUS_OK) {
            continue;
        }
        sxml_name_identifier name = {.index = 0};
        if (!sxml_name_pool_find_name(name_pool,
                                      &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                                                           .local_name = resolved.local_name},
                                      &name)) {
            continue;
        }
        if (!is_reference) {
            status = add_named_edges(lowering, graph,
                                     (named_target){.source = source,
                                                    .kind = SXML_XSLT_AST_KIND_FUNCTION,
                                                    .name = name});
            continue;
        }
        const size_t global_index = find_referenced_global(
            lowering, (global_reference){.static_context = expression->static_context, .name = name});
        if (global_index < lowering->global_count) {
            const uint32_t order =
                graph->declaration_orders[lowering->global_nodes[global_index].value];
            status = add_component_edge(
                lowering, graph, (component_edge){.source = source, .target = order - 1U});
        }
    }
    return status;
}

/* The edges of the stylesheet's instructions: templates called, rules
   applied, attribute sets used; and of the rules node to every rule. */
static sxml_status add_instruction_edges(const sxml_xslt_lowering *lowering,
                                         component_graph *graph) {
    const size_t rules_node = graph->node_count - 1U;
    sxml_status status = add_named_edges(
        lowering, graph,
        (named_target){.source = rules_node, .kind = SXML_XSLT_AST_KIND_TEMPLATE, .name = {0}});
    for (uint32_t node_number = 1;
         status == SXML_STATUS_OK && node_number < lowering->ast->node_count; node_number += 1) {
        const sxml_xslt_ast_node_index node = {.value = node_number};
        const sxml_xslt_ast_kind kind = node_of(lowering, node)->kind;
        const bool calls = kind == SXML_XSLT_AST_KIND_CALL_TEMPLATE;
        const bool applies = kind == SXML_XSLT_AST_KIND_APPLY_TEMPLATES ||
                             kind == SXML_XSLT_AST_KIND_NEXT_MATCH ||
                             kind == SXML_XSLT_AST_KIND_APPLY_IMPORTS;
        const sxml_xslt_ast_attribute *sets =
            find_attribute(lowering, (attribute_lookup){.node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS});
        const bool uses_sets = sets->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME_LIST;
        if (!calls && !applies && !uses_sets) {
            continue;
        }
        const size_t source = holding_declaration(lowering, graph, node);
        if (source == graph->node_count) {
            continue;
        }
        if (calls) {
            status = add_named_edges(lowering, graph,
                                     (named_target){.source = source,
                                                    .kind = SXML_XSLT_AST_KIND_TEMPLATE,
                                                    .name = binding_name_of(lowering, node)});
        } else if (applies) {
            status = add_component_edge(lowering, graph,
                                        (component_edge){.source = source, .target = rules_node});
        }
        const sxml_xslt_ast_name_token_view tokens =
            uses_sets ? sxml_xslt_ast_get_name_tokens(lowering->ast, sets->value.payload.name_tokens)
                      : (sxml_xslt_ast_name_token_view){.name_tokens = NULL, .name_token_count = 0};
        for (size_t token_index = 0; status == SXML_STATUS_OK && token_index < tokens.name_token_count;
             token_index += 1) {
            status = add_named_edges(lowering, graph,
                                     (named_target){.source = source,
                                                    .kind = SXML_XSLT_AST_KIND_ATTRIBUTE_SET,
                                                    .name = tokens.name_tokens[token_index].name});
        }
    }
    return status;
}

/* Groups the collected edges by source (a counting sort). */
static sxml_status group_component_edges(const sxml_xslt_lowering *lowering,
                                         component_graph *graph) {
    sxml_status status = allocate_sizes(lowering, graph->node_count + 1U, &graph->first_edges);
    if (status == SXML_STATUS_OK) {
        status = allocate_sizes(lowering, graph->edge_count, &graph->targets);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(graph->first_edges, 0, (graph->node_count + 1U) * sizeof(size_t));
    for (size_t edge_index = 0; edge_index < graph->edge_count; edge_index += 1) {
        graph->first_edges[graph->edge_sources[edge_index] + 1U] += 1;
    }
    for (size_t node_index = 0; node_index < graph->node_count; node_index += 1) {
        graph->first_edges[node_index + 1U] += graph->first_edges[node_index];
    }
    for (size_t edge_index = 0; edge_index < graph->edge_count; edge_index += 1) {
        const size_t source = graph->edge_sources[edge_index];
        graph->targets[graph->first_edges[source]] = graph->edge_targets[edge_index];
        graph->first_edges[source] += 1;
    }
    for (size_t node_index = graph->node_count; node_index > 0; node_index -= 1) {
        graph->first_edges[node_index] = graph->first_edges[node_index - 1U];
    }
    graph->first_edges[0] = 0;
    return SXML_STATUS_OK;
}

static sxml_status build_component_graph(const sxml_xslt_lowering *lowering,
                                         component_graph *graph) {
    const sxml_xslt_ast *ast = lowering->ast;
    size_t declaration_count = 0;
    while (sxml_xslt_ast_get_declaration(ast, (uint32_t)declaration_count + 1U).value != 0) {
        declaration_count += 1;
    }
    graph->node_count = declaration_count + 1U;
    void *memory = NULL;
    sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        lowering->lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = ast->node_count,
                                              .element_size = sizeof(uint32_t),
                                              .element_alignment = alignof(uint32_t)},
        &memory);
    graph->declaration_orders = memory;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(graph->declaration_orders, 0, ast->node_count * sizeof(uint32_t));
    for (uint32_t order = 1; order <= declaration_count; order += 1) {
        graph->declaration_orders[sxml_xslt_ast_get_declaration(ast, order).value] = order;
    }
    for (uint32_t expression = 1; status == SXML_STATUS_OK && expression < ast->expression_count;
         expression += 1) {
        status = add_expression_edges(lowering, graph,
                                      (sxml_xslt_ast_expression_index){.value = expression});
    }
    if (status == SXML_STATUS_OK) {
        status = add_instruction_edges(lowering, graph);
    }
    if (status == SXML_STATUS_OK) {
        status = group_component_edges(lowering, graph);
    }
    return status;
}

/* One frame of the depth-first walk: a node and its next edge. */
typedef struct component_frame {
    size_t node;
    size_t next_edge;
} component_frame;

/* The walk's arrays, one entry per node (SIZE_MAX: not visited). */
typedef struct component_walk {
    const component_graph *graph;
    /* The globals in order, dependencies first. */
    size_t *order;
    size_t order_count;
    size_t *visit_numbers;
    size_t *low_links;
    size_t *component_stack;
    size_t component_stack_count;
    component_frame *frames;
    size_t frame_count;
    size_t visit_count;
    /* The global of each node; global_count for a node that is none. */
    size_t *globals;
} component_walk;

static void visit_component(component_walk *walk, const component_graph *graph, size_t node) {
    walk->visit_numbers[node] = walk->visit_count;
    walk->low_links[node] = walk->visit_count;
    walk->visit_count += 1;
    walk->component_stack[walk->component_stack_count] = node;
    walk->component_stack_count += 1;
    walk->frames[walk->frame_count] =
        (component_frame){.node = node, .next_edge = graph->first_edges[node]};
    walk->frame_count += 1;
}

/* Tarjan's algorithm from root; each finished component's globals are
   appended to walk->order. A node is on the component stack while it is
   visited and not yet assigned to a component (assigned nodes get
   COMPONENT_ASSIGNED as low link). */
#define COMPONENT_ASSIGNED (SIZE_MAX - 1U)

static void walk_components(component_walk *walk, size_t root) {
    const component_graph *graph = walk->graph;
    visit_component(walk, graph, root);
    while (walk->frame_count > 0) {
        component_frame *top = &walk->frames[walk->frame_count - 1U];
        const size_t node = top->node;
        if (top->next_edge < graph->first_edges[node + 1U]) {
            const size_t target = graph->targets[top->next_edge];
            top->next_edge += 1;
            if (walk->visit_numbers[target] == SIZE_MAX) {
                visit_component(walk, graph, target);
            } else if (walk->low_links[target] != COMPONENT_ASSIGNED &&
                       walk->visit_numbers[target] < walk->low_links[node]) {
                walk->low_links[node] = walk->visit_numbers[target];
            }
            continue;
        }
        walk->frame_count -= 1;
        if (walk->low_links[node] == walk->visit_numbers[node]) {
            size_t member = SIZE_MAX;
            while (member != node) {
                walk->component_stack_count -= 1;
                member = walk->component_stack[walk->component_stack_count];
                walk->low_links[member] = COMPONENT_ASSIGNED;
                if (walk->globals[member] != SIZE_MAX) {
                    walk->order[walk->order_count] = walk->globals[member];
                    walk->order_count += 1;
                }
            }
        }
        if (walk->frame_count > 0) {
            /* An assigned node's low link is COMPONENT_ASSIGNED, larger
               than any visit number: it passes nothing up. */
            const size_t parent = walk->frames[walk->frame_count - 1U].node;
            if (walk->low_links[node] < walk->low_links[parent]) {
                walk->low_links[parent] = walk->low_links[node];
            }
        }
    }
}

/* Rewrites order (global indices, dependencies first) by the component
   graph. */
static sxml_status order_through_components(const sxml_xslt_lowering *lowering, size_t *order) {
    component_graph graph = {.node_count = 0,
                             .first_edges = NULL,
                             .targets = NULL,
                             .edge_sources = NULL,
                             .edge_targets = NULL,
                             .edge_count = 0,
                             .edge_capacity = 0,
                             .declaration_orders = NULL};
    sxml_status status = build_component_graph(lowering, &graph);
    size_t *arrays = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_sizes(lowering, graph.node_count * 4U, &arrays);
    }
    void *frame_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_allocate_scratch(
            lowering->lowerer,
            (sxml_xpath_lowering_scratch_request){.element_count = graph.node_count,
                                                  .element_size = sizeof(component_frame),
                                                  .element_alignment = alignof(component_frame)},
            &frame_memory);
    }
    if (status == SXML_STATUS_OK) {
        component_walk walk = {.graph = &graph,
                               .order = order,
                               .order_count = 0,
                               .visit_numbers = arrays,
                               .low_links = arrays + graph.node_count,
                               .component_stack = arrays + 2U * graph.node_count,
                               .component_stack_count = 0,
                               .frames = frame_memory,
                               .frame_count = 0,
                               .visit_count = 0,
                               .globals = arrays + 3U * graph.node_count};
        for (size_t node_index = 0; node_index < graph.node_count; node_index += 1) {
            walk.visit_numbers[node_index] = SIZE_MAX;
            walk.low_links[node_index] = SIZE_MAX;
            walk.globals[node_index] = SIZE_MAX;
        }
        for (size_t global_index = 0; global_index < lowering->global_count; global_index += 1) {
            walk.globals[graph.declaration_orders[lowering->global_nodes[global_index].value] -
                         1U] = global_index;
        }
        for (size_t global_index = 0; global_index < lowering->global_count; global_index += 1) {
            const size_t node =
                graph.declaration_orders[lowering->global_nodes[global_index].value] - 1U;
            if (walk.visit_numbers[node] == SIZE_MAX) {
                walk_components(&walk, node);
            }
        }
    }
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, frame_memory);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, arrays);
    release_component_graph(lowering, &graph);
    return status;
}

/* A global some static context binds a reference to (3.4.5.5): the
   others (an overridden or abstract variable no reference reaches) get no
   initializer and are never computed. Every global with one package. */
static bool is_bound_global(const sxml_xslt_lowering *lowering, size_t global_index) {
    for (uint32_t context_index = 1; context_index < lowering->static_context_count;
         context_index += 1) {
        const sxml_xpath_static_context *context = &lowering->static_contexts[context_index];
        if (context->global_numbers == NULL) {
            return true;
        }
        for (size_t index = 0; index < context->variable_declarations.variable_declaration_count;
             index += 1) {
            if (context->global_numbers[index] == global_index + 1U) {
                return true;
            }
        }
    }
    return lowering->static_context_count < 2U;
}

sxml_status sxml_xslt_lowering_add_global_variables(const sxml_xslt_lowering *lowering) {
    const size_t global_count = lowering->global_count;
    if (global_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    dependency_graph graph = {.first_edges = NULL, .targets = NULL, .edge_count = 0};
    sxml_status status = build_dependency_graph(lowering, &graph);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_allocate_scratch(
            lowerer,
            (sxml_xpath_lowering_scratch_request){.element_count = global_count * 3U,
                                                  .element_size = sizeof(size_t),
                                                  .element_alignment = alignof(size_t)},
            &memory);
    }
    size_t *order = memory;
    if (status == SXML_STATUS_OK) {
        status = order_globals((order_request){.lowering = lowering, .graph = &graph}, order);
    }
    if (status == SXML_STATUS_OK) {
        /* Also the globals the components they call read (packet 4.12). */
        status = order_through_components(lowering, order);
    }
    /* initializers[i] and ordered[i] after the order (as indices); none
       when the memory could not be had. */
    size_t *initializers = order == NULL ? NULL : order + global_count;
    size_t *ordered = initializers == NULL ? NULL : initializers + global_count;
    for (size_t global_index = 0; status == SXML_STATUS_OK && global_index < global_count;
         global_index += 1) {
        sxml_ir_function_index initializer = {0};
        status = lower_initializer(
            (initializer_request){.lowering = lowering, .global_index = global_index},
            &initializer);
        initializers[global_index] = initializer.value;
    }
    for (size_t order_index = 0; status == SXML_STATUS_OK && order_index < global_count;
         order_index += 1) {
        const size_t global_index = order[order_index];
        sxml_ir_function_index wrapper = {0};
        if (!is_bound_global(lowering, global_index)) {
            ordered[global_index] = 0;
            continue;
        }
        status = add_ordered_initializer(
            lowerer, (sxml_ir_function_index){.value = (uint32_t)initializers[global_index]},
            &wrapper);
        ordered[global_index] = wrapper.value;
    }
    for (size_t global_index = 0; status == SXML_STATUS_OK && global_index < global_count;
         global_index += 1) {
        sxml_ir_global_index global = {0};
        status = sxml_ir_unit_add_global_variable(
            lowerer->unit,
            &(sxml_ir_global_variable){
                .variable_name = lowering->global_declarations[global_index].variable_name,
                .declared_type = lowering->global_declarations[global_index].declared_type,
                .initializer = {.value = (uint32_t)ordered[global_index]}},
            &global);
    }
    if (status == SXML_STATUS_OK) {
        /* The context global (D135): the host's context value. */
        sxml_ir_global_index global = {0};
        status = sxml_ir_unit_add_global_variable(
            lowerer->unit,
            &(sxml_ir_global_variable){
                .variable_name = {.index = 0},
                .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                                  .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
                .initializer = {.value = 0}},
            &global);
    }
    sxml_xpath_lowerer_release_scratch(lowerer, memory);
    sxml_xpath_lowerer_release_scratch(lowerer, graph.first_edges);
    sxml_xpath_lowerer_release_scratch(lowerer, graph.targets);
    return status;
}

/* ================================================================ Rules */

static const sxml_xslt_instruction_rule control_rules[] = {
    {.kind = SXML_XSLT_AST_KIND_IF, .procedure = &if_procedure},
    {.kind = SXML_XSLT_AST_KIND_CHOOSE, .procedure = &choose_procedure},
    {.kind = SXML_XSLT_AST_KIND_SWITCH, .procedure = &choose_procedure},
    {.kind = SXML_XSLT_AST_KIND_FOR_EACH, .procedure = &for_each_procedure},
    {.kind = SXML_XSLT_AST_KIND_ITERATE, .procedure = &iterate_procedure},
    {.kind = SXML_XSLT_AST_KIND_NEXT_ITERATION, .procedure = &next_iteration_procedure},
    {.kind = SXML_XSLT_AST_KIND_BREAK, .procedure = &break_procedure},
    {.kind = SXML_XSLT_AST_KIND_VARIABLE, .procedure = &variable_procedure},
    {.kind = SXML_XSLT_AST_KIND_TRY, .procedure = &try_procedure},
    {.kind = SXML_XSLT_AST_KIND_CATCH, .procedure = &nothing_procedure},
    {.kind = SXML_XSLT_AST_KIND_WHERE_POPULATED, .procedure = &where_populated_procedure},
    {.kind = SXML_XSLT_AST_KIND_MESSAGE, .procedure = &message_procedure},
    {.kind = SXML_XSLT_AST_KIND_ASSERT, .procedure = &message_procedure},
    {.kind = SXML_XSLT_AST_KIND_FORK, .procedure = &fork_procedure},
    {.kind = SXML_XSLT_AST_KIND_EVALUATE, .procedure = &evaluate_procedure},
};

const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_control = {
    .rules = control_rules, .rule_count = sizeof(control_rules) / sizeof(control_rules[0])};
