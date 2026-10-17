/*
 * XQuery 4.0 through the public API: compiled queries, their evaluation
 * and the serialization parameters their prologs declare
 * (include/sxml/xquery.h; docs/DECISIONS.md, D106, D135, D146).
 *
 * Golden file: src/xpath.c (layout, lifecycle, comments), whose context
 * checks and unit evaluation this file shares (src/xpath_api_internal.h):
 * a query is compiled against the same static context as an XPath
 * expression and evaluated by the same code. Compilation is
 * sxml_xquery_compile_module (src/xquery_lower_internal.h) over an AST
 * this file keeps until the output declarations are read.
 *
 * Output declarations (XQuery 4.0, section 5.22): every option
 * declaration whose name is in the serialization namespace (XQST0109 and
 * XQST0110 are checked by the prolog, src/xquery_static.c) is handed, as
 * text, to the query's serialization parameter reader
 * (sxml_serialization_parameter_reader_read_texts, src/serialize_internal.h)
 * over the parameter document of output:parameter-document, if any, so
 * the declarations take precedence over the document and the document
 * over XQuery's defaults (method xml, 5.22.1). The document is asked of
 * the processor's resolver at compile time (kind XML_DOCUMENT; XQST0119
 * when it is not available, XQST0115 when it is not a valid parameter
 * document); a value a parameter does not permit is SEPM0016, reported as
 * a static error at its declaration.
 *
 * Section numbers are of XQuery 4.0 unless another specification is
 * named.
 */
#include "sxml/xquery.h"

#include "processor_internal.h"
#include "serialize_internal.h"
#include "xpath_api_internal.h"
#include "xpath_ast_internal.h"
#include "xpath_static_internal.h"
#include "xquery_lower_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

struct sxml_xquery_query {
    sxml_processor *processor;
    /* The allocator of sxml_xquery_query_create, which destroy frees the
       query with; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    /* Sealed; one reference. */
    sxml_ir_unit *unit;
    /* Owns what serialization_parameters views; NULL when the main module
       declares no serialization parameter. */
    sxml_serialization_parameter_reader *parameter_reader;
    sxml_serialization_parameters serialization_parameters;
};

/* ----------------------------------------------------------- AST access */

static const sxml_ast_node *node_of(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_get_node(ast, node);
}

static sxml_ast_node_index child_of(const sxml_ast *ast, sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

/* The module of the AST: a MAIN_MODULE or a LIBRARY_MODULE. */
static sxml_ast_node_index module_of(const sxml_ast *ast) {
    return child_of(ast, ast->root, SXML_AST_FIELD_MODULE);
}

/* ------------------------------------------------- Output declarations */

/* The output declarations of a main module, found in its prolog. */
typedef struct sxml_output_declarations {
    /* Allocated from the processor's allocator; NULL when count is 0. */
    sxml_serialization_parameter_text *parameter_texts;
    /* The OPTION_DECLARATION of each text. */
    sxml_ast_node_index *declarations;
    size_t parameter_text_count;
    /* The declaration of output:parameter-document; 0 for none. */
    sxml_ast_node_index parameter_document;
} sxml_output_declarations;

/* What reading the output declarations works with. */
typedef struct sxml_output_reading {
    sxml_processor *processor;
    const sxml_ast *ast;
    const sxml_ir_unit *unit;
    sxml_error_record *error_record;
} sxml_output_reading;

static void sxml_output_declarations_release(const sxml_output_reading *reading,
                                             sxml_output_declarations *declarations) {
    sxml_allocator_release(reading->processor->allocator, declarations->parameter_texts);
    sxml_allocator_release(reading->processor->allocator, declarations->declarations);
    declarations->parameter_texts = NULL;
    declarations->declarations = NULL;
}

/* The static context the option names resolve with: the module's
   statically known namespaces, which the unit holds (D135). */
static sxml_xpath_static_context sxml_output_static_context(const sxml_output_reading *reading) {
    return (sxml_xpath_static_context){
        .item_heap = reading->processor->item_heap,
        .allocator = reading->processor->allocator,
        .namespace_bindings = sxml_ir_unit_get_static_namespaces(reading->unit),
        .variable_declarations = {.variable_declarations = NULL,
                                  .variable_declaration_count = 0},
        .function_table = {.state = NULL, .operations = NULL},
        .static_base_uri = {.bytes = NULL, .byte_count = 0}};
}

/*
 * Finds the option declarations in the serialization namespace (5.21,
 * 5.22): the first pass counts them, the second fills the arrays.
 * Unprefixed option names are in the XQuery namespace and are not
 * output declarations.
 */
static sxml_status sxml_output_declarations_find(const sxml_output_reading *reading,
                                                 sxml_output_declarations *declarations) {
    const sxml_ast *ast = reading->ast;
    const sxml_xpath_static_context static_context = sxml_output_static_context(reading);
    const sxml_ast_node_index prolog = child_of(ast, module_of(ast), SXML_AST_FIELD_PROLOG);
    sxml_status status = SXML_STATUS_OK;
    size_t text_count = 0;
    for (int pass_number = 0; status == SXML_STATUS_OK && pass_number < 2; pass_number += 1) {
        if (pass_number == 1 && text_count > 0) {
            void *texts = NULL;
            void *nodes = NULL;
            size_t text_byte_count = 0;
            size_t node_byte_count = 0;
            status = sxml_size_multiply(text_count, sizeof(sxml_serialization_parameter_text),
                                        &text_byte_count);
            if (status == SXML_STATUS_OK) {
                status = sxml_size_multiply(text_count, sizeof(sxml_ast_node_index),
                                            &node_byte_count);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_allocator_allocate(
                    reading->processor->allocator,
                    (sxml_allocation_request){
                        .byte_count = text_byte_count,
                        .alignment = alignof(sxml_serialization_parameter_text)},
                    &texts);
            }
            declarations->parameter_texts = texts;
            if (status == SXML_STATUS_OK) {
                status = sxml_allocator_allocate(
                    reading->processor->allocator,
                    (sxml_allocation_request){.byte_count = node_byte_count,
                                              .alignment = alignof(sxml_ast_node_index)},
                    &nodes);
            }
            declarations->declarations = nodes;
        }
        size_t filled = 0;
        for (sxml_ast_node_index declaration = child_of(ast, prolog, SXML_AST_FIELD_DECLARATION);
             status == SXML_STATUS_OK && prolog.value != 0 && declaration.value != 0;
             declaration = sxml_ast_find_next_with_same_field(ast, declaration)) {
            if (node_of(ast, declaration)->kind != SXML_AST_KIND_OPTION_DECLARATION) {
                continue;
            }
            const sxml_ast_node_index name_node = child_of(ast, declaration, SXML_AST_FIELD_NAME);
            if (node_of(ast, name_node)->kind == SXML_AST_KIND_NCNAME) {
                continue;
            }
            sxml_xpath_resolved_name name = {.namespace_uri = {.index = 0},
                                             .local_name = {.bytes = NULL, .byte_count = 0}};
            status = sxml_xpath_resolve_name(
                &(sxml_xpath_name_request){.ast = ast,
                                           .name_node = name_node,
                                           .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                                           .static_context = &static_context,
                                           .error_record = reading->error_record},
                &name);
            if (status != SXML_STATUS_OK ||
                name.namespace_uri.index != SXML_NAMESPACE_SERIALIZATION) {
                continue;
            }
            if (sxml_byte_view_equals(name.local_name,
                                      SXML_BYTE_VIEW_FROM_LITERAL("parameter-document"))) {
                declarations->parameter_document = declaration;
                continue;
            }
            if (pass_number == 1 && declarations->parameter_texts != NULL &&
                declarations->declarations != NULL && filled < text_count) {
                const sxml_ast_node_index value = child_of(ast, declaration, SXML_AST_FIELD_VALUE);
                declarations->parameter_texts[filled] = (sxml_serialization_parameter_text){
                    .parameter_name = name.local_name,
                    .parameter_value =
                        sxml_ast_get_text(ast, node_of(ast, value)->payload.text)};
                declarations->declarations[filled] = declaration;
            }
            filled += 1;
        }
        text_count = filled;
    }
    declarations->parameter_text_count = text_count;
    return status;
}

/* A loaded parameter document and its document element. */
typedef struct sxml_parameter_document {
    sxml_processor_document_loader loader_memory;
    sxml_processor_document_loader *loader;
    /* Owned; representation 0 when nothing was loaded. */
    sxml_item document_node;
    sxml_node_reference document_element;
    bool has_document_element;
} sxml_parameter_document;

static void sxml_parameter_document_release(sxml_parameter_document *document) {
    if (document->document_node.representation != 0) {
        sxml_item_release(document->document_node);
    }
    sxml_processor_document_loader_finalize(document->loader);
}

/* The document element of a document node; false for none. */
static bool sxml_document_element_of(sxml_node_reference document,
                                     sxml_node_reference *element) {
    sxml_node_description description;
    if (sxml_node_describe(document, &description) != SXML_STATUS_OK) {
        return false;
    }
    bool has_child = description.has_children;
    sxml_node_index child = description.first_child;
    while (has_child) {
        const sxml_node_reference child_node = {
            .tree = document.tree, .node_index = child, .namespace_number = 0};
        sxml_node_description child_description;
        if (sxml_node_describe(child_node, &child_description) != SXML_STATUS_OK) {
            return false;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            *element = child_node;
            return true;
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    return false;
}

/*
 * Loads the parameter document of output:parameter-document (5.22): its
 * value is a URI reference resolved against the static base URI and asked
 * of the processor's resolver as an XML document. XQST0119 when it cannot
 * be read or is not well-formed.
 */
static sxml_status sxml_parameter_document_load(const sxml_output_reading *reading,
                                                sxml_ast_node_index declaration,
                                                sxml_parameter_document *document) {
    sxml_processor *processor = reading->processor;
    sxml_status status = sxml_processor_document_loader_initialize(
        (sxml_memory_view){.bytes = (uint8_t *)&document->loader_memory,
                           .byte_count = sizeof(document->loader_memory)},
        &(sxml_processor_document_loader_configuration){
            .evaluation_environment = {.processor = processor,
                                       .resource_resolver = processor->resource_resolver,
                                       .trace_sink = processor->trace_sink,
                                       .default_language = SXML_BYTE_VIEW_FROM_LITERAL("en"),
                                       .environment_variables =
                                           {.environment_variables = NULL,
                                            .environment_variable_count = 0}},
            .available_documents = {.available_documents = NULL,
                                    .available_document_count = 0}},
        &document->loader);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ast *ast = reading->ast;
    const sxml_ast_node_index value = child_of(ast, declaration, SXML_AST_FIELD_VALUE);
    const sxml_document_loader loader =
        sxml_processor_document_loader_as_document_loader(document->loader);
    sxml_error_record load_error;
    memset(&load_error, 0, sizeof(load_error));
    status = loader.operations->load_document(
        loader.state,
        &(sxml_document_request){
            .uri_reference = sxml_ast_get_text(ast, node_of(ast, value)->payload.text),
            .base_uri = reading->unit->has_static_base_uri
                            ? reading->unit->static_base_uri
                            : (sxml_byte_view){.bytes = NULL, .byte_count = 0},
            .error_record = &load_error},
        &document->document_node);
    sxml_node_reference document_node;
    if (status == SXML_STATUS_OK &&
        sxml_item_get_node(document->document_node, &document_node) == SXML_STATUS_OK) {
        document->has_document_element =
            sxml_document_element_of(document_node, &document->document_element);
    }
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        return sxml_xpath_report_static_error(
            &(sxml_xpath_static_error){
                .ast = ast,
                .node = declaration,
                .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XQST0119"),
                .description = SXML_BYTE_VIEW_FROM_LITERAL(
                    "the parameter document of output:parameter-document cannot be read")},
            reading->error_record);
    }
    return status;
}

/*
 * Reads the output declarations over the parameter document into the
 * query's reader. A failure of the document is XQST0115; a failure of a
 * declaration keeps its code (SEPM0016, SESU0007, SESU0011) as a static
 * error at the declaration.
 */
typedef struct sxml_output_result {
    sxml_serialization_parameter_reader **parameter_reader;
    sxml_serialization_parameters *parameters;
} sxml_output_result;

static sxml_status sxml_output_declarations_apply(const sxml_output_reading *reading,
                                                  const sxml_output_declarations *declarations,
                                                  const sxml_output_result *result) {
    sxml_processor *processor = reading->processor;
    sxml_parameter_document document;
    memset(&document, 0, sizeof(document));
    sxml_status status = SXML_STATUS_OK;
    if (declarations->parameter_document.value != 0) {
        status =
            sxml_parameter_document_load(reading, declarations->parameter_document, &document);
    }
    sxml_serialization_parameter_reader *parameter_reader = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_serialization_parameter_reader_create(
            processor->allocator,
            &(sxml_serialization_parameter_reader_configuration){
                .allocator = processor->allocator, .item_heap = processor->item_heap},
            &parameter_reader);
    }
    sxml_error_record read_error;
    memset(&read_error, 0, sizeof(read_error));
    size_t failed_text_index = SIZE_MAX;
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    const bool has_document = declarations->parameter_document.value != 0;
    if (status == SXML_STATUS_OK) {
        /* A document without an element is no parameter document. */
        status = sxml_serialization_parameter_reader_read_texts(
            parameter_reader,
            &(sxml_serialization_parameter_text_request){
                .has_parameter_document = has_document && document.has_document_element,
                .parameter_document = document.document_element,
                .parameter_texts = {.parameter_texts = declarations->parameter_texts,
                                    .parameter_text_count = declarations->parameter_text_count},
                .namespace_bindings = sxml_ir_unit_get_static_namespaces(reading->unit),
                .error_record = &read_error,
                .failed_text_index = &failed_text_index},
            &parameters);
        if (status == SXML_STATUS_OK && has_document && !document.has_document_element) {
            status = SXML_STATUS_DYNAMIC_ERROR;
        }
    }
    sxml_parameter_document_release(&document);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        const bool is_document_failure = failed_text_index == SIZE_MAX ||
                                         declarations->declarations == NULL ||
                                         failed_text_index >= declarations->parameter_text_count;
        status = sxml_xpath_report_static_error(
            &(sxml_xpath_static_error){
                .ast = reading->ast,
                .node = is_document_failure ? declarations->parameter_document
                                            : declarations->declarations[failed_text_index],
                .code_local_name = is_document_failure
                                       ? SXML_BYTE_VIEW_FROM_LITERAL("XQST0115")
                                       : read_error.code.local_name,
                .description = is_document_failure
                                   ? SXML_BYTE_VIEW_FROM_LITERAL(
                                         "the parameter document is not a valid serialization "
                                         "parameter document")
                                   : read_error.diagnostic.description},
            reading->error_record);
    }
    if (status != SXML_STATUS_OK) {
        sxml_serialization_parameter_reader_destroy(parameter_reader);
        return status;
    }
    *result->parameter_reader = parameter_reader;
    *result->parameters = parameters;
    return SXML_STATUS_OK;
}

/* Reads the output declarations of the compiled module: none leaves the
   reader NULL and XQuery's defaults. */
static sxml_status sxml_output_declarations_read(const sxml_output_reading *reading,
                                                 const sxml_output_result *result) {
    sxml_output_declarations declarations = {.parameter_texts = NULL,
                                             .declarations = NULL,
                                             .parameter_text_count = 0,
                                             .parameter_document = {.value = 0}};
    sxml_status status = sxml_output_declarations_find(reading, &declarations);
    if (status == SXML_STATUS_OK &&
        (declarations.parameter_text_count > 0 || declarations.parameter_document.value != 0)) {
        status = sxml_output_declarations_apply(reading, &declarations, result);
    }
    sxml_output_declarations_release(reading, &declarations);
    return status;
}

/* ----------------------------------------------------------- Compiling */

/* What compilation makes. */
typedef struct sxml_compiled_query {
    sxml_ir_unit *unit;
    sxml_serialization_parameter_reader *parameter_reader;
    sxml_serialization_parameters parameters;
} sxml_compiled_query;

/* Parses, checks the module is a main module (a library module is
   XPST0003 here, include/sxml/xquery.h), compiles it and reads its
   output declarations. */
static sxml_status sxml_xquery_compile_query(
    const sxml_xquery_query_configuration *configuration, sxml_compiled_query *compiled) {
    sxml_processor *processor = configuration->processor;
    const sxml_xpath_static_context static_context =
        sxml_processor_static_context(processor, &configuration->static_context);
    const uint32_t maximum_expression_depth =
        sxml_processor_get_maximum_expression_depth(processor);
    sxml_ast *ast = NULL;
    sxml_status status = sxml_xquery_build_ast(
        &(sxml_ast_build_request){.expression_text = configuration->expression_text,
                                  .source_name = configuration->source_name,
                                  .allocator = processor->allocator,
                                  .maximum_expression_depth = maximum_expression_depth,
                                  .error_record = configuration->error_record},
        &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (node_of(ast, module_of(ast))->kind != SXML_AST_KIND_MAIN_MODULE) {
        status = sxml_xpath_report_static_error(
            &(sxml_xpath_static_error){
                .ast = ast,
                .node = module_of(ast),
                .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XPST0003"),
                .description = SXML_BYTE_VIEW_FROM_LITERAL(
                    "a library module is not a query; only a main module is evaluated")},
            configuration->error_record);
    }
    if (status == SXML_STATUS_OK) {
        /* Library modules come from the processor's resolver (D106,
           D148). */
        status = sxml_xquery_compile_module(
            &(sxml_xquery_lowering_request){.ast = ast,
                                            .static_context = &static_context,
                                            .maximum_expression_count = 0,
                                            .maximum_expression_depth = maximum_expression_depth,
                                            .error_record = configuration->error_record,
                                            .resource_resolver = processor->resource_resolver,
                                            .processor = processor},
            &compiled->unit);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_output_declarations_read(
            &(sxml_output_reading){.processor = processor,
                                   .ast = ast,
                                   .unit = compiled->unit,
                                   .error_record = configuration->error_record},
            &(sxml_output_result){.parameter_reader = &compiled->parameter_reader,
                                  .parameters = &compiled->parameters});
        if (status != SXML_STATUS_OK) {
            sxml_ir_unit_release(compiled->unit);
            compiled->unit = NULL;
        }
    }
    sxml_ast_destroy(ast);
    return status;
}

/* --------------------------------------------------------------- Query */

sxml_status sxml_xquery_query_memory_requirements(
    const sxml_xquery_query_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->processor == NULL || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_xquery_query),
                                               .alignment = alignof(sxml_xquery_query)};
    return SXML_STATUS_OK;
}

sxml_status sxml_xquery_query_initialize(sxml_memory_view memory,
                                         const sxml_xquery_query_configuration *configuration,
                                         sxml_xquery_query **query) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_xquery_query_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK && (query == NULL || !sxml_static_context_configuration_is_valid(
                                                          configuration->processor,
                                                          &configuration->static_context))) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_compiled_query compiled = {.unit = NULL, .parameter_reader = NULL};
    memset(&compiled.parameters, 0, sizeof(compiled.parameters));
    /* 5.22.1: the default output method of XQuery is xml. */
    compiled.parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
    status = sxml_xquery_compile_query(configuration, &compiled);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_xquery_query *new_query = placement;
    *new_query = (sxml_xquery_query){.processor = configuration->processor,
                                     .creating_allocator = {.state = NULL, .operations = NULL},
                                     .unit = compiled.unit,
                                     .parameter_reader = compiled.parameter_reader,
                                     .serialization_parameters = compiled.parameters};
    *query = new_query;
    return SXML_STATUS_OK;
}

void sxml_xquery_query_finalize(sxml_xquery_query *query) {
    if (query == NULL) {
        return;
    }
    sxml_ir_unit_release(query->unit);
    sxml_serialization_parameter_reader_destroy(query->parameter_reader);
    query->unit = NULL;
    query->parameter_reader = NULL;
}

sxml_status sxml_xquery_query_create(sxml_allocator allocator,
                                     const sxml_xquery_query_configuration *configuration,
                                     sxml_xquery_query **query) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_xquery_query_memory_requirements(configuration, &requirements);
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
    sxml_xquery_query *new_query = NULL;
    status = sxml_xquery_query_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration,
        &new_query);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_query->creating_allocator = allocator;
    *query = new_query;
    return SXML_STATUS_OK;
}

void sxml_xquery_query_destroy(sxml_xquery_query *query) {
    if (query == NULL) {
        return;
    }
    const sxml_allocator allocator = query->creating_allocator;
    sxml_xquery_query_finalize(query);
    sxml_allocator_release(allocator, query);
}

/* ------------------------------------------------------------ Evaluation */

sxml_status sxml_xquery_query_evaluate(const sxml_xquery_query *query,
                                       const sxml_xquery_evaluation_request *request,
                                       sxml_sequence *result) {
    if (query == NULL || query->unit == NULL || request == NULL || result == NULL ||
        !sxml_dynamic_context_configuration_is_valid(&request->dynamic_context)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* The unit holds the static base URI: the host's, or a base URI
       declaration's resolved against it (5.5). */
    return sxml_evaluate_unit(
        &(sxml_unit_evaluation){.processor = query->processor,
                                .unit = query->unit,
                                .static_base_uri = {.bytes = NULL, .byte_count = 0},
                                .dynamic_context = &request->dynamic_context,
                                .error_record = request->error_record},
        result);
}

/* --------------------------------------------- Serialization parameters */

sxml_serialization_parameters
sxml_xquery_query_get_serialization_parameters(const sxml_xquery_query *query) {
    if (query == NULL) {
        sxml_serialization_parameters defaults;
        memset(&defaults, 0, sizeof(defaults));
        defaults.serialization_method = SXML_SERIALIZATION_METHOD_XML;
        return defaults;
    }
    return query->serialization_parameters;
}
