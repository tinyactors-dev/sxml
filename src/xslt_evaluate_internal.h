#pragma once

#include "function_library_internal.h"
#include "sxml/status.h"
#include "xslt_lower_internal.h"

/*
 * xsl:evaluate (XSLT 4.0, 10.5; glossary `dynamic_evaluation`; packet
 * 7.3): the internal native row that compiles the target expression at
 * run time and evaluates it as a unit evaluation (D154), and the lowering
 * helper the instruction's procedure (src/xslt_lower_control.c) uses for
 * its xsl:with-param children. Internal to sxml.
 *
 * The row, in no function table, is called by STATIC_CALL with arguments
 * already of these types (the lowering coerces):
 *
 *     evaluate($xpath as xs:string, $parameters as map(*),
 *              $with-params as item()*, $has-with-params as xs:boolean,
 *              $context as item()*, $has-context as xs:boolean,
 *              $namespace-context as item()*,
 *              $has-namespace-context as xs:boolean,
 *              $base-uri as xs:string?, $schema-aware as xs:string?,
 *              $trusted as xs:string?, $functions as map(*))
 *
 * $parameters holds the xsl:with-param values by name; $with-params is the
 * value of the with-params attribute (XTTE3165 unless one map with
 * xs:QName keys), whose entries take precedence (10.5.1). $context is the
 * context-item value (an empty one is no context, XPDY0002 where the
 * target reads it; more than one item is XTTE3210). $namespace-context must be one node (XTTE3170); its
 * in-scope namespaces, the default one included, are the statically
 * known namespaces; without it the instruction's static namespaces are
 * (read from the call's module, D163; prefix 0 is its
 * xpath-default-namespace). $base-uri empty means the instruction's
 * static base URI. $schema-aware and $trusted are the attributes'
 * effective values, "" for no (XTDE0030 for another word). $functions
 * maps the xs:QName of each public stylesheet function to its function
 * items, one per arity (10.5.1, "Function signatures"); the target
 * expression's function table binds those names to the items (function
 * entry kind FUNCTION_ITEM), in front of the F&O library, so the
 * XSLT-defined functions are not available.
 *
 * The first step compiles with the XPath compiler (a static error of the
 * target expression is XTDE3160) and asks for the unit evaluation, whose
 * globals are the parameters (item()*); the second step
 * gives its value. Nothing nests the evaluator in C (D71).
 */
extern const sxml_native_function sxml_xslt_evaluate_function;

/* The parameter map of an instruction's xsl:with-param children (9.10,
   lowered as for xsl:call-template: select or content, the as type,
   XTSE0670), bound in frame->block to *parameters; tunnel parameters go
   to *tunnel_parameters. Pushes a frame. src/xslt_lower_template.c. */
typedef struct sxml_xslt_parameter_map_call {
    const sxml_xslt_instruction_locals *head;
    sxml_xpath_lowering_frame *frame;
    sxml_ir_atom *parameters;
    sxml_ir_atom *tunnel_parameters;
} sxml_xslt_parameter_map_call;

SXML_NODISCARD sxml_status sxml_xslt_lowering_call_parameter_map(
    sxml_xpath_lowering_machine *machine, const sxml_xslt_parameter_map_call *call);

/* The $functions argument (10.5.1, "Function signatures"): a map from the
   xs:QName of each stylesheet function whose visibility is public or final
   to the references of its arities, as request->static_context binds them, bound in request->block. src/xslt_lower_declaration.c. */
typedef struct sxml_xslt_evaluable_functions_request {
    const sxml_xslt_lowering *lowering;
    /* The instruction's: its function table binds the references. */
    const sxml_xpath_static_context *static_context;
    sxml_xpath_lowering_block *block;
} sxml_xslt_evaluable_functions_request;

SXML_NODISCARD sxml_status sxml_xslt_lowering_bind_evaluable_functions(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_evaluable_functions_request *request,
    sxml_ir_atom *functions);
