/*
 * Assertions and type alternatives: compile stage ASSERTIONS and their
 * evaluation during validation (src/schema_internal.h, docs/DECISIONS.md
 * D165, D167, D175).
 *
 * Section numbers are of XSD 1.1 Part 1 unless they name Part 2.
 *
 * The tests are XPath, compiled with include/sxml/xpath.h and evaluated by
 * the one evaluator, so there is no second engine: the XPath 2.0 of XSD
 * 1.1 is read by the XPath 4.0 front end, a superset that XSD 1.1 allows
 * as an implementation-defined extension (3.13.6.2, "the function
 * signatures are implementation-defined"). The static context is the one
 * XPath Valid (3.13.6.2) gives: the statically known namespaces are the
 * record's {namespace bindings}, the default element/type namespace is
 * its {default namespace} (xpathDefaultNamespace), the default function
 * namespace fn, the base URI its {base URI}, the codepoint collation the
 * default collation and XPath 1.0 compatibility mode off; assertions
 * declare $value as xs:anyAtomicType* (3.13.6.1, Part 2 4.3.13.3), type
 * alternatives no variable.
 *
 * Evaluation (3.13.4.2): the context item is the element (an assertion of
 * a complex type: its recorded subtree, 3.13.4.1; a type alternative: a
 * parentless copy of the element with its attributes and inherited
 * attributes and no children, 3.12.4), absent for an assertion facet
 * (Part 2, 4.3.13.3 clause 1.2); $value is the typed value of simple
 * content or the simple value. The result is taken as fn:boolean takes
 * it; a dynamic or type error makes the test false (3.12.4 clause 2,
 * 3.13.4.1).
 *
 * Golden file: src/schema_simple_type.c (a compile stage over one table,
 * reporting through sxml_schema_compilation_report).
 */
#include "schema_validate_internal.h"

#include "cast_internal.h"
#include "evaluator_internal.h"
#include "tree_internal.h"
#include "xdm_internal.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* The local name of the variable assertions declare (3.13.6.1). */
static const sxml_byte_view value_local_name = SXML_BYTE_VIEW_INITIALIZER("value");

/* ------------------------------------------------------- XPath records */

/* The statically known namespaces of a record (3.13.6.2 clauses 2.2.2 and
   2.2.3): its bindings with a prefix, each prefix once (the xml prefix is
   predeclared), and its default namespace as the binding of prefix 0, in
   bindings. */
typedef struct binding_gathering {
    const sxml_schema_components *components;
    sxml_namespace_prefix_identifier xml_prefix;
    sxml_schema_scratch_array *bindings;
} binding_gathering;

static sxml_status gather_bindings(binding_gathering gathering,
                                   const sxml_schema_namespace_context *context,
                                   sxml_namespace_binding_view *view) {
    sxml_schema_scratch_array *kept = gathering.bindings;
    const sxml_namespace_binding_view bindings =
        sxml_schema_get_namespace_bindings(gathering.components, context->namespace_bindings);
    sxml_status status = SXML_STATUS_OK;
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding *binding = &bindings.namespace_bindings[binding_index];
        bool is_skipped = binding->namespace_prefix.index == 0 ||
                          binding->namespace_prefix.index == gathering.xml_prefix.index ||
                          binding->namespace_uri.index == SXML_NAMESPACE_NONE;
        for (size_t earlier = 0; !is_skipped && earlier < kept->entry_count; earlier += 1) {
            const sxml_namespace_binding *known = sxml_schema_scratch_array_at(kept, earlier);
            is_skipped = known->namespace_prefix.index == binding->namespace_prefix.index;
        }
        if (!is_skipped) {
            status = sxml_schema_scratch_array_append(gathering.components->allocator, kept,
                                                      binding);
        }
    }
    if (status == SXML_STATUS_OK && context->default_namespace.index != SXML_NAMESPACE_NONE) {
        const sxml_namespace_binding default_binding = {
            .namespace_prefix = {.index = 0}, .namespace_uri = context->default_namespace};
        status = sxml_schema_scratch_array_append(gathering.components->allocator, kept,
                                                  &default_binding);
    }
    *view = (sxml_namespace_binding_view){
        .namespace_bindings = sxml_schema_scratch_array_at(kept, 0),
        .namespace_binding_count = kept->entry_count};
    return status;
}

/* The expression compiled in the record's static context, or a report. */
typedef struct expression_compilation {
    sxml_schema_compilation *compilation;
    const sxml_schema_xpath_compilation *request;
    sxml_namespace_binding_view bindings;
} expression_compilation;

static sxml_status compile_expression(expression_compilation compilation,
                                      sxml_xpath_expression **expression) {
    sxml_schema_components *components = compilation.compilation->components;
    const sxml_schema_xpath_compilation *request = compilation.request;
    const sxml_schema_xpath_record *record =
        sxml_schema_get_xpath_record(components, request->record);
    const sxml_schema_namespace_context *context =
        sxml_schema_get_namespace_context(components, record->namespace_context);
    sxml_name_identifier value_name = {.index = 0};
    sxml_status status = SXML_STATUS_OK;
    if (request->declares_value) {
        status = sxml_name_pool_intern_name(
            sxml_processor_get_name_pool(components->processor),
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = value_local_name},
            &value_name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view base_uri = sxml_schema_get_text(components, context->base_uri);
    /* XPath 4.0, 2.2.1: the static base URI is absolute; a schema read
       without an absolute URI has none. */
    const sxml_byte_view static_base_uri =
        base_uri.byte_count != 0 && sxml_uri_has_scheme(base_uri)
            ? base_uri
            : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    const sxml_variable_declaration value_declaration = {
        .variable_name = value_name,
        .declared_type = {.item_type = {.index = SXML_TYPE_XS_ANY_ATOMIC_TYPE},
                          .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    status = sxml_xpath_expression_create(
        components->allocator,
        &(sxml_xpath_expression_configuration){
            .processor = components->processor,
            .expression_text = sxml_schema_get_text(components, record->expression),
            .source_name = static_base_uri,
            .static_context =
                {.namespace_bindings = compilation.bindings,
                 .static_base_uri = static_base_uri,
                 .variable_declarations = {.variable_declarations = &value_declaration,
                                           .variable_declaration_count =
                                               request->declares_value ? 1U : 0U}},
            .error_record = &error_record},
        expression);
    if (status == SXML_STATUS_STATIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        /* 3.13.6.2 XPath Valid: a static error (and a type error found
           statically, an undeclared prefix, say) makes the schema
           invalid. */
        *expression = NULL;
        return sxml_schema_compilation_report(
            compilation.compilation,
            &(sxml_schema_report_request){
                .constraint_name = request->constraint_name,
                .description = request->description,
                .subject = error_record.diagnostic.description,
                .position = record->position});
    }
    return status;
}

sxml_status sxml_schema_compile_xpath_record(sxml_schema_compilation *compilation,
                                             const sxml_schema_xpath_compilation *request) {
    sxml_schema_components *components = compilation->components;
    const sxml_schema_xpath_record *record =
        sxml_schema_get_xpath_record(components, request->record);
    if (request->record.value == 0 || record->compiled_expression != NULL) {
        return SXML_STATUS_OK;
    }
    sxml_schema_scratch_array bindings = {.bytes = NULL,
                                          .entry_byte_count = sizeof(sxml_namespace_binding),
                                          .entry_count = 0,
                                          .entry_capacity = 0};
    binding_gathering gathering = {
        .components = components, .xml_prefix = {.index = 0}, .bindings = &bindings};
    sxml_status status = sxml_name_pool_intern_namespace_prefix(
        sxml_processor_get_name_pool(components->processor), TEXT("xml"), &gathering.xml_prefix);
    sxml_namespace_binding_view binding_view = {.namespace_bindings = NULL,
                                                .namespace_binding_count = 0};
    if (status == SXML_STATUS_OK) {
        status = gather_bindings(
            gathering, sxml_schema_get_namespace_context(components, record->namespace_context),
            &binding_view);
    }
    sxml_xpath_expression *expression = NULL;
    if (status == SXML_STATUS_OK) {
        status = compile_expression((expression_compilation){.compilation = compilation,
                                                             .request = request,
                                                             .bindings = binding_view},
                                    &expression);
    }
    sxml_schema_scratch_array_release(components->allocator, &bindings);
    if (status != SXML_STATUS_OK || expression == NULL) {
        return status;
    }
    sxml_schema_xpath_record *edited = sxml_schema_components_edit_entry(
        components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_XPATH_RECORD,
                                            .index = request->record.value});
    if (edited == NULL) {
        sxml_xpath_expression_destroy(expression);
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    edited->compiled_expression = expression;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- The stage */

sxml_status sxml_schema_compile_assertions(sxml_schema_compilation *compilation) {
    sxml_schema_components *components = compilation->components;
    sxml_status status = SXML_STATUS_OK;
    const uint32_t assertion_count =
        components->tables[SXML_SCHEMA_TABLE_ASSERTION].entry_count;
    for (uint32_t assertion = 1; status == SXML_STATUS_OK && assertion < assertion_count;
         assertion += 1) {
        status = sxml_schema_compile_xpath_record(
            compilation,
            &(sxml_schema_xpath_compilation){
                .record = sxml_schema_get_assertion(
                              components, (sxml_schema_assertion_index){.value = assertion})
                              ->test,
                .declares_value = true,
                .constraint_name = TEXT("as-props-correct.2"),
                .description = TEXT("The test is not a valid XPath expression")});
    }
    const uint32_t alternative_count =
        components->tables[SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE].entry_count;
    for (uint32_t alternative = 1; status == SXML_STATUS_OK && alternative < alternative_count;
         alternative += 1) {
        status = sxml_schema_compile_xpath_record(
            compilation,
            &(sxml_schema_xpath_compilation){
                .record = sxml_schema_get_type_alternative(
                              components,
                              (sxml_schema_type_alternative_index){.value = alternative})
                              ->test,
                .declares_value = false,
                .constraint_name = TEXT("ta-props-correct.2"),
                .description = TEXT("The test is not a valid XPath expression")});
    }
    return status;
}

/* ------------------------------------------------------------ Evaluation */

/* One evaluation of a compiled test (3.13.4.2). */
typedef struct test_evaluation {
    const sxml_schema *schema;
    sxml_schema_xpath_record_index test;
    /* The context item; none for an assertion facet. */
    sxml_item context_node;
    /* $value, for assertions. */
    bool binds_value;
    sxml_sequence value;
} test_evaluation;

/* The resources a test reads (3.13.4.2 clauses 8 to 10): no available
   documents and collections, and the empty sequence as the default
   collection. */
static sxml_status resolve_no_resource(void *state, const sxml_resource_request *request,
                                       sxml_resource *resource) {
    (void)state;
    if ((request->resource_kind == SXML_RESOURCE_KIND_COLLECTION ||
         request->resource_kind == SXML_RESOURCE_KIND_URI_COLLECTION) &&
        request->is_default_collection) {
        resource->resource_form = SXML_RESOURCE_FORM_SEQUENCE;
        resource->items = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    resource->resource_form = SXML_RESOURCE_FORM_NONE;
    return SXML_STATUS_OK;
}

static const sxml_resource_resolver_operations no_resource_operations = {
    .resolve = resolve_no_resource, .release = NULL};

typedef struct test_outcome {
    bool is_true;
} test_outcome;

/* An error of the test, which makes it false (3.12.4 clause 2,
   3.13.4.1): not one of the run (memory, limits, the host). */
static bool is_test_error(sxml_status status) {
    return status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR;
}

static sxml_status evaluate_test(const test_evaluation *evaluation, test_outcome *outcome) {
    outcome->is_true = false;
    const sxml_schema_xpath_record *record =
        sxml_schema_get_xpath_record(&evaluation->schema->components, evaluation->test);
    if (record->compiled_expression == NULL) {
        /* Compilation reported it, or the stage did not run. */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_name_identifier value_name = {.index = 0};
    const bool has_value_name = sxml_name_pool_find_name(
        sxml_processor_get_name_pool(evaluation->schema->processor),
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = value_local_name},
        &value_name);
    const sxml_variable_binding value_binding = {.variable_name = value_name,
                                                 .value = evaluation->value};
    const bool has_context =
        evaluation->context_node.representation != SXML_ITEM_REPRESENTATION_NONE;
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_sequence result = sxml_sequence_empty();
    sxml_status status = sxml_xpath_expression_evaluate(
        record->compiled_expression,
        &(sxml_xpath_evaluation_request){
            .dynamic_context =
                {.has_context_value = has_context,
                 /* Borrowed for the call. */
                 .context_value = has_context ? sxml_sequence_from_item(evaluation->context_node)
                                              : sxml_sequence_empty(),
                 .variable_bindings = {.variable_bindings = &value_binding,
                                       .variable_binding_count =
                                           evaluation->binds_value && has_value_name ? 1U : 0U},
                 .resource_resolver = {.state = NULL, .operations = &no_resource_operations}},
            .error_record = &error_record},
        &result);
    if (is_test_error(status)) {
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* 3.13.4.1 clause 3, 3.12.4: as if by fn:boolean. */
    sxml_effective_boolean_value effective_boolean_value = {.is_true = false};
    status = sxml_evaluator_compute_effective_boolean_value(result, NULL, &effective_boolean_value);
    sxml_sequence_release(result);
    if (is_test_error(status)) {
        return SXML_STATUS_OK;
    }
    outcome->is_true = effective_boolean_value.is_true;
    return status;
}

/* ----------------------------------------------------- Type alternatives */

sxml_status sxml_schema_select_type_alternative(
    sxml_schema_validator *validator, const sxml_schema_type_alternative_request *request,
    sxml_schema_type_definition_index *selected_type) {
    (void)validator;
    const sxml_schema_components *components = &request->schema->components;
    const sxml_schema_element_declaration *declaration =
        sxml_schema_get_element_declaration(components, request->element_declaration);
    /* 3.3.4.1: the selected type definition is that of the first
       alternative whose test succeeds (3.12.4), or of the default
       alternative (no test, last); otherwise the declared type. */
    *selected_type = declaration->type_definition;
    const sxml_schema_index_view alternatives =
        sxml_schema_get_list(components, declaration->type_alternatives);
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < alternatives.index_count;
         entry += 1) {
        const sxml_schema_type_alternative *alternative = sxml_schema_get_type_alternative(
            components, (sxml_schema_type_alternative_index){.value = alternatives.indexes[entry]});
        test_outcome outcome = {.is_true = alternative->test.value == 0};
        if (!outcome.is_true) {
            status = evaluate_test(&(test_evaluation){.schema = request->schema,
                                                      .test = alternative->test,
                                                      .context_node = request->element_node,
                                                      .binds_value = false,
                                                      .value = sxml_sequence_empty()},
                                   &outcome);
        }
        if (status == SXML_STATUS_OK && outcome.is_true) {
            *selected_type = alternative->type_definition;
            break;
        }
    }
    return status;
}

/* The tests of two alternatives are the same XPath record, in effect
   (3.8.6.3, equivalent type alternatives, clauses 1 to 4). */
static bool tests_are_same(const sxml_schema_components *components,
                           sxml_schema_xpath_record_index first,
                           sxml_schema_xpath_record_index second) {
    if (first.value == second.value) {
        return true;
    }
    if (first.value == 0 || second.value == 0) {
        return false;
    }
    const sxml_schema_xpath_record *first_record = sxml_schema_get_xpath_record(components, first);
    const sxml_schema_xpath_record *second_record =
        sxml_schema_get_xpath_record(components, second);
    if (!sxml_byte_view_equals(sxml_schema_get_text(components, first_record->expression),
                               sxml_schema_get_text(components, second_record->expression))) {
        return false;
    }
    if (first_record->namespace_context.value == second_record->namespace_context.value) {
        return true;
    }
    const sxml_schema_namespace_context *first_context =
        sxml_schema_get_namespace_context(components, first_record->namespace_context);
    const sxml_schema_namespace_context *second_context =
        sxml_schema_get_namespace_context(components, second_record->namespace_context);
    const sxml_namespace_binding_view first_bindings =
        sxml_schema_get_namespace_bindings(components, first_context->namespace_bindings);
    const sxml_namespace_binding_view second_bindings =
        sxml_schema_get_namespace_bindings(components, second_context->namespace_bindings);
    if (first_context->default_namespace.index != second_context->default_namespace.index ||
        first_bindings.namespace_binding_count != second_bindings.namespace_binding_count ||
        !sxml_byte_view_equals(sxml_schema_get_text(components, first_context->base_uri),
                               sxml_schema_get_text(components, second_context->base_uri))) {
        return false;
    }
    for (size_t first_index = 0; first_index < first_bindings.namespace_binding_count;
         first_index += 1) {
        const sxml_namespace_binding *binding = &first_bindings.namespace_bindings[first_index];
        bool is_found = false;
        for (size_t second_index = 0;
             !is_found && second_index < second_bindings.namespace_binding_count;
             second_index += 1) {
            const sxml_namespace_binding *other = &second_bindings.namespace_bindings[second_index];
            is_found = other->namespace_prefix.index == binding->namespace_prefix.index &&
                       other->namespace_uri.index == binding->namespace_uri.index;
        }
        if (!is_found) {
            return false;
        }
    }
    return true;
}

/* A type table seen as 3.12.2 defines it: its alternatives with a test,
   and its default type definition (the last alternative's without a
   test, else the declaration's type). */
typedef struct type_table {
    sxml_schema_index_view alternatives;
    size_t tested_count;
    sxml_schema_type_definition_index default_type;
} type_table;

static type_table table_of(const sxml_schema_components *components,
                           sxml_schema_element_declaration_index declaration_index) {
    const sxml_schema_element_declaration *declaration =
        sxml_schema_get_element_declaration(components, declaration_index);
    type_table table = {.alternatives =
                            sxml_schema_get_list(components, declaration->type_alternatives),
                        .tested_count = 0,
                        .default_type = declaration->type_definition};
    table.tested_count = table.alternatives.index_count;
    if (table.tested_count != 0) {
        const sxml_schema_type_alternative *last = sxml_schema_get_type_alternative(
            components, (sxml_schema_type_alternative_index){
                            .value = table.alternatives.indexes[table.tested_count - 1U]});
        if (last->test.value == 0) {
            table.tested_count -= 1;
            table.default_type = last->type_definition;
        }
    }
    return table;
}

bool sxml_schema_type_tables_are_equivalent(const sxml_schema_components *components,
                                            const sxml_schema_type_table_pair *pair) {
    if (pair->first.value == pair->second.value) {
        return true;
    }
    const type_table first = table_of(components, pair->first);
    const type_table second = table_of(components, pair->second);
    if (first.alternatives.index_count == 0 || second.alternatives.index_count == 0) {
        return first.alternatives.index_count == second.alternatives.index_count;
    }
    if (first.tested_count != second.tested_count ||
        first.default_type.value != second.default_type.value) {
        return false;
    }
    for (size_t entry = 0; entry < first.tested_count; entry += 1) {
        if (first.alternatives.indexes[entry] == second.alternatives.indexes[entry]) {
            continue;
        }
        const sxml_schema_type_alternative *first_alternative = sxml_schema_get_type_alternative(
            components, (sxml_schema_type_alternative_index){.value = first.alternatives.indexes[entry]});
        const sxml_schema_type_alternative *second_alternative = sxml_schema_get_type_alternative(
            components,
            (sxml_schema_type_alternative_index){.value = second.alternatives.indexes[entry]});
        if (first_alternative->type_definition.value != second_alternative->type_definition.value ||
            !tests_are_same(components, first_alternative->test, second_alternative->test)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------ Assertions */

/* The location of a node of the recorded subtree, for the report. */
static sxml_source_location location_of(sxml_item node_item) {
    sxml_source_location location;
    memset(&location, 0, sizeof(location));
    sxml_node_reference node;
    if (sxml_item_get_node(node_item, &node) != SXML_STATUS_OK ||
        node.tree->operations->get_location == NULL ||
        node.tree->operations->get_location(node.tree->state, node.node_index, &location) !=
            SXML_STATUS_OK) {
        memset(&location, 0, sizeof(location));
    }
    return location;
}

/*
 * The tree an assertion of a complex type sees (3.13.4.1 clauses 1.2 and
 * 1.3): the element with its validated descendants and nothing outside
 * it, the element itself only partially validated, so annotated
 * xs:anyType (XDM 4.0, 3.3.1.1: an element whose validity is not known;
 * the note under clause 2.3.1: "will normally have the type annotation
 * anyType"). A copy of the recorded subtree's element through a builder
 * that retypes the copy's root.
 */
typedef struct root_retyping {
    sxml_tree_builder target;
    bool has_root;
} root_retyping;

static sxml_tree_builder target_of(void *state) {
    const root_retyping *retyping = state;
    return retyping->target;
}

static sxml_status retype_start_document(void *state, const sxml_document_start_event *event) {
    const sxml_tree_builder target = target_of(state);
    return target.operations->start_document(target.state, event);
}

static sxml_status retype_end_document(void *state) {
    const sxml_tree_builder target = target_of(state);
    return target.operations->end_document(target.state);
}

static sxml_status retype_start_element(void *state, const sxml_element_start_event *event) {
    root_retyping *retyping = state;
    sxml_element_start_event retyped = *event;
    if (!retyping->has_root) {
        retyping->has_root = true;
        retyped.type_annotation.index = SXML_TYPE_XS_ANY_TYPE;
        retyped.is_id = false;
        retyped.is_idrefs = false;
    }
    return retyping->target.operations->start_element(retyping->target.state, &retyped);
}

static sxml_status retype_add_namespace_binding(void *state,
                                                const sxml_namespace_binding *namespace_binding) {
    const sxml_tree_builder target = target_of(state);
    return target.operations->add_namespace_binding(target.state, namespace_binding);
}

static sxml_status retype_add_attribute(void *state, const sxml_attribute_event *event) {
    const sxml_tree_builder target = target_of(state);
    return target.operations->add_attribute(target.state, event);
}

static sxml_status retype_end_element(void *state) {
    const sxml_tree_builder target = target_of(state);
    return target.operations->end_element(target.state);
}

static sxml_status retype_add_text(void *state, const sxml_text_event *event) {
    const sxml_tree_builder target = target_of(state);
    return target.operations->add_text(target.state, event);
}

static sxml_status retype_add_comment(void *state, const sxml_comment_event *event) {
    const sxml_tree_builder target = target_of(state);
    return target.operations->add_comment(target.state, event);
}

static sxml_status
retype_add_processing_instruction(void *state, const sxml_processing_instruction_event *event) {
    const sxml_tree_builder target = target_of(state);
    return target.operations->add_processing_instruction(target.state, event);
}

static sxml_status retype_add_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    const sxml_tree_builder target = target_of(state);
    return target.operations->add_unparsed_entity(target.state, entity);
}

static const sxml_tree_builder_operations retyping_operations = {
    .start_document = retype_start_document,
    .end_document = retype_end_document,
    .start_element = retype_start_element,
    .add_namespace_binding = retype_add_namespace_binding,
    .add_attribute = retype_add_attribute,
    .end_element = retype_end_element,
    .add_text = retype_add_text,
    .add_comment = retype_add_comment,
    .add_processing_instruction = retype_add_processing_instruction,
    .add_unparsed_entity = retype_add_unparsed_entity,
};

/* The copy assertions evaluate on: its arena tree, which owns the tree
   (D39) and is destroyed after the evaluations, and its element. */
typedef struct assertion_copy {
    sxml_arena_tree *arena_tree;
    sxml_item element_node;
} assertion_copy;

/* Copies the element of node_item into a new tree whose root is
   xs:anyType. */
static sxml_status copy_for_assertions(sxml_schema_validator *validator, sxml_item node_item,
                                       assertion_copy *copy) {
    sxml_node_reference node;
    sxml_status status = sxml_item_get_node(node_item, &node);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validator_create_tree(validator, &copy->arena_tree);
    }
    if (status != SXML_STATUS_OK) {
        copy->arena_tree = NULL;
        return status;
    }
    root_retyping retyping = {.target = sxml_arena_tree_as_tree_builder(copy->arena_tree),
                              .has_root = false};
    status = sxml_tree_builder_copy_node(
        (sxml_tree_builder){.state = &retyping, .operations = &retyping_operations},
        &(sxml_copy_request){.node = node,
                             .preserves_namespaces = true,
                             .inherits_namespaces = true,
                             .preserves_type_annotations = true});
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(copy->arena_tree, &tree);
    }
    if (status == SXML_STATUS_OK) {
        /* The item takes the caller's reference to the tree. */
        copy->element_node = sxml_node_item_make(
            (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
        sxml_tree_release(tree);
    }
    return status;
}

/* Releases the copy's element and destroys its tree. */
static void release_copy(assertion_copy *copy) {
    sxml_item_release(copy->element_node);
    sxml_arena_tree_destroy(copy->arena_tree);
    *copy = (assertion_copy){.arena_tree = NULL,
                             .element_node = {.representation = SXML_ITEM_REPRESENTATION_NONE}};
}

/* $value of an element (3.13.4.1 clause 2.3): the typed value of simple
   content, the empty sequence for other content, a nilled element or
   simple content without a typed value. Owned. */
static sxml_status element_value(const sxml_schema_assertion_request *request,
                                 const sxml_schema_type_definition *definition,
                                 sxml_sequence *value) {
    *value = sxml_sequence_empty();
    sxml_node_reference node;
    if (definition->content_type_variety != SXML_CONTENT_TYPE_VARIETY_SIMPLE ||
        sxml_item_get_node(request->node, &node) != SXML_STATUS_OK) {
        return SXML_STATUS_OK;
    }
    sxml_sequence typed_value = sxml_sequence_empty();
    const sxml_status status = sxml_node_make_typed_value(
        sxml_processor_get_item_heap(request->schema->processor),
        &(sxml_node_value_request){.node = node, .error_record = NULL}, &typed_value);
    if (status == SXML_STATUS_OK) {
        *value = typed_value;
        return SXML_STATUS_OK;
    }
    return is_test_error(status) ? SXML_STATUS_OK : status;
}

/* What checks run with: the validator and the schema. */
typedef struct assertion_run {
    sxml_schema_validator *validator;
    const sxml_schema *schema;
} assertion_run;

static const sxml_schema_type_definition *type_of(const assertion_run *run, uint32_t index) {
    return sxml_schema_get_type_definition(&run->schema->components,
                                           (sxml_schema_type_definition_index){.value = index});
}

/* A value and the type whose {assertions} it is checked against. */
typedef struct typed_check {
    uint32_t type_definition;
    sxml_sequence value;
} typed_check;

/* Whether every assertion of the type holds for the value, as $value with
   no context item (Part 2, 4.3.13.3). */
static sxml_status assertions_hold(const assertion_run *run, typed_check check,
                                   test_outcome *outcome) {
    outcome->is_true = true;
    const sxml_schema_index_view assertions = sxml_schema_get_list(
        &run->schema->components, type_of(run, check.type_definition)->assertions);
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry = 0; status == SXML_STATUS_OK && outcome->is_true &&
                           entry < assertions.index_count;
         entry += 1) {
        const sxml_schema_assertion *assertion = sxml_schema_get_assertion(
            &run->schema->components,
            (sxml_schema_assertion_index){.value = assertions.indexes[entry]});
        status = evaluate_test(
            &(test_evaluation){.schema = run->schema,
                               .test = assertion->test,
                               .context_node = {.representation = SXML_ITEM_REPRESENTATION_NONE},
                               .binds_value = true,
                               .value = check.value},
            outcome);
    }
    return status;
}

/* The basic members of a union (Part 2, 2.4.1.3: its members with each
   union member replaced by its own, in order), with an explicit stack
   (D71). */
typedef struct member_list {
    /* uint32_t: type definitions to expand, last on top. */
    sxml_schema_scratch_array stack;
    /* uint32_t: the basic members, atomic or list. */
    sxml_schema_scratch_array members;
} member_list;

static sxml_status push_members_reversed(const assertion_run *run, member_list *list,
                                         uint32_t union_type) {
    const sxml_schema_index_view members =
        sxml_schema_get_list(&run->schema->components, type_of(run, union_type)->member_types);
    sxml_status status = SXML_STATUS_OK;
    for (size_t remaining = members.index_count; status == SXML_STATUS_OK && remaining > 0;
         remaining -= 1) {
        status = sxml_schema_scratch_array_append(run->validator->allocator, &list->stack,
                                                  &members.indexes[remaining - 1U]);
    }
    return status;
}

static sxml_status gather_basic_members(const assertion_run *run, member_list *list,
                                        uint32_t union_type) {
    sxml_status status = push_members_reversed(run, list, union_type);
    while (status == SXML_STATUS_OK && list->stack.entry_count > 0) {
        list->stack.entry_count -= 1;
        const uint32_t member =
            *(const uint32_t *)sxml_schema_scratch_array_at(&list->stack, list->stack.entry_count);
        if (type_of(run, member)->variety == SXML_TYPE_VARIETY_UNION) {
            status = push_members_reversed(run, list, member);
        } else {
            status = sxml_schema_scratch_array_append(run->validator->allocator, &list->members,
                                                      &member);
        }
    }
    return status;
}

/* Whether a value's first item is of a basic member: an atomic member's
   registered type, or a list member's item type. */
static bool is_of_member(const assertion_run *run, sxml_sequence value, uint32_t member) {
    sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    if (sxml_sequence_get_item(value, (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &item) != SXML_STATUS_OK) {
        return false;
    }
    const sxml_schema_type_definition *definition = type_of(run, member);
    const uint32_t atomic_type =
        definition->variety == SXML_TYPE_VARIETY_LIST ? definition->item_type.value : member;
    return sxml_item_get_type_annotation(item).index ==
           type_of(run, atomic_type)->registered_type.index;
}

/* The items of a list value against its item type's assertions (an atomic
   item type's; of a union item type, the active member's, Part 2,
   4.3.13.3 "XDM representation" clause 3). */
static sxml_status list_items_hold(const assertion_run *run, typed_check list,
                                   test_outcome *outcome) {
    outcome->is_true = true;
    const uint32_t item_type = type_of(run, list.type_definition)->item_type.value;
    member_list members = {.stack = {.entry_byte_count = sizeof(uint32_t)},
                           .members = {.entry_byte_count = sizeof(uint32_t)}};
    sxml_status status = SXML_STATUS_OK;
    if (type_of(run, item_type)->variety == SXML_TYPE_VARIETY_UNION) {
        status = gather_basic_members(run, &members, item_type);
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(list.value);
    while (status == SXML_STATUS_OK && outcome->is_true) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        const sxml_sequence item = sxml_sequence_from_item(step.item);
        uint32_t checked_type = item_type;
        for (size_t entry = 0; entry < members.members.entry_count; entry += 1) {
            const uint32_t member =
                *(const uint32_t *)sxml_schema_scratch_array_at(&members.members, entry);
            if (is_of_member(run, item, member)) {
                checked_type = member;
                break;
            }
        }
        status = assertions_hold(run, (typed_check){.type_definition = checked_type, .value = item},
                                 outcome);
    }
    sxml_schema_scratch_array_release(run->validator->allocator, &members.stack);
    sxml_schema_scratch_array_release(run->validator->allocator, &members.members);
    return status;
}

/* A value of a basic member against the member's assertions, and a list
   member's items against its item type's. */
static sxml_status member_holds(const assertion_run *run, typed_check check,
                                test_outcome *outcome) {
    sxml_status status = assertions_hold(run, check, outcome);
    if (status == SXML_STATUS_OK && outcome->is_true &&
        type_of(run, check.type_definition)->variety == SXML_TYPE_VARIETY_LIST) {
        status = list_items_hold(run, check, outcome);
    }
    return status;
}

/* A text and its value as the cast engine read it against a union. */
typedef struct union_check {
    uint32_t union_type;
    sxml_sequence value;
    sxml_byte_view text;
} union_check;

/* A union value (Part 2, 4.3.13.3 clause 4): the member the cast engine
   chose, which ignores assertions, and when its assertions fail, each
   later basic member the text is valid for, as the active basic member is
   the first member the value is valid against. */
static sxml_status union_holds(const assertion_run *run, union_check check,
                               test_outcome *outcome) {
    outcome->is_true = true;
    member_list members = {.stack = {.entry_byte_count = sizeof(uint32_t)},
                           .members = {.entry_byte_count = sizeof(uint32_t)}};
    sxml_status status = gather_basic_members(run, &members, check.union_type);
    size_t active = members.members.entry_count;
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < members.members.entry_count;
         entry += 1) {
        const uint32_t member =
            *(const uint32_t *)sxml_schema_scratch_array_at(&members.members, entry);
        if (is_of_member(run, check.value, member)) {
            active = entry;
            break;
        }
    }
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    if (status == SXML_STATUS_OK && active + 1U < members.members.entry_count) {
        status = sxml_schema_validator_get_cast_bindings(run->validator, &bindings);
    }
    for (size_t entry = active; status == SXML_STATUS_OK && entry < members.members.entry_count;
         entry += 1) {
        const uint32_t member =
            *(const uint32_t *)sxml_schema_scratch_array_at(&members.members, entry);
        sxml_sequence value = check.value;
        if (entry != active) {
            value = sxml_sequence_empty();
            const sxml_status cast_status = sxml_cast_make_value_from_text(
                run->validator->item_heap,
                &(sxml_text_cast_request){
                    .text = check.text,
                    .target_type = sxml_schema_cast_target_of(type_of(run, member)->registered_type),
                    .namespace_bindings = bindings,
                    .error_record = NULL},
                &value);
            if (cast_status != SXML_STATUS_OK) {
                if (!is_test_error(cast_status) && cast_status != SXML_STATUS_STATIC_ERROR &&
                    cast_status != SXML_STATUS_INVALID_ARGUMENT) {
                    status = cast_status;
                }
                continue;
            }
        }
        status = member_holds(run, (typed_check){.type_definition = member, .value = value},
                              outcome);
        if (entry != active) {
            sxml_sequence_release(value);
        }
        if (status != SXML_STATUS_OK || outcome->is_true) {
            break;
        }
    }
    sxml_schema_scratch_array_release(run->validator->allocator, &members.stack);
    sxml_schema_scratch_array_release(run->validator->allocator, &members.members);
    return status;
}

/* A simple value (Part 2, 4.3.13.3; 4.1.4 clause 2.3: a list's items are
   valid for its item type, a union's value for its active member): the
   type's assertions, then those of its items or of its active member. */
/* Whether a simple type, its item type or one of its members has
   assertions (a member that is a union or list is assumed to), so most
   values need no more than this look. */
static bool may_have_assertions(const assertion_run *run, uint32_t simple_type) {
    const sxml_schema_type_definition *definition = type_of(run, simple_type);
    if (definition->assertions.entry_count != 0) {
        return true;
    }
    if (definition->variety == SXML_TYPE_VARIETY_LIST) {
        const sxml_schema_type_definition *item = type_of(run, definition->item_type.value);
        return item->assertions.entry_count != 0 || item->variety == SXML_TYPE_VARIETY_UNION;
    }
    if (definition->variety != SXML_TYPE_VARIETY_UNION) {
        return false;
    }
    const sxml_schema_index_view members =
        sxml_schema_get_list(&run->schema->components, definition->member_types);
    for (size_t entry = 0; entry < members.index_count; entry += 1) {
        const sxml_schema_type_definition *member = type_of(run, members.indexes[entry]);
        if (member->assertions.entry_count != 0 || member->variety != SXML_TYPE_VARIETY_ATOMIC) {
            return true;
        }
    }
    return false;
}

static sxml_status check_simple_value(const assertion_run *run,
                                      const sxml_schema_assertion_request *request) {
    const uint32_t simple_type = request->type_definition.value;
    if (!may_have_assertions(run, simple_type)) {
        return SXML_STATUS_OK;
    }
    test_outcome outcome = {.is_true = true};
    sxml_status status = assertions_hold(
        run, (typed_check){.type_definition = simple_type, .value = request->value}, &outcome);
    const sxml_type_variety variety = type_of(run, simple_type)->variety;
    if (status == SXML_STATUS_OK && outcome.is_true && variety == SXML_TYPE_VARIETY_LIST) {
        status = list_items_hold(
            run, (typed_check){.type_definition = simple_type, .value = request->value}, &outcome);
    }
    if (status == SXML_STATUS_OK && outcome.is_true && variety == SXML_TYPE_VARIETY_UNION) {
        status = union_holds(run,
                             (union_check){.union_type = simple_type,
                                           .value = request->value,
                                           .text = request->text},
                             &outcome);
    }
    if (status != SXML_STATUS_OK || outcome.is_true) {
        return status;
    }
    return sxml_schema_validator_report_error(
        run->validator,
        &(sxml_schema_validation_error){
            .constraint_name = TEXT("cvc-assertions-valid"),
            .message = {.text = TEXT("the value %v breaks an assertion of %t"),
                        .value = request->text,
                        .type_definition = request->type_definition},
            .location = request->location});
}

/* The element a node item is, as written, for a report. */
static sxml_schema_written_name element_of(sxml_item node_item) {
    sxml_node_reference node;
    sxml_node_description description;
    if (sxml_item_get_node(node_item, &node) != SXML_STATUS_OK ||
        sxml_node_describe(node, &description) != SXML_STATUS_OK) {
        return (sxml_schema_written_name){.name = {.index = 0}, .namespace_prefix = {.index = 0}};
    }
    return (sxml_schema_written_name){.name = description.node_name,
                                      .namespace_prefix = description.namespace_prefix};
}

/* The assertions of a complex type on the element of its recorded subtree
   (3.13.4.1), each false one reported. */
static sxml_status check_element(const assertion_run *run,
                                 const sxml_schema_assertion_request *request) {
    const sxml_schema_type_definition *definition =
        type_of(run, request->type_definition.value);
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = element_value(request, definition, &value);
    assertion_copy copy = {.arena_tree = NULL,
                           .element_node = {.representation = SXML_ITEM_REPRESENTATION_NONE}};
    if (status == SXML_STATUS_OK) {
        status = copy_for_assertions(run->validator, request->node, &copy);
    }
    const sxml_schema_index_view assertions =
        sxml_schema_get_list(&run->schema->components, definition->assertions);
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < assertions.index_count; entry += 1) {
        const sxml_schema_assertion *assertion = sxml_schema_get_assertion(
            &run->schema->components,
            (sxml_schema_assertion_index){.value = assertions.indexes[entry]});
        test_outcome outcome = {.is_true = false};
        status = evaluate_test(&(test_evaluation){.schema = run->schema,
                                                  .test = assertion->test,
                                                  .context_node = copy.element_node,
                                                  .binds_value = true,
                                                  .value = value},
                               &outcome);
        if (status == SXML_STATUS_OK && !outcome.is_true) {
            status = sxml_schema_validator_report_error(
                run->validator,
                &(sxml_schema_validation_error){
                    .constraint_name = TEXT("cvc-assertion"),
                    .message = {.text = TEXT("%e breaks the assertion %v of %t"),
                                .element = element_of(request->node),
                                .value = sxml_schema_get_text(
                                    &run->schema->components,
                                    sxml_schema_get_xpath_record(&run->schema->components,
                                                                 assertion->test)
                                        ->expression),
                                .type_definition = request->type_definition},
                    .location = location_of(request->node)});
        }
    }
    sxml_sequence_release(value);
    release_copy(&copy);
    return status;
}

sxml_status sxml_schema_check_assertions(sxml_schema_validator *validator,
                                         const sxml_schema_assertion_request *request) {
    const assertion_run run = {.validator = validator, .schema = request->schema};
    const sxml_schema_type_definition *definition = type_of(&run, request->type_definition.value);
    if (definition->variety == SXML_TYPE_VARIETY_COMPLEX) {
        return definition->assertions.entry_count == 0 ? SXML_STATUS_OK
                                                       : check_element(&run, request);
    }
    return check_simple_value(&run, request);
}
