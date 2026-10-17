/*
 * Evaluator part: try/catch (src/evaluator_machine_internal.h, D101, D109).
 *
 * TRY_CATCH (src/ir_internal.h) runs as one catching task (glossary
 * `catching_task`) that steps through four phases:
 *
 *     START    makes the state, pushes the try clause, catching;
 *     BODY     the try clause ended: its value, or an error the machine
 *              unwound to this task; an error that a catch clause
 *              matches binds the error details (glossary `error_details`)
 *              and pushes that clause's handler;
 *     HANDLER  the catch clause ended;
 *     FINALLY  the finally clause ended: it must yield the empty
 *              sequence (XQTY0153); then the value of the try or catch
 *              clause is handed down, or their error (glossary
 *              `pending_error`) is raised again.
 *
 * Only dynamic and type errors are caught (the machine never hands this
 * task any other status): limits, memory and invalid arguments end the
 * evaluation without running a catch or finally clause.
 *
 * When the caller passed no error record, the task points the context at
 * an error record of its own while it runs (D109), so a caught error
 * still has a code, a description and a value to read; the context gets
 * NULL back when the task ends, normally or by unwinding.
 *
 * No C recursion (D71): every clause is a task pushed on the machine's
 * stack; the step functions return to the machine's one loop.
 *
 * Golden file: src/evaluator.c (step functions, machine API).
 *
 * Specification: XQuery 4.0, section 4.20 "Try/Catch Expressions".
 */
#include "evaluator_machine_internal.h"

#include "sxml/name.h"
#include "sxml/types.h"

#include <stdalign.h>

/* --------------------------------------------------------------- State */

typedef enum sxml_try_phase {
    SXML_TRY_PHASE_START = 0,
    SXML_TRY_PHASE_BODY,
    SXML_TRY_PHASE_HANDLER,
    SXML_TRY_PHASE_FINALLY,
} sxml_try_phase;

/*
 * What a caught error binds (XQuery 4.0, "Try/Catch Expressions", the
 * table of $err: variables), copied out of the error record before
 * anything else runs (D41). Every field owned.
 */
typedef struct sxml_try_error_details {
    sxml_status error_status;
    /* $err:code: an xs:QName. */
    sxml_item code;
    /* $err:description: an xs:string or empty. */
    sxml_sequence description;
    /* $err:value, when the error has one. */
    bool has_value;
    sxml_sequence value;
    /* $err:module: an xs:string or empty. */
    sxml_sequence module;
    /* $err:line-number and $err:column-number; 0 when unknown. */
    uint32_t line_number;
    uint32_t column_number;
} sxml_try_error_details;

typedef struct sxml_try_state {
    /* The record the context points at while the task runs, when the
       caller passed none (D109). */
    sxml_error_record own_error_record;
    bool uses_own_error_record;
    /* The value of the try or catch clause, owned, until the finally
       clause has run. */
    sxml_sequence result;
    /* The error of the try or catch clause that the finally clause raises
       again when it succeeds (glossary `pending_error`). */
    bool has_pending_error;
    sxml_try_error_details pending_error;
} sxml_try_state;

static sxml_try_error_details sxml_try_no_error_details(void) {
    return (sxml_try_error_details){
        .error_status = SXML_STATUS_OK,
        .code = {.representation = 0},
        .description = sxml_sequence_empty(),
        .has_value = false,
        .value = sxml_sequence_empty(),
        .module = sxml_sequence_empty(),
        .line_number = 0,
        .column_number = 0};
}

static void sxml_try_release_error_details(sxml_try_error_details *details) {
    sxml_item_release(details->code);
    sxml_sequence_release(details->description);
    sxml_sequence_release(details->value);
    sxml_sequence_release(details->module);
    *details = sxml_try_no_error_details();
}

static void sxml_try_release_state(sxml_dynamic_context *dynamic_context,
                                   void *state) {
    sxml_try_state *try_state = state;
    if (try_state->uses_own_error_record &&
        dynamic_context->error_record == &try_state->own_error_record) {
        dynamic_context->error_record = NULL;
    }
    sxml_sequence_release(try_state->result);
    sxml_try_release_error_details(&try_state->pending_error);
    sxml_allocator_release(dynamic_context->allocator, try_state);
}

/* The state of the TRY_CATCH task on top. */
static sxml_try_state *sxml_try_get_state(sxml_dynamic_context *dynamic_context) {
    return sxml_evaluator_machine_top(dynamic_context)->details.part.state;
}

/* The TRY_CATCH payload of the task on top. */
static const sxml_ir_try_catch *sxml_try_get_expression(
    sxml_dynamic_context *dynamic_context) {
    const sxml_evaluator_task *task = sxml_evaluator_machine_top(dynamic_context);
    return &sxml_ir_unit_get_expression(task->function->unit, task->expression)->try_catch;
}

/* ------------------------------------------------------- Error details */

/* The code of record; err:FOER0000 for a record left without one (a host
   function that returned an error status without reporting it). */
static sxml_error_code sxml_try_read_error_code(const sxml_error_record *record) {
    if (record->code.local_name.byte_count != 0) {
        return record->code;
    }
    return (sxml_error_code){
        .namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
        .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FOER0000"),
        .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")};
}

/* An xs:string item over utf8 (copied), as a sequence. */
static sxml_status sxml_try_make_string(sxml_item_heap *item_heap, sxml_byte_view utf8,
                                        sxml_sequence *string) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = utf8,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *string = sxml_sequence_from_item(item);
    }
    return status;
}

/* The xs:QName item of code, its names interned in the heap's pool. A
   prefix is kept only for a name in a namespace (XDM 4.0, section 5.6). */
static sxml_status sxml_try_make_code(sxml_item_heap *item_heap, sxml_error_code code,
                                      sxml_item *item) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_status status =
        sxml_name_pool_intern_namespace_uri(name_pool, code.namespace_uri, &namespace_uri);
    sxml_name_identifier expanded_name = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = code.local_name},
            &expanded_name);
    }
    sxml_namespace_prefix_identifier namespace_prefix = {0};
    if (status == SXML_STATUS_OK && namespace_uri.index != (uint32_t)SXML_NAMESPACE_NONE) {
        status = sxml_name_pool_intern_namespace_prefix(name_pool, code.namespace_prefix,
                                                        &namespace_prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = expanded_name,
                                                  .namespace_prefix = namespace_prefix}}}},
        item);
}

/* Copies the error in the context's record, raised with error_status, into
   *details (owned). The record's text and value are valid only until the
   next error is raised (D41), so this runs before anything else. */
static sxml_status sxml_try_capture_error(sxml_dynamic_context *dynamic_context,
                                          sxml_status error_status,
                                          sxml_try_error_details *details) {
    const sxml_error_record *record = dynamic_context->error_record;
    sxml_item_heap *item_heap = dynamic_context->item_heap;
    sxml_try_error_details captured = sxml_try_no_error_details();
    captured.error_status = error_status;
    sxml_status status =
        sxml_try_make_code(item_heap, sxml_try_read_error_code(record), &captured.code);
    if (status == SXML_STATUS_OK && record->diagnostic.description.byte_count != 0) {
        status = sxml_try_make_string(item_heap, record->diagnostic.description,
                                      &captured.description);
    }
    if (status == SXML_STATUS_OK && record->diagnostic.location.source_name.byte_count != 0) {
        status = sxml_try_make_string(item_heap, record->diagnostic.location.source_name,
                                      &captured.module);
    }
    if (status != SXML_STATUS_OK) {
        sxml_try_release_error_details(&captured);
        return status;
    }
    if (record->value != NULL) {
        captured.has_value = true;
        captured.value = *record->value;
        sxml_sequence_retain(captured.value);
    }
    captured.line_number = record->diagnostic.location.line_number;
    captured.column_number =
        captured.line_number == 0 ? 0 : record->diagnostic.location.column_number;
    *details = captured;
    return SXML_STATUS_OK;
}

/* The number of entries of the error details map. */
enum { SXML_TRY_MAXIMUM_DETAIL_COUNT = 6 };

/* One entry of the error details map: its key and its value, borrowed. */
typedef struct sxml_try_detail {
    sxml_byte_view key;
    sxml_sequence value;
} sxml_try_detail;

typedef struct sxml_try_detail_list {
    sxml_try_detail details[SXML_TRY_MAXIMUM_DETAIL_COUNT];
    size_t detail_count;
} sxml_try_detail_list;

/* Adds an entry for value unless it is the empty sequence. */
static sxml_status sxml_try_add_detail(sxml_try_detail_list *list,
                                       const sxml_try_detail *detail) {
    sxml_emptiness emptiness = {.is_empty = true};
    const sxml_status status = sxml_sequence_test_emptiness(detail->value, NULL, &emptiness);
    if (status != SXML_STATUS_OK || emptiness.is_empty) {
        return status;
    }
    list->details[list->detail_count] = *detail;
    list->detail_count += 1;
    return SXML_STATUS_OK;
}

/*
 * The map $err:map (XQuery 4.0, "Try/Catch Expressions"): an entry for
 * each variable whose value is not the empty sequence, keyed by the
 * variable's local name as an xs:string, in the order of the
 * specification's table. $err:stack-trace and $err:additional are always
 * the empty sequence in sxml (D109), so they have no entry. Lowering reads
 * every $err: variable from this map, an absent entry being the empty
 * sequence.
 */
static sxml_status sxml_try_make_details_map(sxml_dynamic_context *dynamic_context,
                                             const sxml_try_error_details *details,
                                             sxml_sequence *map) {
    sxml_try_detail_list list = {.detail_count = 0};
    const sxml_sequence line_number = sxml_sequence_from_item(
        sxml_atomic_item_from_integer((int64_t)details->line_number));
    const sxml_sequence column_number = sxml_sequence_from_item(
        sxml_atomic_item_from_integer((int64_t)details->column_number));
    const sxml_try_detail candidates[SXML_TRY_MAXIMUM_DETAIL_COUNT] = {
        {.key = SXML_BYTE_VIEW_INITIALIZER("code"),
         .value = sxml_sequence_from_item(details->code)},
        {.key = SXML_BYTE_VIEW_INITIALIZER("description"), .value = details->description},
        {.key = SXML_BYTE_VIEW_INITIALIZER("value"), .value = details->value},
        {.key = SXML_BYTE_VIEW_INITIALIZER("module"), .value = details->module},
        {.key = SXML_BYTE_VIEW_INITIALIZER("line-number"),
         .value = details->line_number == 0 ? sxml_sequence_empty() : line_number},
        {.key = SXML_BYTE_VIEW_INITIALIZER("column-number"),
         .value = details->column_number == 0 ? sxml_sequence_empty() : column_number},
    };
    sxml_status status = SXML_STATUS_OK;
    for (size_t candidate_index = 0;
         status == SXML_STATUS_OK && candidate_index < SXML_TRY_MAXIMUM_DETAIL_COUNT;
         candidate_index += 1) {
        status = sxml_try_add_detail(&list, &candidates[candidate_index]);
    }
    sxml_map_entry entries[SXML_TRY_MAXIMUM_DETAIL_COUNT];
    size_t entry_count = 0;
    while (status == SXML_STATUS_OK && entry_count < list.detail_count) {
        sxml_sequence key = sxml_sequence_empty();
        status = sxml_try_make_string(dynamic_context->item_heap,
                                      list.details[entry_count].key, &key);
        if (status == SXML_STATUS_OK) {
            entries[entry_count] = (sxml_map_entry){
                .key = key.content, .value = list.details[entry_count].value};
            entry_count += 1;
        }
    }
    sxml_item map_item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            dynamic_context->item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries, .map_entry_count = entry_count},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                .error_record = NULL},
            &map_item);
    }
    for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
        sxml_item_release(entries[entry_index].key);
    }
    if (status == SXML_STATUS_OK) {
        *map = sxml_sequence_from_item(map_item);
    }
    return status;
}

/* The text of a string item of value, or empty for the empty sequence. */
static sxml_status sxml_try_read_text(sxml_sequence value, sxml_byte_view *text) {
    *text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    sxml_iteration_step step;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    sxml_status status = sxml_sequence_iterator_next(&iterator, NULL, &step);
    if (status != SXML_STATUS_OK || !step.has_item) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(step.item, &datum);
    if (status == SXML_STATUS_OK) {
        *text = datum.datum_details.string.utf8;
    }
    return status;
}

/* Raises the error of details again, through the heap (D41), and returns
   its status. */
static sxml_status sxml_try_raise_again(sxml_dynamic_context *dynamic_context,
                                        const sxml_try_error_details *details) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(dynamic_context->item_heap);
    sxml_atomic_datum code_datum;
    sxml_status status = sxml_atomic_item_get_datum(details->code, &code_datum);
    sxml_name_parts parts;
    sxml_error_code code = {.namespace_uri = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(
            name_pool, code_datum.datum_details.qname.expanded_name, &parts);
    }
    if (status == SXML_STATUS_OK) {
        code.local_name = parts.local_name;
        status = sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri,
                                                  &code.namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_prefix(
            name_pool, code_datum.datum_details.qname.namespace_prefix,
            &code.namespace_prefix);
    }
    sxml_byte_view description = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view module = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_try_read_text(details->description, &description);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_try_read_text(details->module, &module);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_item_heap_raise_error(
        dynamic_context->item_heap,
        &(sxml_error_raise_request){
            .error_status = details->error_status,
            .code = code,
            .description = description,
            .has_value = details->has_value,
            .value = details->value,
            .location = {.source_name = module,
                         .line_number = details->line_number,
                         .column_number = details->column_number,
                         .byte_offset = 0}},
        dynamic_context->error_record);
}

/* ------------------------------------------------------ Catch clauses */

/* True when name_test matches code (XQuery 4.0, "Try/Catch Expressions":
   NameTests and wildcards as in path expressions). A name the pool does
   not know matches nothing. */
static bool sxml_try_name_test_matches(const sxml_name_pool *name_pool,
                                       const sxml_name_test *name_test,
                                       const sxml_error_code *code) {
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    switch (name_test->name_test_kind) {
    case SXML_NAME_TEST_KIND_ANY_NAME:
        return true;
    case SXML_NAME_TEST_KIND_EXPANDED_NAME: {
        sxml_name_parts parts;
        return sxml_name_pool_get_name_parts(name_pool, name_test->expanded_name, &parts) ==
                   SXML_STATUS_OK &&
               sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri,
                                                &namespace_uri) == SXML_STATUS_OK &&
               sxml_byte_view_equals(parts.local_name, code->local_name) &&
               sxml_byte_view_equals(namespace_uri, code->namespace_uri);
    }
    case SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD:
        return sxml_name_pool_get_namespace_uri(name_pool, name_test->namespace_uri,
                                                &namespace_uri) == SXML_STATUS_OK &&
               sxml_byte_view_equals(namespace_uri, code->namespace_uri);
    case SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD:
        return sxml_byte_view_equals(name_test->local_name, code->local_name);
    }
    return false;
}

/* Where catch clauses are looked for: the TRY_CATCH and its unit. */
typedef struct sxml_try_clause_search {
    const sxml_ir_unit *unit;
    const sxml_ir_try_catch *try_catch;
} sxml_try_clause_search;

/* The first catch clause whose error tests match code (XQuery 4.0,
   "Try/Catch Expressions": "the first catch clause that matches"); NULL
   when none does. */
static const sxml_ir_catch_clause *sxml_try_find_clause(
    const sxml_try_clause_search *search, const sxml_name_pool *name_pool,
    const sxml_error_code *code) {
    const sxml_ir_unit *unit = search->unit;
    const sxml_ir_catch_clause_list clauses = search->try_catch->catch_clauses;
    if (clauses.first_catch_clause_index > unit->catch_clause_count ||
        clauses.catch_clause_count >
            unit->catch_clause_count - clauses.first_catch_clause_index) {
        return NULL;
    }
    for (uint32_t clause_offset = 0; clause_offset < clauses.catch_clause_count;
         clause_offset += 1) {
        const sxml_ir_catch_clause *clause =
            &unit->catch_clauses[clauses.first_catch_clause_index + clause_offset];
        const sxml_ir_name_test_list tests = clause->error_tests;
        if (tests.first_name_test_index > unit->name_test_count ||
            tests.name_test_count > unit->name_test_count - tests.first_name_test_index) {
            continue;
        }
        for (uint32_t test_offset = 0; test_offset < tests.name_test_count;
             test_offset += 1) {
            if (sxml_try_name_test_matches(
                    name_pool, &unit->name_tests[tests.first_name_test_index + test_offset],
                    code)) {
                return clause;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- Phases */

static bool sxml_try_is_catchable(sxml_status status) {
    return status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR;
}

/* Reads value completely, so the errors of a lazy sequence are raised
   where the clause that made it runs, then gives it to *complete. */
static sxml_status sxml_try_complete_value(sxml_dynamic_context *dynamic_context,
                                           sxml_sequence value, sxml_sequence *complete) {
    uint64_t item_count = 0;
    const sxml_status status =
        sxml_sequence_count(value, dynamic_context->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(value);
        return status;
    }
    *complete = value;
    return SXML_STATUS_OK;
}

/* After the try and catch clauses: runs the finally clause, or hands the
   result down when there is none. */
static sxml_status sxml_try_start_finally(sxml_dynamic_context *dynamic_context) {
    const sxml_ir_try_catch *try_catch = sxml_try_get_expression(dynamic_context);
    sxml_evaluator_task *task = sxml_evaluator_machine_top(dynamic_context);
    task->catches_errors = false;
    if (try_catch->finally_body.value == 0) {
        sxml_try_state *state = task->details.part.state;
        const sxml_sequence result = state->result;
        state->result = sxml_sequence_empty();
        return sxml_evaluator_machine_finish(dynamic_context, result);
    }
    task->phase = SXML_TRY_PHASE_FINALLY;
    return sxml_evaluator_machine_push_expression(dynamic_context, try_catch->finally_body);
}

/* Keeps a caught error for the finally clause to raise again once it
   succeeds, and starts that clause. */
static sxml_status sxml_try_keep_pending_error(sxml_dynamic_context *dynamic_context,
                                               sxml_status error_status) {
    sxml_try_state *state = sxml_try_get_state(dynamic_context);
    sxml_status status =
        sxml_try_capture_error(dynamic_context, error_status, &state->pending_error);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->has_pending_error = true;
    return sxml_try_start_finally(dynamic_context);
}

/* The try clause raised error_status: the first matching catch clause
   runs with the error details bound; with none, the error propagates
   (after the finally clause, if any). */
static sxml_status sxml_try_handle_error(sxml_dynamic_context *dynamic_context,
                                         sxml_status error_status) {
    sxml_evaluator_task *task = sxml_evaluator_machine_top(dynamic_context);
    task->catches_errors = false;
    const sxml_ir_try_catch *try_catch = sxml_try_get_expression(dynamic_context);
    const sxml_error_code code = sxml_try_read_error_code(dynamic_context->error_record);
    const sxml_ir_catch_clause *clause = sxml_try_find_clause(
        &(sxml_try_clause_search){.unit = task->function->unit, .try_catch = try_catch},
        sxml_item_heap_get_name_pool(dynamic_context->item_heap), &code);
    const bool has_finally = try_catch->finally_body.value != 0;
    if (clause == NULL) {
        if (!has_finally) {
            /* The error record still holds the error. */
            return error_status;
        }
        return sxml_try_keep_pending_error(dynamic_context, error_status);
    }
    sxml_try_error_details details = sxml_try_no_error_details();
    sxml_status status = sxml_try_capture_error(dynamic_context, error_status, &details);
    sxml_sequence map = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_try_make_details_map(dynamic_context, &details, &map);
    }
    sxml_try_release_error_details(&details);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_evaluator_machine_bind_slot(dynamic_context, clause->error_details, map);
    /* An error of the catch clause is caught only to run the finally
       clause before it propagates. */
    task = sxml_evaluator_machine_top(dynamic_context);
    task->catches_errors = has_finally;
    task->phase = SXML_TRY_PHASE_HANDLER;
    return sxml_evaluator_machine_push_expression(dynamic_context, clause->handler);
}

static sxml_status sxml_try_start(sxml_dynamic_context *dynamic_context,
                                  sxml_evaluator_task *task) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        dynamic_context->allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_try_state),
                                  .alignment = alignof(sxml_try_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_try_state *state = memory;
    *state = (sxml_try_state){
        .own_error_record = {.value = NULL},
        .uses_own_error_record = false,
        .result = sxml_sequence_empty(),
        .has_pending_error = false,
        .pending_error = sxml_try_no_error_details()};
    task->details.part.state = state;
    task->details.part.release_state = sxml_try_release_state;
    if (dynamic_context->error_record == NULL) {
        dynamic_context->error_record = &state->own_error_record;
        state->uses_own_error_record = true;
    }
    task->catches_errors = true;
    task->phase = SXML_TRY_PHASE_BODY;
    return sxml_evaluator_machine_push_expression(
        dynamic_context, sxml_try_get_expression(dynamic_context)->body);
}

/* The try clause ended, with a value or a caught error. */
static sxml_status sxml_try_after_body(sxml_dynamic_context *dynamic_context) {
    sxml_status caught_status = sxml_evaluator_machine_take_caught_status(dynamic_context);
    if (caught_status == SXML_STATUS_OK) {
        sxml_sequence value = sxml_sequence_empty();
        caught_status = sxml_try_complete_value(
            dynamic_context, sxml_evaluator_machine_take_value(dynamic_context), &value);
        if (caught_status == SXML_STATUS_OK) {
            sxml_try_get_state(dynamic_context)->result = value;
            return sxml_try_start_finally(dynamic_context);
        }
        if (!sxml_try_is_catchable(caught_status)) {
            return caught_status;
        }
    }
    return sxml_try_handle_error(dynamic_context, caught_status);
}

/* The catch clause ended, with a value or (with a finally clause) a caught
   error. */
static sxml_status sxml_try_after_handler(sxml_dynamic_context *dynamic_context) {
    sxml_status caught_status = sxml_evaluator_machine_take_caught_status(dynamic_context);
    if (caught_status == SXML_STATUS_OK) {
        sxml_sequence value = sxml_sequence_empty();
        caught_status = sxml_try_complete_value(
            dynamic_context, sxml_evaluator_machine_take_value(dynamic_context), &value);
        if (caught_status == SXML_STATUS_OK) {
            sxml_try_get_state(dynamic_context)->result = value;
            return sxml_try_start_finally(dynamic_context);
        }
    }
    const bool has_finally = sxml_try_get_expression(dynamic_context)->finally_body.value != 0;
    if (!sxml_try_is_catchable(caught_status) || !has_finally) {
        return caught_status;
    }
    return sxml_try_keep_pending_error(dynamic_context, caught_status);
}

static const sxml_static_error sxml_try_finally_value_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQTY0153"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A finally clause must yield the empty sequence (XQuery 4.0, "
        "\"Try/Catch Expressions\")"),
};

/* The finally clause ended: its error, raised in the clause, has already
   replaced the first one (XQuery 4.0, "Try/Catch Expressions": "this error
   is returned instead"); here it succeeded. */
static sxml_status sxml_try_after_finally(sxml_dynamic_context *dynamic_context) {
    const sxml_sequence value = sxml_evaluator_machine_take_value(dynamic_context);
    uint64_t item_count = 0;
    sxml_status status =
        sxml_sequence_count(value, dynamic_context->error_record, &item_count);
    sxml_sequence_release(value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count != 0) {
        return sxml_evaluator_machine_raise(dynamic_context, &sxml_try_finally_value_error);
    }
    sxml_try_state *state = sxml_try_get_state(dynamic_context);
    if (state->has_pending_error) {
        return sxml_try_raise_again(dynamic_context, &state->pending_error);
    }
    const sxml_sequence result = state->result;
    state->result = sxml_sequence_empty();
    return sxml_evaluator_machine_finish(dynamic_context, result);
}

/* The step function of TRY_CATCH. */
static sxml_status sxml_try_step(sxml_dynamic_context *dynamic_context,
                                 sxml_evaluator_task *task) {
    switch ((sxml_try_phase)task->phase) {
    case SXML_TRY_PHASE_START:
        return sxml_try_start(dynamic_context, task);
    case SXML_TRY_PHASE_BODY:
        return sxml_try_after_body(dynamic_context);
    case SXML_TRY_PHASE_HANDLER:
        return sxml_try_after_handler(dynamic_context);
    case SXML_TRY_PHASE_FINALLY:
        return sxml_try_after_finally(dynamic_context);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_evaluator_kind_handler sxml_try_kind_handlers[] = {
    {.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH, .step = sxml_try_step},
};

const sxml_evaluator_part sxml_evaluator_part_try = {
    .kind_handlers = sxml_try_kind_handlers,
    .kind_handler_count = sizeof(sxml_try_kind_handlers) / sizeof(sxml_try_kind_handlers[0]),
};
