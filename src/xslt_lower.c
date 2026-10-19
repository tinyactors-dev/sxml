/*
 * Lowering XSLT 4.0 (src/xslt_lower_internal.h; docs/DECISIONS.md D103,
 * D110, D124, D142; packet 4.6): the stylesheet lowering, sequence
 * constructors, literal result elements, text nodes, value templates,
 * fallback, and the entry that lowers one template.
 *
 * Golden file: src/xquery_lower_construct.c (procedures as frames whose
 * steps ask the machine for their parts and return, D71; atoms in owned
 * memory; namespaces of an element constructor).
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_lower_internal.h"
#include "memory_internal.h"

#include "function_library_core_internal.h"
#include "ir_pass_internal.h"
#include "xslt_accumulator_internal.h"
#include "xslt_key_internal.h"
#include "xslt_value_template_internal.h"
#include "processor_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The instruction rules of every XSLT lowering file. */
static const sxml_xslt_instruction_rule_view *const instruction_rule_views[] = {
    &sxml_xslt_instruction_rules_accumulator,
    &sxml_xslt_instruction_rules_group,
    &sxml_xslt_instruction_rules_construct,
    &sxml_xslt_instruction_rules_control,
    &sxml_xslt_instruction_rules_template,
    &sxml_xslt_instruction_rules_number,
    &sxml_xslt_instruction_rules_output,
};

/* ------------------------------------------------------------ AST reads */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

static sxml_xslt_ast_kind kind_of(const sxml_xslt_lowering *lowering,
                                  sxml_xslt_ast_node_index node) {
    return node_of(lowering, node)->kind;
}

static sxml_xslt_ast_node_index next_of(const sxml_xslt_lowering *lowering,
                                        sxml_xslt_ast_node_index node) {
    return node_of(lowering, node)->next_sibling;
}

static const sxml_xslt_ast_attribute *attribute_at(const sxml_xslt_lowering *lowering,
                                                   uint32_t attribute_index) {
    return sxml_xslt_ast_get_attribute(lowering->ast,
                                       (sxml_xslt_ast_attribute_index){.value = attribute_index});
}

/* ---------------------------------------------------------------- Errors */

/* Fills the error record's code (err namespace) and location. */
static void fill_error_record(const sxml_xslt_lowering *lowering, sxml_xslt_static_error error) {
    if (lowering->error_record == NULL) {
        return;
    }
    const sxml_byte_view text = error.code_and_description;
    *lowering->error_record = (sxml_error_record){
        .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                 .local_name = {.bytes = text.bytes, .byte_count = 8},
                 .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
        .value = NULL,
        .diagnostic = {.description = {.bytes = text.bytes + 9, .byte_count = text.byte_count - 9},
                       .location = sxml_xslt_ast_get_source_location(lowering->ast, error.node)}};
}

sxml_status sxml_xslt_lowering_report_static_error(const sxml_xslt_lowering *lowering,
                                                   sxml_xslt_static_error error) {
    fill_error_record(lowering, error);
    return SXML_STATUS_STATIC_ERROR;
}

sxml_status sxml_xslt_lowering_report_unsupported(const sxml_xslt_lowering *lowering,
                                                  sxml_xslt_static_error error) {
    if (lowering->error_record != NULL) {
        lowering->error_record->diagnostic = (sxml_diagnostic){
            .description = error.code_and_description,
            .location = sxml_xslt_ast_get_source_location(lowering->ast, error.node)};
    }
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

/* ---------------------------------------------------------- IR helpers */

static const sxml_ast_node_index no_ast_node = {.value = 0};

sxml_status sxml_xslt_lowering_add_expression(sxml_xpath_lowerer *lowerer,
                                              sxml_ir_expression expression,
                                              sxml_ir_expression_index *index) {
    return sxml_xpath_lowerer_add_expression(
        lowerer,
        (sxml_xpath_lowering_expression_to_add){.node = no_ast_node, .expression = expression},
        index);
}

sxml_status sxml_xslt_lowering_bind_expression(sxml_xpath_lowerer *lowerer,
                                               const sxml_xslt_expression_to_bind *request,
                                               sxml_ir_atom *atom) {
    sxml_ir_expression_index index = {0};
    const sxml_status status =
        sxml_xslt_lowering_add_expression(lowerer, request->expression, &index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_bind_expression(
        lowerer,
        (sxml_xpath_lowering_expression_in_block){.block = request->block, .expression = index},
        atom);
}

/* The most arguments sxml_xslt_lowering_bind_library_call passes. */
#define LIBRARY_CALL_MAXIMUM_ARGUMENT_COUNT 3U

static sxml_ir_function_target target_of(const sxml_function_entry *entry) {
    switch (entry->entry_kind) {
    case SXML_FUNCTION_ENTRY_KIND_NATIVE:
        return (sxml_ir_function_target){.target_kind = SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                         .native_function = entry->native_function};
    case SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION:
        return (sxml_ir_function_target){.target_kind = SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
                                         .function = {.value = entry->ir_function_number}};
    case SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR:
    case SXML_FUNCTION_ENTRY_KIND_FUNCTION_ITEM:
    case SXML_FUNCTION_ENTRY_KIND_NONE:
        break;
    }
    return (sxml_ir_function_target){.target_kind = SXML_IR_FUNCTION_TARGET_KIND_NONE};
}

sxml_status sxml_xslt_lowering_bind_library_call(sxml_xpath_lowerer *lowerer,
                                                 const sxml_xslt_library_call *request,
                                                 sxml_ir_atom *atom) {
    const size_t argument_count = request->arguments.atom_count;
    if (argument_count > LIBRARY_CALL_MAXIMUM_ARGUMENT_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_name_identifier function_name = {.index = 0};
    const bool is_known = sxml_name_pool_find_name(
        lowerer->name_pool,
        &(sxml_name_request){.namespace_uri =
                                 sxml_well_known_namespace_identifier(request->function_namespace),
                             .local_name = request->local_name},
        &function_name);
    sxml_function_entry entry = {.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    sxml_status status = SXML_STATUS_OK;
    if (is_known) {
        status = sxml_xpath_find_function(
            lowerer->static_context->function_table,
            &(sxml_function_query){.function_name = function_name, .arity = argument_count},
            &entry);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_function_target target = target_of(&entry);
    if (target.target_kind == SXML_IR_FUNCTION_TARGET_KIND_NONE) {
        if (lowerer->error_record != NULL) {
            lowerer->error_record->diagnostic.description = SXML_BYTE_VIEW_FROM_LITERAL(
                "the function table lacks a function XSLT lowering calls (fn:string-join, "
                "fn:replace, fn:jtree, fn:jvalue, array:of-members, map:merge)");
        }
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    sxml_ir_atom atoms[LIBRARY_CALL_MAXIMUM_ARGUMENT_COUNT] = {{0}, {0}, {0}};
    for (size_t argument_index = 0; status == SXML_STATUS_OK && argument_index < argument_count;
         argument_index += 1) {
        atoms[argument_index] = request->arguments.atoms[argument_index];
        if (argument_index < entry.parameter_types.sequence_type_count) {
            status = sxml_xpath_lowerer_coerce_atom(
                lowerer,
                (sxml_xpath_lowering_coercion_request){
                    .block = request->block,
                    .node = no_ast_node,
                    .value = request->arguments.atoms[argument_index],
                    .sequence_type = entry.parameter_types.sequence_types[argument_index],
                    .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
                &atoms[argument_index]);
        }
    }
    sxml_ir_atom_list arguments = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit, (sxml_ir_atom_view){.atoms = atoms, .atom_count = argument_count},
            &arguments);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_bind_expression(
        lowerer,
        &(sxml_xslt_expression_to_bind){
            .block = request->block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                           .static_call = {.target = target, .arguments = arguments}}},
        atom);
}

/* The most mappings of one sxml_xslt_lowering_bind_mapped_expression. */
#define MAPPED_EXPRESSION_MAXIMUM_MAPPING_COUNT 4U

/* One catch clause of a mapped expression: its name test and handler. */
static sxml_status add_mapping_clause(sxml_xpath_lowerer *lowerer,
                                      const sxml_xslt_error_mapping *mapping,
                                      sxml_ir_catch_clause *clause) {
    sxml_name_identifier caught = {.index = 0};
    sxml_status status = sxml_name_pool_intern_name(
        lowerer->name_pool,
        &(sxml_name_request){.namespace_uri =
                                 sxml_well_known_namespace_identifier(SXML_NAMESPACE_ERRORS),
                             .local_name = mapping->caught_code},
        &caught);
    const sxml_name_test test = {.name_test_kind = SXML_NAME_TEST_KIND_EXPANDED_NAME,
                                 .expanded_name = caught,
                                 .namespace_uri = {.index = 0},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_name_tests(
            lowerer->unit, (sxml_ir_name_test_view){.name_tests = &test, .name_test_count = 1},
            &clause->error_tests);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(lowerer, &clause->error_details);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_raise(
            lowerer,
            (sxml_xpath_lowering_raise_request){.node = no_ast_node,
                                                .error_status = mapping->raised_status,
                                                .code_local_name = mapping->raised_code,
                                                .description = mapping->description},
            &clause->handler);
    }
    return status;
}

/* The circularity errors a guarded match raises again. */
static const sxml_xslt_error_mapping circularity_mappings[] = {
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0640"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE0640"),
     .description = SXML_BYTE_VIEW_INITIALIZER(
         "A pattern depends on itself through a key (XSLT 4.0, 9.11, 6.5)")},
    {.caught_code = SXML_BYTE_VIEW_INITIALIZER("XTDE3400"),
     .raised_status = SXML_STATUS_DYNAMIC_ERROR,
     .raised_code = SXML_BYTE_VIEW_INITIALIZER("XTDE3400"),
     .description = SXML_BYTE_VIEW_INITIALIZER("A pattern depends on itself through an "
                                               "accumulator (XSLT 4.0, 19.6, 6.5)")},
};

#define CIRCULARITY_MAPPING_COUNT (sizeof(circularity_mappings) / sizeof(circularity_mappings[0]))

sxml_status sxml_xslt_lowering_make_guarded_match(sxml_xpath_lowerer *lowerer,
                                                  sxml_ir_expression_index body,
                                                  sxml_ir_expression *guarded) {
    sxml_ir_catch_clause clauses[CIRCULARITY_MAPPING_COUNT + 1];
    memset(clauses, 0, sizeof(clauses));
    sxml_status status = SXML_STATUS_OK;
    for (size_t mapping_index = 0;
         status == SXML_STATUS_OK && mapping_index < CIRCULARITY_MAPPING_COUNT;
         mapping_index += 1) {
        status = add_mapping_clause(lowerer, &circularity_mappings[mapping_index],
                                    &clauses[mapping_index]);
    }
    sxml_ir_catch_clause *any_error = &clauses[CIRCULARITY_MAPPING_COUNT];
    const sxml_name_test any_name = {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME,
                                     .expanded_name = {.index = 0},
                                     .namespace_uri = {.index = 0},
                                     .local_name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_name_tests(
            lowerer->unit, (sxml_ir_name_test_view){.name_tests = &any_name, .name_test_count = 1},
            &any_error->error_tests);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_slot(lowerer, &any_error->error_details);
    }
    sxml_ir_atom false_atom = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(
            lowerer,
            sxml_function_library_make_boolean((sxml_boolean_result){.is_true = false}).content,
            &false_atom);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                               .atom = false_atom}},
            &any_error->handler);
    }
    sxml_ir_catch_clause_list clause_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_catch_clauses(
            lowerer->unit,
            (sxml_ir_catch_clause_view){.catch_clauses = clauses,
                                        .catch_clause_count = CIRCULARITY_MAPPING_COUNT + 1},
            &clause_list);
    }
    if (status == SXML_STATUS_OK) {
        *guarded = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH,
                                        .try_catch = {.body = body,
                                                      .catch_clauses = clause_list,
                                                      .finally_body = {.value = 0}}};
    }
    return status;
}

/* ------------------------------------------------------------ Kept ASTs */

sxml_status sxml_xslt_lowering_keep_ast(const sxml_xslt_lowering *lowering, sxml_ast *ast) {
    sxml_xslt_kept_asts *kept = lowering->kept_asts;
    if (kept == NULL) {
        sxml_ast_destroy(ast);
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (kept->ast_count == kept->ast_capacity) {
        const size_t capacity = kept->ast_capacity == 0 ? 8 : kept->ast_capacity * 2;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, sizeof(sxml_ast *), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                kept->allocator,
                (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(sxml_ast *)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            sxml_ast_destroy(ast);
            return status;
        }
        if (kept->ast_count > 0) {
            memcpy(memory, (const void *)kept->asts, kept->ast_count * sizeof(sxml_ast *));
        }
        if (kept->asts != NULL) {
            sxml_allocator_release(kept->allocator, (void *)kept->asts);
        }
        kept->asts = (sxml_ast **)memory;
        kept->ast_capacity = capacity;
    }
    kept->asts[kept->ast_count] = ast;
    kept->ast_count += 1;
    return SXML_STATUS_OK;
}

void sxml_xslt_release_kept_asts(sxml_xslt_kept_asts *kept_asts) {
    for (size_t ast_index = 0; ast_index < kept_asts->ast_count; ast_index += 1) {
        sxml_ast_destroy(kept_asts->asts[ast_index]);
    }
    if (kept_asts->asts != NULL) {
        sxml_allocator_release(kept_asts->allocator, (void *)kept_asts->asts);
    }
    kept_asts->asts = NULL;
    kept_asts->ast_count = 0;
    kept_asts->ast_capacity = 0;
}

sxml_status sxml_xslt_lowering_parse_attribute(const sxml_xslt_lowering *lowering,
                                              sxml_xslt_attribute_parse request, sxml_ast **ast) {
    *ast = NULL;
    const sxml_xslt_ast_attribute *attribute =
        sxml_xslt_ast_get_attribute(lowering->ast, request.attribute);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_TEXT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xslt_ast_module *module =
        sxml_xslt_ast_get_module(lowering->ast, node_of(lowering, attribute->node)->module);
    const sxml_ast_build_request build = {
        .expression_text = sxml_xslt_ast_get_text(lowering->ast, attribute->value.payload.text),
        /* Module URIs never move (D102), so the AST may borrow it. */
        .source_name = module->module_uri,
        .allocator = lowering->lowerer->allocator,
        /* The lowerer bounds the depth it lowers (maximum_depth). */
        .maximum_expression_depth = lowering->lowerer->maximum_depth,
        .error_record = lowering->error_record};
    sxml_ast *made = NULL;
    const sxml_status status = request.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE
                                   ? sxml_xslt_build_value_template_ast(&build, &made)
                                   : sxml_xpath_build_ast(&build, &made);
    if (status != SXML_STATUS_OK) {
        /* The error inside the text, placed in the module as the reader
           places it (D169). */
        if (lowering->error_record != NULL && sxml_status_has_error_record(status)) {
            sxml_source_location *location = &lowering->error_record->diagnostic.location;
            const sxml_ast_source_span span = sxml_ast_moved_source_span(
                (sxml_ast_source_span){.byte_offset = location->byte_offset > UINT32_MAX
                                                          ? UINT32_MAX
                                                          : (uint32_t)location->byte_offset,
                                       .byte_count = 0,
                                       .line_number = location->line_number,
                                       .column_number = location->column_number},
                attribute->source_span);
            *location = (sxml_source_location){.source_name = module->module_uri,
                                               .line_number = span.line_number,
                                               .column_number = span.column_number,
                                               .byte_offset = span.byte_offset};
        }
        return status;
    }
    /* Its nodes are placed in the module, so a dynamic error points where
       a static one does (D169). */
    sxml_ast_move_source_spans(made, attribute->source_span);
    *ast = made;
    return sxml_xslt_lowering_keep_ast(lowering, made);
}

sxml_status sxml_xslt_lowering_bind_mapped_expression(sxml_xpath_lowerer *lowerer,
                                                      const sxml_xslt_mapped_expression *request,
                                                      sxml_ir_atom *atom) {
    const size_t mapping_count = request->mappings.mapping_count;
    if (mapping_count == 0 || mapping_count > MAPPED_EXPRESSION_MAXIMUM_MAPPING_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ir_expression_index body = {0};
    sxml_status status = sxml_xpath_lowerer_add_expression(
        lowerer,
        (sxml_xpath_lowering_expression_to_add){.node = no_ast_node,
                                                .expression = request->expression},
        &body);
    sxml_ir_catch_clause clauses[MAPPED_EXPRESSION_MAXIMUM_MAPPING_COUNT];
    memset(clauses, 0, sizeof(clauses));
    for (size_t mapping_index = 0; status == SXML_STATUS_OK && mapping_index < mapping_count;
         mapping_index += 1) {
        status = add_mapping_clause(lowerer, &request->mappings.mappings[mapping_index],
                                    &clauses[mapping_index]);
    }
    sxml_ir_catch_clause_list clause_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_catch_clauses(
            lowerer->unit,
            (sxml_ir_catch_clause_view){.catch_clauses = clauses,
                                        .catch_clause_count = mapping_count},
            &clause_list);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_bind_expression(
        lowerer,
        &(sxml_xslt_expression_to_bind){
            .block = request->block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_TRY_CATCH,
                           .try_catch = {.body = body,
                                         .catch_clauses = clause_list,
                                         .finally_body = {.value = 0}}}},
        atom);
}

sxml_status sxml_xslt_lowering_bind_simple_content(sxml_xpath_lowerer *lowerer,
                                                   sxml_xslt_simple_content_request request,
                                                   sxml_ir_atom *atom) {
    /* 5.7.2, steps 1 and 2: adjacent text nodes merged (an internal row
       whose argument needs no coercion), then steps 3 to 5: atomized, cast
       to strings and joined by fn:string-join's coercion and body. */
    sxml_ir_atom_list arguments = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        lowerer->unit, (sxml_ir_atom_view){.atoms = &request.value, .atom_count = 1}, &arguments);
    sxml_ir_atom merged = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_expression(
            lowerer,
            &(sxml_xslt_expression_to_bind){
                .block = request.block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                               .static_call = {.target = {.target_kind =
                                                              SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                          .native_function =
                                                              &sxml_xslt_merge_text_nodes_function},
                                               .arguments = arguments}}},
            &merged);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_atom join_arguments[2] = {merged, request.separator};
    return sxml_xslt_lowering_bind_library_call(
        lowerer,
        &(sxml_xslt_library_call){.block = request.block,
                                  .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                  .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string-join"),
                                  .arguments = {.atoms = join_arguments, .atom_count = 2}},
        atom);
}

/* ------------------------------------------------- Declared types */

uint32_t sxml_xslt_lowering_module_of_node(const sxml_xslt_lowering *lowering,
                                           sxml_xslt_ast_node_index node) {
    const uint32_t context = sxml_xslt_ast_get_node(lowering->ast, node)->static_context.value;
    return context == 0 || context >= lowering->static_context_count
               ? 0
               : lowering->static_contexts[context].ir_module_number;
}

const sxml_xpath_static_context *
sxml_xslt_lowering_expression_context(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_expression_index expression) {
    const uint32_t context =
        sxml_xslt_ast_get_expression(lowering->ast, expression)->static_context.value;
    if (context == 0 || context >= lowering->static_context_count) {
        return NULL;
    }
    return &lowering->static_contexts[context];
}

sxml_status sxml_xslt_lowering_resolve_declared_type(const sxml_xslt_lowering *lowering,
                                                     sxml_xslt_ast_node_index node,
                                                     sxml_xslt_declared_type *type) {
    *type = (sxml_xslt_declared_type){
        .has_type = false,
        .sequence_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                          .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    const sxml_xslt_ast_attribute *as_attribute = sxml_xslt_ast_get_attribute(
        lowering->ast,
        sxml_xslt_ast_find_attribute(lowering->ast, (sxml_xslt_ast_attribute_query){
                                                        .node = node,
                                                        .kind = SXML_XSLT_AST_ATTRIBUTE_AS}));
    if (as_attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, as_attribute->value.payload.expression);
    const sxml_xpath_static_context *context =
        sxml_xslt_lowering_expression_context(lowering, as_attribute->value.payload.expression);
    if (expression->ast == NULL || context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    type->has_type = true;
    return sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = expression->ast,
                                   .type_node = expression->ast->root,
                                   .static_context = context,
                                   .error_record = lowering->error_record},
        &type->sequence_type);
}

/* ------------------------------------------- Backwards compatibility */

bool sxml_xslt_lowering_has_xslt_1_behaviour(const sxml_xslt_lowering *lowering,
                                             sxml_xslt_ast_node_index node) {
    const uint32_t context = node_of(lowering, node)->static_context.value;
    return context != 0 && context < lowering->static_context_count &&
           lowering->static_contexts[context].has_xpath_1_compatibility_mode;
}

sxml_status sxml_xslt_lowering_bind_first_atom(sxml_xpath_lowerer *lowerer,
                                               sxml_xpath_lowering_block *block,
                                               sxml_ir_atom *atom) {
    const sxml_ir_atom value = *atom;
    sxml_ir_atom atoms = {0};
    const sxml_status status = sxml_xslt_lowering_bind_library_call(
        lowerer,
        &(sxml_xslt_library_call){.block = block,
                                  .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                  .local_name = SXML_BYTE_VIEW_FROM_LITERAL("data"),
                                  .arguments = {.atoms = &value, .atom_count = 1}},
        &atoms);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_bind_library_call(
        lowerer,
        &(sxml_xslt_library_call){.block = block,
                                  .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                  .local_name = SXML_BYTE_VIEW_FROM_LITERAL("head"),
                                  .arguments = {.atoms = &atoms, .atom_count = 1}},
        atom);
}

/* ------------------------------------------------ Boolean attributes */

sxml_xslt_boolean_text sxml_xslt_lowering_read_boolean_text(sxml_byte_view text) {
    const sxml_byte_view trimmed = sxml_xml_text_trim_whitespace(text);
    const bool is_yes = sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("yes")) ||
                        sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("true")) ||
                        sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("1"));
    const bool is_no = sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("no")) ||
                       sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("false")) ||
                       sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("0"));
    return (sxml_xslt_boolean_text){.is_valid = is_yes || is_no, .is_yes = is_yes};
}

sxml_status sxml_xslt_lowering_bind_boolean_value(sxml_xpath_lowerer *lowerer,
                                                  sxml_xslt_boolean_value request,
                                                  sxml_ir_atom *atom) {
    sxml_ir_atom normalized = {0};
    sxml_status status = sxml_xslt_lowering_bind_library_call(
        lowerer,
        &(sxml_xslt_library_call){.block = request.block,
                                  .function_namespace = SXML_NAMESPACE_FUNCTIONS,
                                  .local_name = SXML_BYTE_VIEW_FROM_LITERAL("normalize-space"),
                                  .arguments = {.atoms = &request.value, .atom_count = 1}},
        &normalized);
    static const sxml_byte_view words[3] = {SXML_BYTE_VIEW_INITIALIZER("yes"),
                                            SXML_BYTE_VIEW_INITIALIZER("true"),
                                            SXML_BYTE_VIEW_INITIALIZER("1")};
    sxml_ir_atom word_atoms[3] = {{0}, {0}, {0}};
    for (size_t word_index = 0; status == SXML_STATUS_OK && word_index < 3; word_index += 1) {
        status =
            sxml_xpath_lowerer_add_string_constant(lowerer, words[word_index], &word_atoms[word_index]);
    }
    sxml_ir_atom_list word_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit, (sxml_ir_atom_view){.atoms = word_atoms, .atom_count = 3}, &word_list);
    }
    sxml_ir_atom yes_words = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_expression(
            lowerer,
            &(sxml_xslt_expression_to_bind){
                .block = request.block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                               .sequence = word_list}},
            &yes_words);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_bind_expression(
        lowerer,
        &(sxml_xslt_expression_to_bind){
            .block = request.block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                           .binary = {.operator= SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL,
                                      .left_operand = normalized,
                                      .right_operand = yes_words}}},
        atom);
}

/* ------------------------------------------------- Namespaces and aliases */

sxml_status
sxml_xslt_lowering_get_element_namespaces(const sxml_xslt_lowering *lowering,
                                          sxml_xslt_ast_node_index node,
                                          sxml_namespace_binding_view *namespace_bindings) {
    const sxml_xslt_ast_node *entry = node_of(lowering, node);
    *namespace_bindings =
        (sxml_namespace_binding_view){.namespace_bindings = NULL, .namespace_binding_count = 0};
    if (entry->kind == SXML_XSLT_AST_KIND_TEXT_NODE) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_module *module = sxml_xslt_ast_get_module(lowering->ast, entry->module);
    if (module->tree == NULL) {
        return SXML_STATUS_OK;
    }
    return sxml_node_get_in_scope_namespaces((sxml_node_reference){.tree = module->tree,
                                                                   .node_index = entry->source_node,
                                                                   .namespace_number = 0},
                                             namespace_bindings);
}

const sxml_xslt_namespace_alias *sxml_xslt_lowering_find_alias(const sxml_xslt_lowering *lowering,
                                                               sxml_namespace_uri_identifier uri,
                                                               sxml_xslt_ast_node_index node) {
    const uint32_t package = sxml_xslt_package_of_node(lowering, node);
    for (size_t alias_index = 0; alias_index < lowering->namespace_alias_count; alias_index += 1) {
        if (lowering->namespace_aliases[alias_index].stylesheet_uri.index == uri.index &&
            lowering->namespace_aliases[alias_index].package == package) {
            return &lowering->namespace_aliases[alias_index];
        }
    }
    return NULL;
}

/* ------------------------------------------------------ Embedded parts */

sxml_status sxml_xslt_lowering_call_expression(sxml_xpath_lowering_machine *machine,
                                               const sxml_xslt_expression_call *call) {
    const sxml_xslt_lowering *lowering = call->lowering;
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, call->expression);
    if (expression->ast == NULL || expression->static_context.value == 0 ||
        expression->static_context.value >= lowering->static_context_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_xpath_lowering_call_embedded(
        machine, &(sxml_xpath_lowering_embedded_call){
                     .embedding = {.ast = expression->ast,
                                   .node = {.value = 0},
                                   .static_context =
                                       &lowering->static_contexts[expression->static_context.value],
                                   .focus = call->focus},
                     .block = call->block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = call->atom,
                                     .atom_block = call->block,
                                     .focused = NULL}});
}

/* A value template (5.6): its STRING_TEMPLATE's parts in order. */
typedef struct value_template_locals {
    const sxml_xslt_lowering *lowering;
    /* The template's AST (an expression of the stylesheet AST or a kept
       AST) and the static context it is lowered in. */
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
    /* The next part of the body, and the enclosed expression's value. */
    sxml_ast_node_index part;
    sxml_ir_atom value;
    /* The string so far. */
    sxml_ir_atom result;
    bool has_result;
    /* An attribute value template of an element with XSLT 1.0 behaviour
       (5.6.1): each enclosed expression gives its first atomized item.
       Not for text value templates, which XSLT 1.0 did not have. */
    bool takes_first_items;
    /* A text value template of such an element: static_context points at
       context_without_compatibility, the element's context with XPath
       1.0 compatibility mode off (3.8.1; D206). */
    bool is_text_template;
    sxml_xpath_static_context context_without_compatibility;
} value_template_locals;

/* Appends part_atom to the template's string with "||". */
static sxml_status append_template_part(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame, sxml_ir_atom part_atom) {
    value_template_locals *locals = frame->locals;
    if (!locals->has_result) {
        locals->result = part_atom;
        locals->has_result = true;
        return SXML_STATUS_OK;
    }
    return sxml_xslt_lowering_bind_expression(
        machine->state,
        &(sxml_xslt_expression_to_bind){
            .block = frame->block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                           .binary = {.operator= SXML_IR_BINARY_OPERATOR_CONCATENATE,
                                      .left_operand = locals->result,
                                      .right_operand = part_atom}}},
        &locals->result);
}

/* After an enclosed expression: its simple content, appended. */
static sxml_status append_enclosed_part(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    value_template_locals *locals = frame->locals;
    sxml_ir_atom separator = {0};
    sxml_status status = sxml_xpath_lowerer_add_string_constant(
        lowerer, SXML_BYTE_VIEW_FROM_LITERAL(" "), &separator);
    if (status == SXML_STATUS_OK && locals->takes_first_items) {
        status = sxml_xslt_lowering_bind_first_atom(lowerer, frame->block, &locals->value);
    }
    sxml_ir_atom content = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_simple_content(
            lowerer,
            (sxml_xslt_simple_content_request){
                .block = frame->block, .value = locals->value, .separator = separator},
            &content);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return append_template_part(machine, frame, content);
}

static sxml_status continue_value_template(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    value_template_locals *locals = frame->locals;
    const sxml_ast *ast = locals->ast;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        frame->step = 1;
        if (ast == NULL || locals->static_context == NULL) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        if (locals->is_text_template && locals->static_context->has_xpath_1_compatibility_mode) {
            /* XSLT 4.0, 3.8.1: the expressions of a text value template
               are evaluated with XPath 1.0 compatibility mode false, even
               in an element with XSLT 1.0 behaviour. The frame's locals
               stay in place until the frame is popped. */
            locals->context_without_compatibility = *locals->static_context;
            locals->context_without_compatibility.has_xpath_1_compatibility_mode = false;
            locals->static_context = &locals->context_without_compatibility;
        }
        const sxml_ast_node_index body = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = ast->root, .field = SXML_AST_FIELD_BODY});
        locals->part = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = body, .field = SXML_AST_FIELD_PART});
    } else {
        status = append_enclosed_part(machine, frame);
    }
    while (status == SXML_STATUS_OK && locals->part.value != 0) {
        const sxml_ast_node_index part = locals->part;
        locals->part = sxml_ast_find_next_with_same_field(ast, part);
        const sxml_ast_node *part_node = sxml_ast_get_node(ast, part);
        if (part_node->kind == SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART) {
            sxml_ir_atom text = {0};
            status = sxml_xpath_lowerer_add_string_constant(
                lowerer, sxml_ast_get_text(ast, part_node->payload.text), &text);
            if (status == SXML_STATUS_OK) {
                status = append_template_part(machine, frame, text);
            }
            continue;
        }
        const sxml_ast_node_index enclosed = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = part, .field = SXML_AST_FIELD_EXPRESSION});
        if (enclosed.value == 0) {
            /* A variable part of whitespace and comments only: "" (5.6). */
            continue;
        }
        return sxml_xpath_lowering_call_embedded(
            machine,
            &(sxml_xpath_lowering_embedded_call){
                .embedding = {.ast = ast,
                              .node = enclosed,
                              .static_context = locals->static_context,
                              .focus = NULL},
                .block = frame->block,
                .is_body = false,
                .destination = {.expression = NULL,
                                .atom = &locals->value,
                                .atom_block = frame->block,
                                .focused = NULL}});
    }
    if (status == SXML_STATUS_OK && !locals->has_result) {
        status = sxml_xpath_lowerer_add_string_constant(
            lowerer, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &locals->result);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                               .atom = locals->result}},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

static const sxml_xpath_lowering_procedure value_template_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt value template"),
    .step = continue_value_template,
    .locals_size = sizeof(value_template_locals)};

/* Pushes the value template procedure for the template ast. */
typedef struct value_template_push {
    const sxml_xslt_lowering *lowering;
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
    sxml_xpath_lowering_block *block;
    sxml_ir_atom *atom;
    bool is_text_template;
} value_template_push;

static sxml_status push_value_template(sxml_xpath_lowering_machine *machine,
                                       const value_template_push *call) {
    const value_template_locals initial = {
        .lowering = call->lowering,
        .ast = call->ast,
        .static_context = call->static_context,
        .takes_first_items = !call->is_text_template && call->static_context != NULL &&
                             call->static_context->has_xpath_1_compatibility_mode,
        .is_text_template = call->is_text_template};
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = &value_template_procedure,
            .node = no_ast_node,
            .block = call->block,
            .is_body = false,
            .destination = {.expression = NULL,
                            .atom = call->atom,
                            .atom_block = call->block,
                            .focused = NULL},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof(initial)}},
        &frame);
}

sxml_status sxml_xslt_lowering_call_value_template(sxml_xpath_lowering_machine *machine,
                                                   const sxml_xslt_expression_call *call) {
    return push_value_template(
        machine, &(value_template_push){
                     .lowering = call->lowering,
                     .ast = sxml_xslt_ast_get_expression(call->lowering->ast, call->expression)->ast,
                     .static_context =
                         sxml_xslt_lowering_expression_context(call->lowering, call->expression),
                     .block = call->block,
                     .atom = call->atom});
}

sxml_status sxml_xslt_lowering_call_parsed(sxml_xpath_lowering_machine *machine,
                                           const sxml_xslt_parsed_call *call) {
    const sxml_xslt_lowering *lowering = call->lowering;
    const uint32_t context = node_of(lowering, call->node)->static_context.value;
    if (call->ast == NULL || context == 0 || context >= lowering->static_context_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xpath_static_context *static_context = &lowering->static_contexts[context];
    if (call->kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
        return push_value_template(machine, &(value_template_push){.lowering = lowering,
                                                                   .ast = call->ast,
                                                                   .static_context = static_context,
                                                                   .block = call->block,
                                                                   .atom = call->atom});
    }
    return sxml_xpath_lowering_call_embedded(
        machine, &(sxml_xpath_lowering_embedded_call){
                     .embedding = {.ast = call->ast,
                                   .node = {.value = 0},
                                   .static_context = static_context,
                                   .focus = NULL},
                     .block = call->block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = call->atom,
                                     .atom_block = call->block,
                                     .focused = NULL}});
}

/* --------------------------------------------------------- Text nodes */

/* A text node of a sequence constructor (11.4.1) whose content is a text
   value template (5.6.2): a text constructor of the template's string. */
typedef struct text_template_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom value;
} text_template_locals;

static sxml_status continue_text_template(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    text_template_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_xslt_lowering *lowering = locals->head.lowering;
        const sxml_xslt_ast_expression_index expression =
            node_of(lowering, locals->head.node)->content.payload.expression;
        /* 5.6.1: a text value template keeps every item, even with XSLT
           1.0 behaviour; 3.8.1: its expressions are lowered without XPath
           1.0 compatibility mode (continue_value_template, D206). */
        return push_value_template(
            machine,
            &(value_template_push){
                .lowering = lowering,
                .ast = sxml_xslt_ast_get_expression(lowering->ast, expression)->ast,
                .static_context = sxml_xslt_lowering_expression_context(lowering, expression),
                .block = frame->block,
                .atom = &locals->value,
                .is_text_template = true});
    }
    sxml_ir_expression_index text = {0};
    const sxml_status status = sxml_xpath_lowerer_add_expression(
        machine->state,
        (sxml_xpath_lowering_expression_to_add){
            .node = no_ast_node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                           .node_construction = {.value = locals->value}}},
        &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, text);
}

static const sxml_xpath_lowering_procedure text_template_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt text value template"),
    .step = continue_text_template,
    .locals_size = sizeof(text_template_locals)};

/* A text node with fixed text: a text constructor of a constant, bound in
   block. */
typedef struct literal_text_request {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_ast_node_index node;
    sxml_xpath_lowering_block *block;
} literal_text_request;

static sxml_status bind_literal_text(sxml_xpath_lowerer *lowerer, literal_text_request request,
                                     sxml_ir_atom *atom) {
    sxml_ir_atom text = {0};
    const sxml_status status = sxml_xpath_lowerer_add_string_constant(
        lowerer,
        sxml_xslt_ast_get_text(request.lowering->ast,
                               node_of(request.lowering, request.node)->content.payload.text),
        &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_bind_expression(
        lowerer,
        &(sxml_xslt_expression_to_bind){
            .block = request.block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
                           .node_construction = {.value = text}}},
        atom);
}

/* ------------------------------------------------ Literal result elements */

/* A QName constant that keeps its prefix. */
typedef struct qname_parts {
    sxml_name_identifier expanded_name;
    sxml_namespace_prefix_identifier namespace_prefix;
} qname_parts;

static sxml_status add_qname_constant(sxml_xpath_lowerer *lowerer, qname_parts parts,
                                      sxml_ir_atom *atom) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        lowerer->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = parts.expanded_name,
                                                  .namespace_prefix = parts.namespace_prefix}}}},
        &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_item_constant(lowerer, item, atom);
}

/* The name a literal result element or attribute has in the result: in
   the result URI and with the result prefix of an alias of its namespace
   (11.1.4). Attributes in no namespace are never aliased. */
typedef struct result_name_request {
    const sxml_xslt_lowering *lowering;
    /* The literal result element (its package's aliases apply). */
    sxml_xslt_ast_node_index node;
    qname_parts written;
    bool is_attribute;
} result_name_request;

static sxml_status make_result_name(sxml_xpath_lowerer *lowerer, const result_name_request *request,
                                    qname_parts *result) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status =
        sxml_name_pool_get_name_parts(lowerer->name_pool, request->written.expanded_name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = request->written;
    if (request->is_attribute && parts.namespace_uri.index == SXML_NAMESPACE_NONE) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_namespace_alias *alias =
        sxml_xslt_lowering_find_alias(request->lowering, parts.namespace_uri, request->node);
    if (alias == NULL) {
        return SXML_STATUS_OK;
    }
    status = sxml_name_pool_intern_name(
        lowerer->name_pool,
        &(sxml_name_request){.namespace_uri = alias->result_binding.namespace_uri,
                             .local_name = parts.local_name},
        &result->expanded_name);
    /* An attribute in a namespace needs a prefix; the evaluator picks one
       when the alias gives "#default". */
    result->namespace_prefix = alias->result_binding.namespace_uri.index == SXML_NAMESPACE_NONE
                                   ? (sxml_namespace_prefix_identifier){.index = 0}
                                   : alias->result_binding.namespace_prefix;
    return status;
}

static bool uri_is_listed(sxml_xslt_ast_namespace_uri_view uris,
                          sxml_namespace_uri_identifier uri) {
    for (size_t uri_index = 0; uri_index < uris.namespace_uri_count; uri_index += 1) {
        if (uris.namespace_uris[uri_index].index == uri.index) {
            return true;
        }
    }
    return false;
}

static bool uri_is_alias_result(const sxml_xslt_lowering *lowering,
                                sxml_namespace_uri_identifier uri, sxml_xslt_ast_node_index node) {
    const uint32_t package = sxml_xslt_package_of_node(lowering, node);
    for (size_t alias_index = 0; alias_index < lowering->namespace_alias_count; alias_index += 1) {
        if (lowering->namespace_aliases[alias_index].result_binding.namespace_uri.index ==
                uri.index &&
            lowering->namespace_aliases[alias_index].package == package) {
            return true;
        }
    }
    return false;
}

/* "<a ...>...</a>" in a sequence constructor (11.1). */
typedef struct literal_result_locals {
    sxml_xslt_instruction_locals head;
    sxml_ir_atom name;
    sxml_ir_namespace_binding_list namespace_bindings;
    uint32_t construction_flags;
    /* The next attribute (offset in the node's list) to look at. */
    uint32_t attribute_offset;
    /* An attribute whose value is being lowered. */
    bool has_pending_attribute;
    sxml_ir_atom pending_name;
    sxml_ir_atom pending_value;
    /* One atom per attribute, then the sequence constructor's (owned
       memory 0). */
    sxml_ir_atom *atoms;
    size_t atom_count;
    /* The block of the content: the attributes and the sequence
       constructor are computed inside the element constructor, so the
       atoms' LETs are content LETs and their values stream into it
       (D166). */
    sxml_xpath_lowering_block content_block;
    /* The content block's open LET before the atom being lowered. */
    sxml_ir_expression_index open_let_before;
    /* False once an atom could not be marked as a content LET. */
    bool stops_marking;
} literal_result_locals;

/* Marks the LET of the atom just lowered (atoms[atom_count - 1]) as a
   content LET while the atoms before it were marked (D166). */
static sxml_status mark_literal_result_atom(sxml_xpath_lowerer *lowerer,
                                            literal_result_locals *locals) {
    if (locals->stops_marking) {
        return SXML_STATUS_OK;
    }
    sxml_xpath_lowering_content_mark mark = {.goes_on = true};
    const sxml_status status = sxml_xpath_lowerer_mark_content_let(
        lowerer,
        (sxml_xpath_lowering_content_item){.block = &locals->content_block,
                                           .open_let_before = locals->open_let_before,
                                           .atom = locals->atoms[locals->atom_count - 1]},
        &mark);
    locals->stops_marking = !mark.goes_on;
    return status;
}

/* The namespace nodes the element gets (11.1.3, 11.1.4): its in-scope
   namespaces in the stylesheet, without xml, the excluded namespaces
   (unless an alias outputs them) and the literal URIs of aliases. Owned
   memory 1 holds them while they are copied into the unit. */
static sxml_status add_element_namespaces(sxml_xpath_lowering_machine *machine,
                                          sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    literal_result_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_namespace_binding_view in_scope = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status =
        sxml_xslt_lowering_get_element_namespaces(lowering, locals->head.node, &in_scope);
    if (status != SXML_STATUS_OK || in_scope.namespace_binding_count == 0) {
        return status;
    }
    status = sxml_xpath_lowering_allocate_owned(
        machine, frame,
        (sxml_xpath_lowering_owned_request){
            .memory_index = 1,
            .scratch = {.element_count = in_scope.namespace_binding_count,
                        .element_size = sizeof(sxml_namespace_binding),
                        .element_alignment = alignof(sxml_namespace_binding)}});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_namespace_binding *kept = frame->owned_memory[1];
    const sxml_xslt_ast_static_context *context = sxml_xslt_ast_get_static_context(
        lowering->ast, node_of(lowering, locals->head.node)->static_context);
    const sxml_xslt_ast_namespace_uri_view excluded =
        sxml_xslt_ast_get_namespace_uris(lowering->ast, context->excluded_namespaces);
    size_t kept_count = 0;
    for (size_t binding_index = 0; binding_index < in_scope.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = in_scope.namespace_bindings[binding_index];
        if (binding.namespace_uri.index == SXML_NAMESPACE_XML ||
            sxml_xslt_lowering_find_alias(lowering, binding.namespace_uri, locals->head.node) !=
                NULL) {
            continue;
        }
        const bool is_excluded = binding.namespace_uri.index == SXML_NAMESPACE_XSLT ||
                                 uri_is_listed(excluded, binding.namespace_uri);
        if (is_excluded &&
            !uri_is_alias_result(lowering, binding.namespace_uri, locals->head.node)) {
            continue;
        }
        kept[kept_count] = binding;
        kept_count += 1;
    }
    if (kept_count == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_ir_unit_add_namespace_bindings(
        lowerer->unit,
        (sxml_namespace_binding_view){.namespace_bindings = kept,
                                      .namespace_binding_count = kept_count},
        &locals->namespace_bindings);
}

/* The first step: name, namespaces, flags and the atom array. */
static sxml_status start_literal_result(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    literal_result_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_node *element = node_of(lowering, locals->head.node);
    qname_parts name = {.expanded_name = {0}, .namespace_prefix = {0}};
    sxml_status status = make_result_name(
        lowerer,
        &(result_name_request){.lowering = lowering,
                               .node = locals->head.node,
                               .written = {.expanded_name = element->element_name,
                                           .namespace_prefix = element->namespace_prefix},
                               .is_attribute = false},
        &name);
    if (status == SXML_STATUS_OK) {
        status = add_qname_constant(lowerer, name, &locals->name);
    }
    if (status == SXML_STATUS_OK) {
        status = add_element_namespaces(machine, frame);
    }
    locals->construction_flags = (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES;
    size_t attribute_count = 0;
    bool has_attribute_sets = false;
    for (uint32_t offset = 0;
         status == SXML_STATUS_OK && offset < element->attributes.attribute_count; offset += 1) {
        const sxml_xslt_ast_attribute *attribute =
            attribute_at(lowering, element->attributes.first_attribute_index + offset);
        switch ((uint32_t)attribute->kind) {
        case SXML_XSLT_AST_ATTRIBUTE_LITERAL_RESULT:
            attribute_count += 1;
            break;
        case SXML_XSLT_AST_ATTRIBUTE_INHERIT_NAMESPACES:
            /* xsl:inherit-namespaces (11.1.2). */
            if (attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
                !attribute->value.payload.is_true) {
                locals->construction_flags |= (uint32_t)SXML_IR_CONSTRUCTION_NO_INHERIT_NAMESPACES;
            }
            break;
        case SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS:
            /* 10.2.3: the attribute sets' attributes come first. */
            has_attribute_sets = true;
            attribute_count += 1;
            break;
        default:
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = attribute_count + 1,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
        locals->atoms = frame->owned_memory[0];
    }
    if (status == SXML_STATUS_OK && has_attribute_sets) {
        locals->open_let_before = locals->content_block.open_let;
        status = sxml_xslt_lowering_bind_attribute_sets(
            lowerer,
            &(sxml_xslt_attribute_set_call){
                .lowering = lowering, .node = locals->head.node, .block = &locals->content_block},
            &locals->atoms[0]);
        locals->atom_count = 1;
        if (status == SXML_STATUS_OK) {
            status = mark_literal_result_atom(lowerer, locals);
        }
    }
    return status;
}

/* The attribute constructor of the pending attribute, as the next atom. */
static sxml_status finish_pending_attribute(sxml_xpath_lowerer *lowerer,
                                            sxml_xpath_lowering_frame *frame) {
    literal_result_locals *locals = frame->locals;
    locals->has_pending_attribute = false;
    locals->open_let_before = locals->content_block.open_let;
    sxml_status status = sxml_xslt_lowering_bind_expression(
        lowerer,
        &(sxml_xslt_expression_to_bind){
            .block = &locals->content_block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
                           .node_construction = {.name = locals->pending_name,
                                                 .value = locals->pending_value}}},
        &locals->atoms[locals->atom_count]);
    locals->atom_count += 1;
    if (status == SXML_STATUS_OK) {
        status = mark_literal_result_atom(lowerer, locals);
    }
    return status;
}

/* Starts the next attribute value template; *has_started false when no
   attribute is left. */
typedef struct attribute_start {
    bool has_started;
} attribute_start;

static sxml_status start_next_attribute(sxml_xpath_lowering_machine *machine,
                                        sxml_xpath_lowering_frame *frame, attribute_start *start) {
    sxml_xpath_lowerer *lowerer = machine->state;
    literal_result_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_attribute_list list = node_of(lowering, locals->head.node)->attributes;
    start->has_started = false;
    while (locals->attribute_offset < list.attribute_count) {
        const sxml_xslt_ast_attribute *attribute =
            attribute_at(lowering, list.first_attribute_index + locals->attribute_offset);
        locals->attribute_offset += 1;
        if (attribute->kind != SXML_XSLT_AST_ATTRIBUTE_LITERAL_RESULT) {
            continue;
        }
        qname_parts name = {.expanded_name = {0}, .namespace_prefix = {0}};
        sxml_status status = make_result_name(
            lowerer,
            &(result_name_request){.lowering = lowering,
                                   .node = locals->head.node,
                                   .written = {.expanded_name = attribute->attribute_name,
                                               .namespace_prefix = attribute->namespace_prefix},
                                   .is_attribute = true},
            &name);
        if (status == SXML_STATUS_OK) {
            status = add_qname_constant(lowerer, name, &locals->pending_name);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->has_pending_attribute = true;
        start->has_started = true;
        return sxml_xslt_lowering_call_value_template(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = attribute->value.payload.expression,
                                                  .block = &locals->content_block,
                                                  .atom = &locals->pending_value});
    }
    return SXML_STATUS_OK;
}

static sxml_status finish_literal_result(sxml_xpath_lowering_machine *machine,
                                         sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    literal_result_locals *locals = frame->locals;
    const sxml_xslt_validation_query validation = {.lowering = locals->head.lowering,
                                                   .node = locals->head.node};
    sxml_status status = mark_literal_result_atom(lowerer, locals);
    sxml_ir_atom_list atoms = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            lowerer->unit,
            (sxml_ir_atom_view){.atoms = locals->atoms, .atom_count = locals->atom_count},
            &atoms);
    }
    sxml_ir_expression_index sequence = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                               .sequence = atoms}},
            &sequence);
    }
    /* With no LET the content stays an enclosed list (D110), whose atoms
       are constants. */
    sxml_ir_expression_index content = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer,
            (sxml_xpath_lowering_block_result){.block = locals->content_block, .result = sequence},
            &content);
    }
    sxml_ir_expression_index element = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
                               .node_construction = {.module_number =
                                                         sxml_xslt_lowering_module_of_node(
                                                             locals->head.lowering,
                                                             locals->head.node),
                                                     .name = locals->name,
                                                     .content = content,
                                                     .namespace_bindings =
                                                         locals->namespace_bindings,
                                                     .construction_flags =
                                                         locals->construction_flags |
                                                         sxml_xslt_lowering_validation_flags(
                                                             validation)}}},
            &element);
    }
    if (status == SXML_STATUS_OK && sxml_xslt_lowering_validates(validation)) {
        /* 11.1.2: xsl:validation and xsl:type (XSLT 4.0, 25; D179). */
        sxml_ir_atom constructed = {0};
        status = sxml_xpath_lowerer_bind_expression(
            lowerer,
            (sxml_xpath_lowering_expression_in_block){.block = frame->block, .expression = element},
            &constructed);
        sxml_ir_atom validated = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_bind_validation(
                lowerer,
                &(sxml_xslt_validation_request){
                    .query = validation, .block = frame->block, .value = constructed},
                &validated);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_lowering_add_expression(
                lowerer,
                (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                                     .atom = validated},
                &element);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, element);
}

static sxml_status continue_literal_result(sxml_xpath_lowering_machine *machine,
                                           sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    literal_result_locals *locals = frame->locals;
    if (frame->step == 2) {
        return finish_literal_result(machine, frame);
    }
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        frame->step = 1;
        status = start_literal_result(machine, frame);
    } else if (locals->has_pending_attribute) {
        status = finish_pending_attribute(lowerer, frame);
    }
    attribute_start start = {.has_started = false};
    if (status == SXML_STATUS_OK) {
        status = start_next_attribute(machine, frame, &start);
    }
    if (status != SXML_STATUS_OK || start.has_started) {
        return status;
    }
    frame->step = 2;
    sxml_ir_atom *content = &locals->atoms[locals->atom_count];
    locals->atom_count += 1;
    locals->open_let_before = locals->content_block.open_let;
    return sxml_xslt_lowering_call_sequence_constructor(
        machine, &(sxml_xslt_sequence_constructor_call){
                     .lowering = locals->head.lowering,
                     .first = node_of(locals->head.lowering, locals->head.node)->first_child,
                     .block = &locals->content_block,
                     .is_body = false,
                     .is_content = true,
                     .destination = {.expression = NULL,
                                     .atom = content,
                                     .atom_block = &locals->content_block,
                                     .focused = NULL},
                     .focus = NULL});
}

static const sxml_xpath_lowering_procedure literal_result_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt literal result element"),
    .step = continue_literal_result,
    .locals_size = sizeof(literal_result_locals)};

/* ------------------------------------------------------------- Fallback */

/* An extension instruction or an unknown XSLT instruction (24.3, 3.9):
   the sequence constructors of its xsl:fallback children, or XTDE1450
   when it has none. */
typedef struct fallback_locals {
    sxml_xslt_instruction_locals head;
    sxml_xslt_ast_node_index next;
    /* One atom per xsl:fallback (owned memory 0). */
    sxml_ir_atom *atoms;
    size_t atom_count;
} fallback_locals;

static sxml_status continue_fallback(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    fallback_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    if (frame->step == 0) {
        frame->step = 1;
        size_t fallback_count = 0;
        for (sxml_xslt_ast_node_index child = node_of(lowering, locals->head.node)->first_child;
             child.value != 0; child = next_of(lowering, child)) {
            fallback_count += kind_of(lowering, child) == SXML_XSLT_AST_KIND_FALLBACK ? 1U : 0U;
        }
        sxml_status status = SXML_STATUS_OK;
        sxml_ir_expression_index raise = {0};
        if (fallback_count == 0) {
            status = sxml_xpath_lowerer_add_raise(
                lowerer,
                (sxml_xpath_lowering_raise_request){
                    .node = no_ast_node,
                    .error_status = SXML_STATUS_DYNAMIC_ERROR,
                    .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XTDE1450"),
                    .description = SXML_BYTE_VIEW_FROM_LITERAL(
                        "an extension instruction is evaluated that the processor does not "
                        "implement, and it has no xsl:fallback (XSLT 4.0, 24.3.3)")},
                &raise);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            return sxml_xpath_lowering_finish_frame(machine, frame, raise);
        }
        status = sxml_xpath_lowering_allocate_owned(
            machine, frame,
            (sxml_xpath_lowering_owned_request){
                .memory_index = 0,
                .scratch = {.element_count = fallback_count,
                            .element_size = sizeof(sxml_ir_atom),
                            .element_alignment = alignof(sxml_ir_atom)}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        locals->atoms = frame->owned_memory[0];
        locals->next = node_of(lowering, locals->head.node)->first_child;
    }
    while (locals->next.value != 0) {
        const sxml_xslt_ast_node_index child = locals->next;
        locals->next = next_of(lowering, child);
        if (kind_of(lowering, child) != SXML_XSLT_AST_KIND_FALLBACK) {
            continue;
        }
        sxml_ir_atom *atom = &locals->atoms[locals->atom_count];
        locals->atom_count += 1;
        return sxml_xslt_lowering_call_sequence_constructor(
            machine,
            &(sxml_xslt_sequence_constructor_call){.lowering = lowering,
                                                   .first = node_of(lowering, child)->first_child,
                                                   .block = frame->block,
                                                   .is_body = false,
                                                   .destination = {.expression = NULL,
                                                                   .atom = atom,
                                                                   .atom_block = frame->block,
                                                                   .focused = NULL},
                                                   .focus = NULL});
    }
    sxml_ir_atom_list atoms = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        lowerer->unit,
        (sxml_ir_atom_view){.atoms = locals->atoms, .atom_count = locals->atom_count}, &atoms);
    sxml_ir_expression_index sequence = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                               .sequence = atoms}},
            &sequence);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, sequence);
}

static const sxml_xpath_lowering_procedure fallback_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt fallback"),
    .step = continue_fallback,
    .locals_size = sizeof(fallback_locals)};

/* ------------------------------------------------- Sequence constructors */

typedef struct sequence_constructor_locals {
    const sxml_xslt_lowering *lowering;
    sxml_xpath_lowering_focus *focus;
    sxml_xpath_lowering_focus *outer_focus;
    sxml_xslt_ast_node_index first;
    sxml_xslt_ast_node_index next;
    /* One atom per child with a value (owned memory 0). */
    sxml_ir_atom *atoms;
    size_t atom_count;
    /* The scope to go back to: variables of the children end here. */
    size_t scope_entry_count;
    /* With xsl:on-empty or xsl:on-non-empty children (8.5): the value
       src/xslt_lower_control.c makes of the atoms. */
    bool has_conditional_content;
    sxml_ir_atom conditional_content;
    /* True while the children's LETs are marked as content LETs (D166):
       a body or content, without conditional content, and every child
       before was marked or gave (). */
    bool marks_content;
    /* The child whose LET is marked next: its atom is atoms[atom_count -
       1], and the block's open LET before its lowering. */
    bool has_pending_child;
    sxml_ir_expression_index open_let_before;
} sequence_constructor_locals;

/* Marks the LET of the child lowered last as a content LET (D166). */
static sxml_status mark_content_child(sxml_xpath_lowerer *lowerer,
                                      const sxml_xpath_lowering_frame *frame) {
    sequence_constructor_locals *locals = frame->locals;
    if (!locals->has_pending_child) {
        return SXML_STATUS_OK;
    }
    locals->has_pending_child = false;
    if (!locals->marks_content) {
        return SXML_STATUS_OK;
    }
    sxml_xpath_lowering_content_mark mark = {.goes_on = true};
    const sxml_status status = sxml_xpath_lowerer_mark_content_let(
        lowerer,
        (sxml_xpath_lowering_content_item){.block = frame->block,
                                           .open_let_before = locals->open_let_before,
                                           .atom = locals->atoms[locals->atom_count - 1]},
        &mark);
    locals->marks_content = mark.goes_on;
    return status;
}

sxml_xslt_ast_node_index sxml_xslt_lowering_first_constructor_child(const sxml_xslt_ast *ast,
                                                                    sxml_xslt_ast_node_index node) {
    sxml_xslt_ast_node_index child = sxml_xslt_ast_get_node(ast, node)->first_child;
    while (child.value != 0) {
        const sxml_xslt_ast_kind kind = sxml_xslt_ast_get_node(ast, child)->kind;
        if (kind != SXML_XSLT_AST_KIND_PARAM && kind != SXML_XSLT_AST_KIND_CONTEXT_ITEM &&
            kind != SXML_XSLT_AST_KIND_SORT && kind != SXML_XSLT_AST_KIND_ON_COMPLETION) {
            break;
        }
        child = sxml_xslt_ast_get_node(ast, child)->next_sibling;
    }
    return child;
}

static const sxml_xpath_lowering_procedure *find_instruction_procedure(sxml_xslt_ast_kind kind) {
    for (size_t view_index = 0;
         view_index < sizeof(instruction_rule_views) / sizeof(instruction_rule_views[0]);
         view_index += 1) {
        const sxml_xslt_instruction_rule_view *view = instruction_rule_views[view_index];
        for (size_t rule_index = 0; rule_index < view->rule_count; rule_index += 1) {
            if (view->rules[rule_index].kind == kind) {
                return view->rules[rule_index].procedure;
            }
        }
    }
    return NULL;
}

/* Lowers child, one node of the sequence constructor, to *atom: at once
   for literal text, else by pushing its frame. *has_atom false for a child
   that gives nothing (xsl:fallback of a known instruction). */
typedef struct child_lowering {
    sxml_xpath_lowering_frame *frame;
    sxml_xslt_ast_node_index child;
    sxml_ir_atom *atom;
} child_lowering;

typedef struct child_outcome {
    bool has_atom;
    bool has_pushed_frame;
} child_outcome;

static sxml_status lower_child(sxml_xpath_lowering_machine *machine, child_lowering request,
                               child_outcome *outcome) {
    sxml_xpath_lowering_frame *frame = request.frame;
    const sequence_constructor_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->lowering;
    const sxml_xslt_ast_node *child = node_of(lowering, request.child);
    *outcome = (child_outcome){.has_atom = true, .has_pushed_frame = true};
    const sxml_xpath_lowering_procedure *procedure = NULL;
    switch ((uint32_t)child->kind) {
    case SXML_XSLT_AST_KIND_TEXT_NODE:
        if (child->content.kind == SXML_XSLT_AST_VALUE_KIND_TEXT) {
            outcome->has_pushed_frame = false;
            return bind_literal_text(machine->state,
                                     (literal_text_request){.lowering = lowering,
                                                            .node = request.child,
                                                            .block = frame->block},
                                     request.atom);
        }
        procedure = &text_template_procedure;
        break;
    case SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT:
        procedure = &literal_result_procedure;
        break;
    case SXML_XSLT_AST_KIND_EXTENSION_INSTRUCTION:
        /* 10.1.3: one named after a named template calls it; sxml has no
           other extension instructions (24.3). */
        procedure = sxml_xslt_lowering_calls_template(lowering, request.child)
                        ? find_instruction_procedure(child->kind)
                        : &fallback_procedure;
        break;
    case SXML_XSLT_AST_KIND_UNKNOWN_INSTRUCTION:
        procedure = &fallback_procedure;
        break;
    case SXML_XSLT_AST_KIND_FALLBACK:
    case SXML_XSLT_AST_KIND_ON_EMPTY:
    case SXML_XSLT_AST_KIND_ON_NON_EMPTY:
        /* 24.3.3: xsl:fallback is ignored under an instruction the
           processor knows. 8.5.2, 8.5.3: xsl:on-empty and
           xsl:on-non-empty are lowered with the other atoms when the
           sequence constructor ends
           (sxml_xslt_lowering_call_conditional_content). */
        *outcome = (child_outcome){.has_atom = false, .has_pushed_frame = false};
        return SXML_STATUS_OK;
    default:
        procedure = find_instruction_procedure(child->kind);
        break;
    }
    if (procedure == NULL) {
        *outcome = (child_outcome){.has_atom = false, .has_pushed_frame = false};
        return sxml_xslt_lowering_report_unsupported(
            lowering, (sxml_xslt_static_error){.node = request.child,
                                               .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                                   "this XSLT instruction is not lowered yet")});
    }
    const sxml_xslt_instruction_locals initial = {.lowering = lowering, .node = request.child};
    sxml_xpath_lowering_frame *child_frame = NULL;
    return sxml_xpath_lowering_push_procedure(
        machine,
        &(sxml_xpath_lowering_procedure_call){
            .procedure = procedure,
            .node = no_ast_node,
            .block = frame->block,
            .is_body = false,
            .destination = {.expression = NULL,
                            .atom = request.atom,
                            .atom_block = frame->block,
                            .focused = NULL},
            .initial_locals = {.bytes = (const uint8_t *)&initial, .byte_count = sizeof(initial)}},
        &child_frame);
}

static sxml_status finish_sequence_constructor(sxml_xpath_lowering_machine *machine,
                                               sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    const sequence_constructor_locals *locals = frame->locals;
    sxml_xpath_lowerer_pop_variables(lowerer, locals->scope_entry_count);
    if (locals->focus != NULL) {
        lowerer->function->focus = locals->outer_focus;
    }
    sxml_ir_expression expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM};
    sxml_status status = SXML_STATUS_OK;
    if (locals->has_conditional_content) {
        expression.atom = locals->conditional_content;
    } else if (locals->atom_count == 1) {
        expression.atom = locals->atoms[0];
    } else {
        sxml_ir_atom_list atoms = {0};
        status = sxml_ir_unit_add_atoms(
            lowerer->unit,
            (sxml_ir_atom_view){.atoms = locals->atoms, .atom_count = locals->atom_count}, &atoms);
        expression = (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                          .sequence = atoms};
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){.node = no_ast_node, .expression = expression},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

static sxml_status continue_sequence_constructor(sxml_xpath_lowering_machine *machine,
                                                 sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    sequence_constructor_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->lowering;
    if (frame->step == 0) {
        frame->step = 1;
        if (locals->focus != NULL) {
            locals->outer_focus = lowerer->function->focus;
            lowerer->function->focus = locals->focus;
        }
        locals->scope_entry_count = lowerer->scope_entry_count;
        size_t child_count = 0;
        for (sxml_xslt_ast_node_index child = locals->first; child.value != 0;
             child = next_of(lowering, child)) {
            child_count += 1;
            const sxml_xslt_ast_kind kind = kind_of(lowering, child);
            if (kind == SXML_XSLT_AST_KIND_ON_EMPTY || kind == SXML_XSLT_AST_KIND_ON_NON_EMPTY) {
                /* 8.5: the atoms are read by the conditional content. */
                locals->marks_content = false;
            }
        }
        if (child_count > 0) {
            const sxml_status status = sxml_xpath_lowering_allocate_owned(
                machine, frame,
                (sxml_xpath_lowering_owned_request){
                    .memory_index = 0,
                    .scratch = {.element_count = child_count,
                                .element_size = sizeof(sxml_ir_atom),
                                .element_alignment = alignof(sxml_ir_atom)}});
            if (status != SXML_STATUS_OK) {
                return status;
            }
            locals->atoms = frame->owned_memory[0];
        }
        locals->next = locals->first;
    }
    while (locals->next.value != 0) {
        const sxml_status mark_status = mark_content_child(lowerer, frame);
        if (mark_status != SXML_STATUS_OK) {
            return mark_status;
        }
        const sxml_xslt_ast_node_index child = locals->next;
        locals->next = next_of(lowering, child);
        locals->open_let_before = frame->block->open_let;
        child_outcome outcome = {.has_atom = false, .has_pushed_frame = false};
        const sxml_status status = lower_child(
            machine,
            (child_lowering){
                .frame = frame, .child = child, .atom = &locals->atoms[locals->atom_count]},
            &outcome);
        if (outcome.has_atom) {
            locals->atom_count += 1;
            locals->has_pending_child = true;
        }
        if (status != SXML_STATUS_OK || outcome.has_pushed_frame) {
            return status;
        }
        const sxml_xslt_ast_kind kind = kind_of(lowering, child);
        locals->has_conditional_content = locals->has_conditional_content ||
                                          kind == SXML_XSLT_AST_KIND_ON_EMPTY ||
                                          kind == SXML_XSLT_AST_KIND_ON_NON_EMPTY;
    }
    const sxml_status mark_status = mark_content_child(lowerer, frame);
    if (mark_status != SXML_STATUS_OK) {
        return mark_status;
    }
    if (frame->step == 1 && locals->has_conditional_content) {
        frame->step = 2;
        return sxml_xslt_lowering_call_conditional_content(
            machine, &(sxml_xslt_conditional_content_call){
                         .lowering = lowering,
                         .first = locals->first,
                         .block = frame->block,
                         .atoms = {.atoms = locals->atoms, .atom_count = locals->atom_count},
                         .atom = &locals->conditional_content,
                         .scope_mark = locals->scope_entry_count});
    }
    return finish_sequence_constructor(machine, frame);
}

static const sxml_xpath_lowering_procedure sequence_constructor_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xslt sequence constructor"),
    .step = continue_sequence_constructor,
    .locals_size = sizeof(sequence_constructor_locals)};

/* The procedure call of a sequence constructor. */
static sxml_xpath_lowering_procedure_call
sequence_constructor_procedure_call(const sxml_xslt_sequence_constructor_call *call,
                                    const sequence_constructor_locals *initial) {
    return (sxml_xpath_lowering_procedure_call){
        .procedure = &sequence_constructor_procedure,
        .node = no_ast_node,
        .block = call->block,
        .is_body = call->is_body,
        .destination = call->destination,
        .initial_locals = {.bytes = (const uint8_t *)initial, .byte_count = sizeof(*initial)}};
}

sxml_status
sxml_xslt_lowering_call_sequence_constructor(sxml_xpath_lowering_machine *machine,
                                             const sxml_xslt_sequence_constructor_call *call) {
    const sequence_constructor_locals initial = {.lowering = call->lowering,
                                                 .focus = call->focus,
                                                 .first = call->first,
                                                 .marks_content = call->is_body || call->is_content};
    const sxml_xpath_lowering_procedure_call procedure_call =
        sequence_constructor_procedure_call(call, &initial);
    sxml_xpath_lowering_frame *frame = NULL;
    return sxml_xpath_lowering_push_procedure(machine, &procedure_call, &frame);
}

/* ------------------------------------------------------ The lowering */

/* The memory of a stylesheet lowering besides the lowerer. */
typedef struct lowering_storage {
    sxml_xpath_static_context *static_contexts;
    sxml_namespace_binding *namespace_bindings;
    sxml_xslt_namespace_alias *namespace_aliases;
} lowering_storage;

static void release_storage(sxml_allocator allocator, lowering_storage *storage) {
    sxml_allocator_release(allocator, storage->static_contexts);
    sxml_allocator_release(allocator, storage->namespace_bindings);
    sxml_allocator_release(allocator, storage->namespace_aliases);
}

/* What the static contexts of a lowering are made from. */
typedef struct context_source {
    const sxml_xslt_ast *ast;
    const sxml_xpath_static_context *base;
} context_source;

/* One XPath static context per static context of the AST (3.6, 5.1):
   its namespaces (without xml, which XPath predeclares) and its
   xpath-default-namespace as prefix 0 (5.1.2.1; "##any" is not
   expressible and reads as none), its static base URI. */
static sxml_status make_static_contexts(context_source source, lowering_storage *storage,
                                        sxml_xslt_lowering *lowering) {
    const sxml_xslt_ast *ast = source.ast;
    const sxml_allocator allocator = source.base->allocator;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(source.base->item_heap);
    sxml_namespace_prefix_identifier xml_prefix = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_prefix(
        name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xml"), &xml_prefix);
    const uint32_t context_count = ast->static_context_count;
    size_t binding_capacity = 0;
    for (uint32_t context_index = 1; context_index < context_count; context_index += 1) {
        binding_capacity += sxml_xslt_ast_get_namespace_bindings(
                                ast, (sxml_xslt_ast_static_context_index){.value = context_index})
                                .namespace_binding_count +
                            1U;
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status =
            sxml_allocator_allocate_array(allocator,
                           (sxml_array_allocation_request){.element_count = context_count,
                                           .element_size = sizeof(sxml_xpath_static_context),
                                           .element_alignment = alignof(sxml_xpath_static_context)},
                           &memory);
    }
    storage->static_contexts = memory;
    if (status == SXML_STATUS_OK) {
        status =
            sxml_allocator_allocate_array(allocator,
                           (sxml_array_allocation_request){.element_count = binding_capacity,
                                           .element_size = sizeof(sxml_namespace_binding),
                                           .element_alignment = alignof(sxml_namespace_binding)},
                           &memory);
        storage->namespace_bindings = memory;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t binding_count = 0;
    for (uint32_t context_index = 1; context_index < context_count; context_index += 1) {
        const sxml_xslt_ast_static_context_index index = {.value = context_index};
        const sxml_xslt_ast_static_context *context = sxml_xslt_ast_get_static_context(ast, index);
        const sxml_namespace_binding_view bindings =
            sxml_xslt_ast_get_namespace_bindings(ast, index);
        sxml_namespace_binding *first = storage->namespace_bindings + binding_count;
        size_t count = 0;
        if (first == NULL) {
            /* Not reached: every static context has room for one binding. */
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        if (context->xpath_default_namespace.index != SXML_NAMESPACE_NONE) {
            first[count] =
                (sxml_namespace_binding){.namespace_prefix = {.index = 0},
                                         .namespace_uri = context->xpath_default_namespace};
            count += 1;
        }
        for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
             binding_index += 1) {
            const sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
            if (binding.namespace_prefix.index != xml_prefix.index &&
                binding.namespace_prefix.index != 0) {
                first[count] = binding;
                count += 1;
            }
        }
        binding_count += count;
        storage->static_contexts[context_index] = *source.base;
        storage->static_contexts[context_index].namespace_bindings = (sxml_namespace_binding_view){
            .namespace_bindings = count > 0 ? first : NULL, .namespace_binding_count = count};
        storage->static_contexts[context_index].static_base_uri =
            sxml_xslt_ast_get_text(ast, context->static_base_uri);
        /* XSLT 4.0, 5.3.1: XPath 1.0 compatibility mode is true for an
           element processed with XSLT 1.0 behaviour, whose effective
           version is below 2.0 (3.8.1; XSLT 2.0 and 3.0 behaviour is
           4.0's, 3.8.2, 3.8.3). This replaces the XTDE0160 of a processor
           without the feature (27.4; D197). */
        storage->static_contexts[context_index].has_xpath_1_compatibility_mode =
            context->version < 2.0;
    }
    lowering->static_contexts = storage->static_contexts;
    lowering->static_context_count = context_count;
    return SXML_STATUS_OK;
}

/* The namespace aliases of the stylesheet (11.1.4): for each literal
   namespace URI, the declaration of highest import precedence; XTSE0810
   for two of that precedence with different result URIs. */
static sxml_status collect_aliases(const sxml_xslt_lowering *source_lowering,
                                   lowering_storage *storage, sxml_xslt_lowering *lowering) {
    const sxml_xslt_ast *ast = source_lowering->ast;
    size_t alias_capacity = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        alias_capacity +=
            sxml_xslt_ast_get_node(ast, declaration)->kind == SXML_XSLT_AST_KIND_NAMESPACE_ALIAS
                ? 1U
                : 0U;
    }
    void *memory = NULL;
    const sxml_status status =
        sxml_allocator_allocate_array(source_lowering->lowerer->allocator,
                       (sxml_array_allocation_request){.element_count = alias_capacity,
                                       .element_size = sizeof(sxml_xslt_namespace_alias),
                                       .element_alignment = alignof(sxml_xslt_namespace_alias)},
                       &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    storage->namespace_aliases = memory;
    lowering->namespace_aliases = memory;
    lowering->namespace_alias_count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        const sxml_xslt_ast_node *node = sxml_xslt_ast_get_node(ast, declaration);
        if (node->kind != SXML_XSLT_AST_KIND_NAMESPACE_ALIAS) {
            continue;
        }
        const sxml_xslt_ast_attribute *stylesheet_prefix = sxml_xslt_ast_get_attribute(
            ast,
            sxml_xslt_ast_find_attribute(
                ast, (sxml_xslt_ast_attribute_query){
                         .node = declaration, .kind = SXML_XSLT_AST_ATTRIBUTE_STYLESHEET_PREFIX}));
        const sxml_xslt_ast_attribute *result_prefix = sxml_xslt_ast_get_attribute(
            ast, sxml_xslt_ast_find_attribute(
                     ast, (sxml_xslt_ast_attribute_query){
                              .node = declaration, .kind = SXML_XSLT_AST_ATTRIBUTE_RESULT_PREFIX}));
        if (stylesheet_prefix->value.kind != SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING ||
            result_prefix->value.kind != SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING) {
            continue;
        }
        const sxml_xslt_namespace_alias alias = {
            .stylesheet_uri = stylesheet_prefix->value.payload.namespace_binding.namespace_uri,
            .result_binding = result_prefix->value.payload.namespace_binding,
            .import_precedence = sxml_xslt_ast_get_module(ast, node->module)->import_precedence,
            .package = sxml_xslt_package_of_node(source_lowering, declaration)};
        sxml_xslt_namespace_alias *existing = NULL;
        for (size_t alias_index = 0; alias_index < lowering->namespace_alias_count;
             alias_index += 1) {
            if (lowering->namespace_aliases[alias_index].stylesheet_uri.index ==
                    alias.stylesheet_uri.index &&
                lowering->namespace_aliases[alias_index].package == alias.package) {
                existing = &lowering->namespace_aliases[alias_index];
            }
        }
        if (existing == NULL) {
            lowering->namespace_aliases[lowering->namespace_alias_count] = alias;
            lowering->namespace_alias_count += 1;
        } else if (alias.import_precedence > existing->import_precedence) {
            *existing = alias;
        } else if (alias.import_precedence == existing->import_precedence &&
                   alias.result_binding.namespace_uri.index !=
                       existing->result_binding.namespace_uri.index) {
            return sxml_xslt_lowering_report_static_error(
                source_lowering,
                (sxml_xslt_static_error){
                    .node = declaration,
                    .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                        "XTSE0810 two xsl:namespace-alias declarations of one import precedence "
                        "give one literal namespace URI different result URIs (XSLT 4.0, "
                        "11.1.4)")});
        }
    }
    return SXML_STATUS_OK;
}

/* The invocation the request asks for (see sxml_xslt_lowering_request):
   the template to call, or 0 to apply templates. *is_valid false for a
   template_node that is no template. */
typedef struct invocation_choice {
    sxml_xslt_invocation invocation;
    bool is_valid;
} invocation_choice;

static invocation_choice choose_invocation(const sxml_xslt_lowering_request *request) {
    const sxml_xslt_ast *ast = request->ast;
    invocation_choice choice = {.invocation = {.template_node = {.value = 0},
                                               .initial_mode = request->initial_mode,
                                               .selects_unnamed_mode =
                                                   request->selects_unnamed_mode},
                                .is_valid = true};
    if (request->applies_templates) {
        return choice;
    }
    if (request->template_node.value != 0) {
        choice.invocation.template_node = request->template_node;
        choice.is_valid = sxml_xslt_ast_get_node(ast, request->template_node)->kind ==
                          SXML_XSLT_AST_KIND_TEMPLATE;
        return choice;
    }
    sxml_name_identifier initial_name = request->initial_template;
    bool has_initial_name = initial_name.index != 0;
    if (!has_initial_name) {
        has_initial_name = sxml_name_pool_find_name(
            sxml_item_heap_get_name_pool(request->processor->item_heap),
            &(sxml_name_request){.namespace_uri =
                                     sxml_well_known_namespace_identifier(SXML_NAMESPACE_XSLT),
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("initial-template")},
            &initial_name);
    }
    /* 10.1.1: the template of that name with the highest precedence. */
    uint32_t best_precedence = 0;
    for (uint32_t order = 1;
         has_initial_name && sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        const sxml_xslt_ast_node *node = sxml_xslt_ast_get_node(ast, declaration);
        if (node->kind != SXML_XSLT_AST_KIND_TEMPLATE) {
            continue;
        }
        const sxml_xslt_ast_attribute *name = sxml_xslt_ast_get_attribute(
            ast, sxml_xslt_ast_find_attribute(
                     ast, (sxml_xslt_ast_attribute_query){.node = declaration,
                                                          .kind = SXML_XSLT_AST_ATTRIBUTE_NAME}));
        const uint32_t precedence = sxml_xslt_ast_get_module(ast, node->module)->import_precedence;
        if (name->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME &&
            name->value.payload.name.index == initial_name.index &&
            (choice.invocation.template_node.value == 0 || precedence > best_precedence)) {
            choice.invocation.template_node = declaration;
            best_precedence = precedence;
        }
    }
    if (request->initial_template.index != 0 && choice.invocation.template_node.value == 0) {
        choice.invocation.is_unknown_template = true;
    }
    return choice;
}

/* Lowers the invocation as the main function. */
typedef struct main_request {
    const sxml_xslt_lowering *lowering;
    sxml_xslt_invocation invocation;
    bool builds_result_document;
} main_request;

static sxml_status lower_main_function(sxml_xpath_lowerer *lowerer, const main_request *request) {
    sxml_type_identifier signature = {0};
    sxml_status status = sxml_xpath_lowerer_register_signature(
        lowerer,
        (sxml_xpath_lowering_signature_request){
            .parameter_types = {.sequence_types = NULL, .sequence_type_count = 0},
            .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}},
        &signature);
    sxml_ir_function_index main_function = {.value = 1};
    if (status == SXML_STATUS_OK) {
        /* Function 1 was added before the templates (the main function). */
        sxml_ir_function_replacement replacement = {
            .index = main_function,
            .function = *sxml_ir_unit_get_function(lowerer->unit, main_function)};
        replacement.function.signature = signature;
        status = sxml_ir_unit_replace_function(lowerer->unit, &replacement);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xpath_lowering_function_frame function_frame = {
        .function = {.function_index = main_function,
                     .function_level = 0,
                     .slot_count = 0,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_XPATH_LOWERING_FOCUS_KIND_SLOTS},
    };
    sxml_xpath_lowerer_enter_function(lowerer, &function_frame);
    if (request->lowering->global_count > 0) {
        /* The context global takes the context value from this slot
           (D135; global variables, packet 4.7). */
        status = sxml_xpath_lowerer_add_slot(lowerer, &function_frame.focus.slots.context_value);
    }
    sxml_xpath_lowering_block block = {.first = {0}, .open_let = {0}};
    sxml_ir_atom value = {0};
    if (status == SXML_STATUS_OK) {
        /* 3.4.9: xsl:global-context-item (packet 4.12). */
        status = sxml_xslt_lowering_bind_global_context_item(
            lowerer, &(sxml_xslt_global_context_item_call){.lowering = request->lowering,
                                                           .block = &block});
    }
    if (status == SXML_STATUS_OK) {
        /* 19.2: the accumulators applicable to the initial match
           selection's tree (packet 4.14). */
        status = sxml_xslt_bind_accumulator_scope(
            lowerer, &(sxml_xslt_accumulator_scope_call){.table = request->lowering->accumulators,
                                                         .invocation = request->invocation,
                                                         .block = &block});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_invocation(
            lowerer,
            &(sxml_xslt_invocation_request){
                .lowering = request->lowering, .invocation = request->invocation, .block = &block},
            &value);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        sxml_ir_expression expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                                         .atom = value};
        if (request->builds_result_document) {
            /* 2.4: the principal result's tree is built from the raw
               result as a document node's content (5.7.1). */
            status = sxml_xpath_lowerer_add_expression(
                lowerer,
                (sxml_xpath_lowering_expression_to_add){.node = no_ast_node,
                                                        .expression = expression},
                &result);
            expression = (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
                .node_construction = {.content = result,
                                      .construction_flags =
                                          (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES}};
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_lowerer_add_expression(
                lowerer,
                (sxml_xpath_lowering_expression_to_add){.node = no_ast_node,
                                                        .expression = expression},
                &result);
        }
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_finish_block(
            lowerer, (sxml_xpath_lowering_block_result){.block = block, .result = result}, &body);
    }
    sxml_xpath_lowerer_leave_function(lowerer, &function_frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = main_function,
        .function = *sxml_ir_unit_get_function(lowerer->unit, main_function)};
    replacement.function.body = body;
    replacement.function.slot_count = function_frame.function.slot_count;
    replacement.function.focus = function_frame.focus.slots;
    return sxml_ir_unit_replace_function(lowerer->unit, &replacement);
}

/* ------------------------------------------------ Invocation globals */

/*
 * The run-time values of an invocation (packet 4.15, D157) are globals of
 * the unit after the stylesheet's globals and the context global, so a
 * host binds them by name as it binds stylesheet parameters: the
 * parameter maps of an apply-templates or call-template invocation (with
 * the empty map as their default, so a unit evaluated without them still
 * runs), and when asked the initial match selection or the arguments of
 * an initial function (no default).
 */
enum { SXML_INVOCATION_GLOBAL_MAXIMUM_COUNT = 3 };

typedef struct invocation_globals {
    sxml_name_identifier names[SXML_INVOCATION_GLOBAL_MAXIMUM_COUNT];
    bool has_empty_map_default[SXML_INVOCATION_GLOBAL_MAXIMUM_COUNT];
    size_t global_count;
    /* The number of the first one: after the stylesheet's globals and the
       context global, which sxml_xslt_lowering_add_global_variables adds
       later. */
    uint32_t first_number;
} invocation_globals;

/* Interns Q{urn:x-sxml:invocation}local_name and plans it as the next
   global, whose atom it writes. */
typedef struct invocation_global_request {
    sxml_xpath_lowerer *lowerer;
    const char *local_name;
    bool has_empty_map_default;
} invocation_global_request;

static sxml_status plan_invocation_global(invocation_global_request request,
                                          invocation_globals *globals, sxml_ir_atom *atom) {
    sxml_xpath_lowerer *lowerer = request.lowerer;
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        lowerer->name_pool, SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_INVOCATION_NAMESPACE),
        &namespace_uri);
    sxml_name_identifier name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            lowerer->name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = sxml_byte_view_from_c_string(request.local_name)},
            &name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    globals->names[globals->global_count] = name;
    globals->has_empty_map_default[globals->global_count] = request.has_empty_map_default;
    *atom = (sxml_ir_atom){
        .atom_kind = SXML_IR_ATOM_KIND_GLOBAL_VARIABLE,
        .global_variable = {.value = globals->first_number + (uint32_t)globals->global_count}};
    globals->global_count += 1;
    return SXML_STATUS_OK;
}

/* Plans the globals the invocation reads and writes their atoms into it. */
typedef struct invocation_plan {
    const sxml_xslt_lowering *lowering;
    const sxml_xslt_lowering_request *request;
} invocation_plan;

static sxml_status plan_invocation_globals(const invocation_plan *plan,
                                           sxml_xslt_invocation *invocation,
                                           invocation_globals *globals) {
    sxml_xpath_lowerer *lowerer = plan->lowering->lowerer;
    const size_t stylesheet_global_count = plan->lowering->global_count;
    globals->global_count = 0;
    globals->first_number =
        (uint32_t)(stylesheet_global_count > 0 ? stylesheet_global_count + 2U : 1U);
    if (plan->request->initial_function.index != 0) {
        invocation->initial_function = plan->request->initial_function;
        invocation->function_arity = plan->request->initial_function_arity;
        return plan_invocation_global(
            (invocation_global_request){.lowerer = lowerer,
                                        .local_name = SXML_XSLT_INVOCATION_FUNCTION_ARGUMENTS,
                                        .has_empty_map_default = false},
            globals, &invocation->function_arguments);
    }
    sxml_status status = SXML_STATUS_OK;
    if (plan->request->reads_invocation_parameters) {
        status = plan_invocation_global(
            (invocation_global_request){.lowerer = lowerer,
                                        .local_name = SXML_XSLT_INVOCATION_TEMPLATE_PARAMETERS,
                                        .has_empty_map_default = true},
            globals, &invocation->template_parameters);
    }
    if (status == SXML_STATUS_OK && plan->request->reads_invocation_parameters) {
        status = plan_invocation_global(
            (invocation_global_request){.lowerer = lowerer,
                                        .local_name = SXML_XSLT_INVOCATION_TUNNEL_PARAMETERS,
                                        .has_empty_map_default = true},
            globals, &invocation->tunnel_parameters);
    }
    if (status == SXML_STATUS_OK && plan->request->has_initial_match_selection &&
        invocation->template_node.value == 0) {
        status = plan_invocation_global(
            (invocation_global_request){.lowerer = lowerer,
                                        .local_name =
                                            SXML_XSLT_INVOCATION_INITIAL_MATCH_SELECTION,
                                        .has_empty_map_default = false},
            globals, &invocation->initial_match_selection);
    }
    return status;
}

/* An initializer of arity 0 that returns the empty map. */
static sxml_status add_empty_map_initializer(sxml_xpath_lowerer *lowerer,
                                             sxml_ir_function_index *initializer) {
    sxml_type_identifier signature = {0};
    sxml_status status = sxml_xpath_lowerer_register_signature(
        lowerer,
        (sxml_xpath_lowering_signature_request){
            .parameter_types = {.sequence_types = NULL, .sequence_type_count = 0},
            .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}},
        &signature);
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            lowerer->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = NULL, .map_entry_count = 0},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                                .error_record = NULL},
            &map);
    }
    sxml_ir_atom empty_map = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(lowerer, map, &empty_map);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            lowerer,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                               .atom = empty_map}},
            &body);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            lowerer->unit,
            &(sxml_ir_function){.signature = signature, .parameter_count = 0, .body = body},
            initializer);
    }
    return status;
}

/* Adds the planned globals, after sxml_xslt_lowering_add_global_variables. */
static sxml_status add_invocation_globals(sxml_xpath_lowerer *lowerer,
                                          const invocation_globals *globals) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t global_index = 0; status == SXML_STATUS_OK && global_index < globals->global_count;
         global_index += 1) {
        sxml_ir_function_index initializer = {0};
        if (globals->has_empty_map_default[global_index]) {
            status = add_empty_map_initializer(lowerer, &initializer);
        }
        sxml_ir_global_index global = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_add_global_variable(
                lowerer->unit,
                &(sxml_ir_global_variable){
                    .variable_name = globals->names[global_index],
                    .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                                      .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
                    .initializer = initializer},
                &global);
        }
        if (status == SXML_STATUS_OK &&
            global.value != globals->first_number + (uint32_t)global_index) {
            /* The numbers the main function read were planned wrong. */
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    return status;
}

/* Lowers into new_unit with a lowerer in caller memory. */
/* 2.4.4 with packages (3.4.5): the initial template is the named template
   the top-level package binds the name to, when it is public or final
   there (an xsl:stylesheet's named templates are public); otherwise there
   is none (XTDE0040 for a name the host gave). */
static void bind_initial_template(const sxml_xslt_lowering *lowering,
                                  const sxml_xslt_lowering_request *request,
                                  sxml_xslt_invocation *invocation) {
    if (request->applies_templates || request->template_node.value != 0 ||
        invocation->template_node.value == 0) {
        return;
    }
    const sxml_xslt_ast_attribute *name = sxml_xslt_ast_get_attribute(
        lowering->ast,
        sxml_xslt_ast_find_attribute(
            lowering->ast, (sxml_xslt_ast_attribute_query){.node = invocation->template_node,
                                                           .kind = SXML_XSLT_AST_ATTRIBUTE_NAME}));
    if (name->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME) {
        return;
    }
    const sxml_xslt_component_reference reference = {
        .component_kind = SXML_XSLT_COMPONENT_KIND_TEMPLATE,
        .component_name = name->value.payload.name,
        .arity = 0,
        .package = 1};
    const sxml_xslt_visibility visibility =
        sxml_xslt_package_get_reference_visibility(lowering, &reference);
    const sxml_xslt_component_binding binding = sxml_xslt_package_bind(lowering, &reference);
    if (binding.declaration.value != 0 &&
        (visibility == SXML_XSLT_VISIBILITY_PUBLIC || visibility == SXML_XSLT_VISIBILITY_FINAL)) {
        invocation->template_node = binding.declaration;
        return;
    }
    invocation->template_node = (sxml_xslt_ast_node_index){.value = 0};
    invocation->is_unknown_template = request->initial_template.index != 0;
}

typedef struct unit_lowering {
    const sxml_xslt_lowering_request *request;
    sxml_xslt_invocation invocation;
    const sxml_xpath_static_context *base_context;
    sxml_ir_unit *unit;
} unit_lowering;

static sxml_status lower_into_unit(const unit_lowering *work) {
    const sxml_xslt_lowering_request *request = work->request;
    /* A plain struct on the stack is memory of the lowerer's size and
       alignment. */
    sxml_xpath_lowerer storage;
    sxml_xpath_lowerer *lowerer = NULL;
    sxml_status status = sxml_xpath_lowerer_initialize(
        (sxml_memory_view){.bytes = (uint8_t *)&storage, .byte_count = sizeof(storage)},
        &(sxml_xpath_lowerer_configuration){
            .static_context = work->base_context,
            .unit = work->unit,
            .language = SXML_LANGUAGE_XSLT,
            .parts = *sxml_xpath_lowering_xslt_parts(),
            .maximum_expression_depth =
                sxml_processor_get_maximum_expression_depth(request->processor),
            .error_record = request->error_record},
        &lowerer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    lowering_storage memory = {
        .static_contexts = NULL, .namespace_bindings = NULL, .namespace_aliases = NULL};
    sxml_xslt_lowering lowering = {.ast = request->ast,
                                   .lowerer = lowerer,
                                   .static_contexts = NULL,
                                   .static_context_count = 0,
                                   .namespace_aliases = NULL,
                                   .namespace_alias_count = 0,
                                   .error_record = request->error_record,
                                   .templates = NULL,
                                   .components = NULL,
                                   .kept_asts = NULL,
                                   .packages = NULL,
                                   .stream_plan = NULL};
    sxml_xslt_kept_asts kept_asts = {
        .allocator = lowerer->allocator, .asts = NULL, .ast_count = 0, .ast_capacity = 0};
    lowering.kept_asts = &kept_asts;
    status = make_static_contexts((context_source){.ast = request->ast, .base = work->base_context},
                                  &memory, &lowering);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_create_stream_plan(lowerer->allocator, &lowering.stream_plan);
    }
    /* Packages first: every lookup below binds through them (packet
       4.16), the namespace aliases too (D163). */
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_declare_packages(&lowering);
    }
    /* Named types next: every type below may name one (5.4). */
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_declare_named_types(&lowering, memory.static_contexts);
    }
    if (status == SXML_STATUS_OK) {
        /* Before any expression is lowered: their calls name the modules,
           and the functions made below take theirs. */
        status = sxml_xslt_declare_static_context_modules(&lowering, memory.static_contexts);
    }
    if (status == SXML_STATUS_OK) {
        status = collect_aliases(&lowering, &memory, &lowering);
    }
    /* Function 1 is the main function, whose body comes last. */
    sxml_ir_function_index main_function = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(work->unit, &(sxml_ir_function){.parameter_count = 0},
                                           &main_function);
    }
    /* The XSLT functions in front of every static context's table
       (src/xslt_key_internal.h, packet 4.10). */
    sxml_xslt_function_table *xslt_functions = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_declare_functions(&lowering, memory.static_contexts, &xslt_functions);
    }
    /* fn:accumulator-before and -after in front of them (packet 4.14). */
    sxml_xslt_accumulator_table *accumulators = NULL;
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xslt_declare_accumulators(&lowering, memory.static_contexts, &accumulators);
        lowering.accumulators = accumulators;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_declare_global_variables(&lowering, memory.static_contexts);
    }
    if (status == SXML_STATUS_OK) {
        /* Stylesheet functions and attribute sets (packet 4.12): in every
           static context before anything calls them. */
        status = sxml_xslt_declare_components(&lowering, memory.static_contexts);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lower_templates(&lowering);
    }
    if (status == SXML_STATUS_OK) {
        /* Their bodies may call templates. */
        status = sxml_xslt_lower_components(&lowering);
    }
    sxml_xslt_invocation invocation = work->invocation;
    if (status == SXML_STATUS_OK) {
        bind_initial_template(&lowering, request, &invocation);
    }
    invocation_globals globals = {.global_count = 0, .first_number = 0};
    if (status == SXML_STATUS_OK) {
        status = plan_invocation_globals(
            &(invocation_plan){.lowering = &lowering, .request = request}, &invocation, &globals);
    }
    if (status == SXML_STATUS_OK) {
        status = lower_main_function(
            lowerer, &(main_request){.lowering = &lowering,
                                     .invocation = invocation,
                                     .builds_result_document = request->builds_result_document});
    }
    if (status == SXML_STATUS_OK) {
        /* While the templates are there (a key's sequence constructor may
           apply them); again after the global variables. */
        status = sxml_xslt_lower_accumulators(accumulators);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lower_functions(xslt_functions);
    }
    if (status == SXML_STATUS_OK) {
        /* Initializers may call templates (xsl:call-template in a global
           variable's content): before the templates are released. */
        status = sxml_xslt_lowering_add_global_variables(&lowering);
    }
    if (status == SXML_STATUS_OK) {
        status = add_invocation_globals(lowerer, &globals);
    }
    sxml_xslt_release_templates(&lowering);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lower_accumulators(accumulators);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lower_functions(xslt_functions);
    }
    if (status == SXML_STATUS_OK) {
        /* A key's body may name an accumulator function. */
        status = sxml_xslt_lower_accumulators(accumulators);
    }
    const sxml_xslt_ast_node_index context_node =
        invocation.template_node.value != 0
            ? invocation.template_node
            : sxml_xslt_ast_get_module(request->ast, (sxml_xslt_ast_module_index){.value = 1})
                  ->root;
    const sxml_xslt_ast_static_context_index template_context =
        sxml_xslt_ast_get_node(request->ast, context_node)->static_context;
    if (status == SXML_STATUS_OK && template_context.value < lowering.static_context_count) {
        /* For casts to xs:QName at run time (D84) and names given as
           strings: the template's namespaces. */
        const sxml_xpath_static_context *context =
            &lowering.static_contexts[template_context.value];
        status = sxml_ir_unit_set_static_namespaces(work->unit, context->namespace_bindings);
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_set_static_base_uri(work->unit, context->static_base_uri);
        }
    }
    sxml_xslt_lowering_release_global_variables(&lowering);
    sxml_xslt_release_accumulators(accumulators);
    sxml_xslt_release_functions(xslt_functions);
    sxml_xslt_release_components(&lowering);
    sxml_xslt_release_packages(&lowering);
    release_storage(work->base_context->allocator, &memory);
    sxml_xslt_release_kept_asts(&kept_asts);
    sxml_xslt_release_stream_plan(lowering.stream_plan);
    sxml_xpath_lowerer_finalize(lowerer);
    return status;
}

sxml_status sxml_xslt_lower_initial_template(const sxml_xslt_lowering_request *request,
                                             sxml_ir_unit **unit) {
    if (request == NULL || request->ast == NULL || !request->ast->is_sealed ||
        request->processor == NULL || unit == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const invocation_choice choice = choose_invocation(request);
    if (!choice.is_valid) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = request->processor;
    const sxml_xpath_static_context base_context = {
        .item_heap = processor->item_heap,
        .allocator = processor->allocator,
        .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
        .variable_declarations = {.variable_declarations = NULL, .variable_declaration_count = 0},
        .function_table = sxml_processor_get_function_table(processor),
        .static_base_uri = {.bytes = NULL, .byte_count = 0},
        /* 3.13: the stylesheet's in-scope schemas, in every module
           (src/xslt_schema_internal.h, D179). */
        .unit_static_context = {.in_scope_schemas =
                                    sxml_xslt_ast_get_in_scope_schemas(request->ast)}};
    sxml_ir_unit *new_unit = NULL;
    sxml_status status = sxml_ir_unit_create(
        processor->allocator,
        &(sxml_ir_unit_configuration){
            .allocator = processor->allocator,
            .item_heap = processor->item_heap,
            .source_name =
                sxml_xslt_ast_get_module(request->ast, (sxml_xslt_ast_module_index){.value = 1})
                    ->module_uri,
            .maximum_expression_count = request->maximum_expression_count},
        &new_unit);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = lower_into_unit(&(unit_lowering){.request = request,
                                              .invocation = choice.invocation,
                                              .base_context = &base_context,
                                              .unit = new_unit});
    sxml_diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof(diagnostic));
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_run_passes(
            &(sxml_ir_pass_list_request){.pass_request = {.unit = new_unit,
                                                          .type_registry = processor->type_registry,
                                                          .error_record = request->error_record,
                                                          .diagnostic = &diagnostic},
                                         .passes = sxml_ir_standard_passes,
                                         .verifies_after_each_pass = false});
        if (status == SXML_STATUS_INVALID_ARGUMENT && request->error_record != NULL) {
            request->error_record->diagnostic = diagnostic;
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_ir_unit_release(new_unit);
        return status;
    }
    sxml_ir_unit_seal(new_unit);
    *unit = new_unit;
    return SXML_STATUS_OK;
}
