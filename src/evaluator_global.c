/*
 * Evaluator part: global variables with an initializer
 * (src/evaluator_machine_internal.h, D101, D135, D234). Claims no IR
 * kind: the machine's GLOBAL_READ (src/evaluator.c) asks it to compute a
 * global, and sxml_evaluator_evaluate asks it to bind the context global.
 *
 * Globals are computed on first read (XQuery 4.0, 2.4.4 and "Variable
 * Declaration": a variable whose value is not needed need not be
 * evaluated; XSLT 4.0, 9.12: a global variable is evaluated when its value
 * is needed, so an unread abstract variable never raises XTDE3052,
 * 3.4.5.2). The global-read pass (src/ir_global_read.c) puts a GLOBAL_READ
 * before every expression that reads a global with an initializer; the
 * first one evaluated for a global that is still ABSENT pushes this
 * part's task, which calls the initializer and binds the value. A global
 * whose initializer is running is BEING_COMPUTED, and reading it raises
 * the language's circularity error (XQDY0054, XTDE0640; a cycle only a
 * dynamic call can make, lowering rejects the static ones). An initializer
 * that fails leaves its global ABSENT again.
 *
 * The context global (glossary: name 0) holds the initial context value:
 * the host's context value, from the main function's focus slot, when the
 * request gave one (coerced to the global's type, XPTY0004), else its
 * initializer, on first read as any global.
 *
 * Each initializer runs on the machine as a call of its IR function: the
 * part task, past which no try/catch around the read catches (an error
 * of the initializer is caught only inside it: XSLT 4.0, 2.4.2 and 8.3),
 * pushes a task in the reader's frame and turns it into a CALL of the
 * initializer (the CALL task's fields are the machine's, as the machine's
 * own static calls fill them), then binds the value it hands back. No C
 * recursion (D71).
 *
 * Golden file: src/evaluator_try.c.
 */
#include "evaluator_machine_internal.h"

#include <stdalign.h>

/* The global being computed, of the global table at table_index (D154);
   unit tells a table that is gone from one added at its index later. */
typedef struct global_computation {
    size_t table_index;
    const sxml_ir_unit *unit;
    uint32_t global_number;
    bool is_bound;
    /* The current mode and template rule of the reader, absent while the
       initializer runs (XSLT 4.0, 6.7 and 9.5: a global is evaluated
       with no current template rule, in the unnamed mode). */
    bool has_saved_context;
    sxml_ir_mode_index saved_mode;
    uint32_t saved_template_rule_index;
} global_computation;

static const sxml_ir_global_variable *global_of(const sxml_evaluator_global_table *table,
                                                uint32_t global_number) {
    return sxml_ir_unit_get_global_variable(table->unit,
                                            (sxml_ir_global_index){.value = global_number});
}

/* The computation's global slot, or NULL when its table is gone. */
static sxml_evaluator_slot *computed_slot(sxml_dynamic_context *context,
                                          const global_computation *computation) {
    if (computation->table_index >= context->global_table_count) {
        return NULL;
    }
    sxml_evaluator_global_table *table = &context->global_tables[computation->table_index];
    if (table->unit != computation->unit || computation->global_number == 0 ||
        computation->global_number > table->slot_count) {
        return NULL;
    }
    return &table->slots[computation->global_number - 1];
}

/* Gives the reader its current mode and template rule back. */
static void restore_context(sxml_dynamic_context *context, global_computation *computation) {
    if (computation->has_saved_context) {
        context->current_mode = computation->saved_mode;
        context->current_template_rule_index = computation->saved_template_rule_index;
        computation->has_saved_context = false;
    }
}

/* Releases the computation; a global it did not bind (its initializer
   failed) is ABSENT again. */
static void release_computation(sxml_dynamic_context *context, void *state) {
    global_computation *computation = state;
    restore_context(context, computation);
    sxml_evaluator_slot *slot = computed_slot(context, computation);
    if (!computation->is_bound && slot != NULL &&
        slot->slot_state == SXML_EVALUATOR_SLOT_STATE_BEING_COMPUTED) {
        slot->slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT;
    }
    sxml_allocator_release(context->allocator, state);
}

/* The type error of a context value that does not match a library
   module's context value declaration (XQuery 4.0, 5.17). */
static const sxml_static_error library_context_value_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The context value does not match the type a library module declares for it "
        "(XQuery 4.0, 5.17)"),
};

/* Checks the value of the context global, the first global of name 0,
   against the declared types of the table's later globals of name 0:
   those of library modules, matched without coercion (XQuery 4.0, 5.17:
   "the context value must match the type declared in each one"). */
static sxml_status check_library_context_types(sxml_dynamic_context *context,
                                               const sxml_evaluator_global_table *table,
                                               uint32_t context_global_number) {
    const sxml_sequence value = table->slots[context_global_number - 1].value;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t global_number = context_global_number + 1;
         status == SXML_STATUS_OK && global_number <= table->slot_count; global_number += 1) {
        const sxml_ir_global_variable *variable = global_of(table, global_number);
        if (variable->variable_name.index != 0) {
            continue;
        }
        sxml_type_match match = {.is_match = false};
        status = sxml_evaluator_match_sequence_type(
            context,
            &(sxml_type_match_request){.value = value,
                                       .sequence_type = variable->declared_type,
                                       .error_record = context->error_record},
            &match);
        if (status == SXML_STATUS_OK && !match.is_match) {
            status = sxml_evaluator_machine_raise(context, &library_context_value_error);
        }
    }
    return status;
}

/* The number of the context global (the first global of name 0); 0 when
   the table has none. */
static uint32_t context_global_number_of(const sxml_evaluator_global_table *table) {
    for (uint32_t global_number = 1; global_number <= table->slot_count; global_number += 1) {
        if (global_of(table, global_number)->variable_name.index == 0) {
            return global_number;
        }
    }
    return 0;
}

/* Binds the context global (name 0) to the host's context value: the
   main function's focus slot, read in the body task's frame. */
sxml_status sxml_evaluator_bind_context_global(sxml_dynamic_context *context, size_t table_index) {
    const sxml_evaluator_task *body_task = sxml_evaluator_machine_top(context);
    const sxml_ir_slot focus_slot = body_task->function->focus.context_value;
    const sxml_evaluator_global_table *table = &context->global_tables[table_index];
    const uint32_t global_number = context_global_number_of(table);
    if (global_number == 0 || focus_slot.value == 0 ||
        focus_slot.value > body_task->function->slot_count) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_global_variable *variable = global_of(table, global_number);
    sxml_evaluator_slot *global_slot = &table->slots[global_number - 1];
    if (global_slot->slot_state == SXML_EVALUATOR_SLOT_STATE_BOUND) {
        return SXML_STATUS_OK;
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
    *global_slot =
        (sxml_evaluator_slot){.value = coerced, .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
    return check_library_context_types(context, table, global_number);
}

/* Pushes a CALL of the global's initializer (see the file comment), then
   binds the value it returns. */
static sxml_status step_computation(sxml_dynamic_context *context, sxml_evaluator_task *task) {
    global_computation *computation = task->details.part.state;
    sxml_evaluator_slot *slot = computed_slot(context, computation);
    if (slot == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_evaluator_global_table *table = &context->global_tables[computation->table_index];
    if (task->phase == 1) {
        *slot = (sxml_evaluator_slot){.value = sxml_evaluator_machine_take_value(context),
                                      .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
        computation->is_bound = true;
        restore_context(context, computation);
        if (global_of(table, computation->global_number)->variable_name.index == 0) {
            /* The context global's initializer (XQuery 4.0, 5.17). */
            const sxml_status status =
                check_library_context_types(context, table, computation->global_number);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        return sxml_evaluator_machine_finish(context, sxml_sequence_empty());
    }
    const sxml_ir_function *initializer = sxml_ir_unit_get_function(
        table->unit, global_of(table, computation->global_number)->initializer);
    slot->slot_state = SXML_EVALUATOR_SLOT_STATE_BEING_COMPUTED;
    computation->has_saved_context = true;
    computation->saved_mode = context->current_mode;
    computation->saved_template_rule_index = context->current_template_rule_index;
    context->current_mode = (sxml_ir_mode_index){.value = 0};
    context->current_template_rule_index = 0;
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

sxml_status sxml_evaluator_compute_global(sxml_dynamic_context *dynamic_context,
                                          sxml_evaluator_global_request request) {
    const sxml_evaluator_global_table *table =
        &dynamic_context->global_tables[request.table_index];
    if (request.global_number == 0 || request.global_number > table->slot_count ||
        global_of(table, request.global_number)->initializer.value == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        dynamic_context->allocator,
        (sxml_allocation_request){.byte_count = sizeof(global_computation),
                                  .alignment = alignof(global_computation)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *(global_computation *)memory = (global_computation){.table_index = request.table_index,
                                                         .unit = table->unit,
                                                         .global_number = request.global_number,
                                                         .is_bound = false,
                                                         .has_saved_context = false,
                                                         .saved_mode = {.value = 0},
                                                         .saved_template_rule_index = 0};
    const sxml_status pushed = sxml_evaluator_machine_push_part_task(
        dynamic_context, &(sxml_evaluator_part_task_state){.step = step_computation,
                                                           .release_state = release_computation,
                                                           .state = memory});
    if (pushed == SXML_STATUS_OK) {
        /* An initializer's error is not caught around the read. */
        sxml_evaluator_machine_top(dynamic_context)->stops_catching = true;
    }
    return pushed;
}

/* No kinds; the registry lists the part all the same (D101). */
const sxml_evaluator_part sxml_evaluator_part_global = {
    .kind_handlers = NULL,
    .kind_handler_count = 0,
};
