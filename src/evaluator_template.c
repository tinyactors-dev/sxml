/*
 * Evaluator part: template rules (src/evaluator_machine_internal.h, D101,
 * D144). Evaluates APPLY_TEMPLATES (src/ir_internal.h): xsl:apply-templates,
 * xsl:next-match and xsl:apply-imports.
 *
 * One part task per APPLY_TEMPLATES runs through the selected items. For
 * each item it looks for the first template rule of the mode, in the
 * mode's order (6.8: import precedence, priority, the last declared
 * first), whose match function returns true: each match function is
 * called on the machine with the task catching errors, because an error
 * while matching means "no match" (6.5). The template function of the
 * rule found, or the mode's built-in template (6.10) when none matches, is
 * then called on the machine with the item, its position, the size, and
 * the parameter maps; its value is appended to the result, after the
 * separator from the second item on (6.7); a delivering task's template
 * calls deliver their values and the task delivers the separators (D166).
 * A mode with
 * on-multiple-match="fail" keeps looking after the first match among the
 * rules of the same import precedence and priority (XTDE0540).
 *
 * The rules tried for an item are those of its two rule buckets (glossary
 * `rule_bucket`, perf-15): the mode's rules that may match its item
 * category (its node kind, or not an XNode) whatever the name, and those
 * that may match it only under some names, its name among them. Both are
 * in rule order, so the task merges them; a rule in neither cannot match
 * the item, so its match function would have returned false, and the
 * rule chosen is the one the whole mode in order would give. Every rule
 * search, streamed modes and the built-in templates' apply-templates go
 * through this one choice.
 *
 * The current mode and the current template rule (6.9.2, 6.11) are the
 * dynamic context's current_mode and current_template_rule_index (an index
 * into the unit's template rules; 0 when absent, as for a built-in
 * template). The task sets them while a template function it called runs
 * and puts back what they were afterwards, also when it is released by
 * unwinding, so next-match and apply-imports read the rule that is
 * running. Match functions run with the caller's values.
 *
 * Rule searches: FROM_START tries every rule of the mode; AFTER_CURRENT_RULE
 * (xsl:next-match) the rules after the current one; IMPORTED_ONLY
 * (xsl:apply-imports) the rules of the levels the current rule's level
 * imports, an import precedence from the current rule's
 * lowest_imported_precedence up to below its own. Both of those process
 * the current item (select NONE: the function's focus slots) in the
 * current mode, and raise XTDE0560 without a current template rule.
 *
 * Every call is a CALL task on the machine's stack, pushed in the task's
 * frame as src/evaluator_global.c pushes initializers: a template rule
 * that applies templates to its children is a loop over the task stack,
 * never C recursion (D71), so the depth of the tree costs heap only.
 *
 * Golden file: src/evaluator_global.c (calls pushed by a part task),
 * src/evaluator_try.c (a catching part task).
 *
 * Specification: XSLT 4.0, "Template Rules" (section 6).
 */
#include "evaluator_machine_internal.h"

#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Errors */

static const sxml_static_error sxml_template_no_current_rule_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE0560"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "xsl:next-match or xsl:apply-imports is evaluated when there is no current "
        "template rule (XSLT 4.0, 6.11)"),
};

static const sxml_static_error sxml_template_multiple_match_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE0540"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "two template rules of the same import precedence and priority match an item in a "
        "mode with on-multiple-match=\"fail\" (XSLT 4.0, 6.8)"),
};

/* --------------------------------------------------------------- State */

/* The number of arguments of a template function (src/ir_internal.h). */
#define TEMPLATE_ARGUMENT_COUNT 5U

typedef enum sxml_template_phase {
    /* Reads the operands and makes the state. */
    SXML_TEMPLATE_PHASE_START = 0,
    /* Takes the next item, or finishes. */
    SXML_TEMPLATE_PHASE_NEXT_ITEM,
    /* A match function call ended (or failed: no match). */
    SXML_TEMPLATE_PHASE_MATCHED,
    /* A template function call ended. */
    SXML_TEMPLATE_PHASE_CALLED,
} sxml_template_phase;

typedef struct sxml_template_state {
    /* The items to process, owned; the item being processed is borrowed
       from it. */
    sxml_sequence input;
    sxml_sequence_iterator iterator;
    sxml_item current_item;
    uint64_t position;
    uint64_t size;
    /* Items whose template has been called. */
    uint64_t processed_count;
    /* The unit of the APPLY_TEMPLATES: its modes and rules. */
    const sxml_ir_unit *unit;
    /* Owned; empty for none. */
    sxml_sequence parameters;
    sxml_sequence tunnel_parameters;
    /* Owned; empty for none. */
    sxml_sequence separator;
    sxml_ir_mode_index mode_index;
    const sxml_ir_mode *mode;
    /* The candidates: rule indices of the unit from first_rule to before
       end_rule, filtered by precedence for IMPORTED_ONLY, among those of
       the current item's rule buckets. */
    uint32_t first_rule;
    uint32_t end_rule;
    bool filters_precedence;
    int32_t lowest_precedence;
    int32_t precedence_bound;
    /* The current item's buckets: of its category and name, and of its
       category with name 0, each with the position of its next rule. */
    sxml_ir_rule_number_view named_rules;
    size_t named_position;
    sxml_ir_rule_number_view kind_rules;
    size_t kind_position;
    /* The rule whose match function was called last, and the rule chosen
       so far (0: none). */
    uint32_t tried_rule;
    uint32_t matched_rule;
    /* The values the dynamic context had before; restored when a call
       ends or the task is released while calling. */
    sxml_ir_mode_index saved_mode;
    uint32_t saved_template_rule_index;
    bool is_calling;
    /* Owned; made on the first value. */
    sxml_sequence_builder *builder;
} sxml_template_state;

static void sxml_template_restore_context(sxml_dynamic_context *dynamic_context,
                                          sxml_template_state *state) {
    if (state->is_calling) {
        dynamic_context->current_mode = state->saved_mode;
        dynamic_context->current_template_rule_index = state->saved_template_rule_index;
        state->is_calling = false;
    }
}

static void sxml_template_release_state(sxml_dynamic_context *dynamic_context, void *state) {
    sxml_template_state *template_state = state;
    sxml_template_restore_context(dynamic_context, template_state);
    sxml_sequence_release(template_state->input);
    sxml_sequence_release(template_state->parameters);
    sxml_sequence_release(template_state->tunnel_parameters);
    sxml_sequence_release(template_state->separator);
    sxml_sequence_builder_destroy(template_state->builder);
    sxml_allocator_release(dynamic_context->allocator, template_state);
}

/* The APPLY_TEMPLATES payload of the task. */
static const sxml_ir_apply_templates *sxml_template_get_expression(
    const sxml_evaluator_task *task) {
    return &sxml_ir_unit_get_expression(task->function->unit, task->expression)->apply_templates;
}

/* -------------------------------------------------------------- Start */

/* Reads atom (optional) of the top task's frame, retained; the empty
   sequence for NONE. */
static sxml_status sxml_template_copy_atom(sxml_dynamic_context *dynamic_context,
                                           sxml_ir_atom atom, sxml_sequence *value) {
    sxml_status status = sxml_evaluator_machine_read_atom(dynamic_context, atom, value);
    if (status == SXML_STATUS_OK) {
        sxml_sequence_retain(*value);
    }
    return status;
}

/* The unnamed mode (name 0); 0 when the unit has none. */
static sxml_ir_mode_index sxml_template_find_unnamed_mode(const sxml_ir_unit *unit) {
    for (uint32_t mode_number = 1; mode_number < unit->mode_count; mode_number += 1) {
        const sxml_ir_mode_index index = {.value = mode_number};
        if (sxml_ir_unit_get_mode(unit, index)->mode_name.index == 0) {
            return index;
        }
    }
    return (sxml_ir_mode_index){.value = 0};
}

/* Reads one focus part of the task's function: slot (from the function's
   focus) into *value, retained. */
static sxml_status sxml_template_read_focus_part(sxml_dynamic_context *dynamic_context,
                                                 sxml_ir_slot slot, sxml_sequence *value) {
    return sxml_template_copy_atom(
        dynamic_context, (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = slot}, value);
}

/* An xs:integer of a focus part as a count (0 for none). */
static uint64_t sxml_template_count_of(sxml_sequence value) {
    sxml_item item = {.representation = 0};
    if (sxml_sequence_get_item(value, (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &item) !=
        SXML_STATUS_OK) {
        return 0;
    }
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_INTEGER || datum.datum_details.integer < 0) {
        return 0;
    }
    return (uint64_t)datum.datum_details.integer;
}

/* select NONE: the item of the function's focus, as the only item, with
   the function's position and size; or of apply's own focus slots when it
   has them: the node a STREAM selected, without a size (XSLT 4.0, 19;
   D223). */
static sxml_status sxml_template_select_focus(sxml_dynamic_context *dynamic_context,
                                              const sxml_ir_apply_templates *apply,
                                              sxml_template_state *state) {
    const sxml_ir_focus focus = apply->focus.context_value.value != 0
                                    ? apply->focus
                                    : sxml_evaluator_machine_top(dynamic_context)->function->focus;
    sxml_status status =
        sxml_template_read_focus_part(dynamic_context, focus.context_value, &state->input);
    sxml_sequence position = sxml_sequence_empty();
    sxml_sequence size = sxml_sequence_empty();
    if (status == SXML_STATUS_OK && focus.context_position.value != 0) {
        status = sxml_template_read_focus_part(dynamic_context, focus.context_position, &position);
    }
    if (status == SXML_STATUS_OK && focus.context_size.value != 0) {
        status = sxml_template_read_focus_part(dynamic_context, focus.context_size, &size);
    }
    /* The item keeps its own position: next_item adds one to it. */
    const uint64_t focus_position = sxml_template_count_of(position);
    const uint64_t focus_size = sxml_template_count_of(size);
    state->position = focus_position > 0 ? focus_position - 1 : 0;
    state->size = focus_size > 0 ? focus_size : 1;
    sxml_sequence_release(position);
    sxml_sequence_release(size);
    return status;
}

/* The candidate rules of the search (see the file comment). */
static sxml_status sxml_template_choose_candidates(sxml_dynamic_context *dynamic_context,
                                                   const sxml_ir_apply_templates *apply,
                                                   sxml_template_state *state) {
    const sxml_ir_unit *unit = state->unit;
    const sxml_ir_template_rule_list rules = state->mode->template_rules;
    state->first_rule = rules.first_template_rule_index;
    state->end_rule = rules.first_template_rule_index + rules.template_rule_count;
    if (apply->rule_search == SXML_IR_RULE_SEARCH_FROM_START) {
        return SXML_STATUS_OK;
    }
    const uint32_t current_rule = dynamic_context->current_template_rule_index;
    if (current_rule == 0 || current_rule >= unit->template_rule_count) {
        return sxml_evaluator_machine_raise(dynamic_context,
                                            &sxml_template_no_current_rule_error);
    }
    const sxml_ir_template_rule *rule = &unit->template_rules[current_rule];
    if (apply->rule_search == SXML_IR_RULE_SEARCH_AFTER_CURRENT_RULE) {
        if (current_rule >= state->first_rule && current_rule < state->end_rule) {
            state->first_rule = current_rule + 1;
        }
        return SXML_STATUS_OK;
    }
    state->filters_precedence = true;
    state->lowest_precedence = rule->lowest_imported_precedence;
    state->precedence_bound = rule->import_precedence;
    return SXML_STATUS_OK;
}

static sxml_status sxml_template_allocate_state(sxml_dynamic_context *dynamic_context,
                                                sxml_template_state **state) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        dynamic_context->allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_template_state),
                                  .alignment = alignof(sxml_template_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_template_state *new_state = memory;
    memset(new_state, 0, sizeof(*new_state));
    new_state->input = sxml_sequence_empty();
    new_state->parameters = sxml_sequence_empty();
    new_state->tunnel_parameters = sxml_sequence_empty();
    new_state->separator = sxml_sequence_empty();
    *state = new_state;
    return SXML_STATUS_OK;
}

/* Reads the operands into a new state, which the task then owns. */
static sxml_status sxml_template_start(sxml_dynamic_context *dynamic_context,
                                       sxml_evaluator_task *task) {
    const sxml_ir_apply_templates *apply = sxml_template_get_expression(task);
    sxml_template_state *state = NULL;
    sxml_status status = sxml_template_allocate_state(dynamic_context, &state);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    task->details.part.state = state;
    task->details.part.release_state = sxml_template_release_state;
    const sxml_ir_unit *unit = task->function->unit;
    state->unit = unit;
    const bool is_overriding = apply->rule_search != SXML_IR_RULE_SEARCH_FROM_START;
    state->mode_index = apply->mode;
    if (state->mode_index.value == 0 || is_overriding) {
        state->mode_index = dynamic_context->current_mode;
    }
    if (state->mode_index.value == 0) {
        state->mode_index = sxml_template_find_unnamed_mode(unit);
    }
    state->mode = sxml_ir_unit_get_mode(unit, state->mode_index);
    if (state->mode == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (apply->select.atom_kind == SXML_IR_ATOM_KIND_NONE) {
        status = sxml_template_select_focus(dynamic_context, apply, state);
    } else {
        status = sxml_template_copy_atom(dynamic_context, apply->select, &state->input);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(state->input, dynamic_context->error_record,
                                         &state->size);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_template_copy_atom(dynamic_context, apply->parameters, &state->parameters);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_template_copy_atom(dynamic_context, apply->tunnel_parameters,
                                         &state->tunnel_parameters);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_template_copy_atom(dynamic_context, apply->separator, &state->separator);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_template_choose_candidates(dynamic_context, apply, state);
    }
    state->iterator = sxml_sequence_iterator_start(state->input);
    state->saved_mode = dynamic_context->current_mode;
    state->saved_template_rule_index = dynamic_context->current_template_rule_index;
    return status;
}

/* --------------------------------------------------------------- Calls */

/* Pushes a CALL of function with arguments (owned; released on failure)
   in the task's frame; the task resumes in phase. */
typedef struct sxml_template_call {
    const sxml_ir_function *function;
    sxml_sequence *arguments;
    size_t argument_count;
    sxml_template_phase phase;
} sxml_template_call;

static sxml_status sxml_template_push_call(sxml_dynamic_context *dynamic_context,
                                           sxml_template_call call) {
    sxml_evaluator_task *task = sxml_evaluator_machine_top(dynamic_context);
    task->phase = (uint32_t)call.phase;
    /* A template of a delivering task delivers its value (D166); a match
       function's value is read here. */
    const bool delivers_content =
        task->delivers_content && call.phase == SXML_TEMPLATE_PHASE_CALLED;
    /* The task pointer is invalid after the push. */
    const sxml_status status =
        sxml_evaluator_machine_push_expression(dynamic_context, task->expression);
    if (status != SXML_STATUS_OK) {
        for (size_t argument_index = 0; argument_index < call.argument_count;
             argument_index += 1) {
            sxml_sequence_release(call.arguments[argument_index]);
        }
        sxml_allocator_release(dynamic_context->allocator, call.arguments);
        return status;
    }
    sxml_evaluator_task *call_task = sxml_evaluator_machine_top(dynamic_context);
    call_task->task_kind = SXML_EVALUATOR_TASK_KIND_CALL;
    call_task->phase = 0;
    call_task->delivers_content = delivers_content;
    call_task->details.call = (sxml_call_state){.function_item = {.representation = 0},
                                                .callee = call.function,
                                                .native_function = NULL,
                                                .arguments = call.arguments,
                                                .argument_count = call.argument_count,
                                                .native_call = NULL};
    return SXML_STATUS_OK;
}

/* An array of count empty sequences in allocator memory. */
static sxml_status sxml_template_allocate_arguments(sxml_dynamic_context *dynamic_context,
                                                    size_t count, sxml_sequence **arguments) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        dynamic_context->allocator,
        (sxml_allocation_request){.byte_count = count * sizeof(sxml_sequence),
                                  .alignment = alignof(sxml_sequence)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence *array = memory;
    for (size_t argument_index = 0; argument_index < count; argument_index += 1) {
        array[argument_index] = sxml_sequence_empty();
    }
    *arguments = array;
    return SXML_STATUS_OK;
}

/* Calls the match function of rule rule_number on the current item, with
   the task catching errors (6.5). */
static sxml_status sxml_template_push_match(sxml_dynamic_context *dynamic_context,
                                            sxml_template_state *state, uint32_t rule_number) {
    const sxml_ir_unit *unit = state->unit;
    sxml_sequence *arguments = NULL;
    const sxml_status status = sxml_template_allocate_arguments(dynamic_context, 1, &arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item_retain(state->current_item);
    arguments[0] = sxml_sequence_from_item(state->current_item);
    sxml_evaluator_machine_top(dynamic_context)->catches_errors = true;
    return sxml_template_push_call(
        dynamic_context,
        (sxml_template_call){
            .function = sxml_ir_unit_get_function(
                unit, unit->template_rules[rule_number].match_function),
            .arguments = arguments,
            .argument_count = 1,
            .phase = SXML_TEMPLATE_PHASE_MATCHED});
}

/* Calls the template function of rule rule_number (0: the mode's
   built-in template) for the current item, as the current template rule
   in the task's mode. */
static sxml_status sxml_template_push_template(sxml_dynamic_context *dynamic_context,
                                               sxml_template_state *state,
                                               uint32_t rule_number) {
    const sxml_ir_unit *unit = state->unit;
    const sxml_ir_function_index function = rule_number == 0
                                                ? state->mode->built_in_template
                                                : unit->template_rules[rule_number].template_function;
    sxml_sequence *arguments = NULL;
    const sxml_status status =
        sxml_template_allocate_arguments(dynamic_context, TEMPLATE_ARGUMENT_COUNT, &arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item_retain(state->current_item);
    arguments[0] = sxml_sequence_from_item(state->current_item);
    arguments[1] =
        sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)state->position));
    arguments[2] = sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)state->size));
    sxml_sequence_retain(state->parameters);
    arguments[3] = state->parameters;
    sxml_sequence_retain(state->tunnel_parameters);
    arguments[4] = state->tunnel_parameters;
    sxml_evaluator_machine_top(dynamic_context)->catches_errors = false;
    state->is_calling = true;
    dynamic_context->current_mode = state->mode_index;
    dynamic_context->current_template_rule_index = rule_number;
    return sxml_template_push_call(
        dynamic_context,
        (sxml_template_call){.function = sxml_ir_unit_get_function(unit, function),
                             .arguments = arguments,
                             .argument_count = TEMPLATE_ARGUMENT_COUNT,
                             .phase = SXML_TEMPLATE_PHASE_CALLED});
}

/* ------------------------------------------------------------- Matching */

static bool sxml_template_is_candidate(const sxml_template_state *state, uint32_t rule_number) {
    if (!state->filters_precedence) {
        return true;
    }
    const int32_t precedence = state->unit->template_rules[rule_number].import_precedence;
    return precedence >= state->lowest_precedence && precedence < state->precedence_bound;
}

/* True when rule_number may tie with the rule matched first (6.8): the
   same import precedence and priority. */
static bool sxml_template_ties(const sxml_template_state *state, uint32_t rule_number) {
    const sxml_ir_template_rule *rules = state->unit->template_rules;
    const sxml_ir_template_rule *matched = &rules[state->matched_rule];
    const sxml_ir_template_rule *rule = &rules[rule_number];
    return matched->import_precedence == rule->import_precedence &&
           !(matched->priority < rule->priority) && !(matched->priority > rule->priority);
}

/* The position of the first rule number of rules not below first_rule
   (a binary search: the numbers increase). */
static size_t sxml_template_skip_below(sxml_ir_rule_number_view rules, uint32_t first_rule) {
    size_t low = 0;
    size_t high = rules.rule_number_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        if (rules.rule_numbers[middle] < first_rule) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

/* The bucket of mode for category and name; empty when it has none. */
typedef struct sxml_template_bucket_key {
    sxml_ir_item_category item_category;
    sxml_name_identifier node_name;
} sxml_template_bucket_key;

static sxml_ir_rule_number_view sxml_template_find_bucket(const sxml_template_state *state,
                                                          sxml_template_bucket_key key) {
    const sxml_ir_rule_bucket_view buckets =
        sxml_ir_unit_get_rule_buckets(state->unit, state->mode->rule_buckets);
    size_t low = 0;
    size_t high = buckets.rule_bucket_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        const sxml_ir_rule_bucket *bucket = &buckets.rule_buckets[middle];
        if (bucket->item_category == key.item_category &&
            bucket->node_name.index == key.node_name.index) {
            return sxml_ir_unit_get_rule_numbers(state->unit, bucket->rules);
        }
        const bool is_below = bucket->item_category != key.item_category
                                  ? bucket->item_category < key.item_category
                                  : bucket->node_name.index < key.node_name.index;
        if (is_below) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return (sxml_ir_rule_number_view){.rule_numbers = NULL, .rule_number_count = 0};
}

/* The current item's category and, for an element or attribute, name.
   An item that is not an XNode, or one that cannot be described, is
   OTHER, whose bucket holds every rule of the mode (each pattern may
   match some item that is not an XNode, as the analysis has it). */
static sxml_template_bucket_key sxml_template_key_of(sxml_item item) {
    sxml_template_bucket_key key = {.item_category = SXML_IR_ITEM_CATEGORY_OTHER,
                                    .node_name = {.index = 0}};
    sxml_node_reference node = {.tree = NULL};
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    if (sxml_item_get_node(item, &node) != SXML_STATUS_OK ||
        sxml_node_describe(node, &description) != SXML_STATUS_OK) {
        return key;
    }
    switch (description.node_kind) {
    case SXML_NODE_KIND_DOCUMENT:
        key.item_category = SXML_IR_ITEM_CATEGORY_DOCUMENT;
        break;
    case SXML_NODE_KIND_ELEMENT:
        key.item_category = SXML_IR_ITEM_CATEGORY_ELEMENT;
        key.node_name = description.node_name;
        break;
    case SXML_NODE_KIND_ATTRIBUTE:
        key.item_category = SXML_IR_ITEM_CATEGORY_ATTRIBUTE;
        key.node_name = description.node_name;
        break;
    case SXML_NODE_KIND_TEXT:
        key.item_category = SXML_IR_ITEM_CATEGORY_TEXT;
        break;
    case SXML_NODE_KIND_COMMENT:
        key.item_category = SXML_IR_ITEM_CATEGORY_COMMENT;
        break;
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        key.item_category = SXML_IR_ITEM_CATEGORY_PROCESSING_INSTRUCTION;
        break;
    case SXML_NODE_KIND_NAMESPACE:
        key.item_category = SXML_IR_ITEM_CATEGORY_NAMESPACE;
        break;
    case SXML_NODE_KIND_ANY:
        break;
    }
    return key;
}

/* Takes the current item's rule buckets as its candidates, from
   first_rule on. */
static void sxml_template_find_candidates(sxml_template_state *state) {
    const sxml_template_bucket_key key = sxml_template_key_of(state->current_item);
    state->kind_rules = sxml_template_find_bucket(
        state, (sxml_template_bucket_key){.item_category = key.item_category,
                                          .node_name = {.index = 0}});
    state->named_rules = key.node_name.index == 0
                             ? (sxml_ir_rule_number_view){.rule_numbers = NULL,
                                                          .rule_number_count = 0}
                             : sxml_template_find_bucket(state, key);
    state->kind_position = sxml_template_skip_below(state->kind_rules, state->first_rule);
    state->named_position = sxml_template_skip_below(state->named_rules, state->first_rule);
}

/* The next candidate of the two buckets in rule order, or 0 when both
   are done (rule 0 is never a mode's). */
static uint32_t sxml_template_take_candidate(sxml_template_state *state) {
    const bool has_kind_rule = state->kind_position < state->kind_rules.rule_number_count;
    const bool has_named_rule = state->named_position < state->named_rules.rule_number_count;
    if (has_kind_rule &&
        (!has_named_rule || state->kind_rules.rule_numbers[state->kind_position] <
                                state->named_rules.rule_numbers[state->named_position])) {
        state->kind_position += 1;
        return state->kind_rules.rule_numbers[state->kind_position - 1U];
    }
    if (has_named_rule) {
        state->named_position += 1;
        return state->named_rules.rule_numbers[state->named_position - 1U];
    }
    return 0;
}

/* Tries the next candidate for the current item, or calls the template
   chosen when none is left. */
static sxml_status sxml_template_try_next_rule(sxml_dynamic_context *dynamic_context,
                                               sxml_template_state *state) {
    for (uint32_t rule_number = sxml_template_take_candidate(state);
         rule_number != 0 && rule_number < state->end_rule;
         rule_number = sxml_template_take_candidate(state)) {
        if (!sxml_template_is_candidate(state, rule_number)) {
            continue;
        }
        if (state->matched_rule != 0 &&
            !sxml_template_ties(state, rule_number)) {
            break;
        }
        /* The match function's call is the one whose value the next step
           reads. */
        state->tried_rule = rule_number;
        return sxml_template_push_match(dynamic_context, state, rule_number);
    }
    return sxml_template_push_template(dynamic_context, state, state->matched_rule);
}

/* The match function of rule tried_rule ended, or failed. */
static sxml_status sxml_template_take_match(sxml_dynamic_context *dynamic_context,
                                            sxml_template_state *state) {
    const sxml_status caught = sxml_evaluator_machine_take_caught_status(dynamic_context);
    sxml_evaluator_machine_top(dynamic_context)->catches_errors = false;
    if (caught != SXML_STATUS_OK) {
        /* 6.5: an error while matching is no match. */
        sxml_evaluator_machine_forget_caught_error(dynamic_context);
        return sxml_template_try_next_rule(dynamic_context, state);
    }
    const sxml_sequence value = sxml_evaluator_machine_take_value(dynamic_context);
    sxml_effective_boolean_value matches = {.is_true = false};
    const sxml_status status =
        sxml_evaluator_compute_effective_boolean_value(value, NULL, &matches);
    sxml_sequence_release(value);
    if (status != SXML_STATUS_OK || !matches.is_true) {
        return sxml_template_try_next_rule(dynamic_context, state);
    }
    const uint32_t rule_number = state->tried_rule;
    if (state->matched_rule != 0) {
        return sxml_evaluator_machine_raise(dynamic_context, &sxml_template_multiple_match_error);
    }
    state->matched_rule = rule_number;
    if (state->mode->fails_on_multiple_match) {
        return sxml_template_try_next_rule(dynamic_context, state);
    }
    return sxml_template_push_template(dynamic_context, state, rule_number);
}

/* ---------------------------------------------------------------- Items */

/* Appends value (owned) to the result, or delivers it when the task
   delivers (D166: the templates' values were delivered already). */
static sxml_status sxml_template_append(sxml_dynamic_context *dynamic_context,
                                        sxml_template_state *state, sxml_sequence value) {
    if (sxml_evaluator_machine_top(dynamic_context)->delivers_content) {
        return sxml_evaluator_machine_deliver(dynamic_context, value);
    }
    sxml_status status = SXML_STATUS_OK;
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        if (state->builder == NULL) {
            status = sxml_sequence_builder_create(
                dynamic_context->allocator,
                &(sxml_sequence_builder_configuration){.item_heap = dynamic_context->item_heap},
                &state->builder);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(state->builder, value,
                                                           dynamic_context->error_record);
        }
    }
    sxml_sequence_release(value);
    return status;
}

/* Starts the next item, or finishes with the result. */
static sxml_status sxml_template_next_item(sxml_dynamic_context *dynamic_context,
                                           sxml_template_state *state) {
    sxml_iteration_step step = {.has_item = false};
    sxml_status status =
        sxml_sequence_iterator_next(&state->iterator, dynamic_context->error_record, &step);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!step.has_item) {
        sxml_sequence result = sxml_sequence_empty();
        if (state->builder != NULL) {
            status = sxml_sequence_builder_finish(state->builder, &result);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_evaluator_machine_finish(dynamic_context, result);
    }
    if (state->processed_count > 0 &&
        sxml_sequence_get_representation(state->separator) !=
            SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        sxml_sequence_retain(state->separator);
        status = sxml_template_append(dynamic_context, state, state->separator);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    state->current_item = step.item;
    state->position += 1;
    state->processed_count += 1;
    state->matched_rule = 0;
    sxml_template_find_candidates(state);
    return sxml_template_try_next_rule(dynamic_context, state);
}

/* The step function of APPLY_TEMPLATES. */
static sxml_status sxml_template_step(sxml_dynamic_context *dynamic_context,
                                      sxml_evaluator_task *task) {
    sxml_status status = SXML_STATUS_OK;
    switch ((sxml_template_phase)task->phase) {
    case SXML_TEMPLATE_PHASE_START:
        task->phase = (uint32_t)SXML_TEMPLATE_PHASE_NEXT_ITEM;
        status = sxml_template_start(dynamic_context, task);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_template_next_item(dynamic_context, task->details.part.state);
    case SXML_TEMPLATE_PHASE_NEXT_ITEM:
        return sxml_template_next_item(dynamic_context, task->details.part.state);
    case SXML_TEMPLATE_PHASE_MATCHED:
        return sxml_template_take_match(dynamic_context, task->details.part.state);
    case SXML_TEMPLATE_PHASE_CALLED: {
        sxml_template_state *state = task->details.part.state;
        sxml_template_restore_context(dynamic_context, state);
        task->phase = (uint32_t)SXML_TEMPLATE_PHASE_NEXT_ITEM;
        status = sxml_template_append(dynamic_context, state,
                                      sxml_evaluator_machine_take_value(dynamic_context));
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_template_next_item(dynamic_context, state);
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_evaluator_kind_handler sxml_template_kind_handlers[] = {
    {.expression_kind = SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES, .step = sxml_template_step},
};

const sxml_evaluator_part sxml_evaluator_part_template = {
    .kind_handlers = sxml_template_kind_handlers,
    .kind_handler_count =
        sizeof(sxml_template_kind_handlers) / sizeof(sxml_template_kind_handlers[0]),
};

/* Only XSLT lowers to APPLY_TEMPLATES, so only XSLT's units name this
   part, and a module without XSLT does not link it (D227). */
const sxml_language_set sxml_evaluator_part_template_languages = SXML_LANGUAGE_XSLT;
