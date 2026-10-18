#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "ir_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"
#include "xpath_lower_internal.h"
#include "xpath_static_internal.h"
#include "xslt_ast_internal.h"

/*
 * Lowering XSLT 4.0 (docs/DECISIONS.md D46, D55, D102, D103, D110, D124,
 * D142; packet 4.6). Internal to sxml.
 *
 * A sealed stylesheet AST (src/xslt_ast_internal.h) is lowered into an IR
 * unit by the one lowering machine of src/xpath_lower_internal.h, with a
 * lowerer of language SXML_LANGUAGE_XSLT: no machinery of its own. XSLT
 * instructions are not AST kinds, so each is a lowering procedure pushed
 * with sxml_xpath_lowering_push_procedure (D103); every XPath expression,
 * pattern and value template of the stylesheet is lowered through the
 * embedding entry, with the XPath static context made once for each
 * static context of the AST, into the function being lowered. Nothing is
 * evaluated while lowering, and no step recurses (D71).
 *
 * Sequence constructors (XSLT 4.0, 5.7). The children of a node, from a
 * first child to the last, are lowered one after another in the caller's
 * block, each to one atom (instructions, literal result elements, text
 * nodes); the sequence constructor's value is the SEQUENCE of those atoms,
 * which the instructions that use it as content give as one atom: one
 * enclosed expression in the sense of D110, so adjacent atomic values are
 * joined with single spaces across instructions (5.7.1). Variables an
 * instruction brings into scope (xsl:variable, packet 4.7, by
 * sxml_xpath_lowerer_push_variable) stay in scope until the sequence
 * constructor that holds it ends.
 *
 * Node construction (11):
 * - A literal result element (11.1) is an ELEMENT_CONSTRUCTOR with
 *   construction flag XSLT_RULES (D124), an xs:QName constant name, the
 *   namespace bindings of its element in the stylesheet tree without
 *   the excluded namespaces and the literal namespace URIs of namespace
 *   aliases (11.1.3, 11.1.4), and as content one atom per attribute (an
 *   ATTRIBUTE_CONSTRUCTOR of the attribute value template) and one for the
 *   sequence constructor.
 * - Value templates (5.6) are their string templates lowered here: fixed
 *   parts are constants, each enclosed expression's value becomes simple
 *   content (5.7.2, sxml_xslt_simple_content_function: adjacent text
 *   nodes merged, then atomized and joined with spaces), and the parts are
 *   joined by "||". A template of fixed parts only is one constant.
 * - xsl:element, xsl:attribute, xsl:text, xsl:value-of, xsl:comment,
 *   xsl:processing-instruction, xsl:namespace, xsl:document are the
 *   constructor kinds of the same name (src/xslt_lower_construct.c);
 *   xsl:copy is SHALLOW_COPY; xsl:copy-of a MAP over the selected items
 *   that copies each node by SHALLOW_COPY with its attributes and
 *   children as content; xsl:map, xsl:map-entry and xsl:record are
 *   MAP_CONSTRUCTOR, xsl:array a call of array:of-members, xsl:array-member
 *   a call of fn:jtree; xsl:sequence its value.
 *
 * Errors: static errors (XTSE) are reported at the instruction (module
 * URI, line, column); XSLT's dynamic error codes are raised where the
 * evaluator's XQuery codes would be wrong, by a TRY_CATCH that maps one
 * code to the other around the constructor alone. Instructions of later
 * packets give SXML_STATUS_UNSUPPORTED_FEATURE with the diagnostic filled.
 *
 * Thread safety: the thread of the item heap (D35).
 */

/* ----------------------------------------------------------- Aliases */

/* One xsl:namespace-alias in force (11.1.4): literal namespace URI
   stylesheet_uri is output as result_binding. */
typedef struct sxml_xslt_namespace_alias {
    sxml_namespace_uri_identifier stylesheet_uri;
    sxml_namespace_binding result_binding;
    /* The import precedence of the declaration that won. */
    uint32_t import_precedence;
} sxml_xslt_namespace_alias;

/* ---------------------------------------------------- The lowering */

/* The templates and modes of a stylesheet (src/xslt_lower_template.c,
   packet 4.8, D144). */
typedef struct sxml_xslt_template_lowering sxml_xslt_template_lowering;

/* The stylesheet functions and attribute sets of a stylesheet
   (src/xslt_lower_declaration.c, packet 4.12, D150). */
typedef struct sxml_xslt_component_lowering sxml_xslt_component_lowering;

/*
 * One stylesheet lowering (glossary `stylesheet_lowering`). Made by
 * sxml_xslt_lower_initial_template; procedures read it through the
 * pointer at the start of their locals and never change it. Every pointer
 * is borrowed for the lowering.
 */
typedef struct sxml_xslt_lowering {
    const sxml_xslt_ast *ast;
    sxml_xpath_lowerer *lowerer;
    /* static_contexts[n] is the XPath static context of the AST's static
       context n (entry 0 unused): its namespaces with the
       xpath-default-namespace as prefix 0, base URI, the function table. */
    const sxml_xpath_static_context *static_contexts;
    uint32_t static_context_count;
    sxml_xslt_namespace_alias *namespace_aliases;
    size_t namespace_alias_count;
    sxml_error_record *error_record;
    /* The global variables and parameters (9.5, packet 4.7): global
       number n is global_declarations[n - 1], declared by
       global_nodes[n - 1]; global_count + 1 is the context global. Set by
       sxml_xslt_lowering_declare_global_variables; NULL and 0 without. */
    sxml_variable_declaration *global_declarations;
    sxml_xslt_ast_node_index *global_nodes;
    size_t global_count;
    /* Made by sxml_xslt_lower_templates; NULL before. Its state for the
       template being lowered changes while templates are lowered. */
    sxml_xslt_template_lowering *templates;
    /* Made by sxml_xslt_declare_components; NULL before. */
    sxml_xslt_component_lowering *components;
    /* The kept ASTs (glossary `kept_ast`, D152): short XPath texts and
       match expressions that procedures parse and lower, released when the
       lowering ends. */
    struct sxml_xslt_kept_asts *kept_asts;
} sxml_xslt_lowering;

/* ASTs a lowering keeps until it ends (glossary `kept_ast`). */
typedef struct sxml_xslt_kept_asts {
    sxml_allocator allocator;
    sxml_ast **asts;
    size_t ast_count;
    size_t ast_capacity;
} sxml_xslt_kept_asts;

/* Keeps ast, owned, until the lowering ends; on failure ast is destroyed
   (the allocator's failures; SXML_STATUS_INVALID_ARGUMENT without a kept
   list). From a step or outside a machine. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_keep_ast(const sxml_xslt_lowering *lowering,
                                                       sxml_ast *ast);

/* Destroys the kept ASTs and releases the list's memory. */
void sxml_xslt_release_kept_asts(sxml_xslt_kept_asts *kept_asts);

/* What every XSLT lowering procedure's locals start with; it is pushed
   with these as its initial locals. */
typedef struct sxml_xslt_instruction_locals {
    const sxml_xslt_lowering *lowering;
    /* The instruction, literal result element or text node lowered. */
    sxml_xslt_ast_node_index node;
} sxml_xslt_instruction_locals;

/* ------------------------------------------------------ Instruction rules */

/*
 * Which procedure lowers an instruction kind (glossary `instruction_rule`).
 * The procedure's frame is pushed in the sequence constructor's block with
 * an atom destination; its locals start with sxml_xslt_instruction_locals.
 */
typedef struct sxml_xslt_instruction_rule {
    sxml_xslt_ast_kind kind;
    const sxml_xpath_lowering_procedure *procedure;
} sxml_xslt_instruction_rule;

typedef struct sxml_xslt_instruction_rule_view {
    const sxml_xslt_instruction_rule *rules;
    size_t rule_count;
} sxml_xslt_instruction_rule_view;

/* The instructions of src/xslt_lower_construct.c (packet 4.6). Later
   packets define their own views and add them to the list in
   src/xslt_lower.c. */
extern const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_construct;
/* The instructions of src/xslt_lower_group.c (packet 4.9). */
extern const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_group;
/* The instructions of src/xslt_lower_control.c (packet 4.7). */
extern const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_control;
/* The instructions of src/xslt_lower_template.c (packet 4.8). */
extern const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_template;
/* The instructions of src/xslt_lower_number.c (packet 4.11). */
extern const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_number;

/* --------------------------------------------------------- Parts to call */

/* An expression of the stylesheet AST (EXPRESSION or VALUE_TEMPLATE) to
   lower to an atom in block. */
typedef struct sxml_xslt_expression_call {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_expression_index expression;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
    /* The focus while it is lowered (sxml_xpath_lowering_embedding.focus);
       NULL keeps the current one. Ignored for value templates. */
    sxml_xpath_lowering_focus *focus;
} sxml_xslt_expression_call;

/* From a step: an EXPRESSION through the embedding entry, in the
   expression's static context and the current focus. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_call_expression(
    sxml_xpath_lowering_machine *machine, const sxml_xslt_expression_call *call);

/* From a step: a VALUE_TEMPLATE as one xs:string (5.6; see above). */
SXML_NODISCARD sxml_status sxml_xslt_lowering_call_value_template(
    sxml_xpath_lowering_machine *machine, const sxml_xslt_expression_call *call);

/* A sequence constructor to lower: first and its following siblings. */
typedef struct sxml_xslt_sequence_constructor_call {
    const sxml_xslt_lowering *lowering;
    /* 0 for an empty sequence constructor. */
    sxml_xslt_ast_node_index first;
    /* The block its atoms are bound in, or NULL with is_body. */
    sxml_xpath_lowering_block *block;
    bool is_body;
    sxml_xpath_lowering_destination destination;
    /* The focus while it is lowered, restored afterwards (the selected
       item of xsl:copy select); NULL keeps the current one. Must stay
       where it is until the frame finishes. */
    sxml_xpath_lowering_focus *focus;
} sxml_xslt_sequence_constructor_call;

/* From a step: pushes the frame that lowers the sequence constructor. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_call_sequence_constructor(
    sxml_xpath_lowering_machine *machine, const sxml_xslt_sequence_constructor_call *call);

/* The first child of node that is part of its sequence constructor: the
   children before it (xsl:param, xsl:sort, xsl:context-item and others an
   instruction reads itself) skipped. */
sxml_xslt_ast_node_index sxml_xslt_lowering_first_constructor_child(const sxml_xslt_ast *ast,
                                                                    sxml_xslt_ast_node_index node);

/* The children of a sequence constructor with xsl:on-empty or
   xsl:on-non-empty children (8.5.2, 8.5.3), and the atoms of its other
   children, in order (borrowed until the frame finishes). */
typedef struct sxml_xslt_conditional_content_call {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index first;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom_view atoms;
    /* Where the sequence constructor's value is written. */
    sxml_ir_atom *atom;
    /* The scope entries before the sequence constructor's: one more for
       each xsl:variable child after this. */
    size_t scope_mark;
} sxml_xslt_conditional_content_call;

/* From a step: pushes the frame that binds the value of such a sequence
   constructor in call->block (src/xslt_lower_control.c, packet 4.7). */
SXML_NODISCARD sxml_status sxml_xslt_lowering_call_conditional_content(
    sxml_xpath_lowering_machine *machine, const sxml_xslt_conditional_content_call *call);

/* ------------------------------------------------------------- Sorting */

/* A sort key specification (13.1) to apply to a sequence. */
typedef struct sxml_xslt_sort_call {
    const sxml_xslt_lowering *lowering;
    /* The instruction whose xsl:sort children are the specification
       (xsl:for-each, xsl:apply-templates, xsl:perform-sort,
       xsl:for-each-group). */
    sxml_xslt_ast_node_index node;
    /* The initial sequence; for xsl:for-each-group its group arrays
       (src/xslt_group_internal.h), each sorted with its initial item as
       the context item and the group as the current group (13.1.1). */
    sxml_ir_atom input;
    bool sorts_groups;
    /* Nonzero for xsl:merge (15.4): node is the first xsl:merge-source,
       whose xsl:merge-key children give the order, and each input item is
       an array whose member tuple_key_offset + K is its merge key K
       (from 0), computed already. */
    uint32_t tuple_key_offset;
    /* Where the sorted sequence is bound, and its atom. */
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
} sxml_xslt_sort_call;

/* From a step: pushes the frame that binds the sorted sequence: a SORT
   whose keys are the xsl:sort children's (src/xslt_lower_group.c, packet
   4.9, D152). XTSE1015, XTSE1017, XTSE0020 are static errors. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_call_sort(sxml_xpath_lowering_machine *machine,
                                                        const sxml_xslt_sort_call *call);

/* True when node has xsl:sort children. */
bool sxml_xslt_lowering_has_sort(const sxml_xslt_lowering *lowering,
                                 sxml_xslt_ast_node_index node);

/* ------------------------------------------------------ Global variables */

/* Before the main function is lowered: the global variables and
   parameters in force (one per name by import precedence, XTSE0630),
   declared in every static context of static_contexts (the lowering's,
   writable). src/xslt_lower_control.c, packet 4.7. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_declare_global_variables(
    sxml_xslt_lowering *lowering, sxml_xpath_static_context *static_contexts);

/* After the main function: their initializers in dependency order
   (XTDE0640 for a cycle, XTDE0050 for a required parameter) and the
   unit's globals, then the context global (D135), whose value the main
   function's context value slot gives. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_add_global_variables(
    const sxml_xslt_lowering *lowering);

/* Releases what declare allocated. */
void sxml_xslt_lowering_release_global_variables(sxml_xslt_lowering *lowering);

/* ------------------------------------------------------- Simple content */

/* A value to turn into simple content (5.7.2), bound in block. */
typedef struct sxml_xslt_simple_content_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
    /* An xs:string atom. */
    sxml_ir_atom separator;
} sxml_xslt_simple_content_request;

/* Binds the xs:string the simple content of request.value gives. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_simple_content(
    sxml_xpath_lowerer *lowerer, sxml_xslt_simple_content_request request, sxml_ir_atom *atom);

/*
 * The internal native row behind simple content (in no function table,
 * called by STATIC_CALL with arguments of its types, as group by's rows
 * are): merge-text-nodes($value as item()*) as item()* returns $value with
 * each run of two or more adjacent text nodes replaced by one
 * xs:untypedAtomic of their concatenated string values (5.7.2, steps 1
 * and 2; sxml has no zero-length text nodes, D40). Defined in
 * src/xslt_simple_content.c.
 */
extern const sxml_native_function sxml_xslt_merge_text_nodes_function;

/* -------------------------------------------------------------- Copies */

/* The deep copies of value's items (11.9.2), as xsl:copy-of makes them
   with construction_flags (bits of sxml_ir_construction_flag), bound in
   block: fn:copy-of's body (packet 4.10). src/xslt_lower_construct.c. */
typedef struct sxml_xslt_copy_request {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
    uint32_t construction_flags;
} sxml_xslt_copy_request;

SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_copy_of(sxml_xpath_lowerer *lowerer,
                                                           sxml_xslt_copy_request request,
                                                           sxml_ir_atom *atom);

/* ----------------------------------------------------------- Error codes */

/* A dynamic error of the evaluator replaced by XSLT's (both in the err
   namespace, static ASCII such as "XQDY0137"). */
typedef struct sxml_xslt_error_mapping {
    sxml_byte_view caught_code;
    sxml_status raised_status;
    sxml_byte_view raised_code;
    sxml_byte_view description;
} sxml_xslt_error_mapping;

typedef struct sxml_xslt_error_mapping_view {
    const sxml_xslt_error_mapping *mappings;
    size_t mapping_count;
} sxml_xslt_error_mapping_view;

/* An expression to bind in block inside a TRY_CATCH whose clauses raise
   mappings' codes instead of the caught ones. */
typedef struct sxml_xslt_mapped_expression {
    sxml_xpath_lowering_block *block;
    sxml_ir_expression expression;
    sxml_xslt_error_mapping_view mappings;
} sxml_xslt_mapped_expression;

SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_mapped_expression(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_mapped_expression *request, sxml_ir_atom *atom);

/* ------------------------------------------------------------- Helpers */

/* Adds expression and binds it in block. */
typedef struct sxml_xslt_expression_to_bind {
    sxml_xpath_lowering_block *block;
    sxml_ir_expression expression;
} sxml_xslt_expression_to_bind;

SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_expression(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_expression_to_bind *request, sxml_ir_atom *atom);

/* A call of a function of the static context's table, its arguments
   coerced to the parameter types (as a static call in XPath), bound in
   block. UNSUPPORTED_FEATURE when the table lacks it. */
typedef struct sxml_xslt_library_call {
    sxml_xpath_lowering_block *block;
    sxml_well_known_namespace function_namespace;
    sxml_byte_view local_name;
    sxml_ir_atom_view arguments;
} sxml_xslt_library_call;

SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_library_call(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_library_call *request, sxml_ir_atom *atom);

/* Fills the error record with code_and_description ("XTSE0010 text",
   static) at node and returns SXML_STATUS_STATIC_ERROR. */
typedef struct sxml_xslt_static_error {
    sxml_xslt_ast_node_index node;
    sxml_byte_view code_and_description;
} sxml_xslt_static_error;

SXML_NODISCARD sxml_status sxml_xslt_lowering_report_static_error(
    const sxml_xslt_lowering *lowering, sxml_xslt_static_error error);

/* Fills the diagnostic (static text) at node and returns
   SXML_STATUS_UNSUPPORTED_FEATURE. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_report_unsupported(const sxml_xslt_lowering *lowering,
                                                                 sxml_xslt_static_error error);

/* The in-scope namespaces of node's element in its stylesheet module
   (with the default namespace as prefix 0), borrowed from the module's
   tree; empty for a text node. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_get_element_namespaces(
    const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
    sxml_namespace_binding_view *namespace_bindings);

/* The alias whose stylesheet URI is uri; NULL when none. */
const sxml_xslt_namespace_alias *sxml_xslt_lowering_find_alias(const sxml_xslt_lowering *lowering,
                                                               sxml_namespace_uri_identifier uri);

/* ------------------------------------------------- Templates and modes */

/*
 * Lowers every xsl:template into a template function (src/ir_internal.h:
 * five parameters, the focus and two parameter maps), its pattern into a
 * match function (D118), and adds the modes of the stylesheet (6.9) with
 * their ordered rules (6.8) and built-in templates (6.10) to the unit;
 * makes lowering->templates, which sxml_xslt_release_templates releases
 * (also after a failure). Outside a lowering machine, before the main
 * function, in a unit without modes. Returns SXML_STATUS_STATIC_ERROR
 * (XTSE0545, XTSE0650, XTSE0660, XTSE0670, XTSE0680, XTSE0690, XTSE3085,
 * XTSE4005, XTSE4010 and those of patterns and expressions) with the error
 * record filled, SXML_STATUS_UNSUPPORTED_FEATURE, and the failures of
 * lowering and the allocator. Defined in src/xslt_lower_template.c.
 */
SXML_NODISCARD sxml_status sxml_xslt_lower_templates(sxml_xslt_lowering *lowering);

/* Releases lowering->templates and sets it NULL; NULL is allowed. */
void sxml_xslt_release_templates(sxml_xslt_lowering *lowering);

/* How the transformation starts (2.4). */
typedef struct sxml_xslt_invocation {
    /* A template to call (call-template invocation, 2.4.4) with the global
       context item, which may be absent, as its focus; 0 applies templates
       to the global context item (2.4.3). */
    sxml_xslt_ast_node_index template_node;
    /* The initial mode of an apply-templates invocation; 0 with
       selects_unnamed_mode false is the principal module's default
       mode. */
    sxml_name_identifier initial_mode;
    bool selects_unnamed_mode;
    /* The initial template asked for does not exist: the main function
       raises XTDE0040 (2.4.4). */
    bool is_unknown_template;
} sxml_xslt_invocation;

typedef struct sxml_xslt_invocation_request {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_invocation invocation;
    /* A block of the main function, which is being lowered (outside a
       machine). */
    sxml_xpath_lowering_block *block;
} sxml_xslt_invocation_request;

/* Binds in block whether the current function has a focus: true(), or
   false() from a TRY_CATCH of the XPDY0002 reading it raises (the main
   function without a global context item). */
SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_has_focus(sxml_xpath_lowerer *lowerer,
                                                             sxml_xpath_lowering_block *block,
                                                             sxml_ir_atom *atom);

/* Binds the invocation in request->block after sxml_xslt_lower_templates
   and writes its value's atom. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_invocation(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_invocation_request *request, sxml_ir_atom *atom);

/* ----------------------------------------------- Functions, attribute sets */

/*
 * Before anything is lowered: every xsl:function that is a component
 * (10.3.4: not eclipsed, not given up for an extension function under
 * override-extension-function="no") gets one IR function per arity of its
 * arity range, and every static context of static_contexts (the
 * lowering's, writable) a function table that finds them before the table
 * it had (unprefixed names first in no namespace when a function is,
 * 10.3.1); every attribute set (all xsl:attribute-set of one name) gets
 * two IR functions of three parameters, the focus (item()?, position,
 * size, as a template function) and the same with an absent focus. Makes
 * lowering->components, which sxml_xslt_release_components releases (also
 * after a failure). Returns SXML_STATUS_STATIC_ERROR (XTSE0080, XTSE0270,
 * XTSE0580, XTSE0710, XTSE0720, XTSE0760, XTSE0761, XTSE0769, XTSE0770) with the
 * error record filled, and the failures of lowering and the allocator.
 * Defined in src/xslt_lower_declaration.c.
 */
SXML_NODISCARD sxml_status sxml_xslt_declare_components(
    sxml_xslt_lowering *lowering, sxml_xpath_static_context *static_contexts);

/* After sxml_xslt_lower_templates, outside a machine: the bodies of the
   functions sxml_xslt_declare_components added (XTTE0780 for a result
   that does not match a function's as type), and the decimal formats of
   xsl:decimal-format as the unit's (XTSE1290, XTSE1300); the unit's
   static context is set here, once. */
SXML_NODISCARD sxml_status sxml_xslt_lower_components(const sxml_xslt_lowering *lowering);

/* Releases lowering->components and sets it NULL; NULL is allowed. */
void sxml_xslt_release_components(sxml_xslt_lowering *lowering);

/* The [xsl:]use-attribute-sets attribute of node (a literal result
   element, xsl:element, xsl:copy) to bind in block. */
typedef struct sxml_xslt_attribute_set_call {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    sxml_xpath_lowering_block *block;
} sxml_xslt_attribute_set_call;

/* Binds in call->block the attribute nodes of the attribute sets the
   attribute names, in order, each called with the current focus (10.2.3),
   and writes their sequence's atom; XTSE0710 for a name no attribute set
   has. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_attribute_sets(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_attribute_set_call *call, sxml_ir_atom *atom);

/* The xsl:global-context-item check (3.4.9) to bind in block, a block of
   the main function before the invocation. */
typedef struct sxml_xslt_global_context_item_call {
    const sxml_xslt_lowering *lowering;
    sxml_xpath_lowering_block *block;
} sxml_xslt_global_context_item_call;

/* Binds, when the stylesheet declares one, the check of the global context
   item: XTDE3086 for none when required, XTTE0590 for one of another type,
   XTSE3088 for one when absent is asked for. XTSE3087 and XTSE3089 are
   reported as static errors. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_global_context_item(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_global_context_item_call *call);

/* ------------------------------------------- Output (packet 4.13, D156) */

/* The instructions of src/xslt_output.c: xsl:result-document. */
extern const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_output;

/* Whether node is lexically in temporary output state (XSLT 4.0, 26.2.6):
   inside xsl:variable, xsl:param, xsl:with-param, xsl:function, xsl:key,
   xsl:sort, xsl:accumulator-rule or xsl:merge-key of its template or
   declaration. src/xslt_output.c. */
bool sxml_xslt_lowering_is_in_temporary_output_state(const sxml_xslt_lowering *lowering,
                                                     sxml_xslt_ast_node_index node);

/* An instruction that passes tunnel parameters to templates. */
typedef struct sxml_xslt_output_state_mark {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    sxml_xpath_lowering_block *block;
} sxml_xslt_output_state_mark;

/* When mark->node is in temporary output state, binds in mark->block the
   map *tunnel_parameters (in/out) with the entry that tells the templates
   it calls so (src/xslt_output_internal.h); otherwise leaves it as it is. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_mark_temporary_output_state(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_output_state_mark *mark,
    sxml_ir_atom *tunnel_parameters);

/* Writes the atom of the tunnel parameters the template function being
   lowered received to *tunnel_parameters and returns true; false outside a
   template function. src/xslt_lower_template.c. */
bool sxml_xslt_lowering_received_tunnel_parameters(const sxml_xslt_lowering *lowering,
                                                   sxml_ir_atom *tunnel_parameters);

/* ----------------------------------------------------------- The entry */

/* A stylesheet to lower with the invocation that starts it (until packet
   4.15 lowers the transformation of include/sxml/xslt.h). */
typedef struct sxml_xslt_lowering_request {
    /* Sealed; borrowed for the call. */
    const sxml_xslt_ast *ast;
    /* Its function table, heap, allocator and limits; borrowed. */
    sxml_processor *processor;
    /* The TEMPLATE the main function calls (2.4.4); 0 means the template
       named initial_template, else (with no such template) templates are
       applied to the global context item (2.4.3, as for a simplified
       stylesheet module, 3.7). */
    sxml_xslt_ast_node_index template_node;
    /* 0 means xsl:initial-template. */
    sxml_name_identifier initial_template;
    /* Applies templates in the initial mode whatever the templates. */
    bool applies_templates;
    /* As sxml_xslt_invocation. */
    sxml_name_identifier initial_mode;
    bool selects_unnamed_mode;
    /* The main function returns a document node holding the result, as
       the principal result is built (2.4, build-tree): a DOCUMENT
       constructor with XSLT's rules around the value. Otherwise it returns
       the raw result. */
    bool builds_result_document;
    /* sxml_ir_unit_configuration.maximum_expression_count. */
    uint32_t maximum_expression_count;
    sxml_error_record *error_record;
} sxml_xslt_lowering_request;

/*
 * Lowers the stylesheet's templates and modes and, as the main function of
 * a new unit whose focus is the global context item, the invocation; runs
 * sxml_ir_standard_passes,
 * seals the unit and writes it, owned, to *unit. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an unsealed AST or no template,
 * SXML_STATUS_STATIC_ERROR (XTSE, XPST) and SXML_STATUS_TYPE_ERROR with
 * the error record filled, SXML_STATUS_UNSUPPORTED_FEATURE for an
 * instruction not lowered yet (the diagnostic filled), and the failures of
 * lowering and the allocator. *unit is untouched on failure.
 */
SXML_NODISCARD sxml_status
sxml_xslt_lower_initial_template(const sxml_xslt_lowering_request *request, sxml_ir_unit **unit);
