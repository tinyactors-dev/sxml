/*
 * Evaluator part: try/catch (src/evaluator_machine_internal.h, D101).
 * TRY_CATCH (src/ir_internal.h) is claimed here; packet 3.6 evaluates it
 * with a catching task (glossary `catching_task`). Until then it returns
 * SXML_STATUS_UNSUPPORTED_FEATURE.
 *
 * Golden file: src/evaluator.c.
 *
 * Specification: XQuery 4.0, "Try/Catch Expressions".
 */
#include "evaluator_machine_internal.h"

/* TRY_CATCH, until packet 3.6. */
static sxml_status sxml_try_step_unsupported(sxml_dynamic_context *dynamic_context,
                                             sxml_evaluator_task *task) {
    (void)dynamic_context;
    (void)task;
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

static const sxml_evaluator_kind_handler sxml_try_kind_handlers[] = {
    {.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH,
     .step = sxml_try_step_unsupported},
};

const sxml_evaluator_part sxml_evaluator_part_try = {
    .kind_handlers = sxml_try_kind_handlers,
    .kind_handler_count = sizeof(sxml_try_kind_handlers) / sizeof(sxml_try_kind_handlers[0]),
};
