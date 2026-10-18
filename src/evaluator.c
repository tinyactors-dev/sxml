/*
 * The evaluator (src/evaluator_internal.h; D38, D46, D55, D71, D75): the
 * dynamic context, the explicit-stack machine that walks the IR, and the
 * expressions it evaluates.
 *
 * Golden file: src/memory.c (lifecycle, failure paths), with the explicit
 * stacks of src/xpath_lower.c's successors (D71).
 *
 * Specification: XPath 4.0, section 2.2.2 (dynamic context, focus,
 * XPDY0002), section 3 (types, coercion), section 4 (expressions: 4.4
 * filter expressions, 4.5 function calls and inline functions, 4.6 paths,
 * 4.7 maps and arrays, 4.8 sequence expressions, 4.9 arithmetic, 4.10
 * string concatenation, 4.11 comparisons, 4.14 conditionals, 4.15
 * quantified expressions, 4.16 instance of and treat, 4.17 cast, 4.18
 * simple map). The machine is described in src/evaluator_internal.h.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "evaluator_machine_internal.h"
#include "function_library_core_internal.h"
#include "operator_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Errors */

static const sxml_static_error sxml_absent_variable_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The focus or a variable of the dynamic context is absent (XPath 4.0, "
        "section 2.2.2)"),
};

static const sxml_static_error sxml_circular_variable_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0054"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A global variable depends on itself (XQuery 4.0, section 5.16)"),
};

static const sxml_static_error sxml_path_operand_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0019"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An operand on the left of \"/\" is not a node (XPath 4.0, section "
        "4.6.3)"),
};

static const sxml_static_error sxml_treat_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0050"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The value of a treat expression does not match its type (XPath 4.0, "
        "section 4.16.6)"),
};

static const sxml_static_error sxml_single_function_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A dynamic function call needs exactly one function item (XPath 4.0, "
        "section 4.5.3)"),
};

static const sxml_static_error sxml_wrong_arity_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A function item is called with a number of arguments other than its "
        "arity (XPath 4.0, section 4.5.3)"),
};

static const sxml_static_error sxml_lookup_operand_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A lookup is applied to an item that is not a map or an array (XPath "
        "4.0, section 4.7.3)"),
};

static const sxml_static_error sxml_array_key_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An array lookup key is not an xs:integer (XPath 4.0, section "
        "4.7.3)"),
};

static const sxml_static_error sxml_array_bounds_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAY0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Array index out of bounds (F&O 4.0, array:get)"),
};

static const sxml_static_error sxml_map_key_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A map constructor key is not exactly one atomic item (XPath 4.0, "
        "section 4.7.1)"),
};

static const sxml_static_error sxml_map_merge_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A map constructor entry without a value is not a map (XPath 4.0, "
        "section 4.7.1)"),
};

static const sxml_static_error sxml_range_operand_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An operand of \"to\" is not an xs:integer (XPath 4.0, section "
        "4.8.2)"),
};

static const sxml_static_error sxml_range_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A range bound is beyond the integers sxml can count (XPath 4.0, "
        "section 4.8.2)"),
};

static const sxml_static_error sxml_concatenation_operand_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An operand of \"||\" has more than one item (XPath 4.0, section "
        "4.10)"),
};

static const sxml_static_error sxml_sort_key_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A sort key has more than one item (XQuery 4.0, \"order by\")"),
};

/* ---------------------------------------------------------------- Tasks */

/* The task types are in src/evaluator_machine_internal.h (D101). */

/* ------------------------------------------------------------- Growth */

sxml_status sxml_evaluator_grow(sxml_allocator allocator,
                                const sxml_evaluator_growth *growth) {
    const size_t capacity = *growth->element_capacity;
    if (growth->required_count <= capacity) {
        return SXML_STATUS_OK;
    }
    size_t new_capacity = capacity < 8 ? 8 : capacity;
    while (new_capacity < growth->required_count) {
        if (new_capacity > SIZE_MAX / 2) {
            return SXML_STATUS_ARITHMETIC_OVERFLOW;
        }
        new_capacity *= 2;
    }
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(new_capacity, growth->element_byte_count, &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = growth->alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (*growth->elements != NULL) {
        if (growth->element_count > 0) {
            memcpy(memory, *growth->elements,
                   growth->element_count * growth->element_byte_count);
        }
        sxml_allocator_release(allocator, *growth->elements);
    }
    *growth->elements = memory;
    *growth->element_capacity = new_capacity;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- Dynamic context */

sxml_status sxml_dynamic_context_memory_requirements(
    const sxml_evaluator_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->item_heap == NULL ||
        sxml_allocator_is_null(configuration->allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(sxml_dynamic_context),
        .alignment = alignof(sxml_dynamic_context)};
    return SXML_STATUS_OK;
}

sxml_status sxml_dynamic_context_initialize(
    sxml_memory_view memory, const sxml_evaluator_configuration *configuration,
    sxml_dynamic_context **dynamic_context) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_dynamic_context_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_dynamic_context *context = placement;
    *context = (sxml_dynamic_context){
        .item_heap = configuration->item_heap,
        .allocator = configuration->allocator,
        .creating_allocator = {.state = NULL, .operations = NULL},
        .document_loader = configuration->document_loader,
        .clock = configuration->clock,
        .function_table = configuration->function_table,
        .has_clock_reading = false,
        .evaluation_depth = 0,
        .maximum_evaluation_depth =
            configuration->maximum_evaluation_depth == 0
                ? SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_DEPTH
                : configuration->maximum_evaluation_depth,
        .maximum_frame_slot_count = configuration->maximum_frame_slot_count,
        .evaluation_step_count = 0,
        .maximum_evaluation_step_count = configuration->maximum_evaluation_step_count,
        .frame_stack = {.slots = NULL, .slot_count_in_use = 0, .slot_capacity = 0},
        .task_stack = {.tasks = NULL, .task_count = 0, .task_capacity = 0},
        .is_running = false,
        .has_task_value = false,
        .task_value = sxml_sequence_empty(),
        .caught_status = SXML_STATUS_OK,
        .unit = NULL,
        .global_slots = NULL,
        .global_slot_count = 0,
        .error_record = NULL,
        .static_base_uri = {.bytes = NULL, .byte_count = 0},
        .current_mode = {.value = 0},
        .current_template_rule_index = 0,
        .memo_entries = NULL,
        .memo_entry_count = 0,
        .memo_entry_capacity = 0,
    };
    *dynamic_context = context;
    return SXML_STATUS_OK;
}

/* Releases every entry of the evaluation memo and its array. */
static void sxml_release_memo(sxml_dynamic_context *context) {
    for (size_t entry_index = 0; entry_index < context->memo_entry_count; entry_index += 1) {
        sxml_item_release(context->memo_entries[entry_index].key.subject);
        sxml_sequence_release(context->memo_entries[entry_index].value);
    }
    if (context->memo_entries != NULL) {
        sxml_allocator_release(context->allocator, context->memo_entries);
    }
    context->memo_entries = NULL;
    context->memo_entry_count = 0;
    context->memo_entry_capacity = 0;
}

static bool sxml_memo_keys_are_equal(const sxml_evaluation_memo_key *first,
                                     const sxml_evaluation_memo_key *second) {
    return first->computation == second->computation &&
           first->computation_number == second->computation_number &&
           sxml_generic_node_is_same_node(
               (sxml_item_pair){.first = first->subject, .second = second->subject});
}

bool sxml_dynamic_context_find_memo(const sxml_dynamic_context *dynamic_context,
                                    const sxml_evaluation_memo_key *key, sxml_sequence *value) {
    for (size_t entry_index = 0; entry_index < dynamic_context->memo_entry_count;
         entry_index += 1) {
        const sxml_evaluation_memo_entry *entry = &dynamic_context->memo_entries[entry_index];
        if (sxml_memo_keys_are_equal(&entry->key, key)) {
            *value = entry->value;
            return true;
        }
    }
    return false;
}

sxml_status sxml_dynamic_context_store_memo(sxml_dynamic_context *dynamic_context,
                                            const sxml_evaluation_memo_entry *entry) {
    for (size_t entry_index = 0; entry_index < dynamic_context->memo_entry_count;
         entry_index += 1) {
        sxml_evaluation_memo_entry *stored = &dynamic_context->memo_entries[entry_index];
        if (sxml_memo_keys_are_equal(&stored->key, &entry->key)) {
            sxml_sequence_retain(entry->value);
            sxml_sequence_release(stored->value);
            stored->value = entry->value;
            return SXML_STATUS_OK;
        }
    }
    if (dynamic_context->memo_entry_count == dynamic_context->memo_entry_capacity) {
        const size_t capacity =
            dynamic_context->memo_entry_capacity == 0 ? 8 : dynamic_context->memo_entry_capacity * 2;
        size_t byte_count = 0;
        sxml_status status =
            sxml_size_multiply(capacity, sizeof(sxml_evaluation_memo_entry), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                dynamic_context->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_evaluation_memo_entry)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (dynamic_context->memo_entry_count > 0) {
            memcpy(memory, dynamic_context->memo_entries,
                   dynamic_context->memo_entry_count * sizeof(sxml_evaluation_memo_entry));
            sxml_allocator_release(dynamic_context->allocator, dynamic_context->memo_entries);
        }
        dynamic_context->memo_entries = memory;
        dynamic_context->memo_entry_capacity = capacity;
    }
    sxml_item_retain(entry->key.subject);
    sxml_sequence_retain(entry->value);
    dynamic_context->memo_entries[dynamic_context->memo_entry_count] = *entry;
    dynamic_context->memo_entry_count += 1;
    return SXML_STATUS_OK;
}

void sxml_dynamic_context_finalize(sxml_dynamic_context *dynamic_context) {
    if (dynamic_context == NULL) {
        return;
    }
    sxml_release_memo(dynamic_context);
    if (dynamic_context->frame_stack.slots != NULL) {
        sxml_allocator_release(dynamic_context->allocator,
                               dynamic_context->frame_stack.slots);
    }
    if (dynamic_context->task_stack.tasks != NULL) {
        sxml_allocator_release(dynamic_context->allocator,
                               dynamic_context->task_stack.tasks);
    }
    dynamic_context->frame_stack =
        (sxml_evaluator_frame_stack){.slots = NULL, .slot_count_in_use = 0, .slot_capacity = 0};
    dynamic_context->task_stack =
        (sxml_evaluator_task_stack){.tasks = NULL, .task_count = 0, .task_capacity = 0};
}

sxml_status sxml_dynamic_context_create(
    sxml_allocator allocator, const sxml_evaluator_configuration *configuration,
    sxml_dynamic_context **dynamic_context) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_dynamic_context_memory_requirements(configuration, &requirements);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_dynamic_context *context = NULL;
    status = sxml_dynamic_context_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
        configuration, &context);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    context->creating_allocator = allocator;
    *dynamic_context = context;
    return SXML_STATUS_OK;
}

void sxml_dynamic_context_destroy(sxml_dynamic_context *dynamic_context) {
    if (dynamic_context == NULL) {
        return;
    }
    const sxml_allocator allocator = dynamic_context->creating_allocator;
    sxml_dynamic_context_finalize(dynamic_context);
    sxml_allocator_release(allocator, dynamic_context);
}

sxml_status sxml_dynamic_context_read_clock(sxml_dynamic_context *dynamic_context,
                                            sxml_clock_reading *reading) {
    if (!dynamic_context->has_clock_reading) {
        if (dynamic_context->clock.operations == NULL ||
            dynamic_context->clock.operations->read == NULL) {
            /* The null clock (D51). */
            return SXML_STATUS_UNSUPPORTED_FEATURE;
        }
        /* sxml_clock_read checks the reading against its contract
           (include/sxml/processor.h). */
        sxml_clock_reading clock_reading;
        sxml_status status = sxml_clock_read(dynamic_context->clock, &clock_reading);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        dynamic_context->clock_reading = clock_reading;
        dynamic_context->has_clock_reading = true;
    }
    *reading = dynamic_context->clock_reading;
    return SXML_STATUS_OK;
}

sxml_status sxml_dynamic_context_charge_steps(sxml_dynamic_context *dynamic_context,
                                              uint64_t step_count) {
    const uint64_t maximum = dynamic_context->maximum_evaluation_step_count;
    if (maximum == 0) {
        return SXML_STATUS_OK;
    }
    if (step_count > maximum - dynamic_context->evaluation_step_count) {
        dynamic_context->evaluation_step_count = maximum;
        if (dynamic_context->error_record != NULL) {
            dynamic_context->error_record->diagnostic.description = SXML_BYTE_VIEW_FROM_LITERAL(
                "the evaluation took more steps than maximum_evaluation_step_count allows");
        }
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    dynamic_context->evaluation_step_count += step_count;
    return SXML_STATUS_OK;
}

sxml_byte_view sxml_dynamic_context_get_static_base_uri(
    const sxml_dynamic_context *dynamic_context) {
    /* The static context of a function is that of its definition (XPath
       4.0, section 2.2.1): the unit of the innermost task that runs IR
       says, so a function item called after its own evaluation ended, or
       passed to another expression, keeps its own (D91). A call from
       outside an evaluation has no function; the tasks below it may. */
    const sxml_evaluator_task_stack *stack = &dynamic_context->task_stack;
    for (size_t task_count = stack->task_count; task_count > 0; task_count -= 1) {
        const sxml_ir_function *function = stack->tasks[task_count - 1].function;
        if (function != NULL && function->unit != NULL) {
            if (function->unit->has_static_base_uri) {
                return function->unit->static_base_uri;
            }
            break;
        }
    }
    return dynamic_context->static_base_uri;
}

const sxml_ir_unit *sxml_dynamic_context_get_running_unit(
    const sxml_dynamic_context *dynamic_context) {
    /* As sxml_dynamic_context_get_static_base_uri: the definition's static
       context (XPath 4.0, section 2.2.1). */
    const sxml_evaluator_task_stack *stack = &dynamic_context->task_stack;
    for (size_t task_count = stack->task_count; task_count > 0; task_count -= 1) {
        const sxml_ir_function *function = stack->tasks[task_count - 1].function;
        if (function != NULL && function->unit != NULL) {
            return function->unit;
        }
    }
    return dynamic_context->unit;
}

sxml_status sxml_dynamic_context_read_implicit_timezone(
    sxml_dynamic_context *dynamic_context, int16_t *offset_minutes) {
    if (!dynamic_context->has_clock_reading &&
        (dynamic_context->clock.operations == NULL ||
         dynamic_context->clock.operations->read == NULL)) {
        *offset_minutes = 0;
        return SXML_STATUS_OK;
    }
    sxml_clock_reading reading;
    sxml_status status = sxml_dynamic_context_read_clock(dynamic_context, &reading);
    if (status == SXML_STATUS_OK) {
        *offset_minutes = reading.implicit_timezone_offset_minutes;
    }
    return status;
}

static const sxml_static_error sxml_document_unavailable_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "No document is available for the URI (F&O 4.0, fn:doc)"),
};

static const sxml_static_error sxml_document_unparsable_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "No document can be parsed here (F&O 4.0, fn:parse-xml)"),
};

sxml_status sxml_dynamic_context_load_document(
    sxml_dynamic_context *dynamic_context, const sxml_document_request *request,
    sxml_item *document_node) {
    const sxml_document_loader loader = dynamic_context->document_loader;
    if (loader.operations == NULL || loader.operations->load_document == NULL) {
        return sxml_error_record_report(request->error_record,
                                        &sxml_document_unavailable_error);
    }
    return loader.operations->load_document(loader.state, request, document_node);
}

sxml_status sxml_dynamic_context_parse_document(
    sxml_dynamic_context *dynamic_context,
    const sxml_document_parse_request *request, sxml_item *document_node) {
    const sxml_document_loader loader = dynamic_context->document_loader;
    if (loader.operations == NULL || loader.operations->parse_document == NULL) {
        return sxml_error_record_report(request->error_record,
                                        &sxml_document_unparsable_error);
    }
    return loader.operations->parse_document(loader.state, request, document_node);
}

/* ------------------------------------------------------------ Machine */

static sxml_evaluator_task *sxml_machine_top(sxml_dynamic_context *context) {
    return &context->task_stack.tasks[context->task_stack.task_count - 1];
}

/* Pushes task. Pointers to tasks are invalid afterwards. */
static sxml_status sxml_machine_push(sxml_dynamic_context *context,
                                     const sxml_evaluator_task *task) {
    sxml_evaluator_task_stack *stack = &context->task_stack;
    if (stack->task_count == stack->task_capacity) {
        void *elements = stack->tasks;
        sxml_status status = sxml_evaluator_grow(
            context->allocator,
            &(sxml_evaluator_growth){.elements = &elements,
                                     .element_capacity = &stack->task_capacity,
                                     .element_count = stack->task_count,
                                     .required_count = stack->task_count + 1,
                                     .element_byte_count = sizeof(sxml_evaluator_task),
                                     .alignment = alignof(sxml_evaluator_task)});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        stack->tasks = elements;
    }
    stack->tasks[stack->task_count] = *task;
    stack->task_count += 1;
    return SXML_STATUS_OK;
}

/* A task evaluating expression in the frame of function at frame_base. */
static sxml_evaluator_task sxml_expression_task(const sxml_ir_function *function,
                                                size_t frame_base,
                                                sxml_ir_expression_index expression) {
    sxml_evaluator_task task;
    memset(&task, 0, sizeof(task));
    task.task_kind = SXML_EVALUATOR_TASK_KIND_EXPRESSION;
    task.function = function;
    task.frame_base = frame_base;
    task.expression = expression;
    return task;
}

/* Releases the arguments of a CALL task. */
static void sxml_release_arguments(sxml_dynamic_context *context,
                                   sxml_call_state *call) {
    for (size_t argument_index = 0; argument_index < call->argument_count;
         argument_index += 1) {
        sxml_sequence_release(call->arguments[argument_index]);
    }
    if (call->arguments != NULL) {
        sxml_allocator_release(context->allocator, call->arguments);
    }
    call->arguments = NULL;
    call->argument_count = 0;
}

/* Ends the frame at frame_base: releases its slots. */
static void sxml_machine_pop_frame(sxml_dynamic_context *context, size_t frame_base) {
    sxml_evaluator_frame_stack *frames = &context->frame_stack;
    while (frames->slot_count_in_use > frame_base) {
        frames->slot_count_in_use -= 1;
        sxml_evaluator_slot *slot = &frames->slots[frames->slot_count_in_use];
        sxml_sequence_release(slot->value);
        slot->value = sxml_sequence_empty();
        slot->slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT;
    }
}

static void sxml_release_native_call(sxml_dynamic_context *context,
                                     sxml_native_call_record *record) {
    if (record == NULL) {
        return;
    }
    const sxml_resumable_native_body *body = record->row->resumable_body;
    if (body->release_state != NULL && record->state != NULL) {
        body->release_state(record->state);
    }
    for (size_t argument_index = 0; argument_index < record->argument_count;
         argument_index += 1) {
        sxml_sequence_release(record->arguments[argument_index]);
    }
    sxml_allocator_release(context->allocator, record);
}

static void sxml_release_sort_state(sxml_dynamic_context *context,
                                    sxml_sort_state *sort_state) {
    if (sort_state == NULL) {
        return;
    }
    for (size_t value_index = 0; value_index < sort_state->next_value_index;
         value_index += 1) {
        sxml_sequence_release(sort_state->key_values[value_index]);
    }
    sxml_allocator_release(context->allocator, sort_state);
}

/* Releases what task holds (not the task's place on the stack). */
static void sxml_machine_release_task(sxml_dynamic_context *context,
                                      sxml_evaluator_task *task) {
    if (task->counts_depth) {
        context->evaluation_depth -= 1;
        task->counts_depth = false;
    }
    switch (task->task_kind) {
    case SXML_EVALUATOR_TASK_KIND_EXPRESSION: {
        sxml_loop_state *loop = &task->details.loop;
        sxml_sequence_release(loop->input);
        loop->input = sxml_sequence_empty();
        sxml_sequence_builder_destroy(loop->builder);
        loop->builder = NULL;
        sxml_release_sort_state(context, loop->sort_state);
        loop->sort_state = NULL;
        return;
    }
    case SXML_EVALUATOR_TASK_KIND_CALL:
        sxml_item_release(task->details.call.function_item);
        task->details.call.function_item = (sxml_item){.representation = 0};
        sxml_release_arguments(context, &task->details.call);
        return;
    case SXML_EVALUATOR_TASK_KIND_RETURN:
        sxml_machine_pop_frame(context, task->frame_base);
        return;
    case SXML_EVALUATOR_TASK_KIND_NATIVE_STEP:
        sxml_item_release(task->details.call.function_item);
        task->details.call.function_item = (sxml_item){.representation = 0};
        sxml_release_native_call(context, task->details.call.native_call);
        task->details.call.native_call = NULL;
        return;
    case SXML_EVALUATOR_TASK_KIND_COERCE_RESULT:
        return;
    case SXML_EVALUATOR_TASK_KIND_PART: {
        sxml_evaluator_part_task_state *part = &task->details.part;
        if (part->release_state != NULL && part->state != NULL) {
            part->release_state(context, part->state);
        }
        part->state = NULL;
        return;
    }
    }
}

/* Pops the top task and hands value (owned) to the task below. */
static sxml_status sxml_machine_finish(sxml_dynamic_context *context,
                                       sxml_sequence value) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    sxml_machine_release_task(context, task);
    context->task_stack.task_count -= 1;
    sxml_sequence_release(context->task_value);
    context->task_value = value;
    context->has_task_value = true;
    return SXML_STATUS_OK;
}

/* Takes the value the task above finished with, owned. */
static sxml_sequence sxml_machine_take_value(sxml_dynamic_context *context) {
    const sxml_sequence value = context->task_value;
    context->task_value = sxml_sequence_empty();
    context->has_task_value = false;
    return value;
}

/* Pops every task down to base_count, releasing what they hold. */
static void sxml_machine_unwind(sxml_dynamic_context *context, size_t base_count) {
    while (context->task_stack.task_count > base_count) {
        sxml_machine_release_task(context, sxml_machine_top(context));
        context->task_stack.task_count -= 1;
    }
    sxml_sequence_release(sxml_machine_take_value(context));
}

/* ------------------------------------------------------------ Frames */

/* Starts a frame of slot_count absent slots at the top of the frame stack
   and writes its base to *frame_base. */
static sxml_status sxml_machine_push_frame(sxml_dynamic_context *context,
                                           uint32_t slot_count,
                                           size_t *frame_base) {
    sxml_evaluator_frame_stack *frames = &context->frame_stack;
    const size_t base = frames->slot_count_in_use;
    const size_t required = base + slot_count;
    if (context->maximum_frame_slot_count != 0 &&
        required > context->maximum_frame_slot_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *elements = frames->slots;
    sxml_status status = sxml_evaluator_grow(
        context->allocator,
        &(sxml_evaluator_growth){.elements = &elements,
                                 .element_capacity = &frames->slot_capacity,
                                 .element_count = frames->slot_count_in_use,
                                 .required_count = required,
                                 .element_byte_count = sizeof(sxml_evaluator_slot),
                                 .alignment = alignof(sxml_evaluator_slot)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frames->slots = elements;
    for (size_t slot_index = base; slot_index < required; slot_index += 1) {
        frames->slots[slot_index] = (sxml_evaluator_slot){
            .value = sxml_sequence_empty(),
            .slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT};
    }
    frames->slot_count_in_use = required;
    *frame_base = base;
    return SXML_STATUS_OK;
}

/* Binds slot (from 1) of the frame at frame_base to value (owned),
   releasing what it held; slot 0 (unused) releases value. */
typedef struct sxml_slot_binding {
    size_t frame_base;
    sxml_ir_slot slot;
} sxml_slot_binding;

static void sxml_machine_bind(sxml_dynamic_context *context,
                              sxml_slot_binding binding, sxml_sequence value) {
    if (binding.slot.value == 0) {
        sxml_sequence_release(value);
        return;
    }
    sxml_evaluator_slot *slot =
        &context->frame_stack.slots[binding.frame_base + binding.slot.value - 1];
    sxml_sequence_release(slot->value);
    slot->value = value;
    slot->slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND;
}

/* Reads atom in the frame of task: borrowed (from the frame, the unit or
   the global table), valid until that place changes. */
static sxml_status sxml_machine_read_atom(sxml_dynamic_context *context,
    sxml_ir_atom atom, sxml_sequence *value) {
    const sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_evaluator_slot *slot = NULL;
    switch (atom.atom_kind) {
    case SXML_IR_ATOM_KIND_NONE:
        *value = sxml_sequence_empty();
        return SXML_STATUS_OK;
    case SXML_IR_ATOM_KIND_CONSTANT:
        *value = sxml_ir_unit_get_constant(task->function->unit, atom.constant);
        return SXML_STATUS_OK;
    case SXML_IR_ATOM_KIND_SLOT:
        if (atom.slot.value == 0 || atom.slot.value > task->function->slot_count) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        slot = &context->frame_stack.slots[task->frame_base + atom.slot.value - 1];
        break;
    case SXML_IR_ATOM_KIND_GLOBAL_VARIABLE:
        /* Only the evaluation's own unit reads globals: closures capture
           them (D55). */
        if (task->function->unit != context->unit ||
            atom.global_variable.value == 0 ||
            atom.global_variable.value > context->global_slot_count) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        slot = &context->global_slots[atom.global_variable.value - 1];
        break;
    }
    switch (slot->slot_state) {
    case SXML_EVALUATOR_SLOT_STATE_ABSENT:
        return sxml_error_record_report(context->error_record,
                                        &sxml_absent_variable_error);
    case SXML_EVALUATOR_SLOT_STATE_BEING_COMPUTED:
        return sxml_error_record_report(context->error_record,
                                        &sxml_circular_variable_error);
    case SXML_EVALUATOR_SLOT_STATE_BOUND:
        break;
    }
    *value = slot->value;
    return SXML_STATUS_OK;
}

/* Reads atom and retains it. */
static sxml_status sxml_machine_copy_atom(sxml_dynamic_context *context,
    sxml_ir_atom atom, sxml_sequence *value) {
    sxml_status status = sxml_machine_read_atom(context, atom, value);
    if (status == SXML_STATUS_OK) {
        sxml_sequence_retain(*value);
    }
    return status;
}

/* Reads a list of atoms into an array of owned sequences in allocator
   memory (NULL for none). */
static sxml_status sxml_machine_copy_atoms(sxml_dynamic_context *context,
    sxml_ir_atom_list list,
                                           sxml_sequence **values) {
    const sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_ir_atom_view atoms = sxml_ir_unit_get_atoms(task->function->unit, list);
    *values = NULL;
    if (atoms.atom_count == 0) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        context->allocator,
        (sxml_allocation_request){.byte_count = atoms.atom_count * sizeof(sxml_sequence),
                                  .alignment = alignof(sxml_sequence)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence *array = memory;
    size_t copied_count = 0;
    while (status == SXML_STATUS_OK && copied_count < atoms.atom_count) {
        status = sxml_machine_copy_atom(context, atoms.atoms[copied_count],
                                        &array[copied_count]);
        if (status == SXML_STATUS_OK) {
            copied_count += 1;
        }
    }
    if (status != SXML_STATUS_OK) {
        for (size_t value_index = 0; value_index < copied_count; value_index += 1) {
            sxml_sequence_release(array[value_index]);
        }
        sxml_allocator_release(context->allocator, memory);
        return status;
    }
    *values = array;
    return SXML_STATUS_OK;
}

/* -------------------------------------------------------------- Calls */

/* The arguments and closure of an IR function call. */
typedef struct sxml_ir_call {
    const sxml_ir_function *callee;
    /* Owned, moved into the parameter slots; the array is released. */
    sxml_sequence *arguments;
    size_t argument_count;
    /* Borrowed from the function item; retained into the slots. */
    sxml_sequence_view captured_values;
} sxml_ir_call;

/* Fills the frame at frame_base: parameters, then captured values (D38). */
static void sxml_machine_fill_frame(sxml_dynamic_context *context,
                                    size_t frame_base, sxml_ir_call *call) {
    uint32_t slot_number = 1;
    for (size_t argument_index = 0; argument_index < call->argument_count;
         argument_index += 1) {
        sxml_machine_bind(context,
                          (sxml_slot_binding){.frame_base = frame_base,
                                              .slot = {.value = slot_number}},
                          call->arguments[argument_index]);
        slot_number += 1;
    }
    for (size_t value_index = 0;
         value_index < call->captured_values.sequence_count; value_index += 1) {
        const sxml_sequence value = call->captured_values.sequences[value_index];
        sxml_sequence_retain(value);
        sxml_machine_bind(context,
                          (sxml_slot_binding){.frame_base = frame_base,
                                              .slot = {.value = slot_number}},
                          value);
        slot_number += 1;
    }
    if (call->arguments != NULL) {
        sxml_allocator_release(context->allocator, call->arguments);
    }
    call->arguments = NULL;
    call->argument_count = 0;
}

/*
 * Replaces the CALL task on top with the call of an IR function: a tail
 * call when the task below is the RETURN of the CALL's own frame (the
 * call's value is that function's value), which then reuses that frame;
 * otherwise the CALL becomes the RETURN of a new frame. The callee's body
 * is pushed. The CALL's function item is released here.
 */
static sxml_status sxml_machine_enter_ir_function(sxml_dynamic_context *context,
                                                  sxml_ir_call *call) {
    const sxml_ir_function *callee = call->callee;
    sxml_evaluator_task *task = sxml_machine_top(context);
    const size_t parameter_count = callee->parameter_count;
    if (call->argument_count != parameter_count ||
        callee->slot_count < parameter_count + call->captured_values.sequence_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const size_t task_count = context->task_stack.task_count;
    sxml_evaluator_task *below =
        task_count >= 2 ? &context->task_stack.tasks[task_count - 2] : NULL;
    const bool is_tail_call = below != NULL && task->function != NULL &&
                              below->task_kind == SXML_EVALUATOR_TASK_KIND_RETURN &&
                              below->function == task->function &&
                              below->frame_base == task->frame_base;
    /* The function item keeps the captured values alive until they are
       retained into the frame. */
    const sxml_item function_item = task->details.call.function_item;
    task->details.call.function_item = (sxml_item){.representation = 0};
    task->details.call.arguments = NULL;
    task->details.call.argument_count = 0;
    sxml_status status = SXML_STATUS_OK;
    size_t frame_base = 0;
    if (is_tail_call) {
        context->task_stack.task_count -= 1;
        sxml_evaluator_task *caller_return = sxml_machine_top(context);
        sxml_machine_pop_frame(context, caller_return->frame_base);
        status = sxml_machine_push_frame(context, callee->slot_count, &frame_base);
        if (status == SXML_STATUS_OK) {
            caller_return->function = callee;
        }
    } else if (context->evaluation_depth >= context->maximum_evaluation_depth) {
        status = SXML_STATUS_LIMIT_EXCEEDED;
    } else {
        status = sxml_machine_push_frame(context, callee->slot_count, &frame_base);
        if (status == SXML_STATUS_OK) {
            task->task_kind = SXML_EVALUATOR_TASK_KIND_RETURN;
            task->function = callee;
            task->frame_base = frame_base;
            task->counts_depth = true;
            context->evaluation_depth += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_machine_fill_frame(context, frame_base, call);
        const sxml_evaluator_task body =
            sxml_expression_task(callee, frame_base, callee->body);
        status = sxml_machine_push(context, &body);
    } else {
        for (size_t argument_index = 0; argument_index < call->argument_count;
             argument_index += 1) {
            sxml_sequence_release(call->arguments[argument_index]);
        }
        if (call->arguments != NULL) {
            sxml_allocator_release(context->allocator, call->arguments);
        }
    }
    sxml_item_release(function_item);
    return status;
}

/* Turns the CALL task on top into a NATIVE_STEP of a resumable row. */
static sxml_status sxml_machine_start_native_steps(sxml_dynamic_context *context,
                                                   const sxml_native_function *row) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    if (context->evaluation_depth >= context->maximum_evaluation_depth) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const sxml_resumable_native_body *body = row->resumable_body;
    const size_t argument_count = task->details.call.argument_count;
    size_t state_alignment = body->state_alignment == 0 ? 1 : body->state_alignment;
    if (!sxml_alignment_is_valid(state_alignment)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (state_alignment < alignof(sxml_native_call_record)) {
        state_alignment = alignof(sxml_native_call_record);
    }
    size_t arguments_end = 0;
    size_t state_offset = 0;
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(argument_count, sizeof(sxml_sequence),
                                            &arguments_end);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(arguments_end, sizeof(sxml_native_call_record),
                               &arguments_end);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_align_up(arguments_end, state_alignment, &state_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(state_offset, body->state_byte_count, &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = state_alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, byte_count);
    sxml_native_call_record *record = memory;
    void *arguments_place = (uint8_t *)memory + sizeof(sxml_native_call_record);
    record->arguments = arguments_place;
    for (size_t argument_index = 0; argument_index < argument_count;
         argument_index += 1) {
        record->arguments[argument_index] =
            task->details.call.arguments[argument_index];
    }
    if (task->details.call.arguments != NULL) {
        sxml_allocator_release(context->allocator, task->details.call.arguments);
    }
    task->details.call.arguments = NULL;
    task->details.call.argument_count = 0;
    record->argument_count = argument_count;
    record->row = row;
    record->state = body->state_byte_count == 0 ? NULL : (uint8_t *)memory + state_offset;
    record->step_number = 0;
    record->call = (sxml_function_call){
        .function_item = task->details.call.function_item,
        .arguments = {.sequences = record->arguments, .sequence_count = argument_count},
        .dynamic_context = context,
        .item_heap = context->item_heap,
        .error_record = context->error_record};
    task->task_kind = SXML_EVALUATOR_TASK_KIND_NATIVE_STEP;
    task->phase = 0;
    task->details.call.native_call = record;
    task->details.call.native_function = row;
    task->counts_depth = true;
    context->evaluation_depth += 1;
    return SXML_STATUS_OK;
}

/* Runs the leaf function body of the CALL on top and finishes with its
   value. */
static sxml_status sxml_machine_call_leaf(sxml_dynamic_context *context,
                                          const sxml_native_function *row) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_function_call call = {
        .function_item = task->details.call.function_item,
        .arguments = {.sequences = task->details.call.arguments,
                      .sequence_count = task->details.call.argument_count},
        .dynamic_context = context,
        .item_heap = context->item_heap,
        .error_record = context->error_record};
    sxml_sequence result = sxml_sequence_empty();
    sxml_status status = row != NULL ? row->call(&call, &result)
                                     : sxml_function_item_call(&call, &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_machine_finish(context, result);
}

/* The sequence type a map or array called as a function coerces its
   argument to (XDM 4.0, sections 8.2 and 8.4). */
static sxml_sequence_type sxml_collection_parameter_type(sxml_item function_item) {
    return (sxml_sequence_type){
        .item_type = sxml_builtin_type_identifier(
            sxml_item_get_kind(function_item) == SXML_ITEM_KIND_ARRAY
                ? SXML_TYPE_XS_INTEGER
                : SXML_TYPE_XS_ANY_ATOMIC_TYPE),
        .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
}

/* Coerces the arguments of the CALL on top to the signature of its
   function item (XPath 4.0, section 4.5.3: the coercion rules apply to
   each argument). */
static sxml_status sxml_machine_coerce_arguments(sxml_dynamic_context *context) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    sxml_call_state *call = &task->details.call;
    sxml_call_signature signature;
    sxml_status status = sxml_evaluator_describe_call_signature(
        context, call->function_item, &signature);
    for (size_t argument_index = 0;
         status == SXML_STATUS_OK && argument_index < call->argument_count;
         argument_index += 1) {
        const sxml_sequence_type type =
            signature.has_function_type
                ? signature.parameter_types.sequence_types[argument_index]
                : sxml_collection_parameter_type(call->function_item);
        sxml_sequence coerced = sxml_sequence_empty();
        status = sxml_evaluator_coerce(
            context,
            &(sxml_coercion_request){.value = call->arguments[argument_index],
                                     .sequence_type = type,
                                     .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
                                     .error_record = context->error_record},
            &coerced);
        if (status == SXML_STATUS_OK) {
            sxml_sequence_release(call->arguments[argument_index]);
            call->arguments[argument_index] = coerced;
        }
    }
    return status;
}

/* Runs the CALL task on top: checks and coerces, then dispatches on the
   body (see "Reentry" in the header). */
static sxml_status sxml_machine_step_call(sxml_dynamic_context *context) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    if (task->details.call.callee != NULL) {
        sxml_ir_call call = {.callee = task->details.call.callee,
                             .arguments = task->details.call.arguments,
                             .argument_count = task->details.call.argument_count,
                             .captured_values = {.sequences = NULL, .sequence_count = 0}};
        return sxml_machine_enter_ir_function(context, &call);
    }
    if (task->details.call.native_function != NULL) {
        /* A static call of a resumable native row. */
        return sxml_machine_start_native_steps(context,
                                               task->details.call.native_function);
    }
    const sxml_item function_item = task->details.call.function_item;
    sxml_function_item_description description;
    sxml_status status = sxml_function_item_describe(function_item, &description);
    if (status != SXML_STATUS_OK) {
        return sxml_error_record_report(context->error_record,
                                        &sxml_single_function_error);
    }
    if (description.arity != task->details.call.argument_count) {
        return sxml_error_record_report(context->error_record,
                                        &sxml_wrong_arity_error);
    }
    status = sxml_machine_coerce_arguments(context);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    task = sxml_machine_top(context);
    const sxml_function_body_operations *operations = description.body.operations;
    if (operations == &sxml_evaluator_function_body_operations) {
        sxml_ir_call call = {.callee = description.body.state,
                             .arguments = task->details.call.arguments,
                             .argument_count = task->details.call.argument_count,
                             .captured_values = description.captured_values};
        return sxml_machine_enter_ir_function(context, &call);
    }
    if (operations == &sxml_native_function_body_operations) {
        const sxml_native_function *row = description.body.state;
        if (row->call != NULL) {
            return sxml_machine_call_leaf(context, row);
        }
        if (row->resumable_body == NULL) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        return sxml_machine_start_native_steps(context, row);
    }
    if (operations == &sxml_evaluator_coerced_function_body_operations) {
        /* XPath 4.0, section 3.4.4: call the wrapped function with the
           arguments, then coerce its result to the required result type. */
        sxml_call_signature signature;
        status = sxml_evaluator_describe_call_signature(context, function_item,
                                                        &signature);
        if (status != SXML_STATUS_OK || description.captured_values.sequence_count != 1) {
            return status != SXML_STATUS_OK ? status : SXML_STATUS_INVALID_ARGUMENT;
        }
        const sxml_item inner = description.captured_values.sequences[0].content;
        sxml_function_item_description inner_description;
        status = sxml_function_item_describe(inner, &inner_description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* A wrapped function of lower arity gets the leading arguments
           (XPath 4.0, section 3.4.4). */
        while (task->details.call.argument_count > inner_description.arity) {
            task->details.call.argument_count -= 1;
            sxml_sequence_release(
                task->details.call.arguments[task->details.call.argument_count]);
        }
        sxml_evaluator_task inner_call = *task;
        inner_call.counts_depth = false;
        sxml_item_retain(inner);
        inner_call.details.call.function_item = inner;
        task->task_kind = SXML_EVALUATOR_TASK_KIND_COERCE_RESULT;
        task->details.coercion.sequence_type = signature.result_type;
        task->details.coercion.role = SXML_IR_COERCION_ROLE_FUNCTION_RESULT;
        status = sxml_machine_push(context, &inner_call);
        sxml_item_release(function_item);
        if (status != SXML_STATUS_OK) {
            sxml_evaluator_task released = inner_call;
            sxml_machine_release_task(context, &released);
        }
        return status;
    }
    /* Maps, arrays, constructor functions and host bodies are leaves. */
    return sxml_machine_call_leaf(context, NULL);
}

/* Runs one step of the NATIVE_STEP task on top. */
static sxml_status sxml_machine_step_native(sxml_dynamic_context *context) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    sxml_native_call_record *record = task->details.call.native_call;
    sxml_sequence call_result = sxml_sequence_empty();
    if (task->phase == 1) {
        call_result = sxml_machine_take_value(context);
    }
    sxml_native_step step = {.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                             .result = sxml_sequence_empty(),
                             .function_item = {.representation = 0},
                             .arguments = {.sequences = NULL, .sequence_count = 0}};
    const sxml_status status = record->row->resumable_body->step(
        &(sxml_native_step_request){.call = &record->call,
                                    .state = record->state,
                                    .step_number = record->step_number,
                                    .call_result = call_result},
        &step);
    record->step_number += 1;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (step.step_kind) {
    case SXML_NATIVE_STEP_KIND_FINISH:
        return sxml_machine_finish(context, step.result);
    case SXML_NATIVE_STEP_KIND_CALL:
        break;
    }
    /* The step asks for a call: copy what it lends, then run the call as
       any dynamic call. */
    sxml_evaluator_task call = sxml_expression_task(task->function, task->frame_base,
                                                    task->expression);
    call.task_kind = SXML_EVALUATOR_TASK_KIND_CALL;
    call.details.call = (sxml_call_state){.function_item = step.function_item,
                                          .callee = NULL,
                                          .native_function = NULL,
                                          .arguments = NULL,
                                          .argument_count = 0,
                                          .native_call = NULL};
    const size_t argument_count = step.arguments.sequence_count;
    if (argument_count > 0) {
        void *memory = NULL;
        sxml_status allocation_status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count = argument_count * sizeof(sxml_sequence),
                                      .alignment = alignof(sxml_sequence)},
            &memory);
        if (allocation_status != SXML_STATUS_OK) {
            return allocation_status;
        }
        call.details.call.arguments = memory;
        for (size_t argument_index = 0; argument_index < argument_count;
             argument_index += 1) {
            call.details.call.arguments[argument_index] =
                step.arguments.sequences[argument_index];
            sxml_sequence_retain(step.arguments.sequences[argument_index]);
        }
        call.details.call.argument_count = argument_count;
    }
    sxml_item_retain(step.function_item);
    task->phase = 1;
    sxml_status push_status = sxml_machine_push(context, &call);
    if (push_status != SXML_STATUS_OK) {
        sxml_machine_release_task(context, &call);
    }
    return push_status;
}

/* Finishes the COERCE_RESULT task on top with the coerced value. */
static sxml_status sxml_machine_step_coerce_result(sxml_dynamic_context *context) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_sequence value = sxml_machine_take_value(context);
    sxml_sequence coerced = sxml_sequence_empty();
    const sxml_status status = sxml_evaluator_coerce(
        context,
        &(sxml_coercion_request){.value = value,
                                 .sequence_type = task->details.coercion.sequence_type,
                                 .role = task->details.coercion.role,
                                 .error_record = context->error_record},
        &coerced);
    sxml_sequence_release(value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_machine_finish(context, coerced);
}

/* ------------------------------------------------------- Leaf helpers */

static sxml_status sxml_machine_atomize(sxml_dynamic_context *context,
                                        sxml_sequence value, sxml_sequence *atomized) {
    return sxml_evaluator_atomize(
        context,
        &(sxml_atomization_request){.value = value,
                                    .error_record = context->error_record},
        atomized);
}

/* True when an item of an atomized value is a date, time or duration, so
   the operator needs the implicit timezone. */
typedef struct sxml_temporal_check {
    bool has_temporal_item;
} sxml_temporal_check;

/* True for an integer range in either order (D143). */
static bool sxml_is_integer_range(sxml_sequence value) {
    const sxml_sequence_representation representation =
        sxml_sequence_get_representation(value);
    return representation == SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE ||
           representation == SXML_SEQUENCE_REPRESENTATION_DESCENDING_INTEGER_RANGE;
}

static sxml_status sxml_value_has_temporal_item(sxml_sequence value,
                                                sxml_temporal_check *check) {
    check->has_temporal_item = false;
    /* A range holds integers only, and may be far too long to walk:
       "1 = 1 to 100000000000" (found by tests/fuzz/xpath_evaluate_fuzz.c). */
    if (sxml_is_integer_range(value)) {
        return SXML_STATUS_OK;
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    for (;;) {
        sxml_iteration_step step;
        sxml_status status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            return status;
        }
        const uint32_t primitive = sxml_atomic_item_get_primitive_type(step.item).index;
        if (primitive >= SXML_TYPE_XS_DURATION && primitive <= SXML_TYPE_XS_G_MONTH) {
            check->has_temporal_item = true;
            return SXML_STATUS_OK;
        }
    }
}

/* The implicit timezone when an operand needs it, else 0 without reading
   the clock. */
static sxml_status sxml_machine_operand_timezone(sxml_dynamic_context *context,
                                                 const sxml_sequence *operands,
                                                 int16_t *offset_minutes) {
    *offset_minutes = 0;
    for (size_t operand_index = 0; operand_index < 2; operand_index += 1) {
        sxml_temporal_check check = {.has_temporal_item = false};
        sxml_status status =
            sxml_value_has_temporal_item(operands[operand_index], &check);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (check.has_temporal_item) {
            return sxml_dynamic_context_read_implicit_timezone(context, offset_minutes);
        }
    }
    return SXML_STATUS_OK;
}

static sxml_comparison_operator sxml_comparison_operator_of(
    sxml_ir_binary_operator binary_operator) {
    switch (binary_operator) {
    case SXML_IR_BINARY_OPERATOR_VALUE_NOT_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_NOT_EQUAL:
        return SXML_COMPARISON_OPERATOR_NOT_EQUAL;
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS:
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS:
        return SXML_COMPARISON_OPERATOR_LESS;
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS_OR_EQUAL:
        return SXML_COMPARISON_OPERATOR_LESS_OR_EQUAL;
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER:
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER:
        return SXML_COMPARISON_OPERATOR_GREATER;
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER_OR_EQUAL:
        return SXML_COMPARISON_OPERATOR_GREATER_OR_EQUAL;
    case SXML_IR_BINARY_OPERATOR_VALUE_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL:
    case SXML_IR_BINARY_OPERATOR_ADD:
    case SXML_IR_BINARY_OPERATOR_SUBTRACT:
    case SXML_IR_BINARY_OPERATOR_MULTIPLY:
    case SXML_IR_BINARY_OPERATOR_DIVIDE:
    case SXML_IR_BINARY_OPERATOR_INTEGER_DIVIDE:
    case SXML_IR_BINARY_OPERATOR_MODULO:
    case SXML_IR_BINARY_OPERATOR_IS:
    case SXML_IR_BINARY_OPERATOR_IS_NOT:
    case SXML_IR_BINARY_OPERATOR_PRECEDES:
    case SXML_IR_BINARY_OPERATOR_FOLLOWS:
    case SXML_IR_BINARY_OPERATOR_PRECEDES_OR_IS:
    case SXML_IR_BINARY_OPERATOR_FOLLOWS_OR_IS:
    case SXML_IR_BINARY_OPERATOR_RANGE:
    case SXML_IR_BINARY_OPERATOR_UNION:
    case SXML_IR_BINARY_OPERATOR_INTERSECT:
    case SXML_IR_BINARY_OPERATOR_EXCEPT:
    case SXML_IR_BINARY_OPERATOR_CONCATENATE:
        break;
    }
    return SXML_COMPARISON_OPERATOR_EQUAL;
}

static sxml_arithmetic_operator sxml_arithmetic_operator_of(
    sxml_ir_binary_operator binary_operator) {
    switch (binary_operator) {
    case SXML_IR_BINARY_OPERATOR_SUBTRACT:
        return SXML_ARITHMETIC_OPERATOR_SUBTRACT;
    case SXML_IR_BINARY_OPERATOR_MULTIPLY:
        return SXML_ARITHMETIC_OPERATOR_MULTIPLY;
    case SXML_IR_BINARY_OPERATOR_DIVIDE:
        return SXML_ARITHMETIC_OPERATOR_DIVIDE;
    case SXML_IR_BINARY_OPERATOR_INTEGER_DIVIDE:
        return SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE;
    case SXML_IR_BINARY_OPERATOR_MODULO:
        return SXML_ARITHMETIC_OPERATOR_MODULO;
    case SXML_IR_BINARY_OPERATOR_ADD:
    case SXML_IR_BINARY_OPERATOR_VALUE_EQUAL:
    case SXML_IR_BINARY_OPERATOR_VALUE_NOT_EQUAL:
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS:
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER:
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_NOT_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS:
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER:
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_IS:
    case SXML_IR_BINARY_OPERATOR_IS_NOT:
    case SXML_IR_BINARY_OPERATOR_PRECEDES:
    case SXML_IR_BINARY_OPERATOR_FOLLOWS:
    case SXML_IR_BINARY_OPERATOR_PRECEDES_OR_IS:
    case SXML_IR_BINARY_OPERATOR_FOLLOWS_OR_IS:
    case SXML_IR_BINARY_OPERATOR_RANGE:
    case SXML_IR_BINARY_OPERATOR_UNION:
    case SXML_IR_BINARY_OPERATOR_INTERSECT:
    case SXML_IR_BINARY_OPERATOR_EXCEPT:
    case SXML_IR_BINARY_OPERATOR_CONCATENATE:
        break;
    }
    return SXML_ARITHMETIC_OPERATOR_ADD;
}

static sxml_node_comparison_operator sxml_node_comparison_operator_of(
    sxml_ir_binary_operator binary_operator) {
    switch (binary_operator) {
    case SXML_IR_BINARY_OPERATOR_IS_NOT:
        return SXML_NODE_COMPARISON_OPERATOR_IS_NOT;
    case SXML_IR_BINARY_OPERATOR_PRECEDES:
        return SXML_NODE_COMPARISON_OPERATOR_PRECEDES;
    case SXML_IR_BINARY_OPERATOR_FOLLOWS:
        return SXML_NODE_COMPARISON_OPERATOR_FOLLOWS;
    case SXML_IR_BINARY_OPERATOR_PRECEDES_OR_IS:
        return SXML_NODE_COMPARISON_OPERATOR_PRECEDES_OR_IS;
    case SXML_IR_BINARY_OPERATOR_FOLLOWS_OR_IS:
        return SXML_NODE_COMPARISON_OPERATOR_FOLLOWS_OR_IS;
    case SXML_IR_BINARY_OPERATOR_IS:
    case SXML_IR_BINARY_OPERATOR_ADD:
    case SXML_IR_BINARY_OPERATOR_SUBTRACT:
    case SXML_IR_BINARY_OPERATOR_MULTIPLY:
    case SXML_IR_BINARY_OPERATOR_DIVIDE:
    case SXML_IR_BINARY_OPERATOR_INTEGER_DIVIDE:
    case SXML_IR_BINARY_OPERATOR_MODULO:
    case SXML_IR_BINARY_OPERATOR_VALUE_EQUAL:
    case SXML_IR_BINARY_OPERATOR_VALUE_NOT_EQUAL:
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS:
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER:
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_NOT_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS:
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER:
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_RANGE:
    case SXML_IR_BINARY_OPERATOR_UNION:
    case SXML_IR_BINARY_OPERATOR_INTERSECT:
    case SXML_IR_BINARY_OPERATOR_EXCEPT:
    case SXML_IR_BINARY_OPERATOR_CONCATENATE:
        break;
    }
    return SXML_NODE_COMPARISON_OPERATOR_IS;
}

/* One operand of "to": empty, or one xs:integer (an xs:untypedAtomic is
   cast to it); written, owned, to *integer_value. */
static sxml_status sxml_range_operand(sxml_dynamic_context *context,
                                      sxml_sequence operand,
                                      sxml_sequence *integer_value) {
    sxml_single_operand single;
    sxml_status status = sxml_operator_read_single_operand(
        operand, context->error_record, &single);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        return sxml_error_record_report(context->error_record,
                                        &sxml_range_operand_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!single.has_item) {
        *integer_value = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(context->item_heap);
    const sxml_type_identifier annotation = sxml_item_get_type_annotation(single.item);
    if (sxml_atomic_item_get_primitive_type(single.item).index ==
        SXML_TYPE_XS_UNTYPED_ATOMIC) {
        return sxml_cast_make_value(
            context->item_heap,
            &(sxml_cast_request){
                .value = sxml_sequence_from_item(single.item),
                .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                .allows_empty = false,
                .namespace_bindings = {.namespace_bindings = NULL,
                                       .namespace_binding_count = 0},
                .error_record = context->error_record},
            integer_value);
    }
    if (!sxml_type_registry_is_derived_from(
            registry,
            (sxml_type_pair){.derived_type = annotation,
                             .base_type = sxml_builtin_type_identifier(
                                 SXML_TYPE_XS_INTEGER)})) {
        return sxml_error_record_report(context->error_record,
                                        &sxml_range_operand_error);
    }
    sxml_item_retain(single.item);
    *integer_value = sxml_sequence_from_item(single.item);
    return SXML_STATUS_OK;
}

/* "to" (XPath 4.0, section 4.8.2) over atomized operands. */
static sxml_status sxml_make_range_value(sxml_dynamic_context *context,
                                         const sxml_sequence *operands,
                                         sxml_sequence *result) {
    sxml_sequence first = sxml_sequence_empty();
    sxml_sequence last = sxml_sequence_empty();
    sxml_status status = sxml_range_operand(context, operands[0], &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_range_operand(context, operands[1], &last);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_sequence_representation first_representation =
            sxml_sequence_get_representation(first);
        const sxml_sequence_representation last_representation =
            sxml_sequence_get_representation(last);
        if (first_representation == SXML_SEQUENCE_REPRESENTATION_EMPTY ||
            last_representation == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
            *result = sxml_sequence_empty();
        } else if (sxml_item_get_representation(first.content) ==
                       SXML_ITEM_REPRESENTATION_INTEGER &&
                   sxml_item_get_representation(last.content) ==
                       SXML_ITEM_REPRESENTATION_INTEGER) {
            status = sxml_sequence_make_integer_range(
                context->item_heap,
                (sxml_integer_range){.first_integer = first.content.payload.integer,
                                     .last_integer = last.content.payload.integer},
                result);
        } else {
            /* A bound beyond int64_t (D36): empty when the range is empty,
               otherwise more items than sxml counts (FOAR0002, which QT4
               rangeExpr-31 allows). */
            sxml_sequence greater = sxml_sequence_empty();
            status = sxml_operator_make_value_comparison(
                context->item_heap,
                &(sxml_comparison_request){
                    .comparison_operator = SXML_COMPARISON_OPERATOR_GREATER,
                    .left_operand = first,
                    .right_operand = last,
                    .implicit_timezone_offset_minutes = 0,
                    .namespace_bindings = {.namespace_bindings = NULL,
                                           .namespace_binding_count = 0},
                    .error_record = context->error_record},
                &greater);
            if (status == SXML_STATUS_OK) {
                const bool is_empty = greater.content.payload.integer != 0;
                sxml_sequence_release(greater);
                if (is_empty) {
                    *result = sxml_sequence_empty();
                } else {
                    status = sxml_error_record_report(context->error_record,
                                                      &sxml_range_overflow_error);
                }
            }
        }
    }
    sxml_sequence_release(first);
    sxml_sequence_release(last);
    return status;
}

/* Text being concatenated in allocator memory. */
typedef struct sxml_text_buffer {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} sxml_text_buffer;

static sxml_status sxml_text_buffer_append(sxml_text_buffer *buffer,
                                           sxml_byte_view text) {
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t required = 0;
    sxml_status status = sxml_size_add(buffer->byte_count, text.byte_count, &required);
    if (status == SXML_STATUS_OK) {
        void *elements = buffer->bytes;
        status = sxml_evaluator_grow(
            buffer->allocator,
            &(sxml_evaluator_growth){.elements = &elements,
                                     .element_capacity = &buffer->byte_capacity,
                                     .element_count = buffer->byte_count,
                                     .required_count = required,
                                     .element_byte_count = 1,
                                     .alignment = 1});
        buffer->bytes = elements;
    }
    if (status == SXML_STATUS_OK) {
        memcpy(buffer->bytes + buffer->byte_count, text.bytes, text.byte_count);
        buffer->byte_count = required;
    }
    return status;
}

/* Appends the canonical text of an atomic item (casting to xs:string, F&O
   4.0, section 19.1.2). */
static sxml_status sxml_text_buffer_append_item(sxml_dynamic_context *context,
                                                sxml_text_buffer *buffer,
                                                sxml_item item) {
    uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    sxml_canonical_text text;
    sxml_status status = sxml_cast_write_canonical_text(
        context->item_heap,
        &(sxml_canonical_text_request){
            .item = item, .output = {.bytes = output, .byte_count = sizeof(output)}},
        &text);
    if (status != SXML_STATUS_CAPACITY_EXCEEDED) {
        return status == SXML_STATUS_OK ? sxml_text_buffer_append(buffer, text.text)
                                        : status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        context->allocator,
        (sxml_allocation_request){.byte_count = text.required_byte_count, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_cast_write_canonical_text(
        context->item_heap,
        &(sxml_canonical_text_request){
            .item = item,
            .output = {.bytes = memory, .byte_count = text.required_byte_count}},
        &text);
    if (status == SXML_STATUS_OK) {
        status = sxml_text_buffer_append(buffer, text.text);
    }
    sxml_allocator_release(context->allocator, memory);
    return status;
}

/* "||" (XPath 4.0, section 4.10) over atomized operands. */
static sxml_status sxml_make_concatenation_value(sxml_dynamic_context *context,
                                                 const sxml_sequence *operands,
                                                 sxml_sequence *result) {
    sxml_text_buffer buffer = {.allocator = context->allocator,
                               .bytes = NULL,
                               .byte_count = 0,
                               .byte_capacity = 0};
    sxml_status status = SXML_STATUS_OK;
    for (size_t operand_index = 0; status == SXML_STATUS_OK && operand_index < 2;
         operand_index += 1) {
        sxml_single_operand single;
        status = sxml_operator_read_single_operand(operands[operand_index],
                                                   context->error_record, &single);
        if (status == SXML_STATUS_TYPE_ERROR) {
            status = sxml_error_record_report(context->error_record,
                                              &sxml_concatenation_operand_error);
        }
        if (status == SXML_STATUS_OK && single.has_item) {
            status = sxml_text_buffer_append_item(context, &buffer, single.item);
        }
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            context->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = {.bytes = buffer.bytes,
                                                                .byte_count =
                                                                    buffer.byte_count},
                                                       .codepoint_count = 0,
                                                       .is_ascii = false}}}},
            &item);
    }
    if (buffer.bytes != NULL) {
        sxml_allocator_release(context->allocator, buffer.bytes);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* A BINARY expression over its operands (borrowed). */
typedef struct sxml_binary_operands {
    sxml_ir_binary_operator binary_operator;
    /* Borrowed, as evaluated. */
    sxml_sequence raw_operands[2];
} sxml_binary_operands;

/* Charges one step per pair of items of two atomized operands (D89). A
   count that fails is left for the comparison to raise. */
static sxml_status sxml_charge_item_pairs(sxml_dynamic_context *context,
                                          const sxml_sequence *operands) {
    uint64_t left_count = 0;
    uint64_t right_count = 0;
    if (sxml_sequence_count(operands[0], NULL, &left_count) != SXML_STATUS_OK ||
        sxml_sequence_count(operands[1], NULL, &right_count) != SXML_STATUS_OK) {
        return SXML_STATUS_OK;
    }
    const uint64_t pair_count = right_count != 0 && left_count > UINT64_MAX / right_count
                                    ? UINT64_MAX
                                    : left_count * right_count;
    return sxml_dynamic_context_charge_steps(context, pair_count);
}

/* A general comparison of an integer range with one xs:integer item, in
   either order: decided from the range's bounds in O(1). */
typedef struct sxml_range_comparison_request {
    sxml_comparison_operator comparison_operator;
    /* The two atomized operands, borrowed. */
    const sxml_sequence *operands;
} sxml_range_comparison_request;

typedef struct sxml_range_comparison {
    /* False when the operands are not a range and one integer. */
    bool is_decided;
    bool is_true;
} sxml_range_comparison;

/*
 * XPath 4.0, section 4.11.2: a general comparison is true when some pair
 * of items compares true. For the items first to last of a range R (in
 * either order: first is its smallest, last its largest) and an
 * integer V, R = V is first <= V <= last, R != V needs an item other than
 * V, R < V is first < V, R > V is last > V, and so on; with the range on
 * the right the order comparisons look at the other bound.
 */
static sxml_status sxml_compare_with_range(sxml_dynamic_context *context,
                                           sxml_range_comparison_request request,
                                           sxml_range_comparison *comparison) {
    comparison->is_decided = false;
    const sxml_sequence *operands = request.operands;
    const bool range_is_left = sxml_is_integer_range(operands[0]);
    const sxml_sequence range = operands[range_is_left ? 0 : 1];
    const sxml_sequence other = operands[range_is_left ? 1 : 0];
    if (!sxml_is_integer_range(range) ||
        sxml_sequence_get_representation(other) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_item_get_representation(other.content) != SXML_ITEM_REPRESENTATION_INTEGER) {
        return SXML_STATUS_OK;
    }
    sxml_item head_item = {.representation = 0};
    sxml_item foot_item = {.representation = 0};
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(range, context->error_record, &item_count);
    if (status != SXML_STATUS_OK || item_count == 0) {
        return status;
    }
    status = sxml_sequence_get_item(
        range, (sxml_item_request){.item_index = 0, .error_record = context->error_record},
        &head_item);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            range,
            (sxml_item_request){.item_index = item_count - 1,
                                .error_record = context->error_record},
            &foot_item);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_ascending = head_item.payload.integer <= foot_item.payload.integer;
    const int64_t first = is_ascending ? head_item.payload.integer : foot_item.payload.integer;
    const int64_t last = is_ascending ? foot_item.payload.integer : head_item.payload.integer;
    const int64_t value = other.content.payload.integer;
    /* The bound that decides an order comparison: with the range on the
       left, "R < V" asks for its smallest item, "R > V" for its largest. */
    bool is_true = false;
    switch (request.comparison_operator) {
    case SXML_COMPARISON_OPERATOR_EQUAL:
        is_true = first <= value && value <= last;
        break;
    case SXML_COMPARISON_OPERATOR_NOT_EQUAL:
        is_true = item_count > 1 || first != value;
        break;
    case SXML_COMPARISON_OPERATOR_LESS:
        is_true = range_is_left ? first < value : value < last;
        break;
    case SXML_COMPARISON_OPERATOR_LESS_OR_EQUAL:
        is_true = range_is_left ? first <= value : value <= last;
        break;
    case SXML_COMPARISON_OPERATOR_GREATER:
        is_true = range_is_left ? last > value : value > first;
        break;
    case SXML_COMPARISON_OPERATOR_GREATER_OR_EQUAL:
        is_true = range_is_left ? last >= value : value >= first;
        break;
    }
    comparison->is_decided = true;
    comparison->is_true = is_true;
    return SXML_STATUS_OK;
}

static sxml_status sxml_make_binary_value(sxml_dynamic_context *context,
                                          const sxml_binary_operands *binary,
                                          sxml_sequence *result) {
    const sxml_ir_binary_operator binary_operator = binary->binary_operator;
    const sxml_sequence *raw_operands = binary->raw_operands;
    switch (binary_operator) {
    case SXML_IR_BINARY_OPERATOR_IS:
    case SXML_IR_BINARY_OPERATOR_IS_NOT:
    case SXML_IR_BINARY_OPERATOR_PRECEDES:
    case SXML_IR_BINARY_OPERATOR_FOLLOWS:
    case SXML_IR_BINARY_OPERATOR_PRECEDES_OR_IS:
    case SXML_IR_BINARY_OPERATOR_FOLLOWS_OR_IS:
        /* GNode comparisons take the operands as they are (4.11.3). */
        return sxml_operator_make_node_comparison(
            &(sxml_node_comparison_request){
                .node_comparison_operator =
                    sxml_node_comparison_operator_of(binary_operator),
                .left_operand = raw_operands[0],
                .right_operand = raw_operands[1],
                .error_record = context->error_record},
            result);
    case SXML_IR_BINARY_OPERATOR_UNION:
    case SXML_IR_BINARY_OPERATOR_INTERSECT:
    case SXML_IR_BINARY_OPERATOR_EXCEPT:
        return sxml_evaluator_make_node_set_value(
            context,
            &(sxml_node_set_request){
                .node_set_operator =
                    binary_operator == SXML_IR_BINARY_OPERATOR_UNION
                        ? SXML_NODE_SET_OPERATOR_UNION
                        : (binary_operator == SXML_IR_BINARY_OPERATOR_INTERSECT
                               ? SXML_NODE_SET_OPERATOR_INTERSECT
                               : SXML_NODE_SET_OPERATOR_EXCEPT),
                .left_operand = raw_operands[0],
                .right_operand = raw_operands[1],
                .error_record = context->error_record},
            result);
    case SXML_IR_BINARY_OPERATOR_ADD:
    case SXML_IR_BINARY_OPERATOR_SUBTRACT:
    case SXML_IR_BINARY_OPERATOR_MULTIPLY:
    case SXML_IR_BINARY_OPERATOR_DIVIDE:
    case SXML_IR_BINARY_OPERATOR_INTEGER_DIVIDE:
    case SXML_IR_BINARY_OPERATOR_MODULO:
    case SXML_IR_BINARY_OPERATOR_VALUE_EQUAL:
    case SXML_IR_BINARY_OPERATOR_VALUE_NOT_EQUAL:
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS:
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER:
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_NOT_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS:
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER:
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER_OR_EQUAL:
    case SXML_IR_BINARY_OPERATOR_RANGE:
    case SXML_IR_BINARY_OPERATOR_CONCATENATE:
        break;
    }
    /* Every other operator atomizes its operands (2.5.3). */
    sxml_sequence operands[2] = {sxml_sequence_empty(), sxml_sequence_empty()};
    sxml_status status = sxml_machine_atomize(context, raw_operands[0], &operands[0]);
    if (status == SXML_STATUS_OK) {
        status = sxml_machine_atomize(context, raw_operands[1], &operands[1]);
    }
    int16_t timezone = 0;
    if (status == SXML_STATUS_OK && binary_operator != SXML_IR_BINARY_OPERATOR_RANGE &&
        binary_operator != SXML_IR_BINARY_OPERATOR_CONCATENATE) {
        status = sxml_machine_operand_timezone(context, operands, &timezone);
    }
    if (status == SXML_STATUS_OK) {
        switch (binary_operator) {
        case SXML_IR_BINARY_OPERATOR_ADD:
        case SXML_IR_BINARY_OPERATOR_SUBTRACT:
        case SXML_IR_BINARY_OPERATOR_MULTIPLY:
        case SXML_IR_BINARY_OPERATOR_DIVIDE:
        case SXML_IR_BINARY_OPERATOR_INTEGER_DIVIDE:
        case SXML_IR_BINARY_OPERATOR_MODULO:
            status = sxml_operator_make_arithmetic_value(
                context->item_heap,
                &(sxml_arithmetic_request){
                    .arithmetic_operator = sxml_arithmetic_operator_of(binary_operator),
                    .left_operand = operands[0],
                    .right_operand = operands[1],
                    .implicit_timezone_offset_minutes = timezone,
                    .error_record = context->error_record},
                result);
            break;
        case SXML_IR_BINARY_OPERATOR_VALUE_EQUAL:
        case SXML_IR_BINARY_OPERATOR_VALUE_NOT_EQUAL:
        case SXML_IR_BINARY_OPERATOR_VALUE_LESS:
        case SXML_IR_BINARY_OPERATOR_VALUE_LESS_OR_EQUAL:
        case SXML_IR_BINARY_OPERATOR_VALUE_GREATER:
        case SXML_IR_BINARY_OPERATOR_VALUE_GREATER_OR_EQUAL:
            status = sxml_operator_make_value_comparison(
                context->item_heap,
                &(sxml_comparison_request){
                    .comparison_operator = sxml_comparison_operator_of(binary_operator),
                    .left_operand = operands[0],
                    .right_operand = operands[1],
                    .implicit_timezone_offset_minutes = timezone,
                    .namespace_bindings = {.namespace_bindings = NULL,
                                           .namespace_binding_count = 0},
                    .error_record = context->error_record},
                result);
            break;
        case SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL:
        case SXML_IR_BINARY_OPERATOR_GENERAL_NOT_EQUAL:
        case SXML_IR_BINARY_OPERATOR_GENERAL_LESS:
        case SXML_IR_BINARY_OPERATOR_GENERAL_LESS_OR_EQUAL:
        case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER:
        case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER_OR_EQUAL: {
            sxml_item item = {.representation = 0};
            sxml_range_comparison range_comparison = {.is_decided = false};
            status = sxml_compare_with_range(
                context,
                (sxml_range_comparison_request){
                    .comparison_operator = sxml_comparison_operator_of(binary_operator),
                    .operands = operands},
                &range_comparison);
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (range_comparison.is_decided) {
                *result = sxml_sequence_from_item(range_comparison.is_true
                                                      ? sxml_atomic_item_true()
                                                      : sxml_atomic_item_false());
                break;
            }
            /* A general comparison compares every pair (XPath 4.0, section
               4.11.2): charge them first, so long operands cannot run
               unbounded (D89). Counting is O(1) for ranges. */
            status = sxml_charge_item_pairs(context, operands);
            if (status != SXML_STATUS_OK) {
                break;
            }
            status = sxml_operator_make_general_comparison(
                context->item_heap,
                &(sxml_comparison_request){
                    .comparison_operator = sxml_comparison_operator_of(binary_operator),
                    .left_operand = operands[0],
                    .right_operand = operands[1],
                    .implicit_timezone_offset_minutes = timezone,
                    .namespace_bindings = {.namespace_bindings = NULL,
                                           .namespace_binding_count = 0},
                    .error_record = context->error_record},
                &item);
            if (status == SXML_STATUS_OK) {
                *result = sxml_sequence_from_item(item);
            }
            break;
        }
        case SXML_IR_BINARY_OPERATOR_RANGE:
            status = sxml_make_range_value(context, operands, result);
            break;
        case SXML_IR_BINARY_OPERATOR_CONCATENATE:
            status = sxml_make_concatenation_value(context, operands, result);
            break;
        case SXML_IR_BINARY_OPERATOR_IS:
        case SXML_IR_BINARY_OPERATOR_IS_NOT:
        case SXML_IR_BINARY_OPERATOR_PRECEDES:
        case SXML_IR_BINARY_OPERATOR_FOLLOWS:
        case SXML_IR_BINARY_OPERATOR_PRECEDES_OR_IS:
        case SXML_IR_BINARY_OPERATOR_FOLLOWS_OR_IS:
        case SXML_IR_BINARY_OPERATOR_UNION:
        case SXML_IR_BINARY_OPERATOR_INTERSECT:
        case SXML_IR_BINARY_OPERATOR_EXCEPT:
            status = SXML_STATUS_INVALID_ARGUMENT;
            break;
        }
    }
    sxml_sequence_release(operands[0]);
    sxml_sequence_release(operands[1]);
    return status;
}

/* ------------------------------------------------- Lookups, constructors */

/* Appends the member of array at the position key (from 1). */
typedef struct sxml_array_key {
    sxml_item array;
    sxml_item key;
} sxml_array_key;

static sxml_status sxml_lookup_array_member(sxml_dynamic_context *context,
                                            sxml_array_key array_key,
                                            sxml_sequence_builder *builder) {
    const sxml_item array = array_key.array;
    const sxml_item key = array_key.key;
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(context->item_heap);
    if (!sxml_item_is_atomic(key) ||
        !sxml_type_registry_is_derived_from(
            registry,
            (sxml_type_pair){
                .derived_type = sxml_item_get_type_annotation(key),
                .base_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER)})) {
        return sxml_error_record_report(context->error_record, &sxml_array_key_error);
    }
    size_t member_count = 0;
    sxml_status status = sxml_array_get_size(array, &member_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* An integer beyond int64_t is beyond every array. */
    if (sxml_item_get_representation(key) != SXML_ITEM_REPRESENTATION_INTEGER ||
        key.payload.integer < 1 || (uint64_t)key.payload.integer > member_count) {
        return sxml_error_record_report(context->error_record,
                                        &sxml_array_bounds_error);
    }
    sxml_sequence member = sxml_sequence_empty();
    status = sxml_array_get_member(array, (size_t)(key.payload.integer - 1), &member);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_append_sequence(builder, member,
                                                       context->error_record);
    }
    return status;
}

/* The values a lookup selects in one map or array (XPath 4.0, section
   4.7.3); keys NULL for the wildcard. */
typedef struct sxml_lookup_target {
    sxml_item item;
    const sxml_sequence *keys;
} sxml_lookup_target;

static sxml_status sxml_lookup_in_item(sxml_dynamic_context *context,
                                       sxml_lookup_target target,
                                       sxml_sequence_builder *builder) {
    const sxml_item_kind kind = sxml_item_get_kind(target.item);
    if (kind != SXML_ITEM_KIND_MAP && kind != SXML_ITEM_KIND_ARRAY) {
        return sxml_error_record_report(context->error_record,
                                        &sxml_lookup_operand_error);
    }
    sxml_status status = SXML_STATUS_OK;
    if (target.keys == NULL) {
        /* "?*": every value, in entry or member order. */
        size_t part_count = 0;
        status = kind == SXML_ITEM_KIND_MAP ? sxml_map_get_size(target.item, &part_count)
                                            : sxml_array_get_size(target.item, &part_count);
        for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < part_count;
             part_index += 1) {
            sxml_sequence value = sxml_sequence_empty();
            if (kind == SXML_ITEM_KIND_MAP) {
                sxml_map_entry entry;
                status = sxml_map_get_entry(target.item, part_index, &entry);
                value = entry.value;
            } else {
                status = sxml_array_get_member(target.item, part_index, &value);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_sequence_builder_append_sequence(builder, value,
                                                               context->error_record);
            }
        }
        return status;
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(*target.keys);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, context->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        if (kind == SXML_ITEM_KIND_ARRAY) {
            status = sxml_lookup_array_member(
                context, (sxml_array_key){.array = target.item, .key = step.item},
                builder);
            continue;
        }
        sxml_map_lookup lookup;
        status = sxml_map_find(target.item, step.item, &lookup);
        if (status == SXML_STATUS_OK && lookup.is_found) {
            status = sxml_sequence_builder_append_sequence(builder, lookup.entry.value,
                                                           context->error_record);
        }
    }
    return status;
}

/* LOOKUP: for each item of the value, the values its keys select. */
static sxml_status sxml_make_lookup_value(sxml_dynamic_context *context,
    const sxml_ir_lookup *lookup,
                                          sxml_sequence *result) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_sequence keys = sxml_sequence_empty();
    sxml_status status = sxml_machine_read_atom(context, lookup->value, &value);
    const bool is_wildcard = lookup->key.atom_kind == SXML_IR_ATOM_KIND_NONE;
    if (status == SXML_STATUS_OK && !is_wildcard) {
        sxml_sequence raw_keys = sxml_sequence_empty();
        status = sxml_machine_read_atom(context, lookup->key, &raw_keys);
        if (status == SXML_STATUS_OK) {
            status = sxml_machine_atomize(context, raw_keys, &keys);
        }
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            context->allocator,
            &(sxml_sequence_builder_configuration){.item_heap = context->item_heap},
            &builder);
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, context->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        if (sxml_item_get_kind(step.item) != SXML_ITEM_KIND_JSON_NODE) {
            status = sxml_lookup_in_item(
                context,
                (sxml_lookup_target){.item = step.item, .keys = is_wildcard ? NULL : &keys},
                builder);
            continue;
        }
        /* A JNode is coerced to its ·jvalue·, whose items must be maps or
           arrays (XPath 4.0, section 4.15.3; F&O 4.0, fn:jvalue, notes). */
        sxml_json_node_description description;
        status = sxml_json_node_describe(step.item, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_sequence_iterator value_iterator =
            sxml_sequence_iterator_start(description.json_value);
        while (status == SXML_STATUS_OK) {
            sxml_iteration_step value_step;
            status = sxml_sequence_iterator_next(&value_iterator, context->error_record,
                                                 &value_step);
            if (status != SXML_STATUS_OK || !value_step.has_item) {
                break;
            }
            status = sxml_lookup_in_item(
                context,
                (sxml_lookup_target){.item = value_step.item,
                                     .keys = is_wildcard ? NULL : &keys},
                builder);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    sxml_sequence_release(keys);
    return status;
}

/* Map entries collected for a map constructor, keys and values owned. */
typedef struct sxml_entry_list {
    sxml_allocator allocator;
    sxml_map_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
} sxml_entry_list;

static sxml_status sxml_entry_list_add(sxml_entry_list *list, sxml_map_entry entry) {
    void *elements = list->entries;
    sxml_status status = sxml_evaluator_grow(
        list->allocator,
        &(sxml_evaluator_growth){.elements = &elements,
                                 .element_capacity = &list->entry_capacity,
                                 .element_count = list->entry_count,
                                 .required_count = list->entry_count + 1,
                                 .element_byte_count = sizeof(sxml_map_entry),
                                 .alignment = alignof(sxml_map_entry)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    list->entries = elements;
    sxml_item_retain(entry.key);
    sxml_sequence_retain(entry.value);
    list->entries[list->entry_count] = entry;
    list->entry_count += 1;
    return SXML_STATUS_OK;
}

static void sxml_entry_list_finalize(sxml_entry_list *list) {
    for (size_t entry_index = 0; entry_index < list->entry_count; entry_index += 1) {
        sxml_item_release(list->entries[entry_index].key);
        sxml_sequence_release(list->entries[entry_index].value);
    }
    if (list->entries != NULL) {
        sxml_allocator_release(list->allocator, list->entries);
    }
}

/* Adds the entries of every map in maps (a constructor entry without a
   value, XPath 4.0, section 4.7.1). */
static sxml_status sxml_entry_list_add_maps(sxml_dynamic_context *context,
                                            sxml_entry_list *list, sxml_sequence maps) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(maps);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, context->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        if (sxml_item_get_kind(step.item) != SXML_ITEM_KIND_MAP) {
            return sxml_error_record_report(context->error_record,
                                            &sxml_map_merge_error);
        }
        size_t entry_count = 0;
        status = sxml_map_get_size(step.item, &entry_count);
        for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
             entry_index += 1) {
            sxml_map_entry entry;
            status = sxml_map_get_entry(step.item, entry_index, &entry);
            if (status == SXML_STATUS_OK) {
                status = sxml_entry_list_add(list, entry);
            }
        }
    }
    return status;
}

/* MAP_CONSTRUCTOR: keys atomized, one atomic item each; duplicates raise
   XQDY0137 (XPath 4.0, section 4.7.1). */
static sxml_status sxml_make_map_value(sxml_dynamic_context *context,
    const sxml_ir_map_constructor *constructor,
                                       sxml_sequence *result) {
    const sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_ir_map_entry_view entries =
        sxml_ir_unit_get_map_entries(task->function->unit, constructor->entries);
    sxml_entry_list list = {.allocator = context->allocator,
                            .entries = NULL,
                            .entry_count = 0,
                            .entry_capacity = 0};
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < entries.map_entry_count;
         entry_index += 1) {
        const sxml_ir_map_entry *entry = &entries.map_entries[entry_index];
        sxml_sequence raw_key = sxml_sequence_empty();
        status = sxml_machine_read_atom(context, entry->key, &raw_key);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (entry->value.atom_kind == SXML_IR_ATOM_KIND_NONE) {
            status = sxml_entry_list_add_maps(context, &list, raw_key);
            continue;
        }
        sxml_sequence key = sxml_sequence_empty();
        sxml_sequence value = sxml_sequence_empty();
        status = sxml_machine_atomize(context, raw_key, &key);
        if (status == SXML_STATUS_OK &&
            sxml_sequence_get_representation(key) !=
                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
            status = sxml_error_record_report(context->error_record, &sxml_map_key_error);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_machine_read_atom(context, entry->value, &value);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_entry_list_add(
                &list, (sxml_map_entry){.key = key.content, .value = value});
        }
        sxml_sequence_release(key);
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            context->item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = list.entries,
                                .map_entry_count = list.entry_count},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                .error_record = context->error_record},
            &map);
    }
    sxml_entry_list_finalize(&list);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(map);
    }
    return status;
}

/* ARRAY_CONSTRUCTOR (XPath 4.0, section 4.7.2). */
static sxml_status sxml_make_array_value(sxml_dynamic_context *context,
    const sxml_ir_array_constructor *constructor,
                                         sxml_sequence *result) {
    sxml_sequence *members = NULL;
    sxml_status status =
        sxml_machine_copy_atoms(context, constructor->members, &members);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t atom_count = constructor->members.atom_count;
    sxml_item array = {.representation = 0};
    if (constructor->array_form == SXML_IR_ARRAY_FORM_SQUARE) {
        status = sxml_array_make(
            context->item_heap,
            (sxml_sequence_view){.sequences = members, .sequence_count = atom_count},
            &array);
    } else {
        /* "array { E }": each item of E is one member. */
        const sxml_sequence content = atom_count == 1 ? members[0] : sxml_sequence_empty();
        uint64_t item_count = 0;
        status = sxml_sequence_count(content, context->error_record, &item_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK && item_count > SIZE_MAX / sizeof(sxml_sequence)) {
            status = SXML_STATUS_ARITHMETIC_OVERFLOW;
        }
        if (status == SXML_STATUS_OK && item_count > 0) {
            status = sxml_allocator_allocate(
                context->allocator,
                (sxml_allocation_request){
                    .byte_count = (size_t)item_count * sizeof(sxml_sequence),
                    .alignment = alignof(sxml_sequence)},
                &memory);
        }
        sxml_sequence *item_members = memory;
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(content);
        size_t member_index = 0;
        while (status == SXML_STATUS_OK && member_index < item_count) {
            sxml_iteration_step step;
            status = sxml_sequence_iterator_next(&iterator, context->error_record, &step);
            if (status == SXML_STATUS_OK && step.has_item) {
                item_members[member_index] = sxml_sequence_from_item(step.item);
                member_index += 1;
            }
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_array_make(
                context->item_heap,
                (sxml_sequence_view){.sequences = item_members,
                                     .sequence_count = member_index},
                &array);
        }
        if (memory != NULL) {
            sxml_allocator_release(context->allocator, memory);
        }
    }
    for (size_t member_index = 0; member_index < atom_count; member_index += 1) {
        sxml_sequence_release(members[member_index]);
    }
    if (members != NULL) {
        sxml_allocator_release(context->allocator, members);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(array);
    }
    return status;
}

/* CAST and CASTABLE (XPath 4.0, section 4.17). */
static sxml_status sxml_make_cast_value(sxml_dynamic_context *context,
                                        const sxml_ir_expression *expression,
                                        sxml_sequence *result) {
    const sxml_ir_cast *cast = &expression->cast;
    sxml_sequence value = sxml_sequence_empty();
    sxml_sequence atomized = sxml_sequence_empty();
    sxml_status status = sxml_machine_read_atom(context, cast->value, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_machine_atomize(context, value, &atomized);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* A cast to xs:QName resolves prefixes with the statically known
       namespaces (F&O 4.0, section 19.1.9; D84). */
    const sxml_cast_request request = {
        .value = atomized,
        .target_type = cast->target_type,
        .allows_empty = cast->allows_empty,
        .namespace_bindings =
            sxml_ir_unit_get_static_namespaces(sxml_machine_top(context)->function->unit),
        .error_record = context->error_record};
    if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_CAST) {
        status = sxml_cast_make_value(context->item_heap, &request, result);
    } else {
        sxml_castability castability = {.is_castable = false};
        status = sxml_cast_test_castability(context->item_heap, &request, &castability);
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(castability.is_castable
                                                  ? sxml_atomic_item_true()
                                                  : sxml_atomic_item_false());
        }
    }
    sxml_sequence_release(atomized);
    return status;
}

/* RAISE: the error lowering proved (XPDY0002 for "." without focus, ...). */
static sxml_status sxml_raise_error(sxml_dynamic_context *context,
                                    const sxml_ir_raise *raise) {
    if (context->error_record != NULL) {
        *context->error_record = (sxml_error_record){
            .code = raise->error_code,
            .value = NULL,
            .diagnostic = {.description = raise->description,
                           .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                        .line_number = 0,
                                        .column_number = 0,
                                        .byte_offset = 0}}};
    }
    return sxml_status_has_error_record(raise->error_status) ? raise->error_status
                                                             : SXML_STATUS_DYNAMIC_ERROR;
}

/* ---------------------------------------------------------------- Loops */

/* Binds the focus slots of a MAP, FILTER or SORT. */
typedef struct sxml_focus_binding {
    size_t frame_base;
    sxml_ir_focus focus;
    /* Borrowed; retained into the slot. */
    sxml_sequence value;
    uint64_t position;
    uint64_t size;
} sxml_focus_binding;

static void sxml_machine_bind_focus(sxml_dynamic_context *context,
                                    const sxml_focus_binding *binding) {
    sxml_sequence_retain(binding->value);
    sxml_machine_bind(context,
                      (sxml_slot_binding){.frame_base = binding->frame_base,
                                          .slot = binding->focus.context_value},
                      binding->value);
    sxml_machine_bind(context,
                      (sxml_slot_binding){.frame_base = binding->frame_base,
                                          .slot = binding->focus.context_position},
                      sxml_sequence_from_item(
                          sxml_atomic_item_from_integer((int64_t)binding->position)));
    sxml_machine_bind(context,
                      (sxml_slot_binding){.frame_base = binding->frame_base,
                                          .slot = binding->focus.context_size},
                      sxml_sequence_from_item(
                          sxml_atomic_item_from_integer((int64_t)binding->size)));
}

/* Reads a loop's input into the task (owned) and counts it when the size
   is read (fn:last()). */
typedef struct sxml_loop_input {
    sxml_ir_atom input;
    /* True when the context size is read (fn:last()). */
    bool needs_size;
} sxml_loop_input;

static sxml_status sxml_loop_start(sxml_dynamic_context *context,
                                   sxml_loop_input loop_input) {
    const sxml_ir_atom input = loop_input.input;
    const bool needs_size = loop_input.needs_size;
    sxml_evaluator_task *task = sxml_machine_top(context);
    sxml_loop_state *loop = &task->details.loop;
    sxml_status status = sxml_machine_copy_atom(context, input, &loop->input);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    loop->iterator = sxml_sequence_iterator_start(loop->input);
    loop->position = 0;
    loop->size = 0;
    if (needs_size) {
        status = sxml_sequence_count(loop->input, context->error_record, &loop->size);
    }
    task->phase = 1;
    return status;
}

/* Appends value (owned) to the loop's builder. */
static sxml_status sxml_loop_append(sxml_dynamic_context *context,
                                    sxml_loop_state *loop, sxml_sequence value) {
    sxml_status status = SXML_STATUS_OK;
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        if (loop->builder == NULL) {
            status = sxml_sequence_builder_create(
                context->allocator,
                &(sxml_sequence_builder_configuration){.item_heap = context->item_heap},
                &loop->builder);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(loop->builder, value,
                                                           context->error_record);
        }
    }
    sxml_sequence_release(value);
    return status;
}

/* Finishes the loop on top with what its builder collected. */
static sxml_status sxml_loop_finish(sxml_dynamic_context *context) {
    sxml_loop_state *loop = &sxml_machine_top(context)->details.loop;
    sxml_sequence value = sxml_sequence_empty();
    if (loop->builder != NULL) {
        sxml_status status = sxml_sequence_builder_finish(loop->builder, &value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return sxml_machine_finish(context, value);
}

/* ---------------------------------------------- Selectors over JNodes */

/*
 * XPath 4.0 (editor's draft), section 4.7.3: in E1/E2, when the item N
 * that supplies the focus is a JNode and E2 is not a navigational
 * expression, the selector interpretation applies: E2 is the dynamic node
 * test {E2} on the child axis (section 4.7.5.4), evaluated with an absent
 * focus (XPDY0002 when it depends on the focus), and the result is the
 * children of N whose ·jkey· is atomic-equal to an item of E2's atomized
 * value. Otherwise E2 is evaluated with N as the focus.
 *
 * Whether E2 is navigational is decided from the IR of the path MAP's
 * body (lowering keeps E2's shape): the context value ".", an axis step
 * (STEP), a path (a MAP that requires nodes, DOCUMENT_ORDER), the root of
 * an absolute path (fn:root, TREAT), a filter of a navigational input,
 * and union, intersect, except, sequences, conditionals and lets whose
 * results are all navigational. A body too large for the fixed tables
 * below counts as navigational (the Phase 1 behaviour).
 */

/* Bits of the task phase of a path's MAP beyond bit 0, which
   sxml_loop_start sets. */
enum {
    /* The body now running is a selector for loop->current_item. */
    SXML_MAP_PHASE_SELECTING = 1U << 1U,
    /* The body was analysed; NAVIGATIONAL holds the outcome. */
    SXML_MAP_PHASE_ANALYSED = 1U << 2U,
    SXML_MAP_PHASE_NAVIGATIONAL = 1U << 3U,
};

/* How many lets and pending expressions the analysis follows. */
#define SXML_NAVIGATION_TABLE_SIZE 48

typedef struct sxml_navigation_binding {
    sxml_ir_slot slot;
    sxml_ir_expression_index expression;
} sxml_navigation_binding;

typedef struct sxml_navigation_analysis {
    const sxml_ir_unit *unit;
    sxml_ir_slot focus_slot;
    sxml_navigation_binding bindings[SXML_NAVIGATION_TABLE_SIZE];
    size_t binding_count;
    sxml_ir_expression_index pending[SXML_NAVIGATION_TABLE_SIZE];
    size_t pending_count;
    /* False once a result is found that is not navigational. */
    bool is_navigational;
    /* True when a table overflowed. */
    bool is_overflowed;
} sxml_navigation_analysis;

static void sxml_navigation_push(sxml_navigation_analysis *analysis,
                                 sxml_ir_expression_index expression) {
    if (analysis->pending_count == SXML_NAVIGATION_TABLE_SIZE) {
        analysis->is_overflowed = true;
        return;
    }
    analysis->pending[analysis->pending_count] = expression;
    analysis->pending_count += 1;
}

/* An atom in a result position: "." is navigational, a slot bound by a
   let of the body is what it is bound to, anything else is not. */
static void sxml_navigation_follow_atom(sxml_navigation_analysis *analysis,
                                        sxml_ir_atom atom) {
    if (atom.atom_kind == SXML_IR_ATOM_KIND_SLOT) {
        if (analysis->focus_slot.value != 0 &&
            atom.slot.value == analysis->focus_slot.value) {
            return;
        }
        for (size_t binding_index = 0; binding_index < analysis->binding_count;
             binding_index += 1) {
            if (analysis->bindings[binding_index].slot.value == atom.slot.value) {
                sxml_navigation_push(analysis,
                                     analysis->bindings[binding_index].expression);
                return;
            }
        }
    }
    analysis->is_navigational = false;
}

static void sxml_navigation_step(sxml_navigation_analysis *analysis,
                                 const sxml_ir_expression *expression) {
    const sxml_ir_expression_kind kind = expression->expression_kind;
    if (kind == SXML_IR_EXPRESSION_KIND_STEP ||
        kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER) {
        return;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_MAP) {
        if ((expression->map.flags & SXML_IR_MAP_REQUIRES_NODES) == 0) {
            analysis->is_navigational = false;
        }
        return;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_STATIC_CALL) {
        /* fn:root of an absolute path "/" (section 4.7.1). */
        const sxml_ir_function_target target = expression->static_call.target;
        if (target.target_kind != SXML_IR_FUNCTION_TARGET_KIND_NATIVE ||
            !sxml_byte_view_equals(target.native_function->local_name,
                                   SXML_BYTE_VIEW_FROM_LITERAL("root"))) {
            analysis->is_navigational = false;
        }
        return;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_ATOM) {
        sxml_navigation_follow_atom(analysis, expression->atom);
        return;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_LET) {
        if (analysis->binding_count == SXML_NAVIGATION_TABLE_SIZE) {
            analysis->is_overflowed = true;
            return;
        }
        analysis->bindings[analysis->binding_count] = (sxml_navigation_binding){
            .slot = expression->let.variable,
            .expression = expression->let.bound_expression};
        analysis->binding_count += 1;
        sxml_navigation_push(analysis, expression->let.body);
        return;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL) {
        sxml_navigation_push(analysis, expression->conditional.then_branch);
        sxml_navigation_push(analysis, expression->conditional.else_branch);
        return;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        sxml_navigation_follow_atom(analysis, expression->filter.input);
        return;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_TREAT) {
        sxml_navigation_follow_atom(analysis, expression->type_test.value);
        return;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_BINARY &&
        (expression->binary.operator == SXML_IR_BINARY_OPERATOR_UNION ||
         expression->binary.operator == SXML_IR_BINARY_OPERATOR_INTERSECT ||
         expression->binary.operator == SXML_IR_BINARY_OPERATOR_EXCEPT)) {
        sxml_navigation_follow_atom(analysis, expression->binary.left_operand);
        sxml_navigation_follow_atom(analysis, expression->binary.right_operand);
        return;
    }
    if (kind == SXML_IR_EXPRESSION_KIND_SEQUENCE) {
        const sxml_ir_atom_view parts =
            sxml_ir_unit_get_atoms(analysis->unit, expression->sequence);
        for (size_t part_index = 0; part_index < parts.atom_count; part_index += 1) {
            sxml_navigation_follow_atom(analysis, parts.atoms[part_index]);
        }
        return;
    }
    analysis->is_navigational = false;
}

/* Whether the body of the path MAP map, of the top task's function, is a
   navigational expression. */
static bool sxml_path_body_is_navigational(const sxml_ir_unit *unit,
                                           const sxml_ir_map *map) {
    sxml_navigation_analysis analysis = {.unit = unit,
                                         .focus_slot = map->focus.context_value,
                                         .binding_count = 0,
                                         .pending_count = 0,
                                         .is_navigational = true,
                                         .is_overflowed = false};
    sxml_navigation_push(&analysis, map->body);
    while (analysis.pending_count > 0 && analysis.is_navigational &&
           !analysis.is_overflowed) {
        analysis.pending_count -= 1;
        sxml_navigation_step(&analysis, sxml_ir_unit_get_expression(
                                            unit, analysis.pending[analysis.pending_count]));
    }
    return analysis.is_navigational || analysis.is_overflowed;
}

static const sxml_static_error sxml_selector_focus_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A step that selects from a JNode is evaluated with an absent focus, "
        "and this one depends on the focus (XPath 4.0, sections 4.7.3 and "
        "4.7.5.4)"),
};

/* The children of json_node whose ·jkey· is atomic-equal to an item of
   selector (owned; atomized here), in document order (section 4.7.5.4). */
typedef struct sxml_json_selection {
    sxml_item json_node;
    sxml_sequence selector;
} sxml_json_selection;

static sxml_status sxml_select_json_children(sxml_dynamic_context *context,
                                             sxml_json_selection selection,
                                             sxml_sequence *result) {
    sxml_sequence keys = sxml_sequence_empty();
    sxml_status status = sxml_machine_atomize(context, selection.selector, &keys);
    sxml_sequence_release(selection.selector);
    uint64_t key_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(keys, context->error_record, &key_count);
    }
    sxml_json_node_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_json_node_describe(selection.json_node, &description);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK && key_count > 0 && description.child_count > 0) {
        status = sxml_sequence_builder_create(
            context->allocator,
            &(sxml_sequence_builder_configuration){.item_heap = context->item_heap},
            &builder);
    }
    for (uint64_t child_index = 0;
         status == SXML_STATUS_OK && builder != NULL && child_index < description.child_count;
         child_index += 1) {
        sxml_item child = {.representation = 0};
        status = sxml_json_node_make_child(
            context->item_heap,
            (sxml_json_child_request){.json_node = selection.json_node,
                                      .child_index = child_index},
            &child);
        sxml_json_node_description child_description;
        if (status == SXML_STATUS_OK) {
            status = sxml_json_node_describe(child, &child_description);
        }
        bool is_selected = false;
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(keys);
        while (status == SXML_STATUS_OK && !is_selected) {
            sxml_iteration_step key_step;
            status = sxml_sequence_iterator_next(&iterator, context->error_record, &key_step);
            if (status != SXML_STATUS_OK || !key_step.has_item) {
                break;
            }
            is_selected = sxml_atomic_item_is_atomic_equal(key_step.item,
                                                           child_description.json_key);
        }
        if (status == SXML_STATUS_OK && is_selected) {
            status = sxml_sequence_builder_append_item(builder, child);
        }
        sxml_item_release(child);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK && builder != NULL) {
        status = sxml_sequence_builder_finish(builder, &value);
    }
    sxml_sequence_builder_destroy(builder);
    sxml_sequence_release(keys);
    if (status == SXML_STATUS_OK) {
        *result = value;
    }
    return status;
}

/* Whether a loop has run out of items. */
typedef struct sxml_loop_progress {
    bool is_done;
} sxml_loop_progress;

/* Moves the loop on top to its next item: binds it and pushes body, or
   sets progress->is_done. */
typedef struct sxml_loop_advance {
    sxml_ir_focus focus;
    sxml_ir_expression_index body;
    /* A path's MAP: every item must be a node (XPTY0019), and a JNode
       may select by the value of body (see "Selectors over JNodes");
       NULL for other loops. */
    const sxml_ir_map *path_map;
} sxml_loop_advance;

static sxml_status sxml_loop_advance_focus(sxml_dynamic_context *context,
                                           sxml_loop_advance advance,
                                           sxml_loop_progress *progress) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    sxml_loop_state *loop = &task->details.loop;
    sxml_iteration_step step;
    sxml_status status =
        sxml_sequence_iterator_next(&loop->iterator, context->error_record, &step);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    progress->is_done = !step.has_item;
    if (progress->is_done) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_map *path_map = advance.path_map;
    if (path_map != NULL && !sxml_item_is_generic_node(step.item)) {
        return sxml_error_record_report(context->error_record,
                                        &sxml_path_operand_error);
    }
    if (path_map != NULL &&
        sxml_item_get_kind(step.item) == SXML_ITEM_KIND_JSON_NODE) {
        if ((task->phase & SXML_MAP_PHASE_ANALYSED) == 0) {
            task->phase |= SXML_MAP_PHASE_ANALYSED;
            if (sxml_path_body_is_navigational(task->function->unit, path_map)) {
                task->phase |= SXML_MAP_PHASE_NAVIGATIONAL;
            }
        }
        if ((task->phase & SXML_MAP_PHASE_NAVIGATIONAL) == 0) {
            /* The selector interpretation (section 4.7.3): the body must
               not depend on the focus. */
            if (advance.focus.context_value.value != 0 ||
                advance.focus.context_position.value != 0 ||
                advance.focus.context_size.value != 0) {
                return sxml_error_record_report(context->error_record,
                                                &sxml_selector_focus_error);
            }
            task->phase |= SXML_MAP_PHASE_SELECTING;
        }
    }
    loop->position += 1;
    loop->current_item = step.item;
    sxml_machine_bind_focus(context,
                            &(sxml_focus_binding){.frame_base = task->frame_base,
                                                  .focus = advance.focus,
                                                  .value = sxml_sequence_from_item(step.item),
                                                  .position = loop->position,
                                                  .size = loop->size});
    const sxml_evaluator_task body =
        sxml_expression_task(task->function, task->frame_base, advance.body);
    return sxml_machine_push(context, &body);
}

/*
 * The left operand of "/" (XPath 4.0, section 4.7.3): maps and arrays in
 * the input of the path's MAP are converted to JNodes as if by fn:jtree,
 * each the root of a JTree of its own. Replaces loop->input when there are
 * any and restarts its iterator.
 */
static sxml_status sxml_loop_convert_to_json_nodes(sxml_dynamic_context *context,
                                                   sxml_loop_state *loop) {
    if (sxml_is_integer_range(loop->input)) {
        /* Integers only (and possibly very many). */
        return SXML_STATUS_OK;
    }
    sxml_sequence_builder *builder = NULL;
    sxml_status status = SXML_STATUS_OK;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(loop->input);
    uint64_t item_index = 0;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, context->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        const sxml_item_kind kind = sxml_item_get_kind(step.item);
        const bool is_collection =
            kind == SXML_ITEM_KIND_MAP || kind == SXML_ITEM_KIND_ARRAY;
        if (is_collection && builder == NULL) {
            status = sxml_sequence_builder_create(
                context->allocator,
                &(sxml_sequence_builder_configuration){.item_heap = context->item_heap},
                &builder);
            for (uint64_t earlier_index = 0;
                 status == SXML_STATUS_OK && earlier_index < item_index;
                 earlier_index += 1) {
                sxml_item earlier = {.representation = 0};
                status = sxml_sequence_get_item(
                    loop->input,
                    (sxml_item_request){.item_index = earlier_index,
                                        .error_record = context->error_record},
                    &earlier);
                if (status == SXML_STATUS_OK) {
                    status = sxml_sequence_builder_append_item(builder, earlier);
                }
            }
        }
        if (status == SXML_STATUS_OK && is_collection) {
            sxml_item root = {.representation = 0};
            status = sxml_json_node_make_root(context->item_heap,
                                              sxml_sequence_from_item(step.item), &root);
            if (status == SXML_STATUS_OK) {
                status = sxml_sequence_builder_append_item(builder, root);
                sxml_item_release(root);
            }
        } else if (status == SXML_STATUS_OK && builder != NULL) {
            status = sxml_sequence_builder_append_item(builder, step.item);
        }
        item_index += 1;
    }
    sxml_sequence converted = sxml_sequence_empty();
    if (status == SXML_STATUS_OK && builder != NULL) {
        status = sxml_sequence_builder_finish(builder, &converted);
        if (status == SXML_STATUS_OK) {
            sxml_sequence_release(loop->input);
            loop->input = converted;
            loop->iterator = sxml_sequence_iterator_start(loop->input);
        }
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* MAP (XPath 4.0, editor's draft of 13 October 2026, sections 4.7.3 and 4.21). */
static sxml_status sxml_machine_step_map(sxml_dynamic_context *context,
                                         const sxml_ir_map *map) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    sxml_status status = SXML_STATUS_OK;
    if (task->phase == 0) {
        status = sxml_loop_start(
            context, (sxml_loop_input){.input = map->input,
                                       .needs_size = map->focus.context_size.value != 0});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        task = sxml_machine_top(context);
        if ((map->flags & SXML_IR_MAP_REQUIRES_NODES) != 0) {
            status = sxml_loop_convert_to_json_nodes(context, &task->details.loop);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        if ((map->flags & SXML_IR_MAP_ALLOWING_EMPTY) != 0 &&
            sxml_sequence_get_representation(task->details.loop.input) ==
                SXML_SEQUENCE_REPRESENTATION_EMPTY) {
            /* XQuery "allowing empty": the body once, with the empty
               sequence and position 0. */
            sxml_machine_bind_focus(
                context, &(sxml_focus_binding){.frame_base = task->frame_base,
                                               .focus = map->focus,
                                               .value = sxml_sequence_empty(),
                                               .position = 0,
                                               .size = 0});
            const sxml_evaluator_task body =
                sxml_expression_task(task->function, task->frame_base, map->body);
            return sxml_machine_push(context, &body);
        }
    } else {
        sxml_sequence value = sxml_machine_take_value(context);
        if ((task->phase & SXML_MAP_PHASE_SELECTING) != 0) {
            task->phase &= ~(uint32_t)SXML_MAP_PHASE_SELECTING;
            sxml_sequence selected = sxml_sequence_empty();
            status = sxml_select_json_children(
                context,
                (sxml_json_selection){.json_node = task->details.loop.current_item,
                                      .selector = value},
                &selected);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            value = selected;
        }
        status = sxml_loop_append(context, &task->details.loop, value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_loop_progress progress = {.is_done = false};
    status = sxml_loop_advance_focus(
        context,
        (sxml_loop_advance){.focus = map->focus,
                            .body = map->body,
                            .path_map = (map->flags & SXML_IR_MAP_REQUIRES_NODES) != 0
                                            ? map
                                            : NULL},
        &progress);
    if (status == SXML_STATUS_OK && progress.is_done) {
        status = sxml_loop_finish(context);
    }
    return status;
}

/* Whether a predicate value selects the item at position (XPath 4.0,
   section 4.4.1): a numeric singleton by equality with the position,
   anything else by its effective boolean value. */
typedef struct sxml_predicate_test {
    sxml_sequence value;
    uint64_t position;
} sxml_predicate_test;

typedef struct sxml_predicate_outcome {
    bool selects;
} sxml_predicate_outcome;

static const sxml_static_error sxml_numeric_predicate_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A predicate value starts with a number but is not a sequence of "
        "numbers (XPath 4.0, section 4.4.1)"),
};

/* The predicate truth value of two or more items, the first numeric
   (XPath 4.0, section 4.4.1): every item must be numeric (FORG0006), and
   the item at position is selected when one of them equals position. An
   integer range is decided from its bounds. */
static sxml_status sxml_numeric_predicate_selects(sxml_dynamic_context *context,
                                                  sxml_predicate_test test,
                                                  sxml_predicate_outcome *outcome) {
    outcome->selects = false;
    if (sxml_sequence_get_representation(test.value) ==
        SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE) {
        sxml_item head = {.representation = 0};
        uint64_t item_count = 0;
        sxml_status status = sxml_sequence_get_item(
            test.value, (sxml_item_request){.item_index = 0, .error_record = context->error_record},
            &head);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(test.value, context->error_record, &item_count);
        }
        if (status == SXML_STATUS_OK && head.payload.integer <= 0) {
            const uint64_t below_one = (uint64_t)1 - (uint64_t)head.payload.integer;
            outcome->selects = item_count > below_one &&
                               test.position <= item_count - below_one;
        } else if (status == SXML_STATUS_OK) {
            const uint64_t first_position = (uint64_t)head.payload.integer;
            outcome->selects = test.position >= first_position &&
                               test.position - first_position < item_count;
        }
        return status;
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(test.value);
    for (;;) {
        sxml_iteration_step step;
        sxml_status status =
            sxml_sequence_iterator_next(&iterator, context->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            return status;
        }
        if (!sxml_atomic_item_is_numeric(step.item)) {
            return sxml_error_record_report(context->error_record,
                                            &sxml_numeric_predicate_error);
        }
        if (outcome->selects) {
            continue;
        }
        if (sxml_item_get_representation(step.item) == SXML_ITEM_REPRESENTATION_INTEGER) {
            outcome->selects = step.item.payload.integer >= 0 &&
                               (uint64_t)step.item.payload.integer == test.position;
            continue;
        }
        sxml_sequence equal = sxml_sequence_empty();
        status = sxml_operator_make_value_comparison(
            context->item_heap,
            &(sxml_comparison_request){
                .comparison_operator = SXML_COMPARISON_OPERATOR_EQUAL,
                .left_operand = sxml_sequence_from_item(step.item),
                .right_operand = sxml_sequence_from_item(
                    sxml_atomic_item_from_integer((int64_t)test.position)),
                .implicit_timezone_offset_minutes = 0,
                .namespace_bindings = {.namespace_bindings = NULL,
                                       .namespace_binding_count = 0},
                .error_record = context->error_record},
            &equal);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        outcome->selects = equal.content.payload.integer != 0;
        sxml_sequence_release(equal);
    }
}

static sxml_status sxml_predicate_selects(sxml_dynamic_context *context,
                                          sxml_predicate_test test,
                                          sxml_predicate_outcome *outcome) {
    bool *selects = &outcome->selects;
    const sxml_item first = test.value.content;
    const sxml_sequence_representation representation =
        sxml_sequence_get_representation(test.value);
    if (representation != SXML_SEQUENCE_REPRESENTATION_EMPTY &&
        representation != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        sxml_item head = {.representation = 0};
        sxml_status status = sxml_sequence_get_item(
            test.value, (sxml_item_request){.item_index = 0, .error_record = context->error_record},
            &head);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (sxml_atomic_item_is_numeric(head)) {
            return sxml_numeric_predicate_selects(context, test, outcome);
        }
    }
    if (representation == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_atomic_item_is_numeric(first)) {
        if (sxml_item_get_representation(first) == SXML_ITEM_REPRESENTATION_INTEGER) {
            *selects = first.payload.integer >= 0 &&
                       (uint64_t)first.payload.integer == test.position;
            return SXML_STATUS_OK;
        }
        sxml_sequence equal = sxml_sequence_empty();
        sxml_status status = sxml_operator_make_value_comparison(
            context->item_heap,
            &(sxml_comparison_request){
                .comparison_operator = SXML_COMPARISON_OPERATOR_EQUAL,
                .left_operand = test.value,
                .right_operand = sxml_sequence_from_item(
                    sxml_atomic_item_from_integer((int64_t)test.position)),
                .implicit_timezone_offset_minutes = 0,
                .namespace_bindings = {.namespace_bindings = NULL,
                                       .namespace_binding_count = 0},
                .error_record = context->error_record},
            &equal);
        if (status == SXML_STATUS_OK) {
            *selects = equal.content.payload.integer != 0;
            sxml_sequence_release(equal);
        }
        return status;
    }
    sxml_effective_boolean_value effective_boolean_value;
    sxml_status status = sxml_evaluator_compute_effective_boolean_value(
        test.value, context->error_record, &effective_boolean_value);
    if (status == SXML_STATUS_OK) {
        *selects = effective_boolean_value.is_true;
    }
    return status;
}

/* A FILTER whose predicate is a constant: [2] picks one item without
   looking at the others. */
typedef struct sxml_constant_filter {
    bool is_handled;
} sxml_constant_filter;

static sxml_status sxml_filter_by_constant(sxml_dynamic_context *context,
                                           sxml_sequence predicate_value,
                                           sxml_constant_filter *filter) {
    filter->is_handled = true;
    sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_sequence input = task->details.loop.input;
    const sxml_item first = predicate_value.content;
    if (sxml_sequence_get_representation(predicate_value) ==
            SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_item_get_representation(first) == SXML_ITEM_REPRESENTATION_INTEGER) {
        sxml_sequence value = sxml_sequence_empty();
        if (first.payload.integer >= 1) {
            sxml_item item = {.representation = 0};
            sxml_status status = sxml_sequence_get_item(
                input,
                (sxml_item_request){.item_index = (uint64_t)first.payload.integer - 1,
                                    .error_record = context->error_record},
                &item);
            if (status == SXML_STATUS_OK) {
                sxml_item_retain(item);
                value = sxml_sequence_from_item(item);
            } else if (status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
                return status;
            }
        }
        return sxml_machine_finish(context, value);
    }
    if (sxml_sequence_get_representation(predicate_value) ==
            SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_atomic_item_is_numeric(first)) {
        /* Another number: compared with every position by the loop. */
        filter->is_handled = false;
        return SXML_STATUS_OK;
    }
    sxml_effective_boolean_value effective_boolean_value;
    sxml_status status = sxml_evaluator_compute_effective_boolean_value(
        predicate_value, context->error_record, &effective_boolean_value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence value = sxml_sequence_empty();
    if (effective_boolean_value.is_true) {
        sxml_sequence_retain(input);
        value = input;
    }
    return sxml_machine_finish(context, value);
}

/* FILTER (XPath 4.0, section 4.4.1). */
static sxml_status sxml_machine_step_filter(sxml_dynamic_context *context,
                                            const sxml_ir_filter *filter) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    sxml_status status = SXML_STATUS_OK;
    if (task->phase == 0) {
        const sxml_ir_expression *predicate =
            sxml_ir_unit_get_expression(task->function->unit, filter->predicate);
        const bool is_constant =
            predicate->expression_kind == SXML_IR_EXPRESSION_KIND_ATOM &&
            predicate->atom.atom_kind == SXML_IR_ATOM_KIND_CONSTANT;
        status = sxml_loop_start(
            context, (sxml_loop_input){.input = filter->input,
                                       .needs_size = filter->focus.context_size.value != 0});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (is_constant) {
            sxml_constant_filter constant_filter = {.is_handled = false};
            status = sxml_filter_by_constant(
                context,
                sxml_ir_unit_get_constant(task->function->unit, predicate->atom.constant),
                &constant_filter);
            if (status != SXML_STATUS_OK || constant_filter.is_handled) {
                return status;
            }
        }
    } else {
        const sxml_sequence value = sxml_machine_take_value(context);
        sxml_predicate_outcome outcome = {.selects = false};
        status = sxml_predicate_selects(
            context,
            (sxml_predicate_test){.value = value, .position = task->details.loop.position},
            &outcome);
        sxml_sequence_release(value);
        if (status == SXML_STATUS_OK && outcome.selects) {
            sxml_item_retain(task->details.loop.current_item);
            status = sxml_loop_append(context, &task->details.loop,
                                      sxml_sequence_from_item(task->details.loop.current_item));
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_loop_progress progress = {.is_done = false};
    status = sxml_loop_advance_focus(
        context, (sxml_loop_advance){.focus = filter->focus, .body = filter->predicate},
        &progress);
    if (status == SXML_STATUS_OK && progress.is_done) {
        status = sxml_loop_finish(context);
    }
    return status;
}

/* QUANTIFIED (XPath 4.0, section 4.15): stops at the first item that
   decides. */
static sxml_status sxml_machine_step_quantified(sxml_dynamic_context *context,
                                                const sxml_ir_quantified *quantified) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    const bool is_some = quantified->quantifier == SXML_IR_QUANTIFIER_SOME;
    sxml_status status = SXML_STATUS_OK;
    if (task->phase == 0) {
        status = sxml_loop_start(
            context, (sxml_loop_input){.input = quantified->input, .needs_size = false});
    } else {
        const sxml_sequence value = sxml_machine_take_value(context);
        sxml_effective_boolean_value effective_boolean_value = {.is_true = false};
        status = sxml_evaluator_compute_effective_boolean_value(
            value, context->error_record, &effective_boolean_value);
        sxml_sequence_release(value);
        if (status == SXML_STATUS_OK && effective_boolean_value.is_true == is_some) {
            return sxml_machine_finish(
                context, sxml_sequence_from_item(is_some ? sxml_atomic_item_true()
                                                         : sxml_atomic_item_false()));
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    task = sxml_machine_top(context);
    sxml_loop_state *loop = &task->details.loop;
    sxml_iteration_step step;
    status = sxml_sequence_iterator_next(&loop->iterator, context->error_record, &step);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!step.has_item) {
        return sxml_machine_finish(
            context, sxml_sequence_from_item(is_some ? sxml_atomic_item_false()
                                                     : sxml_atomic_item_true()));
    }
    sxml_item_retain(step.item);
    sxml_machine_bind(context,
                      (sxml_slot_binding){.frame_base = task->frame_base,
                                          .slot = quantified->variable},
                      sxml_sequence_from_item(step.item));
    const sxml_evaluator_task condition =
        sxml_expression_task(task->function, task->frame_base, quantified->condition);
    return sxml_machine_push(context, &condition);
}

/* How one sort key compares, resolved once per SORT. */
typedef struct sxml_sort_key_rule {
    sxml_collation collation;
    bool is_descending;
    bool empty_is_least;
} sxml_sort_key_rule;

/* What the sort comparison needs. */
typedef struct sxml_sort_comparison {
    sxml_dynamic_context *context;
    const sxml_sort_state *sort_state;
    const sxml_sort_key_rule *key_rules;
    int16_t implicit_timezone_offset_minutes;
} sxml_sort_comparison;

/* The values of one sort key for two items. */
typedef struct sxml_sort_key_values {
    sxml_sequence first;
    sxml_sequence second;
    const sxml_sort_key_rule *rule;
} sxml_sort_key_values;

/* Orders two values of one sort key in ascending order: the empty
   sequence least or greatest, then item by item as fn:compare does under
   the key's collation, a shorter sequence before a longer one it starts
   (XQuery 4.0, 4.13.9: NaN below every other number; XSLT 4.0, 13.1.2:
   composite sort keys). */
static sxml_status sxml_compare_sort_key_values(const sxml_sort_comparison *comparison,
                                                sxml_sort_key_values values,
                                                sxml_ordering *ordering) {
    sxml_dynamic_context *context = comparison->context;
    sxml_sequence_iterator first_iterator = sxml_sequence_iterator_start(values.first);
    sxml_sequence_iterator second_iterator = sxml_sequence_iterator_start(values.second);
    for (bool is_first_item = true;; is_first_item = false) {
        sxml_iteration_step first_step;
        sxml_iteration_step second_step;
        sxml_status status =
            sxml_sequence_iterator_next(&first_iterator, context->error_record, &first_step);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&second_iterator, context->error_record,
                                                 &second_step);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!first_step.has_item || !second_step.has_item) {
            if (first_step.has_item == second_step.has_item) {
                *ordering = SXML_ORDERING_EQUAL;
            } else {
                /* An empty key is least or greatest; a sequence that ends
                   first is less. */
                const bool first_is_less =
                    is_first_item ? (!first_step.has_item) == values.rule->empty_is_least
                                  : !first_step.has_item;
                *ordering = first_is_less ? SXML_ORDERING_LESS : SXML_ORDERING_GREATER;
            }
            return SXML_STATUS_OK;
        }
        status = sxml_function_library_compare_collated(
            context->item_heap,
            &(sxml_collated_comparison){
                .collation = &values.rule->collation,
                .comparison = {.first = first_step.item,
                               .second = second_step.item,
                               .implicit_timezone_offset_minutes =
                                   comparison->implicit_timezone_offset_minutes,
                               .error_record = context->error_record,
                               .name_pool = sxml_item_heap_get_name_pool(context->item_heap)}},
            ordering);
        if (status != SXML_STATUS_OK || *ordering != SXML_ORDERING_EQUAL) {
            return status;
        }
    }
}

/* Orders two items by their keys, major to minor; a descending key
   reverses its whole order, empty keys included (XQuery 4.0, 4.13.9). */
static sxml_status sxml_compare_sort_keys(void *state, sxml_index_pair pair,
                                          sxml_ordering *ordering) {
    const sxml_sort_comparison *comparison = state;
    const sxml_sort_state *sort_state = comparison->sort_state;
    const size_t key_count = sort_state->key_count;
    *ordering = SXML_ORDERING_EQUAL;
    for (size_t key_index = 0; key_index < key_count; key_index += 1) {
        const sxml_sort_key_rule *rule = &comparison->key_rules[key_index];
        sxml_ordering key_ordering = SXML_ORDERING_EQUAL;
        const sxml_status status = sxml_compare_sort_key_values(
            comparison,
            (sxml_sort_key_values){
                .first = sort_state->key_values[pair.first_index * key_count + key_index],
                .second = sort_state->key_values[pair.second_index * key_count + key_index],
                .rule = rule},
            &key_ordering);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (key_ordering != SXML_ORDERING_EQUAL) {
            if (rule->is_descending) {
                key_ordering = key_ordering == SXML_ORDERING_LESS ? SXML_ORDERING_GREATER
                                                                  : SXML_ORDERING_LESS;
            }
            *ordering = key_ordering;
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_OK;
}

/* The string an optional xs:string atom of a sort key gives; false for
   NONE. */
typedef struct sxml_sort_key_text {
    bool is_present;
    sxml_byte_view utf8;
} sxml_sort_key_text;

static sxml_status sxml_read_sort_key_text(sxml_dynamic_context *context, sxml_ir_atom atom,
                                           sxml_sort_key_text *text) {
    *text = (sxml_sort_key_text){.is_present = false, .utf8 = {.bytes = NULL, .byte_count = 0}};
    if (atom.atom_kind == SXML_IR_ATOM_KIND_NONE) {
        return SXML_STATUS_OK;
    }
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_machine_read_atom(context, atom, &value);
    if (status != SXML_STATUS_OK ||
        sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(value.content, &datum);
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_STRING) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        /* The string stays with the atom's value, which the frame or the
           unit holds while the SORT runs. */
        *text = (sxml_sort_key_text){.is_present = true,
                                     .utf8 = datum.datum_details.string.utf8};
    }
    return status;
}

/* Resolves each key's collation (D116: F&O 4.0, 5.2.7, through the one
   resolver; FOCH0002 for a URI sxml does not support) and direction. */
static sxml_status sxml_resolve_sort_key_rules(sxml_dynamic_context *context,
                                               sxml_ir_sort_key_view sort_keys,
                                               sxml_sort_key_rule *key_rules) {
    const sxml_function_call call = {.dynamic_context = context,
                                     .item_heap = context->item_heap,
                                     .error_record = context->error_record};
    sxml_status status = SXML_STATUS_OK;
    for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < sort_keys.sort_key_count;
         key_index += 1) {
        const sxml_ir_sort_key *sort_key = &sort_keys.sort_keys[key_index];
        sxml_sort_key_rule *rule = &key_rules[key_index];
        *rule = (sxml_sort_key_rule){
            .collation = sxml_collation_codepoint(),
            .is_descending = sort_key->direction == SXML_IR_SORT_DIRECTION_DESCENDING,
            .empty_is_least = sort_key->empty_order == SXML_IR_EMPTY_ORDER_LEAST};
        sxml_sort_key_text text;
        status = sxml_read_sort_key_text(context, sort_key->collation, &text);
        if (status == SXML_STATUS_OK && text.is_present) {
            status = sxml_function_library_find_collation(&call, text.utf8, &rule->collation);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_read_sort_key_text(context, sort_key->order, &text);
        }
        if (status == SXML_STATUS_OK && text.is_present) {
            rule->is_descending =
                sxml_byte_view_equals(text.utf8, SXML_BYTE_VIEW_FROM_LITERAL("descending"));
        }
    }
    return status;
}

/* Sorts the items of the SORT on top by their computed keys and finishes
   with them. */
static sxml_status sxml_sort_finish(sxml_dynamic_context *context,
                                    const sxml_ir_sort *sort) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_sort_state *sort_state = task->details.loop.sort_state;
    const size_t item_count = sort_state->item_count;
    const sxml_ir_sort_key_view sort_keys =
        sxml_ir_unit_get_sort_keys(task->function->unit, sort->keys);
    if (sort_keys.sort_key_count == 0) {
        return sxml_machine_finish(context, sxml_sequence_empty());
    }
    /* The key rules, then the positions (at least one). */
    size_t rules_byte_count = 0;
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(sort_keys.sort_key_count,
                                            sizeof(sxml_sort_key_rule), &rules_byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(item_count == 0 ? 1 : item_count, sizeof(size_t),
                                    &byte_count);
    }
    void *rules_memory = NULL;
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count = rules_byte_count,
                                      .alignment = alignof(sxml_sort_key_rule)},
            &rules_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(size_t)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        if (rules_memory != NULL) {
            sxml_allocator_release(context->allocator, rules_memory);
        }
        return status;
    }
    sxml_sort_key_rule *key_rules = rules_memory;
    size_t *indices = memory;
    for (size_t item_index = 0; item_index < item_count; item_index += 1) {
        indices[item_index] = item_index;
    }
    /* The collations are checked even for an empty input (XSLT 4.0,
       13.1.3 permits it). */
    status = sxml_resolve_sort_key_rules(context, sort_keys, key_rules);
    if (status == SXML_STATUS_OK && item_count == 0) {
        sxml_allocator_release(context->allocator, memory);
        sxml_allocator_release(context->allocator, rules_memory);
        return sxml_machine_finish(context, sxml_sequence_empty());
    }
    int16_t timezone = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_read_implicit_timezone(context, &timezone);
    }
    sxml_sort_comparison comparison = {.context = context,
                                       .sort_state = sort_state,
                                       .key_rules = key_rules,
                                       .implicit_timezone_offset_minutes = timezone};
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_sort_indices(
            context->allocator,
            &(sxml_index_sort_request){.indices = indices,
                                       .index_count = item_count,
                                       .compare = sxml_compare_sort_keys,
                                       .compare_state = &comparison});
    }
    sxml_item *sorted_items = NULL;
    void *sorted_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count = item_count * sizeof(sxml_item),
                                      .alignment = alignof(sxml_item)},
            &sorted_memory);
        sorted_items = sorted_memory;
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        for (size_t position = 0; position < item_count; position += 1) {
            sorted_items[position] = sort_state->items[indices[position]];
        }
        status = sxml_sequence_make_vector(
            context->item_heap,
            (sxml_item_view){.items = sorted_items, .item_count = item_count}, &value);
    }
    if (sorted_memory != NULL) {
        sxml_allocator_release(context->allocator, sorted_memory);
    }
    sxml_allocator_release(context->allocator, memory);
    sxml_allocator_release(context->allocator, rules_memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_machine_finish(context, value);
}

/* Allocates the sort state of the SORT on top over its input's items. */
static sxml_status sxml_sort_start(sxml_dynamic_context *context, size_t key_count) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    sxml_loop_state *loop = &task->details.loop;
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(loop->input, context->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t value_count = 0;
    size_t values_offset = 0;
    size_t byte_count = 0;
    if (item_count > SIZE_MAX) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    status = sxml_size_multiply((size_t)item_count, key_count, &value_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply((size_t)item_count, sizeof(sxml_item), &values_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(values_offset, sizeof(sxml_sort_state), &values_offset);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(value_count, sizeof(sxml_sequence), &byte_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(byte_count, values_offset, &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_sort_state)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sort_state *sort_state = memory;
    void *items_place = (uint8_t *)memory + sizeof(sxml_sort_state);
    void *values_place = (uint8_t *)memory + values_offset;
    sort_state->items = items_place;
    sort_state->item_count = (size_t)item_count;
    sort_state->key_count = key_count;
    sort_state->key_values = values_place;
    sort_state->next_value_index = 0;
    loop->sort_state = sort_state;
    loop->size = item_count;
    for (size_t item_index = 0; status == SXML_STATUS_OK && item_index < item_count;
         item_index += 1) {
        status = sxml_sequence_get_item(
            loop->input,
            (sxml_item_request){.item_index = item_index,
                                .error_record = context->error_record},
            &sort_state->items[item_index]);
    }
    return status;
}

/* SORT: computes every key of every item with the item as focus, then
   sorts stably (XQuery 4.0 "order by", xsl:sort). */
static sxml_status sxml_machine_step_sort(sxml_dynamic_context *context,
                                          const sxml_ir_sort *sort) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    const size_t key_count = sort->keys.sort_key_count;
    sxml_status status = SXML_STATUS_OK;
    if (task->phase == 0) {
        status = sxml_loop_start(
            context, (sxml_loop_input){.input = sort->input, .needs_size = false});
        if (status == SXML_STATUS_OK) {
            status = sxml_sort_start(context, key_count);
        }
    } else {
        const sxml_sequence value = sxml_machine_take_value(context);
        sxml_sequence atomized = sxml_sequence_empty();
        status = sxml_machine_atomize(context, value, &atomized);
        sxml_sequence_release(value);
        if (status == SXML_STATUS_OK) {
            uint64_t item_count = 0;
            status = sxml_sequence_count(atomized, context->error_record, &item_count);
            const size_t key_index = task->details.loop.sort_state->next_value_index %
                                     key_count;
            const sxml_ir_sort_key_view sort_keys =
                sxml_ir_unit_get_sort_keys(task->function->unit, sort->keys);
            if (status == SXML_STATUS_OK && item_count > 1 &&
                (key_index >= sort_keys.sort_key_count ||
                 !sort_keys.sort_keys[key_index].is_composite)) {
                status = sxml_error_record_report(context->error_record,
                                                  &sxml_sort_key_error);
            }
            if (status != SXML_STATUS_OK) {
                sxml_sequence_release(atomized);
            }
        }
        if (status == SXML_STATUS_OK) {
            sxml_sort_state *sort_state = task->details.loop.sort_state;
            sort_state->key_values[sort_state->next_value_index] = atomized;
            sort_state->next_value_index += 1;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    task = sxml_machine_top(context);
    const sxml_sort_state *sort_state = task->details.loop.sort_state;
    if (key_count == 0 ||
        sort_state->next_value_index == sort_state->item_count * key_count) {
        return sxml_sort_finish(context, sort);
    }
    const size_t item_index = sort_state->next_value_index / key_count;
    const size_t key_index = sort_state->next_value_index % key_count;
    const sxml_ir_sort_key_view sort_keys =
        sxml_ir_unit_get_sort_keys(task->function->unit, sort->keys);
    if (sort_keys.sort_keys == NULL || key_index >= sort_keys.sort_key_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_machine_bind_focus(
        context, &(sxml_focus_binding){.frame_base = task->frame_base,
                                       .focus = sort->focus,
                                       .value = sxml_sequence_from_item(
                                           sort_state->items[item_index]),
                                       .position = item_index + 1,
                                       .size = sort_state->item_count});
    task->phase = 1;
    const sxml_evaluator_task key = sxml_expression_task(
        task->function, task->frame_base, sort_keys.sort_keys[key_index].key);
    return sxml_machine_push(context, &key);
}

/* ------------------------------------------------------- Expressions */

/* Borrowed values of a list of atoms; a few on the C stack, more in
   allocator memory. */
#define SXML_EVALUATOR_INLINE_ATOM_COUNT 8U

typedef struct sxml_atom_values {
    sxml_sequence inline_values[SXML_EVALUATOR_INLINE_ATOM_COUNT];
    sxml_sequence *values;
    size_t value_count;
    bool is_allocated;
} sxml_atom_values;

static sxml_status sxml_atom_values_read(sxml_dynamic_context *context,
    sxml_ir_atom_list list,
                                         sxml_atom_values *values) {
    const sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_ir_atom_view atoms = sxml_ir_unit_get_atoms(task->function->unit, list);
    values->values = values->inline_values;
    values->value_count = 0;
    values->is_allocated = false;
    if (atoms.atom_count > SXML_EVALUATOR_INLINE_ATOM_COUNT) {
        void *memory = NULL;
        sxml_status status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count =
                                          atoms.atom_count * sizeof(sxml_sequence),
                                      .alignment = alignof(sxml_sequence)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        values->values = memory;
        values->is_allocated = true;
    }
    for (size_t atom_index = 0; atom_index < atoms.atom_count; atom_index += 1) {
        sxml_status status = sxml_machine_read_atom(context, atoms.atoms[atom_index],
                                                    &values->values[atom_index]);
        if (status != SXML_STATUS_OK) {
            if (values->is_allocated) {
                sxml_allocator_release(context->allocator, values->values);
            }
            values->is_allocated = false;
            return status;
        }
    }
    values->value_count = atoms.atom_count;
    return SXML_STATUS_OK;
}

static void sxml_atom_values_finalize(sxml_dynamic_context *context,
                                      sxml_atom_values *values) {
    if (values->is_allocated) {
        sxml_allocator_release(context->allocator, values->values);
    }
    values->is_allocated = false;
}

/* Makes the task on top a CALL (it was an EXPRESSION; its loop fields are
   unused). Takes over function_item and arguments. */
static void sxml_machine_become_call(sxml_dynamic_context *context,
                                     const sxml_call_state *call) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    task->task_kind = SXML_EVALUATOR_TASK_KIND_CALL;
    task->phase = 0;
    task->details.call = *call;
}

/* STATIC_CALL (XPath 4.0, section 4.5.1): arguments coerced by lowering. */
static sxml_status sxml_start_static_call(sxml_dynamic_context *context,
                                          const sxml_ir_static_call *static_call) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_ir_function_target target = static_call->target;
    sxml_status status = SXML_STATUS_OK;
    switch (target.target_kind) {
    case SXML_IR_FUNCTION_TARGET_KIND_NATIVE: {
        const sxml_native_function *row = target.native_function;
        if (row->call != NULL) {
            /* A leaf: the arguments are borrowed from the frame. */
            sxml_atom_values values;
            status = sxml_atom_values_read(context, static_call->arguments, &values);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            const sxml_function_call call = {
                .function_item = {.representation = 0},
                .arguments = {.sequences = values.values,
                              .sequence_count = values.value_count},
                .dynamic_context = context,
                .item_heap = context->item_heap,
                .error_record = context->error_record};
            sxml_sequence result = sxml_sequence_empty();
            status = row->call(&call, &result);
            sxml_atom_values_finalize(context, &values);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            return sxml_machine_finish(context, result);
        }
        if (row->resumable_body == NULL) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_sequence *arguments = NULL;
        status = sxml_machine_copy_atoms(context, static_call->arguments, &arguments);
        if (status == SXML_STATUS_OK) {
            sxml_machine_become_call(
                context, &(sxml_call_state){.function_item = {.representation = 0},
                                            .callee = NULL,
                                            .native_function = row,
                                            .arguments = arguments,
                                            .argument_count =
                                                static_call->arguments.atom_count,
                                            .native_call = NULL});
        }
        return status;
    }
    case SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION: {
        sxml_sequence *arguments = NULL;
        status = sxml_machine_copy_atoms(context, static_call->arguments, &arguments);
        if (status == SXML_STATUS_OK) {
            sxml_machine_become_call(
                context,
                &(sxml_call_state){
                    .function_item = {.representation = 0},
                    .callee = sxml_ir_unit_get_function(task->function->unit,
                                                        target.function),
                    .native_function = NULL,
                    .arguments = arguments,
                    .argument_count = static_call->arguments.atom_count,
                    .native_call = NULL});
        }
        return status;
    }
    case SXML_IR_FUNCTION_TARGET_KIND_CONSTRUCTOR: {
        sxml_atom_values values;
        status = sxml_atom_values_read(context, static_call->arguments, &values);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_sequence atomized = sxml_sequence_empty();
        sxml_sequence result = sxml_sequence_empty();
        if (values.value_count == 1) {
            status = sxml_machine_atomize(context, values.values[0], &atomized);
        } else {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_atom_values_finalize(context, &values);
        if (status == SXML_STATUS_OK) {
            status = sxml_cast_make_value(
                context->item_heap,
                &(sxml_cast_request){.value = atomized,
                                     .target_type = target.constructor_type,
                                     .allows_empty = true,
                                     .namespace_bindings = sxml_ir_unit_get_static_namespaces(
                                         task->function->unit),
                                     .error_record = context->error_record},
                &result);
            sxml_sequence_release(atomized);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_machine_finish(context, result);
    }
    case SXML_IR_FUNCTION_TARGET_KIND_NONE:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/*
 * The target of a dynamic call coerced to function(*)* (XPath 4.0, section
 * 4.6.3.1): function items (maps and arrays among them) stay, a JNode
 * gives the items of its jvalue (section 3.4.2), anything else is
 * XPTY0004. Writes the functions, owned, to *functions.
 */
static sxml_status sxml_coerce_call_target(sxml_dynamic_context *context,
                                           sxml_sequence value,
                                           sxml_sequence *functions) {
    sxml_sequence_builder *builder = NULL;
    sxml_status status = SXML_STATUS_OK;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    uint64_t item_index = 0;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, context->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_sequence items = sxml_sequence_from_item(step.item);
        if (sxml_item_get_kind(step.item) == SXML_ITEM_KIND_JSON_NODE) {
            sxml_json_node_description description;
            status = sxml_json_node_describe(step.item, &description);
            items = description.json_value;
        }
        sxml_sequence_iterator inner = sxml_sequence_iterator_start(items);
        while (status == SXML_STATUS_OK) {
            sxml_iteration_step inner_step;
            status = sxml_sequence_iterator_next(&inner, context->error_record,
                                                 &inner_step);
            if (status != SXML_STATUS_OK || !inner_step.has_item) {
                break;
            }
            if (!sxml_item_is_function_item(inner_step.item)) {
                status = sxml_error_record_report(context->error_record,
                                                  &sxml_single_function_error);
            }
        }
        if (status == SXML_STATUS_OK && builder == NULL &&
            sxml_item_get_kind(step.item) == SXML_ITEM_KIND_JSON_NODE) {
            /* The first JNode: copy the function items before it. */
            status = sxml_sequence_builder_create(
                context->allocator,
                &(sxml_sequence_builder_configuration){.item_heap = context->item_heap},
                &builder);
            for (uint64_t earlier_index = 0;
                 status == SXML_STATUS_OK && earlier_index < item_index;
                 earlier_index += 1) {
                sxml_item earlier = {.representation = 0};
                status = sxml_sequence_get_item(
                    value,
                    (sxml_item_request){.item_index = earlier_index,
                                        .error_record = context->error_record},
                    &earlier);
                if (status == SXML_STATUS_OK) {
                    status = sxml_sequence_builder_append_item(builder, earlier);
                }
            }
        }
        if (status == SXML_STATUS_OK && builder != NULL) {
            status = sxml_sequence_builder_append_sequence(builder, items,
                                                           context->error_record);
        }
        item_index += 1;
    }
    if (status == SXML_STATUS_OK && builder != NULL) {
        status = sxml_sequence_builder_finish(builder, functions);
    } else if (status == SXML_STATUS_OK) {
        sxml_sequence_retain(value);
        *functions = value;
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/*
 * A call of a sequence of function items (XPath 4.0, section 4.6.3.1:
 * F(X, Y) is "for $FI in F return $FI(X, Y)"): a resumable body of the
 * evaluator's own, not a library row. Argument 0 holds the function items,
 * the others the arguments; each step calls the next function item, as
 * any dynamic call, and the results are concatenated in order.
 */
typedef struct sxml_call_each_state {
    uint64_t next_index;
    /* Owned. */
    sxml_sequence_builder *builder;
} sxml_call_each_state;

static sxml_status sxml_step_call_each(const sxml_native_step_request *request,
                                       sxml_native_step *step) {
    sxml_call_each_state *state = request->state;
    const sxml_function_call *call = request->call;
    sxml_status status = SXML_STATUS_OK;
    if (request->step_number == 0) {
        status = sxml_sequence_builder_create(
            call->dynamic_context->allocator,
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap},
            &state->builder);
    } else {
        status = sxml_sequence_builder_append_sequence(state->builder,
                                                       request->call_result,
                                                       call->error_record);
        sxml_sequence_release(request->call_result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence functions = call->arguments.sequences[0];
    sxml_item function_item = {.representation = 0};
    status = sxml_sequence_get_item(
        functions,
        (sxml_item_request){.item_index = state->next_index,
                            .error_record = call->error_record},
        &function_item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        sxml_sequence result = sxml_sequence_empty();
        status = sxml_sequence_builder_finish(state->builder, &result);
        if (status == SXML_STATUS_OK) {
            *step = (sxml_native_step){.step_kind = SXML_NATIVE_STEP_KIND_FINISH,
                                       .result = result};
        }
        return status;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->next_index += 1;
    *step = (sxml_native_step){
        .step_kind = SXML_NATIVE_STEP_KIND_CALL,
        .result = sxml_sequence_empty(),
        .function_item = function_item,
        .arguments = {.sequences = call->arguments.sequences + 1,
                      .sequence_count = call->arguments.sequence_count - 1}};
    return SXML_STATUS_OK;
}

static void sxml_release_call_each_state(void *state) {
    sxml_call_each_state *call_each = state;
    sxml_sequence_builder_destroy(call_each->builder);
    call_each->builder = NULL;
}

static const sxml_resumable_native_body sxml_call_each_body = {
    .step = sxml_step_call_each,
    .release_state = sxml_release_call_each_state,
    .state_byte_count = sizeof(sxml_call_each_state),
    .state_alignment = alignof(sxml_call_each_state)};

static const sxml_native_function sxml_call_each_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    .properties = SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("dynamic-call"),
    .parameters = {.function_parameters = NULL, .function_parameter_count = 0},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = NULL,
    .resumable_body = &sxml_call_each_body};

/* DYNAMIC_CALL (XPath 4.0, section 4.6.3.1). */
static sxml_status sxml_start_dynamic_call(sxml_dynamic_context *context,
                                           const sxml_ir_dynamic_call *dynamic_call) {
    sxml_sequence function_value = sxml_sequence_empty();
    sxml_status status =
        sxml_machine_read_atom(context, dynamic_call->function_item, &function_value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence functions = sxml_sequence_empty();
    status = sxml_coerce_call_target(context, function_value, &functions);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence_representation representation =
        sxml_sequence_get_representation(functions);
    if (representation == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        /* No function: the arguments are not needed. */
        return sxml_machine_finish(context, functions);
    }
    const size_t argument_count = dynamic_call->arguments.atom_count;
    sxml_sequence *arguments = NULL;
    if (representation == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        status = sxml_machine_copy_atoms(context, dynamic_call->arguments, &arguments);
        if (status != SXML_STATUS_OK) {
            sxml_sequence_release(functions);
            return status;
        }
        /* functions owns the one item; the call takes it over. */
        sxml_machine_become_call(
            context, &(sxml_call_state){.function_item = functions.content,
                                        .callee = NULL,
                                        .native_function = NULL,
                                        .arguments = arguments,
                                        .argument_count = argument_count,
                                        .native_call = NULL});
        return SXML_STATUS_OK;
    }
    /* Several function items: the call-each body with the functions as
       its first argument. */
    void *memory = NULL;
    status = sxml_allocator_allocate(
        context->allocator,
        (sxml_allocation_request){.byte_count = (argument_count + 1) * sizeof(sxml_sequence),
                                  .alignment = alignof(sxml_sequence)},
        &memory);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(functions);
        return status;
    }
    arguments = memory;
    arguments[0] = functions;
    size_t copied_count = 1;
    const sxml_ir_atom_view atoms = sxml_ir_unit_get_atoms(
        sxml_machine_top(context)->function->unit, dynamic_call->arguments);
    while (status == SXML_STATUS_OK && copied_count <= atoms.atom_count) {
        status = sxml_machine_copy_atom(context, atoms.atoms[copied_count - 1],
                                        &arguments[copied_count]);
        if (status == SXML_STATUS_OK) {
            copied_count += 1;
        }
    }
    if (status != SXML_STATUS_OK) {
        for (size_t value_index = 0; value_index < copied_count; value_index += 1) {
            sxml_sequence_release(arguments[value_index]);
        }
        sxml_allocator_release(context->allocator, memory);
        return status;
    }
    sxml_machine_become_call(
        context, &(sxml_call_state){.function_item = {.representation = 0},
                                    .callee = NULL,
                                    .native_function = &sxml_call_each_row,
                                    .arguments = arguments,
                                    .argument_count = copied_count,
                                    .native_call = NULL});
    return SXML_STATUS_OK;
}

/* Expressions whose value needs no nested evaluation. */
static sxml_status sxml_make_leaf_value(sxml_dynamic_context *context,
    const sxml_ir_expression *expression,
                                        sxml_sequence *result) {
    const sxml_evaluator_task *task = sxml_machine_top(context);
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = SXML_STATUS_OK;
    switch (expression->expression_kind) {
    case SXML_IR_EXPRESSION_KIND_ATOM:
        return sxml_machine_copy_atom(context, expression->atom, result);
    case SXML_IR_EXPRESSION_KIND_SEQUENCE: {
        sxml_atom_values values;
        status = sxml_atom_values_read(context, expression->sequence, &values);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_concatenate(
                context->item_heap,
                (sxml_sequence_view){.sequences = values.values,
                                     .sequence_count = values.value_count},
                result);
            sxml_atom_values_finalize(context, &values);
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_BINARY: {
        sxml_binary_operands binary = {
            .binary_operator = expression->binary.operator,
            .raw_operands = {sxml_sequence_empty(), sxml_sequence_empty()}};
        status = sxml_machine_read_atom(context, expression->binary.left_operand,
                                        &binary.raw_operands[0]);
        if (status == SXML_STATUS_OK) {
            status = sxml_machine_read_atom(context, expression->binary.right_operand,
                                            &binary.raw_operands[1]);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_make_binary_value(context, &binary, result);
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_UNARY: {
        status = sxml_machine_read_atom(context, expression->unary.operand, &value);
        sxml_sequence atomized = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            status = sxml_machine_atomize(context, value, &atomized);
        }
        if (status == SXML_STATUS_OK) {
            const sxml_unary_arithmetic_request request = {
                .operand = atomized, .error_record = context->error_record};
            status = expression->unary.operator == SXML_IR_UNARY_OPERATOR_MINUS
                         ? sxml_operator_make_unary_minus_value(context->item_heap,
                                                                &request, result)
                         : sxml_operator_make_unary_plus_value(context->item_heap,
                                                               &request, result);
            sxml_sequence_release(atomized);
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION: {
        const sxml_ir_function *function = sxml_ir_unit_get_function(
            task->function->unit, expression->function_definition.function);
        sxml_atom_values values;
        status = sxml_atom_values_read(context, function->captured_variables, &values);
        sxml_item item = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_make_function_definition(
                context,
                &(sxml_function_definition_request){
                    .function = function,
                    .captured_values = {.sequences = values.values,
                                        .sequence_count = values.value_count}},
                &item);
            sxml_atom_values_finalize(context, &values);
        }
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(item);
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE: {
        sxml_item item = {.representation = 0};
        status = sxml_evaluator_make_function_reference(
            context,
            &(sxml_function_reference_request){
                .unit = task->function->unit,
                .function_reference = &expression->function_reference},
            &item);
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(item);
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_LOOKUP:
        return sxml_make_lookup_value(context, &expression->lookup, result);
    case SXML_IR_EXPRESSION_KIND_STEP:
        status = sxml_machine_read_atom(context, expression->step.origin, &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_make_step_value(
                context,
                &(sxml_step_request){.origin = value,
                                     .axis = expression->step.axis,
                                     .node_test = expression->step.node_test,
                                     .error_record = context->error_record},
                result);
        }
        return status;
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER:
        status = sxml_machine_read_atom(context, expression->document_order.input,
                                        &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_make_document_order_value(
                context,
                &(sxml_document_order_request){.input = value,
                                               .error_record = context->error_record},
                result);
        }
        return status;
    case SXML_IR_EXPRESSION_KIND_CAST:
    case SXML_IR_EXPRESSION_KIND_CASTABLE:
        return sxml_make_cast_value(context, expression, result);
    case SXML_IR_EXPRESSION_KIND_INSTANCE_OF:
    case SXML_IR_EXPRESSION_KIND_TREAT: {
        status = sxml_machine_read_atom(context, expression->type_test.value, &value);
        sxml_type_match match = {.is_match = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_match_sequence_type(
                context,
                &(sxml_type_match_request){
                    .value = value,
                    .sequence_type = expression->type_test.sequence_type,
                    .error_record = context->error_record},
                &match);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_INSTANCE_OF) {
            *result = sxml_sequence_from_item(match.is_match ? sxml_atomic_item_true()
                                                             : sxml_atomic_item_false());
            return SXML_STATUS_OK;
        }
        if (!match.is_match) {
            return sxml_error_record_report(context->error_record, &sxml_treat_error);
        }
        sxml_sequence_retain(value);
        *result = value;
        return SXML_STATUS_OK;
    }
    case SXML_IR_EXPRESSION_KIND_COERCE:
        status = sxml_machine_read_atom(context, expression->coercion.value, &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_coerce(
                context,
                &(sxml_coercion_request){.value = value,
                                         .sequence_type = expression->coercion.sequence_type,
                                         .role = expression->coercion.role,
                                         .error_record = context->error_record},
                result);
        }
        return status;
    case SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR:
        return sxml_make_map_value(context, &expression->map_constructor, result);
    case SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR:
        return sxml_make_array_value(context, &expression->array_constructor,
                                     result);
    case SXML_IR_EXPRESSION_KIND_RAISE:
        return sxml_raise_error(context, &expression->raise);
    case SXML_IR_EXPRESSION_KIND_NONE:
    case SXML_IR_EXPRESSION_KIND_LET:
    case SXML_IR_EXPRESSION_KIND_CONDITIONAL:
    case SXML_IR_EXPRESSION_KIND_STATIC_CALL:
    case SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL:
    case SXML_IR_EXPRESSION_KIND_MAP:
    case SXML_IR_EXPRESSION_KIND_FILTER:
    case SXML_IR_EXPRESSION_KIND_QUANTIFIED:
    case SXML_IR_EXPRESSION_KIND_SORT:
    case SXML_IR_EXPRESSION_KIND_TRY_CATCH:
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_SHALLOW_COPY:
    case SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES:
    case SXML_IR_EXPRESSION_KIND_COUNT:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Turns the EXPRESSION task on top into the PART task of the evaluator
   part that claims kind (D101), which steps from the next turn of the
   loop; SXML_STATUS_UNSUPPORTED_FEATURE when no part (or more than one)
   claims it. */
static sxml_status sxml_machine_start_part_kind(sxml_dynamic_context *context,
                                                sxml_ir_expression_kind kind) {
    const sxml_evaluator_part_step_function step = sxml_evaluator_find_kind_handler(kind);
    if (step == NULL) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    sxml_evaluator_task *task = sxml_machine_top(context);
    task->task_kind = SXML_EVALUATOR_TASK_KIND_PART;
    task->phase = 0;
    task->details.part = (sxml_evaluator_part_task_state){
        .step = step, .release_state = NULL, .state = NULL};
    return SXML_STATUS_OK;
}

/* Runs the EXPRESSION task on top for one step. */
static sxml_status sxml_machine_step_expression(sxml_dynamic_context *context) {
    sxml_evaluator_task *task = sxml_machine_top(context);
    const sxml_ir_expression *expression =
        sxml_ir_unit_get_expression(task->function->unit, task->expression);
    switch (expression->expression_kind) {
    case SXML_IR_EXPRESSION_KIND_LET:
        if (task->phase == 0) {
            task->phase = 1;
            const sxml_evaluator_task bound = sxml_expression_task(
                task->function, task->frame_base, expression->let.bound_expression);
            return sxml_machine_push(context, &bound);
        }
        /* The body replaces the LET: no task waits for it (tail). */
        sxml_machine_bind(context,
                          (sxml_slot_binding){.frame_base = task->frame_base,
                                              .slot = expression->let.variable},
                          sxml_machine_take_value(context));
        task->expression = expression->let.body;
        task->phase = 0;
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_CONDITIONAL: {
        /* XPath 4.0, section 4.14: the effective boolean value chooses;
           the branch replaces the conditional (tail). */
        sxml_sequence condition = sxml_sequence_empty();
        sxml_status status = sxml_machine_read_atom(context, expression->conditional.condition, &condition);
        sxml_effective_boolean_value effective_boolean_value = {.is_true = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_compute_effective_boolean_value(
                condition, context->error_record, &effective_boolean_value);
        }
        if (status == SXML_STATUS_OK) {
            task->expression = effective_boolean_value.is_true
                                   ? expression->conditional.then_branch
                                   : expression->conditional.else_branch;
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_STATIC_CALL:
        return sxml_start_static_call(context, &expression->static_call);
    case SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL:
        return sxml_start_dynamic_call(context, &expression->dynamic_call);
    case SXML_IR_EXPRESSION_KIND_MAP:
        return sxml_machine_step_map(context, &expression->map);
    case SXML_IR_EXPRESSION_KIND_FILTER:
        return sxml_machine_step_filter(context, &expression->filter);
    case SXML_IR_EXPRESSION_KIND_QUANTIFIED:
        return sxml_machine_step_quantified(context, &expression->quantified);
    case SXML_IR_EXPRESSION_KIND_SORT:
        return sxml_machine_step_sort(context, &expression->sort);
    case SXML_IR_EXPRESSION_KIND_TRY_CATCH:
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_SHALLOW_COPY:
    case SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES:
        /* Evaluated by evaluator parts (XQuery and XSLT, D101). */
        return sxml_machine_start_part_kind(context, expression->expression_kind);
    case SXML_IR_EXPRESSION_KIND_NONE:
    case SXML_IR_EXPRESSION_KIND_ATOM:
    case SXML_IR_EXPRESSION_KIND_SEQUENCE:
    case SXML_IR_EXPRESSION_KIND_BINARY:
    case SXML_IR_EXPRESSION_KIND_UNARY:
    case SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION:
    case SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE:
    case SXML_IR_EXPRESSION_KIND_LOOKUP:
    case SXML_IR_EXPRESSION_KIND_STEP:
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER:
    case SXML_IR_EXPRESSION_KIND_CAST:
    case SXML_IR_EXPRESSION_KIND_CASTABLE:
    case SXML_IR_EXPRESSION_KIND_INSTANCE_OF:
    case SXML_IR_EXPRESSION_KIND_TREAT:
    case SXML_IR_EXPRESSION_KIND_COERCE:
    case SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_RAISE:
    case SXML_IR_EXPRESSION_KIND_COUNT:
        break;
    }
    sxml_sequence value = sxml_sequence_empty();
    const sxml_status status = sxml_make_leaf_value(context, expression, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_machine_finish(context, value);
}

/* Puts the source span of the expression of the task on top into the
   error record, unless the error already has a location. */
static void sxml_machine_locate_error(sxml_dynamic_context *context,
                                      sxml_status status) {
    sxml_error_record *record = context->error_record;
    if (record == NULL || !sxml_status_has_error_record(status) ||
        record->diagnostic.location.line_number != 0 ||
        context->task_stack.task_count == 0) {
        return;
    }
    const sxml_evaluator_task *task = sxml_machine_top(context);
    if (task->function == NULL || task->expression.value == 0) {
        return;
    }
    const sxml_ir_unit *unit = task->function->unit;
    const sxml_ir_source_span span =
        sxml_ir_unit_get_expression(unit, task->expression)->source_span;
    if (span.line_number == 0) {
        return;
    }
    record->diagnostic.location = (sxml_source_location){
        .source_name = unit->source_name,
        .line_number = span.line_number,
        .column_number = span.column_number,
        .byte_offset = span.byte_offset};
}

/* An error raised by the top task of a machine loop with base base_count. */
typedef struct sxml_caught_error {
    size_t base_count;
    sxml_status status;
} sxml_caught_error;

/* Hands error to the innermost catching task below the top task and above
   the loop's base (src/evaluator_machine_internal.h, "Catching"; XQuery
   4.0, "Try/Catch Expressions"): releases the tasks above it, clears its mark and
   stores the status. Returns false, changing nothing, when the status is
   not a dynamic or type error or no task catches it. */
static bool sxml_machine_catch(sxml_dynamic_context *context, sxml_caught_error error) {
    if (error.status != SXML_STATUS_DYNAMIC_ERROR &&
        error.status != SXML_STATUS_TYPE_ERROR) {
        return false;
    }
    const size_t task_count = context->task_stack.task_count;
    /* The top task's own failure is never handed to itself. */
    size_t catching_count = task_count > 0 ? task_count - 1 : 0;
    while (catching_count > error.base_count &&
           !context->task_stack.tasks[catching_count - 1].catches_errors) {
        catching_count -= 1;
    }
    if (catching_count <= error.base_count) {
        return false;
    }
    sxml_machine_unwind(context, catching_count);
    sxml_machine_top(context)->catches_errors = false;
    context->caught_status = error.status;
    return true;
}

/* Runs tasks until the stack is back at base_count; the value of the last
   one is left in the context's task value. */
static sxml_status sxml_machine_run(sxml_dynamic_context *context, size_t base_count) {
    while (context->task_stack.task_count > base_count) {
        /* One step per task run (D89), so no evaluation runs unbounded. */
        sxml_status status = sxml_dynamic_context_charge_steps(context, 1);
        if (status != SXML_STATUS_OK) {
            sxml_machine_unwind(context, base_count);
            return status;
        }
        switch (sxml_machine_top(context)->task_kind) {
        case SXML_EVALUATOR_TASK_KIND_EXPRESSION:
            status = sxml_machine_step_expression(context);
            break;
        case SXML_EVALUATOR_TASK_KIND_CALL:
            status = sxml_machine_step_call(context);
            break;
        case SXML_EVALUATOR_TASK_KIND_RETURN:
            status = sxml_machine_finish(context, sxml_machine_take_value(context));
            break;
        case SXML_EVALUATOR_TASK_KIND_NATIVE_STEP:
            status = sxml_machine_step_native(context);
            break;
        case SXML_EVALUATOR_TASK_KIND_COERCE_RESULT:
            status = sxml_machine_step_coerce_result(context);
            break;
        case SXML_EVALUATOR_TASK_KIND_PART: {
            sxml_evaluator_task *task = sxml_machine_top(context);
            status = task->details.part.step(context, task);
            break;
        }
        }
        if (status != SXML_STATUS_OK) {
            sxml_machine_locate_error(context, status);
            if (sxml_machine_catch(context,
                                   (sxml_caught_error){.base_count = base_count,
                                                       .status = status})) {
                continue;
            }
            sxml_machine_unwind(context, base_count);
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Machine API */

sxml_evaluator_part_step_function sxml_evaluator_find_kind_handler(
    sxml_ir_expression_kind kind) {
    const sxml_evaluator_registered_part_view registered = sxml_evaluator_registered_parts();
    sxml_evaluator_part_step_function found = NULL;
    for (size_t part_index = 0; part_index < registered.part_count; part_index += 1) {
        const sxml_evaluator_part *part = registered.parts[part_index].part;
        for (size_t handler_index = 0; handler_index < part->kind_handler_count;
             handler_index += 1) {
            const sxml_evaluator_kind_handler *handler = &part->kind_handlers[handler_index];
            if (handler->expression_kind != kind) {
                continue;
            }
            if (found != NULL || handler->step == NULL) {
                /* Claimed twice: no part evaluates it (D101). */
                return NULL;
            }
            found = handler->step;
        }
    }
    return found;
}

sxml_evaluator_task *sxml_evaluator_machine_top(sxml_dynamic_context *dynamic_context) {
    return sxml_machine_top(dynamic_context);
}

sxml_status sxml_evaluator_machine_push_expression(
    sxml_dynamic_context *dynamic_context, sxml_ir_expression_index expression) {
    const sxml_evaluator_task *top = sxml_machine_top(dynamic_context);
    const sxml_evaluator_task task =
        sxml_expression_task(top->function, top->frame_base, expression);
    return sxml_machine_push(dynamic_context, &task);
}

sxml_status sxml_evaluator_machine_push_part_task(
    sxml_dynamic_context *dynamic_context,
    const sxml_evaluator_part_task_state *part_state) {
    const sxml_evaluator_task *top = sxml_machine_top(dynamic_context);
    sxml_evaluator_task task =
        sxml_expression_task(top->function, top->frame_base, top->expression);
    task.task_kind = SXML_EVALUATOR_TASK_KIND_PART;
    task.details.part = *part_state;
    const sxml_status status = sxml_machine_push(dynamic_context, &task);
    if (status != SXML_STATUS_OK) {
        sxml_machine_release_task(dynamic_context, &task);
    }
    return status;
}

sxml_status sxml_evaluator_machine_finish(sxml_dynamic_context *dynamic_context,
                                          sxml_sequence value) {
    return sxml_machine_finish(dynamic_context, value);
}

sxml_sequence sxml_evaluator_machine_take_value(sxml_dynamic_context *dynamic_context) {
    return sxml_machine_take_value(dynamic_context);
}

sxml_status sxml_evaluator_machine_take_caught_status(
    sxml_dynamic_context *dynamic_context) {
    const sxml_status status = dynamic_context->caught_status;
    dynamic_context->caught_status = SXML_STATUS_OK;
    return status;
}

sxml_status sxml_evaluator_machine_read_atom(sxml_dynamic_context *dynamic_context,
                                             sxml_ir_atom atom, sxml_sequence *value) {
    return sxml_machine_read_atom(dynamic_context, atom, value);
}

void sxml_evaluator_machine_bind_slot(sxml_dynamic_context *dynamic_context,
                                      sxml_ir_slot slot, sxml_sequence value) {
    const size_t frame_base = sxml_machine_top(dynamic_context)->frame_base;
    sxml_machine_bind(dynamic_context,
                      (sxml_slot_binding){.frame_base = frame_base, .slot = slot}, value);
}

sxml_status sxml_evaluator_machine_raise(sxml_dynamic_context *dynamic_context,
                                         const sxml_static_error *error) {
    return sxml_error_record_report(dynamic_context->error_record, error);
}

/* ---------------------------------------------------------- Entry points */

/* Reads a value completely, so lazy sequences in it raise now (D51). */
static sxml_status sxml_value_make_complete(sxml_dynamic_context *context,
                                            sxml_sequence value) {
    uint64_t item_count = 0;
    return sxml_sequence_count(value, context->error_record, &item_count);
}

/* Releases the global table of the evaluation. */
static void sxml_release_global_slots(sxml_dynamic_context *context) {
    for (size_t slot_index = 0; slot_index < context->global_slot_count;
         slot_index += 1) {
        sxml_sequence_release(context->global_slots[slot_index].value);
    }
    if (context->global_slots != NULL) {
        sxml_allocator_release(context->allocator, context->global_slots);
    }
    context->global_slots = NULL;
    context->global_slot_count = 0;
}

/* Fills the global table from the request: external values coerced to
   their declared types (XPTY0004). */
static sxml_status sxml_bind_global_slots(sxml_dynamic_context *context,
                                          const sxml_evaluation_request *request) {
    const sxml_ir_unit *unit = request->unit;
    const size_t global_count =
        unit->global_variable_count > 1 ? unit->global_variable_count - 1 : 0;
    if (global_count == 0) {
        return SXML_STATUS_OK;
    }
    if (request->global_values.slot_count != global_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        context->allocator,
        (sxml_allocation_request){.byte_count = global_count * sizeof(sxml_evaluator_slot),
                                  .alignment = alignof(sxml_evaluator_slot)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->global_slots = memory;
    for (size_t slot_index = 0; slot_index < global_count; slot_index += 1) {
        context->global_slots[slot_index] = (sxml_evaluator_slot){
            .value = sxml_sequence_empty(),
            .slot_state = SXML_EVALUATOR_SLOT_STATE_ABSENT};
    }
    context->global_slot_count = global_count;
    for (size_t slot_index = 0; status == SXML_STATUS_OK && slot_index < global_count;
         slot_index += 1) {
        const sxml_ir_global_variable *variable = sxml_ir_unit_get_global_variable(
            unit, (sxml_ir_global_index){.value = (uint32_t)slot_index + 1});
        const sxml_evaluator_slot *given = &request->global_values.slots[slot_index];
        /* An ABSENT variable with an initializer is computed by the global
           part (sxml_evaluator_start_global_initializers); a given value
           wins over the initializer (XQuery 4.0, "Variable Declaration": an external
           variable's default). */
        if (given->slot_state != SXML_EVALUATOR_SLOT_STATE_BOUND) {
            continue;
        }
        sxml_sequence coerced = sxml_sequence_empty();
        status = sxml_evaluator_coerce(
            context,
            &(sxml_coercion_request){.value = given->value,
                                     .sequence_type = variable->declared_type,
                                     .role = SXML_IR_COERCION_ROLE_VARIABLE,
                                     .error_record = context->error_record},
            &coerced);
        if (status == SXML_STATUS_OK) {
            context->global_slots[slot_index] = (sxml_evaluator_slot){
                .value = coerced, .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
        }
    }
    return status;
}

/* Starts the main function: a RETURN owning its frame, with the focus
   slots bound to the context value (XPath 4.0, section 2.2.2), and its
   body. */
static sxml_status sxml_start_main_function(sxml_dynamic_context *context,
                                            const sxml_evaluation_request *request) {
    const sxml_ir_function *main_function =
        sxml_ir_unit_get_function(request->unit, (sxml_ir_function_index){.value = 1});
    if (main_function->body.value == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t frame_base = 0;
    sxml_status status =
        sxml_machine_push_frame(context, main_function->slot_count, &frame_base);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_evaluator_task main_return =
        sxml_expression_task(main_function, frame_base, main_function->body);
    main_return.task_kind = SXML_EVALUATOR_TASK_KIND_RETURN;
    main_return.counts_depth = true;
    status = sxml_machine_push(context, &main_return);
    if (status != SXML_STATUS_OK) {
        sxml_machine_pop_frame(context, frame_base);
        return status;
    }
    context->evaluation_depth += 1;
    if (request->has_context_value) {
        sxml_machine_bind_focus(
            context, &(sxml_focus_binding){.frame_base = frame_base,
                                           .focus = main_function->focus,
                                           .value = request->context_value,
                                           .position = 1,
                                           .size = 1});
    }
    const sxml_evaluator_task body =
        sxml_expression_task(main_function, frame_base, main_function->body);
    status = sxml_machine_push(context, &body);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Declared global variables (src/evaluator_global.c, D101). */
    return sxml_evaluator_start_global_initializers(context);
}

/* Ends a run: restores the context for the next one. */
static void sxml_end_run(sxml_dynamic_context *context, size_t base_count) {
    sxml_machine_unwind(context, base_count);
    sxml_release_memo(context);
    context->is_running = false;
    context->error_record = NULL;
    context->caught_status = SXML_STATUS_OK;
}

sxml_status sxml_evaluator_evaluate(sxml_dynamic_context *dynamic_context,
                                    const sxml_evaluation_request *request,
                                    sxml_sequence *result) {
    if (dynamic_context->is_running || request->unit == NULL ||
        !request->unit->is_sealed) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_dynamic_context *context = dynamic_context;
    context->is_running = true;
    context->error_record = request->error_record;
    context->has_clock_reading = false;
    context->evaluation_step_count = 0;
    context->unit = request->unit;
    context->static_base_uri = request->static_base_uri;
    sxml_ir_unit_retain(request->unit);
    const size_t base_count = context->task_stack.task_count;
    sxml_status status = sxml_bind_global_slots(context, request);
    if (status == SXML_STATUS_OK) {
        status = sxml_start_main_function(context, request);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_machine_run(context, base_count);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        value = sxml_machine_take_value(context);
        status = sxml_value_make_complete(context, value);
    }
    sxml_end_run(context, base_count);
    sxml_release_global_slots(context);
    context->unit = NULL;
    context->static_base_uri = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    sxml_ir_unit_release(request->unit);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(value);
        return status;
    }
    *result = value;
    return SXML_STATUS_OK;
}

sxml_status sxml_evaluator_call_function_item(const sxml_function_call *call,
                                              sxml_sequence *result) {
    sxml_dynamic_context *context = call->dynamic_context;
    if (context == NULL || context->is_running ||
        !sxml_item_is_function_item(call->function_item)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const size_t argument_count = call->arguments.sequence_count;
    sxml_sequence *arguments = NULL;
    if (argument_count > 0) {
        void *memory = NULL;
        sxml_status status = sxml_allocator_allocate(
            context->allocator,
            (sxml_allocation_request){.byte_count = argument_count * sizeof(sxml_sequence),
                                      .alignment = alignof(sxml_sequence)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        arguments = memory;
        for (size_t argument_index = 0; argument_index < argument_count;
             argument_index += 1) {
            arguments[argument_index] = call->arguments.sequences[argument_index];
            sxml_sequence_retain(arguments[argument_index]);
        }
    }
    context->is_running = true;
    context->error_record = call->error_record;
    context->has_clock_reading = false;
    context->evaluation_step_count = 0;
    const size_t base_count = context->task_stack.task_count;
    sxml_evaluator_task root = sxml_expression_task(NULL, 0, (sxml_ir_expression_index){0});
    root.task_kind = SXML_EVALUATOR_TASK_KIND_CALL;
    sxml_item_retain(call->function_item);
    root.details.call = (sxml_call_state){.function_item = call->function_item,
                                          .callee = NULL,
                                          .native_function = NULL,
                                          .arguments = arguments,
                                          .argument_count = argument_count,
                                          .native_call = NULL};
    sxml_status status = sxml_machine_push(context, &root);
    if (status != SXML_STATUS_OK) {
        sxml_machine_release_task(context, &root);
    } else {
        status = sxml_machine_run(context, base_count);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        value = sxml_machine_take_value(context);
        status = sxml_value_make_complete(context, value);
    }
    sxml_end_run(context, base_count);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(value);
        return status;
    }
    *result = value;
    return SXML_STATUS_OK;
}
