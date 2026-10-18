/*
 * XSLT 4.0 templates and modes (src/xslt_lower_internal.h; XSLT 4.0,
 * sections 6, 9.2 and 10.1; docs/DECISIONS.md D55, D118, D144; packet
 * 4.8).
 *
 * Every xsl:template becomes one template function of the unit: five
 * parameters, the focus (item, position, size: its focus slots 1 to 3)
 * and the maps of the non-tunnel and the tunnel parameters supplied
 * (map(xs:QName, item()*)). Its body binds each xsl:param from its map
 * (or the default, or XTDE0700 when it is required), then the sequence
 * constructor, coerced to the template's `as` type (XTTE0505).
 *
 * - xsl:call-template is a STATIC_CALL of that function with the caller's
 *   focus (the empty sequence for an absent focus, D144), a map of the
 *   non-tunnel xsl:with-param values and the tunnel parameters passed on
 *   (10.1.6: those the enclosing template received, overlaid by the
 *   tunnel xsl:with-param values; none outside a template).
 * - xsl:apply-templates, xsl:next-match and xsl:apply-imports are
 *   APPLY_TEMPLATES (the one IR kind of template rules, src/ir_internal.h),
 *   evaluated by src/evaluator_template.c; next-match and apply-imports
 *   pass on the parameters their template received, overlaid by their
 *   xsl:with-param values (6.11, 4.0).
 * - Modes (6.9): the unnamed mode, every mode declared by xsl:mode and
 *   every mode named by a template, an apply-templates or a default-mode,
 *   with on-no-match and on-multiple-match from the xsl:mode declarations
 *   of highest import precedence (XTSE0545). Each mode's rules (one per
 *   template rule applicable to it: its match function from the pattern,
 *   D118, and its priority, explicit or default) are ordered by import
 *   precedence, priority and declaration order (6.8) when added.
 * - Built-in templates (6.10) are template functions too, lowered here from
 *   short XPath texts and the same IR kinds (APPLY_TEMPLATES in the current
 *   mode with the parameters received, TEXT_CONSTRUCTOR, SHALLOW_COPY,
 *   RAISE of XTDE0555): one per on-no-match value used.
 *
 * Templates are lowered outside a lowering machine, one after another,
 * each by sxml_xpath_lowerer_run_procedure with a procedure of its own;
 * instructions are procedures pushed by the sequence constructor (D103,
 * D142). Nothing recurses (D71).
 *
 * Golden file: src/xslt_lower_construct.c (instructions as procedures
 * whose steps ask the machine for their parts and return), src/xslt_lower.c
 * (the main function, run_procedure from outside a machine).
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_lower_internal.h"

#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xslt_pattern_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The parameters of a template function (src/ir_internal.h). */
#define TEMPLATE_PARAMETER_COUNT 5U

/* Its parameter slots. */
static const sxml_ir_slot item_slot = {.value = 1};
static const sxml_ir_slot position_slot = {.value = 2};
static const sxml_ir_slot size_slot = {.value = 3};
static const sxml_ir_slot parameters_slot = {.value = 4};
static const sxml_ir_slot tunnel_parameters_slot = {.value = 5};

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* ------------------------------------------------------------ State */

/* One xsl:template (a declaration, or a child of an enclosing mode). */
typedef struct template_entry {
    sxml_xslt_ast_node_index node;
    sxml_ir_function_index function;
    /* A named template's second function, lowered with an absent focus,
       for calls where the focus is absent (10.1.2, XPDY0002 on reading
       it); 0 for template rules without a name. */
    sxml_ir_function_index absent_function;
    /* 0 for a template without a name. */
    sxml_name_identifier template_name;
    /* 0 for a template without match. */
    sxml_xslt_ast_expression_index pattern;
    sxml_ir_function_index match_function;
    /* The enclosing xsl:mode (6.9.6); 0 for a declaration. */
    sxml_xslt_ast_node_index enclosing_mode;
    int32_t import_precedence;
    int32_t lowest_imported_precedence;
    uint32_t declaration_order;
} template_entry;

/* One property of a mode from xsl:mode declarations (6.9.1): the value
   of highest import precedence (conflicts: check_mode_conflicts). */
typedef struct mode_property {
    bool is_set;
    uint32_t value;
    uint32_t import_precedence;
} mode_property;

/* One mode of the stylesheet; mode n of the array is the unit's mode
   n + 1. */
typedef struct mode_entry {
    /* 0 for the unnamed mode. */
    sxml_name_identifier mode_name;
    /* The package whose mode it is (3.4.6: each package has its own
       unnamed mode; a used package's public mode is shared). */
    uint32_t package;
    bool is_declared;
    bool is_enclosing;
    /* sxml_ir_built_in_rule values, bools as 0 and 1. */
    mode_property on_no_match;
    mode_property fails_on_multiple_match;
    mode_property copies_namespaces;
    /* The xsl:mode of highest precedence with an as attribute (6.9.5);
       0 for none. */
    sxml_xslt_ast_node_index as_declaration;
    uint32_t as_precedence;
} mode_entry;

/* The kinds of built-in template (6.10), twice: copying namespaces or
   not. */
#define BUILT_IN_TEMPLATE_KIND_COUNT 6U

/* The XPath texts the built-in templates and apply-templates without
   select are made of, parsed once per lowering. */
typedef enum snippet_kind {
    SXML_SNIPPET_IS_DOCUMENT_OR_ELEMENT,
    SXML_SNIPPET_IS_DOCUMENT,
    SXML_SNIPPET_IS_JNODE,
    SXML_SNIPPET_IS_XNODE,
    SXML_SNIPPET_IS_TEXT_OR_ATTRIBUTE_OR_ATOMIC,
    SXML_SNIPPET_IS_ARRAY,
    SXML_SNIPPET_CHILDREN,
    SXML_SNIPPET_JNODE_CHILDREN,
    SXML_SNIPPET_ATTRIBUTES,
    SXML_SNIPPET_MEMBERS,
    SXML_SNIPPET_STRING,
    SXML_SNIPPET_DEFAULT_SELECT,
    SXML_SNIPPET_COUNT,
} snippet_kind;

static const char *const snippet_texts[SXML_SNIPPET_COUNT] = {
    [SXML_SNIPPET_IS_DOCUMENT_OR_ELEMENT] = ". instance of (document-node() | element())",
    [SXML_SNIPPET_IS_DOCUMENT] = ". instance of document-node()",
    [SXML_SNIPPET_IS_JNODE] = ". instance of jnode()",
    [SXML_SNIPPET_IS_XNODE] = ". instance of node()",
    [SXML_SNIPPET_IS_TEXT_OR_ATTRIBUTE_OR_ATOMIC] =
        ". instance of (text() | attribute() | Q{http://www.w3.org/2001/XMLSchema}anyAtomicType)",
    [SXML_SNIPPET_IS_ARRAY] = ". instance of array(*)",
    [SXML_SNIPPET_CHILDREN] = "child::node()",
    [SXML_SNIPPET_JNODE_CHILDREN] = "child::*",
    [SXML_SNIPPET_ATTRIBUTES] = "attribute::node()",
    [SXML_SNIPPET_MEMBERS] = "?*",
    [SXML_SNIPPET_STRING] = "string(.)",
    /* 6.6: child::node() of an XNode, child::* of a JNode, the children of
       jtree(.) for a map or an array, else XTTE0510. */
    [SXML_SNIPPET_DEFAULT_SELECT] =
        "if (. instance of jnode()) then child::* "
        "else if (. instance of node()) then child::node() "
        "else if (. instance of (map(*) | array(*))) then jtree(.)/child::* "
        "else error(QName('http://www.w3.org/2005/xqt-errors', 'XTTE0510'), "
        "'xsl:apply-templates without select is evaluated with a context item that is not "
        "a node, a map or an array (XSLT 4.0, 6.6)')",
};

struct sxml_xslt_template_lowering {
    template_entry *templates;
    size_t template_count;
    mode_entry *modes;
    size_t mode_count;
    size_t mode_capacity;
    /* The empty map, a constant of the unit. */
    sxml_ir_atom empty_map;
    /* By sxml_ir_built_in_rule, then copy-namespaces "no"; 0 until made. */
    sxml_ir_function_index built_in_templates[2U * BUILT_IN_TEMPLATE_KIND_COUNT];
    sxml_ast *snippets[SXML_SNIPPET_COUNT];
    /* The template whose body is being lowered (0: none) and the focus of
       its function frame: instructions in it see its parameter maps;
       those lowered with another focus (xsl:for-each) or in another
       function do not (6.11, 10.1.6). */
    sxml_ir_function_index current_function;
    const sxml_xpath_lowering_focus *current_focus;
    /* The signature of template functions. */
    sxml_type_identifier template_signature;
};

/* ------------------------------------------------------------ AST reads */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

/* The attribute of kind on node; a zero attribute (kind NONE) when it
   has none. */
static const sxml_xslt_ast_attribute *attribute_of(const sxml_xslt_lowering *lowering,
                                                   sxml_xslt_ast_attribute_query query) {
    return sxml_xslt_ast_get_attribute(lowering->ast,
                                       sxml_xslt_ast_find_attribute(lowering->ast, query));
}

static const sxml_xslt_ast_attribute *find_attribute(const sxml_xslt_lowering *lowering,
                                                     sxml_xslt_ast_node_index node,
                                                     sxml_xslt_ast_attribute_kind kind) {
    return attribute_of(lowering, (sxml_xslt_ast_attribute_query){.node = node, .kind = kind});
}

/* A BOOLEAN attribute's value, or fallback when absent. */
typedef struct boolean_query {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_attribute_kind kind;
    bool fallback;
} boolean_query;

static bool boolean_of(const sxml_xslt_lowering *lowering, boolean_query query) {
    const sxml_xslt_ast_attribute *attribute = find_attribute(lowering, query.node, query.kind);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_BOOLEAN) {
        return query.fallback;
    }
    return attribute->value.payload.is_true;
}

/* A NAME attribute's name; 0 when absent. */
static sxml_name_identifier name_of(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *attribute =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_NAME);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME) {
        return (sxml_name_identifier){.index = 0};
    }
    return attribute->value.payload.name;
}

static int32_t precedence_of(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    return (int32_t)sxml_xslt_ast_get_module(lowering->ast, node_of(lowering, node)->module)
        ->import_precedence;
}

/* The XPath static context of an expression of the AST. */
static const sxml_xpath_static_context *
expression_context(const sxml_xslt_lowering *lowering, sxml_xslt_ast_expression_index expression) {
    const uint32_t context = sxml_xslt_ast_get_expression(lowering->ast, expression)
                                 ->static_context.value;
    if (context == 0 || context >= lowering->static_context_count) {
        return NULL;
    }
    return &lowering->static_contexts[context];
}

/* The sequence type of the `as` attribute of node; item()* when absent.
   *has_type tells which. */
typedef struct declared_type {
    bool has_type;
    sxml_sequence_type sequence_type;
} declared_type;

static sxml_status resolve_declared_type(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node, declared_type *type) {
    type->has_type = false;
    type->sequence_type = (sxml_sequence_type){
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    const sxml_xslt_ast_attribute *as_attribute = find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_AS);
    if (as_attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, as_attribute->value.payload.expression);
    const sxml_xpath_static_context *context =
        expression_context(lowering, as_attribute->value.payload.expression);
    if (expression->ast == NULL || context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    type->has_type = true;
    return sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = expression->ast,
                                   .type_node = expression->ast->root,
                                   .static_context = context,
                                   .error_record = lowering->error_record},
        &type->sequence_type);
}

static bool allows_empty(sxml_sequence_type sequence_type) {
    return sequence_type.occurrence == SXML_OCCURRENCE_ZERO_OR_ONE ||
           sequence_type.occurrence == SXML_OCCURRENCE_ZERO_OR_MORE ||
           sequence_type.occurrence == SXML_OCCURRENCE_ZERO;
}

/* ---------------------------------------------------------- IR helpers */

static sxml_status add_expression(sxml_xpath_lowerer *lowerer, sxml_ir_expression expression,
                                  sxml_ir_expression_index *index) {
    return sxml_xpath_lowerer_add_expression(
        lowerer,
        (sxml_xpath_lowering_expression_to_add){.node = no_ast_node, .expression = expression},
        index);
}

/* An expression to bind in a block. */
typedef sxml_xslt_expression_to_bind expression_binding;

static sxml_status bind_expression(sxml_xpath_lowerer *lowerer, expression_binding binding,
                                   sxml_ir_atom *atom) {
    return sxml_xslt_lowering_bind_expression(lowerer, &binding, atom);
}

static sxml_ir_atom slot_atom(sxml_ir_slot slot) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = slot};
}

static sxml_ir_expression atom_expression(sxml_ir_atom atom) {
    return (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = atom};
}

/* An xs:QName constant of name (no prefix: map keys compare expanded
   names). */
static sxml_status add_name_constant(sxml_xpath_lowerer *lowerer, sxml_name_identifier name,
                                     sxml_ir_atom *atom) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        lowerer->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = name,
                                                  .namespace_prefix = {.index = 0}}}}},
        &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_item_constant(lowerer, item, atom);
}

static sxml_status add_integer_constant(sxml_xpath_lowerer *lowerer, int64_t integer,
                                        sxml_ir_atom *atom) {
    return sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_from_integer(integer),
                                                atom);
}

/* The empty sequence, bound in block. */
static sxml_status bind_empty_sequence(sxml_xpath_lowerer *lowerer,
                                       sxml_xpath_lowering_block *block, sxml_ir_atom *atom) {
    return bind_expression(lowerer, (expression_binding){.block = block, .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                                .sequence = {.first_atom_index = 0,
                                                             .atom_count = 0}}}, atom);
}

/* A RAISE of a dynamic or type error with an XSLT code. */
typedef struct raise_text {
    sxml_status error_status;
    /* Static ASCII. */
    const char *code;
    const char *description;
} raise_text;

static sxml_status add_raise(sxml_xpath_lowerer *lowerer, raise_text text,
                             sxml_ir_expression_index *expression) {
    return sxml_xpath_lowerer_add_raise(
        lowerer,
        (sxml_xpath_lowering_raise_request){.node = no_ast_node,
                                            .error_status = text.error_status,
                                            .code_local_name = sxml_byte_view_from_c_string(
                                                text.code),
                                            .description = sxml_byte_view_from_c_string(
                                                text.description)},
        expression);
}

/* A call of a function of the function table, bound in block. */
typedef struct library_call {
    sxml_xpath_lowering_block *block;
    sxml_well_known_namespace function_namespace;
    const char *local_name;
    const sxml_ir_atom *arguments;
    size_t argument_count;
} library_call;

static sxml_status bind_call(sxml_xpath_lowerer *lowerer, library_call call, sxml_ir_atom *atom) {
    return sxml_xslt_lowering_bind_library_call(
        lowerer,
        &(sxml_xslt_library_call){.block = call.block,
                                  .function_namespace = call.function_namespace,
                                  .local_name = sxml_byte_view_from_c_string(call.local_name),
                                  .arguments = {.atoms = call.arguments,
                                                .atom_count = call.argument_count}},
        atom);
}

static sxml_status report_static_error(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_ast_node_index node,
                                       sxml_byte_view code_and_description) {
    return sxml_xslt_lowering_report_static_error(
        lowering,
        (sxml_xslt_static_error){.node = node, .code_and_description = code_and_description});
}

/* ------------------------------------------------------- Reserved names */

/* True for a name in a reserved namespace (3.5.1): the XSLT, XML, xmlns,
   XML Schema, schema instance, function, map, array, math and error
   namespaces; xsl:initial-template is a permitted template name. */
typedef struct reserved_name_query {
    sxml_name_identifier name;
    bool is_template_name;
} reserved_name_query;

static bool is_reserved_name(const sxml_xslt_lowering *lowering, reserved_name_query query) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (query.name.index == 0 ||
        sxml_name_pool_get_name_parts(lowering->lowerer->name_pool, query.name, &parts) !=
            SXML_STATUS_OK) {
        return false;
    }
    const uint32_t uri = parts.namespace_uri.index;
    if (uri == (uint32_t)SXML_NAMESPACE_XSLT) {
        return !(query.is_template_name &&
                 sxml_byte_view_equals(parts.local_name,
                                       SXML_BYTE_VIEW_FROM_LITERAL("initial-template")));
    }
    return uri != (uint32_t)SXML_NAMESPACE_NONE && uri <= (uint32_t)SXML_NAMESPACE_ERRORS;
}

static sxml_status report_reserved_name(const sxml_xslt_lowering *lowering,
                                        sxml_xslt_ast_node_index node) {
    return report_static_error(lowering, node,
                               SXML_BYTE_VIEW_FROM_LITERAL(
                                   "XTSE0080 a template, mode or parameter is named in a "
                                   "reserved namespace (XSLT 4.0, 3.5.1)"));
}

/* ------------------------------------------------------------- Lookups */

/* A named template's name in the package that declares it. */
typedef struct template_key {
    sxml_name_identifier template_name;
    uint32_t package;
} template_key;

static const template_entry *find_named_template(const sxml_xslt_lowering *lowering,
                                                 template_key key) {
    const sxml_xslt_template_lowering *templates = lowering->templates;
    const template_entry *found = NULL;
    for (size_t entry_index = 0; entry_index < templates->template_count; entry_index += 1) {
        const template_entry *entry = &templates->templates[entry_index];
        if (entry->template_name.index == key.template_name.index &&
            sxml_xslt_package_of_node(lowering, entry->node) == key.package &&
            (found == NULL || entry->import_precedence > found->import_precedence)) {
            found = entry;
        }
    }
    return found;
}

static const template_entry *find_template_entry(const sxml_xslt_template_lowering *templates,
                                                 sxml_xslt_ast_node_index node) {
    for (size_t entry_index = 0; entry_index < templates->template_count; entry_index += 1) {
        if (templates->templates[entry_index].node.value == node.value) {
            return &templates->templates[entry_index];
        }
    }
    return NULL;
}

/* The mode a mode token names in node's static context: NAME, UNNAMED,
   DEFAULT (the default mode, 3.6.3). *is_current for #current. The
   package is the one whose mode it is (3.4.6). */
typedef struct mode_reference {
    bool is_current;
    bool is_all;
    sxml_name_identifier mode_name;
    uint32_t package;
} mode_reference;

/* The unit's index of a mode (name 0: the package's unnamed mode); 0 when
   unknown. */
static sxml_ir_mode_index find_mode(const sxml_xslt_template_lowering *templates,
                                    mode_reference reference) {
    for (size_t mode_index = 0; mode_index < templates->mode_count; mode_index += 1) {
        if (templates->modes[mode_index].mode_name.index == reference.mode_name.index &&
            templates->modes[mode_index].package == reference.package) {
            return (sxml_ir_mode_index){.value = (uint32_t)mode_index + 1U};
        }
    }
    return (sxml_ir_mode_index){.value = 0};
}

/* The mode name names from node: its package's own, or a used package's
   it accepts (3.4.5.5, 3.4.6); the unnamed mode is the package's own. */
static mode_reference named_mode(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                                 sxml_name_identifier name) {
    const uint32_t package = sxml_xslt_package_of_node(lowering, node);
    mode_reference reference = {
        .is_current = false, .is_all = false, .mode_name = name, .package = package};
    if (name.index != 0) {
        const sxml_xslt_component_binding binding = sxml_xslt_package_bind(
            lowering, &(sxml_xslt_component_reference){.component_kind = SXML_XSLT_COMPONENT_KIND_MODE,
                                                       .component_name = name,
                                                       .arity = 0,
                                                       .package = package});
        if (binding.declaration.value != 0) {
            reference.package = binding.package;
        }
    }
    return reference;
}

/* The mode an xsl:mode declaration declares, in its own package. */
static mode_reference declared_mode(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index declaration) {
    return (mode_reference){.is_current = false,
                            .is_all = false,
                            .mode_name = name_of(lowering, declaration),
                            .package = sxml_xslt_package_of_node(lowering, declaration)};
}

static mode_reference reference_of(const sxml_xslt_lowering *lowering,
                                   sxml_xslt_ast_node_index node, sxml_xslt_ast_name_token token) {
    mode_reference reference = named_mode(lowering, node, (sxml_name_identifier){.index = 0});
    switch ((uint32_t)token.kind) {
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAME:
        reference = named_mode(lowering, node, token.name);
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT:
        reference.is_current = true;
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ALL:
        reference.is_all = true;
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT: {
        const sxml_xslt_ast_static_context *context =
            sxml_xslt_ast_get_static_context(lowering->ast, node_of(lowering, node)->static_context);
        if (context->default_mode.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME) {
            reference = named_mode(lowering, node, context->default_mode.name);
        }
        break;
    }
    default:
        break;
    }
    return reference;
}

/* The default mode of node's static context (3.6.3) as a reference. */
static mode_reference default_mode_of(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_node_index node) {
    return reference_of(lowering, node,
                        (sxml_xslt_ast_name_token){.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT});
}

/* The tokens of the mode attribute of node; empty when absent. */
static sxml_xslt_ast_name_token_view mode_tokens_of(const sxml_xslt_lowering *lowering,
                                                    sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *mode =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_MODE);
    if (mode->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        return (sxml_xslt_ast_name_token_view){.name_tokens = NULL, .name_token_count = 0};
    }
    return sxml_xslt_ast_get_name_tokens(lowering->ast, mode->value.payload.name_tokens);
}

/* ---------------------------------------------------- The template body */

/* True when the instruction being lowered is directly in the body of the
   template being lowered (no xsl:for-each or other changed focus, and the
   same function): it then sees the template's parameter maps. */
static bool is_in_template_body(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_template_lowering *templates = lowering->templates;
    const sxml_xpath_lowerer *lowerer = lowering->lowerer;
    return templates != NULL && templates->current_function.value != 0 &&
           lowerer->function != NULL &&
           lowerer->function->function_index.value == templates->current_function.value &&
           lowerer->function->focus == templates->current_focus;
}

/* True when the instruction is in a template's function (any focus). */
static bool is_in_template_function(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_template_lowering *templates = lowering->templates;
    const sxml_xpath_lowerer *lowerer = lowering->lowerer;
    return templates != NULL && templates->current_function.value != 0 &&
           lowerer->function != NULL &&
           lowerer->function->function_index.value == templates->current_function.value;
}

/* What one xsl:param binds (9.2): read in the first step for it. */
typedef struct parameter_plan {
    sxml_name_identifier parameter_name;
    bool is_tunnel;
    declared_type type;
    bool has_select;
    bool has_content;
    /* Explicitly or implicitly mandatory (9.2.2). */
    bool is_required;
} parameter_plan;

static sxml_status plan_parameter(const sxml_xslt_lowering *lowering,
                                  sxml_xslt_ast_node_index parameter, parameter_plan *plan) {
    *plan = (parameter_plan){
        .parameter_name = name_of(lowering, parameter),
        .is_tunnel = boolean_of(lowering, (boolean_query){.node = parameter,
                                                          .kind = SXML_XSLT_AST_ATTRIBUTE_TUNNEL,
                                                          .fallback = false}),
        .has_select = find_attribute(lowering, parameter, SXML_XSLT_AST_ATTRIBUTE_SELECT)->kind !=
                      SXML_XSLT_AST_ATTRIBUTE_NONE,
        .has_content = node_of(lowering, parameter)->first_child.value != 0,
        .is_required = boolean_of(lowering, (boolean_query){.node = parameter,
                                                            .kind = SXML_XSLT_AST_ATTRIBUTE_REQUIRED,
                                                            .fallback = false})};
    if (is_reserved_name(lowering, (reserved_name_query){.name = plan->parameter_name,
                                                         .is_template_name = false})) {
        return report_reserved_name(lowering, parameter);
    }
    const sxml_status status = resolve_declared_type(lowering, parameter, &plan->type);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (plan->has_select && plan->has_content) {
        return report_static_error(lowering, parameter, SXML_BYTE_VIEW_FROM_LITERAL("XTSE0620 an xsl:param has both a select attribute and "
                                   "content (XSLT 4.0, 9.3)"));
    }
    if (!plan->has_select && !plan->has_content && plan->type.has_type &&
        !allows_empty(plan->type.sequence_type)) {
        plan->is_required = true;
    }
    return SXML_STATUS_OK;
}

typedef enum template_body_step {
    SXML_TEMPLATE_BODY_STEP_START = 0,
    /* The next xsl:param, or the sequence constructor. */
    SXML_TEMPLATE_BODY_STEP_NEXT_PARAMETER,
    /* A default value was lowered as a body. */
    SXML_TEMPLATE_BODY_STEP_DEFAULT_LOWERED,
    /* The sequence constructor was lowered. */
    SXML_TEMPLATE_BODY_STEP_CONSTRUCTOR_LOWERED,
} template_body_step;

typedef struct template_body_locals {
    sxml_xslt_instruction_locals head;
    sxml_xslt_ast_node_index next_child;
    sxml_xslt_ast_node_index parameter;
    parameter_plan plan;
    sxml_ir_atom is_supplied;
    sxml_ir_expression_index supplied_branch;
    sxml_ir_expression_index default_branch;
    sxml_ir_atom value;
} template_body_locals;

/* The context item checks of 10.1.4: XTTE3090 for an absent context item
   when one is required, XTTE0590 for one of the wrong type. *is_absent
   for use="absent". */
static sxml_status bind_context_item_checks(sxml_xpath_lowering_machine *machine,
                                            sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    const template_body_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index first = node_of(lowering, locals->head.node)->first_child;
    if (first.value == 0 || node_of(lowering, first)->kind != SXML_XSLT_AST_KIND_CONTEXT_ITEM) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_attribute *use = find_attribute(lowering, first, SXML_XSLT_AST_ATTRIBUTE_USE);
    const bool is_required = use->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD &&
                             use->value.payload.keyword == SXML_XSLT_AST_KEYWORD_REQUIRED;
    sxml_status status = SXML_STATUS_OK;
    if (is_required) {
        sxml_ir_atom is_absent = {0};
        status = bind_call(lowerer,
                           (library_call){.block = frame->block,
                                          .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                          .local_name = "empty",
                                          .arguments = (const sxml_ir_atom[]){slot_atom(item_slot)},
                                          .argument_count = 1},
                           &is_absent);
        sxml_ir_expression_index raise = {0};
        sxml_ir_expression_index nothing = {0};
        if (status == SXML_STATUS_OK) {
            status = add_raise(lowerer,
                               (raise_text){.error_status = SXML_STATUS_TYPE_ERROR,
                                            .code = "XTTE3090",
                                            .description = "the template requires a context "
                                                           "item and none is supplied (XSLT 4.0, "
                                                           "10.1.4)"},
                               &raise);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, &nothing);
        }
        sxml_ir_atom ignored = {0};
        if (status == SXML_STATUS_OK) {
            status = bind_expression(lowerer, (expression_binding){.block = frame->block, .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                     .conditional = {.condition = is_absent,
                                                     .then_branch = raise,
                                                     .else_branch = nothing}}}, &ignored);
        }
    }
    const sxml_xslt_ast_attribute *as_attribute = find_attribute(lowering, first, SXML_XSLT_AST_ATTRIBUTE_AS);
    if (status != SXML_STATUS_OK || as_attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE) {
        return status;
    }
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, as_attribute->value.payload.expression);
    const sxml_xpath_static_context *context =
        expression_context(lowering, as_attribute->value.payload.expression);
    if (expression->ast == NULL || context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* The AST's root is a SEQUENCE_TYPE (of occurrence one, D139). */
    sxml_sequence_type context_type = {.item_type = {0}, .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    status = sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = expression->ast,
                                   .type_node = expression->ast->root,
                                   .static_context = context,
                                   .error_record = lowering->error_record},
        &context_type);
    const sxml_type_identifier item_type = context_type.item_type;
    /* 10.1.4: no conversion; the item (when there is one) must match. */
    sxml_ir_atom matches = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(lowerer, (expression_binding){.block = frame->block, .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_INSTANCE_OF,
                                 .type_test = {.value = slot_atom(item_slot),
                                               .sequence_type = {.item_type = item_type,
                                                                 .occurrence =
                                                                     SXML_OCCURRENCE_ZERO_OR_ONE}}}}, &matches);
    }
    sxml_ir_expression_index nothing = {0};
    sxml_ir_expression_index raise = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, &nothing);
    }
    if (status == SXML_STATUS_OK) {
        status = add_raise(lowerer,
                           (raise_text){.error_status = SXML_STATUS_TYPE_ERROR,
                                        .code = "XTTE0590",
                                        .description = "the context item of a template does not "
                                                       "match its xsl:context-item type (XSLT "
                                                       "4.0, 10.1.4)"},
                           &raise);
    }
    sxml_ir_atom ignored = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(lowerer, (expression_binding){.block = frame->block, .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                 .conditional = {.condition = matches,
                                                 .then_branch = nothing,
                                                 .else_branch = raise}}}, &ignored);
    }
    return status;
}

/* The branch for a supplied parameter: map:get of its map, coerced to
   its type (XTTE0590). */
static sxml_status lower_supplied_branch(sxml_xpath_lowerer *lowerer, template_body_locals *locals,
                                         sxml_ir_atom name) {
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    const sxml_ir_atom arguments[2] = {
        slot_atom(locals->plan.is_tunnel ? tunnel_parameters_slot : parameters_slot), name};
    sxml_ir_atom value = {0};
    sxml_status status = bind_call(lowerer,
                                   (library_call){.block = &block,
                                                  .function_namespace = SXML_NAMESPACE_MAP_FUNCTIONS,
                                                  .local_name = "get",
                                                  .arguments = arguments,
                                                  .argument_count = 2},
                                   &value);
    if (status == SXML_STATUS_OK && locals->plan.type.has_type) {
        status = sxml_xpath_lowerer_coerce_atom(
            lowerer,
            (sxml_xpath_lowering_coercion_request){.block = &block,
                                                   .node = no_ast_node,
                                                   .value = value,
                                                   .sequence_type = locals->plan.type.sequence_type,
                                                   .role = SXML_IR_COERCION_ROLE_TEMPLATE_PARAMETER},
            &value);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(lowerer, atom_expression(value), &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = result},
        &locals->supplied_branch);
}

/* Starts the default value of the parameter (9.2.2, 9.3) as the else
   branch; *has_pushed when a frame lowers it. */
typedef struct default_start {
    bool has_pushed;
} default_start;

static sxml_status start_default_branch(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame, default_start *start) {
    sxml_xpath_lowerer *lowerer = machine->state;
    template_body_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    start->has_pushed = false;
    if (locals->plan.is_required) {
        return add_raise(lowerer,
                         (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                      .code = "XTDE0700",
                                      .description = "a template is invoked without a value for "
                                                     "a mandatory parameter (XSLT 4.0, 9.2.2)"},
                         &locals->default_branch);
    }
    if (locals->plan.has_select) {
        const sxml_xslt_ast_attribute *select =
            find_attribute(lowering, locals->parameter, SXML_XSLT_AST_ATTRIBUTE_SELECT);
        const sxml_xslt_ast_expression *expression =
            sxml_xslt_ast_get_expression(lowering->ast, select->value.payload.expression);
        start->has_pushed = true;
        return sxml_xpath_lowering_call_embedded(
            machine,
            &(sxml_xpath_lowering_embedded_call){
                .embedding = {.ast = expression->ast,
                              .node = {.value = 0},
                              .static_context =
                                  expression_context(lowering, select->value.payload.expression),
                              .focus = NULL},
                .block = NULL,
                .is_body = true,
                .destination = {.expression = &locals->default_branch,
                                .atom = NULL,
                                .atom_block = NULL,
                                .focused = NULL}});
    }
    if (locals->plan.has_content) {
        start->has_pushed = true;
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = node_of(lowering, locals->parameter)->first_child,
                         .block = NULL,
                         .is_body = true,
                         .destination = {.expression = &locals->default_branch,
                                         .atom = NULL,
                                         .atom_block = NULL,
                                         .focused = NULL},
                         .focus = NULL});
    }
    /* The implicit default (9.2.2): () with an as attribute, else "". */
    if (locals->plan.type.has_type) {
        return sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node,
                                                     &locals->default_branch);
    }
    sxml_ir_atom text = {0};
    const sxml_status status = sxml_xpath_lowerer_add_string_constant(
        lowerer, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(lowerer, atom_expression(text), &locals->default_branch);
}

/* After the default: content without as is a temporary tree (9.4); with
   as, the default is coerced too. */
static sxml_status finish_default_branch(sxml_xpath_lowerer *lowerer,
                                         template_body_locals *locals) {
    sxml_status status = SXML_STATUS_OK;
    if (locals->plan.has_content && !locals->plan.type.has_type) {
        status = add_expression(
            lowerer,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                                 .node_construction = {.content = locals->default_branch,
                                                       .construction_flags = (uint32_t)
                                                           SXML_IR_CONSTRUCTION_XSLT_RULES}},
            &locals->default_branch);
    }
    if (status != SXML_STATUS_OK || !locals->plan.type.has_type || locals->plan.is_required) {
        return status;
    }
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom value = {0};
    status = sxml_xpath_lowerer_bind_expression(
        lowerer,
        (sxml_xpath_lowering_expression_in_block){.block = &block,
                                                  .expression = locals->default_branch},
        &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_coerce_atom(
            lowerer,
            (sxml_xpath_lowering_coercion_request){.block = &block,
                                                   .node = no_ast_node,
                                                   .value = value,
                                                   .sequence_type = locals->plan.type.sequence_type,
                                                   .role = SXML_IR_COERCION_ROLE_TEMPLATE_PARAMETER},
            &value);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(lowerer, atom_expression(value), &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = result},
        &locals->default_branch);
}

/* Binds the parameter's value (supplied or default) and puts it in
   scope. */
static sxml_status bind_parameter(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_frame *frame) {
    template_body_locals *locals = frame->locals;
    sxml_status status = finish_default_branch(lowerer, locals);
    if (status == SXML_STATUS_OK) {
        status = bind_expression(lowerer, (expression_binding){.block = frame->block, .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                 .conditional = {.condition = locals->is_supplied,
                                                 .then_branch = locals->supplied_branch,
                                                 .else_branch = locals->default_branch}}}, &locals->value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_push_variable(
        lowerer, (sxml_xpath_lowering_scope_entry){.variable_name = locals->plan.parameter_name,
                                                   .function_level =
                                                       lowerer->function->function_level,
                                                   .atom = locals->value});
}

/* Starts the next xsl:param: its plan, map:contains and the supplied
   branch, then the default. *has_pushed when a frame lowers the
   default. */
static sxml_status start_parameter(sxml_xpath_lowering_machine *machine,
                                   sxml_xpath_lowering_frame *frame, default_start *start) {
    sxml_xpath_lowerer *lowerer = machine->state;
    template_body_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = plan_parameter(lowering, locals->parameter, &locals->plan);
    sxml_ir_atom name = {0};
    if (status == SXML_STATUS_OK) {
        status = add_name_constant(lowerer, locals->plan.parameter_name, &name);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_ir_atom arguments[2] = {
            slot_atom(locals->plan.is_tunnel ? tunnel_parameters_slot : parameters_slot), name};
        status = bind_call(lowerer,
                           (library_call){.block = frame->block,
                                          .function_namespace = SXML_NAMESPACE_MAP_FUNCTIONS,
                                          .local_name = "contains",
                                          .arguments = arguments,
                                          .argument_count = 2},
                           &locals->is_supplied);
    }
    if (status == SXML_STATUS_OK) {
        status = lower_supplied_branch(lowerer, locals, name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return start_default_branch(machine, frame, start);
}

/* The xsl:mode whose as type a template rule takes (6.9.5): that of its
   first mode with one; 0 for a named template or none. */
static sxml_xslt_ast_node_index mode_type_declaration_of(const sxml_xslt_lowering *lowering,
                                                         sxml_xslt_ast_node_index node);

static sxml_status finish_template_body(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    template_body_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    declared_type type = {.has_type = false};
    sxml_status status = resolve_declared_type(lowering, locals->head.node, &type);
    if (status == SXML_STATUS_OK && !type.has_type) {
        /* 6.9.5: a rule without as takes the as of its mode. */
        const sxml_xslt_ast_node_index mode_declaration =
            mode_type_declaration_of(lowering, locals->head.node);
        if (mode_declaration.value != 0) {
            status = resolve_declared_type(lowering, mode_declaration, &type);
        }
    }
    sxml_ir_atom value = locals->value;
    if (status == SXML_STATUS_OK && type.has_type) {
        /* 6.1: the result is coerced to the as type, XTTE0505 for one
           that does not match. */
        static const sxml_xslt_error_mapping mappings[] = {
            {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
             .raised_status = SXML_STATUS_TYPE_ERROR,
             .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTTE0505"),
             .description = SXML_BYTE_VIEW_INITIALIZER(
                 "the result of a template does not match its as type (XSLT 4.0, 6.1)")},
        };
        status = sxml_xslt_lowering_bind_mapped_expression(
            lowerer,
            &(sxml_xslt_mapped_expression){
                .block = frame->block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_COERCE,
                               .coercion = {.value = value,
                                            .sequence_type = type.sequence_type,
                                            .role = SXML_IR_COERCION_ROLE_FUNCTION_RESULT}},
                .mappings = {.mappings = mappings,
                             .mapping_count = sizeof(mappings) / sizeof(mappings[0])}},
            &value);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(lowerer, atom_expression(value), &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

static sxml_status continue_template_body(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    template_body_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = SXML_STATUS_OK;
    switch ((template_body_step)frame->step) {
    case SXML_TEMPLATE_BODY_STEP_START:
        status = bind_context_item_checks(machine, frame);
        locals->next_child = node_of(lowering, locals->head.node)->first_child;
        break;
    case SXML_TEMPLATE_BODY_STEP_NEXT_PARAMETER:
        break;
    case SXML_TEMPLATE_BODY_STEP_DEFAULT_LOWERED:
        status = bind_parameter(lowerer, frame);
        break;
    case SXML_TEMPLATE_BODY_STEP_CONSTRUCTOR_LOWERED:
        return finish_template_body(machine, frame);
    }
    frame->step = (uint32_t)SXML_TEMPLATE_BODY_STEP_NEXT_PARAMETER;
    while (status == SXML_STATUS_OK && locals->next_child.value != 0) {
        const sxml_xslt_ast_node_index child = locals->next_child;
        locals->next_child = node_of(lowering, child)->next_sibling;
        if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_PARAM) {
            continue;
        }
        locals->parameter = child;
        default_start start = {.has_pushed = false};
        status = start_parameter(machine, frame, &start);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (start.has_pushed) {
            frame->step = (uint32_t)SXML_TEMPLATE_BODY_STEP_DEFAULT_LOWERED;
            return SXML_STATUS_OK;
        }
        status = bind_parameter(lowerer, frame);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->step = (uint32_t)SXML_TEMPLATE_BODY_STEP_CONSTRUCTOR_LOWERED;
    return sxml_xslt_lowering_call_sequence_constructor(
        machine, &(sxml_xslt_sequence_constructor_call){
                     .lowering = lowering,
                     .first = sxml_xslt_lowering_first_constructor_child(lowering->ast,
                                                                         locals->head.node),
                     .block = frame->block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = &locals->value,
                                     .atom_block = frame->block,
                                     .focused = NULL},
                     .focus = NULL});
}

static const sxml_xpath_lowering_procedure template_body_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:template"),
    .step = continue_template_body,
    .locals_size = sizeof(template_body_locals)};

/* -------------------------------------------- Template functions, entry */

/* One function of a template to lower: its own, or the absent-focus
   variant. */
typedef struct template_function_request {
    const template_entry *entry;
    bool is_focus_absent;
} template_function_request;

/* Lowers one template's body into a function (outside a machine). */
static sxml_status lower_template_function(sxml_xslt_lowering *lowering,
                                           template_function_request request) {
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    sxml_xslt_template_lowering *templates = lowering->templates;
    const template_entry *entry = request.entry;
    const sxml_ir_function_index function =
        request.is_focus_absent ? entry->absent_function : entry->function;
    const sxml_xslt_ast_node_index first = node_of(lowering, entry->node)->first_child;
    bool is_focus_absent = request.is_focus_absent;
    if (first.value != 0 && node_of(lowering, first)->kind == SXML_XSLT_AST_KIND_CONTEXT_ITEM) {
        const sxml_xslt_ast_attribute *use =
            find_attribute(lowering, first, SXML_XSLT_AST_ATTRIBUTE_USE);
        is_focus_absent = is_focus_absent || (use->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD &&
                          use->value.payload.keyword == SXML_XSLT_AST_KEYWORD_ABSENT);
    }
    const sxml_ir_focus focus_slots = {
        .context_value = item_slot, .context_position = position_slot, .context_size = size_slot};
    sxml_xpath_lowering_function_frame function_frame = {
        .function = {.function_index = function,
                     .function_level = 0,
                     .slot_count = TEMPLATE_PARAMETER_COUNT,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = is_focus_absent ? SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT
                                          : SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS,
                  .slots = focus_slots},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &function_frame);
    templates->current_function = function;
    templates->current_focus = &function_frame.focus;
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    sxml_ir_expression_index body = {0};
    const sxml_xslt_instruction_locals initial = {.lowering = lowering, .node = entry->node};
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xslt_package_is_abstract(lowering, entry->node)) {
        /* 3.4.5.2: an abstract template no package overrides. */
        status = add_raise(lowerer,
                           (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                        .code = "XTDE3052",
                                        .description = "an abstract template no package "
                                                       "overrides is called (XSLT 4.0, 3.4.5.2)"},
                           &body);
    } else {
        status = sxml_xpath_lowerer_run_procedure(
            lowerer, &(sxml_xpath_lowering_procedure_call){
                         .procedure = &template_body_procedure,
                         .node = no_ast_node,
                         .block = &block,
                         .is_body = false,
                         .destination = {.expression = &body,
                                         .atom = NULL,
                                         .atom_block = NULL,
                                         .focused = NULL},
                         .initial_locals = {.bytes = (const uint8_t *)&initial,
                                            .byte_count = sizeof(initial)}});
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_finish_block(
                lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = body},
                &body);
        }
    }
    templates->current_function = (sxml_ir_function_index){.value = 0};
    templates->current_focus = NULL;
    sxml_xpath_lowerer_leave_function(lowerer, &function_frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = function, .function = *sxml_ir_unit_get_function(lowerer->unit, function)};
    replacement.function.body = body;
    replacement.function.slot_count = function_frame.function.slot_count;
    replacement.function.focus = focus_slots;
    return sxml_ir_unit_replace_function(lowerer->unit, &replacement);
}

/* ======================================================= Instructions */

/* One xsl:with-param, lowered. */
typedef struct with_parameter {
    sxml_name_identifier parameter_name;
    bool is_tunnel;
    sxml_ir_atom value;
} with_parameter;

/* The parameter maps of an invocation (9.10, 10.1.6): parameters is
   base_parameters (or the empty map) with the non-tunnel xsl:with-param
   values put in, tunnel_parameters likewise. Written through the
   pointers into the caller's frame. */
typedef struct parameter_maps_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom base_parameters;
    sxml_ir_atom base_tunnel_parameters;
    sxml_ir_atom *parameters;
    sxml_ir_atom *tunnel_parameters;
    /* The with-param being lowered and its type. */
    sxml_xslt_ast_node_index next_child;
    sxml_xslt_ast_node_index current;
    declared_type type;
    bool has_content;
    /* Owned memory 0. */
    with_parameter *values;
    size_t value_count;
    /* Where the frame's own (empty) result goes. */
    sxml_ir_expression_index finished;
} parameter_maps_locals;

/* Starts the value of the with-param current (9.3): select, content, or
   "" (() with as). *has_pushed when a frame lowers it. */
static sxml_status start_with_parameter(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame, default_start *start) {
    sxml_xpath_lowerer *lowerer = machine->state;
    parameter_maps_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index current = locals->current;
    with_parameter *value = &locals->values[locals->value_count];
    locals->value_count += 1;
    *value = (with_parameter){
        .parameter_name = name_of(lowering, current),
        .is_tunnel = boolean_of(lowering, (boolean_query){.node = current,
                                                          .kind = SXML_XSLT_AST_ATTRIBUTE_TUNNEL,
                                                          .fallback = false})};
    for (size_t value_index = 0; value_index + 1 < locals->value_count; value_index += 1) {
        if (locals->values[value_index].parameter_name.index == value->parameter_name.index) {
            return report_static_error(lowering, current, SXML_BYTE_VIEW_FROM_LITERAL("XTSE0670 two xsl:with-param elements of one instruction "
                                       "have the same name (XSLT 4.0, 9.10)"));
        }
    }
    sxml_status status = resolve_declared_type(lowering, current, &locals->type);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xslt_ast_attribute *select =
        find_attribute(lowering, current, SXML_XSLT_AST_ATTRIBUTE_SELECT);
    locals->has_content = node_of(lowering, current)->first_child.value != 0;
    start->has_pushed = true;
    if (select->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
        if (locals->has_content) {
            return report_static_error(lowering, current, SXML_BYTE_VIEW_FROM_LITERAL("XTSE0620 an xsl:with-param has both a select attribute "
                                       "and content (XSLT 4.0, 9.3)"));
        }
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &value->value,
                                                  .focus = NULL});
    }
    if (locals->has_content) {
        return sxml_xslt_lowering_call_sequence_constructor(
            machine, &(sxml_xslt_sequence_constructor_call){
                         .lowering = lowering,
                         .first = node_of(lowering, current)->first_child,
                         .block = frame->block,
                         .is_body = false,
                         .destination = {.expression = NULL,
                                         .atom = &value->value,
                                         .atom_block = frame->block,
                                         .focused = NULL},
                         .focus = NULL});
    }
    start->has_pushed = false;
    if (locals->type.has_type) {
        return bind_empty_sequence(lowerer, frame->block, &value->value);
    }
    return sxml_xpath_lowerer_add_string_constant(
        lowerer, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &value->value);
}

/* After the value: a temporary tree for content without as (9.4), and
   the coercion to the as type. */
static sxml_status finish_with_parameter(sxml_xpath_lowerer *lowerer,
                                         sxml_xpath_lowering_frame *frame) {
    parameter_maps_locals *locals = frame->locals;
    with_parameter *value = &locals->values[locals->value_count - 1];
    sxml_status status = SXML_STATUS_OK;
    if (locals->has_content && !locals->type.has_type) {
        sxml_ir_expression_index content = {0};
        status = add_expression(lowerer, atom_expression(value->value), &content);
        if (status == SXML_STATUS_OK) {
            status = bind_expression(lowerer, (expression_binding){.block = frame->block, .expression = (sxml_ir_expression){
                    .expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                    .node_construction = {.content = content,
                                          .construction_flags =
                                              (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES}}}, &value->value);
        }
    }
    if (status == SXML_STATUS_OK && locals->type.has_type) {
        status = sxml_xpath_lowerer_coerce_atom(
            lowerer,
            (sxml_xpath_lowering_coercion_request){.block = frame->block,
                                                   .node = no_ast_node,
                                                   .value = value->value,
                                                   .sequence_type = locals->type.sequence_type,
                                                   .role = SXML_IR_COERCION_ROLE_TEMPLATE_PARAMETER},
            &value->value);
    }
    return status;
}

/* Puts the values whose is_tunnel is tunnel into base with map:put. */
typedef struct map_building {
    sxml_xpath_lowering_frame *frame;
    sxml_ir_atom base;
    bool is_tunnel;
} map_building;

static sxml_status bind_parameter_map(sxml_xpath_lowerer *lowerer, map_building building,
                                      sxml_ir_atom *map) {
    const parameter_maps_locals *locals = building.frame->locals;
    *map = building.base;
    sxml_status status = SXML_STATUS_OK;
    for (size_t value_index = 0; status == SXML_STATUS_OK && value_index < locals->value_count;
         value_index += 1) {
        const with_parameter *value = &locals->values[value_index];
        if (value->is_tunnel != building.is_tunnel) {
            continue;
        }
        sxml_ir_atom name = {0};
        status = add_name_constant(lowerer, value->parameter_name, &name);
        if (status == SXML_STATUS_OK) {
            const sxml_ir_atom arguments[3] = {*map, name, value->value};
            status = bind_call(lowerer,
                               (library_call){.block = building.frame->block,
                                              .function_namespace = SXML_NAMESPACE_MAP_FUNCTIONS,
                                              .local_name = "put",
                                              .arguments = arguments,
                                              .argument_count = 3},
                               map);
        }
    }
    return status;
}

static sxml_status continue_parameter_maps(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    parameter_maps_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        size_t child_count = 0;
        for (sxml_xslt_ast_node_index child = node_of(lowering, locals->head.node)->first_child;
             child.value != 0; child = node_of(lowering, child)->next_sibling) {
            child_count += node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_WITH_PARAM ? 1U : 0U;
        }
        if (child_count > 0) {
            status = sxml_xpath_lowering_allocate_owned(
                machine, frame,
                (sxml_xpath_lowering_owned_request){
                    .memory_index = 0,
                    .scratch = {.element_count = child_count,
                                .element_size = sizeof(with_parameter),
                                .element_alignment = alignof(with_parameter)}});
            locals->values = frame->owned_memory[0];
        }
        locals->next_child = node_of(lowering, locals->head.node)->first_child;
        frame->step = 1;
    } else {
        status = finish_with_parameter(lowerer, frame);
    }
    while (status == SXML_STATUS_OK && locals->next_child.value != 0) {
        const sxml_xslt_ast_node_index child = locals->next_child;
        locals->next_child = node_of(lowering, child)->next_sibling;
        if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_WITH_PARAM) {
            continue;
        }
        locals->current = child;
        default_start start = {.has_pushed = false};
        status = start_with_parameter(machine, frame, &start);
        if (status != SXML_STATUS_OK || start.has_pushed) {
            return status;
        }
        status = finish_with_parameter(lowerer, frame);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_parameter_map(
            lowerer,
            (map_building){.frame = frame, .base = locals->base_parameters, .is_tunnel = false},
            locals->parameters);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_parameter_map(
            lowerer,
            (map_building){.frame = frame, .base = locals->base_tunnel_parameters, .is_tunnel = true},
            locals->tunnel_parameters);
    }
    if (status == SXML_STATUS_OK) {
        /* 26.2.6: templates called in temporary output state stay in it
           (packet 4.13). */
        status = sxml_xslt_lowering_mark_temporary_output_state(
            lowerer,
            &(sxml_xslt_output_state_mark){
                .lowering = lowering, .node = locals->head.node, .block = frame->block},
            locals->tunnel_parameters);
    }
    sxml_ir_expression_index nothing = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, &nothing);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, nothing);
}

static const sxml_xpath_lowering_procedure parameter_maps_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:with-param"),
    .step = continue_parameter_maps,
    .locals_size = sizeof(parameter_maps_locals)};

/* What an invocation passes on (10.1.6): inside a template's body, the
   parameter maps it received; else the empty map. */
typedef struct inherited_maps {
    sxml_ir_atom parameters;
    sxml_ir_atom tunnel_parameters;
} inherited_maps;

static inherited_maps inherited_maps_of(const sxml_xslt_lowering *lowering) {
    const sxml_ir_atom empty_map = lowering->templates->empty_map;
    if (!is_in_template_function(lowering)) {
        return (inherited_maps){.parameters = empty_map, .tunnel_parameters = empty_map};
    }
    return (inherited_maps){.parameters = slot_atom(parameters_slot),
                            .tunnel_parameters = slot_atom(tunnel_parameters_slot)};
}

bool sxml_xslt_lowering_received_tunnel_parameters(const sxml_xslt_lowering *lowering,
                                                   sxml_ir_atom *tunnel_parameters) {
    if (!is_in_template_function(lowering)) {
        return false;
    }
    *tunnel_parameters = slot_atom(tunnel_parameters_slot);
    return true;
}

/* Pushes the parameter maps frame of instruction (its xsl:with-param
   children) over the given bases. */
typedef struct parameter_maps_request {
    const sxml_xslt_instruction_locals *head;
    sxml_xpath_lowering_frame *frame;
    sxml_ir_atom base_parameters;
    sxml_ir_atom base_tunnel_parameters;
    sxml_ir_atom *parameters;
    sxml_ir_atom *tunnel_parameters;
} parameter_maps_request;

static sxml_status push_parameter_maps(sxml_xpath_lowering_machine *machine,
                                       const parameter_maps_request *request) {
    const parameter_maps_locals initial = {.head = *request->head,
                                           .base_parameters = request->base_parameters,
                                           .base_tunnel_parameters =
                                               request->base_tunnel_parameters,
                                           .parameters = request->parameters,
                                           .tunnel_parameters = request->tunnel_parameters};
    sxml_xpath_lowering_frame *frame = NULL;
    const sxml_status status = sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &parameter_maps_procedure,
            .node = no_ast_node,
            .block = request->frame->block,
            .is_body = false,
            .destination = {.expression = NULL,
                            .atom = NULL,
                            .atom_block = NULL,
                            .focused = NULL},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof(initial)}},
        &frame);
    if (status == SXML_STATUS_OK) {
        /* Frames never move: its result goes into its own locals. */
        parameter_maps_locals *locals = frame->locals;
        frame->destination.expression = &locals->finished;
    }
    return status;
}

/* --------------------------------------------- xsl:apply-templates (6.6) */

typedef struct apply_templates_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom select;
    sxml_ir_atom parameters;
    sxml_ir_atom tunnel_parameters;
    sxml_ir_atom separator;
    bool has_separator;
    bool is_sorted;
} apply_templates_locals;

/* The mode of an xsl:apply-templates: its mode attribute or the default
   mode; NONE (0) for #current. */
static sxml_status resolve_apply_mode(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_node_index node, sxml_ir_mode_index *mode) {
    const sxml_xslt_ast_name_token_view tokens = mode_tokens_of(lowering, node);
    const mode_reference reference = tokens.name_token_count == 0
                                         ? default_mode_of(lowering, node)
                                         : reference_of(lowering, node, tokens.name_tokens[0]);
    if (reference.is_current) {
        *mode = (sxml_ir_mode_index){.value = 0};
        return SXML_STATUS_OK;
    }
    *mode = find_mode(lowering->templates, reference);
    if (mode->value == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

/* Parses the XPath text of kind once per lowering. */
static sxml_status ensure_snippet(const sxml_xslt_lowering *lowering, snippet_kind kind) {
    sxml_xslt_template_lowering *templates = lowering->templates;
    if (templates->snippets[kind] != NULL) {
        return SXML_STATUS_OK;
    }
    return sxml_xpath_build_ast(
        &(sxml_ast_build_request){.expression_text =
                                      sxml_byte_view_from_c_string(snippet_texts[kind]),
                                  .source_name = {.bytes = NULL, .byte_count = 0},
                                  .allocator = lowering->lowerer->allocator,
                                  .maximum_expression_depth = 0,
                                  .error_record = lowering->error_record},
        &templates->snippets[kind]);
}

static sxml_status continue_apply_templates(sxml_xpath_lowering_machine *machine,
                                            sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    apply_templates_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node_index node = locals->head.node;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_xslt_ast_attribute *select =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT);
        if (select->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
            return sxml_xslt_lowering_call_expression(
                machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                      .expression = select->value.payload.expression,
                                                      .block = frame->block,
                                                      .atom = &locals->select,
                                                      .focus = NULL});
        }
        const sxml_status status = ensure_snippet(lowering, SXML_SNIPPET_DEFAULT_SELECT);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* The snippet has no names to resolve: the lowerer's static
           context serves. */
        return sxml_xpath_lowering_call_embedded(
            machine,
            &(sxml_xpath_lowering_embedded_call){
                .embedding = {.ast = lowering->templates->snippets[SXML_SNIPPET_DEFAULT_SELECT],
                              .node = {.value = 0},
                              .static_context = NULL,
                              .focus = NULL},
                .block = frame->block,
                .is_body = false,
                .destination = {.expression = NULL,
                                .atom = &locals->select,
                                .atom_block = frame->block,
                                .focused = NULL}});
    }
    if (frame->step == 1 && !locals->is_sorted && sxml_xslt_lowering_has_sort(lowering, node)) {
        /* 13.3: the selected items are processed in sorted order. */
        locals->is_sorted = true;
        return sxml_xslt_lowering_call_sort(
            machine, &(sxml_xslt_sort_call){.lowering = lowering,
                                            .node = node,
                                            .input = locals->select,
                                            .sorts_groups = false,
                                            .block = frame->block,
                                            .atom = &locals->select});
    }
    if (frame->step == 1) {
        frame->step = 2;
        const inherited_maps inherited = inherited_maps_of(lowering);
        return push_parameter_maps(
            machine, &(parameter_maps_request){.head = &locals->head,
                                               .frame = frame,
                                               .base_parameters = lowering->templates->empty_map,
                                               .base_tunnel_parameters =
                                                   inherited.tunnel_parameters,
                                               .parameters = &locals->parameters,
                                               .tunnel_parameters = &locals->tunnel_parameters});
    }
    if (frame->step == 2) {
        frame->step = 3;
        const sxml_xslt_ast_attribute *separator =
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SEPARATOR);
        if (separator->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
            locals->has_separator = true;
            return sxml_xslt_lowering_call_value_template(
                machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                      .expression =
                                                          separator->value.payload.expression,
                                                      .block = frame->block,
                                                      .atom = &locals->separator,
                                                      .focus = NULL});
        }
    }
    sxml_status status = SXML_STATUS_OK;
    if (locals->has_separator) {
        /* 6.7: a text node of the separator's value. */
        status = bind_expression(lowerer, (expression_binding){.block = frame->block, .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                 .node_construction = {.value = locals->separator}}}, &locals->separator);
    }
    sxml_ir_mode_index mode = {0};
    if (status == SXML_STATUS_OK) {
        status = resolve_apply_mode(lowering, node, &mode);
    }
    sxml_ir_expression_index apply = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            lowerer,
            (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES,
                .apply_templates = {.select = locals->select,
                                    .mode = mode,
                                    .rule_search = SXML_IR_RULE_SEARCH_FROM_START,
                                    .parameters = locals->parameters,
                                    .tunnel_parameters = locals->tunnel_parameters,
                                    .separator = locals->has_separator
                                                     ? locals->separator
                                                     : (sxml_ir_atom){
                                                           .atom_kind = SXML_IR_ATOM_KIND_NONE}}},
            &apply);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, apply);
}

static const sxml_xpath_lowering_procedure apply_templates_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:apply-templates"),
    .step = continue_apply_templates,
    .locals_size = sizeof(apply_templates_locals)};

/* ----------------------------------- xsl:next-match, xsl:apply-imports */

typedef struct overriding_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom parameters;
    sxml_ir_atom tunnel_parameters;
} overriding_locals;

static sxml_status continue_overriding(sxml_xpath_lowering_machine *machine,
                                       sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    overriding_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (!is_in_template_body(lowering)) {
        /* 6.11: the current template rule is absent in a changed focus
           (xsl:for-each and the like) and outside templates. */
        sxml_ir_expression_index raise = {0};
        const sxml_status status = add_raise(
            lowerer,
            (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                         .code = "XTDE0560",
                         .description = "xsl:next-match or xsl:apply-imports is evaluated when "
                                        "there is no current template rule (XSLT 4.0, 6.11)"},
            &raise);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowering_finish_frame(machine, frame, raise);
    }
    if (frame->step == 0) {
        frame->step = 1;
        /* 6.11: with an effective version of 4.0 or more, the non-tunnel
           parameters received are passed on too. */
        const double version =
            sxml_xslt_ast_get_static_context(lowering->ast,
                                             node_of(lowering, locals->head.node)->static_context)
                ->version;
        return push_parameter_maps(
            machine, &(parameter_maps_request){.head = &locals->head,
                                               .frame = frame,
                                               .base_parameters =
                                                   version >= 4.0
                                                       ? slot_atom(parameters_slot)
                                                       : lowering->templates->empty_map,
                                               .base_tunnel_parameters =
                                                   slot_atom(tunnel_parameters_slot),
                                               .parameters = &locals->parameters,
                                               .tunnel_parameters = &locals->tunnel_parameters});
    }
    const bool is_next_match =
        node_of(lowering, locals->head.node)->kind == SXML_XSLT_AST_KIND_NEXT_MATCH;
    sxml_ir_expression_index apply = {0};
    const sxml_status status = add_expression(
        lowerer,
        (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES,
            .apply_templates = {.select = {.atom_kind = SXML_IR_ATOM_KIND_NONE},
                                .mode = {.value = 0},
                                .rule_search = is_next_match
                                                   ? SXML_IR_RULE_SEARCH_AFTER_CURRENT_RULE
                                                   : SXML_IR_RULE_SEARCH_IMPORTED_ONLY,
                                .parameters = locals->parameters,
                                .tunnel_parameters = locals->tunnel_parameters,
                                .separator = {.atom_kind = SXML_IR_ATOM_KIND_NONE}}},
        &apply);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, apply);
}

static const sxml_xpath_lowering_procedure overriding_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:next-match"),
    .step = continue_overriding,
    .locals_size = sizeof(overriding_locals)};

/* ------------------------------------------- xsl:call-template (10.1.2) */

typedef struct call_template_locals {
    sxml_xslt_instruction_locals head;
    const template_entry *target;
    sxml_ir_atom parameters;
    sxml_ir_atom tunnel_parameters;
} call_template_locals;

/* XTSE0680 and XTSE0690 (10.1.5, 9.2.2) between a call and its target. */
static sxml_status check_call_parameters(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index call,
                                         const template_entry *target) {
    const double version = sxml_xslt_ast_get_static_context(
                               lowering->ast, node_of(lowering, call)->static_context)
                               ->version;
    for (sxml_xslt_ast_node_index with_parameter_node = node_of(lowering, call)->first_child; with_parameter_node.value != 0;
         with_parameter_node = node_of(lowering, with_parameter_node)->next_sibling) {
        if (node_of(lowering, with_parameter_node)->kind != SXML_XSLT_AST_KIND_WITH_PARAM ||
            boolean_of(lowering, (boolean_query){.node = with_parameter_node,
                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_TUNNEL,
                                                 .fallback = false})) {
            continue;
        }
        bool is_declared = false;
        for (sxml_xslt_ast_node_index parameter = node_of(lowering, target->node)->first_child;
             parameter.value != 0; parameter = node_of(lowering, parameter)->next_sibling) {
            is_declared = is_declared ||
                          (node_of(lowering, parameter)->kind == SXML_XSLT_AST_KIND_PARAM &&
                           !boolean_of(lowering, (boolean_query){.node = parameter,
                                                                 .kind =
                                                                     SXML_XSLT_AST_ATTRIBUTE_TUNNEL,
                                                                 .fallback = false}) &&
                           name_of(lowering, parameter).index == name_of(lowering, with_parameter_node).index);
        }
        if (!is_declared && version >= 2.0) {
            return report_static_error(lowering, with_parameter_node, SXML_BYTE_VIEW_FROM_LITERAL("XTSE0680 xsl:call-template passes a non-tunnel parameter "
                                       "the template does not declare (XSLT 4.0, 10.1.5)"));
        }
    }
    for (sxml_xslt_ast_node_index parameter = node_of(lowering, target->node)->first_child;
         parameter.value != 0; parameter = node_of(lowering, parameter)->next_sibling) {
        if (node_of(lowering, parameter)->kind != SXML_XSLT_AST_KIND_PARAM ||
            boolean_of(lowering, (boolean_query){.node = parameter,
                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_TUNNEL,
                                                 .fallback = false}) ||
            !boolean_of(lowering, (boolean_query){.node = parameter,
                                                  .kind = SXML_XSLT_AST_ATTRIBUTE_REQUIRED,
                                                  .fallback = false})) {
            continue;
        }
        bool is_supplied = false;
        for (sxml_xslt_ast_node_index with_parameter_node = node_of(lowering, call)->first_child; with_parameter_node.value != 0;
             with_parameter_node = node_of(lowering, with_parameter_node)->next_sibling) {
            is_supplied = is_supplied ||
                          (node_of(lowering, with_parameter_node)->kind == SXML_XSLT_AST_KIND_WITH_PARAM &&
                           !boolean_of(lowering, (boolean_query){.node = with_parameter_node,
                                                                 .kind =
                                                                     SXML_XSLT_AST_ATTRIBUTE_TUNNEL,
                                                                 .fallback = false}) &&
                           name_of(lowering, with_parameter_node).index == name_of(lowering, parameter).index);
        }
        if (!is_supplied) {
            return report_static_error(lowering, call, SXML_BYTE_VIEW_FROM_LITERAL("XTSE0690 xsl:call-template supplies no value for a "
                                       "mandatory parameter of the template (XSLT 4.0, 9.2.2)"));
        }
    }
    return SXML_STATUS_OK;
}

/* The atoms of the caller's focus for a call (10.1.2: the focus does not
   change): the empty sequence and 0 for an absent focus. */
typedef struct focus_atoms {
    sxml_ir_atom item;
    sxml_ir_atom position;
    sxml_ir_atom size;
} focus_atoms;

static sxml_status bind_focus_atoms(sxml_xpath_lowerer *lowerer, sxml_xpath_lowering_block *block,
                                    focus_atoms *atoms) {
    if (lowerer->function->focus->kind == SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT) {
        sxml_status status = bind_empty_sequence(lowerer, block, &atoms->item);
        if (status == SXML_STATUS_OK) {
            status = add_integer_constant(lowerer, 0, &atoms->position);
        }
        atoms->size = atoms->position;
        return status;
    }
    const sxml_xpath_lowering_focus_part parts[3] = {SXML_XPATH_LOWERING_FOCUS_PART_VALUE,
                                                     SXML_XPATH_LOWERING_FOCUS_PART_POSITION,
                                                     SXML_XPATH_LOWERING_FOCUS_PART_SIZE};
    sxml_ir_atom *targets[3] = {&atoms->item, &atoms->position, &atoms->size};
    sxml_status status = SXML_STATUS_OK;
    for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < 3; part_index += 1) {
        status = sxml_xpath_lowerer_read_focus(
            lowerer,
            (sxml_xpath_lowering_focus_read){
                .block = block, .node = no_ast_node, .part = parts[part_index]},
            targets[part_index]);
    }
    return status;
}

/* A STATIC_CALL of a template function, bound in block. */
typedef struct template_call {
    sxml_xpath_lowering_block *block;
    sxml_ir_function_index function;
    focus_atoms focus;
    sxml_ir_atom parameters;
    sxml_ir_atom tunnel_parameters;
} template_call;

static sxml_status add_template_call(sxml_xpath_lowerer *lowerer, const template_call *call,
                                     sxml_ir_expression_index *expression) {
    const sxml_ir_atom arguments[TEMPLATE_PARAMETER_COUNT] = {
        call->focus.item, call->focus.position, call->focus.size, call->parameters,
        call->tunnel_parameters};
    sxml_ir_atom_list list = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(
        lowerer->unit,
        (sxml_ir_atom_view){.atoms = arguments, .atom_count = TEMPLATE_PARAMETER_COUNT}, &list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        lowerer,
        (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
            .static_call = {.target = {.target_kind = SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                       .function = call->function},
                            .arguments = list}},
        expression);
}

/* The named template an xsl:call-template binds to (3.4.5.5): its
   package's, through the packages; xsl:original in an overriding template
   the overridden one (3.4.5.4). NULL for none. */
static const template_entry *bound_template(const sxml_xslt_lowering *lowering,
                                            sxml_xslt_ast_node_index call) {
    const sxml_name_identifier name = name_of(lowering, call);
    sxml_name_identifier original = {.index = 0};
    sxml_xslt_component_binding binding = {.declaration = {.value = 0}};
    if (sxml_name_pool_find_name(
            lowering->lowerer->name_pool,
            &(sxml_name_request){.namespace_uri =
                                     sxml_well_known_namespace_identifier(SXML_NAMESPACE_XSLT),
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("original")},
            &original) &&
        original.index == name.index) {
        const sxml_xslt_ast_node_index overriding =
            sxml_xslt_package_overriding_ancestor_of(lowering, call);
        if (node_of(lowering, overriding)->kind != SXML_XSLT_AST_KIND_TEMPLATE) {
            return NULL;
        }
        binding = sxml_xslt_package_bind_original(lowering, overriding);
    } else {
        binding = sxml_xslt_package_bind(
            lowering,
            &(sxml_xslt_component_reference){.component_kind = SXML_XSLT_COMPONENT_KIND_TEMPLATE,
                                             .component_name = name,
                                             .arity = 0,
                                             .package = sxml_xslt_package_of_node(lowering, call)});
    }
    if (binding.declaration.value == 0) {
        return NULL;
    }
    return find_template_entry(lowering->templates, binding.declaration);
}

static sxml_status continue_call_template(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    call_template_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        locals->target = bound_template(lowering, locals->head.node);
        if (locals->target == NULL) {
            return report_static_error(lowering, locals->head.node, SXML_BYTE_VIEW_FROM_LITERAL("XTSE0650 xsl:call-template names no template of the "
                                       "stylesheet (XSLT 4.0, 10.1.2)"));
        }
        const sxml_status status = check_call_parameters(lowering, locals->head.node, locals->target);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const inherited_maps inherited = inherited_maps_of(lowering);
        return push_parameter_maps(
            machine, &(parameter_maps_request){.head = &locals->head,
                                               .frame = frame,
                                               .base_parameters = lowering->templates->empty_map,
                                               .base_tunnel_parameters =
                                                   inherited.tunnel_parameters,
                                               .parameters = &locals->parameters,
                                               .tunnel_parameters = &locals->tunnel_parameters});
    }
    focus_atoms focus = {{0}, {0}, {0}};
    sxml_status status = bind_focus_atoms(lowerer, frame->block, &focus);
    sxml_ir_expression_index call = {0};
    if (status == SXML_STATUS_OK) {
        const bool is_focus_absent =
            lowerer->function->focus->kind == SXML_XPATH_LOWERING_FOCUS_KIND_ABSENT;
        status = add_template_call(lowerer,
                                   &(template_call){.block = frame->block,
                                                    .function = is_focus_absent
                                                                    ? locals->target->absent_function
                                                                    : locals->target->function,
                                                    .focus = focus,
                                                    .parameters = locals->parameters,
                                                    .tunnel_parameters = locals->tunnel_parameters},
                                   &call);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, call);
}

static const sxml_xpath_lowering_procedure call_template_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:call-template"),
    .step = continue_call_template,
    .locals_size = sizeof(call_template_locals)};

/* ================================================================ Rules */

static const sxml_xslt_instruction_rule template_rules[] = {
    {.kind = SXML_XSLT_AST_KIND_APPLY_TEMPLATES, .procedure = &apply_templates_procedure},
    {.kind = SXML_XSLT_AST_KIND_CALL_TEMPLATE, .procedure = &call_template_procedure},
    {.kind = SXML_XSLT_AST_KIND_NEXT_MATCH, .procedure = &overriding_procedure},
    {.kind = SXML_XSLT_AST_KIND_APPLY_IMPORTS, .procedure = &overriding_procedure},
};

const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_template = {
    .rules = template_rules, .rule_count = sizeof(template_rules) / sizeof(template_rules[0])};


/* ======================================================= Collection */

/* count elements of element_size bytes from the lowerer's allocator,
   zeroed. */
typedef struct zeroed_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} zeroed_request;

static sxml_status allocate_zeroed(sxml_allocator allocator, zeroed_request request,
                                   void **memory) {
    /* NULL after a failure, so a caller's release of it is harmless. */
    *memory = NULL;
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(request.element_count, request.element_size, &byte_count);
    if (status == SXML_STATUS_OK && byte_count == 0) {
        byte_count = request.element_size;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = request.element_alignment},
            memory);
    }
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, byte_count);
    }
    return status;
}

/* The index in templates->modes of the mode named name, added when it is
   not there yet. */
static sxml_status add_mode(const sxml_xslt_lowering *lowering, mode_reference reference,
                            size_t *mode_position) {
    sxml_xslt_template_lowering *templates = lowering->templates;
    const sxml_ir_mode_index existing = find_mode(templates, reference);
    if (existing.value != 0) {
        *mode_position = existing.value - 1U;
        return SXML_STATUS_OK;
    }
    if (templates->mode_count == templates->mode_capacity) {
        const size_t capacity = templates->mode_capacity < 8 ? 8 : templates->mode_capacity * 2;
        void *memory = NULL;
        const sxml_status status = allocate_zeroed(
            lowering->lowerer->allocator,
            (zeroed_request){.element_count = capacity,
                             .element_size = sizeof(mode_entry),
                             .element_alignment = alignof(mode_entry)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (templates->mode_count > 0) {
            memcpy(memory, templates->modes, templates->mode_count * sizeof(mode_entry));
        }
        sxml_allocator_release(lowering->lowerer->allocator, templates->modes);
        templates->modes = memory;
        templates->mode_capacity = capacity;
    }
    templates->modes[templates->mode_count] =
        (mode_entry){.mode_name = reference.mode_name, .package = reference.package};
    *mode_position = templates->mode_count;
    templates->mode_count += 1;
    return SXML_STATUS_OK;
}

/* Sets property to value from a declaration of import_precedence
   (6.9.1). */
typedef struct property_setting {
    uint32_t value;
    uint32_t import_precedence;
    sxml_xslt_ast_node_index node;
} property_setting;

static void set_property(mode_property *property, property_setting setting) {
    if (!property->is_set || setting.import_precedence > property->import_precedence) {
        *property = (mode_property){.is_set = true,
                                    .value = setting.value,
                                    .import_precedence = setting.import_precedence};
    }
}

static uint32_t built_in_rule_of(sxml_xslt_ast_keyword keyword) {
    switch ((uint32_t)keyword) {
    case SXML_XSLT_AST_KEYWORD_DEEP_COPY:
        return (uint32_t)SXML_IR_BUILT_IN_RULE_DEEP_COPY;
    case SXML_XSLT_AST_KEYWORD_SHALLOW_COPY:
    case SXML_XSLT_AST_KEYWORD_SHALLOW_COPY_ALL:
        return (uint32_t)SXML_IR_BUILT_IN_RULE_SHALLOW_COPY;
    case SXML_XSLT_AST_KEYWORD_DEEP_SKIP:
        return (uint32_t)SXML_IR_BUILT_IN_RULE_DEEP_SKIP;
    case SXML_XSLT_AST_KEYWORD_SHALLOW_SKIP:
        return (uint32_t)SXML_IR_BUILT_IN_RULE_SHALLOW_SKIP;
    case SXML_XSLT_AST_KEYWORD_FAIL:
        return (uint32_t)SXML_IR_BUILT_IN_RULE_FAIL;
    default:
        return (uint32_t)SXML_IR_BUILT_IN_RULE_TEXT_ONLY_COPY;
    }
}

/* One xsl:mode declaration (6.9.1, 6.9.6). */
static sxml_status read_mode_declaration(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index declaration) {
    if (is_reserved_name(lowering, (reserved_name_query){.name = name_of(lowering, declaration),
                                                         .is_template_name = false})) {
        return report_reserved_name(lowering, declaration);
    }
    size_t position = 0;
    sxml_status status = add_mode(lowering, declared_mode(lowering, declaration), &position);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    mode_entry *mode = &lowering->templates->modes[position];
    mode->is_declared = true;
    const uint32_t precedence = (uint32_t)precedence_of(lowering, declaration);
    for (sxml_xslt_ast_node_index child = node_of(lowering, declaration)->first_child;
         child.value != 0; child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_TEMPLATE) {
            mode->is_enclosing = true;
        }
    }
    const sxml_xslt_ast_attribute *visibility =
        find_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_VISIBILITY);
    if (mode->mode_name.index == 0 && visibility->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD &&
        visibility->value.payload.keyword != SXML_XSLT_AST_KEYWORD_PRIVATE) {
        return report_static_error(lowering, declaration,
                                   SXML_BYTE_VIEW_FROM_LITERAL(
                                       "XTSE0020 the unnamed mode is declared with a visibility "
                                       "other than private (XSLT 4.0, 6.9.1)"));
    }
    if (mode->is_enclosing && mode->mode_name.index == 0) {
        return report_static_error(lowering, declaration, SXML_BYTE_VIEW_FROM_LITERAL("XTSE4005 an xsl:mode with xsl:template children has no name "
                                   "(XSLT 4.0, 6.9.6)"));
    }
    if (find_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_AS)->kind !=
            SXML_XSLT_AST_ATTRIBUTE_NONE &&
        (mode->as_declaration.value == 0 || precedence > mode->as_precedence)) {
        mode->as_declaration = declaration;
        mode->as_precedence = precedence;
    }
    const sxml_xslt_ast_attribute *on_no_match =
        find_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_ON_NO_MATCH);
    if (on_no_match->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD) {
        set_property(&mode->on_no_match,
                     (property_setting){.value = built_in_rule_of(on_no_match->value.payload.keyword),
                                        .import_precedence = precedence,
                                        .node = declaration});
    }
    const sxml_xslt_ast_attribute *on_multiple_match =
        find_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_ON_MULTIPLE_MATCH);
    if (on_multiple_match->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD) {
        set_property(
            &mode->fails_on_multiple_match,
            (property_setting){.value = on_multiple_match->value.payload.keyword ==
                                                SXML_XSLT_AST_KEYWORD_FAIL
                                            ? 1U
                                            : 0U,
                               .import_precedence = precedence,
                               .node = declaration});
    }
    const sxml_xslt_ast_attribute *copy_namespaces =
        find_attribute(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_COPY_NAMESPACES);
    if (copy_namespaces->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN) {
        set_property(&mode->copies_namespaces,
                     (property_setting){.value = copy_namespaces->value.payload.is_true ? 1U : 0U,
                                        .import_precedence = precedence,
                                        .node = declaration});
    }
    return SXML_STATUS_OK;
}

/* Whether node's package requires its modes to be declared (3.4.6.1): an
   xsl:package with declared-modes="yes" (the default there); a mode
   accepted from a used package is declared there. */
static bool requires_declared_modes(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_node_index root =
        sxml_xslt_package_get_root(lowering, sxml_xslt_package_of_node(lowering, node));
    if (node_of(lowering, root)->kind != SXML_XSLT_AST_KIND_PACKAGE) {
        return false;
    }
    return boolean_of(lowering, (boolean_query){.node = root,
                                                .kind = SXML_XSLT_AST_ATTRIBUTE_DECLARED_MODES,
                                                .fallback = true});
}

/* A mode a template, apply-templates or default-mode refers to: added
   (implicitly declared, 6.9.1), or XTSE3085 when modes must be
   declared. */
static sxml_status refer_to_mode(const sxml_xslt_lowering *lowering, mode_reference reference,
                                 sxml_xslt_ast_node_index node) {
    if (reference.is_current || reference.is_all) {
        return SXML_STATUS_OK;
    }
    if (is_reserved_name(lowering, (reserved_name_query){.name = reference.mode_name,
                                                         .is_template_name = false})) {
        return report_reserved_name(lowering, node);
    }
    size_t position = 0;
    const sxml_status status = add_mode(lowering, reference, &position);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!lowering->templates->modes[position].is_declared &&
        requires_declared_modes(lowering, node)) {
        return report_static_error(lowering, node, SXML_BYTE_VIEW_FROM_LITERAL("XTSE3085 a package with declared-modes=\"yes\" uses a mode it "
                                   "does not declare (XSLT 4.0, 3.4.6.1)"));
    }
    return SXML_STATUS_OK;
}

/* The modes every node of the stylesheet refers to. */
static sxml_status collect_mode_references(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_ast *ast = lowering->ast;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t node_number = 1; status == SXML_STATUS_OK && node_number < ast->node_count;
         node_number += 1) {
        const sxml_xslt_ast_node_index node = {.value = node_number};
        const sxml_xslt_ast_kind kind = node_of(lowering, node)->kind;
        const bool is_template_rule =
            kind == SXML_XSLT_AST_KIND_TEMPLATE &&
            find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_MATCH)->kind !=
                SXML_XSLT_AST_ATTRIBUTE_NONE &&
            node_of(lowering, node_of(lowering, node)->parent)->kind != SXML_XSLT_AST_KIND_MODE;
        if (!is_template_rule && kind != SXML_XSLT_AST_KIND_APPLY_TEMPLATES) {
            continue;
        }
        const sxml_xslt_ast_name_token_view tokens = mode_tokens_of(lowering, node);
        if (tokens.name_token_count == 0) {
            status = refer_to_mode(lowering, default_mode_of(lowering, node), node);
        }
        for (size_t token_index = 0;
             status == SXML_STATUS_OK && token_index < tokens.name_token_count; token_index += 1) {
            status = refer_to_mode(lowering,
                                   reference_of(lowering, node, tokens.name_tokens[token_index]),
                                   node);
        }
    }
    for (uint32_t context_number = 1;
         status == SXML_STATUS_OK && context_number < ast->static_context_count;
         context_number += 1) {
        const sxml_xslt_ast_static_context *context = sxml_xslt_ast_get_static_context(
            ast, (sxml_xslt_ast_static_context_index){.value = context_number});
        if (context->default_mode.kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME) {
            size_t position = 0;
            const uint32_t package = sxml_xslt_package_of_static_context(
                lowering, (sxml_xslt_ast_static_context_index){.value = context_number});
            const sxml_xslt_ast_node_index root = sxml_xslt_package_get_root(lowering, package);
            status = add_mode(lowering, named_mode(lowering, root, context->default_mode.name),
                              &position);
        }
    }
    return status;
}

/* The attributes of xsl:mode whose values two declarations of one mode
   must not set differently at one import precedence (6.9.1, XTSE0545). */
static const sxml_xslt_ast_attribute_kind mode_property_kinds[] = {
    SXML_XSLT_AST_ATTRIBUTE_AS,
    SXML_XSLT_AST_ATTRIBUTE_STREAMABLE,
    SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS,
    SXML_XSLT_AST_ATTRIBUTE_ON_NO_MATCH,
    SXML_XSLT_AST_ATTRIBUTE_ON_MULTIPLE_MATCH,
    SXML_XSLT_AST_ATTRIBUTE_WARNING_ON_NO_MATCH,
    SXML_XSLT_AST_ATTRIBUTE_WARNING_ON_MULTIPLE_MATCH,
    SXML_XSLT_AST_ATTRIBUTE_TYPED,
    SXML_XSLT_AST_ATTRIBUTE_COPY_NAMESPACES,
    SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
};

/* True when two attribute values differ (the kinds whose payload is a
   plain value; expressions and name lists compare equal). */
static bool values_differ(const sxml_xslt_lowering *lowering, const sxml_xslt_ast_value *first,
                          const sxml_xslt_ast_value *second) {
    if (first->kind != second->kind) {
        return true;
    }
    switch ((uint32_t)first->kind) {
    case SXML_XSLT_AST_VALUE_KIND_BOOLEAN:
        return first->payload.is_true != second->payload.is_true;
    case SXML_XSLT_AST_VALUE_KIND_KEYWORD:
        return first->payload.keyword != second->payload.keyword;
    case SXML_XSLT_AST_VALUE_KIND_NAME:
        return first->payload.name.index != second->payload.name.index;
    case SXML_XSLT_AST_VALUE_KIND_TEXT:
        return !sxml_byte_view_equals(sxml_xslt_ast_get_text(lowering->ast, first->payload.text),
                                      sxml_xslt_ast_get_text(lowering->ast, second->payload.text));
    default:
        return false;
    }
}

/* True when a declaration of mode name of a precedence above precedence
   sets the attribute of kind. */
typedef struct mode_attribute_query {
    sxml_name_identifier mode_name;
    uint32_t package;
    uint32_t precedence;
    sxml_xslt_ast_attribute_kind kind;
} mode_attribute_query;

static bool is_overridden(const sxml_xslt_lowering *lowering, mode_attribute_query query) {
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(lowering->ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index other = sxml_xslt_ast_get_declaration(lowering->ast, order);
        if (node_of(lowering, other)->kind == SXML_XSLT_AST_KIND_MODE &&
            name_of(lowering, other).index == query.mode_name.index &&
            sxml_xslt_package_of_node(lowering, other) == query.package &&
            (uint32_t)precedence_of(lowering, other) > query.precedence &&
            find_attribute(lowering, other, query.kind)->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
            return true;
        }
    }
    return false;
}

/* XTSE0545: two declarations of one mode and precedence setting an
   attribute differently, unless one of higher precedence sets it. */
static sxml_status check_mode_conflicts(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_ast *ast = lowering->ast;
    for (uint32_t first_order = 1; sxml_xslt_ast_get_declaration(ast, first_order).value != 0;
         first_order += 1) {
        const sxml_xslt_ast_node_index first = sxml_xslt_ast_get_declaration(ast, first_order);
        if (node_of(lowering, first)->kind != SXML_XSLT_AST_KIND_MODE) {
            continue;
        }
        for (uint32_t second_order = first_order + 1;
             sxml_xslt_ast_get_declaration(ast, second_order).value != 0; second_order += 1) {
            const sxml_xslt_ast_node_index second =
                sxml_xslt_ast_get_declaration(ast, second_order);
            if (node_of(lowering, second)->kind != SXML_XSLT_AST_KIND_MODE ||
                name_of(lowering, second).index != name_of(lowering, first).index ||
                !sxml_xslt_package_is_same(lowering, first, second) ||
                precedence_of(lowering, second) != precedence_of(lowering, first)) {
                continue;
            }
            for (size_t kind_index = 0;
                 kind_index < sizeof(mode_property_kinds) / sizeof(mode_property_kinds[0]);
                 kind_index += 1) {
                const sxml_xslt_ast_attribute *first_value =
                    find_attribute(lowering, first, mode_property_kinds[kind_index]);
                const sxml_xslt_ast_attribute *second_value =
                    find_attribute(lowering, second, mode_property_kinds[kind_index]);
                if (first_value->kind == SXML_XSLT_AST_ATTRIBUTE_NONE ||
                    second_value->kind == SXML_XSLT_AST_ATTRIBUTE_NONE ||
                    !values_differ(lowering, &first_value->value, &second_value->value) ||
                    is_overridden(lowering,
                                  (mode_attribute_query){
                                      .mode_name = name_of(lowering, first),
                                      .package = sxml_xslt_package_of_node(lowering, first),
                                      .precedence = (uint32_t)precedence_of(lowering, first),
                                      .kind = mode_property_kinds[kind_index]})) {
                    continue;
                }
                return report_static_error(
                    lowering, second,
                    SXML_BYTE_VIEW_FROM_LITERAL(
                        "XTSE0545 two xsl:mode declarations of the same import precedence give "
                        "a mode conflicting values (XSLT 4.0, 6.9.1)"));
            }
        }
    }
    return SXML_STATUS_OK;
}

/* For each module, the lowest import precedence of the modules below it
   in the module tree (its own when none): an included module's
   precedence is its includer's (3.10.2), so a stylesheet level's value
   is that of the levels it imports (6.11). */
static sxml_status compute_lowest_precedences(const sxml_xslt_lowering *lowering,
                                              int32_t **lowest) {
    const sxml_xslt_ast *ast = lowering->ast;
    void *memory = NULL;
    const sxml_status status = allocate_zeroed(
        lowering->lowerer->allocator,
        (zeroed_request){.element_count = ast->module_count + 1U,
                         .element_size = sizeof(int32_t),
                         .element_alignment = alignof(int32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    int32_t *values = memory;
    for (uint32_t module_number = 1; module_number < ast->module_count; module_number += 1) {
        values[module_number] = (int32_t)sxml_xslt_ast_get_module(
                                    ast, (sxml_xslt_ast_module_index){.value = module_number})
                                    ->import_precedence;
    }
    for (uint32_t module_number = 1; module_number < ast->module_count; module_number += 1) {
        const int32_t precedence = values[module_number];
        sxml_xslt_ast_module_index ancestor =
            sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = module_number})
                ->parent_module;
        /* The module tree has at most maximum_module_count levels. */
        for (uint32_t level = 0; ancestor.value != 0 && level < ast->module_count; level += 1) {
            if (precedence < values[ancestor.value]) {
                values[ancestor.value] = precedence;
            }
            ancestor = sxml_xslt_ast_get_module(ast, ancestor)->parent_module;
        }
    }
    *lowest = values;
    return SXML_STATUS_OK;
}

/* The top module of the stylesheet level of module (3.10.2). */
static sxml_xslt_ast_module_index level_of(const sxml_xslt_ast *ast,
                                           sxml_xslt_ast_module_index module) {
    for (uint32_t level = 0; level < ast->module_count; level += 1) {
        const sxml_xslt_ast_module *entry = sxml_xslt_ast_get_module(ast, module);
        if (entry->relation != SXML_XSLT_AST_MODULE_RELATION_INCLUDED) {
            break;
        }
        module = entry->parent_module;
    }
    return module;
}

/* Every xsl:template: declarations and the children of enclosing
   modes. */
static sxml_status collect_templates(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_ast *ast = lowering->ast;
    sxml_xslt_template_lowering *templates = lowering->templates;
    size_t template_count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        const sxml_xslt_ast_kind kind = node_of(lowering, declaration)->kind;
        template_count += kind == SXML_XSLT_AST_KIND_TEMPLATE ? 1U : 0U;
        if (kind == SXML_XSLT_AST_KIND_MODE) {
            for (sxml_xslt_ast_node_index child = node_of(lowering, declaration)->first_child;
                 child.value != 0; child = node_of(lowering, child)->next_sibling) {
                template_count +=
                    node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_TEMPLATE ? 1U : 0U;
            }
        }
    }
    int32_t *lowest = NULL;
    sxml_status status = compute_lowest_precedences(lowering, &lowest);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(lowering->lowerer->allocator,
                                 (zeroed_request){.element_count = template_count,
                                                  .element_size = sizeof(template_entry),
                                                  .element_alignment = alignof(template_entry)},
                                 &memory);
    }
    templates->templates = memory;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        const sxml_xslt_ast_kind kind = node_of(lowering, declaration)->kind;
        sxml_xslt_ast_node_index candidate =
            kind == SXML_XSLT_AST_KIND_MODE ? node_of(lowering, declaration)->first_child
                                            : declaration;
        while (candidate.value != 0) {
            const sxml_xslt_ast_node_index node = candidate;
            candidate = kind == SXML_XSLT_AST_KIND_MODE ? node_of(lowering, node)->next_sibling
                                                        : (sxml_xslt_ast_node_index){.value = 0};
            if (node_of(lowering, node)->kind != SXML_XSLT_AST_KIND_TEMPLATE) {
                continue;
            }
            const sxml_xslt_ast_attribute *match =
                find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_MATCH);
            template_entry *entry = &templates->templates[templates->template_count];
            templates->template_count += 1;
            *entry = (template_entry){
                .node = node,
                .template_name = name_of(lowering, node),
                .pattern = match->value.kind == SXML_XSLT_AST_VALUE_KIND_PATTERN
                               ? match->value.payload.expression
                               : (sxml_xslt_ast_expression_index){.value = 0},
                .enclosing_mode = kind == SXML_XSLT_AST_KIND_MODE
                                      ? declaration
                                      : (sxml_xslt_ast_node_index){.value = 0},
                .import_precedence = precedence_of(lowering, node),
                .lowest_imported_precedence =
                    lowest[level_of(ast, node_of(lowering, node)->module).value],
                .declaration_order = order};
            if (is_reserved_name(lowering, (reserved_name_query){.name = entry->template_name,
                                                                 .is_template_name = true})) {
                status = report_reserved_name(lowering, node);
                break;
            }
            if (entry->enclosing_mode.value != 0 &&
                (entry->pattern.value == 0 || entry->template_name.index != 0 ||
                 find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_MODE)->kind !=
                     SXML_XSLT_AST_ATTRIBUTE_NONE)) {
                status = report_static_error(lowering, node, SXML_BYTE_VIEW_FROM_LITERAL("XTSE4010 an xsl:template in an xsl:mode has a name "
                                             "or mode attribute, or no match attribute (XSLT "
                                             "4.0, 6.9.6)"));
                break;
            }
            status = sxml_ir_unit_add_function(
                lowering->lowerer->unit,
                &(sxml_ir_function){.signature = templates->template_signature,
                                    .parameter_count = TEMPLATE_PARAMETER_COUNT},
                &entry->function);
            if (status == SXML_STATUS_OK && entry->template_name.index != 0) {
                status = sxml_ir_unit_add_function(
                    lowering->lowerer->unit,
                    &(sxml_ir_function){.signature = templates->template_signature,
                                        .parameter_count = TEMPLATE_PARAMETER_COUNT},
                    &entry->absent_function);
            }
        }
    }
    sxml_allocator_release(lowering->lowerer->allocator, lowest);
    return status;
}

/* XTSE0660: two templates of one name and import precedence, unless
   one of higher precedence has the name (10.1.1). */
static sxml_status check_template_names(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_template_lowering *templates = lowering->templates;
    for (size_t first = 0; first < templates->template_count; first += 1) {
        const template_entry *entry = &templates->templates[first];
        if (entry->template_name.index == 0) {
            continue;
        }
        const template_entry *winner = find_named_template(
            lowering, (template_key){.template_name = entry->template_name,
                                     .package = sxml_xslt_package_of_node(lowering, entry->node)});
        if (winner->import_precedence != entry->import_precedence) {
            continue;
        }
        for (size_t second = first + 1; second < templates->template_count; second += 1) {
            const template_entry *other = &templates->templates[second];
            if (other->template_name.index == entry->template_name.index &&
                other->import_precedence == entry->import_precedence &&
                sxml_xslt_package_is_same(lowering, other->node, entry->node)) {
                return report_static_error(lowering, other->node, SXML_BYTE_VIEW_FROM_LITERAL("XTSE0660 two templates of the same import precedence "
                                           "have the same name (XSLT 4.0, 10.1.1)"));
            }
        }
    }
    return SXML_STATUS_OK;
}

/* =================================================== Built-in templates */

typedef enum built_in_action {
    SXML_BUILT_IN_ACTION_NOTHING,
    SXML_BUILT_IN_ACTION_ITEM,
    SXML_BUILT_IN_ACTION_APPLY_CHILDREN,
    SXML_BUILT_IN_ACTION_APPLY_JNODE_CHILDREN,
    SXML_BUILT_IN_ACTION_APPLY_MEMBERS,
    SXML_BUILT_IN_ACTION_APPLY_ATTRIBUTES_AND_CHILDREN,
    SXML_BUILT_IN_ACTION_TEXT,
    SXML_BUILT_IN_ACTION_SHALLOW_COPY,
    SXML_BUILT_IN_ACTION_DEEP_COPY,
    SXML_BUILT_IN_ACTION_COPY_NODE,
    SXML_BUILT_IN_ACTION_FAIL,
} built_in_action;

/* One branch of a built-in template: when condition, action. */
typedef struct built_in_case {
    snippet_kind condition;
    built_in_action action;
} built_in_case;

/* The branches of each built-in rule (6.10.1 to 6.10.6) and what is done
   for every other item. */
typedef struct built_in_plan {
    built_in_case cases[4];
    size_t case_count;
    built_in_action otherwise;
} built_in_plan;

static built_in_plan plan_of(sxml_ir_built_in_rule rule) {
    switch (rule) {
    case SXML_IR_BUILT_IN_RULE_TEXT_ONLY_COPY:
        return (built_in_plan){
            .cases = {{SXML_SNIPPET_IS_DOCUMENT_OR_ELEMENT, SXML_BUILT_IN_ACTION_APPLY_CHILDREN},
                      {SXML_SNIPPET_IS_JNODE, SXML_BUILT_IN_ACTION_APPLY_JNODE_CHILDREN},
                      {SXML_SNIPPET_IS_TEXT_OR_ATTRIBUTE_OR_ATOMIC, SXML_BUILT_IN_ACTION_TEXT},
                      {SXML_SNIPPET_IS_ARRAY, SXML_BUILT_IN_ACTION_APPLY_MEMBERS}},
            .case_count = 4,
            .otherwise = SXML_BUILT_IN_ACTION_NOTHING};
    case SXML_IR_BUILT_IN_RULE_SHALLOW_COPY:
        return (built_in_plan){
            .cases = {{SXML_SNIPPET_IS_DOCUMENT_OR_ELEMENT, SXML_BUILT_IN_ACTION_SHALLOW_COPY},
                      {SXML_SNIPPET_IS_XNODE, SXML_BUILT_IN_ACTION_COPY_NODE}},
            .case_count = 2,
            .otherwise = SXML_BUILT_IN_ACTION_ITEM};
    case SXML_IR_BUILT_IN_RULE_DEEP_COPY:
        return (built_in_plan){
            .cases = {{SXML_SNIPPET_IS_DOCUMENT_OR_ELEMENT, SXML_BUILT_IN_ACTION_DEEP_COPY},
                      {SXML_SNIPPET_IS_XNODE, SXML_BUILT_IN_ACTION_COPY_NODE}},
            .case_count = 2,
            .otherwise = SXML_BUILT_IN_ACTION_ITEM};
    case SXML_IR_BUILT_IN_RULE_SHALLOW_SKIP:
        return (built_in_plan){
            .cases = {{SXML_SNIPPET_IS_DOCUMENT_OR_ELEMENT,
                       SXML_BUILT_IN_ACTION_APPLY_ATTRIBUTES_AND_CHILDREN},
                      {SXML_SNIPPET_IS_ARRAY, SXML_BUILT_IN_ACTION_APPLY_MEMBERS}},
            .case_count = 2,
            .otherwise = SXML_BUILT_IN_ACTION_NOTHING};
    case SXML_IR_BUILT_IN_RULE_DEEP_SKIP:
        return (built_in_plan){.cases = {{SXML_SNIPPET_IS_DOCUMENT, SXML_BUILT_IN_ACTION_APPLY_CHILDREN}},
                               .case_count = 1,
                               .otherwise = SXML_BUILT_IN_ACTION_NOTHING};
    case SXML_IR_BUILT_IN_RULE_FAIL:
        break;
    }
    return (built_in_plan){.case_count = 0, .otherwise = SXML_BUILT_IN_ACTION_FAIL};
}

/* The snippet of kind lowered as a body of the current function and
   bound in block (outside a machine). */
typedef struct snippet_binding {
    const sxml_xslt_lowering *lowering;
    snippet_kind kind;
    sxml_xpath_lowering_block *block;
} snippet_binding;

static sxml_status bind_snippet(sxml_xpath_lowerer *lowerer, snippet_binding binding,
                                sxml_ir_atom *atom) {
    sxml_status status = ensure_snippet(binding.lowering, binding.kind);
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_lower_embedded(
            lowerer,
            &(sxml_xpath_lowering_embedding){
                .ast = binding.lowering->templates->snippets[binding.kind],
                .node = {.value = 0},
                .static_context = NULL,
                .focus = NULL},
            &body);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_bind_expression(
        lowerer,
        (sxml_xpath_lowering_expression_in_block){.block = binding.block, .expression = body},
        atom);
}

/* APPLY_TEMPLATES of the snippet's items in the current mode with the
   parameters received (6.10), bound in block. */
static sxml_status bind_built_in_apply(sxml_xpath_lowerer *lowerer, snippet_binding binding,
                                       sxml_ir_atom *atom) {
    sxml_ir_atom select = {0};
    const sxml_status status = bind_snippet(lowerer, binding, &select);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_expression(lowerer, (expression_binding){.block = binding.block, .expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES,
            .apply_templates = {.select = select,
                                .mode = {.value = 0},
                                .rule_search = SXML_IR_RULE_SEARCH_FROM_START,
                                .parameters = slot_atom(parameters_slot),
                                .tunnel_parameters = slot_atom(tunnel_parameters_slot),
                                .separator = {.atom_kind = SXML_IR_ATOM_KIND_NONE}}}}, atom);
}

/* A built-in template being lowered. */
typedef struct built_in_lowering {
    const sxml_xslt_lowering *lowering;
    uint32_t construction_flags;
} built_in_lowering;

/* One action of it to lower. */
typedef struct built_in_action_request {
    built_in_lowering lowering;
    built_in_action action;
} built_in_action_request;

/* The copy of the item (SHALLOW_COPY) with the two atoms as content
   (none: no content), bound in block. */
typedef struct copy_request {
    sxml_xpath_lowering_block *block;
    const sxml_ir_atom *content;
    size_t content_count;
    uint32_t construction_flags;
} copy_request;

static sxml_status bind_copy(sxml_xpath_lowerer *lowerer, copy_request request,
                             sxml_ir_atom *atom) {
    sxml_ir_expression_index content = {0};
    sxml_status status = SXML_STATUS_OK;
    if (request.content_count > 0) {
        sxml_ir_atom_list list = {0};
        status = sxml_ir_unit_add_atoms(
            lowerer->unit,
            (sxml_ir_atom_view){.atoms = request.content, .atom_count = request.content_count},
            &list);
        if (status == SXML_STATUS_OK) {
            status = add_expression(
                lowerer,
                (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                     .sequence = list},
                &content);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_expression(lowerer, (expression_binding){.block = request.block, .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
                             .node_construction = {.value = slot_atom(item_slot),
                                                   .content = content,
                                                   .construction_flags =
                                                       request.construction_flags}}}, atom);
}

/* The body of one action, a block of its own. */
static sxml_status lower_built_in_action(sxml_xpath_lowerer *lowerer,
                                         built_in_action_request request,
                                         sxml_ir_expression_index *expression) {
    const built_in_lowering lowering = request.lowering;
    const built_in_action action = request.action;
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    const snippet_binding children = {
        .lowering = lowering.lowering, .kind = SXML_SNIPPET_CHILDREN, .block = &block};
    const snippet_binding attributes = {
        .lowering = lowering.lowering, .kind = SXML_SNIPPET_ATTRIBUTES, .block = &block};
    sxml_ir_atom atoms[2] = {{0}, {0}};
    sxml_ir_atom result = slot_atom(item_slot);
    sxml_status status = SXML_STATUS_OK;
    switch (action) {
    case SXML_BUILT_IN_ACTION_NOTHING:
        return sxml_xpath_lowerer_add_empty_sequence(lowerer, no_ast_node, expression);
    case SXML_BUILT_IN_ACTION_ITEM:
        break;
    case SXML_BUILT_IN_ACTION_FAIL:
        return add_raise(lowerer,
                         (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                      .code = "XTDE0555",
                                      .description = "no template rule matches an item in a mode "
                                                     "with on-no-match=\"fail\" (XSLT 4.0, "
                                                     "6.10.6)"},
                         expression);
    case SXML_BUILT_IN_ACTION_APPLY_CHILDREN:
        status = bind_built_in_apply(lowerer, children, &result);
        break;
    case SXML_BUILT_IN_ACTION_APPLY_JNODE_CHILDREN:
        status = bind_built_in_apply(lowerer,
                                     (snippet_binding){.lowering = lowering.lowering,
                                                       .kind = SXML_SNIPPET_JNODE_CHILDREN,
                                                       .block = &block},
                                     &result);
        break;
    case SXML_BUILT_IN_ACTION_APPLY_MEMBERS:
        status = bind_built_in_apply(lowerer,
                                     (snippet_binding){.lowering = lowering.lowering,
                                                       .kind = SXML_SNIPPET_MEMBERS,
                                                       .block = &block},
                                     &result);
        break;
    case SXML_BUILT_IN_ACTION_APPLY_ATTRIBUTES_AND_CHILDREN:
    case SXML_BUILT_IN_ACTION_SHALLOW_COPY:
        /* 6.10.3, 6.10.5: two applications, so positions restart. */
        status = bind_built_in_apply(lowerer, attributes, &atoms[0]);
        if (status == SXML_STATUS_OK) {
            status = bind_built_in_apply(lowerer, children, &atoms[1]);
        }
        if (status == SXML_STATUS_OK && action == SXML_BUILT_IN_ACTION_SHALLOW_COPY) {
            status = bind_copy(lowerer,
                               (copy_request){.block = &block,
                                              .content = atoms,
                                              .content_count = 2,
                                              .construction_flags = lowering.construction_flags},
                               &result);
        } else if (status == SXML_STATUS_OK) {
            sxml_ir_atom_list list = {0};
            status = sxml_ir_unit_add_atoms(
                lowerer->unit, (sxml_ir_atom_view){.atoms = atoms, .atom_count = 2}, &list);
            if (status == SXML_STATUS_OK) {
                status = bind_expression(lowerer, (expression_binding){.block = &block, .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                         .sequence = list}}, &result);
            }
        }
        break;
    case SXML_BUILT_IN_ACTION_DEEP_COPY:
        /* 6.10.2: the node with copies of its attributes and children. */
        status = bind_snippet(lowerer, attributes, &atoms[0]);
        if (status == SXML_STATUS_OK) {
            status = bind_snippet(lowerer, children, &atoms[1]);
        }
        if (status == SXML_STATUS_OK) {
            status = bind_copy(lowerer,
                               (copy_request){.block = &block,
                                              .content = atoms,
                                              .content_count = 2,
                                              .construction_flags = lowering.construction_flags},
                               &result);
        }
        break;
    case SXML_BUILT_IN_ACTION_COPY_NODE:
        status = bind_copy(lowerer,
                           (copy_request){.block = &block,
                                          .content = NULL,
                                          .content_count = 0,
                                          .construction_flags = lowering.construction_flags},
                           &result);
        break;
    case SXML_BUILT_IN_ACTION_TEXT: {
        sxml_ir_atom text = {0};
        status = bind_snippet(lowerer,
                              (snippet_binding){.lowering = lowering.lowering,
                                                .kind = SXML_SNIPPET_STRING,
                                                .block = &block},
                              &text);
        if (status == SXML_STATUS_OK) {
            status = bind_expression(lowerer, (expression_binding){.block = &block, .expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                                     .node_construction = {.value = text}}}, &result);
        }
        break;
    }
    }
    sxml_ir_expression_index last = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(lowerer, atom_expression(result), &last);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = last}, expression);
}

/* The body of a built-in template: the plan's cases as a chain of
   conditionals, built from the last. */
typedef struct built_in_body_request {
    built_in_lowering lowering;
    built_in_plan plan;
} built_in_body_request;

static sxml_status lower_built_in_body(sxml_xpath_lowerer *lowerer, built_in_body_request request,
                                       sxml_ir_expression_index *body) {
    const built_in_lowering lowering = request.lowering;
    const built_in_plan plan = request.plan;
    sxml_status status = lower_built_in_action(
        lowerer, (built_in_action_request){.lowering = lowering, .action = plan.otherwise}, body);
    for (size_t case_number = plan.case_count; status == SXML_STATUS_OK && case_number > 0;
         case_number -= 1) {
        const built_in_case branch = plan.cases[case_number - 1];
        sxml_ir_expression_index then_branch = {0};
        status = lower_built_in_action(
            lowerer, (built_in_action_request){.lowering = lowering, .action = branch.action},
            &then_branch);
        sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
        sxml_ir_atom condition = {0};
        if (status == SXML_STATUS_OK) {
            status = bind_snippet(lowerer,
                                  (snippet_binding){.lowering = lowering.lowering,
                                                    .kind = branch.condition,
                                                    .block = &block},
                                  &condition);
        }
        sxml_ir_expression_index conditional = {0};
        if (status == SXML_STATUS_OK) {
            status = add_expression(
                lowerer,
                (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                     .conditional = {.condition = condition,
                                                     .then_branch = then_branch,
                                                     .else_branch = *body}},
                &conditional);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_finish_block(
                lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = conditional},
                body);
        }
    }
    return status;
}

/* The built-in template of rule (made once per lowering). */
typedef struct built_in_request {
    sxml_ir_built_in_rule rule;
    bool copies_namespaces;
} built_in_request;

static sxml_status get_built_in_template(const sxml_xslt_lowering *lowering,
                                         built_in_request request,
                                         sxml_ir_function_index *function) {
    sxml_xslt_template_lowering *templates = lowering->templates;
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    const size_t slot = (size_t)request.rule +
                        (request.copies_namespaces ? 0U : BUILT_IN_TEMPLATE_KIND_COUNT);
    if (templates->built_in_templates[slot].value != 0) {
        *function = templates->built_in_templates[slot];
        return SXML_STATUS_OK;
    }
    sxml_ir_function_index index = {0};
    sxml_status status = sxml_ir_unit_add_function(
        lowerer->unit,
        &(sxml_ir_function){.signature = templates->template_signature,
                            .parameter_count = TEMPLATE_PARAMETER_COUNT},
        &index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_focus focus_slots = {
        .context_value = item_slot, .context_position = position_slot, .context_size = size_slot};
    sxml_xpath_lowering_function_frame function_frame = {
        .function = {.function_index = index,
                     .function_level = 0,
                     .slot_count = TEMPLATE_PARAMETER_COUNT,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS, .slots = focus_slots},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &function_frame);
    /* 6.10.2, 6.10.3: copy-namespaces of the mode. */
    const uint32_t flags = (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES |
                           (request.copies_namespaces
                                ? 0U
                                : (uint32_t)SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES);
    sxml_ir_expression_index body = {0};
    status = lower_built_in_body(
        lowerer,
        (built_in_body_request){
            .lowering = {.lowering = lowering, .construction_flags = flags},
            .plan = plan_of(request.rule)},
        &body);
    sxml_xpath_lowerer_leave_function(lowerer, &function_frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = index, .function = *sxml_ir_unit_get_function(lowerer->unit, index)};
    replacement.function.body = body;
    replacement.function.slot_count = function_frame.function.slot_count;
    replacement.function.focus = focus_slots;
    status = sxml_ir_unit_replace_function(lowerer->unit, &replacement);
    if (status == SXML_STATUS_OK) {
        templates->built_in_templates[slot] = index;
        *function = index;
    }
    return status;
}

/* ========================================================= Template rules */

/* One rule of one mode, before it is added. */
typedef struct pending_rule {
    uint32_t mode_position;
    sxml_ir_template_rule rule;
    /* Ties of declaration order (the templates of one enclosing mode)
       go by document order. */
    uint32_t node_number;
} pending_rule;

/* True when first comes before second in the unit's rule array: by mode,
   then 6.8's order (higher precedence, then higher priority, then later
   in declaration order first). */
static bool comes_before(const pending_rule *first, const pending_rule *second) {
    if (first->mode_position != second->mode_position) {
        return first->mode_position < second->mode_position;
    }
    if (first->rule.import_precedence != second->rule.import_precedence) {
        return first->rule.import_precedence > second->rule.import_precedence;
    }
    if (first->rule.priority > second->rule.priority ||
        first->rule.priority < second->rule.priority) {
        return first->rule.priority > second->rule.priority;
    }
    if (first->rule.declaration_order != second->rule.declaration_order) {
        return first->rule.declaration_order > second->rule.declaration_order;
    }
    return first->node_number > second->node_number;
}

/* A heap of pending rules (heapsort: no recursion, O(n log n)). */
typedef struct rule_heap {
    pending_rule *rules;
    size_t rule_count;
} rule_heap;

static void sift_down(rule_heap heap, size_t root) {
    size_t parent = root;
    for (;;) {
        size_t last = parent;
        const size_t left = 2 * parent + 1;
        if (left < heap.rule_count && comes_before(&heap.rules[last], &heap.rules[left])) {
            last = left;
        }
        if (left + 1 < heap.rule_count &&
            comes_before(&heap.rules[last], &heap.rules[left + 1])) {
            last = left + 1;
        }
        if (last == parent) {
            return;
        }
        const pending_rule moved = heap.rules[parent];
        heap.rules[parent] = heap.rules[last];
        heap.rules[last] = moved;
        parent = last;
    }
}

static void sort_rules(pending_rule *rules, size_t rule_count) {
    for (size_t heap_root = rule_count / 2; heap_root > 0; heap_root -= 1) {
        sift_down((rule_heap){.rules = rules, .rule_count = rule_count}, heap_root - 1);
    }
    for (size_t heap_end = rule_count; heap_end > 1; heap_end -= 1) {
        const pending_rule last = rules[0];
        rules[0] = rules[heap_end - 1];
        rules[heap_end - 1] = last;
        sift_down((rule_heap){.rules = rules, .rule_count = heap_end - 1}, 0);
    }
}

/* The modes entry applies to (6.9.2), as positions in templates->modes:
   written to positions (room for every mode), counted in *count. */
typedef struct applicable_modes {
    size_t *positions;
    size_t count;
} applicable_modes;

static void find_applicable_modes(const sxml_xslt_lowering *lowering, const template_entry *entry,
                                  applicable_modes *modes) {
    const sxml_xslt_template_lowering *templates = lowering->templates;
    modes->count = 0;
    if (entry->enclosing_mode.value != 0) {
        modes->positions[0] =
            find_mode(templates, declared_mode(lowering, entry->enclosing_mode)).value - 1U;
        modes->count = 1;
        return;
    }
    const sxml_xslt_ast_name_token_view tokens = mode_tokens_of(lowering, entry->node);
    const size_t token_count = tokens.name_token_count == 0 ? 1 : tokens.name_token_count;
    for (size_t token_index = 0; token_index < token_count; token_index += 1) {
        const mode_reference reference =
            tokens.name_token_count == 0
                ? default_mode_of(lowering, entry->node)
                : reference_of(lowering, entry->node, tokens.name_tokens[token_index]);
        if (reference.is_all) {
            /* #all: the modes of the template's package (3.4.6). */
            for (size_t mode_index = 0; mode_index < templates->mode_count; mode_index += 1) {
                if (!templates->modes[mode_index].is_enclosing &&
                    templates->modes[mode_index].package == reference.package) {
                    modes->positions[modes->count] = mode_index;
                    modes->count += 1;
                }
            }
            return;
        }
        const sxml_ir_mode_index mode = find_mode(templates, reference);
        bool is_listed = mode.value == 0;
        for (size_t listed = 0; listed < modes->count; listed += 1) {
            is_listed = is_listed || modes->positions[listed] == mode.value - 1U;
        }
        if (!is_listed) {
            modes->positions[modes->count] = mode.value - 1U;
            modes->count += 1;
        }
    }
}

/* The priority of a template rule: its priority attribute, else its
   pattern's default priority (6.4). */
static sxml_status priority_of(const sxml_xslt_lowering *lowering, const template_entry *entry,
                               double *priority) {
    const sxml_xslt_ast_attribute *attribute =
        find_attribute(lowering, entry->node, SXML_XSLT_AST_ATTRIBUTE_PRIORITY);
    if (attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_NUMBER) {
        *priority = attribute->value.payload.number;
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_expression *pattern =
        sxml_xslt_ast_get_expression(lowering->ast, entry->pattern);
    const sxml_xslt_ast_static_context *context =
        sxml_xslt_ast_get_static_context(lowering->ast, pattern->static_context);
    return sxml_xslt_pattern_compute_default_priority(
        &(sxml_xslt_pattern_priority_request){.pattern_ast = pattern->ast,
                                              .static_context =
                                                  expression_context(lowering, entry->pattern),
                                              .is_any_namespace = context->is_any_namespace,
                                              .error_record = lowering->error_record},
        priority);
}

/* The match functions, rules and modes of the unit. */
static sxml_status add_rules_and_modes(const sxml_xslt_lowering *lowering) {
    sxml_xslt_template_lowering *templates = lowering->templates;
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    const sxml_allocator allocator = lowerer->allocator;
    void *memory = NULL;
    sxml_status status = allocate_zeroed(allocator,
                                         (zeroed_request){.element_count = templates->mode_count,
                                                          .element_size = sizeof(size_t),
                                                          .element_alignment = alignof(size_t)},
                                         &memory);
    applicable_modes modes = {.positions = memory, .count = 0};
    size_t rule_count = 0;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < templates->template_count;
         entry_index += 1) {
        const template_entry *entry = &templates->templates[entry_index];
        if (entry->pattern.value != 0) {
            find_applicable_modes(lowering, entry, &modes);
            rule_count += modes.count;
        }
    }
    pending_rule *rules = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(allocator,
                                 (zeroed_request){.element_count = rule_count,
                                                  .element_size = sizeof(pending_rule),
                                                  .element_alignment = alignof(pending_rule)},
                                 &memory);
        rules = memory;
    }
    size_t filled = 0;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < templates->template_count;
         entry_index += 1) {
        template_entry *entry = &templates->templates[entry_index];
        if (entry->pattern.value == 0) {
            continue;
        }
        const sxml_xslt_ast_expression *pattern =
            sxml_xslt_ast_get_expression(lowering->ast, entry->pattern);
        status = sxml_xslt_pattern_lower_match_function(
            lowerer,
            &(sxml_xslt_pattern_match_function_request){
                .pattern_ast = pattern->ast,
                .static_context = expression_context(lowering, entry->pattern)},
            &entry->match_function);
        double priority = 0;
        if (status == SXML_STATUS_OK) {
            status = priority_of(lowering, entry, &priority);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        find_applicable_modes(lowering, entry, &modes);
        for (size_t mode_index = 0; mode_index < modes.count; mode_index += 1) {
            rules[filled] = (pending_rule){
                .mode_position = (uint32_t)modes.positions[mode_index],
                .rule = {.match_function = entry->match_function,
                         .template_function = entry->function,
                         .priority = priority,
                         .import_precedence = entry->import_precedence,
                         .lowest_imported_precedence = entry->lowest_imported_precedence,
                         .declaration_order = entry->declaration_order},
                .node_number = entry->node.value};
            filled += 1;
        }
    }
    sxml_allocator_release(allocator, modes.positions);
    if (status == SXML_STATUS_OK) {
        sort_rules(rules, filled);
    }
    sxml_ir_template_rule *ir_rules = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(allocator,
                                 (zeroed_request){.element_count = filled,
                                                  .element_size = sizeof(sxml_ir_template_rule),
                                                  .element_alignment =
                                                      alignof(sxml_ir_template_rule)},
                                 &memory);
        ir_rules = memory;
    }
    sxml_ir_mode *ir_modes = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(allocator,
                                 (zeroed_request){.element_count = templates->mode_count,
                                                  .element_size = sizeof(sxml_ir_mode),
                                                  .element_alignment = alignof(sxml_ir_mode)},
                                 &memory);
        ir_modes = memory;
    }
    size_t rule_index = 0;
    for (size_t mode_index = 0; status == SXML_STATUS_OK && mode_index < templates->mode_count;
         mode_index += 1) {
        const mode_entry *mode = &templates->modes[mode_index];
        size_t first = rule_index;
        while (rule_index < filled && rules[rule_index].mode_position == mode_index) {
            rule_index += 1;
        }
        /* The rules of the mode, from the front of ir_rules (room for all
           of them). */
        for (size_t copied = first; copied < rule_index; copied += 1) {
            ir_rules[copied - first] = rules[copied].rule;
        }
        const sxml_ir_template_rule *run = rule_index > first ? ir_rules : NULL;
        const sxml_ir_built_in_rule built_in = mode->on_no_match.is_set
                                                   ? (sxml_ir_built_in_rule)mode->on_no_match.value
                                                   : SXML_IR_BUILT_IN_RULE_TEXT_ONLY_COPY;
        sxml_ir_function_index built_in_template = {0};
        status = get_built_in_template(
            lowering,
            (built_in_request){.rule = built_in,
                               .copies_namespaces = !mode->copies_namespaces.is_set ||
                                                    mode->copies_namespaces.value != 0},
            &built_in_template);
        sxml_ir_template_rule_list list = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_add_template_rules(
                lowerer->unit,
                (sxml_ir_template_rule_view){.template_rules = run,
                                             .template_rule_count = rule_index - first},
                &list);
        }
        ir_modes[mode_index] = (sxml_ir_mode){
            .mode_name = mode->mode_name,
            .template_rules = list,
            .built_in_rule = built_in,
            .built_in_template = built_in_template,
            .fails_on_multiple_match = mode->fails_on_multiple_match.is_set &&
                                       mode->fails_on_multiple_match.value != 0};
    }
    sxml_ir_mode_index first_mode = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_modes(
            lowerer->unit,
            (sxml_ir_mode_view){.modes = ir_modes, .mode_count = templates->mode_count},
            &first_mode);
    }
    if (status == SXML_STATUS_OK && first_mode.value != 1) {
        /* Instructions were lowered with modes numbered from 1. */
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_allocator_release(allocator, ir_modes);
    sxml_allocator_release(allocator, ir_rules);
    sxml_allocator_release(allocator, rules);
    return status;
}

/* =============================================================== Entry */

static sxml_status make_signature(sxml_xslt_template_lowering *templates,
                                  sxml_xpath_lowerer *lowerer) {
    const sxml_sequence_type map_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_MAP),
                                         .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    const sxml_sequence_type integer_type = {
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
        .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    const sxml_sequence_type parameter_types[TEMPLATE_PARAMETER_COUNT] = {
        {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
         .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE},
        integer_type, integer_type, map_type, map_type};
    return sxml_xpath_lowerer_register_signature(
        lowerer,
        (sxml_xpath_lowering_signature_request){
            .parameter_types = {.sequence_types = parameter_types,
                                .sequence_type_count = TEMPLATE_PARAMETER_COUNT},
            .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}},
        &templates->template_signature);
}

static sxml_status make_empty_map(sxml_xslt_template_lowering *templates,
                                  sxml_xpath_lowerer *lowerer) {
    sxml_item map = {.representation = 0};
    const sxml_status status = sxml_map_make(
        lowerer->item_heap,
        &(sxml_map_request){.map_entries = {.map_entries = NULL, .map_entry_count = 0},
                            .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                            .error_record = NULL},
        &map);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_item_constant(lowerer, map, &templates->empty_map);
}

sxml_status sxml_xslt_lower_templates(sxml_xslt_lowering *lowering) {
    sxml_xpath_lowerer *lowerer = lowering->lowerer;
    void *memory = NULL;
    sxml_status status =
        allocate_zeroed(lowerer->allocator,
                        (zeroed_request){.element_count = 1,
                                         .element_size = sizeof(sxml_xslt_template_lowering),
                                         .element_alignment = alignof(sxml_xslt_template_lowering)},
                        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    lowering->templates = memory;
    size_t unnamed = 0;
    status = make_signature(lowering->templates, lowerer);
    if (status == SXML_STATUS_OK) {
        status = make_empty_map(lowering->templates, lowerer);
    }
    /* Every package's unnamed mode (3.4.6), the top-level package's
       first. */
    for (uint32_t package = 1;
         status == SXML_STATUS_OK && package <= sxml_xslt_package_count(lowering); package += 1) {
        status = add_mode(lowering,
                          (mode_reference){.is_current = false,
                                           .is_all = false,
                                           .mode_name = {.index = 0},
                                           .package = package},
                          &unnamed);
    }
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(lowering->ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index declaration =
            sxml_xslt_ast_get_declaration(lowering->ast, order);
        if (node_of(lowering, declaration)->kind == SXML_XSLT_AST_KIND_MODE) {
            status = read_mode_declaration(lowering, declaration);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = check_mode_conflicts(lowering);
    }
    if (status == SXML_STATUS_OK) {
        status = collect_mode_references(lowering);
    }
    if (status == SXML_STATUS_OK) {
        status = collect_templates(lowering);
    }
    if (status == SXML_STATUS_OK) {
        status = check_template_names(lowering);
    }
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < lowering->templates->template_count;
         entry_index += 1) {
        const template_entry *entry = &lowering->templates->templates[entry_index];
        status = lower_template_function(
            lowering, (template_function_request){.entry = entry, .is_focus_absent = false});
        if (status == SXML_STATUS_OK && entry->absent_function.value != 0) {
            status = lower_template_function(
                lowering, (template_function_request){.entry = entry, .is_focus_absent = true});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = add_rules_and_modes(lowering);
    }
    return status;
}

void sxml_xslt_release_templates(sxml_xslt_lowering *lowering) {
    sxml_xslt_template_lowering *templates = lowering->templates;
    if (templates == NULL) {
        return;
    }
    const sxml_allocator allocator = lowering->lowerer->allocator;
    for (size_t snippet_index = 0; snippet_index < SXML_SNIPPET_COUNT; snippet_index += 1) {
        sxml_ast_destroy(templates->snippets[snippet_index]);
    }
    sxml_allocator_release(allocator, templates->templates);
    sxml_allocator_release(allocator, templates->modes);
    sxml_allocator_release(allocator, templates);
    lowering->templates = NULL;
}

/* ---------------------------------------------------------- Invocation */

/* Whether the main function has a focus (a global context item): true(),
   or false() from a TRY_CATCH of the XPDY0002 reading it raises. Also
   for xsl:global-context-item (src/xslt_lower_declaration.c). */
sxml_status sxml_xslt_lowering_bind_has_focus(sxml_xpath_lowerer *lowerer,
                                              sxml_xpath_lowering_block *block,
                                              sxml_ir_atom *atom) {
    sxml_ir_atom focus = {0};
    sxml_status status = sxml_xpath_lowerer_read_focus(
        lowerer,
        (sxml_xpath_lowering_focus_read){
            .block = block, .node = no_ast_node, .part = SXML_XPATH_LOWERING_FOCUS_PART_VALUE},
        &focus);
    sxml_xpath_lowering_block body_block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom exists = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_call(lowerer,
                           (library_call){.block = &body_block,
                                          .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                          .local_name = "exists",
                                          .arguments = &focus,
                                          .argument_count = 1},
                           &exists);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(lowerer, atom_expression(exists), &body);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer, (sxml_xpath_lowering_block_result){.block = body_block, .result = body},
            &body);
    }
    sxml_name_identifier code = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            lowerer->name_pool,
            &(sxml_name_request){.namespace_uri =
                                     sxml_well_known_namespace_identifier(SXML_NAMESPACE_ERRORS),
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("XPDY0002")},
            &code);
    }
    sxml_ir_catch_clause clause = {0};
    if (status == SXML_STATUS_OK) {
        const sxml_name_test test = {.name_test_kind = SXML_NAME_TEST_KIND_EXPANDED_NAME,
                                     .expanded_name = code,
                                     .namespace_uri = {.index = 0},
                                     .local_name = {.bytes = NULL, .byte_count = 0}};
        status = sxml_ir_unit_add_name_tests(
            lowerer->unit, (sxml_ir_name_test_view){.name_tests = &test, .name_test_count = 1},
            &clause.error_tests);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(lowerer, &clause.error_details);
    }
    sxml_item false_item = sxml_atomic_item_false();
    sxml_ir_atom false_atom = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(lowerer, false_item, &false_atom);
    }
    if (status == SXML_STATUS_OK) {
        status = add_expression(lowerer, atom_expression(false_atom), &clause.handler);
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
    return bind_expression(
        lowerer,
        (expression_binding){.block = block,
                             .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH,
                                            .try_catch = {.body = body,
                                                          .catch_clauses = clauses,
                                                          .finally_body = {.value = 0}}}},
        atom);
}

/* A call of a template from the main function, as a block of its own:
   with the main function's focus, or (is_focus_absent) the absent-focus
   variant with an empty one. */
typedef struct initial_call {
    const sxml_xslt_lowering *lowering;
    const sxml_xslt_invocation *invocation;
    const template_entry *entry;
    bool is_focus_absent;
} initial_call;

/* An atom of the invocation, or default_atom for one of kind NONE. */
static sxml_ir_atom atom_or_default(sxml_ir_atom atom, sxml_ir_atom default_atom) {
    return atom.atom_kind == SXML_IR_ATOM_KIND_NONE ? default_atom : atom;
}

static sxml_status lower_initial_call(sxml_xpath_lowerer *lowerer, initial_call call,
                                      sxml_ir_expression_index *expression);

/* The initial mode (2.4.3): the requested one, else the default mode of
   the principal module. */
static sxml_ir_mode_index initial_mode_of(const sxml_xslt_lowering *lowering,
                                          const sxml_xslt_invocation *invocation) {
    const sxml_xslt_ast_node_index root =
        sxml_xslt_ast_get_module(lowering->ast, (sxml_xslt_ast_module_index){.value = 1})->root;
    if (invocation->selects_unnamed_mode) {
        return find_mode(lowering->templates, named_mode(lowering, root, (sxml_name_identifier){
                                                                             .index = 0}));
    }
    if (invocation->initial_mode.index != 0) {
        /* 2.4.3: a named initial mode an xsl:package declares is public
           or final there; an implicitly declared one is eligible. */
        const sxml_xslt_visibility visibility = sxml_xslt_package_get_reference_visibility(
            lowering,
            &(sxml_xslt_component_reference){.component_kind = SXML_XSLT_COMPONENT_KIND_MODE,
                                             .component_name = invocation->initial_mode,
                                             .arity = 0,
                                             .package = 1});
        if (node_of(lowering, root)->kind == SXML_XSLT_AST_KIND_PACKAGE &&
            visibility != SXML_XSLT_VISIBILITY_NONE && visibility != SXML_XSLT_VISIBILITY_PUBLIC &&
            visibility != SXML_XSLT_VISIBILITY_FINAL) {
            return (sxml_ir_mode_index){.value = 0};
        }
        return find_mode(lowering->templates, named_mode(lowering, root, invocation->initial_mode));
    }
    return find_mode(lowering->templates, default_mode_of(lowering, root));
}

/* Templates applied to the focus (the global context item), checked. */
typedef struct focused_apply {
    const sxml_xslt_invocation_request *request;
    sxml_ir_expression apply;
} focused_apply;

static sxml_status bind_focused_apply(sxml_xpath_lowerer *lowerer, const focused_apply *focused,
                                      sxml_ir_atom *atom);

static sxml_status bind_initial_function(sxml_xpath_lowerer *lowerer,
                                         const sxml_xslt_invocation_request *request,
                                         sxml_ir_atom *atom);

sxml_status sxml_xslt_lowering_bind_invocation(sxml_xpath_lowerer *lowerer,
                                               const sxml_xslt_invocation_request *request,
                                               sxml_ir_atom *atom) {
    const sxml_xslt_lowering *lowering = request->lowering;
    const sxml_xslt_template_lowering *templates = lowering->templates;
    if (request->invocation.initial_function.index != 0) {
        return bind_initial_function(lowerer, request, atom);
    }
    if (request->invocation.is_unknown_template) {
        sxml_ir_expression_index raise = {0};
        const sxml_status status = add_raise(
            lowerer,
            (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                         .code = "XTDE0040",
                         .description = "the stylesheet has no template of the initial template's "
                                        "name (XSLT 4.0, 2.4.4)"},
            &raise);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = request->block, .expression = raise},
            atom);
    }
    if (request->invocation.template_node.value != 0) {
        /* 2.4.4: the template with the global context item as its focus,
           which may be absent. */
        const template_entry *entry =
            find_template_entry(templates, request->invocation.template_node);
        if (entry == NULL) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_ir_atom has_focus = {0};
        sxml_ir_expression_index with_focus = {0};
        sxml_ir_expression_index without_focus = {0};
        sxml_status status = SXML_STATUS_OK;
        if (entry->absent_function.value == 0) {
            /* A template rule without a name: called with the focus. */
            status = lower_initial_call(
                lowerer, (initial_call){.lowering = lowering,
                                              .invocation = &request->invocation,
                                              .entry = entry,
                                              .is_focus_absent = false},
                &with_focus);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            return sxml_xpath_lowerer_bind_expression(
                lowerer,
                (sxml_xpath_lowering_expression_in_block){.block = request->block,
                                                          .expression = with_focus},
                atom);
        }
        status = sxml_xslt_lowering_bind_has_focus(lowerer, request->block, &has_focus);
        if (status == SXML_STATUS_OK) {
            status = lower_initial_call(lowerer,
                                        (initial_call){.lowering = lowering,
                                              .invocation = &request->invocation,
                                              .entry = entry,
                                              .is_focus_absent = false},
                                        &with_focus);
        }
        if (status == SXML_STATUS_OK) {
            status = lower_initial_call(lowerer,
                                        (initial_call){.lowering = lowering,
                                              .invocation = &request->invocation,
                                              .entry = entry,
                                              .is_focus_absent = true},
                                        &without_focus);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return bind_expression(
            lowerer,
            (expression_binding){
                .block = request->block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                               .conditional = {.condition = has_focus,
                                               .then_branch = with_focus,
                                               .else_branch = without_focus}}},
            atom);
    }
    const sxml_ir_mode_index mode = initial_mode_of(lowering, &request->invocation);
    if (mode.value == 0) {
        sxml_ir_expression_index raise = {0};
        const sxml_status status = add_raise(
            lowerer,
            (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                         .code = "XTDE0045",
                         .description = "the initial mode is not a mode of the stylesheet (XSLT "
                                        "4.0, 2.4.3)"},
            &raise);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = request->block, .expression = raise},
            atom);
    }
    const sxml_ir_expression apply = {
        .expression_kind = SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES,
        .apply_templates = {.select = request->invocation.initial_match_selection,
                            .mode = mode,
                            .rule_search = SXML_IR_RULE_SEARCH_FROM_START,
                            .parameters = atom_or_default(request->invocation.template_parameters,
                                                          templates->empty_map),
                            .tunnel_parameters = atom_or_default(
                                request->invocation.tunnel_parameters, templates->empty_map),
                            .separator = {.atom_kind = SXML_IR_ATOM_KIND_NONE}}};
    if (apply.apply_templates.select.atom_kind != SXML_IR_ATOM_KIND_NONE) {
        /* The host's initial match selection (packet 4.15). */
        return bind_expression(lowerer, (expression_binding){.block = request->block,
                                                             .expression = apply},
                               atom);
    }
    return bind_focused_apply(lowerer, &(focused_apply){.request = request, .apply = apply},
                              atom);
}

/* 2.4.3: templates applied to the global context item, the initial match
   selection; XTDE0044 when there is none. */
static sxml_status bind_focused_apply(sxml_xpath_lowerer *lowerer, const focused_apply *focused,
                                      sxml_ir_atom *atom) {
    sxml_ir_atom has_focus = {0};
    sxml_status status =
        sxml_xslt_lowering_bind_has_focus(lowerer, focused->request->block, &has_focus);
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    sxml_ir_expression apply = focused->apply;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_read_focus(
            lowerer,
            (sxml_xpath_lowering_focus_read){.block = &block,
                                             .node = no_ast_node,
                                             .part = SXML_XPATH_LOWERING_FOCUS_PART_VALUE},
            &apply.apply_templates.select);
    }
    sxml_ir_expression_index applied = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer, (sxml_xpath_lowering_expression_to_add){.node = no_ast_node,
                                                             .expression = apply},
            &applied);
    }
    sxml_ir_expression_index with_focus = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = applied},
            &with_focus);
    }
    sxml_ir_expression_index raise = {0};
    if (status == SXML_STATUS_OK) {
        status = add_raise(lowerer,
                           (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                        .code = "XTDE0044",
                                        .description = "an apply-templates invocation has "
                                                       "neither an initial match selection nor a "
                                                       "global context item (XSLT 4.0, 2.4.3)"},
                           &raise);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_expression(
        lowerer,
        (expression_binding){.block = focused->request->block,
                             .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                            .conditional = {.condition = has_focus,
                                                            .then_branch = with_focus,
                                                            .else_branch = raise}}},
        atom);
}

/* The most arguments of an initial function. */
enum { SXML_INITIAL_FUNCTION_MAXIMUM_ARITY = 32 };

/* True when an xsl:function of the name is public or final (2.4.5: the
   functions of an xsl:stylesheet are private unless they say so). */
static bool is_public_function(const sxml_xslt_lowering *lowering, sxml_name_identifier name) {
    if (sxml_xslt_package_count(lowering) > 1U) {
        /* 3.4.5: the top-level package's own or accepted public or final
           function. */
        for (uint32_t arity = 0; arity <= SXML_INITIAL_FUNCTION_MAXIMUM_ARITY; arity += 1) {
            const sxml_xslt_visibility visibility = sxml_xslt_package_get_reference_visibility(
                lowering, &(sxml_xslt_component_reference){
                              .component_kind = SXML_XSLT_COMPONENT_KIND_FUNCTION,
                              .component_name = name,
                              .arity = arity,
                              .package = 1});
            if (visibility == SXML_XSLT_VISIBILITY_PUBLIC ||
                visibility == SXML_XSLT_VISIBILITY_FINAL) {
                return true;
            }
        }
        return false;
    }
    const sxml_xslt_ast *ast = lowering->ast;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (sxml_xslt_ast_get_node(ast, declaration)->kind != SXML_XSLT_AST_KIND_FUNCTION) {
            continue;
        }
        const sxml_xslt_ast_attribute *function_name = sxml_xslt_ast_get_attribute(
            ast, sxml_xslt_ast_find_attribute(
                     ast, (sxml_xslt_ast_attribute_query){.node = declaration,
                                                          .kind = SXML_XSLT_AST_ATTRIBUTE_NAME}));
        const sxml_xslt_ast_attribute *visibility = sxml_xslt_ast_get_attribute(
            ast,
            sxml_xslt_ast_find_attribute(
                ast, (sxml_xslt_ast_attribute_query){.node = declaration,
                                                     .kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY}));
        if (function_name->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME &&
            function_name->value.payload.name.index == name.index &&
            visibility->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD &&
            (visibility->value.payload.keyword == SXML_XSLT_AST_KEYWORD_PUBLIC ||
             visibility->value.payload.keyword == SXML_XSLT_AST_KEYWORD_FINAL)) {
            return true;
        }
    }
    return false;
}

/*
 * 2.4.5: the initial function called with the members of the arguments
 * array, each coerced to its parameter's type (XPTY0004); XTDE0041 when
 * the stylesheet has no public or final function of that name and arity.
 */
static sxml_status bind_initial_function(sxml_xpath_lowerer *lowerer,
                                         const sxml_xslt_invocation_request *request,
                                         sxml_ir_atom *atom) {
    const sxml_xslt_lowering *lowering = request->lowering;
    const sxml_xslt_invocation *invocation = &request->invocation;
    const sxml_xslt_ast_node *root = sxml_xslt_ast_get_node(
        lowering->ast,
        sxml_xslt_ast_get_module(lowering->ast, (sxml_xslt_ast_module_index){.value = 1})->root);
    sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    sxml_status status = SXML_STATUS_OK;
    if (invocation->function_arity <= SXML_INITIAL_FUNCTION_MAXIMUM_ARITY &&
        root->static_context.value < lowering->static_context_count &&
        is_public_function(lowering, invocation->initial_function)) {
        status = sxml_xpath_find_function(
            lowering->static_contexts[root->static_context.value].function_table,
            &(sxml_function_query){.function_name = invocation->initial_function,
                                   .arity = invocation->function_arity},
            &entry);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (entry.entry_kind != SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION) {
        sxml_ir_expression_index raise = {0};
        status = add_raise(lowerer,
                           (raise_text){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                        .code = "XTDE0041",
                                        .description = "the stylesheet has no public or final "
                                                       "function of the initial function's name "
                                                       "and arity (XSLT 4.0, 2.4.5)"},
                           &raise);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = request->block, .expression = raise},
            atom);
    }
    sxml_ir_atom arguments[SXML_INITIAL_FUNCTION_MAXIMUM_ARITY];
    for (uint32_t argument_index = 0;
         status == SXML_STATUS_OK && argument_index < invocation->function_arity;
         argument_index += 1) {
        sxml_ir_atom position = {0};
        status = add_integer_constant(lowerer, (int64_t)argument_index + 1, &position);
        const sxml_ir_atom get_arguments[2] = {invocation->function_arguments, position};
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_bind_library_call(
                lowerer,
                &(sxml_xslt_library_call){
                    .block = request->block,
                    .function_namespace = SXML_NAMESPACE_ARRAY_FUNCTIONS,
                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("get"),
                    .arguments = {.atoms = get_arguments, .atom_count = 2}},
                &arguments[argument_index]);
        }
        if (status == SXML_STATUS_OK &&
            argument_index < entry.parameter_types.sequence_type_count) {
            status = sxml_xpath_lowerer_coerce_atom(
                lowerer,
                (sxml_xpath_lowering_coercion_request){
                    .block = request->block,
                    .node = no_ast_node,
                    .value = arguments[argument_index],
                    .sequence_type = entry.parameter_types.sequence_types[argument_index],
                    .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
                &arguments[argument_index]);
        }
    }
    sxml_ir_atom_list argument_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit,
            (sxml_ir_atom_view){.atoms = arguments, .atom_count = invocation->function_arity},
            &argument_list);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_expression(
        lowerer,
        (expression_binding){
            .block = request->block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                           .static_call = {.target = {.target_kind =
                                                          SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                                      .function = {.value =
                                                                       entry.ir_function_number}},
                                           .arguments = argument_list}}},
        atom);
}

static sxml_status lower_initial_call(sxml_xpath_lowerer *lowerer, initial_call call,
                                      sxml_ir_expression_index *expression) {
    const sxml_xslt_template_lowering *templates = call.lowering->templates;
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    focus_atoms focus = {{0}, {0}, {0}};
    sxml_status status = SXML_STATUS_OK;
    if (call.is_focus_absent) {
        status = bind_empty_sequence(lowerer, &block, &focus.item);
        if (status == SXML_STATUS_OK) {
            status = add_integer_constant(lowerer, 0, &focus.position);
        }
    } else {
        status = sxml_xpath_lowerer_read_focus(
            lowerer,
            (sxml_xpath_lowering_focus_read){.block = &block,
                                             .node = no_ast_node,
                                             .part = SXML_XPATH_LOWERING_FOCUS_PART_VALUE},
            &focus.item);
        if (status == SXML_STATUS_OK) {
            status = add_integer_constant(lowerer, 1, &focus.position);
        }
    }
    focus.size = focus.position;
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_template_call(
            lowerer,
            &(template_call){.block = &block,
                             .function = call.is_focus_absent ? call.entry->absent_function
                                                              : call.entry->function,
                             .focus = focus,
                             .parameters = atom_or_default(call.invocation->template_parameters,
                                                           templates->empty_map),
                             .tunnel_parameters = atom_or_default(
                                 call.invocation->tunnel_parameters, templates->empty_map)},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_finish_block(
        lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = result}, expression);
}

static sxml_xslt_ast_node_index mode_type_declaration_of(const sxml_xslt_lowering *lowering,
                                                         sxml_xslt_ast_node_index node) {
    const sxml_xslt_template_lowering *templates = lowering->templates;
    const template_entry *entry = find_template_entry(templates, node);
    if (entry == NULL || entry->pattern.value == 0) {
        return (sxml_xslt_ast_node_index){.value = 0};
    }
    if (entry->enclosing_mode.value != 0) {
        const sxml_ir_mode_index mode =
            find_mode(templates, declared_mode(lowering, entry->enclosing_mode));
        return mode.value == 0 ? (sxml_xslt_ast_node_index){.value = 0}
                               : templates->modes[mode.value - 1U].as_declaration;
    }
    const sxml_xslt_ast_name_token_view tokens = mode_tokens_of(lowering, node);
    const size_t token_count = tokens.name_token_count == 0 ? 1 : tokens.name_token_count;
    for (size_t token_index = 0; token_index < token_count; token_index += 1) {
        const mode_reference reference =
            tokens.name_token_count == 0
                ? default_mode_of(lowering, node)
                : reference_of(lowering, node, tokens.name_tokens[token_index]);
        if (reference.is_all) {
            continue;
        }
        const sxml_ir_mode_index mode = find_mode(templates, reference);
        if (mode.value != 0 && templates->modes[mode.value - 1U].as_declaration.value != 0) {
            return templates->modes[mode.value - 1U].as_declaration;
        }
    }
    return (sxml_xslt_ast_node_index){.value = 0};
}
