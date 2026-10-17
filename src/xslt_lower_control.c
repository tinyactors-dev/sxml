/*
 * XSLT 4.0 control flow and variables (src/xslt_lower_internal.h; XSLT
 * 4.0, sections 7, 8, 9, 23; packet 4.7).
 *
 * Golden file: src/xslt_lower_construct.c (instructions as procedures
 * whose steps ask the machine for their parts and return, D71).
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_lower_internal.h"

#include <stdbool.h>
#include <stddef.h>

/* Until the instructions are lowered: the same diagnostic as before. */
static sxml_status continue_unlowered(sxml_xpath_lowering_machine *machine,
                                      sxml_xpath_lowering_frame *frame) {
    (void)machine;
    const sxml_xslt_instruction_locals *locals = frame->locals;
    return sxml_xslt_lowering_report_unsupported(
        locals->lowering,
        (sxml_xslt_static_error){.node = locals->node,
                                 .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                     "this XSLT instruction is not lowered yet")});
}

static const sxml_xpath_lowering_procedure unlowered_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt control (not lowered yet)"),
    .step = continue_unlowered,
    .locals_size = sizeof(sxml_xslt_instruction_locals)};

/* ================================================================ Rules */

static const sxml_xslt_instruction_rule control_rules[] = {
    {.kind = SXML_XSLT_AST_KIND_IF, .procedure = &unlowered_procedure},
};

const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_control = {
    .rules = control_rules, .rule_count = sizeof(control_rules) / sizeof(control_rules[0])};
