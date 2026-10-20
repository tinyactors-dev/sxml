/*
 * Static expressions of XSLT 4.0 (src/xslt_static_internal.h; XSLT 4.0,
 * sections 9.6, 9.7, 3.11.3, 3.11.4; docs/DECISIONS.md D139).
 *
 * Golden file: src/xpath.c (an expression compiled through the front end
 * and lowering, then evaluated with a dynamic context of its own); the
 * XSLT functions are native rows as in src/function_library_format.c.
 */
#include "xslt_static_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "ir_internal.h"
#include "ir_pass_internal.h"
#include "processor_internal.h"
#include "xdm_internal.h"
#include "xpath_ast_internal.h"
#include "xpath_static_internal.h"
#include "xslt_key_internal.h"
#include "xslt_value_template_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------- The XSLT functions */

/* XSLT 4.0, 9.7: element-available, function-available, type-available,
   available-system-properties and system-property, the first rows of the
   XSLT library part (src/function_library_xslt.c, packet 4.10), in front
   of the processor's library. */
static const sxml_native_function_view *const static_function_parts[] = {
    &sxml_xslt_static_functions};


/* ---------------------------------------------------------------- Scope */

typedef struct static_variable_entry {
    sxml_name_identifier variable_name;
    /* Owned. */
    sxml_sequence value;
} static_variable_entry;

struct sxml_xslt_static_scope {
    sxml_processor *processor;
    sxml_resource_resolver resource_resolver;
    /* The allocator of create; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    /* The XSLT functions; their table is searched before the
       processor's. */
    sxml_function_library *function_library;
    static_variable_entry *variables;
    size_t variable_count;
    size_t variable_capacity;
};

/* The XSLT functions, then the processor's library. */
static sxml_status find_static_function(void *state, const sxml_function_query *query,
                                        sxml_function_entry *entry) {
    const sxml_xslt_static_scope *scope = state;
    sxml_status status = sxml_function_table_find(
        sxml_function_library_as_table(scope->function_library), query, entry);
    if (status != SXML_STATUS_OK || entry->entry_kind != SXML_FUNCTION_ENTRY_KIND_NONE) {
        return status;
    }
    return sxml_function_table_find(sxml_processor_get_function_table(scope->processor), query,
                                    entry);
}

static const sxml_function_table_operations static_function_table_operations = {
    .find_function = find_static_function};

static sxml_function_table static_function_table(sxml_xslt_static_scope *scope) {
    return (sxml_function_table){.state = scope, .operations = &static_function_table_operations};
}

sxml_status sxml_xslt_static_scope_memory_requirements(
    const sxml_xslt_static_scope_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_xslt_static_scope),
                                               .alignment = alignof(sxml_xslt_static_scope)};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_static_scope_initialize(
    sxml_memory_view memory, const sxml_xslt_static_scope_configuration *configuration,
    sxml_xslt_static_scope **scope) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_xslt_static_scope_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_processor *processor = configuration->processor;
    sxml_function_library *library = NULL;
    status = sxml_function_library_create(
        processor->allocator,
        &(sxml_function_library_configuration){
            .allocator = processor->allocator,
            .item_heap = processor->item_heap,
            .parts = {.native_function_views = static_function_parts,
                      .native_function_view_count = 1}},
        &library);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_xslt_static_scope *new_scope = placement;
    *new_scope = (sxml_xslt_static_scope){
        .processor = processor,
        .resource_resolver = configuration->resource_resolver,
        .creating_allocator = {.state = NULL, .operations = NULL},
        .function_library = library,
        .variables = NULL,
        .variable_count = 0,
        .variable_capacity = 0};
    *scope = new_scope;
    return SXML_STATUS_OK;
}

void sxml_xslt_static_scope_finalize(sxml_xslt_static_scope *scope) {
    if (scope == NULL) {
        return;
    }
    for (size_t variable_index = 0; variable_index < scope->variable_count; variable_index += 1) {
        sxml_sequence_release(scope->variables[variable_index].value);
    }
    sxml_allocator_release(scope->processor->allocator, scope->variables);
    sxml_function_library_destroy(scope->function_library);
    scope->variables = NULL;
    scope->variable_count = 0;
    scope->variable_capacity = 0;
    scope->function_library = NULL;
}

sxml_status sxml_xslt_static_scope_create(
    sxml_allocator allocator, const sxml_xslt_static_scope_configuration *configuration,
    sxml_xslt_static_scope **scope) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_xslt_static_scope_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = requirements.byte_count,
                                  .alignment = requirements.alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_static_scope *new_scope = NULL;
    status = sxml_xslt_static_scope_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
        configuration, &new_scope);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_scope->creating_allocator = allocator;
    *scope = new_scope;
    return SXML_STATUS_OK;
}

void sxml_xslt_static_scope_destroy(sxml_xslt_static_scope *scope) {
    if (scope == NULL) {
        return;
    }
    const sxml_allocator allocator = scope->creating_allocator;
    sxml_xslt_static_scope_finalize(scope);
    sxml_allocator_release(allocator, scope);
}

sxml_status sxml_xslt_static_scope_add_variable(sxml_xslt_static_scope *scope,
                                                const sxml_xslt_static_variable *variable) {
    if (scope->variable_count == scope->variable_capacity) {
        const size_t new_capacity = scope->variable_capacity < 8 ? 8 : scope->variable_capacity * 2;
        size_t byte_count = 0;
        sxml_status status =
            sxml_size_multiply(new_capacity, sizeof(static_variable_entry), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                scope->processor->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(static_variable_entry)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (scope->variable_count > 0) {
            memcpy(memory, scope->variables, scope->variable_count * sizeof(static_variable_entry));
        }
        sxml_allocator_release(scope->processor->allocator, scope->variables);
        scope->variables = memory;
        scope->variable_capacity = new_capacity;
    }
    sxml_sequence_retain(variable->value);
    scope->variables[scope->variable_count] =
        (static_variable_entry){.variable_name = variable->variable_name, .value = variable->value};
    scope->variable_count += 1;
    return SXML_STATUS_OK;
}

sxml_xslt_static_lookup sxml_xslt_static_scope_find_variable(const sxml_xslt_static_scope *scope,
                                                             sxml_name_identifier variable_name) {
    for (size_t entry_number = scope->variable_count; entry_number > 0; entry_number -= 1) {
        const static_variable_entry *entry = &scope->variables[entry_number - 1];
        if (entry->variable_name.index == variable_name.index) {
            return (sxml_xslt_static_lookup){.is_found = true, .value = entry->value};
        }
    }
    return (sxml_xslt_static_lookup){.is_found = false, .value = sxml_sequence_empty()};
}

/* ------------------------------------------------------------ Evaluation */

/* The variables in scope, each name once (the latest declaration), as
   declarations of type item()* and their bindings; both arrays owned. */
typedef struct scope_variables {
    sxml_variable_declaration *declarations;
    sxml_variable_binding *bindings;
    size_t variable_count;
} scope_variables;

static sxml_status collect_scope_variables(const sxml_xslt_static_scope *scope,
                                           scope_variables *variables) {
    *variables = (scope_variables){.declarations = NULL, .bindings = NULL, .variable_count = 0};
    if (scope->variable_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_allocator allocator = scope->processor->allocator;
    size_t declaration_bytes = 0;
    size_t binding_bytes = 0;
    sxml_status status = sxml_size_multiply(scope->variable_count,
                                            sizeof(sxml_variable_declaration), &declaration_bytes);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(scope->variable_count, sizeof(sxml_variable_binding),
                                    &binding_bytes);
    }
    void *declaration_memory = NULL;
    void *binding_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = declaration_bytes,
                                      .alignment = alignof(sxml_variable_declaration)},
            &declaration_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = binding_bytes,
                                      .alignment = alignof(sxml_variable_binding)},
            &binding_memory);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, declaration_memory);
        return status;
    }
    sxml_variable_declaration *declarations = declaration_memory;
    sxml_variable_binding *bindings = binding_memory;
    size_t count = 0;
    /* From the latest: a name already taken is hidden (9.6). */
    for (size_t entry_number = scope->variable_count; entry_number > 0; entry_number -= 1) {
        const static_variable_entry *entry = &scope->variables[entry_number - 1];
        bool is_hidden = false;
        for (size_t taken_index = 0; taken_index < count; taken_index += 1) {
            if (declarations[taken_index].variable_name.index == entry->variable_name.index) {
                is_hidden = true;
                break;
            }
        }
        if (is_hidden) {
            continue;
        }
        declarations[count] = (sxml_variable_declaration){
            .variable_name = entry->variable_name,
            .declared_type = {.item_type = {.index = SXML_TYPE_ITEM},
                              .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
        bindings[count] =
            (sxml_variable_binding){.variable_name = entry->variable_name, .value = entry->value};
        count += 1;
    }
    *variables = (scope_variables){
        .declarations = declarations, .bindings = bindings, .variable_count = count};
    return SXML_STATUS_OK;
}

static void release_scope_variables(const sxml_xslt_static_scope *scope,
                                    scope_variables *variables) {
    sxml_allocator_release(scope->processor->allocator, variables->declarations);
    sxml_allocator_release(scope->processor->allocator, variables->bindings);
    *variables = (scope_variables){.declarations = NULL, .bindings = NULL, .variable_count = 0};
}

/* An AST to lower and run in the scope. */
typedef struct static_run_request {
    /* Root XPATH; borrowed. */
    const sxml_ast *ast;
    sxml_namespace_binding_view namespace_bindings;
    sxml_byte_view static_base_uri;
    /* Variables in scope (declarations and their values). */
    sxml_variable_declaration_view declarations;
    sxml_variable_binding_view bindings;
    sxml_error_record *error_record;
} static_run_request;

/* Lowers and seals request->ast as sxml_xpath_compile does, then
   evaluates it as sxml_xpath_expression_evaluate does, with the scope's
   function table at both. */
static sxml_status run_static_ast(sxml_xslt_static_scope *scope, const static_run_request *request,
                                  sxml_sequence *value) {
    sxml_processor *processor = scope->processor;
    const sxml_xpath_static_context static_context = {
        .item_heap = processor->item_heap,
        .allocator = processor->allocator,
        .namespace_bindings = request->namespace_bindings,
        .variable_declarations = request->declarations,
        .function_table = static_function_table(scope),
        .static_base_uri = request->static_base_uri,
        /* 9.7 and 5.3.1: the applicable static namespaces only (D261). */
        .omits_predeclared_namespaces = true};
    sxml_ir_unit *unit = NULL;
    sxml_status status = sxml_xpath_lower(
        &(sxml_xpath_lowering_request){
            .ast = request->ast,
            .static_context = &static_context,
            .maximum_expression_count = 0,
            .maximum_expression_depth = sxml_processor_get_maximum_expression_depth(processor),
            .error_record = request->error_record},
        &unit);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof(diagnostic));
    status = sxml_ir_run_passes(&(sxml_ir_pass_list_request){
        .pass_request = {.unit = unit,
                         .type_registry = processor->type_registry,
                         .error_record = request->error_record,
                         .diagnostic = &diagnostic},
        .passes = sxml_ir_standard_passes,
        .verifies_after_each_pass = false});
    if (status != SXML_STATUS_OK) {
        sxml_ir_unit_release(unit);
        return status;
    }
    sxml_ir_unit_seal(unit);
    /* Global n of the unit is declaration n (sxml_xpath_lower). */
    const size_t global_count = request->declarations.variable_declaration_count;
    void *slot_memory = NULL;
    if (global_count > 0) {
        size_t byte_count = 0;
        status = sxml_size_multiply(global_count, sizeof(sxml_evaluator_slot), &byte_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                processor->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_evaluator_slot)},
                &slot_memory);
        }
    }
    sxml_evaluator_slot *slots = slot_memory;
    for (size_t global_index = 0; status == SXML_STATUS_OK && global_index < global_count;
         global_index += 1) {
        slots[global_index] =
            (sxml_evaluator_slot){.value = request->bindings.variable_bindings[global_index].value,
                                  .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
    }
    sxml_dynamic_context *dynamic_context = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_create(
            processor->allocator,
            &(sxml_evaluator_configuration){
                .item_heap = processor->item_heap,
                .allocator = processor->allocator,
                /* 9.7 leaves available documents to the implementation:
                   static expressions read none (fn:doc raises FODC0002). */
                .document_loader = {.state = NULL, .operations = NULL},
                .clock = processor->clock,
                .function_table = static_function_table(scope),
                .maximum_evaluation_depth = sxml_processor_get_maximum_evaluation_depth(processor),
                .maximum_frame_slot_count = 0,
                .maximum_evaluation_step_count =
                    sxml_processor_get_maximum_evaluation_step_count(processor)},
            &dynamic_context);
    }
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        /* 9.7: the focus is absent. */
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){
                .unit = unit,
                .has_context_value = false,
                .context_value = sxml_sequence_empty(),
                .global_values = {.slots = slots, .slot_count = global_count},
                .static_base_uri = request->static_base_uri,
                .error_record = request->error_record},
            &result);
    }
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_allocator_release(processor->allocator, slot_memory);
    sxml_ir_unit_release(unit);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *value = result;
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_static_scope_evaluate(sxml_xslt_static_scope *scope,
                                            const sxml_xslt_static_expression *expression,
                                            sxml_sequence *value) {
    if (scope == NULL || expression == NULL || value == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = scope->processor;
    const sxml_ast_build_request build_request = {
        .expression_text = expression->expression_text,
        .source_name = expression->source_name,
        .allocator = processor->allocator,
        .maximum_expression_depth = sxml_processor_get_maximum_expression_depth(processor),
        .error_record = expression->error_record};
    sxml_ast *ast = NULL;
    /* A shadow attribute is a value template whose value is its string
       (3.11.4, 5.6): the AST is that of a string template. */
    sxml_status status = expression->is_value_template
                             ? sxml_xslt_build_value_template_ast(&build_request, &ast)
                             : sxml_xpath_build_ast(&build_request, &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    scope_variables variables = {.declarations = NULL, .bindings = NULL, .variable_count = 0};
    status = collect_scope_variables(scope, &variables);
    if (status == SXML_STATUS_OK) {
        status = run_static_ast(
            scope,
            &(static_run_request){
                .ast = ast,
                .namespace_bindings = expression->namespace_bindings,
                .static_base_uri = expression->static_base_uri,
                .declarations = {.variable_declarations = variables.declarations,
                                 .variable_declaration_count = variables.variable_count},
                .bindings = {.variable_bindings = variables.bindings,
                             .variable_binding_count = variables.variable_count},
                .error_record = expression->error_record},
            value);
    }
    release_scope_variables(scope, &variables);
    sxml_ast_destroy(ast);
    return status;
}

/* The private name of the value being coerced. */
#define STATIC_COERCION_NAMESPACE_URI "urn:x-sxml:static-coercion"

sxml_status sxml_xslt_static_scope_coerce(sxml_xslt_static_scope *scope,
                                          const sxml_xslt_static_coercion *coercion,
                                          sxml_sequence *value) {
    if (scope == NULL || coercion == NULL || value == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = scope->processor;
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_name_identifier name = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        processor->name_pool, SXML_BYTE_VIEW_FROM_LITERAL(STATIC_COERCION_NAMESPACE_URI),
        &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            processor->name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("value")},
            &name);
    }
    sxml_ast *ast = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_build_ast(
            &(sxml_ast_build_request){
                .expression_text =
                    SXML_BYTE_VIEW_FROM_LITERAL("$Q{" STATIC_COERCION_NAMESPACE_URI "}value"),
                .source_name = {.bytes = NULL, .byte_count = 0},
                .allocator = processor->allocator,
                .maximum_expression_depth = 0,
                .error_record = NULL},
            &ast);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The binding is coerced to the declared type when the evaluation
       starts (sxml_evaluation_request.global_values). */
    const sxml_variable_declaration declaration = {.variable_name = name,
                                                   .declared_type = coercion->declared_type};
    const sxml_variable_binding binding = {.variable_name = name, .value = coercion->value};
    sxml_error_record record;
    memset(&record, 0, sizeof(record));
    status = run_static_ast(
        scope,
        &(static_run_request){
            .ast = ast,
            .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
            .static_base_uri = {.bytes = NULL, .byte_count = 0},
            .declarations = {.variable_declarations = &declaration,
                             .variable_declaration_count = 1},
            .bindings = {.variable_bindings = &binding, .variable_binding_count = 1},
            .error_record = &record},
        value);
    sxml_ast_destroy(ast);
    if (status == SXML_STATUS_TYPE_ERROR) {
        /* XPTY0004 from the binding is the variable's own code (9.3). */
        if (coercion->error_record != NULL) {
            *coercion->error_record = (sxml_error_record){
                .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                         .local_name = coercion->error_code,
                         .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
                .value = NULL,
                .diagnostic = {.description = SXML_BYTE_VIEW_FROM_LITERAL(
                                   "The value of a static variable or parameter does not "
                                   "match its declared type (XSLT 4.0, 9.3)"),
                               .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                            .line_number = 0,
                                            .column_number = 0,
                                            .byte_offset = 0}}};
        }
        return SXML_STATUS_TYPE_ERROR;
    }
    if (status != SXML_STATUS_OK && coercion->error_record != NULL) {
        *coercion->error_record = record;
    }
    return status;
}
