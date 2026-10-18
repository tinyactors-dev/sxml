/*
 * Evaluator part: global variables with an initializer
 * (src/evaluator_machine_internal.h, D101, D135). Claims no IR kind:
 * sxml_evaluator_evaluate calls sxml_evaluator_start_global_initializers
 * before the main function's body runs.
 *
 * XQuery 4.0, "Variable Declaration" and "Context Value Declaration": a
 * declared variable is computed before the query body, in the
 * computation order (glossary), which is the order of the initializer
 * functions' indices: lowering (src/xquery_prolog.c) adds the initializer
 * functions of the variables the module uses dependencies first, and
 * leaves the others without one, so a variable the query never reads is
 * never computed. A variable whose initializer is running is
 * BEING_COMPUTED, and reading it raises XQDY0054 (a cycle that only a
 * dynamic call can make; lowering rejects the static ones).
 *
 * The context global (glossary: name 0) holds the initial context value:
 * the host's context value, from the main function's focus slot, when the
 * request gave one (coerced to the global's type, XPTY0004), else its
 * initializer.
 *
 * Each initializer runs on the machine as a call of its IR function: a
 * part task pushes a task in the main function's frame and turns it into a
 * CALL of the initializer (the CALL task's fields are the machine's, as
 * the machine's own static calls fill them), then binds the value it
 * hands back. No C recursion (D71).
 *
 * Golden file: src/evaluator_try.c.
 */
#include "evaluator_machine_internal.h"
#include "sort_internal.h"

#include <stdalign.h>
#include <string.h>

/* The variables to compute, in computation order, and the next one, of
   the global table at table_index (D154). */
typedef struct global_computation {
    size_t table_index;
    /* Global numbers (from 1); allocated with the computation. */
    uint32_t *global_numbers;
    size_t global_count;
    size_t next_index;
} global_computation;

static const sxml_ir_global_variable *global_of(const sxml_evaluator_global_table *table,
                                                uint32_t global_number) {
    return sxml_ir_unit_get_global_variable(table->unit,
                                            (sxml_ir_global_index){.value = global_number});
}

static void release_computation(sxml_dynamic_context *context, void *state) {
    sxml_allocator_release(context->allocator, state);
}

/* Binds the context global (name 0) to the host's context value: the
   main function's focus slot, read in the body task's frame. */
static sxml_status bind_context_global(sxml_dynamic_context *context, size_t table_index) {
    const sxml_evaluator_task *body_task = sxml_evaluator_machine_top(context);
    const sxml_ir_slot focus_slot = body_task->function->focus.context_value;
    const sxml_evaluator_global_table *table = &context->global_tables[table_index];
    for (uint32_t global_number = 1; global_number <= table->slot_count; global_number += 1) {
        const sxml_ir_global_variable *variable = global_of(table, global_number);
        sxml_evaluator_slot *global_slot = &table->slots[global_number - 1];
        if (variable->variable_name.index != 0 ||
            global_slot->slot_state == SXML_EVALUATOR_SLOT_STATE_BOUND ||
            focus_slot.value == 0 || focus_slot.value > body_task->function->slot_count) {
            continue;
        }
        const sxml_evaluator_slot *frame_slot =
            &context->frame_stack.slots[body_task->frame_base + focus_slot.value - 1];
        if (frame_slot->slot_state != SXML_EVALUATOR_SLOT_STATE_BOUND) {
            return SXML_STATUS_OK;
        }
        sxml_sequence coerced = sxml_sequence_empty();
        const sxml_status status = sxml_evaluator_coerce(
            context,
            &(sxml_coercion_request){.value = frame_slot->value,
                                     .sequence_type = variable->declared_type,
                                     .role = SXML_IR_COERCION_ROLE_VARIABLE,
                                     .error_record = context->error_record},
            &coerced);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *global_slot = (sxml_evaluator_slot){.value = coerced,
                                             .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/* Whether a global (a uint32_t number) is computed before another: its
   initializer comes first. state is the global table. */
static bool is_computed_before(const void *state, sxml_sort_pair pair) {
    const sxml_evaluator_global_table *table = state;
    return global_of(table, *(const uint32_t *)pair.first)->initializer.value <
           global_of(table, *(const uint32_t *)pair.second)->initializer.value;
}

/* Sorts the computation's globals by initializer index. */
static void sort_by_initializer(const sxml_evaluator_global_table *table,
                                global_computation *computation) {
    sxml_sort_in_place(&(sxml_sort_request){.elements = computation->global_numbers,
                                            .element_count = computation->global_count,
                                            .element_size = sizeof(uint32_t),
                                            .is_before = is_computed_before,
                                            .state = table});
}

/* Pushes a CALL of the initializer of the next global (see the file
   comment), or finishes when none is left. */
static sxml_status step_computation(sxml_dynamic_context *context, sxml_evaluator_task *task) {
    global_computation *computation = task->details.part.state;
    /* The table array may have grown since the last step. */
    sxml_evaluator_global_table *table = &context->global_tables[computation->table_index];
    if (task->phase == 1) {
        const uint32_t computed = computation->global_numbers[computation->next_index];
        table->slots[computed - 1] = (sxml_evaluator_slot){
            .value = sxml_evaluator_machine_take_value(context),
            .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
        computation->next_index += 1;
        task->phase = 0;
    }
    if (computation->next_index == computation->global_count) {
        return sxml_evaluator_machine_finish(context, sxml_sequence_empty());
    }
    const uint32_t global_number = computation->global_numbers[computation->next_index];
    const sxml_ir_function *initializer =
        sxml_ir_unit_get_function(table->unit, global_of(table, global_number)->initializer);
    table->slots[global_number - 1].slot_state = SXML_EVALUATOR_SLOT_STATE_BEING_COMPUTED;
    /* The task pointer is invalid after the push. */
    task->phase = 1;
    const sxml_status status =
        sxml_evaluator_machine_push_expression(context, task->expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_evaluator_task *call = sxml_evaluator_machine_top(context);
    call->task_kind = SXML_EVALUATOR_TASK_KIND_CALL;
    call->phase = 0;
    call->details.call = (sxml_call_state){.function_item = {.representation = 0},
                                           .callee = initializer,
                                           .native_function = NULL,
                                           .arguments = NULL,
                                           .argument_count = 0,
                                           .native_call = NULL};
    return SXML_STATUS_OK;
}

sxml_status sxml_evaluator_start_global_initializers(sxml_dynamic_context *dynamic_context,
                                                     size_t table_index) {
    const sxml_evaluator_global_table *table = &dynamic_context->global_tables[table_index];
    size_t global_count = 0;
    for (uint32_t global_number = 1; global_number <= table->slot_count;
         global_number += 1) {
        if (global_of(table, global_number)->initializer.value != 0 ||
            global_of(table, global_number)->variable_name.index == 0) {
            global_count += 1;
        }
    }
    if (global_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_status status = bind_context_global(dynamic_context, table_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    global_count = 0;
    for (uint32_t global_number = 1; global_number <= table->slot_count;
         global_number += 1) {
        if (global_of(table, global_number)->initializer.value != 0 &&
            table->slots[global_number - 1].slot_state ==
                SXML_EVALUATOR_SLOT_STATE_ABSENT) {
            global_count += 1;
        }
    }
    if (global_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    status = sxml_size_multiply(global_count, sizeof(uint32_t), &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(byte_count, sizeof(global_computation), &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            dynamic_context->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(global_computation)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The numbers follow the computation in its block (whose alignment is
       at least uint32_t's). */
    global_computation *computation = memory;
    *computation = (global_computation){
        .table_index = table_index,
        .global_numbers = (uint32_t *)(computation + 1),
        .global_count = global_count,
        .next_index = 0};
    size_t filled = 0;
    for (uint32_t global_number = 1; global_number <= table->slot_count;
         global_number += 1) {
        if (global_of(table, global_number)->initializer.value != 0 &&
            table->slots[global_number - 1].slot_state ==
                SXML_EVALUATOR_SLOT_STATE_ABSENT) {
            computation->global_numbers[filled] = global_number;
            filled += 1;
        }
    }
    sort_by_initializer(table, computation);
    return sxml_evaluator_machine_push_part_task(
        dynamic_context, &(sxml_evaluator_part_task_state){.step = step_computation,
                                                           .release_state = release_computation,
                                                           .state = computation});
}

/* No kinds; the registry lists the part all the same (D101). */
const sxml_evaluator_part sxml_evaluator_part_global = {
    .kind_handlers = NULL,
    .kind_handler_count = 0,
};
