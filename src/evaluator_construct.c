/*
 * Evaluator part: node constructors (src/evaluator_machine_internal.h,
 * D101). The seven constructor kinds and SHALLOW_COPY of the IR
 * (src/ir_internal.h, "The node constructor kinds") are claimed here;
 * packet 3.5 evaluates them. Until then each returns
 * SXML_STATUS_UNSUPPORTED_FEATURE.
 *
 * Golden file: src/evaluator.c.
 *
 * Specification: XQuery 4.0, "Node Constructors"; XSLT 4.0,
 * "Constructing Complex Content".
 */
#include "evaluator_machine_internal.h"

/* Every kind of this part, until packet 3.5. */
static sxml_status sxml_construct_step_unsupported(sxml_dynamic_context *dynamic_context,
                                                   sxml_evaluator_task *task) {
    (void)dynamic_context;
    (void)task;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

static const sxml_evaluator_kind_handler sxml_construct_kind_handlers[] = {
    {.expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
     .step = sxml_construct_step_unsupported},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
     .step = sxml_construct_step_unsupported},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
     .step = sxml_construct_step_unsupported},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
     .step = sxml_construct_step_unsupported},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
     .step = sxml_construct_step_unsupported},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
     .step = sxml_construct_step_unsupported},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
     .step = sxml_construct_step_unsupported},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
     .step = sxml_construct_step_unsupported},
};

const sxml_evaluator_part sxml_evaluator_part_construct = {
    .kind_handlers = sxml_construct_kind_handlers,
    .kind_handler_count =
        sizeof(sxml_construct_kind_handlers) / sizeof(sxml_construct_kind_handlers[0]),
};
