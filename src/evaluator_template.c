/*
 * Evaluator part: template rules (src/evaluator_machine_internal.h, D101).
 * APPLY_TEMPLATES (src/ir_internal.h, "XSLT modes") is claimed here; Phase 4
 * evaluates it. Until then it returns SXML_STATUS_UNSUPPORTED_FEATURE.
 *
 * Golden file: src/evaluator.c.
 *
 * Specification: XSLT 4.0, "Template Rules".
 */
#include "evaluator_machine_internal.h"

/* APPLY_TEMPLATES, until Phase 4. */
static sxml_status sxml_template_step_unsupported(sxml_dynamic_context *dynamic_context,
                                                  sxml_evaluator_task *task) {
    (void)dynamic_context;
    (void)task;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

static const sxml_evaluator_kind_handler sxml_template_kind_handlers[] = {
    {.expression_kind = SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES,
     .step = sxml_template_step_unsupported},
};

const sxml_evaluator_part sxml_evaluator_part_template = {
    .kind_handlers = sxml_template_kind_handlers,
    .kind_handler_count =
        sizeof(sxml_template_kind_handlers) / sizeof(sxml_template_kind_handlers[0]),
};
