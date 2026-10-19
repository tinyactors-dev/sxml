/*
 * The verifier: the one pass of Phase 1 (src/ir_pass_internal.h;
 * docs/DECISIONS.md, D46, D55). It checks the invariants every other pass
 * and the evaluator rely on, and changes nothing.
 *
 * Golden file: src/memory.c. Each function body is walked with an explicit
 * stack of tasks (visit an expression, bind a slot, unbind it), so long LET
 * chains cost no C stack, and slots are bound exactly where the IR binds
 * them: a LET's variable in its body only, a focus in the body of the MAP,
 * FILTER or SORT that binds it, a quantified variable in the condition.
 */
#include "ir_pass_internal.h"

#include <stdalign.h>
#include <string.h>

typedef enum verify_task_kind {
    SXML_VERIFY_TASK_KIND_VISIT,
    SXML_VERIFY_TASK_KIND_BIND,
    SXML_VERIFY_TASK_KIND_UNBIND,
} verify_task_kind;

typedef struct verify_task {
    verify_task_kind task_kind;
    /* An expression index or a slot number. */
    uint32_t value;
} verify_task;

typedef struct verifier {
    const sxml_ir_pass_request *request;
    const sxml_ir_unit *unit;
    sxml_allocator allocator;
    /* One flag per expression of the unit: reached from some body. */
    bool *is_reached;
    /* One count per function: the FUNCTION_DEFINITION expressions of it. */
    uint32_t *definition_counts;
    /* Flags for the slots of the function being walked, 1 to slot_count. */
    bool *is_bound;
    size_t bound_capacity;
    verify_task *tasks;
    size_t task_count;
    size_t task_capacity;
    /* The function being walked and the expression being checked. */
    uint32_t function_number;
    sxml_ir_expression_index expression;
} verifier;

/* Fills the diagnostic with what is wrong where, and returns the status. */
// ast-grep-ignore: pointer-parameter-without-length
static sxml_status report(const verifier *checker, const char *description) {
    sxml_diagnostic *diagnostic = checker->request->diagnostic;
    if (diagnostic != NULL) {
        const sxml_ir_source_span span =
            sxml_ir_unit_get_expression(checker->unit, checker->expression)
                ->source_span;
        *diagnostic = (sxml_diagnostic){
            .description = sxml_byte_view_from_c_string(description),
            .location = {.source_name = checker->unit->source_name,
                         .line_number = span.line_number,
                         .column_number = span.column_number,
                         .byte_offset = span.byte_offset},
        };
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static sxml_status allocate_zeroed(verifier *checker,
                                   sxml_memory_requirements requirements,
                                   void **memory) {
    if (requirements.byte_count == 0) {
        *memory = NULL;
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_allocator_allocate(
        checker->allocator,
        (sxml_allocation_request){.byte_count = requirements.byte_count,
                                  .alignment = requirements.alignment},
        memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(*memory, 0, requirements.byte_count);
    return SXML_STATUS_OK;
}

static sxml_status push_task(verifier *checker, verify_task task) {
    if (checker->task_count == checker->task_capacity) {
        const size_t new_capacity =
            checker->task_capacity < 32 ? 32 : checker->task_capacity * 2;
        size_t byte_count = 0;
        sxml_status status =
            sxml_size_multiply(new_capacity, sizeof(verify_task), &byte_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        void *new_tasks = NULL;
        status = sxml_allocator_allocate(
            checker->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(verify_task)},
            &new_tasks);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (checker->task_count > 0) {
            memcpy(new_tasks, checker->tasks,
                   checker->task_count * sizeof(verify_task));
        }
        sxml_allocator_release(checker->allocator, checker->tasks);
        checker->tasks = new_tasks;
        checker->task_capacity = new_capacity;
    }
    checker->tasks[checker->task_count] = task;
    checker->task_count += 1;
    return SXML_STATUS_OK;
}

static const sxml_ir_function *current_function(const verifier *checker) {
    return sxml_ir_unit_get_function(
        checker->unit,
        (sxml_ir_function_index){.value = checker->function_number});
}

static bool type_is_registered(const verifier *checker,
                               sxml_type_identifier type) {
    sxml_type_description description = {0};
    return sxml_type_registry_describe_type(checker->request->type_registry,
                                            type, &description) ==
           SXML_STATUS_OK;
}

static bool sequence_type_is_valid(const verifier *checker,
                                   sxml_sequence_type sequence_type) {
    switch (sequence_type.occurrence) {
    case SXML_OCCURRENCE_ZERO:
        return true;
    case SXML_OCCURRENCE_EXACTLY_ONE:
    case SXML_OCCURRENCE_ZERO_OR_ONE:
    case SXML_OCCURRENCE_ONE_OR_MORE:
    case SXML_OCCURRENCE_ZERO_OR_MORE:
        return type_is_registered(checker, sequence_type.item_type);
    }
    return false;
}

/* An atom read where it is: an optional atom may be NONE. */
typedef struct atom_check {
    sxml_ir_atom atom;
    bool is_optional;
} atom_check;

static sxml_status check_atom(const verifier *checker, atom_check check) {
    const sxml_ir_atom atom = check.atom;
    switch (atom.atom_kind) {
    case SXML_IR_ATOM_KIND_NONE:
        return check.is_optional
                   ? SXML_STATUS_OK
                   : report(checker, "an operand that is required is missing");
    case SXML_IR_ATOM_KIND_CONSTANT:
        if (atom.constant.value == 0 ||
            atom.constant.value >= checker->unit->constant_count) {
            return report(checker, "a constant index is out of range");
        }
        return SXML_STATUS_OK;
    case SXML_IR_ATOM_KIND_SLOT:
        if (atom.slot.value == 0 ||
            atom.slot.value > current_function(checker)->slot_count) {
            return report(checker, "a slot is beyond the function's slots");
        }
        if (!checker->is_bound[atom.slot.value]) {
            return report(checker, "a slot is read where it is not bound");
        }
        return SXML_STATUS_OK;
    case SXML_IR_ATOM_KIND_GLOBAL_VARIABLE:
        if (atom.global_variable.value == 0 ||
            atom.global_variable.value >=
                checker->unit->global_variable_count) {
            return report(checker, "a global variable index is out of range");
        }
        return SXML_STATUS_OK;
    }
    return report(checker, "an atom has an unknown kind");
}

static sxml_status check_required_atom(const verifier *checker,
                                       sxml_ir_atom atom) {
    return check_atom(checker,
                      (atom_check){.atom = atom, .is_optional = false});
}

static sxml_status check_atom_list(const verifier *checker,
                                   sxml_ir_atom_list list) {
    if (list.atom_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_atom_view atoms = sxml_ir_unit_get_atoms(checker->unit, list);
    if (atoms.atom_count != list.atom_count || list.first_atom_index == 0) {
        return report(checker, "an atom list is out of range");
    }
    for (size_t atom_index = 0; atom_index < atoms.atom_count;
         atom_index += 1) {
        sxml_status status =
            check_required_atom(checker, atoms.atoms[atom_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* The arities a native row accepts (src/function_library_internal.h). */
static bool native_arity_is_accepted(const sxml_native_function *row,
                                     uint32_t arity) {
    size_t required_count = 0;
    for (size_t parameter_index = 0;
         parameter_index < row->parameters.function_parameter_count;
         parameter_index += 1) {
        if (row->parameters.function_parameters[parameter_index]
                .parameter_default == SXML_PARAMETER_DEFAULT_REQUIRED) {
            required_count += 1;
        }
    }
    if (arity < required_count) {
        return false;
    }
    /* A row that takes the caller's focus gets three more arguments. */
    const size_t focus_count =
        (row->properties & SXML_NATIVE_FUNCTION_TAKES_CALLER_FOCUS) != 0 ? 3U : 0U;
    return (row->properties & SXML_NATIVE_FUNCTION_IS_VARIADIC) != 0 ||
           arity <= row->parameters.function_parameter_count + focus_count;
}

typedef struct target_check {
    sxml_ir_function_target target;
    uint32_t arity;
} target_check;

static sxml_status check_function_target(const verifier *checker,
                                         target_check check) {
    switch (check.target.target_kind) {
    case SXML_IR_FUNCTION_TARGET_KIND_NONE:
        break;
    case SXML_IR_FUNCTION_TARGET_KIND_NATIVE:
        if (check.target.native_function == NULL ||
            !native_arity_is_accepted(check.target.native_function,
                                      check.arity)) {
            return report(checker,
                          "a native function does not accept this arity");
        }
        return SXML_STATUS_OK;
    case SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION: {
        const uint32_t function_number = check.target.function.value;
        if (function_number == 0 ||
            function_number >= checker->unit->function_count) {
            return report(checker, "an IR function index is out of range");
        }
        if (sxml_ir_unit_get_function(checker->unit, check.target.function)
                ->parameter_count != check.arity) {
            return report(checker, "an IR function has another arity");
        }
        return SXML_STATUS_OK;
    }
    case SXML_IR_FUNCTION_TARGET_KIND_CONSTRUCTOR: {
        sxml_type_description description = {0};
        if (check.arity != 1 ||
            sxml_type_registry_describe_type(checker->request->type_registry,
                                             check.target.constructor_type,
                                             &description) != SXML_STATUS_OK ||
            (description.variety != SXML_TYPE_VARIETY_ATOMIC &&
             description.variety != SXML_TYPE_VARIETY_LIST &&
             description.variety != SXML_TYPE_VARIETY_UNION)) {
            return report(checker, "a constructor function target is invalid");
        }
        return SXML_STATUS_OK;
    }
    }
    return report(checker, "a function target is missing");
}

static sxml_status push_visit(verifier *checker,
                              sxml_ir_expression_index expression) {
    return push_task(checker, (verify_task){.task_kind = SXML_VERIFY_TASK_KIND_VISIT,
                                            .value = expression.value});
}

/* Pushes the tasks that bind slot around what was pushed after it: the
   pushing order is unbind, (the scoped visits), bind. */
static sxml_status push_slot_task(verifier *checker,
                                  verify_task_kind task_kind,
                                  sxml_ir_slot slot) {
    if (slot.value == 0) {
        return SXML_STATUS_OK;
    }
    return push_task(checker,
                     (verify_task){.task_kind = task_kind, .value = slot.value});
}

/* The tasks for a nested expression evaluated with focus bound. */
typedef struct scoped_visit {
    sxml_ir_expression_index expression;
    sxml_ir_focus focus;
} scoped_visit;

static sxml_status push_scoped_visit(verifier *checker, scoped_visit visit) {
    const sxml_ir_slot slots[] = {visit.focus.context_value,
                                  visit.focus.context_position,
                                  visit.focus.context_size};
    sxml_status status = SXML_STATUS_OK;
    for (size_t slot_index = 0; status == SXML_STATUS_OK && slot_index < 3;
         slot_index += 1) {
        status = push_slot_task(checker, SXML_VERIFY_TASK_KIND_UNBIND,
                                slots[slot_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = push_visit(checker, visit.expression);
    }
    for (size_t slot_index = 0; status == SXML_STATUS_OK && slot_index < 3;
         slot_index += 1) {
        status =
            push_slot_task(checker, SXML_VERIFY_TASK_KIND_BIND, slots[slot_index]);
    }
    return status;
}

static sxml_status check_focus_slots(const verifier *checker,
                                     sxml_ir_focus focus) {
    const uint32_t slot_count = current_function(checker)->slot_count;
    if (focus.context_value.value > slot_count ||
        focus.context_position.value > slot_count ||
        focus.context_size.value > slot_count) {
        return report(checker, "a focus slot is beyond the function's slots");
    }
    if ((focus.context_value.value != 0 &&
         (focus.context_value.value == focus.context_position.value ||
          focus.context_value.value == focus.context_size.value)) ||
        (focus.context_position.value != 0 &&
         focus.context_position.value == focus.context_size.value)) {
        return report(checker, "a focus binds one slot twice");
    }
    return SXML_STATUS_OK;
}

static sxml_status check_definition(verifier *checker,
                                    sxml_ir_function_definition definition) {
    const uint32_t function_number = definition.function.value;
    if (function_number <= 1 ||
        function_number >= checker->unit->function_count) {
        return report(checker, "a function definition names no function");
    }
    const sxml_ir_function *defined =
        sxml_ir_unit_get_function(checker->unit, definition.function);
    if (defined->defining_function.value != checker->function_number) {
        return report(checker,
                      "a function is defined outside its defining function");
    }
    checker->definition_counts[function_number] += 1;
    return check_atom_list(checker, defined->captured_variables);
}

/* A TRY_CATCH (XQuery 4.0, section 4.20): the body, each clause's name
   tests, error details slot (bound in its handler only) and handler, and
   the finally clause. Pushed so that they are visited in that order. */
static sxml_status check_try_catch(verifier *checker,
                                   const sxml_ir_try_catch *try_catch) {
    const sxml_ir_catch_clause_list list = try_catch->catch_clauses;
    const sxml_ir_catch_clause_view clauses =
        sxml_ir_unit_get_catch_clauses(checker->unit, list);
    if (clauses.catch_clause_count != list.catch_clause_count ||
        (list.catch_clause_count > 0 && list.first_catch_clause_index == 0)) {
        return report(checker, "a catch clause list is out of range");
    }
    if (try_catch->body.value == 0 ||
        (list.catch_clause_count == 0 && try_catch->finally_body.value == 0)) {
        return report(checker,
                      "a try-catch has no body or neither clauses nor finally");
    }
    sxml_status status = SXML_STATUS_OK;
    if (try_catch->finally_body.value != 0) {
        status = push_visit(checker, try_catch->finally_body);
    }
    const uint32_t slot_count = current_function(checker)->slot_count;
    for (size_t clause_number = clauses.catch_clause_count;
         status == SXML_STATUS_OK && clause_number > 0; clause_number -= 1) {
        const sxml_ir_catch_clause *clause =
            &clauses.catch_clauses[clause_number - 1];
        const sxml_ir_name_test_view tests =
            sxml_ir_unit_get_name_tests(checker->unit, clause->error_tests);
        if (tests.name_test_count == 0 ||
            tests.name_test_count != clause->error_tests.name_test_count ||
            clause->error_tests.first_name_test_index == 0) {
            return report(checker, "a catch clause has no name tests in range");
        }
        for (size_t test_index = 0; test_index < tests.name_test_count;
             test_index += 1) {
            if ((uint32_t)tests.name_tests[test_index].name_test_kind >
                (uint32_t)SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD) {
                return report(checker, "a catch clause has an unknown name test");
            }
        }
        if (clause->error_details.value == 0 ||
            clause->error_details.value > slot_count) {
            return report(checker,
                          "a catch clause binds no slot of the function");
        }
        if (clause->handler.value == 0) {
            return report(checker, "a catch clause has no handler");
        }
        status = push_slot_task(checker, SXML_VERIFY_TASK_KIND_UNBIND,
                                clause->error_details);
        if (status == SXML_STATUS_OK) {
            status = push_visit(checker, clause->handler);
        }
        if (status == SXML_STATUS_OK) {
            status = push_slot_task(checker, SXML_VERIFY_TASK_KIND_BIND,
                                    clause->error_details);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = push_visit(checker, try_catch->body);
    }
    return status;
}

static bool binding_list_is_in_range(const verifier *checker,
                                     sxml_ir_namespace_binding_list list) {
    return list.namespace_binding_count == 0 ||
           (list.first_namespace_binding_index != 0 &&
            sxml_ir_unit_get_namespace_bindings(checker->unit, list)
                    .namespace_binding_count == list.namespace_binding_count);
}

/* The fields one constructor kind uses (src/ir_internal.h,
   sxml_ir_node_construction). */
typedef struct construction_fields {
    bool has_name;
    bool has_value;
    bool has_content;
    bool has_namespace_bindings;
    bool has_name_namespaces;
    bool has_flags;
} construction_fields;

static construction_fields fields_of_kind(sxml_ir_expression_kind kind) {
    if (kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR) {
        return (construction_fields){.has_content = true, .has_flags = true};
    }
    if (kind == SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR) {
        return (construction_fields){.has_name = true,
                                     .has_content = true,
                                     .has_namespace_bindings = true,
                                     .has_name_namespaces = true,
                                     .has_flags = true};
    }
    if (kind == SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR) {
        return (construction_fields){
            .has_name = true, .has_value = true, .has_name_namespaces = true};
    }
    if (kind == SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR ||
        kind == SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR) {
        return (construction_fields){.has_name = true, .has_value = true};
    }
    if (kind == SXML_IR_EXPRESSION_KIND_SHALLOW_COPY) {
        return (construction_fields){
            .has_value = true, .has_content = true, .has_flags = true};
    }
    if (kind == SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR) {
        return (construction_fields){.has_value = true, .has_flags = true};
    }
    /* COMMENT. */
    return (construction_fields){.has_value = true};
}

/* A constructor kind or SHALLOW_COPY: fields the kind does not use empty,
   atoms valid, lists in range, known
   flags; the content is visited. */
static sxml_status check_node_construction(verifier *checker,
                                           const sxml_ir_expression *expression) {
    const sxml_ir_node_construction *construction = &expression->node_construction;
    const construction_fields fields = fields_of_kind(expression->expression_kind);
    if ((!fields.has_name && construction->name.atom_kind != SXML_IR_ATOM_KIND_NONE) ||
        (!fields.has_value &&
         construction->value.atom_kind != SXML_IR_ATOM_KIND_NONE) ||
        (!fields.has_content && construction->content.value != 0) ||
        (!fields.has_namespace_bindings &&
         construction->namespace_bindings.namespace_binding_count != 0) ||
        (!fields.has_name_namespaces &&
         construction->name_namespaces.namespace_binding_count != 0) ||
        (!fields.has_flags && construction->construction_flags != 0)) {
        return report(checker, "a constructor has a field its kind does not use");
    }
    if ((construction->construction_flags & ~SXML_IR_CONSTRUCTION_KNOWN_FLAGS) != 0) {
        return report(checker, "a constructor has unknown flags");
    }
    if (construction->module_number != 0 &&
        construction->module_number >= checker->unit->module_count) {
        return report(checker, "a constructor names a module the unit does not have");
    }
    if (!binding_list_is_in_range(checker, construction->namespace_bindings) ||
        !binding_list_is_in_range(checker, construction->name_namespaces)) {
        return report(checker, "a namespace binding list is out of range");
    }
    /* A NONE name or value is the empty sequence, which the evaluator
       turns into the kind's error or meaning (a namespace node's empty
       prefix, nothing to copy). */
    sxml_status status =
        check_atom(checker, (atom_check){.atom = construction->name, .is_optional = true});
    if (status == SXML_STATUS_OK) {
        status =
            check_atom(checker, (atom_check){.atom = construction->value, .is_optional = true});
    }
    if (status == SXML_STATUS_OK && construction->content.value != 0) {
        status = push_visit(checker, construction->content);
    }
    return status;
}

/* Checks the payload of one expression and pushes its nested parts. */
/* APPLY_TEMPLATES (D144): a unit with modes, a mode in range (0 is the
   current mode), the function's focus when select is NONE, optional
   atoms for the parameters and the separator. */
static sxml_status check_apply_templates(const verifier *checker,
                                         const sxml_ir_apply_templates *apply) {
    if (checker->unit->mode_count < 2) {
        return report(checker, "an apply-templates is in a unit without modes");
    }
    if (apply->mode.value >= checker->unit->mode_count) {
        return report(checker, "a mode index is out of range");
    }
    if ((uint32_t)apply->rule_search > (uint32_t)SXML_IR_RULE_SEARCH_IMPORTED_ONLY) {
        return report(checker, "an apply-templates has an unknown rule search");
    }
    if (apply->select.atom_kind == SXML_IR_ATOM_KIND_NONE &&
        apply->focus.context_value.value == 0 &&
        current_function(checker)->focus.context_value.value == 0) {
        return report(checker, "an apply-templates of the focus is in a function "
                               "without one");
    }
    if (apply->focus.context_value.value > current_function(checker)->slot_count ||
        apply->focus.context_position.value > current_function(checker)->slot_count ||
        apply->focus.context_size.value != 0 ||
        (apply->focus.context_value.value != 0 &&
         apply->select.atom_kind != SXML_IR_ATOM_KIND_NONE)) {
        return report(checker, "an apply-templates focus is beyond the function's slots, "
                               "has a size or comes with a select");
    }
    const sxml_ir_atom optional_atoms[] = {apply->select, apply->parameters,
                                           apply->tunnel_parameters, apply->separator};
    for (size_t atom_index = 0;
         atom_index < sizeof(optional_atoms) / sizeof(optional_atoms[0]);
         atom_index += 1) {
        const sxml_status status = check_atom(
            checker, (atom_check){.atom = optional_atoms[atom_index], .is_optional = true});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* STREAM (D219): an axis of a step down, a focus without a size, a
   counter in range; the predicate and the body in the focus. */
static sxml_status check_stream(verifier *checker, const sxml_ir_stream *stream) {
    if ((stream->flags & ~SXML_IR_STREAM_KNOWN_FLAGS) != 0) {
        return report(checker, "a stream has unknown flags");
    }
    const bool opens_document = (stream->flags & (uint32_t)SXML_IR_STREAM_OPENS_DOCUMENT) != 0;
    if (!opens_document && stream->axis != SXML_AXIS_CHILD &&
        stream->axis != SXML_AXIS_DESCENDANT) {
        return report(checker, "a stream has an axis other than child or descendant");
    }
    if (stream->focus.context_size.value != 0 || stream->body.value == 0 ||
        (opens_document && stream->predicate.value != 0)) {
        return report(checker, "a stream has a size, no body or a document predicate");
    }
    if (stream->position_counter.value > current_function(checker)->slot_count ||
        stream->fold.value > current_function(checker)->slot_count) {
        return report(checker, "a stream counter or fold is beyond the function's slots");
    }
    sxml_status status = check_required_atom(checker, stream->origin);
    if (status == SXML_STATUS_OK) {
        status = check_focus_slots(checker, stream->focus);
    }
    if (status == SXML_STATUS_OK) {
        status = push_scoped_visit(
            checker, (scoped_visit){.expression = stream->body, .focus = stream->focus});
    }
    if (status == SXML_STATUS_OK && stream->predicate.value != 0) {
        status = push_scoped_visit(
            checker, (scoped_visit){.expression = stream->predicate, .focus = stream->focus});
    }
    return status;
}

static sxml_status visit_expression(verifier *checker,
                                    sxml_ir_expression_index index) {
    checker->expression = index;
    if (index.value == 0 || index.value >= checker->unit->expression_count) {
        return report(checker, "an expression index is out of range");
    }
    if (checker->is_reached[index.value]) {
        return report(checker, "an expression is reached twice");
    }
    checker->is_reached[index.value] = true;
    const sxml_ir_expression *expression =
        sxml_ir_unit_get_expression(checker->unit, index);
    sxml_status status = SXML_STATUS_OK;
    switch (expression->expression_kind) {
    case SXML_IR_EXPRESSION_KIND_NONE:
    case SXML_IR_EXPRESSION_KIND_COUNT:
        return report(checker, "an expression has no kind");
    case SXML_IR_EXPRESSION_KIND_ATOM:
        return check_required_atom(checker, expression->atom);
    case SXML_IR_EXPRESSION_KIND_LET: {
        const sxml_ir_slot variable = expression->let.variable;
        if (variable.value == 0 ||
            variable.value > current_function(checker)->slot_count) {
            return report(checker, "a let binds no slot of the function");
        }
        status = push_slot_task(checker, SXML_VERIFY_TASK_KIND_UNBIND, variable);
        if (status == SXML_STATUS_OK) {
            status = push_visit(checker, expression->let.body);
        }
        if (status == SXML_STATUS_OK) {
            status = push_slot_task(checker, SXML_VERIFY_TASK_KIND_BIND, variable);
        }
        if (status == SXML_STATUS_OK) {
            status = push_visit(checker, expression->let.bound_expression);
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_SEQUENCE:
        return check_atom_list(checker, expression->sequence);
    case SXML_IR_EXPRESSION_KIND_CONDITIONAL:
        status = check_required_atom(checker, expression->conditional.condition);
        if (status == SXML_STATUS_OK) {
            status = push_visit(checker, expression->conditional.else_branch);
        }
        if (status == SXML_STATUS_OK) {
            status = push_visit(checker, expression->conditional.then_branch);
        }
        return status;
    case SXML_IR_EXPRESSION_KIND_BINARY:
        if ((uint32_t)expression->binary.operator>
            (uint32_t)SXML_IR_BINARY_OPERATOR_CONCATENATE) {
            return report(checker, "a binary operator is unknown");
        }
        status = check_required_atom(checker, expression->binary.left_operand);
        if (status == SXML_STATUS_OK) {
            status =
                check_required_atom(checker, expression->binary.right_operand);
        }
        return status;
    case SXML_IR_EXPRESSION_KIND_UNARY:
        if (expression->unary.operator != SXML_IR_UNARY_OPERATOR_PLUS &&
            expression->unary.operator != SXML_IR_UNARY_OPERATOR_MINUS) {
            return report(checker, "a unary operator is unknown");
        }
        return check_required_atom(checker, expression->unary.operand);
    case SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION:
        return check_definition(checker, expression->function_definition);
    case SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE:
        if (!type_is_registered(checker,
                                expression->function_reference.signature)) {
            return report(checker, "a function reference has no signature");
        }
        return check_function_target(
            checker,
            (target_check){.target = expression->function_reference.target,
                           .arity = expression->function_reference.arity});
    case SXML_IR_EXPRESSION_KIND_STATIC_CALL:
        status = check_function_target(
            checker,
            (target_check){
                .target = expression->static_call.target,
                .arity = expression->static_call.arguments.atom_count});
        if (status == SXML_STATUS_OK) {
            status = check_atom_list(checker, expression->static_call.arguments);
        }
        if (status == SXML_STATUS_OK &&
            expression->static_call.module_number >= checker->unit->module_count &&
            expression->static_call.module_number != 0) {
            return report(checker, "a call names a module the unit does not have");
        }
        return status;
    case SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL:
        status = check_required_atom(checker,
                                     expression->dynamic_call.function_item);
        if (status == SXML_STATUS_OK) {
            status =
                check_atom_list(checker, expression->dynamic_call.arguments);
        }
        return status;
    case SXML_IR_EXPRESSION_KIND_LOOKUP:
        status = check_required_atom(checker, expression->lookup.value);
        if (status == SXML_STATUS_OK) {
            status = check_atom(checker,
                                (atom_check){.atom = expression->lookup.key,
                                             .is_optional = true});
        }
        return status;
    case SXML_IR_EXPRESSION_KIND_MAP:
        if ((expression->map.flags &
             ~((uint32_t)SXML_IR_MAP_ALLOWING_EMPTY | (uint32_t)SXML_IR_MAP_REQUIRES_NODES |
               (uint32_t)SXML_IR_MAP_IS_AXIS_STEP)) != 0) {
            return report(checker, "a map has unknown flags");
        }
        status = check_required_atom(checker, expression->map.input);
        if (status == SXML_STATUS_OK) {
            status = check_focus_slots(checker, expression->map.focus);
        }
        if (status == SXML_STATUS_OK) {
            status = push_scoped_visit(
                checker, (scoped_visit){.expression = expression->map.body,
                                        .focus = expression->map.focus});
        }
        return status;
    case SXML_IR_EXPRESSION_KIND_FILTER: {
        const sxml_ir_filter_index *filter_index = &expression->filter.index;
        status = check_required_atom(checker, expression->filter.input);
        if (status == SXML_STATUS_OK) {
            status = check_focus_slots(checker, expression->filter.focus);
        }
        if (status == SXML_STATUS_OK) {
            status = push_scoped_visit(
                checker,
                (scoped_visit){.expression = expression->filter.predicate,
                               .focus = expression->filter.focus});
        }
        if (status != SXML_STATUS_OK || filter_index->key.value == 0) {
            return status;
        }
        /* A filter index (D224): the key with the focus value only, the
           probe outside the focus. */
        if (filter_index->probe.value == 0 || expression->filter.focus.context_value.value == 0 ||
            (filter_index->key_operator != SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL &&
             filter_index->key_operator != SXML_IR_BINARY_OPERATOR_VALUE_EQUAL &&
             filter_index->key_operator != SXML_IR_BINARY_OPERATOR_IS)) {
            return report(checker, "a filter index is incomplete");
        }
        status = push_scoped_visit(
            checker,
            (scoped_visit){.expression = filter_index->key,
                           .focus = {.context_value = expression->filter.focus.context_value}});
        if (status == SXML_STATUS_OK) {
            status = push_visit(checker, filter_index->probe);
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_QUANTIFIED: {
        if (expression->quantified.quantifier != SXML_IR_QUANTIFIER_SOME &&
            expression->quantified.quantifier != SXML_IR_QUANTIFIER_EVERY) {
            return report(checker, "a quantifier is unknown");
        }
        const sxml_ir_slot variable = expression->quantified.variable;
        if (variable.value == 0 ||
            variable.value > current_function(checker)->slot_count) {
            return report(checker,
                          "a quantified expression binds no slot of the "
                          "function");
        }
        status = check_required_atom(checker, expression->quantified.input);
        if (status == SXML_STATUS_OK) {
            status = push_scoped_visit(
                checker,
                (scoped_visit){.expression = expression->quantified.condition,
                               .focus = {.context_value = variable}});
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_SORT: {
        status = check_required_atom(checker, expression->sort.input);
        if (status == SXML_STATUS_OK) {
            status = check_focus_slots(checker, expression->sort.focus);
        }
        const sxml_ir_sort_key_view keys =
            sxml_ir_unit_get_sort_keys(checker->unit, expression->sort.keys);
        if (status == SXML_STATUS_OK &&
            (keys.sort_key_count != expression->sort.keys.sort_key_count ||
             keys.sort_key_count == 0)) {
            return report(checker, "a sort has no keys in range");
        }
        for (size_t key_index = 0;
             status == SXML_STATUS_OK && key_index < keys.sort_key_count;
             key_index += 1) {
            const sxml_ir_sort_key *key = &keys.sort_keys[key_index];
            if ((key->direction != SXML_IR_SORT_DIRECTION_ASCENDING &&
                 key->direction != SXML_IR_SORT_DIRECTION_DESCENDING) ||
                (key->empty_order != SXML_IR_EMPTY_ORDER_LEAST &&
                 key->empty_order != SXML_IR_EMPTY_ORDER_GREATEST)) {
                return report(checker, "a sort key has an unknown order");
            }
            status = check_atom(checker, (atom_check){.atom = key->collation,
                                                      .is_optional = true});
            if (status == SXML_STATUS_OK) {
                status = check_atom(checker, (atom_check){.atom = key->order,
                                                          .is_optional = true});
            }
            if (status == SXML_STATUS_OK) {
                status = push_scoped_visit(
                    checker, (scoped_visit){.expression = key->key,
                                            .focus = expression->sort.focus});
            }
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_STEP:
        if ((uint32_t)expression->step.axis >
            (uint32_t)SXML_AXIS_PRECEDING_OR_SELF) {
            return report(checker, "a step has an unknown axis");
        }
        if (!type_is_registered(checker, expression->step.node_test)) {
            return report(checker, "a step's node test is not registered");
        }
        return check_required_atom(checker, expression->step.origin);
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER:
        return check_required_atom(checker, expression->document_order.input);
    case SXML_IR_EXPRESSION_KIND_CAST:
    case SXML_IR_EXPRESSION_KIND_CASTABLE:
        if (!type_is_registered(checker, expression->cast.target_type)) {
            return report(checker, "a cast's target type is not registered");
        }
        return check_required_atom(checker, expression->cast.value);
    case SXML_IR_EXPRESSION_KIND_INSTANCE_OF:
    case SXML_IR_EXPRESSION_KIND_TREAT:
        if (!sequence_type_is_valid(checker,
                                    expression->type_test.sequence_type)) {
            return report(checker, "a type test has an invalid type");
        }
        return check_required_atom(checker, expression->type_test.value);
    case SXML_IR_EXPRESSION_KIND_COERCE:
        if (!sequence_type_is_valid(checker,
                                    expression->coercion.sequence_type) ||
            (uint32_t)expression->coercion.role >
                (uint32_t)SXML_IR_COERCION_ROLE_TEMPLATE_PARAMETER) {
            return report(checker, "a coercion has an invalid type or role");
        }
        return check_required_atom(checker, expression->coercion.value);
    case SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR: {
        const sxml_ir_map_entry_list list = expression->map_constructor.entries;
        const sxml_ir_map_entry_view entries =
            sxml_ir_unit_get_map_entries(checker->unit, list);
        if (entries.map_entry_count != list.map_entry_count ||
            (list.map_entry_count > 0 && list.first_map_entry_index == 0)) {
            return report(checker, "a map entry list is out of range");
        }
        for (size_t entry_index = 0;
             status == SXML_STATUS_OK && entry_index < entries.map_entry_count;
             entry_index += 1) {
            status = check_required_atom(
                checker, entries.map_entries[entry_index].key);
            if (status == SXML_STATUS_OK) {
                status = check_atom(
                    checker,
                    (atom_check){.atom = entries.map_entries[entry_index].value,
                                 .is_optional = true});
            }
        }
        return status;
    }
    case SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR:
        if (expression->array_constructor.array_form !=
                SXML_IR_ARRAY_FORM_SQUARE &&
            (expression->array_constructor.array_form !=
                 SXML_IR_ARRAY_FORM_CURLY ||
             expression->array_constructor.members.atom_count != 1)) {
            return report(checker, "an array constructor has an invalid form");
        }
        return check_atom_list(checker, expression->array_constructor.members);
    case SXML_IR_EXPRESSION_KIND_RAISE:
        if (!sxml_status_has_error_record(expression->raise.error_status) ||
            expression->raise.error_code.local_name.byte_count == 0) {
            return report(checker, "a raise has no error");
        }
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_TRY_CATCH:
        return check_try_catch(checker, &expression->try_catch);
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_SHALLOW_COPY:
        return check_node_construction(checker, expression);
    case SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES:
        return check_apply_templates(checker, &expression->apply_templates);
    case SXML_IR_EXPRESSION_KIND_STREAM:
        return check_stream(checker, &expression->stream);
    }
    return report(checker, "an expression kind is unknown");
}

/* Checks a function's own fields and binds what is bound on entry. */
static sxml_status check_function_header(verifier *checker) {
    const sxml_ir_function *function = current_function(checker);
    checker->expression = function->body;
    sxml_type_description signature = {0};
    if (sxml_type_registry_describe_type(checker->request->type_registry,
                                         function->signature,
                                         &signature) != SXML_STATUS_OK ||
        signature.variety != SXML_TYPE_VARIETY_FUNCTION ||
        signature.variety_details.function_type.is_any_function ||
        signature.variety_details.function_type.parameter_types
                .sequence_type_count != function->parameter_count) {
        return report(checker,
                      "a function's signature is not a function type of its "
                      "arity");
    }
    if (function->defining_function.value >= checker->unit->function_count ||
        function->defining_function.value == checker->function_number) {
        return report(checker, "a function's defining function is invalid");
    }
    const sxml_ir_atom_list captures = function->captured_variables;
    if (sxml_ir_unit_get_atoms(checker->unit, captures).atom_count !=
            captures.atom_count ||
        (captures.atom_count > 0 && captures.first_atom_index == 0)) {
        return report(checker, "a capture list is out of range");
    }
    if (captures.atom_count > 0 && function->defining_function.value == 0) {
        return report(checker, "a top-level function captures variables");
    }
    const uint64_t entry_slot_count =
        (uint64_t)function->parameter_count + captures.atom_count;
    if (entry_slot_count > function->slot_count) {
        return report(checker,
                      "a function has fewer slots than parameters and "
                      "captures");
    }
    sxml_status status = check_focus_slots(checker, function->focus);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (function->body.value == 0) {
        return report(checker, "a function has no body");
    }
    memset(checker->is_bound, 0,
           ((size_t)function->slot_count + 1) * sizeof(bool));
    for (uint64_t slot_number = 1; slot_number <= entry_slot_count;
         slot_number += 1) {
        checker->is_bound[slot_number] = true;
    }
    const sxml_ir_slot focus_slots[] = {function->focus.context_value,
                                        function->focus.context_position,
                                        function->focus.context_size};
    for (size_t slot_index = 0; slot_index < 3; slot_index += 1) {
        checker->is_bound[focus_slots[slot_index].value] = true;
    }
    checker->is_bound[0] = false;
    return SXML_STATUS_OK;
}

static sxml_status run_task(verifier *checker, verify_task task) {
    switch (task.task_kind) {
    case SXML_VERIFY_TASK_KIND_VISIT:
        return visit_expression(checker,
                                (sxml_ir_expression_index){.value = task.value});
    case SXML_VERIFY_TASK_KIND_BIND:
        if (checker->is_bound[task.value]) {
            return report(checker, "a slot is bound twice on one path");
        }
        checker->is_bound[task.value] = true;
        return SXML_STATUS_OK;
    case SXML_VERIFY_TASK_KIND_UNBIND:
        checker->is_bound[task.value] = false;
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static sxml_status verify_function(verifier *checker) {
    sxml_status status = check_function_header(checker);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    checker->task_count = 0;
    status = push_visit(checker, current_function(checker)->body);
    while (status == SXML_STATUS_OK && checker->task_count > 0) {
        checker->task_count -= 1;
        status = run_task(checker, checker->tasks[checker->task_count]);
    }
    return status;
}

static sxml_status verify_globals(verifier *checker) {
    const sxml_ir_unit *unit = checker->unit;
    checker->expression = (sxml_ir_expression_index){.value = 0};
    for (uint32_t global_number = 1;
         global_number < unit->global_variable_count; global_number += 1) {
        const sxml_ir_global_variable *global =
            sxml_ir_unit_get_global_variable(
                unit, (sxml_ir_global_index){.value = global_number});
        if (!sequence_type_is_valid(checker, global->declared_type)) {
            return report(checker, "a global variable has an invalid type");
        }
        if (global->initializer.value != 0 &&
            (global->initializer.value >= unit->function_count ||
             sxml_ir_unit_get_function(unit, global->initializer)
                     ->parameter_count != 0)) {
            return report(checker, "a global variable's initializer is "
                                   "not a function of arity 0");
        }
    }
    return SXML_STATUS_OK;
}

/* The parameter count of a template function (D144). */
#define TEMPLATE_FUNCTION_PARAMETER_COUNT 5U

static bool is_function_of_arity(const sxml_ir_unit *unit, sxml_ir_function_index function,
                                 uint32_t arity) {
    return function.value != 0 && function.value < unit->function_count &&
           sxml_ir_unit_get_function(unit, function)->parameter_count == arity;
}

/* Modes and template rules (D144): rules in range, match functions of
   arity 1, template functions (and built-in templates) of arity 5. */
static sxml_status verify_modes(verifier *checker) {
    const sxml_ir_unit *unit = checker->unit;
    checker->expression = (sxml_ir_expression_index){.value = 0};
    for (uint32_t mode_number = 1; mode_number < unit->mode_count; mode_number += 1) {
        const sxml_ir_mode *mode =
            sxml_ir_unit_get_mode(unit, (sxml_ir_mode_index){.value = mode_number});
        const sxml_ir_template_rule_view rules =
            sxml_ir_unit_get_template_rules(unit, mode->template_rules);
        if (rules.template_rule_count != mode->template_rules.template_rule_count ||
            (rules.template_rule_count > 0 &&
             mode->template_rules.first_template_rule_index == 0)) {
            return report(checker, "a mode's template rules are out of range");
        }
        if (!is_function_of_arity(unit, mode->built_in_template,
                                  TEMPLATE_FUNCTION_PARAMETER_COUNT)) {
            return report(checker, "a mode's built-in template is not a template function");
        }
        for (size_t rule_index = 0; rule_index < rules.template_rule_count; rule_index += 1) {
            const sxml_ir_template_rule *rule = &rules.template_rules[rule_index];
            if (!is_function_of_arity(unit, rule->match_function, 1) ||
                !is_function_of_arity(unit, rule->template_function,
                                      TEMPLATE_FUNCTION_PARAMETER_COUNT)) {
                return report(checker, "a template rule's match or template function "
                                       "has the wrong arity");
            }
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status verify_unit(verifier *checker) {
    const sxml_ir_unit *unit = checker->unit;
    if (unit->function_count < 2) {
        return report(checker, "the unit has no main function");
    }
    sxml_status status = verify_globals(checker);
    if (status == SXML_STATUS_OK) {
        status = verify_modes(checker);
    }
    for (uint32_t function_number = 1;
         status == SXML_STATUS_OK && function_number < unit->function_count;
         function_number += 1) {
        checker->function_number = function_number;
        status = verify_function(checker);
    }
    for (uint32_t function_number = 2;
         status == SXML_STATUS_OK && function_number < unit->function_count;
         function_number += 1) {
        checker->function_number = function_number;
        const sxml_ir_function *function = current_function(checker);
        checker->expression = function->body;
        const uint32_t expected_count =
            function->defining_function.value == 0 ? 0U : 1U;
        if (checker->definition_counts[function_number] != expected_count) {
            status = report(checker,
                            expected_count == 0
                                ? "a top-level function is defined by an "
                                  "expression"
                                : "a nested function is not defined exactly "
                                  "once");
        }
    }
    return status;
}

sxml_status sxml_ir_verify(const sxml_ir_pass_request *request) {
    if (request == NULL || request->unit == NULL ||
        request->type_registry == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_ir_unit *unit = request->unit;
    verifier checker = {
        .request = request,
        .unit = unit,
        .allocator = unit->allocator,
    };
    uint32_t largest_slot_count = 0;
    for (uint32_t function_number = 1; function_number < unit->function_count;
         function_number += 1) {
        const uint32_t slot_count =
            sxml_ir_unit_get_function(
                unit, (sxml_ir_function_index){.value = function_number})
                ->slot_count;
        if (slot_count > largest_slot_count) {
            largest_slot_count = slot_count;
        }
    }
    void *reached_memory = NULL;
    sxml_status status = allocate_zeroed(
        &checker,
        (sxml_memory_requirements){.byte_count = unit->expression_count,
                                   .alignment = alignof(bool)},
        &reached_memory);
    checker.is_reached = reached_memory;
    size_t count_bytes = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(unit->function_count, sizeof(uint32_t),
                                    &count_bytes);
    }
    void *count_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            &checker,
            (sxml_memory_requirements){.byte_count = count_bytes,
                                       .alignment = alignof(uint32_t)},
            &count_memory);
        checker.definition_counts = count_memory;
    }
    void *bound_memory = NULL;
    if (status == SXML_STATUS_OK) {
        checker.bound_capacity = (size_t)largest_slot_count + 1;
        status = allocate_zeroed(
            &checker,
            (sxml_memory_requirements){.byte_count = checker.bound_capacity,
                                       .alignment = alignof(bool)},
            &bound_memory);
        checker.is_bound = bound_memory;
    }
    if (status == SXML_STATUS_OK) {
        status = verify_unit(&checker);
    }
    sxml_allocator_release(checker.allocator, checker.tasks);
    sxml_allocator_release(checker.allocator, checker.is_bound);
    sxml_allocator_release(checker.allocator, checker.definition_counts);
    sxml_allocator_release(checker.allocator, checker.is_reached);
    return status;
}
