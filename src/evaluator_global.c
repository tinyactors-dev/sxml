/*
 * Evaluator part: global variables with an initializer
 * (src/evaluator_machine_internal.h, D101). Claims no IR kind:
 * sxml_evaluator_evaluate calls sxml_evaluator_start_global_initializers
 * before the main function's body runs. Packet 3.10 computes declared
 * variables (on first read, XQDY0054 for a cycle); until then a variable
 * with an initializer and no given value is
 * SXML_STATUS_UNSUPPORTED_FEATURE.
 *
 * Golden file: src/evaluator.c.
 *
 * Specification: XQuery 4.0, "Variable Declaration".
 */
#include "evaluator_machine_internal.h"

sxml_status sxml_evaluator_start_global_initializers(sxml_dynamic_context *dynamic_context) {
    const sxml_ir_unit *unit = dynamic_context->unit;
    for (size_t slot_index = 0; slot_index < dynamic_context->global_slot_count;
         slot_index += 1) {
        const sxml_ir_global_variable *variable = sxml_ir_unit_get_global_variable(
            unit, (sxml_ir_global_index){.value = (uint32_t)slot_index + 1});
        if (variable->initializer.value != 0 &&
            dynamic_context->global_slots[slot_index].slot_state !=
                SXML_EVALUATOR_SLOT_STATE_BOUND) {
            return SXML_STATUS_UNSUPPORTED_FEATURE;
        }
    }
    return SXML_STATUS_OK;
}

/* No kinds; the registry lists the part all the same (D101). */
const sxml_evaluator_part sxml_evaluator_part_global = {
    .kind_handlers = NULL,
    .kind_handler_count = 0,
};
