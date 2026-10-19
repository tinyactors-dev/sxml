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
#include "xslt_package_internal.h"

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
    /* The package whose literal result elements it applies to (3.4.8:
       namespace aliases are local to a package; D163). */
    uint32_t package;
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
    /* Made by sxml_xslt_declare_accumulators (src/xslt_accumulator.c,
       packet 4.14, D155); NULL before. */
    struct sxml_xslt_accumulator_table *accumulators;
    /* Made by sxml_xslt_declare_packages first (src/xslt_package.c,
       packet 4.16, D160): which declaration each reference binds to. */
    struct sxml_xslt_package_table *packages;
    /* The stream points of the streamed bodies planned so far
       (src/xslt_lower_stream.c, D219); NULL without one. */
    struct sxml_xslt_stream_plan *stream_plan;
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

/* An attribute to parse while lowering (D152's kept ASTs): one whose
   form is known only then, such as an attribute of an extension
   instruction that calls a named template (XSLT 4.0, 10.1.3). */
typedef struct sxml_xslt_attribute_parse {
    /* A TEXT attribute of the stylesheet AST. */
    sxml_xslt_ast_attribute_index attribute;
    /* EXPRESSION or VALUE_TEMPLATE. */
    sxml_xslt_ast_value_kind kind;
} sxml_xslt_attribute_parse;

/* Parses the attribute's text as request.kind, placed in its module
   (D169), and keeps the AST until the lowering ends; *ast is borrowed
   from the kept list. XPST0003 (or a value template's XTSE0350, XTSE0370)
   in the error record, located in the module, when it does not parse; the
   allocator's failures. *ast is NULL on failure. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_parse_attribute(const sxml_xslt_lowering *lowering,
                                                              sxml_xslt_attribute_parse request,
                                                              sxml_ast **ast);

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

/* An AST from sxml_xslt_lowering_parse_attribute to lower to an atom in
   block, in the static context of node. */
typedef struct sxml_xslt_parsed_call {
    const sxml_xslt_lowering *lowering;
    /* Borrowed (kept until the lowering ends). */
    const sxml_ast *ast;
    /* EXPRESSION (the current focus) or VALUE_TEMPLATE (one xs:string). */
    sxml_xslt_ast_value_kind kind;
    sxml_xslt_ast_node_index node;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
} sxml_xslt_parsed_call;

/* From a step: as sxml_xslt_lowering_call_expression and
   sxml_xslt_lowering_call_value_template, for a parsed AST. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_call_parsed(sxml_xpath_lowering_machine *machine,
                                                          const sxml_xslt_parsed_call *call);

/* A sequence constructor to lower: first and its following siblings. */
typedef struct sxml_xslt_sequence_constructor_call {
    const sxml_xslt_lowering *lowering;
    /* 0 for an empty sequence constructor. */
    sxml_xslt_ast_node_index first;
    /* The block its atoms are bound in, or NULL with is_body. */
    sxml_xpath_lowering_block *block;
    bool is_body;
    /* True when destination.atom is content of the result block ends with,
       read first and nowhere else (a literal result element's content):
       its children's LETs are content LETs then, as in a body (D166). */
    bool is_content;
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

/* The unit module (glossary ir_module, D163) of node's static context,
   which a node the instruction constructs takes its base URI from; 0 for
   none. */
uint32_t sxml_xslt_lowering_module_of_node(const sxml_xslt_lowering *lowering,
                                           sxml_xslt_ast_node_index node);

/* The XPath static context of an expression of the AST; NULL for none. */
const sxml_xpath_static_context *
sxml_xslt_lowering_expression_context(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_expression_index expression);

/* The sequence type of node's as attribute; item()* (has_type false)
   without one. */
typedef struct sxml_xslt_declared_type {
    bool has_type;
    sxml_sequence_type sequence_type;
} sxml_xslt_declared_type;

SXML_NODISCARD sxml_status sxml_xslt_lowering_resolve_declared_type(
    const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
    sxml_xslt_declared_type *type);

/* A boolean attribute's fixed text (XSLT 4.0, 2.4 and 3.3: yes, true or
   1; no, false or 0; whitespace trimmed): is_valid false for another. */
typedef struct sxml_xslt_boolean_text {
    bool is_valid;
    bool is_yes;
} sxml_xslt_boolean_text;

sxml_xslt_boolean_text sxml_xslt_lowering_read_boolean_text(sxml_byte_view text);

/* A boolean attribute value template computed at run time: binds in block
   whether normalize-space(value) is yes, true or 1 (xsl:message
   terminate, the cdata attribute). */
typedef struct sxml_xslt_boolean_value {
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
} sxml_xslt_boolean_value;

SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_boolean_value(sxml_xpath_lowerer *lowerer,
                                                                 sxml_xslt_boolean_value request,
                                                                 sxml_ir_atom *atom);

/*
 * The internal native row behind simple content (in no function table,
 * called by STATIC_CALL with arguments of its types, as group by's rows
 * are): merge-text-nodes($value as item()*) as item()* returns $value with
 * zero-length text nodes removed and each run of two or more adjacent
 * text nodes replaced by one xs:untypedAtomic of their concatenated
 * string values (5.7.2, steps 1 and 2; D163). Defined in
 * src/xslt_simple_content.c.
 */
extern const sxml_native_function sxml_xslt_merge_text_nodes_function;

/*
 * The internal native row of xsl:function cache="yes" (10.3.7, D163),
 * called by STATIC_CALL from a cached function's full arity:
 * cached-call($number as xs:integer, $arguments as array(*), $body as
 * function(*)) as item()* calls $body with the members of $arguments once
 * per evaluation and per arguments ($number names the function) and keeps
 * the value in the evaluation memo. Defined in src/function_library_xslt.c.
 */
extern const sxml_native_function sxml_xslt_cached_call_function;

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

/* ----------------------------------------------------------- Validation */

/*
 * The validation an instruction or literal result element asks for (XSLT
 * 4.0, 25; D179): its [xsl:]validation or [xsl:]type attribute, else the
 * [xsl:]default-validation of its static context (strip or preserve;
 * none for xsl:source-document, which validates only when asked, 25).
 * src/xslt_lower_construct.c.
 */
typedef struct sxml_xslt_validation_query {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
} sxml_xslt_validation_query;

/* The construction flag the validation adds to the constructor:
   SXML_IR_CONSTRUCTION_PRESERVES_TYPES for preserve (not for xsl:attribute,
   whose preserve is strip, 25.1.1), else 0. */
uint32_t sxml_xslt_lowering_validation_flags(sxml_xslt_validation_query query);

/* True when the validation is strict, lax or by [xsl:]type: the
   constructed nodes go through sxml_xslt_lowering_bind_validation. */
bool sxml_xslt_lowering_validates(sxml_xslt_validation_query query);

/*
 * Binds value, the nodes the instruction constructed or copied, as its
 * validation makes them in block: for strict, lax and [xsl:]type the
 * validate-items row with XSLT's rules (src/schema_scope_internal.h),
 * otherwise value itself. XTSE1520 for a type that is not an in-scope
 * schema type, XTSE1530 for a complex type on xsl:attribute.
 */
typedef struct sxml_xslt_validation_request {
    sxml_xslt_validation_query query;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom value;
} sxml_xslt_validation_request;

SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_validation(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_validation_request *request,
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

/*
 * 6.5: a match that an error makes false. Writes to *guarded a TRY_CATCH
 * of body whose clauses raise a circularity again (XTDE0640, 9.11, and
 * XTDE3400, 19.6: "The requirement to detect and raise a circularity as a
 * dynamic error overrides this rule") and give false for any other error.
 * Its clauses are added to the unit; the caller adds or binds *guarded.
 */
SXML_NODISCARD sxml_status sxml_xslt_lowering_make_guarded_match(sxml_xpath_lowerer *lowerer,
                                                                 sxml_ir_expression_index body,
                                                                 sxml_ir_expression *guarded);

/* ------------------------------------------------- Streamed evaluation */

/*
 * xsl:source-document with streamable="yes" and its stream points
 * (glossary `stream_point`), lowered to STREAM (src/xslt_lower_stream.c;
 * XSLT 4.0, 18.1 and 19; D219). The plan of a lowering records the stream
 * points of the bodies it streams.
 */
typedef struct sxml_xslt_stream_plan sxml_xslt_stream_plan;

/* An empty plan; the allocator's failures. */
SXML_NODISCARD sxml_status sxml_xslt_create_stream_plan(sxml_allocator allocator,
                                                       sxml_xslt_stream_plan **plan);

/* NULL is allowed. */
void sxml_xslt_release_stream_plan(sxml_xslt_stream_plan *plan);

/* Whether node, an xsl:source-document, is streamed, recording the
   stream points of its body when it is. From a step or outside a
   machine; the analysis's and allocator's failures. */
SXML_NODISCARD sxml_status sxml_xslt_plan_source_document(const sxml_xslt_lowering *lowering,
                                                         sxml_xslt_ast_node_index node,
                                                         bool *is_streamed);

/* True when node (an xsl:for-each) is a stream point of a streamed body. */
bool sxml_xslt_is_stream_point(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node);

/* A streamed construct to lower, bound in block, its atom written to
   *atom. */
typedef struct sxml_xslt_stream_call {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    sxml_xpath_lowering_block *block;
    /* For the document: the href, an xs:string. */
    sxml_ir_atom href;
    sxml_ir_atom *atom;
} sxml_xslt_stream_call;

/* From a step: pushes the frame lowering a stream point (xsl:for-each)
   to its chain of STREAMs, in the current focus. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_call_stream_point(
    sxml_xpath_lowering_machine *machine, const sxml_xslt_stream_call *call);

/* From a step: pushes the frame lowering a streamed xsl:source-document
   to STREAM with OPENS_DOCUMENT over call->href. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_call_streamed_document(
    sxml_xpath_lowering_machine *machine, const sxml_xslt_stream_call *call);

/* ------------------------------------------------------------- Helpers */

/* Adds expression to the unit with no AST node: IR an XSLT instruction
   or declaration lowers to, which has no XPath text of its own. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_add_expression(sxml_xpath_lowerer *lowerer,
                                                             sxml_ir_expression expression,
                                                             sxml_ir_expression_index *index);

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

/* True when node is processed with XSLT 1.0 behaviour (XSLT 4.0, 3.8.1;
   glossary `backwards_compatible_behaviour`): its static context has XPath
   1.0 compatibility mode, set for an effective version below 2.0
   (D197). */
bool sxml_xslt_lowering_has_xslt_1_behaviour(const sxml_xslt_lowering *lowering,
                                             sxml_xslt_ast_node_index node);

/* *atom replaced by the first item of its atomized value, bound in block
   (fn:head(fn:data(.))): what XSLT 1.0 behaviour keeps of xsl:value-of
   and of an attribute value template's enclosed expression (XSLT 4.0,
   11.4.2 and 5.6.1). */
SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_first_atom(sxml_xpath_lowerer *lowerer,
                                                             sxml_xpath_lowering_block *block,
                                                             sxml_ir_atom *atom);

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

/* The alias of node's package whose stylesheet URI is uri; NULL when
   none. */
const sxml_xslt_namespace_alias *sxml_xslt_lowering_find_alias(const sxml_xslt_lowering *lowering,
                                                               sxml_namespace_uri_identifier uri,
                                                               sxml_xslt_ast_node_index node);

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
    /* What the host supplies at run time, read from globals of the unit
       (src/xslt.c binds them by the names below; packet 4.15, D157); an
       atom of kind NONE is the default: the global context item as the
       initial match selection (2.4.3; XTDE0044 without one), no
       parameters. */
    sxml_ir_atom initial_match_selection;
    sxml_ir_atom template_parameters;
    sxml_ir_atom tunnel_parameters;
    /* A function call invocation (2.4.5) when initial_function is not 0:
       the stylesheet function of that name and arity function_arity is
       called with the members of the array function_arguments (XTDE0041
       for none that is public or final). */
    sxml_name_identifier initial_function;
    uint32_t function_arity;
    sxml_ir_atom function_arguments;
} sxml_xslt_invocation;

/* The namespace of the globals an invocation reads (sxml_xslt_invocation)
   and their local names: the initial match selection (item()*), the
   template and tunnel parameters (two maps from xs:QName, empty when not
   bound) and the arguments of the initial function (an array). */
#define SXML_XSLT_INVOCATION_NAMESPACE "urn:x-sxml:invocation"
#define SXML_XSLT_INVOCATION_INITIAL_MATCH_SELECTION "initial-match-selection"
#define SXML_XSLT_INVOCATION_TEMPLATE_PARAMETERS "template-parameters"
#define SXML_XSLT_INVOCATION_TUNNEL_PARAMETERS "tunnel-parameters"
#define SXML_XSLT_INVOCATION_FUNCTION_ARGUMENTS "function-arguments"

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
/*
 * One module of the unit per AST static context (glossary ir_module,
 * D163): its static base URI (xml:base), namespaces, default collation and
 * its package's decimal formats (3.4.8, 5.5), which the function calls of
 * its expressions read at run time; writes contexts[n].ir_module_number.
 * The unit's own static context gets the top-level package's decimal
 * formats. src/xslt_lower_declaration.c.
 */
SXML_NODISCARD sxml_status sxml_xslt_declare_static_context_modules(
    const sxml_xslt_lowering *lowering, sxml_xpath_static_context *contexts);

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

/* Binds in mark->block whether the template function being lowered was
   called in temporary output state (the tunnel parameter that
   sxml_xslt_lowering_mark_temporary_output_state puts in) to
   *is_temporary, an xs:boolean; the constant false outside a template
   function. mark->node is the instruction that asks. src/xslt_output.c. */
SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_received_output_state(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_output_state_mark *mark,
    sxml_ir_atom *is_temporary);

/* Writes the atom of the tunnel parameters the template function being
   lowered received to *tunnel_parameters and returns true; false outside a
   template function. src/xslt_lower_template.c. */
bool sxml_xslt_lowering_received_tunnel_parameters(const sxml_xslt_lowering *lowering,
                                                   sxml_ir_atom *tunnel_parameters);

/* True when instruction is an extension instruction named after a named
   template it binds to (XSLT 4.0, 10.1.3), which it then calls as
   xsl:call-template would, its attributes giving the parameters.
   src/xslt_lower_template.c. */
bool sxml_xslt_lowering_calls_template(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_ast_node_index instruction);

/* -------------------------------------------------------- Named types */

/*
 * xsl:item-type and xsl:record-type (5.4; src/xslt_named_type.c): declares
 * the record types in the type registry, resolves the named item types,
 * gives every static context the named item types its package sees and
 * defines the records' fields. After sxml_xslt_declare_packages, before
 * anything resolves a type; outside a machine. Returns
 * SXML_STATUS_STATIC_ERROR with the error record filled (XTSE4030,
 * XTSE4035, XTSE4050, XPST0051 and the other errors of resolving types),
 * and the allocator's, name pool's and type registry's failures. What it
 * keeps lives in the package table's memory.
 */
SXML_NODISCARD sxml_status sxml_xslt_declare_named_types(const sxml_xslt_lowering *lowering,
                                                         sxml_xpath_static_context *static_contexts);

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
    /* A function call invocation (2.4.5) of the function of this name
       with initial_function_arity arguments; 0 for none. */
    sxml_name_identifier initial_function;
    uint32_t initial_function_arity;
    /* The initial match selection is the host's (a global of the unit),
       not the global context item. */
    bool has_initial_match_selection;
    /* The parameters of the initial template or of the template rules of
       the initial mode are the host's (two globals of the unit, empty
       maps by default); without it the invocation passes none. */
    bool reads_invocation_parameters;
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
